// Reel Fishing III mod (step 61, docs/DESIGN.md "Step 61 design", user's choices C1-C7 all A): the co-op fishing
// view. z_fishing.c reports the rod, lure, line and hooked fish as it draws them; after each frame's draw
// (OnGameFrameUpdate runs after GameState_Draw) the parts drawn that frame become one view, turned into the JSON that
// 61c will send as RF3_FISH_VIEW. Choices used here:
// - C1 A: a view whenever the rod is drawn (idle, cast, retrieve, fight, held-up catch);
// - C2 A: only region fish are in it (a hooked pond fish is logged, not captured);
// - C3 A: the fish's draw inputs (record, length, place, turns, bends, speed, motion clock, fight mode), not its bones;
// - C4 A: at most 24 line points (the rod tip, evenly spaced drawn points, the end);
// - C7 A (61b): the mirror: your own view of a few frames ago drawn as a ghost beside you, no network needed.
// Log, don't look: logs/rf3_fishview.txt gets a line per second while fishing, a line per state change, a summary
// when the view ends (frames, JSON bytes), and with the mirror on a line per second of what the ghost drew.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

#include <libultraship/bridge/consolevariablebridge.h>
#include <nlohmann/json.hpp>
#include <ship/Context.h>

#include "soh/cvar_prefixes.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Network/Anchor/Anchor.h" // step 61c: RF3_FISH_VIEW
#include "soh/ShipInit.hpp"
#include "RF3Bridge.h"
#include "RF3FishMove.h" // the fight's turn motion code
#include "RF3FishView.h"
#include "RF3LureModel.h"

extern "C" {
#include "z64.h"
#include "macros.h"
extern PlayState* gPlayState;
}

