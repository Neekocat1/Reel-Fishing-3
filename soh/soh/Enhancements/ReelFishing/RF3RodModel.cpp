// Reel Fishing III mod (step 25f-2): RF3's own rod models in Link's hands. See RF3RodModel.h and docs/DESIGN.md
// "25f design". Loading and drawing are shared with the fish (RF3SkinnedModel.cpp).
//
// rf3models/models.json maps RROD rods 0..9 to rf3models/rod/rod(n+1).json (RF3's own loader adds 1, step 25f).
// User's choices (2026-10-08): the model's own size at the gameplay metre (68 units/m); OoT's bend drives the rod.
//
// Placement: the model's z = 0 (the grip / reel seat) at OoT's hand point, tip toward +z, reel per RF3 (below for
// spinning and fly reels, on top for the baitcast). RF3 is y-down: a half turn about z.
// Bend: OoT turns each of its 22 segments by ratio[i] * bend * 0.5 (Y, then X). Each RF3 chain bone takes the
// summed ratios of the OoT segments that lie between it and the previous chain bone (by fraction of rod length).
//
// Log, don't look: logs/rf3_models.txt gets "rod-model-load" per model and "rod-model-draw"/"rod-model-off" on change.

#include "RF3RodModel.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include <libultraship/bridge/consolevariablebridge.h>

#include "soh/cvar_prefixes.h"
#include "RF3Bridge.h"
#include "RF3SkinnedModel.h"

extern "C" {
#include "functions.h"
}

namespace {

using rf3model::Mat3;

constexpr const char* kIndexPath = "rf3models/models.json";
constexpr int32_t kRodCount = 10; // RROD 0..9; OoT's pole (rod 10) keeps OoT's rod
constexpr float kDefaultUnitsPerMetre = RF3_UNITS_PER_METRE; // user's choice: the gameplay metre

std::map<int32_t, std::string> sRodFiles;
bool sIndexTried = false;
std::string sIndexWhy;
std::string sLastState;
std::string sStatus = "no RF3 rod drawn yet";

void LoadIndex() {
    if (sIndexTried) {
        return;
    }
    sIndexTried = true;
    nlohmann::json j;
    if (!rf3model::ReadJson(kIndexPath, j, sIndexWhy)) {
        rf3model::Log("rod-index-load ok=0 why=" + sIndexWhy);
        return;
    }
    if (j.contains("rods") && j["rods"].is_object()) {
        for (auto& [key, value] : j["rods"].items()) {
            sRodFiles[std::stoi(key)] = value.get<std::string>();
        }
    }
    if (sRodFiles.empty()) {
        sIndexWhy = "models.json has no rods (rebuild with tools/rf3_models.py models)";
    }
    rf3model::Log("rod-index-load ok=" + std::to_string(sRodFiles.empty() ? 0 : 1) +
                  " rods=" + std::to_string(sRodFiles.size()) + (sIndexWhy.empty() ? "" : " why=" + sIndexWhy));
}

void NoteRod(const std::string& state) {
    sStatus = state;
    if (state != sLastState) {
        sLastState = state;
        rf3model::Log(state);
    }
}

// OoT's summed bend ratios from the butt up to fraction f of the rod (segment i spans [i/count, (i+1)/count)).
float RatioUpTo(const float* ratios, int32_t count, float f) {
    float sum = 0.0f;
    for (int32_t i = 0; i < count; i++) {
        float lo = static_cast<float>(i) / count;
        float hi = static_cast<float>(i + 1) / count;
        if (f >= hi) {
            sum += ratios[i];
        } else if (f > lo) {
            sum += ratios[i] * (f - lo) * count;
        }
    }
    return sum;
}

// OoT's bend as turns of the chain bones. OoT: RotY(bendY), then RotX(bendX), in the rod's own frame. After the half
// turn about z (OoT x = -RF3 x, OoT y = -RF3 y), that is RF3 -y and -x: angles negated about RF3's y and x, each
// axis taken in the bone's parent frame (bind).
std::vector<Mat3> ChainTurns(const rf3model::SkinnedModel& m, float bendY, float bendX, const float* ratios,
                             int32_t count) {
    std::vector<Mat3> turns(m.bones.size(), rf3model::Identity());
    if (!m.extra.contains("chain") || !m.extra.contains("chain_z") || ratios == nullptr || count <= 0) {
        return turns;
    }
    std::vector<Mat3> bindRot;
    std::vector<std::array<float, 3>> bindPos;
    rf3model::BoneWorlds(m, nullptr, bindRot, bindPos);
    const auto& chain = m.extra["chain"];
    const auto& chainZ = m.extra["chain_z"];
    float zButt = m.center[2] / m.posScale - m.sizeM[2] * 0.5f;
    float zTip = chainZ.back().get<float>();
    float span = std::fmax(0.01f, zTip - zButt);
    float prevF = 0.0f;
    for (size_t k = 0; k < chain.size() && k < chainZ.size(); k++) {
        int32_t b = chain[k].get<int32_t>();
        float f = std::clamp((chainZ[k].get<float>() - zButt) / span, 0.0f, 1.0f);
        float share = RatioUpTo(ratios, count, f) - RatioUpTo(ratios, count, prevF);
        prevF = f;
        if (b < 0 || b >= static_cast<int32_t>(m.bones.size()) || share == 0.0f) {
            continue;
        }
        int32_t p = m.bones[b].parent;
        Mat3 prot = p >= 0 ? bindRot[p] : rf3model::Identity();
        Mat3 ry = rf3model::AxisAngle(prot[1][0], prot[1][1], prot[1][2], -share * bendY * 0.5f);
        Mat3 rx = rf3model::AxisAngle(prot[0][0], prot[0][1], prot[0][2], -share * bendX * 0.5f);
        turns[b] = rf3model::Mul(ry, rx);
    }
    return turns;
}

} // namespace

