// Reel Fishing III mod (step 76b, P3): see RF3ExtractModels.h. A port of tools/rf3_models.py's "models" path
// (read_chunks, parse_model, triangles, parse_skeleton, parse_motion, bone_worlds, skeleton_fit, lure_batches,
// skinned_batches, the checks, fish_to_json, model_to_json, hook_swatch, parse_mtp, fish_move_params, fish_models,
// cmd_models); the formats are in the project's docs/RESEARCH.md and RF3-MECHANICS.md. The Python tool stays the
// reference: tools/rf3_extract_check.py compares the two outputs entry by entry, so the float operations keep
// Python's order.

#include "RF3ExtractModels.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>

namespace rf3extract {

namespace {

using Bytes = std::vector<uint8_t>;
using Vec3 = std::array<double, 3>;
using Vec4 = std::array<double, 4>;
using Mat3 = std::array<Vec3, 3>;

struct KindInfo {
    const char* kind;
    const char* folder;
    const char* pattern; // printf pattern of the file name
    int count;
};
const KindInfo kModelKinds[] = {
    { "lure", "model/lure/", "lur%03d.gbo", 91 },        { "fly", "model/fly/", "fly%03d.gml", 40 },
    { "bait", "model/baithook/", "baithook%03d.gml", 12 }, { "float", "model/float/", "float%03d.gml", 6 },
    { "hook", "model/hook/", "hook%03d.gml", 7 },         { "baitmenu", "model/bait/", "bt%03d.gml", 12 },
};
constexpr int kBatchVerts = 32;
constexpr int kPosScale = 10000;
constexpr int kRodCount = 10;
constexpr int kMaxTexels = 4096;
constexpr uint32_t kTexTypeRgba32 = 1;
constexpr uint32_t kTexFlagLoadAsRaw = 1;
constexpr uint32_t kOtex = 0x4F544558;
constexpr int kFishHeader = 16;
constexpr int kFishRecord = 176;
const char* const kHookAtlas = "model/common/hook.p2g";
constexpr int kHookSwatch[4] = { 224, 0, 32, 64 };
constexpr int kHookTexelsPerM = 8000;
const int kPartMotions[] = { 9, 10, 11, 13, 14, 18, 17, 27, 27 };
const int kPatternCodes[][3] = { { 60, 0, 0x0C },  { 2, 1, 0x0C },   { 3, 2, 0x0C },   { 4, 3, 0x0C },
                                 { 15, 4, 0x0C },  { 16, 5, 0x0C },  { 28, 6, 0x04 },  { -1, 7, 0x0C },
                                 { -1, 8, 0x0C },  { 61, 9, 0x0E },  { 62, 10, 0x0E }, { -1, 11, 0x0C },
                                 { -1, 12, 0x0C }, { -1, 13, 0x0C }, { -1, 14, 0x0C }, { 63, 15, 0x12 },
                                 { 64, 16, 0x12 }, { 65, 17, 0x12 }, { 66, 18, 0x12 } };
const char* const kMtpPath = "model/fish/fmot00.mtp";
const char* const kMtpKeys[] = { "time", "mot_no", "course", "mot_speed", "mot_chg_speed",
                                 "move_speed", "each", "each_speed", "bank" };
// mouth, dorsal, "2", belly, anal, chest, ho, open, close, gills (rf3_models.py EACH_NAMES)
const char32_t kEachNames[] = { 0x53E3, 0x80CC, 0xFF12, 0x8179, 0x5C3B, 0x80F8, 0x30DB, 0x958B, 0x9589, 0x30A8 };
// left, right, up, down (COURSE_WORDS)
const char32_t kCourseWords[] = { 0x5DE6, 0x53F3, 0x4E0A, 0x4E0B };
const std::map<int, int> kExeFishModelsIndex = { { 41, 0 }, { 42, 1 } };
const char* const kExeFishModels[] = { "model/fish/f22koi_r/f22koi_r.gbo", "model/fish/f23koi_b/f23koi_b.gbo" };

// The Shift-JIS characters fmot00.mtp uses (the file decodes with no replacements); anything else stops the build.
const std::map<uint32_t, char32_t> kSjis = {
    { 0x8163, 0x2026 }, { 0x81A8, 0x2192 }, { 0x819A, 0x2605 }, { 0x8179, 0x3010 }, { 0x817A, 0x3011 },
    { 0x82AD, 0x304F }, { 0x82BB, 0x305D }, { 0x82C1, 0x3063 }, { 0x82CC, 0x306E }, { 0x82D1, 0x3073 },
    { 0x82D6, 0x3078 }, { 0x82E4, 0x3086 }, { 0x82E8, 0x308A }, { 0x82EA, 0x308C }, { 0x834E, 0x30AF },
    { 0x8356, 0x30B7 }, { 0x835F, 0x30C0 }, { 0x8362, 0x30C3 }, { 0x837A, 0x30DB }, { 0x8385, 0x30E5 },
    { 0x8FE3, 0x4E0A }, { 0x89BA, 0x4E0B }, { 0x9286, 0x4E2D }, { 0x904C, 0x4F38 }, { 0x90A8, 0x52E2 },
    { 0x94BD, 0x53CD }, { 0x8945, 0x53F3 }, { 0x8FEA, 0x5834 }, { 0x91E5, 0x5927 }, { 0x8E70, 0x59FF },
    { 0x8FAC, 0x5C0F }, { 0x904B, 0x5C3B }, { 0x8DB6, 0x5DE6 }, { 0x9378, 0x5EA6 }, { 0x88F8, 0x5F15 },
    { 0x8FB8, 0x6607 }, { 0x8E9E, 0x6642 }, { 0x9581, 0x666E }, { 0x965C, 0x66B4 }, { 0x90B3, 0x6B63 },
    { 0x8E80, 0x6B7B }, { 0x896A, 0x6CF3 }, { 0x956D, 0x7015 }, { 0x96B3, 0x7121 }, { 0x8CA1, 0x727D },
    { 0x92BC, 0x76F4 }, { 0x9477, 0x80CC }, { 0x95A0, 0x8179 }, { 0x935D, 0x8EE2 }, { 0x92CA, 0x901A },
    { 0x9756, 0x904A }, { 0x8D7E, 0x964D }, { 0x8149, 0xFF01 }, { 0x8169, 0xFF08 }, { 0x816A, 0xFF09 },
    { 0x8250, 0xFF11 }, { 0x8251, 0xFF12 }, { 0x8261, 0xFF22 }, { 0x826A, 0xFF2B },
};

// Python raises where these throw; BuildModels turns it into the error.
struct ModelError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
[[noreturn]] void Fail(const std::string& msg) {
    throw ModelError(msg);
}

std::string Hex(size_t v) {
    char b[24];
    std::snprintf(b, sizeof(b), "0x%zx", v);
    return b;
}

// Checked little-endian reads (struct.unpack_from raises past the end).
void Need(const Bytes& d, size_t o, size_t n) {
    if (o + n > d.size() || o + n < o) {
        Fail("read past the end at " + Hex(o));
    }
}
uint32_t RU32(const Bytes& d, size_t o) {
    Need(d, o, 4);
    return U32(d, o);
}
int32_t RI32(const Bytes& d, size_t o) {
    Need(d, o, 4);
    return I32(d, o);
}
uint16_t RU16(const Bytes& d, size_t o) {
    Need(d, o, 2);
    return U16(d, o);
}
double RF32(const Bytes& d, size_t o) {
    Need(d, o, 4);
    return F32(d, o);
}
std::string Tag(const Bytes& d, size_t o) {
    Need(d, o, 4);
    return std::string(d.begin() + o, d.begin() + o + 4);
}
// data[a:b].split(b"\0")[0], bytes >= 0x80 as U+FFFD (decode("ascii", "replace")).
std::string CString(const Bytes& d, size_t a, size_t b) {
    std::string s;
    for (size_t i = a; i < b && i < d.size() && d[i] != 0; i++) {
        if (d[i] < 0x80) {
            s.push_back(static_cast<char>(d[i]));
        } else {
            s += "\xEF\xBF\xBD";
        }
    }
    return s;
}

// round(x) for a float: an int, half to even; Python raises on inf and NaN.
int64_t Round(double x) {
    if (!std::isfinite(x)) {
        Fail("cannot round a non-finite value");
    }
    return PyRoundInt(x);
}

Json J(int64_t v) {
    return Json(v);
}
Json JDoubles(const double* v, size_t n, int ndigits) {
    Json a = Json::Array();
    for (size_t i = 0; i < n; i++) {
        a.Push(Json(PyRound(v[i], ndigits)));
    }
    return a;
}

std::string Lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}
std::string Basename(const std::string& p) {
    const size_t k = p.rfind('/');
    return k == std::string::npos ? p : p.substr(k + 1);
}
// os.path.splitext(name)[0] for a file name
std::string Stem(const std::string& name) {
    const size_t k = name.rfind('.');
    if (k == std::string::npos || name.find_first_not_of('.') >= k) {
        return name;
    }
    return name.substr(0, k);
}
bool StartsWith(const std::string& s, const std::string& p) {
    return s.compare(0, p.size(), p) == 0;
}
bool EndsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

// max(items, key=...) / min(...): the first best (a later item replaces it only when strictly better).
template <typename T, typename Key> T FirstMax(const std::vector<T>& items, Key key) {
    if (items.empty()) {
        Fail("max() of an empty sequence");
    }
    T best = items[0];
    double bk = key(best);
    for (size_t i = 1; i < items.size(); i++) {
        const double k = key(items[i]);
        if (k > bk) {
            best = items[i];
            bk = k;
        }
    }
    return best;
}
// max(a, b) of two floats: a unless b > a
double Max2(double a, double b) {
    return b > a ? b : a;
}

// ---------------------------------------------------------------- the disc's files

struct Disc {
    rf3disc::Pack* pack;
    std::set<std::string> files;
    std::vector<std::string> sorted; // pack.paths(): sorted

