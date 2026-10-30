// Reel Fishing III mod (step 24c): randomizer logic for RF3's checks (user's decisions, docs/DESIGN.md step 24).
// - A fish can be caught if its water's randomizer area is reachable (the pond for the Hylian Bass and Loach; a fish
//   region's picked area, step 24a, or any area of its scene), with a rod its record allows (tackle.json's rod
//   masks) that Link can fish with.
// - A rod can fish if Link has it and usable tackle for it: a lure/fly from its box, or a bait (and a float when the
//   set has floats). Logic knows RF3 items by their owned flags (RAND_INF_RF3_HAS_*).
// - Rod conditions need a catchable fish of the condition (species, size, or the rod caught with); catch checks need
//   any catchable fish; the shop needs the pond; the first loach needs a catchable Hylian Loach.
// Rules are plain functions (SoH's ConditionFn), reading tables built at RegionTable_Init.
// Step 42: a fish region counts only at the ages its own age (step 36c) allows, through areas reachable at that age,
// and when its flag conditions hold in logic (DESIGN "Step 42 design").

#include "RF3RandoChecks.h"

#include <algorithm>
#include <array>
#include <utility>
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
#include "RF3Sim.h" // step 43b: the lure conditions
#include "RF3TackleAssets.h"

namespace {

constexpr int kRods = 11; // RF3's 10 + OoT's pole (10)

// A fish that can be caught somewhere: its record's species, longest length and rod mask, and the areas to reach.
// Step 42: what a region's flag condition (step 36c) becomes in logic.
// Step 51: WaterTempleLevel = a Water Temple water-level switch; `levels` is the levels it allows (kWt* bits).
// Step 65b: ColossusOasis = the oasis filled by Song of Storms (fishing it needs no way into the water).
enum class CondLogic { Always, Never, WaterTempleClear, DrainWell, WaterTempleLevel, ColossusOasis };
struct SourceCond {
    rf3::RegionAge age = rf3::RegionAge::Both; // the age it counts at
    CondLogic logic = CondLogic::Never;
    uint8_t levels = 0; // step 51: WaterTempleLevel only
};

struct Source {
    std::vector<RandomizerRegion> areas;
    int record = -1;
    int species = -1;
    int lengthMaxMm = 0;
    uint32_t rodMask = 0;
    rf3::RegionAge age = rf3::RegionAge::Both; // step 42: the region's age
    std::vector<SourceCond> conds;
    char sizeClass = 'n';  // step 43b
    float weightMaxG = 0.0f;
    int scene = -1;
};
std::vector<Source> sSources;
std::vector<RandomizerGet> sTackle[kRods]; // lures/flies, or baits for a bait set
std::vector<int32_t> sTackleKind[kRods];   // step 43b/44c: each sTackle entry's stock kind and index
std::vector<int32_t> sTackleIndex[kRods];
std::vector<RandomizerGet> sFloats[kRods]; // a bait set's floats
std::vector<int32_t> sFloatIndex[kRods];   // step 44c: their float index
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

// Step 42: the game's flags in logic [code]. The lake's "raised" flag and the water control's temp switch 0x3E go both
// ways in rando once the Water Temple is cleared (LakeHyliaWaterControl.cpp), and the lake is low before it; a drained
// well stays drained, so "not drained" could be missed and is never counted; other flags aren't known to logic.
constexpr int32_t kRaisedLake = 0x69;  // EVENTCHKINF_RAISED_LAKE_HYLIA_WATER
constexpr int32_t kDrainedWell = 0x67; // EVENTCHKINF_DRAINED_WELL_IN_KAKARIKO
constexpr int32_t kWaterSwitch = 0x3E; // the rando water control's temp switch (set = lowered)
// Step 51: the Water Temple's water-level switches (z_bg_mizu_water.h WATER_TEMPLE_WATER_F1/F2/F3_FLAG). The water
// actor keeps exactly one set: 0x1C low (-15), 0x1D middle (445), 0x1E high (765, the start).
constexpr int32_t kWtLowSwitch = 0x1C;
constexpr int32_t kWtMidSwitch = 0x1D;
constexpr int32_t kWtHighSwitch = 0x1E;
constexpr uint8_t kWtLow = 1, kWtMid = 2, kWtHigh = 4, kWtAll = 7;

// Step 51: switch flags are per scene, so a switch maps only in its own scene; events are global.
SourceCond LogicOf(const rf3::RegionCondition& c, int scene) {
    SourceCond sc;
    sc.age = c.age;
    if (c.kind == "oasis") { // step 65b: empty on every entry, so "not full" always holds
        sc.logic = c.set ? CondLogic::ColossusOasis : CondLogic::Always;
    } else if (c.kind == "event" && c.flag == kRaisedLake) {
        sc.logic = c.set ? CondLogic::WaterTempleClear : CondLogic::Always;
    } else if (c.kind == "switch" && c.flag == kWaterSwitch && scene == SCENE_LAKE_HYLIA) {
        sc.logic = c.set ? CondLogic::WaterTempleClear : CondLogic::Always;
    } else if (c.kind == "event" && c.flag == kDrainedWell) {
        sc.logic = c.set ? CondLogic::DrainWell : CondLogic::Never;
    } else if (c.kind == "switch" && scene == SCENE_WATER_TEMPLE &&
               (c.flag == kWtLowSwitch || c.flag == kWtMidSwitch || c.flag == kWtHighSwitch)) {
        // step 51: set = that switch's level; unset = the other two (one is always set)
        const uint8_t level = c.flag == kWtLowSwitch ? kWtLow : (c.flag == kWtMidSwitch ? kWtMid : kWtHigh);
        sc.logic = CondLogic::WaterTempleLevel;
        sc.levels = c.set ? level : static_cast<uint8_t>(kWtAll & ~level);
    } else {
        sc.logic = CondLogic::Never;
    }
    return sc;
}

std::string WtLevelsName(uint8_t levels) { // step 51
    static const uint8_t kBits[3] = { kWtLow, kWtMid, kWtHigh };
    static const char* const kNames[3] = { "low", "mid", "high" };
    std::string s;
    for (int i = 0; i < 3; i++) {
        if (levels & kBits[i]) {
            s += (s.empty() ? "" : ",") + std::string(kNames[i]);
        }
    }
    return s.empty() ? "none" : s;
}

std::string CondLogicName(const SourceCond& c) {
    switch (c.logic) {
        case CondLogic::Always: return "always";
        case CondLogic::WaterTempleClear: return "water-temple-clear";
        case CondLogic::DrainWell: return "drain-well";
        case CondLogic::WaterTempleLevel: return "wt(" + WtLevelsName(c.levels) + ")";
        case CondLogic::ColossusOasis: return "song-of-storms";
        default: return "never";
    }
}

bool CondHolds(CondLogic l) {
    switch (l) {
        case CondLogic::Always: return true;
        case CondLogic::WaterTempleClear: return logic->Get(LOGIC_WATER_TEMPLE_CLEAR);
        case CondLogic::DrainWell: return logic->Get(LOGIC_DRAIN_WELL);
        case CondLogic::ColossusOasis: return logic->CanUse(RG_SONG_OF_STORMS);
        default: return false;
    }
}

// Step 51: can Link get the Water Temple's water to one of these levels? SoH's own model (Logic::WaterLevel).
bool WaterLevelAny(uint8_t levels) {
    switch (levels & kWtAll) {
        case 0: return false;
        case kWtLow: return logic->WaterLevel(WL_LOW);
        case kWtMid: return logic->WaterLevel(WL_MID);
        case kWtHigh: return logic->WaterLevel(WL_HIGH);
        case kWtLow | kWtMid: return logic->WaterLevel(WL_LOW_OR_MID);
        case kWtMid | kWtHigh: return logic->WaterLevel(WL_HIGH_OR_MID);
        case kWtLow | kWtHigh: return logic->WaterLevel(WL_LOW) || logic->WaterLevel(WL_HIGH);
        default: return true; // all three
    }
}

bool AgeAllows(rf3::RegionAge age, bool adult) {
    return age == rf3::RegionAge::Both || (age == rf3::RegionAge::Adult) == adult;
}

// The fish are there for Link at this age: the region's age allows it, its conditions at that age hold, and one of
// its areas is reachable at that age.
bool ReachableAt(const Source& s, bool adult) {
    if (!AgeAllows(s.age, adult)) {
        return false;
    }
    uint8_t wtLevels = kWtAll; // step 51: the water levels all of its Water Temple conditions allow
    bool anyWt = false;
    for (const SourceCond& c : s.conds) {
        if (!AgeAllows(c.age, adult)) {
            continue;
        }
        if (c.logic == CondLogic::WaterTempleLevel) {
            wtLevels &= c.levels;
            anyWt = true;
        } else if (!CondHolds(c.logic)) {
            return false;
        }
    }
    if (anyWt && !WaterLevelAny(wtLevels)) {
        return false;
    }
    for (RandomizerRegion rr : s.areas) {
        if (adult ? areaTable[rr].Adult() : areaTable[rr].Child()) {
            return true;
        }
    }
    return false;
}

bool Reachable(const Source& s) {
    return ReachableAt(s, false) || ReachableAt(s, true);
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

void AddSource(const std::vector<RandomizerRegion>& areas, int record, rf3::RegionAge age = rf3::RegionAge::Both,
               const std::vector<SourceCond>& conds = {}, int scene = -1) {
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
    s.age = age;
    s.conds = conds;
    s.sizeClass = rec->sizeClass;
    s.weightMaxG = rec->params.weightMaxG;
    s.scene = scene;
    sSources.push_back(s);
}

std::vector<std::string> sSourceLog; // step 42: one line per fish region, for logs/rf3_rando.txt

void BuildTables() {
    sSources.clear();
    sSourceLog.clear();
    for (int r = 0; r < kRods; r++) {
        sTackle[r].clear();
        sTackleKind[r].clear();
        sTackleIndex[r].clear();
        sFloats[r].clear();
        sFloatIndex[r].clear();
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
                sTackleKind[box.rod].push_back(RF3_STOCK_BAIT);
                sTackleIndex[box.rod].push_back(b.index);
            }
            for (const rf3tackle::Item& f : box.floats) {
                sFloats[box.rod].push_back(ItemOf(RF3_STOCK_FLOAT, f.index));
                sFloatIndex[box.rod].push_back(f.index);
            }
            sNeedsFloat[box.rod] = !box.floats.empty();
        } else {
            int32_t kind = box.kind == "fly" ? RF3_STOCK_FLY : RF3_STOCK_LURE;
            for (const rf3tackle::Item& it : box.lures) {
                sTackle[box.rod].push_back(ItemOf(kind, it.index));
                sTackleKind[box.rod].push_back(kind);
                sTackleIndex[box.rod].push_back(it.index);
            }
        }
    }
    // The pond (step 23b): the Hylian Bass (50, 51) and the Hylian Loach (52).
    for (int record : { 50, 51, 52 }) {
        AddSource({ RR_LH_FISHING_POND }, record, rf3::RegionAge::Both, {}, SCENE_FISHING_POND);
    }
    // The fish regions (step 16), with their randomizer areas (step 24a). Loaded here if no scene has yet.
    if (rf3::FishRegions().empty()) {
        std::string error;
        rf3::LoadFishRegions(&error);
    }
    int regionIndex = 0;
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
        // step 42: the region's age and its conditions in logic
        std::vector<SourceCond> conds;
        std::string condText;
        uint8_t wtLevels = kWtAll; // step 51: for the log, all ages together
        bool anyWt = false;
        for (const rf3::RegionCondition& c : region.conditions) {
            SourceCond sc = LogicOf(c, region.scene);
            conds.push_back(sc);
            if (sc.logic == CondLogic::WaterTempleLevel) {
                wtLevels &= sc.levels;
                anyWt = true;
            }
            char buf[112];
            std::snprintf(buf, sizeof(buf), "%s%s 0x%X %s@%s->%s", condText.empty() ? "" : ",", c.kind.c_str(), c.flag,
                          c.set ? "set" : "unset", rf3::RegionAgeName(c.age), CondLogicName(sc).c_str());
            condText += buf;
        }
        if (anyWt) {
            condText += " wt-levels=" + WtLevelsName(wtLevels);
        }
        for (int record : region.records) {
            AddSource(areas, record, region.age, conds, region.scene);
        }
        char line[400];
        std::snprintf(line, sizeof(line), "logic-source region=%d name=\"%s\" scene=%d areas=%d picked=%s age=%s conds=[%s] records=%d",
                      regionIndex, region.name.c_str(), region.scene, static_cast<int>(areas.size()),
                      region.randoArea.empty() ? "-" : region.randoArea.c_str(), rf3::RegionAgeName(region.age),
                      condText.c_str(), static_cast<int>(region.records.size()));
        sSourceLog.push_back(line);
        regionIndex++;
    }
}

