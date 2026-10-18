// Reel Fishing III mod (step 24b): RF3's checks in the randomizer. The 10 rod conditions, the first Hylian Loach,
// the pond shop and "catch #1..#147" are randomizer locations, each with its own RandomizerInf flag; setting the
// flag in game makes the randomizer give the item placed there (hook_handlers.cpp). Two settings: "Shuffle RF3
// Tackle" and "RF3 Catch Checks" (N; catches above N are not checks). See docs/DESIGN.md step 24.
// Their logic is in RF3RandoLogic.cpp (step 24c).

#include "RF3RandoChecks.h"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

#include <libultraship/bridge/consolevariablebridge.h>
#include <ship/Context.h>

#include "soh/OTRGlobals.h"
#include "soh/ShipInit.hpp"
#include "soh/cvar_prefixes.h"
#include "soh/Enhancements/randomizer/RCToRandInf.h"
#include "soh/Enhancements/randomizer/location_access.h"
#include "soh/Enhancements/randomizer/static_data.h"
#include "soh/Enhancements/randomizer/SeedContext.h"
#include "soh/Enhancements/randomizer/randomizer_check_objects.h" // step 53b
#include "soh/Enhancements/randomizer/settings.h"                 // step 53b
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "RF3Bridge.h"
#include "RF3Sim.h" // step 43b: which lures have a condition

extern "C" {
#include "z64.h"
#include "functions.h"
#include "variables.h"
}

namespace {

static_assert(RC_RF3_CATCH_147 - RC_RF3_CATCH_1 + 1 == RF3_RANDO_CATCH_MAX, "catch checks");
static_assert(RAND_INF_RF3_CATCH_147 - RAND_INF_RF3_CATCH_1 + 1 == RF3_RANDO_CATCH_MAX, "catch flags");
static_assert(RC_RF3_LURE_92 - RC_RF3_LURE_0 + 1 == rf3::kLureCount, "lure checks");
static_assert(RAND_INF_RF3_LURE_92 - RAND_INF_RF3_LURE_0 + 1 == rf3::kLureCount, "lure flags");
// Step 44c: check/flag = RC_RF3_LURE_0 / RAND_INF_RF3_LURE_0 + the condition's slot.
static_assert(RC_RF3_FLY_0 - RC_RF3_LURE_0 == rf3::kFlySlot0 && RC_RF3_BAIT_0 - RC_RF3_LURE_0 == rf3::kBaitSlot0 &&
                  RC_RF3_FLOAT_0 - RC_RF3_LURE_0 == rf3::kFloatSlot0 &&
                  RC_RF3_FLOAT_5 - RC_RF3_LURE_0 + 1 == rf3::kUnlockSlots,
              "condition checks");
static_assert(RAND_INF_RF3_FLY_0 - RAND_INF_RF3_LURE_0 == rf3::kFlySlot0 &&
                  RAND_INF_RF3_BAIT_0 - RAND_INF_RF3_LURE_0 == rf3::kBaitSlot0 &&
                  RAND_INF_RF3_FLOAT_0 - RAND_INF_RF3_LURE_0 == rf3::kFloatSlot0 &&
                  RAND_INF_RF3_FLOAT_5 - RAND_INF_RF3_LURE_0 + 1 == rf3::kUnlockSlots,
              "condition flags");

const char* const kRodNames[10] = { "Lure Rod/Bass",     "Lure Rod/Trout",   "Lure Rod/Salmon", "Fly Rod",
                                    "Big Game Fly Rod",  "Stream Pole S",    "Stream Pole L",   "Crucian Carp Rod",
                                    "All-Purpose Rod L", "Bitterling Rod" };

// Which number catch a check is (1..147), 0 if it isn't a catch check.
int32_t CatchNumber(RandomizerCheck rc) {
    return (rc >= RC_RF3_CATCH_1 && rc <= RC_RF3_CATCH_147) ? static_cast<int32_t>(rc - RC_RF3_CATCH_1) + 1 : 0;
}

// Step 43b: the slot of an unlock condition check (lures 0-92; step 44c: flies, baits, floats up to 150), -1 if it
// isn't one.
int32_t SlotOfCheck(RandomizerCheck rc) {
    return (rc >= RC_RF3_LURE_0 && rc <= RC_RF3_FLOAT_5) ? static_cast<int32_t>(rc - RC_RF3_LURE_0) : -1;
}

void Add(RandomizerCheck rc, RandomizerInf flag, std::string name, RandomizerGet vanilla);

} // namespace

