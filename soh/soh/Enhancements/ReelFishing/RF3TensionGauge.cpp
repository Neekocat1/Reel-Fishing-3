#include "RF3TensionGauge.h"

#include <cstdio>

#include <libultraship/bridge/consolevariablebridge.h>

#include "soh/cvar_prefixes.h"
#include "RF3Bridge.h"

void RF3TensionGauge::Draw() {
    if (!CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.TensionGauge"), 0) || !RF3_IsEnabled() ||
        !RF3Fight_IsActive()) {
        return;
    }
    RF3FightStatus s;
    RF3Fight_GetStatus(&s);
    if (!s.hooked) {
        return; // still in the strike (hook-set) phase
    }
    float load = (s.tensionLimit > 0.0f) ? s.tension / s.tensionLimit : 0.0f;

    // Green below 60%, yellow to 85%, red above.
    ImVec4 colour = load < 0.6f ? ImVec4(0.2f, 0.8f, 0.2f, 1.0f)
                                : (load < 0.85f ? ImVec4(0.9f, 0.8f, 0.1f, 1.0f) : ImVec4(0.9f, 0.15f, 0.1f, 1.0f));

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, colour);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    ImGui::SetNextWindowSize(ImVec2(260, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("RF3TensionGauge", nullptr,
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollWithMouse |
                     ImGuiWindowFlags_NoScrollbar);
    char text[32];
    std::snprintf(text, sizeof(text), "Tension %d%%", static_cast<int>(load * 100.0f + 0.5f));
    ImGui::ProgressBar(load, ImVec2(240, 0), text);
    ImGui::Text("Hook %.0f", s.hook);
    ImGui::End();
    ImGui::PopStyleVar(1);
    ImGui::PopStyleColor(3);
}
