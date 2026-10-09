// Reel Fishing III mod (steps 25c-2, 25d-2): RF3's own lure, fly, bait and float models on the line, read from
// mods/rf3_models.o2r (made from the user's disc by tools/rf3_models.py models). See docs/DESIGN.md "25c design"
// and "25d design".
#ifndef RF3_LURE_MODEL_H
#define RF3_LURE_MODEL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct PlayState;

// Draws the chosen rig's item (a lure box's lure, a fly box's fly, a bait set's bait) at pos (x, y, z):
// lures and flies along the line (rotY, rotX: z_fishing.c's sLureRot, front = +z, plus OoT's sLurePosZOffset in
// its 0.004-scaled model units), bait upright (it hangs under the float), turned by rotY.
// Returns 1 if an RF3 model was drawn; z_fishing.c then skips OoT's lure and hook models.
// Returns 0 (draw OoT's lure) when RF3 is off, the option is off, nothing is chosen, the item has no model (OoT's
// pole lures 91/92), or rf3_models.o2r is missing.
int32_t RF3LureModel_Draw(struct PlayState* play, const float* pos, float rotY, float rotX, float zOffset);
// Step 25d-2: a bait set's float. baitPos: the hooked bait (sLurePos); rodTip: sRodTipPos; waterY: the surface.
// With the bait under water the float sits on the surface above it; otherwise on the line toward the rod tip, the
// set's float depth away. Draws nothing for sets without a float.
void RF3LureModel_DrawFloat(struct PlayState* play, const float* baitPos, const float* rodTip, float waterY, float rotY);
// The last draw decisions, as logged, for the debug window.
const char* RF3LureModel_Status(void);
const char* RF3LureModel_FloatStatus(void);

#ifdef __cplusplus
}
#endif

#endif
