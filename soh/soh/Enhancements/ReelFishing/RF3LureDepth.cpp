#include "RF3Bridge.h"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <ship/Context.h>

#include "RF3Sim.h"
#include "RF3TackleAssets.h"

// Step 20a-5: the lure's depth below the surface (rf3::LureSim, RF3-MECHANICS 3.2) for z_fishing.c.
// Step 20b-5: bait rigs too (rf3::BaitSim, RF3-MECHANICS 2.9). See docs/DESIGN.md "20a-5 refined plan" and
// "20b-5 plan" in the project repository.

namespace {

std::optional<rf3::LureSim> sLure;
std::optional<rf3::BaitSim> sBait;
std::optional<rf3::FlySim> sFly; // step 20c-3b
int32_t sLureIndex = -1; // RLRE record for a lure, the RBIT bait group for a bait (-1 = none)
float sLureRestSinkRate = 0.0f; // step 20c-4: the active lure's RLRE +0x48

// RF3's "rod held back": raw stick Y byte above 230, i.e. about +0.8 on RF3's -1..1 stick. The fight maps
// OoT's stick to RF3's as -stick_y / 60 (RF3Bridge.cpp), so this is OoT stick_y below -48.
constexpr float kRodBackStick = 0.8f;
// A bait set without a float (no Depth row on its screen) sinks to the bottom.
constexpr float kNoFloat = 1000.0f;

void LogLine(const char* line) {
    std::string dir = Ship::Context::GetPathRelativeToAppDirectory("logs");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(dir + "/rf3_records.txt", std::ios::app);
    char stamp[32];
    std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    out << stamp << ' ' << line;
}

void LogLure(const char* what, int32_t box, const rf3tackle::Item* item) {
    char line[320];
    if (item == nullptr) {
        std::snprintf(line, sizeof(line), "%s box=%d lure=-1 (surface lure)\n", what, box);
    } else {
        const rf3::LureMove& m = item->move;
        std::snprintf(line, sizeof(line),
                      "%s box=%d lure=%d name=\"%s\" rest=%d twitch=%d buoyancy=%d base=%.2f/%.2f/%.2f/%.2f "
                      "rate=%.3f/%.3f/%.3f/%.3f landDepthM=%.3f\n",
                      what, box, item->index, item->name.c_str(), m.restAction, m.twitchResponse, m.buoyancy,
                      m.speedBase[0], m.speedBase[1], m.speedBase[2], m.speedBase[3], m.speedRate[0],
                      m.speedRate[1], m.speedRate[2], m.speedRate[3],
                      m.buoyancy == 0 ? rf3::kLureLandingDepthTopwater : rf3::kLureLandingDepth);
    }
    LogLine(line);
}

// Step 20b-5: the bait's target depth from the set's float depth setting (0..4), if its screen has a Depth row.
float BaitTarget(const rf3tackle::Box& box, int32_t depthSetting) {
    if (box.layout == "menu_05") {
        return kNoFloat; // no Float or Depth row (Bitterling Rod Set)
    }
    return rf3::kBaitDepthBySetting[std::clamp(depthSetting, 0, 4)];
}

} // namespace

extern "C" int32_t RF3Lure_Begin(void) {
    sLure.reset();
    sBait.reset();
    sFly.reset();
    sLureIndex = -1;
    if (!RF3_IsEnabled()) {
        return 0;
    }
    RF3TackleChoice c;
    RF3Tackle_Get(&c);
    const rf3tackle::Assets& a = rf3tackle::Get();
    if (!a.loaded || c.box < 0 || c.box >= static_cast<int32_t>(a.boxes.size())) {
        LogLure("lure-move", c.box, nullptr);
        return 0;
    }
    const rf3tackle::Box& box = a.boxes[c.box];
    if (box.key == "pole") { // step 23c (user): OoT's pole keeps OoT's own lure movement
        LogLine(("lure-move box=" + std::to_string(c.box) + " kind=oot lure=" + std::to_string(c.lure) + "\n").c_str());
        return 0;
    }
    if (box.kind == "bait") {
        float target = BaitTarget(box, c.depth);
        const rf3tackle::Bait* bait = rf3tackle::FindBait(box, c.bait);
        char line[256];
        std::snprintf(line, sizeof(line), "lure-move box=%d kind=bait bait=%d name=\"%s\" depthSetting=%d targetM=%.2f "
                                          "landDepthM=%.2f\n",
                      c.box, c.bait, bait ? bait->name.c_str() : "---", c.depth, target, rf3::kBaitLandingDepth);
        LogLine(line);
        sBait.emplace(target);
        sBait->Land();
        sLureIndex = c.bait;
        return 1;
    }
    if (box.kind == "fly") { // step 20c-3b: dry flies rise to the surface, sunken flies stay at 0.3 m
        const rf3tackle::Item* fly = rf3tackle::FindLure(box, c.lure);
        bool sunk = (fly != nullptr) && (fly->sunk == 1);
        char line[256];
        std::snprintf(line, sizeof(line), "lure-move box=%d kind=fly fly=%d name=\"%s\" sunk=%d%s landDepthM=%.2f\n", c.box,
                      c.lure, fly ? fly->name.c_str() : "---", fly ? fly->sunk : -1,
                      (fly != nullptr && fly->sunk < 0) ? " (archive without fly data: rebuild it)" : "",
                      sunk ? rf3::kFlySunkDepth : rf3::kFlyDryLandingDepth);
        LogLine(line);
        sFly.emplace(sunk);
        sFly->Land();
        sLureIndex = c.lure;
        return 1;
    }
    const rf3tackle::Item* item = nullptr;
    if (box.kind == "lure") {
        item = rf3tackle::FindLure(box, c.lure);
        if (item != nullptr && !item->hasMove) {
            item = nullptr;
        }
    }
    LogLure("lure-move", c.box, item);
    if (item == nullptr) {
        return 0;
    }
    sLure.emplace(item->move);
    sLure->Land();
    sLureIndex = item->index;
    sLureRestSinkRate = item->move.speedRate[2];
    return 1;
}

