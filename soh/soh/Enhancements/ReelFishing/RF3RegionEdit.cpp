// Reel Fishing III mod (step 16h): region edit mode. A cursor on the water (or ground) and an editor
// camera that follows it, so regions are placed and moved without Link. While it runs, the controller
// drives the editor and Link stands still. See docs/DESIGN.md step 16h.
//
// Controls: stick = move the cursor (hold Z: faster); C-Left/Right or right stick X = turn the camera;
// C-Up/Down or right stick Y = zoom; A = corner 1, then corner 2 makes a new region; R = select the
// region under the cursor; B = grab the selected region's corner / edge / whole box, B again = drop;
// START = leave.

#include <algorithm>
#include <cmath>
#include <string>

#include <libultraship/bridge/consolevariablebridge.h>

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"
#include "soh/cvar_prefixes.h"
#include "RF3Regions.h"

extern "C" {
#include "z64.h"
#include "functions.h"
#include "macros.h"
#include "variables.h"
extern PlayState* gPlayState;
s32 Fishing_RF3CastState(void); // z_fishing.c
}

namespace {

enum class Grab { None, Corner, Edge, Box };

bool sActive = false;
Input sPad = {};      // the controller, kept for the editor before the game's copy is cleared
s16 sCam = 0;         // the editor's sub-camera
Vec3f sCursor = {};
bool sCursorOnWater = false;
float sYaw = 0.0f;    // camera direction, radians; the camera looks along (sin yaw, cos yaw)
float sDist = 500.0f; // camera distance from the cursor
constexpr float kPitch = 0.70f; // about 40 degrees down
bool sHasCorner1 = false;
Vec3f sCorner1 = {};
Grab sGrab = Grab::None;
// Step 16i: a grab pins a world corner (sFixedX/Z) and builds the box from it and the cursor, angle kept.
// Corner: the cursor is the opposite corner. Edge: the cursor sets the moving side along one box axis
// (sEdgeAxisX), the other side keeps its length (sEdgeOther). Box: the centre follows the cursor.
float sFixedX = 0.0f, sFixedZ = 0.0f;
bool sEdgeAxisX = false;
float sEdgeOther = 0.0f;
float sGrabOffX = 0.0f, sGrabOffZ = 0.0f; // box move: centre - cursor

bool Pressed(u16 button) {
    return CHECK_BTN_ALL(sPad.press.button, button);
}
bool Held(u16 button) {
    return CHECK_BTN_ALL(sPad.cur.button, button);
}

// The water surface at (x, z) if there is water, else the ground below fromY (or fromY if none).
float SnapY(PlayState* play, float x, float z, float fromY, bool* onWater) {
    float surface;
    WaterBox* waterBox;
    if (WaterBox_GetSurface1(play, &play->colCtx, x, z, &surface, &waterBox)) {
        *onWater = true;
        return surface;
    }
    *onWater = false;
    CollisionPoly* poly;
    Vec3f probe = { x, fromY + 500.0f, z };
    float floor = BgCheck_EntityRaycastFloor1(&play->colCtx, &poly, &probe);
    return (floor > BGCHECK_Y_MIN + 1.0f) ? floor : fromY;
}

void NormalizeBox(rf3::FishRegion& r) {
    if (r.minX > r.maxX) {
        std::swap(r.minX, r.maxX);
    }
    if (r.minZ > r.maxZ) {
        std::swap(r.minZ, r.maxZ);
    }
}

rf3::FishRegion* Selected(PlayState* play) {
    int i = rf3::SelectedRegion();
    std::vector<rf3::FishRegion>& regions = rf3::FishRegions();
    if ((i < 0) || (i >= (int)regions.size()) || (regions[i].scene != play->sceneNum)) {
        return nullptr;
    }
    return &regions[i];
}

void DropGrab() {
    if ((sGrab != Grab::None) && (gPlayState != nullptr)) {
        if (rf3::FishRegion* r = Selected(gPlayState)) {
            NormalizeBox(*r);
        }
    }
    sGrab = Grab::None;
}

void StartGrab(PlayState* play) {
    rf3::FishRegion* r = Selected(play);
    if (r == nullptr) {
        return;
    }
    const float cornerReach = 40.0f;
    const float edgeReach = 30.0f;

    // A corner within reach: pin the opposite one.
    for (int i = 0; i < 4; i++) {
        float x, z;
        r->Corner(i, &x, &z);
        if (std::hypot(sCursor.x - x, sCursor.z - z) < cornerReach) {
            r->Corner((i + 2) % 4, &sFixedX, &sFixedZ);
            sGrab = Grab::Corner;
            return;
        }
    }
    // An edge, in the box's own frame: pin a corner of the opposite edge.
    float bx, bz;
    r->ToBox(sCursor.x, sCursor.z, &bx, &bz);
    bool alongZ = (bz > r->minZ - edgeReach) && (bz < r->maxZ + edgeReach);
    bool alongX = (bx > r->minX - edgeReach) && (bx < r->maxX + edgeReach);
    if (alongZ && (std::fabs(bx - r->minX) < edgeReach)) { // the minX side moves; pin (maxX, minZ)
        r->ToWorld(r->maxX, r->minZ, &sFixedX, &sFixedZ);
        sEdgeAxisX = true;
        sEdgeOther = r->maxZ - r->minZ;
        sGrab = Grab::Edge;
        return;
    }
    if (alongZ && (std::fabs(bx - r->maxX) < edgeReach)) { // maxX moves; pin (minX, minZ)
        r->ToWorld(r->minX, r->minZ, &sFixedX, &sFixedZ);
        sEdgeAxisX = true;
        sEdgeOther = r->maxZ - r->minZ;
        sGrab = Grab::Edge;
        return;
    }
    if (alongX && (std::fabs(bz - r->minZ) < edgeReach)) { // minZ moves; pin (minX, maxZ)
        r->ToWorld(r->minX, r->maxZ, &sFixedX, &sFixedZ);
        sEdgeAxisX = false;
        sEdgeOther = r->maxX - r->minX;
        sGrab = Grab::Edge;
        return;
    }
    if (alongX && (std::fabs(bz - r->maxZ) < edgeReach)) { // maxZ moves; pin (minX, minZ)
        r->ToWorld(r->minX, r->minZ, &sFixedX, &sFixedZ);
        sEdgeAxisX = false;
        sEdgeOther = r->maxX - r->minX;
        sGrab = Grab::Edge;
        return;
    }
    // Inside: the whole box.
    if (r->Contains(sCursor.x, sCursor.z)) {
        sGrab = Grab::Box;
        sGrabOffX = r->CentreX() - sCursor.x;
        sGrabOffZ = r->CentreZ() - sCursor.z;
    }
}

void ApplyGrab(PlayState* play) {
    rf3::FishRegion* r = Selected(play);
    if ((r == nullptr) || (sGrab == Grab::None)) {
        sGrab = Grab::None;
        return;
    }
    if (sGrab == Grab::Box) {
        r->MoveTo(sCursor.x + sGrabOffX, sCursor.z + sGrabOffZ);
    } else if (sGrab == Grab::Corner) {
        r->SetFromCornersWorld(sFixedX, sFixedZ, sCursor.x, sCursor.z);
    } else {
        // The cursor in box axes from the pinned corner; the moving corner keeps the other side's length.
        float ux, uz;
        rf3::FishRegion::Turn(-r->Radians(), sCursor.x - sFixedX, sCursor.z - sFixedZ, &ux, &uz);
        float mx = sEdgeAxisX ? ux : sEdgeOther;
        float mz = sEdgeAxisX ? sEdgeOther : uz;
        float wx, wz;
        rf3::FishRegion::Turn(r->Radians(), mx, mz, &wx, &wz);
        r->SetFromCornersWorld(sFixedX, sFixedZ, sFixedX + wx, sFixedZ + wz);
    }
    rf3::RegionsDirty() = true;
}

void SelectUnderCursor(PlayState* play) {
    std::vector<rf3::FishRegion>& regions = rf3::FishRegions();
    int count = (int)regions.size();
    int start = rf3::SelectedRegion();
    for (int k = 1; k <= count; k++) { // cycle through overlapping regions, starting after the selected one
        int i = (start + k + count) % count;
        if ((regions[i].scene == play->sceneNum) && regions[i].Contains(sCursor.x, sCursor.z)) {
            rf3::SelectedRegion() = i;
            return;
        }
    }
    rf3::SelectedRegion() = -1;
}

void PlaceCorner(PlayState* play) {
    if (!sHasCorner1) {
        sCorner1 = sCursor;
        sHasCorner1 = true;
        return;
    }
    sHasCorner1 = false;
    if ((std::fabs(sCursor.x - sCorner1.x) < 20.0f) || (std::fabs(sCursor.z - sCorner1.z) < 20.0f)) {
        return; // too thin: ignored
    }
    std::vector<rf3::FishRegion>& regions = rf3::FishRegions();
    int here = 0;
    for (const rf3::FishRegion& r : regions) {
        here += (r.scene == play->sceneNum) ? 1 : 0;
    }
    rf3::FishRegion r;
    r.scene = play->sceneNum;
    r.name = "Scene " + std::to_string(play->sceneNum) + " region " + std::to_string(here + 1);
    r.minX = sCorner1.x;
    r.maxX = sCursor.x;
    r.minZ = sCorner1.z;
    r.maxZ = sCursor.z;
    NormalizeBox(r);
    bool water;
    r.surfaceY = SnapY(play, sCorner1.x, sCorner1.z, sCorner1.y, &water);
    regions.push_back(r);
    rf3::SelectedRegion() = (int)regions.size() - 1;
    rf3::RegionsDirty() = true;
}

void UpdateCamera(PlayState* play) {
    if (sCam == 0) {
        sCam = Play_CreateSubCamera(play);
        Play_ChangeCameraStatus(play, CAM_ID_MAIN, CAM_STAT_WAIT);
        Play_ChangeCameraStatus(play, sCam, CAM_STAT_ACTIVE);
    }
    Vec3f at = sCursor;
    Vec3f eye;
    float flat = std::cos(kPitch) * sDist;
    eye.x = at.x - std::sin(sYaw) * flat;
    eye.y = at.y + std::sin(kPitch) * sDist;
    eye.z = at.z - std::cos(sYaw) * flat;
    Play_CameraSetAtEye(play, sCam, &at, &eye);
}

void Update() {
    PlayState* play = gPlayState;
    if (!sActive || (play == nullptr)) {
        return;
    }
    if (Pressed(BTN_START)) {
        rf3::RegionEditStop();
        return;
    }

    // Camera: turn and zoom (C-buttons or the right stick).
    float turn = (Held(BTN_CRIGHT) ? 1.0f : 0.0f) - (Held(BTN_CLEFT) ? 1.0f : 0.0f) + sPad.cur.right_stick_x / 80.0f;
    float zoom = (Held(BTN_CDOWN) ? 1.0f : 0.0f) - (Held(BTN_CUP) ? 1.0f : 0.0f) - sPad.cur.right_stick_y / 80.0f;
    sYaw += turn * 0.05f;
    sDist = std::clamp(sDist * (1.0f + zoom * 0.03f), 100.0f, 2000.0f);

    // Cursor: the stick moves it relative to the camera.
    float sx = sPad.cur.stick_x / 80.0f;
    float sy = sPad.cur.stick_y / 80.0f;
    float speed = sDist * 0.02f * (Held(BTN_Z) ? 3.0f : 1.0f);
    float fx = std::sin(sYaw), fz = std::cos(sYaw); // forward
    float rx = -fz, rz = fx;                        // right
    sCursor.x += (rx * sx + fx * sy) * speed;
    sCursor.z += (rz * sx + fz * sy) * speed;
    sCursor.y = SnapY(play, sCursor.x, sCursor.z, sCursor.y, &sCursorOnWater);

    // Step 16i: D-pad Left/Right turn the selected region (hold Z: fine steps). Not during a corner/edge
    // grab (its pinned corner assumes the angle doesn't change).
    float rot = (Held(BTN_DLEFT) ? 1.0f : 0.0f) - (Held(BTN_DRIGHT) ? 1.0f : 0.0f);
    if ((rot != 0.0f) && (sGrab != Grab::Corner) && (sGrab != Grab::Edge)) {
        if (rf3::FishRegion* r = Selected(play)) {
            r->angle += rot * (Held(BTN_Z) ? 0.2f : 1.0f);
            r->angle = std::remainder(r->angle, 360.0f);
            rf3::RegionsDirty() = true;
        }
    }

    if (sGrab != Grab::None) {
        ApplyGrab(play);
    }
    if (Pressed(BTN_A)) {
        PlaceCorner(play);
    }
    if (Pressed(BTN_R)) {
        SelectUnderCursor(play);
    }
    if (Pressed(BTN_B)) {
        if (sGrab != Grab::None) {
            DropGrab();
        } else {
            StartGrab(play);
        }
    }
    UpdateCamera(play);
}

void RegisterRF3RegionEdit() {
    // After the pad is read, before the frame uses it: the editor keeps the controller, the game gets none.
    COND_HOOK(OnGameStateMainStart, true, []() {
        if (sActive && (gGameState != nullptr)) {
            Input* in = &gGameState->input[0];
            sPad = *in;
            in->cur = {};
            in->press = {};
            in->rel = {};
        }
    });
    COND_HOOK(OnPlayerUpdate, true, []() { Update(); });
    // A scene change ends edit mode (the scene's cameras are gone).
    COND_HOOK(OnSceneInit, true, [](int16_t sceneNum) {
        sActive = false;
        sCam = 0;
        sGrab = Grab::None;
        sHasCorner1 = false;
    });
}

} // namespace

