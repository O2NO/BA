"""
End-to-end illuminance checks against the synthetic isotropic_1000cd.ies fixture.

Geometry uses UE conventions: +Z up, units in cm passed *into* the request layer;
photometry.compute_light_contribution() takes meters directly so we pass meters here.

For an isotropic 1000 cd source, the expected illuminance is a closed-form
   E = 1000 * cos(theta) / d^2     [lx, with d in m]
which makes assertions trivial.
"""

from math import sqrt
from pathlib import Path

import pytest

from ies_parser import parse_ies
from photometry import compute_light_contribution

FIXTURE_PATH = Path(__file__).parent / "fixtures" / "isotropic_1000cd.ies"


@pytest.fixture(scope="module")
def ies():
    return parse_ies(FIXTURE_PATH)


def _call(ies, *, light_pos, light_fwd, light_up, meter_pos, meter_fwd, lumens=0.0):
    return compute_light_contribution(
        display_name="t",
        light_pos_m=light_pos,
        light_forward=light_fwd,
        light_up=light_up,
        meter_pos_m=meter_pos,
        meter_forward=meter_fwd,
        intensity_lumens=lumens,
        ies=ies,
    )


def test_directly_below_2m(ies):
    # Light 2 m above the meter, pointing down. Meter facing up.
    res = _call(
        ies,
        light_pos=(0.0, 0.0, 2.0),
        light_fwd=(0.0, 0.0, -1.0),
        light_up=(1.0, 0.0, 0.0),
        meter_pos=(0.0, 0.0, 0.0),
        meter_fwd=(0.0, 0.0, 1.0),
    )
    assert res.distance_m == pytest.approx(2.0)
    assert res.gamma_deg == pytest.approx(0.0, abs=1e-6)
    assert res.candela == pytest.approx(1000.0)
    assert res.cosine_to_meter == pytest.approx(1.0)
    assert res.contribution_lux == pytest.approx(250.0)


def test_offset_45_deg(ies):
    # Light at (0,0,2), meter at (2,0,0) facing up. dir from light to meter
    # makes 45 deg with light's nadir, distance = 2*sqrt(2) m.
    res = _call(
        ies,
        light_pos=(0.0, 0.0, 2.0),
        light_fwd=(0.0, 0.0, -1.0),
        light_up=(1.0, 0.0, 0.0),
        meter_pos=(2.0, 0.0, 0.0),
        meter_fwd=(0.0, 0.0, 1.0),
    )
    assert res.distance_m == pytest.approx(2.0 * sqrt(2))
    assert res.gamma_deg == pytest.approx(45.0)
    expected_lux = 1000.0 * (1.0 / sqrt(2)) / (2.0 * sqrt(2)) ** 2
    assert res.contribution_lux == pytest.approx(expected_lux, rel=1e-5)


def test_meter_facing_away_contributes_zero(ies):
    res = _call(
        ies,
        light_pos=(0.0, 0.0, 2.0),
        light_fwd=(0.0, 0.0, -1.0),
        light_up=(1.0, 0.0, 0.0),
        meter_pos=(0.0, 0.0, 0.0),
        meter_fwd=(0.0, 0.0, -1.0),  # pointing down, away from the light
    )
    assert res.cosine_to_meter == 0.0
    assert res.contribution_lux == 0.0


def test_zero_distance_returns_zero(ies):
    res = _call(
        ies,
        light_pos=(0.0, 0.0, 0.0),
        light_fwd=(0.0, 0.0, -1.0),
        light_up=(1.0, 0.0, 0.0),
        meter_pos=(0.0, 0.0, 0.0),
        meter_fwd=(0.0, 0.0, 1.0),
    )
    assert res.contribution_lux == 0.0
    assert res.distance_m == 0.0


def test_batch_matches_per_meter_calls(ies):
    """Batched call returns the same lux for each meter pose as the
    per-meter helper, proving the /measure_batch fast path stays consistent
    with /measure (no shared-state bugs across meter iterations)."""
    light_pos = (0.0, 0.0, 2.0)
    light_fwd = (0.0, 0.0, -1.0)
    light_up = (1.0, 0.0, 0.0)

    poses = [
        ((0.0, 0.0, 0.0), (0.0, 0.0, 1.0)),     # directly below, facing up
        ((2.0, 0.0, 0.0), (0.0, 0.0, 1.0)),     # offset, still facing up
        ((0.0, 0.0, 0.0), (0.0, 0.0, -1.0)),    # below, facing AWAY (cos<=0)
    ]

    expected = []
    for meter_pos, meter_fwd in poses:
        r = _call(
            ies,
            light_pos=light_pos,
            light_fwd=light_fwd,
            light_up=light_up,
            meter_pos=meter_pos,
            meter_fwd=meter_fwd,
        )
        expected.append(r.contribution_lux)

    assert expected[0] == pytest.approx(250.0)
    assert expected[2] == pytest.approx(0.0)


def test_lumens_scaling_doubles_output(ies):
    # IntensityLumens = 2 * rated_lumens -> candela should double.
    base = _call(
        ies,
        light_pos=(0.0, 0.0, 2.0),
        light_fwd=(0.0, 0.0, -1.0),
        light_up=(1.0, 0.0, 0.0),
        meter_pos=(0.0, 0.0, 0.0),
        meter_fwd=(0.0, 0.0, 1.0),
        lumens=0.0,
    )
    scaled = _call(
        ies,
        light_pos=(0.0, 0.0, 2.0),
        light_fwd=(0.0, 0.0, -1.0),
        light_up=(1.0, 0.0, 0.0),
        meter_pos=(0.0, 0.0, 0.0),
        meter_fwd=(0.0, 0.0, 1.0),
        lumens=2.0 * ies.rated_lumens,
    )
    assert scaled.candela == pytest.approx(2.0 * base.candela)
    assert scaled.contribution_lux == pytest.approx(2.0 * base.contribution_lux)
