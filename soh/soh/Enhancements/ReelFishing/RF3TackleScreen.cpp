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
// Step 35b: A on the lure/fly, float or bait row opens RF3's item select menu (menu_06) instead of 17f's list:
// type pages on stick left/right, 6 rows with heart/shield/count, the description in the right-hand pane.
// Step 39a: the dark right-hand pane shows the box's rod, line and lure/fly in 3D, laid out as RF3's (user's screenshot).

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
#include "RF3LureModel.h"

#include <fast/interpreter.h> // step 35c: gfx_create_framebuffer

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
int32_t sPage = 0;       // step 35b: the item menu's type page (lure/fly record +0x2C)
bool sHelpOpen = false;  // step 35d: RF3's help popup over the item menu (L)
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

// Step 55b (user's rando test, 2026-10-18): with no rod owned the screen shows no box: "No rod." in the rod pane, no
// rows, no input (RF3 never has no rod; the text is the mod's).
bool NoRod() {
    return RF3Stock_AnyRod() == 0;
}
constexpr const char* kNoRodText = "No rod.";

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
// Step 39a-2: RF3 pixels subtracted from every rectangle, so a pane's background can be drawn into its own
// framebuffer (whose origin is the pane's corner). 0 everywhere else.
float sOriginX = 0.0f;
float sOriginY = 0.0f;

