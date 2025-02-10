#include "archipelago.h"
#include "soh/UIWidgets.hpp"
#include "soh/util.h"

#include <fstream>
#include <filesystem>
#include <spdlog/spdlog.h>
#include <iostream>

#include "fixed_string.hpp"
#include "randomizerTypes.h"
#include "static_data.h"

//extern "C" {
//    #include "include/z64item.h"
//    #include "objects/gameplay_keep/gameplay_keep.h"
//    extern SaveContext gSaveContext;
//    extern PlayState* gPlayState;
//}

//constexpr const char* requestedSlotData(int i)

using namespace fixstr; //https://github.com/unterumarmung/fixed_string
template<fixed_string key>
struct CallbackWrapper {
    static void SlotCallbackFunc(int id) {
        ArchipelagoClient::getInstance().add_slot_data(key, id);
        SPDLOG_TRACE("Recieved Slot data ({}, {})", key, id);
    }
};

template<fixed_string key>
auto SubscribeToSlotData() {
    AP_RegisterSlotDataIntCallback(std::string(key), CallbackWrapper<key>::SlotCallbackFunc);
}

ArchipelagoClient::ArchipelagoClient() {
    ItemRecievedCallback = nullptr;

    namespace apc = AP_Client_consts;
    CVarSetInteger("archipelago_connected", 0);
    strncpy(server_address, CVarGetString(apc::SETTING_ADDRESS, apc::DEFAULT_SERVER_NAME), apc::MAX_ADDRESS_LENGTH);
    strncpy(slot_name, CVarGetString(apc::SETTING_NAME, ""), apc::MAX_PLAYER_NAME_LENGHT);
}

ArchipelagoClient& ArchipelagoClient::getInstance() {
    static ArchipelagoClient Client;
    return Client;
}


void ArchipelagoClient::add_slot_data(std::string_view key, int id) {
    slot_data.insert(std::pair<std::string_view, int>(key, id));
}

