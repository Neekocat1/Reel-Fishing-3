// Reel Fishing III mod (step 47b): RF3's free swimming, see RF3FishSwim.h and RF3-MECHANICS 6.9 (read from RF3's
// 0x0019FA60 / 0x0019FD10 hover, 0x001A0380 / 0x001A0400 / 0x0019B7A0 swim, 0x001A0610 / 0x0019B480 glide, the
// mode switch 0x0019F120; checked against a PCSX2 recording, 2026-10-17). Pure C++: no SoH calls.
//
// Hover (0): lasts 2 R + 2 (1 - calmness / 100) + 2 (1 - HP / 100) s, R = the sum of 10 rolls of 0-10, / 100 (about
//   0.5). Drifts toward a point max(1 m, 3 L) away at 0.05 x the species speed, along the direction to it, while
//   turning (0.5 deg per tick) to a wanted heading: +-45 deg of the heading, or into the region's current for fish
//   that face it. A probe hit stops both. Motion 2; turning shows raw 26/25 (held turns), or 6/5 past 90 deg.
// Calm (1) / fast (2) swim: to a point 5 L away (random yaw +-180 deg, pitch +-45), at 0.2 x the species speed
//   turning at 3 deg per tick (calm), or the full speed at 6 (fast), the speed ramped by sine over the speed-up time.
//   A target 80 deg or more off first turns the fish on the spot (65/66 90 deg, 63/64 180 deg). Probe: up to two
//   slides along a hit; blocked ends the swim. Ends when reached (L/2), blocked, or after 7 s. Motions 20 (1 s), 4
//   (to 2 s), then 3.
// Glide (3): coasts on along its heading, the speed falling from the cap to 0 over the slow-down time. Motion 3.
// Targets: a target out of the depth band is levelled; one farther than the home radius from home is replaced by a
//   point toward home at 0.2-0.8 x the radius; then the region's shape clamps it (user's choice 1a, 2026-10-17).
// Not here: fish pushing off each other (step 47e); RF3's map flow field (the region's one current stands in).

#include "RF3FishSwim.h"
#include "RF3FishTable.h" // step 47c: kFishSwim (generated, pure data)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kTick = 1.0f / 60.0f;
constexpr float kBaseTurn = 0.05236f;   // rad per tick (fish +0x29C, 3 deg)
constexpr float kHoverTurn = 0.0087f;   // rad per tick (0x0019FA60, 0.5 deg)
constexpr float kHoverDrift = 0.05f;    // x the species speed (0x0019FD10)
constexpr float kSlowShare = 0.2f;      // speed cap of a calm swim and its glide (0x0019B7A0, 0x0019B480)
constexpr float kSwimSeconds = 7.0f;    // a swim gives up (0x001A0400)
constexpr float kSpotTurn = 1.3962634f; // 80 deg: turn on the spot first (0x0019BEE0)
constexpr float kSpotTurn180 = 1.7453292f; // 100 deg: ... by 180
constexpr float kSpotTurnSeconds = 1.5f;   // [runtime] 91 frames per turn pattern in the 2026-10-17 recording

uint32_t sRand = 0x12345678u;

// RF3's generator (0x00142DA0)
uint32_t Rand() {
    sRand = sRand * 0xBB40E62Du + 0xBu; // * -0x44BF19D3
    return sRand >> 8;
}

// (rand % 20 - 9) x 0.1: -0.9 .. 1.0 (0x00199AD0, 0x001998F0)
float Roll() {
    return (static_cast<int>(Rand() % 20u) - 9) * 0.1f;
}

// 0x001AE0C0(100): the sum of 10 rolls of 0..10, wrapped above 100
int RollHundred() {
    int sum = 0;
    for (int i = 0; i < 10; i++) {
        sum += static_cast<int>(Rand() % 11u);
    }
    while (sum > 100) {
        sum -= 100;
    }
    return sum;
}

float WrapAngle(float a) {
    while (a > kPi) {
        a -= 2.0f * kPi;
    }
    while (a < -kPi) {
        a += 2.0f * kPi;
    }
    return a;
}

void Dir(float yaw, float pitch, float d[3]) {
    d[0] = std::sin(yaw) * std::cos(pitch);
    d[1] = std::sin(pitch);
    d[2] = std::cos(yaw) * std::cos(pitch);
}

