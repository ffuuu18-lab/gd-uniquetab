// ut_bindings.cpp - the registry behind ut_bindings.h: every binding's outcome in one table, one
// INFO line, and the all-or-nothing gate that runs before a single hook is installed.

#include "ut_bindings.h"

#include <stdio.h>
#include <string.h>

#include <string>

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
// The accepted return window (sig+6 .. sig+0x1B) is byte-for-byte rva 0x1EAB30..45 on 1.3.0.8.
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
//   E8 ?? ?? ?? ??           call <the reagent add>     ; returns to sig+0x12 = 0x173972 on 1.3.0.8
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
    // -- the text engine: every row ADVISORY, a miss turns only its own route off -----------------
    {"text.renderWide", UT_BIND_EXPORT, UT_BIND_EARLY,
     "GraphicsCanvas::RenderText2d(.., unsigned short const*, ..) by name",
     "a non-null address in Engine.dll", UT_BIND_ADVISORY},
    {"text.loadFontDirect", UT_BIND_EXPORT, UT_BIND_EARLY,
     "Singleton<StyleManager>::Get + StyleManager::LoadFontDirect by name",
     "both non-null in Engine.dll", UT_BIND_ADVISORY},
    {"text.fontMetrics", UT_BIND_EXPORT, UT_BIND_EARLY,
     "GraphicsFont2::GetStyle / GetMetric / GetKerningAmount by name",
     "all three non-null in Engine.dll", UT_BIND_ADVISORY},
    {"text.localization", UT_BIND_EXPORT, UT_BIND_EARLY,
     "LocalizationManager Instance / GetLanguage / IsEnglish / GetSpecialFontFolder / "
     "LocalizeWithoutParams / TagFoundInMap by name",
     "all six non-null in Engine.dll", UT_BIND_ADVISORY},
    {"font.styleSize", UT_BIND_DECODED, UT_BIND_EARLY,
     "GetStyle's twice-called finder: sub eax,[rdi+d32] then mov edx,[rdi+d32+4]",
     "0x10..0x1000 and 4-aligned; the per-world cross-check proves the sizes", UT_BIND_ADVISORY},
    {"fntMetric.fields", UT_BIND_LITERAL, UT_BIND_EARLY,
     "literals +0x02 xOffset / +0x04 right bearing / +0x06 w",
     "per world: the root font's styles and ASCII glyphs must equal the compiled table, or the "
     "live measure stays off",
     UT_BIND_ADVISORY},
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
    // -- EXPORT, late: DirectInput.dll's device (the screen pad's input, ut_plate.cpp). Late not
    //    for DRM but because the cross-check needs the LIVE device, which exists once the engine
    //    has initialised its input; a failure turns only the screen pad's input off. ------------
    {"dinput.getMouseEvent", UT_BIND_EXPORT, UT_BIND_LATE,
     "DirectInputDevice::GetMouseEvent and ::Update by decorated name in DirectInput.dll",
     "Engine::GetInputDevice()'s vtable slot +0x38 == GetMouseEvent and +0x10 == Update; the "
     "export's own imul names the 0x1C-byte MouseEvent the detour rewrites"},
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

