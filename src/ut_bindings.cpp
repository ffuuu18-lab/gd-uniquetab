// ut_bindings.cpp - the registry behind ut_bindings.h: every binding's outcome in one table, one
// INFO line, and the all-or-nothing gate that runs before a single hook is installed.

#include "ut_bindings.h"

#include <stdio.h>
#include <string.h>

#include "ut_log.h"

namespace ut {
namespace {

// ---- the two converted exe call sites ---------------------------------------------------------
// Site A, 1.3.0.8 rva 0x1EAB2A. The four `call qword ptr [rip+disp32]` displacements are the only
// address-bearing bytes, and all four are wildcarded, so the pattern carries no address at all:
//   84 C0                    test al,al
//   0F 84 ?? ?? ?? ??        je   <no room>
//   8B 95 08 02 00 00        mov  edx,[rbp+0x208]        ; the item id, off the handler's frame
//   FF 15 ?? ?? ?? ??        call [IsInventorySpaceAvailable]   ; returns to sig+0x14
//   84 C0                    test al,al
//   74 ??                    je   <no room>
//   41 B0 01                 mov  r8b,1
//   8B 95 08 02 00 00        mov  edx,[rbp+0x208]
//   48 8B CB                 mov  rcx,rbx
//   FF 15 ?? ?? ?? ??        call [PlayerInventoryCtrl::RemoveItem]
// The accepted return window (sig+6 .. sig+0x1B) is byte-for-byte the old 0x1EAB30..45.
const unsigned char kSiteABytes[] = {
    0x84, 0xC0, 0x0F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x8B, 0x95, 0x08, 0x02, 0x00, 0x00,
    0xFF, 0x15, 0x00, 0x00, 0x00, 0x00, 0x84, 0xC0, 0x74, 0x00, 0x41, 0xB0, 0x01, 0x8B,
    0x95, 0x08, 0x02, 0x00, 0x00, 0x48, 0x8B, 0xCB, 0xFF, 0x15, 0x00, 0x00, 0x00, 0x00};
const unsigned char kSiteAMask[] = {
    1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1,
    1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0};

// Site B, 1.3.0.8 rva 0x1EC64F: the SAME shape with `mov edx,r14d` where site A reads the id off
// its frame, which is exactly what tells the two apart. Label only - site B is the equipment
// quick-move and the table gate refuses every one of them whatever this says.
const unsigned char kSiteBBytes[] = {0x84, 0xC0, 0x0F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x41,
                                     0x8B, 0xD6, 0xFF, 0x15, 0x00, 0x00, 0x00, 0x00, 0x84,
                                     0xC0, 0x74, 0x00, 0x41, 0xB0, 0x01, 0x41, 0x8B, 0xD6,
                                     0x48, 0x8B, 0xCB, 0xFF, 0x15, 0x00, 0x00, 0x00, 0x00};
const unsigned char kSiteBMask[] = {1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1,
                                    1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0};

// The cursor DRAG site, Game.dll rva 0x173960 on 1.3.0.8. This one is not in the exe, so it needs
// no waiting: Game.dll is plain on disk and its .text is readable the moment the module is loaded,
// which is why this binding is resolved EARLY and is covered by the all-or-nothing gate.
//   48 8B DD                 mov  rbx,rbp
//   8B 57 30                 mov  edx,[rdi+0x30]        ; the id the handler read at +0x1738FF
//   48 8B 0D ?? ?? ?? ??     mov  rcx,[rip+disp32]
//   E8 ?? ?? ?? ??           call <the reagent add>     ; returns to sig+0x12 = the old 0x173972
//   84 C0 / 74 ??            test al,al / je
//   48 85 DB / 74 ??         test rbx,rbx / je
//   48 8B 03 / 48 8B CB      mov  rax,[rbx] / mov rcx,rbx
//   FF 90 10 04 00 00        call qword [rax+0x410]
//   48 8B CF / E8 ?? ?? ?? ??
//   48 8B C8 / 8B 57 30      mov  rcx,rax / mov edx,[rdi+0x30]
//   E8 ?? ?? ?? ??           call SendRemoveItemFromInventory
//   89 6F 30                 mov  [rdi+0x30],ebp        ; THE CURSOR SLOT IS CLEARED
// The last instruction is what makes the drag's removal unconditional - no container is searched,
// so it cannot miss - and it is inside the pattern, which is why the pattern is this long.
const unsigned char kDragBytes[] = {
    0x48, 0x8B, 0xDD, 0x8B, 0x57, 0x30, 0x48, 0x8B, 0x0D, 0x00, 0x00, 0x00, 0x00, 0xE8, 0x00, 0x00,
    0x00, 0x00, 0x84, 0xC0, 0x74, 0x00, 0x48, 0x85, 0xDB, 0x74, 0x00, 0x48, 0x8B, 0x03, 0x48, 0x8B,
    0xCB, 0xFF, 0x90, 0x10, 0x04, 0x00, 0x00, 0x48, 0x8B, 0xCF, 0xE8, 0x00, 0x00, 0x00, 0x00, 0x48,
    0x8B, 0xC8, 0x8B, 0x57, 0x30, 0xE8, 0x00, 0x00, 0x00, 0x00, 0x89, 0x6F, 0x30};
const unsigned char kDragMask[] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 0, 0,
                                   0, 0, 1, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1,
                                   1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1,
                                   1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1};

// ---- the table --------------------------------------------------------------------------------
struct Row {
    const char* name;
    unsigned char cls;
    unsigned char phase;
    const char* how;
    const char* confirm;
    unsigned char gate;  // UT_BIND_CRITICAL unless the row says otherwise
};

// `ok` starts true for STRUCTURAL and LITERAL: their confirmation is not a start-up event but a
// test the code performs at EVERY use (an SEH-guarded read with a plausibility range, a vtable
// cross-check). What the gate asserts for them is that the test EXISTS and is named here; a
// failure at use turns its own route off and says so. Everything else starts NOT resolved and
// must be reported by the module that owns it, so a decoder that silently stopped running is a
// gate failure rather than a feature that quietly went missing.
const Row kRows[] = {
    // -- DECODED, early -------------------------------------------------------------------------
    {"item.craftingMaterial", UT_BIND_DECODED, UT_BIND_EARLY,
     "Item::IsReagentCompatible bytes (0F B6 81 <d32> C3)", "non-zero, below 0x2000"},
    {"item.soulbound", UT_BIND_DECODED, UT_BIND_EARLY, "Item::IsSoulbound bytes",
     "non-zero; two bytes below item.untradeable"},
    {"item.untradeable", UT_BIND_DECODED, UT_BIND_EARLY, "Item::IsUntradeable bytes",
     "non-zero; two bytes above item.soulbound"},
    {"item.replicaInfo", UT_BIND_DECODED, UT_BIND_EARLY,
     "Item::GetItemReplicaInfo bytes (48 8D 91 <d32>)", "non-zero, below 0x2000"},
    {"item.replicaSize", UT_BIND_DECODED, UT_BIND_EARLY,
     "Item::Item's next member construction AND ItemReplicaInfo::operator='s largest store",
     "both decoded, equal, 8-aligned, 0x100..0x200"},
    {"item.seedRerolls", UT_BIND_DECODED, UT_BIND_EARLY, "its getter's bytes (8B 81 <d32> C3)",
     "inside the replica block"},
    {"item.affixRerolls", UT_BIND_DECODED, UT_BIND_EARLY, "its getter's bytes",
     "inside the replica block"},
    // Item fields, NOT replica fields - 0x880 / 0x884 on 1.3.0.8, above the block. Nothing reads
    // either offset, so both are ADVISORY.
    {"item.prefixClass", UT_BIND_DECODED, UT_BIND_EARLY, "its getter's bytes (8B 81 <d32> C3)",
     "non-zero, below 0x2000, four bytes below suffixClass", UT_BIND_ADVISORY},
    {"item.suffixClass", UT_BIND_DECODED, UT_BIND_EARLY, "its getter's bytes (8B 81 <d32> C3)",
     "non-zero, below 0x2000, four bytes above prefixClass", UT_BIND_ADVISORY},
    // Its only reader is a diagnostic field nothing decides on: ADVISORY, and no fallback.
    {"item.incrementStackSlot", UT_BIND_DECODED, UT_BIND_EARLY,
     "AddItemToReagents+0x1FF (FF 97 <d32>)", "8-aligned slot below 0x2000", UT_BIND_ADVISORY},
    {"item.stackMirror", UT_BIND_DECODED, UT_BIND_EARLY, "Item::SetStackSize's own store",
     "inside [replicaInfo, replicaInfo+size)"},
    {"player.ctrlId", UT_BIND_DECODED, UT_BIND_EARLY,
     "CursorHandler::GetPlayerCtrl bytes (mov r32,[rax+<d32>])", "non-zero, below 0x20000"},
    {"objectManager.objectFromId", UT_BIND_DECODED, UT_BIND_EARLY,
     "TakeItemFromReagents(id,int)+0x2C (E8 <rel32>)", "the target lands inside Game.dll"},
    // -- STRUCTURAL -----------------------------------------------------------------------------
    {"msvc.treeNode", UT_BIND_STRUCTURAL, UT_BIND_EARLY, "MSVC std::map node layout",
     "+0x20 key, +0x40 protoId, +0x44 count; every read is SEH-guarded and range-checked"},
    {"msvc.string", UT_BIND_STRUCTURAL, UT_BIND_EARLY, "MSVC std::string layout",
     "capacity < 16 means in place; size <= capacity <= 0x4000 or the string is refused"},
    {"boxVector.stride", UT_BIND_STRUCTURAL, UT_BIND_EARLY, "std::vector element stride, 16",
     "(last-first) must divide by 16 and stay under 8192 elements"},
    // -- LITERAL --------------------------------------------------------------------------------
    {"hud.caravanWindow", UT_BIND_LITERAL, UT_BIND_EARLY, "literal 0x4FD08 (a landmark, no route)",
     "the CaravanWindow is found by a stack scan and proved by pages[3] == this window"},
    {"caravan.pages", UT_BIND_LITERAL, UT_BIND_EARLY, "literal 0x13C8",
     "pages[3] must be the window the detour was called on; pages[0..2] non-null and 8-aligned"},
    {"caravan.pageIndex", UT_BIND_LITERAL, UT_BIND_EARLY, "literal 0x1728", "index must be 0..3"},
    {"window.boxVector", UT_BIND_LITERAL, UT_BIND_EARLY, "literal 0x380",
     "(last-first) divides by the 16-byte stride, under the 8192 ceiling"},
    {"window.origin", UT_BIND_LITERAL, UT_BIND_EARLY, "literal 0x40",
     "read only inside the Draw detour, SEH-guarded"},
    {"window.plateWidget", UT_BIND_LITERAL, UT_BIND_EARLY, "literal 0x480",
     "the widget's vtable must be readable before anything is written"},
    {"window.plateTexture", UT_BIND_LITERAL, UT_BIND_EARLY, "literal 0x4B0",
     "the slot must hold a readable GraphicsTexture or the plate stays off"},
    {"window.plateSize", UT_BIND_LITERAL, UT_BIND_EARLY, "literals 0x4C0 / 0x4C4",
     "both must be plausible pixel sizes"},
    {"window.searchRange", UT_BIND_LITERAL, UT_BIND_EARLY, "literals 0x4E8 / 0x4F0",
     "begin <= end and the span must divide by the element size"},
    {"box.protoId", UT_BIND_LITERAL, UT_BIND_EARLY, "literal 0x30",
     "the id must resolve to a record the catalogue knows"},
    {"box.localPos", UT_BIND_LITERAL, UT_BIND_EARLY, "literal 0x6C",
     "read back after the engine's own SetPos wrote it"},
    {"box.parentOrigin", UT_BIND_LITERAL, UT_BIND_EARLY, "literal 0x80",
     "SEH-guarded; a fault turns the label route off"},
    {"vtable.slotNumbers", UT_BIND_LITERAL, UT_BIND_EARLY,
     "literals 0x18 Load / 0x20 Draw / 0x38 mouse / 0x118 search / 0x128 page / 0xA8 / 0xB8",
     "CROSS-CHECK: slot +0x18 of the live object must be the signature-located function, in the "
     "same SEH frame, before any other slot in that vtable is believed"},
    {"gameEngine.saveVariant", UT_BIND_LITERAL, UT_BIND_EARLY, "literal 0x375BA",
     "log line only - nothing decides on it; -1 with a reason when it cannot be read",
     UT_BIND_ADVISORY},
    {"gamedll.dragSite", UT_BIND_SIGNATURE, UT_BIND_EARLY,
     "61 bytes with the three call displacements wildcarded, scanned in Game.dll's .text",
     "exactly one match; the return address must then be inside the located window or the "
     "deposit is REFUSED"},
    {"gameTextLine.size", UT_BIND_LITERAL, UT_BIND_EARLY, "literal 0x40",
     "the tooltip route is optional and turns itself off on any inconsistency"},
    // -- SIGNATURE, late (the exe's .text is encrypted until the Steam stub has run) -------------
    {"exe.ReagentWindowLoad", UT_BIND_SIGNATURE, UT_BIND_LATE, "45-byte prologue, no RIP bytes",
     "exactly one match; the live vtable slot +0x18 must equal it"},
    {"exe.UIReagentItemLoad", UT_BIND_SIGNATURE, UT_BIND_LATE, "45-byte prologue",
     "exactly one match; occupies slot +0x18 of a real box widget"},
    {"exe.UIReagentItemSetItem", UT_BIND_SIGNATURE, UT_BIND_LATE, "29-byte prologue",
     "exactly one match; occupies slot +0xA8"},
    {"exe.UIReagentItemSetPos", UT_BIND_SIGNATURE, UT_BIND_LATE, "the whole 12-byte leaf",
     "exactly one match; occupies slot +0xB8"},
    {"exe.takeReplicaPair", UT_BIND_SIGNATURE, UT_BIND_LATE, "16 bytes, the call through +0x590",
     "exactly TWO matches, less than 0x400 bytes apart"},
    {"exe.depositSiteA", UT_BIND_SIGNATURE, UT_BIND_LATE,
     "42 bytes, the four call displacements wildcarded", "exactly one match in .text"},
    {"exe.depositSiteB", UT_BIND_SIGNATURE, UT_BIND_LATE,
     "36 bytes, the four call displacements wildcarded", "exactly one match in .text"},
};

const int kRowCount = (int)(sizeof(kRows) / sizeof(kRows[0]));

// `why` is COPIED, never pointed at. A caller that builds its reason with _snprintf_s into a local
// buffer - which is the natural thing to do when the reason carries a count - would otherwise hand
// this table a dangling pointer that only reads wrong in the log much later.
struct State {
    unsigned long long value = 0;
    bool ok = false;
    bool reported = false;
    bool warned = false;  // an ADVISORY failure is said once
    char why[192];
};

State g_state[kRowCount];
volatile LONG g_seeded = 0;
volatile LONG g_lateSaid = 0;
int g_exportCount = 0;
char g_summary[224] = "bindings: not resolved yet";
const char* g_failName = "";

void seed() {
    if (InterlockedExchange(&g_seeded, 1)) return;
    for (int i = 0; i < kRowCount; ++i) {
        // STRUCTURAL and LITERAL are confirmed at every use, not once at start-up - see the note
        // above kRows. They start satisfied; the code that uses them is what can still say no.
        _snprintf_s(g_state[i].why, sizeof(g_state[i].why), _TRUNCATE, "%s", "not reported");
        if (kRows[i].cls == UT_BIND_STRUCTURAL || kRows[i].cls == UT_BIND_LITERAL) {
            g_state[i].ok = true;
            g_state[i].reported = true;
            _snprintf_s(g_state[i].why, sizeof(g_state[i].why), _TRUNCATE, "%s",
                        kRows[i].confirm);
        }
    }
}

int indexOf(const char* name) {
    for (int i = 0; i < kRowCount; ++i) {
        if (strcmp(kRows[i].name, name) == 0) return i;
    }
    return -1;
}

const char* className(int cls) {
    switch (cls) {
        case UT_BIND_EXPORT: return "export";
        case UT_BIND_SIGNATURE: return "signature";
        case UT_BIND_DECODED: return "decoded";
        case UT_BIND_STRUCTURAL: return "structural";
        default: return "literal";
    }
}

// The size and the PE timestamp of one loaded module, FOR THE RECORD ONLY. Nothing in the mod
// compares these against anything: a version gate is exactly the thing this work package removes.
void sayModule(const wchar_t* name) {
    HMODULE m = name ? GetModuleHandleW(name) : GetModuleHandleW(nullptr);
    if (!m) {
        logI("module %S: not loaded", name ? name : L"Grim Dawn.exe");
        return;
    }
    const unsigned char* base = (const unsigned char*)m;
    unsigned long stamp = 0, image = 0;
    __try {
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
        if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
            const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE) {
                stamp = nt->FileHeader.TimeDateStamp;
                image = nt->OptionalHeader.SizeOfImage;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        stamp = 0;
        image = 0;
    }
    wchar_t path[MAX_PATH] = {0};
    GetModuleFileNameW(m, path, MAX_PATH);
    LARGE_INTEGER fsize;
    fsize.QuadPart = 0;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        GetFileSizeEx(f, &fsize);
        CloseHandle(f);
    }
    logI("module %S: %lld bytes on disk, image 0x%lX, PE timestamp 0x%08lX (for the record - "
         "nothing is gated on it)",
         name ? name : L"Grim Dawn.exe", (long long)fsize.QuadPart, image, stamp);
}

