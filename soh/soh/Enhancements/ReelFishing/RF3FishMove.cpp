// Reel Fishing III mod (step 26e-3): RF3's own movement for a hooked region fish on RF3's model and motions. See
// RF3FishMove.h, docs/DESIGN.md "26e plan" and RF3-MECHANICS 6.3 (read from RF3's 0x0019DE60, 0x0019B7A0,
// 0x0019C220, the state entries/updates). Runs RF3's 1/60 s ticks, `substeps` per game frame.
//
// RESIST, STRUGGLE, AWAY: the fight target, a point on the circle around the angler at the fight's distance, turned
//   to the fish's side (entry: RESIST 5 deg, STRUGGLE/AWAY 10 deg). Step 30a: the side is RF3's (0x0019DE60 with
//   0x0019EE70): from the fish's heading against the angler -> fish line, sometimes flipped near the line; picked
//   again when reached (within half a body length) at the state's every-frame angle (RESIST 5, STRUGGLE 5, AWAY 10);
//   heading more than 30 deg off on the wrong side picks again at 10 deg. A target that isn't open water switches
//   the side (20 deg) and turns 4x faster. Fight mode: the species' full speed, ramped by sine; turning
//   at 2 x RF3's base 3 deg per tick. STRUGGLE's target depth alternates every 0.25 s between its entry depth and
//   2 body heights deeper. (AWAY's wider circle is in the fight's distance already: the sim adds 2 lengths.)
// STIFFEN (step 30b-1, 0x001A4650): turns to the fight centre's bearing +-90 deg, whichever is nearer its heading,
//   slowly (0.2 x speed); from AWAY it asks for RF3's turn motion (25/26 or 65/66), else 28.
// Step 30a-2: the side choice and the 30-degree check measure the line against the fight centre's bearing (RF3's
//   fish +0x110, set at the hook by 0x0019A3D0), not the fish's heading: RF3 keeps a fight around that bearing.
// STOP: glides to a point 2 body lengths ahead at 0.2 x speed.
// FOLLOW: faces the angler and is drawn in as the fight's distance shrinks (RF3: the line drags it). Step 30b-2
//   (0x001A40E0, 0x001A4340): facing 60 deg or more away on entry it first turns on the spot (65/66 90 deg, 63/64
//   180 deg; the heading turns by the pattern's yaw over its length, standing still); then it turns toward the
//   angler with held turns 25/26 while the turn is capped, else towed (60).
// Obstacle probes (step 26f-1, RF3-MECHANICS 6.4): every state but FOLLOW probes ahead toward its target and slides
//   the target along what it hits (up to 2 times); blocked fight states re-pick on the other side.
// Not used [RF3-MECHANICS 6.3]: AWAY's rod-side aim, the hook dropping when no water is found (the mod keeps the hook
// and logs "blocked"; the user's choice, 2026-10-14).
//
// Log, don't look: logs/rf3_models.txt gets "rf3-move" lines on every mode change, target pick and block.

#include "RF3FishMove.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <string>

#include <libultraship/bridge/consolevariablebridge.h>

