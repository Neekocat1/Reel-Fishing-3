#include <nlohmann/json.hpp>

#include "soh/Network/Anchor/Anchor.h"
#include "soh/Enhancements/ReelFishing/RF3FishView.h"

extern "C" {
#include "variables.h"
extern PlayState* gPlayState;
}

/**
 * RF3_FISH_VIEW (Reel Fishing III mod, step 61c; docs/DESIGN.md "Step 61 design" in the mod's project)
 *
 * The angler's rod, line, lure and hooked fish as drawn this frame (RF3FishView.cpp's view JSON), so the other
 * players in the same scene can draw a copy at the angler's dummy (view only: nothing is simulated there). Sent every
 * game frame while the rod is out, like PLAYER_UPDATE; a null view ends it (step 61d: sent to the scene the view was
 * in). Not queued for offline players.
 */

uint32_t Anchor::SendPacket_RF3FishView(const nlohmann::json* view, s16 sceneNum) {
    if (!IsSaveLoaded()) {
        return 0;
    }

    nlohmann::json payload;
    payload["type"] = RF3_FISH_VIEW;
    payload["sceneNum"] = sceneNum;
    payload["view"] = (view != nullptr) ? *view : nlohmann::json(nullptr);
    payload["quiet"] = true;

    // Step 61d: the players in `sceneNum` (the scene the view was seen in: a stop after a scene change still reaches
    // the old scene's players).
    uint32_t sent = 0;
    for (auto& [clientId, client] : clients) {
        if (client.sceneNum == sceneNum && client.online && client.isSaveLoaded && !client.self) {
            payload["targetClientId"] = clientId;
            SendJsonToRemote(payload);
            sent++;
        }
    }
    return sent;
}

void Anchor::HandlePacket_RF3FishView(nlohmann::json payload) {
    if (!payload.contains("clientId")) {
        return;
    }
    const uint32_t clientId = payload["clientId"].get<uint32_t>();
    const nlohmann::json view = payload.contains("view") ? payload["view"] : nlohmann::json(nullptr);
    RF3FishView_Receive(clientId, payload.value("sceneNum", -1), view);
}
