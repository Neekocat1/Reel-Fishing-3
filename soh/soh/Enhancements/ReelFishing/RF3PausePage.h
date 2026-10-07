#ifndef RF3_PAUSE_PAGE_H
#define RF3_PAUSE_PAGE_H

// Reel Fishing III mod (step 15b): the pause menu's Fishing page, shown in place of the Equipment
// page after a C-Up tap. See RF3PausePage.cpp and docs/DESIGN.md step 15.

#include "z64.h"

#ifdef __cplusplus
extern "C" {
#endif

// True while the Fishing page replaces the Equipment page (RF3 option on).
int RF3Pause_FishingPageActive(void);
// Pause state 6 with no prompt: a C-Up tap on the Equipment page toggles the Fishing page.
void RF3Pause_UpdateToggle(PlayState* play);
// Draws the page's entries after the caller drew the blank page sections. handleInput: the page is
// the one shown and takes input (cursor, D-pad equip).
void RF3Pause_DrawFishingPage(PlayState* play, int handleInput);
// Step 17e: back to the Equipment page (the tackle box screen's Finish).
void RF3Pause_CloseFishingPage(void);

#ifdef __cplusplus
}
#endif

#endif
