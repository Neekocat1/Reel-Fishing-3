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
#include "RF3SizeMap.h" // step 23d: kSpeciesHylianLoach
#include "RF3TackleAssets.h"

extern "C" {
#include "z64.h"
#include "functions.h"
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
constexpr int32_t kSinkingLure = 92; // OoT's sinking lure: caught a Hylian Loach

struct Entry {
    uint8_t collected = 0;
    uint8_t count = 0;
    uint8_t lost = 0; // step 21e: taken by line breaks in this scene; back on the next scene change
};
Entry sStock[RF3_STOCK_KINDS][kMaxSize];

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
    int32_t restored = 0;
    int32_t refilled = 0;
    for (int32_t k = 0; k < RF3_STOCK_KINDS; k++) {
        for (int32_t i = 0; i < kSize[k]; i++) {
            Entry& e = sStock[k][i];
            if (e.lost > 0) {
                int32_t n = e.count + e.lost;
                e.count = static_cast<uint8_t>(n > kMaxCount ? kMaxCount : n);
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
}

void LoadSection() {
    bool fixed = false;
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
        }
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

// Step 21b: one item per fish caught (docs/DESIGN.md step 21). The category is the catch count mod the
// categories that still have items; the item is a random uncollected one a rod Link owns can use, else
// any uncollected one. Step 21c: first the catch counts toward the rod conditions, and any rod it earns
// is given (so the item pick already sees the new rod as usable).
extern "C" int32_t RF3Stock_UnlockForCatch(int32_t species, float lengthMm) {
    sLastUnlockKind = -1;
    sLastUnlockIndex = -1;
    sLastRodCount = 0;
    if (!RF3_IsEnabled()) {
        return 0;
    }
    RF3TackleChoice tackle;
    RF3Tackle_Get(&tackle);
    rf3::RodProgressAddCatch(sRodProgress, species, lengthMm, tackle.rod);
    const rf3::RodProgress& p = sRodProgress;
    char rodWhy[192];
    std::snprintf(rodWhy, sizeof(rodWhy),
                  "rod-condition species=%d mm=%.1f usedRod=%d total=%u bass=%u trout=%u salmon=%u flyRod=%u "
                  "poleS=%u lmb14=%d chub=%d bitt3=%d bitt=%d",
                  species, lengthMm, tackle.rod, p.total, p.bass, p.trout, p.salmon, p.withFlyRod, p.withPoleS,
                  p.largemouthOver14in ? 1 : 0, p.paleChub ? 1 : 0, p.bitterlingOver3in ? 1 : 0, p.bitterling ? 1 : 0);
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
        }
    }
    if (sLastRodCount == 0) {
        Log("rod-progress", -1, -1, rodWhy); // one line per catch, so the counters can be followed
    }
    // Step 23d (user, 2026-10-08): a Hylian Loach brings OoT's sinking lure, once.
    if (species == rf3::kSpeciesHylianLoach && rando) {
        RF3Rando_SetCheck(RAND_INF_RF3_FIRST_LOACH, "rando-first-loach");
    } else if ((species == rf3::kSpeciesHylianLoach) && !sStock[RF3_STOCK_LURE][kSinkingLure].collected) {
        GiveAndShow(RF3_STOCK_LURE, kSinkingLure, "caught-loach");
    }
    // Catch #1..#N are checks; later catches unlock leftover tackle below.
    if (rando && static_cast<int32_t>(sCatchCount) < RF3Rando_CatchChecks()) {
        uint32_t number = sCatchCount + 1;
        sCatchCount++;
        RF3Rando_SetCheck(static_cast<RandomizerInf>(RAND_INF_RF3_CATCH_1 + number - 1), "rando-catch-check");
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
