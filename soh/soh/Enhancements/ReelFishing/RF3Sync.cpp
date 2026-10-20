// Reel Fishing III mod (step 56b, docs/DESIGN.md "Step 56 design"): RF3 catches shared in SoH's co-op (Anchor).
// Link's own catch is sent as an RF3_CATCH packet (soh/Network/Anchor/Packets/RF3Catch.cpp); a teammate's is applied
// to this save's RF3 progress (RF3Stock_ApplyRemoteCatch). RF3 randomizer seeds only (user's choice, 2026-10-18).
// Log, don't look: logs/rf3_records.txt gets "rf3-sync send ..." / "rf3-sync recv from=<client> ..." per catch, and
// "rf3-sync skip why=..." when a catch isn't sent.

#include "RF3Sync.h"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <type_traits>
#include <vector>

#include <ship/Context.h>

#include "soh/Network/Anchor/Anchor.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"
#include "RF3Bridge.h"
#include "RF3RandoChecks.h"

extern "C" PlayState* gPlayState;

namespace {

void LogSync(const char* what, const rf3::UnlockCatch& k, const std::string& extra) {
    if (!RF3_IsEnabled()) {
        return;
    }
    std::string dir = Ship::Context::GetPathRelativeToAppDirectory("logs");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(dir + "/rf3_records.txt", std::ios::app);
    char stamp[32];
    std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    char line[256];
    std::snprintf(line, sizeof(line), "%s rf3-sync %s%s species=%d class=%c mm=%.1f g=%d rod=%d lure=%d fly=%d bait=%d scene=%d\n",
                  stamp, what, extra.c_str(), k.species, k.sizeClass, k.lengthMm, k.weightG, k.rod, k.lure, k.fly,
                  k.bait, k.scene);
    out << line;
}

// Step 56c: a team state that came before gameplay started (Anchor can answer between file select and the first
// scene): merged at the next scene start, before any teammate catch that arrives meanwhile.
bool sPendingState = false;
nlohmann::json sPending;

void LogLine(const std::string& text) {
    if (!RF3_IsEnabled()) {
        return;
    }
    std::string dir = Ship::Context::GetPathRelativeToAppDirectory("logs");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(dir + "/rf3_records.txt", std::ios::app);
    char stamp[32];
    std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    out << stamp << " " << text << "\n";
}

void MergeNow(const nlohmann::json& j) {
    if (!RF3Rando_Active()) {
        LogLine("rf3-sync team-state ignore why=not-an-rf3-seed");
        return;
    }
    if (!j.is_object() || !j.contains("unlock") || !j.contains("rod")) {
        LogLine("rf3-sync team-state ignore why=no-rf3-data");
        return;
    }
    const uint32_t catches = j.value("catches", 0u);
    rf3::RodProgress rod;
    const nlohmann::json& r = j.at("rod");
    rod.total = r.value("total", static_cast<uint16_t>(0));
    rod.bass = r.value("bass", static_cast<uint16_t>(0));
    rod.trout = r.value("trout", static_cast<uint16_t>(0));
    rod.salmon = r.value("salmon", static_cast<uint16_t>(0));
    rod.withFlyRod = r.value("withFlyRod", static_cast<uint16_t>(0));
    rod.withPoleS = r.value("withPoleS", static_cast<uint16_t>(0));
    rod.largemouthOver14in = r.value("lmb14", false);
    rod.paleChub = r.value("chub", false);
    rod.bitterlingOver3in = r.value("bitt3", false);
    rod.bitterling = r.value("bitt", false);
    rf3::UnlockProgress unlock;
    const nlohmann::json& u = j.at("unlock");
    auto fill = [&](const char* key, auto* arr) {
        if (!u.contains(key) || !u.at(key).is_array()) {
            return;
        }
        const nlohmann::json& a = u.at(key);
        for (size_t i = 0; i < a.size() && i < static_cast<size_t>(rf3::kUnlockSlots); i++) {
            arr[i] = a.at(i).get<std::remove_reference_t<decltype(arr[0])>>();
        }
    };
    fill("count", unlock.count);
    fill("setLo", unlock.setLo);
    fill("setHi", unlock.setHi);
    fill("sumG", unlock.sumG);
    LogLine("rf3-sync team-state merge catches=" + std::to_string(catches) + " trout=" + std::to_string(rod.trout));
    RF3Stock_MergeTeamProgress(catches, rod, unlock);
}

void ApplyPending() {
    if (!sPendingState) {
        return;
    }
    sPendingState = false;
    nlohmann::json j = std::move(sPending);
    sPending = nlohmann::json();
    MergeNow(j);
}

void RegisterRF3Sync() {
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnSceneInit>([](int16_t) { ApplyPending(); });
}

} // namespace

