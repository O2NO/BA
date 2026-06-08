"""Blender addon: rigid body solver for OptiTrack/Motive CSV exports.

Workflow:
1. Scan CSV: pick a Motive CSV; the addon lists its rigid bodies with checkboxes.
2. Click "Create Empties for Checked": one collection per body, with an Arrows
   Origin empty + Sphere marker empties (display size 0.018). Marker positions
   start in a ring; you snap each one onto the matching dot on the 3D scan.
   (Marker identity does not matter - the solver figures out which empty maps
   to which CSV marker by trying all permutations.)
3. Move "Origin" empty to your chosen origin on the scan.
4. (Optional) Parent the scan mesh under Origin as "Scan_<BodyName>".
5. With the collection active, click "Export Rigid Body JSON".
6. "Solve CSV" runs the solver on a chosen CSV using the JSONs.
7. "Visualize CSV" loads a solved CSV and animates a Viewer empty per body
   (and the parented scan mesh) so you can verify alignment visually.
"""

from __future__ import annotations

import json
import os

import bpy
import math

from bpy.props import StringProperty, CollectionProperty, BoolProperty, IntProperty
from bpy.types import Operator, Panel, PropertyGroup
from bpy_extras.io_utils import ImportHelper, ExportHelper


bl_info = {
    "name": "RigidBody Solver (Motive CSV)",
    "author": "BA",
    "version": (0, 1, 0),
    "blender": (3, 0, 0),
    "location": "View3D > N-panel > RB Solver",
    "description": "Solve OptiTrack rigid body poses from a 3D scan + Motive CSV",
    "category": "Animation",
}


from . import solver  # solver.py lives next to this file inside the addon package


ORIGIN_NAME = "Origin"


def _find_origin_in_collection(coll: bpy.types.Collection):
    for obj in coll.objects:
        if obj.type == "EMPTY" and obj.name.split(".")[0] == ORIGIN_NAME:
            return obj
        # Also accept exact match including suffixes from collisions.
        if obj.type == "EMPTY" and obj.name == ORIGIN_NAME:
            return obj
    # Fall back: search by exact name only.
    for obj in coll.objects:
        if obj.type == "EMPTY" and obj.name == ORIGIN_NAME:
            return obj
    return None


def _marker_empties(coll: bpy.types.Collection, origin: bpy.types.Object):
    return [o for o in coll.objects if o.type == "EMPTY" and o is not origin]


def _scan_mesh(coll: bpy.types.Collection, body_name: str):
    target = f"Scan_{body_name}"
    for obj in coll.objects:
        if obj.name == target and obj.type == "MESH":
            return obj
    return None


class RBS_OT_export_body(Operator, ExportHelper):
    """Export the active collection as a rigid body JSON."""
    bl_idname = "rbs.export_body"
    bl_label = "Export Rigid Body JSON"
    filename_ext = ".json"
    filter_glob: StringProperty(default="*.json", options={"HIDDEN"})

    def invoke(self, context, event):
        coll = context.view_layer.active_layer_collection.collection
        if coll and coll.name != "Scene Collection":
            blend_dir = os.path.dirname(bpy.data.filepath) if bpy.data.filepath else os.path.expanduser("~")
            self.filepath = os.path.join(blend_dir, f"{coll.name}.json")
        return super().invoke(context, event)

    def execute(self, context):
        coll = context.view_layer.active_layer_collection.collection
        if not coll or coll.name == "Scene Collection":
            self.report({"ERROR"}, "Active collection must be a rigid body collection (not Scene Collection).")
            return {"CANCELLED"}

        origin = _find_origin_in_collection(coll)
        if origin is None:
            self.report({"ERROR"}, f"No empty named '{ORIGIN_NAME}' in collection '{coll.name}'.")
            return {"CANCELLED"}

        markers = _marker_empties(coll, origin)
        markers = [m for m in markers if not m.name.startswith("Scan_")]
        markers = [m for m in markers if m.type == "EMPTY"]
        if len(markers) < 3:
            self.report({"ERROR"}, f"Need at least 3 marker empties; found {len(markers)}.")
            return {"CANCELLED"}

        # csv_markers list lives as a custom property on the collection (populated
        # by the Scan CSV operator). Fall back to empty names if absent.
        csv_markers = list(coll.get("rbs_csv_markers", []))
        if not csv_markers:
            csv_markers = [m.name for m in markers]
        if len(csv_markers) != len(markers):
            self.report({"WARNING"},
                f"Collection lists {len(csv_markers)} CSV markers but found {len(markers)} "
                "empties; positions will be written but correspondence may be ambiguous.")

        inv = origin.matrix_world.inverted()
        scan_positions = []
        # Stable order: by empty name. Solver permutes anyway.
        for m in sorted(markers, key=lambda o: o.name):
            local = inv @ m.matrix_world.translation
            scan_positions.append([float(local.x), float(local.y), float(local.z)])

        data = {
            "name": coll.name,
            "csv_markers": csv_markers,
            "scan_positions": scan_positions,
        }
        rb_id = coll.get("rbs_rb_id", "")
        if rb_id:
            data["id"] = rb_id
        try:
            with open(self.filepath, "w", encoding="utf-8") as f:
                json.dump(data, f, indent=2)
        except OSError as e:
            self.report({"ERROR"}, f"Write failed: {e}")
            return {"CANCELLED"}

        self.report({"INFO"}, f"Wrote {len(scan_positions)} markers to {self.filepath}")
        return {"FINISHED"}


