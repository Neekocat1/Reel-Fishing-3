// Reel Fishing III mod (step 15b): the pause menu's Fishing page.
// A C-Up tap on the Equipment page swaps it for this page (the same idea as SoH's randomizer page
// on the Quest page). It lists the fishing rod, greyed until Link owns it (step 15a). A D-pad press
// on it puts the rod on that D-pad button (SoH's D-pad equips). Later the page will also list RF3's
// lures and rod types. See docs/DESIGN.md step 15.

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <libultraship/bridge/consolevariablebridge.h>
#include <ship/Context.h>

#include "soh/Enhancements/kaleido.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"
#include "soh/cvar_prefixes.h"
#include "RF3Bridge.h"

extern "C" {
#include "z64.h"
#include "functions.h"
#include "macros.h"
#include "variables.h"
#include "textures/icon_item_static/icon_item_static.h"
void KaleidoScope_MoveCursorToSpecialPos(PlayState* play, u16 specialPos);
}

namespace {

// The rod's line on the page: SoH's icon entry, greyed until owned, with a hint as its text.
class RodEntry final : public Rando::KaleidoEntryIcon {
  public:
    RodEntry()
        : KaleidoEntryIcon(gItemIconFishingPoleTex, G_IM_FMT_RGBA, G_IM_SIZ_32b, 32, 32,
                           Color_RGBA8{ 255, 255, 255, 255 }, "Fishing Rod") {
        BuildVertices();
    }

    void Update(PlayState* play) override {
        mAchieved = RF3Rod_IsOwned() != 0;
        std::string text = "Fishing Rod";
        if (!mAchieved) {
            text += " - rent it at the pond";
        } else if (!CVarGetInteger(CVAR_ENHANCEMENT("DpadEquips"), 0)) {
            text += " - turn on D-pad equips";
        } else {
            text += " - D-pad to equip";
        }
        if (text != mText) {
            mText = text;
            RebuildVertices();
        }
    }
};

bool sPageActive = false;
bool sCUpTapPending = false; // C-Up is held and no D-pad button was pressed since
std::unique_ptr<RodEntry> sRodEntry;
std::vector<Gfx> sPageDl;

void LogEquip(int buttonIndex) {
    std::string dir = Ship::Context::GetPathRelativeToAppDirectory("logs");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(dir + "/rf3_records.txt", std::ios::app);
    char stamp[32];
    std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    char line[96];
    std::snprintf(line, sizeof(line), "%s rod-equip button=%d (4 up, 5 down, 6 left, 7 right)\n", stamp, buttonIndex);
    out << line;
}

// Put the rod on button 4..7 (D-pad up, down, left, right). If it is already on another button, that
// button takes the target's old item, as the item page does.
void EquipRod(PlayState* play, int buttonIndex) {
    u8 oldItem = gSaveContext.equips.buttonItems[buttonIndex];
    u8 oldSlot = gSaveContext.equips.cButtonSlots[buttonIndex - 1];
    for (int other = 1; other < ARRAY_COUNT(gSaveContext.equips.buttonItems); other++) {
        if (other != buttonIndex && gSaveContext.equips.buttonItems[other] == ITEM_FISHING_POLE) {
            gSaveContext.equips.buttonItems[other] = oldItem;
            gSaveContext.equips.cButtonSlots[other - 1] = oldSlot;
            if (oldItem != ITEM_NONE) {
                Interface_LoadItemIcon2(play, other);
            }
        }
    }
    gSaveContext.equips.buttonItems[buttonIndex] = ITEM_FISHING_POLE;
    gSaveContext.equips.cButtonSlots[buttonIndex - 1] = SLOT_NONE; // no inventory slot (DESIGN step 15b)
    Interface_LoadItemIcon1(play, buttonIndex);
    Audio_PlaySfxGeneral(NA_SE_SY_DECIDE, &gSfxDefaultPos, 4, &gSfxDefaultFreqAndVolScale,
                         &gSfxDefaultFreqAndVolScale, &gSfxDefaultReverb);
    LogEquip(buttonIndex);
}

void PlayCursorSfx() {
    Audio_PlaySfxGeneral(NA_SE_SY_CURSOR, &gSfxDefaultPos, 4, &gSfxDefaultFreqAndVolScale,
                         &gSfxDefaultFreqAndVolScale, &gSfxDefaultReverb);
}

// Input on the page: the D-pad equips the rod (same rule as the item page), the stick and, with
// "D-pad on pause", the D-pad move to the page-turn arrows.
void HandleInput(PlayState* play) {
    PauseContext* pauseCtx = &play->pauseCtx;
    Input* input = &play->state.input[0];
    bool dpadOnPause = CVarGetInteger(CVAR_SETTING("DPadOnPause"), 0) != 0;

    if (pauseCtx->cursorSpecialPos == 0) {
        bool canEquip = RF3Rod_IsOwned() && CVarGetInteger(CVAR_ENHANCEMENT("DpadEquips"), 0) &&
                        (!dpadOnPause || CHECK_BTN_ALL(input->cur.button, BTN_CUP));
        static const u16 kDpad[4] = { BTN_DUP, BTN_DDOWN, BTN_DLEFT, BTN_DRIGHT };
        for (int d = 0; d < 4; d++) {
            if (CHECK_BTN_ALL(input->press.button, kDpad[d])) {
                if (canEquip) {
                    EquipRod(play, 4 + d);
                    return;
                }
                if (!dpadOnPause) {
                    Audio_PlaySfxGeneral(NA_SE_SY_ERROR, &gSfxDefaultPos, 4, &gSfxDefaultFreqAndVolScale,
                                         &gSfxDefaultFreqAndVolScale, &gSfxDefaultReverb);
                    return;
                }
            }
        }
        if ((pauseCtx->stickRelX < -30) || (dpadOnPause && CHECK_BTN_ALL(input->press.button, BTN_DLEFT))) {
            KaleidoScope_MoveCursorToSpecialPos(play, PAUSE_CURSOR_PAGE_LEFT);
        } else if ((pauseCtx->stickRelX > 30) || (dpadOnPause && CHECK_BTN_ALL(input->press.button, BTN_DRIGHT))) {
            KaleidoScope_MoveCursorToSpecialPos(play, PAUSE_CURSOR_PAGE_RIGHT);
        }
    } else if (pauseCtx->cursorSpecialPos == PAUSE_CURSOR_PAGE_LEFT) {
        if ((pauseCtx->stickRelX > 30) || (dpadOnPause && CHECK_BTN_ALL(input->press.button, BTN_DRIGHT))) {
            pauseCtx->cursorSpecialPos = 0;
            PlayCursorSfx();
        }
    } else if (pauseCtx->cursorSpecialPos == PAUSE_CURSOR_PAGE_RIGHT) {
        if ((pauseCtx->stickRelX < -30) || (dpadOnPause && CHECK_BTN_ALL(input->press.button, BTN_DLEFT))) {
            pauseCtx->cursorSpecialPos = 0;
            PlayCursorSfx();
        }
    }
}

} // namespace

