// test_tooltip.cpp - the OFFLINE proof of the tooltip swap. NO GAME, NO ENGINE.
//
//   tools\build_test_tooltip.bat        (finds vcvars64 itself, like build.bat)
//
// It links the REAL src\ut_tooltip.cpp - so the code under test is the code the game runs - with
// a stubbed trampoline and stubbed collection/journal probes, and proves:
//
//   1. tooltipSwapBuild over an N-line vector produces exactly N+1 records, the first N byte for
//      byte identical to the engine's, the mod's static line last, and a {begin,end,end} triple
//      whose span is (N+1) * 0x40;
//   2. a stub standing in for the trampoline, handed the triple this test built, sees that array
//      and nothing else - and the engine's own vector object is never written;
//   3. every refusal (empty, over-long, not a multiple of 0x40, end < begin, an unreadable
//      pointer that FAULTS inside the mod's SEH) allocates nothing;
//   4. nothing leaks: the borrowed-array balance is 0 before, 0 after every single case, and 0
//      after 5,000 build/free pairs;
//   7. "(another roll)": a monster infrequent the collection holds, hovered with another
//      prefix, suffix or seed than its stored roll, reads "In your collection (another roll)";
//      its stored roll reads the plain line, and so does a unique, an unknown kind, an unreadable
//      roll and a third line that is not ready; the stored roll is read once per quarter second.
//   6. THE BLUEPRINT LINE, through the REAL detour bodies (`tooltipTestArm` plants stub
//      trampolines where MinHook would): a blueprint's line comes from the BLUEPRINT's own row,
//      never from the crafted item whose GetUIDisplayText the engine runs inside it (Game.dll
//      0x31A4CF) - a blueprint you own whose crafted item you do not, and the reverse; the OLD
//      shape (the formula unhooked) reproduced, so that bug is pinned; a plain item
//      right after still gets its own line; an excluded blueprint gets none; and a C++ exception
//      out of the formula's trampoline leaves no suppression behind.
//
// WHAT IS NOT COVERED: the stub trampolines stand in for the engine, so the real vtable call at
// 0x31A4CF (and that it passes the SAME vector) is proved by test_bindings' byte check and by the
// disassembly quoted in ut_tooltip.cpp, not here.
//
// Exit code 0 = all of it held.
#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#include "MinHook.h"

#include "../src/ut_config.h"
#include "../src/ut_log.h"
#include "../src/ut_tooltip.h"
#include "../src/ut_buttons.h"
#include "../src/ut_miroll.h"

