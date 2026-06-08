"""End-to-end tests for the FastAPI surface (TestClient, no live server)."""

from pathlib import Path

import pytest

# httpx is a starlette TestClient runtime dep; skip if missing rather than fail.
pytest.importorskip("httpx")

from fastapi.testclient import TestClient

from server import app

FIXTURE_PATH = (
    Path(__file__).parent / "fixtures" / "isotropic_1000cd.ies"
).resolve()


def _ue_payload_light(name: str = "TestLight"):
    return {
        "location":   {"x": 0.0, "y": 0.0, "z": 200.0},   # 2 m above origin (cm)
        "forward":    {"x": 0.0, "y": 0.0, "z": -1.0},
        "up":         {"x": 1.0, "y": 0.0, "z":  0.0},
        "iESFilePath": str(FIXTURE_PATH),
        "intensityLumens": 0.0,
        "displayName": name,
    }


def test_health():
    r = TestClient(app).get("/health")
    assert r.status_code == 200
    body = r.json()
    assert body["ok"] is True
    assert "version" in body


def test_measure_directly_below():
    payload = {
        "lights": [_ue_payload_light()],
        "meter": {
            "location": {"x": 0.0, "y": 0.0, "z": 0.0},
            "forward":  {"x": 0.0, "y": 0.0, "z": 1.0},
        },
        "units": "cm",
    }
    r = TestClient(app).post("/measure", json=payload)
    assert r.status_code == 200
    assert r.json()["illuminanceLux"] == pytest.approx(250.0)


def test_measure_batch_three_poses():
    """Three meters: directly below (250 lx), off-axis (~88 lx), back-facing (0 lx)."""
    payload = {
        "lights": [_ue_payload_light()],
        "meters": [
            {"location": {"x": 0.0, "y": 0.0, "z": 0.0}, "forward": {"x": 0.0, "y": 0.0, "z": 1.0}},
            {"location": {"x": 200.0, "y": 0.0, "z": 0.0}, "forward": {"x": 0.0, "y": 0.0, "z": 1.0}},
            {"location": {"x": 0.0, "y": 0.0, "z": 0.0}, "forward": {"x": 0.0, "y": 0.0, "z": -1.0}},
        ],
        "units": "cm",
    }
    r = TestClient(app).post("/measure_batch", json=payload)
    assert r.status_code == 200
    body = r.json()
    lux = body["illuminanceLux"]
    assert len(lux) == 3
    assert lux[0] == pytest.approx(250.0)
    assert lux[1] == pytest.approx(88.388, abs=0.5)
    assert lux[2] == pytest.approx(0.0)


def test_measure_batch_matches_per_pose_measure():
    """Verify /measure_batch and /measure give the same total per pose."""
    poses = [
        ({"x": 0.0, "y": 0.0, "z": 0.0},   {"x": 0.0, "y": 0.0, "z": 1.0}),
        ({"x": 100.0, "y": 0.0, "z": 0.0}, {"x": 0.0, "y": 0.0, "z": 1.0}),
        ({"x": 0.0, "y": 100.0, "z": 0.0}, {"x": 0.0, "y": 0.0, "z": 1.0}),
    ]
    client = TestClient(app)

    per_call = []
    for loc, fwd in poses:
        r = client.post("/measure", json={
            "lights": [_ue_payload_light()],
            "meter": {"location": loc, "forward": fwd},
            "units": "cm",
        })
        assert r.status_code == 200
        per_call.append(r.json()["illuminanceLux"])

    r = client.post("/measure_batch", json={
        "lights": [_ue_payload_light()],
        "meters": [{"location": l, "forward": f} for l, f in poses],
        "units": "cm",
    })
    assert r.status_code == 200
    batched = r.json()["illuminanceLux"]

    for a, b in zip(per_call, batched):
        assert a == pytest.approx(b, rel=1e-6)


# ---------- reflector tests ----------

def _ue_payload_light_above(z_cm: float = 100.0):
    return {
        "location":   {"x": 0.0, "y": 0.0, "z": z_cm},     # above origin
        "forward":    {"x": 0.0, "y": 0.0, "z": -1.0},     # shining down
        "up":         {"x": 1.0, "y": 0.0, "z":  0.0},
        "iESFilePath": str(FIXTURE_PATH),
        "intensityLumens": 0.0,
        "displayName": "Iso",
    }


