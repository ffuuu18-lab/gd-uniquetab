// ut_padinput.h - the SCREEN pad's input: the press rules over the engine's mouse-event stream.
//
// WHY THIS EXISTS. A pad (or label) drawn OUTSIDE the caravan window cannot be clicked through the
// ReagentWindow's mouse slot: CaravanWindow's own slot +0x38 rect-tests itself first and returns
// false for every point outside the caravan, so the page never sees it. Every mouse event of the
// game passes ONE exported function first - DirectInput.dll's
// `DirectInputDevice::GetMouseEvent` (the device's vtable slot +0x38, the only copy-out the
// engine's `Engine::ProcessUserInput` pump makes) - before the UI root, the HUD, the windows or
// the WORLD VIEW see it. ut_plate.cpp detours that export, and this header decides, per event,
// what the detour does to the 0x1C-byte copy it hands back:
//
//   untouched   the default: outside the published screen rectangles, before anything was
//               published, when the rectangles are stale, before the event space is proven;
//   REWRITTEN   type -> MOVE (0) and the held-button bytes [0x10..0x11] cleared: the click is
//               consumed for every consumer at once (world, HUD, minimap, other windows) while
//               hover and tooltips keep working - a MOVE is what arrives every tick anyway;
//   CLEARED     the held-button bytes [0x10..0x11] cleared, the type kept: every event passed on
//               DURING a press. The world view (exe 0x21010) acts on those bytes on EVERY event
//               type, not only on MOVE, so clearing them only on MOVE would let a held pad press
//               start a "hold to move".
//
// THE RULES (tools\test_padinput.cpp proves each one):
//   * LEFT DOWN inside a shown button -> rewritten, the button's action (MAT: the game's own
//     materials page / select its group / toggle OWN / NEXT: the next pad page - the in-window
//     pad's own actions; utPadButtonKind names which), a press starts;
//   * LEFT DOWN on the pad's ground or the label (no button) -> rewritten, no action, a press;
//   * LEFT DOWN with an item on the cursor (or the cursor unreadable) -> rewritten, NO action, a
//     press. THE ITEM-ON-CURSOR RULE: the safe one. A passed click outside the caravan would drop
//     the item on the ground, so the click is eaten and the item stays on the cursor;
//   * while a press lasts every event passed on has its button bytes cleared, whatever its type
//     and wherever it is (the rectangles going stale mid-press do not end it);
//   * the press's own UP (LEFT UP, or RIGHT UP for a Ctrl+right press) is rewritten and ends it;
//   * THE POLLED RELEASE ends a press whose UP never came: once the polled button has been up for
//     two whole Update generations (an UP queued in the generation the poll first saw up, or in
//     the next, still ends it normally). "400 ms without events" cannot fire - the device's
//     Update appends a MOVE every tick - so the poll is the fallback;
//   * a second LEFT DOWN while a left press is still open (its UP was lost) ends the old press and
//     is judged on its own;
//   * Ctrl+RIGHT DOWN inside the pad or the label -> handed to the layout editor's entry (the one
//     the in-window Ctrl+right-click reaches), rewritten, and its RIGHT UP rewritten too; a plain
//     RIGHT DOWN inside them is consumed too, with no action (the player's right-click skill
//     must not fire under the pad); the wheel passes;
//   * while the layout editor is OPEN: LEFT DOWN inside the rectangles is handed to the editor
//     (not to a button) and starts an editor press, during which every MOVE and the LEFT UP are
//     handed to the editor too; a MOVE or a stray LEFT UP inside the rectangles with no press is
//     handed over untouched (the editor's own LEFT UP is never consumed, like in the window);
//     with an item on the cursor the LEFT DOWN is still the editor's (consumed: the item stays;
//     the editor then acts only on CANCEL, which is clickable in every state);
//   * THE ADOPTED DRAG: the editor is mid-drag and no press is open here - a drag that
//     STARTED IN THE WINDOW (its LEFT DOWN was the caravan's) and is carried across the window's
//     edge by the polled cursor. It becomes this machine's editor press at its first event, so the
//     world never sees a held byte of it (every event cleared, wherever it is - a held MOVE over
//     the world would reach the world view, which acts on the held bytes of every event: a "hold
//     to move" under an editor drag), its MOVEs and its LEFT UP are handed to the editor wherever
//     they land (handed over only inside the published grounds, a release over the world would end
//     the drag by the poll alone and the UP itself would go to the world), and its LEFT UP is
//     CLEARED, not rewritten (the caravan saw that drag's DOWN, so it sees its UP);
//   * a LEFT DOWN while a RIGHT press is open (its RIGHT UP lost, the poll not yet run) ends the
//     right press and is judged on its own (swallowed as a held event instead, the first click on
//     the toolbar after a Ctrl+right-click that opened the editor from the screen would vanish);
//     so is any DOWN on which the polled release ends a press;
//   * THE WINDOW IS THE WINDOW'S: while the layout editor is open it publishes the
//     caravan window's client rect (`window`) with its grounds. A point inside it is the window's
//     own: no press STARTS there and nothing idle is handed over - the event is untouched and the
//     ReagentWindow's mouse slot hands it to the editor, as the in-window editor does. A
//     LEFT DOWN there is only NOTED to the editor (kUtPadActEditorNote, untouched): the pump's copy
//     of that very click, in the proven client space, is how the window route proves its own space
//     in a world where the in-window pad never drew (the pad pinned to the screen: the in-window
//     probe never runs, and without the note every window click of the editor but CANCEL would be
//     refused - a panel moved wholly inside the caravan could no longer be controlled).
//     An ADOPTED drag (begun in the window) leaves its events inside the window untouched too -
//     its UP there is the caravan's and ends the drag through the window -
//     and keeps clearing / handing over everything outside. A press that began on the SCREEN runs
//     to its UP wherever it goes (the caravan never saw its DOWN);
//   * outside the rectangles, and with no press, NOTHING is touched;
//   * IDEMPOTENT per Update generation: the same (generation, index) seen twice with the same
//     event gets the same rewrite again and NO second action - a re-read can never toggle OWN
//     twice; a DIFFERENT event under the same key is decided as new (a stuck counter can never
//     make an old decision eat a new click).
//
// THE RECTANGLES are published by the draw that painted them (`plateScreenRectsPublish`):
// client pixels - the event's own space (measured: GetCursorPos + ScreenToClient; the canvas the
// mod draws on is the same screen space) - and the frame they were drawn at. Nothing published,
// `shown == 0`, or older than kUtPadScreenStaleTicks frames = INERT.
//
// Pure: standard headers only, no Windows, no mod state; ut_plate.cpp runs it from the detour
// and tools\test_padinput.cpp runs the same code offline.
#pragma once

