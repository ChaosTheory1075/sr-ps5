// SAINTS ROW: ReInstated - ReXGlue recompiled app, Saints Row's and Saints Row 2's (see sr_app.h).
#include "sr_app.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <system_error>
#include <thread>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/platform.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/object_table.h>
#include <rex/system/xthread.h>
#include <rex/ui/overlay/achievement_notification.h>
#if REX_PLATFORM_PS5
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include SR_INIT_H
#include "ps5/sr_dualsense.h"
#include "ps5/sr_install.h"
#include "ps5/sr_menu.h"
#include "ps5/sr_overlay.h"
#include "ps5/sr_prompts.h"
#include "ps5/sr_quick_menu.h"
#include "ps5/sr_trophies_game.h"

namespace fs = std::filesystem;

namespace {

#if REX_PLATFORM_PS5
const fs::path kAppRoot("/app0");
#endif

// The app's own folder: /app0 on the PS5, beside the program elsewhere.
fs::path AppRoot(const rex::PathConfig& defaults) {
#if REX_PLATFORM_PS5
  (void)defaults;
  return kAppRoot;
#else
  return defaults.config_path.parent_path();
#endif
}

fs::path g_app_root;

constexpr const char* kGameName = SR_GAME == 2 ? "Saints Row 2" : "Saints Row";

rex::PathConfig ResolvePaths(const rex::PathConfig& defaults);

}  // namespace

// The crash line's guest half (ps5_diag.cpp, patched in by ci/patch_framework.py):
// the faulting thread's volatile registers (kept in its context), LR, CTR, and
// its callers from the stack's back chain. Runs in the fault handler: no heap.
extern "C" int rex_guest_fault_report(char* out, int capacity) {
  auto* thread = rex::system::XThread::GetCurrentThread();
  if (!thread || !thread->thread_state() || !thread->thread_state()->context()) return 0;
  const auto* c = thread->thread_state()->context();
  int n = std::snprintf(out, size_t(capacity),
                        "thread %X lr %08X ctr %08X r1 %08X r3 %08X r4 %08X r5 %08X r6 %08X r7 %08X r8 %08X "
                        "r9 %08X r10 %08X r11 %08X r12 %08X | callers",
                        thread->handle(), uint32_t(c->lr), c->ctr.u32, c->r1.u32, c->r3.u32, c->r4.u32, c->r5.u32,
                        c->r6.u32, c->r7.u32, c->r8.u32, c->r9.u32, c->r10.u32, c->r11.u32, c->r12.u32);
  auto* kernel = REX_KERNEL_STATE();
  const uint32_t lo = thread->guest_stack_limit(), hi = thread->guest_stack_base();
  uint32_t sp = c->r1.u32;
  for (int i = 0; kernel && i < 16 && n > 0 && n < capacity - 12; ++i) {
    // (only inside this thread's stack: it's committed)
    if (sp < lo || sp + 4 > hi || (sp & 3)) break;
    const uint32_t back = rex::memory::load_and_swap<uint32_t>(kernel->memory()->TranslateVirtual(sp));
    if (back <= sp || back < lo + 8 || back > hi) break;
    const uint32_t lr = rex::memory::load_and_swap<uint32_t>(kernel->memory()->TranslateVirtual(back - 8));
    n += std::snprintf(out + n, size_t(capacity - n), " %08X", lr);
    sp = back;
  }
  return n;
}

#if REX_PLATFORM_PS5
// The start-up trace (patched into main and ReXApp by ci/patch_framework.py): each stage before
// the log file exists, appended to logs/boot.txt and closed at once, so an app that closes before
// its log opens says how far it got. (No heap, no C++ runtime: it runs from the first constructor.)
extern "C" void rex_boot_mark(const char* what) {
  mkdir("/app0/logs", 0777);
  const int fd = open("/app0/logs/boot.txt", O_WRONLY | O_CREAT | O_APPEND, 0666);
  if (fd < 0) return;
  char line[160];
  const int n = std::snprintf(line, sizeof(line), "%s %s\n", SR_GAME == 2 ? "sr2" : "sr1", what);
  if (n > 0) (void)write(fd, line, size_t(n));
  close(fd);
}
__attribute__((constructor(101))) static void BootMarkFirst() { rex_boot_mark("---- static init"); }
#endif

std::unique_ptr<rex::ui::WindowedApp> SrApp::Create(rex::ui::WindowedAppContext& ctx) {
  return std::unique_ptr<SrApp>(new SrApp(ctx, SR_GAME == 2 ? "sr2" : "sr1", PPCImageConfig));
}

