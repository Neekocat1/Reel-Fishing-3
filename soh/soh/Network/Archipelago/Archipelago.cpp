#include "Archipelago.h"
#include "soh/util.h"
#include <apuuid.hpp>
#include <apclient.hpp>

#include <fstream>
#include <filesystem>
#include <iostream>

#include "soh/Network/Archipelago/ArchipelagoConsoleWindow.h"
#include "soh/Enhancements/randomizer/randomizerTypes.h"
#include "soh/Enhancements/randomizer/static_data.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"

ArchipelagoClient::ArchipelagoClient() {
    std::string uuid = ap_get_uuid("uuid");

    ItemRecievedCallback = nullptr;
    game_won = false;

    namespace apc = AP_Client_consts;
    CVarSetInteger("ArchipelagoConnected", 0);
    strncpy(serverAddress, CVarGetString(apc::SETTING_ADDRESS, apc::DEFAULT_SERVER_NAME), apc::MAX_ADDRESS_LENGTH);
    strncpy(slotName, CVarGetString(apc::SETTING_NAME, ""), apc::MAX_PLAYER_NAME_LENGHT);

    // call poll every frame
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameFrameUpdate>([](){ArchipelagoClient::GetInstance().Poll();});
}

ArchipelagoClient& ArchipelagoClient::GetInstance() {
    static ArchipelagoClient Client;
    return Client;
}

bool ArchipelagoClient::StartClient() {
    if(apClient != NULL) {
        apClient.reset();
    }

    apClient = std::unique_ptr<APClient>(new APClient(uuid, AP_Client_consts::AP_GAME_NAME, serverAddress));

    apClient->set_room_info_handler([&]() {
        std::list<std::string> tags;
        // tags.push_back("DeathLink");     // todo, implement deathlink
        apClient->ConnectSlot(slotName, password, 0b001, tags);
    });

    apClient->set_items_received_handler([&](const std::list<APClient::NetworkItem>& items) {
        for(const APClient::NetworkItem& item : items) {
            OnItemReceived(item.item, false);  // todo get rid of notify, since it doesn't work for us right now anyway
        }
    });

    apClient->set_location_info_handler([&](const std::list<APClient::NetworkItem>& items) {
        scoutedItems.clear();
    
        for(const APClient::NetworkItem& item: items) {
            ApItem apItem;
            const std::string game = apClient->get_player_game(item.player);
            apItem.itemName = apClient->get_item_name(item.item, game);
            apItem.locationName = apClient->get_location_name(item.location, game);
            apItem.playerName = apClient->get_player_alias(item.player);
            apItem.flags = item.flags;
            apItem.index = item.index;
            scoutedItems.push_back(apItem);

            const std::string itemName = apItem.itemName;
            const std::string playerName = apItem.playerName;
            const std::string locationName = apItem.locationName;
            std::string logMessage = "[LOG] Location scouted: " + itemName + " for " + playerName + " in location " + locationName;
            ArchipelagoConsole_SendMessage(logMessage.c_str());
        }
    
    });    // todo maybe move these functions to a lambda, since they don't have to be static anymore
    apClient->set_location_checked_handler([&](const std::list<int64_t> locations) {
        // todo implement me
    });

    SaveData();
    return true;
}

void ArchipelagoClient::StartLocationScouts() {
    std::set<int64_t> missing_loc_set = apClient->get_missing_locations();
    std::set<int64_t> found_loc_set = apClient->get_checked_locations();
    std::list<int64_t> location_list;
    for(const int64_t loc_id : missing_loc_set) {
        location_list.emplace_back(loc_id);
    }
    for(const int64_t loc_id : found_loc_set) {
        location_list.emplace_back(loc_id);
    }
    apClient->LocationScouts(location_list);
}

void ArchipelagoClient::SaveData() {
    CVarSetString(AP_Client_consts::SETTING_ADDRESS, serverAddress);
    CVarSetString(AP_Client_consts::SETTING_NAME, slotName);
}

