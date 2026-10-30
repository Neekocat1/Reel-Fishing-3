// Reel Fishing III mod (step 16d): fish in the scene's regions (RF3Regions.h).
// On entering a scene (RF3 on), each region spawns its fish count: a random allowed record, an RF3
// length in that record's range, and a spot inside the box with water at least 25 units deep. The fish
// are the pond's fish actor with params 400 + slot (z_fishing.c). See docs/DESIGN.md step 16.
// Step 45a: a region whose condition flags change while Link is in the scene is respawned live (DESIGN step 45).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <libultraship/bridge/consolevariablebridge.h>
#include <ship/Context.h>

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"
#include "soh/cvar_prefixes.h"
#include "RF3Bridge.h"
#include "RF3Conditions.h"   // step 57d: the season
#include "RF3FishView.h"     // step 61b: ghost fish slots
#include "RF3FishTable.h"
#include "RF3Sim.h"          // step 57d: the length skew
#include "RF3TackleAssets.h" // step 57d: RF3's season sizes
#include "RF3Regions.h"
#include "RF3SizeMap.h"

extern "C" {
#include "z64.h"
#include "functions.h"
#include "macros.h"
#include "variables.h" // step 36c: gSaveContext (LINK_IS_ADULT)
extern PlayState* gPlayState;
s32 Fishing_RF3KillRegionFish(PlayState* play); // z_fishing.c
s32 Fishing_RF3KillRegionSlot(PlayState* play, s32 slot); // step 45a
s32 Fishing_RF3LineBusy(void);
}

namespace {

constexpr int kSlots = 64;         // matches RF3_REGION_SLOTS in z_fishing.c
constexpr int kFishParamBase = 400; // EN_FISH_REGION in z_fishing.c
constexpr float kMinDepth = 25.0f;  // water at least this deep under a spawn spot
constexpr int kSpotTries = 60; // step 46b: was 30; a shape fills less of its box
// Step 45b: a live respawn waits until the water is back at the region's saved surface (the lake and the Water Temple
// move their water over seconds after the flag flips), within this much [assumed], or this many frames at most.
constexpr float kWaterTolerance = 30.0f;
constexpr int kWaterWaitFrames = 400; // 20 s of 20 Hz game frames

struct Slot {
    bool used = false;
    int region = -1; // step 45a: the region index it was spawned for
    // Step 47c-2: which region that was (the index is found again if the list changed: an edit, a delete)
    int regionScene = -1;
    std::string regionName;
    bool regionGoneLogged = false;
    RF3RegionFishInfo info = {};
};
Slot sSlots[kSlots];
bool sSpawnPending = false;
// Step 45a: per region index, the flags of its conditions that count at Link's age, as last seen.
struct Watch {
    bool watched = false;     // a region of this scene with counting conditions
    bool active = false;      // its conditions held when last spawned
    std::vector<uint8_t> values; // per condition (0/1; 2 = doesn't count at this age)
    bool killPending = false; // a flag changed: remove its fish when the line is free
    bool waitLogged = false;
    bool spawnNext = false;   // its fish were removed: spawn on the next frame
    int waterWait = 0;        // step 45b: frames waited for the water
};
std::vector<Watch> sWatch;
int sWatchScene = -1; // the scene the snapshot is for (-1 before the scene's spawn)
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
        if (!r.Contains(x, z)) {
            continue; // step 46b: outside the outline, or in a hole
        }
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

bool AgeMatches(rf3::RegionAge age) {
    const bool adult = LINK_IS_ADULT;
    return age == rf3::RegionAge::Both || (age == rf3::RegionAge::Adult) == adult;
}

// Step 65b: the Colossus oasis is full. BgSpot11Oasis starts at y -100 on every scene load and rises to 0 after Song of
// Storms is played inside it [code]; its actor is read, not the water box, which keeps the last visit's height until
// the actor's init resets it.
bool OasisFull(PlayState* play) {
    if (play->sceneNum != SCENE_DESERT_COLOSSUS) {
        return false;
    }
    for (Actor* a = play->actorCtx.actorLists[ACTORCAT_BG].head; a != nullptr; a = a->next) {
        if (a->id == ACTOR_BG_SPOT11_OASIS) {
            return a->world.pos.y >= -0.01f;
        }
    }
    return false;
}

bool FlagSet(PlayState* play, const rf3::RegionCondition& c) {
    if (c.kind == "oasis") {
        return OasisFull(play);
    }
    return c.kind == "switch" ? Flags_GetSwitch(play, c.flag) != 0 : Flags_GetEventChkInf(c.flag) != 0;
}

// Step 36c: is the region for Link's age, and do its flag conditions (at that age) hold? `why` names the first that
// fails.
bool RegionActive(PlayState* play, const rf3::FishRegion& r, std::string* why) {
    const bool adult = LINK_IS_ADULT;
    auto ageMatches = [adult](rf3::RegionAge age) {
        return age == rf3::RegionAge::Both || (age == rf3::RegionAge::Adult) == adult;
    };
    if (!ageMatches(r.age)) {
        *why = std::string("age=") + rf3::RegionAgeName(r.age) + " link=" + (adult ? "adult" : "child");
        return false;
    }
    for (const rf3::RegionCondition& c : r.conditions) {
        if (!ageMatches(c.age)) {
            continue; // counts only at its age
        }
        const bool isSet = FlagSet(play, c);
        if (isSet != c.set) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%s 0x%X is %s (needs %s)", c.kind.c_str(), c.flag, isSet ? "set" : "unset",
                          c.set ? "set" : "unset");
            *why = buf;
            return false;
        }
    }
    return true;
}

