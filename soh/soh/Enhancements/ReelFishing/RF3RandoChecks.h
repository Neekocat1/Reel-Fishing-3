// Reel Fishing III mod (step 24b): RF3's checks in the randomizer. See RF3RandoChecks.cpp.
#pragma once

#include <cstdint>

#include "soh/Enhancements/randomizer/randomizerEnums.h"

#define RF3_RANDO_CATCH_MAX 147

// Is this RF3 check a location in the seed? shuffle = "Shuffle RF3 Tackle"; catchChecks = "RF3 Catch Checks".
bool RF3Rando_CheckIncluded(RandomizerCheck rc, bool shuffle, int32_t catchChecks);
bool RF3Rando_CheckIncludedBySeed(RandomizerCheck rc);  // from the seed's options (generation, in game)
bool RF3Rando_CheckIncludedByCVars(RandomizerCheck rc); // from the settings menu (location lists)
// Called at the end of RegionTable_Init: puts the RF3 checks into their logic areas (RF3RandoLogic.cpp, step 24c).
void RF3Rando_AddRegionLocations();
// Step 24c: the flag that says Link has an RF3 item (rg in RG_RF3_ROD_0..RG_RF3_LURE_92).
RandomizerInf RF3Rando_OwnedFlag(RandomizerGet rg);
#include <vector>

// Step 24d: in game. Active = a randomizer save with "Shuffle RF3 Tackle" on.
bool RF3Rando_Active();
int32_t RF3Rando_CatchChecks(); // the seed's "RF3 Catch Checks"
// Is this RF3 item placed somewhere in the seed? (Not: a leftover, unlocked by catches after the catch checks.)
bool RF3Rando_ItemInSeed(RandomizerGet rg);
// Sets an RF3 check's flag once (the randomizer then gives the item placed there). Returns true if newly set.
bool RF3Rando_SetCheck(RandomizerInf flag, const char* why);
// Step 24c (user, 2026-10-08): the main item pool gets every rod and one starter tackle per rod; the rest of the
// tackle (not lures 89/90) fills free spots before junk, as many as fit.
std::vector<RandomizerGet> RF3Rando_MainPoolItems();
std::vector<RandomizerGet> RF3Rando_ExtraPoolItems();
// Step 24e: (item_list.cpp) builds an RF3 item table entry with this name.
#include <string>
void RF3Rando_BuildItem(RandomizerGet get, const std::string& name);
// Step 24e: gives RF3 items their tackle names (once per session; only with tackle.json loaded). A name that clashes
// with another item gets its kind added ("Caddis Larva (Fly)"). The generic names stay readable in spoiler logs.
void RF3Rando_ApplyItemNames();
inline bool RF3Rando_IsItem(RandomizerGet rg) {
    return rg >= RG_RF3_ROD_0 && rg <= RG_RF3_LURE_92;
}
