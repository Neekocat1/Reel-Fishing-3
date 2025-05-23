#include "Archipelago.h"
#include "soh/util.h"
#include <apuuid.hpp>
#include <apclient.hpp>

#include <fstream>
#include <filesystem>
#include <iostream>
#include <string>

#include "soh/Network/Archipelago/ArchipelagoConsoleWindow.h"
#include "soh/Enhancements/randomizer/randomizerTypes.h"
#include "soh/Enhancements/randomizer/static_data.h"
#include "soh/Enhancements/randomizer/context.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/ShipInit.hpp"
#include "soh/SaveManager.h"

extern "C" {
#include "variables.h"
#include "macros.h"
extern PlayState* gPlayState;
}

ArchipelagoClient::ArchipelagoClient() {
    std::string uuid = ap_get_uuid("uuid");

    gameWon = false;

    namespace apc = AP_Client_consts;
    CVarSetInteger(CVAR_REMOTE_ARCHIPELAGO("Connected"), 0);

    // call poll every frame
    COND_HOOK(GameInteractor::OnGameFrameUpdate, true, [](){ArchipelagoClient::GetInstance().Poll();});
    COND_HOOK(GameInteractor::OnLoadGame, true, [](int32_t file_id){ArchipelagoClient::GetInstance().GameLoaded();});

}

ArchipelagoClient& ArchipelagoClient::GetInstance() {
    static ArchipelagoClient Client;
    return Client;
}

