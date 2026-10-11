// ut_hudlife.h - the life of the captured boxes, and the "the tab is off in this world" verdict,
// as pure logic.
//
// THE BUG THIS FILE EXISTS TO MAKE IMPOSSIBLE (seen in a real play session, reproduced offline).
// The box pointers ut_live.cpp captures while ReagentWindow::Load builds the frame page (160 at
// the time; the frame cap follows the cover, the F line's N, 160..512)
// belong to ONE HUD. GameEngine::ExitPlayingMode destroys that HUD; ut_plate forgot its window
// there ("plate: forgetting the material ReagentWindow ... (its HUD is gone)"), but the box list
// stayed full, and liveTick's only test was "is the list complete". So in the main menu after a
// Reign of Terror world - group 28 "Other" on screen - the database load of the basemods view
// replaced the group table (24 groups), liveReloadView clamped the wanted group 28 to -1, the
// next tick saw "wanted -1, shown 28" with all 160 boxes in the list and re-pointed box 0 of the DEAD
// HUD to restore the vanilla page. The engine call faulted (grim dawn.exe+0x262BC0, a read at
// 0x5), and a fault in a relayout takes live paging off FOR THE SESSION:
// "live: DISABLED for this session - an engine call faulted while restoring the vanilla layout".
// From then on liveFramePath() was null, so the next views (DeathGrimDawn, then the main
// campaign again) read no frame record back, substituted no page and captured no boxes - the
// tab was dead with nothing at a world start saying so.
//
// The rules:
//   1. the boxes may be re-pointed only while they are a complete capture of a HUD that is still
//      alive - `utBoxesUsable`;
//   2. the moment the HUD dies (the explicit world teardown) or the database under it is replaced
//      (a view change) the list is emptied and nothing is shown any more - `utBoxesHudGone`; the
//      next HUD build captures afresh (`utBoxesBeginCapture`);
//   3. a world that starts with live paging switched on but unable to be live says so with ONE
//      WARN naming the reason - `utTabVerdict` / `utTabOffLine`.
//
// No Windows, no engine, no globals - the `ut_pagegate.h` / `ut_startorder.h` shape - so
// `tools\test_logview.cpp` replays that session's log without a game.
#pragma once

#include <stdio.h>

