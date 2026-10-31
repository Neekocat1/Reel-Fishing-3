#pragma once

// Reel Fishing III mod (step 76b, P2): rf3_sounds.o2r's contents from the disc, as the project's tools/rf3_sound.py
// "build" makes them (docs/DESIGN.md "Step 76 design"): every sound of the fis and sys banks and every fis stream as
// an .rf3c clip, and rf3sounds/index.txt.

#include <string>
#include <vector>

#include "RF3Disc.h"
#include "RF3ExtractCommon.h"

namespace rf3extract {

bool BuildSound(rf3disc::Pack& pack, std::vector<Entry>* out, std::string* error, const ProgressFn& progress);

} // namespace rf3extract
