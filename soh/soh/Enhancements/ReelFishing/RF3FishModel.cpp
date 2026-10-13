// Reel Fishing III mod (steps 25e-2, 25g-2, 26e-2): RF3's own fish models for region fish, animated as RF3 does:
// motions are asked for by RF3 request code and played through RF3's .mtp patterns. See RF3FishModel.h and
// docs/DESIGN.md "25e design", "25g design", "26e plan"; RF3-MECHANICS 6.2. Loading and drawing are shared with the
// rods (RF3SkinnedModel.cpp).
//
// rf3models/models.json (tools/rf3_models.py models) has:
//   fish_records: fishdata record -> rf3models/fish/<model>.json (bones, batches, the model's raw animations)
//   patterns: FMOT00.MTP's 19 patterns; pattern_codes: RF3's request code -> pattern table (0x0024DE80)
//   fish_move: per record the species speed and speed ramp (26e-3) and class
// A request code in the table plays its pattern; any other code plays that raw animation. A pattern step starts its
// animation (a negative number continues it if already playing), sets the playback rate (mot_speed) and blend
// (mot_chg_speed), and turns the drawn fish by course/bank: RELATIVE offsets eased over the step, snapped at its end,
// cleared at the pattern's end and on every new request (as RF3; they turn the drawn fish, not its path).
// Choosing the request [RF3 codes per state, 26d; free swimming still 25g's speed rule, 26c later]: hooked fish by
// RF3 fight mode, free fish by speed and turn rate. Bone 0 (the root) keeps its bind pose: some animations move it,
// and OoT moves the actor itself.
//
// Step 26g-2, the part controllers (RF3-MECHANICS 6.5): the jaw, fins and gill parts each have a controller that,
// while active, replaces the body motion on its role bone's subtree: blend in, play its own motion, hold or loop,
// blend back out, off. The jaw holds its closed pose while the fish swims and fights and opens when it is landed
// (BUKKONUKI/CATCH: "parts off"); pattern steps' each bits flick the fins.
//
// Log, don't look: logs/rf3_models.txt gets one line per model loaded, one per change of what a slot draws, one per
// request ("fish-request") and, for the hooked fish, one per pattern step ("fish-pattern-step"), part trigger and
// parts-off change ("fish-parts").

#include "RF3FishModel.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
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
constexpr float kDefaultUnitsPerMetre = RF3_UNITS_PER_METRE; // user's choice (25e): the gameplay metre
constexpr float kFrameSeconds = 1.0f / 20.0f;                 // OoT's game logic runs at 20 Hz
// Step 25e's bend (when motions are off): how much of OoT's two swim angles each spine bone takes [assumed].
constexpr float kSpineShareA[7] = { 0.0f, 0.0f, 0.0f, 0.5f, 1.0f, 0.0f, 0.0f };
constexpr float kSpineShareB[7] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f };
// RF3 request codes (RF3-MECHANICS 6.2/6.3). In the table: patterns; others: raw animations.
constexpr int32_t kCodeTowed = 60;
constexpr int32_t kCodeSwimSmall = 2;
constexpr int32_t kCodeSwimMedium = 3;
constexpr int32_t kCodeSwimLarge = 4;
constexpr int32_t kCodeThrash = 15;
constexpr int32_t kCodeThrashBig = 16;
constexpr int32_t kCodeKuh = 28;
constexpr int32_t kRawSwimFast = 20;  // AWAY's first second (0x0019B7A0)
constexpr int32_t kRawTurnLeft = 25;  // held turns (25g)
constexpr int32_t kRawTurnRight = 26;
// Step 26g-2: RF3's part controllers (RF3-MECHANICS 6.5).
constexpr int32_t kParts = 9;               // 0 jaw, 1-4 hire2 fins, 5/6 pectorals, 7/8 gill parts
constexpr int32_t kPartOn = 0;              // RF3 controller states (+0x50)
constexpr int32_t kPartBlendIn = 1;
constexpr int32_t kPartBlendOut = 3;
constexpr int32_t kPartOff = 4;
constexpr int32_t kPartLooped = 0x08;       // +0x48 flags: wrapped this tick
constexpr int32_t kPartHolding = 0x10;      // ... held at the end
constexpr int32_t kMotionChest = 12;        // the pectorals' 胸 motion (0x00198130)
constexpr int32_t kMotionHoR = 18;          // their ホ motions (0x001981F0), controller 5 / 6
constexpr int32_t kMotionHoL = 17;
constexpr int32_t kModeBukkonuki = 15;      // fight modes that set "parts off" (0x001A5380, 0x001A55C0)
constexpr int32_t kModeCatch = 16;
// Raw animation playback speed by fishdata record +0x13 (fish +0xE8, 0x00194E10); 1.5x while fighting (0x0019B7A0).
constexpr float kAnimSpeedByClass[4] = { 2.0f, 1.5f, 1.0f, 2.0f };

using Quat = std::array<float, 4>; // w, x, y, z

struct Motion {
    int32_t start = -1;
    int32_t keys = 0;
    std::vector<float> times; // key k -> k+1
    float period = 0.0f;
    bool loop = false; // step 26g: the motion's run ends with -1 (loops); else it holds its last key
};

// Step 26g: one part controller of a model: its role bone and subtree, and its motion (tools/rf3_models.py parts).
struct PartDef {
    int32_t ctrl = -1;
    int32_t bone = -1;
    int32_t motion = -1;
    std::vector<int32_t> bones;
};

struct MotionSet {
    bool ok = false;
    int32_t frames = 0;
    std::vector<std::array<float, 3>> T;
    std::vector<Quat> R; // R(axis, -angle) as quaternions (RF3's angles are negated, step 25b)
    std::vector<uint16_t> tidx;
    std::vector<uint16_t> ridx;
    std::vector<Motion> motions;
    std::vector<PartDef> parts; // step 26g
};

struct PatternStep {
    float time = 0.0f;
    int32_t mot = 0;
    int32_t course = 0; // bit 0 left, 1 right, 2 up, 3 down
    float updown = 45.0f;
    float leftright = 90.0f;
    float motSpeed = 1.0f;
    float motChgSpeed = 0.0f;
    float bank = 0.0f;
    int32_t eachBits = 0;   // step 26g: 口 1, 背 2, ２ 4, 腹 8, 尻 0x10, 胸 0x20, ホ 0x40, 開 0x80, 閉 0x100, エ 0x200
    float eachSpeed = 0.0f; // the parts' playback speed for this step's triggers
};

struct Pattern {
    std::vector<PatternStep> steps;
};

struct CodeEntry {
    int32_t pattern = -1;
    int32_t flags = 0;
};

// Step 26g: one running part controller (RF3's 0x0014AA70 object).
struct PartCtrl {
    bool present = false;
    int32_t motion = -1;
    int32_t state = kPartOff;
    float time = 0.0f;     // motion time (on) or blend time (blend in/out), speed-scaled as RF3's +0x3C
    float blendDur = 0.0f; // RF3's +0x54, in the same speed-scaled time
    float speed = 2.0f;    // +0x44
    int32_t flags = 0;     // +0x48
    std::vector<std::array<float, 3>> snapT; // the owned bones' locals when the blend started (0x0014B9A0)
    std::vector<Quat> snapR;
};

