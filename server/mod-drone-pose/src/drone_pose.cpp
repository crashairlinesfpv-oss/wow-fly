/*
 * AzerothCore module. One character, named in drone_pose.conf, is the pilot
 * for pose logs and ground contacts. Empty DronePose.CharacterName applies
 * to nobody.
 *
 * The pilot is moved with a fly spline (SMSG_MONSTER_MOVE), which the stock
 * client already applies to itself. The pad is anchored to the character's
 * feet. Can-fly is set only while that spline is airborne. Gravity is not
 * disabled: that flag is what made a jump swim in the air.
 *
 * No account passwords. Axis mapping is the hypothesis in docs/pose-protocol.md
 * until a worldserver log confirms it.
 */
#include "Config.h"
#include "Log.h"
#include "Map.h"
#include "MoveSplineInit.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "pose_map.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace
{
constexpr int32 PORT_DEFAULT = 47561;
constexpr float SCALE_DEFAULT = 1.0f;
constexpr uint32 LOG_INTERVAL_MS = 1000;
constexpr float GROUND_SEARCH = 200.f;
constexpr float GROUND_MARGIN = 0.3f;
constexpr float ENGAGE_SPEED = 1.5f;
constexpr float RELEASE_SPEED = 0.6f;
constexpr float RELEASE_RADIUS = 1.2f;
constexpr float SPLINE_MIN = 0.15f;
constexpr uint32 SPLINE_INTERVAL_MS = 50;
constexpr float SPLINE_INTERVAL_SEC = 0.05f;
constexpr float FLY_HEIGHT = 1.5f;
constexpr uint32 STALE_MS = 1000;

class DronePoseService
{
public:
    static DronePoseService& Instance()
    {
        static DronePoseService service;
        return service;
    }

    void LoadConfig()
    {
        _enable = sConfigMgr->GetOption<bool>("DronePose.Enable", true);
        _name = sConfigMgr->GetOption<std::string>("DronePose.CharacterName", "");
        _bind = sConfigMgr->GetOption<std::string>("DronePose.Bind", "0.0.0.0");
        _port = sConfigMgr->GetOption<int32>("DronePose.Port", PORT_DEFAULT);
        _scale = sConfigMgr->GetOption<float>("DronePose.Scale", SCALE_DEFAULT);
        if (_port <= 0 || _port > 65535)
            _port = PORT_DEFAULT;
    }

    void Open()
    {
        if (!_enable || _fd >= 0)
            return;

        int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0)
        {
            LOG_ERROR("server.worldserver", "DronePose socket failed: {}", std::strerror(errno));
            return;
        }

        int reuse = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(_port));
        if (::inet_pton(AF_INET, _bind.c_str(), &addr.sin_addr) != 1)
        {
            LOG_ERROR("server.worldserver", "DronePose bind address '{}' is not IPv4", _bind);
            ::close(fd);
            return;
        }
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
        {
            LOG_ERROR("server.worldserver", "DronePose bind {}:{} failed: {}", _bind, _port, std::strerror(errno));
            ::close(fd);
            return;
        }

        int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags >= 0)
            ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        _fd = fd;
        LOG_INFO("server.worldserver", "DronePose listening on {}:{} scale {:.3f} character '{}'",
            _bind, _port, _scale, _name.empty() ? "(nobody)" : _name);
    }

    void Close()
    {
        if (_fd >= 0)
        {
            ::close(_fd);
            _fd = -1;
        }
    }

    void Poll()
    {
        if (_fd < 0)
            return;

        OnIdle();

        PoseView latest;
        bool have = false;
        sockaddr_in from;
        std::memset(&from, 0, sizeof(from));

        for (int i = 0; i < 64; ++i)
        {
            uint8_t buf[128];
            sockaddr_in src;
            socklen_t slen = sizeof(src);
            ssize_t got = ::recvfrom(_fd, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&src), &slen);
            if (got < 0)
                break;
            PoseView view;
            if (!UnpackPose(buf, static_cast<size_t>(got), view))
                continue;
            latest = view;
            from = src;
            have = true;
        }
        if (!have)
            return;

        _lastPoseMs = getMSTime();
        WowPose wow = MapUnityToWow(latest.px, latest.py, latest.pz, latest.qx, latest.qy, latest.qz, latest.qw, _scale);
        LogMapped(latest, wow);

        Player* player = FindPilot();
        if (!player)
            return;

        Follow(player, latest, wow);
        SendGroundContact(player, latest, from);
    }

    void OnIdle()
    {
        Player* player = FindPilot();
        if (!player)
        {
            _control = false;
            _air = false;
            _haveAnchor = false;
            return;
        }
        if (!_control || !_lastPoseMs)
            return;
        if (getMSTimeDiff(_lastPoseMs, getMSTime()) < STALE_MS)
            return;
        Release(player);
    }

