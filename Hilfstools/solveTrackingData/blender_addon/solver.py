"""Solve rigid body poses from OptiTrack/Motive CSV exports.

Reads a Motive CSV (Format Version 1.23, Quaternion rotation, Global space),
solves the pose of one or more user-defined rigid bodies using marker positions
from a 3D scan (in JSON files), and writes a new Motive-style CSV.

Numpy-only. Runs in Blender's bundled Python and as a standalone script.
"""

from __future__ import annotations

import argparse
import csv
import itertools
import json
import math
import sys
import uuid
from dataclasses import dataclass

import numpy as np


META_KEYS_ORDERED = [
    "Format Version",
    "Take Name",
    "Take Notes",
    "Capture Frame Rate",
    "Export Frame Rate",
    "Capture Start Time",
    "Capture Start Frame",
    "Total Frames in Take",
    "Total Exported Frames",
    "Rotation Type",
    "Length Units",
    "Coordinate Space",
]


@dataclass
class BodyDef:
    name: str
    rb_id: str
    marker_names: list[str]
    marker_positions_scan: np.ndarray  # (N, 3) in scan units, relative to user-chosen origin


@dataclass
class BodySolved:
    body: BodyDef
    scale: float
    corr_rmse: float  # residual (mm) of the resolved marker correspondence on its sample frame
    mean_residual_mm: float  # mean Kabsch fit residual (mm) across all solved frames
    max_residual_mm: float  # worst Kabsch fit residual (mm) across all solved frames
    scaled_marker_positions: np.ndarray  # (N, 3) in mm, relative to origin (after Umeyama scale)
    marker_col_indices: list[tuple[int, int, int]]
    poses: list[tuple[int, float, "np.ndarray | None", "np.ndarray | None"]]


def read_motive_csv(path: str) -> tuple[dict, list[list[str]], list[list[str]]]:
    """Read a Motive CSV.

    Returns (metadata, header_rows, data_rows). header_rows are the 7 raw header
    rows (rows 1..7 from the file). data_rows is everything after.
    """
    with open(path, "r", newline="", encoding="utf-8") as f:
        reader = csv.reader(f)
        rows = list(reader)

    if len(rows) < 8:
        raise ValueError(f"CSV too short to be a Motive export: {path}")

    meta_row = rows[0]
    metadata: dict[str, str] = {}
    i = 0
    while i < len(meta_row) - 1:
        key = meta_row[i].strip()
        if key:
            metadata[key] = meta_row[i + 1].strip()
        i += 2

    header_rows = rows[:7]
    data_rows = rows[7:]
    return metadata, header_rows, data_rows


def build_marker_column_index(header_rows: list[list[str]]) -> dict[str, tuple[int, int, int]]:
    """Map labeled `Marker` (world-frame) column name -> (x_col, y_col, z_col).

    Row indices: header_rows[2]=Type, header_rows[3]=Name, header_rows[5]=Datatype,
    header_rows[6]=Axis.
    """
    type_row = header_rows[2]
    name_row = header_rows[3]
    datatype_row = header_rows[5]
    axis_row = header_rows[6]

    max_cols = max(len(type_row), len(name_row), len(datatype_row), len(axis_row))

    def cell(row: list[str], idx: int) -> str:
        return row[idx].strip() if idx < len(row) else ""

    per_marker: dict[str, dict[str, int]] = {}
    for c in range(max_cols):
        if cell(type_row, c) != "Marker":
            continue
        if cell(datatype_row, c) != "Position":
            continue
        name = cell(name_row, c)
        axis = cell(axis_row, c)
        if not name or axis not in ("X", "Y", "Z"):
            continue
        per_marker.setdefault(name, {})[axis] = c

    index: dict[str, tuple[int, int, int]] = {}
    for name, axes in per_marker.items():
        if {"X", "Y", "Z"}.issubset(axes):
            index[name] = (axes["X"], axes["Y"], axes["Z"])
    return index


