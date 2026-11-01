// SAINTS ROW: ReInstated (PS5) - calls into the games' own script engine (see sr_lua.h).
#include "sr_lua.h"

#if REX_PLATFORM_PS5

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <utility>

#include <SDL3/SDL_timer.h>

#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include SR_INIT_H

namespace srps5 {
namespace {

using GuestFn = void (*)(PPCContext&, uint8_t*);

// The games' Lua 5.0 API, found through the base library's function table (luaB_pcall, luaB_print,
// luaB_tostring) and the order of lapi.c's functions:
//  - Saints Row 2: lua_pushstring(L, s) 829CA8A8, lua_rawget(L, index) 829CAB00, lua_pcall(L, nargs,
//    nresults, errfunc) 829CB108, lua_settop(L, index) 829C9E58. TValues 16 bytes: the type at +0,
//    the value at +8 (numbers are doubles).
//  - Saints Row inlines lua_pcall: luaD_pcall(L, f_call, &CallS{func, nresults}, func's stack
//    offset, errfunc) 8250EE78 and f_call 82505FD0, as luaB_pcall does; lua_pushstring 82505A00,
//    lua_rawget 82505BC8, lua_settop 82505180. TValues 8 bytes: the type at +0, the value at +4
//    (numbers are floats).
// lua_State: top at +8, the stack at +0x1C. LUA_GLOBALSINDEX -10001.
#if SR_GAME == 1
const GuestFn kPushString = sub_82505A00, kRawGet = sub_82505BC8, kDPcall = sub_8250EE78, kSetTop = sub_82505180;
constexpr uint32_t kFCall = 0x82505FD0, kTValue = 8, kValue = 4;
#else
const GuestFn kPushString = sub_829CA8A8, kRawGet = sub_829CAB00, kPcall = sub_829CB108, kSetTop = sub_829C9E58;
constexpr uint32_t kTValue = 16, kValue = 8;
#endif
constexpr int64_t kGlobals = -10001;
constexpr uint32_t kNil = 0, kBoolean = 1, kNumber = 3, kFunction = 6;
constexpr uint32_t kBufferSize = 4096;  // (strings for lua_pushstring; CallS)

struct Pending {
  std::vector<LuaCall> calls;
  std::function<void(const std::vector<LuaResult>&)> done;
  uint64_t first_ms = 0;  // (when first tried)
  uint64_t next_ms = 0;   // (not before: a skipped batch waits a little)
};

std::mutex g_mutex;
std::deque<Pending> g_queue;                                     // guarded by g_mutex
std::function<std::vector<LuaCall>()> g_make;                    // guarded by g_mutex
std::function<void(const std::vector<LuaResult>&)> g_tick_done;  // guarded by g_mutex
uint32_t g_period = 0;                                           // guarded by g_mutex
uint64_t g_last_tick = 0;                                        // (the game threads', under g_run)
std::mutex g_run;                                                // one batch at a time
uint32_t g_buffer = 0;                                           // the guest buffer (under g_run)
thread_local bool t_busy = false;  // (our own calls into the game's Lua: not re-entered)

std::mutex g_status_mutex;
PlayerStatus g_status;
std::atomic<uint32_t> g_errors{0}, g_reports{0};

// The calls run on the state L (a guest pointer); empty when the first call's function isn't there.
std::vector<LuaResult> Run(PPCContext& ctx, uint8_t* base, uint32_t L, const std::vector<LuaCall>& calls) {
  std::vector<LuaResult> results;
  auto* kernel = REX_KERNEL_STATE();
  if (!kernel || !kernel->memory()) return results;
  auto* memory = kernel->memory();
  if (!g_buffer) g_buffer = memory->SystemHeapAlloc(kBufferSize);
  if (!g_buffer) return results;
  auto get32 = [&](uint32_t at) { return rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual(at)); };
  auto put32 = [&](uint32_t at, uint32_t v) { rex::memory::store_and_swap<uint32_t>(memory->TranslateVirtual(at), v); };
  auto put64 = [&](uint32_t at, uint64_t v) { rex::memory::store_and_swap<uint64_t>(memory->TranslateVirtual(at), v); };
  auto get64 = [&](uint32_t at) { return rex::memory::load_and_swap<uint64_t>(memory->TranslateVirtual(at)); };