struct SlotAnim {
    // the raw animation playing, and the one it blends from
    int32_t anim = -1;
    float animTime = 0.0f;
    int32_t prevAnim = -1;
    float prevAnimTime = 0.0f;
    float blend = 1.0f;     // 0..1: share of the current animation
    float blendTime = 0.0f; // seconds for a full blend
    float animRate = 1.0f;  // playback speed (RF3's anim +0x1368)
    // the request and its pattern
    int32_t code = -1;
    int32_t pattern = -1;
    int32_t flags = 0;
    int32_t step = 0;
    float stepLeft = 0.0f;
    bool patternDone = false;
    // course/bank offsets, degrees: yaw (+ = RF3's right), pitch (+ = down), roll (bank)
    float off[3] = { 0, 0, 0 };
    float target[3] = { 0, 0, 0 };
    float rate[3] = { 0, 0, 0 }; // degrees per second
    // inputs
    uint32_t lastFrame = 0;
    bool haveFrame = false;
    int16_t lastYaw = 0;
    int32_t lastMode = -1;
    float modeTime = 0.0f; // seconds in the current fight mode
    std::string why;
    bool animated = false; // last draw used RF3's motions
    // step 26g: the part controllers and their fish fields (+0xF0, +0xF4, +0xF8, +0xEC)
    std::string partsFile;
    std::array<PartCtrl, kParts> parts;
    float partSpeed0 = 2.0f;
    float partSpeed4 = 2.0f;
    bool jawLoop = true;
    bool partsOff = false;
    // step 26g-3: RF3's part timers (0x0019F500: jaw +0x1A0, gills +0x1A4; free swim 0x0019FD10: pectorals +0x1F4)
    float breathTimer = 0.0f;
    float gillTimer = 0.0f;
    float pectoralTimer = 0.0f;
    uint32_t rng = 1;
    std::vector<std::array<float, 3>> lastT; // last frame's final local poses (for blend snapshots)
    std::vector<Quat> lastR;
};

std::map<int32_t, std::string> sRecordFiles; // fishdata record -> file
std::map<int32_t, int32_t> sRecordClass;     // fishdata record -> +0x13 class (fish_move)
std::map<int32_t, std::array<float, 2>> sRecordMove; // fishdata record -> speed m/s, ramp s (fish_move, 26e-3)
std::vector<Pattern> sPatterns;
std::map<int32_t, CodeEntry> sCodes;
bool sIndexTried = false;
std::string sIndexWhy;
std::map<int32_t, std::string> sSlotState; // last logged state per region slot
std::map<std::string, std::unique_ptr<MotionSet>> sMotionSets; // by model file
std::map<int32_t, SlotAnim> sAnims;                           // by region slot
// Step 27c: per slot, the mouth points and drawn frame from the last draw, and the lure's line tie.
struct HookInfo {
    uint32_t frame = 0;
    bool have[3] = { false, false, false };
    float point[3][3] = {};
    float rot[9] = {}; // columns: the fish's x, y, z axes in world
    uint32_t tieFrame = 0;
    bool tieSet = false;
    float tie[3] = {};
};
std::map<int32_t, HookInfo> sHookInfo;
std::string sStatus = "no RF3 fish drawn yet";

void LoadIndex() {
    if (sIndexTried) {
        return;
    }
    sIndexTried = true;
    nlohmann::json j;
    if (!rf3model::ReadJson(kIndexPath, j, sIndexWhy)) {
        rf3model::Log("fish-index-load ok=0 why=" + sIndexWhy);
        return;
    }
    if (j.contains("fish_records") && j["fish_records"].is_object()) {
        for (auto& [key, value] : j["fish_records"].items()) {
            sRecordFiles[std::stoi(key)] = value.get<std::string>();
        }
    }
    if (j.contains("fish_move") && j["fish_move"].is_object()) {
        for (auto& [key, value] : j["fish_move"].items()) {
            sRecordClass[std::stoi(key)] = value.value("class", 0);
            sRecordMove[std::stoi(key)] = { value.value("speed_mps", 0.0f), value.value("ramp_s", 1.0f) };
        }
    }
    if (j.contains("patterns") && j["patterns"].is_array()) {
        for (const auto& p : j["patterns"]) {
            Pattern pat;
            for (const auto& s : p["steps"]) {
                PatternStep st;
                st.time = s.value("time", 0.0f);
                st.mot = s.value("mot", 0);
                st.course = s.value("course", 0);
                st.updown = s.value("updown", 45.0f);
                st.leftright = s.value("leftright", 90.0f);
                st.motSpeed = s.value("mot_speed", 1.0f);
                st.motChgSpeed = s.value("mot_chg_speed", 0.0f);
                st.bank = s.value("bank", 0.0f);
                st.eachBits = s.value("each_bits", 0);
                st.eachSpeed = s.value("each_speed", 0.0f);
                pat.steps.push_back(st);
            }
            sPatterns.push_back(pat);
        }
    }
    if (j.contains("pattern_codes") && j["pattern_codes"].is_array()) {
        for (const auto& c : j["pattern_codes"]) {
            int32_t code = c.value("code", -1);
            int32_t pattern = c.value("pattern", -1);
            if (code >= 0 && pattern >= 0 && pattern < static_cast<int32_t>(sPatterns.size())) {
                sCodes[code] = { pattern, c.value("flags", 0) };
            }
        }
    }
    if (sRecordFiles.empty()) {
        sIndexWhy = "models.json has no fish (rebuild with tools/rf3_models.py models)";
    }
    rf3model::Log("fish-index-load ok=" + std::to_string(sRecordFiles.empty() ? 0 : 1) +
                  " records=" + std::to_string(sRecordFiles.size()) + " patterns=" + std::to_string(sPatterns.size()) +
                  " codes=" + std::to_string(sCodes.size()) + (sIndexWhy.empty() ? "" : " why=" + sIndexWhy));
}

rf3model::SkinnedModel* GetForRecord(int32_t record, std::string& why) {
    LoadIndex();
    auto rf = sRecordFiles.find(record);
    if (rf == sRecordFiles.end()) {
        why = sRecordFiles.empty() ? sIndexWhy : "no RF3 model for record " + std::to_string(record);
        return nullptr;
    }
    return rf3model::Get(rf->second, "fish", "record=" + std::to_string(record));
}

void NoteSlot(int32_t slot, const std::string& state) {
    sStatus = "slot " + std::to_string(slot) + ": " + state;
    auto& last = sSlotState[slot];
    if (last != state) {
        last = state;
        rf3model::Log("slot=" + std::to_string(slot) + " " + state);
    }
}

// ---- quaternions

