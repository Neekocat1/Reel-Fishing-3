#ifndef RF3_LOG_H
#define RF3_LOG_H

// Reel Fishing III mod: per-frame CSV log of the fishing pond state.
// The fishing actor fills an RF3OwnerFrame once per frame and passes it here.
// See docs/DESIGN.md (step 6) in the mod's project repository.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RF3OwnerFrame {
    uint32_t frame; // play->gameplayFrames
    int32_t castState;
    int32_t lureEquipped;
    int32_t lineHooked;
    int32_t reelLock;
    int32_t rodPullback;
    float reelSpeed;
    float lineSpooled;
    float lurePos[3];
    float rodTipPos[3];
    float waterSurfaceY;
    int32_t stickX; // relative (adjusted) stick
    int32_t stickY;
    uint32_t buttons; // buttons held this frame
    int32_t hasFish;  // 1 while a fish is on the line (cast state 4 or 5)
    int32_t fishState;
    int32_t fishStateNext;
    float fishLength; // cm, as the game stores it
    float fishPos[3];
    float fishSpeedXZ;
    float linkPos[3];
    float fishFloorY; // lake floor under the hooked fish
    int32_t fishYaw;    // step 8h: world.rot.y, binary angle units
    float fishTailStep; // step 8h: tail beat speed (phase step per game frame)
    float fishTailMag;  // step 8h: tail beat amplitude
    int32_t sceneNum;    // step 15c: the scene (the pond is SCENE_FISHING_POND)
    int32_t portable;    // step 15c: 1 = the rod used outside the pond
    int32_t lureInWater; // step 15c: the lure is over water (pond: inside its 920-unit radius)
    float lureFloorY;    // step 15c: ground under the lure (portable only)
} RF3OwnerFrame;

// True when CSV logging is switched on in the Enhancements menu.
int RF3Log_IsEnabled(void);

// Record one frame of pond state.
void RF3Log_OwnerFrame(const RF3OwnerFrame* f);

#ifdef __cplusplus
}
#endif

#endif
