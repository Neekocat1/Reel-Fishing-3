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
// Debug (testing aid, user request 2026-10-07): land the hooked fish on the next fight update, through
// the sim's normal catch check. Does nothing unless a fish is hooked and the fight is still running.
// Returns 1 if a catch was queued.
int32_t RF3Fight_ForceCatch(void);

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

// --- Fly cast (step 20c-3c/d, RF3FlyCast.cpp) -----------------------------------

// The chosen box is a fly box.
int32_t RF3FlyCast_IsFlyBox(void);
// z_fishing.c: a fly cast is charging (the throw waits; z_player.c false-casts while this is 1).
void RF3FlyCast_SetCharging(int32_t charging);
int32_t RF3FlyCast_IsCharging(void);
// Throw-animation tuning, saved as CVars: 0 back frame, 1 forward frame, 2 swing speed (animation frames per
// 30 Hz frame), 3 release frame (the fly is cast when the throw reaches it).
float RF3FlyCast_Tuning(int32_t which);
void RF3FlyCast_SetTuning(int32_t which, float value);
// z_player.c, once per charge: logs the throw animation's length and frame, for tuning.
void RF3FlyCast_NoteAnim(float animLength, float curFrame);
// At the launch: the share of a full fly cast for heldFrames game frames (RF3's recorded table); logs it.
float RF3Cast_LaunchFly(int32_t heldFrames, int32_t updateRate);

// --- Water current (step 20c-3a, RF3Regions.cpp) -------------------------------

// The current at (x, z) in OoT units per second: from the scene's closest region (its set direction and speed,
// or its box's long axis at 0.2 m/s), full inside it and falling off linearly to 0 at its reach outside
// (step 20c-4); with no region, the pond's default (inPond: 0.2 m/s toward +X, [assumed]); else none.
// Returns 1 if there is a current.
int32_t RF3Current_At(int32_t inPond, int32_t scene, float x, float z, float* vx, float* vz);

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
// Step 46b: the slot's region shape (an outline with holes). Clamp: a point inside the shape near (x, z) (moved
// `margin` in from the edge when it was outside or in a hole); returns 1 if it moved, 0 if it was inside or the slot
// has no region. OutsideOutline: distance outside the outline (0 inside; holes don't count).
int32_t RF3RegionShape_Clamp(int32_t slot, float x, float z, float margin, float* outX, float* outZ);
float RF3RegionShape_OutsideOutline(int32_t slot, float x, float z);
// Step 47c: the slot's region current (step 20c-3a): the OoT yaw it flows toward (radians) and its speed (m/s); an
// unset current is the box's long axis at 0.2 m/s, as the fly drift uses. Returns 0 if the slot has no region.
int32_t RF3RegionFish_Current(int32_t slot, float* yawRad, float* speedMps);

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

// --- The tackle stock (step 21a, RF3Stock.cpp) --------------------------------
// What Link owns, by RF3 index: collected (0/1) and a count (0-9), as RF3's tackle manager keeps it
// (docs/RF3-MECHANICS.md 5.1). Saved per file. Step 21a only records it; nothing reads it yet.

#define RF3_STOCK_ROD 0   // RROD 0-9, 10 = OoT's pole (step 23c)
#define RF3_STOCK_LURE 1  // RLRE 0-90, 91/92 = OoT's lures (step 23c)
#define RF3_STOCK_FLY 2   // RFLY 0-39
#define RF3_STOCK_BAIT 3  // bait type 0-11 (all 5 hook sizes)
#define RF3_STOCK_FLOAT 4 // RFLT 0-5
#define RF3_STOCK_KINDS 5

