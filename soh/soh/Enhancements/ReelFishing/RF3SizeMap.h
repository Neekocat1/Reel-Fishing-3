#ifndef RF3_SIZE_MAP_H
#define RF3_SIZE_MAP_H

// Reel Fishing III mod (step 12): OoT fish length -> RF3 largemouth bass length and size class.
// Step 14b adds the same mapping for every species in the random pool.
// The user's rule: OoT's lowest size = RF3's lowest adult bass, OoT's highest = RF3's highest,
// linear in between and extrapolated outside. See docs/DESIGN.md step 12.

#include <algorithm>

#include "RF3Params.h"
#include "RF3FishTable.h"

namespace rf3 {

// OoT adult fish: sFishInits baseLength 33..71 plus a random 0..5 (z_fishing.c Fishing_Init).
inline constexpr float kOotLengthMin = 33.0f;
inline constexpr float kOotLengthMax = 76.0f;
// RF3 adult bass: normal adult 251..400 mm, lunker 401..700 mm (RF3-MECHANICS 1.8 / RESEARCH fishdata).
inline constexpr float kRf3AdultMinMm = 251.0f;
inline constexpr float kRf3AdultMaxMm = 700.0f;

inline float OotLengthToRf3Mm(float ootLength) {
    return kRf3AdultMinMm + (ootLength - kOotLengthMin) * (kRf3AdultMaxMm - kRf3AdultMinMm) /
                                (kOotLengthMax - kOotLengthMin);
}

// Size class for an RF3 length: normal adult up to 400 mm, lunker above. Shorter fish
// (child Link only) still count as normal adults: the scope is adult bass.
inline const FishParams& AdultBassForMm(float mm) {
    return (mm <= kBassNormal.lengthMaxMm) ? kBassNormal : kBassLunker;
}

// Step 14b: the same rule for any species in the random pool (RF3FishTable.h). OoT 33..76 maps
// onto the species' adult range (lowest minimum to highest maximum of its pool records).
inline constexpr int kSpeciesCount = 22; // the random pool: species 0..21 (DESIGN step 14)

// Step 23a: OoT's own pond fish as mod species (records 50-52, RF3FishTable.h). Not in the random pool, but the
// size helpers below use their records too. The loach has its own OoT range: base 43..45 plus 0..5
// (z_fishing.c sFishInits / Fishing_Init), mapped onto the user's 500..900 mm.
inline constexpr int kSpeciesHylianBass = 33;
inline constexpr int kSpeciesHylianLoach = 34;
inline constexpr float kOotLoachLengthMin = 43.0f;
inline constexpr float kOotLoachLengthMax = 50.0f;

inline bool SizeRecordFor(const FishRecord& r, int species) {
    return r.species == species && (r.inRandomPool || species == kSpeciesHylianBass || species == kSpeciesHylianLoach);
}

inline void SpeciesAdultRangeMm(int species, float* minMm, float* maxMm) {
    *minMm = 1e9f;
    *maxMm = 0.0f;
    for (const FishRecord& r : kFishRecords) {
        if (SizeRecordFor(r, species)) {
            *minMm = std::min(*minMm, static_cast<float>(r.params.lengthMinMm));
            *maxMm = std::max(*maxMm, static_cast<float>(r.params.lengthMaxMm));
        }
    }
}

inline float OotLengthToSpeciesMm(int species, float ootLength) {
    float minMm, maxMm;
    SpeciesAdultRangeMm(species, &minMm, &maxMm);
    float ootMin = (species == kSpeciesHylianLoach) ? kOotLoachLengthMin : kOotLengthMin;
    float ootMax = (species == kSpeciesHylianLoach) ? kOotLoachLengthMax : kOotLengthMax;
    // Step 23b: the loach's range is narrow (400 mm over 7 OoT units), so a child's loach (OoT x0.73) would go
    // below zero; under its minimum it scales in proportion instead (31.4 -> 365 mm).
    if (species == kSpeciesHylianLoach && ootLength < ootMin) {
        return minMm * ootLength / ootMin;
    }
    return minMm + (ootLength - ootMin) * (maxMm - minMm) / (ootMax - ootMin);
}

// The species' pool record for mm: the smallest class whose maximum is at least mm, else the
// largest class. For bass this is step 12's rule (<= 400 mm normal, above that lunker).
inline const FishRecord& SpeciesRecordForMm(int species, float mm) {
    const FishRecord* best = nullptr;    // smallest class with lengthMaxMm >= mm
    const FishRecord* largest = nullptr; // fallback above every class
    for (const FishRecord& r : kFishRecords) {
        if (!SizeRecordFor(r, species)) {
            continue;
        }
        if (largest == nullptr || r.params.lengthMaxMm > largest->params.lengthMaxMm) {
            largest = &r;
        }
        if (mm <= r.params.lengthMaxMm && (best == nullptr || r.params.lengthMaxMm < best->params.lengthMaxMm)) {
            best = &r;
        }
    }
    if (best != nullptr) {
        return *best;
    }
    return largest != nullptr ? *largest : kFishRecords[0];
}

} // namespace rf3

#endif
