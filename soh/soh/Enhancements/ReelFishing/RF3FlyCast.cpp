#include "RF3Bridge.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

#include <libultraship/bridge/consolevariablebridge.h>
#include <ship/Context.h>
#include <ship/window/Window.h>
#include <ship/window/gui/Gui.h>

#include "soh/cvar_prefixes.h"
#include "RF3Sim.h"
#include "RF3TackleAssets.h"

// Step 20c-3c/d: RF3's fly cast (RF3-MECHANICS 2.9, 3.1). While the rod button is held with a fly box the throw
// waits and Link false-casts (z_player.c swings the throw animation between two frames); on release the fly is
// cast with a distance from the hold time. See docs/DESIGN.md "20c-3 plan" in the project repository.

namespace {

bool sCharging = false;
bool sNoted = false;
float sLastFlyFactor = 0.0f;

struct Tuning {
    const char* cvar;
    float def;
    float lo;
    float hi;
};
// Frames of gPlayerAnim_link_fishing_throw. OoT launches the lure 19 game frames into the throw, i.e. at about frame
// 28.5 (1.5 animation frames per game frame). Defaults are the user's values from the debug window (2026-10-19:
// back 4, forward 14, swing speed 0.80, release 28.50); 20c-3d's first guesses were 6 / 20 / 1.0 / 28.5.
const Tuning kTuning[4] = {
    { CVAR_ENHANCEMENT("ReelFishing3.FlyCast.BackFrame"), 4.0f, 0.0f, 60.0f },
    { CVAR_ENHANCEMENT("ReelFishing3.FlyCast.ForwardFrame"), 14.0f, 0.0f, 60.0f },
    { CVAR_ENHANCEMENT("ReelFishing3.FlyCast.SwingSpeed"), 0.8f, 0.1f, 4.0f },
    { CVAR_ENHANCEMENT("ReelFishing3.FlyCast.ReleaseFrame"), 28.5f, 0.0f, 60.0f },
};

void LogLine(const char* line) {
    std::string dir = Ship::Context::GetPathRelativeToAppDirectory("logs");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(dir + "/rf3_records.txt", std::ios::app);
    char stamp[32];
    std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    out << stamp << ' ' << line;
}

} // namespace

extern "C" int32_t RF3FlyCast_IsFlyBox(void) {
    RF3TackleChoice c;
    RF3Tackle_Get(&c);
    const rf3tackle::Assets& a = rf3tackle::Get();
    return (a.loaded && c.box >= 0 && c.box < static_cast<int32_t>(a.boxes.size()) && a.boxes[c.box].kind == "fly")
               ? 1
               : 0;
}

extern "C" void RF3FlyCast_SetCharging(int32_t charging) {
    if (charging && !sCharging) {
        sNoted = false;
    }
    sCharging = charging != 0;
}

extern "C" int32_t RF3FlyCast_IsCharging(void) {
    return sCharging ? 1 : 0;
}

extern "C" float RF3FlyCast_Tuning(int32_t which) {
    if (which < 0 || which > 3) {
        return 0.0f;
    }
    const Tuning& t = kTuning[which];
    return std::clamp(CVarGetFloat(t.cvar, t.def), t.lo, t.hi);
}

extern "C" void RF3FlyCast_SetTuning(int32_t which, float value) {
    if (which < 0 || which > 3) {
        return;
    }
    CVarSetFloat(kTuning[which].cvar, std::clamp(value, kTuning[which].lo, kTuning[which].hi));
    Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
}

extern "C" void RF3FlyCast_NoteAnim(float animLength, float curFrame) {
    if (sNoted) {
        return;
    }
    sNoted = true;
    char line[200];
    std::snprintf(line, sizeof(line),
                  "fly-cast-anim throwLength=%.1f frameAtCharge=%.1f back=%.1f forward=%.1f speed=%.2f release=%.1f\n",
                  animLength, curFrame, RF3FlyCast_Tuning(0), RF3FlyCast_Tuning(1), RF3FlyCast_Tuning(2),
                  RF3FlyCast_Tuning(3));
    LogLine(line);
}

extern "C" float RF3Cast_LaunchFly(int32_t heldFrames, int32_t updateRate) {
    float held = heldFrames * (std::clamp(static_cast<int>(updateRate), 1, 6) / 60.0f);
    sLastFlyFactor = rf3::FlyCastDistanceFactor(held);
    char line[128];
    std::snprintf(line, sizeof(line), "fly-cast held=%.2fs factor=%.3f (of a full 11.9 m fly cast)\n", held,
                  sLastFlyFactor);
    LogLine(line);
    return sLastFlyFactor;
}
