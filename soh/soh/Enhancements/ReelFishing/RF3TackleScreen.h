#pragma once

// Reel Fishing III mod (step 17e): RF3's tackle box screen, drawn flat over the pause menu while the
// Fishing page is shown. See RF3TackleScreen.cpp and docs/DESIGN.md step 17.

#include "z64.h"

#ifdef __cplusplus
extern "C" {
#endif

// The archive's pictures and data are loaded and complete, so the screen replaces the Fishing page.
// Without them the page keeps its step 15b look.
int RF3TackleScreen_Usable(void);
// Stick, C-Left/C-Right and A on the screen (the page's D-pad equip stays in RF3PausePage.cpp).
// Called while the Fishing page is shown and the menu is idle.
void RF3TackleScreen_HandleInput(PlayState* play);
// Step 17f: a line or lure list is open, so B cancels it instead of opening the save prompt.
int RF3TackleScreen_ListOpen(void);
// End of KaleidoScope_Draw, every pause frame: draws the screen when it should be seen.
void RF3Pause_DrawTackleScreen(PlayState* play);

#ifdef __cplusplus
}
#endif
