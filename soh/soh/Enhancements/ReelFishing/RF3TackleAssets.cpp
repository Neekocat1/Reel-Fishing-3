// Reel Fishing III mod (step 17d): loads the tackle box screen's data from mods/rf3_tackle.o2r.
// See RF3TackleAssets.h.

#include "RF3TackleAssets.h"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>

#include <nlohmann/json.hpp>
#include <ship/Context.h>
#include <ship/resource/ResourceManager.h>
#include <ship/resource/archive/ArchiveManager.h>

namespace rf3tackle {
namespace {

using json = nlohmann::json;

constexpr const char* kTexturePrefix = "textures/rf3tackle/";
constexpr const char* kOtrPrefix = "__OTR__";
constexpr const char* kTacklePath = "rf3tackle/tackle.json";
constexpr const char* kLayoutPath = "rf3tackle/layout.json";
constexpr size_t kMaxMissingLogged = 20;

Assets sAssets;
bool sTried = false;

std::shared_ptr<Ship::ArchiveManager> Archives() {
    auto ctx = Ship::Context::GetRawInstance();
    if (ctx == nullptr || ctx->GetResourceManager() == nullptr) {
        return nullptr;
    }
    return ctx->GetResourceManager()->GetArchiveManager();
}

std::optional<json> ReadJson(const std::string& path, std::string& why) {
    auto archives = Archives();
    if (archives == nullptr) {
        why = "no archive manager";
        return std::nullopt;
    }
    auto file = archives->LoadFile(path);
    if (file == nullptr || file->Buffer == nullptr) {
        why = path + " not found (run tools/rf3_menuassets.py build)";
        return std::nullopt;
    }
    json j = json::parse(file->Buffer->begin() + file->BufferOffset, file->Buffer->end(), nullptr, false);
    if (j.is_discarded()) {
        why = path + " is not valid JSON";
        return std::nullopt;
    }
    return j;
}

HeartShield ReadHeartShield(const json& v) {
    if (!v.is_array() || v.size() != 2) {
        return std::nullopt; // null in the file = not usable for that fish
    }
    return std::array<int32_t, 2>{ v[0].get<int32_t>(), v[1].get<int32_t>() };
}

std::vector<int32_t> ReadInts(const json& j, const char* key) {
    std::vector<int32_t> out;
    if (j.contains(key) && j.at(key).is_array()) {
        for (const json& v : j.at(key)) {
            out.push_back(v.get<int32_t>());
        }
    }
    return out;
}

std::vector<Item> ReadItems(const json& arr) {
    std::vector<Item> out;
    for (const json& j : arr) {
        Item item;
        item.index = j.at("index").get<int32_t>();
        item.name = j.at("name").get<std::string>();
        for (auto& [record, hs] : j.at("values").items()) {
            item.values[std::stoi(record)] = ReadHeartShield(hs);
        }
        item.heart = ReadInts(j, "heart");
        item.shield = ReadInts(j, "shield");
        item.appeal = ReadInts(j, "appeal");
        if (j.contains("move") && j.at("move").is_object()) {
            // tools/rf3_menuassets.py writes these with the names from before step 20a-1:
            // "reeled_action" is RLRE +0x0E (the resting action), "still_action" is +0x12 (the twitch
            // response). Speed pairs: reel down, reel up, rest sink, rest float-up (RF3-MECHANICS 3.2).
            const json& m = j.at("move");
            item.move.restAction = static_cast<uint8_t>(m.at("reeled_action").get<int32_t>());
            item.move.twitchResponse = static_cast<uint8_t>(m.at("still_action").get<int32_t>());
            item.move.buoyancy = static_cast<uint8_t>(m.at("buoyancy").get<int32_t>());
            for (int k = 0; k < 4; k++) {
                item.move.speedBase[k] = m.at("speed_base").at(k).get<float>();
                item.move.speedRate[k] = m.at("speed_rate").at(k).get<float>();
            }
            item.hasMove = true;
        }
        item.sunk = j.value("sunk", -1); // step 20c-3b, flies
        out.push_back(std::move(item));
    }
    return out;
}

void ReadTackle(const json& t) {
    for (const json& b : t.at("boxes")) {
        Box box;
        box.key = b.at("key").get<std::string>();
        box.kind = b.value("kind", std::string("lure"));
        box.title = b.at("title").get<std::string>();
        box.rod = b.at("rod").get<int32_t>();
        box.rodName = b.at("rod_name").get<std::string>();
        box.reelName = b.at("reel_name").get<std::string>();
        for (const json& tg : b.at("targets")) {
            box.targets.push_back(
                { tg.at("record").get<int32_t>(), tg.at("name").get<std::string>(), ReadHeartShield(tg.at("rod")) });
        }
        box.lures = ReadItems(b.at("lures"));
        box.lines = ReadItems(b.at("lines"));
        box.rodValue = b.value("rod_value", 0);
        box.rodLengthMm = b.value("rod_length_mm", 0);
        box.rodLimits = ReadInts(b, "rod_limits");
        box.rodHeart = ReadInts(b, "rod_heart");
        box.rodShield = ReadInts(b, "rod_shield");
        box.layout = b.value("layout", std::string("menu_02"));
        if (b.contains("floats")) {
            box.floats = ReadItems(b.at("floats"));
        }
        if (b.contains("baits")) {
            for (const json& bt : b.at("baits")) {
                Bait bait;
                bait.index = bt.at("index").get<int32_t>();
                bait.name = bt.at("name").get<std::string>();
                bait.hook = bt.at("hook").get<std::string>();
                bait.sizes = ReadItems(bt.at("sizes"));
                box.baits.push_back(std::move(bait));
            }
        }
        box.hookDefault = b.value("hook_default", std::string());
        sAssets.boxes.push_back(std::move(box));
    }
    sAssets.fishRodMask = ReadInts(t, "fish_rod_mask");
    if (t.contains("depth_names")) {
        for (const json& d : t.at("depth_names")) {
            sAssets.depthNames.push_back(d.get<std::string>());
        }
    }
    for (auto& [key, text] : t.at("texts").items()) {
        sAssets.texts[key] = text.get<std::string>();
    }
    const json& font = t.at("font");
    sAssets.fontCell = { font.at("cell")[0].get<int32_t>(), font.at("cell")[1].get<int32_t>() };
    for (const json& sp : font.at("spans")) {
        sAssets.fontSpans.push_back({ sp[0].get<int32_t>(), sp[1].get<int32_t>() });
    }
    sAssets.fontInked.assign(sAssets.fontSpans.size(), false);
    sAssets.fontPaths.assign(sAssets.fontSpans.size(), "");
    for (const json& code : font.at("inked")) {
        size_t c = std::stoul(code.get<std::string>(), nullptr, 16);
        if (c < sAssets.fontInked.size()) {
            sAssets.fontInked[c] = true;
        }
    }
}

void ReadLayout(const json& l) {
    for (auto& [name, elems] : l.at("layouts").items()) {
        std::vector<Element> out;
        for (const json& e : elems) {
            Element el;
            el.name = e.at("name").get<std::string>();
            el.kind = e.at("kind").get<std::string>();
            el.index = e.at("index").get<int32_t>();
            el.flags = e.at("flags").get<int32_t>();
            for (size_t i = 0; i < el.head.size() && i < e.at("head").size(); i++) {
                el.head[i] = e.at("head")[i].get<int32_t>();
            }
            if (e.at("box").is_array()) {
                const json& bx = e.at("box");
                el.box = std::array<int32_t, 4>{ bx[0].get<int32_t>(), bx[1].get<int32_t>(), bx[2].get<int32_t>(),
                                                 bx[3].get<int32_t>() };
            }
            for (const json& s : e.at("sprites")) {
                Sprite sp;
                sp.piece = s.at("piece").get<std::string>();
                sp.mode = s.at("mode").get<int32_t>();
                for (int i = 0; i < 4; i++) {
                    sp.rect[i] = s.at("rect")[i].get<int32_t>();
                    sp.rgba[i] = s.at("rgba")[i].get<int32_t>();
                }
                sp.flip = { s.at("flip")[0].get<bool>(), s.at("flip")[1].get<bool>() };
                el.sprites.push_back(std::move(sp));
            }
            if (e.contains("quad")) {
                for (const json& v : e.at("quad")) {
                    std::array<int32_t, 6> q{};
                    for (int i = 0; i < 6; i++) {
                        q[i] = v[i].get<int32_t>();
                    }
                    el.quad.push_back(q);
                }
            }
            out.push_back(std::move(el));
        }
        sAssets.layouts[name] = std::move(out);
    }
    const json& d = l.at("digits");
    for (const json& n : d.at("names")) {
        sAssets.digitNames.push_back(n.get<std::string>());
    }
    sAssets.digitSize = { d.at("size")[0].get<int32_t>(), d.at("size")[1].get<int32_t>() };
    sAssets.digitPitch = d.at("pitch").get<int32_t>();
    for (auto& [name, p] : l.at("pieces").items()) {
        Piece piece;
        piece.w = p.at("size")[0].get<int32_t>();
        piece.h = p.at("size")[1].get<int32_t>();
        piece.texW = p.value("tex_w", piece.w);
        for (const json& s : p.at("strips")) {
            piece.strips.push_back(
                { kOtrPrefix + TexturePath(s[0].get<std::string>()), s[1].get<int32_t>(), s[2].get<int32_t>() });
        }
        sAssets.pieces[name] = std::move(piece);
    }
    for (const std::string& dn : sAssets.digitNames) {
        sAssets.digitPaths.push_back(kOtrPrefix + TexturePath("digit_" + dn));
    }
}

// Texture name (without prefix) for a glyph, e.g. "font_31".
std::string GlyphName(size_t code) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "font_%02zx", code);
    return buf;
}