    Bytes Read(const std::string& path) {
        Bytes d;
        if (!pack->Read(path, &d)) {
            Fail("short read on " + path);
        }
        return d;
    }
    bool Has(const std::string& path) const {
        return files.count(path) != 0;
    }
};

// ---------------------------------------------------------------- models (25a)

struct Chunk {
    size_t o;
    uint32_t size;
    uint32_t count;
};
using Chunks = std::map<std::string, std::vector<Chunk>>;

struct Corner {
    uint16_t v, n, t;
    bool operator<(const Corner& o) const {
        return std::tie(v, n, t) < std::tie(o.v, o.n, o.t);
    }
    bool operator==(const Corner& o) const {
        return v == o.v && n == o.n && t == o.t;
    }
};
using Tri = std::array<Corner, 3>;

struct Surface {
    std::string name;
    std::optional<uint32_t> texture;
    std::optional<Vec4> colour;
    std::vector<Tri> tris;
    std::vector<std::vector<Corner>> strips;
};

struct Model {
    std::vector<std::string> textures;
    std::vector<Vec4> verts, norms, stq, vcol;
    std::vector<Surface> surfaces;
};

Chunks ReadChunks(const Bytes& d, const std::string& name) {
    const std::string magic = Tag(d, 0);
    if (magic != "GBOB" && magic != "GMLB") {
        Fail(name + ": magic " + magic);
    }
    if (RU32(d, 4) != d.size()) {
        Fail(name + ": header size differs from the file");
    }
    Chunks chunks;
    size_t o = 16;
    while (o + 16 <= d.size()) {
        const std::string tag = Tag(d, o);
        const uint32_t csize = U32(d, o + 4), count = U32(d, o + 8);
        if (csize < 16 || o + csize > d.size()) {
            Fail(name + ": bad chunk " + tag + " at " + Hex(o));
        }
        chunks[tag].push_back({ o, csize, count });
        o += csize;
    }
    if (o != d.size()) {
        Fail(name + ": chunks end at " + Hex(o) + ", the file at " + Hex(d.size()));
    }
    return chunks;
}

const Chunk& First(const Chunks& c, const char* tag, const std::string& name) {
    auto it = c.find(tag);
    if (it == c.end()) {
        Fail(name + ": no " + tag + " chunk");
    }
    return it->second[0];
}

std::vector<Vec4> Vec4s(const Bytes& d, const Chunk& c) {
    if (16 + 16ull * c.count != c.size) {
        Fail("vec4 chunk at " + Hex(c.o) + ": " + std::to_string(c.count) + " entries in " + std::to_string(c.size) +
             " bytes");
    }
    std::vector<Vec4> out(c.count);
    for (uint32_t i = 0; i < c.count; i++) {
        for (int k = 0; k < 4; k++) {
            out[i][k] = RF32(d, c.o + 16 + 16 * i + 4 * k);
        }
    }
    return out;
}

std::vector<Surface> ParseSurfaces(const Bytes& d, const Chunk& chunk, int cornerWords, bool hasUv, size_t nv,
                                   size_t nn, size_t nt, const std::string& name) {
    size_t q = chunk.o + 16;
    std::vector<Surface> out;
    const size_t cs = 2 * cornerWords;
    for (uint32_t si = 0; si < chunk.count; si++) {
        if (Tag(d, q) != "LSUR") {
            Fail(name + ": expected LSUR at " + Hex(q));
        }
        const size_t end = q + RU32(d, q + 4);
        Surface surf;
        surf.name = CString(d, q + 0x14, q + 0x24);
        std::map<uint32_t, std::vector<uint32_t>> keys; // raw words
        size_t o = q + 0x24;
        auto corner = [&](size_t p) {
            const uint16_t v = RU16(d, p), n = RU16(d, p + 2);
            const uint16_t t = hasUv ? RU16(d, p + 2 * (cornerWords - 1)) : 0;
            if (v >= nv || n >= nn || (hasUv && t >= nt)) {
                Fail(name + "/" + surf.name + ": corner at " + Hex(p) + " out of range");
            }
            return Corner{ v, n, t };
        };
        while (o < end) {
            const uint32_t key = RU32(d, o);
            o += 4;
            if (key == 128) {
                const uint32_t n = RU32(d, o);
                o += 4;
                for (uint32_t k = 0; k < n; k++) {
                    surf.tris.push_back({ corner(o), corner(o + cs), corner(o + 2 * cs) });
                    o += 3 * cs;
                }
                o = (o + 3) & ~size_t(3);
            } else if (key == 129) {
                const uint32_t n = RU32(d, o);
                o += 4;
                for (uint32_t k = 0; k < n; k++) {
                    const uint32_t m = RU32(d, o);
                    o += 4;
                    if (m < 3 || m > 4096) {
                        Fail(name + "/" + surf.name + ": strip of " + std::to_string(m) + " corners");
                    }
                    std::vector<Corner> s;
                    for (uint32_t c = 0; c < m; c++) {
                        s.push_back(corner(o + c * cs));
                    }
                    surf.strips.push_back(std::move(s));
                    o = (o + m * cs + 3) & ~size_t(3);
                }
            } else if (key == 0 && std::all_of(d.begin() + std::min(o - 4, d.size()),
                                                d.begin() + std::min(end, d.size()), [](uint8_t b) { return b == 0; })) {
                o = end; // the block is padded with zeros
            } else if (key == 2 || key == 3 || key == 4 || key == 9 || key == 14 || key == 15 || key == 64 ||
                       key == 65) {
                const int nw = key == 4 ? 4 : 1;
                std::vector<uint32_t> val;
                for (int k = 0; k < nw; k++) {
                    val.push_back(RU32(d, o + 4 * k));
                }
                keys[key] = val;
                o += 4 * nw;
            } else {
                Fail(name + "/" + surf.name + ": unknown LSUR key " + std::to_string(key) + " at " + Hex(o - 4));
            }
        }
        if (o != end) {
            Fail(name + "/" + surf.name + ": LSUR data ends at " + Hex(o) + ", the block at " + Hex(end));
        }
        if (keys.count(2)) {
            surf.texture = keys[2][0];
        }
        if (keys.count(4)) {
            Vec4 c;
            for (int k = 0; k < 4; k++) {
                const uint32_t u = keys[4][k];
                float f;
                std::memcpy(&f, &u, 4);
                c[k] = f;
            }
            surf.colour = c;
        }
        if ((keys.count(64) && keys[64][0] != surf.tris.size()) ||
            (keys.count(65) && keys[65][0] != surf.strips.size())) {
            Fail(name + "/" + surf.name + ": the header's triangle / strip counts differ from the data");
        }
        out.push_back(std::move(surf));
        q = end;
    }
    if (q != chunk.o + chunk.size) {
        Fail(name + ": LSUR blocks end at " + Hex(q) + ", SURF at " + Hex(chunk.o + chunk.size));
    }
    return out;
}

Model ParseModel(const Bytes& d, const Chunks& chunks, const std::string& name) {
    Model m;
    auto it = chunks.find("TEXT");
    if (it != chunks.end()) {
        const Chunk& c = it->second[0];
        for (uint32_t i = 0; i < c.count; i++) {
            Need(d, c.o + 16 + 16 * i, 16);
            std::string s;
            for (size_t k = c.o + 16 + 16 * i; k < c.o + 32 + 16 * i && d[k] != 0; k++) {
                if (d[k] >= 0x80) {
                    Fail(name + ": a texture name is not ASCII");
                }
                s.push_back(static_cast<char>(d[k]));
            }
            m.textures.push_back(s);
        }
    }
    m.verts = Vec4s(d, First(chunks, "VERT", name));
    m.norms = Vec4s(d, First(chunks, "NORM", name));
    const bool hasUv = chunks.count("STQ_") != 0;
    const bool hasCol = chunks.count("VCOL") != 0;
    if (hasUv) {
        m.stq = Vec4s(d, chunks.at("STQ_")[0]);
    }
    if (hasCol) {
        m.vcol = Vec4s(d, chunks.at("VCOL")[0]);
    }
    const int cornerWords = 2 + (hasCol ? 1 : 0) + (hasUv ? 1 : 0);
    m.surfaces = ParseSurfaces(d, First(chunks, "SURF", name), cornerWords, hasUv, m.verts.size(), m.norms.size(),
                               m.stq.size(), name);
    return m;
}

// triangles(): the list triangles reversed, then the strips unrolled (degenerate ones dropped, odd ones flipped).
std::vector<Tri> Triangles(const Surface& s) {
    std::vector<Tri> out;
    for (const Tri& t : s.tris) {
        out.push_back({ t[2], t[1], t[0] });
    }
    for (const auto& st : s.strips) {
        for (size_t i = 0; i + 2 < st.size(); i++) {
            const Corner &a = st[i], &b = st[i + 1], &c = st[i + 2];
            if (a.v == b.v || b.v == c.v || a.v == c.v) {
                continue;
            }
            if (i % 2 == 0) {
                out.push_back({ c, b, a });
            } else {
                out.push_back({ c, a, b });
            }
        }
    }
    return out;
}

// bounds(): (min, max) per axis
std::array<std::array<double, 2>, 3> Bounds(const Model& m) {
    if (m.verts.empty()) {
        Fail("min() of an empty sequence");
    }
    std::array<std::array<double, 2>, 3> b;
    for (int i = 0; i < 3; i++) {
        double lo = m.verts[0][i], hi = m.verts[0][i];
        for (const Vec4& v : m.verts) {
            if (v[i] < lo) {
                lo = v[i];
            }
            if (v[i] > hi) {
                hi = v[i];
            }
        }
        b[i] = { lo, hi };
    }
    return b;
}

// ---------------------------------------------------------------- skeleton and motions (25b)

struct Bone {
    uint32_t nv;
    int32_t sibling, child;
    Vec3 t;
    Vec4 rot;
    std::array<int32_t, 4> w60;
    std::vector<uint16_t> verts;
};

std::vector<Bone> ParseBones(const Bytes& d, const Chunks& chunks, std::vector<Vec4>* tvrt, const std::string& name) {
    if (!chunks.count("BONE") || !chunks.count("TVRT")) {
        Fail(name + ": no skeleton");
    }
    const Chunk& c = chunks.at("BONE")[0];
    std::vector<Bone> bones;
    for (uint32_t i = 0; i < c.count; i++) {
        const size_t r = c.o + RU32(d, c.o + 16 + 4 * i);
        Bone b;
        b.nv = RU32(d, r);
        b.sibling = RI32(d, r + 4);
        b.child = RI32(d, r + 8);
        for (int k = 0; k < 3; k++) {
            b.t[k] = RF32(d, r + 0x10 + 4 * k);
        }
        for (int k = 0; k < 4; k++) {
            b.rot[k] = RF32(d, r + 0x30 + 4 * k);
            b.w60[k] = RI32(d, r + 0x60 + 4 * k);
        }
        for (size_t k = r + 0x70;; k += 2) {
            const uint16_t v = RU16(d, k);
            if (v == 0xFFFF) {
                break;
            }
            b.verts.push_back(v);
        }
        bones.push_back(std::move(b));
    }
    *tvrt = Vec4s(d, chunks.at("TVRT")[0]);
    return bones;
}

struct Block {
    double dt;
    int32_t kind;
    uint32_t w;
    std::optional<int> start;
};
struct Motion {
    std::optional<int> start;
    std::optional<int> lastKey;
    std::map<uint32_t, double> durations;
    int32_t terminator;
};
struct Motions {
    std::vector<Vec3> T;
    std::vector<Vec4> R;
    std::vector<uint16_t> tidx, ridx;
    std::vector<Motion> motions;
};

std::optional<Motions> ParseMotion(const Bytes& d, const Chunks& chunks) {
    if (!chunks.count("MOTN")) {
        return std::nullopt;
    }
    const size_t o = chunks.at("MOTN")[0].o;
    auto g = [&](size_t off) { return RU32(d, o + off); };
    Motions mo;
    const uint32_t nBlocks = g(0x14), nMotions = g(0x18);
    for (uint32_t i = 0; i < g(0x34); i++) {
        Vec3 t;
        for (int k = 0; k < 3; k++) {
            t[k] = RF32(d, o + g(0x30) + 16 * i + 4 * k);
        }
        mo.T.push_back(t);
    }
    for (uint32_t i = 0; i < g(0x3c); i++) {
        Vec4 r;
        for (int k = 0; k < 4; k++) {
            r[k] = RF32(d, o + g(0x38) + 16 * i + 4 * k);
        }
        mo.R.push_back(r);
    }
    for (uint32_t i = 0; i < g(0x44); i++) {
        mo.tidx.push_back(RU16(d, o + g(0x40) + 2 * i));
    }
    for (uint32_t i = 0; i < g(0x4c); i++) {
        mo.ridx.push_back(RU16(d, o + g(0x48) + 2 * i));
    }
    std::vector<int32_t> blockTab;
    for (uint32_t i = 0; i < nBlocks + nMotions; i++) {
        blockTab.push_back(RI32(d, o + g(0x24) + 4 * i));
    }
    for (uint32_t m = 0; m < nMotions; m++) {
        const uint32_t p = RU32(d, o + g(0x20) + 4 * m);
        if (p < g(0x24)) {
            Fail("a motion before the block table");
        }
        size_t k = (p - g(0x24)) / 4;
        std::vector<Block> blocks;
        auto at = [&](size_t i) {
            if (i >= blockTab.size()) {
                Fail("a motion runs past the block table");
            }
            return blockTab[i];
        };
        while (at(k) > 0) {
            const size_t b = o + blockTab[k];
            Block bl{ RF32(d, b), RI32(d, b + 4), RU32(d, b + 8), std::nullopt };
            if (bl.kind == 2) {
                bl.start = RU16(d, b + 16);
            }
            blocks.push_back(bl);
            k++;
        }
        Motion mot;
        if (!blocks.empty() && blocks[0].kind == 2) {
            mot.start = blocks[0].start;
            mot.durations[0] = blocks[0].dt;
        }
        if (!blocks.empty() && blocks.back().kind == 1 && (blocks.back().w >> 16) == 1) {
            mot.lastKey = static_cast<int>(blocks.back().w & 0xFFFF);
        }
        for (size_t i = 1; i + 1 < blocks.size(); i++) {
            if (blocks[i].kind == 0) {
                mot.durations[blocks[i].w] = blocks[i].dt;
            }
        }
        mot.terminator = blockTab[k];
        mo.motions.push_back(mot);
    }
    return mo;
}

Mat3 Identity() {
    return { Vec3{ 1.0, 0.0, 0.0 }, Vec3{ 0.0, 1.0, 0.0 }, Vec3{ 0.0, 0.0, 1.0 } };
}

Mat3 AxisAngle(const double* axis, double angle) {
    double x = axis[0], y = axis[1], z = axis[2];
    const double ln = std::sqrt(x * x + y * y + z * z);
    if (ln < 1e-9 || std::fabs(angle) < 1e-12) {
        return Identity();
    }
    x = x / ln;
    y = y / ln;
    z = z / ln;
    const double c = std::cos(angle), s = std::sin(angle), k = 1.0 - std::cos(angle);
    return { Vec3{ c + x * x * k, x * y * k - z * s, x * z * k + y * s },
             Vec3{ y * x * k + z * s, c + y * y * k, y * z * k - x * s },
             Vec3{ z * x * k - y * s, z * y * k + x * s, c + z * z * k } };
}

Mat3 MatMul(const Mat3& a, const Mat3& b) {
    Mat3 r;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            r[i][j] = PySum(3, [&](size_t k) { return a[i][k] * b[k][j]; });
        }
    }
    return r;
}

