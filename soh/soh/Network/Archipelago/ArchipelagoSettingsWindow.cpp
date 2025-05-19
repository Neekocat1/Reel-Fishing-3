#include "ArchipelagoSettingsWindow.h"
#include "Archipelago.h"

#include "soh/SohGui/UIWidgets.hpp"
#include "soh/SohGui/SohGui.hpp"
#include "soh/Network/Archipelago/ArchipelagoConsoleWindow.h"

void ArchipelagoSettingsWindow::DrawElement() {
    ArchipelagoClient& AP_client = ArchipelagoClient::GetInstance();

    ImGui::SeparatorText("Connection info");

    UIWidgets::PushStyleCombobox(THEME_COLOR);
    ImGui::PushStyleColor(ImGuiCol_Border, UIWidgets::ColorValues.at(THEME_COLOR));

    ImGui::Text("Server Address");
    UIWidgets::CVarInputString("##ArchipelagoServerAddress", CVAR_REMOTE_ARCHIPELAGO("ServerAddress"),
                    UIWidgets::InputOptions()
                        .Color(THEME_COLOR)
                        .PlaceholderText("archipelago.gg:38281")
                        .DefaultValue("archipelago.gg:38281")
                        .Size(ImVec2(ImGui::GetFontSize() * 15, 0))
                        .LabelPosition(UIWidgets::LabelPositions::None));
    ImGui::Text("Slot Name");
    UIWidgets::CVarInputString("##ArchipelagoSlotName", CVAR_REMOTE_ARCHIPELAGO("SlotName"),
                               UIWidgets::InputOptions()
                                   .Color(THEME_COLOR)
                                   .Size(ImVec2(ImGui::GetFontSize() * 15, 0))
                                   .LabelPosition(UIWidgets::LabelPositions::None));
    ImGui::Text("Password (leave blank for no password)");
    UIWidgets::CVarInputString("##ArchipelagoPassword", CVAR_REMOTE_ARCHIPELAGO("Password"),
                               UIWidgets::InputOptions()
                                   .Color(THEME_COLOR)
                                   .Size(ImVec2(ImGui::GetFontSize() * 15, 0))
                                   .LabelPosition(UIWidgets::LabelPositions::None));
    ImGui::PopStyleColor();
    UIWidgets::PopStyleCombobox();

    if (UIWidgets::Button("Connect", UIWidgets::ButtonOptions().Color(THEME_COLOR).Size(ImVec2(0.0, 0.0)))) {
        bool success = AP_client.StartClient();
        ArchipelagoConsole_SendMessage("[LOG] Trying to connect...");
    }

    ImGui::SameLine();
    ImGui::Text(ArchipelagoClient::GetInstance().GetConnectionStatus());

    // Temporary developer helpers
    ImGui::SeparatorText("Developer Tools");
    if (UIWidgets::Button("Scout", UIWidgets::ButtonOptions().Color(THEME_COLOR).Size(ImVec2(0.0, 0.0)))) {
        AP_client.StartLocationScouts();
    }
    ImGui::SameLine();
    if (UIWidgets::Button("Link up", UIWidgets::ButtonOptions().Color(THEME_COLOR).Size(ImVec2(0.0, 0.0)))) {
        CVarSetInteger("ArchipelagoConnected", 1);
    }
    ImGui::SameLine();
    if (UIWidgets::Button("Give Blue Rupee", UIWidgets::ButtonOptions().Color(THEME_COLOR).Size(ImVec2(0.0, 0.0)))) {
        ArchipelagoClient::GetInstance().OnItemReceived(66077, true);
    }

};
