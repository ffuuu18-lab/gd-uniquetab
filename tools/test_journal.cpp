// test_journal.cpp - a unit-style check of the deposit journal that needs NO GAME.
//
// It links the real ut_rescue.cpp + ut_log.cpp, fills three entries with a synthetic
// ItemReplicaInfo blob (one heap-shaped string slot, one SSO-shaped one), writes the file
// through the same journalService() the worker thread calls, then re-reads it through
// journalInit() in a second process and compares. It also proves the take-side removal path
// (journalRemove) and the "one entry per record" replace.
//
// The take-side proof. A synthetic ItemReplicaInfo with EVERY
// identity field set - object id, the stack mirror, seed, relicSeed, enchantmentSeed,
// materiaCombines, seedRerolls, affixRerolls, and six std::string slots (base record, prefix,
// suffix, materia/component, enchantment/augment, ascendant), one of them deliberately longer
// than the 15-byte SSO buffer so it has to be re-pointed - is journalled, written, and then in a
// SECOND PROCESS read back off disk and overlaid onto a hostile "incoming" replica through the
// very same identityBuild() the Item::CreateItem detour calls. That is the whole round trip,
// with no game anywhere near it.
//
// Build + run:  tools\build_test_journal.bat      (writes and then deletes out\uniq-items.bin)
//   pass 1: test_journal.exe             writes the journal, checks the overlay in memory
//   pass 2: test_journal.exe --verify    re-reads the FILE and checks the overlay again
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include <string>
#include <vector>

#include "../src/ut_rescue.h"

#include "../src/ut_log.h"  // the flush-rule case drives the real logger

namespace ut {
void utModDirA(HMODULE self, char* out, size_t cap);
}  // namespace ut

static int g_fail = 0;

// ---- journal-file helpers -----------------------------------------------------------------------

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

static int countRecordLines(const std::string& text) {
    int n = 0;
    size_t at = 0;
    while (at < text.size()) {
        size_t nl = text.find('\n', at);
        if (nl == std::string::npos) nl = text.size();
        if (text.compare(at, 10, "{\"record\":") == 0) ++n;
        at = nl + 1;
    }
    return n;
}

// A one-line snapshot of everything about an entry that can reach the engine: the fields the
// journal carries AND identityBuild's whole output, which is the only thing the engine ever
// sees. Two dumps that compare equal ARE the field-for-field proof.
static void dumpEntry(std::string* out, const char* record) {
    ut::UtReplicaCapture e;
    char line[1024];
    if (!ut::journalGet(record, &e)) {
        _snprintf_s(line, sizeof(line), _TRUNCATE, "%s MISSING\n", record);
        out->append(line);
        return;
    }
    _snprintf_s(line, sizeof(line), _TRUNCATE, "%s len=%u stack=%u flags=%u slots=%d flagged=%u\n",
                e.record, e.replicaLen, e.stack, e.flags, e.slotCount,
                ut::journalEntryFlagged(record));
    out->append(line);
    for (int i = 0; i < e.slotCount; ++i) {
        _snprintf_s(line, sizeof(line), _TRUNCATE, "   s%03X=%s\n", e.slotOff[i], e.slotText[i]);
        out->append(line);
    }
    ut::UtIdentityOverlay ov;
    const bool ok = ut::identityBuild(e, e.replica, e.replicaLen, 0x178, &ov);
    // A heap-shaped rebuilt slot carries a live pointer into `ov.strings`, which differs between
    // processes; zero those 8 bytes so the dump means the same thing in both passes. Everything
    // else - the SSO slots' own bytes, every size and capacity, every non-slot byte - is compared.
    for (int i = 0; i < ov.slotCount; ++i) {
        const unsigned int off = ov.slotOff[i];
        if (off + 0x20 > ov.replicaLen) continue;
        size_t capacity = 0;
        memcpy(&capacity, ov.replica + off + 0x18, sizeof(capacity));
        if (capacity != 15) memset(ov.replica + off, 0, 8);
    }
    _snprintf_s(line, sizeof(line), _TRUNCATE, "   build=%d why=%s len=%u slots=%d\n", ok ? 1 : 0,
                ov.why, ov.replicaLen, ov.slotCount);
    out->append(line);
    out->append("   blob=");
    for (unsigned int i = 0; i < ov.replicaLen; ++i) {
        char hx[4];
        _snprintf_s(hx, sizeof(hx), _TRUNCATE, "%02X", ov.replica[i]);
        out->append(hx);
    }
    out->append("\n");
}

static void dumpAll(std::string* out) {
    static char recs[4096][256];
    const int n = ut::journalRecords(recs, 4096);
    char head[64];
    _snprintf_s(head, sizeof(head), _TRUNCATE, "entries=%d\n", n);
    out->append(head);
    for (int i = 0; i < n; ++i) dumpEntry(out, recs[i]);
}


// ---- a .gds reader, written here on purpose --------------------------------------------------
// The same rule as the CSV reader below: the mod never reads uniq-export.gds back, so the only
// way to prove the writer is right is to decode it with a reader that knows nothing about it.
// This one is a transcription of org.gdstash.db.DBStashItem.readGDS (GDStash.jar 1.90b) and of
// tools\gds_read.py, and like both of them it insists on consuming the WHOLE file - a writer
// that gets one field wrong ends with "trailing bytes", not with a plausible-looking dump.
//
//   s[0] itemID  s[1] prefix  s[2] suffix  s[3] modifier  s[4] transmute  s[5] relic
//   s[6] relicBonus  s[7] enchantment  s[8] ascendant  s[9] ascendant2h  s[10] charname
//   i[0] seed  i[1] relicSeed  i[2] enchantmentLevel  i[3] enchantmentSeed  i[4] var1
//   i[5] stackCount  i[6] rerollsUsed  i[7] affixRerollsUsed
struct GdsItem {
    std::string s[11];
    int i[8];
    bool hardcore;
    GdsItem() : hardcore(false) {
        for (int k = 0; k < 8; ++k) i[k] = 0;
    }
};

static bool gdsParse(const std::string& d, std::vector<GdsItem>* items, std::string* why) {
    items->clear();
    why->clear();
    size_t p = 0;
    char msg[192];
    // Little-endian int32, exactly GDReader.readInt.
    struct L {
        static bool i32(const std::string& d, size_t* p, int* out) {
            if (*p + 4 > d.size()) return false;
            unsigned int v = (unsigned char)d[*p] | ((unsigned int)(unsigned char)d[*p + 1] << 8) |
                             ((unsigned int)(unsigned char)d[*p + 2] << 16) |
                             ((unsigned int)(unsigned char)d[*p + 3] << 24);
            *p += 4;
            *out = (int)v;
            return true;
        }
        // One unsigned length byte then that many bytes; 0 is the null string (== "" here).
        static bool str(const std::string& d, size_t* p, std::string* out) {
            if (*p + 1 > d.size()) return false;
            const size_t n = (unsigned char)d[*p];
            ++*p;
            if (*p + n > d.size()) return false;
            out->assign(d, *p, n);
            *p += n;
            return true;
        }
    };
    int version = 0, count = 0;
    if (!L::i32(d, &p, &version) || !L::i32(d, &p, &count)) {
        *why = "the 8-byte header (version int + count int) is not there";
        return false;
    }
    if (version != 3) {
        _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                    "version %d - this build writes 3, what GD Stash 1.90b writes itself", version);
        *why = msg;
        return false;
    }
    if (count < 0) {
        *why = "the item count is negative";
        return false;
    }
    for (int k = 0; k < count; ++k) {
        GdsItem it;
        const bool ok =
            L::str(d, &p, &it.s[0]) && L::str(d, &p, &it.s[1]) && L::str(d, &p, &it.s[2]) &&
            L::str(d, &p, &it.s[3]) && L::str(d, &p, &it.s[4]) && L::i32(d, &p, &it.i[0]) &&
            L::str(d, &p, &it.s[5]) && L::str(d, &p, &it.s[6]) && L::i32(d, &p, &it.i[1]) &&
            L::str(d, &p, &it.s[7]) && L::i32(d, &p, &it.i[2]) && L::i32(d, &p, &it.i[3]) &&
            L::str(d, &p, &it.s[8]) && L::str(d, &p, &it.s[9]) && L::i32(d, &p, &it.i[4]) &&
            L::i32(d, &p, &it.i[5]) && L::i32(d, &p, &it.i[6]) && L::i32(d, &p, &it.i[7]);
        if (!ok || p >= d.size()) {
            _snprintf_s(msg, sizeof(msg), _TRUNCATE, "item %d ran off the end of the file", k + 1);
            *why = msg;
            return false;
        }
        it.hardcore = d[p] != 0;
        ++p;
        if (!L::str(d, &p, &it.s[10])) {
            _snprintf_s(msg, sizeof(msg), _TRUNCATE, "item %d's charname ran off the end", k + 1);
            *why = msg;
            return false;
        }
        items->push_back(it);
    }
    if (p != d.size()) {
        _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                    "trailing bytes: %d item(s) consumed %zu of %zu", count, p, d.size());
        *why = msg;
        return false;
    }
    return true;
}

static void gdsPrint(const GdsItem& it, size_t idx) {
    static const char* const kStrNames[11] = {
        "itemID",     "prefixID",     "suffixID",  "modifierID",   "transmuteID", "relicID",
        "relicBonusID", "enchantmentID", "ascendantID", "ascendant2hID", "charname"};
    static const char* const kIntNames[8] = {"seed",  "relicSeed",  "enchantmentLevel",
                                             "enchantmentSeed", "var1", "stackCount",
                                             "rerollsUsed", "affixRerollsUsed"};
    printf("    [%zu] %s\n", idx + 1, it.s[0].c_str());
    for (int k = 1; k < 11; ++k) {
        if (!it.s[k].empty()) printf("         %-17s %s\n", kStrNames[k], it.s[k].c_str());
    }
    for (int k = 0; k < 8; ++k) {
        if (it.i[k]) printf("         %-17s %u\n", kIntNames[k], (unsigned int)it.i[k]);
    }
    if (it.hardcore) printf("         %-17s yes\n", "hardcore");
}

// ---- a MINIMAL RFC-4180 reader, written here on purpose --------------------------------------
// The mod never reads uniq-export.csv back, so the only way to prove the writer's quoting is
// right is to decode it with a reader that knows nothing about the writer. This is that reader:
// records end at a CRLF outside quotes, fields at a comma outside quotes, and "" inside a quoted
// field is one literal quote. It is 30 lines and it is the whole of RFC 4180 section 2.
static bool csvParse(const std::string& text, std::vector<std::vector<std::string> >* rows) {
    rows->clear();
    std::vector<std::string> row;
    std::string field;
    bool inQuotes = false, sawAny = false;
    size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        sawAny = true;
        if (inQuotes) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') {
                    field.push_back('"');
                    i += 2;
                    continue;
                }
                inQuotes = false;
                ++i;
                continue;
            }
            field.push_back(c);
            ++i;
            continue;
        }
        if (c == '"' && field.empty()) {
            inQuotes = true;
            ++i;
            continue;
        }
        if (c == ',') {
            row.push_back(field);
            field.clear();
            ++i;
            continue;
        }
        if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
            row.push_back(field);
            field.clear();
            rows->push_back(row);
            row.clear();
            i += 2;
            continue;
        }
        if (c == '\n' || c == '\r') return false;  // a bare LF or CR is not RFC-4180
        field.push_back(c);
        ++i;
    }
    if (inQuotes) return false;
    if (!field.empty() || !row.empty()) return false;  // the file must end with a proper CRLF
    return sawAny;
}