Vec3 MatApply(const Mat3& m, const Vec3& t, const double* p) {
    Vec3 r;
    for (int i = 0; i < 3; i++) {
        r[i] = m[i][0] * p[0] + m[i][1] * p[1] + m[i][2] * p[2] + t[i];
    }
    return r;
}

struct World {
    Mat3 m;
    Vec3 t;
};

// local(i, &t, &axis, &angle) in RF3's convention
using LocalFn = std::function<void(int, Vec3*, Vec3*, double*)>;

std::vector<World> BoneWorlds(const std::vector<Bone>& bones, const LocalFn& local) {
    std::vector<World> out(bones.size());
    std::vector<bool> done(bones.size(), false);
    struct Item {
        int i;
        Mat3 pm;
        Vec3 pt;
    };
    std::vector<Item> stack{ { 0, Identity(), Vec3{ 0.0, 0.0, 0.0 } } };
    while (!stack.empty()) {
        const Item it = stack.back();
        stack.pop_back();
        int i = it.i;
        while (i >= 0) {
            if (static_cast<size_t>(i) >= bones.size() || done[i]) {
                Fail("bone " + std::to_string(i) + " out of range or reached twice");
            }
            Vec3 t, axis;
            double angle;
            local(i, &t, &axis, &angle);
            const Mat3 m = MatMul(it.pm, AxisAngle(axis.data(), -angle)); // negated: PS2 row-vector rotations
            const Vec3 wt = MatApply(it.pm, it.pt, t.data());
            out[i] = { m, wt };
            done[i] = true;
            if (bones[i].child >= 0) {
                stack.push_back({ bones[i].child, m, wt });
            }
            i = bones[i].sibling;
        }
    }
    if (std::find(done.begin(), done.end(), false) != done.end()) {
        Fail("a bone is not reached from the root");
    }
    return out;
}

LocalFn BindLocal(const std::vector<Bone>& bones) {
    return [&bones](int i, Vec3* t, Vec3* axis, double* angle) {
        *t = bones[i].t;
        *axis = { bones[i].rot[0], bones[i].rot[1], bones[i].rot[2] };
        *angle = bones[i].rot[3];
    };
}

template <typename T> const T& At(const std::vector<T>& v, size_t i) {
    if (i >= v.size()) {
        Fail("index " + std::to_string(i) + " out of range");
    }
    return v[i];
}

LocalFn FrameLocal(const Motions& mo, size_t fpb, size_t frame) {
    return [&mo, fpb, frame](int i, Vec3* t, Vec3* axis, double* angle) {
        const Vec4& r = At(mo.R, At(mo.ridx, i * fpb + frame));
        *t = At(mo.T, At(mo.tidx, i * fpb + frame));
        *axis = { r[0], r[1], r[2] };
        *angle = r[3];
    };
}

// skeleton_fit: VERT against the bind pose, the mean offset and the largest residual after removing it.
void SkeletonFit(const Model& m, const std::vector<Bone>& bones, const std::vector<Vec4>& tvrt, Vec3* mean,
                 double* resid) {
    const std::vector<World> worlds = BoneWorlds(bones, BindLocal(bones));
    std::vector<std::optional<Vec3>> pos(tvrt.size());
    for (size_t i = 0; i < bones.size(); i++) {
        for (uint16_t v : bones[i].verts) {
            const Vec3 p = MatApply(worlds[i].m, worlds[i].t, At(tvrt, v).data());
            pos[v] = Vec3{ p[0] + 0.0, p[1] + 0.0, p[2] + 0.0 };
        }
    }
    std::vector<Vec3> diffs;
    for (size_t v = 0; v < pos.size(); v++) {
        if (pos[v]) {
            const Vec4& mv = At(m.verts, v);
            diffs.push_back({ mv[0] - (*pos[v])[0], mv[1] - (*pos[v])[1], mv[2] - (*pos[v])[2] });
        }
    }
    if (diffs.empty()) {
        Fail("division by zero (no bone vertices)");
    }
    for (int k = 0; k < 3; k++) {
        (*mean)[k] = PySum(diffs.size(), [&](size_t i) { return diffs[i][k]; }) / static_cast<double>(diffs.size());
    }
    bool first = true;
    double best = 0;
    for (const Vec3& d : diffs) {
        double r = std::fabs(d[0] - (*mean)[0]);
        r = Max2(r, std::fabs(d[1] - (*mean)[1]));
        r = Max2(r, std::fabs(d[2] - (*mean)[2]));
        if (first || r > best) {
            best = r;
            first = false;
        }
    }
    *resid = best;
}

std::vector<int> BoneParents(const std::vector<Bone>& bones) {
    std::vector<int> parent(bones.size(), -1);
    for (size_t i = 0; i < bones.size(); i++) {
        int k = bones[i].child;
        size_t guard = 0;
        while (k >= 0) {
            if (static_cast<size_t>(k) >= bones.size() || ++guard > bones.size()) {
                Fail("bad bone link");
            }
            parent[k] = static_cast<int>(i);
            k = bones[k].sibling;
        }
    }
    return parent;
}

