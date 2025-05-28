#include "ArchipelagoConsoleWindow.h"

#include "soh/SohGui/UIWidgets.hpp"
#include "soh/SohGui/SohGui.hpp"
#include "soh/OTRGlobals.h"

std::vector<std::list<APClient::TextNode>> Items;
bool autoScroll = true;

using namespace UIWidgets;

void ArchipelagoConsole_SendMessage(const char* fmt, bool debugMessage, ...) {
    if (debugMessage && CVarGetInteger(CVAR_REMOTE_ARCHIPELAGO("DebugEnabled"), 0) == 0) {
        return;
    }
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, IM_ARRAYSIZE(buf), fmt, args);
    buf[IM_ARRAYSIZE(buf) - 1] = 0;
    va_end(args);
    APClient::TextNode node;
    if (strstr(buf, "[ERROR]")) {
        node.type = "ERROR";
        node.color = "ERROR";
    } else if (strstr(buf, "[LOG]")) {
        node.type = "LOG";
        node.color = "LOG";
    }
    node.text = std::string(buf);
    std::list<APClient::TextNode> line;
    line.push_back(node);
    Items.push_back(line);
}

void ArchipelagoConsole_PrintJson(const std::list<APClient::TextNode> nodes) {
    Items.push_back(nodes);
}

void ArchipelagoConsoleWindow::DrawElement() {
    ImGui::SeparatorText("Archipelago Log");

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(15.0f, 12.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 2.0f));

    if (ImGui::BeginChild("ScrollingRegion", ImVec2(0, 400), ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_HorizontalScrollbar)) {

        for (const std::list<APClient::TextNode>& line : Items) {
            for(const APClient::TextNode& node : line) {
                APClient* client = ArchipelagoClient::GetInstance().apClient.get();
                std::string color;
                std::string text;

                if(node.type == "player_id") {
                    int id = std::stoi(node.text);
                    if (color.empty() && id == client->get_player_number()) color = "magenta";
                    else if(color.empty()) color = "yellow";
                    text = client->get_player_alias(id);
                } else if (node.type == "item_id") {
                    int64_t id = std::stoll(node.text);
                    if(color.empty()) {
                        if (node.flags & APClient::ItemFlags::FLAG_ADVANCEMENT) color = "plum";
                        else if (node.flags & APClient::ItemFlags::FLAG_NEVER_EXCLUDE) color = "slateblue";
                        else if (node.flags & APClient::ItemFlags::FLAG_TRAP) color = "salmon";
                        else color = "cyan";
                    }
                    text = client->get_item_name(id, client->get_player_game(node.player));
                } else if (node.type == "location_id") {
                    int64_t id = std::stoll(node.text);
                    if (color.empty()) color = "blue";
                    text = client->get_location_name(id, client->get_player_game(node.player));
                } else if (node.type == "hint_status") {
                    text = node.text;
                    if (node.hintStatus == APClient::HINT_FOUND) color = "green";
                    else if (node.hintStatus == APClient::HINT_UNSPECIFIED) color = "grey";
                    else if (node.hintStatus == APClient::HINT_NO_PRIORITY) color = "slateblue";
                    else if (node.hintStatus == APClient::HINT_AVOID) color = "salmon";
                    else if (node.hintStatus == APClient::HINT_PRIORITY) color = "plum";
                    else color = "red";  // unknown status -> red
                } else if (node.type == "ERROR") {
                    color = "ERROR";
                    text = node.text;
                } else if (node.type == "LOG") {
                    color = "LOG";
                    text = node.text;
                } else {
                    color = "white";
                    text = node.text;
                }

                ImGui::PushStyleColor(ImGuiCol_Text, getColorVal(color));
                ImGui::TextUnformatted(text.c_str());
                ImGui::SameLine();
                ImGui::PopStyleColor();
            }
            ImGui::NewLine();
        }

        // Keep up at the bottom of the scroll region if we were already at the bottom at the beginning of the frame.
        // Using a scrollbar or mouse-wheel will take away from the bottom edge.
        if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
            ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
};

ImVec4 getColorVal(const std::string& color) {
    if (color == "ERROR") {
        return ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
    } else if(color =="LOG") {
        return ImVec4(0.7f, 0.7f, 1.0f, 1.0f);
    } else if(color == "black") {
        return ImVec4(0.000f, 0.000f, 0.000f, 1.00f);
    } else if(color == "red") {
        return ImVec4(0.933f, 0.000f, 0.000f, 1.00f);
    } else if(color == "green") {
        return ImVec4(0.000f, 1.000f, 0.498f, 1.00f);
    } else if(color == "yellow") {
        return ImVec4(0.980f, 0.980f, 0.824f, 1.00f);
    } else if(color == "blue") {
        return ImVec4(0.392f, 0.584f, 0.929f, 1.00f);
    } else if(color == "cyan") {
        return ImVec4(0.000f, 0.933f, 0.933f, 1.00f);
    } else if(color == "magenta") {
        return ImVec4(0.933f, 0.000f, 0.933f, 1.00f);
    } else if(color == "slateblue") {
        return ImVec4(0.427f, 0.545f, 0.910f, 1.00f);
    } else if(color == "plum") {
        return ImVec4(0.686f, 0.600f, 0.937f, 1.00f);
    } else if(color == "salmon") {
        return ImVec4(0.980f, 0.502f, 0.447f, 1.00f);
    } else if(color == "white") {
        return ImVec4(0.93f, 0.93f, 0.93f, 1.00f);
    } else if(color == "orange") {
        return ImVec4(1.000, 0.467f, 0.000f, 1.000f);
    }
    return ImVec4(0.93f, 0.93f, 0.93f, 1.00f);
}