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

    // Every frame: remembered level (2.2) and wary timer.
    void Tick(float meter);
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
    const FishParams& GetFish() const { return *mFish; } // step 14b: the chaser's species values

  private:
    const FishParams* mFish = &kBassNormal;
    ApproachMode mMode = ApproachMode::Normal;
    float mMemory = 0.0f;
    float mHold = 0.0f;
    float mWary = 0.0f;
    float mWaitLeft = 0.0f;
};

// Activity level from the fish's tables and the game conditions (2.4).
int ActivityLevel(const FishParams& fish, const Conditions& conditions);
// Sight range in metres for the default lure (appeal 15/15) (2.4).
float SightRange(const FishParams& fish, int activityLevel);

// --- Cast (section 3.1) -----------------------------------------------------

// RF3 cast power after holding the cast button for heldSeconds: 0.95/s, up to 1.0.
float CastPower(float heldSeconds);
// Our mapping (DESIGN step 11), not RF3's: the share of the host game's full cast
// distance for this power. fullHoldPower is the power reached by holding through the
// host's whole wind-up; minimum 0.15.
float CastDistanceFactor(float power, float fullHoldPower);

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
