// ut_live.cpp - the live re-point of the reagent page: the box widgets the engine builds for its
// Crafting Materials page are captured once per HUD build and re-pointed, per group and row, at
// the collection's own display prototypes; the vanilla page is replayed from the captured
// positions.  See ut_live.h for the design.
//
// Invariants: this file writes engine memory only through the three located UIReagentItem
// functions (Load / SetItem / SetLocalPosition), each under its own SEH guard, and one fault
// disables the feature for the session; a display prototype, once created, is cached so no
// relayout leaks another; the group, the row and the owned-only latch are mod-owned (g_cfg is
// replaced from the ini once a second) and reach the ini through one debounced path.

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
#include "ut_plate.h"
#include "ut_tooltip.h"  // the compare byte, written from showBox
#include "ut_store.h"    // the private table, read from showBox
#include "ut_rescue.h"   // journalModeKnown(): the owned counters wait for the mode

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
    int cols = 1;
    int rows = 1;
    int cellW = 32;
    int cellH = 32;
    std::vector<std::string> entries;  // box record paths, in order
    // The ITEM record each box shows, in the same order.  The reagent map is keyed by ITEM
    // record, never by box record, so this is what `recountOwned` and the filter look up.
    // tools/build_uniq_db.py writes it as a second, tab-separated field on every E line; an old
    // file without it leaves these empty and the counters report "unknown" (-1).
    std::vector<std::string> items;
};

std::vector<std::string>* g_vanBoxes = nullptr;  // the 24 real vanilla box records
std::vector<Group>* g_groups = nullptr;
std::string* g_frame = nullptr;
int g_nmax = 0;
int g_x0 = 105, g_y0 = 76, g_rowGap = 2;

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
// 1 while a layout has been built UNFILTERED although the user asked for the filter, i.e. while
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
bool showBox(size_t i, const std::string& record, const float* pos, const char* itemRecord) {
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
        return;
    }
    if (group < 0 || !g_groups || (size_t)group >= g_groups->size()) {
        InterlockedExchange(&g_ownedInGroup, -1);
        InterlockedExchange(&g_ownedAllGroups, -1);
        return;
    }
    // force=false: the snapshot is at most plate_count_ms old, so a fast scroll re-counts the
    // group out of the cached set instead of walking the engine's map on every wheel tick.
    if (plateOwnedRefresh(false) < 0) {
        InterlockedExchange(&g_ownedInGroup, -1);
        InterlockedExchange(&g_ownedAllGroups, -1);
        return;
    }
    int owned = 0;
    int all = 0;
    int keyed = 0;
    for (size_t gi = 0; gi < g_groups->size(); ++gi) {
        const Group& g = (*g_groups)[gi];
        for (size_t i = 0; i < g.entries.size(); ++i) {
            // The reagent map is keyed by the ITEM record, never by the box record.
            if (i >= g.items.size() || g.items[i].empty()) continue;
            ++keyed;
            if (!plateOwns(g.items[i].c_str())) continue;
            ++all;
            if ((int)gi == group) ++owned;
        }
    }
    if (keyed == 0) {   // an old uniq-groups.txt: say UNKNOWN rather than a wrong 0
        InterlockedExchange(&g_ownedInGroup, -1);
        InterlockedExchange(&g_ownedAllGroups, -1);
        return;
    }
    InterlockedExchange(&g_ownedInGroup, (LONG)owned);
    InterlockedExchange(&g_ownedAllGroups, (LONG)all);
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
    } else {
        const Group& g = (*g_groups)[(size_t)group];
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
            const float sc = g_uiScale > 0.0f ? g_uiScale : 1.0f;
            float xy[2];
            xy[0] = floorf((float)(g_x0 + g.cellW * col) * sc + 0.5f);
            xy[1] = floorf((float)(g_y0 + (g.cellH + g_rowGap) * rr) * sc + 0.5f);
            // `g.items[k]` is the ITEM record this box stands for - the key the journal and the
            // reagent map use. It can be empty (an `E` line with no second
            // field), and `showBox` treats that exactly like the vanilla page.
            const char* itemRec = k < g.items.size() ? g.items[k].c_str() : nullptr;
            if (!showBox((size_t)j, g.entries[k], xy, itemRec)) {
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
    const char* label = group < 0 ? "vanilla materials" : (*g_groups)[(size_t)group].label.c_str();
    // What the label (liveViewInfo) reports about this layout.
    InterlockedExchange(&g_shownFiltered, filtered ? 1 : 0);
    InterlockedExchange(&g_shownCount, (LONG)shown);
    char note[96];
    note[0] = 0;
    if (filtered) {
        _snprintf_s(note, sizeof(note), _TRUNCATE, " [owned only: %d of %zu entries shown]", shown,
                    (*g_groups)[(size_t)group].entries.size());
    }
    logT("live: relayout #%ld group=%d \"%s\" firstRow=%d boxes=%zu/%zu protoCache=%zu scale=%.3f%s",
         t, group, label, row, used, n, g_protoCache->size(), g_uiScale, note);
    reagentAfterRelayout(group, used);  // the engine's own box-badge repaint

    // The plate follows the group, and the owned counters are recomputed HERE -
    // one map walk per relayout, never one per frame.
    const int cw = group < 0 ? 0 : (*g_groups)[(size_t)group].cellW;
    const int chh = group < 0 ? 0 : (*g_groups)[(size_t)group].cellH;
    plateApply(group, label, cw, chh);
    recountOwned(group);
}

// Every record that can ever be shown must fit, so the cache is sized from the data, not from
// live_cache_max: 24 vanilla boxes + the loaded record count + 64, and `live_cache_max` may only
// RAISE that (with the shipped data max(derived, 4096) happens to be 4096).  Past the cap each
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
    const Group& g = (*g_groups)[(size_t)group];
    const LONG snap = InterlockedCompareExchange(&g_shownCount, 0, 0);
    return maxRowOf(g, snap >= 0 ? (int)snap : (int)g.entries.size());
}

