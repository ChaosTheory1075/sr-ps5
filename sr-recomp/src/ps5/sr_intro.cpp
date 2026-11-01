// SAINTS ROW: ReInstated (PS5) - SKIP INTRO (the menu's setting, sr_skip_intro): the THQ and
// Volition logo movies (d:\videos\logo_thq_hd.bik, logo_volition_hd.bik) not played.
//
// Both games' start-up plays them from a little state machine (Saints Row 822932F0 / 82293450,
// Saints Row 2 8244B790 / 8244B900) through the movie player's play function (Saints Row 8220B9E8,
// Saints Row 2 82394A50: r3 the path), then waits until the player says nothing is playing: a
// logo movie that never starts is one that has ended, and the title screen comes up.
#include <rex/platform.h>
#if REX_PLATFORM_PS5

#include <atomic>
#include <cstring>
#include <strings.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include SR_INIT_H

REXCVAR_DEFINE_BOOL(sr_skip_intro, true, "SAINTS ROW",
                    "Start the games at their title screens, without the THQ and Volition logo movies");

namespace {

// Whether the movie the game asks for (r3, a guest path) is a logo movie to skip.
bool SkipMovie(uint32_t path_address) {
  if (!REXCVAR_GET(sr_skip_intro) || !path_address) return false;
  auto* kernel = REX_KERNEL_STATE();
  const char* path = kernel ? kernel->memory()->TranslateVirtual<const char*>(path_address) : nullptr;
  if (!path || !strcasestr(path, "videos\\logo_")) return false;
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 4) REXLOG_INFO("SAINTS ROW: ReInstated: {} skipped (SKIP INTRO)", path);
  return true;
}

}  // namespace

#if SR_GAME == 1
extern "C" void sub_8220B9E8(PPCContext& __restrict ctx, uint8_t* base) {
  if (SkipMovie(ctx.r3.u32)) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_8220B9E8(ctx, base);
}
#else
extern "C" void sub_82394A50(PPCContext& __restrict ctx, uint8_t* base) {
  if (SkipMovie(ctx.r3.u32)) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_82394A50(ctx, base);
}
#endif

#endif  // REX_PLATFORM_PS5