#include "soh/cvar_prefixes.h"
#include "RF3Bridge.h"
#include "RF3FishModel.h"
#include "RF3SkinnedModel.h"

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kTick = 1.0f / 60.0f;
constexpr float kBaseTurn = 0.05236f;         // rad per tick (fish +0x29C, 0x00195A70)
constexpr float kStruggleBobSeconds = 0.25f;  // STRUGGLE's depth toggle (0x001A37B0)
constexpr float kSlowShare = 0.2f;            // speed cap outside fight mode (0x0019B7A0)
constexpr float kMaxPitch = kPi / 3.0f;       // [assumed] keep the nose within 60 deg
constexpr float kAheadUnits = 200.0f;         // the point straight ahead handed to OoT's turning
// Step 26e-4 (user, 2026-10-08: "The struggle and away movement speed seems too high"): RF3 fights at the full
// species speed (every state entry restores it, 0x0019A280), but the mod's fights are 2-3 m out where RF3's are
// 10-30 m, so the same speed circles far faster. Scaled by this CVar [user's call; RF3 = 1.0]: 0.5, then 0.2 (user,
// 2026-10-14, with 30b-1b's longer legs).
constexpr float kDefaultSpeedScale = 0.2f;
constexpr float kLogEverySeconds = 1.0f; // at most one repeated log line per event and second
// FOLLOW: at most this far per frame [assumed, not RF3 data]. Reeling closes about 1 unit/frame, so 4 still catches up
// (26e-5's goal), but smoothly. Step 26e-11: was 20, which closed a gap built up at a wall in one 27-unit jump
// (rf3_fishing_20261009_203143.csv, frames 1303 and 1327).
constexpr float kMaxDragUnits = 4.0f;
// Step 30b-1c (user, 2026-10-14: reeling through AWAY, then FOLLOW gave "a quick burst of speed"): FOLLOW drags by
// what the line reeled in this frame, plus this share of the gap left (e.g. from AWAY), at most kCatchUpMax per frame
// [assumed]. Log: gaps of 56-68 units at FOLLOW's entry were closed at 4/frame (~0.8 s).
constexpr float kCatchUpShare = 0.05f;
constexpr float kCatchUpMax = 1.0f;
// Step 30a (0x0019EE70): "on the line" = the heading within 30 deg of straight away from the angler
constexpr float kOnLine = kPi / 6.0f;
// Step 30b-1b (user's choice): a fight target's angle is scaled so its leg is as long as RF3's at this distance
// [assumed: RF3's fights are 10-30 m; the mod's 2-4 m], capped. The flip tests and the 30 deg window keep RF3's angles.
constexpr float kDefaultArcRefM = 15.0f;
constexpr float kMaxTargetAngle = kPi / 3.0f;

struct MoveState {
    int32_t mode = -1;
    uint32_t lastFrame = 0;
    bool started = false;
    int32_t side = 0; // 1 = -angle, 2 = +angle
    float target[3] = { 0, 0, 0 };
    float yaw = 0.0f;   // OoT convention: direction (sin yaw, cos yaw)
    float pitch = 0.0f; // + = nose up
    float speed = 0.0f; // units per tick
    float rampT = 0.0f;
    float turnRate = kBaseTurn;
    float bobDepth[2] = { 0, 0 };
    float bobTimer = 0.0f;
    int32_t bobSide = 0;
    std::string lastEvent;
    float sinceLog = 0.0f;
    int32_t probes = 0;    // step 26f-2: probe runs since the last mode change (shows the probe runs)
    int32_t probeHits = 0; // ... of which deflected or blocked
    std::string lastSideWhy; // step 30a: the rf3-side log throttle
    float sinceSideLog = 0.0f;
    int32_t sidePicks = 0;
    float centerYaw = 0.0f;  // step 30a-2: the bearing from the angler to the fight centre, fixed at the fight's start
    int32_t prevMode = -1;   // step 30b-1: the mode before the current one (STIFFEN's motion depends on AWAY)
    int32_t motionCode = -1; // step 30b-1: the motion code asked of RF3FishModel, -1 = its own choice
    float prevDist = -1.0f;  // step 30b-1c: the fight's distance last frame (FOLLOW drags by its change)
    int32_t awayAim = 0;       // step 30c-1: AWAY's aim mode (RF3 fish +0x210; zeroed on every state entry)
    float spotTurnLeft = 0.0f; // step 30b-2: seconds left of FOLLOW's turn on the spot
    float spotTurnRate = 0.0f; // its heading change, rad per tick (OoT sense, + = left)
};

std::map<int32_t, MoveState> sStates;

float WrapAngle(float a) {
    while (a > kPi) {
        a -= 2.0f * kPi;
    }
    while (a < -kPi) {
        a += 2.0f * kPi;
    }
    return a;
}

void Log(const RF3FishMoveInput& in, MoveState& s, const char* event) {
    // the same event again within a second is skipped (step 26e-4: a blocked fish logged 816 lines)
    if (s.lastEvent == event && s.sinceLog < kLogEverySeconds) {
        return;
    }
    s.lastEvent = event;
    s.sinceLog = 0.0f;
    char line[300];
    std::snprintf(line, sizeof(line),
                  "slot=%d rf3-move mode=%d event=%s side=%d target=(%.0f,%.0f,%.0f) fish=(%.0f,%.0f,%.0f) "
                  "dist=%.0f speed=%.2f/frame turn=%.1fdeg/tick probes=%d hits=%d",
                  in.slot, in.mode, event, s.side, s.target[0], s.target[1], s.target[2], in.fishPos[0],
                  in.fishPos[1], in.fishPos[2], in.distUnits, s.speed * in.substeps, s.turnRate * 180.0f / kPi, s.probes, s.probeHits);
    rf3model::Log(line);
}

