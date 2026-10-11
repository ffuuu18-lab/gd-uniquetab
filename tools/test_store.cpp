// test_store.cpp - THE PRIVATE TABLE'S ARITHMETIC, WITH NO GAME ANYWHERE NEAR IT.
//
// The table is not a container of its own - the journal's entry
// list IS the table and `count` on the entry is the row - so everything the deposit, the take and
// the rescue do to it is in ut_rescue.cpp, which links offline exactly as tools\test_journal.cpp
// already proves. This harness therefore exercises the REAL functions, in the REAL order, with
// the real file on a real disk:
//
//   journalTableRow / journalSetCount / journalCollectStored / journalCollectedTotal
//   journalUpsert (count-preserving) / journalUpsertCount / journalFlushNow
//   journalDepositCommit  <- the one that must be all-or-nothing
//
// The two functions in ut_store.cpp that call them - `storeOnDeposit` and `storeOnTake` - cannot
// be linked here (ut_store.cpp needs the engine), so this file MIRRORS their bodies in
// `depositLikeStore` / `takeLikeStore` below and says so. If either of those changes in
// ut_store.cpp and not here, the mirror is a lie: that is the one maintenance cost of this
// harness.
//
// THE CASES, in order:
//   1  a deposit into an empty row          -> count 1, "stored":true, the file on disk
//   2  a second copy                        -> count 2
//   3  one of two taken                     -> count 1
//   4  the last one taken                   -> count 0, "stored":false, THE ENTRY IS KEPT
//   5  a take from a row at count 0         -> impossible: it stays 0 and nothing is handed back
//   6  a deposit the journal cannot take    -> refused, nothing incremented
//   7  a deposit whose FLUSH fails          -> refused AND rolled back (the crash window, 3.2.1)
//   8  held = tableCount + mapHeld          -> the transition sum, all four combinations
//   9  the count-0 invariant                -> a row at 0 authorises no paint, no take, no rescue
//   9b THE PRUNE CANNOT EAT AN ITEM         -> a row at count 1 whose mark says "stored":false
//                                              SURVIVES journalPruneNotStored
//   10 format 4 round trip                  -> the counts survive a write and a re-read
//   11 a format-3 file                      -> every row at count 0, nothing invented
//   12 THE REHEARSAL                        -> a COPY of a real collection journal
//   14 THE PAINT GATE (ut_paintgate.h)      -> a count-1 row paints from a writable journal and
//                                              NOT from a read-only one
//   15 THE DEPOSIT GATE (ut_depositgate.h)  -> a deposit of one of our records goes into
//                                              the TABLE or is REFUSED, and there is no third
//                                              answer. All 768 fact combinations
//                                              are decided; exactly 5 of them may touch the
//                                              journal, so every refusal leaves it untouched
//   19 ONE ROLL PER MI RECORD (ut_miroll.h) -> the KIND seam (catIsMi by ADL, stubbed so
//                                              no catalogue is needed), the cap against an
//                                              independent oracle, the roll and its words, a second roll
//                                              refused with the first named, the commit's belt
//   16 THE PAGE GATE (ut_pagegate.h)       -> the material record is substituted only while the
//                                              active database can serve the page it is
//                                              substituted for; a world that cannot keeps the
//                                              vanilla page for the rest of its life
//
// Build + run:  tools\build_test_store.bat   (everything happens under build\test)
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include <string>
#include <vector>

#include "../src/ut_depositgate.h"
#include "../src/ut_pagegate.h"
#include "../src/ut_paintgate.h"
#include "../src/ut_rescue.h"

namespace ut {
bool logInit(const wchar_t* path);
void logFlush();
}  // namespace ut

static int g_fail = 0;

static void check(bool ok, const char* what) {
    printf("  %-72s %s\n", what, ok ? "OK" : "**FAIL**");
    if (!ok) ++g_fail;
}

static bool slurp(const char* path, std::string* out) {
    out->clear();
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got) out->append(buf, got);
    CloseHandle(h);
    return true;
}

static bool spit(const char* path, const std::string& text) {
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const BOOL ok = WriteFile(h, text.c_str(), (DWORD)text.size(), &wrote, nullptr);
    CloseHandle(h);
    return ok != 0 && wrote == (DWORD)text.size();
}

// A synthetic capture, the same shape tools\test_journal.cpp uses.
static void fill(ut::UtReplicaCapture* c, const char* record, unsigned int stack) {
    memset(c, 0, sizeof(*c));
    _snprintf_s(c->record, sizeof(c->record), _TRUNCATE, "%s", record);
    c->stack = stack;
    c->replicaLen = 0x190;
    for (unsigned int i = 0; i < c->replicaLen; ++i) c->replica[i] = (unsigned char)(i & 0xFF);
    c->slotCount = 2;
    c->slotOff[0] = 0x008;
    _snprintf_s(c->slotText[0], sizeof(c->slotText[0]), _TRUNCATE, "%s", record);
    c->slotOff[1] = 0x028;
    _snprintf_s(c->slotText[1], sizeof(c->slotText[1]), _TRUNCATE, "records/items/prefix.dbr");
}

static unsigned int countOf(const char* record) {
    unsigned int count = 0;
    int stored = -9;
    unsigned int stack = 0;
    if (!ut::journalTableRow(record, &count, &stored, &stack)) return 0;
    return count;
}

static int storedOf(const char* record) {
    unsigned int count = 0;
    int stored = -9;
    unsigned int stack = 0;
    if (!ut::journalTableRow(record, &count, &stored, &stack)) return -9;
    return stored;
}

// ---- THE MIRRORS -----------------------------------------------------------------------------
// `storeOnDeposit` (ut_store.cpp) minus the two things that need a game: the table-owns test and
// the per-world prototype cache drop. What is left is what this harness is for.
static bool depositLikeStore(const ut::UtReplicaCapture& cap, const char* record) {
    // The real one refuses HERE first, before anything else - there is one collection per
    // mode, and until a live world has named the mode the open file is the start-up
    // guess. The mirror must refuse in the same place or it is a lie (case 16b below).
    if (!ut::journalModeKnown()) return false;
    if (!record || !*record || !cap.record[0]) return false;
    const unsigned int before = countOf(record);
    bool rolledBack = false;
    if (!ut::journalDepositCommit(cap, before + 1, &rolledBack)) return false;
    return true;
}

// `storeOnTake` (ut_store.cpp), same exclusion.
static unsigned int takeLikeStore(const char* record, unsigned int n) {
    if (!record || !*record || !n) return countOf(record);
    const unsigned int before = countOf(record);
    if (!before) return 0;
    // The same guard, and in the real one it is only a BACKSTOP - a take from a painted
    // box is the engine's own code and has already happened by the time the mod is told.
    if (!ut::journalModeKnown()) return before;
    const unsigned int after = n >= before ? 0u : before - n;
    if (!ut::journalSetCount(record, after)) return before;
    return after;
}

// The transition rule as ONE expression. The real one is `reagentHeldTotal` in ut_reagent.cpp,
// which asks the engine for the map half; this is the arithmetic alone, with the map half passed
// in, so the offline harness can prove it.
//
// THE TABLE IS THE ONLY AUTHORITY. The engine's `reagents.gst` is foreign territory: nothing of
// the mod's is ever put into it, and a row still sitting in it from an older build is never
// counted as a copy of anything. The two halves are still reported separately (the refusal line
// prints `table=N map=M`, the takeover census names the rows), so a legacy row is never silent -
// it is just not the collection.
static int heldTotal(const char* record, int mapHeld, int* fromTable, int* fromMap) {
    const int table = (int)countOf(record);
    if (fromTable) *fromTable = table;
    if (fromMap) *fromMap = mapHeld;
    return table;
}

// `storeDisplayProtoId`'s GATE (ut_store.cpp), mirrored the way the deposit is. The real
// one builds an engine prototype after this point and cannot be linked here, but every question
// it asks before that is pure, is asked here in the same order and out of the same journal.
// `storeTableOwns()` is the writable-with-a-path test AND `journalModeKnown()`.
static ut::UtPaintDecision paintLikeStore(const char* record) {
    unsigned int count = 0, stack = 0;
    int stored = -9;
    const bool have = ut::journalTableRow(record, &count, &stored, &stack);
    const char* path = ut::journalPath();
    const bool owns =
        ut::utPaintTableOwns(!ut::journalReadOnly() && path && *path, ut::journalModeKnown());
    return ut::utPaintDecide(have, owns, ut::journalModeKnown(), count, stored, stack);
}

// The owned counters' table half (`plateTableOwns()` + `plateTableFold()`, ut_plate.cpp): the rows
// the mod's own file holds, and 0 whenever the mod does not own the collection this session.
static int ownedRowsLikeStore() {
    const char* path = ut::journalPath();
    if (!ut::utPaintTableOwns(!ut::journalReadOnly() && path && *path, ut::journalModeKnown())) {
        return 0;
    }
    return ut::journalCollectStored(nullptr, nullptr, 0);
}

static const char* const kA = "records/items/gearhead/a01_head.dbr";
static const char* const kB = "records/items/gearweapons/b02_sword.dbr";
static const char* const kLegacy = "records/items/gearhands/c03_gloves.dbr";