#include <stddef.h>
#include <string.h>

namespace ut {

// ---- the engine's InputDevice::MouseEvent (0x1C bytes, decoded from the input pump) -----------
const size_t kUtMouseEventSize = 0x1C;
const size_t kUtMouseEvtTypeOff = 0x00;     // int
const size_t kUtMouseEvtXOff = 0x04;        // float, client px
const size_t kUtMouseEvtYOff = 0x08;        // float, client px
const size_t kUtMouseEvtHeldOff = 0x10;     // u8[8], held buttons: [0] left, [1] right
const size_t kUtMouseEvtHeldCleared = 2;    // the two bytes the world view reads (0x10, 0x11)

const int kUtEvtMove = 0;
const int kUtEvtLeftDown = 1;
const int kUtEvtRightDown = 2;
const int kUtEvtLeftUp = 9;
const int kUtEvtRightUp = 0xA;
const int kUtEvtWheelUp = 0x11;
const int kUtEvtWheelDown = 0x12;

// The stride GetMouseEvent copies with, decoded from its own bytes: `imul r8, rax, imm8`
// (4C 6B C0 ib) in the first 16 bytes (1.3.0.8: `49 63 C0 4C 6B C0 1C ...`). The detour rewrites
// a kUtMouseEventSize-byte record, so the mod requires this to be exactly 0x1C. 0 = not found.
inline int utPadDecodeEventStride(const unsigned char* p, size_t n) {
    if (!p) return 0;
    const size_t lim = n < 16 ? n : 16;
    for (size_t i = 0; i + 4 <= lim; ++i) {
        if (p[i] == 0x4C && p[i + 1] == 0x6B && p[i + 2] == 0xC0) return (int)p[i + 3];
    }
    return 0;
}

// ---- the published rectangles (the screen draw fills them, ut_plate.cpp stamps the frame) -----
const int kUtPadScreenMaxButtons = 128;
// A frame later than this many PresentSurface frames after the draw = stale = inert.
const unsigned long long kUtPadScreenStaleTicks = 4;

struct UtPadScreenRect {
    float x, y, w, h;   // client px; w or h <= 0 = absent
};
struct UtPadScreenButton {
    UtPadScreenRect r;
    int index;          // the pad's LOGICAL button: what panelButtonHit returns for the in-window
                        // pad (ut_padpage.h: MAT, one per G line of the page, OWN, NEXT) - the
                        // detour hands it to panelButtonTarget exactly as hk_WndMouse does
};
struct UtPadScreenRects {
    int shown;                 // 1 = a screen pad and/or label was painted this frame
    unsigned long long tick;   // the frame it was painted at (hookFrameCount); 0 = never
    UtPadScreenRect pad;          // the pad's ground
    UtPadScreenRect label;        // the label's ground
    UtPadScreenRect window;       // the open layout editor only: the caravan window's
                                  // client rect - a point inside it is the window's (the screen
                                  // pad's own publication leaves it 0 x 0 = none)
    int buttonCount;           // 0..kUtPadScreenMaxButtons
    UtPadScreenButton buttons[kUtPadScreenMaxButtons];
};

inline bool utPadScreenRectValid(const UtPadScreenRect& r) { return r.w > 0.0f && r.h > 0.0f; }

// NaN coordinates compare false everywhere, so a NaN point is inside nothing.
inline bool utPadScreenIn(const UtPadScreenRect& r, float x, float y) {
    return utPadScreenRectValid(r) && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

inline bool utPadScreenRectsFresh(const UtPadScreenRects& r, unsigned long long now) {
    if (!r.shown || r.tick == 0 || now < r.tick || now - r.tick > kUtPadScreenStaleTicks) return false;
    return utPadScreenRectValid(r.pad) || utPadScreenRectValid(r.label);
}

// The logical button under (x, y), or -1. Buttons count only inside the pad's ground (a button
// published outside it is a publisher's mistake and is never hit).
inline int utPadScreenButtonAt(const UtPadScreenRects& r, float x, float y) {
    if (!utPadScreenIn(r.pad, x, y)) return -1;
    const int n = r.buttonCount < 0 ? 0
                  : r.buttonCount > kUtPadScreenMaxButtons ? kUtPadScreenMaxButtons
                                                           : r.buttonCount;
    for (int i = 0; i < n; ++i) {
        if (r.buttons[i].index >= 0 && utPadScreenIn(r.buttons[i].r, x, y)) return r.buttons[i].index;
    }
    return -1;
}

inline bool utPadScreenInAny(const UtPadScreenRects& r, float x, float y) {
    return utPadScreenIn(r.pad, x, y) || utPadScreenIn(r.label, x, y);
}

// The open editor's window rect is fresh: published (shown), with a window, at most
// kUtPadScreenStaleTicks frames old. It needs no pad or label (every part in the window).
inline bool utPadScreenWindowFresh(const UtPadScreenRects& r, unsigned long long now) {
    if (!r.shown || r.tick == 0 || now < r.tick || now - r.tick > kUtPadScreenStaleTicks) return false;
    return utPadScreenRectValid(r.window);
}

// ---- what a logical button does (ut_padpage.h's numbering) -------------------------------------
// 0 = MAT (the game's own materials page), 1..G = a button of groups, G + 1 = OWN, G + 2 = NEXT
// (only while the pad is paged). `buttons` = G, the pad's buttons between MAT and OWN. The screen
// pad's click (ut_plate scrButton) and the in-window pad's map MAT through this - the group
// lookup has no group for logical 0 (looked up as a group, a MAT click on the screen pad would be
// "a button that maps to no group" and do nothing).
enum UtPadButtonKind {
    kUtPadKindNone = 0,   // no such logical button
    kUtPadKindMat,
    kUtPadKindGroup,
    kUtPadKindOwn,
    kUtPadKindNext
};

inline int utPadButtonKind(int index, int buttons, int paged) {
    if (buttons < 0) buttons = 0;
    if (index == 0) return kUtPadKindMat;
    if (index >= 1 && index <= buttons) return kUtPadKindGroup;
    if (index == buttons + 1) return kUtPadKindOwn;
    if (index == buttons + 2 && paged) return kUtPadKindNext;
    return kUtPadKindNone;
}

// ---- the press machine -----------------------------------------------------------------------
enum UtPadPress {
    kUtPressNone = 0,
    kUtPressButton = 1,   // LEFT DOWN on a button: its action ran
    kUtPressKept = 2,     // LEFT DOWN on the ground / the label / with an item: consumed, no action
    kUtPressEditor = 3,   // LEFT DOWN handed to the open layout editor: its drag
    kUtPressRight = 4,    // Ctrl+RIGHT DOWN: the editor's entry
};

enum UtPadAct {
    kUtPadActNone = 0,
    kUtPadActButton = 1,   // run the button `button`'s action (once per press)
    kUtPadActEditor = 2,   // hand `editorType` at the event's point to the layout editor
    kUtPadActEditorNote = 3,   // the event stays untouched (the window's); the editor is
                               // only TOLD the pump's copy of this LEFT DOWN (kUtEvtEditorNote) -
                               // the window's mouse slot gets the same click next and proves its
                               // space against it
};

// The type scrEditor hands panelLayoutEditMouse for kUtPadActEditorNote - not an engine type.
const int kUtEvtEditorNote = -1;

enum UtPadWhy {
    kUtPadWhyUntouched = 0,   // outside, or an event nothing applies to
    kUtPadWhyInert,           // nothing fresh published, or the event space not proven
    kUtPadWhyButton,          // LEFT DOWN on a button
    kUtPadWhyGround,          // LEFT DOWN on the pad's ground or the label
    kUtPadWhyCursorItem,      // a DOWN with an item on the cursor
    kUtPadWhyEditor,          // handed to the layout editor
    kUtPadWhyCtrlRight,       // Ctrl+RIGHT DOWN: the editor's entry
    kUtPadWhyHeld,            // an event passed on during a press: bytes cleared
    kUtPadWhyUp,              // the press's own UP
    kUtPadWhyWindow,          // inside the open editor's window rect - the window's own
    kUtPadWhyCount
};

struct UtPadInEvent {
    int type;
    float x, y;   // client px
};

// What the caller knows about the world right now. Every field is 0/1.
struct UtPadInGates {
    int fresh;        // utPadScreenRectsFresh(published, now)
    int proven;       // the event space is proven (a probe agreed with GetCursorPos)
    int cursorItem;   // an item is on the cursor, or the read failed (unknown = an item)
    int ctrl;         // Ctrl is down
    int editorOpen;   // the layout editor is open
    int leftUp;       // POLLED: the logical left button is up now (swapped buttons honoured)
    int rightUp;      // POLLED: the logical right button is up now
    int editorDrag;   // the layout editor is mid-drag (a drag that started in the window too)
    int windowFresh;  // utPadScreenWindowFresh(published, now) and the caravan open
};

struct UtPadInOut {
    int rewrite;      // type -> MOVE, held bytes cleared: the event is consumed
    int clear;        // held bytes cleared, type kept: an event passed on during a press
    int act;          // UtPadAct
    int button;       // the logical button for kUtPadActButton, else -1
    int editorType;   // the event type handed over for kUtPadActEditor
    int why;          // UtPadWhy
    int ended;        // a press ended here: 1 = its UP, 2 = the polled release, 3 = a new DOWN
    int repeat;       // 1 = this (generation, index) was already decided: same bytes, no action
};

struct UtPadInState {
    int press;                    // UtPadPress
    int adopted;                  // 1 = the editor press is an ADOPTED window drag (its UP cleared)
    int pressButton;              // the logical button of a kUtPressButton press, else -1
    unsigned long long upSince;   // the generation the poll first saw the button up (+1), 0 = no
    int have;                     // a (generation, index) has been decided
    unsigned long long gen;
    int idx;
    UtPadInEvent lastEvt;         // the event it was decided on (a repeat is the same bytes)
    UtPadInOut last;              // its decision, replayed on a repeat
};

inline void utPadInReset(UtPadInState* s) {
    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->pressButton = -1;
    s->last.button = -1;
}

// The polled release needs this many generations of "up" with no UP event before it ends a press.
const unsigned long long kUtPadPollGenerations = 2;

namespace padinput_detail {

inline UtPadInOut fresh() {
    UtPadInOut o;
    memset(&o, 0, sizeof(o));
    o.button = -1;
    return o;
}

// No press is open: the rules for an event that may start one.
inline bool inEditorWindow(const UtPadScreenRects& r, const UtPadInGates& g, float x, float y) {
    return g.windowFresh && g.editorOpen && utPadScreenIn(r.window, x, y);
}

inline void decideIdle(UtPadInState* s, const UtPadInEvent& e, const UtPadScreenRects& r,
                       const UtPadInGates& g, UtPadInOut* o) {
    if (inEditorWindow(r, g, e.x, e.y)) {
        // The window's own - untouched, the window's mouse slot hands it to the editor.
        // A LEFT DOWN is noted to the editor (the window route's proof), never consumed.
        o->why = kUtPadWhyWindow;
        if (e.type == kUtEvtLeftDown && g.proven) {
            o->act = kUtPadActEditorNote;
            o->editorType = kUtEvtLeftDown;
        }
        return;
    }
    if (!g.fresh || !g.proven) {
        o->why = kUtPadWhyInert;
        return;
    }
    if (!utPadScreenInAny(r, e.x, e.y)) return;   // outside: untouched
    switch (e.type) {
        case kUtEvtLeftDown:
            o->rewrite = 1;
            if (g.editorOpen) {
                // The editor's, item or not: consumed either way (the item stays on the cursor),
                // and the editor - told the cursor holds an item - acts on CANCEL only.
                s->press = kUtPressEditor;
                o->act = kUtPadActEditor;
                o->editorType = kUtEvtLeftDown;
                o->why = kUtPadWhyEditor;
            } else if (g.cursorItem) {
                s->press = kUtPressKept;
                o->why = kUtPadWhyCursorItem;
            } else {
                const int b = utPadScreenButtonAt(r, e.x, e.y);
                if (b >= 0) {
                    s->press = kUtPressButton;
                    s->pressButton = b;
                    o->act = kUtPadActButton;
                    o->button = b;
                    o->why = kUtPadWhyButton;
                } else {
                    s->press = kUtPressKept;
                    o->why = kUtPadWhyGround;
                }
            }
            s->upSince = 0;
            return;
        case kUtEvtRightDown:
            o->rewrite = 1;
            s->press = kUtPressRight;
            s->upSince = 0;
            if (!g.ctrl) {
                // A PLAIN right click on the pad or the label: consumed with no
                // action, so the player's right-click skill never fires at a point under the
                // pad; its RIGHT UP is rewritten by the right press like the Ctrl one's.
                o->why = kUtPadWhyGround;
                return;
            }
            if (g.cursorItem) {
                o->why = kUtPadWhyCursorItem;
            } else if (g.editorOpen) {
                o->why = kUtPadWhyGround;   // already open: nothing to enter
            } else {
                o->act = kUtPadActEditor;
                o->editorType = kUtEvtRightDown;
                o->why = kUtPadWhyCtrlRight;
            }
            return;
        case kUtEvtLeftUp:
        case kUtEvtMove:
            // The editor's drag reads its own MOVE / LEFT UP; neither is consumed.
            if (g.editorOpen) {
                o->act = kUtPadActEditor;
                o->editorType = e.type;
                o->why = kUtPadWhyEditor;
            }
            return;
        default:
            return;
    }
}

}  // namespace padinput_detail

// One event. `gen` is the device's Update generation (the detour's counter), `idx` the event's
// index inside it - the pump asks each index once per generation, in order.
inline UtPadInOut utPadInput(UtPadInState* s, unsigned long long gen, int idx,
                             const UtPadInEvent& e, const UtPadScreenRects& r,
                             const UtPadInGates& g) {
    using namespace padinput_detail;
    UtPadInOut o = fresh();
    if (!s) return o;
    // A repeat is the same queue slot read again: same generation, same index AND the same
    // event. A different event under the same key (a generation counter that did not advance)
    // is a new event, never a replay of an old decision.
    if (s->have && s->gen == gen && s->idx == idx && s->lastEvt.type == e.type &&
        memcmp(&s->lastEvt.x, &e.x, sizeof(e.x)) == 0 &&
        memcmp(&s->lastEvt.y, &e.y, sizeof(e.y)) == 0) {
        o = s->last;
        o.act = kUtPadActNone;
        o.button = -1;
        o.ended = 0;
        o.repeat = 1;
        return o;
    }
    s->have = 1;
    s->gen = gen;
    s->idx = idx;
    s->lastEvt = e;

    // THE ADOPTED DRAG: the editor drags (its press began in the window) and nothing is open
    // here - this event is the first of a press of ours. A LEFT DOWN is never adopted: it is a
    // new click (the drag's own UP was lost) and is judged below on its own.
    if (s->press == kUtPressNone && g.editorDrag && g.proven && e.type != kUtEvtLeftDown) {
        s->press = kUtPressEditor;
        s->adopted = 1;
        s->pressButton = -1;
        s->upSince = 0;
    }

    if (s->press != kUtPressNone) {
        const bool right = s->press == kUtPressRight;
        const int ownUp = right ? kUtEvtRightUp : kUtEvtLeftUp;
        // An ADOPTED drag inside the open editor's window is the window's - its MOVEs and
        // its UP pass untouched (the caravan saw its DOWN; its UP ends the drag through the
        // window's mouse slot, the polled cursor carries it); a LEFT DOWN there is a new click,
        // judged below (the window's too).
        if (s->adopted && e.type != kUtEvtLeftDown && inEditorWindow(r, g, e.x, e.y)) {
            o.why = kUtPadWhyWindow;
            if (e.type == ownUp) {
                o.ended = 1;
            } else if (!g.leftUp) {
                s->upSince = 0;
            } else if (s->upSince == 0) {
                s->upSince = gen + 1;
            } else if (gen + 1 >= s->upSince + kUtPadPollGenerations) {
                o.ended = 2;   // the polled release, here too (the event itself untouched)
            }
            if (o.ended) {
                s->press = kUtPressNone;
                s->adopted = 0;
                s->pressButton = -1;
                s->upSince = 0;
            }
            s->last = o;
            return o;
        }
        if (e.type == ownUp) {
            // An adopted drag's UP is CLEARED, not rewritten: the caravan saw its DOWN.
            if (s->adopted) o.clear = 1;
            else o.rewrite = 1;
            o.why = kUtPadWhyUp;
            o.ended = 1;
            if (s->press == kUtPressEditor) {
                o.act = kUtPadActEditor;
                o.editorType = kUtEvtLeftUp;
            }
            s->press = kUtPressNone;
            s->adopted = 0;
            s->pressButton = -1;
            s->upSince = 0;
            s->last = o;
            return o;
        }
        // The polled release: up for kUtPadPollGenerations whole generations with no UP event.
        const int upNow = right ? g.rightUp : g.leftUp;
        bool endByPoll = false;
        if (!upNow) {
            s->upSince = 0;
        } else if (s->upSince == 0) {
            s->upSince = gen + 1;
        } else if (gen + 1 >= s->upSince + kUtPadPollGenerations) {
            endByPoll = true;
        }
        // A LEFT DOWN ends ANY open press: a left one lost its UP, a right one is over (otherwise
        // the LEFT DOWN would be swallowed as a held event until the poll ended the press).
        const bool lostUp = e.type == kUtEvtLeftDown;
        const bool down = e.type == kUtEvtLeftDown || e.type == kUtEvtRightDown;
        if (endByPoll || lostUp) {
            s->press = kUtPressNone;
            s->adopted = 0;
            s->pressButton = -1;
            s->upSince = 0;
            o.ended = lostUp ? 3 : 2;
            // The last event of the press: cleared like every other one of it.
            o.clear = 1;
            o.why = kUtPadWhyHeld;
            if (down) {
                // A new DOWN is judged on its own: outside the rectangles it is the world's,
                // byte for byte (its held byte is ITS press, not the lost one) - also when the
                // poll ends the old press on it.
                const int ended = o.ended;
                o = fresh();
                o.ended = ended;
                decideIdle(s, e, r, g, &o);
            }
            s->last = o;
            return o;
        }
        o.clear = 1;
        o.why = kUtPadWhyHeld;
        if (s->press == kUtPressEditor && e.type == kUtEvtMove) {
            o.act = kUtPadActEditor;
            o.editorType = kUtEvtMove;
        }
        s->last = o;
        return o;
    }

    decideIdle(s, e, r, g, &o);
    s->last = o;
    return o;
}

// Apply a decision to the engine's 0x1C-byte record in place.
inline void utPadApply(unsigned char* evt, const UtPadInOut& o) {
    if (!evt) return;
    if (o.rewrite) {
        const int move = kUtEvtMove;
        memcpy(evt + kUtMouseEvtTypeOff, &move, sizeof(move));
    }
    if (o.rewrite || o.clear) memset(evt + kUtMouseEvtHeldOff, 0, kUtMouseEvtHeldCleared);
}

inline UtPadInEvent utPadReadEvent(const unsigned char* evt) {
    UtPadInEvent e = {0, 0.0f, 0.0f};
    if (!evt) return e;
    memcpy(&e.type, evt + kUtMouseEvtTypeOff, sizeof(e.type));
    memcpy(&e.x, evt + kUtMouseEvtXOff, sizeof(e.x));
    memcpy(&e.y, evt + kUtMouseEvtYOff, sizeof(e.y));
    return e;
}

}  // namespace ut
