// Reel Fishing III mod (step 17e): RF3's tackle box screen in place of the Fishing pause page.
// Built from RF3's own layouts (menu_01 + menu_02) and pictures, which tools/rf3_menuassets.py makes
// from the player's disc (mods/rf3_tackle.o2r; RF3TackleAssets.cpp loads them). RF3 draws on a
// 640 x 480 screen; this draws the same rectangles at half size on OoT's 320 x 240 screen with
// texture rectangles on the overlay display list, on top of the pause menu.
// Controls (user's decisions, DESIGN step 17): stick up/down moves the red cursor between RF3's rows
// (Reel Lines, Lures, Finish); stick left/right switches tackle box (RF3's L1/R1); C-Left/C-Right
// change the target fish (RF3's L2/R2); A on Finish goes back to the Equipment page; the D-pad equips the
// rod (RF3PausePage.cpp).
// Step 17f: A on Reel Lines makes the Line Dia bar active (stick left/right moves RF3's blue cell);
// A on Lures opens a list of the box's lures in the right-hand pane (stick up/down). Both preview the
// choice live; A keeps it, B goes back. RF3 itself picks lures from its 3D tackle trays, which this 2D
// screen doesn't have, so the lure list is the mod's own, drawn with RF3's font and cursor.
// Step 19: every box. Lure and fly boxes use menu_02; the bait/pole sets use menu_03/04/05 with rows for
// the float (list), float depth (bar), line (bar), bait (list, "---" = none) and hook size (bar). The
// cursor stops on the rows each layout's List box has anchors for (RF3's own rows).
// Log, don't look: every open, change and close goes to logs/rf3_menu.txt with what was drawn.

#include "RF3TackleScreen.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

#include <ship/Context.h>

#include "RF3Bridge.h"
#include "RF3PausePage.h"
#include "RF3TackleAssets.h"

extern "C" {
#include "functions.h"
#include "macros.h"
#include "variables.h"
}

