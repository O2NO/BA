# LuxMeterCalc — Calculator

Local Python service that performs the photometric math for the LuxMeterCalc UE plugin.

## Install

```
cd Plugins/LuxMeterCalc/Calculator
pip install -r requirements.txt
```

Python 3.10+ is required (uses `|` union types and dataclasses).

## Run

```
run.bat
```

…which is just `python -m uvicorn server:app --host 127.0.0.1 --port 8765 --reload`.

The UE plugin's *Ping* button hits `GET /health`; *Measure* hits `POST /measure`.

Open `http://127.0.0.1:8765/docs` to exercise the API by hand without UE.

## Test

```
pytest
```

Covers the IES parser and the photometric math against a synthetic isotropic fixture in `tests/fixtures/isotropic_1000cd.ies`, where the expected illuminance is `1000 · cos θ / d²` lx and is asserted to within float precision.

## How it works

For each light in the request, the server:

1. Loads (and caches) the referenced `.ies` file via the hand-written LM-63 parser in `ies_parser.py`.
2. Computes the direction from the light to the meter, transforms it into the light's local IES frame using the supplied `Forward` (= IES nadir) and `Up` (= azimuth reference) vectors, and resolves the (γ, C) sampling angles.
3. Bilinearly interpolates the candela grid at those angles, with symmetry handling for rotationally / quadrant / bilateral symmetric IES files.
4. Optionally scales by `IntensityLumens / rated_lumens` from the IES header (skipped if `IntensityLumens=0`, in which case candela is used as-is).
5. Applies Lambert's cosine law at the meter (`max(0, dot(meter_normal, dir_meter→light))`) and divides by distance² (in metres) to get the per-light contribution in lux.

Total illuminance is the sum across lights. The response also carries a `Debug[]` array with `gamma_deg`, `distance_m`, `candela`, `cosine_to_meter`, and `contribution_lux` per light, which the UE panel renders in the per-light breakdown for diagnostics.

## Conventions

- UE coordinates: left-handed, +Z up, units **cm** in the JSON payload (server converts to metres internally).
- Light's `Forward` (= actor +X in UE) is the IES nadir (γ=0).
- Light's `Up` is the C-axis reference; only matters for non-rotationally-symmetric IES files.
- Meter's `Forward` is the surface normal of the sensor; lights hitting the back of the meter contribute 0 lux.
- `IntensityLumens` on the JSON payload:
    - The current UE plugin always sends `0` here, so the calculator uses the IES file's candela values as-is.
    - The lumens-scaling code path (`intensity_lumens > 0` → scale candela by `intensity_lumens / rated_lumens`) is preserved in `photometry.py` for anyone who later wants to drive a fixture with a different lamp than the original IES test report — just have a client send a non-zero value and the math works out.
    - The actor's `ViewportIntensityLumens` knob in UE drives only the spotlight's rendered brightness; it has no effect on calculator output.

## Limitations

- Type C photometry only (no Type A/B). Raises a clear error if you point it at a Type A/B file.
- `TILT=NONE` only.
- No occlusion / shadow / interreflection. Each light is treated as a point-source IES distribution with line-of-sight to the meter.
- Single sensor per request (one meter).
