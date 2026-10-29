// test_rowfold.cpp - the row arithmetic and the text fold, proved without a game.
//
// Both are proved against the REAL headers the mod compiles (`src\ut_rowmath.h`,
// `src\ut_textfold.h`) - never a copy:
//
//   1. THE SCROLL BUG's arithmetic. Rows are counted in FILTERED units the moment the owned-only
//      filter is on, so the row on screen can be past the last row that still exists after a
//      take. The regression case replays a group scrolled down, most of its items taken out, and
//      then the wheel: without a re-clamp the wheel is consumed with nothing to show for it, so
//      the row is clamped the moment the visible count changes and the wheel works again the
//      moment there is more than one row.
//
//   2a. THE OWNED FOLD's arithmetic - `src\ut_ownedfold.h`. The display side
//      of the private table asks three questions of every row and they are all pure: is this row
//      inventory at all (the count-0 invariant, and the format-3 bridge), what shape
//      must its key have to join an owned set the engine's map keys are already in, and how does a
//      row move the change detector that asks for a relayout. The last one has a property worth a
//      test of its own: the fold must be ORDER-INDEPENDENT, because the journal's entry order is
//      not stable and an order-sensitive fold would ask for a relayout once a second for ever.
//
//   2. TIER C's case fold. The needle arrives as UTF-16 (already lowercased by the engine) and
//      the catalogue display name as UTF-8; both go through the same fold and the match is a
//      plain substring. A silent bug here would make tier C answer "no" for ever.
//
//  12. ZOOM_PCT and the row: a zoom change is a cols change (vanilla Helms 5 x 8 at 100 %, 3 x 5
//      of 96 px at 150 %, ut_layout.h utLiveGroupGrid - the call ut_live's applyGridLayout
//      makes), and utRowKeep keeps the first visible item on screen from 100 to 150 and back with
//      the owned-only filter on (rows in FILTERED units), clamped like any row after a take.
//
// Build: tools\build_test_rowfold.bat  (needs a vcvars64 shell; the .bat calls it itself)
#include <stdio.h>
#include <string.h>

#include <string>

#include "../src/ut_layout.h"   // utLiveGroupGrid / utRowKeep at a zoom
#include "../src/ut_ownedfold.h"
#include "../src/ut_rowmath.h"
#include "../src/ut_padmat.h"
#include "../src/ut_startorder.h"
#include "../src/ut_textfold.h"

using namespace ut;

static int g_fail = 0;
static int g_run = 0;

static void ok(const char* what, bool cond) {
    ++g_run;
    if (!cond) ++g_fail;
    printf("  %-64s %s\n", what, cond ? "OK" : "***** FAIL *****");
}

static void okInt(const char* what, int got, int want) {
    ++g_run;
    const bool c = got == want;
    if (!c) ++g_fail;
    printf("  %-64s %s (got %d, want %d)\n", what, c ? "OK" : "***** FAIL *****", got, want);
}

// ------------------------------------------------------------------ 1. the paging arithmetic
static void testRows() {
    printf("\n1. utMaxRowOf - the first-visible-row ceiling\n");
    // A real group: 20 boxes on screen, cols*rows = 20, and with 26..30 entries shown maxRow = 2
    // (which is what the mod's own heartbeat prints as `row=0/2`).
    const int cols = 5, rows = 4;
    okInt("a full window and nothing more has exactly one row position", utMaxRowOf(20, cols, rows), 0);
    okInt("an empty group has one row position, not a negative one", utMaxRowOf(0, cols, rows), 0);
    okInt("8 of 127 entries still fit in one screen", utMaxRowOf(8, cols, rows), 0);
    okInt("30 entries give 2", utMaxRowOf(30, cols, rows), 2);
    okInt("127 unfiltered entries give 22", utMaxRowOf(127, cols, rows), 22);
    okInt("a zero-column group cannot divide by zero", utMaxRowOf(50, 0, rows), 0);
    okInt("a negative total is treated as empty", utMaxRowOf(-3, cols, rows), 0);

    printf("\n2. utClampRow\n");
    okInt("a row past the end comes back to the end", utClampRow(9, 2), 2);
    okInt("a negative row comes back to 0", utClampRow(-4, 2), 0);
    okInt("a row inside the range is untouched", utClampRow(1, 2), 1);
    okInt("maxRow 0 forces row 0", utClampRow(7, 0), 0);
}

