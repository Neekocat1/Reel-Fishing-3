// Reel Fishing III mod (step 16c): fish regions, loaded from and saved to rf3_fish_regions.json.
// See RF3Regions.h and docs/DESIGN.md step 16.

#include "RF3Regions.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>
#include <ship/Context.h>

#include "RF3FishTable.h"

namespace rf3 {

namespace {

constexpr int kFileVersion = 1;
std::vector<FishRegion> sRegions;

const FishRecord* FindRecord(int record) {
    for (const FishRecord& r : kFishRecords) {
        if (r.record == record) {
            return &r;
        }
    }
    return nullptr;
}

std::string RecordLabel(int record) {
    const FishRecord* r = FindRecord(record);
    return r ? (std::string(r->speciesName) + " " + r->sizeClass) : "?";
}

} // namespace

int& SelectedRegion() {
    static int sSelected = -1;
    return sSelected;
}

bool& RegionsDirty() {
    static bool sDirty = false;
    return sDirty;
}

std::vector<FishRegion>& FishRegions() {
    return sRegions;
}

std::string FishRegionsPath() {
    // __FILE__ is this source file's absolute path in MSVC builds: the fork's tracked copy sits next to
    // it. A build moved away from the source tree uses a copy next to soh.exe instead.
    std::filesystem::path source(__FILE__);
    std::error_code ec;
    if (source.is_absolute() && std::filesystem::is_directory(source.parent_path(), ec)) {
        return (source.parent_path() / "rf3_fish_regions.json").string();
    }
    return Ship::Context::GetPathRelativeToAppDirectory("rf3_fish_regions.json");
}

bool IsRegionRecord(int record) {
    const FishRecord* r = FindRecord(record);
    return (r != nullptr) && (r->species >= 0) && (r->species <= 21) &&
           ((r->sizeClass == 's') || (r->sizeClass == 'n') || (r->sizeClass == 'b'));
}

bool LoadFishRegions(std::string* error) {
    std::vector<FishRegion> loaded;
    std::ifstream in(FishRegionsPath());
    if (!in.is_open()) {
        sRegions.clear(); // no file yet: no regions
        return true;
    }
    try {
        nlohmann::json root = nlohmann::json::parse(in);
        for (const nlohmann::json& j : root.at("regions")) {
            FishRegion r;
            r.name = j.value("name", std::string());
            r.scene = j.at("scene").get<int>();
            float x0 = j.at("minX").get<float>();
            float x1 = j.at("maxX").get<float>();
            float z0 = j.at("minZ").get<float>();
            float z1 = j.at("maxZ").get<float>();
            r.minX = std::min(x0, x1);
            r.maxX = std::max(x0, x1);
            r.minZ = std::min(z0, z1);
            r.maxZ = std::max(z0, z1);
            r.surfaceY = j.at("surfaceY").get<float>();
            r.angle = j.value("angle", 0.0f); // step 16i; absent in older files = not turned
            r.fishCount = std::clamp(j.value("fishCount", 6), 0, 16);
            for (const nlohmann::json& rec : j.value("records", nlohmann::json::array())) {
                int index = rec.is_object() ? rec.at("record").get<int>() : rec.get<int>();
                if (IsRegionRecord(index) && (std::find(r.records.begin(), r.records.end(), index) == r.records.end())) {
                    r.records.push_back(index);
                }
            }
            loaded.push_back(r);
        }
    } catch (const std::exception& e) {
        if (error != nullptr) {
            *error = e.what();
        }
        return false; // keep what was loaded before
    }
    sRegions = loaded;
    return true;
}

bool SaveFishRegions(std::string* error) {
    nlohmann::json root;
    root["version"] = kFileVersion;
    root["regions"] = nlohmann::json::array();
    for (const FishRegion& r : sRegions) {
        nlohmann::json j;
        j["name"] = r.name;
        j["scene"] = r.scene;
        j["minX"] = std::min(r.minX, r.maxX);
        j["maxX"] = std::max(r.minX, r.maxX);
        j["minZ"] = std::min(r.minZ, r.maxZ);
        j["maxZ"] = std::max(r.minZ, r.maxZ);
        j["surfaceY"] = r.surfaceY;
        j["angle"] = r.angle; // step 16i
        j["fishCount"] = r.fishCount;
        j["records"] = nlohmann::json::array();
        for (int record : r.records) {
            j["records"].push_back({ { "record", record }, { "name", RecordLabel(record) } });
        }
        root["regions"].push_back(j);
    }
    std::ofstream out(FishRegionsPath(), std::ios::trunc);
    if (!out.is_open()) {
        if (error != nullptr) {
            *error = "can't write " + FishRegionsPath();
        }
        return false;
    }
    out << root.dump(2) << '\n';
    return true;
}

} // namespace rf3
