// ut_live.cpp - the live re-point of the reagent page: the box widgets the engine builds for its
// Crafting Materials page are captured once per HUD build and re-pointed, per group and row, at
// the collection's own display prototypes; the vanilla page is replayed from the captured
// positions.  See ut_live.h for the design.
//
// Invariants: this file writes engine memory only through the three located UIReagentItem
// functions (Load / SetItem / SetLocalPosition), each under its own SEH guard, and one fault
// disables the feature for the session - with TWO direct writes beside them: the compare byte
// +0x7E (ut_tooltip, from showBox) and, at zoom_pct above 100 only, a shown box's widget size
// +0x74/+0x78 (two floats, computed from the box's own texture, written after its SetItem /
// SetLocalPosition and re-asserted once per liveTick and after the mod's own Sync - never from
// the Draw detour, which stays paint-neutral; boxZoomApply / zoomReassert, under SEH, a fault =
// the same session disable); a display prototype, once created, is cached so no
// relayout leaks another; the group, the row and the owned-only latch are mod-owned (g_cfg is
// replaced from the ini once a second) and reach the ini through one debounced path.

#include "ut_generate.h"
#include "ut_live.h"

#include <math.h>   // floorf: the engine's own position rounding
#include <stdio.h>
#include <string.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "MinHook.h"
#include "gd_runtime.h"   // GetUIScaleFactor
#include "ut_config.h"
#include "ut_bindings.h"
#include "ut_log.h"
#include "ut_paths.h"
#include "ut_reagent.h"  // reagentAfterRelayout (the box-badge repaint)
#include "ut_rowmath.h"  // the paging arithmetic, shared with the offline harness
#include "ut_hudlife.h"  // the captured boxes' life and the world-start verdict, pure
#include "ut_plate.h"
#include "ut_tooltip.h"  // the compare byte, written from showBox
#include "ut_store.h"    // the private table, read from showBox
#include "ut_rescue.h"   // journalModeKnown(): the owned counters wait for the mode
#include "ut_panel.h"    // the pad's captions, for the layout's caption fit
#include "gen/ut_groups.h"   // parseGLine: the G line's id and tag

namespace ut {
namespace {

// ---- the three exe-internal functions --------------------------------------------------------
// Prefix bytes copied out of out/gd-exe-image.bin (the DECRYPTED image - the exe is Steam-DRM
// wrapped, so the bytes on disk are not these). Each was checked with tools/xref_exe.py to match
// exactly once in .text (0x1000 .. 0x2D44CB) on 1.3.0.8.
const unsigned char kSigLoad[] = {
    0x48, 0x8B, 0xC4, 0x57, 0x48, 0x83, 0xEC, 0x60, 0x48, 0xC7, 0x40, 0xB8, 0xFE, 0xFF, 0xFF,
    0xFF, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x18, 0x0F, 0x29, 0x70, 0xE8, 0x48, 0x8B,
    0xDA, 0x48, 0x8B, 0xF1, 0x48, 0x83, 0x7A, 0x10, 0x00, 0x0F, 0x84, 0x68, 0x01, 0x00, 0x00};
const unsigned char kSigSetItem[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18,
    0x57, 0x48, 0x83, 0xEC, 0x20, 0x41, 0x0F, 0xB6, 0xE8, 0x8B, 0xDA, 0x48, 0x8B, 0xF1};
const unsigned char kSigSetPos[] = {0x8B, 0x02, 0x89, 0x41, 0x6C, 0x8B, 0x42, 0x04,
                                    0x89, 0x41, 0x70, 0xC3};

// Vtable slots the located addresses must occupy on a real box object.
const size_t kSlotLoad = 0x18;
const size_t kSlotSetItem = 0xA8;
const size_t kSlotSetPos = 0xB8;
const size_t kBoxObjectIdOff = 0x30;  // UIReagentItem + 0x30 == the display prototype's id

typedef void(__cdecl* PfnBoxLoad)(void* box, const void* stdString);
typedef void(__cdecl* PfnBoxSetItem)(void* box, unsigned int objectId, bool flag);
typedef void(__cdecl* PfnBoxSetPos)(void* box, const float* xy);

PfnBoxLoad p_BoxLoad = nullptr;
PfnBoxLoad o_BoxLoad = nullptr;
PfnBoxSetItem p_BoxSetItem = nullptr;
PfnBoxSetPos p_BoxSetPos = nullptr;

// MSVC std::string, built by hand (same layout ut_reagent.cpp documents).
struct LiveString {
    union {
        char buf[16];
        char* ptr;
    } u;
    size_t size;
    size_t capacity;
};

void makeString(LiveString* s, char* storage) {
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

// ---- the data model --------------------------------------------------------------------------
struct Group {
    std::string label;
    std::string id;    // the G line's category id and tag (utg::parseGLine)
    std::string tag;
    int cols = 1;      // in force: the G line's, or utGridFit over the user's grid_ rectangle
    int rows = 1;
    int genCols = 1;   // the G line's, as generated for the derived grid
    int genRows = 1;
    int cellW = 32;    // the G line's: the largest icon of the group, as generated
    int cellH = 32;
    // The cell IN FORCE: cellW x cellH times the group's zoom (zoom_pct, clamped per group so one
    // cell fits the grid in force - ut_layout.h utZoomFit). The pitch the boxes are laid out on,
    // the plate the page asks for and the cell the editor snaps to. = cellW x cellH at zoom 100.
    int zCellW = 32;
    int zCellH = 32;
    int zoom = 100;    // the group's zoom in force, 100..the key's
    std::vector<std::string> entries;  // box record paths, in order
    // The ITEM record each box shows, in the same order.  The reagent map is keyed by ITEM
    // record, never by box record, so this is what `recountOwned` and the filter look up.
    // tools/build_uniq_db.py writes it as a second, tab-separated field on every E line; an old
    // file without it leaves these empty and the counters report "unknown" (-1).
    std::vector<std::string> items;
};

std::vector<std::string>* g_vanBoxes = nullptr;  // the 24 real vanilla box records
std::vector<Group>* g_groups = nullptr;
volatile LONG g_tableGen = 0;   // bumped after a group table loaded completely
std::string* g_frame = nullptr;
int g_nmax = 0;
// The capture's clock (QueryPerformanceCounter ticks): when liveBeginCapture opened it (the frame
// record handed to ReagentWindow::Load) and when the first box's Load returned. The "captured N
// boxes ... in X ms" line is the HUD-build cost of the frame's N boxes - each one a widget, its
// box record and one filler prototype - which the frame size moves.
LONGLONG g_captureT0 = 0, g_captureT1 = 0;
int g_x0 = 105, g_y0 = 76, g_rowGap = 2;
// The P line. The defaults are the vanilla derivation, so a group file written before the
// line existed behaves exactly as it did.
int g_usableW = 333, g_usableH = 552, g_plateW = 438, g_plateH = 627;
bool g_geoDerived = false;
// Where the grid is in force: the P line's origin, or the user's grid_ rectangle's top-left.
int g_gridX = 105, g_gridY = 76;
// The largest cell of any group (a user grid must hold it), set with the group table.
int g_maxCellW = 32, g_maxCellH = 32;
std::string* g_plateName = nullptr;

std::vector<void*>* g_boxes = nullptr;             // the live box widgets, in creation order
std::unordered_map<std::string, unsigned int>* g_protoCache = nullptr;  // record -> object id
// The 24 vanilla boxes' OWN positions, read off the widgets the
// moment UIReagentItem::Load placed them, i.e. `itemBoxX/Y * GetUIScaleFactor()` exactly as the
// engine computed it.  They exist nowhere else - `SetItem` never restores a position and the
// records are only read by Load - so the vanilla layout is replayed from these, always, for
// every box, after the SetItem that changes the texture.
std::vector<float>* g_vanPos = nullptr;           // 2 floats per vanilla box, already scaled

volatile LONG g_capturing = 0;
volatile LONG g_inRelayout = 0;
volatile LONG g_dataOk = 0;
volatile LONG g_codeOk = 0;
volatile LONG g_vtableOk = 0;
volatile LONG g_disabled = 0;
volatile LONG g_disabledWorld = 0;   // re-armed at the world teardown, unlike the session disable
volatile LONG g_relayouts = 0;
volatile LONG g_faults = 0;
volatile LONG g_created = 0;
const char* g_why = "not initialised";
char g_status[460] = "live: idle";

int g_curGroup = -2;  // -2 = nothing applied yet; -1 = vanilla
int g_curRow = 0;

// ---- the owned-only filter ---------------------------------------------------
// THE LATCH is mod-owned on purpose. `configReload` replaces the whole `g_cfg` struct once a
// second, so a button click that wrote only into `g_cfg.ownedOnly` would be undone within a
// second; the group/row pair has always solved this the same way (a mod-owned int plus a
// debounced `configPersistInt`) and this copies that mechanism rather than inventing a second.
// -1 = not seeded yet; seeded from `g_cfg.ownedOnly` the first time it is read.
volatile LONG g_ownedOnly = -1;
// The "the reagent map walk has not succeeded, so the UNFILTERED layout is used" line, said once
// per world (`liveBeginCapture` clears it).  `plateOwns()` returns false both
// for "you do not own it" and for "the walk failed", and a filter built on the second reading
// would show an EMPTY page and read as data loss.
volatile LONG g_filterFallbackLogged = 0;
// 1 while a layout has been built UNFILTERED although the player asked for the filter, i.e. while
// the page owes the user a re-layout. The walk usually succeeds on the very first relayout of a
// world, but it does not have to - and the first SUCCESSFUL walk of a world deliberately asks for
// no repaint (`plateOwnedRefresh`: the first relayout has painted it already), so without this
// the page would stay unfiltered, with the OWN button lit, until something else asked.
volatile LONG g_filterPending = 0;
// What the last relayout actually laid out, for the label (`liveViewInfo`): whether the filter
// was in force and how many of the group's entries passed it.  -1 = no filtered layout yet.
volatile LONG g_shownFiltered = 0;
volatile LONG g_shownCount = -1;
std::vector<int>* g_vis = nullptr;   // the visible-entry index list, reused, game thread only
// ITEM record -> group index, for the category-button search marks. The
// keys are normalised the way the reagent map's own keys are (lowercase, forward slashes),
// because the sweep looks a record up by exactly the string plateOwnedRefresh copied out of a
// map node. Built once, on first use, and never rebuilt (the group file is read once).
std::unordered_map<std::string, int>* g_itemGroup = nullptr;

// The same normalisation ut_plate.cpp's nodeKeyCopy applies to a map key.
void normaliseRecord(std::string* s) {
    for (size_t i = 0; i < s->size(); ++i) {
        char c = (*s)[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c == '\\') c = '/';
        (*s)[i] = c;
    }
}

// Bumped by liveBeginCapture, read by ut_plate.cpp's ReagentWindow::Load detour.
volatile LONG g_captureEpoch = 0;
// Owned counts, recomputed once per relayout (never per frame).
volatile LONG g_ownedInGroup = -1;
volatile LONG g_ownedAllGroups = -1;
// The same walk's count per GROUP (liveGroupOwned: a button of the buttons file holding several
// groups shows their sum). The first g_ownedPerGroupN are valid; 0 = unknown, as -1 above.
const int kOwnedPerGroupMax = 1024;
volatile LONG g_ownedPerGroup[kOwnedPerGroupMax];
volatile LONG g_ownedPerGroupN = 0;
int g_wantGroup = -1;
int g_wantRow = 0;
// The last plateOwnedGeneration() liveTick re-clamped the filtered row against.
// GAME THREAD ONLY, like g_curRow / g_wantRow.
long g_rowClampGen = -1;
DWORD g_dirtyAt = 0;
bool g_iniDirty = false;

// Set by liveRelayoutVisible() (any thread, inside a detour body if need be),
// consumed by the next game-thread liveTick. `g_forcedRelayouts` is how many of those requests
// really turned into a relayout, and it is printed in the heartbeat's `live:` telemetry.
volatile LONG g_relayoutWanted = 0;
volatile LONG g_forcedRelayouts = 0;

// ---- one grid per button ---------------------------------------------------------------------
// A button of the buttons file holding several groups shows ONE page: a virtual Group whose
// entries are its groups' entries concatenated in group order (ut_padpage.h utUnionPlan /
// utUnionLocate), its cell the smallest cell size of the view that holds every member's cell
// (utUnionCell - so the page keeps a plate painted for its lattice), fitted to the grid in force
// like any group. The page is keyed by the union's FIRST group (its rep): g_wantGroup, g_curGroup
// and uniq_group always hold real group indexes, so the deposit gate (any record of the
// collection while any group is shown), the search marks (per real group; a button is marked
// when any of its groups is), the pad's ring and the per-group owned counts read what they read
// before; only the layout, the row window, the label's numbers and the plate read the union. A
// one-group button has no union: its page is (*g_groups)[g], exactly as before.
// Built on the GAME THREAD (unionsSync, from liveTick) whenever the group table or the pad's
// button map moves; published by pointer and never freed (the label, the wheel and the heartbeat
// read it unlocked; it changes a few times a session at most).
struct UnionTable {
    const void* groups = nullptr;   // the g_groups it was built over
    long tableGen = -1;
    long mapGen = -1;
    UtUnionPlan plan;
    std::vector<Group> view;        // per union (plan.members): the virtual group
};
UnionTable* volatile g_unions = nullptr;

const UnionTable* unionsInForce() {
    const UnionTable* u = g_unions;
    return (u && u->groups == g_groups) ? u : nullptr;
}

// The Group whose entries, cell and grid the page of `group` shows: its button's union when it
// is a union's rep, else the group itself. `group` must be a valid index of g_groups.
const Group& pageGroup(int group) {
    const UnionTable* u = unionsInForce();
    if (u && (size_t)group < u->plan.unionOf.size()) {
        const int k = u->plan.unionOf[(size_t)group];
        if (k >= 0 && (size_t)k < u->view.size()) return u->view[(size_t)k];
    }
    return (*g_groups)[(size_t)group];
}

// The group whose page shows `group` (its union's first group; itself without one).
int unionRep(int group) {
    const UnionTable* u = unionsInForce();
    return u ? utUnionRep(u->plan, group) : group;
}

// Is real group `gi` on the page keyed by `page`?
bool onPage(int page, int gi) {
    if (gi == page) return true;
    const UnionTable* u = unionsInForce();
    return u && page >= 0 && (size_t)page < u->plan.unionOf.size() && u->plan.unionOf[(size_t)page] >= 0 &&
           utUnionRep(u->plan, gi) == page;
}

int cacheCap();          // defined below, used by showBox
int derivedCacheCap();   // the data-derived half of that cap, for the log

void disable(const char* why) {
    if (InterlockedExchange(&g_disabled, 1)) return;
    g_why = why;
    logE("live: DISABLED for this session - %s", why);
}

// For a reason that belongs to ONE world - a box whose vtable is not the one the three located
// functions came from, i.e. a HUD this world built differently. liveOnWorldTeardown() re-arms it;
// a fault or a failed detour install does not go through here and stays off for the session.
void disableThisWorld(const char* why) {
    if (InterlockedExchange(&g_disabled, 1)) return;
    InterlockedExchange(&g_disabledWorld, 1);
    g_why = why;
    logE("live: DISABLED for this world - %s", why);
}

// The box list as ut_hudlife.h models it. GAME THREAD (the capture, the relayout, the teardown
// and the view change all run there).
UtHudBoxes boxState() {
    UtHudBoxes b;
    b.captured = g_boxes ? (int)g_boxes->size() : 0;
    b.nmax = g_nmax;
    b.capturing = InterlockedCompareExchange(&g_capturing, 0, 0) != 0;
    b.curGroup = g_curGroup;
    return b;
}

// ---- signature scan --------------------------------------------------------------------------
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

// Exactly-once scan. Returns null unless the pattern matches once and only once.
const unsigned char* scanUnique(const unsigned char* lo, const unsigned char* hi,
                                const unsigned char* pat, size_t n, const char* what) {
    const unsigned char* hit = nullptr;
    long count = 0;
    for (const unsigned char* p = lo; p + n <= hi; ++p) {
        if (p[0] != pat[0]) continue;
        if (memcmp(p, pat, n) != 0) continue;
        if (!hit) hit = p;
        if (++count > 1) break;
    }
    if (count != 1) {
        logD("live: signature %s matched %ld times (need exactly 1)", what, count);
        return nullptr;
    }
    logD("live: signature %s -> %p (exe rva 0x%llX), unique", what, (const void*)hit,
         (unsigned long long)(hit - (const unsigned char*)GetModuleHandleW(nullptr)));
    return hit;
}

volatile LONG g_scanTries = 0;

bool resolveCode() {
    const unsigned char *lo = nullptr, *hi = nullptr;
    if (!textRange(&lo, &hi)) {
        logD("live: cannot find the exe's .text section");
        return false;
    }
    const LONG try_ = InterlockedIncrement(&g_scanTries);
    logD("live: scan attempt %ld over exe .text %p..%p (%llu bytes), first bytes "
         "%02X %02X %02X %02X",
         try_, (const void*)lo, (const void*)hi, (unsigned long long)(hi - lo), lo[0], lo[1],
         lo[2], lo[3]);
    const unsigned char* a = scanUnique(lo, hi, kSigLoad, sizeof(kSigLoad),
                                        "UIReagentItem::Load");
    const unsigned char* b = scanUnique(lo, hi, kSigSetItem, sizeof(kSigSetItem),
                                        "UIReagentItem::SetItem");
    const unsigned char* c = scanUnique(lo, hi, kSigSetPos, sizeof(kSigSetPos),
                                        "UIReagentItem::SetLocalPosition");
    // Each of the three reports to the bindings table. A miss here is not fatal on its own
    // (the display route turns itself off and says so), but the table has to know, because the
    // start-up summary counts what is CONFIRMED, never what was merely attempted.
    bindingsNote("exe.UIReagentItemLoad", (unsigned long long)(ULONG_PTR)a, a != nullptr,
                 "exactly one match in the exe .text");
    bindingsNote("exe.UIReagentItemSetItem", (unsigned long long)(ULONG_PTR)b, b != nullptr,
                 "exactly one match in the exe .text");
    bindingsNote("exe.UIReagentItemSetPos", (unsigned long long)(ULONG_PTR)c, c != nullptr,
                 "exactly one match in the exe .text");
    if (!a || !b || !c) return false;
    p_BoxLoad = (PfnBoxLoad)a;
    p_BoxSetItem = (PfnBoxSetItem)b;
    p_BoxSetPos = (PfnBoxSetPos)c;
    return true;
}

// The second half of the proof: a live box's own vtable must point at the three addresses the
// signatures found. Run once, on the first captured box.
bool checkVtable(void* box) {
    const void* const* vt = nullptr;
    __try {
        vt = *(const void* const**)box;
        if (!vt) return false;
        const void* fnLoad = vt[kSlotLoad / 8];
        const void* fnItem = vt[kSlotSetItem / 8];
        const void* fnPos = vt[kSlotSetPos / 8];
        if (fnLoad != (const void*)p_BoxLoad || fnItem != (const void*)p_BoxSetItem ||
            fnPos != (const void*)p_BoxSetPos) {
            logD("live: VTABLE MISMATCH box=%p vt=%p slots {0x18=%p 0xA8=%p 0xB8=%p} vs "
                 "signatures {%p %p %p}",
                 box, (const void*)vt, fnLoad, fnItem, fnPos, (void*)p_BoxLoad,
                 (void*)p_BoxSetItem, (void*)p_BoxSetPos);
            return false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logD("live: fault reading the box vtable");
        return false;
    }
    logD("live: vtable cross-check OK (box %p slot 0x18/0xA8/0xB8 == the three signatures)", box);
    return true;
}

// ---- the SEH-guarded engine calls -------------------------------------------------------------
// Each is its own function with no C++ objects so __try/__except is legal, and each failure
// disables the feature for the session (a UI widget that faulted once is not trustworthy).
bool callLoad(void* box, const void* str) {
    __try {
        o_BoxLoad ? o_BoxLoad(box, str) : p_BoxLoad(box, str);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_faults);
        return false;
    }
}

bool callSetItem(void* box, unsigned int id) {
    __try {
        p_BoxSetItem(box, id, true);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_faults);
        return false;
    }
}

bool callSetPos(void* box, float x, float y) {
    float xy[2];
    xy[0] = x;
    xy[1] = y;
    __try {
        p_BoxSetPos(box, xy);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_faults);
        return false;
    }
}

bool readBoxId(void* box, unsigned int* out) {
    __try {
        *out = *(const unsigned int*)((const unsigned char*)box + kBoxObjectIdOff);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_faults);
        return false;
    }
}

