#include "ArchipelagoSettingsWindow.h"
#include "Archipelago.h"

#include "soh/SohGui/UIWidgets.hpp"
#include "soh/SohGui/SohGui.hpp"
#include "soh/Network/Archipelago/ArchipelagoConsoleWindow.h"

void ArchipelagoSettingsWindow::DrawElement() {
    ArchipelagoClient& AP_client = ArchipelagoClient::getInstance();
    ImGui::SeparatorText("Connection info");
    ImGui::InputText("Server Address", AP_client.get_server_address_buff(), AP_Client_consts::MAX_ADDRESS_LENGTH);
    ImGui::InputText("Slot Name", AP_client.get_slot_name_buff(), AP_Client_consts::MAX_PLAYER_NAME_LENGHT);
    ImGui::InputText("Password (leave blank for no password)", AP_client.get_password_buff(),
                     AP_Client_consts::MAX_PASSWORD_LENGTH, ImGuiInputTextFlags_Password);

    if (ImGui::Button("Connect")) {
        bool success = AP_client.start_client();
        ArchipelagoConsole_SendMessage("[LOG] Trying to connect...");
    }

    ImGui::SameLine();
    ImGui::Text(ArchipelagoClient::getInstance().get_connection_status());

    if (ImGui::Button("scout")) {
        AP_client.start_location_scouts();
    }
    ImGui::SameLine();
    if (ImGui::Button("link up")) {
        CVarSetInteger("archipelago_connected", 1);
    }

    UIWidgets::PaddedSeparator();
    
    if(ImGui::Button("give blue ruppie")) {
        ArchipelagoClient::getInstance().on_item_recieved(66077, true);
    }
};