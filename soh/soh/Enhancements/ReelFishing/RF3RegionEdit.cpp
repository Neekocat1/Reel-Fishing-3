// Reel Fishing III mod (step 16h): region edit mode. A cursor on the water (or ground) and an editor
// camera that follows it, so regions are placed and moved without Link. While it runs, the controller
// drives the editor and Link stands still. See docs/DESIGN.md step 16h.
//
// Step 46c: regions are outlines with holes (RF3Shape.h). Controls: stick = move the cursor (hold Z: faster);
// C-Left/Right or right stick X = turn the camera; C-Up/Down or right stick Y = zoom; A = drop a point, A near the first
// point (3+ points) closes the outline (a new region, or a hole of the selected region after the window's "Add hole");
// B while drawing = undo the last point; otherwise B = grab the selected region's nearest point, or a new point on the
// nearest edge, or the whole region, B again = drop; L = delete the grabbed point; D-pad Left/Right = turn the selected
// region (hold Z: fine); R = select the region under the cursor; START = leave.

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

enum class Grab { None, Point, Region };

constexpr float kPointReach = 40.0f; // B grabs a point this close; A this close to the first point closes
constexpr float kEdgeReach = 25.0f;  // B this close to an edge inserts a point there
constexpr float kMinStep = 10.0f;    // A closer than this to the last point is ignored (a double press)

bool sActive = false;
Input sPad = {};      // the controller, kept for the editor before the game's copy is cleared
s16 sCam = 0;         // the editor's sub-camera
Vec3f sCursor = {};
bool sCursorOnWater = false;
float sYaw = 0.0f;    // camera direction, radians; the camera looks along (sin yaw, cos yaw)
float sDist = 500.0f; // camera distance from the cursor
constexpr float kPitch = 0.70f; // about 40 degrees down
// Step 46c: the outline being drawn.
bool sDrawing = false;
bool sDrawHole = false;  // it will be a hole of the selected region
bool sHoleArmed = false; // the window's "Add hole": the next outline is a hole
rf3::shape::Ring sDrawPts;
float sDrawY = 0.0f;     // the cursor's height at the first point (the new region's surface)
// What B holds.
Grab sGrab = Grab::None;
int sGrabRing = -1;      // 0 = the outline, k = hole k-1
int sGrabPoint = -1;
rf3::shape::Point sGrabFrom; // the point's place before the grab (put back if the shape becomes invalid)
float sGrabOffX = 0.0f, sGrabOffZ = 0.0f; // region move: box centre - cursor
std::string sMessage; // the last thing the editor wants to say (shown in the window)

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

rf3::FishRegion* Selected(PlayState* play) {
    int i = rf3::SelectedRegion();
    std::vector<rf3::FishRegion>& regions = rf3::FishRegions();
    if ((i < 0) || (i >= (int)regions.size()) || (regions[i].scene != play->sceneNum)) {
        return nullptr;
    }
    return &regions[i];
}

// Ring k of a region: 0 = the outline, k = hole k-1.
rf3::shape::Ring* RingOf(rf3::FishRegion& r, int k) {
    if (k == 0) {
        return &r.outline;
    }
    return (k > 0 && k <= (int)r.holes.size()) ? &r.holes[k - 1] : nullptr;
}

float Dist(float ax, float az, float bx, float bz) {
    return std::hypot(ax - bx, az - bz);
}

void DropGrab() {
    if ((sGrab == Grab::Point) && (gPlayState != nullptr)) {
        if (rf3::FishRegion* r = Selected(gPlayState)) {
            std::string why;
            if (!rf3::shape::Valid(r->outline, r->holes, &why)) {
                if (rf3::shape::Ring* ring = RingOf(*r, sGrabRing);
                    ring != nullptr && sGrabPoint >= 0 && sGrabPoint < (int)ring->size()) {
                    (*ring)[sGrabPoint] = sGrabFrom; // the move would break the shape: put the point back
                }
                r->UpdateBounds();
                sMessage = "Point put back: the " + why + ".";
            }
        }
    }
    sGrab = Grab::None;
    sGrabRing = -1;
    sGrabPoint = -1;
}

