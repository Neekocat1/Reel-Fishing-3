// Reel Fishing III mod (step 24c): randomizer logic for RF3's checks (user's decisions, docs/DESIGN.md step 24).
// - A fish can be caught if its water's randomizer area is reachable (the pond for the Hylian Bass and Loach; a fish
//   region's picked area, step 24a, or any area of its scene), with a rod its record allows (tackle.json's rod
//   masks) that Link can fish with.
// - A rod can fish if Link has it and usable tackle for it: a lure/fly from its box, or a bait (and a float when the
//   set has floats). Logic knows RF3 items by their owned flags (RAND_INF_RF3_HAS_*).
// - Rod conditions need a catchable fish of the condition (species, size, or the rod caught with); catch checks need
//   any catchable fish; the shop needs the pond; the first loach needs a catchable Hylian Loach.
// Rules are plain functions (SoH's ConditionFn), reading tables built at RegionTable_Init.

#include "RF3RandoChecks.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

#include <ship/Context.h>

#include "soh/Enhancements/randomizer/location_access.h"
#include "soh/Enhancements/randomizer/logic.h"
#include "soh/Enhancements/randomizer/static_data.h"
#include "RF3Bridge.h"
#include "RF3FishTable.h"
#include "RF3Items.h"
#include "RF3Regions.h"
#include "RF3TackleAssets.h"

