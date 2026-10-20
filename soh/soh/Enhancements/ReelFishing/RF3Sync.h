// Reel Fishing III mod (step 56, docs/DESIGN.md "Step 56 design"): RF3 progress shared in SoH's co-op (Anchor).
// A catch is an event (rf3::UnlockCatch); a teammate's catch is applied like Link's own, except that it never adds
// to a streak or visit and never gives items itself (RF3Stock.cpp). Randomizer seeds with RF3 tackle shuffled only.
#pragma once

#include "RF3Sim.h"

#include <cstdint>

// Step 56a: applies a teammate's catch (no-op outside an RF3 seed).
void RF3Stock_ApplyRemoteCatch(const rf3::UnlockCatch& k);

// Step 56b (RF3Sync.cpp): after Link's own catch, sends it to the team (Anchor connected, items and flags synced, an
// RF3 seed); and the receiving end of Anchor's RF3_CATCH packet. Both log "rf3-sync" in logs/rf3_records.txt.
void RF3Sync_SendCatch(const rf3::UnlockCatch& k);
void RF3Sync_ReceiveCatch(const rf3::UnlockCatch& k, uint32_t fromClientId);

// Step 56c: this save's RF3 progress, for Anchor's team state, and the merge of a teammate's (RF3Stock.cpp).
void RF3Stock_GetTeamProgress(uint32_t* catches, rf3::RodProgress* rod, rf3::UnlockProgress* unlock);
void RF3Stock_MergeTeamProgress(uint32_t catches, const rf3::RodProgress& rod, const rf3::UnlockProgress& unlock);
// Step 56c (RF3Sync.cpp): the team state's "rf3" object. Export returns an empty object outside an RF3 seed; a
// received one is merged now, or at the next scene start if gameplay hasn't started yet.
#include <nlohmann/json.hpp>
nlohmann::json RF3Sync_ExportTeamState();
void RF3Sync_ReceiveTeamState(const nlohmann::json& rf3State);
