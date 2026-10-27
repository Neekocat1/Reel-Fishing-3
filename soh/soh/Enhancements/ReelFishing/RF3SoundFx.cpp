// Reel Fishing III mod (step 63c, docs/DESIGN.md "Step 63 design", RF3-MECHANICS 9): RF3's fishing and fight sounds,
// at the moments RF3's own code plays them, through the mod's mixer.
// - Reeling with no fish (RF3 0x0017FCF0): the reel loop of the rod's reel type (RROD +0x12) at entry 1 of RF3's tables
//   (0x00257EC0-EE0): type 1 fis 29, types 2/4 fis 33, type 3 fis 37; none for type 0 (poles). Stopped when reeling
//   stops.
// - The fight (0x00198AF0): while reeling, the reel sound by the line's load (tension / limit) in quarters (type 1:
//   28-31, 2/4: 32-35; type 3 in halves: 36/37), played only when the number changes; stopped when reeling stops.
//   Step 63c-3: never replayed (RF3's table 0x0024DF70: 31 and 35 play once, the others loop in their clips).
// - Line strain (0x00198E80): load >= 0.8: stream 65 at SPU pitch load x 2816, every 0.5 - 0.4 x load s.
// - The fish's streams (0x00198FB0, 0x00199090; RF3FishTable.h kFishSound), step 63c-3 (RF3-MECHANICS 9, "The
//   fight's sound timing"), per RF3 step: STRUGGLE and AWAY share one timer, cleared at every mode change, so each
//   plays on its first step; then AWAY's run stream every 1.0 + 0.3 x (rand & 3) s and STRUGGLE's struggle stream
//   every 0.2 + 0.1 x (rand & 3) s. STIFFEN plays the run stream once on entry unless the last run was AWAY.
// - Step 63c-2, RF3's stream volumes (RF3-MECHANICS 9): the fish's record volume (+0x74 run, +0x7C struggle) x 8191
//   out of 0x3FFF; the strain stream 0x3FFF. The mixer clamps to full, as RF3 does, and lets streams overlap.
//   Step 63c-3 left them whole (RF3 halves them with its view below the water; the mod's view is above it). Step
//   63e-4 (user: "Halve the fish's run and struggle streams always, to match my RF3 sessions."): halved always. In the
//   user's 2026-10-26 17:03 RF3 session, 17 of 19 run streams were halved.
// - The strike: fis 48. The hook set (0x001A1820): fis 42 to BUKKONUKI or hook strength > 70, 43 > 30, else 23.
// - Outcomes (0x001A1820, 0x0019E340): missed fis 20, line break 41, hook out 52.
// Channels as RF3's: 10 the reel and the outcomes, 11 the hook, 12 the bite; streams from bank 3. Only in RF3 mode.
// Log, don't look: logs/rf3_records.txt gets "rf3-sfx ..." per moment.

#include "RF3SoundFx.h"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include <ship/Context.h>

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"
#include "RF3Bridge.h"
#include "RF3FishTable.h"
#include "RF3Sim.h"
#include "RF3Sound.h"