void StartGrab(PlayState* play) {
    rf3::FishRegion* r = Selected(play);
    if ((r == nullptr) || !r->HasOutline()) {
        return;
    }
    const int rings = 1 + (int)r->holes.size();
    // The nearest point within reach.
    float best = kPointReach;
    for (int k = 0; k < rings; k++) {
        const rf3::shape::Ring& ring = *RingOf(*r, k);
        for (int i = 0; i < (int)ring.size(); i++) {
            float d = Dist(sCursor.x, sCursor.z, ring[i].x, ring[i].z);
            if (d < best) {
                best = d;
                sGrabRing = k;
                sGrabPoint = i;
            }
        }
    }
    if (sGrabPoint >= 0) {
        sGrabFrom = (*RingOf(*r, sGrabRing))[sGrabPoint];
        sGrab = Grab::Point;
        return;
    }
    // The nearest edge within reach: a new point there.
    best = kEdgeReach;
    int edgeRing = -1, edgeIndex = -1;
    rf3::shape::Point at;
    for (int k = 0; k < rings; k++) {
        const rf3::shape::Ring& ring = *RingOf(*r, k);
        const int n = (int)ring.size();
        for (int i = 0; i < n; i++) {
            const rf3::shape::Point& a = ring[i];
            const rf3::shape::Point& b = ring[(i + 1) % n];
            float dx = b.x - a.x, dz = b.z - a.z;
            float len2 = dx * dx + dz * dz;
            float t = (len2 > 0.0f) ? std::clamp(((sCursor.x - a.x) * dx + (sCursor.z - a.z) * dz) / len2, 0.0f, 1.0f)
                                    : 0.0f;
            float px = a.x + dx * t, pz = a.z + dz * t;
            float d = Dist(sCursor.x, sCursor.z, px, pz);
            if (d < best) {
                best = d;
                edgeRing = k;
                edgeIndex = i;
                at = { px, pz };
            }
        }
    }
    if (edgeRing >= 0) {
        rf3::shape::Ring& ring = *RingOf(*r, edgeRing);
        if ((int)ring.size() >= rf3::shape::kMaxRingPoints) {
            sMessage = "64 points at most in one outline.";
            return;
        }
        ring.insert(ring.begin() + edgeIndex + 1, at);
        sGrabRing = edgeRing;
        sGrabPoint = edgeIndex + 1;
        sGrabFrom = at;
        sGrab = Grab::Point;
        rf3::RegionsDirty() = true;
        return;
    }
    // Inside the outline: the whole region.
    if (r->InOutline(sCursor.x, sCursor.z)) {
        sGrab = Grab::Region;
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
    if (sGrab == Grab::Region) {
        r->MoveTo(sCursor.x + sGrabOffX, sCursor.z + sGrabOffZ);
    } else {
        rf3::shape::Ring* ring = RingOf(*r, sGrabRing);
        if ((ring == nullptr) || (sGrabPoint < 0) || (sGrabPoint >= (int)ring->size())) {
            sGrab = Grab::None;
            return;
        }
        (*ring)[sGrabPoint] = { sCursor.x, sCursor.z };
        r->UpdateBounds();
    }
    rf3::RegionsDirty() = true;
}

// L: delete the grabbed point (a ring keeps 3).
void DeleteGrabbedPoint(PlayState* play) {
    rf3::FishRegion* r = Selected(play);
    rf3::shape::Ring* ring = (r != nullptr && sGrab == Grab::Point) ? RingOf(*r, sGrabRing) : nullptr;
    if ((ring == nullptr) || (sGrabPoint < 0) || (sGrabPoint >= (int)ring->size())) {
        return;
    }
    if (ring->size() <= 3) {
        sMessage = "An outline keeps 3 points (delete a hole in the window).";
        return;
    }
    ring->erase(ring->begin() + sGrabPoint);
    std::string why;
    if (!rf3::shape::Valid(r->outline, r->holes, &why)) {
        ring->insert(ring->begin() + sGrabPoint, sGrabFrom); // deleting it would break the shape
        sMessage = "Not deleted: the " + why + ".";
    }
    r->UpdateBounds();
    sGrab = Grab::None;
    sGrabRing = -1;
    sGrabPoint = -1;
    rf3::RegionsDirty() = true;
}

void SelectUnderCursor(PlayState* play) {
    std::vector<rf3::FishRegion>& regions = rf3::FishRegions();
    int count = (int)regions.size();
    int start = rf3::SelectedRegion();
    for (int k = 1; k <= count; k++) { // cycle through overlapping regions, starting after the selected one
        int i = (start + k + count) % count;
        if ((regions[i].scene == play->sceneNum) && regions[i].InOutline(sCursor.x, sCursor.z)) {
            rf3::SelectedRegion() = i;
            return;
        }
    }
    rf3::SelectedRegion() = -1;
}

// The drawn outline closes: a new region, or a hole of the selected one. Refused (drawing goes on) if the shape is bad.
void CloseOutline(PlayState* play) {
    std::string why;
    if (sDrawHole) {
        rf3::FishRegion* r = Selected(play);
        if (r == nullptr) {
            sMessage = "The region for the hole isn't selected any more: drawing dropped.";
            sDrawing = false;
            sDrawPts.clear();
            return;
        }
        std::vector<rf3::shape::Ring> holes = r->holes;
        holes.push_back(sDrawPts);
        if (!rf3::shape::Valid(r->outline, holes, &why)) {
            sMessage = "Not closed: the " + why + ". B undoes points.";
            return;
        }
        r->holes = holes;
        sMessage = "Hole added (" + std::to_string(sDrawPts.size()) + " points).";
    } else {
        if (!rf3::shape::Valid(sDrawPts, {}, &why)) {
            sMessage = "Not closed: the " + why + ". B undoes points.";
            return;
        }
        std::vector<rf3::FishRegion>& regions = rf3::FishRegions();
        int here = 0;
        for (const rf3::FishRegion& other : regions) {
            here += (other.scene == play->sceneNum) ? 1 : 0;
        }
        rf3::FishRegion r;
        r.scene = play->sceneNum;
        r.name = "Scene " + std::to_string(play->sceneNum) + " region " + std::to_string(here + 1);
        r.outline = sDrawPts;
        r.UpdateBounds();
        bool water;
        r.surfaceY = SnapY(play, sDrawPts[0].x, sDrawPts[0].z, sDrawY, &water);
        regions.push_back(r);
        rf3::SelectedRegion() = (int)regions.size() - 1;
        sMessage = "Region added (" + std::to_string(sDrawPts.size()) + " points)" +
                   (water ? "." : ": no water at the first point, surface = the ground there.");
    }
    sDrawing = false;
    sDrawHole = false;
    sDrawPts.clear();
    rf3::RegionsDirty() = true;
}

// A: drop a point, or close the outline near its first point.
void PlacePoint(PlayState* play) {
    if (!sDrawing) {
        sDrawHole = sHoleArmed && (Selected(play) != nullptr);
        sHoleArmed = false;
        sDrawing = true;
        sDrawPts.clear();
        sDrawPts.push_back({ sCursor.x, sCursor.z });
        sDrawY = sCursor.y;
        sMessage = sDrawHole ? "Drawing a hole: A drops points, A on the first point closes."
                             : "Drawing a region: A drops points, A on the first point closes.";
        return;
    }
    const rf3::shape::Point& first = sDrawPts.front();
    if ((sDrawPts.size() >= 3) && (Dist(sCursor.x, sCursor.z, first.x, first.z) < kPointReach)) {
        CloseOutline(play);
        return;
    }
    const rf3::shape::Point& last = sDrawPts.back();
    if (Dist(sCursor.x, sCursor.z, last.x, last.z) < kMinStep) {
        return;
    }
    if ((int)sDrawPts.size() >= rf3::shape::kMaxRingPoints) {
        sMessage = "64 points at most: close the outline.";
        return;
    }
    sDrawPts.push_back({ sCursor.x, sCursor.z });
}

// B while drawing: take back the last point (none left: drawing stops).
void UndoPoint() {
    if (!sDrawPts.empty()) {
        sDrawPts.pop_back();
    }
    if (sDrawPts.empty()) {
        sDrawing = false;
        sDrawHole = false;
        sMessage = "Drawing dropped.";
    }
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

    // D-pad Left/Right turn the selected region's shape about its box centre (hold Z: fine steps); not while a point is
    // held.
    float rot = (Held(BTN_DLEFT) ? 1.0f : 0.0f) - (Held(BTN_DRIGHT) ? 1.0f : 0.0f);
    if ((rot != 0.0f) && (sGrab != Grab::Point) && !sDrawing) {
        if (rf3::FishRegion* r = Selected(play)) {
            r->RotateShape(rot * (Held(BTN_Z) ? 0.2f : 1.0f));
            rf3::RegionsDirty() = true;
        }
    }

    if (sGrab != Grab::None) {
        ApplyGrab(play);
    }
    if (Pressed(BTN_A) && (sGrab == Grab::None)) {
        PlacePoint(play);
    }
    if (Pressed(BTN_R) && !sDrawing) {
        SelectUnderCursor(play);
    }
    if (Pressed(BTN_L)) {
        DeleteGrabbedPoint(play);
    }
    if (Pressed(BTN_B)) {
        if (sDrawing) {
            UndoPoint();
        } else if (sGrab != Grab::None) {
            DropGrab();
        } else {
            StartGrab(play);
        }
    }
    UpdateCamera(play);
}

void ResetDrawing() {
    sDrawing = false;
    sDrawHole = false;
    sHoleArmed = false;
    sDrawPts.clear();
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
        ResetDrawing();
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
    ResetDrawing();
    sGrab = Grab::None;
    sMessage.clear();
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
    ResetDrawing();
}

void RegionEditArmHole() {
    sHoleArmed = true;
    sMessage = "Next outline is a hole of the selected region: A drops its first point.";
}

RegionEditView GetRegionEditView() {
    RegionEditView v;
    v.active = sActive;
    v.cursorX = sCursor.x;
    v.cursorY = sCursor.y;
    v.cursorZ = sCursor.z;
    v.cursorOnWater = sCursorOnWater;
    v.drawing = sDrawing;
    v.drawHole = sDrawHole;
    v.holeArmed = sHoleArmed;
    v.drawPts = sDrawPts;
    v.grab = (sGrab == Grab::Point) ? "point" : (sGrab == Grab::Region) ? "region" : "";
    v.message = sMessage;
    return v;
}

} // namespace rf3