// ---- pass 1: the whole life of a row ---------------------------------------------------------
static int passMain() {
    if (!ut::journalInit(nullptr)) {
        printf("  [FAIL] journalInit\n");
        return 1;
    }
    printf("  journal: %s\n", ut::journalPath());

    // A world loads and names the collection mode. Every deposit and take below goes through
    // the same gate the game does, and that gate refuses while the mode is unknown (case 16b).
    ut::journalFollowMode(0);

    ut::UtReplicaCapture a, b, legacy;
    fill(&a, kA, 1);
    fill(&b, kB, 1);
    fill(&legacy, kLegacy, 1);

    // --- the LEGACY deposit first: journalUpsert is the map-owned path and must leave the table
    // empty. This is the one that keeps an existing journal's rows out of the table.
    printf("\n0. the legacy (map-owned) deposit puts NOTHING in the table\n");
    check(ut::journalUpsert(legacy), "journalUpsert accepts an ordinary deposit");
    check(countOf(kLegacy) == 0, "and the row's count is 0 - the engine's map holds that item");
    check(storedOf(kLegacy) == ut::UT_STORED_YES, "while its stored mark is YES, as it always was");
    check(ut::journalCollectedTotal() == 0, "the table holds 0 copies");
    check(ut::journalCollectStored(nullptr, nullptr, 0) == 0, "and 0 rows");

    printf("\n1. a deposit into an empty row\n");
    check(countOf(kA) == 0, "the row does not exist yet");
    check(depositLikeStore(a, kA), "the deposit is accepted");
    check(countOf(kA) == 1, "count is 1");
    check(storedOf(kA) == ut::UT_STORED_YES, "and the entry is marked stored");
    {
        std::string text;
        check(slurp(ut::journalPath(), &text), "THE FILE IS ON DISK ALREADY");
        check(text.find("\"count\":1") != std::string::npos,
              "and it carries the count - the row was durable BEFORE the deposit returned true");
        check(text.find("\"tableCopies\":1") != std::string::npos, "the header agrees");
        check(text.find("\"format\":4") != std::string::npos, "the file is format 4");
    }

    printf("\n2. a second copy of the same record\n");
    check(depositLikeStore(a, kA), "the second deposit is accepted");
    check(countOf(kA) == 2, "count is 2");
    check(ut::journalCollectedTotal() == 2, "the table holds two copies in all");
    check(ut::journalCollectStored(nullptr, nullptr, 0) == 1, "in ONE row");
    check(depositLikeStore(b, kB), "a deposit of a DIFFERENT record is accepted");
    check(ut::journalCollectStored(nullptr, nullptr, 0) == 2, "now two rows");
    check(ut::journalCollectedTotal() == 3, "and three copies");
    {
        char recs[8][256];
        unsigned int counts[8];
        const int n = ut::journalCollectStored(recs, counts, 8);
        check(n == 2, "journalCollectStored lists both rows");
        bool sawA = false, sawB = false, sawLegacy = false;
        for (int i = 0; i < n; ++i) {
            if (!_stricmp(recs[i], kA)) sawA = counts[i] == 2;
            if (!_stricmp(recs[i], kB)) sawB = counts[i] == 1;
            if (!_stricmp(recs[i], kLegacy)) sawLegacy = true;
        }
        check(sawA && sawB, "with the right counts");
        check(!sawLegacy,
              "and the MAP-OWNED row is NOT in it - journalCollectStored is table rows only");
    }

    printf("\n3. one of two taken\n");
    check(takeLikeStore(kA, 1) == 1, "the take returns the new count, 1");
    check(countOf(kA) == 1, "the row is at 1");
    check(storedOf(kA) == ut::UT_STORED_YES, "and is still marked stored");

    printf("\n4. the LAST one taken\n");
    check(takeLikeStore(kA, 1) == 0, "the take returns 0");
    check(countOf(kA) == 0, "the row is at 0");
    check(storedOf(kA) == ut::UT_STORED_NO, "the entry is marked \"stored\":false");
    check(ut::journalHas(kA), "AND THE ENTRY IS KEPT - the identity is history, not rubbish");
    check(ut::journalFlushNow(), "the file is written");
    {
        std::string text;
        check(slurp(ut::journalPath(), &text), "and reads back");
        check(text.find(kA) != std::string::npos, "the emptied record is still a line in it");
    }

    printf("\n5. a take from a row at count 0 is impossible\n");
    check(takeLikeStore(kA, 1) == 0, "the take answers 0 and changes nothing");
    check(countOf(kA) == 0, "the row is still 0");
    check(takeLikeStore("records/items/never/deposited.dbr", 1) == 0,
          "a take of a record with no entry at all answers 0 too");
    check(!ut::journalHas("records/items/never/deposited.dbr"),
          "and no entry was invented for it");

    printf("\n6. a deposit the journal cannot take is REFUSED and increments nothing\n");
    {
        ut::UtReplicaCapture bad;
        fill(&bad, kB, 1);
        bad.record[0] = 0;   // the one input journalUpsert refuses
        const unsigned int before = countOf(kB);
        check(!depositLikeStore(bad, ""), "the deposit is refused");
        check(countOf(kB) == before, "and the count did not move");
    }

    printf("\n7. THE CRASH WINDOW: the upsert succeeds and the FLUSH fails\n");
    // The injection: a DIRECTORY where the atomic writer wants to create its .tmp file. Nothing
    // in the mod is modified for the test - writeWholeFileAtomic's CreateFileA simply fails, the
    // same way a full disk or a locked file makes it fail.
    {
        char tmpPath[MAX_PATH];
        _snprintf_s(tmpPath, sizeof(tmpPath), _TRUNCATE, "%s.tmp", ut::journalPath());
        check(CreateDirectoryA(tmpPath, nullptr) != 0, "a directory now blocks the .tmp write");
        std::string fileBefore;
        check(slurp(ut::journalPath(), &fileBefore), "the file on disk is read first");
        const unsigned int before = countOf(kB);
        check(before == 1, "the record is at count 1 before the attempt");
        ut::UtReplicaCapture c2;
        fill(&c2, kB, 1);
        bool rolledBack = false;
        check(!ut::journalDepositCommit(c2, before + 1, &rolledBack),
              "journalDepositCommit REFUSES when the file cannot be written");
        check(rolledBack, "and it says it rolled the row back");
        check(countOf(kB) == before,
              "THE COUNT IS EXACTLY WHERE IT WAS - no phantom copy is left in the table");
        check(storedOf(kB) == ut::UT_STORED_YES, "and the stored mark is unchanged");
        std::string fileAfter;
        check(slurp(ut::journalPath(), &fileAfter), "the file still reads back");
        check(fileAfter == fileBefore, "BYTE FOR BYTE as it was - an abort leaves the file alone");
        check(RemoveDirectoryA(tmpPath) != 0, "the block is removed again");
        check(ut::journalDepositCommit(c2, before + 1, &rolledBack),
              "and the very same deposit now succeeds");
        check(!rolledBack, "with no rollback");
        check(countOf(kB) == before + 1, "the count moved this time");
        check(takeLikeStore(kB, 99) == 0, "put the row back to 0 for the next case");
    }

    printf("\n8. what the collection holds: THE TABLE, AND ONLY THE TABLE\n");
    {
        check(depositLikeStore(a, kA), "a table copy of A");
        int t = -1, m = -1;
        check(heldTotal(kA, 0, &t, &m) == 1 && t == 1 && m == 0, "table 1, map 0 -> 1");
        check(heldTotal(kA, 1, &t, &m) == 1 && t == 1 && m == 1,
              "table 1, map 1 -> 1: the map's row is not counted, and both halves are still "
              "reported (table=1 map=1) so the census and the refusal line can still name it");
        check(heldTotal(kLegacy, 1, &t, &m) == 0 && t == 0 && m == 1,
              "a LEGACY row the engine's map still holds is NOT the collection - it is reported "
              "(map=1) and the takeover census names it, but it holds nothing of ours");
        check(heldTotal(kLegacy, 3, &t, &m) == 0 && t == 0 && m == 3,
              "and three of them are still 0");
        check(heldTotal("records/items/never/deposited.dbr", 0, &t, &m) == 0 && t == 0 && m == 0,
              "a record in neither place is 0");
    }

    printf("\n9. the count-0 invariant\n");
    {
        // The rule the paint, the take and the rescue all ask, in the one form they ask it in.
        check(countOf(kA) >= 1, "a TABLE-OWNED row authorises a paint / a take / a rescue");
        check(countOf(kB) == 0, "a row at 0 does not");
        check(ut::journalHas(kB), "even though its entry is still there (it is HISTORY)");
        check(countOf(kLegacy) == 0,
              "and neither does a MAP-OWNED row - the map side of the mod hands that one back, "
              "which is what stops the rescue handing one item back twice");
    }

    // `journalPruneNotStored` is the ONLY thing in the mod that deliberately removes
    // journal entries, and under the private table a row at count >= 1 IS THE ITEM - there is no
    // engine copy behind it. It drops on `stored == UT_STORED_NO && count == 0`; this is the proof
    // that the second conjunct is load-bearing and not decoration. The contradictory row is built
    // the only way the mod could ever produce one - a reconcile marking a row not-stored without
    // the count following - because `journalSetCount(0)` keeps the pair together by construction.
    printf("\n9b. a count-1 row marked \"stored\":false SURVIVES the prune\n");
    {
        check(countOf(kA) == 1, "A is a TABLE-OWNED row at count 1");
        check(ut::journalMarkStored(kA, false), "a reconcile marks it \"stored\":false");
        check(storedOf(kA) == ut::UT_STORED_NO, "the mark really is NO now");
        check(countOf(kA) == 1, "and the count did NOT follow it down - this is the contradiction");
        check(countOf(kB) == 0 && storedOf(kB) == ut::UT_STORED_NO,
              "B is the row the prune is FOR: count 0 and not stored");
        char dropped[16][256];
        char why[256];
        const int gone = ut::journalPruneNotStored(dropped, 16, why, sizeof(why));
        check(gone >= 0, why[0] ? why : "the prune ran");
        bool droppedA = false, droppedB = false;
        for (int i = 0; i < gone && i < 16; ++i) {
            if (!_stricmp(dropped[i], kA)) droppedA = true;
            if (!_stricmp(dropped[i], kB)) droppedB = true;
        }
        check(!droppedA, "THE COUNT-1 ROW WAS NOT DROPPED - an item is not a stale identity");
        check(ut::journalHas(kA), "its entry is still in the journal");
        check(countOf(kA) == 1, "still at count 1");
        check(droppedB, "while the count-0 not-stored row WAS dropped, as it always was");
        check(!ut::journalHas(kB), "and its entry is gone");
        check(countOf(kLegacy) == 0 && ut::journalHas(kLegacy),
              "the MAP-OWNED row (count 0, \"stored\":true) is untouched either way");
        check(ut::journalMarkStored(kA, true), "the contradiction is repaired for the cases below");
        check(storedOf(kA) == ut::UT_STORED_YES && countOf(kA) == 1, "A is a normal row again");
        // Put B's history row back the way case 4 left it, so case 10 and the --reread pass see
        // the state they were written against: deposited once, then emptied.
        check(depositLikeStore(b, kB) && takeLikeStore(kB, 1) == 0, "B's history row is restored");
        check(ut::journalHas(kB) && countOf(kB) == 0 && storedOf(kB) == ut::UT_STORED_NO,
              "count 0, \"stored\":false, the entry kept - exactly as before the prune");
    }

    printf("\n10. the format-4 round trip (this process wrote it; --reread reads it)\n");
    check(ut::journalSetCount(kA, 3), "A is set to three copies");
    check(ut::journalFlushNow(), "and the file is flushed on THIS thread");
    {
        std::string text;
        check(slurp(ut::journalPath(), &text), "the file reads back");
        check(text.find("\"count\":3") != std::string::npos, "with count 3 on a line");
        check(text.find("\"tableCopies\":3") != std::string::npos, "and 3 in the header");
    }
    ut::logFlush();
    printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}

