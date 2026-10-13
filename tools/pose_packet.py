#!/usr/bin/env python3
"""Byte reference for docs/pose-protocol.md. Pack, unpack, and map types 1 and 2."""

import math
import socket
import struct
import subprocess
import sys
from pathlib import Path

MAGIC = b"WDRN"
VERSION = 1
TYPE_POSE = 1
TYPE_CONTACT = 2
POSE_SIZE = 56
CONTACT_STRIDE = 28
MAX_CONTACTS = 4

_POSE = struct.Struct("<4sBBHII10f")
_CONTACT_HDR = struct.Struct("<4sBBBB")
_CONTACT = struct.Struct("<6fI")

# Shared with bridge/PoseBridge/LayoutCheck.cs and tools/test_pose_map.cpp.
FIXTURE_SEQ = 7
FIXTURE_TIME_MS = 1500
FIXTURE_POS = (1.0, 2.0, 3.0)
FIXTURE_ROT = (0.0, 0.0, 0.0, 1.0)
FIXTURE_VEL = (0.5, 0.0, -0.25)
FIXTURE_CONTACT = (1.0, 2.0, 3.0, 0.0, 1.0, 0.0, 1)
_Y90 = math.sin(math.pi / 4.0)
FIXTURE_ROT_Y90 = (0.0, _Y90, 0.0, _Y90)


def pack_pose(seq, time_ms, pos, rot, vel):
    px, py, pz = pos
    qx, qy, qz, qw = rot
    vx, vy, vz = vel
    return _POSE.pack(
        MAGIC, VERSION, TYPE_POSE, 0, seq & 0xFFFFFFFF, time_ms & 0xFFFFFFFF,
        px, py, pz, qx, qy, qz, qw, vx, vy, vz,
    )


def unpack_pose(data):
    if len(data) < POSE_SIZE:
        return None
    magic, ver, typ, reserved, seq, time_ms, px, py, pz, qx, qy, qz, qw, vx, vy, vz = _POSE.unpack_from(data)
    if magic != MAGIC or ver != VERSION or typ != TYPE_POSE or reserved != 0:
        return None
    return {
        "sequence": seq,
        "time_ms": time_ms,
        "pos": (px, py, pz),
        "rot": (qx, qy, qz, qw),
        "vel": (vx, vy, vz),
    }


def pack_contacts(contacts):
    if len(contacts) > MAX_CONTACTS:
        raise ValueError("at most 4 contacts")
    buf = bytearray(_CONTACT_HDR.pack(MAGIC, VERSION, TYPE_CONTACT, len(contacts), 0))
    for px, py, pz, nx, ny, nz, flags in contacts:
        buf += _CONTACT.pack(px, py, pz, nx, ny, nz, flags & 0xFFFFFFFF)
    return bytes(buf)


def unpack_contacts(data):
    if len(data) < _CONTACT_HDR.size:
        return None
    magic, ver, typ, count, reserved = _CONTACT_HDR.unpack_from(data)
    if magic != MAGIC or ver != VERSION or typ != TYPE_CONTACT or reserved != 0:
        return None
    if count > MAX_CONTACTS:
        return None
    need = _CONTACT_HDR.size + count * CONTACT_STRIDE
    if len(data) < need:
        return None
    out = []
    off = _CONTACT_HDR.size
    for _ in range(count):
        out.append(_CONTACT.unpack_from(data, off))
        off += CONTACT_STRIDE
    return out


def unity_yaw(rot):
    qx, qy, qz, qw = rot
    siny = 2.0 * (qw * qy + qz * qx)
    cosy = 1.0 - 2.0 * (qx * qx + qy * qy)
    return math.atan2(siny, cosy)


def map_unity_to_wow(pos, rot, scale):
    """MapUnityToWow. Hypothesis in docs/pose-protocol.md, not yet confirmed on the guest."""
    ux, uy, uz = pos
    yaw = -unity_yaw(rot) % math.tau
    return (uz * scale, -ux * scale, uy * scale, yaw)


def map_wow_to_unity(pos, scale):
    x, y, z = pos
    s = scale if scale != 0.0 else 1.0
    return (-y / s, z / s, x / s)


def fixture_pose_bytes():
    return pack_pose(FIXTURE_SEQ, FIXTURE_TIME_MS, FIXTURE_POS, FIXTURE_ROT, FIXTURE_VEL)


