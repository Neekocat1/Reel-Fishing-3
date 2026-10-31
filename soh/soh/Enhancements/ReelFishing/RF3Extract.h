#pragma once

// Reel Fishing III mod (step 76b): the in-game extractor's job (docs/DESIGN.md "Step 76 design"). It reads the
// player's Reel Fishing III disc image on a background thread and builds the mod's archives from it. Both jobs check
// the disc and PACKFILE.FPK first. Check (P0) then lists every file (size, CRC-32) in logs/rf3_extract_listing.txt and
// writes a test archive logs/rf3_extract_test.o2r. Build (P1-P4) writes rf3_tackle.o2r, rf3_sounds.o2r,
// rf3_models.o2r and rf3_music.o2r into the mods folder; an archive the running game holds open is written as
// <name>.o2r.new and moved into place at the next start (ApplyPendingArchives). Step 77: the music is optional, each
// archive records the converter version that built it (rf3extract/build.txt), and SoH's startup extractor offers the
// build when an archive is missing or older (RF3ExtractStartup).

#include <string>
#include <vector>

namespace rf3extract {

struct Status {
    bool running = false;
    bool done = false;  // finished (ok or failed)
    bool ok = false;
    float progress = 0; // 0..1
    std::string step;   // what it is doing now
    std::vector<std::string> messages;
};

enum class Job { Check, Build };

struct BuildOptions {
    bool music = true;      // also rf3_music.o2r (the slow one)
    bool atStartup = false; // from SoH's startup extractor: the archives load on this launch
};

// The converters' version, written into every archive the Build job makes. Raise it whenever a converter's output
// changes, so startup offers a rebuild of older archives.
constexpr int kConverterVersion = 1;

// Starts the job on a background thread; false if one is already running.
bool Start(const std::string& discPath, Job job, BuildOptions options = {});
// A copy of the job's state, for drawing.
Status Get();

// At startup, before the mods folder is read: moves every <name>.o2r.new a Build left over its <name>.o2r.
void ApplyPendingArchives();

// The archives a build would make that are missing or older than kConverterVersion, as "rf3_models.o2r (missing)"
// lines. An archive without a version (built by the project's Python tools) counts as current.
std::vector<std::string> ArchivesNeeded(bool music);

} // namespace rf3extract