// ---- stubs: everything ut_tooltip.cpp calls that is not ut_config.cpp ------------------------
namespace ut {
volatile long g_logLevel = UT_LOG_TRACE;  // the harness wants every line, whatever the ini says
void logSetLevel(const char*) {}
std::string g_logAll;   // every line, for the cases that read the log
void logAtV(int level, const char* fmt, va_list ap) {
    const char* tags = "EWIDT";
    char line[2048];
    vsnprintf(line, sizeof(line), fmt, ap);
    printf("    [log %c] %s\n", tags[(level < 0 || level > 4) ? 2 : level], line);
    g_logAll += line;
    g_logAll += "\n";
}

// The buttons file's first look (ut_panel.cpp panelTextStartup): the REAL parser and chain over
// g_buttonsText's [tab] (empty = no file).
std::string g_buttonsText;
void panelTextStartup(char* yes, size_t yesCap, char* no, size_t noCap, bool* yesFromFile,
                      bool* noFromFile, char* roll, size_t rollCap) {
    utb::UtbFile f;
    std::string err;
    utt::UtTextContext c;
    if (!g_buttonsText.empty() && utb::utbParse(g_buttonsText, f, &err)) c.file = &f.words;
    const utt::UtTextPick y = utt::utTextResolve(c, utt::kTextTooltipYes, "");
    const utt::UtTextPick n = utt::utTextResolve(c, utt::kTextTooltipNo, "");
    const utt::UtTextPick r = utt::utTextResolve(c, utt::kTextTooltipRoll, "");
    _snprintf_s(yes, yesCap, _TRUNCATE, "%s", y.text.c_str());
    _snprintf_s(no, noCap, _TRUNCATE, "%s", n.text.c_str());
    if (roll && rollCap) _snprintf_s(roll, rollCap, _TRUNCATE, "%s", r.text.c_str());
    *yesFromFile = y.source <= utt::kSrcCommon;
    *noFromFile = n.source <= utt::kSrcCommon;
}
void logSetFlushEachLine(int) {}

// The journal / reagent-map probes. The swap does not consult them; the detour cases (6) do:
// a record is a collection record when it is in g_collection, owned when it is in g_owned.
std::string g_collection;   // "|rec|rec|"
std::string g_owned;
static bool inSet(const std::string& set, const char* rec) {
    return rec && set.find(std::string("|") + rec + "|") != std::string::npos;
}
bool journalHas(const char*) { return false; }
bool plateOwns(const char* rec) { return inSet(g_owned, rec); }
// The KIND (catIsMi, stubbed) and the stored roll of a row. A record is an MI
// when it is in g_mi, of unknown kind in g_kindUnknown; its stored roll is g_storedRoll when it
// is g_storedRec.
std::string g_mi;
std::string g_kindUnknown;
std::string g_storedRec;
UtRollKey g_storedRoll;
long g_rollReads = 0;
int journalRecordIsMi(const char* rec, bool mayLoad) {
    if (mayLoad) return -9;   // the tooltip must never load the catalogue on the game thread
    if (inSet(g_kindUnknown, rec)) return -1;
    return inSet(g_mi, rec) ? 1 : 0;
}
bool journalRollOf(const char* rec, UtRollKey* out) {
    ++g_rollReads;
    utRollClear(out);
    if (!rec || g_storedRec != rec) return false;
    *out = g_storedRoll;
    return true;
}
bool reagentIsCollectionRecord(const char* rec) { return inSet(g_collection, rec); }
unsigned int reagentReplicaOffset() { return 0x538; }

// The deliberate-probe bracket. Counted, so the test can assert it is balanced even when the
// read inside it FAULTS - an unbalanced depth would make dllmain's vectored handler go quiet for
// the rest of the session.
long g_probeDepth = 0;
long g_probeMax = 0;
void reagentProbeEnter() {
    ++g_probeDepth;
    if (g_probeDepth > g_probeMax) g_probeMax = g_probeDepth;
}
void reagentProbeLeave() { --g_probeDepth; }
long reagentProbeDepth() { return g_probeDepth; }

long liveCaptureEpoch() { return 1; }
}  // namespace ut

// MinHook is never reached by this harness (tooltipInstall is not called), but ut_tooltip.cpp
// references these four, so the link needs them.
extern "C" {
MH_STATUS WINAPI MH_CreateHook(LPVOID, LPVOID, LPVOID*) { return MH_ERROR_NOT_INITIALIZED; }
MH_STATUS WINAPI MH_EnableHook(LPVOID) { return MH_ERROR_NOT_INITIALIZED; }
MH_STATUS WINAPI MH_DisableHook(LPVOID) { return MH_ERROR_NOT_INITIALIZED; }
MH_STATUS WINAPI MH_RemoveHook(LPVOID) { return MH_ERROR_NOT_INITIALIZED; }
const char* WINAPI MH_StatusToString(MH_STATUS) { return "stub"; }
}

// ---- the fixture -----------------------------------------------------------------------------
static const size_t kLine = 0x40;

// mem::vector<GameTextLine> as GameTextLineToString reads it: {begin, end, capacityEnd}.
struct FakeVec {
    const unsigned char* begin;
    const unsigned char* end;
    const unsigned char* cap;
};

static int g_fails = 0;
static void check(bool ok, const char* what) {
    printf("  %-62s %s\n", what, ok ? "OK" : "FAIL");
    if (!ok) ++g_fails;
}

// Fills `n` 0x40-byte records with a per-record pattern that no memcpy bug could reproduce by
// accident: byte i of record r is (r * 7 + i * 13 + 1) & 0xFF, with the class word at +0 set to
// a plausible GameTextClass so the record also looks like a real line.
static unsigned char* makeLines(size_t n) {
    unsigned char* buf = (unsigned char*)malloc(n * kLine);
    for (size_t r = 0; r < n; ++r) {
        for (size_t i = 0; i < kLine; ++i) {
            buf[r * kLine + i] = (unsigned char)((r * 7 + i * 13 + 1) & 0xFF);
        }
        const unsigned int cls = (r == 0) ? 0x11u : 0x12u;
        memcpy(buf + r * kLine, &cls, 4);
    }
    return buf;
}

