// ut_plate.cpp - the captured material ReagentWindow and everything hung off it: the plate
// texture swap, the visible-page accessor (the CaravanWindow scan), the ReagentWindow::Draw /
// mouse / UpdateSearch detours read out of the live vtable, the owned snapshot (the engine's
// reagent map plus the mod's private table) and the category-button search marks.
//
// See ut_plate.h for the disassembly and the ownership argument.  Invariants: the ONLY engine
// memory this file writes is the plate pointer at window+0x4B0, and only while the caravan is
// open on a window whose shape has been proved; every mod LoadTexture is balanced by exactly
// one UnloadTexture at world teardown; a detour never installs a hook from inside an engine
// detour body; and every session kill switch is mod-owned, never a g_cfg field.

#include "ut_plate.h"

#include <intrin.h>   // _AddressOfReturnAddress for the CaravanWindow scan
#include <stdio.h>
#include <string.h>

#include <string>
#include <unordered_set>
#include <vector>

#include "MinHook.h"
#include "gd_exports.h"
#include "gd_exports_reagent.h"
#include "gd_runtime.h"
#include "ut_config.h"
#include "ut_live.h"
#include "ut_bindings.h"
#include "ut_log.h"
#include "ut_paths.h"
#include "ut_ownedfold.h"  // the owned fold's arithmetic, tested offline
#include "ut_panel.h"
#include "ut_reagent.h"
#include "ut_rescue.h"   // journalItemName, for tier C
#include "ut_store.h"    // storeTableOwns() - the ONE gate
#include "ut_textfold.h" // the shared case fold, tested offline