// `per` counts the confirmed rows per class, `adv` the ADVISORY rows per class (declared, whether
// or not they held), `advFailed` the advisory rows that did not hold.
void countClasses(int* per, int* adv, int* advFailed) {
    for (int c = 0; c < UT_BIND_CLASS_COUNT; ++c) per[c] = adv[c] = 0;
    per[UT_BIND_EXPORT] = g_exportCount;
    *advFailed = 0;
    for (int i = 0; i < kRowCount; ++i) {
        if (kRows[i].gate == UT_BIND_ADVISORY) {
            adv[kRows[i].cls]++;
            if (g_state[i].reported && !g_state[i].ok) ++*advFailed;
        }
        if (kRows[i].phase == UT_BIND_LATE && !g_state[i].reported) continue;
        if (!g_state[i].ok) continue;
        per[kRows[i].cls]++;
    }
}

// "13 decoded (3 advisory)" - the advisory count is appended only where there is one.
void classCount(char* out, size_t cap, int n, int adv, const char* what) {
    if (adv > 0) {
        _snprintf_s(out, cap, _TRUNCATE, "%d %s (%d advisory)", n, what, adv);
    } else {
        _snprintf_s(out, cap, _TRUNCATE, "%d %s", n, what);
    }
}

void formatSummary(bool failed, int lateOutstanding) {
    int per[UT_BIND_CLASS_COUNT], adv[UT_BIND_CLASS_COUNT], advFailed = 0;
    countClasses(per, adv, &advFailed);
    char dec[48], str[48], lit[48], tail[64];
    classCount(dec, sizeof(dec), per[UT_BIND_DECODED], adv[UT_BIND_DECODED], "decoded");
    classCount(str, sizeof(str), per[UT_BIND_STRUCTURAL], adv[UT_BIND_STRUCTURAL], "structural");
    classCount(lit, sizeof(lit), per[UT_BIND_LITERAL], adv[UT_BIND_LITERAL], "literal");
    if (failed) {
        _snprintf_s(tail, sizeof(tail), _TRUNCATE, "%s", "NOT confirmed");
    } else if (advFailed > 0) {
        _snprintf_s(tail, sizeof(tail), _TRUNCATE, "confirmed except %d advisory (WARN above)",
                    advFailed);
    } else {
        _snprintf_s(tail, sizeof(tail), _TRUNCATE, "%s", "all confirmed");
    }
    _snprintf_s(g_summary, sizeof(g_summary), _TRUNCATE,
                "bindings: %d by export, %d by signature, %s, %s, %s - %s%s", per[UT_BIND_EXPORT],
                per[UT_BIND_SIGNATURE], dec, str, lit, tail,
                lateOutstanding ? ", exe signatures pending (game thread)" : "");
}

}  // namespace

