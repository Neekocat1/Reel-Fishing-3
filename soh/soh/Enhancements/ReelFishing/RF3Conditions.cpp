// Reel Fishing III mod (step 57c-3, docs/DESIGN.md "Step 57 design"; RF3-MECHANICS section 7): RF3's season,
// weather and time of day from OoT's state, by the user's 57b choices:
// - season: OoT's day count (gSaveContext.totalDays), 8 days per season;
// - time: OoT's clock (gSaveContext.dayTime), night counts as Evening;
// - weather: one roll per OoT day, scene and time of day, weighted by RF3's own counts (tackle.json "conditions"),
//   or OoT's own weather (gWeatherMode 1-5) as Cloudy.
// They feed the fish's activity level (sight range) through RF3Bridge.cpp.
// Log, don't look: logs/rf3_records.txt gets "conditions season=... weather=... time=... day=... hours=... scene=...
// oot_weather=... weights=rf3|fallback" whenever one of them, the day or the scene changes.

#include "RF3Conditions.h"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

#include <ship/Context.h>

#include "RF3Bridge.h"
#include "RF3Sim.h"
#include "RF3TackleAssets.h"
#include "RF3DevTools.h"

extern "C" {
#include "z64.h"
#include "variables.h" // gSaveContext, gWeatherMode
extern PlayState* gPlayState;
}

namespace {

const char* const kSeasonNames[3] = { "Spring", "Summer", "Autumn" };
const char* const kWeatherNames[5] = { "Sunny", "Cloudy", "Fog", "Dense fog", "Rain" }; // step 59a: Rain
const char* const kTimeNames[3] = { "Morning", "Afternoon", "Evening" };

struct Inputs {
    int days = 0;
    float hours = 0.0f;
    int scene = -1;
    int ootWeather = 0;
    bool rf3Weights = false;
};

Inputs sLastInputs;
rf3::Conditions sLast = { -1, -1, -1 };

// Step 59c (user, 2026-10-19: "The song of storms should set the rf3 weather to rain"): OoT's Song of Storms (its
// storm actor sets envCtx.unk_F2[0], the rain request) makes it Rain in that scene until the RF3 time of day changes
// (the next weather roll), or the day.
struct Storm {
    bool active = false;
    int days = 0;
    int time = 0;
    int scene = -1;
};
Storm sStorm;

rf3::WeatherWeights Weights() {
    rf3::WeatherWeights w;
    const rf3tackle::Assets& a = rf3tackle::Get();
    if (a.loaded && a.weatherCountsOk) {
        for (int s = 0; s < 3; s++) {
            for (int t = 0; t < 3; t++) {
                for (int k = 0; k < 4; k++) {
                    w.counts[s][t][k] = a.weatherCounts[s][t][k];
                }
            }
        }
        w.valid = true;
    }
    return w;
}

void Log(const rf3::Conditions& c, const Inputs& in, bool storm) {
    if (!RF3_IsEnabled()) {
        return;
    }
    std::string dir = Ship::Context::GetPathRelativeToAppDirectory("logs");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out = RF3OpenLog(dir + "/rf3_records.txt"); // step 78b: nothing while the logs are off
    char stamp[32];
    std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    char line[256];
    std::snprintf(line, sizeof(line),
                  "%s conditions season=%d(%s) weather=%d(%s) time=%d(%s) day=%d hours=%.2f scene=%d oot_weather=%d "
                  "weights=%s storm=%d\n",
                  stamp, c.season, kSeasonNames[c.season], c.weather, kWeatherNames[c.weather], c.time,
                  kTimeNames[c.time], in.days, in.hours, in.scene, in.ootWeather, in.rf3Weights ? "rf3" : "fallback",
                  storm ? 1 : 0);
    out << line;
}

} // namespace

rf3::Conditions RF3Conditions_Current() {
    if (gPlayState == nullptr) {
        return rf3::kConditionsTestSession;
    }
    Inputs in;
    in.days = gSaveContext.totalDays;
    in.hours = gSaveContext.dayTime * 24.0f / 65536.0f;
    in.scene = gPlayState->sceneNum;
    in.ootWeather = gWeatherMode;
    const rf3::WeatherWeights w = Weights();
    in.rf3Weights = w.valid;
    rf3::Conditions c = rf3::ConditionsFor(in.days, in.hours, in.scene, in.ootWeather, w);
    if (gPlayState->envCtx.unk_F2[0] != 0 &&
        !(sStorm.active && sStorm.days == in.days && sStorm.time == c.time && sStorm.scene == in.scene)) {
        sStorm = { true, in.days, c.time, in.scene }; // the Song of Storms is playing here now
    }
    const bool storm = sStorm.active && sStorm.days == in.days && sStorm.time == c.time && sStorm.scene == in.scene;
    if (storm) {
        c.weather = rf3::kWeatherRain;
    }
    if (c.season != sLast.season || c.weather != sLast.weather || c.time != sLast.time || in.days != sLastInputs.days ||
        in.scene != sLastInputs.scene || in.ootWeather != sLastInputs.ootWeather) {
        Log(c, in, storm);
        sLast = c;
        sLastInputs = in;
    }
    return c;
}

std::string RF3Conditions_Describe() {
    const rf3::Conditions c = RF3Conditions_Current();
    char buf[200];
    std::snprintf(buf, sizeof(buf), "RF3 conditions: %s, %s, %s (OoT day %d = season day %d/%d; OoT weather mode %d%s)",
                  kSeasonNames[c.season], kWeatherNames[c.weather], kTimeNames[c.time], sLastInputs.days,
                  sLastInputs.days % rf3::kDaysPerSeason + 1, rf3::kDaysPerSeason, sLastInputs.ootWeather,
                  sLastInputs.rf3Weights ? ", RF3 weights" : ", fallback weights");
    return gPlayState == nullptr ? std::string("RF3 conditions: Summer, Sunny, Morning (no game running)") : buf;
}