namespace ut {
namespace {

// ---- ReagentWindow::Load -----------------------------------------------------------------
// exe 0x00132100 .. 0x0013249E. The prologue up to the first RIP-relative instruction, so the
// pattern carries no relocated bytes. Verified with tools/dis_img.py: exactly one hit in .text.
const unsigned char kSigWndLoad[] = {
    0x48, 0x8B, 0xC4, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8B, 0xEC, 0x48, 0x83, 0xEC, 0x70,
    0x48, 0xC7, 0x45, 0xB0, 0xFE, 0xFF, 0xFF, 0xFF, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70,
    0x18, 0x48, 0x89, 0x78, 0x20, 0x0F, 0x29, 0x70, 0xD8, 0x48, 0x8B, 0xDA, 0x48, 0x8B, 0xF1};

const size_t kSlotWndLoad = 0x18;      // ReagentWindow's own vtable slot for Load
// Verified against the decrypted exe image: the ReagentWindow
// vtable at exe rva 0x3142B8 reads +0x10 = 0x131E30, +0x18 = 0x132100 (Load), +0x20 = 0x133440
// (Draw), +0x38 = 0x1328D0.  Only the SLOT NUMBER is compiled in - the address is read out of
// the live captured window and cross-checked against the located Load.
const size_t kSlotWndDraw = 0x20;      // ReagentWindow::Draw
const size_t kWinOriginOff = 0x40;     // + 0x44: the origin Draw adds to the caller's Vec2
const size_t kPlateWidgetOff = 0x480;  // the inline BitmapSingle widget (its vtable lives here)
const size_t kPlateTexOff = 0x4B0;     // window + 0x480 (BitmapSingle) + 0x30 (GraphicsTexture*)
const size_t kPlateWOff = 0x4C0;       // + 0x40 : destination width  (frozen at HUD build)
const size_t kPlateHOff = 0x4C4;       // + 0x44 : destination height
// The vector ReagentWindow::Load's box loop push_backs into. Its element stride was measured
// from the exe instead of assuming a vector of bare pointers:
//   0x132389  add rsi,0x380              ; rsi = &window->boxes  ([rsi]/[rsi+8]/[rsi+0x10])
//   0x1323C4  mov [rbp-0x48],rdi         ; element.widget = the new UIReagentItem*
//   0x1323D3  mov eax,[rdi+0x30] ; mov [rbp-0x40],eax ; mov byte [rbp-0x3C],1
//   0x13240F  and rdi,~0xF               ; the aliasing fixup rounds to an ELEMENT boundary
//   0x13243B  movups [rax],xmm0          ; 16 bytes copied in
//   0x13243E  add qword [rsi+8],0x10     ; _Mylast += 16
// so each element is 16 bytes - {UIReagentItem* widget; u32 id; bool} - with the widget pointer
// at offset 0, and there are (last - first) / 16 of them.
const size_t kBoxVecOff = 0x380;
const size_t kBoxVecStride = 16;
const int kMaxBoxes = 8192;            // sanity ceiling for that vector (the frame builds 160)

// The CaravanWindow. Pages are pointers at caravan+0x13C8[4]
// and caravan+0x1728 is the visible page index; the material ReagentWindow is page 3.
const size_t kCaravanPagesOff = 0x13C8;
const size_t kCaravanPageIdxOff = 0x1728;
const int kMaterialPage = 3;
const size_t kHudCaravanOff = 0x4FD08;   // the CaravanWindow is EMBEDDED in the Hud at this offset
const size_t kStackScanBytes = 0x800;    // how far up the caller's frame the `this` search goes
// UIReagentItem/ReagentWindow widget geometry.
const size_t kWidgetParentOriginOff = 0x80;  // + 0x84: the parent origin the engine adds
const size_t kWidgetLocalPosOff = 0x6C;      // + 0x70: the widget's own local position

typedef void(__cdecl* PfnWndLoad)(void* window, const void* stdString);
PfnWndLoad p_WndLoad = nullptr;
PfnWndLoad o_WndLoad = nullptr;

// Draw(this, GraphicsCanvas*, Vec2 const* offset) -> void (plain `ret` at
// 0x133807; the second argument is passed straight through as `this` to
// GraphicsCanvas::RenderRect at 0x1337D7, which is what proves its type).
typedef void(__cdecl* PfnWndDraw)(void* window, void* canvas, const void* offset);
PfnWndDraw p_WndDraw = nullptr;
PfnWndDraw o_WndDraw = nullptr;

// The ReagentWindow's own mouse handler, vtable slot +0x38 (exe 0x1328D0).
// `bool f(this, InputDevice::MouseEvent const*, Vec2 const* offset, void** out)` - all four
// arguments are forwarded verbatim on every straight-through call: `out` is written by the
// original at
// exe 0x133104 with the hovered box and consumed by the CaravanWindow at 0x135E23.
// The caller DISCARDS the return value, so "consuming" a click means
// simply not calling the original.
const size_t kSlotWndMouse = 0x38;
typedef bool(__cdecl* PfnWndMouse)(void* window, const void* evt, const float* offset, void** out);
PfnWndMouse p_WndMouse = nullptr;
PfnWndMouse o_WndMouse = nullptr;
// MouseEvent (0x1C bytes): int type @+0, float x @+4, float y @+8.
// type 0 = MOVE (arrives every input tick, unconditionally), 1 = LEFT DOWN, 2 = RIGHT DOWN,
// 9 = LEFT UP, 0x11/0x12 = wheel.  ONLY type 1 may ever be consumed.
const int kMouseTypeMove = 0;
const int kMouseTypeLeftDown = 1;

// ---- state ---------------------------------------------------------------------------------
void* g_window = nullptr;              // the MATERIAL ReagentWindow
const void* g_origPlate = nullptr;     // its plate pointer as the engine built it
int g_origW = 0, g_origH = 0;          // the FROZEN DRAW RECT: the record size times the UI scale
int g_origTexW = 0, g_origTexH = 0;    // the vanilla plate TEXTURE's own size
const void* g_curPlate = nullptr;      // what the mod last wrote (or g_origPlate)
const void* g_wantPlate = nullptr;     // what the last plateApply() decided on
void* g_caravan = nullptr;             // the CaravanWindow that owns the window
volatile LONG g_stashOpen = 0;         // the last IsTransferOpen seen

volatile LONG g_codeOk = 0;
volatile LONG g_disabled = 0;
volatile LONG g_disabledWorld = 0;   // the disable belongs to one world and is re-armed at its teardown
volatile LONG g_scanTries = 0;
volatile LONG g_swaps = 0;
volatile LONG g_faults = 0;
// plateInstall() has TWO callers in ut_live.cpp (`liveInstall`'s deferred branch and its
// success `return 1 + plateInstall(total)`), and the game-thread late scan in plateTick() is a
// third.  The late scan and liveInstall's late-scan pass can call it within milliseconds of
// each other; MinHook answers the second with MH_ERROR_ALREADY_CREATED (3), which must not be
// treated as a hard failure (a hook that IS live would be declared dead and the whole feature
// disabled).  These two flags make the call
// idempotent, and MH_ERROR_ALREADY_CREATED / MH_ERROR_ENABLED are now benign by name.
volatile LONG g_hookOn = 0;     // the detour is created AND enabled
volatile LONG g_deferLogged = 0;
const char* g_why = "not initialised";
char g_status[640] = "plate: idle";
// The heartbeat's own buffer - plateSearchStatus() is the only thing the worker calls in this
// file, and it must not share a buffer with plateStatus().
char g_searchStatus[192] = "search: idle";

// The ReagentWindow::Draw detour (route 1 of the label).
void* g_pendingDraw = nullptr;        // read from the vtable, install on the next game tick
volatile LONG g_drawHookOn = 0;
volatile LONG g_drawOff = 0;          // 1 = route 1 is unavailable for the rest of the session
volatile LONG g_drawFaults = 0;
volatile LONG g_labelDraws = 0;       // labels emitted from inside ReagentWindow::Draw
LONG g_routeLogged = 0;               // per-world once-flag for the "label: ..." route line
bool g_drawAnchorLogged = false;      // per-world cross-check of the two window origins
// The very first frame after a HUD build always reads page index 0, so "Materials is visible"
// must be an EDGE the mod has observed, never
// the initial state.  -2 = nothing read yet this world.
int g_lastPage = -2;
volatile LONG g_pageEdgeSeen = 0;     // 1 once a read really returned kMaterialPage

// ---- the click route ------------------------------------------------------------------------
void* g_pendingMouse = nullptr;        // read from the vtable, installed on the next game tick
volatile LONG g_mouseHookOn = 0;
volatile LONG g_btnProbes = 0;         // probe lines emitted this world (capped at 20)
volatile LONG g_btnSpaceProven = 0;    // a probe showed |delta| <= 2 px on both axes
volatile LONG g_btnSpaceLogged = 0;    // the ENABLED / REFUSED line, once per world
volatile LONG g_btnPassLogged = 0;     // bitmask: which straight-through reason was logged
volatile LONG g_drawOriginOk = 0;      // the draw origin below has been computed this world
float g_lastDrawOx = 0.0f;             // the origin the engine's own Draw computed, game thread
float g_lastDrawOy = 0.0f;

// ui/caravan/caravan_transfercomponent1_bg.tex, the plate the material window loads for itself.
// BitmapSingle::SetBitmap froze the widget's draw rectangle from THAT size at HUD build, so a
// replacement of any other size would be stretched into it: a plate that does not measure this
// is refused, and the plain transfer cover image is used instead.
const int kPlateW = 438;
const int kPlateH = 627;

struct PlateTex {
    int cellW = 0;
    int cellH = 0;
    // TWO candidates, tried in this order.
    //   modPath - "<the mod folder>\plates\uniq_plate_WxH.tex", a full path, handed to
    //             GraphicsEngine::LoadTexture as it is. The engine's FileSystem::OpenFile takes a
    //             plain char path, so a full one has a real chance of being opened directly - and
    //             if it is, the plates are served OUT OF THE MOD FOLDER and nothing has to be
    //             copied into the user's Documents folder at all.
    //   path    - "ui/caravan/uniq_plate_WxH.tex", the engine-relative name, which the resource
    //             system resolves against its own roots (the loose-file override folder under
    //             Documents\My Games\Grim Dawn\Settings is where the install step puts them
    //             today). THE DOCUMENTED FALLBACK: an installation that already has them there
    //             keeps working exactly as it did, with one extra failed lookup per plate size,
    //             once per session.
    // THE THIRD OPTION, NOT TAKEN, AND WHY - for whoever picks this up next. Engine.dll exports
    // `Engine::GetFileSystem`, and FileSystem exports `AddSource(Partition, string const&, char
    // const*, bool, bool, bool)` and `AddSourceArchive(Partition, string const&, char const*,
    // bool)` - i.e. the engine can be told at run time to mount another loose-file directory or
    // another .arc, which would make "ui/caravan/uniq_plate_*.tex" resolve out of the mod folder
    // for good, the way uniq-pages.arz is already layered onto the database. It was NOT done
    // here: the Partition enum value and the four trailing arguments are not knowable from the
    // export name, they would have to be read off one of the engine's own AddSource call sites,
    // and a wrong argument to a function that rebuilds the file system's search order is the kind
    // of mistake that cannot be undone from inside a frame. The full-path attempt above needs no
    // guess at all - a path the engine cannot open simply returns null - so it is what ships, and
    // the mount stays on the table with its evidence written down.
    std::string modPath;
    std::string path;
    bool fromModFolder = false;   // which of the two actually produced the texture
    const GdTexture* tex = nullptr;
    int w = 0;
    int h = 0;
    int wantW = 0;   // the size `ok` was decided against, so a different want re-tests
    int wantH = 0;
    bool tried = false;
    bool ok = false;
};
std::vector<PlateTex>* g_plates = nullptr;
// The module handle plateInit was given, so the plate loader can ask ut_paths.h where the
// mod folder is. It is the only thing this file uses it for.
HMODULE g_self = nullptr;
const GdTexture* g_fallback = nullptr;   // the plain transfer cover image
bool g_fallbackTried = false;

void disable(const char* why) {
    if (InterlockedExchange(&g_disabled, 1)) return;
    g_why = why;
    logE("plate: DISABLED for this session - %s", why);
}

// The same stop, for a reason that belongs to ONE world: the window this world built is not the
// ReagentWindow the mod knows (a custom game can build an empty frame). The next world gets its
// own window and its own answer, so plateOnWorldTeardown() re-arms this one. A detour that could
// not be installed is not this: that stays off for the session.
void disableThisWorld(const char* why) {
    if (InterlockedExchange(&g_disabled, 1)) return;   // already off, for a reason of its own
    InterlockedExchange(&g_disabledWorld, 1);
    g_why = why;
    logE("plate: DISABLED for this world - %s", why);
}

// The MinHook status by name: the number alone ("FAILED (3)") is not readable in a log.
const char* mhName(MH_STATUS s) {
    switch (s) {
        case MH_OK: return "MH_OK";
        case MH_ERROR_ALREADY_INITIALIZED: return "MH_ERROR_ALREADY_INITIALIZED";
        case MH_ERROR_NOT_INITIALIZED: return "MH_ERROR_NOT_INITIALIZED";
        case MH_ERROR_ALREADY_CREATED: return "MH_ERROR_ALREADY_CREATED";
        case MH_ERROR_NOT_CREATED: return "MH_ERROR_NOT_CREATED";
        case MH_ERROR_ENABLED: return "MH_ERROR_ENABLED";
        case MH_ERROR_DISABLED: return "MH_ERROR_DISABLED";
        case MH_ERROR_NOT_EXECUTABLE: return "MH_ERROR_NOT_EXECUTABLE";
        case MH_ERROR_UNSUPPORTED_FUNCTION: return "MH_ERROR_UNSUPPORTED_FUNCTION";
        case MH_ERROR_MEMORY_ALLOC: return "MH_ERROR_MEMORY_ALLOC";
        case MH_ERROR_MEMORY_PROTECT: return "MH_ERROR_MEMORY_PROTECT";
        case MH_ERROR_MODULE_NOT_FOUND: return "MH_ERROR_MODULE_NOT_FOUND";
        case MH_ERROR_FUNCTION_NOT_FOUND: return "MH_ERROR_FUNCTION_NOT_FOUND";
        default: return "MH_UNKNOWN";
    }
}

// ---- signature scan ---------------------------------------------------------------------
bool textRange(const unsigned char** lo, const unsigned char** hi) {
    HMODULE exe = GetModuleHandleW(nullptr);
    if (!exe) return false;
    const unsigned char* base = (const unsigned char*)exe;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (memcmp(sec[i].Name, ".text", 5) == 0) {
            *lo = base + sec[i].VirtualAddress;
            *hi = *lo + sec[i].Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

bool resolveCode() {
    const unsigned char *lo = nullptr, *hi = nullptr;
    if (!textRange(&lo, &hi)) return false;
    const LONG t = InterlockedIncrement(&g_scanTries);
    const unsigned char* hit = nullptr;
    long count = 0;
    for (const unsigned char* p = lo; p + sizeof(kSigWndLoad) <= hi; ++p) {
        if (p[0] != kSigWndLoad[0]) continue;
        if (memcmp(p, kSigWndLoad, sizeof(kSigWndLoad)) != 0) continue;
        if (!hit) hit = p;
        if (++count > 1) break;
    }
    if (count != 1) {
        if (t <= 2 || count > 1) {
            logD("plate: signature ReagentWindow::Load matched %ld times on attempt %ld "
                 "(need exactly 1)", count, t);
        }
        // Only a FINAL failure is worth reporting to the bindings table - the exe's .text is
        // encrypted until the Steam stub has run, so the first attempts legitimately match zero
        // times and must not be counted as a broken binding.
        if (t >= 40) {
            bindingsNote("exe.ReagentWindowLoad", 0, false,
                         "exactly one match in the exe .text after 40 attempts");
        }
        return false;
    }
    bindingsNote("exe.ReagentWindowLoad", (unsigned long long)(ULONG_PTR)hit, true,
                 "exactly one match in the exe .text");
    p_WndLoad = (PfnWndLoad)hit;
    logD("plate: signature ReagentWindow::Load -> %p (exe rva 0x%llX), unique", (const void*)hit,
         (unsigned long long)(hit - (const unsigned char*)GetModuleHandleW(nullptr)));
    return true;
}

// ---- SEH-guarded engine touches --------------------------------------------------------
bool checkWindowVtable(void* window) {
    __try {
        const void* const* vt = *(const void* const**)window;
        if (!vt) return false;
        const void* fn = vt[kSlotWndLoad / 8];
        if (fn != (const void*)p_WndLoad) {
            logD("plate: VTABLE MISMATCH window=%p slot 0x18=%p vs signature %p", window, fn,
                 (void*)p_WndLoad);
            return false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logD("plate: fault reading the ReagentWindow vtable");
        return false;
    }
    return true;
}

// ---- ReagentWindow::Draw, read out of the captured window's vtable --------------------------
void drawOff(const char* why) {
    if (InterlockedExchange(&g_drawOff, 1)) return;
    logD("label: the ReagentWindow::Draw route is OFF for this session - %s. The label falls "
         "back to the PresentSurface draw with cursor-in-box suppression.", why);
}

// Reads BOTH slots in one SEH frame: +0x18 has to be the Load we located by signature (that is
// the cross-check that makes +0x20 trustworthy without a second byte signature) and +0x20 is
// the Draw candidate.
// Slot +0x38 (the mouse handler) comes out of the SAME frame, so the +0x18
// cross-check covers it too.
bool readVtableSlots(void* window, const void** load, const void** draw, const void** mouse) {
    __try {
        const void* const* vt = *(const void* const**)window;
        if (!vt) return false;
        *load = vt[kSlotWndLoad / 8];
        *draw = vt[kSlotWndDraw / 8];
        *mouse = vt[kSlotWndMouse / 8];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_drawFaults);
        return false;
    }
}

// The same frame, for ONE arbitrary slot - the +0x118 UpdateSearch candidate.
// The +0x18 cross-check comes out of the same read, so it covers the candidate exactly the way it
// covers +0x20 and +0x38.
bool readVtableSlot(void* window, size_t slot, const void** load, const void** fn) {
    __try {
        const void* const* vt = *(const void* const**)window;
        if (!vt) return false;
        *load = vt[kSlotWndLoad / 8];
        *fn = vt[slot / 8];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_drawFaults);
        return false;
    }
}

// A STRUCTURAL self-check, independent of the capture epoch, run before the
// first pointer write ever reaches this window.
//   * window + 0x480 must hold a vtable pointer inside the exe image whose slot 0 points into
//     the exe's .text - i.e. it really is the inline BitmapSingle the ctor built at 0x131D6B;
//   * window + 0x380 must be a well-formed std::vector<T*> of a plausible size;
//   * and that vector must CONTAIN the first box UIReagentItem::Load handed us during this very
//     Load call, which is what proves this window owns the collection page.
// Only the last one can fail while everything else is right (live paging off, no capture), so
// it is enforced only when a box really was captured.
struct WindowShape {
    const void* widgetVt;
    int vecCount;
    int captured;
    bool member;
    bool ok;
};

bool imageRange(const unsigned char** lo, const unsigned char** hi) {
    HMODULE exe = GetModuleHandleW(nullptr);
    if (!exe) return false;
    const unsigned char* base = (const unsigned char*)exe;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    *lo = base;
    *hi = base + nt->OptionalHeader.SizeOfImage;
    return true;
}

bool shapeProbe(void* window, const unsigned char* tlo, const unsigned char* thi,
                const unsigned char* ilo, const unsigned char* ihi, void* const* boxes,
                int captured, WindowShape* out) {
    __try {
        const unsigned char* p = (const unsigned char*)window;
        const void* vt = *(const void* const*)(p + kPlateWidgetOff);
        out->widgetVt = vt;
        if (!vt || ((ULONG_PTR)vt & 7) != 0 || (const unsigned char*)vt < ilo ||
            (const unsigned char*)vt >= ihi) {
            return true;   // probe succeeded, the shape did not
        }
        const unsigned char* slot0 = *(const unsigned char* const*)vt;
        if (slot0 < tlo || slot0 >= thi) return true;

        const unsigned char* const* v = (const unsigned char* const*)(p + kBoxVecOff);
        const unsigned char* first = v[0];
        const unsigned char* last = v[1];
        const unsigned char* end = v[2];
        if (!first || last < first || end < last) return true;
        const ULONG_PTR bytes = (ULONG_PTR)(last - first);
        if ((bytes % kBoxVecStride) != 0 || bytes > (ULONG_PTR)kMaxBoxes * kBoxVecStride) {
            return true;
        }
        out->vecCount = (int)(bytes / kBoxVecStride);
        if (captured > 0 && boxes) {
            for (int i = 0; i < out->vecCount; ++i) {
                if (*(const void* const*)(first + (size_t)i * kBoxVecStride) == boxes[0]) {
                    out->member = true;
                    break;
                }
            }
        }
        out->ok = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool checkWindowShape(void* window) {
    const unsigned char *tlo = nullptr, *thi = nullptr, *ilo = nullptr, *ihi = nullptr;
    if (!textRange(&tlo, &thi) || !imageRange(&ilo, &ihi)) {
        logD("plate: cannot read the exe section table - the structural check is inconclusive");
        return false;
    }
    void* boxes[256];
    int captured = liveCapturedBoxes(boxes, 256);
    if (captured > 256) captured = 256;

    WindowShape s;
    s.widgetVt = nullptr;
    s.vecCount = -1;
    s.captured = captured;
    s.member = false;
    s.ok = false;
    if (!shapeProbe(window, tlo, thi, ilo, ihi, boxes, captured, &s)) {
        InterlockedIncrement(&g_faults);
        logD("plate: STRUCT fault reading window=%p (+0x480 widget / +0x380 box vector)", window);
        return false;
    }
    logD("plate: struct check window=%p widget-vtable=%p boxvec=%d captured=%d firstBoxInVec=%s",
         window, s.widgetVt, s.vecCount, captured, s.member ? "yes" : "no");
    if (!s.ok) {
        logD("plate: STRUCT MISMATCH - window+0x480 is not the inline BitmapSingle or "
             "window+0x380 is not a box vector");
        return false;
    }
    if (captured > 0 && !s.member) {
        logD("plate: STRUCT MISMATCH - the boxes we captured are not in this window's vector");
        return false;
    }
    return true;
}

bool readPlateFields(void* window, const void** tex, int* w, int* h) {
    __try {
        const unsigned char* p = (const unsigned char*)window;
        *tex = *(const void* const*)(p + kPlateTexOff);
        *w = (int)*(const float*)(p + kPlateWOff);
        *h = (int)*(const float*)(p + kPlateHOff);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_faults);
        return false;
    }
}

bool writePlate(void* window, const void* tex) {
    __try {
        *(const void**)((unsigned char*)window + kPlateTexOff) = tex;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_faults);
        return false;
    }
}

// ---- the CaravanWindow and the visible page -------------------------------------------------
// One candidate probe, SEH-only (no C++ object may share a frame with __try). Every one of the
// four tests has to pass, so a stack value that merely happens to be a heap pointer is rejected.
bool caravanCandidate(const unsigned char* p, const void* window, int* pageOut) {
    __try {
        const void* const* pages = (const void* const*)(p + kCaravanPagesOff);
        if (pages[kMaterialPage] != window) return false;
        const void* comp = pages[2];
        if (!comp || ((ULONG_PTR)comp & 7) != 0) return false;
        if (!pages[0] || !pages[1]) return false;
        const void* const* vt = *(const void* const* const*)comp;
        if (!vt) return false;
        if (vt[kSlotWndLoad / 8] != (const void*)p_WndLoad) return false;
        const int idx = *(const int*)(p + kCaravanPageIdxOff);
        if (idx < 0 || idx > 3) return false;
        if (pageOut) *pageOut = idx;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The caller of ReagentWindow::Load is CaravanWindow::Load, so its `this` (or the Hud's, with the
// CaravanWindow embedded at +0x4FD08) is spilled somewhere in the frames ABOVE our return-address
// slot. Bounded, upward only - never into an uncommitted page below the current stack pointer.
void* scanForCaravan(const unsigned char* frameLo, void* window, int* pageOut) {
    const ULONG_PTR guardLo = (ULONG_PTR)frameLo - 0x100000;
    const ULONG_PTR guardHi = (ULONG_PTR)frameLo + kStackScanBytes + 0x100000;
    // Every candidate read is a deliberate, SEH-handled probe, so the whole scan sits inside
    // the mod's probe scope - the vectored handler stays silent for it instead of logging the
    // probes as crashes.
    void* found = nullptr;
    reagentProbeEnter();
    for (size_t off = 0; off + sizeof(void*) <= kStackScanBytes; off += sizeof(void*)) {
        void* v = *(void* const*)(frameLo + off);
        const ULONG_PTR u = (ULONG_PTR)v;
        if (!v || (u & 7) != 0) continue;
        if (u < 0x10000 || u > 0x7FFFFFFFFFFFull) continue;
        if (u >= guardLo && u <= guardHi) continue;           // a pointer back into the stack
        if (caravanCandidate((const unsigned char*)v, window, pageOut)) { found = v; break; }
        void* h = (unsigned char*)v + kHudCaravanOff;         // v was the Hud, not the caravan
        if (caravanCandidate((const unsigned char*)h, window, pageOut)) { found = h; break; }
    }
    reagentProbeLeave();
    return found;
}

bool readPageIndex(const void* caravan, int* out) {
    __try {
        const int idx = *(const int*)((const unsigned char*)caravan + kCaravanPageIdxOff);
        if (idx < 0 || idx > 3) return false;
        *out = idx;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_faults);
        return false;
    }
}

// The window's live rect in screen pixels. `box0` (any captured box) carries the parent origin
// the engine itself adds to every box's local position, which is the one
// number that is certainly right; the window's own +0x80/+0x6C pair is read too and logged once
// so the next session can confirm they agree.
bool readWindowRect(void* window, void* box0, float* out) {
    __try {
        const unsigned char* w = (const unsigned char*)window;
        const float* wo = (const float*)(w + kWidgetParentOriginOff);
        const float* wl = (const float*)(w + kWidgetLocalPosOff);
        float ox = wo[0] + wl[0];
        float oy = wo[1] + wl[1];
        out[4] = ox;
        out[5] = oy;
        if (box0) {
            const float* bo = (const float*)((const unsigned char*)box0 + kWidgetParentOriginOff);
            ox = bo[0];
            oy = bo[1];
        }
        out[0] = ox;
        out[1] = oy;
        out[2] = *(const float*)(w + kPlateWOff);
        out[3] = *(const float*)(w + kPlateHOff);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_faults);
        return false;
    }
}

// Route 2: the box's LIVE hit rect is box+0x6C/+0x70 (local x,y, written by
// SetLocalPosition) and +0x74/+0x78 (w,h, written by SetTexture) - the exact four floats the
// engine's own hit loop reads at exe 0x1329D0..0x132A14.  Screen space is that plus the parent
// origin at +0x80/+0x84.  The 136 filler boxes are parked at -4000,-4000
// and a cleared box has w/h 0, so neither can ever match.
bool boxHitSeh(void* const* boxes, int n, float fx, float fy) {
    __try {
        for (int i = 0; i < n; ++i) {
            const unsigned char* b = (const unsigned char*)boxes[i];
            if (!b) continue;
            const float* o = (const float*)(b + kWidgetParentOriginOff);
            const float* r = (const float*)(b + kWidgetLocalPosOff);
            const float w = r[2], h = r[3];
            if (!(w > 0.0f && h > 0.0f) || w > 20000.0f || h > 20000.0f) continue;
            const float x = o[0] + r[0], y = o[1] + r[1];
            if (fx >= x && fy >= y && fx < x + w && fy < y + h) return true;
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_faults);
        return false;
    }
}

// The same rectangles in the engine's own WINDOW-LOCAL space. The point the
// ReagentWindow's mouse handler computes (evt - offset - window+0x40/+0x44) is compared with
// box+0x6C/+0x70 (the local position SetLocalPosition wrote, UI scale already inside it) and
// +0x74/+0x78 directly - the parent origin is NOT added, because the engine already subtracted
// it. A fault sets *faulted; the caller then passes the event through rather than guessing.
bool boxHitLocalSeh(void* const* boxes, int n, float lx, float ly, bool* faulted) {
    __try {
        for (int i = 0; i < n; ++i) {
            const unsigned char* b = (const unsigned char*)boxes[i];
            if (!b) continue;
            const float* r = (const float*)(b + kWidgetLocalPosOff);
            const float w = r[2], h = r[3];
            if (!(w > 0.0f && h > 0.0f) || w > 20000.0f || h > 20000.0f) continue;
            if (lx >= r[0] && ly >= r[1] && lx < r[0] + w && ly < r[1] + h) return true;
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_faults);
        if (faulted) *faulted = true;
        return false;
    }
}

// ---- texture loading ------------------------------------------------------------------
GdGraphicsEngine* graphics() {
    if (!g_gd.ppEngine || !g_gd.EngineGetGraphicsEngine) return nullptr;
    GdEngine* e = *g_gd.ppEngine;
    return e ? g_gd.EngineGetGraphicsEngine(e) : nullptr;
}

// Loaded once and kept for the life of the process: the engine's own texture cache owns the
// object, and unloading one the widget might still be pointing at is exactly the crash we are
// paid to avoid.
// The probe below runs at the main menu, against seven paths that may not exist at all if
// install_plates.ps1 was never run - so both engine calls get an
// SEH frame of their own (a C++ `catch (...)` does not catch an access violation under /EHsc).
// Neither helper may hold a C++ object with a destructor: __try and object unwinding cannot
// share a function (C2712), which is why the std::string stays in the caller.
const GdTexture* loadTexSeh(GdGraphicsEngine* gfx, const std::string* p) {
    __try {
        return g_gd.GfxLoadTexture(gfx, p);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool texSizeSeh(const GdTexture* t, int* w, int* h) {
    __try {
        *w = g_gd.TextureGetWidth(t);
        *h = g_gd.TextureGetHeight(t);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *w = *h = 0;
        return false;
    }
}

const GdTexture* loadOnce(const char* path, int* w, int* h) {
    *w = *h = 0;
    GdGraphicsEngine* gfx = graphics();
    if (!gfx || !g_gd.GfxLoadTexture || !path || !*path) return nullptr;
    const GdTexture* t = nullptr;
    try {
        const std::string p(path);
        t = loadTexSeh(gfx, &p);
    } catch (...) {
        return nullptr;
    }
    if (!t) return nullptr;
    if (g_gd.TextureGetWidth && g_gd.TextureGetHeight && !texSizeSeh(t, w, h)) {
        InterlockedIncrement(&g_faults);
        logD("plate: fault measuring \"%s\" (tex=%p) - treated as unusable", path,
             (const void*)t);
        return nullptr;
    }
    return t;
}

// Every mod LoadTexture is balanced by exactly one UnloadTexture at world teardown - except
// the one texture the engine's own ReagentWindow destructor unloaded already, which is passed
// in as `engineOwns` and skipped.
typedef void(__cdecl* PfnGfx_UnloadTexture)(GdGraphicsEngine*, const GdTexture*);
PfnGfx_UnloadTexture p_UnloadTexture = nullptr;

bool unloadTexSeh(GdGraphicsEngine* gfx, const GdTexture* t) {
    __try {
        p_UnloadTexture(gfx, t);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const GdTexture* fallbackPlate() {
    if (!g_fallbackTried) {
        g_fallbackTried = true;
        int w = 0, h = 0;
        g_fallback = loadOnce("ui/caravan/caravan_transfercoverimage.tex", &w, &h);
        logD("plate: fallback \"ui/caravan/caravan_transfercoverimage.tex\" -> %p %dx%d",
             (const void*)g_fallback, w, h);
        if (g_fallback && w <= 0) g_fallback = nullptr;
    }
    return g_fallback;
}

PlateTex* plateFor(int cellW, int cellH) {
    if (!g_plates) return nullptr;
    for (size_t i = 0; i < g_plates->size(); ++i) {
        if ((*g_plates)[i].cellW == cellW && (*g_plates)[i].cellH == cellH) return &(*g_plates)[i];
    }
    PlateTex p;
    p.cellW = cellW;
    p.cellH = cellH;
    char buf[128];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "ui/caravan/uniq_plate_%dx%d.tex", cellW, cellH);
    p.path = buf;
    // The mod-folder candidate. utModFile is the ONE resolver (it knows about the Documents
    // fallback and about %UNIQUETAB_OUT%), and it answers whether the file is actually there - so
    // a missing plates folder costs no LoadTexture call at all.
    char leaf[128];
    char full[MAX_PATH];
    _snprintf_s(leaf, sizeof(leaf), _TRUNCATE, "plates\\uniq_plate_%dx%d.tex", cellW, cellH);
    if (utModFile(g_self, leaf, full, sizeof(full))) p.modPath = full;
    try {
        g_plates->push_back(p);
    } catch (...) {
        return nullptr;
    }
    return &g_plates->back();
}

// One LoadTexture per plate, once. Called from the game thread; at the MAIN MENU as soon as the
// group file and the signature are in, so the whole install can be verified without a character.
// The LOAD happens once, but the ACCEPTANCE is recomputed every time the wanted size changes:
// `ok` cached from the menu probe's 438x627 would be wrong for an in-game call that compares
// against a different size.
bool ensurePlate(PlateTex* p, int wantW, int wantH) {
    if (!p) return false;
    if (!p->tried) {
        if (!graphics()) return false;   // no graphics engine yet: try again next tick
        p->tried = true;
        // The mod folder first. GraphicsEngine::LoadTexture resolves names through the engine's
        // resource roots; given a full path it does not fail, it hands back a 64x64 placeholder,
        // so the only acceptance test that means anything is the plate's own size. Anything else
        // is dropped and the engine-relative name is tried - that one resolves through the
        // override roots (<game>\settings\ui\caravan, Documents\My Games\Grim Dawn\Settings),
        // which is where the install step puts the plates.
        if (!p->modPath.empty()) {
            p->tex = loadOnce(p->modPath.c_str(), &p->w, &p->h);
            p->fromModFolder = p->tex != nullptr && p->w == wantW && p->h == wantH;
            if (!p->fromModFolder) {
                p->tex = nullptr;
                p->w = 0;
                p->h = 0;
            }
        }
        if (!p->tex) p->tex = loadOnce(p->path.c_str(), &p->w, &p->h);
        p->wantW = 0;
        p->wantH = 0;
    }
    if (p->wantW == wantW && p->wantH == wantH) return p->ok;
    p->wantW = wantW;
    p->wantH = wantH;
    p->ok = p->tex != nullptr && p->w == wantW && p->h == wantH;
    logD("plate: LoadTexture \"%s\" (%s) -> tex=%p %dx%d (need %dx%d) - %s",
         p->fromModFolder ? p->modPath.c_str() : p->path.c_str(),
         p->fromModFolder ? "the mod folder's own plates\\ - nothing was copied into Documents"
                          : "the engine's own resource roots",
         (const void*)p->tex, p->w, p->h, wantW, wantH,
         p->ok ? "OK" : "REJECTED, the plain transfer cover image is used for these groups");
    return p->ok;
}

volatile LONG g_probed = 0;

void probeAllPlates() {
    if (InterlockedCompareExchange(&g_probed, 0, 0)) return;
    if (!graphics()) return;
    int cw[16], chh[16];
    const int n = liveCellSizes(cw, chh, 16);
    if (n <= 0) return;               // the group file is not loaded yet
    InterlockedExchange(&g_probed, 1);
    int ok = 0;
    const int nn = n < 16 ? n : 16;
    for (int i = 0; i < nn; ++i) {
        PlateTex* p = plateFor(cw[i], chh[i]);
        if (ensurePlate(p, kPlateW, kPlateH)) ++ok;
    }
    int fromMod = 0;
    for (int i = 0; i < nn; ++i) {
        const PlateTex* p = plateFor(cw[i], chh[i]);
        if (p && p->fromModFolder) ++fromMod;
    }
    logD("plate: probed %d generated plate(s), %d usable, %d served out of the mod folder's own "
         "plates\\ folder, the rest through the engine's override roots (<game>\\settings\\ui\\"
         "caravan\\, where the install step puts them).",
         nn, ok, fromMod);
    fallbackPlate();
}

// ---- the plate is installed ONLY while the caravan is open ----------------------------------
// `ReagentWindow::~` runs the embedded plate widget's destructor, which
// UnloadTexture()s WHATEVER pointer sits at window+0x4B0 - the mod's texture if the mod left it
// there - while the vanilla pointer it displaced is never unloaded.  plateOnWorldTeardown()
// cannot repair that: it is reached from liveBeginCapture(), i.e. during the NEXT HUD build,
// when the old window is already freed and writing into it is exactly the fault the liveness
// rules exist to prevent.  So the invariant is moved earlier: our texture goes in when the
// caravan opens and the ENGINE's own texture goes back the moment it closes, on the game
// thread, while the HUD is certainly alive.  A teardown then finds the vanilla pointer in place
// and unloads its own texture, as it did before the mod existed.
void plateAssertPlate() {
    // The caravan-open flag is refreshed FIRST and unconditionally: it is the game-thread
    // snapshot plateMaterialsVisible() reads, so the message thread never touches the engine.
    GdGameEngine* ge = panelGameEngine();
    const bool open = ge && g_gd.GameIsTransferOpen && g_gd.GameIsTransferOpen(ge);
    const LONG was = InterlockedExchange(&g_stashOpen, open ? 1 : 0);
    // The very first frame after every HUD build
    // reads "page index at +0x1728 = 0", so "Materials is the visible page" is only ever true
    // after this game-thread poll has SEEN the index become 3.  Nothing arms on the initial
    // state, and every page change is on the record.
    if (g_caravan) {
        int idx = -1;
        if (readPageIndex(g_caravan, &idx)) {
            if (idx != g_lastPage) {
                const int prev = g_lastPage;
                g_lastPage = idx;
                if (idx == kMaterialPage) InterlockedExchange(&g_pageEdgeSeen, 1);
                logT("plate: caravan page edge %d -> %d (%s)%s", prev, idx,
                     idx == kMaterialPage ? "Crafting Materials - the collection page"
                                          : "another caravan page",
                     prev == -2 ? " - first read of this HUD" : "");
            }
        }
    }
    if (InterlockedCompareExchange(&g_disabled, 0, 0)) return;
    if (!g_window || !g_origPlate) return;
    const void* want = (open && g_cfg.plateSwap && g_wantPlate) ? g_wantPlate : g_origPlate;
    if (want == g_curPlate) return;
    if (!writePlate(g_window, want)) {
        disable("writing window+0x4B0 faulted");
        return;
    }
    const void* prev = g_curPlate;
    g_curPlate = want;
    logT("plate: %s window+0x4B0 %p -> %p (caravan %s%s)",
         want == g_origPlate ? "restored the VANILLA plate into" : "installed the mod plate into",
         prev, want, open ? "open" : "closed",
         (was ? 1 : 0) == (open ? 1 : 0) ? "" : ", edge");
}

// ---- the reagent map walk (owned records) ----------------------------------------------
struct ReagentNode {
    ReagentNode* left;
    ReagentNode* parent;
    ReagentNode* right;
    unsigned char color;
    unsigned char isnil;
};
const unsigned int kNodeKeyOff = 0x20;   // std::string key, same layout ut_reagent.cpp documents
const unsigned int kNodeIdOff = 0x40;    // ReagentData::protoId (the STORED prototype's object id)

typedef const void*(__cdecl* PfnGE_GetPlayerReagents)(const GdGameEngine*);
PfnGE_GetPlayerReagents p_GetPlayerReagents = nullptr;

// NODE PRESENCE is not ownership (a counter built on it says "2 swords" when one was taken
// back out, and keeps counting a removed relic): neither
// take path ever erases a node (`ReagentWindow` take exe 0x132BFA.., GameEngine::
// TakeItemFromReagents) - they only write the STORED PROTOTYPE's stack size down to 0 - so an
// emptied record kept its node for ever and kept being counted.  The one number the engine
// itself acts on is that prototype's live stack (Item::GetStackSize, Item+0x88C), which is
// exactly what `heldOf`/`readProtoStack` read in ut_reagent.cpp.
//
// Those helpers live in ut_reagent.cpp's ANONYMOUS namespace (internal linkage), and the
// id->Object helper they need (Game.dll 0x19D20) is not
// exported, so the same answer is taken from the engine's own exported accessor instead:
//
//   GameEngine::GetReagentItemCount(this, std::string const& record, mem::vector<u32>& out)
//   Game.dll rva 0x2CEF40, disassembled with tools/disasm.py:
//     0x2CEF65  map find on GameEngine+0x36D80          ; not found -> 0x2CF008 xor eax,eax
//     0x2CEF8A  mov ebx,[node+0x40]                     ; the stored prototype's object id
//     0x2CEF8D  ObjectManager::Get  ->  0x2CEF98 ObjectFromId(om, id)
//     0x2CEFA0  test rax,rax ; je 0x2CF008               ; the object is GONE -> 0
//     0x2CEFAB/0x2CEFC7  vtable[0] + the RTTI test 0x57BB10 ; not an Item -> 0
//     0x2CEFD6  call [vt+0x618] = Item::GetStackSize    ; 0 -> 0x2CF008 (returns 0, out
//                                                       ;      vector untouched)
//     otherwise it push_backs the id and returns GetStackSize again.
// So its return value is > 0 IF AND ONLY IF the node's stored prototype still exists, still is
// an Item and still has a stack of at least 1 - the exact test this file needs, done by the
// engine, with no exe RVA and no mod-side object resolution.  It is a READ: nothing on that
// path writes anything (the out vector is the mod's own).
typedef int(__cdecl* PfnGE_GetReagentItemCount)(const GdGameEngine*, const void* stdString,
                                                void* outVec);
PfnGE_GetReagentItemCount p_GetReagentItemCount = nullptr;

// `mem::vector<unsigned int>` = {begin, end, cap}. ONE static vector is
// reused for every query and `end` is reset to `begin` before each call, so the engine allocates
// its storage at most once per process instead of once per record per second.
struct PlateMemVecU32 {
    unsigned int* begin;
    unsigned int* end;
    unsigned int* cap;
};
PlateMemVecU32 g_countVec = {nullptr, nullptr, nullptr};

// GetReagentItemCount is an ENGINE
// call, and not a trivial accessor: it does a map find, an ObjectManager::Get + ObjectFromId,
// the RTTI test 0x57BB10, the virtual Item::GetStackSize and - on the success path - a
// mem::vector push_back that ALLOCATES from the engine's own allocator the first time (g_countVec
// starts empty).  A fault raised inside any of that must therefore be LOGGED and handed straight
// back to the engine's own handler, never swallowed: carrying on with the engine allocator in an
// unknown state is forbidden.  The fail-safe "disable the accessor
// for the session" therefore cannot live in a handler body, so it is a PRE-CALL
// latch: raised before the call, cleared after it returns.  A call that never returns leaves it
// raised, and the next walk disables the accessor and falls back to node presence.
long g_countInFlight = 0;

// MSVC std::string, built by hand - the same POD layout ut_live.cpp's LiveString uses.
struct PlateString {
    union {
        char buf[16];
        char* ptr;
    } u;
    size_t size;
    size_t capacity;
};

void makePlateString(PlateString* s, char* storage) {
    memset(s, 0, sizeof(*s));
    const size_t n = strlen(storage);
    s->size = n;
    if (n < 16) {
        memcpy(s->u.buf, storage, n + 1);
        s->capacity = 15;
    } else {
        s->u.ptr = storage;
        s->capacity = n;
    }
}

std::unordered_set<std::string>* g_owned = nullptr;
volatile LONG g_ownedTotal = 0;   // records whose stored prototype is live with stack >= 1
volatile LONG g_ownedNodes = 0;   // nodes the engine's map holds (the old, wrong, count)
volatile LONG g_ownedEmpty = 0;   // of them, emptied (taken out) or unreadable
volatile LONG g_ownedOk = 0;
DWORD g_ownedAt = 0;
// What the last refresh saw, so the log speaks only when the picture changes and so a changed
// node set / prototype id can ask for a repaint.
LONG g_ownedLoggedTotal = -1;
LONG g_ownedLoggedNodes = -1;
LONG g_ownedLoggedEmpty = -1;
unsigned long long g_mapFingerprint = 0;
bool g_mapFingerprintValid = false;
bool g_heldOffLogged = false;
// Bumped whenever that fingerprint moves. See plateOwnedGeneration().
volatile LONG g_ownedGen = 0;

// ---- the PRIVATE TABLE's half of the owned snapshot -----------------------------------------
// Published like every other counter here: interlocked LONGs the
// heartbeat thread may read, never a container.
volatile LONG g_ownedTableRows = 0;     // rows the private table holds with a count >= 1
volatile LONG g_ownedTableCopies = 0;   // the sum of those counts (a row may hold more than one)
volatile LONG g_ownedTableOnly = 0;     // of those rows, how many the engine's map does NOT hold
LONG g_ownedLoggedTableRows = -1;       // what the last line said, so the log speaks on change
LONG g_ownedLoggedTableCopies = -1;
LONG g_ownedLoggedTableOnly = -1;
bool g_tableModeLogged = false;
// The record buffer `journalRecords` copies into. GAME THREAD ONLY, allocated on the first walk
// that finds the table owning anything - so a session that stores nothing allocates nothing.
std::vector<char>* g_tableBuf = nullptr;
const int kMaxTableRows = 4096;   // 3,288 records is the whole page file; 4,096 cannot be hit

// ---- THE TRANSITION RULE, on the display side -----------------------------------------------
// Stated once for the whole mod:
//
//     a record is TABLE-OWNED   when the mod's own file holds it with a count >= 1
//     a record is MAP-OWNED     when the engine's map still holds it with a live stack >= 1
//     held(record) = tableCount(record) + mapHeld(record)
//
// `plateOwns()` is the OR of the two - a record in either place is a record the user owns and
// the label, the owned-only filter and the search sweep must all say so. The SUM is published
// separately (`plateTableCounts`) because the two halves answer different questions and a
// record that is in BOTH is legitimately two copies.
//
// The per-record POLICY answer (max_per_record) is NOT computed here and must never be:
// `reagentHeldTotal()` owns that arithmetic in one function so it cannot drift. This
// walk is a SNAPSHOT builder, it enumerates both sides by construction, and it feeds nothing but
// the display.
//
// `plateTableOwns()` is `storeTableOwns()` and `plateTableFold()`'s row loop is one
// `journalCollectStored(recs, counts, cap)` call. A row's count is NEVER derived from its stored
// mark (`"stored":true` -> 1 copy would call every row a legacy `reagents.gst` still holds
// TABLE-OWNED): `count` means copies that live ONLY in the mod's own file (ut_rescue.h), every
// legacy MAP-OWNED row is count 0, and `journalCollectStored` returns count >= 1 rows ONLY - so
// the two halves of the fold cannot disagree about who owns a record. Nothing here calls
// `storeCount()`.
bool plateTableOwns() {
    // ONE gate for the whole mod, and this is the wrapper around it. The rule is
    // `storeTableOwns()` (`ut_paintgate.h`): the journal is usable - not READ-ONLY (a
    // file format newer than this build's, whose counts mean something this build does not know)
    // and with a path to write back to. The display half asking the same function as the OWNING
    // half is what makes it impossible to fold a row the deposit/take side refuses, which is
    // exactly the way round the accident would go: the page would show a box for a record
    // nothing would let the user take back out.
    if (storeTableOwns()) {
        if (!g_tableModeLogged) {
            g_tableModeLogged = true;
            logD("owned: the owned counters ask BOTH halves (TABLE-OWNED = the mod's own file "
                 "holds the record, MAP-OWNED = the engine's map still does); a record in both "
                 "is OWNED once, and the rule is THE TABLE WINS - its count is the holding and "
                 "the map's row is not added");
        }
        return true;
    }
    if (!g_tableModeLogged) {
        g_tableModeLogged = true;
        logD("owned: storeTableOwns() says no (the journal is READ-ONLY this session - a newer "
             "file format - or it has no path) - the owned counters stay the engine's map alone, "
             "which can only UNDER-report and never invent a record you do not have");
    }
    return false;
}

// The rules this fold is made of live in `ut_ownedfold.h` - a header with no Windows, no engine
// and no global in it, so `tools\test_rowfold.cpp` links the very code that runs here (the
// `ut_rowmath.h` / `ut_textfold.h` precedent). The count never comes from the stored mark
// anywhere in this build: the format-3 upgrade gives EVERY row count 0 (ut_rescue.h), because a
// format-3 row is a MAP-OWNED row, and `journalCollectStored` hands this fold the real count.

// Folds every TABLE-OWNED row into `g_owned` (the caller has already put the
// MAP-OWNED ones there) and reports the three numbers the heartbeat publishes. `mix` is an
// ORDER-INDEPENDENT hash of the rows: the journal's own entry order is not stable across a
// re-deposit, and a fingerprint that moved for that reason alone would ask for a relayout every
// second for ever. False = the snapshot could not be taken at all, and then the caller keeps the
// map-only answer - which can only UNDER-report a record, never invent one.
bool plateTableFold(int* rows, int* copies, int* onlyTable, unsigned long long* mix) {
    *rows = 0;
    *copies = 0;
    *onlyTable = 0;
    *mix = 0;
    if (!g_owned) return false;
    try {
        // One allocation for both outputs of `journalCollectStored`: the records first, then the
        // counts, which is why the row stride is 256 + 4 and not 256. kMaxTableRows * 256 is a
        // multiple of 4, so the tail is aligned for the unsigned int array that starts there.
        if (!g_tableBuf) {
            g_tableBuf = new std::vector<char>((size_t)kMaxTableRows * (256 + sizeof(unsigned int)));
        }
    } catch (...) {
        return false;
    }
    char (*recs)[256] = (char (*)[256]) & (*g_tableBuf)[0];
    unsigned int* counts = (unsigned int*)&(*g_tableBuf)[(size_t)kMaxTableRows * 256];
    // ONE call, ONE journal lock pair for the whole walk, and `counts[i]` is format 4's REAL
    // count - copies that live ONLY in the mod's own file. Every
    // MAP-OWNED row, including every legacy `"stored":true` row, is count 0 and
    // `journalCollectStored` does not return it at all, so neither the heartbeat's `ownedTable=`
    // nor the `owned: the private table holds ...` line can claim the table holds a collection it
    // holds nothing of.
    const int n = journalCollectStored(recs, counts, kMaxTableRows);
    for (int i = 0; i < n; ++i) {
        const unsigned int count = counts[i];
        // count >= 1 is the ONLY authorisation to call a record owned: a row at count
        // 0 is HISTORY, never inventory. `journalCollectStored` already filters on it; this is
        // the invariant restated at the place that acts on it, not a second rule - a `0` here
        // would mean the two had drifted and the fold must still refuse it.
        if (!utOwnedRowIsInventory(count)) continue;
        char key[400];
        if (!utOwnedNormaliseKey(recs[i], key, sizeof(key))) continue;
        utOwnedMixRow(mix, key, count);
        ++*rows;
        *copies += (int)count;
        try {
            if (g_owned->insert(std::string(key)).second) ++*onlyTable;
        } catch (...) {
            return false;
        }
    }
    return true;
}

// -1 = this record has no display name yet (catalogue.bin has not loaded, or
// the record is not in it), 0 = the folded name does not contain the folded needle, 1 = it does.
// The same `ut_textfold.h` pair both other tiers use, so all three halves fold identically.
int plateTableNameMatch(const char* record, const char* foldedNeedle) {
    if (!record || !*record || !foldedNeedle || !*foldedNeedle) return -1;
    char name[160];
    if (!journalItemName(record, name, sizeof(name)) || !name[0]) return -1;
    try {
        std::string folded;
        utFoldUtf8(name, &folded);
        if (folded.empty()) return -1;
        return strstr(folded.c_str(), foldedNeedle) != nullptr ? 1 : 0;
    } catch (...) {
        return -1;
    }
}

bool keyLooksLikeString(const unsigned char* base, size_t* sizeOut, const char** textOut) {
    const unsigned char* p = base + kNodeKeyOff;
    const size_t size = *(const size_t*)(p + 0x10);
    const size_t cap = *(const size_t*)(p + 0x18);
    if (cap < 15 || cap > 0x4000 || size > cap) return false;
    const char* text = nullptr;
    if (cap == 15) {
        text = (const char*)p;
        size_t n = 0;
        while (n < 16 && text[n]) ++n;
        if (n != size) return false;
    } else {
        text = *(const char* const*)p;
        if (!text) return false;
        size_t n = 0;
        while (n <= size && text[n]) ++n;
        if (n != size) return false;
    }
    *sizeOut = size;
    *textOut = text;
    return true;
}

// SEH-only: collect the node pointers, no C++ objects in this frame (MSVC forbids mixing).
int collectNodesSeh(const GdGameEngine* ge, const void** nodes, int cap) {
    int found = 0;
    __try {
        const ReagentNode* const* mapObj = (const ReagentNode* const*)p_GetPlayerReagents(ge);
        if (!mapObj) return -1;
        const ReagentNode* head = *mapObj;
        if (!head || head->isnil != 1) return -1;
        const ReagentNode* stack[64];
        int sp = 0;
        int visited = 0;
        const ReagentNode* cur = head->parent;
        while ((cur && !cur->isnil) || sp > 0) {
            if (++visited > 8192) return -1;
            while (cur && !cur->isnil) {
                if (sp >= 64) return -1;
                stack[sp++] = cur;
                cur = cur->left;
            }
            const ReagentNode* n = stack[--sp];
            if (found < cap) nodes[found++] = n;
            cur = n->right;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return found;
}

bool nodeKeyCopySeh(const void* node, char* buf, size_t cap) {
    size_t size = 0;
    const char* text = nullptr;
    __try {
        if (!keyLooksLikeString((const unsigned char*)node, &size, &text)) return false;
        if (!text || !size || size >= cap) return false;
        memcpy(buf, text, size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    buf[size] = 0;
    for (size_t i = 0; i < size; ++i) {
        if (buf[i] >= 'A' && buf[i] <= 'Z') buf[i] = (char)(buf[i] - 'A' + 'a');
        if (buf[i] == '\\') buf[i] = '/';
    }
    return true;
}

// SEH only, no C++ object in the frame. 0 = unreadable / no prototype recorded.
unsigned int nodeProtoIdSeh(const void* node) {
    __try {
        return *(const unsigned int*)((const unsigned char*)node + kNodeIdOff);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// The three above walk the engine's reagent map by dereferencing whatever the nodes hold, so a
// fault in them is expected and is the probe's answer - not a crash. Each one runs inside the
// mod's probe scope, which is what tells the vectored handler to stay silent; the scope cannot go
// inside them because a __try may not share a function with anything that unwinds.
int collectNodes(const GdGameEngine* ge, const void** nodes, int cap) {
    reagentProbeEnter();
    const int n = collectNodesSeh(ge, nodes, cap);
    reagentProbeLeave();
    return n;
}

bool nodeKeyCopy(const void* node, char* buf, size_t cap) {
    reagentProbeEnter();
    const bool ok = nodeKeyCopySeh(node, buf, cap);
    reagentProbeLeave();
    return ok;
}

unsigned int nodeProtoId(const void* node) {
    reagentProbeEnter();
    const unsigned int id = nodeProtoIdSeh(node);
    reagentProbeLeave();
    return id;
}

// How many copies of `record` the collection REALLY holds, straight out of the
// engine (see the GetReagentItemCount disassembly above): >= 0 is the stored prototype's live
// stack size, -1 means the accessor is unavailable and the caller must fall back.
// SEH only - `PlateString`, `storage[]` and the ints are PODs, so no unwinding is involved.
int plateHeldOf(const GdGameEngine* ge, const char* record) {
    if (!p_GetReagentItemCount || !ge || !record || !*record) return -1;
    if (g_countInFlight) {
        // The previous call left through an exception (the filter continued the search and the
        // frame was unwound), so this accessor is never used again in this session.
        g_countInFlight = 0;
        p_GetReagentItemCount = nullptr;
        return -1;
    }
    char storage[400];
    const size_t n = strlen(record);
    if (n + 1 >= sizeof(storage)) return -1;
    memcpy(storage, record, n + 1);
    PlateString s;
    makePlateString(&s, storage);
    int c = 0;
    g_countVec.end = g_countVec.begin;   // reuse the storage, never grow it (mod-side write)
    g_countInFlight = 1;
    __try {
        c = p_GetReagentItemCount(ge, &s, &g_countVec);
    } __except (reagentLogFault("GameEngine::GetReagentItemCount (the owned counters)",
                                GetExceptionInformation()->ExceptionRecord, GetCurrentThreadId()),
                EXCEPTION_CONTINUE_SEARCH) {
        c = 0;  // never reached: the filter always continues the search
    }
    g_countInFlight = 0;
    // An absurd answer is a bad READ, not an empty record: report ONE held (node presence), so
    // a garbage value can never hide a record the user owns.  Only a clean
    // 0 - "the node's prototype is gone or its stack is 0" - stops the count.
    return (c < 0 || c > 0x10000) ? 1 : c;
}

// ---- the label, drawn at the tail of the engine's own Draw ----------------------------------
// Called ONLY after the original has returned, so the whole page - plate, boxes, overlay list -
// is already on the canvas and the tooltip layer is still to come.  No engine memory is written
// anywhere on this path; it is a draw and nothing else.
void drawLabelBody(void* window, void* canvas, const void* offset) {
    if (window != g_window) return;              // the Components ReagentWindow: untouched
    if (!canvas || !offset) return;
    // The pad does NOT inherit plate_label - a user who turns the label off still wants the
    // buttons - so the two switches are asked for separately from here down. With BOTH off
    // nothing below runs at all: the page is exactly what the engine drew.
    const bool wantLabel = g_cfg.plateLabel != 0;
    const bool wantButtons = g_cfg.groupButtons != 0;
    if (!wantLabel && !wantButtons) return;
    if (!plateMaterialsVisible()) return;        // includes "the caravan is open"
    const float* v = (const float*)offset;
    const float* wo = (const float*)((const unsigned char*)window + kWinOriginOff);
    const float ox = v[0] + wo[0];
    const float oy = v[1] + wo[1];
    if (!(ox > -4000.0f && ox < 20000.0f && oy > -4000.0f && oy < 20000.0f)) return;
    if (!g_drawAnchorLogged) {
        g_drawAnchorLogged = true;
        float rx = 0.0f, ry = 0.0f, rw = 0.0f, rh = 0.0f;
        const bool haveRect = plateWindowRect(&rx, &ry, &rw, &rh);
        GdGraphicsEngine* gfx = graphics();
        const void* engineCanvas = (gfx && g_gd.GfxGetCanvas) ? (const void*)g_gd.GfxGetCanvas(gfx)
                                                              : nullptr;
        logT("label: ReagentWindow::Draw frame - offset (%.1f,%.1f) + window+0x40 (%.1f,%.1f) = "
             "origin (%.1f,%.1f); the PresentSurface anchor says %s(%.1f,%.1f); canvas arg %p %s "
             "GraphicsEngine::GetCanvas %p",
             v[0], v[1], wo[0], wo[1], ox, oy, haveRect ? "" : "(unavailable) ", rx, ry, canvas,
             canvas == engineCanvas ? "==" : "!=", engineCanvas);
    }
    // Publish the origin the ENGINE computed for this frame. The click's
    // self-proving probe compares the mod's own client point against exactly this pair.
    g_lastDrawOx = ox;
    g_lastDrawOy = oy;
    InterlockedExchange(&g_drawOriginOk, 1);
    // The hover has to be known BEFORE the pad draws (it picks the hover state), so it is
    // computed once, here; the pad is its only consumer.
    // Unconditional: with group_buttons=0 it returns after ONE int store (no GetCursorPos, no
    // texture, no draw), and that store is what stops a stale hover from surviving the switch.
    panelButtonsHoverTick(ox, oy);
    if (wantLabel && panelDrawGroupLabel((GdCanvas*)canvas, ox, oy)) {
        InterlockedIncrement(&g_labelDraws);
        plateNoteLabelRoute(1);
    }
    // Last, so if the two ever collided the buttons would win visually - at y 27..68 against
    // the one-line label's band y 8..24 they cannot.
    if (wantButtons) panelDrawGroupButtons((GdCanvas*)canvas, ox, oy);
}

void drawLabelGuarded(void* window, void* canvas, const void* offset) {
    __try {
        drawLabelBody(window, canvas, offset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_drawFaults);
        drawOff("the label body faulted inside the Draw detour");
    }
    // ut_panel's groupLabelGuarded has its own __except INSIDE this one, so a fault in the
    // label body is consumed there and the handler above never runs; without this ask route 1
    // would stay armed and the fault would repeat every frame inside the engine's own
    // ReagentWindow::Draw.
    if (panelLabelDrawFaulted()) {
        InterlockedIncrement(&g_drawFaults);
        drawOff("the group label faulted inside ReagentWindow::Draw (reported by ut_panel)");
    }
    // The same contract for the pad - panelDrawGroupButtons' own __except is
    // INSIDE this frame and consumes the exception first, so it has to be asked for.
    if (panelButtonsDrawFaulted()) {
        InterlockedIncrement(&g_drawFaults);
        drawOff("the category strip faulted inside ReagentWindow::Draw (reported by ut_panel)");
    }
}

void __cdecl hk_WndDraw(void* window, void* canvas, const void* offset) {
    // The original ALWAYS runs first and unconditionally: this detour may add a label, it may
    // never change what the engine paints.
    if (o_WndDraw) {
        o_WndDraw(window, canvas, offset);
    } else if (p_WndDraw) {
        p_WndDraw(window, canvas, offset);
    }
    if (InterlockedCompareExchange(&g_drawOff, 0, 0)) return;
    drawLabelGuarded(window, canvas, offset);
}

// Reads the Draw address out of the window we just captured. Never called with a window that
// failed checkWindowVtable(), so slot +0x18 has already been proven once; it is re-read here so
// the two slots come out of the same SEH frame and the cross-check is exact.
void noteDrawSlot(void* window) {
    if (InterlockedCompareExchange(&g_drawOff, 0, 0)) return;
    if (InterlockedCompareExchange(&g_drawHookOn, 0, 0)) return;   // already installed
    const void* loadFn = nullptr;
    const void* drawFn = nullptr;
    const void* mouseFn = nullptr;
    if (!readVtableSlots(window, &loadFn, &drawFn, &mouseFn)) {
        drawOff("the ReagentWindow vtable could not be read");
        return;
    }
    if (loadFn != (const void*)p_WndLoad) {
        drawOff("vtable slot +0x18 is not the signature-located ReagentWindow::Load");
        return;
    }
    const unsigned char *tlo = nullptr, *thi = nullptr;
    if (!textRange(&tlo, &thi)) {
        drawOff("the exe section table could not be read");
        return;
    }
    if ((const unsigned char*)drawFn < tlo || (const unsigned char*)drawFn >= thi) {
        drawOff("vtable slot +0x20 does not lie inside the exe .text");
        return;
    }
    g_pendingDraw = (void*)drawFn;
    logD("label: ReagentWindow::Draw -> %p (exe rva 0x%llX) from the captured window's vtable "
         "slot +0x20; slot +0x18 == the located Load %p, and the target is inside .text "
         "[%p,%p) - the detour installs on the next game-thread tick",
         drawFn, (unsigned long long)((const unsigned char*)drawFn -
                                      (const unsigned char*)GetModuleHandleW(nullptr)),
         (void*)p_WndLoad, (const void*)tlo, (const void*)thi);
}

// MinHook is NEVER called from inside an engine detour body: the vtable read happens in
// hk_WndLoad, the install happens on the next plateTick(), both on the game thread.
void installDrawHook() {
    void* target = g_pendingDraw;
    if (!target) return;
    g_pendingDraw = nullptr;
    if (InterlockedCompareExchange(&g_drawOff, 0, 0)) return;
    if (InterlockedCompareExchange(&g_drawHookOn, 0, 0)) return;
    MH_STATUS s = MH_CreateHook(target, (void*)&hk_WndDraw, (void**)&o_WndDraw);
    if (s == MH_ERROR_ALREADY_CREATED) s = o_WndDraw ? MH_OK : MH_ERROR_NOT_CREATED;
    if (s != MH_OK) {
        logE("  hook %-32s FAILED at MH_CreateHook: %s (%d)", "ReagentWindow::Draw", mhName(s),
             (int)s);
        drawOff("the ReagentWindow::Draw detour could not be created");
        return;
    }
    MH_STATUS e = MH_EnableHook(target);
    if (e == MH_ERROR_ENABLED) e = MH_OK;
    if (e != MH_OK) {
        logE("  hook %-32s FAILED at MH_EnableHook: %s (%d)", "ReagentWindow::Draw", mhName(e),
             (int)e);
        drawOff("the ReagentWindow::Draw detour could not be enabled");
        return;
    }
    p_WndDraw = (PfnWndDraw)target;
    InterlockedExchange(&g_drawHookOn, 1);
    logD("  hook %-32s installed at %p (trampoline %p) - the group label is now drawn UNDER the "
         "item tooltips", "ReagentWindow::Draw", target, (void*)o_WndDraw);
}

// ---- the click ------------------------------------------------------------------------------
// `CaravanWindow`'s own slot +0x38 (exe 0x135B40) rect-tests ITSELF and then
// calls pages[caravan+0x1728]->vt[+0x38] for EVERY point inside the caravan, box or no box; the
// page's return value is discarded, so "consuming" means not calling the original.  A MOVE
// (type 0) arrives on this slot every input tick, forever - a detour that consumed anything but
// a left DOWN would eat the whole UI.
//
// The consume path is SELF-PROVING: before a single click is taken away from the engine, the
// mod compares the engine's own window-local point with its OWN client point minus the draw
// origin.  Consumption is refused for the whole world until at least one such probe agreed to
// within 2 px on both axes, because everything the strip does rests on those two spaces being
// the same one.

enum PassReason {
    PR_NONE = 0,
    PR_NOT_DRAWN,
    PR_CURSOR_ITEM,
    PR_CURSOR_FAULT,
    PR_NO_BUTTON,
    PR_IN_BOX,
    PR_BOX_FAULT,
    PR_NOT_PROVEN,
    PR_SELECT_REFUSED,
    PR_MAX
};
const char* const kPassWhy[PR_MAX] = {
    "",
    "the strip has not been painted in the last few frames",
    "an item is on the cursor - this click is the engine's deposit, never the mod's",
    "the cursor-handler read faulted, so the mod assumes an item is on the cursor",
    "the point is in the strip band but on no button",
    "the point is inside a live box's own hit rect",
    "the box hit test faulted",
    "the event space is not proven for this world yet (see the buttons(probe) lines)",
    "the live pager refused the target group, so the engine's own handler was called after all",
};

struct MouseAct {
    bool consume;
    bool probe;
    int reason;          // PassReason
    int button;
    int target;
    const char* btnLabel;   // the GROUP the button selects
    // probe fields
    int type;
    float evx, evy, offx, offy, winx, winy, lx, ly, clx, cly, dx, dy;
    int inStrip, inBox;
};

bool readMouseEvent(const void* evt, int* type, float* x, float* y) {
    __try {
        const unsigned char* e = (const unsigned char*)evt;
        *type = *(const int*)e;
        *x = *(const float*)(e + 4);
        *y = *(const float*)(e + 8);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool readMouseOrigin(void* window, const float* offset, float* ofx, float* ofy, float* wx,
                     float* wy) {
    __try {
        *ofx = offset[0];
        *ofy = offset[1];
        const float* wo = (const float*)((const unsigned char*)window + kWinOriginOff);
        *wx = wo[0];
        *wy = wo[1];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The engine's own "is an item on the cursor" test, as a pure READ: `[[window+0x30]+0x108]` is
// the cursor handler (exe 0x13294C) and its `+0x30` is the item id `PrimaryReagentActivate`
// itself resolves at exe 0x1738FF.  The virtual `vt[0x60]` call the engine makes at 0x132969 is
// deliberately NOT made - this read is the sanctioned stand-in, and calling an unnamed engine
// virtual from inside a detour body buys nothing.
// A FAULT returns false and the caller passes the event through; "empty" is never assumed.
bool readCursorItem(void* window, unsigned int* id) {
    bool ok = false;
    reagentProbeEnter();
    __try {
        const unsigned char* w = (const unsigned char*)window;
        const void* owner = *(const void* const*)(w + 0x30);
        // A NULL owner is NOT "the cursor is empty". The engine
        // dereferences [window+0x30] unconditionally at exe 0x13294C, so a 0 there is a state it
        // never expects and the mod knows nothing about - it takes the straight-through path with
        // every other unreadable answer. Only `ch == 0` legitimately means an empty cursor.
        if (owner) {
            const void* ch = *(const void* const*)((const unsigned char*)owner + 0x108);
            *id = ch ? *(const unsigned int*)((const unsigned char*)ch + 0x30) : 0u;
            ok = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    reagentProbeLeave();
    return ok;
}

const void* readPtrSeh(void* const* p) {
    __try {
        return p ? *p : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void logPassReason(int reason) {
    if (reason <= PR_NONE || reason >= PR_MAX) return;
    const LONG bit = (LONG)(1 << reason);
    if (InterlockedOr(&g_btnPassLogged, bit) & bit) return;
    logT("buttons: click passed through - %s", kPassWhy[reason]);
}

void mouseDecide(void* window, const void* evt, const float* offset, MouseAct* a) {
    if (window != g_window) return;
    if (!g_cfg.groupButtons) return;
    if (panelButtonsAreOff()) return;
    if (!evt || !offset) return;
    int type = 0;
    float ex = 0.0f, ey = 0.0f;
    if (!readMouseEvent(evt, &type, &ex, &ey)) return;
    if (type != kMouseTypeMove && type != kMouseTypeLeftDown) return;   // never touch the rest
    float ofx = 0.0f, ofy = 0.0f, wx = 0.0f, wy = 0.0f;
    if (!readMouseOrigin(window, offset, &ofx, &ofy, &wx, &wy)) return;
    // The three lines the engine itself runs at exe 0x132917..0x13294C.
    const float lx = ex - (ofx + wx);
    const float ly = ey - (ofy + wy);
    const float s = liveUiScale();
    if (!panelButtonsInBand(lx, ly, s)) return;   // outside the band: nothing to see or consume
    if (!InterlockedCompareExchange(&g_drawOriginOk, 0, 0)) return;  // no draw origin yet

    bool boxFaulted = false;
    const bool inBox = plateBoxHitLocal(lx, ly, &boxFaulted);
    const int btn = panelButtonHit(lx, ly, s);

    a->type = type;
    a->evx = ex;
    a->evy = ey;
    a->offx = ofx;
    a->offy = ofy;
    a->winx = wx;
    a->winy = wy;
    a->lx = lx;
    a->ly = ly;
    a->inStrip = btn >= 0 ? 1 : 0;
    a->inBox = inBox ? 1 : 0;

    // The mod's own client point, minus the origin the engine's Draw handed us this world.
    POINT pt = {0, 0};
    HWND h = GetActiveWindow();
    if (!h) h = GetForegroundWindow();
    const bool haveClient = h && GetCursorPos(&pt) && ScreenToClient(h, &pt);
    if (haveClient) {
        a->clx = (float)pt.x;
        a->cly = (float)pt.y;
        a->dx = ((float)pt.x - g_lastDrawOx) - lx;
        a->dy = ((float)pt.y - g_lastDrawOy) - ly;
    }

    if (haveClient && InterlockedCompareExchange(&g_btnProbes, 0, 0) < 20) {
        InterlockedIncrement(&g_btnProbes);
        a->probe = true;
        const bool agree = a->dx > -2.0f && a->dx < 2.0f && a->dy > -2.0f && a->dy < 2.0f;
        if (agree) {
            if (!InterlockedExchange(&g_btnSpaceProven, 1)) {
                InterlockedExchange(&g_btnSpaceLogged, 1);
                logD("buttons: consumption ENABLED - the event space is proven "
                     "(delta=(%.1f,%.1f) px)", a->dx, a->dy);
            }
        } else if (!InterlockedCompareExchange(&g_btnSpaceProven, 0, 0) &&
                   !InterlockedExchange(&g_btnSpaceLogged, 1)) {
            // This is NOT a per-world refusal and must not read
            // like one. A mismatch is one probe disagreeing - the sample is a LIVE GetCursorPos
            // against an event queued earlier, so a fast sweep or an alt-tab produces one - and
            // a later agreeing probe still enables consumption. Latching the mismatch instead
            // would let one transient sample kill the click for the whole world.
            logD("buttons: consumption NOT PROVEN YET - this probe's event space does not match "
                 "the draw space (delta=(%.1f,%.1f) px) - the strip stays draw-only until a "
                 "probe agrees", a->dx, a->dy);
        }
    }

    if (type != kMouseTypeLeftDown) return;   // a MOVE is only ever a probe

    if (!panelButtonsDrawnRecently()) {
        a->reason = PR_NOT_DRAWN;
        return;
    }
    unsigned int cursorId = 0;
    if (!readCursorItem(window, &cursorId)) {
        a->reason = PR_CURSOR_FAULT;
        return;
    }
    if (cursorId != 0) {
        a->reason = PR_CURSOR_ITEM;
        return;
    }
    if (btn < 0) {
        a->reason = PR_NO_BUTTON;
        return;
    }
    if (boxFaulted) {
        a->reason = PR_BOX_FAULT;
        return;
    }
    if (inBox) {
        a->reason = PR_IN_BOX;
        return;
    }
    if (!InterlockedCompareExchange(&g_btnSpaceProven, 0, 0)) {
        a->reason = PR_NOT_PROVEN;
        return;
    }
    const char* label = nullptr;
    const int target = panelButtonTarget(btn, &label);
    // kUtButtonFilterTarget is the OWN button - a legal target that selects no
    // group.  Everything below (-2 and lower) is still an index the table does not know.
    if (target < -1 && target != kUtButtonFilterTarget) {
        a->reason = PR_NO_BUTTON;
        return;
    }
    a->consume = true;
    // The consumed click is reported by its own line, so the probe line is suppressed - and the
    // probe slot is given back, or the suppressed line makes the numbering skip.
    if (a->probe) {
        a->probe = false;
        InterlockedDecrement(&g_btnProbes);
    }
    a->button = btn;
    a->target = target;
    a->btnLabel = label;
}

void mouseDecideGuarded(void* window, const void* evt, const float* offset, MouseAct* a) {
    __try {
        mouseDecide(window, evt, offset, a);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        a->consume = false;
        a->probe = false;
        a->reason = PR_NONE;
        panelButtonsOff("the mouse handler's decision path faulted");
    }
}

bool __cdecl hk_WndMouse(void* window, const void* evt, const float* offset, void** out) {
    MouseAct a;
    memset(&a, 0, sizeof(a));
    a.reason = PR_NONE;
    a.button = -1;
    a.target = -2;
    a.btnLabel = "?";
    mouseDecideGuarded(window, evt, offset, &a);
    // The event is consumed only when the write actually happened. liveSelectGroup refuses for
    // `!liveActive()`, an empty group list and an out-of-range index, and panelButtonTarget only
    // maps a button index onto a group index (a compile-time table of 26 that knows nothing
    // about how many groups actually loaded) - so a shorter catalogue, or live_pages flipped to
    // 0 inside the 500 ms "drawn recently" window, must not eat the click and log a group change
    // that never happened: the engine's own handler runs instead and the reason is logged once.
    // The OWN button toggles the owned-only filter instead of selecting a group.
    // Both writes are mod-side ints plus a relayout request - no engine call, nothing allocated -
    // so either is safe from inside this detour body, and either may REFUSE (live paging not
    // armed), in which case the engine's own handler runs after all and the reason is logged.
    const bool isFilter = a.target == kUtButtonFilterTarget;
    if (a.consume && (isFilter ? liveToggleOwnedOnly() : liveSelectGroup(a.target))) {
        panelButtonsNotePress(a.button);
        if (isFilter) {
            logI("button [%s] clicked -> the owned-only filter is now %s",
                 panelButtonTag(a.button),
                 liveOwnedOnly() ? "ON: only records you own are shown" : "OFF");
        } else {
            logI("button [%s] clicked -> group %d \"%s\"", panelButtonTag(a.button), a.target,
                 a.btnLabel);
        }
        logD("the click was consumed; the engine's own handler was not called");
        return false;   // the caller discards this either way
    }
    if (a.consume) a.reason = PR_SELECT_REFUSED;
    bool r = false;
    if (o_WndMouse) {
        r = o_WndMouse(window, evt, offset, out);
    } else if (p_WndMouse) {
        r = p_WndMouse(window, evt, offset, out);
    }
    if (a.probe) {
        logT("buttons(probe) #%ld type=%d evt=(%.1f,%.1f) offset=(%.1f,%.1f) win=(%.1f,%.1f) "
             "local=(%.1f,%.1f) client=(%.1f,%.1f) delta=(%.1f,%.1f) inStrip=%d inBox=%d "
             "out=%p ret=%d",
             InterlockedCompareExchange(&g_btnProbes, 0, 0), a.type, a.evx, a.evy, a.offx,
             a.offy, a.winx, a.winy, a.lx, a.ly, a.clx, a.cly, a.dx, a.dy, a.inStrip, a.inBox,
             readPtrSeh(out), r ? 1 : 0);
    }
    if (a.reason != PR_NONE) logPassReason(a.reason);
    return r;
}

// Same acceptance rule as noteDrawSlot, out of the same SEH frame: slot +0x18 must be the
// signature-located ReagentWindow::Load and slot +0x38 must lie inside the exe .text.
void noteMouseSlot(void* window) {
    if (!g_cfg.groupButtons) return;
    if (panelButtonsAreOff()) return;
    if (InterlockedCompareExchange(&g_mouseHookOn, 0, 0)) return;
    if (g_pendingMouse) return;
    const void* loadFn = nullptr;
    const void* drawFn = nullptr;
    const void* mouseFn = nullptr;
    if (!readVtableSlots(window, &loadFn, &drawFn, &mouseFn)) {
        panelButtonsOff("the ReagentWindow vtable could not be read for slot +0x38");
        return;
    }
    if (loadFn != (const void*)p_WndLoad) {
        panelButtonsOff("vtable slot +0x18 is not the signature-located ReagentWindow::Load");
        return;
    }
    const unsigned char *tlo = nullptr, *thi = nullptr;
    if (!textRange(&tlo, &thi)) {
        panelButtonsOff("the exe section table could not be read");
        return;
    }
    if ((const unsigned char*)mouseFn < tlo || (const unsigned char*)mouseFn >= thi) {
        panelButtonsOff("vtable slot +0x38 does not lie inside the exe .text");
        return;
    }
    g_pendingMouse = (void*)mouseFn;
}

// MinHook is NEVER called from inside a detour body: the vtable read happens in hk_WndLoad, the
// install happens on the next plateTick(), both on the game thread.
void installMouseHook() {
    void* target = g_pendingMouse;
    if (!target) return;
    g_pendingMouse = nullptr;
    if (panelButtonsAreOff()) return;
    if (InterlockedCompareExchange(&g_mouseHookOn, 0, 0)) return;
    MH_STATUS s = MH_CreateHook(target, (void*)&hk_WndMouse, (void**)&o_WndMouse);
    if (s == MH_ERROR_ALREADY_CREATED) s = o_WndMouse ? MH_OK : MH_ERROR_NOT_CREATED;
    if (s != MH_OK) {
        logE("  hook %-32s FAILED at MH_CreateHook: %s (%d)", "ReagentWindow mouse (+0x38)",
             mhName(s), (int)s);
        panelButtonsOff("the ReagentWindow mouse detour could not be created");
        return;
    }
    MH_STATUS e = MH_EnableHook(target);
    if (e == MH_ERROR_ENABLED) e = MH_OK;
    if (e != MH_OK) {
        logE("  hook %-32s FAILED at MH_EnableHook: %s (%d)", "ReagentWindow mouse (+0x38)",
             mhName(e), (int)e);
        panelButtonsOff("the ReagentWindow mouse detour could not be enabled");
        return;
    }
    p_WndMouse = (PfnWndMouse)target;
    InterlockedExchange(&g_mouseHookOn, 1);
    logD("buttons: ReagentWindow mouse slot +0x38 -> %p (exe rva 0x%llX) hooked from the live "
         "vtable (slot +0x18 == ReagentWindow::Load) - trampoline %p",
         target,
         (unsigned long long)((const unsigned char*)target -
                              (const unsigned char*)GetModuleHandleW(nullptr)),
         (void*)o_WndMouse);
}


// ---- the caravan search box, and which category buttons it marks ----------------------------
// The search on the mod's tab is ONE HUNDRED PER CENT the game's own:
// `CaravanWindow::Update` calls `ReagentWindow::UpdateSearch` (vtable slot +0x118, exe 0x1338B0)
// on all three transfer-view windows EVERY tick, hands it the already-trimmed-and-lowercased
// needle from CaravanWindow+0x1C48, and the callee sorts each box's rect into "matched"
// (window+0x4E8) or "not matched" (window+0x500) with the EXPORTED predicate `Item::SearchText`.
// A box whose item id is 0 lands in NEITHER list - which is why the owned-only filter of part 1
// needs no veto code at all and why this detour changes NOTHING the engine does:
// the original always runs, with all three arguments, and its return value is handed back.
//
// What the mod ADDS is the answer to "which OTHER groups hold a match", because the 24 groups are
// not engine pages at all and the engine's own "pages with matches" vector cannot see them (3.5).
// Three rules this code obeys:
//   * THE DETOUR BODY DOES THE MINIMUM - one guarded read of the needle, a compare, and on a
//     CHANGE a copy plus a flag. It never scans. `Item::SearchText` is an engine call that
//     MUTATES the Item (it builds the search-text cache at Item+0xC48 on first use), so by
//     non-negotiable 9 it may not sit inside a swallowing frame, and it must run from a TICK on
//     the game thread - never from inside this detour and never from the worker.
//   * THE SWEEP IS AMORTISED and EDGE-TRIGGERED: kSweepPerTick records per game-thread tick, and
//     only when the needle actually changed. The first edge of a world pays for one full tooltip
//     build per owned record; every later edge is a handful of `wstring::find` per record.
//   * THE BUTTONS STAY UNLIT until a sweep COMPLETES, a needle change mid-sweep restarts it, and
//     an empty needle clears the mask immediately.
const size_t kSlotWndSearch = 0x118;
// bool ReagentWindow::UpdateSearch(std::wstring const& needle, mem::vector<int>& pagesWithMatches)
// The caller at exe 0x1362A3 / 0x1362BD / 0x1362D7 DISCARDS the return value; it is passed back
// anyway so the detour is transparent even if a future caller does not.
typedef bool(__cdecl* PfnWndSearch)(void* window, const void* needle, void* outVec);
PfnWndSearch p_WndSearch = nullptr;
PfnWndSearch o_WndSearch = nullptr;
void* g_pendingSearch = nullptr;
volatile LONG g_searchHookOn = 0;
volatile LONG g_searchOff = 0;          // the SESSION kill switch - mod-owned, never a g_cfg field
// The two rect vectors UpdateSearch fills on the window it was called on:
// {begin,end,cap} at +0x4E8 (matched) and +0x500 (not matched). Only `matched` is read, and only
// to answer "did anything on the page the user is looking at match" - tier B.
const size_t kWndMatchedBeginOff = 0x4E8;
const size_t kWndMatchedEndOff = 0x4F0;

// MSVC `std::basic_string<unsigned short>` - UTF-16, proven by the predicate's own mangled name
// (`basic_string@G`, and `G` is `unsigned short`). Layout: 16-byte union, `_Mysize` at +0x10,
// `_Myres` at +0x18, and the buffer is INLINE while `_Myres < 8` (the engine tests exactly that
// at exe 0x315357). The mod builds one by hand for the outgoing call, the same way LiveString /
// PlateString are built for `std::string`.
struct PlateWString {
    union {
        wchar_t buf[8];
        wchar_t* ptr;
    } u;
    size_t size;
    size_t res;
};

const int kNeedleMax = 96;          // wchar_t, plus a terminator
const int kSweepPerTick = 32;       // records per game-thread tick
wchar_t g_needle[kNeedleMax + 1] = {0};   // GAME THREAD ONLY (the detour writes, the tick reads)
int g_needleLen = 0;
volatile LONG g_needleEpoch = 0;    // bumped on every CHANGE of the needle
volatile LONG g_needleChars = 0;    // its length, for the log lines and the heartbeat
volatile LONG g_needleLongLogged = 0;   // the "longer than kNeedleMax" line, once per session

LONG g_sweepEpoch = -1;             // the epoch the sweep in progress is answering
int g_sweepAt = 0;
unsigned int g_sweepMask = 0;
int g_sweepScanned = 0;
int g_sweepMatched = 0;
int g_sweepSkipped = 0;
int g_sweepTicks = 0;
DWORD g_sweepStartedAt = 0;
std::vector<std::string>* g_sweepRecs = nullptr;
volatile LONG g_maskLive = 0;       // the PUBLISHED mask: bit i = group i holds an owned match
volatile LONG g_maskValid = 0;      // 0 until a sweep has completed for the current needle
volatile LONG g_curHitMask = 0;     // tier B: the group on screen, from the engine's own answer
volatile LONG g_sweeps = 0;
volatile LONG g_searchInstalledLogged = 0;
volatile LONG g_unownedNoteLogged = 0;

// ---- tier C, the UNOWNED half of the category-button marks ----------------------------------
// Without it a category whose only matches are records you do not own is marked only while it
// is the selected category.
// Tier A answers for records the collection HOLDS (their stored prototype is a live Item, so the
// engine's own predicate can be asked). Tier B answers for the group on screen, free, out of the
// engine's own matched-rect vector. Everything else - a record you do not own, in a group you are
// not looking at - has no answer without tier C.
//
// Tier C is two halves, and the SECOND is the refinement the dig did not have:
//   * EXACT, whenever the record already has a display prototype in the live page's cache
//     (`g_protoCache`, which holds every record SHOWN at any point in this world). That object is
//     a real `Item` built from the record's own `uniq_b*` display record - the very object the
//     engine's own UpdateSearch tests when that group is on screen - so asking `Item::SearchText`
//     about it is the SAME predicate tier A and tier B use, with the same liveness triple and the
//     same CONTINUE_SEARCH fault shape.
//   * NAME ONLY, for a record that has never been on screen: a case-insensitive substring of the
//     catalogue display name. That half cannot see stat lines, affixes or components, so it
//     UNDER-reports and never over-reports - the one direction the marks may never take.
// Both are folded into the SAME 24-bit mask the draw already reads: the strip needs no second
// mask and `ut_panel.cpp` was not touched.
struct TierCRec {
    const char* box;      // the `uniq_b*` display record - the key `g_protoCache` is keyed by
    const char* item;     // the `records/items/...` path - the key catalogue.bin is keyed by
    std::string folded;   // its display name, case-folded; empty = not resolved (yet)
};
std::vector<std::vector<TierCRec> >* g_tierC = nullptr;
volatile LONG g_tierCRecords = 0;       // how many records the table holds
volatile LONG g_tierCNamed = 0;         // how many of them have a display name yet
volatile LONG g_tierCBuildLogged = 0;
int g_sweepCGroup = 0;                  // the tier-C cursor: group, then entry
int g_sweepCIndex = 0;
int g_sweepCExact = 0;                  // records answered by the EXACT half this sweep
int g_sweepCNamed = 0;                  // ... and by the NAME half
int g_sweepCSkipped = 0;                // owned (tier A already answered) or unresolvable
// Tier A's TABLE half - records in the owned set because the mod's own file
// holds them, which have no stored prototype in the engine's map to ask Item::SearchText about.
int g_sweepTableTried = 0;
int g_sweepTableNamed = 0;
int g_sweepTableMatched = 0;
// True only while `g_needleFolded` holds the fold of THIS sweep's needle. The table can start
// owning a record halfway through a sweep, so tier A must never read a fold that was made for an
// earlier needle epoch.
bool g_sweepFolded = false;
unsigned int g_sweepMaskA = 0;          // the mask as tier A left it, for the per-tier log line
std::string* g_needleFolded = nullptr;  // the needle, folded the same way. GAME THREAD ONLY.
bool g_sweepCDone = true;

// ---- case folding ---------------------------------------------------------------------------
// The rule for both sides is `ut_textfold.h` (`utFoldWide` for the engine's UTF-16 needle,
// `utFoldUtf8` for catalogue.bin's UTF-8 display name, both out as folded UTF-8 so the match is
// a plain `strstr`). It is a header on purpose: `tools\test_rowfold.cpp` links the very code
// this file runs, and nothing in it touches Windows, the engine or a global.
// The (box record, item record) index, built ONCE per process out of the loaded group file. No
// catalogue is needed for this half and no engine call is made; the two pointers are into
// `g_groups`, which is filled at load and never mutated, so they live for the process.
bool tierCEnsureTable() {
    if (g_tierC) return true;
    const int ng = liveGroupCount();
    if (ng <= 0) return false;      // uniq-groups.txt has not loaded yet
    try {
        std::vector<std::vector<TierCRec> >* t = new std::vector<std::vector<TierCRec> >();
        t->resize((size_t)ng);
        long total = 0;
        for (int g = 0; g < ng; ++g) {
            for (int i = 0;; ++i) {
                const char* box = nullptr;
                const char* item = nullptr;
                if (!liveGroupRecordAt(g, i, &box, &item)) break;
                if (!box || !box[0] || !item || !item[0]) continue;
                TierCRec r;
                r.box = box;
                r.item = item;
                (*t)[(size_t)g].push_back(r);
                ++total;
            }
        }
        g_tierC = t;
        InterlockedExchange(&g_tierCRecords, total);
        logD("search: tier C - the unowned record index is built, %ld record(s) over %d group(s) "
             "(record pointers only; display names are resolved lazily from catalogue.bin, which "
             "the worker loads on the first journal write of the session)",
             total, ng);
        return true;
    } catch (...) {
        return false;
    }
}

// The display name for one record, folded, cached in the table. Returns null while catalogue.bin
// is not loaded yet or the record is not in it - the caller then simply has no name half for that
// record this sweep and asks again on the next one. `journalItemName` reads a pointer the worker
// writes ONCE, after the Catalogue it points at is fully parsed and immutable; a null read here
// is the only race and it costs a retry, never a wrong answer.
const std::string* tierCName(TierCRec* r) {
    if (!r->folded.empty()) return &r->folded;
    char name[160];
    if (!journalItemName(r->item, name, sizeof(name)) || !name[0]) return nullptr;
    try {
        utFoldUtf8(name, &r->folded);
    } catch (...) {
        return nullptr;
    }
    if (r->folded.empty()) return nullptr;
    InterlockedIncrement(&g_tierCNamed);
    return &r->folded;
}

// `bool Item::SearchText(std::wstring const& needle)` - Game.dll 0x315300, EXPORTED. The mangled
// name is copied VERBATIM BY SCRIPT out of tools/exports/Game-x64-exports.txt (the
// gd_exports_extra.h convention); no address is compiled in.
//   0x31531A  empty needle -> false
//   0x315327  if the cache at this+0xC48 is empty, call Item::GenerateSearchText (0x314F50)
//   0x315357  loop: std::wstring::find over every cached line -> true on the first hit
// It is declared QEAA (non-const) precisely because it may BUILD that cache, so it is a MUTATING
// engine call: rule 9 forbids swallowing a fault raised inside it. The `__except` below therefore
// only LOGS and returns EXCEPTION_CONTINUE_SEARCH, exactly like plateHeldOf's, and the
// fail-safe "never call it again" is a PRE-CALL latch rather than a handler body.
#define GD_ITEM_SEARCHTEXT "?SearchText@Item@GAME@@QEAA_NAEBV?$basic_string@GU?$char_traits@G@std@@V?$allocator@G@2@@std@@@Z"
typedef bool(__cdecl* PfnItemSearchText)(void* item, const void* wstr);
PfnItemSearchText p_ItemSearchText = nullptr;
long g_searchInFlight = 0;

void searchOff(const char* why) {
    if (InterlockedExchange(&g_searchOff, 1)) return;
    InterlockedExchange(&g_maskLive, 0);
    InterlockedExchange(&g_maskValid, 0);
    InterlockedExchange(&g_curHitMask, 0);
    logW("search: FAULT - the category-button marks are OFF for this session: %s", why ? why : "?");
    logD("the game's own search on the boxes themselves is untouched");
}

// SEH only, no C++ object in this frame. -1 = unreadable, -2 = LONGER than `cap` (searchNoteNeedle
// refuses such a needle rather than truncating it), otherwise the number of wchar_t copied.
int readNeedle(const void* ws, wchar_t* out, int cap) {
    __try {
        const unsigned char* p = (const unsigned char*)ws;
        const size_t size = *(const size_t*)(p + 0x10);
        const size_t res = *(const size_t*)(p + 0x18);
        if (res > 0x100000 || size > res) return -1;
        if (size == 0) {
            out[0] = 0;
            return 0;
        }
        const wchar_t* src = (res < 8) ? (const wchar_t*)p : *(const wchar_t* const*)p;
        if (!src) return -1;
        // A needle that does not FIT is refused, never cut down to `cap`. `Item::SearchText` is
        // a substring test, so a prefix matches at least as often as the whole string: marking
        // on a truncated needle could only OVER-report, the one direction the marks may never
        // take - and two needles differing only past `cap` would compare EQUAL, so the sweep
        // would not even restart.
        if (size > (size_t)cap) return -2;
        const int n = (int)size;
        for (int i = 0; i < n; ++i) out[i] = src[i];
        out[n] = 0;
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Tier B: "did anything on the page the user is looking at match" is FREE - the
// engine has already sorted this very tick's boxes into the two rect vectors. Two guarded pointer
// reads and nothing else. Only ever asked of the MATERIAL window the mod captured.
bool readMatchedNotEmpty(void* window, bool* nonEmpty) {
    __try {
        const unsigned char* w = (const unsigned char*)window;
        const void* b = *(const void* const*)(w + kWndMatchedBeginOff);
        const void* e = *(const void* const*)(w + kWndMatchedEndOff);
        *nonEmpty = (b != e);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The FIRST object id GetReagentItemCount push_backed into the mod's own out vector - i.e. the
// node's stored prototype, already proven by the engine itself to exist, to be an Item (its RTTI
// test at 0x2CEFC7) and to carry a stack of at least 1. That proof is what makes it safe to hand
// the pointer to Item::SearchText: the mod never guesses a type.
unsigned int countVecFirstId() {
    __try {
        if (!g_countVec.begin || g_countVec.end == g_countVec.begin) return 0;
        return g_countVec.begin[0];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// THE predicate. 1 = the item matches, 0 = it does not, -1 = the call is unavailable.
// SEH only - PlateWString and the ints are PODs, so nothing unwinds in this frame.
int itemSearchText(void* item) {
    if (!p_ItemSearchText || !item || g_needleLen <= 0) return -1;
    if (g_searchInFlight) {
        // The previous call left through an exception (the filter continued the search and the
        // frame was unwound), so this accessor is never used again in this session.
        g_searchInFlight = 0;
        p_ItemSearchText = nullptr;
        return -1;
    }
    PlateWString s;
    memset(&s, 0, sizeof(s));
    s.size = (size_t)g_needleLen;
    if (g_needleLen < 8) {
        memcpy(s.u.buf, g_needle, ((size_t)g_needleLen + 1) * sizeof(wchar_t));
        s.res = 7;
    } else {
        s.u.ptr = g_needle;
        s.res = (size_t)g_needleLen;
    }
    bool r = false;
    g_searchInFlight = 1;
    __try {
        r = p_ItemSearchText(item, &s);
    } __except (reagentLogFault("Item::SearchText (the category-button search marks)",
                                GetExceptionInformation()->ExceptionRecord, GetCurrentThreadId()),
                EXCEPTION_CONTINUE_SEARCH) {
        r = false;   // never reached: the filter always continues the search
    }
    g_searchInFlight = 0;
    return r ? 1 : 0;
}

// Called from the detour body, once per window per tick. Guarded read + a compare, nothing else.
void searchNoteNeedle(const void* ws) {
    if (InterlockedCompareExchange(&g_searchOff, 0, 0)) return;
    wchar_t buf[kNeedleMax + 1];
    const int n = readNeedle(ws, buf, kNeedleMax);
    if (n == -2) {
        // More than kNeedleMax characters are typed. UNDER-report rather than
        // over-report: forget the needle, clear the marks and leave every button unmarked until
        // the box is short enough again. Idempotent - once cleared, the next tick does nothing.
        if (g_needleLen != 0 || InterlockedCompareExchange(&g_needleChars, 0, 0) != 0) {
            g_needle[0] = 0;
            g_needleLen = 0;
            InterlockedExchange(&g_needleChars, 0);
            InterlockedIncrement(&g_needleEpoch);
            InterlockedExchange(&g_maskValid, 0);
            InterlockedExchange(&g_maskLive, 0);
            InterlockedExchange(&g_curHitMask, 0);
            if (!InterlockedExchange(&g_needleLongLogged, 1)) {
                logD("search: the caravan search box holds more than %d characters - the "
                     "category buttons are left UNMARKED while it does (a needle cut down to "
                     "%d would match MORE items than the one you typed, and a mark that is "
                     "not there is better than one that is wrong)", kNeedleMax, kNeedleMax);
            }
        }
        return;
    }
    if (n < 0) {
        searchOff("the std::wstring handed to UpdateSearch could not be read");
        return;
    }
    if (n == g_needleLen &&
        (n == 0 || memcmp(buf, g_needle, (size_t)n * sizeof(wchar_t)) == 0)) {
        return;   // the same needle as last tick: this is the common case, and it costs nothing
    }
    memcpy(g_needle, buf, ((size_t)n + 1) * sizeof(wchar_t));
    g_needleLen = n;
    InterlockedExchange(&g_needleChars, (LONG)n);
    InterlockedIncrement(&g_needleEpoch);
    // The buttons must never keep a mark that belongs to an older needle.
    InterlockedExchange(&g_maskValid, 0);
    InterlockedExchange(&g_maskLive, 0);
    if (n == 0) InterlockedExchange(&g_curHitMask, 0);
}

bool __cdecl hk_WndSearch(void* window, const void* needle, void* outVec) {
    // The needle is read BEFORE the original runs: the argument is a const reference the engine
    // does not modify, and taking it first keeps the mod out of the way of the engine's own work.
    searchNoteNeedle(needle);
    // The original ALWAYS runs, with all three arguments, and its answer is handed back unchanged.
    bool r = false;
    if (o_WndSearch) {
        r = o_WndSearch(window, needle, outVec);
    } else if (p_WndSearch) {
        r = p_WndSearch(window, needle, outVec);
    }
    // Tier B, and only for OUR window: the engine has just sorted this tick's boxes, so "the page
    // on screen holds a match" is already answered. With owned_only=1 that is exactly the same
    // universe tier A sweeps; with the filter off it also sees UNOWNED boxes, which is honest -
    // the user can see the highlighted box right there - and the policy line says so.
    if (window == g_window && !InterlockedCompareExchange(&g_searchOff, 0, 0)) {
        bool hit = false;
        const int shown = liveShownGroup(nullptr);
        if (g_needleLen > 0 && shown >= 0 && shown < 24 && readMatchedNotEmpty(window, &hit) &&
            hit) {
            InterlockedExchange(&g_curHitMask, (LONG)(1u << (unsigned)shown));
        } else {
            InterlockedExchange(&g_curHitMask, 0);
        }
    }
    return r;
}

// Same acceptance rule as noteDrawSlot / noteMouseSlot, out of the same kind of SEH frame: slot
// +0x18 must be the signature-located ReagentWindow::Load and the candidate must lie inside the
// exe .text. Only the SLOT NUMBER is compiled in - never an address.
void noteSearchSlot(void* window) {
    if (!g_cfg.searchButtons) return;
    if (InterlockedCompareExchange(&g_searchOff, 0, 0)) return;
    if (InterlockedCompareExchange(&g_searchHookOn, 0, 0)) return;
    if (g_pendingSearch) return;
    const void* loadFn = nullptr;
    const void* searchFn = nullptr;
    if (!readVtableSlot(window, kSlotWndSearch, &loadFn, &searchFn)) {
        searchOff("the ReagentWindow vtable could not be read for slot +0x118");
        return;
    }
    if (loadFn != (const void*)p_WndLoad) {
        searchOff("vtable slot +0x18 is not the signature-located ReagentWindow::Load");
        return;
    }
    const unsigned char *tlo = nullptr, *thi = nullptr;
    if (!textRange(&tlo, &thi)) {
        searchOff("the exe section table could not be read");
        return;
    }
    if ((const unsigned char*)searchFn < tlo || (const unsigned char*)searchFn >= thi) {
        searchOff("vtable slot +0x118 does not lie inside the exe .text");
        return;
    }
    g_pendingSearch = (void*)searchFn;
}

// MinHook is NEVER called from inside a detour body: the vtable read happens in hk_WndLoad (or in
// plateTick), the install happens on the next plateTick, both on the game thread.
void installSearchHook() {
    void* target = g_pendingSearch;
    if (!target) return;
    g_pendingSearch = nullptr;
    if (InterlockedCompareExchange(&g_searchOff, 0, 0)) return;
    if (InterlockedCompareExchange(&g_searchHookOn, 0, 0)) return;
    if (!p_ItemSearchText) {
        searchOff("the exported Item::SearchText is not available in this Game.dll");
        return;
    }
    MH_STATUS s = MH_CreateHook(target, (void*)&hk_WndSearch, (void**)&o_WndSearch);
    if (s == MH_ERROR_ALREADY_CREATED) s = o_WndSearch ? MH_OK : MH_ERROR_NOT_CREATED;
    if (s != MH_OK) {
        logE("  hook %-32s FAILED at MH_CreateHook: %s (%d)", "ReagentWindow search (+0x118)",
             mhName(s), (int)s);
        searchOff("the ReagentWindow::UpdateSearch detour could not be created");
        return;
    }
    MH_STATUS e = MH_EnableHook(target);
    if (e == MH_ERROR_ENABLED) e = MH_OK;
    if (e != MH_OK) {
        logE("  hook %-32s FAILED at MH_EnableHook: %s (%d)", "ReagentWindow search (+0x118)",
             mhName(e), (int)e);
        searchOff("the ReagentWindow::UpdateSearch detour could not be enabled");
        return;
    }
    p_WndSearch = (PfnWndSearch)target;
    InterlockedExchange(&g_searchHookOn, 1);
    logD("search: ReagentWindow slot +0x118 (UpdateSearch) -> %p (exe rva 0x%llX, expected "
         "0x1338B0) hooked from the live vtable (slot +0x18 == the signature-located "
         "ReagentWindow::Load, and the target is inside the exe .text) - trampoline %p",
         target,
         (unsigned long long)((const unsigned char*)target -
                              (const unsigned char*)GetModuleHandleW(nullptr)),
         (void*)o_WndSearch);
}

// One sweep step. GAME THREAD, from plateTick, never from a detour body.
void searchSweepTick() {
    if (!g_cfg.searchButtons) return;
    if (InterlockedCompareExchange(&g_searchOff, 0, 0)) return;
    if (!InterlockedCompareExchange(&g_searchHookOn, 0, 0)) return;
    if (!g_window || !p_ItemSearchText) return;
    // Tier C is built. One line the first time it actually runs in a world,
    // saying which half will answer for what - the menu-time policy line cannot know how much of
    // the collection has been on screen yet.
    if (g_cfg.searchButtonsUnowned && !liveOwnedOnly() &&
        !InterlockedExchange(&g_unownedNoteLogged, 1)) {
        logD("search: search_buttons_unowned=%d - records you do NOT own are matched too. A "
             "record that has been SHOWN at any point in this world still has its display "
             "prototype in the page cache and is asked the engine's own Item::SearchText, so "
             "that answer is EXACT (whole rollover text, exactly like an owned record); a record "
             "that has never been on screen falls back to a case-insensitive substring of its "
             "catalogue.bin display NAME, which can only UNDER-report. Skipped entirely while "
             "owned_only=1, because the filter has taken the unowned records off the page",
             g_cfg.searchButtonsUnowned);
    }
    const LONG epoch = InterlockedCompareExchange(&g_needleEpoch, 0, 0);
    if (g_needleLen <= 0) {
        g_sweepEpoch = epoch;    // nothing typed: nothing to sweep, and the mask is already clear
        return;
    }
    const GdGameEngine* ge = panelGameEngine();
    if (!ge) return;
    if (epoch != g_sweepEpoch) {
        // A new needle (or a change mid-sweep): restart from the top, over a fresh snapshot of
        // the records the collection really holds.
        if (plateOwnedRefresh(false) < 0) return;   // the map walk has not succeeded yet
        try {
            if (!g_sweepRecs) g_sweepRecs = new std::vector<std::string>();
            g_sweepRecs->clear();
            g_sweepRecs->reserve(g_owned ? g_owned->size() : 0);
            if (g_owned) {
                for (std::unordered_set<std::string>::const_iterator it = g_owned->begin();
                     it != g_owned->end(); ++it) {
                    if (liveGroupOfItem(it->c_str()) >= 0) g_sweepRecs->push_back(*it);
                }
            }
        } catch (...) {
            return;
        }
        g_sweepEpoch = epoch;
        g_sweepAt = 0;
        g_sweepMask = 0;
        g_sweepScanned = 0;
        g_sweepMatched = 0;
        g_sweepSkipped = 0;
        g_sweepTicks = 0;
        g_sweepStartedAt = GetTickCount();
        // ---- arm tier C for this needle ----------------------------------------------------
        // Skipped, and the cursor left "done", whenever the key is off, the owned-only filter is
        // on (the user asked the marks to respect it, and with the filter on there are no
        // unowned records on the page to mark), the group file has not loaded, or the needle
        // folded to nothing.
        g_sweepMaskA = 0xFFFFFFFFu;
        g_sweepCGroup = 0;
        g_sweepCIndex = 0;
        g_sweepCExact = 0;
        g_sweepCNamed = 0;
        g_sweepCSkipped = 0;
        g_sweepCDone = true;
        // ---- the folded needle is TIER A's as well -----------------------------------------
        // A TABLE-OWNED record has no map node, so `Item::SearchText` has no object to be asked
        // about and tier A answers it from the same folded display NAME tier C's name half uses.
        // With nothing table-owned `tableHalf` is false and this block serves the name half only.
        g_sweepTableTried = 0;
        g_sweepTableNamed = 0;
        g_sweepTableMatched = 0;
        g_sweepFolded = false;
        const bool tierC = g_cfg.searchButtonsUnowned && !liveOwnedOnly() && tierCEnsureTable();
        const bool tableHalf = plateTableOwns();
        if (tierC || tableHalf) {
            try {
                if (!g_needleFolded) g_needleFolded = new std::string();
                utFoldWide(g_needle, g_needleLen, g_needleFolded);
                g_sweepFolded = !g_needleFolded->empty();
                if (tierC && g_sweepFolded) g_sweepCDone = false;
            } catch (...) {
                g_sweepFolded = false;
                g_sweepCDone = true;
            }
        }
    }
    if (!g_sweepRecs) return;
    if (g_sweepAt >= (int)g_sweepRecs->size() && g_sweepCDone) return;   // finished already
    ++g_sweepTicks;
    int budget = g_cfg.searchSweep > 0 ? g_cfg.searchSweep : kSweepPerTick;
    while (budget-- > 0 && g_sweepAt < (int)g_sweepRecs->size()) {
        const std::string& rec = (*g_sweepRecs)[(size_t)g_sweepAt++];
        const int grp = liveGroupOfItem(rec.c_str());
        if (grp < 0 || grp >= 24) continue;
        if (g_sweepMask & (1u << (unsigned)grp)) continue;   // that button is already marked
        // The engine's own proof that this record's stored prototype exists, IS an Item and has a
        // stack of at least 1 - plus the object id, which it push_backs into the mod's own vector.
        const int held = plateHeldOf(ge, rec.c_str());
        if (held < 1) {
            // ---- the TABLE-OWNED half of tier A --------------------------------------------
            // This record is in the sweep list because the mod's own file holds it, not because
            // the engine's map does - so `held < 1` is the NORMAL answer for it and not a skip.
            // There is no stored prototype to hand to `Item::SearchText`, so it is answered by
            // its display NAME, folded by the same header both other tiers fold with. A
            // substring of the name is always also a substring of the rollover text, so this
            // half UNDER-reports and can never over-report, which is the direction the marks
            // require. No engine call is made on this path at all.
            int nm = -1;
            if (g_sweepFolded && g_needleFolded && plateTableOwns()) {
                ++g_sweepTableTried;
                nm = plateTableNameMatch(rec.c_str(), g_needleFolded->c_str());
                if (nm >= 0) ++g_sweepTableNamed;
            }
            if (nm > 0) {
                g_sweepMask |= 1u << (unsigned)grp;
                ++g_sweepTableMatched;
            } else {
                ++g_sweepSkipped;
            }
            continue;
        }
        const unsigned int id = countVecFirstId();
        if (!id) {
            ++g_sweepSkipped;
            continue;
        }
        void* obj = reagentObjectFromId(id);
        if (!obj || !reagentItemIsLive((GdItem*)obj, id)) {
            ++g_sweepSkipped;
            continue;
        }
        const int m = itemSearchText(obj);
        if (m < 0) {
            searchOff("the exported Item::SearchText became unavailable mid-sweep");
            return;
        }
        ++g_sweepScanned;
        if (m > 0) {
            g_sweepMask |= 1u << (unsigned)grp;
            ++g_sweepMatched;
        }
    }
    if (g_sweepAt < (int)g_sweepRecs->size()) return;   // more tier A next tick - still UNLIT
    if (g_sweepMaskA == 0xFFFFFFFFu) {
        g_sweepMaskA = g_sweepMask;
        // Publish the OWNED answer the moment tier A finishes. It is complete on its own, so
        // tier C costs the owned half none of its responsiveness even though it triples the
        // record count. Tier C only ever ADDS groups to the same mask, so the marks grow and
        // never retract, and "never over-report" holds at every instant.
        InterlockedExchange(&g_maskLive, (LONG)g_sweepMask);
        InterlockedExchange(&g_maskValid, 1);
    }
    // ---- TIER C, on the same 32-per-tick budget as tier A ----------------------------------
    // Group by group, so a group tier A (or an earlier tier-C record) has already marked costs
    // nothing at all, and so the cursor is two small ints instead of a flattened index.
    if (!g_sweepCDone && g_tierC) {
        while (budget > 0 && g_sweepCGroup < (int)g_tierC->size()) {
            std::vector<TierCRec>& grp = (*g_tierC)[(size_t)g_sweepCGroup];
            if (g_sweepCGroup >= 24 || (g_sweepMask & (1u << (unsigned)g_sweepCGroup)) ||
                g_sweepCIndex >= (int)grp.size()) {
                ++g_sweepCGroup;      // no budget is spent skipping a group, and the cursor
                g_sweepCIndex = 0;    // always advances, so this loop cannot spin
                continue;
            }
            --budget;
            TierCRec& r = grp[(size_t)g_sweepCIndex++];
            // Owned records belong to tier A and were answered there, exactly and completely.
            if (plateOwns(r.item)) {
                ++g_sweepCSkipped;
                continue;
            }
            bool exact = false;
            bool matched = false;
            // (i) THE EXACT HALF. `g_protoCache` holds a display prototype for every record that
            // has been shown in this world; it is a real Item, built by UIReagentItem::Load from
            // the record's own uniq_b* display record, and it is the SAME object the engine's own
            // UpdateSearch tests when that group is on screen. Same liveness triple and same
            // CONTINUE_SEARCH fault shape as tier A - `itemSearchText` owns both.
            const unsigned int id = liveCachedProtoId(r.box);
            if (id) {
                void* obj = reagentObjectFromId(id);
                if (obj && reagentItemIsLive((GdItem*)obj, id)) {
                    const int m = itemSearchText(obj);
                    if (m < 0) {
                        searchOff("the exported Item::SearchText became unavailable mid-sweep");
                        return;
                    }
                    exact = true;
                    matched = m > 0;
                    ++g_sweepCExact;
                }
            }
            // (ii) THE NAME HALF, only for a record that has never been on screen. No engine
            // call at all: catalogue.bin is already in the mod's memory and both sides were
            // folded by the same function.
            if (!exact) {
                const std::string* nm = tierCName(&r);
                if (!nm) {
                    ++g_sweepCSkipped;   // catalogue.bin not loaded yet, or no name for it
                    continue;
                }
                ++g_sweepCNamed;
                matched = strstr(nm->c_str(), g_needleFolded->c_str()) != nullptr;
            }
            if (matched) g_sweepMask |= 1u << (unsigned)g_sweepCGroup;
        }
        if (g_sweepCGroup >= (int)g_tierC->size()) g_sweepCDone = true;
        if (!g_sweepCDone) return;   // more tier C next tick - still UNLIT
    }
    InterlockedExchange(&g_maskLive, (LONG)g_sweepMask);
    InterlockedExchange(&g_maskValid, 1);
    const LONG n = InterlockedIncrement(&g_sweeps);
    int groups = 0;
    for (int b = 0; b < 24; ++b) {
        if (g_sweepMask & (1u << (unsigned)b)) ++groups;
    }
    // How many groups each tier is responsible for. Tier A is
    // the mask as it stood when the owned pass finished; tier C is everything the unowned pass
    // added on top; tier B is the separate, free, on-screen bit `plateSearchMask` ORs in.
    int groupsA = 0, groupsC = 0;
    for (int b = 0; b < 24; ++b) {
        const unsigned int bit = 1u << (unsigned)b;
        if (g_sweepMaskA & bit) ++groupsA;
        else if (g_sweepMask & bit) ++groupsC;
    }
    logT("search: sweep #%ld done - %d character(s) typed, %d group(s) marked (tier A owned %d, "
         "tier C unowned %d, tier B on-screen 0x%06X) | tier A: %zu owned record(s), %d asked of "
         "Item::SearchText, %d matched, %d skipped | tier C: %s, %d asked of Item::SearchText "
         "EXACTLY (shown this world), %d matched by catalogue NAME only, %d skipped (owned, or "
         "no name yet); %d record(s) indexed, %ld named | %d tick(s), %lu ms",
         n, (int)InterlockedCompareExchange(&g_needleChars, 0, 0), groups, groupsA, groupsC,
         (unsigned int)InterlockedCompareExchange(&g_curHitMask, 0, 0), g_sweepRecs->size(),
         g_sweepScanned, g_sweepMatched, g_sweepSkipped,
         !g_cfg.searchButtonsUnowned
             ? "OFF (search_buttons_unowned=0)"
             : (liveOwnedOnly() ? "skipped, owned_only=1 takes unowned records off the page"
                                : "on"),
         g_sweepCExact, g_sweepCNamed, g_sweepCSkipped,
         (int)InterlockedCompareExchange(&g_tierCRecords, 0, 0),
         InterlockedCompareExchange(&g_tierCNamed, 0, 0), g_sweepTicks,
         (unsigned long)(GetTickCount() - g_sweepStartedAt));
    // One more line, and only when the private table actually owns something.
    if (g_sweepTableTried) {
        logT("search: sweep #%ld tier A, TABLE half - %d owned record(s) had no map node to ask, "
             "%d of them had a display name, %d matched (a name match under-reports and never "
             "over-reports)",
             n, g_sweepTableTried, g_sweepTableNamed, g_sweepTableMatched);
    }
}

// ---- the capture detour -----------------------------------------------------------------
// Both ReagentWindows (relics, materials) come through here. The materials one is the only one
// whose Load makes ObjectManager::LoadTableFile ask for the record we substitute, which is what
// bumps liveCaptureEpoch(); so an epoch that moved across the original call names the window.
// A substitution the detour ROLLED BACK - the active database could not serve the page, so the
// engine was handed the vanilla record instead - takes the epoch back with it. That window is the
// game's own Crafting Materials window: it has no boxes of ours in it and it must not reach the
// structural check below at all. What that check finds is a fact about THIS world's window, so
// its disable is re-armed at the world teardown.
void __cdecl hk_WndLoad(void* window, const void* str) {
    const long before = liveCaptureEpoch();
    if (o_WndLoad) {
        o_WndLoad(window, str);
    } else if (p_WndLoad) {
        p_WndLoad(window, str);
    }
    if (InterlockedCompareExchange(&g_disabled, 0, 0)) return;
    if (liveCaptureEpoch() == before) return;     // the relic window, or no substitution
    if (!checkWindowVtable(window)) {
        disableThisWorld("the ReagentWindow vtable does not match the located ReagentWindow::Load");
        return;
    }
    if (!checkWindowShape(window)) {   // before the FIRST pointer write
        disableThisWorld("the captured window does not have the ReagentWindow shape");
        return;
    }
    const void* tex = nullptr;
    int w = 0, h = 0;
    if (!readPlateFields(window, &tex, &w, &h)) {
        // Also a statement about this window's layout, not about the process.
        disableThisWorld("the plate fields at window+0x4B0 could not be read");
        return;
    }
    g_window = window;
    g_origPlate = tex;
    g_curPlate = tex;
    g_wantPlate = tex;
    g_origW = w;
    g_origH = h;
    // The frozen draw rect is the RECORD size times the UI
    // scale (307x439 at 0.700), so it is the wrong yardstick for a replacement texture. The
    // right one is the vanilla plate texture's own size - measured here, from the engine's own
    // GraphicsTexture - and a generated plate must match THAT.
    g_origTexW = g_origTexH = 0;
    if (tex && g_gd.TextureGetWidth && g_gd.TextureGetHeight) {
        int tw = 0, th = 0;
        if (texSizeSeh((const GdTexture*)tex, &tw, &th) && tw > 0 && th > 0) {
            g_origTexW = tw;
            g_origTexH = th;
        }
    }
    logD("plate: material ReagentWindow captured at %p - vanilla plate %p, texture %dx%d, "
         "frozen draw rect %dx%d (window+0x4B0 / +0x4C0)", window, tex, g_origTexW, g_origTexH, w,
         h);
    // The Draw slot, read out of THIS window's vtable now that it is proven.
    noteDrawSlot(window);
    // And the mouse slot, by the identical rule.
    noteMouseSlot(window);
    // And the UpdateSearch slot, by the identical rule.
    noteSearchSlot(window);
    // The visible-page accessor: recover the CaravanWindow from our caller's frame. Failure is
    // not fatal - the state stays UNKNOWN and every consumer fails safe.
    g_caravan = nullptr;
    int page = -1;
    void* caravan = scanForCaravan((const unsigned char*)_AddressOfReturnAddress(), window, &page);
    if (caravan) {
        g_caravan = caravan;
        logD("plate: CaravanWindow %p resolved from the caller frame (pages[3]==%p, page index "
             "at +0x1728 = %d) - the wheel and the label now follow the visible page",
             caravan, window, page);
    } else {
        logW("plate: the CaravanWindow could not be resolved from the caller frame - page "
             "visibility is UNKNOWN, so the mod consumes no input and draws no label");
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------------
bool plateInit(HMODULE selfModule) {
    g_self = selfModule;
    if (!g_plates) {
        g_plates = new std::vector<PlateTex>();
        g_owned = new std::unordered_set<std::string>();
    }
    HMODULE game = GetModuleHandleA("Game.dll");
    if (game) {
        p_GetPlayerReagents =
            (PfnGE_GetPlayerReagents)GetProcAddress(game, GD_GAMEENGINE_GETPLAYERREAGENTS);
        // The engine's own "how many of this record does the collection hold",
        // resolved by mangled name like everything else (no exe RVA, no signature).
        p_GetReagentItemCount = (PfnGE_GetReagentItemCount)GetProcAddress(
            game, GD_GAMEENGINE_GETREAGENTITEMCOUNT);
        // The search predicate, by mangled name like every other export.
        p_ItemSearchText = (PfnItemSearchText)GetProcAddress(game, GD_ITEM_SEARCHTEXT);
        logD("search: Item::SearchText -> %p (%s) - the game's own search predicate; the mod "
             "calls it on the stored prototype of an OWNED record to decide whether that "
             "record's category button is marked",
             (void*)p_ItemSearchText,
             p_ItemSearchText ? "available"
                              : "MISSING - the category-button search marks are OFF");
        if (!p_ItemSearchText) searchOff("the exported Item::SearchText was not found");
        // The tier-C policy, at MENU time, so the menu test can assert it. It is a separate
        // line on purpose: the owned-half policy line lives in ut_panel.cpp and this one
        // prints the unowned half.
        logD("search: tier C policy - search_buttons_unowned=%d. ON marks a category button for "
             "records you do NOT own as well: a record shown at any point this world is asked "
             "the engine's own Item::SearchText through the display prototype it left in the "
             "page cache, so that half is EXACT; a record never shown falls back to a "
             "case-insensitive substring of its catalogue.bin display NAME, which under-reports "
             "and never over-reports. Skipped while owned_only=1 (the marks respect the filter). "
             "One 24-bit mask for all three tiers - the category strip reads no second mask",
             g_cfg.searchButtonsUnowned);
    }
    logD("plate: GameEngine::GetPlayerReagents -> %p (owned counters %s)",
         (void*)p_GetPlayerReagents, p_GetPlayerReagents ? "available" : "OFF");
    logD("plate: owned counters count a record only when its STORED PROTOTYPE is live with a "
         "stack >= 1 (GameEngine::GetReagentItemCount -> %p) - a record taken back out keeps its "
         "map node and must not keep counting",
         (void*)p_GetReagentItemCount);
    // The counting rule in one menu-time line, so a log always names what was counted.
    logD("plate: owned counters are TABLE-OWNED - the mod's own uniq-items.jsonl holds the record "
         "with a count >= 1. The engine's own reagent map is not added to that number.");
    // Balance every LoadTexture with an UnloadTexture at world teardown.
    HMODULE eng = GetModuleHandleA("Engine.dll");
    if (eng && !p_UnloadTexture) {
        p_UnloadTexture =
            (PfnGfx_UnloadTexture)GetProcAddress(eng, GD_GFXENGINE_UNLOADTEXTURE);
        logD("plate: GraphicsEngine::UnloadTexture -> %p (per-world texture release %s)",
             (void*)p_UnloadTexture, p_UnloadTexture ? "available" : "OFF");
    }
    // The signature, the window capture and the CaravanWindow scan are what the page-visibility
    // accessor rests on, and the input gate must not depend on a cosmetic switch - so
    // `plate_swap=0` only stops the texture WRITE (see plateApply).
    if (!g_cfg.plateSwap) logD("plate: plate_swap=0 - the plate never changes (the page-visibility accessor still arms)");
    // State the label route at the menu. The Draw hook itself needs a HUD (the
    // address is read out of a live ReagentWindow's vtable), so it can only arm in game.
    logD("label: the label is drawn at the tail of ReagentWindow::Draw, under the item tooltips, "
         "with PresentSurface as the fallback (suppressed while the cursor is inside a box). The "
         "ReagentWindow::Draw address is read from the captured window's vtable slot +0x20, so "
         "this hook can only arm once a HUD exists.");
    // The pad's own menu line. The mouse-handler address is read out of a LIVE window's vtable
    // slot +0x38, so nothing about the pad can arm before a HUD exists.
    panelButtonsAnnounce();
    if (!resolveCode()) {
        g_why = "the ReagentWindow::Load signature did not resolve uniquely (yet)";
        return false;
    }
    InterlockedExchange(&g_codeOk, 1);
    g_why = "armed";
    return true;
}

int plateInstall(int* total) {
    // No plate_swap test here - the detour is what captures the window and the CaravanWindow,
    // which the wheel gate and the label need even with the swap off.
    // IDEMPOTENT. Three call sites can reach this (ut_live.cpp x2 + the late scan below) and
    // the second must not kill the feature with MH_ERROR_ALREADY_CREATED.
    if (InterlockedCompareExchange(&g_hookOn, 0, 0)) return 0;
    if (InterlockedCompareExchange(&g_disabled, 0, 0)) return 0;
    if (!InterlockedCompareExchange(&g_codeOk, 0, 0) || !p_WndLoad) {
        if (!InterlockedExchange(&g_deferLogged, 1)) {
            logD("  hook %-32s DEFERRED (the exe .text is not decrypted yet - the game-thread "
                 "late scan installs it)", "ReagentWindow::Load");
        }
        return 0;
    }
    if (total) *total += 1;

    MH_STATUS s = MH_CreateHook((void*)p_WndLoad, (void*)&hk_WndLoad, (void**)&o_WndLoad);
    if (s == MH_ERROR_ALREADY_CREATED) {
        // Someone (only ever this function) already created it; the trampoline from that call is
        // the one in o_WndLoad. Accept it only if we really do hold a trampoline.
        logD("  hook %-32s already created (%s) - keeping the existing trampoline %p",
             "ReagentWindow::Load", mhName(s), (void*)o_WndLoad);
        s = o_WndLoad ? MH_OK : MH_ERROR_NOT_CREATED;
    }
    if (s != MH_OK) {
        logE("  hook %-32s FAILED at MH_CreateHook: %s (%d)", "ReagentWindow::Load", mhName(s),
             (int)s);
        disable("the ReagentWindow::Load detour could not be created");
        return 0;
    }

    MH_STATUS e = MH_EnableHook((void*)p_WndLoad);
    if (e == MH_ERROR_ENABLED) e = MH_OK;   // already enabled is exactly what we want
    if (e != MH_OK) {
        logE("  hook %-32s FAILED at MH_EnableHook: %s (%d)", "ReagentWindow::Load", mhName(e),
             (int)e);
        disable("the ReagentWindow::Load detour could not be enabled");
        return 0;
    }

    InterlockedExchange(&g_hookOn, 1);
    logD("  hook %-32s installed at %p (trampoline %p)", "ReagentWindow::Load", (void*)p_WndLoad,
         (void*)o_WndLoad);
    return 1;
}

void plateTick(bool gameThread) {
    if (!gameThread) return;
    installDrawHook();     // never MinHook from inside an engine detour body
    // The same rule for the +0x38 mouse handler. The slot is re-read here as
    // well as in hk_WndLoad so that turning `group_buttons` on in the ini mid-world still arms
    // the click; both calls are no-ops once the hook is on, the candidate is pending, the key
    // is off, or the strip has disabled itself.
    if (g_window) noteMouseSlot(g_window);
    installMouseHook();
    // The fourth slot, on exactly the same terms.
    if (g_window) noteSearchSlot(g_window);
    installSearchHook();
    searchSweepTick();
    plateAssertPlate();   // install/restore the plate on the caravan's own edges
    // The map walk runs from here as well as from applyLayout, so a deposit or a take made
    // while the page is already up changes the screen without a scroll.
    // The walk is internally throttled to `plate_count_ms` and it is skipped
    // entirely unless the collection page is the one being looked at, so it costs nothing during
    // ordinary play. GAME THREAD, exactly like the relayout it can ask for.
    if (g_window && plateMaterialsVisible()) {
        plateOwnedRefresh(false);
        // The pad's font is loaded HERE, on the tick, not from inside the engine's own Draw -
        // the same rule the plate probe follows.
        panelButtonsPreload();
        // And the UI scale is asked of the engine HERE, for the same reason.
        // GraphicsEngine::GetUIScaleFactor is an engine call, so it may not sit on the mouse
        // handler's decision path nor inside the engine's own Draw; liveUiScale()
        // itself stays pure arithmetic over what this call cached. Gated on the two things that
        // consume the scale, so `group_buttons=0` plus `plate_label=0` really is NO work.
        if (g_cfg.groupButtons || g_cfg.plateLabel) liveUiScaleRefresh();
    }
    if (!g_cfg.plateSwap) return;
    // The probe is pure diagnostics - GraphicsEngine::LoadTexture and a log
    // line per plate, no write into any engine object - so it runs even if the swap disabled
    // itself. It is what tells the user (and the menu test) whether install_plates.ps1 worked.
    if (InterlockedCompareExchange(&g_codeOk, 0, 0)) probeAllPlates();
    if (InterlockedCompareExchange(&g_disabled, 0, 0)) return;
    if (InterlockedCompareExchange(&g_codeOk, 0, 0)) return;
    static DWORD lastTry = 0;
    const DWORD now = GetTickCount();
    if (now - lastTry < 500) return;
    lastTry = now;
    const LONG tries = InterlockedCompareExchange(&g_scanTries, 0, 0);
    if (tries < 40) {
        if (resolveCode()) {
            InterlockedExchange(&g_codeOk, 1);
            g_why = "armed (late scan)";
            logD("plate: armed by the late scan");
            plateInstall(nullptr);
            probeAllPlates();   // same tick: the menu test sees the seven lines immediately
        }
    } else if (tries == 40) {
        InterlockedIncrement(&g_scanTries);
        disable("the ReagentWindow::Load signature never resolved (40 attempts)");
    }
}

void plateApply(int group, const char* label, int cellW, int cellH) {
    if (InterlockedCompareExchange(&g_disabled, 0, 0)) return;
    if (!g_window || !g_origPlate) return;

    const void* want = g_origPlate;
    const char* what = "vanilla materials plate";
    char path[128] = "(vanilla)";
    if (group >= 0 && g_cfg.plateSwap) {
        PlateTex* p = plateFor(cellW, cellH);
        // Compare against the VANILLA PLATE TEXTURE's own size
        // (438x627), never against window+0x4C0, which is that size times the UI scale.
        const int wantW = g_origTexW > 0 ? g_origTexW : kPlateW;
        const int wantH = g_origTexH > 0 ? g_origTexH : kPlateH;
        if (p) {
            ensurePlate(p, wantW, wantH);
            _snprintf_s(path, sizeof(path), _TRUNCATE, "%s", p->path.c_str());
            if (p->ok && p->tex) {
                want = p->tex;
                what = "generated collection plate";
            }
        }
        if (want == g_origPlate) {
            const GdTexture* fb = fallbackPlate();
            if (fb) {
                want = fb;
                what = "plain transfer cover image (fallback)";
            }
        }
    }
    if (want == g_wantPlate) return;
    g_wantPlate = want;
    const LONG n = InterlockedIncrement(&g_swaps);
    logT("plate: swap #%ld group=%d \"%s\" cell=%dx%d -> %s (%s) tex=%p", n, group,
         label ? label : "?", cellW, cellH, path, what, want);
    plateAssertPlate();   // performs the write, and only while the caravan is open
}

void plateOnWorldTeardown() {
    // A disable taken for the shape of ONE world's window goes with that world. Only
    // disableThisWorld() sets the flag, so a failed detour install stays off for the session.
    if (InterlockedExchange(&g_disabledWorld, 0)) {
        InterlockedExchange(&g_disabled, 0);
        g_why = "armed";
        logD("plate: re-armed - the disable belonged to the world that is going away");
    }
    // Every Item* the sweep looked at belonged to the world that is going away,
    // and so does the mask built from them. The needle itself is kept (the search box's own text
    // is the engine's, not the mod's) but its EPOCH is invalidated, so the first tick of the next
    // world sweeps again from the top instead of trusting an answer about dead objects.
    g_sweepEpoch = -1;
    g_sweepAt = 0;
    g_sweepMask = 0;
    if (g_sweepRecs) g_sweepRecs->clear();
    InterlockedExchange(&g_maskLive, 0);
    InterlockedExchange(&g_maskValid, 0);
    InterlockedExchange(&g_curHitMask, 0);
    // Still deliberately NO write: this runs when the HUD that owned the window is being rebuilt
    // or has gone, and writing into an object the engine may have freed is exactly the failure
    // the liveness rules exist to prevent.  plateAssertPlate() has already put the vanilla
    // pointer back at the last caravan close.
    //
    // What DOES have to happen here is releasing the textures. A `GraphicsTexture*` that
    // survives the teardown that unloaded it is a freed texture, and a freed texture draws
    // WHITE.  Every texture the mod loaded is unloaded exactly once, except one the
    // engine's own destructor already unloaded for us (only possible if the caravan was still
    // open at teardown), and the flags are reset so the next world loads them again.
    const void* engineOwns =
        (g_curPlate && g_origPlate && g_curPlate != g_origPlate) ? g_curPlate : nullptr;
    int freed = 0, kept = 0;
    GdGraphicsEngine* gfx = graphics();
    if (g_plates) {
        for (size_t i = 0; i < g_plates->size(); ++i) {
            PlateTex& pt = (*g_plates)[i];
            if (pt.tex) {
                if (pt.tex == engineOwns || !gfx || !p_UnloadTexture) {
                    ++kept;
                } else if (unloadTexSeh(gfx, pt.tex)) {
                    ++freed;
                } else {
                    InterlockedIncrement(&g_faults);
                    ++kept;
                }
            }
            pt.tex = nullptr;
            pt.tried = false;
            pt.ok = false;
            pt.w = pt.h = pt.wantW = pt.wantH = 0;
        }
    }
    if (g_fallback) {
        if (g_fallback != engineOwns && gfx && p_UnloadTexture && unloadTexSeh(gfx, g_fallback)) {
            ++freed;
        } else {
            ++kept;
        }
    }
    g_fallback = nullptr;
    g_fallbackTried = false;
    InterlockedExchange(&g_probed, 0);   // the seven probe lines are re-logged per world
    if (g_window) {
        logD("plate: forgetting the material ReagentWindow %p (its HUD is gone) - released %d "
             "texture(s), %d left to the engine%s",
             g_window, freed, kept,
             engineOwns ? " (the destructor unloaded the mod plate: the caravan was open)" : "");
    }
    g_window = nullptr;
    g_caravan = nullptr;
    g_origPlate = nullptr;
    g_curPlate = nullptr;
    g_wantPlate = nullptr;
    g_origW = g_origH = 0;
    g_origTexW = g_origTexH = 0;
    InterlockedExchange(&g_stashOpen, 0);
    // The page state and the label-route log are per world.  The Draw DETOUR is
    // not: it is installed on an exe address that does not move, `this`-filtered on g_window
    // (now null), and re-arming it every world would create a hook MinHook already holds.
    g_lastPage = -2;
    InterlockedExchange(&g_pageEdgeSeen, 0);
    InterlockedExchange(&g_routeLogged, 0);
    g_drawAnchorLogged = false;
    // Everything the pad proved belongs to the world that is going away: object ids, the draw
    // origin and - above all - the event-space proof, which is re-earned from scratch in the
    // next world. The fonts MUST be dropped here (a GraphicsFont2* held across a teardown
    // faults instead of drawing).
    panelForgetUiFonts();
    InterlockedExchange(&g_btnProbes, 0);
    InterlockedExchange(&g_btnSpaceProven, 0);
    InterlockedExchange(&g_btnSpaceLogged, 0);
    InterlockedExchange(&g_btnPassLogged, 0);
    InterlockedExchange(&g_drawOriginOk, 0);
    g_lastDrawOx = g_lastDrawOy = 0.0f;
    // Object ids belong to the world that created them, so the map fingerprint of
    // the world that is going away must never be compared with the next one's (that would ask
    // for a repaint of a page nothing has built yet). The owned snapshot goes with it.
    g_mapFingerprint = 0;
    g_mapFingerprintValid = false;
    g_ownedLoggedTotal = g_ownedLoggedNodes = g_ownedLoggedEmpty = -1;
    g_ownedAt = 0;
    InterlockedExchange(&g_ownedOk, 0);
    InterlockedExchange(&g_ownedTotal, 0);
    InterlockedExchange(&g_ownedNodes, 0);
    InterlockedExchange(&g_ownedEmpty, 0);
    // The table's own counters are per-world telemetry exactly like the map's
    // (the table itself is the FILE and survives, but what this world's page has been told about
    // it does not). The buffer is kept: it is mod-owned memory with no engine pointer in it.
    InterlockedExchange(&g_ownedTableRows, 0);
    InterlockedExchange(&g_ownedTableCopies, 0);
    InterlockedExchange(&g_ownedTableOnly, 0);
    g_ownedLoggedTableRows = -1;
    g_ownedLoggedTableCopies = -1;
    g_ownedLoggedTableOnly = -1;
}

// ---- the visible caravan page ---------------------------------------------------------------
int plateCaravanState() {
    if (!g_caravan) return 0;
    int idx = -1;
    if (!readPageIndex(g_caravan, &idx)) return 0;
    return idx == kMaterialPage ? 2 : 1;
}

bool plateMaterialsVisible() {
    // The caravan has to be open in every case (the page index survives a close), and then the
    // Materials page has to be the visible one. An UNKNOWN caravan is a NO.
    if (!InterlockedCompareExchange(&g_stashOpen, 0, 0)) return false;
    const int st = plateCaravanState();
    // A page index of 3 also has to have been SEEN as an edge by the
    // game-thread poll, so the first frame of a HUD (index 0) - and any
    // frame before the poll has run at all - can never read as "Materials visible".
    if (st != 0) return st == 2 && InterlockedCompareExchange(&g_pageEdgeSeen, 0, 0) != 0;
    return false;
}

// Which PLATE is on screen, not which PAGE is shown. The label's
// medallion clip needs this: the GAME's own caravan_transfercomponent1_bg.tex carries a central
// medallion at record x 248..279 down to row 24, and a collection group can be shown over it -
// with plate_swap=0 (that key stops only the texture WRITE) or when the mod's
// loose-file plates were never installed. Both are non-null and different only while the mod's
// plate is actually the one the engine will draw this frame.
bool plateModPlateShown() {
    return g_curPlate != nullptr && g_origPlate != nullptr && g_curPlate != g_origPlate;
}

// ---- which route draws the label ------------------------------------------------------------
bool plateLabelInDraw() {
    return InterlockedCompareExchange(&g_drawHookOn, 0, 0) != 0 &&
           !InterlockedCompareExchange(&g_drawOff, 0, 0);
}

bool plateCursorInAnyBox(int sx, int sy) {
    if (!g_window) return false;
    void* boxes[256];
    int n = liveCapturedBoxes(boxes, 256);
    if (n <= 0) return false;
    if (n > 256) n = 256;
    return boxHitSeh(boxes, n, (float)sx, (float)sy);
}

bool plateBoxHitLocal(float lx, float ly, bool* faulted) {
    if (faulted) *faulted = false;
    if (!g_window) return false;
    void* boxes[256];
    int n = liveCapturedBoxes(boxes, 256);
    if (n <= 0) return false;
    if (n > 256) n = 256;
    return boxHitLocalSeh(boxes, n, lx, ly, faulted);
}

bool plateUnloadTexture(const GdTexture* tex) {
    if (!tex) return false;
    GdGraphicsEngine* gfx = graphics();
    if (!gfx || !p_UnloadTexture) return false;
    return unloadTexSeh(gfx, tex);
}

void plateNoteLabelRoute(int route) {
    // Logged on every CHANGE, not just the first draw: the very first frames of a HUD can fall
    // to route 2 because installDrawHook() has not had its game-thread tick yet, and the log
    // must say when route 1 took over. It cannot flap - route 1 is only ever reported while the
    // hook is armed, and drawOff() pins route 2 permanently once it has fired.
    if (InterlockedExchange(&g_routeLogged, route) == route) return;
    if (route == 1) {
        logD("label: drawn from ReagentWindow::Draw (the tail of exe 0x133440) - item tooltips "
             "are painted after it and now cover the label");
    } else {
        logD("label: PresentSurface fallback (cursor-in-box suppression) - the label is hidden "
             "while the cursor is inside a box's live hit rect (%s)",
             "the ReagentWindow::Draw hook is not armed");
    }
}

bool plateWindowRect(float* x, float* y, float* w, float* h) {
    if (!g_window) return false;
    void* boxes[1] = {nullptr};
    const int n = liveCapturedBoxes(boxes, 1);
    float r[6] = {0, 0, 0, 0, 0, 0};
    if (!readWindowRect(g_window, n > 0 ? boxes[0] : nullptr, r)) return false;
    if (!(r[2] > 16.0f && r[3] > 16.0f && r[2] < 20000.0f && r[3] < 20000.0f)) return false;
    if (!(r[0] > -4000.0f && r[0] < 20000.0f && r[1] > -4000.0f && r[1] < 20000.0f)) return false;
    static bool logged = false;
    if (!logged) {
        logged = true;
        logD("plate: window rect (%.1f,%.1f %.1fx%.1f) from box[0]'s parent origin; the window's "
             "own +0x80+0x6C pair says (%.1f,%.1f)", r[0], r[1], r[2], r[3], r[4], r[5]);
    }
    if (x) *x = r[0];
    if (y) *y = r[1];
    if (w) *w = r[2];
    if (h) *h = r[3];
    return true;
}

bool plateCursorInWindow(int sx, int sy) {
    float x = 0, y = 0, w = 0, h = 0;
    if (!plateWindowRect(&x, &y, &w, &h)) return false;
    const float fx = (float)sx, fy = (float)sy;
    return fx >= x && fy >= y && fx < x + w && fy < y + h;
}

// ---- owned-record snapshot ------------------------------------------------------------------
int plateOwnedRefresh(bool force) {
    if (!p_GetPlayerReagents || !g_owned) return -1;
    const GdGameEngine* ge = panelGameEngine();
    if (!ge) return -1;
    const DWORD now = GetTickCount();
    if (!force && g_ownedAt && now - g_ownedAt < (DWORD)(g_cfg.plateCountMs > 0
                                                             ? g_cfg.plateCountMs
                                                             : 1000)) {
        return InterlockedCompareExchange(&g_ownedOk, 0, 0)
                   ? (int)InterlockedCompareExchange(&g_ownedTotal, 0, 0)
                   : -1;
    }
    g_ownedAt = now;
    static const int kMaxNodes = 4096;
    std::vector<const void*> nodes;
    try {
        nodes.resize(kMaxNodes);
    } catch (...) {
        return -1;
    }
    const int n = collectNodes(ge, &nodes[0], kMaxNodes);
    if (n < 0) {
        InterlockedExchange(&g_ownedOk, 0);
        return -1;
    }
    // One line, once, if the engine cannot answer "how many does it hold" - the counter then
    // means node presence and the log says so.
    if (!p_GetReagentItemCount && !g_heldOffLogged) {
        g_heldOffLogged = true;
        logD("plate: owned counters fall back to NODE PRESENCE - "
             "GameEngine::GetReagentItemCount is unavailable, so a record that was taken back "
             "out still counts (it keeps its map node with a stack of 0)");
    }
    int nodeCount = 0;
    int emptied = 0;
    // FNV-1a 64 over every node's key and its stored prototype id, in the map's own (sorted)
    // order: the cheapest honest answer to "did the node set or any node's prototype change".
    unsigned long long fpr = 14695981039346656037ULL;
    try {
        g_owned->clear();
        for (int i = 0; i < n; ++i) {
            char raw[400];
            if (!nodeKeyCopy(nodes[i], raw, sizeof(raw))) continue;
            ++nodeCount;
            const unsigned int pid = nodeProtoId(nodes[i]);
            for (const char* p = raw; *p; ++p) {
                fpr = (fpr ^ (unsigned long long)(unsigned char)*p) * 1099511628211ULL;
            }
            for (int b = 0; b < 4; ++b) {
                fpr = (fpr ^ (unsigned long long)((pid >> (b * 8)) & 0xFFu)) * 1099511628211ULL;
            }
            // Node presence is NOT ownership (see the GetReagentItemCount note above). Only a
            // stored prototype that is still live and still carries a stack of at least one is
            // a record the collection really holds.
            int held = 1;
            if (p_GetReagentItemCount) {
                held = plateHeldOf(ge, raw);
                if (held < 0) held = 1;   // the accessor just died: keep the old meaning
            }
            // ---- the held bit is part of the fingerprint ------------------------------------
            // The held classification is part of the fingerprint, and it has to be. A TAKE
            // decrements the stored prototype's stack and NEVER erases the node and NEVER
            // re-points it, so the key set and every `pid` come out identical and a key+pid
            // fingerprint cannot see a take at all. With the owned-only filter on that would
            // mean: the owned set shrinks, `maxRow()` collapses, NOTHING asks for a relayout,
            // the page keeps drawing the stale slice - and the wheel clamps to a row it is
            // already on and reads as dead. A re-deposit into a
            // count-0 node was invisible the same way (the merge branch at Game.dll 0x2CEE0F
            // keeps the existing prototype). One bit per node is all it takes to see both.
            fpr = (fpr ^ (unsigned long long)(held >= 1 ? 1u : 0u)) * 1099511628211ULL;
            if (held >= 1) {
                g_owned->insert(std::string(raw));
            } else {
                ++emptied;
            }
        }
    } catch (...) {
        InterlockedExchange(&g_ownedOk, 0);
        return -1;
    }
    // ---- THE TABLE HALF (the transition rule) ----------------------------------------------
    // The map walk above is untouched and still fills `g_owned` with every MAP-OWNED record;
    // this adds every TABLE-OWNED one on top, so `plateOwns()` becomes the OR of the two and
    // `g_ownedTotal` - the label's number - is the size of that union. While `plateTableOwns()` is
    // false (a read-only journal, or none) nothing below runs and the snapshot is the map's alone.
    // The rows are folded into the SAME fingerprint, so a deposit into the table or a take
    // out of it moves `g_ownedGen` and asks for the relayout a map change asks for today.
    int tRows = 0, tCopies = 0, tOnly = 0;
    unsigned long long tMix = 0;
    if (plateTableOwns() && plateTableFold(&tRows, &tCopies, &tOnly, &tMix)) {
        for (int b = 0; b < 8; ++b) {
            fpr = (fpr ^ (unsigned long long)((tMix >> (b * 8)) & 0xFFu)) * 1099511628211ULL;
        }
    }
    InterlockedExchange(&g_ownedTableRows, (LONG)tRows);
    InterlockedExchange(&g_ownedTableCopies, (LONG)tCopies);
    InterlockedExchange(&g_ownedTableOnly, (LONG)tOnly);
    const LONG total = (LONG)g_owned->size();
    InterlockedExchange(&g_ownedTotal, total);
    InterlockedExchange(&g_ownedNodes, (LONG)nodeCount);
    InterlockedExchange(&g_ownedEmpty, (LONG)emptied);
    InterlockedExchange(&g_ownedOk, 1);
    // One line whenever the two counts disagree, and only when the picture actually changed -
    // this walk runs once a second while the page is up, so an unconditional line would be a
    // log flood. The triple is the state, not the event.
    if (total != (LONG)nodeCount &&
        (total != g_ownedLoggedTotal || (LONG)nodeCount != g_ownedLoggedNodes ||
         (LONG)emptied != g_ownedLoggedEmpty)) {
        g_ownedLoggedTotal = total;
        g_ownedLoggedNodes = (LONG)nodeCount;
        g_ownedLoggedEmpty = (LONG)emptied;
        logD("owned: %ld records with a live stack (%d nodes, %d emptied)", total, nodeCount,
             emptied);
    }
    // The table half gets its own line, on the same "only when the picture
    // changed" rule, and only when the table holds something.
    if ((tRows || tOnly) &&
        ((LONG)tRows != g_ownedLoggedTableRows || (LONG)tCopies != g_ownedLoggedTableCopies ||
         (LONG)tOnly != g_ownedLoggedTableOnly)) {
        g_ownedLoggedTableRows = (LONG)tRows;
        g_ownedLoggedTableCopies = (LONG)tCopies;
        g_ownedLoggedTableOnly = (LONG)tOnly;
        logD("owned: the private table holds %d record(s) / %d cop(ies), %d of which the engine's "
             "map does NOT hold - the owned set is the union of the two halves (%ld records)",
             tRows, tCopies, tOnly, total);
    }
    // A node that appeared, vanished, or had its prototype re-pointed (the take-path swap does
    // exactly that) changes what the shown boxes must paint, and
    // ReagentWindow::Sync alone will not always repaint them. Ask the game thread for one
    // relayout of the group already on screen. Never on the first walk of a world - the first
    // relayout has painted it already.
    //
    // The fingerprint also covers each node's held>=1 bit, so a take or a
    // re-deposit into a count-0 node moves it. `g_ownedGen` is bumped on the SAME condition
    // and is what `liveTick` watches to re-clamp the filtered row.
    if (g_mapFingerprintValid && fpr != g_mapFingerprint) {
        InterlockedIncrement(&g_ownedGen);
        {
            logD("owned: the collection changed (%d nodes, %ld with a live stack) - asking for a "
                 "repaint of the shown group",
                 nodeCount, total);
            liveRelayoutVisible();
        }
    }
    g_mapFingerprint = fpr;
    g_mapFingerprintValid = true;
    return (int)total;
}

// A monotonic counter that moves whenever the reagent map's node set, any stored
// prototype id, or any node's "does the collection really hold this" bit changes. Interlocked
// read, no lock, no engine call: safe from any thread. `liveTick` uses it to re-clamp the row
// while the owned-only filter is on (a filtered row count is in FILTERED units, so a take that
// empties four records can leave `g_curRow` past the last row that exists).
long plateOwnedGeneration() {
    return InterlockedCompareExchange(&g_ownedGen, 0, 0);
}

bool plateOwns(const char* record) {
    if (!g_owned || !record || !*record) return false;
    if (!InterlockedCompareExchange(&g_ownedOk, 0, 0)) return false;
    try {
        return g_owned->find(std::string(record)) != g_owned->end();
    } catch (...) {
        return false;
    }
}

// Bit i = "collection group i holds a match for what is typed in the caravan
// search box". Pure interlocked reads - no engine call, no lock, no allocation - so the strip's
// draw path stays exactly as call-free as it was.
unsigned int plateSearchMask() {
    if (!g_cfg.searchButtons) return 0;
    if (InterlockedCompareExchange(&g_searchOff, 0, 0)) return 0;
    unsigned int m = 0;
    if (InterlockedCompareExchange(&g_maskValid, 0, 0)) {
        m = (unsigned int)InterlockedCompareExchange(&g_maskLive, 0, 0);
    }
    m |= (unsigned int)InterlockedCompareExchange(&g_curHitMask, 0, 0);
    return m;
}

// Re-arm the sweep from OUTSIDE a needle change. `searchSweepTick` decides tier C's fate once,
// inside `epoch != g_sweepEpoch`, and nothing else bumps that epoch - so a filter toggle with a
// string already typed would neither clear the stale unowned marks (turning the filter ON) nor
// start tier C (turning it OFF).  Bumping the needle
// epoch restarts the whole sweep against the CURRENT filter state on the next tick, and dropping
// `g_maskValid` takes the stale marks off the buttons at once rather than a sweep later, which
// keeps the "never over-report" direction true at every instant.  Three interlocked stores and
// nothing else: no engine call, no lock, no allocation, safe from the strip's click path.
void plateSearchRearm() {
    InterlockedIncrement(&g_needleEpoch);
    InterlockedExchange(&g_maskValid, 0);
    InterlockedExchange(&g_maskLive, 0);
}

int plateOwnedTotal() {
    if (!InterlockedCompareExchange(&g_ownedOk, 0, 0)) return -1;
    return (int)InterlockedCompareExchange(&g_ownedTotal, 0, 0);
}

// The two halves the heartbeat needs to show that "owned" and "has a node" are not the same
// number. -1/-1 while no walk has succeeded in this world.
void plateOwnedCounts(int* nodes, int* emptied) {
    const bool ok = InterlockedCompareExchange(&g_ownedOk, 0, 0) != 0;
    if (nodes) *nodes = ok ? (int)InterlockedCompareExchange(&g_ownedNodes, 0, 0) : -1;
    if (emptied) *emptied = ok ? (int)InterlockedCompareExchange(&g_ownedEmpty, 0, 0) : -1;
}

// The table half of the same snapshot, for the heartbeat: rows the private
// table holds, the copies those rows add up to, and how many of the rows the engine's map does
// NOT hold (the number that goes from 0 to N as the collection moves out of `reagents.gst`).
// 0/0/0 while the mod's own file holds nothing. Interlocked
// reads of mod-owned LONGs only - no lock, no engine memory - so the worker thread may call it.
void plateTableCounts(int* rows, int* copies, int* tableOnly) {
    if (rows) *rows = (int)InterlockedCompareExchange(&g_ownedTableRows, 0, 0);
    if (copies) *copies = (int)InterlockedCompareExchange(&g_ownedTableCopies, 0, 0);
    if (tableOnly) *tableOnly = (int)InterlockedCompareExchange(&g_ownedTableOnly, 0, 0);
}

const char* plateStatus() {
    _snprintf_s(g_status, sizeof(g_status), _TRUNCATE,
                "plate: %s(%s) window=%p caravan=%p page=%d vanilla=%p cur=%p tex=%dx%d "
                "rect=%dx%d swaps=%ld faults=%ld owned=%ld | label: route=%d edge=%ld "
                "drawHook=%ld draws=%ld drawFaults=%ld ownedNodes=%ld ownedEmpty=%ld "
                "| search: hook=%ld off=%ld needle=%ld mask=0x%06X valid=%ld sweeps=%ld",
                (g_cfg.plateSwap && !InterlockedCompareExchange(&g_disabled, 0, 0)) ? "armed"
                                                                                    : "off",
                g_why, g_window, g_caravan, plateCaravanState(), g_origPlate, g_curPlate,
                g_origTexW, g_origTexH, g_origW, g_origH,
                InterlockedCompareExchange(&g_swaps, 0, 0),
                InterlockedCompareExchange(&g_faults, 0, 0),
                InterlockedCompareExchange(&g_ownedOk, 0, 0)
                    ? InterlockedCompareExchange(&g_ownedTotal, 0, 0)
                    : -1,
                plateLabelInDraw() ? 1 : 2, InterlockedCompareExchange(&g_pageEdgeSeen, 0, 0),
                InterlockedCompareExchange(&g_drawHookOn, 0, 0),
                InterlockedCompareExchange(&g_labelDraws, 0, 0),
                InterlockedCompareExchange(&g_drawFaults, 0, 0),
                InterlockedCompareExchange(&g_ownedNodes, 0, 0),
                InterlockedCompareExchange(&g_ownedEmpty, 0, 0),
                // The four numbers that say what the category-button marks are
                // doing - is the +0x118 detour live, did it disable itself, how many characters
                // are in the search box, which groups the last completed sweep marked, and how
                // many sweeps have finished. Interlocked reads only: safe on the heartbeat thread.
                InterlockedCompareExchange(&g_searchHookOn, 0, 0),
                InterlockedCompareExchange(&g_searchOff, 0, 0),
                InterlockedCompareExchange(&g_needleChars, 0, 0),
                (unsigned int)InterlockedCompareExchange(&g_maskLive, 0, 0),
                InterlockedCompareExchange(&g_maskValid, 0, 0),
                InterlockedCompareExchange(&g_sweeps, 0, 0));
    return g_status;
}

// The six search numbers, on their own, for the HEARTBEAT. `plateStatus()` has no caller - the
// heartbeat emits `panelStatus()` and `reagentStatus()` only - so the numbers the menu test
// asserts need a line of their own.
// They are NOT emitted by calling `plateStatus()` from the worker: that line carries
// `plateCaravanState()`, which READS ENGINE MEMORY through `g_caravan`, and the heartbeat thread
// has no business touching a live UI object (the same reason `liveStatus()` uses `uiScaleCached()`
// instead of `liveUiScale()`). Everything below is an interlocked read of a mod-owned LONG, so it
// is safe on any thread.
const char* plateSearchStatus() {
    // Two more interlocked reads on the end for tier C - how many records the
    // unowned index holds and how many of them have resolved a catalogue display name yet
    // (`tierC=0/0` before the first sweep of a world; `named` climbing to `recs` is what says
    // catalogue.bin arrived). Still interlocked reads of mod-owned LONGs only, so the worker
    // thread may keep calling this.
    _snprintf_s(g_searchStatus, sizeof(g_searchStatus), _TRUNCATE,
                "search: hook=%ld off=%ld needle=%ld mask=0x%06X valid=%ld sweeps=%ld "
                "tierC=%ld/%ld",
                InterlockedCompareExchange(&g_searchHookOn, 0, 0),
                InterlockedCompareExchange(&g_searchOff, 0, 0),
                InterlockedCompareExchange(&g_needleChars, 0, 0),
                (unsigned int)InterlockedCompareExchange(&g_maskLive, 0, 0),
                InterlockedCompareExchange(&g_maskValid, 0, 0),
                InterlockedCompareExchange(&g_sweeps, 0, 0),
                InterlockedCompareExchange(&g_tierCNamed, 0, 0),
                InterlockedCompareExchange(&g_tierCRecords, 0, 0));
    return g_searchStatus;
}

}  // namespace ut
