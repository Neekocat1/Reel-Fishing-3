// Reel Fishing III mod (step 18): the tackle box choice applied to the fishing. See RF3TackleRules.h.

#include "RF3TackleRules.h"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>

#include <ship/Context.h>

#include "RF3Bridge.h"
#include "RF3FishTable.h" // step 23a: TackleTableRecord
#include "RF3TackleAssets.h"

namespace rf3tackle {
namespace {

constexpr int32_t kNone = 99; // "no value" in RF3's per-fish tables; counts as 0 (RF3-MECHANICS 1.4)

int32_t PerRecord(const std::vector<int32_t>& table, int32_t record) {
    record = rf3::TackleTableRecord(record); // step 23a: an OoT fish reads the RF3 record it copies
    if (record < 0 || record >= static_cast<int32_t>(table.size())) {
        return 0;
    }
    return table[record] == kNone ? 0 : table[record];
}

// Step 19c: the items whose per-fish tables add up, for the chosen tackle: line and lure/fly for lure
// and fly boxes; line, float and the bait at its hook size for bait/pole sets (no bait: none). The rod is
// added by the callers. Returns false if the choice can't be resolved in this tackle.json.
struct Items {
    const Item* line = nullptr;
    const Item* item = nullptr;  // lure, fly, or bait at its hook size (nullptr: no bait)
    const Item* flt = nullptr;   // float (bait sets)
    bool bait = false;           // a bait set
};

bool ResolveItems(const Box& box, const RF3TackleChoice& c, Items* out) {
    out->line = FindLine(box, c.line);
    out->bait = box.kind == "bait";
    if (out->bait) {
        out->flt = FindFloat(box, c.floatIdx);
        out->item = BaitAtSize(box, c.bait, c.size);
        return out->line != nullptr && out->flt != nullptr && !out->line->shield.empty() && !out->flt->shield.empty();
    }
    out->item = FindLure(box, c.lure);
    return out->line != nullptr && out->item != nullptr && !out->line->shield.empty() && !out->item->shield.empty();
}

int32_t Sum(const Items& it, const std::vector<int32_t>& rodTable, int32_t record, bool heart) {
    int32_t s = PerRecord(rodTable, record);
    for (const Item* i : { it.line, it.item, it.flt }) {
        if (i != nullptr) {
            s += PerRecord(heart ? i->heart : i->shield, record);
        }
    }
    return s;
}

} // namespace

FightTackle ForFight(int32_t record, int32_t defaultTensionBonus) {
    FightTackle ft;
    ft.tensionBonus = defaultTensionBonus;
    const Assets& a = Get();
    RF3TackleChoice c;
    RF3Tackle_Get(&c);
    ft.box = c.box;
    ft.rod = c.rod;
    ft.line = c.line;
    ft.lure = c.lure;
    ft.floatIdx = c.floatIdx;
    ft.bait = c.bait;
    ft.size = c.size;
    if (!a.loaded || c.box < 0 || c.box >= static_cast<int32_t>(a.boxes.size())) {
        ft.why = "no tackle.json";
        return ft;
    }
    const Box& box = a.boxes[c.box];
    Items items;
    if (box.rodLimits.size() < 5 || box.rodShield.empty() || !ResolveItems(box, c, &items)) {
        ft.why = "tackle.json without step 18a numbers (run tools/rf3_menuassets.py build)";
        return ft;
    }
    // The line step is the chosen line's cell on the Line Dia bar, thin to thick (DESIGN step 18:
    // matches RF3's data for 10 lb = step 2; not confirmed for the other cells).
    int32_t step = -1;
    for (size_t i = 0; i < box.lines.size(); i++) {
        if (box.lines[i].index == c.line) {
            step = static_cast<int32_t>(i);
        }
    }
    if (step < 0 || step > 4) {
        ft.why = "line not on the box's bar";
        return ft;
    }
    ft.tackle.lineStep = step;
    for (int i = 0; i < 5; i++) {
        ft.tackle.tensionLimitBase[i] = static_cast<int16_t>(box.rodLimits[i]);
    }
    ft.tackle.rodValue = box.rodValue;
    ft.tackle.rodLengthM = box.rodLengthMm / 1000.0f;
    ft.tensionBonus = Sum(items, box.rodShield, record, false);
    ft.fromChoice = true;
    return ft;
}

ApproachTackle ForApproach(int32_t record, int32_t defaultSightBonus) {
    ApproachTackle at;
    at.sightBonus = defaultSightBonus;
    const Assets& a = Get();
    RF3TackleChoice c;
    RF3Tackle_Get(&c);
    if (!a.loaded || c.box < 0 || c.box >= static_cast<int32_t>(a.boxes.size())) {
        return at;
    }
    const Box& box = a.boxes[c.box];
    Items items;
    if (box.rodHeart.empty() || !ResolveItems(box, c, &items)) {
        return at; // tackle.json predates step 18a
    }
    at.sightBonus = Sum(items, box.rodHeart, record, true);
    // The appeal comes from the lure, fly or bait (at its hook size). A bait set with no bait has
    // nothing on the hook, so no fish notices it (step 19c, as in RF3).
    const Item* lure = items.item;
    int32_t table = rf3::TackleTableRecord(record); // step 23a
    at.appeal = (lure != nullptr && table >= 0 && table < static_cast<int32_t>(lure->appeal.size()))
                    ? lure->appeal[table]
                    : 0;
    // User's decision (2026-10-07): full RF3 rules. A record whose rod mask lacks the rod's bit never
    // notices the lure (RF3-MECHANICS 2.3).
    if (record >= 0 && record < static_cast<int32_t>(a.fishRodMask.size()) && c.rod >= 0 && c.rod < 16) {
        at.rodAllowed = ((a.fishRodMask[record] >> c.rod) & 1) != 0;
    }
    at.fromChoice = true;
    return at;
}

void LogFightTackle(const FightTackle& ft, int32_t record, float tensionLimit) {
    std::string dir = Ship::Context::GetPathRelativeToAppDirectory("logs");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(dir + "/rf3_records.txt", std::ios::app);
    char stamp[32];
    std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    char line[320];
    std::snprintf(line, sizeof(line),
                  "%s fight-tackle record=%d source=%s box=%d rod=%d line=%d lure=%d float=%d bait=%d size=%d "
                  "step=%d base=%d bonus=%d "
                  "limit=%.1f rodValue=%d rodLength=%.2f%s%s\n",
                  stamp, record, ft.fromChoice ? "choice" : "default", ft.box, ft.rod, ft.line, ft.lure, ft.floatIdx,
                  ft.bait, ft.size,
                  ft.tackle.lineStep, ft.tackle.tensionLimitBase[ft.tackle.lineStep], ft.tensionBonus, tensionLimit,
                  ft.tackle.rodValue, ft.tackle.rodLengthM, ft.why.empty() ? "" : " why=", ft.why.c_str());
    out << line;
}

} // namespace rf3tackle