// The mod's prepared line, as a recognisable 0x40-byte blob.
static void makeExtra(unsigned char* out, unsigned int cls) {
    memset(out, 0xA5, kLine);
    memcpy(out, &cls, 4);
}

// The stubbed TRAMPOLINE: this is what the engine's GameTextLineToString would be. It records
// what it was handed so the test can assert the borrowed triple, not just the array.
static const unsigned char* g_sawBegin = nullptr;
static const unsigned char* g_sawEnd = nullptr;
static size_t g_sawCount = 0;
static void stubTrampoline(const void* vec) {
    const FakeVec* v = (const FakeVec*)vec;
    g_sawBegin = v->begin;
    g_sawEnd = v->end;
    g_sawCount = (size_t)(v->end - v->begin) / kLine;
}

static bool caseBuild(size_t n) {
    unsigned char* lines = makeLines(n);
    unsigned char extra[kLine];
    makeExtra(extra, 0x50);
    FakeVec vec;
    vec.begin = lines;
    vec.end = lines + n * kLine;
    vec.cap = vec.end;
    // A copy of the engine's vector object and of its lines, to prove neither is written.
    FakeVec before = vec;
    unsigned char* mirror = (unsigned char*)malloc(n * kLine);
    memcpy(mirror, lines, n * kLine);

    unsigned char* owned = nullptr;
    ut::UtTooltipSwap borrowed;
    memset(&borrowed, 0, sizeof(borrowed));
    bool ok = ut::tooltipSwapBuild(&vec, extra, &owned, &borrowed);
    if (!ok) {
        free(lines);
        free(mirror);
        return false;
    }
    stubTrampoline(&borrowed);

    bool good = true;
    good = good && owned != nullptr && borrowed.begin == owned;
    good = good && (size_t)(borrowed.end - borrowed.begin) == (n + 1) * kLine;
    good = good && borrowed.cap == borrowed.end;
    good = good && g_sawCount == n + 1;
    good = good && g_sawBegin == owned && g_sawEnd == borrowed.end;
    // the engine's N, byte for byte
    good = good && memcmp(owned, mirror, n * kLine) == 0;
    // the mod's static line, last, byte for byte
    good = good && memcmp(owned + n * kLine, extra, kLine) == 0;
    // the engine's own vector object and its storage are untouched
    good = good && memcmp(&before, &vec, sizeof(vec)) == 0;
    good = good && memcmp(lines, mirror, n * kLine) == 0;
    good = good && ut::tooltipSwapAllocBalance() == 1;

    ut::tooltipSwapFree(owned);
    good = good && ut::tooltipSwapAllocBalance() == 0;
    free(lines);
    free(mirror);
    return good;
}

static bool refuses(const void* vec, const char* label) {
    unsigned char extra[kLine];
    makeExtra(extra, 0x50);
    unsigned char* owned = (unsigned char*)(size_t)0xDEADBEEF;  // must be nulled by the callee
    ut::UtTooltipSwap borrowed;
    memset(&borrowed, 0xEE, sizeof(borrowed));
    const bool built = ut::tooltipSwapBuild(vec, extra, &owned, &borrowed);
    const bool ok = !built && owned == nullptr && ut::tooltipSwapAllocBalance() == 0;
    printf("      %-40s built=%d owned=%p balance=%ld\n", label, built ? 1 : 0, (void*)owned,
           ut::tooltipSwapAllocBalance());
    return ok;
}

// ---- 6. the detours, offline ----------------------------------------------------------------
// A fake engine Item: the ItemReplicaInfo at +0x538 holds the base-record std::string at +0x08
// (pointer form - every record is longer than 15 bytes), which is all readRecord reads.
struct FakeItem {
    unsigned char raw[0x600];
    std::string rec;
    FakeItem* crafted;   // a blueprint's [this+0x1108]
};
struct MsvcStr {
    const char* ptr;
    unsigned char pad[8];
    size_t size;
    size_t res;
};
static void fakeItemInit(FakeItem* it, const char* rec, FakeItem* crafted) {
    memset(it->raw, 0, sizeof(it->raw));
    it->rec = rec;
    it->crafted = crafted;
    MsvcStr st;
    memset(&st, 0, sizeof(st));
    st.ptr = it->rec.c_str();
    st.size = it->rec.size();
    st.res = it->rec.size() < 16 ? 16 : it->rec.size();
    memcpy(it->raw + 0x538 + 0x08, &st, sizeof(st));
}