namespace {

constexpr int kLinePoints = RF3_FISHVIEW_LINE_POINTS;
constexpr uint32_t kFishHoldTicks = 3; // 61a log: one landing frame had no fish draw; keep the last fish this long
constexpr int kMirrorFrames = 16;     // the delay buffer (20 Hz game frames: 10 = 500 ms)

struct FishView {
    RF3FishGhost g = {};
    int32_t record = -1;
    float mm = 0.0f, ootLength = 0.0f;
    int32_t fishState = 0;
    int32_t localSlot = -1; // the angler's own region slot of the fish
    int32_t fightMode = -1;
    bool pondFish = false; // a hooked fish that isn't a region fish (not captured)
};

// Parts reported during the current frame's draw, stamped with the frame they belong to.
uint32_t sTick = 1;
FishView sParts;
uint32_t sRodTick = 0, sLureTick = 0, sLineTick = 0, sFishTick = 0, sPondTick = 0;

// The published view and its statistics.
bool sActive = false;
FishView sView;
uint32_t sFrames = 0;
size_t sBytesMax = 0;
uint64_t sBytesSum = 0;
size_t sBytesLast = 0;
std::string sLastKey;
std::string sStatus = "no view";

// 61b: the mirror's delay buffer (views with their hand matrix) and its ghost.
struct MirrorEntry {
    bool valid = false;
    FishView v;
};
MirrorEntry sMirror[kMirrorFrames];
uint32_t sMirrorHead = 0; // next entry to write
FishView sMirrorGhost;        // the view the mirror ghost was last drawn from (its fish info)
bool sMirrorHasGhost = false;
uint32_t sMirrorDrawn = 0;

// 61c: other players' views (RF3_FISH_VIEW), by Anchor client id, each with its own ghost fish slot (65 on).
constexpr uint32_t kRemoteStaleTicks = 20; // a view not renewed for 1 s is dropped [assumed timeout]
struct Remote {
    FishView v;
    bool valid = false;
    uint32_t tick = 0; // sTick when it arrived
    int32_t scene = -1;
    int32_t slot = -1;
    uint32_t received = 0;
    uint32_t drawn = 0;
};
std::map<uint32_t, Remote> sRemotes;
uint32_t sSendFrames = 0;

// The view behind a ghost fish slot: the mirror's (64) or another player's.
const FishView* GhostView(int32_t slot) {
    if (slot == RF3_FISHVIEW_GHOST_SLOT) {
        return sMirrorHasGhost ? &sMirrorGhost : nullptr;
    }
    for (const auto& [id, r] : sRemotes) {
        if (r.valid && (r.slot == slot)) {
            return &r.v;
        }
    }
    return nullptr;
}

// 61c: sends this frame's view (or, with nullptr, the end of it) to the players in this scene. Returns how many.
uint32_t SendRemote(const nlohmann::json* view) {
    Anchor* anchor = Anchor::Instance;
    if ((anchor == nullptr) || !anchor->isConnected) {
        return 0;
    }
    return anchor->SendPacket_RF3FishView(view);
}

float R1(float v) {
    return std::round(v * 10.0f) / 10.0f;
}
float R3(float v) {
    return std::round(v * 1000.0f) / 1000.0f;
}

void Log(const std::string& line) {
    std::string dir = Ship::Context::GetPathRelativeToAppDirectory("logs");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(dir + "/rf3_fishview.txt", std::ios::app);
    char stamp[32];
    std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    out << stamp << ' ' << line << '\n';
}

// The JSON 61c sends (short keys; positions to 0.1 unit, bends to 0.001).
nlohmann::json ToJson(const FishView& v) {
    const RF3FishGhost& g = v.g;
    nlohmann::json j;
    j["cs"] = g.castState;
    j["age"] = g.linkAge;
    j["rod"] = { R3(g.bendY), R3(g.bendX), R3(g.rotX), R3(g.handZ), g.rf3Rod };
    j["tk"] = { g.tackle.box, g.tackle.rod, g.tackle.lure, g.tackle.floatIdx, g.tackle.bait, g.tackle.size,
                g.tackle.depth };
    if (g.hasLure) {
        j["lure"] = { R1(g.lurePos[0]), R1(g.lurePos[1]), R1(g.lurePos[2]), R3(g.lureRotY), R3(g.lureRotX),
                      R3(g.lureZOffset), g.lureEquipped, g.rf3Lure, g.noLure };
        j["lm"] = { g.lureKind,   g.lureIndex,       g.lureSkinned,     g.lureMotion,  R3(g.lureTime),
                    g.lurePrevMotion, R3(g.lurePrevTime), R3(g.lureBlend), g.lureHanging, g.lureHookPoint };
    }
    // 61c (61a log: the line points were most of the ~1 KB): the rod tip to 0.1, then each point as whole units from
    // the tip.
    nlohmann::json line = nlohmann::json::array();
    for (int i = 0; i < g.lineCount; i++) {
        for (int c = 0; c < 3; c++) {
            if (i == 0) {
                line.push_back(R1(g.line[0][c]));
            } else {
                line.push_back(static_cast<int32_t>(std::lround(g.line[i][c] - g.line[0][c])));
            }
        }
    }
    j["line"] = line;
    j["fp"] = g.floatPoint;
    if (g.hasFish) {
        j["fish"] = { v.record,      R1(v.mm),          R1(v.ootLength), R1(g.fishPos[0]), R1(g.fishPos[1]),
                      R1(g.fishPos[2]), g.fishRot[0],    g.fishRot[1],    g.fishRot[2],     g.yaw,
                      g.bendA,       g.bendB,           R3(g.speedXZ),   v.fishState,      g.frame,
                      v.fightMode,   g.motionCode };
    }
    return j;
}

// 61c: the receiver's side of ToJson (the hand matrix isn't sent: the other player's dummy gives it). Returns false
// for a view it can't read.
template <typename T> T At(const nlohmann::json& a, size_t i, T fallback) {
    return (a.is_array() && i < a.size() && a[i].is_number()) ? a[i].get<T>() : fallback;
}

bool FromJson(const nlohmann::json& j, FishView& v) {
    if (!j.is_object() || !j.contains("cs") || !j.contains("rod") || !j.contains("tk")) {
        return false;
    }
    v = FishView{};
    RF3FishGhost& g = v.g;
    g.castState = j.value("cs", 0);
    g.linkAge = j.value("age", 0);
    const nlohmann::json& rod = j["rod"];
    g.bendY = At(rod, 0, 0.0f);
    g.bendX = At(rod, 1, 0.0f);
    g.rotX = At(rod, 2, 0.0f);
    g.handZ = At(rod, 3, 0.0f);
    g.rf3Rod = At(rod, 4, 0);
    const nlohmann::json& tk = j["tk"];
    g.tackle.box = At(tk, 0, -1);
    g.tackle.rod = At(tk, 1, -1);
    g.tackle.lure = At(tk, 2, -1);
    g.tackle.floatIdx = At(tk, 3, -1);
    g.tackle.bait = At(tk, 4, -1);
    g.tackle.size = At(tk, 5, 0);
    g.tackle.depth = At(tk, 6, 0);
    g.hasLure = j.contains("lure");
    if (g.hasLure) {
        const nlohmann::json& l = j["lure"];
        for (int c = 0; c < 3; c++) {
            g.lurePos[c] = At(l, c, 0.0f);
        }
        g.lureRotY = At(l, 3, 0.0f);
        g.lureRotX = At(l, 4, 0.0f);
        g.lureZOffset = At(l, 5, 0.0f);
        g.lureEquipped = At(l, 6, 0);
        g.rf3Lure = At(l, 7, 0);
        g.noLure = At(l, 8, 0);
        const nlohmann::json lm = j.value("lm", nlohmann::json::array());
        g.lureKind = At(lm, 0, -1);
        g.lureIndex = At(lm, 1, -1);
        g.lureSkinned = At(lm, 2, 0);
        g.lureMotion = At(lm, 3, -1);
        g.lureTime = At(lm, 4, 0.0f);
        g.lurePrevMotion = At(lm, 5, -1);
        g.lurePrevTime = At(lm, 6, 0.0f);
        g.lureBlend = At(lm, 7, 1.0f);
        g.lureHanging = At(lm, 8, 0);
        g.lureHookPoint = At(lm, 9, -1);
    }
    const nlohmann::json line = j.value("line", nlohmann::json::array());
    g.lineCount = std::min(static_cast<int>(line.size() / 3), kLinePoints);
    for (int i = 0; i < g.lineCount; i++) {
        for (int c = 0; c < 3; c++) {
            const float x = At(line, static_cast<size_t>(i * 3 + c), 0.0f);
            g.line[i][c] = (i == 0) ? x : g.line[0][c] + x;
        }
    }
    g.floatPoint = j.value("fp", -1);
    g.hasFish = j.contains("fish");
    if (g.hasFish) {
        const nlohmann::json& f = j["fish"];
        v.record = At(f, 0, -1);
        v.mm = At(f, 1, 0.0f);
        v.ootLength = At(f, 2, 0.0f);
        for (int c = 0; c < 3; c++) {
            g.fishPos[c] = At(f, 3 + c, 0.0f);
            g.fishRot[c] = static_cast<int16_t>(At(f, 6 + c, 0));
        }
        g.yaw = static_cast<int16_t>(At(f, 9, 0));
        g.bendA = static_cast<int16_t>(At(f, 10, 0));
        g.bendB = static_cast<int16_t>(At(f, 11, 0));
        g.speedXZ = At(f, 12, 0.0f);
        v.fishState = At(f, 13, 0);
        g.frame = At<uint32_t>(f, 14, 0);
        v.fightMode = At(f, 15, -1);
        g.motionCode = At(f, 16, -1);
        g.hasFish = v.record >= 0 && v.mm > 0.0f;
    }
    return true;
}

// What a state change is: the cast state, the tackle, and whether a fish (and which) is on.
std::string KeyOf(const FishView& v) {
    char key[160];
    std::snprintf(key, sizeof(key), "cs=%d box=%d lure=%d lureType=%d rf3Lure=%d fish=%d/%d pond=%d", v.g.castState,
                  v.g.tackle.box, v.g.tackle.lure, v.g.lureEquipped, v.g.rf3Lure, v.g.hasFish ? v.record : -1,
                  v.g.hasFish ? v.fishState : -1, v.pondFish ? 1 : 0);
    return key;
}

void Describe(const char* what, const FishView& v, size_t bytes) {
    const RF3FishGhost& g = v.g;
    const int n = g.lineCount;
    char fish[96] = "fish=none";
    if (g.hasFish) {
        std::snprintf(fish, sizeof(fish), "fishAt=%.1f,%.1f,%.1f mm=%.0f fight=%d", g.fishPos[0], g.fishPos[1],
                      g.fishPos[2], v.mm, v.fightMode);
    }
    char line[512];
    std::snprintf(line, sizeof(line),
                  "%s %s rod=%.3f/%.3f/%.3f/%.3f rf3Rod=%d age=%d lure=%s%.1f,%.1f,%.1f line=%d float=%d "
                  "tip=%.1f,%.1f,%.1f end=%.1f,%.1f,%.1f %s bytes=%zu",
                  what, KeyOf(v).c_str(), g.bendY, g.bendX, g.rotX, g.handZ, g.rf3Rod, g.linkAge,
                  g.hasLure ? "" : "none ", g.lurePos[0], g.lurePos[1], g.lurePos[2], n, g.floatPoint,
                  n > 0 ? g.line[0][0] : 0.0f, n > 0 ? g.line[0][1] : 0.0f, n > 0 ? g.line[0][2] : 0.0f,
                  n > 0 ? g.line[n - 1][0] : 0.0f, n > 0 ? g.line[n - 1][1] : 0.0f, n > 0 ? g.line[n - 1][2] : 0.0f,
                  fish, bytes);
    Log(line);
}

void PushMirror(const FishView* v) {
    MirrorEntry& e = sMirror[sMirrorHead % kMirrorFrames];
    e.valid = v != nullptr;
    if (v != nullptr) {
        e.v = *v;
    }
    sMirrorHead++;
}

// 61c: other players' views not renewed for a second are dropped (they left, or their packets stopped).
void DropStaleRemotes(uint32_t tick) {
    for (auto& [id, r] : sRemotes) {
        if (r.valid && (tick - r.tick > kRemoteStaleTicks)) {
            r.valid = false;
            char line[128];
            std::snprintf(line, sizeof(line), "fish-view-remote stale client=%u slot=%d received=%u", id, r.slot,
                          r.received);
            Log(line);
        }
    }
}

// After the frame's draw: the parts stamped with this frame make the view.
void OnFrame() {
    const uint32_t tick = sTick++;
    DropStaleRemotes(tick);
    if (!RF3_IsEnabled() || (sRodTick != tick) || (gPlayState == nullptr)) {
        if (sActive) {
            char line[160];
            std::snprintf(line, sizeof(line), "fish-view stop frames=%u bytesMax=%zu bytesAvg=%.0f", sFrames, sBytesMax,
                          sFrames ? static_cast<double>(sBytesSum) / sFrames : 0.0);
            Log(line);
            const uint32_t peers = SendRemote(nullptr); // 61c: the others drop our view now
            if (peers > 0) {
                Log("fish-view-send stop peers=" + std::to_string(peers));
            }
            sActive = false;
            sLastKey.clear();
            sStatus = "no view (rod away)";
        }
        PushMirror(nullptr);
        return;
    }
    FishView v = sParts;
    RF3FishGhost& g = v.g;
    g.hasLure = sLureTick == tick;
    if (sLineTick != tick) {
        g.lineCount = 0;
        g.floatPoint = -1;
    }
    // The fish: drawn this frame, or (61a: one landing frame had none) drawn within the last few frames while hooked.
    const bool hooked = (g.castState == 4) || (g.castState == 5);
    g.hasFish = (sFishTick == tick) || (hooked && sView.g.hasFish && (tick - sFishTick) <= kFishHoldTicks);
    if (g.hasFish && (sFishTick != tick)) {
        v.record = sView.record; // the held fish
        v.mm = sView.mm;
        v.ootLength = sView.ootLength;
        v.fishState = sView.fishState;
        v.localSlot = sView.localSlot;
        std::memcpy(g.fishPos, sView.g.fishPos, sizeof(g.fishPos));
        std::memcpy(g.fishRot, sView.g.fishRot, sizeof(g.fishRot));
        g.yaw = sView.g.yaw;
        g.bendA = sView.g.bendA;
        g.bendB = sView.g.bendB;
        g.speedXZ = sView.g.speedXZ;
        g.frame = sView.g.frame;
    }
    v.fightMode = -1;
    g.motionCode = -1;
    if (g.hasFish) {
        RF3FightStatus f;
        RF3Fight_GetStatus(&f);
        v.fightMode = f.active ? f.mode : 0;
        // User (61b): the mirror's fish didn't animate. Its fight motions also need the movement's turn code, which
        // RF3FishMove keeps per local slot (the ghost's slot had none: plain swims, no turns).
        g.motionCode = RF3FishMove_MotionCode(v.localSlot);
    }
    v.pondFish = sPondTick == tick;
    std::memcpy(g.hand, &GET_PLAYER(gPlayState)->mf_9E0, sizeof(g.hand));
    sView = v;
    const nlohmann::json j = ToJson(v);
    const size_t bytes = j.dump().size();
    sBytesLast = bytes;
    const uint32_t peers = SendRemote(&j); // 61c: to the players in this scene (choice C5 A: every game frame)
    if ((peers > 0) && ((sSendFrames++ % 20) == 0)) {
        Log("fish-view-send peers=" + std::to_string(peers) + " bytes=" + std::to_string(bytes) + " " + KeyOf(v));
    }
    if (!sActive) {
        sActive = true;
        sFrames = 0;
        sBytesMax = 0;
        sBytesSum = 0;
        Describe("fish-view start", v, bytes);
    }
    sFrames++;
    sBytesMax = std::max(sBytesMax, bytes);
    sBytesSum += bytes;
    const std::string key = KeyOf(v);
    if (key != sLastKey) {
        if (!sLastKey.empty()) {
            Describe("fish-view change", v, bytes);
        }
        sLastKey = key;
    } else if ((sFrames % 20) == 0) {
        Describe("fish-view", v, bytes); // once a second at 20 Hz
    }
    sStatus = key + " bytes=" + std::to_string(bytes);
    PushMirror(&v);
}

void RegisterRF3FishView() {
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameFrameUpdate>(OnFrame);
}

} // namespace

