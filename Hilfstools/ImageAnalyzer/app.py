"""Read a number from a video frame-by-frame and emit one CSV row per change."""

from __future__ import annotations

import argparse
import csv
import json
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

import cv2
import numpy as np
import pytesseract


# --- Tesseract discovery -----------------------------------------------------

def _ensure_tesseract() -> None:
    if shutil.which("tesseract"):
        return
    # Common Windows install path from the UB Mannheim build.
    default_win = Path(r"C:\Program Files\Tesseract-OCR\tesseract.exe")
    if default_win.exists():
        pytesseract.pytesseract.tesseract_cmd = str(default_win)
        return
    sys.exit(
        "ERROR: Tesseract binary not found.\n"
        "  Install from https://github.com/UB-Mannheim/tesseract/wiki and add "
        "its install dir to PATH, or edit the path in app.py."
    )


# --- ffprobe summary ---------------------------------------------------------

def _probe(video: Path) -> dict:
    if not shutil.which("ffprobe"):
        return {}
    try:
        out = subprocess.run(
            [
                "ffprobe", "-v", "error", "-select_streams", "v:0",
                "-show_entries",
                "stream=avg_frame_rate,r_frame_rate,nb_frames,duration,width,height,codec_name",
                "-of", "json", str(video),
            ],
            check=True, capture_output=True, text=True,
        )
        return json.loads(out.stdout).get("streams", [{}])[0]
    except (subprocess.CalledProcessError, json.JSONDecodeError):
        return {}


def _print_summary(video: Path, cap: cv2.VideoCapture, probe: dict) -> None:
    fps = cap.get(cv2.CAP_PROP_FPS)
    n = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    duration = n / fps if fps else 0
    print(f"Video:    {video.name}")
    print(f"Size:     {w}x{h}")
    print(f"FPS:      {fps:.3f} (avg from cv2)")
    if probe:
        avg = probe.get("avg_frame_rate", "?")
        r = probe.get("r_frame_rate", "?")
        codec = probe.get("codec_name", "?")
        cfr_hint = " (CFR)" if avg == r else " (likely VFR — check this)"
        print(f"ffprobe:  codec={codec} avg_fps={avg} r_fps={r}{cfr_hint}")
    print(f"Frames:   {n}")
    print(f"Duration: {duration:.3f}s")
    print()


# --- ROI handling ------------------------------------------------------------

@dataclass
class Roi:
    x: int
    y: int
    w: int
    h: int

    def crop(self, frame: np.ndarray) -> np.ndarray:
        return frame[self.y:self.y + self.h, self.x:self.x + self.w]

    def to_list(self) -> list[int]:
        return [self.x, self.y, self.w, self.h]

    @classmethod
    def parse(cls, s: str) -> "Roi":
        parts = [int(p) for p in s.split(",")]
        if len(parts) != 4:
            raise argparse.ArgumentTypeError("ROI must be x,y,w,h")
        return cls(*parts)


def _sidecar_path(video: Path) -> Path:
    return video.with_suffix(video.suffix + ".roi.json")


def _load_roi(video: Path) -> Roi | None:
    p = _sidecar_path(video)
    if not p.exists():
        return None
    data = json.loads(p.read_text())
    return Roi(**data)


def _save_roi(video: Path, roi: Roi) -> None:
    _sidecar_path(video).write_text(json.dumps(roi.__dict__))


def _select_roi_interactive(first_frame: np.ndarray) -> Roi:
    win = "Drag a box around the digits, then press ENTER (or 'c' to cancel)"
    x, y, w, h = cv2.selectROI(win, first_frame, showCrosshair=True, fromCenter=False)
    cv2.destroyWindow(win)
    if w == 0 or h == 0:
        sys.exit("ERROR: no ROI selected.")
    return Roi(int(x), int(y), int(w), int(h))


def _resolve_roi(video: Path, cap: cv2.VideoCapture, args: argparse.Namespace) -> Roi:
    if args.roi:
        roi = args.roi
    elif not args.reselect_roi and (cached := _load_roi(video)):
        print(f"Using cached ROI from {_sidecar_path(video).name}: {cached.to_list()}")
        return cached
    else:
        ok, frame = cap.read()
        if not ok:
            sys.exit("ERROR: could not read first frame for ROI selection.")
        roi = _select_roi_interactive(frame)
        cap.set(cv2.CAP_PROP_POS_FRAMES, 0)
    _save_roi(video, roi)
    print(f"ROI: {roi.to_list()} (saved to {_sidecar_path(video).name})")
    return roi


