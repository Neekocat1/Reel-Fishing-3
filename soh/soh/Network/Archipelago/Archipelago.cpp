#include "Archipelago.h"
#include "soh/util.h"
#include <apuuid.hpp>
#include <apclient.hpp>

#include <fstream>
#include <filesystem>
#include <spdlog/spdlog.h>
#include <iostream>

#include "soh/Enhancements/randomizer/randomizerTypes.h"
#include "soh/Enhancements/randomizer/static_data.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"

ArchipelagoClient::ArchipelagoClient() {
    std::string uuid = ap_get_uuid("uuid");

    ItemRecievedCallback = nullptr;
    game_won = false;

    namespace apc = AP_Client_consts;
    CVarSetInteger("archipelago_connected", 0);
    strncpy(server_address, CVarGetString(apc::SETTING_ADDRESS, apc::DEFAULT_SERVER_NAME), apc::MAX_ADDRESS_LENGTH);
    strncpy(slot_name, CVarGetString(apc::SETTING_NAME, ""), apc::MAX_PLAYER_NAME_LENGHT);

    // call poll every frame
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameFrameUpdate>([](){ArchipelagoClient::getInstance().poll();});
}

ArchipelagoClient& ArchipelagoClient::getInstance() {
    static ArchipelagoClient Client;
    return Client;
}

bool ArchipelagoClient::start_client() {
    if(apclient != NULL) {
        apclient.reset();
    }

    apclient = std::unique_ptr<APClient>(new APClient(uuid, AP_Client_consts::AP_GAME_NAME, server_address));

    apclient->set_room_info_handler([&]() {
        std::list<std::string> tags;
        // tags.push_back("DeathLink");     // todo, implement deathlink
        apclient->ConnectSlot(slot_name, password, 0b001, tags);
    });

    apclient->set_items_received_handler([&](const std::list<APClient::NetworkItem>& items) {
        for(const APClient::NetworkItem& item : items) {
            on_item_recieved(item.item, false);  // todo get rid of notify, since it doesn't work for us right now anyway
        }
    });

    apclient->set_location_info_handler([&](const std::list<APClient::NetworkItem>& items) {
        scouted_items.clear();
    
        for(const APClient::NetworkItem& item: items) {
            ApItem apItem;
            const std::string game = apclient->get_player_game(item.player);
            apItem.itemName = apclient->get_item_name(item.item, game);
            apItem.locationName = apclient->get_location_name(item.location, game);
            apItem.playerName = apclient->get_player_alias(item.player);
            apItem.flags = item.flags;
            apItem.index = item.index;
            scouted_items.push_back(apItem);

            const std::string itemName = apItem.itemName;
            const std::string playerName = apItem.playerName;
            const std::string locationName = apItem.locationName;
            SPDLOG_TRACE("Location scouted: {} for {} in location {}", itemName, playerName, locationName);
        }
    
    });    // todo maybe move these functions to a lambda, since they don't have to be static anymore
    apclient->set_location_checked_handler([&](const std::list<int64_t> locations) {
        // todo implement me
    });

    save_data();
    return true;
}

void ArchipelagoClient::start_location_scouts() {
    std::set<int64_t> missing_loc_set = apclient->get_missing_locations();
    std::set<int64_t> found_loc_set = apclient->get_checked_locations();
    std::list<int64_t> location_list;
    for(const int64_t loc_id : missing_loc_set) {
        location_list.emplace_back(loc_id);
    }
    for(const int64_t loc_id : found_loc_set) {
        location_list.emplace_back(loc_id);
    }
    apclient->LocationScouts(location_list);
}

void ArchipelagoClient::save_data() {
    CVarSetString(AP_Client_consts::SETTING_ADDRESS, server_address);
    CVarSetString(AP_Client_consts::SETTING_NAME, slot_name);
}

bool ArchipelagoClient::isConnected() {
    return apclient->get_state() == APClient::State::SLOT_CONNECTED;
}

void ArchipelagoClient::check_location(RandomizerCheck SoH_check_id) {
    //std::string_view ap_name = Rando::StaticData::SohCheckToAP[SoH_check_id];
    std::string ap_name = Rando::StaticData::GetLocation(SoH_check_id)->GetName();
    if(ap_name.empty()) {
        return;
    }
    int64_t ap_item_id = apclient->get_location_id(std::string(ap_name));
    SPDLOG_TRACE("Checked: {}({}), sending to AP server", ap_name, ap_item_id);

// currently not sending, because i only get so many real chances
    if(!isConnected()) {
        return;
    }
    apclient->LocationChecks({ap_item_id});
}

void ArchipelagoClient::addItemRecievedCallback(std::function<void(const std::string&)> callback) {
    ItemRecievedCallback = callback;
}

void ArchipelagoClient::removeItemRecievedCallback(std::function<void(const std::string&)> old_callback) {
    ItemRecievedCallback = nullptr;
}

void ArchipelagoClient::on_connected() {
    // todo implement me
    SPDLOG_TRACE("AP Connected!!");
}
//void ArchipelagoClient::on_couldntConnect(AP_ConnectionStatus connection_status) {
//    // todo implement me
//}

void ArchipelagoClient::on_item_recieved(int64_t recieved_item_id, bool notify_player) {
    // call each callback
    const std::string item_name = apclient->get_item_name(recieved_item_id, AP_Client_consts::AP_GAME_NAME);
    ArchipelagoClient& ap_client = ArchipelagoClient::getInstance();
    if(ap_client.ItemRecievedCallback) {
        SPDLOG_TRACE("item recieved: {}, notify: {}", item_name, notify_player);
        ap_client.ItemRecievedCallback.operator()(item_name);   // somehow passing it through the itemname breaks it????
    }
}

void ArchipelagoClient::send_game_won() {
    if(!game_won) {
        apclient->StatusUpdate(APClient::ClientStatus::GOAL);
        game_won = true;
    }
}

void ArchipelagoClient::poll() {
    if(apclient == nullptr) {
        return;
    }
    
    apclient->poll();
}

const std::string& ArchipelagoClient::get_slot_name() const {
    if(apclient == NULL) {
        return "";
    }

    return apclient->get_slot();
}

char* ArchipelagoClient::get_server_address_buff() {
    return server_address;
}
char* ArchipelagoClient::get_slot_name_buff() {
    return slot_name;
}
char* ArchipelagoClient::get_password_buff() {
    return password;
}

const std::map<std::string, int>& ArchipelagoClient::get_slot_data() {
    return slot_data;
}

const std::vector<ArchipelagoClient::ApItem>& ArchipelagoClient::get_scouted_items() {
    return scouted_items;
}

const char* ArchipelagoClient::get_connection_status() {
    if (!apclient) {
        return "No status available";
    }

    APClient::State clientStatus = apclient->get_state();

    switch (clientStatus) { 
        case APClient::State::DISCONNECTED: {
            return "Disconnected!";
            break;
        }
        case APClient::State::SOCKET_CONNECTING: {
            return "Socket Connecting!";
            break;
        }
        case APClient::State::SOCKET_CONNECTED: {
            return "Socket Connected!";
            break;
        }
        case APClient::State::ROOM_INFO: {
            return "Room info Recieved!";
            break;
        }
        case APClient::State::SLOT_CONNECTED: {
            return "Slot Connected!";
            break;
        }
        default:
            return "Unknown Status";
    }
}
