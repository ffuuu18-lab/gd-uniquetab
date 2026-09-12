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
//   12 THE REHEARSAL                        -> a COPY of the user's own live journal
//   14 THE PAINT GATE (ut_paintgate.h)      -> a count-1 row paints from a writable journal and
//                                              NOT from a read-only one
//   15 THE DEPOSIT GATE (ut_depositgate.h)  -> a deposit of one of our records goes into
//                                              the TABLE or is REFUSED, and there is no third
//                                              answer. All 768 fact combinations
//                                              are decided; exactly 5 of them may touch the
//                                              journal, so every refusal leaves it untouched
//   16 THE PAGE GATE (ut_pagegate.h)       -> the material record is substituted only while the
//                                              active database can serve the page it is
//                                              substituted for; a world that cannot keeps the
//                                              vanilla page for the rest of its life
//
// Build + run:  tools\build_test_store.bat   (everything happens under build\test)
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
    // empty. This is the one that keeps the user's 537 existing rows out of the table.
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
    // one nobody has reconciled. It is what the user's own file looks like.
    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s", what);
    std::string t;
    t.append("{\"journal\":\"grim dawn uniquetab\",\"format\":3,\"written\":\"2026-09-11T00:00:00Z\""
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

// ---- pass 4: THE REHEARSAL - a COPY of the user's own live journal ---------------------------
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
    printf("\n12. THE REHEARSAL: the user's own journal, %zu entries\n", n);
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
    return passMain();
}
