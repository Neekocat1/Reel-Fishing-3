// Reel Fishing III mod (step 15c): using Link's own rod from the D-pad.
// - At the pond: the rod's D-pad button starts a session as the paid rental does (the pond puts the
//   rod on B), so Link's own rod works there too.
// - Anywhere else: Link takes the rod out and casts as at the pond. While he holds it, a portable
//   instance of the pond actor (z_fishing.c, EN_FISH_PORTABLE) runs the rod, line and lure.
// See docs/DESIGN.md step 15.

#include <libultraship/bridge/consolevariablebridge.h>

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"
#include "soh/cvar_prefixes.h"
#include "RF3Bridge.h"

extern "C" {
#include "z64.h"
#include "functions.h"
#include "macros.h"
#include "variables.h"
extern PlayState* gPlayState;
// z_fishing.c (step 15c)
s32 Fishing_RF3PortableActive(void);
void Fishing_RF3SpawnPortable(PlayState* play);
s32 Fishing_RF3StartOwnRod(PlayState* play);
s32 Fishing_RF3PutAwayRod(PlayState* play);
// z_player.c (public, also used by SoH's AlwaysOnFixes.cpp)
void Player_UseItem(PlayState* play, Player* player, s32 item);
}

namespace {

bool InPond(PlayState* play) {
    return play->sceneNum == SCENE_FISHING_POND;
}

// Step 15e: a save written while the portable rod was out has the rod on B. B's real item is not in
// the save, so give B the equipped sword again (as OoT shows it), or nothing.
void RestoreBAfterLoad() {
    if (gSaveContext.equips.buttonItems[0] != ITEM_FISHING_POLE) {
        return;
    }
    switch (CUR_EQUIP_VALUE(EQUIP_TYPE_SWORD)) {
        case EQUIP_VALUE_SWORD_KOKIRI:
            gSaveContext.equips.buttonItems[0] = ITEM_SWORD_KOKIRI;
            break;
        case EQUIP_VALUE_SWORD_MASTER:
            gSaveContext.equips.buttonItems[0] = ITEM_SWORD_MASTER;
            break;
        case EQUIP_VALUE_SWORD_BIGGORON:
            // Same rule as the Equipment page (z_kaleido_equipment.c): the knife only when it is broken.
            gSaveContext.equips.buttonItems[0] =
                ((gSaveContext.bgsFlag == 0) && CHECK_OWNED_EQUIP_ALT(EQUIP_TYPE_SWORD, EQUIP_INV_SWORD_BROKENGIANTKNIFE))
                    ? ITEM_SWORD_KNIFE
                    : ITEM_SWORD_BGS;
            break;
        default:
            gSaveContext.equips.buttonItems[0] = ITEM_NONE;
            break;
    }
    RF3Rod_LogEvent("b-restored-on-load", gSaveContext.equips.buttonItems[0]);
}

void RegisterRF3Rod() {
    // The rod's D-pad button. Player has already read the item (ITEM_NONE where the scene disables
    // items, as the pond does), so look at the button's own item.
    COND_VB_SHOULD(VB_OVERRIDE_BUTTON_ITEM_USED, CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.Enabled"), 0), {
        s32* button = va_arg(args, s32*);
        va_arg(args, Player*);
        s32* item = va_arg(args, s32*);
        do { // break instead of return: SoH's macro ends the va_list after this body
            if ((gPlayState == nullptr) || (*button < 4) || (*button >= 8) ||
                (gSaveContext.equips.buttonItems[*button] != ITEM_FISHING_POLE)) {
                break;
            }
            if (!RF3Rod_IsOwned()) {
                *item = ITEM_NONE;
                Sfx_PlaySfxCentered(NA_SE_SY_ERROR);
                break;
            }
            if (InPond(gPlayState)) {
                // Start the pond's own session (rod on B); during a session B already holds the rod.
                if (Fishing_RF3StartOwnRod(gPlayState)) {
                    RF3Rod_LogEvent("own-rod-at-pond", gPlayState->sceneNum);
                }
                *item = ITEM_NONE;
                break;
            }
            // Step 15e: with the rod out, its D-pad button puts it away and B gets its item back (the cast
            // is on B). Ignored while the line is out.
            Player* player = GET_PLAYER(gPlayState);
            if (player->heldItemAction == PLAYER_IA_FISHING_POLE) {
                if (Fishing_RF3PutAwayRod(gPlayState)) {
                    Player_UseItem(gPlayState, player, ITEM_NONE);
                    RF3Rod_LogEvent("rod-put-away", gPlayState->sceneNum);
                }
                *item = ITEM_NONE;
                break;
            }
            if (*item == ITEM_FISHING_POLE) {
                // Before the rod is in hand, so the first cast has it. The instance puts the rod on B.
                Fishing_RF3SpawnPortable(gPlayState);
            }
        } while (0);
    });

    // The cast check: the vanilla test refuses z > 1300 (the pond's dock). Outside the pond only the
    // rest of it applies: Link must stand on the ground with room above the rod.
    COND_VB_SHOULD(VB_NOT_CAST_FISHING, CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.Enabled"), 0), {
        Vec3f* rodCheckPos = va_arg(args, Vec3f*);
        if ((gPlayState != nullptr) && !InPond(gPlayState)) {
            Player* player = GET_PLAYER(gPlayState);
            *should = !(player->actor.bgCheckFlags & BGCHECKFLAG_GROUND) ||
                      BgCheck_SphVsFirstPoly(&gPlayState->colCtx, rodCheckPos, 20.0f);
        }
    });

    COND_HOOK(OnLoadGame, CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.Enabled"), 0),
              [](int32_t fileNum) { RestoreBAfterLoad(); });

    // Link holds the rod outside the pond (after a scene change, or the instance ended): run the
    // portable instance.
    COND_HOOK(OnPlayerUpdate, CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.Enabled"), 0), []() {
        if ((gPlayState == nullptr) || InPond(gPlayState)) {
            return;
        }
        Player* player = GET_PLAYER(gPlayState);
        if ((player != nullptr) && (player->heldItemAction == PLAYER_IA_FISHING_POLE) &&
            !Fishing_RF3PortableActive()) {
            Fishing_RF3SpawnPortable(gPlayState);
        }
    });
}

} // namespace

static RegisterShipInitFunc initRF3Rod(RegisterRF3Rod, { CVAR_ENHANCEMENT("ReelFishing3.Enabled") });