// --- Step 43b: the advanced mode's lure condition checks (DESIGN "Step 43 design") ----------------------------------
// A check is in logic when a reachable fish passes its condition's filter and a rod that can catch it can fish (with a
// lure of the filter's family/type when it has one); "N different species/places" counts distinct catchable ones;
// "every species of the set" needs each. Counts, streaks, visits, time, line and timing need nothing more.

bool SourceMatches(const rf3::LureCondition& c, const Source& s) {
    if (c.species != 0 && (s.species < 0 || s.species >= 64 || !((c.species >> s.species) & 1))) {
        return false;
    }
    if (s.lengthMaxMm < c.minMm || s.weightMaxG < static_cast<float>(c.minWeightG)) {
        return false;
    }
    if (c.lunker && s.sizeClass != 'b' && s.sizeClass != 'l') {
        return false;
    }
    return !(c.outsidePond && s.scene == SCENE_FISHING_POND);
}

bool HasLureFilter(const rf3::LureCondition& c) {
    return c.luresLo != 0 || c.luresHi != 0 || c.lureType >= 0;
}
bool HasFlyFilter(const rf3::LureCondition& c) { // step 44c
    return c.flies != 0 || c.flyTypes != 0;
}
bool HasItemFilter(const rf3::LureCondition& c) {
    return HasLureFilter(c) || HasFlyFilter(c) || c.baits != 0 || c.floats != 0;
}

