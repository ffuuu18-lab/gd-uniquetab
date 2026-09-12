// ut_rescue.cpp - the journal (the mod's own record of every deposited item, and the private
// table of the items that exist nowhere else), its CSV and GD Stash exports, and the identity
// overlay that rebuilds a stored item's ItemReplicaInfo for the engine.
//
// See ut_rescue.h for why this exists. Everything in this file is plain Win32 file I/O; the
// engine half of the rescue (the take-path mirror) lives in ut_reagent.cpp.  The mod writes
// only its own files here (uniq-items*.jsonl, the exports, the copies aside), always whole and
// always atomically; the game's own save files are never touched.
//
// ===== uniq-items.jsonl, format 2 - ONE HUMAN-READABLE FILE ===================================
//
// The journal is JSON Lines - one flat JSON object per line, LF-terminated, UTF-8 - because the
// only record of what a deposited item really WAS should be a file the user can open in
// Notepad, grep with findstr, and repair one line at a time.
//
//   line 1   {"journal":"grim dawn uniquetab","format":2,"written":"<ISO-8601 UTC>","entries":N}
//   line n   one stored item, keys in this canonical order:
//              "record"     the lower-cased DBR path - THE KEY (case-insensitive, last wins)
//              "deposited"  ISO-8601 UTC. Written for the human; read back, but nothing uses it
//              "stack"      u32, the box count
//              "flags"      u32 bitmask, UT_JF_* - AUTHORITATIVE
//              "flagsText"  a comment for the human. THE READER IGNORES IT
//              "len"        the ItemReplicaInfo length (400 = 0x190 on 1.3.0.8)
//              "sOOO"       one per RECORDED slot, INCLUDING the empty ones. OOO = 3 hex digits,
//                           the byte offset inside the replica; the value is the string
//              "raw"        the rest of the replica: space-separated OOO:XXXXXXXX, hex offset :
//                           hex little-endian u32. Only NON-ZERO words, only words OUTSIDE every
//                           recorded slot window
//
// WHY "raw" MAY DROP THE SLOT WINDOWS (proved over 356 real entries).
// identityBuild() is the ONLY path from this file to the engine, and its first act after copying
// the blob is writeMsvcString() on every recorded slot - which memsets the whole 0x20 and
// rewrites it. Both refusal gates (nullStringWindowAt, the dangling-heap scan) then run on the
// REBUILT blob. So no byte inside a recorded slot window can ever reach the engine, and storing
// them would be storing noise. The only escape - off + 0x20 > len - is impossible at len 400 with
// a maximum slot offset of 0x140 and is pre-empted by the size-mismatch refusal anyway.
//
// RECONSTRUCTION ORDER IS FIXED AND MUST NOT BE REORDERED:
//     zero the len-byte buffer  ->  apply every "raw" word  ->  (later, in identityBuild)
//     rebuild the slots.
// A "raw" word that lands inside a slot window (a hand-edit, or a future writer) is applied and
// then harmlessly overwritten, exactly as today.
//
// EMPTY SLOTS ARE EMITTED ON PURPOSE. An empty s028 looks like noise; dropping it would leave
// that window zeroed, so the engine would see _Myres = 0 where writeMsvcString would have written
// an SSO empty string with _Myres = 15.
//
// THE READER'S CONTRACT. Split lines FIRST, parse second: a line that will not parse costs THAT
// ENTRY ONLY and the other 355 survive. The object must be FLAT - a `{` or `[` where a value is
// expected is a parse failure by design, and that is what keeps this parser ~200 lines instead of
// 500. Unknown keys are skipped (forward compatibility). A `format` HIGHER than this build knows
// loads what it can and then puts the journal in READ-ONLY mode for the session, so an older
// build can never silently rewrite a newer file and drop the fields it does not understand.
// Two rules of the reader that a later change must not "tidy away":
//   * every bound is written so it CANNOT WRAP (`off > len || len - off < 4`, never `off + 4 >
//     len`): the "raw" offset scanner accepts eight hex digits, so 0xFFFFFFFC is reachable from a
//     hand-edited file and the wrapping form let it through into a memcpy 4 GB past the buffer;
//   * `"len":0` is LEGAL and keeps the entry. ut_reagent's reconcile journals exactly that shape
//     for a record the engine still holds whose prototype it could not read; identityBuild refuses
//     a 0-byte blob, so the entry is inert for identity and exists only to say "this record is in
//     the collection" - which journalHas, journalCount and journal_guard.ps1 all depend on, and
//     dropping it made the one-time migration fail its field-for-field verify forever.
//
// A JOURNAL FILE THAT IS THERE AND CANNOT BE USED IS NEVER TREATED AS AN EMPTY ONE. Whether it is
// share-locked, truncated, over the size cap, carries a header this build cannot read or is a
// migration whose verify failed, the journal goes READ-ONLY for the session and NOTHING is written
// or migrated on top of it. "I cannot read it" must never become "there is nothing in it".
//
// ===== format 3 - NAMES INSTEAD OF BYTE OFFSETS, AND "IS IT STILL THERE?" =====================
//
// Format 3 exists to answer a player's question about the file - "359 rows but only 6 items in
// my stash, I thought it would be a row per item with visible stats and components" - and it
// changes three things and nothing else.
//
// (1) THE SLOT KEYS ARE NAMED. `s090` becomes `component@090`: <name>@<3 hex digits>. The OFFSET
//     IS STILL AUTHORITATIVE - the reader splits at '@' and parses the hex, the name is a comment
//     for the human exactly like `flagsText` - so a game patch that moved a field can only make
//     the NAME wrong, never the reconstruction. A `sOOO` key (format 2) is still accepted.
//
//     WHAT EACH SLOT IS. Six are proved by 358 real entries; the four marked
//     (POSITION ONLY) rest on `ItemReplicaInfo::WriteProperties`' field ORDER alone and have
//     never been seen filled in real data - they are named so a human can read a file that does
//     fill them, not because the game has confirmed them. Do not "tidy" these names away:
//       +0x008 baseRecord       the item's own record. Always equals "record" (357/358 real ones)
//       +0x028 prefix           affix                              (POSITION ONLY, never seen set)
//       +0x048 suffix           affix                              (POSITION ONLY, never seen set)
//       +0x070 modifier         the extra affix; a CRAFTED item's crafting bonus lives here
//       +0x090 component        the inserted component/relic ("materia")
//       +0x0B0 completionBonus  the component's / relic's completion bonus
//       +0x0D8 augment          the applied augment ("enchant")
//       +0x100 illusion         the transmuted appearance - another item's record
//       +0x120 ascendant        Ascendant bonus                    (POSITION ONLY, never seen set)
//       +0x140 ascendant2H      Ascendant, two-handed variant      (POSITION ONLY, never seen set)
//     THE PROOF is `ItemReplicaInfo::WriteProperties` (Game.dll rva 0x570810), which serialises
//     the struct in the order the save file uses: strings +0x08, +0x28, +0x48, +0x70, +0x100,
//     then u32 +0x68, then strings +0x90, +0xB0, u32 +0xD0, string +0xD8, u32 +0xF8, u32 +0xFC,
//     strings +0x120, +0x140, then u32 +0x160, +0x178, +0x180, +0x17C. Laid against the item
//     layout MEASURED out of a real transfer.gst (5 strings base/prefix/
//     suffix/modifier/transmute, u32 seed, 5 strings materia/relicCompletionBonus/enchantment/
//     ascendant/ascendant2H, 7 u32 relicSeed/unknown/enchantmentSeed/materiaCombines/stackCount/
//     seedRerolls/affixRerolls) the two orders line up field for field. FIVE of the ten are then
//     confirmed directly by the 358 real entries: +0x08 always repeats "record"; +0x90
//     holds records/items/materia/compa_* (14 of them); +0xB0 holds
//     records/items/lootaffixes/completionrelics/* on the two relics; +0xD8 holds
//     records/items/enchants/* on the three augmented items; +0x100 holds another 2H weapon's
//     record on a 2H sword, i.e. an illusion. The u32s fall out of the same alignment: +0x68 is
//     the SEED (non-zero on all 358), +0xFC the augment seed (non-zero on exactly the three
//     augmented entries) and +0x178 the stack, which the engine's own
//     ReadPlayerReagents already proved. NOT proved and NOT claimed: +0xD0 is an UNIDENTIFIED
//     u32 sitting where the alignment puts a seed for the component slot, but it is non-zero on
//     only 2 of the 14 component entries and absent on both relics, so it is described as
//     "adjacent to the component slot" and nothing more.  It is never named - it stays inside
//     `raw`.  +0x70 is NOT the relic completion-bonus record: it is the MODIFIER slot (a relic
//     seen with a 56-byte string there held records/items/lootaffixes/crafting/
//     ad05_pierceresist.dbr, which is also 56 bytes).
//
// (2) THREE DERIVED FIELDS, ALL IGNORED BY THE READER, all omitted when empty: "item" (the
//     display name out of catalogue.bin), "parts" (a one-line summary of the non-empty slots with
//     leaf names) and "seed" (the u32 at the seed slot, decimal). They are comments in the same
//     sense `flagsText` is: editing them does nothing, `raw` remains the authority.
//     AND THE ANSWER TO THE QUESTION ABOVE, which belongs next to the code: the rolled numbers
//     ("138-292 Vitality Damage") ARE NOT IN THIS FILE AND CANNOT BE. Grim Dawn stores an item as
//     record + seed and recomputes every roll when the engine builds it, so the seed IS the
//     stats; there is no numeric roll anywhere in ItemReplicaInfo to print.
//
// (3) "stored" / "lastSeen", and the header carries the counts. The 358 entries in the question
//     above were real deposits that the engine REFUNDED as stock copies during a session played
//     without the mod (the one-way door) - nothing pruned them, so the file said 358 and the
//     page held 6. An entry is marked by ut_reagent's journalReconcile
//     after a SUCCESSFUL walk of the engine's reagent map: `"stored":true` with a `lastSeen`
//     stamp, or `"stored":false`. NO MARK AT ALL = the honest third state, "no reconciliation has
//     looked at this entry" - which is where every format-2 entry starts and what the uninstall
//     guard must read as "possibly still stored". Not-stored entries are written LAST, so the top
//     of the file is the collection. NOTHING IS EVER DROPPED BY THE RECONCILIATION; only the
//     explicit, off-by-default `journal_prune` does that, and it copies the file aside first.
//
// THE OLD BINARY FORMAT (version 1: magic "UNIQITM1", u32 version/entryCount/entryStride=56/
// entriesOff=64/blobOff/blobLen/slotsOff/slotsLen/stringsOff/stringsLen, u64 writtenAt, then
// entryCount x 56-byte entries {recordOff,recordLen,replicaOff,replicaLen,slotFirst,slotCount,
// stack,flags,depositedAt}, then the blob, the 12-byte {slotOff,strOff,strLen} triples and one
// flat string table) is STILL READ, by readBinaryFile() below, for ONE purpose: migrating an
// existing uniq-items.bin exactly once. Do not delete it, and do not write it ever again.
#include "ut_rescue.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include "model/catalogue.h"
#include "ut_gds.h"
#include "ut_log.h"
#include "ut_paths.h"

namespace ut {
namespace {

struct Slot {
    unsigned int off;
    std::string text;
};

struct Entry {
    std::string record;
    std::vector<unsigned char> replica;
    std::vector<Slot> slots;
    unsigned int stack;
    unsigned int flags;
    unsigned long long depositedAt;
    // 0 = clean, else offset+1 of a window that would give the engine a NULL
    // string pointer with a non-zero size. Set when the file is READ (never deleted).
    unsigned int nullShape = 0;
    // UT_STORED_UNKNOWN / _NO / _YES, and when the last reconciliation saw it
    // in the engine's map. Written by journalMarkStored only.
    int stored = UT_STORED_UNKNOWN;
    unsigned long long lastSeenAt = 0;
    // The one-shot probe mark an earlier build could set on ONE entry. Nothing
    // sets it any more; it is still read and written back so a file that carries one keeps it,
    // byte for byte.
    bool probe = false;
    // THE PRIVATE TABLE's row. How many copies of this record live ONLY in
    // this file - 0 for every MAP-OWNED row, which is every row of a format-3 journal and
    // every row an older build's deposit path wrote. `count >= 1` is the only
    // authorisation to paint from the table, to let a take succeed against it, or to let the
    // rescue hand the item back out of it. Written as `"count":N` and omitted at 0.
    unsigned int count = 0;
};

CRITICAL_SECTION g_cs;
bool g_csReady = false;
std::vector<Entry>* g_entries = nullptr;
char g_path[MAX_PATH] = {0};      // <journal dir>\uniq-items.jsonl - THE journal
char g_reportPath[MAX_PATH] = {0};
// The resolved journal folder, kept so the three paths can be re-composed when the
// mode switches, and the mode itself - 0 the softcore names, 1 the `-hc` ones. See ut_rescue.h.
char g_dir[MAX_PATH] = {0};
volatile LONG g_mode = 0;
volatile LONG g_modeKnown = 0;
HANDLE g_event = nullptr;
volatile LONG g_dirty = 0;
volatile LONG g_writes = 0;
// The CSV export. A mod-owned latch (see journalSetCsvExport in the header) and its own path +
// counter. The latch holds the MODE - 0 off, 1 the collection (stored yes + not-yet-reconciled),
// 2 every entry including the history. Default 1.
volatile LONG g_csvMode = 1;
// The CSV's OWN dirty flag. A mode change must not set g_dirty, which rewrites uniq-items.jsonl -
// the user's most important file - for a change that concerns only a derived export. This latch
// is consumed by journalService and writes the CSV alone.
volatile LONG g_csvArm = 0;
volatile LONG g_csvWrites = 0;
volatile LONG g_csvFailLogged = 0;
char g_csvPath[MAX_PATH] = {0};   // <journal dir>\uniq-export.csv
// The GD STASH export - the same three-mode latch, the same atomic writer, the same "nothing
// ever reads it back" contract, a different file format (src\ut_gds.h). This one is the DEFAULT
// export (`export_gds=1`); the CSV defaults to 0.
volatile LONG g_gdsMode = 1;
volatile LONG g_gdsArm = 0;
volatile LONG g_gdsWrites = 0;
volatile LONG g_gdsFailLogged = 0;
char g_gdsPath[MAX_PATH] = {0};   // <journal dir>\uniq-export.gds

// A mod-owned volatile LONG, NEVER a g_cfg field: configReload replaces g_cfg
// once a second, so a latch that lived there would be wiped by the next reload.
//   g_readOnly   the file on disk carries a format number this build does not understand.
//                journalService() refuses to write for the rest of the session. This one
//                comparison is all that stands between a future format-3 file and a format-2
//                build silently rewriting it without the fields it never parsed.
//   g_copyAside  the file parsed, but not cleanly (the header's entry count disagreed with what
//                came back, or a line was dropped). The NEXT write copies the current file to
//                uniq-items.jsonl.bad-<stamp> FIRST, so a user who mangled one line can get it
//                back instead of having the evidence replaced by the next deposit.
volatile LONG g_readOnly = 0;
// The COLLECTION MODE this journal belongs to (0 softcore,
// 1 hardcore), -1 when the file predates the field or the mode was never readable. The header
// field is still spelled `saveVariant` for format compatibility. See ut_rescue.h.
volatile LONG g_saveVariant = -1;
volatile LONG g_copyAside = 0;
volatile LONG g_readOnlySaid = 0;

unsigned long long nowFileTime() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

int findEntry(const char* record) {
    if (!g_entries || !record) return -1;
    for (size_t i = 0; i < g_entries->size(); ++i) {
        if (_stricmp((*g_entries)[i].record.c_str(), record) == 0) return (int)i;
    }
    return -1;
}

unsigned int get32(const std::vector<unsigned char>& v, size_t at) {
    unsigned int x = 0;
    if (at + 4 > v.size()) return 0;
    memcpy(&x, &v[at], 4);
    return x;
}

// ---- the atomic write -----------------------------------------------------------------------
//
// THE WHOLE file is built in memory, written to "<path>.tmp" in the SAME FOLDER (so the rename
// is a same-volume metadata operation and cannot fall back to a copy), flushed to the platter
// with FlushFileBuffers, and only then renamed over the real file with
//     MoveFileEx(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH).
//
// WHY THAT IS ATOMIC. MoveFileEx with REPLACE_EXISTING on one volume is a directory-entry
// rename: at every instant the real path names either the whole old file or the whole new one,
// never a mixture, and a crash or a kill at any point leaves at worst an orphan .tmp that
// nothing reads. MOVEFILE_WRITE_THROUGH additionally forbids the rename itself from sitting in
// the cache - it does not return until the change is on the disk - which is what makes a power
// cut between an accepted deposit and the next flush unable to lose the entry. A plain
// CreateFile(CREATE_ALWAYS) + WriteFile over the real path would truncate first and would leave
// a half journal on a crash; that is exactly what this must never do.
//
// One retry after Sleep(30) covers a transient sharing violation (an editor or an antivirus
// holding the file for a moment). On failure the .tmp is removed and the caller re-arms g_dirty.
// Worker thread only.
bool writeWholeFileAtomic(const char* path, const char* data, size_t len) {
    if (!path || !path[0]) return false;
    char tmp[MAX_PATH];
    _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "%s.tmp", path);
    HANDLE h = CreateFileA(tmp, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    size_t at = 0;
    while (ok && at < len) {
        const DWORD chunk = (DWORD)((len - at) > 0x400000u ? 0x400000u : (len - at));
        DWORD wrote = 0;
        if (!WriteFile(h, data + at, chunk, &wrote, nullptr) || wrote != chunk) ok = false;
        at += wrote;
    }
    FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) {
        DeleteFileA(tmp);
        return false;
    }
    const DWORD kMove = MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH;
    if (!MoveFileExA(tmp, path, kMove)) {
        Sleep(30);
        if (!MoveFileExA(tmp, path, kMove)) {
            DeleteFileA(tmp);
            return false;
        }
    }
    return true;
}

// yyyymmdd-hhmmss, for the .migrated- / .rejected- / .bad- / .unreadable- suffixes. A dated name
// means a repeated migration (a user restoring a backup) never collides with an older one.
void stampNow(char* out, size_t cap) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    _snprintf_s(out, cap, _TRUNCATE, "%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay,
                st.wHour, st.wMinute, st.wSecond);
}

