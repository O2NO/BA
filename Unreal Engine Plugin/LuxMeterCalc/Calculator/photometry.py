"""
Photometric math: sample candela from an IESPhotometry record at a given direction,
then compute illuminance contribution at a meter using the inverse square law and
Lambert's cosine law.

UE input convention:
- Coordinates in cm, left-handed, +Z up.
- Light's Forward (=actor +X) is the IES nadir (vertical angle gamma = 0).
- Light's Up provides the azimuth reference for horizontal angle C.
- Meter's Forward is the surface normal of the sensor.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

from ies_parser import IESPhotometry

Vec3 = tuple[float, float, float]


# ---------- vector helpers (kept local — no numpy dep) ----------

def _add(a: Vec3, b: Vec3) -> Vec3:    return (a[0]+b[0], a[1]+b[1], a[2]+b[2])
def _sub(a: Vec3, b: Vec3) -> Vec3:    return (a[0]-b[0], a[1]-b[1], a[2]-b[2])
def _scl(a: Vec3, s: float) -> Vec3:   return (a[0]*s,    a[1]*s,    a[2]*s)
def _dot(a: Vec3, b: Vec3) -> float:   return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]
def _cross(a: Vec3, b: Vec3) -> Vec3:
    return (a[1]*b[2] - a[2]*b[1],
            a[2]*b[0] - a[0]*b[2],
            a[0]*b[1] - a[1]*b[0])
def _len(a: Vec3) -> float:            return math.sqrt(_dot(a, a))
def _norm(a: Vec3) -> Vec3:
    n = _len(a)
    if n < 1e-12:
        raise ValueError("Cannot normalize zero-length vector")
    return _scl(a, 1.0 / n)


# ---------- IES sampling ----------

def _interp_1d(xs: list[float], ys: list[float], x: float) -> float:
    """Linear interpolation, clamped at the endpoints."""
    if x <= xs[0]:
        return ys[0]
    if x >= xs[-1]:
        return ys[-1]
    # binary search would be nicer; xs are short so linear scan is fine.
    for i in range(len(xs) - 1):
        if xs[i] <= x <= xs[i + 1]:
            t = (x - xs[i]) / (xs[i + 1] - xs[i])
            return ys[i] * (1 - t) + ys[i + 1] * t
    return ys[-1]


def sample_candela(ies: IESPhotometry, gamma_deg: float, c_deg: float) -> float:
    """Bilinear sample of candela at (gamma, C). Handles symmetric IES files
    (single horizontal slice, half-plane, or quarter-plane symmetry)."""
    v_angles = ies.vertical_angles
    h_angles = ies.horizontal_angles

    # Wrap horizontal angle.
    c = c_deg % 360.0

    # Symmetry handling per LM-63:
    #   - 1 horizontal angle           -> rotationally symmetric
    #   - max horizontal angle == 0    -> rotationally symmetric (single value at 0)
    #   - max horizontal angle == 90   -> quadrant symmetry; mirror C into [0, 90]
    #   - max horizontal angle == 180  -> bilateral symmetry; mirror C into [0, 180]
    #   - max horizontal angle == 360  -> full distribution
    if len(h_angles) == 1 or h_angles[-1] == 0.0:
        # Rotationally symmetric.
        col = [ies.candela[v][0] for v in range(len(v_angles))]
        return _interp_1d(v_angles, col, gamma_deg)

    h_max = h_angles[-1]
    if h_max <= 90.0:
        c = abs(((c + 90.0) % 180.0) - 90.0)        # fold into [0, 90]
    elif h_max <= 180.0:
        if c > 180.0:
            c = 360.0 - c                            # fold into [0, 180]
    # else h_max == 360: leave c as-is.

    # Bilinear interpolation across the 2D grid.
    # Find h bracket.
    if c <= h_angles[0]:
        h_lo, h_hi, t_h = 0, 0, 0.0
    elif c >= h_angles[-1]:
        h_lo = h_hi = len(h_angles) - 1
        t_h = 0.0
    else:
        h_lo = 0
        while h_lo + 1 < len(h_angles) and h_angles[h_lo + 1] < c:
            h_lo += 1
        h_hi = h_lo + 1
        span = h_angles[h_hi] - h_angles[h_lo]
        t_h = (c - h_angles[h_lo]) / span if span > 0 else 0.0

    col_lo = [ies.candela[v][h_lo] for v in range(len(v_angles))]
    col_hi = [ies.candela[v][h_hi] for v in range(len(v_angles))]
    cd_lo = _interp_1d(v_angles, col_lo, gamma_deg)
    cd_hi = _interp_1d(v_angles, col_hi, gamma_deg)
    return cd_lo * (1 - t_h) + cd_hi * t_h


# ---------- main illuminance computation ----------

@dataclass
class LightDebug:
    display_name: str
    distance_m: float
    gamma_deg: float
    c_deg: float
    candela: float
    cosine_to_meter: float
    contribution_lux: float


def compute_light_contribution(
    *,
    display_name: str,
    light_pos_m: Vec3,
    light_forward: Vec3,
    light_up: Vec3,
    meter_pos_m: Vec3,
    meter_forward: Vec3,
    intensity_lumens: float,
    ies: IESPhotometry,
) -> LightDebug:
    """Compute illuminance contribution at the meter from a single IES light.

    Inputs are already in meters. Returns a LightDebug with all intermediate values.
    """
    delta = _sub(meter_pos_m, light_pos_m)
    distance = _len(delta)
    if distance < 1e-6:
        return LightDebug(display_name, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0)

    dir_world = _scl(delta, 1.0 / distance)              # light -> meter, unit
    nadir = _norm(light_forward)                          # IES gamma = 0 axis

    # Build orthonormal frame (nadir, right, up_ref).
    # Use the user-supplied up as the azimuth reference; re-orthogonalize.
    up_in = _norm(light_up)
    right = _cross(up_in, nadir)
    if _len(right) < 1e-6:
        # Up is parallel to forward; pick an arbitrary perpendicular.
        fallback = (1.0, 0.0, 0.0) if abs(nadir[0]) < 0.9 else (0.0, 1.0, 0.0)
        right = _norm(_cross(fallback, nadir))
    else:
        right = _norm(right)
    up_ref = _cross(nadir, right)

    cos_gamma = max(-1.0, min(1.0, _dot(dir_world, nadir)))
    gamma_deg = math.degrees(math.acos(cos_gamma))

    proj = _sub(dir_world, _scl(nadir, cos_gamma))
    px = _dot(proj, right)
    py = _dot(proj, up_ref)
    if abs(px) < 1e-9 and abs(py) < 1e-9:
        c_deg = 0.0
    else:
        c_deg = math.degrees(math.atan2(py, px)) % 360.0

    candela = sample_candela(ies, gamma_deg, c_deg)

    # Optional lumens scaling: the IES file's rated lumens are a property of the
    # luminaire when measured. If the user has set a different IntensityLumens on
    # the actor, scale candela proportionally. IntensityLumens=0 means "use IES as-is".
    if intensity_lumens > 0 and ies.rated_lumens and ies.rated_lumens > 0:
        candela *= intensity_lumens / ies.rated_lumens

    # Lambert at the meter. dir_meter_to_light = -dir_world.
    meter_n = _norm(meter_forward)
    cos_theta = max(0.0, -_dot(meter_n, dir_world))
    contribution = candela * cos_theta / (distance * distance)

    return LightDebug(
        display_name=display_name,
        distance_m=distance,
        gamma_deg=gamma_deg,
        c_deg=c_deg,
        candela=candela,
        cosine_to_meter=cos_theta,
        contribution_lux=contribution,
    )


# ---------- Lambertian reflector (single-bounce, single reflector) ----------

# A patch's outgoing luminance L is independent of the meter, so for batch use
# we build the patch list + luminance once per request and then integrate
# against many meter poses.
ReflectorPatch = tuple[Vec3, Vec3, float]  # (position_m, normal, luminance)


def _build_reflector_frame(
    reflector_forward: Vec3, reflector_up: Vec3
) -> tuple[Vec3, Vec3, Vec3]:
    """Return orthonormal (normal, u_axis, v_axis) for the reflector's plane."""
    n = _norm(reflector_forward)
    v_in = _norm(reflector_up)
    u_axis = _cross(v_in, n)
    if _len(u_axis) < 1e-6:
        # up parallel to normal — pick arbitrary perpendicular
        fallback = (1.0, 0.0, 0.0) if abs(n[0]) < 0.9 else (0.0, 1.0, 0.0)
        u_axis = _cross(fallback, n)
    u_axis = _norm(u_axis)
    v_axis = _cross(n, u_axis)
    return n, u_axis, v_axis


