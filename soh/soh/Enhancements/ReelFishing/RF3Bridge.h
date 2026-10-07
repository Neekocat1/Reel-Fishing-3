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
    int32_t fishIndex;    // actor params - 100: picks the fish's species (step 14b)
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
// fishIndex = actor params - 100, for the fish's species (step 14b).
void RF3Fight_StartStrike(int32_t fishIndex, float fishLength);

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
    int32_t fishIndex;     // step 14b: actor params - 100
    int32_t species;       // step 14b: RF3 species id, -1 if unknown
    int32_t record;        // step 14b: fishdata.fdt record, -1 if unknown
    const char* speciesName;
    float ootLength;       // step 14d: the OoT fishLength the fight started with
} RF3FightStatus;
void RF3Fight_GetStatus(RF3FightStatus* status);

// --- Species (step 14b) -----------------------------------------------------

// At pond load: give each of the pond's fish (index 0..fishCount-1) a random RF3 species from the
// pool, and append the result to logs/rf3_species.txt.
void RF3Species_RollAll(int32_t fishCount);
// The fish's RF3 species id (rolled now if it wasn't yet).
int32_t RF3Species_Get(int32_t fishIndex);

// --- Region fish (step 16d, RF3RegionFish.cpp) --------------------------------

// Fish index 32 + slot in the bridge; actor params 400 + slot.
#define RF3_REGION_FISH_INDEX_BASE 32
typedef struct RF3RegionFishInfo {
    float minX, maxX, minZ, maxZ, surfaceY; // the region's box (in its own frame) and water surface
    float angle;                            // step 16i: the box's turn about its centre, radians
    int32_t record;                         // fishdata.fdt record (species and size class)
    float mm;                               // RF3 length, drawn at spawn
    float ootLength;                        // OoT fishLength for the model size
} RF3RegionFishInfo;
// The fish in a slot of the current scene. Returns 0 if the slot is empty.
int32_t RF3RegionFish_Get(int32_t slot, RF3RegionFishInfo* out);

// --- Size display (step 12b) ------------------------------------------------

// Step 14d (RF3Records.cpp): what OoT's sFishLengthToWeigh refers to, so the texts can show that
// fish's own RF3 length and species instead of a bass conversion.
#define RF3_WEIGH_RECORD 0  // the record for the given age (intro text, aquarium)
#define RF3_WEIGH_CAUGHT 1  // the fish just caught
#define RF3_WEIGH_ON_HAND 2 // the kept fish, at the weigh-in

// Set wherever OoT sets sFishLengthToWeigh. isChild: OoT's sLinkAge is child (picks the record).
void RF3Size_SetLengthToWeigh(float ootLength, int32_t source, int32_t isChild);
float RF3Size_GetLengthToWeigh(void);
// The RF3 length (mm) to print for the fish being weighed. Falls back to the bass conversion of the
// OoT length when no remembered fish matches it.
float RF3Size_GetWeighMm(void);
// The record's RF3 length for an age (OoT's saved record length given), same fallback.
float RF3Size_GetRecordMm(int32_t isChild, float ootLength);
// OoT kept the fish just caught (sFishOnHandLength = ootLength).
void RF3Size_KeepCaught(float ootLength);
// OoT wrote a record length into HIGH_SCORE for this age slot: remember the kept fish as that record.
void RF3Size_SetRecord(int32_t isChild, float ootLength);

// --- The rod as an item (step 15a, RF3Records.cpp) ----------------------------

// The pond owner handed Link the rod after payment: Link owns it from now on (saved per file).
// Does nothing with the RF3 option off.
void RF3Rod_SetOwned(void);
int32_t RF3Rod_IsOwned(void);
// Step 15c: one line in logs/rf3_records.txt (e.g. "portable-start", scene number).
void RF3Rod_LogEvent(const char* what, int32_t value);

// --- The mod's own records (step 16a, RF3Records.cpp) -------------------------

// A catch, once per fish (the catch message calls it; repeats of the same catch give the same answer).
// Returns 1 if it is a new record for its fishdata record (the longest fish wins; the first catch is
// one). previousMm: the old record, 0 if none.
int32_t RF3Records_SubmitCatch(int32_t record, int32_t species, float mm, int32_t weightG, float* previousMm);
// The record for a fishdata record. Returns 0 if none yet.
int32_t RF3Records_GetBest(int32_t record, float* mm, int32_t* weightG);

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