# ----------------------------------------------------------------------
# Scan CSV: read header, list rigid bodies, create empties for the checked ones
# ----------------------------------------------------------------------


class RBS_ScannedMarker(PropertyGroup):
    name: StringProperty()  # CSV column name, e.g. "Belichtungsmesser:Marker1"


class RBS_ScannedBody(PropertyGroup):
    name: StringProperty()
    rb_id: StringProperty()
    selected: BoolProperty(default=True, name="Include")
    markers: CollectionProperty(type=RBS_ScannedMarker)


class RBS_OT_scan_csv(Operator, ImportHelper):
    """Scan a Motive CSV header and list the rigid bodies it contains."""
    bl_idname = "rbs.scan_csv"
    bl_label = "Scan CSV"
    filename_ext = ".csv"
    filter_glob: StringProperty(default="*.csv", options={"HIDDEN"})

    def execute(self, context):
        try:
            _meta, header_rows, _data = solver.read_motive_csv(self.filepath)
        except Exception as e:
            self.report({"ERROR"}, f"Failed to read CSV: {e}")
            return {"CANCELLED"}

        try:
            bodies = solver.enumerate_rigid_bodies(header_rows)
        except Exception as e:
            self.report({"ERROR"}, f"Failed to parse header: {e}")
            return {"CANCELLED"}

        scene = context.scene
        scene.rbs_scanned_bodies.clear()
        for b in bodies:
            item = scene.rbs_scanned_bodies.add()
            item.name = b["name"]
            item.rb_id = b["id"]
            for m in b["markers"]:
                mi = item.markers.add()
                mi.name = m
        scene.rbs_scanned_csv = self.filepath
        self.report({"INFO"},
                    f"Found {len(bodies)} rigid bodies in {os.path.basename(self.filepath)}")
        return {"FINISHED"}


