// Reel Fishing III mod (step 14d): the weigh-in remembers each fish's species.
// OoT keeps only a length for the fish on hand and for the record. Since step 14b a fish can be any
// RF3 species, so the mod remembers the caught fish, the kept fish and each age's record with their
// species and RF3 size. Records are saved per save file, like OoT's own. See docs/DESIGN.md step 14d.

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

#include <ship/Context.h>

#include "soh/SaveManager.h"
#include "soh/ShipInit.hpp"
#include "RF3Bridge.h"
#include "RF3RandoChecks.h" // step 55a: RF3Rando_Active
#include "RF3SizeMap.h"

namespace {

// One fish as the mod knows it. ootLength is the OoT length it belongs to: an identity is used only
// when OoT shows that length (compared as whole numbers: sFishLengthToWeigh and HIGH_SCORE keep
// whole lengths), otherwise the texts fall back to the bass conversion of the OoT length.
struct FishIdentity {
    bool valid = false;
    int32_t species = -1;
    int32_t record = -1;
    float mm = 0.0f;
    int32_t weightG = 0;
    float ootLength = 0.0f;
};

FishIdentity sOnHand;
FishIdentity sRecord[2]; // [0] adult, [1] child: OoT's two record slots in HIGH_SCORE(HS_FISHING)

bool sHasRod = false; // step 15a: Link has bought the rod at the pond (saved per file)

// Step 16a: the mod's own records, one per fishdata.fdt record (so juvenile, normal and lunker of a
// species are separate). The longest fish wins. Saved per file; the pond's OoT record is separate.
constexpr int kRecordSlots = 53; // fishdata.fdt holds 50 records; 50-52 are the mod's OoT fish (step 23a)
struct BestCatch {
    bool valid = false;
    int32_t species = -1;
    float mm = 0.0f;
    int32_t weightG = 0;
};
BestCatch sBest[kRecordSlots];

// The last catch submitted, so building the catch message twice can't turn a record into a non-record.
struct SubmittedCatch {
    int32_t record = -1;
    float mm = -1.0f;
    int32_t weightG = -1;
    bool newRecord = false;
    float previousMm = 0.0f;
};
SubmittedCatch sLastSubmitted;

float sWeighOotLength = 0.0f;
int32_t sWeighSource = RF3_WEIGH_RECORD;
int32_t sWeighIsChild = 0;

int Slot(int32_t isChild) {
    return isChild ? 1 : 0;
}

bool SameLength(float a, float b) {
    return static_cast<int32_t>(a) == static_cast<int32_t>(b);
}

float BassMm(float ootLength) {
    return rf3::OotLengthToRf3Mm(ootLength); // step 12b's conversion, the fallback
}

// One line per event in logs/rf3_records.txt, so the keep/record flow can be checked from numbers.
void LogIdentity(const char* what, const FishIdentity& id, float ootLength) {
    std::string dir = Ship::Context::GetPathRelativeToAppDirectory("logs");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(dir + "/rf3_records.txt", std::ios::app);
    char stamp[32];
    std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    char line[192];
    std::snprintf(line, sizeof(line), "%s %s ootLength=%.2f valid=%d species=%d record=%d mm=%.1f g=%d idOot=%.2f\n",
                  stamp, what, ootLength, id.valid ? 1 : 0, id.species, id.record, id.mm, id.weightG, id.ootLength);
    out << line;
}

// --- Save file section "reelFishing3" ---------------------------------------------------------

void SaveIdentity(const char* name, const FishIdentity& id) {
    SaveManager::Instance->SaveStruct(name, [&]() {
        SaveManager::Instance->SaveData("valid", id.valid);
        SaveManager::Instance->SaveData("species", id.species);
        SaveManager::Instance->SaveData("record", id.record);
        SaveManager::Instance->SaveData("mm", id.mm);
        SaveManager::Instance->SaveData("weightG", id.weightG);
        SaveManager::Instance->SaveData("ootLength", id.ootLength);
    });
}

void LoadIdentity(const char* name, FishIdentity& id) {
    SaveManager::Instance->LoadStruct(name, [&]() {
        SaveManager::Instance->LoadData("valid", id.valid, false);
        SaveManager::Instance->LoadData("species", id.species, -1);
        SaveManager::Instance->LoadData("record", id.record, -1);
        SaveManager::Instance->LoadData("mm", id.mm, 0.0f);
        SaveManager::Instance->LoadData("weightG", id.weightG, 0);
        SaveManager::Instance->LoadData("ootLength", id.ootLength, 0.0f);
    });
}

void SaveSection(const SaveContext& saveContext, int sectionID, bool fullSave) {
    SaveIdentity("adultRecord", sRecord[0]);
    SaveIdentity("childRecord", sRecord[1]);
    SaveManager::Instance->SaveData("hasRod", sHasRod); // step 15a
    SaveManager::Instance->SaveArray("bestCatches", kRecordSlots, [](size_t i) { // step 16a
        SaveManager::Instance->SaveStruct("", [&]() {
            SaveManager::Instance->SaveData("valid", sBest[i].valid);
            SaveManager::Instance->SaveData("species", sBest[i].species);
            SaveManager::Instance->SaveData("mm", sBest[i].mm);
            SaveManager::Instance->SaveData("weightG", sBest[i].weightG);
        });
    });
}

void LoadSection() {
    LoadIdentity("adultRecord", sRecord[0]);
    LoadIdentity("childRecord", sRecord[1]);
    SaveManager::Instance->LoadData("hasRod", sHasRod, false); // step 15a; absent in older saves
    SaveManager::Instance->LoadArray("bestCatches", kRecordSlots, [](size_t i) { // step 16a; absent = none
        SaveManager::Instance->LoadStruct("", [&]() {
            SaveManager::Instance->LoadData("valid", sBest[i].valid, false);
            SaveManager::Instance->LoadData("species", sBest[i].species, -1);
            SaveManager::Instance->LoadData("mm", sBest[i].mm, 0.0f);
            SaveManager::Instance->LoadData("weightG", sBest[i].weightG, 0);
        });
    });
}

// New file, or before a file's sections load: no remembered records. The kept fish is not saved by
// OoT either (sFishOnHandLength is not in the save), but it does not survive a file change.
void InitFile(bool isDebug) {
    sRecord[0] = {};
    sRecord[1] = {};
    sOnHand = {};
    sHasRod = false;
    for (BestCatch& b : sBest) {
        b = {};
    }
    sLastSubmitted = {};
}

void RegisterRF3Records() {
    SaveManager::Instance->AddInitFunction(InitFile);
    SaveManager::Instance->AddSaveFunction("reelFishing3", 1, SaveSection, true, SECTION_PARENT_NONE);
    SaveManager::Instance->AddLoadFunction("reelFishing3", 1, LoadSection);
}

} // namespace

