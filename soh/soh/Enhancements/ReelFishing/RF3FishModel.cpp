// Reel Fishing III mod (step 25e-2): RF3's own fish models for region fish. See RF3FishModel.h and docs/DESIGN.md
// "25e design".
//
// Each model is rf3models/fish/<model>.json in mods/rf3_models.o2r (rf3models/models.json maps fishdata records to
// files): the bones (parent, bind translation, axis + angle), the spine (head to tail), the snout bone (locator 0),
// and vertex batches in bone space, each listing its vertices bone by bone. Every vertex belongs to one bone (RF3
// skins rigidly, step 25b).
//
// Drawing, as SoH's flex skeletons do: per frame, one matrix per bone in a Graph_Alloc array behind segment 0x0D;
// the model's display list (built once) loads each bone group after gSPMatrix of its bone (vertices are transformed
// when loaded), then draws the batch's triangles, which may join bones, so joints stay closed.
//
// Swimming (user's choice, 2026-10-08): OoT's swim angles bend the RF3 spine; the model has no animation yet.
// World(bone) = World(parent) * T(t) * [swim turn] * R(axis, -angle) (RF3 angles are negated, step 25b).
//
// Log, don't look: logs/rf3_models.txt gets one line per model loaded and one per change of what a slot draws.

#include "RF3FishModel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <libultraship/bridge/consolevariablebridge.h>
#include <nlohmann/json.hpp>
#include <ship/Context.h>
#include <ship/resource/ResourceManager.h>
#include <ship/resource/archive/ArchiveManager.h>

#include "soh/cvar_prefixes.h"
#include "RF3Bridge.h"

extern "C" {
#include "z64.h"
#include "functions.h"
#include "macros.h"
#include "variables.h"
// OPEN_DISPS / CLOSE_DISPS (as kaleido.cpp declares them)
void FrameInterpolation_RecordOpenChild(const void* a, int b);
void FrameInterpolation_RecordCloseChild(void);
}

namespace {

using json = nlohmann::json;

constexpr const char* kOtrPrefix = "__OTR__";
constexpr const char* kIndexPath = "rf3models/models.json";
constexpr float kDefaultUnitsPerMetre = RF3_UNITS_PER_METRE; // user's choice (25e): the gameplay metre
constexpr int32_t kSegment = 0x0D;                           // as SkelAnime_DrawFlexOpa
// The 7 spine bones, head to tail: how much of OoT's two swim angles each takes [assumed]. OoT turns limbs 2/3 by
// angle A and limb 4 (the tail, phase-shifted, 1.6x) by angle B.
constexpr float kSpineShareA[7] = { 0.0f, 0.0f, 0.0f, 0.5f, 1.0f, 0.0f, 0.0f };
constexpr float kSpineShareB[7] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f };

using Mat3 = std::array<std::array<float, 3>, 3>;

struct Bone {
    int32_t parent = -1;
    float t[3] = { 0, 0, 0 };
    Mat3 rot{}; // bind local rotation, R(axis, -angle)
};

struct FishModel {
    bool ok = false;
    std::string why;
    std::string file;
    float posScale = 10000.0f;
    float lengthM = 1.0f;
    float center[3] = { 0, 0, 0 }; // pos units, model space
    float offset[3] = { 0, 0, 0 }; // pos units: VERT = World * TVRT + offset
    std::vector<Bone> bones;
    std::vector<int32_t> spine;
    int32_t mouthBone = -1;
    std::vector<Vtx> vtx;
    std::vector<Gfx> dl;
    std::deque<std::string> texPaths;
    int32_t verts = 0;
    int32_t tris = 0;
    int32_t batches = 0;
    int32_t texturesMissing = 0;
};

std::map<std::string, std::unique_ptr<FishModel>> sModels; // by file
std::map<int32_t, std::string> sRecordFiles;               // fishdata record -> file
bool sIndexTried = false;
std::string sIndexWhy;
std::map<int32_t, std::string> sSlotState; // last logged state per region slot
std::string sStatus = "no RF3 fish drawn yet";

std::shared_ptr<Ship::ArchiveManager> Archives() {
    auto ctx = Ship::Context::GetRawInstance();
    if (ctx == nullptr || ctx->GetResourceManager() == nullptr) {
        return nullptr;
    }
    return ctx->GetResourceManager()->GetArchiveManager();
}

