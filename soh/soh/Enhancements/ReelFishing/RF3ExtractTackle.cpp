// Reel Fishing III mod (step 76b, P1): see RF3ExtractTackle.h. A port of tools/rf3_menuassets.py's "build" path
// (parse_p2g, parse_gps, parse_font, read_texts, tackle_data, pole_box, conditions_data, season_sizes, crop,
// arrow_icon, cmd_build); the formats are in the project's docs/RESEARCH.md and RF3-MECHANICS.md. The Python tool
// stays the reference: tools/rf3_extract_check.py compares the two outputs entry by entry.

#include "RF3ExtractTackle.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <tuple>

namespace rf3extract {

namespace {

const char* const kScreenLayouts[] = { "menu_01", "menu_02", "menu_03", "menu_04", "menu_05", "menu_06" };
constexpr int kDigitPage = 21, kDigitX = 3, kDigitY = 239, kDigitW = 12, kDigitH = 15, kDigitPitch = 18;
const char* const kDigitNames[] = { "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "minus", "plus" };
constexpr int kMaxTexels = 4096;
constexpr int kIconSize = 32;
constexpr int kFishRecords = 48;
constexpr int kNotUsable = 99;
constexpr int kBaitSizes = 5;
constexpr int kPoleRod = 10;

std::vector<uint8_t> Crop(const P2G& t, int x, int y, int w, int h) {
    std::vector<uint8_t> rgba;
    rgba.reserve(static_cast<size_t>(w) * h * 4);
    for (int yy = y; yy < y + h; yy++) {
        const int cy = std::min(std::max(yy, 0), t.height - 1);
        for (int xx = x; xx < x + w; xx++) {
            const int cx = std::min(std::max(xx, 0), t.width - 1);
            const Rgba& c = t.clut[t.indices[static_cast<size_t>(cy) * t.width + cx]];
            rgba.insert(rgba.end(), c.begin(), c.end());
        }
    }
    return rgba;
}

std::vector<uint8_t> PadWidth(const std::vector<uint8_t>& rgba, int w, int h, int tw) {
    if (tw == w) {
        return rgba;
    }
    std::vector<uint8_t> out;
    for (int y = 0; y < h; y++) {
        const auto row = rgba.begin() + static_cast<size_t>(y) * w * 4;
        out.insert(out.end(), row, row + w * 4);
        for (int k = 0; k < tw - w; k++) {
            out.insert(out.end(), row + (w - 1) * 4, row + w * 4);
        }
    }
    return out;
}

// ---------------------------------------------------------------- .gps layouts

struct Vertex {
    double u = 0, v = 0;
    uint32_t r = 0, g = 0, b = 0, a = 0;
    int32_t x = 0, y = 0;
};
struct Element {
    std::string tag;
    uint32_t index = 0;
    std::string name;
    uint32_t kind = 0;
    uint32_t count = 0;
    std::vector<std::pair<uint32_t, uint32_t>> texrefs;
    std::vector<Vertex> vertices;
    std::array<uint32_t, 6> head{};
};

std::vector<Element> ParseGps(const std::vector<uint8_t>& data) {
    std::vector<Element> elems;
    size_t o = 0x10;
    while (o + 0x20 <= data.size()) {
        Element e;
        e.tag = std::string(data.begin() + o, data.begin() + o + 4);
        const uint32_t size = U32(data, o + 4);
        e.index = U32(data, o + 8);
        if (size < 0x20) {
            break;
        }
        size_t n = 0;
        while (n < 16 && data[o + 0x10 + n] != 0) {
            n++;
        }
        e.name = std::string(data.begin() + o + 0x10, data.begin() + o + 0x10 + n);
        const std::vector<uint8_t> body(data.begin() + o + 0x20, data.begin() + std::min(data.size(), o + size));
        e.kind = U32(body, 0);
        e.count = U32(body, 4);
        for (int k = 0; k < 6; k++) {
            e.head[k] = U32(body, 8 + 4 * k);
        }
        size_t p = 0x20;
        const size_t step = e.tag == "LBOX" ? 16 : 8;
        for (uint32_t k = 0; k < e.count && p + step * k + 8 <= body.size(); k++) {
            e.texrefs.emplace_back(U32(body, p + step * k), U32(body, p + step * k + 4));
        }
        p = (p + step * e.count + 15) & ~static_cast<size_t>(15);
        while (p + 48 <= body.size()) {
            Vertex v;
            v.u = F32(body, p);
            v.v = F32(body, p + 4);
            v.r = U32(body, p + 16);
            v.g = U32(body, p + 20);
            v.b = U32(body, p + 24);
            v.a = U32(body, p + 28);
            v.x = I32(body, p + 32);
            v.y = I32(body, p + 36);
            e.vertices.push_back(v);
            p += 48;
        }
        elems.push_back(e);
        o += size;
    }
    return elems;
}

// ---------------------------------------------------------------- texts and records

using Texts = std::map<uint32_t, std::string>;

Texts ReadTexts(const std::vector<uint8_t>& data) {
    Texts texts;
    size_t o = 0x30;
    while (o + 16 <= data.size()) {
        const std::string tag(data.begin() + o, data.begin() + o + 4);
        const uint32_t size = U32(data, o + 4);
        if (tag != "TXTS" || size < 24) {
            break;
        }
        const size_t body = o + 16;
        const size_t bodyLen = std::min<size_t>(size - 16, data.size() - body);
        const uint32_t tid = U32(data, body + 16);
        std::string s;
        for (size_t p = 24; p + 2 <= bodyLen / 2 * 2; p += 2) {
            const uint16_t c = U16(data, body + p);
            if (c == 0) {
                break;
            }
            if (c == 0x1000) {
                s += '\n';
            } else if (c < 0x70) {
                s += static_cast<char>(c + 0x10);
            } else {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "<%x>", c);
                s += buf;
            }
        }
        texts[tid] = s;
        o += size;
    }
    return texts;
}

std::string Get(const Texts& t, uint32_t id, const std::string& fallback = "") {
    auto it = t.find(id);
    return it != t.end() ? it->second : fallback;
}

using Rec = std::vector<uint8_t>;
using Rig = std::map<std::string, std::vector<Rec>>;

Rig RigRecords(const std::vector<uint8_t>& d) {
    Rig out;
    const uint32_t count = U32(d, 8);
    size_t o = 16;
    for (uint32_t c = 0; c < count; c++) {
        const std::string tag(d.begin() + o, d.begin() + o + 4);
        const uint32_t csize = U32(d, o + 4), n = U32(d, o + 8);
        const size_t rsize = tag == "RROD" ? 164 : (csize - 16) / n;
        std::vector<Rec>& recs = out[tag];
        for (uint32_t i = 0; i < n; i++) {
            recs.emplace_back(d.begin() + o + 16 + i * rsize, d.begin() + o + 16 + (i + 1) * rsize);
        }
        o += csize;
    }
    return out;
}

std::pair<int, int> HeartShieldOffsets(const std::string& tag) {
    if (tag == "RROD") return { 0x44, 0x74 };
    if (tag == "RLIN") return { 0x24, 0x54 };
    if (tag == "RFLT") return { 0x20, 0x50 };
    return { 0x80, 0xB0 }; // RLRE, RFLY, RBIT
}

Json HeartShield(const std::string& tag, const Rec& rec, int target) {
    const auto hs = HeartShieldOffsets(tag);
    const int hv = static_cast<int8_t>(rec[hs.first + target]), sv = static_cast<int8_t>(rec[hs.second + target]);
    if (hv == kNotUsable || sv == kNotUsable) {
        return Json();
    }
    Json j = Json::Array();
    j.Push(Json(hv));
    j.Push(Json(sv));
    return j;
}

Json RawTable(const Rec& rec, int off) {
    Json j = Json::Array();
    for (int i = 0; i < kFishRecords; i++) {
        j.Push(Json(static_cast<int>(static_cast<int8_t>(rec[off + i]))));
    }
    return j;
}

Json Bytes(const Rec& rec, int off, int n) {
    Json j = Json::Array();
    for (int i = 0; i < n; i++) {
        j.Push(Json(static_cast<int>(rec[off + i])));
    }
    return j;
}

Json Values(const std::string& tag, const Rec& rec, const std::vector<int>& targets, const std::vector<int>* tableRecord) {
    Json j = Json::Object();
    for (size_t k = 0; k < targets.size(); k++) {
        j[std::to_string(targets[k])] = HeartShield(tag, rec, tableRecord ? (*tableRecord)[k] : targets[k]);
    }
    return j;
}

Json ItemEntry(const Rig& rig, const Texts& tk, const std::string& tag, int i, uint32_t nameBase,
               const std::vector<int>& targets) {
    const Rec& rec = rig.at(tag)[i];
    const auto hs = HeartShieldOffsets(tag);
    Json e = Json::Object();
    e["index"] = Json(i);
    e["name"] = Json(Get(tk, nameBase + i));
    e["values"] = Values(tag, rec, targets, nullptr);
    e["heart"] = RawTable(rec, hs.first);
    e["shield"] = RawTable(rec, hs.second);
    e["appeal"] = (tag == "RLRE" || tag == "RFLY" || tag == "RBIT") ? Bytes(rec, 0x50, kFishRecords) : Json::Array();
    if (tag == "RLRE") {
        Json m = Json::Object();
        m["reeled_action"] = Json(static_cast<int>(rec[0x0E]));
        m["still_action"] = Json(static_cast<int>(rec[0x12]));
        m["buoyancy"] = Json(static_cast<int>(rec[0x2C]));
        Json base = Json::Array(), rate = Json::Array();
        for (int k = 0; k < 4; k++) {
            base.Push(Json(I16(rec, 0x38 + 2 * k) / 100.0));
            rate.Push(Json(static_cast<double>(F32(rec, 0x40 + 4 * k))));
        }
        m["speed_base"] = base;
        m["speed_rate"] = rate;
        e["move"] = m;
    }
    if (tag == "RFLY") {
        e["sunk"] = Json(static_cast<int>(rec[0x0E]));
    }
    if (tag == "RLRE" || tag == "RFLY") {
        e["type"] = Json(static_cast<int>(I16(rec, 0x2C)));
    }
    static const std::map<std::string, uint32_t> kDescBase = {
        { "RLRE", 0x9D0 }, { "RFLY", 0xBD0 }, { "RBIT", 0xAF0 }, { "RFLT", 0xED0 }
    };
    e["desc"] = Json(Get(tk, kDescBase.at(tag) + (tag == "RBIT" ? i / kBaitSizes : i)));
    return e;
}

bool StartsWith(const std::string& s, const char* p) {
    return s.rfind(p, 0) == 0;
}

// tackle_data + pole_box: every box (step 19a) and OoT's pole as box 10 (step 23c).
bool TackleData(rf3disc::Pack& pack, Json* boxesOut, Texts* syOut, std::string* error) {
    std::vector<uint8_t> rigData, tkData, syData, fish;
    if (!pack.Read("etc/rigdata.fdt", &rigData) || !pack.Read("menu/tackle.trf", &tkData) ||
        !pack.Read("menu/system.trf", &syData) || !pack.Read("etc/fishdata.fdt", &fish)) {
        *error = "a tackle data file is missing on the disc";
        return false;
    }
    const Rig rig = RigRecords(rigData);
    const Texts tk = ReadTexts(tkData);
    const Texts sy = ReadTexts(syData);
    *syOut = sy;
    const uint32_t nfish = U32(fish, 8);
    std::vector<int> named;
    for (int i = 0; i < static_cast<int>(std::min<uint32_t>(nfish, 48)); i++) {
        if (!Get(tk, 0x40 + i).empty() && U16(fish, 16 + 176 * i + 6) != 0) {
            named.push_back(i);
        }
    }
    static const std::map<int, std::vector<uint32_t>> kReelByType = {
        { 2, { 0x510 } }, { 1, { 0x511, 0x512 } }, { 3, { 0x513, 0x514 } }, { 4, { 0x518 } }
    };
    static const std::map<int, std::vector<int>> kUserLines = { { 0, { 3, 7, 9, 12, 14 } }, { 1, { 5, 7, 8, 9, 12 } } };
    static const char* const kKinds[3] = { "lure", "fly", "bait" };
    static const char* const kBoxKeys[10] = { "bass",   "trout",  "salmon",  "fly",        "bluewater",
                                              "pole_s", "pole_l", "crucian", "allpurpose", "bitterling" };
    static const std::map<int, std::string> kBaitLayout = {
        { 5, "menu_03" }, { 6, "menu_03" }, { 7, "menu_03" }, { 8, "menu_04" }, { 9, "menu_05" }
    };
    Json boxes = Json::Array();
    std::map<int, int> reelsUsed;
    std::vector<int> bassLines;
    const std::vector<Rec>& rods = rig.at("RROD");
    for (int r = 0; r < static_cast<int>(rods.size()); r++) {
        const Rec& rod = rods[r];
        const int kind = rod[0x14], reelType = rod[0x12];
        uint32_t reelId = 0;
        auto rt = kReelByType.find(reelType);
        if (rt != kReelByType.end()) {
            reelId = rt->second[std::min<size_t>(reelsUsed[reelType], rt->second.size() - 1)];
        }
        reelsUsed[reelType]++;
        if (kind > 2) {
            continue;
        }
        if (r != static_cast<int>(boxes.Items().size())) {
            *error = "box " + std::to_string(r) + ": the boxes kept must be 0..n in rod order";
            return false;
        }
        std::vector<int> targets;
        for (int i : named) {
            if (HeartShield("RROD", rod, i).GetType() != Json::Type::Null) {
                targets.push_back(i);
            }
        }
        std::vector<int> lines;
        const std::vector<Rec>& rlin = rig.at("RLIN");
        for (int i = 0; i < static_cast<int>(rlin.size()); i++) {
            if ((U16(rlin[i], 0x10) >> r) & 1) {
                lines.push_back(i);
            }
        }
        if (lines.size() != 5) {
            *error = "rod " + std::to_string(r) + ": not 5 lines in the RLIN rod mask";
            return false;
        }
        auto ul = kUserLines.find(r);
        if (ul != kUserLines.end() && lines != ul->second) {
            *error = "rod " + std::to_string(r) + ": rod-mask lines differ from the user's RF3 reading";
            return false;
        }
        if (r == 0) {
            bassLines = lines;
        }
        std::string tag;
        uint32_t nameBase = 0;
        std::vector<int> items;
        if (kind == 0) {
            tag = "RLRE";
            nameBase = 0x200;
            const std::vector<Rec>& rlre = rig.at("RLRE");
            for (int i = 0; i < static_cast<int>(rlre.size()); i++) {
                if (U16(rlre[i], 0x1E) == r + 1) {
                    items.push_back(i);
                }
            }
        } else if (kind == 2) {
            tag = "RBIT";
            nameBase = 0x320;
        } else {
            tag = "RFLY";
            nameBase = 0x400;
            for (int i = 0; i < static_cast<int>(rig.at("RFLY").size()); i++) {
                items.push_back(i);
            }
        }
        std::vector<int> kept;
        for (int i : items) {
            if (!StartsWith(Get(tk, nameBase + i, "ID_"), "ID_")) {
                kept.push_back(i);
            }
        }
        Json box = Json::Object();
        box["box"] = Json(r);
        box["kind"] = Json(kKinds[kind]);
        box["item_tag"] = Json(tag);
        box["rod_value"] = Json(static_cast<int>(rod[0x10]));
        box["rod_length_mm"] = Json(U32(rod, 0x0C));
        box["rod_limits"] = Bytes(rod, 0x16, 5);
        box["rod_heart"] = RawTable(rod, 0x44);
        box["rod_shield"] = RawTable(rod, 0x74);
        box["key"] = Json(kBoxKeys[r]);
        box["title"] = Json(Get(sy, 0x500 + r));
        box["rod"] = Json(r);
        box["rod_name"] = Json(Get(tk, 0x500 + r));
        box["reel_name"] = Json(reelId ? Get(tk, reelId) : std::string());
        Json tj = Json::Array();
        for (int t : targets) {
            Json e = Json::Object();
            e["record"] = Json(t);
            e["name"] = Json(Get(tk, 0x40 + t) + Get(sy, 0x100 + t));
            e["rod"] = HeartShield("RROD", rod, t);
            tj.Push(e);
        }
        box["targets"] = tj;
        Json lj = Json::Array();
        for (int i : kept) {
            lj.Push(ItemEntry(rig, tk, tag, i, nameBase, targets));
        }
        box["lures"] = lj;
        Json linesJ = Json::Array();
        for (int i : lines) {
            Json e = Json::Object();
            e["index"] = Json(i);
            e["name"] = Json(Get(tk, 0x600 + i));
            e["values"] = Values("RLIN", rlin[i], targets, nullptr);
            e["heart"] = RawTable(rlin[i], 0x24);
            e["shield"] = RawTable(rlin[i], 0x54);
            linesJ.Push(e);
        }
        box["lines"] = linesJ;
        if (kind == 2) {
            box["layout"] = Json(kBaitLayout.at(r));
            const std::vector<Rec>& rflt = rig.at("RFLT");
            Json fl = Json::Array();
            int nFloats = 0;
            for (int i = 0; i < static_cast<int>(rflt.size()); i++) {
                if ((U16(rflt[i], 0x0C) >> r) & 1) {
                    fl.Push(ItemEntry(rig, tk, "RFLT", i, 0x700, targets));
                    nFloats++;
                }
            }
            box["floats"] = fl;
            const std::vector<Rec>& rbit = rig.at("RBIT");
            Json baits = Json::Array();
            for (int g = 0; g < static_cast<int>(rbit.size()) / kBaitSizes; g++) {
                const Rec& first = rbit[g * kBaitSizes];
                if (!((U16(first, 0x2E) >> r) & 1)) {
                    continue;
                }
                const int hook = U16(first, 0x1E);
                Json sizes = Json::Array();
                for (int k = 0; k < kBaitSizes; k++) {
                    Json sz = ItemEntry(rig, tk, "RBIT", g * kBaitSizes + k, 0x320, targets);
                    sz["name"] = Json(Get(tk, 0x320 + g));
                    sizes.Push(sz);
                }
                Json b = Json::Object();
                b["index"] = Json(g);
                b["name"] = Json(Get(tk, 0x320 + g));
                b["hook"] = Json(Get(tk, 0x300 + hook));
                b["hook_index"] = Json(hook);
                b["desc"] = Json(Get(tk, 0xAF0 + g));
                b["sizes"] = sizes;
                baits.Push(b);
            }
            if (nFloats == 0 || baits.Items().empty()) {
                *error = "bait set " + std::to_string(r) + ": no floats or no baits";
                return false;
            }
            box["hook_default"] = *baits.Items()[0].Find("hook");
            box["hook_default_index"] = *baits.Items()[0].Find("hook_index");
            box["baits"] = baits;
        }
        boxes.Push(box);
    }

    // pole_box: the bass box with OoT's pole, fish and lures (step 23c)
    Json pole = boxes.Items()[0];
    const Rec& rod0 = rods[0];
    pole["box"] = Json(kPoleRod);
    pole["key"] = Json("pole");
    pole["title"] = Json("Fishing Pole");
    pole["rod"] = Json(kPoleRod);
    pole["rod_name"] = Json("Fishing Pole");
    pole["reel_name"] = Json("");
    const std::vector<int> poleTargets = { 50, 51, 52 };
    const std::vector<int> poleTable = { 15, 16, 16 }; // MOD_FISH_TABLE_RECORD
    const char* const poleNames[3] = { "Hylian Bass", "Hylian Bass(Lnkr)", "Hylian Loach" };
    Json pt = Json::Array();
    for (int k = 0; k < 3; k++) {
        Json e = Json::Object();
        e["record"] = Json(poleTargets[k]);
        e["name"] = Json(poleNames[k]);
        e["rod"] = HeartShield("RROD", rod0, poleTable[k]);
        pt.Push(e);
    }
    pole["targets"] = pt;
    struct PoleLure {
        int index;
        const char* name;
        int src;
    };
    const PoleLure poleLures[2] = { { 91, "Fishing Lure", 36 }, { 92, "Sinking Lure", 63 } };
    Json pl = Json::Array();
    for (const PoleLure& p : poleLures) {
        const Rec& rec = rig.at("RLRE")[p.src];
        Json e = Json::Object();
        e["index"] = Json(p.index);
        e["name"] = Json(p.name);
        e["values"] = Values("RLRE", rec, poleTargets, &poleTable);
        e["heart"] = RawTable(rec, 0x80);
        e["shield"] = RawTable(rec, 0xB0);
        e["appeal"] = Bytes(rec, 0x50, kFishRecords);
        e["type"] = Json(static_cast<int>(I16(rec, 0x2C)));
        e["desc"] = Json("");
        pl.Push(e);
    }
    pole["lures"] = pl;
    Json pLines = Json::Array();
    for (size_t k = 0; k < bassLines.size(); k++) {
        Json line = (*pole.Find("lines")).Items()[k];
        line["values"] = Values("RLIN", rig.at("RLIN")[bassLines[k]], poleTargets, &poleTable);
        pLines.Push(line);
    }
    pole["lines"] = pLines;
    boxes.Push(pole);
    *boxesOut = boxes;
    return true;
}

Json FishRodMasks(const std::vector<uint8_t>& fish) {
    const uint32_t n = U32(fish, 8);
    std::vector<int> masks;
    for (uint32_t i = 0; i < n; i++) {
        masks.push_back(U16(fish, 16 + 176 * i + 0x2E));
    }
    Json j = Json::ArrayOf(masks);
    for (int src : { 15, 16, 8 }) {
        j.Push(Json(masks[src] | (1 << kPoleRod)));
    }
    return j;
}

// season_sizes: RF3's mean fish length per species and season (step 57d).
Json SeasonSizes(const std::vector<uint8_t>& d) {
    std::map<int, std::array<std::array<int64_t, 2>, 3>> sums;
    int64_t o = Find(d, "MINS", 0);
    while (o >= 0) {
        const size_t rec = static_cast<size_t>(o) + 0x10;
        const int ngroups = I16(d, rec + 8);
        std::map<int, std::array<std::array<int64_t, 2>, 3>> per;
        for (int st = 0; st < 6; st++) {
            for (int sp = 0; sp < 10; sp++) {
                const size_t row = rec + 0x40 + st * 0x1E + sp * 3;
                for (int season = 0; season < 3; season++) {
                    const int g = d[row + season];
                    if (g == 0xFF || g >= ngroups) {
                        continue;
                    }
                    const size_t base = rec + 0x49C + g * 0x7E;
                    for (int i = 0; i < 31; i++) {
                        const int species = I16(d, base + 2 + i * 4), mm = I16(d, base + 4 + i * 4);
                        if (species < 0) {
                            break;
                        }
                        per[species][season][0] += 1;
                        per[species][season][1] += mm;
                    }
                }
            }
        }
        for (const auto& [species, seasons] : per) {
            if (seasons[0][0] > 0 && seasons[1][0] > 0 && seasons[2][0] > 0) {
                for (int s = 0; s < 3; s++) {
                    sums[species][s][0] += seasons[s][0];
                    sums[species][s][1] += seasons[s][1];
                }
            }
        }
        o = Find(d, "MINS", static_cast<size_t>(o) + 4);
    }
    Json out = Json::Object();
    for (const auto& [species, v] : sums) {
        Json e = Json::Object();
        Json count = Json::Array(), mean = Json::Array();
        for (int s = 0; s < 3; s++) {
            count.Push(Json(v[s][0]));
            mean.Push(Json(PyRound(static_cast<double>(v[s][1]) / static_cast<double>(v[s][0]), 1)));
        }
        e["count"] = count;
        e["mean_mm"] = mean;
        out[std::to_string(species)] = e;
    }
    return out;
}

bool ConditionsData(const std::vector<uint8_t>& d, const Texts& sy, Json* out, std::string* error) {
    if (d.size() < 4 || std::string(d.begin(), d.begin() + 4) != "MINF") {
        *error = "etc/mapinfo.fdt: no MINF magic";
        return false;
    }
    int counts[3][3][4] = {};
    Json areas = Json::Object();
    int64_t o = Find(d, "MINS", 0);
    while (o >= 0) {
        const size_t rec = static_cast<size_t>(o) + 0x10;
        const int area = I32(d, rec);
        Json pattern = Json::Array();
        for (int season = 0; season < 3; season++) {
            const size_t base = rec + 0x40A + season * 0x30;
            Json days = Json::Array();
            for (int day = 0; day < 8; day++) {
                Json row = Json::Array();
                for (int t = 0; t < 3; t++) {
                    const int w = d[base + day * 3 + t];
                    if (w > 3) {
                        *error = "etc/mapinfo.fdt: a weather value out of 0-3";
                        return false;
                    }
                    counts[season][t][w]++;
                    row.Push(Json(w));
                }
                days.Push(row);
            }
            pattern.Push(days);
        }
        areas[std::to_string(area)] = pattern;
        o = Find(d, "MINS", static_cast<size_t>(o) + 4);
    }
    Json wc = Json::Array();
    for (auto& s : counts) {
        Json sj = Json::Array();
        for (auto& t : s) {
            Json tj = Json::Array();
            for (int w : t) {
                tj.Push(Json(w));
            }
            sj.Push(tj);
        }
        wc.Push(sj);
    }
    auto names = [&sy](std::initializer_list<uint32_t> ids) {
        Json j = Json::Array();
        for (uint32_t id : ids) {
            j.Push(Json(Get(sy, id)));
        }
        return j;
    };
    Json nm = Json::Object();
    nm["season"] = names({ 0x42, 0x43, 0x44 });
    nm["weather"] = names({ 0x45, 0x46, 0x47, 0x48 });
    nm["time"] = names({ 0x711, 0x712, 0x713 });
    Json c = Json::Object();
    c["weather_counts"] = wc;
    c["areas"] = areas;
    c["names"] = nm;
    c["days_per_pattern"] = Json(8);
    c["season_sizes"] = SeasonSizes(d);
    *out = c;
    return true;
}

// ---------------------------------------------------------------- font and icons

struct Font {
    int count = 0, width = 0, height = 0;
    std::vector<Rgba> clut;
    std::vector<std::vector<uint8_t>> glyphs;
    std::vector<std::pair<int, int>> spans;
};

bool ParseFont(const std::vector<uint8_t>& data, Font* f) {
    if (data.size() < 16 || std::string(data.begin(), data.begin() + 4) != "FONT") {
        return false;
    }
    std::map<std::string, std::pair<size_t, size_t>> chunks;
    size_t o = 0x10;
    while (o + 16 <= data.size()) {
        const std::string tag(data.begin() + o, data.begin() + o + 4);
        const uint32_t size = U32(data, o + 4);
        if (size < 16) {
            break;
        }
        chunks[tag] = { o + 16, size - 16 };
        o += size;
    }
    if (!chunks.count("INFO") || !chunks.count("CLUT") || !chunks.count("PICT") || !chunks.count("PRPO")) {
        return false;
    }
    const size_t io = chunks["INFO"].first;
    const uint32_t ncol = U32(data, io);
    f->count = static_cast<int>(U32(data, io + 4));
    f->width = static_cast<int>(U32(data, io + 8));
    f->height = static_cast<int>(U32(data, io + 12));
    const size_t co = chunks["CLUT"].first;
    for (uint32_t k = 0; k < ncol; k++) {
        f->clut.push_back({ static_cast<uint8_t>(U32(data, co + 16 * k)), static_cast<uint8_t>(U32(data, co + 16 * k + 4)),
                            static_cast<uint8_t>(U32(data, co + 16 * k + 8)),
                            static_cast<uint8_t>(std::min<uint32_t>(255, U32(data, co + 16 * k + 12) * 2)) });
    }
    const size_t po = chunks["PICT"].first, psize = chunks["PICT"].second;
    const size_t gbytes = static_cast<size_t>(f->width) * f->height / 2;
    if (psize < f->count * gbytes) {
        return false;
    }
    for (int i = 0; i < f->count; i++) {
        std::vector<uint8_t> px;
        for (size_t b = 0; b < gbytes; b++) {
            const uint8_t v = data[po + i * gbytes + b];
            px.push_back(v & 0xF);
            px.push_back(v >> 4);
        }
        f->glyphs.push_back(px);
    }
    const size_t so = chunks["PRPO"].first;
    for (int i = 0; i < f->count; i++) {
        const int right = data[so + 2 * i], left = data[so + 2 * i + 1];
        f->spans.push_back(right == 0 && left == 0 ? std::make_pair(0, f->width) : std::make_pair(left, right));
    }
    return true;
}

// arrow_icon: a circle in fill with a darker rim and a triangle pointing left or right (4x4 supersampled).
std::vector<uint8_t> ArrowIcon(const Rgba& fill, const Rgba& arrow, bool left, int size, int ss = 4) {
    const double n = size * ss;
    const double cx = n / 2, cy = n / 2;
    const double radius = 0.47 * n, rim = 0.06 * n;
    const double tipX = left ? 0.27 * n : 0.73 * n, baseX = left ? 0.63 * n : 0.37 * n, half = 0.22 * n;
    const Rgba rimc = { static_cast<uint8_t>(static_cast<int>(fill[0] * 0.6)),
                        static_cast<uint8_t>(static_cast<int>(fill[1] * 0.6)),
                        static_cast<uint8_t>(static_cast<int>(fill[2] * 0.6)), 0 };
    auto sample = [&](double x, double y, Rgba* c) {
        const double d = std::pow(std::pow(x - cx, 2.0) + std::pow(y - cy, 2.0), 0.5); // Python's ** is C pow()
        if (d > radius) {
            return false;
        }
        const double t = (x - tipX) / (baseX - tipX);
        if (0.0 <= t && t <= 1.0 && std::fabs(y - cy) <= half * t) {
            *c = arrow;
        } else {
            *c = d > radius - rim ? rimc : fill;
        }
        return true;
    };
    std::vector<uint8_t> out;
    for (int ty = 0; ty < size; ty++) {
        for (int tx = 0; tx < size; tx++) {
            int64_t acc[3] = { 0, 0, 0 };
            int cover = 0;
            for (int sy = 0; sy < ss; sy++) {
                for (int sx = 0; sx < ss; sx++) {
                    Rgba c{};
                    if (sample(tx * ss + sx + 0.5, ty * ss + sy + 0.5, &c)) {
                        cover++;
                        for (int k = 0; k < 3; k++) {
                            acc[k] += c[k];
                        }
                    }
                }
            }
            if (cover) {
                for (int k = 0; k < 3; k++) {
                    out.push_back(static_cast<uint8_t>(PyRoundInt(static_cast<double>(acc[k]) / cover)));
                }
                out.push_back(static_cast<uint8_t>(PyRoundInt(255.0 * cover / (ss * ss))));
            } else {
                out.insert(out.end(), { 0, 0, 0, 0 });
            }
        }
    }
    return out;
}

} // namespace

bool BuildTackle(rf3disc::Pack& pack, std::vector<Entry>* out, std::string* error, const ProgressFn& progress) {
    out->clear();
    const std::string texPrefix = "textures/rf3tackle/";
    auto addTexture = [&](const std::string& name, int w, int h, const std::vector<uint8_t>& rgba) {
        out->push_back({ texPrefix + name, TextureRgba32(w, h, rgba) });
    };

    std::map<int, P2G> pages;
    auto page = [&](int num, P2G** t) {
        auto it = pages.find(num);
        if (it == pages.end()) {
            char path[64];
            std::snprintf(path, sizeof(path), "menu/fishspot/p2gf_%02d.p2g", num);
            std::vector<uint8_t> data;
            P2G p;
            if (!pack.Read(path, &data)) {
                *error = std::string(path) + " is missing on the disc";
                return false;
            }
            if (!ParseP2g(data, path, &p, error)) {
                return false;
            }
            it = pages.emplace(num, std::move(p)).first;
        }
        *t = &it->second;
        return true;
    };

    std::map<std::tuple<int, int, int, int, int>, std::string> pieces;
    Json pieceTable = Json::Object();
    std::set<std::string> pieceNames;
    // piece(): a page region as one texture, or strips of at most kMaxTexels (width padded to a multiple of 4).
    auto piece = [&](int num, const Vertex& v0, const Vertex& v1, std::string* name, bool* flipX, bool* flipY) {
        P2G* t = nullptr;
        if (!page(num, &t)) {
            return false;
        }
        const double fx0 = v0.u * t->width, fy0 = v0.v * t->height, fx1 = v1.u * t->width, fy1 = v1.v * t->height;
        const int x0 = static_cast<int>(PyRoundInt(std::min(fx0, fx1))), y0 = static_cast<int>(PyRoundInt(std::min(fy0, fy1)));
        const int x1 = static_cast<int>(PyRoundInt(std::max(fx0, fx1))), y1 = static_cast<int>(PyRoundInt(std::max(fy0, fy1)));
        const int w = std::max(1, x1 - x0), h = std::max(1, y1 - y0);
        const auto key = std::make_tuple(num, x0, y0, w, h);
        auto it = pieces.find(key);
        if (it == pieces.end()) {
            char nm[64];
            std::snprintf(nm, sizeof(nm), "p%02d_%d_%d_%dx%d", num, x0, y0, w, h);
            const int tw = (w + 3) / 4 * 4;
            const int rows = std::max(1, kMaxTexels / tw);
            Json strips = Json::Array();
            for (int k = 0, sy = 0; sy < h; k++, sy += rows) {
                const int sh = std::min(rows, h - sy);
                const std::string tex = rows >= h ? std::string(nm) : std::string(nm) + "_s" + std::to_string(k);
                addTexture(tex, tw, sh, PadWidth(Crop(*t, x0, y0 + sy, w, sh), w, sh, tw));
                Json s = Json::Array();
                s.Push(Json(tex));
                s.Push(Json(sy));
                s.Push(Json(sh));
                strips.Push(s);
            }
            Json pe = Json::Object();
            Json sz = Json::Array();
            sz.Push(Json(w));
            sz.Push(Json(h));
            pe["size"] = sz;
            pe["tex_w"] = Json(tw);
            pe["strips"] = strips;
            pieceTable[nm] = pe;
            pieceNames.insert(nm);
            it = pieces.emplace(key, nm).first;
        }
        *name = it->second;
        *flipX = fx1 < fx0;
        *flipY = fy1 < fy0;
        return true;
    };

    Json layout = Json::Object();
    Json screen = Json::Array();
    screen.Push(Json(640));
    screen.Push(Json(480));
    layout["screen"] = screen;
    layout["texture_prefix"] = Json(texPrefix);
    Json layouts = Json::Object();
    int li = 0;
    for (const char* nm : kScreenLayouts) {
        progress(std::string("Tackle box screen: ") + nm, 0.6f * li++ / 6.0f);
        std::vector<uint8_t> gps;
        if (!pack.Read(std::string("menu/") + nm + ".gps", &gps) || gps.size() < 16 ||
            std::string(gps.begin(), gps.begin() + 4) != "GPOS" || std::string(gps.begin() + 8, gps.begin() + 12) != "PSD7") {
            *error = std::string("menu/") + nm + ".gps is missing or not a GPOS/PSD7 layout";
            return false;
        }
        Json elems = Json::Array();
        for (const Element& e : ParseGps(gps)) {
            Json entry = Json::Object();
            entry["name"] = Json(e.name);
            entry["kind"] = Json(e.tag);
            entry["index"] = Json(e.index);
            entry["flags"] = Json(e.kind);
            Json head = Json::Array();
            for (uint32_t v : e.head) {
                head.Push(Json(v));
            }
            entry["head"] = head;
            if (e.vertices.empty()) {
                entry["box"] = Json();
            } else {
                int32_t mx = e.vertices[0].x, my = e.vertices[0].y, Mx = mx, My = my;
                for (const Vertex& v : e.vertices) {
                    mx = std::min(mx, v.x);
                    my = std::min(my, v.y);
                    Mx = std::max(Mx, v.x);
                    My = std::max(My, v.y);
                }
                Json b = Json::Array();
                for (int32_t v : { mx, my, Mx, My }) {
                    b.Push(Json(static_cast<int>(v)));
                }
                entry["box"] = b;
            }
            Json sprites = Json::Array();
            if (e.tag == "IMGX" || e.tag == "NUMX" || e.tag == "LBOX") {
                const size_t n = std::min<size_t>(e.count, e.vertices.size() / 2);
                for (size_t k = 0; k < n; k++) {
                    const uint32_t mode = k < e.texrefs.size() ? e.texrefs[k].first : 0;
                    const uint32_t tr = k < e.texrefs.size() ? e.texrefs[k].second : 0xFFFF0000u;
                    if ((tr >> 16) == 0xFFFF) {
                        continue;
                    }
                    const Vertex& v0 = e.vertices[2 * k];
                    const Vertex& v1 = e.vertices[2 * k + 1];
                    std::string pname;
                    bool fx = false, fy = false;
                    if (!piece(static_cast<int>(tr >> 16), v0, v1, &pname, &fx, &fy)) {
                        return false;
                    }
                    Json s = Json::Object();
                    s["piece"] = Json(pname);
                    s["mode"] = Json(mode);
                    Json rect = Json::Array();
                    for (int32_t v : { v0.x, v0.y, v1.x, v1.y }) {
                        rect.Push(Json(static_cast<int>(v)));
                    }
                    s["rect"] = rect;
                    Json rgba = Json::Array();
                    for (uint32_t v : { v0.r, v0.g, v0.b, v0.a }) {
                        rgba.Push(Json(v));
                    }
                    s["rgba"] = rgba;
                    Json flip = Json::Array();
                    flip.Push(Json(fx));
                    flip.Push(Json(fy));
                    s["flip"] = flip;
                    sprites.Push(s);
                }
            }
            entry["sprites"] = sprites;
            if (e.tag == "OTHX") {
                Json quad = Json::Array();
                for (const Vertex& v : e.vertices) {
                    Json q = Json::Array();
                    q.Push(Json(static_cast<int>(v.x)));
                    q.Push(Json(static_cast<int>(v.y)));
                    for (uint32_t c : { v.r, v.g, v.b, v.a }) {
                        q.Push(Json(c));
                    }
                    quad.Push(q);
                }
                entry["quad"] = quad;
            }
            elems.Push(entry);
        }
        layouts[nm] = elems;
    }
    layout["layouts"] = layouts;

    // number-box glyphs
    P2G* dp = nullptr;
    if (!page(kDigitPage, &dp)) {
        return false;
    }
    Json digitNames = Json::Array();
    for (int k = 0; k < 12; k++) {
        addTexture(std::string("digit_") + kDigitNames[k], kDigitW, kDigitH,
                   Crop(*dp, kDigitX + k * kDigitPitch, kDigitY, kDigitW, kDigitH));
        digitNames.Push(Json(kDigitNames[k]));
    }
    Json digits = Json::Object();
    digits["names"] = digitNames;
    Json dsz = Json::Array();
    dsz.Push(Json(kDigitW));
    dsz.Push(Json(kDigitH));
    digits["size"] = dsz;
    digits["pitch"] = Json(kDigitPitch);
    layout["digits"] = digits;

    // Step 58b: icons in place of RF3's L1/R1 and L2/R2 plaques. The circle colours are SoH's own button pictures'
    // means (soh/assets/custom/textures/buttons, SoH's files), as rf3_menuassets.py's mean_colour gives them.
    struct Icon {
        const char* plaque;
        const char* icon;
        Rgba fill;
        bool left;
        Rgba arrow;
    };
    const Icon icons[4] = {
        { "p21_0_150_42x26", "btn_stick_left", { 117, 117, 117, 0 }, true, { 255, 255, 255, 0 } },
        { "p21_43_151_41x25", "btn_stick_right", { 117, 117, 117, 0 }, false, { 255, 255, 255, 0 } },
        { "p21_0_177_42x26", "btn_cleft", { 209, 146, 0, 0 }, true, { 40, 32, 0, 0 } },
        { "p21_43_178_41x24", "btn_cright", { 209, 146, 0, 0 }, false, { 40, 32, 0, 0 } },
    };
    Json buttonIcons = Json::Object();
    for (const Icon& ic : icons) {
        if (!pieceNames.count(ic.plaque)) {
            *error = std::string("plaque piece ") + ic.plaque + " is not in the layouts";
            return false;
        }
        if (!pieceNames.count(ic.icon)) {
            addTexture(ic.icon, kIconSize, kIconSize, ArrowIcon(ic.fill, ic.arrow, ic.left, kIconSize));
            Json pe = Json::Object();
            Json sz = Json::Array();
            sz.Push(Json(kIconSize));
            sz.Push(Json(kIconSize));
            pe["size"] = sz;
            pe["tex_w"] = Json(kIconSize);
            Json strip = Json::Array();
            strip.Push(Json(ic.icon));
            strip.Push(Json(0));
            strip.Push(Json(kIconSize));
            Json strips = Json::Array();
            strips.Push(strip);
            pe["strips"] = strips;
            pieceTable[ic.icon] = pe;
            pieceNames.insert(ic.icon);
        }
        Json bi = Json::Object();
        bi["icon"] = Json(ic.icon);
        buttonIcons[ic.plaque] = bi;
    }
    layout["button_icons"] = buttonIcons;
    layout["pieces"] = pieceTable;

    // font: one texture per glyph with ink
    progress("Tackle box screen: the font", 0.65f);
    std::vector<uint8_t> fontData;
    Font f;
    if (!pack.Read("menu/font_eb.fnt", &fontData) || !ParseFont(fontData, &f)) {
        *error = "menu/font_eb.fnt is missing or not a FONT file";
        return false;
    }
    Json inked = Json::Array();
    for (int code = 0; code < f.count; code++) {
        const std::vector<uint8_t>& px = f.glyphs[code];
        if (std::all_of(px.begin(), px.end(), [](uint8_t v) { return v == 0; })) {
            continue;
        }
        std::vector<uint8_t> rgba;
        for (uint8_t i : px) {
            rgba.insert(rgba.end(), f.clut[i].begin(), f.clut[i].end());
        }
        char nm[16];
        std::snprintf(nm, sizeof(nm), "%02x", code);
        addTexture(std::string("font_") + nm, f.width, f.height, rgba);
        inked.Push(Json(nm));
    }
    Json font = Json::Object();
    Json cell = Json::Array();
    cell.Push(Json(f.width));
    cell.Push(Json(f.height));
    font["cell"] = cell;
    Json spans = Json::Array();
    for (const auto& sp : f.spans) {
        Json s = Json::Array();
        s.Push(Json(sp.first));
        s.Push(Json(sp.second));
        spans.Push(s);
    }
    font["spans"] = spans;
    font["inked"] = inked;

    // tackle data
    progress("Tackle box screen: the tackle data", 0.75f);
    Json boxes;
    Texts sy;
    if (!TackleData(pack, &boxes, &sy, error)) {
        return false;
    }
    std::vector<uint8_t> fish, mapinfo;
    if (!pack.Read("etc/fishdata.fdt", &fish) || !pack.Read("etc/mapinfo.fdt", &mapinfo)) {
        *error = "etc/fishdata.fdt or etc/mapinfo.fdt is missing on the disc";
        return false;
    }
    Json conditions;
    if (!ConditionsData(mapinfo, sy, &conditions, error)) {
        return false;
    }
    Json tackle = Json::Object();
    tackle["boxes"] = boxes;
    const std::vector<std::pair<const char*, uint32_t>> textIds = {
        { "finish", 0x201 },           { "line_dia", 0x203 },          { "change_tackle", 0x200 },
        { "rods_reels", 0x6CC },       { "lures", 0x6CD },             { "attach", 0x215 },
        { "not_ready", 0x15E },        { "lure_not_selected", 0x21E }, { "fly_not_selected", 0x21F },
        { "bait_not_selected", 0x220 },
    };
    Json texts = Json::Object();
    for (const auto& [k, id] : textIds) {
        texts[k] = Json(Get(sy, id));
    }
    tackle["texts"] = texts;
    Json depth = Json::Array();
    for (uint32_t id : { 0x20F, 0x20E, 0x20D, 0x20C, 0x20B }) {
        depth.Push(Json(Get(sy, id)));
    }
    tackle["depth_names"] = depth;
    Json typeNames = Json::Object();
    Json lureTypes = Json::Array(), flyTypes = Json::Array();
    for (uint32_t id = 0x210; id < 0x215; id++) {
        lureTypes.Push(Json(Get(sy, id)));
    }
    for (uint32_t id = 0x219; id < 0x21D; id++) {
        flyTypes.Push(Json(Get(sy, id)));
    }
    typeNames["lure"] = lureTypes;
    typeNames["fly"] = flyTypes;
    tackle["type_names"] = typeNames;
    tackle["font"] = font;
    tackle["fish_rod_mask"] = FishRodMasks(fish);
    tackle["conditions"] = conditions;
    tackle["note"] = Json("Generated from the user's own Reel Fishing III disc by tools/rf3_menuassets.py. "
                          "Game data: never commit or share.");

    progress("Tackle box screen: writing", 0.95f);
    for (const auto& [name, obj] : { std::make_pair("rf3tackle/layout.json", &layout),
                                     std::make_pair("rf3tackle/tackle.json", &tackle) }) {
        const std::string text = DumpJson(*obj);
        out->push_back({ name, std::vector<uint8_t>(text.begin(), text.end()) });
    }
    return true;
}

} // namespace rf3extract