static RegisterShipInitFunc initRF3Records(RegisterRF3Records);

extern "C" void RF3Size_SetLengthToWeigh(float ootLength, int32_t source, int32_t isChild) {
    sWeighOotLength = ootLength;
    sWeighSource = source;
    sWeighIsChild = isChild;
}

extern "C" float RF3Size_GetLengthToWeigh(void) {
    return sWeighOotLength;
}

extern "C" float RF3Size_GetRecordMm(int32_t isChild, float ootLength) {
    const FishIdentity& r = sRecord[Slot(isChild)];
    return (r.valid && SameLength(r.ootLength, ootLength)) ? r.mm : BassMm(ootLength);
}

extern "C" float RF3Size_GetWeighMm(void) {
    if (sWeighSource == RF3_WEIGH_RECORD) {
        return RF3Size_GetRecordMm(sWeighIsChild, sWeighOotLength);
    }
    if (sWeighSource == RF3_WEIGH_ON_HAND) {
        return (sOnHand.valid && SameLength(sOnHand.ootLength, sWeighOotLength)) ? sOnHand.mm
                                                                                   : BassMm(sWeighOotLength);
    }
    RF3FightStatus status;
    RF3Fight_GetStatus(&status); // the fight that just ended in the catch
    return (status.record >= 0 && SameLength(status.ootLength, sWeighOotLength)) ? status.lengthMm
                                                                                  : BassMm(sWeighOotLength);
}

extern "C" void RF3Size_KeepCaught(float ootLength) {
    RF3FightStatus status;
    RF3Fight_GetStatus(&status);
    bool caught = (status.outcome == RF3_OUTCOME_CAUGHT) || (status.outcome == RF3_OUTCOME_BUKKONUKI);
    sOnHand = {};
    if (!RF3_IsEnabled()) {
        return; // an OoT fish: nothing to remember
    }
    if (caught && status.record >= 0 && status.ootLength == ootLength) {
        sOnHand.valid = true;
        sOnHand.species = status.species;
        sOnHand.record = status.record;
        sOnHand.mm = status.lengthMm;
        sOnHand.weightG = status.weightG;
        sOnHand.ootLength = ootLength;
    }
    LogIdentity("keep", sOnHand, ootLength);
}

extern "C" void RF3Size_SetRecord(int32_t isChild, float ootLength) {
    FishIdentity& r = sRecord[Slot(isChild)];
    if (RF3_IsEnabled() && sOnHand.valid && SameLength(sOnHand.ootLength, ootLength)) {
        r = sOnHand;
    } else {
        r = {}; // a record we can't identify (option off, ...): texts fall back to bass
    }
    if (RF3_IsEnabled()) {
        LogIdentity(isChild ? "record-child" : "record-adult", r, ootLength);
    }
}

