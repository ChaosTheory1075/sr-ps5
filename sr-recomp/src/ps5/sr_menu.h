// SAINTS ROW: ReInstated (PS5) - the menu the app opens on (sr_menu.cpp),
// laid out as Crackdown PS5's and GTA IV PS5's hub: its pages HOME (PLAY),
// SETTINGS (anti-aliasing, texture filtering, the GPU helper, the menu music's
// level), TROPHIES (the achievements, earned and not: sr_trophies.h) and
// CREDITS, over hub/background.jpg with the menu song (hub/music/) playing.
#pragma once

#include <rex/platform.h>
#if REX_PLATFORM_PS5

#include <cstdint>
#include <filesystem>
#include <functional>

struct ImFontAtlas;
struct ImFont;
namespace rex::ui {
class ImGuiDrawer;
class ImmediateDrawer;
}  // namespace rex::ui

namespace srps5 {

// What the menu chose, for this start.
struct MenuChoice {
  uint32_t game = 1;  // (1: Saints Row; Saints Row 2 is started by the menu itself, as sr2.bin)
};

// The menu's fonts (from OnConfigureFonts), and two of them for the trophy popup.
void ConfigureMenuFonts(ImFontAtlas* atlas);
ImFont* MenuBodyFont();
ImFont* MenuItemFont();

// menu.ini's settings applied (the cvars), for sr2.bin, which starts without the menu.
void ApplySettings(const std::filesystem::path& app);

// Shows the menu; `play` runs (on the UI thread) on PLAY, its settings applied
// (the frame rate's and HD textures' cvars) and saved in app/menu.ini.
void ShowMenu(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate, const std::filesystem::path& app,
              std::function<void(MenuChoice)> play);

}  // namespace srps5

#endif  // REX_PLATFORM_PS5
