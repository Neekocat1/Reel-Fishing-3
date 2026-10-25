// Reel Fishing III mod (step 25f-2): RF3's own rod models in Link's hands, read from mods/rf3_models.o2r (made from
// the user's disc by tools/rf3_models.py models). See docs/DESIGN.md "25f design".
#ifndef RF3_ROD_MODEL_H
#define RF3_ROD_MODEL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct PlayState;

// Draws the chosen box's RF3 rod at the current matrix: Fishing_DrawRod's rod frame (Link's hand, OoT's turns and
// 0.7 scale; the hand at z = 0, the tip toward +z). bendY/bendX: OoT's sideways and up/down bend (sRodBendRotY and
// the summed pull), shared out over the rod as OoT's 22 segments do with ratios[count] (sRodBendRatios). Writes the
// RF3 tip (world) to tipOut, where the line starts. Returns 1 if drawn; 0 = draw OoT's rod (RF3 off, option off,
// OoT's pole, no model, archive missing).
int32_t RF3RodModel_Draw(struct PlayState* play, float bendY, float bendX, const float* ratios, int32_t count,
                         float* tipOut);
// Step 61b: the same for rod `rod` (RROD) instead of the chosen one, without logging: a co-op ghost's rod.
int32_t RF3RodModel_DrawFor(struct PlayState* play, int32_t rod, float bendY, float bendX, const float* ratios,
                            int32_t count, float* tipOut);
// Step 28b: the item-get model of RF3 rod `rod` (RROD 0-9) at the current matrix (the get-item frame): bind pose,
// centred, horizontal, lengthWorld world units long (the frame's scale is measured). Returns 0 (draw OoT's
// pole) for OoT's pole (rod 10) or a missing model.
int32_t RF3RodModel_DrawGI(struct PlayState* play, int32_t rod, float lengthWorld);
// Step 39c: the tackle screen's rod as RF3 places it (DESIGN "Step 39 design" 39c): in RF3's own frame (x right, y
// down, z forward) at the current matrix, node T(pos) Rz Ry Rx S(scale) (rotXYZ radians, RF3's order z, y, x),
// posed with RF3's motion 30 (the rod JSON's menu_pose; the bind pose without it). Writes the tip (current-matrix
// units) to tipOut. Returns 0 for OoT's pole (rod 10) or a missing model; *posed = 1 if menu_pose was used.
int32_t RF3RodModel_DrawMenu(struct PlayState* play, int32_t rod, const float* pos, const float* rotXYZ,
                             const float* scale, float* tipOut, int32_t* posed);
// The last draw decision, as logged, for the debug window.
const char* RF3RodModel_Status(void);

#ifdef __cplusplus
}
#endif

#endif