float Length3(const float v[3]) {
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

struct SwimState {
    bool started = false;
    int32_t mode = RF3_SWIM_HOVER;
    float t = 0.0f;
    float hoverLen = 0.0f;
    float yaw = 0.0f;
    float pitch = 0.0f;
    float wantYaw = 0.0f; // hover's wanted heading
    float wantPitch = 0.0f;
    bool turnDone = false;
    float target[3] = { 0, 0, 0 };
    float moveDir[3] = { 0, 0, 1 }; // hover / glide: fixed at entry
    float speed = 0.0f;             // units per tick (RF3 +0x148)
    float rampT = 0.0f;             // +0x14C
    float turnRate = kBaseTurn;
    bool fast = false;    // +0x2A8: kept from a fast swim into its glide
    float swimT = 0.0f;   // +0x154: the accelerating swim's motion clock
    float spotLeft = 0.0f;
    float spotRate = 0.0f;
    int32_t code = -1;
    int32_t raw = -1;
    float blend = 0.0f;
};

std::map<int32_t, SwimState> sStates;

struct Ctx {
    const RF3FishSwimInput& in;
    SwimState& s;
    float pos[3];   // the fish during this frame's ticks
    float U;        // units per metre
    float L;        // body length, units
    float H;        // body height, units
    float speciesTick; // the species speed, units per tick
};

void Request(SwimState& s, int32_t code, float blend) {
    if (s.code != code) {
        s.code = code;
        s.blend = blend;
    }
    s.raw = -1; // a code replaces a raw animation set over it
}

void RequestRaw(SwimState& s, int32_t raw, float blend) {
    if (s.raw != raw) {
        s.raw = raw;
        s.blend = blend;
    }
}

// Turn the heading toward a direction (0x00199700): yaw by at most the turn rate, pitch by a quarter of it. Returns
// the RF3 flags: 0 = aligned (snapped); 4 = turning right, 8 = left; *rf3YawLeft = the yaw still to turn (RF3 sense,
// + = right).
int TurnToward(SwimState& s, float yaw, float pitch, float* rf3YawLeft) {
    int flags = 0;
    float dp = pitch - s.pitch;
    float rp = s.turnRate / 4.0f;
    if (dp > rp) {
        flags |= 1;
        s.pitch += rp;
    } else if (dp < -rp) {
        flags |= 2;
        s.pitch -= rp;
    } else {
        s.pitch = pitch;
    }
    float dy = WrapAngle(yaw - s.yaw);
    if (rf3YawLeft != nullptr) {
        *rf3YawLeft = -dy;
    }
    if (-dy > s.turnRate) {
        flags |= 4;
        s.yaw = WrapAngle(s.yaw - s.turnRate);
    } else if (-dy < -s.turnRate) {
        flags |= 8;
        s.yaw = WrapAngle(s.yaw + s.turnRate);
    } else {
        s.yaw = yaw;
    }
    return flags;
}

void YawPitchOf(const float d[3], float* yaw, float* pitch) {
    *yaw = std::atan2(d[0], d[2]);
    *pitch = std::atan2(d[1], std::sqrt(d[0] * d[0] + d[2] * d[2]));
}

// A target (0x00199AD0): distM away (at least 3 L), yaw = heading +- yawSpread, pitch +- pitchSpread (absolute);
// levelled when out of the depth band; toward home when beyond the home radius; then the region's shape.
void PickTarget(Ctx& c, float distM, float yawSpread, float pitchSpread) {
    SwimState& s = c.s;
    const RF3FishSwimInput& in = c.in;
    float dist = std::max(distM * c.U, 3.0f * c.L);
    float yaw = WrapAngle(s.yaw - yawSpread * Roll()); // RF3 sense + = right = OoT -
    float pitch = -pitchSpread * Roll();               // RF3 y down: + = nose down
    float d[3];
    Dir(yaw, pitch, d);
    for (int k = 0; k < 3; k++) {
        s.target[k] = c.pos[k] + d[k] * dist;
    }
    if (s.target[1] > in.bandTop || s.target[1] < in.bandBottom) {
        Dir(yaw, 0.0f, d);
        for (int k = 0; k < 3; k++) {
            s.target[k] = c.pos[k] + d[k] * dist;
        }
    }
    float fromHome[3] = { s.target[0] - in.home[0], s.target[1] - in.home[1], s.target[2] - in.home[2] };
    const float radius = in.species.homeRadiusM * c.U;
    if (Length3(fromHome) > radius) {
        float back = radius * static_cast<float>(((Rand() & 3u) + 1u) * 2u) / 10.0f;
        float toHome[3] = { in.home[0] - c.pos[0], in.home[1] - c.pos[1], in.home[2] - c.pos[2] };
        float len = Length3(toHome);
        for (int k = 0; k < 3; k++) {
            s.target[k] = c.pos[k] + (len > 0.001f ? toHome[k] / len : 0.0f) * back;
        }
    }
    if (in.clampXZ != nullptr) {
        in.clampXZ(s.target[0], s.target[2], &s.target[0], &s.target[2]);
    }
}

void Enter(Ctx& c, int32_t mode, RF3FishSwimOutput* out, int32_t why) {
    SwimState& s = c.s;
    out->modeChanged = 1;
    out->endWhy = why;
    out->prevMode = s.mode;
    out->prevModeTime = s.t;
    const int32_t prev = s.mode;
    const float prevT = s.t;
    // the state reset every entry does (0x0019A280); the speed +0x148 and the fast flag stay
    s.mode = mode;
    s.t = 0.0f;
    s.rampT = 0.0f;
    s.turnRate = kBaseTurn;
    s.swimT = 0.0f;
    s.spotLeft = 0.0f;
    s.turnDone = false;
    if (mode == RF3_SWIM_HOVER) {
        s.speed = 0.0f;
        s.hoverLen = 2.0f * RollHundred() * 0.01f + 2.0f * (1.0f - c.in.species.calmness * 0.01f) +
                     2.0f * (1.0f - c.in.hp * 0.01f);
        // 0x0019FA60 sets the slow hover turn only on the random-heading path; fish that face the current keep the
        // base turn (the recording: their hovers all end on the same heading)
        s.turnRate = c.in.species.facesCurrent ? kBaseTurn : kHoverTurn;
        PickTarget(c, 1.0f, kPi, kPi / 4.0f);
        float d[3] = { s.target[0] - c.pos[0], s.target[1] - c.pos[1], s.target[2] - c.pos[2] };
        float len = Length3(d);
        for (int k = 0; k < 3; k++) {
            s.moveDir[k] = len > 0.001f ? d[k] / len : 0.0f;
        }
        // the wanted heading (0x001998F0, or against the flow for fish that face the current)
        if (c.in.species.facesCurrent && c.in.currentSpeedMps > 0.0f) {
            s.wantYaw = WrapAngle(c.in.currentYaw + kPi);
            s.wantPitch = 0.0f;
        } else {
            s.wantYaw = WrapAngle(s.yaw - (kPi / 4.0f) * Roll());
            s.wantPitch = -(5.0f * kPi / 180.0f) * Roll();
        }
        Request(s, 2, 1.0f);
    } else if (mode == RF3_SWIM_CALM || mode == RF3_SWIM_FAST) {
        s.fast = (mode == RF3_SWIM_FAST);
        PickTarget(c, 5.0f * c.in.lengthM, kPi, kPi / 4.0f); // 5 L (0x001A0380)
    } else if (mode == RF3_SWIM_GLIDE) {
        Dir(s.yaw, s.pitch, s.moveDir);
        Request(s, 3, 0.2f);
    }
    if (c.in.log != nullptr) {
        char line[320];
        std::snprintf(line, sizeof(line),
                      "slot=%d rf3-swim mode=%d->%d why=%d after=%.2fs hoverLen=%.2f fast=%d target=(%.0f,%.0f,%.0f) "
                      "fish=(%.0f,%.0f,%.0f) homeDist=%.0f speed=%.3f/tick yaw=%.1fdeg",
                      c.in.slot, prev, mode, why, prevT, s.hoverLen, s.fast ? 1 : 0, s.target[0], s.target[1],
                      s.target[2], c.pos[0], c.pos[1], c.pos[2],
                      std::sqrt((c.pos[0] - c.in.home[0]) * (c.pos[0] - c.in.home[0]) +
                                (c.pos[2] - c.in.home[2]) * (c.pos[2] - c.in.home[2])),
                      s.speed, s.yaw * 180.0f / kPi);
        c.in.log(line);
    }
}

// The probe line for a move of `len` units along d (0x0019A6A0 with "from behind"): from 0.8 L behind, len + L +
// 0.1 m long. Returns 0 / 1 (normal in n) / 2.
int Probe(Ctx& c, const float d[3], float len, float n[3]) {
    if (c.in.probe == nullptr) {
        return 0;
    }
    float from[3], to[3];
    const float reach = len + c.L + 0.1f * c.U;
    for (int k = 0; k < 3; k++) {
        from[k] = c.pos[k] - d[k] * 0.8f * c.L;
        to[k] = from[k] + d[k] * reach;
    }
    n[0] = n[1] = n[2] = 0.0f;
    return c.in.probe(from, to, n);
}

// d projected onto the plane of n (0x001AE1B0); false when head-on
bool Slide(float d[3], const float n[3]) {
    float dn = d[0] * n[0] + d[1] * n[1] + d[2] * n[2];
    float sl[3] = { d[0] - n[0] * dn, d[1] - n[1] * dn, d[2] - n[2] * dn };
    float len = Length3(sl);
    if (len < 0.001f) {
        return false;
    }
    for (int k = 0; k < 3; k++) {
        d[k] = sl[k] / len;
    }
    return true;
}

// The move (0x0019AEE0): kept 0.05 m under the surface and half a body + 0.05 m over the ground.
void Move(Ctx& c, const float d[3], float len) {
    for (int k = 0; k < 3; k++) {
        c.pos[k] += d[k] * len;
    }
    float top = c.in.waterY - 0.05f * c.U;
    if (c.in.floorY > -30000.0f) {
        float bottom = c.in.floorY + (0.5f * c.H + 0.05f * c.U);
        if (bottom > top) {
            c.pos[1] = (top + bottom) * 0.5f;
            return;
        }
        c.pos[1] = std::max(c.pos[1], bottom);
    }
    c.pos[1] = std::min(c.pos[1], top);
}

// One hover tick (0x0019FD10). Returns true when the hover is over.
bool HoverTick(Ctx& c) {
    SwimState& s = c.s;
    float step = c.speciesTick * kHoverDrift;
    float d[3] = { s.target[0] - c.pos[0], s.target[1] - c.pos[1], s.target[2] - c.pos[2] };
    if (Length3(d) > step) {
        float n[3];
        // the probe reaches the drift step + L/2, from behind
        if (Probe(c, s.moveDir, step + 0.5f * c.L, n) == 0) {
            Move(c, s.moveDir, step);
        } else {
            s.turnDone = true; // RF3: a probe hit stops the turn too
        }
    }
    if (!s.turnDone) {
        float left = 0.0f;
        int flags = TurnToward(s, s.wantYaw, s.wantPitch, &left);
        if (flags == 0) {
            s.turnDone = true;
            RequestRaw(s, 2, 0.3f);
        } else if (flags & 4) {
            RequestRaw(s, left > kPi / 2.0f ? 6 : 26, 0.3f);
        } else if (flags & 8) {
            RequestRaw(s, left < -kPi / 2.0f ? 5 : 25, 0.3f);
        }
    }
    return s.t > s.hoverLen;
}

// One swim tick (0x001A0400 -> 0x0019B360 -> 0x0019B7A0). Returns an end reason, or 0 to go on.
int SwimTick(Ctx& c) {
    SwimState& s = c.s;
    const RF3FishSwimInput& in = c.in;
    if (s.t > kSwimSeconds) {
        return RF3_SWIM_END_TIME;
    }
    if (s.spotLeft > 0.0f) {
        // a turn on the spot: the heading turns by the pattern's yaw over its length, standing still [as 30b-2]
        s.yaw = WrapAngle(s.yaw + s.spotRate);
        s.spotLeft -= kTick;
        return 0;
    }
    // reached: within L/2 on every axis, + 0.1 m for fish under 150 mm (0x0019B280)
    float reach = 0.5f * c.L + (in.lengthMm < 150 ? 0.1f * c.U : 0.0f);
    if (std::fabs(s.target[0] - c.pos[0]) <= reach && std::fabs(s.target[1] - c.pos[1]) <= reach &&
        std::fabs(s.target[2] - c.pos[2]) <= reach) {
        return RF3_SWIM_END_REACHED;
    }
    float cap = c.speciesTick;
    float blend = 0.1f;
    if (s.fast) {
        s.turnRate = kBaseTurn * 2.0f;
    } else {
        cap = c.speciesTick * kSlowShare;
        blend = 0.3f;
    }
    // the accelerating swim: raw 20 for 1 s, then 4, then 3 from 2 s (+0x154)
    s.swimT += kTick;
    if (s.swimT < 1.0f) {
        Request(s, 20, blend);
    } else if (s.swimT < 2.0f) {
        Request(s, 4, 0.1f);
    } else {
        Request(s, 3, 0.1f);
    }
    float d[3] = { s.target[0] - c.pos[0], s.target[1] - c.pos[1], s.target[2] - c.pos[2] };
    float dist = Length3(d);
    for (float& v : d) {
        v = dist > 0.001f ? v / dist : 0.0f;
    }
    float toYaw, toPitch;
    YawPitchOf(d, &toYaw, &toPitch);
    float rf3Off = -WrapAngle(toYaw - s.yaw); // RF3 sense, + = right
    if (std::fabs(rf3Off) >= kSpotTurn) {
        int32_t code = rf3Off > 0.0f ? (rf3Off < kSpotTurn180 ? 66 : 64) : (rf3Off > -kSpotTurn180 ? 65 : 63);
        float yawDeg = (code == 66) ? 90.0f : (code == 65) ? -90.0f : (code == 64) ? 180.0f : -180.0f;
        float seconds = kSpotTurnSeconds;
        if (in.codeTurn != nullptr) {
            float y2 = 0.0f, s2 = 0.0f;
            if (in.codeTurn(code, &y2, &s2) && s2 > 0.0f) {
                yawDeg = y2;
                seconds = s2;
            }
        }
        // [assumed, fits the 2026-10-17 recording: a blocked Dace's 22 swims all ended at once] a turn on the spot
        // first probes along the heading; blocked (no normal) ends the swim
        float ahead[3], n[3];
        Dir(s.yaw, s.pitch, ahead);
        if (Probe(c, ahead, 0.05f * c.U, n) == 2) {
            return RF3_SWIM_END_BLOCKED;
        }
        s.code = code;
        s.raw = -1;
        s.blend = 0.1f;
        s.spotLeft = seconds;
        s.spotRate = -(yawDeg * kPi / 180.0f) * kTick / seconds;
        s.speed = 0.0f;
        return 0;
    }
    // speed (0x0019C220): up to the cap by sine over the speed-up time
    float u = std::min(1.0f, s.rampT / std::max(0.01f, in.species.speedUpS));
    s.speed = cap * std::sin(u * kPi * 0.5f);
    s.rampT += kTick;
    // probe: up to two slides along what it hits; blocked ends the swim
    for (int i = 0; i < 2; i++) {
        float n[3];
        int hit = Probe(c, d, s.speed + 0.05f * c.U, n);
        if (hit == 0) {
            break;
        }
        if (hit == 2 || !Slide(d, n)) {
            return RF3_SWIM_END_BLOCKED;
        }
        for (int k = 0; k < 3; k++) {
            s.target[k] = c.pos[k] + d[k] * dist;
        }
        s.target[1] = std::min(s.target[1], in.waterY - (0.5f * c.H + 0.05f * c.U));
    }
    YawPitchOf(d, &toYaw, &toPitch);
    TurnToward(s, toYaw, toPitch, nullptr);
    Move(c, d, s.speed);
    return 0;
}

// One glide tick (0x001A0660 -> 0x0019B480). Returns an end reason, or 0.
int GlideTick(Ctx& c) {
    SwimState& s = c.s;
    if (s.speed <= 0.0f) {
        s.speed = 0.0f;
        return RF3_SWIM_END_STOPPED;
    }
    float cap = c.speciesTick;
    if (s.fast) {
        s.turnRate = kBaseTurn * 2.0f;
    } else {
        cap = c.speciesTick * kSlowShare;
    }
    const float down = std::max(0.01f, c.in.species.slowDownS);
    s.speed = (s.rampT >= down - 1e-4f) ? 0.0f : cap * (1.0f - s.rampT / down); // 60 x 1/60 falls short of 1
    s.rampT = std::min(down, s.rampT + kTick);
    float n[3];
    int hit = Probe(c, s.moveDir, s.speed, n);
    if (hit == 2) {
        return RF3_SWIM_END_BLOCKED;
    }
    if (hit == 1 && !Slide(s.moveDir, n)) {
        s.speed = 0.0f;
        return RF3_SWIM_END_BLOCKED;
    }
    Move(c, s.moveDir, s.speed);
    float yaw, pitch;
    YawPitchOf(s.moveDir, &yaw, &pitch);
    if (!s.turnDone && TurnToward(s, yaw, pitch, nullptr) == 0) {
        s.turnDone = true;
    }
    Request(s, 3, 0.2f);
    return 0;
}

} // namespace