// Rolls one fish of region ri (species, season-skewed length, spot) into the free slot and spawns it. Returns true if
// it was spawned. Step 60a: also used to replace a caught fish.
bool SpawnInSlot(PlayState* play, size_t ri, const rf3::FishRegion& r, int slot) {
    char line[256];
    int record = r.records[static_cast<size_t>(Rand01() * r.records.size()) % r.records.size()];
    const rf3::FishRecord* rec = FindRecord(record);
    Vec3f pos;
    if ((rec == nullptr) || !FindSpot(play, r, &pos)) {
        std::snprintf(line, sizeof(line), "scene %d region %d (%s): no water %.0f+ deep found, fish skipped",
                      play->sceneNum, (int)ri, r.name.c_str(), kMinDepth);
        Log(line);
        return false;
    }
    Slot& s = sSlots[slot];
    s.used = true;
    s.region = static_cast<int>(ri);
    s.regionScene = r.scene;
    s.regionName = r.name;
    s.regionGoneLogged = false;
    s.info.minX = r.minX;
    s.info.maxX = r.maxX;
    s.info.minZ = r.minZ;
    s.info.maxZ = r.maxZ;
    s.info.surfaceY = r.surfaceY;
    s.info.angle = r.Radians(); // step 16i
    s.info.record = record;
    // Step 57d (the user's D1): the length roll leans small in Spring and large in Autumn, by RF3's own
    // per-species growth (tackle.json "season_sizes"); the record's range is unchanged.
    const int season = RF3Conditions_Current().season;
    const auto& means = rf3tackle::Get().seasonMeanMm;
    const auto mean = means.find(rec->species);
    const float minMm = rec->params.lengthMinMm;
    const float maxMm = rec->params.lengthMaxMm;
    const float lengthK = rf3::SeasonLengthExponent(mean != means.end() ? mean->second.data() : nullptr, season, minMm,
                                              maxMm);
    s.info.mm = rf3::SeasonSkewedLength(Rand01(), lengthK, minMm, maxMm);
    s.info.ootLength = OotLengthFor(rec->species, s.info.mm);
    Actor* actor = Actor_Spawn(&play->actorCtx, play, ACTOR_FISHING, pos.x, pos.y, pos.z, 0,
                               static_cast<s16>(Rand01() * 0x10000), 0, kFishParamBase + slot);
    std::snprintf(line, sizeof(line),
                  "scene %d region %d (%s) slot %d: record %d (%s %c) %.0f mm, oot %.1f at %.0f %.0f %.0f "
                  "season=%d k=%.2f%s",
                  play->sceneNum, (int)ri, r.name.c_str(), slot, record, rec->speciesName, rec->sizeClass,
                  s.info.mm, s.info.ootLength, pos.x, pos.y, pos.z, season, lengthK, actor ? "" : "  SPAWN FAILED");
    Log(line);
    if (actor == nullptr) {
        s = {};
        return false;
    }
    return true;
}

// Spawns region ri's fish count into free slots. Returns the fish spawned; *full is set when the slots ran out.
int SpawnRegion(PlayState* play, size_t ri, const rf3::FishRegion& r, bool* full) {
    int spawned = 0;
    int slot = 0;
    for (int k = 0; k < r.fishCount; k++) {
        while ((slot < kSlots) && sSlots[slot].used) {
            slot++;
        }
        if (slot >= kSlots) {
            Log("scene " + std::to_string(play->sceneNum) + ": more than 64 region fish, the rest skipped");
            *full = true;
            return spawned;
        }
        if (SpawnInSlot(play, ri, r, slot)) {
            spawned++;
        }
    }
    return spawned;
}