// ---- the relayout ------------------------------------------------------------------------------
// scale: UIReagentItem::Load multiplies the record's itemBoxX/Y by GraphicsEngine's UI scale
// before storing it, so the mod must apply the same factor to any position it overrides. It is
// MEASURED, never guessed: the frame's filler boxes were Loaded from records whose position is
// exactly (-4000, -4000), so after the HUD build box[nmax-1] + 0x6C == -4000 * scale.
float g_uiScale = 0.0f;
// The ENGINE's own answer, asked exactly once per HUD build on the game thread.  liveUiScale()
// itself must stay pure arithmetic - it is called from the mouse handler's decision path, where
// an engine call inside a swallowing SEH frame is forbidden - so the call lives in
// liveUiScaleRefresh(), which only the DRAW side (plateTick) ever runs.
float g_uiScaleEngine = 0.0f;             // 0 = not asked yet / refused
volatile LONG g_uiScaleEngineOff = 0;     // latched: unavailable, faulted or out of range
volatile LONG g_uiScaleCmpLogged = 0;     // the once-per-HUD agreement line

bool readBoxPos(void* box, float* x, float* y) {
    __try {
        const float* p = (const float*)((const unsigned char*)box + 0x6C);
        *x = p[0];
        *y = p[1];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_faults);
        return false;
    }
}

// ---- the zoom: the box widget's own size ------------------------------------------------------
// zoom_pct above 100 (ut_layout.h utZoomFit) draws a box's icon bigger by making the WIDGET bigger.
// ReagentWindow::Draw maps the WHOLE texture onto the box rectangle box+0x6C..+0x78 (exe 0x1EDE20:
// source {0, 0, texW, texH}, destination the widget rect), the hover / tooltip / pick-up hit test
// reads the same four floats (0x1329D0..0x132A14), and the corner badges, the count text, the
// rarity background and the dimming are anchored to that rectangle; the drop is window-level and
// never reads it. Its w,h (+0x74/+0x78) are written by the engine only in the constructor and in
// SetTexture (slot +0xB0: w,h = the texture's size times GetUIScaleFactor, and ONLY when the
// texture pointer at +0x40 changes - 0x1EE67D returns at once for the same one). So the mod
// writes them after the SetItem / SetLocalPosition of every box a zoomed page shows, always FROM
// THE TEXTURE - floor(texW * scale * zoom / 100 + 0.5), the engine's own rounding - never by
// multiplying the current w,h (an early-out SetTexture would compound it); and re-asserts them
// once per liveTick and right after its own Sync (reagentAfterRelayout): another texture under a
// box is measured again, a size the engine reset is written back. A box no zoomed page shows any
// more gets its native size back (what SetTexture computes) before it is parked or shown at 100,
// so the materials page and zoom_pct=100 are exactly the engine's own sizes. At 100 with no box
// ever zoomed none of this runs: today's page byte for byte.
const size_t kBoxTexOff = 0x40;   // GraphicsTexture*: SetTexture compares and stores it (0x1EE679)
const size_t kBoxSizeOff = 0x74;  // w, h floats: SetTexture 0x1EE6A4 / 0x1EE6B6, 0x1EE824
struct BoxZoom {
    void* box = nullptr;          // the widget this entry describes (another widget = no entry)
    const void* tex = nullptr;    // the texture the size was computed from
    float w = 0.0f, h = 0.0f;     // what was written
    int pct = 0;                  // 0 = nothing written (the engine's own size), else the zoom
};
std::vector<BoxZoom>* g_boxZoom = nullptr;   // per captured box, GAME THREAD
volatile LONG g_zoomLive = 0;        // 1 while some entry has pct != 0 (the re-assert's gate)
volatile LONG g_zoomWrites = 0;      // sizes written by showBox
volatile LONG g_zoomReasserts = 0;   // sizes written back by the re-assert (the engine reset one)
volatile LONG g_zoomRetex = 0;       // re-asserts that found another texture under a zoomed box
volatile LONG g_zoomSaidEpoch = -1;  // the capture epoch the first write-back was said in
volatile LONG g_zoomNoTexSaid = 0;   // "GraphicsTexture::GetWidth is not bound", once

bool readBoxTexSize(void* box, const void** tex, float* w, float* h) {
    __try {
        const unsigned char* b = (const unsigned char*)box;
        *tex = *(const void* const*)(b + kBoxTexOff);
        const float* r = (const float*)(b + kBoxSizeOff);
        *w = r[0];
        *h = r[1];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_faults);
        return false;
    }
}

bool writeBoxSize(void* box, float w, float h) {
    __try {
        float* r = (float*)((unsigned char*)box + kBoxSizeOff);
        r[0] = w;
        r[1] = h;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_faults);
        return false;
    }
}

bool texturePixels(const void* tex, int* w, int* h) {
    __try {
        *w = g_gd.TextureGetWidth((const GdTexture*)tex);
        *h = g_gd.TextureGetHeight((const GdTexture*)tex);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_faults);
        return false;
    }
}

// The factor SetTexture multiplies a box's texture by: GetUIScaleFactor (asked once per HUD
// build), else the filler-box measurement - uiScaleCached's rule without ui_scale_pct, a number
// the engine never sees.
float boxSizeScale() {
    const float m = g_uiScale, e = g_uiScaleEngine;
    const bool okM = m >= 0.2f && m <= 8.0f, okE = e >= 0.2f && e <= 8.0f;
    if (okE && okM) {
        const float d = e > m ? e - m : m - e;
        return d > 0.05f ? m : e;
    }
    if (okE) return e;
    if (okM) return m;
    return 1.0f;
}

// Box `i` shown at zoom `pct` (100 = the engine's own size). False = an engine access faulted.
bool boxZoomApply(size_t i, int pct) {
    if (!g_boxZoom || !g_boxes || i >= g_boxes->size()) return true;
    void* box = (*g_boxes)[i];
    if (!box) return true;
    if (g_boxZoom->size() < g_boxes->size()) {
        if (pct <= 100) return true;   // no entry: nothing was ever written to this box
        try {
            g_boxZoom->resize(g_boxes->size());
        } catch (...) {
            return true;   // no room for the state: the box keeps the engine's size
        }
    }
    BoxZoom& z = (*g_boxZoom)[i];
    if (z.box != box) z = BoxZoom();
    if (pct <= 100 && z.pct == 0) return true;   // the engine's own size, untouched
    if (!g_gd.TextureGetWidth || !g_gd.TextureGetHeight) {
        if (!InterlockedExchange(&g_zoomNoTexSaid, 1))
            logW("live: zoom_pct is not applied - GraphicsTexture::GetWidth/GetHeight are not "
                 "bound in this Engine.dll, so a box's size cannot be computed from its texture");
        z = BoxZoom();
        return true;
    }
    const void* tex = nullptr;
    float w = 0.0f, h = 0.0f;
    if (!readBoxTexSize(box, &tex, &w, &h)) return false;
    if (!tex) {   // no texture: SetTexture sizes the box again with the next one
        z = BoxZoom();
        return true;
    }
    int tw = 0, th = 0;
    if (!texturePixels(tex, &tw, &th)) return false;
    if (tw <= 0 || th <= 0 || tw > 4096 || th > 4096) {
        z = BoxZoom();
        return true;
    }
    const int zp = pct < 100 ? 100 : pct;
    const float f = boxSizeScale() * ((float)zp / 100.0f);
    const float nw = floorf((float)tw * f + 0.5f);   // SetTexture's own rounding
    const float nh = floorf((float)th * f + 0.5f);
    if (nw != w || nh != h) {
        if (!writeBoxSize(box, nw, nh)) return false;
        InterlockedIncrement(&g_zoomWrites);
    }
    if (zp <= 100) {
        z = BoxZoom();   // back at the engine's own size: nothing to re-assert
        return true;
    }
    z.box = box;
    z.tex = tex;
    z.w = nw;
    z.h = nh;
    z.pct = zp;
    InterlockedExchange(&g_zoomLive, 1);
    return true;
}

// GAME THREAD (liveTick, and applyLayout after reagentAfterRelayout). Every box a zoomed page
// shows: another texture under it (ReagentWindow::Sync re-pointed it) is measured again, a size
// the engine reset is written back. Two interlocked reads while nothing is zoomed.
void zoomReassert(const char* when) {
    if (!InterlockedCompareExchange(&g_zoomLive, 0, 0) || !g_boxZoom || !g_boxes) return;
    const size_t n = g_boxZoom->size() < g_boxes->size() ? g_boxZoom->size() : g_boxes->size();
    int live = 0, back = 0, retex = 0;
    for (size_t i = 0; i < n; ++i) {
        BoxZoom& z = (*g_boxZoom)[i];
        if (!z.pct) continue;
        void* box = (*g_boxes)[i];
        if (z.box != box || !box) {
            z = BoxZoom();
            continue;
        }
        const void* tex = nullptr;
        float w = 0.0f, h = 0.0f;
        if (!readBoxTexSize(box, &tex, &w, &h)) {
            disable("an engine read faulted while re-asserting a zoomed box size");
            return;
        }
        if (tex != z.tex) {
            ++retex;
            if (!boxZoomApply(i, z.pct)) {
                disable("an engine call faulted while re-measuring a zoomed box");
                return;
            }
        } else if (w != z.w || h != z.h) {
            if (!writeBoxSize(box, z.w, z.h)) {
                disable("an engine write faulted while re-asserting a zoomed box size");
                return;
            }
            ++back;
        }
        if (z.pct) ++live;
    }
    if (!live) InterlockedExchange(&g_zoomLive, 0);
    if (back) InterlockedExchangeAdd(&g_zoomReasserts, back);
    if (retex) InterlockedExchangeAdd(&g_zoomRetex, retex);
    const LONG epoch = InterlockedCompareExchange(&g_captureEpoch, 0, 0);
    if ((back || retex) && InterlockedExchange(&g_zoomSaidEpoch, epoch) != epoch) {
        logD("live: zoom - %s: %d box size(s) the engine reset written back, %d box(es) with "
             "another texture measured again (said once per HUD; the heartbeat's zoom= counts "
             "every one)", when, back, retex);
    }
}

// The zoom state belongs to the captured boxes: a new capture or a dead HUD drops it.
void zoomForgetAll() {
    if (g_boxZoom) g_boxZoom->clear();
    InterlockedExchange(&g_zoomLive, 0);
}

