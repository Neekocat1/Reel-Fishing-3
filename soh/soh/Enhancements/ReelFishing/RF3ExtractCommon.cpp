// Reel Fishing III mod (step 76b): see RF3ExtractCommon.h.

#include "RF3ExtractCommon.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <system_error>

namespace rf3extract {

Json& Json::operator[](const std::string& key) {
    if (mType != Type::Obj) {
        mType = Type::Obj;
        mObj.clear();
    }
    for (auto& kv : mObj) {
        if (kv.first == key) {
            return kv.second;
        }
    }
    mObj.emplace_back(key, Json());
    return mObj.back().second;
}

const Json* Json::Find(const std::string& key) const {
    for (const auto& kv : mObj) {
        if (kv.first == key) {
            return &kv.second;
        }
    }
    return nullptr;
}

namespace {

// Python's json string encoder with ensure_ascii=True: everything outside ' '..'~' as \uXXXX (UTF-16 units).
// Strings are UTF-8; a byte that doesn't start a valid sequence counts as the code point of its value.
void DumpString(const std::string& s, std::string* out) {
    out->push_back('"');
    for (size_t i = 0; i < s.size();) {
        uint32_t c = static_cast<unsigned char>(s[i]);
        size_t len = 1;
        if (c >= 0xC0) {
            const size_t need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
            uint32_t cp = c & (need == 4 ? 0x07 : need == 3 ? 0x0F : 0x1F);
            bool ok = i + need <= s.size();
            for (size_t k = 1; ok && k < need; k++) {
                const unsigned char b = static_cast<unsigned char>(s[i + k]);
                ok = (b & 0xC0) == 0x80;
                cp = (cp << 6) | (b & 0x3F);
            }
            if (ok) {
                c = cp;
                len = need;
            }
        }
        i += len;
        switch (c) {
            case '"': *out += "\\\""; continue;
            case '\\': *out += "\\\\"; continue;
            case '\n': *out += "\\n"; continue;
            case '\r': *out += "\\r"; continue;
            case '\t': *out += "\\t"; continue;
            case '\b': *out += "\\b"; continue;
            case '\f': *out += "\\f"; continue;
            default: break;
        }
        char buf[16];
        if (c >= 0x10000) {
            const uint32_t v = c - 0x10000;
            std::snprintf(buf, sizeof(buf), "\\u%04x\\u%04x", 0xD800 + (v >> 10), 0xDC00 + (v & 0x3FF));
            *out += buf;
        } else if (c < 0x20 || c >= 0x7F) {
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            *out += buf;
        } else {
            out->push_back(static_cast<char>(c));
        }
    }
    out->push_back('"');
}

} // namespace

// mode: kDumpSorted (indent=1, sort_keys=True), kDumpOrdered (indent=1, insertion order) or kDumpCompact
// (separators=(",", ":"), insertion order).
void DumpTo(const Json& j, int depth, int mode, std::string* out) {
    const bool compact = mode == kDumpCompact;
    auto newline = [&](int d) {
        if (!compact) {
            *out += "\n";
            out->append(d, ' ');
        }
    };
    switch (j.mType) {
        case Json::Type::Null: *out += "null"; return;
        case Json::Type::Bool: *out += j.mBool ? "true" : "false"; return;
        case Json::Type::Int: *out += std::to_string(j.mInt); return;
        case Json::Type::Float: *out += PyFloatRepr(j.mFloat); return;
        case Json::Type::Str: DumpString(j.mStr, out); return;
        case Json::Type::Arr: {
            if (j.mArr.empty()) {
                *out += "[]";
                return;
            }
            *out += "[";
            for (size_t i = 0; i < j.mArr.size(); i++) {
                if (i) {
                    *out += ",";
                }
                newline(depth + 1);
                DumpTo(j.mArr[i], depth + 1, mode, out);
            }
            newline(depth);
            *out += "]";
            return;
        }
        case Json::Type::Obj: {
            if (j.mObj.empty()) {
                *out += "{}";
                return;
            }
            std::vector<const std::pair<std::string, Json>*> kv;
            for (const auto& p : j.mObj) {
                kv.push_back(&p);
            }
            if (mode == kDumpSorted) {
                std::sort(kv.begin(), kv.end(), [](const auto* a, const auto* b) { return a->first < b->first; });
            }
            *out += "{";
            for (size_t i = 0; i < kv.size(); i++) {
                if (i) {
                    *out += ",";
                }
                newline(depth + 1);
                DumpString(kv[i]->first, out);
                *out += compact ? ":" : ": ";
                DumpTo(kv[i]->second, depth + 1, mode, out);
            }
            newline(depth);
            *out += "}";
            return;
        }
    }
}

std::string DumpJson(const Json& j) {
    std::string out;
    DumpTo(j, 0, kDumpSorted, &out);
    return out;
}

std::string DumpJsonOrdered(const Json& j) {
    std::string out;
    DumpTo(j, 0, kDumpOrdered, &out);
    return out;
}

std::string DumpJsonCompact(const Json& j) {
    std::string out;
    DumpTo(j, 0, kDumpCompact, &out);
    return out;
}

std::string PyFloatRepr(double v) {
    if (std::isnan(v)) {
        return "NaN";
    }
    if (std::isinf(v)) {
        return v > 0 ? "Infinity" : "-Infinity";
    }
    if (v == 0) {
        return std::signbit(v) ? "-0.0" : "0.0";
    }
    // The shortest round-trip digits, as Python's repr uses, from the scientific form "d.ddde+XX".
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::scientific);
    std::string sci(buf, res.ptr);
    bool neg = sci[0] == '-';
    if (neg) {
        sci.erase(0, 1);
    }
    const size_t e = sci.find('e');
    std::string digits = sci.substr(0, e);
    digits.erase(std::remove(digits.begin(), digits.end(), '.'), digits.end());
    const int exp10 = std::stoi(sci.substr(e + 1));
    std::string out = neg ? "-" : "";
    if (exp10 >= -4 && exp10 < 16) {
        // fixed: digits with the point after exp10 + 1 of them
        const int point = exp10 + 1;
        if (point <= 0) {
            out += "0." + std::string(-point, '0') + digits;
        } else if (point >= static_cast<int>(digits.size())) {
            out += digits + std::string(point - digits.size(), '0') + ".0";
        } else {
            out += digits.substr(0, point) + "." + digits.substr(point);
        }
    } else {
        out += digits.substr(0, 1);
        if (digits.size() > 1) {
            out += "." + digits.substr(1);
        }
        char eb[16];
        std::snprintf(eb, sizeof(eb), "e%c%02d", exp10 < 0 ? '-' : '+', std::abs(exp10));
        out += eb;
    }
    return out;
}

