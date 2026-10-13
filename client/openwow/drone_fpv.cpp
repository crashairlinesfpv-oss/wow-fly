#include "openwow/game/drone_fpv.h"

#include "openwow/foundation/diagnostics/logging.h"
#include "openwow/net/client_services.h"
#include "openwow/ui/game/cvar_system.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

#include <SDL2/SDL.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#if defined(_WIN32)
using SocketHandle = SOCKET;
constexpr SocketHandle kBadSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
constexpr SocketHandle kBadSocket = -1;
#endif

namespace openwow::game {
namespace {

constexpr int kCameraPort = 47562;
constexpr int kTogglePort = 47563;
constexpr float kEngageSpeed = 1.5f;
constexpr float kReleaseSpeed = 0.6f;
constexpr float kReleaseRadius = 1.2f;
constexpr std::uint64_t kStaleNs = 1000000000ull;
constexpr char kCameraFile[] = "drone_fpv.txt";
constexpr float kPi = 3.14159265358979323846f;

SocketHandle g_socket = kBadSocket;
bool g_bind_logged = false;
bool g_realm_logged = false;
bool g_fly_logged = false;

bool g_have_pose = false;
bool g_have_pad = false;
bool g_flying = false;
float g_pad_x = 0.0f;
float g_pad_y = 0.0f;
float g_pad_z = 0.0f;
float g_px = 0.0f;
float g_py = 0.0f;
float g_pz = 0.0f;
float g_qx = 0.0f;
float g_qy = 0.0f;
float g_qz = 0.0f;
float g_qw = 1.0f;
float g_speed = 0.0f;
bool g_view_ready = false;
float g_ax = 0.0f;
float g_ay = 0.0f;
float g_az = 0.0f;
float g_aux = 0.0f;
float g_auy = 0.0f;
float g_auz = 0.0f;
std::uint64_t g_pose_ns = 0;

float g_pitch_deg = 55.0f;
float g_fov_deg = 137.0f;
float g_eye_height = 1.6f;
std::string g_realm_filter;
bool g_cfg_known = false;
bool g_keys_held = false;
std::filesystem::file_time_type g_cfg_mtime{};

std::uint64_t NowNs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

void CloseSocket(SocketHandle fd) {
#if defined(_WIN32)
  ::closesocket(fd);
#else
  ::close(fd);
#endif
}

float ReadF32(const unsigned char* p) {
  float value = 0.0f;
  std::memcpy(&value, p, sizeof(value));
  return value;
}

bool Finite(float v) { return std::isfinite(v); }

void QuatRotate(float qx, float qy, float qz, float qw, float vx, float vy, float vz,
                float* ox, float* oy, float* oz) {
  const float tx = 2.0f * (qy * vz - qz * vy);
  const float ty = 2.0f * (qz * vx - qx * vz);
  const float tz = 2.0f * (qx * vy - qy * vx);
  *ox = vx + qw * tx + (qy * tz - qz * ty);
  *oy = vy + qw * ty + (qz * tx - qx * tz);
  *oz = vz + qw * tz + (qx * ty - qy * tx);
}

void UnityDirToWow(float ux, float uy, float uz, float* wx, float* wy, float* wz) {
  *wx = uz;
  *wy = -ux;
  *wz = uy;
}

bool Normalize(float* x, float* y, float* z) {
  const float len = std::sqrt((*x) * (*x) + (*y) * (*y) + (*z) * (*z));
  if (!(len > 1.0e-4f)) {
    return false;
  }
  *x /= len;
  *y /= len;
  *z /= len;
  return true;
}

void LoadRealmFilter() {
  std::error_code ec;
  if (!std::filesystem::exists(kCameraFile, ec)) {
    if (!g_realm_filter.empty()) {
      g_realm_logged = false;
    }
    g_realm_filter.clear();
    return;
  }
  std::ifstream in(kCameraFile);
  if (!in) {
    return;
  }
  std::string found;
  bool have = false;
  std::string line;
  while (std::getline(in, line)) {
    const auto comment = line.find('#');
    if (comment != std::string::npos) {
      line.resize(comment);
    }
    std::istringstream row(line);
    std::string key;
    if (!(row >> key) || key != "realm") {
      continue;
    }
    std::string host;
    if (row >> host) {
      found = host;
    } else {
      found.clear();
    }
    have = true;
  }
  if (!have) {
    found.clear();
  }
  if (found != g_realm_filter) {
    g_realm_logged = false;
  }
  g_realm_filter = found;
}

bool HostMatches(const std::string& text) {
  if (g_realm_filter.empty()) {
    return true;
  }
  const std::string& realm = g_realm_filter;
  std::size_t pos = 0;
  while ((pos = text.find(realm, pos)) != std::string::npos) {
    const bool left_ok =
        pos == 0 || !std::isalnum(static_cast<unsigned char>(text[pos - 1]));
    const std::size_t end = pos + realm.size();
    const bool right_ok = end == text.size() || text[end] == ':' ||
                          std::isspace(static_cast<unsigned char>(text[end])) ||
                          text[end] == '"' || text[end] == '\'';
    if (left_ok && right_ok) {
      return true;
    }
    pos = end;
  }
  return false;
}

bool RealmAllowed() {
  LoadRealmFilter();
  if (g_realm_filter.empty()) {
    return true;
  }
  const std::string& selected =
      openwow::net::ClientServices::Instance().GetSelectedRealmAddress();
  if (!selected.empty()) {
    return HostMatches(selected);
  }
  return HostMatches(openwow::ui::game::CVarSystem::Instance().GetCVar("realmList"));
}

void EnsureSocket() {
  if (g_socket != kBadSocket) {
    return;
  }
#if defined(_WIN32)
  static bool wsa_ready = false;
  if (!wsa_ready) {
    WSADATA data{};
    if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) {
      return;
    }
    wsa_ready = true;
  }
#endif
  const SocketHandle fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd == kBadSocket) {
    return;
  }
  const int reuse = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
               sizeof(reuse));