// Show `record` in box `i`. `pos` (already multiplied by the UI scale, exactly as
// UIReagentItem::Load multiplies itemBoxX/Y) is applied AFTER the SetItem that changes the
// texture - SetTexture rewrites x,y,w,h together (exe 0x1EE824) and early-outs when the texture
// pointer is unchanged, so the position must be re-asserted every time, for every box, in every
// layout.  nullptr means "no position is known", which only happens if the capture missed one.
//
// TWO records reach this function and they are NOT the same string. `record` is the BOX record -
// `records/ui/caravan/reagents/uniq/pNN/box_MM.dbr`, the DBR `UIReagentItem::Load` is given and
// the key of `g_protoCache`. `itemRecord` is the ITEM the box stands for -
// `records/items/gearweapons/shields/c021_shield.dbr` - which is what the journal, the engine's
// reagent map and `plateOwns` are all keyed by (`uniq-groups.txt` carries the pair on every `E`
// line, and `g.entries[k]` / `g.items[k]` are the two halves).  `storeDisplayProtoId` must be
// asked with the ITEM record: a box record finds nothing in the journal and silently returns 0
// for every stored item.  nullptr = the vanilla materials page, which must never be substituted.
bool showBox(size_t i, const std::string& record, const float* pos, const char* itemRecord,
             int zoomPct = 100) {
    void* box = (*g_boxes)[i];
    if (!box) return false;
    unsigned int id = 0;
    bool fresh = false;
    std::unordered_map<std::string, unsigned int>::const_iterator it = g_protoCache->find(record);
    if (it != g_protoCache->end()) {
        id = it->second;
    } else {
        char storage[MAX_PATH];
        LiveString s;
        _snprintf_s(storage, sizeof(storage), _TRUNCATE, "%s", record.c_str());
        makeString(&s, storage);
        if (!callLoad(box, &s)) return false;
        fresh = true;
        if (!readBoxId(box, &id) || id == 0) return false;
        InterlockedIncrement(&g_created);
        // The cap must never be the thing that stops a record from being cached - an uncached
        // record re-runs Load -> CreateObjectFromFile on
        // every relayout and nothing frees the surplus prototype.  It is sized from the records
        // that can actually be shown, so `live_cache_max` can only ever RAISE it.
        if ((int)g_protoCache->size() < cacheCap()) (*g_protoCache)[record] = id;
    }
    // A fresh Load has already installed the prototype and the record's own position; only a
    // cached prototype needs the box re-pointed at it.
    //
    // For a record the mod's OWN private table holds, the box is pointed at an identity
    // prototype the mod built out of uniq-items.jsonl instead - the job ReagentWindow::Sync does
    // for the engine's own records. It is the
    // same `callSetItem` either way; only the id differs. For a record the table does not hold,
    // storeDisplayProtoId returns 0 on its first line and the engine's own prototype is shown.
    const unsigned int storeId = storeDisplayProtoId(itemRecord);
    if (storeId) {
        if (!callSetItem(box, storeId)) return false;
    } else if (!fresh && !callSetItem(box, id)) {
        return false;
    }
    if (pos && !callSetPos(box, pos[0], pos[1])) return false;
    // The box's size at the page's zoom, from its texture (after the SetItem that may have
    // re-pointed it); at 100 only a box a zoomed page left bigger is touched (its native size).
    if (!boxZoomApply(i, zoomPct)) return false;
    // The ONE byte that makes the engine's own "Currently Equipped" comparison box appear over
    // the mod's tab.  The shared item-box rollover (exe 0x1EE880)
    // skips its whole compare block on `cmp byte [r12+0x7e],0` at 0x1EE9A4; the base item-box
    // constructor leaves that byte 0 (`mov word [rcx+0x7d],1` at 0x1EDA8A writes +0x7D and +0x7E
    // together, 1 and 0); and `UIReagentItem` is built by its own Load from the DBR, so neither
    // of the two containers that DO set it (0x1E9E17, 0x21F8E8) ever touches ours.  The caravan
    // copies the hovered box's +0x7E straight into its rollover mirror (0x1330EB), five
    // instructions before the `*out` write the mod's own +0x38 detour already watches - so
    // writing it here is the whole fix, with no new detour and no engine call.  It goes AFTER the
    // SetItem / SetLocalPosition calls above, which is what proves this box survived them.
    //
    // NEVER on the vanilla crafting-materials page: base `Item`'s vtable +0x550 - the equip-slot
    // type the partner test compares - is a folded stub, so an Aether Crystal could otherwise
    // acquire a nonsense compare partner.  The 24 real vanilla
    // box records are exactly what `applyLayout` shows for group -1 and they are disjoint from
    // the mod's own uniq_b* box records, so testing `record` against them decides it here without
    // the caller having to say which group this is.
    bool collectionBox = true;
    if (g_vanBoxes) {
        for (size_t v = 0; v < g_vanBoxes->size(); ++v) {
            if ((*g_vanBoxes)[v] == record) {
                collectionBox = false;
                break;
            }
        }
    }
    tooltipCompareBox(box, i, collectionBox);
    return true;
}

bool hideBox(size_t i) {
    void* box = (*g_boxes)[i];
    if (!box) return false;
    // A box a zoomed page showed gets the engine's own size back while its texture is still
    // there to measure: the next SetItem of the same texture would keep the zoomed size.
    if (!boxZoomApply(i, 100)) return false;
    if (!callSetItem(box, 0)) return false;
    const float s = g_uiScale > 0.0f ? g_uiScale : 1.0f;
    const float park = floorf(-4000.0f * s + 0.5f);   // the engine's own rounding
    return callSetPos(box, park, park);
}

// One reagent-map walk, then a lookup per entry of the shown group. Called only from
// applyLayout (game thread), so the label can read the result every frame for free.
//
// `plateOwnedRefresh` folds the mod's own private table into the same snapshot, so `plateOwns()`
// answers MAP-OWNED or TABLE-OWNED and both of the label's numbers follow it. A record the
// engine's map has never held counts here the moment the mod's own file holds it, and a record
// in both places is counted ONCE as owned (`g_owned` is a set); the SUM of the two halves is a
// different question and is published separately by `plateTableCounts()`, for the heartbeat.
void recountOwned(int group) {
    // While the mod does not know whether this character is hardcore or softcore it does not
    // know which collection is his, so it cannot say how much of one he owns - and a 0 there
    // would be a claim ("none of these are yours") it has no right to make. UNKNOWN is the
    // honest answer and the label already has a shape for it: `?`, the same answer an old
    // uniq-groups.txt gets, so the line keeps its format (ut_panel.cpp).
    if (!journalModeKnown()) {
        InterlockedExchange(&g_ownedInGroup, -1);
        InterlockedExchange(&g_ownedAllGroups, -1);
        InterlockedExchange(&g_ownedPerGroupN, 0);
        return;
    }
    if (group < 0 || !g_groups || (size_t)group >= g_groups->size()) {
        InterlockedExchange(&g_ownedInGroup, -1);
        InterlockedExchange(&g_ownedAllGroups, -1);
        InterlockedExchange(&g_ownedPerGroupN, 0);
        return;
    }
    // force=false: the snapshot is at most plate_count_ms old, so a fast scroll re-counts the
    // group out of the cached set instead of walking the engine's map on every wheel tick.
    if (plateOwnedRefresh(false) < 0) {
        InterlockedExchange(&g_ownedInGroup, -1);
        InterlockedExchange(&g_ownedAllGroups, -1);
        InterlockedExchange(&g_ownedPerGroupN, 0);
        return;
    }
    int owned = 0;
    int all = 0;
    int keyed = 0;
    for (size_t gi = 0; gi < g_groups->size(); ++gi) {
        const Group& g = (*g_groups)[gi];
        const bool shownHere = onPage(group, (int)gi);   // the page's: a union sums its groups
        int mine = 0;
        for (size_t i = 0; i < g.entries.size(); ++i) {
            // The reagent map is keyed by the ITEM record, never by the box record.
            if (i >= g.items.size() || g.items[i].empty()) continue;
            ++keyed;
            if (!plateOwns(g.items[i].c_str())) continue;
            ++all;
            ++mine;
            if (shownHere) ++owned;
        }
        if (gi < (size_t)kOwnedPerGroupMax) InterlockedExchange(&g_ownedPerGroup[gi], (LONG)mine);
    }
    if (keyed == 0) {   // an old uniq-groups.txt: say UNKNOWN rather than a wrong 0
        InterlockedExchange(&g_ownedInGroup, -1);
        InterlockedExchange(&g_ownedAllGroups, -1);
        InterlockedExchange(&g_ownedPerGroupN, 0);
        return;
    }
    InterlockedExchange(&g_ownedInGroup, (LONG)owned);
    InterlockedExchange(&g_ownedAllGroups, (LONG)all);
    const size_t per = g_groups->size() < (size_t)kOwnedPerGroupMax ? g_groups->size() : (size_t)kOwnedPerGroupMax;
    InterlockedExchange(&g_ownedPerGroupN, (LONG)per);
}

// ---- the filter, in four small pieces -------------------------------------------------------
// The change is one index indirection.  Everything here is pure mod-side arithmetic over the
// snapshot `plateOwnedRefresh` already takes for the owned counters, so the filter costs nothing
// the relayout was not paying anyway.

int ownedOnlyLatch() {
    LONG v = InterlockedCompareExchange(&g_ownedOnly, 0, 0);
    if (v < 0) {
        InterlockedCompareExchange(&g_ownedOnly, g_cfg.ownedOnly ? 1 : 0, v);
        v = InterlockedCompareExchange(&g_ownedOnly, 0, 0);
    }
    return v > 0 ? 1 : 0;
}

// Is the filter really in force RIGHT NOW?  The mandatory guard: the map walk must have
// SUCCEEDED in this world, or the layout falls back to the unfiltered one and
// says so once.  `mayRefresh` is true only on the relayout path (the game thread, where a map
// walk is already normal); the input path passes false and reads the last snapshot.
bool filterInForce(bool mayRefresh) {
    if (!ownedOnlyLatch()) return false;
    if (mayRefresh) plateOwnedRefresh(false);   // cached for plate_count_ms: usually free
    if (plateOwnedTotal() >= 0) {
        // Only the RELAYOUT path may clear the pending latch. If the read-only input path (the
        // wheel, PgUp/PgDn) cleared it too, it would SWALLOW the re-layout the fallback owes the
        // user: a page laid out unfiltered because the map walk had not answered yet, then one
        // wheel event, and `liveTick`'s pending check would never fire again.
        if (mayRefresh) InterlockedExchange(&g_filterPending, 0);
        return true;
    }
    if (!mayRefresh) return false;
    InterlockedExchange(&g_filterPending, 1);
    if (!InterlockedExchange(&g_filterFallbackLogged, 1)) {
        logD("live: owned_only=1 but the reagent map walk has not succeeded in this world - the "
             "UNFILTERED layout is used (plateOwns() cannot tell \"you do not own it\" from "
             "\"the walk failed\", and a filter built on the second reading would show an empty "
             "page and look like data loss)");
    }
    return false;
}

// Does entry `k` of `g` pass the filter?  An entry whose ITEM record is missing (an old
// uniq-groups.txt - `recountOwned`'s `keyed == 0` case) is UNFILTERABLE and stays visible; it is
// never silently dropped.
bool entryPasses(const Group& g, size_t k) {
    if (k >= g.items.size() || g.items[k].empty()) return true;
    return plateOwns(g.items[k].c_str());
}

// The indices of `g`'s entries the filter lets through, in group order.  With the filter off it
// is 0..n-1, so the layout can index through it unconditionally.
void buildVis(const Group& g, bool filtered, std::vector<int>* vis) {
    vis->clear();
    vis->reserve(g.entries.size());
    for (size_t k = 0; k < g.entries.size(); ++k) {
        if (filtered && !entryPasses(g, k)) continue;
        vis->push_back((int)k);
    }
}

// The same count without the allocation, for the row clamp on the input path.
int visCount(const Group& g, bool filtered) {
    if (!filtered) return (int)g.entries.size();
    int n = 0;
    for (size_t k = 0; k < g.entries.size(); ++k) {
        if (entryPasses(g, k)) ++n;
    }
    return n;
}

// The arithmetic lives in `ut_rowmath.h` so the offline harness (`tools\test_rowfold.cpp`) can
// link the very code the game runs and replay a take/deposit sequence under the filter.
int maxRowOf(const Group& g, int total) { return utMaxRowOf(total, g.cols, g.rows); }

void applyLayout(int group, int row) {
    const size_t n = g_boxes->size();
    size_t used = 0;
    bool filtered = false;   // was the owned-only filter applied to THIS layout
    int shown = -1;          // how many of the group's entries it left
    bool parked = false;     // the layout editor parked every box
    if (group < 0) {
        // The vanilla page, rebuilt from the REAL vanilla box records AND from the positions the
        // engine gave those boxes at HUD build.  A fresh `Load` places a box, but a cached
        // prototype (`SetItem` only) does not, and without the captured position the box would
        // stay on the last group's grid coordinate.
        for (size_t i = 0; i < g_vanBoxes->size() && i < n; ++i) {
            const float* vp = (g_vanPos && g_vanPos->size() >= (i + 1) * 2)
                                  ? &(*g_vanPos)[i * 2]
                                  : nullptr;
            // No item record: the vanilla Crafting Materials page is the engine's own and the
            // private table must never paint one of its boxes.
            if (!showBox(i, (*g_vanBoxes)[i], vp, nullptr)) {
                disable("an engine call faulted while restoring the vanilla layout");
                return;
            }
            ++used;
        }
    } else if (panelLayoutEditing()) {
        // The layout editor is open (ut_panel): every box is parked by the loop below and the
        // editor draws the plain ground and the grid's cells over the page.
        parked = true;
    } else {
        // The page of `group`: its button's union of groups (one grid) or the group itself.
        const Group& g = pageGroup(group);
        // The filter is ONE index indirection and nothing else in this function - the grid
        // maths, showBox, the parking loop, reagentAfterRelayout, plateApply and recountOwned
        // are all driven by `j` and `used`, never by `k`.  The surplus boxes are parked by the
        // loop below exactly like the 136 fillers.
        filtered = filterInForce(true);
        buildVis(g, filtered, g_vis);
        shown = (int)g_vis->size();
        // `g_curRow` / `g_wantRow` are in units of FILTERED rows the moment the filter is on, so
        // the row is re-clamped HERE, against the snapshot this very relayout used - never
        // against the one the wheel, PgUp/PgDn or the ini restore saw.
        // `g_wantRow` is written back too, or liveTick would ask for this same relayout on every
        // tick for ever.
        const int mrow = maxRowOf(g, shown);
        if (row > mrow) row = mrow;
        if (row < 0) row = 0;
        g_wantRow = row;
        const int visible = g.cols * g.rows;
        const size_t start = (size_t)row * (size_t)g.cols;
        for (int j = 0; j < visible && (size_t)j < n; ++j) {
            const size_t vi = start + (size_t)j;
            if (vi >= g_vis->size()) break;
            const size_t k = (size_t)(*g_vis)[vi];
            if (k >= g.entries.size()) break;
            const int col = j % g.cols;
            const int rr = j / g.cols;
            // Rounded exactly the way the engine rounds a position it stores itself
            // (SetTexture: floor(x + 0.5), exe 0x1EE7F0..0x1EE824), so a Sync that
            // re-SetItems the box between relayouts cannot shift it by half a pixel.
            // The pitch is the cell IN FORCE (zCellW x zCellH: the generated cell at zoom 100).
            const float sc = g_uiScale > 0.0f ? g_uiScale : 1.0f;
            float xy[2];
            xy[0] = floorf((float)(g_gridX + g.zCellW * col) * sc + 0.5f);
            xy[1] = floorf((float)(g_gridY + (g.zCellH + g_rowGap) * rr) * sc + 0.5f);
            // `g.items[k]` is the ITEM record this box stands for - the key the journal and the
            // reagent map use. It can be empty (an `E` line with no second
            // field), and `showBox` treats that exactly like the vanilla page.
            const char* itemRec = k < g.items.size() ? g.items[k].c_str() : nullptr;
            if (!showBox((size_t)j, g.entries[k], xy, itemRec, g.zoom)) {
                disable("an engine call faulted while showing a collection group");
                return;
            }
            ++used;
        }
    }
    for (size_t i = used; i < n; ++i) {
        if (!hideBox(i)) {
            disable("an engine call faulted while parking a surplus box");
            return;
        }
    }
    g_curGroup = group;
    g_curRow = row;
    const LONG t = InterlockedIncrement(&g_relayouts);
    const char* label = group < 0 ? "vanilla materials" : pageGroup(group).label.c_str();
    // What the label (liveViewInfo) reports about this layout.
    InterlockedExchange(&g_shownFiltered, filtered ? 1 : 0);
    InterlockedExchange(&g_shownCount, (LONG)shown);
    char note[160];
    note[0] = 0;
    if (parked) _snprintf_s(note, sizeof(note), _TRUNCATE, " [layout editor: every box parked]");
    if (filtered) {
        _snprintf_s(note, sizeof(note), _TRUNCATE, " [owned only: %d of %zu entries shown]", shown,
                    pageGroup(group).entries.size());
    }
    if (!parked && group >= 0 && pageGroup(group).zoom > 100) {
        const size_t at = strlen(note);
        _snprintf_s(note + at, sizeof(note) - at, _TRUNCATE, " [zoom %d%%: cells %dx%d]",
                    pageGroup(group).zoom, pageGroup(group).zCellW, pageGroup(group).zCellH);
    }
    logT("live: relayout #%ld group=%d \"%s\" firstRow=%d boxes=%zu/%zu protoCache=%zu scale=%.3f%s",
         t, group, label, row, used, n, g_protoCache->size(), g_uiScale, note);
    reagentAfterRelayout(group, used);  // the engine's own box-badge repaint
    zoomReassert("after the relayout's Sync");   // a Sync that re-pointed a texture resized it

    // The plate follows the group, and the owned counters are recomputed HERE -
    // one map walk per relayout, never one per frame. The plate is the cell IN FORCE's (at a
    // zoom the zoom's own layout set, ut_plate.cpp plateNames).
    const int cw = group < 0 ? 0 : pageGroup(group).zCellW;
    const int chh = group < 0 ? 0 : pageGroup(group).zCellH;
    plateApply(group, label, cw, chh);
    recountOwned(group);
}

