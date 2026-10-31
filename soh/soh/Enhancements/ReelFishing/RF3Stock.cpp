// Reel Fishing III mod (step 21a): the tackle stock, what Link owns.
// RF3 keeps two bytes per item: collected, and a count up to 9 (docs/RF3-MECHANICS.md 5.1). The mod
// keeps the same, by RF3's own indices, in its own save section. A new file owns nothing; buying the
// rod at the pond gives Lure Rod/Bass and Suspended Vibration G (user's choice, docs/DESIGN.md step
// 21). Step 21a only records the stock; the screen and the fishing don't read it yet.

#include "RF3RandoChecks.h" // step 24d
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

#include <ship/Context.h>

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/SaveManager.h"
#include "soh/ShipInit.hpp"
#include "RF3Bridge.h"
#include "RF3Items.h"
#include "RF3Sim.h"
#include "RF3Sync.h" // step 56a
#include "RF3SizeMap.h" // step 23d: kSpeciesHylianLoach
#include "RF3TackleAssets.h"
#include "RF3FishTable.h" // step 43a: size classes

extern "C" {
#include "z64.h"
#include "functions.h"
#include "variables.h" // step 43a: gSaveContext.dayTime
extern PlayState* gPlayState; // step 43a: the scene and the frame at a catch
}