std::vector<int> Children(const std::vector<Bone>& bones, int i) {
    std::vector<int> out;
    for (int k = bones[i].child; k >= 0; k = bones[k].sibling) {
        if (static_cast<size_t>(k) >= bones.size() || out.size() > bones.size()) {
            Fail("bad bone link");
        }
        out.push_back(k);
    }
    return out;
}

std::vector<int> FishSpine(const std::vector<Bone>& bones, const std::vector<World>& worlds) {
    std::vector<int> chain;
    for (int k : Children(bones, 0)) {
        if (!bones[k].verts.empty()) {
            chain.push_back(k);
            break;
        }
    }
    if (chain.empty()) {
        Fail("no spine start");
    }
    while (true) {
        std::vector<int> cands;
        for (int k : Children(bones, chain.back())) {
            if (!bones[k].verts.empty() && std::fabs(worlds[k].t[0]) < 0.01 &&
                worlds[k].t[2] > worlds[chain.back()].t[2] + 1e-4) {
                cands.push_back(k);
            }
        }
        if (cands.empty()) {
            return chain;
        }
        chain.push_back(FirstMax(cands, [&](int k) { return worlds[k].t[2]; }));
    }
}

std::map<int, int> FishLocators(const std::vector<Bone>& bones) {
    std::map<int, int> loc;
    for (size_t i = 0; i < bones.size(); i++) {
        if (bones[i].w60[1] == 3) {
            loc[bones[i].w60[2]] = static_cast<int>(i);
        }
    }
    return loc;
}

Json LocatorsJson(const std::map<int, int>& loc) {
    Json j = Json::Object();
    for (const auto& kv : loc) {
        j[std::to_string(kv.first)] = Json(kv.second);
    }
    return j;
}

void Subtree(const std::vector<Bone>& bones, int i, std::vector<int>* out, int depth = 0) {
    if (depth > static_cast<int>(bones.size())) {
        Fail("bone loop");
    }
    out->push_back(i);
    for (int k : Children(bones, i)) {
        Subtree(bones, k, out, depth + 1);
    }
}

Json FishParts(const std::vector<Bone>& bones, size_t nMotions) {
    const std::map<int, int> loc = FishLocators(bones);
    Json parts = Json::Array();
    for (int i = 0; i < static_cast<int>(std::size(kPartMotions)); i++) {
        auto it = loc.find(3 + i);
        if (it != loc.end() && static_cast<size_t>(kPartMotions[i]) < nMotions) {
            std::vector<int> sub;
            Subtree(bones, it->second, &sub);
            Json p = Json::Object();
            p["ctrl"] = Json(i);
            p["role"] = Json(3 + i);
            p["bone"] = Json(it->second);
            p["bones"] = Json::ArrayOf(sub);
            p["motion"] = Json(kPartMotions[i]);
            parts.Push(std::move(p));
        }
    }
    return parts;
}

// ---------------------------------------------------------------- batches (25c-1, 25e-1)

struct TexInfo {
    std::string path;
    int w, h, dw, dh, scale;
};
using TexDecl = std::optional<std::pair<int, int>>;

struct Batch {
    int surface;
    std::vector<std::array<int64_t, 8>> verts;
    std::vector<std::vector<int>> tris;
    std::vector<std::array<int, 2>> groups;
};

std::array<int64_t, 2> TexCoord(const Model& m, const TexDecl& decl, uint16_t t) {
    std::array<int64_t, 2> tc{ 0, 0 };
    if (decl && !m.stq.empty()) {
        const Vec4& st = At(m.stq, t);
        double q = st[2];
        if (q == 0) {
            q = 1.0;
        }
        tc = { Round(st[0] / q * decl->first * 32), Round(st[1] / q * decl->second * 32) };
    }
    return tc;
}

TexDecl DeclOf(const Surface& s, const std::vector<TexDecl>& texDecl) {
    if (s.texture && *s.texture < texDecl.size()) {
        return texDecl[*s.texture];
    }
    return std::nullopt;
}

std::vector<Batch> LureBatches(const Model& m, const std::vector<TexDecl>& texDecl) {
    std::vector<Batch> out;
    for (size_t si = 0; si < m.surfaces.size(); si++) {
        const Surface& s = m.surfaces[si];
        const TexDecl decl = DeclOf(s, texDecl);
        std::optional<Batch> batch;
        std::map<Corner, int> index;
        for (const Tri& tri : Triangles(s)) {
            std::set<Corner> fresh;
            for (const Corner& c : tri) {
                if (!index.count(c)) {
                    fresh.insert(c);
                }
            }
            if (!batch || batch->verts.size() + fresh.size() > kBatchVerts) {
                if (batch) {
                    out.push_back(std::move(*batch));
                }
                batch = Batch{ static_cast<int>(si), {}, {}, {} };
                index.clear();
            }
            std::vector<int> idx;
            for (const Corner& c : tri) {
                if (!index.count(c)) {
                    const Vec4& p = At(m.verts, c.v);
                    const Vec4& nm = At(m.norms, c.n);
                    const double nx = nm[0], ny = nm[1], nz = nm[2];
                    double ln = std::sqrt(nx * nx + ny * ny + nz * nz);
                    if (ln == 0) {
                        ln = 1.0;
                    }
                    const auto tc = TexCoord(m, decl, c.t);
                    index[c] = static_cast<int>(batch->verts.size());
                    batch->verts.push_back({ Round(p[0] * kPosScale), Round(p[1] * kPosScale), Round(p[2] * kPosScale),
                                             tc[0], tc[1], Round(nx / ln * 127), Round(ny / ln * 127),
                                             Round(nz / ln * 127) });
                }
                idx.push_back(index[c]);
            }
            batch->tris.push_back(idx);
        }
        if (batch) {
            out.push_back(std::move(*batch));
        }
    }
    return out;
}

std::vector<Batch> SkinnedBatches(const Model& m, const std::vector<Bone>& bones, const std::vector<Vec4>& tvrt,
                                  const std::vector<World>& worlds, const std::vector<TexDecl>& texDecl) {
    std::vector<int> owner(65536, -1);
    for (size_t i = 0; i < bones.size(); i++) {
        for (uint16_t v : bones[i].verts) {
            owner[v] = static_cast<int>(i);
        }
    }
    auto ownerOf = [&](uint16_t v) {
        if (owner[v] < 0) {
            Fail("vertex " + std::to_string(v) + " has no bone");
        }
        return owner[v];
    };
    std::vector<Batch> out;
    for (size_t si = 0; si < m.surfaces.size(); si++) {
        const Surface& s = m.surfaces[si];
        const TexDecl decl = DeclOf(s, texDecl);
        std::optional<std::pair<std::set<Corner>, std::vector<Tri>>> current;
        auto flush = [&]() {
            if (!current || current->second.empty()) {
                return;
            }
            std::vector<Corner> corners(current->first.begin(), current->first.end());
            std::stable_sort(corners.begin(), corners.end(), [&](const Corner& a, const Corner& b) {
                const int oa = ownerOf(a.v), ob = ownerOf(b.v);
                return oa != ob ? oa < ob : a < b;
            });
            std::map<Corner, int> slot;
            for (size_t k = 0; k < corners.size(); k++) {
                slot[corners[k]] = static_cast<int>(k);
            }
            Batch b{ static_cast<int>(si), {}, {}, {} };
            for (const Corner& c : corners) {
                const int o = ownerOf(c.v);
                if (!b.groups.empty() && b.groups.back()[0] == o) {
                    b.groups.back()[1]++;
                } else {
                    b.groups.push_back({ o, 1 });
                }
            }
            for (const Corner& c : corners) {
                const Vec4& p = At(tvrt, c.v);
                const Mat3& rm = worlds[ownerOf(c.v)].m;
                const Vec4& nm = At(m.norms, c.n);
                const double nx = nm[0], ny = nm[1], nz = nm[2];
                // bind world -> bone local: R^T n
                const double lx = rm[0][0] * nx + rm[1][0] * ny + rm[2][0] * nz;
                const double ly = rm[0][1] * nx + rm[1][1] * ny + rm[2][1] * nz;
                const double lz = rm[0][2] * nx + rm[1][2] * ny + rm[2][2] * nz;
                double ln = std::sqrt(lx * lx + ly * ly + lz * lz);
                if (ln == 0) {
                    ln = 1.0;
                }
                const auto tc = TexCoord(m, decl, c.t);
                b.verts.push_back({ Round(p[0] * kPosScale), Round(p[1] * kPosScale), Round(p[2] * kPosScale), tc[0],
                                    tc[1], Round(lx / ln * 127), Round(ly / ln * 127), Round(lz / ln * 127) });
            }
            for (const Tri& tri : current->second) {
                b.tris.push_back({ slot[tri[0]], slot[tri[1]], slot[tri[2]] });
            }
            out.push_back(std::move(b));
        };
        for (const Tri& tri : Triangles(s)) {
            if (current) {
                size_t n = current->first.size();
                std::set<Corner> fresh;
                for (const Corner& c : tri) {
                    if (!current->first.count(c)) {
                        fresh.insert(c);
                    }
                }
                n += fresh.size();
                if (n > kBatchVerts) {
                    flush();
                    current.reset();
                }
            }
            if (!current) {
                current.emplace();
            }
            current->first.insert(tri.begin(), tri.end());
            current->second.push_back(tri);
        }
        flush();
    }
    return out;
}

Json BatchJson(const Batch& b, bool skinned) {
    Json j = Json::Object();
    j["surface"] = Json(b.surface);
    if (skinned) {
        Json g = Json::Array();
        for (const auto& x : b.groups) {
            Json p = Json::Array();
            p.Push(Json(x[0]));
            p.Push(Json(x[1]));
            g.Push(std::move(p));
        }
        j["groups"] = std::move(g);
    }
    Json verts = Json::Array();
    for (const auto& v : b.verts) {
        Json a = Json::Array();
        for (int64_t c : v) {
            a.Push(J(c));
        }
        verts.Push(std::move(a));
    }
    j["verts"] = std::move(verts);
    Json tris = Json::Array();
    for (const auto& t : b.tris) {
        tris.Push(Json::ArrayOf(t));
    }
    j["tris"] = std::move(tris);
    return j;
}

bool S16(int64_t c) {
    return c >= -32768 && c <= 32767;
}

