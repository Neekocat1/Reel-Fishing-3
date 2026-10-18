// Reel Fishing III mod (step 54, docs/DESIGN.md "Step 54 design"): the check tracker names each RF3 check by what it
// takes ("Catch 5 Largemouth Bass") instead of its location name ("RF3 Lure Condition 57"). Only the tracker's
// display changes (SoH's checkNameOverrides); spoiler logs, plando and the exclude list keep the location names.
// The sentences come from rf3::DescribeCondition (RF3Sim.cpp), with species names from the fish table and tackle
// names from tackle.json (generic "RF3 Lure 20" without it).
// Log, don't look: logs/rf3_check_names.txt gets every shown RF3 check's id, location name and tracker name at each
// rando file load.

#include "RF3RandoChecks.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>

#include <ship/Context.h>

#include "soh/OTRGlobals.h"
#include "soh/Enhancements/randomizer/static_data.h"
#include "RF3Bridge.h"
#include "RF3FishTable.h"
#include "RF3Items.h"
#include "RF3Sim.h"

extern "C" {
#include "z64.h"
#include "variables.h" // gSaveContext, for IS_RANDO
}

namespace {

std::string SpeciesName(int species) {
    for (const rf3::FishRecord& r : rf3::kFishRecords) {
        if (r.species == species && r.speciesName != nullptr) {
            return r.speciesName;
        }
    }
    return "species " + std::to_string(species);
}

std::string TackleName(int kind, int index) {
    const char* name = RF3Stock_Name(kind, index);
    if (name == nullptr || name[0] == '\0' || name[0] == '#') { // no tackle.json name: the generic item name
        int32_t offset = RF3Items_Offset(kind, index);
        return std::string("RF3 ") + (offset >= 0 ? RF3Items_KindName(offset) : "Tackle") + " " + std::to_string(index);
    }
    return name;
}

const rf3::ConditionNames& Names() {
    static const rf3::ConditionNames names{ SpeciesName, TackleName };
    return names;
}

constexpr int kHylianLoach = 34; // RF3RandoLogic.cpp's HylianLoach()

} // namespace

std::string RF3Rando_CheckLabel(RandomizerCheck rc) {
    if (rc >= RC_RF3_ROD_CONDITION_0 && rc < RC_RF3_ROD_CONDITION_0 + 10) {
        return rf3::DescribeRodCondition(static_cast<int>(rc - RC_RF3_ROD_CONDITION_0), Names());
    }
    if (rc == RC_RF3_FIRST_LOACH) {
        return "Catch a " + SpeciesName(kHylianLoach);
    }
    if (rc == RC_RF3_POND_SHOP) {
        return "Pay the pond owner to fish";
    }
    if (rc >= RC_RF3_CATCH_1 && rc <= RC_RF3_CATCH_147) {
        return rf3::DescribeCatchNumber(static_cast<int>(rc - RC_RF3_CATCH_1) + 1);
    }
    if (rc >= RC_RF3_LURE_0 && rc <= RC_RF3_FLOAT_5) {
        const rf3::LureCondition* c = rf3::UnlockConditionFor(static_cast<int>(rc - RC_RF3_LURE_0));
        if (c != nullptr) {
            return rf3::DescribeCondition(*c, Names());
        }
    }
    return Rando::StaticData::GetLocation(rc)->GetShortName();
}

void RF3Rando_TrackerNames(std::unordered_map<RandomizerCheck, std::string>& overrides) {
    for (int32_t rc = RC_RF3_ROD_CONDITION_0; rc <= RC_RF3_FLOAT_5; rc++) {
        overrides.erase(static_cast<RandomizerCheck>(rc));
    }
    if (!IS_RANDO) {
        return;
    }
    std::vector<RandomizerCheck> shownChecks;
    std::vector<std::string> labels;
    for (int32_t i = RC_RF3_ROD_CONDITION_0; i <= RC_RF3_FLOAT_5; i++) {
        RandomizerCheck rc = static_cast<RandomizerCheck>(i);
        if (Rando::StaticData::GetLocation(rc)->GetRCType() != RCTYPE_RF3 || !RF3Rando_CheckIncludedBySeed(rc)) {
            continue;
        }
        shownChecks.push_back(rc);
        labels.push_back(RF3Rando_CheckLabel(rc));
    }
    // Rule 10: the same sentence twice in one seed -> " (2)", " (3)" on the later checks, by check id.
    const int clashes = rf3::NumberClashes(labels);

    std::string dir = Ship::Context::GetPathRelativeToAppDirectory("logs");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(dir + "/rf3_check_names.txt", std::ios::app);
    char stamp[32];
    std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    size_t longest = 0;
    std::string lines;
    for (size_t k = 0; k < shownChecks.size(); k++) {
        RandomizerCheck rc = shownChecks[k];
        overrides[rc] = labels[k];
        longest = std::max(longest, labels[k].size());
        lines += std::string(stamp) + " rc=" + std::to_string(static_cast<int>(rc)) + " " +
                 Rando::StaticData::GetLocation(rc)->GetShortName() + " -> " + labels[k] + "\n";
    }
    out << stamp << " rf3-check-names shown=" << shownChecks.size() << " clashes=" << clashes
        << " longest=" << longest << "\n"
        << lines;
}