class RBS_OT_create_empties(Operator):
    """Create Origin + marker empties in collections for each checked rigid body."""
    bl_idname = "rbs.create_empties"
    bl_label = "Create Empties for Checked"

    marker_size: bpy.props.FloatProperty(default=0.018)
    origin_size: bpy.props.FloatProperty(default=0.05)
    ring_radius: bpy.props.FloatProperty(default=0.05)

    def execute(self, context):
        scene = context.scene
        selected = [b for b in scene.rbs_scanned_bodies if b.selected]
        if not selected:
            self.report({"ERROR"}, "No rigid bodies checked.")
            return {"CANCELLED"}

        created = 0
        for body in selected:
            coll = bpy.data.collections.get(body.name)
            if coll is None:
                coll = bpy.data.collections.new(body.name)
                scene.collection.children.link(coll)
            coll["rbs_csv_markers"] = [m.name for m in body.markers]
            if body.rb_id:
                coll["rbs_rb_id"] = body.rb_id

            # Origin empty (single arrow)
            origin = bpy.data.objects.get(f"Origin.{body.name}")
            if origin is None and ORIGIN_NAME not in {o.name for o in coll.objects}:
                origin = bpy.data.objects.new(ORIGIN_NAME, None)
                origin.empty_display_type = "SINGLE_ARROW"
                origin.empty_display_size = self.origin_size
                coll.objects.link(origin)
            elif origin is None:
                # Origin name already exists somewhere; use a unique name.
                origin = bpy.data.objects.new(f"Origin.{body.name}", None)
                origin.empty_display_type = "SINGLE_ARROW"
                origin.empty_display_size = self.origin_size
                coll.objects.link(origin)

            # Marker empties (spheres) arranged in a ring around the origin.
            n = len(body.markers)
            existing_names = {o.name for o in coll.objects}
            for i, m in enumerate(body.markers):
                if m.name in existing_names:
                    continue
                obj = bpy.data.objects.new(m.name, None)
                obj.empty_display_type = "SPHERE"
                obj.empty_display_size = self.marker_size
                angle = 2.0 * math.pi * i / max(1, n)
                obj.location = (self.ring_radius * math.cos(angle),
                                self.ring_radius * math.sin(angle), 0.0)
                obj.parent = origin
                coll.objects.link(obj)
            created += 1

        self.report({"INFO"}, f"Created/updated {created} rigid body collection(s).")
        return {"FINISHED"}


class RBS_BodyJsonItem(PropertyGroup):
    path: StringProperty(name="Path", subtype="FILE_PATH")


class RBS_OT_add_body_json(Operator):
    """Append a rigid body JSON to the solve list."""
    bl_idname = "rbs.add_body_json"
    bl_label = "Add Rigid Body JSON"

    filepath: StringProperty(subtype="FILE_PATH")
    filter_glob: StringProperty(default="*.json", options={"HIDDEN"})

    def invoke(self, context, event):
        context.window_manager.fileselect_add(self)
        return {"RUNNING_MODAL"}

    def execute(self, context):
        item = context.scene.rbs_body_jsons.add()
        item.path = self.filepath
        return {"FINISHED"}


class RBS_OT_remove_body_json(Operator):
    bl_idname = "rbs.remove_body_json"
    bl_label = "Remove"
    index: bpy.props.IntProperty()

    def execute(self, context):
        coll = context.scene.rbs_body_jsons
        if 0 <= self.index < len(coll):
            coll.remove(self.index)
        return {"FINISHED"}


class RBS_OT_clear_body_jsons(Operator):
    bl_idname = "rbs.clear_body_jsons"
    bl_label = "Clear"

    def execute(self, context):
        context.scene.rbs_body_jsons.clear()
        return {"FINISHED"}


class RBS_OT_solve(Operator, ImportHelper):
    """Run the solver: pick the input Motive CSV; uses the JSON list."""
    bl_idname = "rbs.solve"
    bl_label = "Solve CSV"
    filename_ext = ".csv"
    filter_glob: StringProperty(default="*.csv", options={"HIDDEN"})

    def execute(self, context):
        input_csv = self.filepath
        body_paths = [item.path for item in context.scene.rbs_body_jsons if item.path]
        if not body_paths:
            self.report({"ERROR"}, "Add at least one rigid body JSON first.")
            return {"CANCELLED"}

        out_dir = os.path.dirname(input_csv)
        base = os.path.splitext(os.path.basename(input_csv))[0]
        output_csv = os.path.join(out_dir, f"{base}_solved.csv")

        wm = context.window_manager
        wm.progress_begin(0, max(1, len(body_paths)))
        try:
            def cb(i, n, name):
                wm.progress_update(i)
            summary = solver.run(input_csv, body_paths, output_csv, progress_cb=cb)
        except Exception as e:
            wm.progress_end()
            self.report({"ERROR"}, f"Solve failed: {e}")
            return {"CANCELLED"}
        wm.progress_end()

        context.scene.rbs_last_output = output_csv
        msg = " | ".join(
            f"{b['name']}: {b['solved_frames']}/{b['total_frames']} frames, "
            f"mean error={b['mean_residual_mm']:.2f}mm, "
            f"max error={b['max_residual_mm']:.2f}mm"
            for b in summary["bodies"])
        self.report({"INFO"}, f"Wrote {output_csv}. {msg}")
        return {"FINISHED"}


