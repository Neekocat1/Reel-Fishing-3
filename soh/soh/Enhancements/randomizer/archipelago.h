#pragma once
#include "archipelago_settings_window.h"

#include "randomizerTypes.h"
#include "static_data.h"
#include <vector>

// forward declerations
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

        static ArchipelagoClient& getInstance();

        bool start_client();
        bool stop_client();

        void start_location_scouts();

        // getters
        const std::string& get_slot_name() const;

        char* get_server_address_buff();
        char* get_slot_name_buff();
        char* get_password_buff();
        const std::map<std::string, int>& get_slot_data();
        const std::vector<ApItem>& get_scouted_items();

        bool isConnected();
        void check_location(RandomizerCheck SoH_check_id);

        // callback slots
        void addItemRecievedCallback(std::function<void(const std::string&)> callback);
        void removeItemRecievedCallback(std::function<void(const std::string&)> old_callback);


        // todo move me back down when done testing
        void on_item_recieved(int64_t recieved_item_id, bool notify_player);

        void send_game_won();

        void poll();

        std::unique_ptr<APClient> apclient;

    protected:
        ArchipelagoClient();

    private:
        ArchipelagoClient(ArchipelagoClient &) = delete;
        void operator=(const ArchipelagoClient &) = delete;
        std::string uuid;

        static std::shared_ptr<ArchipelagoClient> instance; // is this even used?
        static bool initialized;

        char server_address[AP_Client_consts::MAX_ADDRESS_LENGTH];
        char slot_name[AP_Client_consts::MAX_PLAYER_NAME_LENGHT];
        char password[AP_Client_consts::MAX_PLAYER_NAME_LENGHT];

        bool game_won;

        std::map<std::string, int> slot_data;
        std::set<int64_t> locations;
        std::vector<ApItem> scouted_items;
        
        void save_data();

        // callback functions
        void on_connected();
        
        void on_location_checked(int64_t location_id);
        void on_deathlink_recieved() { }; // TODO: implement me

        // callbacks
        std::function<void(const std::string&)> ItemRecievedCallback;
        
};

