"""Convert a single Optitrack quaternion to UE space, mirroring exactly what
OptitrackToUE::RotationToUE does in the plugin.

Usage:
    python convert_quat.py <qx> <qy> <qz> <qw>

The four arguments are the Motive-default :RX :RY :RZ :RW columns from
track.csv for one frame, in that order.

Conversion (matches Plugins/LuxMeterCalc/Source/LuxMeterCalc/Private/OptitrackToUE.h):

    Direct basis swap, Motive (Y-up, right-handed) -> UE (Z-up, left-handed).

        UE.X =  Motive.Z       (forward)
        UE.Y = -Motive.X       (right; negated for handedness)
        UE.Z =  Motive.Y       (up)

    Quaternion follows the same axis permutation, with W negated to compensate
    for the handedness flip:

        (x, y, z, w)_UE = (Motive.z, -Motive.x, Motive.y, -Motive.w)

    Result is normalised on the way out.
"""

from __future__ import annotations

import math
import sys


Quat = tuple[float, float, float, float]  # (qx, qy, qz, qw)


def quat_mul(a: Quat, b: Quat) -> Quat:
    """Hamilton product. Same convention as UE: (A*B) applies B first, then A."""
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz,
    )


def normalize(q: Quat) -> Quat:
    qx, qy, qz, qw = q
    n = math.sqrt(qx * qx + qy * qy + qz * qz + qw * qw)
    if n < 1e-12:
        return (0.0, 0.0, 0.0, 1.0)
    return (qx / n, qy / n, qz / n, qw / n)


def quat_to_ue_rotator(q: Quat) -> tuple[float, float, float]:
    """Convert quaternion to UE-style FRotator (Pitch, Yaw, Roll) in degrees.

    Matches FQuat::Rotator() exactly. UE flips the sign convention on pitch
    (Z*X - W*Y) and roll relative to a textbook right-handed Z-up Euler — using
    the textbook formula here would yield pitch/roll values with the opposite
    sign of what the editor's Details panel shows.
    """
    qx, qy, qz, qw = q
    sing = qz * qx - qw * qy
    yaw_y = 2.0 * (qw * qz + qx * qy)
    yaw_x = 1.0 - 2.0 * (qy * qy + qz * qz)
    SING_THRESH = 0.4999995

    yaw = math.degrees(math.atan2(yaw_y, yaw_x))
    two_atan_xw_deg = 2.0 * math.degrees(math.atan2(qx, qw))

    if sing < -SING_THRESH:
        pitch = -90.0
        roll = _normalize_axis(-yaw - two_atan_xw_deg)
    elif sing > SING_THRESH:
        pitch = 90.0
        roll = _normalize_axis(yaw - two_atan_xw_deg)
    else:
        pitch = math.degrees(math.asin(max(-1.0, min(1.0, 2.0 * sing))))
        roll = math.degrees(math.atan2(
            -2.0 * (qw * qx + qy * qz),
            1.0 - 2.0 * (qx * qx + qy * qy)))
    return pitch, yaw, roll


def _normalize_axis(angle_deg: float) -> float:
    """Mimic FRotator::NormalizeAxis: wrap into (-180, 180]."""
    a = math.fmod(angle_deg, 360.0)
    if a > 180.0:
        a -= 360.0
    elif a <= -180.0:
        a += 360.0
    return a


def optitrack_to_ue(qx: float, qy: float, qz: float, qw: float) -> dict:
    """Apply the Motive -> UE basis swap and return the result + Euler."""
    # (x, y, z, w)_UE = (Motive.z, -Motive.x, Motive.y, -Motive.w)
    final = normalize((qz, -qx, qy, -qw))
    pitch, yaw, roll = quat_to_ue_rotator(final)
    return {
        "input":      (qx, qy, qz, qw),
        "ue_quat":    final,
        "ue_rotator": (pitch, yaw, roll),
    }


def _fmt_quat(q: Quat) -> str:
    return f"qx={q[0]: .6f}  qy={q[1]: .6f}  qz={q[2]: .6f}  qw={q[3]: .6f}"


def main() -> int:
    if len(sys.argv) != 5:
        print("Usage:  python convert_quat.py <qx> <qy> <qz> <qw>")
        print("Example (identity):  python convert_quat.py 0 0 0 1")
        return 1
    try:
        qx, qy, qz, qw = (float(a) for a in sys.argv[1:5])
    except ValueError:
        print("All four arguments must be numbers.")
        return 1

    r = optitrack_to_ue(qx, qy, qz, qw)

    print("Input (Optitrack RX,RY,RZ,RW):")
    print(f"  {_fmt_quat(r['input'])}")
    print()
    print("UE quaternion  (x,y,z,w) = (Motive.z, -Motive.x, Motive.y, -Motive.w):")
    print(f"  {_fmt_quat(r['ue_quat'])}")
    print()
    pitch, yaw, roll = r["ue_rotator"]
    print("Same rotation as a UE FRotator (degrees):")
    print(f"  roll - Rollen (X) = {roll: 8.3f}    yaw - Gieren (Z) = {yaw: 8.3f}    pitch - Nicken (Y) = {pitch: 8.3f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