// A growable line vector with the engine's {begin, end, cap} at its head.
struct GrowVec {
    unsigned char* begin;
    unsigned char* end;
    unsigned char* cap;
    unsigned char store[64 * 0x40];
};
static void vecReset(GrowVec* v) {
    memset(v->store, 0, sizeof(v->store));
    v->begin = v->store;
    v->end = v->store;
    v->cap = v->store + sizeof(v->store);
}
static void vecPush(GrowVec* v, unsigned int cls) {
    memset(v->end, 0x30 + (int)((v->end - v->begin) / 0x40), 0x40);
    memcpy(v->end, &cls, 4);
    v->end += 0x40;
}

static ut::UtTooltipHooks g_det;   // the REAL detour bodies, filled by tooltipTestArm
static bool g_throwInFormula = false;

// The stub ENGINE: ItemEquipment's text is three lines (name, base stats, bonuses).
static void __cdecl fakeEquipText(void*, const void*, void* lines, bool) {
    GrowVec* v = (GrowVec*)lines;
    vecPush(v, 0x07);
    vecPush(v, 0x12);
    vecPush(v, 0x13);
}
// ItemArtifactFormula's text: its title and description, then - exactly like Game.dll 0x31A4CF -
// the CRAFTED item's GetUIDisplayText into the SAME vector through its vtable (which lands on the
// HOOKED ItemEquipment detour), then the reagent lines.
static void __cdecl fakeFormulaText(void* item, const void* ch, void* lines, bool detailed) {
    GrowVec* v = (GrowVec*)lines;
    vecPush(v, 0x0D);
    vecPush(v, 0x3A);
    FakeItem* self = (FakeItem*)item;
    if (self->crafted) g_det.equip(self->crafted, ch, lines, detailed);
    if (g_throwInFormula) throw 42;
    vecPush(v, 0x3B);
    vecPush(v, 0x3C);
}
static void __cdecl fakeNoText(void*, const void*, void*, bool) {}

static size_t g_strCount = 0;
static unsigned char g_strLast[0x40];
static void __cdecl fakeToString(const void* lines, void*) {
    const FakeVec* v = (const FakeVec*)lines;
    g_strCount = (size_t)(v->end - v->begin) / kLine;
    memcpy(g_strLast, v->end - kLine, kLine);
}

static unsigned char g_yes[0x40], g_no[0x40];
// What the rollover shows under the item: 1 = "In your collection", 2 = "Not in your collection",
// 0 = no line at all. `n` is the engine's own line count.
static int lineAfter(size_t n) {
    if (g_strCount == n + 1 && memcmp(g_strLast, g_yes, kLine) == 0) return 1;
    if (g_strCount == n + 1 && memcmp(g_strLast, g_no, kLine) == 0) return 2;
    if (g_strCount == n) return 0;
    return -1;
}
// One rollover: the given GetUIDisplayText, then GameTextLineToString over the same vector.
static int hover(ut::UtTooltipTextFn text, FakeItem* item, size_t* nOut) {
    static GrowVec v;
    vecReset(&v);
    text(item, nullptr, &v, true);
    *nOut = (size_t)(v.end - v.begin) / kLine;
    g_strCount = 0;
    g_det.toString(&v, nullptr);
    return lineAfter(*nOut);
}

