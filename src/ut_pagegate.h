// ut_pagegate.h - the one question the record substitution asks before it hands the engine one
// of the mod's page records instead of the vanilla Crafting Materials record, as pure logic.
//
// THE BUG THIS FILE EXISTS TO MAKE IMPOSSIBLE. Substituting a record the ACTIVE database cannot
// serve. The mod's pages live in an overlay archive loaded on top of the main database; a custom
// game (the Crucible, any <game>\mods world) loads a database of its own, and in that world the
// overlay may not be there. The substitution still fired, the engine found no table for the
// page, and the caravan opened on a window with no boxes at all - not even the vanilla Crafting
// Materials page, because its record had been swapped out and was never asked for.
//
// The rule: substitute only while the page records are known to be servable. The moment one of
// them does not load, the world falls back to the vanilla record and KEEPS falling back -
// ObjectManager::GetLoadTable returns a reference and cannot be null-checked, so the fallback
// has to be remembered rather than re-discovered. LoadTableFile asks first (ReagentWindow::Load
// calls it before GetLoadTable), which is what makes one flag enough.
//
// No Windows, no engine, no globals - the `ut_paintgate.h` / `ut_rowmath.h` shape - so
// `tools\test_store.cpp` can prove the gate without a game.
#pragma once

namespace ut {

// What the LoadTableFile / GetLoadTable detours do with a request for the material record.
enum UtPageAction {
    kUtPageVanilla = 0,   // hand the engine the record it asked for
    kUtPageSubstitute     // hand it `record`: the live frame page, or the selected page
};

// The facts, all of them known at the call site without touching the database.
struct UtPageFacts {
    bool frameArmed;      // live paging is armed: the page is ALWAYS the frame record
    int wantedPage;       // the selected collection page, or -1 for the vanilla page
    bool unavailable;     // this world has already failed to load one of our page records
    bool isMaterial;      // the request really is records/ui/caravan/caravan_materialwindow.dbr
};

// The decision. `frame` says which record `kUtPageSubstitute` means.
struct UtPageDecision {
    int action;    // UtPageAction
    bool frame;    // true = the live frame record, false = the selected page's record
};

inline UtPageDecision pageAction(const UtPageFacts& f) {
    UtPageDecision d;
    d.action = kUtPageVanilla;
    d.frame = false;
    if (!f.isMaterial) return d;
    // The flag outranks everything: in a world whose database has no page records there is
    // nothing to substitute, and the vanilla page is the whole of what the mod can offer there.
    if (f.unavailable) return d;
    if (f.frameArmed) {
        d.action = kUtPageSubstitute;
        d.frame = true;
        return d;
    }
    if (f.wantedPage >= 0) d.action = kUtPageSubstitute;
    return d;
}

// Whether the table the engine came back with is one the ACTIVE database can serve.
//
// A record the database does not hold does NOT come back null: the engine returns an EMPTY
// table, builds the window from it and the page ends up with no boxes at all. So a null test
// answers "servable" for exactly the case it exists to catch, and the CONTENT is the only real
// answer: every page record of the mod carries a reagentBoxes array (the frame 160, a collection
// page 40), and no entries means the record is not in this database.
//
// `boxes` < 0 = the count could not be read at all (the exports are missing, or the table is not
// a LoadTableBinary). That answers YES: a page wrongly kept shows the empty window this gate was
// written for, while a page wrongly dropped takes the tab off a world that could have had it.
inline bool pageTableServable(bool haveTable, int boxes) {
    if (!haveTable) return false;
    if (boxes < 0) return true;
    return boxes > 0;
}

// After the substituted record went to the engine: `servable` is `pageTableServable` on the table
// it returned. False means the active database cannot serve the page - the detour retries with
// the ORIGINAL record, the box capture is cancelled and the flag goes up for this world.
inline bool pageFallsBack(bool substituted, bool servable) {
    return substituted && !servable;
}

}  // namespace ut