# --- OCR ---------------------------------------------------------------------

_NUMERIC_RE = re.compile(r"-?\d+(?:\.\d+)?")


def _preprocess(crop_bgr: np.ndarray) -> np.ndarray:
    gray = cv2.cvtColor(crop_bgr, cv2.COLOR_BGR2GRAY)
    h, w = gray.shape
    gray = cv2.resize(gray, (w * 3, h * 3), interpolation=cv2.INTER_CUBIC)
    _, th = cv2.threshold(gray, 0, 255, cv2.THRESH_BINARY + cv2.THRESH_OTSU)
    # Tesseract prefers dark text on light background; if background is dark, invert.
    if np.mean(th) < 127:
        th = cv2.bitwise_not(th)
    return th


def _filter_by_height(
    binary: np.ndarray, allow_decimal: bool, min_ratio: float = 0.6,
) -> np.ndarray:
    """Drop connected components shorter than min_ratio * tallest component.

    Strips smaller adjacent text like the "lx" suffix on a light-meter readout
    where the unit characters can be misread as digits (e.g. "l" → "1"). When
    allow_decimal is True, small components whose horizontal center sits inside
    the digit span and whose bottom sits near the digit baseline are also kept,
    so a decimal point between digits survives while "lx" beside them does not.
    Input is white background / dark text; output is the same.
    """
    inv = cv2.bitwise_not(binary)
    n_labels, labels, stats, _ = cv2.connectedComponentsWithStats(inv, connectivity=8)
    if n_labels <= 1:
        return binary
    heights = stats[1:, cv2.CC_STAT_HEIGHT]
    max_h = int(heights.max())
    threshold_h = max_h * min_ratio

    tall = {i for i in range(1, n_labels) if stats[i, cv2.CC_STAT_HEIGHT] >= threshold_h}
    if not tall:
        return binary

    keep = np.zeros_like(inv)
    for i in tall:
        keep[labels == i] = 255

    if allow_decimal:
        xs_left = min(stats[i, cv2.CC_STAT_LEFT] for i in tall)
        xs_right = max(
            stats[i, cv2.CC_STAT_LEFT] + stats[i, cv2.CC_STAT_WIDTH] for i in tall
        )
        ys_bottom = max(
            stats[i, cv2.CC_STAT_TOP] + stats[i, cv2.CC_STAT_HEIGHT] for i in tall
        )
        avg_digit_h = float(np.mean([stats[i, cv2.CC_STAT_HEIGHT] for i in tall]))

        for i in range(1, n_labels):
            if i in tall:
                continue
            x = stats[i, cv2.CC_STAT_LEFT]
            y = stats[i, cv2.CC_STAT_TOP]
            w = stats[i, cv2.CC_STAT_WIDTH]
            h = stats[i, cv2.CC_STAT_HEIGHT]
            cx = x + w / 2
            bottom = y + h
            if (
                h < 0.4 * max_h
                and xs_left <= cx <= xs_right
                and bottom >= ys_bottom - 0.2 * avg_digit_h
            ):
                keep[labels == i] = 255

    return cv2.bitwise_not(keep)


def _ocr_number(crop_bgr: np.ndarray, allow_decimal: bool, height_filter: bool) -> str | None:
    img = _preprocess(crop_bgr)
    if height_filter:
        img = _filter_by_height(img, allow_decimal)
    whitelist = "0123456789.-" if allow_decimal else "0123456789-"
    config = f"--psm 7 -c tessedit_char_whitelist={whitelist}"
    text = pytesseract.image_to_string(img, config=config).strip()
    m = _NUMERIC_RE.search(text)
    if not m:
        return None
    val = m.group(0)
    if not allow_decimal and "." in val:
        val = val.split(".")[0]
    return val or None


# --- Main loop ---------------------------------------------------------------

