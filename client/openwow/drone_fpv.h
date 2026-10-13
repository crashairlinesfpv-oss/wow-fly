#pragma once

namespace openwow::game {

struct DroneFpvView {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float fx = 1.0f;
  float fy = 0.0f;
  float fz = 0.0f;
  float ux = 0.0f;
  float uy = 0.0f;
  float uz = 1.0f;
  float vertical_fov_radians = 1.4f;
};

// True while the quad is flying. If drone_fpv.txt contains a realm host, the
// selected realm must match that host. With no realm line, the realm this
// client already selected is used. player_x/y/z are the active player's feet.
// The orbit camera stays when this returns false.
bool DroneFpvPoll(float player_x, float player_y, float player_z, DroneFpvView* out);

}  // namespace openwow::game
