#include "archipelago_settings_window.h"
#include "../../../../APCpp/Archipelago.h"

#include "fixed_string.hpp"

namespace AP_Client_consts {
    static constexpr int MAX_ADDRESS_LENGTH = 64;
    static constexpr int MAX_PLAYER_NAME_LENGHT = 17;
    static constexpr int MAX_PASSWORD_LENGTH = 32;
    static constexpr char const* DEFAULT_SERVER_NAME = "archipelago.gg:<port number>";

    static constexpr char const* SETTING_ADDRESS = "AP_server_address";
    static constexpr char const* SETTING_NAME = "AP_slot_name";
};

class ArchipelagoClient {
    public:
        static ArchipelagoClient& getInstance();

        bool start_client();
        bool stop_client();

        void start_location_scouts();

        // getters
        char* get_server_address_buff();
        char* get_slot_name_buff();
        char* get_password_buff();
        const std::map<std::string, int>& get_slot_data();
        const std::vector<AP_NetworkItem>& get_scouted_items();

        void add_slot_data(std::string_view key, int id);

    protected:
        ArchipelagoClient();

    private:
        ArchipelagoClient(ArchipelagoClient &) = delete;
        void operator=(const ArchipelagoClient &) = delete;
        static std::shared_ptr<ArchipelagoClient> instance;
        static bool initialized;

        char server_address[AP_Client_consts::MAX_ADDRESS_LENGTH];
        char slot_name[AP_Client_consts::MAX_PLAYER_NAME_LENGHT];
        char password[AP_Client_consts::MAX_PLAYER_NAME_LENGHT];

        std::map<std::string, int> slot_data;
        std::set<int64_t> locations;
        std::vector<AP_NetworkItem> scouted_items;

        //void registerSlotCallbacks();
        
        void save_data();

        // callback functions
        static void on_connected();
        static void on_couldntConnect(AP_ConnectionStatus connection_status);
        static void on_clear_items();
        static void on_item_recieved(int64_t recieved_item_id, bool notify_player);
        static void on_location_checked(int64_t location_id);
        static void on_deathlink_recieved() { }; // TODO: implement me
        static void on_location_scouted(std::vector<AP_NetworkItem> network_items);

        // callbacks
        
};