float ClampDepth(const RF3FishMoveInput& in, float y, float heightUnits) {
    float maxY = in.waterY - 5.0f;
    float minY = (in.floorY > -30000.0f) ? (in.floorY + heightUnits * 0.5f + 10.0f) : (maxY - 1000.0f);
    if (minY > maxY) {
        return (minY + maxY) * 0.5f; // very shallow: stay midway
    }
    return std::clamp(y, minY, maxY);
}

// Yaw of the angler -> fish line (OoT convention, as s.yaw).
float LineYaw(const RF3FishMoveInput& in, const MoveState& s) {
    float dx = in.fishPos[0] - in.anglerPos[0];
    float dz = in.fishPos[2] - in.anglerPos[2];
    if (dx * dx + dz * dz < 1.0f) {
        return s.yaw;
    }
    return std::atan2(dx, dz);
}

// Step 30a: RF3's side choice (0x0019DE60 with 0x0019EE70, DESIGN "30a rules"). rel = (30a-2) the fight centre's
// bearing against the angler -> fish line, 0 = the fish is on the bearing. On the line (|rel| <= 30 deg) its sign flips: always inside +-angle, with
// chance 3/10 between the angle and 30 deg. rel < 0 -> side 1 (-angle), else side 2. The turn becomes 2 x base.
void ChooseSide(const RF3FishMoveInput& in, MoveState& s, float angleDeg, const char* why) {
    float rel = WrapAngle(s.centerYaw - LineYaw(in, s)); // 30a-2: the centre's bearing against the line
    float a = angleDeg * kPi / 180.0f;
    const char* flip = "no";
    if (std::fabs(rel) <= kOnLine) {
        if (std::fabs(rel) < a) {
            rel = -rel;
            flip = "inside";
        } else if (std::rand() % 10 > 6) {
            rel = -rel;
            flip = "chance";
        }
    }
    s.side = (rel < 0.0f) ? 1 : 2;
    s.turnRate = kBaseTurn * 2.0f;
    // at most one line per reason and second; skipped picks are counted (an oscillation shows as a high count)
    s.sidePicks++;
    if (s.lastSideWhy == why && s.sinceSideLog < kLogEverySeconds) {
        return;
    }
    char line[220];
    const float refUnits =
        CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.FishArcRefM"), kDefaultArcRefM) * RF3_UNITS_PER_METRE;
    const float effDeg = std::min(std::max(angleDeg, kMaxTargetAngle * 180.0f / kPi),
                                  std::max(angleDeg, angleDeg * refUnits / std::max(in.distUnits, 1.0f)));
    std::snprintf(line, sizeof(line),
                  "slot=%d rf3-side why=%s mode=%d rel=%.0fdeg flip=%s side=%d angle=%.0f eff=%.0f dist=%.0f "
                  "picks_since_last_line=%d",
                  in.slot, why, in.mode, WrapAngle(s.centerYaw - LineYaw(in, s)) * 180.0f / kPi, flip, s.side, angleDeg,
                  effDeg, in.distUnits, s.sidePicks);
    rf3model::Log(line);
    s.lastSideWhy = why;
    s.sinceSideLog = 0.0f;
    s.sidePicks = 0;
}

// RF3's fight target (0x0019DE60): the circle around the angler at the fight's distance, turned by +-angle to the
// fish's side. Returns false if neither side is open water.
bool PickFightTarget(const RF3FishMoveInput& in, MoveState& s, float angleDeg, float y, const char* why = "pick") {
    float base = LineYaw(in, s);
    if (s.side == 0) {
        ChooseSide(in, s, angleDeg, why);
    }
    // Step 30b-1b: RF3's angle -> the same leg length as RF3's at the reference distance
    const float refUnits =
        CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.FishArcRefM"), kDefaultArcRefM) * RF3_UNITS_PER_METRE;
    const float rf3Angle = angleDeg * kPi / 180.0f;
    const float eff =
        std::clamp(rf3Angle * refUnits / std::max(in.distUnits, 1.0f), rf3Angle, std::max(rf3Angle, kMaxTargetAngle));
    for (int tries = 0; tries < 2; tries++) {
        float a = base + ((s.side == 1) ? -1.0f : 1.0f) * eff;
        float px = in.anglerPos[0] + std::sin(a) * in.distUnits;
        float pz = in.anglerPos[2] + std::cos(a) * in.distUnits;
        if (in.inWater == nullptr || in.inWater(px, pz)) {
            s.target[0] = px;
            s.target[1] = y;
            s.target[2] = pz;
            return true;
        }
        s.side = (s.side == 1) ? 2 : 1; // blocked: the other side, turning 4x
        s.turnRate = kBaseTurn * 4.0f;
    }
    return false;
}

