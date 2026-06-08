"""
IESNA LM-63 photometric file parser.

Supports:
- LM-63-1986 / 1991 / 1995 / 2002
- Type C photometry (the common architectural case)
- TILT=NONE only

Out of scope (raises ValueError):
- Type A/B photometry
- TILT=INCLUDE / external TILT files
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class IESPhotometry:
    rated_lumens: float | None      # None if absolute photometry (-1 in file)
    candela_multiplier: float
    photometric_type: int           # 1 = C, 2 = B, 3 = A
    units_type: int                 # 1 = feet, 2 = meters
    width: float
    length: float
    height: float
    ballast_factor: float
    input_watts: float
    vertical_angles: list[float]
    horizontal_angles: list[float]
    candela: list[list[float]]      # [v_index][h_index], already multiplied
    keywords: dict[str, str] = field(default_factory=dict)


def parse_ies(path: str | Path) -> IESPhotometry:
    text = Path(path).read_text(encoding="utf-8", errors="replace")
    return parse_ies_text(text)


def parse_ies_text(text: str) -> IESPhotometry:
    lines = [ln.rstrip("\r\n") for ln in text.splitlines()]
    # Skip leading blanks; ignore the format-id line ("IESNA:LM-63-...") if present.
    i = 0
    while i < len(lines) and not lines[i].strip():
        i += 1
    if i < len(lines) and lines[i].lstrip().upper().startswith("IESNA"):
        i += 1

    keywords: dict[str, str] = {}
    while i < len(lines):
        s = lines[i].strip()
        if s.upper().startswith("TILT="):
            break
        if s.startswith("[") and "]" in s:
            key_end = s.index("]")
            key = s[1:key_end].strip()
            val = s[key_end + 1 :].strip()
            keywords[key] = val
        i += 1

    if i >= len(lines):
        raise ValueError("IES file missing TILT= line")

    tilt_line = lines[i].strip().upper()
    i += 1
    if tilt_line != "TILT=NONE":
        raise ValueError(f"Only TILT=NONE is supported (got '{tilt_line}')")

    # From here on the file is whitespace-separated tokens, free-form across lines.
    tokens: list[str] = []
    for ln in lines[i:]:
        tokens.extend(ln.split())

    if len(tokens) < 13:
        raise ValueError("IES file truncated before photometric header")

    def take(n: int) -> list[str]:
        nonlocal cursor
        chunk = tokens[cursor : cursor + n]
        cursor += n
        return chunk

    cursor = 0
    num_lamps        = int(float(tokens[cursor])); cursor += 1
    lumens_per_lamp  = float(tokens[cursor]);      cursor += 1
    candela_mult     = float(tokens[cursor]);      cursor += 1
    n_vert           = int(tokens[cursor]);        cursor += 1
    n_horz           = int(tokens[cursor]);        cursor += 1
    photometric_type = int(tokens[cursor]);        cursor += 1
    units_type       = int(tokens[cursor]);        cursor += 1
    width            = float(tokens[cursor]);      cursor += 1
    length           = float(tokens[cursor]);      cursor += 1
    height           = float(tokens[cursor]);      cursor += 1
    ballast_factor   = float(tokens[cursor]);      cursor += 1
    _future          = float(tokens[cursor]);      cursor += 1  # ballast-lamp factor in legacy spec; ignored
    input_watts      = float(tokens[cursor]);      cursor += 1

    if photometric_type != 1:
        raise ValueError(
            f"Only Type C photometry is supported (got type {photometric_type}). "
            "Type A/B luminaires need a different angle convention."
        )

    if cursor + n_vert + n_horz + n_vert * n_horz > len(tokens):
        raise ValueError("IES file truncated within angle/candela arrays")

    vertical = [float(t) for t in take(n_vert)]
    horizontal = [float(t) for t in take(n_horz)]

    # Candela values: in LM-63 the file lists, for each horizontal angle, a row of
    # `n_vert` candela values. We store them as candela[v_index][h_index].
    candela: list[list[float]] = [[0.0] * n_horz for _ in range(n_vert)]
    for h in range(n_horz):
        row = take(n_vert)
        for v in range(n_vert):
            candela[v][h] = float(row[v]) * candela_mult

    rated_lumens: float | None
    if lumens_per_lamp < 0:
        # LM-63 sentinel for "absolute photometry" — candela values are absolute.
        rated_lumens = None
    else:
        rated_lumens = num_lamps * lumens_per_lamp

    return IESPhotometry(
        rated_lumens=rated_lumens,
        candela_multiplier=candela_mult,
        photometric_type=photometric_type,
        units_type=units_type,
        width=width,
        length=length,
        height=height,
        ballast_factor=ballast_factor,
        input_watts=input_watts,
        vertical_angles=vertical,
        horizontal_angles=horizontal,
        candela=candela,
        keywords=keywords,
    )