// --- The rod as an item (step 15a) ------------------------------------------------------------

extern "C" void RF3Rod_SetOwned(void) {
    if (!RF3_IsEnabled()) {
        return;
    }
    bool first = !sHasRod;
    if (first) {
        LogIdentity("rod-bought", FishIdentity{}, 0.0f);
    }
    sHasRod = true;
    if (first) {
        RF3Stock_GiveStarter("bought"); // step 23d: OoT's pole and its normal lure (was: bass rod + lure 48)
    }
}

extern "C" int32_t RF3Rod_IsOwned(void) {
    // Step 24d: any rod in the stock counts too (a randomizer seed hands rods out without the pole).
    // Step 55a (user's rando test, 2026-10-18): in an RF3 seed only the stock counts. Paying the owner sets hasRod
    // there too, but the shop is a check that gives no rod of its own, so hasRod alone isn't a rod.
    const bool rando = RF3Rando_Active();
    const bool stockRods = RF3Stock_AnyRod() != 0;
    const bool owned = rando ? stockRods : (sHasRod || stockRods);
    static int sLogged = -1; // one log line per change of the answer (this is asked every frame)
    const int key = (rando ? 4 : 0) | (sHasRod ? 2 : 0) | (owned ? 1 : 0);
    if (key != sLogged) {
        sLogged = key;
        std::string dir = Ship::Context::GetPathRelativeToAppDirectory("logs");
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::ofstream out(dir + "/rf3_records.txt", std::ios::app);
        char stamp[32];
        std::time_t now = std::time(nullptr);
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
        char line[128];
        std::snprintf(line, sizeof(line), "%s rod-owned-check rando=%d hasRod=%d stockRods=%d -> %d\n", stamp,
                      rando ? 1 : 0, sHasRod ? 1 : 0, stockRods ? 1 : 0, owned ? 1 : 0);
        out << line;
    }
    return owned ? 1 : 0;
}

extern "C" int32_t RF3Rod_PaidWithoutRod(void) {
    // Step 55c option A (user, 2026-10-18): in an RF3 seed with no rod owned, paying is the shop check and nothing
    // else: Link gets the check's item, no rental, no session (z_fishing.c owner state 5).
    if (!RF3_IsEnabled() || !RF3Rando_Active() || RF3Stock_AnyRod()) {
        return 0;
    }
    RF3Rod_SetOwned(); // the shop check (RF3Stock_GiveStarter("bought") in an RF3 seed)
    RF3Rod_LogEvent("pond-no-rod-paid", 0);
    return 1;
}

extern "C" void RF3Rod_LogEvent(const char* what, int32_t value) {
    if (!RF3_IsEnabled()) {
        return;
    }
    FishIdentity none;
    LogIdentity(what, none, static_cast<float>(value)); // value shows in the ootLength column
}

// --- The mod's records (step 16a) ------------------------------------------------------------

extern "C" int32_t RF3Records_SubmitCatch(int32_t record, int32_t species, float mm, int32_t weightG,
                                          float* previousMm) {
    if (!RF3_IsEnabled() || (record < 0) || (record >= kRecordSlots)) {
        *previousMm = 0.0f;
        return 0;
    }
    if ((sLastSubmitted.record == record) && (sLastSubmitted.mm == mm) && (sLastSubmitted.weightG == weightG)) {
        *previousMm = sLastSubmitted.previousMm; // the same catch again (message rebuilt): same answer
        return sLastSubmitted.newRecord ? 1 : 0;
    }
    BestCatch& best = sBest[record];
    float previous = best.valid ? best.mm : 0.0f;
    bool newRecord = !best.valid || (mm > best.mm);
    if (newRecord) {
        best.valid = true;
        best.species = species;
        best.mm = mm;
        best.weightG = weightG;
    }
    sLastSubmitted = { record, mm, weightG, newRecord, previous };
    *previousMm = previous;

    std::string dir = Ship::Context::GetPathRelativeToAppDirectory("logs");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(dir + "/rf3_records.txt", std::ios::app);
    char stamp[32];
    std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    char line[192];
    std::snprintf(line, sizeof(line), "%s catch record=%d species=%d mm=%.1f g=%d previousMm=%.1f newRecord=%d\n",
                  stamp, record, species, mm, weightG, previous, newRecord ? 1 : 0);
    out << line;
    out.close(); // the unlock below appends to the same file
    RF3Stock_UnlockForCatch(record, species, mm, weightG); // steps 21b/21c/43a: rods, lures, then one item per fish
    return newRecord ? 1 : 0;
}

extern "C" int32_t RF3Records_GetBest(int32_t record, float* mm, int32_t* weightG) {
    if ((record < 0) || (record >= kRecordSlots) || !sBest[record].valid) {
        return 0;
    }
    *mm = sBest[record].mm;
    *weightG = sBest[record].weightG;
    return 1;
}
