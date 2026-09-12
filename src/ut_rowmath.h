// ut_rowmath.h - the paging arithmetic behind the live collection page, and nothing else.
//
// THE SCROLL BUG this file exists to prevent: scrolling dies on a category tab once most of its
// items have been taken out with the OWN filter on.
// The cause is arithmetic, so it can be tested without a game:
// rows are counted in FILTERED units the moment the owned-only filter is on, so every
// clamp has to be re-applied against the count the CURRENT layout used, never against the one the
// wheel, PgUp/PgDn or the ini restore happened to see.
//
// Pure integer arithmetic: no Windows, no engine, no allocation, no globals. `ut_live.cpp` uses
// it and `tools\test_rowfold.cpp` links this very header to prove the sequence that broke.
#pragma once

namespace ut {

// The largest first-visible row for `total` entries laid out `cols` wide in a window `rows` high.
// Never negative: a page that does not fill the window has exactly one row position, 0.
inline int utMaxRowOf(int total, int cols, int rows) {
    if (cols <= 0 || rows <= 0) return 0;
    if (total < 0) total = 0;
    const int used = (total + cols - 1) / cols;
    const int m = used - rows;
    return m > 0 ? m : 0;
}

// Clamp a wanted first-visible row into [0, maxRow].
inline int utClampRow(int row, int maxRow) {
    if (maxRow < 0) maxRow = 0;
    if (row > maxRow) row = maxRow;
    if (row < 0) row = 0;
    return row;
}

// One wheel step, exactly as `liveHandleWheel` applies it: a positive tick count scrolls UP.
// Returns the new row. `changed` (optional) says whether the page will actually move - the
// wheel is CONSUMED either way, which is why a stale `maxRow` reads as "scrolling stopped".
inline int utWheelRow(int row, int ticks, int maxRow, bool* changed) {
    const int next = utClampRow(row + (ticks > 0 ? -1 : 1), maxRow);
    if (changed) *changed = next != row;
    return next;
}

}  // namespace ut
