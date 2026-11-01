// SAINTS ROW: ReInstated (PS5) - Lua run in the games' own script engine (see sr_lua.h).
#include "sr_lua.h"

#if REX_PLATFORM_PS5

#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <utility>

#include <SDL3/SDL_timer.h>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include SR_INIT_H

namespace srps5 {
namespace {

using GuestFn = void (*)(PPCContext&, uint8_t*);

// The games' Lua 5.1 API, found through its standalone debug console (ldblib's db_debug: the
// "=(debug command)" chunk name): luaL_loadbuffer(L, buffer, size, name),
// lua_pcall(L, nargs, nresults, errfunc), lua_tolstring(L, index, size_t*), lua_settop(L, index).
// (Prototype's addresses: off until Saints Row's own are found, SR_GAME_HOOKS in CMakeLists.txt)
#if SR_GAME_HOOKS
#if SR_GAME == 1
const GuestFn kLoadBuffer = sub_82ACB660, kPcall = __imp__sub_829F96B0, kToLString = sub_829F8910,
              kSetTop = sub_829F8230;
#else
const GuestFn kLoadBuffer = sub_82C261B8, kPcall = __imp__sub_82BE8610, kToLString = sub_82BE7820,
              kSetTop = sub_8219AB18;
#endif
#endif  // SR_GAME_HOOKS

constexpr uint32_t kBufferSize = 64 * 1024;  // (the chunk name, then the chunk)
constexpr char kChunkName[] = "=ReInstated";

struct Pending {
  std::string chunk;
  std::function<void(const std::string&)> done;
  uint64_t first_ms = 0;  // (when first tried)
  uint64_t next_ms = 0;   // (not before: a skipped chunk waits a little)
};

std::mutex g_mutex;
std::deque<Pending> g_queue;              // guarded by g_mutex
std::function<std::string()> g_make;      // guarded by g_mutex
uint32_t g_period = 0;                    // guarded by g_mutex
uint64_t g_last_tick = 0;                 // (the game threads', under g_run)
std::mutex g_run;                         // one chunk at a time, whichever game thread
uint32_t g_buffer = 0;                    // the guest buffer (under g_run)
thread_local bool t_busy = false;         // (our own lua_pcall is the game's: not re-entered)

std::mutex g_status_mutex;
PlayerStatus g_status;
std::atomic<uint32_t> g_reports{0}, g_errors{0};

// The chunk run on the state L (a guest pointer); what it returned, or "error: ...".
std::string Run(PPCContext& ctx, uint8_t* base, uint32_t L, const std::string& chunk) {
  auto* kernel = REX_KERNEL_STATE();
  if (!kernel || !kernel->memory()) return "error: no memory";
  auto* memory = kernel->memory();
  if (!g_buffer) g_buffer = memory->SystemHeapAlloc(kBufferSize);
  if (!g_buffer) return "error: no guest buffer";
  if (chunk.size() + sizeof(kChunkName) + 1 > kBufferSize) return "error: chunk too long";
  uint8_t* host = memory->TranslateVirtual<uint8_t*>(g_buffer);
  std::memcpy(host, kChunkName, sizeof(kChunkName));
  const uint32_t text = g_buffer + sizeof(kChunkName);
  std::memcpy(host + sizeof(kChunkName), chunk.data(), chunk.size());

  // (the game's registers as they were at its lua_pcall, put back after; our calls use the stack
  // below its)
  const PPCContext saved = ctx;
  const uint64_t stack = (saved.r1.u64 - 0x400) & ~uint64_t(0xF);
  auto call = [&](GuestFn fn, uint64_t r4, uint64_t r5, uint64_t r6) {
    ctx.r1.u64 = stack;
    ctx.r3.u64 = L;
    ctx.r4.u64 = r4;
    ctx.r5.u64 = r5;
    ctx.r6.u64 = r6;
    fn(ctx, base);
    return ctx.r3.u32;
  };
#if SR_GAME_HOOKS
  int status = int32_t(call(kLoadBuffer, text, chunk.size(), g_buffer));
  if (status == 0) status = int32_t(call(kPcall, 0, 1, 0));
  // (one value on top either way: the chunk's result or the error message)
  const uint32_t str = call(kToLString, uint64_t(int64_t(-1)), 0, 0);
  std::string out;
  if (str) {
    const char* s = memory->TranslateVirtual<const char*>(str);
    out.assign(s, strnlen(s, 4096));
  }
  call(kSetTop, uint64_t(int64_t(-2)), 0, 0);
  ctx = saved;
  return status ? "error: " + out : out;
#else
  (void)call;
  (void)text;
  return "error: no Lua hooks yet";
#endif
}

void Report(const std::string& out) {
  PlayerStatus s;
  s.updated_ms = SDL_GetTicks();
  float h = 0, m = 0;
  int g = 0;
  if (std::sscanf(out.c_str(), "%f %f %d", &h, &m, &g) == 3) {
    s.health = h;
    s.max_health = m;
    s.in_game = g != 0 && m > 0;
  }
  std::lock_guard lock(g_status_mutex);
  g_status = s;
}

void OnPcall(PPCContext& ctx, uint8_t* base) {
  if (t_busy || !ctx.r3.u32) return;
  std::unique_lock run(g_run, std::try_to_lock);
  if (!run.owns_lock()) return;
  Pending job;
  std::function<std::string()> make;
  {
    std::lock_guard lock(g_mutex);
    if (!g_queue.empty() && g_queue.front().next_ms <= SDL_GetTicks()) {
      job = std::move(g_queue.front());
      g_queue.pop_front();
    } else if (g_make && SDL_GetTicks() - g_last_tick >= g_period) {
      make = g_make;
    }
  }
  if (job.chunk.empty() && !make) return;
  t_busy = true;
  const uint32_t L = ctx.r3.u32;
  if (!job.chunk.empty()) {
    const std::string out = Run(ctx, base, L, job.chunk);
    if (!job.first_ms) job.first_ms = SDL_GetTicks();
    if (out == "skip" && SDL_GetTicks() - job.first_ms < 10 * 60 * 1000) {
      // (not this state, or no player yet: tried again a quarter second on, for up to 10 minutes)
      job.next_ms = SDL_GetTicks() + 250;
      std::lock_guard lock(g_mutex);
      g_queue.push_back(std::move(job));
    } else {
      if (out.rfind("error:", 0) == 0 && g_errors.fetch_add(1) < 40) REXLOG_WARN("SAINTS ROW: ReInstated: Lua {}", out);
      if (job.done) job.done(out);
    }
  } else {
    g_last_tick = SDL_GetTicks();
    const std::string out = Run(ctx, base, L, make());
    // (a state without the game's API, the front end's, says "skip": another one gets the tick, a
    // twentieth of a second on)
    if (out == "skip") {
      g_last_tick = g_last_tick + 50 - g_period;
    } else if (out.rfind("error:", 0) == 0) {
      if (g_errors.fetch_add(1) < 40) REXLOG_WARN("SAINTS ROW: ReInstated: Lua tick {}", out);
    } else {
      if (g_reports.fetch_add(1) < 5) REXLOG_INFO("SAINTS ROW: ReInstated: Lua tick on {:08X}: {}", L, out);
      Report(out);
    }
  }
  t_busy = false;
}

}  // namespace

PlayerStatus GetPlayerStatus() {
  std::lock_guard lock(g_status_mutex);
  return g_status;
}

void LuaRunOnce(std::string chunk, std::function<void(const std::string&)> done) {
  std::lock_guard lock(g_mutex);
  if (g_queue.size() < 32) g_queue.push_back({std::move(chunk), std::move(done)});
}

void LuaSetTick(std::function<std::string()> make, uint32_t period_ms) {
  std::lock_guard lock(g_mutex);
  g_make = std::move(make);
  g_period = period_ms;
}

}  // namespace srps5

// The game's lua_pcall: our chunks first, at this safe point, then the game's call.
// (Prototype's addresses: off until Saints Row's own are found, SR_GAME_HOOKS in CMakeLists.txt)
#if SR_GAME_HOOKS
#if SR_GAME == 1
extern "C" void sub_829F96B0(PPCContext& __restrict ctx, uint8_t* base) {
  srps5::OnPcall(ctx, base);
  __imp__sub_829F96B0(ctx, base);
}
#else
extern "C" void sub_82BE8610(PPCContext& __restrict ctx, uint8_t* base) {
  srps5::OnPcall(ctx, base);
  __imp__sub_82BE8610(ctx, base);
}
#endif
#endif  // SR_GAME_HOOKS

#endif  // REX_PLATFORM_PS5
