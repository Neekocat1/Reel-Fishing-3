#pragma once

// Reel Fishing III mod (step 16c): fish regions outside the pond. A region is a box on a body of
// water in one scene, with how many fish live there and which RF3 records (species and size class)
// may spawn. Placed with the editor window (RF3RegionWindow) and kept in rf3_fish_regions.json in the
// SoH fork. See docs/DESIGN.md step 16.

#include <cmath>
#include <string>
#include <vector>

#include "RF3Shape.h" // step 46a

namespace rf3 {

// Step 24a (RF3RandoAreas.cpp): the randomizer's areas of a scene, as (enum name, display name).
struct RandoArea {
    std::string enumName;
    std::string name;
};
std::vector<RandoArea> RandoAreasForScene(int scene);
int RandoAreaFromName(const std::string& name); // RR_* value, -1 if empty or unknown

// Step 36c (user, 2026-10-14): when a region's fish are there. Age: Link's age the region is for. A condition is a
// game flag that must be set (or unset), counted only at its age: e.g. Lake Hylia's water is low as an adult until
// EVENTCHKINF_RAISED_LAKE_HYLIA_WATER is set. kind "event" = eventChkInf, "switch" = the scene's switch flag.
// Step 65b: kind "oasis" = the Desert Colossus oasis is full (Song of Storms; it empties on every scene load); no flag.
enum class RegionAge { Both, Child, Adult };
const char* RegionAgeName(RegionAge age); // "both", "child", "adult"
RegionAge RegionAgeFromName(const std::string& name);

struct RegionCondition {
    std::string kind = "event";
    int flag = 0;
    bool set = true;
    RegionAge age = RegionAge::Both;
};

// Step 46a: a region's shape is an outline of world points with holes (RF3Shape.h). minX..maxZ are then the outline's
// bounding box and angle is 0. (Before: a box on the water, minX..maxX / minZ..maxZ in its own frame, turned by
// `angle` (degrees, seen from above) about its centre (step 16i); older files are turned into 4-point outlines on
// load.)
//   world     = centre + R(angle)  * (box point - centre)
//   box point = centre + R(-angle) * (world - centre)
struct FishRegion {
    std::string name;
    int scene = -1;
    float minX = 0.0f;
    float maxX = 0.0f;
    float minZ = 0.0f;
    float maxZ = 0.0f;
    float angle = 0.0f;    // degrees (step 16i)
    float surfaceY = 0.0f; // the water surface, read from the water box when placed
    int fishCount = 6;
    std::vector<int> records; // fishdata.fdt records that may spawn here
    // Step 20c-3a: the water current, which drifts a resting fly (RF3-MECHANICS 2.9). Direction as an OoT
    // yaw in degrees (0 = +Z, 90 = +X, Link's facing); speed in m/s. Not set: the box's long axis at
    // kDefaultCurrentSpeed.
    bool currentSet = false;
    float currentYaw = 0.0f;
    float currentSpeed = 0.2f;
    // Step 20c-4: outside the box the current falls off linearly to 0 at this distance from its edge (m).
    float currentReachM = 5.0f;
    // Step 24a: the randomizer area (logic region) the water is in, by its enum name ("RR_ZORAS_RIVER"); empty =
    // any area of the scene. Randomizer logic (step 24c) requires reaching it to catch this region's fish.
    std::string randoArea;
    // Step 36c: the age it is for, and the flags that must hold (older files: both, none).
    RegionAge age = RegionAge::Both;
    std::vector<RegionCondition> conditions;
    // Step 46a: the shape (world points, seen from above). Fewer than 3 outline points: the box is the shape.
    shape::Ring outline;
    std::vector<shape::Ring> holes;

    bool HasOutline() const { return outline.size() >= 3; }
    // The bounding box from the outline (angle 0).
    void UpdateBounds() {
        if (HasOutline()) {
            shape::Bounds(outline, &minX, &maxX, &minZ, &maxZ);
            angle = 0.0f;
        }
    }
    // Make the box (and its angle) the outline, with no holes.
    void OutlineFromBox() {
        outline = shape::BoxRing(minX, maxX, minZ, maxZ, Radians());
        holes.clear();
        UpdateBounds();
    }
    // In the outline, holes ignored (selecting a region by pointing at it).
    bool InOutline(float x, float z) const { return HasOutline() ? shape::InRing(outline, x, z) : Contains(x, z); }
    // Move every point by (dx, dz).
    void Translate(float dx, float dz) {
        for (shape::Point& p : outline) {
            p.x += dx;
            p.z += dz;
        }
        for (shape::Ring& h : holes) {
            for (shape::Point& p : h) {
                p.x += dx;
                p.z += dz;
            }
        }
        minX += dx;
        maxX += dx;
        minZ += dz;
        maxZ += dz;
    }
    // Turn every point by `degrees` about the bounding box centre.
    void RotateShape(float degrees) {
        const float cx = CentreX(), cz = CentreZ(), rad = degrees * 3.14159265f / 180.0f;
        auto turn = [&](shape::Point& p) {
            float x, z;
            Turn(rad, p.x - cx, p.z - cz, &x, &z);
            p.x = cx + x;
            p.z = cz + z;
        };
        for (shape::Point& p : outline) {
            turn(p);
        }
        for (shape::Ring& h : holes) {
            for (shape::Point& p : h) {
                turn(p);
            }
        }
        UpdateBounds();
    }

    float CentreX() const { return (minX + maxX) * 0.5f; }
    float CentreZ() const { return (minZ + maxZ) * 0.5f; }
    float Radians() const { return angle * 3.14159265f / 180.0f; }