// WHICH WINDOWS THIS IS. ntdll exports wine_get_version only on Wine (Proton, a Steam Deck,
// CrossOver); on Windows the name is simply absent and this logs nothing. Nothing is gated on it -
// it exists so a report from a player who runs the game through Wine says so in its first lines,
// where the runtime differences that matter (the C++ exception tables, the fault dispatch) are
// otherwise invisible.
void sayHost() {
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return;
    typedef const char*(__cdecl * PfnWineGetVersion)(void);
    typedef void(__cdecl * PfnWineGetHostVersion)(const char**, const char**);
    const PfnWineGetVersion getVersion =
        (PfnWineGetVersion)GetProcAddress(ntdll, "wine_get_version");
    if (!getVersion) return;
    const char* version = nullptr;
    const char* sysname = nullptr;
    const char* release = nullptr;
    __try {
        version = getVersion();
        const PfnWineGetHostVersion getHost =
            (PfnWineGetHostVersion)GetProcAddress(ntdll, "wine_get_host_version");
        if (getHost) getHost(&sysname, &release);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        version = nullptr;
    }
    logI("host: WINE %s on %s %s - not Windows (for the record - nothing is gated on it)",
         version ? version : "(version unreadable)", sysname ? sysname : "?",
         release ? release : "?");
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

// ---- the text engine --------------------------------------------------------------------------
// Resolved here, not in gd_runtime.cpp: every name is ADVISORY, so a miss must never reach the
// required-export count, and the rows report their own outcome before the gate reads them.
enum TextExp {
    TX_RENDER_W = 0,
    TX_SM_GET,
    TX_LOAD_DIRECT,
    TX_GET_STYLE,
    TX_GET_METRIC,
    TX_GET_KERNING,
    TX_LOC_INSTANCE,
    TX_LOC_LANGUAGE,
    TX_LOC_ISENGLISH,
    TX_LOC_FOLDER,
    TX_LOC_LOCALIZE,
    TX_LOC_TAGFOUND,
    TX_COUNT
};

UtTextApi g_text = {};
UtTextLanguage g_lang = {0, -1, "", ""};
volatile LONG g_textTried = 0;
bool g_langTried = false;

typedef void*(__cdecl* PfnTxVoidPtr)();
typedef const void*(__cdecl* PfnTxLoadFontDirect)(void* styleManager, const void* stdString);
typedef const void*(__cdecl* PfnTxGetStyle)(const void* font, unsigned size, int flag);
typedef const void*(__cdecl* PfnTxGetMetric)(const void* font, const void* style,
                                              unsigned short cp);
typedef const void*(__cdecl* PfnTxLocString)(const void* loc);
typedef bool(__cdecl* PfnTxLocBool)(const void* loc);

// `need` bytes at an rva of the loaded module whose base is ctx, inside its image; else null.
const unsigned char* atInModule(unsigned long rva, size_t need, void* ctx) {
    const unsigned char* base = (const unsigned char*)ctx;
    if (!base) return nullptr;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    if ((unsigned long long)rva + need > nt->OptionalHeader.SizeOfImage) return nullptr;
    return base + rva;
}

unsigned long decodeStyleSizeSeh(unsigned long getStyleRva, void* base, unsigned long* finder) {
    __try {
        return utDecodeStyleSizeOffset(getStyleRva, atInModule, base, finder);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool rowAllResolved(void* const* p, const char* row) {
    bool any = false;
    for (int i = 0; i < kUtTextExportCount; ++i) {
        if (strcmp(kUtTextExports[i].row, row) != 0) continue;
        any = true;
        if (!p[i]) return false;
    }
    return any;
}

bool textRowsReported() {
    for (int i = 0; i < kRowCount; ++i) {
        if ((strncmp(kRows[i].name, "text.", 5) == 0 || strcmp(kRows[i].name, "font.styleSize") == 0) &&
            g_state[i].reported)
            return true;
    }
    return false;
}

void resolveText(HMODULE engine) {
    void* p[TX_COUNT] = {};
    const unsigned char* base = (const unsigned char*)engine;
    for (int i = 0; i < kUtTextExportCount && i < TX_COUNT; ++i) {
        p[i] = engine ? (void*)GetProcAddress(engine, kUtTextExports[i].mangled) : nullptr;
        logT("  %-14s %-38s -> %p (text, advisory)", "Engine.dll", kUtTextExports[i].pretty, p[i]);
    }
    const char* rows[] = {"text.renderWide", "text.loadFontDirect", "text.fontMetrics",
                          "text.localization"};
    for (int r = 0; r < 4; ++r) {
        unsigned long long first = 0;
        const char* missing = nullptr;
        for (int i = 0; i < kUtTextExportCount; ++i) {
            if (strcmp(kUtTextExports[i].row, rows[r]) != 0) continue;
            if (!p[i] && !missing) missing = kUtTextExports[i].pretty;
            if (p[i] && !first) first = (unsigned long long)((const unsigned char*)p[i] - base);
        }
        char why[160];
        if (missing) {
            _snprintf_s(why, sizeof(why), _TRUNCATE, "%s is not exported by Engine.dll", missing);
        } else {
            _snprintf_s(why, sizeof(why), _TRUNCATE, "%s", "every name resolved in Engine.dll");
        }
        bindingsNote(rows[r], first, missing == nullptr, why);
    }
    unsigned long off = 0, finder = 0;
    if (p[TX_GET_STYLE]) {
        const unsigned long rva = (unsigned long)((const unsigned char*)p[TX_GET_STYLE] - base);
        off = decodeStyleSizeSeh(rva, (void*)engine, &finder);
    }
    char why[160];
    _snprintf_s(why, sizeof(why), _TRUNCATE,
                off ? "Style+0x%lX from the finder at Engine.dll+0x%lX" :
                      "the finder's sub eax,[rdi+d32] / flags pair was not found (+0x%lX, 0x%lX)",
                off, finder);
    bindingsNote("font.styleSize", off, off != 0, why);

    g_text.renderText2dW = rowAllResolved(p, "text.renderWide") ? p[TX_RENDER_W] : nullptr;
    const bool fontRow = rowAllResolved(p, "text.loadFontDirect");
    g_text.styleManagerGet = fontRow ? p[TX_SM_GET] : nullptr;
    g_text.loadFontDirect = fontRow ? p[TX_LOAD_DIRECT] : nullptr;
    const bool metricRow = rowAllResolved(p, "text.fontMetrics");
    g_text.fontGetStyle = metricRow ? p[TX_GET_STYLE] : nullptr;
    g_text.fontGetMetric = metricRow ? p[TX_GET_METRIC] : nullptr;
    g_text.fontGetKerning = metricRow ? p[TX_GET_KERNING] : nullptr;
    const bool locRow = rowAllResolved(p, "text.localization");
    g_text.locInstance = locRow ? p[TX_LOC_INSTANCE] : nullptr;
    g_text.locGetLanguage = locRow ? p[TX_LOC_LANGUAGE] : nullptr;
    g_text.locIsEnglish = locRow ? p[TX_LOC_ISENGLISH] : nullptr;
    g_text.locSpecialFolder = locRow ? p[TX_LOC_FOLDER] : nullptr;
    g_text.locLocalize = locRow ? p[TX_LOC_LOCALIZE] : nullptr;
    g_text.locTagFound = locRow ? p[TX_LOC_TAGFOUND] : nullptr;
    g_text.styleSizeOff = off;
    g_text.draw = g_text.renderText2dW != nullptr;
    g_text.font = fontRow;
    g_text.metrics = metricRow && off != 0;
    g_text.loc = locRow;
    logD("text engine: wide draw %s, language font %s, live metrics %s, language %s",
         g_text.draw ? "bound" : "OFF", g_text.font ? "bound" : "OFF",
         g_text.metrics ? "bound" : "OFF", g_text.loc ? "bound" : "OFF");
}

// MSVC std::string (16-byte SSO, size, capacity) into `out`; an empty string is valid here.
bool readStdString(const void* s, char* out, size_t cap) {
    out[0] = 0;
    if (!s) return false;
    const unsigned char* b = (const unsigned char*)s;
    const size_t size = *(const size_t*)(b + 16);
    const size_t res = *(const size_t*)(b + 24);
    if (size > 0x4000 || size > res) return false;
    const char* data = res < 16 ? (const char*)b : *(const char* const*)b;
    if (size && !data) return false;
    size_t n = size < cap - 1 ? size : cap - 1;
    memcpy(out, data, n);
    out[n] = 0;
    return true;
}

const void* loadFontDirectSeh(const void* stdString) {
    __try {
        void* sm = ((PfnTxVoidPtr)g_text.styleManagerGet)();
        if (!sm) return nullptr;
        return ((PfnTxLoadFontDirect)g_text.loadFontDirect)(sm, stdString);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void readLanguageSeh(UtTextLanguage* out) {
    __try {
        const void* loc = ((PfnTxVoidPtr)g_text.locInstance)();
        if (!loc) return;
        const bool a = readStdString(((PfnTxLocString)g_text.locGetLanguage)(loc), out->language,
                                     sizeof(out->language));
        const bool b = readStdString(((PfnTxLocString)g_text.locSpecialFolder)(loc), out->folder,
                                     sizeof(out->folder));
        out->english = ((PfnTxLocBool)g_text.locIsEnglish)(loc) ? 1 : 0;
        out->known = (a && b) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out->known = 0;
    }
}

}  // namespace

const UtTextExport kUtTextExports[] = {
    {"text.renderWide",
     "?RenderText2d@GraphicsCanvas@GAME@@QEAAXHHAEBVColor@2@PEBGPEBVGraphicsFont2@2@HW4GraphicsXAlign@2@W4GraphicsYAlign@2@W4FontStyleFlag@2@W4FontLayout@2@@Z",
     "GraphicsCanvas::RenderText2d(u16)", 0x9F930},
    {"text.loadFontDirect", "?Get@?$Singleton@VStyleManager@GAME@@@GAME@@SAPEAVStyleManager@2@XZ",
     "Singleton<StyleManager>::Get", 0x7B480},
    {"text.loadFontDirect",
     "?LoadFontDirect@StyleManager@GAME@@QEAAPEBVGraphicsFont2@2@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z",
     "StyleManager::LoadFontDirect", 0x1C3EE0},
    {"text.fontMetrics", "?GetStyle@GraphicsFont2@GAME@@QEBAPEBUStyle@12@IW4FontStyleFlag@2@@Z",
     "GraphicsFont2::GetStyle", 0xB0720},
    {"text.fontMetrics", "?GetMetric@GraphicsFont2@GAME@@QEBAPEBUFNTMetric@12@PEBUStyle@12@G@Z",
     "GraphicsFont2::GetMetric", 0xB08E0},
    {"text.fontMetrics", "?GetKerningAmount@GraphicsFont2@GAME@@QEBAHPEBUStyle@12@GG@Z",
     "GraphicsFont2::GetKerningAmount", 0xB0A20},
    {"text.localization", "?Instance@LocalizationManager@GAME@@SAAEAV12@XZ",
     "LocalizationManager::Instance", 0x143500},
    {"text.localization",
     "?GetLanguage@LocalizationManager@GAME@@QEBAAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@XZ",
     "LocalizationManager::GetLanguage", 0xDF60},
    {"text.localization", "?IsEnglish@LocalizationManager@GAME@@QEBA_NXZ",
     "LocalizationManager::IsEnglish", 0x147960},
    {"text.localization",
     "?GetSpecialFontFolder@LocalizationManager@GAME@@QEBAAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@XZ",
     "LocalizationManager::GetSpecialFontFolder", 0x147970},
    {"text.localization", "?LocalizeWithoutParams@LocalizationManager@GAME@@QEAAPEBGPEBD@Z",
     "LocalizationManager::LocalizeWithoutParams", 0x143700},
    {"text.localization", "?TagFoundInMap@LocalizationManager@GAME@@QEAA_NPEBD@Z",
     "LocalizationManager::TagFoundInMap", 0x145F50},
};
const int kUtTextExportCount = (int)(sizeof(kUtTextExports) / sizeof(kUtTextExports[0]));

const UtTextApi& bindingsText() { return g_text; }

const void* bindingsTextLoadFontDirect(const char* path) {
    if (!g_text.font || !path || !*path) return nullptr;
    const std::string p(path);
    return loadFontDirectSeh(&p);
}

bool bindingsTextStyle(const void* font, int size, const void** style, int* styleSize) {
    if (!g_text.metrics || !font || size < 1 || !style || !styleSize) return false;
    __try {
        const void* st = ((PfnTxGetStyle)g_text.fontGetStyle)(font, (unsigned)size, 0);
        if (!st) return false;
        const int ss = *(const int*)((const unsigned char*)st + g_text.styleSizeOff);
        if (ss < 1 || ss > 400) return false;
        *style = st;
        *styleSize = ss;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool bindingsTextGlyph(const void* font, const void* style, unsigned cp, int* xOff, int* rb,
                       int* w) {
    if (!g_text.metrics || !font || !style || cp > 0xFFFF) return false;
    __try {
        const unsigned char* m = (const unsigned char*)((PfnTxGetMetric)g_text.fontGetMetric)(
            font, style, (unsigned short)cp);
        if (!m) return false;
        *xOff = (int)*(const short*)(m + 2);
        *rb = (int)*(const short*)(m + 4);
        *w = (int)*(const unsigned short*)(m + 6);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const UtTextLanguage& bindingsTextLanguage() {
    if (g_langTried) return g_lang;
    g_langTried = true;
    if (g_text.loc) readLanguageSeh(&g_lang);
    return g_lang;
}

namespace {
typedef bool(__cdecl* PfnTxTagFound)(void* loc, const char* tag);
typedef const wchar_t*(__cdecl* PfnTxLocalize)(void* loc, const char* tag);

// Copies at most cap-1 UTF-16 units of the engine's text for `tag` into `w`; 0 = none.
size_t localizeSeh(const char* tag, wchar_t* w, size_t cap) {
    __try {
        void* loc = ((PfnTxVoidPtr)g_text.locInstance)();
        if (!loc || !((PfnTxTagFound)g_text.locTagFound)(loc, tag)) return 0;
        const wchar_t* s = ((PfnTxLocalize)g_text.locLocalize)(loc, tag);
        size_t n = 0;
        while (s && s[n] && n + 1 < cap) {
            w[n] = s[n];
            ++n;
        }
        w[n] = 0;
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
}  // namespace

bool bindingsTextLocalize(const char* tag, char* out, size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = 0;
    if (!g_text.loc || !tag || !*tag) return false;
    wchar_t w[256];
    const size_t n = localizeSeh(tag, w, sizeof(w) / sizeof(w[0]));
    if (n == 0) return false;
    const int k = WideCharToMultiByte(CP_UTF8, 0, w, (int)n, out, (int)cap - 1, nullptr, nullptr);
    if (k <= 0) {
        out[0] = 0;
        return false;
    }
    out[k] = 0;
    return true;
}

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
    // The text engine reports its own rows first (once; a harness that has already noted them
    // keeps its outcomes).
    if (!InterlockedExchange(&g_textTried, 1) && !textRowsReported()) {
        resolveText(GetModuleHandleW(L"Engine.dll"));
    }

    sayModule(nullptr);
    sayModule(L"Game.dll");
    sayModule(L"Engine.dll");
    sayHost();

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
            logE("bindings: the late binding \"%s\" failed - expected %s. Its route is OFF for "
                 "this session; the hooks that were installed before it was read stay installed "
                 "and keep passing straight through.",
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