Quat AxisAngleQuat(float x, float y, float z, float angle) {
    float ln = std::sqrt(x * x + y * y + z * z);
    if (ln < 1e-9f || std::fabs(angle) < 1e-12f) {
        return { 1.0f, 0.0f, 0.0f, 0.0f };
    }
    float s = std::sin(angle * 0.5f) / ln;
    return { std::cos(angle * 0.5f), x * s, y * s, z * s };
}

Quat Slerp(Quat a, const Quat& b, float u) {
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (d < 0.0f) { // shortest way
        for (float& c : a) {
            c = -c;
        }
        d = -d;
    }
    float wa, wb;
    if (d > 0.9995f) {
        wa = 1.0f - u;
        wb = u;
    } else {
        float th = std::acos(std::min(1.0f, d));
        float s = std::sin(th);
        wa = std::sin((1.0f - u) * th) / s;
        wb = std::sin(u * th) / s;
    }
    Quat q = { wa * a[0] + wb * b[0], wa * a[1] + wb * b[1], wa * a[2] + wb * b[2], wa * a[3] + wb * b[3] };
    float ln = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (float& c : q) {
        c /= (ln > 0.0f ? ln : 1.0f);
    }
    return q;
}

// The same rotation matrix as rf3model::AxisAngle for the same axis and angle.
Mat3 QuatToMat(const Quat& q) {
    float w = q[0], x = q[1], y = q[2], z = q[3];
    Mat3 m;
    m[0] = { 1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y) };
    m[1] = { 2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x) };
    m[2] = { 2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y) };
    return m;
}

// ---- the model's raw animations

MotionSet* GetMotions(const rf3model::SkinnedModel& m) {
    auto it = sMotionSets.find(m.file);
    if (it != sMotionSets.end()) {
        return it->second.get();
    }
    auto set = std::make_unique<MotionSet>();
    if (m.extra.contains("motion")) {
        const auto& j = m.extra["motion"];
        set->frames = j.value("frames", 0);
        for (const auto& t : j["T"]) {
            set->T.push_back({ t[0].get<float>(), t[1].get<float>(), t[2].get<float>() });
        }
        for (const auto& r : j["R"]) {
            set->R.push_back(AxisAngleQuat(r[0].get<float>(), r[1].get<float>(), r[2].get<float>(), -r[3].get<float>()));
        }
        for (const auto& v : j["tidx"]) {
            set->tidx.push_back(v.get<uint16_t>());
        }
        for (const auto& v : j["ridx"]) {
            set->ridx.push_back(v.get<uint16_t>());
        }
        for (const auto& mo : j["motions"]) {
            Motion x;
            x.start = mo["start"].get<int32_t>();
            x.keys = mo["keys"].get<int32_t>();
            for (const auto& t : mo["times"]) {
                x.times.push_back(std::max(0.001f, t.get<float>()));
                x.period += x.times.back();
            }
            x.loop = mo.value("loop", false);
            set->motions.push_back(x);
        }
        if (m.extra.contains("parts") && m.extra["parts"].is_array()) { // step 26g
            for (const auto& pj : m.extra["parts"]) {
                PartDef d;
                d.ctrl = pj.value("ctrl", -1);
                d.bone = pj.value("bone", -1);
                d.motion = pj.value("motion", -1);
                for (const auto& b : pj["bones"]) {
                    int32_t bi = b.get<int32_t>();
                    if (bi > 0 && bi < static_cast<int32_t>(m.bones.size())) { // the root keeps its bind pose
                        d.bones.push_back(bi);
                    }
                }
                if (d.ctrl >= 0 && d.ctrl < kParts) {
                    set->parts.push_back(d);
                }
            }
        }
        const size_t need = static_cast<size_t>(set->frames) * m.bones.size();
        bool sizes = set->frames > 0 && set->tidx.size() == need && set->ridx.size() == need;
        bool indices = sizes && std::all_of(set->tidx.begin(), set->tidx.end(), [&](uint16_t v) { return v < set->T.size(); }) &&
                       std::all_of(set->ridx.begin(), set->ridx.end(), [&](uint16_t v) { return v < set->R.size(); });
        set->ok = indices;
    }
    rf3model::Log("fish-motions-load file=" + m.file + " ok=" + std::to_string(set->ok ? 1 : 0) +
                  " motions=" + std::to_string(set->motions.size()) + " frames=" + std::to_string(set->frames) +
                  " T=" + std::to_string(set->T.size()) + " R=" + std::to_string(set->R.size()) +
                  " parts=" + std::to_string(set->parts.size()));
    MotionSet* out = set.get();
    sMotionSets[m.file] = std::move(set);
    return out;
}

bool Playable(const MotionSet& s, int32_t anim) {
    return anim >= 0 && anim < static_cast<int32_t>(s.motions.size()) && s.motions[anim].keys >= 2 &&
           s.motions[anim].start >= 0 && s.motions[anim].start + s.motions[anim].keys <= s.frames;
}

// The "Kuh!" jolt (the model's last animation) and the dash turns 5/6 play once; the rest loop.
bool Loops(const MotionSet& s, int32_t anim) {
    return anim != static_cast<int32_t>(s.motions.size()) - 1 && anim != 5 && anim != 6;
}

// Bone b's local pose in anim at time t, t already within 0 .. the motion's period.
void SampleAt(const MotionSet& s, int32_t anim, float t, int32_t b, std::array<float, 3>& outT, Quat& outR) {
    const Motion& mo = s.motions[anim];
    t = std::clamp(t, 0.0f, mo.period);
    int32_t k = 0;
    while (k < mo.keys - 2 && t >= mo.times[k]) {
        t -= mo.times[k];
        k++;
    }
    float u = std::clamp(t / mo.times[k], 0.0f, 1.0f);
    const size_t f0 = static_cast<size_t>(b) * s.frames + mo.start + k;
    const size_t f1 = f0 + 1;
    const auto& t0 = s.T[s.tidx[f0]];
    const auto& t1 = s.T[s.tidx[f1]];
    for (int i = 0; i < 3; i++) {
        outT[i] = t0[i] + (t1[i] - t0[i]) * u;
    }
    outR = Slerp(s.R[s.ridx[f0]], s.R[s.ridx[f1]], u);
}

// Bone b's local pose in anim at time t (looping animations wrap, the rest hold their end).
void Sample(const MotionSet& s, int32_t anim, float t, int32_t b, std::array<float, 3>& outT, Quat& outR) {
    const Motion& mo = s.motions[anim];
    if (Loops(s, anim) && mo.period > 0.0f) {
        t = std::fmod(std::max(0.0f, t), mo.period);
    }
    SampleAt(s, anim, t, b, outT, outR);
}

// ---- step 26g: the part controllers (RF3-MECHANICS 6.5)

const PartDef* FindPart(const MotionSet& s, int32_t ctrl) {
    for (const auto& d : s.parts) {
        if (d.ctrl == ctrl) {
            return &d;
        }
    }
    return nullptr;
}

