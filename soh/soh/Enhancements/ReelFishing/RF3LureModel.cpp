// Reel Fishing III mod (steps 25c-2, 25d-2): RF3's own lure, fly, bait and float models on the line. See
// RF3LureModel.h and docs/DESIGN.md "25c design", "25d design".
//
// Each model is rf3models/<kind>/<file>.json in mods/rf3_models.o2r: vertex batches of at most 32 (positions in
// 0.1 mm, texture coordinates in declared texels x 32, normals) per surface, and the surface's texture, an
// HD (version 1) texture resource loaded at its small declared size. The display list is built once per model
// and kept. RF3 models are y-down with the front at -z; every draw turns them half about x.
//
// Step 27b/27c (RF3-MECHANICS 6.6): a lure with a skinned copy is drawn with RF3SkinnedModel (as the fish and rods),
// at the same origin as the rigid model, playing its RF3 motions; hooked, it hangs by its hook from the fish's mouth.
//
// Log, don't look: logs/rf3_models.txt gets one line per model loaded (counts, missing textures) and one line
// whenever what is drawn changes (item, scale, flip, the float's place, or why OoT's lure is drawn instead), plus
// "lure-motion" on each motion change and "lure-attach" on each hook set.

#include "RF3LureModel.h"

#include <algorithm>
#include <array>
#include <cstdlib>
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
#include "RF3SkinnedModel.h"
#include "RF3FishModel.h"
#include "RF3RodModel.h"

extern "C" {
#include "z64.h"
#include "functions.h"
#include "macros.h"
#include "variables.h"
#include "objects/object_fish/object_fish.h" // step 40b: OoT's pole lures
// OPEN_DISPS / CLOSE_DISPS (as kaleido.cpp declares them)
void FrameInterpolation_RecordOpenChild(const void* a, int b);
void FrameInterpolation_RecordCloseChild(void);
}

namespace {

using json = nlohmann::json;

constexpr const char* kOtrPrefix = "__OTR__";
constexpr float kOotLureScale = 0.0039999997f; // z_fishing.c's lure model scale (sLurePosZOffset is in it)

// The model kinds, as tools/rf3_models.py MODEL_KINDS writes them. Item N = file N [assumed: the counts match].
// step 33b: kHook, the bait rig's bare hook; step 35c-3: kBaitMenu, the bait alone (RF3's model/bait/btNNN) for the
// item menu's preview (kBait is the bait on its hook, as on the line)
enum Kind { kLure, kFly, kBait, kFloat, kHook, kBaitMenu, kKinds };
struct KindInfo {
    const char* name;
    const char* file;   // printf pattern of the archive path
    int32_t count;      // lures 0..90 (OoT's pole lures 91/92 keep OoT's models), flies, bait groups, floats
    const char* cvar;   // size, OoT units per RF3 metre
    float defaultScale; // [assumed] (DESIGN 25c/25d): lure 4.4 cm ~ OoT's lure; fly, bait visible; float real size
};
constexpr KindInfo kKindInfo[kKinds] = {
    // step 36a (user, 2026-10-14): default 70 (was 270, OoT's lure size); step 38 (user, 2026-10-16): bait (and so
    // the bare hook) default 200 (was 400)
    { "lure", "rf3models/lure/lur%03d.json", 91, CVAR_ENHANCEMENT("ReelFishing3.LureModelScale"), 70.0f },
    { "fly", "rf3models/fly/fly%03d.json", 40, CVAR_ENHANCEMENT("ReelFishing3.FlyModelScale"), 600.0f },
    { "bait", "rf3models/bait/baithook%03d.json", 12, CVAR_ENHANCEMENT("ReelFishing3.BaitModelScale"), 200.0f },
    { "float", "rf3models/float/float%03d.json", 6, CVAR_ENHANCEMENT("ReelFishing3.FloatModelScale"),
      RF3_UNITS_PER_METRE },
    // step 33b: RF3's 7 hooks (tackle.trf 0x300 + n), at the bait's size so the hook matches its bait (33b-4, user:
    // 33b-2's own slider removed once 33b-3 made them visible)
    { "hook", "rf3models/hook/hook%03d.json", 7, CVAR_ENHANCEMENT("ReelFishing3.BaitModelScale"), 200.0f },
    // step 35c-3: bait type n = btNNN [assumed, as baithook n]; only the item menu's preview (sized there)
    { "baitmenu", "rf3models/baitmenu/bt%03d.json", 12, CVAR_ENHANCEMENT("ReelFishing3.BaitModelScale"), 200.0f },
};
constexpr uint8_t kUntexturedGrey[3] = { 170, 170, 178 }; // step 33b-3: untextured surfaces [assumed]; step 37: bare hooks have a texture now, so a fallback
constexpr float kFloatMaxShare = 0.9f; // out of the water: at most this share of the way from the bait to the rod
// Step 29e: the float's idle bob on the surface. RF3 has no float in the water (RF3-MECHANICS 2.9, 29b), so this is
// the mod's own design [assumed], tuned with the debug window's sliders.
constexpr float kDefaultFloatBobM = 0.01f;      // amplitude, metres
constexpr float kDefaultFloatBobPeriod = 2.5f;  // seconds
constexpr float kGameFrameSeconds = 1.0f / 20.0f; // play->gameplayFrames ticks at 20 Hz
// Step 29f: the nibble dip per WAIT [assumed, sliders]
constexpr float kDefaultFloatDipM = 0.03f;
constexpr float kDefaultFloatDipDown = 0.1f;
constexpr float kDefaultFloatDipUp = 0.4f;
// Step 29g: going under at the bite [assumed, sliders]
constexpr float kDefaultFloatUnderTime = 0.2f; // seconds to get there
// Step 31b: back to the surface (or the line) when the fight ends [assumed, slider]
constexpr float kDefaultFloatReturnTime = 0.3f;
// Step 27b: RF3's lure motions (RF3-MECHANICS 6.6).
constexpr int32_t kLureMotionRod = 17;      // the ready pose at the rod (0x0017FCF0, 0x00180800), blend 0
constexpr int32_t kLureMotionSink = 4;      // at rest, sinking actions 0/3/5 (0x002018E0), and float-up action 4
constexpr int32_t kLureMotionBottom = 6;    // at rest on the bottom, sinking actions
constexpr int32_t kLureMotionRise = 3;      // at rest, float-up actions 1/2, below the surface; [recorded] "moving"
// Step 27d (RF3 recording rf3live_20261013_154219, lure 48 Suspended Vibration G, rest action 2): reeling plays 1, a
// lure at rest plays 3 while it moves and 5 when still, a twitch changes nothing, the fight keeps the last motion.
constexpr int32_t kLureMotionReel = 1;
constexpr int32_t kLureMotionIdle = 5;
constexpr float kLureMoveRiseMps = 0.006f;  // [recorded] rising faster than 0.0001 m per RF3 frame -> 3
constexpr float kLureMoveHorizMps = 0.06f;  // [recorded] drifting faster than 0.001 m per RF3 frame -> 3
// Step 27d-2 (recording rf3live_20261013_160939, lures 27/36/39/42/63): in the fight RF3's lure keeps its own states:
// reel held -> 1; released -> the moving motion for 0.45-0.9 s, then still. [assumed] 0.5 s.
constexpr float kLureFightSettleSeconds = 0.5f;
constexpr float kLureRestBlend = 0.3f;      // 0x002018E0's blend (+0xEC)
constexpr float kFrameSeconds = 1.0f / 20.0f;

using Quat = std::array<float, 4>; // w, x, y, z

// One lure skin's motions (the fish's 25g-1 layout).
struct LureMotions {
    bool ok = false;
    int32_t frames = 0;
    std::vector<std::array<float, 3>> T;
    std::vector<Quat> R;
    std::vector<uint16_t> tidx;
    std::vector<uint16_t> ridx;
    struct Motion {
        int32_t start = -1;
        int32_t keys = 0;
        std::vector<float> times;
        float period = 0.0f;
        bool loop = false;
    };
    std::vector<Motion> motions;
    // 27b fix: the body bone's bind world rotation (the rod pose turns the body; the mod keeps this turn)
    int32_t body = -1;
    rf3model::Mat3 bodyBind{};
    // 27e-1: the model origin (the rigid lure's line tie) in the tie bone's bind frame, metres; 27e-4: the tie bone
    // is the one with the most drawn vertices (not always body_bone)
    int32_t tieBone = -1;
    std::array<float, 3> tieLocal = { 0, 0, 0 };
};

// The lure's motion player (one lure on the line at a time).
struct LureAnim {
    std::string file;
    int32_t motion = -1;
    float time = 0.0f;
    int32_t prevMotion = -1;
    float prevTime = 0.0f;
    float blend = 1.0f;
    float blendTime = 0.0f;
    uint32_t lastFrame = 0;
    bool haveFrame = false;
    std::string why;
    // step 27c: the hook set
    int32_t hookedSlot = -1;
    int32_t hookPoint = -1;
    // step 27d: the lure's movement since the last game frame (RF3's rest motion: 3 moving, 5 still)
    bool haveMove = false;
    float prevDepth = 0.0f;
    float prevPos[3] = { 0, 0, 0 };
    float riseMps = 0.0f;
    float horizMps = 0.0f;
    // step 27d-2: for the fight (the lure sim is off then)
    int32_t lastRestAction = -1;
    float settleLeft = 0.0f;
    float dt = 0.0f;
};

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
    float center[3] = { 0, 0, 0 }; // step 28a: pos units, model space (the item-get draw centres on it)
};

std::map<std::pair<int32_t, int32_t>, std::unique_ptr<LureModel>> sModels; // (kind, index)
std::map<std::string, std::unique_ptr<LureMotions>> sLureMotions;           // step 27b, by skin file
LureAnim sLureAnim;
bool sTieValid = false; // 27c fix: this frame's line tie, for z_fishing.c's hooked line
float sTie[3] = {};
// 27e-1: this frame's tie in every state (for the lure-tie log), how far the pose moved it from the bind origin, and
// whether the lure hung from the fish's mouth
bool sTieAnyValid = false;
float sTieAny[3] = {};
float sTiePoseShift = 0.0f;
bool sTieHanging = false;
int32_t sTieLogCast = -1;
uint32_t sTieLogFrame = 0;
std::map<int32_t, bool> sSkinMissing; // lure index -> no skinned copy (lure 89, or an old archive): drawn rigid
std::string sLastDrawState;
std::string sStatus = "nothing drawn yet";
std::string sLastFloatState;
std::string sFloatBobState; // step 29e: the last float-bob log line (logged on change)
// step 29f: the nibble dip (the chaser and its WAIT count already dipped for)
int32_t sDipChaser = -1;
int32_t sDipWaits = 0;
uint32_t sDipStartFrame = 0;
bool sDipActive = false;
// step 29g: the float under water while hooked
bool sUnderActive = false;
uint32_t sUnderStartFrame = 0;
float sUnderFrom[3] = {};
float sFloatLastAt[3] = {};
// step 31b: the float's return from where the fight left it
bool sReturnActive = false;
uint32_t sReturnStartFrame = 0;
float sReturnFrom[3] = {};
// step 29g-2: the float was drawn this frame at sFloatLastAt (z_fishing.c threads the line through it)
bool sFloatDrawn = false;
uint32_t sFloatDrawnFrame = 0;
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
    if (j.contains("center") && j["center"].size() == 3) {
        for (int i = 0; i < 3; i++) {
            m.center[i] = j["center"][i].get<float>();
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
                gDPSetRenderMode(g++, G_RM_AA_ZB_TEX_EDGE, G_RM_AA_ZB_TEX_EDGE2);
                gDPSetCombineMode(g++, G_CC_MODULATEIDECALA, G_CC_MODULATEIDECALA);
                gSPTexture(g++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON);
                gDPLoadTextureBlock(g++, s.tex, G_IM_FMT_RGBA, G_IM_SIZ_32b, s.w, s.h, 0, G_TX_NOMIRROR | G_TX_WRAP,
                                    G_TX_NOMIRROR | G_TX_WRAP, Log2(s.w), Log2(s.h), G_TX_NOLOD, G_TX_NOLOD);
            } else {
                // Step 33b-3: untextured: opaque, flat grey. TEX_EDGE + shade drew nothing visible (33b/33b-2, user)
                // [grey assumed]. Step 37: the bare hooks carry the mod's metal swatch, so only a missing texture
                // lands here.
                gDPSetRenderMode(g++, G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2);
                gDPSetPrimColor(g++, 0, 0, kUntexturedGrey[0], kUntexturedGrey[1], kUntexturedGrey[2], 255);
                gDPSetCombineMode(g++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);
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
        if (RF3Tackle_LureMissing()) {
            return { kHook, RF3Tackle_HookIndex() }; // step 33b: no bait on the hook (taken, or "---"): the bare hook
        }
        return { kBait, c.bait };
    }
    why = "box=" + std::to_string(c.box) + " kind=" + (boxKind.empty() ? "?" : boxKind);
    return { -1, -1 };
}

