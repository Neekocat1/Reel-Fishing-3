#ifndef RF3_SIM_H
#define RF3_SIM_H

// Reel Fishing III mod: the hook set and fight, as a pure simulation.
// No OoT or SoH headers: the host (z_fishing.c, or the command-line harness)
// feeds one FrameInput per frame and reads the results back.
// Written from docs/RF3-MECHANICS.md section 1 (project repository).

#include <cstdint>

#include "RF3Params.h"

namespace rf3 {

// Fish states, numbered as in Reel Fishing III's state table (RF3-MECHANICS 1.5).
enum class Mode : int32_t {
    None = 0, // NORMAL in RF3; also "no last run"
    Hit = 2,
    HookOver = 5,
    LineBreak = 6,
    Stop = 7,
    Resist = 8,
    Struggle = 9,
    Away = 10,
    Follow = 11,
    Stiffen = 12,
    Bukkonuki = 15,
    Catch = 16,
};

enum class Outcome : int32_t {
    None = 0,
    Caught,
    LineBreak,
    HookOut,   // hook came out during the fight
    Missed,    // hook-set too early, too late, or never
    Bukkonuki, // perfect hook set: fish yanked out, no fight
};

// One frame of input from the host. Stick values use RF3's convention:
// positive stickY = rod raised (stick pulled back).
struct FrameInput {
    bool reel = false;
    float stickX = 0.0f;           // -1 .. 1
    float stickY = 0.0f;           // -1 .. 1
    float fishDistance = 0.0f;     // m, current horizontal angler-to-fish distance
    float distanceCap = 1000.0f;   // m, largest allowed target distance (line out + margin)
    float landingDistance = 0.27f; // m, caught at or below this (1.7)
    float lureDepth = 0.0f;        // m below the surface, for BUKKONUKI (1.6)
};

class FightSim {
  public:
    FightSim(const FishParams& fish, const TackleParams& tackle, uint32_t seed = 1);

    // A fish has struck: start the hook-set timer (state HIT).
    void StartStrike();
    // Skip HIT: the hook is already set, with timing a and strength b (1.6).
    // The fight starts in STRUGGLE on the next Update.
    void StartFight(float timingA, float strengthB, float fishDistance);
    // The fish's body length in metres, for the AWAY surge. 0 (the default) means no surge.
    void SetBodyLength(float metres) { mBodyLength = metres; }
    // Advance one frame (1/60 s).
    void Update(const FrameInput& in);
    // Replay harness only (DESIGN step L4), never called by the game: put the fish
    // into a recorded fight state with recorded values. The state is entered on the
    // next Update, as after a normal transition.
    void Resync(Mode mode, Mode lastRun, float hp, float hook, float tension, float distance);

    Mode GetMode() const { return mMode; }
    Mode GetLastRun() const { return mLastRun; }
    // The state chosen this frame, entered on the next Update (None if no change).
    Mode GetPendingMode() const { return mHasPending ? mPending : Mode::None; }
    Outcome GetOutcome() const { return mOutcome; }
    bool IsHooked() const { return mHooked; }
    float GetHp() const { return mHp; }
    float GetHook() const { return mHook; }
    float GetTension() const { return mTension; }
    float GetTensionLimit() const { return mTensionLimit; }
    float GetDistance() const { return mDistance; }
    float GetStateTime() const { return mStateTime; }
    float GetStateDuration() const { return mDuration; }
    float GetStopWindowEnd() const { return mStopWindowEnd; }
    bool GetStopExpectsReel() const { return mStopExpectReel; }
    const FishParams& GetFish() const { return mFish; }
    float GetHookSetTiming() const { return mHookSetTiming; }   // "a", 0..1 (1.6)
    float GetHookSetStrength() const { return mHookSetStrength; } // "b", 0..1 (1.6)
    int GetShakeSide() const { return mShakeSide; } // STRUGGLE left/right, for visuals

  private:
    void Enter(Mode mode, const FrameInput& in);
    Mode UpdateHit(const FrameInput& in);
    Mode UpdateStruggle(const FrameInput& in);
    Mode UpdateAway(const FrameInput& in);
    Mode UpdateFollow(const FrameInput& in);
    Mode UpdateStiffen(const FrameInput& in);
    Mode UpdateStop(const FrameInput& in);
    Mode UpdateResist(const FrameInput& in);
    void Finish(Mode mode, Outcome outcome);

