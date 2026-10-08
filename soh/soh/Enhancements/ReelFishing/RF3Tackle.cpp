// Reel Fishing III mod (step 17c): the tackle choice made on the tackle box screen.
// RF3's tackle box screen picks a box (bass or trout; the box decides the rod and reel), one of the
// box's 5 lines, one of its lures, and a target fish. The choice is stored as RF3's own indices
// (rigdata.fdt RLIN / RLRE record, fishdata.fdt record) so later steps can add items without
// breaking saves. It is saved per file in its own section. It does not change the fishing yet.
// See docs/DESIGN.md step 17.

#include <algorithm>
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
// Step 19a: the other boxes start unset (-1); with tackle.json loaded, Validate() gives them their
// middle line and first item (lure or fly). Box n uses rod n.
// Target: Largemouth Bass (Juv), fishdata record 14, which both boxes list.
constexpr int32_t kUnset = -1;
constexpr int32_t kDefaultBox = RF3_TACKLE_BOX_BASS;
constexpr int32_t kDefaultLine[RF3_TACKLE_BOXES] = { 9, 9, kUnset, kUnset, kUnset, kUnset, kUnset, kUnset, kUnset, kUnset };
constexpr int32_t kDefaultLure[RF3_TACKLE_BOXES] = { 48, 0, kUnset, kUnset, kUnset, kUnset, kUnset, kUnset, kUnset, kUnset };
constexpr int32_t kDefaultTarget = 14;

// Step 19c, bait/pole sets: float (first of the set until chosen), float depth and hook size (the
// middle cell), bait (none, as RF3's sets start: "---" in the user's screenshots).
constexpr int32_t kNoBait = -1;
constexpr int32_t kDefaultDepth = 2;
constexpr int32_t kDefaultSize = 2;
constexpr int32_t kFloatCount = 6; // RFLT records
constexpr int32_t kBaitCount = 12; // RBIT groups

int32_t sBox = kDefaultBox;
int32_t sLine[RF3_TACKLE_BOXES] = {};
int32_t sLure[RF3_TACKLE_BOXES] = {};
int32_t sFloat[RF3_TACKLE_BOXES] = {};
int32_t sDepth[RF3_TACKLE_BOXES] = {};
int32_t sBait[RF3_TACKLE_BOXES] = {};
int32_t sSize[RF3_TACKLE_BOXES] = {};
int32_t sTarget = kDefaultTarget;

bool InRange(int32_t v, int32_t count) {
    return (v >= 0) && (v < count);
}