// ---- the text writer -------------------------------------------------------------------------
// About 40 lines and no third-party parser anywhere near it.

void isoFromFileTime(unsigned long long ft, char* out, size_t cap) {
    out[0] = 0;
    if (!ft) return;
    FILETIME f;
    f.dwLowDateTime = (DWORD)(ft & 0xFFFFFFFFull);
    f.dwHighDateTime = (DWORD)(ft >> 32);
    SYSTEMTIME st;
    if (!FileTimeToSystemTime(&f, &st)) return;
    // SEVEN fractional digits, i.e. the FILETIME's own 100 ns tick, not microseconds.
    // fileTimeFromIso reads up to 7 and scales, so `deposited` round trips exactly and "every
    // field survives the round trip" is true without an exception.
    const unsigned long long tick = ft % 10000000ull;  // 100ns ticks within the second
    _snprintf_s(out, cap, _TRUNCATE, "%04u-%02u-%02uT%02u:%02u:%02u.%07lluZ", st.wYear, st.wMonth,
                st.wDay, st.wHour, st.wMinute, st.wSecond, tick);
}

// The escape set is deliberately minimal: the five short escapes, the two mandatory ones, and
// \u00XX for any other byte below 0x20. Bytes >= 0x80 pass straight through - the file is UTF-8
// and every string the engine hands us is an ASCII record path, so this is a no-op in practice.
void jsonEscapeTo(std::string* out, const char* s) {
    out->push_back('"');
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
        const unsigned char c = *p;
        switch (c) {
            case '"': out->append("\\\""); break;
            case '\\': out->append("\\\\"); break;
            case '\b': out->append("\\b"); break;
            case '\f': out->append("\\f"); break;
            case '\n': out->append("\\n"); break;
            case '\r': out->append("\\r"); break;
            case '\t': out->append("\\t"); break;
            default:
                if (c < 0x20) {
                    char u[8];
                    _snprintf_s(u, sizeof(u), _TRUNCATE, "\\u%04X", (unsigned int)c);
                    out->append(u);
                } else {
                    out->push_back((char)c);
                }
                break;
        }
    }
    out->push_back('"');
}

// ---- the slot NAMES -------------------------------------------------------------------------
// One table, one place. The offset stays authoritative everywhere: this only decides what the
// human reads. The proof for every row is in the format-3 block at the top of this file
// (ItemReplicaInfo::WriteProperties' field order + the measured save layout + the 358 real
// entries). An offset that is not in the table is written as `slot@OOO`, which says exactly what
// it is: a recorded std::string window nobody has identified.
struct SlotName {
    unsigned int off;
    const char* name;
};

const SlotName kSlotNames[] = {
    {0x008, "baseRecord"},   {0x028, "prefix"},   {0x048, "suffix"},
    {0x070, "modifier"},     {0x090, "component"}, {0x0B0, "completionBonus"},
    {0x0D8, "augment"},      {0x100, "illusion"}, {0x120, "ascendant"},
    {0x140, "ascendant2H"},
};

// The u32 that IS the item's rolled stats. Not a slot (it is a plain word inside `raw`, which
// stays the authority); the writer prints it as the derived "seed" comment only.
const unsigned int kSeedOffset = 0x068;

const char* slotNameFor(unsigned int off) {
    for (size_t i = 0; i < sizeof(kSlotNames) / sizeof(kSlotNames[0]); ++i) {
        if (kSlotNames[i].off == off) return kSlotNames[i].name;
    }
    return "slot";
}

// "records/items/materia/compa_markofthetraveler.dbr" -> "markofthetraveler". DERIVED, not the
// game's own name: the catalogue does not carry components, augments or affixes (see
// journalItemName), so this is the honest fallback and the guide says so.
void leafName(const char* record, char* out, size_t cap) {
    if (!out || !cap) return;
    out[0] = 0;
    if (!record || !record[0]) return;
    const char* leaf = record;
    for (const char* p = record; *p; ++p) {
        if (*p == '/' || *p == '\\') leaf = p + 1;
    }
    char buf[160];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s", leaf);
    const size_t n = strlen(buf);
    if (n > 4 && _stricmp(buf + n - 4, ".dbr") == 0) buf[n - 4] = 0;
    const char* body = buf;
    // compa_ / compb_ / compc_ are the component tiers; the tier is not part of the name.
    if (strlen(body) > 6 && _strnicmp(body, "comp", 4) == 0 && body[5] == '_') body += 6;
    _snprintf_s(out, cap, _TRUNCATE, "%s", body);
}

// ---- catalogue.bin, for the "item" display name ---------------------------------------------
// Loaded LAZILY, from the worker thread, on the first journal write - never from journalInit,
// which dllmain.cpp calls under the loader lock. The Catalogue is immutable once parsed and this
// file is its only user here, so no lock is needed beyond the one-shot attempt flag, which is
// only ever touched from the worker. ~1 MB resident; a failure is not an error, it just means the
// "item" field is omitted.
// The pointer is `volatile` because it is read from a SECOND thread: tier C's name half calls
// `journalItemName` from `searchSweepTick`, i.e. the game thread, while the store happens once
// on the worker. The object behind it is immutable after `loadFromFile` returns and
// `Catalogue::indexOfRecord` is a pure const unordered_map lookup with no lazy caching, so
// concurrent READS are safe; the volatile declares the publication of the pointer.
// THE LATCH IS INTERLOCKED: journalFlushNow() is callable from ANY thread (the deposit path
// calls it on the game thread), so two threads could otherwise run the 480 KB load at once.
// First caller loads, every other caller returns at once.
gdut::Catalogue* volatile g_cat = nullptr;
volatile LONG g_catTried = 0;

void catalogueEnsure() {
    if (InterlockedExchange(&g_catTried, 1)) return;
    // The mod folder, through the one resolver (ut_paths.h). This runs on whichever thread asked
    // for a display name first, so it finds its own module by the address of this function.
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&catalogueEnsure, &self);
    char path[MAX_PATH] = {0};
    if (utModFile(self, "catalogue.bin", path, sizeof(path))) {
        try {
            gdut::Catalogue* c = new gdut::Catalogue();
            std::string err;
            if (c->loadFromFile(path, &err)) {
                g_cat = c;
                logD("rescue journal: display names from \"%s\" (%zu items) - the \"item\" field "
                     "on each line. Components, augments and affixes are NOT in the catalogue; "
                     "their record path is printed and \"parts\" carries a derived leaf name.",
                     path, c->itemCount());
                return;
            }
            delete c;
        } catch (...) {
        }
    }
    logD("rescue journal: no usable catalogue.bin at \"%s\" - lines carry the record path but no "
         "\"item\" display name (this is cosmetic; nothing else changes)",
         path);
}

// A comment for the human. THE READER IGNORES IT - `flags` is the authority.
const char* flagsText(unsigned int flags) {
    switch (flags & (UT_JF_SYNTHESIZED | UT_JF_SOULBOUND | UT_JF_UNTRADEABLE)) {
        case 0: return "none";
        case UT_JF_SYNTHESIZED: return "synthesized";
        case UT_JF_SOULBOUND: return "soulbound";
        case UT_JF_UNTRADEABLE: return "untradeable";
        case UT_JF_SOULBOUND | UT_JF_UNTRADEABLE: return "soulbound+untradeable";
        case UT_JF_SYNTHESIZED | UT_JF_SOULBOUND: return "synthesized+soulbound";
        case UT_JF_SYNTHESIZED | UT_JF_UNTRADEABLE: return "synthesized+untradeable";
        default: return "synthesized+soulbound+untradeable";
    }
}

bool offInAnySlot(const std::vector<Slot>& slots, unsigned int off) {
    for (size_t i = 0; i < slots.size(); ++i) {
        if (off >= slots[i].off && off < slots[i].off + 0x20) return true;
    }
    return false;
}

// The replica exactly as the reader will rebuild it: a zeroed len-byte buffer with every
// non-zero 4-aligned word OUTSIDE a recorded slot window applied. The writer emits those words;
// the migration verify compares against this so both sides mean the same thing.
void leanReplica(const Entry& e, std::vector<unsigned char>* out) {
    const unsigned int len = (unsigned int)e.replica.size();
    out->assign(len, 0);
    for (unsigned int off = 0; off + 4 <= len; off += 4) {
        if (offInAnySlot(e.slots, off)) continue;
        unsigned int w = 0;
        memcpy(&w, &e.replica[off], 4);
        if (!w) continue;
        memcpy(&(*out)[off], &w, 4);
    }
}

// The one-line human summary of what is ON the item. Every name in it is a
// DERIVED leaf name (except the illusion, which is a real item record the catalogue knows), and
// the reader ignores the whole field - the slot keys are the data.
void partsText(const Entry& e, std::string* out) {
    out->clear();
    for (size_t i = 0; i < e.slots.size(); ++i) {
        const Slot& s = e.slots[i];
        // Suppress the item's own base record by VALUE as well as by offset. A real entry has
        // been captured with the record slot at +0x000 rather than +0x008, and an offset-only
        // test would list the item as one of its own parts.
        if (s.text.empty() || s.off == 0x008) continue;
        if (_stricmp(s.text.c_str(), e.record.c_str()) == 0) continue;
        char pretty[192];
        pretty[0] = 0;
        if (!journalItemName(s.text.c_str(), pretty, sizeof(pretty)) || !pretty[0]) {
            leafName(s.text.c_str(), pretty, sizeof(pretty));
        }
        if (!pretty[0]) continue;
        char one[256];
        _snprintf_s(one, sizeof(one), _TRUNCATE, "%s%s %s", out->empty() ? "" : ", ",
                    slotNameFor(s.off), pretty);
        try {
            out->append(one);
        } catch (...) {
            return;
        }
    }
}

void appendEntryLine(std::string* out, const Entry& e) {
    char num[96];
    out->append("{\"record\":");
    jsonEscapeTo(out, e.record.c_str());
    // DERIVED, the reader ignores it: the collection's own display name for this record.
    char item[192];
    if (journalItemName(e.record.c_str(), item, sizeof(item)) && item[0]) {
        out->append(",\"item\":");
        jsonEscapeTo(out, item);
    }
    // ABSENT = UNKNOWN = "no reconciliation has looked at this entry yet", and
    // that is a real, load-bearing state (journal_guard.ps1 reads it as "possibly still stored").
    if (e.stored == UT_STORED_YES || e.stored == UT_STORED_NO) {
        _snprintf_s(num, sizeof(num), _TRUNCATE, ",\"stored\":%s",
                    e.stored == UT_STORED_YES ? "true" : "false");
        out->append(num);
    }
    // The probe mark an earlier build could set on one entry. Nothing sets it any
    // more, and a file that carries one keeps it: the field is read and written back unchanged.
    if (e.probe) out->append(",\"probe\":true");
    // Format 4: how many copies of this record the PRIVATE TABLE holds.
    // ABSENT MEANS 0 and 0 is the ordinary case - every row of a format-3 journal, and every
    // row the collection still keeps in the engine's own map - so an upgraded file gains no
    // noise at all and the rows that carry the key are exactly the rows that live in this file
    // and nowhere else.
    if (e.count) {
        _snprintf_s(num, sizeof(num), _TRUNCATE, ",\"count\":%u", e.count);
        out->append(num);
    }
    char iso[64];
    if (e.lastSeenAt) {
        isoFromFileTime(e.lastSeenAt, iso, sizeof(iso));
        if (iso[0]) {
            out->append(",\"lastSeen\":");
            jsonEscapeTo(out, iso);
        }
    }
    isoFromFileTime(e.depositedAt, iso, sizeof(iso));
    if (iso[0]) {
        out->append(",\"deposited\":");
        jsonEscapeTo(out, iso);
    }
    _snprintf_s(num, sizeof(num), _TRUNCATE, ",\"stack\":%u,\"flags\":%u", e.stack, e.flags);
    out->append(num);
    out->append(",\"flagsText\":");
    jsonEscapeTo(out, flagsText(e.flags));
    std::string parts;
    partsText(e, &parts);
    if (!parts.empty()) {
        out->append(",\"parts\":");
        jsonEscapeTo(out, parts.c_str());
    }
    const unsigned int len = (unsigned int)e.replica.size();
    // DERIVED, the reader ignores it: `raw` carries the same word and stays the authority. This
    // number IS the item's rolled stats - the engine recomputes them from it - which is why no
    // damage range can ever be printed here.
    if (len >= kSeedOffset + 4 && !offInAnySlot(e.slots, kSeedOffset)) {
        unsigned int seed = 0;
        memcpy(&seed, &e.replica[kSeedOffset], 4);
        if (seed) {
            _snprintf_s(num, sizeof(num), _TRUNCATE, ",\"seed\":%u", seed);
            out->append(num);
        }
    }
    _snprintf_s(num, sizeof(num), _TRUNCATE, ",\"len\":%u", len);
    out->append(num);
    for (size_t i = 0; i < e.slots.size(); ++i) {
        _snprintf_s(num, sizeof(num), _TRUNCATE, ",\"%s@%03X\":", slotNameFor(e.slots[i].off),
                    e.slots[i].off);
        out->append(num);
        jsonEscapeTo(out, e.slots[i].text.c_str());
    }
    out->append(",\"raw\":\"");
    bool first = true;
    for (unsigned int off = 0; off + 4 <= len; off += 4) {
        if (offInAnySlot(e.slots, off)) continue;
        unsigned int w = 0;
        memcpy(&w, &e.replica[off], 4);
        if (!w) continue;
        if (!first) out->push_back(' ');
        first = false;
        _snprintf_s(num, sizeof(num), _TRUNCATE, "%03X:%08X", off, w);
        out->append(num);
    }
    out->append("\"}\n");
}

// Builds the WHOLE file. Never writes incrementally: see writeWholeFileAtomic.
//
// The header carries the three counts a reader of the file asks about (rows, stored, not
// stored), and the entries are written in TWO stable
// blocks - everything that is stored or not yet reconciled first, then the not-stored ones - so
// the top of the file reads as the collection and the tail reads as its history. The order of a
// JSON Lines journal has no meaning to the reader (records are keyed by "record", last wins), so
// this is presentation and nothing else; no entry is dropped, moved out or rewritten.
void buildText(const std::vector<Entry>& entries, std::string* out) {
    out->clear();
    out->reserve(entries.size() * 460 + 192);
    size_t stored = 0, notStored = 0, unknown = 0;
    // And the sum of the counts - how many copies live in THIS FILE and
    // nowhere else. It is deliberately NOT called "collected": a user opening
    // their own journal must not read `"collected":0` over a 537-item collection whose items are
    // still in the engine's map. "tableCopies" says exactly what it counts.
    size_t tableCopies = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].stored == UT_STORED_YES) ++stored;
        else if (entries[i].stored == UT_STORED_NO) ++notStored;
        else ++unknown;
        tableCopies += entries[i].count;
    }
    char iso[64];
    isoFromFileTime(nowFileTime(), iso, sizeof(iso));
    char hdr[384];
    const LONG variant = InterlockedCompareExchange(&g_saveVariant, -1, -1);
    char variantField[40];
    variantField[0] = 0;
    if (variant >= 0) {
        _snprintf_s(variantField, sizeof(variantField), _TRUNCATE, ",\"saveVariant\":%ld", variant);
    }
    _snprintf_s(hdr, sizeof(hdr), _TRUNCATE,
                "{\"journal\":\"grim dawn uniquetab\",\"format\":%u,\"written\":\"%s\","
                "\"entries\":%zu,\"stored\":%zu,\"notStored\":%zu,\"unknown\":%zu,"
                "\"tableCopies\":%zu%s}\n",
                (unsigned int)UT_JOURNAL_FORMAT, iso, entries.size(), stored, notStored, unknown,
                tableCopies, variantField);
    out->append(hdr);
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].stored == UT_STORED_NO) continue;
        appendEntryLine(out, entries[i]);
    }
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].stored != UT_STORED_NO) continue;
        appendEntryLine(out, entries[i]);
    }
}

bool writeFile() {
    if (!g_entries || !g_path[0]) return false;
    // The one-shot "the file we read was not clean" rule: copy it aside BEFORE the first
    // overwrite, so a user who mangled one line can get the original back. Best effort - a
    // failed copy must never stop the journal being written.
    if (InterlockedCompareExchange(&g_copyAside, 0, 1) == 1) {
        char stamp[32], aside[MAX_PATH];
        stampNow(stamp, sizeof(stamp));
        _snprintf_s(aside, sizeof(aside), _TRUNCATE, "%s.bad-%s", g_path, stamp);
        if (CopyFileA(g_path, aside, TRUE)) {
            logW("rescue journal: the file that was read did not parse cleanly, so a copy of it "
                 "was kept as %s before this write replaced it",
                 aside);
        }
    }
    std::string text;
    buildText(*g_entries, &text);
    if (!writeWholeFileAtomic(g_path, text.c_str(), text.size())) return false;
    InterlockedIncrement(&g_writes);
    return true;
}

bool readWholeFile(const char* path, std::vector<unsigned char>* out) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > (64 << 20)) {
        CloseHandle(h);
        return false;
    }
    out->resize((size_t)size.QuadPart);
    DWORD got = 0;
    const BOOL r = ReadFile(h, &(*out)[0], (DWORD)out->size(), &got, nullptr);
    CloseHandle(h);
    if (!r || got != out->size()) return false;
    return true;
}

std::string stringAt(const std::vector<unsigned char>& f, unsigned int base, unsigned int off,
                     unsigned int len) {
    const size_t at = (size_t)base + off;
    if (at + len > f.size()) return std::string();
    return std::string((const char*)&f[at], len);
}

void writeMsvcString(unsigned char* blob, unsigned int off, char* storage);  // defined below