static void check(bool ok, const char* what) {
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

static void fill(ut::UtReplicaCapture* c, const char* record, unsigned int stack, int slots) {
    memset(c, 0, sizeof(*c));
    _snprintf_s(c->record, sizeof(c->record), _TRUNCATE, "%s", record);
    c->stack = stack;
    c->replicaLen = 0x190;
    for (unsigned int i = 0; i < c->replicaLen; ++i) c->replica[i] = (unsigned char)(i & 0xFF);
    c->slotCount = slots;
    for (int i = 0; i < slots; ++i) {
        c->slotOff[i] = (unsigned int)(8 + i * 0x20);
        if (i == 0) {
            _snprintf_s(c->slotText[i], sizeof(c->slotText[i]), _TRUNCATE, "%s", record);
        } else {
            _snprintf_s(c->slotText[i], sizeof(c->slotText[i]), _TRUNCATE,
                        "records/items/affix_%d.dbr", i);
        }
    }
}

// ---- the identity fixture --------------------------------------------------------------------
// The synthetic "deposited" item. Numeric fields go where nothing else writes; string slots are
// laid out the way a real ItemReplicaInfo carries them (+0x08 is the base record - the one
// Item::CreateItem itself proves).
static const char* const kIdRecord = "records/items/gearhead/z99_identity.dbr";
static const unsigned int kIdLen = 0x190;
static const unsigned int kIdStackOff = 0x178;  // == Item+0x6B0 - Item+0x538 on 1.3.0.8

struct NumField {
    unsigned int off;
    unsigned int value;
    const char* name;
};

static const NumField kNums[] = {
    {0x120, 0x11223344u, "seed"},         {0x124, 0x55667788u, "relicSeed"},
    {0x128, 0x99AABBCCu, "enchantmentSeed"}, {0x12C, 7u, "materiaCombines"},
    {0x130, 3u, "seedRerolls"},           {0x134, 5u, "affixRerolls"},
    {0x138, 0xFEEDFACEu, "relicCompletionBonusSeed"},
};
static const int kNumCount = (int)(sizeof(kNums) / sizeof(kNums[0]));

struct StrField {
    unsigned int off;
    const char* text;
    const char* name;
};

static const StrField kStrs[] = {
    {0x08, "records/items/gearhead/z99_identity.dbr", "baseRecord"},
    {0x28, "records/items/lootaffixes/prefix/c_gearhead_veterans.dbr", "prefix"},
    {0x48, "records/items/lootaffixes/suffix/d_ofkings.dbr", "suffix"},
    {0x68, "records/items/materia/relic_seal_of_might.dbr", "materia (component)"},
    {0x88, "records/items/enchants/augment_kymon_01.dbr", "enchantment (augment)"},
    {0xA8, "up.dbr", "ascendant (short - stays in the SSO buffer)"},
};
static const int kStrCount = (int)(sizeof(kStrs) / sizeof(kStrs[0]));

// The entry the deposit detour would have produced for that item.
static void fillIdentity(ut::UtReplicaCapture* c) {
    memset(c, 0, sizeof(*c));
    _snprintf_s(c->record, sizeof(c->record), _TRUNCATE, "%s", kIdRecord);
    c->stack = 1;
    c->replicaLen = kIdLen;
    // A recognisable filler so a byte that did NOT come from the journal is obvious.
    for (unsigned int i = 0; i < kIdLen; ++i) c->replica[i] = (unsigned char)(0xA0 + (i & 0x0F));
    const unsigned int zero = 0;
    memcpy(c->replica, &zero, 4);  // captureReplicaRaw zeroes the object id
    const unsigned int one = 1;
    memcpy(c->replica + kIdStackOff, &one, 4);
    for (int i = 0; i < kNumCount; ++i) {
        memcpy(c->replica + kNums[i].off, &kNums[i].value, 4);
    }
    c->slotCount = kStrCount;
    for (int i = 0; i < kStrCount; ++i) {
        c->slotOff[i] = kStrs[i].off;
        _snprintf_s(c->slotText[i], 160, _TRUNCATE, "%s", kStrs[i].text);
    }
}

// The replica the ENGINE was about to create from: same record, everything else wrong.
static void fillIncoming(unsigned char* p) {
    for (unsigned int i = 0; i < kIdLen; ++i) p[i] = 0x5A;
    const unsigned int id = 0x99999999u;
    memcpy(p, &id, 4);
    const unsigned int stack = 7;
    memcpy(p + kIdStackOff, &stack, 4);
}

static unsigned int u32At(const unsigned char* p, unsigned int off) {
    unsigned int v = 0;
    memcpy(&v, p + off, 4);
    return v;
}

// ---- the straddling layout -------------------------------------------------------------------
// c304_sword2h (Frostshriek) had string slots at +0x08, +0x28, +0x48, +0x70, +0x90, +0xB0,
// +0xD8, +0x100, +0x120, +0x140 - the +0xB0 one EMPTY and the +0xD8 one a 39-character
// component/relic path. +0xD0 is then the only 8-aligned window in that layout that OVERLAPS a
// rebuilt slot without STARTING inside one: it read its "size" from +0xE0 (the zeroed half of
// the rebuilt SSO union at +0xD8) and its "capacity" from +0xE8 - which is that string's LENGTH,
// 39. Without an overlap test identityBuild refuses such an entry precisely BECAUSE the item
// carries an identity, which is the one case that matters.
static const char* const kStraddleRecord = "records/items/melee2h/c304_sword2h.dbr";

static void straddleChecks() {
    printf("  --- the straddling layout: empty slot at +0xB0, 39-char string at +0xD8 ---\n");
    char comp[64];
    _snprintf_s(comp, sizeof(comp), _TRUNCATE, "records/items/materia/c_frost_%s.dbr", "12345");
    check(strlen(comp) == 39, "the fixture's component path is 39 characters");

    ut::UtReplicaCapture m;
    memset(&m, 0, sizeof(m));
    _snprintf_s(m.record, sizeof(m.record), _TRUNCATE, "%s", kStraddleRecord);
    m.stack = 1;
    m.replicaLen = kIdLen;
    for (unsigned int i = 0; i < kIdLen; ++i) m.replica[i] = (unsigned char)(0xA0 + (i & 0x0F));
    const unsigned int zero = 0;
    memcpy(m.replica, &zero, 4);
    const unsigned int one = 1;
    memcpy(m.replica + kIdStackOff, &one, 4);
    // The numeric field at +0xD0 that happened to hold a plausible-looking pointer: without it
    // the straddle window would be skipped for a different reason and the case would not
    // reproduce.
    const unsigned long long looksLikePtr = 0x000001F2A3B4C5D0ull;
    memcpy(m.replica + 0xD0, &looksLikePtr, sizeof(looksLikePtr));
    m.slotCount = 3;
    m.slotOff[0] = 0x08;
    _snprintf_s(m.slotText[0], 160, _TRUNCATE, "%s", kStraddleRecord);
    m.slotOff[1] = 0xB0;
    m.slotText[1][0] = 0;  // the EMPTY slot
    m.slotOff[2] = 0xD8;
    _snprintf_s(m.slotText[2], 160, _TRUNCATE, "%s", comp);

    unsigned char incoming[0x200];
    memset(incoming, 0, sizeof(incoming));
    fillIncoming(incoming);

    ut::UtIdentityOverlay ov;
    const bool built = ut::identityBuild(m, incoming, kIdLen, kIdStackOff, &ov);
    check(built, "the straddling layout is ACCEPTED (the +0xD0 window does not refuse it)");
    if (!built) printf("      refusal says: %s\n", ov.why);
    check(ov.slotCount == 3 && ov.slotsSkipped == 0, "all three slots were rebuilt");
    char text[200] = {0};
    check(ut::utReadMsvcString(ov.replica, 0xD8, kIdLen, text, sizeof(text)) &&
              !strcmp(text, comp),
          "the 39-character component path survived the rebuild");
    text[0] = 1;
    check(ut::utReadMsvcString(ov.replica, 0xB0, kIdLen, text, sizeof(text)) && !text[0],
          "the empty slot came back as a valid empty string");
    check(ov.bytesComparable > 0 && ov.bytesFromJournal <= ov.bytesComparable,
          "bytesFromJournal is reported against a real denominator");

    // ...and a REAL dangling pointer in the same layout is still refused: +0x160 is nowhere near
    // a rebuilt slot, so the overlap test cannot swallow it.
    ut::UtReplicaCapture ghost = m;
    const unsigned long long ptr = 0x00007FF012345678ull;
    const size_t gsize = 20, gcap = 31;
    memcpy(ghost.replica + 0x160, &ptr, sizeof(ptr));
    memcpy(ghost.replica + 0x170, &gsize, sizeof(gsize));
    memcpy(ghost.replica + 0x178, &gcap, sizeof(gcap));
    ut::UtIdentityOverlay bad;
    check(!ut::identityBuild(ghost, incoming, kIdLen, 0, &bad),
          "a genuine un-rebuilt heap std::string in that layout is STILL refused");
    printf("      refusal says: %s\n", bad.why);
}

// ---- the relic freeze ------------------------------------------------------------------------
// ItemReplicaInfo carries a u32 at +0x68 (the relic seed) and a std::string at +0x70 (the
// completion-bonus record). An "accept an EMPTY heap string without dereferencing" rule on its own
// accepted the window at +0x68 - it read its "size" from +0x78 (0) and its "capacity" from +0x80
// (56, the REAL string's _Mysize) - and never looked at +0x70. identityBuild then memset
// 0x68..0x87, so the engine saw _Ptr = 0, _Mysize = 15, _Myres = 63 at +0x70 and did
// memcpy(dst, NULL, 15). This fixture is that exact blob.
static char kRelicBonus[] = "records/items/lootaffixes/completionrelics/anecro_02a.dbr";
static const char* const kRelicRecord = "records/items/gearrelic/c101_relic.dbr";

struct TestBlob {
    unsigned char* p;
    unsigned int len;
};

// A plain mirror of ut_reagent's looksLikeStringEx over memory THIS process owns, so the offline
// test drives the shipped scan (ut::utScanReplicaSlots) with no game and no SEH.
static void testProbe(void* ctx, unsigned int off, bool allowEmptyHeap,
                      ut::UtSlotProbeResult* out) {
    const TestBlob* b = (const TestBlob*)ctx;
    memset(out, 0, sizeof(*out));
    if (off + 0x20 > b->len) return;
    const unsigned char* p = b->p + off;
    memcpy(&out->ptr, p, sizeof(out->ptr));
    size_t size = 0, cap = 0;
    memcpy(&size, p + 0x10, sizeof(size));
    memcpy(&cap, p + 0x18, sizeof(cap));
    out->cap = cap;
    if (cap < 15 || cap > 0x4000 || size > cap) return;
    const char* text = nullptr;
    if (cap == 15) {
        text = (const char*)p;
        size_t n = 0;
        while (n < 16 && text[n]) ++n;
        if (n != size) return;
    } else {
        memcpy(&text, p, sizeof(text));
        if (!text) return;
        if (size == 0 && allowEmptyHeap) {  // the empty-heap rule: accepted, never dereferenced
            out->emptyHeap = true;
            out->ok = true;
            return;
        }
        // The engine-side probe is SEH-guarded because a plausible header can carry a garbage
        // pointer (in this very fixture the window at +0x00 does). Mirror that here, or the
        // test crashes where the mod would merely count a fault.
        __try {
            size_t n = 0;
            while (n <= size && text[n]) ++n;
            if (n != size) return;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            out->faulted = true;
            return;
        }
    }
    _snprintf_s(out->text, sizeof(out->text), _TRUNCATE, "%.*s", (int)size, size ? text : "");
    out->ok = true;
}

static void putHeapString(unsigned char* blob, unsigned int off, char* text, size_t capacity) {
    memset(blob + off, 0, 0x20);
    char* p = text;
    memcpy(blob + off, &p, sizeof(p));
    const size_t n = strlen(text);
    memcpy(blob + off + 0x10, &n, sizeof(n));
    memcpy(blob + off + 0x18, &capacity, sizeof(capacity));
}

static void putEmptySso(unsigned char* blob, unsigned int off) {
    memset(blob + off, 0, 0x20);
    const size_t cap = 15;
    memcpy(blob + off + 0x18, &cap, sizeof(cap));
}

static void relicChecks() {
    printf("  --- the relic (u32 at +0x68, heap std::string at +0x70) ---\n");
    static char base[] = "records/items/gearrelic/c101_relic.dbr";
    static unsigned char blob[kIdLen];
    for (unsigned int i = 0; i < kIdLen; ++i) blob[i] = (unsigned char)(0xA0 + (i & 0x0F));
    const unsigned int zero = 0;
    memcpy(blob, &zero, 4);
    const unsigned int one = 1;
    memcpy(blob + kIdStackOff, &one, 4);
    putHeapString(blob, 0x08, base, 47);
    putEmptySso(blob, 0x28);
    putEmptySso(blob, 0x48);
    const unsigned int relicSeed = 0x30DE595Fu;  // a value out of a real relic's hexdump
    memcpy(blob + 0x68, &relicSeed, 4);
    memset(blob + 0x6C, 0, 4);
    putHeapString(blob, 0x70, kRelicBonus, 63);  // _Mysize 56 at +0x80, _Myres 63 at +0x88
    putEmptySso(blob, 0x90);
    putHeapString(blob, 0xB0, kRelicBonus, 63);
    check(strlen(kRelicBonus) > 15,
          "the fixture's completion-bonus path is long enough to be heap-allocated (a real "
          "relic's was 56 characters)");

    // (1) THE CAPTURE. This is the shipped scan, driven with a plain probe.
    TestBlob tb;
    tb.p = blob;
    tb.len = kIdLen;
    unsigned int offs[24];
    char texts[24][160];
    ut::UtSlotShape shapes[24];
    memset(offs, 0, sizeof(offs));
    memset(texts, 0, sizeof(texts));
    memset(shapes, 0, sizeof(shapes));
    int faults = 0;
    const int n =
        ut::utScanReplicaSlots(&tb, kIdLen, &testProbe, offs, texts, shapes, 24, &faults);
    bool has70 = false, has68 = false;
    int at70 = -1;
    for (int i = 0; i < n; ++i) {
        if (offs[i] == 0x70) {
            has70 = true;
            at70 = i;
        }
        if (offs[i] == 0x68) has68 = true;
    }
    printf("      the scan recorded %d slot(s):", n);
    for (int i = 0; i < n; ++i) printf(" +0x%03X", offs[i]);
    printf("\n");
    check(n == 6, "the scan finds six slots");
    check(!has68, "the FALSE +0x068 slot is NOT recorded (the relic freeze)");
    check(has70, "the REAL +0x070 slot is recorded instead");
    check(at70 >= 0 && !strcmp(texts[at70], kRelicBonus),
          "and it carries the relic's completion-bonus record");

    // (2) identityBuild accepts the capture the new scan produces.
    ut::UtReplicaCapture c;
    memset(&c, 0, sizeof(c));
    _snprintf_s(c.record, sizeof(c.record), _TRUNCATE, "%s", kRelicRecord);
    c.stack = 1;
    c.replicaLen = kIdLen;
    memcpy(c.replica, blob, kIdLen);
    c.slotCount = n;
    for (int i = 0; i < n; ++i) {
        c.slotOff[i] = offs[i];
        memcpy(c.slotText[i], texts[i], 160);
    }
    unsigned char incoming[0x200];
    memset(incoming, 0, sizeof(incoming));
    fillIncoming(incoming);
    ut::UtIdentityOverlay ov;
    const bool built = ut::identityBuild(c, incoming, kIdLen, kIdStackOff, &ov);
    check(built, "identityBuild ACCEPTS the relic capture the new scan produces");
    if (!built) printf("      refusal says: %s\n", ov.why);
    char text[200] = {0};
    check(built && ut::utReadMsvcString(ov.replica, 0x70, kIdLen, text, sizeof(text)) &&
              !strcmp(text, kRelicBonus),
          "the completion-bonus record survives the rebuild at +0x070");

    // (3) THE NEGATIVE CONTROL: the blob as a capture without the freeze produces it, with the
    // slot at +0x068. That shape froze the game: it must be refused before anything is handed over.
    ut::UtReplicaCapture bad = c;
    bad.slotCount = 6;
    const unsigned int oldOffs[6] = {0x08, 0x28, 0x48, 0x68, 0x90, 0xB0};
    for (int i = 0; i < 6; ++i) {
        bad.slotOff[i] = oldOffs[i];
        bad.slotText[i][0] = 0;
    }
    _snprintf_s(bad.slotText[0], 160, _TRUNCATE, "%s", base);
    _snprintf_s(bad.slotText[5], 160, _TRUNCATE, "%s", kRelicBonus);
    ut::UtIdentityOverlay bov;
    check(!ut::identityBuild(bad, incoming, kIdLen, kIdStackOff, &bov),
          "the OLD +0x068 capture is REFUSED (this is the blob that froze the game)");
    printf("      refusal says: %s\n", bov.why);
    check(strstr(bov.why, "NULL string pointer") != nullptr,
          "and the refusal names the NULL string pointer, with its offset");

    // ---- (4) THE SWAP OVERLAY ----------------------------------------------------------------
    // The re-deposit path no longer applies a replica onto the LIVE stored prototype: this very
    // relic is why (ItemRelic::InitializeItem re-parses the completion bonus and ADDS its grant
    // again, so a Mortality relic read +1, +2, +3 Drain Essence over three re-deposits).
    // A brand-new prototype is created from this overlay instead, which
    // means it is the replica an object is BORN from and two fields differ from identityBuild's:
    // the object id must be ZERO (the engine zeroes replica+0x00 itself at 0x2CECBD before its
    // own Item::CreateItem) and the stack mirror must be 1 whatever the incoming replica carried.
    printf("  --- the fresh-prototype swap overlay (relic layout) ---\n");
    ut::UtIdentityOverlay sov;
    const bool swapBuilt = ut::utBuildSwapOverlay(c, incoming, kIdLen, kIdStackOff, &sov);
    check(swapBuilt, "utBuildSwapOverlay ACCEPTS the relic capture");
    if (!swapBuilt) printf("      refusal says: %s\n", sov.why);
    check(swapBuilt && u32At(sov.replica, 0) == 0u,
          "+0x000 (the object id) is ZERO - the incoming replica carried 0x99999999");
    char srec[200] = {0};
    check(swapBuilt && ut::utReadMsvcString(sov.replica, 0x08, kIdLen, srec, sizeof(srec)) &&
              !strcmp(srec, base),
          "+0x008 reads back as the node's own record, so CreateObjectFromFile builds the right "
          "object");
    check(swapBuilt && u32At(sov.replica, kIdStackOff) == 1u,
          "+0x178 (the stack mirror) is 1 - the incoming replica carried 7");
    check(swapBuilt && !sov.keptObjectId && !sov.keptStack,
          "the overlay reports that neither the id nor the stack came from the engine");
    char sbonus[200] = {0};
    check(swapBuilt && ut::utReadMsvcString(sov.replica, 0x70, kIdLen, sbonus, sizeof(sbonus)) &&
              !strcmp(sbonus, kRelicBonus),
          "and the completion-bonus record still survives at +0x070");

    // Every refusal identityBuild makes must refuse a SWAP too - it is the same gate.
    ut::UtIdentityOverlay sbad;
    check(!ut::utBuildSwapOverlay(bad, incoming, kIdLen, kIdStackOff, &sbad),
          "the OLD +0x068 capture is refused for a SWAP as well (it would be handed to "
          "Item::CreateItem, not to an assignment)");
    // A stack mirror that does not fit is refused outright: a prototype born with stack 0 paints
    // an invisible box and the exe take bails at 0x132AE0.
    ut::UtIdentityOverlay snost;
    check(!ut::utBuildSwapOverlay(c, incoming, kIdLen, 0, &snost),
          "a swap overlay with no stack-mirror offset is refused");
    printf("      refusal says: %s\n", snost.why);
}

static void identityChecks(const char* label) {
    printf("  --- identity overlay (%s) ---\n", label);
    ut::UtReplicaCapture entry;
    if (!ut::journalGet(kIdRecord, &entry)) {
        check(false, "journalGet returns the identity entry");
        return;
    }
    check(true, "journalGet returns the identity entry");
    check(entry.replicaLen == kIdLen, "the entry still carries a 0x190-byte replica");
    check(entry.slotCount == kStrCount, "the entry still carries every string slot");

    unsigned char incoming[0x200];
    memset(incoming, 0, sizeof(incoming));
    fillIncoming(incoming);

    ut::UtIdentityOverlay ov;
    const bool built = ut::identityBuild(entry, incoming, kIdLen, kIdStackOff, &ov);
    check(built, "identityBuild accepts the entry");
    if (!built) {
        printf("      why: %s\n", ov.why);
        return;
    }
    check(ov.replicaLen == kIdLen, "the substitute is 0x190 bytes");
    check(ov.slotsSkipped == 0, "no slot was skipped");
    check(ov.slotCount == kStrCount, "every string slot was rebuilt");

    // The two fields that belong to the LIVE creation, not to the stored identity.
    check(ov.keptObjectId, "the object id was taken from the engine's replica");
    check(u32At(ov.replica, 0) == 0x99999999u, "object id == the incoming one, not the stored 0");
    check(ov.keptStack, "the stack mirror was taken from the engine's replica");
    check(u32At(ov.replica, kIdStackOff) == 7u, "stack == the incoming 7, not the stored 1");

    // Everything else is the deposited item.
    for (int i = 0; i < kNumCount; ++i) {
        char what[128];
        _snprintf_s(what, sizeof(what), _TRUNCATE, "%s survived (0x%08X)", kNums[i].name,
                    kNums[i].value);
        check(u32At(ov.replica, kNums[i].off) == kNums[i].value, what);
    }
    for (int i = 0; i < kStrCount; ++i) {
        char text[200] = {0};
        char what[192];
        const bool read = ut::utReadMsvcString(ov.replica, kStrs[i].off, kIdLen, text,
                                               sizeof(text));
        _snprintf_s(what, sizeof(what), _TRUNCATE, "%s reads back as \"%.60s\"", kStrs[i].name,
                    kStrs[i].text);
        check(read && !strcmp(text, kStrs[i].text), what);
    }

    // The long ones must be HEAP-shaped and must point inside the overlay, not at the journal's
    // vector or at freed engine memory: that pointer is what Item::CreateItem dereferences.
    {
        const unsigned char* p = ov.replica + kStrs[1].off;
        size_t cap = 0;
        memcpy(&cap, p + 0x18, sizeof(cap));
        const char* ptr = nullptr;
        memcpy(&ptr, p, sizeof(ptr));
        check(cap > 15, "a >15-char slot is stored heap-shaped, like a real std::string");
        const char* lo = (const char*)&ov;
        const char* hi = lo + sizeof(ov);
        check(ptr >= lo && ptr < hi, "its buffer lives INSIDE the overlay (it outlives the call)");
    }
    // The short one must stay in the SSO buffer, capacity 15 - no pointer at all.
    {
        const unsigned char* p = ov.replica + kStrs[5].off;
        size_t cap = 0, size = 0;
        memcpy(&size, p + 0x10, sizeof(size));
        memcpy(&cap, p + 0x18, sizeof(cap));
        check(cap == 15 && size == strlen(kStrs[5].text), "a <16-char slot stays SSO, capacity 15");
    }

    // Bytes outside the slots that the journal actually changed. Every one of the 0x190 filler
    // bytes differs from the incoming 0x5A except the 4-byte id and the 4-byte stack.
    unsigned int expect = 0;
    for (unsigned int j = 0; j < kIdLen; ++j) {
        bool inSlot = false;
        for (int i = 0; i < kStrCount; ++i) {
            if (j >= kStrs[i].off && j < kStrs[i].off + 0x20) inSlot = true;
        }
        if (inSlot) continue;
        if (j < 4 || (j >= kIdStackOff && j < kIdStackOff + 4)) continue;
        ++expect;
    }
    char what[160];
    _snprintf_s(what, sizeof(what), _TRUNCATE,
                "bytesFromJournal == %u (every non-slot byte but the id and the stack)", expect);
    check((unsigned int)ov.bytesFromJournal == expect, what);

    // Refusals. A replica whose size moved (a patch, or a journal from another build) must be
    // REFUSED, never forced - that is how a save-format change stays a non-event.
    ut::UtIdentityOverlay bad;
    check(!ut::identityBuild(entry, incoming, kIdLen - 8, kIdStackOff, &bad),
          "a replica-size mismatch is refused");
    printf("      refusal says: %s\n", bad.why);

    // A slot the live replica cannot hold is skipped, never written out of bounds.
    ut::UtReplicaCapture oob = entry;
    oob.slotOff[oob.slotCount - 1] = kIdLen - 4;
    ut::UtIdentityOverlay ov2;
    check(ut::identityBuild(oob, incoming, kIdLen, kIdStackOff, &ov2), "an out-of-range slot "
          "still builds");
    check(ov2.slotsSkipped == 1 && ov2.slotCount == kStrCount - 1,
          "the out-of-range slot is skipped, not written past the end");

    // stackOff == 0 (the decode failed) keeps the journal's own count instead of guessing.
    ut::UtIdentityOverlay ov3;
    check(ut::identityBuild(entry, incoming, kIdLen, 0, &ov3), "stackOff 0 still builds");
    check(!ov3.keptStack && u32At(ov3.replica, kIdStackOff) == 1u,
          "with no decoded stack offset the journal's own count is kept");

    // The two dangling-pointer refusals. Both exist because Item::CreateItem READS the
    // std::string members of the blob it is handed: a slot the capture missed still holds the
    // deposited item's heap pointer, freed long ago.
    {
        ut::UtReplicaCapture noBase = entry;
        for (int i = 0; i < noBase.slotCount - 1; ++i) {
            noBase.slotOff[i] = noBase.slotOff[i + 1];
            memcpy(noBase.slotText[i], noBase.slotText[i + 1], 160);
        }
        --noBase.slotCount;
        ut::UtIdentityOverlay bad2;
        check(!ut::identityBuild(noBase, incoming, kIdLen, kIdStackOff, &bad2),
              "an entry with no base-record slot at +0x08 is refused");
        printf("      refusal says: %s\n", bad2.why);
    }
    {
        // A heap-shaped std::string header at an offset the capture never recorded.
        ut::UtReplicaCapture ghost = entry;
        const unsigned long long ptr = 0x00007FF012345678ull;
        const size_t size = 20, cap = 31;
        memcpy(ghost.replica + 0x100, &ptr, sizeof(ptr));
        memcpy(ghost.replica + 0x110, &size, sizeof(size));
        memcpy(ghost.replica + 0x118, &cap, sizeof(cap));
        ut::UtIdentityOverlay bad3;
        check(!ut::identityBuild(ghost, incoming, kIdLen, kIdStackOff, &bad3),
              "an un-rebuilt heap std::string in the blob is refused (dangling pointer)");
        printf("      refusal says: %s\n", bad3.why);
    }
    straddleChecks();
    relicChecks();
}


// ================= the readable-format cases ===================================================
//
// Each case is its OWN PROCESS with its OWN %UNIQUETAB_OUT% folder (build_test_journal.bat sets
// it), because journalInit resolves the path and parses the file exactly once per process - that
// is the behaviour under test. Nothing here goes anywhere near the main repo's journal.

static int caseMain(const char* mode, const char* arg) {
    wchar_t log[MAX_PATH];
    GetTempPathW(MAX_PATH, log);
    wcscat_s(log, MAX_PATH, L"uniq-journal-test.log");
    ut::logInit(log);

    char outDir[MAX_PATH] = {0};
    ut::utModDirA(nullptr, outDir, sizeof(outDir));
    printf("case %s   out dir: %s\n", mode, outDir);

    // ---- the two fixture BUILDERS: a valid 3-entry journal, then a deliberate corruption ------
    // ---- the failure-path fixtures --------------------------------------------------------------
    // (d) a journal that is THERE and cannot be used at all: line 1 is not JSON. A uniq-items.bin
    //     is put next to it, because the dangerous half of the old behaviour was that the ladder
    //     would then MIGRATE that .bin straight over the file it had just failed to read.
    // (f/g/h): the FORMAT-2 fixture. The harness prefers a COPY of the user's
    // real 358-entry format-2 journal and only falls back to this synthetic one when that file is
    // not on the machine - so this branch writes nothing if a fixture is already in place, and
    // never touches an original anywhere. The text is written by hand, on purpose: it has to be a
    // genuine format-2 file (sOOO keys, no "stored"), which this build no longer emits.
    if (!strcmp(mode, "--make-v2")) {
        char jsonPath[MAX_PATH];
        _snprintf_s(jsonPath, sizeof(jsonPath), _TRUNCATE, "%s\\uniq-items.jsonl", outDir);
        std::string have;
        if (slurp(jsonPath, &have) && have.find("\"format\":2") != std::string::npos) {
            printf("  a real format-2 fixture is already in place (%zu bytes, %d lines)\n",
                   have.size(), countRecordLines(have));
            check(true, "the real format-2 journal is the fixture");
            printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail,
                   g_fail == 1 ? "" : "s");
            return g_fail ? 1 : 0;
        }
        // Say so, loudly, when the real file could NOT be
        // used. build_test_journal.bat prints "a COPY of the LIVE ..." from `if exist` alone,
        // before this decides anything - and the moment this build has run for real the live
        // journal is format 3, the copy is overwritten here, and cases f/g/h quietly stop testing
        // the user's 358-entry file. The substitution is now named in the same output.
        if (!have.empty()) {
            printf("  [SUBSTITUTED - the real-file case was NOT run] a fixture was copied in "
                   "(%zu bytes) but it is\n"
                   "  not format 2 any more (the live journal has been upgraded). Cases f/g/h run "
                   "on the\n"
                   "  SYNTHETIC 4-entry file below, not on the user's real journal. To test the "
                   "real one\n"
                   "  again, freeze a format-2 copy and point LIVEJSONL at it.\n",
                   have.size());
        }
        std::string t =
            "{\"journal\":\"grim dawn uniquetab\",\"format\":2,"
            "\"written\":\"2026-09-07T22:38:38.1760344Z\",\"entries\":4}\n";
        static const char* const kRec[4] = {"records/items/gearfeet/d101_feet.dbr",
                                            "records/items/gearlegs/c101_legs.dbr",
                                            "records/items/gearrelic/b102_relic.dbr",
                                            "records/items/gearhead/c203_head.dbr"};
        static const char* const kComp[4] = {
            "records/items/materia/compa_markofthetraveler.dbr", "",
            "", "records/items/materia/compa_polishedemerald.dbr"};
        for (int i = 0; i < 4; ++i) {
            char line[1024];
            _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "{\"record\":\"%s\",\"deposited\":\"2026-09-07T22:38:%02u.0000000Z\","
                        "\"stack\":1,\"flags\":0,\"flagsText\":\"none\",\"len\":400,"
                        "\"s008\":\"%s\",\"s028\":\"\",\"s048\":\"\",\"s070\":\"\","
                        "\"s090\":\"%s\",\"s0B0\":\"\",\"s0D8\":\"\",\"s100\":\"\","
                        "\"s120\":\"\",\"s140\":\"\","
                        "\"raw\":\"068:%08X 164:00000001 178:00000001\"}\n",
                        kRec[i], (unsigned int)(20 + i), kRec[i], kComp[i],
                        0x10EB2C33u + (unsigned int)i);
            t.append(line);
        }
        check(spit(jsonPath, t), "a synthetic format-2 fixture was written");
        printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail,
               g_fail == 1 ? "" : "s");
        return g_fail ? 1 : 0;
    }
    if (!strcmp(mode, "--make-unreadable")) {
        char jsonPath[MAX_PATH], binPath[MAX_PATH];
        _snprintf_s(jsonPath, sizeof(jsonPath), _TRUNCATE, "%s\\uniq-items.jsonl", outDir);
        _snprintf_s(binPath, sizeof(binPath), _TRUNCATE, "%s\\uniq-items.bin", outDir);
        check(spit(jsonPath, "this is not a JSON object at all\nand neither is this\n"),
              "an unreadable journal fixture was written");
        check(spit(binPath, "UNIQITM0 not a real binary journal either"),
              "a uniq-items.bin was put next to it");
        printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail,
               g_fail == 1 ? "" : "s");
        return g_fail ? 1 : 0;
    }
    // (e) two HOSTILE lines appended to a good 3-entry journal: the 0xFFFFFFFC "raw" offset whose
    //     bounds test used to wrap (a 4-byte write 4 GB past a 400-byte vector), and a len-0
    //     entry, which the reconcile path really does create and which used to be dropped.
    if (!strcmp(mode, "--make-hostile")) {
        if (!ut::journalInit(nullptr)) {
            printf("  [FAIL] journalInit\n");
            return 1;
        }
        ut::UtReplicaCapture a, b, c;
        fill(&a, "records/items/gearhead/a01_head.dbr", 1, 3);
        fill(&b, "records/items/gearweapons/b02_sword.dbr", 1, 1);
        fill(&c, "records/items/gearhands/c03_gloves.dbr", 2, 2);
        check(ut::journalUpsert(a) && ut::journalUpsert(b) && ut::journalUpsert(c),
              "three fixture entries written");
        ut::journalService();
        std::string text;
        check(slurp(ut::journalPath(), &text), "the fixture file was written");
        text.append(
            "{\"record\":\"records/items/hostile/raw_overflow.dbr\",\"stack\":1,\"flags\":0,"
            "\"len\":400,\"s008\":\"records/items/hostile/raw_overflow.dbr\","
            "\"raw\":\"FFFFFFFC:00000001\"}\n");
        text.append(
            "{\"record\":\"records/items/hostile/no_replica.dbr\",\"stack\":1,\"flags\":1,"
            "\"flagsText\":\"synthesized\",\"len\":0,\"raw\":\"\"}\n");
        // 3 good + the len-0 keeper = 4 parsed; the overflow line is meant to be dropped.
        const size_t at = text.find("\"entries\":3");
        check(at != std::string::npos, "the header carries the fixture count");
        if (at != std::string::npos) text.replace(at, 11, "\"entries\":4");
        check(spit(ut::journalPath(), text), "two hostile lines were appended");
        ut::logFlush();
        printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail,
               g_fail == 1 ? "" : "s");
        return g_fail ? 1 : 0;
    }
    if (!strcmp(mode, "--make-newer") || !strcmp(mode, "--make-mangled")) {
        if (!ut::journalInit(nullptr)) {
            printf("  [FAIL] journalInit\n");
            return 1;
        }
        ut::UtReplicaCapture a, b, c;
        fill(&a, "records/items/gearhead/a01_head.dbr", 1, 3);
        fill(&b, "records/items/gearweapons/b02_sword.dbr", 1, 1);
        fill(&c, "records/items/gearhands/c03_gloves.dbr", 2, 2);
        check(ut::journalUpsert(a) && ut::journalUpsert(b) && ut::journalUpsert(c),
              "three fixture entries written");
        ut::journalService();
        std::string text;
        check(slurp(ut::journalPath(), &text), "the fixture file was written");
        if (!strcmp(mode, "--make-newer")) {
            const size_t at = text.find("\"format\":4");
            check(at != std::string::npos, "the header carries format 4");
            if (at != std::string::npos) text.replace(at, 10, "\"format\":99");
            check(spit(ut::journalPath(), text), "the header was rewritten to format 99");
        } else {
            // Chop the LAST item line in half: exactly the damage a bad hand-edit does.
            size_t last = text.rfind("\n{\"record\"");
            check(last != std::string::npos, "there is a last item line to mangle");
            if (last != std::string::npos) {
                const size_t cut = last + 1 + (text.size() - last) / 2;
                text.erase(cut);
                text.append("\n");
            }
            check(spit(ut::journalPath(), text), "one line was deliberately mangled");
        }
        ut::logFlush();
        printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail,
               g_fail == 1 ? "" : "s");
        return g_fail ? 1 : 0;
    }

    // ---- (a) MIGRATION: a uniq-items.bin is in the folder and no .jsonl ----------------------
    if (!strcmp(mode, "--migrate")) {
        char binPath[MAX_PATH], jsonPath[MAX_PATH];
        _snprintf_s(binPath, sizeof(binPath), _TRUNCATE, "%s\\uniq-items.bin", outDir);
        _snprintf_s(jsonPath, sizeof(jsonPath), _TRUNCATE, "%s\\uniq-items.jsonl", outDir);
        check(GetFileAttributesA(binPath) != INVALID_FILE_ATTRIBUTES,
              "the binary journal fixture is in place");
        check(GetFileAttributesA(jsonPath) == INVALID_FILE_ATTRIBUTES,
              "there is no readable journal yet (that is what triggers the migration)");
        if (!ut::journalInit(nullptr)) {
            printf("  [FAIL] journalInit\n");
            return 1;
        }
        const size_t n = ut::journalCount();
        printf("  migrated entries: %zu\n", n);
        check(n > 0, "the binary journal's entries are in memory after the migration");
        check(GetFileAttributesA(jsonPath) != INVALID_FILE_ATTRIBUTES,
              "uniq-items.jsonl now exists");
        check(GetFileAttributesA(binPath) == INVALID_FILE_ATTRIBUTES,
              "uniq-items.bin was renamed away, not left in place");
        WIN32_FIND_DATAA fd;
        char pat[MAX_PATH];
        _snprintf_s(pat, sizeof(pat), _TRUNCATE, "%s\\uniq-items.bin.migrated-*", outDir);
        HANDLE fh = FindFirstFileA(pat, &fd);
        check(fh != INVALID_HANDLE_VALUE, "the old binary is KEPT as uniq-items.bin.migrated-*");
        if (fh != INVALID_HANDLE_VALUE) {
            printf("  kept as: %s\n", fd.cFileName);
            FindClose(fh);
        }
        std::string text;
        check(slurp(jsonPath, &text), "the readable journal reads back");
        check((size_t)countRecordLines(text) == n, "one text line per migrated entry");
        printf("  %zu bytes of text for %zu entries (the binary was %s)\n", text.size(), n,
               "larger");
        // The dump the RELOAD pass will compare against, field for field.
        std::string dump;
        dumpAll(&dump);
        check(arg != nullptr && spit(arg, dump), "the post-migration field dump was written");
        ut::logFlush();
        printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail,
               g_fail == 1 ? "" : "s");
        return g_fail ? 1 : 0;
    }

    // ---- (a, second half) THE ROUND TRIP: re-read the TEXT and compare every field -----------
    if (!strcmp(mode, "--reload")) {
        if (!ut::journalInit(nullptr)) {
            printf("  [FAIL] journalInit\n");
            return 1;
        }
        printf("  entries read back from the text: %zu\n", ut::journalCount());
        std::string want, got;
        check(arg != nullptr && slurp(arg, &want), "the post-migration dump is there to compare");
        dumpAll(&got);
        const bool same = (want == got);
        check(same,
              "EVERY FIELD of EVERY entry read back from uniq-items.jsonl matches the one read "
              "from uniq-items.bin - record, len, stack, flags, every slot offset and text, the "
              "flagged verdict, and identityBuild's whole engine-facing output");
        if (!same) {
            size_t i = 0;
            while (i < want.size() && i < got.size() && want[i] == got[i]) ++i;
            const size_t from = i > 120 ? i - 120 : 0;
            printf("  first difference at byte %zu\n    .bin  : %.240s\n    .jsonl: %.240s\n", i,
                   want.c_str() + from, got.c_str() + from);
        }
        ut::logFlush();
        printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail,
               g_fail == 1 ? "" : "s");
        return g_fail ? 1 : 0;
    }

    // ---- (b) a format NEWER than this build understands -> READ-ONLY, and it writes NOTHING ---
    if (!strcmp(mode, "--case-readonly")) {
        std::string before;
        char jsonPath[MAX_PATH];
        _snprintf_s(jsonPath, sizeof(jsonPath), _TRUNCATE, "%s\\uniq-items.jsonl", outDir);
        check(slurp(jsonPath, &before), "the format-99 fixture is in place");
        if (!ut::journalInit(nullptr)) {
            printf("  [FAIL] journalInit\n");
            return 1;
        }
        check(ut::journalReadOnly(), "a higher format number puts the journal in READ-ONLY mode");
        check(ut::journalCount() == 3, "the entries it CAN understand are still loaded and usable");
        ut::UtReplicaCapture d;
        fill(&d, "records/items/gearlegs/d04_legs.dbr", 1, 1);
        check(ut::journalUpsert(d), "a new deposit is still accepted into memory");
        ut::journalService();
        ut::journalService();
        std::string after;
        check(slurp(jsonPath, &after), "the file is still there");
        check(before == after,
              "the newer file was NOT rewritten - not one byte - which is the whole point of the "
              "read-only latch");
        check(after.find("d04_legs") == std::string::npos,
              "the new deposit did not leak into the newer file");
        ut::logFlush();
        printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail,
               g_fail == 1 ? "" : "s");
        return g_fail ? 1 : 0;
    }

    // ---- (c) ONE mangled line costs exactly ONE entry ----------------------------------------
    if (!strcmp(mode, "--case-mangled")) {
        if (!ut::journalInit(nullptr)) {
            printf("  [FAIL] journalInit\n");
            return 1;
        }
        printf("  entries after one mangled line: %zu\n", ut::journalCount());
        check(ut::journalCount() == 2, "a mangled line costs exactly ONE entry, not the file");
        check(ut::journalHas("records/items/gearhead/a01_head.dbr"), "the first entry survived");
        check(ut::journalHas("records/items/gearweapons/b02_sword.dbr"), "the second survived");
        check(!ut::journalHas("records/items/gearhands/c03_gloves.dbr"),
              "the mangled entry is the only casualty");
        check(!ut::journalReadOnly(), "a mangled line does NOT make the journal read-only");
        // The next write must copy the damaged file aside first.
        ut::UtReplicaCapture e;
        fill(&e, "records/items/gearlegs/e05_legs.dbr", 1, 1);
        check(ut::journalUpsert(e), "a new deposit is accepted");
        ut::journalService();
        WIN32_FIND_DATAA fd;
        char pat[MAX_PATH];
        _snprintf_s(pat, sizeof(pat), _TRUNCATE, "%s\\uniq-items.jsonl.bad-*", outDir);
        HANDLE fh = FindFirstFileA(pat, &fd);
        check(fh != INVALID_HANDLE_VALUE,
              "the damaged file was copied aside as uniq-items.jsonl.bad-* BEFORE being replaced");
        if (fh != INVALID_HANDLE_VALUE) {
            printf("  kept as: %s\n", fd.cFileName);
            FindClose(fh);
        }
        std::string text;
        check(slurp(ut::journalPath(), &text), "the repaired journal reads back");
        check(countRecordLines(text) == 3, "the repaired journal holds the 2 survivors + the new one");
        ut::logFlush();
        printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail,
               g_fail == 1 ? "" : "s");
        return g_fail ? 1 : 0;
    }

    // ---- (d) a journal that EXISTS and cannot be read: READ-ONLY, and nothing is written -------
    // The worst case: readTextFile fails, the ladder starts empty AND writable, and the
    // first deposit replaces the whole collection with a one-entry file - or, worse, the .bin
    // beside it is migrated straight over the evidence.
    if (!strcmp(mode, "--case-unreadable")) {
        char jsonPath[MAX_PATH], binPath[MAX_PATH];
        _snprintf_s(jsonPath, sizeof(jsonPath), _TRUNCATE, "%s\\uniq-items.jsonl", outDir);
        _snprintf_s(binPath, sizeof(binPath), _TRUNCATE, "%s\\uniq-items.bin", outDir);
        std::string before;
        check(slurp(jsonPath, &before), "the unreadable fixture is in place");
        if (!ut::journalInit(nullptr)) {
            printf("  [FAIL] journalInit\n");
            return 1;
        }
        check(ut::journalReadOnly(),
              "a journal that exists but cannot be read puts the session in READ-ONLY");
        check(ut::journalCount() == 0, "no entries were invented");
        check(GetFileAttributesA(binPath) != INVALID_FILE_ATTRIBUTES,
              "the uniq-items.bin beside it was NOT migrated or renamed");
        // NOT "uniq-items.bin.*": a trailing ".*" in a Win32 wildcard also matches a name with no
        // extension at all, i.e. uniq-items.bin itself. Name the two aside forms exactly.
        static const char* const kAside[2] = {"uniq-items.bin.migrated-*",
                                              "uniq-items.bin.unreadable-*"};
        for (int i = 0; i < 2; ++i) {
            WIN32_FIND_DATAA fd;
            char pat[MAX_PATH];
            _snprintf_s(pat, sizeof(pat), _TRUNCATE, "%s\\%s", outDir, kAside[i]);
            HANDLE fh = FindFirstFileA(pat, &fd);
            check(fh == INVALID_HANDLE_VALUE,
                  i == 0 ? "it was not renamed .migrated-* (no migration was even attempted)"
                         : "and it was not copied aside as .unreadable-* either");
            if (fh != INVALID_HANDLE_VALUE) {
                printf("  unexpected: %s\n", fd.cFileName);
                FindClose(fh);
            }
        }
        // The whole point: a deposit now must not be able to replace the file.
        ut::UtReplicaCapture e;
        fill(&e, "records/items/gearlegs/e05_legs.dbr", 1, 1);
        check(ut::journalUpsert(e), "a deposit is still accepted into memory");
        ut::journalService();
        std::string after;
        check(slurp(jsonPath, &after), "the file is still there");
        check(after == before, "and NOT ONE BYTE of it was rewritten");
        ut::logFlush();
        printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail,
               g_fail == 1 ? "" : "s");
        return g_fail ? 1 : 0;
    }

    // ---- (e) the two hostile lines ------------------------------------------------------------
    if (!strcmp(mode, "--case-hostile")) {
        if (!ut::journalInit(nullptr)) {
            printf("  [FAIL] journalInit\n");
            return 1;
        }
        printf("  entries after the hostile lines: %zu\n", ut::journalCount());
        check(!ut::journalHas("records/items/hostile/raw_overflow.dbr"),
              "the wrapping \"raw\" offset 0xFFFFFFFC is DROPPED, not written 4 GB past the blob");
        check(ut::journalHas("records/items/hostile/no_replica.dbr"),
              "a len-0 entry (the reconcile's 'NO replica' shape) is KEPT, not dropped");
        check(ut::journalCount() == 4, "three good entries plus the len-0 one");
        check(!ut::journalReadOnly(), "neither line makes the journal read-only");
        // And the len-0 entry must survive a rewrite, or it would vanish at the next deposit.
        ut::UtReplicaCapture e;
        fill(&e, "records/items/gearlegs/e05_legs.dbr", 1, 1);
        check(ut::journalUpsert(e), "a new deposit is accepted");
        ut::journalService();
        std::string text;
        check(slurp(ut::journalPath(), &text), "the rewritten journal reads back");
        check(countRecordLines(text) == 5, "five lines: 3 good + the len-0 one + the new deposit");
        check(text.find("\"record\":\"records/items/hostile/no_replica.dbr\"") != std::string::npos,
              "the len-0 entry is still in the file after the rewrite");
        check(text.find("\"len\":0") != std::string::npos, "and it is still written as len 0");
        ut::logFlush();
        printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail,
               g_fail == 1 ? "" : "s");
        return g_fail ? 1 : 0;
    }

    // ---- (f): a FORMAT-2 file is upgraded to format 3 WITHOUT LOSS ---------------------------
    // The fixture the harness drops in is a COPY of the user's real 358-entry format-2 journal
    // (or, when that is not on this machine, a small synthetic one written by --make-v2). This
    // pass reads it, dumps every field, and forces one write; --case-upgraded re-reads the
    // rewritten file in a SEPARATE PROCESS and compares the dumps byte for byte.
    if (!strcmp(mode, "--case-upgrade")) {
        char jsonPath[MAX_PATH];
        _snprintf_s(jsonPath, sizeof(jsonPath), _TRUNCATE, "%s\\uniq-items.jsonl", outDir);
        std::string before;
        check(slurp(jsonPath, &before), "the format-2 fixture is in place");
        check(before.find("\"format\":2") != std::string::npos, "and it really says format 2");
        check(before.find("\"s008\":") != std::string::npos, "with format-2 sOOO slot keys");
        if (!ut::journalInit(nullptr)) {
            printf("  [FAIL] journalInit\n");
            return 1;
        }
        const size_t n = ut::journalCount();
        printf("  entries read out of the format-2 file: %zu\n", n);
        check(n > 0, "a format-2 file is READ, never rejected");
        check(!ut::journalReadOnly(), "an OLDER format does not make the journal read-only");
        check((size_t)countRecordLines(before) == n, "every line of the v2 file became an entry");
        size_t stored = 0, notStored = 0, unknown = 0, total = 0;
        ut::journalCounts(&total, &stored, &notStored, &unknown);
        check(total == n && stored == 0 && notStored == 0 && unknown == n,
              "every upgraded entry starts UNKNOWN - a v2 file says nothing about what is stored");
        std::string dump;
        dumpAll(&dump);
        check(arg != nullptr && spit(arg, dump), "the pre-upgrade field dump was written");
        // Force exactly one write without changing the set: add a throwaway record and take it
        // straight back out again.
        ut::UtReplicaCapture e;
        fill(&e, "records/items/upgrade/throwaway.dbr", 1, 1);
        check(ut::journalUpsert(e), "a throwaway entry is accepted");
        check(ut::journalRemove("records/items/upgrade/throwaway.dbr"), "and removed again");
        check(ut::journalCount() == n, "the entry count is back where it started");
        ut::journalService();
        std::string after;
        check(slurp(jsonPath, &after), "the upgraded journal reads back");
        check(after.find("\"format\":4") != std::string::npos, "the file is format 4 now");
        check(after.find("\"s008\":") == std::string::npos, "no sOOO key survived the upgrade");
        check(after.find("\"baseRecord@008\":") != std::string::npos,
              "the base record is named baseRecord@008");
        check((size_t)countRecordLines(after) == n, "and NOT ONE LINE was lost in the upgrade");
        ut::logFlush();
        printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail,
               g_fail == 1 ? "" : "s");
        return g_fail ? 1 : 0;
    }

    if (!strcmp(mode, "--case-upgraded")) {
        if (!ut::journalInit(nullptr)) {
            printf("  [FAIL] journalInit\n");
            return 1;
        }
        printf("  entries read back from the format-3 file: %zu\n", ut::journalCount());
        std::string want, got;
        check(arg != nullptr && slurp(arg, &want), "the pre-upgrade dump is there to compare");
        dumpAll(&got);
        const bool same = (want == got);
        check(same,
              "EVERY FIELD of EVERY entry survives the format 2 -> 3 upgrade - record, len, "
              "stack, flags, every slot offset and text, the flagged verdict, and identityBuild's "
              "whole engine-facing output");
        if (!same) {
            size_t i = 0;
            while (i < want.size() && i < got.size() && want[i] == got[i]) ++i;
            const size_t from = i > 120 ? i - 120 : 0;
            printf("  first difference at byte %zu\n    v2: %.240s\n    v3: %.240s\n", i,
                   want.c_str() + from, got.c_str() + from);
        }
        ut::logFlush();
        printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail,
               g_fail == 1 ? "" : "s");
        return g_fail ? 1 : 0;
    }

    // ---- (g): the reconciliation, with a KNOWN page ------------------------------------------
    // ut_reagent's journalReconcile walks the engine's map and then calls journalMarkStored once
    // per journal record. This pass is that second half with the map replaced by a fixed list -
    // the marking, the counts, the file order, and the prune - all without a game.
    // ---- the uniq-export.csv round trip ------------------------------------------------------
    // Write a journal that deliberately contains the two things RFC-4180 quoting exists for - a
    // comma and a double quote inside a field - then decode the CSV with a reader that knows
    // nothing about the writer and compare field by field.
    if (!strcmp(mode, "--case-csv")) {
        if (!ut::journalInit(nullptr)) {
            printf("  [FAIL] journalInit\n");
            return 1;
        }
        // This case is about the QUOTING, so it wants every row in the file - including the
        // not-stored one, which the default (mode 1, the collection) leaves out. Mode 2 is every
        // row. The mode itself is exercised by case --case-csvmode.
        ut::journalSetCsvExport(2);
        const char* kOdd = "records/items/odd/a \"quoted\", comma.dbr";
        // The writer quotes on a CR or an LF as well as on a comma or
        // a quote, and nothing exercised that half. This entry's PREFIX slot (offset 0x028, the
        // "prefix" column) carries a CRLF, so the field must come back quoted and must decode to
        // exactly the bytes that went in - and the reader must not mistake it for a record end.
        const char* kCrlfSlot = "records/items/affix_a\r\nrecords/items/affix_b.dbr";
        ut::UtReplicaCapture a, b, c, d;
        fill(&a, "records/items/gearhead/a01_head.dbr", 1, 3);
        fill(&b, kOdd, 2, 2);
        fill(&c, "records/items/gearhands/c03_gloves.dbr", 1, 1);
        fill(&d, "records/items/gearfeet/d04_boots.dbr", 1, 2);
        _snprintf_s(d.slotText[1], sizeof(d.slotText[1]), _TRUNCATE, "%s", kCrlfSlot);
        check(ut::journalUpsert(a) && ut::journalUpsert(b) && ut::journalUpsert(c) &&
                  ut::journalUpsert(d),
              "four fixture entries written");
        check(ut::journalMarkStored("records/items/gearhead/a01_head.dbr", true),
              "one entry is marked stored");
        check(ut::journalMarkStored("records/items/gearhands/c03_gloves.dbr", false),
              "one entry is marked NOT stored");
        ut::journalService();
        check(ut::journalCsvWrites() >= 1, "the CSV was written with the journal");
        std::string csv;
        check(slurp(ut::journalCsvPath(), &csv), "uniq-export.csv reads back off the disk");
        std::vector<std::vector<std::string> > rows;
        check(csvParse(csv, &rows), "it parses as RFC-4180 (CRLF records, doubled quotes)");
        check(rows.size() == 5, "one header row and four data rows");
        if (rows.size() == 5) {
            check(rows[0].size() == 14, "the header names 14 columns, count among them");
            check(rows[0][0] == "record" && rows[0][1] == "item" && rows[0][2] == "stored" &&
                      rows[0][3] == "count" && rows[0][6] == "stack" && rows[0][9] == "seed",
                  "the header columns are the ones the guide documents");
            size_t nCols = 0;
            bool sameWidth = true;
            for (size_t r = 0; r < rows.size(); ++r) {
                if (r == 0) nCols = rows[r].size();
                else if (rows[r].size() != nCols) sameWidth = false;
            }
            check(sameWidth, "every data row has exactly as many fields as the header");
            // The stored entries are written FIRST, so row 1 is the "yes" one.
            check(rows[1][0] == "records/items/gearhead/a01_head.dbr", "row 1 is the stored entry");
            check(rows[1][2] == "yes", "and its stored column says yes");
            check(rows[1][6] == "1", "and its stack column is the deposited stack");
            check(rows[1][3] == "0", "and its count column says 0 - it is MAP-owned");
            bool foundOdd = false, foundNo = false, foundCrlf = false;
            for (size_t r = 1; r < rows.size(); ++r) {
                if (rows[r][0] == "records/items/gearfeet/d04_boots.dbr") {
                    foundCrlf = true;
                    check(rows[r][10] == kCrlfSlot,
                          "a field carrying a CRLF round-trips byte for byte inside quotes");
                }
                if (rows[r][0] == kOdd) {
                    foundOdd = true;
                    // journalUpsert IS a deposit, so it marks the entry stored - "unknown"
                    // is only reachable for an entry read out of an older file that no
                    // reconciliation has looked at, which case (g) already covers.
                    check(rows[r][2] == "yes", "a freshly deposited entry says yes");
                    check(rows[r][6] == "2", "and carries its own stack");
                }
                if (rows[r][0] == "records/items/gearhands/c03_gloves.dbr") {
                    foundNo = true;
                    check(rows[r][2] == "no", "the not-stored entry says no");
                }
            }
            check(foundOdd,
                  "a record carrying a comma AND a double quote round-trips byte for byte");
            check(foundNo, "the not-stored entry is in the file too");
            check(foundCrlf, "the CRLF-carrying entry is in the file too");
        }
        // The raw text must show the quoting, not just decode to it.
        check(csv.find("\"records/items/odd/a \"\"quoted\"\", comma.dbr\"") != std::string::npos,
              "the odd record is quoted with its inner quotes DOUBLED in the raw bytes");
        check(csv.find("\r\n") != std::string::npos && csv.find("\n\n") == std::string::npos,
              "records are separated by CRLF");
        {
            std::string wantCrlf = "\"";
            wantCrlf += kCrlfSlot;
            wantCrlf += "\"";
            check(csv.find(wantCrlf) != std::string::npos,
                  "the CRLF field is QUOTED in the raw bytes, so its CRLF is not a record end");
        }
        // The switch really switches it off.
        ut::journalSetCsvExport(0);
        const long before = ut::journalCsvWrites();
        check(ut::journalUpsert(c), "another upsert to make the journal dirty");
        ut::journalService();
        check(ut::journalCsvWrites() == before, "export_csv=0 writes no CSV at all");
        check(slurp(ut::journalCsvPath(), &csv), "and the file that was there is LEFT there");
        ut::journalSetCsvExport(2);
        ut::logFlush();
        printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail,
               g_fail == 1 ? "" : "s");
        return g_fail ? 1 : 0;
    }

    // ---- (m): A FORMAT-3 FILE IS THE USER'S LIVE FILE ----------------------------------------
    // The journal on the user's disk right now is format 3 with 7 entries, and every one of those
    // entries describes an item the ENGINE's reagent map is holding. So the upgrade rule this
    // build implements - read it in full, every row at count 0, invent nothing - is not an
    // abstract compatibility case: it is what happens the first time they run this build. The
    // maker writes a genuine format-3 file (the current writer's output with the format number
    // put back and the format-4 header field removed), and the case proves the three things that
    // matter: nothing is lost, nothing is invented, and a count written afterwards lands.
    if (!strcmp(mode, "--make-v3")) {
        if (!ut::journalInit(nullptr)) {
            printf("  [FAIL] journalInit\n");
            return 1;
        }
        ut::UtReplicaCapture a, b, c;
        fill(&a, "records/items/gearhead/a01_head.dbr", 1, 3);
        fill(&b, "records/items/gearweapons/b02_sword.dbr", 1, 1);
        fill(&c, "records/items/gearhands/c03_gloves.dbr", 2, 2);
        check(ut::journalUpsert(a) && ut::journalUpsert(b) && ut::journalUpsert(c),
              "three fixture entries written");
        check(ut::journalMarkStored("records/items/gearhands/c03_gloves.dbr", false),
              "one of them is marked NOT stored (the history a refund leaves behind)");
        ut::journalService();
        std::string text;
        check(slurp(ut::journalPath(), &text), "the fixture file was written");
        size_t at = text.find("\"format\":4");
        check(at != std::string::npos, "this build wrote format 4");
        if (at != std::string::npos) text.replace(at, 10, "\"format\":3");
        at = text.find(",\"tableCopies\":");
        check(at != std::string::npos, "and a tableCopies field to remove");
        if (at != std::string::npos) {
            const size_t end = text.find_first_of(",}", at + 1);
            if (end != std::string::npos) text.erase(at, end - at);
        }
        check(text.find("\"count\":") == std::string::npos,
              "there is no count key to remove (a map-owned deposit writes none)");
        check(spit(ut::journalPath(), text), "a genuine format-3 fixture was written");
        ut::logFlush();
        printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail,
               g_fail == 1 ? "" : "s");
        return g_fail ? 1 : 0;
    }

    if (!strcmp(mode, "--case-v3upgrade")) {
        if (!ut::journalInit(nullptr)) {
            printf("  [FAIL] journalInit\n");
            return 1;
        }
        std::string before;
        check(slurp(ut::journalPath(), &before), "the format-3 fixture is in place");
        check(before.find("\"format\":3") != std::string::npos, "and it really says format 3");
        const size_t n = ut::journalCount();
        printf("  entries read out of the format-3 file: %zu\n", n);
        check(n == 3, "a format-3 file is READ in full, never rejected");
        check(!ut::journalReadOnly(), "an OLDER format does not make the journal read-only");
        // THE INVARIANT OF THE UPGRADE: not one copy was invented.
        const char* kA = "records/items/gearhead/a01_head.dbr";
        const char* kC = "records/items/gearhands/c03_gloves.dbr";
        unsigned int count = 99;
        int stored = -9;
        unsigned int stack = 0;
        check(ut::journalTableRow(kA, &count, &stored, &stack), "the stored row is there");
        check(count == 0,
              "and its count is 0 - a \"stored\":true row of a format-3 file is MAP-owned, so the "
              "private table holds NO copy of it (a 1 here would "
              "double-count against held = tableCount + mapHeld and let rescue=1 hand the item "
              "back twice)");
        check(stored == ut::UT_STORED_YES, "its stored mark is untouched");
        check(ut::journalTableRow(kC, &count, &stored, &stack) && count == 0 &&
                  stored == ut::UT_STORED_NO,
              "the not-stored row is untouched too, and also at count 0");
        check(ut::journalCollectedTotal() == 0, "the whole table holds 0 copies after the read");
        check(ut::journalCollectStored(nullptr, nullptr, 0) == 0, "and 0 rows");
        // Now put a copy INTO the table and prove it lands, in memory and on the disk.
        check(ut::journalSetCount(kA, 1), "a count of 1 is written onto the stored row");
        check(ut::journalCollectedTotal() == 1, "the table holds one copy now");
        check(ut::journalFlushNow(), "journalFlushNow() wrote the file on THIS thread");
        std::string after;
        check(slurp(ut::journalPath(), &after), "the upgraded journal reads back");
        check(after.find("\"format\":4") != std::string::npos, "the file is format 4 now");
        check(after.find("\"tableCopies\":1") != std::string::npos,
              "line 1 says one copy lives in the private table");
        check((size_t)countRecordLines(after) == n, "and NOT ONE LINE was lost in the upgrade");
        // Exactly ONE line carries a count key, and it is that row's.
        size_t counts = 0, from = 0;
        while ((from = after.find("\"count\":", from)) != std::string::npos) {
            ++counts;
            from += 8;
        }
        check(counts == 1, "exactly one entry line carries a \"count\" key (absent = 0)");
        const size_t lineA = after.find(kA);
        const size_t keyAt = after.find("\"count\":1");
        check(lineA != std::string::npos && keyAt != std::string::npos &&
                  after.rfind('\n', keyAt) == after.rfind('\n', lineA),
              "and it is on the row it was written to");
        ut::logFlush();
        printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail,
               g_fail == 1 ? "" : "s");
        return g_fail ? 1 : 0;
    }

    // ---- (k): export_csv AS A MODE -----------------------------------------------------------
    // The fixture is the shape the user's own journal has: one entry that IS stored, one that is
    // NOT (the stale history a refund left behind) and one nobody has reconciled yet. The maker
    // writes it to disk with the third entry's "stored" key REMOVED, because that absence is the
    // only way an UNKNOWN entry can exist - journalUpsert is a deposit and always marks yes.
    if (!strcmp(mode, "--make-csvmode")) {
        if (!ut::journalInit(nullptr)) {
            printf("  [FAIL] journalInit\n");
            return 1;
        }
        ut::UtReplicaCapture a, b, c;
        fill(&a, "records/items/gearhead/a01_head.dbr", 1, 3);
        fill(&b, "records/items/gearhands/c03_gloves.dbr", 1, 1);
        fill(&c, "records/items/gearfeet/d04_boots.dbr", 1, 2);
        check(ut::journalUpsert(a) && ut::journalUpsert(b) && ut::journalUpsert(c),
              "three fixture entries written");
        check(ut::journalMarkStored("records/items/gearhead/a01_head.dbr", true),
              "one is marked stored");
        check(ut::journalMarkStored("records/items/gearhands/c03_gloves.dbr", false),
              "one is marked NOT stored");
        ut::journalService();
        std::string text;
        check(slurp(ut::journalPath(), &text), "the fixture file was written");
        const size_t line = text.find("{\"record\":\"records/items/gearfeet/d04_boots.dbr\"");
        check(line != std::string::npos, "the third entry's line is there");
        if (line != std::string::npos) {
            const size_t at = text.find(",\"stored\":true", line);
            check(at != std::string::npos && at < text.find('\n', line),
                  "and it carries a stored key to remove");
            if (at != std::string::npos) text.erase(at, strlen(",\"stored\":true"));
        }
        // Keep line 1 honest: the guard reads those counts and a lying header is case (j).
        const size_t hdr = text.find("\"stored\":2,\"notStored\":1,\"unknown\":0");
        check(hdr != std::string::npos, "the header carries the pre-edit counts");
        if (hdr != std::string::npos) {
            text.replace(hdr, strlen("\"stored\":2,\"notStored\":1,\"unknown\":0"),
                         "\"stored\":1,\"notStored\":1,\"unknown\":1");
        }
        check(spit(ut::journalPath(), text), "the yes / no / unknown fixture was written");
        ut::logFlush();
        printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail,
               g_fail == 1 ? "" : "s");
        return g_fail ? 1 : 0;
    }

    if (!strcmp(mode, "--case-csvmode")) {
        if (!ut::journalInit(nullptr)) {
            printf("  [FAIL] journalInit\n");
            return 1;
        }
        const char* kYes = "records/items/gearhead/a01_head.dbr";
        const char* kNo = "records/items/gearhands/c03_gloves.dbr";
        const char* kUnknown = "records/items/gearfeet/d04_boots.dbr";
        size_t nEntries = 0, nYes = 0, nNo = 0, nUnknown = 0;
        ut::journalCounts(&nEntries, &nYes, &nNo, &nUnknown);
        check(nEntries == 3 && nYes == 1 && nNo == 1 && nUnknown == 1,
              "the fixture reads back as one stored, one not stored and one not yet reconciled");

        // ---- mode 2: EVERY entry, and the not-stored one comes LAST -------------------------
        // Setting the mode is also what arms the write: nothing else here makes the journal
        // dirty, so a CSV appearing at all proves the re-arm on a mode change.
        ut::journalSetCsvExport(2);
        long writes = ut::journalCsvWrites();
        long jwrites = ut::journalWrites();
        ut::journalService();
        check(ut::journalCsvWrites() == writes + 1, "changing the mode re-armed the export");
        // And it armed the CSV **only**. An export preference
        // must never rewrite uniq-items.jsonl: at start-up with export_csv=2 in the ini the old
        // g_dirty arm rewrote the user's most important file on every single launch.
        check(ut::journalWrites() == jwrites,
              "and it did NOT rewrite the journal - a mode change is a CSV-only pass");
        std::string all;
        check(slurp(ut::journalCsvPath(), &all), "the export_csv=2 file reads back");
        std::vector<std::vector<std::string> > rows;
        check(csvParse(all, &rows), "it parses as RFC-4180");
        check(rows.size() == 4, "export_csv=2 writes the header and ALL THREE entries");
        if (rows.size() == 4) {
            check(rows[0].size() == 14 &&
                      rows[0][0] == "record" && rows[0][2] == "stored" &&
                      rows[0][13] == "augment",
                  "the header is the SAME 14 columns, count among them");
            check(rows[3][0] == kNo && rows[3][2] == "no",
                  "the not-stored entry is written LAST (stored-first ordering)");
            bool sawYes = false, sawUnknown = false;
            for (size_t r = 1; r <= 2; ++r) {
                if (rows[r][0] == kYes) sawYes = rows[r][2] == "yes";
                if (rows[r][0] == kUnknown) sawUnknown = rows[r][2] == "unknown";
            }
            check(sawYes, "the stored entry is in the first block and says yes");
            check(sawUnknown, "the unreconciled entry is in the first block and says unknown");
        }

        // ---- mode 1: THE COLLECTION - yes and unknown, and NOTHING else ---------------------
        ut::journalSetCsvExport(1);
        writes = ut::journalCsvWrites();
        jwrites = ut::journalWrites();
        ut::journalService();
        check(ut::journalCsvWrites() == writes + 1, "the mode change re-armed the export again");
        check(ut::journalWrites() == jwrites, "and again without touching the journal file");
        std::string coll;
        check(slurp(ut::journalCsvPath(), &coll), "the export_csv=1 file reads back");
        rows.clear();
        check(csvParse(coll, &rows), "it parses as RFC-4180 too");
        check(rows.size() == 3, "export_csv=1 writes the header and TWO rows - THE USER'S BUG");
        bool collYes = false, collUnknown = false, collNo = false;
        for (size_t r = 1; r < rows.size(); ++r) {
            if (rows[r][0] == kYes) collYes = true;
            if (rows[r][0] == kUnknown) collUnknown = true;
            if (rows[r][0] == kNo) collNo = true;
            check(rows[r][2] != "no", "no row in the collection export says stored=no");
        }
        check(collYes, "the stored entry is in the collection export");
        check(collUnknown,
              "AND SO IS THE UNRECONCILED ONE - unknown means 'may still be stored' and must "
              "never be silently dropped");
        check(!collNo, "the not-stored history row is the only thing left out");
        check(coll.size() < all.size() && all.compare(0, coll.size(), coll) == 0,
              "the collection file is byte-for-byte the full file with its tail cut off");

        // ---- mode 0: no write at all, and the file already there is LEFT there --------------
        ut::journalSetCsvExport(0);
        writes = ut::journalCsvWrites();
        check(ut::journalMarkStored(kYes, true), "something makes the journal dirty");
        ut::journalService();
        check(ut::journalCsvWrites() == writes, "export_csv=0 writes no CSV at all");
        std::string after;
        check(slurp(ut::journalCsvPath(), &after), "the existing file is still on the disk");
        check(after == coll, "and it is untouched - nothing deletes or truncates it");

        // ---- out of range clamps outwards: high = everything, low = off ---------------------
        ut::journalSetCsvExport(7);
        writes = ut::journalCsvWrites();
        ut::journalService();
        check(ut::journalCsvWrites() == writes + 1, "export_csv=7 is a mode change that writes");
        std::string clamped;
        check(slurp(ut::journalCsvPath(), &clamped), "the clamped file reads back");
        rows.clear();
        check(csvParse(clamped, &rows), "and parses");
        // Not a byte compare with `all`: the mode-0 step above stamped lastSeen on the stored
        // entry, so that one column has legitimately moved on. What must hold is the SHAPE.
        check(rows.size() == 4, "export_csv=7 exports EVERYTHING, exactly like 2");
        if (rows.size() == 4) {
            check(rows[3][0] == kNo && rows[3][2] == "no",
                  "including the not-stored row, still last");
        }
        ut::journalSetCsvExport(-3);
        writes = ut::journalCsvWrites();
        check(ut::journalMarkStored(kYes, true), "something makes the journal dirty again");
        ut::journalService();
        check(ut::journalCsvWrites() == writes, "export_csv=-3 is off, exactly like 0");
        ut::journalSetCsvExport(1);
        ut::logFlush();
        printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail,
               g_fail == 1 ? "" : "s");
        return g_fail ? 1 : 0;
    }

    // Case (l): the FRESH INSTALL. No fixture at all - the folder holds
    // no journal, exactly like a machine on which nothing has ever been deposited. Two claims are
    // under test here, both of them promises the USER-GUIDE makes to a reader who will test them
    // on precisely this machine state:
    //   * changing export_csv writes the CSV and NOTHING ELSE - no uniq-items.jsonl is created
    //     (the old code armed g_dirty and had to be guarded against exactly that);
    //   * an empty collection at export_csv=1 really does produce the header line on its own.
    if (!strcmp(mode, "--case-csvempty")) {
        if (!ut::journalInit(nullptr)) {
            printf("  [FAIL] journalInit\n");
            return 1;
        }
        size_t nEntries = 99;
        ut::journalCounts(&nEntries, nullptr, nullptr, nullptr);
        check(nEntries == 0, "the fixture-less folder reads back as an empty journal");
        std::string none;
        check(!slurp(ut::journalCsvPath(), &none), "and there is no CSV there yet either");

        ut::journalSetCsvExport(2);          // a change: 1 (the default latch) -> 2
        const long jwrites = ut::journalWrites();
        const long writes = ut::journalCsvWrites();
        ut::journalService();
        check(ut::journalWrites() == jwrites,
              "a mode change on an EMPTY journal creates no uniq-items.jsonl - THE POINT OF THE "
              "CSV-ONLY PASS");
        check(ut::journalCsvWrites() == writes + 1, "but it does write the CSV");
        std::string head;
        check(slurp(ut::journalCsvPath(), &head), "the CSV is on the disk");
        std::vector<std::vector<std::string> > rows;
        check(csvParse(head, &rows), "it parses as RFC-4180");
        check(rows.size() == 1 && rows[0].size() == 14 && rows[0][0] == "record",
              "an empty collection is the 14-column header line and nothing else - the honest "
              "answer the guide promises");

        ut::journalSetCsvExport(1);
        ut::journalService();
        std::string again;
        check(slurp(ut::journalCsvPath(), &again), "mode 1 writes it too");
        check(again == head, "and an empty collection looks the same in either mode");
        check(ut::journalWrites() == jwrites, "still no journal file anywhere in this case");
        ut::logFlush();
        printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail,
               g_fail == 1 ? "" : "s");
        return g_fail ? 1 : 0;
    }

    // Not a case - a DIAGNOSTIC. Reads whatever journal is in %UNIQUETAB_OUT%,
    // exports it at the mode in argv[2] and prints the row count and the first few records. It is
    // how you can point the REAL writer at a copy of a real journal and see what the user will
    // see, without a game and without touching the original. It writes only inside that folder.
    if (!strcmp(mode, "--dump-csv")) {
        if (!ut::journalInit(nullptr)) {
            printf("  [FAIL] journalInit\n");
            return 1;
        }
        const int want = arg ? atoi(arg) : 1;
        size_t nEntries = 0, nYes = 0, nNo = 0, nUnknown = 0;
        ut::journalCounts(&nEntries, &nYes, &nNo, &nUnknown);
        printf("  journal: %zu entries - %zu stored, %zu not stored, %zu not yet reconciled\n",
               nEntries, nYes, nNo, nUnknown);
        ut::journalSetCsvExport(0);  // so the next call is always a CHANGE, i.e. always re-arms
        ut::journalSetCsvExport(want);
        ut::journalService();
        std::string csv;
        if (!slurp(ut::journalCsvPath(), &csv)) {
            printf("  export_csv=%d wrote NO file\n", want);
            ut::logFlush();
            return 0;
        }
        std::vector<std::vector<std::string> > rows;
        if (!csvParse(csv, &rows)) {
            printf("  [FAIL] the file does not parse\n");
            return 1;
        }
        printf("  export_csv=%d -> %zu data row%s (%zu bytes)\n", want,
               rows.size() ? rows.size() - 1 : 0, rows.size() == 2 ? "" : "s", csv.size());
        for (size_t r = 1; r < rows.size() && r <= 12; ++r) {
            printf("    %-52s %-8s %s\n", rows[r][0].c_str(), rows[r][2].c_str(),
                   rows[r][1].c_str());
        }
        ut::logFlush();
        return 0;
    }

    // THE FRESH INSTALL, for the .gds. No fixture at all - a machine on which
    // nothing has ever been deposited. Two claims, both of them promises the USER-GUIDE makes
    // about exactly this machine state: changing export_gds writes the export and NOTHING else
    // (no uniq-items.jsonl is created), and an empty collection really is the 8-byte header on
    // its own - version 3, count 0 - which is a file GD Stash reads as "no items", not a
    // truncated one.
    if (!strcmp(mode, "--case-gdsempty")) {
        if (!ut::journalInit(nullptr)) {
            printf("  [FAIL] journalInit\n");
            return 1;
        }
        size_t nEntries = 99;
        ut::journalCounts(&nEntries, nullptr, nullptr, nullptr);
        check(nEntries == 0, "the fixture-less folder reads back as an empty journal");
        std::string none;
        check(!slurp(ut::journalGdsPath(), &none), "and there is no .gds there yet either");

        ut::journalSetGdsExport(2);   // a change: 1 (the default latch) -> 2
        const long jwrites = ut::journalWrites();
        const long writes = ut::journalGdsWrites();
        ut::journalService();
        check(ut::journalWrites() == jwrites,
              "a mode change on an EMPTY journal creates no uniq-items.jsonl - THE POINT OF THE "
              "EXPORT-ONLY PASS");
        check(ut::journalGdsWrites() == writes + 1, "but it does write the .gds");
        std::string head;
        check(slurp(ut::journalGdsPath(), &head), "the .gds is on the disk");
        check(head.size() == 8, "an empty collection is the 8-byte header and nothing else");
        std::vector<GdsItem> items;
        std::string why;
        check(gdsParse(head, &items, &why), why.empty() ? "and it parses" : why.c_str());
        check(items.empty(), "as version 3 with a count of zero - not as a truncated file");
        ut::journalSetGdsExport(1);
        ut::journalService();
        std::string again;
        check(slurp(ut::journalGdsPath(), &again), "mode 1 writes it too");
        check(again == head, "and an empty collection looks the same in either mode");
        check(ut::journalWrites() == jwrites, "still no journal file anywhere in this case");
        ut::logFlush();
        printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail,
               g_fail == 1 ? "" : "s");
        return g_fail ? 1 : 0;
    }

    // ==== THE GD STASH IMPORT FILE ==============================================================
    //
    // The strongest proof available without running GD Stash: a journal holding EXACTLY the item
    // GD Stash itself exported, run through the mod's own writer, must come out
    // BYTE FOR BYTE the same 88 bytes. Every field, every length byte, the header ints and the
    // two trailing fields the journal cannot fill are all pinned by that one comparison.
    //
    // kExampleGds below is that file, embedded so the case cannot be skipped by a missing path;
    // when argv[2] names the real file the two are ALSO compared, so the array can never drift
    // away from the artefact it claims to be.
    //
    //   one item exported by GD Stash 1.90b (88 bytes):
    //   version 3, 1 item, records/items/gearfeet/c201_feet.dbr, seed 1048302352 (0x3E7BD310),
    //   no affixes, no component, no augment, stackCount 1, hardcore false, charname null.
    if (!strcmp(mode, "--case-gds") || !strcmp(mode, "--dump-gds")) {
        static const unsigned char kExampleGds[] = {
            0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x24, 0x72, 0x65, 0x63,
            0x6F, 0x72, 0x64, 0x73, 0x2F, 0x69, 0x74, 0x65, 0x6D, 0x73, 0x2F, 0x67,
            0x65, 0x61, 0x72, 0x66, 0x65, 0x65, 0x74, 0x2F, 0x63, 0x32, 0x30, 0x31,
            0x5F, 0x66, 0x65, 0x65, 0x74, 0x2E, 0x64, 0x62, 0x72, 0x00, 0x00, 0x00,
            0x00, 0x10, 0xD3, 0x7B, 0x3E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00,
        };
        const char* kExampleRecord = "records/items/gearfeet/c201_feet.dbr";
        const unsigned int kExampleSeed = 0x3E7BD310u;

        if (!ut::journalInit(nullptr)) {
            printf("  [FAIL] journalInit\n");
            return 1;
        }

        // ---- the DIAGNOSTIC half: --dump-gds <mode> on whatever journal is in the folder -------
        // Not a case. It points the REAL writer at a copy of a real journal and prints what the
        // user will hand to GD Stash, without a game and without touching the original.
        if (!strcmp(mode, "--dump-gds")) {
            const int want = arg ? atoi(arg) : 1;
            size_t nEntries = 0, nYes = 0, nNo = 0, nUnknown = 0;
            ut::journalCounts(&nEntries, &nYes, &nNo, &nUnknown);
            printf("  journal: %zu entries - %zu stored, %zu not stored, %zu not yet reconciled\n",
                   nEntries, nYes, nNo, nUnknown);
            ut::journalSetGdsExport(0);  // so the next call is always a CHANGE, i.e. always re-arms
            ut::journalSetGdsExport(want);
            ut::journalService();
            std::string bytes;
            if (!slurp(ut::journalGdsPath(), &bytes)) {
                printf("  export_gds=%d wrote NO file\n", want);
                ut::logFlush();
                return 0;
            }
            std::vector<GdsItem> items;
            std::string why;
            if (!gdsParse(bytes, &items, &why)) {
                printf("  [FAIL] the file does not parse: %s\n", why.c_str());
                return 1;
            }
            printf("  export_gds=%d -> %zu item%s (%zu bytes), %s\n", want, items.size(),
                   items.size() == 1 ? "" : "s", bytes.size(), ut::journalGdsPath());
            for (size_t i = 0; i < items.size() && i < 12; ++i) gdsPrint(items[i], i);
            ut::logFlush();
            return 0;
        }

        // ---- (1) BYTE-IDENTICAL to the file GD Stash wrote -------------------------------------
        {
            ut::UtReplicaCapture e;
            memset(&e, 0, sizeof(e));
            _snprintf_s(e.record, sizeof(e.record), _TRUNCATE, "%s", kExampleRecord);
            e.stack = 1;
            e.flags = 0;
            e.replicaLen = 0x190;
            memcpy(&e.replica[0x068], &kExampleSeed, 4);   // the seed, where WriteProperties has it
            e.slotCount = 1;
            e.slotOff[0] = 0x008;                          // baseRecord, and nothing else is set
            _snprintf_s(e.slotText[0], sizeof(e.slotText[0]), _TRUNCATE, "%s", kExampleRecord);
            check(ut::journalUpsert(e), "the example item is journalled");
            ut::journalService();
            check(ut::journalGdsWrites() >= 1, "the .gds was written with the journal");
            std::string got;
            check(slurp(ut::journalGdsPath(), &got), "uniq-export.gds reads back off the disk");
            check(got.size() == sizeof(kExampleGds),
                  "it is exactly as long as GD Stash's own export (88 bytes)");
            const bool same = got.size() == sizeof(kExampleGds) &&
                              memcmp(got.data(), kExampleGds, sizeof(kExampleGds)) == 0;
            check(same,
                  "*** BYTE FOR BYTE IDENTICAL to GD Stash's own one-item export\n"
                  "                    - the format is right ***");
            if (!same) {
                const size_t n = got.size() < sizeof(kExampleGds) ? got.size() : sizeof(kExampleGds);
                for (size_t i = 0; i < n; ++i) {
                    if ((unsigned char)got[i] != kExampleGds[i]) {
                        printf("      first difference at offset 0x%02X: wrote 0x%02X, GD Stash "
                               "wrote 0x%02X\n",
                               (unsigned)i, (unsigned char)got[i], kExampleGds[i]);
                        break;
                    }
                }
            }
            // ... and the embedded array is the artefact, not a copy that has drifted from it.
            if (arg && arg[0]) {
                std::string onDisk;
                if (slurp(arg, &onDisk)) {
                    check(onDisk.size() == sizeof(kExampleGds) &&
                              memcmp(onDisk.data(), kExampleGds, sizeof(kExampleGds)) == 0,
                          "the embedded 88 bytes ARE the file GD Stash exported (compared against "
                          "the artefact on disk)");
                } else {
                    printf("      [note] %s is not here - the embedded copy was not cross-checked\n",
                           arg);
                }
            }
        }

        // ---- (2) a MULTI-ITEM file, parsed back by a reader that knows nothing about the writer -
        // The rich entry fills every string slot and every u32 the mapping claims, each with a
        // distinct value, so a field that went in the wrong hole cannot come out looking right.
        {
            ut::UtReplicaCapture rich, plain;
            memset(&rich, 0, sizeof(rich));
            _snprintf_s(rich.record, sizeof(rich.record), _TRUNCATE, "%s",
                        "records/items/gearweapon/swords/d201_sword.dbr");
            rich.stack = 1;
            rich.replicaLen = 0x190;
            const unsigned int kSeed = 0x11111111u, kRelicSeed = 0x22222222u;
            const unsigned int kEnchLevel = 0x33333333u, kEnchSeed = 0x44444444u;
            const unsigned int kVar1 = 0x55555555u, kRerolls = 0x66666666u;
            const unsigned int kAffixRerolls = 0x77777777u;
            memcpy(&rich.replica[0x068], &kSeed, 4);
            memcpy(&rich.replica[0x0D0], &kRelicSeed, 4);
            memcpy(&rich.replica[0x0F8], &kEnchLevel, 4);
            memcpy(&rich.replica[0x0FC], &kEnchSeed, 4);
            memcpy(&rich.replica[0x160], &kVar1, 4);
            memcpy(&rich.replica[0x180], &kRerolls, 4);
            memcpy(&rich.replica[0x17C], &kAffixRerolls, 4);
            struct SlotSeed {
                unsigned int off;
                const char* text;
            };
            static const SlotSeed kSlots[] = {
                {0x008, "records/items/gearweapon/swords/d201_sword.dbr"},
                {0x028, "records/items/lootaffixes/prefix/x_prefix.dbr"},
                {0x048, "records/items/lootaffixes/suffix/x_suffix.dbr"},
                {0x070, "records/items/lootaffixes/crafting/x_modifier.dbr"},
                {0x090, "records/items/materia/compa_markofthetraveler.dbr"},
                {0x0B0, "records/items/lootaffixes/completionrelics/x_bonus.dbr"},
                {0x0D8, "records/items/enchants/x_augment.dbr"},
                {0x100, "records/items/gearweapon/swords/z999_illusion.dbr"},
                {0x120, "records/items/lootaffixes/ascendant/x_asc.dbr"},
                {0x140, "records/items/lootaffixes/ascendant/x_asc2h.dbr"},
            };
            rich.slotCount = (int)(sizeof(kSlots) / sizeof(kSlots[0]));
            for (int i = 0; i < rich.slotCount; ++i) {
                rich.slotOff[i] = kSlots[i].off;
                _snprintf_s(rich.slotText[i], sizeof(rich.slotText[i]), _TRUNCATE, "%s",
                            kSlots[i].text);
            }
            memset(&plain, 0, sizeof(plain));
            _snprintf_s(plain.record, sizeof(plain.record), _TRUNCATE, "%s",
                        "records/items/gearhands/c03_gloves.dbr");
            plain.stack = 1;
            plain.replicaLen = 0x190;
            plain.slotCount = 1;
            plain.slotOff[0] = 0x008;
            _snprintf_s(plain.slotText[0], sizeof(plain.slotText[0]), _TRUNCATE, "%s",
                        plain.record);
            check(ut::journalUpsert(rich) && ut::journalUpsert(plain), "two more entries");
            check(ut::journalMarkStored("records/items/gearhands/c03_gloves.dbr", false),
                  "and one of them is marked NOT stored");
            ut::journalService();

            std::string bytes;
            std::vector<GdsItem> items;
            std::string why;
            check(slurp(ut::journalGdsPath(), &bytes), "mode 1 rewrote the file");
            check(gdsParse(bytes, &items, &why), why.empty() ? "it parses" : why.c_str());
            check(items.size() == 2,
                  "export_gds=1 exports THE COLLECTION - the two stored entries, not the third");
            const GdsItem* r = nullptr;
            for (size_t i = 0; i < items.size(); ++i) {
                if (items[i].s[0] == "records/items/gearweapon/swords/d201_sword.dbr") {
                    r = &items[i];
                }
            }
            check(r != nullptr, "the rich entry is in the file");
            if (r) {
                check(r->s[1] == kSlots[1].text, "prefixID   <- slot +0x028");
                check(r->s[2] == kSlots[2].text, "suffixID   <- slot +0x048");
                check(r->s[3] == kSlots[3].text, "modifierID <- slot +0x070");
                check(r->s[4] == kSlots[7].text, "transmuteID<- slot +0x100 (the illusion)");
                check(r->s[5] == kSlots[4].text, "relicID    <- slot +0x090 (the component)");
                check(r->s[6] == kSlots[5].text, "relicBonusID <- slot +0x0B0");
                check(r->s[7] == kSlots[6].text, "enchantmentID <- slot +0x0D8 (the augment)");
                check(r->s[8] == kSlots[8].text, "ascendantID   <- slot +0x120");
                check(r->s[9] == kSlots[9].text, "ascendant2hID <- slot +0x140");
                check(r->i[0] == (int)kSeed, "seed             <- u32 +0x068");
                check(r->i[1] == (int)kRelicSeed, "relicSeed        <- u32 +0x0D0");
                check(r->i[2] == (int)kEnchLevel, "enchantmentLevel <- u32 +0x0F8");
                check(r->i[3] == (int)kEnchSeed, "enchantmentSeed  <- u32 +0x0FC");
                check(r->i[4] == (int)kVar1, "var1             <- u32 +0x160");
                check(r->i[5] == 1, "stackCount       <- the journal's own stack");
                check(r->i[6] == (int)kRerolls, "rerollsUsed      <- u32 +0x180");
                check(r->i[7] == (int)kAffixRerolls, "affixRerollsUsed <- u32 +0x17C");
                check(!r->hardcore, "hardcore is 0 - the mod has no hardcore character");
                check(r->s[10].empty(), "charname is the null string");
            }

            // ---- (3) mode 2 adds the history, and the not-stored row comes LAST ----------------
            ut::journalSetGdsExport(2);
            const long before = ut::journalGdsWrites();
            ut::journalService();
            check(ut::journalGdsWrites() == before + 1,
                  "changing the mode re-armed the export on its own");
            items.clear();
            check(slurp(ut::journalGdsPath(), &bytes), "the export_gds=2 file reads back");
            check(gdsParse(bytes, &items, &why), why.empty() ? "and parses" : why.c_str());
            check(items.size() == 3, "export_gds=2 exports every entry");
            if (items.size() == 3) {
                check(items[2].s[0] == "records/items/gearhands/c03_gloves.dbr",
                      "and the not-stored one is LAST, exactly like the CSV's second block");
            }

            // ---- (4) mode 0 writes nothing and DELETES nothing ---------------------------------
            ut::journalSetGdsExport(0);
            const long off = ut::journalGdsWrites();
            check(ut::journalUpsert(plain), "another upsert to make the journal dirty");
            ut::journalService();
            check(ut::journalGdsWrites() == off, "export_gds=0 writes no .gds at all");
            std::string still;
            check(slurp(ut::journalGdsPath(), &still), "and the file that was there is LEFT there");
            check(still == bytes, "byte for byte - the mod never deletes or truncates an export");

            // ---- (5) out of range clamps OUTWARDS ----------------------------------------------
            ut::journalSetGdsExport(7);
            ut::journalService();
            items.clear();
            check(slurp(ut::journalGdsPath(), &bytes), "export_gds=7 wrote a file");
            check(gdsParse(bytes, &items, &why), why.empty() ? "which parses" : why.c_str());
            check(items.size() == 3, "and it behaves as mode 2 - a typo exports MORE, never less");
            ut::journalSetGdsExport(-3);
            const long neg = ut::journalGdsWrites();
            check(ut::journalUpsert(plain), "one more upsert");
            ut::journalService();
            check(ut::journalGdsWrites() == neg, "and export_gds=-3 behaves as 0");
        }

        ut::logFlush();
        printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail,
               g_fail == 1 ? "" : "s");
        return g_fail ? 1 : 0;
    }
    // Not a case - a DIAGNOSTIC, and the offline proof that the private table answers for a
    // record the display path asks about.
    // Reads whatever journal is in %UNIQUETAB_OUT% and asks `journalStoreRow` - the exact call
    // `storeDisplayProtoId` makes - for each record named on the command line (or for every
    // record in the journal plus one BOX record when none is named). It prints the three-state
    // answer, so "does the private table's lookup say YES for Flamebreaker?" is answerable
    // without a game. Read-only: it never writes the journal or the CSV.
    if (!strcmp(mode, "--store-row")) {
        if (!ut::journalInit(nullptr)) {
            printf("  [FAIL] journalInit\n");
            return 1;
        }
        size_t nEntries = 0, nYes = 0, nNo = 0, nUnknown = 0;
        ut::journalCounts(&nEntries, &nYes, &nNo, &nUnknown);
        printf("  journal: %zu entries - %zu stored, %zu not stored, %zu not yet reconciled\n",
               nEntries, nYes, nNo, nUnknown);
        std::vector<std::string> want;
        if (arg && *arg) {
            want.push_back(arg);
        } else {
            static char recs[4096][256];
            const int n = ut::journalRecords(recs, 4096);
            for (int i = 0; i < n; ++i) want.push_back(recs[i]);
            // ... and a string the display path really passes: a BOX record from uniq-groups.txt.
            want.push_back("records/ui/caravan/reagents/uniq/p55/box_02.dbr");
            // ... and the same item record with the case and the separators mangled, which is
            // what `findEntry`'s _stricmp does and does not forgive.
            want.push_back("RECORDS/ITEMS/GEARWEAPONS/SHIELDS/C021_SHIELD.DBR");
            want.push_back("records\\items\\gearweapons\\shields\\c021_shield.dbr");
        }
        for (size_t i = 0; i < want.size(); ++i) {
            int stored = -2;
            unsigned int stack = 0;
            const bool got = ut::journalStoreRow(want[i].c_str(), &stored, &stack);
            const char* s = stored == ut::UT_STORED_YES       ? "YES"
                            : stored == ut::UT_STORED_NO      ? "no"
                            : stored == ut::UT_STORED_UNKNOWN ? "unknown"
                                                              : "?";
            printf("    %-52s entry=%s stored=%-7s stack=%u  -> private table %s\n",
                   want[i].c_str(), got ? "yes" : "NO ", s, stack,
                   (got && stored == ut::UT_STORED_YES) ? "PAINTS it" : "bows out");
        }
        ut::logFlush();
        return 0;
    }

    if (!strcmp(mode, "--case-reconcile")) {
        if (!ut::journalInit(nullptr)) {
            printf("  [FAIL] journalInit\n");
            return 1;
        }
        static char recs[4096][256];
        const int n = ut::journalRecords(recs, 4096);
        check(n >= 4, "the fixture has enough entries to reconcile");
        // The "page": the first two records are still in the collection, the rest are not.
        std::string kept[2];
        bool allFound = true;
        for (int i = 0; i < n; ++i) {
            const bool onPage = (i < 2);
            if (onPage) kept[i] = recs[i];
            if (!ut::journalMarkStored(recs[i], onPage)) allFound = false;
        }
        check(allFound, "journalMarkStored finds every entry it is given");
        check(!ut::journalMarkStored("records/items/never/deposited.dbr", false),
              "and refuses a record that is not in the journal");
        size_t total = 0, stored = 0, notStored = 0, unknown = 0;
        ut::journalCounts(&total, &stored, &notStored, &unknown);
        check(total == (size_t)n, "reconciling adds and removes NOTHING");
        check(stored == 2, "exactly the two records the page holds are marked stored");
        check(notStored == (size_t)n - 2, "and the rest are marked not stored");
        check(unknown == 0, "nothing is left unmarked when the walk succeeded");
        ut::journalService();
        std::string text;
        check(slurp(ut::journalPath(), &text), "the reconciled journal reads back");
        char want[96];
        _snprintf_s(want, sizeof(want), _TRUNCATE, "\"entries\":%d,\"stored\":2,\"notStored\":%d",
                    n, n - 2);
        check(text.find(want) != std::string::npos, "line 1 carries all three counts");
        check((size_t)countRecordLines(text) == (size_t)n, "every entry is still in the file");
        // The two stored ones are written FIRST, so the top of the file is the collection.
        const size_t firstNot = text.find("\"stored\":false");
        const size_t lastYes = text.rfind("\"stored\":true");
        check(firstNot != std::string::npos && lastYes != std::string::npos && lastYes < firstNot,
              "not-stored entries are written after the stored ones");
        for (int i = 0; i < 2; ++i) {
            char q[320];
            _snprintf_s(q, sizeof(q), _TRUNCATE, "\"record\":\"%s\"", kept[i].c_str());
            check(text.find(q) != std::string::npos, "a stored record is still greppable");
        }
        // ---- the prune: OFF by default, copies aside, drops only what is marked not stored ----
        static char dropped[4096][256];
        char why[512] = {0};
        const int gone = ut::journalPruneNotStored(dropped, 4096, why, sizeof(why));
        printf("  prune: %d dropped (%s)\n", gone, why);
        check(gone == n - 2, "the prune drops exactly the not-stored entries");
        check(ut::journalCount() == 2, "and leaves the two that are stored");
        WIN32_FIND_DATAA fd;
        char pat[MAX_PATH];
        _snprintf_s(pat, sizeof(pat), _TRUNCATE, "%s\\uniq-items.jsonl.pruned-*", outDir);
        HANDLE fh = FindFirstFileA(pat, &fd);
        check(fh != INVALID_HANDLE_VALUE,
              "the whole journal was copied to uniq-items.jsonl.pruned-* BEFORE the prune");
        if (fh != INVALID_HANDLE_VALUE) {
            char asidePath[MAX_PATH];
            _snprintf_s(asidePath, sizeof(asidePath), _TRUNCATE, "%s\\%s", outDir, fd.cFileName);
            FindClose(fh);
            std::string aside;
            check(slurp(asidePath, &aside), "the copy is readable");
            check(countRecordLines(aside) == n, "and it still holds every pruned entry");
        }
        ut::journalService();
        check(slurp(ut::journalPath(), &text), "the pruned journal reads back");
        check(countRecordLines(text) == 2, "two lines left");
        ut::logFlush();
        printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail,
               g_fail == 1 ? "" : "s");
        return g_fail ? 1 : 0;
    }

    // ---- (h): a FAILED or EMPTY page read marks NOTHING --------------------------------------
    // The dangerous case, and the reason journalMarkStored is only ever called from the far side
    // of a successful walk: when the walk fails ut_reagent returns before the marking loop, so
    // not one journalMarkStored call is made. This pass is that: load the same fixture, mark
    // nothing, write, and prove every entry is still UNKNOWN - which is what keeps
    // journal_guard.ps1 refusing an uninstall.
    if (!strcmp(mode, "--case-noreconcile")) {
        if (!ut::journalInit(nullptr)) {
            printf("  [FAIL] journalInit\n");
            return 1;
        }
        const size_t n = ut::journalCount();
        check(n >= 4, "the fixture loaded");
        size_t total = 0, stored = 0, notStored = 0, unknown = 0;
        ut::journalCounts(&total, &stored, &notStored, &unknown);
        check(unknown == n && stored == 0 && notStored == 0,
              "a reconciliation that never ran leaves every entry UNKNOWN");
        // Force a write and prove the file says so too - "unknown" is what the uninstall guard
        // reads as "possibly still stored".
        ut::UtReplicaCapture e;
        fill(&e, "records/items/upgrade/throwaway.dbr", 1, 1);
        check(ut::journalUpsert(e), "a throwaway entry is accepted");
        check(ut::journalRemove("records/items/upgrade/throwaway.dbr"), "and removed again");
        ut::journalService();
        std::string text;
        check(slurp(ut::journalPath(), &text), "the journal reads back");
        char want[96];
        _snprintf_s(want, sizeof(want), _TRUNCATE, "\"stored\":0,\"notStored\":0,\"unknown\":%zu",
                    n);
        check(text.find(want) != std::string::npos,
              "line 1 says 0 stored, 0 not stored, and every entry unreconciled");
        check(text.find("\"stored\":false") == std::string::npos,
              "and NOT ONE entry was marked not stored by a reconciliation that did not happen");
        check((size_t)countRecordLines(text) == n, "no entry was lost either");
        // And the prune must find nothing to do: UNKNOWN is never dropped.
        static char dropped[64][256];
        char why[512] = {0};
        const int gone = ut::journalPruneNotStored(dropped, 64, why, sizeof(why));
        check(gone == 0, "the prune drops NOTHING when nothing is marked not stored");
        check(ut::journalCount() == n, "every entry is still there after the prune");
        ut::logFlush();
        printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail,
               g_fail == 1 ? "" : "s");
        return g_fail ? 1 : 0;
    }

    // ---- (i): every entry checked and NOT on the page any more -------------------------------
    // The user's exact state, and the one journal_guard.ps1 used to refuse on: a file full of
    // history, a page that holds nothing. The harness runs the guard against the folder this
    // leaves behind and expects exit 0.
    if (!strcmp(mode, "--case-allstale")) {
        if (!ut::journalInit(nullptr)) {
            printf("  [FAIL] journalInit\n");
            return 1;
        }
        static char recs[4096][256];
        const int n = ut::journalRecords(recs, 4096);
        check(n > 0, "the fixture loaded");
        bool allFound = true;
        for (int i = 0; i < n; ++i) {
            if (!ut::journalMarkStored(recs[i], false)) allFound = false;
        }
        check(allFound, "every entry was marked not stored");
        size_t total = 0, stored = 0, notStored = 0, unknown = 0;
        ut::journalCounts(&total, &stored, &notStored, &unknown);
        check(stored == 0 && unknown == 0 && notStored == (size_t)n,
              "0 stored, 0 unknown, everything marked not stored");
        ut::journalService();
        std::string text;
        check(slurp(ut::journalPath(), &text), "the journal reads back");
        check((size_t)countRecordLines(text) == (size_t)n,
              "and EVERY entry is still in the file - none is dropped to make the guard happy");
        ut::logFlush();
        printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail,
               g_fail == 1 ? "" : "s");
        return g_fail ? 1 : 0;
    }

    printf("  [FAIL] unknown mode %s\n", mode);
    return 1;
}

