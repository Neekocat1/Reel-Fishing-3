#ifndef RF3_PARAMS_H
#define RF3_PARAMS_H

// Reel Fishing III mod: fish and tackle numbers.
// Every value is a measured fact recorded in docs/RF3-MECHANICS.md of the
// project repository; the comment gives the section. Nothing here is code
// from Reel Fishing III.

#include <cstdint>

namespace rf3 {

// One fish size class (one fishdata.fdt record). Section 1.8 unless noted.
struct FishParams {
    const char* name;
    float hookGiveUpTime;   // s, hook-set give-up time
    float hookWindowStart;  // s after the strike
    float hookWindowEnd;    // s after the strike
    float perfectBand;      // BUKKONUKI band, fraction of the window length (1.6)
    uint16_t flags;         // bit 2: BUKKONUKI allowed anywhere (1.6)
    int16_t power;          // tension and distance multiplier (1.3)
    int8_t resistExtra;     // RESIST lasts 0.5 s + resistExtra / 100 (1.5)
    uint8_t stopDamageByte; // STOP stamina damage = byte / 255 * 10 (1.5)
    float followDuration;   // s (1.5)
    float stopDuration;     // s (1.5)
    float awayDuration;     // s (1.5)
    float struggleDuration; // s (1.5)
    float hookFactor;       // hook rate multiplier: 1.5 for bass (1.3)
    int tensionBonus;       // default tackle's per-species tension bonus, line+rod+lure (1.4)
    int sightBonus;         // default tackle's per-species sight bonus (2.4)
    uint16_t lengthMinMm;   // size class range (4; RESEARCH.md fishdata table)
    uint16_t lengthMaxMm;
    float weightMinG;
    float weightMaxG;
    // Approach (section 2).
    float sightBaseM;          // record +0x1C (2.4)
    uint8_t maxAction;         // record +0x12: interest only up to this x/255 (2.2)
    int8_t activitySeason[3];  // record +0x48, indexed by condition A (2.4)
    int8_t activityWeather[3]; // record +0x4B, indexed by condition C
    int8_t activityTime[3];    // record +0x4E, indexed by condition B
};

// Game conditions, each 0..2 (2.4). Which index means what is only partly known:
// the user's RF3 session (summer, morning, sunny) was season 1, time 0, weather 0.
struct Conditions {
    int season;  // condition A
    int time;    // condition B
    int weather; // condition C
};
inline constexpr Conditions kConditionsTestSession = { 1, 0, 0 };

// Largemouth bass, fishdata.fdt records 14 / 15 / 16.
// Record 14 (juvenile) is used only to check against the test fight's save states.
inline constexpr FishParams kBassJuvenile = {
    "bass_s (14)", 2.0f, 0.26f, 1.16f, 0.3f, 0, 4, 100, 220, 12.0f, 0.6f, 5.0f, 3.0f, 1.5f, 9, 4, 180, 250, 250.0f, 399.0f,
    3.5f, 255, { 3, 3, 2 }, { 3, 2, 3 }, { 2, 2, 0 },
};
inline constexpr FishParams kBassNormal = {
    "bass_n (15)", 2.0f, 0.26f, 1.16f, 0.1f, 0, 5, 100, 230, 10.0f, 0.5f, 6.0f, 4.0f, 1.5f, 5, 7, 251, 400, 400.0f, 1199.0f,
    3.5f, 255, { 2, 3, 1 }, { 3, 2, 3 }, { 2, 2, 0 },
};
inline constexpr FishParams kBassLunker = {
    "bass_b (16)", 2.0f, 0.26f, 1.16f, 0.0f, 0, 6, 100, 240, 10.0f, 0.5f, 6.0f, 5.0f, 1.5f, 2, 8, 401, 700, 1200.0f, 4000.0f,
    3.5f, 255, { 2, 3, 1 }, { 3, 2, 3 }, { 2, 2, 0 },
};

// Default tackle: Lure Rod/Bass + Baitcast Reel + 10 lb line + Suspended Vibration G.
struct TackleParams {
    float rodLengthM;          // RROD +0x0C, 1.4 / 1.7
    int rodValue;              // RROD +0x10, used by the distance formula (1.3)
    int lineStep;              // line diameter step 0..4; 2 = 10 lb (1.4)
    int16_t tensionLimitBase[5]; // RROD +0x16, one per line step (1.4)
};

inline constexpr TackleParams kDefaultTackle = {
    1.6f, 7, 2, { 120, 140, 160, 180, 200 },
};

} // namespace rf3

#endif