    // Turn (dx, dz) by `radians` seen from above.
    static void Turn(float radians, float dx, float dz, float* outX, float* outZ) {
        float c = std::cos(radians), s = std::sin(radians);
        *outX = dx * c - dz * s;
        *outZ = dx * s + dz * c;
    }
    void ToBox(float x, float z, float* bx, float* bz) const {
        Turn(-Radians(), x - CentreX(), z - CentreZ(), bx, bz);
        *bx += CentreX();
        *bz += CentreZ();
    }
    void ToWorld(float bx, float bz, float* x, float* z) const {
        Turn(Radians(), bx - CentreX(), bz - CentreZ(), x, z);
        *x += CentreX();
        *z += CentreZ();
    }
    bool Contains(float x, float z) const {
        if (HasOutline()) {
            return shape::Inside(outline, holes, x, z); // step 46a
        }
        float bx, bz;
        ToBox(x, z, &bx, &bz);
        return (bx >= minX) && (bx <= maxX) && (bz >= minZ) && (bz <= maxZ);
    }
    // Step 20c-4: distance from (x, z) to the box's edge, OoT units (0 inside).
    float DistanceOutside(float x, float z) const {
        if (HasOutline()) {
            return shape::OutsideOutline(outline, x, z); // step 46a: holes don't count
        }
        float bx, bz;
        ToBox(x, z, &bx, &bz);
        float dx = std::fmax(std::fmax(minX - bx, bx - maxX), 0.0f);
        float dz = std::fmax(std::fmax(minZ - bz, bz - maxZ), 0.0f);
        return std::sqrt(dx * dx + dz * dz);
    }
    // Step 20c-3a: the current's direction (OoT yaw, degrees): the set one, or the box's long axis.
    float CurrentYawDeg() const {
        if (currentSet) {
            return currentYaw;
        }
        float ax, az;
        bool alongX = (maxX - minX) >= (maxZ - minZ);
        Turn(Radians(), alongX ? 1.0f : 0.0f, alongX ? 0.0f : 1.0f, &ax, &az);
        return std::atan2(ax, az) * 180.0f / 3.14159265f;
    }
    // Corners in the world: 0 = (minX, minZ), 1 = (maxX, minZ), 2 = (maxX, maxZ), 3 = (minX, maxZ).
    void Corner(int i, float* x, float* z) const {
        float bx = (i == 0 || i == 3) ? minX : maxX;
        float bz = (i == 0 || i == 1) ? minZ : maxZ;
        ToWorld(bx, bz, x, z);
    }
    // Move the box (same size and angle) so its centre is (x, z). Step 46a: the outline and holes move with it.
    void MoveTo(float x, float z) {
        if (HasOutline()) {
            Translate(x - CentreX(), z - CentreZ());
            return;
        }
        float hx = (maxX - minX) * 0.5f, hz = (maxZ - minZ) * 0.5f;
        minX = x - hx;
        maxX = x + hx;
        minZ = z - hz;
        maxZ = z + hz;
    }
    // Make the box the one with world corners (ax, az) and (bx, bz) opposite each other, angle kept.
    void SetFromCornersWorld(float ax, float az, float bx, float bz) {
        float dx, dz;
        Turn(-Radians(), bx - ax, bz - az, &dx, &dz);
        float hx = std::fabs(dx) * 0.5f, hz = std::fabs(dz) * 0.5f;
        float cx = (ax + bx) * 0.5f, cz = (az + bz) * 0.5f;
        minX = cx - hx;
        maxX = cx + hx;
        minZ = cz - hz;
        maxZ = cz + hz;
    }
};

// All regions of all scenes, as loaded or edited.
std::vector<FishRegion>& FishRegions();
// The file in use: the fork's tracked copy if the source tree is present, else next to soh.exe.
std::string FishRegionsPath();
// Read the file (a missing file means no regions). Returns false and sets error on a bad file.
bool LoadFishRegions(std::string* error);
// Write the file. Returns false and sets error if it can't be written.
bool SaveFishRegions(std::string* error);
// Records a region may list: the juvenile, normal and big records of the 22 species (not the
// legendary or Red/Black Carp records, which are for later).
bool IsRegionRecord(int record);
// Step 16d (RF3RegionFish.cpp): remove this scene's region fish and spawn them again from the regions
// as they are now (saved or not). Returns the count, or -1 (status says why).
int RespawnRegionFish(std::string* status);

// Step 16h: shared by the window and edit mode.
int& SelectedRegion(); // index into FishRegions(), -1 = none
bool& RegionsDirty();  // edits not saved yet

// Step 16h (RF3RegionEdit.cpp): edit mode, a cursor and an editor camera, placement without Link.
bool RegionEditActive();
// Returns false and sets status if edit mode can't start now (no scene, a cast is out).
bool RegionEditStart(std::string* status);
void RegionEditStop();
// Step 46c: the next outline drawn in edit mode becomes a hole of the selected region.
void RegionEditArmHole();
struct RegionEditView {
    bool active = false;
    float cursorX = 0, cursorY = 0, cursorZ = 0;
    bool cursorOnWater = false;
    bool drawing = false;          // step 46c: an outline is being drawn (drawPts so far)
    bool drawHole = false;         // ... as a hole of the selected region
    bool holeArmed = false;        // the next outline will be a hole
    shape::Ring drawPts;
    const char* grab = "";         // what B is holding: "", "point", "region"
    std::string message;           // the editor's last message
};
RegionEditView GetRegionEditView();

} // namespace rf3