// ---- THE FLUSH RULE, offline -----------------------------------------------------------------
// A force-killed game can leave a 0-byte uniquetab.log: with
// log_flush_each_line=0 the only thing that ever drained the buffer was the worker's own loop,
// and a start-up that stops before that loop (the bindings gate returning false) never reached
// it - so every line saying WHY the mod was off died with the process. ut_log.cpp now runs its
// own flusher thread and flushes a WARN or an ERROR inline. Both halves are checked here with no
// game and no kill: this process writes, then does NOTHING for 1.5 s, then reads its own log back
// through a SECOND HANDLE - if the lines are there, only the flusher can have put them there.
static int logFlushCase() {
    char path[MAX_PATH];
    GetTempPathA(MAX_PATH, path);
    strcat_s(path, MAX_PATH, "uniq-logflush-test.log");
    DeleteFileA(path);
    wchar_t wpath[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH);

    printf("log flush rule (no game, no kill): %s\n", path);
    if (!ut::logInit(wpath)) {
        printf("  [FAIL] logInit\n");
        return 1;
    }
    ut::logSetLevel("debug");
    ut::logSetFlushEachLine(0);  // the setting that lost the whole file
    ut::logI("flush case: the line a crash report would need");
    ut::logD("flush case: a debug line behind it");

    // NOTHING else happens for 1.5 s: no further write, no logFlush, no exit, no shutdown.
    Sleep(1500);
    std::string text;
    const bool read1 = slurp(path, &text);
    check(read1 && !text.empty(), "the log is not 0 bytes while the process is still running");
    check(read1 && text.find("the line a crash report would need") != std::string::npos &&
              text.find("a debug line behind it") != std::string::npos,
          "buffered lines reach the disk within a second, with no further write and no exit");

    // A WARN does not wait for the next tick - it is on disk before the call returns.
    const size_t before = text.size();
    ut::logW("flush case: a warning is written before the call returns");
    std::string text2;
    const bool read2 = slurp(path, &text2);
    check(read2 && text2.find("a warning is written before the call returns") != std::string::npos,
          "a WARN is readable IMMEDIATELY, with no sleep at all");
    check(read2 && text2.size() > before, "and it grew the file, not just the buffer");

    ut::logShutdown();
    DeleteFileA(path);
    printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}