private:
    DronePoseService() = default;

    Player* FindPilot() const
    {
        if (!_enable || _name.empty())
            return nullptr;
        Player* player = ObjectAccessor::FindPlayerByName(_name, false);
        if (!player || !player->IsInWorld())
            return nullptr;
        return player;
    }

    void LogMapped(PoseView const& pose, WowPose const& wow)
    {
        uint32 now = getMSTime();
        if (_loggedOnce && getMSTimeDiff(_lastLogMs, now) < LOG_INTERVAL_MS)
            return;
        _loggedOnce = true;
        _lastLogMs = now;
        LOG_INFO("server.worldserver",
            "DronePose seq={} unity=({:.3f}, {:.3f}, {:.3f}) wow=({:.3f}, {:.3f}, {:.3f}) yaw={:.3f} scale={:.3f}",
            pose.sequence, pose.px, pose.py, pose.pz, wow.x, wow.y, wow.z, wow.yaw, _scale);
    }

    void Remember(Player* player, PoseView const& pose)
    {
        _haveAnchor = true;
        _aux = pose.px;
        _auy = pose.py;
        _auz = pose.pz;
        _ax = player->GetPositionX();
        _ay = player->GetPositionY();
        _az = player->GetPositionZ();
    }

    void Place(PoseView const& pose, float& x, float& y, float& z) const
    {
        float scale = _scale == 0.f ? SCALE_DEFAULT : _scale;
        x = _ax + (pose.pz - _auz) * scale;
        y = _ay - (pose.px - _aux) * scale;
        z = _az + (pose.py - _auy) * scale;
    }

    void Take(Player* player)
    {
        if (_control)
            return;
        // Client control off so WASD does not drag the spline back. This is the
        // same packet a flight path uses. Gravity stays on.
        player->SetClientControl(player, false);
        _control = true;
        LOG_INFO("server.worldserver",
            "DronePose follow {} from wow=({:.1f}, {:.1f}, {:.1f})",
            player->GetName(), _ax, _ay, _az);
    }

    void Release(Player* player)
    {
        if (!_control)
            return;
        Movement::MoveSplineInit init(player);
        init.Stop();
        // SetCanFly on a player sends the client packet and does not store the
        // flag, so the HasUnitMovementFlag check never saw it and walk came
        // back still able to fly.
        player->RemoveUnitMovementFlag(MOVEMENTFLAG_FLYING | MOVEMENTFLAG_CAN_FLY | MOVEMENTFLAG_DISABLE_GRAVITY | MOVEMENTFLAG_ASCENDING | MOVEMENTFLAG_DESCENDING);
        player->SetCanFly(false);
        player->SetDisableGravity(false);
        player->SetClientControl(player, true);
        _control = false;
        _air = false;
        _splineMoving = false;
        LOG_INFO("server.worldserver", "DronePose released {}", player->GetName());
    }

    void Follow(Player* player, PoseView const& pose, WowPose const& wow)
    {
        if (player->IsBeingTeleported() || player->IsInFlight())
            return;

        float speed = std::sqrt(pose.vx * pose.vx + pose.vy * pose.vy + pose.vz * pose.vz);
        if (!_haveAnchor)
            Remember(player, pose);

        float dx = pose.px - _aux;
        float dy = pose.py - _auy;
        float dz = pose.pz - _auz;
        float rel = std::sqrt(dx * dx + dy * dy + dz * dz);

        if (!_control)
        {
            // Keep the pad under their feet while the quad is sitting still,
            // including if they walk before takeoff.
            if (speed < ENGAGE_SPEED)
            {
                Remember(player, pose);
                return;
            }
            // A new flight starts from wherever the character is standing now.
            Remember(player, pose);
            Take(player);
        }
        else if (speed < RELEASE_SPEED && rel < RELEASE_RADIUS && std::fabs(dy) < RELEASE_RADIUS)
        {
            Release(player);
            Remember(player, pose);
            return;
        }

        float x, y, z;
        Place(pose, x, y, z);
        if (!Acore::IsValidMapCoord(x, y, z))
            return;

        bool airborne = (pose.py - _auy) > FLY_HEIGHT;
        if (airborne != _air)
        {
            player->SetCanFly(airborne);
            _air = airborne;
        }

        float dist = std::sqrt(
            (x - player->GetPositionX()) * (x - player->GetPositionX()) +
            (y - player->GetPositionY()) * (y - player->GetPositionY()) +
            (z - player->GetPositionZ()) * (z - player->GetPositionZ()));
        if (speed < RELEASE_SPEED && dist < 0.5f)
        {
            if (_splineMoving)
            {
                Movement::MoveSplineInit stop(player);
                stop.Stop();
                _splineMoving = false;
            }
            return;
        }
        uint32 now = getMSTime();
        if (_splineMoving && getMSTimeDiff(_lastSplineMs, now) < SPLINE_INTERVAL_MS)
            return;
        if (dist < SPLINE_MIN)
            return;

        // Cover this gap in one send interval. No speed floor, so a throttle
        // cut is not held up, and no extra catch-up, so the path does not snap.
        float vel = dist / SPLINE_INTERVAL_SEC;
        if (vel > 80.f)
            vel = 80.f;
        if (vel < 0.5f)
            return;

        _lastSplineMs = now;
        _splineMoving = true;

        Movement::MoveSplineInit init(player);
        init.MoveTo(x, y, z, false);
        if (airborne)
            init.SetFly();
        init.SetVelocity(vel);
        init.SetFacing(wow.yaw);
        init.Launch();
    }

    void SendGroundContact(Player* player, PoseView const& pose, sockaddr_in const& to)
    {
        if (!_haveAnchor || !_control)
            return;

        Map* map = player->GetMap();
        if (!map)
            return;

        float x, y, z;
        Place(pose, x, y, z);
        // Search from above the pose so a point already under the floor still hits it.
        float ground = map->GetHeight(x, y, z + GROUND_SEARCH, true, GROUND_SEARCH);
        if (ground <= INVALID_HEIGHT || z > ground + GROUND_MARGIN)
            return;

        float scale = _scale == 0.f ? SCALE_DEFAULT : _scale;
        ContactView contact;
        contact.px = _aux - (y - _ay) / scale;
        contact.py = _auy + (ground - _az) / scale;
        contact.pz = _auz + (x - _ax) / scale;
        contact.nx = 0.f;
        contact.ny = 1.f;
        contact.nz = 0.f;
        contact.flags = 1;

        uint8_t buf[8 + CONTACT_STRIDE];
        size_t n = PackContacts(&contact, 1, buf, sizeof(buf));
        if (n == 0)
            return;
        ::sendto(_fd, buf, n, 0, reinterpret_cast<sockaddr const*>(&to), sizeof(to));
    }

    bool _enable = true;
    std::string _name;
    std::string _bind = "0.0.0.0";
    int32 _port = PORT_DEFAULT;
    float _scale = SCALE_DEFAULT;
    int _fd = -1;
    uint32 _lastLogMs = 0;
    uint32 _lastPoseMs = 0;
    bool _loggedOnce = false;
    bool _haveAnchor = false;
    bool _control = false;
    bool _air = false;
    bool _splineMoving = false;
    uint32 _lastSplineMs = 0;
    float _aux = 0.f;
    float _auy = 0.f;
    float _auz = 0.f;
    float _ax = 0.f;
    float _ay = 0.f;
    float _az = 0.f;
};

class DronePoseWorldScript : public WorldScript
{
public:
    DronePoseWorldScript() : WorldScript("DronePoseWorldScript") { }

    void OnStartup() override
    {
        DronePoseService::Instance().LoadConfig();
        DronePoseService::Instance().Open();
    }

    void OnShutdown() override
    {
        DronePoseService::Instance().Close();
    }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        DronePoseService::Instance().LoadConfig();
    }

    void OnUpdate(uint32 /*diff*/) override
    {
        DronePoseService::Instance().Poll();
    }
};
}

void Addmod_drone_poseScripts()
{
    new DronePoseWorldScript();
}