void Log(const std::string& line) {
    std::string dir = Ship::Context::GetPathRelativeToAppDirectory("logs");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(dir + "/rf3_models.txt", std::ios::app);
    char stamp[32];
    std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    out << stamp << " " << line << "\n";
}

bool ReadJson(const std::string& path, json& out, std::string& why) {
    auto archives = Archives();
    if (archives == nullptr) {
        why = "no archive manager";
        return false;
    }
    auto file = archives->LoadFile(path);
    if (file == nullptr || file->Buffer == nullptr) {
        why = path + " not found (run tools/rf3_models.py models)";
        return false;
    }
    out = json::parse(file->Buffer->begin() + file->BufferOffset, file->Buffer->end(), nullptr, false);
    if (out.is_discarded()) {
        why = path + " is not valid JSON";
        return false;
    }
    return true;
}

void LoadIndex() {
    if (sIndexTried) {
        return;
    }
    sIndexTried = true;
    json j;
    if (!ReadJson(kIndexPath, j, sIndexWhy)) {
        Log("fish-index-load ok=0 why=" + sIndexWhy);
        return;
    }
    if (j.contains("fish_records") && j["fish_records"].is_object()) {
        for (auto& [key, value] : j["fish_records"].items()) {
            sRecordFiles[std::stoi(key)] = value.get<std::string>();
        }
    }
    if (sRecordFiles.empty()) {
        sIndexWhy = "models.json has no fish (rebuild with tools/rf3_models.py models)";
    }
    Log("fish-index-load ok=" + std::to_string(sRecordFiles.empty() ? 0 : 1) +
        " records=" + std::to_string(sRecordFiles.size()) + (sIndexWhy.empty() ? "" : " why=" + sIndexWhy));
}

Mat3 Identity() {
    Mat3 m{};
    m[0][0] = m[1][1] = m[2][2] = 1.0f;
    return m;
}

Mat3 AxisAngle(float x, float y, float z, float angle) {
    float ln = std::sqrt(x * x + y * y + z * z);
    if (ln < 1e-9f || std::fabs(angle) < 1e-12f) {
        return Identity();
    }
    x /= ln;
    y /= ln;
    z /= ln;
    float c = std::cos(angle), s = std::sin(angle), k = 1.0f - c;
    Mat3 m;
    m[0] = { c + x * x * k, x * y * k - z * s, x * z * k + y * s };
    m[1] = { y * x * k + z * s, c + y * y * k, y * z * k - x * s };
    m[2] = { z * x * k - y * s, z * y * k + x * s, c + z * z * k };
    return m;
}

Mat3 Mul(const Mat3& a, const Mat3& b) {
    Mat3 m{};
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            m[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
        }
    }
    return m;
}

int32_t Log2(int32_t v) {
    int32_t n = 0;
    while ((1 << (n + 1)) <= v) {
        n++;
    }
    return n;
}

