// Reel Fishing III mod (steps 76b, 77b): the extractor's window. See RF3ExtractWindow.h and docs/DESIGN.md
// "Step 76 design".

#include "RF3ExtractWindow.h"

#include <string>

#include <imgui.h>
#include <libultraship/bridge/consolevariablebridge.h>

#include "RF3Extract.h"
#include "RF3ExtractStartup.h"

void RF3ExtractWindow::DrawElement() {
    rf3extract::ApplyPickedDisc();
    const std::string disc = CVarGetString(CVAR_RF3_DISC_PATH, "");
    const rf3extract::Status st = rf3extract::Get();

    ImGui::TextWrapped("Builds the mod's Reel Fishing III files from your own copy of the game: Reel Fishing III "
                       "(USA), SLUS-20555, as a .bin (MODE2/2352) or .iso disc image.");
    ImGui::Separator();
    ImGui::TextWrapped("Disc image: %s", disc.empty() ? "(none chosen)" : disc.c_str());
    ImGui::BeginDisabled(st.running);
    ImGui::BeginDisabled(rf3extract::PickingDisc());
    if (ImGui::Button("Choose disc image...")) {
        rf3extract::PickDisc();
    }
    ImGui::EndDisabled();
    bool music = CVarGetInteger(CVAR_RF3_BUILD_MUSIC, 1) != 0;
    if (ImGui::Checkbox("Build the music too", &music)) {
        CVarSetInteger(CVAR_RF3_BUILD_MUSIC, music ? 1 : 0);
        CVarSave();
    }
    bool ask = CVarGetInteger(CVAR_RF3_SETUP_NO_PROMPT, 0) == 0;
    if (ImGui::Checkbox("Ask at startup when these files are missing or out of date", &ask)) {
        CVarSetInteger(CVAR_RF3_SETUP_NO_PROMPT, ask ? 0 : 1);
        CVarSave();
    }
    ImGui::BeginDisabled(disc.empty());
    if (ImGui::Button("Build archives")) {
        rf3extract::BuildOptions o;
        o.music = music;
        rf3extract::Start(disc, rf3extract::Job::Build, o);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Builds rf3_tackle.o2r, rf3_sounds.o2r, rf3_models.o2r and (if checked) rf3_music.o2r in "
                          "the mods folder from the disc. Restart the game afterwards.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Check disc")) {
        rf3extract::Start(disc, rf3extract::Job::Check);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Only checks the disc and reads every file in it (a listing in logs/), without building.");
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    if (st.running || st.done) {
        ImGui::Separator();
        ImGui::ProgressBar(st.progress, ImVec2(-1.0f, 0.0f), st.step.c_str());
        if (st.done) {
            ImGui::TextColored(st.ok ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) : ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s",
                               st.ok ? "Finished." : "Failed. See the messages below.");
        }
        ImGui::BeginChild("rf3extract-messages", ImVec2(0, 0), true);
        for (const std::string& m : st.messages) {
            ImGui::TextWrapped("%s", m.c_str());
        }
        ImGui::EndChild();
    }
}