// ---- pass 2: a SEPARATE PROCESS reads the file back ------------------------------------------
static int passReread() {
    if (!ut::journalInit(nullptr)) {
        printf("  [FAIL] journalInit\n");
        return 1;
    }
    printf("\n10b. the counts survive the round trip\n");
    check(!ut::journalReadOnly(), "a format-4 file is not read-only to a format-4 build");
    check(countOf(kA) == 3, "A comes back at count 3");
    check(storedOf(kA) == ut::UT_STORED_YES, "and stored");
    check(countOf(kB) == 0, "B comes back at count 0");
    check(storedOf(kB) == ut::UT_STORED_NO, "and not stored");
    check(ut::journalHas(kB), "with its entry intact");
    check(countOf(kLegacy) == 0, "the map-owned row is still 0");
    check(storedOf(kLegacy) == ut::UT_STORED_YES, "and still marked stored");
    check(ut::journalCollectedTotal() == 3, "the table holds three copies in all");
    check(ut::journalCollectStored(nullptr, nullptr, 0) == 1, "in one row");
    ut::logFlush();
    printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}

// ---- pass 3: a format-3 file, and the contradiction repair -----------------------------------
static int passLegacyFile(const char* what) {
    // A hand-written format-3 journal: no counts anywhere, one stored row, one not-stored row,
    // one nobody has reconciled. It is what a real file looks like.
    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s", what);
    std::string t;
    t.append("{\"journal\":\"grim dawn uniquetab\",\"format\":3,\"written\":\"2000-01-01T00:00:00Z\""
             ",\"entries\":3,\"stored\":1,\"notStored\":1,\"unknown\":1}\n");
    t.append("{\"record\":\"records/items/legacy/one.dbr\",\"stored\":true,\"stack\":1,\"flags\":0,"
             "\"len\":400,\"baseRecord@008\":\"records/items/legacy/one.dbr\","
             "\"raw\":\"000:00000001\"}\n");
    t.append("{\"record\":\"records/items/legacy/two.dbr\",\"stored\":false,\"stack\":1,\"flags\":0,"
             "\"len\":400,\"baseRecord@008\":\"records/items/legacy/two.dbr\","
             "\"raw\":\"000:00000002\"}\n");
    t.append("{\"record\":\"records/items/legacy/three.dbr\",\"stack\":1,\"flags\":0,"
             "\"len\":400,\"baseRecord@008\":\"records/items/legacy/three.dbr\","
             "\"raw\":\"000:00000003\"}\n");
    // ... and the one contradiction a hand-edit can produce: a count with a "stored":false.
    t.append("{\"record\":\"records/items/legacy/four.dbr\",\"stored\":false,\"count\":2,"
             "\"stack\":1,\"flags\":0,\"len\":400,"
             "\"baseRecord@008\":\"records/items/legacy/four.dbr\",\"raw\":\"000:00000004\"}\n");
    if (!spit(path, t)) {
        printf("  [FAIL] could not write the fixture\n");
        return 1;
    }
    if (!ut::journalInit(nullptr)) {
        printf("  [FAIL] journalInit\n");
        return 1;
    }
    printf("\n11. a FORMAT-3 file: read in full, nothing invented\n");
    check(ut::journalCount() == 4, "all four lines are read");
    check(!ut::journalReadOnly(), "an older format is never read-only");
    check(countOf("records/items/legacy/one.dbr") == 0,
          "the \"stored\":true row is at count 0 - it is MAP-owned, and no copy was invented");
    check(storedOf("records/items/legacy/one.dbr") == ut::UT_STORED_YES,
          "its stored mark is untouched");
    check(countOf("records/items/legacy/two.dbr") == 0, "the not-stored row is at 0");
    check(countOf("records/items/legacy/three.dbr") == 0, "and so is the unreconciled one");
    check(storedOf("records/items/legacy/three.dbr") == ut::UT_STORED_UNKNOWN,
          "whose UNKNOWN mark is left exactly as it was");
    printf("\n11b. the one contradiction the file can carry\n");
    check(countOf("records/items/legacy/four.dbr") == 2, "count 2 with \"stored\":false: the COUNT survives");
    check(storedOf("records/items/legacy/four.dbr") == ut::UT_STORED_YES,
          "and the MARK is repaired to stored - the other direction would throw the item away");
    ut::logFlush();
    printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}