// ------------------------------------------------------------------ 2. THE regression case
static void testScrollRegression() {
    printf("\n3. THE SCROLL BUG regression - filter on, take most of the group out, wheel\n");
    const int cols = 5, rows = 4;      // 20 boxes on screen
    int shown = 30;                    // owned entries under the filter
    int curRow = 2;                    // the user has scrolled to the last row
    int wantRow = 2;

    okInt("before the takes the group has 2 scrollable rows", utMaxRowOf(shown, cols, rows), 2);

    // --- the takes. 22 of the 30 owned records are taken back out. The reagent map keeps every
    // node (a take decrements the stored prototype's stack and erases nothing), so the node set
    // and every prototype id are unchanged - which is exactly why the old key+id fingerprint saw
    // nothing and nothing asked for a relayout.
    shown = 8;
    const int m = utMaxRowOf(shown, cols, rows);
    okInt("after the takes there is only one row position left", m, 0);

    // --- WITHOUT the fix: no relayout is asked for, so cur/want stay at 2 and the page keeps
    // drawing rows that no longer exist. The first wheel event snaps to the top; every one after
    // it is consumed and changes nothing, so the page cannot be scrolled back.
    {
        int row = wantRow;
        bool changed = false;
        row = utWheelRow(row, -1, m, &changed);        // scroll down
        ok("stale state: the first wheel event still moves (it snaps to the top)", changed);
        okInt("stale state: and it lands on row 0", row, 0);
        row = utWheelRow(row, -1, m, &changed);
        ok("stale state: every wheel event after that is consumed and dead", !changed);
    }

    // --- WITH the fix: `liveTick` sees plateOwnedGeneration() move and re-clamps before the user
    // ever touches the wheel, and `applyLayout` re-clamps again against the snapshot it used. The
    // page and the row agree, so nothing "snaps".
    wantRow = utClampRow(wantRow, m);
    curRow = utClampRow(curRow, m);
    okInt("fixed: the wanted row is re-clamped the moment the count changes", wantRow, 0);
    okInt("fixed: and so is the row on screen", curRow, 0);
    ok("fixed: cur == want, so no surprise relayout is pending", curRow == wantRow);

    // --- the items come back (a deposit forces plateOwnedRefresh(true) + a relayout, and a
    // re-deposit into a count-0 node now moves the fingerprint too). The wheel must work again
    // the moment there is more than one row.
    shown = 30;
    const int m2 = utMaxRowOf(shown, cols, rows);
    okInt("items back: 2 rows again", m2, 2);
    bool changed = false;
    int row = utWheelRow(curRow, -1, m2, &changed);
    ok("items back: the wheel moves again on the very first event", changed);
    okInt("items back: down one row", row, 1);
    row = utWheelRow(row, -1, m2, &changed);
    okInt("items back: and one more", row, 2);
    row = utWheelRow(row, -1, m2, &changed);
    ok("items back: the last row is still the last row", !changed);
    row = utWheelRow(row, +1, m2, &changed);
    ok("items back: and scrolling up moves", changed);
    okInt("items back: up one row", row, 1);

    // --- the boundary: "the moment there is more than one row".
    for (int n = 1; n <= 25; ++n) {
        const int mm = utMaxRowOf(n, cols, rows);
        bool ch = false;
        utWheelRow(0, -1, mm, &ch);
        const bool expect = n > cols * rows;      // more than one screen == more than one row
        if (ch != expect) {
            ++g_fail;
            printf("  %-64s ***** FAIL ***** (n=%d)\n", "the wheel moves exactly when a second row exists", n);
            ++g_run;
            return;
        }
    }
    ok("the wheel moves exactly when a second row exists (n = 1..25)", true);
}

// ------------------------------------------------------------------ 3. tier C's case fold
static bool contains(const char* nameUtf8, const wchar_t* needle) {
    std::string n, q;
    utFoldUtf8(nameUtf8, &n);
    utFoldWide(needle, (int)wcslen(needle), &q);
    if (q.empty()) return false;
    return strstr(n.c_str(), q.c_str()) != NULL;
}

