#include "ArchipelagoSettingsWindow.h"
#include "Archipelago.h"

#include <apclient.hpp>
#include "soh/SohGui/UIWidgets.hpp"
#include "soh/SohGui/SohGui.hpp"

void ArchipelagoSettingsWindow::DrawElement() {
    ArchipelagoClient& AP_client = ArchipelagoClient::getInstance();
    ImGui::SeparatorText("Connection info");
    ImGui::InputText("Server Address", AP_client.get_server_address_buff(), AP_Client_consts::MAX_ADDRESS_LENGTH);
    ImGui::InputText("Slot Name", AP_client.get_slot_name_buff(), AP_Client_consts::MAX_PLAYER_NAME_LENGHT);
    ImGui::InputText("Password (leave blank for no password)", AP_client.get_password_buff(),
                     AP_Client_consts::MAX_PASSWORD_LENGTH, ImGuiInputTextFlags_Password);

    static char connected_text[25] = "Disconnected";
    if (ImGui::Button("Connect")) {
        bool success = AP_client.start_client();
    }

    ImGui::SameLine();
    ImGui::Text(connected_text);

    APClient::State con_state = APClient::State::DISCONNECTED;

    if (AP_client.apclient) {
        con_state = AP_client.apclient->get_state();
    }

    switch (con_state) {
        case APClient::State::DISCONNECTED: {
            strncpy(connected_text, "Disconnected!", 25);
            break;
        }
        case APClient::State::SOCKET_CONNECTING: {
            strncpy(connected_text, "Socket Connecting!", 25);
            break;
        }
        case APClient::State::SOCKET_CONNECTED: {
            strncpy(connected_text, "Socket Connected!", 25);
            break;
        }
        case APClient::State::ROOM_INFO: {
            strncpy(connected_text, "Room info Recieved!", 25);
            break;
        }
        case APClient::State::SLOT_CONNECTED: {
            strncpy(connected_text, "Slot Connected!", 25);
            break;
        }
    };

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