extern "C" int32_t RF3RodModel_Draw(PlayState* play, float bendY, float bendX, const float* ratios, int32_t count,
                                    float* tipOut) {
    if (!RF3_IsEnabled() || !CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.RodModels"), 1)) {
        NoteRod("rod-model-off why=rf3-or-option-off");
        return 0;
    }
    int32_t rod = CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.RodModelPreview"), -1);
    bool preview = rod >= 0;
    if (!preview) {
        RF3TackleChoice c;
        RF3Tackle_Get(&c);
        rod = c.rod;
    }
    if (rod < 0 || rod >= kRodCount) {
        NoteRod("rod-model-off why=no-rf3-model rod=" + std::to_string(rod)); // OoT's pole is rod 10
        return 0;
    }
    LoadIndex();
    auto it = sRodFiles.find(rod);
    if (it == sRodFiles.end()) {
        NoteRod("rod-model-off rod=" + std::to_string(rod) + " why=" + (sRodFiles.empty() ? sIndexWhy : "not in models.json"));
        return 0;
    }
    rf3model::SkinnedModel* m = rf3model::Get(it->second, "rod", "rod=" + std::to_string(rod));
    if (m == nullptr || !m->ok) {
        NoteRod("rod-model-off rod=" + std::to_string(rod) + " why=" + (m ? m->why : "load"));
        return 0;
    }

    // World units per unit of this frame (Link's hand matrix includes his scale): measured, not assumed.
    Vec3f a = { 0.0f, 0.0f, 0.0f };
    Vec3f b = { 0.0f, 0.0f, 1000.0f };
    Vec3f wa, wb;
    Matrix_MultVec3f(&a, &wa);
    Matrix_MultVec3f(&b, &wb);
    float frameScale = std::sqrt((wb.x - wa.x) * (wb.x - wa.x) + (wb.y - wa.y) * (wb.y - wa.y) +
                                 (wb.z - wa.z) * (wb.z - wa.z)) /
                       1000.0f;
    if (frameScale <= 1e-6f) {
        NoteRod("rod-model-off rod=" + std::to_string(rod) + " why=degenerate-frame");
        return 0;
    }
    float unitsPerMetre = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.RodModelScale"), kDefaultUnitsPerMetre);
    float bendScale = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.RodModelBend"), 1.0f);
    float gripM = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.RodModelGrip"), 0.0f);
    float scale = unitsPerMetre / frameScale / m->posScale;
    float lengthM = m->sizeM[2];
    char state[220];
    std::snprintf(state, sizeof(state),
                  "rod-model-draw rod=%d preview=%d length_m=%.3f units_per_m=%.0f length_units=%.1f frame_scale=%.5f "
                  "bend=%.2f grip_m=%.3f",
                  rod, preview ? 1 : 0, lengthM, unitsPerMetre, lengthM * unitsPerMetre, frameScale, bendScale, gripM);
    NoteRod(state);

    std::vector<Mat3> turns = ChainTurns(*m, bendY * bendScale, bendX * bendScale, ratios, count);
    std::vector<Mat3> rot;
    std::vector<std::array<float, 3>> pos;
    rf3model::BoneWorlds(*m, &turns, rot, pos);
    // The grip slider moves the rod along itself (+ = the hand further up the rod).
    float pre[3] = { m->offset[0], m->offset[1], m->offset[2] - gripM * m->posScale };
    std::vector<RF3SkinnedPoint> points;
    int32_t tip = m->extra.value("tip_bone", -1);
    if (tipOut != nullptr && tip >= 0) {
        RF3SkinnedPoint p = { tip, { 0.0f, 0.0f, 0.0f }, tipOut };
        if (m->extra.contains("tip_local") && m->extra["tip_local"].size() == 3) {
            for (int i = 0; i < 3; i++) {
                p.local[i] = m->extra["tip_local"][i].get<float>();
            }
        }
        points.push_back(p);
    }
    RF3Skinned_Draw(play, *m, rot, pos, scale, 1, pre, points);
    return 1;
}