static RegisterShipInitFunc initRF3FishView(RegisterRF3FishView);

extern "C" void RF3FishView_SetRod(float bendY, float bendX, float rotX, float handZ, int32_t castState,
                                   int32_t linkAge, int32_t rf3RodDrawn) {
    RF3FishGhost& g = sParts.g;
    g.bendY = bendY;
    g.bendX = bendX;
    g.rotX = rotX;
    g.handZ = handZ;
    g.castState = castState;
    g.linkAge = linkAge;
    g.rf3Rod = rf3RodDrawn;
    RF3Tackle_Get(&g.tackle);
    sRodTick = sTick;
}

extern "C" void RF3FishView_SetLure(const float* pos, float rotY, float rotX, float zOffset, int32_t lureEquipped,
                                    int32_t rf3Drawn, int32_t noLure) {
    RF3FishGhost& g = sParts.g;
    std::copy(pos, pos + 3, g.lurePos);
    g.lureRotY = rotY;
    g.lureRotX = rotX;
    g.lureZOffset = zOffset;
    g.lureEquipped = lureEquipped;
    g.rf3Lure = rf3Drawn;
    g.noLure = noLure;
    RF3LureModel_GetDrawn(&g); // the item drawn and its RF3 motion state (user: the mirror's lure didn't animate)
    sLureTick = sTick;
}