def enumerate_rigid_bodies(header_rows: list[list[str]]) -> list[dict]:
    """Parse the CSV header and return one entry per rigid body found.

    Each entry: {"name": str, "id": str, "markers": [csv_marker_name, ...]}.
    Markers are detected from `Rigid Body Marker` columns (rows 3/4) so that
    only the markers actually belonging to a rigid body are listed (separate
    `Marker`-type columns at the right of the file are ignored).
    """
    type_row = header_rows[2]
    name_row = header_rows[3]
    id_row = header_rows[4]
    datatype_row = header_rows[5]
    axis_row = header_rows[6]
    max_cols = max(len(type_row), len(name_row), len(id_row),
                   len(datatype_row), len(axis_row))

    def cell(row, idx):
        return row[idx].strip() if idx < len(row) else ""

    bodies: dict[str, dict] = {}
    seen_markers: dict[str, set[str]] = {}
    for c in range(max_cols):
        t = cell(type_row, c)
        n = cell(name_row, c)
        if not n:
            continue
        if t == "Rigid Body":
            if n not in bodies:
                bodies[n] = {"name": n, "id": cell(id_row, c), "markers": []}
                seen_markers[n] = set()
        elif t == "Rigid Body Marker":
            if cell(datatype_row, c) != "Position" or cell(axis_row, c) != "X":
                continue
            body_name = n.split(":", 1)[0] if ":" in n else n
            bodies.setdefault(body_name,
                              {"name": body_name, "id": cell(id_row, c), "markers": []})
            seen_markers.setdefault(body_name, set())
            if n not in seen_markers[body_name]:
                seen_markers[body_name].add(n)
                bodies[body_name]["markers"].append(n)
    return list(bodies.values())


def load_body_def(json_path: str) -> BodyDef:
    """Load a rigid body JSON.

    Format v2 (preferred — correspondence resolved by solver):
        {"name": ..., "id": ..., "csv_markers": [name1, name2, ...],
         "scan_positions": [[x,y,z], ...]}
    scan_positions may be in any order; the solver permutes them to match csv_markers.

    Format v1 (legacy — keys must already be CSV marker names):
        {"name": ..., "id": ..., "markers": {"<csv_name>": [x,y,z], ...}}
    """
    with open(json_path, "r", encoding="utf-8") as f:
        d = json.load(f)
    name = d["name"]
    rb_id = d.get("id") or uuid.uuid4().hex.upper()

    if "csv_markers" in d and "scan_positions" in d:
        marker_names = list(d["csv_markers"])
        positions = [[float(v[0]), float(v[1]), float(v[2])] for v in d["scan_positions"]]
        if len(marker_names) != len(positions):
            raise ValueError(
                f"{json_path}: csv_markers ({len(marker_names)}) and scan_positions "
                f"({len(positions)}) must have the same length")
    elif "markers" in d:
        marker_names = []
        positions = []
        for k, v in d["markers"].items():
            if len(v) != 3:
                raise ValueError(f"Marker '{k}' in {json_path} must have 3 coordinates")
            marker_names.append(k)
            positions.append([float(v[0]), float(v[1]), float(v[2])])
    else:
        raise ValueError(f"{json_path}: needs either 'csv_markers'+'scan_positions' or 'markers'")

    if len(marker_names) < 3:
        raise ValueError(f"Body '{name}' needs at least 3 markers, got {len(marker_names)}")
    return BodyDef(name=name, rb_id=rb_id, marker_names=marker_names,
                   marker_positions_scan=np.asarray(positions, dtype=np.float64))


def find_correspondence(scan: np.ndarray, world: np.ndarray) -> tuple[list[int], float]:
    """Find permutation `perm` such that scan[perm[i]] aligns with world[i].

    Brute force over N! permutations. Practical for N <= 8 (40 320 trials).
    Returns (perm_indices_into_scan, rmse_mm).
    """
    n = scan.shape[0]
    if world.shape[0] != n:
        raise ValueError("scan and world point counts must match")
    if n > 9:
        raise ValueError(
            f"find_correspondence: {n} markers exceeds brute-force limit (max 9)")
    best_perm = list(range(n))
    best_rmse = float("inf")
    for perm in itertools.permutations(range(n)):
        src = scan[list(perm)]
        s, R, t = umeyama(src, world, with_scale=True)
        pred = s * (src @ R.T) + t
        rmse = float(np.sqrt(((pred - world) ** 2).sum(axis=1).mean()))
        if rmse < best_rmse:
            best_rmse = rmse
            best_perm = list(perm)
    return best_perm, best_rmse


