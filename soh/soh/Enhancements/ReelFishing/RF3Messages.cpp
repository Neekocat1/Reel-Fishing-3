// Reel Fishing III mod (step 8e): tell a snapped line apart from a lost hook.
// OoT shows the same "got away" text (0x4082) for every escape; with the RF3
// mechanics on, this replaces it after an RF3 line break or hook-out.

// Step 12b: the catch message shows the fish's RF3 length and weight, and the pond
// owner's weigh-in shows RF3 lengths instead of OoT's pounds. See docs/DESIGN.md step 12b.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#include <libultraship/bridge/consolevariablebridge.h>
#include <ship/Context.h>

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/custom-message/CustomMessageManager.h"
#include "soh/ShipInit.hpp"
#include "soh/cvar_prefixes.h"
#include "RF3Bridge.h"
#include "RF3SizeMap.h"

extern "C" {
#include "variables.h"
#include "macros.h"
}

static constexpr uint16_t kFishGotAwayText = 0x4082;
// OoT's "you caught a fish, keep it?" texts (by size; 0x4099 = loach). z_fishing.c fish state 6.
static constexpr uint16_t kCatchTexts[] = { 0x4083, 0x4091, 0x409F, 0x4099 };
// The pond's text range (owner dialogue, weigh-in, records).
static constexpr uint16_t kPondTextFirst = 0x4080;
static constexpr uint16_t kPondTextLast = 0x40B2;
// OoT message codes that print fish numbers: 0x1D = minigameScore (the fish being weighed,
// in pounds); 0x1E 0x02 = the fishing record converted to pounds (z_message_PAL.c).
static const std::string kFishInfoCode = "\x1D";
static const std::string kFishRecordCode = std::string("\x1E\x02", 2);

static float MmToInches(float mm) {
    return mm / 25.4f;
}

static std::string InchesText(float ootLength) {
    char text[16];
    std::snprintf(text, sizeof(text), "%.1f", MmToInches(rf3::OotLengthToRf3Mm(ootLength)));
    return text;
}

// OoT stores the record as a length: low 7 bits for child Link, bits 24-30 for adult.
static float RecordOotLength() {
    s32 hs = HIGH_SCORE(HS_FISHING);
    return static_cast<float>(LINK_IS_CHILD ? (hs & 0x7F) : ((hs >> 24) & 0x7F));
}

// First time each pond text opens, write OoT's original bytes to logs/rf3_pond_texts.txt so
// the length edit can be checked against the real wording (it is not in SoH's source).
static void DumpPondTextOnce(uint16_t textId, const std::string& raw) {
    static std::set<uint16_t> sDumped;
    if (!sDumped.insert(textId).second) {
        return;
    }
    std::string dir = Ship::Context::GetPathRelativeToAppDirectory("logs");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(dir + "/rf3_pond_texts.txt", std::ios::app);
    char head[16];
    std::snprintf(head, sizeof(head), "0x%04X: ", textId);
    out << head;
    for (unsigned char c : raw) {
        if (c >= 0x20 && c < 0x7F && c != '\\') {
            out << static_cast<char>(c);
        } else {
            char hex[8];
            std::snprintf(hex, sizeof(hex), "\\x%02X", c);
            out << hex;
        }
    }
    out << '\n';
}

static bool IsCatchText(uint16_t textId) {
    for (uint16_t id : kCatchTexts) {
        if (id == textId) {
            return true;
        }
    }
    return false;
}

static void BuildRF3CatchMessage(bool* loadFromMessageTable) {
    RF3FightStatus status;
    RF3Fight_GetStatus(&status); // the fight that just ended in the catch
    char text[160];
    std::snprintf(text, sizeof(text),
                  "You caught a %%rlargemouth bass%%w!&%%g%.1f in%%w (%.0f mm), %%g%.2f lb%%w (%d g)&\x1B%%gKeep it&"
                  "Release it%%w",
                  MmToInches(status.lengthMm), status.lengthMm, status.weightG / 453.592f, status.weightG);
    CustomMessage msg = CustomMessage(text);
    msg.AutoFormat();
    msg.LoadIntoFont();
    *loadFromMessageTable = false;
}

static void BuildRF3PondMessage(uint16_t textId, bool* loadFromMessageTable) {
    CustomMessage msg = CustomMessage::LoadVanillaMessageTableEntry(textId);
    std::string raw = msg.GetEnglish(MF_RAW);
    DumpPondTextOnce(textId, raw);
    bool hasFish = raw.find(kFishInfoCode) != std::string::npos;
    bool hasRecord = raw.find(kFishRecordCode) != std::string::npos;
    if (!hasFish && !hasRecord) {
        return; // no fish number in this text: OoT's own message
    }
    if (hasRecord) {
        msg.Replace(std::string(kFishRecordCode), InchesText(RecordOotLength()));
    }
    if (hasFish) {
        msg.Replace(std::string(kFishInfoCode), InchesText(RF3Size_GetLengthToWeigh()));
    }
    // Unit words. Checked against logs/rf3_pond_texts.txt for 0x4080 ("weighed ... pounds") and
    // 0x408B ("it weighs ... pounds"); the other weigh-in texts are not dumped yet.
    msg.Replace("pounds", "inches");
    msg.Replace("pound", "inch");
    msg.Replace("lbs.", "in.");
    msg.Replace("lbs", "in");
    msg.Replace("weighed", "measured"); // before "weigh", which would give "measureed"
    msg.Replace("weighs", "measures");
    msg.Replace("weigh", "measure");
    msg.LoadIntoFont();
    *loadFromMessageTable = false;
}

static void BuildRF3SizeMessages(uint16_t* textId, bool* loadFromMessageTable) {
    if (IsCatchText(*textId)) {
        BuildRF3CatchMessage(loadFromMessageTable);
    } else if (*textId >= kPondTextFirst && *textId <= kPondTextLast && *textId != kFishGotAwayText) {
        BuildRF3PondMessage(*textId, loadFromMessageTable);
    }
}

static void BuildRF3EscapeMessage(uint16_t* textId, bool* loadFromMessageTable) {
    RF3FightStatus status;
    RF3Fight_GetStatus(&status); // after the fight ended: its final values
    if (status.active) {
        return;
    }
    CustomMessage msg;
    if (status.outcome == RF3_OUTCOME_LINEBREAK) {
        msg = CustomMessage("%rThe line snapped!%w&The fish got away with your lure.");
    } else if (status.outcome == RF3_OUTCOME_HOOKOUT) {
        msg = CustomMessage("The hook came loose...&The fish got away.");
    } else {
        return; // any other escape keeps OoT's own text
    }
    msg.AutoFormat();
    msg.LoadIntoFont();
    *loadFromMessageTable = false;
}

static void RegisterRF3Messages() {
    COND_ID_HOOK(OnOpenText, kFishGotAwayText, CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.Enabled"), 0),
                 BuildRF3EscapeMessage);
    // Step 12b: one general hook for the catch texts and the pond's weigh-in texts.
    COND_HOOK(OnOpenText, CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.Enabled"), 0), BuildRF3SizeMessages);
}

static RegisterShipInitFunc initRF3Messages(RegisterRF3Messages, { CVAR_ENHANCEMENT("ReelFishing3.Enabled") });