    void AddHp(float ratePerSecond);
    void AddHook(float ratePerSecond);
    void AddTension(float ratePerSecond);
    void AddDistance(float rate, const FrameInput& in);
    uint32_t NextRandom();

    FishParams mFish;
    TackleParams mTackle;
    uint32_t mRng;

    Mode mMode = Mode::None;
    Mode mPending = Mode::None; // next state chosen by the last handler
    bool mHasPending = false;
    Mode mLastRun = Mode::None; // "last chosen run", RF3 +0x108 (1.5)
    Outcome mOutcome = Outcome::None;
    bool mHooked = false;

    float mHp = 100.0f;
    float mHook = 0.0f;
    float mTension = 0.0f;
    float mTensionLimit = 0.0f;
    float mDistance = 0.0f;
    float mBodyLength = 0.0f;

    float mStateTime = 0.0f;
    float mDuration = 0.0f;
    float mStopWindowEnd = 0.0f;
    bool mStopExpectReel = false;
    float mShakeTimer = 0.0f;
    int mShakeSide = 0;
    bool mArmJerk = false; // stick seen at or below +0.5 since the strike
    bool mArmReel = false; // reel seen released since the strike
    float mHookSetTiming = 0.0f;
    float mHookSetStrength = 0.0f;
};

// --- Approach (section 2) ---------------------------------------------------

// One shared 0..1 "lure action" meter (2.1).
class LureMeter {
  public:
    // moved: the lure moved more than 0.0005 m this frame.
    void Update(bool lureInWater, bool moved, bool reelHeld);
    float Get() const { return mValue; }

  private:
    float mValue = 0.0f;
};

enum class ApproachMode : int32_t { Normal = 0, Chase = 1, Wait = 3 };

// Step 20b-5: the rod kind (RROD +0x14) changes the approach (2.9). Step 20c-3b: flies too.
enum class RodKind : int32_t { Lure = 0, Fly = 1, Bait = 2 };
enum class ApproachEvent : int32_t { None = 0, Strike, Fail };

struct NoticeInput {
    bool lureInWater = false;
    bool otherFishChasing = false;
    float horizontalDistance = 0.0f; // m, fish to lure
    float distance = 0.0f;           // m, fish to lure (3D)
    float angleFromHeading = 0.0f;   // radians, lure direction relative to the fish's heading
    float sightRange = 0.0f;         // m, from SightRange()
};

struct ChaseInput {
    bool lureInWater = false;
    bool lureNearAngler = false; // lure within the landing distance of the angler
    bool reachedLure = false;
};

// One fish's approach state: remembered lure action, wariness, NORMAL / CHASE / WAIT.
class ApproachFish {
  public:
    ApproachFish() = default;
    explicit ApproachFish(const FishParams& fish) : mFish(&fish) {}

    // Every frame: remembered level (2.2) and wary timer. kind: the current rod's kind (2.9).
    // movedM: how far the lure moved in this RF3 step (a fly must drift, 2.9).
    void Tick(float meter, RodKind kind = RodKind::Lure, float movedM = 0.0f);
    bool IsInterested() const;
    // NORMAL -> CHASE check (2.3). Starts the chase and returns true if it passes.
    bool TryNotice(const NoticeInput& in);
    // CHASE / WAIT (2.5, 2.6). randomRoll: a fresh random number per call.
    ApproachEvent UpdateChase(const ChaseInput& in, uint32_t randomRoll);
    void SetWary(float seconds);
    void Stop() { mMode = ApproachMode::Normal; }

    ApproachMode GetMode() const { return mMode; }
    float GetMemory() const { return mMemory; }
    float GetHold() const { return mHold; }
    float GetWary() const { return mWary; }
    float GetWaitLeft() const { return mWaitLeft; }
    int GetWaits() const { return mWaits; } // step 20b-5: WAITs in this chase (2.6, 2.9)
    const FishParams& GetFish() const { return *mFish; } // step 14b: the chaser's species values