extern "C" void RF3FishView_SetLine(const float* tip, const float* points, int32_t count, const float* end,
                                    int32_t floatAt) {
    RF3FishGhost& g = sParts.g;
    int n = 0;
    std::copy(tip, tip + 3, g.line[n++]);
    count = std::max(count, 0);
    const int inner = std::min(count, kLinePoints - 2); // evenly spaced, first and last drawn point included
    g.floatPoint = -1;
    for (int k = 0; k < inner; k++) {
        const int i = (inner == 1) ? 0 : static_cast<int>(std::lround(k * (count - 1) / static_cast<double>(inner - 1)));
        if (i == floatAt) {
            g.floatPoint = n;
        }
        std::copy(points + 3 * i, points + 3 * i + 3, g.line[n++]);
    }
    std::copy(end, end + 3, g.line[n++]);
    g.lineCount = n;
    sLineTick = sTick;
}

extern "C" void RF3FishView_SetFish(int32_t slot, const float* pos, const int16_t* rot, int16_t yaw, int16_t bendA,
                                    int16_t bendB, float speedXZ, int32_t fishState, uint32_t frame) {
    RF3RegionFishInfo info;
    if (!RF3RegionFish_Get(slot, &info)) {
        return;
    }
    RF3FishGhost& g = sParts.g;
    sParts.localSlot = slot;
    sParts.record = info.record;
    sParts.mm = info.mm;
    sParts.ootLength = info.ootLength;
    std::copy(pos, pos + 3, g.fishPos);
    std::copy(rot, rot + 3, g.fishRot);
    g.yaw = yaw;
    g.bendA = bendA;
    g.bendB = bendB;
    g.speedXZ = speedXZ;
    g.frame = frame;
    sParts.fishState = fishState;
    sFishTick = sTick;
}