// Every texture the data names: sprite pieces, number glyphs, inked font glyphs.
void CheckTextures(std::vector<std::string>& missing) {
    std::set<std::string> names;
    for (auto& [_, elems] : sAssets.layouts) {
        for (const Element& e : elems) {
            for (const Sprite& s : e.sprites) {
                auto it = sAssets.pieces.find(s.piece);
                if (it == sAssets.pieces.end() || it->second.strips.empty()) {
                    names.insert(s.piece); // not in the piece table: reported as missing below
                    continue;
                }
                for (const Strip& st : it->second.strips) {
                    names.insert(st.path.substr(std::string(kOtrPrefix).size() + std::string(kTexturePrefix).size()));
                }
            }
        }
    }
    for (const std::string& d : sAssets.digitNames) {
        names.insert("digit_" + d);
    }
    for (size_t c = 0; c < sAssets.fontInked.size(); c++) {
        if (sAssets.fontInked[c]) {
            names.insert(GlyphName(c));
            sAssets.fontPaths[c] = kOtrPrefix + TexturePath(GlyphName(c));
        }
    }
    auto archives = Archives();
    for (const std::string& n : names) {
        if (archives == nullptr || !archives->HasFile(TexturePath(n))) {
            missing.push_back(n);
        }
    }
    sAssets.texturesNeeded = static_cast<int32_t>(names.size());
    sAssets.texturesMissing = static_cast<int32_t>(missing.size());
    sAssets.texturesOk = missing.empty() && !names.empty();
}

