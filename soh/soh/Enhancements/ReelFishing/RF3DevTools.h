#pragma once
// Reel Fishing III mod (step 78a, docs/DESIGN.md "Step 78 design"): the developer tools (the RF3 debug window, the
// fish region editor, the CSV log) exist only in Debug builds. Release builds define NDEBUG on every platform (the
// soh target's Release definitions on Windows, CMAKE_CXX_FLAGS_RELEASE elsewhere); Debug builds don't.

#if defined(NDEBUG)
#define RF3_DEV_TOOLS 0
#else
#define RF3_DEV_TOOLS 1
#endif

// Step 78b: the mod's text logs (logs/rf3_*.txt) are written only while this is on. Default: on in Debug builds, off
// in Release; the menu's "Write Reel Fishing III logs" switches it. The disc build's rf3_extract.txt ignores it.
#define CVAR_RF3_WRITE_LOGS CVAR_ENHANCEMENT("ReelFishing3.WriteLogs")

#ifdef __cplusplus
#include <fstream>
#include <string>

bool RF3LogsOn();

// Opens a log for appending, or returns a closed stream (writes do nothing) while the logs are off.
std::ofstream RF3OpenLog(const std::string& path);
#endif