// Step 28b: the item-get rod's height offset, world units (user's value in game, 2026-10-12).
constexpr float kDefaultGiRodOffsetY = -5.0f;

extern "C" int32_t RF3RodModel_DrawGI(PlayState* play, int32_t rod, float lengthWorld) {
    if (rod < 0 || rod >= kRodCount) {
        return 0; // OoT's pole is rod 10
    }
    LoadIndex();
    auto it = sRodFiles.find(rod);
    if (it == sRodFiles.end()) {
        return 0;
    }
    rf3model::SkinnedModel* m = rf3model::Get(it->second, "rod", "rod=" + std::to_string(rod));
    if (m == nullptr || !m->ok || m->sizeM[2] <= 0.01f) {
        return 0;
    }
    Vec3f a = { 0.0f, 0.0f, 0.0f };
    Vec3f b = { 0.0f, 0.0f, 1000.0f };
    Vec3f wa, wb;
    Matrix_MultVec3f(&a, &wa);
    Matrix_MultVec3f(&b, &wb);
    float frameScale = std::sqrt((wb.x - wa.x) * (wb.x - wa.x) + (wb.y - wa.y) * (wb.y - wa.y) +
                                 (wb.z - wa.z) * (wb.z - wa.z)) /
                       1000.0f;
    if (frameScale <= 1e-6f) {
        return 0;
    }
    float lengthM = m->sizeM[2];
    float scale = lengthWorld / frameScale / lengthM / m->posScale;
    char state[200];
    std::snprintf(state, sizeof(state),
                  "gi-rod-model rod=%d length_m=%.3f length_world=%.0f offset_y=%.1f frame_scale=%.5f", rod, lengthM,
                  lengthWorld, CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.GiRodOffsetY"), kDefaultGiRodOffsetY),
                  frameScale);
    NoteRod(state);
    std::vector<Mat3> rot;
    std::vector<std::array<float, 3>> pos;
    rf3model::BoneWorlds(*m, nullptr, rot, pos);
    // centred on the model's box: pre = offset - centre (pos units), as the fish
    float pre[3] = { m->offset[0] - m->center[0], m->offset[1] - m->center[1], m->offset[2] - m->center[2] };
    Matrix_Push();
    // The user's vertical offset (world units, + = up; slider "RF3 rod offset (item get)"), in the frame's y.
    float offsetY = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.GiRodOffsetY"), kDefaultGiRodOffsetY);
    Matrix_Translate(0.0f, offsetY / frameScale, 0.0f, MTXMODE_APPLY);
    // horizontal (user, 2026-10-12: "the rod should be horizontal instead of vertical"): the rod's length (+z) along
    // the frame's x, side-on as the tackle
    Matrix_RotateY(static_cast<f32>(M_PI) / 2.0f, MTXMODE_APPLY);
    RF3Skinned_Draw(play, *m, rot, pos, scale, 1, pre, {});
    Matrix_Pop();
    return 1;
}

