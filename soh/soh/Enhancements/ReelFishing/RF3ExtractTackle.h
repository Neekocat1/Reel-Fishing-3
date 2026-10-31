#pragma once

// Reel Fishing III mod (step 76b, P1): rf3_tackle.o2r's contents from the disc, as the project's
// tools/rf3_menuassets.py "build" makes them (docs/DESIGN.md "Step 76 design"): the tackle box screen's pictures
// (as texture resources), layout.json and tackle.json.

#include <string>
#include <vector>

#include "RF3Disc.h"
#include "RF3ExtractCommon.h"

namespace rf3extract {

bool BuildTackle(rf3disc::Pack& pack, std::vector<Entry>* out, std::string* error, const ProgressFn& progress);

} // namespace rf3extract