class RBS_OT_visualize(Operator, ImportHelper):
    """Load a solved CSV and keyframe a viewer empty + parented scan mesh per body."""
    bl_idname = "rbs.visualize"
    bl_label = "Visualize Solved CSV"
    filename_ext = ".csv"
    filter_glob: StringProperty(default="*.csv", options={"HIDDEN"})

    def invoke(self, context, event):
        if context.scene.rbs_last_output:
            self.filepath = context.scene.rbs_last_output
        return super().invoke(context, event)

    def execute(self, context):
        try:
            metadata, header_rows, data_rows = solver.read_motive_csv(self.filepath)
        except Exception as e:
            self.report({"ERROR"}, f"Failed to read CSV: {e}")
            return {"CANCELLED"}

        type_row = header_rows[2]
        name_row = header_rows[3]
        datatype_row = header_rows[5]
        axis_row = header_rows[6]
        max_cols = max(len(type_row), len(name_row), len(datatype_row), len(axis_row))

        def cell(row, idx):
            return row[idx].strip() if idx < len(row) else ""

        # Find rigid body rotation/position columns per body name.
        bodies: dict[str, dict] = {}
        for c in range(max_cols):
            if cell(type_row, c) != "Rigid Body":
                continue
            name = cell(name_row, c)
            dtype = cell(datatype_row, c)
            axis = cell(axis_row, c)
            if not name:
                continue
            b = bodies.setdefault(name, {"rot": {}, "pos": {}})
            if dtype == "Rotation" and axis in ("X", "Y", "Z", "W"):
                b["rot"][axis] = c
            elif dtype == "Position" and axis in ("X", "Y", "Z"):
                b["pos"][axis] = c

        usable = {n: b for n, b in bodies.items()
                  if {"X", "Y", "Z", "W"}.issubset(b["rot"]) and {"X", "Y", "Z"}.issubset(b["pos"])}
        if not usable:
            self.report({"ERROR"}, "No rigid bodies with rotation+position columns found in CSV.")
            return {"CANCELLED"}

        # Motive CSVs declare their length unit; Blender works in meters.
        unit = metadata.get("Length Units", "Millimeters").strip().lower()
        unit_to_m = {"millimeters": 1e-3, "centimeters": 1e-2, "meters": 1.0}.get(unit, 1e-3)

        scene = context.scene
        frame_numbers: list[int] = []

        for name, cols in usable.items():
            viewer_name = f"Viewer_{name}"
            viewer = bpy.data.objects.get(viewer_name)
            if viewer is None:
                viewer = bpy.data.objects.new(viewer_name, None)
                viewer.empty_display_type = "SINGLE_ARROW"
                viewer.empty_display_size = 0.1
                scene.collection.objects.link(viewer)
            viewer.rotation_mode = "QUATERNION"
            viewer.animation_data_clear()

            # Parent scan mesh if a Scan_<name> object exists in the scene.
            scan_obj = bpy.data.objects.get(f"Scan_{name}")
            if scan_obj is not None and scan_obj.parent is not viewer:
                # Keep transform=True bakes the current world transform into matrix_parent_inverse.
                scan_obj.parent = viewer
                scan_obj.matrix_parent_inverse = viewer.matrix_world.inverted() @ scan_obj.matrix_world

            rx, ry, rz, rw = cols["rot"]["X"], cols["rot"]["Y"], cols["rot"]["Z"], cols["rot"]["W"]
            px, py, pz = cols["pos"]["X"], cols["pos"]["Y"], cols["pos"]["Z"]

            for row in data_rows:
                if len(row) < 2:
                    continue
                try:
                    frame = int(float(row[0]))
                except ValueError:
                    continue

                def get(col):
                    if col >= len(row):
                        return ""
                    return row[col].strip()

                rot_cells = [get(rx), get(ry), get(rz), get(rw)]
                pos_cells = [get(px), get(py), get(pz)]
                if any(not c for c in rot_cells + pos_cells):
                    continue
                try:
                    qx = float(rot_cells[0]); qy = float(rot_cells[1])
                    qz = float(rot_cells[2]); qw = float(rot_cells[3])
                    tx = float(pos_cells[0]); ty = float(pos_cells[1]); tz = float(pos_cells[2])
                except ValueError:
                    continue

                # Blender quaternion order is (w, x, y, z); Motive CSV is (x, y, z, w).
                viewer.rotation_quaternion = (qw, qx, qy, qz)
                viewer.location = (tx * unit_to_m, ty * unit_to_m, tz * unit_to_m)
                viewer.keyframe_insert(data_path="location", frame=frame)
                viewer.keyframe_insert(data_path="rotation_quaternion", frame=frame)
                frame_numbers.append(frame)

        if frame_numbers:
            scene.frame_start = min(frame_numbers)
            scene.frame_end = max(frame_numbers)

        self.report({"INFO"}, f"Visualized {len(usable)} rigid bodies from {self.filepath}")
        return {"FINISHED"}


