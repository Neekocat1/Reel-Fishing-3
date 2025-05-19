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
    ImGui::InputText("##serveraddress", AP_client.GetServerAddressBuffer(), AP_Client_consts::MAX_ADDRESS_LENGTH);
    ImGui::Text("Slot Name");
    ImGui::InputText("##slotname", AP_client.GetSlotNameBuffer(), AP_Client_consts::MAX_PLAYER_NAME_LENGHT);
    ImGui::Text("Password (leave blank for no password)");
    ImGui::InputText("##password", AP_client.GetPasswordBuffer(),
                     AP_Client_consts::MAX_PASSWORD_LENGTH, ImGuiInputTextFlags_Password);

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