namespace {

constexpr int32_t kSize[RF3_STOCK_KINDS] = { 11, 93, 40, 12, 6 }; // RROD + OoT pole, RLRE + OoT lures, RFLY, bait types, RFLT
constexpr const char* kKindName[RF3_STOCK_KINDS] = { "rod", "lure", "fly", "bait", "float" };
constexpr const char* kSaveName[RF3_STOCK_KINDS] = { "rods", "lures", "flies", "baits", "floats" };
constexpr int32_t kMaxSize = 93;
constexpr uint8_t kMaxCount = 9;  // RF3's cap
constexpr uint8_t kBaitCount = 9; // RF3 refills collected baits to 9

// Step 23d (user, 2026-10-08): the shop sells OoT's pole with OoT's normal lure. (Before: Lure Rod/Bass and Suspended
// Vibration G; Lure Rod/Bass is now earned by any 5 fish.)
constexpr int32_t kStarterRod = 10;  // OoT's fishing pole
constexpr int32_t kStarterLure = 91; // OoT's normal lure ("Fishing Lure")
constexpr int32_t kSinkingLure = 92; // OoT's sinking lure: step 43a, its own condition (a Hylian Bass of 655 mm+)
// Step 43a (docs/LURE-UNLOCK-CHOICES.md, user): rods 0-2 bring their first lure. Step 44b
// (docs/FLY-BAIT-FLOAT-UNLOCK-CHOICES.md, user): the Fly Rod its first fly, the bait rods a bait and a float (the
// Big Game Fly Rod and Stream Pole L use what Link has).
struct RodStarter {
    int32_t rod;
    int32_t kind;
    int32_t index;
};
constexpr RodStarter kRodStarters[] = {
    { 0, RF3_STOCK_LURE, 48 },  { 1, RF3_STOCK_LURE, 0 },   { 2, RF3_STOCK_LURE, 11 }, { 3, RF3_STOCK_FLY, 0 },
    { 5, RF3_STOCK_BAIT, 0 },   { 5, RF3_STOCK_FLOAT, 2 },  { 7, RF3_STOCK_BAIT, 6 },  { 7, RF3_STOCK_FLOAT, 0 },
    { 8, RF3_STOCK_BAIT, 8 },   { 8, RF3_STOCK_FLOAT, 4 },  { 9, RF3_STOCK_BAIT, 7 },  { 9, RF3_STOCK_FLOAT, 2 },
};

struct Entry {
    uint8_t collected = 0;
    uint8_t count = 0;
    uint8_t lost = 0; // step 21e: taken by line breaks in this scene; back on the next scene change
};
Entry sStock[RF3_STOCK_KINDS][kMaxSize];

// Step 32b (user, 2026-10-14): one copy of each lure and fly, owned or lost (rods and floats are already 1; bait
// keeps RF3's 9).
bool SingleCopy(int32_t kind) {
    return (kind == RF3_STOCK_LURE) || (kind == RF3_STOCK_FLY);
}

// Step 43a: the lure conditions' progress (saved), and the lure's landing and the bite of the fight in progress.
rf3::UnlockProgress sUnlock;
bool sLandValid = false;
uint32_t sLandFrame = 0;
bool sBiteValid = false;
uint32_t sBiteFrame = 0;
float sBiteSeconds = -1.0f;
int32_t sHookedBait = -1; // step 44b: the bait the fish took (the set shows "---" after the strike)

// Step 21b: fish caught (counted when the catch is submitted) since the rod was bought, and what the
// latest catch unlocked (-1 = nothing), for the catch message.
uint32_t sCatchCount = 0;
int32_t sLastUnlockKind = -1;
int32_t sLastUnlockIndex = -1;

// Step 21c: progress toward the user's rod conditions (counted from this step on), and the rods the
// latest catch earned, for the catch message.
rf3::RodProgress sRodProgress;
int32_t sLastRods[rf3::kRodCount] = {};
int32_t sLastRodCount = 0;
// Step 21e: what the latest line break took (-1 = nothing), for the line-break message.
int32_t sLastLostKind = -1;
int32_t sLastLostIndex = -1;

// The unlock categories (rf3::UnlockCategory: lure, fly, bait, float) as stock kinds.
constexpr int32_t kCategoryKind[rf3::kUnlockCategories] = { RF3_STOCK_LURE, RF3_STOCK_FLY, RF3_STOCK_BAIT,
                                                            RF3_STOCK_FLOAT };
// Lures 89 and 90 have no name and are in no tray (RF3-MECHANICS 5.2, group 5).
constexpr int32_t kFirstUnnamedLure = 89;
constexpr int32_t kFirstOotLure = 91; // step 23c: 91 normal, 92 sinking (OoT's pole)

// Does this box (tackle.json) list the item? Lure boxes list lures, fly boxes flies, bait sets baits and
// floats; rods by the box's rod.
bool BoxLists(const rf3tackle::Box& box, int32_t kind, int32_t index) {
    if (kind == RF3_STOCK_ROD) {
        return box.rod == index;
    }
    if (((kind == RF3_STOCK_LURE) && (box.kind == "lure")) || ((kind == RF3_STOCK_FLY) && (box.kind == "fly"))) {
        for (const rf3tackle::Item& it : box.lures) {
            if (it.index == index) {
                return true;
            }
        }
    }
    if (kind == RF3_STOCK_BAIT) {
        for (const rf3tackle::Bait& b : box.baits) {
            if (b.index == index) {
                return true;
            }
        }
    }
    if (kind == RF3_STOCK_FLOAT) {
        for (const rf3tackle::Item& it : box.floats) {
            if (it.index == index) {
                return true;
            }
        }
    }
    return false;
}

// Can the catch unlock this item at all? With tackle.json: if some box lists it. Without: every item but
// the unnamed lures.
bool InPool(int32_t kind, int32_t index) {
    // Step 24d: in an RF3 rando seed a catch (after the catch checks) unlocks only what the seed didn't place.
    if (RF3Rando_Active()) {
        int32_t offset = RF3Items_Offset(kind, index);
        if (offset < 0 || RF3Rando_ItemInSeed(static_cast<RandomizerGet>(RG_RF3_ROD_0 + offset))) {
            return false;
        }
    }
    // Step 23c (user, 2026-10-08): OoT's normal lure comes from the shop, the sinking lure from a Hylian Loach.
    if (kind == RF3_STOCK_LURE && index >= kFirstOotLure) {
        return false;
    }
    const rf3tackle::Assets& a = rf3tackle::Get();
    if (!a.loaded) {
        return !((kind == RF3_STOCK_LURE) && (index >= kFirstUnnamedLure));
    }
    for (const rf3tackle::Box& box : a.boxes) {
        if (BoxLists(box, kind, index)) {
            return true;
        }
    }
    return false;
}

// "Usable first" (user, 2026-10-07): a box of an owned rod lists the item. Box n uses rod n.
bool Usable(int32_t kind, int32_t index) {
    const rf3tackle::Assets& a = rf3tackle::Get();
    for (const rf3tackle::Box& box : a.boxes) {
        if ((box.rod >= 0) && (box.rod < 10) && sStock[RF3_STOCK_ROD][box.rod].collected &&
            BoxLists(box, kind, index)) {
            return true;
        }
    }
    return false;
}

bool Valid(int32_t kind, int32_t index) {
    return (kind >= 0) && (kind < RF3_STOCK_KINDS) && (index >= 0) && (index < kSize[kind]);
}

int32_t Collected(int32_t kind) {
    int32_t n = 0;
    for (int32_t i = 0; i < kSize[kind]; i++) {
        n += sStock[kind][i].collected;
    }
    return n;
}

void Clear() {
    for (auto& kind : sStock) {
        for (Entry& e : kind) {
            e = {};
        }
    }
    sCatchCount = 0;
    sLastUnlockKind = -1;
    sLastUnlockIndex = -1;
    sRodProgress = {};
    sLastRodCount = 0;
    sLastLostKind = -1;
    sLastLostIndex = -1;
    sUnlock = {}; // step 43a
    sLandValid = false;
    sBiteValid = false;
    sBiteSeconds = -1.0f;
}

// One line in logs/rf3_records.txt per change, with the totals, so a session can be checked from numbers.
void Log(const char* what, int32_t kind, int32_t index, const char* why) {
    if (!RF3_IsEnabled()) {
        return;
    }
    std::string dir = Ship::Context::GetPathRelativeToAppDirectory("logs");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(dir + "/rf3_records.txt", std::ios::app);
    char stamp[32];
    std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    char item[48] = "-";
    if (Valid(kind, index)) {
        std::snprintf(item, sizeof(item), "kind=%s index=%d count=%d", kKindName[kind], index,
                      sStock[kind][index].count);
    }
    char line[384];
    std::snprintf(line, sizeof(line),
                  "%s %s %s rods=%d lures=%d flies=%d baits=%d floats=%d catches=%u rodOwned=%d why=%s\n",
                  stamp, what, item, Collected(RF3_STOCK_ROD), Collected(RF3_STOCK_LURE), Collected(RF3_STOCK_FLY),
                  Collected(RF3_STOCK_BAIT), Collected(RF3_STOCK_FLOAT), sCatchCount, RF3Rod_IsOwned(),
                  why ? why : "-");
    out << line;
}

// --- Step 21e: loss and restore -------------------------------------------------------------------

void RestoreForScene(int16_t sceneNum) {
    rf3::UnlockNewVisit(sUnlock); // step 43a: "in one visit" starts over
    int32_t restored = 0;
    int32_t refilled = 0;
    for (int32_t k = 0; k < RF3_STOCK_KINDS; k++) {
        for (int32_t i = 0; i < kSize[k]; i++) {
            Entry& e = sStock[k][i];
            if (e.lost > 0) {
                int32_t n = e.count + e.lost;
                const int32_t cap = SingleCopy(k) ? 1 : kMaxCount; // step 32b
                e.count = static_cast<uint8_t>(n > cap ? cap : n);
                restored += e.lost;
                e.lost = 0;
            }
            if ((k == RF3_STOCK_BAIT) && e.collected && (e.count < kBaitCount)) {
                e.count = kBaitCount;
                refilled++;
            }
        }
    }
    if ((restored > 0) || (refilled > 0)) {
        char why[64];
        std::snprintf(why, sizeof(why), "scene=%d restored=%d baitsRefilled=%d", sceneNum, restored, refilled);
        Log("stock-restore", -1, -1, why);
    }
    RF3Tackle_SceneRestore(); // step 33a: a set whose bait was taken gets the same bait back on
    if ((restored > 0) || (refilled > 0)) {
        RF3Tackle_StockChanged("stock-restore"); // e.g. a box whose only lure was lost gets it back
    }
}

// --- Save file section "reelFishing3Stock" ------------------------------------------------------
// Per kind, one array of {collected, count}. An older save has no section: the stock stays empty and
// the load hook below gives the starter kit if the rod is owned.

void SaveSection(const SaveContext& saveContext, int sectionID, bool fullSave) {
    for (int32_t k = 0; k < RF3_STOCK_KINDS; k++) {
        SaveManager::Instance->SaveArray(kSaveName[k], kSize[k], [k](size_t i) {
            SaveManager::Instance->SaveStruct("", [&]() {
                SaveManager::Instance->SaveData("collected", sStock[k][i].collected);
                SaveManager::Instance->SaveData("count", sStock[k][i].count);
                SaveManager::Instance->SaveData("lost", sStock[k][i].lost); // step 21e
            });
        });
    }
    SaveManager::Instance->SaveData("catches", sCatchCount); // step 21b
    SaveManager::Instance->SaveStruct("rodProgress", [&]() { // step 21c
        const rf3::RodProgress& p = sRodProgress;
        SaveManager::Instance->SaveData("total", p.total); // step 23d
        SaveManager::Instance->SaveData("bass", p.bass);
        SaveManager::Instance->SaveData("trout", p.trout);
        SaveManager::Instance->SaveData("salmon", p.salmon);
        SaveManager::Instance->SaveData("withFlyRod", p.withFlyRod);
        SaveManager::Instance->SaveData("withPoleS", p.withPoleS);
        SaveManager::Instance->SaveData("largemouthOver14in", p.largemouthOver14in);
        SaveManager::Instance->SaveData("paleChub", p.paleChub);
        SaveManager::Instance->SaveData("bitterlingOver3in", p.bitterlingOver3in);
        SaveManager::Instance->SaveData("bitterling", p.bitterling);
    });
    SaveManager::Instance->SaveStruct("unlocks", [&]() { // step 43a: per lure; step 44a: per slot (lures, flies, baits, floats)
        SaveManager::Instance->SaveArray("count", rf3::kUnlockSlots, [](size_t i) {
            SaveManager::Instance->SaveData("", sUnlock.count[i]);
        });
        SaveManager::Instance->SaveArray("setLo", rf3::kUnlockSlots, [](size_t i) {
            SaveManager::Instance->SaveData("", sUnlock.setLo[i]);
        });
        SaveManager::Instance->SaveArray("setHi", rf3::kUnlockSlots, [](size_t i) {
            SaveManager::Instance->SaveData("", sUnlock.setHi[i]);
        });
        SaveManager::Instance->SaveArray("sumG", rf3::kUnlockSlots, [](size_t i) {
            SaveManager::Instance->SaveData("", sUnlock.sumG[i]);
        });
    });
}

void LoadSection() {
    bool fixed = false;
    int32_t singleFixed[RF3_STOCK_KINDS] = {}; // step 32b
    for (int32_t k = 0; k < RF3_STOCK_KINDS; k++) {
        SaveManager::Instance->LoadArray(kSaveName[k], kSize[k], [k](size_t i) {
            SaveManager::Instance->LoadStruct("", [&]() {
                SaveManager::Instance->LoadData("collected", sStock[k][i].collected, static_cast<uint8_t>(0));
                SaveManager::Instance->LoadData("count", sStock[k][i].count, static_cast<uint8_t>(0));
                SaveManager::Instance->LoadData("lost", sStock[k][i].lost, static_cast<uint8_t>(0));
            });
        });
        // A damaged or hand-edited save must not leave a value out of range.
        for (int32_t i = 0; i < kSize[k]; i++) {
            Entry& e = sStock[k][i];
            if ((e.collected > 1) || (e.count > kMaxCount) || (e.lost > kMaxCount)) {
                e.collected = e.collected ? 1 : 0;
                e.count = (e.count > kMaxCount) ? kMaxCount : e.count;
                e.lost = (e.lost > kMaxCount) ? kMaxCount : e.lost;
                fixed = true;
            }
            // Step 32b: a save from before keeps one copy: owned if any was owned, else lost.
            if (SingleCopy(k) && (e.count + e.lost > 1)) {
                e.lost = (e.count > 0) ? 0 : 1;
                e.count = (e.count > 0) ? 1 : 0;
                singleFixed[k]++;
            }
        }
    }
    if ((singleFixed[RF3_STOCK_LURE] > 0) || (singleFixed[RF3_STOCK_FLY] > 0)) {
        char why[80];
        std::snprintf(why, sizeof(why), "single-copy-fixed lures=%d flies=%d", singleFixed[RF3_STOCK_LURE],
                      singleFixed[RF3_STOCK_FLY]);
        Log("stock-load", -1, -1, why);
    }
    SaveManager::Instance->LoadData("catches", sCatchCount, 0u); // step 21b; absent before it
    SaveManager::Instance->LoadStruct("rodProgress", [&]() { // step 21c; absent before it: all 0
        rf3::RodProgress& p = sRodProgress;
        SaveManager::Instance->LoadData("total", p.total, static_cast<uint16_t>(0)); // step 23d; absent before it
        SaveManager::Instance->LoadData("bass", p.bass, static_cast<uint16_t>(0));
        SaveManager::Instance->LoadData("trout", p.trout, static_cast<uint16_t>(0));
        SaveManager::Instance->LoadData("salmon", p.salmon, static_cast<uint16_t>(0));
        SaveManager::Instance->LoadData("withFlyRod", p.withFlyRod, static_cast<uint16_t>(0));
        SaveManager::Instance->LoadData("withPoleS", p.withPoleS, static_cast<uint16_t>(0));
        SaveManager::Instance->LoadData("largemouthOver14in", p.largemouthOver14in, false);
        SaveManager::Instance->LoadData("paleChub", p.paleChub, false);
        SaveManager::Instance->LoadData("bitterlingOver3in", p.bitterlingOver3in, false);
        SaveManager::Instance->LoadData("bitterling", p.bitterling, false);
    });
    sUnlock = {};
    SaveManager::Instance->LoadStruct("unlocks", [&]() { // step 43a; absent before it: all 0; step 44a: a save with
                                                         // only the 93 lure slots defaults the rest to 0
        SaveManager::Instance->LoadArray("count", rf3::kUnlockSlots, [](size_t i) {
            SaveManager::Instance->LoadData("", sUnlock.count[i], static_cast<uint16_t>(0));
        });
        SaveManager::Instance->LoadArray("setLo", rf3::kUnlockSlots, [](size_t i) {
            SaveManager::Instance->LoadData("", sUnlock.setLo[i], static_cast<uint64_t>(0));
        });
        SaveManager::Instance->LoadArray("setHi", rf3::kUnlockSlots, [](size_t i) {
            SaveManager::Instance->LoadData("", sUnlock.setHi[i], static_cast<uint64_t>(0));
        });
        SaveManager::Instance->LoadArray("sumG", rf3::kUnlockSlots, [](size_t i) {
            SaveManager::Instance->LoadData("", sUnlock.sumG[i], static_cast<uint32_t>(0));
        });
    });
    Log("stock-load", -1, -1, fixed ? "out-of-range-fixed" : "save");
}

// New file, or before a file's sections load.
void InitFile(bool isDebug) {
    Clear();
}

void RegisterRF3Stock() {
    Clear();
    SaveManager::Instance->AddInitFunction(InitFile);
    SaveManager::Instance->AddSaveFunction("reelFishing3Stock", 1, SaveSection, true, SECTION_PARENT_NONE);
    SaveManager::Instance->AddLoadFunction("reelFishing3Stock", 1, LoadSection);
    // After every section has loaded: an older save that owns the rod (step 15a) but has no stock.
    // Step 21e (user, 2026-10-07): on every scene change, tackle lost to line breaks comes back and every
    // collected bait is refilled to 9.
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnSceneInit>([](int16_t sceneNum) {
        RestoreForScene(sceneNum);
    });
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnLoadFile>([](int32_t fileNum) {
        if (RF3Rod_IsOwned() && !sStock[RF3_STOCK_ROD][kStarterRod].collected) {
            RF3Stock_GiveStarter("migrate-old-save");
        }
        RF3Tackle_StockChanged("stock-loaded"); // step 21d: the tackle choice is judged by the stock now
    });
}

} // namespace