// The lure's or fly's type, from a box of that kind (lure and fly numbers overlap).
int32_t TypeOf(const char* boxKind, int32_t index) {
    for (const rf3tackle::Box& box : rf3tackle::Get().boxes) {
        if (box.kind != boxKind) {
            continue;
        }
        for (const rf3tackle::Item& it : box.lures) {
            if (it.index == index) {
                return it.type;
            }
        }
    }
    return -1;
}

bool LureFilterMatches(const rf3::LureCondition& c, int32_t lure) {
    if (lure < 0 || lure >= rf3::kLureCount) {
        return false;
    }
    if (c.luresLo != 0 || c.luresHi != 0) {
        const uint64_t bits = lure < 64 ? c.luresLo : c.luresHi;
        if (!((bits >> (lure % 64)) & 1)) {
            return false;
        }
    }
    return c.lureType < 0 || TypeOf("lure", lure) == c.lureType;
}

// Step 44c: does a rod's tackle entry (kind, index) pass the condition's lure, fly and bait filters?
bool ItemFilterMatches(const rf3::LureCondition& c, int32_t kind, int32_t index) {
    if (HasLureFilter(c) && (kind != RF3_STOCK_LURE || !LureFilterMatches(c, index))) {
        return false;
    }
    if (HasFlyFilter(c)) {
        if (kind != RF3_STOCK_FLY || index < 0 || index >= rf3::kFlyCount) {
            return false;
        }
        if (c.flies != 0 && !((c.flies >> index) & 1)) {
            return false;
        }
        const int32_t type = TypeOf("fly", index);
        if (c.flyTypes != 0 && (type < 0 || type > 3 || !((c.flyTypes >> type) & 1))) {
            return false;
        }
    }
    if (c.baits != 0 && (kind != RF3_STOCK_BAIT || index < 0 || index >= 16 || !((c.baits >> index) & 1))) {
        return false;
    }
    return true;
}

