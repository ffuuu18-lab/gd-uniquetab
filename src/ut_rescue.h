// ut_rescue.h - the rescue kit: the journal (the mod's own record of every deposited item), the
// identity overlay a take is rebuilt from, the CSV / GDS exports and the rescue report. Pure file
// I/O; not one engine call lives in here.
//
// Why the journal exists. `ReadPlayerReagents` drops every map entry whose record has
// `craftingMaterial == 0` (Game.dll 0x2CDF70 +0x441), and `LoadPlayerReagents` can trigger a
// `SaveReagents` all by itself through `DepositTransferReagents` - so mod removed, gate broken by
// a patch, or uniq-pages.arz missing => the stored uniques are pruned at the next character load
// and Steam Cloud propagates the pruned file to every machine on the account. That is a ONE-WAY
// DOOR. Three things stand in front of it, and all three are here:
//
//   1. THE JOURNAL (`uniq-items.jsonl`). One entry per record (max_per_record = 1 makes the store
//      a map keyed by record path), written on every ACCEPTED deposit from the worker thread,
//      atomically (temp + MoveFileEx). Each entry carries the FULL 0x190-byte ItemReplicaInfo
//      copied out of the item BEFORE `AddItemToReagents` builds its own zeroed copy, plus every
//      MSVC std::string slot inside that blob DEEP-COPIED into the file's own string table -
//      because a raw copy of the blob keeps SSO strings but leaves every heap-allocated one as a
//      dangling pointer. So the file is self-contained: a lost reagent map is rebuildable from
//      it, and identity preservation on a take reads exactly this store.
//
//      The file is JSON Lines - line 1 a header carrying `format`, then ONE FLAT JSON OBJECT PER
//      LINE, one line per stored item, with the record path, the flags, every captured
//      std::string slot and the replica's remaining non-zero words in hex. Open it in Notepad;
//      grep it with findstr; delete a line to forget an item. The format number on line 1 is the
//      compatibility gate: a build that meets a HIGHER number loads what it can and then refuses
//      to write for the rest of the session. The old binary `uniq-items.bin` is still READ,
//      exactly once, to migrate it - and it is renamed, never deleted. ut_rescue.cpp's file
//      header is the full format spec.
//
//   2. THE REPORT. `rescue=1` in the ini hands every stored item back through the engine's own
//      take path (that half lives in ut_reagent.cpp, which owns the engine handles) and this
//      file writes out\rescue-report.txt: one line per record, where each item went.
//
// The journal is the file `undeploy.bat` refuses on: while it has entries and no newer
// rescue-report.txt sits next to it, uninstalling the mod would walk the user through the
// one-way door.
#pragma once

#include <windows.h>

#include <stddef.h>