// ---- THE shape that is guaranteed to fault --------------------------------------------------
// MSVC's basic_string copy reads _Myres first: >= 16 means "the data is on the heap", and it then
// memcpy's _Mysize bytes from _Ptr. `_Ptr == 0 with _Mysize > 0` is therefore not a doubtful
// pointer - it is a NULL memcpy source, a guaranteed freeze (memcpy(dst, NULL, 15) inside
// ItemReplicaInfo::operator=, Game.dll 0x388BE). A scan that treats NULL as "not a string" and
// skips it is backwards, and an overlap rule skips the window that straddles a rebuilt slot -
// which is where this shape is BORN (writeMsvcString memsets 0x20
// bytes, so the window 8 bytes later reads _Ptr = 0 and _Mysize = the rebuilt slot's _Myres).
//
// CALIBRATION (this is why the plausibility bound is not optional). Run over a real 28-entry
// journal that had frozen the game, the unbounded test
// `ptr == 0 && size > 0 && capacity >= 16` refuses ALL 28 entries: every rebuilt slot leaves a
// window at slot+8 whose "size" is the rebuilt _Myres (15) and whose "capacity" is whatever
// number follows - 9,502,583 / 4,294,967,296 / 78,181,826,560 and so on. Adding the same
// plausibility bound the rest of this file uses for a std::string window (16 <= capacity <=
// 0x4000 and size <= capacity) refuses EXACTLY ONE of the 28 - the relic, at +0x070 with
// size 15 and capacity 63, which is the window that actually crashed the game - and accepts all
// 27 others plus a post-FIX-1 relic capture. A capacity of 78 billion is not a struct member the
// engine ever reads; 63 is.
unsigned int nullStringWindowAt(const unsigned char* blob, unsigned int len,
                                const unsigned int* slotOff, int slotCount, unsigned int* sizeOut,
                                unsigned int* capOut) {
    for (unsigned int off = 0; off + 0x20 <= len; off += 8) {
        bool ownSlot = false;
        for (int i = 0; i < slotCount; ++i) {
            if (slotOff[i] == off) ownSlot = true;
        }
        if (ownSlot) continue;  // a rebuilt slot can never BE this shape (writeMsvcString)
        unsigned long long ptr = 0;
        size_t size = 0, capacity = 0;
        memcpy(&ptr, blob + off, sizeof(ptr));
        memcpy(&size, blob + off + 0x10, sizeof(size));
        memcpy(&capacity, blob + off + 0x18, sizeof(capacity));
        if (ptr != 0) continue;
        if (size == 0 || capacity < 16 || capacity > 0x4000 || size > capacity) continue;
        if (sizeOut) *sizeOut = (unsigned int)size;
        if (capOut) *capOut = (unsigned int)capacity;
        return off + 1;
    }
    return 0;
}

// The same test, applied to a journal ENTRY as it comes off disk: the slots are rebuilt into a
// scratch copy exactly as identityBuild would, then the shape is looked for. 0 = clean.
unsigned int entryNullShape(const std::vector<unsigned char>& rep, const std::vector<Slot>& slots,
                            unsigned int* sizeOut, unsigned int* capOut) {
    const unsigned int len = (unsigned int)rep.size();
    if (len < 0x20 || len > 0x200) return 0;
    unsigned char blob[0x200];
    memcpy(blob, &rep[0], len);
    char storage[24][160];
    unsigned int offs[24];
    int n = 0;
    for (size_t i = 0; i < slots.size() && n < 24; ++i) {
        if (slots[i].off + 0x20 > len) continue;
        _snprintf_s(storage[n], sizeof(storage[n]), _TRUNCATE, "%s", slots[i].text.c_str());
        writeMsvcString(blob, slots[i].off, storage[n]);
        offs[n] = slots[i].off;
        ++n;
    }
    return nullStringWindowAt(blob, len, offs, n, sizeOut, capOut);
}

// ---- the LEGACY BINARY reader (format 1) -----------------------------------------------------
// Kept for ONE purpose - reading an existing uniq-items.bin once, so its entries can be migrated
// to uniq-items.jsonl. Nothing writes this format. Do NOT
// delete it: it is the only thing that can still read a collection in the old binary format, and
// entryNullShape() below is shared with the text reader.
//
// A file we cannot parse is left alone on disk and reported; the caller additionally copies it
// aside as .unreadable-<stamp> so a later write cannot destroy the evidence.
bool readBinaryFileInto(const char* path, std::vector<Entry>* into) {
    std::vector<unsigned char> f;
    if (!readWholeFile(path, &f)) return false;
    if (f.size() < 64 || memcmp(&f[0], "UNIQITM1", 8) != 0) {
        logD("rescue journal: %s is not a UNIQITM1 file (%zu bytes) - ignored", path, f.size());
        return false;
    }
    const unsigned int version = get32(f, 0x08);
    const unsigned int count = get32(f, 0x0C);
    const unsigned int stride = get32(f, 0x10);
    const unsigned int entriesOff = get32(f, 0x14);
    const unsigned int blobOff = get32(f, 0x18);
    const unsigned int slotsOff = get32(f, 0x20);
    const unsigned int stringsOff = get32(f, 0x28);
    if (version != 1 || stride < 56 || count > 100000) {
        logD("rescue journal: %s has version %u / stride %u / %u entries - ignored", path,
             version, stride, count);
        return false;
    }
    if ((size_t)entriesOff + (size_t)count * stride > f.size()) {
        logD("rescue journal: %s is truncated - ignored", path);
        return false;
    }
    for (unsigned int i = 0; i < count; ++i) {
        const size_t at = (size_t)entriesOff + (size_t)i * stride;
        Entry e;
        e.record = stringAt(f, stringsOff, get32(f, at + 0x00), get32(f, at + 0x04));
        const unsigned int repOff = get32(f, at + 0x08);
        const unsigned int repLen = get32(f, at + 0x0C);
        if ((size_t)blobOff + repOff + repLen <= f.size() && repLen && repLen <= 0x200) {
            e.replica.assign(f.begin() + blobOff + repOff, f.begin() + blobOff + repOff + repLen);
        }
        const unsigned int slotFirst = get32(f, at + 0x10);
        const unsigned int slotCount = get32(f, at + 0x14);
        for (unsigned int s = 0; s < slotCount && s < 64; ++s) {
            const size_t t = (size_t)slotsOff + ((size_t)slotFirst + s) * 12;
            if (t + 12 > f.size()) break;
            Slot sl;
            sl.off = get32(f, t + 0);
            sl.text = stringAt(f, stringsOff, get32(f, t + 4), get32(f, t + 8));
            e.slots.push_back(sl);
        }
        e.stack = get32(f, at + 0x18);
        e.flags = get32(f, at + 0x1C);
        memcpy(&e.depositedAt, &f[at + 0x20], 8);
        // The READ side of the NULL-shape refusal. The entry is NOT deleted - a re-deposit
        // overwrites an entry of this shape with a correct
        // capture - it is only flagged, and a flagged entry is never used for a substitution.
        unsigned int nsSize = 0, nsCap = 0;
        e.nullShape = entryNullShape(e.replica, e.slots, &nsSize, &nsCap);
        if (e.nullShape) {
            logW("collection: entry %s is UNUSABLE for identity - deposit that item again and it "
                 "is captured correctly", e.record.c_str());
            logD("rebuilding its slots leaves a NULL string pointer with size %u at +0x%X "
                 "(capacity %u); the entry is KEPT, but no take will substitute it",
                 nsSize, e.nullShape - 1, nsCap);
        }
        if (!e.record.empty()) into->push_back(e);
    }
    return true;
}

// ---- the TEXT reader (format 2) --------------------------------------------------------------
//
// Hand-rolled, no third-party parser, deliberately FLAT. See the contract at the top of the file.
// Nothing in here allocates a fixed buffer it can overrun and nothing recurses.

struct JsonKV {
    std::string key;
    std::string val;
    bool isString;
};

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

void skipWs(const char*& p, const char* end) {
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
}

void utf8Put(std::string* out, unsigned int cp) {
    if (cp < 0x80) {
        out->push_back((char)cp);
    } else if (cp < 0x800) {
        out->push_back((char)(0xC0u | (cp >> 6)));
        out->push_back((char)(0x80u | (cp & 0x3Fu)));
    } else {
        out->push_back((char)(0xE0u | (cp >> 12)));
        out->push_back((char)(0x80u | ((cp >> 6) & 0x3Fu)));
        out->push_back((char)(0x80u | (cp & 0x3Fu)));
    }
}

// Full JSON string unescaping. \uXXXX outside the BMP (a surrogate pair) is NOT supported and is
// emitted as the raw code unit: the writer never produces an escape above \u001F, so this can
// only be reached by a hand-edit, and a mangled non-ASCII character in a comment field is
// harmless. Every field the mod actually reads is an ASCII record path.
bool jsonString(const char*& p, const char* end, std::string* out) {
    if (p >= end || *p != '"') return false;
    ++p;
    out->clear();
    while (p < end) {
        const char c = *p++;
        if (c == '"') return true;
        if (c != '\\') {
            out->push_back(c);
            continue;
        }
        if (p >= end) return false;
        const char e = *p++;
        switch (e) {
            case '"': out->push_back('"'); break;
            case '\\': out->push_back('\\'); break;
            case '/': out->push_back('/'); break;
            case 'b': out->push_back('\b'); break;
            case 'f': out->push_back('\f'); break;
            case 'n': out->push_back('\n'); break;
            case 'r': out->push_back('\r'); break;
            case 't': out->push_back('\t'); break;
            case 'u': {
                if (end - p < 4) return false;
                unsigned int cp = 0;
                for (int i = 0; i < 4; ++i) {
                    const int h = hexVal(p[i]);
                    if (h < 0) return false;
                    cp = cp * 16u + (unsigned int)h;
                }
                p += 4;
                utf8Put(out, cp);
                break;
            }
            default: return false;
        }
    }
    return false;
}

// One flat JSON object, whole line, nothing after it. A `{` or `[` where a value is expected is
// a failure BY DESIGN - that restriction is what keeps this parser small enough to audit.
bool parseFlatObject(const char* p, const char* end, std::vector<JsonKV>* out, const char** why) {
    out->clear();
    *why = "?";
    skipWs(p, end);
    if (p >= end || *p != '{') {
        *why = "the line does not start with '{'";
        return false;
    }
    ++p;
    skipWs(p, end);
    if (p < end && *p == '}') {
        ++p;
        skipWs(p, end);
        if (p != end) {
            *why = "trailing text after '}'";
            return false;
        }
        return true;
    }
    for (;;) {
        skipWs(p, end);
        JsonKV kv;
        kv.isString = false;
        if (!jsonString(p, end, &kv.key)) {
            *why = "a key is not a quoted JSON string";
            return false;
        }
        skipWs(p, end);
        if (p >= end || *p != ':') {
            *why = "no ':' after a key";
            return false;
        }
        ++p;
        skipWs(p, end);
        if (p >= end) {
            *why = "the line ends where a value was expected";
            return false;
        }
        if (*p == '"') {
            if (!jsonString(p, end, &kv.val)) {
                *why = "an unterminated or badly escaped string value";
                return false;
            }
            kv.isString = true;
        } else if (*p == '{' || *p == '[') {
            *why = "a nested object or array - this reader is deliberately FLAT";
            return false;
        } else {
            const char* vs = p;
            while (p < end && *p != ',' && *p != '}') ++p;
            const char* ve = p;
            while (ve > vs && (ve[-1] == ' ' || ve[-1] == '\t')) --ve;
            if (ve == vs) {
                *why = "an empty value";
                return false;
            }
            kv.val.assign(vs, (size_t)(ve - vs));
        }
        out->push_back(kv);
        skipWs(p, end);
        if (p < end && *p == ',') {
            ++p;
            continue;
        }
        if (p < end && *p == '}') {
            ++p;
            skipWs(p, end);
            if (p != end) {
                *why = "trailing text after '}'";
                return false;
            }
            return true;
        }
        *why = "expected ',' or '}'";
        return false;
    }
}

// Plain decimal u32. No signs, no exponents, no hex - anything else is a bad value, not a 0.
bool jsonU32(const std::string& v, unsigned int* out) {
    if (v.empty() || v.size() > 10) return false;
    unsigned long long x = 0;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] < '0' || v[i] > '9') return false;
        x = x * 10ull + (unsigned long long)(v[i] - '0');
        if (x > 0xFFFFFFFFull) return false;
    }
    *out = (unsigned int)x;
    return true;
}

// YYYY-MM-DDTHH:MM:SS[.ffffff]Z back to a FILETIME. Nothing in the mod reads depositedAt, so a
// failure here costs a display value only and is never a reason to drop an entry.
unsigned long long fileTimeFromIso(const std::string& s) {
    unsigned int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    if (sscanf_s(s.c_str(), "%u-%u-%uT%u:%u:%u", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
    SYSTEMTIME st;
    memset(&st, 0, sizeof(st));
    st.wYear = (WORD)y;
    st.wMonth = (WORD)mo;
    st.wDay = (WORD)d;
    st.wHour = (WORD)h;
    st.wMinute = (WORD)mi;
    st.wSecond = (WORD)se;
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft)) return 0;
    unsigned long long v = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    const size_t dot = s.find('.');
    if (dot != std::string::npos) {
        unsigned long long frac = 0;
        int digits = 0;
        for (size_t i = dot + 1; i < s.size() && digits < 7; ++i) {
            if (s[i] < '0' || s[i] > '9') break;
            frac = frac * 10ull + (unsigned long long)(s[i] - '0');
            ++digits;
        }
        while (digits < 7) {
            frac *= 10ull;
            ++digits;
        }
        v += frac;
    }
    return v;
}

// Every key this format gives a meaning of its own. A slot key can never be one of these, and
// saying so EXPLICITLY is not belt-and-braces: format 3's derived "seed" key is four characters
// beginning with 's' whose last three ("eed") are all valid hex, so without this list the format-2
// slot rule below would read it as slot +0xEED, find it is not a string and not 8-aligned, and
// DROP THE WHOLE ENTRY. Add a key to the format, add it here.
bool isReservedKey(const std::string& k) {
    static const char* kReserved[] = {"record", "item",   "stored",  "lastSeen",  "deposited",
                                      "stack",  "flags",  "flagsText", "parts",   "seed",
                                      "len",    "raw",    "journal", "format",    "written",
                                      "entries", "notStored", "unknown", "saveVariant",
                                      "probe",
                                      // format 4
                                      "count", "tableCopies"};
    for (size_t i = 0; i < sizeof(kReserved) / sizeof(kReserved[0]); ++i) {
        if (k == kReserved[i]) return true;
    }
    return false;
}

// A slot key, in either shape, and the OFFSET is the only thing taken from it:
//   format 2   "s" + EXACTLY three hex digits          e.g. s090
//   format 3   <name> + "@" + EXACTLY three hex digits e.g. component@090
// The name is a comment - a build that met a slot it has no name for wrote "slot@1A0", and a
// hand-edit that renames a key changes nothing. Anything else is an ordinary unknown key and is
// skipped, which is what keeps the format forward-compatible.
bool slotKeyOffset(const std::string& key, unsigned int* off) {
    if (isReservedKey(key)) return false;
    size_t at;
    const size_t bar = key.find('@');
    if (bar != std::string::npos) {
        if (bar == 0 || key.size() != bar + 4) return false;
        at = bar + 1;
    } else {
        if (key.size() != 4 || (key[0] != 's' && key[0] != 'S')) return false;
        at = 1;
    }
    unsigned int v = 0;
    for (size_t i = at; i < at + 3; ++i) {
        const int hx = hexVal(key[i]);
        if (hx < 0) return false;
        v = v * 16u + (unsigned int)hx;
    }
    *off = v;
    return true;
}

