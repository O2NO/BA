from pathlib import Path

import pytest

from ies_parser import parse_ies

FIXTURE = Path(__file__).parent / "fixtures" / "isotropic_1000cd.ies"


def test_parses_isotropic_fixture():
    ies = parse_ies(FIXTURE)
    assert ies.photometric_type == 1
    assert ies.units_type == 2
    assert ies.vertical_angles == [0.0, 90.0, 180.0]
    assert ies.horizontal_angles == [0.0]
    assert ies.candela == [[1000.0], [1000.0], [1000.0]]
    assert ies.rated_lumens == pytest.approx(12566.37, rel=1e-4)


def test_parses_keywords():
    ies = parse_ies(FIXTURE)
    assert ies.keywords["MANUFAC"] == "LuxMeterCalc"
    assert "1000" in ies.keywords["OTHER"]


def test_rejects_non_type_c():
    bad = (
        "IESNA:LM-63-2002\n"
        "TILT=NONE\n"
        # Type B (= 2) instead of C
        "1 1000 1.0 2 1 2 2 0 0 0\n"
        "1.0 1.0 100.0\n"
        "0 90\n"
        "0\n"
        "100 100\n"
    )
    from ies_parser import parse_ies_text
    with pytest.raises(ValueError, match="Type C"):
        parse_ies_text(bad)


def test_rejects_tilt_include():
    bad = (
        "IESNA:LM-63-2002\n"
        "TILT=INCLUDE\n"
    )
    from ies_parser import parse_ies_text
    with pytest.raises(ValueError, match="TILT=NONE"):
        parse_ies_text(bad)