#if defined(_WIN32)
  u_long nonblock = 1;
  ::ioctlsocket(fd, FIONBIO, &nonblock);
#else
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
#endif
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(kCameraPort);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    if (!g_bind_logged) {
      g_bind_logged = true;
      openwow::diagnostics::Log(openwow::diagnostics::LogLevel::kError,
                                "DroneFpv could not bind 127.0.0.1:47562");
    }
    CloseSocket(fd);
    return;
  }
  g_socket = fd;
  openwow::diagnostics::Log(openwow::diagnostics::LogLevel::kInfo,
                            "DroneFpv listening on 127.0.0.1:47562");
}

void RememberPad(float px, float py, float pz) {
  g_pad_x = px;
  g_pad_y = py;
  g_pad_z = pz;
  g_have_pad = true;
}

void AcceptPose(const unsigned char* data) {
  const float px = ReadF32(data + 16);
  const float py = ReadF32(data + 20);
  const float pz = ReadF32(data + 24);
  const float qx = ReadF32(data + 28);
  const float qy = ReadF32(data + 32);
  const float qz = ReadF32(data + 36);
  const float qw = ReadF32(data + 40);
  const float vx = ReadF32(data + 44);
  const float vy = ReadF32(data + 48);
  const float vz = ReadF32(data + 52);
  if (!Finite(px) || !Finite(py) || !Finite(pz) || !Finite(qx) || !Finite(qy) ||
      !Finite(qz) || !Finite(qw) || !Finite(vx) || !Finite(vy) || !Finite(vz)) {
    return;
  }

  g_px = px;
  g_py = py;
  g_pz = pz;
  g_qx = qx;
  g_qy = qy;
  g_qz = qz;
  g_qw = qw;
  g_speed = std::sqrt(vx * vx + vy * vy + vz * vz);
  g_pose_ns = NowNs();
  g_have_pose = true;

  if (!g_have_pad) {
    RememberPad(px, py, pz);
  }

  const float dx = px - g_pad_x;
  const float dy = py - g_pad_y;
  const float dz = pz - g_pad_z;
  const float rel = std::sqrt(dx * dx + dy * dy + dz * dz);

  if (!g_flying) {
    if (g_speed < kEngageSpeed) {
      RememberPad(px, py, pz);
      return;
    }
    RememberPad(px, py, pz);
    g_flying = true;
    if (!g_fly_logged) {
      g_fly_logged = true;
      openwow::diagnostics::Log(openwow::diagnostics::LogLevel::kInfo,
                                "DroneFpv camera on");
    }
    return;
  }

  if (g_speed < kReleaseSpeed && rel < kReleaseRadius && std::fabs(dy) < kReleaseRadius) {
    g_flying = false;
    g_view_ready = false;
    RememberPad(px, py, pz);
    openwow::diagnostics::Log(openwow::diagnostics::LogLevel::kInfo, "DroneFpv camera off");
  }
}

void ClampCamera() {
  g_pitch_deg = std::clamp(g_pitch_deg, -80.0f, 80.0f);
  g_fov_deg = std::clamp(g_fov_deg, 40.0f, 170.0f);
  g_eye_height = std::clamp(g_eye_height, 0.2f, 6.0f);
}