void SetAhead(const RF3FishMoveInput& in, MoveState& s, float yaw, float lengthUnits) {
    s.target[0] = in.fishPos[0] + std::sin(yaw) * lengthUnits;
    s.target[1] = in.fishPos[1];
    s.target[2] = in.fishPos[2] + std::cos(yaw) * lengthUnits;
}

// Step 26f-1: RF3's obstacle probe and its response (RF3-MECHANICS 6.4: 0x0019A6A0, 0x0019B7A0 lines 136-228). A line
// along the direction to the target, from 0.8 L behind the fish, speed + L + 0.15 m long. On a hit the direction is
// projected onto the hit's plane (RF3's 0x001AE1B0: a slide) and the target moves to fish + new direction x the old
// distance; RF3 deflects at most twice, then moves on. Returns false when blocked (no normal, or head-on).
bool ProbeAndDeflect(const RF3FishMoveInput& in, MoveState& s, float lengthUnits, float heightUnits, float speedFrame,
                     int* deflections) {
    *deflections = 0;
    if (in.probe == nullptr) {
        return true;
    }
    float d[3] = { s.target[0] - in.fishPos[0], s.target[1] - in.fishPos[1], s.target[2] - in.fishPos[2] };
    float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (dist < 1.0f) {
        return true;
    }
    for (float& c : d) {
        c /= dist;
    }
    const float reach = speedFrame + lengthUnits + 0.15f * RF3_UNITS_PER_METRE;
    for (int i = 0; i < 3; i++) {
        float from[3], to[3], n[3] = { 0, 0, 0 };
        for (int k = 0; k < 3; k++) {
            from[k] = in.fishPos[k] - d[k] * 0.8f * lengthUnits;
            to[k] = from[k] + d[k] * reach;
        }
        int32_t hit = in.probe(from, to, n);
        if (hit == 0 || i == 2) {
            return true; // clear, or 2 deflections done: RF3 moves on
        }
        if (hit == 2) {
            return false;
        }
        float dn = d[0] * n[0] + d[1] * n[1] + d[2] * n[2];
        float slide[3] = { d[0] - n[0] * dn, d[1] - n[1] * dn, d[2] - n[2] * dn };
        float len = std::sqrt(slide[0] * slide[0] + slide[1] * slide[1] + slide[2] * slide[2]);
        if (len < 0.001f) {
            return false; // head-on
        }
        for (int k = 0; k < 3; k++) {
            d[k] = slide[k] / len;
            s.target[k] = in.fishPos[k] + d[k] * dist;
        }
        s.target[1] = ClampDepth(in, s.target[1], heightUnits); // RF3: never shallower than half a body + 0.05
        (*deflections)++;
    }
    return true;
}

} // namespace

