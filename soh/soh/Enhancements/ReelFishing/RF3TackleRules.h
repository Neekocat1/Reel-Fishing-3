#pragma once

// Reel Fishing III mod (step 18): the tackle box choice applied to the fishing, by RF3's formulas
// (docs/RF3-MECHANICS.md). Step 18b: the fight's tension limit. See docs/DESIGN.md step 18.

#include <cstdint>
#include <string>

#include "RF3Bridge.h"
#include "RF3Params.h"

namespace rf3tackle {

// The tackle a fight uses.
struct FightTackle {
    rf3::TackleParams tackle = rf3::kDefaultTackle;
    int32_t tensionBonus = 0; // shield sum for the fish's record (RF3-MECHANICS 1.4)
    bool fromChoice = false;  // false: the default tackle (no tackle.json, or it predates step 18a)
    int32_t box = -1;
    int32_t rod = -1;
    int32_t line = -1;
    int32_t lure = -1;
    int32_t floatIdx = -1; // step 19c, bait sets
    int32_t bait = -1;
    int32_t size = -1;
    std::string why; // why the default tackle was used
};

// The saved choice for a fight with a fish of this fishdata record. defaultTensionBonus is the fish
// table's value for the default tackle, used when the choice can't be applied.
FightTackle ForFight(int32_t record, int32_t defaultTensionBonus);

// Step 18c: what the tackle means for a fish noticing the lure (RF3-MECHANICS 2.3, 2.4).
struct ApproachTackle {
    int32_t sightBonus = 0;  // heart sum for the fish's record (only counted if positive, by SightRange)
    int32_t appeal = 15;     // the lure's appeal for the record; 15 = no change, 0 = never seen
    bool rodAllowed = true;  // the record's rod mask has the rod's bit
    bool fromChoice = false; // false: the default tackle
};
ApproachTackle ForApproach(int32_t record, int32_t defaultSightBonus);

// One "fight-tackle" line in logs/rf3_records.txt.
void LogFightTackle(const FightTackle& ft, int32_t record, float tensionLimit);

} // namespace rf3tackle
