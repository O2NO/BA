"""
LuxMeterCalc HTTP server.

GET  /health         -> {"ok": true, "version": "..."}
POST /measure        -> illuminance computation, see /docs
POST /measure_batch  -> illuminance for many meter poses against one fixture set

Run with:
    python -m uvicorn server:app --host 127.0.0.1 --port 8765 --reload

The server is intended to listen on localhost only. The UE plugin uses the
endpoint configured in Project Settings -> Plugins -> LuxMeterCalc.
"""

from __future__ import annotations

import logging
from functools import lru_cache
from pathlib import Path

from fastapi import FastAPI
from fastapi.responses import JSONResponse

from ies_parser import IESPhotometry, parse_ies
from models import (
    LightDebugDTO,
    MeasureBatchRequest,
    MeasureBatchResponse,
    MeasureRequest,
    MeasureResponse,
    ReflectorPayload,
)
from photometry import (
    build_reflector_patches,
    compute_light_contribution,
    reflector_lux_at_meter,
)

VERSION = "0.1.0"
CM_TO_M = 0.01

log = logging.getLogger("luxmeter")
logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")

app = FastAPI(title="LuxMeterCalc", version=VERSION)


@lru_cache(maxsize=64)
def _load_ies_cached(path_str: str) -> IESPhotometry:
    return parse_ies(path_str)


def _build_fixture_tuples(
    lights, ies_by_path: dict[str, "IESPhotometry | None"]
):
    """Convert a list of LightPayloads into the (pos, fwd, up, lumens, ies) tuples
    expected by build_reflector_patches. Skips lights whose IES failed to load."""
    fixtures = []
    for light in lights:
        ies = ies_by_path.get(light.ies_file_path)
        if ies is None:
            continue
        fixtures.append(
            (
                (light.location.x * CM_TO_M, light.location.y * CM_TO_M, light.location.z * CM_TO_M),
                (light.forward.x, light.forward.y, light.forward.z),
                (light.up.x, light.up.y, light.up.z),
                light.intensity_lumens,
                ies,
            )
        )
    return fixtures


def _build_patches_for_reflectors(reflectors: list[ReflectorPayload], fixtures):
    """Flatten all reflectors into a single patch list. v1 typically has 0 or 1
    reflectors, but the math is identical for multiple — they just concatenate."""
    if not reflectors or not fixtures:
        return []
    all_patches = []
    for refl in reflectors:
        if refl.albedo <= 0.0 or refl.width_cm <= 0.0 or refl.height_cm <= 0.0:
            continue
        patches = build_reflector_patches(
            reflector_pos_m=(
                refl.location.x * CM_TO_M,
                refl.location.y * CM_TO_M,
                refl.location.z * CM_TO_M,
            ),
            reflector_forward=(refl.forward.x, refl.forward.y, refl.forward.z),
            reflector_up=(refl.up.x, refl.up.y, refl.up.z),
            width_m=refl.width_cm * CM_TO_M,
            height_m=refl.height_cm * CM_TO_M,
            albedo=refl.albedo,
            fixtures=fixtures,
        )
        all_patches.extend(patches)
    return all_patches


@app.get("/health")
def health() -> dict:
    return {"ok": True, "version": VERSION}


@app.post("/measure", response_model=MeasureResponse, response_model_by_alias=True)
def measure(req: MeasureRequest) -> MeasureResponse:
    if not req.lights:
        return MeasureResponse(error="No lights in request.")

    meter_pos = (
        req.meter.location.x * CM_TO_M,
        req.meter.location.y * CM_TO_M,
        req.meter.location.z * CM_TO_M,
    )
    meter_fwd = (req.meter.forward.x, req.meter.forward.y, req.meter.forward.z)

    debug: list[LightDebugDTO] = []
    per_light: list[float] = []
    total = 0.0
    errors: list[str] = []

    for light in req.lights:
        name = light.display_name or Path(light.ies_file_path).stem or "(unnamed)"

        if not light.ies_file_path:
            errors.append(f"{name}: no IES file path")
            debug.append(LightDebugDTO(display_name=name))
            per_light.append(0.0)
            continue

        try:
            ies = _load_ies_cached(light.ies_file_path)
        except FileNotFoundError:
            errors.append(f"{name}: IES file not found ({light.ies_file_path})")
            debug.append(LightDebugDTO(display_name=name))
            per_light.append(0.0)
            continue
        except Exception as exc:
            errors.append(f"{name}: IES parse error: {exc}")
            debug.append(LightDebugDTO(display_name=name))
            per_light.append(0.0)
            continue

        light_pos = (
            light.location.x * CM_TO_M,
            light.location.y * CM_TO_M,
            light.location.z * CM_TO_M,
        )
        light_fwd = (light.forward.x, light.forward.y, light.forward.z)
        light_up = (light.up.x, light.up.y, light.up.z)

        contribution = compute_light_contribution(
            display_name=name,
            light_pos_m=light_pos,
            light_forward=light_fwd,
            light_up=light_up,
            meter_pos_m=meter_pos,
            meter_forward=meter_fwd,
            intensity_lumens=light.intensity_lumens,
            ies=ies,
        )

        debug.append(
            LightDebugDTO(
                display_name=contribution.display_name,
                distance_meters=contribution.distance_m,
                gamma_deg=contribution.gamma_deg,
                c_deg=contribution.c_deg,
                candela=contribution.candela,
                cosine_to_meter=contribution.cosine_to_meter,
                contribution_lux=contribution.contribution_lux,
            )
        )
        per_light.append(contribution.contribution_lux)
        total += contribution.contribution_lux

    # Reflector contribution (Lambertian single-bounce). Builds patch list each
    # call because /measure is single-meter; per-meter cost is small.
    if req.reflectors:
        # Re-resolve IES per fixture (already cached in lru_cache).
        ies_by_path_local: dict[str, IESPhotometry | None] = {}
        for light in req.lights:
            if not light.ies_file_path or light.ies_file_path in ies_by_path_local:
                continue
            try:
                ies_by_path_local[light.ies_file_path] = _load_ies_cached(light.ies_file_path)
            except Exception:
                ies_by_path_local[light.ies_file_path] = None
        fixtures = _build_fixture_tuples(req.lights, ies_by_path_local)
        patches = _build_patches_for_reflectors(req.reflectors, fixtures)
        reflected = reflector_lux_at_meter(patches, meter_pos, meter_fwd)
        if reflected > 0.0 or patches:
            debug.append(
                LightDebugDTO(
                    display_name="(reflector)",
                    contribution_lux=reflected,
                )
            )
            per_light.append(reflected)
            total += reflected

    log.info("measure: lights=%d reflectors=%d total=%.3f lx",
             len(req.lights), len(req.reflectors), total)

    return MeasureResponse(
        illuminance_lux=total,
        per_light_lux=per_light,
        debug=debug,
        error="; ".join(errors),
    )


