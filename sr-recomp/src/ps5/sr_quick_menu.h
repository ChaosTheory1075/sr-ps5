// SAINTS ROW: ReInstated (PS5) - the in-game quick menu (Max Payne 3 PS5's and Crackdown PS5's): L3 +
// R3 together (both sticks pressed in) opens it over the game, for the cheats, the FPS counter and
// the DualSense features without going back to the app's menu. While it's open the game reads an
// idle controller; O (or L3 + R3 again) closes it. The cheats are kept in /app0/cheats<N>.ini.
//
// The cheats are the games' own script API, run in their Lua (sr_lua.h), held a few times a
// second on the player (the scripts' LOCAL_PLAYER, "#PLAYER1#"):
//  - god mode: turn_invulnerable (turn_vulnerable when it's off);
//  - no police notoriety: notoriety_reset("police");
//  - enemies ignore you: set_ignore_ai_flag;
// and its one-off actions: $10,000 (cash_add), clear every gang's and the police's notoriety
// (notoriety_reset), show/hide the HUD (hud_hide).
#pragma once

#include <rex/platform.h>
#if REX_PLATFORM_PS5

#include <rex/ui/imgui_dialog.h>

namespace srps5 {

class QuickMenu final : public rex::ui::ImGuiDialog {
 public:
  explicit QuickMenu(rex::ui::ImGuiDrawer* drawer);

 protected:
  void OnDraw(ImGuiIO& io) override;
  // (every vblank only while it's open: drawn with the game's frames otherwise)
  bool WantsContinuousRepaint() const override;

 private:
  int row_ = 0;
  float shown_ = 0;  // 0..1, the fade
};

// Put up over the game, the cheats loaded and their Lua tick started (SrApp::OnCreateDialogs).
void StartQuickMenu(rex::ui::ImGuiDrawer* drawer);

}  // namespace srps5

#endif  // REX_PLATFORM_PS5