void WriteCameraFile() {
  std::ofstream out(kCameraFile, std::ios::trunc);
  if (!out) {
    return;
  }
  out << "# Drone camera. Change these while flying; the client reloads the file.\n";
  out << "# pitch: degrees up from the quad nose. fov: vertical degrees. height: yards.\n";
  out << "# realm: optional host. Omit it to follow whichever realm this client selected.\n";
  if (!g_realm_filter.empty()) {
    out << "realm " << g_realm_filter << "\n";
  }
  out << "pitch " << g_pitch_deg << "\n";
  out << "fov " << g_fov_deg << "\n";
  out << "height " << g_eye_height << "\n";
  out.close();
  std::error_code ec;
  g_cfg_mtime = std::filesystem::last_write_time(kCameraFile, ec);
  g_cfg_known = true;
}

void ReadCameraFile() {
  std::error_code ec;
  if (!std::filesystem::exists(kCameraFile, ec)) {
    if (!g_cfg_known) {
      WriteCameraFile();
    }
    return;
  }
  const auto mtime = std::filesystem::last_write_time(kCameraFile, ec);
  if (ec || (g_cfg_known && mtime == g_cfg_mtime)) {
    return;
  }
  std::ifstream in(kCameraFile);
  if (!in) {
    return;
  }
  std::string line;
  while (std::getline(in, line)) {
    const auto comment = line.find('#');
    if (comment != std::string::npos) {
      line.resize(comment);
    }
    std::istringstream row(line);
    std::string key;
    float value = 0.0f;
    if (!(row >> key >> value)) {
      continue;
    }
    if (key == "pitch") {
      g_pitch_deg = value;
    } else if (key == "fov") {
      g_fov_deg = value;
    } else if (key == "height") {
      g_eye_height = value;
    }
  }
  ClampCamera();
  g_cfg_mtime = mtime;
  g_cfg_known = true;
  openwow::diagnostics::Log(
      openwow::diagnostics::LogLevel::kInfo,
      "DroneFpv camera pitch " + std::to_string(g_pitch_deg) + " fov " +
          std::to_string(g_fov_deg) + " height " + std::to_string(g_eye_height));
}

void TiltKeys() {
  const Uint8* keys = SDL_GetKeyboardState(nullptr);
  if (keys == nullptr) {
    return;
  }
  static std::uint64_t last_ns = 0;
  const std::uint64_t now = NowNs();
  float dt = last_ns == 0 ? 0.0f : static_cast<float>(now - last_ns) / 1.0e9f;
  last_ns = now;
  if (dt > 0.1f) {
    dt = 0.1f;
  }
  const bool pitch_up = keys[SDL_SCANCODE_RIGHTBRACKET] != 0;
  const bool pitch_down = keys[SDL_SCANCODE_LEFTBRACKET] != 0;
  const bool fov_up = keys[SDL_SCANCODE_EQUALS] != 0;
  const bool fov_down = keys[SDL_SCANCODE_MINUS] != 0;
  const bool height_up = keys[SDL_SCANCODE_PAGEUP] != 0;
  const bool height_down = keys[SDL_SCANCODE_PAGEDOWN] != 0;
  const bool held = pitch_up || pitch_down || fov_up || fov_down || height_up || height_down;
  if (pitch_up) {
    g_pitch_deg += 40.0f * dt;
  }
  if (pitch_down) {
    g_pitch_deg -= 40.0f * dt;
  }
  if (fov_up) {
    g_fov_deg += 25.0f * dt;
  }
  if (fov_down) {
    g_fov_deg -= 25.0f * dt;
  }
  if (height_up) {
    g_eye_height += 1.2f * dt;
  }
  if (height_down) {
    g_eye_height -= 1.2f * dt;
  }
  if (held) {
    ClampCamera();
    g_keys_held = true;
    return;
  }
  if (g_keys_held) {
    g_keys_held = false;
    WriteCameraFile();
  }
}

void ApplyCameraAngle(DroneFpvView* out) {
  const float pitch = g_pitch_deg * (kPi / 180.0f);
  const float c = std::cos(pitch);
  const float s = std::sin(pitch);
  const float fx = out->fx * c + out->ux * s;
  const float fy = out->fy * c + out->uy * s;
  const float fz = out->fz * c + out->uz * s;
  const float ux = out->ux * c - out->fx * s;
  const float uy = out->uy * c - out->fy * s;
  const float uz = out->uz * c - out->fz * s;
  out->fx = fx;
  out->fy = fy;
  out->fz = fz;
  out->ux = ux;
  out->uy = uy;
  out->uz = uz;
  if (!Normalize(&out->fx, &out->fy, &out->fz) || !Normalize(&out->ux, &out->uy, &out->uz)) {
    return;
  }
  out->z = out->z + g_eye_height;
  out->vertical_fov_radians = g_fov_deg * (kPi / 180.0f);
}