// ---- step 27b: the lure's motions

Quat AxisAngleQuat(float x, float y, float z, float angle) {
    float ln = std::sqrt(x * x + y * y + z * z);
    if (ln < 1e-9f || std::fabs(angle) < 1e-12f) {
        return { 1.0f, 0.0f, 0.0f, 0.0f };
    }
    float s = std::sin(angle * 0.5f) / ln;
    return { std::cos(angle * 0.5f), x * s, y * s, z * s };
}

Quat Slerp(Quat a, const Quat& b, float u) {
    float dt = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (dt < 0.0f) {
        for (float& c : a) {
            c = -c;
        }
        dt = -dt;
    }
    float wa, wb;
    if (dt > 0.9995f) {
        wa = 1.0f - u;
        wb = u;
    } else {
        float th = std::acos(std::min(1.0f, dt));
        float sn = std::sin(th);
        wa = std::sin((1.0f - u) * th) / sn;
        wb = std::sin(u * th) / sn;
    }
    Quat q = { wa * a[0] + wb * b[0], wa * a[1] + wb * b[1], wa * a[2] + wb * b[2], wa * a[3] + wb * b[3] };
    float ln = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (float& c : q) {
        c /= (ln > 0.0f ? ln : 1.0f);
    }
    return q;
}

rf3model::Mat3 QuatToMat(const Quat& q) {
    float w = q[0], x = q[1], y = q[2], z = q[3];
    rf3model::Mat3 m;
    m[0] = { 1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y) };
    m[1] = { 2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x) };
    m[2] = { 2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y) };
    return m;
}

LureMotions* GetLureMotions(const rf3model::SkinnedModel& m) {
    auto it = sLureMotions.find(m.file);
    if (it != sLureMotions.end()) {
        return it->second.get();
    }
    auto set = std::make_unique<LureMotions>();
    if (m.extra.contains("motion")) {
        const auto& j = m.extra["motion"];
        set->frames = j.value("frames", 0);
        for (const auto& t : j["T"]) {
            set->T.push_back({ t[0].get<float>(), t[1].get<float>(), t[2].get<float>() });
        }
        for (const auto& r : j["R"]) { // R(axis, -angle): RF3's angles are negated (25b)
            set->R.push_back(AxisAngleQuat(r[0].get<float>(), r[1].get<float>(), r[2].get<float>(), -r[3].get<float>()));
        }
        for (const auto& v : j["tidx"]) {
            set->tidx.push_back(v.get<uint16_t>());
        }
        for (const auto& v : j["ridx"]) {
            set->ridx.push_back(v.get<uint16_t>());
        }
        for (const auto& mo : j["motions"]) {
            LureMotions::Motion x;
            x.start = mo["start"].get<int32_t>();
            x.keys = mo["keys"].get<int32_t>();
            for (const auto& t : mo["times"]) {
                x.times.push_back(std::max(0.001f, t.get<float>()));
                x.period += x.times.back();
            }
            x.loop = mo.value("loop", false);
            set->motions.push_back(x);
        }
        const size_t need = static_cast<size_t>(set->frames) * m.bones.size();
        set->ok = set->frames > 0 && set->tidx.size() == need && set->ridx.size() == need &&
                  std::all_of(set->tidx.begin(), set->tidx.end(), [&](uint16_t v) { return v < set->T.size(); }) &&
                  std::all_of(set->ridx.begin(), set->ridx.end(), [&](uint16_t v) { return v < set->R.size(); });
    }
    set->body = m.extra.value("body_bone", -1);
    if (set->body >= 0 && set->body < static_cast<int32_t>(m.bones.size())) {
        std::vector<rf3model::Mat3> brot;
        std::vector<std::array<float, 3>> bpos;
        rf3model::BoneWorlds(m, nullptr, brot, bpos);
        set->bodyBind = brot[set->body];
        // 27e-4: the tie is carried by the bone with the most drawn vertices. On every lure that bone sits at the
        // origin and holds still in the motions; body_bone (the tool's per-bone vertex stat) differs on lures 2, 79
        // and 80, where it is a swinging part (lure 2: bone 14, 30 deg in the reel motion, the tie ~19 mm down).
        set->tieBone = set->body;
        for (int32_t b = 0; b < static_cast<int32_t>(m.boneVerts.size()); b++) {
            if (m.boneVerts[b] > m.boneVerts[set->tieBone]) {
                set->tieBone = b;
            }
        }
        set->bodyBind = brot[set->tieBone]; // 27e-5: the rod fix keeps the main body's bind turn
        // 27e-1: the origin is VERT = 0, i.e. bone space -offset (VERT = World * TVRT + offset);
        // tieLocal = Rbind^T * (-offset - pbind)
        const int32_t tb = set->tieBone;
        const auto& pb = bpos[tb];
        float d[3];
        for (int i = 0; i < 3; i++) {
            d[i] = -m.offset[i] / m.posScale - pb[i];
        }
        for (int i = 0; i < 3; i++) {
            set->tieLocal[i] = brot[tb][0][i] * d[0] + brot[tb][1][i] * d[1] + brot[tb][2][i] * d[2];
        }
    } else {
        set->body = -1;
    }
    Log("lure-motions-load file=" + m.file + " ok=" + std::to_string(set->ok ? 1 : 0) +
        " motions=" + std::to_string(set->motions.size()) + " body_bone=" + std::to_string(set->body) +
        " tie_bone=" + std::to_string(set->tieBone));
    LureMotions* out = set.get();
    sLureMotions[m.file] = std::move(set);
    return out;
}

bool LurePlayable(const LureMotions& s, int32_t motion) {
    return motion >= 0 && motion < static_cast<int32_t>(s.motions.size()) && s.motions[motion].keys >= 2 &&
           s.motions[motion].start >= 0 && s.motions[motion].start + s.motions[motion].keys <= s.frames;
}

// Bone b's local pose in motion at time t (loops wrap, the rest hold their end).
void LureSample(const LureMotions& s, int32_t motion, float t, int32_t b, std::array<float, 3>& outT, Quat& outR) {
    const auto& mo = s.motions[motion];
    if (mo.loop && mo.period > 0.0f) {
        t = std::fmod(std::max(0.0f, t), mo.period);
    } else {
        t = std::clamp(t, 0.0f, mo.period);
    }
    int32_t k = 0;
    while (k < mo.keys - 2 && t >= mo.times[k]) {
        t -= mo.times[k];
        k++;
    }
    float u = std::clamp(t / mo.times[k], 0.0f, 1.0f);
    const size_t f0 = static_cast<size_t>(b) * s.frames + mo.start + k;
    const auto& t0 = s.T[s.tidx[f0]];
    const auto& t1 = s.T[s.tidx[f0 + 1]];
    for (int i = 0; i < 3; i++) {
        outT[i] = t0[i] + (t1[i] - t0[i]) * u;
    }
    outR = Slerp(s.R[s.ridx[f0]], s.R[s.ridx[f0 + 1]], u);
}

// The rest motion while the lure moves, by its RLRE +0x0E rest action [code 0x002018E0, recorded for 0, 1, 2, 4]:
// sinking actions 0/3/5 and the topwater action 4 play 4, float-up actions 1/2 play 3.
int32_t MovingMotion(int32_t restAction) {
    return (restAction == 1 || restAction == 2) ? kLureMotionRise : kLureMotionSink;
}

bool Sinks(int32_t restAction) {
    return restAction == 0 || restAction == 3 || restAction == 5;
}

// RF3's motion choice (RF3-MECHANICS 6.6; 27d recordings). Returns the motion to request, or -1 to keep the current
// one (in flight and while landing RF3 changes nothing); blend set.
int32_t ChooseLureMotion(int32_t castState, bool reelHeld, LureAnim& a, float& blend, std::string& why) {
    if (castState == 0) {
        blend = 0.0f;
        why = "rod";
        return kLureMotionRod;
    }
    blend = kLureRestBlend;
    if (castState == 4) {
        // hooked [recorded]: the lure's own states go on; the mod's lure sim is off, so the reel button drives it
        if (a.lastRestAction < 0) {
            return -1;
        }
        if (reelHeld) {
            a.settleLeft = kLureFightSettleSeconds;
            why = "fight-reel";
            return kLureMotionReel;
        }
        if (a.settleLeft > 0.0f) {
            a.settleLeft -= a.dt;
            why = "fight-settle";
            return MovingMotion(a.lastRestAction);
        }
        if (Sinks(a.lastRestAction)) {
            return -1; // [recorded] the jig kept its rest motion through the fight
        }
        why = "fight-still";
        return kLureMotionIdle;
    }
    if (castState != 3) {
        return -1; // flight, landing: the last motion plays on [recorded]
    }
    RF3LureStatus st;
    RF3Lure_GetStatus(&st);
    if (!st.active || st.kind != 0) {
        return -1;
    }
    if (st.restAction >= 0) {
        a.lastRestAction = st.restAction;
    }
    a.settleLeft = 0.0f;
    if (st.mode == 1) {
        why = "reel"; // [recorded]; a twitch keeps it [recorded]
        return kLureMotionReel;
    }
    if (st.mode != 2 || st.restAction < 0) {
        return -1;
    }
    if (Sinks(st.restAction)) { // [recorded, jig action 0] 4 while sinking, 6 on the bottom
        why = st.bottom ? "rest-bottom" : "rest-sink";
        return st.bottom ? kLureMotionBottom : kLureMotionSink;
    }
    // float-up actions 1/2/4: [recorded] the moving motion (3, or 4 for action 4) while it moves, 5 when still
    bool moving = a.riseMps > kLureMoveRiseMps || a.horizMps > kLureMoveHorizMps;
    why = moving ? "rest-moving" : "rest-still";
    return moving ? MovingMotion(st.restAction) : kLureMotionIdle;
}