// Builds one Entry from one parsed line. Returns false = DROP THIS ENTRY ONLY; `why` says which
// of the reader's rules it broke. Two passes on purpose: `len` bounds the slot and raw
// checks, and a hand-edited file may have reordered the keys.
bool entryFromKVs(const std::vector<JsonKV>& kvs, Entry* e, char* why, size_t whyCap) {
    e->record.clear();
    e->replica.clear();
    e->slots.clear();
    e->stack = 0;
    e->flags = 0;
    e->depositedAt = 0;
    e->nullShape = 0;
    e->stored = UT_STORED_UNKNOWN;
    e->lastSeenAt = 0;
    e->probe = false;
    // ABSENT `"count"` IS 0, and that is what makes reading a format-3 file
    // safe - every row of one is MAP-OWNED and the table holds none of it. A migration must
    // never invent a copy the user does not have.
    e->count = 0;

    unsigned int len = 0;
    bool sawRecord = false, sawLen = false;
    for (size_t i = 0; i < kvs.size(); ++i) {
        const JsonKV& kv = kvs[i];
        if (kv.key == "record") {
            if (!kv.isString) {
                _snprintf_s(why, whyCap, _TRUNCATE, "\"record\" is not a string");
                return false;
            }
            e->record = kv.val;
            sawRecord = true;
        } else if (kv.key == "len") {
            if (kv.isString || !jsonU32(kv.val, &len)) {
                _snprintf_s(why, whyCap, _TRUNCATE, "\"len\" is not a plain number");
                return false;
            }
            sawLen = true;
        } else if (kv.key == "stack") {
            if (kv.isString || !jsonU32(kv.val, &e->stack)) {
                _snprintf_s(why, whyCap, _TRUNCATE, "\"stack\" is not a plain number");
                return false;
            }
        } else if (kv.key == "flags") {
            if (kv.isString || !jsonU32(kv.val, &e->flags)) {
                _snprintf_s(why, whyCap, _TRUNCATE, "\"flags\" is not a plain number");
                return false;
            }
        } else if (kv.key == "count") {
            // Format 4. A garbled count is a DROPPED ENTRY, exactly like a
            // garbled "stack": this number decides whether the mod believes it is holding the
            // user's only copy of an item, and guessing at it is the one thing it may not do.
            if (kv.isString || !jsonU32(kv.val, &e->count)) {
                _snprintf_s(why, whyCap, _TRUNCATE, "\"count\" is not a plain number");
                return false;
            }
            if (e->count > 0x10000u) {
                _snprintf_s(why, whyCap, _TRUNCATE, "\"count\" is %u (a hand-edit; max 65536)",
                            e->count);
                return false;
            }
        } else if (kv.key == "deposited" && kv.isString) {
            e->depositedAt = fileTimeFromIso(kv.val);
        } else if (kv.key == "lastSeen" && kv.isString) {
            e->lastSeenAt = fileTimeFromIso(kv.val);
        } else if (kv.key == "probe" && !kv.isString) {
            // Only the literal true arms it. Absent, "false" or a garbled
            // value all mean "not the probe target", which is the safe answer.
            e->probe = (kv.val == "true");
        } else if (kv.key == "stored" && !kv.isString) {
            // Absent, or anything that is neither literal, stays UNKNOWN - the
            // third state is the safe one and a garbled value must never read as "not stored".
            if (kv.val == "true") e->stored = UT_STORED_YES;
            else if (kv.val == "false") e->stored = UT_STORED_NO;
        }
        // "item", "parts", "seed", "flagsText" and every unknown key are skipped here on
        // purpose: they are comments for the human, derived at write time from the fields above.
    }
    if (!sawRecord || e->record.empty()) {
        _snprintf_s(why, whyCap, _TRUNCATE, "no \"record\"");
        return false;
    }
    if (e->record.size() > 255) {
        _snprintf_s(why, whyCap, _TRUNCATE, "\"record\" is %zu bytes (the capture holds 255)",
                    e->record.size());
        return false;
    }
    // len 0 is LEGAL and must NOT drop the entry. ut_reagent's reconcile
    // synthesises exactly this shape - a record the engine's map still holds whose prototype it
    // could not read logs "NO replica" and is journalled with replicaLen 0 - and the binary
    // format kept those too. Dropping it here would (a) make the one-time migration fail its
    // field-for-field verify FOREVER, since the binary side has the entry and the text side does
    // not, and (b) silently shrink the journal, and with it the uninstall guard's count. The
    // entry is inert for identity either way: identityBuild refuses a 0-byte blob before it
    // touches anything. What it still does is say "this record is in the collection", which is
    // what journalHas / journalCount / journal_guard.ps1 need.
    if (!sawLen || len > 0x200) {
        _snprintf_s(why, whyCap, _TRUNCATE, "\"len\" is %u (must be 0..0x200)", len);
        return false;
    }

    // Pass 2: the slots and the raw words, both bounded by `len`.
    for (size_t i = 0; i < kvs.size(); ++i) {
        const JsonKV& kv = kvs[i];
        unsigned int off = 0;
        if (slotKeyOffset(kv.key, &off)) {
            if (!kv.isString) {
                _snprintf_s(why, whyCap, _TRUNCATE, "slot %s is not a string", kv.key.c_str());
                return false;
            }
            if (e->slots.size() >= 24) {
                _snprintf_s(why, whyCap, _TRUNCATE, "more than 24 slots (the capture holds 24)");
                return false;
            }
            if ((off & 7u) != 0) {
                _snprintf_s(why, whyCap, _TRUNCATE, "slot offset +0x%X is not 8-aligned", off);
                return false;
            }
            // len 0 (see the note above) has no replica for a slot to fit INTO, so the fit test
            // is meaningless there; the slots are carried through verbatim so the round trip
            // stays exact, and every consumer refuses a 0-byte blob before it can read them
            // (entryNullShape returns 0 below 0x20 bytes, identityBuild refuses outright).
            if (len && off + 0x20 > len) {
                _snprintf_s(why, whyCap, _TRUNCATE,
                            "slot offset +0x%X does not fit a %u-byte replica", off, len);
                return false;
            }
            if (kv.val.size() >= 160) {
                _snprintf_s(why, whyCap, _TRUNCATE, "slot +0x%X carries %zu bytes of text (max 159)",
                            off, kv.val.size());
                return false;
            }
            Slot sl;
            sl.off = off;
            sl.text = kv.val;
            e->slots.push_back(sl);
        }
    }

    // The blob: zero, then apply every raw word. THE ORDER IS FIXED (see the file header).
    e->replica.assign(len, 0);
    for (size_t i = 0; i < kvs.size(); ++i) {
        const JsonKV& kv = kvs[i];
        if (kv.key != "raw") continue;
        if (!kv.isString) {
            _snprintf_s(why, whyCap, _TRUNCATE, "\"raw\" is not a string");
            return false;
        }
        const char* p = kv.val.c_str();
        const char* end = p + kv.val.size();
        while (p < end) {
            while (p < end && (*p == ' ' || *p == '\t')) ++p;
            if (p >= end) break;
            unsigned int off = 0;
            int n = 0;
            while (p < end && hexVal(*p) >= 0 && n < 8) {
                off = off * 16u + (unsigned int)hexVal(*p);
                ++p;
                ++n;
            }
            if (!n || p >= end || *p != ':') {
                _snprintf_s(why, whyCap, _TRUNCATE, "a \"raw\" token is not <hex>:<hex>");
                return false;
            }
            ++p;
            unsigned long long word = 0;
            n = 0;
            while (p < end && hexVal(*p) >= 0 && n < 8) {
                word = word * 16ull + (unsigned long long)hexVal(*p);
                ++p;
                ++n;
            }
            if (!n) {
                _snprintf_s(why, whyCap, _TRUNCATE, "a \"raw\" token has no value");
                return false;
            }
            if (p < end && *p != ' ' && *p != '\t') {
                _snprintf_s(why, whyCap, _TRUNCATE, "a \"raw\" token has trailing junk");
                return false;
            }
            // THE BOUNDS TEST MUST NOT WRAP. The offset scanner above accepts up to 8 hex
            // digits, so a hand-edited "FFFFFFFC:..." reaches here; with `off + 4 > len` that
            // sum wraps to 0, 0 > len is false for every legal len, the 4-alignment test passes
            // as well, and the memcpy below would write four bytes 4 GB past a 512-byte vector.
            // Subtracting instead can never wrap.
            if ((off & 3u) != 0 || off > len || len - off < 4) {
                _snprintf_s(why, whyCap, _TRUNCATE,
                            "\"raw\" offset +0x%X is not 4-aligned or does not fit %u bytes", off,
                            len);
                return false;
            }
            const unsigned int w = (unsigned int)word;
            memcpy(&e->replica[off], &w, 4);
        }
    }

    // Format-independent: rebuild the slots into a scratch copy and look for the one
    // std::string shape that is guaranteed to fault.
    unsigned int nsSize = 0, nsCap = 0;
    e->nullShape = entryNullShape(e->replica, e->slots, &nsSize, &nsCap);
    if (e->nullShape) {
        logW("collection: entry %s is UNUSABLE for identity - deposit that item again and it is "
             "captured correctly", e->record.c_str());
        logD("rebuilding its slots leaves a NULL string pointer with size %u at +0x%X "
             "(capacity %u); the entry is KEPT, but no take will substitute it",
             nsSize, e->nullShape - 1, nsCap);
    }
    // THE ONE INVARIANT THE FILE CAN CONTRADICT. `count >= 1` means the mod's
    // own file is the only place this item exists, and `"stored":false` means "the collection
    // does not hold it". Both cannot be true. The repair goes the ONLY direction that cannot
    // lose an item: the count is kept and the mark is corrected to stored. (A hand-edit is the
    // realistic source; the mod itself always writes the pair together - journalSetCount marks
    // NO at 0 and journalUpsertCount marks YES above it.)
    if (e->count >= 1 && e->stored != UT_STORED_YES) {
        logD("rescue journal: entry %s says \"count\":%u but \"stored\" is %s - the COUNT wins "
             "and the entry is marked stored. A count of 1 or more means the item lives in this "
             "file and nowhere else, so believing the mark instead would throw it away.",
             e->record.c_str(), e->count,
             e->stored == UT_STORED_NO ? "false" : "absent (not yet reconciled)");
        e->stored = UT_STORED_YES;
    }
    return true;
}

struct TextParse {
    bool ok;                    // the file existed and line 1 was a usable header
    unsigned int format;
    unsigned int headerEntries;
    int badLines;
    int dropped;
    bool tooNew;                // format > UT_JOURNAL_FORMAT
};

// Parses a whole buffer. A bad line costs THAT LINE ONLY. `quiet` silences the per-line noise for
// the migration's verify pass, which re-reads a file it just wrote.
TextParse parseTextBuffer(const std::vector<unsigned char>& f, const char* path,
                          std::vector<Entry>* into, bool quiet) {
    TextParse r;
    r.ok = false;
    r.format = 0;
    r.headerEntries = 0;
    r.badLines = 0;
    r.dropped = 0;
    r.tooNew = false;

    const char* base = f.empty() ? nullptr : (const char*)&f[0];
    const size_t total = f.size();
    size_t at = 0;
    int lineNo = 0;
    bool headerSeen = false;
    std::vector<JsonKV> kvs;

    while (at <= total) {
        if (at == total && lineNo > 0) break;
        size_t nl = at;
        while (nl < total && base[nl] != '\n') ++nl;
        const char* ls = base + at;
        size_t ll = nl - at;
        while (ll && (ls[ll - 1] == '\r')) --ll;
        const size_t nextAt = nl < total ? nl + 1 : total + 1;
        at = nextAt;
        ++lineNo;

        // blank line: skipped, never an error
        {
            size_t k = 0;
            while (k < ll && (ls[k] == ' ' || ls[k] == '\t')) ++k;
            if (k == ll) continue;
        }

        const char* why = "?";
        if (!parseFlatObject(ls, ls + ll, &kvs, &why)) {
            if (!headerSeen) {
                logE("collection: line 1 is not a JSON object (%s) - the file is IGNORED", why);
                logD("\"%s\" is left exactly as it is on disk", path);
                return r;
            }
            ++r.badLines;
            char head[88];
            const size_t take = ll < 80 ? ll : 80;
            memcpy(head, ls, take);
            head[take] = 0;
            for (size_t k = 0; k < take; ++k) {
                if ((unsigned char)head[k] < 0x20) head[k] = '.';
            }
            logW("collection: line %d does not parse (%s) - that ONE entry is lost, the rest of "
                 "the file is fine", lineNo, why);
            logD("\"%s\" line %d began: %s", path, lineNo, head);
            continue;
        }

        if (!headerSeen) {
            headerSeen = true;
            bool sawFormat = false;
            for (size_t i = 0; i < kvs.size(); ++i) {
                if (kvs[i].key == "format") {
                    if (kvs[i].isString || !jsonU32(kvs[i].val, &r.format)) break;
                    sawFormat = true;
                } else if (kvs[i].key == "entries" && !kvs[i].isString) {
                    jsonU32(kvs[i].val, &r.headerEntries);
                } else if (kvs[i].key == "saveVariant" && !kvs[i].isString) {
                    // Which shared reagent stash (hardcore or
                    // softcore) this file was written against. Only ever ADOPTED here - never
                    // overwritten - so a journal that has seen both variants keeps the first and
                    // the reconcile stays conservative on the other.
                    unsigned int v = 0;
                    if (jsonU32(kvs[i].val, &v) && v <= 255) {
                        InterlockedExchange(&g_saveVariant, (LONG)v);
                    }
                }
            }
            if (!sawFormat || r.format < 1) {
                logE("collection: line 1 has no usable \"format\" number - the file is IGNORED");
                logD("\"%s\" is left exactly as it is on disk", path);
                return r;
            }
            if (r.format > UT_JOURNAL_FORMAT) {
                r.tooNew = true;
                logE("***** collection: the file is format %u and this build reads at most %u - "
                     "READ-ONLY this session; update the mod *****",
                     r.format, (unsigned int)UT_JOURNAL_FORMAT);
                logD("\"%s\" is loaded as far as it can be; deposits will NOT be recorded and "
                     "nothing is overwritten", path);
            }
            // The format-3 (or 2) UPGRADE, said once, at the top of the read.
            // There is no per-entry migration to announce because there is no per-entry change:
            // an older file has no counts, the private table therefore holds nothing of it, and
            // every row stays exactly what it was - a record the ENGINE's map is holding.
            if (!quiet && r.format < (unsigned int)UT_JOURNAL_FORMAT) {
                logI("collection: the file is format %u and this build writes %u - it is read in "
                     "full and upgraded by the next write",
                     r.format, (unsigned int)UT_JOURNAL_FORMAT);
                logD("every entry starts at \"count\":0 - the private table holds no copy of it "
                     "and the item is where it has always been, in the game's own reagent stash");
            }
            r.ok = true;
            continue;
        }

        Entry e;
        char why2[192] = {0};
        if (!entryFromKVs(kvs, &e, why2, sizeof(why2))) {
            ++r.dropped;
            logW("***** rescue journal: %s line %d is DROPPED (%s) - that ONE entry is lost, the "
                 "rest of the file is fine",
                 path, lineNo, why2);
            continue;
        }
        // Duplicate record, case-insensitively, exactly as findEntry compares: keep the LAST,
        // matching journalUpsert's replace semantics.
        bool replaced = false;
        for (size_t i = 0; i < into->size(); ++i) {
            if (_stricmp((*into)[i].record.c_str(), e.record.c_str()) == 0) {
                if (!quiet) {
                    logD("rescue journal: %s line %d repeats record %s - the LAST one wins (that "
                         "is what a re-deposit does)",
                         path, lineNo, e.record.c_str());
                }
                (*into)[i] = e;
                replaced = true;
                break;
            }
        }
        if (!replaced) into->push_back(e);
    }
    if (!headerSeen) {
        logD("rescue journal: %s is empty - ignored", path);
        return r;
    }
    return r;
}

// Reads the real journal into g_entries. Returns true when a file was there AND parsed.
bool readTextFile(const char* path) {
    std::vector<unsigned char> f;
    if (!readWholeFile(path, &f)) return false;   // absent, empty, or over the 64 MB cap
    const TextParse r = parseTextBuffer(f, path, g_entries, false);
    if (!r.ok) return false;
    if (r.tooNew) InterlockedExchange(&g_readOnly, 1);
    const size_t got = g_entries->size();
    if (r.headerEntries != (unsigned int)got || r.badLines || r.dropped) {
        // WARN, never refuse. One broken line must not cost 355 good entries - but the file is
        // copied aside before the next write replaces it.
        InterlockedExchange(&g_copyAside, 1);
        logW("***** collection: the file says %u entries and %zu were read (%d unparseable, %d "
             "dropped) - the good ones are kept *****",
             r.headerEntries, got, r.badLines, r.dropped);
        logD("\"%s\" is copied aside as <collection>.bad-<stamp> before the next write replaces "
             "it", path);
    }
    return true;
}

// ---- the migration --------------------------------------------------------------------------

void captureFromEntry(const Entry& e, UtReplicaCapture* out) {
    memset(out, 0, sizeof(*out));
    _snprintf_s(out->record, sizeof(out->record), _TRUNCATE, "%s", e.record.c_str());
    unsigned int n = (unsigned int)e.replica.size();
    if (n > sizeof(out->replica)) n = (unsigned int)sizeof(out->replica);
    if (n) memcpy(out->replica, &e.replica[0], n);
    out->replicaLen = n;
    out->stack = e.stack;
    out->flags = e.flags;
    for (size_t i = 0; i < e.slots.size() && out->slotCount < 24; ++i) {
        out->slotOff[out->slotCount] = e.slots[i].off;
        _snprintf_s(out->slotText[out->slotCount], 160, _TRUNCATE, "%s", e.slots[i].text.c_str());
        ++out->slotCount;
    }
}

// A rebuilt slot longer than 15 characters is a HEAP std::string, so its first 8 bytes are a
// pointer into the overlay's own `strings` array - a live address, different in every overlay and
// every process. It is the one part of identityBuild's output that is meaningless to compare, so
// it is zeroed before the comparison. Everything else, including the SSO slots' own bytes, the
// sizes and the capacities, is compared exactly. (utReadMsvcString's own rule is used to tell the
// two shapes apart: capacity == 15 means the text is in the SSO buffer.)
void maskOverlayPointers(UtIdentityOverlay* ov) {
    for (int i = 0; i < ov->slotCount; ++i) {
        const unsigned int off = ov->slotOff[i];
        if (off + 0x20 > ov->replicaLen) continue;
        size_t capacity = 0;
        memcpy(&capacity, ov->replica + off + 0x18, sizeof(capacity));
        if (capacity == 15) continue;  // SSO: the bytes ARE the text, compare them
        memset(ov->replica + off, 0, 8);
    }
}

// RANK 2 in the dig's risk register: "a migration that verifies too weakly". A count-only check
// would bless a subtly wrong text file and then rename the .bin away. So this compares EVERY
// field of EVERY entry - record (case-insensitively, as findEntry does), len, stack, flags, the
// slot COUNT (not just the non-empty ones - RANK 3), every slot offset and text, and the
// reconstructed replica bytes - AND, belt and braces, it runs identityBuild() over both sides
// with the same incoming replica and requires the engine-facing output and the verdict to be
// identical. That last check is the offline proof (prove_lean.py, 356/356) done at run time.
bool sameEntry(const Entry& a, const Entry& b, char* why, size_t whyCap) {
    if (_stricmp(a.record.c_str(), b.record.c_str()) != 0) {
        _snprintf_s(why, whyCap, _TRUNCATE, "record \"%s\" came back as \"%s\"", a.record.c_str(),
                    b.record.c_str());
        return false;
    }
    if (a.replica.size() != b.replica.size()) {
        _snprintf_s(why, whyCap, _TRUNCATE, "%s: len %zu came back as %zu", a.record.c_str(),
                    a.replica.size(), b.replica.size());
        return false;
    }
    if (a.stack != b.stack || a.flags != b.flags) {
        _snprintf_s(why, whyCap, _TRUNCATE, "%s: stack/flags %u/%u came back as %u/%u",
                    a.record.c_str(), a.stack, a.flags, b.stack, b.flags);
        return false;
    }
    if (a.slots.size() != b.slots.size()) {
        _snprintf_s(why, whyCap, _TRUNCATE, "%s: %zu slots came back as %zu", a.record.c_str(),
                    a.slots.size(), b.slots.size());
        return false;
    }
    for (size_t i = 0; i < a.slots.size(); ++i) {
        if (a.slots[i].off != b.slots[i].off || a.slots[i].text != b.slots[i].text) {
            _snprintf_s(why, whyCap, _TRUNCATE, "%s: slot %zu (+0x%X \"%s\") came back as +0x%X "
                        "\"%s\"",
                        a.record.c_str(), i, a.slots[i].off, a.slots[i].text.c_str(),
                        b.slots[i].off, b.slots[i].text.c_str());
            return false;
        }
    }
    std::vector<unsigned char> lean;
    leanReplica(a, &lean);
    if (lean.size() != b.replica.size() ||
        (!lean.empty() && memcmp(&lean[0], &b.replica[0], lean.size()) != 0)) {
        _snprintf_s(why, whyCap, _TRUNCATE, "%s: the replica words did not come back",
                    a.record.c_str());
        return false;
    }
    if (a.nullShape != b.nullShape) {
        _snprintf_s(why, whyCap, _TRUNCATE, "%s: the NULL-shape verdict changed (%u -> %u)",
                    a.record.c_str(), a.nullShape, b.nullShape);
        return false;
    }
    // The engine-facing proof: identityBuild's output must be byte-identical from both sides.
    if (!a.replica.empty() && a.replica.size() <= 0x200) {
        UtReplicaCapture ca, cb;
        captureFromEntry(a, &ca);
        captureFromEntry(b, &cb);
        UtIdentityOverlay oa, ob;
        const bool ra = identityBuild(ca, &a.replica[0], (unsigned int)a.replica.size(), 0, &oa);
        const bool rb = identityBuild(cb, &a.replica[0], (unsigned int)a.replica.size(), 0, &ob);
        maskOverlayPointers(&oa);
        maskOverlayPointers(&ob);
        if (ra != rb || oa.replicaLen != ob.replicaLen || oa.slotCount != ob.slotCount ||
            memcmp(oa.replica, ob.replica, oa.replicaLen) != 0 || strcmp(oa.why, ob.why) != 0) {
            _snprintf_s(why, whyCap, _TRUNCATE,
                        "%s: identityBuild disagrees between the .bin and the text (%s / %s)",
                        a.record.c_str(), oa.why, ob.why);
            return false;
        }
    }
    return true;
}