void registerSlotCallbacks() {
    SubscribeToSlotData<"open_forest">();
    SubscribeToSlotData<"open_kakoriko">();
    SubscribeToSlotData<"open_door_of_time">();
    SubscribeToSlotData<"zora_fountain">();
    SubscribeToSlotData<"gerudo_fortress">();
    SubscribeToSlotData<"bridge">();
    SubscribeToSlotData<"bridge_stones">();
    SubscribeToSlotData<"bridge_medallions">();
    SubscribeToSlotData<"bridge_rewards">();
    SubscribeToSlotData<"bridge_tokens">();
    SubscribeToSlotData<"bridge_hearts">();
    SubscribeToSlotData<"shuffle_ganon_bosskey">();
    SubscribeToSlotData<"ganon_bosskey_medallions">();
    SubscribeToSlotData<"ganon_bosskey_stones">();
    SubscribeToSlotData<"ganon_bosskey_rewards">();
    SubscribeToSlotData<"ganon_bosskey_tokens">();
    SubscribeToSlotData<"ganon_bosskey_hearts">();
    SubscribeToSlotData<"trials">();
    SubscribeToSlotData<"triforce_hunt">();
    SubscribeToSlotData<"triforce_goal">();
    SubscribeToSlotData<"extra_triforce_percentage">();
    SubscribeToSlotData<"shopsanity">();
    SubscribeToSlotData<"shop_slots">();
    SubscribeToSlotData<"shopsanity_prices">();
    SubscribeToSlotData<"tokensanity">();
    SubscribeToSlotData<"dungeon_shortcuts">();
    SubscribeToSlotData<"mq_dungeons_mode">();
    SubscribeToSlotData<"mq_dungeons_count">();
    SubscribeToSlotData<"shuffle_interior_entrances">();
    SubscribeToSlotData<"shuffle_grotto_entrances">();
    SubscribeToSlotData<"shuffle_dungeon_entrances">();
    SubscribeToSlotData<"shuffle_overworld_entrances">();
    SubscribeToSlotData<"shuffle_bosses">();
    SubscribeToSlotData<"key_rings">();
    SubscribeToSlotData<"enhance_map_compass">();
    SubscribeToSlotData<"shuffle_mapcompass">();
    SubscribeToSlotData<"shuffle_smallkeys">();
    SubscribeToSlotData<"shuffle_hideoutkeys">();
    SubscribeToSlotData<"shuffle_bosskeys">();
    SubscribeToSlotData<"logic_rules">();
    SubscribeToSlotData<"logic_no_night_tokens_without_suns_song">();
    SubscribeToSlotData<"warp_songs">();
    SubscribeToSlotData<"shuffle_song_items">();
    SubscribeToSlotData<"shuffle_medigoron_carpet_salesman">();
    SubscribeToSlotData<"shuffle_frog_song_rupees">();
    SubscribeToSlotData<"shuffle_scrubs">();
    SubscribeToSlotData<"shuffle_child_trade">();
    SubscribeToSlotData<"shuffle_freestanding_items">();
    SubscribeToSlotData<"shuffle_pots">();
    SubscribeToSlotData<"shuffle_crates">();
    SubscribeToSlotData<"shuffle_cows">();
    SubscribeToSlotData<"shuffle_beehives">();
    SubscribeToSlotData<"shuffle_kokiri_sword">();
    SubscribeToSlotData<"shuffle_ocarinas">();
    SubscribeToSlotData<"shuffle_gerudo_card">();
    SubscribeToSlotData<"shuffle_beans">();
    SubscribeToSlotData<"starting_age">();
    SubscribeToSlotData<"bombchus_in_logic">();
    SubscribeToSlotData<"spawn_positions">();
    SubscribeToSlotData<"owl_drops">();
    SubscribeToSlotData<"no_epona_race">();
    SubscribeToSlotData<"skip_some_minigame_phases">();
    SubscribeToSlotData<"complete_mask_quest">();
    SubscribeToSlotData<"free_scarecrow">();
    SubscribeToSlotData<"plant_beans">();
    SubscribeToSlotData<"chicken_count">();
    SubscribeToSlotData<"big_poe_count">();
    SubscribeToSlotData<"fae_torch_count">();
    SubscribeToSlotData<"blue_fire_arrows">();
    SubscribeToSlotData<"damage_multiplier">();
    SubscribeToSlotData<"deadly_bonks">();
    SubscribeToSlotData<"starting_tod">();
    SubscribeToSlotData<"junk_ice_traps">();
    SubscribeToSlotData<"start_with_consumables">();
    SubscribeToSlotData<"adult_trade_start">();
}

bool ArchipelagoClient::start_client() {
    switch(AP_GetConnectionStatus()) {
        case AP_ConnectionStatus::ConnectionRefused:
            SPDLOG_TRACE("refused");
            break;
        case AP_ConnectionStatus::Authenticated:
            SPDLOG_TRACE("Authenticated");
            break;
        case AP_ConnectionStatus::Connected:
            SPDLOG_TRACE("Connected");
            break;
        case AP_ConnectionStatus::Disconnected:
            SPDLOG_TRACE("Disconnected");
            break;
    }

    if(AP_GetConnectionStatus() != AP_ConnectionStatus::Disconnected) {
        SPDLOG_TRACE("AP already connected, shutting it down");
        AP_Shutdown();
    }

    AP_Init(server_address, "Ocarina of Time", slot_name, password);
    //AP_SetClientConnectedCallback(&ArchipelagoClient::on_connected);  // currently broken :(
    //AP_SetClientCouldntConnectCallback(5, &ArchipelagoClient::on_couldntConnect);
    AP_SetItemClearCallback(&ArchipelagoClient::on_clear_items);
    AP_SetItemRecvCallback(&ArchipelagoClient::on_item_recieved);
    AP_SetLocationCheckedCallback(&ArchipelagoClient::on_location_checked);
    AP_SetLocationInfoCallback(&ArchipelagoClient::on_location_scouted);
    registerSlotCallbacks();
    AP_Start();
    AP_ConnectionStatus conn_status = AP_GetConnectionStatus();

    //switch(conn_status) {
    //    case AP_ConnectionStatus::Authenticated:
    //        SPDLOG_TRACE("Authenticated");
    //        break;
    //    case AP_ConnectionStatus::Connected:
    //        SPDLOG_TRACE("Connected");
    //        break;
    //    case AP_ConnectionStatus::Disconnected:
    //        SPDLOG_TRACE("Disconnected");
    //        break;
    //}
    save_data();
    return conn_status == AP_ConnectionStatus::Connected;
}

