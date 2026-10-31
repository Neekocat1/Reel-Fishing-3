// Reel Fishing III mod (step 76b, P0): see RF3Disc.h. Mirrors tools/rf3_disc.py (Disc, read_fpk, verify) and
// rf3_menuassets.py's check_disc.

#include "RF3Disc.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>

namespace rf3disc {

namespace {

constexpr uint32_t kSector = 2048;
constexpr uint32_t kRawSector = 2352;
constexpr uint8_t kSync[12] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };

uint32_t U32(const uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// 64-bit seek: a .bin is about 2-4 GB.
bool Seek(FILE* f, long long pos) {
#ifdef _WIN32
    return _fseeki64(f, pos, SEEK_SET) == 0;
#else
    return fseeko(f, static_cast<off_t>(pos), SEEK_SET) == 0;
#endif
}

std::string Upper(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

// A NUL-padded name of at most n bytes.
std::string CName(const uint8_t* p, size_t n) {
    size_t len = 0;
    while (len < n && p[len] != 0) {
        len++;
    }
    return std::string(reinterpret_cast<const char*>(p), len);
}

} // namespace

Disc::~Disc() {
    if (mFile != nullptr) {
        std::fclose(mFile);
    }
}

bool Disc::Open(const std::string& path, std::string* error) {
    mFile = std::fopen(path.c_str(), "rb");
    if (mFile == nullptr) {
        *error = "Can't open " + path;
        return false;
    }
    uint8_t head[16] = {};
    if (std::fread(head, 1, sizeof(head), mFile) != sizeof(head)) {
        *error = "The file is too short to be a disc image.";
        return false;
    }
    mRaw = std::memcmp(head, kSync, sizeof(kSync)) == 0;
    if (mRaw && head[15] != 2) {
        *error = "Unexpected CD sector mode " + std::to_string(head[15]) + " (expected 2).";
        return false;
    }
    std::vector<uint8_t> pvd;
    if (!ReadSectors(16, 1, &pvd) || std::memcmp(pvd.data() + 1, "CD001", 5) != 0) {
        *error = "No ISO 9660 volume descriptor found. Is this the Reel Fishing III disc image (.bin or .iso)?";
        return false;
    }
    mRootLba = U32(pvd.data() + 156 + 2);
    mRootSize = U32(pvd.data() + 156 + 10);
    return true;
}

bool Disc::ReadSectors(uint32_t lba, uint32_t count, std::vector<uint8_t>* out) {
    out->assign(static_cast<size_t>(count) * kSector, 0);
    if (!mRaw) {
        if (!Seek(mFile, static_cast<long long>(lba) * kSector)) {
            return false;
        }
        return std::fread(out->data(), 1, out->size(), mFile) == out->size();
    }
    for (uint32_t i = 0; i < count; i++) {
        // Mode 2 Form 1: 12 sync + 4 header + 8 subheader, then 2048 data bytes.
        if (!Seek(mFile, static_cast<long long>(lba + i) * kRawSector + 24)) {
            return false;
        }
        if (std::fread(out->data() + static_cast<size_t>(i) * kSector, 1, kSector, mFile) != kSector) {
            return false;
        }
    }
    return true;
}

bool Disc::Read(uint32_t lba, uint32_t offset, uint32_t size, std::vector<uint8_t>* out) {
    const uint32_t first = lba + offset / kSector;
    const uint32_t skip = offset % kSector;
    const uint32_t count = (skip + size + kSector - 1) / kSector;
    std::vector<uint8_t> sectors;
    if (!ReadSectors(first, count, &sectors)) {
        return false;
    }
    out->assign(sectors.begin() + skip, sectors.begin() + skip + size);
    return true;
}

bool Disc::FindIn(uint32_t dirLba, uint32_t dirSize, const std::string& name, uint32_t* lba, uint32_t* size) {
    std::vector<uint8_t> data;
    if (!ReadSectors(dirLba, (dirSize + kSector - 1) / kSector, &data)) {
        return false;
    }
    const std::string want = Upper(name);
    size_t i = 0;
    while (i < data.size()) {
        const uint8_t length = data[i];
        if (length == 0) {
            i = (i / kSector + 1) * kSector;
            continue;
        }
        if (i + length > data.size()) {
            break;
        }
        const uint8_t* rec = data.data() + i;
        i += length;
        std::string ident(reinterpret_cast<const char*>(rec + 33), rec[32]);
        ident = ident.substr(0, ident.find(';'));
        if (Upper(ident) == want) {
            *lba = U32(rec + 2);
            *size = U32(rec + 10);
            return true;
        }
    }
    return false;
}

bool Disc::FindRootFile(const std::string& name, uint32_t* lba, uint32_t* size) {
    return FindIn(mRootLba, mRootSize, name, lba, size);
}

bool Disc::FindDirFile(const std::string& dir, const std::string& name, uint32_t* lba, uint32_t* size) {
    uint32_t dirLba = 0;
    uint32_t dirSize = 0;
    return FindRootFile(dir, &dirLba, &dirSize) && FindIn(dirLba, dirSize, name, lba, size);
}

bool Pack::Open(const std::string& discPath, std::string* error) {
    if (!mDisc.Open(discPath, error)) {
        return false;
    }
    if (!mDisc.FindRootFile("PACKFILE.FPK", &mFpkLba, &mFpkSize)) {
        *error = "PACKFILE.FPK is not on this disc. Is this the Reel Fishing III disc image?";
        return false;
    }
    std::vector<uint8_t> head;
    if (!mDisc.Read(mFpkLba, 0, 0x20, &head) || std::memcmp(head.data(), "OFPK", 4) != 0) {
        *error = "PACKFILE.FPK doesn't start with OFPK.";
        return false;
    }
    mDirs = U32(head.data() + 8);
    mHeaderFiles = U32(head.data() + 12);
    const uint32_t dirOff = U32(head.data() + 16);
    mHeaderSize = U32(head.data() + 24);
    std::vector<uint8_t> table;
    if (mHeaderSize > (64u << 20) || !mDisc.Read(mFpkLba, 0, mHeaderSize, &table)) {
        *error = "PACKFILE.FPK's table can't be read.";
        return false;
    }
    mFiles.clear();
    for (uint32_t d = 0; d < mDirs; d++) {
        const size_t dOff = dirOff + static_cast<size_t>(d) * 32;
        if (dOff + 32 > table.size()) {
            *error = "PACKFILE.FPK's folder table runs past its header.";
            return false;
        }
        const uint32_t first = U32(table.data() + dOff);
        const uint32_t count = U32(table.data() + dOff + 4);
        const std::string dname = CName(table.data() + dOff + 8, 24);
        for (uint32_t j = 0; j < count; j++) {
            const size_t o = first + static_cast<size_t>(j) * 32;
            if (o + 32 > table.size()) {
                *error = "PACKFILE.FPK's file table runs past its header.";
                return false;
            }
            FpkFile f;
            f.startSector = U32(table.data() + o);
            f.sectors = U32(table.data() + o + 4);
            f.size = U32(table.data() + o + 8);
            f.path = dname + CName(table.data() + o + 16, 16);
            mFiles.push_back(f);
        }
    }
    return true;
}

bool Pack::ReadFile(const FpkFile& f, std::vector<uint8_t>* out) {
    return mDisc.Read(mFpkLba + f.startSector, 0, f.size, out) && out->size() == f.size;
}

bool Pack::Read(const std::string& path, std::vector<uint8_t>* out) {
    for (const FpkFile& f : mFiles) {
        if (f.path == path) {
            return ReadFile(f, out);
        }
    }
    return false;
}

bool Pack::CheckDisc(std::string* cnf, std::string* error) {
    uint32_t lba = 0;
    uint32_t size = 0;
    std::vector<uint8_t> data;
    if (!mDisc.FindRootFile("SYSTEM.CNF", &lba, &size) || size > 4096 || !mDisc.Read(lba, 0, size, &data)) {
        *error = "SYSTEM.CNF is missing: this is not a PlayStation 2 disc image.";
        return false;
    }
    *cnf = std::string(data.begin(), data.end());
    while (!cnf->empty() && std::isspace(static_cast<unsigned char>(cnf->back()))) {
        cnf->pop_back();
    }
    // As check_disc: "SLUS_205.55" anywhere, and "VER" + spaces + "=" + spaces + "1.01".
    bool version = false;
    for (size_t at = cnf->find("VER"); at != std::string::npos && !version; at = cnf->find("VER", at + 3)) {
        size_t p = at + 3;
        while (p < cnf->size() && std::isspace(static_cast<unsigned char>((*cnf)[p]))) {
            p++;
        }
        if (p < cnf->size() && (*cnf)[p] == '=') {
            p++;
            while (p < cnf->size() && std::isspace(static_cast<unsigned char>((*cnf)[p]))) {
                p++;
            }
            version = cnf->compare(p, 4, "1.01") == 0;
        }
    }
    if (cnf->find("SLUS_205.55") == std::string::npos || !version) {
        *error = "This is not Reel Fishing III (USA), SLUS-20555 version 1.01. SYSTEM.CNF reads: " + *cnf;
        return false;
    }
    return true;
}

std::vector<std::string> Pack::Verify() {
    std::vector<std::string> problems;
    const uint32_t fpkSectors = (mFpkSize + kSector - 1) / kSector;
    if (mFiles.size() != mHeaderFiles) {
        problems.push_back("header says " + std::to_string(mHeaderFiles) + " files, table has " +
                           std::to_string(mFiles.size()));
    }
    std::set<std::string> paths;
    for (const FpkFile& f : mFiles) {
        paths.insert(f.path);
        if (f.size > f.sectors * kSector) {
            problems.push_back(f.path + ": size larger than its sectors");
        }
        if (f.size != 0 && f.sectors > 0 && f.size <= (f.sectors - 1) * kSector) {
            problems.push_back(f.path + ": more sectors than needed");
        }
        if (static_cast<uint64_t>(f.startSector) * kSector < mHeaderSize) {
            problems.push_back(f.path + ": starts inside the header");
        }
        if (f.startSector + f.sectors > fpkSectors) {
            problems.push_back(f.path + ": runs past the end of PACKFILE.FPK");
        }
    }
    if (paths.size() != mFiles.size()) {
        problems.push_back("duplicate paths");
    }
    std::vector<const FpkFile*> byStart;
    for (const FpkFile& f : mFiles) {
        byStart.push_back(&f);
    }
    std::sort(byStart.begin(), byStart.end(),
              [](const FpkFile* a, const FpkFile* b) { return a->startSector < b->startSector; });
    for (size_t i = 1; i < byStart.size(); i++) {
        if (byStart[i - 1]->startSector + byStart[i - 1]->sectors > byStart[i]->startSector) {
            problems.push_back("overlap: " + byStart[i - 1]->path + " and " + byStart[i]->path);
        }
    }
    return problems;
}

uint32_t Crc32(const uint8_t* data, size_t size) {
    struct Table {
        uint32_t v[256];
        Table() {
            for (uint32_t i = 0; i < 256; i++) {
                uint32_t c = i;
                for (int k = 0; k < 8; k++) {
                    c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                }
                v[i] = c;
            }
        }
    };
    static const Table table; // built once, thread-safe (C++11 static init)
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; i++) {
        crc = table.v[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

} // namespace rf3disc