static void testFold() {
    printf("\n4. tier C - the case fold that matches an UNOWNED record by name\n");
    ok("an exact lowercase needle matches", contains("Stormtitan Treads", L"stormtitan"));
    ok("the engine lowercases the needle, and the NAME is folded to match",
       contains("Stormtitan Treads", L"titan"));
    ok("a substring in the middle of a word matches (SearchText is a substring test too)",
       contains("Mythical Bloodsworn Repeater", L"sworn"));
    ok("a needle that is not there does not match", !contains("Stormtitan Treads", L"aetherfire"));
    ok("an empty needle never matches (the caller clears the mask instead)",
       !contains("Stormtitan Treads", L""));
    ok("a needle longer than the name does not match",
       !contains("Ugdenbog Leather", L"ugdenbog leather works"));

    // A needle the engine has NOT lowercased must still work, because the mod folds it too.
    ok("an upper-case needle is folded on our side as well", contains("Beronath, Reforged", L"BERONATH"));
    ok("punctuation and spaces survive the fold", contains("Beronath, Reforged", L", refor"));

    // Non-ASCII: the fold is ASCII-ONLY on purpose. The engine lowercases both the needle and
    // the item's rollover text with its own locale ctype<wchar_t>::tolower, which the mod cannot
    // prove folds anything outside ASCII - so folding Latin-1 here would risk an OVER-report,
    // and under-reporting is the correct answer.
    {
        // "Gr(U-umlaut)NER Stein": the umlaut is UTF-8 C3 9C in the name, UTF-16 0x00DC.
        const char* nameUtf8 = "Gr\xC3\x9C" "NER Stein";
        const wchar_t lower[] = {0x0067, 0x0072, 0x00FC, 0x006E, 0x0065, 0x0072, 0};  // gr-u:-ner
        const wchar_t same[] = {0x0067, 0x0072, 0x00DC, 0x006E, 0x0065, 0x0072, 0};   // gr-U:-ner
        ok("a Latin-1 capital is NOT folded, so the name half can only UNDER-report",
           !contains(nameUtf8, lower));
        ok("the very same code point still matches, with the ASCII around it folded",
           contains(nameUtf8, same));
        ok("ASCII inside a name that carries a non-ASCII byte still folds",
           contains(nameUtf8, L"ner ste"));
    }
    {
        std::string out;
        utFoldUtf8("\x80\x80" "abc", &out);          // stray continuation bytes
        ok("a malformed UTF-8 lead byte is dropped, not looped on", out == "abc");
        utFoldUtf8("\xE2\x82", &out);                 // a truncated 3-byte sequence
        ok("a truncated sequence at the end terminates", out.empty());
        utFoldUtf8("", &out);
        ok("an empty name folds to an empty string", out.empty());
        utFoldUtf8(NULL, &out);
        ok("a null name folds to an empty string", out.empty());
        utFoldWide(NULL, 0, &out);
        ok("a null needle folds to an empty string", out.empty());
    }
    {
        // Round trip: everything the fold emits must be valid UTF-8 that folds to itself.
        std::string a, b;
        utFoldUtf8("MYTHICAL Ugdenbog \xC3\x89" "clair", &a);
        utFoldUtf8(a.c_str(), &b);
        ok("the fold is idempotent", a == b);
    }
}