// Every record that can ever be shown must fit, so the cache is sized from the data, not from
// live_cache_max: 24 vanilla boxes + the loaded record count + 64, and `live_cache_max` (4096)
// may only RAISE that - with today's catalogues the derived half is the larger (the base game's
// 7,184 records give 7,272), so the key is a floor nobody needs to touch.  Past the cap each
// relayout re-runs UIReagentItem::Load -> CreateObjectFromFile and leaks one display prototype
// per box.  The heartbeat reports both halves (`cached=N/CAP derived=D`).
// The memoisation latches only once the data is in: a cacheCap() call made before
// uniq-groups.txt was read (n = 0, derived 64) must not freeze the cap at `live_cache_max` for
// the session - harmless at the default 4096, a real prototype leak at any value below the
// derived one.
int derivedCacheCap() {
    size_t n = g_vanBoxes ? g_vanBoxes->size() : 0;
    if (g_groups) {
        for (size_t i = 0; i < g_groups->size(); ++i) n += (*g_groups)[i].entries.size();
    }
    return (int)n + 64;
}

int cacheCap() {
    static int cap = 0;
    if (cap) return cap;
    const int derived = derivedCacheCap();
    int c = derived;
    if (g_cfg.liveCacheMax > c) c = g_cfg.liveCacheMax;
    // Only remember it once the group file is loaded; before that `derived` is not the answer.
    if (g_groups && !g_groups->empty()) cap = c;
    return c;
}

// The row max for a LOG LINE, taken from the snapshot the last relayout published
// (`g_shownCount`), never from the filter.  `maxRow()` below asks
// `filterInForce()`, which reads `plateOwns()` - a `std::unordered_set<std::string>` the GAME
// thread clears and refills inside `plateOwnedRefresh` once a second with `g_ownedOk` still 1 the
// whole time - and it also WRITES `g_filterPending`.  `liveStatus()` runs on the heartbeat thread
// (see its own comment about `uiScaleCached`), so it must touch neither: a find() into a container
// mid-rehash is a crash path, and clearing the pending-relayout latch off-thread would swallow the
// re-layout the fallback owes the user.
int statusMaxRow(int group) {
    if (group < 0 || !g_groups || (size_t)group >= g_groups->size()) return 0;
    const Group& g = pageGroup(group);
    const LONG snap = InterlockedCompareExchange(&g_shownCount, 0, 0);
    return maxRowOf(g, snap >= 0 ? (int)snap : (int)g.entries.size());
}

int maxRow(int group) {
    if (group < 0 || !g_groups || (size_t)group >= g_groups->size()) return 0;
    const Group& g = pageGroup(group);
    // THE single clamp - the wheel, PgUp/PgDn and the ini restore all come here -
    // and it counts FILTERED rows whenever the owned-only filter is in force.  `false`: the input
    // path must not walk the engine's map, so it reads the last snapshot; applyLayout re-clamps
    // against a fresh one and is the authority.
    return maxRowOf(g, visCount(g, filterInForce(false)));
}

void markDirty() {
    g_iniDirty = true;
    g_dirtyAt = GetTickCount();
}

// ---- the file ------------------------------------------------------------------------------
void trimEol(char* s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0;
}

