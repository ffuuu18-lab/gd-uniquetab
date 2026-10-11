// ut_layoutedit.h - the in-game layout editor, its pure half: where the handles and the toolbar
// are, what a drag does to a rectangle (kept inside the page, the grid snapped to whole cells),
// whether the edited layout may be saved, and the state machine that opens, drags, saves and
// cancels. No keyboard: everything is a mouse event, the polled cursor or the clock.
//
// Pure arithmetic over ut_layout.h: no Windows, no engine, no allocation, no globals. ut_panel.cpp
// feeds it the engine's mouse events, the polled cursor and button state and the clock, and draws
// what it says; tools\test_layoutedit.cpp proves every transition and clamp.
//
// Units: the rectangles are RECORD px, window-local (ut_layout.h); the points are SCREEN px,
// window-local (the engine's event point minus the Draw origin), so a handle is at least
// kUtEdHandleMinPx on screen at every UI scale.
//
//   utEditOpen          the layout in force -> an open editor (or false: no usable scale/group)
//   utEditHitTest       a point -> a toolbar button, a handle (its edges) or a rectangle's body
//   utEditLeftDown      LEFT DOWN: start a drag or press a button; kUtEdActPass = not consumed
//   utEditToolbarAt     the toolbar button under a point in ANY state but closed (CANCEL's)
//   utEditDragTo        the polled cursor while a drag runs: the rectangle follows, clamped
//   utEditLeftUp        LEFT UP: the drag ends where the event says
//   utEditTick          once a frame: the cancel rules, the polled release, the save handover
//   utEditCheck         the edited rectangles through utLayoutResolve: SAVE only when all hold
//   utEditKeyValue      the text SAVE writes for one part (nothing = the key is removed)
//   utEditParks         whether the page's boxes stay parked (open and drawn recently)
//   utEditOneShot       the layout_edit=1 key as a rising edge
//
// THE SCREEN MODE (pad_screen / label_screen, ut_layout.h "the screen-anchored pad and label").
// The pad and the label each have a WINDOW mode (kept inside the plate, saved as the per-cover
// pad_/label_<W>x<H> line of the active view's file) and a SCREEN mode (kept inside the screen,
// saved as pad_screen / label_screen = the nearest corner, in that same file). The grid
// is always in window mode. Once the screen is known (utEditSetScreen: the canvas size and the
// window's origin), a MOVE that carries the pad or the label across the plate's edge puts it in
// screen mode - it keeps following the cursor, clamped to the screen - and a move or a resize that
// brings it wholly back inside the plate puts it in window mode again; a resize keeps the mode
// otherwise (clamped to the plate or to the screen). The rectangles stay window-local record px
// in both modes (a screen part's lies off the plate, negative too), so the hit test, the drag and
// the draw are the same code. The toolbar always sits at the top right of the PAD's rectangle and
// travels with it (kept inside the pad's bounds). RESET puts every part back in the window at its
// derived rectangle; SAVE then removes the screen keys. utEditSavePlanOf says which key each part
// writes and to which file; utEditScreenGrounds says where a screen part takes the clicks
// outside the window (published for the screen input, ut_padinput.h).
//
// TWO ROUTES, ONE BORDER. Every click INSIDE the caravan window reaches the editor
// through the window's own mouse slot (ut_plate editMouse), every click OUTSIDE it through the
// screen input (the GetMouseEvent detour): utEditWindowClient is the border the editor publishes
// with its grounds, and the screen input leaves everything inside it untouched. The grounds are
// published only for the parts in SCREEN mode (and for a toolbar outside the plate). The window
// route cannot wait for the in-window pad's own probe to prove the event space this world - that
// probe never runs while the pad is pinned to the screen, so in such a world every in-window
// click of the editor but CANCEL would be refused (a part dragged wholly back into the window
// could no longer be moved, resized or saved). So the screen input NOTES every in-window LEFT
// DOWN it passes (its own copy of the click, in the client space ITS probe proved) and the
// window route accepts a click whose point equals that note within kUtEdProofPx
// (utEditNoteProves) - the same event seen by both routes, so the two spaces agree; one proof
// holds for the rest of the edit.
//
//   utEditSetScreen     the screen size and the window's origin -> the screen bounds
//   utEditScreenKey     a screen part's rectangle -> its pad_screen / label_screen value
//   utEditSavePlanOf    the keys SAVE writes, per mode and file
//   utEditRefusal       the words for a refused part ("the pad would cover the item grid")
#pragma once

#include <math.h>
#include <stddef.h>

#include "ut_layout.h"