// --------------------------------------------------------------- 4. the owned fold
static void testOwnedFold() {
    printf("\n7. utOwnedRowIsInventory - the count-0 invariant\n");
    // A row's count is `journalCollectStored`'s second output (format 4), never a reading of its
    // `"stored"` mark: `count` counts copies living ONLY in the mod's file, so a format-3 row -
    // and every MAP-OWNED row - is count 0 whatever its mark says. `tools\test_store.cpp` owns
    // the count's own arithmetic.
    ok("count 0 is HISTORY, not inventory", !utOwnedRowIsInventory(0));
    ok("count 1 is inventory", utOwnedRowIsInventory(1));
    ok("count 4 is inventory", utOwnedRowIsInventory(4));

    printf("\n8. utOwnedNormaliseKey - the shape plateOwns() can find\n");
    char k[64];
    ok("an already-normalised record path is unchanged",
       utOwnedNormaliseKey("records/items/gearweapons/shields/c021_shield.dbr", k, sizeof(k)) &&
           !strcmp(k, "records/items/gearweapons/shields/c021_shield.dbr"));
    ok("upper case is folded down (plateOwns is a case-SENSITIVE hash lookup)",
       utOwnedNormaliseKey("Records/Items/GearWeapons/C021_Shield.DBR", k, sizeof(k)) &&
           !strcmp(k, "records/items/gearweapons/c021_shield.dbr"));
    ok("a backslash becomes a forward slash, exactly as nodeKeyCopy folds it",
       utOwnedNormaliseKey("records\\items\\a.dbr", k, sizeof(k)) &&
           !strcmp(k, "records/items/a.dbr"));
    {
        char tiny[8];
        ok("a key that does not fit is REFUSED, never truncated to a wrong key",
           !utOwnedNormaliseKey("records/items/a.dbr", tiny, sizeof(tiny)));
        ok("... and the buffer is left empty, so a caller that ignores the false has nothing",
           tiny[0] == 0);
        char exact[8];
        ok("a key that fits exactly is accepted", utOwnedNormaliseKey("abc.dbr", exact, sizeof(exact)) &&
                                                      !strcmp(exact, "abc.dbr"));
    }
    ok("an empty record is not a key", !utOwnedNormaliseKey("", k, sizeof(k)));
    ok("a null record is not a key", !utOwnedNormaliseKey(NULL, k, sizeof(k)));
    ok("a null buffer is refused rather than written to", !utOwnedNormaliseKey("a.dbr", NULL, 8));

    printf("\n9. utOwnedRowHash / utOwnedMixRow - the change detector\n");
    const char* a = "records/items/a.dbr";
    const char* b = "records/items/b.dbr";
    ok("the same row hashes the same way twice", utOwnedRowHash(a, 1) == utOwnedRowHash(a, 1));
    ok("a different record hashes differently", utOwnedRowHash(a, 1) != utOwnedRowHash(b, 1));
    ok("the COUNT is in the hash - a second copy moves it",
       utOwnedRowHash(a, 1) != utOwnedRowHash(a, 2));
    ok("a row at count 0 does not hash like the same row at count 1",
       utOwnedRowHash(a, 0) != utOwnedRowHash(a, 1));
    {
        // THE PROPERTY: the journal's entry order is not stable (an upsert replaces in place, a
        // new record appends), so the same three rows in any order must give the same mix - or
        // the page would ask for a relayout once a second for ever, for nothing.
        const char* r[3] = {a, b, "records/items/c.dbr"};
        const unsigned int c[3] = {1, 2, 1};
        unsigned long long base = 0;
        for (int i = 0; i < 3; ++i) utOwnedMixRow(&base, r[i], c[i]);
        int order[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
        bool same = true;
        for (int o = 0; o < 6; ++o) {
            unsigned long long m = 0;
            for (int i = 0; i < 3; ++i) utOwnedMixRow(&m, r[order[o][i]], c[order[o][i]]);
            if (m != base) same = false;
        }
        ok("all six orderings of three rows give one mix (order-INDEPENDENT)", same);
        unsigned long long m = 0;
        utOwnedMixRow(&m, r[0], c[0]);
        utOwnedMixRow(&m, r[1], c[1]);
        ok("dropping a row moves the mix (a take that empties a record is seen)", m != base);
        m = 0;
        for (int i = 0; i < 3; ++i) utOwnedMixRow(&m, r[i], i == 1 ? 3u : c[i]);
        ok("a changed count moves the mix (a second copy deposited is seen)", m != base);
        m = 0;
        for (int i = 0; i < 3; ++i) utOwnedMixRow(&m, r[i], c[i]);
        utOwnedMixRow(&m, "records/items/d.dbr", 1);
        ok("a new row moves the mix (a first deposit of a record is seen)", m != base);
        m = 0;
        ok("no rows at all leave the mix at zero", m == 0);
        utOwnedMixRow(NULL, a, 1);
        ok("a null mix is a no-op, not a crash", true);
    }
}

// 3. THE START-UP ORDER (src\ut_startorder.h). The plates are loaded only once a materials window
//    has been captured in a world - never on the first hooked frame at the main menu, which can
//    fall in the engine's start-up streaming. And a frame counter that never moved past the first
//    hooked frame is "no frame yet", not a crash, unless the crash reporter's event exists.
static void testStartOrder() {
    using namespace ut;
    ok("main menu, code resolved, no window: no plate load", !utPlateProbeAllowed(true, false));
    ok("code not resolved: no plate load", !utPlateProbeAllowed(false, true));
    ok("nothing yet: no plate load", !utPlateProbeAllowed(false, false));
    ok("a captured materials window with the code resolved: the plates load",
       utPlateProbeAllowed(true, true));
    // the fresh-install session: frames=1, CrashReport.dll loaded, no CRASHREPORT event
    ok("frames=1 with CrashReport.dll loaded is no frame yet",
       utStallVerdict(false, true, 1) == UT_STALL_NO_FRAME_YET);
    ok("frames=0 is no frame yet", utStallVerdict(false, false, 0) == UT_STALL_NO_FRAME_YET);
    ok("the CRASHREPORT event is a crash even before a frame",
       utStallVerdict(true, true, 1) == UT_STALL_CRASHED);
    ok("the CRASHREPORT event is a crash", utStallVerdict(true, false, 500) == UT_STALL_CRASHED);
    ok("frames stopped with the reporter loaded is not called a crash",
       utStallVerdict(false, true, 500) == UT_STALL_CRASH_DLL);
    ok("frames stopped without a reporter", utStallVerdict(false, false, 500) == UT_STALL_UNKNOWN);
    ok("the no-frame text says no frame yet",
       strstr(utStallVerdictText(UT_STALL_NO_FRAME_YET), "no frame yet") != NULL);
    ok("only the crash verdict says CRASHED",
       strstr(utStallVerdictText(UT_STALL_NO_FRAME_YET), "CRASHED") == NULL &&
           strstr(utStallVerdictText(UT_STALL_CRASH_DLL), "CRASHED") == NULL &&
           strstr(utStallVerdictText(UT_STALL_UNKNOWN), "CRASHED") == NULL &&
           strstr(utStallVerdictText(UT_STALL_CRASHED), "CRASHED") != NULL);
}

// 4. THE PAD ON THE MATERIALS PAGE (src\ut_padmat.h). Vanilla's 24 materials boxes start at
//    y 76, below the band, so the pad draws as it always did. A page with 15 boxes at y 13..15
//    (x 33..881, 32 px icons) has boxes under it. utPadReturnCell (phase 0's return-cell choice,
//    the first cell that meets no box) keeps its arithmetic here only: the materials
//    page draws the whole pad where it is, over the boxes, and its clicks are the pad's
//    (utMatPadResolve, test_layout section 7; utPadLeftDown, test_layoutedit section 8).
static void testPadMat() {
    using namespace ut;
    const UtRectF pad = {102.0f, 25.0f, 320.0f, 44.0f};   // the default pad at UI scale 1
    UtRectF cells[27];
    for (int c = 0; c < 27; ++c)
        cells[c] = UtRectF{105.0f + (float)(c % 9) * 35.0f, 25.0f + (float)(c / 9) * 15.0f, 34.0f, 14.0f};
    // a vanilla-like page: 4 columns of 6 rows from 128,76
    UtRectF van[24];
    for (int i = 0; i < 24; ++i)
        van[i] = UtRectF{128.0f + (float)(i % 4) * 64.0f, 76.0f + (float)(i / 4) * 64.0f, 32.0f, 32.0f};
    ok("vanilla's boxes leave the pad alone", !utAnyOverlap(pad, van, 24));
    for (int i = 0; i < 24; ++i) ok("a vanilla box record is clear of the band", utBoxClearOfBand(76 + (i / 4) * 64));
    // a page with boxes at the top
    const int topX[15] = {33, 73, 117, 191, 231, 270, 335, 399, 463, 527, 591, 655, 753, 817, 881};
    const int topY[15] = {15, 15, 15, 15, 15, 15, 13, 13, 13, 13, 13, 13, 13, 13, 13};
    UtRectF top[15];
    for (int i = 0; i < 15; ++i) top[i] = UtRectF{(float)topX[i], (float)topY[i], 32.0f, 32.0f};
    ok("boxes at y 13..15 reach under the pad", utAnyOverlap(pad, top, 15));
    ok("their records are not clear of the band", !utBoxClearOfBand(13));
    const int cell = utPadReturnCell(cells, 27, top, 15);
    okInt("the return button takes the first free cell (row 3, column 1)", cell, 18);
    ok("that cell meets no box", cell >= 0 && !utAnyOverlap(cells[cell], top, 15));
    // a page whose boxes cover every cell
    UtRectF wall[1] = {{100.0f, 20.0f, 330.0f, 60.0f}};
    okInt("every cell blocked: no free cell (the caller searches the rows, else no button)",
          utPadReturnCell(cells, 27, wall, 1), -1);
    UtRectF none[1] = {{0.0f, 0.0f, 0.0f, 0.0f}};
    ok("a zero-size rect overlaps nothing", !utAnyOverlap(pad, none, 1));
    ok("rects that only touch do not overlap",
       !utRectsOverlap(UtRectF{0, 0, 10, 10}, UtRectF{10, 0, 10, 10}));
}

// ------------------------------------------------------------------ 12. zoom_pct and the row
static void testZoomRowKeep() {
    printf("\n12. zoom_pct - the first visible item kept from 100 %% to 150 %% and back, OWN on\n");
    UtLayout L = {};
    L.rect[kUtLayGrid] = {105, 76, 333, 552};   // vanilla's derived grid
    UtZoomFit z100 = {}, z150 = {};
    const UtGrid g100 = utLiveGroupGrid(L, 5, 8, 64, 64, kUtRowGap, 234, 100, &z100);
    const UtGrid g150 = utLiveGroupGrid(L, 5, 8, 64, 64, kUtRowGap, 234, 150, &z150);
    okInt("Helms at 100 %: the G line's 5 columns", g100.cols, 5);
    okInt("Helms at 100 %: and its 8 rows", g100.rows, 8);
    okInt("Helms at 150 %: 3 columns", g150.cols, 3);
    okInt("Helms at 150 %: 5 rows", g150.rows, 5);
    okInt("Helms at 150 %: cells of 96 px", z150.cellW, 96);
    // the owned-only filter on: 200 owned Helms, rows are FILTERED rows
    const int shown = 200;
    const int max100 = utMaxRowOf(shown, g100.cols, g100.rows);
    const int max150 = utMaxRowOf(shown, g150.cols, g150.rows);
    okInt("200 owned at 100 %: the last first-row is 32", max100, 32);
    okInt("200 owned at 150 %: the last first-row is 62", max150, 62);
    int lost = 0, notTop = 0, drift = 0, far = 0;
    for (int row = 0; row <= max100; ++row) {
        const int first = row * g100.cols;   // the first visible item at 100 %
        const int r150 = utClampRow(utRowKeep(row, g100.cols, g150.cols), max150);
        if (first < r150 * g150.cols || first >= r150 * g150.cols + g150.cols * g150.rows) ++lost;
        if (first >= r150 * g150.cols + g150.cols) ++notTop;   // not in the first row on screen
        const int back = utClampRow(utRowKeep(r150, g150.cols, g100.cols), max100);
        if (first < back * g100.cols || first >= back * g100.cols + g100.cols * g100.rows) ++lost;
        if (back != row) ++drift;
        if (back < row - 1 || back > row) ++far;
    }
    ok("100 -> 150: every first visible item is in the first row on screen", lost == 0 && notTop == 0);
    char what[96];
    snprintf(what, sizeof(what), "150 -> 100: back on screen; %d of %d rows come back one row up, none further",
             drift, max100 + 1);
    ok(what, lost == 0 && far == 0);
    // a take while zoomed: 160 of the 200 go, the row past the end comes back to the last one
    const int r = utRowKeep(max100, g100.cols, g150.cols);
    okInt("the last 100 % row is row 53 at 150 %", r, 53);
    okInt("40 left at 150 %: clamped to the last row, 9", utClampRow(r, utMaxRowOf(40, g150.cols, g150.rows)), 9);
    okInt("nothing owned: row 0", utClampRow(r, utMaxRowOf(0, g150.cols, g150.rows)), 0);
    // zoom 100 changes nothing: the same cols, the row kept as it is
    okInt("100 -> 100: the row is the row", utRowKeep(17, g100.cols, g100.cols), 17);
}

int main() {
    printf("offline harness - the paging arithmetic and the tier-C case fold\n");
    printf("plus the owned fold's own arithmetic (src\\ut_ownedfold.h)\n");
    printf("and the start-up order (src\\ut_startorder.h)\n");
    testRows();
    testScrollRegression();
    testFold();
    testOwnedFold();
    testStartOrder();
    testPadMat();
    testZoomRowKeep();
    printf("\n%s (%d failure(s) of %d checks)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_run);
    return g_fail ? 1 : 0;
}