  // (the game's registers as they were at its call, put back after; our calls use the stack below
  // its)
  const PPCContext saved = ctx;
  const uint64_t stack = (saved.r1.u64 - 0x400) & ~uint64_t(0xF);
  auto call = [&](GuestFn fn, uint64_t r4, uint64_t r5 = 0, uint64_t r6 = 0, uint64_t r7 = 0) {
    ctx.r1.u64 = stack;
    ctx.r3.u64 = L;
    ctx.r4.u64 = r4;
    ctx.r5.u64 = r5;
    ctx.r6.u64 = r6;
    ctx.r7.u64 = r7;
    fn(ctx, base);
    return ctx.r3.u32;
  };
  // A string for lua_pushstring, in the guest buffer (after the CallS's 8 bytes).
  auto push_string = [&](const std::string& s) {
    const uint32_t at = g_buffer + 16;
    const size_t n = std::min<size_t>(s.size(), kBufferSize - 32);
    std::memcpy(memory->TranslateVirtual<char*>(at), s.data(), n);
    memory->TranslateVirtual<char*>(at)[n] = 0;
    call(kPushString, at);
  };
  // A boolean or a number written as a TValue at the top (lua_call left LUA_MINSTACK free slots).
  auto push_value = [&](uint32_t type, double n) {
    const uint32_t top = get32(L + 8);
    put32(top, type);
    if (type == kBoolean) {
      put32(top + kValue, n != 0 ? 1 : 0);
    } else if (kValue == 8) {
      uint64_t bits;
      std::memcpy(&bits, &n, 8);
      put64(top + kValue, bits);
    } else {
      const float f = float(n);
      uint32_t bits;
      std::memcpy(&bits, &f, 4);
      put32(top + kValue, bits);
    }
    put32(L + 8, top + kTValue);
  };

  const uint32_t top0 = get32(L + 8);
  for (size_t i = 0; i < calls.size(); ++i) {
    const LuaCall& c = calls[i];
    // the function: globals[name]
    push_string(c.function);
    call(kRawGet, uint64_t(kGlobals));
    const uint32_t func = get32(L + 8) - kTValue;
    if (get32(func) != kFunction) {
      put32(L + 8, func);  // (popped)
      if (i == 0) break;   // (not there yet: the batch is for later)
      results.push_back({});
      continue;
    }
    for (const LuaArg& a : c.args) {
      if (a.kind == LuaArg::kString) push_string(a.s);
      else push_value(a.kind == LuaArg::kBool ? kBoolean : kNumber, a.n);
    }
    const uint32_t nargs = uint32_t(c.args.size());
#if SR_GAME == 1
    put32(g_buffer, func);
    put32(g_buffer + 4, 1);  // (one result)
    const int status = int32_t(call(kDPcall, kFCall, g_buffer, func - get32(L + 0x1C), 0));
#else
    const int status = int32_t(call(kPcall, nargs, 1, 0));
#endif
    (void)nargs;
    // one value on top either way: the result or the error message
    const uint32_t value = get32(L + 8) - kTValue;
    LuaResult r;
    r.ok = status == 0;
    const uint32_t type = get32(value);
    if (type == kNumber) {
      if (kValue == 8) {
        const uint64_t bits = get64(value + kValue);
        std::memcpy(&r.value, &bits, 8);
      } else {
        const uint32_t bits = get32(value + kValue);
        float f;
        std::memcpy(&f, &bits, 4);
        r.value = f;
      }
    } else if (type == kBoolean) {
      r.value = get32(value + kValue) ? 1 : 0;
    } else {
      r.value = std::nan("");
    }
    if (!r.ok && g_errors.fetch_add(1) < 20) REXLOG_WARN("SAINTS ROW: ReInstated: Lua {} failed", c.function);
    results.push_back(r);
    call(kSetTop, uint64_t(int64_t(-2)));  // (the value popped)
  }
  // (the stack as the game left it)
  const int32_t extra = int32_t(get32(L + 8) - top0) / int32_t(kTValue);
  if (extra > 0) call(kSetTop, uint64_t(int64_t(-extra - 1)));
  ctx = saved;
  return results;
}

void OnPcall(PPCContext& ctx, uint8_t* base) {
  if (t_busy || !ctx.r3.u32) return;
  // Not while the game is starting: 45 s after its first script call.
  static const uint64_t first_call_ms = SDL_GetTicks();
  if (SDL_GetTicks() - first_call_ms < 45000) return;
  std::unique_lock run(g_run, std::try_to_lock);
  if (!run.owns_lock()) return;
  Pending job;
  std::function<std::vector<LuaCall>()> make;
  std::function<void(const std::vector<LuaResult>&)> tick_done;
  {
    std::lock_guard lock(g_mutex);
    if (!g_queue.empty() && g_queue.front().next_ms <= SDL_GetTicks()) {
      job = std::move(g_queue.front());
      g_queue.pop_front();
    } else if (g_make && SDL_GetTicks() - g_last_tick >= g_period) {
      make = g_make;
      tick_done = g_tick_done;
    }
  }
  if (job.calls.empty() && !make) return;
  t_busy = true;
  const uint32_t L = ctx.r3.u32;
  if (!job.calls.empty()) {
    const std::vector<LuaResult> out = Run(ctx, base, L, job.calls);
    if (!job.first_ms) job.first_ms = SDL_GetTicks();
    if (out.empty() && SDL_GetTicks() - job.first_ms < 10 * 60 * 1000) {
      // (the game's API not in this state yet: tried again a quarter second on, for up to 10 minutes)
      job.next_ms = SDL_GetTicks() + 250;
      std::lock_guard lock(g_mutex);
      g_queue.push_back(std::move(job));
    } else if (job.done) {
      job.done(out);
    }
  } else {
    g_last_tick = SDL_GetTicks();
    const std::vector<LuaResult> out = Run(ctx, base, L, make());
    if (out.empty()) {
      g_last_tick = g_last_tick + 50 - g_period;  // (another state gets the tick, a moment on)
    } else {
      if (g_reports.fetch_add(1) < 3) REXLOG_INFO("SAINTS ROW: ReInstated: Lua tick on {:08X}: {} results", L, out.size());
      if (tick_done) tick_done(out);
    }
  }
  t_busy = false;
}

}  // namespace