// 0x0014B9A0: the owned bones' current locals become the blend's start.
void PartSnapshot(const SlotAnim& a, const PartDef& d, PartCtrl& c) {
    c.snapT.clear();
    c.snapR.clear();
    if (a.lastT.empty()) {
        return; // no frame drawn yet: blends start at their target
    }
    for (int32_t b : d.bones) {
        c.snapT.push_back(a.lastT[b]);
        c.snapR.push_back(a.lastR[b]);
    }
}

// 0x0014B440: off -> blend in (restart from the motion's start).
bool PartRestart(SlotAnim& a, const MotionSet& s, int32_t ctrl, float blend) {
    const PartDef* d = FindPart(s, ctrl);
    PartCtrl& c = a.parts[ctrl];
    if (d == nullptr || !c.present || c.state != kPartOff) {
        return false;
    }
    PartSnapshot(a, *d, c);
    c.state = kPartBlendIn;
    c.blendDur = blend;
    c.time = 0.0f;
    c.flags = 0;
    return true;
}

// 0x0014B4C0: on -> blend out to the body motion.
bool PartBlendOut(SlotAnim& a, const MotionSet& s, int32_t ctrl, float blend) {
    const PartDef* d = FindPart(s, ctrl);
    PartCtrl& c = a.parts[ctrl];
    if (d == nullptr || !c.present || c.state != kPartOn) {
        return false;
    }
    PartSnapshot(a, *d, c);
    c.state = kPartBlendOut;
    c.blendDur = blend;
    c.time = 0.0f;
    return true;
}

// 0x0014B590: switch to another motion and blend in.
bool PartSwitch(SlotAnim& a, const MotionSet& s, int32_t ctrl, int32_t motion, float blend) {
    PartCtrl& c = a.parts[ctrl];
    if (motion == c.motion) {
        return PartRestart(a, s, ctrl, blend);
    }
    const PartDef* d = FindPart(s, ctrl);
    if (d == nullptr || !c.present || !Playable(s, motion)) {
        return false;
    }
    c.motion = motion;
    PartSnapshot(a, *d, c);
    c.state = kPartBlendIn;
    c.blendDur = blend;
    c.time = 0.0f;
    c.flags = 0;
    return true;
}

// 0x001984C0 (fish load): the jaw plays its closed pose and holds it; the others blend out at once (off).
void PartsInit(SlotAnim& a, const MotionSet& s, const std::string& file, int32_t slot) {
    a.partsFile = file;
    a.parts = {};
    a.partSpeed0 = a.partSpeed4 = 2.0f;
    a.jawLoop = true;
    a.partsOff = false;
    a.breathTimer = a.gillTimer = a.pectoralTimer = 0.0f;
    a.rng = 0x2545F491u ^ static_cast<uint32_t>(slot * 7919 + 1);
    a.lastT.clear();
    a.lastR.clear();
    std::string list;
    for (const auto& d : s.parts) {
        PartCtrl& c = a.parts[d.ctrl];
        c.present = Playable(s, d.motion);
        c.motion = d.motion;
        c.speed = 2.0f;
        c.state = (d.ctrl == 0) ? kPartOn : kPartOff;
        list += " " + std::to_string(d.ctrl) + ":" + std::to_string(d.bone) + (c.present ? "" : "(no motion)");
    }
    rf3model::Log("slot=" + std::to_string(slot) + " fish-parts load file=" + file +
                  " parts=" + std::to_string(s.parts.size()) + " ctrl:bone" + list);
}

// The jaw's open (0x00197EE0): if it holds its closed pose, blend out to the body (open) at the parts' speed.
bool JawOpen(SlotAnim& a, const MotionSet& s) {
    PartCtrl& jaw = a.parts[0];
    if (!jaw.present || !(jaw.flags & kPartHolding)) {
        return false;
    }
    jaw.speed = a.partSpeed4;
    return PartBlendOut(a, s, 0, 1.0f / jaw.speed);
}

// 0x00198130 (胸) / 0x001981F0 (ホ): if both pectorals are off, both switch to motion 12, or to 18 / 17.
bool Pectorals(SlotAnim& a, const MotionSet& s, bool chest) {
    PartCtrl& p5 = a.parts[5];
    PartCtrl& p6 = a.parts[6];
    if (!p5.present || !p6.present || p5.state != kPartOff || p6.state != kPartOff) {
        return false;
    }
    p5.speed = p6.speed = a.partSpeed0;
    bool ok = PartSwitch(a, s, 5, chest ? kMotionChest : kMotionHoR, 1.0f / a.partSpeed0);
    return PartSwitch(a, s, 6, chest ? kMotionChest : kMotionHoL, 1.0f / a.partSpeed0) || ok;
}

// 0x001982B0: if both gill parts are off, both restart at the parts' speed.
bool Gills(SlotAnim& a, const MotionSet& s) {
    PartCtrl& g7 = a.parts[7];
    PartCtrl& g8 = a.parts[8];
    if (!g7.present || !g8.present || g7.state != kPartOff || g8.state != kPartOff) {
        return false;
    }
    g7.speed = g8.speed = a.partSpeed0;
    PartRestart(a, s, 7, 0.02f);
    PartRestart(a, s, 8, 0.02f);
    return true;
}

uint32_t NextRandom(SlotAnim& a) {
    a.rng = a.rng * 1103515245u + 12345u;
    return (a.rng >> 16) & 0x7FFF;
}

// RF3's timers outside the patterns (step 26g-3), while parts are on:
// - every state (0x0019F500): the jaw, while holding its closed pose, opens and closes again (a breath) every
//   3.0-6.5 s (0.5 steps); the gill parts (bass only: RF3 checks species 8/9, the only fish with them) flap every
//   2.0-2.75 s (0.25 steps)
// - free swimming (0x0019FD10, not hooked here [assumed for chase/wait too]): the pectorals beat (ホ) every 1.0, 1.5 or
//   2.0 s. (WAIT uses 3.0 s, 0x001A21C0; the mod has no WAIT of its own.)
// Each timer only runs while its part is ready (as RF3's conditions), and restarts after it fires.
void PartsTimers(SlotAnim& a, const MotionSet& s, float dt, bool hooked, int32_t slot, bool log) {
    if (a.partsOff) {
        return;
    }
    std::string fired;
    PartCtrl& jaw = a.parts[0];
    if (jaw.present && (jaw.flags & kPartHolding)) {
        a.breathTimer -= dt;
        if (a.breathTimer <= 0.0f) {
            fired += JawOpen(a, s) ? " breath" : "";
            a.jawLoop = true;
            a.breathTimer = static_cast<float>(NextRandom(a) % 8) * 0.5f + 3.0f;
        }
    }
    if (a.parts[7].present && a.parts[8].present && a.parts[7].state == kPartOff && a.parts[8].state == kPartOff) {
        a.gillTimer -= dt;
        if (a.gillTimer <= 0.0f) {
            fired += Gills(a, s) ? " gills" : "";
            a.gillTimer = static_cast<float>(NextRandom(a) % 4) * 0.25f + 2.0f;
        }
    }
    if (!hooked && a.parts[5].present && a.parts[6].present && a.parts[5].state == kPartOff &&
        a.parts[6].state == kPartOff) {
        a.pectoralTimer -= dt;
        if (a.pectoralTimer <= 0.0f) {
            fired += Pectorals(a, s, false) ? " pectorals-ho" : "";
            a.pectoralTimer = static_cast<float>(NextRandom(a) % 3 + 1) * 0.5f + 0.5f;
        }
    }
    if (log && !fired.empty()) {
        rf3model::Log("slot=" + std::to_string(slot) + " fish-parts timer:" + fired);
    }
}