extern "C" void RF3FishSwim_Seed(uint32_t seed) {
    sRand = seed;
}

extern "C" int32_t RF3FishSwim_Species(int32_t record, RF3FishSwimSpecies* out, int32_t* layerMin,
                                       int32_t* layerMax) {
    const rf3::FishSwim* sw = rf3::FishSwimFor(record);
    if (sw == nullptr || sw->speedMps <= 0.0f) {
        return 0;
    }
    out->speedMps = sw->speedMps;
    out->speedUpS = sw->speedUpS;
    out->slowDownS = sw->slowDownS;
    out->homeRadiusM = sw->homeRadiusM;
    out->calmness = sw->calmness;
    out->rank = sw->rank;
    out->facesCurrent = sw->facesCurrent ? 1 : 0;
    *layerMin = sw->layerMin;
    *layerMax = sw->layerMax;
    return 1;
}

extern "C" void RF3FishSwim_Forget(int32_t slot) {
    sStates.erase(slot);
}

extern "C" void RF3FishSwim_DepthBand(float waterY, float centreFloorY, float spotFloorY, int32_t layerMin,
                                      int32_t layerMax, float heightUnits, float* top, float* bottom) {
    // depths below the surface, as RF3 measures them (y down)
    float column = (centreFloorY > -30000.0f) ? (waterY - centreFloorY) - heightUnits : 1000.0f; // [assumed] no floor
    float layer = std::max(0.0f, column) / 6.0f;
    float shallow = layer * layerMin + heightUnits;
    float deep = layer * (layerMax + 1);
    if (deep < shallow) {
        shallow = deep - heightUnits;
    }
    if (spotFloorY > -30000.0f) {
        float ground = waterY - spotFloorY;
        if (ground <= deep) {
            deep = std::max(ground - heightUnits, shallow);
        }
    }
    *top = waterY - shallow;
    *bottom = waterY - deep;
}