namespace {

constexpr int kRods = 11; // RF3's 10 + OoT's pole (10)

// A fish that can be caught somewhere: its record's species, longest length and rod mask, and the areas to reach.
struct Source {
    std::vector<RandomizerRegion> areas;
    int record = -1;
    int species = -1;
    int lengthMaxMm = 0;
    uint32_t rodMask = 0;
};
std::vector<Source> sSources;
std::vector<RandomizerGet> sTackle[kRods]; // lures/flies, or baits for a bait set
std::vector<RandomizerGet> sFloats[kRods]; // a bait set's floats
bool sNeedsFloat[kRods] = {};

RandomizerGet ItemOf(int32_t kind, int32_t index) {
    int32_t offset = RF3Items_Offset(kind, index);
    return offset < 0 ? RG_NONE : static_cast<RandomizerGet>(RG_RF3_ROD_0 + offset);
}

bool HasRod(int r) {
    if (r == 10) { // OoT's pole: RF3's item, or SoH's own Fishing Pole when that is shuffled
        return logic->HasItem(RG_RF3_ROD_10) ||
               (ctx->GetOption(RSK_SHUFFLE_FISHING_POLE) && logic->HasItem(RG_FISHING_POLE));
    }
    return logic->HasItem(static_cast<RandomizerGet>(RG_RF3_ROD_0 + r));
}

bool HasAny(const std::vector<RandomizerGet>& items) {
    for (RandomizerGet rg : items) {
        if (logic->HasItem(rg)) {
            return true;
        }
    }
    return false;
}

bool CanFishWith(int r) {
    return HasRod(r) && HasAny(sTackle[r]) && (!sNeedsFloat[r] || HasAny(sFloats[r]));
}

bool Reachable(const Source& s) {
    for (RandomizerRegion rr : s.areas) {
        if (areaTable[rr].HasAccess()) {
            return true;
        }
    }
    return false;
}

// Is there a reachable fish matching `match`, with a rod (onlyRod if >= 0) that can catch it and fish?
template <typename Match> bool CanCatch(Match match, int onlyRod = -1) {
    for (const Source& s : sSources) {
        if (!match(s) || !Reachable(s)) {
            continue;
        }
        for (int r = 0; r < kRods; r++) {
            if ((onlyRod < 0 || r == onlyRod) && ((s.rodMask >> r) & 1) && CanFishWith(r)) {
                return true;
            }
        }
    }
    return false;
}

bool IsSpecies(const Source& s, std::initializer_list<int> species) {
    for (int sp : species) {
        if (s.species == sp) {
            return true;
        }
    }
    return false;
}

// RF3 species ids (fishdata.fdt +0x02) as in rf3::RodProgressAddCatch (RF3Sim.cpp): the user's rod conditions.
bool AnyFish() {
    return CanCatch([](const Source&) { return true; });
}
bool Bass() {
    return CanCatch([](const Source& s) { return IsSpecies(s, { 8, 9 }); });
}
bool Trout() {
    return CanCatch([](const Source& s) { return IsSpecies(s, { 0, 1, 2, 11, 12, 15, 31 }); });
}
bool Salmon() {
    return CanCatch([](const Source& s) { return IsSpecies(s, { 13, 14, 16, 32 }); });
}
bool WithFlyRod() {
    return CanCatch([](const Source&) { return true; }, 3);
}
bool LargemouthOver14in() {
    return CanCatch([](const Source& s) { return s.species == 8 && s.lengthMaxMm > 355.6f; });
}
bool WithPoleS() {
    return CanCatch([](const Source&) { return true; }, 5);
}
bool PaleChub() {
    return CanCatch([](const Source& s) { return s.species == 6; });
}
bool BitterlingOver3in() {
    return CanCatch([](const Source& s) { return s.species == 7 && s.lengthMaxMm > 76.2f; });
}
bool Bitterling() {
    return CanCatch([](const Source& s) { return s.species == 7; });
}
bool HylianLoach() {
    return CanCatch([](const Source& s) { return s.species == 34; });
}
bool PondShop() {
    return logic->HasItem(RG_SPEAK_HYLIAN); // in the pond's area; the owner must be understood, as for his prize
}

constexpr ConditionFn kRodRules[10] = { AnyFish, Bass, Trout, Salmon, WithFlyRod, LargemouthOver14in, WithPoleS,
                                        PaleChub, BitterlingOver3in, Bitterling };

const rf3::FishRecord* RecordOf(int record) {
    for (const rf3::FishRecord& r : rf3::kFishRecords) {
        if (r.record == record) {
            return &r;
        }
    }
    return nullptr;
}

void AddSource(const std::vector<RandomizerRegion>& areas, int record) {
    const rf3::FishRecord* rec = RecordOf(record);
    const std::vector<int32_t>& masks = rf3tackle::Get().fishRodMask;
    if (rec == nullptr || areas.empty()) {
        return;
    }
    Source s;
    s.areas = areas;
    s.record = record;
    s.species = rec->species;
    s.lengthMaxMm = rec->params.lengthMaxMm;
    s.rodMask = (record >= 0 && record < static_cast<int>(masks.size())) ? static_cast<uint32_t>(masks[record]) : 0;
    sSources.push_back(s);
}

void BuildTables() {
    sSources.clear();
    for (int r = 0; r < kRods; r++) {
        sTackle[r].clear();
        sFloats[r].clear();
        sNeedsFloat[r] = false;
    }
    // Tackle per rod, from tackle.json (box n = rod n).
    const rf3tackle::Assets& a = rf3tackle::Get();
    for (const rf3tackle::Box& box : a.boxes) {
        if (box.rod < 0 || box.rod >= kRods) {
            continue;
        }
        if (box.kind == "bait") {
            for (const rf3tackle::Bait& b : box.baits) {
                sTackle[box.rod].push_back(ItemOf(RF3_STOCK_BAIT, b.index));
            }
            for (const rf3tackle::Item& f : box.floats) {
                sFloats[box.rod].push_back(ItemOf(RF3_STOCK_FLOAT, f.index));
            }
            sNeedsFloat[box.rod] = !box.floats.empty();
        } else {
            int32_t kind = box.kind == "fly" ? RF3_STOCK_FLY : RF3_STOCK_LURE;
            for (const rf3tackle::Item& it : box.lures) {
                sTackle[box.rod].push_back(ItemOf(kind, it.index));
            }
        }
    }
    // The pond (step 23b): the Hylian Bass (50, 51) and the Hylian Loach (52).
    for (int record : { 50, 51, 52 }) {
        AddSource({ RR_LH_FISHING_POND }, record);
    }
    // The fish regions (step 16), with their randomizer areas (step 24a). Loaded here if no scene has yet.
    if (rf3::FishRegions().empty()) {
        std::string error;
        rf3::LoadFishRegions(&error);
    }
    for (const rf3::FishRegion& region : rf3::FishRegions()) {
        std::vector<RandomizerRegion> areas;
        int picked = rf3::RandoAreaFromName(region.randoArea);
        if (picked >= 0) {
            areas.push_back(static_cast<RandomizerRegion>(picked));
        } else {
            for (int rr = 0; rr < RR_MAX; rr++) {
                if (static_cast<int>(areaTable[rr].scene) == region.scene && !areaTable[rr].regionName.empty()) {
                    areas.push_back(static_cast<RandomizerRegion>(rr));
                }
            }
        }
        for (int record : region.records) {
            AddSource(areas, record);
        }
    }
}

// One line per build in logs/rf3_rando.txt, so a seed's RF3 logic can be checked from numbers.
void LogTables() {
    std::string dir = Ship::Context::GetPathRelativeToAppDirectory("logs");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(dir + "/rf3_rando.txt", std::ios::app);
    char stamp[32];
    std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    out << stamp << " logic-tables assets=" << (rf3tackle::Get().loaded ? 1 : 0) << " sources=" << sSources.size()
        << " regions=" << rf3::FishRegions().size() << " tackle=";
    for (int r = 0; r < kRods; r++) {
        out << (r ? "," : "") << sTackle[r].size() << (sNeedsFloat[r] ? "+f" + std::to_string(sFloats[r].size()) : "");
    }
    out << "\n";
}

} // namespace