int32_t RF3Stock_Size(int32_t kind);
// Step 24d: does Link have any rod (RF3's or OoT's pole)? The rod item counts as owned then.
int32_t RF3Stock_AnyRod(void);
int32_t RF3Stock_Has(int32_t kind, int32_t index);
int32_t RF3Stock_Count(int32_t kind, int32_t index);
// Collects an item: rods and floats count 1, baits 9, lures and flies +1 up to 9. Logged. Returns 0
// for a bad kind/index.
int32_t RF3Stock_Give(int32_t kind, int32_t index, const char* why);
// The bass rod's kit (Lure Rod/Bass + Suspended Vibration G), given when the rod is bought, and to an
// older save that owns the rod but has no stock yet.
void RF3Stock_GiveStarter(const char* why);
// Step 21b: a fish was caught (once per catch, from RF3Records_SubmitCatch): unlock one item, the category
// by catch count mod the categories left, usable first. Returns 1 if an item was given.
// Step 21c: first the catch counts toward the user's rod conditions (with the rod in use), and any rod
// earned is given.
int32_t RF3Stock_UnlockForCatch(int32_t record, int32_t species, float lengthMm, int32_t weightG);
// Step 43a: the lure unlock conditions' events. A hooked fish was lost (line break, hook out, line reeled in with a
// fish on); the lure landed in the water (cast state 2 -> 3); a fish bit (the strike). Frames are play->gameplayFrames.
void RF3Stock_FishLost(const char* why);
void RF3Stock_NoteLureLanded(uint32_t frame);
void RF3Stock_NoteBite(uint32_t frame);
// The rods the latest catch earned (up to max); returns how many.
int32_t RF3Stock_LastRods(int32_t* rods, int32_t max);
typedef struct {
    int32_t total; // step 23d: any fish (Lure Rod/Bass at 5)
    int32_t bass, trout, salmon, withFlyRod, withPoleS;
    int32_t largemouthOver14in, paleChub, bitterlingOver3in, bitterling;
} RF3RodProgress;
void RF3Stock_GetRodProgress(RF3RodProgress* out);
// Step 21e: the line broke: the chosen box's lure/fly (or a bait set's bait) loses 1 until the next scene
// change (then it comes back; baits are refilled to 9). Returns 1 if something was taken.
int32_t RF3Stock_LoseCurrent(void);
// Step 33a (RF3-MECHANICS 5.4): a bite (why "strike") or a give-up at the bait ("give-up") uses 1 of the open bait
// set's bait and empties the hook. 0 if not a bait set or no bait on the hook.
int32_t RF3Stock_UseBait(const char* why);
int32_t RF3Stock_LastLost(int32_t* kind, int32_t* index); // what the latest line break took; 0 if nothing
int32_t RF3Stock_Lost(int32_t kind, int32_t index);       // how many are waiting to come back
// What the latest catch unlocked; returns 0 (kind/index -1) if nothing.
int32_t RF3Stock_LastUnlock(int32_t* kind, int32_t* index);
uint32_t RF3Stock_CatchCount(void);
// An item's name from tackle.json, or "#index". Valid until the next call.
const char* RF3Stock_Name(int32_t kind, int32_t index);
const char* RF3Stock_KindName(int32_t kind); // "rod", "lure", "fly", "bait", "float"
const char* RF3Stock_BoxTitle(int32_t kind, int32_t index); // step 22b: first box listing it, "" if none

// --- The mod's own records (step 16a, RF3Records.cpp) -------------------------

// A catch, once per fish (the catch message calls it; repeats of the same catch give the same answer).
// Returns 1 if it is a new record for its fishdata record (the longest fish wins; the first catch is
// one). previousMm: the old record, 0 if none.
int32_t RF3Records_SubmitCatch(int32_t record, int32_t species, float mm, int32_t weightG, float* previousMm);
// The record for a fishdata record. Returns 0 if none yet.
int32_t RF3Records_GetBest(int32_t record, float* mm, int32_t* weightG);

// --- The tackle choice (step 17c, RF3Tackle.cpp) -----------------------------

// RF3's tackle boxes in this mod. The box decides the rod (box n = RROD n) and the reel.
#define RF3_TACKLE_BOX_BASS 0
#define RF3_TACKLE_BOX_TROUT 1
#define RF3_TACKLE_BOXES 11 // RF3's 10 boxes (step 19) + OoT's pole (box 10, step 23c); box n uses rod n
// How many boxes the loaded tackle.json has (step 19a: the 5 lure and fly boxes), 2 without it.
int32_t RF3Tackle_BoxCount(void);

// All indices are RF3's own: line = rigdata RLIN record, lure = RLRE record, target = fishdata record.
typedef struct RF3TackleChoice {
    int32_t box;    // RF3_TACKLE_BOX_*
    int32_t rod;    // RROD record of the box
    int32_t line;   // the box's chosen line
    int32_t lure;   // the box's chosen lure
    int32_t target; // the target fish (shared by both boxes)
    // Step 19c, bait/pole sets: RFLT float, float depth 0..4 (Surface .. Deepest), RBIT bait group
    // (-1 = none, as RF3's "---"), hook size 0..4 (small .. big).
    int32_t floatIdx;
    int32_t depth;
    int32_t bait;
    int32_t size;
} RF3TackleChoice;
void RF3Tackle_Get(RF3TackleChoice* out);
int32_t RF3Tackle_GetBoxLine(int32_t box);
int32_t RF3Tackle_GetBoxLure(int32_t box);
// Setters return 0 for an index out of range (nothing changes). A real change writes a
// "tackle-select" line to logs/rf3_records.txt with why (e.g. "debug-window"). Saved per file;
// the fishing does not use the choice yet.
int32_t RF3Tackle_SetBox(int32_t box, const char* why);
int32_t RF3Tackle_SetLine(int32_t box, int32_t line, const char* why);
int32_t RF3Tackle_SetLure(int32_t box, int32_t lure, const char* why);
int32_t RF3Tackle_SetTarget(int32_t record, const char* why);
// Step 19c, bait/pole sets.
int32_t RF3Tackle_SetFloat(int32_t box, int32_t floatIdx, const char* why);
int32_t RF3Tackle_SetDepth(int32_t box, int32_t depth, const char* why);
int32_t RF3Tackle_SetBait(int32_t box, int32_t bait, const char* why);
int32_t RF3Tackle_SetSize(int32_t box, int32_t size, const char* why);