static void caseDetours() {
    printf("\n6. the blueprint line: its OWN row, never the crafted item's\n");
    makeExtra(g_yes, 0x2C);
    g_yes[8] = 'Y';
    makeExtra(g_no, 0x4A);
    g_no[8] = 'N';
    ut::UtTooltipHooks orig;
    orig.item = &fakeNoText;
    orig.equip = &fakeEquipText;
    orig.artifact = &fakeNoText;
    orig.relic = &fakeNoText;
    orig.formula = &fakeFormulaText;
    orig.toString = &fakeToString;
    ut::tooltipTestArm(orig, g_yes, g_no, &g_det);
    ut::g_cfg.tooltipMark = 1;

    // Two blueprints and the two items they craft, all four on the pages.
    static FakeItem swordX, ringY, bpA, bpB, bpTest, plainZ;
    fakeItemInit(&swordX, "records/items/crafting/blueprints/../gearweapons/x_sword.dbr", nullptr);
    fakeItemInit(&ringY, "records/items/gearaccessories/rings/y_ring.dbr", nullptr);
    fakeItemInit(&bpA, "records/items/crafting/blueprints/weapon/craft_x_sword.dbr", &swordX);
    fakeItemInit(&bpB, "records/items/crafting/blueprints/ring/craft_y_ring.dbr", &ringY);
    fakeItemInit(&bpTest, "records/items/crafting/blueprints/blueprint_test_y.dbr", &ringY);
    fakeItemInit(&plainZ, "records/items/gearaccessories/rings/z_ring.dbr", nullptr);
    ut::g_collection = "|" + swordX.rec + "|" + ringY.rec + "|" + bpA.rec + "|" + bpB.rec + "|" +
                       plainZ.rec + "|";
    // You own blueprint A (its sword you do not) and ring Y (its blueprint B you do not).
    ut::g_owned = "|" + bpA.rec + "|" + ringY.rec + "|" + plainZ.rec + "|";

    size_t n = 0;
    const long supBefore = ut::tooltipInnerSuppressed();
    int got = hover(g_det.formula, &bpA, &n);
    printf("      blueprint A (owned), crafts sword X (not owned): %zu engine lines, mark %d\n", n, got);
    check(n == 7 && got == 1, "a blueprint you OWN reads \"In your collection\"");
    check(ut::tooltipInnerSuppressed() == supBefore + 1,
          "the crafted item's latch inside it was suppressed (once)");
    got = hover(g_det.formula, &bpB, &n);
    printf("      blueprint B (not owned), crafts ring Y (owned): %zu engine lines, mark %d\n", n, got);
    check(n == 7 && got == 2, "a blueprint you do NOT own whose crafted item you own: \"Not in\"");

    // The OLD shape: the formula override unhooked (as in 1.1.0). The engine's own formula code
    // still calls the crafted item's HOOKED text, whose latch then decorated the blueprint.
    got = hover(&fakeFormulaText, &bpA, &n);
    check(got == 2, "OLD (formula unhooked): blueprint A showed the SWORD's \"Not in\" - the bug");
    got = hover(&fakeFormulaText, &bpB, &n);
    check(got == 1, "OLD: blueprint B showed the RING's \"In your collection\"");

    // Suppression ends with the blueprint's tooltip.
    got = hover(g_det.equip, &plainZ, &n);
    check(n == 3 && got == 1, "a plain item hovered right after keeps its own line");
    got = hover(g_det.equip, &swordX, &n);
    check(n == 3 && got == 2, "the sword itself still reads \"Not in your collection\"");

    // A blueprint that is not on the pages (Crate's test blueprints): no line at all - not even
    // the crafted ring's, which IS a collection record and owned.
    got = hover(g_det.formula, &bpTest, &n);
    check(n == 7 && got == 0, "an excluded blueprint gets no line (not the crafted item's either)");

    // An engine C++ exception out of the formula's trampoline: the __finally lowers the depth.
    g_throwInFormula = true;
    bool threw = false;
    try {
        static GrowVec v;
        vecReset(&v);
        g_det.formula(&bpA, nullptr, &v, true);
    } catch (int) {
        threw = true;
    }
    g_throwInFormula = false;
    check(threw, "the exception unwound through the detour untouched");
    got = hover(g_det.equip, &plainZ, &n);
    check(got == 1, "and left no suppression behind: the next item still gets its line");
    check(ut::tooltipSwapAllocBalance() == 0, "no borrowed array left alive by the detour cases");
    ut::g_collection.clear();
    ut::g_owned.clear();
}

// ---- 7. "(another roll)" on a monster infrequent -----------------------------------------------
// The hovered item's roll is read out of ITS replica (+0x538 + 0x28 / 0x48 / 0x68), the stored
// one through the journal accessor (stubbed above), and only an MI whose record the collection
// holds can read "(another roll)".
static std::string g_rollStr[8];
static int g_rollStrN = 0;
static void putStr(unsigned char* at, const char* text) {
    std::string& keep = g_rollStr[g_rollStrN++ & 7];
    keep = text;
    MsvcStr st;
    memset(&st, 0, sizeof(st));
    if (keep.empty()) {
        st.res = 15;   // an empty SSO string
    } else {
        st.ptr = keep.c_str();
        st.size = keep.size();
        st.res = keep.size() < 16 ? 16 : keep.size();
    }
    memcpy(at, &st, sizeof(st));
}
static void fakeItemRoll(FakeItem* it, const char* prefix, const char* suffix, unsigned int seed) {
    putStr(it->raw + 0x538 + 0x28, prefix);
    putStr(it->raw + 0x538 + 0x48, suffix);
    memcpy(it->raw + 0x538 + 0x68, &seed, 4);
}