std::optional<rex::PathConfig> SrApp::OnFinalizePaths(const rex::PathConfig& defaults,
                                                       std::function<void(rex::PathConfig)> resume) {
#if REX_PLATFORM_PS5
#if SR_GAME == 1
  // The menu first (sr_menu.cpp). PLAY on Saints Row carries on with the start; on Saints Row 2 the
  // menu starts sr2.bin instead (srps5::StartGame).
  srps5::ShowMenu(imgui_drawer(), immediate_drawer(), AppRoot(defaults),
                     [defaults, resume](srps5::MenuChoice) { resume(ResolvePaths(defaults)); });
  return std::nullopt;
#else
  // Started by the menu (eboot.bin's): its settings, and on with the game.
  (void)resume;
  srps5::ApplySettings(AppRoot(defaults));
  return ResolvePaths(defaults);
#endif
#else
  (void)resume;
  return ResolvePaths(defaults);
#endif
}

void SrApp::OnConfigureFonts(ImFontAtlas* atlas) {
#if REX_PLATFORM_PS5
  srps5::ConfigureMenuFonts(atlas);
#else
  (void)atlas;
#endif
}

void SrApp::OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) {
#if REX_PLATFORM_PS5
  // The frame rate counter (drawing nothing while it's off in SETTINGS).
  static std::unique_ptr<srps5::GameOverlay> overlay;
  overlay = std::make_unique<srps5::GameOverlay>(drawer);
  // The in-game quick menu, L3 + R3: the cheats (ps5/sr_quick_menu.h), and their Lua tick, which
  // also gives the DualSense the player's state.
  srps5::StartQuickMenu(drawer);
#else
  (void)drawer;
#endif
}

std::unique_ptr<rex::ui::AchievementNotificationDialog> SrApp::CreateAchievementNotificationDialog() {
#if REX_PLATFORM_PS5
  // (an unlock as a PS5 trophy: ps5/sr_trophies_game.h)
  return std::make_unique<srps5::TrophyPopup>(imgui_drawer(), immediate_drawer(), runtime());
#else
  return rex::ReXApp::CreateAchievementNotificationDialog();
#endif
}

void SrApp::OnPostLaunchModule(rex::system::XThread* thread) {
  rex::ReXApp::OnPostLaunchModule(thread);
#if REX_PLATFORM_PS5
  // (saves the menu imported before the game made its profile folder: into it now)
  if (auto* kernel = REX_KERNEL_STATE(); kernel && kernel->user_profile() && !g_app_root.empty())
    srps5::install::PlacePendingSaves(g_app_root / "saves", kernel->user_profile()->xuid(), SR_GAME);
  srps5::StartDualSense(SR_GAME);
  srps5::InstallPrompts();
  // Bring-up: where every guest thread is, every 10 s for the first 3 minutes (a thread that never
  // waits - the main thread spinning on a loading screen - shows in no wait list).
  std::thread([] {
    for (int tick = 1; tick <= 18; ++tick) {
      std::this_thread::sleep_for(std::chrono::seconds(10));
      auto* kernel = REX_KERNEL_STATE();
      if (!kernel) continue;
      auto* memory = kernel->memory();
      for (const auto& t : kernel->object_table()->GetObjectsByType<rex::system::XThread>(
               rex::system::XObject::Type::Thread)) {
        if (!t->is_guest_thread() || !t->thread_state() || !t->thread_state()->context()) continue;
        const auto* c = t->thread_state()->context();
        std::string callers;
        const uint32_t lo = t->guest_stack_limit(), hi = t->guest_stack_base();
        uint32_t sp = c->r1.u32;
        for (int i = 0; i < 10; ++i) {
          if (sp < lo || sp + 4 > hi || (sp & 3)) break;
          const uint32_t back = rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual(sp));
          if (back <= sp || back < lo + 8 || back > hi) break;
          callers += fmt::format(" < {:08X}", rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual(back - 8)));
          sp = back;
        }
        REXLOG_INFO("{} threads {}s: {:X} lr {:08X} r3 {:08X} r31 {:08X} |{}", kGameName, tick * 10, t->handle(),
                    uint32_t(c->lr), c->r3.u32, c->r31.u32, callers);
      }
    }
  }).detach();
#endif
}

