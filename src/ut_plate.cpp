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

#include "ut_generate.h"
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
#include "hooks.h"         // hookFrameCount - the screen pad's freshness clock
#include "ut_config.h"
#include "ut_live.h"
#include "ut_bindings.h"
#include "ut_caps.h"       // every bound a collection can reach, sized from the data
#include "ut_log.h"
#include "ut_paths.h"
#include "ut_ownedfold.h"  // the owned fold's arithmetic, tested offline
#include "ut_padmat.h"     // the pad's click rule (utPadLeftDown), tested offline
#include "ut_screendraw.h" // the screen pad's draw site proof and its frame, tested offline
#include "ut_panel.h"
#include "ut_reagent.h"
#include "ut_rescue.h"   // journalItemName, for tier C
#include "ut_store.h"    // storeTableOwns() - the ONE gate
#include "ut_startorder.h" // when the plates may be loaded, tested offline
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
const int kMaxBoxes = 8192;            // sanity ceiling for that vector (the frame builds N, 160..512)

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
// idempotent, and MH_ERROR_ALREADY_CREATED / MH_ERROR_ENABLED are benign by name.
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
volatile LONG g_matKeepLogged = 0;     // bitmask: the once-per-world "kept from the box" lines
float g_lastDrawOx = 0.0f;             // the origin the engine's own Draw computed, game thread
float g_lastDrawOy = 0.0f;

// ui/caravan/caravan_transfercomponent1_bg.tex, the plate the material window loads for itself.
// BitmapSingle::SetBitmap froze the widget's draw rectangle from THAT size at HUD build, so a
// replacement of any other size would be stretched into it: a plate that does not measure this
// is refused, and the plain transfer cover image is used instead.
const int kPlateW = 438;
const int kPlateH = 627;

// ...unless the generator derived another one. The P line of uniq-groups.txt carries the cover
// the MERGED database names and that texture's real pixel size, so a stash-UI mod that enlarges
// the caravan is followed; a group file without the line answers with the two constants above,
// which is exactly what such a file described.
int platePlateW() {
    ut::LiveGeometry g;
    liveGeometry(&g);
    return g.plateW > 0 ? g.plateW : kPlateW;
}
int platePlateH() {
    ut::LiveGeometry g;
    liveGeometry(&g);
    return g.plateH > 0 ? g.plateH : kPlateH;
}

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
    //             system resolves against its own roots: the generator paints the plates into
    //             the loose-file override root <game>\settings\ui\caravan\ (src\gen\generate.h),
    //             so this is the candidate that finds them. THE DOCUMENTED FALLBACK: it costs one
    //             failed modPath lookup per plate size before it, once per session, and nothing
    //             else.
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

// Every box the last material ReagentWindow::Load captured, however many the view's frame carries
// (160..512, the F line): the count is asked first and the copy sized to it, so no box past a
// fixed array is ever left out of a hit test. Returns how many `out` holds.
int capturedBoxesAll(std::vector<void*>& out) {
    out.clear();
    const int want = liveCapturedBoxes(nullptr, 0);
    if (want <= 0) return 0;
    out.resize((size_t)want);
    int n = liveCapturedBoxes(out.data(), want);
    if (n > want) n = want;
    if (n < 0) n = 0;
    out.resize((size_t)n);
    return n;
}

bool checkWindowShape(void* window) {
    const unsigned char *tlo = nullptr, *thi = nullptr, *ilo = nullptr, *ihi = nullptr;
    if (!textRange(&tlo, &thi) || !imageRange(&ilo, &ihi)) {
        logD("plate: cannot read the exe section table - the structural check is inconclusive");
        return false;
    }
    std::vector<void*> boxVec;
    const int captured = capturedBoxesAll(boxVec);
    void* const* boxes = boxVec.data();

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
    // The box vector is built from the frame record's reagentBoxes list, so its size is that
    // list's length - unless the engine was handed another copy of the record.
    int wroteBoxes = 0;
    if (s.vecCount >= 0 && reagentWrittenFrame(nullptr, nullptr, &wroteBoxes) &&
        s.vecCount != wroteBoxes)
        logW("plate: the window holds %d boxes but the frame record the %s view wrote lists %d - "
             "the engine built the page from another copy of that record",
             s.vecCount, viewName(), wroteBoxes);
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

// window+0x40/+0x44: ReagentWindow::Load writes WindowLocationX/Y * UI scale there (exe
// 0x132184..0x1321B5), i.e. the page's place inside the caravan window, in pixels.
const size_t kWindowLocationOff = 0x40;

bool readWindowLocation(void* window, float* out) {
    __try {
        const float* p = (const float*)((const unsigned char*)window + kWindowLocationOff);
        out[0] = p[0];
        out[1] = p[1];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_faults);
        return false;
    }
}

int roundUnits(float v) { return (int)(v >= 0.0f ? v + 0.5f : v - 0.5f); }

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

// The live layout's grid as the plate set reads it (utPlateSetChoice): -1 = not resolved yet (no
// UI scale), 0 = the derived grid at zoom 100, 1 = a layout of its own - the user's grid_ line or a
// zoom_pct above 100 (utLiveGridKind: the derived plates are painted for the generated cells, so
// zoomed boxes get the zoom's own set, which the layout-plates thread paints for the derived grid
// too). Game thread (the plate probe and the relayout), like every other liveLayoutGet caller there.
int plateLiveGrid() {
    ut::LiveGeometry g;
    liveGeometry(&g);
    if (g.plateW <= 0 || g.plateH <= 0) return -1;   // no P line loaded: no cover, no grid yet
    LiveLayout lay;
    if (!liveLayoutGet(&lay)) return -1;
    return utLiveGridKind(lay.L.user[kUtLayGrid], lay.zoom);
}

// Which set the page shows now: the layout-plates thread's published set (layoutPlatesState)
// compared with the live layout's grid. `suffix` gets "_L<hash>" for the layout plates, else "".
int plateSetNow(char* suffix, size_t cap) {
    const int pw = platePlateW(), ph = platePlateH();
    char lay[16];
    bool ready = true;
    layoutPlatesState(pw, ph, lay, sizeof(lay), &ready);
    const int set = utPlateSetChoice(lay[0] ? 1 : 0, ready ? 1 : 0, plateLiveGrid());
    if (suffix && cap) _snprintf_s(suffix, cap, _TRUNCATE, "%s", set == kUtPlateSetLayout ? lay : "");
    return set;
}

// The two candidate names of one plate, from its cell size and the LIVE view. Rebuilt in
// place on a view change: the draw path may hold a pointer into g_plates, so the vector
// itself must not be reallocated.
void plateNames(PlateTex& p) {
    // THE NAME. A start-up view's plate for a 438x627 cover keeps the vanilla name, so the set
    // that ships today goes on being found; any other cover gets the plate size in the name,
    // because the same cell size on a bigger caravan is a different picture.
    const int pw = platePlateW(), ph = platePlateH();
    const bool vanillaPlate = pw == kPlateW && ph == kPlateH;
    // A custom game's view always adds the size and its tag, whatever its cover size: every
    // view's plates live in the same override folder, and a custom game with the vanilla cover
    // size would otherwise share the main campaign's names. src\gen\plate_gen.cpp builds the same
    // name from the same pieces.
    const char* tag = viewTag();
    char buf[192];
    // A user grid in force (ut_generate's layout plates): its own set, always with the cover and
    // the tag, served only from the override root. Not painted yet - or the live page has the user
    // grid in force but the layout-plates thread has no set for it: no name at all, so the plain
    // cover is shown instead of frames that do not sit under the boxes. The live page's derived
    // grid always gets the derived plates, whatever the thread published (utPlateSetChoice).
    char lay[16];
    const int set = plateSetNow(lay, sizeof(lay));
    if (set != kUtPlateSetDerived) {
        p.modPath.clear();
        p.path.clear();
        if (set == kUtPlateSetPlain) return;
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "ui/caravan/uniq_plate_%dx%d_%dx%d%s%s%s.tex",
                    p.cellW, p.cellH, pw, ph, tag[0] ? "_" : "", tag, lay);
        p.path = buf;
        return;
    }
    if (tag[0])
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "ui/caravan/uniq_plate_%dx%d_%dx%d_%s.tex",
                    p.cellW, p.cellH, pw, ph, tag);
    else if (vanillaPlate)
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "ui/caravan/uniq_plate_%dx%d.tex", p.cellW, p.cellH);
    else
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "ui/caravan/uniq_plate_%dx%d_%dx%d.tex",
                    p.cellW, p.cellH, pw, ph);
    p.path = buf;
    // The mod-folder candidate. utModFile is the ONE resolver (it knows about the Documents
    // fallback and about %UNIQUETAB_OUT%), and it answers whether the file is actually there - so
    // a missing plates folder costs no LoadTexture call at all.
    char leaf[192];
    char full[MAX_PATH];
    if (tag[0])
        _snprintf_s(leaf, sizeof(leaf), _TRUNCATE, "plates\\uniq_plate_%dx%d_%dx%d_%s.tex",
                    p.cellW, p.cellH, pw, ph, tag);
    else if (vanillaPlate)
        _snprintf_s(leaf, sizeof(leaf), _TRUNCATE, "plates\\uniq_plate_%dx%d.tex", p.cellW, p.cellH);
    else
        _snprintf_s(leaf, sizeof(leaf), _TRUNCATE, "plates\\uniq_plate_%dx%d_%dx%d.tex",
                    p.cellW, p.cellH, pw, ph);
    p.modPath.clear();
    if (utModFile(g_self, leaf, full, sizeof(full))) p.modPath = full;
}

