// Reel Fishing III mod (step 25e-2): RF3's own fish models for region fish, read from mods/rf3_models.o2r (made from
// the user's disc by tools/rf3_models.py models). See docs/DESIGN.md "25e design".
#ifndef RF3_FISH_MODEL_H
#define RF3_FISH_MODEL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct PlayState;

// Draws the region fish in slot (RF3RegionFish_Get) with its record's RF3 model, at the current matrix: the fish's
// own frame (z_fishing.c's translate and turns, before its scale; front = +z). bendA/bendB: OoT's swim angles
// (fishLimb23RotYDelta, fishLimb4RotYDelta, binary angle units), which bend the RF3 spine. Writes the snout (RF3
// locator 0) to mouthOut (x, y, z), as OoT's limb 13 would. Returns 1 if drawn; 0 = draw OoT's fish (RF3 off,
// option off, no model for the record, archive missing).
int32_t RF3FishModel_DrawRegion(struct PlayState* play, int32_t slot, int16_t bendA, int16_t bendB, float* mouthOut);
// The last draw decision, as logged, for the debug window.
const char* RF3FishModel_Status(void);

#ifdef __cplusplus
}
#endif

#endif