extern "C" int32_t RF3RodModel_DrawMenu(PlayState* play, int32_t rod, const float* pos, const float* rotXYZ,
                                        const float* scale, float* tipOut, int32_t* posed) {
    *posed = 0;
    if (rod < 0 || rod >= kRodCount) {
        return 0; // OoT's pole is rod 10
    }
    LoadIndex();
    auto it = sRodFiles.find(rod);
    if (it == sRodFiles.end()) {
        return 0;
    }
    rf3model::SkinnedModel* m = rf3model::Get(it->second, "rod", "rod=" + std::to_string(rod));
    if (m == nullptr || !m->ok) {
        return 0;
    }
    std::vector<Mat3> rot;
    std::vector<std::array<float, 3>> bpos;
    const auto& pose = m->extra.contains("menu_pose") ? m->extra["menu_pose"] : nlohmann::json();
    if (pose.is_array() && pose.size() == m->bones.size()) {
        // RF3's motion 30: local translation and axis + angle per bone (angles negated as the bind's, 25b)
        std::vector<std::array<float, 3>> localT(pose.size());
        std::vector<Mat3> localR(pose.size());
        for (size_t i = 0; i < pose.size(); i++) {
            for (int k = 0; k < 3; k++) {
                localT[i][k] = pose[i]["t"][k].get<float>();
            }
            localR[i] = rf3model::AxisAngle(pose[i]["axis"][0].get<float>(), pose[i]["axis"][1].get<float>(),
                                            pose[i]["axis"][2].get<float>(), -pose[i]["angle"].get<float>());
        }
        rf3model::BoneWorldsLocal(*m, localT, localR, rot, bpos);
        *posed = 1;
    } else {
        rf3model::BoneWorlds(*m, nullptr, rot, bpos);
    }
    float pre[3] = { m->offset[0], m->offset[1], m->offset[2] };
    std::vector<RF3SkinnedPoint> points;
    int32_t tipBone = m->extra.value("tip_bone", -1);
    if (tipOut != nullptr && tipBone >= 0) {
        RF3SkinnedPoint p = { tipBone, { 0.0f, 0.0f, 0.0f }, tipOut };
        if (m->extra.contains("tip_local") && m->extra["tip_local"].size() == 3) {
            for (int i = 0; i < 3; i++) {
                p.local[i] = m->extra["tip_local"][i].get<float>();
            }
        }
        points.push_back(p);
    }
    Matrix_Push();
    Matrix_Translate(pos[0], pos[1], pos[2], MTXMODE_APPLY);
    Matrix_RotateZ(rotXYZ[2], MTXMODE_APPLY);
    Matrix_RotateY(rotXYZ[1], MTXMODE_APPLY);
    Matrix_RotateX(rotXYZ[0], MTXMODE_APPLY);
    Matrix_Scale(scale[0], scale[1], scale[2], MTXMODE_APPLY);
    // RF3Skinned_Draw's axis fix 1 turns y-down into y-up by a half turn about z; this frame is RF3's own, so undo it
    Matrix_RotateZ(static_cast<f32>(M_PI), MTXMODE_APPLY);
    RF3Skinned_Draw(play, *m, rot, bpos, 1.0f / m->posScale, 1, pre, points); // pos units -> metres
    Matrix_Pop();
    char state[200];
    std::snprintf(state, sizeof(state), "menu-rod-model rod=%d posed=%d scale=%.2f,%.2f,%.2f", rod, *posed, scale[0],
                  scale[1], scale[2]);
    NoteRod(state);
    return 1;
}

extern "C" const char* RF3RodModel_Status(void) {
    return sStatus.c_str();
}
