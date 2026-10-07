#include "RF3Bridge.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <libultraship/bridge/consolevariablebridge.h>
#include <ship/Context.h>

#include "soh/cvar_prefixes.h"
#include "RF3Sim.h"
#include "RF3SizeMap.h"
#include "RF3FishTable.h"

namespace {

std::optional<rf3::FightSim> sFight;
RF3FightStatus sEndedStatus = {}; // last fight's final values, for the log
rf3::FrameInput sLastFrame;        // last frame fed to the sim, for the debug window
float sLastFishDepthUnits = 0.0f;
float sLastWaterDepthUnits = 0.0f;
// The current fight's RF3 size, drawn once at the strike (step 12).
float sFightLengthMm = 0.0f;
int32_t sFightWeightG = 0;
uint32_t sSizeRng = 0x9E3779B9u;
// Step 14b: the current fight's fish and its table record.
int32_t sFightFishIndex = -1;
float sFightOotLength = 0.0f; // step 14d: OoT length the fight started with, to match the kept fish
const rf3::FishRecord* sFightRecord = nullptr;

// Step 14b: each OoT fish's RF3 species (index = actor params - 100), rolled at pond load.
constexpr int kMaxSpeciesFish = 32;
int sSpecies[kMaxSpeciesFish];
bool sSpeciesRolled[kMaxSpeciesFish] = {};
uint32_t sSpeciesRng = 0;

int RollSpecies() {
    if (sSpeciesRng == 0) {
        sSpeciesRng = static_cast<uint32_t>(std::time(nullptr)) | 1u;
    }
    sSpeciesRng ^= sSpeciesRng << 13;
    sSpeciesRng ^= sSpeciesRng >> 17;
    sSpeciesRng ^= sSpeciesRng << 5;
    return static_cast<int>(sSpeciesRng % rf3::kSpeciesCount);
}

// Step 16d: a region fish (index 32 + slot) has a fixed record and RF3 length, set when it spawned.
const rf3::FishRecord* RegionRecord(int32_t fishIndex, float* mm) {
    RF3RegionFishInfo info;
    if ((fishIndex < RF3_REGION_FISH_INDEX_BASE) ||
        !RF3RegionFish_Get(fishIndex - RF3_REGION_FISH_INDEX_BASE, &info)) {
        return nullptr;
    }
    for (const rf3::FishRecord& r : rf3::kFishRecords) {
        if (r.record == info.record) {
            if (mm != nullptr) {
                *mm = info.mm;
            }
            return &r;
        }
    }
    return nullptr;
}

// The fish's species; one not rolled yet (option switched on mid-visit) rolls now.
// Out-of-range indices fall back to largemouth bass (species 8).
int SpeciesOf(int32_t fishIndex) {
    if (const rf3::FishRecord* region = RegionRecord(fishIndex, nullptr)) {
        return region->species;
    }
    if (fishIndex < 0 || fishIndex >= kMaxSpeciesFish) {
        return 8;
    }
    if (!sSpeciesRolled[fishIndex]) {
        sSpecies[fishIndex] = RollSpecies();
        sSpeciesRolled[fishIndex] = true;
    }
    return sSpecies[fishIndex];
}

void FillStatus(RF3FightStatus* status) {
    *status = {};
    status->active = 1;
    status->mode = static_cast<int32_t>(sFight->GetMode());
    status->lastRun = static_cast<int32_t>(sFight->GetLastRun());
    status->hp = sFight->GetHp();
    status->hook = sFight->GetHook();
    status->tension = sFight->GetTension();
    status->tensionLimit = sFight->GetTensionLimit();
    status->distanceM = sFight->GetDistance();
    status->outcome = static_cast<int32_t>(sFight->GetOutcome());
    status->stateTime = sFight->GetStateTime();
    status->stateDuration = sFight->GetStateDuration();
    status->stopWindowEnd = sFight->GetStopWindowEnd();
    status->stopExpectsReel = sFight->GetStopExpectsReel() ? 1 : 0;
    status->actualDistanceM = sLastFrame.fishDistance;
    status->landingDistanceM = sLastFrame.landingDistance;
    status->fishDepthUnits = sLastFishDepthUnits;
    status->waterDepthUnits = sLastWaterDepthUnits;
    status->reelHeld = sLastFrame.reel ? 1 : 0;
    status->stickX = sLastFrame.stickX;
    status->stickY = sLastFrame.stickY;
    status->fishName = sFight->GetFish().name;
    status->hooked = sFight->IsHooked() ? 1 : 0;
    status->hookWindowStart = sFight->GetFish().hookWindowStart;
    status->hookWindowEnd = sFight->GetFish().hookWindowEnd;
    status->hookGiveUpTime = sFight->GetFish().hookGiveUpTime;
    status->hookSetTiming = sFight->GetHookSetTiming();
    status->hookSetStrength = sFight->GetHookSetStrength();
    status->lengthMm = sFightLengthMm;
    status->weightG = sFightWeightG;
    status->fishIndex = sFightFishIndex;
    status->species = sFightRecord ? sFightRecord->species : -1;
    status->record = sFightRecord ? sFightRecord->record : -1;
    status->speciesName = sFightRecord ? sFightRecord->speciesName : "";
    status->ootLength = sFightOotLength;
}

// Placeholder hook set until step 9 replaces OoT's: a mid-window rod jerk
// (RF3-MECHANICS 1.6): HP 87.5, hook 65.
constexpr float kPlaceholderTimingA = 0.5f;
constexpr float kPlaceholderStrengthB = 0.8f;

// Steps 12 and 14b: the fish's species, and the size class its mapped RF3 length falls in.
const rf3::FishRecord& RecordFor(int32_t fishIndex, float oFishLength) {
    if (const rf3::FishRecord* region = RegionRecord(fishIndex, nullptr)) {
        return *region; // step 16d
    }
    int species = SpeciesOf(fishIndex);
    return rf3::SpeciesRecordForMm(species, rf3::OotLengthToSpeciesMm(species, oFishLength));
}

const rf3::FishParams& FishFor(int32_t fishIndex, float oFishLength) {
    return RecordFor(fishIndex, oFishLength).params;
}

// A new strike or fallback fight: the fish, its record, and its RF3 size.
void StartFightFish(int32_t fishIndex, float oFishLength) {
    sSizeRng ^= sSizeRng << 13;
    sSizeRng ^= sSizeRng >> 17;
    sSizeRng ^= sSizeRng << 5;
    sFightFishIndex = fishIndex;
    sFightOotLength = oFishLength;
    sFightRecord = &RecordFor(fishIndex, oFishLength);
    float regionMm = 0.0f;
    sFightLengthMm = (RegionRecord(fishIndex, &regionMm) != nullptr)
                         ? regionMm // step 16d: drawn at spawn
                         : rf3::OotLengthToSpeciesMm(SpeciesOf(fishIndex), oFishLength);
    sFightWeightG = rf3::WeightFromLength(sFightRecord->params, static_cast<int>(sFightLengthMm),
                                          static_cast<int>(sSizeRng % 10));
}

// OoT relative stick (about -60..60) to RF3 -1..1; RF3's positive Y = rod raised = OoT stick down.
float StickToRf3(float oStick) {
    return std::clamp(oStick / 60.0f, -1.0f, 1.0f);
}

} // namespace