int64_t PyRoundInt(double x) {
    return static_cast<int64_t>(std::nearbyint(x)); // the default rounding mode: half to even, as Python
}

double PyRound(double x, int ndigits) {
    // Python rounds the double's exact decimal value, ties to even. %.60f gives that exact value for the
    // magnitudes used here (lengths in mm; a double's fraction needs at most 52 decimals below 2), then the decimal
    // rounding is done by hand.
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%.60f", std::fabs(x));
    std::string s(buf);
    const size_t dot = s.find('.');
    std::string intPart = s.substr(0, dot);
    std::string frac = s.substr(dot + 1);
    std::string keep = intPart + frac.substr(0, ndigits);
    const std::string rest = frac.substr(ndigits);
    bool up = false;
    if (!rest.empty()) {
        if (rest[0] > '5') {
            up = true;
        } else if (rest[0] == '5') {
            const bool exactHalf = rest.find_first_not_of('0', 1) == std::string::npos;
            up = !exactHalf || ((keep.back() - '0') % 2 == 1);
        }
    }
    if (up) {
        int i = static_cast<int>(keep.size()) - 1;
        while (i >= 0 && keep[i] == '9') {
            keep[i--] = '0';
        }
        if (i < 0) {
            keep.insert(keep.begin(), '1');
        } else {
            keep[i]++;
        }
    }
    const std::string num = keep.substr(0, keep.size() - ndigits) + "." + keep.substr(keep.size() - ndigits);
    double r = 0;
    std::from_chars(num.data(), num.data() + num.size(), r);
    return std::signbit(x) ? -r : r; // round(-0.0, n) is -0.0, as in Python
}

std::vector<uint8_t> TextureRgba32(uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba) {
    std::vector<uint8_t> out(64, 0);
    auto put32 = [&out](size_t at, uint32_t v) {
        if (out.size() < at + 4) {
            out.resize(at + 4);
        }
        for (int k = 0; k < 4; k++) {
            out[at + k] = static_cast<uint8_t>(v >> (8 * k));
        }
    };
    put32(4, 0x4F544558); // "OTEX" as the resource type, little-endian
    put32(8, 0);          // version 0
    put32(12, 0xDEADBEEF); // the id torch's header leaves at 0xDEADBEEFDEADBEEF
    put32(16, 0xDEADBEEF);
    put32(64, 1); // TextureType::RGBA32bpp
    put32(68, width);
    put32(72, height);
    put32(76, static_cast<uint32_t>(rgba.size()));
    out.insert(out.end(), rgba.begin(), rgba.end());
    return out;
}

