// SAINTS ROW: ReInstated (PS5) - calls into the games' own script engine. Both games embed Lua 5.0
// and register their game API in it (turn_invulnerable, notoriety_reset, cash_add, hud_hide,
// get_current_hit_points...: what the mission scripts call); the cheats and the DualSense's game
// state call those functions, so they go through the games' own code.
//
// No Lua is compiled: each call is the global looked up by name (lua_pushstring + lua_rawget on
// the globals), its arguments pushed (strings with lua_pushstring; booleans and numbers written
// on the stack as the game's TValues are laid out) and a protected call made, its results read
// off the stack and the stack put back. (Compiling a chunk crashed Saints Row 2's parser.)
//
// The calls run on the game's thread at a safe point: as the game's code calls into its scripts
// (Saints Row: luaD_call 8250EB28 from outside the Lua core; Saints Row 2: lua_call 829CB0E0), on
// that state, before the game's call, a while after the game started.
#pragma once

#include <rex/platform.h>
#if REX_PLATFORM_PS5

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace srps5 {

// What the periodic calls last reported about the player (any thread).
struct PlayerStatus {
  bool in_game = false;  // the player is there and alive
  float health = 0, max_health = 0;
  uint64_t updated_ms = 0;  // (SDL_GetTicks of the report)
};
PlayerStatus GetPlayerStatus();

// One argument: a string, a boolean or a number.
struct LuaArg {
  enum Kind { kString, kBool, kNumber } kind = kNumber;
  std::string s;
  double n = 0;
  static LuaArg Str(std::string v) { LuaArg a; a.kind = kString; a.s = std::move(v); return a; }
  static LuaArg Bool(bool v) { LuaArg a; a.kind = kBool; a.n = v ? 1 : 0; return a; }
  static LuaArg Num(double v) { LuaArg a; a.kind = kNumber; a.n = v; return a; }
};

// One call of a global function, and its first result: a number (or a boolean as 0/1), NaN when
// it's something else; ok false when the function isn't there or raised an error.
struct LuaCall {
  std::string function;
  std::vector<LuaArg> args;
};
struct LuaResult {
  bool ok = false;
  double value = 0;
};

// Runs the calls once, in order, at the next safe point; `done` (optional) gets their results, on
// the game's thread. A batch whose first call's function isn't defined yet (the game's API not
// there, a menu state) is tried again later.
void LuaRunOnce(std::vector<LuaCall> calls, std::function<void(const std::vector<LuaResult>&)> done = nullptr);

// Every `period_ms`: the calls `make` returns, their results to `done` (the tick: the cheats held,
// the player's health).
void LuaSetTick(std::function<std::vector<LuaCall>()> make,
                std::function<void(const std::vector<LuaResult>&)> done, uint32_t period_ms);

// (for the tick's report)
void SetPlayerStatus(const PlayerStatus& status);

}  // namespace srps5

#endif  // REX_PLATFORM_PS5