// Reads `binPath` with the legacy reader, writes the text file, VERIFIES it off disk field by
// field, and only then renames the .bin aside. g_entries holds the migrated entries either way -
// a failed migration still gives the session working identities, it just does not touch the .bin
// and re-tries at the next start.
bool migrateBinary(const char* binPath, const char* whence) {
    std::vector<Entry> fromBin;
    if (!readBinaryFileInto(binPath, &fromBin)) return false;

    logI("collection: MIGRATING %zu entr%s to %s ...", fromBin.size(),
         fromBin.size() == 1 ? "y" : "ies", g_path);
    logD("the source is the binary journal %s (%s)", binPath, whence);

    std::string text;
    buildText(fromBin, &text);
    *g_entries = fromBin;   // the session runs from these whatever happens below

    if (!writeWholeFileAtomic(g_path, text.c_str(), text.size())) {
        logE("***** collection: MIGRATION FAILED - \"%s\" could not be written (err %lu) *****",
             g_path, GetLastError());
        logD("%s is still the record and is UNTOUCHED; the %zu entries are loaded in memory so "
             "this session's identities work, and the migration is re-tried next start",
             binPath, fromBin.size());
        InterlockedExchange(&g_dirty, 1);
        return false;
    }

    // VERIFY: re-read what we just wrote, off the disk, with the real reader.
    std::vector<unsigned char> back;
    std::vector<Entry> fromText;
    char why[256] = {0};
    bool good = false;
    if (!readWholeFile(g_path, &back)) {
        _snprintf_s(why, sizeof(why), _TRUNCATE, "the file could not be read back at all");
    } else {
        const TextParse r = parseTextBuffer(back, g_path, &fromText, true);
        if (!r.ok) {
            _snprintf_s(why, sizeof(why), _TRUNCATE, "the file we just wrote does not parse");
        } else if (fromText.size() != fromBin.size()) {
            _snprintf_s(why, sizeof(why), _TRUNCATE, "%zu entries went in and %zu came back",
                        fromBin.size(), fromText.size());
        } else {
            good = true;
            for (size_t i = 0; i < fromBin.size() && good; ++i) {
                if (!sameEntry(fromBin[i], fromText[i], why, sizeof(why))) good = false;
            }
        }
    }
    if (!good) {
        char stamp[32], rejected[MAX_PATH];
        stampNow(stamp, sizeof(stamp));
        _snprintf_s(rejected, sizeof(rejected), _TRUNCATE, "%s.rejected-%s", g_path, stamp);
        MoveFileExA(g_path, rejected, 0);
        // Renaming the text aside is not enough on its own. g_entries still
        // holds everything read from the .bin, and without this latch the FIRST deposit of the
        // session would set g_dirty and journalService would write exactly the content the
        // verify refused straight back to g_path - unverified, and from the next start on rule 1
        // would trust it forever. READ-ONLY for the session: the session runs from memory, the
        // .bin stays the record, and nothing can establish rejected content without a passing
        // verify at the next start.
        InterlockedExchange(&g_readOnly, 1);
        logE("***** collection: MIGRATION FAILED THE VERIFY (%s) - READ-ONLY this session *****",
             why);
        logD("the new file was moved to %s so it can never be mistaken for the record, %s is "
             "UNTOUCHED and stays the record, this session's deposits are held in memory only, "
             "and the migration is re-tried next start",
             rejected, binPath);
        return false;
    }

    // Only now is the old file allowed to move. NO REPLACE_EXISTING, so a second migration (a
    // user restoring a backup) can never clobber an earlier one. NEVER DELETED.
    char stamp[32], kept[MAX_PATH];
    stampNow(stamp, sizeof(stamp));
    _snprintf_s(kept, sizeof(kept), _TRUNCATE, "%s.migrated-%s", binPath, stamp);
    if (MoveFileExA(binPath, kept, 0)) {
        logI("collection: MIGRATED %zu entr%s to %s, verified field by field",
             fromBin.size(), fromBin.size() == 1 ? "y" : "ies", g_path);
        logD("the old file is KEPT as %s and is never deleted - keep it until you have taken a "
             "few items back out successfully", kept);
    } else {
        logI("collection: MIGRATED %zu entr%s to %s, verified field by field",
             fromBin.size(), fromBin.size() == 1 ? "y" : "ies", g_path);
        logW("collection: the old file %s could not be renamed (err %lu) - it is left where it "
             "is and ignored from now on", binPath, GetLastError());
    }
    InterlockedExchange(&g_dirty, 0);
    return true;
}

// A .bin that will not parse is evidence. Copy it aside before anything else can touch it.
void keepUnreadable(const char* binPath) {
    char stamp[32], aside[MAX_PATH];
    stampNow(stamp, sizeof(stamp));
    _snprintf_s(aside, sizeof(aside), _TRUNCATE, "%s.unreadable-%s", binPath, stamp);
    if (CopyFileA(binPath, aside, TRUE)) {
        logE("***** collection: %s exists but does NOT parse - the mod starts with an EMPTY "
             "collection *****", binPath);
        logD("a copy is kept as %s so nothing can destroy the evidence, and the original is left "
             "exactly where it is", aside);
    } else {
        logE("***** collection: %s exists but does NOT parse - the mod starts with an EMPTY "
             "collection *****", binPath);
        logW("collection: %s could not be copied aside (err %lu) - it is left exactly where it is",
             binPath, GetLastError());
    }
}

// ---- the save folder -----------------------------------------------------------------------
bool fileExists(const char* path) {
    const DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// The three names the collection lives under, for one mode. Softcore keeps the
// original trio EXACTLY - an existing collection must not move because this build landed - and
// hardcore gets its own `-hc` trio beside it, in the same folder, under the same `journal_dir`.
void composePaths(const char* dir, int mode) {
    const char* suffix = mode == 1 ? "-hc" : "";
    _snprintf_s(g_path, sizeof(g_path), _TRUNCATE, "%s\\uniq-items%s.jsonl", dir, suffix);
    _snprintf_s(g_csvPath, sizeof(g_csvPath), _TRUNCATE, "%s\\uniq-export%s.csv", dir, suffix);
    _snprintf_s(g_gdsPath, sizeof(g_gdsPath), _TRUNCATE, "%s\\uniq-export%s.gds", dir, suffix);
}

}  // namespace

bool journalInit(HMODULE selfModule) {
    return journalInit(selfModule, nullptr);
}

// `journalDir` is the ini key `journal_dir` (empty = the out-dir). It is read
// ONCE, here, and never again: configReload replaces g_cfg once a second and a journal that could
// move under a running game would be a footgun. Everything else - the log, the ini, the
// diagnostics - stays in the out-dir whatever this says.
bool journalInit(HMODULE selfModule, const char* journalDir) {
    if (!g_csReady) {
        InitializeCriticalSection(&g_cs);
        g_csReady = true;
    }
    if (g_entries) return true;
    try {
        g_entries = new std::vector<Entry>();
    } catch (...) {
        return false;
    }
    char outDir[MAX_PATH] = {0};
    utModDirA(selfModule, outDir, sizeof(outDir));
    utModPathA(selfModule, "rescue-report.txt", g_reportPath, sizeof(g_reportPath));
    g_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // Name the resolved folder and say WHY it won, every session, in one line.
    logI("mod folder = \"%s\" (%s)", outDir, utModDirWhy());
    logD("the log, the settings file and rescue-report.txt all live there");

    char dir[MAX_PATH];
    _snprintf_s(dir, sizeof(dir), _TRUNCATE, "%s", outDir);
    if (journalDir && journalDir[0]) {
        // Trailing slashes and quotes are what a hand-edited ini actually contains.
        char want[MAX_PATH];
        _snprintf_s(want, sizeof(want), _TRUNCATE, "%s", journalDir);
        size_t n = strlen(want);
        while (n && (want[n - 1] == '\\' || want[n - 1] == '/' || want[n - 1] == ' ' ||
                     want[n - 1] == '"')) {
            want[--n] = 0;
        }
        const char* from = want;
        if (from[0] == '"') ++from;
        const DWORD a = GetFileAttributesA(from);
        if (from[0] && a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
            _snprintf_s(dir, sizeof(dir), _TRUNCATE, "%s", from);
            logI("collection: journal_dir=%s - the collection is kept there", dir);
            logD("journal_dir is read once at start-up and never re-read");
        } else {
            logW("***** rescue journal: journal_dir=%s in the ini is not an existing folder - "
                 "IGNORED, the journal stays in %s *****",
                 journalDir, outDir);
        }
    }
    // The CSV export lives beside the journal, always - it is a derived view of that one file
    // and moving it anywhere else would only make the pair harder to copy. It is not a place a
    // user should have to think about. The GD Stash import file is beside it for the same
    // reason. All three are named for the MODE (composePaths). Start-up runs before any
    // character exists, so there is nobody to ask which collection is his yet and the softcore
    // names are what the journal opens on; journalFollowMode() moves it when the first world
    // says otherwise. The folder is kept, because that re-open has to compose the paths again.
    _snprintf_s(g_dir, sizeof(g_dir), _TRUNCATE, "%s", dir);
    composePaths(g_dir, (int)InterlockedCompareExchange(&g_mode, 0, 0));

    // ---- the ladder. ONE rule decides: the ABSENCE of uniq-items.jsonl means
    // ---- convert. No content sniffing, no timestamps - which is what makes it idempotent.
    char binPath[MAX_PATH];
    _snprintf_s(binPath, sizeof(binPath), _TRUNCATE, "%s\\uniq-items.bin", dir);
    const bool haveBin = fileExists(binPath);

    bool had = readTextFile(g_path);
    // readTextFile also returns false when the file IS there and could not be
    // used: share-locked by an editor or an antivirus, a read error part way, zero bytes, over the
    // 64 MB cap, or a line 1 that is not a usable header. Falling through from there would be the
    // worst outcome in the file: zero entries, still writable, so the next deposit REPLACES a
    // 356-entry journal with a one-entry one - and, worse, the ladder below would migrate a .bin
    // straight over the file we could not read. So it counts as "there is a journal here" (no
    // migration on top of it) and the journal goes READ-ONLY for the session. Nothing on disk is
    // touched; the next start tries again.
    if (!had && fileExists(g_path)) {
        InterlockedExchange(&g_readOnly, 1);
        logE("***** collection: %s EXISTS but could not be read - READ-ONLY this session; close "
             "whatever is holding the file and restart *****", g_path);
        logD("it is left exactly as it is on disk, nothing is migrated on top of it and nothing "
             "can overwrite it - this session's deposits are held in memory only");
        had = true;
    }
    if (had) {
        // Rule 1. NEVER migrate on top of an existing readable journal.
        if (haveBin) {
            logD("rescue journal: %s already exists, so the old binary journal (%s) is IGNORED "
                 "and left exactly where it is. Nothing is ever migrated on top of a journal "
                 "that is already there.",
                 g_path, binPath);
        }
    } else if (haveBin) {
        if (!migrateBinary(binPath, "the mod's own folder")) {
            if (g_entries->empty()) keepUnreadable(binPath);
        }
        had = true;
    }
    logI("collection: %zu entr%s in \"%s\"%s%s", g_entries->size(),
         g_entries->size() == 1 ? "y" : "ies", g_path, had ? "" : " (no file yet)",
         InterlockedCompareExchange(&g_readOnly, 0, 0) ? " [READ-ONLY this session]" : "");
    return true;
}

void journalSetCsvExport(int mode) {
    // Clamp rather than refuse: a hand-typed export_csv=7 must not turn the export off, and a
    // negative one must not be read as "mode 4294967295". Out of range HIGH means "everything"
    // and out of range LOW means "off" - the only two answers that cannot surprise a user.
    LONG want = mode < 0 ? 0 : (mode > 2 ? 2 : (LONG)mode);
    const LONG had = InterlockedExchange(&g_csvMode, want);
    if (had == want || want == 0) return;
    // The mode CHANGED and the new one writes a file. Arm the export so the CSV on
    // disk catches up on the worker's next pass instead of staying stale until the next deposit -
    // the user flips this key to look at the file, not to wait for it. Mode 0 is deliberately
    // excluded: there is nothing to write, and the existing file is left alone.
    //
    // This arms g_csvArm, NOT g_dirty. Setting g_dirty would make journalService rewrite
    // uniq-items.jsonl as well - content-identical and safe, but an ini key about a derived
    // export has no business rewriting the journal, and at start-up with export_csv=2 in the
    // ini it would do so on EVERY launch (had=1 -> want=2 is a change). The
    // CSV-only pass in journalService takes the same lock and uses the same csvWrite; nothing
    // here writes anything itself.
    //
    // No "is the journal empty" guard any more: that guard existed only because g_dirty would
    // have created an empty uniq-items.jsonl on a machine that has never deposited anything.
    // A CSV-only pass writes the CSV and nothing else, so an empty collection now honestly
    // produces the header line on its own - which is what the USER-GUIDE promises.
    InterlockedExchange(&g_csvArm, 1);
    if (g_event) SetEvent(g_event);
}

const char* journalCsvPath() {
    return g_csvPath;
}

long journalCsvWrites() {
    return InterlockedCompareExchange(&g_csvWrites, 0, 0);
}

// The same shape as journalSetCsvExport above, deliberately line for line: the
// two exports are independent (both may be on at once), share nothing but the lock and the
// entry list, and neither can ever touch g_dirty. See ut_rescue.h for the mode table.
void journalSetGdsExport(int mode) {
    LONG want = mode < 0 ? 0 : (mode > 2 ? 2 : (LONG)mode);
    const LONG had = InterlockedExchange(&g_gdsMode, want);
    if (had == want || want == 0) return;
    InterlockedExchange(&g_gdsArm, 1);
    if (g_event) SetEvent(g_event);
}

const char* journalGdsPath() {
    return g_gdsPath;
}

long journalGdsWrites() {
    return InterlockedCompareExchange(&g_gdsWrites, 0, 0);
}

HANDLE journalEvent() {
    return g_event;
}


// ---- the CSV export -------------------------------------------------------------------------
// RFC 4180: fields are separated by commas, records by CRLF, and a field is quoted only when it
// has to be - when it carries a comma, a double quote, a CR or an LF - with every embedded quote
// doubled. That is the whole spec this needs, and writing the minimal form keeps the file
// diffable. No BOM: every field here is a DBR path, an ISO stamp, a number or a catalogue display
// name, and the journal's own display names are the only place non-ASCII could appear at all.
// A throw inside csvField must not be swallowed silently: it would leave the row one separator
// short, and csvWrite would then publish that malformed file. The flag is
// worker-thread-only (csvBuild/csvWrite are called under g_cs) and csvWrite abandons the write.
bool g_csvFieldOk = true;

void csvField(std::string* out, const char* text, bool last) {
    const char* t = text ? text : "";
    bool needQuote = false;
    for (const char* p = t; *p; ++p) {
        if (*p == ',' || *p == '"' || *p == '\r' || *p == '\n') {
            needQuote = true;
            break;
        }
    }
    try {
        if (needQuote) {
            out->push_back('"');
            for (const char* p = t; *p; ++p) {
                if (*p == '"') out->push_back('"');
                out->push_back(*p);
            }
            out->push_back('"');
        } else {
            out->append(t);
        }
        out->append(last ? "\r\n" : ",");
    } catch (...) {
        g_csvFieldOk = false;
    }
}

// The record path stored in one named slot of this entry, or "" when the entry has none. The
// OFFSET is authoritative (the slot name in the journal is a comment), which is why this looks
// the offset up rather than the name.
const char* slotTextAt(const Entry& e, unsigned int off) {
    for (size_t i = 0; i < e.slots.size(); ++i) {
        if (e.slots[i].off == off) return e.slots[i].text.c_str();
    }
    return "";
}

// One row. Caller holds g_cs, has already written the header and owns the block order.
void csvRow(std::string* out, const Entry& e) {
    char num[64];
    char iso[64];
    csvField(out, e.record.c_str(), false);
    char item[192];
    if (!journalItemName(e.record.c_str(), item, sizeof(item))) item[0] = 0;
    csvField(out, item, false);
    csvField(out, e.stored == UT_STORED_YES ? "yes" : e.stored == UT_STORED_NO ? "no" : "unknown",
             false);
    // The 14th column: how many copies of this record live in
    // the mod's OWN file and nowhere else. 0 is the ordinary value today - the item is in the
    // game's reagent stash, exactly where it has always been.
    _snprintf_s(num, sizeof(num), _TRUNCATE, "%u", e.count);
    csvField(out, num, false);
    isoFromFileTime(e.depositedAt, iso, sizeof(iso));
    csvField(out, iso, false);
    if (e.lastSeenAt) {
        isoFromFileTime(e.lastSeenAt, iso, sizeof(iso));
    } else {
        iso[0] = 0;
    }
    csvField(out, iso, false);
    _snprintf_s(num, sizeof(num), _TRUNCATE, "%u", e.stack);
    csvField(out, num, false);
    csvField(out, flagsText(e.flags), false);
    std::string parts;
    partsText(e, &parts);
    csvField(out, parts.c_str(), false);
    const unsigned int len = (unsigned int)e.replica.size();
    num[0] = 0;
    if (len >= kSeedOffset + 4 && !offInAnySlot(e.slots, kSeedOffset)) {
        unsigned int seed = 0;
        memcpy(&seed, &e.replica[kSeedOffset], 4);
        if (seed) _snprintf_s(num, sizeof(num), _TRUNCATE, "%u", seed);
    }
    csvField(out, num, false);
    csvField(out, slotTextAt(e, 0x028), false);  // prefix
    csvField(out, slotTextAt(e, 0x048), false);  // suffix
    csvField(out, slotTextAt(e, 0x090), false);  // component
    csvField(out, slotTextAt(e, 0x0D8), true);   // augment
}

