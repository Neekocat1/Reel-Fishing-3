// Reel Fishing III mod (step 26e-3): RF3's own movement for a hooked region fish drawn with RF3's model and motions.
// See docs/DESIGN.md "26e plan" and RF3-MECHANICS 6.3. The RF3 fight sim still decides the mode and the distance;
// this decides where the fish swims, how fast and how it turns.
#ifndef RF3_FISH_MOVE_H
#define RF3_FISH_MOVE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RF3FishMoveInput {
    int32_t slot;       // region slot (the per-fish state is kept per slot)
    int32_t record;     // fishdata record (species speed, size)
    float mm;           // RF3 length
    float fishPos[3];   // world, OoT units
    int16_t yaw;        // the fish's world yaw (binary angle units) on entry
    float anglerPos[3]; // Link
    float waterY;       // surface
    float floorY;       // ground under the fish (OoT floor probe; below -30000 = none)
    int32_t mode;       // RF3 fight mode (RF3FightOutput.mode)
    float distUnits;    // the fight's distance (RF3FightOutput.targetDistUnits)
    int32_t substeps;   // RF3 ticks (1/60 s) per game frame
    uint32_t frame;     // play->gameplayFrames
    // Is (x, z) open water? (z_fishing.c: the region's box or the pond)
    int32_t (*inWater)(float x, float z);
    // Step 26f-1: RF3's obstacle probe (RF3-MECHANICS 6.4), a line from `from` to `to` (world, OoT units).
    // Returns 0 = clear, 1 = scenery hit with its unit surface normal in `normal`, 2 = blocked with no normal
    // (the tip isn't open water). May be null (no probing).
    int32_t (*probe)(const float from[3], const float to[3], float normal[3]);
} RF3FishMoveInput;

typedef struct RF3FishMoveOutput {
    float target[3]; // a point straight ahead on RF3's heading: z_fishing.c's fishTargetPos
    float speed;     // units per game frame: actor.speedXZ
} RF3FishMoveOutput;

// One game frame. Returns 1 if RF3 movement applies (out is filled), 0 for modes it leaves to the old code
// (HIT, BUKKONUKI, CATCH, none) or when the species is unknown.
int32_t RF3FishMove_Step(const RF3FishMoveInput* in, RF3FishMoveOutput* out);

// Step 26e-8: one line to logs/rf3_models.txt (z_fishing.c's wall test).
void RF3FishMove_Log(const char* line);

#ifdef __cplusplus
}
#endif

#endif
