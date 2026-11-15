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
#include <rex/memory.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include SR_INIT_H

#include "sr_lua.h"

namespace srps5 {
namespace {

namespace sony = rex::input::sony;

// The player, as the games' own script functions find him (ci: tmp/reg3.py found the C functions
// the games register for their script names; addresses from their code):
//  - Saints Row: is_player_in_vehicle (824E67C0) reads the player at *(82F7EB04) and asks 8245E0C8,
//    which answers "in a vehicle" when +0xD80 is 0 and the vehicle at +0x9C0 is set (and alive).
//    scripted_cutscene_playing (824F0A40 -> 8220B640) keeps its flag at 835F23DC.
//  - Saints Row 2: "#PLAYER#" (8274ED08's name lookup) is the local player at *(835BF42C); his
//    vehicle handle is at +0xD64 (8274EF00, get_char_in_vehicle's lookup: 0 = on foot).
#if SR_GAME == 1
constexpr uint32_t kPlayerPtr = 0x82F7EB04, kVehicleField = 0x9C0, kOnFootOverride = 0xD80, kCutscene = 0x835F23DC;
#else
constexpr uint32_t kPlayerPtr = 0x835BF42C, kVehicleField = 0xD64, kOnFootOverride = 0, kCutscene = 0;
#endif

constexpr uint8_t kHeld = 40;  // a trigger past this (of 255) is held

std::atomic<uint64_t> g_rumble_up_ms{0};  // the game's own rumble last rose (its XInputSetState)
uint32_t g_rumble_last = 0;               // (the game thread's)

// A guest word, only where the memory is mapped and readable (the player can be freed under us:
// these are reads from our own thread, never writes).
bool Read32(rex::memory::Memory* memory, uint32_t address, uint32_t* out) {
  if (!address) return false;
  uint32_t protect = 0;
  auto* heap = memory->LookupHeap(address);
  if (!heap || !heap->QueryProtect(address, &protect) || !(protect & rex::memory::kMemoryProtectRead)) return false;
  *out = rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual(address));
  return true;
}

struct Where {
  bool in_play = false;     // a player, and no scripted cutscene
  bool in_vehicle = false;  // in one (driving or a passenger)
};

Where Find() {
  Where w;
  auto* kernel = REX_KERNEL_STATE();
  if (!kernel) return w;
  auto* memory = kernel->memory();
  uint32_t player = 0, vehicle = 0, cutscene = 0, override_ = 0;
  if (!Read32(memory, kPlayerPtr, &player) || !player) return w;
  if (kCutscene && Read32(memory, kCutscene, &cutscene) && cutscene) return w;
  if (!Read32(memory, player + kVehicleField, &vehicle)) return w;
  w.in_play = true;
  if (kOnFootOverride && Read32(memory, player + kOnFootOverride, &override_) && override_) vehicle = 0;
  w.in_vehicle = vehicle != 0;
  return w;
}

}  // namespace

void DualSenseRumble(uint32_t user, uint32_t motors) {
  if (user != 0) return;
  if (motors > g_rumble_last) g_rumble_up_ms.store(SDL_GetTicks(), std::memory_order_relaxed);
  g_rumble_last = motors;
}

void StartDualSense(int game) {
  static std::atomic<bool> started{false};
  if (started.exchange(true)) return;
  sony::GameState state{};
  state.active = true;
  state.custom = true;
  state.color = std::array<uint8_t, 3>{150, 40, 230};  // (the Saints' purple, both games)
  REXLOG_INFO("SAINTS ROW: ReInstated: DualSense light bar {:02X}{:02X}{:02X}, adaptive triggers in play "
              "(on foot: R2 the gun, L2 the aim; in a vehicle: both pedals; R2 kicks with the game's own "
              "rumble while it fires)",
              state.color[0], state.color[1], state.color[2]);
  // (the service's lease on a published state is 250 ms; the input driver reads it on SDL's clock)
  std::thread([state] {
    float last_health = -1;
    int last_context = -1;
    uint64_t last_kick = 0;
    bool said_kick = false;
    int said_context = 0;
    for (;;) {
      const uint64_t now = SDL_GetTicks();
      // The player's health from the games' scripts (sr_lua.h, only while a cheat is held): the
      // light bar dims with it and blinks when it's low, a hit pulses the controller.
      const PlayerStatus player = GetPlayerStatus();
      const bool fresh = player.updated_ms && now - player.updated_ms < 1500;
      const bool scripts_in_game = fresh && player.in_game;
      // Where the player is, from the game's own memory (sr: Find): the triggers.
      const Where where = Find();
      sony::GameState s = state;
      s.sprint = 0;
      if (where.in_play) {
        s.trigger_context = where.in_vehicle ? sony::TriggerContext::kVehicle : sony::TriggerContext::kWeapon;
        s.in_vehicle = s.driver = where.in_vehicle;
        s.aiming = s.can_fire = !where.in_vehicle;
      }
      const int context = static_cast<int>(s.trigger_context);
      if (context != last_context) {
        last_context = context;
        if (said_context++ < 12)
          REXLOG_INFO("SAINTS ROW: ReInstated: DualSense triggers {}",
                      context == 1 ? "on (on foot: R2 gun, L2 aim)" : context == 2 ? "on (vehicle: pedals)" : "off");
      }
      float hit = 0;
      if (scripts_in_game) {
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
        // A shot: on foot, R2 held and the game's own rumble just rose (its guns rumble as they fire).
        if (user == 0 && where.in_play && !where.in_vehicle && device.right_trigger > kHeld &&
            now - g_rumble_up_ms.load(std::memory_order_relaxed) < 40 && now - last_kick >= 70) {
          last_kick = now;
          sony::GetService().Emit(0, device.generation, sony::Event::kShot, 1.0f, now);
          if (!said_kick) {
            said_kick = true;
            REXLOG_INFO("SAINTS ROW: ReInstated: DualSense first recoil in R2");
          }
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(where.in_play ? 16 : 50));
    }
  }).detach();
}

}  // namespace srps5

// The title's XInputSetState (r3 the user, r4 its XINPUT_VIBRATION: two big-endian u16 motors):
// the game's own rumble goes on as always; its rises are the guns' shots for R2's kick.
#if SR_GAME == 1
extern "C" void sub_8272CE98(PPCContext& __restrict ctx, uint8_t* base) {
#else
extern "C" void sub_82AA61E0(PPCContext& __restrict ctx, uint8_t* base) {
#endif
  const uint32_t user = ctx.r3.u32, vibration = ctx.r4.u32;
  if (vibration) {
    const uint32_t motors = rex::memory::load_and_swap<uint32_t>(base + vibration);
    srps5::DualSenseRumble(user, (motors >> 16) + (motors & 0xFFFF));
  }
#if SR_GAME == 1
  __imp__sub_8272CE98(ctx, base);
#else
  __imp__sub_82AA61E0(ctx, base);
#endif
}

#endif  // REX_PLATFORM_PS5
