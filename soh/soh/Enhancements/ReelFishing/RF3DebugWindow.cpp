#include "RF3DebugWindow.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <libultraship/bridge/consolevariablebridge.h>

#include "soh/cvar_prefixes.h"
#include "RF3Bridge.h"
#include "RF3Conditions.h" // step 57c-3
#include "RF3Items.h"
#include "RF3FishModel.h"
#include "RF3FishView.h" // step 61b
#include "RF3Vibration.h" // step 62c
#include "RF3Sound.h"     // step 63b
#include "RF3RodModel.h"
#include "RF3LureModel.h"
#include "RF3TackleAssets.h"

namespace {

const char* ModeName(int32_t mode) {
    switch (mode) {
        case 0: return "none";
        case 2: return "HIT (hook set)";
        case 5: return "HOOKOVER";
        case 6: return "LINEBREAK";
        case 7: return "STOP";
        case 8: return "RESIST";
        case 9: return "STRUGGLE";
        case 10: return "AWAY";
        case 11: return "FOLLOW";
        case 12: return "STIFFEN";
        case 15: return "BUKKONUKI";
        case 16: return "CATCH";
        default: return "?";
    }
}

const char* ModeHint(int32_t mode) {
    switch (mode) {
        case 7: return "pauses: give the expected input inside the window";
        case 9: return "thrashing: reeling raises tension fast";
        case 10: return "running away: reel + rod up to pump line back";
        case 11: return "swimming toward you: reel in now";
        case 12: return "turning: reel choice can decide the next run";
        default: return "";
    }
}

const char* OutcomeName(int32_t outcome) {
    switch (outcome) {
        case RF3_OUTCOME_NONE: return "-";
        case RF3_OUTCOME_CAUGHT: return "CAUGHT";
        case RF3_OUTCOME_LINEBREAK: return "LINE BREAK";
        case RF3_OUTCOME_HOOKOUT: return "HOOK CAME OUT";
        case RF3_OUTCOME_MISSED: return "MISSED HOOK SET";
        case RF3_OUTCOME_BUKKONUKI: return "YANKED OUT";
        default: return "?";
    }
}

void Bar(const char* label, float value, float max) {
    char text[48];
    std::snprintf(text, sizeof(text), "%.1f / %.0f", value, max);
    ImGui::Text("%-9s", label);
    ImGui::SameLine();
    ImGui::ProgressBar(max > 0.0f ? value / max : 0.0f, ImVec2(-1.0f, 0.0f), text);
}

} // namespace

static void DrawApproach() {
    RF3ApproachStatus a;
    RF3Approach_GetStatus(&a);
    char text[48];
    std::snprintf(text, sizeof(text), "%.2f", a.meter);
    ImGui::Text("%-9s", "Lure act.");
    ImGui::SameLine();
    ImGui::ProgressBar(a.meter, ImVec2(-1.0f, 0.0f), text);
    ImGui::Text("(reel while the lure moves: +0.95/s; still: -0.7/s; drifting: -0.5/s)");
    if (a.chaser < 0) {
        ImGui::Text("Chasing fish: none");
    } else {
        const char* mode = a.chaserMode == 1 ? "CHASE" : (a.chaserMode == 3 ? "WAIT (inspecting)" : "STRIKE (lunging)");
        ImGui::Text("Chasing fish: #%d  %s", a.chaser, mode);
        ImGui::Text("  remembers %.2f of lure action: %s", a.chaserMemory,
                    a.chaserInterested ? "interested" : "NOT interested (will give up)");
        if (a.chaserMode == 3) {
            ImGui::Text("  inspecting for %.1f s more, then chases again", a.chaserWaitLeft);
        }
    }
    ImGui::Text("Sight range %.1f m (activity %d, tackle heart %+d, lure appeal %d/15)   wary fish: %d", a.sightRangeM,
                a.activityLevel, a.sightBonus, a.lureAppeal, a.waryFish);
    ImGui::TextUnformatted(RF3Conditions_Describe().c_str()); // step 57c-3
    ImGui::Text("Fish ignoring this tackle this visit: %d can't take the rod, %d don't see the lure (appeal 0)",
                a.blockedRod, a.blockedAppeal); // step 18c
    float power, factor;
    RF3Cast_GetLast(&power, &factor);
    ImGui::Text("Last cast: power %.2f -> %.0f%% of a full cast (hold B through the swing for full)", power,
                factor * 100.0f);
}

// Step 17c/17d: the tackle choice, changeable here until the tackle box screen exists. With
// mods/rf3_tackle.o2r loaded (17d) it shows RF3's names and heart/shield values; without it, RF3's
// index numbers. Saved with the file; the fishing ignores it.
static void HeartShieldText(const char* label, const rf3tackle::HeartShield& hs) {
    if (hs) {
        ImGui::Text("  %-13s heart %+d  shield %+d", label, (*hs)[0], (*hs)[1]);
    } else {
        ImGui::Text("  %-13s (not usable for this target)", label);
    }
}