namespace {

using namespace rf3tackle;

// Step 19c: what a cursor row edits. Each layout has its own rows, in RF3's order (the List box's
// anchors, top to bottom).
enum RowKind { RK_LINE, RK_ITEM, RK_FLOAT, RK_DEPTH, RK_BAIT, RK_SIZE, RK_FINISH };
const char* kRowKindNames[] = { "lines", "lures", "float", "depth", "bait", "size", "finish" };

constexpr int kStickThreshold = 30; // same as the pause menu's own cursor
constexpr int kRepeatDelay = 8;     // game frames (20 Hz) before a held stick repeats
constexpr int kRepeatEvery = 3;
constexpr float kScale = 0.5f;      // RF3's 640 x 480 -> OoT's 320 x 240
constexpr int kScreenW = 640;
constexpr int kScreenH = 480;

int sRow = 1; // index into RowsFor(): RF3 opens on the item row (Lures; Baits & Hooks in the user's screenshots)

// Step 17f / 19c: the open bar or list, if any. The previewed value is shown everywhere (names,
// numbers, the bar) but only saved when A keeps it.
enum Mode { MODE_ROWS, MODE_BAR, MODE_LIST };
const char* kModeNames[] = { "rows", "bar", "list" };
Mode sMode = MODE_ROWS;
RowKind sEdit = RK_LINE; // the row the bar or list edits
int32_t sPreview = -1;   // RLIN line, RLRE/RFLY item, RFLT float, depth 0..4, RBIT bait (-1 none), size 0..4
int32_t sListTop = 0;    // first entry shown in the pane
int32_t sListMoves = 0;  // cursor moves since the bar or list opened (logged on close)
u32 sFrames = 0;         // for the active cell's pulse
bool sOpen = false;
bool sLogPending = false;
std::string sLogWhy;

struct Repeat {
    int dir = 0;
    int frames = 0;
};
Repeat sRepX, sRepY;

// Per-frame drawing counts, logged with each open and change.
struct Counts {
    int elements = 0;
    int sprites = 0;
    int strips = 0;
    int glyphs = 0;
    int numbers = 0;
    int missingPieces = 0;
};
Counts sCounts;
std::string sDrawn; // names of the elements drawn, for the log

// -1, 0 or +1 for a stick axis: once on the push, then repeated while held.
int StickStep(Repeat& r, int value) {
    int dir = value > kStickThreshold ? 1 : (value < -kStickThreshold ? -1 : 0);
    if (dir == 0) {
        r = {};
        return 0;
    }
    if (dir != r.dir) {
        r.dir = dir;
        r.frames = 0;
        return dir;
    }
    r.frames++;
    return (r.frames >= kRepeatDelay && (r.frames - kRepeatDelay) % kRepeatEvery == 0) ? dir : 0;
}

void Sfx(u16 id) {
    Audio_PlaySfxGeneral(id, &gSfxDefaultPos, 4, &gSfxDefaultFreqAndVolScale, &gSfxDefaultFreqAndVolScale,
                         &gSfxDefaultReverb);
}

void WriteLog(const std::string& text) {
    std::string dir = Ship::Context::GetPathRelativeToAppDirectory("logs");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(dir + "/rf3_menu.txt", std::ios::app);
    char stamp[32];
    std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    out << stamp << " screen " << text << "\n";
}

void RequestLog(const std::string& why) {
    sLogPending = true;
    sLogWhy = sLogWhy.empty() ? why : sLogWhy + "," + why;
}

const Box* CurrentBox() {
    const Assets& a = Get();
    RF3TackleChoice c;
    RF3Tackle_Get(&c);
    return (c.box >= 0 && c.box < static_cast<int32_t>(a.boxes.size())) ? &a.boxes[c.box] : nullptr;
}

const std::string& LayoutOf(const Box* box) {
    static const std::string kLure = "menu_02";
    return (box != nullptr && !box->layout.empty()) ? box->layout : kLure;
}

// The cursor rows of a box's layout. menu_03/04/05 differ by which settings the set has: menu_04 (the
// All-Purpose set, one float) has no float row, menu_05 (bitterling, one float, no depth) has neither.
const std::vector<RowKind>& RowsFor(const Box* box) {
    static const std::vector<RowKind> kLure = { RK_LINE, RK_ITEM, RK_FINISH };
    static const std::vector<RowKind> k03 = { RK_FLOAT, RK_DEPTH, RK_LINE, RK_BAIT, RK_SIZE, RK_FINISH };
    static const std::vector<RowKind> k04 = { RK_DEPTH, RK_LINE, RK_BAIT, RK_SIZE, RK_FINISH };
    static const std::vector<RowKind> k05 = { RK_LINE, RK_BAIT, RK_SIZE, RK_FINISH };
    const std::string& l = LayoutOf(box);
    return l == "menu_03" ? k03 : (l == "menu_04" ? k04 : (l == "menu_05" ? k05 : kLure));
}

// The row the cursor starts on: the lure/fly or bait row, as in the user's RF3 screenshots.
int DefaultRow(const Box* box) {
    const std::vector<RowKind>& rows = RowsFor(box);
    for (size_t i = 0; i < rows.size(); i++) {
        if (rows[i] == RK_ITEM || rows[i] == RK_BAIT) {
            return static_cast<int>(i);
        }
    }
    return 0;
}

RowKind CurrentRowKind() {
    const std::vector<RowKind>& rows = RowsFor(CurrentBox());
    return rows[std::clamp(sRow, 0, static_cast<int>(rows.size()) - 1)];
}

// --- Drawing ---------------------------------------------------------------------------------

int Prim(int32_t v) { // RF3 colour 0x80 = 1.0
    return v * 2 > 255 ? 255 : v * 2;
}

// One texture rectangle: RF3 screen rectangle (x0, y0)-(x1, y1) showing texels [s0, s0 + sw) x
// [t0, t0 + th) of the loaded texture.
void TexRect(Gfx*& g, float x0, float y0, float x1, float y1, float s0, float t0, float sw, float th) {
    if (x1 <= x0 || y1 <= y0 || sw <= 0 || th <= 0) {
        return;
    }
    // Screen in 10.2 fixed point (RF3 x 0.5 x 4), texture start in 10.5, steps in 5.10.
    s32 xl = static_cast<s32>(std::lround(x0 * kScale * 4));
    s32 yl = static_cast<s32>(std::lround(y0 * kScale * 4));
    s32 xh = static_cast<s32>(std::lround(x1 * kScale * 4));
    s32 yh = static_cast<s32>(std::lround(y1 * kScale * 4));
    s32 dsdx = static_cast<s32>(std::lround(sw * 1024.0 / ((x1 - x0) * kScale)));
    s32 dtdy = static_cast<s32>(std::lround(th * 1024.0 / ((y1 - y0) * kScale)));
    gSPWideTextureRectangle(g++, xl, yl, xh, yh, G_TX_RENDERTILE, static_cast<s32>(s0 * 32),
                            static_cast<s32>(t0 * 32), dsdx, dtdy);
}

void LoadTexture(Gfx*& g, const std::string& path, int32_t w, int32_t h) {
    gDPLoadTextureBlock(g++, path.c_str(), G_IM_FMT_RGBA, G_IM_SIZ_32b, w, h, 0, G_TX_NOMIRROR | G_TX_CLAMP,
                        G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
}

// A whole piece stretched over an RF3 rectangle, strip by strip.
void DrawPiece(Gfx*& g, const std::string& name, float x0, float y0, float x1, float y1) {
    const Piece* p = FindPiece(name);
    if (p == nullptr || p->w <= 0 || p->h <= 0) {
        sCounts.missingPieces++;
        return;
    }
    // Strip edges are snapped to the quarter-pixel grid TexRect uses, and each strip starts at the
    // exact (fractional) texel row that falls on its snapped top, so a shrunk or stretched piece samples
    // continuously across strips (user's screenshot, 2026-10-07: a line through the bass shadow).
    float perRow = (y1 - y0) / p->h;
    auto snap = [](float y) { return std::round(y * kScale * 4) / (kScale * 4); };
    for (const Strip& st : p->strips) {
        float top = snap(y0 + st.y * perRow);
        float bottom = snap(y0 + (st.y + st.h) * perRow);
        // Loaded at its stored (padded) width; only the piece's w columns are drawn.
        LoadTexture(g, st.path, p->texW > 0 ? p->texW : p->w, st.h);
        TexRect(g, x0, top, x1, bottom, 0, (top - y0) / perRow - st.y, static_cast<float>(p->w),
                (bottom - top) / perRow);
        sCounts.strips++;
    }
}

// Mode 5 (the wood background): the piece repeated at one texel per RF3 pixel. A 48-pixel tile can't
// use the hardware's wrap (powers of two only), so it is drawn as a grid.
void DrawTiled(Gfx*& g, const rf3tackle::Sprite& s) {
    const Piece* p = FindPiece(s.piece);
    if (p == nullptr || p->w <= 0 || p->h <= 0) {
        sCounts.missingPieces++;
        return;
    }
    int x1 = std::min(s.rect[2], kScreenW);
    int y1 = std::min(s.rect[3], kScreenH);
    for (int y = s.rect[1]; y < y1; y += p->h) {
        for (int x = s.rect[0]; x < x1; x += p->w) {
            DrawPiece(g, s.piece, static_cast<float>(x), static_cast<float>(y), static_cast<float>(x + p->w),
                      static_cast<float>(y + p->h));
        }
    }
}

void DrawSprite(Gfx*& g, const rf3tackle::Sprite& s) {
    gDPSetPrimColor(g++, 0, 0, Prim(s.rgba[0]), Prim(s.rgba[1]), Prim(s.rgba[2]), Prim(s.rgba[3]));
    if (s.mode == 5) {
        DrawTiled(g, s);
    } else {
        DrawPiece(g, s.piece, static_cast<float>(s.rect[0]), static_cast<float>(s.rect[1]),
                  static_cast<float>(s.rect[2]), static_cast<float>(s.rect[3]));
    }
    sCounts.sprites++;
}

// Proportional width of RF3 text at a scale (the font's per-letter widths).
float TextWidth(const std::string& text, float scale) {
    const Assets& a = Get();
    float w = 0;
    for (unsigned char ch : text) {
        int code = static_cast<int>(ch) - 0x10;
        if (code >= 0 && code < static_cast<int>(a.fontSpans.size())) {
            w += static_cast<float>(a.fontSpans[code][1] - a.fontSpans[code][0]) * scale;
        }
    }
    return w;
}

// RF3 text at (x, y). pitch > 0: fixed advance per letter; 0: proportional letters from the font's
// widths. drawH: letter height; scale narrows proportional letters (the lure list).
void DrawString(Gfx*& g, float x, float y, const std::string& text, int32_t pitch, float drawH, float scale) {
    const Assets& a = Get();
    if (a.fontCell[0] <= 0) {
        return;
    }
    float gw = static_cast<float>(a.fontCell[0]);
    float gh = static_cast<float>(a.fontCell[1]);
    gDPSetPrimColor(g++, 0, 0, 255, 255, 255, 255);
    for (unsigned char ch : text) {
        int code = static_cast<int>(ch) - 0x10; // RF3 text code = ASCII - 0x10
        if (code < 0 || code >= static_cast<int>(a.fontSpans.size())) {
            continue;
        }
        float left = 0, right = gw;
        if (pitch == 0) {
            left = static_cast<float>(a.fontSpans[code][0]);
            right = static_cast<float>(a.fontSpans[code][1]);
        }
        if (!a.fontPaths[code].empty()) {
            LoadTexture(g, a.fontPaths[code], a.fontCell[0], a.fontCell[1]);
            TexRect(g, x, y, x + (right - left) * scale, y + drawH, left, 0, right - left, gh);
            sCounts.glyphs++;
        }
        x += pitch > 0 ? static_cast<float>(pitch) : (right - left) * scale;
    }
}

// RF3 text in a TXTX box. head[4] low byte = fixed advance per letter, high byte = letter height
// (0 = proportional letters, 22 high). Checked against the user's screenshot: the fixed boxes (title,
// rod, target) and proportional ones (lure, line) match within a few pixels.
void DrawText(Gfx*& g, const Element& e, const std::string& text) {
    const Assets& a = Get();
    if (!e.box) {
        return;
    }
    int32_t cell = e.head[4];
    int32_t height = (cell >> 8) & 0xFF;
    float drawH = height > 0 ? static_cast<float>(height) : static_cast<float>(a.fontCell[1]);
    DrawString(g, static_cast<float>((*e.box)[0]), static_cast<float>((*e.box)[1]), text, cell & 0xFF, drawH,
               1.0f);
}

// A NUMX box: a sign cell (plus, minus, or empty for 0), then `digits` cells, 18 RF3 pixels apart
// (the user's screenshot: "+ _4", "- _2", "_ _0").
void DrawNumber(Gfx*& g, const Element& e, int32_t value) {
    const Assets& a = Get();
    if (e.sprites.empty() || a.digitPaths.size() < 12) {
        return;
    }
    const rf3tackle::Sprite& s = e.sprites[0];
    float x0 = static_cast<float>(s.rect[0]);
    float y0 = static_cast<float>(s.rect[1]);
    float w = static_cast<float>(s.rect[2] - s.rect[0]);
    float h = static_cast<float>(s.rect[3] - s.rect[1]);
    float pitch = static_cast<float>(a.digitPitch);
    int32_t digits = e.head[0] > 0 ? e.head[0] : 2;
    gDPSetPrimColor(g++, 0, 0, Prim(s.rgba[0]), Prim(s.rgba[1]), Prim(s.rgba[2]), Prim(s.rgba[3]));
    auto cell = [&](int index, int glyph) {
        LoadTexture(g, a.digitPaths[glyph], a.digitSize[0], a.digitSize[1]);
        TexRect(g, x0 + pitch * index, y0, x0 + pitch * index + w, y0 + h, 0, 0,
                static_cast<float>(a.digitSize[0]), static_cast<float>(a.digitSize[1]));
    };
    if (value != 0) {
        cell(0, value > 0 ? 11 : 10);
    }
    int32_t v = std::abs(value);
    int32_t limit = 1;
    for (int i = 0; i < digits; i++) {
        limit *= 10;
    }
    v = std::min(v, limit - 1);
    for (int i = digits; i >= 1; i--) {
        cell(i, v % 10);
        v /= 10;
        if (v == 0) {
            break; // no leading zeros
        }
    }
    sCounts.numbers++;
}

// The heart/shield rows a screen shows (numbox pairs: 3/4 rod, 5/6 line, 7/8 lure or fly, 9/10 bait,
// 11/12 float; 1/2 the totals).
enum Hs { HS_ROD, HS_LINE, HS_ITEM, HS_BAIT, HS_FLOAT, HS_COUNT };

struct Values {
    const Box* box = nullptr;
    const Target* target = nullptr;
    const Item* line = nullptr;
    const Item* lure = nullptr;      // lure or fly
    const Item* flt = nullptr;       // step 19c: float
    const Bait* bait = nullptr;      // step 19c: nullptr = no bait ("---")
    HeartShield rows[HS_COUNT];
    std::optional<std::array<int32_t, 2>> total;
    int32_t lineCell = -1; // the chosen line's place in the box's 5 lines
    int32_t depthCell = -1;
    int32_t sizeCell = -1;
};

HeartShield ValueFor(const Item* item, int32_t target) {
    return (item != nullptr && item->values.count(target)) ? item->values.at(target) : HeartShield{};
}

Values CurrentValues() {
    Values v;
    RF3TackleChoice c;
    RF3Tackle_Get(&c);
    if (sMode != MODE_ROWS) {
        switch (sEdit) {
            case RK_LINE: c.line = sPreview; break;
            case RK_ITEM: c.lure = sPreview; break;
            case RK_FLOAT: c.floatIdx = sPreview; break;
            case RK_DEPTH: c.depth = sPreview; break;
            case RK_BAIT: c.bait = sPreview; break;
            case RK_SIZE: c.size = sPreview; break;
            default: break;
        }
    }
    v.box = CurrentBox();
    if (v.box == nullptr) {
        return v;
    }
    v.target = FindTarget(*v.box, c.target);
    v.line = FindLine(*v.box, c.line);
    if (v.target != nullptr) {
        v.rows[HS_ROD] = v.target->rod;
    }
    v.rows[HS_LINE] = ValueFor(v.line, c.target);
    std::vector<Hs> used = { HS_ROD, HS_LINE };
    if (v.box->kind == "bait") {
        v.flt = FindFloat(*v.box, c.floatIdx);
        v.bait = FindBait(*v.box, c.bait);
        v.rows[HS_FLOAT] = ValueFor(v.flt, c.target);
        // No bait: the row reads 0/0 (user's screenshots); a bait uses its record at the hook size.
        v.rows[HS_BAIT] = v.bait ? ValueFor(BaitAtSize(*v.box, c.bait, c.size), c.target)
                                 : HeartShield(std::array<int32_t, 2>{ 0, 0 });
        v.depthCell = c.depth;
        v.sizeCell = c.size;
        used.push_back(HS_FLOAT);
        used.push_back(HS_BAIT);
    } else {
        v.lure = FindLure(*v.box, c.lure);
        v.rows[HS_ITEM] = ValueFor(v.lure, c.target);
        used.push_back(HS_ITEM);
    }
    std::array<int32_t, 2> sum{ 0, 0 };
    bool all = true;
    for (Hs h : used) {
        if (!v.rows[h]) {
            all = false;
        } else {
            sum[0] += (*v.rows[h])[0];
            sum[1] += (*v.rows[h])[1];
        }
    }
    if (all) {
        v.total = sum;
    }
    for (size_t i = 0; i < v.box->lines.size(); i++) {
        if (v.box->lines[i].index == c.line) {
            v.lineCell = static_cast<int32_t>(i);
        }
    }
    return v;
}

// Which of RF3's elements this screen shows, and what goes in its texts and numbers.
// Not drawn: fish shadows other than the target's, the fly and
// fly-tippet headings, the "Line Dia" text box (hutosa: RF3 shows it as a picture, part of the
// reelline element, so the text doubled it: user, 2026-10-07), the Japanese-only labels
// (hosoi/hutoi/go), the off-screen OTHX quad, and the
// "end" text (Finish Modification is a picture in menu_02's Image04, 212 x 20, not font text).
void DrawElement(Gfx*& g, const std::string& layout, const Element& e, const Values& v) {
    const std::string& n = e.name;
    bool drawn = false;
    if (e.kind == "IMGX") {
        // "Image01-off" / "Image02-off" are the L1/R1 and L2/R2 arrows, which the user's RF3
        // screenshot shows; they are drawn (17e's first build skipped them).
        if (n.rfind("gyoei", 0) == 0) {
            char want[16];
            std::snprintf(want, sizeof(want), "gyoei%02d", v.target ? v.target->record : -1);
            if (n != want) {
                return;
            }
        }
        // Step 19a: menu_02 holds both headings; lure boxes show "Reel Lines" / "Lures", fly boxes
        // "Fly Tippet" / "Flies" (RF3's own pictures).
        bool fly = v.box != nullptr && v.box->kind == "fly";
        if ((n == "flytippett" || n == "fly") && !fly) {
            return;
        }
        if ((n == "reelline" || n == "lure") && fly) {
            return;
        }
        // menu_01 Image06: 48 light-wood tiles over the whole right pane. The user's RF3 screenshot shows
        // the dark pane (Image07) and its frame (Image00) there, so RF3 doesn't show Image06 on this
        // screen (or hides it under the 3D view). Meaning of the element not known.
        if (layout == "menu_01" && n == "Image06") {
            return;
        }
        if (n.rfind("nodata-", 0) == 0) { // a row with no value for this target (RF3's 99)
            Hs row = n == "nodata-rod"    ? HS_ROD
                     : n == "nodata-line" ? HS_LINE
                     : n == "nodata-uki"  ? HS_FLOAT
                     : n == "nodata-bait" ? HS_BAIT
                                          : HS_ITEM;
            if (v.rows[row]) {
                return;
            }
        }
        // Within an element, too, the first sprite is the top layer (menu_02 Image04 lists the
        // "Finish Modification" picture before the button it sits on).
        for (auto s = e.sprites.rbegin(); s != e.sprites.rend(); ++s) {
            DrawSprite(g, *s);
        }
        drawn = true;
    } else if (e.kind == "TXTX") {
        std::string text;
        if (n == "set-name" && v.box) {
            text = v.box->title;
        } else if (n == "fish-name" && v.target) {
            text = v.target->name;
        } else if (n == "rod-name" && v.box) {
            text = v.box->rodName;
        } else if (n == "reel-name" && v.box) {
            text = v.box->reelName;
        } else if (n == "lb" && v.line) {
            text = v.line->name;
        } else if (n == "lure-name" && v.lure) {
            text = v.lure->name;
        } else if (n == "florts-name" && v.flt) { // step 19c, bait sets
            text = v.flt->name;
        } else if (n == "suisin" && v.depthCell >= 0 && v.depthCell < static_cast<int32_t>(Get().depthNames.size())) {
            text = Get().depthNames[v.depthCell]; // the depth word beside the Ldr bar ("Deep")
        } else if (n == "bait-name" && v.box && v.box->kind == "bait") {
            text = v.bait ? v.bait->name : "---"; // RF3's "no bait" (user's screenshots)
        } else if (n == "hook-name" && v.box && v.box->kind == "bait") {
            text = v.bait ? v.bait->hook : v.box->hookDefault;
        }
        if (text.empty()) {
            return;
        }
        DrawText(g, e, text);
        drawn = true;
    } else if (e.kind == "NUMX") {
        // numbox1/2 totals; 3/4 rod, 5/6 line, 7/8 lure or fly, 9/10 bait, 11/12 float (heart, shield).
        static const Hs kRowOf[6] = { HS_ROD, HS_LINE, HS_ITEM, HS_BAIT, HS_FLOAT, HS_FLOAT };
        int which = -1;
        if (n.rfind("numbox", 0) == 0) {
            which = std::atoi(n.c_str() + 6);
        }
        std::optional<int32_t> value;
        if (which == 1 || which == 2) {
            if (v.total) {
                value = (*v.total)[which - 1];
            }
        } else if (which >= 3 && which <= 12) {
            Hs row = kRowOf[(which - 3) / 2];
            if (v.rows[row]) {
                value = (*v.rows[row])[(which - 3) % 2];
            }
        }
        if (!value) {
            return;
        }
        DrawNumber(g, e, *value);
        drawn = true;
    } else if (e.kind == "LBOX" && e.sprites.size() >= 2) {
        if (n == "List") {
            // Sprite 0 is the cursor, placed relative to sprite 1 (row 0's anchor); sprites 1..3 are
            // the rows' anchors.
            size_t anchor = 1 + static_cast<size_t>(sRow);
            if (anchor >= e.sprites.size()) {
                return;
            }
            const rf3tackle::Sprite& cur = e.sprites[0];
            const rf3tackle::Sprite& a0 = e.sprites[1];
            const rf3tackle::Sprite& a = e.sprites[anchor];
            rf3tackle::Sprite moved = cur;
            for (int i = 0; i < 4; i += 2) {
                moved.rect[i] = cur.rect[i] - a0.rect[0] + a.rect[0];
                moved.rect[i + 1] = cur.rect[i + 1] - a0.rect[1] + a.rect[1];
            }
            DrawSprite(g, moved);
            drawn = true;
        } else if (n == "hutoi" || n == "ukisita" || n == "size") {
            // RF3's bars: the layout already drew the five dark cells; this list box's sprites 1..5 are
            // where RF3's blue "selected" piece goes, so only the chosen cell's is drawn, in its own
            // colours. hutoi = Line Dia, ukisita = Ldr below Flt (depth), size = hook size. While a bar
            // is being changed its blue cell pulses.
            RowKind kind = n == "hutoi" ? RK_LINE : (n == "ukisita" ? RK_DEPTH : RK_SIZE);
            int32_t sel = kind == RK_LINE ? v.lineCell : (kind == RK_DEPTH ? v.depthCell : v.sizeCell);
            size_t cell = static_cast<size_t>(sel) + 1;
            if (sel < 0 || cell >= e.sprites.size()) {
                return;
            }
            rf3tackle::Sprite selSprite = e.sprites[cell];
            if (sMode == MODE_BAR && sEdit == kind) {
                selSprite.rgba[3] = (sFrames / 4) % 2 ? 80 : 128;
            }
            DrawSprite(g, selSprite);
            drawn = true;
        }
    }
    if (drawn) {
        sCounts.elements++;
        sDrawn += (sDrawn.empty() ? "" : " ") + layout + "/" + n;
    }
}

// Step 17f: the lure list in the right-hand pane (menu_01 Image07, where RF3 shows its 3D view).
struct ListGeometry {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0; // inside the pane, RF3 pixels
    float scale = 0.75f;                  // letter size vs RF3's 12 x 22
    float rowH = 0;
    int32_t rows = 0;
};
constexpr float kListCursorW = 14;

// The pane list's entries (RF3 index, name) for the row being chosen: lures/flies, floats, or baits
// with "---" (no bait) first.
std::vector<std::pair<int32_t, std::string>> ListEntries(const Box& box, RowKind kind) {
    std::vector<std::pair<int32_t, std::string>> out;
    if (kind == RK_ITEM) {
        for (const Item& i : box.lures) {
            out.push_back({ i.index, i.name });
        }
    } else if (kind == RK_FLOAT) {
        for (const Item& i : box.floats) {
            out.push_back({ i.index, i.name });
        }
    } else if (kind == RK_BAIT) {
        out.push_back({ -1, "---" });
        for (const Bait& b : box.baits) {
            out.push_back({ b.index, b.name });
        }
    }
    return out;
}

ListGeometry ListGeometryFor(const std::vector<std::pair<int32_t, std::string>>& entries) {
    ListGeometry gm;
    const Element* pane = FindElement("menu_01", "Image07");
    if (pane == nullptr || !pane->box) {
        return gm;
    }
    gm.x0 = static_cast<float>((*pane->box)[0]) + 6;
    gm.y0 = static_cast<float>((*pane->box)[1]) + 6;
    gm.x1 = static_cast<float>((*pane->box)[2]) - 6;
    gm.y1 = static_cast<float>((*pane->box)[3]) - 6;
    float widest = 0;
    for (const auto& e : entries) {
        widest = std::max(widest, TextWidth(e.second, 1.0f));
    }
    if (widest > 0) {
        gm.scale = std::min(0.75f, (gm.x1 - gm.x0 - kListCursorW) / widest);
    }
    gm.rowH = Get().fontCell[1] * gm.scale + 3;
    // The last row is kept for the "n/count" line.
    gm.rows = gm.rowH > 0 ? static_cast<int32_t>((gm.y1 - gm.y0 - gm.rowH) / gm.rowH) : 0;
    return gm;
}

int32_t EntryAt(const std::vector<std::pair<int32_t, std::string>>& entries, int32_t index) {
    for (size_t i = 0; i < entries.size(); i++) {
        if (entries[i].first == index) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

void DrawList(Gfx*& g, const std::vector<std::pair<int32_t, std::string>>& entries) {
    ListGeometry gm = ListGeometryFor(entries);
    if (gm.rows <= 0) {
        return;
    }
    const Element* list = FindElement("menu_02", "List");
    float textH = Get().fontCell[1] * gm.scale;
    int32_t at = EntryAt(entries, sPreview);
    int32_t n = static_cast<int32_t>(entries.size());
    for (int32_t r = 0; r < gm.rows && sListTop + r < n; r++) {
        int32_t i = sListTop + r;
        float y = gm.y0 + r * gm.rowH;
        DrawString(g, gm.x0 + kListCursorW, y, entries[i].second, 0, textH, gm.scale);
        if (i == at && list != nullptr && !list->sprites.empty()) {
            // RF3's red cursor (the List box's sprite 0), shrunk to the row
            const rf3tackle::Sprite& cur = list->sprites[0];
            float h = gm.rowH - 2;
            float w = h * (cur.rect[2] - cur.rect[0]) / std::max(1, cur.rect[3] - cur.rect[1]);
            gDPSetPrimColor(g++, 0, 0, 255, 255, 255, 255);
            DrawPiece(g, cur.piece, gm.x0, y, gm.x0 + w, y + h);
        }
    }
    char count[32];
    std::snprintf(count, sizeof(count), "%d/%d", at + 1, n);
    DrawString(g, gm.x1 - TextWidth(count, gm.scale), gm.y1 - textH, count, 0, textH, gm.scale);
}

void ScrollToPreview(const std::vector<std::pair<int32_t, std::string>>& entries) {
    ListGeometry gm = ListGeometryFor(entries);
    if (gm.rows <= 0) {
        return;
    }
    int32_t at = std::max(0, EntryAt(entries, sPreview));
    int32_t n = static_cast<int32_t>(entries.size());
    if (at < sListTop) {
        sListTop = at;
    } else if (at >= sListTop + gm.rows) {
        sListTop = at - gm.rows + 1;
    }
    sListTop = std::max(0, std::min(sListTop, std::max(0, n - gm.rows)));
}

void CloseList(const char* how) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s-%s-%s preview=%d moves=%d", kRowKindNames[sEdit], kModeNames[sMode], how,
                  sPreview, sListMoves);
    RequestLog(buf);
    sMode = MODE_ROWS;
    sPreview = -1;
    sListMoves = 0;
}

bool Visible(PlayState* play) {
    PauseContext* pauseCtx = &play->pauseCtx;
    return RF3Pause_FishingPageActive() && RF3TackleScreen_Usable() && (pauseCtx->pageIndex == PAUSE_EQUIP) &&
           (pauseCtx->state == 6) && (pauseCtx->unk_1E4 == 0) && (pauseCtx->debugState == 0);
}

std::string HsText(const HeartShield& hs) {
    if (!hs) {
        return "none";
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%+d/%+d", (*hs)[0], (*hs)[1]);
    return buf;
}

void LogState(const Values& v) {
    RF3TackleChoice c;
    RF3Tackle_Get(&c);
    char head[400];
    std::snprintf(head, sizeof(head),
                  "%s row=%s mode=%s edit=%s preview=%d box=%d target=%d line=%d lure=%d float=%d depth=%d bait=%d "
                  "size=%d lineCell=%d rod=%s lineHS=%s lureHS=%s floatHS=%s baitHS=%s total=%s elements=%d "
                  "sprites=%d strips=%d glyphs=%d numbers=%d missingPieces=%d",
                  sLogWhy.c_str(), kRowKindNames[CurrentRowKind()], kModeNames[sMode], kRowKindNames[sEdit], sPreview,
                  c.box, c.target, c.line, c.lure, c.floatIdx, c.depth, c.bait, c.size, v.lineCell,
                  HsText(v.rows[HS_ROD]).c_str(), HsText(v.rows[HS_LINE]).c_str(), HsText(v.rows[HS_ITEM]).c_str(),
                  HsText(v.rows[HS_FLOAT]).c_str(), HsText(v.rows[HS_BAIT]).c_str(), HsText(v.total).c_str(),
                  sCounts.elements, sCounts.sprites, sCounts.strips, sCounts.glyphs, sCounts.numbers,
                  sCounts.missingPieces);
    std::string text = head;
    text += " texts=[" + (v.box ? v.box->title : std::string("?")) + " | " +
            (v.target ? v.target->name : std::string("?")) + " | " + (v.box ? v.box->rodName : "?") + " | " +
            (v.box ? v.box->reelName : "?") + " | " + (v.line ? v.line->name : std::string("?")) + " | " +
            (v.lure ? v.lure->name : std::string("-")) + " | " + (v.flt ? v.flt->name : std::string("-")) + " | " +
            (v.bait ? v.bait->name : std::string("-")) + "]";
    if (sLogWhy.find("open") != std::string::npos) {
        text += " drawn=[" + sDrawn + "]";
    }
    WriteLog(text);
}

} // namespace

extern "C" int RF3TackleScreen_ListOpen(void) {
    return sMode != MODE_ROWS ? 1 : 0;
}

extern "C" int RF3TackleScreen_Usable(void) {
    const Assets& a = Get();
    return (a.loaded && a.texturesOk && a.boxes.size() >= 2) ? 1 : 0;
}

extern "C" void RF3TackleScreen_HandleInput(PlayState* play) {
    PauseContext* pauseCtx = &play->pauseCtx;
    Input* input = &play->state.input[0];
    pauseCtx->cursorSpecialPos = 0; // the stick is the screen's; Z/R still turn pages

    const Box* listBox = CurrentBox();
    if (sMode != MODE_ROWS && listBox != nullptr) {
        RF3TackleChoice c;
        RF3Tackle_Get(&c);
        if (sMode == MODE_BAR) {
            // Line: the box's 5 lines; depth and hook size: cells 0..4.
            int lx = StickStep(sRepX, pauseCtx->stickRelX);
            int count = sEdit == RK_LINE ? static_cast<int>(listBox->lines.size()) : 5;
            int at = sPreview;
            if (sEdit == RK_LINE) {
                at = 0;
                for (size_t i = 0; i < listBox->lines.size(); i++) {
                    if (listBox->lines[i].index == sPreview) {
                        at = static_cast<int>(i);
                    }
                }
            }
            int next = at + lx;
            if (lx != 0 && next >= 0 && next < count) {
                sPreview = sEdit == RK_LINE ? listBox->lines[next].index : next;
                sListMoves++;
                Sfx(NA_SE_SY_CURSOR);
            }
        } else {
            std::vector<std::pair<int32_t, std::string>> entries = ListEntries(*listBox, sEdit);
            int ly = StickStep(sRepY, pauseCtx->stickRelY);
            int at = std::max(0, EntryAt(entries, sPreview));
            int next = at + (ly > 0 ? -1 : (ly < 0 ? 1 : 0)); // stick up = the entry above
            if (ly != 0 && next >= 0 && next < static_cast<int>(entries.size())) {
                sPreview = entries[next].first;
                sListMoves++;
                ScrollToPreview(entries);
                Sfx(NA_SE_SY_CURSOR);
            }
        }
        if (CHECK_BTN_ALL(input->press.button, BTN_A)) {
            int32_t ok = 0;
            switch (sEdit) {
                case RK_LINE: ok = RF3Tackle_SetLine(c.box, sPreview, "screen-line"); break;
                case RK_ITEM: ok = RF3Tackle_SetLure(c.box, sPreview, "screen-lure"); break;
                case RK_FLOAT: ok = RF3Tackle_SetFloat(c.box, sPreview, "screen-float"); break;
                case RK_DEPTH: ok = RF3Tackle_SetDepth(c.box, sPreview, "screen-depth"); break;
                case RK_BAIT: ok = RF3Tackle_SetBait(c.box, sPreview, "screen-bait"); break;
                case RK_SIZE: ok = RF3Tackle_SetSize(c.box, sPreview, "screen-size"); break;
                default: break;
            }
            Sfx(ok ? NA_SE_SY_DECIDE : NA_SE_SY_ERROR);
            CloseList(ok ? "kept" : "refused");
            return;
        }
        if (CHECK_BTN_ALL(input->press.button, BTN_B)) {
            Sfx(NA_SE_SY_CANCEL);
            CloseList("cancelled"); // the pause menu's own B (save prompt) is held off while a list is open
            return;
        }
        // C-Left/C-Right still change the target below, so its numbers can be compared while choosing.
    }

    int y = sMode == MODE_ROWS ? StickStep(sRepY, pauseCtx->stickRelY) : 0;
    if (y != 0) {
        int row = sRow + (y > 0 ? -1 : 1); // stick up = the row above
        if (row >= 0 && row < static_cast<int>(RowsFor(listBox).size())) {
            sRow = row;
            Sfx(NA_SE_SY_CURSOR);
            RequestLog("cursor");
        }
    }
    // User's change (2026-10-07): stick left/right switches box (RF3's L1/R1), C-Left/C-Right the
    // target (RF3's L2/R2).
    int x = sMode == MODE_ROWS ? StickStep(sRepX, pauseCtx->stickRelX) : 0;
    if (x != 0 && sRepX.frames == 0) { // one box per push: holding the stick doesn't flicker boxes
        RF3TackleChoice c;
        RF3Tackle_Get(&c);
        int count = std::max(1, static_cast<int>(RF3Tackle_BoxCount()));
        int next = (c.box + x + count) % count;
        if (RF3Tackle_SetBox(next, "screen-box")) {
            sRow = DefaultRow(CurrentBox()); // the new box may have another layout and other rows
            Sfx(NA_SE_SY_DECIDE);
            RequestLog("box");
        }
    }
    const Box* box = CurrentBox();
    int t = CHECK_BTN_ALL(input->press.button, BTN_CRIGHT) ? 1 : (CHECK_BTN_ALL(input->press.button, BTN_CLEFT) ? -1 : 0);
    if (t != 0 && box != nullptr && !box->targets.empty()) {
        RF3TackleChoice c;
        RF3Tackle_Get(&c);
        int n = static_cast<int>(box->targets.size());
        int at = 0;
        for (int i = 0; i < n; i++) {
            if (box->targets[i].record == c.target) {
                at = i;
            }
        }
        int next = (at + t + n) % n;
        if (RF3Tackle_SetTarget(box->targets[next].record, "screen-target")) {
            Sfx(NA_SE_SY_CURSOR);
            RequestLog("target");
        }
    }
    if (sMode == MODE_ROWS && CHECK_BTN_ALL(input->press.button, BTN_A) && box != nullptr) {
        RF3TackleChoice c;
        RF3Tackle_Get(&c);
        RowKind kind = CurrentRowKind();
        if (kind == RK_FINISH) {
            Sfx(NA_SE_SY_DECIDE);
            RequestLog("finish");
            RF3Pause_CloseFishingPage();
        } else if (kind == RK_LINE || kind == RK_DEPTH || kind == RK_SIZE) {
            if (kind == RK_LINE && box->lines.empty()) {
                return;
            }
            sMode = MODE_BAR;
            sEdit = kind;
            sPreview = kind == RK_LINE ? c.line : (kind == RK_DEPTH ? c.depth : c.size);
            sListMoves = 0;
            sRepX = {};
            Sfx(NA_SE_SY_DECIDE);
            RequestLog(std::string(kRowKindNames[kind]) + "-bar-open");
        } else {
            std::vector<std::pair<int32_t, std::string>> entries = ListEntries(*box, kind);
            if (entries.empty()) {
                return;
            }
            sMode = MODE_LIST;
            sEdit = kind;
            sPreview = kind == RK_ITEM ? c.lure : (kind == RK_FLOAT ? c.floatIdx : c.bait);
            sListMoves = 0;
            sRepY = {};
            sListTop = 0;
            ScrollToPreview(entries);
            Sfx(NA_SE_SY_DECIDE);
            RequestLog(std::string(kRowKindNames[kind]) + "-list-open");
        }
    }
}

extern "C" void RF3Pause_DrawTackleScreen(PlayState* play) {
    if (!Visible(play)) {
        if (sOpen) {
            if (sMode != MODE_ROWS) {
                CloseList("cancelled-by-hiding"); // page turn or pause closed: nothing is kept
            }
            sOpen = false;
            std::string why = sLogWhy.empty() ? "" : " after " + sLogWhy;
            WriteLog("close" + why);
            sLogPending = false;
            sLogWhy.clear();
        }
        return;
    }
    if (!sOpen) {
        sOpen = true;
        sRow = DefaultRow(CurrentBox());
        sMode = MODE_ROWS;
        sRepX = {};
        sRepY = {};
        RequestLog("open");
    }

    // OoT draws its HUD after the pause menu, so its A/B/START/C buttons ("Decide", "Save",
    // "Return") would sit on top of the Target panel (user's screenshot, 2026-10-07). Hide them while
    // the screen shows; OoT's own update fades them back in afterwards. The D-pad items stay visible
    // so a D-pad equip can be seen.
    InterfaceContext* interfaceCtx = &play->interfaceCtx;
    interfaceCtx->aAlpha = 0;
    interfaceCtx->bAlpha = 0;
    interfaceCtx->cLeftAlpha = 0;
    interfaceCtx->cDownAlpha = 0;
    interfaceCtx->cRightAlpha = 0;
    interfaceCtx->startAlpha = 0;

    sFrames++;
    Values v = CurrentValues();
    sCounts = {};
    sDrawn.clear();
    OPEN_DISPS(play->state.gfxCtx);
    Gfx_SetupDL_39Overlay(play->state.gfxCtx);
    Gfx* g = OVERLAY_DISP;
    gDPPipeSync(g++);
    gDPSetCycleType(g++, G_CYC_1CYCLE);
    gDPSetRenderMode(g++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
    gDPSetCombineMode(g++, G_CC_MODULATEIA_PRIM, G_CC_MODULATEIA_PRIM);
    gDPSetTexturePersp(g++, G_TP_NONE);
    gDPSetTextureFilter(g++, G_TF_POINT); // strips must meet without filtering seams
    // menu_01 is the base screen, menu_02 the rows on top. Within a layout the last element is the
    // bottom layer (menu_01's last element is the full-screen background), so draw back to front.
    // Step 19c: the box's own layout on top of menu_01 (menu_02 lure/fly, menu_03/04/05 bait sets).
    const std::string rowsLayout = LayoutOf(v.box);
    for (const std::string& layout : { std::string("menu_01"), rowsLayout }) {
        auto it = Get().layouts.find(layout);
        if (it == Get().layouts.end()) {
            continue;
        }
        for (auto e = it->second.rbegin(); e != it->second.rend(); ++e) {
            DrawElement(g, layout, *e, v);
        }
    }
    if (sMode == MODE_LIST && v.box != nullptr) {
        DrawList(g, ListEntries(*v.box, sEdit));
    }
    gDPPipeSync(g++);
    OVERLAY_DISP = g;
    CLOSE_DISPS(play->state.gfxCtx);

    if (sLogPending) {
        LogState(v);
        sLogPending = false;
        sLogWhy.clear();
    }
}