void SrApp::OnPostSetup() {
#if REX_PLATFORM_PS5
  // The achievements' list and pictures, for the menu's Trophies page.
  if (runtime()) srps5::WriteTrophyCatalog(runtime(), runtime()->user_data_root());
#endif
}

namespace {

// The game's folders: game1/ (Saints Row's disc) or game2/ (Saints Row 2's), the DLC (Saints Row 2's
// in dlc2/; Saints Row has none: an empty folder in UserData/) and saves/ (the two keep theirs
// under their own title ids).
rex::PathConfig ResolvePaths(const rex::PathConfig& defaults) {
  auto paths = defaults;
  const fs::path root = AppRoot(defaults);
  std::error_code ec;
  g_app_root = root;
  if (paths.game_data_root.empty()) paths.game_data_root = root / (SR_GAME == 2 ? "game2" : "game1");
  if (paths.update_data_root.empty() && fs::is_directory(paths.game_data_root / "update", ec))
    paths.update_data_root = paths.game_data_root / "update";
  if (paths.marketplace_content_root.empty())
    paths.marketplace_content_root = root / (SR_GAME == 2 ? "dlc2" : "dlc1");
  if (paths.saved_game_root.empty()) paths.saved_game_root = root / "saves";
  fs::create_directories(paths.saved_game_root, ec);
  fs::create_directories(paths.marketplace_content_root, ec);

  REXLOG_INFO("{}: game {}, update {}, dlc {}, saves {}", kGameName, paths.game_data_root.string(),
              paths.update_data_root.string(), paths.marketplace_content_root.string(),
              paths.saved_game_root.string());
  if (!fs::exists(paths.game_data_root / "default.xex", ec))
    REXLOG_ERROR("{}: no default.xex in {} (install the game from the menu, see README.txt)", kGameName,
                 paths.game_data_root.string());
  else if (!fs::exists(paths.game_data_root / "default.xexp", ec))
    REXLOG_ERROR("{}: no default.xexp in {} (the program is the title update's code: install {})", kGameName,
                 paths.game_data_root.string(), SR_GAME == 2 ? "Title Update 3" : "Title Update 1");
  return paths;
}

}  // namespace

void SrApp::OnPreSetup(rex::RuntimeConfig& config) {
  // The generic Xenos GPU (Xenia's Vulkan backend): no game-specific renderer.
  if (!config.graphics && config.gpu_plugin.empty()) config.gpu_plugin = "xenos";
  // Xenia's defaults otherwise: Xenia Canary plays both (xenia-canary/game-compatibility#501,
  // #1039). sr1.toml / sr2.toml beside eboot.bin can set any of these to try another way.
  // Textures whose fetch constants are typed "invalid" (all zero) are drawn, as Xenia suggests,
  // instead of the draw being dropped (Metal Gear Rising's title screen needed it).
  if (!rex::cvar::HasNonDefaultValue("gpu_allow_invalid_fetch_constants"))
    rex::cvar::SetLaunchFlag("gpu_allow_invalid_fetch_constants", "true");
  // The 360's EDRAM emulated exactly (fragment shader interlock), as Max Payne 3 RePrescribed
  // runs. On host render targets Saints Row's distant buildings z-fought into stripes (24-bit float
  // depth only approximated; Xenia's tracker: "corrupted draw distance (maybe this has to do with
  // non-ROV rendering)", game-compatibility#472), and render targets the games reinterpret between
  // formats lose their data. sr1.toml / sr2.toml can set render_target_path_vulkan = "host".
  if (!rex::cvar::HasNonDefaultValue("render_target_path_vulkan"))
    rex::cvar::SetLaunchFlag("render_target_path_vulkan", "fsi");
  // Occlusion queries answered with Xenia's default fake count, not the host's: each real one
  // made the GPU thread wait for the PS5's GPU (Saints Row 2's gameplay: half the thread's time
  // in CompleteOcclusionQueries).
  if (!rex::cvar::HasNonDefaultValue("occlusion_query_enable"))
    rex::cvar::SetLaunchFlag("occlusion_query_enable", "false");
  // As Max Payne 3 RePrescribed: frames with a pipeline still being built are shown (not
  // skipped), and three creation threads, not 3/4 of the CPUs, so a burst of new pipelines in a
  // new area doesn't starve the game's threads.
  rex::cvar::SetLaunchFlag("vulkan_async_skip_incomplete_frames", "false");
  if (!rex::cvar::HasNonDefaultValue("vulkan_pipeline_creation_threads"))
    rex::cvar::SetLaunchFlag("vulkan_pipeline_creation_threads", "3");
  // No stall reports: with the FPS counter's NotePresent wired up, their SIGPROF sampling of every
  // thread fires on every load, and in Crackdown PS5 loads froze right after one (its 58a24b5).
  if (!rex::cvar::HasNonDefaultValue("ps5_hang_reports")) rex::cvar::SetLaunchFlag("ps5_hang_reports", "false");
#if SR_GAME == 2
  // Saints Row 2's Bink cutscenes are frames the GPU resolves into textures: read back into guest
  // memory ("fast": a frame late) so a reload of those pages from guest memory brings back the
  // movie, not black (it went black after 3 s).
  if (!rex::cvar::HasNonDefaultValue("readback_resolve")) rex::cvar::SetLaunchFlag("readback_resolve", "fast");
#endif
  REXLOG_INFO("{}: readback_resolve={} gpu_allow_invalid_fetch_constants={} render_target_path_vulkan={}", kGameName,
              rex::cvar::GetFlagByName("readback_resolve"),
              rex::cvar::GetFlagByName("gpu_allow_invalid_fetch_constants"),
              rex::cvar::GetFlagByName("render_target_path_vulkan"));
}