namespace ut {

// ---- 1 + 2. the captured boxes ------------------------------------------------------------------
// What ut_live.cpp knows about its box list. `curGroup` is what the list SHOWS: -2 nothing applied
// yet, -1 the vanilla page, >= 0 a collection group.
struct UtHudBoxes {
    int captured;     // pointers in the list
    int nmax;         // the frame's box count (the F line's N: the cover's fit, 160..512)
    bool capturing;   // a capture is open (ReagentWindow::Load is building the page)
    int curGroup;
};

// liveTick's gate: the boxes may be re-pointed at all. A complete capture, no capture open, and a
// vtable the three located functions came from.
inline bool utBoxesUsable(const UtHudBoxes& b, bool vtableOk) {
    return !b.capturing && b.nmax > 0 && b.captured == b.nmax && vtableOk;
}

// The frame record was substituted and ReagentWindow::Load is about to build its boxes.
inline void utBoxesBeginCapture(UtHudBoxes* b) {
    b->captured = 0;
    b->capturing = true;
    b->curGroup = -2;
}

// The HUD that built the boxes is gone (GameEngine::ExitPlayingMode), or the database under it is
// being replaced (a view change): every pointer in the list is dead, a capture still open will
// never complete, and the list shows nothing.
inline void utBoxesHudGone(UtHudBoxes* b) {
    b->captured = 0;
    b->capturing = false;
    b->curGroup = -2;
}

// liveReloadView's clamp: a wanted group the new table does not have becomes the vanilla page.
inline int utWantedAfterTable(int wantGroup, int groups) {
    return wantGroup >= groups ? -1 : wantGroup;
}

// Whether liveTick re-points the boxes on this tick: a wanted group/row that differs from the one
// shown, or a forced repaint of the group on screen - and in every case only on usable boxes.
inline bool utTickRelayouts(const UtHudBoxes& b, bool vtableOk, int wantGroup, int wantRow,
                            int curRow, bool forced) {
    if (!utBoxesUsable(b, vtableOk)) return false;
    if (wantGroup != b.curGroup || wantRow != curRow) return true;
    return forced && b.curGroup != -2;
}

// ---- 3. the world-start verdict -----------------------------------------------------------------
// The facts, all of them known at the first Update with a main player (the HUD of the world, and
// with it the page substitution, has been built by then).
struct UtTabFacts {
    bool livePages;       // live_pages=1 (the default)
    bool disabled;        // live paging is stopped
    bool disabledWorld;   // ... and the stop belongs to this world only (a box vtable mismatch)
    bool dataOk;          // the view's uniq-groups.txt was read
    bool codeOk;          // the three exe signatures resolved
    bool frameNamed;      // the group table names a frame record
    bool unservable;      // this world's database could not serve the page records
};

enum UtTabVerdict {
    kUtTabLive = 0,       // the tab can be live in this world
    kUtTabOffByChoice,    // live_pages=0 - the user's choice, not a failure: no WARN
    kUtTabOffSession,     // stopped for the session (a fault, a detour, the exe scan)
    kUtTabOffWorld,       // stopped for this world only
    kUtTabOffNoTable,     // no group table
    kUtTabOffNoCode,      // the exe signatures are not resolved (yet)
    kUtTabOffNoFrame,     // the group table names no frame record
    kUtTabOffUnservable,  // this world's database cannot serve the page records
};

inline int utTabVerdict(const UtTabFacts& f) {
    if (!f.livePages) return kUtTabOffByChoice;
    if (f.disabled) return f.disabledWorld ? kUtTabOffWorld : kUtTabOffSession;
    if (!f.dataOk) return kUtTabOffNoTable;
    if (!f.codeOk) return kUtTabOffNoCode;
    if (!f.frameNamed) return kUtTabOffNoFrame;
    if (f.unservable) return kUtTabOffUnservable;
    return kUtTabLive;
}

inline bool utTabWarns(int verdict) {
    return verdict != kUtTabLive && verdict != kUtTabOffByChoice;
}

// The one WARN line (without the logger's prefix). `view` is the view in force, `why` the live
// layer's own reason (its g_why) - used for the verdicts whose reason only it knows. Returns the
// length written, 0 when the verdict warns about nothing.
inline int utTabOffLine(char* out, size_t cap, int verdict, const char* view, const char* why) {
    if (!out || cap == 0) return 0;
    out[0] = 0;
    if (!utTabWarns(verdict)) return 0;
    const bool haveWhy = why && why[0];
    const char* reason = haveWhy ? why : "of an earlier error";
    const char* until = "";
    switch (verdict) {
    case kUtTabOffSession:
        until = "it stays off until the game is restarted (the ERROR line above says when it went "
                "off)";
        break;
    case kUtTabOffWorld:
        until = "the next world gets its own answer";
        break;
    case kUtTabOffNoTable:
        reason = "the view's uniq-groups.txt is missing or unreadable";
        until = "the file is read again at the next database load";
        break;
    case kUtTabOffNoCode:
        until = "the late scan is still retrying the exe signatures";
        break;
    case kUtTabOffNoFrame:
        reason = "the view's uniq-groups.txt names no frame record";
        until = "the view is generated again when its catalogue.stamp no longer matches";
        break;
    default:   // kUtTabOffUnservable
        reason = "this world's database cannot serve the view's page records";
        until = "the next world asks again";
        break;
    }
    const int n = _snprintf_s(out, cap, _TRUNCATE,
                              "live: the tab is off in this world (the %s view) because %s - the "
                              "Crafting Materials page is the game's own here: no category "
                              "buttons, Ctrl+wheel does nothing, only the rollover line still "
                              "works; %s",
                              view && view[0] ? view : "?", reason, until);
    return n < 0 ? (int)(cap - 1) : n;
}

}  // namespace ut