static RegisterShipInitFunc initRF3RegionEdit(RegisterRF3RegionEdit);

namespace rf3 {

bool RegionEditActive() {
    return sActive;
}

bool RegionEditStart(std::string* status) {
    PlayState* play = gPlayState;
    if (play == nullptr) {
        *status = "Load a save file first.";
        return false;
    }
    if (Fishing_RF3CastState() != 0) {
        *status = "Reel the line in first.";
        return false;
    }
    Player* player = GET_PLAYER(play);
    sCursor = player->actor.world.pos;
    sCursor.y = SnapY(play, sCursor.x, sCursor.z, sCursor.y, &sCursorOnWater);
    sYaw = player->actor.shape.rot.y * (M_PI / 32768.0f);
    sDist = 500.0f;
    sHasCorner1 = false;
    sGrab = Grab::None;
    sPad = {};
    sCam = 0;
    sActive = true;
    *status = "Edit mode: START or the button below to leave.";
    return true;
}

void RegionEditStop() {
    DropGrab();
    if ((sCam != 0) && (gPlayState != nullptr)) {
        func_800C08AC(gPlayState, sCam, 0); // clear the sub-camera, back to the main camera (as the pond does)
    }
    sCam = 0;
    sActive = false;
    sHasCorner1 = false;
}

RegionEditView GetRegionEditView() {
    RegionEditView v;
    v.active = sActive;
    v.cursorX = sCursor.x;
    v.cursorY = sCursor.y;
    v.cursorZ = sCursor.z;
    v.cursorOnWater = sCursorOnWater;
    v.hasCorner1 = sHasCorner1;
    v.corner1X = sCorner1.x;
    v.corner1Z = sCorner1.z;
    v.grab = (sGrab == Grab::Corner) ? "corner" : (sGrab == Grab::Edge) ? "edge" : (sGrab == Grab::Box) ? "box" : "";
    return v;
}

} // namespace rf3