bool ArchipelagoClient::IsConnected() {
    return apClient->get_state() == APClient::State::SLOT_CONNECTED;
}

void ArchipelagoClient::check_location(RandomizerCheck SoH_check_id) {
    //std::string_view ap_name = Rando::StaticData::SohCheckToAP[SoH_check_id];
    std::string ap_name = Rando::StaticData::GetLocation(SoH_check_id)->GetName();
    if(ap_name.empty()) {
        return;
    }
    int64_t ap_item_id = apClient->get_location_id(std::string(ap_name));
    std::string logMessage = "[LOG] Checked: " + ap_name + "(" + std::to_string(ap_item_id) + "), sending to AP server";
    ArchipelagoConsole_SendMessage(logMessage.c_str());

// currently not sending, because i only get so many real chances
    if(!IsConnected()) {
        return;
    }
    apClient->LocationChecks({ap_item_id});
}

void ArchipelagoClient::AddItemRecievedCallback(std::function<void(const std::string&)> callback) {
    ItemRecievedCallback = callback;
}

void ArchipelagoClient::RemoveItemRecievedCallback(std::function<void(const std::string&)> old_callback) {
    ItemRecievedCallback = nullptr;
}

void ArchipelagoClient::OnConnected() {
    // todo implement me
    ArchipelagoConsole_SendMessage("[LOG] AP Connected!");
}
//void ArchipelagoClient::on_couldntConnect(AP_ConnectionStatus connection_status) {
//    // todo implement me
//}

void ArchipelagoClient::OnItemReceived(int64_t recieved_item_id, bool notify_player) {
    // call each callback
    const std::string item_name = apClient->get_item_name(recieved_item_id, AP_Client_consts::AP_GAME_NAME);
    ArchipelagoClient& ap_client = ArchipelagoClient::GetInstance();
    if(ap_client.ItemRecievedCallback) {
        std::string logMessage = "[LOG] Item recieved: " + item_name + ". Notify: " + std::to_string(notify_player);
        ArchipelagoConsole_SendMessage(logMessage.c_str());
        ap_client.ItemRecievedCallback.operator()(item_name);   // somehow passing it through the itemname breaks it????
    }
}

void ArchipelagoClient::SendGameWon() {
    if(!game_won) {
        apClient->StatusUpdate(APClient::ClientStatus::GOAL);
        game_won = true;
    }
}

void ArchipelagoClient::Poll() {
    if(apClient == nullptr) {
        return;
    }
    
    apClient->poll();
}

const std::string& ArchipelagoClient::GetSlotName() const {
    if(apClient == NULL) {
        return "";
    }

    return apClient->get_slot();
}

char* ArchipelagoClient::GetServerAddressBuffer() {
    return serverAddress;
}
char* ArchipelagoClient::GetSlotNameBuffer() {
    return slotName;
}
char* ArchipelagoClient::GetPasswordBuffer() {
    return password;
}

const std::map<std::string, int>& ArchipelagoClient::GetSlotData() {
    return slotData;
}

const std::vector<ArchipelagoClient::ApItem>& ArchipelagoClient::GetScoutedItems() {
    return scoutedItems;
}

const char* ArchipelagoClient::GetConnectionStatus() {
    if (!apClient) {
        return "";
    }

    APClient::State clientStatus = apClient->get_state();

    switch (clientStatus) { 
        case APClient::State::DISCONNECTED: {
            return "Disconnected!";
        }
        case APClient::State::SOCKET_CONNECTING: {
            return "Socket Connecting!";
        }
        case APClient::State::SOCKET_CONNECTED: {
            return "Socket Connected!";
        }
        case APClient::State::ROOM_INFO: {
            return "Room info Recieved!";
        }
        case APClient::State::SLOT_CONNECTED: {
            return "Slot Connected!";
        }
        default:
            return "";
    }
}