PlateTex* plateFor(int cellW, int cellH) {
    if (!g_plates) return nullptr;
    for (size_t i = 0; i < g_plates->size(); ++i) {
        if ((*g_plates)[i].cellW == cellW && (*g_plates)[i].cellH == cellH) return &(*g_plates)[i];
    }
    PlateTex p;
    p.cellW = cellW;
    p.cellH = cellH;
    plateNames(p);
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

// Textures a rename replaced. One the window may still show (installed or about to be) waits for
// the world teardown's UnloadTexture; any other is released at once. Full: left to the engine.
const GdTexture* g_superseded[64];
int g_supersededN = 0;

void plateSupersede(const GdTexture* t, bool unloadNow) {
    if (!t) return;
    GdGraphicsEngine* gfx = graphics();
    bool shown = t == g_curPlate || t == g_wantPlate || t == g_origPlate || t == g_fallback;
    for (int i = 0; i < g_supersededN && !shown; ++i) shown = g_superseded[i] == t;
    if (unloadNow && !shown && gfx && p_UnloadTexture && unloadTexSeh(gfx, t)) return;
    if (g_supersededN < (int)(sizeof(g_superseded) / sizeof(g_superseded[0])))
        g_superseded[g_supersededN++] = t;
}

// Every cached plate gets its name again and is probed again on the next use; the textures it
// held are superseded (released now when `unloadNow` and nothing shows them, else at teardown).
// `all` = false keeps an entry whose names did not change. Returns how many were renamed.
int plateRenameAll(bool unloadNow, bool all) {
    InterlockedExchange(&g_probed, 0);
    if (!g_plates) return 0;
    int renamed = 0;
    for (PlateTex& pt : *g_plates) {
        if (!all) {
            PlateTex fresh;
            fresh.cellW = pt.cellW;
            fresh.cellH = pt.cellH;
            plateNames(fresh);
            if (fresh.path == pt.path && fresh.modPath == pt.modPath) continue;
        }
        ++renamed;
        plateSupersede(pt.tex, unloadNow);
        plateNames(pt);
        pt.tex = nullptr;
        pt.tried = false;
        pt.ok = false;
        pt.fromModFolder = false;
        pt.w = pt.h = pt.wantW = pt.wantH = 0;
    }
    return renamed;
}

// At the world teardown, BEFORE the cached plates are released: every superseded texture but the
// one the engine's own destructor unloaded, and none that a cached plate, the fallback or an
// earlier entry still holds (the engine may hand the same object back for a name loaded again) -
// those are released once, by their other owner, or left to the engine.
void plateReleaseSuperseded(GdGraphicsEngine* gfx, const void* engineOwns, int* freed, int* kept) {
    for (int i = 0; i < g_supersededN; ++i) {
        const GdTexture* t = g_superseded[i];
        bool held = t == engineOwns || t == g_fallback;
        for (int j = 0; j < i && !held; ++j) held = g_superseded[j] == t;
        if (g_plates)
            for (size_t k = 0; k < g_plates->size() && !held; ++k) held = (*g_plates)[k].tex == t;
        if (held || !gfx || !p_UnloadTexture) ++*kept;
        else if (unloadTexSeh(gfx, t)) ++*freed;
        else { InterlockedIncrement(&g_faults); ++*kept; }
    }
    g_supersededN = 0;
}

// The layout plates changed state (a user grid's set is being painted, is ready, or went), or the
// live page's grid changed between the derived and the user's (utPlateSetChoice): the cached names
// are rebuilt and probed again, and the shown group picks its plate again.
long g_layoutSeen = -1;
int g_plateSetSeen = -1;   // the utPlateSetChoice the cached names were last checked against
void plateLayoutEdge() {
    const long gen = layoutPlatesGen();
    char lay[16];
    const int set = plateSetNow(lay, sizeof(lay));
    if (gen == g_layoutSeen && set == g_plateSetSeen) return;
    const bool first = g_layoutSeen == -1;
    g_layoutSeen = gen;
    g_plateSetSeen = set;
    if (!g_plates || g_plates->empty()) return;
    const int n = plateRenameAll(true, false);
    if (first || n == 0) return;
    char pub[16];
    bool ready = true;
    layoutPlatesState(platePlateW(), platePlateH(), pub, sizeof(pub), &ready);
    if (set == kUtPlateSetDerived && pub[0])
        logI("plate: the live page has the derived grid in force - the derived plates are shown, "
             "not the layout plates %s painted for the user grid; %d plate(s) renamed and probed "
             "again", pub, n);
    else if (set == kUtPlateSetDerived)
        logI("plate: the derived plates are in force again - %d plate(s) renamed and probed again", n);
    else if (set == kUtPlateSetLayout)
        logI("plate: the layout plates %s are ready - %d plate(s) renamed and probed again; the "
             "page switches to them", lay, n);
    else if (pub[0])
        logI("plate: a user grid's plates %s are being painted - the plain cover is shown until "
             "they are ready", pub);
    else
        logI("plate: the live page has the user grid or a zoom in force and no layout plates are "
             "published for it - the plain cover is shown until they are");
    liveRelayoutVisible();   // plateApply chooses the shown group's plate again
}

// ORDERING. The plates are written by the generation, which runs on the start-up worker
// (dllmain, before panelInit and before any hook exists) or inside a custom game's database
// load. This probe runs only once a materials window has been captured in a world (plateTick,
// utPlateProbeAllowed): the HUD build, where the engine loads its own plate through the same
// LoadTexture. It never runs at the main menu - on a fresh install the first hooked frame fell
// in the engine's start-up streaming, and the engine's loader thread faulted in its archive
// decoder right after seven plate loads and the cover load there. A world teardown clears
// g_probed and the cached textures, so the next world re-probes the files on disk now.
void probeAllPlates() {
    plateLayoutEdge();
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
        if (ensurePlate(p, platePlateW(), platePlateH())) ++ok;
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
    // Only while the engine says the caravan is OPEN. A page edge can happen at no other time,
    // and the engine frees the CaravanWindow before GameEngine::ExitPlayingMode reaches the
    // teardown that forgets it - so an unconditional read probes a freed window on the last
    // tick or two of every quit. The read stays inside the mod's probe scope, where an expected
    // fault is counted and not printed as a crash.
    if (open && g_caravan) {
        int idx = -1;
        reagentProbeEnter();
        const bool read = readPageIndex(g_caravan, &idx);
        reagentProbeLeave();
        if (read) {
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
// The record buffer `journalCollectStored` copies into. GAME THREAD ONLY, allocated on the first
// walk that finds the table owning anything, and SIZED FROM THE JOURNAL: the table's rows plus
// 256, grown whenever a walk finds more (ut_caps.h `utTableWalk`), up to a ceiling that protects
// memory only (65,536 rows x 260 B = 17 MB). A fixed 4,096-row buffer counts a 7,169-record
// collection as 4,096 - and WHICH 4,096 follows the order of the journal's lines.
UtTableBuf* g_tableBuf = nullptr;
bool g_tableCeilingWarned = false;

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
//
// `uncopied` is the rows the walk COUNTED but could not copy (the buffer at its memory ceiling, or
// an allocation that failed): 0 for every collection a catalogue can hold. The caller adds them
// to the label's number, so the count stays the truth; the owned filter and the search see the
// copied rows only, and one WARN says so.
bool plateTableFold(int* rows, int* copies, int* onlyTable, unsigned long long* mix,
                    int* uncopied) {
    *rows = 0;
    *copies = 0;
    *onlyTable = 0;
    *mix = 0;
    *uncopied = 0;
    if (!g_owned) return false;
    try {
        if (!g_tableBuf) g_tableBuf = new UtTableBuf();
    } catch (...) {
        return false;
    }
    // ONE walk (one journal lock pair per try), and `counts[i]` is format 4's REAL count - copies
    // that live ONLY in the mod's own file. Every MAP-OWNED row, including every legacy
    // `"stored":true` row, is count 0 and `journalCollectStored` does not return it at all, so
    // neither the heartbeat's `ownedTable=` nor the `owned: the private table holds ...` line can
    // claim the table holds a collection it holds nothing of. The walk returns EVERY row and the
    // buffer is sized from that count: the first walk of a session counts, allocates the table's
    // rows + 256 and copies; a later one that finds more (deposits) grows it and walks again. A
    // table that holds nothing allocates nothing.
    const UtTableWalk w = utTableWalk(g_tableBuf, 0,
                                      [](char (*o)[256], unsigned int* c, int cap) {
                                          return journalCollectStored(o, c, cap);
                                      });
    if (w.grewTo) {
        logD("owned: the private table's walk buffer holds %d rows now (was %d; %d table rows; "
             "%zu bytes) - sized from the journal, it grows with it",
             w.grewTo, w.grewFrom, w.rows, (size_t)w.grewTo * kUtTableStride);
    }
    if (w.truncated && !g_tableCeilingWarned) {
        g_tableCeilingWarned = true;
        logW("***** owned: the private table holds %d rows and the walk copied %d (%s) - the "
             "label still counts all %d; the owned filter and the search see the first %d *****",
             w.rows, w.copied,
             w.allocFailed ? "the buffer could not be allocated"
                           : "the 65,536-row memory ceiling",
             w.rows, w.copied);
    }
    char (*recs)[256] = g_tableBuf->recs();
    unsigned int* counts = g_tableBuf->counts();
    const int n = w.copied;
    if (w.truncated) {
        *uncopied = w.rows - w.copied;
        // The rows past the buffer still move the fingerprint: a deposit there must relayout.
        utOwnedMixRow(mix, "#rows", (unsigned int)w.rows);
    }
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
// Returns how many nodes the map HOLDS and writes the first min(that, cap); -1 = no map, or a walk
// past kUtMapWalkCeiling nodes (a corrupt tree).
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
            if (++visited > kUtMapWalkCeiling) return -1;
            while (cur && !cur->isnil) {
                if (sp >= 64) return -1;
                stack[sp++] = cur;
                cur = cur->left;
            }
            const ReagentNode* n = stack[--sp];
            if (found < cap) nodes[found] = n;
            ++found;   // EVERY node is counted: the caller grows to the count (utCollectAll)
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

// ---- the SCREEN pad's draw site: its state, used by the Draw tail below -------------------------
// pad_screen / label_screen put the pad and the label at a corner of the SCREEN. They are drawn
// from the HEAD of the HUD's item-rollover Draw - after the 19 HUD children drawn after the caravan
// (whichever of them is the minimap or the quest tracker) and before the item tooltips and every
// tooltip the UI root draws later: "one level below the tooltips" (ut_screendraw.h: the proof).
// The Draw tail stays the documented fallback (kUtScreenSiteChosen, no ini key). The site is
// installed only while a screen key is set; until it works the Draw tail publishes no screen
// size, so the layout keeps both parts in the window ("cannot be placed yet").
const size_t kHudRolloverOff = 0xB800;     // LITERAL: the item rollover inside the Hud; confirmed
                                           // per session by its draw call in the HUD's own Draw
const size_t kSlotChildDraw = 0x20;        // a HUD child's Draw (the HUD's own Draw too)
const size_t kHudDrawScanBytes = 0x1C00;   // 1.3.0.8: the child calls end at +0x172F
typedef void(__cdecl* PfnChildDraw)(void* self, void* canvas, const void* offset, float alpha);
PfnChildDraw p_RollDraw = nullptr;        // the hooked target (the rollover's Draw)
PfnChildDraw o_RollDraw = nullptr;        // its trampoline
void* g_siteTarget = nullptr;             // read and proved, installs on the next game tick
void* g_siteRollPending = nullptr;
void* g_siteRoll = nullptr;               // THIS world's item-rollover object (the hook's `self`)
volatile LONG g_siteOn = 0;               // 1 = the rollover-head detour is installed
volatile LONG g_siteOff = 0;              // refused or faulted: the screen parts stay in the window
volatile LONG g_sitePublished = 0;        // 1 = the screen rectangles are published
volatile LONG64 g_tailFrame = 0;          // hookFrameCount() of the last Draw tail that drew the page

void siteRetract() {
    if (InterlockedExchange(&g_sitePublished, 0)) plateScreenRectsPublish(nullptr);
}

// The site that draws the screen parts works (or is the Draw tail itself).
bool screenSiteUsable() {
    if constexpr (kUtScreenSiteChosen == kUtScreenSiteDrawTail) {
        return InterlockedCompareExchange(&g_siteOff, 0, 0) == 0;
    } else {
        return InterlockedCompareExchange(&g_siteOn, 0, 0) != 0 &&
               InterlockedCompareExchange(&g_siteOff, 0, 0) == 0;
    }
}

// One WARN: the screen parts stay in the window for the rest of the session.
void siteOff(const char* why, bool fault) {
    if (InterlockedExchange(&g_siteOff, 1)) return;
    liveSetScreenView(0, 0, 0.0f, 0.0f);   // "cannot be placed yet": both parts back in the window
    siteRetract();
    if (fault) {
        logE("buttons: the screen pad's draw site is OFF for this session - %s. The pad and the "
             "label stay in the caravan window.", why);
    } else {
        logW("buttons: the screen pad's draw site could not be installed - %s. pad_screen / "
             "label_screen are not used this session: the pad and the label stay in the caravan "
             "window.", why);
    }
}

// Draws the screen parts here and publishes what was painted for the screen pad's input.
void screenDrawHere(GdCanvas* canvas) {
    // The layout editor open: it draws the parts and publishes its own grounds every frame (ut_panel
    // editPublishScreen, from the Draw tail earlier in this frame), so the site neither draws nor
    // publishes nor retracts - either would replace them. The site's last publication is the
    // editor's to replace (or goes stale); it is not retracted again after the editor closes.
    if (panelLayoutEditOpen()) {
        InterlockedExchange(&g_sitePublished, 0);
        return;
    }
    static UtPadScreenRects pub;   // the thread of the Draw tail only (2.6 KB, off the stack)
    if (panelDrawScreenParts(canvas, &pub) && pub.shown) {
        plateScreenRectsPublish(&pub);
        InterlockedExchange(&g_sitePublished, 1);
    } else {
        siteRetract();
        // A fault inside the screen draw (reported by ut_panel, whose own __except consumed it):
        // the site goes off, the screen size is withdrawn, both parts go back into the window.
        if (panelScreenDrawFaulted()) siteOff("a fault while drawing the pinned pad / label", true);
    }
}

// The Draw tail drew the page this frame: say so (the site draws only then), and publish the
// screen and the window's origin the screen parts are placed against.
void screenTailNote(void* canvas, float ox, float oy) {
    InterlockedExchange64(&g_tailFrame, (LONG64)hookFrameCount());
    if (!g_cfg.padScreen[0] && !g_cfg.labelScreen[0]) return;   // nothing is placed on the screen
    if (!screenSiteUsable()) return;
    const int w = g_gd.CanvasGetWidth ? g_gd.CanvasGetWidth((GdCanvas*)canvas) : 0;
    const int h = g_gd.CanvasGetHeight ? g_gd.CanvasGetHeight((GdCanvas*)canvas) : 0;
    liveSetScreenView(w, h, ox, oy);
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
    screenTailNote(canvas, ox, oy);
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
    // The documented fallback site: the screen parts drawn here, under the HUD children drawn
    // after the caravan. With the rollover head (the site) they are drawn there instead.
    if constexpr (kUtScreenSiteChosen == kUtScreenSiteDrawTail) screenDrawHere((GdCanvas*)canvas);
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

// ---- the SCREEN pad's draw site: the head of the HUD's item-rollover Draw ---------------------
void siteBody(void* canvas) {
    // The page was drawn THIS frame (its Draw tail ran before the HUD reached the rollover): the
    // caravan is open on the materials window. Otherwise nothing is drawn and the input is told.
    const LONG64 tail = InterlockedCompareExchange64(&g_tailFrame, 0, 0);
    if (tail == 0 || tail != (LONG64)hookFrameCount()) {
        siteRetract();
        return;
    }
    screenDrawHere((GdCanvas*)canvas);
}

void siteGuarded(void* canvas) {
    __try {
        siteBody(canvas);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_drawFaults);
        siteOff("a fault at the head of the item rollover's Draw", true);
    }
}

// The original ALWAYS runs, after the screen parts: the tooltips are drawn over them.
void __cdecl hk_RollDraw(void* self, void* canvas, const void* offset, float alpha) {
    if (self && self == g_siteRoll && !InterlockedCompareExchange(&g_siteOff, 0, 0))
        siteGuarded(canvas);
    if (o_RollDraw) o_RollDraw(self, canvas, offset, alpha);
}

struct SiteRead {
    void* roll;
    const void* rollDraw;
    const void* hudDraw;
    const void* hudVt;
    const void* rollVt;
    size_t codeN;
};
unsigned char g_siteCode[kHudDrawScanBytes];   // the HUD Draw's bytes, game thread only

bool siteReadSeh(void* caravan, const unsigned char* thi, SiteRead* o) {
    __try {
        unsigned char* hud = (unsigned char*)caravan - kHudCaravanOff;
        const void* const* hudVt = *(const void* const* const*)hud;
        unsigned char* roll = hud + kHudRolloverOff;
        const void* const* rollVt = *(const void* const* const*)roll;
        if (!hudVt || !rollVt) return false;
        o->roll = roll;
        o->hudVt = hudVt;
        o->rollVt = rollVt;
        o->hudDraw = hudVt[kSlotChildDraw / 8];
        o->rollDraw = rollVt[kSlotChildDraw / 8];
        size_t n = kHudDrawScanBytes;
        const unsigned char* code = (const unsigned char*)o->hudDraw;
        if (code < thi && (size_t)(thi - code) < n) n = (size_t)(thi - code);
        memcpy(g_siteCode, code, n);
        o->codeN = n;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// GAME THREAD (plateTick). Finds THIS world's item rollover from the captured caravan (the Hud
// embeds both), proves the site once per session from the HUD's own Draw, and queues the install.
void siteNote() {
    if constexpr (kUtScreenSiteChosen != kUtScreenSiteRollover) return;
    if (InterlockedCompareExchange(&g_siteOff, 0, 0)) return;
    if (!g_caravan || g_siteRoll || g_siteTarget) return;
    if (!g_cfg.padScreen[0] && !g_cfg.labelScreen[0]) return;   // only while a screen key is set
    const unsigned char *tlo = nullptr, *thi = nullptr, *ilo = nullptr, *ihi = nullptr;
    if (!textRange(&tlo, &thi) || !imageRange(&ilo, &ihi)) {
        siteOff("the exe section table could not be read", false);
        return;
    }
    SiteRead r = {};
    if (!siteReadSeh(g_caravan, thi, &r)) {
        siteOff("the Hud around the caravan could not be read", false);
        return;
    }
    auto inText = [&](const void* p) {
        return (const unsigned char*)p >= tlo && (const unsigned char*)p < thi;
    };
    auto inData = [&](const void* p) {
        return (const unsigned char*)p >= ilo && (const unsigned char*)p < ihi && !inText(p);
    };
    if (!inData(r.hudVt) || !inData(r.rollVt) || !inText(r.hudDraw) || !inText(r.rollDraw)) {
        siteOff("the Hud's or the item rollover's vtable / Draw is not where the exe keeps them",
                false);
        return;
    }
    if (InterlockedCompareExchange(&g_siteOn, 0, 0)) {
        // installed in an earlier world: only this world's object is new
        if (r.rollDraw != (const void*)p_RollDraw) {
            siteOff("this world's item rollover has another Draw than the hooked one", false);
            return;
        }
        g_siteRoll = r.roll;
        return;
    }
    long cAt = -1, rAt = -1;
    const UtSiteProof pr =
        utHudSiteProof(g_siteCode, r.codeN, (unsigned)kHudCaravanOff, (unsigned)kHudRolloverOff,
                       &cAt, &rAt);
    if (pr != kUtSiteProved) {
        siteOff(utSiteProofText(pr), false);
        return;
    }
    const unsigned char* base = (const unsigned char*)GetModuleHandleW(nullptr);
    logD("buttons: the screen site - HUD Draw %p (exe rva 0x%llX) draws the caravan (+0x%llX) at "
         "+0x%lX and the item rollover (+0x%llX) at +0x%lX, after it; the rollover's Draw %p (exe "
         "rva 0x%llX, its vtable slot +0x20) installs on the next game tick",
         r.hudDraw, (unsigned long long)((const unsigned char*)r.hudDraw - base),
         (unsigned long long)kHudCaravanOff, cAt, (unsigned long long)kHudRolloverOff, rAt,
         r.rollDraw, (unsigned long long)((const unsigned char*)r.rollDraw - base));
    g_siteRollPending = r.roll;
    g_siteTarget = (void*)r.rollDraw;
}

void siteInstall() {
    void* target = g_siteTarget;
    if (!target) return;
    g_siteTarget = nullptr;
    if (InterlockedCompareExchange(&g_siteOff, 0, 0)) return;
    if (InterlockedCompareExchange(&g_siteOn, 0, 0)) return;
    MH_STATUS s = MH_CreateHook(target, (void*)&hk_RollDraw, (void**)&o_RollDraw);
    if (s == MH_ERROR_ALREADY_CREATED) s = o_RollDraw ? MH_OK : MH_ERROR_NOT_CREATED;
    if (s != MH_OK) {
        logE("  hook %-32s FAILED at MH_CreateHook: %s (%d)", "HUD item rollover::Draw", mhName(s),
             (int)s);
        siteOff("the item rollover's Draw detour could not be created", false);
        return;
    }
    MH_STATUS e = MH_EnableHook(target);
    if (e == MH_ERROR_ENABLED) e = MH_OK;
    if (e != MH_OK) {
        logE("  hook %-32s FAILED at MH_EnableHook: %s (%d)", "HUD item rollover::Draw", mhName(e),
             (int)e);
        siteOff("the item rollover's Draw detour could not be enabled", false);
        return;
    }
    p_RollDraw = (PfnChildDraw)target;
    g_siteRoll = g_siteRollPending;
    InterlockedExchange(&g_siteOn, 1);
    logD("  hook %-32s installed at %p (trampoline %p) - the pad and the label pinned to the "
         "screen are drawn at its head, one level below the tooltips",
         "HUD item rollover::Draw", target, (void*)o_RollDraw);
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
// The pure rule (ut_padmat.h utPadLeftDown) names its reasons in this order.
static_assert(PR_NOT_DRAWN == kUtPadPassNotDrawn && PR_CURSOR_ITEM == kUtPadPassCursorItem &&
                  PR_CURSOR_FAULT == kUtPadPassCursorFault && PR_NO_BUTTON == kUtPadPassNoButton &&
                  PR_IN_BOX == kUtPadPassInBox && PR_BOX_FAULT == kUtPadPassBoxFault &&
                  PR_NOT_PROVEN == kUtPadPassNotProven,
              "PassReason and ut_padmat.h UtPadPass must agree");

struct MouseAct {
    bool consume;
    bool keep;           // consumed with no action: a point of the materials page's pad
    bool matPad;         // the point lies inside the pad on the game's own materials page
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
    // DEBUG, once per reason per world: a pad click that reaches this hook and is NOT taken
    // says why here; a click that leaves no line at all never reached the materials window.
    logD("buttons: click passed through - %s", kPassWhy[reason]);
}

// What a click on the pad's logical button `btn` selects, for BOTH pads (the window's mouse slot
// and the screen input): MAT -> -1, the game's own materials page (the group lookup knows no group
// for logical 0, so without this rule MAT on the screen pad is "a button that maps to no group");
// OWN -> kUtButtonFilterTarget; EDIT -> kUtButtonEditTarget (the layout editor); a button of
// groups and NEXT -> panelButtonTarget's group; any other value < -1 = none.
int padClickTarget(int btn, const char** label) {
    LiveLayout lay;
    const int kind = liveLayoutGet(&lay)
                         ? utPadButtonKind(btn, lay.pages.groups, lay.pages.pages > 1 ? 1 : 0,
                                           lay.pages.edit)
                         : (btn == 0 ? (int)kUtPadKindMat : (int)kUtPadKindGroup);
    if (kind == kUtPadKindMat) {
        if (label) *label = "the game's own materials page";
        return -1;
    }
    if (kind == kUtPadKindEdit) {
        if (label) *label = "the layout editor";
        return kUtButtonEditTarget;
    }
    return panelButtonTarget(btn, label);
}

// The EDIT button's click, for both routes: the latch (panelEditButton) and one INFO line. The
// click is always the pad's (consumed): the editor opens on the pad's next frame.
void padEditClick(int btn, const char* route) {
    const int g = panelEditButton();
    if (g >= 0)
        logI("button [%s] clicked%s -> the layout editor, after the last collection group %d \"%s\" "
             "(the materials page has no layout to edit)",
             panelButtonTag(btn), route, g, liveGroupLabel(g));
    else if (g == -1)
        logI("button [%s] clicked%s -> the layout editor", panelButtonTag(btn), route);
    else
        logI("button [%s] clicked%s -> the layout editor: the live pager refused the last collection "
             "group, so nothing opens (on a collection page, EDIT or the [keys] editor_open chord "
             "opens it)",
             panelButtonTag(btn), route);
}

void mouseDecide(void* window, const void* evt, const float* offset, MouseAct* a) {
    if (window != g_window) return;
    if (!g_cfg.groupButtons) return;
    if (panelButtonsAreOff()) return;
    if (!evt || !offset) return;
    int type = 0;
    float ex = 0.0f, ey = 0.0f;
    if (!readMouseEvent(evt, &type, &ex, &ey)) return;
    float ofx = 0.0f, ofy = 0.0f, wx = 0.0f, wy = 0.0f;
    if (!readMouseOrigin(window, offset, &ofx, &ofy, &wx, &wy)) return;
    // The three lines the engine itself runs at exe 0x132917..0x13294C.
    const float lx = ex - (ofx + wx);
    const float ly = ey - (ofy + wy);
    const float s = liveUiScale();
    // On the game's own materials page the pad is drawn over the page's boxes and a point inside
    // it is the pad's, for every event type: hk_WndMouse puts the hovered-box answer back.
    a->matPad = panelMatPadAt(lx, ly, s);
    // never touch the rest - but a RIGHT DOWN on the materials page's pad, which the box under it
    // would act on (exe 0x132DD9: a box takes its item on type 2 as on type 1)
    if (type != kMouseTypeMove && type != kMouseTypeLeftDown &&
        !(type == kUtMouseRightDown && a->matPad))
        return;
    // outside the band (and off the materials page's pad): nothing to see or consume
    if (!a->matPad && !panelButtonsInBand(lx, ly, s)) return;
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

    if (type == kUtMouseRightDown) {
        // the materials page's pad: a right-click there is a click on no button - kept from the
        // box under it (utPadLeftDown with no button), or passed when the cursor holds an item
        const bool drawnR = panelButtonsDrawnRecently();
        unsigned int idR = 0;
        const bool readR = drawnR && readCursorItem(window, &idR);
        const UtPadClick c = utPadLeftDown(
            drawnR ? 1 : 0, readR ? 1 : 0, idR != 0 ? 1 : 0, 0, 1, inBox ? 1 : 0,
            boxFaulted ? 1 : 0, InterlockedCompareExchange(&g_btnSpaceProven, 0, 0) ? 1 : 0);
        if (c.act == kUtPadClickKeep) {
            a->consume = true;
            a->keep = true;
            if (a->probe) {   // no probe line for a kept click: give the slot back
                a->probe = false;
                InterlockedDecrement(&g_btnProbes);
            }
        }
        return;
    }
    if (type != kMouseTypeLeftDown) return;   // a MOVE is only ever a probe

    // THE RULE (ut_padmat.h utPadLeftDown, proved by test_layoutedit): drawn, cursor, then on a
    // collection page button, box, proven; inside the materials page's pad: proven, then the
    // button acts or the click is kept - the box under the pad never sees it.
    const bool drawn = panelButtonsDrawnRecently();
    unsigned int cursorId = 0;
    const bool cursorRead = drawn && readCursorItem(window, &cursorId);
    const UtPadClick c = utPadLeftDown(
        drawn ? 1 : 0, cursorRead ? 1 : 0, cursorId != 0 ? 1 : 0, btn >= 0 ? 1 : 0,
        a->matPad ? 1 : 0, inBox ? 1 : 0, boxFaulted ? 1 : 0,
        InterlockedCompareExchange(&g_btnSpaceProven, 0, 0) ? 1 : 0);
    if (c.act == kUtPadClickPass) {
        a->reason = c.reason;
        return;
    }
    const bool keepIt = c.act == kUtPadClickKeep;
    const char* label = nullptr;
    const int target = keepIt ? -2 : padClickTarget(btn, &label);
    // kUtButtonFilterTarget is the OWN button and kUtButtonEditTarget the EDIT button - legal
    // targets that select no group. Every other value below -1 is an index the table does not know.
    if (!keepIt && target < -1 && target != kUtButtonFilterTarget && target != kUtButtonEditTarget) {
        if (utPadRefusedAct(a->matPad ? 1 : 0) == kUtPadClickPass) {
            a->reason = PR_NO_BUTTON;
            return;
        }
        a->keep = true;   // the materials page's pad: nothing acts, the box still never sees it
    }
    a->consume = true;
    if (keepIt) a->keep = true;
    // The consumed click is reported by its own line, so the probe line is suppressed - and the
    // probe slot is given back, or the suppressed line makes the numbering skip.
    if (a->probe) {
        a->probe = false;
        InterlockedDecrement(&g_btnProbes);
    }
    a->button = btn;
    a->target = target;
    a->btnLabel = label ? label : "?";
}

void mouseDecideGuarded(void* window, const void* evt, const float* offset, MouseAct* a) {
    __try {
        mouseDecide(window, evt, offset, a);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        a->consume = false;
        a->keep = false;
        a->matPad = false;
        a->probe = false;
        a->reason = PR_NONE;
        panelButtonsOff("the mouse handler's decision path faulted");
    }
}

// ---- the layout editor's input (ut_panel) ----------------------------------------------------
// The editor chord ([keys] editor_open, Ctrl+RightClick by default: its click with all of its
// modifiers held) on the pad opens the editor; while it is open LEFT DOWN on its toolbar, a
// handle or a rectangle is consumed and LEFT UP ends a drag (passed on). Between the two the drag
// follows the cursor polled from the Draw tail, so it survives the cursor leaving the caravan.
// Every other event, and every event while the editor is closed but the chord, is untouched.
bool editMouse(void* window, const void* evt, const float* offset) {
    if (window != g_window || !evt || !offset) return false;
    if (!g_cfg.groupButtons || panelButtonsAreOff()) return false;
    int type = 0;
    float ex = 0.0f, ey = 0.0f;
    if (!readMouseEvent(evt, &type, &ex, &ey)) return false;
    const UtKeyBind editorOpen = g_cfg.keys.editorOpen;
    if (editorOpen.kind == kUtKeyClick && type == editorOpen.code) {
        const int mods = utKeyModsOf((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0,
                                     (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0,
                                     (GetAsyncKeyState(VK_MENU) & 0x8000) != 0);
        if (!utKeyClickMatches(editorOpen, type, mods)) return false;
    } else if (type != kUtMouseLeftDown && type != kUtMouseLeftUp) {
        return false;
    } else if (!panelLayoutEditOpen()) {
        return false;
    }
    if (!InterlockedCompareExchange(&g_drawOriginOk, 0, 0)) return false;
    float ofx = 0.0f, ofy = 0.0f, wx = 0.0f, wy = 0.0f;
    if (!readMouseOrigin(window, offset, &ofx, &ofy, &wx, &wy)) return false;
    const float lx = ex - (ofx + wx);
    const float ly = ey - (ofy + wy);
    bool cursorEmpty = false, inBox = false;
    if (type != kUtMouseLeftUp) {
        unsigned int cursorId = 0;
        cursorEmpty = readCursorItem(window, &cursorId) && cursorId == 0;
        bool boxFaulted = false;
        inBox = plateBoxHitLocal(lx, ly, &boxFaulted) || boxFaulted;
    }
    const bool proven = InterlockedCompareExchange(&g_btnSpaceProven, 0, 0) != 0;
    return panelLayoutEditMouse(type, lx, ly, cursorEmpty, inBox, proven);
}

bool editMouseGuarded(void* window, const void* evt, const float* offset) {
    __try {
        return editMouse(window, evt, offset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        panelButtonsOff("the layout editor's mouse path faulted");
        return false;
    }
}

// The hovered-widget answer the caravan reads after the page's handler (`*out`, written by the
// original at exe 0x133104 only when a box is under the point, read by caravan->vt[0xD0] at
// 0x135E23 - the tooltip). On the materials page's pad it is put back to what the caravan had
// before the page ran - exactly the answer for a point on no box - so a box the pad covers shows
// no tooltip. A fault reads or writes nothing more.
bool readOutSeh(void** out, void** v) {
    __try {
        if (!out) return false;
        *v = *out;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void writeOutSeh(void** out, void* v) {
    __try {
        if (out) *out = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool __cdecl hk_WndMouse(void* window, const void* evt, const float* offset, void** out) {
    if (editMouseGuarded(window, evt, offset)) return false;   // the layout editor took it
    MouseAct a;
    memset(&a, 0, sizeof(a));
    a.reason = PR_NONE;
    a.button = -1;
    a.target = -2;
    a.btnLabel = "?";
    mouseDecideGuarded(window, evt, offset, &a);
    // The event is consumed only when the write actually happened. liveSelectGroup refuses for
    // `!liveActive()`, an empty group list and an out-of-range index, and panelButtonTarget only
    // maps a logical button (ut_padpage.h) onto a group index - so a shorter catalogue, or
    // live_pages flipped to 0 inside the 500 ms "drawn recently" window, must not eat the click
    // and log a group change that never happened: the engine's own handler runs instead and the
    // reason is logged once.
    // The OWN button toggles the owned-only filter instead of selecting a group.
    // Both writes are mod-side ints plus a relayout request - no engine call, nothing allocated -
    // so either is safe from inside this detour body, and either may REFUSE (live paging not
    // armed), in which case the engine's own handler runs after all and the reason is logged.
    if (a.consume && a.keep) {
        // The materials page's pad, on no button (or a button whose target is refused): nothing
        // acts, and the box under the pad never sees the click.
        if (!(InterlockedOr(&g_matKeepLogged, 1) & 1))
            logD("buttons: a click on the materials page's pad, on no button, was kept from the "
                 "box under it");
        return false;
    }
    if (a.consume && a.target == kUtButtonEditTarget) {
        // The EDIT button: always the pad's click; the editor opens on the pad's next frame.
        panelButtonsNotePress(a.button);
        padEditClick(a.button, "");
        logD("the click was consumed; the engine's own handler was not called");
        return false;
    }
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
    if (a.consume && a.matPad) {
        // the pager refused the group: on the materials page's pad the click stays the pad's
        // (ut_padmat.h utPadRefusedAct), the reason is logged once per world
        if (!(InterlockedOr(&g_matKeepLogged, 2) & 2))
            logD("buttons: the live pager refused the group of a click on the materials page's "
                 "pad - the click was kept from the box under it");
        return false;
    }
    if (a.consume) a.reason = PR_SELECT_REFUSED;
    void* hovered = nullptr;
    const bool keepHover = a.matPad && readOutSeh(out, &hovered);
    bool r = false;
    if (o_WndMouse) {
        r = o_WndMouse(window, evt, offset, out);
    } else if (p_WndMouse) {
        r = p_WndMouse(window, evt, offset, out);
    }
    if (keepHover) writeOutSeh(out, hovered);   // the pad's hover, not the covered box's
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

// ---- the SCREEN pad's input: DirectInputDevice::GetMouseEvent ------------------------------
// A pad or label drawn OUTSIDE the caravan cannot be clicked through the mouse slot above:
// CaravanWindow's slot +0x38 rect-tests itself and returns false for every point outside it.
// Every mouse event of the game is first copied out by ONE exported function - DirectInput.dll's
// `DirectInputDevice::GetMouseEvent` (the device's vtable slot +0x38; `Engine::ProcessUserInput`
// at Engine 0x78EC2 is its only caller) - into the pump's own
// stack copy, which the UI root (exe 0xBEF10: HUD, windows, minimap, tracker) and then the WORLD
// VIEW (exe 0x21010) read. The detour calls the original and then lets ut_padinput.h decide what
// that copy says: untouched, REWRITTEN (type -> MOVE, held bytes 0x10..0x11 cleared - the click
// is consumed for every consumer at once, hover and tooltips keep working) or CLEARED (an event
// passed on during a press: the world acts on the held bytes of every event type).
//
//   binding   `dinput.getMouseEvent` (EXPORT, LATE, critical for this route only): both exports
//             by decorated name, cross-checked against `Engine::GetInputDevice()`'s live vtable
//             (slot +0x38 == GetMouseEvent, +0x10 == Update) and the export's own `imul ..,0x1C`
//             naming the record the detour rewrites. Resolved from the game thread (the device
//             exists once the engine initialised input; DirectInput.dll is loaded by name), 40
//             tries 500 ms apart; a refusal turns ONLY this route off.
//   install   from plateTick, never from a detour body, once a world has captured the materials
//             window (never at the main menu), both detours or none. `Update` only counts the
//             device's generations - the idempotence key: the pump asks each index once per
//             generation, and a repeat of (generation, index) replays the bytes, never the action.
//   inert     until the screen draw site publishes fresh rectangles (plateScreenRectsPublish) while
//             the caravan is open AND the event space is proven (a probe agreed with GetCursorPos,
//             or the in-window pad's proof): with nothing published the body returns after a few
//             int reads and the event is the original's copy, byte for byte (test_padinput section
//             3). A press already open runs to its UP (or the polled release) whatever happens.
//   latch     a fault inside the body = one ERROR and the route off for the session; the detours
//             stay installed and pass everything through.
//   actions   padClickTarget (MAT -> the materials page, else panelButtonTarget) -> liveSelectGroup
//             / liveToggleOwnedOnly, exactly hk_WndMouse's;
//             mod-side ints plus a relayout request, no engine call. The pump runs on the thread
//             of the Draw tail and the mouse slot (fact 11); the first detour call logs its id.
//   MP        nothing network-visible: the detour edits the LOCAL copy of a local input event
//             before any local consumer reads it; a consumed click simply never becomes a move or
//             a skill command, exactly as a click on any game window never does. No packet, no
//             item, no character state is touched.
typedef void* (*PfnDevGetMouseEvent)(const void* dev, void* out, int idx);
typedef void (*PfnDevUpdate)(void* dev);
typedef void* (*PfnEngineGetInputDevice)(void* engine);

const char kExpDevGetMouseEvent[] =
    "?GetMouseEvent@DirectInputDevice@GAME@@UEBA?AUMouseEvent@InputDevice@2@H@Z";
const char kExpDevUpdate[] = "?Update@DirectInputDevice@GAME@@UEAAXXZ";
const char kExpEngineGetInputDevice[] = "?GetInputDevice@Engine@GAME@@QEAAPEAVInputDevice@2@XZ";
const size_t kSlotDevUpdate = 0x10;
const size_t kSlotDevGetMouseEvent = 0x38;
const LONG kScrResolveTries = 40;
const LONG kScrProbeCap = 20;

PfnDevGetMouseEvent o_DevGetMouseEvent = nullptr;
PfnDevUpdate o_DevUpdate = nullptr;
void* g_scrEvtTarget = nullptr;      // GetMouseEvent, resolved AND cross-checked
void* g_scrUpdTarget = nullptr;      // Update, the same
volatile LONG g_scrResolved = 0;     // 0 pending, 1 confirmed, -1 refused
volatile LONG g_scrTries = 0;
volatile LONG g_scrHookOn = 0;
volatile LONG g_scrOff = 0;          // the SESSION latch
volatile LONG64 g_scrGen = 0;        // the device's Update generation
volatile LONG g_scrFirstCall = 0;
volatile LONG g_scrReset = 0;        // a world teardown asks the input thread to drop its press
volatile LONG g_scrProbes = 0;       // per world
volatile LONG g_scrProven = 0;       // per world
volatile LONG g_scrLogged = 0;       // bitmask: once-per-world lines
UtPadInState g_scrState;             // the pump's thread only
UtPadScreenRects g_scrSnap;             // the pump's thread only (2.6 KB, off the stack)
// The published rectangles, behind a sequence lock (odd = being written).
volatile LONG g_scrSeq = 0;
UtPadScreenRects g_scrRects;

void scrOff(const char* why) {
    if (InterlockedExchange(&g_scrOff, 1)) return;
    logE("screen pad input: OFF for this session - %s. The detours stay installed and pass every "
         "event through untouched; the in-window pad is not affected.", why ? why : "?");
}

void scrLogOnce(LONG bit, const char* text) {
    if (InterlockedOr(&g_scrLogged, bit) & bit) return;
    logD("screen pad input: %s", text);
}

bool scrSnapshot(UtPadScreenRects* out) {
    for (int i = 0; i < 4; ++i) {
        const LONG a = InterlockedCompareExchange(&g_scrSeq, 0, 0);
        if (a & 1) continue;
        *out = g_scrRects;
        if (InterlockedCompareExchange(&g_scrSeq, 0, 0) == a) return true;
    }
    return false;
}

// The event-space proof, the in-window probe's rule: the event's point against the mod's own
// GetCursorPos + ScreenToClient, within 2 px on both axes. Up to kScrProbeCap MOVEs per world,
// only while the screen pad is shown.
void scrProbe(float ex, float ey) {
    if (InterlockedCompareExchange(&g_scrProven, 0, 0)) return;
    if (InterlockedCompareExchange(&g_scrProbes, 0, 0) >= kScrProbeCap) return;
    POINT pt = {0, 0};
    HWND h = GetActiveWindow();
    if (!h) h = GetForegroundWindow();
    if (!h || !GetCursorPos(&pt) || !ScreenToClient(h, &pt)) return;
    const LONG n = InterlockedIncrement(&g_scrProbes);
    const float dx = (float)pt.x - ex;
    const float dy = (float)pt.y - ey;
    if (dx > -2.0f && dx < 2.0f && dy > -2.0f && dy < 2.0f) {
        if (!InterlockedExchange(&g_scrProven, 1))
            logD("screen pad input: consumption ENABLED - the event space is proven (probe %ld, "
                 "delta=(%.1f,%.1f) px)", n, dx, dy);
    } else if (n == kScrProbeCap) {
        logD("screen pad input: consumption NOT PROVEN after %ld probes (last delta=(%.1f,%.1f) "
             "px) - the screen pad stays draw-only this world unless the in-window pad proves it",
             n, dx, dy);
    }
}

void scrButton(int btn) {
    const char* label = nullptr;
    const int target = padClickTarget(btn, &label);
    if (target == kUtButtonEditTarget) {
        panelButtonsNotePress(btn);
        padEditClick(btn, " on the screen pad");
        return;
    }
    const bool isFilter = target == kUtButtonFilterTarget;
    if (target < -1 && !isFilter) {
        scrLogOnce(1, "a click on a screen pad button that maps to no group was consumed with no "
                      "action");
        return;
    }
    if (isFilter ? liveToggleOwnedOnly() : liveSelectGroup(target)) {
        panelButtonsNotePress(btn);
        if (isFilter) {
            logI("button [%s] clicked on the screen pad -> the owned-only filter is now %s",
                 panelButtonTag(btn),
                 liveOwnedOnly() ? "ON: only records you own are shown" : "OFF");
        } else if (target < 0) {
            logI("button [%s] clicked on the screen pad -> %s", panelButtonTag(btn),
                 label ? label : "the game's own materials page");
        } else {
            logI("button [%s] clicked on the screen pad -> group %d \"%s\"", panelButtonTag(btn),
                 target, label ? label : "?");
        }
        return;
    }
    scrLogOnce(2, "the live pager refused the target of a screen pad click - consumed with no "
                  "action (outside the caravan there is no engine handler to fall back to)");
}

// The layout editor gets what the in-window editor gets: panelLayoutEditMouse at the point in
// window-local screen px (the event minus the origin the engine's own Draw computed).
// kUtEvtEditorNote is the machine's note of a LEFT DOWN it passed untouched inside the caravan
// window (the window's mouse slot gets that click next and proves its own space against it).
void scrEditor(int type, float x, float y, bool cursorItem) {
    if (!InterlockedCompareExchange(&g_drawOriginOk, 0, 0)) return;
    const float lx = x - g_lastDrawOx;
    const float ly = y - g_lastDrawOy;
    bool inBox = false;
    if (type == kUtEvtLeftDown || type == kUtEvtRightDown) {
        bool faulted = false;
        inBox = plateBoxHitLocal(lx, ly, &faulted) || faulted;
    }
    panelLayoutEditMouse(type, lx, ly, !cursorItem, inBox, true);
}

void scrBody(unsigned char* out, int idx) {
    if (!InterlockedExchange(&g_scrFirstCall, 1))
        logD("screen pad input: the first event through the GetMouseEvent detour, thread t%lu",
             GetCurrentThreadId());
    if (InterlockedExchange(&g_scrReset, 0)) utPadInReset(&g_scrState);
    const bool pressed = g_scrState.press != kUtPressNone;
    // Never act while the caravan is closed: the rectangles of a pad that was painted a frame ago
    // are not trusted once the stash has gone (g_stashOpen is the last IsTransferOpen seen).
    const bool stashOpen = InterlockedCompareExchange(&g_stashOpen, 0, 0) != 0;
    // THE ADOPTED DRAG (ut_padinput.h): the layout editor carries a drag that began in the window
    // - the machine takes it whatever is published (a pad still inside the window publishes no
    // ground), so no held byte of it reaches the world and its UP reaches the editor.
    const bool editorDrag = !pressed && panelLayoutEditDragging();
    if (!pressed && !editorDrag) {
        // The cheap exit: nothing fresh published = the original's copy, untouched.
        if (!stashOpen || !g_scrRects.shown) return;
        const unsigned long long t = g_scrRects.tick;
        const unsigned long long now = hookFrameCount();
        if (t == 0 || now < t || now - t > kUtPadScreenStaleTicks) return;
    }
    const bool have = scrSnapshot(&g_scrSnap);
    if (!have) g_scrSnap.shown = 0;
    const UtPadInEvent e = utPadReadEvent(out);
    UtPadInGates g;
    memset(&g, 0, sizeof(g));
    g.fresh = stashOpen && have && utPadScreenRectsFresh(g_scrSnap, hookFrameCount()) ? 1 : 0;
    if (g.fresh && e.type == kUtEvtMove) scrProbe(e.x, e.y);
    g.proven = (InterlockedCompareExchange(&g_scrProven, 0, 0) ||
                InterlockedCompareExchange(&g_btnSpaceProven, 0, 0))
                   ? 1
                   : 0;
    g.editorOpen = panelLayoutEditOpen() ? 1 : 0;
    bool cursorItem = false;
    if (g.fresh && (e.type == kUtEvtLeftDown || e.type == kUtEvtRightDown)) {
        // Unknown is an item: a fault or no captured window never lets a click through.
        unsigned int id = 0;
        cursorItem = !(g_window && readCursorItem(g_window, &id)) || id != 0;
    }
    g.cursorItem = cursorItem ? 1 : 0;
    // The editor chord: [keys] editor_open's click type and modifiers (0 = bound to a key, or
    // none), and the modifiers held - read only for a DOWN of that type.
    const UtKeyBind editorOpen = g_cfg.keys.editorOpen;
    g.editorType = editorOpen.kind == kUtKeyClick ? editorOpen.code : 0;
    g.editorMods = editorOpen.mods;
    if (g.editorType != 0 && e.type == g.editorType)
        g.mods = utKeyModsOf((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0,
                             (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0,
                             (GetAsyncKeyState(VK_MENU) & 0x8000) != 0);
    g.editorDrag = editorDrag ? 1 : 0;
    // The open editor's window rect - a point inside it is the window's own (untouched;
    // a LEFT DOWN there only noted to the editor), whatever the grounds say.
    g.windowFresh = stashOpen && have && utPadScreenWindowFresh(g_scrSnap, hookFrameCount()) ? 1 : 0;
    if (pressed || editorDrag) {
        // GetAsyncKeyState reads the PHYSICAL buttons: with swapped buttons the logical left is
        // the physical right.
        const bool sw = GetSystemMetrics(SM_SWAPBUTTON) != 0;
        g.leftUp = (GetAsyncKeyState(sw ? VK_RBUTTON : VK_LBUTTON) & 0x8000) ? 0 : 1;
        g.rightUp = (GetAsyncKeyState(sw ? VK_LBUTTON : VK_RBUTTON) & 0x8000) ? 0 : 1;
    }
    const UtPadInOut o = utPadInput(
        &g_scrState, (unsigned long long)InterlockedCompareExchange64(&g_scrGen, 0, 0), idx, e,
        g_scrSnap, g);
    utPadApply(out, o);
    if (o.act == kUtPadActButton) {
        scrButton(o.button);
    } else if (o.act == kUtPadActEditor) {
        scrEditor(o.editorType, e.x, e.y, cursorItem);
    } else if (o.act == kUtPadActEditorNote) {
        scrEditor(kUtEvtEditorNote, e.x, e.y, cursorItem);
    }
    if (o.repeat) return;
    if (o.why == kUtPadWhyCursorItem)
        scrLogOnce(4, "a click with an item on the cursor was consumed - no action, the item "
                      "stays on the cursor");
    if (o.why == kUtPadWhyGround)
        scrLogOnce(8, "a click on the screen pad's ground or the label was consumed - no action");
    if (o.ended == 2)
        scrLogOnce(16, "a press ended on the polled release (its UP event never arrived)");
}

void scrGuarded(unsigned char* out, int idx) {
    __try {
        scrBody(out, idx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        scrOff("the GetMouseEvent detour body faulted");
    }
}

// The original ALWAYS runs first with the caller's arguments; only its copy is edited.
void* hk_DevGetMouseEvent(const void* dev, void* out, int idx) {
    void* r = o_DevGetMouseEvent(dev, out, idx);
    if (out && !InterlockedCompareExchange(&g_scrOff, 0, 0)) scrGuarded((unsigned char*)out, idx);
    return r;
}

void hk_DevUpdate(void* dev) {
    o_DevUpdate(dev);
    InterlockedIncrement64(&g_scrGen);
}

bool scrReadDevice(PfnEngineGetInputDevice gid, void** dev, void** slotUpd, void** slotEvt) {
    __try {
        GdEngine* eng = g_gd.ppEngine ? *g_gd.ppEngine : nullptr;
        if (!eng) return false;
        void* d = gid(eng);
        if (!d) return false;
        void* const* vt = *(void* const* const*)d;
        *dev = d;
        *slotUpd = vt[kSlotDevUpdate / sizeof(void*)];
        *slotEvt = vt[kSlotDevGetMouseEvent / sizeof(void*)];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int scrReadStride(const void* fn) {
    unsigned char b[16];
    __try {
        memcpy(b, fn, sizeof(b));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return utPadDecodeEventStride(b, sizeof(b));
}

// A refused binding is not a fault: the late report names the row (one ERROR, the bindings'
// own), and this WARN says what it means for the player.
void scrRefuse(const char* why) {
    InterlockedExchange(&g_scrResolved, -1);
    InterlockedExchange(&g_scrOff, 1);
    bindingsNote("dinput.getMouseEvent", 0, false, why);
    bindingsLateReport();
    logW("screen pad input: not installed - %s. A pad or label placed off the caravan is drawn "
         "but takes no click; the in-window pad is not affected.", why ? why : "?");
}

// GAME THREAD (plateTick). Reads only: two GetProcAddress, one exported getter, two vtable slots
// and 16 code bytes.
void scrResolveTick() {
    if (InterlockedCompareExchange(&g_scrResolved, 0, 0) != 0) return;
    static DWORD last = 0;
    const DWORD now = GetTickCount();
    if (now - last < 500) return;
    last = now;
    const LONG tries = InterlockedIncrement(&g_scrTries);
    HMODULE di = GetModuleHandleW(L"DirectInput.dll");
    HMODULE en = GetModuleHandleW(L"Engine.dll");
    void* fe = di ? (void*)GetProcAddress(di, kExpDevGetMouseEvent) : nullptr;
    void* fu = di ? (void*)GetProcAddress(di, kExpDevUpdate) : nullptr;
    const PfnEngineGetInputDevice gid =
        en ? (PfnEngineGetInputDevice)(void*)GetProcAddress(en, kExpEngineGetInputDevice) : nullptr;
    void *dev = nullptr, *su = nullptr, *se = nullptr;
    const char* why = nullptr;
    if (!di) {
        why = "DirectInput.dll is not loaded";
    } else if (!fe || !fu) {
        why = "DirectInputDevice::GetMouseEvent / ::Update are not exported by those names";
    } else if (!gid) {
        why = "Engine::GetInputDevice is not exported by that name";
    } else if (!scrReadDevice(gid, &dev, &su, &se)) {
        why = "Engine::GetInputDevice() has no device (or its vtable is unreadable)";
    }
    if (why) {
        if (tries < kScrResolveTries) return;   // the device appears once the engine has input
        scrRefuse(why);
        return;
    }
    const int stride = scrReadStride(fe);
    if (se != fe) {
        scrRefuse("Engine::GetInputDevice()'s vtable slot +0x38 is not the exported GetMouseEvent");
        return;
    }
    if (su != fu) {
        scrRefuse("Engine::GetInputDevice()'s vtable slot +0x10 is not the exported Update");
        return;
    }
    if (stride != (int)kUtMouseEventSize) {
        scrRefuse("GetMouseEvent's own imul does not name a 0x1C-byte MouseEvent");
        return;
    }
    g_scrEvtTarget = fe;
    g_scrUpdTarget = fu;
    InterlockedExchange(&g_scrResolved, 1);
    const unsigned long long rva =
        (unsigned long long)((const unsigned char*)fe - (const unsigned char*)di);
    bindingsNote("dinput.getMouseEvent", rva, true, nullptr);
    bindingsLateReport();
    logD("screen pad input: DirectInputDevice::GetMouseEvent %p (DirectInput.dll rva 0x%llX) == "
         "slot +0x38 and ::Update %p == slot +0x10 of Engine::GetInputDevice() %p; the event "
         "stride is 0x%X - confirmed on t%lu, the detours install once a world exists",
         fe, rva, fu, dev, stride, GetCurrentThreadId());
}

// GAME THREAD (plateTick). Both detours or none; never from inside a detour body.
void scrInstall() {
    if (InterlockedCompareExchange(&g_scrResolved, 0, 0) != 1) return;
    if (InterlockedCompareExchange(&g_scrHookOn, 0, 0)) return;
    if (InterlockedCompareExchange(&g_scrOff, 0, 0)) return;
    if (!g_window) return;   // never at the main menu: a world has captured the materials window
    // Only for a player who pins the pad or the label to the screen, or while the layout
    // editor is open: a player with no screen key yet who drags a part out of the window
    // for the first time needs the screen input for the adopted drag, the toolbar that travels
    // onto the screen and SAVE there. Otherwise there is no DirectInput detour at all. Set
    // mid-session (a key, or the editor opened), it installs on the next tick; once installed it
    // stays for the session, inert while nothing is published (scrBody's cheap exit).
    if (!g_cfg.padScreen[0] && !g_cfg.labelScreen[0] && !panelLayoutEditOpen()) return;
    utPadInReset(&g_scrState);
    MH_STATUS s = MH_CreateHook(g_scrUpdTarget, (void*)&hk_DevUpdate, (void**)&o_DevUpdate);
    if (s == MH_ERROR_ALREADY_CREATED) s = o_DevUpdate ? MH_OK : MH_ERROR_NOT_CREATED;
    if (s != MH_OK) {
        logE("  hook %-32s FAILED at MH_CreateHook: %s (%d)", "DirectInputDevice::Update",
             mhName(s), (int)s);
        scrOff("the DirectInputDevice::Update detour could not be created");
        return;
    }
    s = MH_CreateHook(g_scrEvtTarget, (void*)&hk_DevGetMouseEvent, (void**)&o_DevGetMouseEvent);
    if (s == MH_ERROR_ALREADY_CREATED) s = o_DevGetMouseEvent ? MH_OK : MH_ERROR_NOT_CREATED;
    if (s != MH_OK) {
        logE("  hook %-32s FAILED at MH_CreateHook: %s (%d)", "DirectInputDevice::GetMouseEvent",
             mhName(s), (int)s);
        scrOff("the DirectInputDevice::GetMouseEvent detour could not be created");
        return;
    }
    // Update first: a generation counter that runs before the first event is harmless.
    MH_STATUS e = MH_EnableHook(g_scrUpdTarget);
    if (e == MH_ERROR_ENABLED) e = MH_OK;
    if (e != MH_OK) {
        logE("  hook %-32s FAILED at MH_EnableHook: %s (%d)", "DirectInputDevice::Update",
             mhName(e), (int)e);
        scrOff("the DirectInputDevice::Update detour could not be enabled");
        return;
    }
    e = MH_EnableHook(g_scrEvtTarget);
    if (e == MH_ERROR_ENABLED) e = MH_OK;
    if (e != MH_OK) {
        MH_DisableHook(g_scrUpdTarget);
        logE("  hook %-32s FAILED at MH_EnableHook: %s (%d)", "DirectInputDevice::GetMouseEvent",
             mhName(e), (int)e);
        scrOff("the DirectInputDevice::GetMouseEvent detour could not be enabled");
        return;
    }
    InterlockedExchange(&g_scrHookOn, 1);
    logD("  hook %-32s installed at %p (trampoline %p), ::Update at %p (trampoline %p) on t%lu - "
         "the screen pad's input, inert until the screen pad publishes its rectangles",
         "DirectInputDevice::GetMouseEvent", g_scrEvtTarget, (void*)o_DevGetMouseEvent,
         g_scrUpdTarget, (void*)o_DevUpdate, GetCurrentThreadId());
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
// The marks as a bitset over the group table, any size: bit i = collection group i.
struct GroupBits {
    std::vector<unsigned long long> w;
    void clear() { w.clear(); }
    bool test(int i) const {
        return i >= 0 && (size_t)(i >> 6) < w.size() && ((w[(size_t)(i >> 6)] >> (i & 63)) & 1ull);
    }
    void set(int i) {
        if (i < 0) return;
        const size_t k = (size_t)(i >> 6);
        try {
            if (k >= w.size()) w.resize(k + 1, 0ull);
        } catch (...) {
            return;   // a mark lost, never a wrong one
        }
        w[k] |= 1ull << (i & 63);
    }
    int bits() const { return (int)w.size() * 64; }
};
GroupBits g_sweepMask;              // GAME THREAD ONLY
int g_sweepScanned = 0;
int g_sweepMatched = 0;
int g_sweepSkipped = 0;
int g_sweepTicks = 0;
DWORD g_sweepStartedAt = 0;
std::vector<std::string>* g_sweepRecs = nullptr;
// The PUBLISHED marks: bit i = group i holds a match. Written by the game thread, read by the
// draw under the shared lock (no allocation, no engine call).
SRWLOCK g_maskLock = SRWLOCK_INIT;
GroupBits g_maskPub;                // under g_maskLock
volatile LONG g_maskValid = 0;      // 0 until a sweep has completed for the current needle
volatile LONG g_curHitGroup = -1;   // tier B: the group on screen, from the engine's own answer
volatile LONG g_maskLive = 0;       // the first 32 groups' published bits - the status lines only

void maskPublish(const GroupBits* b) {
    AcquireSRWLockExclusive(&g_maskLock);
    try {
        if (b) {
            g_maskPub.w = b->w;
        } else {
            g_maskPub.clear();
        }
    } catch (...) {
        g_maskPub.clear();
    }
    InterlockedExchange(&g_maskLive, g_maskPub.w.empty() ? 0 : (LONG)(unsigned long)g_maskPub.w[0]);
    ReleaseSRWLockExclusive(&g_maskLock);
}
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
GroupBits g_sweepMaskA;                 // the mask as tier A left it, for the per-tier log line
bool g_sweepADone = false;              // tier A finished for this needle
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
    maskPublish(nullptr);
    InterlockedExchange(&g_maskValid, 0);
    InterlockedExchange(&g_curHitGroup, -1);
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
            maskPublish(nullptr);
            InterlockedExchange(&g_curHitGroup, -1);
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
    maskPublish(nullptr);
    if (n == 0) InterlockedExchange(&g_curHitGroup, -1);
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
        if (g_needleLen > 0 && shown >= 0 && readMatchedNotEmpty(window, &hit) && hit) {
            InterlockedExchange(&g_curHitGroup, (LONG)shown);
        } else {
            InterlockedExchange(&g_curHitGroup, -1);
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
        g_sweepMask.clear();
        g_sweepScanned = 0;
        g_sweepMatched = 0;
        g_sweepSkipped = 0;
        g_sweepTicks = 0;
        g_sweepStartedAt = GetTickCount();
        // ---- arm tier C for this needle ----------------------------------------------------
        // Skipped, and the cursor left "done", whenever the key is off, the owned-only filter is
        // on (the marks respect that filter by design, and with the filter on there are no
        // unowned records on the page to mark), the group file has not loaded, or the needle
        // folded to nothing.
        g_sweepMaskA.clear();
        g_sweepADone = false;
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
        if (grp < 0) continue;
        if (g_sweepMask.test(grp)) continue;   // that button is already marked
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
                g_sweepMask.set(grp);
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
            g_sweepMask.set(grp);
            ++g_sweepMatched;
        }
    }
    if (g_sweepAt < (int)g_sweepRecs->size()) return;   // more tier A next tick - still UNLIT
    if (!g_sweepADone) {
        g_sweepADone = true;
        try {
            g_sweepMaskA = g_sweepMask;
        } catch (...) {
            g_sweepMaskA.clear();
        }
        // Publish the OWNED answer the moment tier A finishes. It is complete on its own, so
        // tier C costs the owned half none of its responsiveness even though it triples the
        // record count. Tier C only ever ADDS groups to the same mask, so the marks grow and
        // never retract, and "never over-report" holds at every instant.
        maskPublish(&g_sweepMask);
        InterlockedExchange(&g_maskValid, 1);
    }
    // ---- TIER C, on the same 32-per-tick budget as tier A ----------------------------------
    // Group by group, so a group tier A (or an earlier tier-C record) has already marked costs
    // nothing at all, and so the cursor is two small ints instead of a flattened index.
    if (!g_sweepCDone && g_tierC) {
        while (budget > 0 && g_sweepCGroup < (int)g_tierC->size()) {
            std::vector<TierCRec>& grp = (*g_tierC)[(size_t)g_sweepCGroup];
            if (g_sweepMask.test(g_sweepCGroup) ||
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
            if (matched) g_sweepMask.set(g_sweepCGroup);
        }
        if (g_sweepCGroup >= (int)g_tierC->size()) g_sweepCDone = true;
        if (!g_sweepCDone) return;   // more tier C next tick - still UNLIT
    }
    maskPublish(&g_sweepMask);
    InterlockedExchange(&g_maskValid, 1);
    const LONG n = InterlockedIncrement(&g_sweeps);
    int groups = 0;
    for (int b = 0; b < g_sweepMask.bits(); ++b) {
        if (g_sweepMask.test(b)) ++groups;
    }
    // How many groups each tier is responsible for. Tier A is
    // the mask as it stood when the owned pass finished; tier C is everything the unowned pass
    // added on top; tier B is the separate, free, on-screen group `plateSearchMarked` adds.
    int groupsA = 0, groupsC = 0;
    for (int b = 0; b < g_sweepMask.bits(); ++b) {
        if (g_sweepMaskA.test(b)) ++groupsA;
        else if (g_sweepMask.test(b)) ++groupsC;
    }
    logT("search: sweep #%ld done - %d character(s) typed, %d group(s) marked (tier A owned %d, "
         "tier C unowned %d, tier B on-screen group %d) | tier A: %zu owned record(s), %d asked of "
         "Item::SearchText, %d matched, %d skipped | tier C: %s, %d asked of Item::SearchText "
         "EXACTLY (shown this world), %d matched by catalogue NAME only, %d skipped (owned, or "
         "no name yet); %d record(s) indexed, %ld named | %d tick(s), %lu ms",
         n, (int)InterlockedCompareExchange(&g_needleChars, 0, 0), groups, groupsA, groupsC,
         (int)InterlockedCompareExchange(&g_curHitGroup, -1, -1), g_sweepRecs->size(),
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
void plateScreenRectsPublish(const UtPadScreenRects* r) {
    InterlockedIncrement(&g_scrSeq);   // odd: being written
    if (r && r->shown) {
        g_scrRects = *r;
        if (g_scrRects.buttonCount < 0) g_scrRects.buttonCount = 0;
        if (g_scrRects.buttonCount > kUtPadScreenMaxButtons)
            g_scrRects.buttonCount = kUtPadScreenMaxButtons;
        const unsigned long long f = hookFrameCount();
        g_scrRects.tick = f ? f : 1;
    } else {
        g_scrRects.shown = 0;
    }
    InterlockedIncrement(&g_scrSeq);   // even: whole again
}

const char* plateScreenSiteName() {
    if constexpr (kUtScreenSiteChosen == kUtScreenSiteDrawTail) {
        return "the ReagentWindow::Draw tail (the fallback site, under the HUD drawn after the caravan)";
    } else {
        return "the head of the HUD's item-rollover Draw (one level below the tooltips)";
    }
}

const char* plateScreenInputState() {
    if (InterlockedCompareExchange(&g_scrOff, 0, 0)) {
        return InterlockedCompareExchange(&g_scrResolved, 0, 0) < 0 ? "refused" : "OFF (fault)";
    }
    if (InterlockedCompareExchange(&g_scrHookOn, 0, 0)) return "installed";
    switch (InterlockedCompareExchange(&g_scrResolved, 0, 0)) {
        case 1: return "resolved";
        case -1: return "refused";
        default: return "pending";
    }
}

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
    // The screen parts' draw site: this world's item rollover, the proof, the install.
    siteNote();
    siteInstall();
    // The same rule for the +0x38 mouse handler. The slot is re-read here as
    // well as in hk_WndLoad so that turning `group_buttons` on in the ini mid-world still arms
    // the click; both calls are no-ops once the hook is on, the candidate is pending, the key
    // is off, or the strip has disabled itself.
    if (g_window) noteMouseSlot(g_window);
    installMouseHook();
    // The fourth slot, on exactly the same terms.
    if (g_window) noteSearchSlot(g_window);
    installSearchHook();
    // The screen pad's input: the binding is resolved and cross-checked at any game-thread tick
    // (it reports even at the main menu), the two DirectInput detours install once a world has
    // captured the materials window.
    scrResolveTick();
    scrInstall();
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
    // The probe is diagnostics - GraphicsEngine::LoadTexture and a log line per plate, no write
    // into any engine object - so it runs even if the swap disabled itself. It waits for a
    // captured materials window (src\ut_startorder.h): never at the main menu, where the first
    // hooked frame can fall in the engine's start-up streaming.
    if (utPlateProbeAllowed(InterlockedCompareExchange(&g_codeOk, 0, 0) != 0, g_window != nullptr))
        probeAllPlates();
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
        }
    } else if (tries == 40) {
        InterlockedIncrement(&g_scanTries);
        disable("the ReagentWindow::Load signature never resolved (40 attempts)");
    }
}

// ---- the verifier -----------------------------------------------------------------------
// The generated geometry is an offline DERIVATION; the window in front of the player is the
// truth. One line per world puts the two side by side, so every player's log reports its own
// compatibility without anyone having to ask. This measures and complains, it lays nothing out:
// a disagreement is information, not a reason to guess at run time.
//
// The window's frozen draw rectangle at +0x4C0 is the cover's pixel size times the UI scale, so
// the comparison is made in record pixels, after dividing the scale out.
volatile LONG g_geoVerified = 0;

void plateVerifyGeometry() {
    if (InterlockedExchange(&g_geoVerified, 1)) return;
    ut::LiveGeometry g;
    liveGeometry(&g);
    float rx = 0, ry = 0, rw = 0, rh = 0;
    if (!plateWindowRect(&rx, &ry, &rw, &rh)) {
        InterlockedExchange(&g_geoVerified, 0);   // no rect yet; try again next swap
        return;
    }
    const float sc = liveUiScale() > 0.05f ? liveUiScale() : 1.0f;
    const int liveW = (int)(rw / sc + 0.5f), liveH = (int)(rh / sc + 0.5f);
    const int dw = liveW - g.plateW, dh = liveH - g.plateH;
    logD("plate: geometry check - generated %dx%d (%s, \"%s\"), live %dx%d at UI scale %.4f "
         "(window %.1fx%.1f), delta %+d%+d",
         g.plateW, g.plateH, g.derived ? "derived" : "vanilla defaults", g.plate,
         liveW, liveH, sc, rw, rh, dw, dh);
    // A few pixels is rounding in the scale; anything more means the generated assumption and
    // the window the engine built are describing different caravans.
    if (dw < -4 || dw > 4 || dh < -4 || dh > 4)
        logW("plate: the live caravan cover is %dx%d but the generation assumed %dx%d - the "
             "collection grid and its plate are sized for a different window. Delete "
             "catalogue.stamp to regenerate against the database this session loaded.",
             liveW, liveH, g.plateW, g.plateH);
    // And WHERE, in the same units: the page's location over the UI scale against the
    // WindowLocationX/Y of the frame record the active view wrote.
    int wx = 0, wy = 0;
    float loc[2] = {0.0f, 0.0f};
    if (!reagentWrittenFrame(&wx, &wy, nullptr)) {
        logD("plate: position check - the written frame record is not known, not checked");
        return;
    }
    if (!readWindowLocation(g_window, loc)) return;
    const int lx = roundUnits(loc[0] / sc), ly = roundUnits(loc[1] / sc);
    const int dx = lx - wx, dy = ly - wy;
    logD("plate: position check - the page is at %d,%d (window %.1f,%.1f at UI scale %.4f), the "
         "frame record the %s view wrote says %d,%d, delta %+d%+d",
         lx, ly, loc[0], loc[1], sc, viewName(), wx, wy, dx, dy);
    if (dx < -4 || dx > 4 || dy < -4 || dy > 4)
        logW("plate: the page is drawn at %d,%d but the frame record the %s view wrote puts it at "
             "%d,%d - the engine built the page from another copy of that record",
             lx, ly, viewName(), wx, wy);
}

void plateApply(int group, const char* label, int cellW, int cellH) {
    if (InterlockedCompareExchange(&g_disabled, 0, 0)) return;
    if (!g_window || !g_origPlate) return;
    plateVerifyGeometry();

    const void* want = g_origPlate;
    const char* what = "vanilla materials plate";
    char path[128] = "(vanilla)";
    if (group >= 0 && g_cfg.plateSwap) {
        PlateTex* p = plateFor(cellW, cellH);
        // Compare against the VANILLA PLATE TEXTURE's own size
        // (438x627), never against window+0x4C0, which is that size times the UI scale.
        const int wantW = g_origTexW > 0 ? g_origTexW : platePlateW();
        const int wantH = g_origTexH > 0 ? g_origTexH : platePlateH();
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

void plateOnViewChanged() {
    // The names carry the view and the plate size, so every entry is renamed and re-probed. The
    // textures already loaded are released at the world teardown, with the cached ones.
    plateRenameAll(false, true);
    g_layoutSeen = layoutPlatesGen();
}

void plateOnWorldTeardown() {
    // A disable taken for the shape of ONE world's window goes with that world. Only
    // disableThisWorld() sets the flag, so a failed detour install stays off for the session.
    InterlockedExchange(&g_geoVerified, 0);   // the next world says its own numbers
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
    g_sweepMask.clear();
    if (g_sweepRecs) g_sweepRecs->clear();
    maskPublish(nullptr);
    InterlockedExchange(&g_maskValid, 0);
    InterlockedExchange(&g_curHitGroup, -1);
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
    plateReleaseSuperseded(gfx, engineOwns, &freed, &kept);
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
    InterlockedExchange(&g_matKeepLogged, 0);
    InterlockedExchange(&g_drawOriginOk, 0);
    g_lastDrawOx = g_lastDrawOy = 0.0f;
    // The screen pad's input: its proof and its once-per-world lines go with the world, the
    // rectangles are retracted, and the pump's thread drops any open press on its next event.
    InterlockedExchange(&g_scrReset, 1);
    InterlockedExchange(&g_scrProbes, 0);
    InterlockedExchange(&g_scrProven, 0);
    InterlockedExchange(&g_scrLogged, 0);
    plateScreenRectsPublish(nullptr);
    // The screen parts' site: the next world has its own item rollover (the hook stays; the last
    // screen size and origin stay published, so the next world's first frame does not relayout).
    g_siteRoll = nullptr;
    g_siteRollPending = nullptr;
    g_siteTarget = nullptr;
    InterlockedExchange(&g_sitePublished, 0);
    InterlockedExchange64(&g_tailFrame, 0);
    panelScreenWorldReset();
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
    // The same liveness rule plateAssertPlate() reads the index under: a closed caravan is an
    // UNKNOWN page, never a read of a window the engine may already have freed.
    if (!g_caravan || !InterlockedCompareExchange(&g_stashOpen, 0, 0)) return 0;
    int idx = -1;
    reagentProbeEnter();
    const bool read = readPageIndex(g_caravan, &idx);
    reagentProbeLeave();
    if (!read) return 0;
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
    std::vector<void*> boxes;
    const int n = capturedBoxesAll(boxes);
    if (n <= 0) return false;
    return boxHitSeh(boxes.data(), n, (float)sx, (float)sy);
}

int boxRectsLocalSeh(void* const* boxes, int n, float* xywh, int cap) {
    __try {
        int m = 0;
        for (int i = 0; i < n && m < cap; ++i) {
            const unsigned char* b = (const unsigned char*)boxes[i];
            if (!b) continue;
            const float* r = (const float*)(b + kWidgetLocalPosOff);
            const float w = r[2], h = r[3];
            if (!(w > 0.0f && h > 0.0f) || w > 20000.0f || h > 20000.0f) continue;
            xywh[4 * m + 0] = r[0];
            xywh[4 * m + 1] = r[1];
            xywh[4 * m + 2] = w;
            xywh[4 * m + 3] = h;
            ++m;
        }
        return m;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_faults);
        return -1;
    }
}

int plateBoxRectsLocal(float* xywh, int cap) {
    if (!g_window || !xywh || cap <= 0) return 0;
    std::vector<void*> boxes;
    const int n = capturedBoxesAll(boxes);
    if (n <= 0) return 0;
    return boxRectsLocalSeh(boxes.data(), n, xywh, cap);
}

bool plateBoxHitLocal(float lx, float ly, bool* faulted) {
    if (faulted) *faulted = false;
    if (!g_window) return false;
    std::vector<void*> boxes;
    const int n = capturedBoxesAll(boxes);
    if (n <= 0) return false;
    return boxHitLocalSeh(boxes.data(), n, lx, ly, faulted);
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
    // EVERY node of the engine's map: the vector is sized from the count the walk returns
    // (ut_caps.h `utCollectAll`) - a legacy map of an older build can be past any fixed number.
    std::vector<const void*> nodes;
    const int n = utCollectAll(&nodes, [ge](const void** out, int cap) {
        return collectNodes(ge, out, cap);
    });
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
    int tRows = 0, tCopies = 0, tOnly = 0, tUncopied = 0;
    unsigned long long tMix = 0;
    if (plateTableOwns() && plateTableFold(&tRows, &tCopies, &tOnly, &tMix, &tUncopied)) {
        for (int b = 0; b < 8; ++b) {
            fpr = (fpr ^ (unsigned long long)((tMix >> (b * 8)) & 0xFFu)) * 1099511628211ULL;
        }
    }
    // Rows the fold counted and could not copy (only past the walk's memory ceiling) are table
    // rows the map does not hold as far as anything here can tell: the label counts them.
    tRows += tUncopied;
    tOnly += tUncopied;
    InterlockedExchange(&g_ownedTableRows, (LONG)tRows);
    InterlockedExchange(&g_ownedTableCopies, (LONG)tCopies);
    InterlockedExchange(&g_ownedTableOnly, (LONG)tOnly);
    const LONG total = (LONG)g_owned->size() + (LONG)tUncopied;
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
bool plateSearchMarked(int group) {
    if (group < 0 || !g_cfg.searchButtons) return false;
    if (InterlockedCompareExchange(&g_searchOff, 0, 0)) return false;
    if ((int)InterlockedCompareExchange(&g_curHitGroup, -1, -1) == group) return true;
    if (!InterlockedCompareExchange(&g_maskValid, 0, 0)) return false;
    AcquireSRWLockShared(&g_maskLock);
    const bool r = g_maskPub.test(group);
    ReleaseSRWLockShared(&g_maskLock);
    return r;
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