static unsigned char g_roll[0x40];
static int lineAfter3(size_t n) {
    if (g_strCount == n + 1 && memcmp(g_strLast, g_roll, kLine) == 0) return 3;
    return lineAfter(n);
}
static int hover3(ut::UtTooltipTextFn text, FakeItem* item, size_t* nOut) {
    static GrowVec v;
    vecReset(&v);
    text(item, nullptr, &v, true);
    *nOut = (size_t)(v.end - v.begin) / kLine;
    g_strCount = 0;
    g_det.toString(&v, nullptr);
    return lineAfter3(*nOut);
}

static void caseAnotherRoll() {
    printf("\n7. a monster infrequent of ANOTHER roll: \"In your collection (another roll)\"\n");
    makeExtra(g_roll, 0x2C);
    g_roll[8] = 'R';
    ut::tooltipTestArmRoll(g_roll);
    ut::g_cfg.tooltipMark = 1;
    static const char* const kPre = "records/items/lootaffixes/prefix/b_ar007_ar_b.dbr";
    static const char* const kSuf = "records/items/lootaffixes/suffix/a030b_off_dmg%fire_05_je.dbr";
    static FakeItem same, other, seedOnly, noAffix, unique, notOwned, unknownKind, garbage;
    const char* const mi = "records/items/gearaccessories/rings/b305c_ring.dbr";
    fakeItemInit(&same, mi, nullptr);
    fakeItemRoll(&same, kPre, kSuf, 2115467035u);
    fakeItemInit(&other, mi, nullptr);
    fakeItemRoll(&other, "records/items/lootaffixes/prefix/aa010a_damod_01.dbr",
                 "records/items/lootaffixes/suffix/b_ar044_ar_b.dbr", 1621750206u);
    fakeItemInit(&seedOnly, mi, nullptr);
    fakeItemRoll(&seedOnly, kPre, kSuf, 2115467036u);
    fakeItemInit(&noAffix, mi, nullptr);
    fakeItemRoll(&noAffix, "", "", 2115467035u);
    fakeItemInit(&garbage, mi, nullptr);
    fakeItemRoll(&garbage, kPre, kSuf, 2115467035u);
    {
        // a prefix window whose size is larger than its capacity: not a std::string
        size_t bad = 999;
        memcpy(garbage.raw + 0x538 + 0x28 + 0x10, &bad, sizeof(bad));
    }
    fakeItemInit(&unique, "records/items/gearhead/d208_head.dbr", nullptr);
    fakeItemRoll(&unique, "", "", 77u);
    fakeItemInit(&notOwned, "records/items/gearaccessories/rings/b305d_ring.dbr", nullptr);
    fakeItemRoll(&notOwned, kPre, kSuf, 5u);
    fakeItemInit(&unknownKind, "records/items/gearaccessories/rings/b305e_ring.dbr", nullptr);
    fakeItemRoll(&unknownKind, kPre, kSuf, 6u);

    ut::g_collection = "|" + same.rec + "|" + unique.rec + "|" + notOwned.rec + "|" +
                       unknownKind.rec + "|";
    ut::g_owned = "|" + same.rec + "|" + unique.rec + "|" + unknownKind.rec + "|";
    ut::g_mi = "|" + same.rec + "|" + notOwned.rec + "|";
    ut::g_kindUnknown = "|" + unknownKind.rec + "|";
    // The stored row: the decoded roll (ut_miroll.h).
    ut::g_storedRec = mi;
    ut::utRollClear(&ut::g_storedRoll);
    strcpy_s(ut::g_storedRoll.prefix, sizeof(ut::g_storedRoll.prefix), kPre);
    strcpy_s(ut::g_storedRoll.suffix, sizeof(ut::g_storedRoll.suffix), kSuf);
    ut::g_storedRoll.seed = 2115467035u;
    ut::g_storedRoll.known = true;

    size_t n = 0;
    const long linesBefore = ut::tooltipRollLines();
    check(hover3(g_det.equip, &same, &n) == 1, "the STORED roll hovered: \"In your collection\"");
    check(hover3(g_det.equip, &other, &n) == 3,
          "another prefix, suffix and seed: \"In your collection (another roll)\"");
    check(hover3(g_det.equip, &seedOnly, &n) == 3, "the same affixes, another seed: another roll");
    check(hover3(g_det.equip, &noAffix, &n) == 3, "no affixes at all over a stored affixed roll: another roll");
    check(ut::tooltipRollLines() == linesBefore + 3, "the anotherRoll counter counted the three");
    check(hover3(g_det.equip, &garbage, &n) == 1,
          "an unreadable hovered roll keeps the plain collected line (never a guess)");
    check(hover3(g_det.equip, &unique, &n) == 1,
          "a UNIQUE the collection holds never reads \"(another roll)\", whatever its seed");
    check(hover3(g_det.equip, &notOwned, &n) == 2, "an MI the collection does not hold: \"Not in\"");
    check(hover3(g_det.equip, &unknownKind, &n) == 1,
          "a record of UNKNOWN kind keeps the plain collected line");
    // The memo: one journal read per record per quarter second, however often it is hovered.
    ut::tooltipTestArmRoll(g_roll);
    const long readsBefore = ut::g_rollReads;
    for (int i = 0; i < 50; ++i) hover3(g_det.equip, &other, &n);
    check(ut::g_rollReads - readsBefore == 1, "50 rollovers of one MI read its stored roll ONCE (memoised)");
    // Not ready (the third line did not verify): the plain collected line, never nothing.
    ut::tooltipTestArmRoll(nullptr);
    check(hover3(g_det.equip, &other, &n) == 1,
          "with the third line not ready, another roll reads the plain collected line");
    ut::tooltipTestArmRoll(g_roll);
    check(ut::tooltipSwapAllocBalance() == 0, "no borrowed array left alive by the roll cases");
    ut::g_collection.clear();
    ut::g_owned.clear();
    ut::g_mi.clear();
    ut::g_kindUnknown.clear();
    ut::g_storedRec.clear();
}