namespace ut {

// The edges a drag moves; all four = the rectangle moves.
enum { kUtEdgeL = 1, kUtEdgeR = 2, kUtEdgeT = 4, kUtEdgeB = 8, kUtEdgeMove = 15 };

// The toolbar's buttons, left to right.
enum { kUtEdSave = 0, kUtEdCancel = 1, kUtEdReset = 2, kUtEdButtons = 3 };

enum UtEdState { kUtEdClosed = 0, kUtEdOpen, kUtEdDrag, kUtEdSaving };

// Why the editor closed (UtEdit::why).
enum UtEdWhy {
    kUtEdWhyNone = 0,
    kUtEdWhyCancel,     // CANCEL
    kUtEdWhyGone,       // the page was not drawn: caravan closed, another page, world teardown
    kUtEdWhyGroup,      // the game's own materials page came up
    kUtEdWhyFocus,      // the game lost the focus
    kUtEdWhyScale,      // the UI scale changed
    kUtEdWhyCover,      // another caravan cover (a view change)
    kUtEdWhyLayout,     // no layout in force any more
    kUtEdWhySaved,      // SAVE, and the layout in force now is the edited one
    kUtEdWhySaveSlow,   // SAVE, and the layout in force did not follow in time
    kUtEdWhyScreen      // the screen size changed (a resolution change)
};

inline const char* utEditWhyText(int why) {
    switch (why) {
        case kUtEdWhyCancel: return "CANCEL - nothing was written";
        case kUtEdWhyGone: return "the page left the screen (caravan closed, another caravan "
                                  "page or a world change) - nothing was written";
        case kUtEdWhyGroup: return "the game's own materials page came up - nothing was written";
        case kUtEdWhyFocus: return "the game lost the focus - nothing was written";
        case kUtEdWhyScale: return "the UI scale changed - nothing was written";
        case kUtEdWhyCover: return "the caravan cover changed - nothing was written";
        case kUtEdWhyLayout: return "the layout could not be resolved - nothing was written";
        case kUtEdWhySaved: return "saved; the page follows the new layout";
        case kUtEdWhySaveSlow: return "saved; the settings file has not been read back yet";
        case kUtEdWhyScreen: return "the screen size changed - nothing was written";
        default: return "?";
    }
}

// What a LEFT DOWN did.
enum UtEdAct {
    kUtEdActPass = 0,     // not the editor's: the engine gets the event
    kUtEdActDrag,         // a handle or a rectangle: the drag started (consumed)
    kUtEdActSave,         // SAVE pressed and allowed: the caller writes, then utEditSaved
    kUtEdActSaveRefused,  // SAVE pressed while a rectangle is refused (consumed, nothing done)
    kUtEdActCancel,       // CANCEL: closed (consumed)
    kUtEdActReset         // RESET: the derived rectangles are back, not saved yet (consumed)
};

enum { kUtEdHitNone = 0, kUtEdHitButton, kUtEdHitHandle, kUtEdHitBody };

struct UtEdHit {
    int kind;     // kUtEdHit*
    int part;     // kUtLay* for a handle or a body, else -1
    int edges;    // kUtEdge* the drag moves
    int button;   // kUtEd* for a toolbar button, else -1
};

const int kUtEdHandleMinPx = 8;          // screen px: the smallest handle and edge strip
const unsigned kUtEdGoneMs = 400;        // not drawn for this long = the page left the screen
const unsigned kUtEdSaveWaitMs = 3000;   // SAVE waits this long for the settings read-back
const int kUtEdButtonW = 52;             // record px, the toolbar's buttons
const int kUtEdButtonH = 14;
const int kUtEdButtonGap = 3;
const int kUtEdMinPadW = 2 * kUtPadMargin + 2 * kUtPadMinCellW + 1;
const int kUtEdMinLabelW = 24;

struct UtEdit {
    int state;                    // UtEdState
    int why;                      // UtEdWhy: why it last closed
    UtRect rect[kUtLayParts];     // the rectangles being edited
    UtRect def[kUtLayParts];      // the derived ones (RESET; a part equal to it is not stored)
    UtRect orig[kUtLayParts];     // the ones in force when the editor opened
    UtRect plate;                 // the cover plate: the grid and a window-mode part stay inside
    UtRect tool[kUtEdButtons];    // the toolbar, record px
    int minW[kUtLayParts], minH[kUtLayParts];
    int snapW, snapH;             // the grid's step: the shown group's cell, its height + row gap
    float scale;                  // the UI scale at open; another one cancels
    int coverW, coverH;           // the cover at open; another one cancels
    int group;                    // the collection group shown
    unsigned lastTick;            // the clock of the last utEditTick (or the open)
    unsigned savedAt;
    // the drag
    int part, edges;
    float x0, y0;                 // where it started, screen px window-local
    UtRect start;                 // the rectangle when it started
    int button;                   // the PHYSICAL button it polls: 1 left, 2 right, 0 none held
    // utEditCheck
    int ok;                       // 1 = every rectangle holds: SAVE is allowed
    int partWhy[kUtLayParts];     // UtLayoutWhy per part (a screen part: its screen key's)
    int against[kUtLayParts];
    // the screen mode
    int scr[kUtLayParts];         // 1 = the part is in screen mode (never the grid)
    int scrOrig[kUtLayParts];     // the modes at the open
    int screenKnown;              // 1 = screenW/H and originX/Y are known
    int screenW, screenH;         // the canvas, client px
    float originX, originY;       // the materials window's origin on it, client px
    UtRect screen;                // the screen as window-local record px (0 x 0 = unknown)
};

// The side of a handle in screen px: 6 record px, never under kUtEdHandleMinPx.
inline int utEditHandlePx(float s) {
    const int v = (int)(6.0f * s + 0.5f);
    return v < kUtEdHandleMinPx ? kUtEdHandleMinPx : v;
}

// The toolbar: three buttons right-aligned at the top of `band` (the derived pad's rectangle).
inline void utEditToolbar(const UtRect& band, UtRect out[kUtEdButtons]) {
    const int w = kUtEdButtons * kUtEdButtonW + (kUtEdButtons - 1) * kUtEdButtonGap;
    int x = band.x + band.w - kUtPadMargin - w;
    if (x < band.x) x = band.x;
    for (int b = 0; b < kUtEdButtons; ++b) {
        out[b].x = x + b * (kUtEdButtonW + kUtEdButtonGap);
        out[b].y = band.y;
        out[b].w = kUtEdButtonW;
        out[b].h = kUtEdButtonH;
    }
}

// The smallest rectangle each part may be dragged to, record px: the grid holds the largest
// cell (utLayoutResolve's rule), the pad two of the narrowest cells, the label a size-9 line.
inline void utEditMinSizes(int maxCellW, int maxCellH, int rowGap, float s,
                           int minW[kUtLayParts], int minH[kUtLayParts]) {
    minW[kUtLayGrid] = maxCellW < 1 ? 1 : maxCellW;
    minH[kUtLayGrid] = (maxCellH < 1 ? 1 : maxCellH) + (rowGap < 0 ? 0 : rowGap);
    minW[kUtLayPad] = kUtEdMinPadW;
    minH[kUtLayPad] = kUtPadMinCellH;
    const float sc = (s >= 0.2f && s <= 8.0f) ? s : 1.0f;
    int lw = (int)ceilf(8.0f / sc);
    if (lw < kUtEdMinLabelW) lw = kUtEdMinLabelW;
    minW[kUtLayLabel] = lw;
    minH[kUtLayLabel] = 1 + (int)ceilf((float)kUtLabelSizeMin / sc);
}

// A length after a drag: whole `unit`s when unit > 1 (rounded to the nearest), never under
// `minLen` (rounded up to whole units), never over `room` (rounded down). When the room cannot
// hold the minimum the room wins, and utEditCheck then refuses the rectangle.
inline int utEditSnap(int len, int unit, int minLen, int room) {
    if (room < 1) room = 1;
    if (unit <= 1) {
        if (len < minLen) len = minLen;
        if (len > room) len = room;
        return len < 1 ? 1 : len;
    }
    int v = len <= 0 ? 0 : ((len + unit / 2) / unit) * unit;
    const int lo = ((minLen + unit - 1) / unit) * unit;
    const int hi = (room / unit) * unit;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    if (v < 1) v = room < unit ? room : unit;
    return v;
}

// `start` dragged by dx, dy record px on `edges`. A move keeps the size and stays inside the
// plate; a resize moves only the dragged edges, keeps the opposite ones where they were, and
// snaps the length to whole snapW x snapH steps (1 = every pixel).
inline UtRect utEditDragRect(const UtRect& start, int edges, int dx, int dy, const UtRect& plate,
                             int minW, int minH, int snapW, int snapH) {
    UtRect r = start;
    const int px1 = plate.x + plate.w, py1 = plate.y + plate.h;
    if (edges == kUtEdgeMove) {
        r.x = start.x + dx;
        r.y = start.y + dy;
        if (r.x > px1 - r.w) r.x = px1 - r.w;
        if (r.x < plate.x) r.x = plate.x;
        if (r.y > py1 - r.h) r.y = py1 - r.h;
        if (r.y < plate.y) r.y = plate.y;
        return r;
    }
    if (edges & kUtEdgeL) {
        const int right = start.x + start.w;
        const int len = utEditSnap(start.w - dx, snapW, minW, right - plate.x);
        r.x = right - len;
        r.w = len;
    } else if (edges & kUtEdgeR) {
        const int left = start.x < plate.x ? plate.x : start.x;
        r.x = left;
        r.w = utEditSnap(start.x + start.w + dx - left, snapW, minW, px1 - left);
    }
    if (edges & kUtEdgeT) {
        const int bottom = start.y + start.h;
        const int len = utEditSnap(start.h - dy, snapH, minH, bottom - plate.y);
        r.y = bottom - len;
        r.h = len;
    } else if (edges & kUtEdgeB) {
        const int top = start.y < plate.y ? plate.y : start.y;
        r.y = top;
        r.h = utEditSnap(start.y + start.h + dy - top, snapH, minH, py1 - top);
    }
    return r;
}

// The physical button a drag polls (GetAsyncKeyState reads PHYSICAL buttons): the logical left
// one - the right one when the buttons are swapped - if it is down at the start, else the other
// one if that is down (an engine reading the physical left), else none (the click is over).
inline int utEditDragButton(int swapped, int physLeftDown, int physRightDown) {
    const int want = swapped ? 2 : 1;
    const int other = swapped ? 1 : 2;
    const int wantDown = want == 1 ? physLeftDown : physRightDown;
    const int otherDown = other == 1 ? physLeftDown : physRightDown;
    return wantDown ? want : otherDown ? other : 0;
}

// The one-shot key: true once per 0 -> 1 edge. The caller sets *latch = 1 when it consumed the
// key; the latch clears only when the key reads 0 again, so the second or so before the
// settings file is read back can never open the editor twice.
inline bool utEditOneShot(int value, int* latch) {
    if (!value) {
        *latch = 0;
        return false;
    }
    return *latch == 0;
}

struct UtEditOpenIn {
    UtRect inForce[kUtLayParts];  // the rectangles in force (a pad with no size: none drawn)
    UtRect def[kUtLayParts];      // the derived ones
    UtRect padFallback;           // the pad to start from when neither has a size
    UtRect plate;
    int maxCellW, maxCellH, rowGap;
    int cellW, cellH;             // the shown group's cell
    float scale;
    int coverW, coverH;
    int group;                    // the collection group shown; < 0 = the editor does not open
    unsigned now;
    // the screen (zero = not known: the pad and the label cannot leave the plate)
    int screenIn[kUtLayParts];    // 1 = the part in force is its screen key (UtLayout::screen)
    int screenKnown;
    int screenW, screenH;         // client px
    float originX, originY;       // the window's origin, client px
};

inline bool utEditUsable(const UtRect& r) { return r.w > 0 && r.h > 0; }

// The shown group's cell, as the grid's snapping step.
inline void utEditSetCell(UtEdit* e, int cellW, int cellH, int rowGap) {
    e->snapW = cellW > 0 ? cellW : 1;
    e->snapH = (cellH > 0 ? cellH : 1) + (rowGap > 0 ? rowGap : 0);
}

// ---- the screen mode ------------------------------------------------------------------------

// The screenW x screenH canvas as WINDOW-LOCAL record px at scale s (origin = the window's place
// on it): the whole record px whose drawn edges stay on the canvas. 0 x 0 when unknown.
inline UtRect utEditScreenBounds(int screenW, int screenH, float originX, float originY, float s) {
    UtRect r = {0, 0, 0, 0};
    if (screenW < 1 || screenH < 1 || !(s >= 0.2f && s <= 8.0f)) return r;
    const int x0 = (int)ceilf(-originX / s), y0 = (int)ceilf(-originY / s);
    const int x1 = (int)floorf(((float)screenW - originX) / s);
    const int y1 = (int)floorf(((float)screenH - originY) / s);
    if (x1 <= x0 || y1 <= y0) return r;
    r.x = x0;
    r.y = y0;
    r.w = x1 - x0;
    r.h = y1 - y0;
    return r;
}

// True when `part` may leave the plate: the pad or the label, with the screen known.
inline bool utEditCanLeave(const UtEdit& e, int part) {
    return (part == kUtLayPad || part == kUtLayLabel) && e.screenKnown && utEditUsable(e.screen);
}

// What a part is kept inside: the screen in screen mode (no bound while the screen is unknown),
// the plate in window mode (the grid always).
inline UtRect utEditBounds(const UtEdit& e, int part) {
    if (part != kUtLayGrid && part >= 0 && part < kUtLayParts && e.scr[part]) {
        if (utEditUsable(e.screen)) return e.screen;
        const UtRect any = {-kUtScreenMax, -kUtScreenMax, 2 * kUtScreenMax, 2 * kUtScreenMax};
        return any;
    }
    return e.plate;
}

// The toolbar at the top right of the PAD's rectangle (utEditToolbar), shifted (never resized)
// back inside the pad's bounds when a narrow pad near an edge would push it out.
inline void utEditPlaceToolbar(UtEdit* e) {
    utEditToolbar(e->rect[kUtLayPad], e->tool);
    const UtRect b = utEditBounds(*e, kUtLayPad);
    const int x0 = e->tool[0].x, x1 = e->tool[kUtEdButtons - 1].x + kUtEdButtonW;
    int dx = 0;
    if (x1 > b.x + b.w) dx = b.x + b.w - x1;
    if (x0 + dx < b.x) dx = b.x - x0;
    int dy = 0;
    if (e->tool[0].y + kUtEdButtonH > b.y + b.h) dy = b.y + b.h - kUtEdButtonH - e->tool[0].y;
    if (e->tool[0].y + dy < b.y) dy = b.y - e->tool[0].y;
    for (int k = 0; k < kUtEdButtons; ++k) {
        e->tool[k].x += dx;
        e->tool[k].y += dy;
    }
}

// The screen size and the window's origin (once a frame; the open's too). Unknown (`known` 0 or
// an unusable size) = the pad and the label cannot leave the plate; a part already on the screen
// stays there. A screen part's bounds follow the origin, and so does the toolbar.
inline void utEditSetScreen(UtEdit* e, int known, int screenW, int screenH, float originX,
                            float originY) {
    const UtRect b = known ? utEditScreenBounds(screenW, screenH, originX, originY, e->scale)
                           : UtRect{0, 0, 0, 0};
    e->screenKnown = utEditUsable(b) ? 1 : 0;
    e->screenW = e->screenKnown ? screenW : 0;
    e->screenH = e->screenKnown ? screenH : 0;
    e->originX = e->screenKnown ? originX : 0.0f;
    e->originY = e->screenKnown ? originY : 0.0f;
    e->screen = b;
    utEditPlaceToolbar(e);
}

inline bool utEditAnyScreen(const UtEdit& e) {
    return e.scr[kUtLayPad] != 0 || e.scr[kUtLayLabel] != 0;
}

// A screen part's key: the NEAREST screen corner of where it is drawn (utScreenFromClient), its
// size the rectangle's own record px (never re-derived from the drawn px, which a scale under 1
// would round away).
inline UtScreenRect utEditScreenKey(const UtEdit& e, int part) {
    const UtRect& r = e.rect[part];
    const UtRectF px = utLocalToClient(r, e.originX, e.originY, e.scale);
    UtScreenRect k = utScreenFromClient(px, e.screenW, e.screenH, e.scale);
    k.w = r.w < 1 ? 1 : r.w > kUtScreenMax ? kUtScreenMax : r.w;
    k.h = r.h < 1 ? 1 : r.h > kUtScreenMax ? kUtScreenMax : r.h;
    return k;
}

// The rectangle the live page will have for `part` once SAVE is read back: the edited one in
// window mode; in screen mode the one its key resolves to (utScreenLocate - a record px off at
// most, from the rounding of the corner offsets).
inline UtRect utEditResolvedRect(const UtEdit& e, int part) {
    if (part < 0 || part >= kUtLayParts || !e.scr[part]) return e.rect[part];
    const UtScreenSpot spot = utScreenLocate(utEditScreenKey(e, part), e.screenKnown, e.screenW,
                                             e.screenH, e.originX, e.originY, e.scale);
    return spot.why == kUtLayoutOk ? spot.local : e.rect[part];
}

// Where a part in SCREEN mode takes the clicks outside the window, window-local screen px: the
// pad's rectangle joined with the toolbar, the label's rectangle, each grown by half a handle
// (the handles straddle the edges). A part in window mode: 0 x 0 (the window's own mouse slot
// hands its clicks over) - a window-mode pad's toolbar only where it leaves the plate. The
// caller adds the window's origin and publishes them with utEditWindowClient: the parts of these
// grounds inside the window stay the window's.
inline void utEditScreenGrounds(const UtEdit& e, UtRectF* pad, UtRectF* label) {
    const UtRectF none = {0.0f, 0.0f, 0.0f, 0.0f};
    *pad = none;
    *label = none;
    if (e.state == kUtEdClosed) return;
    const float s = e.scale;
    const float half = (float)utEditHandlePx(s) * 0.5f;
    if (e.scr[kUtLayPad]) {
        UtRect u = e.rect[kUtLayPad];
        for (int k = 0; k < kUtEdButtons; ++k) {
            const UtRect& t = e.tool[k];
            const int x1 = (u.x + u.w > t.x + t.w) ? u.x + u.w : t.x + t.w;
            const int y1 = (u.y + u.h > t.y + t.h) ? u.y + u.h : t.y + t.h;
            if (t.x < u.x) u.x = t.x;
            if (t.y < u.y) u.y = t.y;
            u.w = x1 - u.x;
            u.h = y1 - u.y;
        }
        const UtRectF f = utScaledRectF(u, s);
        *pad = UtRectF{f.x - half, f.y - half, f.w + 2.0f * half, f.h + 2.0f * half};
    } else {
        // A window-mode pad: its toolbar is the screen input's only where it leaves the plate
        // (utEditPlaceToolbar keeps it inside, so normally never).
        bool out = false;
        UtRect u = e.tool[0];
        for (int k = 0; k < kUtEdButtons; ++k) {
            const UtRect& t = e.tool[k];
            if (!utRectInside(t, e.plate)) out = true;
            const int x1 = (u.x + u.w > t.x + t.w) ? u.x + u.w : t.x + t.w;
            const int y1 = (u.y + u.h > t.y + t.h) ? u.y + u.h : t.y + t.h;
            if (t.x < u.x) u.x = t.x;
            if (t.y < u.y) u.y = t.y;
            u.w = x1 - u.x;
            u.h = y1 - u.y;
        }
        if (out) {
            const UtRectF f = utScaledRectF(u, s);
            *pad = UtRectF{f.x - half, f.y - half, f.w + 2.0f * half, f.h + 2.0f * half};
        }
    }
    if (e.scr[kUtLayLabel]) {
        const UtRectF f = utScaledRectF(e.rect[kUtLayLabel], s);
        *label = UtRectF{f.x - half, f.y - half, f.w + 2.0f * half, f.h + 2.0f * half};
    }
}

// The caravan window's client rect as the editor sees it - the plate, window-local screen px (the
// caller adds the window's origin). The screen input leaves every point inside it to the window's
// own mouse slot while the editor is open.
inline UtRectF utEditWindowClient(const UtEdit& e) {
    if (e.state == kUtEdClosed || !utEditUsable(e.plate)) return UtRectF{0.0f, 0.0f, 0.0f, 0.0f};
    return utScaledRectF(e.plate, e.scale);
}

// ---- the window route's proof (see "TWO ROUTES, ONE BORDER" above) ----------------------------
const float kUtEdProofPx = 2.0f;     // the in-window probe's own tolerance, both axes
const unsigned kUtEdNoteMs = 250;    // the note and the window's click are one event: same pump pass

// The screen input's copy of the last LEFT DOWN it passed inside the window, window-local px.
struct UtEdPumpNote {
    int valid;
    float x, y;
    unsigned at;
};

inline void utEditNoteDown(UtEdPumpNote* n, float x, float y, unsigned now) {
    if (!n) return;
    n->valid = 1;
    n->x = x;
    n->y = y;
    n->at = now;
}

// True when the window route's LEFT DOWN at (x, y) is the noted one: the note fresh and the point
// the same within kUtEdProofPx on both axes. A match uses the note up; *dx / *dy (optional) say the
// difference for the log. NaN never matches.
inline bool utEditNoteProves(UtEdPumpNote* n, float x, float y, unsigned now, float* dx = nullptr,
                             float* dy = nullptr) {
    if (!n || !n->valid) return false;
    const float ddx = x - n->x, ddy = y - n->y;
    if (dx) *dx = ddx;
    if (dy) *dy = ddy;
    if (now - n->at > kUtEdNoteMs) return false;
    if (!(ddx >= -kUtEdProofPx && ddx <= kUtEdProofPx && ddy >= -kUtEdProofPx && ddy <= kUtEdProofPx))
        return false;
    n->valid = 0;
    return true;
}

// The status words while a part is on the screen; null when none is.
inline const char* utEditScreenHint(const UtEdit& e) {
    if (e.scr[kUtLayPad] && e.scr[kUtLayLabel])
        return "on the screen - SAVE writes pad_screen and label_screen";
    if (e.scr[kUtLayPad]) return "on the screen - SAVE writes pad_screen";
    if (e.scr[kUtLayLabel]) return "on the screen - SAVE writes label_screen";
    return nullptr;
}

// The words for a refused part: "the pad would cover the item grid", "the label overlaps another
// part of the page: the pad". Empty when the part holds. Returns the length written.
inline int utEditRefusal(const UtEdit& e, int part, char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = 0;
    if (part < 0 || part >= kUtLayParts || e.partWhy[part] == kUtLayoutOk) return 0;
    const int why = e.partWhy[part];
    const bool named = e.against[part] >= 0 && why != kUtLayoutScreenOverGrid;
    const int n = snprintf(out, cap, "the %s %s%s%s", utLayoutPartName(part), utLayoutWhyText(why),
                           named ? ": the " : "", named ? utLayoutPartName(e.against[part]) : "");
    return (n > 0 && (size_t)n < cap) ? n : (int)(cap - 1);
}

// What SAVE writes, per part and file. The grid: grid_<cover> (its value, or removed when it is
// the derived rectangle). A part in WINDOW mode: <part>_<cover> likewise AND <part>_screen
// removed (back in the window). A part in SCREEN mode: <part>_screen = its key; its
// <part>_<cover> line is not touched (the screen key wins over it while set; RESET + SAVE brings
// the window back). Every key goes to the active view's file (a custom game's own one while it
// is active), the screen keys too since ini_version 42. `mainOnly` = a screen key (pad_screen /
// label_screen): its removal pins the part to the window (the name is the old model's).
struct UtEditSaveKey {
    char key[40];
    char value[48];
    int set;        // 1 = `value` is written; 0 = the line is removed
    int mainOnly;   // 1 = a screen key: removed = the part back in the window
};
const int kUtEdSaveKeysMax = 5;
struct UtEditSavePlan {
    int n;
    UtEditSaveKey k[kUtEdSaveKeysMax];
};

inline void utEditSavePlanOf(const UtEdit& e, const char* cover, UtEditSavePlan* out) {
    out->n = 0;
    for (int p = 0; p < kUtLayParts; ++p) {
        if (p != kUtLayGrid && e.scr[p]) {
            UtEditSaveKey& s = out->k[out->n++];
            snprintf(s.key, sizeof(s.key), "%s_screen", utLayoutPartName(p));
            s.set = utScreenRectFormat(utEditScreenKey(e, p), s.value, sizeof(s.value)) > 0 ? 1 : 0;
            s.mainOnly = 1;
            continue;
        }
        UtEditSaveKey& w = out->k[out->n++];
        snprintf(w.key, sizeof(w.key), "%s_%s", utLayoutPartName(p), cover ? cover : "");
        w.value[0] = 0;
        w.set = 0;
        if (!utRectEq(e.rect[p], e.def[p]))
            w.set = utRectFormat(e.rect[p], w.value, sizeof(w.value)) > 0 ? 1 : 0;
        w.mainOnly = 0;
        if (p == kUtLayGrid) continue;
        UtEditSaveKey& s = out->k[out->n++];
        snprintf(s.key, sizeof(s.key), "%s_screen", utLayoutPartName(p));
        s.value[0] = 0;
        s.set = 0;
        s.mainOnly = 1;
    }
}

inline bool utEditOpen(UtEdit* e, const UtEditOpenIn& in) {
    if (e->state != kUtEdClosed) return false;
    if (!(in.scale >= 0.2f && in.scale <= 8.0f) || in.group < 0) return false;
    UtEdit n = {};
    n.state = kUtEdOpen;
    n.why = kUtEdWhyNone;
    n.plate = in.plate;
    for (int p = 0; p < kUtLayParts; ++p) {
        n.def[p] = utEditUsable(in.def[p]) ? in.def[p] : in.padFallback;
        n.rect[p] = utEditUsable(in.inForce[p]) ? in.inForce[p] : n.def[p];
        n.orig[p] = n.rect[p];
        n.partWhy[p] = kUtLayoutOk;
        n.against[p] = -1;
        n.scr[p] = (p != kUtLayGrid && in.screenIn[p] && utEditUsable(in.inForce[p])) ? 1 : 0;
        n.scrOrig[p] = n.scr[p];
    }
    utEditMinSizes(in.maxCellW, in.maxCellH, in.rowGap, in.scale, n.minW, n.minH);
    utEditSetCell(&n, in.cellW, in.cellH, in.rowGap);
    n.scale = in.scale;
    utEditSetScreen(&n, in.screenKnown, in.screenW, in.screenH, in.originX, in.originY);   // + toolbar
    n.coverW = in.coverW;
    n.coverH = in.coverH;
    n.group = in.group;
    n.lastTick = in.now;
    n.part = -1;
    n.ok = 1;
    *e = n;
    return true;
}

// The edited rectangles through the resolver the live page runs, every one as a user rectangle:
// SAVE is allowed only when all three would be in force as they are. A part in screen mode goes
// in as its screen key (pad_screen / label_screen, placed against the editor's screen and
// origin) and is judged by its screen verdict (L.screenWhy: over the grid, as large as the
// screen, no fit, the label over the screen pad); a part in window mode as its <W>x<H> rectangle
// with no screen key (the key in force must not hide the edited rectangle).
inline bool utEditCheck(UtEdit* e, const UtLayoutInput& base) {
    UtLayoutInput in = base;
    in.screenKnown = e->screenKnown;
    in.screenW = e->screenW;
    in.screenH = e->screenH;
    in.originX = e->originX;
    in.originY = e->originY;
    for (int p = 0; p < kUtLayParts; ++p) {
        in.screenSet[p] = 0;
        in.user[p] = &e->rect[p];
        if (p != kUtLayGrid && e->scr[p]) {
            in.screenSet[p] = 1;
            in.screenRect[p] = utEditScreenKey(*e, p);
            in.user[p] = nullptr;
        }
    }
    const UtLayout L = utLayoutResolve(in);
    e->ok = 1;
    for (int p = 0; p < kUtLayParts; ++p) {
        const bool scr = p != kUtLayGrid && e->scr[p];
        e->partWhy[p] = scr ? L.screenWhy[p] : L.why[p];
        e->against[p] = scr ? L.screenAgainst[p] : L.against[p];
        if (e->partWhy[p] != kUtLayoutOk) e->ok = 0;
    }
    return e->ok != 0;
}

inline bool utEditInside(float x, float y, const UtRectF& r) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

// Toolbar first; then the handles of the label, the pad and the grid (corners, then the edge
// strips); then their bodies in the same order - the small parts win over the grid.
inline UtEdHit utEditHitTest(const UtEdit& e, float x, float y) {
    UtEdHit h = {kUtEdHitNone, -1, 0, -1};
    if (e.state != kUtEdOpen) return h;
    const float s = e.scale;
    for (int b = 0; b < kUtEdButtons; ++b) {
        if (utEditInside(x, y, utScaledRectF(e.tool[b], s))) {
            h.kind = kUtEdHitButton;
            h.button = b;
            return h;
        }
    }
    static const int kOrder[kUtLayParts] = {kUtLayLabel, kUtLayPad, kUtLayGrid};
    const float half = (float)utEditHandlePx(s) * 0.5f;
    for (int k = 0; k < kUtLayParts; ++k) {
        const int p = kOrder[k];
        const UtRectF r = utScaledRectF(e.rect[p], s);
        const float l = r.x, t = r.y, rr = r.x + r.w, b = r.y + r.h;
        const float xs[2] = {l, rr}, ys[2] = {t, b};
        for (int cy = 0; cy < 2; ++cy) {
            for (int cx = 0; cx < 2; ++cx) {
                if (fabsf(x - xs[cx]) <= half && fabsf(y - ys[cy]) <= half) {
                    h.kind = kUtEdHitHandle;
                    h.part = p;
                    h.edges = (cx ? kUtEdgeR : kUtEdgeL) | (cy ? kUtEdgeB : kUtEdgeT);
                    return h;
                }
            }
        }
        int edges = 0;
        if (y >= t && y <= b) {
            if (fabsf(x - l) <= half) edges = kUtEdgeL;
            else if (fabsf(x - rr) <= half) edges = kUtEdgeR;
        }
        if (!edges && x >= l && x <= rr) {
            if (fabsf(y - t) <= half) edges = kUtEdgeT;
            else if (fabsf(y - b) <= half) edges = kUtEdgeB;
        }
        if (edges) {
            h.kind = kUtEdHitHandle;
            h.part = p;
            h.edges = edges;
            return h;
        }
    }
    for (int k = 0; k < kUtLayParts; ++k) {
        const int p = kOrder[k];
        if (utEditInside(x, y, utScaledRectF(e.rect[p], s))) {
            h.kind = kUtEdHitBody;
            h.part = p;
            h.edges = kUtEdgeMove;
            return h;
        }
    }
    return h;
}

inline void utEditClose(UtEdit* e, int why) {
    e->state = kUtEdClosed;
    e->why = why;
    e->part = -1;
    e->button = 0;
}

// The toolbar button under (x, y) - screen px, window-local - in EVERY state but closed (the hit
// test answers only while open), or -1.
inline int utEditToolbarAt(const UtEdit& e, float x, float y) {
    if (e.state == kUtEdClosed) return -1;
    for (int b = 0; b < kUtEdButtons; ++b)
        if (utEditInside(x, y, utScaledRectF(e.tool[b], e.scale))) return b;
    return -1;
}

// LEFT DOWN at (x, y). `inputOk` = the cursor holds no item and the event space is proven: with
// it false nothing is consumed - except CANCEL. physL / physR / swapped pick the button the drag
// polls.
//
// CANCEL answers in EVERY state - open, mid-drag, saving - and whatever the cursor holds, so the
// editor always has an exit that is not Esc (while saving it stops the wait: the file is written,
// the page follows it once read back). And a LEFT DOWN during a drag ends that drag first (its
// release was lost and the poll has not run yet) and is then judged as a click of an open editor
// - otherwise every click would be refused until the drag's poll ran, the toolbar drawn off.
inline int utEditLeftDown(UtEdit* e, float x, float y, int inputOk, int physL, int physR,
                          int swapped) {
    if (e->state == kUtEdClosed) return kUtEdActPass;
    if (utEditToolbarAt(*e, x, y) == kUtEdCancel) {
        utEditClose(e, e->state == kUtEdSaving ? kUtEdWhySaveSlow : kUtEdWhyCancel);
        return kUtEdActCancel;
    }
    if (e->state == kUtEdDrag) {
        e->state = kUtEdOpen;
        e->part = -1;
        e->button = 0;
    }
    if (e->state != kUtEdOpen || !inputOk) return kUtEdActPass;
    const UtEdHit h = utEditHitTest(*e, x, y);
    if (h.kind == kUtEdHitButton) {
        if (h.button == kUtEdSave) return e->ok ? kUtEdActSave : kUtEdActSaveRefused;
        for (int p = 0; p < kUtLayParts; ++p) {
            e->rect[p] = e->def[p];
            e->scr[p] = 0;   // back in the window: SAVE removes the screen keys
        }
        utEditPlaceToolbar(e);
        return kUtEdActReset;
    }
    if (h.kind == kUtEdHitNone) return kUtEdActPass;
    e->state = kUtEdDrag;
    e->part = h.part;
    e->edges = h.edges;
    e->x0 = x;
    e->y0 = y;
    e->start = e->rect[h.part];
    e->button = utEditDragButton(swapped, physL, physR);
    return kUtEdActDrag;
}

// The drag follows the point; true when the rectangle or its mode changed. The pad and the label
// (with the screen known): a MOVE is clamped to the screen and the mode follows the rectangle -
// wholly inside the plate = window mode, across its edge = screen mode; a resize keeps the mode
// (clamped to its bounds) unless it brings a screen part wholly inside the plate. The grid, and
// every part while the screen is unknown, as before: clamped to the plate.
inline bool utEditDragTo(UtEdit* e, float x, float y) {
    if (e->state != kUtEdDrag || e->part < 0 || e->part >= kUtLayParts) return false;
    const int p = e->part;
    const float s = e->scale;
    const int dx = (int)floorf((x - e->x0) / s + 0.5f);
    const int dy = (int)floorf((y - e->y0) / s + 0.5f);
    const bool grid = p == kUtLayGrid;
    int scr = e->scr[p];
    UtRect r;
    if (utEditCanLeave(*e, p) && e->edges == kUtEdgeMove) {
        r = utEditDragRect(e->start, kUtEdgeMove, dx, dy, e->screen, e->minW[p], e->minH[p], 1, 1);
        scr = utRectInside(r, e->plate) ? 0 : 1;
    } else {
        r = utEditDragRect(e->start, e->edges, dx, dy, utEditBounds(*e, p), e->minW[p], e->minH[p],
                           grid ? e->snapW : 1, grid ? e->snapH : 1);
        if (scr && utRectInside(r, e->plate)) scr = 0;
    }
    if (utRectEq(r, e->rect[p]) && scr == e->scr[p]) return false;
    e->rect[p] = r;
    e->scr[p] = scr;
    if (p == kUtLayPad) utEditPlaceToolbar(e);
    return true;
}

// LEFT UP (the engine's event): the drag ends at (x, y). True when a drag ended.
inline bool utEditLeftUp(UtEdit* e, float x, float y) {
    if (e->state != kUtEdDrag) return false;
    utEditDragTo(e, x, y);
    e->state = kUtEdOpen;
    e->part = -1;
    e->button = 0;
    return true;
}

// After a SAVE: `written` = the settings file took the keys. Then the editor waits (boxes still
// parked) until the layout in force is the edited one; a failed write leaves it open.
inline void utEditSaved(UtEdit* e, int written, unsigned now) {
    if (e->state != kUtEdOpen || !written) return;
    e->state = kUtEdSaving;
    e->savedAt = now;
}

struct UtEdTick {
    unsigned now;
    int group;          // the shown group (-1 = the game's own materials page)
    float scale;
    int focused;        // the game has the focus
    int coverW, coverH;
    int layoutOk;       // a layout is in force at a usable scale
    int physL, physR;   // the polled physical buttons
    int haveCursor;
    float cx, cy;       // the polled cursor, screen px window-local
    int matches;        // the layout in force equals the edited rectangles (the save handover)
    // the screen this frame (screenKnown 0 = not known: nothing changes)
    int screenKnown;
    int screenW, screenH;
    float originX, originY;
};

// Once a frame while the page is drawn. Returns the UtEdWhy it closed with this tick, else 0;
// `*moved` = a drag moved a rectangle.
inline int utEditTick(UtEdit* e, const UtEdTick& t, int* moved) {
    if (moved) *moved = 0;
    if (e->state == kUtEdClosed) return kUtEdWhyNone;
    int why = kUtEdWhyNone;
    if (t.now - e->lastTick > kUtEdGoneMs) why = kUtEdWhyGone;
    else if (!t.layoutOk) why = kUtEdWhyLayout;
    else if (t.coverW != e->coverW || t.coverH != e->coverH) why = kUtEdWhyCover;
    else if (fabsf(t.scale - e->scale) > 0.0005f) why = kUtEdWhyScale;
    else if (t.screenKnown && e->screenKnown &&
             (t.screenW != e->screenW || t.screenH != e->screenH))
        why = kUtEdWhyScreen;
    else if (t.group < 0) why = kUtEdWhyGroup;
    else if (!t.focused) why = kUtEdWhyFocus;
    e->lastTick = t.now;
    if (why != kUtEdWhyNone) {
        utEditClose(e, why);
        return why;
    }
    e->group = t.group;
    if (t.screenKnown &&
        (!e->screenKnown || t.originX != e->originX || t.originY != e->originY))
        utEditSetScreen(e, 1, t.screenW, t.screenH, t.originX, t.originY);
    if (e->state == kUtEdDrag) {
        bool m = false;
        if (t.haveCursor) m = utEditDragTo(e, t.cx, t.cy);
        const int held = e->button == 1 ? t.physL : e->button == 2 ? t.physR : 0;
        if (!held) {
            e->state = kUtEdOpen;
            e->part = -1;
            e->button = 0;
        }
        if (moved) *moved = m ? 1 : 0;
        return kUtEdWhyNone;
    }
    if (e->state == kUtEdSaving) {
        if (t.matches) why = kUtEdWhySaved;
        else if (t.now - e->savedAt > kUtEdSaveWaitMs) why = kUtEdWhySaveSlow;
        if (why != kUtEdWhyNone) utEditClose(e, why);
    }
    return why;
}

// The page's boxes stay parked while the editor is open AND was ticked recently: a page that
// left the screen gets its boxes back at the next relayout even before the editor hears of it.
inline bool utEditParks(const UtEdit& e, unsigned now) {
    return e.state != kUtEdClosed && now - e.lastTick <= kUtEdGoneMs;
}

// True when every edited rectangle equals the one in force (the save has been read back): a
// screen part's as its key resolves (utEditResolvedRect). `screenInForce` (UtLayout::screen,
// optional) must then say the same mode for the pad and the label.
inline bool utEditMatches(const UtEdit& e, const UtRect inForce[kUtLayParts],
                          const int* screenInForce = nullptr) {
    for (int p = 0; p < kUtLayParts; ++p) {
        if (!utRectEq(utEditResolvedRect(e, p), inForce[p])) return false;
        if (screenInForce && p != kUtLayGrid && (screenInForce[p] != 0) != (e.scr[p] != 0))
            return false;
    }
    return true;
}

// The value SAVE writes for `part`: "x,y,w,h", or nothing (returns 0) when the rectangle is the
// derived one - its key is removed, so a page left at the defaults keeps no user rectangle (and
// no layout plates of its own).
inline int utEditKeyValue(const UtEdit& e, int part, char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = 0;
    if (part < 0 || part >= kUtLayParts) return 0;
    if (utRectEq(e.rect[part], e.def[part])) return 0;
    return utRectFormat(e.rect[part], out, cap);
}

// True when at least one rectangle differs from the one in force at the open.
inline bool utEditChanged(const UtEdit& e) {
    for (int p = 0; p < kUtLayParts; ++p)
        if (!utRectEq(e.rect[p], e.orig[p]) || e.scr[p] != e.scrOrig[p]) return true;
    return false;
}

}  // namespace ut
