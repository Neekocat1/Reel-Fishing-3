// Reel Fishing III mod (step 16d): fish in the scene's regions (RF3Regions.h).
// On entering a scene (RF3 on), each region spawns its fish count: a random allowed record, an RF3
// length in that record's range, and a spot inside the box with water at least 25 units deep. The fish
// are the pond's fish actor with params 400 + slot (z_fishing.c). See docs/DESIGN.md step 16.

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

#include <libultraship/bridge/consolevariablebridge.h>
#include <ship/Context.h>

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"
#include "soh/cvar_prefixes.h"
#include "RF3Bridge.h"
#include "RF3FishTable.h"
#include "RF3Regions.h"
#include "RF3SizeMap.h"

extern "C" {
#include "z64.h"
#include "functions.h"
#include "macros.h"
extern PlayState* gPlayState;
s32 Fishing_RF3KillRegionFish(PlayState* play); // z_fishing.c
}

namespace {

constexpr int kSlots = 64;         // matches RF3_REGION_SLOTS in z_fishing.c
constexpr int kFishParamBase = 400; // EN_FISH_REGION in z_fishing.c
constexpr float kMinDepth = 25.0f;  // water at least this deep under a spawn spot
constexpr int kSpotTries = 30;

struct Slot {
    bool used = false;
    RF3RegionFishInfo info = {};
};
Slot sSlots[kSlots];
bool sSpawnPending = false;
bool sRegionsLoaded = false;
uint32_t sRng = 0;

float Rand01() {
    if (sRng == 0) {
        sRng = static_cast<uint32_t>(std::time(nullptr)) | 1u;
    }
    sRng ^= sRng << 13;
    sRng ^= sRng >> 17;
    sRng ^= sRng << 5;
    return (sRng & 0xFFFFFF) / static_cast<float>(0x1000000);
}

void Log(const std::string& line) {
    std::string dir = Ship::Context::GetPathRelativeToAppDirectory("logs");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(dir + "/rf3_region_fish.txt", std::ios::app);
    char stamp[32];
    std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    out << stamp << ' ' << line << '\n';
}

const rf3::FishRecord* FindRecord(int record) {
    for (const rf3::FishRecord& r : rf3::kFishRecords) {
        if (r.record == record) {
            return &r;
        }
    }
    return nullptr;
}

// OoT length for the model size: step 14's species mapping (adult range <-> 33..76), inverted.
float OotLengthFor(int species, float mm) {
    float minMm, maxMm;
    rf3::SpeciesAdultRangeMm(species, &minMm, &maxMm);
    if (maxMm <= minMm) {
        return 40.0f;
    }
    float oot = rf3::kOotLengthMin + (mm - minMm) * (rf3::kOotLengthMax - rf3::kOotLengthMin) / (maxMm - minMm);
    return std::clamp(oot, 15.0f, 90.0f);
}

// A spot inside the box with water at least kMinDepth deep. Returns false if none was found.
bool FindSpot(PlayState* play, const rf3::FishRegion& r, Vec3f* pos) {
    for (int t = 0; t < kSpotTries; t++) {
        float x, z; // a random point of the box, turned into the world (step 16i)
        r.ToWorld(r.minX + Rand01() * (r.maxX - r.minX), r.minZ + Rand01() * (r.maxZ - r.minZ), &x, &z);
        float surface;
        WaterBox* waterBox;
        if (!WaterBox_GetSurface1(play, &play->colCtx, x, z, &surface, &waterBox)) {
            continue;
        }
        CollisionPoly* poly;
        Vec3f probe = { x, surface + 10.0f, z };
        float floor = BgCheck_EntityRaycastFloor1(&play->colCtx, &poly, &probe);
        float depth = surface - floor;
        if (depth < kMinDepth) {
            continue;
        }
        float below = 10.0f + Rand01() * (std::min(60.0f, depth - 15.0f) - 10.0f);
        *pos = { x, surface - below, z };
        return true;
    }
    return false;
}

int SpawnForScene(PlayState* play) {
    for (Slot& s : sSlots) {
        s = {};
    }
    RF3Approach_Reset(); // new fish in the region slots: no lure memory, chaser or wariness carries over
    if (!sRegionsLoaded) {
        std::string error;
        if (!rf3::LoadFishRegions(&error)) {
            Log("load failed: " + error);
        }
        sRegionsLoaded = true;
    }
    int slot = 0;
    int spawned = 0;
    const std::vector<rf3::FishRegion>& regions = rf3::FishRegions();
    for (size_t ri = 0; ri < regions.size(); ri++) {
        const rf3::FishRegion& r = regions[ri];
        if ((r.scene != play->sceneNum) || r.records.empty()) {
            continue;
        }
        for (int k = 0; k < r.fishCount; k++) {
            char line[256];
            if (slot >= kSlots) {
                Log("scene " + std::to_string(play->sceneNum) + ": more than 64 region fish, the rest skipped");
                return spawned;
            }
            int record = r.records[static_cast<size_t>(Rand01() * r.records.size()) % r.records.size()];
            const rf3::FishRecord* rec = FindRecord(record);
            Vec3f pos;
            if ((rec == nullptr) || !FindSpot(play, r, &pos)) {
                std::snprintf(line, sizeof(line), "scene %d region %d (%s): no water %.0f+ deep found, fish skipped",
                              play->sceneNum, (int)ri, r.name.c_str(), kMinDepth);
                Log(line);
                continue;
            }
            Slot& s = sSlots[slot];
            s.used = true;
            s.info.minX = r.minX;
            s.info.maxX = r.maxX;
            s.info.minZ = r.minZ;
            s.info.maxZ = r.maxZ;
            s.info.surfaceY = r.surfaceY;
            s.info.angle = r.Radians(); // step 16i
            s.info.record = record;
            s.info.mm = rec->params.lengthMinMm + Rand01() * (rec->params.lengthMaxMm - rec->params.lengthMinMm);
            s.info.ootLength = OotLengthFor(rec->species, s.info.mm);
            Actor* actor = Actor_Spawn(&play->actorCtx, play, ACTOR_FISHING, pos.x, pos.y, pos.z, 0,
                                       static_cast<s16>(Rand01() * 0x10000), 0, kFishParamBase + slot);
            std::snprintf(line, sizeof(line),
                          "scene %d region %d (%s) slot %d: record %d (%s %c) %.0f mm, oot %.1f at %.0f %.0f %.0f%s",
                          play->sceneNum, (int)ri, r.name.c_str(), slot, record, rec->speciesName, rec->sizeClass,
                          s.info.mm, s.info.ootLength, pos.x, pos.y, pos.z, actor ? "" : "  SPAWN FAILED");
            Log(line);
            if (actor == nullptr) {
                s.used = false;
                continue;
            }
            spawned++;
            slot++;
        }
    }
    return spawned;
}

void RegisterRF3RegionFish() {
    COND_HOOK(OnSceneInit, CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.Enabled"), 0), [](int16_t sceneNum) {
        for (Slot& s : sSlots) {
            s = {};
        }
        sSpawnPending = true; // the room's actors are loading now; spawn on the first player update
    });
    COND_HOOK(OnPlayerUpdate, CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.Enabled"), 0), []() {
        if (sSpawnPending && (gPlayState != nullptr)) {
            sSpawnPending = false;
            SpawnForScene(gPlayState);
        }
    });
}

} // namespace

static RegisterShipInitFunc initRF3RegionFish(RegisterRF3RegionFish, { CVAR_ENHANCEMENT("ReelFishing3.Enabled") });

extern "C" int32_t RF3RegionFish_Get(int32_t slot, RF3RegionFishInfo* out) {
    if ((slot < 0) || (slot >= kSlots) || !sSlots[slot].used) {
        return 0;
    }
    *out = sSlots[slot].info;
    return 1;
}

namespace rf3 {

int RespawnRegionFish(std::string* status) {
    PlayState* play = gPlayState;
    if (play == nullptr) {
        *status = "No scene.";
        return -1;
    }
    if (Fishing_RF3KillRegionFish(play) < 0) {
        *status = "A fish is on the line: land it first.";
        return -1;
    }
    sRegionsLoaded = true; // use the editor's regions as they are now, saved or not
    int n = SpawnForScene(play);
    *status = "Spawned " + std::to_string(n) + " region fish (see logs/rf3_region_fish.txt).";
    return n;
}

} // namespace rf3