// ---- pass 4: THE REHEARSAL - a COPY of a real collection journal ---------------------------
// The strongest offline evidence there is that this build is safe to put in front of the player:
// their real file, read by the real reader, upgraded by the real writer, checked line for line.
// The harness copies it in; the original is only ever READ. With no fixture the pass SKIPS
// LOUDLY (exit 2), because a silent skip is how a harness comes to prove nothing.
static int passLive() {
    if (!ut::journalInit(nullptr)) {
        printf("  [FAIL] journalInit\n");
        return 1;
    }
    const size_t n = ut::journalCount();
    if (!n) {
        printf("  ***** SKIPPED - there is no live journal to rehearse against *****\n");
        return 2;
    }
    printf("\n12. THE REHEARSAL: a real journal, %zu entries\n", n);
    check(!ut::journalReadOnly(), "the live journal is NOT read-only to this build");
    static char recs[3600][256];
    const int got = ut::journalRecords(recs, 3600);
    check((size_t)got == n, "every record path came back");
    // THESE THREE CHECKS ASSERT INVARIANTS, NOT FACTS. A live journal legitimately carries rows
    // at count >= 1 and a non-zero `tableCopies` as soon as the private table has taken anything,
    // so a rehearsal against the player's real file has to hold whatever that file says. What is
    // asserted is therefore what must be true of ANY file: the row list and the total agree with
    // each other, every count is readable, and the round trip below loses nothing.
    const unsigned int collected = ut::journalCollectedTotal();
    const int tableRows = ut::journalCollectStored(nullptr, nullptr, 0);
    printf("  the private table holds %u cop(y/ies) across %d row(s) of this journal\n", collected,
           tableRows);
    check(tableRows >= 0, "the table row list can be counted");
    check((collected == 0) == (tableRows == 0),
          "the table's total and its row list agree - either both are empty or neither is");
    size_t stored = 0, notStored = 0, unknown = 0, entries = 0;
    ut::journalCounts(&entries, &stored, &notStored, &unknown);
    printf("  %zu entries: %zu stored, %zu not stored, %zu not yet reconciled\n", entries, stored,
           notStored, unknown);
    unsigned int countSum = 0;
    int counted = 0;
    bool rowsReadable = true;
    for (int i = 0; i < got; ++i) {
        unsigned int count = 99;
        int mark = -9;
        unsigned int stack = 0;
        if (!ut::journalTableRow(recs[i], &count, &mark, &stack)) {
            rowsReadable = false;
            printf("      the unreadable row was %s\n", recs[i]);
            break;
        }
        if (count) {
            ++counted;
            countSum += count;
        }
    }
    check(rowsReadable, "EVERY row of the live journal reads back");
    check(counted == tableRows && countSum == collected,
          "and the per-row counts add up to exactly what the table says it holds - nothing "
          "invented, nothing dropped");
    // Now the upgrade write, which is what the first real run will do.
    check(ut::journalFlushNow(), "the upgrade write succeeds");
    std::string after;
    check(slurp(ut::journalPath(), &after), "and the upgraded file reads back");
    check(after.find("\"format\":4") != std::string::npos, "it is format 4");
    {
        char want[64];
        _snprintf_s(want, sizeof(want), _TRUNCATE, "\"tableCopies\":%u", collected);
        check(after.find(want) != std::string::npos,
              "and its header repeats the table total it was read with");
    }
    {
        // A `count` key appears on exactly the rows that have one: a file with no table rows
        // gains no noise, and one with table rows keeps every count.
        int keys = 0;
        for (size_t at = 0; (at = after.find("\"count\":", at)) != std::string::npos; ++at) ++keys;
        check(keys == tableRows,
              "and exactly one `count` key per table row - no noise on the rows without one");
    }
    int lines = 0;
    for (size_t at = 0; (at = after.find("{\"record\":", at)) != std::string::npos; ++at) ++lines;
    check((size_t)lines == n, "one line per entry - NOT ONE WAS LOST");
    int missing = 0;
    for (int i = 0; i < got; ++i) {
        if (after.find(recs[i]) == std::string::npos) ++missing;
    }
    check(missing == 0, "and every record path is still findable in the file, in plain text");
    ut::logFlush();
    printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// 14. THE PAINT GATE - `src\ut_paintgate.h`
// ---------------------------------------------------------------------------------------------
// Without this gate `storeDisplayProtoId` would paint a TABLE-OWNED row (count >= 1) with the
// count as its stack on a mode byte alone, while every path that BALANCES such a paint - the
// deposit, the take, the substitution, restoreOne's table branch, runRescue's table merge - is
// gated on `storeTableOwns()`. Where the two disagreed that is a box nothing can ever decrement.
// The decision now lives in one pure function and this pass is the proof: THE SAME ROW, over a
// writable journal and over a read-only one.
// ---- pass 7: THE DEPOSIT GATE (src\ut_depositgate.h) -----------------------------------------
// The rule, proved without a game: a deposit of one of OUR records either goes into the private
// table or is REFUSED, and there is no third answer. The verdict enum has no "hand it to the
// engine" value, so the first two checks below are the whole of that rule; the rest prove each
// refusal fires for the right reason and, most
// importantly, that EVERY refusal is reached with the journal untouched.
static ut::UtDepositFacts okFacts() {
    ut::UtDepositFacts f;
    f.caller = ut::kUtDepCallerDrag;
    f.bagProof = ut::kUtBagNotAsked;
    f.tableOwns = true;
    f.faulted = false;
    f.mapOk = true;
    f.mapNode = false;
    f.journal = true;
    f.haveCap = true;
    f.capUsable = true;
    return f;
}

static int passDepositGate() {
    printf("\n15. the deposit gate: THE TABLE OR A REFUSAL, never the engine's map\n");
    {
        ut::UtDepositFacts f = okFacts();
        check(ut::utDepositDecide(f) == ut::kUtDepTable,
              "the cursor DRAG, a clean map and a usable capture -> THE TABLE");
        check(!ut::utDepositIsRefusal(ut::utDepositDecide(f)), "and that is not a refusal");

        f.caller = ut::kUtDepCallerExeA;
        f.bagProof = ut::kUtBagYes;
        check(ut::utDepositDecide(f) == ut::kUtDepTable,
              "the bag SHIFT-CLICK with the item proved to be in a bag -> THE TABLE");
    }

    printf("\n15b. every other shape is a REFUSAL - and the journal is never touched\n");
    {
        // The exhaustive sweep. Every combination of the seven flag facts is decided, and the
        // ONLY ones that reach the journal are the two accepted shapes above. If a future edit
        // ever adds a third answer, this loop is what catches it.
        int table = 0, refusals = 0, total = 0;
        for (int caller = 0; caller <= 2; ++caller) {
            for (int bag = -2; bag <= 1; ++bag) {
                for (int bits = 0; bits < 128; ++bits) {
                    ut::UtDepositFacts f;
                    f.caller = caller;
                    f.bagProof = bag;
                    f.mapOk = (bits & 1) != 0;
                    f.mapNode = (bits & 2) != 0;
                    f.journal = (bits & 4) != 0;
                    f.haveCap = (bits & 8) != 0;
                    f.capUsable = (bits & 16) != 0;
                    f.tableOwns = (bits & 32) != 0;
                    f.faulted = (bits & 64) != 0;
                    const int v = ut::utDepositDecide(f);
                    ++total;
                    if (v == ut::kUtDepTable) {
                        ++table;
                        // The conditions of a `true` without the original, restated: a table that
                        // can own the row, a classification that finished, a proved caller, a
                        // proved removal, a readable map with NO row of the engine's own, and a
                        // capture the row can be written from.
                        if (!f.tableOwns || f.faulted ||
                            f.caller == ut::kUtDepCallerUnknown ||
                            (f.caller == ut::kUtDepCallerExeA && f.bagProof != ut::kUtBagYes) ||
                            !f.mapOk || f.mapNode || !f.journal || !f.haveCap || !f.capUsable) {
                            check(false, "a deposit was accepted on facts that forbid it");
                        }
                    } else {
                        ++refusals;
                        if (!ut::utDepositIsRefusal(v)) {
                            check(false, "a non-table verdict did not count as a refusal");
                        }
                    }
                }
            }
        }
        printf("  (%d fact combinations: %d -> the table, %d -> a refusal)\n", total, table,
               refusals);
        check(total == 3 * 4 * 128, "every combination of the nine facts was decided");
        // drag: bag is irrelevant, so 4 bag values x the one accepting bit pattern (mapOk=1,
        // mapNode=0, journal=1, haveCap=1, capUsable=1, tableOwns=1, faulted=0) = 4.
        // site A: only bagProof == kUtBagYes, x the same one bit pattern = 1.
        // unknown caller: never.
        check(table == 5, "and exactly five of them may touch the journal");
        check(refusals == total - 5, "every other one is a refusal, with nothing written");
    }

    printf("\n15c. each refusal names the right reason, in the right order\n");
    {
        ut::UtDepositFacts f = okFacts();
        f.tableOwns = false;
        f.caller = ut::kUtDepCallerUnknown;
        f.mapNode = true;
        check(ut::utDepositDecide(f) == ut::kUtDepRefuseTableOff,
              "a READ-ONLY journal -> REFUSED, and NOT the engine's map "
              "(the whole block is gated on storeTableOwns())");

        f = okFacts();
        f.faulted = true;
        check(ut::utDepositDecide(f) == ut::kUtDepRefuseFaulted,
              "a classification that threw -> REFUSED (the key is cleared, and a cleared key is "
              "not a licence to fall through)");

        f = okFacts();
        f.caller = ut::kUtDepCallerUnknown;
        check(ut::utDepositDecide(f) == ut::kUtDepRefuseCaller,
              "exe site B / the deposit-all button / anything unread -> REFUSED, not the engine");

        f = okFacts();
        f.caller = ut::kUtDepCallerExeA;
        f.bagProof = ut::kUtBagNo;
        check(ut::utDepositDecide(f) == ut::kUtDepRefuseBag,
              "site A with the item in no bag (the equipment shift-click) -> REFUSED");
        f.bagProof = ut::kUtBagCannotAsk;
        check(ut::utDepositDecide(f) == ut::kUtDepRefuseBag,
              "and \"could not ask\" fails CLOSED, exactly like a no");
        f.bagProof = ut::kUtBagNotAsked;
        check(ut::utDepositDecide(f) == ut::kUtDepRefuseBag,
              "and so does \"never asked\" - site A is never accepted on an unanswered proof");

        f = okFacts();
        f.mapOk = false;
        check(ut::utDepositDecide(f) == ut::kUtDepRefuseMapUnreadable,
              "an unreadable reagent map -> REFUSED (the mod cannot tell what the file holds)");

        f = okFacts();
        f.mapNode = true;
        check(ut::utDepositDecide(f) == ut::kUtDepRefuseMapNode,
              "a record reagents.gst STILL has a row for -> REFUSED (run the offline clean-up)");

        f = okFacts();
        f.journal = false;
        check(ut::utDepositDecide(f) == ut::kUtDepRefuseCapture, "journal=0 -> REFUSED");
        f = okFacts();
        f.haveCap = false;
        check(ut::utDepositDecide(f) == ut::kUtDepRefuseCapture, "no replica capture -> REFUSED");
        f = okFacts();
        f.capUsable = false;
        check(ut::utDepositDecide(f) == ut::kUtDepRefuseCapture,
              "a capture with no base-record slot at +0x08 -> REFUSED");
    }

    printf("\n15d. the bag proof is the ONLY thing that separates the two quick-move answers\n");
    {
        // The regression this exists to stop: hundreds of deposits once reached the choke point
        // at exe site A and were sent to the ENGINE because the mod's own sack map had not seen
        // them. There is no longer a verdict that can do that, and the bag proof - which asks the
        // engine about the container `PlayerInventoryCtrl::RemoveItem` actually searches - decides.
        ut::UtDepositFacts f = okFacts();
        f.caller = ut::kUtDepCallerExeA;
        f.bagProof = ut::kUtBagYes;
        check(ut::utDepositDecide(f) == ut::kUtDepTable, "proved in a bag -> the table");
        f.bagProof = ut::kUtBagNo;
        check(ut::utDepositDecide(f) == ut::kUtDepRefuseBag,
              "not in a bag -> a refusal, and NOT the engine's map (this is the shape that once "
              "sent hundreds of items to reagents.gst)");
        // And the drag never consults it at all.
        f.caller = ut::kUtDepCallerDrag;
        for (int bag = -2; bag <= 1; ++bag) {
            f.bagProof = bag;
            check(ut::utDepositDecide(f) == ut::kUtDepTable,
                  "the DRAG is accepted whatever the bag proof says - its source is the cursor "
                  "slot, which the caller clears itself (Game.dll 0x17399A)");
        }
    }

    printf("\n16. the page gate: never substitute a record the active database cannot serve\n");
    {
        ut::UtPageFacts f;
        f.frameArmed = true;
        f.wantedPage = -1;
        f.unavailable = false;
        f.isMaterial = true;
        check(ut::pageAction(f).action == ut::kUtPageSubstitute,
              "live paging armed -> the material record is substituted");
        check(ut::pageAction(f).frame, "and the record is the FRAME page, not a collection page");

        f.isMaterial = false;
        check(ut::pageAction(f).action == ut::kUtPageVanilla,
              "every other record in the game is handed over untouched");

        f.isMaterial = true;
        f.unavailable = true;
        check(ut::pageAction(f).action == ut::kUtPageVanilla,
              "a world whose database has no page records of ours keeps the VANILLA page");
        f.frameArmed = false;
        f.wantedPage = 3;
        check(ut::pageAction(f).action == ut::kUtPageVanilla,
              "and a selected page cannot re-open the substitution in that world either");
        f.unavailable = false;
        check(ut::pageAction(f).action == ut::kUtPageSubstitute,
              "with the records back, page 3 is substituted again");
        check(!ut::pageAction(f).frame, "as the PAGE record - live paging is not armed");
        f.wantedPage = -1;
        check(ut::pageAction(f).action == ut::kUtPageVanilla,
              "and the vanilla page is what -1 means");

        check(ut::pageFallsBack(true, false),
              "a substituted record the engine found no table for -> fall back to the original");
        check(!ut::pageFallsBack(true, true), "a record that loaded is kept");
        check(!ut::pageFallsBack(false, false),
              "a record we never substituted is the engine's own business, loaded or not");

        // The content rule. A record the active database does not hold comes back as a table all
        // the same - an EMPTY one - so the box count is what says whether the page is servable.
        check(!ut::pageTableServable(false, -1), "no table at all is not servable");
        check(!ut::pageTableServable(true, 0),
              "a table with no reagentBoxes is the empty one a missing record produces");
        check(ut::pageTableServable(true, 160), "the frame page's 160 boxes are servable");
        check(ut::pageTableServable(true, 40), "so are a collection page's 40");
        check(ut::pageTableServable(true, -1),
              "a count that could not be read answers YES - the world keeps the tab it had");
        check(ut::pageFallsBack(true, ut::pageTableServable(true, 0)),
              "so an empty table falls back to the vanilla record, where a null test did not");
    }

    ut::logFlush();
    printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}

static int passPaintGate() {
    printf("\n14. the paint gate: a count-1 row, and a journal that cannot own one\n");

    const bool owns = ut::utPaintTableOwns(true, true);
    const bool ownsReadOnly = ut::utPaintTableOwns(false, true);
    check(owns, "a writable journal: the MOD owns the collection");
    check(!ownsReadOnly,
          "a READ-ONLY journal: it does NOT (an unwritable row is a lost item)");

    // The row the whole finding is about: one copy, in the mod's own file, nowhere else.
    ut::UtPaintDecision d = ut::utPaintDecide(true, owns, true, 1, ut::kUtStoredYes, 7);
    check(d.what == ut::kUtPaintFromTable, "count 1 AUTHORISES a paint");
    check(d.stack == 1,
          "and the COUNT is the stack, not the journal's recorded stack (1, not 7)");

    d = ut::utPaintDecide(true, ownsReadOnly, true, 1, ut::kUtStoredYes, 7);
    check(d.what == ut::kUtPaintNothing, "the SAME row over a READ-ONLY journal paints NOTHING");
    check(d.why == ut::kUtPaintWhyTableOff, "and the reason is table-off, not not-stored");
    check(d.stack == 0, "and no stack is offered");

    d = ut::utPaintDecide(true, owns, true, 5, ut::kUtStoredYes, 1);
    check(d.what == ut::kUtPaintFromTable && d.stack == 5, "five copies paint a stack of five");

    printf("\n14b. and a count-0 row paints NOTHING - the map never holds one of ours\n");
    d = ut::utPaintDecide(true, owns, true, 0, ut::kUtStoredYes, 3);
    check(d.what == ut::kUtPaintNothing,
          "count 0 paints nothing even when the mark still says \"stored\":true");
    d = ut::utPaintDecide(true, owns, true, 0, ut::kUtStoredNo, 3);
    check(d.what == ut::kUtPaintNothing && d.why == ut::kUtPaintWhyNotStored,
          "\"stored\":false paints nothing and says so");
    d = ut::utPaintDecide(true, owns, true, 0, ut::kUtStoredUnknown, 3);
    check(d.what == ut::kUtPaintNothing && d.why == ut::kUtPaintWhyUnknown,
          "UNKNOWN paints nothing and says so");
    d = ut::utPaintDecide(false, owns, true, 0, ut::kUtStoredUnknown, 0);
    check(d.what == ut::kUtPaintNothing && d.why == ut::kUtPaintWhyNoEntry,
          "no entry at all paints nothing and says so");

    printf("\n14c. while the MODE is unknown, NOTHING paints - not one record\n");
    const bool ownsNoMode = ut::utPaintTableOwns(true, false);
    check(!ownsNoMode,
          "a writable journal does NOT own the collection while the mode is unknown");
    d = ut::utPaintDecide(true, ownsNoMode, false, 1, ut::kUtStoredYes, 7);
    check(d.what == ut::kUtPaintNothing && d.why == ut::kUtPaintWhyModeUnknown,
          "the very row that painted in 14 paints nothing, and the MODE is the reason");
    check(d.stack == 0, "and no stack is offered");
    d = ut::utPaintDecide(false, ownsNoMode, false, 0, ut::kUtStoredUnknown, 0);
    check(d.why == ut::kUtPaintWhyModeUnknown,
          "a record with no entry says the same - the refusal is not about the row");
    {
        // The whole point of the gate: there is no shape of row that can paint without a mode.
        int painted = 0;
        for (int bits = 0; bits < 32; ++bits) {
            const bool have = (bits & 1) != 0;
            const bool owns2 = (bits & 2) != 0;   // even a table that DOES own the file
            const unsigned int count = (bits & 4) ? 5u : 0u;
            const int stored = (bits & 8) ? ut::kUtStoredYes : ut::kUtStoredNo;
            const unsigned int stack = (bits & 16) ? 3u : 0u;
            if (ut::utPaintDecide(have, owns2, false, count, stored, stack).what !=
                ut::kUtPaintNothing) {
                ++painted;
            }
        }
        check(painted == 0, "all 32 shapes of row paint nothing while the mode is unknown");
    }

    // The three values ut_paintgate.h mirrors out of ut_rescue.h. ut_store.cpp static_asserts
    // them; this harness is where the assertion is READABLE.
    check(ut::kUtStoredYes == ut::UT_STORED_YES && ut::kUtStoredNo == ut::UT_STORED_NO &&
              ut::kUtStoredUnknown == ut::UT_STORED_UNKNOWN,
          "the gate's UT_STORED_* mirror still matches the journal's");

    ut::logFlush();
    printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}

// ---- pass 8: ONE COLLECTION PER MODE ---------------------------------------------------------
// The engine keeps two shared crafting stashes (`reagents.gst` softcore, `reagents.gsh` hardcore)
// and the mod keeps one collection per stash: the softcore trio under its original names,
// and a `-hc` trio beside it in the same folder. This pass drives journalFollowMode() the way a
// world load does - open on mode 0 (the state at start-up, when no character exists and there is
// nobody to ask), write an entry, switch to 1, write another, switch back - and then reads BOTH
// files off the disk to prove each holds its own entry and that each header carries its own
// saveVariant field. There is no engine here: the mode is a number handed in, which is exactly
// the seam is for: the number is GameInfo::GetHardcore's answer, and journalFollowMode neither
// knows nor cares where it came from.
static bool endsWith(const char* s, const char* tail) {
    const size_t n = strlen(s), m = strlen(tail);
    return n >= m && !strcmp(s + n - m, tail);
}

static int passModes() {
    static const char* const kSc = "records/items/mode/softcore_one.dbr";
    static const char* const kHc = "records/items/mode/hardcore_two.dbr";
    if (!ut::journalInit(nullptr)) {
        printf("  [FAIL] journalInit\n");
        return 1;
    }
    printf("\n16. the collection follows the shared stash\n");
    check(ut::journalMode() == 0, "start-up opens on SOFTCORE - no character exists yet");
    check(!ut::journalModeKnown(), "and the mode is NOT known until a world says so");
    check(endsWith(ut::journalPath(), "\\uniq-items.jsonl"), "the softcore journal keeps its name");
    check(endsWith(ut::journalGdsPath(), "\\uniq-export.gds"), "... and so does its .gds");
    check(endsWith(ut::journalCsvPath(), "\\uniq-export.csv"), "... and its .csv");
    const std::string scPath = ut::journalPath();

    // ---- 16b: THE PAINT GATE, in the order the danger arises --------------------------------
    // The file open at start-up is the SOFTCORE one (no character existed when the journal
    // opened) and it already holds rows. A world is live but has not named the variant yet.
    // Until it does, not one of those rows may be painted, counted or deposited into - and the
    // paint is the half that matters, because a take from a painted box is the ENGINE's own code
    // and is over before the mod is told (that is how three items came to exist twice).
    printf("\n16b. the collection is HIDDEN until the world names the variant\n");
    ut::UtReplicaCapture pre;
    fill(&pre, kSc, 1);
    check(ut::journalUpsertCount(pre, 1) && ut::journalFlushNow(),
          "the open file already holds a stored row - count 1, as an old session left it");
    check(countOf(kSc) == 1, "the journal itself still knows that count");
    check(!ut::journalModeKnown(), "and the mode is STILL not known");
    ut::UtPaintDecision g = paintLikeStore(kSc);
    check(g.what == ut::kUtPaintNothing && g.why == ut::kUtPaintWhyModeUnknown,
          "storeDisplayProtoId would answer 0 for it: NO box is painted, so no take can reach it");
    check(ownedRowsLikeStore() == 0, "the owned counters report 0 rows, not the file's 1");
    check(!depositLikeStore(pre, kSc), "a deposit of that record is REFUSED");
    check(countOf(kSc) == 1, "and the refusal left the row exactly as it was");
    check(takeLikeStore(kSc, 1) == 1, "a take cannot move it either (the backstop)");

    check(!ut::journalFollowMode(0), "a softcore world confirms the mode without a switch");
    check(ut::journalModeKnown(), "... and the mode is known from then on");
    g = paintLikeStore(kSc);
    check(g.what == ut::kUtPaintFromTable && g.stack == 1,
          "the SAME row now paints, with the count as its stack - the gate opened, nothing else");
    check(ownedRowsLikeStore() == 1, "and the owned counters report the row");
    ut::UtReplicaCapture a, b;
    fill(&a, kSc, 1);
    check(ut::journalUpsertCount(a, 1) && ut::journalFlushNow(), "a softcore deposit is written");

    check(ut::journalFollowMode(1), "a HARDCORE world switches the collection");
    const std::string hcPath = ut::journalPath();
    check(endsWith(hcPath.c_str(), "\\uniq-items-hc.jsonl"),
          "the hardcore journal has a name of its own");
    check(endsWith(ut::journalGdsPath(), "\\uniq-export-hc.gds"), "... and its own .gds");
    check(endsWith(ut::journalCsvPath(), "\\uniq-export-hc.csv"), "... and its own .csv");
    check(ut::journalCount() == 0, "it starts empty - the softcore entry did not come with it");
    check(!ut::journalHas(kSc), "the softcore record is NOT in the hardcore collection");
    check(ut::journalModeKnown(), "the switch leaves the mode known - the collection stays shown");
    check(paintLikeStore(kSc).why == ut::kUtPaintWhyNoEntry,
          "and the softcore row paints nothing here: it is not in THIS collection at all");
    check(ownedRowsLikeStore() == 0, "the hardcore collection counts 0 rows of its own so far");
    check(ut::journalSaveVariant() == 1, "the header's saveVariant follows the mode");
    fill(&b, kHc, 1);
    check(ut::journalUpsertCount(b, 1) && ut::journalFlushNow(), "a hardcore deposit is written");

    check(ut::journalFollowMode(0), "and back again when a softcore character loads");
    check(std::string(ut::journalPath()) == scPath, "the softcore path is composed again exactly");
    check(ut::journalCount() == 1 && ut::journalHas(kSc), "the softcore entry is still there");
    check(!ut::journalHas(kHc), "and the hardcore one never entered this file");
    check(ut::journalSaveVariant() == 0, "the header's saveVariant is softcore again");

    std::string sc, hc;
    check(slurp(scPath.c_str(), &sc) && slurp(hcPath.c_str(), &hc),
          "both files are on the disk, side by side");
    check(sc.find(kSc) != std::string::npos && sc.find(kHc) == std::string::npos,
          "uniq-items.jsonl holds the softcore entry and nothing else");
    check(hc.find(kHc) != std::string::npos && hc.find(kSc) == std::string::npos,
          "uniq-items-hc.jsonl holds the hardcore entry and nothing else");
    check(sc.find("\"saveVariant\":0") != std::string::npos,
          "the softcore header says saveVariant 0 (the field keeps its name; it holds the mode)");
    check(hc.find("\"saveVariant\":1") != std::string::npos,
          "the hardcore header says saveVariant 1");
    check(!ut::journalFollowMode(-1) && ut::journalMode() == 0,
          "an unreadable mode never moves the collection");
    check(!ut::journalFollowMode(2) && ut::journalMode() == 0,
          "and neither does a mode that is neither softcore nor hardcore");

    printf("\n16c. the world ends and takes the mode with it\n");
    check(paintLikeStore(kSc).what == ut::kUtPaintFromTable, "the softcore row is painting now");
    ut::journalForgetMode();
    check(!ut::journalModeKnown(), "a world teardown makes the mode unknown again");
    check(ut::journalMode() == 0 && endsWith(ut::journalPath(), "\\uniq-items.jsonl"),
          "the open file is untouched by that - only the flag went");
    check(paintLikeStore(kSc).why == ut::kUtPaintWhyModeUnknown,
          "so the collection is HIDDEN again: the next character has not said which mode he is");
    check(ownedRowsLikeStore() == 0, "and the owned counters are back to 0");
    check(!ut::journalFollowMode(0), "the next softcore world confirms the mode, moving no file");
    check(ut::journalModeKnown() && paintLikeStore(kSc).what == ut::kUtPaintFromTable,
          "and the same row paints again, out of the same file");
    ut::logFlush();
    printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}

// ---- pass 9: ONE COLLECTION PER CUSTOM GAME ----------------------------------------------------
// A custom game with a view of its own keeps its own journal set under
// <journal dir>\mods\<name>\, keyed by the custom mod's name so the same mod with and
// without /basemods is one collection; the start-up files never move. This pass drives
// journalFollow() the way the world's first tick does after a view change: rows on both sides
// of a switch made mid-process, then both files read off the disk.
static int passViews() {
    static const char* const kMain = "records/items/view/main_one.dbr";
    static const char* const kCustom = "records/items/view/custom_two.dbr";
    static const char* const kCustomHc = "records/items/view/custom_hc_three.dbr";
    if (!ut::journalInit(nullptr)) {
        printf("  [FAIL] journalInit\n");
        return 1;
    }
    printf("\n17. the collection follows the view\n");
    char key[64];
    check(ut::journalViewKeyFor("vanilla", key, sizeof key) && !key[0] &&
              ut::journalViewKeyFor("basemods", key, sizeof key) && !key[0],
          "the start-up views (vanilla, basemods) have no key - they keep the start-up files");
    check(ut::journalViewKeyFor("custom-lootascension", key, sizeof key) &&
              !strcmp(key, "custom-lootascension"),
          "custom-lootascension keys its journal as custom-lootascension");
    check(ut::journalViewKeyFor("basemods+custom-lootascension", key, sizeof key) &&
              !strcmp(key, "custom-lootascension"),
          "basemods+custom-lootascension keys it the SAME - the custom mod's name, not the view's");
    check(!ut::journalViewKeyFor("custom-..", key, sizeof key) &&
              !ut::journalViewKeyFor("custom-a\\b", key, sizeof key) &&
              !ut::journalViewKeyFor("custom-", key, sizeof key) &&
              !ut::journalViewKeyFor("custom-A b", key, sizeof key),
          "a name that is not a safe folder name gives no key");

    check(!ut::journalFollow(0, ""), "a softcore world in the start-up view confirms the files");
    const std::string mainPath = ut::journalPath();
    check(endsWith(mainPath.c_str(), "\\uniq-items.jsonl") && !strcmp(ut::journalViewKey(), ""),
          "the start-up journal keeps its name and place");
    ut::UtReplicaCapture a, c, h;
    fill(&a, kMain, 1);
    check(ut::journalUpsertCount(a, 1) && ut::journalFlushNow(), "a start-up deposit is written");
    std::string mainBefore;
    check(slurp(mainPath.c_str(), &mainBefore), "the start-up journal is on the disk");

    // the view changed at the database load; the world's first tick moves the files
    check(ut::journalFollow(0, "custom-lootascension"), "a custom game's world switches the collection");
    const std::string viewPath = ut::journalPath();
    check(endsWith(viewPath.c_str(), "\\mods\\lootascension\\uniq-items.jsonl"),
          "its journal is <journal dir>\\mods\\lootascension\\uniq-items.jsonl");
    check(endsWith(ut::journalGdsPath(), "\\mods\\lootascension\\uniq-export.gds") &&
              endsWith(ut::journalCsvPath(), "\\mods\\lootascension\\uniq-export.csv"),
          "... with its own .gds and .csv beside it");
    check(endsWith(ut::rescueReportPath(), "\\mods\\lootascension\\rescue-report.txt"),
          "... and its own rescue-report.txt");
    check(ut::journalCount() == 0 && !ut::journalHas(kMain),
          "it starts empty - the start-up rows did not come with it");
    check(paintLikeStore(kMain).why == ut::kUtPaintWhyNoEntry,
          "a start-up row paints nothing in the custom game");
    fill(&c, kCustom, 1);
    check(depositLikeStore(c, kCustom), "a deposit in the custom game is accepted");
    check(countOf(kCustom) == 1, "... and lands in the custom game's table");
    std::string mainMid;
    check(slurp(mainPath.c_str(), &mainMid) && mainMid == mainBefore,
          "the start-up journal is byte-identical while the custom game is played");

    // the same mod through /basemods is the same collection
    check(ut::journalViewKeyFor("basemods+custom-lootascension", key, sizeof key) &&
              !ut::journalFollow(0, key),
          "the same mod under /basemods moves NO file - it is the same journal");
    check(std::string(ut::journalPath()) == viewPath && ut::journalHas(kCustom),
          "and its row is there");

    // hardcore inside the custom game: the -hc set in the same view folder
    check(ut::journalFollow(1, key), "a hardcore character in the custom game switches the mode");
    check(endsWith(ut::journalPath(), "\\mods\\lootascension\\uniq-items-hc.jsonl"),
          "its hardcore journal is the -hc file in the same view folder");
    fill(&h, kCustomHc, 1);
    check(ut::journalUpsertCount(h, 1) && ut::journalFlushNow(), "a hardcore custom deposit is written");
    check(ut::journalFollow(0, key) && std::string(ut::journalPath()) == viewPath,
          "back to softcore in the custom game");

    // a bad key never becomes a folder and never moves the collection
    check(!ut::journalFollow(0, "custom-../x") && std::string(ut::journalPath()) == viewPath,
          "an unusable key moves nothing");

    // the return: the start-up files, as they were
    check(ut::journalFollow(0, ""), "the main campaign's world switches back");
    check(std::string(ut::journalPath()) == mainPath && !strcmp(ut::journalViewKey(), ""),
          "the start-up path is composed again exactly");
    check(endsWith(ut::rescueReportPath(), "\\rescue-report.txt") &&
              !strstr(ut::rescueReportPath(), "\\mods\\"),
          "and the rescue report is the start-up one again");
    check(ut::journalCount() == 1 && ut::journalHas(kMain) && !ut::journalHas(kCustom),
          "the start-up table holds its own row and nothing of the custom game's");
    check(countOf(kMain) == 1, "with its count untouched");

    std::string mainText, viewText, hcText;
    check(slurp(mainPath.c_str(), &mainText) && slurp(viewPath.c_str(), &viewText),
          "both journals are on the disk");
    const std::string hcPath = viewPath.substr(0, viewPath.size() - 6) + "-hc.jsonl";
    check(slurp(hcPath.c_str(), &hcText), "and the custom game's hardcore one");
    check(mainText == mainBefore, "the start-up journal was never rewritten by the custom game");
    check(mainText.find(kCustom) == std::string::npos && mainText.find(kCustomHc) == std::string::npos,
          "uniq-items.jsonl holds no custom-game row");
    check(viewText.find(kCustom) != std::string::npos && viewText.find(kMain) == std::string::npos &&
              viewText.find(kCustomHc) == std::string::npos,
          "the custom game's journal holds its softcore row and nothing else");
    check(hcText.find(kCustomHc) != std::string::npos && hcText.find(kCustom) == std::string::npos,
          "its -hc journal holds its hardcore row and nothing else");

    // and entering again finds the row where it was left
    check(ut::journalFollow(0, "custom-lootascension") && ut::journalHas(kCustom) &&
              countOf(kCustom) == 1 && !ut::journalHas(kMain),
          "entering the custom game again hands back exactly what its journal holds");
    check(ut::journalFollow(0, ""), "and leaving it once more");
    ut::logFlush();
    printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}

// The catalogue stand-in: a record under /gone/ is one the active database has no record of.
static int fakeInDb(const char* record) { return strstr(record, "/gone/") ? 0 : 1; }

static int passDbViews() {
    static const char* const kMain = "records/items/dbview/main_one.dbr";
    static const char* const kMainGone = "records/items/gone/main_gone.dbr";
    static const char* const kRot = "records/0_rot/items/dbview/rot_one.dbr";
    static const char* const kRotGone = "records/0_rot/items/gone/rot_gone.dbr";
    if (!ut::journalInit(nullptr)) {
        printf("  [FAIL] journalInit\n");
        return 1;
    }
    printf("\n18. the database follows the view: rows on both sides, a record it cannot build\n");
    check(!ut::journalFollow(0, ""), "the start-up world");
    ut::UtReplicaCapture a, b, c, d;
    fill(&a, kMain, 1);
    fill(&b, kMainGone, 1);
    check(ut::journalUpsertCount(a, 2) && ut::journalUpsertCount(b, 1) && ut::journalFlushNow(),
          "the start-up table: a base record and one no database of the view has");
    // The view changes mid-process (a custom game's database came in).
    check(ut::journalFollow(0, "custom-reignofterror"), "Reign of Terror's world switches the collection");
    fill(&c, kRot, 1);
    fill(&d, kRotGone, 1);
    check(ut::journalUpsertCount(c, 1) && ut::journalUpsertCount(d, 3) && ut::journalFlushNow(),
          "its table: a mod record, and one a mod update removed (3 copies)");
    std::vector<ut::UtRescueRow> restore, kept;
    const int n = ut::journalRescuePlan(&fakeInDb, &restore, &kept);
    check(n == 2 && restore.size() == 1 && restore[0].record == kRot && kept.size() == 1 &&
              kept[0].record == kRotGone && kept[0].count == 3,
          "the rescue's plan: the mod record is handed back, the one the database lacks is SKIPPED");
    // The rescue hands back what it may (storeOnTake after the player holds the item).
    for (const ut::UtRescueRow& r : restore) ut::journalSetCount(r.record.c_str(), 0);
    check(ut::journalFlushNow() && countOf(kRot) == 0 && countOf(kRotGone) == 3,
          "after the rescue: the restored row is at 0, the skipped row still holds its 3 - never zeroed");
    check(ut::journalHas(kRotGone) && ut::journalRescuePlan(&fakeInDb, &restore, &kept) == 1 &&
              restore.empty() && kept.size() == 1,
          "a second rescue finds nothing else to do and skips it again");
    fill(&c, kRot, 1);
    check(ut::journalUpsertCount(c, 1) && ut::journalFlushNow() && countOf(kRotGone) == 3,
          "a deposit of another record leaves it alone");
    std::string csv;
    check(slurp(ut::journalCsvPath(), &csv) && csv.find(kRotGone) != std::string::npos,
          "the export still lists it");
    check(ut::journalRescuePlan(nullptr, &restore, &kept) == 2 && restore.size() == 2 && kept.empty(),
          "with no catalogue to ask (unknown) nothing is skipped - the build decides, as before");
    // Back to the main campaign: its rows as they were.
    check(ut::journalFollow(0, ""), "the main campaign's world switches back");
    check(countOf(kMain) == 2 && countOf(kMainGone) == 1 && !ut::journalHas(kRot) &&
              !ut::journalHas(kRotGone),
          "the start-up table holds its own rows, untouched, and nothing of the custom game's");
    check(ut::journalRescuePlan(&fakeInDb, &restore, &kept) == 2 && restore.size() == 1 &&
              restore[0].record == kMain && kept.size() == 1 && kept[0].record == kMainGone,
          "and its plan skips its own unbuildable row the same way");
    check(ut::journalFollow(0, "custom-reignofterror") && countOf(kRotGone) == 3 && countOf(kRot) == 1,
          "entering the custom game again reads its file back: the skipped row still 3");
    check(ut::journalFollow(0, ""), "and leaving it once more");
    ut::logFlush();
    printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}

// ---- 19 MONSTER INFREQUENTS: ONE ROLL PER RECORD (ut_miroll.h) ----------------------------------
// The KIND seam stubbed the way the catalogue provides it: a catalogue entry type with a free
// `catIsMi(entry)` in its own namespace is found by argument-dependent lookup; a type without one
// answers "not an MI" and says so at compile time.
namespace fakecat {
struct MarkedEntry {
    bool mi;
};
inline bool catIsMi(const MarkedEntry& e) { return e.mi; }
struct PlainEntry {
    int unused;
};
}  // namespace fakecat

static const char* const kMiRing = "records/items/gearaccessories/rings/b305c_ring.dbr";
static const char* const kRollAPrefix = "records/items/lootaffixes/prefix/b_ar007_ar_b.dbr";
static const char* const kRollASuffix = "records/items/lootaffixes/suffix/a030b_off_dmg%fire_05_je.dbr";
static const unsigned int kRollASeed = 2115467035u;   // the decoded roll (ut_miroll.h)

// An MI capture in the real layout: base +0x08, prefix +0x28, suffix +0x48, seed +0x68.
static void fillMi(ut::UtReplicaCapture* c, const char* prefix, const char* suffix,
                   unsigned int seed) {
    memset(c, 0, sizeof(*c));
    _snprintf_s(c->record, sizeof(c->record), _TRUNCATE, "%s", kMiRing);
    c->stack = 1;
    c->replicaLen = 0x190;
    const unsigned int one = 1;
    memcpy(c->replica + 0x178, &one, 4);
    memcpy(c->replica + 0x68, &seed, 4);
    c->slotCount = 3;
    c->slotOff[0] = 0x008;
    _snprintf_s(c->slotText[0], sizeof(c->slotText[0]), _TRUNCATE, "%s", kMiRing);
    c->slotOff[1] = 0x028;
    _snprintf_s(c->slotText[1], sizeof(c->slotText[1]), _TRUNCATE, "%s", prefix);
    c->slotOff[2] = 0x048;
    _snprintf_s(c->slotText[2], sizeof(c->slotText[2]), _TRUNCATE, "%s", suffix);
}

// An independent statement of the rule, written from the brief, not from ut_miroll.h.
static int capOracle(int held, int maxPerRecord, int kind) {
    if (kind == 1) return held >= 1 ? ut::kUtCapRefuseMiRoll : ut::kUtCapAccept;
    if (maxPerRecord > 0 && held >= maxPerRecord) return ut::kUtCapRefuseMax;
    if (kind == -1 && held >= 1) return ut::kUtCapRefuseKindUnknown;
    return ut::kUtCapAccept;
}

static int passMi() {
    if (!ut::journalInit(nullptr)) {
        printf("  [FAIL] journalInit\n");
        return 1;
    }
    ut::journalFollowMode(0);
    printf("\n19. monster infrequents: one roll per record\n");

    // (a) the seam
    printf("  this build: catIsMi(entry) is %s (the KIND mark; absent = no MI anywhere)\n",
           ut::journalKindMarkBuilt() ? "DECLARED" : "not declared yet");
    const fakecat::MarkedEntry yes = {true}, no = {false};
    const fakecat::PlainEntry plain = {0};
    check(ut::utCatHasKindOf<fakecat::MarkedEntry>() && ut::utCatIsMiOf(yes) && !ut::utCatIsMiOf(no),
          "a catalogue entry with catIsMi(entry) is read through the seam (ADL)");
    check(!ut::utCatHasKindOf<fakecat::PlainEntry>() && !ut::utCatIsMiOf(plain),
          "an entry type with no catIsMi is never an MI");
    check(ut::journalKindMarkBuilt() || ut::journalRecordIsMi(kMiRing, true) == 0,
          "without the KIND mark journalRecordIsMi answers 0 for every record");

    // (b) the decision, every combination against the independent oracle
    int combos = 0, agree = 0;
    for (int held = 0; held <= 4; ++held) {
        for (int mx = 0; mx <= 4; ++mx) {
            for (int kind = -1; kind <= 1; ++kind) {
                ++combos;
                if (ut::utRecordCapDecide(held, mx, kind) == capOracle(held, mx, kind)) ++agree;
            }
        }
    }
    char line[256];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "all %d (held, max_per_record, kind) cases agree "
                "with the rule as the brief states it (%d)", combos, agree);
    check(agree == combos, line);
    check(ut::utRecordCapDecide(1, 1, 1) == ut::kUtCapRefuseMiRoll,
          "an MI at max_per_record=1 is refused with the ROLL reason, not the max one");
    check(ut::utRecordCapDecide(1, 2, 1) == ut::kUtCapRefuseMiRoll &&
              ut::utRecordCapDecide(3, 0, 1) == ut::kUtCapRefuseMiRoll,
          "an MI is refused at max_per_record=2 and at 0 (unlimited) alike");
    check(ut::utRecordCapDecide(0, 2, 1) == ut::kUtCapAccept,
          "an empty MI box takes its first roll");
    check(ut::utRecordCapDecide(1, 2, 0) == ut::kUtCapAccept &&
              ut::utRecordCapDecide(2, 2, 0) == ut::kUtCapRefuseMax,
          "a unique keeps max_per_record exactly as before");
    check(ut::utRecordCapDecide(1, 2, -1) == ut::kUtCapRefuseKindUnknown &&
              ut::utRecordCapDecide(1, 1, -1) == ut::kUtCapRefuseMax &&
              ut::utRecordCapDecide(0, 2, -1) == ut::kUtCapAccept,
          "an unknown kind refuses only a second copy max_per_record would have allowed");

    // (c) the rolls
    ut::UtReplicaCapture a, b;
    fillMi(&a, kRollAPrefix, kRollASuffix, kRollASeed);
    fillMi(&b, "records/items/lootaffixes/prefix/aa010a_damod_01.dbr",
           "records/items/lootaffixes/suffix/b_ar044_ar_b.dbr", 1621750206u);
    ut::UtRollKey ra, rb, ra2;
    ut::utRollFromSlots(a.slotOff, a.slotText, a.slotCount, a.replica, a.replicaLen, &ra);
    ut::utRollFromSlots(b.slotOff, b.slotText, b.slotCount, b.replica, b.replicaLen, &rb);
    ut::UtReplicaCapture a2 = a;
    for (char* p = a2.slotText[1]; *p; ++p) *p = (char)toupper(*p);
    ut::utRollFromSlots(a2.slotOff, a2.slotText, a2.slotCount, a2.replica, a2.replicaLen, &ra2);
    check(ra.known && !strcmp(ra.prefix, kRollAPrefix) && !strcmp(ra.suffix, kRollASuffix) &&
              ra.seed == kRollASeed,
          "a roll is {prefix +0x28, suffix +0x48, seed +0x68}");
    check(!ut::utRollSame(ra, rb) && ut::utRollSame(ra, ra2),
          "two rolls differ by any of the three; the record case does not matter");
    ut::UtRollKey seedOnly = ra;
    seedOnly.seed ^= 1;
    check(!ut::utRollSame(ra, seedOnly), "the same affixes with another seed are another roll");
    ut::UtReplicaCapture straddle = a;
    straddle.slotOff[2] = 0x058;   // a string window over +0x68: not this layout
    ut::UtRollKey rs;
    check(!ut::utRollFromSlots(straddle.slotOff, straddle.slotText, straddle.slotCount,
                               straddle.replica, straddle.replicaLen, &rs) && !rs.known &&
              !ut::utRollSame(rs, rs),
          "a blob whose string window covers +0x68 has no known roll (never 'the same')");
    ut::utRollDescribe(ra, "Chthonian Devourer", 55, line, sizeof(line));
    printf("      %s\n", line);
    check(!strcmp(line, "\"Chthonian Devourer\" level 55: prefix b_ar007_ar_b, suffix "
                        "a030b_off_dmg%fire_05_je, seed 2115467035"),
          "the roll in words: name, level, prefix, suffix, seed");
    ut::UtRollKey bare = ra;
    bare.prefix[0] = 0;
    bare.suffix[0] = 0;
    ut::utRollDescribe(bare, "", 0, line, sizeof(line));
    check(!strcmp(line, "no prefix, no suffix, seed 2115467035"), "an affix-less roll says so");

    // (d) the table: roll A deposited, roll B refused with roll A named, roll A still there
    check(depositLikeStore(a, kMiRing) && countOf(kMiRing) == 1, "roll A goes into an empty MI box");
    const int verdict = ut::utRecordCapDecide((int)countOf(kMiRing), 2, 1);
    check(verdict == ut::kUtCapRefuseMiRoll, "roll B of the same record is refused (max_per_record=2)");
    char reason[256];
    check(ut::journalRollText(kMiRing, reason, sizeof(reason)), "journalRollText reads the stored roll");
    printf("      the refusal names: %s\n", reason);
    check(strstr(reason, "b_ar007_ar_b") && strstr(reason, "a030b_off_dmg%fire_05_je") &&
              strstr(reason, "seed 2115467035"),
          "the refusal reason names roll A: its prefix, suffix and seed");
    ut::UtRollKey stored;
    check(ut::journalRollOf(kMiRing, &stored) && ut::utRollSame(stored, ra) && countOf(kMiRing) == 1,
          "after the refusal the box still holds roll A, one copy");

    // (e) THE BELT: journalDepositCommit itself refuses a second copy of a known MI - nothing
    // changes in memory or on disk. Without it (the kind unknown to the belt) the second copy
    // REPLACES roll A: the loss the cap exists for, shown on purpose.
    std::string before, after;
    slurp(ut::journalPath(), &before);
    ut::journalTestSetKind(1);
    bool rolledBack = true;
    const bool belt = ut::journalDepositCommit(b, 2, &rolledBack);
    slurp(ut::journalPath(), &after);
    check(!belt && !rolledBack, "journalDepositCommit refuses count 2 of an MI before touching anything");
    check(ut::journalRollOf(kMiRing, &stored) && ut::utRollSame(stored, ra) &&
              countOf(kMiRing) == 1 && before == after,
          "roll A, count 1 and the file are all exactly as they were");
    check(ut::journalSetCount(kMiRing, 0) && ut::journalDepositCommit(b, 1, &rolledBack),
          "after a take empties the box (count 0), its count-1 commit of roll B is allowed");
    check(ut::journalRollOf(kMiRing, &stored) && ut::utRollSame(stored, rb) && countOf(kMiRing) == 1,
          "and the box then holds roll B - roll A went back to the player with the take");
    check(ut::journalSetCount(kMiRing, 0) && ut::journalDepositCommit(a, 1, &rolledBack),
          "roll A back in the same way, for the last check");
    ut::journalTestSetKind(0);
    check(ut::journalDepositCommit(b, 2, &rolledBack) && countOf(kMiRing) == 2 &&
              ut::journalRollOf(kMiRing, &stored) && ut::utRollSame(stored, rb),
          "WITHOUT the cap: count 2 holding roll B only - roll A is gone (why the cap exists)");
    ut::journalTestSetKind(-2);
    ut::logFlush();
    printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}

