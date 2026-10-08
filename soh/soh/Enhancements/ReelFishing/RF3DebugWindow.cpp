#include "RF3DebugWindow.h"

#include <cstdio>
#include <string>
#include <vector>

#include "RF3Bridge.h"
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

void RF3DebugWindow::DrawElement() {
    RF3FightStatus s;
    RF3Fight_GetStatus(&s);

    ImGui::Text("Reel Fishing III mechanics: %s", RF3_IsEnabled() ? "ON" : "OFF");
    ImGui::Text("Fishing rod owned (this save file): %s", RF3Rod_IsOwned() ? "yes" : "no"); // step 15a
    DrawTackle();
    DrawFlyCast();
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
