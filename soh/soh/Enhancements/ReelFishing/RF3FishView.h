#pragma once
// Reel Fishing III mod (step 61, docs/DESIGN.md "Step 61 design"): the co-op fishing view. The angler's rod, line,
// lure and hooked fish, captured each frame from the fishing actor's draw so other players (Anchor) can draw a copy.
// 61a: capture and log (logs/rf3_fishview.txt). 61b: a draw-only copy (the "ghost"), and the solo mirror mode that
// draws your own view as a ghost beside you (debug window, "Co-op fishing view").

#include <stdint.h>

#include "RF3Bridge.h" // RF3TackleChoice, RF3RegionFishInfo

#ifdef __cplusplus
extern "C" {
#endif

struct PlayState;

#define RF3_FISHVIEW_LINE_POINTS 24
// Region-fish slots from here on are ghosts' fish (RF3RegionFish_Get and RF3FishModel ask RF3FishView for them).
// 64 = the mirror's; 61c: 65 on = other players'.
#define RF3_FISHVIEW_GHOST_SLOT 64
#define RF3_FISHVIEW_GHOST_SLOTS 8

// What a ghost draws: everything in world units, already placed (the mirror's shift added).
typedef struct RF3FishGhost {
    float hand[4][4];              // MtxF of the angler's hand (Player.mf_9E0), the rod's frame
    float bendY, bendX, rotX, handZ; // OoT's rod bend values (Fishing_DrawRod)
    int32_t castState;             // OoT's sRodCastState
    int32_t linkAge;
    int32_t rf3Rod;                // 1: the RF3 rod model was drawn (tackle.rod), 0: OoT's 22-segment rod
    RF3TackleChoice tackle;
    int32_t hasLure;
    float lurePos[3];
    float lureRotY, lureRotX, lureZOffset;
    int32_t lureEquipped;          // OoT's lure type (0 stock, 1 sinking)
    int32_t rf3Lure;               // 1: an RF3 model was drawn for the lure
    int32_t noLure;                // 1: the line is empty (no OoT lure either: a used-up bait, step 32a/33a)
    // what RF3LureModel drew (RF3LureModel_GetDrawn): its item kind and index (-1 = none), and its motion state
    int32_t lureKind, lureIndex;
    int32_t lureSkinned;           // drawn with bones and RF3's motions
    int32_t lureMotion, lurePrevMotion;
    float lureTime, lurePrevTime, lureBlend;
    int32_t lureHanging;           // hanging from the hooked fish's mouth point lureHookPoint
    int32_t lureHookPoint;
    int32_t lineCount;
    float line[RF3_FISHVIEW_LINE_POINTS][3]; // rod tip, drawn points, end
    int32_t floatPoint;            // index in line of the float's foot, or -1
    int32_t hasFish;
    int32_t slot;                  // the ghost's fish slot (RF3_FISHVIEW_GHOST_SLOT + n)
    float fishPos[3];
    int16_t fishRot[3];            // shape.rot plus OoT's draw offsets (x, y, z)
    int16_t yaw;                   // shape.rot.y (the motion pick's turn rate)
    int16_t bendA, bendB;
    float speedXZ;
    uint32_t frame;                // the angler's motion clock
    int32_t motionCode;            // RF3FishMove_MotionCode of the angler's fish (the fight's turn motions), -1 none
} RF3FishGhost;

// The parts are reported from z_fishing.c as they are drawn; after each frame's draw (OnGameFrameUpdate) they are put
// together into one view. A frame without a rod draw ends the view.
// Fishing_DrawRod: OoT's bend values (sRodBendRotY, the bend the rod is drawn with, D_80B7A6C0, player->unk_858), the
// cast state, Link's age, and whether the RF3 rod model was drawn (else OoT's own 22-segment rod).
void RF3FishView_SetRod(float bendY, float bendX, float rotX, float handZ, int32_t castState, int32_t linkAge,
                        int32_t rf3RodDrawn);
// Fishing_DrawLureAndLine: the lure's place and turns as drawn, OoT's lure type, whether RF3's model was drawn and
// whether the line is empty (sRF3NoLure). Right after RF3LureModel_Draw: also takes what it drew (its motion state).
void RF3FishView_SetLure(const float* pos, float rotY, float rotX, float zOffset, int32_t lureEquipped,
                         int32_t rf3Drawn, int32_t noLure);
// The line as drawn: the rod tip, then `count` points (x, y, z each), then the end. Kept as at most 24 points.
// floatAt: the index in `points` of the float's foot, or -1.
void RF3FishView_SetLine(const float* tip, const float* points, int32_t count, const float* end, int32_t floatAt);
// Fishing_DrawFishImpl, a hooked or landed region fish: its slot, place, turns (shape.rot plus OoT's draw offsets),
// yaw (shape.rot.y), swim bends, speed, OoT fish state and the motion clock.
void RF3FishView_SetFish(int32_t slot, const float* pos, const int16_t* rot, int16_t yaw, int16_t bendA, int16_t bendB,
                         float speedXZ, int32_t fishState, uint32_t frame);
// A hooked fish that isn't a region fish (the pond's): not shown in the view yet (step 61 choice C2 A), logged only.
void RF3FishView_SetPondFish(int32_t fishState);

// 61b: called from the fishing actor's draw, after your own rod and line: with the mirror on, draws your view of
// a few frames ago as a ghost, shifted to the side.
void RF3FishView_DrawMirror(struct PlayState* play);
// z_fishing.c: draws a ghost (draw only; no fishing state is read or changed).
void Fishing_RF3DrawGhost(struct PlayState* play, const RF3FishGhost* ghost);
// A ghost fish slot's record and length (RF3RegionFish_Get for slots >= RF3_FISHVIEW_GHOST_SLOT), and the angler's
// RF3 fight mode for its motions (-1 = not hooked). Return 0 for an unknown or empty ghost slot.
int32_t RF3FishView_GhostFishInfo(int32_t slot, RF3RegionFishInfo* out);
int32_t RF3FishView_GhostFightMode(int32_t slot, int32_t* mode);
// The angler's fish's fight turn motion code (RF3FishMove_MotionCode) for a ghost slot; returns 0 if not a ghost slot.
int32_t RF3FishView_GhostMotionCode(int32_t slot, int32_t* code);
// One line for the debug window.
const char* RF3FishView_Status(void);
// 61c: from DummyPlayer_Draw (Anchor), right after Player_Draw: draws that player's received view as a ghost, its rod in
// the dummy's hand matrix (Player.mf_9E0, 16 floats).
void RF3FishView_DrawRemote(struct PlayState* play, uint32_t clientId, const float* hand);

#ifdef __cplusplus
}

#include <nlohmann/json.hpp>
// 61c: another player's RF3_FISH_VIEW (Network/Anchor/Packets/RF3FishView.cpp): their view, or null when it ends.
void RF3FishView_Receive(uint32_t clientId, int32_t sceneNum, const nlohmann::json& view);
#endif
