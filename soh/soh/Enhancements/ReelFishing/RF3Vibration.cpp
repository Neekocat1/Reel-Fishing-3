// Reel Fishing III mod (step 62c, docs/DESIGN.md "Step 62 design", RF3-MECHANICS 8): RF3's controller vibration in
// the fight, instead of OoT's (user's choices V1 A: through OoT's rumble; V2 A: only RF3's moments; V3 A: RF3's
// tables as constants, rf3::VibPlayer).
// - RF3's moments come from the fight sim, through the bridge (RF3Vib_FightStep): the hook-set window opening
//   (pattern 0 for the window), the hook set (1, 1 s), STRUGGLE entered (4, its length), BUKKONUKI and CATCH entered
//   (3, 1 s); HOOKOVER and LINEBREAK stop it.
// - Each game frame (not while paused) the pattern steps R_UPDATE_RATE / 60 s.
// - Step 62d (user, after 62c: "weaker and choppy compared to rf3 ... In rf3, its a constant vibration"; V1 A -> B):
//   OoT's rumble can only switch the motor on and off, so the motors are driven directly, at RF3's levels:
//   SDL_GameControllerRumble on port 1's SDL gamepads, RF3's large motor -> the low-frequency (strong) motor and the
//   small one -> the high-frequency motor, scaled by the port's SDL rumble mapping percentages (the input editor's).
//   Each call lasts kDirectMs and the next frame's replaces it, so the level holds without gaps.
// - Without an SDL gamepad on port 1, the two motors are folded into one strength (OotStrength) and sent through
//   OoT's rumble as in 62c: func_800A9F6C holds it for 4 rumble ticks (a game frame is 3), then lets it fall to 0.
// Log, don't look: logs/rf3_records.txt gets "rf3-vib start|stop ..." and, while playing, one level line a second.

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

#include <SDL2/SDL.h>
#include <ship/Context.h>
#include <ship/controller/controldeck/ControlDeck.h>
#include <ship/controller/controldevice/controller/Controller.h>
#include <ship/controller/controldevice/controller/ControllerRumble.h>
#include <ship/controller/physicaldevice/ConnectedPhysicalDeviceManager.h>

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"
#include "RF3Bridge.h"
#include "RF3Sim.h"
#include "RF3Vibration.h"
#include "RF3DevTools.h"

extern "C" {
#include "z64.h"
#include "functions.h" // func_800A9F6C
#include "macros.h"    // R_UPDATE_RATE
#include "variables.h" // gGameInfo, for R_UPDATE_RATE
extern PlayState* gPlayState;
}