extern "C" int RF3_IsEnabled(void) {
    return CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.Enabled"), 0) != 0;
}

extern "C" int RF3Fight_IsActive(void) {
    return sFight.has_value() ? 1 : 0;
}

extern "C" void RF3Fight_StartStrike(int32_t fishIndex, float fishLength) {
    StartFightFish(fishIndex, fishLength);
    sFight.emplace(sFightRecord->params, rf3::kDefaultTackle);
    sFight->StartStrike();
    sFight->SetBodyLength(sFightLengthMm / 1000.0f); // AWAY surge (DESIGN step 8f)
}

extern "C" int32_t RF3Fight_Update(const RF3FightInput* in, RF3FightOutput* out) {
    float fishDistM = in->fishDistUnits / RF3_UNITS_PER_METRE;
    if (!sFight.has_value()) {
        // Fallback only (option switched on mid-fight): no strike was seen, so
        // start the fight as if hooked with a mid-window rod jerk.
        StartFightFish(in->fishIndex, in->fishLength);
        sFight.emplace(sFightRecord->params, rf3::kDefaultTackle);
        sFight->StartFight(kPlaceholderTimingA, kPlaceholderStrengthB, fishDistM);
        sFight->SetBodyLength(sFightLengthMm / 1000.0f);
    }

    rf3::FrameInput frame;
    frame.reel = in->reelHeld != 0;
    frame.stickX = StickToRf3(in->stickX);
    frame.stickY = -StickToRf3(in->stickY);
    frame.fishDistance = fishDistM;
    frame.distanceCap = in->lineMaxUnits / RF3_UNITS_PER_METRE;
    frame.landingDistance = in->catchDistUnits / RF3_UNITS_PER_METRE;
    frame.lureDepth = in->lureDepthUnits / RF3_UNITS_PER_METRE; // used only by HIT
    // OoT's game logic runs at 20 Hz (R_UPDATE_RATE = 3); RF3 steps are 1/60 s.
    int steps = std::clamp(static_cast<int>(in->substeps), 1, 6);
    for (int i = 0; i < steps && sFight->GetOutcome() == rf3::Outcome::None; i++) {
        sFight->Update(frame);
        if (sFight->IsHooked() && i + 1 < steps) {
            frame.fishDistance = sFight->GetDistance(); // later sub-steps: the fish follows the target
        }
    }
    sLastFrame = frame;
    sLastFishDepthUnits = in->waterY - in->fishY;
    sLastWaterDepthUnits = in->waterY - in->floorY;

    int32_t outcome = static_cast<int32_t>(sFight->GetOutcome());
    out->targetDistUnits = sFight->GetDistance() * RF3_UNITS_PER_METRE;
    out->mode = static_cast<int32_t>(sFight->GetMode());
    out->outcome = outcome;
    out->tension = sFight->GetTension();
    out->tensionLimit = sFight->GetTensionLimit();
    out->hooked = sFight->IsHooked() ? 1 : 0;
    out->shakeSide = sFight->GetShakeSide();
    const rf3::FishParams& fish = sFight->GetFish();
    float t = sFight->GetStateTime();
    out->hookWindowOpen = (sFight->GetMode() == rf3::Mode::Hit && !sFight->IsHooked() &&
                           t >= fish.hookWindowStart && t <= fish.hookWindowEnd)
                              ? 1
                              : 0;
    out->hookWindowFrames = static_cast<int32_t>((fish.hookWindowEnd - fish.hookWindowStart) * 60.0f + 0.5f);
    return outcome;
}

