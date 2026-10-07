// Reel Fishing III mod (step 17c): the tackle choice made on the tackle box screen.
// RF3's tackle box screen picks a box (bass or trout; the box decides the rod and reel), one of the
// box's 5 lines, one of its lures, and a target fish. The choice is stored as RF3's own indices
// (rigdata.fdt RLIN / RLRE record, fishdata.fdt record) so later steps can add items without
// breaking saves. It is saved per file in its own section. It does not change the fishing yet.
// See docs/DESIGN.md step 17.

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

#include <ship/Context.h>

#include "soh/SaveManager.h"
#include "soh/ShipInit.hpp"
#include "RF3Bridge.h"
#include "RF3TackleAssets.h"

namespace {

// Range limits. Which items belong to which box comes from tackle.json (made from the user's disc by
// tools/rf3_menuassets.py, loaded by RF3TackleAssets.cpp, step 17d); without it only ranges are checked.
constexpr int32_t kLineCount = 28;   // rigdata.fdt RLIN records
constexpr int32_t kLureCount = 91;   // rigdata.fdt RLRE records
constexpr int32_t kFishRecords = 50; // fishdata.fdt records

// Today's tackle (RF3Params.h kDefaultTackle): Lure Rod/Bass, 10 lb line (RLIN 9), Suspended
// Vibration G (RLRE 48). The trout box starts on the same 10 lb line and Trout Spinner Basic (RLRE 0).
// Target: Largemouth Bass (Juv), fishdata record 14, which both boxes list.
constexpr int32_t kDefaultBox = RF3_TACKLE_BOX_BASS;
constexpr int32_t kDefaultLine[RF3_TACKLE_BOXES] = { 9, 9 };
constexpr int32_t kDefaultLure[RF3_TACKLE_BOXES] = { 48, 0 };
constexpr int32_t kDefaultTarget = 14;
constexpr int32_t kBoxRod[RF3_TACKLE_BOXES] = { 0, 1 }; // RROD: box n uses rod n (Lure Rod/Bass, /Trout)

int32_t sBox = kDefaultBox;
int32_t sLine[RF3_TACKLE_BOXES] = { kDefaultLine[0], kDefaultLine[1] };
int32_t sLure[RF3_TACKLE_BOXES] = { kDefaultLure[0], kDefaultLure[1] };
int32_t sTarget = kDefaultTarget;

bool InRange(int32_t v, int32_t count) {
    return (v >= 0) && (v < count);
}

// One line per change in logs/rf3_records.txt, next to the other rod events.
void LogChoice(const char* what, const char* why) {
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
    char line[224];
    std::snprintf(line, sizeof(line),
                  "%s %s box=%d rod=%d line=%d lure=%d target=%d bassLine=%d bassLure=%d troutLine=%d "
                  "troutLure=%d why=%s\n",
                  stamp, what, sBox, kBoxRod[sBox], sLine[sBox], sLure[sBox], sTarget, sLine[0], sLure[0], sLine[1],
                  sLure[1], why ? why : "-");
    out << line;
}

// Step 17d: with tackle.json loaded, an item must be in its box's list.
const rf3tackle::Box* AssetBox(int32_t box) {
    const rf3tackle::Assets& a = rf3tackle::Get();
    return (a.loaded && InRange(box, static_cast<int32_t>(a.boxes.size()))) ? &a.boxes[box] : nullptr;
}

// Puts every stored choice back into its box's lists: the default if the box has it, else the box's
// middle line / first lure / first target. Logs "tackle-fix" if anything changed. No-op without assets.
bool Validate(const char* why) {
    bool fixed = false;
    for (int32_t b = 0; b < RF3_TACKLE_BOXES; b++) {
        const rf3tackle::Box* box = AssetBox(b);
        if (box == nullptr) {
            return false;
        }
        if (rf3tackle::FindLine(*box, sLine[b]) == nullptr && !box->lines.empty()) {
            sLine[b] = rf3tackle::FindLine(*box, kDefaultLine[b]) ? kDefaultLine[b]
                                                                   : box->lines[box->lines.size() / 2].index;
            fixed = true;
        }
        if (rf3tackle::FindLure(*box, sLure[b]) == nullptr && !box->lures.empty()) {
            sLure[b] = rf3tackle::FindLure(*box, kDefaultLure[b]) ? kDefaultLure[b] : box->lures.front().index;
            fixed = true;
        }
    }
    const rf3tackle::Box* box = AssetBox(sBox);
    if (rf3tackle::FindTarget(*box, sTarget) == nullptr && !box->targets.empty()) {
        sTarget = rf3tackle::FindTarget(*box, kDefaultTarget) ? kDefaultTarget : box->targets.front().record;
        fixed = true;
    }
    if (fixed) {
        LogChoice("tackle-fix", why);
    }
    return fixed;
}

void ResetChoice() {
    sBox = kDefaultBox;
    for (int b = 0; b < RF3_TACKLE_BOXES; b++) {
        sLine[b] = kDefaultLine[b];
        sLure[b] = kDefaultLure[b];
    }
    sTarget = kDefaultTarget;
}

// --- Save file section "reelFishing3Tackle" -----------------------------------------------------

void SaveSection(const SaveContext& saveContext, int sectionID, bool fullSave) {
    SaveManager::Instance->SaveData("box", sBox);
    SaveManager::Instance->SaveArray("line", RF3_TACKLE_BOXES,
                                     [](size_t i) { SaveManager::Instance->SaveData("", sLine[i]); });
    SaveManager::Instance->SaveArray("lure", RF3_TACKLE_BOXES,
                                     [](size_t i) { SaveManager::Instance->SaveData("", sLure[i]); });
    SaveManager::Instance->SaveData("target", sTarget);
}

void LoadSection() {
    SaveManager::Instance->LoadData("box", sBox, kDefaultBox);
    SaveManager::Instance->LoadArray("line", RF3_TACKLE_BOXES, [](size_t i) {
        SaveManager::Instance->LoadData("", sLine[i], kDefaultLine[i]);
    });
    SaveManager::Instance->LoadArray("lure", RF3_TACKLE_BOXES, [](size_t i) {
        SaveManager::Instance->LoadData("", sLure[i], kDefaultLure[i]);
    });
    SaveManager::Instance->LoadData("target", sTarget, kDefaultTarget);
    // A damaged or hand-edited save must not leave an index out of range.
    bool fixed = false;
    if (!InRange(sBox, RF3_TACKLE_BOXES)) {
        sBox = kDefaultBox;
        fixed = true;
    }
    for (int b = 0; b < RF3_TACKLE_BOXES; b++) {
        if (!InRange(sLine[b], kLineCount)) {
            sLine[b] = kDefaultLine[b];
            fixed = true;
        }
        if (!InRange(sLure[b], kLureCount)) {
            sLure[b] = kDefaultLure[b];
            fixed = true;
        }
    }
    if (!InRange(sTarget, kFishRecords)) {
        sTarget = kDefaultTarget;
        fixed = true;
    }
    LogChoice("tackle-load", fixed ? "out-of-range-fixed" : "save");
    Validate("load-not-in-box");
}

// New file, or before a file's sections load (an old save has no tackle section: defaults stay).
void InitFile(bool isDebug) {
    ResetChoice();
}

void RegisterRF3Tackle() {
    SaveManager::Instance->AddInitFunction(InitFile);
    SaveManager::Instance->AddSaveFunction("reelFishing3Tackle", 1, SaveSection, true, SECTION_PARENT_NONE);
    SaveManager::Instance->AddLoadFunction("reelFishing3Tackle", 1, LoadSection);
}

} // namespace

