# Pose protocol

Version 1. Little-endian. UDP. One datagram, one message.

The plugin sends Unity world space. Mapping into WoW yards lives in one shared function, not in the plugin. `tools/pose_packet.py` is the byte reference; the plugin and the server include a test that matches it.

## Pose (sim → server), type 1

| Offset | Type | Field |
|---:|---|---|
| 0 | 4 bytes | magic `WDRN` |
| 4 | u8 | version `1` |
| 5 | u8 | type `1` |
| 6 | u16 | reserved `0` |
| 8 | u32 | sequence, increments by 1 |
| 12 | u32 | milliseconds, from Unity `Time.fixedTime` scaled |
| 16 | 3×f32 | position xyz, metres, Unity world (Y up) |
| 28 | 4×f32 | rotation xyzw |
| 44 | 3×f32 | linear velocity xyz, metres/second, Unity world |

56 bytes. Default destination port **47561**. The plugin config key `RemoteHost` defaults to `127.0.0.1` and `RemotePort` to `47561`. Point `RemoteHost` at the machine running worldserver when that is not this computer. The camera copy always stays on `127.0.0.1:47562`.

Send every `FixedUpdate` of the quad. Log measured rate once a second. Drop nothing to "save bandwidth".

Find the body with `VDroneCtrl` plus `GetComponent<Rigidbody>()` on that object. Do not read private fields and do not patch `BetaFlight`.

## Map (server and client)

One function, `MapUnityToWow(pos, rot, scale) -> (x, y, z, yaw)`.

Fixture check, 2026-10-12: worldserver logged `unity=(1.000, 2.000, 3.000)` as `wow=(3.000, -1.000, 2.000) yaw=-0.000` at scale `1.0`. That matches this map. A measured length in the world has not been walked yet, so scale stays `1.0`.

- Unity: X right, Y up, Z forward.
- WoW 3.3.5: Z up. X is north, Y is west. Yaw `0` faces north and increases counter-clockwise.
- `wow_x = unity_z * scale`
- `wow_y = -unity_x * scale`
- `wow_z = unity_y * scale`
- Unity yaw is the Y-up angle of the quaternion, radians, `0` facing Unity +Z, positive toward Unity +X: `atan2(2(qw*qy + qz*qx), 1 - 2(qx*qx + qy*qy))`.
- `wow_yaw = -unity_yaw`, then wrapped into `[0, 2π)`.
- Inverse, for type 2 points and normals: `unity_x = -wow_y / scale`, `unity_y = wow_z / scale`, `unity_z = wow_x / scale`. A WoW up normal `(0, 0, 1)` is Unity `(0, 1, 0)`.
- `scale` defaults to `1.0` in config. Tune after a measured length. Do not hardcode a yards-per-metre constant in the packet.

The server stores position and yaw only. The full quaternion stays in the datagram for the client camera. Yaw flight is phase 3. Roll is after that gate, still client-side.

Camera basis, same quaternion, same axes. Rotate Unity forward `(0, 0, 1)` and Unity up `(0, 1, 0)` by the quaternion, then map each vector with `wow_x = unity_z`, `wow_y = -unity_x`, `wow_z = unity_y`. Identity looks north with up as WoW Z. A +90° Unity yaw (quaternion `(0, sin(π/4), 0, cos(π/4))`) looks along WoW `-Y`. Packet bytes do not change.

Placement is not in the datagram. The first settled pose is the pad. The server anchors that pad to the character's feet, then adds the mapped delta of later poses. Packet bytes stay as written above.

The same type 1 datagram is also sent to `127.0.0.1:47562` for the local camera client. That port is not a new message.

## Contact (server → sim), type 2

Defined now so the socket stays compatible. The server sends type 2 to the source of the latest pose. The plugin applies contacts whose `flags` bit 0 is set to that same rigidbody. No contact leaves motor output unchanged.

The server replies to the source address of the latest pose, same port.

| Offset | Type | Field |
|---:|---|---|
| 0 | 4 bytes | magic `WDRN` |
| 4 | u8 | version `1` |
| 5 | u8 | type `2` |
| 6 | u8 | count `0..4` |
| 7 | u8 | reserved `0` |
| 8 | count × 28 | point xyz and normal xyz as f32, plus one u32 flags. Point and normal are **Unity metres and Unity axes**, already inverse-mapped. |

`flags` bit 0: solid. Other bits reserved 0.

Phase 4 casts a few rays against the guest's vmaps (or the client's collision if that is already in process) and fills this. The plugin applies the contacts to the same rigidbody. The flight model never parses ADT files.
