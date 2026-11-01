// SAINTS ROW: ReInstated (PS5) - SKIP INTRO (the menu's setting, sr_skip_intro): Saints Row's
// Activision and Radical logo movie (movies/activisionRadicalLogos.bik) not played.
//
// Saints Row's front end plays its movies through one function (82273D60: r3 the path, the rest
// how), and moves on when the movie player (82D84670) says nothing is playing (82273378): a logo
// movie that never starts is one that has ended, and the title screen comes up. (Saints Row 2's
// Radical and Activision logos are frames of its legal screen's movie, art/hud/legalloading.gfx:
// tools/prompts/make_prompts.py makes the Radical logo's frame go to the end of them, a patch
// sr_prompts.cpp writes over the movie as it's read while this is on.)
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
                    "Start the games at their title screens, without the Activision and Radical logos");

#if SR_GAME == 1 && SR_GAME_HOOKS
extern "C" void sub_82273D60(PPCContext& __restrict ctx, uint8_t* base) {
  if (REXCVAR_GET(sr_skip_intro) && ctx.r3.u32) {
    auto* kernel = REX_KERNEL_STATE();
    const char* path = kernel ? kernel->memory()->TranslateVirtual<const char*>(ctx.r3.u32) : nullptr;
    if (path && strncasecmp(path, "movies/activisionRadicalLogos", 29) == 0) {
      static std::atomic<int> logged{0};
      if (logged.fetch_add(1) < 3) REXLOG_INFO("SAINTS ROW: ReInstated: {} skipped (SKIP INTRO)", path);
      ctx.r3.u64 = 0;
      return;
    }
  }
  __imp__sub_82273D60(ctx, base);
}
#endif

#endif  // REX_PLATFORM_PS5
