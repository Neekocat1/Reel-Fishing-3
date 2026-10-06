#include "RF3Log.h"

#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

#include <libultraship/bridge/consolevariablebridge.h>
#include <ship/Context.h>

#include "soh/cvar_prefixes.h"
#include "RF3Bridge.h"

// One CSV file per logging session. A session starts on the first frame logged
// after the "Log Fishing Data (CSV)" option is switched on, and ends when it is
// switched off.
static std::ofstream sFile;
static bool sOpen = false;
static int sRowsSinceFlush = 0;
static int32_t sPrevCastState = 0;

static const int kFlushEveryRows = 60;

static void CloseLog() {
    if (sOpen) {
        sFile.flush();
        sFile.close();
        sOpen = false;
    }
    sRowsSinceFlush = 0;
    sPrevCastState = 0;
}

static bool OpenLog() {
    std::string dir = Ship::Context::GetPathRelativeToAppDirectory("logs");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    char stamp[32];
    std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", std::localtime(&now));
    std::string path = dir + "/rf3_fishing_" + stamp + ".csv";

    sFile.open(path, std::ios::out | std::ios::trunc);
    if (!sFile.is_open()) {
        return false;
    }
    sFile << "frame,castState,lureEquipped,lineHooked,reelLock,rodPullback,reelSpeed,lineSpooled,"
             "lureX,lureY,lureZ,rodTipX,rodTipY,rodTipZ,waterY,stickX,stickY,buttons,"
             "hasFish,fishState,fishStateNext,fishLength,fishX,fishY,fishZ,fishSpeedXZ,"
             "rf3Enabled,rf3Active,rf3Mode,rf3LastRun,rf3Hp,rf3Hook,rf3Tension,rf3Limit,rf3DistanceM,rf3Outcome,"
             "linkX,linkY,linkZ,fishFloorY,rf3Meter,rf3Chaser,rf3ChaserMemory,rf3ApproachMode,"
             "rf3CastPower,rf3CastFactor,rf3LengthMm,rf3WeightG,fishYaw,fishTailStep,fishTailMag\n";
    sOpen = true;
    sRowsSinceFlush = 0;
    return true;
}

extern "C" int RF3Log_IsEnabled(void) {
    bool enabled = CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.Log"), 0) != 0;
    if (!enabled) {
        CloseLog();
    }
    return enabled ? 1 : 0;
}

extern "C" void RF3Log_OwnerFrame(const RF3OwnerFrame* f) {
    // Log while the line is out, plus the frame it comes back in.
    bool wanted = (f->castState != 0) || (sPrevCastState != 0);
    sPrevCastState = f->castState;
    if (!wanted) {
        return;
    }
    if (!sOpen && !OpenLog()) {
        return;
    }

    sFile << f->frame << ',' << f->castState << ',' << f->lureEquipped << ',' << f->lineHooked << ','
          << f->reelLock << ',' << f->rodPullback << ',' << f->reelSpeed << ',' << f->lineSpooled << ','
          << f->lurePos[0] << ',' << f->lurePos[1] << ',' << f->lurePos[2] << ',' << f->rodTipPos[0] << ','
          << f->rodTipPos[1] << ',' << f->rodTipPos[2] << ',' << f->waterSurfaceY << ',' << f->stickX << ','
          << f->stickY << ',' << f->buttons << ',' << f->hasFish << ',' << f->fishState << ','
          << f->fishStateNext << ',' << f->fishLength << ',' << f->fishPos[0] << ',' << f->fishPos[1] << ','
          << f->fishPos[2] << ',' << f->fishSpeedXZ << ',';

    // Reel Fishing III fight (step 8). After a fight ends, rf3Active is 0 and the
    // other columns keep that fight's final values.
    RF3FightStatus fight;
    RF3Fight_GetStatus(&fight);
    sFile << RF3_IsEnabled() << ',' << fight.active << ',' << fight.mode << ',' << fight.lastRun << ','
          << fight.hp << ',' << fight.hook << ',' << fight.tension << ',' << fight.tensionLimit << ','
          << fight.distanceM << ',' << fight.outcome << ',' << f->linkPos[0] << ',' << f->linkPos[1] << ','
          << f->linkPos[2] << ',' << f->fishFloorY << ',';

    // Approach (step 10).
    RF3ApproachStatus approach;
    RF3Approach_GetStatus(&approach);
    float castPower, castFactor;
    RF3Cast_GetLast(&castPower, &castFactor);
    sFile << approach.meter << ',' << approach.chaser << ',' << approach.chaserMemory << ','
          << approach.chaserMode << ',' << castPower << ',' << castFactor << ',' << fight.lengthMm << ','
          << fight.weightG << ',' << f->fishYaw << ',' << f->fishTailStep << ',' << f->fishTailMag << '\n';

    if (++sRowsSinceFlush >= kFlushEveryRows) {
        sFile.flush();
        sRowsSinceFlush = 0;
    }
}