extern "C" int32_t RF3FishMove_Step(const RF3FishMoveInput* inp, RF3FishMoveOutput* out) {
    const RF3FishMoveInput& in = *inp;
    if (!(in.mode == 7 || in.mode == 8 || in.mode == 9 || in.mode == 10 || in.mode == 11 || in.mode == 12)) {
        return 0;
    }
    float speedMps = 0.0f, rampS = 1.0f, heightPerM = 0.3f;
    if (!RF3FishModel_MoveInfo(in.record, &speedMps, &rampS, &heightPerM)) {
        return 0;
    }
    const float U = RF3_UNITS_PER_METRE;
    const float lengthUnits = in.mm / 1000.0f * U; // fish models are 1 m long: L = mm / 1000 m
    const float heightUnits = lengthUnits * heightPerM;
    const float speedScale = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.FishMoveSpeed"), kDefaultSpeedScale);
    const float fullCap = speedMps * U * kTick * speedScale; // units per tick

    MoveState& s = sStates[in.slot];
    if (!s.started || (in.frame - s.lastFrame) > 3) {
        s = MoveState();
        s.started = true;
        s.yaw = in.yaw * kPi / 32768.0f;
        // Step 30a-2: the fight centre's bearing from the angler, once per fight (RF3: at the hook set, 0x0019A3D0)
        float cx = in.centerPos[0] - in.anglerPos[0];
        float cz = in.centerPos[2] - in.anglerPos[2];
        const bool center = in.hasCenter && (cx * cx + cz * cz) > 1.0f;
        s.centerYaw = center ? std::atan2(cx, cz) : LineYaw(in, s);
        char line[200];
        std::snprintf(line, sizeof(line), "slot=%d rf3-centre bearing=%.0fdeg from=%s centre=(%.0f,%.0f) line=%.0fdeg",
                      in.slot, s.centerYaw * 180.0f / kPi, center ? "region-box" : "line-at-start", in.centerPos[0],
                      in.centerPos[2], LineYaw(in, s) * 180.0f / kPi);
        rf3model::Log(line);
    }
    s.lastFrame = in.frame;
    s.sinceLog += kTick * std::max(1, in.substeps);
    s.sinceSideLog += kTick * std::max(1, in.substeps);

    if (in.mode != s.mode) {
        s.prevMode = s.mode;
        s.mode = in.mode;
        s.motionCode = -1;
        s.awayAim = 0; // RF3's 0x0019A280 zeroes +0x210 on every state entry
        s.rampT = 0.0f;
        s.turnRate = kBaseTurn;
        float y = ClampDepth(in, in.fishPos[1], heightUnits);
        const char* event = "enter";
        switch (in.mode) {
            case 8:  // RESIST
            case 9:  // STRUGGLE
            case 10: // AWAY
                s.side = 0;
                if (in.mode == 9) {
                    s.bobDepth[0] = y;
                    s.bobDepth[1] = ClampDepth(in, y - 2.0f * heightUnits, heightUnits); // 2 heights deeper
                    s.bobTimer = 0.0f;
                    s.bobSide = 0;
                }
                if (!PickFightTarget(in, s, in.mode == 8 ? 5.0f : 10.0f, y, "enter")) {
                    SetAhead(in, s, s.yaw, lengthUnits);
                    event = "enter-blocked";
                }
                break;
            case 12: { // STIFFEN (30b-1, 0x001A4650): the centre bearing +-90 deg, whichever is nearer the heading
                float a = WrapAngle(s.centerYaw - kPi / 2.0f);
                float b = WrapAngle(s.centerYaw + kPi / 2.0f);
                float fa = WrapAngle(a - s.yaw);
                float fb = WrapAngle(b - s.yaw);
                float f = 0.0f; // the turn, OoT sense (+ = left)
                float dir = s.yaw;
                if (std::fabs(fa) < std::fabs(fb)) {
                    dir = a;
                    f = fa;
                } else if (std::fabs(fb) < std::fabs(fa)) {
                    dir = b;
                    f = fb;
                }
                SetAhead(in, s, dir, 2.0f * lengthUnits);
                s.turnRate = kBaseTurn * 2.0f;
                // the motion: from AWAY, RF3's turn by its angle (RF3 sense = -OoT: 26/66 right, 25/65 left; 60 deg
                // splits small and on-the-spot turns); otherwise 28 "Kuh!" [RF3 also plays it 4x faster: not done]
                const float fr = -f;
                const float big = kPi / 3.0f;
                if (s.prevMode == 10) {
                    s.motionCode = (fr > 0.0f) ? ((fr < big) ? 26 : 66) : (fr < 0.0f) ? ((fr > -big) ? 25 : 65) : 2;
                } else {
                    s.motionCode = 28;
                }
                char line[200];
                std::snprintf(line, sizeof(line),
                              "slot=%d rf3-stiffen turn=%.0fdeg (OoT sense, + = left) to=%.0fdeg centre=%.0fdeg "
                              "from_away=%d motion=%d",
                              in.slot, f * 180.0f / kPi, dir * 180.0f / kPi, s.centerYaw * 180.0f / kPi,
                              s.prevMode == 10 ? 1 : 0, s.motionCode);
                rf3model::Log(line);
                break;
            }
            case 7: // STOP: glide 2 lengths ahead
                SetAhead(in, s, s.yaw, 2.0f * lengthUnits);
                break;
            case 11: { // FOLLOW
                s.target[0] = in.anglerPos[0];
                s.target[1] = in.fishPos[1];
                s.target[2] = in.anglerPos[2];
                s.turnRate = kBaseTurn * 2.0f;
                // Step 30b-2 (0x001A40E0): by the angle to the angler, RF3 sense (= -OoT): >= 60 deg a turn on the
                // spot, 90 deg (66 right / 65 left) below 120, else 180 (64 / 63)
                float toAngler = std::atan2(in.anglerPos[0] - in.fishPos[0], in.anglerPos[2] - in.fishPos[2]);
                float fr = -WrapAngle(toAngler - s.yaw);
                const float a60 = kPi / 3.0f;
                const float a120 = 2.0f * kPi / 3.0f;
                int32_t code = -1;
                if (fr >= a60) {
                    code = (fr < a120) ? 66 : 64;
                } else if (fr <= -a60) {
                    code = (fr > -a120) ? 65 : 63;
                }
                float yawDeg = 0.0f;
                float seconds = 0.0f;
                s.spotTurnLeft = 0.0f;
                if (code >= 0 && RF3FishModel_CodeTurn(code, &yawDeg, &seconds) && seconds > 0.0f) {
                    s.motionCode = code;
                    s.spotTurnLeft = seconds;
                    // the pattern's yaw (RF3 sense, + = right) as a heading change per tick, OoT sense
                    s.spotTurnRate = -(yawDeg * kPi / 180.0f) * kTick / seconds;
                }
                char line[200];
                std::snprintf(line, sizeof(line),
                              "slot=%d rf3-follow enter to_angler=%.0fdeg (RF3 sense, + = right) code=%d "
                              "pattern_yaw=%.0f seconds=%.2f",
                              in.slot, fr * 180.0f / kPi, code, yawDeg, seconds);
                rf3model::Log(line);
                break;
            }
        }
        Log(in, s, event); // its probes/hits are the previous mode's
        s.probes = 0;
        s.probeHits = 0;
    }

    // Step 30c-1: AWAY's run (0x001A3B90). Aim mode = the rod lowered (stick Y <= -0.5), or the stick in the middle
    // and not reeling. When it switches on, or every frame while the line is more than 30 deg off the fight centre
    // (RF3: a spot with centre points; the mod always has one), the target is straight out along the line, 10 m
    // beyond the fish's distance; the steering below goes there until reached, blocked or the wrong-side check.
    if (in.mode == 10) {
        const int32_t aim = (in.stickY <= -0.5f) || (in.stickY < 0.5f && !in.reelHeld);
        const float rel = WrapAngle(s.centerYaw - LineYaw(in, s));
        bool set = false;
        const char* why = "";
        if (std::fabs(rel) <= kOnLine) {
            if (aim != s.awayAim) {
                s.awayAim = aim;
                set = aim != 0;
                why = "aim-on";
            }
        } else if (aim) {
            set = true; // off the line: re-aimed every frame
            why = "off-line";
        }
        if (set) {
            float ax = in.fishPos[0] - in.anglerPos[0];
            float az = in.fishPos[2] - in.anglerPos[2];
            float d = std::sqrt(ax * ax + az * az);
            if (d > 1.0f) {
                float out = d + 10.0f * RF3_UNITS_PER_METRE;
                s.target[0] = in.anglerPos[0] + ax / d * out;
                s.target[1] = ClampDepth(in, in.fishPos[1], heightUnits);
                s.target[2] = in.anglerPos[2] + az / d * out;
                char line[200];
                std::snprintf(line, sizeof(line),
                              "slot=%d rf3-away-run why=%s stickY=%.2f reel=%d rel=%.0fdeg dist=%.0f", in.slot, why,
                              in.stickY, in.reelHeld, rel * 180.0f / kPi, d);
                if (s.lastEvent != std::string("away-run-") + why || s.sinceLog >= kLogEverySeconds) {
                    rf3model::Log(line);
                    s.lastEvent = std::string("away-run-") + why;
                    s.sinceLog = 0.0f;
                }
            }
        }
    }

    // Step 26f-1: RF3's obstacle probe, for every state with its own path (FOLLOW has none: the line drags it).
    if (in.mode != 11) {
        int deflections = 0;
        bool clear = ProbeAndDeflect(in, s, lengthUnits, heightUnits, s.speed * std::max(1, in.substeps), &deflections);
        s.probes++;
        if (!clear || deflections > 0) {
            s.probeHits++;
        }
        if (!clear) {
            if (in.mode == 8 || in.mode == 9 || in.mode == 10) {
                // RF3 0x0019B360: blocked -> the fight target on the other side, turning at 4 x base
                float y = (in.mode == 9) ? s.bobDepth[s.bobSide] : ClampDepth(in, in.fishPos[1], heightUnits);
                s.side = (s.side == 1) ? 2 : 1;
                if (!PickFightTarget(in, s, 20.0f, y)) {
                    SetAhead(in, s, s.yaw, 2.0f * lengthUnits);
                }
                s.turnRate = kBaseTurn * 4.0f;
            }
            // STIFFEN/STOP keep their target [assumed: 0x0019B480's blocked handling not read]
            Log(in, s, "probe-blocked");
        } else if (deflections > 0) {
            Log(in, s, deflections == 1 ? "probe-deflect" : "probe-deflect2");
        }
    }

    const bool fight = (in.mode == 8 || in.mode == 9 || in.mode == 10);
    const float cap = (fight || in.mode == 11) ? fullCap : fullCap * kSlowShare;
    for (int32_t t = 0; t < std::max(1, in.substeps); t++) {
        if (fight) {
            float hx = s.target[0] - in.fishPos[0];
            float hz = s.target[2] - in.fishPos[2];
            float y = (in.mode == 9) ? s.bobDepth[s.bobSide] : ClampDepth(in, in.fishPos[1], heightUnits);
            if (std::sqrt(hx * hx + hz * hz) < lengthUnits * 0.5f) {
                // Step 30a: reached -> the side is picked again from the heading, at the state's every-frame angle
                // (RF3: RESIST 5, STRUGGLE 5, AWAY 10 deg). Was: the same side at 20 deg.
                s.side = 0;
                if (PickFightTarget(in, s, in.mode == 10 ? 10.0f : 5.0f, y, "reached")) {
                    Log(in, s, "reached-repick");
                } else {
                    // neither side is open water: keep swimming ahead (RF3 would drop the hook, 6.3)
                    SetAhead(in, s, s.yaw, 2.0f * lengthUnits);
                    Log(in, s, "reached-blocked");
                }
            } else {
                // Step 30a (B): heading more than 30 deg off the line on the wrong side for its side -> pick again at
                // 10 deg (0x0019DE60's last branch).
                float rel = WrapAngle(s.centerYaw - LineYaw(in, s)); // 30a-2: against the centre's bearing
                if ((rel > kOnLine && s.side != 2) || (rel < -kOnLine && s.side != 1)) {
                    s.side = 0;
                    if (PickFightTarget(in, s, 10.0f, y, "wrong-side")) {
                        Log(in, s, "wrong-side-repick");
                    }
                }
            }
            if (in.mode == 9) {
                s.bobTimer += kTick;
                if (s.bobTimer >= kStruggleBobSeconds) {
                    s.bobTimer = 0.0f;
                    s.bobSide ^= 1;
                }
                s.target[1] = s.bobDepth[s.bobSide];
            }
        }
        if (in.mode == 11) {
            s.target[0] = in.anglerPos[0];
            s.target[2] = in.anglerPos[2];
        }
        // turn toward the target, at most turnRate per tick, in yaw and pitch
        float dx = s.target[0] - in.fishPos[0];
        float dy = s.target[1] - in.fishPos[1];
        float dz = s.target[2] - in.fishPos[2];
        float flat = std::sqrt(dx * dx + dz * dz);
        if (in.mode == 11 && s.spotTurnLeft > 0.0f) {
            // Step 30b-2: FOLLOW's turn on the spot: the pattern's turn, no steering
            s.yaw = WrapAngle(s.yaw + s.spotTurnRate);
            s.spotTurnLeft -= kTick;
            if (s.spotTurnLeft <= 0.0f) {
                s.motionCode = -1;
                char line[120];
                std::snprintf(line, sizeof(line), "slot=%d rf3-follow turn-done heading=%.0fdeg", in.slot,
                              s.yaw * 180.0f / kPi);
                rf3model::Log(line);
            }
        } else if (flat > 0.01f) {
            float want = std::atan2(dx, dz);
            float diff = WrapAngle(want - s.yaw);
            s.yaw = WrapAngle(s.yaw + std::clamp(diff, -s.turnRate, s.turnRate));
            if (in.mode == 11) {
                // Step 30b-2 (0x001A4340): a held turn while the turn is capped (RF3: 26 right, 25 left; OoT +
                // = left), else towed
                s.motionCode = (std::fabs(diff) > s.turnRate) ? ((diff > 0.0f) ? 25 : 26) : -1;
            }
        }
        float wantPitch = std::clamp(std::atan2(dy, std::max(flat, 1.0f)), -kMaxPitch, kMaxPitch);
        s.pitch += std::clamp(wantPitch - s.pitch, -s.turnRate, s.turnRate);
        // speed (0x0019C220): up to the cap by sine over the ramp; never above the cap
        if (s.speed < cap) {
            float u = std::min(1.0f, s.rampT / rampS);
            s.speed = cap * std::sin(u * kPi * 0.5f);
            s.rampT = std::min(rampS, s.rampT + kTick);
        } else {
            s.speed = cap;
        }
    }

    float speedFrame = s.speed * std::max(1, in.substeps);
    out->drag[0] = out->drag[1] = out->drag[2] = 0.0f;
    if (in.mode == 11 && s.spotTurnLeft > 0.0f) {
        // Step 30b-2b (user, 2026-10-14: the turn "brought back the jerk when going from away to follow"): while it
        // turns on the spot the line still drags it (RF3: FOLLOW has no path of its own), straight toward the angler
        // by the same amount as below (the reel-in + a gentle catch-up), whatever its heading. 30b-2 stood it still,
        // so reeling built a gap (~40 units over the 1.5 s turn) that it then closed at once.
        speedFrame = 0.0f;
        float ax = in.anglerPos[0] - in.fishPos[0];
        float az = in.anglerPos[2] - in.fishPos[2];
        float dist = std::sqrt(ax * ax + az * az);
        float gap = dist - in.distUnits;
        if (dist > 1.0f && gap > 0.0f) {
            const float reelIn = (s.prevDist > 0.0f) ? std::max(0.0f, s.prevDist - in.distUnits) : 0.0f;
            const float catchUp = std::clamp(gap * kCatchUpShare, 0.0f, kCatchUpMax);
            const float d = std::clamp(std::min(gap, reelIn + catchUp), 0.0f, kMaxDragUnits);
            out->drag[0] = ax / dist * d;
            out->drag[2] = az / dist * d;
        }
    } else if (in.mode == 11) {
        // FOLLOW: RF3's line drags the fish, so it comes in as fast as the line is reeled, whatever its species speed
        // or the speed CVar (step 26e-5; 26e-3/4 capped it at the species speed, which made reeling slower: user,
        // 2026-10-08). Step 30b-1c: by the reel-in plus a gentle catch-up, no longer the whole gap at once.
        float ax = in.fishPos[0] - in.anglerPos[0];
        float az = in.fishPos[2] - in.anglerPos[2];
        float dist = std::sqrt(ax * ax + az * az);
        float gap = dist - in.distUnits;
        // Step 26e-7: OoT moves the fish along its real heading, which its wall bump can turn 90 deg (bumpTimer). Drag
        // only by the heading's share toward the angler, so a fish facing sideways or away is never pushed further out
        // (2026-10-09 CSV: a sideways fish opened the gap and ran away at 20 units/frame, through a wall).
        float toward = 0.0f;
        if (dist > 1.0f) {
            float realYaw = in.yaw * kPi / 32768.0f;
            toward = std::max(0.0f, -(std::sin(realYaw) * ax + std::cos(realYaw) * az) / dist);
        }
        // Step 30b-1c: the line's reel-in this frame (the fight's distance shrinking) + a gentle catch-up of the rest
        const float reelIn = (s.prevDist > 0.0f) ? std::max(0.0f, s.prevDist - in.distUnits) : 0.0f;
        const float catchUp = std::clamp(gap * kCatchUpShare, 0.0f, kCatchUpMax);
        speedFrame = std::clamp(std::min(gap, reelIn + catchUp), 0.0f, kMaxDragUnits) * toward;
    }
    s.prevDist = in.distUnits;
    float cp = std::cos(s.pitch);
    out->target[0] = in.fishPos[0] + std::sin(s.yaw) * cp * kAheadUnits;
    out->target[1] = in.fishPos[1] + std::sin(s.pitch) * kAheadUnits;
    out->target[2] = in.fishPos[2] + std::cos(s.yaw) * cp * kAheadUnits;
    out->speed = speedFrame;
    return 1;
}

extern "C" int32_t RF3FishMove_MotionCode(int32_t slot) {
    auto it = sStates.find(slot);
    return (it == sStates.end()) ? -1 : it->second.motionCode;
}

extern "C" void RF3FishMove_Log(const char* line) {
    rf3model::Log(line);
}
