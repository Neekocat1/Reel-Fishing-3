#include "ArchipelagoConsoleWindow.h"

#include "soh/SohGui/UIWidgets.hpp"
#include "soh/SohGui/SohGui.hpp"
#include "soh/OTRGlobals.h"

ImVector<char*> Items;
bool autoScroll = true;

using namespace UIWidgets;

void ArchipelagoConsole_SendMessage(const char* fmt, bool debugMessage, ...) {
    if (!debugMessage || CVarGetInteger(CVAR_REMOTE_ARCHIPELAGO("DebugEnabled"), 0)) {
        char buf[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, IM_ARRAYSIZE(buf), fmt, args);
        buf[IM_ARRAYSIZE(buf) - 1] = 0;
        va_end(args);
        Items.push_back(strdup(buf));
    }
}

void ArchipelagoConsoleWindow::DrawElement() {
    ImGui::SeparatorText("Archipelago Log");

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(15.0f, 12.0f));

    if (ImGui::BeginChild("ScrollingRegion", ImVec2(0, 400), ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_HorizontalScrollbar)) {

        for (int i = 0; i < Items.Size; i++) {
            const char* item = Items[i];
            ImVec4 color;
            bool hasColor = false;
            if (strstr(item, "[ERROR]")) {
                color = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
                hasColor = true;
            } else if (strstr(item, "[LOG]")) {
                color = ImVec4(0.7f, 0.7f, 1.0f, 1.0f);
                hasColor = true;
            }
            if (hasColor) {
                ImGui::PushStyleColor(ImGuiCol_Text, color);
            }

            ImGui::TextUnformatted(item);

            if (hasColor) {
                ImGui::PopStyleColor();
            }
        }

        // Keep up at the bottom of the scroll region if we were already at the bottom at the beginning of the frame.
        // Using a scrollbar or mouse-wheel will take away from the bottom edge.
        if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
            ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
};