// Builds the whole file in memory. Caller holds g_cs. `mode` is 1 (the collection) or 2
// (everything) - csvWrite has already dealt with 0 and with anything out of range.
//
// The two blocks are the journal writer's own two blocks (buildText), for the
// same reason and in the same order: stored-or-not-yet-reconciled first, then the history. So
// the ONLY difference between mode 1 and mode 2 is whether the second block is written at all,
// and mode 1's file is byte-for-byte mode 2's file with its tail removed. The header row is
// identical in both - 14 columns - so a spreadsheet that
// reads one reads the other.
//
// UNKNOWN (no `"stored"` key: an entry no reconciliation has looked at yet) goes in the FIRST
// block and is therefore exported by BOTH modes. That is the load-bearing half of this change:
// "we have not checked yet" must never be exported as "not in your collection", or the first
// run after a restart - when every entry read off the disk is unknown until the caravan is
// opened - would hand the user an empty file and call it their collection.
void csvBuild(std::string* out, int mode) {
    out->clear();
    g_csvFieldOk = true;
    out->append("record,item,stored,count,deposited,lastSeen,stack,flags,parts,seed,prefix,"
                "suffix,component,augment\r\n");
    for (size_t i = 0; i < g_entries->size(); ++i) {
        if ((*g_entries)[i].stored == UT_STORED_NO) continue;
        csvRow(out, (*g_entries)[i]);
    }
    if (mode < 2) return;
    for (size_t i = 0; i < g_entries->size(); ++i) {
        if ((*g_entries)[i].stored != UT_STORED_NO) continue;
        csvRow(out, (*g_entries)[i]);
    }
}

// Worker thread, called straight after a successful journal write, with g_cs still held.
// A failure here is REPORTED and then forgotten: nothing reads this file, so a stale or missing
// uniq-export.csv can never cost the user an item, and re-arming g_dirty over it would loop.
void csvWrite() {
    // Read the mode ONCE: storeTick pushes the ini key in from another thread, and a build that
    // saw mode 2 for its first block and mode 1 for its second would publish a file no mode ever
    // asked for.
    const LONG mode = InterlockedCompareExchange(&g_csvMode, 0, 0);
    if (mode <= 0) return;
    if (!g_csvPath[0] || !g_entries) return;
    std::string text;
    try {
        csvBuild(&text, (int)mode);
    } catch (...) {
        return;
    }
    if (!g_csvFieldOk) {
        if (!InterlockedExchange(&g_csvFailLogged, 1)) {
            logW("export: %s was ABANDONED - a field could not be built", g_csvPath);
            logD("the collection file itself was written and is intact; nothing reads the CSV "
                 "back, so this costs no data");
        }
        return;
    }
    if (writeWholeFileAtomic(g_csvPath, text.c_str(), text.size())) {
        InterlockedIncrement(&g_csvWrites);
        InterlockedExchange(&g_csvFailLogged, 0);
        return;
    }
    if (!InterlockedExchange(&g_csvFailLogged, 1)) {
        logW("export: %s could NOT be written", g_csvPath);
        logD("the collection file itself was written and is intact; nothing reads the CSV back, "
             "so this costs no data");
    }
}

// ---- the GD STASH import file ---------------------------------------------------------------
// `uniq-export.gds`, beside the journal, same atomic writer, same lock, same mode semantics as
// the CSV. src\ut_gds.h carries the decompiled format and the field-by-field mapping; this half
// is only "which journal bytes go in which field".
//
// The mod NEVER reads a .gds back and GD Stash never writes into anything of ours: the file is
// a one-way hand-off, so a stale or missing one cannot cost the user an item.
bool g_gdsFieldOk = true;

// A u32 out of the entry's reconstructed replica. ZERO whenever it cannot be read honestly:
// the blob is too short (a `"len":0` reconcile row has none at all) or the offset falls inside a
// recorded std::string window, where the journal deliberately keeps no bytes. Every u32 this
// writer wants sits immediately AFTER a 0x20 window, so the second case is a guard against a
// hand-edited file rather than something the game can produce.
unsigned int u32At(const Entry& e, unsigned int off) {
    const unsigned int len = (unsigned int)e.replica.size();
    if (off > len || len - off < 4) return 0;
    if (offInAnySlot(e.slots, off)) return 0;
    unsigned int v = 0;
    memcpy(&v, &e.replica[off], 4);
    return v;
}

void gdsItemFrom(const Entry& e, UtGdsItem* it) {
    // The record is the journal's KEY, not slot +0x008: they are the same string on 357 of the
    // user's 358 real entries, and the key is the one field an entry can never be missing.
    it->itemID = e.record;
    it->prefixID = slotTextAt(e, 0x028);
    it->suffixID = slotTextAt(e, 0x048);
    it->modifierID = slotTextAt(e, 0x070);
    it->transmuteID = slotTextAt(e, 0x100);   // the journal's `illusion`
    it->seed = u32At(e, kSeedOffset);         // 0x068
    it->relicID = slotTextAt(e, 0x090);       // the journal's `component`
    it->relicBonusID = slotTextAt(e, 0x0B0);  // `completionBonus`
    it->relicSeed = u32At(e, 0x0D0);          // the u32 ut_rescue's format-3 block left unnamed
    it->enchantmentID = slotTextAt(e, 0x0D8); // `augment`
    it->enchantmentLevel = u32At(e, 0x0F8);
    it->enchantmentSeed = u32At(e, 0x0FC);
    it->ascendantID = slotTextAt(e, 0x120);
    it->ascendant2hID = slotTextAt(e, 0x140);
    it->var1 = u32At(e, 0x160);
    // The STACK comes from the entry, not from the replica mirror at +0x178: the entry's own
    // `stack` is what the deposit recorded and what every other export prints, and the two agree
    // wherever both exist. CLAMPED TO 1: a row that says 0 (a reconcile-synthesised entry, or a
    // hand-edited one) would otherwise import as an item with no copies at all, which is worse
    // than one copy of something the page really is holding. Nothing is ever multiplied here -
    // the clamp only ever raises 0 to 1.
    it->stackCount = e.stack ? e.stack : 1u;
    it->rerollsUsed = u32At(e, 0x180);
    it->affixRerollsUsed = u32At(e, 0x17C);
    // Not item properties - see ut_gds.h. `hardcore` is which STASH the item came out of, and
    // the collection HAS the answer - this export is the active mode's file and
    // every row in it came out of that mode's stash. There is still no character name to
    // attach, and this format carries no soulbound flag at all.
    it->hardcore = InterlockedCompareExchange(&g_mode, 0, 0) == 1;
    it->charname.clear();
}

void gdsRow(std::string* out, const Entry& e) {
    UtGdsItem it;
    try {
        gdsItemFrom(e, &it);
        if (!utGdsAppendItem(out, it)) g_gdsFieldOk = false;
    } catch (...) {
        g_gdsFieldOk = false;
    }
}

// Counts what mode `mode` will export, so the header's count int can be written before the
// records (the format has no way to patch it afterwards without seeking).
size_t gdsCount(int mode) {
    size_t n = 0;
    for (size_t i = 0; i < g_entries->size(); ++i) {
        if ((*g_entries)[i].stored == UT_STORED_NO) {
            if (mode >= 2) ++n;
            continue;
        }
        ++n;
    }
    return n;
}

// The whole file in memory. Caller holds g_cs. Mode 1 = the collection (stored plus every entry
// no reconciliation has looked at yet), 2 = every entry with the not-stored history LAST -
// exactly the CSV's two blocks, in exactly the same order, for exactly the same reasons.
void gdsBuild(std::string* out, int mode) {
    out->clear();
    g_gdsFieldOk = true;
    const size_t n = gdsCount(mode);
    utGdsAppendHeader(out, (unsigned int)n);
    for (size_t i = 0; i < g_entries->size(); ++i) {
        if ((*g_entries)[i].stored == UT_STORED_NO) continue;
        gdsRow(out, (*g_entries)[i]);
    }
    if (mode < 2) return;
    for (size_t i = 0; i < g_entries->size(); ++i) {
        if ((*g_entries)[i].stored != UT_STORED_NO) continue;
        gdsRow(out, (*g_entries)[i]);
    }
}

void gdsWrite() {
    // Read the mode ONCE, for the same reason csvWrite does: storeTick pushes it in from another
    // thread and a file built half at mode 2 and half at mode 1 would carry a count int that
    // disagrees with its own records - which is the ONE corruption this format cannot survive.
    const LONG mode = InterlockedCompareExchange(&g_gdsMode, 0, 0);
    if (mode <= 0) return;
    if (!g_gdsPath[0] || !g_entries) return;
    std::string bytes;
    try {
        gdsBuild(&bytes, (int)mode);
    } catch (...) {
        return;
    }
    if (!g_gdsFieldOk) {
        if (!InterlockedExchange(&g_gdsFailLogged, 1)) {
            logW("export: %s was ABANDONED - a field could not be encoded", g_gdsPath);
            logD("a record path of 256 bytes or more cannot be written in this format; the "
                 "collection file was written and is intact, and nothing reads the .gds back");
        }
        return;
    }
    if (writeWholeFileAtomic(g_gdsPath, bytes.c_str(), bytes.size())) {
        InterlockedIncrement(&g_gdsWrites);
        InterlockedExchange(&g_gdsFailLogged, 0);
        return;
    }
    if (!InterlockedExchange(&g_gdsFailLogged, 1)) {
        logW("export: %s could NOT be written", g_gdsPath);
        logD("the collection file itself was written and is intact; nothing reads the .gds back, "
             "so this costs no data");
    }
}

void journalService() {
    if (!g_entries || !g_csReady) return;
    if (!InterlockedCompareExchange(&g_dirty, 0, 0)) {
        // The CSV-ONLY pass. `export_csv` changed to a mode that
        // writes, so the export is stale - but nothing about the JOURNAL changed, and rewriting
        // the user's most important file to refresh a derived spreadsheet is not a trade this mod
        // makes. This branch never calls writeFile(): it takes the same lock and runs the same
        // csvWrite, so the CSV still cannot be built from a half-updated entry list.
        //
        // BOTH arms are consumed here, and each writes its own file - the two
        // exports are independent switches and either, both or neither may be armed. The arms
        // are exchanged to 0 BEFORE the read-only refusal below on purpose: a session that
        // cannot write must not accumulate an arm that fires the moment anything else does.
        const LONG csvArm = InterlockedExchange(&g_csvArm, 0);
        const LONG gdsArm = InterlockedExchange(&g_gdsArm, 0);
        if (!csvArm && !gdsArm) return;
        // The read-only session refuses the journal write, and it refuses this too: those entries
        // are whatever could be read out of a file this build does not fully understand, and
        // exporting that partial view under the name "your collection" would be a lie on disk.
        // Nothing is logged - the read-only refusal was already said once, at start-up.
        if (InterlockedCompareExchange(&g_readOnly, 0, 0)) return;
        // The `item` column is a catalogue lookup (journalItemName -> g_cat), and the catalogue is
        // loaded lazily by the FULL pass. Without this a mode flip before the session's first
        // journal write - which is exactly what start-up with export_csv=2 does - would publish a
        // CSV whose display-name column was blank on every row. Called with NO lock held, on the
        // worker, for the same reason the full pass does it there.
        catalogueEnsure();
        EnterCriticalSection(&g_cs);
        try {
            if (csvArm) csvWrite();
            if (gdsArm) gdsWrite();
        } catch (...) {
            // Same policy as every other export failure: reported inside csvWrite / gdsWrite,
            // then forgotten. Nothing reads either file back, so a stale export can never cost
            // the user an item.
        }
        LeaveCriticalSection(&g_cs);
        return;
    }
    // THE FULL PASS IS journalFlushNow(), and this is the only caller that is allowed to be
    // lazy about it: the read-only refusal (said once, g_dirty deliberately LEFT SET so the
    // entries stay pending rather than being forgotten), the arm discharge, catalogue.bin's
    // one-shot load with no lock held, the locked writeFile + csvWrite + gdsWrite, and the
    // re-arm of g_dirty on failure. The deposit path calls the same function synchronously, on
    // the game thread, because an in-memory row is not a recorded item.
    journalFlushNow();
}

bool journalUpsert(const UtReplicaCapture& cap) {
    if (!g_entries || !g_csReady || !cap.record[0]) return false;
    bool ok = false;
    EnterCriticalSection(&g_cs);
    try {
        Entry e;
        e.record.assign(cap.record);
        const unsigned int len = cap.replicaLen > sizeof(cap.replica)
                                     ? (unsigned int)sizeof(cap.replica)
                                     : cap.replicaLen;
        e.replica.assign(cap.replica, cap.replica + len);
        for (int i = 0; i < cap.slotCount && i < 24; ++i) {
            Slot s;
            s.off = cap.slotOff[i];
            s.text.assign(cap.slotText[i]);
            e.slots.push_back(s);
        }
        e.stack = cap.stack;
        e.flags = cap.flags;
        e.depositedAt = nowFileTime();
        // An upsert only ever happens because the engine ACCEPTED the deposit
        // (or because the reconcile found the record in the engine's own map and synthesised the
        // entry), so the item is in the collection at this instant - by construction, not by
        // inference. Without this the entry would be born UNKNOWN and stay so until the next
        // caravan open.
        e.stored = UT_STORED_YES;
        e.lastSeenAt = e.depositedAt;
        const int at = findEntry(cap.record);
        if (at >= 0) {
            // THE COUNT SURVIVES A REPLACE. An upsert replaces the whole entry,
            // and `count` is the one field that is not in the capture - it is the private
            // table's own row. Losing it here would silently empty the table on a re-deposit of
            // a record it already holds. journalUpsertCount sets the new value straight after;
            // the legacy (map-owned) deposit path leaves it exactly where it was, which for a
            // brand-new entry is 0.
            e.count = (*g_entries)[at].count;
            (*g_entries)[at] = e;
        } else {
            g_entries->push_back(e);
        }
        ok = true;
    } catch (...) {
        ok = false;
    }
    LeaveCriticalSection(&g_cs);
    if (ok) {
        InterlockedExchange(&g_dirty, 1);
        if (g_event) SetEvent(g_event);
    }
    return ok;
}

bool journalRemove(const char* record) {
    if (!g_entries || !g_csReady || !record) return false;
    bool removed = false;
    EnterCriticalSection(&g_cs);
    const int at = findEntry(record);
    if (at >= 0) {
        try {
            g_entries->erase(g_entries->begin() + at);
            removed = true;
        } catch (...) {
        }
    }
    LeaveCriticalSection(&g_cs);
    if (removed) {
        InterlockedExchange(&g_dirty, 1);
        if (g_event) SetEvent(g_event);
    }
    return removed;
}

bool journalHas(const char* record) {
    if (!g_entries || !g_csReady) return false;
    EnterCriticalSection(&g_cs);
    const bool r = findEntry(record) >= 0;
    LeaveCriticalSection(&g_cs);
    return r;
}

size_t journalCount() {
    if (!g_entries || !g_csReady) return 0;
    EnterCriticalSection(&g_cs);
    const size_t n = g_entries->size();
    LeaveCriticalSection(&g_cs);
    return n;
}

int journalReplicaLengthCensus(unsigned int size, unsigned int* otherLen) {
    if (otherLen) *otherLen = 0;
    if (!g_entries || !g_csReady) return 0;
    int n = 0;
    EnterCriticalSection(&g_cs);
    for (size_t k = 0; k < g_entries->size(); ++k) {
        const Entry& e = (*g_entries)[k];
        if (e.replica.empty() || e.replica.size() == size) continue;
        if (otherLen && !*otherLen) *otherLen = (unsigned int)e.replica.size();
        ++n;
    }
    LeaveCriticalSection(&g_cs);
    return n;
}

// One row of the private table. Read-only, no allocation, no engine call.
bool journalStoreRow(const char* record, int* stored, unsigned int* stack) {
    if (stored) *stored = UT_STORED_UNKNOWN;
    if (stack) *stack = 0;
    if (!g_entries || !g_csReady || !record) return false;
    bool got = false;
    EnterCriticalSection(&g_cs);
    const int at = findEntry(record);
    if (at >= 0) {
        if (stored) *stored = (*g_entries)[at].stored;
        if (stack) *stack = (*g_entries)[at].stack;
        got = true;
    }
    LeaveCriticalSection(&g_cs);
    return got;
}

// ---- the private table's accessors ----------------------------------------------------------
// The declarations are in ut_rescue.h. All of them follow
// journalStoreRow's shape: take g_cs, copy out plain data, release, allocate nothing while
// held - and none of them may EVER be called from hk_ItemLoad or any SEH frame that swallows
// (ut_rescue.cpp's own note at journalProbeRecord says why: a fault under the lock unwinds past
// the Leave and strands the journal for the session).
//
// THE ONE DELIBERATE EXCEPTION, named here so the rule stays readable instead of quietly eroding
// `journalDepositCommit` DOES allocate under g_cs - it copies a whole
// `Entry` (the record string, the replica vector and the slot strings) as its pre-image. The
// deposit contract demands an EXACT rollback of the previous row and only this file may copy an `Entry` at
// all, so the copy has to happen here and it has to happen under the lock that makes it a
// snapshot. It is contained: the copy sits inside a try/catch whose every path reaches the
// LeaveCriticalSection, so an allocation failure costs the deposit (which is then refused, the
// item untouched) and never the lock.
bool journalTableRow(const char* record, unsigned int* count, int* stored, unsigned int* stack) {
    if (count) *count = 0;
    if (stored) *stored = UT_STORED_UNKNOWN;
    if (stack) *stack = 0;
    if (!g_entries || !g_csReady || !record) return false;
    bool got = false;
    EnterCriticalSection(&g_cs);
    const int at = findEntry(record);
    if (at >= 0) {
        if (count) *count = (*g_entries)[at].count;
        if (stored) *stored = (*g_entries)[at].stored;
        if (stack) *stack = (*g_entries)[at].stack;
        got = true;
    }
    LeaveCriticalSection(&g_cs);
    return got;
}

