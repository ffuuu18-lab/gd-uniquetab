// ut_layout.h - where the collection page's three parts go: the item GRID, the button PAD and the
// LABEL, each a rectangle in RECORD px, window-local (the materials window's own coordinates; the
// live side multiplies by the UI scale with floorf(v*s+0.5f) per edge, as it does for the page).
//
// Pure arithmetic: no Windows, no engine, no allocation, no globals. The generator, the plates,
// the live page and tools\test_layout.cpp are meant to run exactly these functions.
//
//   utRectParse / utRectFormat   "x,y,w,h" <-> UtRect (the value of a [layout] key)
//   utCoverKeyParse / Format     "<W>x<H>", the suffix of a [layout] key: the cover size it is for
//   utGridFit                    rectangle + cell size -> cols x rows under the frame's box cap
//                                (THE rule: pages_gen's grid(), verbatim)
//   utFrameBoxes                 the view's frame box count: the cover's fit, 160..512
//   utLatticeFrame               the frame the plate painter draws under one cell of that fit
//                                (every cell of it: the lattice has the page's cols x rows)
//   utPadFit                     rectangle + N buttons -> cols x rows, cell size, caption size
//   utPadCellRect                one button's rectangle inside a fitted pad
//   utLabelFit                   rectangle + requested size -> the size drawn, or refused
//   utLayoutResolve              derived + user rectangles -> the three in force, and for each
//                                user rectangle not in force the reason it was refused; the
//                                pad and the label may instead be pinned to a SCREEN corner
//   utScreenRectParse / Format   "<corner>,x,y,w,h" <-> UtScreenRect (pad_screen, label_screen)
//   utScreenPlace                a screen rectangle -> client px, shifted into the viewport
//   utScreenToLocal / utLocalToClient   client px <-> window-local record px
//   utScreenLocate               the two above in one: where a screen part lands and is drawn
//   utScreenFromClient           a client rectangle -> the key of its nearest corner (the editor)
//   utMatPadResolve              the pad on the game's own materials page: padmat_ or the pad in
//                                force, drawn where it is over the page's boxes; the log's text
//                                (utMatPadText)
//   utLayoutFromSource           the P line + the [layout] values + the pad keys -> the layout
//   utLiveGroupGrid / utRowKeep  a group's cols x rows on the live page, the row across a change
//   utLayoutGridRect             the grid in force for the generator (the plates it paints)
//   utLayoutCanonical            a grid override as the layout plates' hash reads it
//   utLayoutPlatesGrid           what the layout-plates thread paints: the grid in force with the
//                                screen keys pinned (no screen known there), and its canonical text
//   utPlateSetChoice            which plate set the page shows: the layout plates only under the
//                                live page's user grid, the derived ones only under its derived grid
//
// The derived rectangles are what 1.1.0 draws: utPadDefault / utLabelDefault give the default pad
// and label, the grid's is the view's usable rectangle (the P line of uniq-groups.txt).
#pragma once

#include <math.h>
#include <stddef.h>
#include <stdio.h>

#include "ut_fontmetrics.h"
#include "ut_padmat.h"   // the joint band, UtRectF and its overlap test