  private:
    const FishParams* mFish = &kBassNormal;
    ApproachMode mMode = ApproachMode::Normal;
    float mMemory = 0.0f;
    float mHold = 0.0f;
    float mWary = 0.0f;
    float mWaitLeft = 0.0f;
    // Step 20b-5 (2.9).
    float mMeter = 0.0f;              // the meter seen by the last Tick
    float mMoved = 0.0f;              // step 20c-3b: lure movement in the last Tick's step, m
    RodKind mKind = RodKind::Lure;
    float mLostInterest = 0.0f;       // s without interest in this chase (bait patience)
    int mWaits = 0;
};

// Fly and bait rods: a chase fails after this long without interest (2.5, 2.9; 0x001A08D0).
inline constexpr float kBaitPatience = 0.5f;
// Fly rods: interested while the meter is below this and the fly drifts (0x0019E710).
inline constexpr float kFlyMaxMeter = 0.1f;
inline constexpr float kFlyMinDrift = 0.00016666666f; // m per RF3 step
// Bait rods: a fish is interested while the meter is below this, i.e. the bait is still (0x0019E820).
inline constexpr float kBaitStillMeter = 0.01f;

// Activity level from the fish's tables and the game conditions (2.4).
int ActivityLevel(const FishParams& fish, const Conditions& conditions);
// Sight range in metres for the default lure (appeal 15/15) (2.4).
float SightRange(const FishParams& fish, int activityLevel);
// Step 18c: with the lure's appeal for this fish record (2.4: x appeal / 15). Appeal 0 = never seen.
float SightRangeWithAppeal(const FishParams& fish, int activityLevel, int lureAppeal);

// --- Cast (section 3.1) -----------------------------------------------------

// RF3 cast power after holding the cast button for heldSeconds: 0.95/s, up to 1.0.
float CastPower(float heldSeconds);
// Our mapping (DESIGN step 11), not RF3's: the share of the host game's full cast
// distance for this power. fullHoldPower is the power reached by holding through the
// host's whole wind-up; minimum 0.15.
float CastDistanceFactor(float power, float fullHoldPower);

// --- Lure depth (section 3.2; DESIGN step 20a-4) ----------------------------

// One lure's movement fields, from its RLRE record (tackle.json "move").
// Speeds are m/s: base + rate x t, t in seconds (3.2).
struct LureMove {
    uint8_t restAction = 2;     // +0x0E: 0/3/5 sink, 1/2/4 float up, when Cross is not held
    uint8_t twitchResponse = 2; // +0x12: 0 down, 1 up, 2 hold (twitch, or reeling with the rod held back)
    uint8_t buoyancy = 2;       // +0x2C: landing depth class
    float speedBase[4] = {};    // reel down, reel up, rest sink, rest float-up
    float speedRate[4] = {};
};

// Lure +0x50 in RF3: 1 reeling, 2 resting.
enum class LureMode : int32_t { Reel = 1, Rest = 2 };

// One 60 Hz frame of input.
struct LureStep {
    bool reeling = false;        // Cross held (in OoT: the reel button)
    bool rodBack = false;        // rod pulled far back (RF3: raw stick Y > 230)
    float floorDepth = 1000.0f;  // m below the surface: the lure's resting depth on the bottom
};

// Depth below the surface (m, + = deeper) of a lure in the water. Vertical only: the host
// keeps moving the lure horizontally. The surface is RF3's resting level for a floating lure.
class LureSim {
  public:
    explicit LureSim(const LureMove& move) : mMove(move) {}

    // The cast has landed: start resting at the landing depth (3.2 "Landing").
    void Land();
    // Replay tests: put the lure at this depth (keeps t, mode, twitch).
    void SetDepth(float depth) { mDepth = depth; }
    void Step(const LureStep& in);

    float GetDepth() const { return mDepth; }
    float GetT() const { return mT; }
    float GetVelocity() const { return mVelocity; } // m/s this frame, + = down
    LureMode GetMode() const { return mMode; }
    bool IsTwitching() const { return mTwitchFrames > 0; }
    bool IsOnBottom() const { return mOnBottom; }
    bool IsDiveStopped() const { return mDiveStopped; }

  private:
    float Speed(int pair) const { return mMove.speedBase[pair] + mMove.speedRate[pair] * mT; }
    float ByTwitchResponse() const;