PlayerStatus GetPlayerStatus() {
  std::lock_guard lock(g_status_mutex);
  return g_status;
}

void SetPlayerStatus(const PlayerStatus& status) {
  std::lock_guard lock(g_status_mutex);
  g_status = status;
}

void LuaRunOnce(std::vector<LuaCall> calls, std::function<void(const std::vector<LuaResult>&)> done) {
  std::lock_guard lock(g_mutex);
  if (g_queue.size() < 32) g_queue.push_back({std::move(calls), std::move(done)});
}

void LuaSetTick(std::function<std::vector<LuaCall>()> make,
                std::function<void(const std::vector<LuaResult>&)> done, uint32_t period_ms) {
  std::lock_guard lock(g_mutex);
  g_make = std::move(make);
  g_tick_done = std::move(done);
  g_period = period_ms;
}

}  // namespace srps5

// The safe point: the game's own calls into its scripts, our chunks first, on that state.
#if SR_GAME == 1
// Saints Row: luaD_call(L, func, nresults) (its lua_call is inlined into the game's script calls),
// only when the game's code called it (LR outside the Lua core, 82504000-82520000: not a call the
// interpreter makes from inside a running script). func is a stack pointer: taken again after our
// chunk, as its offset, in case the stack grew.
extern "C" void sub_8250EB28(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t lr = uint32_t(ctx.lr);
  if (lr && (lr < 0x82504000 || lr >= 0x82520000)) {
    const uint32_t L = ctx.r3.u32;
    auto* kernel = REX_KERNEL_STATE();
    if (L && kernel) {
      auto* memory = kernel->memory();
      const uint32_t offset = ctx.r4.u32 - rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual(L + 0x1C));
      srps5::OnPcall(ctx, base);
      ctx.r4.u64 = rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual(L + 0x1C)) + offset;
    }
  }
  __imp__sub_8250EB28(ctx, base);
}
#else
// Saints Row 2: lua_call(L, nargs, nresults), which only C code calls (the game's and its libraries').
extern "C" void sub_829CB0E0(PPCContext& __restrict ctx, uint8_t* base) {
  srps5::OnPcall(ctx, base);
  __imp__sub_829CB0E0(ctx, base);
}
#endif

#endif  // REX_PLATFORM_PS5
