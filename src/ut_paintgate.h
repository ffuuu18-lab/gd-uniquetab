// ut_paintgate.h - the one question `storeDisplayProtoId` asks before it builds a display
// prototype, as pure logic - and nothing else.
//
// THE BUG THIS FILE EXISTS TO MAKE IMPOSSIBLE. Gating the paint on a mode byte, while every path
// that BALANCES a paint (the deposit and take accounting, the
// substitution, `restoreOne`'s table branch, `runRescue`'s table merge) is gated on
// `storeTableOwns()`. A session where the two disagreed painted a TABLE-OWNED row (count >= 1)
// into a box that nothing could ever decrement: an infinitely refilling box. Both halves now ask
// the SAME question, once, here.
//
// The rule: held(record) = tableCount(record). count >= 1 is TABLE-OWNED - the item exists in
// the mod's own file and NOWHERE ELSE, so it may only be painted while the journal can own it
// (it is writable and has a path). When it cannot, the box keeps the engine's ordinary display
// prototype: a cosmetic loss, never an accounting one. A row at count 0 paints nothing: the mod
// never puts anything into the engine's own reagent map, so there is no second authority.
//
// It lives here, in the `ut_rowmath.h` / `ut_textfold.h` shape - no Windows, no
// engine, no file, no globals - so `tools\test_store.cpp` can prove the gate without a game.
#pragma once

namespace ut {

// Mirrors `ut_rescue.h`'s UT_STORED_*; `ut_store.cpp` static_asserts the three against it, so the
// two can never drift apart silently.
const int kUtStoredUnknown = -1;
const int kUtStoredNo = 0;
const int kUtStoredYes = 1;

// What the box should be painted FROM.
enum UtPaintWhat {
    kUtPaintNothing = 0,
    kUtPaintFromTable   // the mod's own file holds it: the COUNT is the prototype's stack
};

// Why not, when it is `kUtPaintNothing`. These map one-to-one onto `ut_store.cpp`'s BowReason
// values, so every refusal keeps naming itself in the log and in the parity report.
enum UtPaintWhy {
    kUtPaintWhyOk = 0,
    kUtPaintWhyNoEntry,      // the journal has no entry for that record
    kUtPaintWhyNotStored,    // "stored":false - the player took it back out
    kUtPaintWhyUnknown,      // not reconciled against the page yet
    kUtPaintWhyTableOff,     // count >= 1 but the mod does not own the collection this session
    kUtPaintWhyModeUnknown   // which of the two collections is live is not known yet
};

struct UtPaintDecision {
    int what;             // UtPaintWhat
    int why;              // UtPaintWhy - meaningful only when `what` is kUtPaintNothing
    unsigned int stack;   // the stack to build the prototype with; 0 when nothing is painted
};

// Does the MOD own the collection this session? A journal that can actually be written: an
// accepted deposit that cannot be written down is a lost item, so a read-only or path-less
// journal can never own anything. `storeTableOwns()` is this function plus the lookups it needs.
// THE SECOND HALF IS THE SAFETY-CRITICAL ONE. There are TWO collection files, one per mode
// (hardcore, softcore), and until a live world has told the mod which of them is this
// character's (`journalModeKnown()`), the open file is a GUESS - at start-up it is always the
// softcore one, because no character existed when the journal opened. Painting on that guess
// duplicates items: a hardcore session played in that state shows the SOFTCORE collection, and
// because the ENGINE's own take runs inline in the exe on a painted table box - the mod only
// ever records it AFTERWARDS - anything taken out of such a box is handed over while the
// recording side refuses it. The item then exists twice.
// A count that belongs to a file the mod is not sure of may therefore not be painted, may not be
// counted and may not be deposited into; and since a take can only reach a table row through a
// PAINTED box, refusing to paint is the only defence that acts before the engine does.
inline bool utPaintTableOwns(bool journalWritable, bool modeKnown) {
    return journalWritable && modeKnown;
}

// `haveEntry` is journalTableRow's own answer; `count`, `stored` and `stack` are its three
// outputs; `tableOwns` is utPaintTableOwns above. Nothing else decides anything.
// `modeKnown` is asked FIRST and on its own, before the entry is even considered: while it is
// false NO record paints, so the refusal is not a property of one row and must not read like one.
inline UtPaintDecision utPaintDecide(bool haveEntry, bool tableOwns, bool modeKnown,
                                     unsigned int count, int stored, unsigned int stack) {
    UtPaintDecision d;
    d.what = kUtPaintNothing;
    d.why = kUtPaintWhyOk;
    d.stack = 0;
    if (!modeKnown) {
        d.why = kUtPaintWhyModeUnknown;
        return d;
    }
    if (!haveEntry) {
        d.why = kUtPaintWhyNoEntry;
        return d;
    }
    if (count >= 1) {
        // TABLE-OWNED. The count IS the stack, and that is not cosmetic: the engine's take path
        // reads `Item::GetStackSize` at exe 0x132AD0 and ENDS THE TAKE at 0x132AE0 if it is 0, so
        // a display prototype at stack 0 would look like a box that cannot be emptied.
        if (!tableOwns) {
            d.why = kUtPaintWhyTableOff;
            return d;
        }
        d.what = kUtPaintFromTable;
        d.stack = count;
        return d;
    }
    // count 0: the row is history. Nothing of ours is ever in the engine's own map, so there is
    // no second place the item could be.
    (void)stack;
    d.why = (stored == kUtStoredNo) ? kUtPaintWhyNotStored : kUtPaintWhyUnknown;
    return d;
}

}   // namespace ut
