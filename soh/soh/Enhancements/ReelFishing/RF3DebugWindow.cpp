#include "RF3DebugWindow.h"

#include <cstdio>

#include "RF3Bridge.h"

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
    ImGui::Text("Sight range %.1f m (activity %d)   wary fish: %d", a.sightRangeM, a.activityLevel, a.waryFish);
    float power, factor;
    RF3Cast_GetLast(&power, &factor);
    ImGui::Text("Last cast: power %.2f -> %.0f%% of a full cast (hold B through the swing for full)", power,
                factor * 100.0f);
}

void RF3DebugWindow::DrawElement() {
    RF3FightStatus s;
    RF3Fight_GetStatus(&s);

    ImGui::Text("Reel Fishing III mechanics: %s", RF3_IsEnabled() ? "ON" : "OFF");
    ImGui::Text("Fishing rod owned (this save file): %s", RF3Rod_IsOwned() ? "yes" : "no"); // step 15a
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
