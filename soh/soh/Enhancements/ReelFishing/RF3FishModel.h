// Reel Fishing III mod (steps 25e-2, 25g-2): RF3's own fish models for region fish, read from mods/rf3_models.o2r
// (made from the user's disc by tools/rf3_models.py models), animated with RF3's own motions. See docs/DESIGN.md
// "25e design" and "25g design".
#ifndef RF3_FISH_MODEL_H
#define RF3_FISH_MODEL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct PlayState;

// What z_fishing.c knows about the fish this frame (step 25g-2).
typedef struct RF3FishDrawInput {
    int32_t slot;      // region slot (RF3RegionFish_Get)
    int16_t bendA;     // OoT's swim angles (fishLimb23RotYDelta, fishLimb4RotYDelta), binary angle units; used
    int16_t bendB;     //   when RF3's motions are switched off (step 25e's bend)
    float speedXZ;     // actor.speedXZ, units per game frame
    int16_t yaw;       // actor.shape.rot.y, binary angle units
    int32_t hooked;    // 1: this fish is on the line (sFishingHookedFish, cast state 4/5)
    uint32_t frame;    // play->gameplayFrames: the motion clock advances once per game frame (1/20 s)
} RF3FishDrawInput;

// Draws the region fish with its record's RF3 model, at the current matrix: the fish's own frame (z_fishing.c's
// translate and turns, before its scale; front = +z). Writes the snout (RF3 locator 0) to mouthOut (x, y, z), as
// OoT's limb 13 would. Returns 1 if drawn; 0 = draw OoT's fish (RF3 off, option off, no model for the record,
// archive missing).
int32_t RF3FishModel_DrawRegion(struct PlayState* play, const RF3FishDrawInput* in, float* mouthOut);
// The last draw decision, as logged, for the debug window.
const char* RF3FishModel_Status(void);
// Step 25g-4: 1 if the region fish in slot was last drawn with RF3's motions (z_fishing.c then gives it RF3's
// struggle movement instead of step 8h's sweep).
int32_t RF3FishModel_PlaysMotions(int32_t slot);
// Step 26e-3: the movement values of a fishdata record: species speed (m/s), speed ramp (s) and the model's height
// per metre of length (fish models are 1 m long). Returns 0 if unknown.
int32_t RF3FishModel_MoveInfo(int32_t record, float* speedMps, float* rampSeconds, float* heightPerMetre);
// Step 27c: the region fish's mouth point (RF3 locator 0, 1 or 2: where RF3 puts the hook, RF3-MECHANICS 6.6) in world
// space and the fish's drawn frame (rot: its x, y, z axes in world, 3 x 3 floats), from its last draw. frame: when
// that was (play->gameplayFrames). Returns 0 if the fish wasn't drawn with an RF3 model or has no such point.
int32_t RF3FishModel_HookInfo(int32_t slot, int32_t point, float* pos, float* rot, uint32_t* frame);
// Step 27c: kept for the record; no longer read (the line's tie is drawn by z_fishing.c, RF3LureModel_LineTie).
void RF3FishModel_SetLineTie(int32_t slot, const float* tie, uint32_t frame);

#ifdef __cplusplus
}
#endif

#endif
