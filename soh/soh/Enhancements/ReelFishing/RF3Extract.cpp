// Reel Fishing III mod (step 76b): see RF3Extract.h.
// Log, don't look: the job's messages also go to logs/rf3_extract.txt.

#include "RF3Extract.h"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>

#include <ship/Context.h>
#include <zip.h>

#include "RF3Disc.h"
#include "RF3ExtractModels.h"
#include "RF3ExtractMusic.h"
#include "RF3ExtractSound.h"
#include "RF3ExtractTackle.h"
#include "RF3O2rWriter.h"

namespace rf3extract {

namespace {

std::mutex sMutex;
Status sStatus;

std::string LogDir() {
    std::string dir = Ship::Context::GetPathRelativeToAppDirectory("logs");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

void Message(const std::string& text) {
    {
        std::lock_guard<std::mutex> lock(sMutex);
        sStatus.messages.push_back(text);
    }
    std::ofstream out(LogDir() + "/rf3_extract.txt", std::ios::app);
    char stamp[32];
    std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    out << stamp << ' ' << text << '\n';
}

std::string ModsDir() {
    // as OTRGlobals.cpp CheckAndCreateModFolder: the mods folder the game reads, or a new one by the app
    std::string dir = Ship::Context::LocateFileAcrossAppDirs("mods", "soh");
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) {
        dir = Ship::Context::GetPathRelativeToAppDirectory("mods", "soh");
        std::filesystem::create_directories(dir, ec);
    }
    return dir;
}

const char* const kArchives[] = { "rf3_tackle.o2r", "rf3_sounds.o2r", "rf3_models.o2r", "rf3_music.o2r" };

void Progress(const std::string& step, float progress) {
    std::lock_guard<std::mutex> lock(sMutex);
    sStatus.step = step;
    sStatus.progress = progress;
}

void Finish(bool ok, const std::string& text) {
    Message(text);
    std::lock_guard<std::mutex> lock(sMutex);
    sStatus.running = false;
    sStatus.done = true;
    sStatus.ok = ok;
    sStatus.progress = ok ? 1.0f : sStatus.progress;
    sStatus.step = ok ? "Done" : "Failed";
}

// Both jobs: open the disc, check it is Reel Fishing III (USA) 1.01 and that PACKFILE.FPK is sound.
bool OpenPack(const std::string& discPath, rf3disc::Pack* pack) {
    Message("disc: " + discPath);
    Progress("Opening the disc image", 0.0f);
    std::string error;
    if (!pack->Open(discPath, &error)) {
        Finish(false, "Failed: " + error);
        return false;
    }
    std::string cnf;
    if (!pack->CheckDisc(&cnf, &error)) {
        Finish(false, "Failed: " + error);
        return false;
    }
    Message(std::string("disc ok: Reel Fishing III (USA) SLUS-20555 VER 1.01, ") +
            (pack->DiscImage().Raw() ? "raw MODE2/2352 image" : "2048-byte image"));
    const std::vector<std::string> problems = pack->Verify();
    for (const std::string& p : problems) {
        Message("PACKFILE.FPK problem: " + p);
    }
    if (!problems.empty()) {
        Finish(false, "Failed: PACKFILE.FPK doesn't look right (" + std::to_string(problems.size()) +
                          " problems). Is the image damaged?");
        return false;
    }
    Message("PACKFILE.FPK ok: " + std::to_string(pack->Files().size()) + " files in " + std::to_string(pack->Dirs()) +
            " folders, at LBA " + std::to_string(pack->FpkLba()));
    return true;
}

// P0: list every file with its CRC-32, write a test archive.
void RunCheck(rf3disc::Pack& pack) {
    std::string error;
    const std::vector<rf3disc::FpkFile>& files = pack.Files();
    // The listing: one line per file, "path<TAB>size<TAB>crc32", in the table's order (tools/rf3_extract_check.py
    // compares it with Python's own reading of the disc).
    std::string listing = "# rf3 extract listing (step 76b P0): path, size, CRC-32\n";
    uint64_t total = 0;
    for (size_t i = 0; i < files.size(); i++) {
        std::vector<uint8_t> data;
        if (!pack.ReadFile(files[i], &data)) {
            Finish(false, "Failed: can't read " + files[i].path + " from the disc image.");
            return;
        }
        char line[160];
        std::snprintf(line, sizeof(line), "%s\t%u\t%08x\n", files[i].path.c_str(), files[i].size,
                      rf3disc::Crc32(data.data(), data.size()));
        listing += line;
        total += files[i].size;
        if (i % 16 == 0) {
            Progress("Reading " + files[i].path, 0.95f * static_cast<float>(i) / static_cast<float>(files.size()));
        }
    }
    const std::string listPath = LogDir() + "/rf3_extract_listing.txt";
    {
        std::ofstream out(listPath, std::ios::binary | std::ios::trunc);
        out << listing;
    }
    Message("read " + std::to_string(files.size()) + " files, " + std::to_string(total) + " bytes; listing: " +
            listPath);

    Progress("Writing the test archive", 0.97f);
    rf3disc::O2rWriter writer;
    const std::string testPath = LogDir() + "/rf3_extract_test.o2r";
    if (!writer.Open(testPath, &error) ||
        !writer.Add("rf3extract/listing.txt", std::vector<uint8_t>(listing.begin(), listing.end()), &error) ||
        !writer.Close(&error)) {
        Finish(false, "Failed: " + error);
        return;
    }
    Finish(true, "test archive: " + testPath + " (the disc reads correctly)");
}

const char* const kStampEntry = "rf3extract/build.txt";

// The converter version in an archive's rf3extract/build.txt: -1 no archive, 0 no stamp (or unreadable).
int ArchiveVersion(const std::string& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return -1;
    }
    int err = 0;
    zip_t* z = zip_open(path.c_str(), ZIP_RDONLY, &err);
    if (z == nullptr) {
        return 0;
    }
    int version = 0;
    zip_file_t* f = zip_fopen(z, kStampEntry, 0);
    if (f != nullptr) {
        char buf[64] = {};
        const zip_int64_t n = zip_fread(f, buf, sizeof(buf) - 1);
        if (n > 0) {
            std::sscanf(buf, "converter %d", &version);
        }
        zip_fclose(f);
    }
    zip_close(z);
    return version;
}

// P1-P4: each converter's entries into its archive in the mods folder, one archive at a time.
void RunBuild(rf3disc::Pack& pack, BuildOptions options) {
    using BuildFn = bool (*)(rf3disc::Pack&, std::vector<Entry>*, std::string*, const ProgressFn&);
    struct Part {
        const char* what;
        BuildFn build;
        float from, to; // the progress bar's share (the music takes most of the time)
    };
    // the bar's shares follow the Debug build's times (models about 25 s, music about 30 s on 4 threads)
    Part parts[] = { { "tackle box pictures and data", BuildTackle, 1, 0 },
                     { "sounds", BuildSound, 1, 0 },
                     { "models", BuildModels, 6, 0 },
                     { "music", BuildMusic, options.music ? 7.0f : 0.0f, 0 } };
    float sum = 0;
    for (const Part& p : parts) {
        sum += p.from;
    }
    float at = 0;
    for (Part& p : parts) {
        const float share = p.from / sum;
        p.from = at;
        p.to = at + share;
        at += share;
    }
    const std::string mods = ModsDir();
    Message("mods folder: " + mods + (options.music ? "" : " (music left out)"));
    int pending = 0;
    for (size_t i = 0; i < std::size(parts); i++) {
        const Part& part = parts[i];
        if (part.build == BuildMusic && !options.music) {
            continue;
        }
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<Entry> entries;
        std::string error;
        const ProgressFn progress = [&part](const std::string& step, float p) {
            Progress(std::string(part.what) + ": " + step, part.from + (part.to - part.from) * 0.95f * p);
        };
        Progress(std::string(part.what) + ": starting", part.from);
        if (!part.build(pack, &entries, &error, progress)) {
            Finish(false, std::string("Failed building the ") + part.what + ": " + error);
            return;
        }
        size_t bytes = 0;
        for (const Entry& e : entries) {
            bytes += e.data.size();
        }
        Progress(std::string(part.what) + ": writing " + kArchives[i], part.from + (part.to - part.from) * 0.95f);
        const std::string path = mods + "/" + kArchives[i];
        rf3disc::O2rWriter writer;
        if (!writer.Open(path, &error)) {
            Finish(false, "Failed: " + error);
            return;
        }
        // step 77b: the converter version, for the startup check
        const std::string stamp = "converter " + std::to_string(kConverterVersion) + "\n";
        entries.push_back({ kStampEntry, std::vector<uint8_t>(stamp.begin(), stamp.end()) });
        for (Entry& e : entries) {
            if (!writer.Add(e.name, e.data, &error)) {
                Finish(false, "Failed: " + error);
                return;
            }
            std::vector<uint8_t>().swap(e.data); // the writer has its own copy
        }
        if (!writer.Close(&error)) {
            Finish(false, "Failed: " + error);
            return;
        }
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        char line[200];
        std::snprintf(line, sizeof(line), "%s: %zu entries, %zu bytes, %.1f s", kArchives[i], entries.size(), bytes,
                      secs);
        Message(std::string(line) +
                (writer.Pending() ? " -> " + path + ".new (the game has the old one open; it replaces it at the "
                                                    "next start)"
                                  : " -> " + path));
        pending += writer.Pending() ? 1 : 0;
    }
    Finish(true, pending              ? "Built. Restart the game: the new archives replace the old ones when it starts."
                 : options.atStartup ? "Built. The game starts with them now."
                                     : "Built. Restart the game to load the archives.");
}

void Run(std::string discPath, Job job, BuildOptions options) {
    Message(job == Job::Check ? "job: check the disc" : "job: build the archives");
    rf3disc::Pack pack;
    if (!OpenPack(discPath, &pack)) {
        return;
    }
    if (job == Job::Check) {
        RunCheck(pack);
    } else {
        RunBuild(pack, options);
    }
}

} // namespace