// check_lure_json / the first half of check_fish_json: sizes, groups, index ranges, s16.
void CheckBatches(const std::vector<Batch>& batches, size_t nSurfaces, bool skinned, const std::string& name) {
    for (const Batch& b : batches) {
        size_t grouped = 0;
        for (const auto& g : b.groups) {
            grouped += g[1];
        }
        if (b.verts.empty() || b.verts.size() > kBatchVerts || (skinned && grouped != b.verts.size())) {
            Fail(name + ": a batch of " + std::to_string(b.verts.size()) + " vertices");
        }
        if (!skinned && (b.surface < 0 || static_cast<size_t>(b.surface) >= nSurfaces)) {
            Fail(name + ": batch surface " + std::to_string(b.surface));
        }
        for (const auto& t : b.tris) {
            for (int i : t) {
                if (i < 0 || static_cast<size_t>(i) >= b.verts.size()) {
                    Fail(name + ": a triangle index out of range");
                }
            }
        }
        for (const auto& v : b.verts) {
            for (int k = 0; k < 5; k++) {
                if (!S16(v[k])) {
                    Fail(name + ": a vertex out of s16 range");
                }
            }
        }
    }
}

// check_fish_json's bind check: the largest distance (per axis) from a VERT position to the nearest emitted
// bind-posed batch vertex (rounded to 0.1 mm, as Python's keys).
double BindError(const std::vector<Batch>& batches, const Model& m, const std::vector<World>& worlds,
                 const Vec3& offset) {
    std::vector<Vec3> emitted;
    for (const Batch& b : batches) {
        size_t k = 0;
        for (const auto& g : b.groups) {
            const World& w = At(worlds, g[0]);
            for (size_t i = k; i < k + g[1]; i++) {
                const double local[3] = { static_cast<double>(b.verts[i][0]) / kPosScale,
                                          static_cast<double>(b.verts[i][1]) / kPosScale,
                                          static_cast<double>(b.verts[i][2]) / kPosScale };
                Vec3 p = MatApply(w.m, w.t, local);
                emitted.push_back({ PyRound(p[0] + offset[0], 4), PyRound(p[1] + offset[1], 4),
                                    PyRound(p[2] + offset[2], 4) });
            }
            k += g[1];
        }
    }
    double posErr = 0.0;
    for (const Vec4& vv : m.verts) {
        double best = 1e9;
        bool first = true;
        for (const Vec3& e : emitted) {
            double d = std::fabs(vv[0] - e[0]);
            d = Max2(d, std::fabs(vv[1] - e[1]));
            d = Max2(d, std::fabs(vv[2] - e[2]));
            if (first || d < best) {
                best = d;
                first = false;
            }
        }
        posErr = Max2(posErr, best);
    }
    return posErr;
}

Json SurfacesJson(const Model& m, const std::vector<std::optional<TexInfo>>& tex) {
    Json out = Json::Array();
    for (const Surface& s : m.surfaces) {
        const std::optional<TexInfo>* x = nullptr;
        if (s.texture && *s.texture < tex.size() && tex[*s.texture]) {
            x = &tex[*s.texture];
        }
        const Vec4 col = s.colour ? *s.colour : Vec4{ 128.0, 128.0, 128.0, 128.0 };
        Json j = Json::Object();
        j["name"] = Json(s.name);
        if (x) {
            j["texture"] = Json((*x)->path);
            Json ts = Json::Array();
            ts.Push(Json((*x)->dw));
            ts.Push(Json((*x)->dh));
            j["tex_size"] = std::move(ts);
            j["hd_scale"] = Json((*x)->scale);
        } else {
            j["texture"] = Json(nullptr);
            j["tex_size"] = Json(nullptr);
            j["hd_scale"] = Json(nullptr);
        }
        Json c = Json::Array();
        for (double v : col) {
            c.Push(J(std::min<int64_t>(255, Round(v * 255 / 128))));
        }
        j["colour"] = std::move(c);
        out.Push(std::move(j));
    }
    return out;
}

std::vector<TexDecl> Decls(const std::vector<std::optional<TexInfo>>& tex) {
    std::vector<TexDecl> d;
    for (const auto& x : tex) {
        d.push_back(x ? TexDecl(std::make_pair(x->dw, x->dh)) : std::nullopt);
    }
    return d;
}

// ---------------------------------------------------------------- textures

// hd_scale: the smallest power-of-two scale that brings w x h under 4096 texels, the width a multiple of 4.
bool HdScale(int w, int h, int* scale) {
    int s = 1;
    while ((w / s) * (h / s) > kMaxTexels || (w / s) % 4) {
        s *= 2;
        if (w / s < 4 || w % s || h % s) {
            return false;
        }
    }
    *scale = s;
    return true;
}

// texture_v1: a LUS texture resource, version 1, loaded raw at the full size with the HD scales.
Bytes TextureV1(int w, int h, int scale, const Bytes& rgba) {
    Bytes out(0x40, 0);
    auto put32 = [&out](uint32_t v) {
        for (int k = 0; k < 4; k++) {
            out.push_back(static_cast<uint8_t>(v >> (8 * k)));
        }
    };
    auto set32 = [&out](size_t at, uint32_t v) {
        for (int k = 0; k < 4; k++) {
            out[at + k] = static_cast<uint8_t>(v >> (8 * k));
        }
    };
    set32(4, kOtex);
    set32(8, 1);
    set32(12, 0xDEADBEEF);
    set32(16, 0xDEADBEEF);
    const float fs = static_cast<float>(scale);
    uint32_t us;
    std::memcpy(&us, &fs, 4);
    put32(kTexTypeRgba32);
    put32(static_cast<uint32_t>(w));
    put32(static_cast<uint32_t>(h));
    put32(kTexFlagLoadAsRaw);
    put32(us);
    put32(us);
    put32(static_cast<uint32_t>(rgba.size()));
    out.insert(out.end(), rgba.begin(), rgba.end());
    return out;
}

struct Stage {
    Disc disc;
    std::map<std::string, std::optional<TexInfo>> textures; // .p2g path (or the swatch key) -> staged texture
    std::map<std::string, Bytes> files;                      // archive path -> bytes

    std::optional<std::string> FindTexture(const std::string& folder, const std::string& name) const {
        const std::string want = Lower(folder + name);
        for (const std::string& p : disc.sorted) {
            if (StartsWith(p, folder) && Lower(p) == want) {
                return p;
            }
        }
        const std::string tail = "/" + Lower(name);
        for (const std::string& p : disc.sorted) {
            if (StartsWith(p, "model/common/") && EndsWith(Lower(p), tail)) {
                return p;
            }
        }
        return std::nullopt;
    }

    // staged_texture_name (66b): the file name, lower case; a name another source already took gets its folder.
    std::string StagedName(const std::string& kind, const std::string& src) const {
        std::string name = Lower(Stem(Basename(src)));
        const std::string want = "textures/rf3models/" + kind + "/" + name;
        for (const auto& kv : textures) {
            if (kv.first != src && kv.second && kv.second->path == want) {
                std::vector<std::string> parts;
                size_t a = 0;
                for (size_t b; (b = src.find('/', a)) != std::string::npos; a = b + 1) {
                    parts.push_back(src.substr(a, b - a));
                }
                parts.push_back(src.substr(a));
                if (parts.size() < 2) {
                    Fail("list index out of range");
                }
                name = Lower(parts[parts.size() - 2]) + "_" + name;
                break;
            }
        }
        return name;
    }

    // Decode and stage src (path kind/name, file written under texDir). false if it doesn't decode or has no
    // HD scale (model_to_json's ValueError); fish_to_json treats that as fatal.
    bool StageTexture(const std::string& src, const std::string& kind, const std::string& texDir) {
        P2G p2g;
        std::string err;
        int scale = 0;
        if (!ParseP2g(disc.Read(src), src, &p2g, &err) || !HdScale(p2g.width, p2g.height, &scale)) {
            return false;
        }
        const std::string name = StagedName(kind, src);
        files[texDir + "/" + name] = TextureV1(p2g.width, p2g.height, scale, P2gRgba(p2g));
        textures[src] = TexInfo{ "textures/rf3models/" + kind + "/" + name, p2g.width, p2g.height,
                                 p2g.width / scale, p2g.height / scale, scale };
        return true;
    }
};

// hook_swatch (step 37): a bare hook gets UVs projected on x/y and hook.p2g's grey swatch as texture 0.
std::optional<TexInfo> HookSwatch(Stage& st, Model& m, const std::string& texDir) {
    const std::string key = std::string(kHookAtlas) + "#swatch";
    if (!st.textures.count(key)) {
        P2G p2g;
        std::string err;
        if (!ParseP2g(st.disc.Read(kHookAtlas), kHookAtlas, &p2g, &err)) {
            Fail(err);
        }
        const Bytes rgba = P2gRgba(p2g);
        const int x0 = kHookSwatch[0], y0 = kHookSwatch[1], w = kHookSwatch[2], h = kHookSwatch[3];
        Bytes crop;
        for (int y = 0; y < h; y++) {
            const size_t a = (static_cast<size_t>(y0 + y) * p2g.width + x0) * 4;
            const size_t b = (static_cast<size_t>(y0 + y) * p2g.width + x0 + w) * 4;
            if (b > rgba.size()) {
                Fail("the hook swatch lies outside hook.p2g");
            }
            crop.insert(crop.end(), rgba.begin() + a, rgba.begin() + b);
        }
        st.files[texDir + "/hook_swatch"] = TextureV1(w, h, 1, crop);
        st.textures[key] = TexInfo{ "textures/rf3models/hook/hook_swatch", w, h, w, h, 1 };
    }
    const TexInfo t = *st.textures[key];
    const auto b = Bounds(m);
    const double lx = b[0][0], ly = b[1][0];
    m.stq.clear();
    for (const Vec4& v : m.verts) {
        m.stq.push_back({ (v[0] - lx) * kHookTexelsPerM / t.w, (v[1] - ly) * kHookTexelsPerM / t.h, 1.0, 0.0 });
    }
    for (Surface& s : m.surfaces) {
        for (Tri& tri : s.tris) {
            for (Corner& c : tri) {
                c.t = c.v;
            }
        }
        for (auto& strip : s.strips) {
            for (Corner& c : strip) {
                c.t = c.v;
            }
        }
        s.texture = 0;
    }
    return t;
}

