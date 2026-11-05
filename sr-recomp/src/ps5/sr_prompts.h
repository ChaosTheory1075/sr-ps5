// SAINTS ROW: ReInstated (PS5) - PlayStation button prompts (the menu's BUTTON
// PROMPTS). Saints Row draws its button hints as characters of its own fonts (in its
// Scaleform movies), Saints Row 2 from small textures its strings name; both have a
// few more button textures. tools/prompts/make_prompts.py draws the DualSense's
// over each in the archive entry that holds it, from the user's own game files,
// into prompts<N>.bin (built into the app): the byte ranges of each archive that
// differ. As the game reads those ranges from its files, the new bytes are written
// over what was read.
#pragma once

#include <rex/platform.h>
#if REX_PLATFORM_PS5

namespace srps5 {

// Installs the file read hook (once): patches while sr_ps_prompts is on.
void InstallPrompts();

// The build has the games' text in Brazilian Portuguese (ptbr<N>.bin, sr_ptbr).
bool HasPortuguese();

}  // namespace srps5

#endif  // REX_PLATFORM_PS5