extern "C" void RF3Fight_End(void) {
    if (sFight.has_value()) {
        FillStatus(&sEndedStatus);
        sEndedStatus.active = 0;
        sFight.reset();
    }
}

// Size display (steps 12b, 14d): RF3Records.cpp.

// --- Cast (step 11) ---------------------------------------------------------

namespace {
float sLastCastPower = 0.0f;
float sLastCastFactor = 0.0f;
} // namespace

extern "C" float RF3Cast_Launch(int32_t heldFrames, int32_t windupFrames, int32_t updateRate) {
    float secondsPerFrame = std::clamp(static_cast<int>(updateRate), 1, 6) / 60.0f;
    float power = rf3::CastPower(heldFrames * secondsPerFrame);
    float fullHold = rf3::CastPower(windupFrames * secondsPerFrame);
    sLastCastPower = power;
    sLastCastFactor = rf3::CastDistanceFactor(power, fullHold);
    return sLastCastFactor;
}

extern "C" void RF3Cast_GetLast(float* power, float* distanceFactor) {
    *power = sLastCastPower;
    *distanceFactor = sLastCastFactor;
}

// --- Approach (step 10) -----------------------------------------------------

namespace {

constexpr int kMaxFish = 96; // pond fish 0..31, region fish 32 + slot (step 16d)
rf3::LureMeter sMeter;
rf3::ApproachFish sApproach[kMaxFish];
bool sApproachInit[kMaxFish] = {};
int sChaser = -1;
uint32_t sApproachFrame = 0;
uint32_t sChaserPingFrame = 0;
uint32_t sApproachRng = 0x1234567u;

// Our choice (DESIGN step 10): conditions fixed to the user's RF3 session. The activity level is
// per fish record (step 14b).
int ActivityOf(const rf3::FishParams& fish) {
    return rf3::ActivityLevel(fish, rf3::kConditionsTestSession);
}
float SightRangeOf(const rf3::FishParams& fish) {
    return rf3::SightRange(fish, ActivityOf(fish));
}

uint32_t NextApproachRandom() {
    sApproachRng ^= sApproachRng << 13;
    sApproachRng ^= sApproachRng >> 17;
    sApproachRng ^= sApproachRng << 5;
    return sApproachRng;
}

rf3::ApproachFish* FishAt(int32_t index, float fishLength) {
    if (index < 0 || index >= kMaxFish) {
        return nullptr;
    }
    if (!sApproachInit[index]) {
        sApproach[index] = rf3::ApproachFish(FishFor(index, fishLength));
        sApproachInit[index] = true;
    }
    return &sApproach[index];
}

} // namespace

