// Reel Fishing III mod (step 26e-3): RF3's own movement for a hooked region fish on RF3's model and motions. See
// RF3FishMove.h, docs/DESIGN.md "26e plan" and RF3-MECHANICS 6.3 (read from RF3's 0x0019DE60, 0x0019B7A0,
// 0x0019C220, the state entries/updates). Runs RF3's 1/60 s ticks, `substeps` per game frame.
//
// RESIST, STRUGGLE, AWAY: the fight target, a point on the circle around the angler at the fight's distance, turned
//   to the fish's side (entry: RESIST 5 deg, STRUGGLE/AWAY 10 deg; later picks 20 deg on the same side, so the fish
//   works its way around the angler). Picked again when reached (within half a body length); a target that isn't
//   open water switches the side and turns 4x faster. Fight mode: the species' full speed, ramped by sine; turning
//   at 2 x RF3's base 3 deg per tick. STRUGGLE's target depth alternates every 0.25 s between its entry depth and
//   2 body heights deeper. (AWAY's wider circle is in the fight's distance already: the sim adds 2 lengths.)
// STIFFEN: turns 90 deg to the side that points away from the angler [assumed which side], slowly (0.2 x speed).
// STOP: glides to a point 2 body lengths ahead at 0.2 x speed.
// FOLLOW: faces the angler and is drawn in as the fight's distance shrinks (RF3: the line drags it).
// Obstacle probes (step 26f-1, RF3-MECHANICS 6.4): every state but FOLLOW probes ahead toward its target and slides
//   the target along what it hits (up to 2 times); blocked fight states re-pick on the other side.
// Not used [RF3-MECHANICS 6.3]: AWAY's rod-side aim, the 30-degree wrong-side check, the hook dropping when no water
// is found (the mod keeps the hook and logs "blocked").
//
// Log, don't look: logs/rf3_models.txt gets "rf3-move" lines on every mode change, target pick and block.

#include "RF3FishMove.h"

#include <algorithm>
#include <cmath>
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
// 10-30 m, so the same speed circles far faster. Scaled by this CVar, default 0.5 [user's call; RF3 = 1.0].
constexpr float kDefaultSpeedScale = 0.5f;
constexpr float kLogEverySeconds = 1.0f; // at most one repeated log line per event and second
// FOLLOW: at most this far per frame [assumed, not RF3 data]. Reeling closes about 1 unit/frame, so 4 still catches up
// (26e-5's goal), but smoothly. Step 26e-11: was 20, which closed a gap built up at a wall in one 27-unit jump
// (rf3_fishing_20261009_203143.csv, frames 1303 and 1327).
constexpr float kMaxDragUnits = 4.0f;

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

// RF3's fight target (0x0019DE60): the circle around the angler at the fight's distance, turned by +-angle to the
// fish's side. Returns false if neither side is open water.
bool PickFightTarget(const RF3FishMoveInput& in, MoveState& s, float angleDeg, float y) {
    float dx = in.fishPos[0] - in.anglerPos[0];
    float dz = in.fishPos[2] - in.anglerPos[2];
    float len = std::sqrt(dx * dx + dz * dz);
    if (len < 1.0f) {
        dx = std::sin(s.yaw);
        dz = std::cos(s.yaw);
        len = 1.0f;
    }
    float base = std::atan2(dx / len, dz / len); // yaw of the angler -> fish line
    if (s.side == 0) {
        // [assumed] the side the fish is already heading toward (RF3 uses its angle to the line, 0x0019EE70)
        s.side = (WrapAngle(s.yaw - base) < 0.0f) ? 1 : 2;
        s.turnRate = kBaseTurn * 2.0f;
    }
    for (int tries = 0; tries < 2; tries++) {
        float a = base + ((s.side == 1) ? -1.0f : 1.0f) * angleDeg * kPi / 180.0f;
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
    }
    s.lastFrame = in.frame;
    s.sinceLog += kTick * std::max(1, in.substeps);

    if (in.mode != s.mode) {
        s.mode = in.mode;
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
                if (!PickFightTarget(in, s, in.mode == 8 ? 5.0f : 10.0f, y)) {
                    SetAhead(in, s, s.yaw, lengthUnits);
                    event = "enter-blocked";
                }
                break;
            case 12: { // STIFFEN: 90 deg to the side that points away from the angler [assumed]
                float ax = in.fishPos[0] - in.anglerPos[0];
                float az = in.fishPos[2] - in.anglerPos[2];
                float left = s.yaw + kPi / 2.0f;
                float right = s.yaw - kPi / 2.0f;
                float dl = std::sin(left) * ax + std::cos(left) * az;
                float dr = std::sin(right) * ax + std::cos(right) * az;
                SetAhead(in, s, dl >= dr ? left : right, 2.0f * lengthUnits);
                s.turnRate = kBaseTurn * 2.0f;
                break;
            }
            case 7: // STOP: glide 2 lengths ahead
                SetAhead(in, s, s.yaw, 2.0f * lengthUnits);
                break;
            case 11: // FOLLOW
                s.target[0] = in.anglerPos[0];
                s.target[1] = in.fishPos[1];
                s.target[2] = in.anglerPos[2];
                s.turnRate = kBaseTurn * 2.0f;
                break;
        }
        Log(in, s, event); // its probes/hits are the previous mode's
        s.probes = 0;
        s.probeHits = 0;
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
            if (std::sqrt(hx * hx + hz * hz) < lengthUnits * 0.5f) { // reached: the next point, same side, 20 deg
                float y = (in.mode == 9) ? s.bobDepth[s.bobSide] : ClampDepth(in, in.fishPos[1], heightUnits);
                if (PickFightTarget(in, s, 20.0f, y)) {
                    Log(in, s, "reached-repick");
                } else {
                    // neither side is open water: keep swimming ahead (RF3 would drop the hook, 6.3)
                    SetAhead(in, s, s.yaw, 2.0f * lengthUnits);
                    Log(in, s, "reached-blocked");
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
        if (flat > 0.01f) {
            float want = std::atan2(dx, dz);
            s.yaw = WrapAngle(s.yaw + std::clamp(WrapAngle(want - s.yaw), -s.turnRate, s.turnRate));
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
    if (in.mode == 11) {
        // FOLLOW: RF3's line drags the fish, so it comes in as fast as the line is reeled, whatever its species speed
        // or the speed CVar. It closes the whole gap to the fight's distance each frame (step 26e-5; 26e-3/4 capped
        // it at the species speed, which made reeling slower: user, 2026-10-08).
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
        speedFrame = std::clamp(gap, 0.0f, kMaxDragUnits) * toward;
    }
    float cp = std::cos(s.pitch);
    out->target[0] = in.fishPos[0] + std::sin(s.yaw) * cp * kAheadUnits;
    out->target[1] = in.fishPos[1] + std::sin(s.pitch) * kAheadUnits;
    out->target[2] = in.fishPos[2] + std::cos(s.yaw) * cp * kAheadUnits;
    out->speed = speedFrame;
    return 1;
}

extern "C" void RF3FishMove_Log(const char* line) {
    rf3model::Log(line);
}
