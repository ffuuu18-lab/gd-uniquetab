// test_layoutedit.cpp - the in-game layout editor's pure half, proved without a game:
// src\ut_layoutedit.h, the real header ut_panel.cpp runs.
//
//   tools\build_test_layoutedit.bat        (needs a vcvars64 shell; the .bat calls it itself)
//
//   1. the handle size, the toolbar, the minimum sizes;
//   2. the snap and every clamp of a drag: a move at each plate edge, each edge and a corner, the
//      grid in whole cells, the minimum sizes, the room left;
//   3. the button a drag polls (swapped or not) and the one-shot key;
//   4. the open: refused at a bad scale, on the materials page, twice; the pad fallback;
//   5. the hit test: the toolbar, corners, edge strips, bodies, the order, nothing outside;
//   6. every transition: LEFT DOWN (pass, drag, SAVE, SAVE refused, CANCEL, RESET), the polled
//      drag and its release (swapped too), LEFT UP, every cancel rule of the tick, the save
//      handover (read back, too slow, a failed write), the parking rule;
//   7. the check (every refusal reason through utLayoutResolve), the values SAVE writes;
//   8. a wide cover (Reign of Terror), a pad above the label band (passes), the pad's click on the
//      materials page (ut_padmat.h utPadLeftDown: inside the pad the pad's, outside the box's, an
//      item on the cursor never the pad's), the header's purity;
//   9. the screen mode: the pad and the label across the plate's edge both ways (move and resize),
//      clamped to the screen, the grid never; the toolbar travelling with the pad in both modes;
//      the keys and their files SAVE writes per mode; RESET per mode; the refusal words; every
//      cancel rule in the middle of a screen drag; the screen unknown; a scale under 1;
//  10. CANCEL answers in every state (open, mid-drag, saving) and with an item on the
//      cursor; a LEFT DOWN during a drag whose release was lost ends that drag and is judged;
//  11. the border the editor publishes (utEditWindowClient), the grounds only for the
//      parts in screen mode and a toolbar off the plate, and the window route's proof - the
//      screen input's note of the same click (utEditNoteProves: the tolerance, the age, used up
//      once, NaN) - replayed on a recorded campaign geometry.
// Exit code 0 = all of it held. Argument 1: the repository root (for the purity check).
#include <stdio.h>
#include <string.h>

#include <string>

#include "../src/ut_layoutedit.h"

using namespace ut;

static int g_fails = 0;
static int g_checks = 0;
static void check(bool ok, const char* what) {
    ++g_checks;
    printf("  %-74s %s\n", what, ok ? "OK" : "FAIL");
    if (!ok) ++g_fails;
}
static bool eq(const UtRect& r, int x, int y, int w, int h) {
    return r.x == x && r.y == y && r.w == w && r.h == h;
}

static const char* const kTags[26] = {"MAT", "HEL", "SHO", "CHE", "GLO", "BEL", "PAN", "BOO", "AMU",
                                      "MED", "RNG", "OFF", "SHD", "AX1", "DAG", "MC1", "SCP", "SW1",
                                      "GUN", "AX2", "MC2", "SPR", "SW2", "GN2", "REL", "OWN"};
static const UtCaptions kCaps = {kTags, 26};

// The vanilla page as the live side resolves it with today's ini (no [layout] key).
struct Page {
    UtLayoutInput in;
    UtRect store[kUtLayParts];
    UtLayout L;
};
static void page(Page* pg, int x0, int y0, int uw, int uh, int pw, int ph, float s) {
    UtLayoutSource src = {};
    src.x0 = x0;
    src.y0 = y0;
    src.usableW = uw;
    src.usableH = uh;
    src.plateW = pw;
    src.plateH = ph;
    src.maxCellW = 64;
    src.maxCellH = 128;
    src.rowGap = 2;
    src.padY = 25;
    src.padH = 14;
    src.padGap = 1;
    src.labelSize = 13;
    src.captionMax = 14;
    src.buttons = 26;
    src.scale = s;
    src.widest = utWidestCaptionPx;
    src.widestCtx = &kCaps;
    pg->L = utLayoutFromSource(src, &pg->in, pg->store);
}
static void vanilla(Page* pg, float s) { page(pg, 105, 76, 333, 552, 438, 627, s); }

static UtEditOpenIn openIn(const Page& pg, float s, int group, unsigned now) {
    UtEditOpenIn o = {};
    for (int p = 0; p < kUtLayParts; ++p) {
        o.inForce[p] = pg.L.rect[p];
        o.def[p] = pg.in.def[p];
    }
    o.padFallback = utPadDefault(25, 14, 1);
    o.plate = pg.in.plate;
    o.maxCellW = 64;
    o.maxCellH = 128;
    o.rowGap = 2;
    o.cellW = 64;
    o.cellH = 64;
    o.scale = s;
    o.coverW = 438;
    o.coverH = 627;
    o.group = group;
    o.now = now;
    return o;
}

static UtEdTick tickAt(const UtEdit& e, unsigned now) {
    UtEdTick t = {};
    t.now = now;
    t.group = e.group;
    t.scale = e.scale;
    t.focused = 1;
    t.coverW = e.coverW;
    t.coverH = e.coverH;
    t.layoutOk = 1;
    return t;
}

static UtEdit openVanilla(Page* pg, float s, unsigned now) {
    vanilla(pg, s);
    UtEdit e = {};
    utEditOpen(&e, openIn(*pg, s, 3, now));
    utEditCheck(&e, pg->in);
    return e;
}