extern "C" void RF3Approach_UpdateMeter(int32_t lureInWater, float lureMovedUnits, int32_t reelHeld,
                                        int32_t substeps) {
    sApproachFrame++;
    int steps = std::clamp(static_cast<int>(substeps), 1, 6);
    // "Moved" is per RF3 step (1/60 s): share the game frame's movement across the steps.
    bool moved = (lureMovedUnits / steps / RF3_UNITS_PER_METRE) > 0.0005f;
    for (int s = 0; s < steps; s++) {
        sMeter.Update(lureInWater != 0, moved, reelHeld != 0);
        for (int i = 0; i < kMaxFish; i++) {
            if (sApproachInit[i]) {
                sApproach[i].Tick(sMeter.Get());
            }
        }
    }
    // Safety: release a chaser that stopped reporting (left the chase by a path we missed).
    if (sChaser >= 0 && sApproachFrame - sChaserPingFrame > 2) {
        sApproach[sChaser].Stop();
        sChaser = -1;
    }
}

extern "C" int32_t RF3Approach_Notice(int32_t fishIndex, float fishLength, float fishToLureUnits,
                                      float fishToLureXZUnits, float angleFromHeading) {
    rf3::ApproachFish* fish = FishAt(fishIndex, fishLength);
    if (fish == nullptr) {
        return 0;
    }
    rf3::NoticeInput in;
    in.lureInWater = true; // the caller only asks while the lure is in the water
    in.otherFishChasing = (sChaser >= 0 && sChaser != fishIndex);
    in.horizontalDistance = fishToLureXZUnits / RF3_UNITS_PER_METRE;
    in.distance = fishToLureUnits / RF3_UNITS_PER_METRE;
    in.angleFromHeading = angleFromHeading;
    in.sightRange = SightRangeOf(FishFor(fishIndex, fishLength));
    if (!fish->TryNotice(in)) {
        return 0;
    }
    sChaser = fishIndex;
    sChaserPingFrame = sApproachFrame;
    return 1;
}

