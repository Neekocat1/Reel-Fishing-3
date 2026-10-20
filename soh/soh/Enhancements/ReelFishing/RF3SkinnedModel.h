// Reel Fishing III mod (steps 25e/25f): RF3's rigidly skinned models (fish, rods) from mods/rf3_models.o2r, shared by
// RF3FishModel.cpp and RF3RodModel.cpp. C++ only. See docs/DESIGN.md "25e design", "25f design".
//
// A model file (tools/rf3_models.py models) has the bones (parent, bind translation, axis + angle), vertex batches
// in bone space that list their vertices bone by bone, and kind-specific fields (fish: spine, mouth_bone; rod:
// chain, chain_z, tip_bone, tip_local). Drawing works as SoH's flex skeletons: one matrix per bone in a Graph_Alloc
// array behind segment 0x0D; the display list (built once) loads each bone group after gSPMatrix of its bone.
#ifndef RF3_SKINNED_MODEL_H
#define RF3_SKINNED_MODEL_H

#ifdef __cplusplus

#include <array>
#include <deque>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

extern "C" {
#include "z64.h"
}

namespace rf3model {

using Mat3 = std::array<std::array<float, 3>, 3>;

struct Bone {
    int32_t parent = -1;
    float t[3] = { 0, 0, 0 }; // metres
    Mat3 rot{};               // bind local rotation, R(axis, -angle) (RF3's angles are negated, step 25b)
};

struct SkinnedModel {
    bool ok = false;
    std::string why;
    std::string file;
    float posScale = 10000.0f;
    float sizeM[3] = { 0, 0, 0 };
    float center[3] = { 0, 0, 0 }; // pos units, model space
    float offset[3] = { 0, 0, 0 }; // pos units: VERT = World * TVRT + offset
    std::vector<Bone> bones;
    nlohmann::json extra; // the file's kind-specific fields (spine, mouth_bone, chain, tip_bone, ...)
    std::vector<Vtx> vtx;
    std::vector<Gfx> dl;
    std::deque<std::string> texPaths;
    int32_t verts = 0;
    int32_t tris = 0;
    int32_t batches = 0;
    int32_t texturesMissing = 0;
    int32_t clampedS = 0; // step 60b: textured surfaces drawn clamped in s / in t (fish only)
    int32_t clampedT = 0;
    // step 27e-3: per bone, its vertex count and how many of them are on a hook surface (hook, lhook, hari)
    std::vector<int32_t> boneVerts;
    std::vector<int32_t> boneHookVerts;
};

// Appends one line to logs/rf3_models.txt.
void Log(const std::string& line);
// Reads a JSON file from the loaded archives. Returns false with why on failure.
bool ReadJson(const std::string& path, nlohmann::json& out, std::string& why);
// The model in file, loaded and built once (cached by file). Logs a "<what>-model-load" line on the first load.
SkinnedModel* Get(const std::string& file, const std::string& what, const std::string& key);

Mat3 Identity();
Mat3 AxisAngle(float x, float y, float z, float angle);
Mat3 Mul(const Mat3& a, const Mat3& b);
// World rotation and position (metres) per bone. turns[i] (optional) is an extra local rotation put between a bone's
// translation and its bind rotation: World(i) = World(parent) * T(t) * turns[i] * Rbind.
void BoneWorlds(const SkinnedModel& m, const std::vector<Mat3>* turns, std::vector<Mat3>& rot,
                std::vector<std::array<float, 3>>& pos);
// The same from given local translations (metres) and rotations per bone (step 25g: motion poses).
void BoneWorldsLocal(const SkinnedModel& m, const std::vector<std::array<float, 3>>& localT,
                     const std::vector<Mat3>& localR, std::vector<Mat3>& rot, std::vector<std::array<float, 3>>& pos);

} // namespace rf3model

// At global scope (OPEN_DISPS declares functions inside the calling function).
// Draws m at the current matrix: Matrix_Scale(scale), then the axis fix (0: half turn about x, for fish and tackle:
// RF3 y-down, front -z -> y-up, front +z; 1: half turn about z, for rods: y-down -> y-up, tip stays +z), then
// translate by pre (pos units), then each bone's world. points: bone-local points (pos units) to transform to
// world space after the draw, as (bone, x, y, z) -> out (x, y, z); may be empty.
struct RF3SkinnedPoint {
    int32_t bone;
    float local[3];
    float* out;
};
void RF3Skinned_Draw(PlayState* play, const rf3model::SkinnedModel& m, const std::vector<rf3model::Mat3>& rot,
                     const std::vector<std::array<float, 3>>& pos, float scale, int32_t axisFix, const float* pre,
                     const std::vector<RF3SkinnedPoint>& points);

#endif // __cplusplus

#endif
