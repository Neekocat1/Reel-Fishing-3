// Reel Fishing III mod (step 22a): RF3's tackle as randomizer items (RG_RF3_*). See RF3Items.h and
// docs/DESIGN.md step 22. The stock (RF3Stock.cpp) stays the source of truth: an item given through
// Randomizer_Item_Give lands there.

#include "RF3RandoChecks.h" // step 24d
#include "RF3Items.h"

#include <deque>

#include <libultraship/bridge/consolevariablebridge.h>

#include "soh/OTRGlobals.h"
#include "soh/ShipInit.hpp"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/cvar_prefixes.h"
#include "soh/Enhancements/randomizer/randomizer.h"
#include "soh/Enhancements/randomizer/static_data.h"
#include "RF3Bridge.h"
#include "RF3LureModel.h" // step 28
#include "RF3RodModel.h"
#include "soh/Enhancements/randomizer/draw.h" // Randomizer_DrawFishingPoleGI (OoT's pole, step 28b)

extern "C" {
#include <z64.h>
#include "variables.h"
#include "functions.h"
#include "macros.h"
#include "objects/object_fish/object_fish.h"
extern PlayState* gPlayState;
// z_player.c (public, also used by RF3Rod.cpp and SoH's AlwaysOnFixes.cpp)
void Player_UseItem(PlayState* play, Player* player, s32 item);
}

namespace {

// Kinds in RG order, with their sizes (RF3Bridge.h RF3_STOCK_*; the sizes are RF3Stock_Size).
constexpr int32_t kKinds[] = { RF3_STOCK_ROD, RF3_STOCK_LURE, RF3_STOCK_FLY, RF3_STOCK_BAIT, RF3_STOCK_FLOAT };
constexpr int32_t kSizes[] = { 10, 91, 40, 12, 6 };
constexpr const char* kNames[] = { "Rod", "Lure", "Fly", "Bait", "Float" };
static_assert(RG_RF3_LURE_92 - RG_RF3_ROD_0 + 1 == RF3_ITEM_COUNT, "RG_RF3_* must match RF3Items.h");
// Step 23c: after the five groups (159 items), OoT's pole and lures, each with its kind and index.
constexpr int32_t kGroupedCount = 159;
constexpr int32_t kExtraKind[] = { RF3_STOCK_ROD, RF3_STOCK_LURE, RF3_STOCK_LURE };
constexpr int32_t kExtraIndex[] = { 10, 91, 92 };

// The pond draws its lure at 0.004 (z_fishing.c); a get-item model is much larger. Tunable from the debug
// window until it looks right (the user checks it).
constexpr float kDefaultLureScale = 0.03f;
// Step 28: RF3 tackle's longest side in get-item frame units, and a rod's length in world units. Sliders in the debug
// window; defaults set by the user in game (2026-10-12: "35 rod length and -5.0 rod height look correct. 60 tackle
// size looks correct"). The rod's height offset default is in RF3RodModel.cpp.
constexpr float kDefaultGiTackleSize = 60.0f;
constexpr float kDefaultGiRodLength = 35.0f;

// Step 22c: items already in the stock, waiting for Link to hold them up (front first). Not saved: a quit
// before the animation only skips the animation. sAsked: the front was handed to GiveItemEntryWithoutActor.
std::deque<int32_t> sQueue;
bool sAsked = false;
// Step 22c fix (user, 2026-10-08): the item get puts the rod away. If Link held it when the first queued item
// started, he takes it out again once the last one is shown and its text is closed. sRestoreFrames: tries left.
bool sHadRod = false;
bool sInRun = false; // a run = queued items shown back to back; the rod is judged at its first item only
bool sRestoreRod = false;
int32_t sRestoreFrames = 0;
constexpr int32_t kRestoreTries = 40; // player updates (2 s at 20 Hz)
// Step 24d fix: any item get (the randomizer's too) that starts while Link holds the rod brings it back afterwards.
// sGetHadRod sticks while a restore is pending, so back-to-back item gets keep it.
bool sPrevHeldRod = false;
bool sInGet = false;
bool sGetHadRod = false;

// Like the randomizer's item queue (hook_handlers.cpp), plus: no text box open, so the item comes after the
// catch message.
bool LinkIsFree(PlayState* play) {
    Player* player = GET_PLAYER(play);
    return player != nullptr && !Player_InBlockingCsMode(play, player) &&
           !(player->stateFlags1 & (PLAYER_STATE1_IN_ITEM_CS | PLAYER_STATE1_GETTING_ITEM |
                                    PLAYER_STATE1_CARRYING_ACTOR)) &&
           Message_GetState(&play->msgCtx) == TEXT_STATE_NONE;
}

void RestoreRod() {
    Player* player = GET_PLAYER(gPlayState);
    if (player->heldItemAction == PLAYER_IA_FISHING_POLE) {
        sRestoreRod = false;
        RF3Rod_LogEvent("item-rod-restored", kRestoreTries - sRestoreFrames);
        return;
    }
    if (sRestoreFrames-- <= 0) {
        sRestoreRod = false;
        RF3Rod_LogEvent("item-rod-restore-failed", gPlayState->sceneNum);
        return;
    }
    // As the rod's D-pad button does (RF3Rod.cpp); outside the pond its player-update hook then starts the
    // portable rod.
    Player_UseItem(gPlayState, player, ITEM_FISHING_POLE);
}

// Step 24d fix: watch every item get, not only the queue's.
void WatchItemGets() {
    Player* player = GET_PLAYER(gPlayState);
    if (player == nullptr) {
        return;
    }
    bool getting = (player->stateFlags1 & PLAYER_STATE1_GETTING_ITEM) != 0;
    if (getting && !sInGet) {
        sInGet = true;
        sGetHadRod = sPrevHeldRod || sRestoreRod || sGetHadRod;
    } else if (!getting && sInGet) {
        sInGet = false;
        if (sGetHadRod && sQueue.empty()) {
            sRestoreRod = true; // UpdateQueue takes it out once the text is closed
            sRestoreFrames = kRestoreTries;
            sGetHadRod = false;
        }
    }
    sPrevHeldRod = player->heldItemAction == PLAYER_IA_FISHING_POLE;
}

void UpdateQueue() {
    if (gPlayState == nullptr) {
        return;
    }
    WatchItemGets();
    if (!LinkIsFree(gPlayState)) {
        return;
    }
    if (sQueue.empty()) {
        if (sRestoreRod) {
            RestoreRod();
        }
        return;
    }
    GetItemEntry entry =
        Rando::StaticData::RetrieveItem(static_cast<RandomizerGet>(RG_RF3_ROD_0 + sQueue.front())).GetGIEntry_Copy();
    Player* player = GET_PLAYER(gPlayState);
    bool holdsRod = player->heldItemAction == PLAYER_IA_FISHING_POLE;
    if (GiveItemEntryWithoutActor(gPlayState, entry) && !sAsked) {
        sAsked = true;
        if (!sInRun) { // the first item of a run: remember the rod (the next ones start with it put away)
            sInRun = true;
            sHadRod = holdsRod;
        }
        RF3Rod_LogEvent("item-queue-start", sQueue.front());
    }
}

void RegisterRF3Items() {
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnPlayerUpdate>([]() { UpdateQueue(); });
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnLoadFile>([](int32_t fileNum) {
        sQueue.clear(); // another file: its items are its own
        sAsked = false;
        sInRun = false;
        sRestoreRod = false;
        sInGet = false;
        sGetHadRod = false;
    });
}

// Which kind group the offset is in; -1 if out of range. base = the group's first offset.
int32_t Group(int32_t offset, int32_t* base) {
    int32_t start = 0;
    for (int32_t g = 0; g < 5; g++) {
        if (offset >= start && offset < start + kSizes[g]) {
            *base = start;
            return g;
        }
        start += kSizes[g];
    }
    return -1;
}

} // namespace