def fixture_contact_bytes():
    return pack_contacts([FIXTURE_CONTACT])


def format_pose(pose):
    px, py, pz = pose["pos"]
    vx, vy, vz = pose["vel"]
    return (
        f"seq={pose['sequence']} t={pose['time_ms']} "
        f"pos=({px:.3f},{py:.3f},{pz:.3f}) "
        f"vel=({vx:.3f},{vy:.3f},{vz:.3f})"
    )


def _close(a, b, eps=1e-5):
    return abs(a - b) <= eps


def self_test():
    raw = fixture_pose_bytes()
    assert len(raw) == POSE_SIZE
    got = unpack_pose(raw)
    assert got["sequence"] == FIXTURE_SEQ
    assert got["time_ms"] == FIXTURE_TIME_MS
    assert got["pos"] == FIXTURE_POS
    assert got["rot"] == FIXTURE_ROT
    assert got["vel"] == FIXTURE_VEL
    assert unpack_pose(raw[:10]) is None
    assert unpack_pose(b"\x00" * POSE_SIZE) is None

    wow = map_unity_to_wow(FIXTURE_POS, FIXTURE_ROT, 1.0)
    assert _close(wow[0], 3.0) and _close(wow[1], -1.0) and _close(wow[2], 2.0) and _close(wow[3], 0.0)
    back = map_wow_to_unity(wow[:3], 1.0)
    assert _close(back[0], 1.0) and _close(back[1], 2.0) and _close(back[2], 3.0)

    yaw90 = map_unity_to_wow(FIXTURE_POS, FIXTURE_ROT_Y90, 2.0)
    assert _close(yaw90[0], 6.0) and _close(yaw90[1], -2.0) and _close(yaw90[2], 4.0)
    assert _close(yaw90[3], 1.5 * math.pi)

    packed = pack_contacts([FIXTURE_CONTACT])
    assert len(packed) == 8 + CONTACT_STRIDE
    contacts = unpack_contacts(packed)
    assert contacts == [FIXTURE_CONTACT]
    assert unpack_contacts(pack_contacts([])) == []
    assert unpack_contacts(raw) is None
    assert unpack_pose(packed) is None


def _check_block(text, label, expected_hex):
    prefix = label + " "
    for line in text.splitlines():
        if line.startswith(prefix):
            got = line[len(prefix):].strip()
            if got != expected_hex:
                raise SystemExit(f"{label} bytes mismatch:\n  impl {got}\n  py   {expected_hex}")
            return
    raise SystemExit(f"{label} line missing")


def check_impl(cmd, name, need_contact=True):
    try:
        out = subprocess.check_output(cmd, text=True)
    except (OSError, subprocess.CalledProcessError) as exc:
        raise SystemExit(f"{name} failed: {exc}") from exc
    _check_block(out, "pose", fixture_pose_bytes().hex())
    if need_contact:
        _check_block(out, "contact", fixture_contact_bytes().hex())


def check_built_impls():
    root = Path(__file__).resolve().parents[1]
    native = root / "tools" / "test_pose_map"
    if native.exists():
        check_impl([str(native)], "server map test")
    layout = root / "bridge" / "PoseBridge" / "bin" / "LayoutCheck.exe"
    if layout.exists():
        cmd = [str(layout)] if sys.platform == "win32" else ["mono", str(layout)]
        check_impl(cmd, "plugin layout test")
    client = root / "client" / "test_pose"
    if client.exists():
        check_impl([str(client)], "client map test", need_contact=False)


def send_fixture(host, port):
    payload = fixture_pose_bytes()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.sendto(payload, (host, port))
    finally:
        sock.close()
    wow = map_unity_to_wow(FIXTURE_POS, FIXTURE_ROT, 1.0)
    print(
        f"sent {len(payload)} bytes to {host}:{port} "
        f"seq={FIXTURE_SEQ} wow=({wow[0]:.3f},{wow[1]:.3f},{wow[2]:.3f}) yaw={wow[3]:.3f}",
        flush=True,
    )


def main(argv):
    if len(argv) >= 2 and argv[1] == "--send":
        host = argv[2] if len(argv) >= 3 else "127.0.0.1"
        port = int(argv[3]) if len(argv) >= 4 else 47561
        send_fixture(host, port)
        return 0
    self_test()
    check_built_impls()
    print("pose_packet ok", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
