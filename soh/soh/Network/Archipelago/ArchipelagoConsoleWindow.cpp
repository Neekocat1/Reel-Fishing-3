#include "ArchipelagoConsoleWindow.h"

#include "soh/SohGui/UIWidgets.hpp"
#include "soh/SohGui/SohGui.hpp"

ImVector<char*> Items;
bool autoScroll = true;
bool scrollToBottom = false;

void ArchipelagoConsole_SendMessage(const char* fmt, ...) IM_FMTARGS(2) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, IM_ARRAYSIZE(buf), fmt, args);
    buf[IM_ARRAYSIZE(buf) - 1] = 0;
    va_end(args);
    Items.push_back(strdup(buf));
}

void ArchipelagoConsoleWindow::DrawElement() {
    if (ImGui::Button("Add line to log")) {
        ArchipelagoConsole_SendMessage("[LOG] Hello World");
        ArchipelagoConsole_SendMessage("[ERROR] Hello World");
        ArchipelagoConsole_SendMessage("Hello World");
    }

    if (ImGui::BeginChild("ScrollingRegion", ImVec2(0, 400), false,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 1)); // Tighten spacing

        for (int i = 0; i < Items.Size; i++) {
            const char* item = Items[i];

            // Normally you would store more information in your item than just a string.
            // (e.g. make Items[] an array of structure, store color/type etc.)
            ImVec4 color;
            bool hasColor = false;
            if (strstr(item, "[ERROR]")) {
                color = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
                hasColor = true;
            } else if (strstr(item, "[LOG]")) {
                color = ImVec4(0.7f, 0.7f, 1.0f, 1.0f);
                hasColor = true;
            } else if (strncmp(item, "# ", 2) == 0) {
                color = ImVec4(1.0f, 0.8f, 0.6f, 1.0f);
                hasColor = true;
            }
            if (hasColor)
                ImGui::PushStyleColor(ImGuiCol_Text, color);
            ImGui::TextUnformatted(item);
            if (hasColor)
                ImGui::PopStyleColor();
        }

        // Keep up at the bottom of the scroll region if we were already at the bottom at the beginning of the frame.
        // Using a scrollbar or mouse-wheel will take away from the bottom edge.
        if (scrollToBottom || (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())) {
            ImGui::SetScrollHereY(1.0f);
        } 
        scrollToBottom = false;

        ImGui::PopStyleVar();
    }
    ImGui::EndChild();
};