static RegisterShipInitFunc initRF3Items(RegisterRF3Items);

extern "C" void RF3Items_Queue(int32_t offset) {
    if (offset < 0 || offset >= RF3_ITEM_COUNT) {
        return;
    }
    sQueue.push_back(offset);
    RF3Rod_LogEvent("item-queue", offset);
}

extern "C" int32_t RF3Items_QueueLength(void) {
    return static_cast<int32_t>(sQueue.size());
}

extern "C" int32_t RF3Items_Kind(int32_t offset) {
    if (offset >= kGroupedCount && offset < RF3_ITEM_COUNT) {
        return kExtraKind[offset - kGroupedCount];
    }
    int32_t base = 0;
    int32_t g = Group(offset, &base);
    return g < 0 ? -1 : kKinds[g];
}

extern "C" int32_t RF3Items_Index(int32_t offset) {
    if (offset >= kGroupedCount && offset < RF3_ITEM_COUNT) {
        return kExtraIndex[offset - kGroupedCount];
    }
    int32_t base = 0;
    int32_t g = Group(offset, &base);
    return g < 0 ? -1 : offset - base;
}

extern "C" const char* RF3Items_KindName(int32_t offset) {
    if (offset >= kGroupedCount && offset < RF3_ITEM_COUNT) {
        return kExtraKind[offset - kGroupedCount] == RF3_STOCK_ROD ? "Rod" : "Lure";
    }
    int32_t base = 0;
    int32_t g = Group(offset, &base);
    return g < 0 ? "?" : kNames[g];
}