extern "C" int32_t RF3Approach_Chase(int32_t fishIndex, int32_t lureInWater, int32_t lureNearAngler,
                                     int32_t reachedLure, int32_t substeps) {
    if (fishIndex != sChaser || fishIndex < 0 || fishIndex >= kMaxFish) {
        return RF3_APPROACH_FAIL; // not the chaser (released, or never noticed)
    }
    sChaserPingFrame = sApproachFrame;
    rf3::ChaseInput in;
    in.lureInWater = lureInWater != 0;
    in.lureNearAngler = lureNearAngler != 0;
    in.reachedLure = reachedLure != 0;
    // RF3 steps are 1/60 s; the game frame is R_UPDATE_RATE of them. Stop at the first event.
    int steps = std::clamp(static_cast<int>(substeps), 1, 6);
    rf3::ApproachEvent e = rf3::ApproachEvent::None;
    for (int s = 0; s < steps && e == rf3::ApproachEvent::None; s++) {
        e = sApproach[fishIndex].UpdateChase(in, NextApproachRandom());
    }
    if (e == rf3::ApproachEvent::Fail) {
        sChaser = -1;
        return RF3_APPROACH_FAIL;
    }
    if (e == rf3::ApproachEvent::Strike) {
        // Still the chaser until the bite (OoT's line-up and lunge run first).
        return RF3_APPROACH_STRIKE;
    }
    return RF3_APPROACH_NONE;
}

extern "C" void RF3Approach_SetWary(int32_t fishIndex, float seconds) {
    if (fishIndex >= 0 && fishIndex < kMaxFish) {
        FishAt(fishIndex, 30.0f)->SetWary(seconds);
        if (sChaser == fishIndex) {
            sChaser = -1;
        }
    }
}

extern "C" void RF3Approach_EndChase(int32_t fishIndex) {
    if (fishIndex >= 0 && fishIndex < kMaxFish && sChaser == fishIndex) {
        sApproach[fishIndex].Stop();
        sChaser = -1;
    }
}

extern "C" void RF3Approach_Reset(void) {
    sMeter = rf3::LureMeter();
    for (int i = 0; i < kMaxFish; i++) {
        sApproachInit[i] = false;
    }
    sChaser = -1;
}

extern "C" void RF3Approach_GetStatus(RF3ApproachStatus* status) {
    *status = {};
    status->meter = sMeter.Get();
    status->chaser = sChaser;
    // The chaser's own values; with no chaser, a normal adult bass (as before step 14b).
    const rf3::FishParams& shown = (sChaser >= 0) ? sApproach[sChaser].GetFish() : rf3::kBassNormal;
    status->activityLevel = ActivityOf(shown);
    status->sightRangeM = SightRangeOf(shown);
    for (int i = 0; i < kMaxFish; i++) {
        if (sApproachInit[i] && sApproach[i].GetWary() > 0.0f) {
            status->waryFish++;
        }
    }
    if (sChaser >= 0) {
        const rf3::ApproachFish& f = sApproach[sChaser];
        status->chaserMode = static_cast<int32_t>(f.GetMode());
        status->chaserMemory = f.GetMemory();
        status->chaserInterested = f.IsInterested() ? 1 : 0;
        status->chaserWaitLeft = f.GetWaitLeft();
    }
}

extern "C" void RF3Fight_GetStatus(RF3FightStatus* status) {
    if (sFight.has_value()) {
        FillStatus(status);
    } else {
        *status = sEndedStatus; // active = 0: the values are from the fight that just ended
    }
}

// --- Species (step 14b) -----------------------------------------------------

extern "C" void RF3Species_RollAll(int32_t fishCount) {
    int count = std::clamp(static_cast<int>(fishCount), 0, kMaxSpeciesFish);
    for (int i = 0; i < kMaxSpeciesFish; i++) {
        sSpeciesRolled[i] = false;
    }
    for (int i = 0; i < count; i++) {
        SpeciesOf(i);
    }
    // One line per pond visit in logs/rf3_species.txt, so the spread can be counted (DESIGN step 14).
    std::string dir = Ship::Context::GetPathRelativeToAppDirectory("logs");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(dir + "/rf3_species.txt", std::ios::app);
    char stamp[32];
    std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    out << stamp;
    for (int i = 0; i < count; i++) {
        out << ' ' << i << '=' << sSpecies[i];
    }
    out << '\n';
}

extern "C" int32_t RF3Species_Get(int32_t fishIndex) {
    return SpeciesOf(fishIndex);
}
