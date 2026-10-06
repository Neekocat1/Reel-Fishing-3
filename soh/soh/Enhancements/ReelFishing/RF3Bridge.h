#ifndef RF3_BRIDGE_H
#define RF3_BRIDGE_H

// Reel Fishing III mod: C interface between z_fishing.c and the fight
// simulation (RF3Sim). See docs/DESIGN.md step 8 in the project repository.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// OoT units per RF3 metre: RF3's full cast (11.7 m) = OoT's cast (794 units).
#define RF3_UNITS_PER_METRE 68.0f

// Outcome codes, same values as rf3::Outcome.
#define RF3_OUTCOME_NONE 0
#define RF3_OUTCOME_CAUGHT 1
#define RF3_OUTCOME_LINEBREAK 2
#define RF3_OUTCOME_HOOKOUT 3
#define RF3_OUTCOME_MISSED 4
#define RF3_OUTCOME_BUKKONUKI 5

typedef struct RF3FightInput {
    int32_t reelHeld;     // A held
    float stickX;         // OoT rel.stick_x
    float stickY;         // OoT rel.stick_y (negative = stick pulled down = rod raised)
    float fishDistUnits;  // horizontal Link-to-fish distance, OoT units
    float lineMaxUnits;   // longest line, OoT units
    float catchDistUnits; // OoT's catch radius, OoT units
    float fishLength;     // OoT fishLength, used to pick the size class
    float fishY;          // fish height, OoT units (debug display only)
    float waterY;         // water surface height, OoT units (debug display only)
    float floorY;         // lake floor under the fish, OoT units (debug display only)
    float lureDepthUnits; // lure below the water surface, OoT units (perfect hook set check)
    int32_t substeps;     // RF3 steps (1/60 s) per game frame: OoT's R_UPDATE_RATE, 3 in normal play
} RF3FightInput;

typedef struct RF3FightOutput {
    float targetDistUnits; // where the fish should be, OoT units from Link
    int32_t mode;          // rf3::Mode value
    int32_t outcome;       // RF3_OUTCOME_*
    float tension;         // for rod bend, rumble and reel sound
    float tensionLimit;
    int32_t hooked;        // 1 once the hook is set (after HIT)
    int32_t hookWindowOpen;   // 1 while in HIT and inside the hook-set window
    int32_t hookWindowFrames; // length of the hook-set window, frames
    int32_t shakeSide;        // STRUGGLE: 0/1, flips every 0.25 s (for the thrash, step 8g)
} RF3FightOutput;

// True when "Reel Fishing III Mechanics" is on.
int RF3_IsEnabled(void);

// True while a strike or fight is running.
int RF3Fight_IsActive(void);

// A fish has bitten: start Reel Fishing III's hook-set timer (state HIT).
void RF3Fight_StartStrike(float fishLength);

// Start a fight if none is active (call on each hooked frame). Then advance it one frame.
// Returns the outcome; any outcome other than NONE ends the fight.
int32_t RF3Fight_Update(const RF3FightInput* in, RF3FightOutput* out);

// Abandon the current fight (fish lost by OoT's own rules, area left, ...).
void RF3Fight_End(void);

// For the CSV log: snapshot of the running fight. Returns 0 if none is active.
typedef struct RF3FightStatus {
    int32_t active;
    int32_t mode;
    int32_t lastRun;
    float hp;
    float hook;
    float tension;
    float tensionLimit;
    float distanceM;
    int32_t outcome;
    // For the debug window.
    float stateTime;
    float stateDuration;
    float stopWindowEnd;
    int32_t stopExpectsReel;
    float actualDistanceM; // fish's horizontal distance from Link
    float landingDistanceM;
    float fishDepthUnits;  // below the water surface (negative = above)
    float waterDepthUnits; // surface to lake floor under the fish
    int32_t reelHeld;      // inputs as the fight read them (RF3 convention)
    float stickX;
    float stickY;
    const char* fishName;
    int32_t hooked;
    float hookWindowStart; // s after the strike
    float hookWindowEnd;
    float hookGiveUpTime;
    float hookSetTiming;   // "a" of the hook set, 0..1
    float hookSetStrength; // "b" of the hook set, 0..1
    float lengthMm;        // RF3 length of this fish (step 12)
    int32_t weightG;       // RF3 weight of this fish (step 12)
} RF3FightStatus;
void RF3Fight_GetStatus(RF3FightStatus* status);

// --- Size display (step 12b) ------------------------------------------------

// The pond owner's fish being weighed (OoT's sFishLengthToWeigh), set each frame by the owner.
void RF3Size_SetLengthToWeigh(float ootLength);
float RF3Size_GetLengthToWeigh(void);

// --- Cast (step 11) ---------------------------------------------------------

// At the launch frame: B was held for heldFrames of a windupFrames-long wind-up (game frames,
// updateRate RF3 steps each). Returns the share of OoT's full cast speed to use (0.15..1).
float RF3Cast_Launch(int32_t heldFrames, int32_t windupFrames, int32_t updateRate);
// Last cast, for the debug window and log.
void RF3Cast_GetLast(float* power, float* distanceFactor);

// --- Approach (step 10): notice, chase, strike ---------------------------------

#define RF3_APPROACH_NONE 0
#define RF3_APPROACH_STRIKE 1
#define RF3_APPROACH_FAIL 2

// Once per frame (pond owner): the shared lure action meter, and each fish's memory.
// substeps: RF3 steps (1/60 s) per game frame (OoT's R_UPDATE_RATE).
void RF3Approach_UpdateMeter(int32_t lureInWater, float lureMovedUnits, int32_t reelHeld, int32_t substeps);
// An idle fish looks at the lure. fishIndex = actor params - 100. Positions in OoT units.
// angleFromHeading: lure direction relative to the fish's heading, radians.
// Returns 1 if this fish starts chasing.
int32_t RF3Approach_Notice(int32_t fishIndex, float fishLength, float fishToLureUnits, float fishToLureXZUnits,
                           float angleFromHeading);
// The chasing fish, every frame until the bite. Returns RF3_APPROACH_*.
int32_t RF3Approach_Chase(int32_t fishIndex, int32_t lureInWater, int32_t lureNearAngler, int32_t reachedLure,
                          int32_t substeps);
void RF3Approach_SetWary(int32_t fishIndex, float seconds);
void RF3Approach_EndChase(int32_t fishIndex);
// New pond visit.
void RF3Approach_Reset(void);

typedef struct RF3ApproachStatus {
    float meter;
    int32_t chaser;       // fish index, -1 = none
    int32_t chaserMode;   // 0 NORMAL, 1 CHASE, 3 WAIT
    float chaserMemory;   // remembered lure action, 0..1
    int32_t chaserInterested;
    float chaserWaitLeft; // s
    float sightRangeM;    // for the chaser (or a normal adult bass)
    int32_t waryFish;     // how many fish are wary
    int32_t activityLevel;
} RF3ApproachStatus;
void RF3Approach_GetStatus(RF3ApproachStatus* status);

#ifdef __cplusplus
}
#endif

#endif