// A pattern step's each bits (0x00196FC0 lines 126-172). Logs what started (hooked fish only).
void PartsStep(SlotAnim& a, const MotionSet& s, const PatternStep& st, int32_t slot, bool log) {
    if (a.partsOff || st.eachBits == 0 || s.parts.empty()) {
        return;
    }
    const int32_t bits = st.eachBits;
    a.partSpeed0 = (st.eachSpeed != 0.0f) ? st.eachSpeed : 0.01f; // RF3: time scale (1 here) x each_speed
    std::string started;
    if (bits & 0x001) { // 口: a gulp (open, then close again)
        a.partSpeed4 = a.partSpeed0;
        started += JawOpen(a, s) ? " jaw-open" : "";
        a.jawLoop = true;
    } else if (bits & 0x080) { // 開: open and stay open
        a.partSpeed4 = a.partSpeed0;
        started += JawOpen(a, s) ? " jaw-open" : "";
        a.jawLoop = false;
    } else if (bits & 0x100) { // 閉: close on the next frame
        a.partSpeed4 = a.partSpeed0;
        a.jawLoop = true;
    }
    const int32_t finBits[4] = { 0x002, 0x004, 0x008, 0x010 }; // 背 ２ 腹 尻 -> controllers 1-4
    for (int32_t i = 0; i < 4; i++) {
        PartCtrl& c = a.parts[1 + i];
        if ((bits & finBits[i]) && c.present && c.state == kPartOff) {
            c.speed = a.partSpeed0;
            if (PartRestart(a, s, 1 + i, 0.02f)) {
                started += " fin" + std::to_string(1 + i);
            }
        }
    }
    if (bits & 0x060) {
        bool chest = (bits & 0x020) != 0; // 胸 wins over ホ
        started += Pectorals(a, s, chest) ? (chest ? " pectorals-chest" : " pectorals-ho") : "";
    }
    if (bits & 0x200) {
        started += Gills(a, s) ? " gills" : "";
    }
    if (log && !started.empty()) {
        char line[160];
        std::snprintf(line, sizeof(line), "slot=%d fish-parts step bits=0x%03X speed=%.2f started:%s", slot, bits,
                      a.partSpeed0, started.c_str());
        rf3model::Log(line);
    }
}

// One game frame: RF3's per-frame rules (0x00198360), then each controller's tick (0x0014B090).
void PartsFrame(SlotAnim& a, const MotionSet& s, float dt) {
    PartCtrl& jaw = a.parts[0];
    if (!a.partsOff) {
        if (jaw.present && jaw.state == kPartOff && a.jawLoop) { // close again
            jaw.speed = a.partSpeed4;
            PartRestart(a, s, 0, 0.01f);
        }
    } else if (jaw.present && (jaw.flags & kPartHolding)) { // landed: open over 0.5
        PartBlendOut(a, s, 0, 0.5f);
    }
    for (int32_t i = 1; i < kParts; i++) {
        PartCtrl& c = a.parts[i];
        if (c.present && c.state == kPartOn && (c.flags & (kPartLooped | kPartHolding))) {
            PartBlendOut(a, s, i, 1.0f / std::max(0.01f, c.speed));
        }
    }
    for (auto& c : a.parts) {
        if (!c.present || c.state == kPartOff) {
            continue;
        }
        if (c.flags & kPartLooped) {
            c.flags = 0;
        }
        c.time += dt * c.speed;
        if (c.state == kPartOn) {
            const Motion& mo = s.motions[c.motion];
            if (c.time >= mo.period) {
                if (mo.loop && mo.period > 0.0f) {
                    c.time = std::fmod(c.time, mo.period);
                    c.flags = kPartLooped;
                } else {
                    c.time = mo.period;
                    c.flags = kPartHolding;
                }
            }
        } else if (c.time > c.blendDur) {
            c.state = (c.state == kPartBlendIn) ? kPartOn : kPartOff;
            c.time = 0.0f;
        }
    }
}

// 0x00149F60: an active controller replaces the body pose on its bones (on: its motion; blend in: snapshot ->
// its motion's start; blend out: snapshot -> the body pose).
void PartsApply(const SlotAnim& a, const MotionSet& s, std::vector<std::array<float, 3>>& lt, std::vector<Quat>& lq) {
    for (const auto& d : s.parts) {
        const PartCtrl& c = a.parts[d.ctrl];
        if (!c.present || c.state == kPartOff) {
            continue;
        }
        float u = (c.blendDur > 0.0f) ? std::clamp(c.time / c.blendDur, 0.0f, 1.0f) : 1.0f;
        for (size_t k = 0; k < d.bones.size(); k++) {
            const int32_t b = d.bones[k];
            if (b >= static_cast<int32_t>(lt.size())) {
                continue;
            }
            std::array<float, 3> t;
            Quat q;
            if (c.state == kPartOn) {
                SampleAt(s, c.motion, c.time, b, t, q);
            } else {
                if (c.state == kPartBlendIn) {
                    SampleAt(s, c.motion, 0.0f, b, t, q);
                } else {
                    t = lt[b];
                    q = lq[b];
                }
                if (k < c.snapT.size()) {
                    for (int i = 0; i < 3; i++) {
                        t[i] = c.snapT[k][i] + (t[i] - c.snapT[k][i]) * u;
                    }
                    q = Slerp(c.snapR[k], q, u);
                }
            }
            lt[b] = t;
            lq[b] = q;
        }
    }
}

// ---- the pattern player (RF3's 0x00197C90 / 0x00196FC0, RF3-MECHANICS 6.2)

// Starts a raw animation with a blend of blendSeconds (a jolt or dash restarts even when playing).
void StartAnim(SlotAnim& a, const MotionSet& s, int32_t anim, float blendSeconds) {
    // The "Kuh!" pattern names animation 28: the bass rig's last; 39-bone fish have 28 animations, the jolt is 27.
    if (anim >= static_cast<int32_t>(s.motions.size()) && !s.motions.empty()) {
        anim = static_cast<int32_t>(s.motions.size()) - 1;
    }
    if (!Playable(s, anim)) {
        return;
    }
    a.prevAnim = a.anim;
    a.prevAnimTime = a.animTime;
    a.anim = anim;
    a.animTime = 0.0f;
    a.blendTime = blendSeconds;
    a.blend = (a.prevAnim >= 0 && blendSeconds > 0.0f) ? 0.0f : 1.0f;
}

