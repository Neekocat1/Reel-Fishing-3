// Reel Fishing III mod (step 47b): RF3's free swimming for a region fish that isn't hooked (RF3-MECHANICS 6.9,
// docs/DESIGN.md "Step 47 design"). Pure: no SoH calls, so the offline harness (tools/rf3sim) runs it too.
// The cycle: hover (0) -> calm swim (1, 70 %) or fast swim (2, 30 %) -> glide (3) -> hover. Fish of rank 4+ only
// hover; fish that face the current skip the glide. Runs RF3's 1/60 s ticks, `substeps` per game frame.
// World units and OoT's conventions: y up, yaw = direction (sin yaw, cos yaw), pitch + = nose up.
#ifndef RF3_FISH_SWIM_H
#define RF3_FISH_SWIM_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { RF3_SWIM_HOVER = 0, RF3_SWIM_CALM = 1, RF3_SWIM_FAST = 2, RF3_SWIM_GLIDE = 3 };

// Why the previous mode ended (RF3FishSwimOutput.endWhy)
enum { RF3_SWIM_END_NONE = 0, RF3_SWIM_END_TIME = 1, RF3_SWIM_END_REACHED = 2, RF3_SWIM_END_BLOCKED = 3,
       RF3_SWIM_END_STOPPED = 4 };

// The species' values (RF3FishTable.h kFishSwim, from fishdata.fdt)
typedef struct RF3FishSwimSpecies {
    float speedMps;       // record +0x14 x 0.075
    float speedUpS;       // fish +0x2AC, by record +0x13
    float slowDownS;      // fish +0x2B0
    float homeRadiusM;    // record +0x18
    int32_t calmness;     // record +0x36
    int32_t rank;         // record +0x3B (4+ = hovers only)
    int32_t facesCurrent; // record +0x2C bit 0
} RF3FishSwimSpecies;

typedef struct RF3FishSwimInput {
    int32_t slot;  // the per-fish state is kept per slot
    int32_t reset; // 1 = start (again) in hover, heading yaw/pitch: spawn, back from the approach or a fight
    RF3FishSwimSpecies species;
    float lengthM;  // body length L (m)
    float heightM;  // body height H (m)
    int32_t lengthMm;
    float unitsPerMetre;
    float pos[3];     // the fish now (world)
    float yaw;        // heading, read on reset only (afterwards the module keeps it)
    float pitch;
    float home[3];    // the spawn point (RF3 fish +0x80)
    float bandTop;    // the depth band (world y, top > bottom): targets stay in it (RF3_FishSwim_DepthBand)
    float bandBottom;
    float waterY;     // surface at the fish
    float floorY;     // ground under the fish; below -30000 = none
    float currentYaw; // the region's current: OoT yaw it flows toward (rad), and its speed (0 = no flow)
    float currentSpeedMps;
    float hp; // 0..100 (100 for a fish that wasn't fought)
    int32_t substeps;
    uint32_t frame; // step 47d: play->gameplayFrames (RF3FishSwim_Motion tells the model whether the fish swam now)
    // RF3's obstacle probe (RF3-MECHANICS 6.4), a line from `from` to `to`: 0 clear, 1 hit with its unit normal,
    // 2 blocked (no normal). May be null (open water).
    int32_t (*probe)(const float from[3], const float to[3], float normal[3]);
    // The region's shape: moves (x, z) inside it (step 46b). May be null.
    void (*clampXZ)(float x, float z, float* outX, float* outZ);
    // A turn-on-the-spot pattern (codes 63-66): its yaw (RF3 sense, + = right) and length. May be null (then 90/180
    // deg over 1.5 s, the recorded length).
    int32_t (*codeTurn)(int32_t code, float* yawDeg, float* seconds);
    void (*log)(const char* line); // may be null
} RF3FishSwimInput;

typedef struct RF3FishSwimOutput {
    float step[3]; // units to move this game frame (RF3 moves along the direction to its target, not the heading)
    float yaw;     // heading after this frame
    float pitch;
    int32_t mode;
    float modeTime;   // s in this mode
    float hoverLen;   // the hover's planned length (s)
    float speed;      // units per tick (the hover's drift included)
    float target[3];
    int32_t code;     // RF3 motion request code now (2, 3, 4, 20, 63-66); -1 none yet
    int32_t rawAnim;  // a raw animation set straight on the model over the code (hover turns 25/26/5/6, 2), -1 none
    float blend;      // s, for the latest request
    int32_t modeChanged; // a mode started this frame
    int32_t endWhy;      // why the mode before it ended
    int32_t prevMode;
    float prevModeTime;
} RF3FishSwimOutput;

