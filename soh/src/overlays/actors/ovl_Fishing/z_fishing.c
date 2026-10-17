/*
 * File: z_fishing.c
 * Overlay: ovl_Fishing
 * Description: Fishing Pond Elements (Owner, Fish, Props, Effects...)
 */

#include "z_fishing.h"

#include "overlays/actors/ovl_En_Kanban/z_en_kanban.h"
#include "objects/object_fish/object_fish.h"
#include "vt.h"

#include "soh/frame_interpolation.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/Enhancements/ReelFishing/RF3Log.h"
#include "soh/Enhancements/ReelFishing/RF3Bridge.h"
#include "soh/Enhancements/ReelFishing/RF3LureModel.h"
#include "soh/Enhancements/ReelFishing/RF3FishModel.h"
#include "soh/Enhancements/ReelFishing/RF3RodModel.h"
#include "soh/Enhancements/ReelFishing/RF3FishMove.h"
// Reel Fishing III mod (step 25c-2): this frame's lure is RF3's own model, so OoT's lure and hook models are not
// drawn (the hook points and the owner's hat still work).
static u8 sRF3LureModelShown = false;
// Step 32a: the line has no lure (it snapped off; RF3Tackle_LureMissing): nothing is drawn at its end.
static u8 sRF3NoLure = false;

// Reel Fishing III mod: RF3 steps are 1/60 s; OoT's game logic runs R_UPDATE_RATE of them per
// game frame (3 = 20 Hz in normal play).
#define RF3_SUBSTEPS ((R_UPDATE_RATE > 0) ? R_UPDATE_RATE : 1)
// OoT only lets fish line up and bite within 800 units of the pond centre (states 2-4).
#define RF3_LURE_IN_BITE_AREA() (Fishing_RF3LureInBiteArea()) // pond: within 800 of the centre; step 16d: region
// Step 11: B held during OoT's cast wind-up (sRodCastTimer 37 -> 18) charges RF3 cast power.
#define RF3_CAST_WINDUP_FRAMES 19
static s16 sRF3CastHeldFrames = 0;
static u8 sRF3CastCharging = false;
#include <libultraship/bridge/consolevariablebridge.h>
#include <stdio.h> // step 26e-8: snprintf

#define FLAGS ACTOR_FLAG_UPDATE_CULLING_DISABLED
// Reel Fishing III mod (step 15c): the rod used outside the pond. A portable instance of this actor
// (params EN_FISH_PORTABLE) runs the rod, line and lure in any scene. Its water and ground come from
// under the lure instead of the pond's fixed layout (first water box, 920-unit radius). See
// docs/DESIGN.md step 15.
#define EN_FISH_PORTABLE 300
static u8 sRF3Portable = false;         // a portable instance is running (never in the pond)
static u8 sRF3PortableInWater = false;  // the lure is over a water box whose surface is above the ground
static f32 sRF3PortableSurfaceY = 0.0f; // that surface; over land, 50 units below the ground
static f32 sRF3PortableFloorY = 0.0f;   // the ground under the lure
static u16 sRF3RodBtn = BTN_B;          // the button holding the rod: B at the pond, else a D-pad button
static u8 sRF3Cancelled = false;        // step 20a-6: the rod button cancelled a fight; no "got away" text

// Reel Fishing III mod (step 16d): fish in hand-placed regions outside the pond (params EN_FISH_REGION
// + slot). While a region fish updates or draws, sRF3CurRegion is its region: its water surface and box
// stand in for the pond's.
#define EN_FISH_REGION 400
#define RF3_REGION_SLOTS 64
#define RF3_IS_REGION_FISH(fish) ((fish)->actor.params >= EN_FISH_REGION)
typedef struct {
    f32 minX, maxX, minZ, maxZ, surfaceY; // the box in its own frame (step 16i: turned by angle)
    f32 angle;                            // radians, about the box centre
} RF3RegionBox;
static RF3RegionBox sRF3RegionBoxes[RF3_REGION_SLOTS];
static RF3RegionBox* sRF3CurRegion = NULL;
static Vec3f sRF3RegionLastGood[RF3_REGION_SLOTS]; // step 16f: last spot over water deep enough
#define RF3_REGION_MIN_DEPTH 15.0f                 // step 16f: shallower than this counts as shore
// Step 26e-8: a hooked region fish's own wall test (Fishing_RF3WallTest).
static Vec3f sRF3WallPrev[RF3_REGION_SLOTS];    // the fish's position at the end of its last update
static u32 sRF3WallPrevFrame[RF3_REGION_SLOTS]; // play->gameplayFrames of that update
static s32 sRF3WallContact[RF3_REGION_SLOTS];   // frames in a row with a hit (0 = clear)
#define RF3_WALL_MARGIN 10.0f                   // [assumed] how far off a wall's plane the fish is kept
// Step 26e-9: the shore probe (Fishing_RF3FloorAt) starts this far above the water [assumed: above the Lake Hylia
// island's +43; a lower bridge deck would count as land]
#define RF3_SHORE_PROBE_HEIGHT 60.0f
static s32 sRF3ShoreBack[RF3_REGION_SLOTS]; // step 26e-9: frames in a row the shore rule sent the fish back

#define WATER_SURFACE_Y(play)                         \
    ((sRF3CurRegion != NULL) ? sRF3CurRegion->surfaceY \
                             : (sRF3Portable ? sRF3PortableSurfaceY : play->colCtx.colHeader->waterBoxes->ySurface))

// Step 20a-5: OoT's sinking lure sinks only with RF3 off; with RF3 on, a lure box's lure follows RF3's
// lure depth and flies and bait sets stay at the surface (user's decisions, 2026-10-07).
// Step 23c: with RF3 on, only OoT's pole (box 10) with its sinking lure sinks the OoT way.
#define FISHING_LURE_SINKS() ((sLureEquipped == FS_LURE_SINKING) && (!RF3_IsEnabled() || (RF3Tackle_PoleLure() == 1)))
// Step 20c-3c: while a fly cast charges, OoT's cast timer is held here (the launch is at 18).
#define RF3_FLY_HOLD_TIMER 30
// Step 20a-6: with the lure sim active, a lure this close to the rod tip horizontally (OoT units) is reeled in.
#define RF3_LURE_REELED_IN_UNITS 10.0f

// Is the lure (or a line point) on land? The pond: beyond its 920-unit radius. Portable: no water under the lure.
static s32 Fishing_RF3OnLand(f32 xzLenSq) {
    return sRF3Portable ? !sRF3PortableInWater : (xzLenSq > SQ(920.0f));
}

// Ground height for a lure on land. The pond: its bank slope. Portable: the ground under the lure.
static f32 Fishing_RF3LandY(PlayState* play, f32 xzLenSq, f32 slope) {
    return sRF3Portable ? sRF3PortableFloorY : (((sqrtf(xzLenSq) - 920.0f) * slope) + WATER_SURFACE_Y(play));
}

void Fishing_Init(Actor* thisx, PlayState* play);
void Fishing_Destroy(Actor* thisx, PlayState* play);
void Fishing_UpdateFish(Actor* thisx, PlayState* play);
void Fishing_UpdateOwner(Actor* thisx, PlayState* play);
void Fishing_DrawFish(Actor* thisx, PlayState* play);
void Fishing_DrawOwner(Actor* thisx, PlayState* play);
void Fishing_Reset(void);
static void Fishing_RF3InitPortable(Fishing* this, PlayState* play);
static void Fishing_RF3DestroyPortable(PlayState* play);
static void Fishing_UpdateCinematics(Fishing* this, PlayState* play);
static s32 Fishing_RF3LureInBiteArea(void);
static void Fishing_RF3KeepInWater(Fishing* this, PlayState* play);
static void Fishing_RF3WallTest(Fishing* this, PlayState* play);
static f32 Fishing_RF3ShoreDistance(Fishing* this, PlayState* play);
static s32 Fishing_RF3OutOfArea(f32 x, f32 z);
static s32 Fishing_RF3FishIndex(Fishing* this);
static s32 Fishing_RF3ApproachMove(Fishing* this, PlayState* play);
static f32 Fishing_RF3Perception(Fishing* this);
static void Fishing_RF3RegionTarget(Fishing* this);
static void Fishing_RF3ClampFightTarget(Vec3f* target);
static s32 Fishing_RF3PointInWater(f32 x, f32 z);
static s32 Fishing_RF3Probe(const f32 from[3], const f32 to[3], f32 normal[3]);
static f32 Fishing_RF3FloorAt(PlayState* play, f32 x, f32 z);
static PlayState* sRF3MovePlay = NULL; // step 26e-4: the play state for Fishing_RF3PointInWater's water probe
static Fishing* sRF3MoveFish = NULL;   // step 26f-2: the fish RF3FishMove is probing for (Fishing_RF3Probe)
// Step 30b-1d: the fish RF3FishMove moved, and on which frame (OoT's line leash skips it the next frame)
static Fishing* sRF3MovedFish = NULL;
static u32 sRF3MovedFrame = 0;
static s32 sRF3LeashSkips = 0; // skips since the last rf3-leash line
static u32 sRF3LeashLogFrame = 0;
static u32 sRF3ReelTurnLogFrame = 0; // step 30b-1e: the last rf3-reel-turn line
#define RF3_OOT_WALL_HEIGHT 30.0f       // step 26f-2: OoT's fish wall check (Actor_UpdateBgCheckInfo in Fishing_UpdateFishImpl)
#define RF3_OOT_WALL_RADIUS 30.0f
#define RF3_PROBE_LAND_STEP 10.0f // step 26f-3: spacing of the probe's land tests [assumed]
static void Fishing_RF3InitRegionFish(Fishing* this, PlayState* play);

typedef struct {
    /* 0x00 */ u8 isLoach;
    /* 0x02 */ Vec3s pos;
    /* 0x08 */ u8 baseLength;
    /* 0x0C */ f32 perception;
} FishingFishInit; // size = 0x10

typedef enum {
    /* 0x00 */ FS_EFF_NONE,
    /* 0x01 */ FS_EFF_RIPPLE,
    /* 0x02 */ FS_EFF_DUST_SPLASH,
    /* 0x03 */ FS_EFF_WATER_DUST,
    /* 0x04 */ FS_EFF_BUBBLE,
    /* 0x05 */ FS_EFF_RAIN_DROP,
    /* 0x06 */ FS_EFF_OWNER_HAT,
    /* 0x07 */ FS_EFF_RAIN_RIPPLE,
    /* 0x08 */ FS_EFF_RAIN_SPLASH
} FishingEffectType;

#define FISHING_EFFECT_COUNT 130

typedef struct {
    /* 0x00 */ Vec3f pos;
    /* 0x0C */ Vec3f vel;
    /* 0x18 */ Vec3f accel;
    /* 0x24 */ u8 type;
    /* 0x25 */ u8 timer;
    /* 0x26 */ char unk_26[0x04];
    /* 0x2A */ s16 alpha;
    /* 0x2C */ s16 state;
    /* 0x2E */ s16 alphaMax;
    /* 0x30 */ f32 scale;
    /* 0x34 */ Vec3f rot;
    u32 epoch;
} FishingEffect; // size = 0x40

#define POND_PROP_COUNT 140

typedef enum {
    /* 0x00 */ FS_PROP_NONE,
    /* 0x01 */ FS_PROP_REED,
    /* 0x02 */ FS_PROP_LILY_PAD,
    /* 0x03 */ FS_PROP_ROCK,
    /* 0x04 */ FS_PROP_WOOD_POST,
    /* 0x23 */ FS_PROP_INIT_STOP = 0x23
} FishingPropType;

typedef struct {
    /* 0x00 */ u8 type;
    /* 0x02 */ Vec3s pos;
} FishingPropInit; // size = 0x08

typedef struct {
    /* 0x00 */ Vec3f pos;
    /* 0x0C */ f32 rotX;
    /* 0x10 */ f32 rotY;
    /* 0x14 */ f32 reedAngle;
    /* 0x18 */ Vec3f projectedPos;
    /* 0x24 */ f32 scale;
    /* 0x28 */ s16 lilyPadAngle;
    /* 0x2C */ f32 lilyPadOffset;
    /* 0x30 */ u8 type;
    /* 0x32 */ s16 timer;
    /* 0x34 */ u8 shouldDraw;
    /* 0x38 */ f32 drawDistance;
    u32 epoch;
} FishingProp; // size = 0x3C

typedef enum {
    /* 0x00 */ FS_GROUP_FISH_NONE,
    /* 0x01 */ FS_GROUP_FISH_NORMAL
} FishingGroupFishType;

#define GROUP_FISH_COUNT 60

typedef struct {
    /* 0x00 */ u8 type;
    /* 0x02 */ s16 timer;
    /* 0x04 */ Vec3f pos;
    /* 0x10 */ Vec3f homePos;
    /* 0x1C */ Vec3f projectedPos;
    /* 0x28 */ f32 velY;
    /* 0x2C */ f32 scaleX;
    /* 0x30 */ f32 unk_30;
    /* 0x34 */ f32 unk_34;
    /* 0x38 */ f32 unk_38;
    /* 0x3C */ s16 unk_3C;
    /* 0x3E */ s16 unk_3E;
    /* 0x40 */ s16 unk_40;
    /* 0x42 */ s16 unk_42;
    /* 0x44 */ u8 shouldDraw;
    u32 epoch;
} FishingGroupFish; // size = 0x48

typedef enum {
    /* 0x00 */ FS_LURE_STOCK,
    /* 0x01 */ FS_LURE_UNK, // hinted at with an "== 1"
    /* 0x02 */ FS_LURE_SINKING
} FishingLureTypes;

#define LINE_SEG_COUNT 200
#define SINKING_LURE_SEG_COUNT 20

const ActorInit Fishing_InitVars = {
    ACTOR_FISHING,
    ACTORCAT_NPC,
    FLAGS,
    OBJECT_FISH,
    sizeof(Fishing),
    (ActorFunc)Fishing_Init,
    (ActorFunc)Fishing_Destroy,
    (ActorFunc)Fishing_UpdateFish,
    (ActorFunc)Fishing_DrawFish,
    (ActorResetFunc)Fishing_Reset,
};

static f32 sStormStrength = 0.0f;

static u8 sStormStrengthTarget = 0;

static f32 sFishingStormShade = 0.0f;

static Vec3f sFishingStormSfxPos = { 0.0f, 0.0f, 0.0f };

static f32 sStormSfxFreqScale = 0.0f;

static u8 sSinkingLureLocation = 0;

/// Weight of caught fish.
static f32 sFishOnHandLength = 0.0f;

static u8 sIsRodVisible = true;

static u16 sFishLengthToWeigh = 0;

static u8 sFishingCaughtTextDelay = 0;

static s32 sFishingTimePlayed = 0;

static s16 sOwnerTheftTimer = 0;

typedef enum {
    /* 0x00 */ FS_OWNER_BALD,
    /* 0x01 */ FS_OWNER_CAPPED,
    /* 0x02 */ FS_OWNER_HAIR
} FishingOwnerHair;

static u8 sOwnerHair = FS_OWNER_BALD;
static u8 sIsOwnersHatHooked = false; // hat is on fishing hook
static u8 sIsOwnersHatSunk = false;   // hat is sinking into pond.

static s16 sRodCastState = 0;

static Vec3f sFishMouthOffset = { 500.0f, 500.0f, 0.0f };

static u8 D_80B7A6A4 = 0;

static f32 sRodBendRotY = 0.0f;
static f32 D_80B7A6AC = 0.0f;
static f32 D_80B7A6B0 = 0.0f;
static f32 D_80B7A6B4 = 0.0f;
static f32 D_80B7A6B8 = 0.0f;
static f32 D_80B7A6BC = 0.0f;
static f32 D_80B7A6C0 = 0.0f;

static s16 sStickAdjXPrev = 0;
static s16 sStickAdjYPrev = 0;

static u8 sFishingPlayerCinematicState = 0;
static u8 sFishingCinematicTimer = 0;
static u8 sSinkingLureFound = false;

static ColliderJntSphElementInit sJntSphElementsInit[12] = {
    {
        {
            ELEMTYPE_UNK0,
            { 0xFFCFFFFF, 0x00, 0x10 },
            { 0xFFCFFFFF, 0x00, 0x00 },
            TOUCH_NONE,
            BUMP_NONE,
            OCELEM_ON,
        },
        { 0, { { 0, 0, 0 }, 30 }, 100 },
    },
    {
        {
            ELEMTYPE_UNK0,
            { 0xFFCFFFFF, 0x00, 0x10 },
            { 0xFFCFFFFF, 0x00, 0x00 },
            TOUCH_NONE,
            BUMP_NONE,
            OCELEM_ON,
        },
        { 0, { { 0, 0, 0 }, 30 }, 100 },
    },
    {
        {
            ELEMTYPE_UNK0,
            { 0xFFCFFFFF, 0x00, 0x10 },
            { 0xFFCFFFFF, 0x00, 0x00 },
            TOUCH_NONE,
            BUMP_NONE,
            OCELEM_ON,
        },
        { 0, { { 0, 0, 0 }, 30 }, 100 },
    },
    {
        {
            ELEMTYPE_UNK0,
            { 0xFFCFFFFF, 0x00, 0x10 },
            { 0xFFCFFFFF, 0x00, 0x00 },
            TOUCH_NONE,
            BUMP_NONE,
            OCELEM_ON,
        },
        { 0, { { 0, 0, 0 }, 30 }, 100 },
    },
    {
        {
            ELEMTYPE_UNK0,
            { 0xFFCFFFFF, 0x00, 0x10 },
            { 0xFFCFFFFF, 0x00, 0x00 },
            TOUCH_NONE,
            BUMP_NONE,
            OCELEM_ON,
        },
        { 0, { { 0, 0, 0 }, 30 }, 100 },
    },
    {
        {
            ELEMTYPE_UNK0,
            { 0xFFCFFFFF, 0x00, 0x10 },
            { 0xFFCFFFFF, 0x00, 0x00 },
            TOUCH_NONE,
            BUMP_NONE,
            OCELEM_ON,
        },
        { 0, { { 0, 0, 0 }, 30 }, 100 },
    },
    {
        {
            ELEMTYPE_UNK0,
            { 0xFFCFFFFF, 0x00, 0x10 },
            { 0xFFCFFFFF, 0x00, 0x00 },
            TOUCH_NONE,
            BUMP_NONE,
            OCELEM_ON,
        },
        { 0, { { 0, 0, 0 }, 30 }, 100 },
    },
    {
        {
            ELEMTYPE_UNK0,
            { 0xFFCFFFFF, 0x00, 0x10 },
            { 0xFFCFFFFF, 0x00, 0x00 },
            TOUCH_NONE,
            BUMP_NONE,
            OCELEM_ON,
        },
        { 0, { { 0, 0, 0 }, 30 }, 100 },
    },
    {
        {
            ELEMTYPE_UNK0,
            { 0xFFCFFFFF, 0x00, 0x10 },
            { 0xFFCFFFFF, 0x00, 0x00 },
            TOUCH_NONE,
            BUMP_NONE,
            OCELEM_ON,
        },
        { 0, { { 0, 0, 0 }, 30 }, 100 },
    },
    {
        {
            ELEMTYPE_UNK0,
            { 0xFFCFFFFF, 0x00, 0x10 },
            { 0xFFCFFFFF, 0x00, 0x00 },
            TOUCH_NONE,
            BUMP_NONE,
            OCELEM_ON,
        },
        { 0, { { 0, 0, 0 }, 30 }, 100 },
    },
    {
        {
            ELEMTYPE_UNK0,
            { 0xFFCFFFFF, 0x00, 0x10 },
            { 0xFFCFFFFF, 0x00, 0x00 },
            TOUCH_NONE,
            BUMP_NONE,
            OCELEM_ON,
        },
        { 0, { { 0, 0, 0 }, 30 }, 100 },
    },
    {
        {
            ELEMTYPE_UNK0,
            { 0xFFCFFFFF, 0x00, 0x10 },
            { 0xFFCFFFFF, 0x00, 0x00 },
            TOUCH_NONE,
            BUMP_NONE,
            OCELEM_ON,
        },
        { 0, { { 0, 0, 0 }, 30 }, 100 },
    },
};

static ColliderJntSphInit sJntSphInit = {
    {
        COLTYPE_NONE,
        AT_TYPE_ENEMY,
        AC_TYPE_PLAYER,
        OC1_ON | OC1_TYPE_PLAYER,
        OC2_TYPE_1,
        COLSHAPE_JNTSPH,
    },
    12,
    sJntSphElementsInit,
};

static f32 sFishGroupVar = 0.0f;

static Vec3f sZeroVec = { 0.0f, 0.0f, 0.0f };
static Vec3f sUnusedVec = { 0.0f, 0.0f, 2000.0f };

static Fishing* sFishingMain;
static u8 sReelLock;
static u8 sLinkAge;
static u8 sFishingFoggy;
static u8 sStormChanceTimer;
static f32 sFishingRecordLength;
static u8 sFishOnHandIsLoach;
static u8 sFishGameNumber; // increments for each purchased play. effects weather
static u8 sLureCaughtWith;
static s16 sFishFightTime;
static u8 sPondOwnerTextIdIndex;
static u16 sFishesCaught;
static u16 sFishingCaughtTextId;
static s8 sLureCameraZoomLevel;
static Vec3f sOwnerHeadPos;
static Vec3s sEffOwnersHatRot;
static u8 sLureMoveDelay; // a small delay between the lure hitting the water, and being able to reel.
static s16 sRumbleDelay;
static s16 sFishingMusicDelay;
static Fishing* sFishingHookedFish;
static s16 sFishingPlayingState;
static s16 sLureTimer; // AND'd for various effects/checks
static s16 D_80B7E0B0;
static s16 D_80B7E0B2;
static s16 sRodCastTimer; // used for the inital line casting
static u8 sLureEquipped;
static Vec3f sLurePos;
static Vec3f sLureDrawPos;
static Vec3f sLureRot;
static Vec3f sLurePosDelta;
static Vec3f sLureCastDelta;
static f32 sLure1Rotate; // lure type 1 is programmed to change this.
static f32 sLurePosZOffset;
static f32 sLureRotXTarget;
static f32 sLureRotXStep;
static s8 D_80B7E114;
static s16 sRodPullback; // holding A+Down to keep the fish on line
static u8 D_80B7E118;
static f32 sRodReelingSpeed;
static u8 sWiggleAttraction;
static s16 sLureBitTimer;
static u8 sLineHooked;
static Vec3f sLureLineSegPosDelta;
static f32 sLureWiggleRotYTarget;
static f32 sLureWigglePosY;
static s16 sLureWiggleRotY;
static f32 sLureWiggleSign; // +/-1.0f
static f32 sRodLineSpooled; // 200 represents the full spool.
static f32 D_80B7E148;
static f32 sFishingLineScale;
static s16 D_80B7E150;
static f32 sReelLinePosStep;
static Vec3f sRodTipPos;
static Vec3f sReelLinePos[LINE_SEG_COUNT];
static Vec3f sReelLineRot[LINE_SEG_COUNT];
static Vec3f sReelLineUnk[LINE_SEG_COUNT];
static Vec3f sLureHookRefPos[2];
static f32 sLureHookRotY[2];
static u8 sRodHitTimer; // brief timer for bending rod when line is snapped
static Vec3f sSinkingLurePos[SINKING_LURE_SEG_COUNT];
static s16 sSinkingLureSegmentIndex;
static f32 sProjectedW;
static Vec3f sCameraEye;
static Vec3f sCameraAt;
static s16 sSubCamId;
static f32 sCatchCamX;
static f32 sSubCamVelFactor;
static f32 D_80B7FED0;
static Vec3f sSinkingLureBasePos;
static f32 sSinkingLureHeldY; // the lure going to its mark when being held up.
static s32 sRandSeed0;
static s32 sRandSeed1;
static s32 sRandSeed2;
static FishingProp sPondProps[POND_PROP_COUNT];
static FishingGroupFish sGroupFishes[GROUP_FISH_COUNT];
static f32 sFishGroupAngle1;
static f32 sFishGroupAngle2;
static f32 sFishGroupAngle3;
static FishingEffect sFishingEffects[FISHING_EFFECT_COUNT];
static Vec3f sStreamSoundProjectedPos;
static s16 sFishOnHandParams;

void Fishing_SetColliderElement(s32 index, ColliderJntSph* collider, Vec3f* pos, f32 scale) {
    collider->elements[index].dim.worldSphere.center.x = pos->x;
    collider->elements[index].dim.worldSphere.center.y = pos->y;
    collider->elements[index].dim.worldSphere.center.z = pos->z;
    collider->elements[index].dim.worldSphere.radius =
        collider->elements[index].dim.modelSphere.radius * collider->elements[index].dim.scale * scale * 1.6f;
}

void Fishing_SeedRand(s32 seed0, s32 seed1, s32 seed2) {
    sRandSeed0 = seed0;
    sRandSeed1 = seed1;
    sRandSeed2 = seed2;
}

f32 Fishing_RandZeroOne(void) {
    f32 rand;

    // Wichmann-Hill algorithm
    sRandSeed0 = (sRandSeed0 * 171) % 30269;
    sRandSeed1 = (sRandSeed1 * 172) % 30307;
    sRandSeed2 = (sRandSeed2 * 170) % 30323;

    rand = (sRandSeed0 / 30269.0f) + (sRandSeed1 / 30307.0f) + (sRandSeed2 / 30323.0f);
    while (rand >= 1.0f) {
        rand -= 1.0f;
    }

    return fabsf(rand);
}

s16 Fishing_SmoothStepToS(s16* pValue, s16 target, s16 scale, s16 step) {
    s16 stepSize;
    s16 diff;

    diff = target - *pValue;
    stepSize = diff / scale;

    if (stepSize > step) {
        stepSize = step;
    }

    if (stepSize < -step) {
        stepSize = -step;
    }

    *pValue += stepSize;

    return stepSize;
}

void Fishing_SpawnRipple(Vec3f* projectedPos, FishingEffect* effect, Vec3f* pos, f32 scale, f32 rotX, s16 alpha,
                         s16 countLimit) {
    s16 i;

    if ((projectedPos != NULL) && ((projectedPos->z > 500.0f) || (projectedPos->z < 0.0f))) {
        return;
    }

    for (i = 0; i < countLimit; i++) {
        if (effect->type == FS_EFF_NONE) {
            effect->type = FS_EFF_RIPPLE;
            effect->pos = *pos;
            effect->vel = sZeroVec;
            effect->accel = sZeroVec;
            effect->scale = scale * 0.0025f;
            effect->rot.x = rotX * 0.0025f;

            if (scale > 300.0f) {
                effect->alpha = 0;
                effect->alphaMax = alpha;
                effect->state = 0;
                effect->rot.y = (effect->rot.x - effect->scale) * 0.05f;
            } else {
                effect->alpha = alpha;
                effect->state = 1;
                effect->rot.y = (effect->rot.x - effect->scale) * 0.1f;
            }

            effect->epoch++;
            break;
        }

        effect++;
    }
}

void Fishing_SpawnDustSplash(Vec3f* projectedPos, FishingEffect* effect, Vec3f* pos, Vec3f* vel, f32 scale) {
    s16 i;
    Vec3f accel = { 0.0f, -1.0f, 0.0f };

    if ((projectedPos != NULL) && ((projectedPos->z > 500.0f) || (projectedPos->z < 0.0f))) {
        return;
    }

    for (i = 0; i < 100; i++) {
        if ((effect->type == FS_EFF_NONE) || (effect->type == FS_EFF_RAIN_DROP) ||
            (effect->type == FS_EFF_RAIN_RIPPLE) || (effect->type == FS_EFF_RAIN_SPLASH)) {
            effect->type = FS_EFF_DUST_SPLASH;
            effect->pos = *pos;
            effect->vel = *vel;
            effect->accel = accel;
            effect->alpha = 100 + (s16)Rand_ZeroFloat(100.0f);
            effect->scale = scale;
            effect->epoch++;
            break;
        }

        effect++;
    }
}

void Fishing_SpawnWaterDust(Vec3f* projectedPos, FishingEffect* effect, Vec3f* pos, f32 scale) {
    s16 i;
    Vec3f accel = { 0.0f, 0.05f, 0.0f };

    if ((projectedPos != NULL) && ((projectedPos->z > 500.0f) || (projectedPos->z < 0.0f))) {
        return;
    }

    for (i = 0; i < 90; i++) {
        if (effect->type == FS_EFF_NONE) {
            effect->type = FS_EFF_WATER_DUST;
            effect->pos = *pos;
            effect->vel = sZeroVec;
            effect->accel = accel;
            effect->alpha = 255;
            effect->timer = (s16)Rand_ZeroFloat(100.0f);
            effect->scale = scale;
            effect->rot.x = 2.0f * scale;
            effect->epoch++;
            break;
        }

        effect++;
    }
}

void Fishing_SpawnBubble(Vec3f* projectedPos, FishingEffect* effect, Vec3f* pos, f32 scale, u8 state) {
    s16 i;
    Vec3f vel = { 0.0f, 1.0f, 0.0f };

    if ((projectedPos != NULL) && ((projectedPos->z > 500.0f) || (projectedPos->z < 0.0f))) {
        return;
    }

    for (i = 0; i < 90; i++) {
        if (effect->type == FS_EFF_NONE) {
            effect->type = FS_EFF_BUBBLE;
            effect->pos = *pos;
            effect->vel = vel;
            effect->accel = sZeroVec;
            effect->timer = (s16)Rand_ZeroFloat(100.0f);
            effect->scale = scale;
            effect->state = state;
            effect->epoch++;
            break;
        }

        effect++;
    }
}

void Fishing_SpawnRainDrop(FishingEffect* effect, Vec3f* pos, Vec3f* rot) {
    s16 i;
    Vec3f velSrc;

    velSrc.x = 0.0f;
    velSrc.y = 0.0f;
    velSrc.z = 300.0f;

    effect += 30;

    for (i = 30; i < FISHING_EFFECT_COUNT; i++) {
        if (effect->type == FS_EFF_NONE) {
            effect->type = FS_EFF_RAIN_DROP;
            effect->pos = *pos;
            effect->accel = sZeroVec;
            effect->rot.x = rot->x;
            effect->rot.y = rot->y;
            effect->rot.z = rot->z;
            Matrix_RotateY(rot->y, MTXMODE_NEW);
            Matrix_RotateX(rot->x, MTXMODE_APPLY);
            Matrix_MultVec3f(&velSrc, &effect->vel);
            effect->epoch++;
            break;
        }

        effect++;
    }
}

static FishingPropInit sPondPropInits[POND_PROP_COUNT + 1] = {
    { FS_PROP_ROCK, { 529, -53, -498 } },
    { FS_PROP_ROCK, { 461, -66, -480 } },
    { FS_PROP_ROCK, { 398, -73, -474 } },
    { FS_PROP_ROCK, { -226, -52, -691 } },
    { FS_PROP_ROCK, { -300, -41, -710 } },
    { FS_PROP_ROCK, { -333, -50, -643 } },
    { FS_PROP_ROCK, { -387, -46, -632 } },
    { FS_PROP_ROCK, { -484, -43, -596 } },
    { FS_PROP_ROCK, { -409, -57, -560 } },
    { FS_PROP_WOOD_POST, { 444, -87, -322 } },
    { FS_PROP_WOOD_POST, { 447, -91, -274 } },
    { FS_PROP_WOOD_POST, { 395, -109, -189 } },
    { FS_PROP_REED, { 617, -29, 646 } },
    { FS_PROP_REED, { 698, -26, 584 } },
    { FS_PROP_REED, { 711, -29, 501 } },
    { FS_PROP_REED, { 757, -28, 457 } },
    { FS_PROP_REED, { 812, -29, 341 } },
    { FS_PROP_REED, { 856, -30, 235 } },
    { FS_PROP_REED, { 847, -31, 83 } },
    { FS_PROP_REED, { 900, -26, 119 } },
    { FS_PROP_LILY_PAD, { 861, -22, 137 } },
    { FS_PROP_LILY_PAD, { 836, -22, 150 } },
    { FS_PROP_LILY_PAD, { 829, -22, 200 } },
    { FS_PROP_LILY_PAD, { 788, -22, 232 } },
    { FS_PROP_LILY_PAD, { 803, -22, 319 } },
    { FS_PROP_LILY_PAD, { 756, -22, 348 } },
    { FS_PROP_LILY_PAD, { 731, -22, 377 } },
    { FS_PROP_LILY_PAD, { 700, -22, 392 } },
    { FS_PROP_LILY_PAD, { 706, -22, 351 } },
    { FS_PROP_LILY_PAD, { 677, -22, 286 } },
    { FS_PROP_LILY_PAD, { 691, -22, 250 } },
    { FS_PROP_LILY_PAD, { 744, -22, 290 } },
    { FS_PROP_LILY_PAD, { 766, -22, 201 } },
    { FS_PROP_LILY_PAD, { 781, -22, 128 } },
    { FS_PROP_LILY_PAD, { 817, -22, 46 } },
    { FS_PROP_LILY_PAD, { 857, -22, -50 } },
    { FS_PROP_LILY_PAD, { 724, -22, 110 } },
    { FS_PROP_LILY_PAD, { 723, -22, 145 } },
    { FS_PROP_LILY_PAD, { 728, -22, 202 } },
    { FS_PROP_LILY_PAD, { 721, -22, 237 } },
    { FS_PROP_LILY_PAD, { 698, -22, 312 } },
    { FS_PROP_LILY_PAD, { 660, -22, 349 } },
    { FS_PROP_LILY_PAD, { 662, -22, 388 } },
    { FS_PROP_LILY_PAD, { 667, -22, 432 } },
    { FS_PROP_LILY_PAD, { 732, -22, 429 } },
    { FS_PROP_LILY_PAD, { 606, -22, 366 } },
    { FS_PROP_LILY_PAD, { 604, -22, 286 } },
    { FS_PROP_LILY_PAD, { 620, -22, 217 } },
    { FS_PROP_LILY_PAD, { 663, -22, 159 } },
    { FS_PROP_LILY_PAD, { 682, -22, 73 } },
    { FS_PROP_LILY_PAD, { 777, -22, 83 } },
    { FS_PROP_LILY_PAD, { 766, -22, 158 } },
    { FS_PROP_REED, { 1073, 0, -876 } },
    { FS_PROP_REED, { 970, 0, -853 } },
    { FS_PROP_REED, { 896, 0, -886 } },
    { FS_PROP_REED, { 646, -27, -651 } },
    { FS_PROP_REED, { 597, -29, -657 } },
    { FS_PROP_REED, { 547, -32, -651 } },
    { FS_PROP_REED, { 690, -29, -546 } },
    { FS_PROP_REED, { 720, -29, -490 } },
    { FS_PROP_REED, { -756, -30, -409 } },
    { FS_PROP_REED, { -688, -34, -458 } },
    { FS_PROP_REED, { -613, -34, -581 } },
    { FS_PROP_LILY_PAD, { -593, -22, -479 } },
    { FS_PROP_LILY_PAD, { -602, -22, -421 } },
    { FS_PROP_LILY_PAD, { -664, -22, -371 } },
    { FS_PROP_LILY_PAD, { -708, -22, -316 } },
    { FS_PROP_LILY_PAD, { -718, -22, -237 } },
    { FS_PROP_REED, { -807, -36, -183 } },
    { FS_PROP_REED, { -856, -29, -259 } },
    { FS_PROP_LILY_PAD, { -814, -22, -317 } },
    { FS_PROP_LILY_PAD, { -759, -22, -384 } },
    { FS_PROP_LILY_PAD, { -718, -22, -441 } },
    { FS_PROP_LILY_PAD, { -474, -22, -567 } },
    { FS_PROP_LILY_PAD, { -519, -22, -517 } },
    { FS_PROP_LILY_PAD, { -539, -22, -487 } },
    { FS_PROP_LILY_PAD, { -575, -22, -442 } },
    { FS_PROP_LILY_PAD, { -594, -22, -525 } },
    { FS_PROP_LILY_PAD, { -669, -22, -514 } },
    { FS_PROP_LILY_PAD, { -653, -22, -456 } },
    { FS_PROP_REED, { -663, -28, -606 } },
    { FS_PROP_REED, { -708, -26, -567 } },
    { FS_PROP_REED, { -739, -27, -506 } },
    { FS_PROP_REED, { -752, -28, -464 } },
    { FS_PROP_REED, { -709, -29, -513 } },
    { FS_PROP_LILY_PAD, { -544, -22, -436 } },
    { FS_PROP_LILY_PAD, { -559, -22, -397 } },
    { FS_PROP_LILY_PAD, { -616, -22, -353 } },
    { FS_PROP_LILY_PAD, { -712, -22, -368 } },
    { FS_PROP_LILY_PAD, { -678, -22, -403 } },
    { FS_PROP_LILY_PAD, { -664, -22, -273 } },
    { FS_PROP_LILY_PAD, { -630, -22, -276 } },
    { FS_PROP_LILY_PAD, { -579, -22, -311 } },
    { FS_PROP_LILY_PAD, { -588, -22, -351 } },
    { FS_PROP_LILY_PAD, { -555, -22, -534 } },
    { FS_PROP_LILY_PAD, { -547, -22, -567 } },
    { FS_PROP_LILY_PAD, { -592, -22, -571 } },
    { FS_PROP_LILY_PAD, { -541, -22, -610 } },
    { FS_PROP_LILY_PAD, { -476, -22, -629 } },
    { FS_PROP_LILY_PAD, { -439, -22, -598 } },
    { FS_PROP_LILY_PAD, { -412, -22, -550 } },
    { FS_PROP_LILY_PAD, { -411, -22, -606 } },
    { FS_PROP_LILY_PAD, { -370, -22, -634 } },
    { FS_PROP_LILY_PAD, { -352, -22, -662 } },
    { FS_PROP_LILY_PAD, { -413, -22, -641 } },
    { FS_PROP_LILY_PAD, { -488, -22, -666 } },
    { FS_PROP_LILY_PAD, { -578, -22, -656 } },
    { FS_PROP_LILY_PAD, { -560, -22, -640 } },
    { FS_PROP_LILY_PAD, { -531, -22, -654 } },
    { FS_PROP_LILY_PAD, { -451, -22, -669 } },
    { FS_PROP_LILY_PAD, { -439, -22, -699 } },
    { FS_PROP_LILY_PAD, { -482, -22, -719 } },
    { FS_PROP_LILY_PAD, { -524, -22, -720 } },
    { FS_PROP_LILY_PAD, { -569, -22, -714 } },
    { FS_PROP_REED, { -520, -27, -727 } },
    { FS_PROP_REED, { -572, -28, -686 } },
    { FS_PROP_REED, { -588, -32, -631 } },
    { FS_PROP_REED, { -622, -34, -571 } },
    { FS_PROP_REED, { -628, -36, -510 } },
    { FS_PROP_REED, { -655, -36, -466 } },
    { FS_PROP_REED, { -655, -41, -393 } },
    { FS_PROP_REED, { -661, -47, -328 } },
    { FS_PROP_REED, { -723, -40, -287 } },
    { FS_PROP_REED, { -756, -33, -349 } },
    { FS_PROP_REED, { -755, -43, -210 } },
    { FS_PROP_LILY_PAD, { -770, -22, -281 } },
    { FS_PROP_LILY_PAD, { -750, -22, -313 } },
    { FS_PROP_LILY_PAD, { -736, -22, -341 } },
    { FS_PROP_LILY_PAD, { -620, -22, -418 } },
    { FS_PROP_LILY_PAD, { -601, -22, -371 } },
    { FS_PROP_LILY_PAD, { -635, -22, -383 } },
    { FS_PROP_LILY_PAD, { -627, -22, -311 } },
    { FS_PROP_LILY_PAD, { -665, -22, -327 } },
    { FS_PROP_LILY_PAD, { -524, -22, -537 } },
    { FS_PROP_LILY_PAD, { -514, -22, -579 } },
    { FS_PROP_LILY_PAD, { -512, -22, -623 } },
    { FS_PROP_LILY_PAD, { -576, -22, -582 } },
    { FS_PROP_LILY_PAD, { -600, -22, -608 } },
    { FS_PROP_LILY_PAD, { -657, -22, -531 } },
    { FS_PROP_LILY_PAD, { -641, -22, -547 } },
    { FS_PROP_INIT_STOP, { 0 } },
};

void Fishing_InitPondProps(Fishing* this, PlayState* play) {
    FishingProp* prop = &sPondProps[0];
    Vec3f colliderPos;
    s16 i;

    Fishing_SeedRand(1, 29100, 9786);

    for (i = 0; i < POND_PROP_COUNT; i++) {
        if (sPondPropInits[i].type == FS_PROP_INIT_STOP) {
            break;
        }

        prop->epoch++;
        prop->type = sPondPropInits[i].type;
        prop->pos.x = sPondPropInits[i].pos.x;
        prop->pos.y = sPondPropInits[i].pos.y;
        prop->pos.z = sPondPropInits[i].pos.z;
        prop->rotX = 0.0f;
        prop->reedAngle = 0.0f;

        prop->timer = Rand_ZeroFloat(100.0f);
        prop->drawDistance = 800.0f;

        if (prop->type == FS_PROP_REED) {
            prop->scale = (Fishing_RandZeroOne() * 0.25f) + 0.75f;
            prop->reedAngle = Rand_ZeroFloat(2 * M_PI);
            if (sLinkAge == LINK_AGE_CHILD) {
                prop->scale *= 0.6f;
            }
            prop->drawDistance = 1200.0f;
        } else if (prop->type == FS_PROP_WOOD_POST) {
            prop->scale = 0.08f;
            prop->drawDistance = 1200.0f;
            colliderPos = prop->pos;
            colliderPos.y += 50.0f;
            Fishing_SetColliderElement(i, &sFishingMain->collider, &colliderPos, prop->scale * 3.5f);
        } else if (prop->type == FS_PROP_LILY_PAD) {
            prop->scale = (Fishing_RandZeroOne() * 0.3f) + 0.5f;
            prop->rotY = Rand_ZeroFloat(2 * M_PI);
            if (sLinkAge == LINK_AGE_CHILD) {
                if ((i % 4) != 0) {
                    prop->scale *= 0.6f;
                } else {
                    prop->type = FS_PROP_NONE;
                }
            }
        } else {
            prop->scale = (Fishing_RandZeroOne() * 0.1f) + 0.3f;
            prop->rotY = Rand_ZeroFloat(2 * M_PI);
            prop->drawDistance = 1000.0f;
            Fishing_SetColliderElement(i, &sFishingMain->collider, &prop->pos, prop->scale);
        }

        prop++;
    }
}

static FishingFishInit sFishInits[] = {
    { 0, { 666, -45, 354 }, 38, 0.1f },    { 0, { 681, -45, 240 }, 36, 0.1f },   { 0, { 670, -45, 90 }, 41, 0.05f },
    { 0, { 615, -45, -450 }, 35, 0.2f },   { 0, { 500, -45, -420 }, 39, 0.1f },  { 0, { 420, -45, -550 }, 44, 0.05f },
    { 0, { -264, -45, -640 }, 40, 0.1f },  { 0, { -470, -45, -540 }, 34, 0.2f }, { 0, { -557, -45, -430 }, 54, 0.01f },
    { 0, { -260, -60, -330 }, 47, 0.05f }, { 0, { -500, -60, 330 }, 42, 0.06f }, { 0, { 428, -40, -283 }, 33, 0.2f },
    { 0, { 409, -70, -230 }, 57, 0.0f },   { 0, { 450, -67, -300 }, 63, 0.0f },  { 0, { -136, -65, -196 }, 71, 0.0f },
    { 1, { -561, -35, -547 }, 45, 0.0f },  { 1, { 667, -35, 317 }, 43, 0.0f },
};

static InitChainEntry sInitChain[] = {
    ICHAIN_U8(targetMode, 5, ICHAIN_CONTINUE),
    ICHAIN_F32(targetArrowOffset, 0, ICHAIN_STOP),
};

void Fishing_Init(Actor* thisx, PlayState* play2) {
    PlayState* play = play2;
    Fishing* this = (Fishing*)thisx;
    u16 fishCount;
    s16 i;

    Actor_ProcessInitChain(thisx, sInitChain);
    ActorShape_Init(&thisx->shape, 0.0f, NULL, 0.0f);

    if (KREG(5) != 0) {
        sLinkAge = LINK_AGE_CHILD;
    } else {
        sLinkAge = gSaveContext.linkAge;
    }

    if (thisx->params == EN_FISH_PORTABLE) {
        Fishing_RF3InitPortable(this, play); // Reel Fishing III mod (step 15c)
        return;
    }
    if (thisx->params >= EN_FISH_REGION) {
        Fishing_RF3InitRegionFish(this, play); // Reel Fishing III mod (step 16d)
        return;
    }

    if (thisx->params < EN_FISH_PARAM) {
        sReelLock = 0;
        RF3Fight_End();      // Reel Fishing III mod: no fight carries over into a new pond visit
        RF3Approach_Reset(); // ... and no lure meter, chaser or wariness either
        sFishingMain = this;
        Collider_InitJntSph(play, &sFishingMain->collider);
        Collider_SetJntSph(play, &sFishingMain->collider, thisx, &sJntSphInit, sFishingMain->colliderElements);

        thisx->params = EN_FISH_OWNER;

        SkelAnime_InitFlex(play, &this->skelAnime, &gFishingOwnerSkel, &gFishingOwnerAnim, NULL, NULL, 0);
        Animation_MorphToLoop(&this->skelAnime, &gFishingOwnerAnim, 0.0f);

        thisx->update = Fishing_UpdateOwner;
        thisx->draw = Fishing_DrawOwner;

        thisx->shape.rot.y = -0x6000;
        thisx->world.pos.x = 160.0f;
        thisx->world.pos.y = -2.0f;
        thisx->world.pos.z = 1208.0f;

        Actor_SetScale(thisx, 0.011f);

        thisx->focus.pos = thisx->world.pos;
        thisx->focus.pos.y += 75.0f;
        thisx->flags |= ACTOR_FLAG_ATTENTION_ENABLED | ACTOR_FLAG_FRIENDLY;

        if (sLinkAge != LINK_AGE_CHILD) {
            if (HIGH_SCORE(HS_FISHING) & HS_FISH_STOLE_HAT) {
                sOwnerHair = FS_OWNER_BALD;
            } else {
                sOwnerHair = FS_OWNER_CAPPED;
            }
        } else {
            sOwnerHair = FS_OWNER_HAIR;
        }

        sOwnerTheftTimer = 20;
        play->specialEffects = sFishingEffects;
        gTimeSpeed = 1;
        sFishingPlayingState = 0;
        sFishingMusicDelay = 10;

        Audio_QueueSeqCmd(0x1 << 28 | SEQ_PLAYER_BGM_MAIN << 24 | 0x0100FF);

        if (sLinkAge == LINK_AGE_CHILD) {
            if ((HIGH_SCORE(HS_FISHING) & HS_FISH_LENGTH_CHILD) != 0) {
                sFishingRecordLength = HIGH_SCORE(HS_FISHING) & HS_FISH_LENGTH_CHILD;
            } else if (GameInteractor_Should(VB_FISHING_USE_DEFAULT_RECORD_LENGTH, true, &sFishingRecordLength)) {
                sFishingRecordLength = 40.0f; // 6 lbs
            }
        } else {
            if ((HIGH_SCORE(HS_FISHING) & HS_FISH_LENGTH_ADULT) != 0) {
                sFishingRecordLength = (HIGH_SCORE(HS_FISHING) & HS_FISH_LENGTH_ADULT) >> 0x18;
            } else if (GameInteractor_Should(VB_FISHING_USE_DEFAULT_RECORD_LENGTH, true, &sFishingRecordLength)) {
                sFishingRecordLength = 45.0f; // 7 lbs
            }
        }

        sFishGameNumber = (HIGH_SCORE(HS_FISHING) & (HS_FISH_PLAYED * 255)) >> 0x10;
        if ((sFishGameNumber & 7) == 7) {
            play->roomCtx.unk_74[0] = 90;
            sFishingFoggy = 1;
        } else {
            play->roomCtx.unk_74[0] = 40;
            sFishingFoggy = 0;
        }

        if (((sFishGameNumber & 7) == 6) || (KREG(3) != 0)) {
            sStormChanceTimer = 100;
            if (KREG(3) != 0) {
                KREG(3) = 0;
                HIGH_SCORE(HS_FISHING) &= ~(HS_FISH_PLAYED * 255);
                HIGH_SCORE(HS_FISHING) |= (HS_FISH_PLAYED * 6);
            }
        } else {
            sStormChanceTimer = 0;
        }

        for (i = 0; i < FISHING_EFFECT_COUNT; i++) {
            sFishingEffects[i].type = FS_EFF_NONE;
        }

        for (i = 0; i < POND_PROP_COUNT; i++) {
            sPondProps[i].type = FS_PROP_NONE;
        }

        sFishGroupAngle1 = 0.7f;
        sFishGroupAngle2 = 2.3f;
        sFishGroupAngle3 = 4.6f;

        for (i = 0; i < GROUP_FISH_COUNT; i++) {
            FishingGroupFish* fish = &sGroupFishes[i];

            fish->epoch++;

            fish->type = FS_GROUP_FISH_NORMAL;

            if (i <= 20) {
                fish->homePos.x = fish->pos.x = sinf(sFishGroupAngle1) * 720.0f;
                fish->homePos.z = fish->pos.z = cosf(sFishGroupAngle1) * 720.0f;
            } else if (i <= 40) {
                fish->homePos.x = fish->pos.x = sinf(sFishGroupAngle2) * 720.0f;
                fish->homePos.z = fish->pos.z = cosf(sFishGroupAngle2) * 720.0f;
            } else {
                fish->homePos.x = fish->pos.x = sinf(sFishGroupAngle3) * 720.0f;
                fish->homePos.z = fish->pos.z = cosf(sFishGroupAngle3) * 720.0f;
            }

            fish->homePos.y = fish->pos.y = -35.0f;

            fish->timer = Rand_ZeroFloat(100.0f);

            fish->unk_3C = 0;
            fish->unk_3E = 0;
            fish->unk_40 = 0;

            if (sLinkAge != LINK_AGE_CHILD) {
                if (((i >= 15) && (i < 20)) || ((i >= 35) && (i < 40)) || ((i >= 55) && (i < 60))) {
                    fish->type = FS_GROUP_FISH_NONE;
                }
            }
        }

        Fishing_InitPondProps(this, play);
        Actor_SpawnAsChild(&play->actorCtx, thisx, play, ACTOR_EN_KANBAN, 53.0f, -17.0f, 982.0f, 0, 0, 0,
                           ENKANBAN_FISHING);
        Actor_Spawn(&play->actorCtx, play, ACTOR_FISHING, 0.0f, 0.0f, 0.0f, 0, 0, 0, 200);

        // Loach(es) will spawn every fourth game
        if (GameInteractor_Should(VB_FISHING_SPAWN_LOACHES, (KREG(1) == 1) || ((sFishGameNumber & 3) == 3))) {
            // Fishes 16 and 17 are loaches. Only 16 is spawned as adult; child also spawns 17.
            if (sLinkAge != LINK_AGE_CHILD) {
                fishCount = 16;
            } else {
                fishCount = 17;
            }
        } else {
            fishCount = 15;
        }

        for (i = 0; i < fishCount; i++) {
            Actor_Spawn(&play->actorCtx, play, ACTOR_FISHING, sFishInits[i].pos.x, sFishInits[i].pos.y,
                        sFishInits[i].pos.z, 0, Rand_ZeroFloat(0x10000), 0, 100 + i);
        }
        if (RF3_IsEnabled()) {
            RF3Species_RollAll(fishCount); // Reel Fishing III mod (steps 14b/23b): each pond fish's species
        }
    } else {
        if (!GameInteractor_Should(VB_FISHING_FISH_IS_LOACH, thisx->params >= (EN_FISH_PARAM + 15)) ||
            (thisx->params == EN_FISH_AQUARIUM)) {
            SkelAnime_InitFlex(play, &this->skelAnime, &gFishingFishSkel, &gFishingFishAnim, NULL, NULL, 0);
            Animation_MorphToLoop(&this->skelAnime, &gFishingFishAnim, 0.0f);
        } else {
            SkelAnime_InitFlex(play, &this->skelAnime, &gFishingLoachSkel, &gFishingLoachAnim, NULL, NULL, 0);
            Animation_MorphToLoop(&this->skelAnime, &gFishingLoachAnim, 0.0f);
        }

        SkelAnime_Update(&this->skelAnime);

        if (thisx->params == EN_FISH_AQUARIUM) {
            this->fishState = 100;
            Actor_ChangeCategory(play, &play->actorCtx, thisx, ACTORCAT_PROP);
            thisx->targetMode = 0;
            thisx->flags |= ACTOR_FLAG_ATTENTION_ENABLED | ACTOR_FLAG_FRIENDLY;
            this->lightNode = LightContext_InsertLight(play, &play->lightCtx, &this->lightInfo);
        } else {
            this->fishState = 10;
            this->fishStateNext = 10;

            this->isLoach =
                GameInteractor_Should(VB_FISHING_FISH_IS_LOACH, sFishInits[thisx->params - EN_FISH_PARAM].isLoach);
            this->perception = sFishInits[thisx->params - EN_FISH_PARAM].perception;
            this->fishLength = sFishInits[thisx->params - EN_FISH_PARAM].baseLength;

            this->fishLength += Rand_ZeroFloat(4.99999f);

            // small chance to make big fish even bigger.
            if ((this->fishLength >= 65.0f) && (Rand_ZeroOne() < 0.05f)) {
                this->fishLength += Rand_ZeroFloat(7.99999f);
            }

            if (KREG(6) != 0) {
                this->fishLength = KREG(6) + 80.0f;
            }

            // "Come back when you get older! The fish will be bigger, too!"
            if (sLinkAge == LINK_AGE_CHILD) {
                this->fishLength *= 0.73f;
            }
        }
    }
}

void Fishing_Destroy(Actor* thisx, PlayState* play2) {
    PlayState* play = play2;
    Fishing* this = (Fishing*)thisx;

    if (thisx->params == EN_FISH_PORTABLE) {
        Fishing_RF3DestroyPortable(play); // Reel Fishing III mod (step 15c): no skeleton to free
        return;
    }

    SkelAnime_Free(&this->skelAnime, play);

    if (thisx->params == EN_FISH_AQUARIUM) {
        LightContext_RemoveLight(play, &play->lightCtx, this->lightNode);
    } else if (thisx->params == EN_FISH_OWNER) {
        Collider_DestroyJntSph(play, &this->collider);
    }
}

void Fishing_UpdateEffects(FishingEffect* effect, PlayState* play) {
    f32 rippleY;
    s16 i;

    for (i = 0; i < FISHING_EFFECT_COUNT; i++) {
        if (effect->type) {
            effect->timer++;
            effect->pos.x += effect->vel.x;
            effect->pos.y += effect->vel.y;
            effect->pos.z += effect->vel.z;
            effect->vel.y += effect->accel.y;

            if (effect->type == FS_EFF_RIPPLE) {
                Math_ApproachF(&effect->scale, effect->rot.x, 0.2f, effect->rot.y);

                if (effect->state == 0) {
                    effect->alpha += 20;

                    if (effect->alpha >= effect->alphaMax) {
                        effect->alpha = effect->alphaMax;
                        effect->state++;
                    }
                } else {
                    effect->alpha -= 8;

                    if (effect->alpha <= 0) {
                        effect->type = FS_EFF_NONE;
                    }
                }
            } else if (effect->type == FS_EFF_WATER_DUST) {
                Math_ApproachF(&effect->scale, effect->rot.x, 0.1f, 0.1f);
                effect->alpha -= 10;

                if (effect->pos.y > (WATER_SURFACE_Y(play) - 5.0f)) {
                    effect->accel.y = 0.0f;
                    effect->vel.y = 0.0f;
                    effect->alpha -= 5;
                }

                if (effect->alpha <= 0) {
                    effect->type = FS_EFF_NONE;
                }
            } else if (effect->type == FS_EFF_BUBBLE) {
                if (effect->state == 0) {
                    rippleY = WATER_SURFACE_Y(play);
                } else {
                    rippleY = 69.0f;
                }

                if (effect->pos.y >= rippleY) {
                    effect->type = FS_EFF_NONE;

                    if (Rand_ZeroOne() < 0.3f) {
                        Vec3f pos = effect->pos;
                        pos.y = rippleY;
                        Fishing_SpawnRipple(NULL, sFishingEffects, &pos, 20.0f, 60.0f, 150, 90);
                    }
                }
            } else if (effect->type == FS_EFF_DUST_SPLASH) {
                if (effect->vel.y < -20.0f) {
                    effect->vel.y = -20.0f;
                    effect->accel.y = 0.0f;
                }

                if (effect->pos.y <= WATER_SURFACE_Y(play)) {
                    effect->type = FS_EFF_NONE;
                    if (Rand_ZeroOne() < 0.5f) {
                        Vec3f pos = effect->pos;
                        pos.y = WATER_SURFACE_Y(play);
                        Fishing_SpawnRipple(NULL, sFishingEffects, &pos, 40.0f, 110.0f, 150, 90);
                    }
                }
            } else if (effect->type == FS_EFF_RAIN_DROP) {
                if (effect->pos.y < WATER_SURFACE_Y(play)) {
                    f32 sqDistXZ = SQ(effect->pos.x) + SQ(effect->pos.z);

                    if (sqDistXZ > SQ(920.0f)) {
                        effect->pos.y = WATER_SURFACE_Y(play) + ((sqrtf(sqDistXZ) - 920.0f) * 0.11f);
                        effect->timer = KREG(17) + 2;
                        effect->type = FS_EFF_RAIN_SPLASH;
                        effect->scale = (KREG(18) + 30) * 0.001f;
                    } else {
                        effect->pos.y = WATER_SURFACE_Y(play) + 3.0f;
                        effect->timer = 0;
                        if (Rand_ZeroOne() < 0.75f) {
                            effect->type = FS_EFF_RAIN_RIPPLE;
                            effect->vel = sZeroVec;
                            effect->scale = (KREG(18) + 30) * 0.001f;
                        } else {
                            effect->type = FS_EFF_NONE;
                        }
                    }

                    effect->vel = sZeroVec;
                }
            } else if (effect->type >= FS_EFF_RAIN_RIPPLE) {
                effect->scale += (KREG(18) + 30) * 0.001f;

                if (effect->timer >= 6) {
                    effect->type = FS_EFF_NONE;
                }
            } else if (effect->type == FS_EFF_OWNER_HAT) {
                f32 sqDistXZ;
                f32 bottomY;

                effect->scale = 10 * .001f;

                Math_ApproachS(&sEffOwnersHatRot.y, 0, 20, 100);
                Math_ApproachS(&sEffOwnersHatRot.x, 0, 20, 100);
                Math_ApproachS(&sEffOwnersHatRot.z, -0x4000, 20, 100);

                sqDistXZ = SQ(effect->pos.x) + SQ(effect->pos.z);
                bottomY = WATER_SURFACE_Y(play) + ((sqrtf(sqDistXZ) - 920.0f) * 0.147f);

                if (effect->pos.y > (bottomY - 10.0f)) {
                    effect->pos.y -= 0.1f;
                }

                if ((effect->timer % 16) == 0) {
                    Vec3f pos = effect->pos;
                    pos.y = WATER_SURFACE_Y(play);
                    Fishing_SpawnRipple(NULL, sFishingEffects, &pos, 30.0f, 300.0f, 150, 90);
                }

                if (effect->state >= 0) {
                    effect->state++;
                }

                if (effect->state == 30) {
                    Message_StartTextbox(play, 0x40B3, NULL);
                }

                if ((effect->state >= 100) && (Message_GetState(&play->msgCtx) == TEXT_STATE_EVENT)) {
                    if (Message_ShouldAdvance(play) || (Message_GetState(&play->msgCtx) == TEXT_STATE_NONE)) {
                        Message_CloseTextbox(play);
                        Rupees_ChangeBy(-50);
                        effect->state = -1;
                    }
                }
            }
        }

        effect++;
    }
}

void Fishing_DrawEffects(FishingEffect* effect, PlayState* play) {
    u8 flag = 0;
    f32 rotY;
    s16 i;
    s32 pad;
    FishingEffect* firstEffect = effect;

    OPEN_DISPS(play->state.gfxCtx);

    Matrix_Push();

    gDPPipeSync(POLY_XLU_DISP++);

    for (i = 0; i < 100; i++) {
        if (effect->type == FS_EFF_RIPPLE) {
            FrameInterpolation_RecordOpenChild(effect, effect->epoch);
            if (flag == 0) {
                gSPDisplayList(POLY_XLU_DISP++, gFishingRippleMaterialDL);
                gDPSetEnvColor(POLY_XLU_DISP++, 155, 155, 155, 0);
                flag++;
            }

            gDPSetPrimColor(POLY_XLU_DISP++, 0, 0, 255, 255, 255, effect->alpha);

            Matrix_Translate(effect->pos.x, effect->pos.y, effect->pos.z, MTXMODE_NEW);
            Matrix_Scale(effect->scale, 1.0f, effect->scale, MTXMODE_APPLY);

            gSPMatrix(POLY_XLU_DISP++, MATRIX_NEWMTX(play->state.gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);

            gSPDisplayList(POLY_XLU_DISP++, gFishingRippleModelDL);
            FrameInterpolation_RecordCloseChild();
        }
        effect++;
    }

    effect = firstEffect;
    flag = 0;
    for (i = 0; i < 100; i++) {
        if (effect->type == FS_EFF_DUST_SPLASH) {
            FrameInterpolation_RecordOpenChild(effect, effect->epoch);
            if (flag == 0) {
                gSPDisplayList(POLY_XLU_DISP++, gFishingDustSplashMaterialDL);
                gDPSetEnvColor(POLY_XLU_DISP++, 200, 200, 200, 0);
                flag++;
            }

            gDPSetPrimColor(POLY_XLU_DISP++, 0, 0, 180, 180, 180, effect->alpha);

            Matrix_Translate(effect->pos.x, effect->pos.y, effect->pos.z, MTXMODE_NEW);
            Matrix_ReplaceRotation(&play->billboardMtxF);
            Matrix_Scale(effect->scale, effect->scale, 1.0f, MTXMODE_APPLY);

            gSPMatrix(POLY_XLU_DISP++, MATRIX_NEWMTX(play->state.gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);

            gSPDisplayList(POLY_XLU_DISP++, gFishingDustSplashModelDL);
            FrameInterpolation_RecordCloseChild();
        }
        effect++;
    }

    effect = firstEffect;
    flag = 0;
    for (i = 0; i < 100; i++) {
        if (effect->type == FS_EFF_WATER_DUST) {
            FrameInterpolation_RecordOpenChild(effect, effect->epoch);
            if (flag == 0) {
                gSPDisplayList(POLY_OPA_DISP++, gFishingWaterDustMaterialDL);
                gDPSetEnvColor(POLY_OPA_DISP++, 40, 90, 80, 128);
                flag++;
            }

            gDPSetPrimColor(POLY_OPA_DISP++, 0, 0, 40, 90, 80, effect->alpha);

            gSPSegment(POLY_OPA_DISP++, 0x08,
                       Gfx_TwoTexScrollEx(play->state.gfxCtx, 0, effect->timer + (i * 3), (effect->timer + (i * 3)) * 5,
                                          32, 64, 1, 0, 0, 32, 32, 1, 5, 0, 0));

            Matrix_Translate(effect->pos.x, effect->pos.y, effect->pos.z, MTXMODE_NEW);
            Matrix_ReplaceRotation(&play->billboardMtxF);
            Matrix_Scale(effect->scale, effect->scale, 1.0f, MTXMODE_APPLY);

            gSPMatrix(POLY_OPA_DISP++, MATRIX_NEWMTX(play->state.gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);

            gSPDisplayList(POLY_OPA_DISP++, gFishingWaterDustModelDL);
            FrameInterpolation_RecordCloseChild();
        }
        effect++;
    }

    effect = firstEffect;
    flag = 0;
    for (i = 0; i < 100; i++) {
        if (effect->type == FS_EFF_BUBBLE) {
            FrameInterpolation_RecordOpenChild(effect, effect->epoch);
            if (flag == 0) {
                gSPDisplayList(POLY_XLU_DISP++, gFishingBubbleMaterialDL);
                gDPSetEnvColor(POLY_XLU_DISP++, 150, 150, 150, 0);
                gDPSetPrimColor(POLY_XLU_DISP++, 0, 0, 255, 255, 255, 255);
                flag++;
            }

            Matrix_Translate(effect->pos.x, effect->pos.y, effect->pos.z, MTXMODE_NEW);
            Matrix_ReplaceRotation(&play->billboardMtxF);
            Matrix_Scale(effect->scale, effect->scale, 1.0f, MTXMODE_APPLY);

            gSPMatrix(POLY_XLU_DISP++, MATRIX_NEWMTX(play->state.gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);

            gSPDisplayList(POLY_XLU_DISP++, gFishingBubbleModelDL);
            FrameInterpolation_RecordCloseChild();
        }
        effect++;
    }

    effect = firstEffect + 30;
    flag = 0;
    for (i = 30; i < FISHING_EFFECT_COUNT; i++) {
        if (effect->type == FS_EFF_RAIN_DROP) {
            FrameInterpolation_RecordOpenChild(effect, effect->epoch);
            if (flag == 0) {
                POLY_XLU_DISP = Gfx_SetupDL(POLY_XLU_DISP, 0x14);
                gDPSetCombineMode(POLY_XLU_DISP++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);
                gDPSetPrimColor(POLY_XLU_DISP++, 0, 0, 150, 255, 255, 30);
                flag++;
            }

            Matrix_Translate(effect->pos.x, effect->pos.y, effect->pos.z, MTXMODE_NEW);
            Matrix_RotateY(effect->rot.y, MTXMODE_APPLY);
            Matrix_RotateX(effect->rot.x, MTXMODE_APPLY);
            Matrix_RotateZ(effect->rot.z, MTXMODE_APPLY);
            Matrix_Scale(0.002f, 1.0f, 0.1f, MTXMODE_APPLY);

            gSPMatrix(POLY_XLU_DISP++, MATRIX_NEWMTX(play->state.gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);

            gSPDisplayList(POLY_XLU_DISP++, gFishingRainDropModelDL);
            FrameInterpolation_RecordCloseChild();
        }
        effect++;
    }

    Gfx_SetupDL_25Xlu(play->state.gfxCtx);

    effect = firstEffect + 30;
    flag = 0;
    for (i = 30; i < FISHING_EFFECT_COUNT; i++) {
        if (effect->type == FS_EFF_RAIN_RIPPLE) {
            FrameInterpolation_RecordOpenChild(effect, effect->epoch);
            if (flag == 0) {
                gSPDisplayList(POLY_XLU_DISP++, gFishingRippleMaterialDL);
                gDPSetEnvColor(POLY_XLU_DISP++, 155, 155, 155, 0);
                gDPSetPrimColor(POLY_XLU_DISP++, 0, 0, 255, 255, 255, 130);
                flag++;
            }

            Matrix_Translate(effect->pos.x, effect->pos.y, effect->pos.z, MTXMODE_NEW);
            Matrix_Scale(effect->scale, 1.0f, effect->scale, MTXMODE_APPLY);

            gSPMatrix(POLY_XLU_DISP++, MATRIX_NEWMTX(play->state.gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);

            gSPDisplayList(POLY_XLU_DISP++, gFishingRippleModelDL);
            FrameInterpolation_RecordCloseChild();
        }
        effect++;
    }

    effect = firstEffect + 30;
    flag = 0;
    for (i = 30; i < FISHING_EFFECT_COUNT; i++) {
        if (effect->type == FS_EFF_RAIN_SPLASH) {
            FrameInterpolation_RecordOpenChild(effect, effect->epoch);
            if (flag == 0) {
                gSPDisplayList(POLY_XLU_DISP++, gFishingRainSplashMaterialDL);
                gDPSetPrimColor(POLY_XLU_DISP++, 0, 0, 255, 255, 255, KREG(19) + 80);
                flag++;
            }

            if (Rand_ZeroOne() < 0.5f) {
                rotY = 0.0f;
            } else {
                rotY = M_PI;
            }

            Matrix_Translate(effect->pos.x, effect->pos.y, effect->pos.z, MTXMODE_NEW);
            Matrix_ReplaceRotation(&play->billboardMtxF);
            Matrix_RotateY(rotY, MTXMODE_APPLY);
            Matrix_Scale(effect->scale, effect->scale, 1.0f, MTXMODE_APPLY);

            gSPMatrix(POLY_XLU_DISP++, MATRIX_NEWMTX(play->state.gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);

            gSPDisplayList(POLY_XLU_DISP++, gFishingRainSplashModelDL);
            FrameInterpolation_RecordCloseChild();
        }
        effect++;
    }

    effect = firstEffect;
    if (effect->type == FS_EFF_OWNER_HAT) {
        FrameInterpolation_RecordOpenChild(effect, effect->epoch);
        Matrix_Translate(effect->pos.x, effect->pos.y, effect->pos.z, MTXMODE_NEW);
        Matrix_RotateY((sEffOwnersHatRot.y * M_PI) / 32768, MTXMODE_APPLY);
        Matrix_RotateX((sEffOwnersHatRot.x * M_PI) / 32768, MTXMODE_APPLY);
        Matrix_RotateZ((sEffOwnersHatRot.z * M_PI) / 32768, MTXMODE_APPLY);
        Matrix_Scale(effect->scale, effect->scale, effect->scale, MTXMODE_APPLY);
        Matrix_Translate(-1250.0f, 0.0f, 0.0f, MTXMODE_APPLY);
        Matrix_RotateX(M_PI / 2, MTXMODE_APPLY);
        gSPMatrix(POLY_OPA_DISP++, MATRIX_NEWMTX(play->state.gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);

        gSPDisplayList(POLY_OPA_DISP++, gFishingOwnerHatDL);
        FrameInterpolation_RecordCloseChild();
    }

    Matrix_Pop();

    CLOSE_DISPS(play->state.gfxCtx);
}

void Fishing_DrawStreamSplash(PlayState* play) {
    s32 pad;

    OPEN_DISPS(play->state.gfxCtx);

    gSPSegment(POLY_XLU_DISP++, 0x09,
               Gfx_TwoTexScrollEx(play->state.gfxCtx, 0, play->gameplayFrames * 1, play->gameplayFrames * 8, 32, 64, 1,
                                  -(play->gameplayFrames * 2), 0, 16, 16, 1, 8, -2, 0));

    gDPSetPrimColor(POLY_XLU_DISP++, 0, 0, 195, 225, 235, 50);

    Matrix_Translate(670.0f, -24.0f, -600.0f, MTXMODE_NEW);
    Matrix_Scale(0.02f, 1.0f, 0.02f, MTXMODE_APPLY);

    gSPMatrix(POLY_XLU_DISP++, MATRIX_NEWMTX(play->state.gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    gSPDisplayList(POLY_XLU_DISP++, SEGMENTED_TO_VIRTUAL(gFishingStreamSplashDL));

    CLOSE_DISPS(play->state.gfxCtx);
}

s32 Fishing_IsAboveCounter(Vec3f* vec) {
    if (((vec->x >= 110.0f) && (vec->x <= 150.0f) && (vec->z <= 1400.0f) && (vec->z >= 1160.0f)) ||
        ((vec->x >= 110.0f) && (vec->x <= 210.0f) && (vec->z <= 1200.0f) && (vec->z >= 1160.0f))) {
        if (vec->y <= 42.0f) {
            return true;
        }
    }

    return false;
}

void Fishing_UpdateLine(PlayState* play, Vec3f* basePos, Vec3f* pos, Vec3f* rot, Vec3f* unk) {
    s16 i;
    s16 k;
    f32 dx;
    f32 dy;
    f32 dz;
    f32 rx;
    f32 ry;
    f32 dist;
    f32 spD8;
    s16 spooled;
    s32 pad;
    f32 temp_f20;
    Vec3f posSrc = { 0.0f, 0.0f, 0.0f };
    Vec3f posStep;
    f32 lineLength;
    Vec3f tempPos;
    Vec3f segPos;
    f32 sp94;
    f32 sp90;
    f32 sp8C;
    f32 sqDistXZ;
    f32 temp_f18;
    f32 phi_f12;
    f32 phi_f2;

    if (D_80B7A6A4 != 0) {
        tempPos = *basePos;
        segPos = pos[LINE_SEG_COUNT - 1];

        sp94 = segPos.x - tempPos.x;
        sp90 = segPos.y - tempPos.y;
        sp8C = segPos.z - tempPos.z;

        lineLength = sqrtf(SQ(sp94) + SQ(sp90) + SQ(sp8C)) * 0.97f;
        if (lineLength > 1000.0f) {
            lineLength = 1000.0f;
        }

        sRodLineSpooled = 200.0f - (lineLength * 200.0f * 0.001f);
    }

    spooled = sRodLineSpooled;
    posSrc.z = 5.0f;

    for (i = 0; i < LINE_SEG_COUNT; i++) {
        if (i <= spooled) {
            pos[i] = *basePos;
        } else if (D_80B7A6A4 != 0) {
            temp_f20 = (f32)(i - spooled) / (f32)(LINE_SEG_COUNT - spooled + 1);
            Math_ApproachF(&pos[i].x, (sp94 * temp_f20) + tempPos.x, 1.0f, 20.0f);
            Math_ApproachF(&pos[i].y, (sp90 * temp_f20) + tempPos.y, 1.0f, 20.0f);
            Math_ApproachF(&pos[i].z, (sp8C * temp_f20) + tempPos.z, 1.0f, 20.0f);
        }
    }

    for (i = spooled + 1, k = 0; i < LINE_SEG_COUNT; i++, k++) {
        temp_f18 = 2.0f * D_80B7E148;

        dx = (pos + i)->x - (pos + i - 1)->x;
        spD8 = (pos + i)->y;

        sqDistXZ = SQ((pos + i)->x) + SQ((pos + i)->z);

        if (Fishing_RF3OnLand(sqDistXZ)) { // step 15c: portable = the lure's own ground
            phi_f12 = Fishing_RF3LandY(play, sqDistXZ, 0.11f);
        } else {
            phi_f12 = WATER_SURFACE_Y(play);
        }

        if (sLureEquipped == FS_LURE_SINKING) {
            if (spD8 < phi_f12) {
                phi_f12 = ((sqrtf(sqDistXZ) - 920.0f) * 0.147f) + WATER_SURFACE_Y(play);
                if (spD8 > phi_f12) {
                    phi_f2 = (spD8 - phi_f12) * 0.05f;
                    if (phi_f2 > 0.29999998f) {
                        phi_f2 = 0.29999998f;
                    }
                    if (i >= 100) {
                        phi_f2 *= (i - 100) * 0.02f;
                        spD8 -= phi_f2;
                    }
                }
            } else {
                spD8 -= temp_f18;
            }
        } else if (i > LINE_SEG_COUNT - 10) {
            if (spD8 > phi_f12) {
                phi_f2 = (spD8 - phi_f12) * 0.2f;
                if (phi_f2 > temp_f18) {
                    phi_f2 = temp_f18;
                }
                spD8 -= phi_f2;
            }
        } else {
            if (spD8 > phi_f12) {
                spD8 -= temp_f18;
            }
        }

        if (Fishing_IsAboveCounter(&pos[i])) {
            spD8 = 42.0f;
        }

        dy = spD8 - (pos + i - 1)->y;
        dz = (pos + i)->z - (pos + i - 1)->z;

        ry = Math_Atan2F(dz, dx);
        dist = sqrtf(SQ(dx) + SQ(dz));
        rx = -Math_Atan2F(dist, dy);

        (rot + i - 1)->y = ry;
        (rot + i - 1)->x = rx;

        Matrix_RotateY(ry, MTXMODE_NEW);
        Matrix_RotateX(rx, MTXMODE_APPLY);
        Matrix_MultVec3f(&posSrc, &posStep);

        (pos + i)->x = (pos + i - 1)->x + posStep.x;
        (pos + i)->y = (pos + i - 1)->y + posStep.y;
        (pos + i)->z = (pos + i - 1)->z + posStep.z;
    }
}

void Fishing_UpdateLinePos(Vec3f* pos) {
    s16 i;
    f32 dx;
    f32 dy;
    f32 dz;
    f32 rx;
    f32 ry;
    f32 dist;
    Vec3f posSrc = { 0.0f, 0.0f, 0.0f };
    Vec3f posStep;
    s16 spooled = sRodLineSpooled;

    posSrc.z = 5.0f;

    for (i = LINE_SEG_COUNT - 2; i > spooled; i--) {
        dx = (pos + i)->x - (pos + i + 1)->x;
        dy = (pos + i)->y - (pos + i + 1)->y;
        dz = (pos + i)->z - (pos + i + 1)->z;

        ry = Math_Atan2F(dz, dx);
        dist = sqrtf(SQ(dx) + SQ(dz));
        rx = -Math_Atan2F(dist, dy);

        Matrix_RotateY(ry, MTXMODE_NEW);
        Matrix_RotateX(rx, MTXMODE_APPLY);
        Matrix_MultVec3f(&posSrc, &posStep);

        (pos + i)->x = (pos + i + 1)->x + posStep.x;
        (pos + i)->y = (pos + i + 1)->y + posStep.y;
        (pos + i)->z = (pos + i + 1)->z + posStep.z;
    }
}

void Fishing_DrawLureHook(PlayState* play, Vec3f* pos, Vec3f* refPos, u8 hookIndex) {
    f32 dx;
    f32 dy;
    f32 dz;
    f32 rx;
    f32 ry;
    f32 dist;
    f32 offsetY;
    Vec3f posSrc = { 0.0f, 0.0f, 1.0f };
    Vec3f posStep;
    Player* player = GET_PLAYER(play);

    OPEN_DISPS(play->state.gfxCtx);

    Matrix_Push();

    if ((sRodCastState == 3) && ((pos->y > WATER_SURFACE_Y(play)) || (sIsOwnersHatHooked && hookIndex))) {
        offsetY = 0.0f;
    } else if (pos->y < WATER_SURFACE_Y(play)) {
        offsetY = -1.0f;
    } else {
        offsetY = -3.0f;
    }

    dx = refPos->x - pos->x;
    dy = refPos->y - pos->y + offsetY;
    dz = refPos->z - pos->z;

    ry = Math_Atan2F(dz, dx);
    dist = sqrtf(SQ(dx) + SQ(dz));
    rx = -Math_Atan2F(dist, dy);

    Matrix_RotateY(ry, MTXMODE_NEW);
    Matrix_RotateX(rx, MTXMODE_APPLY);
    Matrix_MultVec3f(&posSrc, &posStep);

    refPos->x = pos->x + posStep.x;
    refPos->y = pos->y + posStep.y;
    refPos->z = pos->z + posStep.z;

    Matrix_Translate(pos->x, pos->y, pos->z, MTXMODE_NEW);

    if ((player->actor.speedXZ == 0.0f) && (sLureWigglePosY == 0.0f)) {
        Math_ApproachF(&sLureHookRotY[hookIndex], ry, 0.1f, 0.3f);
    } else {
        sLureHookRotY[hookIndex] = ry;
    }

    Matrix_RotateY(sLureHookRotY[hookIndex], MTXMODE_APPLY);
    Matrix_RotateX(rx, MTXMODE_APPLY);
    Matrix_Scale(0.0039999997f, 0.0039999997f, 0.005f, MTXMODE_APPLY);
    Matrix_RotateY(M_PI, MTXMODE_APPLY);

    // Reel Fishing III mod (step 25c-2): RF3's lure brings its own hook; step 32a: no lure, no hook
    if (!sRF3LureModelShown && !sRF3NoLure) {
        gSPMatrix(POLY_OPA_DISP++, MATRIX_NEWMTX(play->state.gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
        gSPDisplayList(POLY_OPA_DISP++, gFishingLureHookDL);
    }

    Matrix_RotateZ(M_PI / 2, MTXMODE_APPLY);

    if (!sRF3LureModelShown && !sRF3NoLure) {
        gSPMatrix(POLY_OPA_DISP++, MATRIX_NEWMTX(play->state.gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
        gSPDisplayList(POLY_OPA_DISP++, gFishingLureHookDL);
    }

    if ((hookIndex == 1) && (sIsOwnersHatHooked)) {
        Matrix_Scale(2.0f, 2.0f, 2.0f, MTXMODE_APPLY);
        Matrix_Translate(250.0f, 0.0f, -1400.0f, MTXMODE_APPLY);
        Matrix_Push();

        if (sIsOwnersHatSunk) {
            FishingEffect* effect = sFishingEffects;
            MtxF mf;

            Matrix_MultVec3f(&sZeroVec, &effect->pos);
            Matrix_Get(&mf);
            Matrix_MtxFToYXZRotS(&mf, &sEffOwnersHatRot, 0);

            sIsOwnersHatSunk = false;
            sIsOwnersHatHooked = false;

            effect->type = FS_EFF_OWNER_HAT;
            effect->state = 0;
            effect->vel = sZeroVec;
            effect->accel = sZeroVec;
        }

        Matrix_Pop();
        Matrix_Translate(-1250.0f, 0.0f, 0.0f, MTXMODE_APPLY);
        Matrix_RotateX(M_PI / 2, MTXMODE_APPLY);

        gSPMatrix(POLY_OPA_DISP++, MATRIX_NEWMTX(play->state.gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
        gSPDisplayList(POLY_OPA_DISP++, gFishingOwnerHatDL);
    }

    Matrix_Pop();

    CLOSE_DISPS(play->state.gfxCtx);
}

void Fishing_UpdateSinkingLure(PlayState* play) {
    s16 i;
    f32 dx;
    f32 dy;
    f32 dz;
    f32 rx;
    f32 ry;
    f32 dist;
    f32 offsetY;
    Vec3f posSrc = { 0.0f, 0.0f, 0.0f };
    Vec3f posStep;
    Vec3f sp94;
    Vec3f sp88;
    f32 offsetX;
    f32 offsetZ;
    Player* player = GET_PLAYER(play);

    posSrc.z = 0.85f;

    sSinkingLurePos[0] = sLurePos;

    if (sSinkingLureFound) {
        offsetY = -1.0f;
    } else if (sLurePos.y < WATER_SURFACE_Y(play)) {
        offsetY = 0.5f;
    } else {
        offsetY = -5.0f;
    }

    if (sRodCastState == 5) {
        Matrix_RotateY(player->actor.shape.rot.y * (M_PI / 32768), MTXMODE_NEW);
        sp94.x = 5.0f;
        sp94.y = 0.0f;
        sp94.z = 3.0f;
        Matrix_MultVec3f(&sp94, &sp88);
    }

    for (i = 1; i < SINKING_LURE_SEG_COUNT; i++) {
        Vec3f* pos = sSinkingLurePos;

        if ((i < 10) && (sRodCastState == 5)) {
            offsetX = (10 - i) * sp88.x * 0.1f;
            offsetZ = (10 - i) * sp88.z * 0.1f;
        } else {
            offsetX = offsetZ = 0.0f;
        }

        dx = (pos + i)->x - (pos + i - 1)->x + offsetX;
        dy = (pos + i)->y - (pos + i - 1)->y + offsetY;
        dz = (pos + i)->z - (pos + i - 1)->z + offsetZ;

        ry = Math_Atan2F(dz, dx);
        dist = sqrtf(SQ(dx) + SQ(dz));
        rx = -Math_Atan2F(dist, dy);

        Matrix_RotateY(ry, MTXMODE_NEW);
        Matrix_RotateX(rx, MTXMODE_APPLY);
        Matrix_MultVec3f(&posSrc, &posStep);

        (pos + i)->x = (pos + i - 1)->x + posStep.x;
        (pos + i)->y = (pos + i - 1)->y + posStep.y;
        (pos + i)->z = (pos + i - 1)->z + posStep.z;
    }
}

static f32 sSinkingLureSizes[] = {
    1.0f, 1.5f,  1.8f, 2.0f, 1.8f, 1.6f, 1.4f, 1.2f, 1.0f, 1.0f,
    0.9f, 0.85f, 0.8f, 0.7f, 0.8f, 1.0f, 1.2f, 1.1f, 1.0f, 0.8f,
};

void Fishing_DrawSinkingLure(PlayState* play) {
    s16 i;
    f32 scale;

    OPEN_DISPS(play->state.gfxCtx);

    Fishing_UpdateSinkingLure(play);

    if (sLurePos.y < WATER_SURFACE_Y(play)) {
        Gfx_SetupDL_25Opa(play->state.gfxCtx);

        gSPDisplayList(POLY_OPA_DISP++, gFishingSinkingLureSegmentMaterialDL);

        for (i = SINKING_LURE_SEG_COUNT - 1; i >= 0; i--) {
            if ((i + sSinkingLureSegmentIndex) < SINKING_LURE_SEG_COUNT) {
                FrameInterpolation_RecordOpenChild("Fishing Lures 1", i);
                Matrix_Translate(sSinkingLurePos[i].x, sSinkingLurePos[i].y, sSinkingLurePos[i].z, MTXMODE_NEW);
                scale = sSinkingLureSizes[i + sSinkingLureSegmentIndex] * 0.04f;
                Matrix_Scale(scale, scale, scale, MTXMODE_APPLY);
                Matrix_ReplaceRotation(&play->billboardMtxF);

                gSPMatrix(POLY_OPA_DISP++, MATRIX_NEWMTX(play->state.gfxCtx),
                          G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
                gSPDisplayList(POLY_OPA_DISP++, gFishingSinkingLureSegmentModelDL);
                FrameInterpolation_RecordCloseChild();
            }
        }
    } else {
        Gfx_SetupDL_25Xlu(play->state.gfxCtx);

        gSPDisplayList(POLY_XLU_DISP++, gFishingSinkingLureSegmentMaterialDL);

        for (i = SINKING_LURE_SEG_COUNT - 1; i >= 0; i--) {
            if ((i + sSinkingLureSegmentIndex) < SINKING_LURE_SEG_COUNT) {
                FrameInterpolation_RecordOpenChild("Fishing Lures 2", i);
                Matrix_Translate(sSinkingLurePos[i].x, sSinkingLurePos[i].y, sSinkingLurePos[i].z, MTXMODE_NEW);
                scale = sSinkingLureSizes[i + sSinkingLureSegmentIndex] * 0.04f;
                Matrix_Scale(scale, scale, scale, MTXMODE_APPLY);
                Matrix_ReplaceRotation(&play->billboardMtxF);

                gSPMatrix(POLY_XLU_DISP++, MATRIX_NEWMTX(play->state.gfxCtx),
                          G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
                gSPDisplayList(POLY_XLU_DISP++, gFishingSinkingLureSegmentModelDL);
                FrameInterpolation_RecordCloseChild();
            }
        }
    }

    CLOSE_DISPS(play->state.gfxCtx);
}

// Step 29g-2: one straight piece of line from a to b (the same model and scale as the hooked line).
static void Fishing_RF3LinePiece(PlayState* play, Vec3f* a, Vec3f* b) {
    f32 dx = b->x - a->x;
    f32 dy = b->y - a->y;
    f32 dz = b->z - a->z;
    f32 ry = Math_FAtan2F(dx, dz);
    f32 rx = -Math_FAtan2F(dy, sqrtf(SQ(dx) + SQ(dz)));
    f32 dist = sqrtf(SQ(dx) + SQ(dy) + SQ(dz)) * 0.001f;

    OPEN_DISPS(play->state.gfxCtx);
    Matrix_Translate(a->x, a->y, a->z, MTXMODE_NEW);
    Matrix_RotateY(ry, MTXMODE_APPLY);
    Matrix_RotateX(rx, MTXMODE_APPLY);
    Matrix_Scale(sFishingLineScale, 1.0f, dist, MTXMODE_APPLY);
    gSPMatrix(POLY_XLU_DISP++, MATRIX_NEWMTX(play->state.gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    gSPDisplayList(POLY_XLU_DISP++, gFishingLineModelDL);
    CLOSE_DISPS(play->state.gfxCtx);
}

void Fishing_DrawLureAndLine(PlayState* play, Vec3f* linePos, Vec3f* lineRot) {
    Vec3f posSrc;
    Vec3f posStep;
    Vec3f hookPos[2];
    s16 i;
    s16 spooled = sRodLineSpooled;
    s32 pad;
    Player* player = GET_PLAYER(play);

    OPEN_DISPS(play->state.gfxCtx);

    Gfx_SetupDL_25Opa(play->state.gfxCtx);
    Matrix_Push();

    if (sSinkingLureFound) {
        Vec3f posTemp = sLurePos;
        sLurePos = sSinkingLureBasePos;
        Fishing_DrawSinkingLure(play);
        sLurePos = posTemp;
    }

    if ((sRodCastState == 4) || (sRodCastState == 5)) {
        sLurePos = sFishingHookedFish->fishMouthPos;

        if ((sRodCastState == 5) && (sLureEquipped == FS_LURE_SINKING)) {
            Matrix_RotateY(player->actor.shape.rot.y * (M_PI / 32768), MTXMODE_NEW);
            posSrc.x = 2.0f;
            posSrc.y = 0.0f;
            posSrc.z = 0.0f;
            Matrix_MultVec3f(&posSrc, &posStep);
            sLurePos.x += posStep.x;
            sLurePos.z += posStep.z;
        }
    } else if (sRodCastState == 0) {
        sLurePos = sReelLinePos[LINE_SEG_COUNT - 1];
        sLureRot.x = sReelLineRot[LINE_SEG_COUNT - 2].x + M_PI;

        if ((player->actor.speedXZ == 0.0f) && (D_80B7E0B0 == 0)) {
            Math_ApproachF(&sLureRot.y, sReelLineRot[LINE_SEG_COUNT - 2].y, 0.1f, 0.2f);
        } else {
            sLureRot.y = sReelLineRot[LINE_SEG_COUNT - 2].y;
        }
    }

    sRF3LureModelShown = false;
    sRF3NoLure = RF3_IsEnabled() && RF3Tackle_LureMissing(); // step 32a
    if (sLureEquipped != FS_LURE_SINKING) {
        Matrix_Translate(sLurePos.x, sLurePos.y, sLurePos.z, MTXMODE_NEW);
        Matrix_RotateY(sLureRot.y + sLure1Rotate, MTXMODE_APPLY);
        Matrix_RotateX(sLureRot.x, MTXMODE_APPLY);
        // Reel Fishing III mod (steps 25c-2, 25d-2): RF3's own model of the chosen lure, fly or bait.
        // Step 27b/27c: the cast state picks RF3's lure motion; hooked, the region fish's slot lets the lure hang
        // from its mouth.
        // Step 32a: an empty line draws no RF3 lure (and, below, no OoT lure or hooks). Step 33b: an empty bait set's
        // RF3 draw is its bare hook; an empty lure/fly box draws nothing.
        sRF3LureModelShown = RF3LureModel_Draw(
            play, &sLurePos.x, sLureRot.y + sLure1Rotate, sLureRot.x, sLurePosZOffset, sRodCastState,
            (((sRodCastState == 4) || (sRodCastState == 5)) && (sFishingHookedFish != NULL) &&
             RF3_IS_REGION_FISH(sFishingHookedFish))
                ? (sFishingHookedFish->actor.params - EN_FISH_REGION)
                : -1,
            play->gameplayFrames);
        Matrix_Scale(0.0039999997f, 0.0039999997f, 0.0039999997f, MTXMODE_APPLY);
        Matrix_Translate(0.0f, 0.0f, sLurePosZOffset, MTXMODE_APPLY);
        Matrix_RotateZ(M_PI / 2, MTXMODE_APPLY);
        Matrix_RotateY(M_PI / 2, MTXMODE_APPLY);

        Gfx_SetupDL_25Opa(play->state.gfxCtx);

        if (!sRF3LureModelShown && !sRF3NoLure) {
            gSPMatrix(POLY_OPA_DISP++, MATRIX_NEWMTX(play->state.gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
            gSPDisplayList(POLY_OPA_DISP++, gFishingLureFloatDL);
        }

        posSrc.x = -850.0f;
        posSrc.y = 0.0f;
        posSrc.z = 0.0f;
        Matrix_MultVec3f(&posSrc, &sLureDrawPos);

        posSrc.x = 500.0f;
        posSrc.z = -300.0f;
        Matrix_MultVec3f(&posSrc, &hookPos[0]);
        Fishing_DrawLureHook(play, &hookPos[0], &sLureHookRefPos[0], 0);

        posSrc.x = 2100.0f;
        posSrc.z = -50.0f;
        Matrix_MultVec3f(&posSrc, &hookPos[1]);
        Fishing_DrawLureHook(play, &hookPos[1], &sLureHookRefPos[1], 1);

        // Reel Fishing III mod (step 25d-2): a bait set's float, on the surface above the bait or on the line.
        // Step 29g: the cast state lets the float go under at the bite.
        RF3LureModel_DrawFloat(play, &sLurePos.x, &sRodTipPos.x, WATER_SURFACE_Y(play), sLureRot.y + sLure1Rotate,
                               sRodCastState);
    }

    POLY_XLU_DISP = Gfx_SetupDL(POLY_XLU_DISP, 0x14);

    gDPSetCombineMode(POLY_XLU_DISP++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);
    gDPSetPrimColor(POLY_XLU_DISP++, 0, 0, 255, 255, 255, 55);

    // Step 29g-2: a bait set's float drawn this frame: the line runs through it (rod -> float -> bait).
    Vec3f rf3Float;
    s32 rf3FloatOn = RF3LureModel_FloatLinePoint(&rf3Float.x, play->gameplayFrames);

    // Step 27e-1: where the drawn line ends, for the lure-tie log (the branches below, in order)
    Vec3f lineDrawnEnd = linePos[LINE_SEG_COUNT - 1];
    if ((sLureEquipped == FS_LURE_STOCK) && (sRodCastState == 3) && (spooled <= LINE_SEG_COUNT - 3)) {
        lineDrawnEnd = sLureDrawPos;
    }

    if ((sRodCastState == 4) && ((sLineHooked != 0) || (sLureEquipped != 2))) {
        f32 rx;
        f32 ry;
        f32 dist;
        f32 dx;
        f32 dy;
        f32 dz;
        Vec3f lineEnd = sLurePos;

        // Step 27c fix: an RF3 lure hanging from the fish's mouth: the line is drawn to the lure's tie (drawing only;
        // sLurePos/fishMouthPos stay the snout for the game logic).
        if (sRF3LureModelShown) {
            RF3LureModel_LineTie(&lineEnd.x);
        }
        lineDrawnEnd = lineEnd;
        if (rf3FloatOn) {
            // Step 29g-2: hooked with a float: rod -> float -> the fish's end.
            Fishing_RF3LinePiece(play, &sRodTipPos, &rf3Float);
            Fishing_RF3LinePiece(play, &rf3Float, &lineEnd);
        } else {
            dx = lineEnd.x - sRodTipPos.x;
            dy = lineEnd.y - sRodTipPos.y;
            dz = lineEnd.z - sRodTipPos.z;

            ry = Math_FAtan2F(dx, dz);
            dist = sqrtf(SQ(dx) + SQ(dz));
            rx = -Math_FAtan2F(dy, dist);

            dist = sqrtf(SQ(dx) + SQ(dy) + SQ(dz)) * 0.001f;

            Matrix_Translate(sRodTipPos.x, sRodTipPos.y, sRodTipPos.z, MTXMODE_NEW);
            Matrix_RotateY(ry, MTXMODE_APPLY);
            Matrix_RotateX(rx, MTXMODE_APPLY);
            Matrix_Scale(sFishingLineScale, 1.0f, dist, MTXMODE_APPLY);

            gSPMatrix(POLY_XLU_DISP++, MATRIX_NEWMTX(play->state.gfxCtx),
                      G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
            gSPDisplayList(POLY_XLU_DISP++, gFishingLineModelDL);
        }
    } else {
        // Step 27e-2: at the rod (0) and at rest (3) the line ends at the RF3 lure's tie (lure-tie log: 3.2 and 3.4
        // units short; OoT's lure is drawn sLurePosZOffset along its axis and its eye is sLureDrawPos). Flight and
        // landing keep OoT's line.
        Vec3f rf3Tie;
        s32 rf3TieOn = sRF3LureModelShown && ((sRodCastState == 0) || (sRodCastState == 3)) &&
                       RF3LureModel_TieAny(&rf3Tie.x);
        // Step 29g-5 (user's choice after 29g-3/4 failed, 2026-10-13): at rest with a float the line is straight, as
        // in the fight: rod tip -> float -> the bait (the lure's tie, or OoT's lure eye). OoT's segments meet the water
        // short of the float, so no cut of them gave rod -> float.
        if (rf3FloatOn && (sRodCastState == 3)) {
            Vec3f* end = rf3TieOn ? &rf3Tie : &sLureDrawPos;

            lineDrawnEnd = *end;
            Fishing_RF3LinePiece(play, &sRodTipPos, &rf3Float);
            Fishing_RF3LinePiece(play, &rf3Float, end);
            spooled = LINE_SEG_COUNT; // skip OoT's segments below
        }
        for (i = spooled; i < LINE_SEG_COUNT - 1; i++) {
            if (((i == LINE_SEG_COUNT - 3) && (sLureEquipped == FS_LURE_STOCK) && (sRodCastState == 3)) ||
                (rf3TieOn && (sRodCastState == 0) && (i == LINE_SEG_COUNT - 2))) {
                f32 rx;
                f32 ry;
                f32 dist;
                f32 dx;
                f32 dy;
                f32 dz;
                Vec3f* end = rf3TieOn ? &rf3Tie : &sLureDrawPos;

                lineDrawnEnd = *end;
                dx = end->x - (linePos + i)->x;
                dy = end->y - (linePos + i)->y;
                dz = end->z - (linePos + i)->z;

                ry = Math_FAtan2F(dx, dz);
                dist = sqrtf(SQ(dx) + SQ(dz));
                rx = -Math_FAtan2F(dy, dist);

                dist = sqrtf(SQ(dx) + SQ(dy) + SQ(dz)) * 0.001f;

                Matrix_Translate((linePos + i)->x, (linePos + i)->y, (linePos + i)->z, MTXMODE_NEW);
                Matrix_RotateY(ry, MTXMODE_APPLY);
                Matrix_RotateX(rx, MTXMODE_APPLY);
                Matrix_Scale(sFishingLineScale, 1.0f, dist, MTXMODE_APPLY);

                gSPMatrix(POLY_XLU_DISP++, MATRIX_NEWMTX(play->state.gfxCtx),
                          G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
                gSPDisplayList(POLY_XLU_DISP++, gFishingLineModelDL);
                break;
            }

            Matrix_Translate((linePos + i)->x, (linePos + i)->y, (linePos + i)->z, MTXMODE_NEW);
            Matrix_RotateY((lineRot + i)->y, MTXMODE_APPLY);
            Matrix_RotateX((lineRot + i)->x, MTXMODE_APPLY);
            Matrix_Scale(sFishingLineScale, 1.0f, 0.005f, MTXMODE_APPLY);

            gSPMatrix(POLY_XLU_DISP++, MATRIX_NEWMTX(play->state.gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
            gSPDisplayList(POLY_XLU_DISP++, gFishingLineModelDL);
        }
    }
    if (sRF3LureModelShown) {
        RF3LureModel_LogLineEnd(&lineDrawnEnd.x, sRodCastState, play->gameplayFrames);
    }

    Matrix_Pop();
    Gfx_SetupDL_25Xlu(play->state.gfxCtx);

    CLOSE_DISPS(play->state.gfxCtx);
}

static f32 sRodScales[22] = {
    1.0f,        1.0f,        1.0f,        0.9625f,     0.925f, 0.8875f,     0.85f,       0.8125f,
    0.775f,      0.73749995f, 0.7f,        0.6625f,     0.625f, 0.5875f,     0.54999995f, 0.5125f,
    0.47499996f, 0.4375f,     0.39999998f, 0.36249995f, 0.325f, 0.28749996f,
};

static f32 sRodBendRatios[22] = {
    0.0f,  0.0f,  0.0f,  0.0f,  0.0f,  0.06f,   0.12f,   0.18f,   0.24f,   0.30f,   0.36f,
    0.42f, 0.48f, 0.54f, 0.60f, 0.60f, 0.5142f, 0.4285f, 0.3428f, 0.2571f, 0.1714f, 0.0857f,
};

static Vec3f sRodTipOffset = { 0.0f, 0.0f, 0.0f };

void Fishing_DrawRod(PlayState* play) {
    s16 i;
    f32 lureXZLen;
    f32 spC4;
    f32 spC0;
    Input* input = &play->state.input[0];
    Player* player = GET_PLAYER(play);
    s32 pad;

    OPEN_DISPS(play->state.gfxCtx);

    if (sRodHitTimer != 0) {
        sRodHitTimer--;

        Math_ApproachF(&D_80B7A6C0, 35.0f, 1.0f, 100.0f);
        Math_ApproachF(&D_80B7A6BC, -0.8f, 1.0f, 0.4f);
        Math_ApproachS(&player->actor.shape.rot.x, -4000, 2, 15000);
    } else {
        s16 target = 0;

        if ((sRodCastState == 4) && sLineHooked) {
            target = Math_SinS(sLureTimer * 25600) * 1500.0f;
        } else {
            Math_ApproachZeroF(&D_80B7A6C0, 0.1f, 10.0f);
            Math_ApproachZeroF(&D_80B7A6BC, 1.0f, 0.05f);
        }

        Math_ApproachS(&player->actor.shape.rot.x, target, 5, 1000);
    }

    if ((sRodCastState == 3) || (sRodCastState == 4)) {
        if ((input->rel.stick_x == 0) && (sStickAdjXPrev != 0)) {
            D_80B7A6B0 = 0.0f;
        }
        if ((input->rel.stick_y == 0) && (sStickAdjYPrev != 0)) {
            D_80B7A6B4 = 0.0f;
        }

        lureXZLen = player->unk_85C;
        Math_SmoothStepToF(&player->unk_85C, input->rel.stick_y * 0.02f, 0.3f, 5.0f, 0.0f);
        lureXZLen = player->unk_85C - lureXZLen;

        spC4 = player->unk_858;
        Math_SmoothStepToF(&player->unk_858,
                           input->rel.stick_x * 0.02f * (CVarGetInteger(CVAR_ENHANCEMENT("MirroredWorld"), 0) ? -1 : 1),
                           0.3f, 5.0f, 0.0f);
        spC4 = player->unk_858 - spC4;

        if (player->unk_858 > 1.0f) {
            player->unk_858 = 1.0f;
        }
        if (player->unk_85C > 1.0f) {
            player->unk_85C = 1.0f;
        }
        if (player->unk_858 < -1.0f) {
            player->unk_858 = -1.0f;
        }
        if (player->unk_85C < -1.0f) {
            player->unk_85C = -1.0f;
        }

        Math_ApproachF(&sRodBendRotY, spC4 * 70.0f * -0.01f, 1.0f, D_80B7A6B0);
        Math_ApproachF(&D_80B7A6B0, 1.0f, 1.0f, 0.1f);
        Math_ApproachF(&D_80B7A6AC, lureXZLen * 70.0f * 0.01f, 1.0f, D_80B7A6B4);
        Math_ApproachF(&D_80B7A6B4, 1.0f, 1.0f, 0.1f);
        Math_ApproachZeroF(&D_80B7A6B8, 1.0f, 0.05f);
    } else {
        Math_ApproachZeroF(&player->unk_85C, 1.0f, 0.1f);
        Math_ApproachZeroF(&player->unk_858, 1.0f, 0.1f);
        Math_ApproachF(&D_80B7A6AC, (Math_SinS(sLureTimer * 3000) * 0.025f) + -0.03f, 1.0f, 0.05f);
        Math_ApproachZeroF(&sRodBendRotY, 1.0f, 0.05f);

        if ((sRodCastTimer > 18) && (sRodCastTimer < 25)) {
            Math_ApproachF(&D_80B7A6B8, 0.8f, 1.0f, 0.2f);
        } else {
            Math_ApproachF(&D_80B7A6B8, 0.0f, 1.0f, 0.4f);
        }
    }

    Gfx_SetupDL_25Opa(play->state.gfxCtx);

    gSPDisplayList(POLY_OPA_DISP++, gFishingRodMaterialDL);

    gDPSetPrimColor(POLY_OPA_DISP++, 0, 0, 255, 155, 0, 255);

    Matrix_Mult(&player->mf_9E0, MTXMODE_NEW);

    if (sLinkAge != LINK_AGE_CHILD) {
        Matrix_Translate(0.0f, 400.0f, 0.0f, MTXMODE_APPLY);
    } else {
        Matrix_Translate(0.0f, 230.0f, 0.0f, MTXMODE_APPLY);
    }

    if (sRodCastState == 5) {
        Matrix_RotateY(0.56f * M_PI, MTXMODE_APPLY);
    } else {
        Matrix_RotateY(0.41f * M_PI, MTXMODE_APPLY);
    }

    Matrix_RotateX(-M_PI / 5.0000003f, MTXMODE_APPLY);
    Matrix_RotateZ((player->unk_858 * 0.5f) + 3.0f * M_PI / 20.0f, MTXMODE_APPLY);
    Matrix_RotateX((D_80B7A6C0 + 20.0f) * 0.01f * M_PI, MTXMODE_APPLY);
    Matrix_Scale(0.70000005f, 0.70000005f, 0.70000005f, MTXMODE_APPLY);

    spC0 = (D_80B7A6BC * (((player->unk_85C - 1.0f) * -0.25f) + 0.5f)) + (D_80B7A6AC + D_80B7A6B8);

    // Reel Fishing III mod (step 25f-2): the chosen box's RF3 rod, bent by OoT's bend; it sets sRodTipPos (where
    // the line starts) from its own tip. OoT's pole (box 10) and anything without a model keep OoT's 22 segments
    // (the loop below runs 0 times when the RF3 rod was drawn; CLOSE_DISPS must stay at the end).
    s32 rf3RodDrawn =
        RF3RodModel_Draw(play, sRodBendRotY, spC0, sRodBendRatios, ARRAY_COUNT(sRodBendRatios), &sRodTipPos.x);

    Matrix_Translate(0.0f, 0.0f, -1300.0f, MTXMODE_APPLY);

    for (i = 0; i < (rf3RodDrawn ? 0 : 22); i++) {
        Matrix_RotateY(sRodBendRatios[i] * sRodBendRotY * 0.5f, MTXMODE_APPLY);
        Matrix_RotateX(sRodBendRatios[i] * spC0 * 0.5f, MTXMODE_APPLY);

        Matrix_Push();
        Matrix_Scale(sRodScales[i], sRodScales[i], 0.52f, MTXMODE_APPLY);

        gSPMatrix(POLY_OPA_DISP++, MATRIX_NEWMTX(play->state.gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);

        if (i < 5) {
            gDPLoadTextureBlock(POLY_OPA_DISP++, gFishingRodSegmentBlackTex, G_IM_FMT_RGBA, G_IM_SIZ_16b, 16, 8, 0,
                                G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMIRROR | G_TX_WRAP, 4, 3, G_TX_NOLOD, G_TX_NOLOD);
        } else if ((i < 8) || ((i % 2) == 0)) {
            gDPLoadTextureBlock(POLY_OPA_DISP++, gFishingRodSegmentWhiteTex, G_IM_FMT_RGBA, G_IM_SIZ_16b, 16, 8, 0,
                                G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMIRROR | G_TX_WRAP, 4, 3, G_TX_NOLOD, G_TX_NOLOD);
        } else {
            gDPLoadTextureBlock(POLY_OPA_DISP++, gFishingRodSegmentStripTex, G_IM_FMT_RGBA, G_IM_SIZ_16b, 16, 8, 0,
                                G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMIRROR | G_TX_WRAP, 4, 3, G_TX_NOLOD, G_TX_NOLOD);
        }

        gSPDisplayList(POLY_OPA_DISP++, gFishingRodSegmentDL);

        Matrix_Pop();
        Matrix_Translate(0.0f, 0.0f, 500.0f, MTXMODE_APPLY);

        if (i == 21) {
            Matrix_MultVec3f(&sRodTipOffset, &sRodTipPos);
        }
    }

    CLOSE_DISPS(play->state.gfxCtx);
}

static Vec3f sSoundPos = { 0.0f, 0.0f, 0.0f };

// Step 20a-5: where the lure rests on the bottom, as metres below the surface (OoT's sinking lure rests
// 5 units above the floor). Portable: the known ground under the lure.
static f32 Fishing_RF3LureFloorDepthM(PlayState* play) {
    f32 floorY;

    if (sRF3Portable) {
        floorY = sRF3PortableFloorY;
    } else {
        CollisionPoly* poly;
        Vec3f probe = sLurePos;

        probe.y = WATER_SURFACE_Y(play) + 10.0f;
        floorY = BgCheck_EntityRaycastFloor1(&play->colCtx, &poly, &probe);
    }
    return (WATER_SURFACE_Y(play) - (floorY + 5.0f)) / RF3_UNITS_PER_METRE;
}

// Step 20a-5: RF3's lure depth sets the lure's height; OoT keeps moving it horizontally.
static void Fishing_RF3LureDepthApply(PlayState* play, Input* input) {
    f32 depthM = RF3Lure_Step(CHECK_BTN_ALL(input->cur.button, BTN_A), input->rel.stick_y,
                              Fishing_RF3LureFloorDepthM(play), RF3_SUBSTEPS);

    sReelLinePos[LINE_SEG_COUNT - 1].y = sLurePos.y = WATER_SURFACE_Y(play) - (depthM * RF3_UNITS_PER_METRE);
}

// Step 20a-6: with the lure sim active the lure leads and the line follows it, so the rod's swing doesn't drag
// it (user, 2026-10-07). Only the reel moves it horizontally: straight toward the rod tip by reelUnits.
static void Fishing_RF3LureReel(f32 reelUnits) {
    f32 dx = sRodTipPos.x - sLurePos.x;
    f32 dz = sRodTipPos.z - sLurePos.z;
    f32 dist = sqrtf(SQ(dx) + SQ(dz));

    if ((reelUnits > 0.0f) && (dist > 0.001f)) {
        if (reelUnits > dist) {
            reelUnits = dist;
        }
        sLurePos.x += dx / dist * reelUnits;
        sLurePos.z += dz / dist * reelUnits;
    }
}

// Step 20a-6: RF3's cancel (user, 2026-10-07). The rod button (B at the pond) ends the cast at once, as OoT's own
// reeled-in end does. With a fish on, OoT's escape path (sRodCastState < 3) then ends the RF3 fight and lets the
// fish go on its next update.
static void Fishing_RF3Cancel(void) {
    s32 fishOn = (sRodCastState == 4);

    RF3Rod_LogEvent("rf3-cancel", fishOn);
    RF3Lure_End();
    sRF3Cancelled = fishOn;
    sRodLineSpooled = 195.0f;
    sRodCastState = 0;
    D_80B7E148 = 520.0f;
    sFishingPlayerCinematicState = 3;
}

void Fishing_UpdateLure(Fishing* this, PlayState* play) {
    f32 spE4;
    f32 spE0;
    s16 timer;
    s16 spDC;
    f32 spD8;
    f32 spD4;
    f32 spD0;
    f32 lengthCasted;
    f32 lureXZLen;
    s16 i;
    Player* player = GET_PLAYER(play);
    Vec3f zeroVec = { 0.0f, 0.0f, 0.0f };
    Vec3f spA8;
    Vec3f sp9C;
    Vec3f sp90;
    Input* input = &play->state.input[0];
    Vec3f sp80;
    f32 sp7C;
    f32 sp78;
    f32 phi_f0;
    f32 wiggle;
    Vec3f sp64;
    Vec3f sp58;
    s32 pad;
    f32 rf3ReelUnits = 0.0f; // step 20a-6: this frame's reel, OoT units

    sLureTimer++;

    if (D_80B7E0B0 != 0) {
        D_80B7E0B0--;
    }

    if (D_80B7E0B2 != 0) {
        D_80B7E0B2--;
    }

    if (sRodCastTimer != 0) {
        sRodCastTimer--;
    }

    if (sLureBitTimer != 0) {
        sLureBitTimer--;
    }

    if (D_80B7E150 != 0) {
        D_80B7E150--;
    }

    if (D_80B7A6A4 != 0) {
        D_80B7A6A4--;
    }

    if (sRumbleDelay != 0) {
        sRumbleDelay--;
    }

    if (D_80B7E114 != 0) {
        D_80B7E114--;
    }

    if (sFishingPlayingState == 1) {
        sFishingPlayingState = 2;
        sFishesCaught = 0;
        sPondOwnerTextIdIndex = 0;
        sLureEquipped = FS_LURE_STOCK;

        // if prize item won as child or adult, set the sinking lure location.
        if (((sLinkAge == LINK_AGE_CHILD) && (HIGH_SCORE(HS_FISHING) & HS_FISH_PRIZE_CHILD)) ||
            ((sLinkAge != LINK_AGE_CHILD) && (HIGH_SCORE(HS_FISHING) & HS_FISH_PRIZE_ADULT))) {
            sSinkingLureLocation = (u8)Rand_ZeroFloat(3.999f) + 1;
        }

        D_80B7E148 = 520.0f;
        sRodLineSpooled = 195.0f;

        sRodCastState = sLureEquipped = sLureTimer = D_80B7E0B0 = D_80B7E0B2 = sRodCastTimer = sWiggleAttraction =
            D_80B7E114 = D_80B7E150 = 0;
        sLure1Rotate = sReelLinePosStep = sLurePosZOffset = 0.0f;

        sLureLineSegPosDelta = zeroVec;

        for (i = 0; i < LINE_SEG_COUNT; i++) {
            sReelLinePos[i] = zeroVec;
            sReelLineRot[i] = zeroVec;
            sReelLineUnk[i] = zeroVec;
        }
    }

    SkinMatrix_Vec3fMtxFMultXYZW(&play->viewProjectionMtxF, &sLurePos, &sSoundPos, &sProjectedW);

    if (RF3Lure_IsActive() && (sRodCastState != 2) && (sRodCastState != 3)) {
        RF3Lure_End(); // step 20a-5: a fish took the lure, or it was reeled in
    }
    if (RF3FlyCast_IsCharging() && ((sRodCastState != 0) || (sRodCastTimer == 0))) {
        RF3FlyCast_SetCharging(false); // step 20c-3c: the cast ended another way
    }

    // Step 20a-6: with RF3 on, the rod button cancels (lure in the water or on land, or a fish on). It replaces
    // OoT's rod-button wiggle and its pull on a lure on land.
    if (RF3_IsEnabled() && ((sRodCastState == 2) || (sRodCastState == 3) || (sRodCastState == 4)) &&
        CHECK_BTN_ALL(play->state.input[0].press.button, sRF3RodBtn)) {
        Fishing_RF3Cancel();
    }

    if (sRodCastState == 0) {
        Math_ApproachF(&sLurePosZOffset, -800.0f, 1.0f, 20.0f);
    } else {
        Math_ApproachF(&sLurePosZOffset, 300.0f, 1.0f, 20.0f);
    }

    switch (sRodCastState) {
        case 0:
            sSinkingLureSegmentIndex = 0;

            // Reel Fishing III mod (step 23c): the lure on the line follows the tackle choice: OoT's sinking lure
            // only when the pole's sinking lure is chosen, else the normal lure (RF3's rods use RF3's lure model).
            if (RF3_IsEnabled()) {
                sLureEquipped = (RF3Tackle_PoleLure() == 1) ? FS_LURE_SINKING : FS_LURE_STOCK;
            }

            if (KREG(14) != 0) {
                KREG(14) = 0;
                sLureEquipped = FS_LURE_SINKING - sLureEquipped;
                if (sLureEquipped != FS_LURE_STOCK) {
                    sPondOwnerTextIdIndex = 0;
                }
            }

            Math_ApproachF(&sRodLineSpooled, 195.0f, 1.0f, 1.0f);

            if (player->stateFlags1 & PLAYER_STATE1_IN_WATER) {
                sRodCastTimer = 0;
                player->unk_860 = 0;
            }

            if (sRodCastTimer == 0) {
                const char* rf3NotReady = NULL;
                if ((D_80B7E0B0 == 0) && (player->unk_860 == 1) && RF3_IsEnabled() &&
                    !RF3Tackle_Ready(&rf3NotReady)) {
                    // Reel Fishing III mod (step 21d): no owned lure/fly/bait in the chosen box: no cast; RF3's
                    // reason in a text box (it borrows the "got away" text, see RF3Messages.cpp).
                    player->unk_860 = 0;
                    if (Message_GetState(&play->msgCtx) == TEXT_STATE_NONE) {
                        RF3Tackle_ShowNotReady(rf3NotReady);
                        Message_StartTextbox(play, 0x4082, NULL);
                        Audio_PlaySfxGeneral(NA_SE_SY_ERROR, &gSfxDefaultPos, 4, &gSfxDefaultFreqAndVolScale,
                                             &gSfxDefaultFreqAndVolScale, &gSfxDefaultReverb);
                    }
                    RF3Rod_LogEvent("cast-refused", play->sceneNum); // the reason: RF3Tackle_Ready, the tackle-* lines
                } else if ((D_80B7E0B0 == 0) && (player->unk_860 == 1)) {
                    sRodCastTimer = 37;
                    Message_CloseTextbox(play);
                    sRF3CastHeldFrames = 0; // step 11: B was just pressed; charging starts
                    sRF3CastCharging = true;
                }
            } else {
                sLureRot.x = sReelLineRot[LINE_SEG_COUNT - 2].x + M_PI;
                sLureRot.y = sReelLineRot[LINE_SEG_COUNT - 2].y;

                // Step 11: RF3 charges while B is held; letting go fixes the power.
                if (sRF3CastCharging && CHECK_BTN_ALL(input->cur.button, sRF3RodBtn)) {
                    sRF3CastHeldFrames++;
                    // Step 20c-3c: a fly cast charges for as long as the button is held. The throw waits and Link
                    // false-casts (z_player.c).
                    if (RF3_IsEnabled() && RF3FlyCast_IsFlyBox() && (sRodCastTimer <= RF3_FLY_HOLD_TIMER)) {
                        sRodCastTimer = RF3_FLY_HOLD_TIMER;
                        RF3FlyCast_SetCharging(true);
                    }
                } else {
                    if (RF3FlyCast_IsCharging()) {
                        // Step 20c-3c: released. Cast when Link's throw reaches its release frame (1.5 animation
                        // frames per game frame).
                        f32 framesLeft = (RF3FlyCast_Tuning(3) - player->skelAnime.curFrame) / (R_UPDATE_RATE * 0.5f);
                        s32 wait = (s32)ceilf(framesLeft);
                        sRodCastTimer = 18 + CLAMP(wait, 1, 30);
                        RF3FlyCast_SetCharging(false);
                    }
                    sRF3CastCharging = false;
                }

                if (sRodCastTimer == 18) {
                    sRodCastState = 1;
                    sLurePos = sRodTipPos;
                    Matrix_RotateY((player->actor.shape.rot.y / 32768.0f) * M_PI, MTXMODE_NEW);
                    sp90.x = 0.0f;
                    sp90.y = 0.0f;
                    sp90.z = 25.0f;
                    Matrix_MultVec3f(&sp90, &sLurePosDelta);
                    if (RF3_IsEnabled()) {
                        // Step 11: scale OoT's forward launch speed by the charged power
                        // (a full hold through the wind-up = OoT's normal full cast).
                        f32 castFactor = RF3FlyCast_IsFlyBox()
                                             ? RF3Cast_LaunchFly(sRF3CastHeldFrames, RF3_SUBSTEPS) // step 20c-3c
                                             : RF3Cast_Launch(sRF3CastHeldFrames, RF3_CAST_WINDUP_FRAMES, RF3_SUBSTEPS);
                        sLurePosDelta.x *= castFactor;
                        sLurePosDelta.z *= castFactor;
                    }
                    sRF3CastCharging = false;
                    sLurePosDelta.y = 15.0f;
                    sLureCastDelta.x = sLureCastDelta.z = 0.0f;
                    sLureCastDelta.y = -1.0f;
                    D_80B7E148 = 0.0f;
                    D_80B7E0B2 = 5;
                    sRodReelingSpeed = 0.5f;
                    D_80B7E118 = Rand_ZeroFloat(1.9f);
                    sFishMouthOffset.y = 500.0f;
                    Sfx_PlaySfxAtPos(&sSoundPos, NA_SE_IT_SWORD_SWING_HARD);
                }
            }
            break;

        case 1: // casting the line
            spE0 = sLurePos.y;

            sLurePos.x += sLurePosDelta.x;
            sLurePos.y += sLurePosDelta.y;
            sLurePos.z += sLurePosDelta.z;

            sLurePosDelta.x += sLureCastDelta.x;
            sLurePosDelta.y += sLureCastDelta.y;
            sLurePosDelta.z += sLureCastDelta.z;
            // air drag by hat or reeling during cast.
            if (CHECK_BTN_ALL(input->cur.button, BTN_A) || sIsOwnersHatHooked) {
                sLurePosDelta.x *= 0.9f;
                sLurePosDelta.z *= 0.9f;
                if (!sIsOwnersHatHooked) {
                    Sfx_PlaySfxCentered(NA_SE_IT_FISHING_REEL_HIGH - SFX_FLAG);
                }
            }

            spD8 = sLurePos.x - sRodTipPos.x;
            spD4 = sLurePos.y - sRodTipPos.y;
            spD0 = sLurePos.z - sRodTipPos.z;

            if (D_80B7E0B2 != 0) {
                sLureRot.x = sReelLineRot[LINE_SEG_COUNT - 2].x + M_PI;
                sLureRot.y = sReelLineRot[LINE_SEG_COUNT - 2].y;
            } else {
                sLureRot.x = 0.0f;
                sLureRot.y = Math_Atan2F(spD0, spD8) + M_PI;
            }

            lengthCasted = sqrtf(SQ(spD8) + SQ(spD4) + SQ(spD0));
            if (lengthCasted > 1000.0f) {
                lengthCasted = 1000.0f;
            }
            sRodLineSpooled = 200.0f - (lengthCasted * 200.0f * 0.001f);

            lureXZLen = SQ(sLurePos.x) + SQ(sLurePos.z);
            if (Fishing_RF3OnLand(lureXZLen)) { // step 15c
                if (sRF3Portable || (KREG(56) != 0) || (sLurePos.y > 160.0f) || (sLurePos.x < 80.0f) || (sLurePos.x > 180.0f) ||
                    (sLurePos.z > 1350.0f) || (sLurePos.z < 1100.0f) || (sLurePos.y < 45.0f)) {
                    sp80 = this->actor.world.pos;
                    this->actor.prevPos = this->actor.world.pos = sLurePos;
                    Actor_UpdateBgCheckInfo(play, &this->actor, 15.0f, 30.0f, 30.0f, 0x43);
                    this->actor.world.pos = sp80;

                    if (this->actor.bgCheckFlags & 0x10) {
                        sLurePosDelta.y = -0.5f;
                    }
                    if (this->actor.bgCheckFlags & BGCHECKFLAG_WALL) {
                        if (sLurePosDelta.y > 0.0f) {
                            sLurePosDelta.y = 0.0f;
                        }
                        sLurePosDelta.x = sLurePosDelta.z = 0.0f;
                    }
                } else {
                    if (Fishing_IsAboveCounter(&sLurePos)) {
                        sRodCastState = 3;
                        sReelLinePosStep = 0.0f;
                    }
                }

                spE4 = Fishing_RF3LandY(play, lureXZLen, 0.11f); // step 15c
                if (sLurePos.y <= spE4) {
                    sLurePos.y = spE4;
                    sLurePosDelta.x = sLurePosDelta.y = sLurePosDelta.z = 0.0f;
                    sRodCastState = 3;
                    sReelLinePosStep = 0.0;
                } else {
                    Math_ApproachF(&D_80B7E148, 0.0f, 1.0f, 0.05f);
                    Sfx_PlaySfxAtPos(&sSoundPos, NA_SE_EN_FANTOM_FLOAT - SFX_FLAG);
                }
            } else {
                spE4 = WATER_SURFACE_Y(play);

                if (sLurePos.y <= spE4) {
                    sRodCastState = 2;
                    sReelLinePosStep = 0.0f;
                    sLurePosDelta.x = sLurePosDelta.z = 0.0f;
                    RF3Lure_Begin(); // step 20a-5 (inactive with RF3 off)

                    if (FISHING_LURE_SINKS()) {
                        sLureMoveDelay = 0;
                    } else {
                        sLureMoveDelay = 10;
                    }

                    if ((sLurePos.y <= spE4) && (spE4 < spE0) && (spE4 == WATER_SURFACE_Y(play))) {
                        D_80B7E114 = 10;
                        Sfx_PlaySfxAtPos(&sSoundPos, NA_SE_EV_BOMB_DROP_WATER);
                        sLureCastDelta.y = 0.0f;
                        sLurePosDelta.y *= 0.2f;

                        for (i = 0; i < 50; i++) {
                            sp7C = Rand_ZeroFloat(1.5f) + 0.5f;
                            sp78 = Rand_ZeroFloat(6.28f);

                            sp9C.x = sinf(sp78) * sp7C;
                            sp9C.z = cosf(sp78) * sp7C;
                            sp9C.y = Rand_ZeroFloat(3.0f) + 3.0f;

                            spA8 = sLurePos;
                            spA8.x += (sp9C.x * 3.0f);
                            spA8.y = WATER_SURFACE_Y(play);
                            spA8.z += (sp9C.z * 3.0f);
                            Fishing_SpawnDustSplash(NULL, sFishingEffects, &spA8, &sp9C,
                                                    Rand_ZeroFloat(0.02f) + 0.025f);
                        }

                        spA8 = sLurePos;
                        spA8.y = WATER_SURFACE_Y(play);
                        Fishing_SpawnRipple(NULL, sFishingEffects, &spA8, 100.0f, 800.0f, 150, 90);
                    }
                } else {
                    Math_ApproachZeroF(&D_80B7E148, 1.0f, 0.05f);
                    Sfx_PlaySfxAtPos(&sSoundPos, NA_SE_EN_FANTOM_FLOAT - SFX_FLAG);
                }
            }

            sReelLinePos[LINE_SEG_COUNT - 1].x = sLurePos.x;
            sReelLinePos[LINE_SEG_COUNT - 1].y = sLurePos.y;
            sReelLinePos[LINE_SEG_COUNT - 1].z = sLurePos.z;

            sLureWiggleSign = 1.0f;
            sLureRotXTarget = 0.5f;
            break;

        case 2:
            if (RF3Lure_IsActive()) {
                Fishing_RF3LureDepthApply(play, input); // step 20a-5
            } else if (sLurePos.y <= WATER_SURFACE_Y(play)) {
                sLurePos.y += sLurePosDelta.y;

                Math_ApproachZeroF(&sLurePosDelta.y, 1.0f, 1.0f);

                if (!FISHING_LURE_SINKS()) {
                    Math_ApproachF(&sLurePos.y, WATER_SURFACE_Y(play), 0.5f, 1.0f);
                }
            }

            Math_ApproachF(&D_80B7E148, 2.0f, 1.0f, 0.1f);

            if (sLureMoveDelay == 0) {
                sRodCastState = 3;
            } else {
                sLureMoveDelay--;
            }
            break;

        case 3:
            sSinkingLureSegmentIndex = 0;

            if (sIsOwnersHatHooked && ((SQ(sLurePos.x) + SQ(sLurePos.z)) < SQ(500.0f))) {
                sIsOwnersHatSunk = true;
            }

            player->unk_860 = 2;

            if (sLureWigglePosY < 3.0f) {
                spD0 = sLureRotXTarget * Math_SinS(sLureTimer * 0x1060);
                Math_ApproachF(&sLureRot.x, -M_PI / 6.0f + spD0, 0.3f, sLureRotXStep);
                Math_ApproachF(&sLureRotXStep, 0.5f, 1.0f, 0.02f);
                Math_ApproachZeroF(&sLureRotXTarget, 1.0f, 0.02f);
            } else {
                sLureRotXStep = 0.0f;
            }

            spDC = 0x4000;
            spE4 = WATER_SURFACE_Y(play);

            lureXZLen = SQ(sLurePos.x) + SQ(sLurePos.z);
            if (!Fishing_RF3OnLand(lureXZLen)) { // step 15c
                if (sLurePos.y <= (spE4 + 4.0f)) {
                    wiggle = 0.0f;

                    // Step 20a-5: with RF3's lure depth active the stick twitches the lure RF3's way
                    // (RF3Lure_Step), so OoT's stick wiggle is off (user, 2026-10-07).
                    if ((D_80B7E150 == 0) && !RF3Lure_IsActive()) {
                        if (fabsf(input->rel.stick_x) > 30.0f) {
                            wiggle = fabsf((input->rel.stick_x - sStickAdjXPrev) * (1.0f / 60.0f));
                        } else if (fabsf(input->rel.stick_y) > 30.0f) {
                            wiggle = fabsf((input->rel.stick_y - sStickAdjYPrev) * (1.0f / 60.0f));
                        }
                    }

                    if (wiggle > 1.0f) {
                        wiggle = 1.0f;
                    }
                    if (CHECK_BTN_ALL(input->press.button, sRF3RodBtn)) {
                        wiggle = 0.5f;
                    }

                    if (sIsOwnersHatHooked) {
                        if (wiggle > 0.3f) {
                            wiggle = 0.3f;
                        }
                    }

                    if ((wiggle > 0.2f) && (sLureWigglePosY < 4.0f)) {
                        D_80B7E150 = 5;

                        if (wiggle > 0.8f) {
                            sWiggleAttraction = 2;
                        } else {
                            sWiggleAttraction = 1;
                        }

                        sp90.x = player->actor.world.pos.x - sLurePos.x;
                        sp90.z = player->actor.world.pos.z - sLurePos.z;
                        sp90.y = Math_Atan2F(sp90.z, sp90.x);

                        sLureWiggleRotYTarget = (wiggle * sLureWiggleSign) + sp90.y;
                        sLureWiggleSign *= -1.0f;
                        sLureWigglePosY = fabsf(wiggle) * 6.0f;
                        sLureRot.x = 0.0f;
                        sLureRotXTarget = 0.5f;
                        sRodLineSpooled += (fabsf(wiggle) * (7.5f + (KREG(25) * 0.1f)));

                        func_800F436C(&sSoundPos, NA_SE_EV_LURE_MOVE_W, (wiggle * 1.999f * 0.25f) + 0.75f);

                        if (FISHING_LURE_SINKS()) {
                            sLureLineSegPosDelta.y = 5.0f * wiggle;
                            sReelLinePos[LINE_SEG_COUNT - 1].y += sLureLineSegPosDelta.y;
                            sLurePos.y += sLureLineSegPosDelta.y;
                        }
                    } else if (CHECK_BTN_ALL(input->cur.button, BTN_A)) {
                        spDC = 0x500;
                        sLureWiggleRotYTarget = sReelLineRot[LINE_SEG_COUNT - 2].y + M_PI;
                        sLureRot.x = 0.0f;
                        sLureRotXTarget = 0.5f;
                        if (FISHING_LURE_SINKS()) {
                            sLureLineSegPosDelta.y = 0.2f;
                            sReelLinePos[LINE_SEG_COUNT - 1].y += sLureLineSegPosDelta.y;
                            sLurePos.y += sLureLineSegPosDelta.y;
                        }
                    }
                } else {
                    if (sRodLineSpooled > 150.0f) {
                        sLureRot.x = sReelLineRot[LINE_SEG_COUNT - 2].x + M_PI;
                        sLureWiggleRotYTarget = sReelLineRot[LINE_SEG_COUNT - 2].y + M_PI;
                        sRodLineSpooled += 2.0f;
                    }
                }
            } else {
                spE4 = Fishing_RF3LandY(play, lureXZLen, 0.11f); // step 15c
                if (sLurePos.y <= spE4) {
                    sLurePos.y = spE4;
                    spDC = 0x500;
                    sLureWiggleRotYTarget = sReelLineRot[LINE_SEG_COUNT - 2].y + M_PI;
                    sLureRot.x = 0.0f;
                    if (CHECK_BTN_ALL(input->press.button, sRF3RodBtn)) {
                        sRodLineSpooled += 6.0f;
                        Sfx_PlaySfxAtPos(&sSoundPos, NA_SE_PL_WALK_SAND);
                    }
                } else {
                    if (sRodLineSpooled > 150.0f) {
                        sLureRot.x = sReelLineRot[LINE_SEG_COUNT - 2].x + M_PI;
                        sLureWiggleRotYTarget = sReelLineRot[LINE_SEG_COUNT - 2].y + M_PI;
                        sRodLineSpooled += 2.0f;
                    }
                }
            }

            Math_ApproachZeroF(&sLureWigglePosY, 1.0f, 0.3f);
            Math_ApproachS(&sLureWiggleRotY, (sLureWiggleRotYTarget * 32768.0f) / M_PI, 3, spDC);

            sLureRot.y = (sLureWiggleRotY / 32768.0f) * M_PI;

            sp90.x = 0.0f;
            sp90.y = 0.0f;
            sp90.z = sLureWigglePosY;

            Matrix_RotateY(sLureRot.y, MTXMODE_NEW);

            if (FISHING_LURE_SINKS()) {
                Matrix_MultVec3f(&sp90, &sp64);
                sLureLineSegPosDelta.x = sp64.x;
                sLureLineSegPosDelta.z = sp64.z;
                phi_f0 = 10.0f;
            } else {
                Matrix_MultVec3f(&sp90, &sLureLineSegPosDelta);
                phi_f0 = 0.0f;
            }

            sLure1Rotate = 0.0f;

            if ((sLureEquipped == FS_LURE_UNK) && CHECK_BTN_ALL(input->cur.button, BTN_A)) {
                sLureLineSegPosDelta.y = -2.0f;

                if ((sLureTimer & 1) != 0) {
                    sLure1Rotate = 0.5f;
                } else {
                    sLure1Rotate = -0.5f;
                }
            } else if (!RF3Lure_IsActive() && (sReelLinePos[LINE_SEG_COUNT - 1].y < (WATER_SURFACE_Y(play) + phi_f0))) {
                if (FISHING_LURE_SINKS()) {
                    sp58 = this->actor.world.pos;
                    this->actor.prevPos = this->actor.world.pos = sLurePos;
                    Actor_UpdateBgCheckInfo(play, &this->actor, 15.0f, 30.0f, 30.0f, 0x44);
                    this->actor.world.pos = sp58;

                    sLureLineSegPosDelta.y += -0.5f;
                    if (sLureLineSegPosDelta.y < -1.0f) {
                        sLureLineSegPosDelta.y = -1.0f;
                    }

                    if (sLurePos.y < (this->actor.floorHeight + 5.0f)) {
                        sReelLinePos[LINE_SEG_COUNT - 1].y = sLurePos.y = this->actor.floorHeight + 5.0f;
                        sLureLineSegPosDelta.y = 0.0f;
                    } else {
                        sWiggleAttraction = 1;
                    }
                } else {
                    sLureLineSegPosDelta.y = fabsf(sReelLinePos[LINE_SEG_COUNT - 1].y - WATER_SURFACE_Y(play)) * 0.2f;
                    if (sLureLineSegPosDelta.y > 1.5f) {
                        sLureLineSegPosDelta.y = 1.5f;
                    }
                }
            }

            sReelLinePos[LINE_SEG_COUNT - 1].x += sLureLineSegPosDelta.x;
            sReelLinePos[LINE_SEG_COUNT - 1].y += sLureLineSegPosDelta.y;
            sReelLinePos[LINE_SEG_COUNT - 1].z += sLureLineSegPosDelta.z;

            if (sReelLinePos[LINE_SEG_COUNT - 1].y > (spE4 + 6.0f)) {
                sReelLinePos[LINE_SEG_COUNT - 1].y -= 5.0f;
            }

            sLurePosDelta.x = sLurePosDelta.y = sLurePosDelta.z = sLureCastDelta.y = 0.0f;

            if (CHECK_BTN_ALL(input->cur.button, BTN_A)) {
                // Step 20a-6: OoT's line segments are 5 units, so a spool step of s = 5 s units of line.
                if (CHECK_BTN_ALL(input->cur.button, BTN_R)) {
                    rf3ReelUnits = 1.5f * 5.0f;
                    if (!RF3Lure_IsActive()) {
                        sRodLineSpooled += 1.5f;
                    }
                    Sfx_PlaySfxCentered(NA_SE_IT_FISHING_REEL_HIGH - SFX_FLAG);
                    Math_ApproachF(&sReelLinePosStep, 1000.0f, 1.0f, 2.0f);
                } else {
                    rf3ReelUnits = sRodReelingSpeed * 5.0f;
                    if (!RF3Lure_IsActive()) {
                        sRodLineSpooled += sRodReelingSpeed;
                    }
                    Sfx_PlaySfxCentered(NA_SE_IT_FISHING_REEL_SLOW - SFX_FLAG);
                    Math_ApproachF(&sReelLinePosStep, 1000.0f, 1.0f, 0.2f);
                }

                if (sReelLinePos[LINE_SEG_COUNT - 1].y > (WATER_SURFACE_Y(play) + 4.0f)) {
                    Math_ApproachF(&D_80B7E148, 3.0f, 1.0f, 0.2f);
                } else {
                    Math_ApproachF(&D_80B7E148, 1.0f, 1.0f, 0.2f);
                }
            } else {
                Math_ApproachF(&D_80B7E148, 2.0f, 1.0f, 0.2f);
            }

            if (RF3Lure_IsActive()) {
                Fishing_RF3LureReel(rf3ReelUnits); // step 20a-6: the lure leads
                if (rf3ReelUnits == 0.0f) {
                    // Step 20c-3b/4: a resting rig drifts with the current (RF3-MECHANICS 2.9), a lure at its share.
                    f32 curX, curZ;
                    if (RF3Current_At(!sRF3Portable, play->sceneNum, sLurePos.x, sLurePos.z, &curX, &curZ)) {
                        f32 share = RF3Lure_DriftScale() * (RF3_SUBSTEPS / 60.0f);
                        sLurePos.x += curX * share;
                        sLurePos.z += curZ * share;
                    }
                }
            } else {
                Math_ApproachF(&sLurePos.x, sReelLinePos[LINE_SEG_COUNT - 1].x, 1.0f, sReelLinePosStep);
                Math_ApproachF(&sLurePos.y, sReelLinePos[LINE_SEG_COUNT - 1].y, 1.0f, sReelLinePosStep);
                Math_ApproachF(&sLurePos.z, sReelLinePos[LINE_SEG_COUNT - 1].z, 1.0f, sReelLinePosStep);
            }

            if (RF3Lure_IsActive()) { // step 20a-5
                if (Fishing_RF3OnLand(SQ(sLurePos.x) + SQ(sLurePos.z))) {
                    RF3Lure_End();
                } else {
                    Fishing_RF3LureDepthApply(play, input);
                    // Step 20a-6: the line follows the lure. OoT's fish-on line mode draws it from the rod tip to
                    // its end and sets the spooled length from their distance (Fishing_UpdateLine).
                    sReelLinePos[LINE_SEG_COUNT - 1] = sLurePos;
                    D_80B7A6A4 = 2;
                    // The reel only closes the horizontal gap and the tip is 75-94 units above the water (user's
                    // log, 2026-10-07), so that distance never gets short enough for OoT's check below. Under the
                    // tip, the lure is reeled in.
                    if (SQ(sLurePos.x - sRodTipPos.x) + SQ(sLurePos.z - sRodTipPos.z) < SQ(RF3_LURE_REELED_IN_UNITS)) {
                        sRodLineSpooled = 195.0f;
                    }
                }
            }

            if (sLureWigglePosY > 1.0f) {
                Math_ApproachF(&sReelLinePosStep, 1000.0f, 1.0f, 1.0f);
            }

            Math_ApproachF(&sReelLinePosStep, 1000.0f, 1.0f, 0.1f);

            if (sRodLineSpooled >= 195.0f) {
                sRodLineSpooled = 195.0f;
                sRodCastState = 0;
                D_80B7E148 = 520.0f;
                sFishingPlayerCinematicState = 3;
            }

            if ((sLurePos.y <= (WATER_SURFACE_Y(play) + 4.0f)) && (sLurePos.y >= (WATER_SURFACE_Y(play) - 4.0f))) {

                timer = 63;
                if (CHECK_BTN_ALL(input->cur.button, BTN_A) || (sLureWigglePosY > 1.0f)) {
                    timer = 1;
                }

                if ((sLureTimer & timer) == 0) {
                    spA8 = sLurePos;
                    spA8.y = WATER_SURFACE_Y(play);
                    Fishing_SpawnRipple(NULL, sFishingEffects, &spA8, 30.0f, 300.0f, 150, 90);
                }
            }
            break;

        case 4:
            if (this->unk_157 != 0) {
                this->unk_157--;
                sRodLineSpooled += sRodReelingSpeed;
            }

            if (CHECK_BTN_ALL(input->cur.button, BTN_A)) {
                if (Fishing_RF3OnLand(SQ(sLurePos.x) + SQ(sLurePos.z))) { // step 15c
                    sRodLineSpooled += (1.0f + (KREG(65) * 0.1f));
                } else {
                    sRodLineSpooled += sRodReelingSpeed;
                }
                Sfx_PlaySfxCentered(NA_SE_IT_FISHING_REEL_SLOW - SFX_FLAG);
            }

            if ((sLureTimer & 0x1F) == 0) {
                if (sLineHooked || (sLureEquipped != FS_LURE_SINKING)) {
                    D_80B7A6A4 = 5;
                }
            }

            Math_ApproachF(&D_80B7E148, 0.0f, 1.0f, 0.2f);
            break;

        case 5:
            sFishingLineScale = 0.0005000001f;
            sReelLinePos[LINE_SEG_COUNT - 1].x = sLurePos.x;
            sReelLinePos[LINE_SEG_COUNT - 1].y = sLurePos.y;
            sReelLinePos[LINE_SEG_COUNT - 1].z = sLurePos.z;
            D_80B7E148 = 2.0f;
            break;
    }
}

s32 Fishing_SplashBySize(Fishing* this, PlayState* play, u8 ignorePosCheck) {
    s16 i;
    s16 count;
    f32 scale;
    Vec3f pos;
    Vec3f vel;
    f32 speedXZ;
    f32 angle;

    if ((this->actor.world.pos.y < (WATER_SURFACE_Y(play) - 10.0f)) && !ignorePosCheck) {
        return false;
    }

    if (this->fishLength >= 40.0f) {
        count = 40;
        scale = 1.2f;
    } else {
        count = 30;
        scale = 1.0f;
    }

    for (i = 0; i < count; i++) {
        speedXZ = (Rand_ZeroFloat(1.5f) + 0.5f) * scale;
        angle = Rand_ZeroFloat(6.28f);

        vel.x = sinf(angle) * speedXZ;
        vel.z = cosf(angle) * speedXZ;
        vel.y = (Rand_ZeroFloat(3.0f) + 3.0f) * scale;

        pos = this->actor.world.pos;
        pos.x += vel.x * 3.0f;
        pos.y = WATER_SURFACE_Y(play);
        pos.z += vel.z * 3.0f;

        Fishing_SpawnDustSplash(&this->actor.projectedPos, sFishingEffects, &pos, &vel,
                                (Rand_ZeroFloat(0.02f) + 0.025f) * scale);
    }

    pos = this->actor.world.pos;
    pos.y = WATER_SURFACE_Y(play);

    Fishing_SpawnRipple(&this->actor.projectedPos, sFishingEffects, &pos, 100.0f, 800.0f, 150, 90);

    this->lilyTimer = 30;

    return true;
}

void Fishing_SplashBySize2(Fishing* this, PlayState* play) {
    s16 count;
    s16 i;
    f32 scale;
    Vec3f pos;
    Vec3f vel;
    f32 speedXZ;
    f32 angle;

    if (this->fishLength >= 45.0f) {
        count = 30;
        scale = 0.5f;
    } else {
        count = 20;
        scale = 0.3f;
    }

    for (i = 0; i < count; i++) {
        speedXZ = (Rand_ZeroFloat(1.5f) + 0.5f) * scale;
        angle = Rand_ZeroFloat(6.28f);

        vel.x = sinf(angle) * speedXZ;
        vel.z = cosf(angle) * speedXZ;
        vel.y = Rand_ZeroFloat(2.0f) + 2.0f;

        pos = this->actor.world.pos;
        pos.x += (vel.x * 3.0f);
        pos.y += (vel.y * 3.0f);
        pos.z += (vel.z * 3.0f);

        Fishing_SpawnDustSplash(&this->actor.projectedPos, sFishingEffects, &pos, &vel,
                                (Rand_ZeroFloat(0.02f) + 0.025f) * scale);
    }
}

void func_80B70ED4(Fishing* this, Input* input) {
    Vec3f lineVec;
    Vec3f sp28;
    f32 lineLengthSQ;

    lineVec.x = sLurePos.x - this->actor.world.pos.x;
    lineVec.y = sLurePos.y - this->actor.world.pos.y;
    lineVec.z = sLurePos.z - this->actor.world.pos.z;

    lineLengthSQ = SQ(lineVec.x) + SQ(lineVec.y) + SQ(lineVec.z);

    if ((sRodCastState == 3) && (this->unk_1A2 == 0) && (sIsOwnersHatHooked == 0) && RF3_IsEnabled() &&
        RF3_LURE_IN_BITE_AREA()) {
        // Reel Fishing III mod (step 10): RF3 decides who notices the lure (RF3-MECHANICS 2.3).
        // OoT's range/button rules and its dawn/dusk frenzy shortcut are skipped.
        s16 toLureYaw = Math_Atan2S(lineVec.z, lineVec.x);
        f32 angleFromHeading = (s16)(toLureYaw - this->actor.shape.rot.y) * (M_PI / 32768.0f);

        if (RF3Approach_Notice(Fishing_RF3FishIndex(this), this->fishLength, sqrtf(lineLengthSQ),
                               sqrtf(SQ(lineVec.x) + SQ(lineVec.z)), angleFromHeading)) {
            this->fishState = 2;
            this->unk_15E = 0;
            this->timerArray[0] = 0;
            this->timerArray[2] = 30000; // RF3 decides when the chase ends
            this->perception = Fishing_RF3Perception(this);
            this->rotationStep = 0.0f;
        }
    } else if ((sRodCastState == 3) && (this->unk_1A2 == 0) && (sIsOwnersHatHooked == 0)) {
        Matrix_RotateY((-this->actor.shape.rot.y / 32768.0f) * M_PI, MTXMODE_NEW);
        Matrix_MultVec3f(&lineVec, &sp28);

        if ((sp28.z > 0.0f) || (this->fishLength < 40.0f)) {
            if ((this->fishState == 7) && (lineLengthSQ < SQ(200.0f))) {
                this->fishState = 4;
                this->fishTargetPos = sLurePos;
                this->rotationStep = 28672.0f;
                this->speedTarget = 5.0f;
            } else {
                if ((CHECK_BTN_ALL(input->cur.button, BTN_A) || (sLureWigglePosY > 1.0f)) &&
                    (lineLengthSQ < SQ(120.0f))) {
                    this->fishState = 2;
                    this->unk_15E = 0;
                    this->timerArray[0] = 0;
                    this->timerArray[2] = (s16)Rand_ZeroFloat(100.0f) + 100;
                    this->perception = Fishing_RF3Perception(this);
                    this->rotationStep = 0.0f;
                }

                if ((this->timerArray[1] == 0) && (lineLengthSQ < SQ(70.0f))) {
                    this->fishState = 2;
                    this->unk_15E = 0;
                    this->timerArray[0] = 0;
                    this->timerArray[2] = (s16)Rand_ZeroFloat(100.0f) + 100;
                    this->perception = Fishing_RF3Perception(this);
                    this->rotationStep = 0.0f;
                }
            }
        }
    } else if ((sRodCastState == 4) && sLineHooked && (lineLengthSQ < SQ(100.0f)) && (this->fishState >= 10)) {
        this->fishStateNext = 0;
        this->fishState = 1;
        this->unk_1A4 = 1000;
        this->unk_1A2 = 100;
        this->timerArray[1] = 50;
    }

    if ((sLureEquipped != FS_LURE_SINKING) && (D_80B7E114 != 0) && (this->fishLength > 60.0f) &&
        (lineLengthSQ < SQ(30.0f)) && (this->fishState >= 10)) {
        this->fishStateNext = 0;
        this->fishState = 1;
        this->unk_1A4 = 1000;
        this->unk_1A2 = 100;
        this->timerArray[1] = 50;
    }
}

void Fishing_FishLeapSfx(Fishing* this, u8 outOfWater) {
    s16 sfxId;
    u8 length;

    if (this->isLoach == 0) {
        length = this->fishLength;
    } else {
        length = 2.0f * this->fishLength;
    }

    if (outOfWater == false) {
        if (length >= 50) {
            sfxId = NA_SE_EV_DIVE_INTO_WATER;
        } else if (length >= 40) {
            sfxId = NA_SE_EV_BOMB_DROP_WATER;
        } else {
            sfxId = NA_SE_EV_BOMB_DROP_WATER;
        }
    } else {
        if (length >= 50) {
            sfxId = NA_SE_EV_JUMP_OUT_WATER;
        } else if (length >= 40) {
            sfxId = NA_SE_EV_OUT_OF_WATER;
        } else {
            sfxId = NA_SE_EV_OUT_OF_WATER;
        }
    }

    Audio_PlayActorSound2(&this->actor, sfxId);
}

void Fishing_HandleAquariumDialog(Fishing* this, PlayState* play) {
    if (sLinkAge == LINK_AGE_CHILD) {
        if ((HIGH_SCORE(HS_FISHING) & HS_FISH_LENGTH_CHILD) != 0) {
            if (HIGH_SCORE(HS_FISHING) & HS_FISH_CHEAT_CHILD) {
                this->actor.textId = 0x40B1;
            } else {
                this->actor.textId = 0x4089;
            }
        } else {
            this->actor.textId = 0x40AE;
        }
    } else {
        if ((HIGH_SCORE(HS_FISHING) & HS_FISH_LENGTH_ADULT) != 0) {
            if (HIGH_SCORE(HS_FISHING) & HS_FISH_CHEAT_ADULT) {
                this->actor.textId = 0x40B1;
            } else {
                this->actor.textId = 0x4089;
            }
        } else {
            this->actor.textId = 0x40AE;
        }
    }

    if (!this->isAquariumMessage) {
        if (this->aquariumWaitTimer == 0) {
            this->actor.flags |= ACTOR_FLAG_ATTENTION_ENABLED;

            if (Actor_ProcessTalkRequest(&this->actor, play)) {
                sFishLengthToWeigh = sFishingRecordLength;
                RF3Size_SetLengthToWeigh(sFishLengthToWeigh, RF3_WEIGH_RECORD, sLinkAge == LINK_AGE_CHILD); // steps 12b/14d: weigh-in shows RF3 length
                this->isAquariumMessage = true;
            } else {
                Actor_OfferTalkNearColChkInfoCylinder(&this->actor, play);
            }
        } else {
            this->aquariumWaitTimer--;
            this->actor.flags &= ~ACTOR_FLAG_ATTENTION_ENABLED;
        }
    } else if (Actor_TextboxIsClosing(&this->actor, play)) {
        this->isAquariumMessage = false;
        this->aquariumWaitTimer = 20;
    }
}

// Reel Fishing III mod (step 8): one frame of the RF3 fight for the hooked fish.
// Steers the fish to the fight's distance from Link, keeping its bearing, and turns
// OoT's line into a visual that follows the fish. Swim speed and depth per fight
// state are our visual choices, not Reel Fishing III data (docs/DESIGN.md, steps 8b-8c).
// Returns an RF3_OUTCOME_* code.
static f32 sRF3FishSpeed = 0.0f;
static f32 sRF3PrevTargetDist = -1.0f; // last frame's target distance, -1 = new fight
static f32 sRF3RodBend = 0.0f;         // smoothed rod bend angle from tension
static u8 sRF3FeedbackTimer = 0;
static f32 sRF3ReelFreq = 1.0f; // static: the audio code keeps this pointer for continuous sounds
static u8 sRF3HookPending = false; // step 9: RF3 set the hook; OoT's hook-set block applies its effects
static u8 sRF3WindowRumbled = false; // step 9b: the hook-set window rumble has started
static u8 sRF3SweepTimer = 0;   // step 8h: game frames until STRUGGLE's sweep changes side
static u8 sRF3SweepSide = 0;    // step 8h: STRUGGLE's current side of the line (0/1)
static s16 sRF3TurnScale = 0;   // step 8h: turn scale for Fishing_UpdateFish this frame, 0 = OoT's own
static s32 sRF3LastStruggleLogMode = 0; // step 25g-4: logs "rf3-struggle-sweep" once per entry into STRUGGLE

// Reset the steering and feedback state for a new strike or fight.
static void Fishing_RF3ResetVisuals(Fishing* this) {
    sRF3PrevTargetDist = -1.0f;
    sRF3FishSpeed = this->actor.speedXZ;
    sRF3RodBend = D_80B7A6BC;
    sRF3FeedbackTimer = 0;
    sRF3HookPending = false;
    sRF3WindowRumbled = false;
    sRF3SweepTimer = 0;
    sRF3TurnScale = 0;
}

static s32 Fishing_RF3Fight(Fishing* this, PlayState* play, Input* input, u8* hookedOut) {
    Player* player = GET_PLAYER(play);
    RF3FightInput rf3In;
    RF3FightOutput rf3Out;
    f32 dx = this->actor.world.pos.x - player->actor.world.pos.x;
    f32 dz = this->actor.world.pos.z - player->actor.world.pos.z;
    f32 dist = sqrtf(SQ(dx) + SQ(dz));
    f32 scale;
    f32 gap;
    f32 radialSpeed;
    f32 drift;
    f32 depthBelowSurface;
    f32 targetY;
    f32 minY;
    f32 maxY;
    f32 circleX;
    f32 circleZ;
    // Turn 0.2 rad around Link when circling.
    const f32 circleCos = 0.9801f;
    const f32 circleSin = 0.1987f;

    f32 load;
    f32 bendTarget;

    if (!RF3Fight_IsActive()) {
        Fishing_RF3ResetVisuals(this); // fallback start (option switched on mid-fight)
    }

    rf3In.reelHeld = CHECK_BTN_ALL(input->cur.button, BTN_A) ? 1 : 0;
    rf3In.stickX = input->rel.stick_x;
    rf3In.stickY = input->rel.stick_y;
    rf3In.fishDistUnits = dist;
    rf3In.lineMaxUnits = 1000.0f;            // OoT's full line: 200 segments of 5 units
    rf3In.catchDistUnits = 1.5f * RF3_UNITS_PER_METRE; // step 8i: caught at 1.5 m (user choice; OoT's was 50 units)
    if (sRF3CurRegion != NULL) {
        // Step 16f: Link may stand inland; the fish can't come closer than the shore, so it lands there.
        rf3In.catchDistUnits = MAX(rf3In.catchDistUnits, Fishing_RF3ShoreDistance(this, play));
    }
    rf3In.fishLength = this->fishLength;
    rf3In.fishIndex = Fishing_RF3FishIndex(this); // step 14b: the fish's species
    rf3In.fishY = this->actor.world.pos.y;
    rf3In.waterY = WATER_SURFACE_Y(play);
    rf3In.floorY = this->actor.floorHeight;
    rf3In.lureDepthUnits = WATER_SURFACE_Y(play) - sLurePos.y;
    rf3In.substeps = RF3_SUBSTEPS;
    RF3Fight_Update(&rf3In, &rf3Out);

    *hookedOut = rf3Out.hooked ? true : false;
    sRF3TurnScale = 0;
    if (!rf3Out.hooked) {
        // Strike phase (HIT): OoT keeps the fish at the lure; nothing to steer yet.
        // Step 9b: the only rumble during the strike marks the hook-set window, so the
        // player can feel when to set the hook. OoT's random fight rumble is held off.
        sRumbleDelay = 2;
        if (rf3Out.hookWindowOpen && !sRF3WindowRumbled) {
            // Full strength for the window's length, then a decrease of 255 stops it at once
            // (the rumble update holds for the duration, then subtracts the rate each frame).
            func_800A9F6C(0.0f, 220, (u8)CLAMP(rf3Out.hookWindowFrames, 1, 255), 255);
            sRF3WindowRumbled = true;
        }
        return rf3Out.outcome;
    }

    gap = fabsf(rf3Out.targetDistUnits - dist);

    // Step 25g-4: a region fish drawn with RF3's motions struggles as RF3 does (see the STRUGGLE target below).
    s32 rf3Motions = RF3_IS_REGION_FISH(this) && RF3FishModel_PlaysMotions(this->actor.params - EN_FISH_REGION);
    if ((rf3Out.mode == 9) && (sRF3LastStruggleLogMode != 9)) {
        RF3Rod_LogEvent("rf3-struggle-sweep", rf3Motions ? 0 : 1);
    }
    sRF3LastStruggleLogMode = rf3Out.mode;

    if (dist > 1.0f) {
        if (gap < 20.0f) {
            // Close enough: swim sideways around Link instead of back and forth.
            circleX = (dx * circleCos) - (dz * circleSin);
            circleZ = (dx * circleSin) + (dz * circleCos);
            this->fishTargetPos.x = player->actor.world.pos.x + circleX;
            this->fishTargetPos.z = player->actor.world.pos.z + circleZ;
        } else {
            scale = rf3Out.targetDistUnits / dist;
            this->fishTargetPos.x = player->actor.world.pos.x + (dx * scale);
            this->fishTargetPos.z = player->actor.world.pos.z + (dz * scale);
        }
        // Keep the target inside the pond, as OoT's own fight targets are.
        Fishing_RF3ClampFightTarget(&this->fishTargetPos); // step 16d: the pond's 750 radius, or the region's box
    }

    // Step 8g: movement that shows the fight state (our visual choice; the fight's numbers are
    // unchanged). AWAY heads straight away from Link, FOLLOW straight toward him, STRUGGLE
    // thrashes across the line, switching sides with the sim's 0.25 s shake.
    if ((dist > 1.0f) && ((rf3Out.mode == 9) || (rf3Out.mode == 10) || (rf3Out.mode == 11))) {
        f32 ux = dx / dist; // unit vector, Link -> fish
        f32 uz = dz / dist;
        f32 radial;
        f32 across = 0.0f;

        if (rf3Out.mode == 10) {
            radial = dist + 40.0f;
        } else if (rf3Out.mode == 11) {
            radial = MAX(dist - 40.0f, 0.0f);
        } else if (rf3Motions) {
            // Step 25g-4: a fish playing RF3's motions thrashes as RF3's scripts do (move_speed 0, course
            // straight): on the line at the fight distance, no sweep; RF3FishModel rolls it (bank).
            radial = (gap > 20.0f) ? rf3Out.targetDistUnits : dist;
        } else {
            // Step 8h: a wide sweep across the line, changing side every 0.5 s, so it reads
            // as a fight. 8g's �40 units every 0.25 s was too small and too fast to see.
            if (sRF3SweepTimer == 0) {
                sRF3SweepSide ^= 1;
                sRF3SweepTimer = 10;
            }
            sRF3SweepTimer--;
            radial = (gap > 20.0f) ? rf3Out.targetDistUnits : dist;
            across = sRF3SweepSide ? 80.0f : -80.0f;
        }
        this->fishTargetPos.x = player->actor.world.pos.x + (ux * radial) - (uz * across);
        this->fishTargetPos.z = player->actor.world.pos.z + (uz * radial) + (ux * across);
        Fishing_RF3ClampFightTarget(&this->fishTargetPos); // step 16d: the pond's 750 radius, or the region's box
    }

    // Depth and a slow drift per fight state.
    switch (rf3Out.mode) {
        case 9: // STRUGGLE: thrash near the surface
            drift = 0.6f;
            depthBelowSurface = 10.0f;
            break;
        case 10: // AWAY: run deep
            drift = 0.8f;
            depthBelowSurface = 50.0f;
            break;
        case 11: // FOLLOW
            drift = 0.6f;
            depthBelowSurface = 30.0f;
            break;
        case 7: // STOP
            drift = 0.2f;
            depthBelowSurface = -1.0f;
            break;
        default: // STIFFEN and anything else
            drift = 0.5f;
            depthBelowSurface = -1.0f;
            break;
    }

    // Depth target, kept between the lake floor (plus half the fish's thickness and a
    // margin) and just under the surface. actor.floorHeight is OoT's own floor probe.
    targetY = (depthBelowSurface >= 0.0f) ? (WATER_SURFACE_Y(play) - depthBelowSurface) : this->actor.world.pos.y;
    maxY = WATER_SURFACE_Y(play) - 5.0f;
    minY = (this->actor.floorHeight > BGCHECK_Y_MIN + 1.0f) ? (this->actor.floorHeight + (this->fishLength * 0.1f) + 10.0f)
                                                            : (maxY - 1000.0f);
    if (minY > maxY) {
        targetY = (minY + maxY) * 0.5f; // very shallow: stay midway
    } else {
        targetY = CLAMP(targetY, minY, maxY);
    }
    this->fishTargetPos.y = targetY;

    // Speed: only as fast as the fight's distance changes, plus a gap correction and a
    // slow drift. OoT's own state-5 code also pushes speedXZ earlier in the frame, so
    // keep our own smoothed value and assign it.
    radialSpeed = (sRF3PrevTargetDist >= 0.0f) ? fabsf(rf3Out.targetDistUnits - sRF3PrevTargetDist) : 0.0f;
    sRF3PrevTargetDist = rf3Out.targetDistUnits;
    if ((rf3Out.mode == 10) || (rf3Out.mode == 11)) {
        // Step 8g: AWAY/FOLLOW move only as fast as the fight's distance changes (FOLLOW: the
        // reel speed), slowly, plus a catch-up only in the direction the fish is heading.
        f32 behind = (rf3Out.mode == 10) ? (rf3Out.targetDistUnits - dist) : (dist - rf3Out.targetDistUnits);

        Math_ApproachF(&sRF3FishSpeed, CLAMP(radialSpeed + (MAX(behind, 0.0f) * 0.02f), 0.3f, 2.5f), 1.0f, 0.2f);
    } else if ((rf3Out.mode == 9) && !rf3Motions) {
        Math_ApproachF(&sRF3FishSpeed, 6.0f, 1.0f, 1.0f); // STRUGGLE: fast sweep (step 8h)
    } else {
        Math_ApproachF(&sRF3FishSpeed, CLAMP(radialSpeed + (gap * 0.03f) + drift, 0.3f, 4.0f), 1.0f, 0.1f);
    }
    this->actor.speedXZ = sRF3FishSpeed;

    // Step 8h: tail beat by state (OoT's procedural swim: unk_190 = beat speed, unk_194 =
    // amplitude), set after OoT's own state-5 values. STRUGGLE also turns fast so the sweep shows.
    if ((rf3Out.mode == 9) && !rf3Motions) {
        this->unk_190 = 1.6f;
        this->unk_194 = 6000.0f;
        this->rotationStep = 16384.0f;
        sRF3TurnScale = 3;
    } else if ((rf3Out.mode == 10) || (rf3Out.mode == 11)) {
        this->unk_190 = 0.6f;
        this->unk_194 = 1500.0f;
    }

    // Step 26e-3: a region fish on RF3's model and motions swims RF3's way (RF3FishMove: the fight target on the circle
    // around Link, RF3's turning and speed). It replaces the target, speed and turn set above; OoT's mover then turns
    // the fish straight to the point ahead and moves it. Pond fish keep the paths above.
    if (rf3Motions) {
        RF3FishMoveInput mi;
        RF3FishMoveOutput mo;
        RF3RegionFishInfo regionInfo;
        s32 slot = this->actor.params - EN_FISH_REGION;

        if (RF3RegionFish_Get(slot, &regionInfo)) {
            mi.slot = slot;
            mi.record = regionInfo.record;
            mi.mm = regionInfo.mm;
            mi.fishPos[0] = this->actor.world.pos.x;
            mi.fishPos[1] = this->actor.world.pos.y;
            mi.fishPos[2] = this->actor.world.pos.z;
            mi.yaw = this->actor.world.rot.y;
            mi.anglerPos[0] = player->actor.world.pos.x;
            mi.anglerPos[1] = player->actor.world.pos.y;
            mi.anglerPos[2] = player->actor.world.pos.z;
            mi.waterY = WATER_SURFACE_Y(play);
            mi.floorY = this->actor.floorHeight;
            mi.mode = rf3Out.mode;
            mi.distUnits = rf3Out.targetDistUnits;
            mi.substeps = RF3_SUBSTEPS;
            mi.frame = play->gameplayFrames;
            mi.inWater = Fishing_RF3PointInWater;
            mi.probe = Fishing_RF3Probe; // step 26f-1
            // Step 30a-2: RF3's fight centre -> the region box's centre (the box turns about it) [assumed stand-in]
            mi.centerPos[0] = (regionInfo.minX + regionInfo.maxX) * 0.5f;
            mi.centerPos[1] = regionInfo.surfaceY;
            mi.centerPos[2] = (regionInfo.minZ + regionInfo.maxZ) * 0.5f;
            mi.hasCenter = 1;
            {
                // Step 30c-1: the fight's inputs, for AWAY's run
                RF3FightStatus fs;
                RF3Fight_GetStatus(&fs);
                mi.reelHeld = fs.reelHeld;
                mi.stickY = fs.stickY;
            }
            sRF3MovePlay = play;
            sRF3MoveFish = this; // step 26f-2
            if (RF3FishMove_Step(&mi, &mo)) {
                sRF3MovedFish = this; // step 30b-1d: OoT's line leash leaves this fish alone next frame
                sRF3MovedFrame = play->gameplayFrames;
                this->fishTargetPos.x = mo.target[0];
                this->fishTargetPos.y = mo.target[1];
                this->fishTargetPos.z = mo.target[2];
                sRF3FishSpeed = mo.speed;
                this->actor.speedXZ = mo.speed;
                // Step 30b-2b: the line's drag while it turns on the spot (before OoT's move and wall check)
                this->actor.world.pos.x += mo.drag[0];
                this->actor.world.pos.z += mo.drag[2];
                this->rotationStep = 16384.0f; // RF3 sets the heading; OoT's turn just follows it
                sRF3TurnScale = 1;
            }
        }
    }

    // Tension feedback, Reel Fishing III style (step 8d; our tuning, not RF3 data).
    load = (rf3Out.tensionLimit > 0.0f) ? CLAMP(rf3Out.tension / rf3Out.tensionLimit, 0.0f, 1.0f) : 0.0f;
    sRF3FeedbackTimer++;
    // Rod bend: slight when slack, deep near the limit, trembling above 85%.
    bendTarget = -0.25f - (0.85f * load);
    if (load > 0.85f) {
        bendTarget += Math_SinS(sRF3FeedbackTimer * 0x3000) * 0.06f * ((load - 0.85f) / 0.15f);
    }
    Math_ApproachF(&sRF3RodBend, bendTarget, 0.5f, 0.08f);
    D_80B7A6BC = sRF3RodBend;
    // Rumble: OoT's random fight rumble is held off; pulse harder as tension rises past 40%.
    sRumbleDelay = 2;
    if ((load > 0.4f) && ((sRF3FeedbackTimer % 10) == 0)) {
        func_800A9F6C(0.0f, (u8)(60.0f + (195.0f * (load - 0.4f) / 0.6f)), 10, 5);
    }
    // Warning: above 80% the reel screams, higher-pitched toward the limit.
    if (load > 0.8f) {
        sRF3ReelFreq = 0.8f + ((load - 0.8f) * 3.0f);
        Audio_PlaySfxGeneral(NA_SE_IT_FISHING_REEL_HIGH - SFX_FLAG, &gSfxDefaultPos, 4, &sRF3ReelFreq,
                             &gSfxDefaultFreqAndVolScale, &gSfxDefaultReverb);
    }

    // OoT's reel no longer pulls the fish in; refit the line to the fish every frame.
    sRodReelingSpeed = 0.0f;
    D_80B7A6A4 = 5;
    return rf3Out.outcome;
}

static void Fishing_UpdateFishImpl(Actor* thisx, PlayState* play2) {
    s16 i;
    s16 rotXYScale = 10;
    f32 distX;
    f32 distY;
    f32 distZ;
    f32 distToTarget;
    f32 multiplier;
    f32 chance;
    f32 playerSpeedMod;
    Vec3f multiVecSrc;
    Vec3f targetPosOffset;
    s16 rotXTarget;
    s16 rotYTarget;
    s16 rotZScale;
    s16 timer;
    s16 spF6;
    s16 rotXScale;
    s16 rotXStep;
    s16 rotYScale;
    s16 rotYStep;
    Fishing* this = (Fishing*)thisx;
    PlayState* play = play2;
    Player* player = GET_PLAYER(play);
    Input* input = &play->state.input[0];
    f32 spD8;
    f32 phi_f0;
    f32 phi_f2;
    Vec3f bubblePos;
    Vec3f spB8;
    u8 phi_v0_2;
    f32 temp_f0;
    f32 temp;
    s32 pad;
    f32 rumbleStrength;
    u16 attempts;
    u8 rumbleStrength8;
    s32 rf3Outcome;
    u8 rf3Fight;
    u8 rf3Hooked;
    s32 rf3Approach;
    s32 rf3Reached; // step 29h: the chase's "reached the bait" input

    this->actor.uncullZoneForward = 700.0f;
    this->actor.uncullZoneScale = 50.0f;

    if (this->isLoach == 0) {
        playerSpeedMod = (player->actor.speedXZ * 0.15f) + 0.25f;
    } else {
        playerSpeedMod = (player->actor.speedXZ * 0.3f) + 0.25f;
    }

    if ((D_80B7E0B0 != 0) || (sSubCamId != 0) || ((player->actor.world.pos.z > 1150.0f) && (this->fishState != 100))) {
        this->actor.flags &= ~ACTOR_FLAG_ATTENTION_ENABLED;
    } else {
        this->actor.flags |= ACTOR_FLAG_ATTENTION_ENABLED;
        if (sRodCastState != 0) {
            if (D_80B7E0B2 == 0) {
                this->actor.focus.pos = sLurePos;
            } else if (D_80B7E0B2 == 1) {
                sFishingPlayerCinematicState = 1;
                D_80B7FED0 = 0.0f;
                sLureCameraZoomLevel = 2;
            }
        }
        this->actor.focus.pos = this->actor.world.pos;
    }

    this->stateAndTimer++;

    for (i = 0; i < 4; i++) {
        if (this->timerArray[i] != 0) {
            this->timerArray[i]--;
        }
    }

    if (this->unk_1A4 != 0) {
        this->unk_1A4--;
    }

    if (this->unk_1A2 != 0) {
        this->unk_1A2--;
    }

    if (this->bumpTimer != 0) {
        this->bumpTimer--;
    }

    if (this->lilyTimer != 0) {
        this->lilyTimer--;
    }

    Math_ApproachF(&this->fishLimbRotPhaseStep, this->unk_190, 1.0f, 0.2f);

    if (this->fishState == 6) {
        Math_ApproachF(&this->fishLimbRotPhaseMag, this->unk_194, 0.2f, 200.0f);
    } else {
        phi_f0 = 1.0f;
        phi_f2 = 1.0f;
        if (this->actor.world.pos.y > WATER_SURFACE_Y(play)) {
            phi_f0 = (KREG(64) * 0.1f) + 1.5f;
            phi_f2 = 3.0f;
        }
        Math_ApproachF(&this->fishLimbRotPhaseMag, this->unk_194 * phi_f0, 1.0f, 500.0f * phi_f2);
    }

    Math_ApproachS(&this->fishLimbDRotZDelta, 0, 5, 0x1F4);

    if (this->isLoach == 0) {
        Actor_SetScale(&this->actor, this->fishLength * 15.0f * 0.00001f);

        this->fishLimbRotPhase += this->fishLimbRotPhaseStep;

        temp = cosf(this->fishLimbRotPhase);
        this->fishLimb23RotYDelta = this->unk_16E + (s16)(temp * this->fishLimbRotPhaseMag);

        temp = cosf(this->fishLimbRotPhase + -1.2f);
        this->fishLimb4RotYDelta = this->unk_16E + (s16)(temp * this->fishLimbRotPhaseMag * 1.6f);
    } else {
        Actor_SetScale(&this->actor, this->fishLength * 65.0f * 0.000001f);

        this->actor.scale.x = this->actor.scale.z * 1.1f;
        this->actor.scale.y = this->actor.scale.z * 1.1f;

        this->fishLimbRotPhase += this->fishLimbRotPhaseStep * 0.8f;

        for (i = 0; i < 3; i++) {
            temp = cosf(this->fishLimbRotPhase + (i * 2.1f));
            this->loachRotYDelta[i] = this->unk_16E + (s16)(temp * this->fishLimbRotPhaseMag * 2.0f);
        }

        temp = cosf(this->fishLimbRotPhase + 0.4f);
        this->fishLimb23RotYDelta = (this->fishLimbRotPhaseMag * temp * 2.0f) * 0.6f;
    }

    distX = this->fishTargetPos.x - this->actor.world.pos.x;
    distY = this->fishTargetPos.y - this->actor.world.pos.y;
    distZ = this->fishTargetPos.z - this->actor.world.pos.z;

    rotYTarget = Math_Atan2S(distZ, distX);
    distToTarget = sqrtf(SQ(distX) + SQ(distZ));

    rotXTarget = Math_Atan2S(distToTarget, distY);
    distToTarget = sqrtf(SQ(distX) + SQ(distZ) + SQ(distY));

    // Step 26e-10 (user, 2026-10-09: "it should slide along the wall"): a hooked region fish on RF3's movement skips
    // OoT's +-90 deg wall-bump turn, so it keeps heading for RF3's target and OoT's wall push slides it along the wall.
    // With the turn, it sat still sideways for the 20 bump frames, then jumped (rf3_fishing_20261009_203143.csv).
    if ((this->bumpTimer != 0) && (this->fishState != 2) && (this->fishState != 3) && (this->fishState != 4) &&
        !((this->fishState == 5) && RF3_IS_REGION_FISH(this) &&
          RF3FishModel_PlaysMotions(this->actor.params - EN_FISH_REGION))) {
        if ((this->stateAndTimer & 0x40) != 0) {
            rotYTarget += 0x4000;
        } else {
            rotYTarget -= 0x4000;
        }
        if (((this->stateAndTimer + 0x20) & 0x40) != 0) {
            rotXTarget += 0x2000;
        } else {
            rotXTarget -= 0x2000;
        }
    }

    switch (this->fishState) {
        case 100:
            Fishing_HandleAquariumDialog(this, play);

            this->actor.uncullZoneForward = 500.0f;
            this->actor.uncullZoneScale = 300.0f;

            Lights_PointNoGlowSetInfo(&this->lightInfo, (s16)this->actor.world.pos.x,
                                      (s16)this->actor.world.pos.y + 20.0f, (s16)this->actor.world.pos.z - 50.0f, 255,
                                      255, 255, 255);

            this->fishLength = sFishingRecordLength;
            targetPosOffset.y = (f32)Math_SinS(play->gameplayFrames * 300) * 1;
            targetPosOffset.z = (f32)Math_SinS(play->gameplayFrames * 230) * 2;
            this->actor.world.pos.x = 130.0f;
            this->actor.world.pos.y = 55.0f + targetPosOffset.y;
            this->actor.world.pos.z = 1300.0f + targetPosOffset.z;
            this->actor.shape.rot.y = -0x8000;

            if ((this->actor.projectedPos.z < 200.0f) && (this->actor.projectedPos.z > 0.0f)) {
                bubblePos.x = Rand_CenteredFloat(5.0f) + 130.0f;
                bubblePos.y = 40.0f;
                bubblePos.z = Rand_CenteredFloat(5.0f) + 1280.0f;
                Fishing_SpawnBubble(NULL, sFishingEffects, &bubblePos, Rand_ZeroFloat(0.02f) + 0.03f, 1);
            }

            Math_ApproachS(&this->fishLimbEFRotYDelta, (Math_SinS(this->stateAndTimer * 0x800) * 2500.0f) + 2500.0f, 2,
                           0x7D0);
            Math_ApproachS(&this->fishLimb89RotYDelta, Math_SinS(this->stateAndTimer * 0xA00) * 1500.0f, 2, 0x7D0);

            this->unk_190 = 0.3f;
            this->unk_194 = 1000.0f / 3.0f;
            return;

        case 10:
            this->fishTargetPos = this->actor.home.pos;

            Math_ApproachF(&this->actor.speedXZ, 2.0f, 1.0f, 0.5f);
            Math_ApproachF(&this->rotationStep, 4096.0f, 1.0f, 256.0f);

            if (distToTarget < 40.0f) {
                this->fishState = 11;
                this->unk_190 = 0.4f;
                this->unk_194 = 500.0f;
            }

            func_80B70ED4(this, input);

            if (this->actor.xzDistToPlayer < (250.0f * playerSpeedMod)) {
                this->fishStateNext = this->fishState = 0;
                this->unk_1A4 = 1000;
                this->unk_1A2 = 200;
                this->timerArray[1] = 50;
            }
            break;

        case 11:
            this->fishTargetPos = this->actor.home.pos;

            Math_ApproachF(&this->actor.speedXZ, 0.0f, 1.0f, 0.05f);
            Math_ApproachF(&this->rotationStep, 0.0f, 1.0f, 256.0f);

            if (distToTarget >= 40.0f) {
                this->fishState = 10;
                this->unk_190 = 1.0f;
                this->unk_194 = 2000.0f;
            }
            func_80B70ED4(this, input);

            if (this->actor.xzDistToPlayer < (250.0f * playerSpeedMod)) {
                this->fishStateNext = this->fishState = 0;
                this->unk_1A4 = 1000;
                this->unk_1A2 = 200;
                this->timerArray[1] = 50;
            }

            // Step 16d: the dawn/dusk group swim follows the pond's group fish; not for region fish.
            if ((Message_GetState(&play->msgCtx) == TEXT_STATE_NONE) && !RF3_IS_REGION_FISH(this)) {
                if ((gSaveContext.dayTime >= 0xC000) && (gSaveContext.dayTime <= 0xC01B)) {
                    this->fishState = 7;
                    this->timerArray[3] = (s16)Rand_ZeroFloat(150.0f) + 200;
                }
                if ((gSaveContext.dayTime >= 0x3AAA) && (gSaveContext.dayTime <= 0x3AC5)) {
                    this->fishState = 7;
                    this->timerArray[3] = (s16)Rand_ZeroFloat(150.0f) + 200;
                }
            }

            if (KREG(15) != 0) {
                KREG(15) = 0;
                this->fishState = 7;
                this->timerArray[3] = (s16)Rand_ZeroFloat(150.0f) + 2000;
            }
            break;

        case 0:
            Math_ApproachF(&this->actor.speedXZ, 1.0f, 1.0f, 0.05f);
            Math_ApproachF(&this->rotationStep, 0.0f, 1.0f, 256.0f);

            if (this->timerArray[0] == 0) {
                if (this->unk_1A4 == 0) {
                    this->fishState = this->fishStateNext = 10;
                } else {
                    this->fishState = 1;
                    this->timerArray[0] = (s16)Rand_ZeroFloat(30.0f) + 10;
                    this->fishTargetPos.x = Rand_CenteredFloat(300.0f);
                    this->fishTargetPos.y = (WATER_SURFACE_Y(play) - 50.0f) - Rand_ZeroFloat(50.0f);
                    this->fishTargetPos.z = Rand_CenteredFloat(300.0f);
                    Fishing_RF3RegionTarget(this); // step 16d: a region fish wanders inside its box
                    this->unk_190 = 1.0f;
                    this->unk_194 = 2000.0f;
                }
            }

            if (sLureEquipped == FS_LURE_SINKING) {
                func_80B70ED4(this, input);
            } else {
                this->actor.flags &= ~ACTOR_FLAG_ATTENTION_ENABLED;
            }
            break;

        case 1:
            if (this->isLoach == 1) {
                this->fishState = -1;
                this->unk_1A4 = 20000;
                this->unk_1A2 = 20000;
                this->fishTargetPos.x = 0.0f;
                this->fishTargetPos.y = -140.0f;
                this->fishTargetPos.z = 0.0f;
            } else {
                Math_ApproachF(&this->rotationStep, 4096.0f, 1.0f, 256.0f);

                if ((this->actor.xzDistToPlayer < (250.0f * playerSpeedMod)) || (this->timerArray[1] != 0)) {
                    Math_ApproachF(&this->rotationStep, 8192.0f, 1.0f, 768.0f);
                    Math_ApproachF(&this->actor.speedXZ, 4.2f, 1.0f, 0.75);
                    this->unk_190 = 1.2f;
                    this->unk_194 = 4000.0f;
                    this->timerArray[0] = 20;
                } else {
                    this->unk_190 = 1.0f;
                    this->unk_194 = 2000.0f;
                    Math_ApproachF(&this->actor.speedXZ, 1.5f, 1.0f, 0.1f);
                }

                if ((this->timerArray[0] == 0) || (distToTarget < 50.0f)) {
                    this->fishState = 0;
                    this->timerArray[0] = (s16)Rand_ZeroFloat(30.0f) + 3;
                    this->unk_190 = 1.0f;
                    this->unk_194 = 500.0f;
                }

                if (sLureEquipped == FS_LURE_SINKING) {
                    func_80B70ED4(this, input);
                } else {
                    this->actor.flags &= ~ACTOR_FLAG_ATTENTION_ENABLED;
                }
            }
            break;

        case -1: // loach state 1
            Math_ApproachS(&this->rotationTarget.x, 0, 0x14, 0x20);

            if ((this->actor.xzDistToPlayer < (250.0f * playerSpeedMod)) || (this->timerArray[1] != 0)) {
                Math_ApproachF(&this->actor.speedXZ, 3.0f, 1.0f, 0.75);
                this->unk_190 = 1.0f;
                this->timerArray[0] = 20;
                this->unk_194 = 4000.0f;
                Math_ApproachF(&this->rotationStep, 4096.0f, 1.0f, 256.0f);

                if ((play->gameplayFrames % 32) == 0) {
                    this->fishTargetPos.x = Rand_CenteredFloat(600.0f);
                    this->fishTargetPos.z = Rand_CenteredFloat(600.0f);
                    this->fishTargetPos.y = -120.0f;
                }
            } else if (distToTarget > 50.0f) {
                this->unk_190 = 0.8f;
                this->unk_194 = 1500.0f;
                Math_ApproachF(&this->actor.speedXZ, 1.0f, 1.0f, 0.1f);
                Math_ApproachF(&this->rotationStep, 2048.0f, 1.0f, 128.0f);
            } else {
                this->unk_190 = 0.4f;
                this->unk_194 = 500.0f;
                Math_ApproachZeroF(&this->actor.speedXZ, 1.0f, 0.02f);
                Math_ApproachF(&this->rotationStep, 0.0f, 1.0f, 256.0f);
            }

            if (this->unk_1A4 == 0) {
                this->fishState = 10;
                this->fishStateNext = 10;
            } else if ((KREG(2) != 0) || (((this->unk_1A4 & 0x7FF) == 0) && (this->unk_1A4 < 15000))) {
                KREG(2) = 0;
                this->fishState = -2;
                this->actor.world.rot.x = this->actor.shape.rot.x = 0;
                this->fishTargetPos.y = WATER_SURFACE_Y(play) + 10.0f;
                this->fishTargetPos.x = Rand_ZeroFloat(50.0f);
                this->fishTargetPos.z = Rand_ZeroFloat(50.0f);
            }

            this->actor.flags &= ~ACTOR_FLAG_ATTENTION_ENABLED;
            break;

        case -2:
            if ((this->actor.xzDistToPlayer < (250.0f * playerSpeedMod)) || (this->timerArray[1] != 0)) {
                this->fishState = -1;
                this->fishTargetPos.y = -120.0f;
            } else {
                this->unk_190 = 0.6f;
                this->unk_194 = 1000.0f;

                Math_ApproachS(&this->rotationTarget.x, -0x1000, 0x14, 0x100);

                if (this->actor.world.pos.y < (WATER_SURFACE_Y(play) - 20.0f)) {
                    Math_ApproachF(&this->actor.speedXZ, 0.5f, 1.0f, 0.1f);
                } else {
                    Math_ApproachZeroF(&this->actor.speedXZ, 1.0f, 0.01f);

                    if ((this->actor.speedXZ == 0.0f) || (this->actor.world.pos.y > (WATER_SURFACE_Y(play) - 5.0f))) {
                        this->fishTargetPos.x = Rand_ZeroFloat(300.0f);
                        this->fishTargetPos.z = Rand_ZeroFloat(300.0f);
                        this->fishTargetPos.y = this->actor.floorHeight + 10.0f;
                        this->fishState = -25;
                        this->rotationStep = 0.0f;

                        spB8 = this->fishMouthPos;
                        spB8.y = WATER_SURFACE_Y(play);
                        Fishing_SpawnRipple(&this->actor.projectedPos, sFishingEffects, &spB8, 10.0f, 300.0f, 150,
                                            90);
                        Fishing_SpawnRipple(&this->actor.projectedPos, sFishingEffects, &spB8, 30.0f, 400.0f, 150,
                                            90);

                        Audio_PlayActorSound2(&this->actor, NA_SE_PL_CATCH_BOOMERANG);
                        break;
                    }
                }

                Math_ApproachF(&this->rotationStep, 2048.0f, 1.0f, 128.0f);
                this->actor.flags &= ~ACTOR_FLAG_ATTENTION_ENABLED;
            }
            break;

        case -25:
            if ((this->actor.xzDistToPlayer < (250.0f * playerSpeedMod)) || (this->timerArray[1] != 0)) {
                this->fishState = -1;
                this->fishTargetPos.y = -120.0f;
            } else {
                Math_ApproachS(&this->rotationTarget.x, 0x1000, 0x14, 0x6A);

                if (distToTarget > 40.0f) {
                    this->unk_190 = 0.7f;
                    this->unk_194 = 1200.0f;
                    Math_ApproachF(&this->actor.speedXZ, 0.5f, 1.0f, 0.01f);
                    Math_ApproachF(&this->rotationStep, 2048.0f, 1.0f, 128.0f);
                } else {
                    this->fishState = -1;
                }
            }
            break;

        case 2:
            // Step 29h: an RF3 region fish moves as RF3's CHASE/WAIT (straight to the bait, then backs off while
            // inspecting it) instead of OoT's circling; OoT's target, turn and speed below are skipped for it.
            rf3Reached = distToTarget < ((this->fishLength * 0.5f) + 20.0f);
            if (RF3_IsEnabled() && RF3_IS_REGION_FISH(this)) {
                rf3Reached = Fishing_RF3ApproachMove(this, play);
            } else {
                if (((this->actor.params + D_80B7E118) & 1) != 0) {
                    multiVecSrc.x = 10.0f;
                } else {
                    multiVecSrc.x = -10.0f;
                }
                multiVecSrc.y = 0.0f;
                multiVecSrc.z = 0.0f;
                Matrix_RotateY(sLureRot.y, MTXMODE_NEW);
                Matrix_MultVec3f(&multiVecSrc, &targetPosOffset);

                this->fishTargetPos.x = sLurePos.x + targetPosOffset.x;
                this->fishTargetPos.z = sLurePos.z + targetPosOffset.z;

                if (sLureEquipped == FS_LURE_SINKING) {
                    this->fishTargetPos.y = sLurePos.y;
                } else if (this->isLoach == 0) {
                    this->fishTargetPos.y = sLurePos.y - 15.0f;
                } else {
                    this->fishTargetPos.y = sLurePos.y - 5.0f;
                }

                if (this->fishTargetPos.y <= this->actor.floorHeight) {
                    this->fishTargetPos.y = this->actor.floorHeight + 3.0f;
                }

                if ((sLureEquipped != FS_LURE_SINKING) && (this->fishTargetPos.y < this->actor.world.pos.y)) {
                    Math_ApproachF(&this->actor.world.pos.y, this->fishTargetPos.y, 0.1f,
                                   (this->actor.world.pos.y - this->fishTargetPos.y) * 0.1f);
                }

                Math_ApproachF(&this->rotationStep, 8192.0f, 1.0f, (KREG(16) * 128) + 384.0f);
            }
            if (CHECK_BTN_ALL(input->press.button, BTN_A)) {
                this->perception += 0.005f;
            }

            if (sWiggleAttraction != 0) {
                if (sWiggleAttraction == 1) {
                    this->perception += 0.01f;
                } else {
                    this->perception += 0.05f;
                }
                sWiggleAttraction = 0;
            }

            if (CHECK_BTN_ALL(input->press.button, sRF3RodBtn)) {
                this->perception += 0.008f;
            }

            if (RF3_IsEnabled() && RF3_IS_REGION_FISH(this)) {
                // step 29h: the speed is set by Fishing_RF3ApproachMove
            } else if (distToTarget < ((this->fishLength * 0.5f) + 20.0f)) {
                if (this->unk_15E == 0) {
                    this->unk_190 = 1.0f;
                    this->unk_194 = 500.0f;
                    this->timerArray[0] = (s16)Rand_ZeroFloat(10.0f) + 2;
                }
                Math_ApproachF(&this->actor.speedXZ, -0.2f, 1.0f, 0.1f);
                this->unk_15E = 1;
            } else {
                if (this->unk_15E != 0) {
                    this->unk_190 = 1.0f;
                    this->rotationStep = 0.0f;
                    this->unk_194 = 3000.0f;
                }
                Math_ApproachF(&this->actor.speedXZ, 3.0f, 1.0f, 0.15f);
                this->unk_15E = 0;
            }

            if (RF3_IsEnabled()) {
                // Reel Fishing III mod (step 10): RF3's chase decides; OoT's perception and bite
                // chance are skipped. "Reached" is OoT's own close-to-the-lure test above.
                // A lure outside OoT's biting area counts as out of play (step 10b).
                rf3Approach = RF3Approach_Chase(
                    Fishing_RF3FishIndex(this), (sRodCastState == 3) && RF3_LURE_IN_BITE_AREA(),
                    (SQ(sLurePos.x - player->actor.world.pos.x) + SQ(sLurePos.z - player->actor.world.pos.z)) <
                        SQ(KREG(59) + 50.0f),
                    rf3Reached, RF3_SUBSTEPS);
                if (rf3Approach == RF3_APPROACH_STRIKE) {
                    // OoT's own strike setup: line up (state 3, or -3 for a loach), then lunge.
                    if (this->isLoach == 0) {
                        this->fishState = 3;
                        this->unk_190 = 1.2f;
                        this->unk_194 = 5000.0f;
                        this->timerArray[0] = Rand_ZeroFloat(10.0f);
                    } else {
                        this->fishState = -3;
                        this->unk_190 = 1.0f;
                        this->unk_194 = 3000.0f;
                        this->timerArray[0] = 40;
                    }
                    if (sLureEquipped == FS_LURE_SINKING) {
                        this->speedTarget = Rand_ZeroFloat(1.5f) + 3.0f;
                    } else {
                        this->speedTarget = Rand_ZeroFloat(1.5f) + 4.5f;
                    }
                } else if (rf3Approach == RF3_APPROACH_FAIL) {
                    // Lost interest: OoT's own end-of-follow (the fish swims off).
                    this->fishState = this->fishStateNext;
                    this->timerArray[1] = (s16)Rand_ZeroFloat(30.0f) + 50;
                    this->timerArray[0] = (s16)Rand_ZeroFloat(10.0f) + 5;
                    this->unk_190 = 1.0f;
                    this->rotationStep = 0.0f;
                    this->unk_194 = 2000.0f;
                }
                if ((this->fishState == 2) &&
                    Fishing_RF3OutOfArea(this->actor.world.pos.x, this->actor.world.pos.z)) {
                    // OoT's own exit: the fish strayed too far from the pond centre.
                    RF3Approach_EndChase(Fishing_RF3FishIndex(this));
                    this->fishState = this->fishStateNext;
                    this->timerArray[1] = (s16)Rand_ZeroFloat(30.0f) + 50;
                    this->timerArray[0] = (s16)Rand_ZeroFloat(10.0f) + 5;
                    this->unk_190 = 1.0f;
                    this->rotationStep = 0.0f;
                    this->unk_194 = 2000.0f;
                }
                if (this->actor.xzDistToPlayer < (100.0f * playerSpeedMod)) {
                    // OoT's own exit: Link came too close and scared the fish.
                    RF3Approach_EndChase(Fishing_RF3FishIndex(this));
                    this->fishStateNext = this->fishState = 0;
                    this->unk_1A4 = 1000;
                    this->unk_1A2 = 200;
                    this->timerArray[1] = 50;
                }
                break;
            }

            if (this->fishLength >= 60.0f) {
                multiplier = 0.3f;
            } else if (this->fishLength >= 45.0f) {
                multiplier = 0.6f;
            } else {
                multiplier = 1.0f;
            }

            if ((gSaveContext.dayTime >= 0xB555) && (gSaveContext.dayTime <= 0xCAAA)) {
                multiplier *= 1.75f;
            } else if ((gSaveContext.dayTime >= 0x3555) && (gSaveContext.dayTime <= 0x4AAA)) {
                multiplier *= 1.5f;
            } else if (sFishingFoggy != 0) {
                multiplier *= 1.5f;
            } else if ((u8)sStormStrength != 0) {
                multiplier *= 3.0f;
            }

            chance = 0.03f * multiplier;
            if (sLureEquipped == FS_LURE_SINKING) {
                chance *= 5.0f;
            }
            if (GameInteractor_Should(VB_FISHING_FISH_BITE, ((this->timerArray[0] == 1) || (Rand_ZeroOne() < chance)) &&
                                                                ((Rand_ZeroOne() < (this->perception * multiplier)) ||
                                                                 ((this->isLoach + 1) == KREG(69))))) {
                if (this->isLoach == 0) {
                    this->fishState = 3;
                    this->unk_190 = 1.2f;
                    this->unk_194 = 5000.0f;
                    this->timerArray[0] = Rand_ZeroFloat(10.0f);
                } else {
                    this->fishState = -3;
                    this->unk_190 = 1.0f;
                    this->unk_194 = 3000.0f;
                    this->timerArray[0] = 40;
                }
                if (sLureEquipped == FS_LURE_SINKING) {
                    this->speedTarget = Rand_ZeroFloat(1.5f) + 3.0f;
                } else {
                    this->speedTarget = Rand_ZeroFloat(1.5f) + 4.5f;
                }
            }

            if ((sRodCastState != 3) || (this->timerArray[2] == 0) ||
                Fishing_RF3OutOfArea(this->actor.world.pos.x, this->actor.world.pos.z)) {
                this->fishState = this->fishStateNext;
                this->timerArray[1] = (s16)Rand_ZeroFloat(30.0f) + 50;
                this->timerArray[0] = (s16)Rand_ZeroFloat(10.0f) + 5;
                this->unk_190 = 1.0f;
                this->rotationStep = 0.0f;
                this->unk_194 = 2000.0f;
            }

            if (this->actor.xzDistToPlayer < (100.0f * playerSpeedMod)) {
                this->fishStateNext = this->fishState = 0;
                this->unk_1A4 = 1000;
                this->unk_1A2 = 200;
                this->timerArray[1] = 50;
            }
            break;

        case 3:
            if (RF3_IsEnabled()) {
                // Step 10: still the chaser while lining up; released after the bite.
                RF3Approach_Chase(Fishing_RF3FishIndex(this), sRodCastState == 3, false, false, RF3_SUBSTEPS);
            }
            this->lilyTimer = 6;
            rotXYScale = 2;

            if ((((s16)player->actor.world.pos.x + D_80B7E118) & 1) != 0) {
                multiVecSrc.x = 30.0f;
            } else {
                multiVecSrc.x = -30.0f;
            }
            multiVecSrc.y = 0.0f;
            multiVecSrc.z = 30.0f;

            Matrix_RotateY(sLureRot.y, MTXMODE_NEW);
            Matrix_MultVec3f(&multiVecSrc, &targetPosOffset);

            this->fishTargetPos.x = sLurePos.x + targetPosOffset.x;
            this->fishTargetPos.z = sLurePos.z + targetPosOffset.z;
            this->fishTargetPos.y = sLurePos.y - 10.0f;
            this->rotationStep = 4096.0f;
            Math_ApproachF(&this->actor.speedXZ, this->speedTarget * 0.8f, 1.0f, 1.0f);

            if ((sRodCastState != 3) || (sLurePos.y > (WATER_SURFACE_Y(play) + 5.0f)) ||
                Fishing_RF3OutOfArea(sLurePos.x, sLurePos.z)) {
                this->fishState = this->fishStateNext;
                this->timerArray[0] = 0;
                this->unk_190 = 1.0f;
                this->unk_194 = 2000.0f;
            } else if ((this->timerArray[0] == 0) || (distToTarget < 30.0f)) {
                this->fishState = 4;
                this->fishTargetPos = sLurePos;
                this->rotationStep = 16384.0f;
                this->unk_190 = 1.2f;
                this->unk_194 = 5000.0f;
                // Step 10b: an RF3 strike means the fish takes the lure, so give the lunge 3 s
                // (60 game frames) to catch a lure that is still being reeled; OoT allows 1 s.
                this->timerArray[0] = RF3_IsEnabled() ? 60 : 20;
            }
            break;

        case 4:
            if (RF3_IsEnabled()) {
                // Step 10: still the chaser while lunging; released after the bite.
                RF3Approach_Chase(Fishing_RF3FishIndex(this), sRodCastState == 3, false, false, RF3_SUBSTEPS);
            }
            Math_ApproachF(&this->rotationStep, 16384.0f, 1.0f, 4096.0f);
            Math_ApproachS(&this->fishLimbDRotZDelta, 0x4E20, 4, 0x1388);

            this->lilyTimer = 50;
            rotXYScale = 2;
            this->fishTargetPos = sLurePos;
            Math_ApproachF(&this->actor.speedXZ, this->speedTarget, 1.0f, 1.0f);

            if ((sRodCastState != 3) || (this->timerArray[0] == 0) || (sLurePos.y > (WATER_SURFACE_Y(play) + 5.0f)) ||
                Fishing_RF3OutOfArea(sLurePos.x, sLurePos.z)) {

                this->timerArray[0] = 0;
                this->fishState = this->fishStateNext;
                this->unk_190 = 1.0f;
                this->unk_194 = 2000.0f;
            } else if (distToTarget < 10.0f) {
                if (Fishing_SplashBySize(this, play, false)) {
                    Fishing_FishLeapSfx(this, false);
                }

                this->fishState = 5;
                this->unk_190 = 1.2f;
                this->unk_194 = 5000.0f;
                this->timerArray[1] = 150;
                this->timerArray[0] = 0;
                this->timerArray[2] = 0;
                this->timerArray[3] = 120;

                sRodCastState = 4;
                sFishingHookedFish = this;
                sFishMouthOffset.y = 500.0f - Rand_ZeroFloat(400.0f);

                // get rumble time based on size and lure used.
                if (sLureEquipped == FS_LURE_SINKING) {
                    if (this->fishLength > 70.0f) {
                        timer = (s16)Rand_ZeroFloat(20.0f) + 10;
                    } else if (this->fishLength > 60.0f) {
                        timer = (s16)Rand_ZeroFloat(30.0f) + 20;
                    } else if (this->fishLength > 50.0f) {
                        timer = (s16)Rand_ZeroFloat(30.0f) + 30;
                    } else {
                        timer = (s16)Rand_ZeroFloat(40.0f) + 40;
                    }
                    sLureBitTimer = timer;
                    sRumbleDelay = timer;
                    if (!RF3_IsEnabled()) { // step 9b: in RF3 mode the rumble marks the hook-set window instead
                        func_800A9F6C(0.0f, 60, timer * 3, 10);
                    }
                } else {
                    if (this->fishLength > 70.0f) {
                        timer = (s16)Rand_ZeroFloat(5.0f) + 10;
                    } else if (this->fishLength > 60.0f) {
                        timer = (s16)Rand_ZeroFloat(5.0f) + 15;
                    } else if (this->fishLength > 50.0f) {
                        timer = (s16)Rand_ZeroFloat(5.0f) + 17;
                    } else {
                        timer = (s16)Rand_ZeroFloat(5.0f) + 25;
                    }
                    sLureBitTimer = timer;
                    sRumbleDelay = timer;
                    if (!RF3_IsEnabled()) { // step 9b: in RF3 mode the rumble marks the hook-set window instead
                        func_800A9F6C(0.0f, 180, timer * 3, 10);
                    }
                }

                sLineHooked = 0;
                sRodPullback = 100;
                sFishFightTime = 0;
            }
            break;

        case -3:
            if (RF3_IsEnabled()) {
                // Step 10: still the chaser while a loach lunges; released after the bite.
                RF3Approach_Chase(Fishing_RF3FishIndex(this), sRodCastState == 3, false, false, RF3_SUBSTEPS);
            }
            this->lilyTimer = 50;
            this->fishTargetPos = sLurePos;
            Math_ApproachF(&this->actor.speedXZ, 2.0f, 1.0f, 1.0f);

            if ((sRodCastState != 3) || (this->timerArray[0] == 0) || (sLurePos.y > (WATER_SURFACE_Y(play) + 5.0f)) ||
                Fishing_RF3OutOfArea(sLurePos.x, sLurePos.z)) {

                this->timerArray[0] = 0;
                this->unk_190 = 1.0f;
                this->fishState = this->fishStateNext;
                this->unk_194 = 2000.0f;
            } else if (distToTarget < 10.0f) {
                if (sLurePos.y > (WATER_SURFACE_Y(play) - 10.0f)) {
                    Audio_PlayActorSound2(&this->actor, NA_SE_EV_JUMP_OUT_WATER);
                    Sfx_PlaySfxCentered(NA_SE_PL_CATCH_BOOMERANG);
                }

                Fishing_SplashBySize(this, play, false);
                this->fishState = 5;
                this->unk_190 = 1.2f;
                this->unk_194 = 5000.0f;
                this->timerArray[1] = 150;
                this->timerArray[0] = 0;
                this->timerArray[2] = 0;
                this->timerArray[3] = 120;

                sRodCastState = 4;
                sFishingHookedFish = this;

                if (sLureEquipped == FS_LURE_SINKING) {
                    sLureBitTimer = 30;
                    sRumbleDelay = 100;
                    if (!RF3_IsEnabled()) { // step 9b: in RF3 mode the rumble marks the hook-set window instead
                        func_800A9F6C(0.0f, 60, 90, 10);
                    }
                } else {
                    sLureBitTimer = 30;
                    sRumbleDelay = 40;
                    if (!RF3_IsEnabled()) { // step 9b: in RF3 mode the rumble marks the hook-set window instead
                        func_800A9F6C(0.0f, 180, 90, 10);
                    }
                }

                sLineHooked = false;
                sRodPullback = 100;
                sFishFightTime = 0;
            }
            break;

        case 5:
            this->actor.uncullZoneForward = 1200.0f;
            this->actor.uncullZoneScale = 200.0f;

            sFishFightTime++;
            osSyncPrintf("HIT FISH %dcm\n", (u8)this->fishLength);

            Math_ApproachS(&this->fishLimbDRotZDelta, 0x2AF8, 4, 0xBB8);
            sFishingHookedFish = this;
            Math_ApproachS(&player->actor.shape.rot.y, this->actor.yawTowardsPlayer + 0x8000, 5, 0x500);

            if (sLineHooked == 0) {
                if ((sSinkingLureSegmentIndex < 20) && ((sLureTimer & 3) == 0)) {
                    sSinkingLureSegmentIndex++;
                }
            }

            // Reel Fishing III mod (step 9): with the RF3 option on, the RF3 hook set decides
            // (sRF3HookPending, set below) and OoT's short bite timer no longer gates it.
            if (((sLureBitTimer != 0) || RF3_IsEnabled()) && (sLineHooked == 0)) { // pull the line to hook it
                if (RF3_IsEnabled() ? sRF3HookPending
                                    : (((input->rel.stick_y < -50) && (sStickAdjYPrev > -40)) ||
                                       CHECK_BTN_ALL(input->press.button, BTN_A))) {
                    sRF3HookPending = false;
                    if (input->rel.stick_y < -50) {
                        temp_f0 = 40.0f - ((this->fishLength - 30.0f) * 1.333333f);
                        if (temp_f0 > 0.0f) {
                            this->unk_152 = temp_f0;
                            this->unk_154 = this->actor.yawTowardsPlayer - this->actor.shape.rot.y;
                            this->unk_156 = 1;
                        }
                    }

                    this->fishLimbRotPhaseStep = 1.7f;
                    this->fishLimbRotPhaseMag = 7000.0f;
                    sLineHooked = 1;
                    Audio_QueueSeqCmd(SEQ_PLAYER_BGM_MAIN << 24 | NA_BGM_ENEMY | 0x800);
                    sFishingMusicDelay = 0;

                    if (this->isLoach == 1) {
                        rumbleStrength = (this->fishLength * 3.0f) + 120.0f;
                    } else {
                        rumbleStrength = (2.0f * this->fishLength) + 120.0f;
                    }
                    if (rumbleStrength > 255.0f) {
                        rumbleStrength = 255.0f;
                    }

                    func_800A9F6C(0.0f, rumbleStrength, 120, 5);
                    sRumbleDelay = 40;
                    sRodHitTimer = 10;
                    Sfx_PlaySfxCentered(NA_SE_IT_FISHING_HIT);
                }
            }

            if (this->actor.world.pos.y < WATER_SURFACE_Y(play)) {
                if (this->timerArray[1] > 30) {
                    phi_v0_2 = 7;
                } else {
                    phi_v0_2 = 0xF;
                }

                if (((this->stateAndTimer & phi_v0_2) == 0) && (Rand_ZeroOne() < 0.75f) && (sRumbleDelay == 0)) {
                    if (this->fishLength >= 70.0f) {
                        rumbleStrength = 255.0f;
                    } else if (this->fishLength >= 60.0f) {
                        rumbleStrength = 230.0f;
                    } else if (this->fishLength >= 50.0f) {
                        rumbleStrength = 200.0f;
                    } else if (this->fishLength >= 40.0f) {
                        rumbleStrength = 170.0f;
                    } else {
                        rumbleStrength = 140.0f;
                    }

                    if (phi_v0_2 == 0xF) {
                        rumbleStrength *= 3.0f / 4.0f;
                    }

                    func_800A9F6C(0.0f, rumbleStrength, (s16)Rand_ZeroFloat(5.0f) + 10, 5);
                }

                if (this->timerArray[1] > 30) {
                    if (this->timerArray[0] == 0) {
                        multiVecSrc.x = 0.0f;
                        multiVecSrc.y = 0.0f;
                        multiVecSrc.z = 200.0f;

                        for (attempts = 0; attempts < 100; attempts++) {
                            Matrix_RotateY(Rand_CenteredFloat(3.0f * M_PI / 4.0f) +
                                               (((this->actor.yawTowardsPlayer + 0x8000) / 32768.0f) * M_PI),
                                           MTXMODE_NEW);
                            Matrix_MultVec3f(&multiVecSrc, &targetPosOffset);

                            this->fishTargetPos.x = this->actor.world.pos.x + targetPosOffset.x;
                            this->fishTargetPos.z = this->actor.world.pos.z + targetPosOffset.z;

                            if ((SQ(this->fishTargetPos.x) + SQ(this->fishTargetPos.z)) < SQ(750.0f)) {
                                break;
                            }
                        }

                        if ((Rand_ZeroOne() < 0.1f) && (this->timerArray[3] == 0)) {
                            if (this->fishLength >= 60.0f) {
                                rumbleStrength8 = 255;
                            } else if (this->fishLength >= 50.0f) {
                                rumbleStrength8 = 200;
                            } else {
                                rumbleStrength8 = 180;
                            }
                            func_800A9F6C(0.0f, rumbleStrength8, 90, 2);
                            this->timerArray[0] = 20;
                            this->timerArray[1] = 100;
                            this->timerArray[2] = 20;
                            this->timerArray[3] = 100;
                            this->fishTargetPos.y = 300.0f;
                            sRumbleDelay = 0x28;
                            sRodPullback = (s16)Rand_ZeroFloat(30.0f) + 20;
                        } else {
                            this->timerArray[0] = (s16)Rand_ZeroFloat(10.0f) + 3;
                            this->timerArray[2] = 0;
                            this->fishTargetPos.y = -70.0f - Rand_ZeroFloat(150.0f);
                        }
                    }

                    if (this->timerArray[2] != 0) {
                        sRodReelingSpeed = 0.0f;
                        this->unk_190 = 1.6f;
                        this->unk_194 = 6000.0f;
                        Math_ApproachF(&this->actor.speedXZ, 7.5f, 1.0f, 1.0f);
                        Math_ApproachS(&this->fishLimbDRotZDelta, 0x4E20, 2, 0xFA0);
                    } else {
                        if ((sLineHooked == 0) && (sLureEquipped == FS_LURE_SINKING)) {
                            this->unk_190 = 1.0f;
                            this->unk_194 = 2000.0f;
                            Math_ApproachF(&this->actor.speedXZ, 3.0f, 1.0f, 0.2f);
                        } else {
                            this->unk_190 = 1.4f;
                            this->unk_194 = 5000.0f;
                            Math_ApproachF(&this->actor.speedXZ, 5.0f, 1.0f, 0.5f);
                        }

                        if (this->isLoach == 0) {
                            sRodReelingSpeed = 1.0f - (this->fishLength * 0.00899f);
                        } else {
                            sRodReelingSpeed = 1.0f - (this->fishLength * 0.00899f * 1.4f);
                        }
                    }
                } else {
                    if (((this->timerArray[1] & 0xF) == 0) && CHECK_BTN_ALL(input->cur.button, BTN_A) &&
                        (!(this->fishLength >= 60.0f) || (sFishFightTime >= 2000))) {
                        if ((sRF3MovedFish == this) && ((u32)(play->gameplayFrames - sRF3MovedFrame) <= 1)) {
                            // Step 30b-1e (user, 2026-10-14: in AWAY, while reeling, "the fish occasionally twitches
                            // towards the reeling direction"): OoT's reel turn (every 16 frames with A held) turns the
                            // drawn body toward Link for 15-45 frames (unk_152/154 -> rotationTarget.y -> unk_162, which
                            // Fishing_DrawFishImpl adds to the RF3 model's yaw). Skipped for a fish on RF3's movement.
                            if ((u32)(play->gameplayFrames - sRF3ReelTurnLogFrame) >= 20) {
                                char line[160];
                                snprintf(line, sizeof(line), "slot=%d rf3-reel-turn skipped toward_link=%.0fdeg",
                                         (s32)(this->actor.params - EN_FISH_REGION),
                                         (s16)(this->actor.yawTowardsPlayer - this->actor.shape.rot.y) *
                                             (360.0f / 65536.0f));
                                RF3FishMove_Log(line);
                                sRF3ReelTurnLogFrame = play->gameplayFrames;
                            }
                        } else {
                            this->unk_152 = (s16)Rand_ZeroFloat(30.0f) + 15;
                            this->unk_154 = this->actor.yawTowardsPlayer - this->actor.shape.rot.y;
                        }
                    }

                    this->unk_190 = 1.0f;
                    this->unk_194 = 4500.0f;

                    if (this->isLoach == 0) {
                        sRodReelingSpeed = 1.3f - (this->fishLength * 0.00899f);
                    } else {
                        sRodReelingSpeed = 1.3f - (this->fishLength * 0.00899f * 1.4f);
                    }

                    Math_ApproachF(&this->actor.speedXZ, 2.0f, 1.0f, 0.5f);

                    if (this->timerArray[1] == 0) {
                        this->unk_152 = 0;

                        if (sFishFightTime < 2000) {
                            this->timerArray[1] = (s16)Rand_ZeroFloat(50.0f) + 50;
                        } else if (sFishFightTime < 3000) {
                            this->timerArray[1] = (s16)Rand_ZeroFloat(20.0f) + 30;
                        } else {
                            this->timerArray[1] = (s16)Rand_ZeroFloat(10.0f) + 25;
                        }
                    }
                }
            }

            if (sReelLock != 0) {
                sRodReelingSpeed = 0.0f;
            }

            if (sLineHooked || (sLureEquipped != FS_LURE_SINKING)) {
                if (this->actor.speedXZ < 3.0f) {
                    if ((sLureTimer & 8) != 0) {
                        targetPosOffset.x = -0.8f;
                    } else {
                        targetPosOffset.x = -0.75f;
                    }
                } else {
                    if ((sLureTimer & 4) != 0) {
                        targetPosOffset.x = -0.9f;
                    } else {
                        targetPosOffset.x = -0.85f;
                    }
                }

                Math_ApproachF(&D_80B7A6C0, 35.0f, 0.1f, 3.5f);
                Math_ApproachF(&D_80B7A6BC, targetPosOffset.x, 0.3f, 0.1f);
            }

            sReelLinePos[LINE_SEG_COUNT - 1] = this->fishMouthPos;
            multiVecSrc.x = sReelLinePos[LINE_SEG_COUNT - 1].x - sReelLinePos[LINE_SEG_COUNT - 2].x;
            multiVecSrc.y = sReelLinePos[LINE_SEG_COUNT - 1].y - sReelLinePos[LINE_SEG_COUNT - 2].y;
            multiVecSrc.z = sReelLinePos[LINE_SEG_COUNT - 1].z - sReelLinePos[LINE_SEG_COUNT - 2].z;

            if (((SQ(multiVecSrc.x) + SQ(multiVecSrc.y) + SQ(multiVecSrc.z)) > SQ(20.0f)) &&
                (sRF3MovedFish == this) && ((u32)(play->gameplayFrames - sRF3MovedFrame) <= 1)) {
                // Step 30b-1d (user, 2026-10-14: in AWAY, while reeling, "the fish occasionally twitches towards the
                // reeling direction"): OoT's line leash pulls a hooked fish to the line's next point once its mouth is
                // more than 20 units away. Reeling shortens OoT's line, so a fish on RF3's movement (the sim's distance
                // and FOLLOW's drag already follow the line) was yanked toward Link. Skipped for it; logged.
                f32 gapX = sReelLinePos[LINE_SEG_COUNT - 2].x - this->actor.world.pos.x;
                f32 gapZ = sReelLinePos[LINE_SEG_COUNT - 2].z - this->actor.world.pos.z;
                f32 gap = sqrtf(SQ(multiVecSrc.x) + SQ(multiVecSrc.y) + SQ(multiVecSrc.z));
                f32 pull = MIN(sqrtf(SQ(gapX) + SQ(gapZ)) * 0.2f, 3.0f * this->actor.speedXZ);
                sRF3LeashSkips++;
                if ((u32)(play->gameplayFrames - sRF3LeashLogFrame) >= 20) {
                    RF3FightStatus fs;
                    char line[200];
                    RF3Fight_GetStatus(&fs);
                    snprintf(line, sizeof(line),
                             "slot=%d rf3-leash skipped=%d gap=%.1f would_pull=%.2f/frame mode=%d reel=%d",
                             (s32)(this->actor.params - EN_FISH_REGION), sRF3LeashSkips, gap, pull,
                             fs.active ? fs.mode : -1, fs.reelHeld);
                    RF3FishMove_Log(line);
                    sRF3LeashSkips = 0;
                    sRF3LeashLogFrame = play->gameplayFrames;
                }
            } else if ((SQ(multiVecSrc.x) + SQ(multiVecSrc.y) + SQ(multiVecSrc.z)) > SQ(20.0f)) {
                Math_ApproachF(&this->actor.world.pos.x, sReelLinePos[LINE_SEG_COUNT - 2].x, 0.2f,
                               2.0f * (this->actor.speedXZ * 1.5f));
                Math_ApproachF(&this->actor.world.pos.y, sReelLinePos[LINE_SEG_COUNT - 2].y, 0.2f,
                               2.0f * (this->actor.speedXZ * 1.5f) * 5.0f * 0.1f);
                Math_ApproachF(&this->actor.world.pos.z, sReelLinePos[LINE_SEG_COUNT - 2].z, 0.2f,
                               2.0f * (this->actor.speedXZ * 1.5f));
            }

            // Reel Fishing III mod (steps 8-9): RF3 runs the hook set from the bite on, then
            // the fight: the fish's distance and whether it gets away or is caught.
            rf3Outcome = RF3_OUTCOME_NONE;
            rf3Hooked = false;
            rf3Fight = RF3_IsEnabled();
            if (rf3Fight) {
                if (!sLineHooked && !RF3Fight_IsActive()) {
                    RF3Fight_StartStrike(Fishing_RF3FishIndex(this), this->fishLength); // the bite is the strike
                    // Step 33a: a bait set's bait goes with the bite (after StartStrike read the fight's tackle).
                    RF3Stock_UseBait("strike");
                    RF3Stock_NoteBite(play->gameplayFrames); // step 43a: bite timing for the lure conditions
                    Fishing_RF3ResetVisuals(this);
                }
                rf3Outcome = Fishing_RF3Fight(this, play, input, &rf3Hooked);
                if (sRF3TurnScale != 0) {
                    rotXYScale = sRF3TurnScale; // step 8h: STRUGGLE turns fast
                }
                if (rf3Hooked && !sLineHooked) {
                    sRF3HookPending = true; // OoT's hook-set block above applies the effects next frame
                }
            }

            if (CHECK_BTN_ALL(input->cur.button, BTN_A) || (input->rel.stick_y < -30)) {
                if (sRodPullback < 100) {
                    sRodPullback++;
                }
            } else {
                if (sRodPullback != 0) {
                    sRodPullback--;
                }
            }

            if (rf3Fight
                    ? ((sRodCastState < 3) || (rf3Outcome == RF3_OUTCOME_LINEBREAK) ||
                       (rf3Outcome == RF3_OUTCOME_HOOKOUT) || (rf3Outcome == RF3_OUTCOME_MISSED))
                    : ((sRodCastState < 3) || ((sReelLock != 0) && (sFishFightTime > 50)) ||
                       (sFishFightTime >= 6000) || ((sLureBitTimer == 0) && (sLineHooked == 0)) ||
                       (sRodPullback == 0) ||
                       GameInteractor_Should(VB_FISHING_FISH_ESCAPE,
                                             ((sLureTimer & 0x7F) == 0) && (Rand_ZeroOne() < 0.05f) &&
                                                 (sLureEquipped != FS_LURE_SINKING) && (KREG(69) == 0)))) {
                if (rf3Fight) {
                    RF3Fight_End();
                    sRF3HookPending = false;
                    // Step 43a: a hooked fish lost (line break, hook out, or the line reeled in with it on) ends the lure
                    // conditions' streaks; a missed bite was never hooked.
                    if ((rf3Outcome == RF3_OUTCOME_LINEBREAK) || (rf3Outcome == RF3_OUTCOME_HOOKOUT) ||
                        ((rf3Outcome != RF3_OUTCOME_MISSED) && sLineHooked)) {
                        RF3Stock_FishLost(rf3Outcome == RF3_OUTCOME_LINEBREAK  ? "line-break"
                                          : rf3Outcome == RF3_OUTCOME_HOOKOUT ? "hook-out"
                                                                              : "reeled-in");
                    }
                    if (rf3Outcome == RF3_OUTCOME_MISSED) {
                        sLureBitTimer = 0; // step 9: OoT's "missed bite" text (0x4081) below
                        // Step 10: a fish that spat the lure stays wary for 60 s (RF3-MECHANICS 2.3).
                        RF3Approach_SetWary(Fishing_RF3FishIndex(this), 60.0f);
                    }
                    if (rf3Outcome == RF3_OUTCOME_LINEBREAK) {
                        // Step 8e: the line snaps. OoT's rod-snap bend plus a twang.
                        sRodHitTimer = 10;
                        Sfx_PlaySfxCentered(NA_SE_IT_BOW_FLICK);
                        RF3Stock_LoseCurrent(); // step 21e: the fish takes the lure, until the next scene
                                                // change
                        if (RF3Tackle_LureMissing()) {
                            sLureEquipped = FS_LURE_STOCK; // step 32a: a lost pole sinking lure isn't drawn either
                        }
                    }
                }
                sFishingCaughtTextDelay = 20;

                if ((sLureBitTimer == 0) && (sLineHooked == 0)) {
                    sFishingCaughtTextId = 0x4081;
                    if (((sLinkAge == LINK_AGE_CHILD) && (HIGH_SCORE(HS_FISHING) & HS_FISH_PRIZE_CHILD)) ||
                        ((sLinkAge != LINK_AGE_CHILD) && (HIGH_SCORE(HS_FISHING) & HS_FISH_PRIZE_ADULT))) {
                        sFishingCaughtTextDelay = 0;
                    }
                } else {
                    sFishingCaughtTextId = 0x4082;
                    func_800A9F6C(0.0f, 1, 3, 1);
                    Audio_QueueSeqCmd(0x1 << 28 | SEQ_PLAYER_BGM_MAIN << 24 | 0x0A00FF);
                }
                if (sRF3Cancelled) {
                    sFishingCaughtTextDelay = 0; // step 20a-6: the player cancelled; no text
                    sRF3Cancelled = false;
                }

                this->fishState = this->fishStateNext = 0;
                this->unk_1A4 = 10000;
                this->unk_1A2 = 500;
                this->timerArray[1] = 50;
                this->timerArray[0] = 0;
                this->unk_190 = 1.0f;
                this->unk_194 = 3000.0f;

                if (sRodCastState == 4) {
                    sRodCastState = 3;
                }

                sFishingMusicDelay = 50;
                sRodReelingSpeed = 0.5f;
                this->unk_152 = 0;
            } else if (rf3Fight ? ((rf3Outcome == RF3_OUTCOME_CAUGHT) || (rf3Outcome == RF3_OUTCOME_BUKKONUKI))
                                : GameInteractor_Should(VB_FISHING_CATCH_FISH,
                                                        this->actor.xzDistToPlayer < (KREG(59) + 50.0f))) {
                if (rf3Fight) {
                    RF3Fight_End();
                    sRF3HookPending = false;
                    sLineHooked = 1; // a perfect hook set (BUKKONUKI) lands the fish before OoT's hook block runs
                }
                this->fishState = 6;
                this->timerArray[0] = 100;
                player->unk_860 = 3;
                func_800A9F6C(0.0f, 1, 3, 1);
                sFishesCaught++;
                func_80064520(play, &play->csCtx);
                sFishingPlayerCinematicState = 100;
                sCatchCamX = 45.0f;
                sRodCastState = 5;
                this->unk_190 = 1.0f;
                this->unk_194 = 500.0f;
                this->fishLimbRotPhaseMag = 5000.0f;

                if (this->actor.world.pos.y <= WATER_SURFACE_Y(play)) {
                    Fishing_FishLeapSfx(this, true);
                    Fishing_SplashBySize(this, play, true);
                }
                goto hoistCatch;
            }
            break;

        hoistCatch:
        case 6:
            Math_ApproachS(&this->fishLimbDRotZDelta, 0x2AF8, 2, 0xFA0);
            Math_ApproachF(&sCatchCamX, 15.0f, 0.05f, 0.75f);

            multiVecSrc.x = sCatchCamX;
            if (sLinkAge != LINK_AGE_CHILD) {
                multiVecSrc.y = 30.0f;
                multiVecSrc.z = 55.0f;
            } else {
                multiVecSrc.y = 10.0f;
                multiVecSrc.z = 50.0f;
            }
            Matrix_RotateY((player->actor.shape.rot.y / 32768.0f) * M_PI, MTXMODE_NEW);
            Matrix_MultVec3f(&multiVecSrc, &sCameraEye);

            sCameraEye.x += player->actor.world.pos.x;
            sCameraEye.y += player->actor.world.pos.y;
            sCameraEye.z += player->actor.world.pos.z;

            sCameraAt = player->actor.world.pos;
            if (sLinkAge != LINK_AGE_CHILD) {
                sCameraAt.y += 40.0f;
            } else {
                sCameraAt.y += 25.0f;
            }

            if (this->timerArray[0] == 90) {
                Audio_QueueSeqCmd(SEQ_PLAYER_BGM_MAIN << 24 | NA_BGM_HEART_GET | 0x900);
                sFishingCaughtTextDelay = 40;

                if (this->isLoach == 0) {
                    sFishLengthToWeigh = this->fishLength;
                    RF3Size_SetLengthToWeigh(sFishLengthToWeigh, RF3_WEIGH_CAUGHT, sLinkAge == LINK_AGE_CHILD); // steps 12b/14d: weigh-in shows RF3 length

                    if (sFishLengthToWeigh >= 75) {
                        sFishingCaughtTextId = 0x409F;
                    } else if (sFishLengthToWeigh >= 50) {
                        sFishingCaughtTextId = 0x4091;
                    } else {
                        sFishingCaughtTextId = 0x4083;
                    }
                } else {
                    sFishLengthToWeigh = 2.0f * this->fishLength;
                    // Step 23b: the RF3 size code keys the fish by its own OoT length; OoT weighs a loach at double.
                    RF3Size_SetLengthToWeigh(this->fishLength, RF3_WEIGH_CAUGHT, sLinkAge == LINK_AGE_CHILD);
                    sFishingCaughtTextId = 0x4099;
                }

                this->keepState = 0;
            }

            this->unk_160 = -0x4000;
            this->actor.shape.rot.y = player->actor.shape.rot.y + 0x5000;
            this->actor.shape.rot.x = this->actor.shape.rot.z = this->unk_162 = this->unk_164 = this->unk_16E = 0;

            multiVecSrc.x = 4.0f;
            multiVecSrc.y = -10.0f;
            multiVecSrc.z = 5.0f;
            Matrix_MultVec3f(&multiVecSrc, &targetPosOffset);
            if (!GameInteractor_Should(VB_FISHING_INSTANT_CATCH, false)) {
                Math_ApproachF(&this->actor.world.pos.x, player->bodyPartsPos[15].x + targetPosOffset.x, 1.0f, 6.0f);
                Math_ApproachF(&this->actor.world.pos.y, player->bodyPartsPos[15].y + targetPosOffset.y, 1.0f, 6.0f);
                Math_ApproachF(&this->actor.world.pos.z, player->bodyPartsPos[15].z + targetPosOffset.z, 1.0f, 6.0f);
            } else {
                this->actor.world.pos.x = player->bodyPartsPos[15].x + targetPosOffset.x;
                this->actor.world.pos.y = player->bodyPartsPos[15].y + targetPosOffset.y;
                this->actor.world.pos.z = player->bodyPartsPos[15].z + targetPosOffset.z;
            }
            sRodLineSpooled = 188.0f;

            if (this->timerArray[0] <= 50) {
                switch (this->keepState) {
                    case 0:
                        if (RF3_IS_REGION_FISH(this)) {
                            // Step 16e: outside the pond the catch is recorded, then released: no Keep
                            // choice. Once the catch message (started by the delay) has closed, let go.
                            if ((sFishingCaughtTextDelay == 0) && (Message_GetState(&play->msgCtx) == TEXT_STATE_NONE)) {
                                sRodCastState = 0;
                            }
                        } else if ((Message_GetState(&play->msgCtx) == TEXT_STATE_CHOICE) ||
                            (Message_GetState(&play->msgCtx) == TEXT_STATE_NONE)) {
                            if (Message_ShouldAdvance(play)) {
                                Message_CloseTextbox(play);
                                if (play->msgCtx.choiceIndex == 0) {
                                    if (sFishOnHandLength == 0.0f) {
                                        sFishOnHandLength = this->fishLength;
                                        sFishOnHandIsLoach = this->isLoach;
                                        sLureCaughtWith = sLureEquipped;
                                        RF3Size_KeepCaught(sFishOnHandLength); // step 14d: remember its species
                                        Actor_Kill(&this->actor);
                                    } else if (GameInteractor_Should(
                                                   VB_FISHING_CONFIRM_KEEPING_SMALLER_FISH,
                                                   (this->isLoach == 0) && (sFishOnHandIsLoach == 0) &&
                                                       ((s16)this->fishLength < (s16)sFishOnHandLength))) {
                                        this->keepState = 1;
                                        this->timerArray[0] = 0x3C;
                                        Message_StartTextbox(play, 0x4098, NULL);
                                    } else {
                                        f32 lengthTemp = sFishOnHandLength;
                                        s16 loachTemp = sFishOnHandIsLoach;
                                        sFishOnHandLength = this->fishLength;
                                        sFishOnHandIsLoach = this->isLoach;
                                        sLureCaughtWith = sLureEquipped;
                                        RF3Size_KeepCaught(sFishOnHandLength); // step 14d: remember its species
                                        this->fishLength = lengthTemp;
                                        this->isLoach = loachTemp;
                                    }
                                }
                                if (this->keepState == 0) {
                                    sRodCastState = 0;
                                }
                            }
                        }
                        break;
                    case 1:
                        if ((Message_GetState(&play->msgCtx) == TEXT_STATE_CHOICE) ||
                            (Message_GetState(&play->msgCtx) == TEXT_STATE_NONE)) {
                            if (Message_ShouldAdvance(play)) {
                                Message_CloseTextbox(play);
                                if (play->msgCtx.choiceIndex != 0) {
                                    f32 temp1 = sFishOnHandLength;
                                    s16 temp2 = sFishOnHandIsLoach;
                                    sFishOnHandLength = this->fishLength;
                                    sLureCaughtWith = sLureEquipped;
                                    RF3Size_KeepCaught(sFishOnHandLength); // step 14d: remember its species
                                    this->fishLength = temp1;
                                    this->isLoach = temp2;
                                }
                                sRodCastState = 0;
                            }
                        }
                        break;
                }
            }

            if (sRodCastState == 0) {
                if (this->actor.update != NULL) {
                    if (RF3_IS_REGION_FISH(this)) {
                        // Step 16e: released back where it lives (Link may stand on land).
                        this->actor.world.pos = this->actor.prevPos = this->actor.home.pos;
                    }
                    this->fishState = this->fishStateNext = 0;
                    this->unk_1A4 = 10000;
                    this->unk_1A2 = 500;
                    this->timerArray[1] = 50;
                    this->timerArray[0] = 0;
                    this->unk_190 = 1.0f;
                    this->unk_194 = 2000.0f;
                    SkelAnime_Free(&this->skelAnime, play);

                    if (this->isLoach == 0) {
                        SkelAnime_InitFlex(play, &this->skelAnime, &gFishingFishSkel, &gFishingFishAnim, 0, 0, 0);
                        Animation_MorphToLoop(&this->skelAnime, &gFishingFishAnim, 0.0f);
                    } else {
                        SkelAnime_InitFlex(play, &this->skelAnime, &gFishingLoachSkel, &gFishingLoachAnim, 0, 0, 0);
                        Animation_MorphToLoop(&this->skelAnime, &gFishingLoachAnim, 0.0f);
                    }
                }

                D_80B7E148 = 520.0f;
                sRodLineSpooled = 195.0f;
                Audio_QueueSeqCmd(0x1 << 28 | SEQ_PLAYER_BGM_MAIN << 24 | 0xA00FF);
                sFishingMusicDelay = 20;
                sFishingPlayerCinematicState = 3;
            }
            break;

        case 7:
            this->lilyTimer = 50;
            rotXYScale = 5;
            this->rotationStep = 12288.0f;

            if (this->actor.params < (EN_FISH_PARAM + 4)) {
                this->fishTargetPos = sGroupFishes[this->actor.params - EN_FISH_PARAM].pos;
                sFishGroupVar = 1;
            } else if (this->actor.params < (EN_FISH_PARAM + 8)) {
                this->fishTargetPos = sGroupFishes[this->actor.params - EN_FISH_PARAM + 16].pos;
                sFishGroupVar = 2;
            } else {
                this->fishTargetPos = sGroupFishes[this->actor.params - EN_FISH_PARAM + 32].pos;
                sFishGroupVar = 3;
            }

            Math_ApproachF(&this->actor.speedXZ, 5.0f, 1.0f, 1.0f);

            if (distToTarget < 20.0f) {
                Math_ApproachS(&this->fishLimbDRotZDelta, 0x4E20, 2, 0xFA0);

                if ((this->timerArray[2] == 0) && Fishing_SplashBySize(this, play, false)) {
                    Fishing_FishLeapSfx(this, Rand_ZeroFloat(1.99f));
                    this->timerArray[2] = (s16)Rand_ZeroFloat(20.0f) + 20;
                }
            }

            if (this->timerArray[3] == 0) {
                this->fishState = 10;
                this->fishStateNext = 10;
            } else {
                func_80B70ED4(this, input);
                if (this->actor.xzDistToPlayer < (100.0f * playerSpeedMod)) {
                    this->fishStateNext = this->fishState = 0;
                    this->unk_1A4 = 500;
                    this->unk_1A2 = 200;
                    this->timerArray[1] = 50;
                }
            }
            break;
    }

    Math_ApproachS(&this->fishLimbEFRotYDelta, (Math_SinS(this->stateAndTimer * 0x1000) * 5000.0f) + 5000.0f, 2, 0x7D0);

    if (this->fishState != 6) {
        if (this->actor.world.pos.y > WATER_SURFACE_Y(play)) {
            this->unk_190 = 1.5f;
            this->unk_194 = 5000.0f;

            Math_ApproachS(&this->unk_16E, 0, 5, 0x7D0);

            rotXScale = rotYScale = rotZScale = 3;
            rotXStep = rotYStep = 0x2000;

            this->timerArray[2] = 0;
            this->unk_184 -= 1.0f;
        } else {
            Math_ApproachZeroF(&this->unk_184, 1.0f, 2.0f);
            if ((this->fishState != -1) && (this->fishState != -2) && (this->fishState != -25)) {
                this->rotationTarget.x = 0;
            }

            this->rotationTarget.y = this->rotationTarget.z = 0;
            rotXScale = rotYScale = rotZScale = 4;
            rotXStep = rotYStep = 0x2000;

            spF6 = Fishing_SmoothStepToS(&this->actor.world.rot.y, rotYTarget, rotXYScale, this->rotationStep) * 3.0f;
            Math_ApproachS(&this->actor.world.rot.x, rotXTarget, rotXYScale, this->rotationStep * 0.5f);

            if (spF6 > 0x1F40) {
                spF6 = 0x1F40;
            } else if (spF6 < -0x1F40) {
                spF6 = -0x1F40;
            }

            if (this->actor.speedXZ >= 3.2f) {
                Math_ApproachS(&this->unk_16E, spF6, 2, 0x4E20);
            } else {
                Math_ApproachS(&this->unk_16E, spF6, 3, 0xBB8);
            }

            Actor_UpdateVelocityXYZ(&this->actor);
        }

        Actor_UpdatePos(&this->actor);

        this->actor.world.pos.y += (this->unk_184 * 1.5f);

        if (this->unk_152 != 0) {
            this->rotationTarget.y = this->unk_154;
            this->unk_152--;
            if (this->unk_156 != 0) {
                rotYScale = 5;
                rotYStep = 0x4000;
            } else {
                rotYScale = 10;
                rotYStep = 0x800;
            }
            this->rotationTarget.x = -0x500 - this->actor.shape.rot.x;
            rotXScale = 5;
            rotXStep = 0x4000;
        } else {
            this->unk_156 = 0;
        }

        Math_ApproachS(&this->unk_160, this->rotationTarget.x, rotXScale, rotXStep);
        Math_ApproachS(&this->unk_162, this->rotationTarget.y, rotYScale, rotYStep);
        Math_ApproachS(&this->unk_164, this->rotationTarget.z, rotZScale, 0x2000);

        if (this->actor.speedXZ <= 0.5f) {
            Math_ApproachS(&this->actor.shape.rot.x, 0, 10, this->unk_178);
            Math_ApproachS(&this->unk_178, 0x500, 1, 0x20);
        } else {
            Math_ApproachS(&this->actor.shape.rot.x, -this->actor.world.rot.x, 10, 0x1000);
            this->unk_178 = 0;
        }

        this->actor.shape.rot.y = this->actor.world.rot.y;

        if ((this->fishState != -1) && (this->fishState != -2) && (this->fishState != -25)) {
            if ((this->actor.world.pos.y > WATER_SURFACE_Y(play)) && (this->actor.prevPos.y <= WATER_SURFACE_Y(play))) {
                Fishing_SplashBySize(this, play, true);
                Fishing_FishLeapSfx(this, true);
                this->unk_184 = this->actor.velocity.y;
                this->actor.velocity.y = 0.0f;
                this->rotationTarget.z = Rand_CenteredFloat(32768.0f);
            } else if ((this->actor.world.pos.y < WATER_SURFACE_Y(play)) &&
                       (this->actor.prevPos.y >= WATER_SURFACE_Y(play))) {
                if (this->unk_184 < -5.0f) {
                    this->unk_184 = -5.0f;
                }
                this->actor.world.rot.x = -0xFA0;
                Fishing_SplashBySize(this, play, true);
                this->bubbleTime = 20;
                Fishing_FishLeapSfx(this, 0);
            }
        }

        if ((this->actor.world.pos.y < WATER_SURFACE_Y(play)) &&
            (this->actor.world.pos.y > (WATER_SURFACE_Y(play) - 10.0f)) && ((this->stateAndTimer & 1) == 0) &&
            (this->actor.speedXZ > 0.0f)) {
            Vec3f pos = this->actor.world.pos;
            pos.y = WATER_SURFACE_Y(play);
            Fishing_SpawnRipple(&this->actor.projectedPos, sFishingEffects, &pos, 80.0f, 500.0f, 150, 90);
        }

        if ((this->actor.speedXZ > 0.0f) || (this->fishState == 5)) {
            f32 velocityY = this->actor.velocity.y;

            spD8 = this->fishLength * 0.1f;

            this->actor.world.pos.y -= spD8;
            this->actor.prevPos.y -= spD8;
            this->actor.velocity.y = -1.0f;
            Actor_UpdateBgCheckInfo(play, &this->actor, 30.0f, 30.0f, 100.0f, 0x45);
            this->actor.world.pos.y += spD8;
            this->actor.prevPos.y += spD8;

            this->actor.velocity.y = velocityY;

            if (this->actor.bgCheckFlags & BGCHECKFLAG_WALL) {
                this->bumpTimer = 20;
            }

            if (this->actor.bgCheckFlags & BGCHECKFLAG_GROUND) {
                if (this->actor.world.pos.y > WATER_SURFACE_Y(play)) {
                    this->unk_184 = Rand_ZeroFloat(3.0f) + 3.0f;
                    this->actor.velocity.x = this->actor.world.pos.x * -0.003f;
                    this->actor.velocity.z = this->actor.world.pos.z * -0.003f;

                    Audio_PlayActorSound2(&this->actor, NA_SE_EV_FISH_LEAP);
                    Fishing_SplashBySize2(this, play);

                    if (Rand_ZeroOne() < 0.5f) {
                        this->rotationTarget.z = 0x4000;
                    } else {
                        this->rotationTarget.z = -0x4000;
                    }

                    if (Rand_ZeroOne() < 0.5f) {
                        this->rotationTarget.x = 0;
                    } else {
                        this->rotationTarget.x = (s16)Rand_CenteredFloat(32.0f) + 0x8000;
                    }

                    this->rotationTarget.y = (s16)Rand_CenteredFloat(16384.0f);
                    this->unk_190 = 1.0f;
                    this->unk_194 = 5000.0f;
                    this->fishLimbRotPhaseMag = 5000.0f;
                } else {
                    this->unk_184 = 0.0f;

                    if ((this->fishState == 5) && ((this->stateAndTimer & 1) == 0)) {
                        Vec3f pos;

                        pos.x = Rand_CenteredFloat(10.0f) + this->actor.world.pos.x;
                        pos.z = Rand_CenteredFloat(10.0f) + this->actor.world.pos.z;
                        pos.y = this->actor.floorHeight + 5.0f;
                        Fishing_SpawnWaterDust(&this->actor.projectedPos, sFishingEffects, &pos,
                                               (this->fishLength * 0.005f) + 0.15f);
                    }
                }
            }
        }
    }

    if (this->bubbleTime != 0) {
        s16 i;
        Vec3f pos;
        f32 range = (this->fishLength * 0.075f) + 10.0f;

        this->bubbleTime--;

        for (i = 0; i < 2; i++) {
            pos.x = Rand_CenteredFloat(range) + this->actor.world.pos.x;
            pos.y = Rand_CenteredFloat(range) + this->actor.world.pos.y;
            pos.z = Rand_CenteredFloat(range) + this->actor.world.pos.z;
            Fishing_SpawnBubble(&this->actor.projectedPos, sFishingEffects, &pos, Rand_ZeroFloat(0.035f) + 0.04f,
                                0);
        }
    }
}

// Reel Fishing III mod (step 16d): a region fish updates with its region standing in for the pond.
void Fishing_UpdateFish(Actor* thisx, PlayState* play) {
    Fishing* this = (Fishing*)thisx;

    sRF3CurRegion = RF3_IS_REGION_FISH(this) ? &sRF3RegionBoxes[this->actor.params - EN_FISH_REGION] : NULL;
    Fishing_UpdateFishImpl(thisx, play);
    Fishing_RF3WallTest(this, play);    // step 26e-8
    Fishing_RF3KeepInWater(this, play); // step 16f
    if (sRF3CurRegion != NULL) {
        sRF3WallPrev[this->actor.params - EN_FISH_REGION] = this->actor.world.pos; // step 26e-8: where the next test starts
        sRF3WallPrevFrame[this->actor.params - EN_FISH_REGION] = play->gameplayFrames;
    }
    sRF3CurRegion = NULL;
}

s32 Fishing_FishOverrideLimbDraw(PlayState* play, s32 limbIndex, Gfx** dList, Vec3f* pos, Vec3s* rot, void* thisx) {
    Fishing* this = (Fishing*)thisx;

    if (limbIndex == 0xD) {
        rot->z -= this->fishLimbDRotZDelta - 11000;
    } else if ((limbIndex == 2) || (limbIndex == 3)) {
        rot->y += this->fishLimb23RotYDelta;
    } else if (limbIndex == 4) {
        rot->y += this->fishLimb4RotYDelta;
    } else if (limbIndex == 0xE) {
        rot->y -= this->fishLimbEFRotYDelta;
    } else if (limbIndex == 0xF) {
        rot->y += this->fishLimbEFRotYDelta;
    } else if (limbIndex == 8) {
        rot->y += this->fishLimb89RotYDelta;
    } else if (limbIndex == 9) {
        rot->y -= this->fishLimb89RotYDelta;
    }

    return 0;
}

void Fishing_FishPostLimbDraw(PlayState* play, s32 limbIndex, Gfx** dList, Vec3s* rot, void* thisx) {
    Fishing* this = (Fishing*)thisx;

    if (limbIndex == 0xD) {
        Matrix_MultVec3f(&sFishMouthOffset, &this->fishMouthPos);
    }
}

s32 Fishing_LoachOverrideLimbDraw(PlayState* play, s32 limbIndex, Gfx** dList, Vec3f* pos, Vec3s* rot, void* thisx) {
    Fishing* this = (Fishing*)thisx;

    if (limbIndex == 3) {
        rot->y += this->loachRotYDelta[0];
    } else if (limbIndex == 4) {
        rot->y += this->loachRotYDelta[1];
    } else if (limbIndex == 5) {
        rot->y += this->loachRotYDelta[2];
    }

    return 0;
}

void Fishing_LoachPostLimbDraw(PlayState* play, s32 limbIndex, Gfx** dList, Vec3s* rot, void* thisx) {
    static Vec3f sLoachMouthOffset = { 500.0f, 500.0f, 0.0f };
    Fishing* this = (Fishing*)thisx;

    if (limbIndex == 0xB) {
        Matrix_MultVec3f(&sLoachMouthOffset, &this->fishMouthPos);
    }
}

static void Fishing_DrawFishImpl(Actor* thisx, PlayState* play) {
    Fishing* this = (Fishing*)thisx;

    Gfx_SetupDL_25Opa(play->state.gfxCtx);

    Matrix_Translate(this->actor.world.pos.x, this->actor.world.pos.y, this->actor.world.pos.z, MTXMODE_NEW);
    Matrix_RotateY(((this->unk_162 + this->actor.shape.rot.y) / 32768.0f) * M_PI, MTXMODE_APPLY);
    Matrix_RotateX(((this->unk_160 + this->actor.shape.rot.x) / 32768.0f) * M_PI, MTXMODE_APPLY);
    Matrix_RotateZ(((this->unk_164 + this->actor.shape.rot.z) / 32768.0f) * M_PI, MTXMODE_APPLY);

    // Reel Fishing III mod (steps 25e-2, 25g-2): a region fish with an RF3 model draws that instead of OoT's skeleton,
    // animated with RF3's own motions (chosen from its speed, turning, and the fight when hooked), and sets
    // fishMouthPos from RF3's snout.
    if (RF3_IS_REGION_FISH(this)) {
        RF3FishDrawInput in;
        in.slot = this->actor.params - EN_FISH_REGION;
        in.bendA = this->fishLimb23RotYDelta;
        in.bendB = this->fishLimb4RotYDelta;
        in.speedXZ = this->actor.speedXZ;
        in.yaw = this->actor.shape.rot.y;
        in.hooked = (sFishingHookedFish == this) && ((sRodCastState == 4) || (sRodCastState == 5));
        in.frame = play->gameplayFrames;
        if (RF3FishModel_DrawRegion(play, &in, &this->fishMouthPos.x)) {
            return;
        }
    }

    Matrix_Scale(this->actor.scale.x, this->actor.scale.y, this->actor.scale.z, MTXMODE_APPLY);

    if (this->isLoach == 0) {
        Matrix_RotateY((this->fishLimb23RotYDelta * (M_PI / 32768)) - (M_PI / 2), MTXMODE_APPLY);
        Matrix_Translate(0.0f, 0.0f, this->fishLimb23RotYDelta * 10.0f * 0.01f, MTXMODE_APPLY);

        SkelAnime_DrawSkeletonOpa(play, &this->skelAnime, Fishing_FishOverrideLimbDraw, Fishing_FishPostLimbDraw, this);
    } else {
        Matrix_Translate(0.0f, 0.0f, 3000.0f, MTXMODE_APPLY);
        Matrix_RotateY(this->fishLimb23RotYDelta * (M_PI / 32768), MTXMODE_APPLY);
        Matrix_Translate(0.0f, 0.0f, -3000.0f, MTXMODE_APPLY);
        Matrix_RotateY(-(M_PI / 2), MTXMODE_APPLY);

        SkelAnime_DrawSkeletonOpa(play, &this->skelAnime, Fishing_LoachOverrideLimbDraw, Fishing_LoachPostLimbDraw,
                                  this);
    }
}

// Reel Fishing III mod (step 16d): the same for drawing.
void Fishing_DrawFish(Actor* thisx, PlayState* play) {
    Fishing* this = (Fishing*)thisx;

    sRF3CurRegion = RF3_IS_REGION_FISH(this) ? &sRF3RegionBoxes[this->actor.params - EN_FISH_REGION] : NULL;
    Fishing_DrawFishImpl(thisx, play);
    sRF3CurRegion = NULL;
}

void Fishing_HandleReedContact(FishingProp* prop, Vec3f* entityPos) {
    f32 dx = prop->pos.x - entityPos->x;
    f32 dz = prop->pos.z - entityPos->z;
    f32 distXZ = sqrtf(SQ(dx) + SQ(dz));

    if (distXZ <= 20.0f) {
        prop->rotY = Math_Atan2F(dz, dx);

        Math_ApproachF(&prop->rotX, (20.0f - distXZ) * 0.03f, 0.2f, 0.2f);
    }
}

void Fishing_HandleLilyPadContact(FishingProp* prop, Vec3f* entityPos, u8 fishTimer) {
    f32 dx = prop->pos.x - entityPos->x;
    f32 dz = prop->pos.z - entityPos->z;
    f32 distXZ = sqrtf(SQ(dx) + SQ(dz));

    if (distXZ <= 40.0f) {
        Math_ApproachS(&prop->lilyPadAngle, Math_Atan2S(dz, dx), 10, 0x300);
    }

    if (fishTimer && (distXZ <= 60.0f)) {
        f32 heightTarget = 1.0f;

        if (fishTimer >= 21) {
            heightTarget = 1.5f;
        }

        Math_ApproachF(&prop->lilyPadOffset, heightTarget, 0.1f, 0.2f);
    }
}

void Fishing_UpdatePondProps(PlayState* play) {
    FishingProp* prop = &sPondProps[0];
    Player* player = GET_PLAYER(play);
    Actor* actor;
    s16 i;

    for (i = 0; i < POND_PROP_COUNT; i++) {
        if (prop->type != FS_PROP_NONE) {
            prop->shouldDraw = false;
            prop->timer++;

            SkinMatrix_Vec3fMtxFMultXYZW(&play->viewProjectionMtxF, &prop->pos, &prop->projectedPos, &sProjectedW);

            if ((prop->projectedPos.z < prop->drawDistance) &&
                (fabsf(prop->projectedPos.x) < (100.0f + prop->projectedPos.z))) {
                prop->shouldDraw = true;
            }

            if ((prop->projectedPos.z < 500.0f) && (fabsf(prop->projectedPos.x) < (100.0f + prop->projectedPos.z))) {
                if (prop->type == FS_PROP_REED) {
                    Fishing_HandleReedContact(prop, &player->actor.world.pos);

                    actor = play->actorCtx.actorLists[ACTORCAT_NPC].head;
                    while (actor != NULL) {
                        if (!((actor->id == ACTOR_FISHING) && (actor->params >= EN_FISH_PARAM))) {
                            actor = actor->next;
                        } else {
                            Fishing_HandleReedContact(prop, &actor->world.pos);
                            actor = actor->next;
                        }
                    }

                    Math_ApproachZeroF(&prop->rotX, 0.05f, 0.05f);
                } else if (prop->type == FS_PROP_LILY_PAD) {
                    Fishing_HandleLilyPadContact(prop, &player->actor.world.pos, 0);

                    actor = play->actorCtx.actorLists[ACTORCAT_NPC].head;
                    while (actor != NULL) {
                        if (!((actor->id == ACTOR_FISHING) && (actor->params >= EN_FISH_PARAM))) {
                            actor = actor->next;
                        } else {
                            Fishing_HandleLilyPadContact(prop, &actor->world.pos, ((Fishing*)actor)->lilyTimer);
                            actor = actor->next;
                        }
                    }

                    Math_ApproachS(&prop->lilyPadAngle, 0, 20, 80);
                    prop->pos.y =
                        (Math_SinS(prop->timer * 0x1000) * prop->lilyPadOffset) + (WATER_SURFACE_Y(play) + 2.0f);
                    Math_ApproachZeroF(&prop->lilyPadOffset, 0.1f, 0.02f);
                }
            }
        }

        prop++;
    }

    if (sSubCamId == 0) {
        CollisionCheck_SetOC(play, &play->colChkCtx, &sFishingMain->collider.base);
    }
}

void Fishing_DrawPondProps(PlayState* play) {
    u8 materialFlag = 0;
    FishingProp* prop = &sPondProps[0];
    s16 i;
    s32 pad;

    OPEN_DISPS(play->state.gfxCtx);

    Matrix_Push();

    for (i = 0; i < POND_PROP_COUNT; i++) {
        if (prop->type == FS_PROP_REED) {
            if (materialFlag == 0) {
                gSPDisplayList(POLY_XLU_DISP++, gFishingReedMaterialDL);
                materialFlag++;
            }

            if (prop->shouldDraw) {
                FrameInterpolation_RecordOpenChild(prop, prop->epoch);
                Matrix_Translate(prop->pos.x, prop->pos.y, prop->pos.z, MTXMODE_NEW);
                Matrix_Scale(prop->scale, prop->scale, prop->scale, MTXMODE_APPLY);
                Matrix_RotateY(prop->rotY, MTXMODE_APPLY);
                Matrix_RotateX(prop->rotX, MTXMODE_APPLY);
                Matrix_RotateY(prop->reedAngle, MTXMODE_APPLY);

                gSPMatrix(POLY_XLU_DISP++, MATRIX_NEWMTX(play->state.gfxCtx),
                          G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
                gSPDisplayList(POLY_XLU_DISP++, gFishingReedModelDL);
                FrameInterpolation_RecordCloseChild();
            }
        }

        prop++;
    }

    prop = &sPondProps[0];
    materialFlag = 0;
    for (i = 0; i < POND_PROP_COUNT; i++) {
        if (prop->type == FS_PROP_WOOD_POST) {
            if (materialFlag == 0) {
                gSPDisplayList(POLY_OPA_DISP++, gFishingWoodPostMaterialDL);
                materialFlag++;
            }

            if (prop->shouldDraw) {
                FrameInterpolation_RecordOpenChild(prop, prop->epoch);
                Matrix_Translate(prop->pos.x, prop->pos.y, prop->pos.z, MTXMODE_NEW);
                Matrix_Scale(prop->scale, prop->scale, prop->scale, MTXMODE_APPLY);

                gSPMatrix(POLY_OPA_DISP++, MATRIX_NEWMTX(play->state.gfxCtx),
                          G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
                gSPDisplayList(POLY_OPA_DISP++, gFishingWoodPostModelDL);
                FrameInterpolation_RecordCloseChild();
            }
        }

        prop++;
    }

    prop = &sPondProps[0];
    materialFlag = 0;
    for (i = 0; i < POND_PROP_COUNT; i++) {
        if (prop->type == FS_PROP_LILY_PAD) {
            if (materialFlag == 0) {
                gSPDisplayList(POLY_XLU_DISP++, gFishingLilyPadMaterialDL);
                materialFlag++;
            }

            if (prop->shouldDraw) {
                FrameInterpolation_RecordOpenChild(prop, prop->epoch);
                Matrix_Translate(prop->pos.x, prop->pos.y, prop->pos.z, MTXMODE_NEW);
                Matrix_Scale(prop->scale, 1.0f, prop->scale, MTXMODE_APPLY);
                Matrix_RotateY(prop->lilyPadAngle * (M_PI / 32768), MTXMODE_APPLY);
                Matrix_Translate(0.0f, 0.0f, 20.0f, MTXMODE_APPLY);
                Matrix_RotateY(prop->rotY, MTXMODE_APPLY);

                gSPMatrix(POLY_XLU_DISP++, MATRIX_NEWMTX(play->state.gfxCtx),
                          G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
                gSPDisplayList(POLY_XLU_DISP++, gFishingLilyPadModelDL);
                FrameInterpolation_RecordCloseChild();
            }
        }

        prop++;
    }

    prop = &sPondProps[0];
    materialFlag = 0;
    for (i = 0; i < POND_PROP_COUNT; i++) {
        if (prop->type == FS_PROP_ROCK) {
            if (materialFlag == 0) {
                gSPDisplayList(POLY_OPA_DISP++, gFishingRockMaterialDL);
                materialFlag++;
            }

            if (prop->shouldDraw) {
                FrameInterpolation_RecordOpenChild(prop, prop->epoch);
                Matrix_Translate(prop->pos.x, prop->pos.y, prop->pos.z, MTXMODE_NEW);
                Matrix_Scale(prop->scale, prop->scale, prop->scale, MTXMODE_APPLY);
                Matrix_RotateY(prop->rotY, MTXMODE_APPLY);

                gSPMatrix(POLY_OPA_DISP++, MATRIX_NEWMTX(play->state.gfxCtx),
                          G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
                gSPDisplayList(POLY_OPA_DISP++, gFishingRockModelDL);
                FrameInterpolation_RecordCloseChild();
            }
        }

        prop++;
    }

    Matrix_Pop();

    CLOSE_DISPS(play->state.gfxCtx);
}

void Fishing_UpdateGroupFishes(PlayState* play) {
    s16 groupContactFlags = 0;
    Player* player = GET_PLAYER(play);
    FishingGroupFish* fish = &sGroupFishes[0];
    f32 dy;
    f32 dx;
    f32 dist;
    f32 dz;
    f32 offset;
    s16 groupIndex;
    s16 groupFlag;
    f32 spD8;
    s16 spD6;
    s16 spD4;
    s16 target;
    s16 i;
    Vec3f basePos[3];
    Vec3f ripplePos;
    Vec3f* refPos;
    f32 temp1;
    f32 temp2;

    if ((D_80B7E114 != 0) || (sRodCastState == 4)) {
        refPos = &sLurePos;
    } else {
        refPos = &player->actor.world.pos;
    }

    basePos[0].x = sinf(sFishGroupAngle1) * 720.0f;
    basePos[0].y = -35.0f;
    basePos[0].z = cosf(sFishGroupAngle1) * 720.0f;

    temp1 = refPos->x - basePos[0].x;
    temp2 = refPos->z - basePos[0].z;

    if ((SQ(temp1) + SQ(temp2)) < SQ(50.0f)) {
        sFishGroupAngle1 += 0.3f;
        groupContactFlags |= 1;
    } else if (sFishGroupVar != 0.0f) {
        sFishGroupAngle1 += 0.05f;
        basePos[0].y = WATER_SURFACE_Y(play) - 5.0f;
    } else {
        Math_ApproachF(&sFishGroupAngle1, 0.7f, 1.0f, 0.001f);
    }

    basePos[1].x = sinf(sFishGroupAngle2) * 720.0f;
    basePos[1].y = -35.0f;
    basePos[1].z = cosf(sFishGroupAngle2) * 720.0f;

    temp1 = refPos->x - basePos[1].x;
    temp2 = refPos->z - basePos[1].z;

    if ((SQ(temp1) + SQ(temp2)) < SQ(50.0f)) {
        sFishGroupAngle2 -= 0.3f;
        groupContactFlags |= 2;
    } else if (sFishGroupVar != 0.0f) {
        sFishGroupAngle2 -= 0.05f;
        basePos[1].y = WATER_SURFACE_Y(play) - 5.0f;
    } else {
        Math_ApproachF(&sFishGroupAngle2, 2.3f, 1.0f, 0.001f);
    }

    basePos[2].x = sinf(sFishGroupAngle3) * 720.0f;
    basePos[2].y = -35.0f;
    basePos[2].z = cosf(sFishGroupAngle3) * 720.0f;

    temp1 = refPos->x - basePos[2].x;
    temp2 = refPos->z - basePos[2].z;

    if ((SQ(temp1) + SQ(temp2)) < SQ(50.0f)) {
        sFishGroupAngle3 -= 0.3f;
        groupContactFlags |= 4;
    } else if (sFishGroupVar != 0.0f) {
        sFishGroupAngle3 -= 0.05f;
        basePos[2].y = WATER_SURFACE_Y(play) - 5.0f;
    } else {
        Math_ApproachF(&sFishGroupAngle3, 4.6f, 1.0f, 0.001f);
    }

    if (sLinkAge == LINK_AGE_CHILD) {
        spD8 = 0.8f;
    } else {
        spD8 = 1.0f;
    }

    for (i = 0; i < GROUP_FISH_COUNT; i++) {
        if (fish->type != FS_GROUP_FISH_NONE) {
            fish->timer++;

            SkinMatrix_Vec3fMtxFMultXYZW(&play->viewProjectionMtxF, &fish->pos, &fish->projectedPos, &sProjectedW);

            if ((fish->projectedPos.z < 400.0f) && (fabsf(fish->projectedPos.x) < (100.0f + fish->projectedPos.z))) {
                fish->shouldDraw = true;
            } else {
                fish->shouldDraw = false;
            }

            if (i <= 20) {
                groupIndex = 0;
                groupFlag = 1;
            } else if (i <= 40) {
                groupIndex = 1;
                groupFlag = 2;
            } else {
                groupIndex = 2;
                groupFlag = 4;
            }

            dx = fish->homePos.x - fish->pos.x;
            dy = fish->homePos.y - fish->pos.y;
            dz = fish->homePos.z - fish->pos.z;
            spD4 = Math_Atan2S(dz, dx);
            dist = sqrtf(SQ(dx) + SQ(dz));
            spD6 = Math_Atan2S(dist, dy);

            if ((dist < 10.0f) || (((fish->timer % 32) == 0) && (Rand_ZeroOne() > 0.5f))) {
                fish->homePos.y = basePos[groupIndex].y + Rand_CenteredFloat(10.0f);

                if (sFishGroupVar != 0.0f) {
                    fish->homePos.x = basePos[groupIndex].x + Rand_CenteredFloat(200.0f);
                    fish->homePos.z = basePos[groupIndex].z + Rand_CenteredFloat(200.0f);
                } else {
                    fish->homePos.x = basePos[groupIndex].x + Rand_CenteredFloat(100.0f);
                    fish->homePos.z = basePos[groupIndex].z + Rand_CenteredFloat(100.0f);
                }

                ripplePos = fish->pos;
                ripplePos.y = WATER_SURFACE_Y(play);
                Fishing_SpawnRipple(&fish->projectedPos, sFishingEffects, &ripplePos, 20.0f,
                                    Rand_ZeroFloat(50.0f) + 100.0f, 150, 90);

                if (fish->velY < 1.5f) {
                    fish->velY = 1.5f;
                }

                fish->unk_34 = 1.5f;
                fish->unk_38 = 1.0f;
            }

            target = Fishing_SmoothStepToS(&fish->unk_3E, spD4, 5, 0x4000) * 3.0f;
            if (target > 0x1F40) {
                target = 0x1F40;
            } else if (target < -0x1F40) {
                target = -0x1F40;
            }

            Math_ApproachS(&fish->unk_42, target, 3, 0x1388);

            offset = fish->unk_42 * -0.0001f;
            Math_ApproachS(&fish->unk_3C, spD6, 5, 0x4000);

            if (groupContactFlags & groupFlag) {
                fish->unk_38 = 1.0f;
                fish->velY = 6.0f;
                fish->unk_34 = 2.0f;
            }

            if (sFishGroupVar != 0.0f) {
                fish->unk_38 = 1.0f;
                fish->velY = 4.0f;
                fish->unk_34 = 2.0f;
            }

            Math_ApproachF(&fish->velY, 0.75f, 1.0f, 0.05f);

            temp1 = fish->velY * spD8;
            temp2 = Math_CosS(fish->unk_3C) * temp1;

            fish->pos.x += temp2 * Math_SinS(fish->unk_3E);
            fish->pos.y += temp1 * Math_SinS(fish->unk_3C);
            fish->pos.z += temp2 * Math_CosS(fish->unk_3E);

            if (fish->shouldDraw) {
                Math_ApproachF(&fish->unk_34, 1.0f, 1.0f, 0.1f);
                Math_ApproachF(&fish->unk_38, 0.4f, 1.0f, 0.04f);
                fish->unk_30 += fish->unk_34;
                fish->scaleX = (cosf(fish->unk_30) * fish->unk_38) + offset;
            }
        }

        fish++;
    }

    sFishGroupVar = 0.0f;
}

void Fishing_DrawGroupFishes(PlayState* play) {
    u8 materialFlag = 0;
    FishingGroupFish* fish = &sGroupFishes[0];
    f32 scale;
    s16 i;
    s32 pad;

    if (sLinkAge == LINK_AGE_CHILD) {
        scale = 0.003325f;
    } else {
        scale = 0.00475f;
    }

    OPEN_DISPS(play->state.gfxCtx);

    for (i = 0; i < GROUP_FISH_COUNT; i++) {
        if (fish->type != FS_GROUP_FISH_NONE) {
            if (!materialFlag) {
                gSPDisplayList(POLY_OPA_DISP++, gFishingGroupFishMaterialDL);
                gDPSetPrimColor(POLY_OPA_DISP++, 0, 0, 155, 155, 155, 255);
                materialFlag++;
            }

            if (fish->shouldDraw) {
                FrameInterpolation_RecordOpenChild(fish, fish->epoch);
                Matrix_Translate(fish->pos.x, fish->pos.y, fish->pos.z, MTXMODE_NEW);
                Matrix_RotateY(((f32)fish->unk_3E * M_PI) / 32768.0f, MTXMODE_APPLY);
                Matrix_RotateX((-(f32)fish->unk_3C * M_PI) / 32768.0f, MTXMODE_APPLY);
                Matrix_Scale(fish->scaleX * scale, scale, scale, MTXMODE_APPLY);

                gSPMatrix(POLY_OPA_DISP++, MATRIX_NEWMTX(play->state.gfxCtx),
                          G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
                gSPDisplayList(POLY_OPA_DISP++, gFishingGroupFishModelDL);
                FrameInterpolation_RecordCloseChild();
            }
        }
        fish++;
    }

    CLOSE_DISPS(play->state.gfxCtx);
}

static u16 sPondOwnerTextIds[] = { 0x4096, 0x408D, 0x408E, 0x408F, 0x4094, 0x4095 };

void Fishing_HandleOwnerDialog(Fishing* this, PlayState* play) {
    switch (this->stateAndTimer) {
        case 0:
            if (sFishingPlayingState == 0) {
                if (sLinkAge != LINK_AGE_CHILD) {
                    if ((HIGH_SCORE(HS_FISHING) & HS_FISH_PLAYED_CHILD) &&
                        !(HIGH_SCORE(HS_FISHING) & HS_FISH_PLAYED_ADULT)) {
                        this->actor.textId = 0x4093;
                    } else {
                        this->actor.textId = 0x407B;
                    }
                } else {
                    this->actor.textId = 0x407B;
                }
            } else if (!sIsOwnersHatHooked) {
                this->actor.textId = 0x4084;
            } else {
                this->actor.textId = 0x4097;
            }

            if (Actor_ProcessTalkRequest(&this->actor, play)) {
                if (sFishingPlayingState == 0) {
                    this->stateAndTimer = 1;
                    if (sLinkAge != LINK_AGE_CHILD) {
                        HIGH_SCORE(HS_FISHING) |= HS_FISH_PLAYED_ADULT;
                    } else {
                        HIGH_SCORE(HS_FISHING) |= HS_FISH_PLAYED_CHILD;
                    }
                } else {
                    this->stateAndTimer = 10;
                }
            } else {
                Actor_OfferTalk(&this->actor, play, 100.0f);
            }
            break;

        case 1:
            if ((Message_GetState(&play->msgCtx) == TEXT_STATE_CHOICE) && Message_ShouldAdvance(play)) {
                Message_CloseTextbox(play);

                switch (play->msgCtx.choiceIndex) {
                    case 0:
                        if (gSaveContext.rupees >= 20) {
                            Rupees_ChangeBy(-20);
                            if (func_800AA148() == 0) {
                                this->actor.textId = 0x407C;
                            } else {
                                this->actor.textId = 0x407D;
                            }
                            Message_ContinueTextbox(play, this->actor.textId);
                            this->stateAndTimer = 2;
                        } else {
                            Message_ContinueTextbox(play, 0x407E);
                            this->stateAndTimer = 3;
                        }
                        break;
                    case 1:
                        Message_ContinueTextbox(play, 0x2D);
                        this->stateAndTimer = 3;
                        break;
                }
                // Fix for owner getting stuck when the pond is closed (Fishing Pole Shuffle)
            } else if (Message_GetState(&play->msgCtx) == TEXT_STATE_DONE && Message_ShouldAdvance(play)) {
                Message_CloseTextbox(play);
                this->stateAndTimer = 0;
            }
            break;

        case 2:
            if ((Message_GetState(&play->msgCtx) == TEXT_STATE_EVENT) && Message_ShouldAdvance(play)) {
                Message_CloseTextbox(play);
                Message_ContinueTextbox(play, 0x407F);
                this->stateAndTimer = 4;
            }
            break;

        case 3:
            if ((Message_GetState(&play->msgCtx) == TEXT_STATE_EVENT) && Message_ShouldAdvance(play)) {
                Message_CloseTextbox(play);
                this->stateAndTimer = 0;
            }
            if (Message_GetState(&play->msgCtx) == TEXT_STATE_DONE) {
                this->stateAndTimer = 0;
            }
            break;

        case 4:
            if ((Message_GetState(&play->msgCtx) == TEXT_STATE_CHOICE) && Message_ShouldAdvance(play)) {
                Message_CloseTextbox(play);

                switch (play->msgCtx.choiceIndex) {
                    case 0:
                        sFishLengthToWeigh = sFishingRecordLength;
                        RF3Size_SetLengthToWeigh(sFishLengthToWeigh, RF3_WEIGH_RECORD, sLinkAge == LINK_AGE_CHILD); // steps 12b/14d: weigh-in shows RF3 length
                        Message_ContinueTextbox(play, 0x4080);
                        this->stateAndTimer = 5;
                        break;
                    case 1:
                        Message_ContinueTextbox(play, 0x407F);
                        break;
                }
            }
            break;

        case 5:
            if ((Message_GetState(&play->msgCtx) == TEXT_STATE_EVENT) && Message_ShouldAdvance(play)) {
                Message_CloseTextbox(play);

                play->interfaceCtx.unk_260 = 1;
                play->startPlayerFishing(play);
                RF3Rod_SetOwned(); // Reel Fishing III mod (step 15a): the first paid rental buys the rod
                sFishingPlayingState = 1;
                sOwnerTheftTimer = 20;
                this->stateAndTimer = 0;

                if ((HIGH_SCORE(HS_FISHING) & 0xFF0000) < 0xFF0000) {
                    HIGH_SCORE(HS_FISHING) += HS_FISH_PLAYED;
                }
            }
            break;

        case 10:
            if (sIsOwnersHatHooked) { // owner asks for hat back
                if ((Message_GetState(&play->msgCtx) == TEXT_STATE_CHOICE) && Message_ShouldAdvance(play)) {
                    Message_CloseTextbox(play);

                    switch (play->msgCtx.choiceIndex) {
                        case 0:
                            Message_ContinueTextbox(play, 0x40B2);
                            sOwnerHair = FS_OWNER_CAPPED;
                            sIsOwnersHatHooked = false;
                            this->stateAndTimer = 20;
                            break;
                        case 1:
                            this->stateAndTimer = 0;
                            break;
                    }
                }
            } else {
                if ((Message_GetState(&play->msgCtx) == TEXT_STATE_CHOICE) && Message_ShouldAdvance(play)) {
                    Message_CloseTextbox(play);

                    switch (play->msgCtx.choiceIndex) {
                        case 0:
                            if (sFishOnHandLength == 0.0f) {
                                this->actor.textId = 0x408C;
                                this->stateAndTimer = 20;
                            } else if (sFishOnHandIsLoach == 0) {
                                sFishLengthToWeigh = sFishOnHandLength;
                                RF3Size_SetLengthToWeigh(sFishLengthToWeigh, RF3_WEIGH_ON_HAND, sLinkAge == LINK_AGE_CHILD); // steps 12b/14d: weigh-in shows RF3 length
                                // if we set the record or get the reward
                                if (GameInteractor_Should(VB_SHOULD_CHECK_FOR_FISHING_RECORD,
                                                          (s16)sFishingRecordLength < (s16)sFishOnHandLength,
                                                          &sFishOnHandLength)) {
                                    if (sLureCaughtWith == FS_LURE_SINKING) {
                                        this->actor.textId = 0x40B0;
                                    } else {
                                        this->actor.textId = 0x4086;
                                    }
                                    this->stateAndTimer = 11;
                                } else {
                                    this->actor.textId = 0x408B;
                                    this->stateAndTimer = 20;
                                }
                            } else {
                                this->actor.textId = 0x409B;
                                this->stateAndTimer = 11;
                            }
                            Message_ContinueTextbox(play, this->actor.textId);
                            break;
                        case 1:
                            if (sFishingTimePlayed > 36000) {
                                sFishingTimePlayed = 30000;
                                Message_ContinueTextbox(play, 0x4088);
                            } else {
                                if (sFishingFoggy == 0) {
                                    if (sPondOwnerTextIdIndex == 0) {
                                        sPondOwnerTextIdIndex++;
                                    }
                                }

                                if ((sLureEquipped == FS_LURE_SINKING) &&
                                    (sPondOwnerTextIds[sPondOwnerTextIdIndex] == 0x408D)) {
                                    Message_ContinueTextbox(play, 0x40AF);
                                } else {
                                    Message_ContinueTextbox(play, sPondOwnerTextIds[sPondOwnerTextIdIndex]);
                                }

                                sPondOwnerTextIdIndex++;

                                if (sLinkAge != LINK_AGE_CHILD) {
                                    if (sPondOwnerTextIdIndex >= 6) {
                                        sPondOwnerTextIdIndex = 0;
                                    }
                                } else {
                                    if (sPondOwnerTextIdIndex >= 4) {
                                        sPondOwnerTextIdIndex = 0;
                                    }
                                }
                            }
                            this->stateAndTimer = 0;
                            break;
                        case 2:
                            if (sFishesCaught == 0) {
                                Message_ContinueTextbox(play, 0x4085);
                            } else if (sLinkAge == LINK_AGE_CHILD) {
                                Message_ContinueTextbox(play, 0x4092);
                            }
                            this->stateAndTimer = 22;
                            break;
                    }
                }
            }
            break;

        case 11: // collect prize, update record.
            if (((Message_GetState(&play->msgCtx) == TEXT_STATE_EVENT) ||
                 (Message_GetState(&play->msgCtx) == TEXT_STATE_NONE)) &&
                Message_ShouldAdvance(play)) {
                s32 getItemId;
                GetItemEntry getItemEntry = (GetItemEntry)GET_ITEM_NONE;

                Message_CloseTextbox(play);
                // Declare and fill a struct for use in hooks
                struct VBFishingData fishData;
                fishData.actor = this;
                fishData.sFishOnHandIsLoach = &sFishOnHandIsLoach;
                fishData.sSinkingLureLocation = &sSinkingLureLocation;
                fishData.fishWeight = sFishOnHandLength;
                fishData.sFishOnHandLength = &sFishOnHandLength;
                fishData.sFishingRecordLength = sFishingRecordLength;

                if (sFishOnHandIsLoach == 0) {
                    // if we set the record. Needed because of the wider check at VB_SHOULD_CHECK_FOR_FISHING_RECORD
                    // if not, reset sFishOnHandLength here as we carry it in the struct anyway
                    if (GameInteractor_Should(VB_SHOULD_SET_FISHING_RECORD, true, &fishData)) {
                        sFishingRecordLength = sFishOnHandLength;
                        sFishOnHandLength = 0.0f; // if skipped here, is set at the end of the function. needs to be
                                                  // kept for later checks

                        if (sLinkAge == LINK_AGE_CHILD) {
                            f32 temp;

                            HIGH_SCORE(HS_FISHING) &= 0xFFFFFF00;
                            HIGH_SCORE(HS_FISHING) |= (s32)sFishingRecordLength & HS_FISH_LENGTH_CHILD;
                            RF3Size_SetRecord(true, sFishingRecordLength); // step 14d

                            temp = (HIGH_SCORE(HS_FISHING) & HS_FISH_LENGTH_ADULT) >> 0x18;
                            if (temp < sFishingRecordLength) {
                                HIGH_SCORE(HS_FISHING) &= 0xFFFFFF;
                                HIGH_SCORE(HS_FISHING) |= ((s32)sFishingRecordLength & HS_FISH_LENGTH_CHILD) << 0x18;
                                RF3Size_SetRecord(false, sFishingRecordLength); // step 14d

                                if (sLureCaughtWith == FS_LURE_SINKING) {
                                    HIGH_SCORE(HS_FISHING) |= HS_FISH_CHEAT_ADULT;
                                }
                            }

                            if (sLureCaughtWith == FS_LURE_SINKING) {
                                HIGH_SCORE(HS_FISHING) |= HS_FISH_CHEAT_CHILD;
                                this->stateAndTimer = 0;
                                break;
                            }
                        } else {
                            HIGH_SCORE(HS_FISHING) &= 0xFFFFFF;
                            HIGH_SCORE(HS_FISHING) |= ((s32)sFishingRecordLength & HS_FISH_LENGTH_CHILD) << 0x18;
                            RF3Size_SetRecord(false, sFishingRecordLength); // step 14d

                            if (sLureCaughtWith == FS_LURE_SINKING) {
                                HIGH_SCORE(HS_FISHING) |= HS_FISH_CHEAT_ADULT;
                                this->stateAndTimer = 0;
                                break;
                            }
                        }
                    }

                    if (sFishingRecordLength >= 60.0f) { // 13 lbs
                        getItemId = GI_RUPEE_PURPLE;
                    } else if (sFishingRecordLength >= 50.0f) { // 9 lbs
                        getItemId = GI_RUPEE_RED;
                    } else if (sFishingRecordLength >= 40.0f) { // 6 lbs
                        getItemId = GI_RUPEE_BLUE;
                    } else {
                        getItemId = GI_RUPEE_GREEN;
                    }

                    if (sLinkAge == LINK_AGE_CHILD) { // 9 lbs
                        // if we should give the main prize AND it's not rando
                        if (GameInteractor_Should(VB_SHOULD_GIVE_VANILLA_FISHING_PRIZE,
                                                  (sFishingRecordLength >= 50.0f) &&
                                                      !(HIGH_SCORE(HS_FISHING) & HS_FISH_PRIZE_CHILD),
                                                  &fishData)) {
                            HIGH_SCORE(HS_FISHING) |= HS_FISH_PRIZE_CHILD;
                            getItemId = GI_HEART_PIECE;
                            sSinkingLureLocation = (u8)Rand_ZeroFloat(3.999f) + 1;
                        }
                    } else { // 13 lbs
                        // if we should give the main prize AND it's not rando
                        if (GameInteractor_Should(VB_SHOULD_GIVE_VANILLA_FISHING_PRIZE,
                                                  (sFishingRecordLength >= 60.0f) &&
                                                      !(HIGH_SCORE(HS_FISHING) & HS_FISH_PRIZE_ADULT),
                                                  &fishData)) {
                            HIGH_SCORE(HS_FISHING) |= HS_FISH_PRIZE_ADULT;
                            getItemId = GI_SCALE_GOLDEN;
                            sSinkingLureLocation = (u8)Rand_ZeroFloat(3.999f) + 1;
                        }
                    }
                } else {
                    getItemId = GI_RUPEE_PURPLE;
                    sFishOnHandLength = 0.0f; // doesn't record loach
                }

                this->actor.parent = NULL;

                // if we should give the check in rando
                if (!GameInteractor_Should(VB_GIVE_RANDO_FISHING_PRIZE, false, &fishData)) {
                    Actor_OfferGetItem(&this->actor, play, getItemId, 2000.0f, 1000.0f);
                    this->stateAndTimer = 23;
                }
            }
            break;

        case 20:
            if ((Message_GetState(&play->msgCtx) == TEXT_STATE_EVENT) && Message_ShouldAdvance(play)) {
                Message_CloseTextbox(play);
                this->stateAndTimer = 0;
            }
            break;

        case 21:
            if ((Message_GetState(&play->msgCtx) == TEXT_STATE_CHOICE) && Message_ShouldAdvance(play)) {
                Message_CloseTextbox(play);

                switch (play->msgCtx.choiceIndex) {
                    case 0:
                        this->stateAndTimer = 0;
                        break;
                    case 1:
                        if (sFishesCaught == 0) {
                            Message_ContinueTextbox(play, 0x4085);
                        } else if (sLinkAge == LINK_AGE_CHILD) {
                            Message_ContinueTextbox(play, 0x4092);
                        }
                        this->stateAndTimer = 22;
                        break;
                }
            }
            break;

        case 22:
            if (play) {}

            if (Message_GetState(&play->msgCtx) == TEXT_STATE_NONE) {
                this->stateAndTimer = 0;
                if (sIsOwnersHatHooked) {
                    sOwnerHair = FS_OWNER_CAPPED;
                    sIsOwnersHatHooked = false;
                }
                sFishingPlayingState = 0;
                play->interfaceCtx.unk_260 = 0;
            }
            break;

        case 23:
            sIsRodVisible = false;
            if (Actor_HasParent(&this->actor, play)) {
                this->stateAndTimer = 24;
            } else {
                if (!GameInteractor_Should(VB_GIVE_RANDO_GLITCH_FISHING_PRIZE, false, this)) {
                    Actor_OfferGetItem(&this->actor, play, GI_SCALE_GOLDEN, 2000.0f, 1000.0f);
                }
            }
            break;

        case 24:
            sIsRodVisible = false;
            if ((Message_GetState(&play->msgCtx) == TEXT_STATE_DONE) && Message_ShouldAdvance(play)) {
                if (sFishOnHandIsLoach == 0) {
                    this->stateAndTimer = 0;
                } else {
                    Message_StartTextbox(play, 0x409C, NULL);
                    this->stateAndTimer = 20;
                }
            }
            break;
    }
}

static s16 sOwnerBlinkIndex[] = { 0, 1, 2, 2, 1 };

static Vec3f sStreamSoundPos = { 670.0f, 0.0f, -600.0f };

static Vec3s sSinkingLureLocationPos[] = {
    { -364, -30, -269 }, // in the pond, log past the lilies.
    { 1129, 3, -855 },   // rock next to stream
    { -480, 0, -1055 },  // wall opposite of entrance
    { 553, -48, -508 },  // tip of log beside 3 posts
};

// #region SOH [Enhancement]
void Fishing_QuitAtDoor(Fishing* this, PlayState* play) {
    if ((Message_GetState(&play->msgCtx) == TEXT_STATE_CHOICE) && Message_ShouldAdvance(play)) {
        Message_CloseTextbox(play);

        switch (play->msgCtx.choiceIndex) {
            case 0:
                if (sFishesCaught == 0) {
                    Message_ContinueTextbox(play, 0x4085);
                } else if (sLinkAge == 1) {
                    Message_ContinueTextbox(play, 0x4092);
                }

                if (Message_GetState(&play->msgCtx) == TEXT_STATE_DONE_FADING) {

                    if (sIsOwnersHatHooked != 0) {
                        sOwnerHair = 1;
                        sIsOwnersHatHooked = 0;
                    }
                    sFishingPlayingState = 0;
                    play->interfaceCtx.unk_260 = 0;
                }
                break;
            case 1:
                func_800A9F6C(0.0f, 150, 10, 10);
                break;
        }
    }
}
// #endregion

// Reel Fishing III mod: one CSV row of the rod, line, lure and hooked fish (owner or portable).
// Step 43a: the lure landing in the water (cast state 2 -> 3), for the lure conditions' bite timing.
static void Fishing_RF3WatchCast(PlayState* play) {
    static s16 sPrevCastState = 0;
    if ((sRodCastState == 3) && (sPrevCastState == 2)) {
        RF3Stock_NoteLureLanded(play->gameplayFrames);
    }
    sPrevCastState = sRodCastState;
}

static void Fishing_RF3LogFrame(PlayState* play, Input* input, Player* player) {
    if (RF3Log_IsEnabled()) {
        RF3OwnerFrame logFrame = { 0 };

        logFrame.frame = play->gameplayFrames;
        logFrame.castState = sRodCastState;
        logFrame.lureEquipped = sLureEquipped;
        logFrame.lineHooked = sLineHooked;
        logFrame.reelLock = sReelLock;
        logFrame.rodPullback = sRodPullback;
        logFrame.reelSpeed = sRodReelingSpeed;
        logFrame.lineSpooled = sRodLineSpooled;
        logFrame.lurePos[0] = sLurePos.x;
        logFrame.lurePos[1] = sLurePos.y;
        logFrame.lurePos[2] = sLurePos.z;
        logFrame.rodTipPos[0] = sRodTipPos.x;
        logFrame.rodTipPos[1] = sRodTipPos.y;
        logFrame.rodTipPos[2] = sRodTipPos.z;
        logFrame.waterSurfaceY = WATER_SURFACE_Y(play);
        logFrame.stickX = input->rel.stick_x;
        logFrame.stickY = input->rel.stick_y;
        logFrame.buttons = input->cur.button;
        logFrame.linkPos[0] = player->actor.world.pos.x;
        logFrame.linkPos[1] = player->actor.world.pos.y;
        logFrame.linkPos[2] = player->actor.world.pos.z;
        // Same guard the game uses before touching sFishingHookedFish; it is never cleared.
        if (((sRodCastState == 4) || (sRodCastState == 5)) && (sFishingHookedFish != NULL)) {
            logFrame.hasFish = 1;
            logFrame.fishState = sFishingHookedFish->fishState;
            logFrame.fishStateNext = sFishingHookedFish->fishStateNext;
            logFrame.fishLength = sFishingHookedFish->fishLength;
            logFrame.fishPos[0] = sFishingHookedFish->actor.world.pos.x;
            logFrame.fishPos[1] = sFishingHookedFish->actor.world.pos.y;
            logFrame.fishPos[2] = sFishingHookedFish->actor.world.pos.z;
            logFrame.fishSpeedXZ = sFishingHookedFish->actor.speedXZ;
            logFrame.fishFloorY = sFishingHookedFish->actor.floorHeight;
            logFrame.fishYaw = sFishingHookedFish->actor.world.rot.y;
            logFrame.fishTailStep = sFishingHookedFish->fishLimbRotPhaseStep;
            logFrame.fishTailMag = sFishingHookedFish->fishLimbRotPhaseMag;
            logFrame.fishBgFlags = sFishingHookedFish->actor.bgCheckFlags; // step 26e-6
            logFrame.fishBumpTimer = sFishingHookedFish->bumpTimer;
            // Fishing_RF3PointInWater needs the play state and the fish's region, which is only set during the fish's
            // own update (step 26e-6b: without it every row read 0, the pond's radius test)
            sRF3MovePlay = play;
            sRF3CurRegion = RF3_IS_REGION_FISH(sFishingHookedFish)
                                ? &sRF3RegionBoxes[sFishingHookedFish->actor.params - EN_FISH_REGION]
                                : NULL;
            logFrame.fishInWater =
                Fishing_RF3PointInWater(sFishingHookedFish->actor.world.pos.x, sFishingHookedFish->actor.world.pos.z);
            sRF3CurRegion = NULL;
        } else {
            logFrame.fishInWater = -1;
        }
        logFrame.sceneNum = play->sceneNum; // step 15c
        logFrame.portable = sRF3Portable;
        logFrame.lureInWater = sRF3Portable ? sRF3PortableInWater : !Fishing_RF3OnLand(SQ(sLurePos.x) + SQ(sLurePos.z));
        logFrame.lureFloorY = sRF3PortableFloorY;
        RF3Log_OwnerFrame(&logFrame);
    }
}

// The pond's camera and cinematic states: the lure camera (Z changes the view), reeling in, returning
// the rod, the sinking lure, the catch. Moved out of Fishing_UpdateOwner unchanged so the portable rod
// (step 15e) gets the same lure camera; outside the pond only states 1-3 are ever set.
static void Fishing_UpdateCinematics(Fishing* this, PlayState* play) {
    Vec3f multiVecSrc;
    Vec3f eyeTarget;
    Vec3f lureDist;
    f32 subCamAtMaxVelFrac;
    f32 lureDistXZ;
    Player* player = GET_PLAYER(play);
    Input* input = &play->state.input[0];

    if (sFishingCinematicTimer != 0) {
        sFishingCinematicTimer--;
    }

    switch (sFishingPlayerCinematicState) {
        case 0:
            break;

        case 1: {
            Camera* mainCam;

            sSubCamId = Play_CreateSubCamera(play);
            Play_ChangeCameraStatus(play, CAM_ID_MAIN, CAM_STAT_WAIT);
            Play_ChangeCameraStatus(play, sSubCamId, CAM_STAT_ACTIVE);
            mainCam = Play_GetCamera(play, CAM_ID_MAIN);
            sCameraEye.x = mainCam->eye.x;
            sCameraEye.y = mainCam->eye.y;
            sCameraEye.z = mainCam->eye.z;
            sCameraAt.x = mainCam->at.x;
            sCameraAt.y = mainCam->at.y;
            sCameraAt.z = mainCam->at.z;
            sFishingPlayerCinematicState = 2;
            Interface_ChangeHudVisibilityMode(12);
            sSubCamVelFactor = 0.0f;
            // fallthrough
        }

        case 2:
            Letterbox_SetSizeTarget(0x1B);

            lureDist.x = sLurePos.x - player->actor.world.pos.x;
            lureDist.z = sLurePos.z - player->actor.world.pos.z;
            lureDistXZ = sqrtf(SQ(lureDist.x) + SQ(lureDist.z));
            Matrix_RotateY(Math_Atan2F(lureDist.z, lureDist.x), MTXMODE_NEW);

            multiVecSrc.x = 0.0f;
            multiVecSrc.y = 0.0f;
            multiVecSrc.z = 100.0f;
            Matrix_MultVec3f(&multiVecSrc, &lureDist);

            if (sRodCastState == 1) {
                subCamAtMaxVelFrac = 0.2f;
            } else {
                subCamAtMaxVelFrac = 0.1f;
            }

            Math_ApproachF(&sCameraAt.x, sLurePos.x, subCamAtMaxVelFrac, fabsf(lureDist.x) * sSubCamVelFactor);
            Math_ApproachF(&sCameraAt.y, sLurePos.y, subCamAtMaxVelFrac, 50.0f * sSubCamVelFactor);
            Math_ApproachF(&sCameraAt.z, sLurePos.z, subCamAtMaxVelFrac, fabsf(lureDist.z) * sSubCamVelFactor);

            multiVecSrc.x = 0.0f - D_80B7FED0;
            if (sLinkAge != LINK_AGE_CHILD) {
                multiVecSrc.y = 80.0f;
            } else {
                multiVecSrc.y = 55.0f;
            }
            multiVecSrc.z = -80.0f;

            Matrix_MultVec3f(&multiVecSrc, &eyeTarget);
            eyeTarget.x += player->actor.world.pos.x;
            eyeTarget.y += player->actor.world.pos.y;
            eyeTarget.z += player->actor.world.pos.z;

            Math_ApproachF(&D_80B7FED0, 30.0f, 0.1f, 0.4f);

            if (CHECK_BTN_ALL(input->press.button, BTN_Z)) { // zoom in/out from the lure
                if ((sLureCameraZoomLevel >= 0) && (sLureBitTimer == 0)) {
                    sLureCameraZoomLevel++;

                    if (sLureCameraZoomLevel >= 4) {
                        sLureCameraZoomLevel = 0;
                    }

                    if ((sLureCameraZoomLevel == 0) || (sLureCameraZoomLevel == 3)) {
                        Sfx_PlaySfxCentered(NA_SE_SY_CAMERA_ZOOM_DOWN);
                    } else {
                        Sfx_PlaySfxCentered(NA_SE_SY_CAMERA_ZOOM_UP);
                    }
                }
            }

            if (sRodCastState >= 3) {
                if (lureDistXZ < 110.0f) {
                    sLureCameraZoomLevel = -1;
                } else if ((lureDistXZ > 300.0f) && (sLureCameraZoomLevel < 0)) {
                    sLureCameraZoomLevel = 0;
                }
            }

            if (sLureCameraZoomLevel > 0) {
                f32 dist;
                f32 offset;
                f32 factor;

                dist = sqrtf(SQ(lureDist.x) + SQ(lureDist.z)) * 0.001f;
                if (dist > 1.0f) {
                    dist = 1.0f;
                }
                if (sLureCameraZoomLevel == 2) {
                    offset = 0.3f;
                } else {
                    offset = 0.1f;
                }
                factor = 0.4f + offset + (dist * 0.4f);

                eyeTarget.x += (sLurePos.x - eyeTarget.x) * factor;
                eyeTarget.y += ((sLurePos.y - eyeTarget.y) * factor) + 20.0f;
                eyeTarget.z += (sLurePos.z - eyeTarget.z) * factor;
                sFishingLineScale = 0.0005000001f;
            }

            multiVecSrc.x = 0.0f;
            multiVecSrc.y = 0.0f;
            multiVecSrc.z = 100.0f;
            Matrix_MultVec3f(&multiVecSrc, &lureDist);

            Math_ApproachF(&sCameraEye.x, eyeTarget.x, 0.3f, fabsf(lureDist.x) * sSubCamVelFactor);
            Math_ApproachF(&sCameraEye.y, eyeTarget.y, 0.3f, 20.0f * sSubCamVelFactor);
            Math_ApproachF(&sCameraEye.z, eyeTarget.z, 0.3f, fabsf(lureDist.z) * sSubCamVelFactor);
            break;

        case 3: {
            Camera* mainCam = Play_GetCamera(play, CAM_ID_MAIN);

            mainCam->eye = sCameraEye;
            mainCam->eyeNext = sCameraEye;
            mainCam->at = sCameraAt;
            func_800C08AC(play, sSubCamId, 0);
            func_80064534(play, &play->csCtx);
            sFishingPlayerCinematicState = 0;
            sSubCamId = 0;
            Environment_EnableUnderwaterLights(play, 0);
            play->envCtx.adjFogNear = 0;
            player->unk_860 = -5;
            D_80B7E0B0 = 5;
            break;
        }

        case 10: { // owner tells you to return the rod.
            Camera* mainCam;

            func_80064520(play, &play->csCtx);
            sSubCamId = Play_CreateSubCamera(play);
            Play_ChangeCameraStatus(play, CAM_ID_MAIN, CAM_STAT_WAIT);
            Play_ChangeCameraStatus(play, sSubCamId, CAM_STAT_ACTIVE);
            Player_SetCsActionWithHaltedActors(play, &this->actor, 5);
            mainCam = Play_GetCamera(play, CAM_ID_MAIN);
            sCameraEye.x = mainCam->eye.x;
            sCameraEye.y = mainCam->eye.y;
            sCameraEye.z = mainCam->eye.z;
            sCameraAt.x = mainCam->at.x;
            sCameraAt.y = mainCam->at.y;
            sCameraAt.z = mainCam->at.z;
            Message_StartTextbox(play, 0x409E, NULL);
            sFishingPlayerCinematicState = 11;
            func_800A9F6C(0.0f, 150, 10, 10);
            // fallthrough
        }

        case 11:
            player->actor.world.pos.z = 1360.0f;
            player->actor.speedXZ = 0.0f;

            // #region SOH [Enhancement]
            if (GameInteractor_Should(VB_FISHING_QUIT_AT_DOOR, false)) {
                Fishing_QuitAtDoor(this, play);
            }
            // #endregion

            if (Message_GetState(&play->msgCtx) == TEXT_STATE_NONE) {
                Camera* mainCam = Play_GetCamera(play, CAM_ID_MAIN);

                mainCam->eye = sCameraEye;
                mainCam->eyeNext = sCameraEye;
                mainCam->at = sCameraAt;
                func_800C08AC(play, sSubCamId, 0);
                func_80064534(play, &play->csCtx);
                Player_SetCsActionWithHaltedActors(play, &this->actor, 7);
                sFishingPlayerCinematicState = 0;
                sSubCamId = 0;
                sFishingCinematicTimer = 30;
                Environment_EnableUnderwaterLights(play, 0);
                play->envCtx.adjFogNear = 0;
            }
            break;

        case 20: { // found the sinking lure
            Camera* mainCam;

            func_80064520(play, &play->csCtx);
            sSubCamId = Play_CreateSubCamera(play);
            Play_ChangeCameraStatus(play, CAM_ID_MAIN, CAM_STAT_WAIT);
            Play_ChangeCameraStatus(play, sSubCamId, CAM_STAT_ACTIVE);
            Player_SetCsActionWithHaltedActors(play, &this->actor, 5);
            mainCam = Play_GetCamera(play, CAM_ID_MAIN);
            sCameraEye.x = mainCam->eye.x;
            sCameraEye.y = mainCam->eye.y;
            sCameraEye.z = mainCam->eye.z;
            sCameraAt.x = mainCam->at.x;
            sCameraAt.y = mainCam->at.y;
            sCameraAt.z = mainCam->at.z;
            Message_StartTextbox(play, 0x409A, NULL);
            sFishingPlayerCinematicState = 21;
            sCatchCamX = 45.0f;
            sFishingCinematicTimer = 10;
            // fallthrough
        }

        case 21:
            if ((sFishingCinematicTimer == 0) && Message_ShouldAdvance(play)) {
                sFishingPlayerCinematicState = 22;
                sFishingCinematicTimer = 40;
                Player_SetCsActionWithHaltedActors(play, &this->actor, 0x1C);
                sSinkingLureHeldY = 0.0f;
            }
            break;

        case 22:
            if (sFishingCinematicTimer == 30) {
                Audio_QueueSeqCmd(SEQ_PLAYER_BGM_MAIN << 24 | NA_BGM_ITEM_GET | 0x900);
            }

            sSinkingLureFound = 1;

            Math_ApproachF(&sSinkingLureHeldY, 71.0f, 0.5f, 3.0f);
            Matrix_RotateY((player->actor.shape.rot.y / 32768.0f) * M_PI, MTXMODE_NEW);

            multiVecSrc.x = Math_SinS(play->gameplayFrames * 0x1000);
            multiVecSrc.y = sSinkingLureHeldY;
            multiVecSrc.z = -5.0f;
            if (sLinkAge == LINK_AGE_CHILD) {
                multiVecSrc.y -= 20.0f;
            }

            Matrix_MultVec3f(&multiVecSrc, &eyeTarget);

            sSinkingLureBasePos.x = player->actor.world.pos.x + eyeTarget.x;
            sSinkingLureBasePos.y = player->actor.world.pos.y + eyeTarget.y;
            sSinkingLureBasePos.z = player->actor.world.pos.z + eyeTarget.z;

            Math_ApproachF(&sCatchCamX, 15.0f, 0.1f, 0.75f);

            multiVecSrc.x = sCatchCamX - 15.0f;

            if (sLinkAge != LINK_AGE_CHILD) {
                multiVecSrc.y = 60.0f;
                multiVecSrc.z = -30.0f;
            } else {
                multiVecSrc.y = 40.0f;
                multiVecSrc.z = -35.0f;
            }

            Matrix_MultVec3f(&multiVecSrc, &sCameraEye);
            sCameraEye.x += player->actor.world.pos.x;
            sCameraEye.y += player->actor.world.pos.y;
            sCameraEye.z += player->actor.world.pos.z;

            sCameraAt = player->actor.world.pos;
            if (sLinkAge != LINK_AGE_CHILD) {
                sCameraAt.y += 62.0f;
            } else {
                sCameraAt.y += 40.0f;
            }

            if (sFishingCinematicTimer == 0) {
                if ((Message_GetState(&play->msgCtx) == TEXT_STATE_CHOICE) ||
                    (Message_GetState(&play->msgCtx) == TEXT_STATE_NONE)) {
                    if (Message_ShouldAdvance(play)) {
                        Camera* mainCam = Play_GetCamera(play, CAM_ID_MAIN);

                        Message_CloseTextbox(play);
                        if (play->msgCtx.choiceIndex == 0) {
                            sLureEquipped = FS_LURE_SINKING;
                            sPondOwnerTextIdIndex = 0;
                        }

                        mainCam->eye = sCameraEye;
                        mainCam->eyeNext = sCameraEye;
                        mainCam->at = sCameraAt;
                        func_800C08AC(play, sSubCamId, 0);
                        func_80064534(play, &play->csCtx);
                        Player_SetCsActionWithHaltedActors(play, &this->actor, 7);
                        sFishingPlayerCinematicState = 0;
                        sSubCamId = 0;
                        player->unk_860 = -5;
                        D_80B7E0B0 = 5;
                        sSinkingLureFound = false;
                        sFishingMusicDelay = 20;
                        Environment_EnableUnderwaterLights(play, 0);
                        play->envCtx.adjFogNear = 0;
                    }
                }
            }
            break;

        case 100:
            break;
    }

    if (sSubCamId != 0) {
        Play_CameraSetAtEye(play, sSubCamId, &sCameraAt, &sCameraEye);
        Math_ApproachF(&sSubCamVelFactor, 1.0f, 1.0f, 0.02f);

        if (sCameraEye.y <= (WATER_SURFACE_Y(play) + 1.0f)) {
            Environment_EnableUnderwaterLights(play, 1);
            if (sFishingFoggy != 0) {
                play->envCtx.adjFogNear = -0xB2;
            } else {
                play->envCtx.adjFogNear = -0x2E;
            }
        } else {
            Environment_EnableUnderwaterLights(play, 0);
            play->envCtx.adjFogNear = 0;
        }
    }
}

void Fishing_UpdateOwner(Actor* thisx, PlayState* play2) {
    PlayState* play = play2;
    Fishing* this = (Fishing*)thisx;
    Vec3f multiVecSrc;
    s16 headRotTarget;
    s16 playerShadowAlpha;
    f32 target;
    s32 pad;
    Player* player = GET_PLAYER(play);
    Input* input = &play->state.input[0];

    if (0) {
        // Strings existing only in rodata
        osSyncPrintf(VT_FGCOL(GREEN));
        osSyncPrintf(VT_FGCOL(YELLOW));
        osSyncPrintf("plays %x\n");
        osSyncPrintf("ys %x\n");
        osSyncPrintf(VT_RST);
    }

    sRF3RodBtn = BTN_B; // step 15c: the pond's rod is always on B

    playerShadowAlpha = player->actor.shape.shadowAlpha;

    if ((SQ(player->actor.world.pos.x) + SQ(player->actor.world.pos.z)) < SQ(920.0f)) {
        Math_ApproachS(&playerShadowAlpha, 0, 1, 40);
    } else {
        Math_ApproachS(&playerShadowAlpha, 200, 1, 40);
    }

    player->actor.shape.shadowAlpha = playerShadowAlpha;

    SkelAnime_Update(&this->skelAnime);

    if ((sOwnerTheftTimer != 0) || (Message_GetState(&play->msgCtx) != TEXT_STATE_NONE)) {
        this->actor.flags &= ~ACTOR_FLAG_ATTENTION_ENABLED;
    } else {
        this->actor.flags |= ACTOR_FLAG_ATTENTION_ENABLED | ACTOR_FLAG_DRAW_CULLING_DISABLED;
    }

    if ((this->actor.xzDistToPlayer < 120.0f) || (Message_GetState(&play->msgCtx) != TEXT_STATE_NONE)) {
        headRotTarget = this->actor.shape.rot.y - this->actor.yawTowardsPlayer;
    } else {
        headRotTarget = 0;
    }

    if (headRotTarget > 0x2710) {
        headRotTarget = 0x2710;
    } else if (headRotTarget < -0x2710) {
        headRotTarget = -0x2710;
    }

    Math_ApproachS(&this->unk_164, headRotTarget, 3, 0x1388);

    if (((play->gameplayFrames % 32) == 0) && (Rand_ZeroOne() < 0.3f)) {
        this->unk_162 = 4;
    }

    this->unk_160 = sOwnerBlinkIndex[this->unk_162];

    if (this->unk_162 != 0) {
        this->unk_162--;
    }

    if (sOwnerTheftTimer != 0) {
        sOwnerTheftTimer--;
    }

    // steal the owner's hat
    if (!sIsOwnersHatHooked && (sLureEquipped != FS_LURE_SINKING) && (sRodCastState > 0) &&
        (sOwnerHair == FS_OWNER_CAPPED) && (sOwnerTheftTimer == 0)) {
        f32 dx = sOwnerHeadPos.x - sLurePos.x;
        f32 dy = sOwnerHeadPos.y - sLurePos.y;
        f32 dz = sOwnerHeadPos.z - sLurePos.z;

        if ((sqrtf(SQ(dx) + SQ(dy) + SQ(dz)) < 25.0f) || (KREG(77) > 0)) {
            KREG(77) = 0;
            sOwnerHair = FS_OWNER_BALD;
            sIsOwnersHatHooked = true;
            Message_StartTextbox(play, 0x4087, NULL);
        }
    }

    // update hat flag.
    if (sOwnerHair == FS_OWNER_BALD) {
        HIGH_SCORE(HS_FISHING) |= HS_FISH_STOLE_HAT;
    } else if (sOwnerHair == FS_OWNER_CAPPED) {
        HIGH_SCORE(HS_FISHING) &= ~HS_FISH_STOLE_HAT;
    }

    if (KREG(77) < 0) {
        KREG(77) = 0;
        sIsOwnersHatSunk = true;
    }

    if (sFishingCaughtTextDelay != 0) {
        sFishingCaughtTextDelay--;
        if (sFishingCaughtTextDelay == 0) {
            Message_StartTextbox(play, sFishingCaughtTextId, NULL);
        }
    }

    Fishing_HandleOwnerDialog(this, play);

    sFishingLineScale = 0.0015f;
    sFishingTimePlayed++;

    if ((sFishingPlayingState != 0) && sIsRodVisible) {
        Fishing_UpdateLure(this, play);
    }

    Fishing_UpdateEffects(sFishingEffects, play);
    Fishing_UpdatePondProps(play);
    Fishing_UpdateGroupFishes(play);
    // can't leave with the rod
    if ((sFishingPlayingState != 0) && (sFishingPlayerCinematicState == 0) && (player->actor.world.pos.z > 1360.0f) &&
        (fabsf(player->actor.world.pos.x) < 25.0f)) {
        player->actor.world.pos.z = 1360.0f;
        player->actor.speedXZ = 0.0f;

        if (sFishingCinematicTimer == 0) {
            sFishingPlayerCinematicState = 10;
        }
    }

    if ((sSinkingLureLocation != 0) &&
        (fabsf(player->actor.world.pos.x - sSinkingLureLocationPos[sSinkingLureLocation - 1].x) < 25.0f) &&
        (fabsf(player->actor.world.pos.y - sSinkingLureLocationPos[sSinkingLureLocation - 1].y) < 10.0f) &&
        (fabsf(player->actor.world.pos.z - sSinkingLureLocationPos[sSinkingLureLocation - 1].z) < 25.0f)) {
        sSinkingLureLocation = 0;
        sFishingPlayerCinematicState = 20;
        func_800A9F6C(0.0f, 150, 10, 10);
        Sfx_PlaySfxCentered(NA_SE_SY_TRE_BOX_APPEAR);
        Audio_QueueSeqCmd(0x1 << 28 | SEQ_PLAYER_BGM_MAIN << 24 | 0x1400FF);
    }

    if (KREG(0) != 0) {
        KREG(0) = 0;
        sLureEquipped = FS_LURE_STOCK;
        sFishingPlayerCinematicState = 20;
        func_800A9F6C(0.0f, 150, 10, 10);
        Sfx_PlaySfxCentered(NA_SE_SY_TRE_BOX_APPEAR);
        Audio_QueueSeqCmd(0x1 << 28 | SEQ_PLAYER_BGM_MAIN << 24 | 0x1400FF);
    }

    Fishing_UpdateCinematics(this, play); // moved into a function, unchanged (step 15e)

    if ((player->actor.floorHeight < (WATER_SURFACE_Y(play) - 3.0f)) &&
        (player->actor.world.pos.y < (player->actor.floorHeight + 3.0f)) && (player->actor.speedXZ > 1.0f) &&
        ((play->gameplayFrames % 2) == 0)) {
        Vec3f pos;

        pos.x = Rand_CenteredFloat(20.0f) + player->actor.world.pos.x;
        pos.z = Rand_CenteredFloat(20.0f) + player->actor.world.pos.z;
        pos.y = player->actor.floorHeight + 5.0f;
        Fishing_SpawnWaterDust(NULL, sFishingEffects, &pos, 0.5f);
    }

    if ((player->actor.floorHeight < WATER_SURFACE_Y(play)) &&
        (player->actor.floorHeight > (WATER_SURFACE_Y(play) - 10.0f)) && (player->actor.speedXZ >= 4.0f) &&
        ((play->gameplayFrames % 4) == 0)) {
        s16 i;

        for (i = 0; i < 10; i++) {
            Vec3f pos;
            Vec3f vel;
            f32 speedXZ;
            f32 angle;

            speedXZ = Rand_ZeroFloat(1.5f) + 1.5f;
            angle = Rand_ZeroFloat(6.28f);

            vel.x = sinf(angle) * speedXZ;
            vel.z = cosf(angle) * speedXZ;
            vel.y = Rand_ZeroFloat(3.0f) + 2.0f;

            pos = player->actor.world.pos;
            pos.x += 2.0f * vel.x;
            pos.y = WATER_SURFACE_Y(play);
            pos.z += 2.0f * vel.z;
            Fishing_SpawnDustSplash(NULL, sFishingEffects, &pos, &vel, Rand_ZeroFloat(0.01f) + 0.020000001f);
        }
    }

    if (sREG(15) != 0) {
        if (sStormStrengthTarget != (sREG(15) - 1)) {
            if (sStormStrengthTarget == 0) {
                play->envCtx.gloomySkyMode = 1;
            } else {
                play->envCtx.gloomySkyMode = 2;
            }
        }

        sStormStrengthTarget = sREG(15) - 1;
    }

    if (sREG(14) == 1) {
        play->envCtx.gloomySkyMode = 1;
    }
    if (sREG(14) == -1) {
        play->envCtx.gloomySkyMode = 2;
    }

    sREG(14) = 0;

    osSyncPrintf(VT_FGCOL(GREEN));
    osSyncPrintf("zelda_time %x\n", ((void)0, gSaveContext.dayTime));
    osSyncPrintf(VT_RST);

    if (sStormChanceTimer >= 2) {
        sStormChanceTimer--;
    }

    if ((sStormChanceTimer == 1) && (Message_GetState(&play->msgCtx) == TEXT_STATE_NONE) &&
        ((sFishingTimePlayed & 0xFFF) == 0xFFF)) {
        sStormChanceTimer = 200;

        if (Rand_ZeroOne() < 0.5f) {
            sStormStrengthTarget = (u8)Rand_ZeroFloat(10.0f) + 5;
            play->envCtx.gloomySkyMode = 1;
        } else {
            sStormStrengthTarget = 0;
            play->envCtx.gloomySkyMode = 2;
        }
    }

    Math_ApproachF(&sStormStrength, sStormStrengthTarget, 1.0f, 0.05f);

    if (sStormStrength > 0.0f) {
        target = (sStormStrength * 0.03f) + 0.8f;
        if (target > 1.2f) {
            target = 1.2f;
        }
        Math_ApproachF(&sStormSfxFreqScale, target, 1.0f, 0.01f);
    }

    target = (10.0f - sStormStrength) * 150.1f;
    if (target < 0.0f) {
        target = 0.0f;
    }
    Math_ApproachF(&sFishingStormSfxPos.z, target, 1.0f, 5.0f);

    if (sFishingStormSfxPos.z < 1500.0f) {
        func_800F436C(&sFishingStormSfxPos, NA_SE_EV_RAIN - SFX_FLAG, sStormSfxFreqScale);
    }

    if (sStormStrengthTarget != 0) {
        Math_ApproachF(&sFishingStormShade, -200.0f, 1.0f, 2.0f);
    } else {
        Math_ApproachZeroF(&sFishingStormShade, 1.0f, 2.0f);
    }

    play->envCtx.adjLight1Color[0] = play->envCtx.adjLight1Color[1] = play->envCtx.adjLight1Color[2] =
        sFishingStormShade;

    if ((u8)sStormStrength > 0) {
        s32 pad;
        Camera* mainCam = Play_GetCamera(play, CAM_ID_MAIN);
        s16 i;
        s32 pad1;
        Vec3f pos;
        Vec3f rot;
        Vec3f projectedPos;
        s32 pad2;

        rot.x = M_PI / 2.0f + 0.1f;
        rot.y = 1.0f;
        rot.z = (Camera_GetInputDirYaw(mainCam) * -(M_PI / 32768)) + rot.y;

        for (i = 0; i < (u8)sStormStrength; i++) {
            pos.x = Rand_CenteredFloat(700.0f) + play->view.eye.x;
            pos.y = (Rand_ZeroFloat(100.0f) + 150.0f) - 170.0f;
            pos.z = Rand_CenteredFloat(700.0f) + play->view.eye.z;

            if (pos.z < 1160.0f) {
                SkinMatrix_Vec3fMtxFMultXYZW(&play->viewProjectionMtxF, &pos, &projectedPos, &sProjectedW);

                if (projectedPos.z < 0.0f) {
                    i--;
                } else {
                    Fishing_SpawnRainDrop(sFishingEffects, &pos, &rot);
                }
            }
        }
    }

    SkinMatrix_Vec3fMtxFMultXYZW(&play->viewProjectionMtxF, &sStreamSoundPos, &sStreamSoundProjectedPos, &sProjectedW);

    Sfx_PlaySfxAtPos(&sStreamSoundProjectedPos, NA_SE_EV_WATER_WALL - SFX_FLAG);

    gSaveContext.minigameScore = (SQ((f32)sFishLengthToWeigh) * 0.0036f) + 0.5f;

    if (BREG(26) != 0) {
        BREG(26) = 0;
        Message_StartTextbox(play, 0x407B + BREG(27), NULL);
    }

    // Reel Fishing III mod (step 10): the shared lure action meter and every fish's memory of it.
    if (RF3_IsEnabled()) {
        static Vec3f sRF3PrevLurePos = { 0.0f, 0.0f, 0.0f };
        f32 lureMoved = sqrtf(SQ(sLurePos.x - sRF3PrevLurePos.x) + SQ(sLurePos.y - sRF3PrevLurePos.y) +
                              SQ(sLurePos.z - sRF3PrevLurePos.z));

        RF3Approach_UpdateMeter(sRodCastState == 3, lureMoved, CHECK_BTN_ALL(input->cur.button, BTN_A), RF3_SUBSTEPS);
        sRF3PrevLurePos = sLurePos;
    }

    Fishing_RF3LogFrame(play, input, player); // Reel Fishing III mod: log this frame. Read-only.
    Fishing_RF3WatchCast(play);               // step 43a

    osSyncPrintf("HI_SCORE = %x\n", HIGH_SCORE(HS_FISHING));
}

s32 Fishing_OwnerOverrideLimbDraw(PlayState* play, s32 limbIndex, Gfx** dList, Vec3f* pos, Vec3s* rot, void* thisx) {
    Fishing* this = (Fishing*)thisx;

    if (limbIndex == 8) { // Head
        rot->x -= this->unk_164;
    }

    return 0;
}

void Fishing_OwnerPostLimbDraw(PlayState* play, s32 limbIndex, Gfx** dList, Vec3s* rot, void* thisx) {
    if (limbIndex == 8) { // Head
        OPEN_DISPS(play->state.gfxCtx);
        Matrix_MultVec3f(&sZeroVec, &sOwnerHeadPos);

        if (sOwnerHair == FS_OWNER_CAPPED) {
            gSPDisplayList(POLY_OPA_DISP++, SEGMENTED_TO_VIRTUAL(gFishingOwnerHatDL));
        } else if (sOwnerHair == FS_OWNER_HAIR) {
            gSPDisplayList(POLY_OPA_DISP++, SEGMENTED_TO_VIRTUAL(gFishingOwnerHairDL));
        }

        CLOSE_DISPS(play->state.gfxCtx);
    }
}

static void* sFishingOwnerEyeTexs[] = {
    gFishingOwnerEyeOpenTex,
    gFishingOwnerEyeHalfTex,
    gFishingOwnerEyeClosedTex,
};

void Fishing_DrawOwner(Actor* thisx, PlayState* play) {
    s32 pad;
    Fishing* this = (Fishing*)thisx;
    Input* input = &play->state.input[0];

    OPEN_DISPS(play->state.gfxCtx);

    Gfx_SetupDL_25Opa(play->state.gfxCtx);
    Gfx_SetupDL_25Xlu(play->state.gfxCtx);

    if ((this->actor.projectedPos.z < 1500.0f) &&
        (fabsf(this->actor.projectedPos.x) < (100.0f + this->actor.projectedPos.z))) {
        gSPSegment(POLY_OPA_DISP++, 0x08, SEGMENTED_TO_VIRTUAL(sFishingOwnerEyeTexs[this->unk_160]));

        SkelAnime_DrawSkeletonOpa(play, &this->skelAnime, Fishing_OwnerOverrideLimbDraw, Fishing_OwnerPostLimbDraw,
                                  this);
    }

    Fishing_DrawPondProps(play);
    Fishing_DrawEffects(sFishingEffects, play);
    Fishing_DrawGroupFishes(play);
    Fishing_DrawStreamSplash(play);

    if (sFishingMusicDelay != 0) {
        sFishingMusicDelay--;

        if (sFishingMusicDelay == 0) {
            if (sLinkAge != LINK_AGE_CHILD) {
                Audio_QueueSeqCmd(SEQ_PLAYER_BGM_MAIN << 24 | NA_BGM_KAKARIKO_ADULT);
            } else {
                Audio_QueueSeqCmd(SEQ_PLAYER_BGM_MAIN << 24 | NA_BGM_KAKARIKO_KID);
            }

            if (sLinkAge != LINK_AGE_CHILD) {
                Audio_QueueSeqCmd(SEQ_PLAYER_BGM_MAIN << 24 | NA_BGM_KAKARIKO_ADULT);
            } else {
                Audio_QueueSeqCmd(SEQ_PLAYER_BGM_MAIN << 24 | NA_BGM_KAKARIKO_KID);
            }
        }
    }

    if ((sFishingPlayingState != 0) && sIsRodVisible) {
        Fishing_DrawRod(play);
        Fishing_UpdateLinePos(sReelLinePos);
        Fishing_UpdateLine(play, &sRodTipPos, sReelLinePos, sReelLineRot, sReelLineUnk);
        Fishing_DrawLureAndLine(play, sReelLinePos, sReelLineRot);

        sStickAdjXPrev = input->rel.stick_x;
        sStickAdjYPrev = input->rel.stick_y;
    }

    sIsRodVisible = true;

    Matrix_Translate(130.0f, 40.0f, 1300.0f, MTXMODE_NEW);
    Matrix_Scale(0.08f, 0.12f, 0.14f, MTXMODE_APPLY);

    gSPMatrix(POLY_OPA_DISP++, MATRIX_NEWMTX(play->state.gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    gSPMatrix(POLY_XLU_DISP++, MATRIX_NEWMTX(play->state.gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);

    gSPDisplayList(POLY_OPA_DISP++, gFishingAquariumBottomDL);
    gSPDisplayList(POLY_XLU_DISP++, gFishingAquariumContainerDL);

    if ((sFishingPlayingState != 0) && (sLureEquipped == 2)) {
        Fishing_DrawSinkingLure(play);
    }

    CLOSE_DISPS(play->state.gfxCtx);
}

void Fishing_Reset(void) {
    // Reset static variables for fishing camera and cinematic state to prevent crashing when dying
    // or re-entering the scene while the fishing rod was cast
    sSubCamId = 0;
    sFishingPlayerCinematicState = 0;
}

// --- Reel Fishing III mod (step 15c): the portable rod ---------------------------------------------

// The button holding the rod outside the pond: a D-pad button (step 15b), else B.
static u16 Fishing_RF3RodButton(void) {
    static const u16 sDpadButtons[4] = { BTN_DUP, BTN_DDOWN, BTN_DLEFT, BTN_DRIGHT };
    s32 i;

    if (gSaveContext.equips.buttonItems[0] == ITEM_FISHING_POLE) {
        return BTN_B;
    }
    for (i = 0; i < 4; i++) {
        if (gSaveContext.equips.buttonItems[4 + i] == ITEM_FISHING_POLE) {
            return sDpadButtons[i];
        }
    }
    return BTN_B;
}

// Step 15e: while the portable rod runs, B holds the rod (it casts with B, as at the pond) and B's own
// item waits here. Restored when the rod is put away or the instance ends.
static u8 sRF3StashedBItem = ITEM_NONE;
static u8 sRF3BSwapped = false;

static void Fishing_RF3RodOnB(PlayState* play) {
    if (gSaveContext.equips.buttonItems[0] != ITEM_FISHING_POLE) {
        sRF3StashedBItem = gSaveContext.equips.buttonItems[0];
        sRF3BSwapped = true;
        // Step 22c fix: also where the pond keeps B's item (z_parameter.c). Closing the pause menu
        // (func_80084BF4) puts B back from buttonStatus[0] when B holds the rod; without this it held
        // BTN_ENABLED (0) and B became item 0 (Deku Stick).
        gSaveContext.buttonStatus[0] = sRF3StashedBItem;
        gSaveContext.equips.buttonItems[0] = ITEM_FISHING_POLE;
        Interface_LoadItemIcon1(play, 0);
    }
}

static void Fishing_RF3RestoreB(PlayState* play) {
    if (sRF3BSwapped && (gSaveContext.equips.buttonItems[0] == ITEM_FISHING_POLE)) {
        gSaveContext.equips.buttonItems[0] = sRF3StashedBItem;
        if (sRF3StashedBItem != ITEM_NONE) {
            Interface_LoadItemIcon1(play, 0);
        }
    }
    if (sRF3BSwapped) {
        gSaveContext.buttonStatus[0] = BTN_ENABLED; // step 22c fix: B's status again, not a parked item
    }
    sRF3BSwapped = false;
}

// Water and ground under the lure, once per frame before the lure moves.
static void Fishing_RF3ProbeUnderLure(PlayState* play) {
    CollisionPoly* poly;
    WaterBox* waterBox;
    Vec3f probe = sLurePos;
    f32 surface;

    probe.y += 50.0f;
    sRF3PortableFloorY = BgCheck_EntityRaycastFloor1(&play->colCtx, &poly, &probe);
    if (WaterBox_GetSurface1(play, &play->colCtx, sLurePos.x, sLurePos.z, &surface, &waterBox) &&
        (surface > sRF3PortableFloorY)) {
        sRF3PortableInWater = true;
        sRF3PortableSurfaceY = surface;
    } else {
        sRF3PortableInWater = false;
        sRF3PortableSurfaceY = sRF3PortableFloorY - 50.0f; // no water: "below the ground", as off the pond's banks
    }
}

static void Fishing_RF3UpdatePortable(Actor* thisx, PlayState* play) {
    Fishing* this = (Fishing*)thisx;
    Player* player = GET_PLAYER(play);
    Input* input = &play->state.input[0];
    s16 prevCinematicState;

    // Put away with the line in: this instance ends (it is spawned again when the rod comes out).
    if ((player->heldItemAction != PLAYER_IA_FISHING_POLE) && (sRodCastState == 0)) {
        Actor_Kill(thisx);
        return;
    }

    // Step 22c fix (user, 2026-10-08): something (after a menu) put B's own item back while Link still holds the
    // rod, so B swung that item. While the rod is out, B holds the rod; the item on B now waits in the stash.
    if ((player->heldItemAction == PLAYER_IA_FISHING_POLE) &&
        (gSaveContext.equips.buttonItems[0] != ITEM_FISHING_POLE)) {
        RF3Rod_LogEvent("portable-b-reapplied", gSaveContext.equips.buttonItems[0]);
        Fishing_RF3RodOnB(play);
    }

    sRF3RodBtn = Fishing_RF3RodButton();
    Fishing_RF3ProbeUnderLure(play);

    sFishingLineScale = 0.0015f;
    sFishingTimePlayed++;
    Fishing_UpdateLure(this, play);
    Fishing_UpdateEffects(sFishingEffects, play);

    // Step 16d: RF3's lure action meter, as the owner runs it in the pond, so region fish notice the lure.
    if (RF3_IsEnabled()) {
        static Vec3f sRF3PortablePrevLurePos = { 0.0f, 0.0f, 0.0f };
        f32 lureMoved = sqrtf(SQ(sLurePos.x - sRF3PortablePrevLurePos.x) + SQ(sLurePos.y - sRF3PortablePrevLurePos.y) +
                              SQ(sLurePos.z - sRF3PortablePrevLurePos.z));

        RF3Approach_UpdateMeter(sRodCastState == 3, lureMoved, CHECK_BTN_ALL(input->cur.button, BTN_A), RF3_SUBSTEPS);
        sRF3PortablePrevLurePos = sLurePos;
    }

    // Step 16e: the owner's caught/escaped text delay, and the music: after a fight or catch, the
    // scene's own music comes back (the pond's owner would start Kakariko's).
    if (sFishingCaughtTextDelay != 0) {
        sFishingCaughtTextDelay--;
        if (sFishingCaughtTextDelay == 0) {
            Message_StartTextbox(play, sFishingCaughtTextId, NULL);
        }
    }
    if (sFishingMusicDelay != 0) {
        sFishingMusicDelay--;
        if (sFishingMusicDelay == 0) {
            Audio_QueueSeqCmd(SEQ_PLAYER_BGM_MAIN << 24 | play->sequenceCtx.seqId);
        }
    }

    // Step 15e: the pond's lure camera. In the pond the first fish to update starts it a few frames
    // after the lure leaves the rod (D_80B7E0B2 == 1); there are no fish here, so start it the same way.
    if ((sRodCastState != 0) && (D_80B7E0B2 == 1) && (sFishingPlayerCinematicState == 0)) {
        sFishingPlayerCinematicState = 1;
        D_80B7FED0 = 0.0f;
        sLureCameraZoomLevel = 2;
    }
    // The owner's states 1-3: follow the lure (Z changes the view), then on reel-in hand Link back
    // his normal stance (player->unk_860 = -5).
    prevCinematicState = sFishingPlayerCinematicState;
    Fishing_UpdateCinematics(this, play);
    if ((prevCinematicState != 0) && (sFishingPlayerCinematicState == 0)) {
        Interface_ChangeHudVisibilityMode(HUD_VISIBILITY_ALL); // the pond keeps its own HUD; restore ours
    }

    Fishing_RF3LogFrame(play, input, player);
    Fishing_RF3WatchCast(play); // step 43a
}

static void Fishing_RF3DrawPortable(Actor* thisx, PlayState* play) {
    Input* input = &play->state.input[0];

    OPEN_DISPS(play->state.gfxCtx);

    Gfx_SetupDL_25Opa(play->state.gfxCtx);
    Gfx_SetupDL_25Xlu(play->state.gfxCtx);

    Fishing_DrawEffects(sFishingEffects, play);

    Fishing_DrawRod(play);
    Fishing_UpdateLinePos(sReelLinePos);
    Fishing_UpdateLine(play, &sRodTipPos, sReelLinePos, sReelLineRot, sReelLineUnk);
    Fishing_DrawLureAndLine(play, sReelLinePos, sReelLineRot);

    sStickAdjXPrev = input->rel.stick_x;
    sStickAdjYPrev = input->rel.stick_y;

    // Step 40a (user: "The sinking lure isn't visible outside the pond"): the pond draws the equipped sinking lure from
    // Fishing_DrawOwner; away from the pond only this draw runs, so it draws it too.
    if (sLureEquipped == FS_LURE_SINKING) {
        Fishing_DrawSinkingLure(play);
    }

    CLOSE_DISPS(play->state.gfxCtx);
}

static void Fishing_RF3InitPortable(Fishing* this, PlayState* play) {
    s16 i;

    Actor_ChangeCategory(play, &play->actorCtx, &this->actor, ACTORCAT_MISC);
    this->actor.flags &= ~(ACTOR_FLAG_ATTENTION_ENABLED | ACTOR_FLAG_FRIENDLY);
    this->actor.flags |= ACTOR_FLAG_DRAW_CULLING_DISABLED;
    this->actor.update = Fishing_RF3UpdatePortable;
    this->actor.draw = Fishing_RF3DrawPortable;

    sRF3Portable = true;
    sRF3PortableInWater = false;
    sFishingPlayingState = 1; // Fishing_UpdateLure resets the rod, line and lure on its first frame
    sFishingPlayerCinematicState = 0;
    sSubCamId = 0;
    sIsOwnersHatHooked = false;
    sIsOwnersHatSunk = false;
    sLineHooked = 0;
    sReelLock = 0;
    sLureBitTimer = 0;
    sFishingCaughtTextDelay = 0;
    sFishingHookedFish = NULL;
    for (i = 0; i < FISHING_EFFECT_COUNT; i++) {
        sFishingEffects[i].type = FS_EFF_NONE;
    }
    Fishing_RF3RodOnB(play); // step 15e
    RF3Rod_LogEvent("portable-start", play->sceneNum);
}

static void Fishing_RF3DestroyPortable(PlayState* play) {
    sRF3Portable = false;
    sFishingPlayingState = 0;
    sRodCastState = 0;
    Fishing_RF3RestoreB(play); // step 15e
    if (sSubCamId != 0) {
        Interface_ChangeHudVisibilityMode(HUD_VISIBILITY_ALL);
    }
    Fishing_Reset(); // no camera or cinematic state carries over (as on death / scene change)
    RF3Rod_LogEvent("portable-end", play->sceneNum);
}

// Called from RF3Rod.cpp (step 15c).
s32 Fishing_RF3PortableActive(void) {
    return sRF3Portable;
}

// Step 15e: the rod's D-pad button while the rod is out: B gets its item back. Only with the line in.
// Returns true if the caller should put the rod away.
s32 Fishing_RF3PutAwayRod(PlayState* play) {
    if (!sRF3Portable || (sRodCastState != 0)) {
        return false;
    }
    Fishing_RF3RestoreB(play);
    return true;
}

// Link holds the rod outside the pond: start the portable instance if none runs.
void Fishing_RF3SpawnPortable(PlayState* play) {
    Player* player = GET_PLAYER(play);

    if (sRF3Portable || (play->sceneNum == SCENE_FISHING_POND)) {
        return;
    }
    Actor_Spawn(&play->actorCtx, play, ACTOR_FISHING, player->actor.world.pos.x, player->actor.world.pos.y,
                player->actor.world.pos.z, 0, 0, 0, EN_FISH_PORTABLE);
}

// Link's own rod at the pond (D-pad): start a session as the paid rental does, without paying.
// Returns true if a session started.
s32 Fishing_RF3StartOwnRod(PlayState* play) {
    if ((play->sceneNum != SCENE_FISHING_POND) || (sFishingPlayingState != 0) ||
        (play->interfaceCtx.unk_260 != 0) || (Message_GetState(&play->msgCtx) != TEXT_STATE_NONE)) {
        return false;
    }
    play->interfaceCtx.unk_260 = 1;
    play->startPlayerFishing(play);
    sFishingPlayingState = 1;
    sOwnerTheftTimer = 20;
    return true;
}

// --- Reel Fishing III mod (step 16d): region fish ------------------------------------------------

static s32 Fishing_RF3LureInBiteArea(void) {
    if (sRF3CurRegion != NULL) {
        // Step 16f: inside the box (plus margin) and over water deep enough, not on the shore shelf.
        return !Fishing_RF3OutOfArea(sLurePos.x, sLurePos.z) && sRF3PortableInWater &&
               (sRF3PortableFloorY < (sRF3CurRegion->surfaceY - RF3_REGION_MIN_DEPTH));
    }
    return (SQ(sLurePos.x) + SQ(sLurePos.z)) < SQ(800.0f);
}

// Step 46b: the current region fish's slot (the region code knows its shape).
static s32 Fishing_RF3CurSlot(void) {
    return (s32)(sRF3CurRegion - sRF3RegionBoxes);
}

// Beyond the fishing area: the pond's 800 from its centre, or 100 outside the region's outline (step 46b; was its
// box).
static s32 Fishing_RF3OutOfArea(f32 x, f32 z) {
    if (sRF3CurRegion != NULL) {
        return RF3RegionShape_OutsideOutline(Fishing_RF3CurSlot(), x, z) > 100.0f;
    }
    return sqrtf(SQ(x) + SQ(z)) > 800.0f;
}

// The bridge's fish index: pond fish 0..31 (params - 100), region fish 32 + slot.
static s32 Fishing_RF3FishIndex(Fishing* this) {
    return RF3_IS_REGION_FISH(this) ? (32 + this->actor.params - EN_FISH_REGION) : (this->actor.params - EN_FISH_PARAM);
}

// Step 29h (RF3-MECHANICS 2.5/2.6, DESIGN "29h details"): an RF3 region fish's movement while it chases the bait
// (OoT's state 2). CHASE: straight at the bait, slowing near it, stopping when its mouth reaches it. WAIT (RF3's
// inspect, 0x001A1FD0): backs off 0.1 m (0.15 m if longer than 0.2 m) over the WAIT, still facing the bait [code].
// Sets fishTargetPos, rotationStep and speedXZ; returns whether it reached the bait (RF3Approach_Chase's input).
static s32 sRF3WaitFish = -1; // the fish and WAIT count the back-off speed was set for
static s32 sRF3WaitCount = -1;
static f32 sRF3WaitSpeed = 0.0f; // units per game frame
static s32 sRF3ApproachLogFish = -1;
static s32 sRF3ApproachLogMode = -1;
static s32 sRF3ApproachTick = 0; // step 29h-2: calls since the last periodic approach line
// step 29h-4: the lure's own movement per game frame (without the current), measured between frames
static Vec3f sRF3PrevLurePos;
static u32 sRF3PrevLureFrame = 0;
static f32 sRF3LureVelX = 0.0f;
static f32 sRF3LureVelZ = 0.0f;

static s32 Fishing_RF3ApproachMove(Fishing* this, PlayState* play) {
    RF3ApproachStatus ap;
    RF3RegionFishInfo info;
    s32 me = Fishing_RF3FishIndex(this);
    f32 lengthM = 0.3f; // [assumed] if the region slot has no RF3 length
    f32 reach;
    f32 dx;
    f32 dz;
    f32 dxz;
    f32 dy;
    s32 waiting;
    s32 mode;

    if (RF3RegionFish_Get(this->actor.params - EN_FISH_REGION, &info) && (info.mm > 0.0f)) {
        lengthM = info.mm * 0.001f;
    }
    // the centre within half the body + 0.05 m [assumed] of the bait: the mouth is at it
    reach = (lengthM * 0.5f + 0.05f) * RF3_UNITS_PER_METRE;

    RF3Approach_GetStatus(&ap);
    waiting = (ap.chaser == me) && (ap.chaserMode == 3);

    this->fishTargetPos.x = sLurePos.x;
    this->fishTargetPos.z = sLurePos.z;
    if ((ap.rodKind == 2) || (sLureEquipped == FS_LURE_SINKING)) {
        this->fishTargetPos.y = sLurePos.y; // bait and sinking lures: the bait itself
    } else {
        this->fishTargetPos.y = sLurePos.y - 15.0f; // floating lures: just under it, as OoT
    }
    if (this->fishTargetPos.y <= this->actor.floorHeight) {
        this->fishTargetPos.y = this->actor.floorHeight + 3.0f;
    }

    // 29h-3: the resting bait drifts with the region's current (step 20c-4, about 0.6 units/frame in the user's log),
    // so the chasing fish drifts with it, the same amount the bait got this frame. Relative to the bait, the chase
    // and the back-off then work as in still water (29h-2 log: the fish held a fixed gap at the bait's own speed and
    // never reached it).
    f32 driftX = 0.0f;
    f32 driftZ = 0.0f;
    if (RF3Lure_IsActive() && (sRodCastState == 3)) {
        f32 curX;
        f32 curZ;
        if (RF3Current_At(!sRF3Portable, play->sceneNum, sLurePos.x, sLurePos.z, &curX, &curZ)) {
            f32 share = RF3Lure_DriftScale() * (RF3_SUBSTEPS / 60.0f);
            driftX = curX * share;
            driftZ = curZ * share;
            this->actor.world.pos.x += driftX;
            this->actor.world.pos.z += driftZ;
        }
    }
    // 29h-4: the lure's own speed (reeling, twitches), without the current the fish already shares. Measured once per
    // game frame; a jump (a new cast) or a gap in frames counts as still.
    if (play->gameplayFrames != sRF3PrevLureFrame) {
        f32 mx = sLurePos.x - sRF3PrevLurePos.x - driftX;
        f32 mz = sLurePos.z - sRF3PrevLurePos.z - driftZ;
        if ((play->gameplayFrames - sRF3PrevLureFrame == 1) && ((SQ(mx) + SQ(mz)) < SQ(20.0f))) {
            sRF3LureVelX = mx;
            sRF3LureVelZ = mz;
        } else {
            sRF3LureVelX = sRF3LureVelZ = 0.0f;
        }
        sRF3PrevLurePos = sLurePos;
        sRF3PrevLureFrame = play->gameplayFrames;
    }

    dx = this->fishTargetPos.x - this->actor.world.pos.x;
    dz = this->fishTargetPos.z - this->actor.world.pos.z;
    dy = this->fishTargetPos.y - this->actor.world.pos.y;
    dxz = sqrtf(SQ(dx) + SQ(dz));

    Math_ApproachF(&this->rotationStep, 8192.0f, 1.0f, 2048.0f); // turns to face the bait quickly

    if (waiting) {
        if ((sRF3WaitFish != me) || (sRF3WaitCount != ap.chaserWaits)) {
            f32 backM = (lengthM > 0.2f) ? 0.15f : 0.1f;
            f32 frames = ap.chaserWaitLeft * 20.0f; // game frames are 1/20 s
            if (frames < 1.0f) {
                frames = 1.0f;
            }
            sRF3WaitFish = me;
            sRF3WaitCount = ap.chaserWaits;
            sRF3WaitSpeed = backM * RF3_UNITS_PER_METRE / frames;
        }
        // 29h-2: straight back horizontally, as RF3 (its back-off direction is flattened). A negative speedXZ moved
        // along the fish's pitch too, so a fish facing down at the bait rose as it backed off, pitched further down
        // and rose more (log: 12 -> 32-40 units away, 23 above the bait after one WAIT).
        this->actor.speedXZ = 0.0f;
        if (dxz > 0.01f) {
            this->actor.world.pos.x -= dx / dxz * sRF3WaitSpeed;
            this->actor.world.pos.z -= dz / dxz * sRF3WaitSpeed;
        }
    } else {
        // 29h-4: the lure's own speed away from the fish, plus closing in (3 units/frame, slowing over the last 20
        // units [assumed]). Without the first part a reeled lure kept the fish at the gap where the closing speed
        // equalled the lure's (user: "the fish just followed until I got to shore").
        f32 away = (dxz > 0.01f) ? ((sRF3LureVelX * dx + sRF3LureVelZ * dz) / dxz) : 0.0f;
        f32 want = 0.0f;
        if (away < 0.0f) {
            away = 0.0f; // coming toward the fish: closing in alone does it
        }
        if (dxz > reach) {
            want = (dxz - reach) * 0.15f;
            if (want > 3.0f) {
                want = 3.0f;
            } else if (want < 0.3f) {
                want = 0.3f;
            }
        }
        want += away;
        if (want > 4.5f) {
            want = 4.5f; // [assumed] as OoT's strike lunge
        }
        Math_ApproachF(&this->actor.speedXZ, want, 1.0f, 0.3f);
    }

    mode = waiting ? 3 : 1;
    sRF3ApproachTick++;
    if ((me == sRF3ApproachLogFish) && (mode == sRF3ApproachLogMode) && (sRF3ApproachTick >= 20)) {
        // 29h-2: once a second during the approach, to see where a chase stalls
        char line[260];
        s16 yawErr = (s16)(Math_Atan2S(dz, dx) - this->actor.world.rot.y);
        sRF3ApproachTick = 0;
        snprintf(line, sizeof(line),
                 "slot=%d rf3-approach-tick mode=%s dist_xz=%.1f dy=%.1f reach=%.1f speed=%.2f yaw_err=%.0f "
                 "pitch=%.0f fish_y=%.1f floor=%.1f bait=(%.0f,%.1f,%.0f) lure_vel=%.2f",
                 (s32)(this->actor.params - EN_FISH_REGION), waiting ? "WAIT" : "CHASE", dxz, dy, reach,
                 this->actor.speedXZ, yawErr * (360.0f / 65536.0f), this->actor.world.rot.x * (360.0f / 65536.0f),
                 this->actor.world.pos.y, this->actor.floorHeight, sLurePos.x, sLurePos.y, sLurePos.z,
                 sqrtf(SQ(sRF3LureVelX) + SQ(sRF3LureVelZ)));
        RF3FishMove_Log(line);
    }
    if ((me != sRF3ApproachLogFish) || (mode != sRF3ApproachLogMode)) {
        char line[200];
        sRF3ApproachLogFish = me;
        sRF3ApproachLogMode = mode;
        sRF3ApproachTick = 0;
        snprintf(line, sizeof(line),
                 "slot=%d rf3-approach mode=%s waits=%d dist_xz=%.1f dy=%.1f reach=%.1f length_m=%.2f back=%.3f/frame",
                 (s32)(this->actor.params - EN_FISH_REGION), waiting ? "WAIT" : "CHASE", ap.chaserWaits, dxz, dy,
                 reach, lengthM, waiting ? sRF3WaitSpeed : 0.0f);
        RF3FishMove_Log(line);
    }
    return (dxz < reach) && (fabsf(dy) < 20.0f);
}

static f32 Fishing_RF3Perception(Fishing* this) {
    return RF3_IS_REGION_FISH(this) ? 0.1f : sFishInits[this->actor.params - EN_FISH_PARAM].perception;
}

// OoT's wander target is +-150 around the pond's centre: for a region fish, around the box centre,
// inside the box, and not below the ground.
static void Fishing_RF3RegionTarget(Fishing* this) {
    f32 centreX;
    f32 centreZ;

    if (sRF3CurRegion == NULL) {
        return;
    }
    centreX = (sRF3CurRegion->minX + sRF3CurRegion->maxX) * 0.5f;
    centreZ = (sRF3CurRegion->minZ + sRF3CurRegion->maxZ) * 0.5f;
    // Step 46b: inside the region's shape (out of its holes), 10 units in from an edge it was moved to.
    RF3RegionShape_Clamp(Fishing_RF3CurSlot(), centreX + this->fishTargetPos.x, centreZ + this->fishTargetPos.z, 10.0f,
                         &this->fishTargetPos.x, &this->fishTargetPos.z);
    if (this->fishTargetPos.y < (this->actor.floorHeight + 10.0f)) {
        this->fishTargetPos.y = this->actor.floorHeight + 10.0f;
    }
}

// The hooked fish's steering target stays in the fishing area: the pond's 750 radius (unchanged), or the
// region's box.
// Steps 26e-3/26e-4: is (x, z) open water for a hooked RF3 fish's target? In a region: any water, inside the box
// or not (user, 2026-10-08: "I want fish to be able to swim out of the region when hooked"): a water surface there,
// near the region's, over ground at least RF3_REGION_MIN_DEPTH deep (Fishing_RF3KeepInWater's shore rule). In the
// pond: its 750 radius.
static s32 Fishing_RF3PointInWater(f32 x, f32 z) {
    if (sRF3CurRegion != NULL) {
        f32 surface;
        WaterBox* waterBox;

        if (sRF3MovePlay == NULL) {
            return false;
        }
        if (!WaterBox_GetSurface1(sRF3MovePlay, &sRF3MovePlay->colCtx, x, z, &surface, &waterBox) ||
            (fabsf(surface - sRF3CurRegion->surfaceY) > 20.0f)) {
            return false;
        }
        return Fishing_RF3FloorAt(sRF3MovePlay, x, z) < (sRF3CurRegion->surfaceY - RF3_REGION_MIN_DEPTH);
    }
    return (SQ(x) + SQ(z)) <= SQ(750.0f);
}

// Step 26f-1: RF3's obstacle probe for RF3FishMove (RF3-MECHANICS 6.4, adapted). OoT's line test along the probe
// (walls, floors, ceilings, front faces) stands in for RF3's swept box: 1 with the hit polygon's normal. A clear line
// whose tip isn't open water (the shore probe from +60, which sees the Lake Hylia shelf with no collision under its
// lip) is 2, blocked with no normal. 0 = clear.
static s32 Fishing_RF3Probe(const f32 from[3], const f32 to[3], f32 normal[3]) {
    Vec3f a;
    Vec3f b;
    Vec3f hit;
    CollisionPoly* poly;
    s32 bgId;

    if (sRF3MovePlay == NULL) {
        return 0;
    }
    a.x = from[0];
    a.y = from[1];
    a.z = from[2];
    b.x = to[0];
    b.y = to[1];
    b.z = to[2];
    if (BgCheck_EntityLineTest1(&sRF3MovePlay->colCtx, &a, &b, &hit, &poly, true, true, true, true, &bgId)) {
        normal[0] = COLPOLY_GET_NORMAL(poly->normal.x);
        normal[1] = COLPOLY_GET_NORMAL(poly->normal.y);
        normal[2] = COLPOLY_GET_NORMAL(poly->normal.z);
        return 1;
    }
    // Step 26f-2: also where OoT's own wall check will push the fish: at its check height (the fish lowered by
    // fishLength * 0.1, plus 30), from the fish out past the check's 30 radius. A bank lip there stopped a STRUGGLE fish
    // for 3.5 s while the centre line was clear (rf3_fishing_20261010_032439.csv, frames 4527-4596).
    if (sRF3MoveFish != NULL) {
        f32 dx = b.x - a.x;
        f32 dy = b.y - a.y;
        f32 dz = b.z - a.z;
        f32 len = sqrtf(SQ(dx) + SQ(dy) + SQ(dz));
        f32 ahead;
        Vec3f c;
        Vec3f e;

        if (len > 0.01f) {
            dx /= len;
            dy /= len;
            dz /= len;
            c = sRF3MoveFish->actor.world.pos;
            c.y += RF3_OOT_WALL_HEIGHT - (sRF3MoveFish->fishLength * 0.1f);
            // as far ahead as the RF3 line's tip, but at least past OoT's radius
            ahead = ((b.x - c.x) * dx) + ((b.z - c.z) * dz);
            ahead = MAX(ahead, RF3_OOT_WALL_RADIUS + 5.0f);
            e.x = c.x + (dx * ahead);
            e.y = c.y + (dy * ahead);
            e.z = c.z + (dz * ahead);
            if (BgCheck_EntityLineTest1(&sRF3MovePlay->colCtx, &c, &e, &hit, &poly, true, true, true, true, &bgId)) {
                normal[0] = COLPOLY_GET_NORMAL(poly->normal.x);
                normal[1] = COLPOLY_GET_NORMAL(poly->normal.y);
                normal[2] = COLPOLY_GET_NORMAL(poly->normal.z);
                return 1;
            }
        }
    }
    // Step 26f-3: the land test every RF3_PROBE_LAND_STEP from the fish to the tip, not just at the tip: a thin shelf
    // lip slipped between the centre line (open under it), the OoT-height line (over its top) and the tip (beyond it)
    // (rf3_fishing_20261010_034256.csv, frames 9840-9863; only the shore rule caught it).
    {
        Vec3f start = (sRF3MoveFish != NULL) ? sRF3MoveFish->actor.world.pos : a;
        f32 sx = b.x - start.x;
        f32 sz = b.z - start.z;
        f32 span = sqrtf(SQ(sx) + SQ(sz));
        s32 steps = (s32)(span / RF3_PROBE_LAND_STEP) + 1;
        s32 i;

        for (i = 1; i <= steps; i++) {
            f32 t = (f32)i / steps;

            if (!Fishing_RF3PointInWater(start.x + (sx * t), start.z + (sz * t))) {
                return 2;
            }
        }
    }
    return 0;
}

static void Fishing_RF3ClampFightTarget(Vec3f* target) {
    f32 targetSq;
    f32 scale;

    if (sRF3CurRegion != NULL) { // step 46b: into the region's shape (was its box)
        RF3RegionShape_Clamp(Fishing_RF3CurSlot(), target->x, target->z, 0.0f, &target->x, &target->z);
        return;
    }
    targetSq = SQ(target->x) + SQ(target->z);
    if (targetSq > SQ(750.0f)) {
        scale = 750.0f / sqrtf(targetSq);
        target->x *= scale;
        target->z *= scale;
    }
}

static void Fishing_RF3InitRegionFish(Fishing* this, PlayState* play) {
    s32 slot = this->actor.params - EN_FISH_REGION;
    RF3RegionFishInfo info;

    if ((slot < 0) || (slot >= RF3_REGION_SLOTS) || !RF3RegionFish_Get(slot, &info)) {
        Actor_Kill(&this->actor);
        return;
    }
    SkelAnime_InitFlex(play, &this->skelAnime, &gFishingFishSkel, &gFishingFishAnim, NULL, NULL, 0);
    Animation_MorphToLoop(&this->skelAnime, &gFishingFishAnim, 0.0f);
    SkelAnime_Update(&this->skelAnime);

    sRF3RegionBoxes[slot].minX = info.minX;
    sRF3RegionBoxes[slot].maxX = info.maxX;
    sRF3RegionBoxes[slot].minZ = info.minZ;
    sRF3RegionBoxes[slot].maxZ = info.maxZ;
    sRF3RegionBoxes[slot].surfaceY = info.surfaceY;
    sRF3RegionBoxes[slot].angle = info.angle; // step 16i
    sRF3RegionLastGood[slot] = this->actor.world.pos; // spawned over deep water (RF3RegionFish.cpp)

    this->fishState = 10;
    this->fishStateNext = 10;
    this->isLoach = 0;
    this->perception = 0.1f;
    this->fishLength = info.ootLength;
    this->actor.home.pos = this->actor.world.pos;
}

// Called from RF3RegionFish.cpp: remove this scene's region fish (editor "respawn"). Not while one is
// hooked. Returns how many were removed, or -1 if a fight is running.
// Step 45a: is a fish on the line (cast state 4+)? Region fish are not removed then.
s32 Fishing_RF3LineBusy(void) {
    return sRodCastState >= 4;
}

// Step 45a: kill one region slot's fish (the caller checks Fishing_RF3LineBusy first). Returns how many were killed.
s32 Fishing_RF3KillRegionSlot(PlayState* play, s32 slot) {
    Actor* actor = play->actorCtx.actorLists[ACTORCAT_NPC].head;
    s32 count = 0;

    while (actor != NULL) {
        if ((actor->id == ACTOR_FISHING) && (actor->params == EN_FISH_REGION + slot)) {
            Actor_Kill(actor);
            count++;
        }
        actor = actor->next;
    }
    return count;
}

s32 Fishing_RF3KillRegionFish(PlayState* play) {
    Actor* actor = play->actorCtx.actorLists[ACTORCAT_NPC].head;
    s32 count = 0;

    if (sRodCastState >= 4) {
        return -1;
    }
    while (actor != NULL) {
        if ((actor->id == ACTOR_FISHING) && (actor->params >= EN_FISH_REGION)) {
            Actor_Kill(actor);
            count++;
        }
        actor = actor->next;
    }
    return count;
}

// --- Reel Fishing III mod (step 16f): region fish stay in water --------------------------------------

static f32 Fishing_RF3FloorAt(PlayState* play, f32 x, f32 z) {
    CollisionPoly* poly;
    Vec3f probe;

    probe.x = x;
    // Step 26e-9: from RF3_SHORE_PROBE_HEIGHT above the water (was 10). The Lake Hylia island's shelf top is 15-43
    // above the water and has no collision under its lip, so a probe from +10 started inside it, found the lake bed
    // and called it deep water: hooked fish swam under the island (2026-10-09 CSVs).
    probe.y = sRF3CurRegion->surfaceY + RF3_SHORE_PROBE_HEIGHT;
    probe.z = z;
    return BgCheck_EntityRaycastFloor1(&play->colCtx, &poly, &probe);
}

// Step 26e-8: a hooked region fish's wall test at its own centre height. OoT's fish wall check runs about 24 units above
// the fish (Actor_UpdateBgCheckInfo with wall height 30, after lowering it by fishLength * 0.1), so it misses a shelf
// whose top is just above the water (2026-10-09, the Lake Hylia island: the fish swam through one and under the island).
// A line from the last position to the new one at the fish's centre; on a hit the move keeps only its part along the
// wall (the fish slides along it), RF3_WALL_MARGIN off the wall's plane. If the slid move hits too (a corner), the fish
// stays put. Front faces only, so a fish already inside can still get out. Logs contact start and end to rf3_models.txt.
static void Fishing_RF3WallTest(Fishing* this, PlayState* play) {
    s32 slot;
    Vec3f prev;
    Vec3f next;
    Vec3f hit;
    Vec3f slid;
    Vec3f hit2;
    CollisionPoly* poly;
    CollisionPoly* poly2;
    s32 bgId;
    f32 nx;
    f32 ny;
    f32 nz;
    f32 into;
    f32 planeDist;
    s32 stopped = false;
    char line[320];

    if ((sRF3CurRegion == NULL) || (this->actor.update == NULL)) {
        return;
    }
    slot = this->actor.params - EN_FISH_REGION;
    next = this->actor.world.pos;
    if ((this->fishState != 5) || ((u32)(sRF3WallPrevFrame[slot] + 1) != play->gameplayFrames) ||
        !BgCheck_EntityLineTest1(&play->colCtx, &sRF3WallPrev[slot], &next, &hit, &poly, true, true, true, true,
                                 &bgId)) {
        // not hooked, no update last frame (nothing to test from), or a clear move
        if (sRF3WallContact[slot] != 0) {
            snprintf(line, sizeof(line), "slot=%d rf3-wall-clear after=%d frames fish=(%.0f,%.0f,%.0f)", slot,
                     sRF3WallContact[slot], next.x, next.y, next.z);
            RF3FishMove_Log(line);
            sRF3WallContact[slot] = 0;
        }
        return;
    }
    prev = sRF3WallPrev[slot];
    nx = COLPOLY_GET_NORMAL(poly->normal.x);
    ny = COLPOLY_GET_NORMAL(poly->normal.y);
    nz = COLPOLY_GET_NORMAL(poly->normal.z);
    into = ((next.x - prev.x) * nx) + ((next.y - prev.y) * ny) + ((next.z - prev.z) * nz);
    slid.x = next.x - (nx * into);
    slid.y = next.y - (ny * into);
    slid.z = next.z - (nz * into);
    planeDist = (nx * prev.x) + (ny * prev.y) + (nz * prev.z) + poly->dist; // prev's distance in front of the wall
    if (planeDist < RF3_WALL_MARGIN) {
        slid.x += nx * (RF3_WALL_MARGIN - planeDist);
        slid.y += ny * (RF3_WALL_MARGIN - planeDist);
        slid.z += nz * (RF3_WALL_MARGIN - planeDist);
    }
    if (BgCheck_EntityLineTest1(&play->colCtx, &prev, &slid, &hit2, &poly2, true, true, true, true, &bgId)) {
        slid = prev; // a corner: stay put
        stopped = true;
    }
    if (sRF3WallContact[slot] == 0) {
        snprintf(line, sizeof(line),
                 "slot=%d rf3-wall-cross from=(%.0f,%.0f,%.0f) to=(%.0f,%.0f,%.0f) hit=(%.0f,%.0f,%.0f) "
                 "normal=(%.2f,%.2f,%.2f) ootWall=%d result=%s at=(%.0f,%.0f,%.0f)",
                 slot, prev.x, prev.y, prev.z, next.x, next.y, next.z, hit.x, hit.y, hit.z, nx, ny, nz,
                 (this->actor.bgCheckFlags & BGCHECKFLAG_WALL) ? 1 : 0, stopped ? "stop" : "slide", slid.x, slid.y,
                 slid.z);
        RF3FishMove_Log(line);
    }
    sRF3WallContact[slot]++;
    this->actor.world.pos = slid;
}

// A region box can take in the shore. Over ground shallower than RF3_REGION_MIN_DEPTH the fish goes
// back to its last good spot, and it never rises out of the water (OoT's airborne-fish code would
// throw it). Not while it is being landed (state 6: lifted to Link's hand).
static void Fishing_RF3KeepInWater(Fishing* this, PlayState* play) {
    s32 slot;
    f32 floorY;

    if ((sRF3CurRegion == NULL) || (this->actor.update == NULL) || (this->fishState == 6)) {
        return;
    }
    slot = this->actor.params - EN_FISH_REGION;
    floorY = Fishing_RF3FloorAt(play, this->actor.world.pos.x, this->actor.world.pos.z);
    if (floorY > (sRF3CurRegion->surfaceY - RF3_REGION_MIN_DEPTH)) {
        if ((sRF3ShoreBack[slot] == 0) && (this->fishState == 5)) { // step 26e-9: log a hooked fish's first frame back
            char line[200];

            snprintf(line, sizeof(line), "slot=%d rf3-shore-back fish=(%.0f,%.0f,%.0f) floor=%.0f surface=%.0f to=(%.0f,%.0f)",
                     slot, this->actor.world.pos.x, this->actor.world.pos.y, this->actor.world.pos.z, floorY,
                     sRF3CurRegion->surfaceY, sRF3RegionLastGood[slot].x, sRF3RegionLastGood[slot].z);
            RF3FishMove_Log(line);
        }
        sRF3ShoreBack[slot]++;
        this->actor.world.pos.x = sRF3RegionLastGood[slot].x;
        this->actor.world.pos.z = sRF3RegionLastGood[slot].z;
    } else {
        sRF3ShoreBack[slot] = 0;
        sRF3RegionLastGood[slot] = this->actor.world.pos;
    }
    if (this->actor.world.pos.y > (sRF3CurRegion->surfaceY - 3.0f)) {
        this->actor.world.pos.y = sRF3CurRegion->surfaceY - 3.0f;
        this->actor.velocity.y = 0.0f;
    }
}

// Distance from Link to where the water gets shallow, walking from the hooked fish toward Link.
static f32 Fishing_RF3ShoreDistance(Fishing* this, PlayState* play) {
    Player* player = GET_PLAYER(play);
    f32 dx = player->actor.world.pos.x - this->actor.world.pos.x;
    f32 dz = player->actor.world.pos.z - this->actor.world.pos.z;
    f32 dist = sqrtf(SQ(dx) + SQ(dz));
    f32 step;
    s32 i;

    if (dist < 1.0f) {
        return 0.0f;
    }
    for (i = 0; i < 80; i++) {
        step = i * 15.0f;
        if (step >= dist) {
            break;
        }
        if (Fishing_RF3FloorAt(play, this->actor.world.pos.x + (dx * step / dist),
                               this->actor.world.pos.z + (dz * step / dist)) >
            (sRF3CurRegion->surfaceY - RF3_REGION_MIN_DEPTH)) {
            return (dist - step) + 15.0f; // land just before the shallows
        }
    }
    return 0.0f; // deep water all the way to Link
}

// Step 16h: the rod's cast state (0 = line in), so the region editor's camera doesn't fight the lure camera.
s32 Fishing_RF3CastState(void) {
    return sRodCastState;
}