void StartStep(SlotAnim& a, const MotionSet& s, int32_t slot, bool logSteps) {
    const Pattern& p = sPatterns[a.pattern];
    // RF3 skips steps with a negative time.
    while (a.step < static_cast<int32_t>(p.steps.size()) && p.steps[a.step].time < 0.0f) {
        a.step++;
    }
    if (a.step >= static_cast<int32_t>(p.steps.size())) {
        a.patternDone = true;
        return;
    }
    const PatternStep& st = p.steps[a.step];
    a.stepLeft = st.time;
    int32_t mot = st.mot;
    if (mot >= 0) {
        StartAnim(a, s, mot, st.motChgSpeed);
    } else if (-mot != a.anim) { // negative: continue if already playing
        StartAnim(a, s, -mot, st.motChgSpeed);
    }
    a.animRate = st.motSpeed;
    float dur = std::max(0.001f, st.time);
    // course: left subtracts / right adds the left-right angle (yaw); up subtracts / down adds the up-down angle
    // (pitch); bank adds to the roll. Each eased over the step.
    float d[3] = { 0, 0, 0 };
    if (st.course & 1) {
        d[0] = -st.leftright;
    } else if (st.course & 2) {
        d[0] = st.leftright;
    }
    if (st.course & 4) {
        d[1] = -st.updown;
    } else if (st.course & 8) {
        d[1] = st.updown;
    }
    d[2] = st.bank;
    for (int i = 0; i < 3; i++) {
        a.target[i] += d[i];
        a.rate[i] = d[i] / dur;
    }
    PartsStep(a, s, st, slot, logSteps); // step 26g
    if (logSteps) {
        char line[200];
        std::snprintf(line, sizeof(line),
                      "slot=%d fish-pattern-step code=%d pattern=%d step=%d anim=%d rate=%.2f blend=%.2f "
                      "yaw=%.0f pitch=%.0f roll=%.0f (targets, deg)",
                      slot, a.code, a.pattern, a.step, a.anim, a.animRate, st.motChgSpeed, a.target[0], a.target[1],
                      a.target[2]);
        rf3model::Log(line);
    }
}

void ClearOffsets(SlotAnim& a) {
    for (int i = 0; i < 3; i++) {
        a.off[i] = a.target[i] = a.rate[i] = 0.0f;
    }
}

// RF3's 0x00196D70 + the engine's change: a new code clears the offsets; a code in the table plays its pattern,
// another plays that raw animation at rawRate.
void Request(SlotAnim& a, const MotionSet& s, int32_t slot, int32_t code, float blendSeconds, float rawRate,
             const std::string& why, bool logSteps) {
    if (code == a.code) {
        return;
    }
    a.code = code;
    a.why = why;
    ClearOffsets(a);
    auto it = sCodes.find(code);
    if (it != sCodes.end()) {
        a.pattern = it->second.pattern;
        a.flags = it->second.flags;
        a.step = 0;
        a.patternDone = false;
        StartStep(a, s, slot, logSteps);
    } else {
        a.pattern = -1;
        a.flags = 0;
        StartAnim(a, s, code, blendSeconds);
        a.animRate = rawRate;
    }
    char line[220];
    std::snprintf(line, sizeof(line), "slot=%d fish-request code=%d pattern=%d anim=%d rate=%.2f why=%s", slot, code,
                  a.pattern, a.anim, a.animRate, why.c_str());
    rf3model::Log(line);
}

// One game frame of the pattern: offsets ease toward their targets; at a step's end they snap and the next starts;
// at the pattern's end they are cleared, and the pattern loops (flag 0x08) or holds its last animation.
void AdvancePattern(SlotAnim& a, const MotionSet& s, int32_t slot, float dt, bool logSteps) {
    if (a.pattern < 0 || a.patternDone) {
        return;
    }
    for (int i = 0; i < 3; i++) {
        a.off[i] += a.rate[i] * dt;
    }
    a.stepLeft -= dt;
    if (a.stepLeft > 0.0f) {
        return;
    }
    for (int i = 0; i < 3; i++) {
        a.off[i] = a.target[i];
        a.rate[i] = 0.0f;
    }
    a.step++;
    if (a.step >= static_cast<int32_t>(sPatterns[a.pattern].steps.size())) {
        ClearOffsets(a);
        if (a.flags & 0x08) {
            a.step = 0;
        } else {
            a.patternDone = true;
            return;
        }
    }
    StartStep(a, s, slot, logSteps);
}

// The request and why [RF3 codes per fight state, RF3-MECHANICS 6.3; free swimming: 25g's speed rule as codes].
int32_t ChooseRequest(const RF3FishDrawInput& in, SlotAnim& a, int32_t yawRate, float& blend, std::string& why) {
    int32_t forced = CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.FishMotionForce"), -1);
    if (forced >= 0) {
        why = "forced";
        blend = 0.2f;
        return forced;
    }
    if (in.hooked) {
        RF3FightStatus f;
        RF3Fight_GetStatus(&f);
        int32_t mode = f.active ? f.mode : 0;
        why = "hooked mode=" + std::to_string(mode);
        blend = 0.2f; // RF3's fight states ask with 0.2 s (HIT 0.02, FOLLOW 0.5)
        switch (mode) {
            case 2: // HIT
                blend = 0.02f;
                return kCodeKuh;
            case 8: // RESIST
                return kCodeThrash;
            case 9: // STRUGGLE
                return kCodeThrashBig;
            case 10: // AWAY: no code, so 0x0019B7A0's accelerating swim: raw 20 for 1 s, then 4 until 2 s, then 3
                blend = 0.1f;
                return a.modeTime < 1.0f ? kRawSwimFast : (a.modeTime < 2.0f ? kCodeSwimLarge : kCodeSwimMedium);
            case 11: // FOLLOW (its turns come with 26e-3)
                blend = 0.5f;
                return kCodeTowed;
            case 12: // STIFFEN (its turn codes come with 26e-3)
            case 7:  // STOP
            case 15: // BUKKONUKI
            case 16: // CATCH
                return kCodeSwimSmall;
            default:
                return kCodeTowed;
        }
    }
    blend = 1.0f; // the free swim asks with 1.0 s (0x0019FD10)
    int32_t turn = CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.FishMotionTurnRate"), 0x300);
    if (std::abs(yawRate) > turn) {
        why = "turn rate=" + std::to_string(yawRate);
        blend = 0.2f;
        return yawRate > 0 ? kRawTurnLeft : kRawTurnRight; // [assumed] which way +yaw is (25g)
    }
    float slow = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.FishMotionSlow"), 1.0f);
    float fast = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.FishMotionFast"), 3.0f);
    char buf[48];
    std::snprintf(buf, sizeof(buf), "swim speed=%.2f", in.speedXZ);
    why = buf;
    return in.speedXZ < slow ? kCodeSwimSmall : (in.speedXZ < fast ? kCodeSwimMedium : kCodeSwimLarge);
}

