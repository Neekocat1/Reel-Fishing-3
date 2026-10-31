#pragma once

// Reel Fishing III mod (step 76b, P0): Reel Fishing III's disc image, read in the game for the in-game extractor
// (docs/DESIGN.md "Step 76 design"). The same reading as the project's tools/rf3_disc.py: ISO 9660 on a raw
// MODE2/2352 .bin or a 2048-byte .iso, and PACKFILE.FPK's table. Read-only; nothing here holds game data.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace rf3disc {

class Disc {
  public:
    ~Disc();
    // Opens the image and finds the root directory. false and *error on failure.
    bool Open(const std::string& path, std::string* error);
    // size bytes from offset bytes into the extent at lba.
    bool Read(uint32_t lba, uint32_t offset, uint32_t size, std::vector<uint8_t>* out);
    // A file in the disc root, or in a root folder (e.g. MODULES). false if absent.
    bool FindRootFile(const std::string& name, uint32_t* lba, uint32_t* size);
    bool FindDirFile(const std::string& dir, const std::string& name, uint32_t* lba, uint32_t* size);
    bool Raw() const {
        return mRaw;
    }

  private:
    bool ReadSectors(uint32_t lba, uint32_t count, std::vector<uint8_t>* out);
    bool FindIn(uint32_t dirLba, uint32_t dirSize, const std::string& name, uint32_t* lba, uint32_t* size);
    FILE* mFile = nullptr;
    bool mRaw = false;
    uint32_t mRootLba = 0;
    uint32_t mRootSize = 0;
};

struct FpkFile {
    std::string path; // e.g. "etc/fishdata.fdt"
    uint32_t startSector = 0;
    uint32_t sectors = 0;
    uint32_t size = 0;
};

// The disc plus PACKFILE.FPK's table (rf3_menuassets.py's Pack).
class Pack {
  public:
    bool Open(const std::string& discPath, std::string* error);
    const std::vector<FpkFile>& Files() const {
        return mFiles;
    }
    bool Read(const std::string& path, std::vector<uint8_t>* out);
    bool ReadFile(const FpkFile& f, std::vector<uint8_t>* out);
    // Reel Fishing III (USA), SLUS-20555 VER 1.01 (SYSTEM.CNF), as check_disc. *cnf = its text.
    bool CheckDisc(std::string* cnf, std::string* error);
    // rf3_disc.py verify: every file fits its sectors, lies inside the archive, no overlaps. Problems listed.
    std::vector<std::string> Verify();
    Disc& DiscImage() {
        return mDisc;
    }
    uint32_t FpkLba() const {
        return mFpkLba;
    }
    uint32_t Dirs() const {
        return mDirs;
    }
    uint32_t HeaderFiles() const {
        return mHeaderFiles;
    }

  private:
    Disc mDisc;
    std::vector<FpkFile> mFiles;
    uint32_t mFpkLba = 0;
    uint32_t mFpkSize = 0;
    uint32_t mDirs = 0;
    uint32_t mHeaderFiles = 0;
    uint32_t mHeaderSize = 0;
};

// zlib's CRC-32 (the listing's check against the Python reading).
uint32_t Crc32(const uint8_t* data, size_t size);

} // namespace rf3disc
