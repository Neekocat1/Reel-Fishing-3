// Reel Fishing III mod (step 8e): tell a snapped line apart from a lost hook.
// OoT shows the same "got away" text (0x4082) for every escape; with the RF3
// mechanics on, this replaces it after an RF3 line break or hook-out.

// Step 12b: the catch message shows the fish's RF3 length and weight, and the pond
// owner's weigh-in shows RF3 lengths instead of OoT's pounds. See docs/DESIGN.md step 12b.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#include <libultraship/bridge/consolevariablebridge.h>
#include <ship/Context.h>

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/custom-message/CustomMessageManager.h"
#include "soh/Enhancements/custom-message/CustomMessageTypes.h" // step 22b: TEXT_RANDOMIZER_CUSTOM_ITEM
#include "soh/ShipInit.hpp"
#include "soh/cvar_prefixes.h"
#include "RF3Bridge.h"
#include "RF3Items.h"
#include "RF3RandoChecks.h" // step 55c: RF3Rando_Active
#include "soh/Enhancements/randomizer/randomizer.h" // RG_RF3_*, IS_RANDO
#include "soh/OTRGlobals.h"
#include "RF3SizeMap.h"
#include "RF3TackleAssets.h" // step 76: RF3's texts from the disc

extern "C" {
#include "functions.h"
#include "variables.h"
#include "macros.h"
extern PlayState* gPlayState;
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

// Step 14d: the length comes from RF3Records.cpp (the fish's own species and size, or the bass
// conversion of the OoT length when no remembered fish matches).
static std::string InchesText(float mm) {
    char text[16];
    std::snprintf(text, sizeof(text), "%.1f", MmToInches(mm));
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
    // Step 14c: the fish's RF3 species name, as RF3 writes it ("an" before a vowel).
    const char* name = (status.speciesName != nullptr && status.speciesName[0] != '\0') ? status.speciesName
                                                                                        : "Largemouth Bass";
    const char* article = (std::strchr("AEIOUaeiou", name[0]) != nullptr) ? "an" : "a";

    // Step 16a: the mod's own record for this fish's RF3 record (juvenile/normal/lunker separate).
    // A new record gets its own line and a chime; the Keep/Release choice moves to a second box.
    float previousMm = 0.0f;
    bool newRecord = RF3Records_SubmitCatch(status.record, status.species, status.lengthMm, status.weightG,
                                            &previousMm) != 0;
    // Steps 21b/21c: what this catch unlocked is shown by OoT's item get after this message (step 22c).
    // Step 16e: outside the pond the fish is released after the catch: no Keep choice, one box.
    bool releaseOnly = (gPlayState != nullptr) && (gPlayState->sceneNum != SCENE_FISHING_POND);
    char recordLine[64] = "";
    if (releaseOnly) {
        std::snprintf(recordLine, sizeof(recordLine), "%s", newRecord ? "&%yNew record!%w" : "");
        if (newRecord) {
            Audio_PlaySfxGeneral(NA_SE_SY_CORRECT_CHIME, &gSfxDefaultPos, 4, &gSfxDefaultFreqAndVolScale,
                                 &gSfxDefaultFreqAndVolScale, &gSfxDefaultReverb);
        }
        char text[640];
        std::snprintf(text, sizeof(text), "You caught %s %%r%s%%w!&%%g%.1f in%%w (%.0f mm), %%g%.2f lb%%w (%d g)%s",
                      article, name, MmToInches(status.lengthMm), status.lengthMm, status.weightG / 453.592f,
                      status.weightG, recordLine);
        CustomMessage msg = CustomMessage(text);
        msg.AutoFormat();
        msg.LoadIntoFont();
        *loadFromMessageTable = false;
        return;
    }
    if (newRecord) {
        if (previousMm > 0.0f) {
            std::snprintf(recordLine, sizeof(recordLine), "&%%yNew record!%%w (was %.1f in)^", MmToInches(previousMm));
        } else {
            std::snprintf(recordLine, sizeof(recordLine), "&%%yNew record!%%w^");
        }
        Audio_PlaySfxGeneral(NA_SE_SY_CORRECT_CHIME, &gSfxDefaultPos, 4, &gSfxDefaultFreqAndVolScale,
                             &gSfxDefaultFreqAndVolScale, &gSfxDefaultReverb);
    } else {
        std::snprintf(recordLine, sizeof(recordLine), "&");
    }
    char text[640];
    std::snprintf(text, sizeof(text),
                  "You caught %s %%r%s%%w!&%%g%.1f in%%w (%.0f mm), %%g%.2f lb%%w (%d g)%s\x1B%%gKeep it&"
                  "Release it%%w",
                  article, name, MmToInches(status.lengthMm), status.lengthMm, status.weightG / 453.592f,
                  status.weightG, recordLine);
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
        msg.Replace(std::string(kFishRecordCode), InchesText(RF3Size_GetRecordMm(LINK_IS_CHILD, RecordOotLength())));
    }
    if (hasFish) {
        msg.Replace(std::string(kFishInfoCode), InchesText(RF3Size_GetWeighMm()));
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

// Step 21d: a refused cast borrows the "fish got away" text (z_fishing.c starts 0x4082) for RF3's reason.
static std::string sNotReady;

extern "C" void RF3Tackle_ShowNotReady(const char* reason) {
    sNotReady = reason != nullptr ? reason : rf3tackle::Text("not_ready", "Your tackle isn't ready."); // step 76
}

static void BuildRF3EscapeMessage(uint16_t* textId, bool* loadFromMessageTable) {
    if (!sNotReady.empty()) {
        CustomMessage notReady = CustomMessage(sNotReady);
        sNotReady.clear();
        notReady.AutoFormat();
        notReady.LoadIntoFont();
        *loadFromMessageTable = false;
        return;
    }
    RF3FightStatus status;
    RF3Fight_GetStatus(&status); // after the fight ended: its final values
    if (status.active) {
        return;
    }
    CustomMessage msg;
    if (status.outcome == RF3_OUTCOME_LINEBREAK) {
        // Step 21e: name what was taken; it comes back on the next scene change.
        int32_t lostKind = -1;
        int32_t lostIndex = -1;
        if (RF3Stock_LastLost(&lostKind, &lostIndex)) {
            std::string text = std::string("%rThe line snapped!%w&The fish got away with your %b") +
                               RF3Stock_Name(lostKind, lostIndex) + "%w.";
            msg = CustomMessage(text);
        } else {
            msg = CustomMessage("%rThe line snapped!%w&The fish got away with your lure.");
        }
    } else if (status.outcome == RF3_OUTCOME_HOOKOUT) {
        msg = CustomMessage("The hook came loose...&The fish got away.");
    } else {
        return; // any other escape keeps OoT's own text
    }
    msg.AutoFormat();
    msg.LoadIntoFont();
    *loadFromMessageTable = false;
}

// Step 22b: the get-item text of an RF3 item. Step 24d: also used by the randomizer's item text
// (ItemMessages.cpp), so a rando seed names the item too.
std::string RF3Items_ItemGetText(int32_t offset) {
    int32_t kind = RF3Items_Kind(offset);
    int32_t index = RF3Items_Index(offset);
    std::string name = RF3Stock_Name(kind, index);
    if (kind == RF3_STOCK_ROD) {
        return "You got the %r" + name + "%w!";
    }
    static const char* kWhat[] = { "", "a lure", "a fly", "bait", "a float" };
    return std::string("You got ") + kWhat[kind] + ": %r" + name + "%w!";
}

// Outside the randomizer, TEXT_RANDOMIZER_CUSTOM_ITEM isn't built by SoH (rando saves only).
static void BuildRF3ItemGetMessage(uint16_t* textId, bool* loadFromMessageTable) {
    if (gPlayState == nullptr) {
        return;
    }
    GetItemEntry entry = GET_PLAYER(gPlayState)->getItemEntry;
    int32_t offset = entry.getItemId - RG_RF3_ROD_0;
    if (entry.modIndex != MOD_RANDOMIZER || offset < 0 || offset >= RF3_ITEM_COUNT) {
        return; // not an RF3 item
    }
    CustomMessage msg = CustomMessage(RF3Items_ItemGetText(offset));
    msg.AutoFormat();
    msg.LoadIntoFont();
    *loadFromMessageTable = false;
    RF3Rod_LogEvent("item-get-text", offset);
}

// Step 55c option A (user, 2026-10-18): in an RF3 seed, once the shop check is done and Link still owns no rod, the
// owner's opening text (with its 20-rupee choice) becomes a plain line: no payment, no session. The owner's state 1
// already closes a text without a choice (SoH's fix for the closed pond under pole shuffle).
static void BuildRF3NoRodPondMessage(uint16_t* textId, bool* loadFromMessageTable) {
    if (!RF3Rando_Active() || !Flags_GetRandomizerInf(RAND_INF_RF3_POND_SHOP) || RF3Stock_AnyRod()) {
        return;
    }
    CustomMessage msg = CustomMessage("You'll need a rod of your own to fish here.");
    msg.AutoFormat();
    msg.LoadIntoFont();
    *loadFromMessageTable = false;
    RF3Rod_LogEvent("pond-no-rod-text", *textId);
}

static void RegisterRF3Messages() {
    COND_ID_HOOK(OnOpenText, kFishGotAwayText, CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.Enabled"), 0),
                 BuildRF3EscapeMessage);
    COND_ID_HOOK(OnOpenText, TEXT_RANDOMIZER_CUSTOM_ITEM, !IS_RANDO, BuildRF3ItemGetMessage); // step 22b
    COND_ID_HOOK(OnOpenText, TEXT_FISHING_POND_START, IS_RANDO, BuildRF3NoRodPondMessage);     // step 55c
    COND_ID_HOOK(OnOpenText, TEXT_FISHING_POND_START_MET, IS_RANDO, BuildRF3NoRodPondMessage); // step 55c
    // Step 12b: one general hook for the catch texts and the pond's weigh-in texts.
    COND_HOOK(OnOpenText, CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.Enabled"), 0), BuildRF3SizeMessages);
}

static RegisterShipInitFunc initRF3Messages(RegisterRF3Messages, { CVAR_ENHANCEMENT("ReelFishing3.Enabled"), "IS_RANDO" });