extern "C" int32_t RF3Items_Offset(int32_t kind, int32_t index) {
    for (int32_t e = 0; e < RF3_ITEM_COUNT - kGroupedCount; e++) {
        if (kExtraKind[e] == kind && kExtraIndex[e] == index) {
            return kGroupedCount + e;
        }
    }
    int32_t start = 0;
    for (int32_t g = 0; g < 5; g++) {
        if (kKinds[g] == kind) {
            return (index >= 0 && index < kSizes[g]) ? start + index : -1;
        }
        start += kSizes[g];
    }
    return -1;
}

// Step 28: the item's offset in the RF3 item block (RandomizerGet - RG_RF3_ROD_0), from the get-item entry.
static int32_t GiOffset(const GetItemEntry* e) {
    int32_t offset = (e != nullptr) ? static_cast<int32_t>(e->getItemId) - RG_RF3_ROD_0 : -1;
    return (offset >= 0 && offset < RF3_ITEM_COUNT) ? offset : -1;
}

extern "C" void RF3Items_DrawTackleGI(PlayState* play, GetItemEntry* getItemEntry) {
    // Step 28a: the item's own RF3 model when it has one (OoT's pole lures 91/92 keep the pond lure).
    int32_t offset = GiOffset(getItemEntry);
    float size = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.GiTackleSize"), kDefaultGiTackleSize);
    if (offset >= 0 && RF3_IsEnabled() &&
        RF3LureModel_DrawGI(play, RF3Items_Kind(offset), RF3Items_Index(offset), size)) {
        return;
    }
    float scale = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.GiLureScale"), kDefaultLureScale);
    OPEN_DISPS(play->state.gfxCtx);
    Gfx_SetupDL_25Opa(play->state.gfxCtx);
    Matrix_Push();
    Matrix_Scale(scale, scale, scale, MTXMODE_APPLY);
    Matrix_RotateZ(M_PI / 2, MTXMODE_APPLY); // as the pond orients it: the body along the item's long axis
    Matrix_RotateY(M_PI / 2, MTXMODE_APPLY);
    gSPMatrix(POLY_OPA_DISP++, Matrix_NewMtx(play->state.gfxCtx, (char*)__FILE__, __LINE__),
              G_MTX_MODELVIEW | G_MTX_LOAD);
    gSPDisplayList(POLY_OPA_DISP++, (Gfx*)gFishingLureFloatDL);
    Matrix_Pop();
    CLOSE_DISPS(play->state.gfxCtx);
}

// Step 28b: a rod's get-item model: its RF3 rod (OoT's pole, rod 10, keeps OoT's GI pole).
extern "C" void RF3Items_DrawRodGI(PlayState* play, GetItemEntry* getItemEntry) {
    int32_t offset = GiOffset(getItemEntry);
    float length = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.GiRodLength"), kDefaultGiRodLength);
    if (offset >= 0 && RF3_IsEnabled() && RF3Items_Kind(offset) == RF3_STOCK_ROD &&
        RF3RodModel_DrawGI(play, RF3Items_Index(offset), length)) {
        return;
    }
    Randomizer_DrawFishingPoleGI(play, getItemEntry);
}

extern "C" void RF3Items_Receive(int32_t offset) {
    int32_t kind = RF3Items_Kind(offset);
    if (kind < 0) {
        return;
    }
    // Step 22c: the queued item being shown is already in the stock.
    if (!sQueue.empty() && sQueue.front() == offset) {
        sQueue.pop_front();
        sAsked = false;
        RF3Rod_LogEvent("item-shown", offset);
        if (sQueue.empty()) {
            sInRun = false;
            if (sHadRod) {
                sRestoreRod = true; // after the text closes (UpdateQueue)
                sRestoreFrames = kRestoreTries;
                sHadRod = false;
            }
        }
        return;
    }
    RF3Stock_Give(kind, RF3Items_Index(offset), "item-get");
    if (IS_RANDO) { // step 24d: the tracker and logic see it
        Flags_SetRandomizerInf(RF3Rando_OwnedFlag(static_cast<RandomizerGet>(RG_RF3_ROD_0 + offset)));
    }
}

extern "C" void RF3Items_ReceiveSohFishingPole(void) {
    // Step 24d: SoH's own Fishing Pole item (randomizer) is OoT's pole, RF3's rod 10.
    RF3Items_Receive(RF3Items_Offset(RF3_STOCK_ROD, 10));
}

extern "C" int32_t RF3Items_GiveWithAnimation(int32_t offset) {
    if (gPlayState == nullptr || offset < 0 || offset >= RF3_ITEM_COUNT) {
        return 0;
    }
    GetItemEntry entry =
        Rando::StaticData::RetrieveItem(static_cast<RandomizerGet>(RG_RF3_ROD_0 + offset)).GetGIEntry_Copy();
    return GiveItemEntryWithoutActor(gPlayState, entry) ? 1 : 0;
}