int main() {
    printf("the borrowed N+1 swap, offline\n\n");
    printf("1. the shape of the borrowed array\n");
    check(ut::tooltipSwapAllocBalance() == 0, "balance is 0 before anything is built");
    check(caseBuild(1), "N=1   -> 2 records, original first, static line last");
    check(caseBuild(20), "N=20  -> 21 records, original first, static line last");
    check(caseBuild(37), "N=37  -> 38 records, original first, static line last");
    check(caseBuild(512), "N=512 -> 513 records (the cap is inclusive)");

    printf("\n2. refusals allocate nothing\n");
    unsigned char* lines = makeLines(4);
    FakeVec v;
    bool all = true;

    v.begin = lines;
    v.end = lines;
    v.cap = lines;
    all = refuses(&v, "empty vector (N = 0)") && all;

    v.begin = lines;
    v.end = lines + 4 * kLine + 7;  // not a whole number of records
    v.cap = v.end;
    all = refuses(&v, "span is not a multiple of 0x40") && all;

    v.begin = lines + 4 * kLine;
    v.end = lines;  // end before begin
    v.cap = lines;
    all = refuses(&v, "end < begin") && all;

    v.begin = lines;
    v.end = lines + 513 * kLine;  // over the 512-line cap
    v.cap = v.end;
    all = refuses(&v, "N = 513, past the sanity cap") && all;

    all = refuses((const void*)(size_t)0x10, "an UNREADABLE vector pointer (SEH)") && all;
    all = refuses(nullptr, "a null vector") && all;
    check(all, "every refusal returned false, nulled the owner and allocated nothing");
    check(ut::g_probeDepth == 0, "the deliberate-probe bracket is balanced, faults included");
    check(ut::g_probeMax > 0, "the probe bracket was actually raised around the engine reads");
    free(lines);

    printf("\n3. nothing leaks over 5,000 build/free pairs\n");
    unsigned char extra[kLine];
    makeExtra(extra, 0x12);
    unsigned char* src = makeLines(30);
    FakeVec big;
    big.begin = src;
    big.end = src + 30 * kLine;
    big.cap = big.end;
    long peak = 0;
    for (int i = 0; i < 5000; ++i) {
        unsigned char* owned = nullptr;
        ut::UtTooltipSwap b;
        memset(&b, 0, sizeof(b));
        if (!ut::tooltipSwapBuild(&big, extra, &owned, &b)) {
            check(false, "build #i failed");
            break;
        }
        if (ut::tooltipSwapAllocBalance() > peak) peak = ut::tooltipSwapAllocBalance();
        ut::tooltipSwapFree(owned);
    }
    printf("      peak balance during the loop = %ld\n", peak);
    check(peak == 1, "at most one borrowed array is ever alive");
    check(ut::tooltipSwapAllocBalance() == 0, "balance is 0 again after 5,000 pairs");
    check(ut::g_probeDepth == 0, "the probe bracket is still balanced");
    free(src);

    printf("\n4. the compare byte is written INSIDE the widget\n");
    // sizeof(the base item box) is 0xE0 (exe 0x1E9DA8, `mov ecx,0xE0` right before the ctor call
    // at 0x1EDA10). The mod writes +0x7E. Both numbers live in ut_tooltip.cpp; this is the
    // arithmetic that makes the write legal, asserted where a reader can see it.
    check(0x7E < 0xE0, "box + 0x7E lies inside the 0xE0-byte item-box object");
    unsigned char widget[0xE0];
    memset(widget, 0, sizeof(widget));
    ut::g_cfg.comparePopup = 1;
    ut::tooltipCompareBox(widget, 0, true);
    check(widget[0x7E] == 1, "a COLLECTION box gets +0x7E = 1");
    check(widget[0x7D] == 0 && widget[0x7F] == 0, "no neighbouring byte was touched");
    widget[0x7E] = 1;
    ut::tooltipCompareBox(widget, 1, false);
    check(widget[0x7E] == 0, "a VANILLA-page box is cleared back to 0");
    widget[0x7E] = 1;
    ut::g_cfg.comparePopup = 0;
    ut::tooltipCompareBox(widget, 2, true);
    check(widget[0x7E] == 0, "compare_popup=0 clears it on the next relayout");

    caseDetours();
    caseAnotherRoll();

    printf("\n5. the two lines' text: [tab] of the buttons file, else the built-in English\n");
    // tooltipInit reads them before anything else (and then stops here: no Game.dll). [tab]
    // tooltip.yes of the buttons file is the collected line; tooltip.no in Cyrillic keeps its
    // UTF-8; the dead ini key tooltip_text_yes (gone since ini_version 39) is not read.
    strcpy_s(ut::g_cfg.tooltipTextYes, sizeof(ut::g_cfg.tooltipTextYes), "Legacy yes");
    ut::g_cfg.tooltipTextNo[0] = 0;
    ut::g_buttonsText = "[tab]\ntooltip.yes = Got it   ; a comment\ntooltip.no = \xD0\x9D\xD0\xB5\xD1\x82\n"
                        "tooltip.roll = \" (noch ein Wurf)\"\n"
                        "[HEL]\ngroups = head\n";
    ut::g_logAll.clear();
    ut::tooltipInit(nullptr);
    check(ut::g_logAll.find("collected=\"Got it\"") != std::string::npos,
          "tooltip.yes from [tab] of the buttons file (its comment cut)");
    check(ut::g_logAll.find("Legacy yes") == std::string::npos, "the dead tooltip_text_yes is not read");
    check(ut::g_logAll.find("notCollected=\"\xD0\x9D\xD0\xB5\xD1\x82\"") != std::string::npos,
          "tooltip.no from [tab], UTF-8 kept");
    check(ut::g_logAll.find("anotherRoll=\"Got it (noch ein Wurf)\"") != std::string::npos,
          "tooltip.roll from [tab] (quoted: its leading blank kept) follows tooltip.yes on the third line");
    // Without lines (no Game.dll) the per-world refresh is a no-op, never a crash.
    ut::tooltipTextRefresh("x", true, "y", true, " (z)");
    check(true, "tooltipTextRefresh before the lines exist does nothing");

    printf("\n%s (%d failure(s))\n", g_fails ? "FAILED" : "PASSED", g_fails);
    return g_fails ? 1 : 0;
}