// Step 24b: called once at start-up, like SoH's RegisterFishLocations (static_data.h).
void Rando::StaticData::RegisterRF3Locations() {
    static bool registered = false;
    if (registered) {
        return;
    }
    registered = true;
    for (int32_t r = 0; r < 10; r++) {
        Add(static_cast<RandomizerCheck>(RC_RF3_ROD_CONDITION_0 + r),
            static_cast<RandomizerInf>(RAND_INF_RF3_ROD_CONDITION_0 + r), std::string("RF3 ") + kRodNames[r],
            static_cast<RandomizerGet>(RG_RF3_ROD_0 + r));
    }
    Add(RC_RF3_FIRST_LOACH, RAND_INF_RF3_FIRST_LOACH, "RF3 First Hylian Loach", RG_RF3_LURE_92);
    Add(RC_RF3_POND_SHOP, RAND_INF_RF3_POND_SHOP, "RF3 Fishing Pond Shop", RG_RF3_ROD_10);
    for (int32_t n = 1; n <= RF3_RANDO_CATCH_MAX; n++) {
        Add(static_cast<RandomizerCheck>(RC_RF3_CATCH_1 + n - 1), static_cast<RandomizerInf>(RAND_INF_RF3_CATCH_1 + n - 1),
            "RF3 Catch " + std::to_string(n), RG_NONE);
    }
    // Step 43b: one per lure (only those with a condition are ever in a seed); stable names for spoiler logs.
    for (int32_t l = 0; l < rf3::kLureCount; l++) {
        Add(static_cast<RandomizerCheck>(RC_RF3_LURE_0 + l), static_cast<RandomizerInf>(RAND_INF_RF3_LURE_0 + l),
            "RF3 Lure Condition " + std::to_string(l), RG_NONE);
    }
    // Step 44c: flies, baits and floats, by their own index.
    for (int32_t slot = rf3::kFlySlot0; slot < rf3::kUnlockSlots; slot++) {
        std::string name = slot < rf3::kBaitSlot0    ? "RF3 Fly Condition " + std::to_string(slot - rf3::kFlySlot0)
                           : slot < rf3::kFloatSlot0 ? "RF3 Bait Condition " + std::to_string(slot - rf3::kBaitSlot0)
                                                     : "RF3 Float Condition " + std::to_string(slot - rf3::kFloatSlot0);
        Add(static_cast<RandomizerCheck>(RC_RF3_LURE_0 + slot), static_cast<RandomizerInf>(RAND_INF_RF3_LURE_0 + slot),
            name, RG_NONE);
    }
}

namespace {

void Add(RandomizerCheck rc, RandomizerInf flag, std::string name, RandomizerGet vanilla) {
    Rando::StaticData::GetLocationTable()[rc] =
        Rando::Location::Base(rc, RCQUEST_BOTH, RCTYPE_RF3, RCAREA_REEL_FISHING, ACTOR_ID_MAX, SCENE_FISHING_POND, 0x00,
                              std::move(name), // step 53b: the tracker's and exclusions' "Reel Fishing"
                              RHT_LH_POND_FISH, vanilla, Rando::SpoilerCollectionCheck::RandomizerInf(flag));
    rcToRandomizerInf[rc] = flag;
}

// Step 24d: which RF3 items the loaded seed placed somewhere (built on first use after a file loads).
bool sInSeedBuilt = false;
bool sInSeed[RG_RF3_LURE_92 - RG_RF3_ROD_0 + 1] = {};

// Step 53b: one line per rando file load in logs/rf3_rando.txt: how many RF3 checks have the Reel Fishing area, how
// many the seed includes, the exclude list's size for that area, and which tracker area the pond's scene maps to
// (should stay Lake Hylia).
void LogTrackerArea() {
    int32_t inArea = 0;
    int32_t otherArea = 0;
    int32_t included = 0;
    for (int32_t rc = RC_RF3_ROD_CONDITION_0; rc <= RC_RF3_FLOAT_5; rc++) {
        const Rando::Location* loc = Rando::StaticData::GetLocation(static_cast<RandomizerCheck>(rc));
        if (loc->GetRCType() != RCTYPE_RF3) {
            continue;
        }
        (loc->GetArea() == RCAREA_REEL_FISHING ? inArea : otherArea)++;
        included += RF3Rando_CheckIncludedBySeed(static_cast<RandomizerCheck>(rc)) ? 1 : 0;
    }
    size_t excludes = Rando::Settings::GetInstance()->GetExcludeOptionsForArea(RCAREA_REEL_FISHING).size();
    RandomizerCheckArea pond = RandomizerCheckObjects::GetRCAreaBySceneID(SCENE_FISHING_POND);
    std::string dir = Ship::Context::GetPathRelativeToAppDirectory("logs");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(dir + "/rf3_rando.txt", std::ios::app);
    char stamp[32];
    std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    out << stamp << " rf3-tracker area=" << RandomizerCheckObjects::GetRCAreaName(RCAREA_REEL_FISHING)
        << " checks=" << inArea << " otherArea=" << otherArea << " inSeed=" << included << " excludes=" << excludes
        << " pondScene=" << RandomizerCheckObjects::GetRCAreaName(pond) << "\n";
}

void RegisterRF3RandoChecks() {
    Rando::StaticData::RegisterRF3Locations();
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnLoadFile>([](int32_t fileNum) {
        sInSeedBuilt = false;
        RF3Rando_ApplyItemNames(); // step 24e: the tracker and hints show the real names
        if (IS_RANDO) {
            LogTrackerArea(); // step 53b
        }
    });
}

} // namespace

