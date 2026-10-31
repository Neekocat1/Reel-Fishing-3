#pragma once

// Reel Fishing III mod (step 76b): helpers shared by the in-game extractor's converters (RF3ExtractTackle.cpp, ...).
// They reproduce what the project's Python tools write, byte for byte (docs/DESIGN.md "Step 76 design"):
// - Json + DumpJson*: Python's json.dump (ensure_ascii, Python float repr), sorted, ordered or compact;
// - PyRound: Python's round(x) and round(x, n) (half to even, on the double's exact value);
// - TextureRgba32: the texture resource soh-o2r-packer makes from a name.rgba32.png.
// No SoH or game headers: the converters also build outside the game for checking.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace rf3extract {

struct Entry {
    std::string name; // the archive path, e.g. "rf3tackle/tackle.json"
    std::vector<uint8_t> data;
};

// step text and 0..1 within the converter
using ProgressFn = std::function<void(const std::string& step, float progress)>;

class Json {
  public:
    enum class Type { Null, Bool, Int, Float, Str, Arr, Obj };
    Json() = default;
    Json(std::nullptr_t) {
    }
    Json(bool b) : mType(Type::Bool), mBool(b) {
    }
    Json(int v) : mType(Type::Int), mInt(v) {
    }
    Json(int64_t v) : mType(Type::Int), mInt(v) {
    }
    Json(uint32_t v) : mType(Type::Int), mInt(v) {
    }
    Json(double v) : mType(Type::Float), mFloat(v) {
    }
    Json(const char* s) : mType(Type::Str), mStr(s) {
    }
    Json(std::string s) : mType(Type::Str), mStr(std::move(s)) {
    }
    static Json Array() {
        Json j;
        j.mType = Type::Arr;
        return j;
    }
    static Json Object() {
        Json j;
        j.mType = Type::Obj;
        return j;
    }
    template <typename T> static Json ArrayOf(const std::vector<T>& v) {
        Json j = Array();
        for (const T& x : v) {
            j.Push(Json(x));
        }
        return j;
    }
    Json& Push(Json v) {
        mArr.push_back(std::move(v));
        return mArr.back();
    }
    // Sets (or replaces) a key.
    Json& operator[](const std::string& key);
    const Json* Find(const std::string& key) const;
    Type GetType() const {
        return mType;
    }
    std::vector<Json>& Items() {
        return mArr;
    }
    const std::vector<Json>& Items() const {
        return mArr;
    }

  private:
    friend void DumpTo(const Json& j, int depth, int mode, std::string* out);
    Type mType = Type::Null;
    bool mBool = false;
    int64_t mInt = 0;
    double mFloat = 0;
    std::string mStr;
    std::vector<Json> mArr;
    std::vector<std::pair<std::string, Json>> mObj;
};

// json.dump as Python writes it (ensure_ascii; no trailing newline). Strings are UTF-8.
constexpr int kDumpSorted = 0, kDumpOrdered = 1, kDumpCompact = 2;
// indent=1, sort_keys=True
std::string DumpJson(const Json& j);
// indent=1, keys in insertion order
std::string DumpJsonOrdered(const Json& j);
// separators=(",", ":"), keys in insertion order
std::string DumpJsonCompact(const Json& j);
// Python's repr(float).
std::string PyFloatRepr(double v);
// Python's round(x) (an integer, half to even) and round(x, ndigits) (a float).
int64_t PyRoundInt(double x);
double PyRound(double x, int ndigits);

// soh-o2r-packer's resource for a name.rgba32.png: the 64-byte header (OTEX, version 0) then type 1 (RGBA32),
// width, height, size and the pixels.
std::vector<uint8_t> TextureRgba32(uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba);

// Python 3.13's sum() of floats (start 0): the first item, then Neumaier's compensated sum of the rest.
template <typename Get> double PySum(size_t n, Get get) {
    if (n == 0) {
        return 0.0;
    }
    double f = 0.0 + get(0);
    double c = 0.0;
    for (size_t i = 1; i < n; i++) {
        const double x = get(i);
        const double t = f + x;
        if (std::fabs(f) >= std::fabs(x)) {
            c += (f - t) + x;
        } else {
            c += (x - t) + f;
        }
        f = t;
    }
    if (c != 0.0 && std::isfinite(c)) {
        f += c;
    }
    return f;
}

using Rgba = std::array<uint8_t, 4>;

// A .p2g texture (rf3_menuassets.py parse_p2g): the palette as RGBA (alpha doubled, 0..255) and one index
// per pixel.
struct P2G {
    int width = 0;
    int height = 0;
    std::vector<Rgba> clut;
    std::vector<uint8_t> indices;
};
bool ParseP2g(const std::vector<uint8_t>& data, const std::string& name, P2G* t, std::string* error);
// p2g_rgba: flat RGBA bytes.
std::vector<uint8_t> P2gRgba(const P2G& t);

// Little-endian readers (bounds are the caller's job).
inline uint16_t U16(const std::vector<uint8_t>& d, size_t o) {
    return static_cast<uint16_t>(d[o] | (d[o + 1] << 8));
}
inline int16_t I16(const std::vector<uint8_t>& d, size_t o) {
    return static_cast<int16_t>(U16(d, o));
}
inline uint32_t U32(const std::vector<uint8_t>& d, size_t o) {
    return d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (static_cast<uint32_t>(d[o + 3]) << 24);
}
inline int32_t I32(const std::vector<uint8_t>& d, size_t o) {
    return static_cast<int32_t>(U32(d, o));
}
float F32(const std::vector<uint8_t>& d, size_t o);

// Python's bytes.find(needle, start): -1 if absent.
int64_t Find(const std::vector<uint8_t>& d, const char* needle, size_t start);

} // namespace rf3extract