void ArchipelagoClient::start_location_scouts() {
    AP_SendLocationScouts(AP_GetAllLocations(), false);
}

void ArchipelagoClient::save_data() {
    CVarSetString(AP_Client_consts::SETTING_ADDRESS, server_address);
    CVarSetString(AP_Client_consts::SETTING_NAME, slot_name);
}

bool ArchipelagoClient::isConnected() {
    return AP_GetConnectionStatus() == AP_ConnectionStatus::Authenticated;
}

void ArchipelagoClient::check_location(RandomizerCheck SoH_check_id) {
    std::string_view ap_name = Rando::StaticData::SohCheckToAP[SoH_check_id];
    int64_t ap_item_id = CheckNameToId(std::string(ap_name));
    SPDLOG_TRACE("Checked: {}({}), sending to AP server", ap_name, ap_item_id);

// currently not sending, because i only get so many real chances
    if(!isConnected()) {
        return;
    }
    AP_SendItem(ap_item_id);
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
void ArchipelagoClient::on_couldntConnect(AP_ConnectionStatus connection_status) {
    // todo implement me
}


void ArchipelagoClient::on_clear_items() {
    // todo implement me
}

void ArchipelagoClient::on_item_recieved(int64_t recieved_item_id, bool notify_player) {
    // call each callback
    SPDLOG_TRACE("Trying to give rupie...");
    std::string item_name = getAPitemName(recieved_item_id);
    ArchipelagoClient& ap_client = ArchipelagoClient::getInstance();
    if(ap_client.ItemRecievedCallback) {
        SPDLOG_TRACE("Giving Rupie! {}", item_name);
        ap_client.ItemRecievedCallback.operator()(item_name);   // somehow passing it through the itemname breaks it????
    }
}

void ArchipelagoClient::on_location_checked(int64_t location_id) {
    // todo implement me
}

void ArchipelagoClient::on_location_scouted(std::vector<AP_NetworkItem> network_items) {
    for(const AP_NetworkItem& item: network_items) {
        SPDLOG_TRACE("Location scouted: {} for {} in location {}", item.itemName, item.playerName, item.locationName);
    }
    getInstance().scouted_items = network_items;
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

const std::vector<AP_NetworkItem>& ArchipelagoClient::get_scouted_items() {
    return scouted_items;
}


void ArchipelagoWindow::ArchipelagoDrawConnectPage() {
    ArchipelagoClient& AP_client = ArchipelagoClient::getInstance();
    ImGui::SeparatorText("Connection info");
    ImGui::InputText("Server Address", AP_client.get_server_address_buff(), AP_Client_consts::MAX_ADDRESS_LENGTH);
    ImGui::InputText("Slot Name", AP_client.get_slot_name_buff(), AP_Client_consts::MAX_PLAYER_NAME_LENGHT);
    ImGui::InputText("Password (leave blank for no password)", AP_client.get_password_buff(), AP_Client_consts::MAX_PASSWORD_LENGTH, ImGuiInputTextFlags_Password);

    static char connected_text[25] = "Disconnected";
    if(ImGui::Button("Connect")) {
        bool success = AP_client.start_client();
    }
        
    ImGui::SameLine();
    ImGui::Text(connected_text);
    AP_ConnectionStatus con_status = AP_GetConnectionStatus();
    if(con_status == AP_ConnectionStatus::Connected) {
        strncpy(connected_text, "Connected!", 25);
    } else if(con_status == AP_ConnectionStatus::Authenticated) {
        strncpy(connected_text, "Authenticated!", 25);
    }
    else {
        strncpy(connected_text, "Not Connected", 25);
    }
    
    if(ImGui::Button("scout")) {
        AP_client.start_location_scouts();
    }
    ImGui::SameLine();
    if(ImGui::Button("link up")) {
        CVarSetInteger("archipelago_connected", 1);
    }

};

void ArchipelagoWindow::DrawElement() {
    ArchipelagoDrawConnectPage();
    UIWidgets::PaddedSeparator();
    
    if(ImGui::Button("give blue ruppie")) {
        ArchipelagoClient::getInstance().on_item_recieved(66077, true);
    }
};