// Step 45b: is the water at the region's saved surface now? Tries the box's centre, then a few random points (the
// centre may be on land in an odd-shaped region). *y = the surface found, or -99999 if none.
bool WaterReady(PlayState* play, const rf3::FishRegion& r, float* y) {
    *y = -99999.0f;
    for (int t = 0; t < 16; t++) {
        float x, z;
        if (t == 0) {
            r.ToWorld(r.CentreX(), r.CentreZ(), &x, &z);
        } else {
            r.ToWorld(r.minX + Rand01() * (r.maxX - r.minX), r.minZ + Rand01() * (r.maxZ - r.minZ), &x, &z);
        }
        if (!r.Contains(x, z)) {
            continue; // step 46b: only the shape's own water
        }
        float surface;
        WaterBox* waterBox;
        if (WaterBox_GetSurface1(play, &play->colCtx, x, z, &surface, &waterBox)) {
            *y = surface;
            if (std::fabs(surface - r.surfaceY) <= kWaterTolerance) {
                return true;
            }
        }
    }
    return false;
}

// Step 45a: the region's condition flags now (2 for a condition that doesn't count at Link's age).
std::vector<uint8_t> ReadFlags(PlayState* play, const rf3::FishRegion& r) {
    std::vector<uint8_t> v;
    for (const rf3::RegionCondition& c : r.conditions) {
        v.push_back(AgeMatches(c.age) ? (FlagSet(play, c) ? 1 : 0) : 2);
    }
    return v;
}

// Step 45a: after a spawn, remember the flags of every region of the scene that has conditions counting now.
void SnapshotWatch(PlayState* play) {
    const std::vector<rf3::FishRegion>& regions = rf3::FishRegions();
    sWatch.assign(regions.size(), Watch{});
    sWatchScene = play->sceneNum;
    for (size_t ri = 0; ri < regions.size(); ri++) {
        const rf3::FishRegion& r = regions[ri];
        if ((r.scene != play->sceneNum) || r.records.empty() || !AgeMatches(r.age)) {
            continue;
        }
        Watch& w = sWatch[ri];
        w.values = ReadFlags(play, r);
        w.watched = std::any_of(w.values.begin(), w.values.end(), [](uint8_t x) { return x != 2; });
        std::string why;
        w.active = RegionActive(play, r, &why);
    }
}