// Step 21d: only owned tackle (RF3Tackle.cpp). The stock loaded or changed: from now on the choices are
// judged by it (a box needs its rod; a lure, fly, float or bait must be owned with a count above 0), and
// any choice that isn't owned moves to the first owned one or unset ("tackle-fix" in the log).
void RF3Tackle_StockChanged(const char* why);
int32_t RF3Tackle_BoxOwned(int32_t box);
#define RF3_TACKLE_ROW_ITEM 1  // a lure or fly
#define RF3_TACKLE_ROW_FLOAT 2
#define RF3_TACKLE_ROW_BAIT 4  // -1 ("---") is always allowed
int32_t RF3Tackle_ItemOwned(int32_t box, int32_t rowKind, int32_t index);
// Can Link cast with the current choice? If not, reason = RF3's text ("Lure not selected." etc.).
int32_t RF3Tackle_Ready(const char** reason);
// Step 32a: 1 if the open box is a lure/fly box with no owned lure/fly on the line (after a line break): nothing is
// drawn at the line's end.
int32_t RF3Tackle_LureMissing(void);
// Step 33a: a bite or a give-up took the open bait set's bait: its bait becomes "---" (RF3Stock_UseBait calls it).
void RF3Tackle_BaitTaken(const char* why);
// Step 33a: on a scene change (after the bait refill), sets whose bait was taken get the same bait back on.
void RF3Tackle_SceneRestore(void);
// Step 33b: the open bait set's hook model index (model/hook/hookNNN), or -1.
int32_t RF3Tackle_HookIndex(void);
// Step 23c: OoT's pole (box 10) uses OoT's own lure movement. -1 if another box is chosen, else 1 for the
// sinking lure (92) and 0 for the normal lure (91).
int32_t RF3Tackle_PoleLure(void);
// Step 21d: z_fishing.c refused a cast; the next "fish got away" text (0x4082) shows this reason instead.
void RF3Tackle_ShowNotReady(const char* reason);

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
    float sightRangeM;    // for the chaser (or a normal adult bass), with the chosen tackle (step 18c)
    int32_t waryFish;     // how many fish are wary
    int32_t activityLevel;
    int32_t sightBonus;   // step 18c: heart sum used for that range
    int32_t lureAppeal;   // step 18c: the lure's appeal for that fish (15 = no change)
    int32_t blockedRod;   // step 18c: fish this visit that can't take the rod (RF3's rod mask)
    int32_t blockedAppeal; // step 18c: fish this visit whose appeal for the lure is 0
    // Step 20b-6: diagnostics for bait fishing (logged to the CSV).
    int32_t rodKind;       // rf3::RodKind of the chosen box: 0 lure (flies too), 2 bait
    int32_t chaserRecord;  // the chaser's fishdata record, -1 = no chaser
    int32_t chaserC;       // its record +0x36 (bait strike rule, WAIT length; RF3-MECHANICS 2.9)
    int32_t chaserWaits;   // WAITs in this chase
    int32_t chasePingAgo;  // game frames since the chaser's last RF3Approach_Chase call (1 = every frame)
} RF3ApproachStatus;
void RF3Approach_GetStatus(RF3ApproachStatus* status);

// --- Lure depth (step 20a-5, RF3-MECHANICS 3.2; RF3LureDepth.cpp) ---------

// At the splash. Starts the lure's depth when RF3 is on, the chosen box is a lure box and its lure has
// movement data; logs a "lure-move" line. Returns 1 if active (else the lure keeps OoT's surface lure).
int32_t RF3Lure_Begin(void);
int32_t RF3Lure_IsActive(void);
// Step 20c-3b: the active rig: 0 lure, 1 fly, 2 bait, -1 none.
int32_t RF3Lure_Kind(void);
// Step 20c-4: the share of the region's current that moves the active rig at rest (flies and bait 1).
float RF3Lure_DriftScale(void);
// Once per game frame while the lure is in the water. reeling: A held. stickY: OoT rel.stick_y
// (negative = pulled back; below -48 = RF3's rod held back, which twitches the lure).
// floorDepthM: m below the surface where the lure rests on the bottom.
// substeps: RF3 steps (1/60 s) per game frame. Returns the depth below the surface in m.
float RF3Lure_Step(int32_t reeling, float stickY, float floorDepthM, int32_t substeps);
// The cast ended (fish on, reeled in, lure on land).
void RF3Lure_End(void);

typedef struct RF3LureStatus {
    int32_t active;
    int32_t kind;    // step 20b-5: 0 lure, 1 fly (20c-3b), 2 bait
    int32_t lure;    // RLRE record (a bait: the RBIT bait group), -1 = none
    float depthM;    // below the surface
    float t;         // s, the lure timer
    int32_t mode;    // 1 reeling, 2 resting
    int32_t twitch;  // 1 during a twitch
    int32_t bottom;  // 1 on the bottom
    int32_t stopped; // 1 held by the [assumed] dive stop
    int32_t restAction; // step 27b: the lure's RLRE +0x0E (0/3/5 sink, 1/2/4 float up); -1 = not a lure
    int32_t atSurface;  // step 27b: 1 at the surface (RF3's lure +0x70 "may go up" is 0 there)
} RF3LureStatus;
void RF3Lure_GetStatus(RF3LureStatus* status);

#ifdef __cplusplus
}
#endif

#endif
