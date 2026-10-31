#pragma once

// Reel Fishing III mod (step 76b, P0): writes an .o2r archive (a deflate zip plus "portVersion"), as torch's
// Companion::Pack does for soh-o2r-packer (docs/DESIGN.md "Step 76 design"). The archive is written to
// "<path>.tmp" and moved over <path> only when it is complete, so a failed build never breaks a working archive. If
// <path> can't be replaced (the running game holds it open), the archive is left as "<path>.new" (Pending()).

#include <cstdint>
#include <string>
#include <vector>

struct zip;

namespace rf3disc {

class O2rWriter {
  public:
    ~O2rWriter();
    bool Open(const std::string& path, std::string* error);
    // One entry; the bytes are copied. Names use "/" (e.g. "rf3models/models.json").
    bool Add(const std::string& name, const std::vector<uint8_t>& data, std::string* error);
    // Adds portVersion (this build's version), writes the archive and moves it into place (or to <path>.new).
    bool Close(std::string* error);
    size_t Entries() const {
        return mEntries;
    }
    bool Pending() const {
        return mPending;
    }

  private:
    struct zip* mZip = nullptr;
    std::string mPath;
    std::string mTmp;
    size_t mEntries = 0;
    bool mPending = false;
};

} // namespace rf3disc