def umeyama(src: np.ndarray, dst: np.ndarray, with_scale: bool = True):
    """Solve dst ~= s * R @ src + t (Umeyama 1991). Returns (s, R, t)."""
    assert src.shape == dst.shape and src.shape[1] == 3
    n = src.shape[0]
    src_mean = src.mean(axis=0)
    dst_mean = dst.mean(axis=0)
    src_c = src - src_mean
    dst_c = dst - dst_mean
    H = src_c.T @ dst_c / n
    U, S, Vt = np.linalg.svd(H)
    D = np.eye(3)
    if np.linalg.det(U) * np.linalg.det(Vt) < 0:
        D[2, 2] = -1.0
    R = Vt.T @ D @ U.T
    if with_scale:
        var_src = (src_c ** 2).sum() / n
        s = float((S * np.diag(D)).sum() / var_src) if var_src > 0 else 1.0
    else:
        s = 1.0
    t = dst_mean - s * (R @ src_mean)
    return s, R, t


def kabsch(src: np.ndarray, dst: np.ndarray):
    """Solve dst ~= R @ src + t with R a proper rotation (no scale)."""
    assert src.shape == dst.shape and src.shape[1] == 3
    src_mean = src.mean(axis=0)
    dst_mean = dst.mean(axis=0)
    H = (src - src_mean).T @ (dst - dst_mean)
    U, _S, Vt = np.linalg.svd(H)
    D = np.eye(3)
    if np.linalg.det(U) * np.linalg.det(Vt) < 0:
        D[2, 2] = -1.0
    R = Vt.T @ D @ U.T
    t = dst_mean - R @ src_mean
    return R, t


def matrix_to_quat_xyzw(R: np.ndarray) -> np.ndarray:
    """Convert a 3x3 rotation matrix to quaternion (x, y, z, w). Shepperd's method."""
    m00, m01, m02 = R[0, 0], R[0, 1], R[0, 2]
    m10, m11, m12 = R[1, 0], R[1, 1], R[1, 2]
    m20, m21, m22 = R[2, 0], R[2, 1], R[2, 2]
    tr = m00 + m11 + m22
    if tr > 0.0:
        s = math.sqrt(tr + 1.0) * 2.0
        qw = 0.25 * s
        qx = (m21 - m12) / s
        qy = (m02 - m20) / s
        qz = (m10 - m01) / s
    elif m00 > m11 and m00 > m22:
        s = math.sqrt(1.0 + m00 - m11 - m22) * 2.0
        qw = (m21 - m12) / s
        qx = 0.25 * s
        qy = (m01 + m10) / s
        qz = (m02 + m20) / s
    elif m11 > m22:
        s = math.sqrt(1.0 + m11 - m00 - m22) * 2.0
        qw = (m02 - m20) / s
        qx = (m01 + m10) / s
        qy = 0.25 * s
        qz = (m12 + m21) / s
    else:
        s = math.sqrt(1.0 + m22 - m00 - m11) * 2.0
        qw = (m10 - m01) / s
        qx = (m02 + m20) / s
        qy = (m12 + m21) / s
        qz = 0.25 * s
    q = np.array([qx, qy, qz, qw], dtype=np.float64)
    n = np.linalg.norm(q)
    if n > 0:
        q /= n
    if q[3] < 0:
        q = -q
    return q


def parse_row_marker_positions(row: list[str], cols: list[tuple[int, int, int]]):
    """Return (points (N,3) with NaN for missing, mask (N,) bool of visible)."""
    n = len(cols)
    pts = np.full((n, 3), np.nan, dtype=np.float64)
    for i, (cx, cy, cz) in enumerate(cols):
        if cz >= len(row):
            continue
        sx, sy, sz = row[cx].strip(), row[cy].strip(), row[cz].strip()
        if not sx or not sy or not sz:
            continue
        try:
            pts[i, 0] = float(sx)
            pts[i, 1] = float(sy)
            pts[i, 2] = float(sz)
        except ValueError:
            pass
    mask = ~np.isnan(pts).any(axis=1)
    return pts, mask