bool ArchipelagoClient::StartClient() {
    if(apClient != NULL) {
        apClient.reset();
    }

    apClient = std::unique_ptr<APClient>(
        new APClient(uuid, AP_Client_consts::AP_GAME_NAME,
                     CVarGetString(CVAR_REMOTE_ARCHIPELAGO("ServerAddress"), "localhost:38281"), "cacert.pem"));

    apClient->set_room_info_handler([&]() {
        std::list<std::string> tags;
        // tags.push_back("DeathLink");     // todo, implement deathlink
        apClient->ConnectSlot(CVarGetString(CVAR_REMOTE_ARCHIPELAGO("SlotName"), ""),
                              CVarGetString(CVAR_REMOTE_ARCHIPELAGO("Password"), ""),
                              0b001, tags);
    });

    apClient->set_slot_connected_handler([&](const nlohmann::json) {
        ArchipelagoConsole_SendMessage("[LOG] Connected.", false);
        ArchipelagoClient::StartLocationScouts();
    });

    apClient->set_items_received_handler([&](const std::list<APClient::NetworkItem>& items) {
        for(const APClient::NetworkItem& item : items) {
            OnItemReceived(item.item, item.index);
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

        ArchipelagoConsole_SendMessage("[LOG] Scouting finished.");
    });    // todo maybe move these functions to a lambda, since they don't have to be static anymore

    apClient->set_location_checked_handler([&](const std::list<int64_t> locations) {
        // todo implement me
    });

    return true;
}

void ArchipelagoClient::GameLoaded() {
    // if its not an AP save, disconnect
    if(!IS_ARCHIPELAGO) {
        if(apClient != nullptr) {
            apClient->reset();
        }
        return;
    }

    SynchItems();
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

void ArchipelagoClient::SynchItems() {
    ArchipelagoConsole_SendMessage("[LOG] Synching Items and Locations.");

    // send already checked locations
    std::list<int64_t> checkedLocations;
    for(const auto& loc : Rando::StaticData::GetLocationTable()) {
        const RandomizerCheck rc = loc.GetRandomizerCheck();
        if(Rando::Context::GetInstance()->GetItemLocation(rc)->HasObtained()) {
            const int64_t apLocation = apClient->get_location_id(loc.GetName());
            checkedLocations.emplace_back(apLocation);
        }
    }
    std::string locationLog = "[LOG] Synching " + std::to_string(checkedLocations.size())+ " checks already found in game";
    ArchipelagoConsole_SendMessage(locationLog.c_str());

    apClient->LocationChecks(checkedLocations);
    // Open checks that have been found previously but went unsaved
    for(const int64_t apLoc : apClient->get_checked_locations()) {
        // TODO call location checked function to open any unopened checks.
    }

    // Send a Synch request to get any items we may have missed
    ArchipelagoConsole_SendMessage("[LOG] Sending synch request");
    apClient->Sync();
}

bool ArchipelagoClient::IsConnected() {
    return apClient->get_state() == APClient::State::SLOT_CONNECTED;
}

void ArchipelagoClient::CheckLocation(RandomizerCheck sohCheckId) {
    std::string apName = Rando::StaticData::GetLocation(sohCheckId)->GetName();
    if (apName.empty()) {
        return;
    }
    int64_t apItemId = apClient->get_location_id(std::string(apName));

    std::string logMessage = "[LOG] Checked: " + apName + "(" + std::to_string(apItemId) + "), sending to AP server";
    ArchipelagoConsole_SendMessage(logMessage.c_str());

    if(!IsConnected()) {
        return;
    }
    apClient->LocationChecks({ apItemId });
}

void ArchipelagoClient::OnItemReceived(int64_t apItemId, int64_t itemIndex) {
    if(!GameInteractor::IsSaveLoaded(true)) {
        // Don't queue up any items when we aren't in game
        // Any Items missed this way will get synched when we load the save
        return;
    }

    if(itemIndex < gSaveContext.ship.quest.data.archipelago.lastReceivedItemIndex) {
        // Skip recieving any items we already have
        return;
    }

    const std::string item_name = apClient->get_item_name(apItemId, AP_Client_consts::AP_GAME_NAME);
    std::string logMessage = "[Log] Recieved " + item_name;
    ArchipelagoConsole_SendMessage(logMessage.c_str());
    const RandomizerGet item = Rando::StaticData::itemNameToEnum[item_name];
    GameInteractor_ExecuteOnArchipelagoItemRecieved(static_cast<int32_t>(item));
}

void ArchipelagoClient::SendGameWon() {
    if(!gameWon) {
        apClient->StatusUpdate(APClient::ClientStatus::GOAL);
        gameWon = true;
    }
}

void ArchipelagoClient::Poll() {
    if(apClient == nullptr) {
        return;
    }
    
    apClient->poll();
}

const std::string ArchipelagoClient::GetSlotName() const {
    if(apClient == NULL) {
        return "";
    }

    return apClient->get_slot();
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

extern "C" void Archipelago_InitSaveFile() {
    gSaveContext.ship.quest.data.archipelago.isArchipelago = 1;

    std::vector<ArchipelagoClient::ApItem> scoutedItems = ArchipelagoClient::GetInstance().GetScoutedItems();

    for (uint32_t i = 0; i < scoutedItems.size(); i++) {
        RandomizerCheck rc = Rando::StaticData::locationNameToEnum[scoutedItems[i].locationName];
        gSaveContext.ship.quest.data.archipelago.locations[rc].itemType = scoutedItems[i].flags;

        SohUtils::CopyStringToCharArray(gSaveContext.ship.quest.data.archipelago.locations[rc].itemName,
                                        scoutedItems[i].itemName,
                                        ARRAY_COUNT(gSaveContext.ship.quest.data.archipelago.locations[rc].itemName));
        SohUtils::CopyStringToCharArray(gSaveContext.ship.quest.data.archipelago.locations[rc].locationName,
                                        scoutedItems[i].locationName,
            ARRAY_COUNT(gSaveContext.ship.quest.data.archipelago.locations[rc].locationName));
        SohUtils::CopyStringToCharArray(gSaveContext.ship.quest.data.archipelago.locations[rc].playerName,
                                        scoutedItems[i].playerName,
                                        ARRAY_COUNT(gSaveContext.ship.quest.data.archipelago.locations[rc].playerName));
    }
}

void LoadArchipelagoData() {
    SaveManager::Instance->LoadData("isArchipelago", gSaveContext.ship.quest.data.archipelago.isArchipelago);
    SaveManager::Instance->LoadData("lastReceivedItemIndex",
                                    gSaveContext.ship.quest.data.archipelago.lastReceivedItemIndex);
    SaveManager::Instance->LoadData("deathLink", gSaveContext.ship.quest.data.archipelago.deathLink);

    SaveManager::Instance->LoadCharArray("roomHash", gSaveContext.ship.quest.data.archipelago.roomHash,
                                         ARRAY_COUNT(gSaveContext.ship.quest.data.archipelago.roomHash));
    SaveManager::Instance->LoadCharArray("slotName", gSaveContext.ship.quest.data.archipelago.slotName,
                                         ARRAY_COUNT(gSaveContext.ship.quest.data.archipelago.slotName));

    SaveManager::Instance->LoadArray(
        "locations", ARRAY_COUNT(gSaveContext.ship.quest.data.archipelago.locations),
        [](size_t i) { 
            SaveManager::Instance->LoadStruct("", [&i]() {
                SaveManager::Instance->LoadData("itemType",
                                                gSaveContext.ship.quest.data.archipelago.locations[i].itemType);

                SaveManager::Instance->LoadCharArray(
                    "itemName", gSaveContext.ship.quest.data.archipelago.locations[i].itemName,
                    ARRAY_COUNT(gSaveContext.ship.quest.data.archipelago.locations[i].itemName));
                SaveManager::Instance->LoadCharArray(
                    "locationName", gSaveContext.ship.quest.data.archipelago.locations[i].locationName,
                    ARRAY_COUNT(gSaveContext.ship.quest.data.archipelago.locations[i].locationName));
                SaveManager::Instance->LoadCharArray(
                    "playerName", gSaveContext.ship.quest.data.archipelago.locations[i].playerName,
                    ARRAY_COUNT(gSaveContext.ship.quest.data.archipelago.locations[i].playerName));
            });
        });
}

void SaveArchipelagoData(SaveContext* saveContext, int sectionID, bool fullSave) {
    SaveManager::Instance->SaveData("isArchipelago", saveContext->ship.quest.data.archipelago.isArchipelago);
    SaveManager::Instance->SaveData("lastReceivedItemIndex",
                                    saveContext->ship.quest.data.archipelago.lastReceivedItemIndex);
    SaveManager::Instance->SaveData("deathLink", saveContext->ship.quest.data.archipelago.deathLink);

    SaveManager::Instance->SaveData("roomHash", saveContext->ship.quest.data.archipelago.roomHash);
    SaveManager::Instance->SaveData("slotName", saveContext->ship.quest.data.archipelago.slotName);

    SaveManager::Instance->SaveArray(
        "locations", ARRAY_COUNT(saveContext->ship.quest.data.archipelago.locations), [&](size_t i) {
            SaveManager::Instance->SaveStruct("", [&]() {
                SaveManager::Instance->SaveData("itemType",
                                                saveContext->ship.quest.data.archipelago.locations[i].itemType);

                SaveManager::Instance->SaveData("itemName",
                                                saveContext->ship.quest.data.archipelago.locations[i].itemName);
                SaveManager::Instance->SaveData("locationName",
                                                saveContext->ship.quest.data.archipelago.locations[i].locationName);
                SaveManager::Instance->SaveData("playerName",
                                                saveContext->ship.quest.data.archipelago.locations[i].playerName);
            });
        });
}

void InitArchipelagoData(bool isDebug) {
    gSaveContext.ship.quest.data.archipelago.isArchipelago = 0;
    gSaveContext.ship.quest.data.archipelago.lastReceivedItemIndex = 0;
    gSaveContext.ship.quest.data.archipelago.deathLink = 0;

    SohUtils::CopyStringToCharArray(gSaveContext.ship.quest.data.archipelago.roomHash, "",
                                    ARRAY_COUNT(gSaveContext.ship.quest.data.archipelago.roomHash));
    SohUtils::CopyStringToCharArray(gSaveContext.ship.quest.data.archipelago.slotName, "",
                                    ARRAY_COUNT(gSaveContext.ship.quest.data.archipelago.slotName));

    for (uint32_t i = 0; i < ARRAY_COUNT(gSaveContext.ship.quest.data.archipelago.locations); i++) {
        gSaveContext.ship.quest.data.archipelago.locations[i].itemType = -1;

        SohUtils::CopyStringToCharArray(gSaveContext.ship.quest.data.archipelago.locations[i].itemName, "",
                                        ARRAY_COUNT(gSaveContext.ship.quest.data.archipelago.locations[i].itemName));
        SohUtils::CopyStringToCharArray(gSaveContext.ship.quest.data.archipelago.locations[i].locationName, "",
            ARRAY_COUNT(gSaveContext.ship.quest.data.archipelago.locations[i].locationName));
        SohUtils::CopyStringToCharArray(gSaveContext.ship.quest.data.archipelago.locations[i].playerName, "",
                                        ARRAY_COUNT(gSaveContext.ship.quest.data.archipelago.locations[i].playerName));
    }
}

void RegisterArchipelago() {
    COND_HOOK(GameInteractor::OnRandomizerItemGivenHooks, IS_ARCHIPELAGO,
              [](uint32_t rc) { 
        if (rc == RC_ARCHIPELAGO_RECIEVED_ITEM) {
            gSaveContext.ship.quest.data.archipelago.lastReceivedItemIndex++;
        } else {
            ArchipelagoClient::GetInstance().CheckLocation((RandomizerCheck)rc);
        }
    });
}

static RegisterShipInitFunc initFunc(RegisterArchipelago, { "IS_ARCHIPELAGO" });