extern "C" int32_t RF3Lure_IsActive(void) {
    return (sLure.has_value() || sBait.has_value() || sFly.has_value()) ? 1 : 0;
}

extern "C" int32_t RF3Lure_Kind(void) {
    return sFly ? 1 : sBait ? 2 : sLure ? 0 : -1;
}

extern "C" float RF3Lure_DriftScale(void) {
    // Step 20c-4: RF3's current strength per rig (0x0017D2C0): flies and bait 0.35; a lure max(0.3 - its resting
    // sink rate (RLRE +0x48), 0.05). The region's current is the fly/bait one, so a lure drifts at its share of it.
    if (sLure) {
        return std::max(0.3f - sLureRestSinkRate, 0.05f) / 0.35f;
    }
    return (sFly || sBait) ? 1.0f : 0.0f;
}

extern "C" float RF3Lure_Step(int32_t reeling, float stickY, float floorDepthM, int32_t substeps) {
    int32_t steps = std::max(substeps, 1);
    float floorDepth = std::max(floorDepthM, 0.0f);
    if (sBait) {
        for (int32_t i = 0; i < steps; i++) {
            sBait->Step(reeling != 0, floorDepth);
        }
        return sBait->GetDepth();
    }
    if (sFly) {
        for (int32_t i = 0; i < steps; i++) {
            sFly->Step(reeling != 0, floorDepth);
        }
        return sFly->GetDepth();
    }
    if (!sLure) {
        return 0.0f;
    }
    rf3::LureStep step;
    step.reeling = reeling != 0;
    step.rodBack = std::clamp(-stickY / 60.0f, -1.0f, 1.0f) > kRodBackStick;
    step.floorDepth = floorDepth;
    for (int32_t i = 0; i < steps; i++) {
        sLure->Step(step);
    }
    return sLure->GetDepth();
}

extern "C" void RF3Lure_End(void) {
    sLure.reset();
    sBait.reset();
    sFly.reset();
    sLureIndex = -1;
}

extern "C" void RF3Lure_GetStatus(RF3LureStatus* status) {
    *status = {};
    status->lure = sLureIndex;
    status->restAction = -1;
    if (sFly) {
        status->active = 1;
        status->kind = 1;
        status->depthM = sFly->GetDepth();
        status->mode = sFly->IsReeling() ? 1 : 2;
        status->bottom = sFly->IsOnBottom() ? 1 : 0;
        return;
    }
    if (sBait) {
        status->active = 1;
        status->kind = 2;
        status->depthM = sBait->GetDepth();
        status->mode = sBait->IsReeling() ? 1 : 2;
        status->bottom = sBait->IsOnBottom() ? 1 : 0;
        return;
    }
    if (!sLure) {
        return;
    }
    status->active = 1;
    status->depthM = sLure->GetDepth();
    status->t = sLure->GetT();
    status->mode = static_cast<int32_t>(sLure->GetMode());
    status->twitch = sLure->IsTwitching() ? 1 : 0;
    status->bottom = sLure->IsOnBottom() ? 1 : 0;
    status->stopped = sLure->IsDiveStopped() ? 1 : 0;
    status->restAction = sLure->GetRestAction();
    status->atSurface = sLure->IsAtSurface() ? 1 : 0;
}