def _ue_payload_reflector(width_cm=10.0, height_cm=10.0, albedo=0.5):
    return {
        "location": {"x": 0.0, "y": 0.0, "z": 0.0},
        "forward":  {"x": 0.0, "y": 0.0, "z": 1.0},        # normal up
        "up":       {"x": 0.0, "y": 1.0, "z": 0.0},        # height axis
        "widthCm":  width_cm,
        "heightCm": height_cm,
        "albedo":   albedo,
        "displayName": "Refl",
    }


def test_reflector_lambertian_bounce():
    """Light at 1 m above the origin pointing down, 10×10 cm reflector at the
    origin facing up, meter 0.5 m above the reflector facing DOWN (so direct
    light is blocked). Only the bounce reaches the meter.

    Analytic: each cm² patch sees ~1000 lx incident → outgoing luminance
    L = 0.5·1000/π ≈ 159 cd/m². 100 patches at ~0.5 m → reflected ≈ 6.4 lx.
    """
    payload = {
        "lights": [_ue_payload_light_above(z_cm=100.0)],
        "meter": {
            "location": {"x": 0.0, "y": 0.0, "z": 50.0},
            "forward":  {"x": 0.0, "y": 0.0, "z": -1.0},   # facing the reflector
        },
        "reflectors": [_ue_payload_reflector()],
        "units": "cm",
    }
    r = TestClient(app).post("/measure", json=payload)
    assert r.status_code == 200
    body = r.json()

    total = body["illuminanceLux"]
    # Direct light is blocked (meter facing away from light source above), so
    # total ≈ reflected contribution only.
    assert 5.0 < total < 7.5, f"Expected ~6 lx reflected, got {total}"

    refl_rows = [d for d in body["debug"] if d.get("displayName") == "(reflector)"]
    assert len(refl_rows) == 1
    assert refl_rows[0]["contributionLux"] == pytest.approx(total, abs=1e-3)


def test_reflector_zero_albedo_no_contribution():
    payload = {
        "lights": [_ue_payload_light_above()],
        "meter": {
            "location": {"x": 0.0, "y": 0.0, "z": 50.0},
            "forward":  {"x": 0.0, "y": 0.0, "z": -1.0},
        },
        "reflectors": [_ue_payload_reflector(albedo=0.0)],
        "units": "cm",
    }
    r = TestClient(app).post("/measure", json=payload)
    assert r.status_code == 200
    assert r.json()["illuminanceLux"] == pytest.approx(0.0, abs=1e-3)


def test_reflector_facing_away_no_contribution():
    """Reflector normal pointing AWAY from the meter → 0 contribution."""
    refl = _ue_payload_reflector()
    refl["forward"] = {"x": 0.0, "y": 0.0, "z": -1.0}  # normal pointing down, away from meter above
    payload = {
        "lights": [_ue_payload_light_above()],
        "meter": {
            "location": {"x": 0.0, "y": 0.0, "z": 50.0},
            "forward":  {"x": 0.0, "y": 0.0, "z": -1.0},
        },
        "reflectors": [refl],
        "units": "cm",
    }
    r = TestClient(app).post("/measure", json=payload)
    assert r.status_code == 200
    assert r.json()["illuminanceLux"] == pytest.approx(0.0, abs=1e-3)


def test_reflector_in_measure_batch():
    """Same geometry as the single-meter reflector test, sent via /measure_batch.
    Both meter poses should include the reflected component."""
    meters = [
        {"location": {"x": 0.0, "y": 0.0, "z": 50.0}, "forward": {"x": 0.0, "y": 0.0, "z": -1.0}},
        {"location": {"x": 0.0, "y": 0.0, "z": 30.0}, "forward": {"x": 0.0, "y": 0.0, "z": -1.0}},
    ]
    payload = {
        "lights": [_ue_payload_light_above()],
        "meters": meters,
        "reflectors": [_ue_payload_reflector()],
        "units": "cm",
    }
    r = TestClient(app).post("/measure_batch", json=payload)
    assert r.status_code == 200
    lux = r.json()["illuminanceLux"]
    assert len(lux) == 2
    # Closer meter should see more reflected light.
    assert lux[1] > lux[0]
    assert lux[0] > 0.0
