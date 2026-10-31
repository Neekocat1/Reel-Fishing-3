#pragma once

// Reel Fishing III mod (step 76b, P4): rf3_music.o2r's contents from the disc, as the project's tools/rf3_music.py
// "build" makes them (docs/DESIGN.md "Step 68" and "Step 76 design"): RF3's music tracks m05-m21 played through the
// sequence and bank rules of RF3's sound driver, as streamed SoH custom music (sequence, font and sample XML plus a
// 16-bit 32 kHz WAV per side) and rf3music/index.txt.

#include <string>
#include <vector>

#include "RF3Disc.h"
#include "RF3ExtractCommon.h"

namespace rf3extract {

bool BuildMusic(rf3disc::Pack& pack, std::vector<Entry>* out, std::string* error, const ProgressFn& progress);

} // namespace rf3extract