bool journalSetCount(const char* record, unsigned int count) {
    if (!g_entries || !g_csReady || !record) return false;
    bool found = false, changed = false;
    EnterCriticalSection(&g_cs);
    const int at = findEntry(record);
    if (at >= 0) {
        found = true;
        Entry& e = (*g_entries)[at];
        if (e.count != count) changed = true;
        e.count = count;
        // The pair is written together, always. A row at 0 is HISTORY - the take emptied it -
        // and a row above 0 is inventory this file is the only copy of.
        const int want = count ? UT_STORED_YES : UT_STORED_NO;
        if (e.stored != want) {
            e.stored = want;
            changed = true;
        }
        if (count) e.lastSeenAt = nowFileTime();
    }
    LeaveCriticalSection(&g_cs);
    if (changed) {
        InterlockedExchange(&g_dirty, 1);
        if (g_event) SetEvent(g_event);
    }
    return found;
}

int journalCollectStored(char (*out)[256], unsigned int* counts, int cap) {
    if (!g_entries || !g_csReady) return 0;
    if (out && cap <= 0) return 0;
    int n = 0;
    EnterCriticalSection(&g_cs);
    for (size_t i = 0; i < g_entries->size(); ++i) {
        if (!(*g_entries)[i].count) continue;   // TABLE-OWNED rows ONLY - see ut_rescue.h
        if (out) {
            if (n >= cap) break;
            _snprintf_s(out[n], 256, _TRUNCATE, "%s", (*g_entries)[i].record.c_str());
            if (counts) counts[n] = (*g_entries)[i].count;
        }
        ++n;   // `out == nullptr` COUNTS the rows and copies nothing (storeStatus) - cap is
               // then ignored, because a truncated count would be a wrong number in the log.
    }
    LeaveCriticalSection(&g_cs);
    return n;
}

unsigned int journalCollectedTotal(void) {
    unsigned int total = 0;
    if (!g_entries || !g_csReady) return 0;
    EnterCriticalSection(&g_cs);
    for (size_t i = 0; i < g_entries->size(); ++i) total += (*g_entries)[i].count;
    LeaveCriticalSection(&g_cs);
    return total;
}

// THE FUNCTION THE WHOLE OF THE PRIVATE TABLE RESTS ON.
//
// Under (d) the deposit returns true WITHOUT calling the engine's original, and every caller's
// TRUE branch has already destroyed the source item (the drag's SendRemoveItemFromInventory at
// Game.dll 0x173995 and the cursor clear at 0x17399A; PlayerInventoryCtrl::RemoveItem(id,true)
// at exe 0x1EAB4E / 0x1EC66D). At that instant the ONLY copy of the item in the world is a row
// in g_entries. A crash, a taskkill or a power cut before the worker's next pass loses the ITEM,
// not merely its identity - there is no engine copy behind it and no refund branch to hand it
// back. So the deposit path writes the file itself, on its own thread, before it returns.
//
// It is journalService's FULL pass factored out, and journalService now calls it: the same
// read-only refusal, the same lock, the same writeFile() + csvWrite() + gdsWrite(), the same
// re-arm of g_dirty on failure. Everything it touches is thread-agnostic - writeWholeFileAtomic
// is .tmp + MoveFileEx(REPLACE_EXISTING | WRITE_THROUGH) and its own comment says why that is
// atomic wherever it runs.
//
// Returns false when the journal is READ-ONLY this session or the write failed. A false MUST
// refuse the deposit, and the caller must roll the in-memory row back - only the caller knows
// what was there before.
bool journalFlushNow(void) {
    if (!g_entries || !g_csReady) return false;
    // A full pass writes every file, so it discharges any pending export-only arm as well - and
    // it does so BEFORE the read-only refusal, exactly as the export-only branch does, so a
    // session that cannot write never accumulates an arm that fires the moment anything else does.
    InterlockedExchange(&g_csvArm, 0);
    InterlockedExchange(&g_gdsArm, 0);
    // The journal must not be written this session - the file
    // on disk carries a format newer than this build understands (writing it back would silently
    // drop every field this build never parsed), or it exists and could not be read, or a
    // migration failed its verify. g_dirty is deliberately LEFT SET so the entries stay pending
    // rather than being forgotten. The refusal is said ONCE and then silent - g_readOnlySaid is
    // never reset.
    if (InterlockedCompareExchange(&g_readOnly, 0, 0)) {
        if (InterlockedExchange(&g_readOnlySaid, 1) == 0) {
            logW("***** collection: %s is NOT written - READ-ONLY this session, so new deposits "
                 "will not survive a restart *****", g_path);
            logD("the reason was logged at start-up: a newer format, a file that could not be "
                 "read, or a migration that failed its verify");
        }
        return false;
    }
    InterlockedExchange(&g_dirty, 0);
    // catalogue.bin is loaded HERE and nowhere else - with no lock held, on the first write of
    // the session. NOT in journalInit: dllmain calls that under the loader lock, and reading a
    // 480 KB file there for a cosmetic display name is not a trade worth making. The latch is
    // interlocked because this pass is not the worker's alone - a deposit calls it on the game
    // thread.
    catalogueEnsure();
    bool ok = false;
    size_t n = 0;
    EnterCriticalSection(&g_cs);
    try {
        n = g_entries->size();
        ok = writeFile();
        if (ok) {
            csvWrite();
            gdsWrite();
        }
    } catch (...) {
        ok = false;
    }
    LeaveCriticalSection(&g_cs);
    if (!ok) {
        logE("rescue journal: WRITE FAILED (%s) - the backup is stale", g_path);
        InterlockedExchange(&g_dirty, 1);
        return false;
    }
    size_t stored = 0, notStored = 0, unknown = 0;
    journalCounts(nullptr, &stored, &notStored, &unknown);
    logD("rescue journal: wrote %zu entr%s to %s (%zu stored, %zu not stored, %zu not yet "
         "reconciled)",
         n, n == 1 ? "y" : "ies", g_path, stored, notStored, unknown);
    return true;
}

// The deposit commit, step by step. See ut_rescue.h for the contract; the whole of it is "TRUE means the
// item is on disk, FALSE means the file and the entry list are exactly as they were".
bool journalDepositCommit(const UtReplicaCapture& cap, unsigned int count, bool* rolledBack) {
    if (rolledBack) *rolledBack = false;
    if (!g_entries || !g_csReady || !cap.record[0]) return false;
    if (InterlockedCompareExchange(&g_readOnly, 0, 0)) {
        logW("deposit REFUSED: the collection file is READ-ONLY this session - the item was not "
             "touched");
        logD("the row could never have been written to disk, and an unwritten row is a lost "
             "item, not a lost identity");
        return false;
    }
    // Step 2: the pre-image. A whole Entry copy, taken under the lock, so the restore is exact -
    // the replica, the slots, the stored mark, the timestamps, the probe flag and the count.
    Entry prev;
    bool had = false;
    bool ok = false;
    EnterCriticalSection(&g_cs);
    try {
        const int at = findEntry(cap.record);
        if (at >= 0) {
            prev = (*g_entries)[at];
            had = true;
        }
        ok = true;
    } catch (...) {
        ok = false;
    }
    LeaveCriticalSection(&g_cs);
    if (!ok) return false;

    if (journalUpsertCount(cap, count) && journalFlushNow()) return true;

    // Step 5: PUT IT BACK. The upsert may have replaced or appended a row and the flush may have
    // failed after it; either way the caller is about to refuse, and a row left at count+1 would
    // be a phantom copy - it would paint a box for an item still in the player's bag and
    // max_per_record would refuse the real deposit next time.
    EnterCriticalSection(&g_cs);
    try {
        const int at = findEntry(cap.record);
        if (at >= 0) {
            if (had) {
                (*g_entries)[at] = prev;
            } else {
                g_entries->erase(g_entries->begin() + at);
            }
            if (rolledBack) *rolledBack = true;
        }
    } catch (...) {
    }
    LeaveCriticalSection(&g_cs);
    // The list is dirty either way: what is on disk is the PRE-image, and the entries in memory
    // are the pre-image too, so the worker's next pass writes what is already there. Harmless,
    // and it is what makes a transient failure (an antivirus holding the file for a moment)
    // heal itself.
    InterlockedExchange(&g_dirty, 1);
    if (g_event) SetEvent(g_event);
    return false;
}

// The TABLE's upsert: everything journalUpsert does, plus the count. This is the only function
// in the mod that can put a copy INTO the private table, and it is called on the deposit path
// only, immediately before journalFlushNow().
bool journalUpsertCount(const UtReplicaCapture& cap, unsigned int count) {
    if (!journalUpsert(cap)) return false;
    if (!count) return true;   // a 0 here is journalUpsert's own semantics, nothing to do
    // journalUpsert has already replaced the row and preserved the OLD count; set the new one.
    // journalSetCount marks it stored, stamps lastSeen and re-arms the dirty flag.
    return journalSetCount(cap.record, count);
}

int journalRecords(char (*out)[256], int cap) {
    if (!g_entries || !g_csReady || !out || cap <= 0) return 0;
    int n = 0;
    EnterCriticalSection(&g_cs);
    for (size_t i = 0; i < g_entries->size() && n < cap; ++i) {
        _snprintf_s(out[n], 256, _TRUNCATE, "%s", (*g_entries)[i].record.c_str());
        ++n;
    }
    LeaveCriticalSection(&g_cs);
    return n;
}

// ---- the reconciliation marks ---------------------------------------------------------------

bool journalMarkStored(const char* record, bool stored) {
    if (!g_entries || !g_csReady || !record) return false;
    bool changed = false, found = false;
    EnterCriticalSection(&g_cs);
    const int at = findEntry(record);
    if (at >= 0) {
        found = true;
        Entry& e = (*g_entries)[at];
        const int want = stored ? UT_STORED_YES : UT_STORED_NO;
        if (e.stored != want) changed = true;
        e.stored = want;
        if (stored) {
            const unsigned long long now = nowFileTime();
            // A day's granularity would be enough for a human, but the write is free and the
            // stamp is the only evidence of WHEN the mod last saw the item in the page.
            e.lastSeenAt = now;
            changed = true;
        }
    }
    LeaveCriticalSection(&g_cs);
    if (changed) {
        InterlockedExchange(&g_dirty, 1);
        if (g_event) SetEvent(g_event);
    }
    return found;
}

int journalSaveVariant() {
    return (int)InterlockedCompareExchange(&g_saveVariant, -1, -1);
}

// The header field keeps the name `saveVariant` so that every journal ever written still parses,
// but what it holds is the COLLECTION MODE - 0 softcore, 1 hardcore, as GameInfo::GetHardcore
// answered it - not the GameEngine+0x375BA byte the field was named after.
void journalSetSaveVariant(int variant) {
    if (variant < 0 || variant > 255) return;
    // First writer wins on purpose: adopting the CURRENT mode every time would make the header
    // follow whichever character was loaded last, and the mismatch refusal would never fire.
    InterlockedCompareExchange(&g_saveVariant, (LONG)variant, -1);
}

// ---- ONE COLLECTION PER MODE ----------------------------------------------------------------
int journalMode() {
    return (int)InterlockedCompareExchange(&g_mode, 0, 0);
}

bool journalModeKnown() {
    return InterlockedCompareExchange(&g_modeKnown, 0, 0) != 0;
}

// The world is gone, so what it told the mod about the collection mode goes with it. The mode
// is a property of the CHARACTER, not of the session - a stale start-up mode must never be
// painted onto a character that has not been asked about - and the next character must
// establish it for himself before one box of the collection is painted again. The mode itself
// (and so the open file) is left exactly where it is: nothing is flushed, moved or re-read here,
// and the very next world that agrees with it simply sets this flag again.
void journalForgetMode() {
    InterlockedExchange(&g_modeKnown, 0);
}

bool journalFollowMode(int variant) {
    // Anything but 0 or 1 is "the mode could not be read": stay on the file we are on and say
    // nothing. Guessing here would be the one mistake that writes a hardcore item into the
    // softcore collection.
    if (variant != 0 && variant != 1) return false;
    if (!g_entries || !g_csReady || !g_dir[0]) return false;
    const char* name = variant == 1 ? "hardcore" : "softcore";
    const LONG had = InterlockedExchange(&g_mode, (LONG)variant);
    const bool first = InterlockedExchange(&g_modeKnown, 1) == 0;
    if (had == (LONG)variant) {
        // Already on the right files. Pin the header's variant to the mode anyway: the files are
        // per mode now, so a header carried over from a build that kept one journal for both is
        // simply wrong about which stash THIS file belongs to.
        InterlockedExchange(&g_saveVariant, (LONG)variant);
        if (first) logI("collection: %s mode - using \"%s\"", name, g_path);
        return false;
    }
    // The mode CHANGED. Flush what the file we are leaving still owes BEFORE a single path moves
    // - after composePaths the old name is gone and that write would land in the wrong file.
    journalFlushNow();
    logI("collection: %s mode - leaving \"%s\" (%zu entries flushed)", name, g_path,
         g_entries->size());
    EnterCriticalSection(&g_cs);
    try {
        g_entries->clear();
    } catch (...) {
    }
    // Every session latch belongs to the file that is closing, not to the session: a read-only
    // mark earned by one mode's file must not silence the other mode's.
    InterlockedExchange(&g_dirty, 0);
    InterlockedExchange(&g_readOnly, 0);
    InterlockedExchange(&g_copyAside, 0);
    InterlockedExchange(&g_readOnlySaid, 0);
    InterlockedExchange(&g_saveVariant, (LONG)variant);
    composePaths(g_dir, variant);
    LeaveCriticalSection(&g_cs);
    // Re-open: journalInit's read, without journalInit's ladder. The old binary journal is a
    // softcore-era file with no mode at all and is migrated ONCE, at start-up, or never.
    bool had2 = readTextFile(g_path);
    if (!had2 && fileExists(g_path)) {
        InterlockedExchange(&g_readOnly, 1);
        logE("***** collection: %s EXISTS but could not be read - READ-ONLY this session; close "
             "whatever is holding the file and restart *****", g_path);
        had2 = true;
    }
    // After the read, not before: the file IS this mode's file, whatever its header said.
    InterlockedExchange(&g_saveVariant, (LONG)variant);
    // The exports are derived from the entry list that just changed under them, so arm the ones
    // that are on - the new mode's .gds and .csv are written on the worker's next pass instead
    // of staying the other mode's until the next deposit.
    if (InterlockedCompareExchange(&g_gdsMode, 0, 0) != 0) InterlockedExchange(&g_gdsArm, 1);
    if (InterlockedCompareExchange(&g_csvMode, 0, 0) != 0) InterlockedExchange(&g_csvArm, 1);
    if (g_event) SetEvent(g_event);
    logI("collection: %s mode - using \"%s\" (%zu entr%s)%s%s", name, g_path, g_entries->size(),
         g_entries->size() == 1 ? "y" : "ies", had2 ? "" : " (no file yet)",
         InterlockedCompareExchange(&g_readOnly, 0, 0) ? " [READ-ONLY this session]" : "");
    return true;
}

void journalCounts(size_t* entries, size_t* stored, size_t* notStored, size_t* unknown) {
    size_t n = 0, y = 0, no = 0, unk = 0;
    if (g_entries && g_csReady) {
        EnterCriticalSection(&g_cs);
        n = g_entries->size();
        for (size_t i = 0; i < n; ++i) {
            const int s = (*g_entries)[i].stored;
            if (s == UT_STORED_YES) ++y;
            else if (s == UT_STORED_NO) ++no;
            else ++unk;
        }
        LeaveCriticalSection(&g_cs);
    }
    if (entries) *entries = n;
    if (stored) *stored = y;
    if (notStored) *notStored = no;
    if (unknown) *unknown = unk;
}

int journalPruneNotStored(char (*out)[256], int cap, char* why, size_t whyCap) {
    if (why && whyCap) why[0] = 0;
    if (!g_entries || !g_csReady || !g_path[0]) {
        if (why) _snprintf_s(why, whyCap, _TRUNCATE, "the journal is not initialised");
        return -1;
    }
    if (InterlockedCompareExchange(&g_readOnly, 0, 0)) {
        if (why)
            _snprintf_s(why, whyCap, _TRUNCATE,
                        "the journal is READ-ONLY for this session - nothing may be dropped");
        return -1;
    }
    // THE COPY COMES FIRST AND ITS FAILURE IS FATAL TO THE CALL. This is the only place in the
    // mod that deliberately removes journal entries, so the pre-image has to exist on disk before
    // a single one goes.
    char stamp[32], aside[MAX_PATH];
    stampNow(stamp, sizeof(stamp));
    _snprintf_s(aside, sizeof(aside), _TRUNCATE, "%s.pruned-%s", g_path, stamp);
    if (!CopyFileA(g_path, aside, TRUE)) {
        if (why)
            _snprintf_s(why, whyCap, _TRUNCATE,
                        "the journal could not be copied to %s (error %lu) - nothing was dropped",
                        aside, GetLastError());
        return -1;
    }
    int dropped = 0;
    EnterCriticalSection(&g_cs);
    try {
        std::vector<Entry> keep;
        keep.reserve(g_entries->size());
        for (size_t i = 0; i < g_entries->size(); ++i) {
            // `count == 0` TOO. This is the only
            // place in the mod that deliberately removes journal entries, and under the private
            // table a row at count >= 1 is THE ITEM ITSELF - dropping it would destroy the
            // player's property, not a stale identity. `journalSetCount` keeps the pair together
            // (count 0 <-> "stored":false) and the reader repairs any file that disagrees, so
            // this conjunct should never be the deciding one; it is here so that a future bug in
            // either of those two cannot turn a prune into a loss.
            if ((*g_entries)[i].stored == UT_STORED_NO && !(*g_entries)[i].count) {
                if (out && dropped < cap) {
                    _snprintf_s(out[dropped], 256, _TRUNCATE, "%s",
                                (*g_entries)[i].record.c_str());
                }
                ++dropped;
                continue;
            }
            keep.push_back((*g_entries)[i]);
        }
        g_entries->swap(keep);
    } catch (...) {
        dropped = -1;
        if (why) _snprintf_s(why, whyCap, _TRUNCATE, "allocation failure - nothing was dropped");
    }
    LeaveCriticalSection(&g_cs);
    if (dropped > 0) {
        InterlockedExchange(&g_dirty, 1);
        if (g_event) SetEvent(g_event);
    }
    if (dropped >= 0 && why) {
        _snprintf_s(why, whyCap, _TRUNCATE, "the file before the prune was kept as %s", aside);
    }
    return dropped;
}