void WriteLog(const std::string& why, const std::vector<std::string>& missing) {
    std::string dir = Ship::Context::GetPathRelativeToAppDirectory("logs");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(dir + "/rf3_menu.txt", std::ios::app);
    char stamp[32];
    std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    out << stamp << " assets loaded=" << (sAssets.loaded ? 1 : 0) << " texturesOk=" << (sAssets.texturesOk ? 1 : 0)
        << " needed=" << sAssets.texturesNeeded << " missing=" << sAssets.texturesMissing;
    for (const Box& b : sAssets.boxes) {
        out << " " << b.key << "(rod=" << b.rod << " lines=" << b.lines.size() << " lures=" << b.lures.size()
            << " targets=" << b.targets.size() << ")";
    }
    for (auto& [name, elems] : sAssets.layouts) {
        size_t sprites = 0;
        for (const Element& e : elems) {
            sprites += e.sprites.size();
        }
        out << " " << name << "(elements=" << elems.size() << " sprites=" << sprites << ")";
    }
    out << " fontGlyphs=" << sAssets.fontSpans.size();
    if (!why.empty()) {
        out << " why=\"" << why << "\"";
    }
    out << "\n";
    for (size_t i = 0; i < missing.size() && i < kMaxMissingLogged; i++) {
        out << stamp << "   missing texture " << missing[i] << "\n";
    }
    if (missing.size() > kMaxMissingLogged) {
        out << stamp << "   ... and " << (missing.size() - kMaxMissingLogged) << " more\n";
    }
}