const UtBindPattern kUtSigDepositSiteA = {
    "exe.depositSiteA", kSiteABytes, kSiteAMask, sizeof(kSiteABytes), 1, 0x1EAB2A};
const UtBindPattern kUtSigDepositSiteB = {
    "exe.depositSiteB", kSiteBBytes, kSiteBMask, sizeof(kSiteBBytes), 1, 0x1EC64F};
const UtBindPattern kUtSigDragSite = {"gamedll.dragSite", kDragBytes, kDragMask,
                                      sizeof(kDragBytes), 1, 0x173960};

void bindingsNote(const char* name, unsigned long long value, bool ok, const char* why) {
    seed();
    const int i = indexOf(name);
    if (i < 0) {
        // A name that is not in the table is a mistake in the mod, not in the game. Say it once
        // and loudly rather than counting a binding nobody declared.
        logW("bindings: \"%s\" reported an outcome but is not in the table", name ? name : "(null)");
        return;
    }
    g_state[i].value = value;
    g_state[i].ok = ok;
    g_state[i].reported = true;
    _snprintf_s(g_state[i].why, sizeof(g_state[i].why), _TRUNCATE, "%s",
                why && *why ? why : kRows[i].confirm);
}

bool bindingsGate(int resolvedExports, int missingExports) {
    seed();
    g_exportCount = resolvedExports > 0 ? resolvedExports : 0;

    sayModule(nullptr);
    sayModule(L"Game.dll");
    sayModule(L"Engine.dll");

    const char* failName = nullptr;
    const char* failWhy = nullptr;
    int lateOutstanding = 0;
    for (int i = 0; i < kRowCount; ++i) {
        if (kRows[i].phase == UT_BIND_LATE) {
            if (!g_state[i].reported) ++lateOutstanding;
            continue;
        }
        if (g_state[i].reported && g_state[i].ok) continue;
        if (kRows[i].gate == UT_BIND_ADVISORY) {
            // Nothing reads this row, so it may not switch the mod off: one WARN, said once.
            if (!g_state[i].warned) {
                g_state[i].warned = true;
                logW("bindings: the advisory binding \"%s\" could not be confirmed - expected %s. "
                     "Nothing in the mod reads it, so the mod stays ON.",
                     kRows[i].name,
                     g_state[i].reported ? g_state[i].why : "it never reported an outcome");
            }
            continue;
        }
        if (!failName) {
            failName = kRows[i].name;
            failWhy = g_state[i].reported ? g_state[i].why : "it never reported an outcome";
        }
    }
    if (missingExports > 0 && !failName) {
        failName = "(required exports)";
        failWhy = "every required symbol must resolve by name";
    }
    g_failName = failName ? failName : "";

    formatSummary(failName != nullptr, lateOutstanding);
    logI("%s", g_summary);

    if (logWants(UT_LOG_DEBUG)) {
        for (int i = 0; i < kRowCount; ++i) {
            logD("  binding %-26s %-10s %-5s %-8s value=0x%llX  how: %s  confirmed by: %s",
                 kRows[i].name, className(kRows[i].cls),
                 kRows[i].phase == UT_BIND_LATE ? "late" : "early",
                 kRows[i].gate == UT_BIND_ADVISORY ? "advisory" : "critical", g_state[i].value,
                 kRows[i].how,
                 g_state[i].reported ? g_state[i].why : "NOT REPORTED YET");
        }
    }

    if (failName) {
        logE("the mod is OFF: the binding \"%s\" could not be confirmed - expected %s. Nothing is "
             "hooked, no tab is drawn, no deposit is taken: this game is not the shape this build "
             "knows and a half-bound mod must never touch a save.",
             failName, failWhy ? failWhy : "(no reason recorded)");
        return false;
    }
    return true;
}

