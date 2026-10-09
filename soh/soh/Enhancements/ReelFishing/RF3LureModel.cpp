// Reel Fishing III mod (steps 25c-2, 25d-2): RF3's own lure, fly, bait and float models on the line. See
// RF3LureModel.h and docs/DESIGN.md "25c design", "25d design".
//
// Each model is rf3models/<kind>/<file>.json in mods/rf3_models.o2r: vertex batches of at most 32 (positions in
// 0.1 mm, texture coordinates in declared texels x 32, normals) per surface, and the surface's texture, an
// HD (version 1) texture resource loaded at its small declared size. The display list is built once per model
// and kept. RF3 models are y-down with the front at -z; every draw turns them half about x.
//
// Log, don't look: logs/rf3_models.txt gets one line per model loaded (counts, missing textures) and one line
// whenever what is drawn changes (item, scale, flip, the float's place, or why OoT's lure is drawn instead).

#include "RF3LureModel.h"

#include <algorithm>
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
#include "RF3Sim.h"
#include "RF3TackleAssets.h"

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
constexpr float kOotLureScale = 0.0039999997f; // z_fishing.c's lure model scale (sLurePosZOffset is in it)

// The model kinds, as tools/rf3_models.py MODEL_KINDS writes them. Item N = file N [assumed: the counts match].
enum Kind { kLure, kFly, kBait, kFloat, kKinds };
struct KindInfo {
    const char* name;
    const char* file;   // printf pattern of the archive path
    int32_t count;      // lures 0..90 (OoT's pole lures 91/92 keep OoT's models), flies, bait groups, floats
    const char* cvar;   // size, OoT units per RF3 metre
    float defaultScale; // [assumed] (DESIGN 25c/25d): lure 4.4 cm ~ OoT's lure; fly, bait visible; float real size
};
constexpr KindInfo kKindInfo[kKinds] = {
    { "lure", "rf3models/lure/lur%03d.json", 91, CVAR_ENHANCEMENT("ReelFishing3.LureModelScale"), 270.0f },
    { "fly", "rf3models/fly/fly%03d.json", 40, CVAR_ENHANCEMENT("ReelFishing3.FlyModelScale"), 600.0f },
    { "bait", "rf3models/bait/baithook%03d.json", 12, CVAR_ENHANCEMENT("ReelFishing3.BaitModelScale"), 400.0f },
    { "float", "rf3models/float/float%03d.json", 6, CVAR_ENHANCEMENT("ReelFishing3.FloatModelScale"),
      RF3_UNITS_PER_METRE },
};
constexpr float kFloatMaxShare = 0.9f; // out of the water: at most this share of the way from the bait to the rod

struct LureModel {
    bool ok = false;
    std::string why; // why not, for the log and the debug window
    float posScale = 10000.0f;
    std::vector<Vtx> vtx;
    std::vector<Gfx> dl;
    std::deque<std::string> texPaths; // the display list points at these strings
    int32_t verts = 0;
    int32_t tris = 0;
    int32_t batches = 0;
    int32_t texturesMissing = 0;
    float sizeM[3] = { 0, 0, 0 };
};

std::map<std::pair<int32_t, int32_t>, std::unique_ptr<LureModel>> sModels; // (kind, index)
std::string sLastDrawState;
std::string sStatus = "nothing drawn yet";
std::string sLastFloatState;
std::string sFloatStatus = "no float drawn yet";

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

int32_t Log2(int32_t v) {
    int32_t n = 0;
    while ((1 << (n + 1)) <= v) {
        n++;
    }
    return n;
}

