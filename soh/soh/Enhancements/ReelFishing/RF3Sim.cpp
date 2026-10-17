#include "RF3Sim.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

// Section numbers in comments refer to docs/RF3-MECHANICS.md.

namespace rf3 {

namespace {

constexpr float kFrameTime = 1.0f / 60.0f;
constexpr float kDeg10 = 0.17453292f;  // 10 degrees in radians
constexpr float kDeg80 = 1.3962635f;   // 80 degrees
constexpr float kDeg90 = 1.5707964f;   // 90 degrees
constexpr float kStickThreshold = 0.5f; // "Back" / "Fwd" (1.2)

// The PS2's main CPU rounds float operations toward zero, so RF3's state timer runs a
// little slow: after 30 frames it reads 0.4999998, not 0.5000002, which decides whether
// a 0.5 s STOP ends on frame 30 or 31 (recorded bit-exact, DESIGN step 8g).
float AddTowardZero(float a, float b) {
    double exact = static_cast<double>(a) + static_cast<double>(b);
    float r = static_cast<float>(exact);
    if (std::fabs(static_cast<double>(r)) > std::fabs(exact)) {
        r = std::nextafter(r, 0.0f);
    }
    return r;
}

bool StickBack(const FrameInput& in) {
    return in.stickY >= kStickThreshold;
}
bool StickFwd(const FrameInput& in) {
    return in.stickY <= -kStickThreshold;
}

} // namespace

// --- Approach (section 2) ---------------------------------------------------

void LureMeter::Update(bool lureInWater, bool moved, bool reelHeld) {
    if (!lureInWater) {
        mValue = 0.0f;
        return;
    }
    if (moved) {
        mValue += reelHeld ? 0.015833333f : -0.008333334f; // +0.95/s or -0.5/s
    } else {
        mValue -= 0.011666667f; // -0.7/s
    }
    mValue = std::clamp(mValue, 0.0f, 1.0f);
}

void ApproachFish::Tick(float meter, RodKind kind, float movedM) {
    mMeter = meter;
    mKind = kind;
    mMoved = movedM;
    // Remembered level (2.2): jump up to the meter and hold for 2 x level seconds,
    // then fade by 1 per second.
    if (mMemory <= meter) {
        mMemory = meter;
        mHold = mMemory * 2.0f;
    } else if (mHold > 0.0f) {
        mHold -= kFrameTime;
    } else {
        mMemory = std::max(mMemory - kFrameTime, 0.0f);
    }
    if (mWary > 0.0f) {
        mWary = std::max(mWary - kFrameTime, 0.0f);
    }
}

bool ApproachFish::IsInterested() const {
    if (mKind == RodKind::Bait) {
        return mMeter < kBaitStillMeter; // a bait interests fish only while it is still (2.9)
    }
    if (mKind == RodKind::Fly) {
        return mMeter < kFlyMaxMeter && mMoved > kFlyMinDrift; // a drifting fly (2.9)
    }
    int level = static_cast<int>(mMemory * 255.0f); // truncated, as RF3 does
    return level >= 1 && level <= mFish->maxAction;
}

bool ApproachFish::TryNotice(const NoticeInput& in) {
    if (mMode != ApproachMode::Normal || !in.lureInWater || mWary > 0.0f || in.otherFishChasing) {
        return false;
    }
    // A lure almost straight behind (within 10 degrees) is only seen when close (2.3).
    constexpr float kBehind = 2.9670596f; // 170 degrees
    bool behind = std::fabs(in.angleFromHeading) > kBehind;
    if (behind && in.horizontalDistance > mFish->sightBaseM) {
        return false;
    }
    if (in.distance > in.sightRange || !IsInterested()) {
        return false;
    }
    mMode = ApproachMode::Chase;
    mWaits = 0;
    mLostInterest = 0.0f;
    return true;
}

ApproachEvent ApproachFish::UpdateChase(const ChaseInput& in, uint32_t randomRoll) {
    mTookBait = false;
    if (mMode == ApproachMode::Normal) {
        return ApproachEvent::None;
    }
    if (!in.lureInWater) {
        mMode = ApproachMode::Normal;
        mWary = 30.0f;
        return ApproachEvent::Fail;
    }
    if (mMode == ApproachMode::Wait) {
        mWaitLeft -= kFrameTime;
        if (mWaitLeft <= 0.0f) {
            mMode = ApproachMode::Chase; // inspect over: chase again, interest re-checked below
        }
        return ApproachEvent::None;
    }
    // CHASE (2.5). With lures, losing interest ends the chase at once (bass patience is 0 s; other species'
    // lure patience is not modelled yet). Bait rods allow kBaitPatience (2.9).
    bool interested = IsInterested();
    if (!interested && mKind != RodKind::Lure && !in.lureNearAngler) { // fly and bait rods: 0.5 s patience
        mLostInterest = AddTowardZero(mLostInterest, kFrameTime); // as the PS2 adds it: 0.5 s = 31 frames
        if (mLostInterest <= kBaitPatience) {
            return ApproachEvent::None; // the fish waits for the bait to be still again
        }
    }
    if (in.lureNearAngler || !interested) {
        mMode = ApproachMode::Normal;
        mWary = 30.0f;
        return ApproachEvent::Fail;
    }
    mLostInterest = 0.0f;
    if (!in.reachedLure) {
        return ApproachEvent::None;
    }
    int roll = static_cast<int>(randomRoll % 101);
    if (mKind == RodKind::Bait) {
        // Bait (2.9, 0x001A0B00): c = record +0x36. Strike after more than c/10 WAITs or on a roll above c;
        // give up (wary 30 s) on a roll below c/4; otherwise WAIT.
        int c = mFish->resistExtra;
        if (mWaits > c / 10 || roll > c) {
            mMode = ApproachMode::Normal;
            return ApproachEvent::Strike;
        }
        if (roll < c / 4) {
            mMode = ApproachMode::Normal;
            mWary = 30.0f;
            mTookBait = true; // step 33a: RF3 takes the bait off the hook here (0x001B38F0)
            return ApproachEvent::Fail;
        }
    } else if (roll < 78) {
        mMode = ApproachMode::Normal;
        return ApproachEvent::Strike;
    }
    // WAIT (2.6): (resistExtra / 100 - r / 10) x 2 + 1 seconds, r = 0..5.
    int r = static_cast<int>((randomRoll / 101) % 6);
    mWaitLeft = (mFish->resistExtra / 100.0f - r / 10.0f) * 2.0f + 1.0f;
    mMode = ApproachMode::Wait;
    mWaits++;
    return ApproachEvent::None;
}

void ApproachFish::SetWary(float seconds) {
    mWary = std::max(mWary, seconds);
    mMode = ApproachMode::Normal;
}

int ActivityLevel(const FishParams& fish, const Conditions& c) {
    int season = std::clamp(c.season, 0, 2);
    int time = std::clamp(c.time, 0, 2);
    int weather = std::clamp(c.weather, 0, 2);
    return fish.activitySeason[season] + fish.activityWeather[weather] + fish.activityTime[time];
}

float SightRange(const FishParams& fish, int activityLevel) {
    float range = fish.sightBaseM + (fish.sightBonus > 0 ? fish.sightBonus * 0.05f : 0.0f);
    if (activityLevel > 0) {
        range *= 1.0f + activityLevel * 0.25f;
    } else if (activityLevel < 0) {
        range /= 1.0f + (-activityLevel) * 0.25f;
    }
    return range; // x lure appeal / 15, which is 15 / 15 for the default lure and bass
}

float SightRangeWithAppeal(const FishParams& fish, int activityLevel, int lureAppeal) {
    return SightRange(fish, activityLevel) * static_cast<float>(std::max(lureAppeal, 0)) / 15.0f;
}

float CastPower(float heldSeconds) {
    return std::clamp(heldSeconds * 0.95f, 0.0f, 1.0f);
}

float CastDistanceFactor(float power, float fullHoldPower) {
    if (fullHoldPower <= 0.0f) {
        return 1.0f;
    }
    return std::clamp(power / fullHoldPower, 0.15f, 1.0f);
}

// --- Lure depth (3.2) --------------------------------------------------------

void LureSim::Land() {
    mMode = LureMode::Rest;
    mDepth = mMove.buoyancy == 0 ? kLureLandingDepthTopwater : kLureLandingDepth;
    mT = 0.0f;
    mVelocity = 0.0f;
    mMomentum = 0.0f;
    mOnBottom = false;
    mAtSurface = mDepth <= 0.0f;
    mDiveStopped = false;
    mTwitchArmed = false;
    mTwitchState = 0;
    mTwitchFrames = 0;
}

// +0x12 (twitch, or reeling with the rod held back): 0 down at the reeling down speed,
// 1 up at the reeling up speed, 2 hold.
float LureSim::ByTwitchResponse() const {
    switch (mMove.twitchResponse) {
        case 0:
            return Speed(0);
        case 1:
            return -Speed(1);
        default:
            return 0.0f;
    }
}

void LureSim::Step(const LureStep& in) {
    const LureMode mode = in.reeling ? LureMode::Reel : LureMode::Rest;
    // t restarts when reeling starts or stops, otherwise it advances before use. A lure that is
    // on the bottom after this frame's move gets its previous t back (0x00203230), so t holds on
    // the bottom, and a restart there only sticks if the lure lifts off (recorded 2026-10-07, 3.2).
    const float tBefore = mT;
    if (mode != mMode) {
        mMode = mode;
        mT = 0.0f;
        if (mode == LureMode::Reel) {
            // Each Cross press sets the twitch detector up afresh (0x002027C0's entry).
            mTwitchArmed = true;
            mTwitchState = 0;
            mTwitchFrames = 0;
        }
    } else {
        mT += kFrameTime;
    }

    // Twitch detector (0x002027C0), only once set up (3.2, quirk). State 0: rod pulled back
    // starts a twitch. State 1: wait for the twitch to end and the rod to be released.
    // State 2: one frame, then back to 0. The state machine sees the previous frame's flag.
    if (mTwitchArmed) {
        if (mTwitchState == 2) {
            mTwitchState = 0;
        } else if (mTwitchState == 1) {
            if (mTwitchFrames == 0 && !in.rodBack) {
                mTwitchState = 2;
            }
        } else if (in.rodBack) {
            mTwitchState = 1;
            mTwitchFrames = kLureTwitchFrames + 1; // counted down below in the same frame
        }
    }
    if (mTwitchFrames > 0) {
        mTwitchFrames--;
    }

    // RF3 applies a downward speed only off the bottom (+0x71) and an upward one only below the
    // surface (+0x70), both as left by the previous frame (0x00201640, 0x002018E0).
    auto gated = [this](float v) {
        return (v > 0.0f && mOnBottom) || (v < 0.0f && mAtSurface) ? 0.0f : v;
    };
    float speed;
    if (mMode == LureMode::Reel) {
        if (in.rodBack) {
            speed = ByTwitchResponse();
        } else if (mMove.speedBase[0] + mMove.speedRate[0] > mMove.speedBase[1] + mMove.speedRate[1]) {
            speed = Speed(0); // RF3 compares base + rate, not the speeds at t
        } else {
            speed = -Speed(1);
        }
        speed = gated(speed);
        mMomentum = speed;
    } else {
        if (IsTwitching()) {
            speed = ByTwitchResponse();
        } else if (mMove.restAction == 0 || mMove.restAction == 3 || mMove.restAction == 5) {
            speed = Speed(2);
        } else {
            speed = -Speed(3);
        }
        speed = gated(speed);
        mMomentum *= 0.95f;
        speed += mMomentum;
    }

    mVelocity = speed;
    mDepth += speed * kFrameTime;
    // [assumed] (DESIGN "Polish later"): the stop holds in both modes, and the momentum keeps
    // building behind it, as RF3's stored velocity does.
    const float diveStop = mMove.buoyancy == 0   ? kLureDiveStopBuoyancy0
                           : mMove.buoyancy == 1 ? kLureDiveStopBuoyancy1
                                                 : -1.0f;
    mDiveStopped = diveStop >= 0.0f && mDepth > diveStop;
    if (mDiveStopped) {
        mDepth = diveStop;
    }
    // The surface and the bottom only stop the position; the stored velocity is kept (RF3's
    // collision code writes the position only).
    mAtSurface = mDepth <= 0.0f;
    if (mAtSurface) {
        mDepth = 0.0f; // RF3's surface probe keeps t at 0 there
        mT = 0.0f;
    }
    // RF3's bottom probe reaches 0.02 m below the lure, which rests 0.01 m off the floor: the lure
    // counts as on the bottom within kLureBottomBand of its resting depth (0x00203230).
    if (mDepth > in.floorDepth) {
        mDepth = in.floorDepth;
    }
    mOnBottom = mDepth >= in.floorDepth - kLureBottomBand;
    if (mOnBottom) {
        mT = tBefore;
    }
}

// --- Fly depth and cast (step 20c-3) -------------------------------------------------

void FlySim::Step(bool reeling, float floorDepth) {
    mReeling = reeling;
    if (!mSunk && !reeling && mDepth > 0.0f) {
        mDepth = std::max(mDepth - kFlyRiseSpeed * kFrameTime, 0.0f);
    }
    if (mDepth > floorDepth) {
        mDepth = floorDepth;
    }
    mOnBottom = mDepth >= floorDepth - kLureBottomBand;
}

int UnlockCategory(const bool remaining[kUnlockCategories], uint32_t catchCount) {
    int left[kUnlockCategories];
    int k = 0;
    for (int c = 0; c < kUnlockCategories; c++) {
        if (remaining[c]) {
            left[k++] = c;
        }
    }
    return (k == 0) ? -1 : left[catchCount % static_cast<uint32_t>(k)];
}

int PickUnlockItem(const int* usable, int usableCount, const int* others, int othersCount, uint32_t roll) {
    if (usableCount > 0) {
        return usable[roll % static_cast<uint32_t>(usableCount)];
    }
    if (othersCount > 0) {
        return others[roll % static_cast<uint32_t>(othersCount)];
    }
    return -1;
}

void RodProgressAddCatch(RodProgress& p, int species, float lengthMm, int rod) {
    // RF3 species ids (fishdata.fdt +0x02): 0 Char, 1 Yamame, 2 Splake, 6 Pale Chub, 7 Bitterling,
    // 8 Largemouth, 9 Florida, 11 Rainbow, 12 Brown, 13 King, 14 Pink, 15 Steelhead, 16 Cherry,
    // 31 Monster Char, 32 Giant King Salmon.
    auto bump = [](uint16_t& n) {
        if (n < 0xFFFF) {
            n++;
        }
    };
    bump(p.total);
    switch (species) {
        case 8:
        case 9:
            bump(p.bass);
            break;
        case 0:
        case 1:
        case 2:
        case 11:
        case 12:
        case 15:
        case 31:
            bump(p.trout);
            break;
        case 13:
        case 14:
        case 16:
        case 32:
            bump(p.salmon);
            break;
        default:
            break;
    }
    if (rod == 3) {
        bump(p.withFlyRod);
    } else if (rod == 5) {
        bump(p.withPoleS);
    }
    if ((species == 8) && (lengthMm > 355.6f)) { // 1'2"
        p.largemouthOver14in = true;
    }
    if (species == 6) {
        p.paleChub = true;
    }
    if (species == 7) {
        p.bitterling = true;
        if (lengthMm > 76.2f) { // 3"
            p.bitterlingOver3in = true;
        }
    }
}

bool RodConditionMet(const RodProgress& p, int rod) {
    switch (rod) {
        case 0: // Lure Rod/Bass: catch any 5 fish (user, 2026-10-08)
            return p.total >= 5;
        case 1: // Lure Rod/Trout: catch 3 bass
            return p.bass >= 3;
        case 2: // Lure Rod/Salmon: catch any 3 trout
            return p.trout >= 3;
        case 3: // Fly Rod: catch any 3 salmon
            return p.salmon >= 3;
        case 4: // Big Game Fly Rod: catch 5 fish with the Fly Rod
            return p.withFlyRod >= 5;
        case 5: // Stream Pole S: a Largemouth Bass over 1'2"
            return p.largemouthOver14in;
        case 6: // Stream Pole L: catch 5 fish with Stream Pole S
            return p.withPoleS >= 5;
        case 7: // Crucian Carp Rod: catch a Pale Chub
            return p.paleChub;
        case 8: // All-Purpose Rod L: a Bitterling over 3"
            return p.bitterlingOver3in;
        case 9: // Bitterling Rod: catch a Bitterling
            return p.bitterling;
        default:
            return false;
    }
}

// --- Step 43a: lure unlock conditions --------------------------------------------------------------------------------

namespace {

constexpr uint64_t Sp(std::initializer_list<int> ids) {
    uint64_t m = 0;
    for (int i : ids) {
        m |= uint64_t{ 1 } << i;
    }
    return m;
}
struct LureBits {
    uint64_t lo = 0;
    uint64_t hi = 0;
};
constexpr LureBits Lu(std::initializer_list<int> ids) {
    LureBits b;
    for (int i : ids) {
        if (i < 64) {
            b.lo |= uint64_t{ 1 } << i;
        } else {
            b.hi |= uint64_t{ 1 } << (i - 64);
        }
    }
    return b;
}

// Species: 0 Char, 1 Yamame, 2 Splake, 8 Largemouth, 9 Florida, 10 Bluegill, 11 Rainbow, 12 Brown, 13 King Salmon,
// 14 Pink Salmon, 15 Steelhead, 31 Monster Char, 32 Giant King Salmon, 33 Hylian Bass.
constexpr uint64_t kLmb = Sp({ 8 });
constexpr uint64_t kFla = Sp({ 9 });
constexpr uint64_t kGill = Sp({ 10 });
constexpr uint64_t kRainbow = Sp({ 11 });
constexpr uint64_t kBrown = Sp({ 12 });
constexpr uint64_t kTrout = Sp({ 0, 1, 2, 11, 12, 15, 31 }); // the rod conditions' "trout"
constexpr uint64_t kTroutSix = Sp({ 0, 1, 2, 11, 12, 15 });  // "every trout species"
constexpr uint64_t kSalmon = Sp({ 13, 14, 32 });             // King and Pink (and Giant King) Salmon
constexpr uint64_t kBassRodFish = Sp({ 8, 9, 10, 11, 12 });

// Lure families ("with Suspended Vibrations" etc.).
constexpr LureBits kSuspVib = Lu({ 48, 49, 50 });
constexpr LureBits kFloatMinnow = Lu({ 36, 37, 38 });
constexpr LureBits kFVib = Lu({ 45, 46, 47 });
constexpr LureBits kShallowRunner = Lu({ 54, 55, 56 });
constexpr LureBits kSpinnerBait = Lu({ 69, 70, 71, 72 });
constexpr LureBits kPopper = Lu({ 26, 27, 28, 29 });

LureCondition C(int lure, UnlockGoal goal, int n) {
    LureCondition c;
    c.slot = static_cast<int16_t>(lure);
    c.goal = goal;
    c.n = static_cast<uint16_t>(n);
    return c;
}
LureCondition Sp1(LureCondition c, uint64_t species) {
    c.species = species;
    return c;
}
LureCondition Mm(LureCondition c, int mm) {
    c.minMm = static_cast<uint16_t>(mm);
    return c;
}
LureCondition Big(LureCondition c) {
    c.lunker = true;
    return c;
}
LureCondition With(LureCondition c, LureBits b) {
    c.luresLo = b.lo;
    c.luresHi = b.hi;
    return c;
}
LureCondition Type(LureCondition c, int type) {
    c.lureType = static_cast<int8_t>(type);
    return c;
}
LureCondition Rod(LureCondition c, int rod) {
    c.rods = static_cast<uint16_t>(1u << rod);
    return c;
}
// Step 44a: flies, baits and floats.
LureCondition F(int fly, UnlockGoal goal, int n) {
    return C(kFlySlot0 + fly, goal, n);
}
LureCondition B(int bait, UnlockGoal goal, int n) {
    return C(kBaitSlot0 + bait, goal, n);
}
LureCondition Fl(int fl, UnlockGoal goal, int n) {
    return C(kFloatSlot0 + fl, goal, n);
}
LureCondition Rods(LureCondition c, uint16_t rods) {
    c.rods = rods;
    return c;
}
LureCondition FlyT(LureCondition c, uint8_t types) {
    c.flyTypes = types;
    return c;
}
LureCondition Flies(LureCondition c, std::initializer_list<int> ids) {
    c.flies = Sp(ids);
    return c;
}
LureCondition Baits(LureCondition c, std::initializer_list<int> ids) {
    c.baits = static_cast<uint16_t>(Sp(ids));
    return c;
}
LureCondition Floats(LureCondition c, std::initializer_list<int> ids) {
    c.floats = static_cast<uint8_t>(Sp(ids));
    return c;
}
constexpr uint16_t kStreamPoles = (1u << 5) | (1u << 6); // "with a stream pole": Stream Pole S or L
LureCondition At(LureCondition c, UnlockTime t) {
    c.time = t;
    return c;
}
LureCondition Kg(LureCondition c, int grams) {
    c.minWeightG = static_cast<uint32_t>(grams);
    return c;
}

using G = UnlockGoal;

// docs/LURE-UNLOCK-CHOICES.md, in its order. Sizes in mm (the file gives inches first).
const LureCondition* Table(int* count) {
    static LureCondition t[kUnlockSlots];
    static int n = 0;
    if (n == 0) {
        auto add = [](LureCondition c) { t[n++] = c; };
        // Tackle Box Bass (48 comes with Lure Rod/Bass)
        add(C(36, G::Count, 1));
        add(C(45, G::Count, 3));
        add(Sp1(C(54, G::Count, 2), kGill));
        add(With(C(49, G::Count, 5), Lu({ 48 })));
        add(C(39, G::Count, 10));
        add(C(40, G::Species, 3));
        { LureCondition c = C(73, G::Count, 1); c.outsidePond = true; add(c); }
        add(Sp1(C(57, G::Count, 5), kLmb));
        add(With(C(37, G::Count, 5), Lu({ 36 })));
        add(With(Sp1(C(38, G::Count, 1), kRainbow), kFloatMinnow));
        add(C(46, G::Streak, 3));
        add(With(Mm(Sp1(C(47, G::Count, 1), kLmb), 305), kFVib));
        add(Mm(Sp1(C(55, G::Count, 1), kGill), 229));
        add(With(C(56, G::Count, 10), kShallowRunner));
        add(C(74, G::Places, 3));
        add(With(C(50, G::Count, 10), kSuspVib));
        add(Mm(Sp1(C(41, G::Count, 1), kLmb), 254));
        add(At(C(58, G::Count, 1), kNight));
        add(C(59, G::Visit, 5));
        add(C(69, G::Count, 15));
        add(At(C(70, G::Count, 1), kDawnDusk));
        add(C(71, G::Streak, 5));
        add(With(C(72, G::Count, 15), kSpinnerBait));
        add(Sp1(C(81, G::Count, 1), kFla));
        add(Mm(Sp1(C(82, G::Count, 1), kFla), 406));
        { LureCondition c = C(26, G::Count, 1); c.biteWithin = 5.0f; add(c); }
        add(C(30, G::Count, 20));
        add(Mm(Sp1(C(31, G::Count, 1), kGill), 254));
        add(Rod(C(33, G::Species, 4), 0));
        add(With(C(27, G::Count, 5), Lu({ 26 })));
        add(With(Mm(Sp1(C(28, G::Count, 1), kLmb), 381), kPopper));
        add(With(Sp1(C(29, G::Streak, 3), kLmb), kPopper));
        add(At(Type(C(32, G::Count, 5), 0), kNight));
        add(Sp1(C(34, G::Count, 1), kBrown));
        add(Type(Mm(Sp1(C(35, G::Count, 1), kRainbow), 305), 0));
        add(Big(C(42, G::Count, 1)));
        add(Type(C(43, G::Count, 5), 3));
        add(Big(Sp1(C(44, G::Count, 1), kRainbow)));
        add(C(51, G::Count, 30));
        add(Big(Sp1(C(52, G::Count, 1), kLmb)));
        add(C(53, G::Streak, 8));
        add(Mm(Sp1(C(60, G::Count, 1), kBrown), 508));
        add(Type(C(61, G::Count, 10), 3));
        add(Sp1(C(75, G::Count, 20), kLmb));
        add(Kg(C(79, G::Count, 1), 2000));
        add(Mm(Sp1(C(62, G::Count, 1), kLmb), 508));
        add(Big(C(76, G::Count, 5)));
        { LureCondition c = Kg(C(80, G::Count, 1), 1000); c.thinLine = true; add(c); }
        add(C(63, G::Count, 50));
        add(Type(C(64, G::Count, 5), 4));
        add(Big(Sp1(C(66, G::Count, 1), kFla)));
        add(Sp1(C(67, G::Count, 10), kFla));
        { LureCondition c = Big(C(77, G::Count, 1)); c.landWithin = 30.0f; add(c); }
        add(C(65, G::Streak, 10));
        add(Mm(Sp1(C(68, G::Count, 1), kFla), 610));
        { LureCondition c = C(78, G::WeightTotal, 1); c.totalG = 20000; add(c); }
        add(C(83, G::Count, 75));
        add(Sp1(C(84, G::SpeciesAll, 5), kBassRodFish));
        add(Big(Sp1(C(85, G::Count, 1), kBrown)));
        add(Big(C(86, G::Visit, 3)));
        add(Mm(Sp1(C(87, G::Count, 1), kLmb), 610));
        add(C(88, G::Count, 100));
        // Tackle Box Trout (0 comes with Lure Rod/Trout)
        add(Rod(C(1, G::Count, 5), 1));
        add(Sp1(C(7, G::Count, 1), Sp({ 1 })));
        add(Sp1(C(8, G::Count, 1), Sp({ 0 })));
        add(Sp1(C(9, G::Count, 1), Sp({ 2 })));
        add(Sp1(C(2, G::Species, 3), kTroutSix));
        add(Sp1(C(10, G::Count, 1), Sp({ 15 })));
        add(Sp1(C(3, G::Count, 15), kTrout));
        add(Sp1(C(5, G::Streak, 5), kTrout));
        add(Big(Sp1(C(4, G::Count, 1), Sp({ 0 }))));
        add(Sp1(C(6, G::SpeciesAll, 6), kTroutSix));
        // Tackle Box Salmon (11 comes with Lure Rod/Salmon)
        add(Sp1(C(12, G::Count, 3), Sp({ 13, 14 })));
        add(Rod(Sp1(C(23, G::Count, 1), Sp({ 15 })), 2));
        add(At(Sp1(C(20, G::Count, 1), kSalmon), kDawnDusk));
        add(Mm(Sp1(C(13, G::Count, 1), Sp({ 14 })), 762));
        add(Sp1(C(17, G::Count, 1), Sp({ 13 })));
        add(Sp1(C(24, G::Visit, 3), kSalmon));
        add(With(C(21, G::Count, 5), Lu({ 20 })));
        add(Sp1(C(14, G::Count, 10), kSalmon));
        add(Rod(C(25, G::Streak, 5), 2));
        add(Mm(Sp1(C(18, G::Count, 1), Sp({ 13 })), 1016));
        add(Big(Sp1(C(19, G::Count, 1), Sp({ 15 }))));
        add(Kg(C(15, G::Count, 1), 8000));
        add(Mm(Sp1(C(16, G::Count, 1), Sp({ 13 })), 1270));
        add(Sp1(C(22, G::Count, 25), kSalmon));
        // Fishing Pole (91 comes with it): the user's sinking lure, a Hylian Bass in the top 10 % of its range
        // (251-700 mm, RF3SizeMap.h): 655 mm or more.
        add(Mm(Sp1(C(92, G::Count, 1), Sp({ 33 })), 655));
        // Step 44a: docs/FLY-BAIT-FLOAT-UNLOCK-CHOICES.md, in its order. Flies (0 comes with the Fly Rod).
        add(FlyT(F(13, G::Count, 3), kFlyDry));
        add(FlyT(Sp1(F(31, G::Count, 1), kRainbow), kFlyDry));
        add(FlyT(Sp1(F(2, G::Count, 1), kGill), kAnyFly));
        add(Rod(Sp1(F(9, G::Count, 5), kTrout), 3));
        add(FlyT(Sp1(F(26, G::Count, 1), Sp({ 1 })), kAnyFly));
        add(FlyT(Sp1(F(3, G::Count, 1), Sp({ 0 })), kAnyFly));
        add(FlyT(Sp1(F(4, G::Count, 1), Sp({ 2 })), kAnyFly));
        add(FlyT(Sp1(F(12, G::Count, 1), Sp({ 6 })), kAnyFly));
        add(At(FlyT(F(10, G::Count, 1), kAnyFly), kDawnDusk));
        add(At(FlyT(F(21, G::Count, 1), kFlyDry), kNight));
        add(FlyT(Mm(Sp1(F(24, G::Count, 1), kGill), 229), kFlyDry));
        add(FlyT(Sp1(F(38, G::Count, 1), kLmb), kFlyDry));
        add(FlyT(Sp1(F(16, G::Count, 1), kBrown), kAnyFly));
        add(Rod(F(34, G::Count, 10), 3));
        add(Flies(F(8, G::Count, 3), { 9 }));
        add(FlyT(Sp1(F(5, G::Count, 1), Sp({ 4 })), kAnyFly));
        add(FlyT(Sp1(F(1, G::Count, 1), Sp({ 14 })), kAnyFly));
        add(FlyT(F(14, G::Count, 3), kSunkenFly));
        add(Flies(F(7, G::Count, 3), { 8 }));
        add(Flies(F(6, G::Count, 3), { 7 }));
        add(Flies(F(25, G::Count, 5), { 16 }));
        add(FlyT(Sp1(F(18, G::Streak, 5), kTrout), kFlyDry));
        add(FlyT(Sp1(F(19, G::Visit, 3), kTrout), kFlyDry));
        add(FlyT(Sp1(F(39, G::Count, 1), Sp({ 15 })), kFlyDry));
        add(Flies(F(35, G::Count, 5), { 34 }));
        add(FlyT(Sp1(F(20, G::Species, 3), kTroutSix), kFlyWet));
        add(FlyT(Mm(Sp1(F(22, G::Count, 1), Sp({ 0 })), 305), kFlyNymph));
        add(FlyT(F(30, G::Count, 5), kFlyNymph));
        add(Flies(F(15, G::Count, 5), { 5 }));
        add(FlyT(Mm(Sp1(F(29, G::Count, 1), kLmb), 381), kAnyFly));
        add(FlyT(F(32, G::Count, 5), kFlyStreamer));
        add(Rod(F(28, G::Count, 5), 4));
        add(FlyT(Sp1(F(37, G::Count, 1), Sp({ 13 })), kAnyFly));
        add(Big(Flies(Sp1(F(17, G::Count, 1), kTrout), { 6 })));
        add(FlyT(Mm(Sp1(F(27, G::Count, 1), kBrown), 508), kAnyFly));
        add(Big(FlyT(F(33, G::Count, 1), kAnyFly)));
        add(Big(FlyT(Sp1(F(36, G::Count, 1), Sp({ 15 })), kAnyFly))); // "32 in or more" read as the Lunker size
        add(Rod(Mm(Sp1(F(23, G::Count, 1), Sp({ 13 })), 1016), 4));
        add(FlyT(Sp1(F(11, G::Count, 10), kSalmon), kAnyFly));
        // Baits (0, 6, 8, 7 come with Stream Pole S, the Crucian Carp Rod, All-Purpose Rod L, the Bitterling Rod).
        add(Baits(B(1, G::Count, 5), { 0 }));
        add(Rods(Sp1(B(2, G::Count, 1), Sp({ 1 })), kStreamPoles));
        add(Rods(B(3, G::Species, 3), kStreamPoles));
        add(Rods(Sp1(B(5, G::Count, 1), kLmb), kStreamPoles));
        add(Rods(Sp1(B(4, G::Count, 1), Sp({ 16 })), kStreamPoles));
        add(Sp1(B(11, G::Count, 3), Sp({ 7 })));
        add(Sp1(B(9, G::Count, 1), Sp({ 21 })));
        add(Sp1(B(10, G::Count, 1), Sp({ 17 })));
        // Floats (2, 0, 4 come with Stream Pole S or the Bitterling Rod, the Crucian Carp Rod, All-Purpose Rod L).
        add(Floats(Fl(3, G::Count, 5), { 2 }));
        add(Rods(Fl(5, G::Streak, 3), kStreamPoles));
        add(Rod(Sp1(Fl(1, G::Count, 1), Sp({ 5 })), 7));
    }
    *count = n;
    return t;
}

int PopCount(uint64_t v) {
    int c = 0;
    for (; v; v &= v - 1) {
        c++;
    }
    return c;
}

} // namespace

const LureCondition* UnlockConditionFor(int slot) {
    int n = 0;
    const LureCondition* t = Table(&n);
    for (int i = 0; i < n; i++) {
        if (t[i].slot == slot) {
            return &t[i];
        }
    }
    return nullptr;
}

const LureCondition* LureConditionFor(int lure) {
    return (lure >= 0 && lure < kLureCount) ? UnlockConditionFor(lure) : nullptr;
}

int UnlockConditionCount() {
    int n = 0;
    Table(&n);
    return n;
}

int LureConditionCount() {
    int n = 0;
    const LureCondition* t = Table(&n);
    int lures = 0;
    for (int i = 0; i < n; i++) {
        lures += t[i].slot < kLureCount ? 1 : 0;
    }
    return lures;
}

bool UnlockMatches(const LureCondition& c, const UnlockCatch& k) {
    if (c.species != 0 && (k.species < 0 || k.species >= 64 || !((c.species >> k.species) & 1))) {
        return false;
    }
    if (k.lengthMm < c.minMm || static_cast<uint32_t>(std::max(0, k.weightG)) < c.minWeightG) {
        return false;
    }
    if (c.lunker && k.sizeClass != 'b' && k.sizeClass != 'l') {
        return false;
    }
    if (c.rods != 0 && (k.rod < 0 || k.rod >= 16 || !((c.rods >> k.rod) & 1))) {
        return false;
    }
    // Step 44a: the fly, its type, the bait and the float on the line.
    if (c.flies != 0 && (k.fly < 0 || k.fly >= 64 || !((c.flies >> k.fly) & 1))) {
        return false;
    }
    if (c.flyTypes != 0 && (k.fly < 0 || k.flyType < 0 || k.flyType > 3 || !((c.flyTypes >> k.flyType) & 1))) {
        return false;
    }
    if (c.baits != 0 && (k.bait < 0 || k.bait >= 16 || !((c.baits >> k.bait) & 1))) {
        return false;
    }
    if (c.floats != 0 && (k.floatIdx < 0 || k.floatIdx >= 8 || !((c.floats >> k.floatIdx) & 1))) {
        return false;
    }
    if (c.luresLo != 0 || c.luresHi != 0) {
        if (k.lure < 0 || k.lure >= kLureCount) {
            return false;
        }
        const uint64_t bits = k.lure < 64 ? c.luresLo : c.luresHi;
        if (!((bits >> (k.lure % 64)) & 1)) {
            return false;
        }
    }
    if (c.lureType >= 0 && (k.lure < 0 || k.lureType != c.lureType)) {
        return false;
    }
    const float h = k.dayHours;
    if (c.time == kNight && !(h >= 18.0f || h < 6.0f)) {
        return false;
    }
    if (c.time == kDawnDusk && !((h >= 5.0f && h < 7.0f) || (h >= 17.0f && h < 19.0f))) {
        return false;
    }
    if (c.outsidePond && k.pond) {
        return false;
    }
    if (c.thinLine && k.lineCell != 0) {
        return false;
    }
    if (c.biteWithin > 0.0f && !(k.biteSeconds >= 0.0f && k.biteSeconds <= c.biteWithin)) {
        return false;
    }
    if (c.landWithin > 0.0f && !(k.landSeconds >= 0.0f && k.landSeconds <= c.landWithin)) {
        return false;
    }
    return true;
}

void UnlockAddCatch(UnlockProgress& p, const UnlockCatch& k) {
    int n = 0;
    const LureCondition* t = Table(&n);
    for (int i = 0; i < n; i++) {
        const LureCondition& c = t[i];
        if (!UnlockMatches(c, k)) {
            continue;
        }
        const int l = c.slot;
        if (p.count[l] < 0xFFFF) {
            p.count[l]++;
        }
        if ((c.goal == UnlockGoal::Species || c.goal == UnlockGoal::SpeciesAll) && k.species >= 0 && k.species < 64) {
            p.setLo[l] |= uint64_t{ 1 } << k.species;
        }
        if (c.goal == UnlockGoal::Places && k.scene >= 0 && k.scene < 128) {
            if (k.scene < 64) {
                p.setLo[l] |= uint64_t{ 1 } << k.scene;
            } else {
                p.setHi[l] |= uint64_t{ 1 } << (k.scene - 64);
            }
        }
        if (c.goal == UnlockGoal::WeightTotal) {
            p.sumG[l] += static_cast<uint32_t>(std::max(0, k.weightG));
        }
    }
}

void UnlockFishLost(UnlockProgress& p) {
    int n = 0;
    const LureCondition* t = Table(&n);
    for (int i = 0; i < n; i++) {
        if (t[i].goal == UnlockGoal::Streak) {
            p.count[t[i].slot] = 0;
        }
    }
}

void UnlockNewVisit(UnlockProgress& p) {
    int n = 0;
    const LureCondition* t = Table(&n);
    for (int i = 0; i < n; i++) {
        if (t[i].goal == UnlockGoal::Visit) {
            p.count[t[i].slot] = 0;
        }
    }
}

bool UnlockMet(const UnlockProgress& p, int slot) {
    const LureCondition* c = UnlockConditionFor(slot);
    if (c == nullptr) {
        return false;
    }
    const int l = c->slot;
    switch (c->goal) {
        case UnlockGoal::Count:
        case UnlockGoal::Streak:
        case UnlockGoal::Visit:
            return p.count[l] >= c->n;
        case UnlockGoal::Species:
            return PopCount(p.setLo[l]) >= c->n;
        case UnlockGoal::Places:
            return PopCount(p.setLo[l]) + PopCount(p.setHi[l]) >= c->n;
        case UnlockGoal::SpeciesAll:
            return (p.setLo[l] & c->species) == c->species;
        case UnlockGoal::WeightTotal:
            return p.sumG[l] >= c->totalG;
    }
    return false;
}

float FlyCastDistanceFactor(float heldSeconds) {
    // Recorded hold (s) -> distance (m), RF3-MECHANICS 2.9 "Fly recording"; (0, 0) added (user, 2026-10-07).
    static constexpr float kTable[][2] = { { 0.0f, 0.0f },   { 2.72f, 3.18f },  { 3.73f, 4.37f },
                                           { 4.75f, 5.98f }, { 5.78f, 10.34f }, { 6.0f, 11.9f } };
    constexpr float kFull = 11.9f;
    constexpr int kRows = sizeof(kTable) / sizeof(kTable[0]);
    float t = std::max(heldSeconds, 0.0f);
    if (t >= kTable[kRows - 1][0]) {
        return 1.0f;
    }
    for (int i = 1; i < kRows; i++) {
        if (t <= kTable[i][0]) {
            float f = (t - kTable[i - 1][0]) / (kTable[i][0] - kTable[i - 1][0]);
            return (kTable[i - 1][1] + f * (kTable[i][1] - kTable[i - 1][1])) / kFull;
        }
    }
    return 1.0f;
}

// --- Bait depth (step 20b-5) ----------------------------------------------------

void BaitSim::Land() {
    mDepth = std::min(kBaitLandingDepth, mTarget);
    mReeling = false;
    mOnBottom = false;
}

void BaitSim::Step(bool reeling, float floorDepth) {
    mReeling = reeling;
    if (!reeling && mDepth < mTarget) {
        mDepth = std::min(mDepth + kBaitSinkSpeed * kFrameTime, mTarget);
    }
    if (mDepth > floorDepth) {
        mDepth = floorDepth;
    }
    mOnBottom = mDepth >= floorDepth - kLureBottomBand;
}

float TensionLimit(const FishParams& fish, const TackleParams& tackle) {
    int step = std::clamp(tackle.lineStep, 0, 4);
    return static_cast<float>(tackle.tensionLimitBase[step] + fish.tensionBonus);
}

int WeightFromLength(const FishParams& fish, int lengthMm, int noiseRoll) {
    float noise = static_cast<float>(noiseRoll - 5) / 100.0f;
    float fraction = static_cast<float>(lengthMm - fish.lengthMinMm) / static_cast<float>(fish.lengthMaxMm) + noise;
    // Truncate like RF3's float-to-int conversion, then clamp (4).
    int grams = static_cast<int>(fish.weightMinG + fraction * (fish.weightMaxG - fish.weightMinG));
    return std::clamp(grams, static_cast<int>(fish.weightMinG), static_cast<int>(fish.weightMaxG));
}

FightSim::FightSim(const FishParams& fish, const TackleParams& tackle, uint32_t seed)
    : mFish(fish), mTackle(tackle), mRng(seed != 0 ? seed : 1) {
    mTensionLimit = TensionLimit(fish, tackle);
}

uint32_t FightSim::NextRandom() {
    // xorshift32: deterministic, so harness runs repeat exactly.
    mRng ^= mRng << 13;
    mRng ^= mRng >> 17;
    mRng ^= mRng << 5;
    return mRng;
}

void FightSim::StartStrike() {
    mOutcome = Outcome::None;
    mHooked = false;
    mLastRun = Mode::None;
    mHasPending = false;
    mHp = 100.0f;
    mHook = 0.0f;
    mTension = 0.0f;
    mMode = Mode::Hit;
    mStateTime = 0.0f;
    mArmJerk = false;
    mArmReel = false;
}

void FightSim::Finish(Mode mode, Outcome outcome) {
    mMode = mode;
    mOutcome = outcome;
    mHasPending = false;
}

// --- Value updates (1.3) ---------------------------------------------------

void FightSim::AddHp(float rate) {
    if (rate == 0.0f) {
        return;
    }
    mHp = std::clamp(mHp + rate / 60.0f, 30.0f, 100.0f);
}

void FightSim::AddHook(float rate) {
    if (rate == 0.0f) {
        return;
    }
    mHook = std::clamp(mHook + rate * mFish.hookFactor / 60.0f, 0.0f, 100.0f);
}

void FightSim::AddTension(float rate) {
    if (rate == 0.0f) {
        return;
    }
    float factor = 1.0f;
    if (rate > 0.0f) {
        // Rises slow down near the limit: sin(10..90 degrees).
        float theta = (1.0f - mTension / mTensionLimit) * kDeg80 + kDeg10;
        factor = std::sin(std::clamp(theta, kDeg10, kDeg90));
    }
    mTension = std::clamp(mTension + (mFish.power / 60.0f) * rate * factor, 0.0f, mTensionLimit);
}

void FightSim::AddDistance(float rate, const FrameInput& in) {
    if (rate == 0.0f) {
        return;
    }
    float step;
    if (rate > 0.0f) {
        // Fish gaining line, per frame.
        step = (mFish.power / 15.0f) * 0.05f * (rate / 4.0f) * mHp * 0.01f;
        step = std::max(step, 0.01f);
    } else {
        // Angler gaining line, per frame.
        step = (mTackle.rodValue / 15.0f) * 0.025f * (rate / 4.0f) * (1.0f - mHp * 0.01f);
        step = std::min(step, -0.005f);
    }
    mDistance = std::clamp(mDistance + step, 0.0f, std::max(in.distanceCap, 0.0f));
    // RF3 keeps the distance within 3 model lengths of where the fish really is (DESIGN step 8g).
    if (mBodyLength > 0.0f) {
        mDistance = std::min(mDistance, in.fishDistance + kDistanceLeadLengths * kModelLengthFactor * mBodyLength);
    }
}

// --- State entry (1.5) -----------------------------------------------------

void FightSim::Enter(Mode mode, const FrameInput& in) {
    mMode = mode;
    mStateTime = 0.0f;
    mHasPending = false;
    switch (mode) {
        case Mode::Stop: {
            mDistance = in.fishDistance;
            mDuration = mFish.stopDuration;
            float open = mDuration * 0.1f;
            mStopWindowEnd = open + (1.0f - mHp / 100.0f) * (mDuration - open);
            mStopExpectReel = (mLastRun == Mode::Away);
            break;
        }
        case Mode::Struggle:
            mDistance = in.fishDistance;
            mDuration = mFish.struggleDuration * ((mHp / 100.0f) / 4.0f + 0.75f);
            mShakeTimer = 0.0f;
            break;
        case Mode::Away:
            // RF3 starts the run 2 model lengths further out (DESIGN step 8f).
            mDistance = in.fishDistance + kAwaySurgeLengths * kModelLengthFactor * mBodyLength;
            mDuration = mFish.awayDuration * ((mHp / 100.0f) / 4.0f + 0.75f);
            break;
        case Mode::Follow:
            mDistance = in.fishDistance;
            mDuration = mFish.followDuration * (((100.0f - mHp) / 100.0f) / 4.0f + 0.75f);
            break;
        case Mode::Stiffen:
            mDistance = in.fishDistance;
            mDuration = kStiffenTurnTime;
            break;
        case Mode::Resist:
            mDistance = in.fishDistance;
            mDuration = 0.5f + mFish.resistExtra * 0.01f;
            break;
        default:
            mDuration = 0.0f;
            break;
    }
}

// --- Per-frame update -------------------------------------------------------

void FightSim::Update(const FrameInput& in) {
    if (mMode == Mode::None || mOutcome != Outcome::None) {
        return;
    }

    // RF3's frame order (DESIGN step 8g): a state is entered on the frame its handler
    // chooses it, with the timer at 0; its own handler first runs on the next frame.
    // A state set from outside (Resync, StartFight) gets an entry-only frame.
    if (mHasPending) {
        Enter(mPending, in);
        return;
    }

    // 1. State timer.
    mStateTime = AddTowardZero(mStateTime, kFrameTime);

    // 2. State handler.
    Mode next = Mode::None;
    switch (mMode) {
        case Mode::Hit:
            next = UpdateHit(in);
            break;
        case Mode::Struggle:
            next = UpdateStruggle(in);
            break;
        case Mode::Away:
            next = UpdateAway(in);
            break;
        case Mode::Follow:
            next = UpdateFollow(in);
            break;
        case Mode::Stiffen:
            next = UpdateStiffen(in);
            break;
        case Mode::Stop:
            next = UpdateStop(in);
            break;
        case Mode::Resist:
            next = UpdateResist(in);
            break;
        default:
            break;
    }
    if (mOutcome != Outcome::None) {
        return;
    }

    // 3. Base drain every frame while hooked (1.3).
    if (mHooked) {
        AddHp(-0.5f);
        AddHook(-0.5f);

        // 4. Outcome check, in RF3's order (1.1), after this frame's changes.
        if (mDistance <= in.landingDistance) {
            Finish(Mode::Catch, Outcome::Caught);
            return;
        }
        if (mTension >= mTensionLimit) {
            Finish(Mode::LineBreak, Outcome::LineBreak);
            return;
        }
        if (mHook <= 0.0f) {
            Finish(Mode::HookOver, Outcome::HookOut);
            return;
        }
    }

    // 5. Enter the chosen state now.
    if (next != Mode::None) {
        Enter(next, in);
    }
}

// HIT: hook-set timing (1.6).
Mode FightSim::UpdateHit(const FrameInput& in) {
    float t = mStateTime;
    if (t > mFish.hookGiveUpTime) {
        Finish(Mode::HookOver, Outcome::Missed);
        return Mode::None;
    }

    // An action counts only after returning to neutral since the strike.
    if (!mArmJerk && !(in.stickY > kStickThreshold)) {
        mArmJerk = true;
    }
    if (!mArmReel && !in.reel) {
        mArmReel = true;
    }
    bool jerk = mArmJerk && in.stickY > kStickThreshold;
    bool press = mArmReel && in.reel;
    if (!jerk && !press) {
        return Mode::None;
    }

    float start = mFish.hookWindowStart;
    float end = mFish.hookWindowEnd;
    if (t < start || t > end) {
        Finish(Mode::HookOver, Outcome::Missed);
        return Mode::None;
    }

    float length = end - start;
    float a = ((t - start) / length) * 2.0f;
    if (a > 1.0f) {
        a = 1.0f - (a - 1.0f);
    }
    float b = (jerk ? 0.8f : 0.0f) + (press ? 0.2f : 0.0f);
    mHookSetTiming = a;
    mHookSetStrength = b;

    float centre = start + length / 2.0f;
    float band = (mFish.perfectBand / 2.0f) * length;
    bool inBand = (t >= centre - band) && (t <= centre + band);
    bool place = (mFish.flags & 2) != 0 || (in.lureDepth < 0.4f && in.fishDistance < 5.0f);
    if (inBand && place) {
        mHooked = true;
        mHp = 100.0f - static_cast<float>((NextRandom() % 5) * 5);
        Finish(Mode::Bukkonuki, Outcome::Bukkonuki);
        return Mode::None;
    }

    mHooked = true;
    mLastRun = Mode::None;
    mHp = std::clamp(100.0f - a * 25.0f, 30.0f, 100.0f);
    mHook = std::clamp((b + a) * 50.0f, 0.0f, 100.0f);
    mDistance = in.fishDistance;
    mTension = 0.0f;
    return Mode::Struggle;
}

void FightSim::StartFight(float timingA, float strengthB, float fishDistance) {
    StartStrike();
    float a = std::clamp(timingA, 0.0f, 1.0f);
    float b = std::clamp(strengthB, 0.0f, 1.0f);
    mHookSetTiming = a;
    mHookSetStrength = b;
    mHooked = true;
    mHp = std::clamp(100.0f - a * 25.0f, 30.0f, 100.0f);
    mHook = std::clamp((b + a) * 50.0f, 0.0f, 100.0f);
    mDistance = fishDistance;
    mPending = Mode::Struggle;
    mHasPending = true;
}

void FightSim::Resync(Mode mode, Mode lastRun, float hp, float hook, float tension, float distance) {
    mOutcome = Outcome::None;
    mHooked = true;
    mMode = mode;
    mLastRun = lastRun;
    mHp = hp;
    mHook = hook;
    mTension = tension;
    mDistance = distance;
    mPending = mode;
    mHasPending = true;
}

// STRUGGLE (1.5).
Mode FightSim::UpdateStruggle(const FrameInput& in) {
    float hp = 0.0f, tension = 0.0f, hook = 0.0f, dist = 0.0f;
    bool idle = !in.reel;
    if (in.reel) {
        hp = -2.0f;
        tension = 4.0f;
        hook = -1.0f;
    }
    if (StickFwd(in)) {
        idle = false;
        tension -= 4.0f;
        hook -= 3.0f;
        dist = 2.0f;
    } else if (StickBack(in)) {
        idle = false;
        tension += 1.0f;
        hook += 1.0f;
    } else {
        tension += 1.0f;
        hook -= 1.0f;
    }
    if (idle) {
        tension = 2.0f;
        hook = -0.5f;
        dist = 1.0f;
    }
    AddHp(hp);
    AddTension(tension);
    AddDistance(dist, in);
    AddHook(hook);

    // Side-to-side shake every 0.25 s (visual only).
    mShakeTimer -= kFrameTime;
    if (mShakeTimer <= 0.0f) {
        mShakeTimer = 0.25f;
        mShakeSide ^= 1;
    }

    return (mStateTime > mDuration) ? Mode::Stiffen : Mode::None;
}

// AWAY (1.5). No extra stamina change in this state.
Mode FightSim::UpdateAway(const FrameInput& in) {
    float tension = 0.0f, hook = 0.0f, dist = 0.0f;
    bool idle = !in.reel;
    if (in.reel) {
        tension = 3.0f;
        hook = 1.0f;
    }
    if (StickFwd(in)) {
        idle = false;
        tension -= 4.0f;
        hook -= 1.0f;
        dist = 3.0f;
    } else if (StickBack(in)) {
        idle = false;
        tension += 3.0f;
        hook += 1.0f;
        if (in.reel) {
            dist = -0.5f;
        }
    } else {
        hook -= 0.5f;
    }
    if (idle) {
        tension = 1.0f;
        hook = -0.5f;
        dist = 2.0f;
    }
    AddTension(tension);
    AddDistance(dist, in);
    AddHook(hook);

    return (mStateTime > mDuration) ? Mode::Stiffen : Mode::None;
}

// FOLLOW (1.5). No extra stamina change in this state.
Mode FightSim::UpdateFollow(const FrameInput& in) {
    float tension = 0.0f, hook, dist = 0.0f;
    bool idle = !in.reel;
    if (in.reel) {
        hook = 1.0f;
        dist = -4.0f;
    } else {
        hook = -1.0f;
    }
    if (StickFwd(in)) {
        idle = false;
        tension = -10.0f;
        hook -= 1.0f;
        if (in.reel) {
            dist -= 2.0f;
        }
    } else if (StickBack(in)) {
        idle = false;
        tension = 1.0f;
        dist -= 0.25f;
    } else {
        tension = -2.0f;
        hook -= 1.0f;
    }
    if (idle) {
        tension = -10.0f;
        hook = 0.0f;
        dist = -0.125f;
    }
    AddTension(tension);
    AddHook(hook);
    AddDistance(dist, in);

    return (mStateTime > mDuration) ? Mode::Stiffen : Mode::None;
}

// STIFFEN (1.5): the reel choice can skip straight to the next run.
Mode FightSim::UpdateStiffen(const FrameInput& in) {
    if (mLastRun == Mode::Away && in.reel) {
        mLastRun = Mode::Follow;
        return Mode::Follow;
    }
    if ((mLastRun == Mode::Struggle || mLastRun == Mode::Follow) && !in.reel) {
        mLastRun = Mode::Away;
        return Mode::Away;
    }
    AddTension(in.reel ? 2.0f : -1.0f);
    return (mStateTime >= mDuration) ? Mode::Stop : Mode::None;
}

// STOP (1.5): a timing test.
Mode FightSim::UpdateStop(const FrameInput& in) {
    if (in.reel) {
        AddDistance(-1.0f, in);
    } else if (in.stickX == 0.0f && in.stickY == 0.0f) {
        AddTension(-2.0f);
    }

    bool fromAway = (mLastRun == Mode::Away);
    int result = 0; // 0 undecided, 1 failure, 2 success
    if (mStateTime > mDuration) {
        result = 1;
    } else if (in.reel == mStopExpectReel) {
        result = (mStateTime <= mStopWindowEnd) ? 2 : 1;
    }
    if (result == 0) {
        return Mode::None;
    }

    float amount = (mFish.stopDamageByte / 255.0f) * 10.0f;
    Mode next;
    if (result == 2) {
        next = fromAway ? Mode::Follow : Mode::Away;
        AddHp(-std::max(amount, 1.0f));
    } else {
        next = fromAway ? Mode::Away : Mode::Struggle;
        AddHp(std::max(amount / 2.0f, 1.0f));
    }
    mLastRun = next;
    return next;
}

// RESIST (1.5). Not entered yet: RF3's trigger is unknown.
Mode FightSim::UpdateResist(const FrameInput& in) {
    if (mStateTime <= mDuration) {
        return Mode::None;
    }
    bool noInput = !in.reel && in.stickY == 0.0f && in.stickX == 0.0f;
    return noInput ? Mode::Away : Mode::Struggle;
}

} // namespace rf3