template <typename T, typename Name, typename Key>
static bool PickFrom(const char* label, const std::vector<T>& items, int32_t current, Name name, Key key,
                     int32_t* picked) {
    const T* cur = nullptr;
    for (const T& i : items) {
        if (key(i) == current) {
            cur = &i;
        }
    }
    bool changed = false;
    if (ImGui::BeginCombo(label, cur ? name(*cur).c_str() : "?")) {
        for (const T& i : items) {
            if (ImGui::Selectable(name(i).c_str(), key(i) == current)) {
                *picked = key(i);
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

static void DrawStock() {
    static const char* kKinds[RF3_STOCK_KINDS] = { "Rods", "Lures", "Flies", "Baits", "Floats" };
    if (!ImGui::CollapsingHeader("Tackle stock (saved; step 21b: one item per catch)")) {
        return;
    }
    int32_t lastKind = -1;
    int32_t lastIndex = -1;
    RF3Stock_LastUnlock(&lastKind, &lastIndex);
    ImGui::Text("Fish caught (unlock count): %u   last unlock: %s %s", RF3Stock_CatchCount(),
                RF3Stock_KindName(lastKind), lastKind >= 0 ? RF3Stock_Name(lastKind, lastIndex) : "-");
    // Step 21c: progress toward the rod conditions (docs/DESIGN.md step 21).
    RF3RodProgress p;
    RF3Stock_GetRodProgress(&p);
    ImGui::TextWrapped("Rod conditions: any fish %d/5 (bass rod), bass %d/3 (trout rod), trout %d/3 (salmon rod), "
                       "salmon %d/3 (fly rod), "
                       "with Fly Rod %d/5 (big game), Largemouth > 14 in %s (pole S), with Pole S %d/5 (pole L), "
                       "Pale Chub %s (crucian), Bitterling > 3 in %s (all-purpose), Bitterling %s (bitterling rod)",
                       p.total, p.bass, p.trout, p.salmon, p.withFlyRod, p.largemouthOver14in ? "yes" : "no", p.withPoleS,
                       p.paleChub ? "yes" : "no", p.bitterlingOver3in ? "yes" : "no", p.bitterling ? "yes" : "no");
    for (int32_t k = 0; k < RF3_STOCK_KINDS; k++) {
        std::string list;
        int32_t have = 0;
        for (int32_t i = 0; i < RF3Stock_Size(k); i++) {
            if (RF3Stock_Has(k, i)) {
                have++;
                list += (list.empty() ? "" : ", ") + std::to_string(i) + " " + RF3Stock_Name(k, i) + " x" +
                        std::to_string(RF3Stock_Count(k, i));
                if (RF3Stock_Lost(k, i) > 0) { // step 21e: back on the next scene change
                    list += " (" + std::to_string(RF3Stock_Lost(k, i)) + " lost)";
                }
            }
        }
        ImGui::TextWrapped("%s %d/%d: %s", kKinds[k], have, RF3Stock_Size(k), list.empty() ? "-" : list.c_str());
    }
    // Step 22a: give one item through OoT's item-get (Link holds it up), to check the models and the give path.
    static int giveKind = RF3_STOCK_LURE;
    static int giveIndex = 0;
    ImGui::Combo("Give kind", &giveKind, kKinds, RF3_STOCK_KINDS);
    ImGui::InputInt("Give index", &giveIndex);
    giveIndex = std::max(0, std::min(giveIndex, RF3Stock_Size(giveKind) - 1));
    ImGui::SameLine();
    ImGui::Text("%s", RF3Stock_Name(giveKind, giveIndex));
    if (ImGui::Button("Give with item-get animation")) {
        int32_t offset = RF3Items_Offset(giveKind, giveIndex);
        int32_t ok = RF3Items_GiveWithAnimation(offset);
        RF3Rod_LogEvent(ok ? "item-get-debug" : "item-get-debug-refused", offset);
    }
    float scale = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.GiLureScale"), 0.03f);
    if (ImGui::SliderFloat("Lure model scale (item get)", &scale, 0.005f, 0.2f, "%.3f")) {
        CVarSetFloat(CVAR_ENHANCEMENT("ReelFishing3.GiLureScale"), scale);
    }
    // Step 28: the RF3 models in the item get
    float tackle = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.GiTackleSize"), 60.0f);
    if (ImGui::SliderFloat("RF3 tackle size (item get)", &tackle, 10.0f, 400.0f, "%.0f")) {
        CVarSetFloat(CVAR_ENHANCEMENT("ReelFishing3.GiTackleSize"), tackle);
    }
    float rodLength = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.GiRodLength"), 35.0f);
    if (ImGui::SliderFloat("RF3 rod length (item get, world units)", &rodLength, 10.0f, 200.0f, "%.0f")) {
        CVarSetFloat(CVAR_ENHANCEMENT("ReelFishing3.GiRodLength"), rodLength);
    }
    float rodOffset = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.GiRodOffsetY"), -5.0f);
    if (ImGui::SliderFloat("RF3 rod offset (item get, world units, + = up)", &rodOffset, -60.0f, 60.0f, "%.1f")) {
        CVarSetFloat(CVAR_ENHANCEMENT("ReelFishing3.GiRodOffsetY"), rodOffset);
    }
}

static void DrawTackle() {
    if (!ImGui::CollapsingHeader("Tackle choice (saved; step 18b: the fight uses it, bites not yet)")) {
        return;
    }
    const rf3tackle::Assets& a = rf3tackle::Get();
    ImGui::Text("Tackle box pictures/data (mods/rf3_tackle.o2r): %s, textures %d needed, %d missing",
                a.loaded ? "loaded" : "NOT loaded (run tools/rf3_menuassets.py build)", a.texturesNeeded,
                a.texturesMissing);
    RF3TackleChoice c;
    RF3Tackle_Get(&c);
    // Step 19a: every box tackle.json has; without it, the two lure boxes by number.
    std::vector<std::string> boxNames;
    for (int32_t b = 0; b < RF3Tackle_BoxCount(); b++) {
        boxNames.push_back(a.loaded && b < static_cast<int32_t>(a.boxes.size()) ? a.boxes[b].title
                                                                                : "Box " + std::to_string(b));
    }
    if (ImGui::BeginCombo("Box", c.box < static_cast<int32_t>(boxNames.size()) ? boxNames[c.box].c_str() : "?")) {
        for (int32_t b = 0; b < static_cast<int32_t>(boxNames.size()); b++) {
            if (ImGui::Selectable(boxNames[b].c_str(), b == c.box)) {
                RF3Tackle_SetBox(b, "debug-window");
            }
        }
        ImGui::EndCombo();
    }
    if (!a.loaded || c.box >= static_cast<int32_t>(a.boxes.size())) {
        int line = c.line;
        if (ImGui::InputInt("Line (RLIN 0-27)", &line)) {
            RF3Tackle_SetLine(c.box, line, "debug-window");
        }
        int lure = c.lure;
        if (ImGui::InputInt("Lure (RLRE 0-90)", &lure)) {
            RF3Tackle_SetLure(c.box, lure, "debug-window");
        }
        int target = c.target;
        if (ImGui::InputInt("Target (fish record 0-49)", &target)) {
            RF3Tackle_SetTarget(target, "debug-window");
        }
        ImGui::Text("Bass box: line %d, lure %d   Trout box: line %d, lure %d", RF3Tackle_GetBoxLine(0),
                    RF3Tackle_GetBoxLure(0), RF3Tackle_GetBoxLine(1), RF3Tackle_GetBoxLure(1));
        return;
    }
    const rf3tackle::Box& b = a.boxes[c.box];
    if (b.kind == "bait") { // step 19c: set on the tackle box screen
        const rf3tackle::Item* f = rf3tackle::FindFloat(b, c.floatIdx);
        const rf3tackle::Bait* bt = rf3tackle::FindBait(b, c.bait);
        ImGui::Text("Bait set: float %s, depth %d/4, bait %s, hook size %d/4 (%s)", f ? f->name.c_str() : "?", c.depth,
                    bt ? bt->name.c_str() : "--- (nothing bites)", c.size,
                    bt ? bt->hook.c_str() : b.hookDefault.c_str());
    }
    auto itemName = [](const rf3tackle::Item& i) { return std::to_string(i.index) + " " + i.name; };
    auto itemKey = [](const rf3tackle::Item& i) { return i.index; };
    auto targetName = [](const rf3tackle::Target& t) { return std::to_string(t.record) + " " + t.name; };
    auto targetKey = [](const rf3tackle::Target& t) { return t.record; };
    int32_t picked;
    if (PickFrom("Line", b.lines, c.line, itemName, itemKey, &picked)) {
        RF3Tackle_SetLine(c.box, picked, "debug-window");
    }
    if (PickFrom("Lure", b.lures, c.lure, itemName, itemKey, &picked)) {
        RF3Tackle_SetLure(c.box, picked, "debug-window");
    }
    if (PickFrom("Target", b.targets, c.target, targetName, targetKey, &picked)) {
        RF3Tackle_SetTarget(picked, "debug-window");
    }
    // The screen's numbers for this choice; with the defaults they must read like the user's RF3
    // screenshot: rod +1/+4, line -2/+5, lure +5/0, total +4/+9.
    const rf3tackle::Target* t = rf3tackle::FindTarget(b, c.target);
    const rf3tackle::Item* line = rf3tackle::FindLine(b, c.line);
    const rf3tackle::Item* lure = rf3tackle::FindLure(b, c.lure);
    ImGui::Text("%s: %s + %s, target %s", b.title.c_str(), b.rodName.c_str(), b.reelName.c_str(),
                t ? t->name.c_str() : "?");
    rf3tackle::HeartShield none;
    rf3tackle::HeartShield rows[3] = { t ? t->rod : none, none, none };
    if (line && line->values.count(c.target)) {
        rows[1] = line->values.at(c.target);
    }
    if (lure && lure->values.count(c.target)) {
        rows[2] = lure->values.at(c.target);
    }
    HeartShieldText("Rods & Reels", rows[0]);
    HeartShieldText("Reel Lines", rows[1]);
    HeartShieldText("Lures", rows[2]);
    if (rows[0] && rows[1] && rows[2]) {
        ImGui::Text("  %-13s heart %+d  shield %+d", "Total", (*rows[0])[0] + (*rows[1])[0] + (*rows[2])[0],
                    (*rows[0])[1] + (*rows[1])[1] + (*rows[2])[1]);
    }
}

// Step 63b (DESIGN "Step 63 design"): RF3's sounds through the mod's mixer, one button each (mods/rf3_sounds.o2r).
// fis sounds play on channel 10, sys sounds on 2, streams on 3, as RF3 uses them (looped ones play until stopped).
static void DrawSoundButtons(const char* label, const int* numbers, int count, int channel, bool stream,
                             const char* bank) {
    ImGui::TextUnformatted(label);
    for (int i = 0; i < count; i++) {
        if (i % 12 != 0) {
            ImGui::SameLine();
        }
        ImGui::PushID(channel * 1000 + numbers[i]);
        char text[8];
        std::snprintf(text, sizeof(text), "%d", numbers[i]);
        if (ImGui::Button(text, ImVec2(34, 0))) {
            if (stream) {
                RF3Sound_PlayStream(channel, numbers[i], 1.0f, 1.0f);
            } else {
                RF3Sound_Play(channel, bank, numbers[i], 1.0f, 1.0f);
            }
        }
        ImGui::PopID();
    }
}

static void DrawSounds() {
    if (!ImGui::CollapsingHeader("RF3 sounds (step 63b)")) {
        return;
    }
    ImGui::TextWrapped("Last: %s", RF3Sound_Status());
    int volume = CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.SoundVolumePct"), 100); // also in Settings > Audio
    if (ImGui::SliderInt("RF3 sound volume (%, 100 = RF3's own)", &volume, 0, 200)) {
        CVarSetInteger(CVAR_ENHANCEMENT("ReelFishing3.SoundVolumePct"), volume);
    }
    if (ImGui::Button("Stop all")) {
        RF3Sound_StopAll();
    }
    // Step 63c-5a: the mixer's own output for tools/rf3_audiocmp.py.
    bool recording = RF3Sound_IsRecording() != 0;
    if (ImGui::Checkbox("Record RF3 mix (logs/rf3_mix_<date>.wav + .csv)", &recording)) {
        RF3Sound_SetRecording(recording ? 1 : 0);
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(RF3Sound_RecordingStatus());
    static int fis[54], sys[15];
    for (int i = 0; i < 54; i++) {
        fis[i] = i;
    }
    for (int i = 0; i < 15; i++) {
        sys[i] = i;
    }
    static const int streams[] = { 5,  6,  7,  8,  9,  10, 13, 14, 15, 16, 26, 27, 31, 32, 35, 36,
                                   65, 73, 74, 79, 80, 81, 82, 85, 86, 87, 88, 89, 90, 91, 92 };
    DrawSoundButtons("fis (fishing), channel 10:", fis, 54, 10, false, "fis");
    DrawSoundButtons("sys (menus), channel 2:", sys, 15, 2, false, "sys");
    DrawSoundButtons("fis streams, channel 3:", streams, static_cast<int>(sizeof(streams) / sizeof(streams[0])), 3,
                     true, nullptr);
}

// Step 61b (DESIGN "Step 61 design", choice C7 A): the co-op fishing view's solo mirror. Your own view, a few frames
// late, drawn as a ghost beside you: the copy other players will see in 61c. Log: rf3_fishview.txt.
static void DrawFishView() {
    if (!ImGui::CollapsingHeader("Co-op fishing view (step 61)")) {
        return;
    }
    ImGui::TextWrapped("View: %s", RF3FishView_Status());
    bool mirror = CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.FishViewMirror"), 0) != 0;
    if (ImGui::Checkbox("Mirror my view as a ghost (solo test)", &mirror)) {
        CVarSetInteger(CVAR_ENHANCEMENT("ReelFishing3.FishViewMirror"), mirror ? 1 : 0);
    }
    float offset = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.FishViewMirrorOffset"), 150.0f);
    if (ImGui::SliderFloat("Ghost distance to the side (units, - = other side)", &offset, -400.0f, 400.0f, "%.0f")) {
        CVarSetFloat(CVAR_ENHANCEMENT("ReelFishing3.FishViewMirrorOffset"), offset);
    }
    int delay = CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.FishViewMirrorDelay"), 0);
    if (ImGui::SliderInt("Ghost delay (ms, imitates lag)", &delay, 0, 500)) {
        CVarSetInteger(CVAR_ENHANCEMENT("ReelFishing3.FishViewMirrorDelay"), delay);
    }
}

// Step 67: how an OoT-model pond fish (Hylian Bass, Loach) looks in STRUGGLE, tuned live. Log: rf3_models.txt
// "pond-struggle-sweep" at each side change.
static void DrawPondStruggle() {
    if (!ImGui::CollapsingHeader("Pond fish struggle (step 67)")) {
        return;
    }
    float sweep = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.PondStruggleSweep"), 30.0f);
    if (ImGui::SliderFloat("Sweep across the line (units; was 80)", &sweep, 0.0f, 120.0f, "%.0f")) {
        CVarSetFloat(CVAR_ENHANCEMENT("ReelFishing3.PondStruggleSweep"), sweep);
    }
    int frames = CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.PondStruggleFrames"), 15);
    if (ImGui::SliderInt("Game frames per side (20 = 1 s; was 10)", &frames, 1, 60)) {
        CVarSetInteger(CVAR_ENHANCEMENT("ReelFishing3.PondStruggleFrames"), frames);
    }
    float speed = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.PondStruggleSpeed"), 3.0f);
    if (ImGui::SliderFloat("Swim speed (was 6)", &speed, 0.5f, 10.0f, "%.1f")) {
        CVarSetFloat(CVAR_ENHANCEMENT("ReelFishing3.PondStruggleSpeed"), speed);
    }
    float beat = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.PondStruggleBeat"), 1.2f);
    if (ImGui::SliderFloat("Tail beat speed (was 1.6)", &beat, 0.2f, 3.0f, "%.2f")) {
        CVarSetFloat(CVAR_ENHANCEMENT("ReelFishing3.PondStruggleBeat"), beat);
    }
    float amp = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.PondStruggleAmp"), 3500.0f);
    if (ImGui::SliderFloat("Tail beat size (was 6000)", &amp, 500.0f, 8000.0f, "%.0f")) {
        CVarSetFloat(CVAR_ENHANCEMENT("ReelFishing3.PondStruggleAmp"), amp);
    }
    int turn = CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.PondStruggleTurn"), 2);
    if (ImGui::SliderInt("Turn speed x (was 3)", &turn, 1, 5)) {
        CVarSetInteger(CVAR_ENHANCEMENT("ReelFishing3.PondStruggleTurn"), turn);
    }
    if (ImGui::Button("Reset to the step 67 defaults")) {
        CVarClear(CVAR_ENHANCEMENT("ReelFishing3.PondStruggleSweep"));
        CVarClear(CVAR_ENHANCEMENT("ReelFishing3.PondStruggleFrames"));
        CVarClear(CVAR_ENHANCEMENT("ReelFishing3.PondStruggleSpeed"));
        CVarClear(CVAR_ENHANCEMENT("ReelFishing3.PondStruggleBeat"));
        CVarClear(CVAR_ENHANCEMENT("ReelFishing3.PondStruggleAmp"));
        CVarClear(CVAR_ENHANCEMENT("ReelFishing3.PondStruggleTurn"));
    }
}

// Step 70e: the big (rank 4+) fish's hover turn speed, saved as a CVar.
static void DrawBigFishTurn() {
    if (!ImGui::CollapsingHeader("Big fish turn (step 70e)")) {
        return;
    }
    float turn = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.BigFishTurn"), 20.0f);
    if (ImGui::SliderFloat("Top turn speed (deg/s; RF3 180 or 30)", &turn, 5.0f, 90.0f, "%.0f")) {
        CVarSetFloat(CVAR_ENHANCEMENT("ReelFishing3.BigFishTurn"), turn);
    }
    if (ImGui::Button("Reset to the step 70e default (20)")) {
        CVarClear(CVAR_ENHANCEMENT("ReelFishing3.BigFishTurn"));
    }
}

// Step 20c-3d: the fly cast's false-casting animation (frames of Link's throw), saved as CVars.
static void DrawFlyCast() {
    if (!ImGui::CollapsingHeader("Fly cast animation (step 20c-3d)")) {
        return;
    }
    ImGui::TextDisabled("While B is held with a fly box, Link swings the throw between Back and Forward;");
    ImGui::TextDisabled("on release the fly is cast when the throw reaches Release. Lengths: see rf3_records.txt.");
    const char* labels[4] = { "Back frame", "Forward frame", "Swing speed", "Release frame" };
    const float steps[4] = { 0.5f, 0.5f, 0.05f, 0.5f };
    for (int i = 0; i < 4; i++) {
        float v = RF3FlyCast_Tuning(i);
        if (ImGui::DragFloat(labels[i], &v, steps[i], 0.0f, 60.0f, "%.2f")) {
            RF3FlyCast_SetTuning(i, v);
        }
    }
    ImGui::Text("Charging now: %s", RF3FlyCast_IsCharging() ? "yes" : "no");
}

// Steps 25c-2, 25d-2: RF3's lure, fly, bait and float models (mods/rf3_models.o2r). Every change is logged to
// rf3_models.txt.
static void DrawLureModel() {
    if (!ImGui::CollapsingHeader("RF3 tackle models (steps 25c-2, 25d-2)")) {
        return;
    }
    bool on = CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.LureModels"), 1) != 0;
    if (ImGui::Checkbox("Draw RF3's models on the line", &on)) {
        CVarSetInteger(CVAR_ENHANCEMENT("ReelFishing3.LureModels"), on ? 1 : 0);
    }
    // Size per kind, OoT units per RF3 metre; defaults as RF3LureModel.cpp (DESIGN 25c/25d, assumed).
    struct SizeRow {
        const char* label;
        const char* cvar;
        float def;
        float max;
    };
    static const SizeRow kRows[] = {
        { "Lure size (units/m; 270 = OoT's lure)", CVAR_ENHANCEMENT("ReelFishing3.LureModelScale"), 70.0f, 1000.0f },
        { "Fly size (units/m)", CVAR_ENHANCEMENT("ReelFishing3.FlyModelScale"), 600.0f, 2000.0f },
        { "Bait size (units/m; also the bare hook)", CVAR_ENHANCEMENT("ReelFishing3.BaitModelScale"), 200.0f, 2000.0f },
        { "Float size (units/m; 68 = the game's metre)", CVAR_ENHANCEMENT("ReelFishing3.FloatModelScale"), 68.0f,
          500.0f },
    };
    for (const SizeRow& r : kRows) {
        float v = CVarGetFloat(r.cvar, r.def);
        if (ImGui::SliderFloat(r.label, &v, 20.0f, r.max, "%.0f")) {
            CVarSetFloat(r.cvar, v);
        }
    }
    // Step 39a-2..39c: the tackle screen's rod pane: fine-tuning of the lure/fly's tilt and turn (0: RF3's camera
    // gives the view; the 39a roll slider is gone, RF3's rod node sets the roll).
    float paneTilt = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.RodPaneItemTilt"), 0.0f);
    if (ImGui::SliderFloat("Rod pane item tilt (degrees; + shows its top)", &paneTilt, -60.0f, 60.0f, "%.0f")) {
        CVarSetFloat(CVAR_ENHANCEMENT("ReelFishing3.RodPaneItemTilt"), paneTilt);
    }
    // 39a-3/4: the item's turn about the pane's vertical (+ = counterclockwise from above) [assumed 5] and the
    // pane's light (diffuse; 200 = the user's value).
    float paneTurn = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.RodPaneItemTurn"), 0.0f);
    if (ImGui::SliderFloat("Rod pane item turn (degrees, about the vertical)", &paneTurn, -30.0f, 30.0f, "%.0f")) {
        CVarSetFloat(CVAR_ENHANCEMENT("ReelFishing3.RodPaneItemTurn"), paneTurn);
    }
    float paneLight = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.RodPaneLight"), 200.0f);
    if (ImGui::SliderFloat("Rod pane light (0-255)", &paneLight, 0.0f, 255.0f, "%.0f")) {
        CVarSetFloat(CVAR_ENHANCEMENT("ReelFishing3.RodPaneLight"), paneLight);
    }
    // Step 29e: the float's idle bob on the surface (the mod's own design; RF3 has no float in the water).
    float bobM = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.FloatBobM"), 0.01f);
    if (ImGui::SliderFloat("Float bob size (m, up and down)", &bobM, 0.0f, 0.05f, "%.3f")) {
        CVarSetFloat(CVAR_ENHANCEMENT("ReelFishing3.FloatBobM"), bobM);
    }
    float bobPeriod = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.FloatBobPeriod"), 2.5f);
    if (ImGui::SliderFloat("Float bob period (s)", &bobPeriod, 0.5f, 6.0f, "%.2f")) {
        CVarSetFloat(CVAR_ENHANCEMENT("ReelFishing3.FloatBobPeriod"), bobPeriod);
    }
    // Step 29f: the nibble dip, once per WAIT of the fish at the bait.
    float dipM = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.FloatDipM"), 0.03f);
    if (ImGui::SliderFloat("Float nibble dip (m)", &dipM, 0.0f, 0.15f, "%.3f")) {
        CVarSetFloat(CVAR_ENHANCEMENT("ReelFishing3.FloatDipM"), dipM);
    }
    float dipDown = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.FloatDipDown"), 0.1f);
    if (ImGui::SliderFloat("Float nibble dip: down time (s)", &dipDown, 0.05f, 1.0f, "%.2f")) {
        CVarSetFloat(CVAR_ENHANCEMENT("ReelFishing3.FloatDipDown"), dipDown);
    }
    float dipUp = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.FloatDipUp"), 0.4f);
    if (ImGui::SliderFloat("Float nibble dip: back up time (s)", &dipUp, 0.05f, 2.0f, "%.2f")) {
        CVarSetFloat(CVAR_ENHANCEMENT("ReelFishing3.FloatDipUp"), dipUp);
    }
    // Step 29g: going under at the bite (29g-2: then on the line, the float depth from the fish).
    float underTime = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.FloatUnderTime"), 0.2f);
    if (ImGui::SliderFloat("Float going under time (s)", &underTime, 0.05f, 2.0f, "%.2f")) {
        CVarSetFloat(CVAR_ENHANCEMENT("ReelFishing3.FloatUnderTime"), underTime);
    }
    // Step 31b: back to the surface (or the line) when the fight ends.
    float returnTime = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.FloatReturnTime"), 0.3f);
    if (ImGui::SliderFloat("Float return time (s)", &returnTime, 0.05f, 2.0f, "%.2f")) {
        CVarSetFloat(CVAR_ENHANCEMENT("ReelFishing3.FloatReturnTime"), returnTime);
    }
    bool flip = CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.LureModelFlip"), 0) != 0;
    if (ImGui::Checkbox("Turn lures and flies around (front <-> back)", &flip)) {
        CVarSetInteger(CVAR_ENHANCEMENT("ReelFishing3.LureModelFlip"), flip ? 1 : 0);
    }
    // Preview: show any model in place of the chosen one (only the look changes).
    static const char* kKinds[] = { "Off (the chosen tackle)", "Lure", "Fly", "Bait", "Float" };
    static const int kStockKind[] = { RF3_STOCK_LURE, RF3_STOCK_FLY, RF3_STOCK_BAIT, RF3_STOCK_FLOAT };
    static const int kCounts[] = { 91, 40, 12, 6 };
    int kind = CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.ModelPreviewKind"), -1);
    int combo = kind + 1;
    if (ImGui::Combo("Show model", &combo, kKinds, IM_ARRAYSIZE(kKinds))) {
        kind = combo - 1;
        CVarSetInteger(CVAR_ENHANCEMENT("ReelFishing3.ModelPreviewKind"), kind);
    }
    if (kind >= 0 && kind < 4) {
        int index = CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.ModelPreviewIndex"), 0);
        if (ImGui::InputInt("Model #", &index)) {
            index = std::max(0, std::min(index, kCounts[kind] - 1));
            CVarSetInteger(CVAR_ENHANCEMENT("ReelFishing3.ModelPreviewIndex"), index);
        }
        ImGui::Text("  %s (only the look changes; the fishing uses the chosen tackle)",
                    RF3Stock_Name(kStockKind[kind], index));
    }
    ImGui::TextWrapped("Now: %s", RF3LureModel_Status());
    ImGui::TextWrapped("Float: %s", RF3LureModel_FloatStatus());
}

// Step 25e-3: RF3's fish models for region fish (mods/rf3_models.o2r). Changes are logged to rf3_models.txt.
static void DrawFishModel() {
    if (!ImGui::CollapsingHeader("RF3 fish models (step 25e)")) {
        return;
    }
    bool on = CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.FishModels"), 1) != 0;
    if (ImGui::Checkbox("Draw region fish with RF3's models", &on)) {
        CVarSetInteger(CVAR_ENHANCEMENT("ReelFishing3.FishModels"), on ? 1 : 0);
    }
    float scale = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.FishModelScale"), RF3_UNITS_PER_METRE);
    if (ImGui::SliderFloat("Fish size (units/m; 68 = the game's metre)", &scale, 20.0f, 400.0f, "%.0f")) {
        CVarSetFloat(CVAR_ENHANCEMENT("ReelFishing3.FishModelScale"), scale);
    }
    float bend = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.FishModelBend"), 1.0f);
    if (ImGui::SliderFloat("Swim bend (x OoT's; negative = other way)", &bend, -3.0f, 3.0f, "%.2f")) {
        CVarSetFloat(CVAR_ENHANCEMENT("ReelFishing3.FishModelBend"), bend);
    }
    bool bind = CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.FishModelBindPose"), 0) != 0;
    if (ImGui::Checkbox("Freeze in the bind pose (no bending)", &bind)) {
        CVarSetInteger(CVAR_ENHANCEMENT("ReelFishing3.FishModelBindPose"), bind ? 1 : 0);
    }
    int preview = CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.FishModelPreview"), -1);
    if (ImGui::InputInt("Show fish record # on every region fish (-1 = its own)", &preview)) {
        preview = std::max(-1, std::min(preview, 40));
        CVarSetInteger(CVAR_ENHANCEMENT("ReelFishing3.FishModelPreview"), preview);
    }
    ImGui::TextDisabled("Only the look changes; records 0-40 have RF3 models (32 shows 33's).");
    // Step 25g-3: RF3's own motions.
    bool motions = CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.FishMotions"), 1) != 0;
    if (ImGui::Checkbox("Play RF3's own motions (off = OoT's bend, step 25e)", &motions)) {
        CVarSetInteger(CVAR_ENHANCEMENT("ReelFishing3.FishMotions"), motions ? 1 : 0);
    }
    int force = CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.FishMotionForce"), -1);
    if (ImGui::InputInt("Force RF3 request code (-1 = chosen by state)", &force)) {
        force = std::max(-1, std::min(force, 66));
        CVarSetInteger(CVAR_ENHANCEMENT("ReelFishing3.FishMotionForce"), force);
    }
    // Step 26e-2: RF3's request codes (RF3-MECHANICS 6.2); other numbers play that raw animation.
    ImGui::TextDisabled("Patterns: 60 towed, 2/3/4 swim S/M/L, 15/16 thrash/big, 28 Kuh!, 61/62 keep box, 63-66 turns");
    float rate = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.FishMotionRate"), 1.0f);
    if (ImGui::SliderFloat("Motion speed (x)", &rate, 0.1f, 3.0f, "%.2f")) {
        CVarSetFloat(CVAR_ENHANCEMENT("ReelFishing3.FishMotionRate"), rate);
    }
    float slow = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.FishMotionSlow"), 1.0f);
    if (ImGui::SliderFloat("Swim S below speed", &slow, 0.0f, 10.0f, "%.2f")) {
        CVarSetFloat(CVAR_ENHANCEMENT("ReelFishing3.FishMotionSlow"), slow);
    }
    float fast = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.FishMotionFast"), 3.0f);
    if (ImGui::SliderFloat("Swim L from speed", &fast, 0.0f, 20.0f, "%.2f")) {
        CVarSetFloat(CVAR_ENHANCEMENT("ReelFishing3.FishMotionFast"), fast);
    }
    // Step 26e-4: the hooked fish's RF3 movement speed (RF3 = 1.0; user found it too fast).
    float moveSpeed = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.FishMoveSpeed"), 0.2f); // user, 2026-10-14
    if (ImGui::SliderFloat("Hooked fish speed (x RF3's)", &moveSpeed, 0.1f, 1.5f, "%.2f")) {
        CVarSetFloat(CVAR_ENHANCEMENT("ReelFishing3.FishMoveSpeed"), moveSpeed);
    }
    // Step 30b-1b: the fight target's leg is as long as RF3's at this distance (RF3's fights are 10-30 m).
    float arcRef = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.FishArcRefM"), 15.0f);
    if (ImGui::SliderFloat("Fight leg length as RF3's at (m)", &arcRef, 1.0f, 40.0f, "%.1f")) {
        CVarSetFloat(CVAR_ENHANCEMENT("ReelFishing3.FishArcRefM"), arcRef);
    }
    int turnRate = CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.FishMotionTurnRate"), 0x300);
    if (ImGui::SliderInt("Turn motion above yaw/frame", &turnRate, 0, 0x2000)) {
        CVarSetInteger(CVAR_ENHANCEMENT("ReelFishing3.FishMotionTurnRate"), turnRate);
    }
    ImGui::TextWrapped("Now: %s", RF3FishModel_Status());
}

// Step 25f-3: RF3's rod models in Link's hands (mods/rf3_models.o2r). Changes are logged to rf3_models.txt.
static void DrawRodModel() {
    if (!ImGui::CollapsingHeader("RF3 rod models (step 25f)")) {
        return;
    }
    bool on = CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.RodModels"), 1) != 0;
    if (ImGui::Checkbox("Draw RF3's rod in Link's hands", &on)) {
        CVarSetInteger(CVAR_ENHANCEMENT("ReelFishing3.RodModels"), on ? 1 : 0);
    }
    float scale = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.RodModelScale"), RF3_UNITS_PER_METRE);
    if (ImGui::SliderFloat("Rod size (units/m; 68 = the game's metre)", &scale, 20.0f, 200.0f, "%.0f")) {
        CVarSetFloat(CVAR_ENHANCEMENT("ReelFishing3.RodModelScale"), scale);
    }
    float bend = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.RodModelBend"), 1.0f);
    if (ImGui::SliderFloat("Rod bend (x OoT's; negative = other way)", &bend, -3.0f, 3.0f, "%.2f")) {
        CVarSetFloat(CVAR_ENHANCEMENT("ReelFishing3.RodModelBend"), bend);
    }
    float grip = CVarGetFloat(CVAR_ENHANCEMENT("ReelFishing3.RodModelGrip"), 0.0f);
    if (ImGui::SliderFloat("Grip (m along the rod; + = hand further up)", &grip, -0.5f, 0.5f, "%.3f")) {
        CVarSetFloat(CVAR_ENHANCEMENT("ReelFishing3.RodModelGrip"), grip);
    }
    int preview = CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.RodModelPreview"), -1);
    if (ImGui::InputInt("Show rod # (-1 = the chosen box's)", &preview)) {
        preview = std::max(-1, std::min(preview, 9));
        CVarSetInteger(CVAR_ENHANCEMENT("ReelFishing3.RodModelPreview"), preview);
    }
    if (preview >= 0) {
        ImGui::Text("  %s (only the look changes)", RF3Stock_Name(RF3_STOCK_ROD, preview));
    }
    ImGui::TextWrapped("Now: %s", RF3RodModel_Status());
}

void RF3DebugWindow::DrawElement() {
    RF3FightStatus s;
    RF3Fight_GetStatus(&s);

    ImGui::Text("Reel Fishing III mechanics: %s", RF3_IsEnabled() ? "ON" : "OFF");
    ImGui::Text("Fishing rod owned (this save file): %s", RF3Rod_IsOwned() ? "yes" : "no"); // step 15a
    ImGui::Text("RF3 vibration: %s", RF3Vib_Status()); // step 62c
    DrawStock();
    DrawTackle();
    DrawFlyCast();
    DrawPondStruggle(); // step 67
    DrawBigFishTurn();  // step 70e
    DrawFishView(); // step 61b
    DrawSounds();   // step 63b
    DrawLureModel();
    DrawFishModel();
    DrawRodModel();
    if (!s.active) {
        ImGui::Separator();
        DrawApproach();
        ImGui::Separator();
        ImGui::Text("No strike or fight running. Last fight outcome: %s", OutcomeName(s.outcome));
        return;
    }

    ImGui::Separator();
    ImGui::Text("Fish: %s, %.0f mm, %d g (%.2f lb)", s.fishName ? s.fishName : "?", s.lengthMm, s.weightG,
                s.weightG / 453.592f);
    ImGui::Text("State: %s   (last run: %s)", ModeName(s.mode), ModeName(s.lastRun));
    ImGui::Text("  %s", ModeHint(s.mode));
    if (s.mode == 2) {
        const char* when = s.stateTime < s.hookWindowStart
                               ? "TOO EARLY - wait"
                               : (s.stateTime <= s.hookWindowEnd ? "NOW: jerk rod (stick down) or press A" : "too late");
        ImGui::Text("Strike! %.2f s since the bite: %s", s.stateTime, when);
        ImGui::Text("Hook-set window %.2f - %.2f s, fish lets go at %.2f s", s.hookWindowStart, s.hookWindowEnd,
                    s.hookGiveUpTime);
        return; // the fight values below are not used until the hook is set
    }
    // Testing aid (user request, 2026-10-07): land the hooked fish now, through the normal catch path.
    if (ImGui::Button("Catch the hooked fish now (debug)")) {
        RF3Fight_ForceCatch();
    }
    ImGui::Text("Hook set: timing %.2f, strength %.2f  (start stamina = 100 - 25 x timing)", s.hookSetTiming,
                s.hookSetStrength);
    ImGui::Text("Time in state: %.2f / %.2f s", s.stateTime, s.stateDuration);
    if (s.mode == 7) {
        bool open = s.stateTime <= s.stopWindowEnd;
        ImGui::Text("STOP: expects %s, window %s (closes at %.2f s)", s.stopExpectsReel ? "REEL" : "NO REEL",
                    open ? "OPEN" : "closed", s.stopWindowEnd);
    }

    ImGui::Separator();
    Bar("Tension", s.tension, s.tensionLimit);
    Bar("Hook", s.hook, 100.0f);
    Bar("Stamina", s.hp, 100.0f);
    ImGui::Text("(line breaks at full tension, hook comes out at 0, stamina never drops below 30)");

    ImGui::Separator();
    ImGui::Text("Distance: target %.2f m, fish %.2f m (gap %+.2f m)", s.distanceM, s.actualDistanceM,
                s.actualDistanceM - s.distanceM);
    ImGui::Text("Caught at: %.2f m or closer", s.landingDistanceM);
    ImGui::Text("Fish depth: %.0f units below the surface (water here is %.0f deep)", s.fishDepthUnits,
                s.waterDepthUnits);

    ImGui::Separator();
    const char* rod = s.stickY >= 0.5f ? "RAISED (back)" : (s.stickY <= -0.5f ? "LOWERED (forward)" : "middle");
    ImGui::Text("Input: reel %s, rod %s  (stick X %+.2f, Y %+.2f)", s.reelHeld ? "HELD" : "off", rod, s.stickX,
                s.stickY);
}