float F32(const std::vector<uint8_t>& d, size_t o) {
    uint32_t u = U32(d, o);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

int64_t Find(const std::vector<uint8_t>& d, const char* needle, size_t start) {
    const size_t n = std::strlen(needle);
    if (n == 0 || d.size() < n) {
        return -1;
    }
    for (size_t i = start; i + n <= d.size(); i++) {
        if (std::memcmp(d.data() + i, needle, n) == 0) {
            return static_cast<int64_t>(i);
        }
    }
    return -1;
}

// ---------------------------------------------------------------- .p2g textures (rf3_menuassets.py)

namespace {

constexpr uint32_t kPsmt8 = 0x13;
constexpr uint32_t kPsmt4 = 0x14;

std::vector<uint8_t> Unslid(const uint8_t* src, size_t srcLen, size_t outLen, size_t* used) {
    std::vector<uint8_t> win(4096, 0);
    uint32_t r = 0xFEE;
    std::vector<uint8_t> out;
    size_t i = 0;
    uint32_t flags = 0, bits = 0;
    while (out.size() < outLen && i < srcLen) {
        if (bits == 0) {
            flags = src[i++];
            bits = 8;
        }
        if (flags & 1) {
            const uint8_t c = src[i++];
            out.push_back(c);
            win[r] = c;
            r = (r + 1) & 0xFFF;
        } else {
            if (i + 1 >= srcLen) {
                break;
            }
            const uint32_t b0 = src[i], b1 = src[i + 1];
            i += 2;
            const uint32_t off = b0 | ((b1 & 0xF0) << 4);
            for (uint32_t k = 0; k < (b1 & 0xF) + 3; k++) {
                const uint8_t c = win[(off + k) & 0xFFF];
                out.push_back(c);
                win[r] = c;
                r = (r + 1) & 0xFFF;
            }
        }
        flags >>= 1;
        bits--;
    }
    if (out.size() > outLen) {
        out.resize(outLen);
    }
    *used = i;
    return out;
}

std::vector<uint8_t> Unswizzle8(const std::vector<uint8_t>& buf, int w, int h) {
    std::vector<uint8_t> out(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; y++) {
        const int blockRow = (y & ~0xF) * w;
        const int swap = (((y + 2) >> 2) & 1) * 4;
        const int colRow = ((((y & ~3) >> 1) + (y & 1)) & 7) * w * 2;
        const int byteY = (y >> 1) & 1;
        for (int x = 0; x < w; x++) {
            out[static_cast<size_t>(y) * w + x] =
                buf[blockRow + (x & ~0xF) * 2 + colRow + ((x + swap) & 7) * 4 + byteY + ((x >> 2) & 2)];
        }
    }
    return out;
}

int Block32(int bx, int by) {
    return (bx & 1) | ((by & 1) << 1) | ((bx & 2) << 1) | ((by & 2) << 2) | ((bx & 4) << 2);
}
int Column32(int x, int y) {
    return (x & 1) | ((y & 1) << 1) | ((x & 6) << 1) | ((y & 6) << 3);
}
int Block4(int bx, int by) {
    return (by & 1) | ((bx & 1) << 1) | ((by & 2) << 1) | ((bx & 2) << 2) | ((by & 4) << 2);
}
int Column4(int x, int y) {
    static const int base[8] = { 0, 8, 32, 40, 64, 72, 96, 104 };
    const int col = y >> 2, r = y & 3, g = x >> 3, k = x & 7;
    int v;
    if (((r >> 1) ^ (col & 1)) != 0) {
        v = base[(k + 4) & 7] + ((col & 1) == 0 ? 1 : 0);
    } else {
        v = base[k] + ((col & 1) ? 1 : 0);
    }
    return v + 16 * (r & 1) + 2 * g + 128 * col;
}

bool Unswizzle4(const std::vector<uint8_t>& buf, int w, int h, std::vector<uint8_t>* out) {
    const int w32 = w / 2;
    const int pagesWide = std::max(1, w / 128);
    std::vector<int32_t> addrToFile;
    for (int y32 = 0; y32 < h / 4; y32++) {
        for (int x32 = 0; x32 < w32; x32++) {
            const int page = (y32 / 32) * pagesWide + x32 / 64;
            const int word = Block32((x32 >> 3) & 7, (y32 >> 3) & 3) * 64 + Column32(x32 & 7, y32 & 7);
            const size_t base = static_cast<size_t>(page) * 8192 + word * 4;
            const int f = (y32 * w32 + x32) * 4;
            if (addrToFile.size() < base + 4) {
                addrToFile.resize(base + 4, -1);
            }
            for (int b = 0; b < 4; b++) {
                addrToFile[base + b] = f + b;
            }
        }
    }
    out->assign(static_cast<size_t>(w) * h, 0);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const int page = (y / 128) * pagesWide + x / 128;
            const size_t nib = static_cast<size_t>(page) * 16384 + Block4((x >> 5) & 3, (y >> 4) & 7) * 512 +
                               Column4(x & 31, y & 15);
            if ((nib >> 1) >= addrToFile.size() || addrToFile[nib >> 1] < 0 ||
                static_cast<size_t>(addrToFile[nib >> 1]) >= buf.size()) {
                return false;
            }
            const uint8_t byte = buf[addrToFile[nib >> 1]];
            (*out)[static_cast<size_t>(y) * w + x] = (nib & 1) ? (byte >> 4) : (byte & 0xF);
        }
    }
    return true;
}

} // namespace

