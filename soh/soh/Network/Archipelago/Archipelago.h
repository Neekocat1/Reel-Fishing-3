#pragma once
#include "soh/Enhancements/randomizer/randomizerTypes.h"
#include "soh/Enhancements/randomizer/static_data.h"
#include <vector>

// Forward declaration
class APClient;

namespace AP_Client_consts {
    static constexpr int MAX_ADDRESS_LENGTH = 64;
    static constexpr int MAX_PLAYER_NAME_LENGHT = 17;
    static constexpr int MAX_PASSWORD_LENGTH = 32;
    static constexpr char const* DEFAULT_SERVER_NAME = "archipelago.gg:<port number>";

    static constexpr char const* SETTING_ADDRESS = "AP_server_address";
    static constexpr char const* SETTING_NAME = "AP_slot_name";

    static constexpr char const* AP_GAME_NAME = "Ocarina of Time (SoH)";
}

class ArchipelagoClient{
    public:
        struct ApItem {
            std::string itemName;
            std::string locationName;
            std::string playerName;
            unsigned int flags;
            int index;
        };

        static ArchipelagoClient& GetInstance();

        bool StartClient();
        bool StopClient();

        void StartLocationScouts();

        // getters
        const std::string& GetSlotName() const;

        char* GetServerAddressBuffer();
        char* GetSlotNameBuffer();
        char* GetPasswordBuffer();
        const char* GetConnectionStatus();
        const std::map<std::string, int>& GetSlotData();
        const std::vector<ApItem>& GetScoutedItems();

        bool IsConnected();
        void check_location(RandomizerCheck SoH_check_id);

        // callback slots
        void AddItemRecievedCallback(std::function<void(const std::string&)> callback);
        void RemoveItemRecievedCallback(std::function<void(const std::string&)> old_callback);

        // todo move me back down when done testing
        void OnItemReceived(int64_t recieved_item_id, bool notify_player);

        void SendGameWon();

        void Poll();

        std::unique_ptr<APClient> apClient;

    protected:
        ArchipelagoClient();

    private:
        ArchipelagoClient(ArchipelagoClient &) = delete;
        void operator=(const ArchipelagoClient &) = delete;
        std::string uuid;

        static std::shared_ptr<ArchipelagoClient> instance; // is this even used?
        static bool initialized;

        char serverAddress[AP_Client_consts::MAX_ADDRESS_LENGTH];
        char slotName[AP_Client_consts::MAX_PLAYER_NAME_LENGHT];
        char password[AP_Client_consts::MAX_PLAYER_NAME_LENGHT];

        bool game_won;

        std::map<std::string, int> slotData;
        std::set<int64_t> locations;
        std::vector<ApItem> scoutedItems;
        
        void SaveData();

        // callback functions
        void OnConnected();
        
        void OnLocationChecked(int64_t location_id);
        void OnDeathLinkReceived() { }; // TODO: implement me

        // callbacks
        std::function<void(const std::string&)> ItemRecievedCallback;
        
};

