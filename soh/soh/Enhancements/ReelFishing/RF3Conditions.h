// Reel Fishing III mod (step 57c-3, docs/DESIGN.md "Step 57 design"): RF3's season, weather and time of day, live
// from OoT's day count, clock, scene and weather (rf3::ConditionsFor, RF3Sim.cpp).
#pragma once

#include <string>

#include "RF3Params.h"

// The conditions now. Outside gameplay (no play state) the user's RF3 session (Summer, Sunny, Morning).
// Logs "conditions ..." in logs/rf3_records.txt when the conditions, the OoT day or the scene change.
rf3::Conditions RF3Conditions_Current();
// One line for the debug window: names, the day count and what decided the weather.
std::string RF3Conditions_Describe();