static RegisterShipInitFunc initRF3Tackle(RegisterRF3Tackle);

extern "C" void RF3Tackle_Get(RF3TackleChoice* out) {
    out->box = sBox;
    out->rod = kBoxRod[sBox];
    out->line = sLine[sBox];
    out->lure = sLure[sBox];
    out->target = sTarget;
}

extern "C" int32_t RF3Tackle_GetBoxLine(int32_t box) {
    return InRange(box, RF3_TACKLE_BOXES) ? sLine[box] : -1;
}

extern "C" int32_t RF3Tackle_GetBoxLure(int32_t box) {
    return InRange(box, RF3_TACKLE_BOXES) ? sLure[box] : -1;
}

extern "C" int32_t RF3Tackle_SetBox(int32_t box, const char* why) {
    if (!InRange(box, RF3_TACKLE_BOXES)) {
        return 0;
    }
    if (box != sBox) {
        sBox = box;
        LogChoice("tackle-select", why);
        Validate("target-not-in-new-box"); // the target list differs per box
    }
    return 1;
}

extern "C" int32_t RF3Tackle_SetLine(int32_t box, int32_t line, const char* why) {
    if (!InRange(box, RF3_TACKLE_BOXES) || !InRange(line, kLineCount)) {
        return 0;
    }
    if (const rf3tackle::Box* b = AssetBox(box); b != nullptr && rf3tackle::FindLine(*b, line) == nullptr) {
        return 0;
    }
    if (line != sLine[box]) {
        sLine[box] = line;
        LogChoice("tackle-select", why);
    }
    return 1;
}

extern "C" int32_t RF3Tackle_SetLure(int32_t box, int32_t lure, const char* why) {
    if (!InRange(box, RF3_TACKLE_BOXES) || !InRange(lure, kLureCount)) {
        return 0;
    }
    if (const rf3tackle::Box* b = AssetBox(box); b != nullptr && rf3tackle::FindLure(*b, lure) == nullptr) {
        return 0;
    }
    if (lure != sLure[box]) {
        sLure[box] = lure;
        LogChoice("tackle-select", why);
    }
    return 1;
}

extern "C" int32_t RF3Tackle_SetTarget(int32_t record, const char* why) {
    if (!InRange(record, kFishRecords)) {
        return 0;
    }
    if (const rf3tackle::Box* b = AssetBox(sBox); b != nullptr && rf3tackle::FindTarget(*b, record) == nullptr) {
        return 0;
    }
    if (record != sTarget) {
        sTarget = record;
        LogChoice("tackle-select", why);
    }
    return 1;
}
