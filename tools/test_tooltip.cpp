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
//      after 5,000 build/free pairs.
//
// WHAT IS NOT COVERED (said here so nobody reads more into a pass than is in it):
// this harness drives `tooltipSwapBuild` / `tooltipSwapFree` / `tooltipCompareBox`
// directly. It never enters a detour body, so `hk_GameTextLineToString`, the latch
// (`latchBody` / `pendingLine`) and the `__finally` that must free the array when the trampoline
// UNWINDS are exercised only in the game. Driving them offline needs the trampoline pointer to be
// injectable, which it is not today.
//
// Exit code 0 = all of it held.
#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "MinHook.h"

#include "../src/ut_config.h"
#include "../src/ut_log.h"
#include "../src/ut_tooltip.h"

// ---- stubs: everything ut_tooltip.cpp calls that is not ut_config.cpp ------------------------
namespace ut {
volatile long g_logLevel = UT_LOG_TRACE;  // the harness wants every line, whatever the ini says
void logSetLevel(const char*) {}
void logAtV(int level, const char* fmt, va_list ap) {
    const char* tags = "EWIDT";
    printf("    [log %c] ", tags[(level < 0 || level > 4) ? 2 : level]);
    vprintf(fmt, ap);
    printf("\n");
}
void logSetFlushEachLine(int) {}

// The journal / reagent-map probes. The swap does not consult them, but the linker wants them.
bool journalHas(const char*) { return false; }
bool plateOwns(const char*) { return false; }
bool reagentIsCollectionRecord(const char*) { return false; }
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

    printf("\n%s (%d failure(s))\n", g_fails ? "FAILED" : "PASSED", g_fails);
    return g_fails ? 1 : 0;
}
