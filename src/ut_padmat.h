// ut_padmat.h - the pad's band, the overlap test and the pad's click rule, kept pure so the
// offline harness can prove them.
//
// THE BAND is the joint free rectangle of the vanilla cover and the mod's own plates, record px:
// x 102..422, y 25..69 (ut_panel.cpp measures it). The derived pad is either wholly inside it or
// not drawn.
//
// THE MATERIALS PAGE (button MAT) shows the game's own reagent boxes on the game's own cover. A
// mod can put real boxes under the pad (Reign of Terror: 116 boxes, 15 of them at y 13..15). The
// pad is drawn WHERE IT IS there (the user's padmat_ rectangle when that is set and usable, else
// the pad in force - ut_layout.h utMatPadResolve) over whatever boxes lie under it, and a point
// inside it is the pad's: utPadLeftDown keeps a LEFT DOWN there from the engine (a button acts,
// the rest of the pad does nothing), so a covered box never sees the click.
#pragma once

namespace ut {

const int kUtPadBandX0 = 102;
const int kUtPadBandX1 = 422;
const int kUtPadBandY0 = 25;
const int kUtPadBandY1 = 69;

struct UtRectF {
    float x, y, w, h;
};

inline bool utRectsOverlap(const UtRectF& a, const UtRectF& b) {
    if (!(a.w > 0.0f && a.h > 0.0f && b.w > 0.0f && b.h > 0.0f)) return false;
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

inline bool utAnyOverlap(const UtRectF& r, const UtRectF* boxes, int n) {
    for (int i = 0; i < n; ++i)
        if (utRectsOverlap(r, boxes[i])) return true;
    return false;
}

// The first cell that meets no box, or -1 when every cell meets one. The materials-page rule
// does not use it to place a button; test_rowfold section 4 keeps its arithmetic checked.
inline int utPadReturnCell(const UtRectF* cells, int nCells, const UtRectF* boxes, int nBoxes) {
    for (int c = 0; c < nCells; ++c)
        if (!utAnyOverlap(cells[c], boxes, nBoxes)) return c;
    return -1;
}

// A box record's top-left (itemBoxX/Y, record px): true when no box of that size or larger can
// reach the band, i.e. the box starts below its last row. Used on the box RECORDS of a view,
// where the drawn size is not known yet.
inline bool utBoxClearOfBand(int boxY) { return boxY > kUtPadBandY1; }

inline bool utRectFHas(const UtRectF& r, float x, float y) {
    return r.w > 0.0f && r.h > 0.0f && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

// ---- THE PAD'S CLICK: what ut_plate.cpp's mouse slot +0x38 does with a LEFT DOWN -------------
// A LEFT DOWN (type 1) is the only event the mod ever keeps from the engine's page handler; every
// other type reaches it untouched (a MOVE arrives every input tick). Why a click is passed on, in
// ut_plate.cpp's PassReason order (it static_asserts the values):
enum UtPadPass {
    kUtPadPassNone = 0,
    kUtPadPassNotDrawn,      // the pad was not painted in the last few frames
    kUtPadPassCursorItem,    // an item is on the cursor: the engine's deposit, never the pad's
    kUtPadPassCursorFault,   // the cursor read faulted: an item is assumed
    kUtPadPassNoButton,      // a collection page: in the band, on no button
    kUtPadPassInBox,         // a collection page: inside a live box's own hit rect
    kUtPadPassBoxFault,      // a collection page: the box hit test faulted
    kUtPadPassNotProven      // the event space is not proven for this world yet
};

enum UtPadClickAct {
    kUtPadClickPass = 0,   // the engine's handler runs (`reason` says why)
    kUtPadClickButton,     // the button acts; the engine's handler does not run
    kUtPadClickKeep        // nothing acts and the engine's handler does not run either
};

struct UtPadClick {
    int act;      // UtPadClickAct
    int reason;   // UtPadPass, for kUtPadClickPass
};

// `matPad` = the game's own materials page AND the point lies inside the pad drawn there (its
// whole rectangle, the ground between the buttons included). There the pad covers the page's
// boxes, so once the pad is on screen, the cursor is empty and the event space is proven, the
// click is the pad's whether or not a box lies under it: a button acts, a point between the
// buttons is kept. An item on the cursor is never the pad's, on any page (kUtPadPassCursorItem:
// the engine's deposit - PrimaryReagentActivate takes a click anywhere in the window and consults
// no box). A collection page keeps its order exactly: drawn, cursor, button, box, proven.
inline UtPadClick utPadLeftDown(int drawn, int cursorRead, int cursorItem, int onButton,
                                int matPad, int inBox, int boxFault, int proven) {
    UtPadClick c = {kUtPadClickPass, kUtPadPassNone};
    if (!drawn) c.reason = kUtPadPassNotDrawn;
    else if (!cursorRead) c.reason = kUtPadPassCursorFault;
    else if (cursorItem) c.reason = kUtPadPassCursorItem;
    else if (matPad) {
        if (!proven) c.reason = kUtPadPassNotProven;
        else c.act = onButton ? kUtPadClickButton : kUtPadClickKeep;
    } else if (!onButton) c.reason = kUtPadPassNoButton;
    else if (boxFault) c.reason = kUtPadPassBoxFault;
    else if (inBox) c.reason = kUtPadPassInBox;
    else if (!proven) c.reason = kUtPadPassNotProven;
    else c.act = kUtPadClickButton;
    return c;
}

// A button whose action is then refused (an index the table does not know, or the pager refusing
// the group): a collection page passes the click on after all; on the materials page it stays
// the pad's - the box under the pad never sees it.
inline int utPadRefusedAct(int matPad) { return matPad ? kUtPadClickKeep : kUtPadClickPass; }

}  // namespace ut
