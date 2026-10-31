#include <nlohmann/json.hpp>

#include "soh/Network/Anchor/Anchor.h"
#include "soh/Enhancements/ReelFishing/RF3Sync.h"

/**
 * RF3_CATCH (Reel Fishing III mod, step 56b; docs/DESIGN.md "Step 56 design" in the mod's project)
 *
 * One RF3 catch, so teammates' RF3 progress (catch count, rod and unlock conditions) adds up. Sent after each local
 * catch in an RF3 randomizer seed; queued for offline teammates. The receiver applies it as a teammate's catch
 * (RF3Stock_ApplyRemoteCatch): it counts toward every goal except streaks and visits and never gives items itself.
 */

void Anchor::SendPacket_RF3Catch(const rf3::UnlockCatch& k) {
    if (!IsSaveLoaded() || isProcessingIncomingPacket || !roomState.syncItemsAndFlags) {
        return;
    }

    nlohmann::json c;
    c["record"] = k.record; // step 74
    c["species"] = k.species;
    c["sizeClass"] = static_cast<int>(k.sizeClass);
    c["lengthMm"] = k.lengthMm;
    c["weightG"] = k.weightG;
    c["rod"] = k.rod;
    c["lure"] = k.lure;
    c["fly"] = k.fly;
    c["flyType"] = k.flyType;
    c["bait"] = k.bait;
    c["floatIdx"] = k.floatIdx;
    c["lureType"] = k.lureType;
    c["scene"] = k.scene;
    c["pond"] = k.pond;
    c["dayHours"] = k.dayHours;
    c["lineCell"] = k.lineCell;
    c["biteSeconds"] = k.biteSeconds;
    c["landSeconds"] = k.landSeconds;

    nlohmann::json payload;
    payload["type"] = RF3_CATCH;
    payload["targetTeamId"] = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
    payload["addToQueue"] = true;
    payload["catch"] = c;

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_RF3Catch(nlohmann::json payload) {
    if (!IsSaveLoaded() || !roomState.syncItemsAndFlags || !payload.contains("catch")) {
        return;
    }

    const nlohmann::json& c = payload.at("catch");
    rf3::UnlockCatch k;
    k.record = c.value("record", -1); // step 74; -1 from an older build: it counts toward no record filter
    k.species = c.value("species", -1);
    k.sizeClass = static_cast<char>(c.value("sizeClass", static_cast<int>('n')));
    k.lengthMm = c.value("lengthMm", 0.0f);
    k.weightG = c.value("weightG", 0);
    k.rod = c.value("rod", -1);
    k.lure = c.value("lure", -1);
    k.fly = c.value("fly", -1);
    k.flyType = c.value("flyType", -1);
    k.bait = c.value("bait", -1);
    k.floatIdx = c.value("floatIdx", -1);
    k.lureType = c.value("lureType", -1);
    k.scene = c.value("scene", -1);
    k.pond = c.value("pond", false);
    k.dayHours = c.value("dayHours", 12.0f);
    k.lineCell = c.value("lineCell", -1);
    k.biteSeconds = c.value("biteSeconds", -1.0f);
    k.landSeconds = c.value("landSeconds", -1.0f);

    uint32_t from = payload.contains("clientId") ? payload["clientId"].get<uint32_t>() : 0;
    RF3Sync_ReceiveCatch(k, from);
}