// Called from the WORKER thread (every journal write) and from the GAME
// thread as well (tier C's name half, `searchSweepTick`). Read the volatile pointer ONCE into a
// local: a null just means the catalogue has not been loaded yet and costs the caller a retry,
// never a wrong answer.
bool journalItemName(const char* record, char* out, size_t cap) {
    if (out && cap) out[0] = 0;
    gdut::Catalogue* cat = g_cat;
    if (!cat || !record || !record[0] || !out || !cap) return false;
    try {
        const int idx = cat->indexOfRecord(std::string_view(record));
        if (idx < 0) return false;
        const std::string_view name = cat->item((size_t)idx).name;
        if (name.empty()) return false;
        size_t n = name.size();
        if (n >= cap) n = cap - 1;
        memcpy(out, name.data(), n);
        out[n] = 0;
        return true;
    } catch (...) {
        return false;
    }
}

const char* journalPath() {
    return g_path;
}

long journalWrites() {
    return InterlockedCompareExchange(&g_writes, 0, 0);
}

bool journalReadOnly() {
    return InterlockedCompareExchange(&g_readOnly, 0, 0) != 0;
}

bool rescueReportWrite(const char* text, size_t len) {
    if (!g_reportPath[0] || !text) return false;
    char tmp[MAX_PATH];
    _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "%s.tmp", g_reportPath);
    HANDLE h = CreateFileA(tmp, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const BOOL ok = WriteFile(h, text, (DWORD)len, &wrote, nullptr);
    FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || wrote != (DWORD)len) {
        DeleteFileA(tmp);
        return false;
    }
    if (!MoveFileExA(tmp, g_reportPath, MOVEFILE_REPLACE_EXISTING)) {
        Sleep(30);
        if (!MoveFileExA(tmp, g_reportPath, MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileA(tmp);
            return false;
        }
    }
    return true;
}

const char* rescueReportPath() {
    return g_reportPath;
}

// ---- identity -------------------------------------------------------------------------------
// Pure data. No engine call, no Win32, no allocation on the hot path: identityBuild() runs inside
// the engine's own Item::CreateItem frame, and tools/test_journal.cpp runs the very same code
// with no game at all.

bool journalGet(const char* record, UtReplicaCapture* out) {
    if (!g_entries || !g_csReady || !record || !out) return false;
    bool got = false;
    EnterCriticalSection(&g_cs);
    const int at = findEntry(record);
    if (at >= 0) {
        try {
            const Entry& e = (*g_entries)[at];
            memset(out, 0, sizeof(*out));
            _snprintf_s(out->record, sizeof(out->record), _TRUNCATE, "%s", e.record.c_str());
            unsigned int n = (unsigned int)e.replica.size();
            if (n > sizeof(out->replica)) n = (unsigned int)sizeof(out->replica);
            if (n) memcpy(out->replica, &e.replica[0], n);
            out->replicaLen = n;
            out->stack = e.stack;
            out->flags = e.flags;
            for (size_t i = 0; i < e.slots.size() && out->slotCount < 24; ++i) {
                out->slotOff[out->slotCount] = e.slots[i].off;
                _snprintf_s(out->slotText[out->slotCount], 160, _TRUNCATE, "%s",
                            e.slots[i].text.c_str());
                ++out->slotCount;
            }
            got = true;
        } catch (...) {
            got = false;
        }
    }
    LeaveCriticalSection(&g_cs);
    return got;
}

unsigned int journalEntryFlagged(const char* record) {
    if (!g_entries || !g_csReady || !record) return 0;
    unsigned int flagged = 0;
    EnterCriticalSection(&g_cs);
    const int at = findEntry(record);
    if (at >= 0) {
        try {
            flagged = (*g_entries)[at].nullShape;
        } catch (...) {
            flagged = 0;
        }
    }
    LeaveCriticalSection(&g_cs);
    return flagged;
}

// The capture's slot scan. See the contract in ut_rescue.h. Pure: the only thing
// that touches memory it does not own is `probe`, which the caller guards.
int utScanReplicaSlots(void* ctx, unsigned int len, UtSlotProbeFn probe, unsigned int* slotOff,
                       char (*slotText)[160], UtSlotShape* slotShape, int maxSlots, int* faults) {
    if (faults) *faults = 0;
    if (!probe || !slotOff || !slotText || maxSlots <= 0) return 0;
    int n = 0;
    for (unsigned int off = 0; off + 0x20 <= len && n < maxSlots; off += 8) {
        // The empty-heap relaxation is offered only where the NEXT member of a consecutive
        // std::string run lives - exactly 0x20 past the slot this scan last confirmed.
        const bool allowEmpty = n > 0 && off == slotOff[n - 1] + 0x20;
        UtSlotProbeResult r;
        memset(&r, 0, sizeof(r));
        probe(ctx, off, allowEmpty, &r);
        if (!r.ok) {
            if (r.faulted && faults) ++*faults;
            continue;
        }
        unsigned int at = off;
        // THE EMPTY-WINDOW RULE. A window that reads back EMPTY is provisional: it
        // is what a `u32 then std::string` pair looks like from 8 bytes too early (the relic's
        // +0x68 seed in front of the +0x70 completion-bonus record; ItemReplicaInfo::operator=
        // has the same shape at +0xD0/+0xD8 and +0xF8/+0xFC/+0x100). Probe `off + 8` STRICTLY -
        // no relaxation, so its pointer must dereference to a NUL-terminated string of exactly
        // _Mysize bytes - and prefer it when it answers. A genuinely empty SSO string cannot
        // lose here: its own buffer is zeroed, so the window at off+8 reads _Ptr == 0 and the
        // strict probe refuses it.
        // The discriminator is `emptyHeap` - "accepted ONLY because of the empty-heap
        // relaxation, pointer never dereferenced" - and not "the text read back empty". An
        // empty SSO string is FULLY VERIFIED (its own 16 bytes were read), so it must never be
        // displaced by an unverified window 8 bytes on.
        if (r.emptyHeap && off + 8 + 0x20 <= len) {
            UtSlotProbeResult r2;
            memset(&r2, 0, sizeof(r2));
            probe(ctx, off + 8, false, &r2);
            if (r2.faulted && faults) ++*faults;
            if (r2.ok) {
                at = off + 8;
                memcpy(&r, &r2, sizeof(r));
            }
        }
        slotOff[n] = at;
        memcpy(slotText[n], r.text, sizeof(r.text));
        slotText[n][sizeof(r.text) - 1] = 0;
        if (slotShape) {
            slotShape[n].ptr = r.ptr;
            slotShape[n].cap = r.cap;
            slotShape[n].emptyHeap = r.emptyHeap ? 1u : 0u;
        }
        ++n;
        off = at + 0x18;  // a confirmed string object is 0x20 bytes; the loop adds the last 8
    }
    return n;
}

bool utReadMsvcString(const unsigned char* blob, unsigned int off, unsigned int blobLen,
                      char* out, size_t cap) {
    if (!blob || !out || !cap) return false;
    if (off + 0x20 > blobLen) return false;
    const unsigned char* p = blob + off;
    size_t size = 0, capacity = 0;
    memcpy(&size, p + 0x10, sizeof(size));
    memcpy(&capacity, p + 0x18, sizeof(capacity));
    if (capacity < 15 || capacity > 0x4000 || size > capacity) return false;
    const char* text = nullptr;
    if (capacity == 15) {
        text = (const char*)p;
    } else {
        const char* ptr = nullptr;
        memcpy(&ptr, p, sizeof(ptr));
        if (!ptr) return false;
        text = ptr;
    }
    const size_t room = cap - 1;
    const size_t n = size > room ? room : size;
    memcpy(out, text, n);
    out[n] = 0;
    return true;
}

namespace {

// Writes one MSVC std::string (16-byte union, size_t size, size_t capacity) at blob+off. Short
// strings go in the SSO buffer exactly as the engine would store them; a long one points at
// `storage`, which lives inside the overlay and therefore outlives the Item::CreateItem call.
void writeMsvcString(unsigned char* blob, unsigned int off, char* storage) {
    unsigned char* p = blob + off;
    memset(p, 0, 0x20);
    const size_t n = strlen(storage);
    memcpy(p + 0x10, &n, sizeof(n));
    if (n < 16) {
        memcpy(p, storage, n + 1);
        const size_t cap = 15;
        memcpy(p + 0x18, &cap, sizeof(cap));
    } else {
        char* ptr = storage;
        memcpy(p, &ptr, sizeof(ptr));
        memcpy(p + 0x18, &n, sizeof(n));
    }
}

}  // namespace

bool identityBuild(const UtReplicaCapture& entry, const unsigned char* incoming,
                   unsigned int incomingLen, unsigned int stackOff, UtIdentityOverlay* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    _snprintf_s(out->why, sizeof(out->why), _TRUNCATE, "not built");
    if (!incoming) {
        _snprintf_s(out->why, sizeof(out->why), _TRUNCATE, "no incoming replica");
        return false;
    }
    if (!entry.replicaLen || entry.replicaLen > sizeof(out->replica)) {
        _snprintf_s(out->why, sizeof(out->why), _TRUNCATE,
                    "the journal entry carries a %u-byte replica", entry.replicaLen);
        return false;
    }
    if (incomingLen != entry.replicaLen) {
        // A size change means ItemReplicaInfo moved between the deposit and now - a patch, or a
        // journal from another build. Refusing is the only safe answer; the engine's own replica
        // is used unchanged and the entry stays in the file.
        _snprintf_s(out->why, sizeof(out->why), _TRUNCATE,
                    "replica size mismatch: the journal has %u bytes, the live one is %u",
                    entry.replicaLen, incomingLen);
        return false;
    }
    const unsigned int len = entry.replicaLen;
    memcpy(out->replica, entry.replica, len);
    out->replicaLen = len;

    // The two fields that belong to THIS creation, not to the stored identity.
    if (len >= 4) {
        memcpy(out->replica, incoming, 4);  // ItemReplicaInfo + 0x00 = the object id
        out->keptObjectId = true;
    }
    if (stackOff && stackOff + 4 <= len) {
        memcpy(out->replica + stackOff, incoming + stackOff, 4);
        out->keptStack = true;
    }

    // Re-point every std::string the capture found. Without this the blob's heap pointers are
    // the deposited item's, freed long ago - and Item::CreateItem reads them.
    for (int i = 0; i < entry.slotCount && i < 24; ++i) {
        const unsigned int off = entry.slotOff[i];
        if (off + 0x20 > len) {
            ++out->slotsSkipped;
            continue;
        }
        _snprintf_s(out->strings[out->slotCount], 160, _TRUNCATE, "%s", entry.slotText[i]);
        writeMsvcString(out->replica, off, out->strings[out->slotCount]);
        out->slotOff[out->slotCount] = off;
        ++out->slotCount;
    }

    // Diagnostic only: how much of the identity the journal actually carried that the engine's
    // own replica did not. Slot bytes are excluded - they are pointers on both sides.
    for (unsigned int j = 0; j < len; ++j) {
        bool inSlot = false;
        for (int i = 0; i < out->slotCount; ++i) {
            if (j >= out->slotOff[i] && j < out->slotOff[i] + 0x20) {
                inSlot = true;
                break;
            }
        }
        if (inSlot) continue;
        ++out->bytesComparable;
        if (out->replica[j] != incoming[j]) ++out->bytesFromJournal;
    }

    // ---- the dangling-pointer check, and why it is a REFUSAL and not a warning ---------------
    // The blob's std::string members are re-pointed above only for the slots the capture found.
    // Any slot it MISSED (its probe faulted, or the pointer no longer read back as a string)
    // still holds the deposited item's heap pointer, freed long ago - and Item::CreateItem reads
    // those members. So: no 32-byte window outside the rebuilt slots may still look like a
    // HEAP-shaped std::string with a plausible pointer. Only the header is examined; the pointer
    // is never dereferenced. A hit means the entry cannot be restored safely, so the engine's own
    // replica is used unchanged and the entry stays in the file.
    bool sawBaseRecord = false;
    for (int i = 0; i < out->slotCount; ++i) {
        if (out->slotOff[i] == 0x08) sawBaseRecord = true;
    }
    if (!sawBaseRecord) {
        _snprintf_s(out->why, sizeof(out->why), _TRUNCATE,
                    "the journal entry has no base-record slot at +0x08 - its string pointers "
                    "cannot be re-pointed safely");
        return false;
    }
    // The NULL-source shape FIRST, at every 8-aligned offset that is not itself a rebuilt
    // slot's OWN offset - deliberately not the overlap rule below, because this shape is born
    // INSIDE a rebuilt window (writeMsvcString memsets 0x20 bytes, so the window 8 bytes on
    // reads _Ptr = 0 and _Mysize = that slot's _Myres). It is the one pointer value guaranteed
    // to fault. See nullStringWindowAt for the calibration against a real 28-entry journal.
    {
        unsigned int nsSize = 0, nsCap = 0;
        const unsigned int hit =
            nullStringWindowAt(out->replica, len, out->slotOff, out->slotCount, &nsSize, &nsCap);
        if (hit) {
            _snprintf_s(out->why, sizeof(out->why), _TRUNCATE,
                        "NULL string pointer with size %u at +0x%X (capacity %u) - the capture "
                        "recorded the window 8 bytes before the real std::string, and the engine "
                        "would memcpy %u bytes from address 0",
                        nsSize, hit - 1, nsCap, nsSize);
            return false;
        }
    }
    //
    // The skip test must not compare the window's START only (`off >= slotOff[i] && off <
    // slotOff[i] + 0x20`): a window that begins in the gap BEFORE a rebuilt slot and runs into
    // it would survive the skip and then read its "size" and "capacity" out of that slot's own
    // bytes (a false positive seen on a real item). With slots at +0xB0 and
    // +0xD8 the single 8-aligned window at +0xD0 read size from +0xE0 (the zeroed half of the
    // rebuilt SSO union) and capacity from +0xE8 (the LENGTH field of the rebuilt string) - so
    // "capacity 39" was the length of a 39-character component/relic path and the entry was
    // refused precisely BECAUSE the item carried an identity. The test is now an OVERLAP test.
    // It cannot make anything less safe: bytes inside a slot the mod itself rewrote are the
    // mod's own (writeMsvcString memsets the whole 0x20), never a stale heap pointer.
    for (unsigned int off = 0; off + 0x20 <= len; off += 8) {
        bool rebuilt = false;
        for (int i = 0; i < out->slotCount; ++i) {
            if (off + 0x20 > out->slotOff[i] && off < out->slotOff[i] + 0x20) rebuilt = true;
        }
        if (rebuilt) continue;
        size_t size = 0, capacity = 0;
        memcpy(&size, out->replica + off + 0x10, sizeof(size));
        memcpy(&capacity, out->replica + off + 0x18, sizeof(capacity));
        if (capacity <= 15 || capacity > 0x4000 || size > capacity) continue;
        unsigned long long ptr = 0;
        memcpy(&ptr, out->replica + off, sizeof(ptr));
        if (ptr < 0x10000ull || ptr > 0x7FFFFFFFFFFFull) continue;
        // Name the offending window AND the nearest rebuilt slot, so the next session's log says
        // which slot a straddle came from instead of leaving it to be re-derived by hand.
        unsigned int nearest = 0;
        unsigned int bestDist = 0xFFFFFFFFu;
        for (int i = 0; i < out->slotCount; ++i) {
            const unsigned int so = out->slotOff[i];
            const unsigned int d = so > off ? so - off : off - so;
            if (d < bestDist) {
                bestDist = d;
                nearest = so;
            }
        }
        _snprintf_s(out->why, sizeof(out->why), _TRUNCATE,
                    "replica+0x%X still looks like a heap std::string the capture never copied "
                    "(size %u, capacity %u) - its pointer is dangling; nearest rebuilt slot "
                    "+0x%X (%u bytes away)",
                    off, (unsigned int)size, (unsigned int)capacity, nearest,
                    bestDist == 0xFFFFFFFFu ? 0u : bestDist);
        return false;
    }
    _snprintf_s(out->why, sizeof(out->why), _TRUNCATE, "ok");
    return true;
}

// ---- the FRESH-PROTOTYPE swap overlay -------------------------------------------------------
// See the header for why the in-place refresh is gone. This is identityBuild plus the two fields
// that belong to a BIRTH rather than to an assignment, and nothing else - so every refusal
// identityBuild can make (no +0x08 slot, the NULL-shape, a dangling window, a size change) still
// refuses the swap, which is the whole point of routing the swap through it.
bool utBuildSwapOverlay(const UtReplicaCapture& entry, const unsigned char* incoming,
                        unsigned int incomingLen, unsigned int stackOff, UtIdentityOverlay* out) {
    if (!identityBuild(entry, incoming, incomingLen, stackOff, out)) return false;
    const unsigned int len = out->replicaLen;
    // `stackOff + 4 > len` alone WRAPS for an absurd stackOff. Subtracting instead is
    // overflow-free and makes the one test carry both guarantees: the
    // memset of the first 4 bytes and the 4-byte write at +stackOff are both in range.
    if (!stackOff || stackOff > len || len - stackOff < 4) {
        _snprintf_s(out->why, sizeof(out->why), _TRUNCATE,
                    "the stack mirror offset (+0x%X) does not fit a %u-byte replica - a prototype "
                    "born with stack 0 paints an invisible box",
                    stackOff, len);
        return false;
    }
    // The object id: the engine zeroes replica+0x00 itself at 0x2CECBD before its own
    // Item::CreateItem, and Item::SetItemReplicaInfo re-stamps it from the new object afterwards
    // Zero is what the engine hands its factory, so it is what the mod hands it too.
    memset(out->replica, 0, 4);
    out->keptObjectId = false;
    // The stack mirror. ItemEquipment::InitializeItem+0x45 copies replica+0x6B0 into the live
    // stack +0x88C, and an ordinary equipment item's replica carries 0 there.
    const unsigned int one = 1u;
    memcpy(out->replica + stackOff, &one, sizeof(one));
    out->keptStack = false;
    _snprintf_s(out->why, sizeof(out->why), _TRUNCATE, "ok");
    return true;
}

}  // namespace ut
