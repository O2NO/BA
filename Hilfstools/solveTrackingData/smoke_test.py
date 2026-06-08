"""Smoke test for solver.py.

Synthesizes rigid-body JSONs from the existing Kallibrierung_Origin.csv by reading
the Rigid Body Marker local-frame columns (Motive's own asset definitions). Runs
the solver and verifies that for one frame with all markers visible, the
reconstructed world positions agree with the input CSV.
"""

from __future__ import annotations

import csv
import json
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "blender_addon"))
import solver  # noqa: E402


INPUT_CSV = os.path.normpath(os.path.join(HERE, "..", "Kallibrierung_Origin.csv"))
OUT_DIR = HERE


def _quat_to_R(qx, qy, qz, qw):
    x, y, z, w = qx, qy, qz, qw
    return np.array([
        [1 - 2*(y*y + z*z),     2*(x*y - z*w),       2*(x*z + y*w)],
        [    2*(x*y + z*w), 1 - 2*(x*x + z*z),       2*(y*z - x*w)],
        [    2*(x*z - y*w),     2*(y*z + x*w),   1 - 2*(x*x + y*y)],
    ])


def extract_rigid_body_marker_positions(header_rows, body_name, marker_names, data_rows):
    """Build a self-consistent local-frame marker definition by inverting Motive's
    own solved pose on a frame where the body is tracked and all markers visible.

    local[i] = R^T @ (world[i] - t) using Motive's quaternion and position.
    """
    type_row = header_rows[2]
    name_row = header_rows[3]
    datatype_row = header_rows[5]
    axis_row = header_rows[6]
    max_cols = max(len(type_row), len(name_row), len(datatype_row), len(axis_row))

    def cell(row, idx):
        return row[idx].strip() if idx < len(row) else ""

    # Find body rotation + position columns.
    rot_cols = {}
    pos_cols = {}
    for c in range(max_cols):
        if cell(type_row, c) != "Rigid Body":
            continue
        if cell(name_row, c) != body_name:
            continue
        dt = cell(datatype_row, c); ax = cell(axis_row, c)
        if dt == "Rotation" and ax in ("X", "Y", "Z", "W"):
            rot_cols[ax] = c
        elif dt == "Position" and ax in ("X", "Y", "Z"):
            pos_cols[ax] = c

    # Find world-frame Marker columns for these names.
    world_cols = {}
    for c in range(max_cols):
        if cell(type_row, c) != "Marker":
            continue
        if cell(datatype_row, c) != "Position":
            continue
        n = cell(name_row, c); a = cell(axis_row, c)
        if n in marker_names and a in ("X", "Y", "Z"):
            world_cols.setdefault(n, {})[a] = c

    # Find a frame where pose is non-trivial AND all markers are visible.
    for row in data_rows:
        try:
            qx = float(row[rot_cols["X"]]); qy = float(row[rot_cols["Y"]])
            qz = float(row[rot_cols["Z"]]); qw = float(row[rot_cols["W"]])
            tx = float(row[pos_cols["X"]]); ty = float(row[pos_cols["Y"]])
            tz = float(row[pos_cols["Z"]])
        except (ValueError, IndexError):
            continue
        # Reject identity pose (Motive's "unsolved" placeholder).
        if abs(qx) + abs(qy) + abs(qz) < 1e-6 and abs(tx) + abs(ty) + abs(tz) < 1e-6:
            continue
        worlds = {}
        ok = True
        for n in marker_names:
            ax = world_cols.get(n, {})
            if not {"X", "Y", "Z"}.issubset(ax):
                ok = False; break
            try:
                worlds[n] = np.array([float(row[ax["X"]]), float(row[ax["Y"]]), float(row[ax["Z"]])])
            except (ValueError, IndexError):
                ok = False; break
        if not ok:
            continue
        R = _quat_to_R(qx, qy, qz, qw)
        t = np.array([tx, ty, tz])
        out = {}
        for n in marker_names:
            local = R.T @ (worlds[n] - t)
            out[n] = [float(local[0]), float(local[1]), float(local[2])]
        return out
    return {}


def make_body_json(out_path, body_name, marker_names, header_rows, data_rows, shuffle_seed=None):
    """Write a v2 rigid body JSON. If shuffle_seed is given, the scan_positions
    list is permuted so the solver must resolve correspondence on its own."""
    positions = extract_rigid_body_marker_positions(header_rows, body_name, marker_names, data_rows)
    if len(positions) != len(marker_names):
        raise RuntimeError(f"Could not extract all marker positions for {body_name}: {positions}")
    # Preserve CSV marker order in csv_markers; shuffle scan_positions to exercise auto-correspondence.
    ordered_scan = [positions[m] for m in marker_names]
    if shuffle_seed is not None:
        rng = np.random.default_rng(shuffle_seed)
        order = list(range(len(ordered_scan)))
        rng.shuffle(order)
        ordered_scan = [ordered_scan[i] for i in order]
        print(f"  [{body_name}] scan_positions shuffled with permutation {order}")
    data = {"name": body_name, "csv_markers": marker_names, "scan_positions": ordered_scan}
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)
    return data


