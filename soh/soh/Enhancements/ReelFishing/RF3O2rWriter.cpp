// Reel Fishing III mod (step 76b, P0): see RF3O2rWriter.h.

#include "RF3O2rWriter.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>

#include <zip.h>

extern "C" {
#include "variables.h" // gBuildVersionMajor/Minor/Patch
}

namespace rf3disc {

O2rWriter::~O2rWriter() {
    if (mZip != nullptr) {
        zip_discard(mZip); // not closed: drop it, the .tmp is never moved into place
        std::error_code ec;
        std::filesystem::remove(mTmp, ec);
    }
}

bool O2rWriter::Open(const std::string& path, std::string* error) {
    mPath = path;
    mTmp = path + ".tmp";
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::filesystem::remove(mTmp, ec);
    int err = 0;
    mZip = zip_open(mTmp.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &err);
    if (mZip == nullptr) {
        *error = "Can't create " + mTmp + " (libzip error " + std::to_string(err) + ")";
        return false;
    }
    return true;
}

bool O2rWriter::Add(const std::string& name, const std::vector<uint8_t>& data, std::string* error) {
    // libzip reads the bytes when the archive is closed: give it its own copy, which it frees.
    void* copy = std::malloc(data.empty() ? 1 : data.size());
    if (copy == nullptr) {
        *error = "Out of memory for " + name;
        return false;
    }
    if (!data.empty()) {
        std::memcpy(copy, data.data(), data.size());
    }
    zip_source_t* src = zip_source_buffer(mZip, copy, data.size(), 1);
    if (src == nullptr) {
        std::free(copy);
        *error = "libzip: " + std::string(zip_strerror(mZip));
        return false;
    }
    const zip_int64_t index = zip_file_add(mZip, name.c_str(), src, ZIP_FL_OVERWRITE | ZIP_FL_ENC_UTF_8);
    if (index < 0) {
        zip_source_free(src);
        *error = "libzip: " + std::string(zip_strerror(mZip)) + " (" + name + ")";
        return false;
    }
    zip_set_file_compression(mZip, static_cast<zip_uint64_t>(index), ZIP_CM_DEFLATE, 0);
    mEntries++;
    return true;
}

bool O2rWriter::Close(std::string* error) {
    // portVersion as torch writes it and OTRGlobals.cpp reads it: an endianness byte (1 = big), then major, minor and
    // patch as big-endian u16.
    const uint16_t v[3] = { gBuildVersionMajor, gBuildVersionMinor, gBuildVersionPatch };
    std::vector<uint8_t> version = { 1 };
    for (uint16_t x : v) {
        version.push_back(static_cast<uint8_t>(x >> 8));
        version.push_back(static_cast<uint8_t>(x & 0xFF));
    }
    if (!Add("portVersion", version, error)) {
        return false;
    }
    if (zip_close(mZip) != 0) {
        *error = "libzip: " + std::string(zip_strerror(mZip));
        return false; // the destructor discards it
    }
    mZip = nullptr;
    std::error_code ec;
    std::filesystem::rename(mTmp, mPath, ec);
    if (ec) {
        std::filesystem::remove(mPath, ec);
        ec.clear();
        std::filesystem::rename(mTmp, mPath, ec);
    }
    if (ec) {
        // the game holds the old archive open: leave the new one beside it for the next start
        const std::string pending = mPath + ".new";
        std::error_code ec2;
        std::filesystem::remove(pending, ec2);
        std::filesystem::rename(mTmp, pending, ec2);
        if (ec2) {
            *error = "Can't move " + mTmp + " to " + mPath + " or " + pending + ": " + ec2.message();
            return false;
        }
        mPending = true;
    }
    return true;
}

} // namespace rf3disc