static RegisterShipInitFunc initRF3Sync(RegisterRF3Sync);

nlohmann::json RF3Sync_ExportTeamState() {
    nlohmann::json j = nlohmann::json::object();
    if (!RF3_IsEnabled() || !RF3Rando_Active()) {
        return j;
    }
    uint32_t catches = 0;
    rf3::RodProgress rod;
    rf3::UnlockProgress unlock;
    RF3Stock_GetTeamProgress(&catches, &rod, &unlock);
    j["catches"] = catches;
    j["rod"] = { { "total", rod.total },           { "bass", rod.bass },
                 { "trout", rod.trout },           { "salmon", rod.salmon },
                 { "withFlyRod", rod.withFlyRod }, { "withPoleS", rod.withPoleS },
                 { "lmb14", rod.largemouthOver14in }, { "chub", rod.paleChub },
                 { "bitt3", rod.bitterlingOver3in }, { "bitt", rod.bitterling } };
    j["unlock"]["count"] = std::vector<uint16_t>(unlock.count, unlock.count + rf3::kUnlockSlots);
    j["unlock"]["setLo"] = std::vector<uint64_t>(unlock.setLo, unlock.setLo + rf3::kUnlockSlots);
    j["unlock"]["setHi"] = std::vector<uint64_t>(unlock.setHi, unlock.setHi + rf3::kUnlockSlots);
    j["unlock"]["sumG"] = std::vector<uint32_t>(unlock.sumG, unlock.sumG + rf3::kUnlockSlots);
    LogLine("rf3-sync team-state send catches=" + std::to_string(catches) + " trout=" + std::to_string(rod.trout));
    return j;
}

void RF3Sync_ReceiveTeamState(const nlohmann::json& rf3State) {
    if (gPlayState == nullptr) { // gameplay not started: merge at the next scene start
        sPending = rf3State;
        sPendingState = true;
        LogLine("rf3-sync team-state pending");
        return;
    }
    sPendingState = false;
    MergeNow(rf3State);
}

void RF3Sync_SendCatch(const rf3::UnlockCatch& k) {
    Anchor* anchor = Anchor::Instance;
    if (anchor == nullptr || !anchor->isConnected) {
        return; // not in co-op: nothing to log
    }
    if (!RF3Rando_Active()) {
        LogSync("skip", k, " why=not-an-rf3-seed");
        return;
    }
    if (!anchor->roomState.syncItemsAndFlags) {
        LogSync("skip", k, " why=room-sync-off");
        return;
    }
    anchor->SendPacket_RF3Catch(k);
    LogSync("send", k, "");
}

void RF3Sync_ReceiveCatch(const rf3::UnlockCatch& k, uint32_t fromClientId) {
    if (!RF3Rando_Active()) {
        LogSync("ignore", k, " from=" + std::to_string(fromClientId) + " why=not-an-rf3-seed");
        return;
    }
    ApplyPending(); // step 56c: a pending team state first, so this catch adds to the merged progress
    LogSync("recv", k, " from=" + std::to_string(fromClientId));
    RF3Stock_ApplyRemoteCatch(k);
}