// model_to_json: one rigid model; appends its manifest entry.
void ModelToJson(Stage& st, const std::string& kind, int i, const std::string& path, const std::string& folder,
                 Json* manifest) {
    const Bytes data = st.disc.Read(path);
    Model m = ParseModel(data, ReadChunks(data, path), path);
    const std::string texDir = "textures/rf3models/" + kind;
    std::vector<std::optional<TexInfo>> tex;
    if (kind == "hook" && m.textures.empty()) {
        tex.push_back(HookSwatch(st, m, texDir));
    } else {
        for (const std::string& t : m.textures) {
            const auto src = st.FindTexture(folder, t);
            if (!src) {
                tex.push_back(std::nullopt);
                continue;
            }
            if (!st.textures.count(*src) && !st.StageTexture(*src, kind, texDir)) {
                st.textures[*src] = std::nullopt; // not decoded: drawn untextured
            }
            tex.push_back(st.textures[*src]);
        }
    }
    const std::string stem = Stem(Basename(path));
    const auto b = Bounds(m);
    Json j = Json::Object();
    j["kind"] = Json(kind);
    j["index"] = Json(i);
    j["source"] = Json(path);
    j["pos_scale"] = Json(kPosScale);
    Json size = Json::Array(), center = Json::Array();
    for (const auto& lh : b) {
        size.Push(Json(PyRound(lh[1] - lh[0], 4)));
        center.Push(J(Round((lh[0] + lh[1]) / 2 * kPosScale)));
    }
    j["size_m"] = size;
    j["center"] = center;
    const std::vector<Batch> batches = LureBatches(m, Decls(tex));
    j["surfaces"] = SurfacesJson(m, tex);
    Json bj = Json::Array();
    size_t nv = 0, nt = 0;
    for (const Batch& x : batches) {
        bj.Push(BatchJson(x, false));
        nv += x.verts.size();
        nt += x.tris.size();
    }
    j["batches"] = std::move(bj);
    CheckBatches(batches, m.surfaces.size(), false, kind + " " + std::to_string(i));
    const std::string file = "rf3models/" + kind + "/" + stem + ".json";
    const std::string text = DumpJsonCompact(j);
    st.files[file] = Bytes(text.begin(), text.end());
    Json e = Json::Object();
    e["index"] = Json(i);
    e["file"] = Json(file);
    e["verts"] = J(static_cast<int64_t>(nv));
    e["tris"] = J(static_cast<int64_t>(nt));
    e["batches"] = J(static_cast<int64_t>(batches.size()));
    e["size_m"] = size;
    e["center"] = center;
    manifest->Push(std::move(e));
}

Json MotionJson(const Motions& mo, size_t nBones) {
    Json j = Json::Object();
    j["frames"] = J(static_cast<int64_t>(mo.ridx.size() / nBones));
    Json t = Json::Array(), r = Json::Array();
    for (const Vec3& x : mo.T) {
        t.Push(JDoubles(x.data(), 3, 6));
    }
    for (const Vec4& x : mo.R) {
        r.Push(JDoubles(x.data(), 4, 6));
    }
    j["T"] = std::move(t);
    j["R"] = std::move(r);
    j["tidx"] = Json::ArrayOf(std::vector<int>(mo.tidx.begin(), mo.tidx.end()));
    j["ridx"] = Json::ArrayOf(std::vector<int>(mo.ridx.begin(), mo.ridx.end()));
    Json ms = Json::Array();
    for (const Motion& x : mo.motions) {
        Json m = Json::Object();
        if (x.start && x.lastKey) {
            m["start"] = Json(*x.start);
            m["keys"] = Json(*x.lastKey + 1);
            Json times = Json::Array();
            for (int k = 0; k < *x.lastKey; k++) {
                auto it = x.durations.find(static_cast<uint32_t>(k));
                times.Push(Json(PyRound(it != x.durations.end() ? it->second : 0.0, 4)));
            }
            m["times"] = std::move(times);
            m["loop"] = Json(x.terminator == -1);
        } else {
            m["start"] = Json(-1);
            m["keys"] = Json(0);
            m["times"] = Json::Array();
        }
        ms.Push(std::move(m));
    }
    j["motions"] = std::move(ms);
    return j;
}

struct SkinResult {
    Json info;
    std::string file;
    double bindErrMm;
};

// fish_to_json: one skinned model (a fish, a rod or a lure's skinned copy). Throws where Python records an error.
SkinResult SkinnedToJson(Stage& st, int record, const std::string& path, const std::string& kind,
                         const std::string& texDir) {
    const Bytes data = st.disc.Read(path);
    const Chunks chunks = ReadChunks(data, path);
    const Model m = ParseModel(data, chunks, path);
    std::vector<Vec4> tvrt;
    const std::vector<Bone> bones = ParseBones(data, chunks, &tvrt, path);
    const std::vector<World> worlds = BoneWorlds(bones, BindLocal(bones));
    Vec3 offset;
    double resid = 0;
    SkeletonFit(m, bones, tvrt, &offset, &resid);
    const std::string folder = path.substr(0, path.rfind('/')) + "/";
    std::vector<std::optional<TexInfo>> tex;
    for (const std::string& t : m.textures) {
        const auto src = st.FindTexture(folder, t);
        if (!src) {
            tex.push_back(std::nullopt);
            continue;
        }
        if (!st.textures.count(*src) && !st.StageTexture(*src, kind, texDir)) {
            Fail(path + ": texture " + *src + " does not decode");
        }
        tex.push_back(st.textures[*src]);
    }
    const std::vector<int> parents = BoneParents(bones);
    const auto b = Bounds(m);
    Json j = Json::Object();
    j["kind"] = Json(kind);
    j["model"] = Json(path);
    j["pos_scale"] = Json(kPosScale);
    Json size = Json::Array(), center = Json::Array(), off = Json::Array();
    for (const auto& lh : b) {
        size.Push(Json(PyRound(lh[1] - lh[0], 4)));
        center.Push(J(Round((lh[0] + lh[1]) / 2 * kPosScale)));
    }
    for (double x : offset) {
        off.Push(J(Round(x * kPosScale)));
    }
    const double lengthM = PyRound(b[2][1] - b[2][0], 4);
    j["size_m"] = std::move(size);
    j["center"] = std::move(center);
    j["offset"] = std::move(off);
    Json bj = Json::Array();
    for (size_t i = 0; i < bones.size(); i++) {
        Json x = Json::Object();
        x["parent"] = Json(parents[i]);
        x["t"] = JDoubles(bones[i].t.data(), 3, 6);
        x["axis"] = JDoubles(bones[i].rot.data(), 3, 6);
        x["angle"] = Json(PyRound(bones[i].rot[3], 6));
        bj.Push(std::move(x));
    }
    j["bones"] = std::move(bj);
    const std::vector<Batch> batches = SkinnedBatches(m, bones, tvrt, worlds, Decls(tex));
    j["surfaces"] = SurfacesJson(m, tex);
    Json batchJson = Json::Array();
    size_t nGroups = 0, nv = 0, nt = 0;
    for (const Batch& x : batches) {
        batchJson.Push(BatchJson(x, true));
        nGroups += x.groups.size();
        nv += x.verts.size();
        nt += x.tris.size();
    }
    j["batches"] = std::move(batchJson);

    const std::map<int, int> loc = FishLocators(bones);
    std::vector<int> spine, chain;
    int tip = -1;
    std::array<int64_t, 3> tipLocal{ 0, 0, 0 };
    Json chainZ = Json::Array();
    size_t menuPose = 0;
    std::optional<Motions> mo;
    bool hasMotion = false;
    double frame0 = 0;
    Json partsShort = Json::Array();
    if (kind == "fish") {
        spine = FishSpine(bones, worlds);
        j["spine"] = Json::ArrayOf(spine);
        j["mouth_bone"] = Json(loc.count(0) ? loc.at(0) : -1);
        j["locators"] = LocatorsJson(loc);
        // Step 25g-1: RF3's motions; table frame 0 must be the bind pose.
        mo = ParseMotion(data, chunks);
        if (mo && mo->ridx.size() % bones.size() == 0) {
            hasMotion = true;
            const size_t fpb = mo->ridx.size() / bones.size();
            j["motion"] = MotionJson(*mo, bones.size());
            bool first = true;
            for (size_t bi = 0; bi < bones.size(); bi++) {
                const Vec3& t = At(mo->T, At(mo->tidx, bi * fpb));
                double a = std::fabs(t[0] - bones[bi].t[0]);
                a = Max2(a, std::fabs(t[1] - bones[bi].t[1]));
                a = Max2(a, std::fabs(t[2] - bones[bi].t[2]));
                a = Max2(a, std::fabs(At(mo->R, At(mo->ridx, bi * fpb))[3] - bones[bi].rot[3]));
                if (first || a > frame0) {
                    frame0 = a;
                    first = false;
                }
            }
            j["motion_check_frame0"] = Json(PyRound(frame0, 6));
            Json parts = FishParts(bones, mo->motions.size());
            for (const Json& p : parts.Items()) {
                Json pair = Json::Array();
                pair.Push(*p.Find("ctrl"));
                pair.Push(*p.Find("bone"));
                partsShort.Push(std::move(pair));
            }
            j["parts"] = std::move(parts);
        }
    } else if (kind == "lure_skin") {
        // Step 27a: the lure's role bones (locator 0 = the hook), the body bone and its motions.
        j["locators"] = LocatorsJson(loc);
        j["hook_bone"] = Json(loc.count(0) ? loc.at(0) : -1);
        std::vector<int> all;
        for (size_t i = 0; i < bones.size(); i++) {
            all.push_back(static_cast<int>(i));
        }
        j["body_bone"] = Json(FirstMax(all, [&](int i) { return static_cast<double>(bones[i].nv); }));
        mo = ParseMotion(data, chunks);
        if (mo && mo->ridx.size() % bones.size() == 0) {
            hasMotion = true;
            j["motion"] = MotionJson(*mo, bones.size());
        }
    } else {
        // Rod: the bend chain (root to tip), the tip point in the tip bone's space, the chain bones' z, the menu pose.
        std::vector<int> all;
        for (size_t i = 0; i < bones.size(); i++) {
            all.push_back(static_cast<int>(i));
        }
        tip = FirstMax(all, [&](int i) { return worlds[i].t[2]; });
        chain = { tip };
        while (parents[chain.back()] >= 0) {
            chain.push_back(parents[chain.back()]);
            if (chain.size() > bones.size()) {
                Fail("bone loop");
            }
        }
        std::reverse(chain.begin(), chain.end());
        if (!bones[tip].verts.empty()) {
            std::vector<int> vs(bones[tip].verts.begin(), bones[tip].verts.end());
            const int far = FirstMax(vs, [&](int v) { return At(m.verts, v)[2]; });
            for (int k = 0; k < 3; k++) {
                tipLocal[k] = Round(At(tvrt, far)[k] * kPosScale);
            }
        }
        j["chain"] = Json::ArrayOf(chain);
        j["tip_bone"] = Json(tip);
        Json tl = Json::Array();
        for (int64_t c : tipLocal) {
            tl.Push(J(c));
        }
        j["tip_local"] = std::move(tl);
        for (int k : chain) {
            chainZ.Push(Json(PyRound(worlds[k].t[2], 4)));
        }
        j["chain_z"] = chainZ;
        // Step 39c: RF3's tackle-screen pose, motion 30's first frame.
        mo = ParseMotion(data, chunks);
        if (mo && mo->motions.size() > 30 && mo->motions[30].start) {
            const LocalFn local = FrameLocal(*mo, mo->ridx.size() / bones.size(), *mo->motions[30].start);
            Json pose = Json::Array();
            for (size_t i = 0; i < bones.size(); i++) {
                Vec3 t, axis;
                double angle;
                local(static_cast<int>(i), &t, &axis, &angle);
                Json x = Json::Object();
                x["t"] = JDoubles(t.data(), 3, 6);
                x["axis"] = JDoubles(axis.data(), 3, 6);
                x["angle"] = Json(PyRound(angle, 6));
                pose.Push(std::move(x));
            }
            menuPose = bones.size();
            j["menu_pose"] = std::move(pose);
        }
    }
    CheckBatches(batches, m.surfaces.size(), true, path);
    const double posErr = BindError(batches, m, worlds, offset);
    const std::string stem = Stem(Basename(path));
    SkinResult r;
    r.file = "rf3models/" + kind + "/" + stem + ".json";
    r.bindErrMm = PyRound(posErr * 1000, 2);
    Json& info = r.info;
    info["record"] = Json(record);
    info["file"] = Json(r.file);
    info["bones"] = J(static_cast<int64_t>(bones.size()));
    info["batches"] = J(static_cast<int64_t>(batches.size()));
    info["groups"] = J(static_cast<int64_t>(nGroups));
    info["verts"] = J(static_cast<int64_t>(nv));
    info["tris"] = J(static_cast<int64_t>(nt));
    info["bind_err_mm"] = Json(r.bindErrMm);
    info["fit_resid_mm"] = Json(PyRound(resid * 1000, 2));
    if (kind == "lure_skin") {
        info["hook_bone"] = Json(loc.count(0) ? loc.at(0) : -1);
        info["locators"] = LocatorsJson(loc);
        info["motions"] = J(hasMotion ? static_cast<int64_t>(mo->motions.size()) : 0);
    } else if (kind == "fish") {
        info["spine"] = Json::ArrayOf(spine);
        info["mouth_bone"] = Json(loc.count(0) ? loc.at(0) : -1);
        if (hasMotion) {
            info["motions"] = J(static_cast<int64_t>(mo->motions.size()));
            info["frames"] = J(static_cast<int64_t>(mo->ridx.size() / bones.size()));
            info["T"] = J(static_cast<int64_t>(mo->T.size()));
            info["R"] = J(static_cast<int64_t>(mo->R.size()));
            info["frame0_vs_bind"] = Json(PyRound(frame0, 6));
            info["parts"] = std::move(partsShort);
        }
    } else {
        info["chain"] = Json::ArrayOf(chain);
        info["tip_bone"] = Json(tip);
        Json tl = Json::Array();
        for (int64_t c : tipLocal) {
            tl.Push(J(c));
        }
        info["tip_local"] = std::move(tl);
        info["chain_z"] = std::move(chainZ);
        info["length_m"] = Json(lengthM);
        info["menu_pose"] = J(static_cast<int64_t>(menuPose));
    }
    const std::string text = DumpJsonCompact(j);
    st.files[r.file] = Bytes(text.begin(), text.end());
    return r;
}