extern "C" void RF3FishView_SetPondFish(int32_t fishState) {
    sParts.fishState = fishState;
    sPondTick = sTick;
}

// 61b (choice C7 A): the mirror ghost. Your view of `delay` frames ago (the buffer holds the frames already drawn, so
// the newest entry is last frame's), shifted `offset` units to Link's side; its rod in that frame's hand matrix
// shifted the same way. Settings: debug window "Co-op fishing view".
extern "C" void RF3FishView_DrawMirror(PlayState* play) {
    if (!RF3_IsEnabled() || !CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.FishViewMirror"), 0)) {
        sMirrorHasGhost = false;
        return;
    }
    const int delayMs = std::clamp(CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.FishViewMirrorDelay"), 0), 0, 500);
    const uint32_t back = 1 + static_cast<uint32_t>(std::lround(delayMs / 50.0)); // 1 = last frame's
    if (sMirrorHead < back) {
        return;
    }
    const MirrorEntry& e = sMirror[(sMirrorHead - back) % kMirrorFrames];
    if (!e.valid) {
        sMirrorHasGhost = false;
        return;
    }
    // 61c: through the packet's JSON, both ways, as another player gets it (the hand matrix isn't sent).
    FishView v;
    if (!FromJson(ToJson(e.v), v)) {
        sMirrorHasGhost = false;
        return;
    }
    std::memcpy(v.g.hand, e.v.g.hand, sizeof(v.g.hand));
    RF3FishGhost& g = v.g;
    // Link's side: perpendicular to his facing (sin, cos) in the ground plane.
    const float offset = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.FishViewMirrorOffset"), 150.0f);
    const float yaw = GET_PLAYER(play)->actor.shape.rot.y * static_cast<float>(M_PI) / 32768.0f;
    const float d[3] = { std::cos(yaw) * offset, 0.0f, -std::sin(yaw) * offset };
    for (int i = 0; i < 3; i++) {
        g.hand[3][i] += d[i]; // the matrix's translation (MtxF xw, yw, zw)
        g.lurePos[i] += d[i];
        g.fishPos[i] += d[i];
        for (int k = 0; k < g.lineCount; k++) {
            g.line[k][i] += d[i];
        }
    }
    g.slot = RF3_FISHVIEW_GHOST_SLOT;
    sMirrorGhost = v;
    sMirrorHasGhost = true;
    Fishing_RF3DrawGhost(play, &g);
    if ((sMirrorDrawn++ % 20) == 0) { // once a second at 20 Hz
        char line[384];
        std::snprintf(line, sizeof(line),
                      "fish-ghost mirror delay=%dms back=%u offset=%.0f cs=%d rf3Rod=%d lure=%d rf3Lure=%d noLure=%d "
                      "item=%d/%d skinned=%d motion=%d t=%.2f hanging=%d point=%d line=%d float=%d fish=%d record=%d "
                      "fight=%d code=%d",
                      delayMs, back, offset, g.castState, g.rf3Rod, g.hasLure, g.rf3Lure, g.noLure, g.lureKind,
                      g.lureIndex, g.lureSkinned, g.lureMotion, g.lureTime, g.lureHanging, g.lureHookPoint,
                      g.lineCount, g.floatPoint, g.hasFish, g.hasFish ? v.record : -1, v.fightMode, g.motionCode);
        Log(line);
    }
}

extern "C" int32_t RF3FishView_GhostFishInfo(int32_t slot, RF3RegionFishInfo* out) {
    const FishView* v = GhostView(slot);
    if ((v == nullptr) || !v->g.hasFish) {
        return 0;
    }
    *out = {};
    out->record = v->record;
    out->mm = v->mm;
    out->ootLength = v->ootLength;
    return 1;
}

extern "C" int32_t RF3FishView_GhostFightMode(int32_t slot, int32_t* mode) {
    const FishView* v = GhostView(slot);
    if (v == nullptr) {
        return 0;
    }
    *mode = v->fightMode;
    return 1;
}

extern "C" int32_t RF3FishView_GhostMotionCode(int32_t slot, int32_t* code) {
    const FishView* v = GhostView(slot);
    if (v == nullptr) {
        return 0;
    }
    *code = v->g.motionCode;
    return 1;
}

// 61c: another player's RF3_FISH_VIEW (Packets/RF3FishView.cpp). A null view ends theirs.
void RF3FishView_Receive(uint32_t clientId, int32_t sceneNum, const nlohmann::json& view) {
    Remote& r = sRemotes[clientId];
    char line[200];
    if (view.is_null()) {
        if (r.valid) {
            std::snprintf(line, sizeof(line), "fish-view-remote stop client=%u slot=%d received=%u", clientId, r.slot,
                          r.received);
            Log(line);
        }
        r.valid = false;
        return;
    }
    FishView v;
    if (!FromJson(view, v)) {
        if (r.received == 0) {
            Log("fish-view-remote unreadable client=" + std::to_string(clientId));
        }
        return;
    }
    if (r.slot < 0) { // a ghost fish slot of its own, kept for this player
        for (int32_t s = RF3_FISHVIEW_GHOST_SLOT + 1; s < RF3_FISHVIEW_GHOST_SLOT + RF3_FISHVIEW_GHOST_SLOTS; s++) {
            bool taken = false;
            for (const auto& [id, o] : sRemotes) {
                taken = taken || (o.slot == s);
            }
            if (!taken) {
                r.slot = s;
                break;
            }
        }
        if (r.slot < 0) {
            Log("fish-view-remote full client=" + std::to_string(clientId));
            return;
        }
    }
    const bool started = !r.valid;
    v.g.slot = r.slot;
    std::memcpy(v.g.hand, r.v.g.hand, sizeof(v.g.hand)); // the dummy's hand is set when it is drawn
    r.v = v;
    r.valid = true;
    r.tick = sTick;
    r.scene = sceneNum;
    r.received++;
    if (started) {
        std::snprintf(line, sizeof(line), "fish-view-remote start client=%u slot=%d scene=%d %s bytes=%zu", clientId,
                      r.slot, sceneNum, KeyOf(v).c_str(), view.dump().size());
        Log(line);
    }
}

// 61c: from DummyPlayer_Draw, right after Player_Draw set the dummy's hand matrix (mf_9E0).
extern "C" void RF3FishView_DrawRemote(PlayState* play, uint32_t clientId, const float* hand) {
    auto it = sRemotes.find(clientId);
    if (!RF3_IsEnabled() || (it == sRemotes.end()) || !it->second.valid || (it->second.scene != play->sceneNum)) {
        return;
    }
    Remote& r = it->second;
    std::memcpy(r.v.g.hand, hand, sizeof(r.v.g.hand));
    Fishing_RF3DrawGhost(play, &r.v.g);
    if ((r.drawn++ % 20) == 0) { // once a second at 20 Hz
        const RF3FishGhost& g = r.v.g;
        char line[320];
        std::snprintf(line, sizeof(line),
                      "fish-view-remote draw client=%u slot=%d age=%u cs=%d rf3Rod=%d item=%d/%d skinned=%d motion=%d "
                      "hanging=%d line=%d float=%d fish=%d record=%d fight=%d code=%d hand=%.0f,%.0f,%.0f "
                      "tip=%.0f,%.0f,%.0f",
                      clientId, r.slot, sTick - r.tick, g.castState, g.rf3Rod, g.lureKind, g.lureIndex, g.lureSkinned,
                      g.lureMotion, g.lureHanging, g.lineCount, g.floatPoint, g.hasFish, g.hasFish ? r.v.record : -1,
                      r.v.fightMode, g.motionCode, g.hand[3][0], g.hand[3][1], g.hand[3][2],
                      g.lineCount > 0 ? g.line[0][0] : 0.0f, g.lineCount > 0 ? g.line[0][1] : 0.0f,
                      g.lineCount > 0 ? g.line[0][2] : 0.0f);
        Log(line);
    }
}

extern "C" const char* RF3FishView_Status(void) {
    return sStatus.c_str();
}
