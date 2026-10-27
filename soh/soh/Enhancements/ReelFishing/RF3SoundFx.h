#pragma once
// Reel Fishing III mod (step 63c, docs/DESIGN.md "Step 63 design", RF3-MECHANICS 9): RF3's fishing and fight sounds
// at RF3's moments, through the mod's mixer (RF3Sound.h). Only in RF3 mode; z_fishing.c mutes OoT's counterparts.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// z_fishing.c, each frame where OoT would play its reel sound (no fish on): RF3's reel loop for the rod's reel type.
// It stops on the first frame without a call.
void RF3SoundFx_Reeling(void);
// The bridge: a strike starts (RF3's bite sound).
void RF3SoundFx_Strike(void);
// The bridge, each fight sub-step (one RF3 step): the mode before and after (rf3::Mode), the sim's last run, the hook
// just set, the hook strength and the fish's fishdata record. Step 63c-3: the fish's streams are timed here.
void RF3SoundFx_FightStep(int32_t prevMode, int32_t mode, int32_t lastRun, int32_t hookSet, float hookStrength,
                          int32_t record);
// The bridge, once per fight frame: the mode, hooked, the reel held, the line's load (tension / limit), the fish's
// fishdata record and the seconds the frame covered.
void RF3SoundFx_FightFrame(int32_t mode, int32_t hooked, int32_t reelHeld, float load, int32_t record, float dt);
// The bridge: the fight's outcome (rf3::Outcome), once.
void RF3SoundFx_FightOutcome(int32_t outcome);
// The bridge: the fight ended (RF3Fight_End).
void RF3SoundFx_FightEnded(void);

#ifdef __cplusplus
}
#endif
