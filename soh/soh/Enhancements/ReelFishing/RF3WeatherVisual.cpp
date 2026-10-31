// Reel Fishing III mod (step 59b, docs/DESIGN.md "Step 59 design"): RF3's weather (RF3Conditions_Current) shown in
// the world, outdoors only, with OoT's own weather winning. Built from OoT's own pieces (z_kankyo.c):
// - Cloudy and Rain: OoT's gloomy sky (sky blend 1, light preset 2, as its weather modes 2-4 and the Song of
//   Storms). At scene start it is set directly; inside a scene it fades with OoT's own sky fade (unk_19/unk_1A, the
//   one func_8006FB94 starts), started by us (step 59d) so it also works while the sky blends between times of day.
// - Rain: envCtx.unk_EE[0] = 20 drops (the Song of Storms' amount); OoT's own ramp eases them in and out. The rain
//   sound channel is switched on only where the scene already plays nature ambience: music is never replaced
//   [assumed choice, DESIGN].
// - Fog / Dense fog (the user's "thicker + greyer"): fog near pulled in and the fog colour moved toward grey, over
//   about 2 s. Step 59d: added to OoT's drawn fog (lightCtx, recomputed by Environment_Update every frame) just
//   before the frame's fog is set, so OoT's own writers of envCtx.adjFog* (the Sun's Song light, warps) can't
//   erase it.
// Outdoors = not an indoor scene (envCtx.indoors) and some skybox, not the fishing pond (its own storm). OoT's
// weather wins: a scene that starts with gWeatherMode set, rain falling or a gloomy sky gets nothing from us.
// Log, don't look: logs/rf3_records.txt gets "weather-visual ..." at each scene start, each change and each sky fade.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

#include <ship/Context.h>

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"
#include "RF3Bridge.h"
#include "RF3Conditions.h"
#include "RF3Sim.h"
#include "RF3DevTools.h"

extern "C" {
#include "z64.h"
#include "functions.h"
#include "variables.h" // gWeatherMode, gSkyboxBlendingEnabled, D_8011FB34
extern PlayState* gPlayState;
}