// Step 44c: rod r's float need for the condition: a float filter needs a matching float Link has (and a set that
// takes floats); otherwise a set that needs a float needs any.
bool FloatFits(const rf3::LureCondition& c, int r) {
    if (c.floats == 0) {
        return !sNeedsFloat[r] || HasAny(sFloats[r]);
    }
    for (size_t i = 0; i < sFloats[r].size() && i < sFloatIndex[r].size(); i++) {
        const int32_t f = sFloatIndex[r][i];
        if (f >= 0 && f < 8 && ((c.floats >> f) & 1) && logic->HasItem(sFloats[r][i])) {
            return true;
        }
    }
    return false;
}

// Can rod r fish for this condition: the rod filter, and with an item filter a matching lure, fly, bait (and float)
// Link has (step 44c).
bool RodFits(const rf3::LureCondition& c, int r) {
    if (c.rods != 0 && !((c.rods >> r) & 1)) {
        return false;
    }
    if (!HasItemFilter(c)) {
        return CanFishWith(r);
    }
    if (!HasRod(r) || !FloatFits(c, r)) {
        return false;
    }
    for (size_t i = 0; i < sTackleKind[r].size() && i < sTackle[r].size(); i++) {
        if (ItemFilterMatches(c, sTackleKind[r][i], sTackleIndex[r][i]) && logic->HasItem(sTackle[r][i])) {
            return true;
        }
    }
    return false;
}

