// SAINTS ROW: ReInstated (PS5) - the in-game quick menu (Max Payne 3 PS5's and Crackdown PS5's): L3 +
// R3 together (both sticks pressed in) opens it over the game, for the cheats, the FPS counter and
// the DualSense features without going back to the app's menu. While it's open the game reads an
// idle controller; O (or L3 + R3 again) closes it. The cheats are kept in /app0/cheats<N>.ini.
//
// The cheats are the games' own script API, run in their Lua (sr_lua.h), held a few times a
// second on the player (Saints Row 2's PLAYER; Saints Row's object "alex", as its own code finds him):
//  - god mode: go_SetDamageable false, health kept full (as Saints Row 2's debug_SetPlayerInvincible);
//  - infinite ammo: go_SetGlobalInfiniteClip;
//  - enemies ignore you: ai_SetTargetable false;
//  - Saints Row 2: infinite mass (go_SetMassMeterInfinite) and one-hit kills (its debug menu's
//    Instakill, the Medusa cheat: fes_SetInstakill);
// and its one-off actions: give EP (Saints Row 2's debug menu's ep_AwardEP; Saints Row's saved EP),
// lose the alert (ai_ClearAllBlownDisguise), show/hide the HUD (fe_ToggleHideHUDOption), the time
// of day (Saints Row 2's tod_SetTimeOfDay) and unlock the Radnet events (UnlockRadnet below).
// Saints Row 2's look, under DISPLAY (kept in menu.ini): COLOUR FILTER off (no zone tints: its
// colour grades never blended in) and FOG reduced or light (its fog density scale lowered).
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

// Put up over the game, the cheats loaded and their Lua tick started (SrApp::OnCreateDialogs);
// in Saints Row 2 with the menu's RADNET EVENTS on (menu.ini radnet, the default), the Radnet
// events unlocked as the game gets under way.
void StartQuickMenu(rex::ui::ImGuiDrawer* drawer);

// Restores Saints Row 2's Radnet offline: its debug menu's "Unlock Radnet Events" (the scripts'
// fes_UnlockRadnetEventsDbg: every week's event unlock keys posted, as Radnet's server did week by
// week, and their collectible sets unlocked), run once the player is in the game. (Saints Row: none.)
void UnlockRadnet();

}  // namespace srps5

#endif  // REX_PLATFORM_PS5