// ---------------------------------------------------------------- motion patterns (26e-1) and movement

std::u32string DecodeSjis(const Bytes& d) {
    std::u32string out;
    for (size_t i = 0; i < d.size(); i++) {
        const uint8_t b = d[i];
        if (b < 0x80) {
            out.push_back(b);
        } else if (b >= 0xA1 && b <= 0xDF) {
            out.push_back(0xFF61 + (b - 0xA1));
        } else {
            const uint32_t code = i + 1 < d.size() ? (static_cast<uint32_t>(b) << 8) | d[i + 1] : b;
            auto it = kSjis.find(code);
            if (it == kSjis.end()) {
                Fail(std::string(kMtpPath) + ": Shift-JIS character " + Hex(code) + " not known");
            }
            out.push_back(it->second);
            i++;
        }
    }
    return out;
}

std::string Utf8(const std::u32string& s) {
    std::string out;
    for (char32_t c : s) {
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (c >> 18)));
            out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

// Python's str.isspace / \s
bool PySpace(char32_t c) {
    return (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x20) || c == 0x85 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}
std::u32string Strip(const std::u32string& s) {
    size_t a = 0, b = s.size();
    while (a < b && PySpace(s[a])) {
        a++;
    }
    while (b > a && PySpace(s[b - 1])) {
        b--;
    }
    return s.substr(a, b - a);
}
std::vector<std::u32string> SplitOn(const std::u32string& s, char32_t sep) {
    std::vector<std::u32string> out;
    size_t a = 0;
    for (size_t b; (b = s.find(sep, a)) != std::u32string::npos; a = b + 1) {
        out.push_back(s.substr(a, b - a));
    }
    out.push_back(s.substr(a));
    return out;
}
bool PyDigit(char32_t c) {
    return (c >= '0' && c <= '9') || (c >= 0xFF10 && c <= 0xFF19);
}
// The ASCII form of a number token (full-width digits as Python's float/int read them).
std::string NumberText(const std::u32string& s) {
    std::string out;
    for (char32_t c : Strip(s)) {
        if (c >= 0xFF10 && c <= 0xFF19) {
            out.push_back(static_cast<char>('0' + (c - 0xFF10)));
        } else if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else {
            Fail("not a number: " + Utf8(s));
        }
    }
    return out;
}
double PyFloat(const std::u32string& s) {
    std::string t = NumberText(s);
    bool neg = false;
    if (!t.empty() && (t[0] == '+' || t[0] == '-')) {
        neg = t[0] == '-';
        t.erase(0, 1);
    }
    double v = 0;
    const auto r = std::from_chars(t.data(), t.data() + t.size(), v);
    if (t.empty() || r.ec != std::errc() || r.ptr != t.data() + t.size()) {
        Fail("could not convert to float: " + Utf8(s));
    }
    return neg ? -v : v;
}
int PyInt(const std::u32string& s) {
    std::string t = NumberText(s);
    if (!t.empty() && t[0] == '+') {
        t.erase(0, 1);
    }
    int v = 0;
    const auto r = std::from_chars(t.data(), t.data() + t.size(), v);
    if (t.empty() || r.ec != std::errc() || r.ptr != t.data() + t.size()) {
        Fail("invalid literal for int(): " + Utf8(s));
    }
    return v;
}

// parse_course: flags + up/down angle (default 45) + left/right angle (default 90)
void ParseCourse(const std::u32string& token, int* flags, double* updown, double* leftright) {
    *flags = 0;
    *updown = 45.0;
    *leftright = 90.0;
    for (int bit = 0; bit < 4; bit++) {
        const size_t k = token.find(kCourseWords[bit]);
        if (k == std::u32string::npos) {
            continue;
        }
        *flags |= 1 << bit;
        std::u32string digits;
        for (size_t i = k + 1; i < token.size() && (PyDigit(token[i]) || token[i] == '.'); i++) {
            digits.push_back(token[i]);
        }
        if (!digits.empty()) {
            (bit >= 2 ? *updown : *leftright) = PyFloat(digits);
        }
    }
}