@app.post(
    "/measure_batch",
    response_model=MeasureBatchResponse,
    response_model_by_alias=True,
)
def measure_batch(req: MeasureBatchRequest) -> MeasureBatchResponse:
    """Compute illuminance for every meter pose against one shared set of fixtures.

    Lights are parsed and cached once per request via _load_ies_cached, so a
    1000-pose batch is roughly N_meters * N_lights cheap photometric ops.
    """
    if not req.lights:
        return MeasureBatchResponse(error="No lights in request.")
    if not req.meters:
        return MeasureBatchResponse(error="No meters in request.")

    # Resolve and cache the IES files once for this request.
    ies_by_path: dict[str, IESPhotometry | None] = {}
    errors: list[str] = []
    for light in req.lights:
        path = light.ies_file_path
        if not path:
            errors.append(f"{light.display_name or '(unnamed)'}: no IES file path")
            ies_by_path[path] = None
            continue
        if path in ies_by_path:
            continue
        try:
            ies_by_path[path] = _load_ies_cached(path)
        except FileNotFoundError:
            errors.append(f"{light.display_name or path}: IES file not found ({path})")
            ies_by_path[path] = None
        except Exception as exc:
            errors.append(f"{light.display_name or path}: IES parse error: {exc}")
            ies_by_path[path] = None

    # Pre-build reflector patches once (independent of meter pose).
    fixtures = _build_fixture_tuples(req.lights, ies_by_path)
    patches = _build_patches_for_reflectors(req.reflectors, fixtures)

    out: list[float] = []
    for meter in req.meters:
        meter_pos = (
            meter.location.x * CM_TO_M,
            meter.location.y * CM_TO_M,
            meter.location.z * CM_TO_M,
        )
        meter_fwd = (meter.forward.x, meter.forward.y, meter.forward.z)

        total = 0.0
        for light in req.lights:
            ies = ies_by_path.get(light.ies_file_path)
            if ies is None:
                continue
            light_pos = (
                light.location.x * CM_TO_M,
                light.location.y * CM_TO_M,
                light.location.z * CM_TO_M,
            )
            contribution = compute_light_contribution(
                display_name=light.display_name or "",
                light_pos_m=light_pos,
                light_forward=(light.forward.x, light.forward.y, light.forward.z),
                light_up=(light.up.x, light.up.y, light.up.z),
                meter_pos_m=meter_pos,
                meter_forward=meter_fwd,
                intensity_lumens=light.intensity_lumens,
                ies=ies,
            )
            total += contribution.contribution_lux
        # Reflector single-bounce contribution, if any.
        if patches:
            total += reflector_lux_at_meter(patches, meter_pos, meter_fwd)
        out.append(total)

    log.info(
        "measure_batch: lights=%d meters=%d reflectors=%d patches=%d (errors=%d)",
        len(req.lights),
        len(req.meters),
        len(req.reflectors),
        len(patches),
        len(errors),
    )

    return MeasureBatchResponse(
        illuminance_lux=out,
        error="; ".join(errors),
    )


@app.exception_handler(Exception)
async def _unhandled(_request, exc: Exception):
    log.exception("unhandled error")
    return JSONResponse(
        status_code=500,
        content={"illuminanceLux": 0.0, "perLightLux": [], "debug": [], "error": f"server error: {exc}"},
    )