    LureMove mMove;
    LureMode mMode = LureMode::Rest;
    float mDepth = 0.0f;
    float mT = 0.0f;
    float mVelocity = 0.0f;
    float mMomentum = 0.0f;  // m/s carried from reeling, fading x0.95 per frame at rest
    bool mOnBottom = false;
    bool mAtSurface = false;
    bool mDiveStopped = false;
    // Twitch detector (0x002027C0): only set up once Cross has been pressed after the cast.
    bool mTwitchArmed = false;
    int mTwitchState = 0;
    int mTwitchFrames = 0; // frames left with the twitch flag set
};

// Landing depths below the resting level: the recorded landing y (0.057 topwater, 0.316
// otherwise; 0.3 m cap + one frame) minus the resting level 0.035 (RF3-MECHANICS 3.2).
inline constexpr float kLureLandingDepthTopwater = 0.022f;
inline constexpr float kLureLandingDepth = 0.281f;
// On the bottom while within this of the resting depth there (probe 0.02 m below, rest 0.01 m up).
inline constexpr float kLureBottomBand = 0.01f;
// A twitch lasts 30 frames (0.5 s), recorded.
inline constexpr int kLureTwitchFrames = 30;
// [assumed] The Floating Minnow stops diving at y 0.204 while reeled (0.169 m below the resting
// level), for a reason not found yet (DESIGN "Polish later"). Applied to every buoyancy-1 lure.
inline constexpr float kLureDiveStopBuoyancy1 = 0.169f;
// [assumed] A reeled Popper gets no deeper than 0.048 m below the resting level (recorded, 4 reels;
// RF3 then brings it back to the surface, not modelled). Applied to every buoyancy-0 (topwater) lure
// (user's decision, 2026-10-07; DESIGN "Polish later").
inline constexpr float kLureDiveStopBuoyancy0 = 0.048f;

// --- Bait depth (step 20b-5; RF3-MECHANICS 2.9, DESIGN 20b-5) -----------------

// Mod design (user, 2026-10-07): the float depth setting (Surface .. Deepest) sets the bait's depth in m.
// RF3 itself rests every bait at 0.2 m whatever the setting; equal steps as RF3's tackle pane shows them.
inline constexpr float kBaitDepthBySetting[5] = { 0.2f, 0.5f, 0.8f, 1.1f, 1.4f };
// RF3: a bait rig lands with a 0.2 m target depth (0x00201F70).
inline constexpr float kBaitLandingDepth = 0.2f;
// [assumed] It then sinks to its depth at RF3's bait sink speed (0x002194B0: 0.1 m/s below 0.5 m).
inline constexpr float kBaitSinkSpeed = 0.1f;

// A bait under a float: sinks to its target depth and holds there; reeling keeps its depth (RF3); the
// bottom stops it. targetDepth >= 1000 = no float (sinks to the bottom).
class BaitSim {
  public:
    explicit BaitSim(float targetDepth) : mTarget(targetDepth) {}

    void Land();
    void Step(bool reeling, float floorDepth);

    float GetDepth() const { return mDepth; }
    float GetTarget() const { return mTarget; }
    bool IsReeling() const { return mReeling; }
    bool IsOnBottom() const { return mOnBottom; }

  private:
    float mTarget;
    float mDepth = 0.0f;
    bool mReeling = false;
    bool mOnBottom = false;
};

// --- Fly depth (step 20c-3b; RF3-MECHANICS 2.9) -----------------------------------

// RF3: a dry fly lands with a 0.05 m target and floats up at 0.1 m/s; a sunken fly lands at the 0.3 m cap and
// stays (it only sinks deeper than 0.5 m). Depths below the surface ([assumed] relative to the resting level).
inline constexpr float kFlyDryLandingDepth = 0.05f;
inline constexpr float kFlySunkDepth = 0.3f;
inline constexpr float kFlyRiseSpeed = 0.1f;

class FlySim {
  public:
    explicit FlySim(bool sunk) : mSunk(sunk) {}

    void Land() {
        mDepth = mSunk ? kFlySunkDepth : kFlyDryLandingDepth;
        mReeling = false;
        mOnBottom = false;
    }
    void Step(bool reeling, float floorDepth);