def build_reflector_patches(
    *,
    reflector_pos_m: Vec3,
    reflector_forward: Vec3,
    reflector_up: Vec3,
    width_m: float,
    height_m: float,
    albedo: float,
    fixtures: list[tuple[Vec3, Vec3, Vec3, float, "IESPhotometry"]],
) -> list[ReflectorPatch]:
    """Sample the reflector on a 1 cm x 1 cm grid and compute each patch's
    outgoing Lambertian luminance from the supplied fixture set.

    Each fixture entry is (light_pos_m, light_forward, light_up, intensity_lumens, ies).
    """
    if albedo <= 0.0 or width_m <= 0.0 or height_m <= 0.0:
        return []

    n, u_axis, v_axis = _build_reflector_frame(reflector_forward, reflector_up)

    # 1 cm spacing — match the user's "1 ray per cm" intent.
    nu = max(1, int(round(width_m * 100.0)))
    nv = max(1, int(round(height_m * 100.0)))
    du = width_m / nu
    dv = height_m / nv
    patch_area_m2 = du * dv

    # Half-extents so the grid is centred on the reflector origin.
    u0 = -0.5 * width_m + 0.5 * du
    v0 = -0.5 * height_m + 0.5 * dv

    inv_pi = 1.0 / math.pi
    patches: list[ReflectorPatch] = []

    for j in range(nv):
        dv_j = v0 + j * dv
        for i in range(nu):
            du_i = u0 + i * du
            # Patch position in world (metres)
            px = reflector_pos_m[0] + du_i * u_axis[0] + dv_j * v_axis[0]
            py = reflector_pos_m[1] + du_i * u_axis[1] + dv_j * v_axis[1]
            pz = reflector_pos_m[2] + du_i * u_axis[2] + dv_j * v_axis[2]
            p = (px, py, pz)

            # Accumulate incident illuminance at this patch from each fixture.
            incident = 0.0
            for (light_pos, light_fwd, light_up, intensity_lumens, ies) in fixtures:
                delta = _sub(p, light_pos)
                d2 = _dot(delta, delta)
                if d2 < 1e-6:
                    continue
                d = math.sqrt(d2)
                dir_world = _scl(delta, 1.0 / d)

                # γ, C in light's local frame (mirrors compute_light_contribution).
                nadir = _norm(light_fwd)
                up_in = _norm(light_up)
                right = _cross(up_in, nadir)
                if _len(right) < 1e-6:
                    fallback = (1.0, 0.0, 0.0) if abs(nadir[0]) < 0.9 else (0.0, 1.0, 0.0)
                    right = _norm(_cross(fallback, nadir))
                else:
                    right = _norm(right)
                up_ref = _cross(nadir, right)

                cos_gamma = max(-1.0, min(1.0, _dot(dir_world, nadir)))
                gamma_deg = math.degrees(math.acos(cos_gamma))

                proj = _sub(dir_world, _scl(nadir, cos_gamma))
                px_ = _dot(proj, right)
                py_ = _dot(proj, up_ref)
                if abs(px_) < 1e-9 and abs(py_) < 1e-9:
                    c_deg = 0.0
                else:
                    c_deg = math.degrees(math.atan2(py_, px_)) % 360.0

                cd = sample_candela(ies, gamma_deg, c_deg)
                if intensity_lumens > 0 and ies.rated_lumens and ies.rated_lumens > 0:
                    cd *= intensity_lumens / ies.rated_lumens

                # Patch is hit on the side facing the fixture. cos_θ relative to
                # the patch normal n. dir_world points light→patch, so the patch
                # receives light from −dir_world.
                cos_in = -_dot(n, dir_world)
                if cos_in <= 0.0:
                    continue
                incident += cd * cos_in / d2

            # Lambertian outgoing luminance, weighted by patch area so the
            # per-meter loop can multiply by cos·cos / d² directly.
            l_p = albedo * inv_pi * incident * patch_area_m2
            patches.append((p, n, l_p))

    return patches


def reflector_lux_at_meter(
    patches: list[ReflectorPatch],
    meter_pos_m: Vec3,
    meter_forward: Vec3,
) -> float:
    """Integrate the reflector patches against a single meter pose."""
    if not patches:
        return 0.0
    meter_n = _norm(meter_forward)
    total = 0.0
    for (p, n_patch, l_scaled) in patches:
        delta = _sub(p, meter_pos_m)
        d2 = _dot(delta, delta)
        if d2 < 1e-6:
            continue
        d = math.sqrt(d2)
        dir_meter_to_patch = _scl(delta, 1.0 / d)

        cos_meter = _dot(meter_n, dir_meter_to_patch)
        if cos_meter <= 0.0:
            continue
        cos_patch = -_dot(n_patch, dir_meter_to_patch)
        if cos_patch <= 0.0:
            continue
        # l_scaled already includes albedo/π and patch area.
        total += l_scaled * cos_meter * cos_patch / d2
    return total