extern "C" void RF3FishSwim_Step(const RF3FishSwimInput* inp, RF3FishSwimOutput* out) {
    const RF3FishSwimInput& in = *inp;
    SwimState& s = sStates[in.slot];
    Ctx c{ in, s, { in.pos[0], in.pos[1], in.pos[2] }, in.unitsPerMetre, in.lengthM * in.unitsPerMetre,
           in.heightM * in.unitsPerMetre, in.species.speedMps / 60.0f * in.unitsPerMetre };
    out->modeChanged = 0;
    out->endWhy = RF3_SWIM_END_NONE;
    out->prevMode = s.mode;
    out->prevModeTime = s.t;
    if (in.reset || !s.started) {
        s = SwimState{};
        s.started = true;
        s.yaw = in.yaw;
        s.pitch = in.pitch;
        s.mode = -1;
        Enter(c, RF3_SWIM_HOVER, out, RF3_SWIM_END_NONE);
    }
    const int ticks = std::max(1, in.substeps);
    for (int i = 0; i < ticks; i++) {
        s.t += kTick;
        if (s.mode == RF3_SWIM_HOVER) {
            if (HoverTick(c)) {
                int32_t next = RF3_SWIM_HOVER;
                if (in.species.rank < 4) {
                    next = (Rand() % 10u < 7u) ? RF3_SWIM_CALM : RF3_SWIM_FAST;
                }
                Enter(c, next, out, RF3_SWIM_END_TIME);
            }
        } else if (s.mode == RF3_SWIM_CALM || s.mode == RF3_SWIM_FAST) {
            int why = SwimTick(c);
            if (why != 0) {
                Enter(c, in.species.facesCurrent ? RF3_SWIM_HOVER : RF3_SWIM_GLIDE, out, why);
            }
        } else {
            int why = GlideTick(c);
            if (why != 0) {
                Enter(c, RF3_SWIM_HOVER, out, why);
            }
        }
    }
    for (int k = 0; k < 3; k++) {
        out->step[k] = c.pos[k] - in.pos[k];
        out->target[k] = s.target[k];
    }
    out->yaw = s.yaw;
    out->pitch = s.pitch;
    out->mode = s.mode;
    out->modeTime = s.t;
    out->hoverLen = s.hoverLen;
    out->speed = (s.mode == RF3_SWIM_HOVER) ? c.speciesTick * kHoverDrift : s.speed;
    out->code = s.code;
    out->rawAnim = s.raw;
    out->blend = s.blend;
}
