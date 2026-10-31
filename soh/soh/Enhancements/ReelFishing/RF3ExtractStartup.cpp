// Reel Fishing III mod (step 77c): see RF3ExtractStartup.h.

#include "RF3ExtractStartup.h"

#include <mutex>
#include <thread>
#include <vector>

#include <imgui.h>
#include <libultraship/bridge/consolevariablebridge.h>

#include "soh/Extractor/portable-file-dialogs.h"
#include "RF3Extract.h"

namespace rf3extract {

namespace {

std::mutex sPickMutex;
bool sPicking = false;
bool sPicked = false;
std::string sPickedPath;

enum class Stage { Check, Prompt, Building, Failed, Done };
Stage sStage = Stage::Check;
std::vector<std::string> sNeeded;

const char* const kPopup = "Reel Fishing III setup";

void SetMusic(bool on) {
    CVarSetInteger(CVAR_RF3_BUILD_MUSIC, on ? 1 : 0);
    CVarSave();
    sNeeded = ArchivesNeeded(on);
}

// Opens the modal (once) and starts it; the caller ends it with EndPopup when this returns true.
bool BeginModal() {
    if (!ImGui::IsPopupOpen(kPopup)) {
        ImGui::OpenPopup(kPopup);
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    return ImGui::BeginPopupModal(kPopup, nullptr,
                                  ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                                      ImGuiWindowFlags_NoSavedSettings);
}

void DrawPrompt() {
    ApplyPickedDisc();
    const std::string disc = CVarGetString(CVAR_RF3_DISC_PATH, "");
    bool music = CVarGetInteger(CVAR_RF3_BUILD_MUSIC, 1) != 0;
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
    ImGui::TextWrapped("The Reel Fishing III mod builds its pictures, sounds, models and music from your own copy of "
                       "Reel Fishing III (USA), SLUS-20555, as a .bin (MODE2/2352) or .iso disc image.");
    ImGui::Spacing();
    if (sNeeded.empty()) {
        ImGui::TextWrapped("Nothing is missing.");
    } else {
        ImGui::TextUnformatted("Needed:");
        for (const std::string& n : sNeeded) {
            ImGui::BulletText("%s", n.c_str());
        }
    }
    ImGui::Spacing();
    ImGui::TextWrapped("Disc image: %s", disc.empty() ? "(none chosen)" : disc.c_str());
    ImGui::BeginDisabled(PickingDisc());
    if (ImGui::Button("Choose disc image...")) {
        PickDisc();
    }
    ImGui::EndDisabled();
    ImGui::Spacing();
    if (ImGui::Checkbox("Build the music too", &music)) {
        SetMusic(music);
    }
    ImGui::PopTextWrapPos();
    ImGui::Separator();
    if (sNeeded.empty()) {
        if (ImGui::Button("Continue")) {
            sStage = Stage::Done;
        }
        return;
    }
    ImGui::BeginDisabled(disc.empty() || PickingDisc());
    if (ImGui::Button("Build")) {
        BuildOptions o;
        o.music = music;
        o.atStartup = true;
        if (Start(disc, Job::Build, o)) {
            sStage = Stage::Building;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Not now")) {
        sStage = Stage::Done;
    }
    ImGui::SameLine();
    if (ImGui::Button("Don't ask again")) {
        CVarSetInteger(CVAR_RF3_SETUP_NO_PROMPT, 1);
        CVarSave();
        sStage = Stage::Done;
    }
    ImGui::TextDisabled("The Disc Setup window (Enhancements menu) can build them later.");
}

void DrawProgress(const Status& st) {
    ImGui::TextUnformatted("Building the Reel Fishing III files...");
    ImGui::ProgressBar(st.progress, ImVec2(ImGui::GetFontSize() * 32.0f, 0.0f), st.step.c_str());
    if (!st.messages.empty()) {
        ImGui::TextDisabled("%s", st.messages.back().c_str());
    }
}

void DrawFailed(const Status& st) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "The build failed. Nothing the game uses was changed.");
    ImGui::BeginChild("rf3setup-messages", ImVec2(ImGui::GetFontSize() * 32.0f, ImGui::GetFontSize() * 10.0f), true);
    for (const std::string& m : st.messages) {
        ImGui::TextWrapped("%s", m.c_str());
    }
    ImGui::EndChild();
    ImGui::PopTextWrapPos();
    if (ImGui::Button("Try again")) {
        sNeeded = ArchivesNeeded(CVarGetInteger(CVAR_RF3_BUILD_MUSIC, 1) != 0);
        sStage = Stage::Prompt;
    }
    ImGui::SameLine();
    if (ImGui::Button("Continue without")) {
        sStage = Stage::Done;
    }
}

} // namespace

void PickDisc() {
    {
        std::lock_guard<std::mutex> lock(sPickMutex);
        if (sPicking) {
            return;
        }
        sPicking = true;
    }
    std::thread([] {
        std::vector<std::string> chosen =
            pfd::open_file("Select your Reel Fishing III disc image", "",
                           { "PS2 disc images", "*.bin *.iso *.BIN *.ISO", "All files", "*" })
                .result();
        std::lock_guard<std::mutex> lock(sPickMutex);
        sPicking = false;
        if (!chosen.empty()) {
            sPickedPath = chosen[0];
            sPicked = true;
        }
    }).detach();
}

bool PickingDisc() {
    std::lock_guard<std::mutex> lock(sPickMutex);
    return sPicking;
}

void ApplyPickedDisc() {
    std::lock_guard<std::mutex> lock(sPickMutex);
    if (sPicked) {
        sPicked = false;
        CVarSetString(CVAR_RF3_DISC_PATH, sPickedPath.c_str());
        CVarSave();
    }
}

bool StartupFinished() {
    if (sStage == Stage::Check) {
        if (CVarGetInteger(CVAR_RF3_SETUP_NO_PROMPT, 0)) {
            sStage = Stage::Done;
        } else {
            sNeeded = ArchivesNeeded(CVarGetInteger(CVAR_RF3_BUILD_MUSIC, 1) != 0);
            sStage = sNeeded.empty() ? Stage::Done : Stage::Prompt;
        }
    }
    if (sStage == Stage::Building) {
        const Status st = Get();
        if (st.done) {
            sStage = st.ok ? Stage::Done : Stage::Failed;
        }
    }
    return sStage == Stage::Done;
}

void StartupDraw() {
    if (sStage == Stage::Check || sStage == Stage::Done) {
        return;
    }
    if (!BeginModal()) {
        return;
    }
    if (sStage == Stage::Prompt) {
        DrawPrompt();
    } else if (sStage == Stage::Building) {
        DrawProgress(Get());
    } else {
        DrawFailed(Get());
    }
    if (sStage == Stage::Done) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace rf3extract