    float GetDepth() const { return mDepth; }
    bool IsSunk() const { return mSunk; }
    bool IsReeling() const { return mReeling; }
    bool IsOnBottom() const { return mOnBottom; }

  private:
    bool mSunk;
    float mDepth = 0.0f;
    bool mReeling = false;
    bool mOnBottom = false;
};

// Step 20c-3c: RF3's fly cast (3.1, recorded 2026-10-07). Holding the rod button raises the power and the line
// length by 0.15/s (to 0.9 and 1.1); the cast's distance follows the hold time. Returns the share of a full fly
// cast (11.9 m recorded) for heldSeconds, from the recorded table, extended linearly to 0 m at 0 s.
float FlyCastDistanceFactor(float heldSeconds);

// Step 21b: the per-catch unlock (docs/DESIGN.md step 21, the user's rule, not RF3's). The categories are
// lure, fly, bait, float, in that order; remaining[c] says category c still has items to give. Returns the
// category for a catch, catchCount = catches counted before this one: the (catchCount mod k)-th of the k
// remaining categories. -1 if none remain.
constexpr int kUnlockCategories = 4;
int UnlockCategory(const bool remaining[kUnlockCategories], uint32_t catchCount);
// Usable first: entry (roll mod count) of usable if it has any, else of others. -1 if both are empty.
int PickUnlockItem(const int* usable, int usableCount, const int* others, int othersCount, uint32_t roll);

// Step 21c: the user's rod conditions (docs/DESIGN.md step 21, docs/UNLOCK-CHOICES.md), counted from the
// catches. Species are RF3's (fishdata.fdt +0x02); rods are RROD numbers.
struct RodProgress {
    uint16_t total = 0;        // step 23d: any fish caught (Lure Rod/Bass: 5)
    uint16_t bass = 0;         // Largemouth or Florida Bass caught
    uint16_t trout = 0;        // Yamame, Splake, Rainbow, Brown, Steelhead Trout, Char, Monster Char
    uint16_t salmon = 0;       // King, Pink, Cherry Salmon, Giant King Salmon
    uint16_t withFlyRod = 0;   // any fish caught with rod 3 (Fly Rod)
    uint16_t withPoleS = 0;    // any fish caught with rod 5 (Stream Pole S)
    bool largemouthOver14in = false; // a Largemouth Bass longer than 355.6 mm
    bool paleChub = false;
    bool bitterlingOver3in = false;  // a Bitterling longer than 76.2 mm
    bool bitterling = false;
};
constexpr int kRodCount = 10;
// Counts one catch: its species, length and the rod it was caught with.
void RodProgressAddCatch(RodProgress& p, int species, float lengthMm, int rod);
// Has rod (0-9) met its condition? Step 23d: rod 0 (Lure Rod/Bass) is earned by any 5 fish; OoT's pole (10) is
// bought: false.
bool RodConditionMet(const RodProgress& p, int rod);

// Tension limit for this fish and tackle (1.4).
float TensionLimit(const FishParams& fish, const TackleParams& tackle);

// Weight in grams for a fish of this length in this size class (4).
// noiseRoll is 0..9 (RF3 draws it at random).
int WeightFromLength(const FishParams& fish, int lengthMm, int noiseRoll);

// How long the STIFFEN turn takes before STOP. RF3 turns the fish model at a per-fish
// rate, so its length varies: 9-16 frames in the recorded fights. 0.2 s (12 frames) is
// their median (RF3-MECHANICS 1.10, DESIGN step 8f). It was 0.5 s, our first guess.
inline constexpr float kStiffenTurnTime = 0.2f;

// AWAY entry: RF3 puts the fish 2 model lengths further out (RF3-MECHANICS 1.10).
// The model length is the fish length x 0.988-1.000; 0.99 is used.
inline constexpr float kAwaySurgeLengths = 2.0f;
inline constexpr float kModelLengthFactor = 0.99f;
// The distance can't run more than 3 model lengths past the fish's actual distance
// (RF3 0x0019CF40; RF3-MECHANICS 1.10, DESIGN step 8g).
inline constexpr float kDistanceLeadLengths = 3.0f;

} // namespace rf3

#endif