namespace {

constexpr uint8_t kHoldTicks = 4; // one game frame of rumble ticks (3), plus one so frames join up
constexpr uint8_t kFall = 255;    // then straight to 0
constexpr uint32_t kDirectMs = 120; // a direct rumble call lasts 2.4 game frames; each frame renews it
constexpr uint8_t kPort = 0;        // port 1, the player's controller

rf3::VibPlayer sVib;
std::string sStatus = "idle";
uint32_t sFrames = 0;
bool sDirectOn = false; // 62d: the gamepads are being driven directly (stop them when done)

// 62d: drives port 1's SDL gamepads at RF3's levels, scaled by the port's SDL rumble mapping (the largest
// percentages if there are several). Returns the gamepads driven; 0 = none (no SDL gamepad, or no SDL rumble
// mapping, which in SoH means rumble is off for it). low/high: the values sent (0-65535).
int SendDirect(int smallLevel, int largeLevel, uint16_t* low, uint16_t* high) {
    *low = *high = 0;
    auto deck = Ship::Context::GetRawInstance()->GetControlDeck();
    if (deck == nullptr || deck->GetControllerByPort(kPort) == nullptr) {
        return 0;
    }
    int lowPct = -1, highPct = -1;
    for (const auto& [id, mapping] : deck->GetControllerByPort(kPort)->GetRumble()->GetAllRumbleMappings()) {
        if (mapping->GetPhysicalDeviceType() == Ship::PhysicalDeviceType::SDLGamepad) {
            lowPct = std::max(lowPct, static_cast<int>(mapping->GetLowFrequencyIntensityPercentage()));
            highPct = std::max(highPct, static_cast<int>(mapping->GetHighFrequencyIntensityPercentage()));
        }
    }
    if (lowPct < 0) {
        return 0;
    }
    *low = static_cast<uint16_t>(std::clamp(largeLevel, 0, 255) / 255.0f * (lowPct / 100.0f) * UINT16_MAX);
    *high = static_cast<uint16_t>((smallLevel != 0 ? 1.0f : 0.0f) * (highPct / 100.0f) * UINT16_MAX);
    int pads = 0;
    for (const auto& [instance, pad] : deck->GetConnectedPhysicalDeviceManager()->GetConnectedSDLGamepadsForPort(kPort)) {
        SDL_GameControllerRumble(pad, *low, *high, kDirectMs);
        pads++;
    }
    return pads;
}

void StopDirect() {
    if (!sDirectOn) {
        return;
    }
    auto deck = Ship::Context::GetRawInstance()->GetControlDeck();
    if (deck != nullptr) {
        for (const auto& [instance, pad] :
             deck->GetConnectedPhysicalDeviceManager()->GetConnectedSDLGamepadsForPort(kPort)) {
            SDL_GameControllerRumble(pad, 0, 0, 0);
        }
    }
    sDirectOn = false;
}

const char* PatternName(int p) {
    switch (p) {
        case rf3::kVibHookWindow:
            return "hook-window";
        case rf3::kVibHookSet:
            return "hook-set";
        case rf3::kVibLanding:
            return "landing";
        case rf3::kVibStruggle:
            return "struggle";
        default:
            return "?";
    }
}

void Log(const std::string& line) {
    std::string dir = Ship::Context::GetPathRelativeToAppDirectory("logs");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out = RF3OpenLog(dir + "/rf3_records.txt"); // step 78b: nothing while the logs are off
    char stamp[32];
    std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    out << stamp << ' ' << line << '\n';
}

void Start(int pattern, float seconds, const char* why) {
    sVib.Start(pattern, seconds);
    sFrames = 0;
    char line[160];
    std::snprintf(line, sizeof(line), "rf3-vib start pattern=%d (%s) seconds=%.2f why=%s", pattern,
                  PatternName(pattern), seconds, why);
    Log(line);
}

void Stop(const char* why) {
    if (!sVib.Active()) {
        return;
    }
    char line[120];
    std::snprintf(line, sizeof(line), "rf3-vib stop pattern=%d (%s) why=%s", sVib.Pattern(), PatternName(sVib.Pattern()),
                  why);
    Log(line);
    sVib.Stop();
    StopDirect();
}

void OnFrame() {
    if (!sVib.Active()) {
        return;
    }
    if (!RF3_IsEnabled() || (gPlayState == nullptr)) {
        Stop("rf3-off");
        return;
    }
    if (gPlayState->pauseCtx.state != 0) {
        StopDirect(); // the pattern waits while paused, motors still
        return;
    }
    const int pattern = sVib.Pattern();
    int smallLevel = 0, largeLevel = 0;
    sVib.Step(R_UPDATE_RATE / 60.0f, &smallLevel, &largeLevel);
    uint16_t low = 0, high = 0;
    int strength = 0;
    const int pads = SendDirect(smallLevel, largeLevel, &low, &high);
    if (pads > 0) {
        sDirectOn = true;
    } else {
        strength = rf3::VibPlayer::OotStrength(smallLevel, largeLevel); // 62c's fallback: no SDL gamepad on port 1
        if (strength > 0) {
            func_800A9F6C(0.0f, static_cast<u8>(strength), kHoldTicks, kFall);
        }
    }
    char status[160];
    std::snprintf(status, sizeof(status), "%s small=%d large=%d via=%s low=%u high=%u pads=%d strength=%d",
                  PatternName(pattern), smallLevel, largeLevel, pads > 0 ? "sdl" : "oot", low, high, pads, strength);
    sStatus = sVib.Active() ? status : "idle";
    if ((sFrames++ % 20) == 0) { // once a second at 20 Hz
        Log(std::string("rf3-vib ") + status);
    }
    if (!sVib.Active()) {
        StopDirect();
        Log(std::string("rf3-vib done pattern=") + std::to_string(pattern) + " (" + PatternName(pattern) + ")");
    }
}

void RegisterRF3Vibration() {
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameFrameUpdate>(OnFrame);
}

} // namespace

static RegisterShipInitFunc initRF3Vibration(RegisterRF3Vibration);

extern "C" void RF3Vib_FightStep(int32_t prevMode, int32_t mode, int32_t hookSet, int32_t windowOpened,
                                 float windowSeconds, float stateSeconds) {
    if (!RF3_IsEnabled()) {
        return;
    }
    if (windowOpened) {
        Start(rf3::kVibHookWindow, windowSeconds, "hook-window-open"); // RF3 0x001A1820
    }
    if (hookSet) {
        Start(rf3::kVibHookSet, 1.0f, "hook-set"); // RF3 stops the window's, then 1 s of pattern 1
    }
    if (mode == prevMode) {
        return;
    }
    switch (static_cast<rf3::Mode>(mode)) {
        case rf3::Mode::Struggle:
            Start(rf3::kVibStruggle, stateSeconds, "struggle"); // RF3 0x001A36B0
            break;
        case rf3::Mode::Bukkonuki:
            Start(rf3::kVibLanding, 1.0f, "bukkonuki"); // RF3 0x001A5380
            break;
        case rf3::Mode::Catch:
            Start(rf3::kVibLanding, 1.0f, "catch"); // RF3 0x001A55C0
            break;
        case rf3::Mode::HookOver:
            Stop("hook-out"); // RF3 0x001A2530
            break;
        case rf3::Mode::LineBreak:
            Stop("line-break"); // RF3 0x001A29E0
            break;
        default:
            break;
    }
}

extern "C" void RF3Vib_FightEnded(void) {
    if (sVib.Pattern() != rf3::kVibLanding) {
        Stop("fight-ended");
    }
}

extern "C" const char* RF3Vib_Status(void) {
    return sStatus.c_str();
}