int main(int argc, char** argv) {
    // An ABSOLUTE path, the way tools\test_journal.cpp does it. A relative `L"test_store.log"`
    // would resolve against the PROCESS's current directory, which is whatever shell called the
    // .bat - so running `tools\build_test_store.bat` by full path from the repo root would drop
    // test_store.log and its dated archives into the REPO ROOT. Nothing else about the harness
    // reaches outside build\test, and neither does its log.
    wchar_t log[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, log)) log[0] = 0;
    wcscat_s(log, MAX_PATH, L"uniq-store-test.log");
    ut::logInit(log);
    const char* mode = argc > 1 ? argv[1] : "";
    printf("== test_store: the private table's arithmetic (%s) ==\n", *mode ? mode : "main pass");
    if (!strcmp(mode, "--paint")) return passPaintGate();
    if (!strcmp(mode, "--deposit")) return passDepositGate();
    if (!strcmp(mode, "--reread")) return passReread();
    if (!strcmp(mode, "--legacy")) return passLegacyFile(argc > 2 ? argv[2] : "uniq-items.jsonl");
    if (!strcmp(mode, "--live")) return passLive();
    if (!strcmp(mode, "--modes")) return passModes();
    if (!strcmp(mode, "--views")) return passViews();
    if (!strcmp(mode, "--dbviews")) return passDbViews();
    if (!strcmp(mode, "--mi")) return passMi();
    return passMain();
}
