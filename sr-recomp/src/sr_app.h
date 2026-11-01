// SAINTS ROW: ReInstated - ReXGlue recompiled app, built twice: sr1 (Saints Row, eboot.bin) and
// sr2 (Saints Row 2, sr2.bin), told apart by SR_GAME.
//
// On the PS5 the app folder is /app0 (the homebrew folder): Saints Row's disc files in
// /app0/game1, Saints Row 2's in /app0/game2, Saints Row 2's DLC packages in /app0/dlc2 and both
// games' saves in /app0/saves. eboot.bin opens on the menu (ps5/sr_menu.h), which starts either
// game; achievements show as PS5 trophies (ps5/sr_trophies.h).

#pragma once

#include <filesystem>
#include <functional>
#include <optional>

#include <rex/rex_app.h>

class SrApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx);

 protected:
  std::optional<rex::PathConfig> OnFinalizePaths(const rex::PathConfig& defaults,
                                                 std::function<void(rex::PathConfig)> resume) override;
  void OnPreSetup(rex::RuntimeConfig& config) override;
  // The menu's UPSCALING and SHARPNESS, to the presenter (it read them before the menu).
  void ApplyUpscaling();
  void OnPostSetup() override;
  void OnPostLaunchModule(rex::system::XThread* thread) override;
  void OnConfigureFonts(ImFontAtlas* atlas) override;
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override;
  std::unique_ptr<rex::ui::AchievementNotificationDialog> CreateAchievementNotificationDialog() override;
};