namespace {

constexpr int kChReel = 10;
constexpr int kChHook = 11;
constexpr int kChBite = 12;
constexpr int kStreamBank = 3;
constexpr int kStrainStream = 65;
constexpr float kRF3FullVolume = 16383.0f; // 0x3FFF
constexpr float kFishHalved = 0.5f;        // step 63e-4: RF3's halving, always on
// Step 63e-6 (user: "It seems the rf3 behavior is unintuitive or possibly a bug, lower the volume of the struggle
// splashes to 0.5 instead of using the rf3 value."): RF3's struggle volume (record +0x7C) equals the record's index,
// so nearly every fish's splash hit the stream cap. A fixed 0.5 of full for every fish instead (Char s, silent in
// RF3's data, gets the splash too).
constexpr float kStruggleVolume = 0.5f;
constexpr float kStepSeconds = 1.0f / 60.0f; // one RF3 step
constexpr float kStrainLoad = 0.8f;
// RROD +0x12 per rod 0-9 [data]; rod 10 = OoT's pole, a spinning reel [assumed].
constexpr int kReelType[11] = { 2, 1, 1, 3, 3, 0, 0, 0, 4, 0, 1 };

uint32_t sTick = 0;
uint32_t sReeledTick = 0;
bool sIdleReel = false;  // the no-fish reel loop is playing
bool sFight = false;     // a strike or fight is running
int sFightReel = -1;     // the fight's reel sound playing on channel 10
float sStrainTimer = 0.0f;
float sStreamTimer = 0.0f; // STRUGGLE's and AWAY's shared timer (RF3 +0x20C), cleared at each mode change
int sLastMode = -1;
std::minstd_rand sRng(12345);

void Log(const std::string& line) {
    std::string dir = Ship::Context::GetPathRelativeToAppDirectory("logs");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(dir + "/rf3_records.txt", std::ios::app);
    char stamp[32];
    std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    out << stamp << ' ' << line << '\n';
}

void Play(int channel, int number, const char* why) {
    RF3Sound_Play(channel, "fis", number, 1.0f, 1.0f);
    Log("rf3-sfx " + std::string(why) + " sound=fis/" + std::to_string(number) + " channel=" + std::to_string(channel));
}

int ReelType() {
    RF3TackleChoice c;
    RF3Tackle_Get(&c);
    return (c.rod >= 0 && c.rod <= 10) ? kReelType[c.rod] : 0;
}

// RF3's random spread: the base plus (rand & 3) steps (rand is never negative).
float Spread(float base, float step) {
    return base + static_cast<int>(sRng() & 3) * step;
}

void FishStream(int record, bool struggle, int mode, const char* why) {
    const rf3::FishSound* s = rf3::FishSoundFor(record);
    if (s == nullptr) {
        return;
    }
    const int id = struggle ? s->struggleStream : s->runStream;
    const int pitch = struggle ? s->strugglePitch : s->runPitch;
    const float volume = struggle ? kStruggleVolume : s->runVolume * 8191.0f * kFishHalved / kRF3FullVolume;
    RF3Sound_PlayStreamSpu(kStreamBank, id, volume, pitch);
    char line[128];
    std::snprintf(line, sizeof(line), "rf3-sfx fish-stream %s mode=%d id=%d volume=%.2f next=%.2f", why, mode, id,
                  volume, sStreamTimer);
    Log(line);
}

void OnFrame() {
    const uint32_t tick = sTick++;
    if (tick % 20 == 0) {
        RF3Sound_LimiterReport(); // step 63e-5: once a second (20 game frames), only when it worked
    }
    if (sIdleReel && sReeledTick != tick) { // no reel call this frame
        RF3Sound_Stop(kChReel);
        sIdleReel = false;
    }
}

void RegisterRF3SoundFx() {
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameFrameUpdate>(OnFrame);
}

} // namespace

static RegisterShipInitFunc initRF3SoundFx(RegisterRF3SoundFx);

extern "C" void RF3SoundFx_Reeling(void) {
    if (!RF3_IsEnabled() || sFight) {
        return; // the fight has its own reel sounds
    }
    sReeledTick = sTick;
    if (sIdleReel) {
        return; // the loop keeps playing
    }
    const int type = ReelType();
    const int sound = type == 1 ? 29 : (type == 2 || type == 4) ? 33 : (type == 3) ? 37 : -1;
    if (sound >= 0) {
        Play(kChReel, sound, "reel");
        sIdleReel = true;
    }
}

extern "C" void RF3SoundFx_Strike(void) {
    if (!RF3_IsEnabled()) {
        return;
    }
    if (sIdleReel) {
        RF3Sound_Stop(kChReel);
        sIdleReel = false;
    }
    sFight = true;
    sFightReel = -1;
    sStrainTimer = sStreamTimer = 0.0f;
    sLastMode = -1;
    Play(kChBite, 48, "bite");
}