namespace ut {

struct UtRect {
    int x, y, w, h;   // top-left and size; the right edge is x + w (exclusive)
};

inline bool utRectEq(const UtRect& a, const UtRect& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

// True when `in` lies wholly inside `out`.
inline bool utRectInside(const UtRect& in, const UtRect& out) {
    return in.x >= out.x && in.y >= out.y && in.x + in.w <= out.x + out.w &&
           in.y + in.h <= out.y + out.h;
}

// True when the two share at least one pixel (touching edges do not meet).
inline bool utRectsMeet(const UtRect& a, const UtRect& b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

// ---- text: "x,y,w,h" and "<W>x<H>" ------------------------------------------------------------

// One signed whole number of at most 5 digits, blanks around it skipped. Advances `p`.
inline bool utParseNum(const char*& p, int* v) {
    while (*p == ' ' || *p == '\t') ++p;
    bool neg = false;
    if (*p == '-' || *p == '+') neg = (*p++ == '-');
    if (*p < '0' || *p > '9') return false;
    int acc = 0, digits = 0;
    while (*p >= '0' && *p <= '9') {
        if (++digits > 5) return false;
        acc = acc * 10 + (*p++ - '0');
    }
    while (*p == ' ' || *p == '\t') ++p;
    *v = neg ? -acc : acc;
    return true;
}

// "x,y,w,h": four whole numbers, w and h at least 1, nothing after the fourth. A position may be
// negative (the parse is format only; utLayoutResolve decides whether the rectangle is usable).
inline bool utRectParse(const char* s, UtRect* out) {
    if (!s || !out) return false;
    const char* p = s;
    int v[4];
    for (int i = 0; i < 4; ++i) {
        if (!utParseNum(p, &v[i])) return false;
        if (i < 3) {
            if (*p != ',') return false;
            ++p;
        }
    }
    while (*p == '\r' || *p == '\n') ++p;
    if (*p) return false;
    if (v[2] < 1 || v[3] < 1) return false;
    out->x = v[0];
    out->y = v[1];
    out->w = v[2];
    out->h = v[3];
    return true;
}

// Writes "x,y,w,h". Returns the length, or 0 when it did not fit.
inline int utRectFormat(const UtRect& r, char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    const int n = snprintf(out, cap, "%d,%d,%d,%d", r.x, r.y, r.w, r.h);
    return (n > 0 && (size_t)n < cap) ? n : 0;
}

// "<W>x<H>": 1..4 digits, a lower-case x, 1..4 digits, both at least 1. Nothing else.
inline bool utCoverKeyParse(const char* s, int* w, int* h) {
    if (!s) return false;
    int v[2] = {0, 0};
    const char* p = s;
    for (int i = 0; i < 2; ++i) {
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            if (++digits > 4) return false;
            v[i] = v[i] * 10 + (*p++ - '0');
        }
        if (digits == 0 || v[i] < 1) return false;
        if (i == 0) {
            if (*p != 'x') return false;
            ++p;
        }
    }
    if (*p) return false;
    if (w) *w = v[0];
    if (h) *h = v[1];
    return true;
}

inline int utCoverKeyFormat(int w, int h, char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    const int n = snprintf(out, cap, "%dx%d", w, h);
    return (n > 0 && (size_t)n < cap) ? n : 0;
}

// ---- the grid ----------------------------------------------------------------------------------

// The frame's box count is the view's own (utFrameBoxes below), between these two. 160 is the
// vanilla page's own fit (10 x 16 at 32 px) and the count every view had through 1.2; 512 is
// about 1.4 x the largest window a shipping mod builds (Reign of Terror's components page, 364
// boxes). ReagentWindow::Load has no box limit of its own (a growing std::vector).
const int kUtFrameBoxFloor = 160;
const int kUtFrameBoxMax = 512;
const int kUtRowGap = 2;   // the vanilla 64 px rows sit on a 66 px pitch

struct UtGrid {
    int cols, rows;
    int capped;   // 1 = the cap took rows away (the group paginates further)
};

// cols = w / cellW, rows = h / (cellH + rowGap), each at least 1; rows then walk back while
// cols * rows exceeds `cap`. The slack stays empty at the right and the bottom (top-left anchor).
inline UtGrid utGridFit(const UtRect& r, int cellW, int cellH, int rowGap, int cap) {
    if (cellW < 1) cellW = 1;
    if (cellH < 1) cellH = 1;
    if (rowGap < 0) rowGap = 0;
    UtGrid g;
    g.cols = r.w / cellW;
    if (g.cols < 1) g.cols = 1;
    g.rows = r.h / (cellH + rowGap);
    if (g.rows < 1) g.rows = 1;
    const int full = g.rows;
    while (g.cols * g.rows > cap && g.rows > 1) --g.rows;
    g.capped = g.rows < full ? 1 : 0;
    return g;
}

// The frame's box count for a view: the most boxes ANY grid on the cover can show at any of the
// view's cell sizes - utGridFit over the whole cover plate (coverW x coverH, which every user grid
// lies inside), uncapped - clamped to kUtFrameBoxFloor..kUtFrameBoxMax. So no user grid on the
// cover is capped below the ceiling, and the derived grid (inside the cover) never is either.
// `cellW` / `cellH` are the view's distinct cell sizes (n of them; n == 0 gives the floor).
// Vanilla 438x627 at 32 px: 13 x 18 = 234; Loot Ascension 346x625: 10 x 18 = 180; Reign of
// Terror and X-Mod: over 512, so 512.
inline int utFrameBoxes(int coverW, int coverH, const int* cellW, const int* cellH, int n,
                        int rowGap) {
    int best = 0;
    const UtRect cover = {0, 0, coverW < 1 ? 1 : coverW, coverH < 1 ? 1 : coverH};
    for (int i = 0; cellW && cellH && i < n; ++i) {
        const UtGrid g = utGridFit(cover, cellW[i], cellH[i], rowGap, 1 << 30);
        const int boxes = g.cols * g.rows;
        if (boxes > best) best = boxes;
    }
    if (best < kUtFrameBoxFloor) best = kUtFrameBoxFloor;
    if (best > kUtFrameBoxMax) best = kUtFrameBoxMax;
    return best;
}

// ---- the painted lattice (the cover plates, src\gen\plate_gen.cpp and tools\make_plates.py) ----

const int kUtFrameOutset = 2;   // a painted frame is its cell grown by 2 px on every side
// The lattice rule's number, part of a layout plate set's hash (gen::layoutPlateHash): a set
// painted under an older rule gets a new name and is painted again. 2 = every cell of the fit
// framed (utLatticeFrame); 1 (no number in the hash) dropped a cell whose outset left the plate.
const int kUtLatticeRule = 2;

// The frame painted under cell (col, row) of a grid whose first cell is at x0,y0 (window px) -
// the cell the page puts its box in, x0 + col * cellW, y0 + row * (cellH + rowGap) - on a cover
// plate plateW x plateH drawn at bmpX,bmpY: in PLATE px, the cell grown by kUtFrameOutset on
// every side and then cut to the plate. The painter frames every cell of utGridFit's cols x rows
// with it, so the lattice has exactly the page's columns and rows: a cell flush with the plate's
// edge (Reign of Terror's grid 46,72,928,520: column 29 ends at 974, the plate's width) keeps
// its frame, whose outer line lands on the plate's last pixels over the cell's own edge. A
// painter that skipped such a cell because its OUTSET does not fit would leave that column on the
// bare ground (28 painted columns under 29 of items). False only when the cell itself is not
// wholly on the plate, which no grid the resolver accepts gives (a user grid lies inside the
// plate, the derived one inside the usable rectangle).
inline bool utLatticeFrame(int x0, int y0, int cellW, int cellH, int rowGap, int col, int row,
                           int bmpX, int bmpY, int plateW, int plateH, UtRect* out) {
    if (rowGap < 0) rowGap = 0;
    const int bx = x0 + cellW * col - bmpX;
    const int by = y0 + (cellH + rowGap) * row - bmpY;
    if (cellW < 1 || cellH < 1 || bx < 0 || by < 0 || bx + cellW > plateW || by + cellH > plateH)
        return false;
    UtRect f = {bx - kUtFrameOutset, by - kUtFrameOutset, cellW + 2 * kUtFrameOutset,
                cellH + 2 * kUtFrameOutset};
    if (f.x < 0) { f.w += f.x; f.x = 0; }
    if (f.y < 0) { f.h += f.y; f.y = 0; }
    if (f.x + f.w > plateW) f.w = plateW - f.x;
    if (f.y + f.h > plateH) f.h = plateH - f.y;
    if (out) *out = f;
    return true;
}

// ---- text width (the engine's pen rule over ut_fontmetrics.h) ---------------------------------

// Engine.dll 0xAE550: the baked style nearest the requested size, the first on a tie.
inline int utFontStyleFor(int size) {
    int best = 0, bestD = 1 << 24;
    for (int i = 0; i < fontmetrics::kStyles; ++i) {
        int d = size - (int)fontmetrics::kStyleSize[i];
        if (d < 0) d = -d;
        if (d < bestD) {
            bestD = d;
            best = i;
        }
    }
    return best;
}

// Width in SCREEN px of `s` at `size` in savapromedium (the label's and the pad's font). An upper
// bound, never under. The same arithmetic as ut_panel.cpp's textWidthPx.
inline int utTextWidthPx(const char* s, int size) {
    if (!s || !*s || size <= 0) return 0;
    const int st = utFontStyleFor(size);
    const int ss = (int)fontmetrics::kStyleSize[st];
    if (ss <= 0) return 0;
    int pen = 0;
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
        int c = (int)*p;
        if (c < fontmetrics::kFirstChar || c >= fontmetrics::kFirstChar + fontmetrics::kChars)
            c = (int)'?';
        const int i = c - fontmetrics::kFirstChar;
        pen += (int)fontmetrics::kGlyphBearing[st][i] +
               ((int)fontmetrics::kGlyphW[st][i] * size + ss / 2) / ss;
    }
    return pen + (int)fontmetrics::kStyleSlack[st];
}

// The pad's captions are measured through this, so the fitter never reads a fixed table: the
// caller passes the widest caption at a size (translated or per-category captions included).
typedef int (*UtWidestFn)(int size, const void* ctx);

struct UtCaptions {
    const char* const* tags;
    int n;
};

// A UtWidestFn over a UtCaptions: the widest of `n` captions at `size`.
inline int utWidestCaptionPx(int size, const void* ctx) {
    const UtCaptions* c = (const UtCaptions*)ctx;
    int w = 0;
    for (int i = 0; c && i < c->n; ++i) {
        const int t = utTextWidthPx(c->tags[i], size);
        if (t > w) w = t;
    }
    return w;
}

const int kUtCaptionMin = 6;       // below this a caption is not drawn at all
const int kUtCaptionMaxDefault = 14;

// The caption size for a button of boxW x boxH SCREEN px: three quarters of the height, at most
// boxH - 2 (the two edge lines are inside the button) and at most `capMax`, then down until the
// widest caption fits boxW - 1. 0 = not even size 6 fits. ut_panel.cpp's buttonTagSize with the
// ceiling as a parameter (it is 14 there).
inline int utCaptionSize(int boxW, int boxH, int capMax, UtWidestFn widest, const void* ctx) {
    int top = (boxH * 3) / 4;
    if (top > boxH - 2) top = boxH - 2;
    if (top > capMax) top = capMax;
    for (int cand = top; cand >= kUtCaptionMin; --cand) {
        if (!widest || widest(cand, ctx) <= boxW - 1) return cand;
    }
    return 0;
}

// ---- the pad -----------------------------------------------------------------------------------

const int kUtPadMargin = 3;      // record px left and right of the cells (none above or below)
const int kUtPadMinCellW = 24;   // record px; narrower cells are refused
const int kUtPadMinCellH = 12;   // record px; pad_h's floor, the last height with a size-6 caption
const int kUtPadRowsMax = 12;

struct UtPadFit {
    int ok;              // 1 = a layout was found
    int cols, rows;      // the cell grid
    int cellW, cellH;    // record px
    int gap;             // record px between two cells
    int x0, y0;          // the first cell's top-left, record px
    int buttons;         // N; the LAST button spans two cells
    int caption;         // the caption size in SCREEN px at the scale of the fit
    int empty;           // cells left unused
    int minW, minH;      // the smallest single-width button, SCREEN px
};

// Button `index` of a fitted pad, record px. Buttons 0..N-2 take one cell each in reading order;
// the last one takes two neighbouring cells of one row - the next pair, or the start of the next
// row when only one cell is left in the current one.
inline UtRect utPadCellRect(const UtPadFit& f, int index) {
    UtRect r = {0, 0, 0, 0};
    if (!f.ok || index < 0 || index >= f.buttons || f.cols < 1) return r;
    int cell = index;
    const bool wide = (index == f.buttons - 1);
    if (wide && f.cols >= 2 && cell % f.cols == f.cols - 1) ++cell;
    const int row = cell / f.cols;
    const int col = cell % f.cols;
    r.x = f.x0 + col * (f.cellW + f.gap);
    r.y = f.y0 + row * (f.cellH + f.gap);
    r.w = wide ? 2 * f.cellW + f.gap : f.cellW;
    r.h = f.cellH;
    return r;
}

namespace layout_detail {
inline int roundEdge(int v, float s) { return (int)floorf((float)v * s + 0.5f); }
}  // namespace layout_detail

// Fills `out` with cols, rows and cell size for `buttons` buttons (the last one double wide) in
// `r`, at UI scale `s`. Every rows count 1..kUtPadRowsMax is tried with cols = ceil((N+1)/rows);
// cells narrower than 24 or shorter than 12 record px are refused, and so is a layout whose
// smallest button cannot carry a size-6 caption. The winner has the largest caption (capped at
// `capMax`), then the fewest empty cells, then the fewest rows. The default pad, 102,25,320,44 with
// 26 buttons and gap 1, comes out as the 9 x 3 cells of 34 x 14 that 1.1.0 draws.
inline UtPadFit utPadFit(const UtRect& r, int buttons, int gap, float s, int capMax,
                         UtWidestFn widest, const void* ctx) {
    UtPadFit best = {};
    if (buttons < 1 || r.w < 1 || r.h < 1 || !(s >= 0.2f && s <= 8.0f)) return best;
    if (gap < 0) gap = 0;
    const int need = buttons + 1;   // the last button spans two cells
    for (int rows = 1; rows <= kUtPadRowsMax; ++rows) {
        const int cols = (need + rows - 1) / rows;
        if (cols < 2) continue;    // the double-wide button needs two columns
        UtPadFit f = {};
        f.ok = 1;
        f.cols = cols;
        f.rows = rows;
        f.gap = gap;
        f.buttons = buttons;
        f.x0 = r.x + kUtPadMargin;
        f.y0 = r.y;
        f.cellW = (r.w - 2 * kUtPadMargin - (cols - 1) * gap) / cols;
        f.cellH = (r.h - (rows - 1) * gap) / rows;
        if (f.cellW < kUtPadMinCellW || f.cellH < kUtPadMinCellH) continue;
        // The double-wide button moves to the next row when it would start in the last column.
        int last = buttons - 1;
        if (last % cols == cols - 1) ++last;
        if (last + 2 > rows * cols) continue;
        f.empty = rows * cols - need;
        // The caption is fitted to the smallest single-width button in SCREEN px, both edges
        // rounded as the draw rounds them (the double-wide one can never be the narrowest).
        f.minW = 1 << 24;
        f.minH = 1 << 24;
        const int narrow = buttons > 1 ? buttons - 1 : 1;
        for (int i = 0; i < narrow; ++i) {
            const UtRect c = utPadCellRect(f, i);
            const int sw = layout_detail::roundEdge(c.x + c.w, s) - layout_detail::roundEdge(c.x, s);
            const int sh = layout_detail::roundEdge(c.y + c.h, s) - layout_detail::roundEdge(c.y, s);
            if (sw < f.minW) f.minW = sw;
            if (sh < f.minH) f.minH = sh;
        }
        f.caption = utCaptionSize(f.minW, f.minH, capMax, widest, ctx);
        if (f.caption < kUtCaptionMin) continue;
        const bool better = !best.ok || f.caption > best.caption ||
                            (f.caption == best.caption && f.empty < best.empty);
        if (better) best = f;
    }
    return best;
}

// The default pad from the [advanced] keys, clamped exactly as ut_panel.cpp's stripLayout clamps
// them: 9 x 3 cells of 34 x pad_h, a 3 px margin, x 102.
inline UtRect utPadDefault(int padY, int padH, int padGap) {
    if (padH < 12) padH = 12;
    if (padH > 20) padH = 20;
    if (padGap < 0) padGap = 0;
    if (padGap > 4) padGap = 4;
    const UtRect r = {102, padY, 2 * kUtPadMargin + 9 * 34 + 8 * padGap, 3 * padH + 2 * padGap};
    return r;
}

// ---- the label ---------------------------------------------------------------------------------

const int kUtLabelSizeMin = 9;   // screen px; ut_panel.cpp's floor

struct UtLabelFit {
    int ok;        // 1 = the label can be drawn in the rectangle
    int size;      // the font size drawn, SCREEN px
    int bounded;   // 1 = the rectangle's height brought the requested size down
    int x, top;    // where the text starts, record px: the left edge, one px under the top
    int limitPx;   // the width budget, SCREEN px
};

// The default label: x 105, the band y 8..23, the right limit 422 (the text starts at y 9).
inline UtRect utLabelDefault() {
    const UtRect r = {105, 8, 317, 16};
    return r;
}

// `requested` is plate_label_size (record px). The size is the requested one at the scale, never
// under 9 px; when its text would pass the rectangle's bottom the size comes down until it fits,
// and below 9 px, or with under 8 px of width, the label is refused. (1.1.0 refuses instead of
// bringing the size down; with the default rectangle both agree for every size up to 15.)
inline UtLabelFit utLabelFit(const UtRect& r, int requested, float s) {
    UtLabelFit f = {};
    if (!(s >= 0.2f && s <= 8.0f) || r.w < 1 || r.h < 1) return f;
    const float want = (float)requested * s;
    int fs = (int)(want + (want >= 0.0f ? 0.5f : -0.5f));
    if (fs < kUtLabelSizeMin) fs = kUtLabelSizeMin;
    const int start = fs;
    f.x = r.x;
    f.top = r.y + 1;
    while (fs >= kUtLabelSizeMin && (float)f.top + (float)fs / s > (float)(r.y + r.h)) --fs;
    if (fs < kUtLabelSizeMin) return f;
    const float lim = (float)r.w * s;
    f.limitPx = (int)(lim + 0.5f);
    if (f.limitPx < 8) return f;
    f.size = fs;
    f.bounded = fs < start ? 1 : 0;
    f.ok = 1;
    return f;
}

// ---- resolving the user's rectangles ----------------------------------------------------------

enum { kUtLayGrid = 0, kUtLayPad = 1, kUtLayLabel = 2, kUtLayParts = 3 };

enum UtLayoutWhy {
    kUtLayoutOk = 0,      // the user's rectangle is in force
    kUtLayoutUnset,       // no user rectangle; the derived one is in force
    kUtLayoutOutside,     // not wholly inside the plate
    kUtLayoutGridSmall,   // smaller than the largest cell (its height plus the row gap)
    kUtLayoutPadNoFit,    // no cols x rows gives cells of 24 x 12 with a caption of 6 or more
    kUtLayoutLabelSmall,  // under 9 px of text, or under 8 px of width, at this UI scale
    kUtLayoutOverlap,     // meets another rectangle in force; `against` names it
    // The screen parts only (pad_screen / label_screen):
    kUtLayoutScreenOverGrid,   // its client rectangle meets the item grid in force
    kUtLayoutScreenLarge,      // at this UI scale it is as wide or as tall as the screen
    kUtLayoutScreenNoView      // the screen size or the window's place is not known (yet)
};

inline const char* utLayoutPartName(int part) {
    return part == kUtLayGrid ? "grid" : part == kUtLayPad ? "pad" : part == kUtLayLabel ? "label"
                                                                                       : "?";
}

inline const char* utLayoutWhyText(int why) {
    switch (why) {
        case kUtLayoutOk: return "in force";
        case kUtLayoutUnset: return "not set";
        case kUtLayoutOutside: return "is not inside the page";
        case kUtLayoutGridSmall: return "is smaller than the largest item cell";
        case kUtLayoutPadNoFit: return "has no room for the buttons with readable captions";
        case kUtLayoutLabelSmall: return "is too small for the label text";
        case kUtLayoutOverlap: return "overlaps another part of the page";
        case kUtLayoutScreenOverGrid: return "would cover the item grid";
        case kUtLayoutScreenLarge: return "is as large as the screen";
        case kUtLayoutScreenNoView: return "cannot be placed yet (no screen size or window place)";
        default: return "?";
    }
}

// A record-px rectangle at UI scale `s`, each EDGE rounded with floorf(v*s+0.5f) - the rounding
// the pad draw, the hit test and the box positions share.
inline UtRectF utScaledRectF(const UtRect& r, float s) {
    const float x = floorf((float)r.x * s + 0.5f), y = floorf((float)r.y * s + 0.5f);
    const UtRectF f = {x, y, floorf((float)(r.x + r.w) * s + 0.5f) - x,
                       floorf((float)(r.y + r.h) * s + 0.5f) - y};
    return f;
}

// ---- the screen-anchored pad and label: pad_screen / label_screen ------------------------------
//
// THE MODEL (the one the screen draw, the screen input and the layout editor all build on).
//
// MEANING. `pad_screen` / `label_screen` = "<corner>,x,y,w,h" (UtScreenRect): <corner> is one of
// tl tr bl br (the screen corner the part is pinned to), x,y record px from that corner INWARD to
// the part's nearest edges, w,h the part's size in record px. ONE value for every view, mod and
// caravan cover ("one for all", by design). Record px are the caravan window's own unit:
// the measured UI scale `s` (= the plate ratio) turns them into client px exactly as it does for
// the page, so the part keeps its corner and its UI-scaled margin on every resolution. The grid
// is NEVER a screen part (its boxes are only clickable inside the caravan's rect).
//
// PLACEMENT (utScreenPlace): each number scaled with floorf(v*s+0.5f); left = ix (tl, bl) or
// screenW - ix - pw (tr, br); top = iy (tl, tr) or screenH - iy - ph (bl, br); then SHIFTED, never
// resized, back into 0..screenW x 0..screenH when the offset pushes it past an edge. screenW/H =
// GraphicsCanvas::GetWidth/GetHeight of the frame's canvas (client px = canvas px = the mouse
// event's x/y, both measured in game).
//
// INTO THE PAGE'S MODEL (utScreenToLocal): the client rectangle becomes a WINDOW-LOCAL record
// rectangle relative to the materials window's screen origin (originX/Y: the origin the Draw tail
// draws the page at - the caller's Vec2 + window+0x40/+0x44). It lies outside the plate, often
// far outside (negative too), and that is fine: utPadFit / utLabelFit, the PadPlan, the cells,
// the hover, the outline and utMatPadResolve read it exactly as they read an in-window rectangle,
// and utLocalToClient(rect, originX, originY, s) is where it is drawn (rounded the page's way,
// nudged by whole record px so the drawn rectangle stays on screen).
//
// RESOLUTION (utLayoutResolve). A screen part is IN FORCE when its key parses, the view is known
// (screenKnown), it is smaller than the screen at this scale, its local rectangle fits (the pad:
// utPadFit finds a layout with a readable caption; the label: utLabelFit), and its drawn client
// rectangle does not meet the client rectangle of the GRID in force (kUtLayoutScreenOverGrid -
// the boxes must stay clickable). A screen label that meets a screen pad yields to it
// (kUtLayoutOverlap, against the pad). It skips kUtLayoutOutside and the pairwise rules against
// the IN-WINDOW parts (it is drawn above the page; only the grid is protected). Refused, the part
// falls back to what the page has without the key - the user's in-window pad_/label_<W>x<H> when
// legal, else the derived one - and L.screenWhy says why. In force: L.screen[p] = 1, L.user[p] = 1,
// L.why[p] = kUtLayoutOk, L.rect[p] = the local rectangle, L.screenPx[p] = the client rectangle
// drawn. A refusal is a property of THIS window place: the result changes when the window moves,
// so the caller resolves again whenever screenW/H, originX/Y or the scale change.
//
// THE MATERIALS PAGE (utMatPadResolve): a screen pad stays whole at its corner (kUtMatSrcScreen);
// padmat_ is not used then (kUtMatWhyScreen) and kUtMatWhyOutside never applies to it.
//
// THE CALL SEQUENCE (ut_live.cpp layoutCompute; each step is one line of the caller):
//   1. UtLayoutSource as for the in-window parts, plus padScreen / labelScreen =
//      g_cfg.padScreen / labelScreen (empty = no key), screenKnown, screenW/H (the canvas),
//      originX/Y (the window's origin);
//   2. utLayoutFromSource -> UtLayout L (the screen rectangles are parsed BY VALUE into
//      UtLayoutInput, so a copy of LiveLayout stays self-contained);
//   3. screenKnown, screenW/H and originX/Y join the layout's change signature (LaySig): the
//      window's place is an input;
//   4. a refused screen part is said once per change, like an in-window one: "pad_screen=<value>
//      <utLayoutWhyText(L.screenWhy[p])>" (the in-window WARN keeps reading L.why / L.against).
//
// WHAT THE SCREEN DRAW MUST PUBLISH FOR THE SCREEN INPUT, every frame it draws a screen pad:
//   - the button rectangles in CLIENT px: in.originX/Y (the resolved input's) + the PadPlan's
//     window-local scaled rectangles - one array, written on the draw thread and read by the
//     GetMouseEvent detour on the same thread (measured in game);
//   - each rectangle's logical button (ut_padpage.h) and the pad page on screen;
//   - the pad's whole client rectangle (L.screenPx[kUtLayPad]): a press on the ground between
//     the buttons is consumed and does nothing, as in the window;
//   - the drawn-at tick (GetTickCount of that draw): the input acts only while it is recent
//     (500 ms), so a pad that stopped being drawn (caravan closed, page left) never eats a click.
// The layout editor clamps a screen part to the screen in record px (screenW/s x screenH/s), takes
// the nearest corner on SAVE (utScreenFromClient), writes pad_screen / label_screen through
// configPersistStr (the active view's settings file; a NULL value deletes the line), and clears
// in.screenSet[p] for a part it checks in window mode (utEditCheck) - else the screen part in
// force hides the edited in-window rectangle from the check.

enum UtCorner { kUtCornerTL = 0, kUtCornerTR, kUtCornerBL, kUtCornerBR };

struct UtScreenRect {
    int corner;   // UtCorner: the screen corner the part is pinned to
    int x, y;     // record px from that corner INWARD to the part's nearest edges (>= 0)
    int w, h;     // record px (>= 1)
};

const int kUtScreenMax = 16384;   // the largest number a screen key may hold (record px)

inline const char* utCornerName(int c) {
    switch (c) {
        case kUtCornerTL: return "tl";
        case kUtCornerTR: return "tr";
        case kUtCornerBL: return "bl";
        case kUtCornerBR: return "br";
        default: return "?";
    }
}

inline bool utScreenRectEq(const UtScreenRect& a, const UtScreenRect& b) {
    return a.corner == b.corner && a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

// "<corner>,x,y,w,h": the corner tl|tr|bl|br (any case), then four whole numbers 0..16384, w and
// h at least 1; blanks around every item. A minus sign anywhere is refused (the offset runs
// inward from the corner; there is no negative of it). Nothing after the fifth item.
inline bool utScreenRectParse(const char* s, UtScreenRect* out) {
    if (!s || !out) return false;
    for (const char* q = s; *q; ++q)
        if (*q == '-') return false;
    const char* p = s;
    while (*p == ' ' || *p == '\t') ++p;
    char c0 = p[0];
    char c1 = c0 ? p[1] : (char)0;
    if (c0 >= 'A' && c0 <= 'Z') c0 = (char)(c0 - 'A' + 'a');
    if (c1 >= 'A' && c1 <= 'Z') c1 = (char)(c1 - 'A' + 'a');
    int corner = -1;
    if (c0 == 't' && c1 == 'l') corner = kUtCornerTL;
    if (c0 == 't' && c1 == 'r') corner = kUtCornerTR;
    if (c0 == 'b' && c1 == 'l') corner = kUtCornerBL;
    if (c0 == 'b' && c1 == 'r') corner = kUtCornerBR;
    if (corner < 0) return false;
    p += 2;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p != ',') return false;
    ++p;
    int v[4];
    for (int i = 0; i < 4; ++i) {
        if (!utParseNum(p, &v[i])) return false;
        if (i < 3) {
            if (*p != ',') return false;
            ++p;
        }
    }
    while (*p == '\r' || *p == '\n') ++p;
    if (*p) return false;
    for (int i = 0; i < 4; ++i)
        if (v[i] < 0 || v[i] > kUtScreenMax) return false;
    if (v[2] < 1 || v[3] < 1) return false;
    out->corner = corner;
    out->x = v[0];
    out->y = v[1];
    out->w = v[2];
    out->h = v[3];
    return true;
}

// Writes "tr,8,8,320,44". Returns the length, or 0 when it did not fit.
inline int utScreenRectFormat(const UtScreenRect& r, char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    const int n = snprintf(out, cap, "%s,%d,%d,%d,%d", utCornerName(r.corner), r.x, r.y, r.w, r.h);
    return (n > 0 && (size_t)n < cap) ? n : 0;
}

// Where `r` lands on a screenW x screenH canvas at UI scale `s`, client px (whole numbers held in
// floats): see PLACEMENT above. *shifted = 1 when an edge was brought back into the viewport. A
// zero rectangle for an unusable screen size or scale. A part as large as the screen ends at the
// top-left and still overflows - utScreenLocate refuses it first (kUtLayoutScreenLarge).
inline UtRectF utScreenPlace(const UtScreenRect& r, int screenW, int screenH, float s,
                             int* shifted) {
    UtRectF f = {0.0f, 0.0f, 0.0f, 0.0f};
    if (shifted) *shifted = 0;
    if (screenW < 1 || screenH < 1 || !(s >= 0.2f && s <= 8.0f)) return f;
    const float ix = floorf((float)r.x * s + 0.5f), iy = floorf((float)r.y * s + 0.5f);
    const float pw = floorf((float)r.w * s + 0.5f), ph = floorf((float)r.h * s + 0.5f);
    const bool right = r.corner == kUtCornerTR || r.corner == kUtCornerBR;
    const bool bottom = r.corner == kUtCornerBL || r.corner == kUtCornerBR;
    float left = right ? (float)screenW - ix - pw : ix;
    float top = bottom ? (float)screenH - iy - ph : iy;
    int moved = 0;
    if (left + pw > (float)screenW) {
        left = (float)screenW - pw;
        moved = 1;
    }
    if (left < 0.0f) {
        left = 0.0f;
        moved = 1;
    }
    if (top + ph > (float)screenH) {
        top = (float)screenH - ph;
        moved = 1;
    }
    if (top < 0.0f) {
        top = 0.0f;
        moved = 1;
    }
    if (shifted) *shifted = moved;
    f.x = left;
    f.y = top;
    f.w = pw;
    f.h = ph;
    return f;
}

// Window-local record px -> client px: the page's own rule - the window origin plus each EDGE
// rounded with floorf(v*s+0.5f) (utScaledRectF), which is where the draw puts it.
inline UtRectF utLocalToClient(const UtRect& r, float originX, float originY, float s) {
    UtRectF f = utScaledRectF(r, s);
    f.x += originX;
    f.y += originY;
    return f;
}

// Client px -> window-local record px: the position (px - origin) / s rounded to the nearest
// record px, the size the key's own w x h (never re-derived from px, which a scale under 1 would
// round away). utLocalToClient of the result lies within s/2 + 1 px of `px` per edge.
inline UtRect utScreenToLocal(const UtRectF& px, int w, int h, float originX, float originY,
                              float s) {
    UtRect r = {0, 0, w, h};
    if (!(s >= 0.2f && s <= 8.0f)) return r;
    r.x = (int)floorf((px.x - originX) / s + 0.5f);
    r.y = (int)floorf((px.y - originY) / s + 0.5f);
    return r;
}

struct UtScreenSpot {
    int why;          // kUtLayoutOk, kUtLayoutScreenNoView or kUtLayoutScreenLarge
    UtRect local;     // window-local record px: L.rect of a screen part in force
    UtRectF px;       // client px where it is drawn: utLocalToClient(local, origin, s)
    UtRectF placed;   // client px as utScreenPlace put it (px is within s/2 + 1 of it)
    int shifted;      // 1 = utScreenPlace brought it back into the viewport
};

// Place, convert, and keep the DRAWN rectangle on screen: the local position moves by whole
// record px until utLocalToClient lies inside 0..screenW x 0..screenH (the rounding alone can put
// an edge a pixel past the screen's). screenKnown = 0 (no frame drawn yet, the window's place not
// captured) refuses with kUtLayoutScreenNoView; a part as wide or as tall as the screen with
// kUtLayoutScreenLarge.
inline UtScreenSpot utScreenLocate(const UtScreenRect& r, int screenKnown, int screenW,
                                   int screenH, float originX, float originY, float s) {
    UtScreenSpot o = {};
    o.why = kUtLayoutScreenNoView;
    if (!screenKnown || screenW < 1 || screenH < 1 || !(s >= 0.2f && s <= 8.0f)) return o;
    o.placed = utScreenPlace(r, screenW, screenH, s, &o.shifted);
    if (o.placed.w >= (float)screenW || o.placed.h >= (float)screenH) {
        o.why = kUtLayoutScreenLarge;
        return o;
    }
    o.local = utScreenToLocal(o.placed, r.w, r.h, originX, originY, s);
    for (int k = 0; k < 64; ++k) {
        const UtRectF d = utLocalToClient(o.local, originX, originY, s);
        bool moved = false;
        if (d.x < 0.0f) {
            ++o.local.x;
            moved = true;
        } else if (d.x + d.w > (float)screenW) {
            --o.local.x;
            moved = true;
        }
        if (d.y < 0.0f) {
            ++o.local.y;
            moved = true;
        } else if (d.y + d.h > (float)screenH) {
            --o.local.y;
            moved = true;
        }
        if (!moved) break;
    }
    o.px = utLocalToClient(o.local, originX, originY, s);
    o.why = kUtLayoutOk;
    return o;
}

// The layout editor's SAVE: a client rectangle -> the screen key of its NEAREST corner (the one
// whose two edges are closest; left and top on a tie), offsets and size back in record px at `s`.
inline UtScreenRect utScreenFromClient(const UtRectF& px, int screenW, int screenH, float s) {
    UtScreenRect r = {kUtCornerTL, 0, 0, 1, 1};
    if (!(s >= 0.2f && s <= 8.0f)) return r;
    const float dl = px.x, dr = (float)screenW - (px.x + px.w);
    const float dt = px.y, db = (float)screenH - (px.y + px.h);
    const bool right = dr < dl, bottom = db < dt;
    r.corner = right ? (bottom ? kUtCornerBR : kUtCornerTR) : (bottom ? kUtCornerBL : kUtCornerTL);
    const float ox = right ? dr : dl, oy = bottom ? db : dt;
    r.x = (int)floorf((ox < 0.0f ? 0.0f : ox) / s + 0.5f);
    r.y = (int)floorf((oy < 0.0f ? 0.0f : oy) / s + 0.5f);
    r.w = (int)floorf(px.w / s + 0.5f);
    r.h = (int)floorf(px.h / s + 0.5f);
    if (r.w < 1) r.w = 1;
    if (r.h < 1) r.h = 1;
    if (r.x > kUtScreenMax) r.x = kUtScreenMax;
    if (r.y > kUtScreenMax) r.y = kUtScreenMax;
    if (r.w > kUtScreenMax) r.w = kUtScreenMax;
    if (r.h > kUtScreenMax) r.h = kUtScreenMax;
    return r;
}

struct UtLayoutInput {
    UtRect plate;                 // the cover plate, record px window-local
    UtRect def[kUtLayParts];      // the derived grid, pad and label
    const UtRect* user[kUtLayParts];   // the user's rectangles; null = no key
    int maxCellW, maxCellH;       // the view's largest group cell, record px
    int rowGap;                   // the grid's row gap
    int buttons;                  // the pad's buttons; the last spans two cells
    int padGap;                   // record px between two buttons
    float scale;                  // the UI scale the pad and the label are fitted at
    int captionMax;               // the caption ceiling (pad_caption_max)
    UtWidestFn widest;
    const void* widestCtx;
    int labelSize;                // plate_label_size, record px
    // The screen parts (pad_screen / label_screen), BY VALUE so a copy of the input is
    // self-contained. All zero = no screen key = exactly the in-window resolution. The grid's
    // slot is never read: the grid is never a screen part.
    int screenSet[kUtLayParts];           // 1 = screenRect[p] is the user's key for part p
    UtScreenRect screenRect[kUtLayParts];
    int screenKnown;              // 1 = screenW/H and originX/Y are this frame's
    int screenW, screenH;         // the canvas, client px (GraphicsCanvas::GetWidth/GetHeight)
    float originX, originY;       // the materials window's screen origin, client px
};

struct UtLayout {
    UtRect rect[kUtLayParts];     // the rectangles in force (a screen part's: window-local)
    int user[kUtLayParts];        // 1 = the user's rectangle is the one in force
    int why[kUtLayParts];         // UtLayoutWhy
    int against[kUtLayParts];     // for kUtLayoutOverlap: the part it met, else -1
    UtPadFit pad;                 // the pad fitted in rect[kUtLayPad]
    UtLabelFit label;             // the label fitted in rect[kUtLayLabel]
    int screen[kUtLayParts];      // 1 = the part in force is its screen key (pad_screen ...)
    int screenWhy[kUtLayParts];   // what became of the screen key: Unset, Ok or the refusal
    int screenAgainst[kUtLayParts];   // the part a refused screen key met, else -1
    UtRectF screenPx[kUtLayParts];    // a screen part in force: client px, as drawn
    int screenShifted[kUtLayParts];   // 1 = it was shifted back into the viewport
    // 1 = the part's <part>_<W>x<H> line is set but was NOT judged: its screen key is set, so the
    // line is ignored (no refusal, no overlap; one DEBUG line, utLayoutIgnoredText).
    int ignored[kUtLayParts];
};

namespace layout_detail {
// The in-window resolution (utLayoutResolve's rules for the user's <W>x<H> rectangles). A part
// with skip[p] set is pinned to the screen: it takes no part in the pairwise rules (its in-window
// rectangle is replaced afterwards).
inline UtLayout resolveWindow(const UtLayoutInput& in, const int skip[kUtLayParts]) {
    UtLayout L = {};
    for (int p = 0; p < kUtLayParts; ++p) {
        L.rect[p] = in.def[p];
        L.user[p] = 0;
        L.why[p] = in.user[p] ? kUtLayoutOk : kUtLayoutUnset;
        L.against[p] = -1;
        if (!in.user[p]) continue;
        const UtRect& u = *in.user[p];
        int why = kUtLayoutOk;
        if (!utRectInside(u, in.plate)) {
            why = kUtLayoutOutside;
        } else if (p == kUtLayGrid) {
            if (u.w < in.maxCellW || u.h < in.maxCellH + in.rowGap) why = kUtLayoutGridSmall;
        } else if (p == kUtLayPad) {
            if (!utPadFit(u, in.buttons, in.padGap, in.scale, in.captionMax, in.widest,
                          in.widestCtx).ok)
                why = kUtLayoutPadNoFit;
        } else if (!utLabelFit(u, in.labelSize, in.scale).ok) {
            why = kUtLayoutLabelSmall;
        }
        L.why[p] = why;
        if (why == kUtLayoutOk) {
            L.rect[p] = u;
            L.user[p] = 1;
        }
    }
    static const int kPairs[3][2] = {{kUtLayGrid, kUtLayPad},
                                     {kUtLayGrid, kUtLayLabel},
                                     {kUtLayPad, kUtLayLabel}};
    for (int pass = 0; pass < 2 * kUtLayParts; ++pass) {
        bool moved = false;
        for (int k = 0; k < 3 && !moved; ++k) {
            const int a = kPairs[k][0], b = kPairs[k][1];
            if (skip[a] || skip[b]) continue;
            if (!utRectsMeet(L.rect[a], L.rect[b])) continue;
            const int drop = L.user[b] ? b : L.user[a] ? a : -1;
            if (drop < 0) continue;   // two derived rectangles: legal by construction
            L.rect[drop] = in.def[drop];
            L.user[drop] = 0;
            L.why[drop] = kUtLayoutOverlap;
            L.against[drop] = drop == a ? b : a;
            moved = true;
        }
        if (!moved) break;
    }
    return L;
}
}  // namespace layout_detail

// Each user rectangle is checked on its own first (inside the plate; the grid holds the largest
// cell; the pad fits its buttons; the label its text), then the three in force must be pairwise
// disjoint, in the order grid, pad, label: of two user rectangles that meet, the later one falls
// back to its derived rectangle; a user rectangle that meets a DERIVED one falls back itself.
// This repeats until nothing meets, so the worst case is all three derived - which is what 1.1.0
// draws and is legal by construction. Nothing is clamped: a refused rectangle is refused whole,
// and `why` / `against` say why, for the one WARN line per edit. `scale` must be the measured UI
// scale: outside 0.2..8 the pad and the label do not fit, so a user pad or label is refused.
//
// THE SCREEN PARTS (see "the screen-anchored pad and label" above): a pad or label whose screen
// key is set is placed first (utScreenLocate) and fitted at its local rectangle; the in-window
// rules then run without it; a screen part whose drawn rectangle meets the grid in force is
// refused (kUtLayoutScreenOverGrid) and takes part in the in-window rules again, and so on until
// nothing changes (at most three rounds: a screen part is only ever dropped). A screen label that
// meets a screen pad yields to it. The grid in force never depends on a screen part.
//
// THE PER-COVER LINE OF A PINNED PART. A part is PINNED while its screen key is in force or
// waits to be placed (kUtLayoutScreenNoView: the page has not been drawn once yet, or the screen
// site is not working). A pinned part takes no part in the in-window rules - no in-window part is
// refused for meeting it - and its <part>_<W>x<H> line is not judged at all (never checked, never
// in force, never met): L.ignored[p] says so, which the live page logs once at DEBUG ("ignored
// while the part is on the screen") instead of a refusal. A part waiting to be placed sits at
// its derived rectangle meanwhile. Only a screen key refused for a reason of its own (over the
// grid, as large as the screen, no fit, the label over the screen pad - each its own WARN) puts
// the part back among the in-window parts, where its per-cover line is judged as before.
inline UtLayout utLayoutResolve(const UtLayoutInput& in) {
    UtLayoutInput win = in;   // what the in-window rules see: no per-cover line for a pinned part
    int scr[kUtLayParts] = {0, 0, 0};
    int scrWhy[kUtLayParts] = {kUtLayoutUnset, kUtLayoutUnset, kUtLayoutUnset};
    int scrAgainst[kUtLayParts] = {-1, -1, -1};
    UtScreenSpot spot[kUtLayParts] = {};
    for (int p = kUtLayPad; p < kUtLayParts; ++p) {   // never the grid
        if (!in.screenSet[p]) continue;
        spot[p] = utScreenLocate(in.screenRect[p], in.screenKnown, in.screenW, in.screenH,
                                 in.originX, in.originY, in.scale);
        int why = spot[p].why;
        if (why == kUtLayoutOk) {
            if (p == kUtLayPad) {
                if (!utPadFit(spot[p].local, in.buttons, in.padGap, in.scale, in.captionMax,
                              in.widest, in.widestCtx)
                         .ok)
                    why = kUtLayoutPadNoFit;
            } else if (!utLabelFit(spot[p].local, in.labelSize, in.scale).ok) {
                why = kUtLayoutLabelSmall;
            }
        }
        scrWhy[p] = why;
        scr[p] = why == kUtLayoutOk ? 1 : 0;
    }
    // pinned: on the screen, or waiting to be placed - out of the in-window rules either way
    int pin[kUtLayParts] = {0, 0, 0};
    for (int p = kUtLayPad; p < kUtLayParts; ++p) {
        pin[p] = scr[p] || (in.screenSet[p] && scrWhy[p] == kUtLayoutScreenNoView);
        if (pin[p]) win.user[p] = nullptr;
    }
    UtLayout L = layout_detail::resolveWindow(win, pin);
    for (int round = 0; round < kUtLayParts + 1; ++round) {
        bool dropped = false;
        const UtRectF grid = utLocalToClient(L.rect[kUtLayGrid], in.originX, in.originY, in.scale);
        for (int p = kUtLayPad; p < kUtLayParts; ++p) {
            if (!scr[p] || !utRectsOverlap(spot[p].px, grid)) continue;
            scr[p] = 0;
            pin[p] = 0;
            win.user[p] = in.user[p];   // back in the window: its per-cover line is judged
            scrWhy[p] = kUtLayoutScreenOverGrid;
            scrAgainst[p] = kUtLayGrid;
            dropped = true;
        }
        if (scr[kUtLayPad] && scr[kUtLayLabel] &&
            utRectsOverlap(spot[kUtLayPad].px, spot[kUtLayLabel].px)) {
            scr[kUtLayLabel] = 0;
            pin[kUtLayLabel] = 0;
            win.user[kUtLayLabel] = in.user[kUtLayLabel];
            scrWhy[kUtLayLabel] = kUtLayoutOverlap;
            scrAgainst[kUtLayLabel] = kUtLayPad;
            dropped = true;
        }
        if (!dropped) break;
        L = layout_detail::resolveWindow(win, pin);
    }
    for (int p = 0; p < kUtLayParts; ++p) {
        L.ignored[p] = (pin[p] && in.user[p]) ? 1 : 0;
        L.screenWhy[p] = scrWhy[p];
        L.screenAgainst[p] = scrAgainst[p];
        if (!scr[p]) continue;
        L.rect[p] = spot[p].local;
        L.user[p] = 1;
        L.why[p] = kUtLayoutOk;
        L.against[p] = -1;
        L.screen[p] = 1;
        L.screenPx[p] = spot[p].px;
        L.screenShifted[p] = spot[p].shifted;
    }
    L.pad = utPadFit(L.rect[kUtLayPad], in.buttons, in.padGap, in.scale, in.captionMax, in.widest,
                     in.widestCtx);
    L.label = utLabelFit(L.rect[kUtLayLabel], in.labelSize, in.scale);
    return L;
}

// ---- the pad on the game's own materials page -------------------------------------------------

enum UtMatPadMode {
    kUtMatPadNone = 0,   // no pad on this page (no pad in force; Ctrl+wheel still switches)
    kUtMatPadFull        // the whole pad at `rect`
};

// Where the materials page's pad comes from.
enum UtMatSrc {
    kUtMatSrcDerived = 0,   // the derived pad (no user rectangle in force)
    kUtMatSrcPad,           // the user's pad_<W>x<H>, in force
    kUtMatSrcPadmat,        // the user's padmat_<W>x<H>
    kUtMatSrcScreen         // the user's pad_screen, in force: the whole pad at its corner
};

// What became of padmat_.
enum UtMatWhy {
    kUtMatWhyUnset = 0,   // no key
    kUtMatWhyChosen,      // in force on the materials page
    kUtMatWhyOutside,     // not wholly inside the page: refused, the pad in force is drawn
    kUtMatWhyNoFit,       // no room for the buttons: refused, the pad in force is drawn
    kUtMatWhyScreen       // not used: the pad is pinned to the screen (pad_screen in force)
};

struct UtMatPad {
    int mode;        // UtMatPadMode
    UtRect rect;     // record px: the pad drawn on the materials page
    int user;        // 1 = `rect` is a user rectangle (pad_ or padmat_)
    int src;         // UtMatSrc
    int padmatWhy;   // UtMatWhy
    int under;       // live boxes that meet `rect`: the boxes the pad covers
};

// THE materials-page rule, the only one. The game's own page shows its real boxes; `boxes` are
// their live rectangles in SCALED window-local px (what the captured widgets report).
//   1. no pad in force (refused at this scale): no pad here either;
//   2. the user's padmat_ rectangle, when it is set, wholly inside the page's plate and fits the
//      buttons: the whole pad there ("on the materials page use this rectangle instead");
//   3. else the pad in force - the user's pad_ or the derived pad - unchanged.
// A SCREEN pad in force (pad_screen) is step 3 whatever padmat_ says: the whole pad stays at its
// screen corner (its rectangle is window-local and far off the plate; that is never refused).
// The pad is drawn WHERE IT IS, whatever lies under it: the boxes it meets are covered (counted
// in `under` for the log), and a point inside it is the pad's (ut_padmat.h utPadLeftDown; the
// hover is the pad's too), so every button works over the boxes. Nothing here depends on the
// boxes but that count.
inline UtMatPad utMatPadResolve(const UtLayoutInput& in, const UtLayout& lay,
                                const UtRect* padmatUser, const UtRectF* boxes, int nBoxes,
                                float s) {
    UtMatPad m = {};
    m.padmatWhy = kUtMatWhyUnset;
    if (!lay.pad.ok) return m;
    m.mode = kUtMatPadFull;
    m.rect = lay.rect[kUtLayPad];
    m.user = lay.user[kUtLayPad];
    m.src = m.user ? kUtMatSrcPad : kUtMatSrcDerived;
    if (lay.screen[kUtLayPad]) m.src = kUtMatSrcScreen;
    if (padmatUser) {
        if (lay.screen[kUtLayPad]) {
            m.padmatWhy = kUtMatWhyScreen;
        } else if (!utRectInside(*padmatUser, in.plate)) {
            m.padmatWhy = kUtMatWhyOutside;
        } else if (!utPadFit(*padmatUser, in.buttons, in.padGap, s, in.captionMax, in.widest,
                             in.widestCtx)
                        .ok) {
            m.padmatWhy = kUtMatWhyNoFit;
        } else {
            m.padmatWhy = kUtMatWhyChosen;
            m.rect = *padmatUser;
            m.user = 1;
            m.src = kUtMatSrcPadmat;
        }
    }
    const UtRectF f = utScaledRectF(m.rect, s);
    for (int i = 0; i < nBoxes; ++i)
        if (utRectsOverlap(f, boxes[i])) ++m.under;
    return m;
}

// The decision as the log says it: "the pad 52,1,780,153 (pad_974x659) covers 29 of the page's
// 116 live box(es)" / "... leaves the page's 24 live box(es) clear", plus what became of padmat_
// when it was set and refused. `cover` is the "<W>x<H>" key suffix. Returns the length written,
// cut (never overrun) at `cap`.
inline int utMatPadText(const UtMatPad& m, const char* cover, int nBoxes, const UtRect* padmat,
                        char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = 0;
    const char* cv = cover ? cover : "?";
    int n;
    if (m.mode != kUtMatPadFull) {
        n = snprintf(out, cap, "no pad in force");
    } else {
        char src[40];
        if (m.src == kUtMatSrcDerived) snprintf(src, sizeof(src), "the derived pad");
        else if (m.src == kUtMatSrcScreen) snprintf(src, sizeof(src), "pad_screen");
        else snprintf(src, sizeof(src), "%s_%s", m.src == kUtMatSrcPadmat ? "padmat" : "pad", cv);
        if (m.under > 0)
            n = snprintf(out, cap, "the pad %d,%d,%d,%d (%s) covers %d of the page's %d live "
                         "box(es)", m.rect.x, m.rect.y, m.rect.w, m.rect.h, src, m.under, nBoxes);
        else
            n = snprintf(out, cap, "the pad %d,%d,%d,%d (%s) leaves the page's %d live box(es) "
                         "clear", m.rect.x, m.rect.y, m.rect.w, m.rect.h, src, nBoxes);
    }
    if (n < 0) return 0;
    if ((size_t)n >= cap) return (int)cap - 1;
    if (padmat && (m.padmatWhy == kUtMatWhyOutside || m.padmatWhy == kUtMatWhyNoFit ||
                   m.padmatWhy == kUtMatWhyScreen)) {
        const int k = snprintf(out + n, cap - (size_t)n, "; padmat_%s=%d,%d,%d,%d is %s", cv,
                               padmat->x, padmat->y, padmat->w, padmat->h,
                               m.padmatWhy == kUtMatWhyOutside ? "refused: not inside the page"
                               : m.padmatWhy == kUtMatWhyNoFit
                                   ? "refused: no room for the buttons"
                                   : "not used: the pad is pinned to the screen");
        if (k > 0) n += (size_t)k < cap - (size_t)n ? k : (int)(cap - (size_t)n) - 1;
    }
    return n;
}

// ---- from the ini and the view to the layout in force -----------------------------------------

// The cover plate, window-local record px, out of the P line: the usable rectangle ends where the
// cover ends (ui_geometry: usableW = plateW - (x0 - bmpX)), so bmpX = x0 + usableW - plateW.
inline UtRect utPlateRectFromP(int x0, int y0, int usableW, int usableH, int plateW, int plateH) {
    const UtRect r = {x0 + usableW - plateW, y0 + usableH - plateH, plateW, plateH};
    return r;
}

// The default pad as 1.1.0 places it: utPadDefault, and REFUSED whole (false) when it leaves the
// joint band x 102..422, y 25..69 - then 1.1.0 draws no pad at all.
inline bool utPadDefaultInBand(int padY, int padH, int padGap, UtRect* out) {
    const UtRect r = utPadDefault(padY, padH, padGap);
    if (out) *out = r;
    return r.x + r.w - 1 <= kUtPadBandX1 && r.y >= kUtPadBandY0 && r.y + r.h - 1 <= kUtPadBandY1;
}

// What the live page reads: the P line, the largest cell, the [layout] values for its cover (null
// = no key) and the [advanced] pad keys.
struct UtLayoutSource {
    int x0, y0, usableW, usableH, plateW, plateH;   // the P line
    int maxCellW, maxCellH, rowGap;
    const char* grid;     // grid_<W>x<H>, or null
    const char* pad;      // pad_<W>x<H>
    const char* label;    // label_<W>x<H>
    int padY, padH, padGap;
    int labelSize;        // plate_label_size
    int captionMax;       // pad_caption_max
    int buttons;
    float scale;
    UtWidestFn widest;
    const void* widestCtx;
    // The screen keys (one for every view and cover) and this frame's view of the screen. A null
    // or empty key, or one that does not parse, is no key.
    const char* padScreen;    // pad_screen
    const char* labelScreen;  // label_screen
    int screenKnown;          // 1 = the four below are this frame's
    int screenW, screenH;     // the canvas, client px
    float originX, originY;   // the materials window's screen origin, client px
};

// Builds the UtLayoutInput (the user rectangles land in `userStore`, which must outlive `in`) and
// resolves it. A value that does not parse counts as no key (the ini reader has already refused it
// with a WARN). A derived pad 1.1.0 would refuse becomes an empty rectangle off the page, so no pad
// is fitted there and nothing can meet it - exactly 1.1.0's "no button is drawn".
inline UtLayout utLayoutFromSource(const UtLayoutSource& src, UtLayoutInput* in,
                                   UtRect userStore[kUtLayParts]) {
    UtLayoutInput& I = *in;
    I = UtLayoutInput{};
    I.plate = utPlateRectFromP(src.x0, src.y0, src.usableW, src.usableH, src.plateW, src.plateH);
    const UtRect grid = {src.x0, src.y0, src.usableW, src.usableH};
    I.def[kUtLayGrid] = grid;
    UtRect pad;
    if (!utPadDefaultInBand(src.padY, src.padH, src.padGap, &pad)) {
        const UtRect none = {-30000, -30000, 0, 0};
        pad = none;
    }
    I.def[kUtLayPad] = pad;
    I.def[kUtLayLabel] = utLabelDefault();
    const char* val[kUtLayParts] = {src.grid, src.pad, src.label};
    for (int p = 0; p < kUtLayParts; ++p)
        I.user[p] = (val[p] && utRectParse(val[p], &userStore[p])) ? &userStore[p] : nullptr;
    I.maxCellW = src.maxCellW;
    I.maxCellH = src.maxCellH;
    I.rowGap = src.rowGap;
    I.buttons = src.buttons;
    int gap = src.padGap;
    if (gap < 0) gap = 0;
    if (gap > 4) gap = 4;
    I.padGap = gap;
    I.scale = src.scale;
    I.captionMax = src.captionMax;
    I.widest = src.widest;
    I.widestCtx = src.widestCtx;
    I.labelSize = src.labelSize;
    const char* scr[kUtLayParts] = {nullptr, src.padScreen, src.labelScreen};
    for (int p = kUtLayPad; p < kUtLayParts; ++p)
        I.screenSet[p] = (scr[p] && scr[p][0] && utScreenRectParse(scr[p], &I.screenRect[p])) ? 1 : 0;
    I.screenKnown = src.screenKnown;
    I.screenW = src.screenW;
    I.screenH = src.screenH;
    I.originX = src.originX;
    I.originY = src.originY;
    // A pinned part's per-cover line (L.ignored) leaves the input once resolved: it stays parsed
    // in userStore for the DEBUG line, and nothing that reads in.user[] (the refusal WARN, the
    // outline) can take it for a refused rectangle. Resolving the input again gives the same.
    const UtLayout L = utLayoutResolve(I);
    for (int p = 0; p < kUtLayParts; ++p)
        if (L.ignored[p]) I.user[p] = nullptr;
    return L;
}

// The DEBUG line for a per-cover line that is not judged because the part is on the screen:
// "label_438x627=103,4,333,23 is ignored while the label is on the screen (label_screen=tl,...)".
// `value` is the line's text, `screen` the screen key's. Returns the length written, cut (never
// overrun) at `cap`.
inline int utLayoutIgnoredText(int part, const char* cover, const char* value, const char* screen,
                               char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    const char* nm = utLayoutPartName(part);
    const int n = snprintf(out, cap, "%s_%s=%s is ignored while the %s is on the screen "
                           "(%s_screen=%s)", nm, cover ? cover : "?", value ? value : "", nm, nm,
                           screen ? screen : "");
    if (n < 0) { out[0] = 0; return 0; }
    return (size_t)n >= cap ? (int)cap - 1 : n;
}

// One group's grid on the live page: the generated cols x rows (the G line) while the derived
// grid is in force, utGridFit over the user's rectangle under the live frame's box count when a
// user grid is.
inline UtGrid utLiveGroupGrid(const UtLayout& lay, int genCols, int genRows, int cellW, int cellH,
                              int rowGap, int cap) {
    if (!lay.user[kUtLayGrid]) {
        const UtGrid g = {genCols < 1 ? 1 : genCols, genRows < 1 ? 1 : genRows, 0};
        return g;
    }
    return utGridFit(lay.rect[kUtLayGrid], cellW, cellH, rowGap, cap < 1 ? 1 : cap);
}

// The first visible ITEM stays the first visible item across a cols change: row r of `oldCols`
// starts at item r * oldCols, which is row (r * oldCols) / newCols of `newCols`.
inline int utRowKeep(int row, int oldCols, int newCols) {
    if (row <= 0 || oldCols < 1 || newCols < 1) return 0;
    return (row * oldCols) / newCols;
}

// The grid rectangle in force for the GENERATOR (which plates to paint): the same resolution the
// live page runs, at UI scale 1 when `src.scale` is not a usable one (the generator measures
// none; only a user pad or label fit depends on it). `*user` = 1 when it is the user's grid_.
// The caller passes the screen keys (pad_screen / label_screen) exactly as the live page does,
// with screenKnown = 0: a pinned part waits to be placed and takes no part in the in-window rules,
// so a grid the live page accepts beside a pad on the screen is accepted here too. Without them
// the pad's in-window rectangle would refuse that grid and its plates would never be painted
// (e.g. grid_438x627=101,33,320,594 under pad_438x627=10,4,413,90 with the pad on the screen).
inline UtRect utLayoutGridRect(const UtLayoutSource& src, int* user) {
    UtLayoutSource s = src;
    if (!(s.scale >= 0.2f && s.scale <= 8.0f)) s.scale = 1.0f;
    if (s.buttons < 1) s.buttons = 26;
    if (s.captionMax < kUtCaptionMin) s.captionMax = kUtCaptionMaxDefault;
    UtLayoutInput in;
    UtRect store[kUtLayParts];
    const UtLayout L = utLayoutFromSource(s, &in, store);
    if (user) *user = L.user[kUtLayGrid];
    return L.rect[kUtLayGrid];
}

// The canonical text of a grid override as the layout plates' hash reads it: "x,y,w,h"
// re-formatted from the parsed rectangle, so blanks or a leading + never change the hash. Empty
// when `value` is null or does not parse. Returns the length.
inline int utLayoutCanonical(const char* value, char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = 0;
    UtRect r;
    if (!value || !utRectParse(value, &r)) return 0;
    return utRectFormat(r, out, cap);
}

// What the layout-plates thread decides (ut_generate.cpp layWantNow): the grid in force for the
// plates, resolved from `src` - the P line, the [layout] values for the cover, the pad keys AND the
// two screen keys, as the live page reads them - with no screen known (that thread never sees one:
// a screen key waits to be placed, so its part is pinned out of the in-window rules exactly as on
// the live page before its first draw). `*rect` = the grid in force; `canon` = the user grid_
// value's canonical text ("" for none). Returns 1 when the USER's grid is in force and has a
// canonical text: the thread then paints (or finds on disk) the set "grid_<cover>=<canon>".
inline int utLayoutPlatesGrid(const UtLayoutSource& src, UtRect* rect, char* canon, size_t cap) {
    UtLayoutSource s = src;
    s.screenKnown = 0;
    s.screenW = s.screenH = 0;
    s.originX = s.originY = 0.0f;
    int user = 0;
    const UtRect r = utLayoutGridRect(s, &user);
    if (rect) *rect = r;
    char tmp[64];
    char* out = (canon && cap) ? canon : tmp;
    const size_t n = (canon && cap) ? cap : sizeof(tmp);
    out[0] = 0;
    if (!user) return 0;
    return utLayoutCanonical(s.grid, out, n) > 0 ? 1 : 0;
}

// ---- the plate set under the grid in force ----------------------------------------------------
// Two resolvers decide whether the user's grid_<W>x<H> is in force. The layout-plates thread
// (ut_generate.cpp layWantNow -> utLayoutGridRect: no UI scale, no screen known) decides which set
// to paint and publishes it (layoutPlatesState: an _L<hash> suffix, and whether its files are on
// disk). The live page (ut_live.cpp layoutCompute -> utLayoutFromSource at the measured scale and
// on the drawn screen) fits the BOXES. Both read the same keys, but the live page can still
// refuse what the thread could not judge - a screen key over the grid or as large as the screen, a
// user pad or label at the measured scale - and the user grid with it. The plate follows the
// boxes: the layout plates only while the live grid is the user's, the derived plates only while
// it is the derived one; a user grid with no set of its own (not published yet, being painted,
// refused by the thread) shows the plain cover, never a lattice painted for another grid.
// ut_plate.cpp plateNames compares the two (layoutPlatesState against liveLayoutGet) and
// plateLayoutEdge renames the cached plates when either side changes.
enum UtPlateSet {
    kUtPlateSetDerived = 0,   // the view's derived plates (the P line's lattice)
    kUtPlateSetLayout,        // the user grid's layout plates, _L<hash>
    kUtPlateSetPlain          // no plate: the plain transfer cover
};
// `layoutSet`: the thread published a user grid's set for this view and cover; `layoutReady`: its
// files are on disk. `liveGrid`: -1 = the live layout is not resolved yet (no UI scale or no P
// line loaded: no box is fitted to any grid, the thread's answer stands, as before), 0 = the live
// grid is the derived one, 1 = it is the user's.
inline int utPlateSetChoice(int layoutSet, int layoutReady, int liveGrid) {
    if (liveGrid == 0) return kUtPlateSetDerived;   // never _L under the derived grid
    if (!layoutSet) return liveGrid > 0 ? kUtPlateSetPlain : kUtPlateSetDerived;
    return layoutReady ? kUtPlateSetLayout : kUtPlateSetPlain;
}

inline const char* utPlateSetName(int set) {
    switch (set) {
        case kUtPlateSetDerived: return "the derived plates";
        case kUtPlateSetLayout: return "the layout plates";
        case kUtPlateSetPlain: return "the plain cover";
        default: return "?";
    }
}

}  // namespace ut