int main(int argc, char** argv) {
    // Unbuffered, so that when a check CRASHES the last line printed is the last line that ran.
    // Redirected output is block-buffered otherwise and the tail - the interesting part - is lost.
    setvbuf(stdout, nullptr, _IONBF, 0);
    // The log-flush case touches no journal at all, so it runs before the out-dir guard.
    if (argc > 1 && argv[1] && !strcmp(argv[1], "--case-logflush")) return logFlushCase();
    // THE BELT GOES IN FRONT. journalInit() MUTATES the folder it
    // resolves: it writes uniq-items.jsonl and RENAMES an existing uniq-items.bin aside as
    // .migrated-<stamp>. It is also the first thing every mode below does, i.e. it runs BEFORE
    // the .pretest move-aside further down could protect anything. With %UNIQUETAB_OUT% unset the
    // mod folder resolves beside this exe, so a hand-run would write a journal wherever the exe
    // happens to sit. build_test_journal.bat always sets the variable; refusing without it is
    // what makes that not merely a convention.
    {
        char envOut[MAX_PATH] = {0};
        const DWORD n = GetEnvironmentVariableA("UNIQUETAB_OUT", envOut, MAX_PATH);
        if (n == 0 || n >= MAX_PATH || !envOut[0]) {
            printf("  [FAIL] %%UNIQUETAB_OUT%% is not set. This test writes a journal and can "
                   "MIGRATE an existing uniq-items.bin, so it refuses to run anywhere but a "
                   "scratch folder the harness handed it. Run tools\\build_test_journal.bat, or "
                   "set UNIQUETAB_OUT yourself.\n");
            return 1;
        }
        printf("  out-dir: %%UNIQUETAB_OUT%% = %s\n", envOut);
    }
    // The readable-format cases run in their own processes and their own
    // %UNIQUETAB_OUT% folders. See caseMain above.
    if (argc > 1 && argv[1] && !strncmp(argv[1], "--make-", 7)) {
        return caseMain(argv[1], argc > 2 ? argv[2] : nullptr);
    }
    if (argc > 1 && argv[1] &&
        (!strcmp(argv[1], "--migrate") || !strcmp(argv[1], "--reload") ||
         !strcmp(argv[1], "--case-readonly") || !strcmp(argv[1], "--case-mangled") ||
         !strcmp(argv[1], "--case-unreadable") || !strcmp(argv[1], "--case-hostile") ||
         !strcmp(argv[1], "--case-upgrade") || !strcmp(argv[1], "--case-upgraded") ||
         !strcmp(argv[1], "--case-reconcile") || !strcmp(argv[1], "--case-noreconcile") ||
         !strcmp(argv[1], "--case-allstale") || !strcmp(argv[1], "--case-csv") ||
         !strcmp(argv[1], "--case-csvmode") || !strcmp(argv[1], "--case-csvempty") ||
         !strcmp(argv[1], "--dump-csv") || !strcmp(argv[1], "--case-gds") ||
         !strcmp(argv[1], "--case-gdsempty") || !strcmp(argv[1], "--dump-gds") ||
         !strcmp(argv[1], "--store-row") ||
         // (m): the format-3 file the user has on disk right now
         !strcmp(argv[1], "--case-v3upgrade"))) {
        return caseMain(argv[1], argc > 2 ? argv[2] : nullptr);
    }
    const bool verifyOnly = (argc > 1 && argv[1] && !strcmp(argv[1], "--verify"));
    wchar_t log[MAX_PATH];
    GetTempPathW(MAX_PATH, log);
    wcscat_s(log, MAX_PATH, L"uniq-journal-test.log");
    ut::logInit(log);

    printf(verifyOnly ? "journal identity re-read (pass 2, straight off the file)\n"
                      : "journal round-trip test (pass 1)\n");
    if (!ut::journalInit(nullptr)) {
        printf("  [FAIL] journalInit\n");
        return 1;
    }
    const char* path = ut::journalPath();
    printf("  journal path: %s\n", path);

    if (verifyOnly) {
        // Nothing is written in this pass: everything below comes out of the file pass 1 left.
        check(ut::journalCount() >= 1, "the file survived and was parsed");
        check(ut::journalHas(kIdRecord), "the identity record is in the file");
        identityChecks("after a write + a process restart");
        // Clean up after ourselves wherever the out-dir helper put the file, and put back any
        // real journal pass 1 moved aside. The test must leave the tree exactly as it found it.
        char keep2[MAX_PATH];
        _snprintf_s(keep2, sizeof(keep2), _TRUNCATE, "%s.pretest", path);
        DeleteFileA(path);
        if (GetFileAttributesA(keep2) != INVALID_FILE_ATTRIBUTES) {
            check(MoveFileExA(keep2, path, MOVEFILE_REPLACE_EXISTING) != 0,
                  "the pre-existing journal was put back");
        } else {
            printf("  removed the test journal at %s\n", path);
        }
        printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail,
               g_fail == 1 ? "" : "s");
        ut::logFlush();
        return g_fail ? 1 : 0;
    }

    // The journal path is resolved by the mod folder resolver, i.e. potentially on top of a real
    // collection. Never destroy one: move it aside for the length of the test and put it back in
    // pass 2.
    char keep[MAX_PATH];
    _snprintf_s(keep, sizeof(keep), _TRUNCATE, "%s.pretest", path);
    // NEVER overwrite an existing .pretest. It means a previous pass 1 moved the
    // real journal aside and its pass 2 never put it back (it crashed, or pass 1 was run twice
    // by hand) - and MOVEFILE_REPLACE_EXISTING would then destroy the only copy of a real
    // collection's identities.
    if (GetFileAttributesA(keep) != INVALID_FILE_ATTRIBUTES) {
        printf("  [FAIL] %s already exists - a previous run left the real journal moved aside. "
               "Put it back by hand before running this test again; REFUSING to touch %s\n",
               keep, path);
        return 1;
    }
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
        if (MoveFileExA(path, keep, MOVEFILE_REPLACE_EXISTING)) {
            printf("  NOTE: an existing journal was moved aside to %s, restored in pass 2\n",
                   keep);
        } else {
            printf("  [FAIL] a journal already exists at %s and could not be moved aside - "
                   "REFUSING to overwrite it\n",
                   path);
            return 1;
        }
    }
    DeleteFileA(path);
    // journalInit already parsed whatever was there, so drain the in-memory copy too - the
    // counts below are absolute.
    for (;;) {
        char one[1][256];
        if (ut::journalRecords(one, 1) != 1) break;
        if (!ut::journalRemove(one[0])) break;
    }
    check(ut::journalCount() == 0, "the journal starts empty");

    ut::UtReplicaCapture a, b, c;
    fill(&a, "records/items/gearhead/a01_head.dbr", 1, 3);
    fill(&b, "records/items/gearweapons/b02_sword.dbr", 1, 1);
    fill(&c, "records/items/gearhands/c03_gloves.dbr", 2, 2);
    check(ut::journalUpsert(a), "upsert A");
    check(ut::journalUpsert(b), "upsert B");
    check(ut::journalUpsert(c), "upsert C");
    check(ut::journalCount() == 3, "three entries in memory");
    // max_per_record == 1, so a second deposit of the SAME record must replace, not append.
    fill(&a, "records/items/gearhead/a01_head.dbr", 1, 4);
    check(ut::journalUpsert(a), "upsert A again (same record)");
    check(ut::journalCount() == 3, "still three entries - one per record");

    // Entries captured by a game whose ItemReplicaInfo had another size: the census behind the
    // start-up WARN. These three carry 0x190 bytes, so against a pretend 0x1A0 all three are
    // "another length" and a take of each would be refused; against 0x190 none is.
    {
        unsigned int otherLen = 0;
        check(ut::journalReplicaLengthCensus(0x1A0, &otherLen) == 3 && otherLen == 0x190,
              "census: three entries carry a 0x190 replica against a pretend 0x1A0");
        check(ut::journalReplicaLengthCensus(0x190, &otherLen) == 0 && otherLen == 0,
              "census: none against this build's 0x190");
    }
    check(ut::journalHas("records/items/gearhead/a01_head.dbr"), "journalHas A");
    check(!ut::journalHas("records/items/nope.dbr"), "journalHas rejects an unknown record");

    ut::journalService();  // the worker's write
    ut::logFlush();
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    check(h != INVALID_HANDLE_VALUE, "the file exists after journalService()");
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    // The journal is TEXT. Line 1 carries the format; one line per item.
    std::string text;
    check(slurp(path, &text), "the journal reads back as text");
    check(text.rfind("{\"journal\":\"grim dawn uniquetab", 0) == 0, "line 1 is the header object");
    check(text.find("\"format\":4") != std::string::npos, "line 1 carries format 4");
    // tableCopies is 0 because these three entries went in through journalUpsert - the legacy,
    // map-owned deposit, which never puts a copy into the private table. That 0 is the whole
    // safety property of the format-3 upgrade.
    check(text.find("\"tableCopies\":0") != std::string::npos,
          "line 1 says 0 copies in the private table (an ordinary deposit is MAP-owned)");
    check(text.find("\"count\":") == std::string::npos,
          "and no entry carries a \"count\" key at all (absent = 0, so an upgraded file gains no noise)");
    check(text.find("\"entries\":3") != std::string::npos,
          "line 1 says 3 entries (this is what journal_guard.ps1 cross-checks)");
    // The header counts. A fresh deposit is stored BY CONSTRUCTION, so three
    // upserts give three stored and nothing unknown.
    check(text.find("\"stored\":3") != std::string::npos, "line 1 says 3 stored");
    check(text.find("\"notStored\":0") != std::string::npos, "line 1 says 0 not stored");
    check(text.find("\"unknown\":0") != std::string::npos, "line 1 says 0 unreconciled");
    check(countRecordLines(text) == 3, "three item lines (one per stored record)");
    check(text.find("\"record\":\"records/items/gearhead/a01_head.dbr\"") != std::string::npos,
          "record A is greppable in the file, in plain text");
    // NAMED slot keys - <name>@<3 hex digits>, the offset still authoritative.
    check(text.find("\"baseRecord@008\":") != std::string::npos,
          "the base-record slot is written as baseRecord@008");
    check(text.find("\"s008\":") == std::string::npos, "the bare s008 key is gone from format 3");
    check(text.find("\"stored\":true") != std::string::npos,
          "a deposited entry is marked \"stored\":true");
    check(text.find("\"raw\":\"") != std::string::npos, "the replica words are written as raw");
    check(text.empty() || text[text.size() - 1] == '\n', "the file ends with a newline");
    printf("      line 1: %.*s\n", (int)text.find('\n'), text.c_str());

    // The take side: emptying a box drops the entry, and the file follows.
    check(ut::journalRemove("records/items/gearweapons/b02_sword.dbr"), "remove B");
    check(!ut::journalRemove("records/items/gearweapons/b02_sword.dbr"), "remove B twice = false");
    check(ut::journalCount() == 2, "two entries after the take");
    ut::journalService();
    check(slurp(path, &text), "the journal reads back as text after the removal");
    check(text.find("\"entries\":2") != std::string::npos, "line 1 says 2 entries after the take");
    check(countRecordLines(text) == 2, "two item lines after the take");
    check(text.find("b02_sword") == std::string::npos, "the removed record is gone from the file");

    char recs[8][256];
    const int n = ut::journalRecords(recs, 8);
    check(n == 2, "journalRecords returns two paths");
    for (int i = 0; i < n; ++i) printf("      %s\n", recs[i]);

    // ---- the entry pass 2 will read back off the disk --------------------------------------
    ut::UtReplicaCapture id;
    fillIdentity(&id);
    check(ut::journalUpsert(id), "upsert the identity entry (every field set)");
    identityChecks("in memory, before the file is written");
    ut::journalService();
    ut::logFlush();

    printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
