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
}

void LoadSection() {
    LoadIdentity("adultRecord", sRecord[0]);
    LoadIdentity("childRecord", sRecord[1]);
}

// New file, or before a file's sections load: no remembered records. The kept fish is not saved by
// OoT either (sFishOnHandLength is not in the save), but it does not survive a file change.
void InitFile(bool isDebug) {
    sRecord[0] = {};
    sRecord[1] = {};
    sOnHand = {};
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