def verify_frame(input_csv_path, output_csv_path, body_name, marker_names, frame_to_check=2):
    """Compare reconstructed world positions from output rigid body pose to input world marker columns."""
    # Read input world-frame markers and find the row.
    in_meta, in_hdr, in_data = solver.read_motive_csv(input_csv_path)
    in_marker_idx = solver.build_marker_column_index(in_hdr)

    # Read output rigid body rotation/position columns.
    out_meta, out_hdr, out_data = solver.read_motive_csv(output_csv_path)

    type_row = out_hdr[2]; name_row = out_hdr[3]
    datatype_row = out_hdr[5]; axis_row = out_hdr[6]
    max_cols = max(len(type_row), len(name_row), len(datatype_row), len(axis_row))

    def cell(row, idx):
        return row[idx].strip() if idx < len(row) else ""

    rot_cols = {}
    pos_cols = {}
    rbm_cols = {}  # rigid body marker local positions
    for c in range(max_cols):
        if cell(name_row, c) != body_name and cell(name_row, c) not in marker_names:
            continue
        if cell(name_row, c) == body_name and cell(type_row, c) == "Rigid Body":
            if cell(datatype_row, c) == "Rotation":
                rot_cols[cell(axis_row, c)] = c
            elif cell(datatype_row, c) == "Position":
                pos_cols[cell(axis_row, c)] = c
        elif cell(type_row, c) == "Rigid Body Marker" and cell(datatype_row, c) == "Position":
            n = cell(name_row, c); a = cell(axis_row, c)
            rbm_cols.setdefault(n, {})[a] = c

    # Find row with the target frame in BOTH files.
    def row_with_frame(rows, frame):
        for r in rows:
            try:
                if int(float(r[0])) == frame:
                    return r
            except (ValueError, IndexError):
                continue
        return None

    in_row = row_with_frame(in_data, frame_to_check)
    out_row = row_with_frame(out_data, frame_to_check)
    if in_row is None or out_row is None:
        return None

    qx = float(out_row[rot_cols["X"]]); qy = float(out_row[rot_cols["Y"]])
    qz = float(out_row[rot_cols["Z"]]); qw = float(out_row[rot_cols["W"]])
    tx = float(out_row[pos_cols["X"]]); ty = float(out_row[pos_cols["Y"]])
    tz = float(out_row[pos_cols["Z"]])

    # Build rotation from quaternion (xyzw)
    x, y, z, w = qx, qy, qz, qw
    R = np.array([
        [1 - 2*(y*y + z*z),     2*(x*y - z*w),       2*(x*z + y*w)],
        [    2*(x*y + z*w), 1 - 2*(x*x + z*z),       2*(y*z - x*w)],
        [    2*(x*z - y*w),     2*(y*z + x*w),   1 - 2*(x*x + y*y)],
    ])
    t = np.array([tx, ty, tz])

    print(f"\n[{body_name}] frame {frame_to_check} | q=({qx:.4f},{qy:.4f},{qz:.4f},{qw:.4f}) "
          f"|q|={np.sqrt(x*x+y*y+z*z+w*w):.6f} | t=({tx:.2f},{ty:.2f},{tz:.2f})")

    max_err = 0.0
    for mname in marker_names:
        # local position from output's RB marker columns
        ax = rbm_cols.get(mname, {})
        if not {"X", "Y", "Z"}.issubset(ax):
            continue
        local = np.array([float(out_row[ax["X"]]), float(out_row[ax["Y"]]), float(out_row[ax["Z"]])])
        reconstructed_world = R @ local + t

        # actual world from input CSV
        cidx = in_marker_idx.get(mname)
        if cidx is None:
            continue
        cx, cy, cz = cidx
        try:
            actual = np.array([float(in_row[cx]), float(in_row[cy]), float(in_row[cz])])
        except ValueError:
            continue

        err = float(np.linalg.norm(reconstructed_world - actual))
        max_err = max(max_err, err)
        print(f"  {mname}: world={actual.round(3)} reconstructed={reconstructed_world.round(3)} err={err:.4f}mm")
    print(f"  MAX ERROR: {max_err:.4f}mm")
    return max_err


def main():
    metadata, header_rows, data_rows = solver.read_motive_csv(INPUT_CSV)

    belicht_markers = [f"Belichtungsmesser:Marker{i}" for i in range(1, 6)]
    quik_markers = [f"Quikpunch:Marker{i}" for i in range(1, 5)]

    b1 = os.path.join(OUT_DIR, "test_belichtungsmesser.json")
    b2 = os.path.join(OUT_DIR, "test_quikpunch.json")
    make_body_json(b1, "Belichtungsmesser", belicht_markers, header_rows, data_rows, shuffle_seed=42)
    make_body_json(b2, "Quikpunch", quik_markers, header_rows, data_rows, shuffle_seed=7)

    out_csv = os.path.join(OUT_DIR, "test_output.csv")

    print(f"Running solver on {INPUT_CSV} -> {out_csv}")
    summary = solver.run(INPUT_CSV, [b1, b2], out_csv,
                         progress_cb=lambda i, n, name: print(f"  [{i}/{n}] {name}"))
    print(json.dumps(summary, indent=2))

    err1 = verify_frame(INPUT_CSV, out_csv, "Belichtungsmesser", belicht_markers)
    err2 = verify_frame(INPUT_CSV, out_csv, "Quikpunch", quik_markers)

    ok = (err1 is not None and err1 < 1.0) and (err2 is not None and err2 < 1.0)
    print("\nRESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