Json ParseMtp(const std::u32string& raw) {
    // comments: /* ... */ (re.sub non-greedy, dot matches newlines)
    std::u32string text;
    for (size_t i = 0; i < raw.size();) {
        if (raw.compare(i, 2, U"/*") == 0) {
            const size_t e = raw.find(U"*/", i + 2);
            if (e != std::u32string::npos) {
                i = e + 2;
                continue;
            }
        }
        text.push_back(raw[i++]);
    }
    // blocks: re.split(r"\n(?=#)", "\n" + text)
    const std::u32string all = U"\n" + text;
    std::vector<std::u32string> blocks;
    size_t a = 0;
    for (size_t i = 0; i + 1 < all.size(); i++) {
        if (all[i] == '\n' && all[i + 1] == '#') {
            blocks.push_back(all.substr(a, i - a));
            a = i + 1;
        }
    }
    blocks.push_back(all.substr(a));
    Json patterns = Json::Array();
    for (const std::u32string& rawBlock : blocks) {
        const std::u32string block = Strip(rawBlock);
        if (block.empty() || block[0] != '#') {
            continue;
        }
        const std::vector<std::u32string> lines = SplitOn(block, '\n');
        std::u32string title = lines[0];
        title.erase(0, title.find_first_not_of(U'#') == std::u32string::npos ? title.size()
                                                                              : title.find_first_not_of(U'#'));
        title = Strip(title);
        std::map<std::string, std::vector<std::u32string>> fields;
        for (size_t li = 1; li < lines.size(); li++) {
            const std::u32string line = Strip(SplitOn(lines[li], '#')[0]);
            if (line.empty()) {
                continue;
            }
            size_t e = 0;
            while (e < line.size() && !PySpace(line[e])) {
                e++;
            }
            const std::string key = Utf8(line.substr(0, e));
            size_t r = e;
            while (r < line.size() && PySpace(line[r])) {
                r++;
            }
            if (r >= line.size() || std::find(std::begin(kMtpKeys), std::end(kMtpKeys), key) == std::end(kMtpKeys)) {
                continue; // not a keyword line, or no value (split(None, 1) gives one part)
            }
            std::vector<std::u32string> vals;
            std::u32string cur;
            for (size_t i = r; i <= line.size(); i++) {
                if (i == line.size() || line[i] == ',' || PySpace(line[i])) {
                    if (!cur.empty()) {
                        vals.push_back(cur);
                    }
                    cur.clear();
                } else {
                    cur.push_back(line[i]);
                }
            }
            fields[key] = vals;
        }
        if (!fields.count("time")) {
            continue;
        }
        const size_t n = fields["time"].size();
        for (const char* k : kMtpKeys) {
            if (fields[k].size() != n) {
                Fail("pattern " + Utf8(title) + ": keyword counts differ");
            }
        }
        Json steps = Json::Array();
        for (size_t i = 0; i < n; i++) {
            int flags;
            double updown, leftright;
            ParseCourse(fields["course"][i], &flags, &updown, &leftright);
            const std::u32string& each = fields["each"][i];
            int bits = 0;
            for (int b = 0; b < static_cast<int>(std::size(kEachNames)); b++) {
                if (each.find(kEachNames[b]) != std::u32string::npos) {
                    bits += 1 << b;
                }
            }
            Json s = Json::Object();
            s["time"] = Json(PyFloat(fields["time"][i]));
            s["mot"] = Json(PyInt(fields["mot_no"][i]));
            s["course"] = Json(flags);
            s["updown"] = Json(updown);
            s["leftright"] = Json(leftright);
            s["mot_speed"] = Json(PyFloat(fields["mot_speed"][i]));
            s["mot_chg_speed"] = Json(PyFloat(fields["mot_chg_speed"][i]));
            s["move_speed"] = Json(PyFloat(fields["move_speed"][i]));
            s["each_speed"] = Json(PyFloat(fields["each_speed"][i]));
            s["bank"] = Json(PyFloat(fields["bank"][i]));
            s["each"] = Json(Utf8(each));
            s["each_bits"] = Json(bits);
            steps.Push(std::move(s));
        }
        Json p = Json::Object();
        p["title"] = Json(Utf8(title));
        p["steps"] = std::move(steps);
        patterns.Push(std::move(p));
    }
    return patterns;
}

std::vector<Bytes> FishRecords(Disc& disc) {
    const Bytes d = disc.Read("etc/fishdata.fdt");
    if (Tag(d, 0) != "FISH" || RU32(d, 4) != d.size() || kFishHeader + RU32(d, 8) * kFishRecord != d.size()) {
        Fail("etc/fishdata.fdt: the header does not match the known layout");
    }
    std::vector<Bytes> out;
    for (uint32_t i = 0; i < U32(d, 8); i++) {
        out.emplace_back(d.begin() + kFishHeader + i * kFishRecord, d.begin() + kFishHeader + (i + 1) * kFishRecord);
    }
    return out;
}

// fish_move_params: per fishdata record, speed (m/s), speed ramp (s) and the class.
Json FishMove(const std::vector<Bytes>& records) {
    static const std::map<int, double> kRamp = { { 0, 1.0 }, { 1, 2.0 }, { 2, 4.0 }, { 3, 1.0 } };
    Json out = Json::Object();
    for (size_t i = 0; i < records.size(); i++) {
        const Bytes& r = records[i];
        const int cls = r[0x13];
        Json x = Json::Object();
        x["speed_mps"] = Json(PyRound(static_cast<double>(F32(r, 0x14)) * 0.075, 5));
        x["ramp_s"] = Json(kRamp.count(cls) ? kRamp.at(cls) : 1.0);
        x["class"] = Json(cls);
        out[std::to_string(i)] = std::move(x);
    }
    return out;
}

// fish_models: record -> model path (the fishdata model name minus its last letter; _s falls back to _n; 41/42 from
// the executable's table).
std::map<int, std::string> FishModels(const Stage& st, const std::vector<Bytes>& records) {
    std::map<int, std::string> out;
    for (size_t i = 0; i < records.size(); i++) {
        const std::string name = CString(records[i], 0x80, records[i].size());
        if (name.empty()) {
            auto it = kExeFishModelsIndex.find(static_cast<int>(i));
            if (it != kExeFishModelsIndex.end() && st.disc.Has(kExeFishModels[it->second])) {
                out[static_cast<int>(i)] = kExeFishModels[it->second];
            }
            continue;
        }
        std::string folder = name.substr(0, name.size() - 1);
        std::string path = "model/fish/" + folder + "/" + folder + ".gbo";
        if (!st.disc.Has(path) && EndsWith(folder, "_s")) {
            folder = folder.substr(0, folder.size() - 2) + "_n";
            path = "model/fish/" + folder + "/" + folder + ".gbo";
        }
        if (st.disc.Has(path)) {
            out[static_cast<int>(i)] = path;
        }
    }
    return out;
}

void Build(rf3disc::Pack& pack, std::vector<Entry>* out, const ProgressFn& progress) {
    Stage st;
    st.disc.pack = &pack;
    for (const auto& f : pack.Files()) {
        st.disc.files.insert(f.path);
        st.disc.sorted.push_back(f.path);
    }
    std::sort(st.disc.sorted.begin(), st.disc.sorted.end());

    const std::vector<Bytes> records = FishRecords(st.disc);
    const std::map<int, std::string> fishModels = FishModels(st, records);
    int total = kRodCount + static_cast<int>(fishModels.size());
    for (const KindInfo& k : kModelKinds) {
        total += k.count;
    }
    int done = 0;
    auto step = [&](const std::string& what) {
        if (progress) {
            progress(what, static_cast<float>(done) / static_cast<float>(total + 1));
        }
        done++;
    };

    Json manifest = Json::Object();
    for (const KindInfo& k : kModelKinds) {
        manifest[k.kind] = Json::Array();
    }
    for (const KindInfo& k : kModelKinds) {
        for (int i = 0; i < k.count; i++) {
            char file[64];
            std::snprintf(file, sizeof(file), k.pattern, i);
            const std::string path = std::string(k.folder) + file;
            step(path);
            if (!st.disc.Has(path)) {
                Fail(std::string(k.kind) + " " + std::to_string(i) + ": " + path + " missing");
            }
            Json& list = manifest[k.kind];
            ModelToJson(st, k.kind, i, path, k.folder, &list);
            if (std::string(k.kind) == "lure") {
                // Step 27a: the lure's skinned copy; a rig that doesn't reproduce the mesh is drawn rigid.
                SkinResult r = SkinnedToJson(st, i, path, "lure_skin", "textures/rf3models/lure");
                if (r.bindErrMm > 1.0) {
                    st.files.erase(r.file);
                    continue;
                }
                manifest[k.kind].Items().back()["skin"] = Json(r.file);
            }
        }
    }
    // Step 25e-1: fish, one file per model, and the record -> file map.
    manifest["fish"] = Json::Array();
    std::map<std::string, Json> fishFiles;
    Json fishRecords = Json::Object(), rods = Json::Object(); // record -> file, in the manifest's order
    for (const auto& rp : fishModels) {
        step(rp.second);
        auto it = fishFiles.find(rp.second);
        if (it != fishFiles.end()) {
            Json shared = it->second;
            shared["record"] = Json(rp.first);
            manifest["fish"].Push(std::move(shared));
            fishRecords[std::to_string(rp.first)] = *it->second.Find("file");
            continue;
        }
        SkinResult r = SkinnedToJson(st, rp.first, rp.second, "fish", "textures/rf3models/fish");
        fishFiles[rp.second] = r.info;
        manifest["fish"].Push(std::move(r.info));
        fishRecords[std::to_string(rp.first)] = Json(r.file);
    }
    // Step 25f-1: rods; RROD n uses model/rod/rod(n+1).gbo.
    manifest["rod"] = Json::Array();
    for (int rod = 0; rod < kRodCount; rod++) {
        char path[64];
        std::snprintf(path, sizeof(path), "model/rod/rod%02d.gbo", rod + 1);
        step(path);
        if (!st.disc.Has(path)) {
            Fail(std::string("rod ") + std::to_string(rod) + ": " + path + " missing");
        }
        SkinResult r = SkinnedToJson(st, rod, path, "rod", "textures/rf3models/rod");
        manifest["rod"].Push(std::move(r.info));
        rods[std::to_string(rod)] = Json(r.file);
    }
    step("rf3models/models.json");
    // Step 26e-1: the motion patterns (FMOT00.MTP), the code table and per-record movement values.
    const Json patterns = ParseMtp(DecodeSjis(st.disc.Read(kMtpPath)));
    if (patterns.Items().size() != std::size(kPatternCodes)) {
        Fail(std::string(kMtpPath) + ": " + std::to_string(patterns.Items().size()) +
             " patterns, the code table has " + std::to_string(std::size(kPatternCodes)));
    }
    Json m = Json::Object();
    m["version"] = Json(5);
    m["pos_scale"] = Json(kPosScale);
    m["kinds"] = manifest;
    m["fish_records"] = std::move(fishRecords);
    m["rods"] = std::move(rods);
    m["patterns"] = patterns;
    Json codes = Json::Array();
    for (const auto& c : kPatternCodes) {
        Json x = Json::Object();
        x["code"] = Json(c[0]);
        x["pattern"] = Json(c[1]);
        x["flags"] = Json(c[2]);
        codes.Push(std::move(x));
    }
    m["pattern_codes"] = std::move(codes);
    m["fish_move"] = FishMove(records);
    const std::string text = DumpJsonOrdered(m);
    st.files["rf3models/models.json"] = Bytes(text.begin(), text.end());
    for (auto& kv : st.files) {
        out->push_back({ kv.first, std::move(kv.second) });
    }
}

} // namespace

bool BuildModels(rf3disc::Pack& pack, std::vector<Entry>* out, std::string* error, const ProgressFn& progress) {
    try {
        Build(pack, out, progress);
    } catch (const ModelError& e) {
        *error = e.what();
        out->clear();
        return false;
    }
    return true;
}

} // namespace rf3extract