// Step 45a: every player update. A watched region whose flags changed loses its fish (when no fish is on the line), and
// on the next frame spawns again if its conditions now hold.
void WatchFlags(PlayState* play) {
    const std::vector<rf3::FishRegion>& regions = rf3::FishRegions();
    if ((sWatchScene != play->sceneNum) || (sWatch.size() != regions.size())) {
        return; // the scene's spawn hasn't run yet, or the editor changed the region list (its Respawn re-snapshots)
    }
    for (size_t ri = 0; ri < regions.size(); ri++) {
        const rf3::FishRegion& r = regions[ri];
        Watch& w = sWatch[ri];
        if (!w.watched || (r.scene != play->sceneNum)) {
            continue;
        }
        char line[256];
        std::vector<uint8_t> now = ReadFlags(play, r);
        if (now.size() != w.values.size()) {
            w.values = now; // the editor changed this region's conditions: take them as they are
            continue;
        }
        if (now != w.values) {
            for (size_t ci = 0; ci < now.size() && ci < r.conditions.size(); ci++) {
                if (now[ci] != w.values[ci]) {
                    const rf3::RegionCondition& c = r.conditions[ci];
                    std::snprintf(line, sizeof(line), "region-flag-change scene=%d region=%d (%s) %s 0x%X %d->%d",
                                  play->sceneNum, (int)ri, r.name.c_str(), c.kind.c_str(), c.flag, w.values[ci], now[ci]);
                    Log(line);
                }
            }
            w.values = now;
            if (w.spawnNext) {
                w.waterWait = 0; // its fish are already gone: wait again for this change's water (step 45b)
            } else {
                w.killPending = true;
            }
        }
        if (w.killPending) {
            if (Fishing_RF3LineBusy()) {
                if (!w.waitLogged) {
                    std::snprintf(line, sizeof(line), "region-respawn-wait scene=%d region=%d (%s): a fish is on the line",
                                  play->sceneNum, (int)ri, r.name.c_str());
                    Log(line);
                    w.waitLogged = true;
                }
                continue;
            }
            int killed = 0;
            for (int slot = 0; slot < kSlots; slot++) {
                if (sSlots[slot].used && (sSlots[slot].region == static_cast<int>(ri))) {
                    killed += Fishing_RF3KillRegionSlot(play, slot);
                    sSlots[slot] = {};
                }
            }
            RF3Approach_Reset(); // as the full respawn: no chaser or lure memory points at a removed fish
            std::snprintf(line, sizeof(line), "region-respawn scene=%d region=%d (%s) killed=%d", play->sceneNum,
                          (int)ri, r.name.c_str(), killed);
            Log(line);
            w.killPending = false;
            w.waitLogged = false;
            w.spawnNext = true; // from the next frame on, once the water is there
            w.waterWait = 0;
            continue;
        }
        if (!w.spawnNext) {
            continue;
        }
        std::string why;
        const bool active = RegionActive(play, r, &why);
        float waterY = 0.0f;
        bool timedOut = false;
        if (active && !WaterReady(play, r, &waterY)) {
            if (w.waterWait == 0) {
                std::snprintf(line, sizeof(line),
                              "region-respawn-wait scene=%d region=%d (%s): water at %.0f, region surface %.0f",
                              play->sceneNum, (int)ri, r.name.c_str(), waterY, r.surfaceY);
                Log(line);
            }
            if (++w.waterWait < kWaterWaitFrames) {
                continue;
            }
            timedOut = true;
        }
        w.spawnNext = false;
        bool full = false;
        const int n = active ? SpawnRegion(play, ri, r, &full) : 0;
        std::snprintf(line, sizeof(line),
                      "region-respawn scene=%d region=%d (%s) active=%d->%d spawned=%d waited=%d%s%s%s",
                      play->sceneNum, (int)ri, r.name.c_str(), w.active ? 1 : 0, active ? 1 : 0, n, w.waterWait,
                      timedOut ? " water-timeout" : "", active ? "" : " why=", active ? "" : why.c_str());
        Log(line);
        w.active = active;
        w.waterWait = 0;
    }
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
    int spawned = 0;
    bool full = false;
    const std::vector<rf3::FishRegion>& regions = rf3::FishRegions();
    for (size_t ri = 0; ri < regions.size() && !full; ri++) {
        const rf3::FishRegion& r = regions[ri];
        if ((r.scene != play->sceneNum) || r.records.empty()) {
            continue;
        }
        if (std::string why; !RegionActive(play, r, &why)) { // step 36c
            Log("scene " + std::to_string(play->sceneNum) + " region " + std::to_string(ri) + " (" + r.name +
                "): no fish, " + why);
            continue;
        }
        spawned += SpawnRegion(play, ri, r, &full);
    }
    SnapshotWatch(play); // step 45a
    return spawned;
}

void RegisterRF3RegionFish() {
    COND_HOOK(OnSceneInit, CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.Enabled"), 0), [](int16_t sceneNum) {
        for (Slot& s : sSlots) {
            s = {};
        }
        sWatchScene = -1; // step 45a: no watch until this scene's spawn
        sSpawnPending = true; // the room's actors are loading now; spawn on the first player update
    });
    COND_HOOK(OnPlayerUpdate, CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.Enabled"), 0), []() {
        if (sSpawnPending && (gPlayState != nullptr)) {
            sSpawnPending = false;
            SpawnForScene(gPlayState);
        } else if (gPlayState != nullptr) {
            WatchFlags(gPlayState); // step 45a
        }
    });
}

} // namespace

static RegisterShipInitFunc initRF3RegionFish(RegisterRF3RegionFish, { CVAR_ENHANCEMENT("ReelFishing3.Enabled") });

extern "C" int32_t RF3RegionFish_Get(int32_t slot, RF3RegionFishInfo* out) {
    if (slot >= RF3_FISHVIEW_GHOST_SLOT) { // step 61b: a co-op ghost's fish
        return RF3FishView_GhostFishInfo(slot, out);
    }
    if ((slot < 0) || (slot >= kSlots) || !sSlots[slot].used) {
        return 0;
    }
    *out = sSlots[slot].info;
    return 1;
}

