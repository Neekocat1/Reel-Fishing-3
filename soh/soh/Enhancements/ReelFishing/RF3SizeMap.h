#ifndef RF3_SIZE_MAP_H
#define RF3_SIZE_MAP_H

// Reel Fishing III mod (step 12): OoT fish length -> RF3 largemouth bass length and size class.
// The user's rule: OoT's lowest size = RF3's lowest adult bass, OoT's highest = RF3's highest,
// linear in between and extrapolated outside. See docs/DESIGN.md step 12.

#include "RF3Params.h"

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

} // namespace rf3

#endif