void Build(FishModel& m) {
    json j;
    if (!ReadJson(m.file, j, m.why)) {
        return;
    }
    if (!j.contains("bones") || !j.contains("batches") || !j.contains("surfaces") || !j.contains("spine")) {
        m.why = m.file + " lacks bones/batches/surfaces/spine";
        return;
    }
    m.posScale = j.value("pos_scale", 10000.0f);
    if (j.contains("size_m") && j["size_m"].size() == 3) {
        m.lengthM = std::max(0.01f, j["size_m"][2].get<float>()); // fish lie along z
    }
    for (int i = 0; i < 3; i++) {
        m.center[i] = j["center"][i].get<float>();
        m.offset[i] = j["offset"][i].get<float>();
    }
    for (const json& b : j["bones"]) {
        Bone bone;
        bone.parent = b["parent"].get<int32_t>();
        for (int i = 0; i < 3; i++) {
            bone.t[i] = b["t"][i].get<float>();
        }
        bone.rot = AxisAngle(b["axis"][0].get<float>(), b["axis"][1].get<float>(), b["axis"][2].get<float>(),
                             -b["angle"].get<float>()); // RF3's angles are negated (25b)
        m.bones.push_back(bone);
    }
    for (int32_t i = 0; i < static_cast<int32_t>(m.bones.size()); i++) {
        if (m.bones[i].parent >= i) {
            m.why = "bone " + std::to_string(i) + " has parent " + std::to_string(m.bones[i].parent) +
                    " (parents must come first)";
            return;
        }
    }
    for (const json& s : j["spine"]) {
        m.spine.push_back(s.get<int32_t>());
    }
    m.mouthBone = j.value("mouth_bone", -1);

    auto archives = Archives();
    struct Surface {
        const char* tex = nullptr;
        int32_t w = 0;
        int32_t h = 0;
    };
    std::vector<Surface> surfaces;
    for (const json& s : j["surfaces"]) {
        Surface out;
        if (s.contains("texture") && s["texture"].is_string() && s["tex_size"].size() == 2) {
            std::string tex = s["texture"].get<std::string>();
            if (archives != nullptr && archives->HasFile(tex)) {
                m.texPaths.push_back(kOtrPrefix + tex);
                out.tex = m.texPaths.back().c_str();
                out.w = s["tex_size"][0].get<int32_t>();
                out.h = s["tex_size"][1].get<int32_t>();
            } else {
                m.texturesMissing++;
            }
        }
        surfaces.push_back(out);
    }

    size_t nv = 0, nt = 0, ng = 0;
    for (const json& b : j["batches"]) {
        nv += b["verts"].size();
        nt += b["tris"].size();
        ng += b["groups"].size();
    }
    m.vtx.reserve(nv);
    for (const json& b : j["batches"]) {
        for (const json& v : b["verts"]) {
            Vtx x = {};
            for (int i = 0; i < 3; i++) {
                x.n.ob[i] = v[i].get<int16_t>();
                x.n.n[i] = static_cast<int8_t>(v[5 + i].get<int32_t>());
            }
            x.n.tc[0] = v[3].get<int16_t>();
            x.n.tc[1] = v[4].get<int16_t>();
            x.n.a = 255;
            m.vtx.push_back(x);
        }
    }
    m.dl.resize(16 + 16 * (j["batches"].size() + surfaces.size()) + 2 * ng + nt);
    Gfx* g = m.dl.data();
    gDPPipeSync(g++);
    gDPSetCycleType(g++, G_CYC_1CYCLE);
    gDPSetRenderMode(g++, G_RM_AA_ZB_TEX_EDGE, G_RM_AA_ZB_TEX_EDGE2);
    gSPClearGeometryMode(g++, G_CULL_FRONT);
    gSPSetGeometryMode(g++, G_ZBUFFER | G_SHADE | G_CULL_BACK | G_LIGHTING | G_SHADING_SMOOTH);
    size_t first = 0;
    int32_t current = -1;
    const int32_t nBones = static_cast<int32_t>(m.bones.size());
    for (const json& b : j["batches"]) {
        int32_t si = b.value("surface", 0);
        if (si != current && si >= 0 && si < static_cast<int32_t>(surfaces.size())) {
            current = si;
            const Surface& s = surfaces[si];
            gDPPipeSync(g++);
            if (s.tex != nullptr) {
                gDPSetCombineMode(g++, G_CC_MODULATEIDECALA, G_CC_MODULATEIDECALA);
                gSPTexture(g++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON);
                gDPLoadTextureBlock(g++, s.tex, G_IM_FMT_RGBA, G_IM_SIZ_32b, s.w, s.h, 0, G_TX_NOMIRROR | G_TX_WRAP,
                                    G_TX_NOMIRROR | G_TX_WRAP, Log2(s.w), Log2(s.h), G_TX_NOLOD, G_TX_NOLOD);
            } else {
                gDPSetCombineMode(g++, G_CC_SHADE, G_CC_SHADE);
                gSPTexture(g++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_OFF);
            }
        }
        int32_t slot = 0;
        for (const json& grp : b["groups"]) {
            int32_t bone = grp[0].get<int32_t>();
            int32_t count = grp[1].get<int32_t>();
            if (bone < 0 || bone >= nBones) {
                m.why = "batch group bone " + std::to_string(bone) + " out of range";
                return;
            }
            // Segmented address: LUS marks it with bit 0 (Interpreter::SegAddr).
            uintptr_t mtxAddr = (static_cast<uintptr_t>(kSegment) << 24) | (bone * sizeof(Mtx)) | 1;
            gSPMatrix(g++, mtxAddr, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
            gSPVertex(g++, reinterpret_cast<uintptr_t>(&m.vtx[first + slot]), count, slot);
            slot += count;
        }
        for (const json& t : b["tris"]) {
            gSP1Triangle(g++, t[0].get<int32_t>(), t[1].get<int32_t>(), t[2].get<int32_t>(), 0);
        }
        first += b["verts"].size();
        m.batches++;
    }
    gSPTexture(g++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_OFF);
    gSPEndDisplayList(g++);
    m.dl.resize(g - m.dl.data());
    m.verts = static_cast<int32_t>(nv);
    m.tris = static_cast<int32_t>(nt);
    m.ok = true;
}

FishModel* GetForRecord(int32_t record, std::string& why) {
    LoadIndex();
    auto rf = sRecordFiles.find(record);
    if (rf == sRecordFiles.end()) {
        why = sRecordFiles.empty() ? sIndexWhy : "no RF3 model for record " + std::to_string(record);
        return nullptr;
    }
    auto it = sModels.find(rf->second);
    if (it != sModels.end()) {
        return it->second.get();
    }
    auto m = std::make_unique<FishModel>();
    m->file = rf->second;
    Build(*m);
    char line[320];
    std::snprintf(line, sizeof(line),
                  "fish-model-load record=%d file=%s ok=%d bones=%d spine=%d mouth_bone=%d verts=%d tris=%d batches=%d "
                  "textures=%s length_m=%.3f%s%s",
                  record, m->file.c_str(), m->ok ? 1 : 0, static_cast<int32_t>(m->bones.size()),
                  static_cast<int32_t>(m->spine.size()), m->mouthBone, m->verts, m->tris, m->batches,
                  m->texturesMissing ? "missing" : (m->texPaths.empty() ? "none" : "ok"), m->lengthM,
                  m->why.empty() ? "" : " why=", m->why.c_str());
    Log(line);
    FishModel* out = m.get();
    sModels[rf->second] = std::move(m);
    return out;
}

void NoteSlot(int32_t slot, const std::string& state) {
    sStatus = "slot " + std::to_string(slot) + ": " + state;
    auto& last = sSlotState[slot];
    if (last != state) {
        last = state;
        Log("slot=" + std::to_string(slot) + " " + state);
    }
}

// Bone worlds (model space, translations in metres) with OoT's swim angles bending the spine.
void BoneWorlds(const FishModel& m, float angleA, float angleB, bool bind, std::vector<Mat3>& rot,
                std::vector<std::array<float, 3>>& pos) {
    const size_t n = m.bones.size();
    rot.assign(n, Identity());
    pos.assign(n, { 0, 0, 0 });
    std::vector<float> turn(n, 0.0f);
    if (!bind) {
        for (size_t k = 0; k < m.spine.size() && k < 7; k++) {
            int32_t b = m.spine[k];
            if (b >= 0 && b < static_cast<int32_t>(n)) {
                turn[b] = kSpineShareA[k] * angleA + kSpineShareB[k] * angleB;
            }
        }
    }
    for (size_t i = 0; i < n; i++) {
        const Bone& b = m.bones[i];
        Mat3 prot = b.parent >= 0 ? rot[b.parent] : Identity();
        std::array<float, 3> ppos = b.parent >= 0 ? pos[b.parent] : std::array<float, 3>{ 0, 0, 0 };
        for (int r = 0; r < 3; r++) {
            pos[i][r] = ppos[r] + prot[r][0] * b.t[0] + prot[r][1] * b.t[1] + prot[r][2] * b.t[2];
        }
        Mat3 local = b.rot;
        if (turn[i] != 0.0f) {
            // A turn about the model's vertical (RF3 y), expressed in the parent's frame: prot^T * (0, 1, 0).
            Mat3 q = AxisAngle(prot[1][0], prot[1][1], prot[1][2], turn[i]);
            local = Mul(q, local);
        }
        rot[i] = Mul(prot, local);
    }
}

// MtxF from rotation and translation (OoT's MtxF is column-major: mf[column][row]).
MtxF ToMtxF(const Mat3& r, float tx, float ty, float tz) {
    MtxF f;
    for (int c = 0; c < 3; c++) {
        for (int row = 0; row < 3; row++) {
            f.mf[c][row] = r[row][c];
        }
        f.mf[c][3] = 0.0f;
    }
    f.mf[3][0] = tx;
    f.mf[3][1] = ty;
    f.mf[3][2] = tz;
    f.mf[3][3] = 1.0f;
    return f;
}

} // namespace