// Advances the lure's motion player one game frame and fills the bones' world rotations and positions (metres).
bool LurePose(const rf3model::SkinnedModel& m, int32_t index, int32_t castState, bool reelHeld, uint32_t frame,
              const float* lurePos,
              std::vector<rf3model::Mat3>& rot, std::vector<std::array<float, 3>>& pos) {
    LureMotions* s = GetLureMotions(m);
    if (s == nullptr || !s->ok) {
        return false;
    }
    LureAnim& a = sLureAnim;
    if (a.file != m.file) {
        a = LureAnim();
        a.file = m.file;
    }
    float dt = 0.0f;
    if (a.haveFrame) {
        dt = std::clamp(static_cast<int32_t>(frame - a.lastFrame), 0, 4) * kFrameSeconds;
    }
    a.lastFrame = frame;
    a.haveFrame = true;
    {
        // step 27d: rise speed (depth getting smaller) and horizontal speed, m/s, over this game frame
        RF3LureStatus st;
        RF3Lure_GetStatus(&st);
        if (st.active && a.haveMove && dt > 0.0f) {
            a.riseMps = (a.prevDepth - st.depthM) / dt;
            float dx = lurePos[0] - a.prevPos[0];
            float dz = lurePos[2] - a.prevPos[2];
            a.horizMps = std::sqrt(dx * dx + dz * dz) / RF3_UNITS_PER_METRE / dt;
        } else if (!st.active) {
            a.riseMps = a.horizMps = 0.0f;
        }
        if (dt > 0.0f || !a.haveMove) {
            a.haveMove = st.active != 0;
            a.prevDepth = st.depthM;
            for (int32_t i = 0; i < 3; i++) {
                a.prevPos[i] = lurePos[i];
            }
        }
    }
    float blend = 0.0f;
    std::string why;
    a.dt = dt;
    int32_t want = ChooseLureMotion(castState, reelHeld, a, blend, why);
    if (want >= 0 && want != a.motion && LurePlayable(*s, want)) {
        a.prevMotion = a.motion;
        a.prevTime = a.time;
        a.motion = want;
        a.time = 0.0f;
        a.blendTime = blend;
        a.blend = (a.prevMotion >= 0 && blend > 0.0f) ? 0.0f : 1.0f;
        a.why = why;
        char line[160];
        std::snprintf(line, sizeof(line), "lure-motion lure=%d motion=%d from=%d blend=%.2f why=%s cast=%d", index,
                      want, a.prevMotion, blend, why.c_str(), castState);
        Log(line);
    }
    if (!LurePlayable(*s, a.motion)) {
        a.motion = kLureMotionRod; // nothing chosen yet (first frame in the water): the pose RF3 starts from
        if (!LurePlayable(*s, a.motion)) {
            return false;
        }
    }
    a.time += dt;
    if (a.prevMotion >= 0) {
        a.prevTime += dt;
    }
    a.blend = (a.blendTime > 0.0f) ? std::min(1.0f, a.blend + dt / a.blendTime) : 1.0f;
    const size_t n = m.bones.size();
    std::vector<std::array<float, 3>> lt(n);
    std::vector<rf3model::Mat3> lr(n);
    for (size_t b = 0; b < n; b++) {
        std::array<float, 3> t;
        Quat q;
        LureSample(*s, a.motion, a.time, static_cast<int32_t>(b), t, q);
        if (a.blend < 1.0f && LurePlayable(*s, a.prevMotion)) {
            std::array<float, 3> pt;
            Quat pq;
            LureSample(*s, a.prevMotion, a.prevTime, static_cast<int32_t>(b), pt, pq);
            for (int i = 0; i < 3; i++) {
                t[i] = pt[i] + (t[i] - pt[i]) * a.blend;
            }
            q = Slerp(pq, q, a.blend);
        }
        lt[b] = t;
        lr[b] = QuatToMat(q);
    }
    rf3model::BoneWorldsLocal(m, lt, lr, rot, pos);
    // 27e-5: turned about the main body bone (tieBone): body_bone is a swinging part on lures 2, 79, 80 (user: lure 2
    // hung horizontal, 79/80 vertical but offset up and away)
    if (a.motion == kLureMotionRod && s->tieBone >= 0) {
        // 27b fix (user, 2026-10-12: "When hanging on the rod while not cast the lure is horizontal and upside
        // down"): RF3's rod pose turns the body 78-90 deg to hang it, and the mod already hangs the lure along the
        // line (as the rigid lure). Turn the whole pose back about the body so the body keeps its bind turn; the
        // hooks keep the pose's swing relative to it. C = Rbind(body) * Rpose(body)^T.
        const rf3model::Mat3& rb = s->bodyBind;
        const rf3model::Mat3& rp = rot[s->tieBone];
        rf3model::Mat3 rpt;
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) {
                rpt[i][j] = rp[j][i];
            }
        }
        rf3model::Mat3 c = rf3model::Mul(rb, rpt);
        const std::array<float, 3> pb = pos[s->tieBone];
        for (size_t b = 0; b < n; b++) {
            rot[b] = rf3model::Mul(c, rot[b]);
            float d[3] = { pos[b][0] - pb[0], pos[b][1] - pb[1], pos[b][2] - pb[2] };
            for (int i = 0; i < 3; i++) {
                pos[b][i] = pb[i] + c[i][0] * d[0] + c[i][1] * d[1] + c[i][2] * d[2];
            }
        }
    }
    return true;
}

// Step 27e-3 (non-RF3, the user's choice): the bones hidden while the lure hangs from the fish's mouth: the hook bone
// (role 0) and the bones under it that carry only hook surfaces. Empty when the hook is part of the body mesh
// (lures 2, 66-90): those keep RF3's look.
constexpr float kHookHideScale = 1e-4f; // not 0: the hidden vertices' normals stay finite

// The lure's hook bone (role 0), or -1.
int32_t HookBoneOf(const rf3model::SkinnedModel& m) {
    return m.extra.contains("locators") && m.extra["locators"].contains("0") ? m.extra["locators"]["0"].get<int32_t>()
                                                                               : -1;
}

std::vector<int32_t> HookHideBones(const rf3model::SkinnedModel& m, int32_t hookBone) {
    std::vector<int32_t> out;
    const int32_t n = static_cast<int32_t>(m.bones.size());
    if (hookBone < 0 || hookBone >= n || static_cast<int32_t>(m.boneVerts.size()) != n) {
        return out;
    }
    for (int32_t b = 0; b < n; b++) {
        int32_t up = b;
        while (up >= 0 && up != hookBone) {
            up = m.bones[up].parent;
        }
        if (up == hookBone && m.boneVerts[b] > 0 && m.boneHookVerts[b] == m.boneVerts[b]) {
            out.push_back(b);
        }
    }
    return out;
}

