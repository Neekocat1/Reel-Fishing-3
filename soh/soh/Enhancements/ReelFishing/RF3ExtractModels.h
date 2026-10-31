#pragma once

// Reel Fishing III mod (step 76b, P3): rf3_models.o2r's contents from the disc, as the project's tools/rf3_models.py
// "models" makes them (docs/DESIGN.md "Step 76 design"): every lure, fly, bait, float, hook and menu bait as rigid
// batches, the lures' skinned copies, the fish and the rods as skinned models, their HD textures, and
// rf3models/models.json (the list, RF3's motion patterns and the fish movement values).

#include <string>
#include <vector>

#include "RF3Disc.h"
#include "RF3ExtractCommon.h"

namespace rf3extract {

bool BuildModels(rf3disc::Pack& pack, std::vector<Entry>* out, std::string* error, const ProgressFn& progress);

} // namespace rf3extract