extern "C" int RF3Pause_FishingPageActive(void) {
    return (sPageActive && RF3_IsEnabled()) ? 1 : 0;
}

// Every frame of pause state 6 (no prompt open), on any page: a C-Up tap on the Equipment page
// toggles the Fishing page.
extern "C" void RF3Pause_UpdateToggle(PlayState* play) {
    PauseContext* pauseCtx = &play->pauseCtx;
    Input* input = &play->state.input[0];
    if (!RF3_IsEnabled() || pauseCtx->pageIndex != PAUSE_EQUIP) {
        sCUpTapPending = false;
        return;
    }
    if (CHECK_BTN_ALL(input->press.button, BTN_CUP)) {
        sCUpTapPending = true;
    }
    if (CHECK_BTN_ANY(input->press.button, BTN_DUP | BTN_DDOWN | BTN_DLEFT | BTN_DRIGHT)) {
        sCUpTapPending = false; // C-Up was a modifier for a D-pad equip, not a tap
    }
    if (sCUpTapPending && !CHECK_BTN_ALL(input->cur.button, BTN_CUP)) {
        sCUpTapPending = false;
        sPageActive = !sPageActive;
        Audio_PlaySfxGeneral(NA_SE_SY_DECIDE, &gSfxDefaultPos, 4, &gSfxDefaultFreqAndVolScale,
                             &gSfxDefaultFreqAndVolScale, &gSfxDefaultReverb);
    }
}

// Draws the page's entries (the caller draws the blank page sections). handleInput: this is the page
// being shown and the menu is idle.
extern "C" void RF3Pause_DrawFishingPage(PlayState* play, int handleInput) {
    PauseContext* pauseCtx = &play->pauseCtx;
    if (!sRodEntry) {
        sRodEntry = std::make_unique<RodEntry>();
    }
    sRodEntry->Update(play);
    if (handleInput && (pauseCtx->state == 6) && (pauseCtx->unk_1E4 == 0)) {
        HandleInput(play);
    }
    if (handleInput) {
        pauseCtx->cursorItem[PAUSE_EQUIP] = PAUSE_ITEM_NONE; // no equipment name under this page
    }

    sPageDl.clear();
    OPEN_DISPS(play->state.gfxCtx);
    sPageDl.push_back(gsDPPipeSync());
    Gfx_SetupDL_39Opa(play->state.gfxCtx);
    Gfx_SetupDL_42Opa(play->state.gfxCtx);
    sPageDl.push_back(gsDPSetCombineMode(G_CC_MODULATEIA_PRIM, G_CC_MODULATEIA_PRIM));
    // Same frame as SoH's randomizer page: origin at the page's top-left, y down.
    Matrix_Translate(-108.f, 58.f, 0.0f, MTXMODE_APPLY);
    Matrix_Scale(1.0f, -1.0f, 1.0f, MTXMODE_APPLY);
    Matrix_Push();
    sRodEntry->SetYOffset(1);
    sRodEntry->SetSelected(handleInput && pauseCtx->cursorSpecialPos == 0);
    sRodEntry->Draw(play, &sPageDl);
    Matrix_Pop();
    sPageDl.push_back(gsSPEndDisplayList());
    gSPDisplayList(POLY_OPA_DISP++, sPageDl.data());
    CLOSE_DISPS(play->state.gfxCtx);
}
