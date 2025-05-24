#pragma once
#ifdef __cplusplus
#include "soh/Enhancements/randomizer/randomizerTypes.h"
#include "soh/Enhancements/randomizer/static_data.h"
#include <vector>

// Forward declaration
class APClient;

namespace AP_Client_consts {
    static constexpr int MAX_ADDRESS_LENGTH = 64;
    static constexpr int MAX_PLAYER_NAME_LENGHT = 17;
    static constexpr int MAX_PASSWORD_LENGTH = 32;

    static constexpr char const* AP_GAME_NAME = "Ship of Harkinian";
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

        void GameLoaded();
        void StartLocationScouts();
        void SynchItems();
        void SynchSentLocations();
        void SynchRecievedLocations();

        // getters
        const std::string GetSlotName() const;

        const char* GetConnectionStatus();
        const std::map<std::string, int>& GetSlotData();
        const std::vector<ApItem>& GetScoutedItems();

        bool IsConnected();
        void CheckLocation(RandomizerCheck SoH_check_id);

        // todo move me back down when done testing
        void OnItemReceived(int64_t apItemId, int64_t itemIndex);

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

        bool gameWon;

        std::map<std::string, int> slotData;
        std::set<int64_t> locations;
        std::vector<ApItem> scoutedItems;
};

void LoadArchipelagoData();
void SaveArchipelagoData(SaveContext* saveContext, int sectionID, bool fullSave);
void InitArchipelagoData(bool isDebug);
extern "C" {
#endif // END __cplusplus
void Archipelago_InitSaveFile();
#ifdef __cplusplus
}
#endif
