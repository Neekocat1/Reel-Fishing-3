#include "ArchipelagoSettingsWindow.h"
#include "Archipelago.h"

#include "soh/SohGui/UIWidgets.hpp"
#include "soh/SohGui/SohGui.hpp"
#include "soh/Network/Archipelago/ArchipelagoConsoleWindow.h"

void ArchipelagoSettingsWindow::DrawElement() {
    ArchipelagoClient& AP_client = ArchipelagoClient::GetInstance();
    ImGui::SeparatorText("Connection info");
    ImGui::InputText("Server Address", AP_client.GetServerAddressBuffer(), AP_Client_consts::MAX_ADDRESS_LENGTH);
    ImGui::InputText("Slot Name", AP_client.GetSlotNameBuffer(), AP_Client_consts::MAX_PLAYER_NAME_LENGHT);
    ImGui::InputText("Password (leave blank for no password)", AP_client.GetPasswordBuffer(),
                     AP_Client_consts::MAX_PASSWORD_LENGTH, ImGuiInputTextFlags_Password);

    if (ImGui::Button("Connect")) {
        bool success = AP_client.StartClient();
        ArchipelagoConsole_SendMessage("[LOG] Trying to connect...");
    }

    ImGui::SameLine();
    ImGui::Text(ArchipelagoClient::GetInstance().GetConnectionStatus());

    if (ImGui::Button("Scout")) {
        AP_client.StartLocationScouts();
    }
    ImGui::SameLine();
    if (ImGui::Button("Link up")) {
        CVarSetInteger("ArchipelagoConnected", 1);
    }

    UIWidgets::PaddedSeparator();
    
    if(ImGui::Button("Give Blue Rupee")) {
        ArchipelagoClient::GetInstance().OnItemReceived(66077, true);
    }
};