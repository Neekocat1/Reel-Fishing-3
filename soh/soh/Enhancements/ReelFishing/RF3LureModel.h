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
// Step 27b: a lure with a skinned copy (rf3models/lure_skin) is drawn with its bones, playing RF3's motions (at the rod
// 17; at rest 4/6/3 by its rest action; otherwise the last one keeps playing). castState: z_fishing.c's sRodCastState.
// Step 27c: hooked (castState 4/5) with hookedSlot >= 0 (the hooked region fish), the lure hangs by its hook (role 0)
// from one of the fish's 3 mouth points (picked at random when the hook is set, as RF3) and turns with the fish; its
// tie point is passed to the fish so the line ends there. frame: play->gameplayFrames.
int32_t RF3LureModel_Draw(struct PlayState* play, const float* pos, float rotY, float rotX, float zOffset,
                          int32_t castState, int32_t hookedSlot, uint32_t frame);
// Step 25d-2: a bait set's float. baitPos: the hooked bait (sLurePos); rodTip: sRodTipPos; waterY: the surface.
// With the bait under water the float sits on the surface above it; otherwise on the line toward the rod tip, the
// set's float depth away. Draws nothing for sets without a float.
// Steps 29e-g (the mod's own design; RF3 has no float in the water): on the surface it bobs and dips once per WAIT
// of the fish at the bait; from the bite (castState 4, z_fishing.c's sRodCastState) it goes under and follows the
// hooked fish on the line.
void RF3LureModel_DrawFloat(struct PlayState* play, const float* baitPos, const float* rodTip, float waterY, float rotY,
                            int32_t castState);
// Step 27c fix: if this frame's lure draw hung the lure from the fish's mouth, its line tie (world) in out; returns 1.
// z_fishing.c draws the hooked line to it; the game logic keeps the fish's snout (fishMouthPos).
int32_t RF3LureModel_LineTie(float* out);
// Step 29g-2: where the line passes through the float (its foot, world), if the float was drawn this frame
// (frame = play->gameplayFrames); returns 1. z_fishing.c draws the line rod -> float -> lure through it.
int32_t RF3LureModel_FloatLinePoint(float* out, uint32_t frame);
// Step 27e-2: this frame's lure tie (world) in any state (the model origin as drawn); returns 1 if the lure was drawn.
int32_t RF3LureModel_TieAny(float* out);
// Step 27e-1: logs `lure-tie` (once a second and on cast state changes): the line's drawn end vs this frame's lure tie
// (the gap, world units) and how far the lure's pose moved the tie off the bind origin. lineEnd: world.
void RF3LureModel_LogLineEnd(const float* lineEnd, int32_t castState, uint32_t frame);
// Step 28a: the item-get model of a tackle item: kind RF3_STOCK_LURE/FLY/BAIT/FLOAT (RF3Bridge.h) and its index,
// drawn at the current matrix (the get-item frame), centred, its longest side `size` frame units, side-on.
// Returns 0 (draw OoT's) if the item has no RF3 model.
int32_t RF3LureModel_DrawGI(struct PlayState* play, int32_t stockKind, int32_t index, float size);
// Step 35c: the item menu's preview: the item's model, slowly turning, drawn into framebuffer `fb` (width x height,
// at most 128 x 144) cleared to clearRgb. Returns 0 (fb untouched) if the item has no RF3 model.
// Step 39a: the tackle screen's rod pane, as RF3's: rod `rod` (RROD), a straight line from its tip and the lure/fly
// (stockKind RF3_STOCK_LURE/FLY, index; anything else = none) at the line's end; 39b: stockKind RF3_STOCK_BAIT =
// a bait set's layout, the bait `index` (< 0: the bare hook) and float floatIdx (< 0: none) at depth 0-4; 39b-2:
// item, float and line placed from RF3's own record for tackle set `box` (0-9; else nothing is drawn), drawn into framebuffer `fb` (width x
// height, at most 128 x 176) over `background` (a Gfx display list drawing the pane's picture into the buffer, or
// nullptr; SoH clears framebuffers opaque, so the pane can't be transparent). paneRf3 = the pane's RF3 box (x0, y0, x1, y1, 640 x 480 pixels).
// 39c: drawn in RF3's frame with RF3's camera; the item's tilt/turn fine-tuning are the sliders RodPaneItemTilt/Turn. Writes a one-line state for the log to
// stateOut. Returns 0 (fb untouched or empty) if nothing was drawn.
int32_t RF3LureModel_DrawRodPane(struct PlayState* play, int32_t fb, int32_t width, int32_t height,
                                 const void* background, const float* paneRf3, int32_t rod, int32_t box,
                                 int32_t stockKind, int32_t index, int32_t floatIdx, int32_t depth, char* stateOut,
                                 int32_t stateSize);
int32_t RF3LureModel_DrawMenuPreview(struct PlayState* play, int32_t stockKind, int32_t index, int32_t fb,
                                     int32_t width, int32_t height, uint32_t frame, const uint8_t* clearRgb);
// The last draw decisions, as logged, for the debug window.
const char* RF3LureModel_Status(void);
const char* RF3LureModel_FloatStatus(void);

#ifdef __cplusplus
}
#endif

#endif