// Step 25e's bend (motions off): OoT's swim angles turn the spine bones about the model's vertical.
std::vector<Mat3> SpineTurns(const rf3model::SkinnedModel& m, float angleA, float angleB) {
    std::vector<Mat3> bindRot;
    std::vector<std::array<float, 3>> bindPos;
    rf3model::BoneWorlds(m, nullptr, bindRot, bindPos);
    std::vector<Mat3> turns(m.bones.size(), rf3model::Identity());
    if (!m.extra.contains("spine")) {
        return turns;
    }
    const auto& spine = m.extra["spine"];
    for (size_t k = 0; k < spine.size() && k < 7; k++) {
        int32_t b = spine[k].get<int32_t>();
        float turn = kSpineShareA[k] * angleA + kSpineShareB[k] * angleB;
        if (b < 0 || b >= static_cast<int32_t>(m.bones.size()) || turn == 0.0f) {
            continue;
        }
        int32_t p = m.bones[b].parent;
        Mat3 prot = p >= 0 ? bindRot[p] : rf3model::Identity();
        turns[b] = rf3model::AxisAngle(prot[1][0], prot[1][1], prot[1][2], turn); // prot^T * (0, 1, 0)
    }
    return turns;
}

// Advances the slot's request, pattern and animation clocks and fills the bones' local poses. Returns false if
// motions can't play.
bool MotionPose(const rf3model::SkinnedModel& m, int32_t record, const RF3FishDrawInput& in,
                std::vector<std::array<float, 3>>& lt, std::vector<Mat3>& lr) {
    MotionSet* s = GetMotions(m);
    if (s == nullptr || !s->ok) {
        return false;
    }
    SlotAnim& a = sAnims[in.slot];
    if (a.partsFile != m.file) {
        PartsInit(a, *s, m.file, in.slot); // step 26g
    }
    int32_t frames = 1;
    if (a.haveFrame) {
        frames = static_cast<int32_t>(in.frame - a.lastFrame);
        frames = std::clamp(frames, 0, 4);
    }
    int32_t yawRate = a.haveFrame && frames > 0 ? static_cast<int16_t>(in.yaw - a.lastYaw) / frames : 0;
    a.lastFrame = in.frame;
    a.lastYaw = in.yaw;
    a.haveFrame = true;
    float dt = frames * kFrameSeconds;

    // fight mode clock (AWAY's accelerating swim)
    int32_t mode = -1;
    if (in.hooked) {
        RF3FightStatus f;
        RF3Fight_GetStatus(&f);
        mode = f.active ? f.mode : 0;
    }
    if (mode != a.lastMode) {
        a.lastMode = mode;
        a.modeTime = 0.0f;
    } else {
        a.modeTime += dt;
    }
    // step 26g: "parts off" (+0xEC) while being landed (BUKKONUKI/CATCH): the jaw opens
    bool off = in.hooked && (mode == kModeBukkonuki || mode == kModeCatch);
    if (off != a.partsOff) {
        a.partsOff = off;
        rf3model::Log("slot=" + std::to_string(in.slot) + " fish-parts parts-off=" + (off ? "1" : "0") +
                      " mode=" + std::to_string(mode));
    }

    int32_t cls = 0;
    auto rc = sRecordClass.find(record);
    if (rc != sRecordClass.end()) {
        cls = std::clamp(rc->second, 0, 3);
    }
    float rawRate = kAnimSpeedByClass[cls] * (in.hooked ? 1.5f : 1.0f);

    float blend = 0.2f;
    std::string why;
    int32_t want = ChooseRequest(in, a, yawRate, blend, why);
    if (sCodes.find(want) == sCodes.end() && !Playable(*s, want)) {
        why += " (code " + std::to_string(want) + " not playable)";
        want = kCodeSwimSmall;
    }
    Request(a, *s, in.slot, want, blend, rawRate, why, in.hooked != 0);
    if (a.pattern < 0) {
        a.animRate = rawRate; // raw animations follow the fish's speed class (and the fight's 1.5x)
    }
    AdvancePattern(a, *s, in.slot, dt, in.hooked != 0);
    if (!Playable(*s, a.anim)) {
        StartAnim(a, *s, kCodeSwimMedium, 0.0f); // nothing playable yet: the medium swim animation
        if (!Playable(*s, a.anim)) {
            return false;
        }
    }
    float speedScale = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.FishMotionRate"), 1.0f);
    a.animTime += dt * a.animRate * speedScale;
    if (a.prevAnim >= 0) {
        a.prevAnimTime += dt * a.animRate * speedScale;
    }
    a.blend = (a.blendTime > 0.0f) ? std::min(1.0f, a.blend + dt / a.blendTime) : 1.0f;
    const bool parts = CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.FishParts"), 1) != 0; // step 26g
    if (parts) {
        PartsTimers(a, *s, dt, in.hooked != 0, in.slot, in.hooked != 0); // step 26g-3
        PartsFrame(a, *s, dt * speedScale);
    }

    const size_t n = m.bones.size();
    lt.assign(n, { 0, 0, 0 });
    lr.assign(n, rf3model::Identity());
    std::vector<Quat> lq(n, Quat{ 1.0f, 0.0f, 0.0f, 0.0f });
    for (size_t b = 0; b < n; b++) {
        if (b == 0) { // the root keeps its bind pose (OoT moves the actor)
            lt[b] = { m.bones[0].t[0], m.bones[0].t[1], m.bones[0].t[2] };
            lr[b] = m.bones[0].rot;
            continue;
        }
        std::array<float, 3> t;
        Quat q;
        Sample(*s, a.anim, a.animTime, static_cast<int32_t>(b), t, q);
        if (a.blend < 1.0f && Playable(*s, a.prevAnim)) {
            std::array<float, 3> pt;
            Quat pq;
            Sample(*s, a.prevAnim, a.prevAnimTime, static_cast<int32_t>(b), pt, pq);
            for (int i = 0; i < 3; i++) {
                t[i] = pt[i] + (t[i] - pt[i]) * a.blend;
            }
            q = Slerp(pq, q, a.blend);
        }
        lt[b] = t;
        lq[b] = q;
    }
    if (parts) {
        PartsApply(a, *s, lt, lq);
    }
    a.lastT = lt;
    a.lastR = lq;
    for (size_t b = 1; b < n; b++) {
        lr[b] = QuatToMat(lq[b]);
    }
    return true;
}

} // namespace

