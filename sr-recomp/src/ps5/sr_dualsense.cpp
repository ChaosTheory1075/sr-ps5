// SAINTS ROW: ReInstated (PS5) - the DualSense features (see sr_dualsense.h).
#include "sr_dualsense.h"

#if REX_PLATFORM_PS5

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

#include <SDL3/SDL_timer.h>

#include <rex/input/sony_feedback.h>
#include <rex/logging.h>

#include "sr_lua.h"

namespace srps5 {

void StartDualSense(int game) {
  static std::atomic<bool> started{false};
  if (started.exchange(true)) return;
  namespace sony = rex::input::sony;
  sony::GameState state{};
  state.active = true;
  state.custom = true;
  state.color = game == 2 ? std::array<uint8_t, 3>{255, 64, 0} : std::array<uint8_t, 3>{200, 16, 16};
  state.sprint = 0.35f;
  REXLOG_INFO("SAINTS ROW: ReInstated: DualSense light bar {:02X}{:02X}{:02X}, R2 resistance {:.2f}", state.color[0],
              state.color[1], state.color[2], state.sprint);
  // (the service's lease on a published state is 250 ms; the input driver reads it on SDL's clock)
  std::thread([state] {
    float last_health = -1;
    for (;;) {
      const uint64_t now = SDL_GetTicks();
      // The player's state from the games' scripts (sr_lua.h, a few times a second): the light
      // bar dims with his health and blinks when it's low, a hit pulses the controller, and R2's
      // sprint resistance is only there in the game itself (not in its menus and movies).
      const PlayerStatus player = GetPlayerStatus();
      const bool fresh = player.updated_ms && now - player.updated_ms < 1500;
      const bool in_game = fresh && player.in_game;
      sony::GameState s = state;
      s.sprint = in_game ? state.sprint : 0;
      float hit = 0;
      if (in_game) {
        const float frac = std::clamp(player.health / player.max_health, 0.0f, 1.0f);
        float level = 0.3f + 0.7f * frac;
        if (frac < 0.3f && (now / 250) % 2) level *= 0.25f;
        for (int i = 0; i < 3; ++i) s.color[i] = uint8_t(std::lround(state.color[i] * level));
        if (last_health >= 0 && player.health < last_health - player.max_health * 0.02f)
          hit = std::clamp((last_health - player.health) / player.max_health * 4.0f, 0.3f, 1.0f);
        last_health = player.health;
      } else {
        last_health = -1;
      }
      for (uint32_t user = 0; user < 4; ++user) {
        const sony::Device device = sony::GetService().ReadDevice(user);
        if (!device.generation) continue;
        sony::GetService().Publish(user, device.generation, s, now);
        if (hit > 0) sony::GetService().Emit(user, device.generation, sony::Event::kDamage, hit, now);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }).detach();
}

}  // namespace srps5

#endif  // REX_PLATFORM_PS5