bool ParseP2g(const std::vector<uint8_t>& data, const std::string& name, P2G* t, std::string* error) {
    if (data.size() < 0x20 || std::string(data.begin(), data.begin() + 4) != "PS2G") {
        *error = name + ": not PS2G";
        return false;
    }
    std::map<std::string, std::pair<size_t, uint32_t>> chunks;
    size_t o = 0x20;
    while (o + 8 <= data.size()) {
        const std::string tag(data.begin() + o, data.begin() + o + 4);
        const uint32_t size = U32(data, o + 4);
        if (size < 8) {
            break;
        }
        chunks[tag] = { o, size };
        o += size;
    }
    if (!chunks.count("INFO") || !chunks.count("CLUT") || !chunks.count("PICT")) {
        *error = name + ": a chunk is missing";
        return false;
    }
    const size_t io = chunks["INFO"].first + 8;
    const int w = static_cast<int>(U32(data, io + 4)), h = static_cast<int>(U32(data, io + 8));
    const uint32_t swizzle = U32(data, io + 32);
    const size_t co = chunks["CLUT"].first;
    const uint32_t ncol = U32(data, co + 12);
    std::vector<Rgba> clut;
    for (uint32_t k = 0; k < ncol; k++) {
        const size_t c = co + 0x10 + 4 * k;
        clut.push_back({ data[c], data[c + 1], data[c + 2], static_cast<uint8_t>(std::min(255, data[c + 3] * 2)) });
    }
    if (ncol == 256) {
        std::vector<Rgba> sw(256);
        for (int i = 0; i < 256; i++) {
            sw[i] = clut[(i & ~0x18) | ((i & 8) << 1) | ((i & 0x10) >> 1)];
        }
        clut = sw;
    }
    const size_t po = chunks["PICT"].first;
    const uint32_t psize = chunks["PICT"].second;
    const uint32_t psm = U32(data, po + 12);
    const bool compressed = std::string(data.begin() + po + 16, data.begin() + po + 20) == "SLID";
    const size_t need = psm == kPsmt8 ? static_cast<size_t>(w) * h : static_cast<size_t>(w) * h / 2;
    std::vector<uint8_t> pix;
    if (compressed) {
        const uint32_t csize = U32(data, po + 20), usize = U32(data, po + 24);
        size_t used = 0;
        pix = Unslid(data.data() + po + 32, data.size() - (po + 32), usize, &used);
        if (pix.size() != usize || used != csize - 16) {
            *error = name + ": SLID size mismatch";
            return false;
        }
    } else {
        pix.assign(data.begin() + po + 16, data.begin() + std::min(data.size(), po + psize));
    }
    if (pix.size() < need) {
        *error = name + ": too few pixel bytes";
        return false;
    }
    if (psm == kPsmt8) {
        t->indices = swizzle == 3 ? Unswizzle8(pix, w, h) : std::vector<uint8_t>(pix.begin(), pix.begin() + w * h);
    } else if (psm == kPsmt4) {
        if (swizzle == 3) {
            if (!Unswizzle4(pix, w, h, &t->indices)) {
                *error = name + ": a PSMT4 address outside the picture";
                return false;
            }
        } else {
            t->indices.clear();
            for (size_t i = 0; i < need; i++) {
                t->indices.push_back(pix[i] & 0xF);
                t->indices.push_back(pix[i] >> 4);
            }
        }
    } else {
        *error = name + ": pixel format not handled";
        return false;
    }
    t->width = w;
    t->height = h;
    t->clut = clut;
    return true;
}

std::vector<uint8_t> P2gRgba(const P2G& t) {
    std::vector<uint8_t> out;
    out.reserve(t.indices.size() * 4);
    for (uint8_t i : t.indices) {
        out.insert(out.end(), t.clut[i].begin(), t.clut[i].end());
    }
    return out;
}

} // namespace rf3extract