// At global scope: OPEN_DISPS declares the frame-interpolation functions inside the calling function (see
// RF3RegionWindow.cpp).
extern "C" int32_t RF3FishModel_DrawRegion(PlayState* play, int32_t slot, int16_t bendA, int16_t bendB,
                                           float* mouthOut) {
    if (!RF3_IsEnabled() || !CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.FishModels"), 1)) {
        NoteSlot(slot, "fish-model-off why=rf3-or-option-off");
        return 0;
    }
    RF3RegionFishInfo info;
    if (!RF3RegionFish_Get(slot, &info)) {
        NoteSlot(slot, "fish-model-off why=empty-slot");
        return 0;
    }
    int32_t record = CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.FishModelPreview"), -1);
    bool preview = record >= 0;
    if (!preview) {
        record = info.record;
    }
    std::string why;
    FishModel* m = GetForRecord(record, why);
    if (m == nullptr || !m->ok) {
        NoteSlot(slot, "fish-model-off record=" + std::to_string(record) + " why=" + (m ? m->why : why));
        return 0;
    }
    float unitsPerMetre = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.FishModelScale"), kDefaultUnitsPerMetre);
    float bend = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.FishModelBend"), 1.0f);
    bool bindPose = CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.FishModelBindPose"), 0) != 0;
    float lengthUnits = info.mm / 1000.0f * unitsPerMetre;
    float scale = (info.mm / 1000.0f) / m->lengthM * unitsPerMetre / m->posScale;
    char state[200];
    std::snprintf(state, sizeof(state),
                  "fish-model-draw record=%d preview=%d mm=%.0f units_per_m=%.0f length_units=%.1f bend=%.2f bind=%d",
                  record, preview ? 1 : 0, info.mm, unitsPerMetre, lengthUnits, bend, bindPose ? 1 : 0);
    NoteSlot(slot, state);

    std::vector<Mat3> rot;
    std::vector<std::array<float, 3>> pos;
    float a = bend * bendA * static_cast<float>(M_PI) / 32768.0f;
    float b = bend * bendB * static_cast<float>(M_PI) / 32768.0f;
    BoneWorlds(*m, a, b, bindPose, rot, pos);

    OPEN_DISPS(play->state.gfxCtx);
    Matrix_Push();
    Matrix_Scale(scale, scale, scale, MTXMODE_APPLY);
    // RF3 is y-down with the snout at -z; this frame is y-up with the front at +z (step 25c-2).
    Matrix_RotateX(static_cast<f32>(M_PI), MTXMODE_APPLY);
    // Centre the fish on the actor: model space p = World * local + offset; draw p - center.
    Matrix_Translate(m->offset[0] - m->center[0], m->offset[1] - m->center[1], m->offset[2] - m->center[2],
                     MTXMODE_APPLY);
    const size_t n = m->bones.size();
    Mtx* mtx = static_cast<Mtx*>(Graph_Alloc(play->state.gfxCtx, n * sizeof(Mtx)));
    for (size_t i = 0; i < n; i++) {
        MtxF f = ToMtxF(rot[i], pos[i][0] * m->posScale, pos[i][1] * m->posScale, pos[i][2] * m->posScale);
        Matrix_Push();
        Matrix_Mult(&f, MTXMODE_APPLY);
        Matrix_ToMtx(&mtx[i], const_cast<char*>(__FILE__), __LINE__);
        Matrix_Pop();
    }
    if (mouthOut != nullptr && m->mouthBone >= 0 && m->mouthBone < static_cast<int32_t>(n)) {
        // The snout: locator 0's origin, as OoT's limb 13 + sFishMouthOffset gives fishMouthPos.
        Vec3f zero = { 0.0f, 0.0f, 0.0f };
        Vec3f out;
        MtxF f = ToMtxF(rot[m->mouthBone], pos[m->mouthBone][0] * m->posScale, pos[m->mouthBone][1] * m->posScale,
                        pos[m->mouthBone][2] * m->posScale);
        Matrix_Push();
        Matrix_Mult(&f, MTXMODE_APPLY);
        Matrix_MultVec3f(&zero, &out);
        Matrix_Pop();
        mouthOut[0] = out.x;
        mouthOut[1] = out.y;
        mouthOut[2] = out.z;
    }
    gSPSegment(POLY_OPA_DISP++, kSegment, reinterpret_cast<uintptr_t>(mtx));
    gSPDisplayList(POLY_OPA_DISP++, m->dl.data());
    Matrix_Pop();
    CLOSE_DISPS(play->state.gfxCtx);
    return 1;
}

extern "C" const char* RF3FishModel_Status(void) {
    return sStatus.c_str();
}