extern "C" void RF3SoundFx_FightStep(int32_t prevMode, int32_t mode, int32_t lastRun, int32_t hookSet,
                                     float hookStrength, int32_t record) {
    if (!RF3_IsEnabled()) {
        return;
    }
    if (hookSet) {
        const bool strong = mode == static_cast<int32_t>(rf3::Mode::Bukkonuki) || hookStrength > 70.0f;
        const int sound = strong ? 42 : (hookStrength > 30.0f ? 43 : 23);
        char why[48];
        std::snprintf(why, sizeof(why), "hook-set strength=%.0f", hookStrength);
        Play(kChHook, sound, why);
    }
    // The fish's streams, per RF3 step (RF3-MECHANICS 9, "The fight's sound timing").
    const rf3::Mode m = static_cast<rf3::Mode>(mode);
    if (mode != prevMode) {
        sStreamTimer = 0.0f; // 0x0019A280 clears it
        if (m == rf3::Mode::Stiffen && static_cast<rf3::Mode>(lastRun) != rf3::Mode::Away) {
            FishStream(record, false, mode, "stiffen-entry");
        }
    }
    if (m == rf3::Mode::Away || m == rf3::Mode::Struggle) {
        if (sStreamTimer <= 0.0f) {
            const bool struggle = m == rf3::Mode::Struggle;
            sStreamTimer = struggle ? Spread(0.2f, 0.1f) : Spread(1.0f, 0.3f);
            FishStream(record, struggle, mode, struggle ? "struggle" : "away");
        } else {
            sStreamTimer -= kStepSeconds;
        }
    }
}

extern "C" void RF3SoundFx_FightFrame(int32_t mode, int32_t hooked, int32_t reelHeld, float load, int32_t record,
                                      float dt) {
    if (!RF3_IsEnabled() || !hooked) {
        return;
    }
    const bool entered = mode != sLastMode;
    sLastMode = mode;
    (void)record; // logged with the mode below; the fish's streams use RF3SoundFx_FightStep's
    // The reel, by the line's load.
    if (reelHeld) {
        const int type = ReelType();
        const int q = load < 0.25f ? 0 : (load < 0.5f ? 1 : (load < 0.75f ? 2 : 3));
        int want = -1;
        if (type == 1) {
            want = 28 + q;
        } else if (type == 2 || type == 4) {
            want = 32 + q;
        } else if (type == 3) {
            want = load < 0.5f ? 36 : 37;
        }
        if (want >= 0 && want != sFightReel) { // step 63c-3: only on a change, never replayed
            RF3Sound_Play(kChReel, "fis", want, 1.0f, 1.0f);
            char why[48];
            std::snprintf(why, sizeof(why), "fight-reel load=%.2f", load);
            Log("rf3-sfx " + std::string(why) + " sound=fis/" + std::to_string(want));
            sFightReel = want;
        }
    } else if (sFightReel >= 0) {
        RF3Sound_Stop(kChReel);
        sFightReel = -1;
    }
    // Line strain.
    if (load >= kStrainLoad) {
        sStrainTimer -= dt;
        if (sStrainTimer <= 0.0f) {
            RF3Sound_PlayStreamSpu(kStreamBank, kStrainStream, 1.0f, static_cast<int32_t>(load * 2816.0f));
            sStrainTimer = 0.5f - load * 0.4f;
        }
    } else {
        sStrainTimer = 0.0f;
    }
    // The fish's streams: RF3SoundFx_FightStep (step 63c-3).
    if (entered) {
        char line[96];
        std::snprintf(line, sizeof(line), "rf3-sfx mode=%d record=%d load=%.2f", mode, record, load);
        Log(line);
    }
}

extern "C" void RF3SoundFx_FightOutcome(int32_t outcome) {
    if (!RF3_IsEnabled()) {
        return;
    }
    switch (static_cast<rf3::Outcome>(outcome)) {
        case rf3::Outcome::Missed:
            Play(kChHook, 20, "missed");
            break;
        case rf3::Outcome::LineBreak:
            sFightReel = -1;
            Play(kChReel, 41, "line-break");
            break;
        case rf3::Outcome::HookOut:
            sFightReel = -1;
            Play(kChReel, 52, "hook-out");
            break;
        default:
            break;
    }
}

extern "C" void RF3SoundFx_FightEnded(void) {
    if (sFightReel >= 0) {
        RF3Sound_Stop(kChReel);
    }
    sFightReel = -1;
    sFight = false;
    sLastMode = -1;
}
