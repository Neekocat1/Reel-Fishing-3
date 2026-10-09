// Reel Fishing III mod (step 24b): RF3's checks in the randomizer. The 10 rod conditions, the first Hylian Loach,
// the pond shop and "catch #1..#147" are randomizer locations, each with its own RandomizerInf flag; setting the
// flag in game makes the randomizer give the item placed there (hook_handlers.cpp). Two settings: "Shuffle RF3
// Tackle" and "RF3 Catch Checks" (N; catches above N are not checks). See docs/DESIGN.md step 24.
// Their logic is in RF3RandoLogic.cpp (step 24c).

#include "RF3RandoChecks.h"

#include <string>

#include <libultraship/bridge/consolevariablebridge.h>

#include "soh/OTRGlobals.h"
#include "soh/ShipInit.hpp"
#include "soh/cvar_prefixes.h"
#include "soh/Enhancements/randomizer/RCToRandInf.h"
#include "soh/Enhancements/randomizer/location_access.h"
#include "soh/Enhancements/randomizer/static_data.h"
#include "soh/Enhancements/randomizer/SeedContext.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "RF3Bridge.h"

extern "C" {
#include "z64.h"
#include "functions.h"
#include "variables.h"
}

namespace {

static_assert(RC_RF3_CATCH_147 - RC_RF3_CATCH_1 + 1 == RF3_RANDO_CATCH_MAX, "catch checks");
static_assert(RAND_INF_RF3_CATCH_147 - RAND_INF_RF3_CATCH_1 + 1 == RF3_RANDO_CATCH_MAX, "catch flags");

const char* const kRodNames[10] = { "Lure Rod/Bass",     "Lure Rod/Trout",   "Lure Rod/Salmon", "Fly Rod",
                                    "Big Game Fly Rod",  "Stream Pole S",    "Stream Pole L",   "Crucian Carp Rod",
                                    "All-Purpose Rod L", "Bitterling Rod" };

// Which number catch a check is (1..147), 0 if it isn't a catch check.
int32_t CatchNumber(RandomizerCheck rc) {
    return (rc >= RC_RF3_CATCH_1 && rc <= RC_RF3_CATCH_147) ? static_cast<int32_t>(rc - RC_RF3_CATCH_1) + 1 : 0;
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
}

namespace {

void Add(RandomizerCheck rc, RandomizerInf flag, std::string name, RandomizerGet vanilla) {
    Rando::StaticData::GetLocationTable()[rc] =
        Rando::Location::Base(rc, RCQUEST_BOTH, RCTYPE_RF3, ACTOR_ID_MAX, SCENE_FISHING_POND, 0x00, std::move(name),
                              RHT_LH_POND_FISH, vanilla, Rando::SpoilerCollectionCheck::RandomizerInf(flag));
    rcToRandomizerInf[rc] = flag;
}

// Step 24d: which RF3 items the loaded seed placed somewhere (built on first use after a file loads).
bool sInSeedBuilt = false;
bool sInSeed[RG_RF3_LURE_92 - RG_RF3_ROD_0 + 1] = {};

void RegisterRF3RandoChecks() {
    Rando::StaticData::RegisterRF3Locations();
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnLoadFile>([](int32_t fileNum) {
        sInSeedBuilt = false;
        RF3Rando_ApplyItemNames(); // step 24e: the tracker and hints show the real names
    });
}

} // namespace

static RegisterShipInitFunc initRF3RandoChecks(RegisterRF3RandoChecks);

bool RF3Rando_CheckIncluded(RandomizerCheck rc, bool shuffle, int32_t catchChecks) {
    if (!shuffle) {
        return false;
    }
    int32_t n = CatchNumber(rc);
    return n == 0 || n <= catchChecks;
}

bool RF3Rando_CheckIncludedBySeed(RandomizerCheck rc) {
    auto ctx = Rando::Context::GetInstance();
    return RF3Rando_CheckIncluded(rc, ctx->GetOption(RSK_RF3_TACKLE_SHUFFLE).Get() != 0,
                                  ctx->GetOption(RSK_RF3_CATCH_CHECKS).Get());
}

bool RF3Rando_CheckIncludedByCVars(RandomizerCheck rc) {
    return RF3Rando_CheckIncluded(rc, CVarGetInteger(CVAR_RANDOMIZER_SETTING("RF3TackleShuffle"), 0) != 0,
                                  CVarGetInteger(CVAR_RANDOMIZER_SETTING("RF3CatchChecks"), 20));
}

// RF3Rando_AddRegionLocations: RF3RandoLogic.cpp (step 24c).

// --- Step 24d: in game ---------------------------------------------------------------------------------------------

bool RF3Rando_Active() {
    return IS_RANDO && RAND_GET_OPTION(RSK_RF3_TACKLE_SHUFFLE).Get() != 0;
}

int32_t RF3Rando_CatchChecks() {
    return RAND_GET_OPTION(RSK_RF3_CATCH_CHECKS).Get();
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
