#pragma once
// Reel Fishing III mod (step 62c, docs/DESIGN.md "Step 62 design"): RF3's controller vibration in the fight. The
// bridge (RF3Fight_Update) reports RF3's vibration moments as the fight sim passes them; RF3Vibration.cpp plays RF3's
// pattern (rf3::VibPlayer) and sends it through OoT's rumble each game frame (choice V1 A).

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One fight sub-step's changes: the mode before and after (rf3::Mode values), the hook just set, the hook-set window
// just opened (its length), and the new state's length (STRUGGLE's).
void RF3Vib_FightStep(int32_t prevMode, int32_t mode, int32_t hookSet, int32_t windowOpened, float windowSeconds,
                      float stateSeconds);
// The fight ended (RF3Fight_End): a landing pattern plays out; anything else stops.
void RF3Vib_FightEnded(void);
// The debug window's line.
const char* RF3Vib_Status(void);

#ifdef __cplusplus
}
#endif
