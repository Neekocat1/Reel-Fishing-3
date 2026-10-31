// Reel Fishing III mod (steps 25e/25f): RF3's rigidly skinned models, shared by fish and rods. See RF3SkinnedModel.h.
// (Moved out of RF3FishModel.cpp in step 25f-2; the logic is unchanged.)

#include "RF3SkinnedModel.h"
#include "RF3DevTools.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>

#include <ship/Context.h>
#include <ship/resource/ResourceManager.h>
#include <ship/resource/archive/ArchiveManager.h>

extern "C" {
#include "functions.h"
#include "macros.h"
#include "variables.h"
// OPEN_DISPS / CLOSE_DISPS (as kaleido.cpp declares them)
void FrameInterpolation_RecordOpenChild(const void* a, int b);
void FrameInterpolation_RecordCloseChild(void);
}

namespace rf3model {
namespace {

using json = nlohmann::json;

constexpr const char* kOtrPrefix = "__OTR__";
constexpr int32_t kSegment = 0x0D; // as SkelAnime_DrawFlexOpa

std::map<std::string, std::unique_ptr<SkinnedModel>> sModels; // by file

std::shared_ptr<Ship::ArchiveManager> Archives() {
    auto ctx = Ship::Context::GetRawInstance();
    if (ctx == nullptr || ctx->GetResourceManager() == nullptr) {
        return nullptr;
    }
    return ctx->GetResourceManager()->GetArchiveManager();
}

int32_t Log2(int32_t v) {
    int32_t n = 0;
    while ((1 << (n + 1)) <= v) {
        n++;
    }
    return n;
}

void Build(SkinnedModel& m) {
    json j;
    if (!ReadJson(m.file, j, m.why)) {
        return;
    }
    if (!j.contains("bones") || !j.contains("batches") || !j.contains("surfaces")) {
        m.why = m.file + " lacks bones/batches/surfaces";
        return;
    }
    m.posScale = j.value("pos_scale", 10000.0f);
    for (int i = 0; i < 3; i++) {
        m.sizeM[i] = j["size_m"][i].get<float>();
        m.center[i] = j["center"][i].get<float>();
        m.offset[i] = j["offset"][i].get<float>();
    }
    // "parts": step 26g's part controllers (left out of this list at first, so every fish loaded parts=0);
    // "menu_pose": step 39c, the rod's tackle-screen pose (RF3's motion 30)
    for (const char* key :
         { "spine", "mouth_bone", "locators", "chain", "chain_z", "tip_bone", "tip_local", "motion", "parts",
           "body_bone", "menu_pose" }) {
        if (j.contains(key)) {
            m.extra[key] = j[key];
        }
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

    auto archives = Archives();
    struct Surface {
        const char* tex = nullptr;
        int32_t w = 0;
        int32_t h = 0;
        uint32_t cms = G_TX_NOMIRROR | G_TX_WRAP;
        uint32_t cmt = G_TX_NOMIRROR | G_TX_WRAP;
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
    // Step 60b (user, 2026-10-19: "some of the fish's fins have extra pixels of texture on the very end"): the fins'
    // texture coordinates run right up to the texture's edge (u to 1.000, v from 0.000, all 40 fish), and wrap mode
    // blends the opposite edge into the tips. A fish surface whose coordinates all lie inside the texture on an axis
    // is drawn clamped on that axis. Rods and lures keep wrap.
    if (j.value("kind", "") == "fish") {
        for (size_t si = 0; si < surfaces.size(); si++) {
            Surface& s = surfaces[si];
            if (s.tex == nullptr) {
                continue;
            }
            bool insideS = true, insideT = true;
            for (const json& b : j["batches"]) {
                if (b.value("surface", 0) != static_cast<int32_t>(si)) {
                    continue;
                }
                for (const json& v : b["verts"]) {
                    const int32_t sc = v[3].get<int32_t>(), tc = v[4].get<int32_t>();
                    insideS = insideS && sc >= 0 && sc <= s.w * 32;
                    insideT = insideT && tc >= 0 && tc <= s.h * 32;
                }
            }
            if (insideS) {
                s.cms = G_TX_NOMIRROR | G_TX_CLAMP;
                m.clampedS++;
            }
            if (insideT) {
                s.cmt = G_TX_NOMIRROR | G_TX_CLAMP;
                m.clampedT++;
            }
        }
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
    m.boneVerts.assign(nBones, 0);
    m.boneHookVerts.assign(nBones, 0);
    for (const json& b : j["batches"]) {
        int32_t si = b.value("surface", 0);
        bool hookSurface = false;
        if (si >= 0 && si < static_cast<int32_t>(j["surfaces"].size())) {
            const std::string name = j["surfaces"][si].value("name", "");
            hookSurface = name == "hook" || name == "lhook" || name == "hari";
        }
        if (si != current && si >= 0 && si < static_cast<int32_t>(surfaces.size())) {
            current = si;
            const Surface& s = surfaces[si];
            gDPPipeSync(g++);
            if (s.tex != nullptr) {
                gDPSetCombineMode(g++, G_CC_MODULATEIDECALA, G_CC_MODULATEIDECALA);
                gSPTexture(g++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON);
                gDPLoadTextureBlock(g++, s.tex, G_IM_FMT_RGBA, G_IM_SIZ_32b, s.w, s.h, 0, s.cms, s.cmt, Log2(s.w),
                                    Log2(s.h), G_TX_NOLOD, G_TX_NOLOD);
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
            m.boneVerts[bone] += count;
            if (hookSurface) {
                m.boneHookVerts[bone] += count;
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

} // namespace

void Log(const std::string& line) {
    std::string dir = Ship::Context::GetPathRelativeToAppDirectory("logs");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out = RF3OpenLog(dir + "/rf3_models.txt"); // step 78b: nothing while the logs are off
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

SkinnedModel* Get(const std::string& file, const std::string& what, const std::string& key) {
    auto it = sModels.find(file);
    if (it != sModels.end()) {
        return it->second.get();
    }
    auto m = std::make_unique<SkinnedModel>();
    m->file = file;
    Build(*m);
    char line[360];
    std::snprintf(line, sizeof(line),
                  "%s-model-load %s file=%s ok=%d bones=%d verts=%d tris=%d batches=%d textures=%s size_m=%.3f,%.3f,%.3f"
                  " clamped=%d/%d extra=%s%s%s",
                  what.c_str(), key.c_str(), file.c_str(), m->ok ? 1 : 0, static_cast<int32_t>(m->bones.size()),
                  m->verts, m->tris, m->batches, m->texturesMissing ? "missing" : (m->texPaths.empty() ? "none" : "ok"),
                  m->sizeM[0], m->sizeM[1], m->sizeM[2], m->clampedS, m->clampedT, m->extra.dump().substr(0, 120).c_str(),
                  m->why.empty() ? "" : " why=", m->why.c_str());
    Log(line);
    SkinnedModel* out = m.get();
    sModels[file] = std::move(m);
    return out;
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

void BoneWorlds(const SkinnedModel& m, const std::vector<Mat3>* turns, std::vector<Mat3>& rot,
                std::vector<std::array<float, 3>>& pos) {
    const size_t n = m.bones.size();
    rot.assign(n, Identity());
    pos.assign(n, { 0, 0, 0 });
    for (size_t i = 0; i < n; i++) {
        const Bone& b = m.bones[i];
        Mat3 prot = b.parent >= 0 ? rot[b.parent] : Identity();
        std::array<float, 3> ppos = b.parent >= 0 ? pos[b.parent] : std::array<float, 3>{ 0, 0, 0 };
        for (int r = 0; r < 3; r++) {
            pos[i][r] = ppos[r] + prot[r][0] * b.t[0] + prot[r][1] * b.t[1] + prot[r][2] * b.t[2];
        }
        Mat3 local = (turns != nullptr && i < turns->size()) ? Mul((*turns)[i], b.rot) : b.rot;
        rot[i] = Mul(prot, local);
    }
}

void BoneWorldsLocal(const SkinnedModel& m, const std::vector<std::array<float, 3>>& localT,
                     const std::vector<Mat3>& localR, std::vector<Mat3>& rot, std::vector<std::array<float, 3>>& pos) {
    const size_t n = m.bones.size();
    rot.assign(n, Identity());
    pos.assign(n, { 0, 0, 0 });
    for (size_t i = 0; i < n; i++) {
        const Bone& b = m.bones[i];
        Mat3 prot = b.parent >= 0 ? rot[b.parent] : Identity();
        std::array<float, 3> ppos = b.parent >= 0 ? pos[b.parent] : std::array<float, 3>{ 0, 0, 0 };
        const float* t = i < localT.size() ? localT[i].data() : b.t;
        for (int r = 0; r < 3; r++) {
            pos[i][r] = ppos[r] + prot[r][0] * t[0] + prot[r][1] * t[1] + prot[r][2] * t[2];
        }
        rot[i] = Mul(prot, i < localR.size() ? localR[i] : b.rot);
    }
}

} // namespace rf3model

namespace {

constexpr int32_t kSkinSegment = 0x0D; // as SkelAnime_DrawFlexOpa (the display lists' gSPMatrix use it)

// MtxF from rotation and translation (OoT's MtxF is column-major: mf[column][row]).
MtxF ToMtxF(const rf3model::Mat3& r, float tx, float ty, float tz) {
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

void RF3Skinned_Draw(PlayState* play, const rf3model::SkinnedModel& m, const std::vector<rf3model::Mat3>& rot,
                     const std::vector<std::array<float, 3>>& pos, float scale, int32_t axisFix, const float* pre,
                     const std::vector<RF3SkinnedPoint>& points) {
    OPEN_DISPS(play->state.gfxCtx);
    Matrix_Push();
    Matrix_Scale(scale, scale, scale, MTXMODE_APPLY);
    if (axisFix == 0) {
        // RF3 is y-down with the front at -z; this frame is y-up with the front at +z (step 25c-2).
        Matrix_RotateX(static_cast<f32>(M_PI), MTXMODE_APPLY);
    } else {
        // Rods: y-down -> y-up, the tip stays at +z (step 25f).
        Matrix_RotateZ(static_cast<f32>(M_PI), MTXMODE_APPLY);
    }
    if (pre != nullptr) {
        Matrix_Translate(pre[0], pre[1], pre[2], MTXMODE_APPLY);
    }
    const size_t n = m.bones.size();
    Mtx* mtx = static_cast<Mtx*>(Graph_Alloc(play->state.gfxCtx, n * sizeof(Mtx)));
    for (size_t i = 0; i < n; i++) {
        MtxF f = ToMtxF(rot[i], pos[i][0] * m.posScale, pos[i][1] * m.posScale, pos[i][2] * m.posScale);
        Matrix_Push();
        Matrix_Mult(&f, MTXMODE_APPLY);
        Matrix_ToMtx(&mtx[i], const_cast<char*>(__FILE__), __LINE__);
        Matrix_Pop();
    }
    for (const RF3SkinnedPoint& p : points) {
        if (p.bone < 0 || p.bone >= static_cast<int32_t>(n) || p.out == nullptr) {
            continue;
        }
        Vec3f local = { p.local[0], p.local[1], p.local[2] };
        Vec3f out;
        MtxF f = ToMtxF(rot[p.bone], pos[p.bone][0] * m.posScale, pos[p.bone][1] * m.posScale,
                        pos[p.bone][2] * m.posScale);
        Matrix_Push();
        Matrix_Mult(&f, MTXMODE_APPLY);
        Matrix_MultVec3f(&local, &out);
        Matrix_Pop();
        p.out[0] = out.x;
        p.out[1] = out.y;
        p.out[2] = out.z;
    }
    gSPSegment(POLY_OPA_DISP++, kSkinSegment, reinterpret_cast<uintptr_t>(mtx));
    gSPDisplayList(POLY_OPA_DISP++, const_cast<Gfx*>(m.dl.data()));
    Matrix_Pop();
    CLOSE_DISPS(play->state.gfxCtx);
}
