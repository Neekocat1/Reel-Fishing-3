#pragma once

// Reel Fishing III mod (step 17d): the tackle box screen's pictures and data.
// They are not in the mod: tools/rf3_menuassets.py makes them from the player's own RF3 disc and
// packs them into mods/rf3_tackle.o2r (textures/rf3tackle/*, rf3tackle/layout.json,
// rf3tackle/tackle.json). This loads the two JSON files once, checks that every texture they name
// is in the loaded archives, and logs the result to logs/rf3_menu.txt. Without the archive the
// screen falls back to a plain style (step 17e). See docs/DESIGN.md step 17.

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace rf3tackle {

// Heart and shield for one fish record; empty = the item can't be used for that fish (RF3's 99).
using HeartShield = std::optional<std::array<int32_t, 2>>;

struct Item {
    int32_t index = -1; // RLRE / RLIN record
    std::string name;
    std::map<int32_t, HeartShield> values; // by fishdata record
};

struct Target {
    int32_t record = -1;
    std::string name;  // with RF3's "(Juv)" / "(Lnkr)" suffix
    HeartShield rod;   // the box's rod for this fish
};

struct Box {
    std::string key;   // "bass", "trout"
    std::string title; // "Tackle Box Bass"
    int32_t rod = -1;
    std::string rodName;
    std::string reelName;
    std::vector<Target> targets;
    std::vector<Item> lures;
    std::vector<Item> lines; // the 5 Line Dia cells, thin to thick
};

struct Sprite {
    std::string piece; // texture name under textures/rf3tackle/
    int32_t mode = 1;  // 1 plain, 5 tiled (guess)
    std::array<int32_t, 4> rect{}; // x0, y0, x1, y1 on RF3's 640 x 480 screen
    std::array<int32_t, 4> rgba{}; // 0x80 = 1.0
    std::array<bool, 2> flip{};
};

struct Element {
    std::string name;
    std::string kind; // IMGX TXTX NUMX LBOX OTHX
    int32_t index = -1;
    int32_t flags = 0;
    std::array<int32_t, 6> head{};
    std::optional<std::array<int32_t, 4>> box;
    std::vector<Sprite> sprites;
    std::vector<std::array<int32_t, 6>> quad; // OTHX: x, y, r, g, b, a per vertex
};

// A picture piece, stored as one or more horizontal strips (SoH loads at most 4096 texels per
// texture). path = "__OTR__textures/rf3tackle/<texture>", ready for gDPLoadTextureBlock; the strings
// live as long as the assets.
struct Strip {
    std::string path;
    int32_t y = 0; // first row in the piece
    int32_t h = 0;
};
struct Piece {
    int32_t w = 0;
    int32_t h = 0;
    int32_t texW = 0; // stored texture width: w padded to a multiple of 4 (SoH's RGBA32 line size)
    std::vector<Strip> strips;
};

struct Assets {
    bool loaded = false;       // both JSON files read and parsed
    bool texturesOk = false;   // every texture they name is in the archives
    int32_t texturesNeeded = 0;
    int32_t texturesMissing = 0;
    std::vector<Box> boxes;    // index = RF3_TACKLE_BOX_*
    std::map<std::string, std::string> texts; // finish, line_dia, ...
    std::array<int32_t, 2> fontCell{};
    std::vector<std::array<int32_t, 2>> fontSpans; // by RF3 text code: left, right
    std::vector<bool> fontInked;
    std::vector<std::string> digitNames;
    std::array<int32_t, 2> digitSize{};
    int32_t digitPitch = 0;
    std::map<std::string, std::vector<Element>> layouts; // "menu_01", "menu_02"
    std::map<std::string, Piece> pieces;
    std::vector<std::string> fontPaths;  // by RF3 text code; empty = no glyph (space)
    std::vector<std::string> digitPaths; // 0-9, minus, plus
};

// Loads on the first call (later calls return the same result). Safe to call every frame.
const Assets& Get();
// "textures/rf3tackle/<name>", as the archives hold it (drawing needs the "__OTR__" prefix).
std::string TexturePath(const std::string& name);

// Lookups in a box; nullptr if absent.
const Item* FindLine(const Box& box, int32_t index);
const Item* FindLure(const Box& box, int32_t index);
const Target* FindTarget(const Box& box, int32_t record);
const Piece* FindPiece(const std::string& name);
// An element of a layout by name; nullptr if absent.
const Element* FindElement(const std::string& layout, const std::string& name);

} // namespace rf3tackle
