/*
 * MapUnityToWow and the pose datagram layout from docs/pose-protocol.md.
 * tools/pose_packet.py is the byte reference. This header is included by the
 * module and by tools/test_pose_map.cpp.
 *
 * Little-endian loads are explicit so a big-endian build cannot silently swap
 * the datagram. The guest is x86_64.
 */
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

static const uint8_t POSE_MAGIC[4] = {'W', 'D', 'R', 'N'};
static const uint8_t POSE_VERSION = 1;
static const uint8_t POSE_TYPE_POSE = 1;
static const uint8_t POSE_TYPE_CONTACT = 2;
static const size_t POSE_SIZE = 56;
static const size_t CONTACT_STRIDE = 28;
static const size_t CONTACT_MAX = 4;
static const float POSE_TAU = 6.28318530717958647692f;

struct PoseView
{
    uint32_t sequence;
    uint32_t time_ms;
    float px, py, pz;
    float qx, qy, qz, qw;
    float vx, vy, vz;
};

struct ContactView
{
    float px, py, pz;
    float nx, ny, nz;
    uint32_t flags;
};

struct WowPose
{
    float x, y, z, yaw;
};

inline uint32_t PoseRdU32(uint8_t const* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

inline uint16_t PoseRdU16(uint8_t const* p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

inline float PoseRdF32(uint8_t const* p)
{
    uint32_t u = PoseRdU32(p);
    float f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

inline void PoseWrU32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

inline void PoseWrF32(uint8_t* p, float f)
{
    uint32_t u;
    std::memcpy(&u, &f, sizeof(u));
    PoseWrU32(p, u);
}

inline bool PoseMagicOk(uint8_t const* p)
{
    return p[0] == POSE_MAGIC[0] && p[1] == POSE_MAGIC[1] && p[2] == POSE_MAGIC[2] && p[3] == POSE_MAGIC[3];
}

inline bool UnpackPose(uint8_t const* data, size_t len, PoseView& out)
{
    if (!data || len < POSE_SIZE || !PoseMagicOk(data))
        return false;
    if (data[4] != POSE_VERSION || data[5] != POSE_TYPE_POSE || PoseRdU16(data + 6) != 0)
        return false;
    out.sequence = PoseRdU32(data + 8);
    out.time_ms = PoseRdU32(data + 12);
    out.px = PoseRdF32(data + 16);
    out.py = PoseRdF32(data + 20);
    out.pz = PoseRdF32(data + 24);
    out.qx = PoseRdF32(data + 28);
    out.qy = PoseRdF32(data + 32);
    out.qz = PoseRdF32(data + 36);
    out.qw = PoseRdF32(data + 40);
    out.vx = PoseRdF32(data + 44);
    out.vy = PoseRdF32(data + 48);
    out.vz = PoseRdF32(data + 52);
    return true;
}

inline size_t PackContacts(ContactView const* contacts, uint8_t count, uint8_t* dest, size_t destLen)
{
    if (count > CONTACT_MAX)
        return 0;
    size_t need = 8 + (size_t)count * CONTACT_STRIDE;
    if (!dest || destLen < need)
        return 0;
    dest[0] = POSE_MAGIC[0];
    dest[1] = POSE_MAGIC[1];
    dest[2] = POSE_MAGIC[2];
    dest[3] = POSE_MAGIC[3];
    dest[4] = POSE_VERSION;
    dest[5] = POSE_TYPE_CONTACT;
    dest[6] = count;
    dest[7] = 0;
    for (uint8_t i = 0; i < count; ++i)
    {
        uint8_t* p = dest + 8 + (size_t)i * CONTACT_STRIDE;
        PoseWrF32(p + 0, contacts[i].px);
        PoseWrF32(p + 4, contacts[i].py);
        PoseWrF32(p + 8, contacts[i].pz);
        PoseWrF32(p + 12, contacts[i].nx);
        PoseWrF32(p + 16, contacts[i].ny);
        PoseWrF32(p + 20, contacts[i].nz);
        PoseWrU32(p + 24, contacts[i].flags);
    }
    return need;
}

inline bool UnpackContacts(uint8_t const* data, size_t len, ContactView* out, uint8_t& count)
{
    count = 0;
    if (!data || len < 8 || !PoseMagicOk(data))
        return false;
    if (data[4] != POSE_VERSION || data[5] != POSE_TYPE_CONTACT || data[7] != 0)
        return false;
    uint8_t n = data[6];
    if (n > CONTACT_MAX || len < 8 + (size_t)n * CONTACT_STRIDE)
        return false;
    for (uint8_t i = 0; i < n; ++i)
    {
        uint8_t const* p = data + 8 + (size_t)i * CONTACT_STRIDE;
        out[i].px = PoseRdF32(p + 0);
        out[i].py = PoseRdF32(p + 4);
        out[i].pz = PoseRdF32(p + 8);
        out[i].nx = PoseRdF32(p + 12);
        out[i].ny = PoseRdF32(p + 16);
        out[i].nz = PoseRdF32(p + 20);
        out[i].flags = PoseRdU32(p + 24);
    }
    count = n;
    return true;
}

inline float UnityYaw(float qx, float qy, float qz, float qw)
{
    float siny = 2.f * (qw * qy + qz * qx);
    float cosy = 1.f - 2.f * (qx * qx + qy * qy);
    return std::atan2(siny, cosy);
}

inline WowPose MapUnityToWow(float ux, float uy, float uz, float qx, float qy, float qz, float qw, float scale)
{
    float yaw = -UnityYaw(qx, qy, qz, qw);
    yaw = std::fmod(yaw, POSE_TAU);
    if (yaw < 0.f)
        yaw += POSE_TAU;
    WowPose w;
    w.x = uz * scale;
    w.y = -ux * scale;
    w.z = uy * scale;
    w.yaw = yaw;
    return w;
}

inline void MapWowToUnity(float x, float y, float z, float scale, float& ux, float& uy, float& uz)
{
    float s = scale == 0.f ? 1.f : scale;
    ux = -y / s;
    uy = z / s;
    uz = x / s;
}