void SendCollisionToggle() {
#if defined(_WIN32)
  WSADATA data{};
  if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) {
    return;
  }
#endif
  const SocketHandle fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd == kBadSocket) {
    return;
  }
  const unsigned char buf[7] = {'W', 'D', 'R', 'N', 1, 3, 0};
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(kTogglePort);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ::sendto(fd, reinterpret_cast<const char*>(buf), sizeof(buf), 0,
           reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  CloseSocket(fd);
  openwow::diagnostics::Log(openwow::diagnostics::LogLevel::kInfo,
                            "DroneFpv collision toggle");
}

void PollCollisionKey() {
  const Uint8* keys = SDL_GetKeyboardState(nullptr);
  if (keys == nullptr) {
    return;
  }
  static bool held = false;
  const bool down = keys[SDL_SCANCODE_F8] != 0;
  if (down && !held) {
    SendCollisionToggle();
  }
  held = down;
}

void Drain() {
  EnsureSocket();
  if (g_socket == kBadSocket) {
    return;
  }
  unsigned char buf[128];
  for (;;) {
    int n = 0;
#if defined(_WIN32)
    n = ::recv(g_socket, reinterpret_cast<char*>(buf), static_cast<int>(sizeof(buf)), 0);
    if (n < 0) {
      if (::WSAGetLastError() == WSAEWOULDBLOCK) {
        break;
      }
      break;
    }
#else
    n = static_cast<int>(::recv(g_socket, buf, sizeof(buf), 0));
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      break;
    }
#endif
    if (n < 56) {
      continue;
    }
    if (buf[0] != 'W' || buf[1] != 'D' || buf[2] != 'R' || buf[3] != 'N') {
      continue;
    }
    if (buf[4] != 1 || buf[5] != 1 || buf[6] != 0 || buf[7] != 0) {
      continue;
    }
    AcceptPose(buf);
  }
}

}  // namespace

bool DroneFpvPoll(float player_x, float player_y, float player_z, DroneFpvView* out) {
  if (out == nullptr || !Finite(player_x) || !Finite(player_y) || !Finite(player_z)) {
    return false;
  }
  if (!RealmAllowed()) {
    if (!g_realm_logged) {
      g_realm_logged = true;
      openwow::diagnostics::Log(
          openwow::diagnostics::LogLevel::kError,
          "DroneFpv refused: realm is not " + g_realm_filter);
    }
    g_flying = false;
    g_view_ready = false;
    return false;
  }

  PollCollisionKey();
  Drain();
  if (!g_have_pose || !g_flying) {
    return false;
  }
  if (NowNs() - g_pose_ns > kStaleNs) {
    g_flying = false;
    g_view_ready = false;
    openwow::diagnostics::Log(openwow::diagnostics::LogLevel::kInfo,
                              "DroneFpv camera off (pose stale)");
    return false;
  }

  float fx = 0.0f;
  float fy = 0.0f;
  float fz = 0.0f;
  float ux = 0.0f;
  float uy = 0.0f;
  float uz = 0.0f;
  QuatRotate(g_qx, g_qy, g_qz, g_qw, 0.0f, 0.0f, 1.0f, &fx, &fy, &fz);
  QuatRotate(g_qx, g_qy, g_qz, g_qw, 0.0f, 1.0f, 0.0f, &ux, &uy, &uz);
  UnityDirToWow(fx, fy, fz, &out->fx, &out->fy, &out->fz);
  UnityDirToWow(ux, uy, uz, &out->ux, &out->uy, &out->uz);
  if (!Normalize(&out->fx, &out->fy, &out->fz) || !Normalize(&out->ux, &out->uy, &out->uz)) {
    return false;
  }

  TiltKeys();
  if (!g_keys_held) {
    ReadCameraFile();
  }
  if (!g_view_ready) {
    g_view_ready = true;
    g_ax = player_x;
    g_ay = player_y;
    g_az = player_z;
    g_aux = g_px;
    g_auy = g_py;
    g_auz = g_pz;
  }
  out->x = g_ax + (g_pz - g_auz);
  out->y = g_ay - (g_px - g_aux);
  out->z = g_az + (g_py - g_auy);
  ApplyCameraAngle(out);
  return true;
}

}  // namespace openwow::game