static RegisterShipInitFunc initRF3Stock(RegisterRF3Stock);

extern "C" int32_t RF3Stock_AnyRod(void) {
    for (int32_t r = 0; r < kSize[RF3_STOCK_ROD]; r++) {
        if (sStock[RF3_STOCK_ROD][r].collected) {
            return 1;
        }
    }
    return 0;
}

extern "C" int32_t RF3Stock_Size(int32_t kind) {
    return ((kind >= 0) && (kind < RF3_STOCK_KINDS)) ? kSize[kind] : 0;
}

extern "C" int32_t RF3Stock_Has(int32_t kind, int32_t index) {
    return Valid(kind, index) ? sStock[kind][index].collected : 0;
}

extern "C" int32_t RF3Stock_Count(int32_t kind, int32_t index) {
    return Valid(kind, index) ? sStock[kind][index].count : 0;
}

extern "C" int32_t RF3Stock_Give(int32_t kind, int32_t index, const char* why) {
    if (!Valid(kind, index)) {
        Log("stock-give-bad", -1, -1, why);
        return 0;
    }
    Entry& e = sStock[kind][index];
    e.collected = 1;
    if (SingleCopy(kind)) {
        // Step 32b: one copy. Owned already: nothing changes. Lost on a line break: it comes back now.
        const char* what = "stock-give";
        if (e.count > 0) {
            what = "stock-give-dupe";
        } else if (e.lost > 0) {
            e.lost = 0;
            what = "stock-give-back";
        }
        e.count = 1;
        Log(what, kind, index, why);
        RF3Tackle_StockChanged(what); // step 32a: an emptied box takes a returned lure back
        return 1;
    }
    if ((kind == RF3_STOCK_ROD) || (kind == RF3_STOCK_FLOAT)) {
        e.count = 1;
    } else if (kind == RF3_STOCK_BAIT) {
        e.count = kBaitCount;
    } else if (e.count < kMaxCount) {
        e.count++;
    }
    Log("stock-give", kind, index, why);
    RF3Tackle_StockChanged("stock-give"); // step 21d: e.g. a box with no lure gets the new one
    return 1;
}