#if REX_PLATFORM_PS5 && SR_GAME == 1
// Saints Row's DemonWare networking (bdNetImpl::receiveAndDispatchAll, 82704560) reads its socket every
// frame through 82763BF8 (r3 the socket object: its vtable's receive). Offline there is no socket: r3
// was null and the first read jumped through address 0 right after the start-up movie. No socket,
// nothing received (r3 = 0: the caller takes it as no packets).
extern "C" void sub_82763BF8(PPCContext& __restrict ctx, uint8_t* base) {
  if (!ctx.r3.u32) {
    static bool logged = false;
    if (!logged) REXLOG_INFO("Saints Row: no network socket (offline): DemonWare receives skipped");
    logged = true;
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_82763BF8(ctx, base);
}
#endif

#if REX_PLATFORM_PS5 && SR_GAME == 1
// Its network object (made at start-up by 8236B270: 827050A0) is deleted at once when the network
// doesn't come up (offline, always here); the destructor's shutdown (82704840: disconnect, pump the
// last packets) ran over the parts never set up and read through a null pointer (+0xC) on the way
// to the main menu. Nothing was connected: nothing to shut down.
extern "C" void sub_82704840(PPCContext& __restrict ctx, uint8_t* base) {
  (void)ctx;
  (void)base;
  static bool logged = false;
  if (!logged) REXLOG_INFO("Saints Row: offline: the network object's shutdown skipped");
  logged = true;
}
#endif

#if REX_PLATFORM_PS5 && SR_GAME == 1
// The network object's update (827644C8 > 827703D8 > 827708A8) walks the connection list of a
// lobby that was never made offline: r3 = 8 (null + 8), its first read at 0xC jumped the game out
// right after the start-up movie. A list that isn't there has nothing in it.
extern "C" void sub_827708A8(PPCContext& __restrict ctx, uint8_t* base) {
  if (ctx.r3.u32 < 0x10000) {
    static bool logged = false;
    if (!logged) REXLOG_INFO("Saints Row: offline: no lobby connection list ({:08X})", ctx.r3.u32);
    logged = true;
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_827708A8(ctx, base);
}
#endif

#if REX_PLATFORM_PS5 && SR_GAME == 1
// ...and the update itself (827644C8, r3 the lobby: null offline, its first read at +0x38) from the
// main loop's network tick (8236A24C). No lobby, nothing to update.
extern "C" void sub_827644C8(PPCContext& __restrict ctx, uint8_t* base) {
  if (ctx.r3.u32 < 0x10000) {
    static bool logged = false;
    if (!logged) REXLOG_INFO("Saints Row: offline: no lobby to update");
    logged = true;
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_827644C8(ctx, base);
}
#endif

#if REX_PLATFORM_PS5 && SR_GAME == 1
// The main loop's network tick (82704558, from 8236A1F0 with the network object 8236B270 returned:
// null offline, as it deletes the one it made when the network doesn't come up). No object, no tick.
extern "C" void sub_82704558(PPCContext& __restrict ctx, uint8_t* base) {
  if (ctx.r3.u32 < 0x10000) {
    static bool logged = false;
    if (!logged) REXLOG_INFO("Saints Row: offline: no network object, its tick skipped");
    logged = true;
    return;
  }
  __imp__sub_82704558(ctx, base);
}
#endif