def estimate_scale(body: BodyDef,
                   marker_cols: list[tuple[int, int, int]],
                   data_rows: list[list[str]],
                   scan_window: int = 500) -> float:
    """Estimate scan->mm scale via Umeyama on frames with all markers visible."""
    scales: list[float] = []
    n_markers = len(body.marker_names)
    for row in data_rows[:scan_window]:
        pts, mask = parse_row_marker_positions(row, marker_cols)
        if mask.sum() < n_markers:
            continue
        s, _R, _t = umeyama(body.marker_positions_scan, pts, with_scale=True)
        if s > 0 and math.isfinite(s):
            scales.append(s)
    if not scales:
        for row in data_rows[scan_window:]:
            pts, mask = parse_row_marker_positions(row, marker_cols)
            if mask.sum() < n_markers:
                continue
            s, _R, _t = umeyama(body.marker_positions_scan, pts, with_scale=True)
            if s > 0 and math.isfinite(s):
                scales.append(s)
            if len(scales) >= 50:
                break
    if not scales:
        raise RuntimeError(
            f"Body '{body.name}': no frame has all {n_markers} markers visible "
            "- cannot estimate scale.")
    return float(np.median(scales))


def resolve_correspondence(body: BodyDef,
                           marker_cols: list[tuple[int, int, int]],
                           data_rows: list[list[str]],
                           scan_window: int = 500) -> tuple[list[int], float]:
    """Find the permutation of body.marker_positions_scan that matches body.marker_names.

    Searches for the first frame in `scan_window` where all markers are visible,
    runs `find_correspondence`, and returns (perm, rmse_mm).
    """
    n_markers = len(body.marker_names)
    for row in data_rows[:scan_window]:
        pts, mask = parse_row_marker_positions(row, marker_cols)
        if mask.sum() < n_markers:
            continue
        return find_correspondence(body.marker_positions_scan, pts)
    # Fall back: search the rest of the file
    for row in data_rows[scan_window:]:
        pts, mask = parse_row_marker_positions(row, marker_cols)
        if mask.sum() < n_markers:
            continue
        return find_correspondence(body.marker_positions_scan, pts)
    raise RuntimeError(
        f"Body '{body.name}': no frame has all {n_markers} markers visible - "
        "cannot resolve marker correspondence.")


def solve_body(body: BodyDef,
               marker_col_index: dict[str, tuple[int, int, int]],
               data_rows: list[list[str]]) -> BodySolved:
    missing = [m for m in body.marker_names if m not in marker_col_index]
    if missing:
        raise RuntimeError(
            f"Body '{body.name}': markers not found as labeled Markers in CSV: {missing}")
    cols = [marker_col_index[m] for m in body.marker_names]

    perm, perm_rmse = resolve_correspondence(body, cols, data_rows)
    body.marker_positions_scan = body.marker_positions_scan[perm]

    scale = estimate_scale(body, cols, data_rows)
    scaled_scan = body.marker_positions_scan * scale

    poses = []
    residuals: list[float] = []
    prev_q = None
    for row in data_rows:
        if len(row) < 2:
            continue
        try:
            frame = int(float(row[0]))
            time_s = float(row[1])
        except (ValueError, IndexError):
            continue

        pts, mask = parse_row_marker_positions(row, cols)
        # Require all markers visible: a missing marker means Motive lost the
        # track on that frame, so don't fabricate a pose from a partial set.
        if not mask.all():
            poses.append((frame, time_s, None, None))
            continue

        R, t = kabsch(scaled_scan, pts)
        pred = scaled_scan @ R.T + t
        residuals.append(float(np.sqrt(((pred - pts) ** 2).sum(axis=1).mean())))
        q = matrix_to_quat_xyzw(R)
        if prev_q is not None and float(np.dot(prev_q, q)) < 0:
            q = -q
        prev_q = q
        poses.append((frame, time_s, q, t))

    mean_res = float(np.mean(residuals)) if residuals else float("nan")
    max_res = float(np.max(residuals)) if residuals else float("nan")
    return BodySolved(body=body, scale=scale, corr_rmse=perm_rmse,
                      mean_residual_mm=mean_res, max_residual_mm=max_res,
                      scaled_marker_positions=scaled_scan,
                      marker_col_indices=cols, poses=poses)