// Step 22c: give, then queue the item get (Link holds it up after the current text).
static int32_t GiveAndShow(int32_t kind, int32_t index, const char* why) {
    if (!RF3Stock_Give(kind, index, why)) {
        return 0;
    }
    RF3Items_Queue(RF3Items_Offset(kind, index));
    return 1;
}

extern "C" void RF3Stock_GiveStarter(const char* why) {
    // Step 24d: in an RF3 rando seed the pond shop is a check; the migration of old saves doesn't apply.
    if (RF3Rando_Active()) {
        if (std::string(why) == "bought") {
            RF3Rando_SetCheck(RAND_INF_RF3_POND_SHOP, "rando-pond-shop");
        }
        return;
    }
    // Bought at the pond: Link holds both up (step 22c). An old save's migration is silent: one that owns the rod item
    // but not OoT's pole (bought before step 23d) gets the pole and its lure, and keeps its RF3 tackle.
    bool show = std::string(why) == "bought";
    if (show) {
        GiveAndShow(RF3_STOCK_ROD, kStarterRod, why);
        GiveAndShow(RF3_STOCK_LURE, kStarterLure, why);
        return;
    }
    RF3Stock_Give(RF3_STOCK_ROD, kStarterRod, why);
    RF3Stock_Give(RF3_STOCK_LURE, kStarterLure, why);
}

// --- Step 43a: lure unlock conditions ------------------------------------------------------------------------------

