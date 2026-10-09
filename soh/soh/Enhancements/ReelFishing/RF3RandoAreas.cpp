// Reel Fishing III mod (step 24a): the randomizer's areas (logic regions, RR_*) for a scene, so each fish region
// can name the area its water is in. Step 24c's logic requires reaching that area to catch the region's fish.
// See docs/DESIGN.md step 24.

#include "RF3Regions.h"

#include "soh/Enhancements/randomizer/location_access.h"
#include "soh/Enhancements/randomizer/randomizerEnumStrings.h"

namespace rf3 {

std::vector<RandoArea> RandoAreasForScene(int scene) {
    // The table is built for seed generation and by the check tracker; build it here if neither has run yet.
    bool built = false;
    for (int i = 0; i < RR_MAX && !built; i++) {
        built = !areaTable[i].regionName.empty();
    }
    if (!built) {
        RegionTable_Init();
    }
    std::vector<RandoArea> out;
    for (int i = 0; i < RR_MAX; i++) {
        if (static_cast<int>(areaTable[i].scene) == scene && !areaTable[i].regionName.empty()) {
            out.push_back({ std::string(EnumToString(static_cast<RandomizerRegion>(i))), areaTable[i].regionName });
        }
    }
    return out;
}

int RandoAreaFromName(const std::string& name) {
    if (name.empty()) {
        return -1;
    }
    for (int i = 0; i < RR_MAX; i++) {
        if (EnumToString(static_cast<RandomizerRegion>(i)) == name) {
            return i;
        }
    }
    return -1; // a name this SoH build doesn't have
}

} // namespace rf3
