"""Convert a number-change CSV from app.py into an SRT subtitle file.

Each CSV row becomes one subtitle, displayed from its timestamp until the
next row's timestamp (i.e., until the next change). Import the resulting
.srt into DaVinci Resolve via File > Import > Subtitle, or drag it onto a
subtitle track in the timeline.
"""

from __future__ import annotations

import argparse
import csv
import json
import shutil
import subprocess
import sys
from pathlib import Path


def _ms_to_srt(ms: int) -> str:
    ms = max(0, ms)
    h, rem = divmod(ms, 3_600_000)
    m, rem = divmod(rem, 60_000)
    s, ms = divmod(rem, 1000)
    return f"{h:02d}:{m:02d}:{s:02d},{ms:03d}"


def _probe_duration_ms(video: Path) -> int | None:
    if not shutil.which("ffprobe"):
        return None
    try:
        out = subprocess.run(
            ["ffprobe", "-v", "error", "-show_entries", "format=duration",
             "-of", "json", str(video)],
            check=True, capture_output=True, text=True,
        )
        duration = json.loads(out.stdout).get("format", {}).get("duration")
        return int(float(duration) * 1000) if duration is not None else None
    except (subprocess.CalledProcessError, json.JSONDecodeError, ValueError):
        return None


def _load_rows(csv_path: Path) -> list[tuple[int, int, str]]:
    rows: list[tuple[int, int, str]] = []
    with csv_path.open("r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        for r in reader:
            frame = int(r["frame"])
            start_ms = int(r["seconds"]) * 1000 + int(r["milliseconds"])
            rows.append((frame, start_ms, r["value"]))
    rows.sort(key=lambda r: r[1])
    return rows


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("csv", help="Input CSV from app.py.")
    p.add_argument("--out", default=None,
                   help="Output SRT path (default: <csv path with .srt extension>).")
    p.add_argument("--video", default=None,
                   help="Optional source video; if given, the LAST subtitle runs "
                        "to end-of-video (probed via ffprobe).")
    p.add_argument("--final-duration", type=float, default=5.0,
                   help="Seconds to show the last value when --video is not given "
                        "(default 5).")
    p.add_argument("--show-frame", action="store_true",
                   help="Append the source frame number to each subtitle line "
                        "(useful for cross-checking against the video timeline).")
    args = p.parse_args()

    csv_path = Path(args.csv).resolve()
    if not csv_path.exists():
        sys.exit(f"ERROR: CSV not found: {csv_path}")

    out_path = Path(args.out).resolve() if args.out else csv_path.with_suffix(".srt")
    out_path.parent.mkdir(parents=True, exist_ok=True)

    rows = _load_rows(csv_path)
    if not rows:
        sys.exit("ERROR: CSV has no rows.")

    if args.video:
        duration_ms = _probe_duration_ms(Path(args.video).resolve())
        if duration_ms is None:
            print("WARNING: could not probe video duration; falling back to "
                  "--final-duration.", file=sys.stderr)
            last_end_ms = rows[-1][1] + int(args.final_duration * 1000)
        else:
            last_end_ms = duration_ms
    else:
        last_end_ms = rows[-1][1] + int(args.final_duration * 1000)

    with out_path.open("w", encoding="utf-8", newline="\n") as f:
        for i, (frame, start_ms, value) in enumerate(rows):
            if i + 1 < len(rows):
                end_ms = rows[i + 1][1] - 1
            else:
                end_ms = last_end_ms
            if end_ms <= start_ms:
                end_ms = start_ms + 1
            text = f"{value}  [f{frame}]" if args.show_frame else value
            f.write(f"{i + 1}\n")
            f.write(f"{_ms_to_srt(start_ms)} --> {_ms_to_srt(end_ms)}\n")
            f.write(f"{text}\n\n")

    print(f"Wrote {len(rows)} subtitles to {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