int maxRow(int group) {
    if (group < 0 || !g_groups || (size_t)group >= g_groups->size()) return 0;
    const Group& g = (*g_groups)[(size_t)group];
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

// uniq-groups.txt, written by src\gen\pages_gen.cpp (tools\build_uniq_db.py is the reference):
//   F <frame record> <nmax> <vanillaCount> <x0> <y0> <rowGap>
//   V <vanilla box record>                     (vanillaCount of them, in order)
//   G <n> <label> <cols> <rows> <cellW> <cellH> <count>
//   E <box record>                             (count of them, in order)
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
        } else if (!strcmp(line, "V")) {
            g_vanBoxes->push_back(rest);
        } else if (!strcmp(line, "G")) {
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

// ---- the capture detour --------------------------------------------------------------------
void __cdecl hk_BoxLoad(void* box, const void* str) {
    if (o_BoxLoad) o_BoxLoad(box, str);
    if (!InterlockedCompareExchange(&g_capturing, 0, 0)) return;
    if (InterlockedCompareExchange(&g_inRelayout, 0, 0)) return;
    if (!g_boxes || (int)g_boxes->size() >= g_nmax) return;
    g_boxes->push_back(box);
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
        logD("live: captured %zu boxes for the material page (the frame is complete)",
             g_boxes->size());
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
    g_boxes = new std::vector<void*>();
    g_vanPos = new std::vector<float>();
    g_protoCache = new std::unordered_map<std::string, unsigned int>();
    g_vis = new std::vector<int>();   // the owned-only index list
    plateInit(selfModule);   // its own signature, its own late scan, its own log

    char path[MAX_PATH];
    if (!utModFile(selfModule, "uniq-groups.txt", path, sizeof(path)) || !loadGroups(path)) {
        g_why = "uniq-groups.txt is missing or unreadable";
        logE("live: %s - the tab cannot page; the group hotkeys still work", g_why);
        return false;
    }
    InterlockedExchange(&g_dataOk, 1);

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
    if (InterlockedCompareExchange(&g_capturing, 0, 0)) return;
    if (!g_boxes || (int)g_boxes->size() != g_nmax) return;
    if (!InterlockedCompareExchange(&g_vtableOk, 0, 0)) return;
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

bool liveHandleWheel(int ticks, bool ctrl) {
    if (!liveActive() || !g_groups || g_groups->empty()) return false;
    if (ticks == 0) return false;
    if (!inputAllowed(!ctrl, ctrl ? "Ctrl+wheel" : "wheel")) return false;
    if (ctrl) {
        const int n = (int)g_groups->size();
        int next = g_wantGroup + (ticks > 0 ? -1 : 1);
        while (next < -1) next += n + 1;
        while (next > n - 1) next -= n + 1;
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

bool liveHandleKey(int vk, bool ctrl) {
    if (!liveActive() || !g_groups || g_groups->empty()) return false;
    if (vk != VK_PRIOR && vk != VK_NEXT) return false;
    if (!inputAllowed(false, ctrl ? "Ctrl+PageUp/Down" : "PageUp/Down")) return false;
    const int dir = (vk == VK_PRIOR) ? -1 : 1;
    if (ctrl) return liveHandleWheel(dir > 0 ? -1 : 1, true);
    if (g_wantGroup < 0) return false;
    const Group& g = (*g_groups)[(size_t)g_wantGroup];
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
    if (label) *label = (*g_groups)[(size_t)g].label.c_str();
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

// How many catalogue entries a group holds. -1 = there is no such group.  Exported, cheap and
// stateless; nothing in the DLL calls it today.
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
    g_wantGroup = group;
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

int liveCellSizes(int* w, int* h, int cap) {
    if (!g_groups) return 0;
    int n = 0;
    for (size_t i = 0; i < g_groups->size(); ++i) {
        const int cw = (*g_groups)[i].cellW;
        const int ch = (*g_groups)[i].cellH;
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
    const Group& grp = (*g_groups)[(size_t)g];
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
    out->cellW = grp.cellW;
    out->cellH = grp.cellH;
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
    return g_status;
}

}  // namespace ut
