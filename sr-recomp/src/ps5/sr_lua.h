// SAINTS ROW: ReInstated (PS5) - Lua run in the games' own script engine. Both games embed Lua 5.0
// and register their game API in it (turn_invulnerable, notoriety_reset, cash_add, hud_hide,
// get_current_hit_points...: the mission scripts' own calls); the cheats and the DualSense's game
// state are short chunks of Lua calling it, so they go through the games' own code.
//
// A chunk runs on the game's thread at a safe point: as the game's code calls into its scripts
// (Saints Row: luaD_call 8250EB28 from outside the Lua core; Saints Row 2: lua_call 829CB0E0), on
// that state, before the game's call - loaded and run with the game's own Lua (sr_lua.cpp), the
// stack put back as it was.
#pragma once

#include <rex/platform.h>
#if REX_PLATFORM_PS5

#include <cstdint>
#include <functional>
#include <string>

namespace srps5 {

// What the periodic chunk last reported about the player (any thread).
struct PlayerStatus {
  bool in_game = false;  // the player is there and the HUD is up (not a movie, not a menu)
  float health = 0, max_health = 0;
  uint64_t updated_ms = 0;  // (SDL_GetTicks of the report)
};
PlayerStatus GetPlayerStatus();

// Runs `chunk` once at the next safe point; `done` (optional) gets what it returned (as a string,
// or "error: ..."), on the game's thread. A chunk returning "skip" (not the state it wants: the
// game's API or the player missing) is tried again at the next one.
void LuaRunOnce(std::string chunk, std::function<void(const std::string&)> done = {});

// The periodic chunk: `make` builds its text (on the game's thread, every `period_ms`); it
// returns "<health> <max> <in game 0/1>" or "none" when there's no player.
void LuaSetTick(std::function<std::string()> make, uint32_t period_ms);

}  // namespace srps5

#endif  // REX_PLATFORM_PS5