bool Start(const std::string& discPath, Job job, BuildOptions options) {
    {
        std::lock_guard<std::mutex> lock(sMutex);
        if (sStatus.running) {
            return false;
        }
        sStatus = Status{};
        sStatus.running = true;
        sStatus.step = "Starting";
    }
    std::thread(Run, discPath, job, options).detach();
    return true;
}

Status Get() {
    std::lock_guard<std::mutex> lock(sMutex);
    return sStatus;
}

void ApplyPendingArchives() {
    const std::string mods = ModsDir();
    for (const char* name : kArchives) {
        const std::string path = mods + "/" + name;
        std::error_code ec;
        if (!std::filesystem::exists(path + ".new", ec)) {
            continue;
        }
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(path + ".new", path, ec);
        Message(std::string("startup: ") + name + ".new " +
                (ec ? "could not replace " + path + ": " + ec.message() : "moved into place"));
    }
}

std::vector<std::string> ArchivesNeeded(bool music) {
    std::vector<std::string> out;
    const std::string mods = ModsDir();
    for (const char* name : kArchives) {
        if (!music && std::string(name) == "rf3_music.o2r") {
            continue;
        }
        const int v = ArchiveVersion(mods + "/" + name);
        if (v < 0) {
            out.push_back(std::string(name) + " (missing)");
        } else if (v > 0 && v < kConverterVersion) {
            out.push_back(std::string(name) + " (made by an older version of the mod)");
        }
    }
    return out;
}

} // namespace rf3extract