int main(int argc, char** argv) {
    const std::string root = argc > 1 ? argv[1] : ".";
    printf("test_layoutedit - the layout editor (src\\ut_layoutedit.h)\n");

    printf("1. handles, toolbar, minimum sizes\n");
    check(utEditHandlePx(0.7f) == 8 && utEditHandlePx(1.0f) == 8 && utEditHandlePx(1.5f) == 9 &&
              utEditHandlePx(2.0f) == 12,
          "handle 8 px at 0.7 and 1.0, 9 at 1.5, 12 at 2.0 (never under 8)");
    {
        UtRect t[kUtEdButtons];
        utEditToolbar(utPadDefault(25, 14, 1), t);
        check(eq(t[0], 257, 25, 52, 14) && eq(t[1], 312, 25, 52, 14) && eq(t[2], 367, 25, 52, 14),
              "toolbar right-aligned in today's pad: SAVE 257, CANCEL 312, RESET 367 at y 25");
        check(t[2].x + t[2].w <= 102 + 320 - kUtPadMargin, "toolbar ends inside the pad's margin");
        const UtRect narrow = {102, 25, 100, 44};
        utEditToolbar(narrow, t);
        check(t[0].x == 102, "a band narrower than the toolbar: it starts at the band's left");
    }
    {
        int w[3], h[3];
        utEditMinSizes(64, 128, 2, 1.0f, w, h);
        check(w[kUtLayGrid] == 64 && h[kUtLayGrid] == 130, "grid minimum = the largest cell + gap");
        check(w[kUtLayPad] == 55 && h[kUtLayPad] == 12, "pad minimum 55 x 12");
        check(w[kUtLayLabel] == 24 && h[kUtLayLabel] == 10, "label minimum 24 x 10 at 1.0");
        utEditMinSizes(64, 128, 2, 0.7f, w, h);
        check(h[kUtLayLabel] == 14, "label minimum 14 tall at 0.7 (a size-9 line)");
    }

    printf("2. snap and clamps\n");
    check(utEditSnap(50, 1, 24, 300) == 50 && utEditSnap(10, 1, 24, 300) == 24 &&
              utEditSnap(400, 1, 24, 300) == 300,
          "unit 1: the length, the minimum, the room");
    check(utEditSnap(333, 64, 64, 333) == 320, "64 px cells: 333 -> 320 (5 cells)");
    check(utEditSnap(352, 64, 64, 500) == 384, "64 px cells: 352 -> 384 (rounds to nearest)");
    check(utEditSnap(-50, 64, 64, 500) == 64, "64 px cells: a negative length -> one cell");
    check(utEditSnap(100, 64, 130, 500) == 192, "the minimum rounds UP to whole cells (130 -> 192)");
    check(utEditSnap(600, 64, 64, 300) == 256, "the room rounds DOWN to whole cells (300 -> 256)");
    check(utEditSnap(100, 64, 130, 100) == 64, "no room for the minimum: the room wins (refused later)");
    check(utEditSnap(30, 64, 64, 30) == 30, "room under one cell: the room");
    const UtRect plate = {0, 1, 438, 627};
    const UtRect pad = {102, 25, 320, 44};
    check(eq(utEditDragRect(pad, kUtEdgeMove, 10, 5, plate, 55, 12, 1, 1), 112, 30, 320, 44),
          "move by (10,5)");
    check(eq(utEditDragRect(pad, kUtEdgeMove, 100, 0, plate, 55, 12, 1, 1), 118, 25, 320, 44),
          "move right stops at the plate's right edge");
    check(eq(utEditDragRect(pad, kUtEdgeMove, -200, -100, plate, 55, 12, 1, 1), 0, 1, 320, 44),
          "move left and up stops at the plate's top-left");
    check(eq(utEditDragRect(pad, kUtEdgeMove, 0, 1000, plate, 55, 12, 1, 1), 102, 584, 320, 44),
          "move down stops at the plate's bottom");
    const UtRect label = {105, 8, 317, 16};
    check(eq(utEditDragRect(label, kUtEdgeR, -400, 0, plate, 24, 10, 1, 1), 105, 8, 24, 16),
          "right edge pulled past the left: the minimum width, the left edge stays");
    check(eq(utEditDragRect(label, kUtEdgeL, 10, 0, plate, 24, 10, 1, 1), 115, 8, 307, 16),
          "left edge in by 10: the right edge stays");
    check(eq(utEditDragRect(label, kUtEdgeL, -500, 0, plate, 24, 10, 1, 1), 0, 8, 422, 16),
          "left edge out past the plate: stops at the plate");
    check(eq(utEditDragRect(pad, kUtEdgeB, 0, 1000, plate, 55, 12, 1, 1), 102, 25, 320, 603),
          "bottom edge out past the plate: stops at the plate");
    check(eq(utEditDragRect(pad, kUtEdgeT, 0, 100, plate, 55, 12, 1, 1), 102, 57, 320, 12),
          "top edge past the bottom: the minimum height, the bottom stays");
    check(eq(utEditDragRect(label, kUtEdgeR | kUtEdgeB, 5, 6, plate, 24, 10, 1, 1), 105, 8, 322, 22),
          "a corner moves two edges");
    const UtRect grid = {105, 76, 333, 552};
    check(eq(utEditDragRect(grid, kUtEdgeR, 0, 0, plate, 64, 130, 64, 66), 105, 76, 320, 552),
          "grid right edge: whole 64 px cells (333 -> 320)");
    check(eq(utEditDragRect(grid, kUtEdgeB, 0, 0, plate, 64, 130, 64, 66), 105, 76, 333, 528),
          "grid bottom edge: whole 66 px rows (552 -> 528, 8 rows)");
    check(eq(utEditDragRect(grid, kUtEdgeT, 0, 44, plate, 64, 130, 64, 66), 105, 100, 333, 528),
          "grid top edge down 44: 8 rows, the bottom stays at 628");
    check(eq(utEditDragRect(grid, kUtEdgeR, -1000, 0, plate, 64, 130, 64, 66), 105, 76, 64, 552),
          "grid squeezed: one cell wide");
    check(eq(utEditDragRect(grid, kUtEdgeB, 0, -1000, plate, 64, 130, 64, 66), 105, 76, 333, 132),
          "grid squeezed: the largest cell's height in whole rows (132)");
    check(eq(utEditDragRect(grid, kUtEdgeL | kUtEdgeT, -105, -75, plate, 64, 130, 64, 66), 54, 34,
             384, 594),
          "grid corner out: whole cells, inside the plate");
    check(eq(utEditDragRect(grid, kUtEdgeMove, 7, 3, plate, 64, 130, 64, 66), 105, 76, 333, 552) &&
              eq(utEditDragRect(grid, kUtEdgeMove, -7, -3, plate, 64, 130, 64, 66), 98, 73, 333,
                 552),
          "a grid move keeps the size, 1 px steps, clamped at the plate's right and bottom");

    printf("3. drag button and one-shot key\n");
    check(utEditDragButton(0, 1, 0) == 1 && utEditDragButton(0, 0, 1) == 2 &&
              utEditDragButton(0, 0, 0) == 0 && utEditDragButton(0, 1, 1) == 1,
          "not swapped: the left one, else the right one held, else none");
    check(utEditDragButton(1, 0, 1) == 2 && utEditDragButton(1, 1, 0) == 1 &&
              utEditDragButton(1, 1, 1) == 2 && utEditDragButton(1, 0, 0) == 0,
          "swapped (SM_SWAPBUTTON): the right one first");
    {
        int latch = 0;
        const bool a = utEditOneShot(1, &latch);
        latch = 1;   // consumed
        const bool b = utEditOneShot(1, &latch);
        const bool c = utEditOneShot(0, &latch);
        const int afterZero = latch;
        const bool d = utEditOneShot(1, &latch);
        check(a && !b && !c && afterZero == 0 && d,
              "one-shot: fires on 1, not again until the file reads 0, then again");
        int l2 = 0;
        check(!utEditOneShot(0, &l2), "one-shot: 0 never fires");
    }

    printf("4. open\n");
    {
        Page pg;
        vanilla(&pg, 1.0f);
        UtEdit e = {};
        check(!utEditOpen(&e, openIn(pg, 0.0f, 3, 0)) && e.state == kUtEdClosed,
              "no usable UI scale: refused");
        check(!utEditOpen(&e, openIn(pg, 1.0f, -1, 0)) && e.state == kUtEdClosed,
              "the game's own materials page (group -1): refused");
        check(utEditOpen(&e, openIn(pg, 1.0f, 3, 0)) && e.state == kUtEdOpen,
              "a collection group at 1.0: open");
        check(!utEditOpen(&e, openIn(pg, 1.0f, 3, 0)), "already open: refused");
        check(eq(e.rect[kUtLayGrid], 105, 76, 333, 552) && eq(e.rect[kUtLayPad], 102, 25, 320, 44) &&
                  eq(e.rect[kUtLayLabel], 105, 8, 317, 16),
              "the rectangles in force (today's) are the ones edited");
        check(e.snapW == 64 && e.snapH == 66, "the grid snaps to the shown group's 64 x 66 step");
        utEditCheck(&e, pg.in);
        check(e.ok == 1 && !utEditChanged(e), "today's layout passes the check, nothing changed");
        UtEditOpenIn o = openIn(pg, 1.0f, 3, 0);
        const UtRect none = {-30000, -30000, 0, 0};
        o.inForce[kUtLayPad] = none;
        o.def[kUtLayPad] = none;
        UtEdit f = {};
        utEditOpen(&f, o);
        check(eq(f.rect[kUtLayPad], 102, 25, 320, 44) && eq(f.def[kUtLayPad], 102, 25, 320, 44) &&
                  eq(f.tool[0], 257, 25, 52, 14),
              "no pad in force (refused by the band): today's pad to start from");
    }

    printf("5. hit test\n");
    {
        Page pg;
        UtEdit e = openVanilla(&pg, 1.0f, 0);
        UtEdHit h = utEditHitTest(e, 260, 30);
        check(h.kind == kUtEdHitButton && h.button == kUtEdSave, "SAVE at (260,30)");
        h = utEditHitTest(e, 330, 30);
        check(h.kind == kUtEdHitButton && h.button == kUtEdCancel, "CANCEL at (330,30)");
        h = utEditHitTest(e, 380, 38);
        check(h.kind == kUtEdHitButton && h.button == kUtEdReset, "RESET at (380,38)");
        h = utEditHitTest(e, 106, 9);
        check(h.kind == kUtEdHitHandle && h.part == kUtLayLabel && h.edges == (kUtEdgeL | kUtEdgeT),
              "the label's top-left corner");
        h = utEditHitTest(e, 420, 23);
        check(h.kind == kUtEdHitHandle && h.part == kUtLayLabel && h.edges == (kUtEdgeR | kUtEdgeB),
              "the label's bottom-right corner (it wins over the pad's corner below it)");
        h = utEditHitTest(e, 200, 8);
        check(h.kind == kUtEdHitHandle && h.part == kUtLayLabel && h.edges == kUtEdgeT,
              "the label's top edge strip");
        h = utEditHitTest(e, 200, 16);
        check(h.kind == kUtEdHitBody && h.part == kUtLayLabel && h.edges == kUtEdgeMove,
              "the label's body");
        h = utEditHitTest(e, 103, 50);
        check(h.kind == kUtEdHitHandle && h.part == kUtLayPad && h.edges == kUtEdgeL,
              "the pad's left edge strip");
        h = utEditHitTest(e, 150, 50);
        check(h.kind == kUtEdHitBody && h.part == kUtLayPad, "the pad's body");
        h = utEditHitTest(e, 200, 68);
        check(h.kind == kUtEdHitHandle && h.part == kUtLayPad && h.edges == kUtEdgeB,
              "the pad's bottom edge strip");
        h = utEditHitTest(e, 107, 77);
        check(h.kind == kUtEdHitHandle && h.part == kUtLayGrid && h.edges == (kUtEdgeL | kUtEdgeT),
              "the grid's top-left corner");
        h = utEditHitTest(e, 436, 300);
        check(h.kind == kUtEdHitHandle && h.part == kUtLayGrid && h.edges == kUtEdgeR,
              "the grid's right edge strip");
        h = utEditHitTest(e, 300, 300);
        check(h.kind == kUtEdHitBody && h.part == kUtLayGrid, "the grid's body");
        h = utEditHitTest(e, 50, 300);
        check(h.kind == kUtEdHitNone, "beside everything: nothing (the engine gets the click)");
        UtEdit d = openVanilla(&pg, 2.0f, 0);
        h = utEditHitTest(d, 2.0f * 102 + 5, 2.0f * 50);
        check(h.kind == kUtEdHitHandle && h.part == kUtLayPad && h.edges == kUtEdgeL,
              "at 2.0 the strip is 12 px: 5 px inside the pad's left edge still grabs it");
        h = utEditHitTest(d, 2.0f * 300, 2.0f * 300);
        check(h.kind == kUtEdHitBody && h.part == kUtLayGrid, "at 2.0 the grid's body");
        e.state = kUtEdDrag;
        check(utEditHitTest(e, 260, 30).kind == kUtEdHitNone, "not open (dragging): no hit");
    }

    printf("6. transitions\n");
    {
        Page pg;
        UtEdit e = openVanilla(&pg, 1.0f, 1000);
        check(utEditLeftDown(&e, 150, 50, 0, 1, 0, 0) == kUtEdActPass && e.state == kUtEdOpen,
              "LEFT DOWN with an item on the cursor or the space unproven: passed");
        check(utEditLeftDown(&e, 50, 300, 1, 1, 0, 0) == kUtEdActPass && e.state == kUtEdOpen,
              "LEFT DOWN on nothing: passed");
        check(utEditLeftDown(&e, 150, 50, 1, 1, 0, 0) == kUtEdActDrag && e.state == kUtEdDrag &&
                  e.part == kUtLayPad && e.button == 1,
              "LEFT DOWN on the pad: drag, polling the left button");
        check(utEditLeftDown(&e, 150, 50, 1, 1, 0, 0) == kUtEdActDrag && e.state == kUtEdDrag &&
                  e.part == kUtLayPad && utRectEq(e.start, e.rect[kUtLayPad]),
              "a second LEFT DOWN while dragging (its release lost): the old drag ends there, "
              "the click starts a new one");
        UtEdTick t = tickAt(e, 1050);
        t.physL = 1;
        t.haveCursor = 1;
        t.cx = 160;
        t.cy = 55;
        int moved = 0;
        check(utEditTick(&e, t, &moved) == kUtEdWhyNone && moved == 1 &&
                  eq(e.rect[kUtLayPad], 112, 30, 320, 44) && e.state == kUtEdDrag,
              "tick, button held: the pad follows the polled cursor");
        t = tickAt(e, 1100);
        t.physL = 0;
        t.haveCursor = 1;
        t.cx = 170;
        t.cy = 55;
        check(utEditTick(&e, t, &moved) == kUtEdWhyNone && eq(e.rect[kUtLayPad], 118, 30, 320, 44) &&
                  e.state == kUtEdOpen && e.part == -1,
              "tick, button released (polled): the drag ends there, clamped at the plate");
        utEditCheck(&e, pg.in);
        check(e.ok == 1 && utEditChanged(e), "the moved pad still passes; the layout changed");
        check(utEditLeftDown(&e, 150, 16, 1, 1, 0, 0) == kUtEdActDrag && e.part == kUtLayLabel,
              "LEFT DOWN on the label: drag");
        check(utEditLeftUp(&e, 160, 16) && e.state == kUtEdOpen &&
                  eq(e.rect[kUtLayLabel], 115, 8, 317, 16),
              "LEFT UP (the engine's event): the drag ends at the event's point");
        check(!utEditLeftUp(&e, 160, 16), "LEFT UP with no drag: nothing");
        // swapped buttons
        check(utEditLeftDown(&e, 300, 300, 1, 0, 1, 1) == kUtEdActDrag && e.button == 2,
              "swapped: the drag polls the physical right button");
        t = tickAt(e, 1150);
        t.physL = 1;
        t.physR = 0;
        utEditTick(&e, t, &moved);
        check(e.state == kUtEdOpen, "swapped: the right button's release ends it (left held)");
        // a click with no button held any more
        check(utEditLeftDown(&e, 300, 300, 1, 0, 0, 0) == kUtEdActDrag && e.button == 0,
              "a click already released: a drag that polls nothing");
        t = tickAt(e, 1160);
        t.physL = 1;
        t.physR = 1;
        utEditTick(&e, t, &moved);
        check(e.state == kUtEdOpen, "it ends on the next tick");
        // the parking rule
        check(utEditParks(e, 1160) && utEditParks(e, 1160 + kUtEdGoneMs) &&
                  !utEditParks(e, 1161 + kUtEdGoneMs),
              "boxes parked while open and drawn within 400 ms, not after");
        // RESET - the toolbar travelled with the pad (118,30): RESET at 383..435 x 30..44
        check(eq(e.tool[kUtEdReset], 383, 30, 52, 14) && eq(e.tool[kUtEdSave], 273, 30, 52, 14),
              "the toolbar travelled with the moved pad (+16,+5)");
        check(utEditHitTest(e, 380, 30).kind != kUtEdHitButton,
              "the toolbar's old place is no RESET any more");
        check(utEditLeftDown(&e, 390, 35, 1, 1, 0, 0) == kUtEdActReset && e.state == kUtEdOpen &&
                  eq(e.rect[kUtLayPad], 102, 25, 320, 44) && eq(e.rect[kUtLayLabel], 105, 8, 317, 16),
              "RESET: the derived rectangles, still open");
        check(eq(e.tool[kUtEdReset], 367, 25, 52, 14), "RESET: the toolbar is back with the pad");
        // SAVE refused
        e.rect[kUtLayPad].y = 80;
        utEditCheck(&e, pg.in);
        check(e.ok == 0 && e.partWhy[kUtLayPad] == kUtLayoutOverlap &&
                  e.against[kUtLayPad] == kUtLayGrid,
              "a pad dropped on the grid: refused (overlap, against the grid)");
        check(utEditLeftDown(&e, 260, 30, 1, 1, 0, 0) == kUtEdActSaveRefused && e.state == kUtEdOpen,
              "SAVE on a red outline: consumed, nothing done");
        e.rect[kUtLayPad].y = 25;
        utEditCheck(&e, pg.in);
        // SAVE, a failed write
        check(utEditLeftDown(&e, 260, 30, 1, 1, 0, 0) == kUtEdActSave, "SAVE allowed");
        utEditSaved(&e, 0, 1200);
        check(e.state == kUtEdOpen, "the write failed: still open");
        // SAVE, read back
        utEditSaved(&e, 1, 1200);
        check(e.state == kUtEdSaving && utEditParks(e, 1200), "written: saving, boxes still parked");
        check(utEditLeftDown(&e, 150, 50, 1, 1, 0, 0) == kUtEdActPass, "saving: no input taken");
        t = tickAt(e, 1300);
        check(utEditTick(&e, t, &moved) == kUtEdWhyNone && e.state == kUtEdSaving,
              "not read back yet: waits");
        t = tickAt(e, 1400);
        t.matches = 1;
        check(utEditTick(&e, t, &moved) == kUtEdWhySaved && e.state == kUtEdClosed &&
                  e.why == kUtEdWhySaved && !utEditParks(e, 1400),
              "read back: closed (saved), the boxes come back");
        check(utEditTick(&e, tickAt(e, 1500), &moved) == kUtEdWhyNone, "closed: a tick does nothing");
        // SAVE, too slow
        e = openVanilla(&pg, 1.0f, 2000);
        utEditLeftDown(&e, 260, 30, 1, 1, 0, 0);
        utEditSaved(&e, 1, 2000);
        unsigned now = 2000;
        int why = 0;
        for (int i = 0; i < 40 && !why; ++i) {
            now += 100;
            why = utEditTick(&e, tickAt(e, now), &moved);
        }
        check(why == kUtEdWhySaveSlow && now == 2000 + kUtEdSaveWaitMs + 100,
              "never read back: closed after 3 s (saved, slow)");
        // CANCEL
        e = openVanilla(&pg, 1.0f, 3000);
        check(utEditLeftDown(&e, 330, 30, 1, 1, 0, 0) == kUtEdActCancel && e.state == kUtEdClosed &&
                  e.why == kUtEdWhyCancel,
              "CANCEL: closed, nothing written");
        // the cancel rules of the tick
        struct Rule {
            const char* what;
            int why;
        };
        const Rule rules[] = {
            {"page not drawn for 401 ms (caravan closed / other page / teardown)", kUtEdWhyGone},
            {"no layout in force", kUtEdWhyLayout},
            {"another cover (view change)", kUtEdWhyCover},
            {"UI scale 1.0 -> 0.7", kUtEdWhyScale},
            {"the game's own materials page", kUtEdWhyGroup},
            {"focus lost", kUtEdWhyFocus}};
        for (int r = 0; r < 6; ++r) {
            UtEdit c = openVanilla(&pg, 1.0f, 5000);
            utEditLeftDown(&c, 150, 50, 1, 1, 0, 0);   // mid-drag: every rule cancels a drag too
            UtEdTick k = tickAt(c, 5100);
            k.physL = 1;
            switch (rules[r].why) {
                case kUtEdWhyGone: k.now = 5000 + kUtEdGoneMs + 1; break;
                case kUtEdWhyLayout: k.layoutOk = 0; break;
                case kUtEdWhyCover: k.coverW = 974; break;
                case kUtEdWhyScale: k.scale = 0.7f; break;
                case kUtEdWhyGroup: k.group = -1; break;
                default: k.focused = 0; break;
            }
            const UtRect before = c.rect[kUtLayPad];
            const int got = utEditTick(&c, k, &moved);
            char line[128];
            _snprintf_s(line, sizeof(line), _TRUNCATE, "cancel: %s", rules[r].what);
            check(got == rules[r].why && c.state == kUtEdClosed && c.why == rules[r].why &&
                      utRectEq(before, c.rect[kUtLayPad]),
                  line);
        }
        UtEdit c = openVanilla(&pg, 1.0f, 6000);
        UtEdTick k = tickAt(c, 6000 + kUtEdGoneMs);
        k.group = 7;
        check(utEditTick(&c, k, &moved) == kUtEdWhyNone && c.group == 7 && c.state == kUtEdOpen,
              "400 ms and another collection group: still open, the group follows");
        utEditLeftDown(&c, 150, 50, 1, 1, 0, 0);
        utEditSaved(&c, 1, 6500);
        check(c.state == kUtEdDrag, "SAVE cannot start while a drag runs");
        // the drag at UI scale 0.7: screen px -> record px
        UtEdit s7 = openVanilla(&pg, 0.7f, 0);
        const UtRectF pr = utScaledRectF(s7.rect[kUtLayPad], 0.7f);
        utEditLeftDown(&s7, pr.x + 30, pr.y + 15, 1, 1, 0, 0);
        utEditDragTo(&s7, pr.x + 37, pr.y + 15 - 7);
        check(s7.part == kUtLayPad && eq(s7.rect[kUtLayPad], 112, 15, 320, 44),
              "0.7: 7 screen px = 10 record px");
        utEditLeftUp(&s7, pr.x + 37, pr.y + 15 - 7);
        // the grid's step follows the group's cell
        utEditSetCell(&s7, 32, 32, 2);
        check(s7.snapW == 32 && s7.snapH == 34, "another group's cell: the grid snaps to 32 x 34");
    }

    printf("7. the check and what SAVE writes\n");
    {
        Page pg;
        UtEdit e = openVanilla(&pg, 1.0f, 0);
        char v[32];
        check(utEditKeyValue(e, kUtLayGrid, v, sizeof(v)) == 0 && v[0] == 0 &&
                  utEditKeyValue(e, kUtLayPad, v, sizeof(v)) == 0 &&
                  utEditKeyValue(e, kUtLayLabel, v, sizeof(v)) == 0,
              "all derived: SAVE removes the three keys");
        UtRect inForce[kUtLayParts] = {pg.L.rect[0], pg.L.rect[1], pg.L.rect[2]};
        check(utEditMatches(e, inForce), "matches the layout in force");
        e.rect[kUtLayPad] = UtRect{102, 30, 320, 40};
        check(utEditKeyValue(e, kUtLayPad, v, sizeof(v)) > 0 && !strcmp(v, "102,30,320,40"),
              "a moved pad: \"102,30,320,40\"");
        check(!utEditMatches(e, inForce), "no longer matches until read back");
        check(utEditKeyValue(e, 5, v, sizeof(v)) == 0, "a part out of range: nothing");
        e.rect[kUtLayGrid] = UtRect{105, 120, 320, 462};
        utEditCheck(&e, pg.in);
        check(e.ok == 1 && e.partWhy[kUtLayPad] == kUtLayoutOk &&
                  e.partWhy[kUtLayGrid] == kUtLayoutOk && e.partWhy[kUtLayLabel] == kUtLayoutOk,
              "grid 105,120,320,462 and pad 102,30,320,40: all three hold (nothing meets)");
        e.rect[kUtLayLabel] = UtRect{105, 20, 317, 16};
        utEditCheck(&e, pg.in);
        check(e.ok == 0 && e.partWhy[kUtLayLabel] == kUtLayoutOverlap &&
                  e.against[kUtLayLabel] == kUtLayPad,
              "a label dropped on the pad: refused (overlap, against the pad)");
        e.rect[kUtLayLabel] = utLabelDefault();
        e.rect[kUtLayPad] = UtRect{102, 25, 60, 12};
        utEditCheck(&e, pg.in);
        check(e.partWhy[kUtLayPad] == kUtLayoutPadNoFit, "a 60 x 12 pad: no room for 26 buttons");
        e.rect[kUtLayPad] = utPadDefault(25, 14, 1);
        e.rect[kUtLayLabel] = UtRect{105, 8, 24, 8};
        utEditCheck(&e, pg.in);
        check(e.partWhy[kUtLayLabel] == kUtLayoutLabelSmall, "an 8 px label: too small");
        e.rect[kUtLayLabel] = utLabelDefault();
        e.rect[kUtLayGrid] = UtRect{300, 76, 333, 552};
        utEditCheck(&e, pg.in);
        check(e.partWhy[kUtLayGrid] == kUtLayoutOutside && e.ok == 0, "a grid off the page: outside");
        e.rect[kUtLayGrid] = UtRect{105, 76, 60, 552};
        utEditCheck(&e, pg.in);
        check(e.partWhy[kUtLayGrid] == kUtLayoutGridSmall, "a grid narrower than a cell: too small");
        e.rect[kUtLayGrid] = UtRect{105, 120, 320, 462};
        utEditCheck(&e, pg.in);
        check(e.ok == 1 && utEditKeyValue(e, kUtLayGrid, v, sizeof(v)) > 0 &&
                  !strcmp(v, "105,120,320,462"),
              "the research's moved grid passes: \"105,120,320,462\"");
    }

    printf("8. a wide cover, purity\n");
    {
        Page pg;
        page(&pg, 12, 76, 962, 584, 974, 659, 1.0f);
        check(eq(pg.in.plate, 0, 1, 974, 659), "Reign of Terror: the plate 974 x 659");
        UtEditOpenIn o = openIn(pg, 1.0f, 0, 0);
        o.coverW = 974;
        o.coverH = 659;
        o.cellW = 32;
        o.cellH = 32;
        UtEdit e = {};
        utEditOpen(&e, o);
        utEditCheck(&e, pg.in);
        check(e.ok == 1, "its derived layout passes");
        utEditLeftDown(&e, 150, 50, 1, 1, 0, 0);
        utEditLeftUp(&e, 150 + 328, 50);
        char v[32];
        utEditCheck(&e, pg.in);
        check(eq(e.rect[kUtLayPad], 430, 25, 320, 44) && e.ok == 1 &&
                  utEditKeyValue(e, kUtLayPad, v, sizeof(v)) > 0 && !strcmp(v, "430,25,320,44"),
              "the pad dragged off the band to 430,25 (the ROT experiment)");
        utEditLeftDown(&e, 12 + 962 - 1, 300, 1, 1, 0, 0);
        check(e.part == kUtLayGrid && e.edges == kUtEdgeR, "the grid's right edge");
        utEditLeftUp(&e, 12 + 400, 300);
        check(eq(e.rect[kUtLayGrid], 12, 76, 416, 584), "pulled to 400 px: 13 cells of 32 (416)");
        utEditCheck(&e, pg.in);
        check(e.ok == 1, "and it passes");
        // A pad above the label band and over the materials page's box rows is the user's to
        // choose: the editor passes it, and the materials page draws it where it is, over the
        // boxes, its clicks its own (test_layout 7b, and the click rule below).
        e.rect[kUtLayGrid] = UtRect{910, 76, 64, 584};
        e.rect[kUtLayPad] = UtRect{52, 1, 780, 153};
        e.rect[kUtLayLabel] = UtRect{428, 165, 317, 23};
        utEditCheck(&e, pg.in);
        check(e.ok == 1, "a pad at 52,1,780,153 (above the label band) with the grid and label moved: passes");
    }
    // THE PAD'S CLICK (ut_padmat.h utPadLeftDown, what ut_plate.cpp's mouse slot runs). On the
    // game's own materials page the pad is drawn over the page's boxes, and a LEFT DOWN inside it
    // is the pad's: a button acts, the ground between the buttons keeps the click, a box under it
    // never sees it. Outside the pad the click goes to the engine untouched. An item on the cursor
    // is never the pad's, on any page (kUtPadPassCursorItem: the engine's deposit), so a pad
    // click can never drop it. The collection pages keep their order.
    {
        // args: drawn, cursorRead, cursorItem, onButton, matPad, inBox, boxFault, proven
        struct Case {
            int a[8];
            int act, reason;
            const char* what;
        };
        static const Case kCases[] = {
            {{1, 1, 0, 1, 1, 1, 0, 1}, kUtPadClickButton, kUtPadPassNone,
             "materials pad: a button over a box acts, the box never sees it"},
            {{1, 1, 0, 1, 1, 0, 0, 1}, kUtPadClickButton, kUtPadPassNone,
             "materials pad: a button over the plate acts"},
            {{1, 1, 0, 0, 1, 1, 0, 1}, kUtPadClickKeep, kUtPadPassNone,
             "materials pad: between the buttons, over a box: kept, nothing acts"},
            {{1, 1, 0, 0, 1, 1, 0, 1}, kUtPadClickKeep, kUtPadPassNone,
             "materials pad: a right-click (no button) over a box: kept too"},
            {{1, 1, 1, 0, 1, 1, 0, 1}, kUtPadClickPass, kUtPadPassCursorItem,
             "materials pad: a right-click with an item: the engine's"},
            {{1, 1, 0, 1, 1, 0, 1, 1}, kUtPadClickButton, kUtPadPassNone,
             "materials pad: a faulted box test does not matter there"},
            {{1, 1, 1, 1, 1, 1, 0, 1}, kUtPadClickPass, kUtPadPassCursorItem,
             "materials pad: an item on the cursor is the engine's deposit"},
            {{1, 0, 0, 1, 1, 1, 0, 1}, kUtPadClickPass, kUtPadPassCursorFault,
             "materials pad: an unreadable cursor counts as an item"},
            {{1, 1, 0, 1, 1, 1, 0, 0}, kUtPadClickPass, kUtPadPassNotProven,
             "materials pad: the event space not proven yet: passed"},
            {{0, 1, 0, 1, 1, 1, 0, 1}, kUtPadClickPass, kUtPadPassNotDrawn,
             "materials pad not painted lately: passed"},
            {{1, 1, 0, 0, 0, 1, 0, 1}, kUtPadClickPass, kUtPadPassNoButton,
             "materials page, beside the pad, on a box: the box's"},
            {{1, 1, 0, 1, 0, 0, 0, 1}, kUtPadClickButton, kUtPadPassNone,
             "collection page: a button acts"},
            {{1, 1, 0, 1, 0, 1, 0, 1}, kUtPadClickPass, kUtPadPassInBox,
             "collection page: a button over a live box stays the box's"},
            {{1, 1, 0, 1, 0, 0, 1, 1}, kUtPadClickPass, kUtPadPassBoxFault,
             "collection page: a faulted box test passes"},
            {{1, 1, 0, 0, 0, 1, 1, 0}, kUtPadClickPass, kUtPadPassNoButton,
             "collection page: no button comes before the box and the proof"},
            {{1, 1, 1, 1, 0, 0, 0, 1}, kUtPadClickPass, kUtPadPassCursorItem,
             "collection page: an item on the cursor is the engine's deposit"},
            {{1, 1, 0, 1, 0, 0, 0, 0}, kUtPadClickPass, kUtPadPassNotProven,
             "collection page: not proven: passed"},
        };
        for (const Case& c : kCases) {
            const UtPadClick r =
                utPadLeftDown(c.a[0], c.a[1], c.a[2], c.a[3], c.a[4], c.a[5], c.a[6], c.a[7]);
            check(r.act == c.act && r.reason == c.reason, c.what);
        }
        check(utPadRefusedAct(1) == kUtPadClickKeep && utPadRefusedAct(0) == kUtPadClickPass,
              "a refused target: kept on the materials pad, passed on a collection page");
        // A Reign of Terror pad at UI scale 0.878: every one of its 30 buttons lies inside
        // the pad's scaled rectangle (so a click on any is the pad's), and a box beside the pad -
        // the top row's last one at 881,13 - lies outside it (its click still goes to the box).
        // 30 with the class-word groups (the staff and the javelin groups are SPE; 31 without
        // them).
        static const char* const kRot[30] = {
            "MAT", "HEL", "SHO", "CHE", "GLO", "BEL", "PAN", "BOO", "AMU", "MED", "RNG",
            "OFF", "SHD", "AX1", "DAG", "MC1", "SCP", "SW1", "GUN", "AX2", "MC2", "SPR",
            "SW2", "GN2", "BOW", "SPE", "REL", "JWL", "OTH", "OWN"};
        static const UtCaptions kRotCaps = {kRot, 30};
        const float s = 0.878f;
        const UtRect rotPad = {52, 1, 780, 153};
        const UtPadFit f = utPadFit(rotPad, 30, 1, s, kUtCaptionMaxDefault, utWidestCaptionPx, &kRotCaps);
        const UtRectF pf = utScaledRectF(rotPad, s);
        int inside = 0;
        for (int i = 0; f.ok && i < 30; ++i) {
            const UtRectF b = utScaledRectF(utPadCellRect(f, i), s);
            if (utRectFHas(pf, b.x, b.y) && utRectFHas(pf, b.x + b.w - 1.0f, b.y + b.h - 1.0f)) ++inside;
        }
        check(f.ok && inside == 30, "ROT at 0.878: all 30 buttons lie inside the pad's rectangle");
        const UtRectF beside = utScaledRectF(UtRect{881, 13, 64, 64}, s);
        check(!utRectFHas(pf, beside.x + beside.w / 2, beside.y + beside.h / 2),
              "the box at 881,13 lies beside the pad: its click is the box's");
    }
    {
        std::string text;
        FILE* f = nullptr;
        const std::string path = root + "\\src\\ut_layoutedit.h";
        if (!fopen_s(&f, path.c_str(), "rb") && f) {
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
            fclose(f);
        }
        size_t includes = 0, at = 0;
        bool onlyAllowed = !text.empty();
        while ((at = text.find("#include", at)) != std::string::npos) {
            const size_t eol = text.find('\n', at);
            const std::string line = text.substr(at, eol - at);
            if (line.find("<math.h>") == std::string::npos &&
                line.find("<stddef.h>") == std::string::npos &&
                line.find("\"ut_layout.h\"") == std::string::npos)
                onlyAllowed = false;
            ++includes;
            at = eol;
        }
        check(onlyAllowed && includes == 3,
              "the header includes only <math.h>, <stddef.h> and ut_layout.h");
    }

    printf("9. the screen mode (pad_screen / label_screen)\n");
    {
        // vanilla at 1.0 on a 1920 x 1080 screen, the window's origin at 1169,451 (a logged one)
        const int SW = 1920, SH = 1080;
        const float OX = 1169.0f, OY = 451.0f;
        Page pg;
        vanilla(&pg, 1.0f);
        auto openScr = [&](unsigned now) {
            UtEditOpenIn o = openIn(pg, 1.0f, 3, now);
            o.screenKnown = 1;
            o.screenW = SW;
            o.screenH = SH;
            o.originX = OX;
            o.originY = OY;
            UtEdit n = {};
            utEditOpen(&n, o);
            utEditCheck(&n, pg.in);
            return n;
        };
        auto tickScr = [&](const UtEdit& n, unsigned now) {
            UtEdTick t = tickAt(n, now);
            t.screenKnown = 1;
            t.screenW = SW;
            t.screenH = SH;
            t.originX = OX;
            t.originY = OY;
            return t;
        };
        // a whole drag through the engine's events: LEFT DOWN at (x, y), LEFT UP at (x+dx, y+dy)
        auto drag = [&](UtEdit* n, float x, float y, float dx, float dy) {
            if (utEditLeftDown(n, x, y, 1, 1, 0, 0) != kUtEdActDrag) return false;
            utEditLeftUp(n, x + dx, y + dy);
            utEditCheck(n, pg.in);
            return true;
        };
        auto planHas = [](const UtEditSavePlan& p, const char* key, const char* value, int mainOnly) {
            for (int i = 0; i < p.n; ++i) {
                if (strcmp(p.k[i].key, key) != 0) continue;
                if (p.k[i].mainOnly != mainOnly) return false;
                return value ? (p.k[i].set == 1 && strcmp(p.k[i].value, value) == 0) : p.k[i].set == 0;
            }
            return false;
        };
        auto planLacks = [](const UtEditSavePlan& p, const char* key) {
            for (int i = 0; i < p.n; ++i)
                if (strcmp(p.k[i].key, key) == 0) return false;
            return true;
        };
        int moved = 0;
        char v[64];
        UtEditSavePlan sp;

        UtEdit e = openScr(5000);
        check(e.screenKnown == 1 && eq(e.screen, -1169, -451, 1920, 1080),
              "the screen as window-local record px: -1169,-451 1920 x 1080");
        check(!utEditAnyScreen(e) && utEditScreenHint(e) == nullptr &&
                  eq(e.tool[kUtEdSave], 257, 25, 52, 14),
              "opened in window mode, the toolbar at the top right of the pad");
        // the pad across the plate's right edge, and back
        check(drag(&e, 150, 60, 300, 0) && e.scr[kUtLayPad] == 1 &&
                  eq(e.rect[kUtLayPad], 402, 25, 320, 44),
              "the pad dragged across the plate's right edge: screen mode, where the cursor took it");
        check(eq(e.tool[kUtEdSave], 557, 25, 52, 14) && eq(e.tool[kUtEdReset], 667, 25, 52, 14),
              "the toolbar travelled with the pad onto the screen");
        check(utEditHitTest(e, 560, 30).kind == kUtEdHitButton &&
                  utEditHitTest(e, 560, 30).button == kUtEdSave,
              "SAVE is hit at its new place (window-local px off the plate)");
        check(e.ok == 1 && e.partWhy[kUtLayPad] == kUtLayoutOk, "the screen pad holds (clear of the grid)");
        check(utEditScreenHint(e) &&
                  strcmp(utEditScreenHint(e), "on the screen - SAVE writes pad_screen") == 0,
              "the status line: on the screen - SAVE writes pad_screen");
        utScreenRectFormat(utEditScreenKey(e, kUtLayPad), v, sizeof(v));
        check(strcmp(v, "tr,29,476,320,44") == 0, "its key: the nearest corner - tr,29,476,320,44");
        check(eq(utEditResolvedRect(e, kUtLayPad), 402, 25, 320, 44), "the key resolves back to where it is");
        UtRectF gp, gl;
        utEditScreenGrounds(e, &gp, &gl);
        check(gp.x == 398.0f && gp.y == 21.0f && gp.w == 328.0f && gp.h == 52.0f && gl.w == 0.0f,
              "its click ground: the pad with the toolbar, grown by half a handle; no label ground");
        check(utEditChanged(e), "a mode change is a change");
        utEditSavePlanOf(e, "438x627", &sp);
        check(sp.n == 4 && planHas(sp, "pad_screen", "tr,29,476,320,44", 1) &&
                  planLacks(sp, "pad_438x627") && planHas(sp, "grid_438x627", nullptr, 0) &&
                  planHas(sp, "label_438x627", nullptr, 0) && planHas(sp, "label_screen", nullptr, 1),
              "SAVE, pad on the screen: pad_screen=tr,... (main ini), pad_438x627 untouched");
        {
            const UtRect inForce[kUtLayParts] = {pg.in.def[kUtLayGrid], {402, 25, 320, 44},
                                                 pg.in.def[kUtLayLabel]};
            const int scrOn[kUtLayParts] = {0, 1, 0}, scrOff[kUtLayParts] = {0, 0, 0};
            check(utEditMatches(e, inForce, scrOn) && !utEditMatches(e, inForce, scrOff),
                  "the save handover: the rectangle AND the mode must be in force");
        }
        check(drag(&e, 452, 60, -300, 0) && e.scr[kUtLayPad] == 0 &&
                  eq(e.rect[kUtLayPad], 102, 25, 320, 44) && eq(e.tool[kUtEdSave], 257, 25, 52, 14),
              "dragged wholly back inside the plate: window mode, the toolbar with it");
        utEditScreenGrounds(e, &gp, &gl);
        check(gp.w == 0.0f && gl.w == 0.0f, "window mode: no click ground outside the window");
        utEditSavePlanOf(e, "438x627", &sp);
        check(sp.n == 5 && planHas(sp, "pad_438x627", nullptr, 0) && planHas(sp, "pad_screen", nullptr, 1),
              "SAVE, pad back in the window: pad_438x627 and pad_screen both removed");
        check(drag(&e, 150, 60, 10, 10) && e.scr[kUtLayPad] == 0 && eq(e.rect[kUtLayPad], 112, 35, 320, 44),
              "a move inside the plate stays in window mode");
        utEditSavePlanOf(e, "438x627", &sp);
        check(planHas(sp, "pad_438x627", "112,35,320,44", 0) && planHas(sp, "pad_screen", nullptr, 1),
              "SAVE, a moved window pad: pad_438x627=112,35,320,44 (the view's file), pad_screen removed");
        // clamped to the screen
        check(drag(&e, 180, 70, 5000, -5000) && e.scr[kUtLayPad] == 1 &&
                  eq(e.rect[kUtLayPad], 431, -451, 320, 44),
              "a move far past the screen: clamped to the screen's top right (431,-451)");
        utScreenRectFormat(utEditScreenKey(e, kUtLayPad), v, sizeof(v));
        check(strcmp(v, "tr,0,0,320,44") == 0, "its key: tr,0,0,320,44");
        // resizing in screen mode
        e = openScr(6000);
        drag(&e, 150, 60, 100, 0);
        check(e.scr[kUtLayPad] == 1 && eq(e.rect[kUtLayPad], 202, 25, 320, 44), "the pad at 202 crosses the edge");
        check(utEditHitTest(e, 522, 47).kind == kUtEdHitHandle && utEditHitTest(e, 522, 47).edges == kUtEdgeR,
              "its right edge strip is a handle off the plate");
        check(drag(&e, 522, 47, 2000, 0) && e.scr[kUtLayPad] == 1 && eq(e.rect[kUtLayPad], 202, 25, 549, 44),
              "a resize in screen mode: clamped to the screen (right edge at 751), still on the screen");
        check(drag(&e, 751, 47, -400, 0) && e.scr[kUtLayPad] == 0 && eq(e.rect[kUtLayPad], 202, 25, 149, 44),
              "a resize that brings it wholly inside the plate: window mode");
        check(drag(&e, 351, 47, 400, 0) && e.scr[kUtLayPad] == 0 && eq(e.rect[kUtLayPad], 202, 25, 236, 44),
              "a resize in window mode: clamped to the plate as before, no switch");
        // the label across the top edge
        e = openScr(7000);
        check(drag(&e, 150, 16, 0, -20) && e.scr[kUtLayLabel] == 1 && e.scr[kUtLayPad] == 0 &&
                  eq(e.rect[kUtLayLabel], 105, -12, 317, 16),
              "the label dragged across the top edge: screen mode");
        utScreenRectFormat(utEditScreenKey(e, kUtLayLabel), v, sizeof(v));
        check(strcmp(v, "tr,329,439,317,16") == 0 && e.ok == 1, "its key: tr,329,439,317,16; it holds");
        check(strcmp(utEditScreenHint(e), "on the screen - SAVE writes label_screen") == 0,
              "the status line names label_screen");
        check(eq(e.tool[kUtEdSave], 257, 25, 52, 14), "the label has no toolbar: it stays with the pad");
        utEditScreenGrounds(e, &gp, &gl);
        check(gp.w == 0.0f && gl.x == 101.0f && gl.y == -16.0f && gl.w == 325.0f && gl.h == 24.0f,
              "the label's click ground only");
        utEditSavePlanOf(e, "438x627", &sp);
        check(planHas(sp, "label_screen", "tr,329,439,317,16", 1) && planLacks(sp, "label_438x627") &&
                  planHas(sp, "pad_438x627", nullptr, 0) && planHas(sp, "pad_screen", nullptr, 1),
              "SAVE: label_screen (main ini); the pad's window keys as usual");
        check(drag(&e, 150, -4, 0, 20) && e.scr[kUtLayLabel] == 0 && eq(e.rect[kUtLayLabel], 105, 8, 317, 16),
              "dragged back inside: window mode");
        check(drag(&e, 150, 60, 300, 0) && drag(&e, 150, 16, 0, -20) &&
                  strcmp(utEditScreenHint(e), "on the screen - SAVE writes pad_screen and label_screen") == 0,
              "both on the screen: the status line names both keys");
        // the grid never leaves
        e = openScr(8000);
        check(drag(&e, 200, 300, 1000, 0) && !utEditAnyScreen(e) && e.scr[kUtLayGrid] == 0 &&
                  eq(e.rect[kUtLayGrid], 105, 76, 333, 552),
              "the grid (105,76 333 x 552) dragged past the edge: clamped to the plate, never a screen part");
        check(drag(&e, 438, 300, 1000, 0) && e.scr[kUtLayGrid] == 0 &&
                  e.rect[kUtLayGrid].x + e.rect[kUtLayGrid].w <= 438,
              "the grid resized past the edge: clamped to the plate");
        // a narrow screen pad at the screen's right edge: the toolbar is shifted back on screen
        e = openScr(8500);
        e.rect[kUtLayPad] = UtRect{751 - 55, 100, 55, 44};
        e.scr[kUtLayPad] = 1;
        utEditPlaceToolbar(&e);
        check(e.tool[kUtEdReset].x + kUtEdButtonW == 751 && e.tool[kUtEdSave].x == 751 - 162 &&
                  e.tool[kUtEdSave].y == 100,
              "a narrow pad at the screen's edge: the toolbar is shifted to end on the screen");
        // the screen unknown: nothing leaves the plate until it is known
        {
            Page pv;
            UtEdit u = openVanilla(&pv, 1.0f, 8600);
            check(u.screenKnown == 0 && drag(&u, 150, 60, 300, 0) && u.scr[kUtLayPad] == 0 &&
                      eq(u.rect[kUtLayPad], 118, 25, 320, 44),
                  "screen unknown: the pad is clamped to the plate as before");
            UtEdTick t = tickScr(u, 8700);
            utEditTick(&u, t, &moved);
            check(u.screenKnown == 1 && eq(u.screen, -1169, -451, 1920, 1080),
                  "the first tick with the screen: known");
            check(drag(&u, 150, 60, 300, 0) && u.scr[kUtLayPad] == 1, "then it can leave");
        }
        // opened with a screen pad in force; RESET brings it back
        {
            UtEditOpenIn o = openIn(pg, 1.0f, 3, 9000);
            o.screenKnown = 1;
            o.screenW = SW;
            o.screenH = SH;
            o.originX = OX;
            o.originY = OY;
            o.inForce[kUtLayPad] = UtRect{402, 25, 320, 44};
            o.screenIn[kUtLayPad] = 1;
            o.screenIn[kUtLayGrid] = 1;   // never the grid
            UtEdit n = {};
            check(utEditOpen(&n, o) && n.scr[kUtLayPad] == 1 && n.scr[kUtLayGrid] == 0 &&
                      eq(n.tool[kUtEdSave], 557, 25, 52, 14) && !utEditChanged(n),
                  "opened with pad_screen in force: the pad in screen mode, the toolbar at it, unchanged");
            utEditCheck(&n, pg.in);
            check(utEditLeftDown(&n, 670, 30, 1, 1, 0, 0) == kUtEdActReset && n.scr[kUtLayPad] == 0 &&
                      eq(n.rect[kUtLayPad], 102, 25, 320, 44) && eq(n.tool[kUtEdSave], 257, 25, 52, 14) &&
                      utEditChanged(n),
                  "RESET in screen mode: back in the window at the derived pad, the toolbar with it");
            utEditSavePlanOf(n, "438x627", &sp);
            check(planHas(sp, "pad_screen", nullptr, 1) && planHas(sp, "pad_438x627", nullptr, 0) &&
                      planHas(sp, "label_screen", nullptr, 1),
                  "SAVE after RESET: pad_screen removed (the main ini), pad_438x627 removed");
        }
        // the refusals in screen mode
        e = openScr(9500);
        check(drag(&e, 150, 60, 198, 175) && e.scr[kUtLayPad] == 1 && e.ok == 0 &&
                  e.partWhy[kUtLayPad] == kUtLayoutScreenOverGrid,
              "a screen pad over the grid: refused (would cover the item grid)");
        utEditRefusal(e, kUtLayPad, v, sizeof(v));
        check(strcmp(v, "the pad would cover the item grid") == 0, "  its words: the pad would cover the item grid");
        check(utEditLeftDown(&e, 460, 205, 1, 1, 0, 0) == kUtEdActSaveRefused,
              "  SAVE (with the pad at 300,200) is refused");
        e = openScr(9600);
        e.rect[kUtLayLabel] = UtRect{-1169, -451, 1920, 16};
        e.scr[kUtLayLabel] = 1;
        utEditCheck(&e, pg.in);
        utEditRefusal(e, kUtLayLabel, v, sizeof(v));
        check(e.ok == 0 && e.partWhy[kUtLayLabel] == kUtLayoutScreenLarge &&
                  strcmp(v, "the label is as large as the screen") == 0,
              "a screen label as wide as the screen: refused (is as large as the screen)");
        e = openScr(9700);
        e.rect[kUtLayPad] = UtRect{500, 0, 60, 12};
        e.scr[kUtLayPad] = 1;
        utEditCheck(&e, pg.in);
        utEditRefusal(e, kUtLayPad, v, sizeof(v));
        check(e.ok == 0 && e.partWhy[kUtLayPad] == kUtLayoutPadNoFit &&
                  strcmp(v, "the pad has no room for the buttons with readable captions") == 0,
              "a screen pad too small for its buttons: refused (no room)");
        e = openScr(9800);
        e.rect[kUtLayPad] = UtRect{500, 0, 320, 44};
        e.scr[kUtLayPad] = 1;
        e.rect[kUtLayLabel] = UtRect{520, 10, 200, 16};
        e.scr[kUtLayLabel] = 1;
        utEditCheck(&e, pg.in);
        utEditRefusal(e, kUtLayLabel, v, sizeof(v));
        check(e.ok == 0 && e.partWhy[kUtLayPad] == kUtLayoutOk && e.partWhy[kUtLayLabel] == kUtLayoutOverlap &&
                  strcmp(v, "the label overlaps another part of the page: the pad") == 0,
              "a screen label over the screen pad: the label is refused (overlaps ...: the pad)");
        e.rect[kUtLayLabel] = UtRect{520, 60, 200, 16};
        utEditCheck(&e, pg.in);
        check(e.ok == 1 && utEditRefusal(e, kUtLayLabel, v, sizeof(v)) == 0 && v[0] == 0,
              "moved clear: both hold, no words");
        // every cancel rule in the middle of a screen drag (the polled cursor took it off the plate)
        {
            static const int kRules[7] = {kUtEdWhyGone, kUtEdWhyLayout, kUtEdWhyCover, kUtEdWhyScale,
                                          kUtEdWhyScreen, kUtEdWhyGroup, kUtEdWhyFocus};
            int held = 0, cancelled = 0;
            for (int k = 0; k < 7; ++k) {
                UtEdit c = openScr(10000);
                utEditLeftDown(&c, 150, 60, 1, 1, 0, 0);
                UtEdTick t = tickScr(c, 10100);
                t.physL = 1;
                t.haveCursor = 1;
                t.cx = 450;
                t.cy = 60;
                utEditTick(&c, t, &moved);
                if (c.state == kUtEdDrag && c.scr[kUtLayPad] == 1 && moved) ++held;
                t = tickScr(c, 10200);
                t.physL = 1;
                t.haveCursor = 1;
                t.cx = 460;
                t.cy = 60;
                switch (kRules[k]) {
                    case kUtEdWhyGone: t.now = 10100 + kUtEdGoneMs + 1; break;
                    case kUtEdWhyLayout: t.layoutOk = 0; break;
                    case kUtEdWhyCover: t.coverW = 974; break;
                    case kUtEdWhyScale: t.scale = 0.9f; break;
                    case kUtEdWhyScreen: t.screenW = 2560; break;
                    case kUtEdWhyGroup: t.group = -1; break;
                    default: t.focused = 0; break;
                }
                if (utEditTick(&c, t, &moved) == kUtEdWhyNone) continue;
                if (c.state == kUtEdClosed && c.why == kRules[k] && !utEditParks(c, t.now)) ++cancelled;
            }
            check(held == 7, "the polled cursor carries the pad off the plate: screen mode mid-drag");
            check(cancelled == 7,
                  "gone, layout, cover, UI scale, screen size, materials page, focus: each cancels a "
                  "screen drag");
            check(strcmp(utEditWhyText(kUtEdWhyScreen), "the screen size changed - nothing was written") == 0,
                  "the screen-size words");
        }
        // the MOVEs the screen input hands over during a press follow the drag as the poll does
        e = openScr(11000);
        utEditLeftDown(&e, 150, 60, 1, 1, 0, 0);
        check(utEditDragTo(&e, 450, 60) && e.scr[kUtLayPad] == 1 && !utEditDragTo(&e, 450, 60) &&
                  utEditLeftUp(&e, 451, 60) && eq(e.rect[kUtLayPad], 403, 25, 320, 44),
              "a MOVE outside the window drags (a repeat does nothing); the LEFT UP ends it there");
        // a scale under 1: the key keeps the record size, the round trip within a record px
        {
            Page p7;
            vanilla(&p7, 0.7f);
            UtEditOpenIn o = openIn(p7, 0.7f, 3, 12000);
            o.screenKnown = 1;
            o.screenW = 3440;
            o.screenH = 1440;
            o.originX = 297.0f;
            o.originY = 195.6f;
            UtEdit n = {};
            utEditOpen(&n, o);
            const float lx = floorf(150 * 0.7f), ly = floorf(60 * 0.7f);
            utEditLeftDown(&n, lx, ly, 1, 1, 0, 0);
            utEditLeftUp(&n, lx + 500.0f, ly);
            utEditCheck(&n, p7.in);
            const UtScreenRect k = utEditScreenKey(n, kUtLayPad);
            const UtRect back = utEditResolvedRect(n, kUtLayPad);
            const int ddx = back.x - n.rect[kUtLayPad].x, ddy = back.y - n.rect[kUtLayPad].y;
            check(n.scr[kUtLayPad] == 1 && k.w == 320 && k.h == 44 && ddx >= -1 && ddx <= 1 &&
                      ddy >= -1 && ddy <= 1,
                  "3440 x 1440 at 0.7: the key keeps 320 x 44, the round trip within a record px");
        }
    }

    // ---------------------------------------------------------------------------------------
    printf("10. CANCEL in every state, a click during a drag\n");
    {
        // The bug this pins: the save/reset/cancel buttons took no click, and Esc was the only way
        // out. CANCEL answers open, mid-drag and saving, with or without an item on
        // the cursor; a LEFT DOWN during a drag whose release was lost ends that drag first.
        const float s = 0.878f;
        Page pg;
        const auto at = [&](const UtEdit& e, int b, float* x, float* y) {
            *x = ((float)e.tool[b].x + 2.0f) * s;
            *y = ((float)e.tool[b].y + 2.0f) * s;
        };
        float cx = 0, cy = 0, sx = 0, sy = 0, rx = 0, ry = 0;
        UtEdit e = openVanilla(&pg, s, 1000);
        at(e, kUtEdCancel, &cx, &cy);
        at(e, kUtEdSave, &sx, &sy);
        at(e, kUtEdReset, &rx, &ry);
        check(utEditToolbarAt(e, cx, cy) == kUtEdCancel && utEditToolbarAt(e, sx, sy) == kUtEdSave &&
                  utEditToolbarAt(e, rx, ry) == kUtEdReset && utEditToolbarAt(e, -50, -50) == -1,
              "utEditToolbarAt: the three buttons, and nothing off them");
        check(utEditLeftDown(&e, sx, sy, 0, 1, 0, 0) == kUtEdActPass &&
                  utEditLeftDown(&e, rx, ry, 0, 1, 0, 0) == kUtEdActPass && e.state == kUtEdOpen,
              "an item on the cursor: SAVE and RESET are still refused (passed)");
        check(utEditLeftDown(&e, cx, cy, 0, 1, 0, 0) == kUtEdActCancel && e.state == kUtEdClosed &&
                  e.why == kUtEdWhyCancel,
              "an item on the cursor: CANCEL closes (the click is the editor's, the item stays)");
        check(utEditToolbarAt(e, cx, cy) == -1 &&
                  utEditLeftDown(&e, cx, cy, 1, 1, 0, 0) == kUtEdActPass,
              "closed: no toolbar, nothing consumed");

        e = openVanilla(&pg, s, 1000);
        const UtRect pad0 = e.rect[kUtLayPad];
        const float px = ((float)pad0.x + (float)pad0.w * 0.25f) * s;
        const float py = ((float)pad0.y + (float)pad0.h - 4.0f) * s;
        const UtEdHit hp = utEditHitTest(e, px, py);
        check(hp.part == kUtLayPad && utEditLeftDown(&e, px, py, 1, 1, 0, 0) == kUtEdActDrag &&
                  e.state == kUtEdDrag,
              "a drag on the pad");
        check(utEditHitTest(e, cx, cy).kind == kUtEdHitNone &&
                  utEditToolbarAt(e, cx, cy) == kUtEdCancel,
              "mid-drag the hit test answers nothing, the toolbar lookup still finds CANCEL");
        check(utEditLeftDown(&e, cx, cy, 1, 1, 0, 0) == kUtEdActCancel && e.state == kUtEdClosed &&
                  e.why == kUtEdWhyCancel && e.part == -1 && e.button == 0,
              "mid-drag (its release lost, the poll not run yet): CANCEL closes, nothing written");

        e = openVanilla(&pg, s, 1000);
        utEditLeftDown(&e, px, py, 1, 1, 0, 0);
        UtEdTick t = tickAt(e, 1020);
        t.physL = 1;
        t.haveCursor = 1;
        t.cx = px;
        t.cy = py + 30.0f;
        int moved = 0;
        utEditTick(&e, t, &moved);
        const UtRect afterDrag = e.rect[kUtLayPad];
        check(e.state == kUtEdDrag && moved && !utRectEq(afterDrag, pad0), "the drag moved the pad");
        at(e, kUtEdCancel, &cx, &cy);
        at(e, kUtEdReset, &rx, &ry);
        const int a = utEditLeftDown(&e, rx, ry, 1, 1, 0, 0);
        check(a == kUtEdActReset && e.state == kUtEdOpen && utRectEq(e.rect[kUtLayPad], e.def[kUtLayPad]),
              "mid-drag, a click on RESET: the drag ends, RESET answers (was: passed - the toolbar "
              "dead until the poll ran)");

        e = openVanilla(&pg, s, 1000);
        utEditSaved(&e, 1, 1100);
        at(e, kUtEdCancel, &cx, &cy);
        at(e, kUtEdSave, &sx, &sy);
        check(e.state == kUtEdSaving && utEditLeftDown(&e, sx, sy, 1, 1, 0, 0) == kUtEdActPass,
              "saving: SAVE answers nothing (passed)");
        check(utEditLeftDown(&e, cx, cy, 1, 1, 0, 0) == kUtEdActCancel && e.state == kUtEdClosed &&
                  e.why == kUtEdWhySaveSlow,
              "saving: CANCEL stops the wait - closed \"saved; not read back yet\" (the file IS "
              "written, so never \"nothing was written\")");
    }


    printf("11. the editor's border, its grounds and the window route's proof\n");
    {
        Page pg;
        const float s = 0.878f;
        UtEdit e = openVanilla(&pg, s, 1000);
        const UtRectF w = utEditWindowClient(e);
        const UtRectF ps = utScaledRectF(e.plate, s);
        check(eq(e.plate, 0, 1, 438, 627) && w.x == ps.x && w.y == ps.y && w.w == ps.w && w.h == ps.h &&
                  fabsf(w.w - 438.0f * s) < 0.75f && fabsf(w.h - 627.0f * s) < 0.75f,
              "the border is the plate (vanilla 0,1,438,627), window-local screen px at 0.878");
        UtRectF gpad, glabel;
        utEditScreenGrounds(e, &gpad, &glabel);
        check(!(gpad.w > 0.0f) && !(glabel.w > 0.0f),
              "every part in the window: no ground published (the window's slot takes every click)");
        // a window-mode pad whose toolbar left the plate (never made by utEditPlaceToolbar - forced)
        UtEdit f = e;
        for (int k = 0; k < kUtEdButtons; ++k) f.tool[k].y = -20;
        utEditScreenGrounds(f, &gpad, &glabel);
        check(gpad.w > 0.0f && gpad.y < 0.0f && gpad.h < 40.0f,
              "  a window pad's toolbar off the plate: the toolbar alone is published");
        // the screen pad: its ground with the toolbar, as before
        UtEditOpenIn in = openIn(pg, s, 3, 1000);
        in.inForce[kUtLayPad] = UtRect{-120, -251, 735, 79};
        in.screenIn[kUtLayPad] = 1;
        in.screenKnown = 1;
        in.screenW = 1600;
        in.screenH = 900;
        in.originX = 317.0f;
        in.originY = 275.2f;
        UtEdit g = {};
        check(utEditOpen(&g, in) && g.scr[kUtLayPad] == 1, "the pad pinned (-120,-251,735,79) opens in screen mode");
        utEditScreenGrounds(g, &gpad, &glabel);
        const UtRectF gw = utEditWindowClient(g);
        check(gpad.w > 0.0f && gpad.y + gpad.h <= gw.y + 1.0f,
              "  its ground (pad + toolbar) is published, wholly above the window here");
        UtEdit c = g;
        utEditClose(&c, kUtEdWhyCancel);
        const UtRectF cw = utEditWindowClient(c);
        check(!(cw.w > 0.0f), "  closed: no border");

        // the note
        UtEdPumpNote n = {};
        float dx = 9.0f, dy = 9.0f;
        check(!utEditNoteProves(&n, 10.0f, 10.0f, 1000), "no note: nothing is proven");
        utEditNoteDown(&n, 129.0f, 41.7f, 1000);
        check(!utEditNoteProves(&n, 132.5f, 41.7f, 1010, &dx, &dy) && n.valid && fabsf(dx - 3.5f) < 0.01f,
              "  3.5 px off: refused, the note kept (the delta said)");
        check(!utEditNoteProves(&n, 129.0f, 41.7f, 1000 + kUtEdNoteMs + 1), "  older than kUtEdNoteMs: refused");
        check(utEditNoteProves(&n, 130.9f, 39.8f, 1000 + kUtEdNoteMs) && !n.valid,
              "  within 2 px and kUtEdNoteMs: proven, the note used up");
        check(!utEditNoteProves(&n, 130.9f, 39.8f, 1001), "  used up: the same point again is not proven by it");
        const float nan = sqrtf(-1.0f);
        utEditNoteDown(&n, 129.0f, 41.7f, 1000);
        check(!utEditNoteProves(&n, nan, 41.7f, 1000) && !utEditNoteProves(&n, 129.0f, nan, 1000),
              "  NaN never matches");

        // A recorded campaign click: the pad back in the window (62,12,345,57), the
        // editor told no proof by the window route. A click on its body: refused without a proof
        // (inputOk 0, the state the bug left), accepted with the note's proof.
        UtEdit h = g;
        h.rect[kUtLayPad] = UtRect{62, 12, 345, 57};
        h.scr[kUtLayPad] = 0;
        utEditPlaceToolbar(&h);
        UtEdit h8 = h;
        const float bx = 150.0f * s, by = 50.0f * s;
        check(utEditLeftDown(&h8, bx, by, 0, 1, 0, 0) == kUtEdActPass && h8.state == kUtEdOpen,
              "the pad back in the window, its body clicked unproven: passed (no drag)");
        float tx = ((float)h8.tool[kUtEdSave].x + 3.0f) * s, ty = ((float)h8.tool[kUtEdSave].y + 3.0f) * s;
        check(utEditLeftDown(&h8, tx, ty, 0, 1, 0, 0) == kUtEdActPass, "  SAVE unproven: passed");
        UtEdPumpNote m = {};
        utEditNoteDown(&m, bx, by, 5000);   // the screen input passed that DOWN and noted it
        const int ok = utEditNoteProves(&m, bx, by, 5000) ? 1 : 0;
        check(ok == 1 && utEditLeftDown(&h, bx, by, ok, 1, 0, 0) == kUtEdActDrag && h.part == kUtLayPad,
              "the same click proven by the note: the pad's drag starts");
        check(utEditLeftUp(&h, bx + 20.0f * s, by + 30.0f * s) && eq(h.rect[kUtLayPad], 82, 42, 345, 57),
              "  released 20,30 record px further: 82,42,345,57, still in the window");
        tx = ((float)h.tool[kUtEdSave].x + 3.0f) * s;
        ty = ((float)h.tool[kUtEdSave].y + 3.0f) * s;
        const int a = utEditLeftDown(&h, tx, ty, 1, 1, 0, 0);
        check(a == kUtEdActSave || a == kUtEdActSaveRefused, "  SAVE on its toolbar in the window answers");
    }

    printf("%d checks, %d failed - %s\n", g_checks, g_fails,
           g_fails ? "LAYOUT EDITOR TESTS FAILED" : "LAYOUT EDITOR TESTS PASSED");
    return g_fails ? 1 : 0;
}