def run(args: argparse.Namespace) -> int:
    video = Path(args.video).resolve()
    if not video.exists():
        sys.exit(f"ERROR: video not found: {video}")

    _ensure_tesseract()
    probe = _probe(video)

    cap = cv2.VideoCapture(str(video))
    if not cap.isOpened():
        sys.exit(f"ERROR: OpenCV could not open {video}")

    _print_summary(video, cap, probe)
    fps = cap.get(cv2.CAP_PROP_FPS) or 0.0
    total_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))

    roi = _resolve_roi(video, cap, args)

    out_path = Path(args.out).resolve()
    out_path.parent.mkdir(parents=True, exist_ok=True)

    prev_crop_gray: np.ndarray | None = None
    last_ocr_value: str | None = None  # last OCR-confirmed value (for diff-skip reuse)

    stable_value: str | None = None     # currently committed value
    candidate_value: str | None = None  # value waiting for debounce confirmation
    candidate_frame: int = -1
    candidate_t_ms: float = 0.0
    candidate_streak: int = 0

    rows_written = 0
    frame_idx = 0

    with out_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(["frame", "seconds", "milliseconds", "value"])

        while True:
            ok, frame = cap.read()
            if not ok:
                break

            t_ms = cap.get(cv2.CAP_PROP_POS_MSEC)
            if t_ms <= 0 and fps > 0:
                t_ms = frame_idx * 1000.0 / fps

            crop = roi.crop(frame)
            crop_gray = cv2.cvtColor(crop, cv2.COLOR_BGR2GRAY)

            triggered = (
                prev_crop_gray is None
                or float(np.mean(cv2.absdiff(crop_gray, prev_crop_gray))) >= args.diff_threshold
            )
            prev_crop_gray = crop_gray

            if triggered:
                value = _ocr_number(crop, args.allow_decimal, args.height_filter)
                if value is not None:
                    last_ocr_value = value
            else:
                value = last_ocr_value

            if value is None:
                # OCR failure on a triggered frame — leave any pending candidate alone.
                pass
            elif value == stable_value:
                # Change reverted before debounce confirmed it; drop the candidate.
                candidate_value = None
                candidate_streak = 0
            else:
                if value != candidate_value:
                    candidate_value = value
                    candidate_frame = frame_idx
                    candidate_t_ms = t_ms
                    candidate_streak = 1
                else:
                    candidate_streak += 1

                if candidate_streak >= args.debounce:
                    seconds = int(candidate_t_ms // 1000)
                    millis = int(round(candidate_t_ms - seconds * 1000))
                    if millis == 1000:
                        seconds += 1
                        millis = 0
                    writer.writerow([candidate_frame, seconds, millis, candidate_value])
                    rows_written += 1
                    stable_value = candidate_value
                    candidate_value = None
                    candidate_streak = 0

            frame_idx += 1
            if total_frames and frame_idx % max(1, total_frames // 20) == 0:
                pct = 100.0 * frame_idx / total_frames
                print(f"  {pct:5.1f}%  frame {frame_idx}/{total_frames}  rows={rows_written}")

    cap.release()
    print(f"\nDone. Wrote {rows_written} rows to {out_path}")
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("video", help="Path to the input video file.")
    p.add_argument("--out", default="dataset.csv", help="Output CSV path (default: dataset.csv).")
    p.add_argument(
        "--roi", type=Roi.parse, default=None,
        help="ROI as x,y,w,h (in pixels). If omitted, prompts interactively or reuses sidecar.",
    )
    p.add_argument(
        "--reselect-roi", action="store_true",
        help="Force interactive ROI selection even if a sidecar exists.",
    )
    p.add_argument(
        "--debounce", type=int, default=2,
        help="Frames a new value must persist before it's committed (default: 2). "
             "Recorded timestamp is still the FIRST frame the value appeared.",
    )
    p.add_argument(
        "--diff-threshold", type=float, default=2.0,
        help="Mean absolute pixel diff (0-255) required to retrigger OCR (default: 2.0). "
             "Lower = more OCR calls, higher = faster but may miss subtle changes.",
    )
    p.add_argument(
        "--allow-decimal", action="store_true",
        help="Permit a decimal point in the parsed number.",
    )
    p.add_argument(
        "--no-height-filter", dest="height_filter", action="store_false",
        help="Disable the connected-component height filter that strips smaller "
             "adjacent text (e.g. unit suffixes like 'lx'). With --allow-decimal, "
             "the filter preserves decimal points sitting between digits.",
    )
    return run(p.parse_args())


if __name__ == "__main__":
    raise SystemExit(main())