void Load() {
    std::string why;
    std::vector<std::string> missing;
    auto tackle = ReadJson(kTacklePath, why);
    auto layout = tackle ? ReadJson(kLayoutPath, why) : std::nullopt;
    if (tackle && layout) {
        try {
            ReadTackle(*tackle);
            ReadLayout(*layout);
            sAssets.loaded = !sAssets.boxes.empty() && !sAssets.layouts.empty();
            if (!sAssets.loaded) {
                why = "JSON files have no boxes or layouts";
            }
        } catch (const json::exception& e) {
            why = std::string("JSON layout not as expected: ") + e.what();
            sAssets = Assets{};
        }
    }
    if (sAssets.loaded) {
        CheckTextures(missing);
    }
    WriteLog(why, missing);
}

} // namespace

const Assets& Get() {
    if (!sTried) {
        sTried = true;
        Load();
    }
    return sAssets;
}

std::string TexturePath(const std::string& name) {
    return std::string(kTexturePrefix) + name;
}

const Item* FindLine(const Box& box, int32_t index) {
    for (const Item& i : box.lines) {
        if (i.index == index) {
            return &i;
        }
    }
    return nullptr;
}

const Item* FindLure(const Box& box, int32_t index) {
    for (const Item& i : box.lures) {
        if (i.index == index) {
            return &i;
        }
    }
    return nullptr;
}

const Piece* FindPiece(const std::string& name) {
    auto it = sAssets.pieces.find(name);
    return it == sAssets.pieces.end() ? nullptr : &it->second;
}

const Element* FindElement(const std::string& layout, const std::string& name) {
    auto it = sAssets.layouts.find(layout);
    if (it == sAssets.layouts.end()) {
        return nullptr;
    }
    for (const Element& e : it->second) {
        if (e.name == name) {
            return &e;
        }
    }
    return nullptr;
}

const Item* FindFloat(const Box& box, int32_t index) {
    for (const Item& i : box.floats) {
        if (i.index == index) {
            return &i;
        }
    }
    return nullptr;
}

const Bait* FindBait(const Box& box, int32_t index) {
    for (const Bait& b : box.baits) {
        if (b.index == index) {
            return &b;
        }
    }
    return nullptr;
}

const Item* BaitAtSize(const Box& box, int32_t bait, int32_t size) {
    const Bait* b = FindBait(box, bait);
    if (b == nullptr || size < 0 || size >= static_cast<int32_t>(b->sizes.size())) {
        return nullptr;
    }
    return &b->sizes[size];
}

const Target* FindTarget(const Box& box, int32_t record) {
    for (const Target& t : box.targets) {
        if (t.record == record) {
            return &t;
        }
    }
    return nullptr;
}

} // namespace rf3tackle