void bindingsLateReport() {
    seed();
    for (int i = 0; i < kRowCount; ++i) {
        if (kRows[i].phase != UT_BIND_LATE) continue;
        if (!g_state[i].reported) return;  // still waiting for the rest
        if (!g_state[i].ok) {
            if (InterlockedExchange(&g_lateSaid, 1)) return;
            logE("bindings: the exe binding \"%s\" failed - expected %s. Its route is OFF for this "
                 "session; the hooks that were installed before the exe became readable stay "
                 "installed and keep passing straight through.",
                 kRows[i].name, g_state[i].why[0] ? g_state[i].why : kRows[i].confirm);
            return;
        }
    }
    if (InterlockedExchange(&g_lateSaid, 1)) return;
    formatSummary(false, 0);
    logI("%s", g_summary);
}

const char* bindingsSummary() { return g_summary; }

const char* bindingsGateFailure() { return g_failName; }

int bindingsRowCount() { return kRowCount; }

bool bindingsRowAt(int i, const char** name, int* cls, int* phase, int* gate) {
    if (i < 0 || i >= kRowCount) return false;
    if (name) *name = kRows[i].name;
    if (cls) *cls = (int)kRows[i].cls;
    if (phase) *phase = (int)kRows[i].phase;
    if (gate) *gate = (int)kRows[i].gate;
    return true;
}

}  // namespace ut