// The lure's skinned copy, or nullptr (logged once per lure).
rf3model::SkinnedModel* GetLureSkin(int32_t index) {
    if (sSkinMissing.count(index)) {
        return nullptr;
    }
    char path[64];
    std::snprintf(path, sizeof(path), "rf3models/lure_skin/lur%03d.json", index);
    rf3model::SkinnedModel* m = rf3model::Get(path, "lure-skin", "lure=" + std::to_string(index));
    if (m == nullptr || !m->ok) {
        sSkinMissing[index] = true;
        Log("lure-skin-off lure=" + std::to_string(index) + " why=" + (m ? m->why : std::string("not loaded")) +
            " (drawn rigid)");
        return nullptr;
    }
    return m;
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

extern "C" int32_t RF3LureModel_Draw(PlayState* play, const float* pos, float rotY, float rotX, float zOffset,
                                     int32_t castState, int32_t hookedSlot, uint32_t frame) {
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
    if (kind == kHook && (castState == 4 || castState == 5)) {
        // Step 33b: the bare hook in the fish's mouth is hidden, as 27e-3's lure hooks (a non-RF3 choice).
        NoteState(sLastDrawState, sStatus, "model-off why=bare-hook-in-mouth index=" + std::to_string(index));
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
    bool flip = kind != kBait && kind != kHook && CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.LureModelFlip"), 0);
    char state[200];
    std::snprintf(state, sizeof(state),
                  "model-draw kind=%s index=%d units_per_m=%.0f flip=%d preview=%d length_units=%.1f", kKindInfo[kind].name,
                  index, unitsPerMetre, flip ? 1 : 0, PreviewKind() >= 0 ? 1 : 0, LongestUnits(m, unitsPerMetre));
    NoteState(sLastDrawState, sStatus, state);

    sTieValid = false;
    sTieAnyValid = false;
    // Step 27b/27c: a lure with a skinned copy is drawn with its bones and RF3's motions.
    rf3model::SkinnedModel* skin =
        (kind == kLure && CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.LureMotions"), 1)) ? GetLureSkin(index)
                                                                                          : nullptr;
    std::vector<rf3model::Mat3> brot;
    std::vector<std::array<float, 3>> bpos;
    // step 27d-2: the reel (A, as z_fishing.c's RF3 fight reads it) drives the hooked lure's motion
    const bool reelHeld = CHECK_BTN_ALL(play->state.input[0].cur.button, BTN_A);
    if (skin != nullptr && !LurePose(*skin, index, castState, reelHeld, frame, pos, brot, bpos)) {
        skin = nullptr;
    }
    if (skin == nullptr && !(castState == 4 || castState == 5)) {
        sLureAnim.hookedSlot = -1;
    }

    // Step 27c: hooked, the lure hangs by its hook from the fish's mouth point (RF3's 0x00188120).
    bool hanging = false;
    float mouth[3];
    float frot[9];
    if (skin != nullptr && (castState == 4 || castState == 5) && hookedSlot >= 0) {
        LureAnim& a = sLureAnim;
        if (a.hookedSlot != hookedSlot) { // the hook was set: RF3 picks one of the fish's 3 mouth points
            a.hookedSlot = hookedSlot;
            a.hookPoint = std::rand() % 3;
            char line[120];
            std::snprintf(line, sizeof(line), "lure-attach lure=%d slot=%d point=%d", index, hookedSlot, a.hookPoint);
            Log(line);
            const int32_t hookBone = HookBoneOf(*skin);
            std::string hidden;
            for (int32_t b : HookHideBones(*skin, hookBone)) {
                hidden += (hidden.empty() ? "" : ",") + std::to_string(b);
            }
            Log("lure-hook-hide lure=" + std::to_string(index) + " hook_bone=" + std::to_string(hookBone) +
                " bones=" + (hidden.empty() ? std::string("none (hook in the body mesh: shown)") : hidden));
        }
        uint32_t fishFrame = 0;
        int32_t point = a.hookPoint;
        if (!RF3FishModel_HookInfo(hookedSlot, point, mouth, frot, &fishFrame)) {
            point = 0; // the model has no such point: the snout
        }
        hanging = RF3FishModel_HookInfo(hookedSlot, point, mouth, frot, &fishFrame) && frame - fishFrame <= 2;
    } else if (!(castState == 4 || castState == 5)) {
        sLureAnim.hookedSlot = -1;
    }

    Matrix_Push();
    if (hanging) {
        // The fish's frame at the mouth point, then the lure moved so its hook bone (role 0) sits there.
        MtxF f;
        for (int32_t c = 0; c < 3; c++) {
            for (int32_t r = 0; r < 3; r++) {
                f.mf[c][r] = frot[c * 3 + r];
            }
            f.mf[c][3] = 0.0f;
        }
        f.mf[3][0] = mouth[0];
        f.mf[3][1] = mouth[1];
        f.mf[3][2] = mouth[2];
        f.mf[3][3] = 1.0f;
        Matrix_Put(&f);
        int32_t hb = skin->extra.contains("locators") && skin->extra["locators"].contains("0")
                         ? skin->extra["locators"]["0"].get<int32_t>()
                         : -1;
        if (hb >= 0 && hb < static_cast<int32_t>(bpos.size())) {
            float s = unitsPerMetre / skin->posScale;
            // the hook bone's origin in the drawn frame: scale, half turn about x (y, z negate), pre = offset
            float hx = s * (skin->offset[0] + bpos[hb][0] * skin->posScale);
            float hy = -s * (skin->offset[1] + bpos[hb][1] * skin->posScale);
            float hz = -s * (skin->offset[2] + bpos[hb][2] * skin->posScale);
            Matrix_Translate(-hx, -hy, -hz, MTXMODE_APPLY);
        }
    } else {
        Matrix_Translate(pos[0], pos[1], pos[2], MTXMODE_NEW);
        Matrix_RotateY(rotY, MTXMODE_APPLY);
        if (kind != kBait && kind != kHook) { // step 33b: the bare hook hangs as the bait does
            // Along the line, as OoT's lure (z_fishing.c applies sLurePosZOffset after its 0.004 scale).
            Matrix_RotateX(rotX, MTXMODE_APPLY);
            Matrix_Translate(0.0f, 0.0f, zOffset * kOotLureScale, MTXMODE_APPLY);
        } // bait: upright, hanging from its origin (the hook eye; the model extends below it)
    }
    // The line's tie: the lure's origin (where the line met the rigid lure). 27e-1: on a skinned lure it moves with
    // the posed body bone (the motions turn the body, so the bind origin can sit off the drawn nose).
    Vec3f tieDrawn = { 0.0f, 0.0f, 0.0f };
    if (skin != nullptr) {
        LureMotions* lm = GetLureMotions(*skin);
        if (lm != nullptr && lm->tieBone >= 0 && lm->tieBone < static_cast<int32_t>(bpos.size())) {
            const auto& r = brot[lm->tieBone];
            const auto& p = bpos[lm->tieBone];
            const auto& tl = lm->tieLocal;
            float v[3]; // pos units, VERT space
            for (int32_t i = 0; i < 3; i++) {
                v[i] = skin->offset[i] + (r[i][0] * tl[0] + r[i][1] * tl[1] + r[i][2] * tl[2] + p[i]) * skin->posScale;
            }
            // the drawn frame: scale, half turn about x (y, z negate), as the hook bone above
            float s = unitsPerMetre / skin->posScale;
            tieDrawn = { s * v[0], -s * v[1], -s * v[2] };
        }
    }
    {
        Vec3f tie;
        Matrix_MultVec3f(&tieDrawn, &tie);
        sTieAny[0] = tie.x;
        sTieAny[1] = tie.y;
        sTieAny[2] = tie.z;
        sTieAnyValid = true;
        sTiePoseShift = std::sqrt(tieDrawn.x * tieDrawn.x + tieDrawn.y * tieDrawn.y + tieDrawn.z * tieDrawn.z);
        sTieHanging = hanging;
        if (hanging) { // z_fishing.c draws the hooked line to it
            sTie[0] = tie.x;
            sTie[1] = tie.y;
            sTie[2] = tie.z;
            sTieValid = true;
        }
    }
    if (skin != nullptr && hanging) {
        // Step 27e-3 (non-RF3): the hook in the fish's mouth is hidden: its bones shrink to their origins (the hook
        // bone's origin is its eye, at the mouth point). After the tie above, which uses the body bone.
        for (int32_t b : HookHideBones(*skin, HookBoneOf(*skin))) {
            for (auto& row : brot[b]) {
                for (float& v : row) {
                    v *= kHookHideScale;
                }
            }
        }
    }
    if (skin != nullptr) {
        // same origin as the rigid model: VERT = World * TVRT + offset (pre); RF3SkinnedModel scales and turns it
        RF3Skinned_Draw(play, *skin, brot, bpos, unitsPerMetre / skin->posScale, 0, skin->offset, {});
    } else {
        DrawModel(play, m, unitsPerMetre, flip);
    }
    Matrix_Pop();
    return 1;
}

extern "C" void RF3LureModel_DrawFloat(PlayState* play, const float* baitPos, const float* rodTip, float waterY,
                                       float rotY, int32_t castState) {
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
    // Step 29g: from the bite (cast state 4: hooked) the float goes under and follows the fish.
    if (castState != 4) {
        // Step 31b: the fight ended (lost or landed): blend back from where the float was last drawn, if that was
        // the frame before (else it was off screen or not drawn, and there is nothing to blend from).
        if (sUnderActive && sFloatDrawn && play->gameplayFrames - sFloatDrawnFrame <= 1) {
            sReturnActive = true;
            sReturnStartFrame = play->gameplayFrames;
            for (int32_t i = 0; i < 3; i++) {
                sReturnFrom[i] = sFloatLastAt[i];
            }
        }
        sUnderActive = false;
    } else if (!sUnderActive) {
        sReturnActive = false;
        sUnderActive = true;
        sUnderStartFrame = play->gameplayFrames;
        for (int32_t i = 0; i < 3; i++) {
            sUnderFrom[i] = sFloatLastAt[i]; // where it was drawn last (on the surface above the bait)
        }
    }
    if (sUnderActive) {
        // On the line from the fish (baitPos = the fish's mouth) toward the rod tip, the set's float depth away (as in
        // hand); 29g-2: no forced depth; 29g-3 (user): it may be in the air. Reached over underTime s from where it
        // was (smoothstep).
        float underTime =
            std::max(0.05f, CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.FloatUnderTime"), kDefaultFloatUnderTime));
        float d[3] = { rodTip[0] - baitPos[0], rodTip[1] - baitPos[1], rodTip[2] - baitPos[2] };
        float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        float along = std::fmin(depthM * RF3_UNITS_PER_METRE, len * kFloatMaxShare);
        float target[3];
        for (int32_t i = 0; i < 3; i++) {
            target[i] = baitPos[i] + (len > 0.001f ? d[i] / len * along : 0.0f);
        }
        float share = std::min(1.0f, static_cast<float>(play->gameplayFrames - sUnderStartFrame) *
                                         kGameFrameSeconds / underTime);
        share = share * share * (3.0f - 2.0f * share);
        for (int32_t i = 0; i < 3; i++) {
            at[i] = sUnderFrom[i] + (target[i] - sUnderFrom[i]) * share;
        }
        where = "under";
        char line[120];
        std::snprintf(line, sizeof(line), "float-bob state=under time_s=%.2f along_m=%.2f", underTime, depthM);
        if (sFloatBobState != line) {
            sFloatBobState = line;
            Log(line);
        }
    } else if (baitPos[1] < waterY) {
        // The bait is in the water: the float rides the surface straight above it.
        at[0] = baitPos[0];
        at[1] = waterY;
        at[2] = baitPos[2];
        where = "surface";
        // Step 29e: the idle bob, a slow sine on the surface.
        float ampM = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.FloatBobM"), kDefaultFloatBobM);
        float periodS = std::max(0.1f, CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.FloatBobPeriod"),
                                                    kDefaultFloatBobPeriod));
        float t = static_cast<float>(play->gameplayFrames) * kGameFrameSeconds;
        float bobUnits = ampM * RF3_UNITS_PER_METRE * std::sin(2.0f * static_cast<float>(M_PI) * t / periodS);
        at[1] += bobUnits;
        char line[120];
        std::snprintf(line, sizeof(line), "float-bob state=idle amp_m=%.3f period_s=%.2f", ampM, periodS);
        // Step 29f: a nibble dip each time the chasing fish enters WAIT (RF3's inspect: it reached the bait and backs
        // off, RF3-MECHANICS 2.6; its WAIT count rises). Down over dipDown s, back over dipUp s (smoothstep).
        RF3ApproachStatus ap;
        RF3Approach_GetStatus(&ap);
        if (ap.chaser != sDipChaser) {
            sDipChaser = ap.chaser;
            sDipWaits = 0;
        }
        if (ap.chaser >= 0 && ap.chaserWaits > sDipWaits) {
            sDipWaits = ap.chaserWaits;
            sDipStartFrame = play->gameplayFrames;
            sDipActive = true;
        }
        if (sDipActive) {
            float dipM = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.FloatDipM"), kDefaultFloatDipM);
            float down = std::max(0.05f, CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.FloatDipDown"),
                                                      kDefaultFloatDipDown));
            float up = std::max(0.05f, CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.FloatDipUp"), kDefaultFloatDipUp));
            float dt = static_cast<float>(play->gameplayFrames - sDipStartFrame) * kGameFrameSeconds;
            float share; // 0 = on the surface, 1 = fully dipped
            if (dt < down) {
                share = dt / down;
            } else if (dt < down + up) {
                share = 1.0f - (dt - down) / up;
            } else {
                share = 0.0f;
                sDipActive = false;
            }
            share = share * share * (3.0f - 2.0f * share);
            at[1] -= share * dipM * RF3_UNITS_PER_METRE;
            if (sDipActive) {
                std::snprintf(line, sizeof(line), "float-bob state=nibble waits=%d chaser=%d dip_m=%.3f down_s=%.2f "
                              "up_s=%.2f", sDipWaits, sDipChaser, dipM, down, up);
            }
        }
        if (sFloatBobState != line) {
            sFloatBobState = line;
            Log(line);
        }
    } else {
        // In hand or in flight: on the line toward the rod tip, the float depth away (at most 90 % of the way).
        float d[3] = { rodTip[0] - baitPos[0], rodTip[1] - baitPos[1], rodTip[2] - baitPos[2] };
        float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        float along = std::fmin(depthM * RF3_UNITS_PER_METRE, len * kFloatMaxShare);
        for (int i = 0; i < 3; i++) {
            at[i] = baitPos[i] + (len > 0.001f ? d[i] / len * along : 0.0f);
        }
        where = "line";
        if (sFloatBobState != "float-bob state=none") { // step 29e: no bob off the water
            sFloatBobState = "float-bob state=none";
            Log(sFloatBobState);
        }
    }
    if (sReturnActive) {
        // Step 31b: from where the fight left it to this frame's spot (which keeps its bob), over returnTime s
        // (smoothstep).
        float returnTime = std::max(0.05f, CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.FloatReturnTime"),
                                                        kDefaultFloatReturnTime));
        uint32_t frames = play->gameplayFrames - sReturnStartFrame;
        float share = std::min(1.0f, static_cast<float>(frames) * kGameFrameSeconds / returnTime);
        if (frames == 0) {
            char line[160];
            std::snprintf(line, sizeof(line),
                          "float-return start time_s=%.2f cast=%d from=(%.1f %.1f %.1f) to=(%.1f %.1f %.1f) where=%s",
                          returnTime, castState, sReturnFrom[0], sReturnFrom[1], sReturnFrom[2], at[0], at[1], at[2],
                          where);
            Log(line);
        }
        if (share >= 1.0f) {
            sReturnActive = false;
            char line[100];
            std::snprintf(line, sizeof(line), "float-return done frames=%u where=%s", frames, where);
            Log(line);
        } else {
            share = share * share * (3.0f - 2.0f * share);
            for (int32_t i = 0; i < 3; i++) {
                at[i] = sReturnFrom[i] + (at[i] - sReturnFrom[i]) * share;
            }
            where = "return";
        }
    }
    float unitsPerMetre = CVarGetFloat(kKindInfo[kFloat].cvar, kKindInfo[kFloat].defaultScale);
    char state[200];
    std::snprintf(state, sizeof(state),
                  "float-draw index=%d depth_setting=%d depth_m=%.1f where=%s units_per_m=%.0f preview=%d "
                  "length_units=%.1f",
                  index, c.depth, depthM, where, unitsPerMetre, preview ? 1 : 0, LongestUnits(m, unitsPerMetre));
    NoteState(sLastFloatState, sFloatStatus, state);

    for (int32_t i = 0; i < 3; i++) {
        sFloatLastAt[i] = at[i]; // step 29g: where the float goes under from
    }
    sFloatDrawnFrame = play->gameplayFrames; // step 29g-2: the line runs through it this frame
    sFloatDrawn = true;
    Matrix_Push();
    Matrix_Translate(at[0], at[1], at[2], MTXMODE_NEW);
    Matrix_RotateY(rotY, MTXMODE_APPLY); // upright; the model stands above its origin (the line tie at its foot)
    DrawModel(play, m, unitsPerMetre, false);
    Matrix_Pop();
}

// Step 40b (DESIGN "Step 40 design"): OoT's own pole lures (the pole box's lures 91 normal, 92 sinking), which have no
// RF3 model, drawn still with the pond's display lists for the item get and the item menu preview: centred at the
// current matrix, side-on (their length along the frame's x), the longest side `size` frame units. billboard: the
// rotation the sinking lure's segments face (the camera's). Sizes from the models' vertex bounds [test].
constexpr int32_t kOoTLureNormal = 91;
constexpr int32_t kOoTLureSinking = 92;
constexpr float kOoTLureScale = kOotLureScale;              // the pond's 0.004 (body and hooks; hooks 0.005 deep)
constexpr float kOoTLureLength = 12.5f;                     // body 3128 units x 0.004
constexpr float kOoTLureCentre[3] = { 0.0f, -1.4f, -2.78f }; // body + hanging hooks, pond units, before the side turn
constexpr float kOoTLureHooks[2][3] = { { 500.0f, 0.0f, -300.0f }, { 2100.0f, 0.0f, -50.0f } }; // body points (pond)
constexpr int32_t kOoTSinkingSegs = 20;
constexpr float kOoTSinkingSizes[kOoTSinkingSegs] = { 1.0f, 1.5f,  1.8f, 2.0f, 1.8f, 1.6f, 1.4f, 1.2f, 1.0f, 1.0f,
                                                      0.9f, 0.85f, 0.8f, 0.7f, 0.8f, 1.0f, 1.2f, 1.1f, 1.0f, 0.8f };
constexpr float kOoTSinkingStep = 0.85f;  // pond units between segments (Fishing_UpdateSinkingLure)
constexpr float kOoTSinkingLength = 18.2f; // 19 steps + the end segments (31 units x 0.04 x their size)

static bool DrawOoTLure(PlayState* play, int32_t index, float size, const MtxF* billboard) {
    if (index != kOoTLureNormal && index != kOoTLureSinking) {
        return false;
    }
    OPEN_DISPS(play->state.gfxCtx);
    Gfx_SetupDL_25Opa(play->state.gfxCtx);
    Matrix_Push();
    if (index == kOoTLureNormal) {
        const float k = size / kOoTLureLength;
        Matrix_Scale(k, k, k, MTXMODE_APPLY);
        Matrix_RotateY(static_cast<f32>(M_PI) / 2.0f, MTXMODE_APPLY); // the body's length (pond z) along the frame's x
        Matrix_Translate(-kOoTLureCentre[0], -kOoTLureCentre[1], -kOoTLureCentre[2], MTXMODE_APPLY);
        // the body, as Fishing_DrawLureAndLine places it (its own z offset left out)
        Matrix_Push();
        Matrix_Scale(kOoTLureScale, kOoTLureScale, kOoTLureScale, MTXMODE_APPLY);
        Matrix_RotateZ(static_cast<f32>(M_PI) / 2.0f, MTXMODE_APPLY);
        Matrix_RotateY(static_cast<f32>(M_PI) / 2.0f, MTXMODE_APPLY);
        gSPMatrix(POLY_OPA_DISP++, Matrix_NewMtx(play->state.gfxCtx, (char*)__FILE__, __LINE__),
                  G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
        gSPDisplayList(POLY_OPA_DISP++, (Gfx*)gFishingLureFloatDL);
        Matrix_Pop();
        // the hooks, as Fishing_DrawLureHook draws them (two crossed copies), hanging straight down [assumed]. A body
        // point (x, y, z) sits at 0.004 (-y, z, -x) after the body's turns.
        for (const auto& h : kOoTLureHooks) {
            Matrix_Push();
            Matrix_Translate(-h[1] * kOoTLureScale, h[2] * kOoTLureScale, -h[0] * kOoTLureScale, MTXMODE_APPLY);
            Matrix_RotateX(static_cast<f32>(M_PI) / 2.0f, MTXMODE_APPLY); // the hook's length (-z) downward
            Matrix_Scale(kOoTLureScale, kOoTLureScale, 0.005f, MTXMODE_APPLY);
            Matrix_RotateY(static_cast<f32>(M_PI), MTXMODE_APPLY);
            gSPMatrix(POLY_OPA_DISP++, Matrix_NewMtx(play->state.gfxCtx, (char*)__FILE__, __LINE__),
                      G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
            gSPDisplayList(POLY_OPA_DISP++, (Gfx*)gFishingLureHookDL);
            Matrix_RotateZ(static_cast<f32>(M_PI) / 2.0f, MTXMODE_APPLY);
            gSPMatrix(POLY_OPA_DISP++, Matrix_NewMtx(play->state.gfxCtx, (char*)__FILE__, __LINE__),
                      G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
            gSPDisplayList(POLY_OPA_DISP++, (Gfx*)gFishingLureHookDL);
            Matrix_Pop();
        }
    } else {
        // the sinking lure: OoT's segments in a straight line along the frame's x, each facing the camera
        const float k = size / kOoTSinkingLength;
        Matrix_Scale(k, k, k, MTXMODE_APPLY);
        gSPDisplayList(POLY_OPA_DISP++, (Gfx*)gFishingSinkingLureSegmentMaterialDL);
        for (int32_t i = kOoTSinkingSegs - 1; i >= 0; i--) {
            Matrix_Push();
            Matrix_Translate((i - (kOoTSinkingSegs - 1) * 0.5f) * kOoTSinkingStep, 0.0f, 0.0f, MTXMODE_APPLY);
            const float s = kOoTSinkingSizes[i] * 0.04f;
            Matrix_Scale(s, s, s, MTXMODE_APPLY);
            Matrix_ReplaceRotation(const_cast<MtxF*>(billboard)); // keeps the scale
            gSPMatrix(POLY_OPA_DISP++, Matrix_NewMtx(play->state.gfxCtx, (char*)__FILE__, __LINE__),
                      G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
            gSPDisplayList(POLY_OPA_DISP++, (Gfx*)gFishingSinkingLureSegmentModelDL);
            Matrix_Pop();
        }
    }
    Matrix_Pop();
    CLOSE_DISPS(play->state.gfxCtx);
    char state[120];
    std::snprintf(state, sizeof(state), "oot-lure-model index=%d size=%.0f", index, size);
    NoteState(sLastDrawState, sStatus, state);
    return true;
}

extern "C" int32_t RF3LureModel_DrawGI(PlayState* play, int32_t stockKind, int32_t index, float size) {
    int32_t kind = -1;
    switch (stockKind) {
        case RF3_STOCK_LURE:
            kind = kLure;
            break;
        case RF3_STOCK_FLY:
            kind = kFly;
            break;
        case RF3_STOCK_BAIT:
            kind = kBait; // [assumed] bait type n = baithook n (12 each, 25d)
            break;
        case RF3_STOCK_FLOAT:
            kind = kFloat;
            break;
    }
    if (stockKind == RF3_STOCK_LURE && DrawOoTLure(play, index, size, &play->billboardMtxF)) {
        return 1; // step 40b: OoT's pole lures 91/92, OoT's detailed models
    }
    if (kind < 0 || index < 0 || index >= kKindInfo[kind].count) {
        return 0; // not tackle
    }
    LureModel* m = Get(kind, index);
    if (m == nullptr || !m->ok) {
        return 0;
    }
    float longestM = std::fmax(std::fmax(m->sizeM[0], m->sizeM[1]), m->sizeM[2]);
    if (longestM <= 1e-5f) {
        return 0;
    }
    float unitsPerMetre = size / longestM;
    float s = unitsPerMetre / m->posScale;
    char state[160];
    std::snprintf(state, sizeof(state), "gi-model kind=%s index=%d size=%.0f longest_m=%.3f", kKindInfo[kind].name,
                  index, size, longestM);
    NoteState(sLastDrawState, sStatus, state);
    Matrix_Push();
    // [assumed] side-on to the camera: the model's length (z after the axis fix) along the frame's x
    Matrix_RotateY(static_cast<f32>(M_PI) / 2.0f, MTXMODE_APPLY);
    // centre: the model's centre after DrawModel's scale and half turn about x (y, z negate)
    Matrix_Translate(-s * m->center[0], s * m->center[1], s * m->center[2], MTXMODE_APPLY);
    DrawModel(play, m, unitsPerMetre, false);
    Matrix_Pop();
    return 1;
}

// Step 35c: the tackle screen's item menu preview. As SoH's pause Link (Player_DrawPauseImpl): on the work list the
// framebuffer `fb` is bound, a nested opaque list clears depth and colour (the pane's colour), sets the viewport,
// perspective, look-at and lights and draws the rigid model, turning about the vertical; the caller shows `fb` as
// a texture. Framed as RF3LureModel_DrawGI (longest side fitted, centred). Returns 0 with nothing to draw.
constexpr float kPreviewTurnSeconds = 6.0f; // one turn per 6 s [assumed; user: RF3's preview slowly spins]
constexpr float kPreviewEyeDist = 200.0f;
constexpr float kPreviewFovy = 30.0f;
constexpr float kPreviewFill = 0.75f;  // the longest side's share of the view's height
constexpr int32_t kPreviewMaxW = 128; // the work buffers below (the pane is 108 x 132)
constexpr int32_t kPreviewMaxH = 144;

// Steps 35c / 39a: an offscreen pass, as SoH's pause Link (Player_DrawPauseImpl): on the work list the framebuffer
// `fb` is bound and a nested opaque list clears depth and colour (clearRgb) and sets the viewport and the lit render
// state. The caller then sets its projection and lights and draws; EndOffscreen closes the list and unbinds `fb`.
// Returns the slot EndOffscreen branches from.
static Gfx* BeginOffscreen(PlayState* play, int32_t fb, int32_t width, int32_t height, const uint8_t* clearRgb,
                           u16* colorBuf, u16* depthBuf, Vp* viewport) {
    Gfx* opaRef = nullptr; // outside OPEN_DISPS's block (35c's first build: C2065)
    OPEN_DISPS(play->state.gfxCtx);
    gsSPSetFB(WORK_DISP++, fb);
    opaRef = POLY_OPA_DISP;
    POLY_OPA_DISP++;
    gSPDisplayList(WORK_DISP++, POLY_OPA_DISP);

    gDPPipeSync(POLY_OPA_DISP++);
    gSPLoadGeometryMode(POLY_OPA_DISP++, 0);
    gSPTexture(POLY_OPA_DISP++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_OFF);
    gDPSetCombineMode(POLY_OPA_DISP++, G_CC_SHADE, G_CC_SHADE);
    gDPSetOtherMode(POLY_OPA_DISP++,
                    G_AD_DISABLE | G_CD_MAGICSQ | G_CK_NONE | G_TC_FILT | G_TF_BILERP | G_TT_NONE | G_TL_TILE |
                        G_TD_CLAMP | G_TP_PERSP | G_CYC_FILL | G_PM_NPRIMITIVE,
                    G_AC_NONE | G_ZS_PIXEL | G_RM_NOOP | G_RM_NOOP2);
    gSPLoadGeometryMode(POLY_OPA_DISP++, G_ZBUFFER | G_SHADE | G_CULL_BACK | G_LIGHTING | G_SHADING_SMOOTH);
    gDPSetScissor(POLY_OPA_DISP++, G_SC_NON_INTERLACE, 0, 0, width, height);
    gSPClipRatio(POLY_OPA_DISP++, FRUSTRATIO_1);

    gDPSetColorImage(POLY_OPA_DISP++, G_IM_FMT_RGBA, G_IM_SIZ_16b, width, depthBuf);
    gDPSetCycleType(POLY_OPA_DISP++, G_CYC_FILL);
    gDPSetRenderMode(POLY_OPA_DISP++, G_RM_NOOP, G_RM_NOOP2);
    gDPSetFillColor(POLY_OPA_DISP++, (GPACK_ZDZ(G_MAXFBZ, 0) << 16) | GPACK_ZDZ(G_MAXFBZ, 0));
    gDPFillRectangle(POLY_OPA_DISP++, 0, 0, width - 1, height - 1);
    gDPPipeSync(POLY_OPA_DISP++);

    const u16 clear = GPACK_RGBA5551(clearRgb[0], clearRgb[1], clearRgb[2], 1);
    gDPSetColorImage(POLY_OPA_DISP++, G_IM_FMT_RGBA, G_IM_SIZ_16b, width, colorBuf);
    gDPSetCycleType(POLY_OPA_DISP++, G_CYC_FILL);
    gDPSetRenderMode(POLY_OPA_DISP++, G_RM_NOOP, G_RM_NOOP2);
    gDPSetFillColor(POLY_OPA_DISP++, (clear << 16) | clear);
    gDPFillRectangle(POLY_OPA_DISP++, 0, 0, width - 1, height - 1);
    gDPPipeSync(POLY_OPA_DISP++);
    gDPSetDepthImage(POLY_OPA_DISP++, depthBuf);

    viewport->vp.vscale[0] = viewport->vp.vtrans[0] = width * ((1 << 2) / 2);
    viewport->vp.vscale[1] = viewport->vp.vtrans[1] = height * ((1 << 2) / 2);
    viewport->vp.vscale[2] = viewport->vp.vtrans[2] = G_MAXZ / 2;
    gSPViewport(POLY_OPA_DISP++, viewport);
    CLOSE_DISPS(play->state.gfxCtx);
    return opaRef;
}

static void EndOffscreen(PlayState* play, Gfx* opaRef) {
    OPEN_DISPS(play->state.gfxCtx);
    gSPEndDisplayList(POLY_OPA_DISP++);
    gSPBranchList(opaRef, POLY_OPA_DISP);
    gsSPResetFB(WORK_DISP++);
    CLOSE_DISPS(play->state.gfxCtx);
}

static Lights1 sPreviewLights = gdSPDefLights1(80, 80, 80, 255, 255, 255, 84, 84, -84); // pause Link's

extern "C" int32_t RF3LureModel_DrawMenuPreview(PlayState* play, int32_t stockKind, int32_t index, int32_t fb,
                                                int32_t width, int32_t height, uint32_t frame, const uint8_t* clearRgb) {
    int32_t kind = -1;
    switch (stockKind) {
        case RF3_STOCK_LURE: kind = kLure; break;
        case RF3_STOCK_FLY: kind = kFly; break;
        case RF3_STOCK_BAIT: kind = kBaitMenu; break; // 35c-3: RF3's bait-only model, as RF3's bait menu (user)
        case RF3_STOCK_FLOAT: kind = kFloat; break;
    }
    // step 40b: OoT's pole lures 91/92 (no RF3 model) are drawn with OoT's own models
    const bool oot = stockKind == RF3_STOCK_LURE && (index == kOoTLureNormal || index == kOoTLureSinking);
    if (fb < 0 || width <= 0 || height <= 0 || width > kPreviewMaxW || height > kPreviewMaxH || kind < 0 ||
        index < 0 || (!oot && index >= kKindInfo[kind].count)) {
        return 0;
    }
    LureModel* m = oot ? nullptr : Get(kind, index);
    if (!oot && (m == nullptr || !m->ok)) {
        return 0;
    }
    const float longestM = oot ? 1.0f : std::fmax(std::fmax(m->sizeM[0], m->sizeM[1]), m->sizeM[2]);
    if (longestM <= 1e-5f) {
        return 0;
    }
    // The longest side fills kPreviewFill of the view's height at the eye's distance.
    const float viewH = 2.0f * kPreviewEyeDist * std::tan(kPreviewFovy * 0.5f * static_cast<float>(M_PI) / 180.0f);
    const float unitsPerMetre = kPreviewFill * viewH / longestM;
    const float s = oot ? 0.0f : unitsPerMetre / m->posScale;
    const float turn = 2.0f * static_cast<float>(M_PI) * std::fmod(frame * kGameFrameSeconds, kPreviewTurnSeconds) /
                       kPreviewTurnSeconds;

    static u16 sColorBuf[kPreviewMaxW * kPreviewMaxH];
    static u16 sDepthBuf[kPreviewMaxW * kPreviewMaxH];
    static Vp viewport;
    Gfx* opaRef = BeginOffscreen(play, fb, width, height, clearRgb, sColorBuf, sDepthBuf, &viewport);
    Mtx* perspMtx = static_cast<Mtx*>(Graph_Alloc(play->state.gfxCtx, sizeof(Mtx)));
    Mtx* lookAtMtx = static_cast<Mtx*>(Graph_Alloc(play->state.gfxCtx, sizeof(Mtx)));
    u16 perspNorm;
    OPEN_DISPS(play->state.gfxCtx);
    guPerspective(perspMtx, &perspNorm, kPreviewFovy, static_cast<f32>(width) / static_cast<f32>(height), 10.0f,
                  4000.0f, 1.0f);
    gSPPerspNormalize(POLY_OPA_DISP++, perspNorm);
    gSPMatrix(POLY_OPA_DISP++, perspMtx, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
    guLookAt(lookAtMtx, 0.0f, 0.0f, kPreviewEyeDist, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);
    gSPMatrix(POLY_OPA_DISP++, lookAtMtx, G_MTX_NOPUSH | G_MTX_MUL | G_MTX_PROJECTION);
    gSPSetLights1(POLY_OPA_DISP++, sPreviewLights);

    Matrix_Push();
    Matrix_Translate(0.0f, 0.0f, 0.0f, MTXMODE_NEW);
    Matrix_RotateY(turn, MTXMODE_APPLY);
    if (oot) {
        // the preview's camera looks straight down -z: the segments face it with no rotation
        static MtxF sFacing = { { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } } };
        DrawOoTLure(play, index, kPreviewFill * viewH, &sFacing);
    } else {
        Matrix_Translate(-s * m->center[0], s * m->center[1], s * m->center[2], MTXMODE_APPLY);
        DrawModel(play, m, unitsPerMetre, false);
    }
    Matrix_Pop();

    CLOSE_DISPS(play->state.gfxCtx);
    EndOffscreen(play, opaRef);

    char state[160];
    std::snprintf(state, sizeof(state), "menu-preview kind=%s index=%d units_per_m=%.0f longest_m=%.3f fb=%d %dx%d",
                  oot ? "oot-lure" : kKindInfo[kind].name, index, unitsPerMetre, longestM, fb, width, height);
    NoteState(sLastDrawState, sStatus, state);
    return 1;
}

// Step 39a-39c (DESIGN "Step 39 design"): the tackle screen's rod pane as RF3 draws it. 39c: everything in RF3's own
// frame (x right, y down, z forward, metres) from RF3's pane record per tackle set, seen by RF3's camera: at the
// origin looking down +z, projection distance 450 px [code: 0x0012BFD0], pixel aspect and image centre fitted to the
// user's screenshots (10 points within 4.5 px RMS) [measured].
// 39b-2/39c: RF3's pane record per tackle set [code: 0x00255F70 + box x 0x160, set by 0x001B0A80]: the rod node
// (+0x189C) position, rotation (x, y, z; applied z, y, x) and scale, posed with motion 30; the item node (+0x18A8)
// position (z 4.16), its scale x the extra node's (+0x18AC) scale, that node's turn about y; the float node (+0x18A4)
// scale and turn about z; line points 6-10 (the float sits on point 10 - depth).
struct PaneBox {
    float rodPos[3];
    float rodRot[3];
    float rodScale[3];
    float item[2];
    float itemScale;
    float itemTurn;      // radians about y (1.57: lures side-on)
    float floatScale[3];
    float floatTurn;     // radians about z
    float line[5][2];    // points 6..10
};
constexpr float kPaneItemZ = 4.16f; // every item, float and line point
constexpr PaneBox kPaneBoxes[10] = {
    { { 1.65f, 0.55f, 2.65f }, { 1.12f, -1.01f, 0.00f }, { 2.5f, 0.8f, 0.8f }, { 1.01f, 1.20f }, 10.00f, 1.57f, { 10.00f, 10.00f, 10.00f }, 0.00f, { { 1.00f, 1.20f }, { 1.00f, 1.20f }, { 1.00f, 1.20f }, { 1.00f, 1.20f }, { 1.00f, 1.20f } } }, // box 0
    { { 1.65f, 0.55f, 2.65f }, { 1.12f, -1.01f, 0.00f }, { 1.5f, 0.9f, 0.9f }, { 1.03f, 1.20f }, 15.00f, 1.57f, { 10.00f, 10.00f, 10.00f }, 0.00f, { { 1.03f, 1.20f }, { 1.03f, 1.20f }, { 1.03f, 1.20f }, { 1.03f, 1.20f }, { 1.03f, 1.20f } } }, // box 1
    { { 1.70f, 0.65f, 2.65f }, { 1.12f, -1.01f, 0.00f }, { 2.0f, 1.0f, 1.0f }, { 1.00f, 1.20f }, 10.00f, 1.57f, { 10.00f, 10.00f, 10.00f }, 0.00f, { { 1.03f, 1.20f }, { 1.03f, 1.20f }, { 1.03f, 1.20f }, { 1.03f, 1.20f }, { 1.03f, 1.20f } } }, // box 2
    { { 1.60f, 0.65f, 2.65f }, { 1.12f, -1.01f, 0.00f }, { 0.9f, 1.2f, 0.9f }, { 1.00f, 1.20f }, 60.00f, 1.57f, { 10.00f, 10.00f, 10.00f }, 0.00f, { { 1.00f, 1.20f }, { 1.00f, 1.20f }, { 1.00f, 1.20f }, { 1.00f, 1.20f }, { 1.00f, 1.20f } } }, // box 3
    { { 1.70f, 0.70f, 2.65f }, { 1.12f, -1.01f, 0.00f }, { 1.0f, 1.0f, 1.0f }, { 1.00f, 1.20f }, 60.00f, 1.57f, { 10.00f, 10.00f, 10.00f }, 0.00f, { { 1.00f, 1.20f }, { 1.00f, 1.20f }, { 1.00f, 1.20f }, { 1.00f, 1.20f }, { 1.00f, 1.20f } } }, // box 4
    { { 1.68f, 1.00f, 2.65f }, { 1.57f, 0.00f, -0.31f }, { 3.0f, 1.0f, 1.0f }, { 1.20f, 0.90f }, 30.00f, 0.00f, { 9.00f, 9.00f, 9.00f }, -0.21f, { { 1.40f, 0.00f }, { 1.36f, 0.16f }, { 1.33f, 0.32f }, { 1.29f, 0.48f }, { 1.25f, 0.64f } } }, // box 5
    { { 1.70f, 1.00f, 2.65f }, { 1.57f, 0.00f, -0.31f }, { 4.0f, 1.0f, 1.0f }, { 1.20f, 0.90f }, 30.00f, 0.00f, { 9.00f, 9.00f, 9.00f }, -0.21f, { { 1.38f, 0.00f }, { 1.35f, 0.16f }, { 1.32f, 0.32f }, { 1.28f, 0.48f }, { 1.25f, 0.64f } } }, // box 6
    { { 1.60f, 0.95f, 2.65f }, { 1.57f, 0.00f, -0.31f }, { 2.5f, 1.0f, 1.0f }, { 1.12f, 1.25f }, 20.00f, 1.57f, { 5.00f, 3.00f, 3.00f }, -0.21f, { { 1.38f, 0.05f }, { 1.35f, 0.21f }, { 1.32f, 0.37f }, { 1.28f, 0.53f }, { 1.24f, 0.69f } } }, // box 7
    { { 1.70f, 1.00f, 2.65f }, { 1.57f, 0.00f, -0.31f }, { 2.5f, 1.0f, 1.0f }, { 1.12f, 1.25f }, 12.00f, 0.00f, { 3.00f, 2.00f, 2.00f }, -0.21f, { { 1.38f, 0.05f }, { 1.35f, 0.21f }, { 1.32f, 0.37f }, { 1.28f, 0.53f }, { 1.24f, 0.69f } } }, // box 8
    { { 1.59f, 0.70f, 2.65f }, { 1.57f, 0.00f, -0.47f }, { 2.0f, 0.9f, 0.9f }, { 1.20f, 0.90f }, 30.00f, 0.00f, { 9.00f, 9.00f, 9.00f }, -0.21f, { { 1.38f, 0.00f }, { 1.35f, 0.16f }, { 1.32f, 0.32f }, { 1.28f, 0.48f }, { 1.25f, 0.64f } } }, // box 9
};
// RF3's camera: at the origin, looking down +z; RF3 pixel = centre + f (aspect x / z, y / z).
constexpr float kPaneCamF = 450.0f;                        // [code] 0x0012BFD0 (+0x324)
constexpr float kPaneCamAspect = 1.048f;                   // [measured, fitted]
constexpr float kPaneCamCentre[2] = { 313.1f, 244.7f };    // [measured, fitted; RF3's default is (320, 240)]
constexpr float kPaneNear = 0.5f;                          // the pane's depth range (the scene is at z 2.5-4.3)
constexpr float kPaneFar = 20.0f;
// 39c-2: the pane is drawn in millimetres. In metres a rod bone's matrix carries 1/10000 (pos units -> m), entries
// ~1e-4, and the fixed-point Mtx (1/65536 steps) rounded them by up to ~10 % per bone: the rod came out crooked
// and thick (user, 2026-10-16: "Pose 30 doesn't seem to be properly matched to the model"); a Python render of the
// same pose was smooth. x1000 gives entries ~0.1. The projection divides by z, so only near/far scale.
constexpr float kPaneUnitsPerM = 1000.0f;
constexpr uint8_t kPaneLineRgb[3] = { 190, 170, 150 }; // [measured, approximate]
constexpr float kPaneLinePx = 1.3f;                // RF3 pixels [measured]
// The rod's turn about its axis was a 39a slider (150); RF3's rod node has its own rotation now (39c), so it is gone.
// 39a-2..39c: the item's tilt (top toward the viewer) and turn (about the vertical): the user's 15 / 15 for the flat
// view were the perspective RF3's camera gives (the lure seen 16 deg from above, 14 from the side), so 0 now; the
// sliders "Rod pane item tilt" / "Rod pane item turn" stay for fine-tuning.
constexpr float kDefaultRodPaneItemTilt = 0.0f;
constexpr float kDefaultRodPaneItemTurn = 0.0f;
// 39a-3 (user: "lit up a little"): the pane's own light, from the upper left and in front. 35c's light comes from
// behind, so the side facing the viewer got only the ambient. Ambient 70 + diffuse 200 (39a-4: the user's slider
// value); slider "Rod pane light". 39c: the direction in RF3's frame (up = -y, toward the viewer = -z).
constexpr uint8_t kRodPaneAmbient = 70;
constexpr float kDefaultRodPaneLight = 200.0f;
constexpr int8_t kRodPaneLightDir[3] = { -30, -60, -100 }; // toward the light: left, up, toward the viewer
constexpr int32_t kPaneMaxW = 128;                 // the work buffers (the pane is 108 x 166)
constexpr int32_t kPaneMaxH = 176;

// The line from a to b (RF3 frame, metres): a strip facing the camera, ~kPaneLinePx wide where it is, opaque in one
// colour (untextured: OPA + primitive, 33b-3); its vertices in the pane's mm.
static void DrawPaneLine(PlayState* play, const float* a, const float* b) {
    const float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    const float m[3] = { (a[0] + b[0]) * 0.5f, (a[1] + b[1]) * 0.5f, (a[2] + b[2]) * 0.5f }; // the camera's ray
    float n[3] = { d[1] * m[2] - d[2] * m[1], d[2] * m[0] - d[0] * m[2], d[0] * m[1] - d[1] * m[0] };
    const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (nl <= 1e-9f) {
        return;
    }
    const float half = kPaneLinePx * 0.5f * m[2] / kPaneCamF; // metres per pixel at that depth
    for (float& c : n) {
        c *= half / nl;
    }
    const float corners[4][3] = { { a[0] + n[0], a[1] + n[1], a[2] + n[2] }, { a[0] - n[0], a[1] - n[1], a[2] - n[2] },
                                  { b[0] - n[0], b[1] - n[1], b[2] - n[2] }, { b[0] + n[0], b[1] + n[1], b[2] + n[2] } };
    Vtx* v = static_cast<Vtx*>(Graph_Alloc(play->state.gfxCtx, 4 * sizeof(Vtx)));
    for (int32_t i = 0; i < 4; i++) {
        v[i] = {};
        for (int32_t k = 0; k < 3; k++) {
            v[i].v.ob[k] = static_cast<s16>(std::lround(corners[i][k] * kPaneUnitsPerM)); // mm, at most 32 m
        }
        v[i].v.cn[3] = 255;
    }
    OPEN_DISPS(play->state.gfxCtx);
    Matrix_Push();
    Matrix_Translate(0.0f, 0.0f, 0.0f, MTXMODE_NEW); // the vertices are already in the pane's mm
    gDPPipeSync(POLY_OPA_DISP++);
    gDPSetCycleType(POLY_OPA_DISP++, G_CYC_1CYCLE);
    gSPTexture(POLY_OPA_DISP++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_OFF);
    gSPClearGeometryMode(POLY_OPA_DISP++, G_LIGHTING | G_CULL_BACK | G_CULL_FRONT);
    gDPSetRenderMode(POLY_OPA_DISP++, G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2);
    gDPSetPrimColor(POLY_OPA_DISP++, 0, 0, kPaneLineRgb[0], kPaneLineRgb[1], kPaneLineRgb[2], 255);
    gDPSetCombineMode(POLY_OPA_DISP++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);
    gSPMatrix(POLY_OPA_DISP++, Matrix_NewMtx(play->state.gfxCtx, (char*)__FILE__, __LINE__),
              G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    gSPVertex(POLY_OPA_DISP++, reinterpret_cast<uintptr_t>(v), 4, 0);
    gSP2Triangles(POLY_OPA_DISP++, 0, 1, 2, 0, 0, 2, 3, 0);
    // the models after this set their own state but not the geometry mode: lit again
    gSPLoadGeometryMode(POLY_OPA_DISP++, G_ZBUFFER | G_SHADE | G_CULL_BACK | G_LIGHTING | G_SHADING_SMOOTH);
    Matrix_Pop();
    CLOSE_DISPS(play->state.gfxCtx);
}

// The loaded model of kind/index, or nullptr (no model, or out of range).
static LureModel* PaneModel(int32_t kind, int32_t index) {
    if (kind < 0 || index < 0 || index >= kKindInfo[kind].count) {
        return nullptr;
    }
    LureModel* m = Get(kind, index);
    return (m != nullptr && m->ok) ? m : nullptr;
}

// A rigid model drawn in RF3's frame at the current matrix (metres): DrawModel's half turn about x (RF3 y-down ->
// y-up) undone first.
static void DrawModelRf3(PlayState* play, const LureModel* m) {
    Matrix_RotateX(static_cast<f32>(M_PI), MTXMODE_APPLY);
    DrawModel(play, m, 1.0f, false);
}

// RF3 pixel of an RF3-frame point (for the log).
static void PanePixel(const float* w, float* out) {
    out[0] = kPaneCamCentre[0] + kPaneCamF * kPaneCamAspect * w[0] / w[2];
    out[1] = kPaneCamCentre[1] + kPaneCamF * w[1] / w[2];
}

// Step 41 (DESIGN "Step 41 design"): OoT's Fishing Pole (box 10) has no RF3 pane record or rod model; the mod lays
// OoT's own rod where box 0's RF3 rod lies (its motion-30 butt and tip, computed from the 39c model [test]) and hangs
// the OoT lure at box 0's tie, nose left, at RF3's lure scale there [assumed].
constexpr int32_t kPaneOoTBox = 10;
constexpr float kPaneOoTButt[3] = { 1.6144f, 0.6704f, 2.6535f };
constexpr float kPaneOoTTip[3] = { 1.1357f, -0.4275f, 2.9729f };
constexpr float kPaneOoTBow[3] = { 0.922f, -0.383f, 0.065f }; // box 0's bow (sagitta 1.5 cm), unit, across the chord
constexpr float kPaneOoTTurn = 0.095f;                         // the bend's total turn for that sagitta (8 s / L)
constexpr int32_t kOoTRodSegs = 22;                            // Fishing_DrawRod's tables
constexpr float kOoTRodScales[kOoTRodSegs] = { 1.0f,   1.0f,   1.0f,   0.9625f, 0.925f, 0.8875f, 0.85f,  0.8125f,
                                               0.775f, 0.7375f, 0.7f,  0.6625f, 0.625f, 0.5875f, 0.55f,  0.5125f,
                                               0.475f, 0.4375f, 0.4f,  0.3625f, 0.325f, 0.2875f };
constexpr float kOoTRodRatios[kOoTRodSegs] = { 0.0f,  0.0f,  0.0f,  0.0f,  0.0f,    0.06f,   0.12f,   0.18f,
                                               0.24f, 0.30f, 0.36f, 0.42f, 0.48f,   0.54f,   0.60f,   0.60f,
                                               0.5142f, 0.4285f, 0.3428f, 0.2571f, 0.1714f, 0.0857f };
constexpr float kOoTRodLength = 11020.0f; // 21 steps of 500 + one segment of 1000 x 0.52
constexpr float kPaneOoTLureM = 0.46f;    // OoT's lure ~4.6 cm (12.5 pond units at 270 units/m) x box 0's item scale 10
constexpr float kPaneOoTSinkingM = 0.67f; // the sinking lure's 18.2 pond units, the same way

// OoT's rod from the pane's butt toward its tip, bowed as box 0's RF3 rod; the current matrix is the pane's metres.
// Writes the rod tip (current-matrix units) to tipOut.
static void DrawOoTRodPane(PlayState* play, float* tipOut) {
    float f[3] = { kPaneOoTTip[0] - kPaneOoTButt[0], kPaneOoTTip[1] - kPaneOoTButt[1], kPaneOoTTip[2] - kPaneOoTButt[2] };
    const float chord = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (float& c : f) {
        c /= chord;
    }
    // the start tilted half the turn toward the bow; the segments then turn back toward -bow (see RotateX below)
    const float h = kPaneOoTTurn * 0.5f;
    float z[3], y[3], x[3];
    for (int i = 0; i < 3; i++) {
        z[i] = f[i] * std::cos(h) + kPaneOoTBow[i] * std::sin(h);
    }
    const float zd = z[0] * kPaneOoTBow[0] + z[1] * kPaneOoTBow[1] + z[2] * kPaneOoTBow[2];
    for (int i = 0; i < 3; i++) {
        y[i] = kPaneOoTBow[i] - zd * z[i]; // the bow, square to the start
    }
    const float yl = std::sqrt(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
    for (float& c : y) {
        c /= yl;
    }
    x[0] = y[1] * z[2] - y[2] * z[1];
    x[1] = y[2] * z[0] - y[0] * z[2];
    x[2] = y[0] * z[1] - y[1] * z[0];
    MtxF basis = {}; // column vectors x, y, z (OoT's MtxF: mf[column][row])
    for (int i = 0; i < 3; i++) {
        basis.mf[0][i] = x[i];
        basis.mf[1][i] = y[i];
        basis.mf[2][i] = z[i];
    }
    basis.mf[3][3] = 1.0f;
    float ratioSum = 0.0f;
    for (float r : kOoTRodRatios) {
        ratioSum += r;
    }
    const float u = chord / kOoTRodLength; // metres per OoT rod unit (the bend shortens the chord by < 0.1 %)
    OPEN_DISPS(play->state.gfxCtx);
    Gfx_SetupDL_25Opa(play->state.gfxCtx);
    gSPDisplayList(POLY_OPA_DISP++, (Gfx*)gFishingRodMaterialDL);
    gDPSetPrimColor(POLY_OPA_DISP++, 0, 0, 255, 155, 0, 255);
    Matrix_Push();
    Matrix_Translate(kPaneOoTButt[0], kPaneOoTButt[1], kPaneOoTButt[2], MTXMODE_APPLY);
    Matrix_Mult(&basis, MTXMODE_APPLY);
    Matrix_Scale(u, u, u, MTXMODE_APPLY);
    for (int32_t i = 0; i < kOoTRodSegs; i++) {
        // RotateX(+a) turns +z toward -y: toward -bow, so the rod bows toward +bow
        Matrix_RotateX(kOoTRodRatios[i] * kPaneOoTTurn / ratioSum, MTXMODE_APPLY);
        Matrix_Push();
        Matrix_Scale(kOoTRodScales[i], kOoTRodScales[i], 0.52f, MTXMODE_APPLY);
        gSPMatrix(POLY_OPA_DISP++, Matrix_NewMtx(play->state.gfxCtx, (char*)__FILE__, __LINE__),
                  G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
        const char* tex = (i < 5) ? gFishingRodSegmentBlackTex
                                  : ((i < 8) || ((i % 2) == 0)) ? gFishingRodSegmentWhiteTex : gFishingRodSegmentStripTex;
        gDPLoadTextureBlock(POLY_OPA_DISP++, tex, G_IM_FMT_RGBA, G_IM_SIZ_16b, 16, 8, 0, G_TX_NOMIRROR | G_TX_WRAP,
                            G_TX_NOMIRROR | G_TX_WRAP, 4, 3, G_TX_NOLOD, G_TX_NOLOD);
        gSPDisplayList(POLY_OPA_DISP++, (Gfx*)gFishingRodSegmentDL);
        Matrix_Pop();
        Matrix_Translate(0.0f, 0.0f, 500.0f, MTXMODE_APPLY);
        if (i == kOoTRodSegs - 1) {
            Vec3f zero = { 0.0f, 0.0f, 0.0f };
            Vec3f out;
            Matrix_MultVec3f(&zero, &out);
            tipOut[0] = out.x;
            tipOut[1] = out.y;
            tipOut[2] = out.z;
        }
    }
    Matrix_Pop();
    CLOSE_DISPS(play->state.gfxCtx);
}

// OoT's lure (91/92) with its line tie at `tie` (the pane's metres; the current matrix is the pane's metres), nose
// left: step 40's model turned from OoT's y-up into RF3's y-down frame.
static void DrawOoTLurePane(PlayState* play, int32_t index, const float* tie) {
    // 41-2: RF3's camera looks down +z, so the segments (one-sided squares facing +z) must face -z: a half turn about x
    // (also y-down for this frame). Identity faced them away and they were culled (user: "the sinking lure doesn't
    // render in the main preview screen").
    static MtxF sFacing = { { { 1, 0, 0, 0 }, { 0, -1, 0, 0 }, { 0, 0, -1, 0 }, { 0, 0, 0, 1 } } };
    const bool sinking = index == kOoTLureSinking;
    const float size = sinking ? kPaneOoTSinkingM : kPaneOoTLureM;
    float tieLocal[3];
    if (sinking) {
        const float k = size / kOoTSinkingLength; // the head segment (0), the left end of the line of segments
        tieLocal[0] = -(kOoTSinkingSegs - 1) * 0.5f * kOoTSinkingStep * k;
        tieLocal[1] = 0.0f;
    } else {
        const float k = size / kOoTLureLength; // the body's front tip (point x -850) in DrawOoTLure's frame
        tieLocal[0] = (3.4f - kOoTLureCentre[2]) * k; // front tip at pond z +3.4, centre z -2.78: +6.18 along x
        tieLocal[1] = -kOoTLureCentre[1] * k;          // the body's middle height, 1.4 above the centre
    }
    tieLocal[2] = 0.0f;
    Matrix_Push();
    Matrix_Translate(tie[0], tie[1], tie[2], MTXMODE_APPLY);
    Matrix_RotateX(static_cast<f32>(M_PI), MTXMODE_APPLY); // OoT y-up -> RF3 y-down
    if (!sinking) {
        Matrix_RotateY(static_cast<f32>(M_PI), MTXMODE_APPLY); // the front from +x to -x: nose left
    }
    Matrix_Translate(-tieLocal[0], -tieLocal[1], -tieLocal[2], MTXMODE_APPLY);
    DrawOoTLure(play, index, size, &sFacing);
    Matrix_Pop();
}

extern "C" int32_t RF3LureModel_DrawRodPane(PlayState* play, int32_t fb, int32_t width, int32_t height,
                                            const void* background, const float* paneRf3, int32_t rod,
                                            int32_t box, int32_t stockKind, int32_t index, int32_t floatIdx,
                                            int32_t depth, char* stateOut, int32_t stateSize) {
    if (fb < 0 || width <= 0 || height <= 0 || width > kPaneMaxW || height > kPaneMaxH) {
        std::snprintf(stateOut, stateSize, "rod-pane drawn=0 why=buffer %dx%d", width, height);
        return 0;
    }
    const bool ootPole = box == kPaneOoTBox; // step 41: OoT's Fishing Pole, laid out as box 0
    if (box < 0 || (box >= 10 && !ootPole)) {
        std::snprintf(stateOut, stateSize, "rod-pane drawn=0 why=box %d (no RF3 record)", box);
        return 0;
    }
    const bool baitSet = stockKind == RF3_STOCK_BAIT;
    const PaneBox& rec = kPaneBoxes[ootPole ? 0 : box];
    int32_t kind = -1;
    int32_t itemIndex = index;
    switch (stockKind) {
        case RF3_STOCK_LURE: kind = kLure; break;
        case RF3_STOCK_FLY: kind = kFly; break;
        case RF3_STOCK_BAIT:
            kind = index >= 0 ? kBait : kHook; // 33b: no bait (taken, or "---"): the bare hook
            itemIndex = index >= 0 ? index : RF3Tackle_HookIndex();
            break;
    }
    const bool ootLure = stockKind == RF3_STOCK_LURE && (index == kOoTLureNormal || index == kOoTLureSinking);
    LureModel* item = ootLure ? nullptr : PaneModel(kind, itemIndex);
    LureModel* flt = baitSet ? PaneModel(kFloat, floatIdx) : nullptr;
    const float tiltDeg = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.RodPaneItemTilt"), kDefaultRodPaneItemTilt);
    const float turnDeg = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.RodPaneItemTurn"), kDefaultRodPaneItemTurn);
    const uint8_t light = static_cast<uint8_t>(
        std::clamp(CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.RodPaneLight"), kDefaultRodPaneLight), 0.0f, 255.0f));
    Lights1* lights = static_cast<Lights1*>(Graph_Alloc(play->state.gfxCtx, sizeof(Lights1)));
    *lights = gdSPDefLights1(kRodPaneAmbient, kRodPaneAmbient, kRodPaneAmbient, light, light, light, kRodPaneLightDir[0],
                             kRodPaneLightDir[1], kRodPaneLightDir[2]);

    // RF3's camera as a projection onto the pane's box (pane RF3 pixels x0..x1, y0..y1): clip w = z, row vectors
    const float pw = paneRf3[2] - paneRf3[0];
    const float ph = paneRf3[3] - paneRf3[1];
    MtxF proj = {};
    proj.mf[0][0] = 2.0f * kPaneCamF * kPaneCamAspect / pw;
    proj.mf[2][0] = 2.0f * (kPaneCamCentre[0] - paneRf3[0]) / pw - 1.0f;
    proj.mf[1][1] = -2.0f * kPaneCamF / ph;
    proj.mf[2][1] = 1.0f - 2.0f * (kPaneCamCentre[1] - paneRf3[1]) / ph;
    const float zn = kPaneNear * kPaneUnitsPerM; // 39c-2: the pane's units are mm
    const float zf = kPaneFar * kPaneUnitsPerM;
    proj.mf[2][2] = (zf + zn) / (zf - zn);
    proj.mf[3][2] = -2.0f * zf * zn / (zf - zn);
    proj.mf[2][3] = 1.0f;
    Mtx* projMtx = Matrix_MtxFToNewMtx(&proj, play->state.gfxCtx);

    static const uint8_t kClear[3] = { 0, 0, 0 }; // under the background
    static u16 sColorBuf[kPaneMaxW * kPaneMaxH];
    static u16 sDepthBuf[kPaneMaxW * kPaneMaxH];
    static Vp viewport;
    Gfx* opaRef = BeginOffscreen(play, fb, width, height, kClear, sColorBuf, sDepthBuf, &viewport);
    // kept for the log, so outside OPEN_DISPS's block
    const float tie[3] = { rec.item[0], rec.item[1], kPaneItemZ };
    float tip[3] = { tie[0], tie[1], tie[2] };
    float floatAt[3] = { tie[0], tie[1], tie[2] };
    int32_t rodDrawn = 0;
    int32_t rodPosed = 0;
    OPEN_DISPS(play->state.gfxCtx);
    if (background != nullptr) {
        // 39a-2: the pane's own picture (SoH's framebuffer clear is opaque)
        gSPDisplayList(POLY_OPA_DISP++, const_cast<Gfx*>(static_cast<const Gfx*>(background)));
        gDPPipeSync(POLY_OPA_DISP++);
        gSPLoadGeometryMode(POLY_OPA_DISP++, G_ZBUFFER | G_SHADE | G_CULL_BACK | G_LIGHTING | G_SHADING_SMOOTH);
    }
    gSPMatrix(POLY_OPA_DISP++, projMtx, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
    gSPSetLights1(POLY_OPA_DISP++, (*lights));

    // the rod node, bent by motion 30; where its tip lands starts the line
    Matrix_Push();
    Matrix_Scale(kPaneUnitsPerM, kPaneUnitsPerM, kPaneUnitsPerM, MTXMODE_NEW); // metres -> the pane's mm
    if (ootPole) {
        DrawOoTRodPane(play, tip); // step 41
        rodDrawn = 1;
    } else {
        rodDrawn = RF3RodModel_DrawMenu(play, rod, rec.rodPos, rec.rodRot, rec.rodScale, tip, &rodPosed);
    }
    Matrix_Pop();
    if (rodDrawn) {
        for (float& c : tip) {
            c /= kPaneUnitsPerM; // back to metres
        }
    }

    // the item node: position, the extra node's turn about y, scale; the user's fine-tuning about its origin
    if (item != nullptr) {
        Matrix_Push();
        Matrix_Scale(kPaneUnitsPerM, kPaneUnitsPerM, kPaneUnitsPerM, MTXMODE_NEW);
        Matrix_Translate(tie[0], tie[1], tie[2], MTXMODE_APPLY);
        if (!baitSet) {
            // RF3's frame is y-down: a turn about y and a tilt about x keep 39a-4's senses (+ = counterclockwise seen
            // from above, + = top toward the viewer)
            Matrix_RotateY(-turnDeg * static_cast<f32>(M_PI) / 180.0f, MTXMODE_APPLY);
            Matrix_RotateX(tiltDeg * static_cast<f32>(M_PI) / 180.0f, MTXMODE_APPLY);
        }
        Matrix_RotateY(rec.itemTurn, MTXMODE_APPLY);
        Matrix_Scale(rec.itemScale, rec.itemScale, rec.itemScale, MTXMODE_APPLY);
        DrawModelRf3(play, item);
        Matrix_Pop();
    }

    if (ootLure) {
        Matrix_Push();
        Matrix_Scale(kPaneUnitsPerM, kPaneUnitsPerM, kPaneUnitsPerM, MTXMODE_NEW);
        DrawOoTLurePane(play, index, tie); // step 41
        Matrix_Pop();
    }

    // the float node on line point 10 - depth
    if (flt != nullptr) {
        const float* p = rec.line[4 - std::clamp(depth, 0, 4)]; // line[i] = point 6 + i
        floatAt[0] = p[0];
        floatAt[1] = p[1];
        floatAt[2] = kPaneItemZ;
        Matrix_Push();
        Matrix_Scale(kPaneUnitsPerM, kPaneUnitsPerM, kPaneUnitsPerM, MTXMODE_NEW);
        Matrix_Translate(floatAt[0], floatAt[1], floatAt[2], MTXMODE_APPLY);
        Matrix_RotateZ(rec.floatTurn, MTXMODE_APPLY);
        Matrix_Scale(rec.floatScale[0], rec.floatScale[1], rec.floatScale[2], MTXMODE_APPLY);
        DrawModelRf3(play, flt);
        Matrix_Pop();
    }
    CLOSE_DISPS(play->state.gfxCtx);

    if (rodDrawn) {
        if (flt != nullptr) {
            DrawPaneLine(play, tip, floatAt); // through the float's eye
            DrawPaneLine(play, floatAt, tie);
        } else {
            DrawPaneLine(play, tip, tie);
        }
    }
    EndOffscreen(play, opaRef);

    float tipPx[2] = { 0.0f, 0.0f };
    float tiePx[2];
    float floatPx[2];
    if (tip[2] > 1e-3f) {
        PanePixel(tip, tipPx);
    }
    PanePixel(tie, tiePx);
    PanePixel(floatAt, floatPx);
    std::snprintf(stateOut, stateSize,
                  "rod-pane box=%d rod=%d rod_drawn=%d rod_posed=%d item=%s/%d item_drawn=%d item_scale=%.0f float=%d "
                  "float_drawn=%d depth=%d tip_px=(%.0f,%.0f) tie_px=(%.0f,%.0f) float_px=(%.0f,%.0f) tilt=%.0f "
                  "turn=%.0f light=%d bg=%d oot_rod=%d oot_lure=%d fb=%d %dx%d",
                  box, rod, rodDrawn, rodPosed, kind >= 0 ? kKindInfo[kind].name : "none", itemIndex, item ? 1 : 0,
                  rec.itemScale, baitSet ? floatIdx : -1, flt ? 1 : 0, baitSet ? depth : -1, tipPx[0], tipPx[1],
                  tiePx[0], tiePx[1], floatPx[0], floatPx[1], tiltDeg, turnDeg, light, background != nullptr ? 1 : 0,
                  ootPole ? 1 : 0, ootLure ? 1 : 0, fb, width, height);
    return rodDrawn || item != nullptr || ootLure ? 1 : 0;
}

extern "C" int32_t RF3LureModel_LineTie(float* out) {
    if (!sTieValid) {
        return 0;
    }
    for (int32_t i = 0; i < 3; i++) {
        out[i] = sTie[i];
    }
    return 1;
}

extern "C" int32_t RF3LureModel_FloatLinePoint(float* out, uint32_t frame) {
    if (!sFloatDrawn || sFloatDrawnFrame != frame) {
        return 0;
    }
    for (int32_t i = 0; i < 3; i++) {
        out[i] = sFloatLastAt[i];
    }
    return 1;
}

extern "C" int32_t RF3LureModel_TieAny(float* out) {
    if (!sTieAnyValid) {
        return 0;
    }
    for (int32_t i = 0; i < 3; i++) {
        out[i] = sTieAny[i];
    }
    return 1;
}

extern "C" void RF3LureModel_LogLineEnd(const float* lineEnd, int32_t castState, uint32_t frame) {
    if (!sTieAnyValid) {
        return;
    }
    // once a second (20 game frames), and on every cast state change
    if (castState == sTieLogCast && frame - sTieLogFrame < 20) {
        return;
    }
    sTieLogCast = castState;
    sTieLogFrame = frame;
    float d[3] = { sTieAny[0] - lineEnd[0], sTieAny[1] - lineEnd[1], sTieAny[2] - lineEnd[2] };
    float gap = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    char line[260];
    std::snprintf(line, sizeof(line),
                  "lure-tie cast=%d hanging=%d end=(%.2f,%.2f,%.2f) tie=(%.2f,%.2f,%.2f) gap=%.2f pose_shift=%.2f "
                  "motion=%d",
                  castState, sTieHanging ? 1 : 0, lineEnd[0], lineEnd[1], lineEnd[2], sTieAny[0], sTieAny[1],
                  sTieAny[2], gap, sTiePoseShift, sLureAnim.motion);
    Log(line);
}

extern "C" const char* RF3LureModel_Status(void) {
    return sStatus.c_str();
}

extern "C" const char* RF3LureModel_FloatStatus(void) {
    return sFloatStatus.c_str();
}