class RBS_PT_main(Panel):
    bl_label = "RigidBody Solver"
    bl_idname = "RBS_PT_main"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "RB Solver"

    def draw(self, context):
        layout = self.layout
        scene = context.scene

        # 1. Scan CSV
        box = layout.box()
        box.label(text="Scan CSV", icon="FILE_TEXT")
        if scene.rbs_scanned_csv:
            box.label(text=os.path.basename(scene.rbs_scanned_csv), icon="FILE")
        box.operator("rbs.scan_csv", icon="VIEWZOOM")
        if len(scene.rbs_scanned_bodies):
            for body in scene.rbs_scanned_bodies:
                row = box.row(align=True)
                row.prop(body, "selected", text="")
                row.label(text=f"{body.name}  ({len(body.markers)} markers)")
            box.operator("rbs.create_empties", icon="EMPTY_DATA")

        # 2. Define rigid body
        coll = context.view_layer.active_layer_collection.collection
        box = layout.box()
        box.label(text="Define rigid body", icon="EMPTY_AXIS")
        box.label(text=f"Active collection: {coll.name if coll else '-'}")
        box.operator("rbs.export_body")

        box = layout.box()
        box.label(text="Solve", icon="PLAY")
        for i, item in enumerate(scene.rbs_body_jsons):
            row = box.row(align=True)
            row.label(text=os.path.basename(item.path) or item.path, icon="FILE")
            op = row.operator("rbs.remove_body_json", text="", icon="X")
            op.index = i
        row = box.row(align=True)
        row.operator("rbs.add_body_json", text="Add JSON", icon="ADD")
        row.operator("rbs.clear_body_jsons", text="", icon="TRASH")
        box.operator("rbs.solve", icon="PLAY")

        box = layout.box()
        box.label(text="Visualize", icon="HIDE_OFF")
        if scene.rbs_last_output:
            box.label(text=os.path.basename(scene.rbs_last_output), icon="FILE")
        box.operator("rbs.visualize")


_classes = (
    RBS_ScannedMarker,
    RBS_ScannedBody,
    RBS_BodyJsonItem,
    RBS_OT_scan_csv,
    RBS_OT_create_empties,
    RBS_OT_export_body,
    RBS_OT_add_body_json,
    RBS_OT_remove_body_json,
    RBS_OT_clear_body_jsons,
    RBS_OT_solve,
    RBS_OT_visualize,
    RBS_PT_main,
)


def register():
    for c in _classes:
        bpy.utils.register_class(c)
    bpy.types.Scene.rbs_body_jsons = CollectionProperty(type=RBS_BodyJsonItem)
    bpy.types.Scene.rbs_last_output = StringProperty(name="Last solved CSV", subtype="FILE_PATH")
    bpy.types.Scene.rbs_scanned_bodies = CollectionProperty(type=RBS_ScannedBody)
    bpy.types.Scene.rbs_scanned_csv = StringProperty(name="Scanned CSV", subtype="FILE_PATH")


def unregister():
    del bpy.types.Scene.rbs_body_jsons
    del bpy.types.Scene.rbs_last_output
    del bpy.types.Scene.rbs_scanned_bodies
    del bpy.types.Scene.rbs_scanned_csv
    for c in reversed(_classes):
        bpy.utils.unregister_class(c)


if __name__ == "__main__":
    register()