void TexRect(Gfx*& g, float x0, float y0, float x1, float y1, float s0, float t0, float sw, float th) {
    if (x1 <= x0 || y1 <= y0 || sw <= 0 || th <= 0) {
        return;
    }
    x0 -= sOriginX;
    x1 -= sOriginX;
    y0 -= sOriginY;
    y1 -= sOriginY;
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
void DrawNumber(Gfx*& g, const Element& e, int32_t value, bool sign = true) { // step 35b: sign off for counts
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
    if (value != 0 && sign) {
        cell(0, value > 0 ? 11 : 10);
    }
    int32_t v = std::abs(value);
    int32_t limit = 1;
    for (int i = 0; i < digits; i++) {
        limit *= 10;
    }
    v = std::min(v, limit - 1);
    // Step 35b-3: without a sign (the item menu's x count) the digits start in the box's own cell (they sat one
    // cell to the right, user).
    const int first = sign ? 1 : 0;
    for (int i = digits - 1 + first; i >= first; i--) {
        cell(i, v % 10);
        v /= 10;
        if (v == 0) {
            break; // no leading zeros
        }
    }
    sCounts.numbers++;
}

// Step 35b: an OTHX quad (RF3's flat panes) as one filled rectangle in its first vertex's colour.
void FillQuad(Gfx*& g, const Element& e) {
    if (e.quad.size() < 4) {
        return;
    }
    int32_t x0 = e.quad[0][0], y0 = e.quad[0][1], x1 = e.quad[0][0], y1 = e.quad[0][1];
    for (const auto& q : e.quad) {
        x0 = std::min(x0, q[0]);
        y0 = std::min(y0, q[1]);
        x1 = std::max(x1, q[0]);
        y1 = std::max(y1, q[1]);
    }
    gDPPipeSync(g++);
    gDPSetCombineMode(g++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);
    gDPSetPrimColor(g++, 0, 0, Prim(e.quad[0][2]), Prim(e.quad[0][3]), Prim(e.quad[0][4]), Prim(e.quad[0][5]));
    gDPFillRectangle(g++, static_cast<s32>(x0 * kScale), static_cast<s32>(y0 * kScale), static_cast<s32>(x1 * kScale),
                     static_cast<s32>(y1 * kScale));
    gDPPipeSync(g++);
    gDPSetCombineMode(g++, G_CC_MODULATEIA_PRIM, G_CC_MODULATEIA_PRIM);
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
        // Step 35b-2 (user's RF3 screenshot): while the item menu is open RF3 keeps only the two top panels: no
        // L1/R1 arrows (Image01-off) and no right-hand pane (Image07 and its frame Image00); menu_06 has its own.
        if (sMode == MODE_LIST && layout == "menu_01" && (n == "Image01-off" || n == "Image07" || n == "Image00")) {
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

// Step 35b: RF3's item select menu (menu_06, DESIGN step 35), drawn over the tackle screen while a lure, fly,
// float or bait is being chosen. It replaces 17f's list in the right-hand pane. Lures and flies are split into
// RF3's type pages (record +0x2C; stick left/right, RF3's L1/R1); floats and baits have one page. As RF3
// (0x00229470) it lists every collected item, also those with none left ("Out of Tackle", can't be attached).
constexpr int32_t kMenuRows = 6;

struct MenuEntry {
    int32_t index = -1; // RLRE/RFLY item, RFLT float, RBIT bait (-1 = "---", no bait)
    std::string name;
    std::string desc;
    HeartShield hs;      // for the target
    int32_t count = 0;
    bool lost = false;   // collected, none left
};

int32_t StockKindOf(const Box& box, RowKind kind) {
    if (kind == RK_FLOAT) {
        return RF3_STOCK_FLOAT;
    }
    if (kind == RK_BAIT) {
        return RF3_STOCK_BAIT;
    }
    return box.kind == "fly" ? RF3_STOCK_FLY : RF3_STOCK_LURE;
}

// The type pages of a row: lure 5 / fly 4 from tackle.json's type_names; 1 for floats, baits, or an archive
// without types.
int32_t PageCount(const Box& box, RowKind kind) {
    if (kind != RK_ITEM) {
        return 1;
    }
    auto it = Get().typeNames.find(box.kind);
    return (it != Get().typeNames.end() && !it->second.empty()) ? static_cast<int32_t>(it->second.size()) : 1;
}

std::string PageName(const Box& box, RowKind kind, int32_t page) {
    if (kind != RK_ITEM) {
        return ""; // RF3's blank header (0x41) for floats and baits
    }
    auto it = Get().typeNames.find(box.kind);
    if (it == Get().typeNames.end() || page < 0 || page >= static_cast<int32_t>(it->second.size())) {
        return "";
    }
    return it->second[page];
}

// The entries on a page, in record order [assumed: RF3's own order inside a type was not read].
std::vector<MenuEntry> MenuEntries(const Box& box, RowKind kind, int32_t page) {
    std::vector<MenuEntry> out;
    RF3TackleChoice c;
    RF3Tackle_Get(&c);
    const int32_t stock = StockKindOf(box, kind);
    const bool paged = PageCount(box, kind) > 1;
    auto add = [&](int32_t index, const std::string& name, const std::string& desc, const HeartShield& hs) {
        if (!RF3Stock_Has(stock, index)) {
            return;
        }
        MenuEntry e;
        e.index = index;
        e.name = name;
        e.desc = desc;
        e.hs = hs;
        e.count = RF3Stock_Count(stock, index);
        e.lost = e.count <= 0;
        out.push_back(std::move(e));
    };
    if (kind == RK_ITEM) {
        for (const Item& i : box.lures) {
            if (!paged || i.type == page || (page == 0 && i.type < 0)) {
                add(i.index, i.name, i.desc, ValueFor(&i, c.target));
            }
        }
    } else if (kind == RK_FLOAT) {
        for (const Item& i : box.floats) {
            add(i.index, i.name, i.desc, ValueFor(&i, c.target));
        }
    } else if (kind == RK_BAIT) {
        // Step 35b-2: no "---" entry: RF3's list starts at the first bait (user's RF3 screenshot).
        for (const Bait& b : box.baits) {
            add(b.index, b.name, b.desc, ValueFor(BaitAtSize(box, b.index, c.size), c.target));
        }
    }
    return out;
}

std::vector<MenuEntry> CurrentEntries(const Box& box) {
    return MenuEntries(box, sEdit, sPage);
}

int32_t EntryAt(const std::vector<MenuEntry>& entries, int32_t index) {
    for (size_t i = 0; i < entries.size(); i++) {
        if (entries[i].index == index) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

// The page an item is on (its type), or the first page with an entry.
int32_t PageOf(const Box& box, RowKind kind, int32_t index) {
    const int32_t pages = PageCount(box, kind);
    if (pages <= 1) {
        return 0;
    }
    if (const Item* i = FindLure(box, index); i != nullptr && i->type >= 0 && i->type < pages) {
        return i->type;
    }
    for (int32_t p = 0; p < pages; p++) {
        if (!MenuEntries(box, kind, p).empty()) {
            return p;
        }
    }
    return 0;
}

void ScrollToPreview(const std::vector<MenuEntry>& entries) {
    int32_t at = std::max(0, EntryAt(entries, sPreview));
    int32_t n = static_cast<int32_t>(entries.size());
    if (at < sListTop) {
        sListTop = at;
    } else if (at >= sListTop + kMenuRows) {
        sListTop = at - kMenuRows + 1;
    }
    sListTop = std::max(0, std::min(sListTop, std::max(0, n - kMenuRows)));
}

// Step 35d: RF3's help popup's description (menu_06 `explain`): its lines (RF3's own line breaks) from the box's top
// left, inside the popup's dark quad (window2), letters shrunk only if they wouldn't fit.
void DrawExplain(Gfx*& g, const Element& box, const std::string& desc) {
    const Element* inner = FindElement("menu_06", "window2");
    if (!box.box || inner == nullptr || !inner->box || desc.empty()) {
        return;
    }
    const float x0 = static_cast<float>((*box.box)[0]);
    const float y0 = static_cast<float>((*box.box)[1]);
    const float x1 = static_cast<float>((*inner->box)[2]) - 8;
    const float y1 = static_cast<float>((*inner->box)[3]) - 8;
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= desc.size()) {
        size_t nl = desc.find('\n', start);
        std::string line = desc.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        while (!line.empty() && line.back() == ' ') {
            line.pop_back();
        }
        lines.push_back(line);
        if (nl == std::string::npos) {
            break;
        }
        start = nl + 1;
    }
    float widest = 1;
    for (const std::string& l : lines) {
        widest = std::max(widest, TextWidth(l, 1.0f));
    }
    const float gh = static_cast<float>(Get().fontCell[1]);
    const float lineGap = 4.0f; // [assumed]
    float scale = std::min(1.0f, (x1 - x0) / widest);
    scale = std::min(scale, (y1 - y0) / (lines.size() * (gh + lineGap)));
    float y = y0;
    for (const std::string& l : lines) {
        if (!l.empty()) {
            DrawString(g, x0, y, l, 0, gh * scale, scale);
        }
        y += (gh + lineGap) * scale;
    }
}

// Step 35b-3: the hovered row pulses (user, from RF3). Its panel is drawn again as a white wash shaped by the
// texture's alpha. 35b-5 (user): only normal -> brighter -> normal, no darker half [strength assumed: RF3's own
// values were not read].
constexpr float kPulseFrames = 40.0f; // 35b-4 (user): half 35b-3's speed, one cycle per 2 s at 20 draws/s
constexpr float kPulseStrength = 0.35f;

void PulsePanel(Gfx*& g, const Element& e) {
    // 0 (normal) .. 1 (brightest) and back, once per kPulseFrames
    const float t =
        0.5f - 0.5f * std::cos(2.0f * static_cast<float>(M_PI) * static_cast<float>(sFrames) / kPulseFrames);
    const int shade = 255;
    const int alpha = static_cast<int>(std::lround(t * kPulseStrength * 255.0f));
    if (alpha <= 0) {
        return;
    }
    gDPPipeSync(g++);
    // colour = PRIMITIVE, alpha = TEXEL0 x PRIMITIVE alpha
    gDPSetCombineLERP(g++, 0, 0, 0, PRIMITIVE, TEXEL0, 0, PRIMITIVE, 0, 0, 0, 0, PRIMITIVE, TEXEL0, 0, PRIMITIVE, 0);
    gDPSetPrimColor(g++, 0, 0, shade, shade, shade, alpha);
    for (auto s = e.sprites.rbegin(); s != e.sprites.rend(); ++s) {
        DrawPiece(g, s->piece, static_cast<float>(s->rect[0]), static_cast<float>(s->rect[1]),
                  static_cast<float>(s->rect[2]), static_cast<float>(s->rect[3]));
    }
    gDPPipeSync(g++);
    gDPSetCombineMode(g++, G_CC_MODULATEIA_PRIM, G_CC_MODULATEIA_PRIM);
}

// Step 35c: the previewed item's 3D model in the right-hand pane (sbox1), drawn by RF3LureModel_DrawMenuPreview into
// this screen's own framebuffer (fixed size: the pane at OoT's 320 x 240), then shown with an image rectangle as SoH's
// framebuffer effects do. The pane's colour clears it.
int sPreviewFb = -1;
bool sPreviewDrawn = false; // this frame, for the log

void DrawPreview(Gfx*& g, PlayState* play, const Element& pane, const Box& box, const MenuEntry& entry) {
    sPreviewDrawn = false;
    if (!pane.box || entry.index < 0) {
        return;
    }
    const int32_t x0 = static_cast<int32_t>(std::lround((*pane.box)[0] * kScale));
    const int32_t y0 = static_cast<int32_t>(std::lround((*pane.box)[1] * kScale));
    const int32_t x1 = static_cast<int32_t>(std::lround((*pane.box)[2] * kScale));
    const int32_t y1 = static_cast<int32_t>(std::lround((*pane.box)[3] * kScale));
    const int32_t w = x1 - x0;
    const int32_t h = y1 - y0;
    if (w <= 0 || h <= 0) {
        return;
    }
    if (sPreviewFb < 0) {
        // 35c-2: scaled with the window (native = OoT's 320 x 240, so the pane renders at the window's resolution;
        // fixed size drew it at 107 x 131, pixelated: user); fixed aspect: no widescreen squeeze in a small buffer.
        sPreviewFb = gfx_create_framebuffer(w, h, SCREEN_WIDTH, SCREEN_HEIGHT, true, true);
        WriteLog("preview framebuffer=" + std::to_string(sPreviewFb) + " size=" + std::to_string(w) + "x" +
                 std::to_string(h));
    }
    uint8_t clear[3] = { 0, 0, 0 };
    if (!pane.quad.empty()) {
        clear[0] = static_cast<uint8_t>(Prim(pane.quad[0][2]));
        clear[1] = static_cast<uint8_t>(Prim(pane.quad[0][3]));
        clear[2] = static_cast<uint8_t>(Prim(pane.quad[0][4]));
    }
    if (!RF3LureModel_DrawMenuPreview(play, StockKindOf(box, sEdit), entry.index, sPreviewFb, w, h,
                                      sFrames, clear)) { // 35c-2: the screen's counter (gameplayFrames stops while paused)
        return; // no RF3 model (OoT's pole lures): the dark pane stays
    }
    gDPPipeSync(g++);
    gDPSetOtherMode(g++,
                    G_AD_NOISE | G_CD_NOISE | G_CK_NONE | G_TC_FILT | G_TF_POINT | G_TT_NONE | G_TL_TILE | G_TD_CLAMP |
                        G_TP_NONE | G_CYC_1CYCLE | G_PM_NPRIMITIVE,
                    G_AC_NONE | G_ZS_PRIM | G_RM_OPA_SURF | G_RM_OPA_SURF2);
    gDPSetCombineLERP(g++, 0, 0, 0, TEXEL0, 0, 0, 0, 1, 0, 0, 0, TEXEL0, 0, 0, 0, 1);
    gDPSetTextureImageFB(g++, 0, 0, 0, sPreviewFb);
    gDPImageRectangle(g++, x0 << 2, y0 << 2, 0, 0, x1 << 2, y1 << 2, w, h, G_TX_RENDERTILE, w, h);
    // back to the screen's own settings (RF3Pause_DrawTackleScreen)
    gDPPipeSync(g++);
    gDPSetCycleType(g++, G_CYC_1CYCLE);
    gDPSetRenderMode(g++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
    gDPSetCombineMode(g++, G_CC_MODULATEIA_PRIM, G_CC_MODULATEIA_PRIM);
    gDPSetTexturePersp(g++, G_TP_NONE);
    gDPSetTextureFilter(g++, G_TF_POINT);
    sPreviewDrawn = true;
}

// Step 39a: RF3's rod pane (DESIGN "Step 39 design"): the box's rod, the line and the lure/fly, drawn by
// RF3LureModel_DrawRodPane into a second framebuffer over Image07, then shown as 35c's preview. 39a-2: SoH clears
// framebuffers opaque (alpha 1; the first build's transparent clear showed black: user), so Image07's own dark wood
// is drawn into the framebuffer first. 39b: bait sets show their float and bait. Logged when the state changes.
int sRodPaneFb = -1;
std::string sRodPaneLogged;

void DrawRodPane(Gfx*& g, PlayState* play, const Element& pane, const Values& v) {
    if (!pane.box || v.box == nullptr) {
        return;
    }
    const int32_t x0 = static_cast<int32_t>(std::lround((*pane.box)[0] * kScale));
    const int32_t y0 = static_cast<int32_t>(std::lround((*pane.box)[1] * kScale));
    const int32_t x1 = static_cast<int32_t>(std::lround((*pane.box)[2] * kScale));
    const int32_t y1 = static_cast<int32_t>(std::lround((*pane.box)[3] * kScale));
    const int32_t w = x1 - x0;
    const int32_t h = y1 - y0;
    if (w <= 0 || h <= 0) {
        return;
    }
    if (sRodPaneFb < 0) {
        sRodPaneFb = gfx_create_framebuffer(w, h, SCREEN_WIDTH, SCREEN_HEIGHT, true, true); // as 35c-2
        WriteLog("rod-pane framebuffer=" + std::to_string(sRodPaneFb) + " size=" + std::to_string(w) + "x" +
                 std::to_string(h));
    }
    const bool bait = v.box->kind == "bait";
    int32_t stock = -1;
    int32_t index = -1;
    int32_t floatIdx = -1;
    int32_t depth = 0;
    if (!bait) {
        stock = StockKindOf(*v.box, RK_ITEM);
        if (v.lure != nullptr && !RF3Tackle_LureMissing()) { // 32a: a snapped line comes back empty
            index = v.lure->index;
        }
    } else {
        // 39b: the bait (none, "---" or taken: the bare hook, index -1) and the float at its depth
        stock = RF3_STOCK_BAIT;
        if (v.bait != nullptr && !RF3Tackle_LureMissing()) {
            index = v.bait->index;
        }
        floatIdx = v.flt != nullptr ? v.flt->index : -1;
        depth = v.depthCell;
    }
    const float paneRf3[4] = { static_cast<float>((*pane.box)[0]), static_cast<float>((*pane.box)[1]),
                               static_cast<float>((*pane.box)[2]), static_cast<float>((*pane.box)[3]) };
    // 39a-2: the pane's wood as a list for the framebuffer: Image07 drawn as on screen, moved to the buffer's corner
    constexpr int32_t kBgMax = 1024; // Image07 is 15 sprites: ~200 commands
    Gfx* bg = static_cast<Gfx*>(Graph_Alloc(play->state.gfxCtx, kBgMax * sizeof(Gfx)));
    Gfx* b = bg;
    gDPPipeSync(b++);
    gDPSetCycleType(b++, G_CYC_1CYCLE);
    gDPSetRenderMode(b++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
    gDPSetCombineMode(b++, G_CC_MODULATEIA_PRIM, G_CC_MODULATEIA_PRIM);
    gDPSetTexturePersp(b++, G_TP_NONE);
    gDPSetTextureFilter(b++, G_TF_POINT);
    const Counts screenCounts = sCounts; // the log counts the screen's own drawing only
    sOriginX = paneRf3[0];
    sOriginY = paneRf3[1];
    DrawElement(b, "menu_01", pane, v);
    sOriginX = 0.0f;
    sOriginY = 0.0f;
    sCounts = screenCounts;
    gDPPipeSync(b++);
    gDPSetTexturePersp(b++, G_TP_PERSP); // the 3D models' textures (their lists don't set it)
    gDPSetTextureFilter(b++, G_TF_BILERP);
    gSPEndDisplayList(b++);
    const int32_t bgUsed = static_cast<int32_t>(b - bg);
    char state[300];
    const int32_t boxIndex = static_cast<int32_t>(v.box - Get().boxes.data()); // RF3's tackle set number
    const int32_t drawn = RF3LureModel_DrawRodPane(play, sRodPaneFb, w, h, bg, paneRf3, v.box->rod, boxIndex, stock,
                                                   index, floatIdx, depth, state, sizeof(state));
    std::string line = std::string(state) + " box=" + v.box->key + " bg_cmds=" + std::to_string(bgUsed);
    if (line != sRodPaneLogged) {
        sRodPaneLogged = line;
        WriteLog(line);
    }
    if (!drawn) {
        return; // OoT's pole: the dark pane stays
    }
    gDPPipeSync(g++);
    gDPSetOtherMode(g++,
                    G_AD_NOISE | G_CD_NOISE | G_CK_NONE | G_TC_FILT | G_TF_POINT | G_TT_NONE | G_TL_TILE | G_TD_CLAMP |
                        G_TP_NONE | G_CYC_1CYCLE | G_PM_NPRIMITIVE,
                    G_AC_NONE | G_ZS_PRIM | G_RM_OPA_SURF | G_RM_OPA_SURF2); // opaque: the wood is in the buffer
    gDPSetCombineLERP(g++, 0, 0, 0, TEXEL0, 0, 0, 0, 1, 0, 0, 0, TEXEL0, 0, 0, 0, 1);
    gDPSetTextureImageFB(g++, 0, 0, 0, sRodPaneFb);
    gDPImageRectangle(g++, x0 << 2, y0 << 2, 0, 0, x1 << 2, y1 << 2, w, h, G_TX_RENDERTILE, w, h);
    // back to the screen's own settings (RF3Pause_DrawTackleScreen)
    gDPPipeSync(g++);
    gDPSetCycleType(g++, G_CYC_1CYCLE);
    gDPSetRenderMode(g++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
    gDPSetCombineMode(g++, G_CC_MODULATEIA_PRIM, G_CC_MODULATEIA_PRIM);
    gDPSetTexturePersp(g++, G_TP_NONE);
    gDPSetTextureFilter(g++, G_TF_POINT);
}

void DrawItemMenu(Gfx*& g, PlayState* play, const Values& v) {
    if (v.box == nullptr) {
        return;
    }
    auto it = Get().layouts.find("menu_06");
    if (it == Get().layouts.end()) {
        return;
    }
    const std::vector<MenuEntry> entries = CurrentEntries(*v.box);
    const int32_t n = static_cast<int32_t>(entries.size());
    const int32_t at = EntryAt(entries, sPreview);
    auto rowEntry = [&](int32_t row) -> const MenuEntry* {
        int32_t i = sListTop + row;
        return (i >= 0 && i < n) ? &entries[i] : nullptr;
    };
    const bool paged = PageCount(*v.box, sEdit) > 1;
    // Back to front, as the other layouts (the last element is the bottom layer).
    for (auto e = it->second.rbegin(); e != it->second.rend(); ++e) {
        const std::string& nm = e->name;
        bool drawn = false;
        // Step 35b-2: RF3's SELECT=HELP popup (its frame window1, dark quad window2, sbox2, texts info-title and
        // explain, totals numbox31/32 and info-nodata) is hidden: drawn always it covered the rows (user). The
        // type header and its tabs only with more than one page (RF3's bait screen has neither).
        // Step 35d: L shows the popup (help) with the previewed item's name, description and heart/shield.
        const bool popupPart = nm == "window1" || nm == "window2" || nm == "info-title" || nm == "explain" ||
                               nm == "numbox31" || nm == "numbox32" || nm == "info-nodata";
        if (nm == "sbox2" || (popupPart && (!sHelpOpen || at < 0)) || (!paged && (nm == "L1R1" || nm == "syurui"))) {
            continue;
        }
        if (popupPart && e->kind != "IMGX" && e->kind != "OTHX") {
            const MenuEntry& cur = entries[at];
            if (nm == "info-title") {
                DrawText(g, *e, cur.name);
            } else if (nm == "explain") {
                DrawExplain(g, *e, cur.desc);
            } else if (cur.hs) { // numbox31 heart, numbox32 shield
                DrawNumber(g, *e, (*cur.hs)[nm == "numbox31" ? 0 : 1]);
            } else {
                continue;
            }
            sCounts.elements++;
            sDrawn += (sDrawn.empty() ? "" : " ") + std::string("menu_06/") + nm;
            continue;
        }
        if (e->kind == "OTHX") {
            FillQuad(g, *e); // window2 behind the list, sbox1/2 the right-hand pane
            if (nm == "sbox1" && at >= 0) {
                DrawPreview(g, play, *e, *v.box, entries[at]); // step 35c: under the frame and the lost stamp
            }
            drawn = true;
        } else if (e->kind == "IMGX") {
            if (nm.rfind("nodata", 0) == 0 && nm != "info-nodata") {
                const MenuEntry* r = rowEntry(std::atoi(nm.c_str() + 6));
                if (r == nullptr || r->hs) {
                    continue; // only a row whose item has no value for this target (RF3's 99)
                }
            } else if (nm == "info-nodata") {
                if (entries[at].hs) {
                    continue; // step 35d: the popup's item has no value for this target
                }
            } else if (nm == "up") {
                if (sListTop <= 0) {
                    continue;
                }
            } else if (nm == "down") {
                if (sListTop + kMenuRows >= n) {
                    continue;
                }
            } else if (nm == "lost") {
                if (at < 0 || !entries[at].lost) {
                    continue;
                }
            }
            for (auto s = e->sprites.rbegin(); s != e->sprites.rend(); ++s) {
                DrawSprite(g, *s);
            }
            if (nm.rfind("panel", 0) == 0 && at >= 0 && std::atoi(nm.c_str() + 5) == at - sListTop) {
                PulsePanel(g, *e); // step 35b-3: the hovered row
            }
            drawn = true;
        } else if (e->kind == "TXTX") {
            std::string text;
            if (nm == "syurui") {
                text = PageName(*v.box, sEdit, sPage);
            } else if (nm.rfind("name", 0) == 0 && nm.size() == 5) {
                if (const MenuEntry* r = rowEntry(nm[4] - '1'); r != nullptr) {
                    text = r->name;
                }
            } else if (nm == "popup-text") {
                text = "L button=HELP"; // step 35d: RF3's "SELECT button=HELP" (0x217) with the mod's L
            } else if (nm == "get-text") {
                // Step 35b-2: RF3's "(X) button=Attach" (system.trf 0x215); the mod attaches with A.
                auto t = Get().texts.find("attach");
                text = t != Get().texts.end() ? t->second : "";
                if (size_t p = text.find("<e2><e3>"); p != std::string::npos) {
                    text.replace(p, 8, "A");
                }
            }
            if (text.empty() || !e->box) {
                continue;
            }
            // Long names are narrowed to end before the row's numbers (heart at x 160).
            float w = TextWidth(text, 1.0f);
            float room = 330.0f - static_cast<float>((*e->box)[0]);
            // 35b-4: only the name rows (get-text starts right of x 330: a negative room drew it invisibly, user)
            const bool nameRow = nm.rfind("name", 0) == 0;
            float scale = (!nameRow || w <= room) ? 1.0f : room / w;
            DrawString(g, static_cast<float>((*e->box)[0]), static_cast<float>((*e->box)[1]), text, 0,
                       static_cast<float>(Get().fontCell[1]), scale);
            drawn = true;
        } else if (e->kind == "NUMX" && nm.rfind("numbox", 0) == 0) {
            int which = std::atoi(nm.c_str() + 6);
            if (which >= 13 && which <= 24) { // heart, shield per row
                const MenuEntry* r = rowEntry((which - 13) / 2);
                if (r == nullptr || !r->hs) {
                    continue;
                }
                DrawNumber(g, *e, (*r->hs)[(which - 13) % 2]);
            } else if (which >= 25 && which <= 30) { // x count per row
                const MenuEntry* r = rowEntry(which - 25);
                if (r == nullptr || r->index < 0) {
                    continue;
                }
                DrawNumber(g, *e, r->count, false);
            } else if (which == 31 || which == 32) { // the whole tackle's totals with the previewed item
                if (!v.total) {
                    continue;
                }
                DrawNumber(g, *e, (*v.total)[which - 31]);
            } else {
                continue;
            }
            drawn = true;
        }
        // Step 35b-3: BaitList (LBOX) isn't drawn: its sprite drawn as a cursor showed as a squared-off piece on
        // the row's left (user); the hovered row pulses instead (PulsePanel).
        if (drawn) {
            sCounts.elements++;
            sDrawn += (sDrawn.empty() ? "" : " ") + std::string("menu_06/") + nm;
        }
    }
    // Step 35c: the description left the pane for the model; it comes back in RF3's help popup (35d).
}

void CloseList(const char* how) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s-%s-%s preview=%d moves=%d", kRowKindNames[sEdit], kModeNames[sMode], how,
                  sPreview, sListMoves);
    RequestLog(buf);
    sMode = MODE_ROWS;
    sPreview = -1;
    sListMoves = 0;
    sHelpOpen = false; // step 35d
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
    if (NoRod()) {
        text += " noRod=1"; // step 55b
    }
    text += " texts=[" + (v.box ? v.box->title : std::string("?")) + " | " +
            (v.target ? v.target->name : std::string("?")) + " | " + (v.box ? v.box->rodName : "?") + " | " +
            (v.box ? v.box->reelName : "?") + " | " + (v.line ? v.line->name : std::string("?")) + " | " +
            (v.lure ? v.lure->name : std::string("-")) + " | " + (v.flt ? v.flt->name : std::string("-")) + " | " +
            (v.bait ? v.bait->name : std::string("-")) + "]";
    if (sMode == MODE_LIST && v.box != nullptr) { // step 35b: the item menu
        std::vector<MenuEntry> entries = CurrentEntries(*v.box);
        int32_t at = EntryAt(entries, sPreview);
        char menu[200];
        std::snprintf(menu, sizeof(menu),
                      " menu06 page=%d/%d pageName=\"%s\" entries=%d at=%d top=%d lost=%d count=%d preview3d=%d fb=%d help=%d",
                      sPage, PageCount(*v.box, sEdit), PageName(*v.box, sEdit, sPage).c_str(),
                      static_cast<int>(entries.size()), at, sListTop, at >= 0 && entries[at].lost ? 1 : 0,
                      at >= 0 ? entries[at].count : -1, sPreviewDrawn ? 1 : 0, sPreviewFb, sHelpOpen ? 1 : 0);
        text += menu;
        if (at >= 0) {
            text += " item=\"" + entries[at].name + "\" descChars=" + std::to_string(entries[at].desc.size());
        }
    }
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
    if (NoRod()) { // step 55b: nothing to choose
        if (sMode != MODE_ROWS) {
            CloseList("no-rod");
        }
        return;
    }

    const Box* listBox = CurrentBox();
    if (sMode != MODE_ROWS && listBox != nullptr) {
        RF3TackleChoice c;
        RF3Tackle_Get(&c);
        if (sMode == MODE_LIST) {
            // Step 35d: L opens RF3's help popup (RF3's SELECT); while it shows only L or B do anything (both close
            // it), as RF3's popup.
            const bool l = CHECK_BTN_ALL(input->press.button, BTN_L);
            if (sHelpOpen) {
                if (l || CHECK_BTN_ALL(input->press.button, BTN_B)) {
                    sHelpOpen = false;
                    Sfx(NA_SE_SY_CANCEL);
                    RequestLog("help-close");
                }
                return;
            }
            if (l) {
                sHelpOpen = true;
                Sfx(NA_SE_SY_DECIDE);
                RequestLog("help-open");
                return;
            }
        }
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
            // Step 35b: stick left/right turns RF3's type page (L1/R1), one page per push.
            const int32_t pages = PageCount(*listBox, sEdit);
            int lx = StickStep(sRepX, pauseCtx->stickRelX);
            if (lx != 0 && sRepX.frames == 0 && pages > 1) {
                sPage = (sPage + lx + pages) % pages;
                std::vector<MenuEntry> pageEntries = CurrentEntries(*listBox);
                sPreview = pageEntries.empty() ? -1 : pageEntries[0].index;
                sListTop = 0;
                sListMoves++;
                Sfx(NA_SE_SY_CURSOR);
                RequestLog("page");
            }
            std::vector<MenuEntry> entries = CurrentEntries(*listBox);
            int ly = StickStep(sRepY, pauseCtx->stickRelY);
            int at = std::max(0, EntryAt(entries, sPreview));
            int next = at + (ly > 0 ? -1 : (ly < 0 ? 1 : 0)); // stick up = the entry above
            if (ly != 0 && next >= 0 && next < static_cast<int>(entries.size())) {
                sPreview = entries[next].index;
                sListMoves++;
                ScrollToPreview(entries);
                Sfx(NA_SE_SY_CURSOR);
                RequestLog("move");
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
        // Step 21d: skip the boxes of rods Link doesn't own (back to the same box if he owns only one).
        for (int tries = 1; tries < count && !RF3Tackle_BoxOwned(next); tries++) {
            next = (next + x + count) % count;
        }
        if (next != c.box && RF3Tackle_SetBox(next, "screen-box")) {
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
            // Step 35b: RF3's item menu, opened on the chosen item's type page.
            int32_t current = kind == RK_ITEM ? c.lure : (kind == RK_FLOAT ? c.floatIdx : c.bait);
            int32_t page = PageOf(*box, kind, current);
            std::vector<MenuEntry> entries = MenuEntries(*box, kind, page);
            if (entries.empty()) {
                return; // nothing collected for this row
            }
            sMode = MODE_LIST;
            sEdit = kind;
            sPage = page;
            sPreview = EntryAt(entries, current) >= 0 ? current : entries[0].index;
            sListMoves = 0;
            sRepX = {};
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
    const bool noRod = NoRod(); // step 55b
    Values v = noRod ? Values{} : CurrentValues();
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
        if (it == Get().layouts.end() || (sMode == MODE_LIST && layout != "menu_01") ||
            (noRod && layout != "menu_01")) {
            continue; // step 35b-2: the item menu hides the tackle rows (RF3's screenshot); step 55b: no rod, no rows
        }
        for (auto e = it->second.rbegin(); e != it->second.rend(); ++e) {
            DrawElement(g, layout, *e, v);
            if (layout == "menu_01" && e->name == "Image07" && sMode != MODE_LIST) {
                DrawRodPane(g, play, *e, v); // step 39a: the 3D rod pane on top of the dark pane
                if (noRod && e->box) { // step 55b: centred in the pane
                    const auto& b = *e->box;
                    const float h = static_cast<float>(Get().fontCell[1]);
                    DrawString(g, (b[0] + b[2]) * 0.5f - TextWidth(kNoRodText, 1.0f) * 0.5f, (b[1] + b[3]) * 0.5f - h * 0.5f,
                               kNoRodText, 0, h, 1.0f);
                }
            }
        }
    }
    if (sMode == MODE_LIST && v.box != nullptr) {
        DrawItemMenu(g, play, v); // step 35b: RF3's menu_06 on top
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
