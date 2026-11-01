// SAINTS ROW: ReInstated (PS5) - the DualSense features (the menu's DUALSENSE
// FEATURES, the framework's gta4_sony_* cvars): the light bar in the game's
// colour (Saints Row's crimson, Saints Row 2's orange-red), dimmed with the
// player's health and blinking when it's low, a pulse when he's hit, and a light
// resistance on R2, the games' sprint, in the game only (the player's state from
// the games' scripts, sr_lua.h), through GTA IV PS5's feedback service
// (rex/input/sony_feedback.h, patched by ci/patch_framework.py). The games' own
// rumble reaches the controller as always.
#pragma once

#include <rex/platform.h>
#if REX_PLATFORM_PS5

namespace srps5 {

// Starts publishing the game's feel to the controllers (once; `game` 1 or 2).
void StartDualSense(int game);

}  // namespace srps5

#endif  // REX_PLATFORM_PS5