extern "C" int32_t RF3FishModel_DrawRegion(PlayState* play, const RF3FishDrawInput* in, float* mouthOut) {
    const int32_t slot = in->slot;
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
    rf3model::SkinnedModel* m = GetForRecord(record, why);
    if (m == nullptr || !m->ok) {
        NoteSlot(slot, "fish-model-off record=" + std::to_string(record) + " why=" + (m ? m->why : why));
        return 0;
    }
    float unitsPerMetre = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.FishModelScale"), kDefaultUnitsPerMetre);
    float bend = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.FishModelBend"), 1.0f);
    bool bindPose = CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.FishModelBindPose"), 0) != 0;
    bool motions = CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.FishMotions"), 1) != 0 && !bindPose;
    float lengthM = std::fmax(0.01f, m->sizeM[2]); // fish lie along z
    float lengthUnits = info.mm / 1000.0f * unitsPerMetre;
    float scale = (info.mm / 1000.0f) / lengthM * unitsPerMetre / m->posScale;

    std::vector<Mat3> rot;
    std::vector<std::array<float, 3>> pos;
    std::vector<std::array<float, 3>> lt;
    std::vector<Mat3> lr;
    bool animated = motions && MotionPose(*m, record, *in, lt, lr);
    sAnims[slot].animated = animated;
    if (animated) {
        rf3model::BoneWorldsLocal(*m, lt, lr, rot, pos);
    } else {
        float a = bindPose ? 0.0f : bend * in->bendA * static_cast<float>(M_PI) / 32768.0f;
        float b = bindPose ? 0.0f : bend * in->bendB * static_cast<float>(M_PI) / 32768.0f;
        std::vector<Mat3> turns = SpineTurns(*m, a, b);
        rf3model::BoneWorlds(*m, &turns, rot, pos);
    }
    char state[220];
    std::snprintf(state, sizeof(state),
                  "fish-model-draw record=%d preview=%d mm=%.0f units_per_m=%.0f length_units=%.1f animate=%s",
                  record, preview ? 1 : 0, info.mm, unitsPerMetre, lengthUnits,
                  animated ? "rf3-patterns" : (bindPose ? "bind" : (motions ? "oot-bend(no-motions)" : "oot-bend")));
    NoteSlot(slot, state);
    float offsets[3] = { 0, 0, 0 };
    if (animated) {
        const SlotAnim& a = sAnims[slot];
        for (int i = 0; i < 3; i++) {
            offsets[i] = a.off[i];
        }
        char extra[160];
        std::snprintf(extra, sizeof(extra), " | code %d pattern %d step %d anim %d (%s), yaw %.0f pitch %.0f roll %.0f",
                      a.code, a.pattern, a.step, a.anim, a.why.c_str(), a.off[0], a.off[1], a.off[2]);
        sStatus += extra;
    }

    // Centre the fish on the actor: model space p = World * local + offset; draw p - center.
    float pre[3] = { m->offset[0] - m->center[0], m->offset[1] - m->center[1], m->offset[2] - m->center[2] };
    std::vector<RF3SkinnedPoint> points;
    int32_t mouth = m->extra.value("mouth_bone", -1);
    if (mouthOut != nullptr && mouth >= 0) {
        // The snout: locator 0's origin, as OoT's limb 13 + sFishMouthOffset gives fishMouthPos.
        points.push_back({ mouth, { 0.0f, 0.0f, 0.0f }, mouthOut });
    }
    // Step 27c: the 3 mouth points RF3 hooks the lure at (locators 0-2, RF3-MECHANICS 6.6), in world space.
    HookInfo& hook = sHookInfo[slot];
    for (int32_t k = 0; k < 3; k++) {
        hook.have[k] = false;
        if (m->extra.contains("locators") && m->extra["locators"].contains(std::to_string(k))) {
            hook.have[k] = true;
            points.push_back({ m->extra["locators"][std::to_string(k)].get<int32_t>(), { 0.0f, 0.0f, 0.0f },
                               hook.point[k] });
        }
    }
    // The pattern's course/bank offsets turn the drawn fish (step 26e-2). This frame is y-up, front +z: the fish's
    // right is -x, so RF3's "right" (+yaw) is a turn about -y; RF3's "down" (+pitch) tips the front toward -y, which
    // is +x; the bank rolls about the length [sign not known: a mirror only].
    bool turned = offsets[0] != 0.0f || offsets[1] != 0.0f || offsets[2] != 0.0f;
    if (turned) {
        const float d2r = static_cast<float>(M_PI) / 180.0f;
        Matrix_Push();
        Matrix_RotateY(-offsets[0] * d2r, MTXMODE_APPLY);
        Matrix_RotateX(offsets[1] * d2r, MTXMODE_APPLY);
        Matrix_RotateZ(offsets[2] * d2r, MTXMODE_APPLY);
    }
    {
        // Step 27c: the fish's drawn frame (after the pattern's turns, before the model's scale), for the lure.
        MtxF f;
        Matrix_Get(&f);
        for (int32_t c = 0; c < 3; c++) {
            float len = std::sqrt(f.mf[c][0] * f.mf[c][0] + f.mf[c][1] * f.mf[c][1] + f.mf[c][2] * f.mf[c][2]);
            for (int32_t r = 0; r < 3; r++) {
                hook.rot[c * 3 + r] = (len > 1e-6f) ? f.mf[c][r] / len : (c == r ? 1.0f : 0.0f);
            }
        }
        hook.frame = in->frame;
    }
    RF3Skinned_Draw(play, *m, rot, pos, scale, 0, pre, points);
    if (turned) {
        Matrix_Pop();
    }
    // (27c fix, 2026-10-12: mouthOut stays the snout. fishMouthPos drives OoT's line end and pulls the hooked fish
    // toward the line, so moving it to the lure's tie made the fish jump forward. The tie is used only to draw.)
    return 1;
}

extern "C" int32_t RF3FishModel_HookInfo(int32_t slot, int32_t point, float* pos, float* rot, uint32_t* frame) {
    auto it = sHookInfo.find(slot);
    if (it == sHookInfo.end() || point < 0 || point > 2 || !it->second.have[point]) {
        return 0;
    }
    for (int32_t i = 0; i < 3; i++) {
        pos[i] = it->second.point[point][i];
    }
    for (int32_t i = 0; i < 9; i++) {
        rot[i] = it->second.rot[i];
    }
    *frame = it->second.frame;
    return 1;
}

extern "C" void RF3FishModel_SetLineTie(int32_t slot, const float* tie, uint32_t frame) {
    HookInfo& h = sHookInfo[slot];
    if (tie == nullptr) {
        h.tieSet = false;
        return;
    }
    h.tieSet = true;
    h.tieFrame = frame;
    for (int32_t i = 0; i < 3; i++) {
        h.tie[i] = tie[i];
    }
}

extern "C" int32_t RF3FishModel_PlaysMotions(int32_t slot) {
    auto it = sAnims.find(slot);
    return (it != sAnims.end() && it->second.animated) ? 1 : 0;
}

extern "C" int32_t RF3FishModel_MoveInfo(int32_t record, float* speedMps, float* rampSeconds, float* heightPerMetre) {
    LoadIndex();
    auto it = sRecordMove.find(record);
    if (it == sRecordMove.end() || it->second[0] <= 0.0f) {
        return 0;
    }
    *speedMps = it->second[0];
    *rampSeconds = std::max(0.05f, it->second[1]);
    *heightPerMetre = 0.3f; // [assumed] until the model is loaded
    std::string why;
    rf3model::SkinnedModel* m = GetForRecord(record, why);
    if (m != nullptr && m->ok && m->sizeM[2] > 0.01f) {
        *heightPerMetre = m->sizeM[1] / m->sizeM[2]; // RF3's +0xDB4 / +0xDB8 (0x00195DE0)
    }
    return 1;
}

extern "C" const char* RF3FishModel_Status(void) {
    return sStatus.c_str();
}