void RF3Rando_AddRegionLocations() {
    RF3Rando_ApplyItemNames(); // step 24e: before the spoiler log is written
    BuildTables();
    LogTables();
    for (int r = 0; r < 10; r++) {
        areaTable[RR_ROOT].locations.push_back(
            LocationAccess(static_cast<RandomizerCheck>(RC_RF3_ROD_CONDITION_0 + r), kRodRules[r]));
    }
    areaTable[RR_ROOT].locations.push_back(LocationAccess(RC_RF3_FIRST_LOACH, HylianLoach));
    areaTable[RR_LH_FISHING_POND].locations.push_back(LocationAccess(RC_RF3_POND_SHOP, PondShop));
    for (int rc = RC_RF3_CATCH_1; rc <= RC_RF3_CATCH_147; rc++) {
        areaTable[RR_ROOT].locations.push_back(LocationAccess(static_cast<RandomizerCheck>(rc), AnyFish));
    }
}

// Each rod's starter tackle: Suspended Vibration G for the bass rod, the Fishing Lure for OoT's pole, else the box's
// first lure/fly; a bait set gets its first bait and, if it has floats, its first float.
std::vector<RandomizerGet> RF3Rando_MainPoolItems() {
    std::vector<RandomizerGet> out;
    auto add = [&out](RandomizerGet rg) {
        if (rg != RG_NONE && std::find(out.begin(), out.end(), rg) == out.end()) {
            out.push_back(rg);
        }
    };
    for (int r = 0; r < kRods; r++) {
        add(static_cast<RandomizerGet>(r == 10 ? RG_RF3_ROD_10 : RG_RF3_ROD_0 + r));
    }
    for (const rf3tackle::Box& box : rf3tackle::Get().boxes) {
        if (box.kind == "bait") {
            if (!box.baits.empty()) {
                add(ItemOf(RF3_STOCK_BAIT, box.baits.front().index));
            }
            if (!box.floats.empty()) {
                add(ItemOf(RF3_STOCK_FLOAT, box.floats.front().index));
            }
        } else if (!box.lures.empty()) {
            int32_t kind = box.kind == "fly" ? RF3_STOCK_FLY : RF3_STOCK_LURE;
            int32_t index = box.lures.front().index;
            if (box.rod == 0) {
                index = 48; // Suspended Vibration G, the bass rod's kit before step 23d
            } else if (box.rod == 10) {
                index = 91; // the Fishing Lure
            }
            add(ItemOf(kind, index));
        }
    }
    return out;
}

std::vector<RandomizerGet> RF3Rando_ExtraPoolItems() {
    std::vector<RandomizerGet> main = RF3Rando_MainPoolItems();
    std::vector<RandomizerGet> out;
    for (int rg = RG_RF3_ROD_0; rg <= RG_RF3_LURE_92; rg++) {
        RandomizerGet get = static_cast<RandomizerGet>(rg);
        if (get != RG_RF3_LURE_89 && get != RG_RF3_LURE_90 &&
            std::find(main.begin(), main.end(), get) == main.end()) {
            out.push_back(get);
        }
    }
    return out;
}

void RF3Rando_ApplyItemNames() {
    static bool sApplied = false;
    if (sApplied || !rf3tackle::Get().loaded) {
        return;
    }
    sApplied = true;
    std::vector<std::string> names;
    for (int rg = RG_RF3_ROD_0; rg <= RG_RF3_LURE_92; rg++) {
        int32_t offset = rg - RG_RF3_ROD_0;
        names.push_back(RF3Stock_Name(RF3Items_Kind(offset), RF3Items_Index(offset)));
    }
    auto& nameToEnum = Rando::StaticData::itemNameToEnum;
    for (int rg = RG_RF3_ROD_0; rg <= RG_RF3_LURE_92; rg++) {
        int32_t offset = rg - RG_RF3_ROD_0;
        std::string name = names[offset];
        if (name.empty() || name[0] == '#') {
            continue; // no name in tackle.json (lures 89/90): keep the generic one
        }
        bool clash = std::count(names.begin(), names.end(), name) > 1;
        auto other = nameToEnum.find(name);
        if (other != nameToEnum.end() && !RF3Rando_IsItem(other->second)) {
            clash = true;
        }
        if (clash) {
            name += std::string(" (") + RF3Items_KindName(offset) + ")";
        }
        RF3Rando_BuildItem(static_cast<RandomizerGet>(rg), name);
        nameToEnum[name] = static_cast<RandomizerGet>(rg); // the generic name stays mapped too
    }
}

RandomizerInf RF3Rando_OwnedFlag(RandomizerGet rg) {
    return static_cast<RandomizerInf>(RAND_INF_RF3_HAS_0 + (rg - RG_RF3_ROD_0));
}
