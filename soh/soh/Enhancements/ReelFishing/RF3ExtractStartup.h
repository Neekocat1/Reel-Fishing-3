#pragma once

// Reel Fishing III mod (step 77c): the RF3 setup in SoH's startup extractor, the way oot.o2r is made
// (docs/DESIGN.md "Step 77 design"). OTRGlobals::RunExtract runs it after its own ROM step and before any archive or
// mod is loaded, so archives built here load on the same launch. Also the disc picker shared with the Disc Setup
// window (RF3ExtractWindow).

#include <string>

#include "soh/cvar_prefixes.h"

#define CVAR_RF3_DISC_PATH CVAR_ENHANCEMENT("ReelFishing3.DiscPath")
#define CVAR_RF3_BUILD_MUSIC CVAR_ENHANCEMENT("ReelFishing3.BuildMusic")       // default 1
#define CVAR_RF3_SETUP_NO_PROMPT CVAR_ENHANCEMENT("ReelFishing3.SetupNoPrompt") // "Don't ask again"

namespace rf3extract {

// The file dialog, on its own thread (it blocks until the player chooses) so the game keeps drawing.
void PickDisc();
bool PickingDisc();
// Once a frame, on the main thread: stores a newly chosen disc path in its CVar and saves the CVars.
void ApplyPickedDisc();

// RunExtract's step: true when the RF3 setup is finished (nothing needed, built, skipped or declined).
bool StartupFinished();
// Draws the setup's modal; called inside RunExtract's frame while the step runs.
void StartupDraw();

} // namespace rf3extract