int32_t BoxCount() {
    const rf3tackle::Assets& a = rf3tackle::Get();
    return a.loaded ? std::min(static_cast<int32_t>(a.boxes.size()), static_cast<int32_t>(RF3_TACKLE_BOXES)) : 2;
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
    char line[288];
    std::snprintf(line, sizeof(line),
                  "%s %s box=%d rod=%d line=%d lure=%d float=%d depth=%d bait=%d size=%d target=%d bassLine=%d "
                  "bassLure=%d troutLine=%d troutLure=%d why=%s\n",
                  stamp, what, sBox, sBox, sLine[sBox], sLure[sBox], sFloat[sBox], sDepth[sBox], sBait[sBox],
                  sSize[sBox], sTarget, sLine[0], sLure[0], sLine[1], sLure[1], why ? why : "-");
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
    if (AssetBox(sBox) == nullptr) {
        return false; // no tackle.json, or the saved box isn't in it (checked at load)
    }
    for (int32_t b = 0; b < RF3_TACKLE_BOXES; b++) {
        const rf3tackle::Box* box = AssetBox(b);
        if (box == nullptr) {
            continue; // a box this tackle.json doesn't have (bait sets before step 19c)
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
        // Step 19c: a bait set's float, depth, bait and hook size.
        if (rf3tackle::FindFloat(*box, sFloat[b]) == nullptr && !box->floats.empty()) {
            sFloat[b] = box->floats.front().index;
            fixed = true;
        }
        if (!box->baits.empty() && sBait[b] != kNoBait && rf3tackle::FindBait(*box, sBait[b]) == nullptr) {
            sBait[b] = kNoBait;
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
        sFloat[b] = kUnset;
        sDepth[b] = kDefaultDepth;
        sBait[b] = kNoBait;
        sSize[b] = kDefaultSize;
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
    // Step 19c (added to the same section; older saves load the defaults).
    SaveManager::Instance->SaveArray("float", RF3_TACKLE_BOXES,
                                     [](size_t i) { SaveManager::Instance->SaveData("", sFloat[i]); });
    SaveManager::Instance->SaveArray("depth", RF3_TACKLE_BOXES,
                                     [](size_t i) { SaveManager::Instance->SaveData("", sDepth[i]); });
    SaveManager::Instance->SaveArray("bait", RF3_TACKLE_BOXES,
                                     [](size_t i) { SaveManager::Instance->SaveData("", sBait[i]); });
    SaveManager::Instance->SaveArray("size", RF3_TACKLE_BOXES,
                                     [](size_t i) { SaveManager::Instance->SaveData("", sSize[i]); });
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
    SaveManager::Instance->LoadArray("float", RF3_TACKLE_BOXES,
                                     [](size_t i) { SaveManager::Instance->LoadData("", sFloat[i], kUnset); });
    SaveManager::Instance->LoadArray("depth", RF3_TACKLE_BOXES,
                                     [](size_t i) { SaveManager::Instance->LoadData("", sDepth[i], kDefaultDepth); });
    SaveManager::Instance->LoadArray("bait", RF3_TACKLE_BOXES,
                                     [](size_t i) { SaveManager::Instance->LoadData("", sBait[i], kNoBait); });
    SaveManager::Instance->LoadArray("size", RF3_TACKLE_BOXES,
                                     [](size_t i) { SaveManager::Instance->LoadData("", sSize[i], kDefaultSize); });
    // A damaged or hand-edited save must not leave an index out of range. kUnset is allowed.
    bool fixed = false;
    if (!InRange(sBox, BoxCount())) {
        sBox = kDefaultBox;
        fixed = true;
    }
    for (int b = 0; b < RF3_TACKLE_BOXES; b++) {
        if (sLine[b] != kUnset && !InRange(sLine[b], kLineCount)) {
            sLine[b] = kDefaultLine[b];
            fixed = true;
        }
        if (sLure[b] != kUnset && !InRange(sLure[b], kLureCount)) {
            sLure[b] = kDefaultLure[b];
            fixed = true;
        }
        if (sFloat[b] != kUnset && !InRange(sFloat[b], kFloatCount)) {
            sFloat[b] = kUnset;
            fixed = true;
        }
        if (!InRange(sDepth[b], 5)) {
            sDepth[b] = kDefaultDepth;
            fixed = true;
        }
        if (sBait[b] != kNoBait && !InRange(sBait[b], kBaitCount)) {
            sBait[b] = kNoBait;
            fixed = true;
        }
        if (!InRange(sSize[b], 5)) {
            sSize[b] = kDefaultSize;
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
    ResetChoice(); // the arrays' defaults before any file loads
    SaveManager::Instance->AddInitFunction(InitFile);
    SaveManager::Instance->AddSaveFunction("reelFishing3Tackle", 1, SaveSection, true, SECTION_PARENT_NONE);
    SaveManager::Instance->AddLoadFunction("reelFishing3Tackle", 1, LoadSection);
}

} // namespace

static RegisterShipInitFunc initRF3Tackle(RegisterRF3Tackle);

extern "C" void RF3Tackle_Get(RF3TackleChoice* out) {
    out->box = sBox;
    out->rod = sBox; // box n uses rod n
    out->line = sLine[sBox];
    out->lure = sLure[sBox];
    out->target = sTarget;
    out->floatIdx = sFloat[sBox];
    out->depth = sDepth[sBox];
    out->bait = sBait[sBox];
    out->size = sSize[sBox];
}

extern "C" int32_t RF3Tackle_GetBoxLine(int32_t box) {
    return InRange(box, RF3_TACKLE_BOXES) ? sLine[box] : -1;
}

extern "C" int32_t RF3Tackle_GetBoxLure(int32_t box) {
    return InRange(box, RF3_TACKLE_BOXES) ? sLure[box] : -1;
}

extern "C" int32_t RF3Tackle_BoxCount(void) {
    return BoxCount();
}

extern "C" int32_t RF3Tackle_SetBox(int32_t box, const char* why) {
    if (!InRange(box, BoxCount())) {
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

// --- Step 19c: bait/pole sets ------------------------------------------------------------------

namespace {
// A bait set of the loaded tackle.json, or nullptr.
const rf3tackle::Box* BaitBox(int32_t box) {
    const rf3tackle::Box* b = AssetBox(box);
    return (b != nullptr && b->kind == "bait") ? b : nullptr;
}

int32_t SetIfChanged(int32_t* slot, int32_t value, const char* why) {
    if (*slot != value) {
        *slot = value;
        LogChoice("tackle-select", why);
    }
    return 1;
}
} // namespace

extern "C" int32_t RF3Tackle_SetFloat(int32_t box, int32_t floatIdx, const char* why) {
    const rf3tackle::Box* b = BaitBox(box);
    if (b == nullptr || rf3tackle::FindFloat(*b, floatIdx) == nullptr) {
        return 0;
    }
    return SetIfChanged(&sFloat[box], floatIdx, why);
}

extern "C" int32_t RF3Tackle_SetDepth(int32_t box, int32_t depth, const char* why) {
    if (BaitBox(box) == nullptr || !InRange(depth, 5)) {
        return 0;
    }
    return SetIfChanged(&sDepth[box], depth, why);
}

extern "C" int32_t RF3Tackle_SetBait(int32_t box, int32_t bait, const char* why) {
    const rf3tackle::Box* b = BaitBox(box);
    if (b == nullptr || (bait != kNoBait && rf3tackle::FindBait(*b, bait) == nullptr)) {
        return 0;
    }
    return SetIfChanged(&sBait[box], bait, why);
}

extern "C" int32_t RF3Tackle_SetSize(int32_t box, int32_t size, const char* why) {
    if (BaitBox(box) == nullptr || !InRange(size, 5)) {
        return 0;
    }
    return SetIfChanged(&sSize[box], size, why);
}