namespace {

constexpr uint8_t kRainDrops = 20;     // the Song of Storms' amount (EnOkarinaEffect: unk_F2[0] = 20)
// Step 59c (user, 2026-10-19: "Fog and heavy fog are too foggy and hard to see in"): about a quarter of 59b's
// -200 / -400 and 35% / 55%. User after 59c: "The fog levels look good now."
constexpr float kFogNear[2] = { -50.0f, -110.0f }; // Fog, Dense fog
constexpr float kFogGrey[2] = { 0.15f, 0.30f };    // share of the way to grey
constexpr float kGrey = 150.0f;
constexpr float kFogStep = 1.0f / 40.0f;             // about 2 s at 20 Hz
constexpr int16_t kSkyFadeFrames = 100;              // OoT's own gloomy fade length (func_8006FB94)

struct State {
    bool eligible = false;   // outdoors and no OoT weather at scene start
    const char* why = "-";   // why not, for the log
    bool ownsGloomy = false; // we made the sky gloomy (or took it over from the Song of Storms)
    bool ownsRain = false;   // we set the rain target
    bool rainSound = false;  // we switched the rain channel on
    bool sawStorm = false;   // the Song of Storms was running last frame
    float fogAmount = 0.0f;  // our current fog amount (0..1 of the fog kind's numbers)
    int fogKind = -1;        // 0 Fog, 1 Dense fog, -1 none (what fogAmount is scaled by)
    int16_t drawnNear = 0;   // what we last added to the drawn fog (log only)
    int16_t drawnColor[3] = {};
    int lastLogged = -2;
};
State sW;

void Log(const PlayState* play, const char* what, int weather) {
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
    const EnvironmentContext& env = play->envCtx;
    char line[384];
    std::snprintf(line, sizeof(line),
                  "%s weather-visual %s scene=%d weather=%d eligible=%d why=%s indoors=%d skybox=%d oot_weather=%d "
                  "gloomy=%d->%d fade=%d mode=%d storm=%d blending=%d hours=%.2f rain=%d/%d fogNear=%+d (scene %d) "
                  "fogColor=%+d/%+d/%+d\n",
                  stamp, what, play->sceneNum, weather, sW.eligible ? 1 : 0, sW.why, env.indoors, play->skyboxId,
                  gWeatherMode, env.unk_17, env.unk_18, env.unk_19, env.gloomySkyMode, env.unk_F2[0] != 0 ? 1 : 0,
                  gSkyboxBlendingEnabled ? 1 : 0, gSaveContext.dayTime * 24.0f / 65536.0f, env.unk_EE[0],
                  env.unk_EE[1], sW.drawnNear, env.lightSettings.fogNear, sW.drawnColor[0], sW.drawnColor[1],
                  sW.drawnColor[2]);
    out << line;
}

bool WantsGloomy(int weather) {
    return weather == 1 || weather == rf3::kWeatherRain;
}

// Step 59d: starts OoT's own sky + light fade (what func_8006FB94 does for gloomySkyMode 1 / 2), but also while the
// sky blends between times of day. OoT only starts it outside a blend, and after the Sun's Song the sky sits in the
// 6:00-8:00 dawn blend (D_8011FC1C), so the clouds didn't come (user, 2026-10-19). Inside a blend the fade step
// counter is set straight to 3 (OoT counts 1 -> 3 only outside a blend, giving the skybox textures two frames to
// load; SoH loads them at once [assumed]).
void StartSkyFade(EnvironmentContext* env, bool toGloomy) {
    env->unk_19 = gSkyboxBlendingEnabled ? 3 : 1;
    env->unk_17 = toGloomy ? 0 : 1;
    env->unk_18 = toGloomy ? 1 : 0;
    env->unk_1A = kSkyFadeFrames;
    env->unk_21 = 1;
    env->unk_1F = toGloomy ? 0 : 2;
    env->unk_20 = toGloomy ? 2 : 0;
    D_8011FB34 = toGloomy ? 2 : 0;
    env->unk_22 = env->unk_24 = kSkyFadeFrames;
}

// Moves our fog amount toward the weather's numbers. Drawn in OnDrawBegin.
void UpdateFog(int weather) {
    const int kind = weather == 2 ? 0 : (weather == 3 ? 1 : -1);
    if (kind >= 0) {
        sW.fogKind = kind;
    }
    const float target = kind >= 0 ? 1.0f : 0.0f;
    sW.fogAmount =
        sW.fogAmount < target ? std::min(target, sW.fogAmount + kFogStep) : std::max(target, sW.fogAmount - kFogStep);
    if (sW.fogAmount <= 0.0f) {
        sW.fogKind = -1;
    }
}

void OnSceneStart(int16_t) {
    sW = State{};
    PlayState* play = gPlayState;
    if (play == nullptr || !RF3_IsEnabled()) {
        return;
    }
    EnvironmentContext* env = &play->envCtx;
    // Step 59c (user: no weather in Kokiri Forest; its skybox is 29, not the normal sky, and indoors is 0): outdoors =
    // not marked indoors and some skybox. Houses, shops, grottos and dungeons are marked indoors (log, 59b).
    if (env->indoors || play->skyboxId == SKYBOX_NONE) {
        sW.why = "indoors";
    } else if (play->sceneNum == SCENE_FISHING_POND) {
        sW.why = "pond";
    } else if (gWeatherMode != 0 || env->unk_EE[0] != 0 || env->unk_17 != 0 || env->gloomySkyMode != 0) {
        sW.why = "oot-weather";
    } else {
        sW.eligible = true;
    }
    const int weather = RF3Conditions_Current().weather;
    if (sW.eligible) {
        if (WantsGloomy(weather)) { // at once, as Environment_Init does for OoT's weather modes 2-4
            env->unk_17 = 1;
            env->unk_18 = 1;
            env->unk_1F = 2;
            env->unk_20 = 2;
            env->unk_DE = 1; // as a Song of Storms in a gloomy sky would find it
            sW.ownsGloomy = true;
        }
        if (weather == rf3::kWeatherRain) {
            env->unk_EE[0] = kRainDrops;
            env->unk_EE[1] = kRainDrops; // already raining when the scene appears
            sW.ownsRain = true;
        }
        if (weather == 2 || weather == 3) { // fog at full strength from the start
            sW.fogAmount = 1.0f - kFogStep;
        }
    }
    Log(play, "scene-start", weather);
    sW.lastLogged = weather;
}

void OnUpdate() {
    PlayState* play = gPlayState;
    if (play == nullptr || !sW.eligible || !RF3_IsEnabled()) {
        return;
    }
    EnvironmentContext* env = &play->envCtx;
    const int weather = RF3Conditions_Current().weather;
    // Step 59c: the fog follows the weather even while OoT owns the sky and rain (the Song of Storms makes it Rain,
    // RF3Conditions.cpp, so the fog clears).
    UpdateFog(weather);
    if (gWeatherMode != 0) {
        // OoT's story weather owns the sky and the rain until it ends.
        if (weather != sW.lastLogged) {
            Log(play, "change", weather);
            sW.lastLogged = weather;
        }
        return;
    }
    const bool storm = env->unk_F2[0] != 0;
    if (storm && env->gloomySkyMode == 1 && WantsGloomy(weather)) {
        // Step 59d (user, 2026-10-19: "The sky flicker from when the song of storms ends to when the rain begins again
        // looks bad"): the storm's end fades its sky to clear (gloomySkyMode 2), then we faded it back in. Since the
        // weather stays Rain after the storm, we take the storm's sky over: its end then finds gloomySkyMode 0 and
        // leaves the sky alone (EnOkarinaEffect_ManageStorm's else branch). If OoT hasn't started its fade yet (it
        // waits for a sky blend to end), we start it.
        if (env->unk_DE == 0 && env->unk_19 == 0 && env->unk_17 == 0) {
            StartSkyFade(env, true);
        }
        env->gloomySkyMode = 0;
        env->unk_DE = 0;
        sW.ownsGloomy = true;
        Log(play, "storm-sky", weather);
    }
    if (!storm && sW.sawStorm) {
        sW.rainSound = false; // the storm's end switched the rain channel off (Environment_StopStormNatureAmbience)
    }
    sW.sawStorm = storm;
    // The sky: fade in or out when neither OoT's gloomy machine nor a fade is running.
    if (env->gloomySkyMode == 0 && env->unk_19 == 0) {
        if (WantsGloomy(weather) && env->unk_17 == 0) {
            StartSkyFade(env, true);
            sW.ownsGloomy = true;
            Log(play, "sky-in", weather);
        } else if (!WantsGloomy(weather) && sW.ownsGloomy && env->unk_17 != 0) {
            StartSkyFade(env, false);
            sW.ownsGloomy = false;
            Log(play, "sky-out", weather);
        }
    }
    // The rain: OoT eases the drawn drops toward the larger of this and the storm's request (func_800766C4), so
    // setting it during the storm too leaves no gap when the storm ends.
    if (weather == rf3::kWeatherRain) {
        env->unk_EE[0] = kRainDrops;
        sW.ownsRain = true;
    } else if (sW.ownsRain) {
        env->unk_EE[0] = 0;
        sW.ownsRain = false;
    }
    if (!storm) { // the storm plays its own sound
        const bool natureAmbience = func_800FA0B4(SEQ_PLAYER_BGM_MAIN) == NA_BGM_NATURE_AMBIENCE;
        if (sW.ownsRain && natureAmbience && !sW.rainSound) {
            Audio_SetNatureAmbienceChannelIO(NATURE_CHANNEL_RAIN, CHANNEL_IO_PORT_1, 1);
            sW.rainSound = true;
        } else if (!sW.ownsRain && sW.rainSound) {
            Audio_SetNatureAmbienceChannelIO(NATURE_CHANNEL_RAIN, CHANNEL_IO_PORT_1, 0);
            sW.rainSound = false;
        }
    }
    if (weather != sW.lastLogged) {
        Log(play, "change", weather);
        sW.lastLogged = weather;
    }
}

// Step 59d: our fog goes on top of the drawn fog, right before Play_Draw sets it (z_play.c: OnPlayDrawBegin, then
// Play_SetFog). Environment_Update recomputes lightCtx from lightSettings + adjFog* every unpaused frame, so nothing
// builds up; while paused it isn't recomputed and still holds the last frame's (ours included), so we skip.
void OnDrawBegin() {
    PlayState* play = gPlayState;
    if (play == nullptr || !sW.eligible || !RF3_IsEnabled() || sW.fogKind < 0 || play->pauseCtx.state != 0) {
        return;
    }
    LightContext* light = &play->lightCtx;
    const int16_t nearDelta = static_cast<int16_t>(std::lround(kFogNear[sW.fogKind] * sW.fogAmount)); // near: a macro
    light->fogNear = static_cast<int16_t>(std::clamp(light->fogNear + nearDelta, 0, 996));
    sW.drawnNear = nearDelta;
    for (int i = 0; i < 3; i++) {
        const float base = light->fogColor[i];
        const int16_t c = static_cast<int16_t>(std::lround((kGrey - base) * kFogGrey[sW.fogKind] * sW.fogAmount));
        light->fogColor[i] = static_cast<u8>(std::clamp(light->fogColor[i] + c, 0, 255));
        sW.drawnColor[i] = c;
    }
}

void RegisterRF3WeatherVisual() {
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnSceneInit>(OnSceneStart);
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnPlayerUpdate>(OnUpdate);
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnPlayDrawBegin>(OnDrawBegin);
}

} // namespace

static RegisterShipInitFunc initRF3WeatherVisual(RegisterRF3WeatherVisual);