namespace ut {

// What the deposit detour captures on the game thread. Fixed size on purpose: the capture runs
// inside an engine frame, so it allocates nothing.
struct UtReplicaCapture {
    char record[256];              // lower-cased DBR path - the journal key
    unsigned char replica[0x200];  // the raw ItemReplicaInfo blob (0x190 bytes on 1.3.0.8)
    unsigned int replicaLen;
    unsigned int stack;   // the box count this deposit produced
    unsigned int flags;   // UT_JF_*
    int slotCount;        // std::string slots found inside the blob
    int slotFaults;       // candidate offsets whose read faulted (diagnostic only)
    unsigned int slotOff[24];
    char slotText[24][160];
};

// ---- the slot scan, as PURE logic ---------------------------------------------------------------
// A wrong slot decision is fatal: recording a false std::string slot at ItemReplicaInfo+0x68 (a
// u32) 8 bytes before the real one at +0x70 makes identityBuild memset the wrong window, and the
// engine then memcpy's 15 bytes from address 0 inside ItemReplicaInfo::operator= (Game.dll
// 0x388BE). The decision therefore lives here, where it can be tested offline: ut_reagent
// supplies an SEH-guarded probe over ENGINE memory, the offline test supplies a plain one over a
// synthetic blob, and both run the same rule.
struct UtSlotProbeResult {
    bool ok;                 // the 32 bytes at `off` read as a std::string
    bool faulted;            // the probe raised an access violation (engine memory only)
    bool emptyHeap;          // accepted WITHOUT dereferencing: size == 0 and capacity > 15
    unsigned long long ptr;  // the raw _Ptr the window carried (diagnostic)
    unsigned long long cap;  // the raw _Myres (diagnostic)
    char text[160];          // the string, truncated
};

typedef void (*UtSlotProbeFn)(void* ctx, unsigned int off, bool allowEmptyHeap,
                              UtSlotProbeResult* out);

// What one RECORDED slot looked like, for the capture's WARNING line.
struct UtSlotShape {
    unsigned long long ptr;
    unsigned long long cap;
    unsigned char emptyHeap;
};

// Walks 8-aligned windows and records the string slots. THE RULE: a window that reads back
// EMPTY is only PROVISIONAL - `off + 8` is probed STRICTLY (no empty-heap relaxation, so its
// pointer must dereference) and, when that succeeds, `off + 8` is recorded instead. That is what
// turns the relic's false +0x68 into the real +0x70, and it covers the other "u32 then
// std::string" pairs ItemReplicaInfo::operator= shows at +0xD0/+0xD8 and +0xF8/+0xFC/+0x100 as
// well. Returns the number of slots recorded.
int utScanReplicaSlots(void* ctx, unsigned int len, UtSlotProbeFn probe, unsigned int* slotOff,
                       char (*slotText)[160], UtSlotShape* slotShape, int maxSlots, int* faults);

const unsigned int UT_JF_SYNTHESIZED = 1u;  // rebuilt from a node prototype, not from a deposit
// The two drop-condition bytes of the DEPOSITED item (Item+0xC00 / Item+0xC02). Nothing in
// Item::GetItemReplicaInfo / SetItemReplicaInfo / CreateItem carries them, so a taken-back copy
// is NOT soulbound unless the mod writes them back after the identity is applied.
const unsigned int UT_JF_SOULBOUND = 2u;
const unsigned int UT_JF_UNTRADEABLE = 4u;

// The journal format this build writes and is willing to write BACK. Bump it only when the
// meaning of a field changes; a reader that meets a higher number goes read-only (see below).
// 1 was the binary "UNIQITM1" file; 2 is uniq-items.jsonl; 3 is the same JSON Lines file with
// NAMED slot keys, a display name, and the stored/lastSeen annotation. A format-2 file is READ
// in full and UPGRADED by the next write; it is never rejected.
//
// 4 adds `"count":N` on the entry (absent = 0) and `"tableCopies":N` in the header. THE BUMP IS
// NOT COSMETIC: `entryFromKVs` skips every unknown key by design, so a format-3 build would read
// a format-4 file happily, lose every count in memory and write them away on its next deposit.
// The version test is what prevents that - a file whose `format` is greater than this number
// puts the older build READ-ONLY for the session and it writes nothing. A format-3 file is READ
// in full and every row starts at count 0, which is exactly right: a format-3 journal describes
// MAP-OWNED items and the table holds none of them (see journalUpsertCount).
#define UT_JOURNAL_FORMAT 4

// ---- is this entry actually IN the collection right now? --------------------------------------
// Three states, because two would be a lie. The journal is the only record of what a deposited
// item WAS, so an entry is never removed just because the engine no longer holds the record - it
// is ANNOTATED. UNKNOWN is the honest state for an entry no reconciliation has looked at yet
// (every entry in a format-2 file starts there), and it is the state `journal_guard.ps1` must
// treat as "possibly still stored", or a never-reconciled journal would wave an uninstall
// through.
const int UT_STORED_UNKNOWN = -1;
const int UT_STORED_NO = 0;
const int UT_STORED_YES = 1;

// Resolves the journal path and reads an existing file. Safe to call twice.
//
// THE PATH. The folder is the mod folder, resolved by ut_paths.h - the uniquetab folder beside
// the DLL, or the Documents fallback when that one cannot be written to. `journalDir` (the ini
// key `journal_dir`, empty = the mod folder) overrides it for the JOURNAL and its exports ONLY -
// the log, the ini and rescue-report.txt stay in the mod folder. It is read ONCE here and never
// re-read, because configReload replaces g_cfg once a second and a journal that moved under a
// running game would be a footgun.
//
// THE MIGRATION. One rule decides, and it is the absence of uniq-items.jsonl: if the readable
// file is not there and a uniq-items.bin is, the binary is read, the text is written atomically,
// VERIFIED field by field off the disk, and only then is the .bin renamed to
// uniq-items.bin.migrated-<stamp>. It is never deleted. If any step fails the .bin is left
// untouched, the entries stay in memory so the session still works, and the whole thing is
// re-tried at the next start.
bool journalInit(HMODULE selfModule);
bool journalInit(HMODULE selfModule, const char* journalDir);

// True when the file on disk carries a format number this build does not understand. The
// journal is then READ-ONLY for the session - journalService() writes nothing - which is the
// only thing standing between a future format and an older build silently rewriting it without
// the fields it never parsed. Mod-owned latch, never a g_cfg field.
bool journalReadOnly();

// The auto-reset event the worker waits on. The game thread only queues; the worker writes.
HANDLE journalEvent();

// Worker thread: writes uniq-items.jsonl if anything changed since the last write. The whole
// file is built in memory, written to <path>.tmp in the same folder, flushed, and renamed over
// the real file with MoveFileEx(REPLACE_EXISTING | WRITE_THROUGH) - so a crash or a kill at any
// instant leaves either the whole old file or the whole new one, never a half journal.
void journalService();

// Game thread. Adds or REPLACES the entry for `cap.record`. Returns false only on bad input.
bool journalUpsert(const UtReplicaCapture& cap);

// Game thread. Drops the entry for `record` (a take emptied the box). False = there was none.
bool journalRemove(const char* record);

bool journalHas(const char* record);

// 0 = the entry is clean, otherwise `offset + 1` of the first window that would hand the engine
// a NULL string pointer with a non-zero size (the operator= memcpy from address 0 above). Such
// an entry is never used for a substitution and is never deleted - a re-deposit of the same
// record overwrites it with a correct capture.
unsigned int journalEntryFlagged(const char* record);

size_t journalCount();
// How many entries carry a replica whose length is not `size` (empty ones are not counted);
// `otherLen` gets the first such length. A take of such an entry is refused until the item is
// deposited again, which re-captures it at this game's size.
int journalReplicaLengthCensus(unsigned int size, unsigned int* otherLen);
const char* journalPath();
long journalWrites();

// ---- the reconciliation -------------------------------------------------------------------------
// Game thread, from ut_reagent's journalReconcile() and ONLY from there. Marks one entry stored
// or not stored and stamps `lastSeen` when it is stored. Returns false when there is no such
// entry. It never adds, never removes and never touches the replica - the ONLY thing that can go
// wrong here is a wrong LABEL, and the label is three-state so "we did not look" is sayable.
//
// THE CALLER'S CONTRACT, and it is the whole safety argument: call this only after a SUCCESSFUL
// read of the engine's reagent map (`reagentWalkHeld` returned true) in a session where the
// engine has actually loaded a character's reagents. A reconcile that runs on a failed walk, or
// before the load, would mark every real entry NOT stored - which loses no data but does unlock
// the uninstall interlock, and that is the one-way door.
bool journalMarkStored(const char* record, bool stored);

// Entry counts by state. Any pointer may be null.
void journalCounts(size_t* entries, size_t* stored, size_t* notStored, size_t* unknown);

// ---- WHICH COLLECTION THIS FILE WAS WRITTEN AGAINST ----------------------------------------------
// `reagents.gst` is a SHARED save and there are TWO of them, one per COLLECTION MODE: 0 softcore,
// 1 hardcore. Each mode has its OWN journal (see journalFollowMode below), and this field is what
// PROVES a file is the one it says it is. A file written by a build in which one journal served
// both modes can still say the other mode, which is why the switch below pins it: loading a
// character of the OTHER mode gives a walk that legitimately SUCCEEDS and legitimately returns a
// map holding none of this file's records - and marking every entry "stored":false on that
// evidence is exactly the wrong answer, because `journal_guard.ps1` would then let the uninstall
// through and `journal_prune=1` would drop them.
//
// The header field KEEPS ITS NAME, `"saveVariant":N`, so that every journal ever written still
// parses - but what it holds is the COLLECTION MODE (0 softcore / 1 hardcore) as read from
// GameInfo::GetHardcore, never the GameEngine+0x375BA byte it was named after: that byte reads
// 1 on softcore characters. journalReconcile refuses to mark ANY entry NOT stored while the live
// mode disagrees with it. -1 = the file predates this field, or the mode was not known when it
// was written; the getter never faults and never calls the engine.
int journalSaveVariant();
void journalSetSaveVariant(int variant);

// ---- SEPARATE COLLECTIONS FOR HARDCORE AND SOFTCORE ----------------------------------------------
// The engine keeps two shared crafting stashes, one per mode (`reagents.gst` at 0 softcore,
// `reagents.gsh` at 1 hardcore), and which one is THIS character's is read from the character
// himself - GameInfo::GetHardcore, through readCollectionMode() in ut_reagent.cpp. The collection
// follows that, file for file:
//   mode 0   uniq-items.jsonl   uniq-export.gds   uniq-export.csv    (an existing collection
//                                                                     never moves)
//   mode 1   uniq-items-hc.jsonl uniq-export-hc.gds uniq-export-hc.csv
// in the SAME folder, under the same `journal_dir`. journalInit runs at start-up, before any
// character exists and so before there is anyone to ask, so it opens on mode 0; the first live
// world calls journalFollowMode() and the journal moves if it has to.
int journalMode();

// Point the journal at the collection `variant` (0 softcore, 1 hardcore) names. Anything else is
// ignored - an unreadable mode must never be guessed at. When it differs from journalMode() the
// current file is FLUSHED and closed, the paths are re-composed and the other mode's file is read
// in its place; true is returned ONLY then, so the caller can tear the world's tables down around
// it (a mode switch is a teardown plus an init as far as the private table is concerned). The
// entry list, the read-only/copy-aside latches and the header's saveVariant all belong to the
// file, not to the session, and are reset with it.
bool journalFollowMode(int variant);

// False until a live world has told the journal which stash is loaded. A deposit or a take may
// not touch a file while it is false: it cannot know which of the two it would be writing to.
// Neither can happen without the caravan, which needs a world - this is the belt and braces.
bool journalModeKnown();

// Make it false again. Called on world teardown only: which of the two stashes is loaded is the
// CHARACTER's property, so the next one must say it for himself before the collection is
// painted, deposited into or rescued. The open file and journalMode() are untouched.
void journalForgetMode();

// OFF by default and behind `journal_prune`: drop every entry the last reconciliation marked NOT
// stored. The file is COPIED to <journal>.pruned-<stamp> first (and the call is refused if that
// copy fails), every dropped record is written into `out`, and entries in the UNKNOWN state are
// never touched. Returns the number dropped, or -1 when it refused (`why` says which). This is
// the only code path in the mod that deliberately loses journal entries, so it is one-shot,
// user-asked-for, and reversible from the copy.
int journalPruneNotStored(char (*out)[256], int cap, char* why, size_t whyCap);

// ---- the display name ---------------------------------------------------------------------------
// "records/items/gearfeet/d101_feet.dbr" -> "Stormtitan Treads", out of catalogue.bin, which
// this file loads once (lazily, on the worker thread, never under the loader lock) and keeps.
// False = no catalogue, or the record is not in it (components, augments and affixes are NOT -
// see the leaf-name fallback in ut_rescue.cpp). Callers must treat the name as a COMMENT: it is
// derived at write time and the reader ignores it.
bool journalItemName(const char* record, char* out, size_t cap);

// Copies out at most `cap` record paths (for the reconcile walk). Returns how many were written.
int journalRecords(char (*out)[256], int cap);

// THE PRIVATE TABLE's row for one record - the three-state stored mark (UT_STORED_*) and the
// stack the deposit recorded. This is all the display side needs from the journal before it
// asks for the identity itself. False = no such entry.
bool journalStoreRow(const char* record, int* stored, unsigned int* stack);

// ---- THE PRIVATE TABLE ---------------------------------------------------------------------------
// The mod's own table of collection copies, kept in the journal. What `count` MEANS is defined
// under `journalUpsertCount` - read that paragraph before you use any of these.

// The private table's row. `count` is how many copies the collection holds RIGHT NOW; `stored`
// is the three-state mark, kept for the reconcile and the CSV. count >= 1 is the ONLY
// authorisation to paint a box, to let a take succeed or to let the rescue hand the item back -
// a row at count 0 is HISTORY, never inventory. False = no such entry.
bool journalTableRow(const char* record, unsigned int* count, int* stored, unsigned int* stack);

// Set the count. `count == 0` also marks the entry "stored":false; the entry itself is NEVER
// removed here (journal_prune stays the only thing in the mod that removes a row). Returns
// false when there is no such entry - the caller must then upsert, not invent.
bool journalSetCount(const char* record, unsigned int count);

// Every row with count >= 1, for the table build at journalInit and for plateOwnedRefresh.
// `out` is the caller's array; the return is how many were written, capped at `cap`.
int journalCollectStored(char (*out)[256], unsigned int* counts, int cap);

// The sum of every count - the number the label and the heartbeat call "collected".
unsigned int journalCollectedTotal(void);

// journalService's FULL pass, callable from ANY thread. The deposit path calls it before it
// returns true, so the row is on disk and not merely in g_entries. Returns false when the
// journal is READ-ONLY this session or the atomic write failed - and a false MUST refuse the
// deposit and roll the in-memory row back. Blocking: one .tmp + MoveFileEx of the whole file
// (a few KB, ~330 KB at a complete collection).
bool journalFlushNow(void);

// ---- WHAT `count` MEANS --------------------------------------------------------------------------
// The transition rule is
//     held(record) = tableCount(record) + mapHeld(record)
// so a MAP-OWNED row (one the engine's own reagent map still holds) must NOT also carry
// `count = 1`: `held` would say 2 for one physical item, `max_per_record=1` would refuse the next
// real deposit, and - far worse - `rescue=1` would hand the item back TWICE, once from the table
// branch and once from the map branch. Therefore:
//
//   count = HOW MANY COPIES LIVE ONLY IN THIS FILE (the mod's own private table).
//           0 for every MAP-OWNED row, including every row of a format-3 journal.
//           A row becomes count >= 1 only when a deposit records it here instead of in the
//           engine's map.
//
// Consequences, all of them deliberate:
//   * a format-3 file upgrades to format 4 with EVERY count 0 and nothing else changed - a
//     migration must never invent a copy the user does not have;
//   * `journalCollectStored` returns the TABLE-OWNED rows ONLY. The owned filter, the label
//     and the badges must OR it with the engine map (`reagentHeldTotal`) for as long as any row
//     is MAP-OWNED, or a user with a map-owned collection would be shown zero.
//     ONE EXTENSION to its contract: `out == nullptr` COUNTS the table's rows and copies
//     nothing (and `cap` is then ignored, because a truncated count would be a wrong number in
//     a log line);
//   * `stored` keeps its meaning ("the last reconcile saw this record in the collection");
//   * the header field is `"tableCopies"`, not `"collected"`: a user opening their own journal
//     must not read `"collected":0` over a large map-owned collection.
//
// `journalUpsert` therefore PRESERVES the existing count (a new entry starts at 0) - that is the
// map-owned deposit - and the table path calls this instead, which is the only function in the
// mod that can put a copy INTO the table.
bool journalUpsertCount(const UtReplicaCapture& cap, unsigned int count);

// THE DEPOSIT'S ONE ALL-OR-NOTHING STEP, and the reason it lives in ut_rescue.cpp rather than in
// the caller: only this file can put the PREVIOUS ROW back byte for byte.
//
//   1. refuse outright if the journal is READ-ONLY (nothing is touched);
//   2. take a private copy of the row as it stands (or of the fact that there was none);
//   3. upsert `cap` at `count`;
//   4. journalFlushNow() - the FILE, on this thread, before returning;
//   5. on any failure in 3 or 4: restore the copy exactly (or erase the row that was invented),
//      set *rolledBack and return false.
//
// TRUE means the item is ON DISK. Only then may the choke point return true without calling the
// engine's original, because every caller destroys the source item on a true. `rolledBack` may
// be null; it says whether step 5 had to run, which is the difference between "nothing was ever
// changed" and "something was changed and put back".
bool journalDepositCommit(const UtReplicaCapture& cap, unsigned int count, bool* rolledBack);

// ---- the CSV export (`export_csv`) --------------------------------------------------------------
// `uniq-export.csv`, beside the journal, written by the SAME atomic writer immediately after
// every successful journal write - so the two files are never more than one write apart.
// One header line (14 columns - `count` after `stored`), then one row per exported entry.
// NOTHING EVER READS IT BACK: it is an export for a human and for a spreadsheet, and losing it
// costs nothing.
//
// The switch carries a MODE, not a bool, because the journal is a history and the CSV is what
// the user calls "my collection":
//   0 = off, no file is written (an existing one is LEFT WHERE IT IS - nothing deletes it).
//   1 = THE COLLECTION (the default): entries whose stored mark is YES or UNKNOWN. UNKNOWN
//       means "not reconciled yet, so it may well still be in the page" and is therefore
//       exported - the export may never silently drop something that might be stored.
//   2 = EVERYTHING, including the `"stored":false` history rows.
// Anything above 2 is treated as 2 and anything below 0 as 0, so a typo can only ever export
// more than asked, never less.
// Both modes write the SAME 14 columns in the SAME order, and both put the stored-or-unknown
// rows FIRST (mode 2's not-stored rows follow them), exactly like the journal's own two
// blocks - so mode 1's file is mode 2's file with the tail cut off.
//
// The switch is a mod-owned latch, not a g_cfg read, for the same reason journalReadOnly()
// is: `configReload` replaces g_cfg once a second, and this file deliberately links without
// ut_config.cpp so tools/test_journal.cpp can exercise it offline. ut_store.cpp pushes the
// ini key in on its tick. Default 1, matching `export_csv=1`. A CHANGE to a non-zero mode
// arms the CSV-only pass in journalService, so flipping the key in the ini refreshes the
// export within a second or so instead of waiting for the next deposit. It does NOT touch
// g_dirty: an export preference never rewrites uniq-items.jsonl.
//
// PASS THE MODE, NOT A BOOL. bool -> int is a silent standard conversion, so a call site written
// `journalSetCsvExport(g_cfg.exportCsv != 0)` would compile clean under /W4 /WX and turn every
// export_csv=2 into mode 1, with no test able to see it (the offline harness calls this function
// directly and never goes through ut_store.cpp). The deleted overload below turns that into a
// COMPILE ERROR - if it ever fires, drop the `!= 0` and pass g_cfg.exportCsv itself.
void journalSetCsvExport(int mode);
void journalSetCsvExport(bool on) = delete;
const char* journalCsvPath();
long journalCsvWrites();

// ---- the GD STASH import file (`export_gds`) ----------------------------------------------------
// `uniq-export.gds`, beside the journal, written by the SAME atomic writer under the SAME
// lock as uniq-items.jsonl and uniq-export.csv - so the three files are never more than one
// write apart and can never disagree about what the collection was at that instant.
//
// A .gds is what GD Stash (v1.90b) imports, so the collection can be looked at, searched and
// moved with that tool. `export_gds=1` by default and `export_csv=0`; both may be on at once.
//
// The MODE is the CSV's mode, key for key, so one sentence describes both:
//   0 = off, no file is written (an existing one is LEFT WHERE IT IS - nothing deletes it).
//   1 = THE COLLECTION (the default): entries whose stored mark is YES or UNKNOWN.
//   2 = EVERYTHING, including the `"stored":false` history rows.
// Out of range clamps OUTWARDS (>2 behaves as 2, <0 as 0), and both modes put the
// stored-or-unknown records FIRST, so mode 1's file is mode 2's file with its tail removed
// and its count int lowered to match.
//
// NOTHING EVER READS IT BACK - not the mod, and not GD Stash into anything of ours. It is a
// one-way hand-off, and losing it costs nothing.
//
// The switch is a mod-owned latch pushed in by ut_store.cpp, NOT a g_cfg read, for the same
// two reasons journalSetCsvExport is: configReload replaces g_cfg once a second, and this
// file deliberately links without ut_config.cpp so tools/test_journal.cpp can round-trip the
// writer offline. PASS THE MODE, NOT A BOOL - the deleted overload turns a `!= 0` call site
// into a compile error instead of an export_gds=2 that behaves like 1.
void journalSetGdsExport(int mode);
void journalSetGdsExport(bool on) = delete;
const char* journalGdsPath();
long journalGdsWrites();

// ---- reading an entry back, and building the substitute replica ----------------------------------
//
// A taken item must come back as the item that was deposited. The journal carries the WHOLE
// 0x190-byte ItemReplicaInfo of the deposited item plus every std::string slot inside it,
// deep-copied - so every identity field is already in the file:
//
//   * seed, relicSeed, enchantmentSeed, materiaCombines, seedRerolls, affixRerolls and the stack
//     are plain u32/u64 fields INSIDE the 0x190 blob and travel with it byte for byte;
//   * prefix, suffix, modifier, transmute, materia (the component), relicCompletionBonus,
//     enchantment (the augment), ascendant and ascendant2H are std::string record paths - i.e.
//     exactly the slots `captureReplicaRaw` finds and copies into the file's own string table.
//
// So the substitute is NOT built field by field from a hard-coded layout (there is none: every
// offset in this project is decoded at run time or discovered by shape). It is built the
// layout-agnostic way: the journal blob IS the base, and only the two fields that belong to the
// LIVE creation - the object id at +0x00 and the stack-size mirror, whose offset the caller
// decodes from Item::SetStackSize's own code bytes - are taken from the replica the engine was
// about to use. Every std::string slot is then re-pointed at storage inside the overlay, because
// the pointers inside the stored blob belonged to the deposited item's heap and are dangling.
//
// identityBuild() is pure data: no engine call, no allocation, no Win32. That is what lets
// tools/test_journal.cpp round-trip it offline with no game.
struct UtIdentityOverlay {
    unsigned char replica[0x200];  // hand THIS to Item::CreateItem
    unsigned int replicaLen;
    int slotCount;                 // std::string slots rebuilt inside `replica`
    unsigned int slotOff[24];
    char strings[24][160];         // the storage the rebuilt heap strings point at
    int slotsSkipped;              // slots the journal carried that do not fit the live replica
    int bytesFromJournal;          // blob bytes (outside the slots) the journal actually changed
    int bytesComparable;           // how many bytes were compared at all (the 0x20 string
                                   // windows are excluded), so the number above reads as
                                   // "N of M", never as a completeness score
    bool keptObjectId;
    bool keptStack;
    char why[192];
};

// Copies the journal entry for `record` into `out`. False = no such entry (or no journal).
bool journalGet(const char* record, UtReplicaCapture* out);

// Builds the substitute. `incoming` is the replica the engine was about to create from, read
// by the caller (`incomingLen` bytes; it must equal the journal entry's replica length or the
// build is refused - a size change means the save format moved and nothing may be forced).
// `stackOff` is the offset of the stack-size mirror inside the replica, decoded by the caller
// from Item::SetStackSize; pass 0 when it is unknown and the journal's own value is kept.
// Returns false with `out->why` filled in; never throws, never allocates.
bool identityBuild(const UtReplicaCapture& entry, const unsigned char* incoming,
                   unsigned int incomingLen, unsigned int stackOff, UtIdentityOverlay* out);

// ---- the FRESH-PROTOTYPE swap overlay -----------------------------------------------------------
// The re-deposit refresh must not apply a replica onto the live stored prototype in place:
// ItemRelic::InitializeItem is ADDITIVE (a relic gains its bonus a second time on every
// re-deposit) and ItemEquipment's is not proven idempotent either. A NEW prototype is created
// from this overlay with the engine's own factory instead, exactly as AddItemToReagents' new-node
// branch does at Game.dll 0x2CECC4. So the overlay is not "an identity applied to a live object"
// but "the replica an object is BORN from", and two fields are forced on top of identityBuild's
// output:
//   +0x00      the object id, ZEROED (the engine zeroes it itself at 0x2CECBD before CreateItem)
//   +stackOff  the stack mirror (+0x178), set to 1 - one collection copy
// Everything else is identityBuild's, including every one of its refusals. `stackOff` must be
// non-zero and fit inside the replica or the build is refused: a prototype born with stack 0
// paints an invisible box and the exe take bails at 0x132AE0.
bool utBuildSwapOverlay(const UtReplicaCapture& entry, const unsigned char* incoming,
                        unsigned int incomingLen, unsigned int stackOff, UtIdentityOverlay* out);

// Reads an MSVC std::string out of a buffer THIS PROCESS built (identityBuild's output, or the
// test's synthetic blob). No SEH: never point it at engine memory - ut_reagent.cpp's
// `looksLikeString` is the guarded reader for that. False = the 32 bytes are not a std::string.
bool utReadMsvcString(const unsigned char* blob, unsigned int off, unsigned int blobLen,
                      char* out, size_t cap);

// Every row with count >= 1 (declared again for the table's file side; same contract as above).
int journalCollectStored(char (*out)[256], unsigned int* counts, int cap);

// ---- the rescue report -----------------------------------------------------------------------
bool rescueReportWrite(const char* text, size_t len);
const char* rescueReportPath();

}  // namespace ut