// Reads the model's JSON and builds its vertices and display list. Fills why on failure.
void Build(LureModel& m, int32_t kind, int32_t index) {
    char path[64];
    std::snprintf(path, sizeof(path), kKindInfo[kind].file, index);
    auto archives = Archives();
    if (archives == nullptr) {
        m.why = "no archive manager";
        return;
    }
    auto file = archives->LoadFile(path);
    if (file == nullptr || file->Buffer == nullptr) {
        m.why = std::string(path) + " not found (run tools/rf3_models.py models)";
        return;
    }
    json j = json::parse(file->Buffer->begin() + file->BufferOffset, file->Buffer->end(), nullptr, false);
    if (j.is_discarded() || !j.contains("batches") || !j.contains("surfaces")) {
        m.why = std::string(path) + " is not valid";
        return;
    }
    m.posScale = j.value("pos_scale", 10000.0f);
    if (j.contains("size_m") && j["size_m"].size() == 3) {
        for (int i = 0; i < 3; i++) {
            m.sizeM[i] = j["size_m"][i].get<float>();
        }
    }

    // Surfaces: the texture (if it is in the archives) and its declared size.
    struct Surface {
        const char* tex = nullptr;
        int32_t w = 0;
        int32_t h = 0;
    };
    std::vector<Surface> surfaces;
    for (const json& s : j["surfaces"]) {
        Surface out;
        if (s.contains("texture") && s["texture"].is_string() && s.contains("tex_size") && s["tex_size"].size() == 2) {
            std::string tex = s["texture"].get<std::string>();
            if (archives->HasFile(tex)) {
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

    // Vertices first (all of them, so the pointers in the display list stay valid), then the list.
    size_t nv = 0;
    size_t nt = 0;
    for (const json& b : j["batches"]) {
        nv += b["verts"].size();
        nt += b["tris"].size();
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
    // Upper bound: setup, a texture load per surface change, a vertex load + one command per triangle.
    m.dl.resize(16 + 16 * (j["batches"].size() + surfaces.size()) + nt);
    Gfx* g = m.dl.data();
    gDPPipeSync(g++);
    gDPSetCycleType(g++, G_CYC_1CYCLE); // z_fishing.c's setup is 2-cycle (with fog); this list is plain 1-cycle
    gDPSetRenderMode(g++, G_RM_AA_ZB_TEX_EDGE, G_RM_AA_ZB_TEX_EDGE2); // cut-out where the texture is clear
    gSPClearGeometryMode(g++, G_CULL_FRONT);
    gSPSetGeometryMode(g++, G_ZBUFFER | G_SHADE | G_CULL_BACK | G_LIGHTING | G_SHADING_SMOOTH);
    size_t first = 0;
    int32_t current = -1;
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
        size_t count = b["verts"].size();
        gSPVertex(g++, reinterpret_cast<uintptr_t>(&m.vtx[first]), static_cast<int32_t>(count), 0);
        for (const json& t : b["tris"]) {
            gSP1Triangle(g++, t[0].get<int32_t>(), t[1].get<int32_t>(), t[2].get<int32_t>(), 0);
        }
        first += count;
        m.batches++;
    }
    gSPTexture(g++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_OFF);
    gSPEndDisplayList(g++);
    m.dl.resize(g - m.dl.data());
    m.verts = static_cast<int32_t>(nv);
    m.tris = static_cast<int32_t>(nt);
    m.ok = true;
}

LureModel* Get(int32_t kind, int32_t index) {
    auto key = std::make_pair(kind, index);
    auto it = sModels.find(key);
    if (it != sModels.end()) {
        return it->second.get();
    }
    auto m = std::make_unique<LureModel>();
    Build(*m, kind, index);
    char line[320];
    std::snprintf(line, sizeof(line),
                  "model-load kind=%s index=%d ok=%d verts=%d tris=%d batches=%d textures=%s size_m=%.3f,%.3f,%.3f%s%s",
                  kKindInfo[kind].name, index, m->ok ? 1 : 0, m->verts, m->tris, m->batches,
                  m->texturesMissing ? "missing" : (m->texPaths.empty() ? "none" : "ok"), m->sizeM[0], m->sizeM[1],
                  m->sizeM[2], m->why.empty() ? "" : " why=", m->why.c_str());
    Log(line);
    LureModel* out = m.get();
    sModels[key] = std::move(m);
    return out;
}

// Logs a state once each time it changes, and keeps it for the debug window.
void NoteState(std::string& last, std::string& status, const std::string& state) {
    status = state;
    if (state != last) {
        last = state;
        Log(state);
    }
}

// The chosen box, or nullptr (no tackle.json, or out of range).
const rf3tackle::Box* ChosenBox(RF3TackleChoice& c) {
    RF3Tackle_Get(&c);
    const rf3tackle::Assets& a = rf3tackle::Get();
    if (!a.loaded || c.box < 0 || c.box >= static_cast<int32_t>(a.boxes.size())) {
        return nullptr;
    }
    return &a.boxes[c.box];
}

int32_t PreviewKind() {
    return CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.ModelPreviewKind"), -1);
}

int32_t PreviewIndex() {
    return CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.ModelPreviewIndex"), 0);
}

// The rig's item on the line: kind and index, or kind -1 with why.
std::pair<int32_t, int32_t> ChosenItem(std::string& why) {
    int32_t pk = PreviewKind();
    if (pk >= kLure && pk <= kBait) {
        return { pk, PreviewIndex() };
    }
    RF3TackleChoice c;
    const rf3tackle::Box* box = ChosenBox(c);
    std::string boxKind = box ? box->kind
                              : ((c.box == RF3_TACKLE_BOX_BASS || c.box == RF3_TACKLE_BOX_TROUT) ? "lure" : "");
    if (boxKind == "lure") {
        return { kLure, c.lure };
    }
    if (boxKind == "fly") {
        return { kFly, c.lure };
    }
    if (boxKind == "bait") {
        return { kBait, c.bait };
    }
    why = "box=" + std::to_string(c.box) + " kind=" + (boxKind.empty() ? "?" : boxKind);
    return { -1, -1 };
}

float LongestUnits(const LureModel* m, float unitsPerMetre) {
    return std::fmax(std::fmax(m->sizeM[0], m->sizeM[1]), m->sizeM[2]) * unitsPerMetre;
}

} // namespace

// At global scope: OPEN_DISPS declares the frame-interpolation functions inside the calling function (see
// RF3RegionWindow.cpp). Scale and the RF3 -> OoT axes (half turn about x: RF3 is y-down, front at -z), then the list.
static void DrawModel(PlayState* play, const LureModel* m, float unitsPerMetre, bool flip) {
    float scale = unitsPerMetre / m->posScale;
    OPEN_DISPS(play->state.gfxCtx);
    Matrix_Scale(scale, scale, scale, MTXMODE_APPLY);
    // RF3 models are y-down with the nose at -z (step 25c-2, from the bass texture: its back has the smallest y,
    // its head the smallest z); this frame is y-up with the front at +z. A half turn about x fixes both (a
    // rotation, so the faces keep their winding). User, first run without it: "backwards and upside down".
    Matrix_RotateX(static_cast<f32>(M_PI), MTXMODE_APPLY);
    if (flip) {
        Matrix_RotateY(static_cast<f32>(M_PI), MTXMODE_APPLY); // front <-> back
    }
    gSPMatrix(POLY_OPA_DISP++, Matrix_NewMtx(play->state.gfxCtx, (char*)__FILE__, __LINE__),
              G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    gSPDisplayList(POLY_OPA_DISP++, const_cast<Gfx*>(m->dl.data()));
    CLOSE_DISPS(play->state.gfxCtx);
}

extern "C" int32_t RF3LureModel_Draw(PlayState* play, const float* pos, float rotY, float rotX, float zOffset) {
    if (!RF3_IsEnabled()) {
        NoteState(sLastDrawState, sStatus, "model-off why=rf3-off");
        return 0;
    }
    if (!CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.LureModels"), 1)) {
        NoteState(sLastDrawState, sStatus, "model-off why=option-off");
        return 0;
    }
    std::string why;
    auto [kind, index] = ChosenItem(why);
    if (kind < 0) {
        NoteState(sLastDrawState, sStatus, "model-off why=" + why);
        return 0;
    }
    if (index < 0 || index >= kKindInfo[kind].count) {
        NoteState(sLastDrawState, sStatus,
                  std::string("model-off why=no-rf3-model kind=") + kKindInfo[kind].name + " index=" +
                      std::to_string(index));
        return 0;
    }
    LureModel* m = Get(kind, index);
    if (m == nullptr || !m->ok) {
        NoteState(sLastDrawState, sStatus,
                  std::string("model-off why=load-failed kind=") + kKindInfo[kind].name + " index=" +
                      std::to_string(index));
        return 0;
    }
    float unitsPerMetre = CVarGetFloat(kKindInfo[kind].cvar, kKindInfo[kind].defaultScale);
    bool flip = kind != kBait && CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.LureModelFlip"), 0);
    char state[200];
    std::snprintf(state, sizeof(state),
                  "model-draw kind=%s index=%d units_per_m=%.0f flip=%d preview=%d length_units=%.1f", kKindInfo[kind].name,
                  index, unitsPerMetre, flip ? 1 : 0, PreviewKind() >= 0 ? 1 : 0, LongestUnits(m, unitsPerMetre));
    NoteState(sLastDrawState, sStatus, state);

    Matrix_Push();
    Matrix_Translate(pos[0], pos[1], pos[2], MTXMODE_NEW);
    Matrix_RotateY(rotY, MTXMODE_APPLY);
    if (kind != kBait) {
        // Along the line, as OoT's lure (z_fishing.c applies sLurePosZOffset after its 0.004 scale).
        Matrix_RotateX(rotX, MTXMODE_APPLY);
        Matrix_Translate(0.0f, 0.0f, zOffset * kOotLureScale, MTXMODE_APPLY);
    } // bait: upright, hanging from its origin (the hook eye; the model extends below it)
    DrawModel(play, m, unitsPerMetre, flip);
    Matrix_Pop();
    return 1;
}

extern "C" void RF3LureModel_DrawFloat(PlayState* play, const float* baitPos, const float* rodTip, float waterY,
                                       float rotY) {
    if (!RF3_IsEnabled() || !CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.LureModels"), 1)) {
        NoteState(sLastFloatState, sFloatStatus, "float-off why=rf3-or-option-off");
        return;
    }
    RF3TackleChoice c;
    const rf3tackle::Box* box = ChosenBox(c);
    int32_t index = c.floatIdx;
    bool preview = PreviewKind() == kFloat;
    if (preview) {
        index = PreviewIndex();
    } else if (box == nullptr || box->kind != "bait") {
        NoteState(sLastFloatState, sFloatStatus, "float-off why=not-a-bait-set");
        return;
    } else if (box->layout == "menu_05") {
        NoteState(sLastFloatState, sFloatStatus, "float-off why=set-without-float"); // Bitterling Rod Set
        return;
    }
    if (index < 0 || index >= kKindInfo[kFloat].count) {
        NoteState(sLastFloatState, sFloatStatus, "float-off why=no-float index=" + std::to_string(index));
        return;
    }
    LureModel* m = Get(kFloat, index);
    if (m == nullptr || !m->ok) {
        NoteState(sLastFloatState, sFloatStatus, "float-off why=load-failed index=" + std::to_string(index));
        return;
    }
    float depthM = rf3::kBaitDepthBySetting[std::clamp(c.depth, 0, 4)];
    float at[3];
    const char* where;
    if (baitPos[1] < waterY) {
        // The bait is in the water: the float rides the surface straight above it.
        at[0] = baitPos[0];
        at[1] = waterY;
        at[2] = baitPos[2];
        where = "surface";
    } else {
        // In hand or in flight: on the line toward the rod tip, the float depth away (at most 90 % of the way).
        float d[3] = { rodTip[0] - baitPos[0], rodTip[1] - baitPos[1], rodTip[2] - baitPos[2] };
        float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        float along = std::fmin(depthM * RF3_UNITS_PER_METRE, len * kFloatMaxShare);
        for (int i = 0; i < 3; i++) {
            at[i] = baitPos[i] + (len > 0.001f ? d[i] / len * along : 0.0f);
        }
        where = "line";
    }
    float unitsPerMetre = CVarGetFloat(kKindInfo[kFloat].cvar, kKindInfo[kFloat].defaultScale);
    char state[200];
    std::snprintf(state, sizeof(state),
                  "float-draw index=%d depth_setting=%d depth_m=%.1f where=%s units_per_m=%.0f preview=%d "
                  "length_units=%.1f",
                  index, c.depth, depthM, where, unitsPerMetre, preview ? 1 : 0, LongestUnits(m, unitsPerMetre));
    NoteState(sLastFloatState, sFloatStatus, state);

    Matrix_Push();
    Matrix_Translate(at[0], at[1], at[2], MTXMODE_NEW);
    Matrix_RotateY(rotY, MTXMODE_APPLY); // upright; the model stands above its origin (the line tie at its foot)
    DrawModel(play, m, unitsPerMetre, false);
    Matrix_Pop();
}

extern "C" const char* RF3LureModel_Status(void) {
    return sStatus.c_str();
}

extern "C" const char* RF3LureModel_FloatStatus(void) {
    return sFloatStatus.c_str();
}