static RegisterShipInitFunc initRF3RandoChecks(RegisterRF3RandoChecks);

bool RF3Rando_CheckIncluded(RandomizerCheck rc, bool shuffle, int32_t catchChecks, bool advanced) {
    if (!shuffle) {
        return false;
    }
    int32_t slot = SlotOfCheck(rc);
    if (slot >= 0) {
        return advanced && rf3::UnlockConditionFor(slot) != nullptr; // steps 43b, 44c
    }
    int32_t n = CatchNumber(rc);
    return n == 0 || (!advanced && n <= catchChecks);
}

bool RF3Rando_CheckIncludedBySeed(RandomizerCheck rc) {
    auto ctx = Rando::Context::GetInstance();
    return RF3Rando_CheckIncluded(rc, ctx->GetOption(RSK_RF3_TACKLE_SHUFFLE).Get() != 0,
                                  ctx->GetOption(RSK_RF3_CATCH_CHECKS).Get(),
                                  ctx->GetOption(RSK_RF3_CHECK_MODE).Get() == 1);
}

bool RF3Rando_CheckIncludedByCVars(RandomizerCheck rc) {
    return RF3Rando_CheckIncluded(rc, CVarGetInteger(CVAR_RANDOMIZER_SETTING("RF3TackleShuffle"), 0) != 0,
                                  CVarGetInteger(CVAR_RANDOMIZER_SETTING("RF3CatchChecks"), 20),
                                  CVarGetInteger(CVAR_RANDOMIZER_SETTING("RF3CheckMode"), 0) == 1);
}

// RF3Rando_AddRegionLocations: RF3RandoLogic.cpp (step 24c).

// --- Step 24d: in game ---------------------------------------------------------------------------------------------

bool RF3Rando_Active() {
    return IS_RANDO && RAND_GET_OPTION(RSK_RF3_TACKLE_SHUFFLE).Get() != 0;
}

int32_t RF3Rando_CatchChecks() {
    return RAND_GET_OPTION(RSK_RF3_CATCH_CHECKS).Get();
}

bool RF3Rando_Advanced() {
    return RF3Rando_Active() && RAND_GET_OPTION(RSK_RF3_CHECK_MODE).Get() == 1;
}

bool RF3Rando_ItemInSeed(RandomizerGet rg) {
    if (!RF3Rando_IsItem(rg)) {
        return false;
    }
    if (!sInSeedBuilt) {
        sInSeedBuilt = true;
        for (bool& b : sInSeed) {
            b = false;
        }
        auto ctx = Rando::Context::GetInstance();
        for (int rc = 0; rc < RC_MAX; rc++) {
            RandomizerGet placed = ctx->GetItemLocation(static_cast<RandomizerCheck>(rc))->GetPlacedRandomizerGet();
            if (RF3Rando_IsItem(placed)) {
                sInSeed[placed - RG_RF3_ROD_0] = true;
            }
        }
    }
    return sInSeed[rg - RG_RF3_ROD_0];
}

bool RF3Rando_SetCheck(RandomizerInf flag, const char* why) {
    if (Flags_GetRandomizerInf(flag)) {
        return false;
    }
    Flags_SetRandomizerInf(flag); // the randomizer queues the check's item (hook_handlers.cpp)
    RF3Rod_LogEvent(why, static_cast<int32_t>(flag));
    return true;
}