def format_float(x) -> str:
    if x is None:
        return ""
    if isinstance(x, float) and not math.isfinite(x):
        return ""
    return f"{x:.6f}"


def write_motive_csv(out_path: str, metadata: dict, solved: list[BodySolved]) -> None:
    take_name = metadata.get("Take Name", "Solved") + "_solved"
    meta_out = dict(metadata)
    meta_out["Take Name"] = take_name

    n_frames = max((len(s.poses) for s in solved), default=0)
    if "Total Exported Frames" not in meta_out:
        meta_out["Total Exported Frames"] = str(n_frames)

    meta_cells: list[str] = []
    for k in META_KEYS_ORDERED:
        if k in meta_out:
            meta_cells.extend([k, meta_out[k]])
    for k, v in meta_out.items():
        if k not in META_KEYS_ORDERED:
            meta_cells.extend([k, v])

    type_row = ["", "Type"]
    name_row = ["", "Name"]
    id_row = ["", "ID"]
    datatype_row = ["", ""]
    axis_row = ["Frame", "Time (Seconds)"]

    for s in solved:
        body = s.body
        type_row.extend(["Rigid Body"] * 7)
        name_row.extend([body.name] * 7)
        id_row.extend([body.rb_id] * 7)
        datatype_row.extend(["Rotation"] * 4 + ["Position"] * 3)
        axis_row.extend(["X", "Y", "Z", "W", "X", "Y", "Z"])

    with open(out_path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, lineterminator="\r\n")
        w.writerow(meta_cells)
        w.writerow([])
        w.writerow(type_row)
        w.writerow(name_row)
        w.writerow(id_row)
        w.writerow(datatype_row)
        w.writerow(axis_row)

        for fi in range(n_frames):
            frame_val = None
            time_val = None
            for s in solved:
                if fi < len(s.poses):
                    frame_val = s.poses[fi][0]
                    time_val = s.poses[fi][1]
                    break
            if frame_val is None:
                break
            row = [str(frame_val), format_float(time_val)]
            for s in solved:
                if fi < len(s.poses):
                    _f, _t, q, p = s.poses[fi]
                else:
                    q, p = None, None
                if q is None or p is None:
                    row.extend([""] * 7)
                else:
                    row.extend([format_float(q[0]), format_float(q[1]),
                                format_float(q[2]), format_float(q[3])])
                    row.extend([format_float(p[0]), format_float(p[1]), format_float(p[2])])
            w.writerow(row)


def run(input_csv: str, body_json_paths: list[str], output_csv: str,
        progress_cb=None) -> dict:
    metadata, header_rows, data_rows = read_motive_csv(input_csv)
    marker_col_index = build_marker_column_index(header_rows)

    bodies = [load_body_def(p) for p in body_json_paths]
    solved: list[BodySolved] = []
    for i, b in enumerate(bodies):
        if progress_cb:
            progress_cb(i, len(bodies), b.name)
        solved.append(solve_body(b, marker_col_index, data_rows))
    if progress_cb:
        progress_cb(len(bodies), len(bodies), "writing")

    write_motive_csv(output_csv, metadata, solved)

    return {
        "input": input_csv,
        "output": output_csv,
        "frames": len(data_rows),
        "bodies": [
            {"name": s.body.name, "scale": s.scale,
             "correspondence_rmse_mm": s.corr_rmse,
             "mean_residual_mm": s.mean_residual_mm,
             "max_residual_mm": s.max_residual_mm,
             "marker_order": list(s.body.marker_names),
             "solved_frames": sum(1 for p in s.poses if p[2] is not None),
             "total_frames": len(s.poses)}
            for s in solved
        ],
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Solve rigid body poses from a Motive CSV.")
    parser.add_argument("--in", dest="input_csv", required=True, help="Input Motive CSV")
    parser.add_argument("--bodies", nargs="+", required=True, help="Rigid body JSON files")
    parser.add_argument("--out", dest="output_csv", required=True, help="Output CSV path")
    args = parser.parse_args(argv)

    def cb(i, n, name):
        print(f"[{i}/{n}] {name}", file=sys.stderr)

    summary = run(args.input_csv, args.bodies, args.output_csv, progress_cb=cb)
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