// uniq-groups.txt, written by src\gen\pages_gen.cpp (tools\build_uniq_db.py is the reference),
// tab-separated (README "The files the mod writes"):
//   F <frame record> <nmax> <vanillaCount> <x0> <y0> <rowGap>
//   P <x0> <y0> <usableW> <usableH> <plateW> <plateH> <plate bitmapName>
//   V <vanilla box record>                     (vanillaCount of them, in order)
//   G <n> <label> <cols> <rows> <cellW> <cellH> <count> <id> <tag>
//                                              (nine fields, one per group in page order; an old
//                                              file's seven-field line has no id and tag -
//                                              utg::parseGLine reads both)
//   E <box record> <item record>               (count of them, in order: the group above's items)
bool loadGroups(const char* path) {
    FILE* fh = nullptr;
    if (fopen_s(&fh, path, "rb") != 0 || !fh) return false;
    char line[512];
    int vanWant = 0;
    while (fgets(line, sizeof(line), fh)) {
        trimEol(line);
        if (!line[0]) continue;
        char* tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        char* rest = tab + 1;
        if (!strcmp(line, "F")) {
            char* f[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
            char* p = rest;
            for (int i = 0; i < 6; ++i) {
                f[i] = p;
                char* t = strchr(p, '\t');
                if (!t) break;
                *t = 0;
                p = t + 1;
            }
            *g_frame = f[0] ? f[0] : "";
            g_nmax = f[1] ? atoi(f[1]) : 0;
            vanWant = f[2] ? atoi(f[2]) : 0;
            if (f[3]) g_x0 = atoi(f[3]);
            if (f[4]) g_y0 = atoi(f[4]);
            if (f[5]) g_rowGap = atoi(f[5]);
        } else if (!strcmp(line, "P")) {
            char* f[7] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
            char* p = rest;
            for (int i = 0; i < 7; ++i) {
                f[i] = p;
                char* t = strchr(p, '\t');
                if (!t) break;
                *t = 0;
                p = t + 1;
            }
            // All seven or none: a half-read line would put the grid somewhere no record
            // describes, and the vanilla defaults are a better answer than that.
            if (f[6] && f[6][0]) {
                const int v[6] = {atoi(f[0]), atoi(f[1]), atoi(f[2]), atoi(f[3]),
                                  atoi(f[4]), atoi(f[5])};
                bool ok = v[2] >= 32 && v[3] >= 32 && v[4] >= 32 && v[5] >= 32;
                for (int i = 0; i < 6 && ok; ++i) ok = v[i] > -4000 && v[i] < 20000;
                if (ok) {
                    g_x0 = v[0];
                    g_y0 = v[1];
                    g_usableW = v[2];
                    g_usableH = v[3];
                    g_plateW = v[4];
                    g_plateH = v[5];
                    if (g_plateName) *g_plateName = f[6];
                    g_geoDerived = true;
                }
            }
        } else if (!strcmp(line, "V")) {
            g_vanBoxes->push_back(rest);
        } else if (!strcmp(line, "G")) {
            // The id and tag first, from the untouched line (7 or 9 fields).
            utg::GLine gl;
            const bool glOk = utg::parseGLine(std::string("G\t") + rest, gl);
            char* f[7] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
            char* p = rest;
            for (int i = 0; i < 7; ++i) {
                f[i] = p;
                char* t = strchr(p, '\t');
                if (!t) break;
                *t = 0;
                p = t + 1;
            }
            Group g;
            g.label = f[1] ? f[1] : "?";
            g.cols = f[2] ? atoi(f[2]) : 1;
            g.rows = f[3] ? atoi(f[3]) : 1;
            g.cellW = f[4] ? atoi(f[4]) : 32;
            g.cellH = f[5] ? atoi(f[5]) : 32;
            if (g.cols < 1) g.cols = 1;
            if (g.rows < 1) g.rows = 1;
            g.genCols = g.cols;
            g.genRows = g.rows;
            g.zCellW = g.cellW;   // zoom 100 until the first layout fits the groups
            g.zCellH = g.cellH;
            if (glOk) {
                g.id = gl.id;
                g.tag = gl.tag;
            }
            g_groups->push_back(g);
        } else if (!strcmp(line, "E")) {
            if (!g_groups->empty()) {
                char* t = strchr(rest, '\t');   // <box record>\t<item record>
                if (t) *t = 0;
                g_groups->back().entries.push_back(rest);
                g_groups->back().items.push_back(t ? t + 1 : "");
            }
        }
    }
    fclose(fh);
    if (g_frame->empty() || g_nmax <= 0 || g_groups->empty()) return false;
    g_gridX = g_x0;
    g_gridY = g_y0;
    g_maxCellW = 1;
    g_maxCellH = 1;
    for (size_t i = 0; i < g_groups->size(); ++i) {
        if ((*g_groups)[i].cellW > g_maxCellW) g_maxCellW = (*g_groups)[i].cellW;
        if ((*g_groups)[i].cellH > g_maxCellH) g_maxCellH = (*g_groups)[i].cellH;
    }
    if ((int)g_vanBoxes->size() != vanWant) {
        logW("live: uniq-groups.txt says %d vanilla boxes but carries %zu", vanWant,
             g_vanBoxes->size());
        return false;
    }
    size_t entries = 0;
    size_t withItem = 0;
    for (size_t i = 0; i < g_groups->size(); ++i) {
        const Group& g = (*g_groups)[i];
        entries += g.entries.size();
        for (size_t j = 0; j < g.items.size(); ++j) {
            if (!g.items[j].empty()) ++withItem;
        }
    }
    logD("live: uniq-groups.txt loaded from \"%s\": frame \"%s\" nmax=%d vanilla=%zu "
         "groups=%zu entries=%zu itemRecords=%zu origin=(%d,%d) rowGap=%d",
         path, g_frame->c_str(), g_nmax, g_vanBoxes->size(), g_groups->size(), entries, withItem,
         g_x0, g_y0, g_rowGap);
    logD("live: geometry %s: origin (%d,%d) usable %dx%d plate %dx%d \"%s\"",
         g_geoDerived ? "derived from the merged database" : "vanilla defaults (no P line)",
         g_x0, g_y0, g_usableW, g_usableH, g_plateW, g_plateH,
         g_plateName ? g_plateName->c_str() : "");
    if (withItem != entries) {
        logW("live: %zu of %zu entries carry no item record - the owned counters read UNKNOWN; "
             "delete catalogue.stamp to regenerate it",
             entries - withItem, entries);
    }
    // Say the derivation out loud once, so `cached=N/CAP` in the heartbeat cannot be read as
    // "the cap is just live_cache_max".
    {
        const int derived = derivedCacheCap();
        const int cap = cacheCap();
        logD("live: display-prototype cache cap = %d (derived %zu vanilla + %zu entries + 64 = "
             "%d, live_cache_max=%d may only raise it)",
             cap, g_vanBoxes->size(), entries, derived, g_cfg.liveCacheMax);
    }
    return true;
}

// ---- the layout in force --------------------------------------------------------------------
// One resolution of the grid, the pad and the label (ut_layout.h), shared by this file (the grid)
// and ut_panel.cpp (the pad, the label, the outline). It is rebuilt when its inputs move: the
// ini (configGeneration), the measured UI scale, the P line, the group table, the text measure.
long g_layApplied = -1;   // GAME THREAD: the LiveLayout.gen the group grids were last fitted to
SRWLOCK g_layLock = SRWLOCK_INIT;
struct LaySig {             // compared with memcmp: no padding (8 + 4 + 4 + 16 * 4 = 80 bytes)
    const void* groups;     // a view change swaps the table
    long cfgGen;
    float scale;
    int x0, y0, usableW, usableH, plateW, plateH, maxCellW, maxCellH, rowGap, spare[3];
    int scrW, scrH;  float scrOx, scrOy;   // the screen and the window origin (screen keys only)
};
static_assert(sizeof(LaySig) == 80, "LaySig must have no padding");
LaySig g_laySig;           // guarded by g_layLock, like the two below
// The screen and the window's origin on it (liveSetScreenView): the Draw tail's thread writes,
// laySigNow reads. Never reset on a world change - the next world's first frame keeps them.
volatile LONG g_scrW = 0, g_scrH = 0, g_scrOx = 0, g_scrOy = 0;   // the origin as float bits
bool g_layHave = false;
LiveLayout g_lay;
char g_layWarned[kUtLayParts][128];   // the refusal last said per part ("" = none)
char g_laySaid[256];                  // the last layout line said
volatile LONG g_layPlateNoted = 0;

LaySig laySigNow() {
    LaySig s;
    memset(&s, 0, sizeof(s));
    s.cfgGen = configGeneration();
    s.scale = liveUiScale();
    s.x0 = g_x0;
    s.y0 = g_y0;
    s.usableW = g_usableW;
    s.usableH = g_usableH;
    s.plateW = g_plateW;
    s.plateH = g_plateH;
    s.maxCellW = g_maxCellW;
    s.maxCellH = g_maxCellH;
    s.rowGap = g_rowGap;
    s.groups = g_groups;
    s.spare[0] = (int)InterlockedCompareExchange(&g_tableGen, 0, 0);
    s.spare[1] = (int)panelMeasureGen();   // the caption fit measures again
    if (g_cfg.padScreen[0] || g_cfg.labelScreen[0]) {   // the window's place matters only then
        s.scrW = (int)InterlockedCompareExchange(&g_scrW, 0, 0);
        s.scrH = (int)InterlockedCompareExchange(&g_scrH, 0, 0);
        const LONG bx = InterlockedCompareExchange(&g_scrOx, 0, 0);
        const LONG by = InterlockedCompareExchange(&g_scrOy, 0, 0);
        memcpy(&s.scrOx, &bx, sizeof(bx));
        memcpy(&s.scrOy, &by, sizeof(by));
    }
    return s;
}

// g_cfg is replaced by a struct copy on the worker thread, so a family store is read twice and
// used only when both copies agree; never two that agree = no user rectangle this time (the next
// configGeneration bump rebuilds it).
void copyStore(const UtCfgFamilyStore& src, UtCfgFamilyStore* out) {
    static UtCfgFamilyStore b;   // under g_layLock (exclusive)
    for (int t = 0; t < 4; ++t) {
        memcpy(out, &src, sizeof(*out));
        memcpy(&b, &src, sizeof(b));
        if (!memcmp(out, &b, sizeof(b))) return;
    }
    memset(out, 0, sizeof(*out));
}

// Under g_layLock (exclusive). Logs a refused user rectangle once per change at WARN, and the
// layout once per change (INFO while a user key is in play, DEBUG otherwise).
void layoutCompute(const LaySig& sig, LiveLayout* out) {
    memset(out, 0, sizeof(*out));
    out->scale = sig.scale;
    utCoverKeyFormat(sig.plateW, sig.plateH, out->cover, sizeof(out->cover));
    if (!(sig.scale >= 0.2f && sig.scale <= 8.0f)) return;
    static UtCfgFamilyStore st[4];
    copyStore(g_cfg.layGrid, &st[0]);
    copyStore(g_cfg.layPad, &st[1]);
    copyStore(g_cfg.layLabel, &st[2]);
    copyStore(g_cfg.layPadMat, &st[3]);
    const char* val[kUtLayParts] = {configFamilyGet(st[0], out->cover),
                                    configFamilyGet(st[1], out->cover),
                                    configFamilyGet(st[2], out->cover)};
    UtLayoutSource src = {};
    src.x0 = sig.x0;
    src.y0 = sig.y0;
    src.usableW = sig.usableW;
    src.usableH = sig.usableH;
    src.plateW = sig.plateW;
    src.plateH = sig.plateH;
    src.maxCellW = sig.maxCellW;
    src.maxCellH = sig.maxCellH;
    src.rowGap = sig.rowGap;
    src.grid = val[kUtLayGrid];
    src.pad = val[kUtLayPad];
    src.label = val[kUtLayLabel];
    src.padY = g_cfg.padY;
    src.padH = g_cfg.padH;
    src.padGap = g_cfg.padGap;
    src.labelSize = panelLabelSizeInForce();    // size.label / size.caption_max of the strings
    src.captionMax = panelCaptionMaxInForce();  // file when set, else the ini's
    src.scale = sig.scale;
    src.widest = widestCaptionLivePx;   // the font the pad draws (ut_panel.cpp textWidthPx)
    // The screen keys (the active view's settings file), copied like the family stores - g_cfg
    // is replaced by a struct copy on the worker - and the screen the Draw tail published.
    static char scrPad[64], scrLabel[64];   // under g_layLock (exclusive)
    memcpy(scrPad, g_cfg.padScreen, sizeof(scrPad));
    memcpy(scrLabel, g_cfg.labelScreen, sizeof(scrLabel));
    scrPad[sizeof(scrPad) - 1] = scrLabel[sizeof(scrLabel) - 1] = 0;
    src.padScreen = scrPad;
    src.labelScreen = scrLabel;
    src.screenKnown = sig.scrW > 0 && sig.scrH > 0;
    src.screenW = sig.scrW;
    src.screenH = sig.scrH;
    src.originX = sig.scrOx;
    src.originY = sig.scrOy;
    // The pad holds MAT, every BUTTON of the buttons file (one per group without one), OWN - or,
    // when no layout of them carries a size-6 caption, pages of them with a NEXT button
    // (ut_padpage.h).
    // The EDIT button ([advanced] edit_button, default 1) is one more button before OWN; the
    // settings file's reload bumps configGeneration, in the signature, so 0 re-lays the pad out.
    const int groups = panelPadButtonCount();
    const int edit = g_cfg.editButton ? 1 : 0;
    {
        UtLayoutInput tryIn;
        UtRect tryUser[kUtLayParts];
        out->pages = utPadPaginate(groups, edit, [&](int n) {
            src.buttons = n;
            src.widestCtx = panelButtonCaptionsFor(n == groups + 2 + edit ? 0 : 1, edit);
            return utLayoutFromSource(src, &tryIn, tryUser).pad.ok != 0;
        });
    }
    src.buttons = out->pages.buttons;
    src.widestCtx = panelButtonCaptionsFor(out->pages.pages > 1 ? 1 : 0, edit);
    out->L = utLayoutFromSource(src, &out->in, out->user);
    // zoom_pct: the cells in force are magnified (applyGridLayout fits every group with it). The
    // resolver above judged a user grid against the GENERATED largest cell (a grid too small for a
    // magnified one lowers that group's zoom instead, utZoomFit); the editor's smallest grid is
    // the largest cell as the page shows it at the zoom, so in.maxCellW/H carry that from here on
    // (ut_panel.cpp editOpen hands them to utEditMinSizes, utEditCheck judges the edit with them).
    out->zoom = utZoomPct(g_cfg.zoomPct);
    if (out->zoom > kUtZoomMin)
        utZoomLargestCell(out->L.rect[kUtLayGrid], sig.maxCellW, sig.maxCellH, sig.rowGap,
                          out->zoom, &out->in.maxCellW, &out->in.maxCellH);
    const char* pm = configFamilyGet(st[3], out->cover);
    out->padmatSet = (pm && utRectParse(pm, &out->padmat)) ? 1 : 0;
    out->ok = 1;

    const UtLayout& L = out->L;
    bool anyUser = out->padmatSet != 0 || out->zoom > kUtZoomMin;
    for (int p = 0; p < kUtLayParts; ++p) {
        char msg[128];
        msg[0] = 0;
        if (out->in.user[p]) {
            anyUser = true;
            if (L.why[p] != kUtLayoutOk) {
                _snprintf_s(msg, sizeof(msg), _TRUNCATE, "%s_%s=%s|%d|%d|%.3f",
                            utLayoutPartName(p), out->cover, val[p], L.why[p], L.against[p],
                            sig.scale);
            }
        }
        if (strcmp(msg, g_layWarned[p]) != 0) {
            strcpy_s(g_layWarned[p], sizeof(g_layWarned[p]), msg);
            if (msg[0]) {
                logW("layout: %s_%s=%s is refused - it %s%s%s%s at UI scale %.3f; the derived %s is "
                     "used",
                     utLayoutPartName(p), out->cover, val[p], utLayoutWhyText(L.why[p]),
                     L.against[p] >= 0 ? " (the " : "",
                     L.against[p] >= 0 ? utLayoutPartName(L.against[p]) : "",
                     L.against[p] >= 0 ? ")" : "", sig.scale, utLayoutPartName(p));
            }
        }
    }
    // A pinned part's per-cover line is not judged (ut_layout.h utLayoutResolve, L.ignored): said
    // once per change at DEBUG, never as a refusal.
    static char ignSaid[kUtLayParts][192];
    for (int p = kUtLayPad; p < kUtLayParts; ++p) {
        char m3[192] = {0};
        if (L.ignored[p] && val[p])
            utLayoutIgnoredText(p, out->cover, val[p], p == kUtLayPad ? scrPad : scrLabel, m3,
                                sizeof(m3));
        if (strcmp(m3, ignSaid[p]) != 0) {
            strcpy_s(ignSaid[p], sizeof(ignSaid[p]), m3);
            if (m3[0]) logD("layout: %s", m3);
        }
    }
    // A refused screen key: one WARN per change (the part stays in the window). "Not placed yet"
    // is the normal state before the first frame (or while the screen site is not installed):
    // DEBUG, never WARN.
    static char scrWarned[kUtLayParts][128];
    for (int p = kUtLayPad; p < kUtLayParts; ++p) {
        char m2[128] = {0};
        if (out->in.screenSet[p]) {
            anyUser = true;
            if (L.screenWhy[p] != kUtLayoutOk)
                _snprintf_s(m2, sizeof(m2), _TRUNCATE, "%s_screen=%s %s", utLayoutPartName(p),
                            p == kUtLayPad ? scrPad : scrLabel, utLayoutWhyText(L.screenWhy[p]));
        }
        if (strcmp(m2, scrWarned[p]) != 0) {
            strcpy_s(scrWarned[p], sizeof(scrWarned[p]), m2);
            if (m2[0] && L.screenWhy[p] == kUtLayoutScreenNoView) {
                logD("layout: %s at UI scale %.3f - the %s stays in the window until the page has "
                     "been drawn once", m2, sig.scale, utLayoutPartName(p));
            } else if (m2[0]) {
                logW("layout: %s at UI scale %.3f - the %s stays in the window", m2, sig.scale,
                     utLayoutPartName(p));
            }
        }
    }
    char said[256];
    const UtRect& g = L.rect[kUtLayGrid];
    const UtRect& pd = L.rect[kUtLayPad];
    const UtRect& lb = L.rect[kUtLayLabel];
    _snprintf_s(said, sizeof(said), _TRUNCATE,
                "layout: %s at UI scale %.3f - grid %d,%d,%d,%d (%s), pad %d,%d,%d,%d (%s) %s%d x %d "
                "cells of %d x %d caption %d, label %d,%d,%d,%d (%s) size %d%s",
                out->cover, sig.scale, g.x, g.y, g.w, g.h, L.user[kUtLayGrid] ? "user" : "derived",
                pd.x, pd.y, pd.w, pd.h, L.user[kUtLayPad] ? "user" : "derived",
                L.pad.ok ? "" : "NOT DRAWN ", L.pad.cols, L.pad.rows, L.pad.cellW, L.pad.cellH,
                L.pad.caption, lb.x, lb.y, lb.w, lb.h, L.user[kUtLayLabel] ? "user" : "derived",
                L.label.size, L.label.ok ? (L.label.bounded ? " (brought down to fit)" : "")
                                         : " NOT DRAWN");
    if (out->pages.pages > 1) {
        const size_t at = strlen(said);
        _snprintf_s(said + at, sizeof(said) - at, _TRUNCATE,
                    "; %d groups do not fit the pad - %d pages of %d with a NEXT button",
                    out->pages.groups, out->pages.pages, out->pages.perPage);
    }
    if (out->zoom > kUtZoomMin) {
        const size_t at = strlen(said);
        _snprintf_s(said + at, sizeof(said) - at, _TRUNCATE, "; zoom_pct=%d (the cells magnified)",
                    out->zoom);
    }
    if (strcmp(said, g_laySaid) != 0) {
        strcpy_s(g_laySaid, sizeof(g_laySaid), said);
        if (anyUser) {
            logI("%s", said);
        } else {
            logD("%s", said);
        }
    }
}

// GAME THREAD. Fits every group to the grid in force and, when anything moved, asks for one
// relayout of the page on screen with its first visible item kept (uniq_row follows).
void applyGridLayout(const LiveLayout& lay) {
    if (!g_groups || g_groups->empty()) return;
    const UtLayout& L = lay.L;
    const bool user = L.user[kUtLayGrid] != 0;
    const int gx = user ? L.rect[kUtLayGrid].x : g_x0;
    const int gy = user ? L.rect[kUtLayGrid].y : g_y0;
    const int cur = g_curGroup;
    const bool curOk = cur >= 0 && (size_t)cur < g_groups->size();
    const int oldCols = curOk ? pageGroup(cur).cols : 0;
    bool moved = gx != g_gridX || gy != g_gridY;
    int capped = 0;
    // zoom_pct: every group's cell in force is its cell magnified, lowered per group where one
    // magnified cell would not fit the grid in force (utZoomFit); the grid is fitted again.
    const int zoom = utZoomPct(lay.zoom);
    int clamped = 0;
    char clampedText[200];
    clampedText[0] = 0;
    auto fit = [&](Group& g) {
        UtZoomFit zf;
        const UtGrid gg = utLiveGroupGrid(L, g.genCols, g.genRows, g.cellW, g.cellH, g_rowGap,
                                          g_nmax, zoom, &zf);
        if (gg.cols != g.cols || gg.rows != g.rows || zf.cellW != g.zCellW ||
            zf.cellH != g.zCellH || zf.pct != g.zoom) {
            g.cols = gg.cols;
            g.rows = gg.rows;
            g.zCellW = zf.cellW;
            g.zCellH = zf.cellH;
            g.zoom = zf.pct;
            moved = true;
        }
        return gg;
    };
    for (size_t i = 0; i < g_groups->size(); ++i) {
        Group& g = (*g_groups)[i];
        const UtGrid gg = fit(g);
        if (gg.capped) ++capped;
        if (g.zoom < zoom) {
            const size_t at = strlen(clampedText);
            if (clamped < 6)
                _snprintf_s(clampedText + at, sizeof(clampedText) - at, _TRUNCATE, "%s%s %d%% (%dx%d)",
                            clamped ? ", " : "", g.label.c_str(), g.zoom, g.zCellW, g.zCellH);
            ++clamped;
        }
    }
    // The pages of several-group buttons are fitted like any group (not counted in "capped",
    // which counts the table's groups as before).
    if (UnionTable* u = g_unions) {
        if (u->groups == g_groups) {
            for (Group& g : u->view) fit(g);
        }
    }
    g_gridX = gx;
    g_gridY = gy;
    if (!moved) return;
    if (curOk && g_wantGroup == cur) {
        const int row = utRowKeep(g_curRow, oldCols, pageGroup(cur).cols);
        if (row != g_wantRow) {
            g_wantRow = row;
            markDirty();
        }
    }
    if (curOk) InterlockedExchange(&g_relayoutWanted, 1);
    char zoomNote[48];
    zoomNote[0] = 0;
    if (zoom > kUtZoomMin)
        _snprintf_s(zoomNote, sizeof(zoomNote), _TRUNCATE, " at zoom_pct=%d", zoom);
    logI("live: the grid in force is %s %d,%d,%d,%d on the %s cover - every group re-fitted%s (%d "
         "capped at the frame's %d boxes)%s",
         user ? "the user's" : "the derived", L.rect[kUtLayGrid].x, L.rect[kUtLayGrid].y,
         L.rect[kUtLayGrid].w, L.rect[kUtLayGrid].h, lay.cover, zoomNote, capped, g_nmax,
         curOk ? "; the page on screen is laid out again from its first visible item" : "");
    if (clamped) {
        logI("live: zoom_pct=%d - %d group(s) shown at a lower zoom, so one magnified cell fits the "
             "grid %dx%d: %s%s", zoom, clamped, L.rect[kUtLayGrid].w, L.rect[kUtLayGrid].h,
             clampedText, clamped > 6 ? ", ..." : "");
    }
    if (user && !InterlockedExchange(&g_layPlateNoted, 1)) {
        logI("layout: a user grid is in force - the page keeps the plate generated for the derived "
             "grid, so its frames do not follow the moved boxes until layout plates exist");
    }
}

// GAME THREAD (liveTick). One grid per button: rebuilds the unions when the group table or the
// pad's button map moved (the buttons file is re-read within a second, the TextWorld is rebuilt
// once per world and on a change), fitted to the grid in force; puts the wanted group on its
// union's first group (the first visible item kept while the filter is off); and asks for one
// relayout when the page on screen now holds another set of groups. Says the unions once per
// change at INFO; with no several-group button it says nothing, as before.
void unionsSync(const LiveLayout& lay) {
    if (!g_groups || g_groups->empty()) return;
    const long tableGen = InterlockedCompareExchange(&g_tableGen, 0, 0);
    const long mapGen = panelButtonMapGen();
    const UnionTable* old = unionsInForce();
    if (!old || old->tableGen != tableGen || old->mapGen != mapGen) {
        UnionTable* u = nullptr;
        try {
            u = new UnionTable();
            u->groups = g_groups;
            u->tableGen = tableGen;
            u->mapGen = mapGen;
            const int n = (int)g_groups->size();
            u->plan = utUnionPlan(n, [](int g, int* out, int cap) { return panelButtonGroupsOf(g, out, cap); });
            std::vector<UtCell> cells;   // the view's distinct cell sizes (its plates)
            for (const Group& g : *g_groups) {
                bool have = false;
                for (const UtCell& c : cells) have = have || (c.w == g.cellW && c.h == g.cellH);
                if (!have) cells.push_back(UtCell{g.cellW, g.cellH});
            }
            for (const std::vector<int>& m : u->plan.members) {
                Group v;
                const Group& first = (*g_groups)[(size_t)m[0]];
                v.id = first.id;
                v.tag = first.tag;
                std::vector<UtCell> mc;
                std::string ids;   // for the log line: "23 staff, 24 spear1h"
                for (size_t i = 0; i < m.size(); ++i) {
                    const Group& g = (*g_groups)[(size_t)m[i]];
                    ids += (i ? ", " : "") + std::to_string(m[i]) + (g.id.empty() ? "" : " " + g.id);
                    if (i) v.label += " + ";
                    v.label += g.label;
                    mc.push_back(UtCell{g.cellW, g.cellH});
                    v.entries.insert(v.entries.end(), g.entries.begin(), g.entries.end());
                    for (size_t k = 0; k < g.entries.size(); ++k)
                        v.items.push_back(k < g.items.size() ? g.items[k] : std::string());
                }
                int exact = 0;
                const UtCell c = utUnionCell(mc, cells, &exact);
                v.cellW = c.w;
                v.cellH = c.h;
                // The derived grid at this cell, exactly as pages_gen fits a group's G line.
                const UtGrid dg = utGridFit(UtRect{0, 0, g_usableW, g_usableH}, c.w, c.h, g_rowGap, g_nmax);
                v.genCols = dg.cols;
                v.genRows = dg.rows;
                UtZoomFit zf = {kUtZoomMin, c.w, c.h, 0};
                const UtGrid gg = lay.ok ? utLiveGroupGrid(lay.L, v.genCols, v.genRows, c.w, c.h,
                                                           g_rowGap, g_nmax, lay.zoom, &zf)
                                         : dg;
                v.cols = gg.cols;
                v.rows = gg.rows;
                v.zCellW = zf.cellW;   // the cell in force (zoom_pct), as applyGridLayout fits it
                v.zCellH = zf.cellH;
                v.zoom = zf.pct;
                if (u->view.size() < 8) {
                    logI("live: one grid per button - \"%s\" (groups %s, %zu items) is one page: cells "
                         "%dx%d, %d x %d%s",
                         v.label.c_str(), ids.c_str(), v.entries.size(), v.cellW, v.cellH,
                         v.cols, v.rows,
                         exact ? "" : " - no plate of this view is painted for that cell, so the page "
                                      "shows the plain cover");
                }
                u->view.push_back(v);
            }
            if (u->view.size() > 8)
                logI("live: one grid per button - and %zu more button(s) of several groups", u->view.size() - 8);
            if (u->view.empty() && old && !old->view.empty())
                logI("live: one grid per button - no button holds several groups any more");
        } catch (...) {
            delete u;
            return;   // nothing changes; the next tick tries again
        }
        // The page on screen: the same groups before and after, or one relayout.
        const int cur = g_curGroup;
        const std::vector<int> before = (cur >= 0 && old) ? utUnionMembers(old->plan, cur)
                                                           : std::vector<int>(1, cur);
        g_unions = u;   // the previous table is never freed
        const std::vector<int> after = cur >= 0 ? utUnionMembers(u->plan, cur) : std::vector<int>(1, cur);
        if (cur >= 0 && (size_t)cur < g_groups->size() && before != after)
            InterlockedExchange(&g_relayoutWanted, 1);
        // A restart restored uniq_row clamped to the restored group's OWN rows (liveInit runs
        // before the button map exists). When that group's page has just become a union and
        // nothing has moved the page since (the row is still the saved one, nothing to write), the
        // saved row comes back; applyLayout clamps it against the union. Never for a one-group page.
        const int w = g_wantGroup;
        if (w >= 0 && (size_t)w < g_groups->size() && w == g_cfg.uniqGroup && !g_iniDirty &&
            g_wantRow < g_cfg.uniqRow && utUnionMembers(u->plan, w).size() > 1 &&
            (!old || utUnionMembers(old->plan, w).size() < 2)) {
            logD("live: the saved row %d of group %d is restored on its button's page (one grid per "
                 "button; the start-up clamp used the group's own rows)", g_cfg.uniqRow, w);
            g_wantRow = g_cfg.uniqRow;
        }
    }
    // The wanted group on its page's key; the first visible item kept when the rows are in plain
    // units (with the owned-only filter on the rows are filtered ones: row 0).
    const int want = g_wantGroup;
    if (want < 0 || (size_t)want >= g_groups->size()) return;
    const int rep = unionRep(want);
    if (rep == want) return;
    int row = 0;
    if (!ownedOnlyLatch()) {
        const UnionTable* u = unionsInForce();
        if (u)
            row = utUnionRowOf(utUnionMembers(u->plan, rep), liveGroupEntries, want, g_wantRow,
                               (*g_groups)[(size_t)want].cols, pageGroup(rep).cols);
    }
    logD("live: group %d is shown on the page of its button's first group %d (one grid per button) "
         "- row %d", want, rep, row);
    g_wantGroup = rep;
    g_wantRow = row;
    markDirty();
}

// ---- the capture detour --------------------------------------------------------------------
void __cdecl hk_BoxLoad(void* box, const void* str) {
    if (o_BoxLoad) o_BoxLoad(box, str);
    if (!InterlockedCompareExchange(&g_capturing, 0, 0)) return;
    if (InterlockedCompareExchange(&g_inRelayout, 0, 0)) return;
    if (!g_boxes || (int)g_boxes->size() >= g_nmax) return;
    g_boxes->push_back(box);
    if (g_boxes->size() == 1) {
        LARGE_INTEGER qpcNow;
        QueryPerformanceCounter(&qpcNow);
        g_captureT1 = qpcNow.QuadPart;
    }
    // The frame lists the 24 REAL vanilla box records first, so these first boxes
    // are sitting on the engine's own hand-placed coordinates right now.
    if (g_vanPos && g_vanBoxes && g_boxes->size() <= g_vanBoxes->size()) {
        float px = 0.0f, py = 0.0f;
        if (readBoxPos(box, &px, &py)) {
            g_vanPos->push_back(px);
            g_vanPos->push_back(py);
        }
    }
    if ((int)g_boxes->size() == g_nmax) {
        InterlockedExchange(&g_capturing, 0);
        LARGE_INTEGER qpcNow, qpcFreq;
        QueryPerformanceCounter(&qpcNow);
        QueryPerformanceFrequency(&qpcFreq);
        const double perMs = qpcFreq.QuadPart > 0 ? (double)qpcFreq.QuadPart / 1000.0 : 0.0;
        const double allMs =
            perMs > 0.0 && g_captureT0 ? (double)(qpcNow.QuadPart - g_captureT0) / perMs : -1.0;
        const double spanMs =
            perMs > 0.0 && g_captureT1 ? (double)(qpcNow.QuadPart - g_captureT1) / perMs : -1.0;
        const size_t got = g_boxes->size();
        logD("live: captured %zu boxes for the material page in %.1f ms (the frame is complete; "
             "%.1f ms from the first box to the last, %.1f us a box)",
             got, allMs, spanMs, got > 1 && spanMs >= 0.0 ? spanMs * 1000.0 / (double)(got - 1) : 0.0);
        float px = 0.0f, py = 0.0f;
        if (readBoxPos(box, &px, &py)) {
            const float sc = px / -4000.0f;
            if (sc >= 0.2f && sc <= 8.0f) g_uiScale = sc;
            logD("live: UI scale measured from the last filler box: pos=(%.1f,%.1f) -> %.4f%s",
                 px, py, sc, (sc >= 0.2f && sc <= 8.0f) ? "" : " (REJECTED, using 1.0)");
        }
        if (!InterlockedCompareExchange(&g_vtableOk, 0, 0)) {
            if (checkVtable(box)) {
                InterlockedExchange(&g_vtableOk, 1);
            } else {
                disableThisWorld("the box vtable does not match the three located functions");
            }
        }
        // Force the first relayout so a restored group/row is applied without any input.
        g_curGroup = -2;
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------------
bool liveInit(HMODULE selfModule) {
    if (g_groups) return InterlockedCompareExchange(&g_dataOk, 0, 0) != 0;
    g_groups = new std::vector<Group>();
    g_vanBoxes = new std::vector<std::string>();
    g_frame = new std::string();
    g_plateName = new std::string("ui/caravan/caravan_transfercomponent1_bg.tex");
    g_boxes = new std::vector<void*>();
    g_vanPos = new std::vector<float>();
    g_protoCache = new std::unordered_map<std::string, unsigned int>();
    g_vis = new std::vector<int>();   // the owned-only index list
    g_boxZoom = new std::vector<BoxZoom>();   // the zoomed boxes' sizes (zoom_pct)
    plateInit(selfModule);   // its own signature, its own late scan, its own log

    char path[MAX_PATH];
    if (!utViewFile(selfModule, "uniq-groups.txt", path, sizeof(path)) || !loadGroups(path)) {
        g_why = "uniq-groups.txt is missing or unreadable";
        logE("live: %s - the tab cannot page; the group hotkeys still work", g_why);
        return false;
    }
    InterlockedExchange(&g_dataOk, 1);
    InterlockedIncrement(&g_tableGen);

    g_wantGroup = g_cfg.uniqGroup;
    if (g_wantGroup < -1 || g_wantGroup >= (int)g_groups->size()) g_wantGroup = -1;
    g_wantRow = g_cfg.uniqRow;
    if (g_wantRow < 0) g_wantRow = 0;
    // The restored row is clamped with the UNFILTERED count.  This runs on the WORKER thread at
    // init, where no reagent map walk can have succeeded yet, so asking `maxRow()` here would
    // only print the fallback line - whose wording says "in this world", before any world
    // exists - on any ini that already carries `owned_only=1`, and set `g_filterPending` from
    // the wrong thread.  `applyLayout` re-clamps the row against the snapshot its own relayout
    // used and is the authority.
    {
        const int m = (g_wantGroup >= 0 && (size_t)g_wantGroup < g_groups->size())
                          ? maxRowOf((*g_groups)[(size_t)g_wantGroup],
                                     (int)(*g_groups)[(size_t)g_wantGroup].entries.size())
                          : 0;
        if (g_wantRow > m) g_wantRow = m;
    }
    if (!resolveCode()) {
        // Expected under Steam: the exe's .text is still encrypted when the mod initialises, so
        // liveTick's late scan (game thread) resolves the signatures a second later. Only that
        // scan's 40th failure is an error.
        g_why = "the exe signatures are not resolved yet - the late scan retries";
        logI("live: the exe signatures did not resolve at init (the exe is not decrypted yet) - "
             "the game-thread late scan retries; restored group=%d row=%d", g_wantGroup, g_wantRow);
        return false;
    }
    InterlockedExchange(&g_codeOk, 1);
    g_why = "armed";
    logI("live: the tab is armed - restored group=%d row=%d", g_wantGroup, g_wantRow);
    return true;
}

bool liveReloadView(HMODULE selfModule) {
    if (!g_groups) return false;   // nothing has loaded yet: liveInit will read the right one
    char path[MAX_PATH];
    if (!utViewFile(selfModule, "uniq-groups.txt", path, sizeof(path))) {
        logW("live: the %s view has no uniq-groups.txt - the group table stays as it was",
             viewName());
        return false;
    }
    // The old table is not freed. A view change happens at most a few times in a session and the
    // table is tens of kilobytes; the draw path reads it without a lock, so the one thing that
    // must never happen is memory going away under it.
    // The database under any HUD is being replaced, and the new table's frame is another record:
    // whatever the list still holds is not a box of the page the next world builds.
    liveOnHudGone("the view changed - the database under the HUD is replaced");
    std::vector<Group>* oldGroups = g_groups;
    std::vector<std::string>* oldVan = g_vanBoxes;
    std::string* oldFrame = g_frame;
    std::string* oldPlate = g_plateName;
    g_groups = new std::vector<Group>();
    g_vanBoxes = new std::vector<std::string>();
    g_frame = new std::string();
    g_plateName = new std::string("ui/caravan/caravan_transfercomponent1_bg.tex");
    g_geoDerived = false;
    g_itemGroup = nullptr;   // the record -> group index is the old table's: rebuilt on next use
    if (!loadGroups(path)) {
        g_groups = oldGroups;   // the reader failed: keep what was working
        g_vanBoxes = oldVan;
        g_frame = oldFrame;
        g_plateName = oldPlate;
        logW("live: the %s view's uniq-groups.txt could not be read - the group table stays as "
             "it was", viewName());
        return false;
    }
    InterlockedExchange(&g_dataOk, 1);
    InterlockedIncrement(&g_tableGen);
    // Where the tab was left is per view (the view folder's uniquetab.state, which configSetView
    // read before this runs), so the new view starts on ITS last group, row and OWN filter - a
    // group index never crosses views.
    g_wantGroup = utWantedAfterTable(g_cfg.uniqGroup, (int)g_groups->size());
    g_wantRow = g_cfg.uniqRow < 0 ? 0 : g_cfg.uniqRow;
    InterlockedExchange(&g_ownedOnly, g_cfg.ownedOnly ? 1 : 0);
    g_layApplied = -1;      // the new table carries the generated grids: apply the layout again
    plateOnViewChanged();   // the plate names carry the view: probe them again
    logI("live: the group table is the %s view's - origin (%d,%d) usable %dx%d plate %dx%d",
         viewName(), g_x0, g_y0, g_usableW, g_usableH, g_plateW, g_plateH);
    return true;
}

int liveInstall(int* total) {
    if (!InterlockedCompareExchange(&g_codeOk, 0, 0) || !p_BoxLoad) {
        // Not counted in `total`: the exe is Steam-DRM wrapped and its .text is still encrypted
        // when the mod initialises, so this hook is normally installed by liveTick's late scan
        // a second or two later. Counting it here would make the detour tally read 39 of 40.
        logD("  hook %-32s DEFERRED (the exe .text is not decrypted yet - the game-thread "
             "late scan installs it)", "UIReagentItem::Load");
        plateInstall(total);   // same story, same late scan
        return 0;
    }
    if (total) *total += 1;
    MH_STATUS s = MH_CreateHook((void*)p_BoxLoad, (void*)&hk_BoxLoad, (void**)&o_BoxLoad);
    if (s == MH_OK) s = MH_EnableHook((void*)p_BoxLoad);
    if (s != MH_OK) {
        logE("  hook %-32s FAILED (%d)", "UIReagentItem::Load", (int)s);
        disable("the UIReagentItem::Load detour could not be installed");
        return 0;
    }
    logD("  hook %-32s installed at %p (trampoline %p)", "UIReagentItem::Load", (void*)p_BoxLoad,
         (void*)o_BoxLoad);
    return 1 + plateInstall(total);
}

bool liveActive() {
    if (!g_cfg.livePages) return false;
    if (InterlockedCompareExchange(&g_disabled, 0, 0)) return false;
    return InterlockedCompareExchange(&g_dataOk, 0, 0) != 0 &&
           InterlockedCompareExchange(&g_codeOk, 0, 0) != 0;
}

const char* liveFramePath() {
    if (!liveActive() || !g_frame || g_frame->empty()) return nullptr;
    return g_frame->c_str();
}

void liveBeginCapture() {
    if (!liveActive() || !g_boxes) return;
    // Say out loud what the previous HUD leaked. The engine's ReagentWindow destructor destroys
    // the prototype ids it recorded at BUILD time (exe 0x131F48), never the ones the mod
    // created afterwards, and those are
    // still displayed by live widgets until the widgets die - so they cannot be destroyed here.
    // The cache is what keeps their number bounded by "records shown", not "boxes re-pointed".
    if (g_protoCache && !g_protoCache->empty()) {
        logD("live: releasing the prototype cache of the previous HUD: %zu cached, %ld created "
             "in total this process (the engine destroys only the ids it recorded at build time)",
             g_protoCache->size(), InterlockedCompareExchange(&g_created, 0, 0));
    }
    g_boxes->clear();
    if (g_vanPos) g_vanPos->clear();
    zoomForgetAll();   // the sizes written belong to the old HUD's widgets
    g_protoCache->clear();  // the old prototypes belong to a HUD that no longer exists
    g_uiScale = 0.0f;
    // The scale is re-asked per HUD build - that is exactly when the options slider or the
    // resolution can have changed (ReagentWindow::Load runs
    // again, and it is the call that writes window+0x40/+0x44 = WindowLocation * s).
    g_uiScaleEngine = 0.0f;
    InterlockedExchange(&g_uiScaleCmpLogged, 0);
    g_curGroup = -2;
    // A repaint request made against the HUD that is going away means nothing;
    // the first relayout of the new one repaints everything anyway.
    InterlockedExchange(&g_relayoutWanted, 0);
    // The map-walk fallback line is once per WORLD, and no layout of the new HUD
    // has been filtered yet.
    InterlockedExchange(&g_filterFallbackLogged, 0);
    InterlockedExchange(&g_filterPending, 0);
    InterlockedExchange(&g_shownFiltered, 0);
    InterlockedExchange(&g_shownCount, -1);
    plateOnWorldTeardown();   // the old HUD's window pointer is dead
    {
        LARGE_INTEGER qpcNow;
        QueryPerformanceCounter(&qpcNow);
        g_captureT0 = qpcNow.QuadPart;
        g_captureT1 = 0;
    }
    InterlockedExchange(&g_capturing, 1);
    InterlockedIncrement(&g_captureEpoch);   // names the MATERIAL ReagentWindow
    logD("live: capturing the boxes of the frame page (expecting %d)", g_nmax);
}

void liveCancelCapture() {
    if (!InterlockedCompareExchange(&g_capturing, 0, 0)) return;
    InterlockedExchange(&g_capturing, 0);
    // The partial list is boxes of a page that was never built; the vanilla positions read from
    // them belong to the same nothing. Both go, so liveTick's "the frame is complete" test
    // (size == g_nmax) can never be met by a cancelled capture.
    if (g_boxes) g_boxes->clear();
    if (g_vanPos) g_vanPos->clear();
    zoomForgetAll();
    InterlockedDecrement(&g_captureEpoch);
    logD("live: the box capture is cancelled - the frame record is not in the active database");
}

void liveOnWorldTeardown() {
    // A stop taken for the shape of one world's HUD goes with that world; the next one builds its
    // own boxes and gets its own answer. Only disableThisWorld() sets the flag.
    if (InterlockedExchange(&g_disabledWorld, 0)) {
        InterlockedExchange(&g_disabled, 0);
        g_why = "armed";
        logD("live: re-armed - the disable belonged to the world that is going away");
    }
}

void liveOnHudGone(const char* why) {
    if (!g_boxes) return;
    UtHudBoxes b = boxState();
    const int had = b.captured;
    const bool wasCapturing = b.capturing;
    utBoxesHudGone(&b);
    g_boxes->clear();   // b.captured == 0: every pointer in it belonged to the dead HUD
    if (g_vanPos) g_vanPos->clear();
    zoomForgetAll();
    InterlockedExchange(&g_capturing, b.capturing ? 1 : 0);
    g_curGroup = b.curGroup;   // shows nothing; g_wantGroup/g_wantRow keep the user's choice
    // A repaint asked for against the dead HUD means nothing (liveBeginCapture says the same).
    InterlockedExchange(&g_relayoutWanted, 0);
    if (had > 0 || wasCapturing) {
        logD("live: %d captured box(es) dropped%s - %s; nothing is re-pointed until the next HUD "
             "build captures its own (wanted group=%d row=%d)",
             had, wasCapturing ? " and the open capture closed" : "", why ? why : "?",
             g_wantGroup, g_wantRow);
    }
}

void liveTabFacts(UtTabFacts* f, const char** why) {
    if (!f) return;
    f->livePages = g_cfg.livePages != 0;
    f->disabled = InterlockedCompareExchange(&g_disabled, 0, 0) != 0;
    f->disabledWorld = InterlockedCompareExchange(&g_disabledWorld, 0, 0) != 0;
    f->dataOk = InterlockedCompareExchange(&g_dataOk, 0, 0) != 0;
    f->codeOk = InterlockedCompareExchange(&g_codeOk, 0, 0) != 0;
    f->frameNamed = g_frame && !g_frame->empty();
    f->unservable = false;   // ut_reagent's flag; the caller fills it
    if (why) *why = g_why;
}

void liveTick(bool gameThread) {
    if (!gameThread) return;
    plateTick(true);   // its own late scan, independent of this one
    // The exe is Steam-DRM wrapped: its .text is encrypted on disk and only decrypted into
    // memory by the stub. If the scan at init found nothing, retry from the game thread - by
    // then the process is certainly past the stub. 40 tries, then give up for good.
    if (!g_cfg.livePages) return;
    if (!InterlockedCompareExchange(&g_disabled, 0, 0) &&
        InterlockedCompareExchange(&g_dataOk, 0, 0) &&
        !InterlockedCompareExchange(&g_codeOk, 0, 0)) {
        static DWORD lastTry = 0;
        const DWORD now = GetTickCount();
        if (now - lastTry < 500) return;
        lastTry = now;
        if (InterlockedCompareExchange(&g_scanTries, 0, 0) < 40) {
            if (resolveCode()) {
                InterlockedExchange(&g_codeOk, 1);
                g_why = "armed (late scan)";
                logI("live: the tab is armed by the late scan - the page goes live at the next "
                     "HUD build (group=%d row=%d)", g_wantGroup, g_wantRow);
                liveInstall(nullptr);
            }
        } else if (InterlockedCompareExchange(&g_scanTries, 0, 0) == 40) {
            InterlockedIncrement(&g_scanTries);
            disable("the exe signatures never resolved (40 attempts)");
        }
        return;
    }
    if (!liveActive()) return;
    // Only a complete capture of a HUD that is still alive: the list is emptied the moment that
    // HUD dies (liveOnHudGone), so between worlds - where a view change can move the wanted group
    // - nothing here can reach a box of a destroyed window (ut_hudlife.h has the whole story).
    if (!g_boxes || !utBoxesUsable(boxState(), InterlockedCompareExchange(&g_vtableOk, 0, 0) != 0))
        return;
    // The layout in force (the ini is re-read once a second): a grid change re-fits every group
    // and asks for one relayout of the page on screen, keeping its first visible item.
    {
        LiveLayout lay;
        liveLayoutGet(&lay);
        if (lay.ok && lay.gen != g_layApplied) {
            g_layApplied = lay.gen;
            applyGridLayout(lay);
        }
        // One grid per button: the unions follow the buttons file and the group table.
        unionsSync(lay);
    }
    // A layout that fell back to the UNFILTERED page because the reagent map walk
    // had not succeeded yet owes the user a re-layout the moment it does. Two interlocked reads
    // per tick while the filter is off, and nothing at all once the page is filtered.
    if (InterlockedCompareExchange(&g_filterPending, 0, 0)) {
        if (!ownedOnlyLatch()) {
            InterlockedExchange(&g_filterPending, 0);
        } else if (plateOwnedTotal() >= 0) {
            InterlockedExchange(&g_filterPending, 0);
            InterlockedExchange(&g_relayoutWanted, 1);
            logD("live: the reagent map walk has succeeded - re-laying the page out with "
                 "owned_only=1 (the layout on screen was built before the walk could answer)");
        }
    }
    // ---- the row clamp under the filter ---------------------------------------------------
    // Rows are in FILTERED units while the owned-only filter is on, so the moment the owned set
    // changes the row on screen can be past the last row that still exists - and then every
    // wheel event clamps to the row it is already on and the page looks frozen. `applyLayout`
    // re-clamps, but nothing was ASKING for a relayout after a take (the reagent map's node set
    // and prototype ids do not move; see plateOwnedRefresh's fingerprint). One interlocked read
    // per tick, and the clamp itself runs only when that generation has actually moved - at most
    // once per `plate_count_ms`, i.e. once a second by default.
    // The clamp only ever moves the row DOWN, and it is skipped while a group change is still
    // pending.  Both `liveToggleOwnedOnly` and `liveSelectGroup` set `g_wantRow = 0` while
    // `g_curRow` may still be past the new last row, and an unconditional `g_wantRow = m` would
    // RAISE the wanted row and land the page on the BOTTOM filtered row instead of row 0.
    // `g_rowClampGen` is only ever written inside this branch, so it is stale every time the
    // filter goes off -> on - the common path.  The pending-group test is the second half:
    // `maxRow(g_curGroup)` is the OLD group's last row, which must never be written into the
    // NEW group's wanted row.
    if (g_curGroup >= 0 && g_wantGroup == g_curGroup && ownedOnlyLatch()) {
        const long gen = plateOwnedGeneration();
        if (gen != g_rowClampGen) {
            g_rowClampGen = gen;
            const int m = maxRow(g_curGroup);
            if (g_wantRow > m || g_curRow > m) {
                logD("live: the collection changed under the owned-only filter - the last row is "
                     "now %d (wanted row %d, row on screen %d; rows are in FILTERED units and the "
                     "row is only ever clamped DOWN)", m, g_wantRow, g_curRow);
                if (g_wantRow > m) g_wantRow = m;
                // g_wantRow <= m < g_curRow needs no clamp at all: want != cur already forces
                // the relayout below, and applyLayout re-clamps what it is handed.
                if (g_curRow > m) InterlockedExchange(&g_relayoutWanted, 1);
            }
        }
    }
    if (g_wantGroup != g_curGroup || g_wantRow != g_curRow) {
        InterlockedExchange(&g_inRelayout, 1);
        applyLayout(g_wantGroup, g_wantRow);
        InterlockedExchange(&g_inRelayout, 0);
        // The relayout that just ran already repainted everything a forced one would have.
        InterlockedExchange(&g_relayoutWanted, 0);
    } else if (InterlockedExchange(&g_relayoutWanted, 0)) {
        // A prototype under one of the shown boxes changed (an in-place refresh keeps the
        // object id, so ReagentWindow::Sync alone repaints nothing). Re-run the layout that is
        // already on screen; applyLayout ends in
        // reagentAfterRelayout(), i.e. the engine's own Sync, exactly like a Ctrl+wheel.
        if (g_curGroup != -2) {
            const LONG n = InterlockedIncrement(&g_forcedRelayouts);
            logT("live: forced relayout #%ld of the group already on screen (group=%d row=%d) - "
                 "a shown prototype changed", n, g_curGroup, g_curRow);
            InterlockedExchange(&g_inRelayout, 1);
            applyLayout(g_curGroup, g_curRow);
            InterlockedExchange(&g_inRelayout, 0);
        }
    }
    // zoom_pct: a size the engine reset under a zoomed box is written back (two interlocked reads
    // and nothing else while no box is zoomed). Here, on the tick - never in the Draw detour.
    zoomReassert("the tick");
    if (g_iniDirty && GetTickCount() - g_dirtyAt > 1200) {
        g_iniDirty = false;
        g_cfg.uniqGroup = g_curGroup;
        g_cfg.uniqRow = g_curRow;
        configPersistInt("uniq_group", g_curGroup);
        configPersistInt("uniq_row", g_curRow);
        // The toggle's latch is mod-owned (configReload replaces g_cfg once a second), so it
        // goes into g_cfg AND into the ini through the very same debounced path the group and
        // the row use - otherwise a click is undone within a second.
        const int oo = ownedOnlyLatch();
        g_cfg.ownedOnly = oo;
        configPersistInt("owned_only", oo);
    }
}

// Deliberately the smallest possible body: ONE interlocked store. It is called
// from ut_reagent's choke point right after a prototype swap/refresh (i.e. from inside an engine
// detour, with engine locks held) and from ut_plate's map-change watch, so it must not take a
// lock, allocate, log, or touch a single byte of engine memory. Everything real happens on the
// game thread in liveTick, where a relayout is already known to be safe.
void liveRelayoutVisible() { InterlockedExchange(&g_relayoutWanted, 1); }

// A page that is not `pages[caravan+0x1728]` gets no input at all from the engine, so a wheel
// event the mod consumed on another caravan page would be the mod's alone and the stash list
// would stop scrolling. So: the Materials page must BE the visible page
// (plateMaterialsVisible(), which also requires the caravan to be open), and a PLAIN wheel
// additionally needs the cursor inside the captured window's own rect. Everything else is
// passed through untouched.
static bool inputAllowed(bool plainWheel, const char* what) {
    static DWORD lastLog = 0;
    static long refused = 0;
    const char* why = nullptr;
    if (!plateMaterialsVisible()) {
        why = "the Materials page is not the visible caravan page";
    } else if (plainWheel) {
        POINT pt;
        HWND h = GetActiveWindow();
        if (!h) h = GetForegroundWindow();
        if (!h || !GetCursorPos(&pt) || !ScreenToClient(h, &pt)) {
            why = "the cursor position could not be mapped to the game window";
        } else if (!plateCursorInWindow((int)pt.x, (int)pt.y)) {
            why = "the cursor is outside the reagent window's rect";
        }
    }
    if (!why) return true;
    ++refused;
    const DWORD now = GetTickCount();
    if (now - lastLog > 2000) {
        lastLog = now;
        logD("live: %s passed through (%s) - %ld event(s) not consumed so far", what, why,
             refused);
    }
    return false;
}

bool liveMaterialsVisible() { return plateMaterialsVisible(); }

bool liveHandleWheel(int ticks, bool buttons) {
    if (!liveActive() || !g_groups || g_groups->empty()) return false;
    if (ticks == 0) return false;
    // The cursor test belongs to the ROW wheel (rows_wheel), whatever its chord.
    if (!inputAllowed(!buttons, buttons ? "the button wheel (button_wheel)" : "the row wheel (rows_wheel)"))
        return false;
    if (buttons) {
        // One step over the materials page and the pad's BUTTONS in the order the pad draws them
        // (the buttons file's), wrapping - never the group order (a group appended as the last
        // button, as DGD's Blueprints, is the last stop, not the one after its group index's
        // neighbour). Each button is one stop at the group its click selects (its first group =
        // the rep of a several-group button's page); the groups on no button ([unassigned]) are
        // none: the wheel reaches what the pad reaches. g_wantGroup stays a real group index.
        const int n = (int)g_groups->size();
        const int next = utPadWheelButton(g_wantGroup, ticks > 0 ? -1 : 1, n,
                                          [](int g) { return panelButtonOfGroup(g); });
        g_wantGroup = next;
        g_wantRow = 0;
        markDirty();
        return true;
    }
    if (g_wantGroup < 0) return false;  // the vanilla page keeps its own wheel behaviour
    int row = g_wantRow + (ticks > 0 ? -1 : 1);
    if (row < 0) row = 0;
    const int m = maxRow(g_wantGroup);
    if (row > m) row = m;
    if (row == g_wantRow) return true;  // consumed, but already at the end
    g_wantRow = row;
    markDirty();
    return true;
}

bool liveHandleStep(int action) {
    if (!liveActive() || !g_groups || g_groups->empty()) return false;
    const bool buttons = action == kUtKeyActButtonPrev || action == kUtKeyActButtonNext;
    if (!buttons && action != kUtKeyActRowsUp && action != kUtKeyActRowsDown) return false;
    if (!inputAllowed(false, kUtKeyActions[action].name)) return false;
    const int dir = (action == kUtKeyActRowsUp || action == kUtKeyActButtonPrev) ? -1 : 1;
    if (buttons) return liveHandleWheel(dir > 0 ? -1 : 1, true);
    if (g_wantGroup < 0) return false;
    const Group& g = pageGroup(g_wantGroup);
    int row = g_wantRow + dir * g.rows;
    if (row < 0) row = 0;
    const int m = maxRow(g_wantGroup);
    if (row > m) row = m;
    g_wantRow = row;
    markDirty();
    return true;
}

// g_curGroup is written only by applyLayout, on the game thread, and every caller that
// matters (the deposit gate, the gate follow-up in reagentLateLoadTick) also runs there; the
// heartbeat thread only reads it for a log line, where a torn int cannot do harm.
int liveShownGroup(const char** label) {
    if (label) *label = "vanilla materials";
    if (!liveActive()) return -1;
    const int g = g_curGroup;
    if (g < 0 || !g_groups || (size_t)g >= g_groups->size()) return -1;
    if (label) *label = pageGroup(g).label.c_str();
    return g;
}

// ---- the UI scale ---------------------------------------------------------------------------
// THE scale, in one place, in this order:
//   1. GraphicsEngine::GetUIScaleFactor()   - the engine's own number, and provably THE number:
//      UIReagentItem::Load multiplies itemBoxX/itemBoxY by it at exe 0x1F06A0 and
//      ReagentWindow::Load writes window+0x40/+0x44 = WindowLocationX/Y * it at 0x132167.
//      Asked once per HUD build by liveUiScaleRefresh(), never from here.
//   2. the filler-box measurement (g_uiScale - and the cross-check that catches a wrong
//      GraphicsEngine*).
//   3. the captured window's rect width over the record plate width (438): the two logged cases
//      give 307/438 = 0.7009 and 406/438 = 0.9269, within half a pixel of 0.700 / 0.928.
//   4. 0 = draw nothing.  NEVER 1.0 - see the header.
// When both (1) and (2) are available and they disagree by more than 0.05 the MEASURED one
// wins, because the boxes are laid out with it. The disagreement is logged once per HUD build
// either way (>0.002).
// Pure arithmetic: two float reads and at most one plateWindowRect() - safe on the click path.
// Rungs 1 and 2 are split out here because they are the only two that are safe off the game
// thread. They read two plain floats and nothing else - no engine call, no captured-box walk.
// liveStatus() runs on the HEARTBEAT thread and uses THIS one; everything on the draw and click
// paths uses liveUiScale() below, which adds rung 3.
float uiScaleCached() {
    // ui_scale_pct: 0 = follow the game, which is both rungs below. Anything above 0 is the
    // user's own percentage and overrides them, clamped to the window the settings file names -
    // for a display where the measured scale comes out wrong. It is deliberately the FIRST rung:
    // a number the user typed is the one answer the mod must not argue with.
    const int pct = g_cfg.uiScalePct;
    if (pct > 0) {
        const int clamped = pct < 50 ? 50 : (pct > 300 ? 300 : pct);
        return (float)clamped / 100.0f;
    }
    const float m = g_uiScale;
    const float e = g_uiScaleEngine;
    const bool okM = m >= 0.2f && m <= 8.0f;
    const bool okE = e >= 0.2f && e <= 8.0f;
    if (okE && okM) {
        const float d = e > m ? e - m : m - e;
        return d > 0.05f ? m : e;
    }
    if (okE) return e;
    if (okM) return m;
    return 0.0f;
}

float liveUiScale() {
    const float c = uiScaleCached();
    if (c > 0.0f) return c;
    float wx = 0.0f, wy = 0.0f, ww = 0.0f, wh = 0.0f;
    if (plateWindowRect(&wx, &wy, &ww, &wh) && ww > 16.0f) {
        const float d = ww / 438.0f;
        if (d >= 0.2f && d <= 8.0f) return d;
    }
    return 0.0f;
}

// GAME THREAD ONLY, from plateTick's Materials-page branch - the same place the pad's fonts are
// loaded, and for the same reason: an engine call belongs on the tick, never inside the engine's
// own Draw and never on the mouse handler's decision path.
//
// The __except here SWALLOWS, and that is deliberate and bounded. This is a
// MOD-side READ of engine state through an exported accessor; it is one-shot (once per HUD
// build), self-disabling (a fault latches g_uiScaleEngineOff for the session and logs once) and
// it mutates no engine state - the same three properties as the two sanctioned draw-side
// exceptions. C2712: no C++ object may live in this frame, so everything it touches is a call.
float callUiScaleFactor() {
    __try {
        if (!g_gd.ppEngine || !g_gd.EngineGetGraphicsEngine || !g_gd.GfxGetUIScaleFactor) {
            return 0.0f;
        }
        GdEngine* e = *g_gd.ppEngine;
        if (!e) return 0.0f;
        GdGraphicsEngine* gfx = g_gd.EngineGetGraphicsEngine(e);
        if (!gfx) return 0.0f;
        return g_gd.GfxGetUIScaleFactor(gfx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1.0f;
    }
}

void liveUiScaleRefresh() {
    if (!InterlockedCompareExchange(&g_uiScaleEngineOff, 0, 0) && g_uiScaleEngine <= 0.0f) {
        if (!g_gd.GfxGetUIScaleFactor) {
            if (!InterlockedExchange(&g_uiScaleEngineOff, 1)) {
                logD("live: GraphicsEngine::GetUIScaleFactor is not exported by this Engine.dll "
                     "- the UI scale stays the filler-box measurement");
            }
        } else {
            const float v = callUiScaleFactor();
            if (v < 0.0f) {
                if (!InterlockedExchange(&g_uiScaleEngineOff, 1)) {
                    logD("live: GraphicsEngine::GetUIScaleFactor FAULTED - it is not called "
                         "again this session; the UI scale stays the filler-box measurement");
                }
            } else if (v > 0.0f && !(v >= 0.2f && v <= 8.0f)) {
                if (!InterlockedExchange(&g_uiScaleEngineOff, 1)) {
                    logD("live: GraphicsEngine::GetUIScaleFactor returned %.4f, outside the "
                         "0.2..8.0 window - REJECTED for this session", v);
                }
            } else if (v > 0.0f) {
                g_uiScaleEngine = v;
                logD("live: UI scale from the engine: GraphicsEngine::GetUIScaleFactor() = "
                     "%.4f (the same call UIReagentItem::Load multiplies itemBoxX/Y by)", v);
            }
        }
    }
    // The agreement line - the cheapest possible regression detector.
    const float m = g_uiScale;
    const float e = g_uiScaleEngine;
    if (InterlockedCompareExchange(&g_uiScaleCmpLogged, 0, 0)) return;   // said once per HUD
    if (m >= 0.2f && m <= 8.0f && e >= 0.2f && e <= 8.0f &&
        !InterlockedExchange(&g_uiScaleCmpLogged, 1)) {
        const float d = e > m ? e - m : m - e;
        if (d > 0.002f) {
            logD("live: UI scale DISAGREEMENT - GetUIScaleFactor()=%.4f vs the filler-box "
                 "measurement %.4f (delta %.4f) - %s is used", e, m, d,
                 d > 0.05f ? "the MEASURED value" : "the engine value");
        } else {
            logD("live: UI scale agreed - GetUIScaleFactor()=%.4f, filler box %.4f (delta "
                 "%.4f)", e, m, d);
        }
    }
}

int liveGroupOwned(int group) {
    if (group < 0 || group >= (int)InterlockedCompareExchange(&g_ownedPerGroupN, 0, 0)) return -1;
    return (int)InterlockedCompareExchange(&g_ownedPerGroup[group], 0, 0);
}

// How many catalogue entries a group holds. -1 = there is no such group.  Exported, cheap and
// stateless; a several-group button's page (its union) holds the sum over its groups.
int liveGroupEntries(int group) {
    if (group < 0 || !g_groups || (size_t)group >= g_groups->size()) return -1;
    return (int)(*g_groups)[(size_t)group].entries.size();
}

const char* liveGroupLabel(int group) {
    if (group < 0) return "Crafting Materials";
    if (!g_groups || (size_t)group >= g_groups->size()) return "?";
    return (*g_groups)[(size_t)group].label.c_str();
}

// The group the mod has ALREADY asked for. g_curGroup only moves when applyLayout runs on the
// next GameEngine::Update, so a caller stepping from liveShownGroup() would compute the same
// target twice if two clicks landed inside one tick (or while the capture holds liveTick off).
// Nothing in the DLL calls it today: the pad's clicks are absolute.
int liveWantedGroup() {
    if (!liveActive()) return -1;
    const int g = g_wantGroup;
    if (g < 0 || !g_groups || (size_t)g >= g_groups->size()) return -1;
    return g;
}

bool liveSelectGroup(int group) {
    if (!liveActive() || !g_groups || g_groups->empty()) return false;
    if (group < -1 || group >= (int)g_groups->size()) return false;
    g_wantGroup = unionRep(group);   // a several-group button's page is keyed by its first group
    g_wantRow = 0;
    markDirty();
    return true;
}

// Which group holds this ITEM record, or -1.
int liveGroupOfItem(const char* record) {
    if (!record || !*record || !g_groups || g_groups->empty()) return -1;
    try {
        if (!g_itemGroup) {
            std::unordered_map<std::string, int>* m = new std::unordered_map<std::string, int>();
            for (size_t gi = 0; gi < g_groups->size(); ++gi) {
                const Group& g = (*g_groups)[gi];
                for (size_t i = 0; i < g.items.size(); ++i) {
                    if (g.items[i].empty()) continue;
                    std::string k = g.items[i];
                    normaliseRecord(&k);
                    m->insert(std::make_pair(k, (int)gi));
                }
            }
            g_itemGroup = m;
            logD("search: the item-record -> group index is built - %zu records over %zu groups "
                 "(one lookup per owned record per search sweep, no engine call)",
                 g_itemGroup->size(), g_groups->size());
        }
        std::string q(record);
        normaliseRecord(&q);
        std::unordered_map<std::string, int>::const_iterator it = g_itemGroup->find(q);
        return it == g_itemGroup->end() ? -1 : it->second;
    } catch (...) {
        return -1;
    }
}

// ---- the three read-only accessors the unowned search marks (tier C) need ------------------
// All three touch only data the game thread owns and none of them calls the engine.
int liveGroupCount() { return g_groups ? (int)g_groups->size() : 0; }

const char* liveGroupId(int group) {
    if (group < 0 || !g_groups || (size_t)group >= g_groups->size()) return "";
    return (*g_groups)[(size_t)group].id.c_str();
}

const char* liveGroupTag(int group) {
    if (group < 0 || !g_groups || (size_t)group >= g_groups->size()) return "?";
    const std::string& t = (*g_groups)[(size_t)group].tag;
    return t.empty() ? "?" : t.c_str();
}

long liveGroupTableGen() { return InterlockedCompareExchange(&g_tableGen, 0, 0); }

int liveVanillaBoxCount() { return g_vanBoxes ? (int)g_vanBoxes->size() : 0; }

// Entry `index` of group `group`: its BOX record (the `uniq_b*` display record the mod's own
// prototype is built from, which is what `g_protoCache` is keyed by) and its ITEM record (the
// real `records/items/...` path, which is what the reagent map and catalogue.bin are keyed by).
// Either may come back empty on an old uniq-groups.txt; false past the end.  Both pointers are
// into `g_groups`, which is filled once at load and never mutated afterwards, so they are valid
// for the process.
bool liveGroupRecordAt(int group, int index, const char** boxRec, const char** itemRec) {
    if (boxRec) *boxRec = nullptr;
    if (itemRec) *itemRec = nullptr;
    if (group < 0 || !g_groups || (size_t)group >= g_groups->size()) return false;
    const Group& g = (*g_groups)[(size_t)group];
    if (index < 0 || (size_t)index >= g.entries.size()) return false;
    if (boxRec) *boxRec = g.entries[(size_t)index].c_str();
    if (itemRec) {
        *itemRec = (size_t)index < g.items.size() ? g.items[(size_t)index].c_str() : "";
    }
    return true;
}

// The display prototype's object id for a BOX record, or 0 when this record has not been shown
// in this world yet.  GAME THREAD ONLY: `g_protoCache` is filled by `showBox` on the game thread
// and cleared on a world teardown, and an unordered_map read racing that clear is a crash path.
unsigned int liveCachedProtoId(const char* boxRecord) {
    if (!boxRecord || !*boxRecord || !g_protoCache) return 0;
    try {
        std::unordered_map<std::string, unsigned int>::const_iterator it =
            g_protoCache->find(std::string(boxRecord));
        return it == g_protoCache->end() ? 0u : it->second;
    } catch (...) {
        return 0;
    }
}

// ---- the owned-only filter, as the pad's 26th button sees it --------------------------------
bool liveOwnedOnly() { return ownedOnlyLatch() != 0; }

bool liveToggleOwnedOnly() {
    if (!liveActive()) return false;
    const int now = ownedOnlyLatch() ? 0 : 1;
    InterlockedExchange(&g_ownedOnly, (LONG)now);
    // The search sweep arms (or deliberately SKIPS) tier C only inside its own "the needle
    // changed" branch, and a filter toggle changes no needle.  With a string already typed,
    // turning the filter ON would otherwise leave a tier-C mark for an unowned record standing
    // on a category whose matches have just been taken off the page (an over-report), and
    // turning it OFF would never start tier C for that needle at all.  Re-arm the sweep here -
    // three interlocked stores, no engine call, no allocation, on the game thread the pad click
    // already runs on.
    plateSearchRearm();
    // The row is reset IN THE SAME CALL that flips the flag, because the rows change UNITS: row 4
    // of 117 entries is not row 4 of the 13 you own, and without this the first relayout after a
    // toggle scrolls to a row that no longer exists.  This is exactly
    // what liveSelectGroup does on a group change.
    g_wantRow = 0;
    // A toggle usually changes neither the group nor the row, so liveTick's "want != cur" test
    // would not fire: ask for the forced relayout of the group already on screen instead.
    InterlockedExchange(&g_relayoutWanted, 1);
    markDirty();
    logI("live: owned_only -> %d - the first visible row is reset to 0", now);
    logD("rows are counted in FILTERED units while the filter is on");
    return true;
}

long liveCaptureEpoch() { return InterlockedCompareExchange(&g_captureEpoch, 0, 0); }

// Read-only view of the boxes captured during the last (material)
// ReagentWindow::Load, so ut_plate.cpp can prove STRUCTURALLY - not just by the capture epoch -
// that the window it is about to write a texture pointer into is the one that owns them.
int liveCapturedBoxes(void** out, int cap) {
    if (!g_boxes) return 0;
    const int n = (int)g_boxes->size();
    if (out && cap > 0) {
        const int m = n < cap ? n : cap;
        for (int i = 0; i < m; ++i) out[i] = (*g_boxes)[i];
    }
    return n;
}

void liveGeometry(LiveGeometry* out) {
    if (!out) return;
    out->x0 = g_x0;
    out->y0 = g_y0;
    out->usableW = g_usableW;
    out->usableH = g_usableH;
    out->plateW = g_plateW;
    out->plateH = g_plateH;
    out->plate = g_plateName ? g_plateName->c_str() : "";
    out->derived = g_geoDerived;
}

void liveSetScreenView(int screenW, int screenH, float originX, float originY) {
    LONG bx, by;
    memcpy(&bx, &originX, sizeof(bx));
    memcpy(&by, &originY, sizeof(by));
    InterlockedExchange(&g_scrW, screenW);
    InterlockedExchange(&g_scrH, screenH);
    InterlockedExchange(&g_scrOx, bx);
    InterlockedExchange(&g_scrOy, by);
}

bool liveLayoutGet(LiveLayout* out) {
    if (!out) return false;
    const LaySig sig = laySigNow();
    bool fresh = false;
    AcquireSRWLockShared(&g_layLock);
    if (g_layHave && !memcmp(&sig, &g_laySig, sizeof(sig))) {
        *out = g_lay;
        fresh = true;
    }
    ReleaseSRWLockShared(&g_layLock);
    if (!fresh) {
        AcquireSRWLockExclusive(&g_layLock);
        if (!(g_layHave && !memcmp(&sig, &g_laySig, sizeof(sig)))) {
            static LiveLayout next;   // under the exclusive lock
            layoutCompute(sig, &next);
            const long prev = g_layHave ? g_lay.gen : 0;
            next.gen = prev;
            const bool same = g_layHave && next.ok == g_lay.ok &&
                              !strcmp(next.cover, g_lay.cover) &&
                              !memcmp(&next.L, &g_lay.L, sizeof(next.L)) &&
                              next.padmatSet == g_lay.padmatSet &&
                              (!next.padmatSet || utRectEq(next.padmat, g_lay.padmat)) &&
                              next.zoom == g_lay.zoom;
            if (!same) next.gen = prev + 1;
            g_lay = next;
            g_laySig = sig;
            g_layHave = true;
        }
        *out = g_lay;
        ReleaseSRWLockExclusive(&g_layLock);
    }
    for (int p = 0; p < kUtLayParts; ++p)
        out->in.user[p] = out->in.user[p] ? &out->user[p] : nullptr;
    return out->ok != 0;
}

int liveCellSizes(int* w, int* h, int cap) {
    if (!g_groups) return 0;
    int n = 0;
    for (size_t i = 0; i < g_groups->size(); ++i) {
        const int cw = (*g_groups)[i].zCellW;   // the cell in force (= the G line's at zoom 100)
        const int ch = (*g_groups)[i].zCellH;
        bool seen = false;
        for (int j = 0; j < n && j < cap; ++j) {
            if (w[j] == cw && h[j] == ch) {
                seen = true;
                break;
            }
        }
        if (seen) continue;
        if (n < cap) {
            w[n] = cw;
            h[n] = ch;
        }
        ++n;
    }
    return n;
}

bool liveViewInfo(LiveView* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!liveActive() || !g_groups) return false;
    if (InterlockedCompareExchange(&g_capturing, 0, 0)) return false;
    const int g = g_curGroup;
    if (g < 0 || (size_t)g >= g_groups->size()) return false;
    const Group& grp = pageGroup(g);   // a several-group button: the union of its groups
    const int entries = (int)grp.entries.size();
    // With the filter on, the rows the label prints are FILTERED rows - and they
    // are the rows applyLayout really laid out, taken from that layout rather than recomputed
    // here, so the label can never describe a page the relayout did not build.
    const bool filt = InterlockedCompareExchange(&g_shownFiltered, 0, 0) != 0;
    int shown = (int)InterlockedCompareExchange(&g_shownCount, 0, 0);
    if (!filt || shown < 0) shown = entries;
    const int totalRows = (shown + grp.cols - 1) / grp.cols;
    int last = g_curRow + grp.rows;
    if (last > totalRows) last = totalRows;
    out->group = g;
    out->label = grp.label.c_str();
    out->firstRow = totalRows > 0 ? g_curRow + 1 : 0;
    out->lastRow = last;
    out->totalRows = totalRows;
    out->entries = entries;
    out->shown = shown;
    out->filtered = filt ? 1 : 0;
    out->owned = (int)InterlockedCompareExchange(&g_ownedInGroup, 0, 0);
    out->ownedAll = (int)InterlockedCompareExchange(&g_ownedAllGroups, 0, 0);
    int all = 0;
    for (size_t i = 0; i < g_groups->size(); ++i) all += (int)(*g_groups)[i].entries.size();
    out->collection = all;
    out->cellW = grp.zCellW;   // the cell in force: the editor snaps to it, the outline names it
    out->cellH = grp.zCellH;
    out->cols = grp.cols;
    out->rows = grp.rows;
    // The LABEL and the PAD must agree, so the label takes the same number liveUiScale() gives
    // the buttons (engine first, then the filler boxes).
    {
        const float s = liveUiScale();
        out->scale = s > 0.0f ? s : 1.0f;
    }
    return true;
}

const char* liveStatus() {
    // `ownedNodes` is what the engine's map holds, `ownedEmpty` how many of those records were
    // taken back out (node still there, stored prototype stack 0); the label counts
    // nodes-minus-empty.
    // `scale=` is uiScaleCached() - the number the pad and the label are drawn with - and
    // `scaleBox=` is the raw filler-box measurement beside it, so a disagreement between the
    // engine's number and the measured one is visible in the heartbeat. It is
    // uiScaleCached() and NOT liveUiScale() on purpose: this runs on the heartbeat thread and
    // liveUiScale()'s last rung walks the captured box vector, which belongs to the game thread.
    // The two differ only in the few frames before either scale is known, and the fallback is a
    // window measurement the heartbeat has no business taking.
    int ownedNodes = -1, ownedEmpty = -1;
    plateOwnedCounts(&ownedNodes, &ownedEmpty);
    // And the private table's half of the same snapshot - rows / copies / rows the engine's map
    // does not hold. `ownedTable=0/0/0` says in three
    // characters that the mod's own file holds nothing. Atomics only, like every other field.
    int tableRows = 0, tableCopies = 0, tableOnly = 0;
    plateTableCounts(&tableRows, &tableCopies, &tableOnly);
    const char* label = "vanilla materials";
    if (g_groups && g_curGroup >= 0 && (size_t)g_curGroup < g_groups->size())
        label = (*g_groups)[(size_t)g_curGroup].label.c_str();
    _snprintf_s(g_status, sizeof(g_status), _TRUNCATE,
                "live: %s(%s) boxes=%zu/%d group=%d[%s] row=%d/%d want=%d/%d relayouts=%ld "
                "protos=%ld cached=%zu/%d derived=%d faults=%ld scale=%.3f forced=%ld "
                "ownedNodes=%d ownedEmpty=%d scaleBox=%.3f ownedTable=%d/%d/%d",
                liveActive() ? "armed" : "off", g_why, g_boxes ? g_boxes->size() : 0, g_nmax,
                g_curGroup, label, g_curRow, statusMaxRow(g_curGroup), g_wantGroup, g_wantRow,
                InterlockedCompareExchange(&g_relayouts, 0, 0),
                InterlockedCompareExchange(&g_created, 0, 0),
                g_protoCache ? g_protoCache->size() : 0, cacheCap(), derivedCacheCap(),
                InterlockedCompareExchange(&g_faults, 0, 0), uiScaleCached(),
                InterlockedCompareExchange(&g_forcedRelayouts, 0, 0), ownedNodes, ownedEmpty,
                g_uiScale, tableRows, tableCopies, tableOnly);
    const LONG zw = InterlockedCompareExchange(&g_zoomWrites, 0, 0);
    if (zw || utZoomPct(g_cfg.zoomPct) > kUtZoomMin) {
        const size_t at = strlen(g_status);
        _snprintf_s(g_status + at, sizeof(g_status) - at, _TRUNCATE, " zoom=%d/%ld/%ld/%ld",
                    utZoomPct(g_cfg.zoomPct), zw, InterlockedCompareExchange(&g_zoomReasserts, 0, 0),
                    InterlockedCompareExchange(&g_zoomRetex, 0, 0));
    }
    return g_status;
}

}  // namespace ut
