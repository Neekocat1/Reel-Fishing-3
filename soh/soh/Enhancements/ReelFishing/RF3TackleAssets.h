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

#include "RF3Sim.h"

namespace rf3tackle {

// Heart and shield for one fish record; empty = the item can't be used for that fish (RF3's 99).
using HeartShield = std::optional<std::array<int32_t, 2>>;

struct Item {
    int32_t index = -1; // RLRE / RLIN record
    std::string name;
    std::map<int32_t, HeartShield> values; // by fishdata record
    // Step 18: raw per-fish-record bytes (index = fishdata record 0..47, 99 = none). Empty when the
    // archive predates step 18a.
    std::vector<int32_t> heart;
    std::vector<int32_t> shield;
    std::vector<int32_t> appeal; // lures only (RLRE +0x50)
    // Step 20a-5: lures only, the movement fields (RF3-MECHANICS 3.2). False for flies, baits,
    // lines, floats, and archives built before step 20.
    bool hasMove = false;
    rf3::LureMove move;
    // Step 20c-3b: flies only, RFLY +0x0E: 0 dry, 1 sunken; -1 = not a fly or an archive before step 20c.
    int32_t sunk = -1;
    // Step 35a: lures and flies, the item menu's type page (record +0x2C; -1 = none or an older archive), and
    // every item's RF3 description ("" if none).
    int32_t type = -1;
    std::string desc;
};

// Step 19c: a bait (RBIT group) and its 5 hook sizes, small to big (sizes[k] = RBIT record 5 * index + k).
struct Bait {
    int32_t index = -1;
    std::string name;
    std::string hook; // the bait's hook type
    int32_t hookIndex = -1; // step 33b: its hook model (model/hook/hookNNN), -1 in an older tackle.json
    std::string desc;       // step 35a: RF3's description
    std::vector<Item> sizes;
};

struct Target {
    int32_t record = -1;
    std::string name;  // with RF3's "(Juv)" / "(Lnkr)" suffix
    HeartShield rod;   // the box's rod for this fish
};

struct Box {
    std::string kind;  // step 19a: "lure", "fly" (the item row shows lures or flies)
    std::string key;   // "bass", "trout", ...
    std::string title; // "Tackle Box Bass"
    int32_t rod = -1;
    std::string rodName;
    std::string reelName;
    std::vector<Target> targets;
    std::vector<Item> lures;
    std::vector<Item> lines; // the 5 Line Dia cells, thin to thick
    // Step 18: the rod's numbers (RF3-MECHANICS 1.3, 1.4, 1.7). Empty/0 before step 18a.
    int32_t rodValue = 0;
    int32_t rodLengthMm = 0;
    std::vector<int32_t> rodLimits; // tension limit per line step 0..4
    std::vector<int32_t> rodHeart;  // per fish record
    std::vector<int32_t> rodShield;
    // Step 19c, bait/pole sets (kind "bait"): their layout (menu_03/04/05), floats and baits.
    std::string layout;      // "menu_02" for lure and fly boxes
    std::vector<Item> floats;
    std::vector<Bait> baits;
    std::string hookDefault; // the hook RF3 shows with no bait chosen
    int32_t hookDefaultIndex = -1; // step 33b: its hook model, -1 in an older tackle.json
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

// Step 58b (layout.json "button_icons", tools/rf3_menuassets.py): the icon drawn instead of an RF3 L1/R1/L2/R2
// plaque piece (the stick left/right, C-Left/C-Right, each with its arrow).
struct ButtonIcon {
    std::string icon;
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
    std::vector<int32_t> fishRodMask;    // step 18: per fish record, bit n = rod n allowed
    std::vector<std::string> depthNames; // step 19c: Ldr below Flt, left to right (Surface .. Deepest)
    // Step 35a: the item menu's type names by box kind ("lure": Top Water .. Bottom, "fly": Dry .. Streamer)
    std::map<std::string, std::vector<std::string>> typeNames;
    std::map<std::string, ButtonIcon> buttonIcons; // step 58: by plaque piece; empty with an older archive
    // Step 57c-3 (tackle.json "conditions", written by tools/rf3_menuassets.py, 57c-2): RF3's weather counts per
    // [season][time][weather] over its area records, and RF3's names (system.trf) for the three conditions.
    bool weatherCountsOk = false;
    int32_t weatherCounts[3][3][4] = {};
    std::vector<std::string> seasonNames;  // Spring, Summer, Autumn
    std::vector<std::string> weatherNames; // Sunny, Cloudy, Fog, Dense fog
    std::vector<std::string> timeNames;    // "Result (Morning)" .. (the results screens' titles)
    // Step 57d ("conditions" "season_sizes"): RF3's mean fish length per species (RF3 species id) and season, mm.
    std::map<int32_t, std::array<float, 3>> seasonMeanMm;
};

// Loads on the first call (later calls return the same result). Safe to call every frame.
const Assets& Get();
// "textures/rf3tackle/<name>", as the archives hold it (drawing needs the "__OTR__" prefix).
std::string TexturePath(const std::string& name);
// Step 76: one of RF3's texts from tackle.json (the player's disc), trimmed; the fallback (the mod's own wording) when
// the archive or the text is missing. Returns a string that lives as long as the program.
const char* Text(const std::string& key, const char* fallback);

// Lookups in a box; nullptr if absent.
const Item* FindLine(const Box& box, int32_t index);
const Item* FindLure(const Box& box, int32_t index);
const Target* FindTarget(const Box& box, int32_t record);
const Item* FindFloat(const Box& box, int32_t index);
const Bait* FindBait(const Box& box, int32_t index);
// Step 19c: the bait at a hook size (0..4), or nullptr (no bait, or out of range).
const Item* BaitAtSize(const Box& box, int32_t bait, int32_t size);
const Piece* FindPiece(const std::string& name);
// An element of a layout by name; nullptr if absent.
const Element* FindElement(const std::string& layout, const std::string& name);

} // namespace rf3tackle