void RF3FishSwim_Step(const RF3FishSwimInput* in, RF3FishSwimOutput* out);

// Step 49d: RF3's CHASE and WAIT for a region fish going for the lure (RF3-MECHANICS 2.5/2.6, 0x001A0B00, 0x001A21C0).
// The modes share the slot's swim state (heading, speed); the free swim starts again in hover afterwards (its reset).
// Step 49e: HIT (13), a hooked fish from the bite to the hook set (0x001A1820): it steers to its home point (in.home)
// as CHASE steers, a blocked probe picks a new target, and at the hook window's centre it holds still for "Kuh!" (28).
enum { RF3_SWIM_CHASE = 10, RF3_SWIM_CHASE_LOST = 11, RF3_SWIM_WAIT = 12, RF3_SWIM_HIT = 13 };
enum { RF3_CHASE_GOING = 0, RF3_CHASE_REACHED = 1, RF3_CHASE_BLOCKED = 2, RF3_CHASE_STUCK = 3 };
typedef struct RF3FishChaseInput {
    int32_t want;      // RF3_SWIM_CHASE (interested), RF3_SWIM_CHASE_LOST (not interested), RF3_SWIM_WAIT, RF3_SWIM_HIT
    float lure[3];     // the lure (world); not used in HIT
    float waitSeconds; // the WAIT's length, read when a WAIT starts (its back-off is spread over it)
    float kuhAt;       // HIT: s after the bite when "Kuh!" plays (the hook window's centre)
    float kuhSeconds;  // HIT: how long it holds still for it
} RF3FishChaseInput;
// One game frame of the chase. Uses `in` as RF3FishSwim_Step does (home, band, current, clampXZ are not used).
// Returns RF3_CHASE_*: reached the lure (RF3's strike or WAIT follows), blocked or stuck (the chase fails).
int32_t RF3FishSwim_Chase(const RF3FishSwimInput* in, const RF3FishChaseInput* chase, RF3FishSwimOutput* out);

void RF3FishSwim_Forget(int32_t slot);
// Step 47c: the record's species values (RF3FishTable.h kFishSwim) and depth layers. Returns 0 for an unknown record.
int32_t RF3FishSwim_Species(int32_t record, RF3FishSwimSpecies* out, int32_t* layerMin, int32_t* layerMax);
// Step 47d: the motion the slot's free swim asks for: RF3's request code and the raw animation set over it (-1 none)
// and the blend (s). Returns 0 if the slot didn't free-swim on `frame` (or the frame before).
int32_t RF3FishSwim_Motion(int32_t slot, uint32_t frame, int32_t* code, int32_t* rawAnim, float* blend);
// Step 47e: a region fish's body this frame (every region fish, hooked or chasing too), for the push-off test:
// position (world), heading (yaw, pitch + = up) and length (m)
void RF3FishSwim_Body(int32_t slot, const float pos[3], float yaw, float pitch, float lengthM, uint32_t frame);
// Step 47e: push-off on or off (CVar ReelFishing3.FishPushOff; the harness compares both)
void RF3FishSwim_SetPushOff(int32_t on);
// Step 47e: do these two slots' bodies overlap now (RF3's two-sphere test, as seen by slot a)?
int32_t RF3FishSwim_BodiesOverlap(int32_t a, int32_t b);
// RF3's random generator is seeded per run (the harness sets it for repeatable runs)
void RF3FishSwim_Seed(uint32_t seed);
// The depth band (spawner 0x0018CA10, OoT y up). The water column under the spot's centre (surface down to
// centreFloorY, minus H) is split into 6 layers; the band is layers layerMin..layerMax, the top kept H below the
// layer edge; the floor at the spawn spot (spotFloorY) can raise the bottom to that floor + H.
void RF3FishSwim_DepthBand(float waterY, float centreFloorY, float spotFloorY, int32_t layerMin, int32_t layerMax,
                           float heightUnits, float* top, float* bottom);

#ifdef __cplusplus
}
#endif
#endif