// Step 46b: the shape of a slot's region, for z_fishing.c.
static const rf3::FishRegion* SlotRegion(int32_t slot) {
    if ((slot < 0) || (slot >= kSlots) || !sSlots[slot].used) {
        return nullptr;
    }
    const std::vector<rf3::FishRegion>& regions = rf3::FishRegions();
    Slot& s = sSlots[slot];
    const int ri = s.region;
    if ((ri >= 0) && (ri < static_cast<int>(regions.size())) && (regions[ri].scene == s.regionScene) &&
        (regions[ri].name == s.regionName)) {
        return &regions[ri];
    }
    // Step 47c-2 (2026-10-17 log: six Lake Hylia fish clamped into Kokiri Forest's region 4 from 03:39:23 on, their
    // index pointing at another region): the region list changed since the spawn; find the region again by scene and
    // name.
    for (size_t i = 0; i < regions.size(); i++) {
        if ((regions[i].scene == s.regionScene) && (regions[i].name == s.regionName)) {
            char line[200];
            std::snprintf(line, sizeof(line), "slot-region-moved slot=%d region %d -> %d (%s)", slot, ri, (int)i,
                          s.regionName.c_str());
            Log(line);
            s.region = static_cast<int>(i);
            return &regions[i];
        }
    }
    if (!s.regionGoneLogged) {
        char line[200];
        std::snprintf(line, sizeof(line), "slot-region-gone slot=%d region %d (%s): no shape until the next spawn",
                      slot, ri, s.regionName.c_str());
        Log(line);
        s.regionGoneLogged = true;
    }
    return nullptr;
}

// Step 60a (user, 2026-10-19: "When a fish is caught I want it to be despawned and a new fish spawned in its place"):
// called by z_fishing.c when a caught region fish would be released. Removes it and rolls a new fish for the same
// slot and region (species, season-skewed length, a random spot). Returns 0 if the slot's region is gone (the caller
// then releases the fish as before), else 1 (the fish was removed; a new one spawned unless no water was found).
extern "C" int32_t RF3RegionFish_ReplaceCaught(int32_t slot) {
    PlayState* play = gPlayState;
    const rf3::FishRegion* r = SlotRegion(slot);
    if ((play == nullptr) || (r == nullptr) || r->records.empty()) {
        return 0;
    }
    const size_t ri = static_cast<size_t>(sSlots[slot].region);
    const RF3RegionFishInfo old = sSlots[slot].info;
    const int killed = Fishing_RF3KillRegionSlot(play, slot);
    sSlots[slot] = {};
    RF3Approach_Reset(); // as a live respawn: no chaser or lure memory points at the removed fish
    const bool spawned = SpawnInSlot(play, ri, *r, slot);
    const rf3::FishRecord* oldRec = FindRecord(old.record);
    const rf3::FishRecord* newRec = spawned ? FindRecord(sSlots[slot].info.record) : nullptr;
    char line[256];
    std::snprintf(line, sizeof(line),
                  "region-replace scene=%d region=%d (%s) slot=%d killed=%d: record %d (%s) %.0f mm -> %s%d (%s) %.0f mm",
                  play->sceneNum, (int)ri, r->name.c_str(), slot, killed, old.record,
                  oldRec ? oldRec->speciesName : "?", old.mm, spawned ? "record " : "none ",
                  spawned ? sSlots[slot].info.record : -1, newRec ? newRec->speciesName : "-",
                  spawned ? sSlots[slot].info.mm : 0.0f);
    Log(line);
    return 1;
}

extern "C" int32_t RF3RegionFish_Current(int32_t slot, float* yawRad, float* speedMps) {
    const rf3::FishRegion* r = SlotRegion(slot);
    if (r == nullptr) {
        return 0;
    }
    *yawRad = r->CurrentYawDeg() * 3.14159265f / 180.0f;
    *speedMps = r->currentSet ? r->currentSpeed : 0.2f; // as the fly drift (RF3Regions.cpp)
    return 1;
}

extern "C" int32_t RF3RegionShape_Clamp(int32_t slot, float x, float z, float margin, float* outX, float* outZ) {
    *outX = x;
    *outZ = z;
    const rf3::FishRegion* r = SlotRegion(slot);
    if ((r == nullptr) || !r->HasOutline()) {
        return 0;
    }
    return rf3::shape::ClampInside(r->outline, r->holes, x, z, margin, outX, outZ) ? 1 : 0;
}

extern "C" float RF3RegionShape_OutsideOutline(int32_t slot, float x, float z) {
    const rf3::FishRegion* r = SlotRegion(slot);
    return (r == nullptr) ? 0.0f : r->DistanceOutside(x, z);
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