bool SlotLogic(int slot) {
    const rf3::LureCondition* c = rf3::UnlockConditionFor(slot);
    if (c == nullptr) {
        return false;
    }
    bool any = false;
    uint64_t species = 0;
    uint64_t scenesLo = 0;
    uint64_t scenesHi = 0;
    for (const Source& s : sSources) {
        if (!SourceMatches(*c, s) || !Reachable(s)) {
            continue;
        }
        bool rodOk = false;
        for (int r = 0; r < kRods && !rodOk; r++) {
            rodOk = ((s.rodMask >> r) & 1) && RodFits(*c, r);
        }
        if (!rodOk) {
            continue;
        }
        any = true;
        if (s.species >= 0 && s.species < 64) {
            species |= uint64_t{ 1 } << s.species;
        }
        if (s.scene >= 0 && s.scene < 64) {
            scenesLo |= uint64_t{ 1 } << s.scene;
        } else if (s.scene >= 64 && s.scene < 128) {
            scenesHi |= uint64_t{ 1 } << (s.scene - 64);
        }
    }
    auto pop = [](uint64_t v) {
        int n = 0;
        for (; v; v &= v - 1) {
            n++;
        }
        return n;
    };
    switch (c->goal) {
        case rf3::UnlockGoal::Species:
            return pop(species) >= c->n;
        case rf3::UnlockGoal::SpeciesAll:
            return (species & c->species) == c->species;
        case rf3::UnlockGoal::Places:
            return pop(scenesLo) + pop(scenesHi) >= c->n;
        default:
            return any;
    }
}

template <int L> bool SlotRule() {
    return SlotLogic(L);
}
template <int... I> constexpr std::array<ConditionFn, sizeof...(I)> MakeSlotRules(std::integer_sequence<int, I...>) {
    return { &SlotRule<I>... };
}
constexpr auto kSlotRules = MakeSlotRules(std::make_integer_sequence<int, rf3::kUnlockSlots>{});

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
    for (const std::string& line : sSourceLog) { // step 42
        out << stamp << " " << line << "\n";
    }
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
    // Step 43b: the advanced mode's unlock condition checks (only those with a condition; the seed's mode decides which
    // RF3 checks are locations, RF3Rando_CheckIncludedBySeed). Step 44c: flies, baits and floats too (check = slot).
    for (int l = 0; l < rf3::kUnlockSlots; l++) {
        if (rf3::UnlockConditionFor(l) != nullptr) {
            areaTable[RR_ROOT].locations.push_back(
                LocationAccess(static_cast<RandomizerCheck>(RC_RF3_LURE_0 + l), kSlotRules[static_cast<size_t>(l)]));
        }
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
    // Step 53a: the hint texts the items now carry (clear / ambiguous / obscure), for three samples.
    std::string dir = Ship::Context::GetPathRelativeToAppDirectory("logs");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(dir + "/rf3_rando.txt", std::ios::app);
    char stamp[32];
    std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    int keyed = 0;
    for (int rg = RG_RF3_ROD_0; rg <= RG_RF3_LURE_92; rg++) {
        RandomizerHintTextKey key = Rando::StaticData::RetrieveItem(static_cast<RandomizerGet>(rg)).GetHintKey();
        keyed += (key == RHT_RF3_ITEM_0 + (rg - RG_RF3_ROD_0)) ? 1 : 0;
    }
    out << stamp << " rf3-hint-text items=" << (RG_RF3_LURE_92 - RG_RF3_ROD_0 + 1) << " keyed=" << keyed;
    for (RandomizerGet rg : { ItemOf(RF3_STOCK_ROD, 0), ItemOf(RF3_STOCK_LURE, 48), ItemOf(RF3_STOCK_ROD, 10) }) {
        const HintText& h = Rando::StaticData::RetrieveItem(rg).GetHint();
        out << " | " << h.GetClear().GetEnglish(MF_CLEAN) << " / " << h.GetAmbiguous().GetEnglish(MF_CLEAN) << " / "
            << h.GetObscure().GetEnglish(MF_CLEAN);
    }
    out << "\n";
}

RandomizerInf RF3Rando_OwnedFlag(RandomizerGet rg) {
    return static_cast<RandomizerInf>(RAND_INF_RF3_HAS_0 + (rg - RG_RF3_ROD_0));
}