namespace {

// The lure's or fly's type (tackle.json "type"), from a box of that kind (lure and fly numbers overlap).
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

char SizeClassOf(int32_t record) {
    for (const rf3::FishRecord& r : rf3::kFishRecords) {
        if (r.record == record) {
            return r.sizeClass;
        }
    }
    return 'n';
}

// The chosen line's place among the box's lines (0 = thinnest), or -1.
int32_t LineCellOf(const RF3TackleChoice& c) {
    const rf3tackle::Assets& a = rf3tackle::Get();
    if (!a.loaded || c.box < 0 || c.box >= static_cast<int32_t>(a.boxes.size())) {
        return -1;
    }
    const auto& lines = a.boxes[c.box].lines;
    for (size_t i = 0; i < lines.size(); i++) {
        if (lines[i].index == c.line) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

// Step 44a: a condition slot's stock kind and index (rf3::kFlySlot0 etc.).
int32_t SlotKind(int32_t slot) {
    return slot < rf3::kFlySlot0    ? RF3_STOCK_LURE
           : slot < rf3::kBaitSlot0 ? RF3_STOCK_FLY
           : slot < rf3::kFloatSlot0 ? RF3_STOCK_BAIT
                                    : RF3_STOCK_FLOAT;
}
int32_t SlotIndex(int32_t slot) {
    return slot < rf3::kFlySlot0    ? slot
           : slot < rf3::kBaitSlot0 ? slot - rf3::kFlySlot0
           : slot < rf3::kFloatSlot0 ? slot - rf3::kBaitSlot0
                                    : slot - rf3::kFloatSlot0;
}

// Is the slot's item still to be earned? Normal play: not collected. An advanced seed: its check not done (check and
// flag = RAND_INF_RF3_LURE_0 + slot, step 44c).
bool SlotOpen(int32_t slot, bool rando) {
    if (rando) {
        return RF3Rando_Advanced() && !Flags_GetRandomizerInf(static_cast<RandomizerInf>(RAND_INF_RF3_LURE_0 + slot));
    }
    return !sStock[SlotKind(slot)][SlotIndex(slot)].collected;
}

} // namespace

// Step 56a: a catch as an event. Everything the rod rules, the loach check and the unlock conditions read is in
// rf3::UnlockCatch, so a teammate's catch (56b) can be applied the same way as Link's own.
static rf3::UnlockCatch BuildCatch(int32_t record, int32_t species, float lengthMm, int32_t weightG,
                                   const RF3TackleChoice& tackle) {
    rf3::UnlockCatch k;
    k.record = record; // step 74: the own-fish conditions name records
    k.species = species;
    k.sizeClass = SizeClassOf(record);
    k.lengthMm = lengthMm;
    k.weightG = weightG;
    k.rod = tackle.rod;
    const rf3tackle::Assets& a = rf3tackle::Get();
    const rf3tackle::Box* box =
        (a.loaded && tackle.box >= 0 && tackle.box < static_cast<int32_t>(a.boxes.size())) ? &a.boxes[tackle.box] : nullptr;
    const std::string boxKind = box != nullptr ? box->kind : "";
    k.lure = boxKind == "lure" ? tackle.lure : -1;
    k.lureType = k.lure >= 0 ? TypeOf("lure", k.lure) : -1;
    k.fly = boxKind == "fly" ? tackle.lure : -1; // a fly box's choice is in the lure field
    k.flyType = k.fly >= 0 ? TypeOf("fly", k.fly) : -1;
    k.bait = boxKind == "bait" ? (sHookedBait >= 0 ? sHookedBait : tackle.bait) : -1;
    k.floatIdx = boxKind == "bait" ? tackle.floatIdx : -1;
    k.scene = gPlayState != nullptr ? gPlayState->sceneNum : -1;
    k.pond = k.scene == SCENE_FISHING_POND;
    k.dayHours = gSaveContext.dayTime * 24.0f / 65536.0f;
    k.lineCell = LineCellOf(tackle);
    k.biteSeconds = sBiteSeconds;
    if (sBiteValid && gPlayState != nullptr) {
        k.landSeconds = static_cast<float>(gPlayState->gameplayFrames - sBiteFrame) / 20.0f; // 20 Hz game frames
    }
    sBiteValid = false;
    sBiteSeconds = -1.0f;
    sHookedBait = -1;
    return k;
}

// Counts the catch toward every unlock condition (lures; step 44a: flies, baits, floats), then gives (normal play) or
// checks (an advanced seed) each met one. Step 56a: remote = a teammate's catch (no Streak/Visit goals).
static void CountConditions(const rf3::UnlockCatch& k, bool rando, bool remote) {
    rf3::UnlockAddCatch(sUnlock, k, remote);
    char why[256];
    std::snprintf(why, sizeof(why),
                  "record=%d species=%d class=%c mm=%.1f g=%d rod=%d lure=%d type=%d fly=%d flytype=%d bait=%d float=%d "
                  "scene=%d pond=%d hours=%.2f line=%d bite_s=%.2f land_s=%.2f remote=%d",
                  k.record, k.species, k.sizeClass, k.lengthMm, k.weightG, k.rod, k.lure, k.lureType, k.fly, k.flyType, k.bait,
                  k.floatIdx, k.scene, k.pond ? 1 : 0, k.dayHours, k.lineCell, k.biteSeconds, k.landSeconds,
                  remote ? 1 : 0);
    Log("unlock-progress", -1, -1, why);
    for (int32_t slot = 0; slot < rf3::kUnlockSlots; slot++) {
        if (rf3::UnlockConditionFor(slot) == nullptr || !SlotOpen(slot, rando) || !rf3::UnlockMet(sUnlock, slot)) {
            continue;
        }
        const int32_t kind = SlotKind(slot);
        const int32_t index = SlotIndex(slot);
        char met[80];
        std::snprintf(met, sizeof(met), "unlock-condition slot=%d count=%u", slot, sUnlock.count[slot]);
        if (rando) {
            RF3Rando_SetCheck(static_cast<RandomizerInf>(RAND_INF_RF3_LURE_0 + slot), "rando-unlock-condition");
            Log("unlock-condition-check", kind, index, met);
        } else {
            GiveAndShow(kind, index, met);
        }
    }
}

extern "C" void RF3Stock_FishLost(const char* why) {
    if (!RF3_IsEnabled()) {
        return;
    }
    rf3::UnlockFishLost(sUnlock);
    sBiteValid = false;
    sBiteSeconds = -1.0f;
    sHookedBait = -1;
    Log("lure-streak-reset", -1, -1, why);
}

extern "C" void RF3Stock_NoteLureLanded(uint32_t frame) {
    sLandValid = true;
    sLandFrame = frame;
}

extern "C" void RF3Stock_NoteBite(uint32_t frame) {
    sBiteValid = true;
    sBiteFrame = frame;
    sBiteSeconds = (sLandValid && frame >= sLandFrame) ? static_cast<float>(frame - sLandFrame) / 20.0f : -1.0f;
    sLandValid = false;
}

// Step 21b: one item per fish caught (docs/DESIGN.md step 21). The category is the catch count mod the
// categories that still have items; the item is a random uncollected one a rod Link owns can use, else
// any uncollected one. Step 21c: first the catch counts toward the rod conditions, and any rod it earns
// is given (so the item pick already sees the new rod as usable).
// Step 56a: applies one catch event: rod progress, the loach, the unlock conditions, the catch count. A remote event
// (a teammate's, rando only) sets the same checks, but never gives items itself and never runs the one-per-catch
// pick after catch #N: the finder's item reaches the team through Anchor's GIVE_ITEM.
static int32_t ApplyCatch(const rf3::UnlockCatch& k, bool remote) {
    const int32_t species = k.species;
    const float lengthMm = k.lengthMm;
    rf3::RodProgressAddCatch(sRodProgress, species, lengthMm, k.rod);
    const rf3::RodProgress& p = sRodProgress;
    char rodWhy[200];
    std::snprintf(rodWhy, sizeof(rodWhy),
                  "rod-condition species=%d mm=%.1f usedRod=%d total=%u bass=%u trout=%u salmon=%u flyRod=%u "
                  "poleS=%u lmb14=%d chub=%d bitt3=%d bitt=%d remote=%d",
                  species, lengthMm, k.rod, p.total, p.bass, p.trout, p.salmon, p.withFlyRod, p.withPoleS,
                  p.largemouthOver14in ? 1 : 0, p.paleChub ? 1 : 0, p.bitterlingOver3in ? 1 : 0, p.bitterling ? 1 : 0,
                  remote ? 1 : 0);
    bool rando = RF3Rando_Active(); // step 24d: rod conditions, the loach and catches are checks
    for (int32_t r = 0; r < rf3::kRodCount; r++) { // step 23d: rod 0 too (any 5 fish)
        if (rando) {
            if (rf3::RodConditionMet(p, r)) {
                RF3Rando_SetCheck(static_cast<RandomizerInf>(RAND_INF_RF3_ROD_CONDITION_0 + r), "rando-rod-condition");
            }
            continue;
        }
        if (!sStock[RF3_STOCK_ROD][r].collected && rf3::RodConditionMet(p, r) &&
            GiveAndShow(RF3_STOCK_ROD, r, rodWhy)) {
            sLastRods[sLastRodCount++] = r;
            for (const RodStarter& st : kRodStarters) { // steps 43a/44b: what the rod needs to cast
                if (st.rod == r && !sStock[st.kind][st.index].collected) {
                    GiveAndShow(st.kind, st.index, "rod-starter");
                }
            }
        }
    }
    if (sLastRodCount == 0) {
        Log("rod-progress", -1, -1, rodWhy); // one line per catch, so the counters can be followed
    }
    // Step 24d: in a seed the first Hylian Loach is a check. (Step 23d's "a loach brings the sinking lure" is replaced
    // by lure 92's own condition, step 43a.)
    if (species == rf3::kSpeciesHylianLoach && rando) {
        RF3Rando_SetCheck(RAND_INF_RF3_FIRST_LOACH, "rando-first-loach");
    }
    CountConditions(k, rando, remote); // steps 43a, 44a
    // Catch #1..#N are checks; later catches unlock leftover tackle below.
    if (rando && !RF3Rando_Advanced() && static_cast<int32_t>(sCatchCount) < RF3Rando_CatchChecks()) {
        uint32_t number = sCatchCount + 1;
        sCatchCount++;
        RF3Rando_SetCheck(static_cast<RandomizerInf>(RAND_INF_RF3_CATCH_1 + number - 1), "rando-catch-check");
        return 0;
    }
    // Step 44b (user): the stand-in "one item per catch" rule is only for a simple seed's catches after #N. Normal
    // play and advanced seeds give every item by its condition. Step 56a: a teammate's catch only counts.
    if (remote) {
        sCatchCount++;
        Log("catch-remote", -1, -1, "counted");
        return 0;
    }
    if (!rando || RF3Rando_Advanced()) {
        sCatchCount++;
        Log("catch-unlock-off", -1, -1, rando ? "mode=advanced" : "mode=normal");
        return 0;
    }
    bool remaining[rf3::kUnlockCategories] = {};
    std::string cats;
    for (int32_t c = 0; c < rf3::kUnlockCategories; c++) {
        int32_t kind = kCategoryKind[c];
        for (int32_t i = 0; i < kSize[kind] && !remaining[c]; i++) {
            remaining[c] = !sStock[kind][i].collected && InPool(kind, i);
        }
        if (remaining[c]) {
            cats += (cats.empty() ? "" : ",") + std::string(kKindName[kind]);
        }
    }
    uint32_t n = sCatchCount;
    sCatchCount++;
    int32_t category = rf3::UnlockCategory(remaining, n);
    char why[96];
    if (category < 0) {
        std::snprintf(why, sizeof(why), "n=%u cats=none", n);
        Log("catch-unlock", -1, -1, why);
        return 0;
    }
    int32_t kind = kCategoryKind[category];
    int usable[kMaxSize];
    int others[kMaxSize];
    int usableCount = 0;
    int othersCount = 0;
    for (int32_t i = 0; i < kSize[kind]; i++) {
        if (sStock[kind][i].collected || !InPool(kind, i)) {
            continue;
        }
        if (Usable(kind, i)) {
            usable[usableCount++] = i;
        } else {
            others[othersCount++] = i;
        }
    }
    int32_t index = rf3::PickUnlockItem(usable, usableCount, others, othersCount, Rand_Next());
    std::snprintf(why, sizeof(why), "n=%u cats=%s pick=%s usable=%d left=%d+%d", n, cats.c_str(), kKindName[kind],
                  usableCount > 0 ? 1 : 0, usableCount, othersCount);
    if (index < 0 || !GiveAndShow(kind, index, why)) {
        Log("catch-unlock-none", -1, -1, why);
        return 0;
    }
    sLastUnlockKind = kind;
    sLastUnlockIndex = index;
    return 1;
}

extern "C" int32_t RF3Stock_UnlockForCatch(int32_t record, int32_t species, float lengthMm, int32_t weightG) {
    sLastUnlockKind = -1;
    sLastUnlockIndex = -1;
    sLastRodCount = 0;
    if (!RF3_IsEnabled()) {
        return 0;
    }
    RF3TackleChoice tackle;
    RF3Tackle_Get(&tackle);
    const rf3::UnlockCatch k = BuildCatch(record, species, lengthMm, weightG, tackle); // step 56a
    const int32_t unlocked = ApplyCatch(k, false);
    RF3Sync_SendCatch(k); // step 56b: teammates count it too (RF3 seeds in Anchor co-op only)
    return unlocked;
}

// Step 56a: a teammate's catch (Anchor co-op, 56b), in an RF3 seed only (user's choice, 2026-10-18).
void RF3Stock_ApplyRemoteCatch(const rf3::UnlockCatch& k) {
    if (!RF3_IsEnabled() || !RF3Rando_Active()) {
        return;
    }
    ApplyCatch(k, true);
}

// Step 56c: the team state (Anchor's UPDATE_TEAM_STATE carries this save's RF3 progress for late joiners).
void RF3Stock_GetTeamProgress(uint32_t* catches, rf3::RodProgress* rod, rf3::UnlockProgress* unlock) {
    *catches = sCatchCount;
    *rod = sRodProgress;
    *unlock = sUnlock;
}

// Step 56c: merges a teammate's progress, then sets every check the merged progress meets (most are already set
// through Anchor's flag sync) and puts RF3 items the team owns (their owned flags arrive merged) into the stock,
// silently. RF3 seeds only.
void RF3Stock_MergeTeamProgress(uint32_t catches, const rf3::RodProgress& rod, const rf3::UnlockProgress& unlock) {
    if (!RF3_IsEnabled() || !RF3Rando_Active()) {
        return;
    }
    const uint32_t before = sCatchCount;
    const uint16_t troutBefore = sRodProgress.trout;
    sCatchCount = std::max(sCatchCount, catches);
    rf3::RodProgressMerge(sRodProgress, rod);
    rf3::UnlockProgressMerge(sUnlock, unlock);
    int checks = 0;
    for (int32_t r = 0; r < rf3::kRodCount; r++) {
        if (rf3::RodConditionMet(sRodProgress, r)) {
            checks += RF3Rando_SetCheck(static_cast<RandomizerInf>(RAND_INF_RF3_ROD_CONDITION_0 + r),
                                        "rando-rod-condition-team") ? 1 : 0;
        }
    }
    for (int32_t slot = 0; slot < rf3::kUnlockSlots; slot++) {
        if (rf3::UnlockConditionFor(slot) != nullptr && SlotOpen(slot, true) && rf3::UnlockMet(sUnlock, slot)) {
            checks += RF3Rando_SetCheck(static_cast<RandomizerInf>(RAND_INF_RF3_LURE_0 + slot),
                                        "rando-unlock-condition-team") ? 1 : 0;
        }
    }
    if (!RF3Rando_Advanced()) {
        const uint32_t upTo = std::min<uint32_t>(sCatchCount, static_cast<uint32_t>(RF3Rando_CatchChecks()));
        for (uint32_t n = 1; n <= upTo; n++) {
            checks += RF3Rando_SetCheck(static_cast<RandomizerInf>(RAND_INF_RF3_CATCH_1 + n - 1),
                                        "rando-catch-check-team") ? 1 : 0;
        }
    }
    int items = 0;
    for (int32_t offset = 0; offset < RF3_ITEM_COUNT; offset++) {
        const int32_t kind = RF3Items_Kind(offset);
        const int32_t index = RF3Items_Index(offset);
        if (kind < 0 || !Valid(kind, index) || sStock[kind][index].collected) {
            continue;
        }
        if (Flags_GetRandomizerInf(RF3Rando_OwnedFlag(static_cast<RandomizerGet>(RG_RF3_ROD_0 + offset)))) {
            items += RF3Stock_Give(kind, index, "team-state") ? 1 : 0;
        }
    }
    if (items > 0) {
        RF3Tackle_StockChanged("team-state");
    }
    char why[160];
    std::snprintf(why, sizeof(why), "catches=%u->%u (team %u) trout=%u->%u newChecks=%d newItems=%d", before,
                  sCatchCount, catches, troutBefore, sRodProgress.trout, checks, items);
    Log("team-merge", -1, -1, why);
}

extern "C" int32_t RF3Stock_LastRods(int32_t* rods, int32_t max) {
    int32_t n = 0;
    for (; (n < sLastRodCount) && (n < max); n++) {
        rods[n] = sLastRods[n];
    }
    return n;
}

extern "C" void RF3Stock_GetRodProgress(RF3RodProgress* out) {
    out->total = sRodProgress.total;
    out->bass = sRodProgress.bass;
    out->trout = sRodProgress.trout;
    out->salmon = sRodProgress.salmon;
    out->withFlyRod = sRodProgress.withFlyRod;
    out->withPoleS = sRodProgress.withPoleS;
    out->largemouthOver14in = sRodProgress.largemouthOver14in ? 1 : 0;
    out->paleChub = sRodProgress.paleChub ? 1 : 0;
    out->bitterlingOver3in = sRodProgress.bitterlingOver3in ? 1 : 0;
    out->bitterling = sRodProgress.bitterling ? 1 : 0;
}

extern "C" int32_t RF3Stock_LastUnlock(int32_t* kind, int32_t* index) {
    *kind = sLastUnlockKind;
    *index = sLastUnlockIndex;
    return sLastUnlockKind >= 0 ? 1 : 0;
}

extern "C" uint32_t RF3Stock_CatchCount(void) {
    return sCatchCount;
}

// An item's name from tackle.json (any box that lists it), or "#index" without it.
extern "C" const char* RF3Stock_Name(int32_t kind, int32_t index) {
    static std::string sName;
    sName = "#" + std::to_string(index);
    const rf3tackle::Assets& a = rf3tackle::Get();
    for (const rf3tackle::Box& box : a.boxes) {
        if (!BoxLists(box, kind, index)) {
            continue;
        }
        if (kind == RF3_STOCK_ROD) {
            sName = box.rodName;
        } else if (kind == RF3_STOCK_BAIT) {
            for (const rf3tackle::Bait& b : box.baits) {
                if (b.index == index) {
                    sName = b.name;
                }
            }
        } else {
            for (const rf3tackle::Item& it : (kind == RF3_STOCK_FLOAT) ? box.floats : box.lures) {
                if (it.index == index) {
                    sName = it.name;
                }
            }
        }
        break;
    }
    return sName.c_str();
}

// Step 21e: the line broke. The chosen box's lure or fly (or a bait set's bait) loses 1 until the next scene
// change, as RF3 takes it (RF3-MECHANICS 5.1). Returns 1 if something was taken.
extern "C" int32_t RF3Stock_LoseCurrent(void) {
    sLastLostKind = -1;
    sLastLostIndex = -1;
    if (!RF3_IsEnabled()) {
        return 0;
    }
    RF3TackleChoice c;
    RF3Tackle_Get(&c);
    const rf3tackle::Assets& a = rf3tackle::Get();
    if (!a.loaded || (c.box < 0) || (c.box >= static_cast<int32_t>(a.boxes.size()))) {
        return 0;
    }
    const rf3tackle::Box& box = a.boxes[c.box];
    int32_t kind = box.kind == "bait" ? RF3_STOCK_BAIT : (box.kind == "fly" ? RF3_STOCK_FLY : RF3_STOCK_LURE);
    int32_t index = (kind == RF3_STOCK_BAIT) ? c.bait : c.lure;
    if (!Valid(kind, index) || (sStock[kind][index].count == 0)) {
        Log("stock-lost-none", kind, Valid(kind, index) ? index : -1, "line-break");
        return 0;
    }
    Entry& e = sStock[kind][index];
    e.count--;
    if (e.lost < kMaxCount) {
        e.lost++;
    }
    sLastLostKind = kind;
    sLastLostIndex = index;
    Log("stock-lost", kind, index, "line-break");
    RF3Tackle_StockChanged("stock-lost"); // none left: the box moves to another owned item, or unset
    return 1;
}

// Step 33a (RF3-MECHANICS 5.4): a bite or a give-up at the bait takes the bait off the hook: 1 of its count. The
// set's bait becomes "---" until the menu picks one or the scene changes (RF3Tackle_BaitTaken).
extern "C" int32_t RF3Stock_UseBait(const char* why) {
    if (!RF3_IsEnabled()) {
        return 0;
    }
    RF3TackleChoice c;
    RF3Tackle_Get(&c);
    const rf3tackle::Assets& a = rf3tackle::Get();
    if (!a.loaded || (c.box < 0) || (c.box >= static_cast<int32_t>(a.boxes.size())) ||
        (a.boxes[c.box].kind != "bait")) {
        return 0; // not a bait set
    }
    if (std::string(why) == "strike") {
        sHookedBait = Valid(RF3_STOCK_BAIT, c.bait) ? c.bait : -1; // step 44b: for "with <bait>" conditions
    }
    if (!Valid(RF3_STOCK_BAIT, c.bait) || (sStock[RF3_STOCK_BAIT][c.bait].count == 0)) {
        Log("stock-bait-none", RF3_STOCK_BAIT, Valid(RF3_STOCK_BAIT, c.bait) ? c.bait : -1, why);
        return 0;
    }
    sStock[RF3_STOCK_BAIT][c.bait].count--;
    Log("stock-bait-used", RF3_STOCK_BAIT, c.bait, why);
    RF3Tackle_BaitTaken(why);
    RF3Tackle_StockChanged("stock-bait-used");
    return 1;
}

extern "C" int32_t RF3Stock_LastLost(int32_t* kind, int32_t* index) {
    *kind = sLastLostKind;
    *index = sLastLostIndex;
    return sLastLostKind >= 0 ? 1 : 0;
}

extern "C" int32_t RF3Stock_Lost(int32_t kind, int32_t index) {
    return Valid(kind, index) ? sStock[kind][index].lost : 0;
}

// Step 22b: the title of the first tackle box that lists the item ("Tackle Box Bass"), "" if none.
extern "C" const char* RF3Stock_BoxTitle(int32_t kind, int32_t index) {
    for (const rf3tackle::Box& box : rf3tackle::Get().boxes) {
        if ((kind != RF3_STOCK_ROD) && BoxLists(box, kind, index)) {
            return box.title.c_str();
        }
    }
    return "";
}

extern "C" const char* RF3Stock_KindName(int32_t kind) {
    return ((kind >= 0) && (kind < RF3_STOCK_KINDS)) ? kKindName[kind] : "?";
}
