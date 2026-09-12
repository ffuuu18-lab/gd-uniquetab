// ut_store.cpp - the private table: its display side (the mod-owned prototypes a collection box
// is painted with) and its owning side (the counts). See ut_store.h for what this file is for.
#include "ut_store.h"

#include <stdio.h>
#include <string.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "ut_config.h"
#include "ut_live.h"
#include "ut_paintgate.h"
#include "ut_log.h"
#include "ut_plate.h"
#include "ut_reagent.h"
#include "ut_rescue.h"

namespace ut {
namespace {

// ---------------------------------------------------------------------------------------------
// 3. THE PRIVATE TABLE, DISPLAY SIDE - and what ReagentWindow::Sync does about it
// ---------------------------------------------------------------------------------------------
// THE LOAD-BEARING FACT, READ OFF THE LOADED EXE IMAGE:
//
//   00132655  cmp  rbx, rdi            ; rbx = the node found for this box, rdi = the map's end
//   00132658  je   0x132676            ; NODE-LESS BOX -> skip, next box. Nothing is written.
//   0013265A  mov  edx, [rbx + 0x40]   ; ReagentData::protoId
//   0013265D  mov  rcx, [rsi]          ; the box object
//   00132660  cmp  [rcx + 0x30], edx   ; the id the box is CURRENTLY showing vs the node's
//   00132663  je   0x132676            ; already right -> skip
//   00132665  mov  rax, [rcx]
//   00132668  mov  r8b, 1
//   0013266B  call [rax + 0xa8]        ; box->SetItem(node->protoId, true)
//
// So the +0x30 compare at 0x132660 answers the question in the plainest possible way: **for a record that still HAS a map node, Sync RE-POINTS the box back at the map's
// prototype.** It does not merely tolerate the mod's SetItem - it undoes it, on the very next
// SyncCaravanReagents, which the mod itself calls after every relayout and which the engine calls
// after every deposit and take.
//
// THIS IS NOT FOUGHT. Once a record is table-owned it is not in the map at all, and then
// 0x132655 - not 0x132660 - is the instruction that decides, and it skips the box entirely.
// What the display side guarantees for a MAP-OWNED record is therefore exactly this and no more:
//
//   * the mod can build a full identity prototype for a stored record out of its OWN file, with
//     no engine prototype to copy from, and the box accepts it;
//   * and the parity line says whether that prototype is the same identity the map is holding.
//
// The box may be re-pointed at the map's prototype again a frame later, and that is CORRECT while
// the map is still authoritative. The parity number is what carries the proof, not the pixel.
const int kProtoCap = 320;  // mod-owned prototypes per world; the display cache's own bound idea

std::unordered_map<std::string, unsigned int>* g_protoTable = nullptr;  // record -> mod-owned id
volatile LONG g_protoBuilt = 0;
volatile LONG g_protoRefused = 0;
// RE-builds of a record the cache had already built. A deposit and a
// take both drop the cached prototype on purpose - the stack has changed and the next relayout
// must build it again at the new count - but that rebuild is CHURN on a record the cap has
// already paid for, not new COVERAGE, and the cap exists to bound coverage (one mod-owned Item
// per record) rather than to punish a player for using the page. It is bounded by the player's
// own clicks, one per deposit or take, never by a frame loop: the thing the cap was written to
// stop (a per-frame retry of an unbuildable record) still counts in full,
// because a REFUSAL is cached as 0 and a cached 0 is never churned.
volatile LONG g_protoChurn = 0;
volatile LONG g_protoCapLogged = 0;
volatile LONG g_takeoverDone = 0;  // the takeover census, once per world
volatile LONG g_refuseLogged = 0;
CRITICAL_SECTION g_dcs;
bool g_dcsReady = false;

struct DGuard {
    DGuard() { if (g_dcsReady) EnterCriticalSection(&g_dcs); }
    ~DGuard() { if (g_dcsReady) LeaveCriticalSection(&g_dcs); }
};

// THE OTHER WAY THE PARITY LINE STAYS SILENT, and it is not a bug in the
// substitution at all: `storeNoteDisplayBox` is what arms the report, and it is only reached for
// a box the private table actually painted. If not one record on the page is marked
// `"stored":true` in the journal - the vanilla materials page (group -1), a group that holds none
// of the collection, or the owned-only filter down to a single box in another group - then a
// perfectly healthy relayout arms nothing and the log says NOTHING AT ALL. That is precisely what
// a user reading "it worked but I see no line" is looking at. So: whenever a relayout is asked
// for, start a countdown, and if it runs out with no box painted, say why.

// THE SILENCE MUST NEVER BE REPEATABLE. A relayout of a page holding a stored item can log
// NOTHING - no parity line, no refusal, no cap line - when `storeDisplayProtoId` bows out on a
// line that has no log of its own (for instance when it is handed a BOX record instead of the
// item record). Every path out of `storeDisplayProtoId` that returns 0 for a non-empty record
// therefore names a REASON - one line the first time EACH REASON is met in a world - and the
// counts go into both the parity report and the "NOTHING TO COMPARE" watch, so the user's own
// log answers "why did nothing paint?" without another session.
enum BowReason {
    kBowNotReady = 0,   // the display table never came up
    kBowNoEntry,        // the journal has no entry for this record at all
    kBowNotStored,      // an entry that says "stored":false
    kBowUnknown,        // an entry not yet reconciled against the page
    kBowCached,         // a refusal cached earlier in this world (already explained)
    kBowCap,            // the per-world prototype cap
    kBowFault,          // a C++ exception inside the lookup (the try/catch at the bottom)
    kBowTableOff,       // count >= 1, but the mod does not own the collection this session
    kBowModeUnknown,    // which of the two collections is this character's is not known
    kBowCount
};
volatile LONG g_bow[kBowCount] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
// ONE explanatory line PER REASON per world, and each one NAMES THE RECORD IT WAS ASKED ABOUT.
// That last part is the whole point: a line reading `... did NOT paint
// records/ui/caravan/reagents/uniq/p55/box_02.dbr - the journal has NO entry for that record`
// makes a wrong-record bug visible in the user's own log without another session.
// A budget of one line TOTAL would not do: on a real page the first bow-out is always a record
// the player has never collected, so the interesting reasons would never get to speak.
volatile LONG g_bowLogged[kBowCount] = {0, 0, 0, 0, 0, 0, 0, 0, 0};

const char* bowText(int why) {
    switch (why) {
        case kBowNotReady:
            return "the private table's own state never came up (storeInit's critical section or "
                   "its map could not be allocated) - nothing can be painted this session";
        case kBowNoEntry:
            return "the journal has NO entry for that record - the mod has never seen that item "
                   "deposited, so there is no identity to paint";
        case kBowNotStored:
            return "the journal entry says \"stored\":false - the item was taken back out, so "
                   "painting it would show an item you do not own";
        case kBowUnknown:
            return "the journal entry is still UNKNOWN - it has not been reconciled against the "
                   "page yet (that happens when the caravan opens), so it is not painted until it "
                   "is proved stored";
        case kBowCached:
            return "a refusal for that record was already cached in this world (the reason was "
                   "logged when it was first refused; it is not retried until the world reloads)";
        case kBowCap:
            return "the per-world prototype cap has been reached";
        case kBowFault:
            return "the lookup threw (an allocation failure in the private table) - the box keeps "
                   "the ordinary display prototype and nothing of yours is affected";
        case kBowTableOff:
            // See ut_paintgate.h: a row the mod's own file
            // owns may only be painted while the mod OWNS the collection, because the accounting
            // that balances the paint (the deposit, the take, the rescue) needs a table that can
            // own something.
            return "that item lives in the mod's OWN file (\"count\":1 or more) but the table "
                   "cannot own anything this session (uniq-items.jsonl is read-only, or the mod "
                   "has no path to it) - so nothing could decrement the box if you took from it, "
                   "and the mod will not paint an item it cannot account for. The item is safe in "
                   "uniq-items.jsonl either way";
        case kBowModeUnknown:
            // This is THE safety refusal - it is not about one row. See
            // ut_paintgate.h: the mod holds one collection per mode and does not know
            // which is this character's until a live world has told it.
            return "the mod does not know yet whether this character is HARDCORE or SOFTCORE "
                   "(GameInfo::GetHardcore has not been read), and there is one "
                   "collection per mode - so NOTHING of the collection is painted at all. A box "
                   "the mod paints can be taken from by the game itself, before the mod is asked, "
                   "so painting the wrong mode's collection would hand over items the mod could "
                   "not then account for. It clears itself the moment a character is in the world";
        default:
            return "?";
    }
}

// One line the first time each reason is met in a world. It names the RECORD and the reason, so
// the user never has to guess which of the reasons it was - or which string the lookup was
// even given.
void bowNote(int why, const char* record) {
    if (why < 0 || why >= (int)kBowCount) return;
    InterlockedIncrement(&g_bow[why]);
    if (InterlockedExchange(&g_bowLogged[why], 1)) return;
    logD("store: the private table did NOT paint %s - %s. (First time for this reason in this "
         "world; the rest are counted, not logged, and every count appears in the parity report "
         "or in the NOTHING TO COMPARE line. \"no-entry\" for a record you have never collected "
         "is the ordinary case and is not a fault.)",
         record && *record ? record : "(no record)", bowText(why));
}
// ---------------------------------------------------------------------------------------------
// 4. RUNTIME FLIPS OF THE EXPORT KEYS
// ---------------------------------------------------------------------------------------------
// The ini is re-read once a second, so a key can change under a running world - and the log must
// say so when one does, or a user cannot tell from the log which mode wrote the file they open.
//
// So both export modes are watched against a mod-owned last-seen copy, with ONE line per move
// naming the mode in words and which file is about to be rewritten. This is safe from here:
// `storeTick(true)` is the `Engine::PresentSurface` game-thread tick, the same thread `liveTick`
// consumes a relayout request on.
volatile LONG g_lastGdsMode = -1;
volatile LONG g_lastCsvMode = -1;

// The clamp journalSetCsvExport / journalSetGdsExport apply, repeated here so the flip watcher
// compares what the export actually DID against what it did last time. Without it `export_csv=5`
// would read as a change from 2 on every single tick and log a line a second.
LONG exportModeClamp(int v) {
    return v < 0 ? 0 : (v > 2 ? 2 : (LONG)v);
}

const char* exportModeWords(LONG mode) {
    return mode >= 2 ? "EVERY journal entry, the not-stored history included"
                     : mode == 1 ? "THE COLLECTION - the entries that are stored, plus any not "
                                   "yet reconciled against the page"
                                 : "OFF";
}

// One line per move of one export key. `path` may be empty (journalInit has not run), which is
// why the file is named from the key rather than from the path when there is none.
void exportFlipLine(const char* key, const char* file, const char* path, LONG prev, LONG now) {
    if (now > 0) {
        logI("store export: %s %d -> %d - %s is rewritten from the journal within a second (%s)",
             key, (int)prev, (int)now, file, exportModeWords(now));
        logD("an export preference never touches uniq-items.jsonl%s%s",
             path && path[0] ? "; the file is " : "", path && path[0] ? path : "");
        return;
    }
    logI("store export: %s %d -> 0 - %s is not written again this session", key, (int)prev, file);
    logD("the file that is already there is left where it is; the mod never deletes an export%s%s",
         path && path[0] ? " - it is still at " : "", path && path[0] ? path : "");
}

void configFlipTick() {
    // ---- export_gds / export_csv ------------------------------------------------------
    // These two are FIRST because the store_probe block below returns out of the function.
    // Neither writes anything here: journalSet*Export has already been called by storeTick on
    // this same tick and has armed the export-only pass, so all that is left to do is SAY so.
    {
        const LONG g = exportModeClamp(g_cfg.exportGds);
        const LONG gPrev = InterlockedExchange(&g_lastGdsMode, g);
        if (gPrev >= 0 && gPrev != g) {
            exportFlipLine("export_gds", "uniq-export.gds", journalGdsPath(), gPrev, g);
        }
        const LONG c = exportModeClamp(g_cfg.exportCsv);
        const LONG cPrev = InterlockedExchange(&g_lastCsvMode, c);
        if (cPrev >= 0 && cPrev != c) {
            exportFlipLine("export_csv", "uniq-export.csv", journalCsvPath(), cPrev, c);
        }
    }
}

char g_status[240] = {0};

// ---------------------------------------------------------------------------------------------
// 5. THE PRIVATE TABLE'S OWN STATE, AND THE DERIVED CAP
// ---------------------------------------------------------------------------------------------
// THERE IS NO SECOND TABLE. The journal's entry list IS the table: `count` on the entry is the
// row and `uniq-items.jsonl` is the authority (a second in-memory copy of the counts would be a
// second authority, and two authorities drift). What lives here is only what is per WORLD: the
// prototype ids in `g_protoTable` above, and the derived cap below.

// `kProtoCap = 320` is too small for a large collection (~500 records): browsing every category
// in one world would hit the cap and stop painting owned boxes. The cap is derived instead, with
// no new key:
//
//     cap = max(320, paintable * 2 + 64)
//
// and `paintable` is the number of rows that could ASK for a prototype in this world - every row
// the journal marks stored plus every row it has not reconciled yet, NOT the rows with
// count >= 1: that number is the TABLE's rows, which is 0 until something is deposited, and the
// boxes that need a prototype over an older journal are the MAP-OWNED ones. Refusals still count
// against it: that is what stops an unbuildable record leaking one Item per frame.
volatile LONG g_protoCapCache = 0;   // 0 = not derived in this world yet

// The two numbers `storeStatus()` prints, published from the GAME thread so the worker's
// heartbeat never has to take the journal's lock to read them (see storeStatus).
volatile LONG g_tableRows = 0;
volatile LONG g_tableCopies = 0;
volatile LONG g_tablePublishTick = 0;

// Refreshes the pair. Takes g_cs, so: game thread, no lock of this file's held, and never from a
// frame that swallows. `force` is for the moments that change it (a deposit, a take, start-up);
// otherwise it costs one interlocked increment per tick and walks the journal every ~2 seconds.
void tablePublish(bool force) {
    if (!force && (InterlockedIncrement(&g_tablePublishTick) % 120) != 0) return;
    InterlockedExchange(&g_tableRows, (LONG)journalCollectStored(nullptr, nullptr, 0));
    InterlockedExchange(&g_tableCopies, (LONG)journalCollectedTotal());
}

int protoCapNow() {
    const LONG cached = InterlockedCompareExchange(&g_protoCapCache, 0, 0);
    if (cached > 0) return (int)cached;
    size_t entries = 0, stored = 0, notStored = 0, unknown = 0;
    journalCounts(&entries, &stored, &notStored, &unknown);
    const size_t paintable = stored + unknown;
    size_t cap = paintable * 2 + 64;
    if (cap < (size_t)kProtoCap) cap = (size_t)kProtoCap;
    if (cap > 4096) cap = 4096;   // a mod-owned Item per record; 4096 is far past any collection
    InterlockedExchange(&g_protoCapCache, (LONG)cap);
    if (cap != (size_t)kProtoCap) {
        logD("store: the private table's per-world prototype cap is %zu for this journal (%zu "
             "rows could ask for a prototype: %zu stored, %zu not yet reconciled). The fixed 320 "
             "would have stopped painting owned boxes part-way through a large "
             "collection. Refusals still count against it.",
             cap, paintable, stored, unknown);
    }
    return (int)cap;
}

// A deposit and a take both change the stack the display prototype must carry, so the cached one
// has to go and the next relayout builds it again at the new count. (The old object is left to
// the world, like every prototype in ut_live's cache.)
//
// The drop is COUNTED. Without this the per-world cap measured churn
// rather than coverage - a session of deposits and takes on a handful of records could spend a
// 1,138-prototype cap without ever covering 1,138 records, and the page would then stop painting
// owned boxes with one line. A record whose cached value is a REFUSAL (0) is not counted: that
// is precisely the case the cap exists to bound.
void protoDropForRestack(const char* record) {
    if (!record || !*record || !g_dcsReady) return;
    try {
        DGuard g;
        if (!g_protoTable) return;
        std::unordered_map<std::string, unsigned int>::iterator it = g_protoTable->find(record);
        if (it == g_protoTable->end()) return;
        if (it->second) InterlockedIncrement(&g_protoChurn);
        g_protoTable->erase(it);
    } catch (...) {
    }
}

}  // namespace

void storeInit() {
    if (!g_dcsReady) {
        InitializeCriticalSection(&g_dcs);
        g_dcsReady = true;
        try {
            g_protoTable = new std::unordered_map<std::string, unsigned int>();
        } catch (...) {
            g_protoTable = nullptr;
        }
    }
    logI("store: armed - export_gds=%d export_csv=%d", g_cfg.exportGds, g_cfg.exportCsv);
    // The table's own start-up line, kept separate from the "armed" line above so that line
    // stays word for word what it is at the defaults.
    logD("store: the private table %s. Journal format %d; it holds %u cop%s right now. "
         "count >= 1 is the only authorisation to paint a box from the table, to take from it or "
         "to rescue out of it.",
         storeTableOwns()
             ? "OWNS the collection: every deposit is written into the mod's own file and that "
               "file is on disk BEFORE the deposit is accepted"
             : "CANNOT OWN ANYTHING THIS SESSION - the journal is read-only or has no path, so "
               "every deposit is refused",
         (int)UT_JOURNAL_FORMAT, journalCollectedTotal(),
         journalCollectedTotal() == 1 ? "y" : "ies");
    tablePublish(true);
    // The flip watchers start from what the ini says NOW, so the start-up line above is the "no
    // flip" statement and the first logged flip is a real one.
    // The same for the two export MODES, so the first flip line the user ever sees is a real flip and not the start-up value repeated. Clamped the way
    // journalSet*Export clamps, so `export_csv=7` in the ini cannot log a flip every second.
    InterlockedExchange(&g_lastGdsMode, exportModeClamp(g_cfg.exportGds));
    InterlockedExchange(&g_lastCsvMode, exportModeClamp(g_cfg.exportCsv));
    // The MODE goes in, never `g_cfg.exportCsv != 0`: ut_rescue.h deletes the bool overload, so
    // such a call is a compile error, not an export_csv=2 that silently behaves like 1. The GD
    // Stash export is pushed in exactly as the CSV mode is (ut_rescue.cpp links without
    // ut_config.cpp on purpose), and announced in the same shape.
    journalSetGdsExport(g_cfg.exportGds);
    if (g_cfg.exportGds > 0 && journalGdsPath()[0]) {
        logD("store gds: uniq-export.gds will be written to \"%s\" on every journal write "
             "(export_gds=%d = %s). It is a GD STASH IMPORT FILE - open GD Stash, go to the "
             "\"Im- / Export\" tab and press \"Load GD Stash file\" in the \"Import items\" "
             "box - and nothing is ever read back out of it; the mod never touches GD Stash's "
             "own database",
             journalGdsPath(), g_cfg.exportGds,
             g_cfg.exportGds >= 2
                 ? "EVERY journal entry, the not-stored history included"
                 : "THE COLLECTION - the entries that are stored, plus any not yet reconciled "
                   "against the page; the not-stored history is left out (export_gds=2 for all "
                   "of it)");
    }
    journalSetCsvExport(g_cfg.exportCsv);
    if (g_cfg.exportCsv > 0 && journalCsvPath()[0]) {
        // The mode in WORDS: the user reads this line to find out what they are about to open,
        // and "export_csv=1" alone does not say why the file can hold more rows than the
        // collection.
        logD("store csv: uniq-export.csv will be written to \"%s\" on every journal write "
             "(export_csv=%d = %s; one RFC-4180 row per exported entry, 14 columns, stored ones "
             "first; nothing ever reads it back)",
             journalCsvPath(), g_cfg.exportCsv,
             g_cfg.exportCsv >= 2
                 ? "EVERY journal entry, the not-stored history included"
                 : "THE COLLECTION - the entries that are stored, plus any not yet reconciled "
                   "against the page; the not-stored history is left out (export_csv=2 for all "
                   "of it)");
    }
}

// ---------------------------------------------------------------------------------------------
// THE TAKEOVER CENSUS
// ---------------------------------------------------------------------------------------------
// THE RULE: the engine's `reagents.gst` is foreign territory - nothing of ours goes in and
// nothing of ours should be in it. That is a claim about a file, so it gets checked out loud,
// once per world, the first time the map can be walked: **"reagents.gst holds N rows of ours"**.
// It must read 0, and any N > 0 is a DEFECT line in capitals naming the records - a stale Steam
// Cloud copy, a save restored from a backup, or rows an older build left behind are all ways for
// it to come back.
//
// It never changes anything. It is one in-order walk of the reagent map (`reagentStoreCensusEx`,
// bounded by the NODE COUNT), the caravan has to be open for the map
// read, and it runs ONCE per world.
//
// THE TABLE WINS - FOR COUNTING, AND FOR COUNTING ONLY. A record that is table-owned
// (`storeCount >= 1`) AND still in the map is named separately: `heldSumFrom` counts the table's
// copy and only the table's copy, and the map's row is never counted and never taken from.
//
// THE TABLE WINS is true for counting and FALSE on screen: the BOX for such a record is NOT the
// table's. `ReagentWindow::Sync` re-points a box whose record
// still has a map node at exe 0x13266B, so whatever the mod painted, the ENGINE's prototype is
// what the user ends up looking at. That is not a bug to fix on the paint side - it is why a
// deposit into such a record is REFUSED, and it is the honest reason the row has to go. So these
// lines print BOTH numbers (`table=N map=M`, both already collected here) and say the box is the
// game's until the row is gone. The row is NOT deleted by the mod - deleting engine data is
// exactly what this mod does not do - so the line says how to get rid of it: take the item out of
// the Crafting Materials page yourself.
void takeoverCensusTick() {
    if (!storeTableOwns()) return;
    if (InterlockedCompareExchange(&g_takeoverDone, 0, 0)) return;
    if (!reagentTransferOpen()) return;   // the map read needs the caravan, like migrateCheck
    // These are ~154 KB, and they are static on purpose. Not stack
    // (150 KB would blow a game-thread frame) and not heap (no allocation on a detour-reachable
    // tick). Safe because this runs on the game thread only (`storeTick` returns early for the
    // worker) and the fill and the read are one straight line inside a single call - and it
    // reports at most once per world, `g_takeoverDone` latching it. So the statics cannot race,
    // cannot be re-entered, and nothing outside this function ever looks at them.
    static UtStoreCensus c;
    static char ours[600][256];
    static int held[600];
    int copied = 0;
    memset(&c, 0, sizeof(c));
    if (!reagentStoreCensusEx(&c, nullptr, 0, ours, held, 600, &copied)) {
        // Inconclusive: say nothing and try again on the next tick. A census that cannot read the
        // map must never be reported as "clean".
        return;
    }
    InterlockedExchange(&g_takeoverDone, 1);
    if (c.rows <= 0) {
        logI("store takeover: reagents.gst holds 0 rows of ours - the game's own reagent file is "
             "clean (%d map node(s))", c.nodes);
        logD("every deposit of ours goes into \"%s\" or is refused, and the mod never calls the "
             "engine's AddItemToReagents for one of our %d page records",
             journalPath(), c.pageRecords);
        return;
    }
    logW("***** store takeover: reagents.gst HOLDS %d ROW(S) OF OURS - THE GAME'S OWN SAVE FILE "
         "IS NOT CLEAN *****", c.rows);
    logD("store takeover: %d row(s) still hold an item, %d are empty leftovers, %d could not be "
         "read (%d map node(s) in all). The mod never adds to that file, so these rows are older "
         "than this build, or came back with a save from Steam Cloud or a backup.",
         c.holding, c.empty, c.unreadable, c.nodes);
    logD("store takeover: nothing of yours is lost and nothing is deleted here - those rows stay "
         "alive, stay painted from the map and can still be taken out or handed back by rescue=1. "
         "What they DO cost you: a deposit of one of those records is REFUSED while the row "
         "exists (the engine would paint over the table's copy). To clear one, open the caravan on "
         "the Crafting Materials page and move that item out of it and back into a bag: the "
         "engine's own row goes empty, and the record then deposits into the collection normally.");
    int named = 0;
    int bothPlaces = 0;
    for (int i = 0; i < copied && i < c.rows; ++i) {
        const int tableN = storeCount(ours[i]);
        const bool table = tableN >= 1;
        if (table) ++bothPlaces;
        if (named < 24) {
            logD("store takeover:   still in reagents.gst: %s (table=%d map=%d)%s", ours[i],
                 tableN, held[i],
                 table ? " *** AND THE PRIVATE TABLE HOLDS THIS RECORD TOO - THE TABLE WINS FOR "
                         "COUNTING: the count you see is the table's and the map's row is not "
                         "added to it, and the map's row is never taken from. The BOX, though, is "
                         "still drawn by the GAME from its own row - ReagentWindow::Sync re-points "
                         "it - so you are looking at the map's copy until the row is gone ***"
                       : "");
            ++named;
        }
    }
    if (copied < c.rows || named < copied) {
        logD("store takeover:   ... and %d more row(s) not listed (%d name(s) printed of %d "
             "collected)",
             c.rows - named, named, copied);
    }
    if (bothPlaces > 0) {
        logW("store takeover: %d record(s) are in BOTH the private table and reagents.gst - move "
             "them out of the caravan page and back into a bag to clear the game's own row",
             bothPlaces);
        logD("the table wins for counting, so no count is doubled - but the BOX is painted by the "
             "GAME from its own row (ReagentWindow::Sync re-points a box whose record has a node), "
             "so the item you see is the map's copy until that row is gone. You own both, and "
             "rescue=1 hands back one copy per authority, i.e. two of those.");
    }
}

void storeTick(bool gameThread) {
    // The CSV switch. ut_rescue.cpp deliberately links without ut_config.cpp
    // (tools\test_journal.cpp exercises it offline), so the ini key is PUSHED in rather than
    // read there - one interlocked store per tick, from whichever thread got here first.
    // The MODE goes in, not a bool (0 off / 1 the collection / 2 everything);
    // ut_rescue clamps it and arms a CSV-ONLY pass when it changes, so a live edit of the ini
    // refreshes the export on the worker's next pass without rewriting uniq-items.jsonl.
    // `g_cfg.exportCsv != 0` here would be a compile error (ut_rescue.h deletes the bool
    // overload) - which is the point: such a line must fail the build instead of quietly
    // demoting mode 2 to mode 1.
    journalSetCsvExport(g_cfg.exportCsv);
    // The same push for the GD Stash export - one interlocked store per tick.
    journalSetGdsExport(g_cfg.exportGds);
    if (!gameThread) return;
    configFlipTick();          // say it out loud when a key moves under a running world
    takeoverCensusTick(); // once per world
    tablePublish(false);  // ... and the two numbers the heartbeat prints, every ~2 seconds
}

void storeOnWorldTeardown() {
    // Every id in the table belongs to the world that created it, exactly like ut_live's
    // display prototype cache - the objects die with the world and the ids get recycled.
    try {
        DGuard g;
        if (g_protoTable) g_protoTable->clear();
    } catch (...) {
    }
    InterlockedExchange(&g_protoBuilt, 0);
    InterlockedExchange(&g_protoRefused, 0);
    InterlockedExchange(&g_protoChurn, 0);
    InterlockedExchange(&g_protoCapLogged, 0);
    InterlockedExchange(&g_takeoverDone, 0);  // one takeover census per WORLD
    InterlockedExchange(&g_refuseLogged, 0);
    // The bow-out tally and its one explanatory line are per WORLD, like every
    // other display latch above - the ids in g_protoTable die with the world and so do the
    // reasons that were derived from them.
    for (int i = 0; i < (int)kBowCount; ++i) {
        InterlockedExchange(&g_bow[i], 0);
        InterlockedExchange(&g_bowLogged[i], 0);
    }
    // The derived prototype cap is per WORLD like every id it bounds - the
    // journal may have grown since it was worked out, and the line that explains it belongs to
    // the world it explains.
    InterlockedExchange(&g_protoCapCache, 0);
    // storeStatus() reads mod-owned atomics only, so it is safe here and on a frozen game.
    // It has a caller on purpose: a status line nothing ever emits is telemetry that does not
    // exist.
    logI("store: world teardown - %s", storeStatus());
}

const char* storeStatus() {
    // `table=<rows>/<copies>`, read out of TWO MOD-OWNED ATOMICS, never out of the journal. This
    // line is the stall check's own line (a freeze is diagnosed by the worker still writing
    // complete heartbeats while every lock is provably free), so it may not take g_cs: a heartbeat that can block on the journal lock
    // is a heartbeat that stops exactly when it is needed. `tablePublish()` refreshes the pair on
    // the GAME thread.
    const LONG rows = InterlockedCompareExchange(&g_tableRows, 0, 0);
    const LONG copies = InterlockedCompareExchange(&g_tableCopies, 0, 0);
    _snprintf_s(g_status, sizeof(g_status), _TRUNCATE, "store: table=%ld/%ld", rows, copies);
    return g_status;
}


unsigned int storeDisplayProtoId(const char* record) {
    // A null / empty record is the VANILLA page (showBox passes nullptr for
    // group -1 and for an `E` line with no item record). That is not a bow-out, it is "there is
    // no item here", and it must stay completely silent - it happens 24 times per vanilla
    // relayout.
    if (!record || !*record) return 0;
    // THE PAINT GATE, asked before the journal is even looked up. While the mod does not
    // know which of the two collections is this character's, NO record paints - not "this row
    // cannot be painted", but "the collection is hidden" - so the question is asked here, once,
    // for every record, and not per row. `utPaintDecide` below asks it again through
    // `storeTableOwns()`; that is deliberate belt and braces, because a caller that skipped this
    // line must still not be able to paint. The vanilla Crafting Materials page is untouched
    // either way: it reaches this function with a null record and returns above.
    if (!journalModeKnown()) {
        bowNote(kBowModeUnknown, record);
        return 0;
    }
    if (!g_dcsReady) {
        bowNote(kBowNotReady, record);
        return 0;
    }
    try {
        {
            DGuard g;
            if (!g_protoTable) {
                bowNote(kBowNotReady, record);
                return 0;
            }
            std::unordered_map<std::string, unsigned int>::const_iterator it =
                g_protoTable->find(record);
            if (it != g_protoTable->end()) {
                // A CACHED 0 is a refusal this world already explained once. It is counted, so
                // the parity report can say how many boxes it covers.
                if (!it->second) bowNote(kBowCached, record);
                return it->second;
            }
        }
        // THE PRIVATE TABLE's row. Only an entry the last reconciliation actually SAW in the
        // collection may paint a box: showing an identity for a record that is not stored would
        // put an item the player does not own on the page, which is the one thing a display
        // substitution must never do. UNKNOWN is not good enough here.
        int stored = UT_STORED_UNKNOWN;
        unsigned int stack = 0;
        unsigned int count = 0;
        // The three ways this can fail are three different messages to the user - "the mod never
        // saw that item", "you took it back out" and "the caravan has not been opened yet this
        // world" - and none of them may be a silent `return 0`.
        if (!journalTableRow(record, &count, &stored, &stack)) {
            bowNote(kBowNoEntry, record);
            return 0;
        }
        // The whole decision is `ut_paintgate.h` so that
        // `tools\test_store.cpp` can prove it without a game. TWO ways a box may be painted:
        //   TABLE-OWNED (count >= 1 AND the mod owns the collection) - the mod's own file is the
        //     only place this item exists, and the count IS the prototype's stack.
        //   MAP-OWNED (count 0, "stored":true) - the journal's
        //     own `stack` field, and ReagentWindow::Sync re-points the box at the map's
        //     prototype a frame later, which is correct while the map still holds it.
        //
        // The second half of the first test matters: every path that balances a paint (the
        // deposit, the take, the substitution, restoreOne's table branch, runRescue's table merge)
        // is gated on `storeTableOwns()`, and a count >= 1 row painted on a mode byte alone would
        // be a box nothing can ever decrement. The paint and the accounting ask the SAME question.
        const ut::UtPaintDecision d =
            ut::utPaintDecide(true, storeTableOwns(), journalModeKnown(), count, stored, stack);
        if (d.what == ut::kUtPaintNothing) {
            bowNote(d.why == ut::kUtPaintWhyNotStored
                        ? kBowNotStored
                        : d.why == ut::kUtPaintWhyTableOff
                              ? kBowTableOff
                              : d.why == ut::kUtPaintWhyModeUnknown ? kBowModeUnknown
                                                                    : kBowUnknown,
                    record);
            return 0;
        }
        const int cap = protoCapNow();
        // The churn discount. `g_protoChurn` counts the prototypes a
        // deposit or a take deliberately dropped so they could be rebuilt at the new stack; those
        // rebuilds are not coverage and must not spend the cap. Everything else - every first
        // build and EVERY REFUSAL - still counts, so an unbuildable record can still never be
        // retried into a leak.
        if (InterlockedCompareExchange(&g_protoBuilt, 0, 0) -
                InterlockedCompareExchange(&g_protoChurn, 0, 0) >=
            cap) {
            bowNote(kBowCap, record);
            if (!InterlockedExchange(&g_protoCapLogged, 1)) {
                logD("store: the private table has attempted %d prototypes in this world (the "
                     "cap; refusals count too, so a record that cannot be built can never be "
                     "retried into a leak) - the rest of the page keeps the ordinary display "
                     "prototypes",
                     cap);
            }
            return 0;
        }
        const char* why = "?";
        const unsigned int id = reagentBuildIdentityProto(record, d.stack, &why);
        if (!id) {
            // CACHE THE REFUSAL. `reagentBuildIdentityProto` can fail AFTER Item::CreateItem has
            // already made an object (it does not destroy it - prototypes belong to the world),
            // so retrying the same record on every relayout would leak one engine Item per
            // frame, unbounded. A stored 0 means
            // "already tried in this world"; the lookup above returns it and nothing is built
            // again until the world is torn down. Refusals count against the cap too.
            InterlockedIncrement(&g_protoRefused);
            InterlockedIncrement(&g_protoBuilt);
            if (InterlockedIncrement(&g_refuseLogged) <= 6) {
                logD("store: no private-table prototype for %s - %s (the box keeps the ordinary "
                     "display prototype; this record is not tried again in this world)",
                     record, why);
            }
            DGuard g;
            if (g_protoTable) (*g_protoTable)[record] = 0;
            return 0;
        }
        InterlockedIncrement(&g_protoBuilt);
        DGuard g;
        if (g_protoTable) (*g_protoTable)[record] = id;
        return id;
    } catch (...) {
        bowNote(kBowFault, record);
        return 0;
    }
}

// ---- the owning side ----------------------------------------------------------------------------
// The declarations are in ut_store.h. Every one of these is a thin,
// locked read or write of the JOURNAL - there is no second container to keep in step.

// A READ-ONLY journal can never own the collection: an accepted deposit that cannot be written
// down is a lost item, and the whole point of the table is that the file IS the item. When this
// returns false every caller refuses rather than handing anything to the engine's own map.
bool storeTableOwns(void) {
    // The rule itself is `ut_paintgate.h`'s, so the display gate and the accounting gate are one
    // function with one test around it.
    // `journalModeKnown()` is part of the same one question. There are two
    // collection files; until a world has said which one is this character's, the open file is
    // the start-up guess (softcore, because no character existed then). Everything that reads or
    // writes a count goes through here - the paint, the owned counters, the deposit, the take and
    // the rescue - so one test keeps the whole mod silent about a collection it is not sure of.
    const char* p = journalPath();
    return ut::utPaintTableOwns(!journalReadOnly() && p && *p, journalModeKnown());
}

// The three values `ut_paintgate.h` mirrors. A drift here would make the pure gate disagree with
// the journal about what "stored" means, and nothing would say so.
static_assert(ut::kUtStoredUnknown == UT_STORED_UNKNOWN, "UT_STORED_UNKNOWN drifted");
static_assert(ut::kUtStoredNo == UT_STORED_NO, "UT_STORED_NO drifted");
static_assert(ut::kUtStoredYes == UT_STORED_YES, "UT_STORED_YES drifted");

// The two READ-ONLY views of the display cache (declared in ut_store.h).
// Neither builds anything, so either is safe from any thread that may take `g_dcs` - and neither
// takes the JOURNAL's lock while it is held.
unsigned int storeBuiltProtoId(const char* record) {
    if (!record || !*record || !g_dcsReady) return 0;
    try {
        DGuard g;
        if (!g_protoTable) return 0;
        std::unordered_map<std::string, unsigned int>::const_iterator it =
            g_protoTable->find(record);
        return it == g_protoTable->end() ? 0u : it->second;
    } catch (...) {
        return 0;
    }
}

int storeCollectBuilt(char (*out)[256], unsigned int* protoIds, unsigned int* counts, int cap) {
    if (!g_dcsReady) return 0;
    if (out && cap <= 0) return 0;
    int n = 0;
    try {
        DGuard g;
        if (!g_protoTable) return 0;
        for (std::unordered_map<std::string, unsigned int>::const_iterator it =
                 g_protoTable->begin();
             it != g_protoTable->end(); ++it) {
            if (!it->second) continue;   // a CACHED REFUSAL is not a prototype
            if (out) {
                if (n >= cap) break;
                _snprintf_s(out[n], 256, _TRUNCATE, "%s", it->first.c_str());
                if (protoIds) protoIds[n] = it->second;
            }
            ++n;
        }
    } catch (...) {
        return n;
    }
    // The counts come from the JOURNAL, and they are read with the display lock RELEASED:
    // ParityRow::tableCount is taken at paint time for the same reason, and nothing in this file
    // may hold `g_dcs` across `g_cs`.
    if (out && counts) {
        for (int i = 0; i < n; ++i) counts[i] = storeCount(out[i]);
    }
    return n;
}

unsigned int storeCount(const char* record) {
    if (!record || !*record) return 0;
    unsigned int count = 0;
    int stored = UT_STORED_UNKNOWN;
    unsigned int stack = 0;
    if (!journalTableRow(record, &count, &stored, &stack)) return 0;
    return count;
}

// The row goes in AND THE FILE IS WRITTEN before this
// returns true, because the caller is about to destroy the only other copy of the item. On any
// failure the previous row is put back exactly as it was and the answer is false, which the
// deposit MUST turn into a refusal - the item then stays on the cursor with the game's own
// "transfer stash" error, which is the outcome this mod always prefers to a guess.
bool storeOnDeposit(const UtReplicaCapture& cap, const char* record) {
    // FIRST, because `storeTableOwns()` folds the unknown mode in with the read-only
    // journal and would otherwise refuse with the wrong reason in the log.
    if (!journalModeKnown()) {
        logW("deposit REFUSED: the mod does not know yet whether this character is hardcore or "
             "softcore, and there is one collection per mode - the item has not been touched");
        logD("nothing of the collection is painted while that is true either (ut_paintgate.h), "
             "so this line means the deposit was tried before the first live world was seen");
        return false;
    }
    if (!storeTableOwns()) {
        logW("deposit REFUSED: %s - the item has not been touched",
             journalReadOnly() ? "the collection file is read-only this session"
                               : "the collection file has no path this session");
        logD("an accepted deposit could never be written down, so accepting it would throw the "
             "item away");
        return false;
    }
    if (!record || !*record || !cap.record[0]) {
        logI("deposit REFUSED: the item has no record name - it has not been touched");
        return false;
    }
    const unsigned int before = storeCount(record);
    bool rolledBack = false;
    if (!journalDepositCommit(cap, before + 1, &rolledBack)) {
        logE("deposit REFUSED: the collection file could not be written - nothing of yours has "
             "moved; check that the mod folder is writable");
        logD("the row was %s and the count is still %u; the caller destroys the item the moment "
             "the mod says yes, so a deposit is only accepted after the file is on disk",
             rolledBack ? "PUT BACK exactly as it was" : "never changed", storeCount(record));
        return false;
    }
    tablePublish(true);
    logI("deposit accepted: %s - %u cop%s in the collection (%u in all)",
         record, before + 1, before + 1 == 1 ? "y" : "ies", journalCollectedTotal());
    logD("the collection file was written to disk before the deposit was accepted");
    protoDropForRestack(record);
    return true;
}

unsigned int storeOnTake(const char* record, unsigned int n) {
    if (!record || !*record || !n) return storeCount(record);
    const unsigned int before = storeCount(record);
    if (!before) return 0;
    // The same guard the deposit has. A take decrements a count in whichever file is
    // open, and while the mode is unknown that could be the wrong one.
    //
    // THIS CAN NEVER BE THE FIRST LINE OF DEFENCE. A take from a table box is the ENGINE's own
    // code, running inline in the exe; the mod is only told about it afterwards, so by the time
    // this refusal is reached the items are already in the player's bags and refusing only keeps
    // the count from moving - which leaves an item existing twice (in the bags AND in the other
    // mode's collection, still at count 1, with the re-deposit then refused by max_per_record).
    // The defence that actually works is upstream, in `storeDisplayProtoId`: while the mode is
    // unknown nothing is painted, so no take can reach a table row at all. This stays as the
    // backstop for a box painted before the mode changed under the world.
    if (!journalModeKnown()) {
        logW("take REFUSED: the mod does not know yet whether this character is hardcore or "
             "softcore, and there is one collection per mode - the count is unchanged");
        logW("***** if you just took %s out of a collection box, the mod could NOT record it: "
             "the item is in your bags and the collection still counts it *****", record);
        return before;
    }
    const unsigned int after = n >= before ? 0u : before - n;
    if (!journalSetCount(record, after)) return before;   // no such entry: nothing to decrement
    tablePublish(true);
    logI("take: %u cop%s of %s handed back - count %u -> %u", n, n == 1 ? "y" : "ies", record,
         before, after);
    if (!after) {
        logD("the entry is KEPT as history: the identity is still in the collection file and a "
             "re-deposit finds it");
    }
    protoDropForRestack(record);
    return after;
}

}  // namespace ut
