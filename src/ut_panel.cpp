// ut_panel.cpp - the collection model and the two things the mod paints over the reagent page:
// the 26-button category pad and the one-line group label.
//
// Owns the loaded catalogue (g_cat), the per-character ownership model (g_own) fed by the sack
// detours in hooks.cpp, the record-name reader for a live Item*, and the session state of the
// pad and the label.
//
// Read-only by construction: the only engine calls made here are accessors and canvas draw
// calls. Nothing creates, moves, deletes or saves an item; the sack detours call the original
// first and hand over the Item* afterwards purely to read its record name. The draw paths write
// no engine memory, allocate nothing and log nothing per frame; a fault in either latches a
// mod-owned session switch - never a g_cfg field, which the ini re-read replaces once a second.

#include "ut_panel.h"

#include <windows.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "model/catalogue.h"
#include "model/collection.h"
#include "model/layout.h"
#include "ut_config.h"
#include "ut_live.h"
#include "ut_plate.h"   // page visibility + the captured window rect
#include "ut_log.h"
#include "ut_paths.h"
#include "ut_reagent.h"
#include "ut_rescue.h"   // journalModeKnown(): the label says when the mode is not known
#include "ut_fontmetrics.h"  // savapromedium's own glyph metrics (generated)

namespace ut {
namespace {

// =============================================================================================
// state
// =============================================================================================

std::mutex g_modelMx;      // guards g_own, g_unknownSamples, g_liveBitmaps
gdut::Catalogue g_cat;
gdut::Collection g_own;
std::vector<std::string> g_unknownSamples;   // first 10 records not in the catalogue
std::unordered_map<int, const GdTexture*> g_liveBitmaps;  // Item::GetBitmap() per catalogue item

bool g_initialised = false;
bool g_loadFailed = false;

// Observed GAME::ItemClassification enum values, learned from live items (Epic 3, Legendary 4).
int g_classEnum[2] = {-1, -1};

// counters (interlocked, read from any thread)
volatile LONG64 g_addEvents = 0;
volatile LONG64 g_removeEvents = 0;
volatile LONG64 g_nameRouteObject = 0;
volatile LONG64 g_nameRouteReplica = 0;
volatile LONG64 g_nameRouteNone = 0;
volatile LONG g_objectNameLogged = 0;
volatile LONG g_transferOpen = 0;


// The GameEngine the render thread last saw.
GdGameEngine* volatile g_lastGameEngine = nullptr;

volatile LONG64 g_firstAddTick = 0;

char g_status[320] = {0};

// While >0 the mod itself is inside GraphicsEngine::LoadTexture, so the LoadTexture detour must
// not log the path as one of the GAME's.
thread_local int t_ourTextureLoad = 0;

// Fonts named by the game's own text styles (records/ui/styles/text/*.dbr).
const GdFont* g_fontTitle = nullptr;    // style_windowtitle01  : nevisnooutlinespaced, 20
const GdFont* g_fontHeader = nullptr;   // style_rollover_title : savapromedium, 18
const GdFont* g_fontBody = nullptr;     // style_textwhite_sizen: jura, 18
bool g_fontsTried = false;

// =============================================================================================
// small helpers
// =============================================================================================

inline GdRect gdrect(float x, float y, float w, float h) {
    GdRect r = {x, y, w, h};
    return r;
}
inline GdRect gdrect(const gdut::Rect& r) {
    return gdrect((float)r.x, (float)r.y, (float)r.w, (float)r.h);
}
inline int iround(float v) { return (int)(v + (v >= 0.0f ? 0.5f : -0.5f)); }

void fillRect(GdCanvas* c, const gdut::Rect& r, const GdColor& col) {
    if (r.w <= 0 || r.h <= 0 || !g_gd.CanvasRenderRectSolid) return;
    const GdRect gr = gdrect(r);
    g_gd.CanvasRenderRectSolid(c, &gr, &col);
}

// 1 px (scaled) outline drawn as four filled rects.
void outlineRect(GdCanvas* c, const gdut::Rect& r, int t, const GdColor& col) {
    if (t < 1) t = 1;
    fillRect(c, {r.x, r.y, r.w, t}, col);
    fillRect(c, {r.x, r.y + r.h - t, r.w, t}, col);
    fillRect(c, {r.x, r.y, t, r.h}, col);
    fillRect(c, {r.x + r.w - t, r.y, t, r.h}, col);
}

// Returns whether it drew, so the pad can count the captions that really landed.
bool drawText(GdCanvas* c, int x, int y, int size, const GdColor& col, const char* s,
              const GdFont* font) {
    if (!font) font = g_fontBody;
    if (!g_gd.CanvasRenderText2d || !font || !s || !*s || size <= 0) return false;
    g_gd.CanvasRenderText2d(c, x, y, &col, s, font, size, 0, 0, 0, 0);
    return true;
}

// =============================================================================================
// TEXT MEASUREMENT - how wide a string really is, in screen px
// =============================================================================================
//
// Every "how much text fits" decision in this file is made in pixels measured with the game's
// own font, never with a flat per-character estimate (a 0.52 em guess clips labels early and
// lets three-letter captions overhang their buttons).
//
// `ut_fontmetrics.h` is generated by
// `tools\font_metrics.py cxx savapromedium.fnt` out of resources\Fonts.arc, and the pen
// rule below is Engine.dll's, read out of its line builder at rva 0xA7630 (loop 0xA76C5..
// 0xA7819): per glyph the pen moves by
//     xOffset + rightBearing + round(w * size / styleSize)      [+ kerning, always <= 0]
// where `GetStyle` (0xAE550) picks the baked style size NEAREST the requested one and only the
// BITMAP is scaled, never the two bearings.  Kerning is deliberately left out, so every number
// here is an UPPER BOUND on what the engine will draw; the per-style `slack` covers the one
// other thing the loop does that this does not - it clamps a negative xOffset to 0 on the first
// glyph of a line (0xA7789).  Verified offline over sizes 5..36 against the exact pen (which
// does model kerning and the clamp): zero under-estimates, and on a full-length label the
// over-estimate is the 2-3 px of slack, under 1%.
//
// Only savapromedium is tabulated, because it is the only font the mod draws measured text in
// (g_fontHeader - the label and the button tags).  A caller that passes another font gets the
// savapromedium answer, which is why nothing else uses these.

// Engine.dll 0xAE550: the baked size nearest the requested one, first match wins on a tie.
int fmStyleFor(int size) {
    int best = 0;
    int bestD = 1 << 24;
    for (int i = 0; i < fontmetrics::kStyles; ++i) {
        int d = size - (int)fontmetrics::kStyleSize[i];
        if (d < 0) d = -d;
        if (d < bestD) {
            bestD = d;
            best = i;
        }
    }
    return best;
}

// Width in SCREEN px of `s` drawn by CanvasRenderText2d at `size`.  Upper bound, never under.
int textWidthPx(const char* s, int size) {
    if (!s || !*s || size <= 0) return 0;
    const int st = fmStyleFor(size);
    const int ss = (int)fontmetrics::kStyleSize[st];
    if (ss <= 0) return 0;
    int pen = 0;
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
        int c = (int)*p;
        if (c < fontmetrics::kFirstChar || c >= fontmetrics::kFirstChar + fontmetrics::kChars) {
            c = (int)'?';
        }
        const int i = c - fontmetrics::kFirstChar;
        const int w = (int)fontmetrics::kGlyphW[st][i];
        pen += (int)fontmetrics::kGlyphBearing[st][i] + (w * size + ss / 2) / ss;
    }
    return pen + (int)fontmetrics::kStyleSlack[st];
}

// Cuts `s` in place at the last character whose measured width still fits `limitPx`.  Returns
// true when nothing had to go.  There is no character budget: a clip is only ever made when
// the PIXELS run out.
bool textClipToWidth(char* s, int size, int limitPx) {
    if (!s || !*s) return true;
    if (textWidthPx(s, size) <= limitPx) return true;
    for (int n = (int)strlen(s) - 1; n > 0; --n) {
        const char keep = s[n];
        s[n] = 0;
        if (textWidthPx(s, size) <= limitPx) return false;
        s[n] = keep;
    }
    s[0] = 0;
    return false;
}

GdGraphicsEngine* graphicsEngine() {
    if (!g_gd.ppEngine || !g_gd.EngineGetGraphicsEngine) return nullptr;
    GdEngine* engine = *g_gd.ppEngine;
    if (!engine) return nullptr;
    return g_gd.EngineGetGraphicsEngine(engine);
}

// LoadTexture wrapped so the LoadTexture detour can tell our own calls from the game's.
const GdTexture* loadTextureOwn(const char* path) {
    GdGraphicsEngine* gfx = graphicsEngine();
    if (!gfx || !g_gd.GfxLoadTexture || !path) return nullptr;
    const std::string p(path);
    ++t_ourTextureLoad;
    const GdTexture* t = g_gd.GfxLoadTexture(gfx, &p);
    --t_ourTextureLoad;
    return t;
}

void ensureFonts() {
    if (g_fontsTried) return;
    g_fontsTried = true;
    GdGraphicsEngine* gfx = graphicsEngine();
    if (!gfx || !g_gd.GfxLoadFont) return;
    struct { const char* path; const GdFont** slot; } wanted[] = {
        {"fonts/nevisnooutlinespaced.fnt", &g_fontTitle},   // style_windowtitle01
        {"fonts/savapromedium.fnt", &g_fontHeader},         // style_rollover_title
        {"fonts/jura.fnt", &g_fontBody},                    // style_textwhite_sizen
    };
    for (int i = 0; i < 3; ++i) {
        const std::string p(wanted[i].path);
        *wanted[i].slot = g_gd.GfxLoadFont(gfx, &p);
        logD("panel: font \"%s\" -> %p", wanted[i].path, (void*)*wanted[i].slot);
    }
    if (!g_fontBody) g_fontBody = g_fontTitle ? g_fontTitle : g_fontHeader;
    if (!g_fontTitle) g_fontTitle = g_fontBody;
    if (!g_fontHeader) g_fontHeader = g_fontBody;
}

// =============================================================================================
// record name of a live Item*
// =============================================================================================

// MSVC's std::string, which is what the game's ItemReplicaInfo (and LoadTexture's argument) is
// built from - both sides use the VC14x runtime. 16-byte SSO buffer, then size, then capacity.
struct MsvcString {
    union {
        char buf[16];
        char* ptr;
    } bx;
    size_t size;
    size_t res;
};

bool msvcStringToBuf(const void* s, char* out, size_t outSize) {
    out[0] = 0;
    if (!s) return false;
    const MsvcString* ms = (const MsvcString*)s;
    if (ms->size == 0 || ms->size > 400 || ms->size > ms->res) return false;
    const char* data = (ms->res < 16) ? ms->bx.buf : ms->bx.ptr;
    if (!data) return false;
    size_t n = ms->size;
    if (n >= outSize) n = outSize - 1;
    memcpy(out, data, n);
    out[n] = 0;
    return true;
}

bool looksLikeRecord(const char* s, size_t n) {
    if (n < 8 || n > 250) return false;
    if (_strnicmp(s, "records/", 8) != 0 && _strnicmp(s, "records\\", 8) != 0) return false;
    return _stricmp(s + n - 4, ".dbr") == 0;
}

// The replica route reads the ItemReplicaInfo IN PLACE: it lives inside the Item, at
// Item + reagentReplicaOffset() (0x538 on 1.3.0.8, decoded from the getter's own `lea rdx,
// [rcx+disp32]`), with the base-record std::string at +0x08. Item::GetItemReplicaInfo is never
// called - it constructs std::strings into its sret buffer, which a static would leak and a
// layout change would overrun. If the offset could not be decoded this route reports "no
// record" and Object::GetObjectName, the primary route, is the only one.
volatile LONG g_replicaOffsetLogged = 0;

int readRecordInner(GdItem* item, char* out, size_t outSize) {
    out[0] = 0;
    if (g_gd.ObjectGetObjectName) {
        const char* name = g_gd.ObjectGetObjectName(item);
        if (name) {
            size_t n = 0;
            while (n < outSize - 1 && name[n]) ++n;
            if (n > 0) {
                memcpy(out, name, n);
                out[n] = 0;
                const LONG seq = InterlockedIncrement(&g_objectNameLogged);
                if (seq <= 20) logT("  Object::GetObjectName[%ld] = \"%s\"", seq, out);
                if (looksLikeRecord(out, n)) return 1;
            }
        }
    }
    out[0] = 0;
    const unsigned int replicaOff = reagentReplicaOffset();
    if (!replicaOff) {
        if (!InterlockedExchange(&g_replicaOffsetLogged, 1)) {
            logD("  Item::GetItemReplicaInfo route unavailable: the ItemReplicaInfo offset could "
                 "not be decoded, so no replica is read (Object::GetObjectName is the only route)");
        }
        return 0;
    }
    // ItemReplicaInfo + 0x08 is the base-record std::string (Item::CreateItem's own
    // `cmp [rdx+0x18],0x10` proves the MSVC 32-byte string is there). Read in place.
    const unsigned char* replica = (const unsigned char*)item + replicaOff;
    char tmp[256];
    if (!msvcStringToBuf(replica + 0x08, tmp, sizeof(tmp))) return 0;
    const size_t n = strlen(tmp);
    if (n >= outSize) return 0;
    memcpy(out, tmp, n + 1);
    return looksLikeRecord(out, n) ? 2 : 0;
}

int readRecord(GdItem* item, char* out, size_t outSize) {
    int route = 0;
    __try {
        route = readRecordInner(item, out, outSize);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
        route = 0;
    }
    return route;
}

unsigned int readObjectId(const void* obj) {
    unsigned int id = 0;
    __try {
        if (g_gd.ObjectGetObjectId) id = g_gd.ObjectGetObjectId(obj);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        id = 0;
    }
    return id;
}

int readClassification(GdItem* item) {
    int c = -1;
    __try {
        if (g_gd.ItemGetItemClassification) c = g_gd.ItemGetItemClassification(item, false);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        c = -1;
    }
    return c;
}


// The texture object the ENGINE itself already holds for this item (counted in the status line).
const GdTexture* readItemBitmap(GdItem* item) {
    const GdTexture* t = nullptr;
    __try {
        if (g_gd.ItemGetBitmap) t = g_gd.ItemGetBitmap(item);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        t = nullptr;
    }
    return t;
}

}  // namespace

// =============================================================================================
// public: init
// =============================================================================================

bool panelInit(HMODULE selfModule) {
    if (g_initialised || g_loadFailed) return g_initialised;

    // The catalogue ships with the mod and is read from the mod folder - ut_paths.h decides
    // where that is, and nothing else in this file may name a path.
    char catPath[MAX_PATH] = {0};
    utModFile(selfModule, "catalogue.bin", catPath, sizeof(catPath));
    const std::vector<std::string> candidates(1, std::string(catPath));

    std::string err;
    for (const std::string& p : candidates) {
        err.clear();
        if (g_cat.loadFromFile(p, &err)) {
            logI("catalogue loaded: %llu items, %llu sets (format v%u)",
                 (unsigned long long)g_cat.itemCount(), (unsigned long long)g_cat.setCount(),
                 g_cat.formatVersion());
            logD("catalogue: \"%s\" - %u equipment, %u relics, %u default-visible, %llu bytes",
                 p.c_str(), g_cat.equipmentCount(), g_cat.relicCount(),
                 g_cat.defaultVisibleCount(), (unsigned long long)g_cat.byteSize());
            int nonEmpty = 0;
            for (int gi = 0; gi < gdut::kSlotGroupCount; ++gi) {
                const gdut::GroupRange r = g_cat.groupRange((gdut::SlotGroup)gi);
                if (r.count == 0) continue;
                ++nonEmpty;
            }
            logD("catalogue: %d non-empty groups of %d", nonEmpty, gdut::kSlotGroupCount);
            {
                std::lock_guard<std::mutex> lock(g_modelMx);
                g_own.reset(&g_cat);
            }
            g_initialised = true;
            return true;
        }
        logE("catalogue: \"%s\" not usable: %s", p.c_str(), err.c_str());
    }
    g_loadFailed = true;
    logE("catalogue: NO usable catalogue.bin - the mod tab will say NO DATA and draw no grid");
    return false;
}

// =============================================================================================
// public: game-thread events
// =============================================================================================

void panelOnItemAdded(GdItem* item) {
    if (!item) return;
    InterlockedIncrement64(&g_addEvents);
    if (InterlockedCompareExchange64(&g_firstAddTick, (LONG64)GetTickCount64(), 0) == 0) {
        logD("first InventorySack::AddItem seen");
    }
    if (!g_initialised) return;

    char record[256];
    const int route = readRecord(item, record, sizeof(record));
    if (route == 1) InterlockedIncrement64(&g_nameRouteObject);
    else if (route == 2) InterlockedIncrement64(&g_nameRouteReplica);
    else {
        InterlockedIncrement64(&g_nameRouteNone);
        return;
    }
    const unsigned int id = readObjectId(item);
    if (id == 0) return;

    const int catIndex = g_cat.indexOfRecord(record);

    // Learn the engine's ItemClassification numbering from an item whose classification the
    // catalogue already knows. Before the lock: no engine call is made while it is held.
    const GdTexture* liveBitmap = nullptr;
    if (catIndex >= 0) {
        const int want = (int)g_cat.item((size_t)catIndex).classification;
        if (want >= 0 && want < 2 && g_classEnum[want] < 0) {
            const int seen = readClassification(item);
            if (seen >= 0) {
                g_classEnum[want] = seen;
                logD("ItemClassification: catalogue %s == engine enum %d (record %s)",
                     gdut::classificationName((gdut::Classification)want), seen, record);
            }
        }
        // The GraphicsTexture the engine itself holds for this item.
        liveBitmap = readItemBitmap(item);
    }

    std::lock_guard<std::mutex> lock(g_modelMx);
    g_own.onItemAdded(id, record);
    if (catIndex >= 0 && liveBitmap && g_liveBitmaps.find(catIndex) == g_liveBitmaps.end()) {
        g_liveBitmaps.emplace(catIndex, liveBitmap);
    }
    if (catIndex < 0 && g_unknownSamples.size() < 10) {
        bool seen = false;
        for (const std::string& s : g_unknownSamples)
            if (s == record) { seen = true; break; }
        if (!seen) g_unknownSamples.push_back(record);
    }
}

void panelOnItemRemoved(unsigned int itemId) {
    InterlockedIncrement64(&g_removeEvents);
    if (!g_initialised || itemId == 0) return;
    std::lock_guard<std::mutex> lock(g_modelMx);
    g_own.onItemRemoved(itemId);
}

void panelOnTransferOpen(GdGameEngine* gameEngine, bool open) {
    // The game calls SetTransferOpen(true) EVERY FRAME while the caravan is up, so everything
    // below runs on the transition only.
    const LONG was = InterlockedExchange(&g_transferOpen, open ? 1 : 0);
    if ((was != 0) == open) return;
    if (!open) {
        logD("STASH CLOSED");
        return;
    }

    // mem::vector<InventorySack*> is laid out like MSVC's release std::vector: three pointers.
    long long sacks = -1;
    if (gameEngine && g_gd.GameGetPlayerTransfer) {
        struct MemVectorRaw { void* first; void* last; void* endCap; };
        const MemVectorRaw* v = (const MemVectorRaw*)g_gd.GameGetPlayerTransfer(gameEngine);
        if (v) {
            const char* first = (const char*)v->first;
            const char* last = (const char*)v->last;
            const char* endCap = (const char*)v->endCap;
            if (first && last >= first && last <= endCap) {
                const ptrdiff_t bytes = last - first;
                if ((bytes % (ptrdiff_t)sizeof(void*)) == 0) {
                    const ptrdiff_t n = bytes / (ptrdiff_t)sizeof(void*);
                    if (n >= 0 && n <= 64) sacks = (long long)n;
                }
            }
        }
    }

    float cellW = 0.0f, cellH = 0.0f;
    if (gameEngine && g_gd.GameGetInventoryCellSize)
        g_gd.GameGetInventoryCellSize(gameEngine, &cellW, &cellH);

    unsigned int selected = 0;
    if (gameEngine && g_gd.GameGetSelectedTransferSack)
        selected = g_gd.GameGetSelectedTransferSack(gameEngine);

    std::lock_guard<std::mutex> lock(g_modelMx);
    logI("stash open: %d unique item(s) on this character, %d distinct record(s), tab %u",
         g_own.totalItems(), g_own.ownedCount(), selected);
    logD("stash open: sacks=%lld addEvents=%lld removeEvents=%lld unknownEvents=%llu "
         "distinctUnknown=%llu cellSize=%.2fx%.2f (scale %.3f) liveBitmaps=%u",
         sacks, (long long)g_addEvents, (long long)g_removeEvents,
         (unsigned long long)g_own.unknownRecordEvents(),
         (unsigned long long)g_own.distinctUnknownRecords(), cellW, cellH, cellW / 32.0f,
         (unsigned)g_liveBitmaps.size());
    logD("  record route: GetObjectName=%lld ItemReplicaInfo=%lld none=%lld",
         (long long)g_nameRouteObject, (long long)g_nameRouteReplica, (long long)g_nameRouteNone);
    for (size_t i = 0; i < g_unknownSamples.size(); ++i) {
        logD("  unknown record[%u] = \"%s\"", (unsigned)i, g_unknownSamples[i].c_str());
    }
}

// =============================================================================================
// the 26-button CATEGORY PAD
// =============================================================================================
// The design, in record px: a 9-column x 3-row grid of 34 x 14 cells, a 1 px gutter and a 3 px
// side margin over a dark ground - 320 x 44 record px at record x 102..421, y 25..68, wholly
// inside the rectangle that is flat and opaque on the GAME's own materials plate AND on all
// seven of the mod's generated plates: x 102..422, y 25..69 (measured by decoding both plates).
// 27 cells hold 26 buttons: index 0 is the vanilla crafting-materials page, then groups 0..23,
// then the OWN owned-only filter spanning the last TWO cells of row 3.  The label is ONE line
// and lives in the band above at y 8..24; the first box row starts at y 74.
//
// There is no texture in the pad - every pixel is a solid quad through fillRect/outlineRect
// (CanvasRenderRectSolid) plus the game's own savapromedium caption - so the colours land
// exactly as written instead of being multiplied into a texture's own brown.  The flat look,
// the 1 px top and bottom lines, the green inset selection ring and the amber LED toggle are
// the design's.
//
// A click is ABSOLUTE - button i selects group i-1, through liveSelectGroup(); the last one
// toggles the owned-only filter instead.  Drawn at the tail of the engine's own
// ReagentWindow::Draw, i.e. under the item tooltips, exactly like the label - but with its OWN
// gates: it does not inherit `plate_label`, and it does not go through liveViewInfo(), which
// returns false for group -1 and would kill the pad on the vanilla page.  The click side
// (ut_plate.cpp's detour on the ReagentWindow's mouse handler, vtable slot +0x38) calls the SAME
// panelButtonRects()/panelButtonTarget() as the draw, so the two can never disagree about where
// a button is or what clicking it means.
//
// Everything here is arithmetic plus RenderRect: no engine memory is written, nothing is
// allocated on the draw path, NO texture is loaded, and nothing is logged per
// frame.  Multiplayer neutrality is by construction - a click writes the same two ints
// Ctrl+wheel writes (ut_live.cpp's g_wantGroup/g_wantRow) and nothing else.

namespace {

// ---- the 26-button table -------------------------------------------------------------------
// Index 0 is the vanilla crafting-materials page (group -1), then groups 0..23 in the order
// uniq-groups.txt lists them - which is the order Ctrl+wheel already cycles, so the pad and the
// wheel can never disagree.  `tag` is the three-letter caption drawn in the button.
struct UtCatButton {
    const char* tag;   // exactly 3 uppercase chars, all 26 distinct
};
const UtCatButton kButtons[kUtButtonCount] = {
    {"MAT"},   //  0  group -1  the game's own Crafting Materials page
    {"HEL"},   //  1  group  0  Helms
    {"SHO"},   //  2  group  1  Shoulders
    {"CHE"},   //  3  group  2  Chest
    {"GLO"},   //  4  group  3  Gloves
    {"BEL"},   //  5  group  4  Belts
    {"PAN"},   //  6  group  5  Pants
    {"BOO"},   //  7  group  6  Boots
    {"AMU"},   //  8  group  7  Amulets
    {"MED"},   //  9  group  8  Medals
    {"RNG"},   // 10  group  9  Rings
    {"OFF"},   // 11  group 10  Off-hands
    {"SHD"},   // 12  group 11  Shields
    {"AX1"},   // 13  group 12  1H Axes
    {"DAG"},   // 14  group 13  Daggers
    {"MC1"},   // 15  group 14  1H Maces
    {"SCP"},   // 16  group 15  Scepters
    {"SW1"},   // 17  group 16  1H Swords
    {"GUN"},   // 18  group 17  Guns
    {"AX2"},   // 19  group 18  2H Axes
    {"MC2"},   // 20  group 19  2H Maces
    {"SPR"},   // 21  group 20  Spears
    {"SW2"},   // 22  group 21  2H Swords
    {"GN2"},   // 23  group 22  2H Guns
    {"REL"},   // 24  group 23  Relics
    // NOT a group: the owned-only filter toggle.  Its ON look is its own - a double-width amber
    // plate with a lit lamp - so it can never be mistaken for the selected group, which wears a
    // green ring.
    {"OWN"},   // 25  the owned-only filter
};
// The filter toggle is always the LAST button, so adding a group would never move it.
const int kFilterButtonIndex = kUtButtonCount - 1;
// button index <-> group index, in one place. Index 0 is always the way home.
inline int groupOfButton(int index) { return index - 1; }
inline int buttonOfGroup(int group) { return group + 1; }

// ---- geometry: the 9 x 3 BUTTON PAD ---------------------------------------------------------
// THE JOINT FREE RECTANGLE, measured by decoding both plates (they are
// TEX\x02 + 8 bytes + a DDS whose magic is DDSR, 438x627 32-bit BGRA):
//   the GAME's caravan_transfercomponent1_bg.tex is flat over x 102..427 for rows 25..69, and
//   flat over x 102..247 + x 280..418 for rows 8..24 (a central medallion owns 248..279);
//   all seven of the mod's own plates are flat over x 102..422 continuously from row 2 to 72.
// So the rectangle free on BOTH is x 102..422, y 25..69 (321 x 45) - and the text-only band
// above it is y 8..24, where the ONE-line label lives.  x 423..427 is legal on the vanilla
// plate and NOT on the mod plates, which is why the refusal limit is 422 and not 427.
const int kFreeX0 = 102;
const int kFreeX1 = 422;
const int kFreeY0 = 25;
const int kFreeY1 = 69;

// THE PAD: a 9-column x 3-row
// grid of 34 x pad_h cells with a pad_gap gutter and a 3 px side margin.
//   width  = 3 + 9*34 + 8*pad_gap + 3 = 320 at pad_gap=1  ->  record x 102..421
//   height = 3*pad_h + 2*pad_gap      =  44 at pad_h=14   ->  record y  25..68
// The HTML's 15 px button does NOT fit: 3*15 + 2*1 = 47 > 45.  14 does, and it clears the box
// grid (which starts at y 74) by 6 rows and the vanilla plate's lower ornament (x 261..267 in
// rows 70..72) entirely.  Only pad_y / pad_h / pad_gap are ini keys - the column count, the
// 34 px cell and the 3 px margin ARE the design.
const int kPadCols = 9;
const int kPadRows = 3;
const int kPadCellW = 34;
const int kPadMargin = 3;
const int kPadX0 = kFreeX0;   // the pad's left edge IS the free rectangle's left edge
// 26 buttons in 27 cells: the vanilla materials page, the 24 collection groups, and the OWN
// owned-only filter spanning the LAST TWO cells of row 3 (the HTML's `grid-column: span 2`).
// Every array here is sized from this one constant, and it is the header's kUtButtonCount so
// the two can never drift apart.
const int kButtonCount = kUtButtonCount;
static_assert(kPadCols * kPadRows == kUtButtonCount + 1,
              "the pad is 27 cells and 26 buttons - the OWN toggle spans two of them");
static_assert(kPadX0 >= kFreeX0, "the pad may not start left of the joint free rectangle");

// The ONE layout, in record px.  pad_h and pad_gap are CLAMPED to their sanity window here, on
// use and never on parse (the ini is re-read once a second, so a silly value must not stick);
// the RESULTING RECTANGLE is then REFUSED WHOLE - the pad draws nothing at all and says so once -
// when it would leave the joint free rectangle: the pad is either wholly on plate art that is
// flat on both plate types, or it is not drawn.
struct UtStripLayout {
    int x0, y0, w, h;   // the whole pad, record px, both edges inclusive of x0+w-1 / y0+h-1
    int gridX0;         // the first cell's left edge = x0 + kPadMargin
    int btnH, gap;      // the clamped pad_h / pad_gap
};
bool stripLayout(UtStripLayout* L) {
    int bh = g_cfg.padH;
    // The floor is 12, not 8.  The three-letter captions are the pad's ONLY mark, and
    // buttonTagSize returns 0 - no caption drawn at all - for any cell under 8 screen px tall.
    // At the game's own UI-scale floor of 0.700 a pad_h of 8..11 gives a 5..7 px cell, i.e. 26
    // IDENTICAL BLANK buttons that the click path would still arm; 12 is the smallest height
    // that still yields a size-6 caption there
    // (measured over the shipped ut_fontmetrics.h at 0.700 / 0.800 / 0.928 / 1.000 / 1.250 /
    // 1.500 / 2.000).  The template and ut_config.h say 12..20 for the same reason.
    if (bh < 12) bh = 12;
    if (bh > 20) bh = 20;
    int gap = g_cfg.padGap;
    if (gap < 0) gap = 0;
    if (gap > 4) gap = 4;
    const int y0 = g_cfg.padY;
    const int w = 2 * kPadMargin + kPadCols * kPadCellW + (kPadCols - 1) * gap;
    const int h = kPadRows * bh + (kPadRows - 1) * gap;
    if (kPadX0 + w - 1 > kFreeX1) return false;   // 9 columns must end by x 422
    if (y0 < kFreeY0) return false;               // never on the vanilla plate's frame
    if (y0 + h - 1 > kFreeY1) return false;       // never on its lower ornament or the grid rule
    L->x0 = kPadX0;
    L->y0 = y0;
    L->w = w;
    L->h = h;
    L->gridX0 = kPadX0 + kPadMargin;
    L->btnH = bh;
    L->gap = gap;
    return true;
}

// ONE function produces every rect: cell index == button index for 0..24, and button 25 (OWN)
// starts at cell 25 and is two cells wide.  Both the draw and panelButtonHit go through it.
void padCellRec(const UtStripLayout& L, int index, int* rx, int* ry, int* rw, int* rh) {
    const int row = index / kPadCols;
    const int col = index % kPadCols;
    *rx = L.gridX0 + col * (kPadCellW + L.gap);
    *ry = L.y0 + row * (L.btnH + L.gap);
    *rw = (index == kFilterButtonIndex) ? (2 * kPadCellW + L.gap) : kPadCellW;
    *rh = L.btnH;
}

// ---- the palette (the pad design's hex colours -> 0..1 floats) -----------------------------
// CanvasRenderRectTex MULTIPLIES its colour into the texture, so a textured button could never
// show a colour as written.  Nothing here is textured: every quad below goes through
// fillRect/outlineRect, i.e. CanvasRenderRectSolid, where there is no texel to multiply into -
// so each value lands on screen exactly as written.
const GdColor kPadBack   = {0.055f, 0.059f, 0.067f, 1.00f};   // #0e0f11 the pad ground
const GdColor kBtnBg     = {0.227f, 0.247f, 0.278f, 1.00f};   // #3a3f47
const GdColor kBtnHover  = {0.275f, 0.314f, 0.349f, 1.00f};   // #465059
const GdColor kBtnDown   = {0.165f, 0.180f, 0.204f, 1.00f};   // #2a2e34
const GdColor kLineTop   = {0.329f, 0.353f, 0.392f, 1.00f};   // #545a64
const GdColor kLineBot   = {0.141f, 0.153f, 0.173f, 1.00f};   // #24272c
const GdColor kRing      = {0.247f, 0.831f, 0.416f, 1.00f};   // #3fd46a the selected group
const GdColor kCaption   = {0.910f, 0.918f, 0.929f, 1.00f};   // #e8eaed
const GdColor kTglBg     = {0.176f, 0.196f, 0.220f, 1.00f};   // #2d3238 the OWN toggle, off
const GdColor kTglOnBg   = {0.851f, 0.604f, 0.118f, 1.00f};   // #d99a1e the OWN toggle, on
const GdColor kTglOnTop  = {0.941f, 0.741f, 0.333f, 1.00f};   // #f0bd55
// The ON caption is a dark WARM brown, #473719, 4.70:1 over the amber plate #d99a1e (WCAG 2.x
// relative luminance on the 8-bit hex; the floats are that hex to 3 decimals like every other
// entry in this table).  Near black on gold reads as bold, and #473719 is the lightest step on
// this hue ramp that still clears the >= 4.5:1 floor (#48381a is 4.63, #49391b 4.56, #4a3a1c
// 4.49 and under).  The bottom line is a shade of the plate, #a07520, rather than a second dark
// mark next to the caption.
const GdColor kTglOnBot  = {0.627f, 0.459f, 0.125f, 1.00f};   // #a07520
const GdColor kTglOnText = {0.278f, 0.216f, 0.098f, 1.00f};   // #473719, 4.70:1
const GdColor kLedOff    = {0.333f, 0.357f, 0.388f, 1.00f};   // #555b63
const GdColor kLedOn     = {1.000f, 0.957f, 0.761f, 1.00f};   // #fff4c2
const GdColor kLedHalo   = {1.000f, 0.898f, 0.541f, 0.55f};   // #ffe58a, the CSS box-shadow glow
// The search mark is not green - green is the selection ring.  It sits on the BACKGROUND as a
// muted blue, so a button can be marked AND selected at once and both facts read: blue plate,
// green ring, parchment-white caption.  The plate is 1.66:1 against the unmarked button #3a3f47
// (L* 32.6 -> 40.3), a step that announces itself in a pad of 26 without turning into a second
// selection mark, and the caption #e8eaed still reads on both: 5.31:1 on the plate and 4.56:1
// hovered.  The HOVER is the constrained one: #456c93 is the brightest step on this ramp that
// keeps the caption above the 4.5:1 floor (#466d94 is already 4.496).
const GdColor kMarkBg    = {0.227f, 0.384f, 0.533f, 1.00f};   // #3a6288, cap 5.31:1
const GdColor kMarkHover = {0.271f, 0.424f, 0.576f, 1.00f};   // #456c93, cap 4.56:1

// ---- session state (mod-owned; never a g_cfg field - configReload replaces g_cfg once a sec) -
volatile LONG g_buttonsOff = 0;        // the strip is dead for this session
volatile LONG g_buttonsDrawFault = 0;  // one-shot report for ut_plate (mirrors the label)
volatile LONG g_btnDrawnAt = 0;        // GetTickCount of the last frame the strip painted
volatile LONG g_btnDownAt = 0;         // GetTickCount of the last consumed click
volatile LONG g_btnDownIdx = -1;       // which button it was
volatile LONG g_btnStripLogged = 0;    // the once-per-world "strip drawn" line
volatile LONG g_btnBlankLogged = 0;    // the once-per-world "no caption fits this cell" line
int g_btnHover = -1;                   // game thread only (the Draw detour)

// The widest of the 26 captions at `size`.  Pure arithmetic over
// ut_fontmetrics.h - no engine call - and the answer only changes when the cell does, so
// buttonTagSize caches it on the cell.
int widestTagPx(int size) {
    int w = 0;
    for (int i = 0; i < kButtonCount; ++i) {
        const int t = textWidthPx(kButtons[i].tag, size);
        if (t > w) w = t;
    }
    return w;
}

// The largest caption size that fits the SMALLEST cell, recomputed for the pad.
//
// The design's own ratio is bold 10 px in a 15 px button = 0.667; three quarters of the cell
// height lands on exactly 10 in the 14 px cell at UI scale 1.000, which is the design, and on 6
// in the 9 px cell at the hard UI-scale floor of 0.700, which is still readable there.  The two
// 1 px lines live INSIDE the button (the HTML's border-box), so
// the size is also capped at boxH - 2 and the glyph run can never sit on them.
//
// WIDTH is not the binding constraint: the cell is 34 record px, so at 0.700 the narrowest cell
// is 23 SCREEN px against a widest caption of 16 px at size 6.  The budget is `boxW - 1` - one
// pixel of breathing room across the whole
// button, not one per side - and `(dst.w - tw) / 2` truncates toward ZERO, so that pixel lands
// wholly on the RIGHT edge, inside the gutter, never on the neighbour.
// Measured with the pad's own rects: 0.700 -> 23x9 cells, size 6, widest 16; 0.928 -> 31x13,
// size 9; 1.000 -> 34x14, size 10, widest 29; 1.250 -> 42x17, size 12; 2.000 -> 68x28, size 14
// (the ceiling: above it a three-letter mark starts to read as a word).
// Those numbers come from the shipped ut_fontmetrics.h with this file's own per-edge
// floorf(v*s+0.5f).
//
// A return of ZERO means even size 6 does not fit the cell's HEIGHT; the caller then draws no
// caption at all, so drawButtonsInner says so once.  pad_h's clamp floor of 12 keeps that out
// of the whole legal ini window at every UI scale the game itself offers (>= 0.700); only a
// hooked or mis-measured scale below that can still reach it.
int buttonTagSize(int boxW, int boxH) {
    static int lastW = -1;
    static int lastH = -1;
    static int lastSize = 0;
    if (boxW == lastW && boxH == lastH) return lastSize;
    int top = (boxH * 3) / 4;
    if (top > boxH - 2) top = boxH - 2;
    if (top > 14) top = 14;
    int fit = 0;
    for (int cand = top; cand >= 6; --cand) {
        if (widestTagPx(cand) <= boxW - 1) {
            fit = cand;
            break;
        }
    }
    lastW = boxW;
    lastH = boxH;
    lastSize = fit;
    return fit;
}

// 1 px of the DESIGN, in screen px, never 0 - at UI scale 0.700 a 1 px line rounds to 1 and at
// 2.000 to 2, and a 0 px line would silently delete the whole look.
int padPx(float s, int recordPx) {
    int v = iround((float)recordPx * s);
    if (v < 1) v = 1;
    return v;
}

bool drawButtonsInner(GdCanvas* canvas, float ox, float oy) {
    // The pad is solid quads and nothing else.  CanvasRenderRectSolid is a REQUIRED export (a
    // miss installs no detour at all, gd_runtime.cpp), so this can only be null in a build that
    // never armed - but the whole `painted` proof below rests on it, so it is tested here rather
    // than assumed: with it gone NOTHING can appear and no click may be authorised.
    if (!g_gd.CanvasRenderRectSolid) return false;
    const float s = liveUiScale();          // never 1.0 by default; 0 = draw nothing
    UtButtonRect r[kButtonCount];
    if (panelButtonRects(r, s) != kButtonCount) return false;
    UtStripLayout L;
    if (!stripLayout(&L)) return false;
    const int active = buttonOfGroup(liveShownGroup(nullptr));
    // The OWN button is not a group, so it can never be `active` (buttonOfGroup never returns
    // kFilterButtonIndex).  It gets its own ON look from its own flag.
    const bool filterOn = liveOwnedOnly();
    const DWORD now = GetTickCount();
    const LONG downIdx = InterlockedCompareExchange(&g_btnDownIdx, -1, -1);
    const DWORD downAt = (DWORD)InterlockedCompareExchange(&g_btnDownAt, 0, 0);
    const bool downLive = downIdx >= 0 && downAt != 0 && (now - downAt) < 150;
    const unsigned int matchMask = plateSearchMask();
    const int line = padPx(s, 1);

    // The pad GROUND, drawn first: it is what the 1 px gutters and the 3 px side margins are
    // made of, exactly as the HTML's `background:#0e0f11` shows through its grid gaps.  Without
    // it a 1 px gutter would be a 1 px window onto the plate art and the grid would not read.
    const int px0 = iround(ox + floorf((float)L.x0 * s + 0.5f));
    const int py0 = iround(oy + floorf((float)L.y0 * s + 0.5f));
    const int px1 = iround(ox + floorf((float)(L.x0 + L.w) * s + 0.5f));
    const int py1 = iround(oy + floorf((float)(L.y0 + L.h) * s + 0.5f));
    const gdut::Rect ground = {px0, py0, px1 - px0, py1 - py0};
    fillRect(canvas, ground, kPadBack);

    // The caption size is fitted to the SMALLEST NARROW cell.  The OWN toggle is deliberately
    // left out of the minimum: it is two cells wide, so it can never be the one that overflows,
    // and letting it in would only ever raise the minimum and never lower it.
    int minW = iround(r[0].w);
    int minH = iround(r[0].h);
    for (int i = 1; i < kButtonCount; ++i) {
        if (i == kFilterButtonIndex) continue;
        if (iround(r[i].w) < minW) minW = iround(r[i].w);
        if (iround(r[i].h) < minH) minH = iround(r[i].h);
    }
    const int fs = buttonTagSize(minW, minH);
    // A cell too short for even a size-6 caption draws 26 INDISTINGUISHABLE buttons, and
    // `painted` would still authorise consuming clicks on them.
    // pad_h's clamp floor of 12 makes that unreachable from the ini at any UI scale the game
    // offers; this says so once per world for the scales below 0.700 that only a hooked or
    // mis-measured GetUIScaleFactor can produce.  Once-per-world, never per frame.
    if (fs == 0 && !InterlockedExchange(&g_btnBlankLogged, 1)) {
        logW("buttons: the %dx%d px cell (pad_h=%d at UI scale %.3f) is too short for a caption - "
             "%d buttons are drawn UNLABELLED",
             minW, minH, L.btnH, s, kButtonCount);
    }
    int tags = 0, painted = 0;
    for (int i = 0; i < kButtonCount; ++i) {
        const gdut::Rect dst = {iround(ox + r[i].x), iround(oy + r[i].y), iround(r[i].w),
                                iround(r[i].h)};
        if (dst.w <= 0 || dst.h <= 0) continue;
        const bool toggle = (i == kFilterButtonIndex);
        // "this button is ON" - the shown group, or the OWN toggle while the owned-only filter
        // is in force.  ONE flag, so the background, the caption colour and the ring can never
        // disagree about it.
        const bool on = toggle ? filterOn : (i == active);
        // index 0 is the vanilla materials page and the last one is the filter toggle; neither
        // is a collection group, so neither can carry a search mark.
        const int grp = (i > 0 && !toggle) ? groupOfButton(i) : -1;
        const bool match = grp >= 0 && grp < 24 && (matchMask & (1u << (unsigned)grp)) != 0;
        const bool hot = (i == g_btnHover);
        const bool down = downLive && (int)downIdx == i;
        // The HTML's cascade, in order.  `.tgl[aria-pressed=true]` beats :active and :hover;
        // `:active` beats `:hover`; `.pad button:hover` beats `.pad .tgl`'s own background.
        GdColor bg = kBtnBg, top = kLineTop, bot = kLineBot, cap = kCaption;
        if (toggle && on) {
            bg = kTglOnBg;
            top = kTglOnTop;
            bot = kTglOnBot;
            cap = kTglOnText;
        } else if (down) {
            bg = kBtnDown;
            top = kLineBot;   // the HTML swaps the two lines on :active
            bot = kLineTop;
        } else if (match) {
            bg = hot ? kMarkHover : kMarkBg;
        } else if (hot) {
            bg = kBtnHover;
        } else if (toggle) {
            bg = kTglBg;
        }
        fillRect(canvas, dst, bg);
        ++painted;
        // The two 1 px edges, INSIDE the button (the HTML is border-box), so they never change
        // its size.  Skipped only when the button is too short to carry both.
        if (dst.h > 2 * line) {
            fillRect(canvas, {dst.x, dst.y, dst.w, line}, top);
            fillRect(canvas, {dst.x, dst.y + dst.h - line, dst.w, line}, bot);
        }
        // The caption, centred on its MEASURED width so the glyph run really ends inside the
        // button.  The toggle carries a 6 px LED to its left with a 5 px gap, and the LED plus
        // the gap plus the caption are centred as one group - the HTML's flex row.
        int tx = dst.x;
        const int ty = dst.y + (dst.h - fs) / 2;
        if (toggle) {
            const int led = padPx(s, 6);
            const int lgap = padPx(s, 5);
            const int tw = fs >= 6 ? textWidthPx(kButtons[i].tag, fs) : 0;
            int cx = dst.x + (dst.w - (led + lgap + tw)) / 2;
            if (cx < dst.x + line) cx = dst.x + line;
            gdut::Rect ledR = {cx, dst.y + (dst.h - led) / 2, led, led};
            if (ledR.y < dst.y + line) ledR.y = dst.y + line;
            if (ledR.y + ledR.h > dst.y + dst.h - line) ledR.h = dst.y + dst.h - line - ledR.y;
            if (ledR.h > 0) {
                if (on) {
                    // The CSS glow, as one quad: a 1 px lighter halo behind the lamp, drawn only
                    // when it fits inside the button so it can never spill into the gutter.
                    // The halo is 1 DESIGN px through padPx like every other line on the pad,
                    // not 1 screen px, so a 12 px lamp at UI scale 2.000 gets a 2 px glow.
                    const int hw = padPx(s, 1);
                    const gdut::Rect halo = {ledR.x - hw, ledR.y - hw, ledR.w + 2 * hw,
                                             ledR.h + 2 * hw};
                    if (halo.x >= dst.x && halo.y >= dst.y &&
                        halo.x + halo.w <= dst.x + dst.w && halo.y + halo.h <= dst.y + dst.h) {
                        fillRect(canvas, halo, kLedHalo);
                    }
                    fillRect(canvas, ledR, kLedOn);
                } else {
                    fillRect(canvas, ledR, kLedOff);
                }
            }
            tx = cx + led + lgap;
        } else if (fs >= 6) {
            const int tw = textWidthPx(kButtons[i].tag, fs);
            tx = dst.x + (dst.w - tw) / 2;
            if (tx < dst.x) tx = dst.x;
        }
        if (fs >= 6 && drawText(canvas, tx, ty, fs, cap, kButtons[i].tag, g_fontHeader)) ++tags;
        // The selected group's INSET ring: drawn INSIDE the button rect, so it changes no size
        // at all - it replaces the button's own two edge lines rather than sitting between them
        // and the caption, which at the 9 px cell of UI scale 0.700 would cross the glyphs.
        // The OWN toggle never wears it: its ON look is the amber plate and the lit lamp, and a
        // second ON mark on the same button would only be one mark too many.
        if (on && !toggle) {
            outlineRect(canvas, dst, line, kRing);
        }
    }
    // Only a pad the user can SEE may authorise a consumed click, so the "drawn this frame-pair"
    // stamp the click path reads is set from what was really painted: every quad above went
    // through CanvasRenderRectSolid, which was proved non-null at the top of this function, so
    // `painted > 0` means the buttons really are on the screen.
    if (painted <= 0) return false;
    InterlockedExchange(&g_btnDrawnAt, (LONG)now);
    if (!InterlockedExchange(&g_btnStripLogged, 1)) {
        logT("buttons: strip drawn at origin=(%.1f,%.1f) scale=%.3f - the %d x %d pad, %d "
             "buttons in %d cells: screen x=%d..%d y=%d..%d; record x %d..%d y %d..%d inside "
             "the joint free rectangle 102..422 / 25..69; caption size %d, widest caption %d px "
             "in a %dx%d px cell; %d caption(s), %d button(s), 0 textures",
             ox, oy, s, kPadCols, kPadRows, kButtonCount, kPadCols * kPadRows, ground.x,
             ground.x + ground.w - 1, ground.y, ground.y + ground.h - 1, L.x0, L.x0 + L.w - 1,
             L.y0, L.y0 + L.h - 1, fs, fs >= 6 ? widestTagPx(fs) : 0, minW, minH, tags, painted);
    }
    return true;
}

// SEH cannot share a function with C++ unwinding (C2712), so the guard is its own frame and
// everything it touches is a call. A fault latches the MOD-OWNED session switch and reports
// itself to ut_plate exactly the way the label does.
bool groupButtonsGuarded(GdCanvas* canvas, float ox, float oy) {
    bool drew = false;
    __try {
        drew = drawButtonsInner(canvas, ox, oy);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (!InterlockedExchange(&g_buttonsDrawFault, 1)) {
            logW("buttons: FAULT 0x%08lX inside the category strip drawn from "
                 "ReagentWindow::Draw - the strip is OFF for this session",
                 GetExceptionCode());
            InterlockedExchange(&g_buttonsOff, 1);
        }
    }
    return drew;
}

}  // namespace

int panelButtonRow1Count() {
    UtStripLayout L;
    return stripLayout(&L) ? kPadCols : 0;
}
int panelButtonRecX0() {
    UtStripLayout L;
    return stripLayout(&L) ? L.x0 : 0;
}
int panelButtonRecX1() {
    UtStripLayout L;
    return stripLayout(&L) ? L.x0 + L.w - 1 : 0;
}
int panelButtonRecY0() {
    UtStripLayout L;
    return stripLayout(&L) ? L.y0 : 0;
}
int panelButtonRecY1() {
    UtStripLayout L;
    return stripLayout(&L) ? L.y0 + L.h - 1 : 0;
}

int panelButtonRects(UtButtonRect out[kButtonCount], float s) {
    if (!out) return 0;
    // (1) the scale guard stays even with the exported GetUIScaleFactor: a hooked or odd value
    //     must still refuse the pad, never scale it.
    if (!(s >= 0.2f && s <= 8.0f)) return 0;
    UtStripLayout L;
    if (!stripLayout(&L)) return 0;
    for (int i = 0; i < kButtonCount; ++i) {
        int rx = 0, ry = 0, rw = 0, rh = 0;
        padCellRec(L, i, &rx, &ry, &rw, &rh);
        // Both EDGES are rounded (not width * scale), so neighbouring cells stay gutter-exact
        // and hit-test-exact at every UI scale - the same floorf(v*s+0.5f) applyLayout uses.
        out[i].x = floorf((float)rx * s + 0.5f);
        out[i].y = floorf((float)ry * s + 0.5f);
        out[i].w = floorf((float)(rx + rw) * s + 0.5f) - out[i].x;
        out[i].h = floorf((float)(ry + rh) * s + 0.5f) - out[i].y;
    }
    return kButtonCount;
}

// The probe band is the THREE ROW BANDS in y - never the whole plate - or the pad starts
// eating clicks in empty plate area. In x it is the joint rectangle, deliberately wider than
// the buttons, because a probe has to see the events that MISS them too.
bool panelButtonsInBand(float lx, float ly, float s) {
    if (!(s >= 0.2f && s <= 8.0f)) return false;
    UtButtonRect r[kButtonCount];
    if (panelButtonRects(r, s) != kButtonCount) return false;
    if (!(lx >= (float)kFreeX0 * s && lx < (float)kFreeX1 * s)) return false;
    for (int i = 0; i < kButtonCount; ++i) {
        if (ly >= r[i].y && ly < r[i].y + r[i].h) return true;   // y only: the row bands
    }
    return false;
}

int panelButtonHit(float lx, float ly, float s) {
    UtButtonRect r[kButtonCount];
    if (panelButtonRects(r, s) != kButtonCount) return -1;
    for (int i = 0; i < kButtonCount; ++i) {
        if (lx >= r[i].x && lx < r[i].x + r[i].w && ly >= r[i].y && ly < r[i].y + r[i].h) {
            return i;
        }
    }
    return -1;
}

// A click is ABSOLUTE - one button, one group - so this is a table lookup and nothing else.
int panelButtonTarget(int index, const char** label) {
    if (label) *label = "?";
    if (index < 0 || index >= kButtonCount) return -2;
    // The last button toggles the owned-only filter and selects no group.
    if (index == kFilterButtonIndex) {
        if (label) *label = "owned-only filter";
        return kUtButtonFilterTarget;
    }
    const int group = groupOfButton(index);
    if (label) *label = liveGroupLabel(group);
    return group;
}

// The button's own three-letter tag, for a log line. Never null.
const char* panelButtonTag(int index) {
    if (index < 0 || index >= kButtonCount) return "?";
    return kButtons[index].tag;
}

void panelButtonsHoverTick(float ox, float oy) {
    g_btnHover = -1;
    if (!g_cfg.groupButtons) return;
    if (InterlockedCompareExchange(&g_buttonsOff, 0, 0)) return;
    if (!liveActive()) return;
    const float s = liveUiScale();
    if (!(s >= 0.2f && s <= 8.0f)) return;
    POINT pt;
    HWND h = GetActiveWindow();
    if (!h) h = GetForegroundWindow();
    if (!h || !GetCursorPos(&pt) || !ScreenToClient(h, &pt)) return;
    g_btnHover = panelButtonHit((float)pt.x - ox, (float)pt.y - oy, s);
}

bool panelDrawGroupButtons(GdCanvas* canvas, float originX, float originY) {
    if (!canvas || !g_cfg.groupButtons) return false;          // FIRST test: no work before it
    if (InterlockedCompareExchange(&g_buttonsOff, 0, 0)) return false;
    if (!liveActive()) return false;   // with live_pages=0 there is no group to switch to
    return groupButtonsGuarded(canvas, originX, originY);
}

bool panelButtonsDrawFaulted() { return InterlockedExchange(&g_buttonsDrawFault, 0) != 0; }

void panelButtonsOff(const char* why) {
    if (InterlockedExchange(&g_buttonsOff, 1)) return;
    logW("buttons: FAULT - %s - the strip is OFF for this session", why ? why : "?");
}

bool panelButtonsAreOff() { return InterlockedCompareExchange(&g_buttonsOff, 0, 0) != 0; }

bool panelButtonsDrawnRecently() {
    const LONG at = InterlockedCompareExchange(&g_btnDrawnAt, 0, 0);
    if (!at) return false;
    return (GetTickCount() - (DWORD)at) <= 500;
}

void panelButtonsNotePress(int index) {
    InterlockedExchange(&g_btnDownIdx, (LONG)index);
    InterlockedExchange(&g_btnDownAt, (LONG)GetTickCount());
}

void panelForgetUiFonts() {
    // The FONTS have a per-world lifetime: drawText() cannot tell a stale GraphicsFont2* from a
    // good one (it returns true as soon as the pointer is non-null), so a font that did not
    // survive a world teardown would fault, latch g_buttonsOff and kill the pad for the rest of
    // the PROCESS, not just that world. Dropping the pointers costs one GfxLoadFont per world on
    // the next Materials tick. There is deliberately NO UnloadFont here: these three fonts are
    // the GAME's own (style_windowtitle01 / style_rollover_title / style_textwhite_sizen) and
    // releasing a font the engine is still drawing its own UI with is not the mod's to do.
    g_fontsTried = false;
    g_fontTitle = nullptr;
    g_fontHeader = nullptr;
    g_fontBody = nullptr;
    g_btnHover = -1;
    InterlockedExchange(&g_btnStripLogged, 0);
    InterlockedExchange(&g_btnBlankLogged, 0);
    InterlockedExchange(&g_btnDrawnAt, 0);
    InterlockedExchange(&g_btnDownAt, 0);
    InterlockedExchange(&g_btnDownIdx, -1);
}

void panelButtonsPreload() {
    if (!g_cfg.groupButtons) return;
    if (InterlockedCompareExchange(&g_buttonsOff, 0, 0)) return;
    if (!liveActive()) return;
    // There is no texture to preload - the pad is solid quads.  The FONT is loaded here, on the
    // tick, because the caption is the pad's only mark and the only
    // other place the fonts are loaded is panelDrawGroupLabel, which is gated on `plate_label`:
    // with plate_label=0 the pad would draw 26 wordless plates for the whole session.  One-shot
    // for the world (g_fontsTried), so this is free after the first Materials-page tick.
    ensureFonts();
}

void panelButtonsAnnounce() {
    const int n1 = panelButtonRow1Count();
    logD("buttons: group_buttons=%d pad_y=%d pad_h=%d pad_gap=%d - 26 category buttons in a "
         "%d x %d pad of %d px cells (the strip arms in game only)",
         g_cfg.groupButtons, g_cfg.padY, g_cfg.padH, g_cfg.padGap, kPadCols, kPadRows,
         kPadCellW);
    {
        char tags[192];   // 26 three-letter tags plus separators
        tags[0] = 0;
        int at = 0;
        for (int i = 0; i < kButtonCount; ++i) {
            const int n = _snprintf_s(tags + at, sizeof(tags) - (size_t)at, _TRUNCATE, "%s%s",
                                      i ? " " : "", kButtons[i].tag);
            if (n < 0) break;
            at += n;
        }
        logD("buttons: strip order (index 0 = the vanilla materials page, then groups 0..23, "
             "then OWN = the owned-only filter): %s",
             tags);
    }
    if (n1 <= 0) {
        logW("buttons: the ini geometry is REFUSED (pad_y=%d pad_h=%d pad_gap=%d would leave the "
             "joint free rectangle x 102..422 y 25..69) - no button is drawn",
             g_cfg.padY, g_cfg.padH, g_cfg.padGap);
    } else {
        // The COMPUTED rectangle, in record px and inclusive of both edges, so the menu test can
        // assert the numbers that matter without a HUD: the right edge must stay inside 422 (the
        // joint limit - the mod's own plates stop there even though the vanilla plate is flat to
        // 427) and the bottom inside 69.
        // n1 > 0 already means stripLayout succeeded, but the zero-init makes that independent
        // of this function's control flow rather than a fact a reader has to re-derive.
        UtStripLayout L = {};
        const bool ok = stripLayout(&L);
        logD("buttons: geometry accepted - record x %d..%d y %d..%d, %d rows of %d cells at "
             "y %d, %d and %d, %d x %d px buttons on a %d px gap (the joint free rectangle is "
             "x 102..422 y 25..69)",
             panelButtonRecX0(), panelButtonRecX1(), panelButtonRecY0(), panelButtonRecY1(),
             kPadRows, kPadCols, ok ? L.y0 : 0, ok ? L.y0 + L.btnH + L.gap : 0,
             ok ? L.y0 + 2 * (L.btnH + L.gap) : 0, kPadCellW, ok ? L.btnH : 0, ok ? L.gap : 0);
    }
    // The filter's POLICY, said once at start-up so the menu test can assert it without a HUD
    // (the button itself, the relayouts and the toggle are all in-game only).
    logD("search: owned_only=%d - the 26th category button [OWN] toggles the owned-only filter. "
         "With it ON only the records the reagent map already holds are laid out; every other "
         "box is parked off-grid exactly like the 136 fillers, which also hides it from the "
         "GAME'S own search, because UpdateSearch skips a box whose item id is 0. A deposit can "
         "never be lost by it - the deposit path takes an item id and never touches a box - and "
         "an accepted deposit forces the map walk plus one "
         "relayout, so a record deposited under the filter appears at once.",
         g_cfg.ownedOnly);
    logD("search: search_buttons=%d unowned=%d - a category button is MARKED (its BACKGROUND "
         "turns a muted blue, RGB 58,98,136, hovered 69,108,147; green is the selected group's "
         "ring) while its group holds a match for whatever is typed "
         "in the caravan's own search box. The predicate is the GAME'S OWN exported "
         "Item::SearchText, asked of the stored prototype of every record you OWN - so it matches "
         "an item's whole rollover text (the name, the type line, every stat line, affixes, "
         "components), not just its name - swept 32 records per game-thread tick from the moment "
         "the needle changes, and the buttons stay unlit until that sweep finishes. The group on "
         "screen also marks itself, free and instantly, from the engine's own matched-box list. "
         "With owned_only=1 that answer is complete and exact for the page; with the filter off, "
         "unowned records are swept too (search_buttons_unowned) - exactly, through the display "
         "prototype of every record this world has shown once, and by catalogue NAME for the rest. "
         "The `search: tier C policy` line prints that half.",
         g_cfg.searchButtons, g_cfg.searchButtonsUnowned);
}

namespace {

// =============================================================================================
// the group label over the reagent page
// =============================================================================================
// Drawn ONLY while the caravan window is open AND a collection group is on screen, so the main
// menu and the vanilla Crafting Materials layout are untouched.  It is independent of the
// `enabled` overlay switch.
//
// Geometry, all in RECORD space and then scaled exactly like every other number in this file:
//   caravan_materialwindow.dbr WindowLocationX/Y = (0, 88)   -> the reagent window
//   caravan_materialcoverimage.dbr bitmapPosition = (0, 1)   -> the plate inside it
//   uniq-groups.txt origin                        = (105,76) -> the first box inside the window
// so the first box frame's top-left is caravan-window-local (105 - 2, 88 + 76 - 2) = (103, 162),
// and there are ~70 record px of plate above it - room for two lines without covering anything.
// The label is anchored on the CAPTURED ReagentWindow's own live rect and on the measured UI
// scale, never on a seed or a default: a wrong anchor puts it in the screen's top-left corner,
// so with an unknown scale it is not drawn at all (drawLiveLabel).
//
// `plateWindowRect()` returns the window's rect in SCREEN pixels: the origin is the parent
// origin the engine itself adds to every box's local position (screen =
// [w+0x80]/[w+0x84] + [w+0x6C]/[w+0x70], computed on demand by exe 0x1EE840), and the size is
// the plate's frozen draw rect (window+0x4C0/+0x4C4).  Everything below is record space
// WINDOW-LOCAL, scaled by the measured `g_uiScale` ut_live.cpp reads off the filler boxes:
//   uniq-groups.txt origin (105, 76) = the first box, window-local record px
//   -> the first box FRAME's top-left is (103, 74), so the label block ends at 72.
// ONE line, in the SECOND joint band: a rectangle free on both plates at y 8..24 (the vanilla
// plate's central medallion owns x 248..279 there, and nothing else); the label is left-anchored
// at x 105 and never reaches it. One line is what leaves room for the three pad rows at y 25, 40
// and 55 (bottom 68) - a second line would collide with row 1.
const int kLabelX = 105;         // record px, reagent-window-local
const int kLabelTop = 9;         // record px, the label's TOP
const int kLabelBandY0 = 8;      // the band the label may occupy, measured on BOTH plates
const int kLabelBandY1 = 24;
// The plate interior the label may use, in record px: the same joint rectangle the buttons use.
// The drawn string is clamped by WIDTH, never by a character count.
const int kLabelMaxX = 422;

// The anchor is passed in: route 2 (PresentSurface) supplies plateWindowRect()'s origin, route 1
// (the ReagentWindow::Draw detour) supplies the origin the engine's own Draw computed for this
// very frame.  Returns true when something was actually painted.  The label describes what is
// on screen and nothing else - it is not a hover line, and it has nothing to say on the VANILLA
// page (there is no collection view there), so it is not drawn there at all.
bool drawLiveLabel(GdCanvas* canvas, float wx, float wy) {
    if (!g_cfg.plateLabel) return false;
    if (!plateMaterialsVisible()) return false;   // Materials is not the page on screen

    // NEVER 1.0: an unknown scale would put the label in the screen's top-left corner, so it is
    // skipped for those few frames exactly the way the pad is.
    const float s = liveUiScale();
    if (!(s >= 0.2f && s <= 8.0f)) return false;
    int fs = iround((float)g_cfg.plateLabelSize * s);
    if (fs < 9) fs = 9;
    // Refusal 9: skip the LABEL (the buttons still draw) rather than let it leave its band and
    // touch the vanilla plate's frame above, or the first button row below. The top is a
    // compile-time constant, so that half is a static_assert; the BOTTOM depends on the font
    // size the ini asks for and on the UI scale, so it is a real test - at plate_label_size 16
    // the label would reach record y 25 and it is dropped instead.
    static_assert(kLabelTop >= kLabelBandY0, "the label must start inside the free text band");
    if ((float)kLabelTop + (float)fs / s > (float)kLabelBandY1) return false;

    LiveView v;
    if (!liveViewInfo(&v)) return false;   // the vanilla page (group -1) has nothing to describe

    // The budget is a WIDTH in screen px measured with the engine's own pen rule (textWidthPx),
    // never a character count.  The clip limit is 422 over one of the mod's own plates, and 245
    // when a group is drawn over the GAME's plate (plate_swap=0, or the loose-file plates never
    // installed), whose central medallion owns record x 248..279 in this very band.
    const int limitX = plateModPlateShown() ? kLabelMaxX : 245;
    const int limitPx = iround((float)(limitX - kLabelX) * s);
    if (limitPx < 8) return false;

    // ONE format, in every case - no marker for the owned-only filter and no sentence for an
    // empty group, because either would push the counts the label exists for off the end of the
    // line.  With the owned-only filter on the row numbers are filtered rows, and a group where
    // nothing is owned reads `<group> - 0/N owned - row 0-0/0 - coll c/t`: `liveViewInfo`
    // publishes shown=0 for that layout, and its totalRows/firstRow/lastRow are then a real 0
    // apiece, never -1 and never a stale row from the layout before.
    //
    // The line is measured with the engine's own pen (textWidthPx over the generated
    // ut_fontmetrics.h) and clipped by WIDTH, never by a character budget.  A count the mod does
    // not have - recountOwned could not walk the map (an old uniq-groups.txt with no item keys,
    // or an unreadable map) - prints as `?` in the same slots, so the shape stays identical.
    char ownS[16], allS[16];
    if (v.owned >= 0) {
        _snprintf_s(ownS, sizeof(ownS), _TRUNCATE, "%d", v.owned);
    } else {
        strcpy_s(ownS, sizeof(ownS), "?");
    }
    if (v.ownedAll >= 0) {
        _snprintf_s(allS, sizeof(allS), _TRUNCATE, "%d", v.ownedAll);
    } else {
        strcpy_s(allS, sizeof(allS), "?");
    }
    // The one thing that may go in FRONT of the group name, and only while the mod does not know
    // whether this character is hardcore or softcore. It goes first on purpose: the concessions
    // below eat the END of the line, and this is the one part that must survive them - with no
    // mode there is no collection on screen at all (nothing is painted, every count is `?`) and
    // the label is the only place that says why. It is a WORD, not a new layout: the format
    // string is the same and the prefix is "" in every ordinary session.
    const char* modeS = journalModeKnown() ? "" : "mode unknown - ";
    char line[192];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "%s%s - %s/%d owned - row %d-%d/%d - coll %s/%d",
                modeS, v.label, ownS, v.entries, v.firstRow, v.lastRow, v.totalRows, allS,
                v.collection);
    // The ONE concession before a number is cut: the word " owned" goes.  The catalogue's
    // longest possible line is 57 characters ("Off-hands - 172/172 owned - row 31-35/35 -
    // coll 3288/3288") and at UI scale 1.000 that is 367 screen px against a 317 px band,
    // so something has to give on the widest groups; losing a WORD keeps every count on
    // screen, losing the tail of "coll 3288/3288" does not.
    // Measured against ut_fontmetrics.h, the concession is enough at UI scale 0.928 (368 px ->
    // 282 px against a 294 px band) but NOT at every scale - at 1.000 the shortened line is
    // still 324 px against 317, at 1.500 494 against 476 and at 0.700 225 against 222, so on the
    // three or four widest groups textClipToWidth still eats the tail of "coll 3288/3288" there.
    // A SECOND concession (dropping " - coll c/t", or the row range) would fix that; the format
    // is deliberately kept as it is.
    if (textWidthPx(line, fs) > limitPx) {
        char alt[192];
        _snprintf_s(alt, sizeof(alt), _TRUNCATE, "%s%s - %s/%d - row %d-%d/%d - coll %s/%d", modeS,
                    v.label, ownS, v.entries, v.firstRow, v.lastRow, v.totalRows, allS,
                    v.collection);
        if (textWidthPx(alt, fs) <= limitPx) strcpy_s(line, sizeof(line), alt);
    }
    // Last resort, and only when the PIXELS have really run out.
    textClipToWidth(line, fs, limitPx);
    // A cut inside " - " leaves a dangling separator; trim it rather than show one.
    for (int n = (int)strlen(line); n > 0 && (line[n - 1] == ' ' || line[n - 1] == '-'); --n) {
        line[n - 1] = 0;
    }
    if (!line[0]) return false;

    const int x = iround(wx + (float)kLabelX * s);
    const int y = iround(wy + (float)kLabelTop * s);
    const GdColor head = {0.86f, 0.78f, 0.58f, 1.0f};   // the caravan's own parchment gold
    if (!drawText(canvas, x, y, fs, head, line, g_fontHeader)) return false;

    static int lastGroup = -99;
    static int lastRow = -99;
    if (lastGroup != v.group || lastRow != v.firstRow) {
        lastGroup = v.group;
        lastRow = v.firstRow;
        logT("plate label: \"%s\" at (%d,%d) uiScale=%.3f fs=%d width=%d/%d px window "
             "origin=(%.1f,%.1f) route %d",
             line, x, y, s, fs, textWidthPx(line, fs), limitPx, wx, wy,
             plateLabelInDraw() ? 1 : 2);
    }
    return true;
}

// The label has to run even when the overlay is off, so it gets its own tiny entry point; it
// needs only the fonts and the captured window's own rect.
// This is ROUTE 2 only.  When the ReagentWindow::Draw detour is armed the label has already
// been drawn this frame, under the tooltips, and drawing it again here would put a second copy
// back on top of them.  Without that hook the label is drawn here but suppressed while the
// cursor is inside a box's live hit rect - the only situation in which a box tooltip can be up.
// The label's kill switches are owned by the MOD and not by the ini: configReload does
// `g_cfg = parsed;` once a second, so a `plateLabel = 0` written into g_cfg would come straight
// back from the file and a fault (and its `EXCEPTION 0x` line, which the menu test asserts is
// absent) would repeat every frame.
//   g_labelOff       - route 2 faulted: stop drawing the label at all for the session.
//   g_labelDrawFault - route 1 faulted: ut_plate reads it and pins route 2 with drawOff().
volatile LONG g_labelOff = 0;
volatile LONG g_labelDrawFault = 0;

void drawLabelOnly(GdCanvas* canvas) {
    if (!g_cfg.plateLabel) return;
    if (InterlockedCompareExchange(&g_labelOff, 0, 0)) return;
    if (plateLabelInDraw()) return;
    if (!plateMaterialsVisible()) return;
    POINT pt;
    HWND h = GetActiveWindow();
    if (!h) h = GetForegroundWindow();
    if (h && GetCursorPos(&pt) && ScreenToClient(h, &pt) &&
        plateCursorInAnyBox((int)pt.x, (int)pt.y)) {
        return;   // a tooltip may be showing over that box
    }
    float wx = 0.0f, wy = 0.0f, ww = 0.0f, wh = 0.0f;
    if (!plateWindowRect(&wx, &wy, &ww, &wh)) return;   // no captured window, no label
    ensureFonts();
    if (drawLiveLabel(canvas, wx, wy)) plateNoteLabelRoute(2);
}

}  // namespace


GdGameEngine* panelGameEngine() { return g_lastGameEngine; }

namespace {

// SEH cannot share a function with C++ objects that need unwinding, so the guard is its own
// frame and everything it touches is a call.
bool groupLabelGuarded(GdCanvas* canvas, float ox, float oy) {
    bool drew = false;
    __try {
        drew = drawLiveLabel(canvas, ox, oy);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // This handler is INSIDE ut_plate's drawLabelGuarded, so it consumes the exception
        // before that outer __except can see it; the flag is how ut_plate learns of the fault,
        // turns it into drawOff() and lets route 2 take over for the session.
        if (!InterlockedExchange(&g_labelDrawFault, 1)) {
            logW("FAULT 0x%08lX drawing the group label from ReagentWindow::Draw - it is drawn "
                 "from PresentSurface for the rest of this session", GetExceptionCode());
        }
    }
    return drew;
}

}  // namespace

// Route 1: called from ut_plate.cpp's ReagentWindow::Draw detour, after the original returned.
// It draws the label and nothing else - no engine memory is written.
// ut_plate asks after every route-1 frame whether the label body faulted, because
// groupLabelGuarded's own __except consumes the exception first. One-shot: reading it clears it.
bool panelLabelDrawFaulted() { return InterlockedExchange(&g_labelDrawFault, 0) != 0; }

bool panelDrawGroupLabel(GdCanvas* canvas, float originX, float originY) {
    if (!canvas || !g_cfg.plateLabel) return false;
    if (InterlockedCompareExchange(&g_labelOff, 0, 0)) return false;
    ensureFonts();
    return groupLabelGuarded(canvas, originX, originY);
}

void panelDraw(GdCanvas* canvas, GdGameEngine* gameEngine) {
    if (!canvas) return;
    g_lastGameEngine = gameEngine;
    // The group label is NOT part of the overlay and must run with `enabled=0`. Its own guard,
    // so a fault there can never take the rest of the frame down.
    __try {
        drawLabelOnly(canvas);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Latched in a mod-owned flag, not in g_cfg - configReload rewrites the whole struct
        // from the ini once a second, so a `plateLabel = 0` there would come back as 1 and the
        // fault (and its `EXCEPTION 0x` line) would repeat on every frame.
        if (!InterlockedExchange(&g_labelOff, 1)) {
            logW("FAULT 0x%08lX drawing the group label - the label is OFF for this session; set "
                 "plate_label=0 to keep it off across restarts", GetExceptionCode());
        }
    }
}

// =============================================================================================
// public: diagnostics
// =============================================================================================

const char* panelStatus() {
    int ownedDistinct = 0, unknownDistinct = 0, live = 0;
    {
        std::lock_guard<std::mutex> lock(g_modelMx);
        ownedDistinct = g_own.ownedCount();
        unknownDistinct = (int)g_own.distinctUnknownRecords();
        live = (int)g_liveBitmaps.size();
    }
    _snprintf_s(g_status, sizeof(g_status), _TRUNCATE,
                "cat=%s add=%lld rem=%lld matched=%d unknown=%d route(obj=%lld rep=%lld none=%lld)"
                " live=%d",
                g_initialised ? "ok" : "none", (long long)g_addEvents, (long long)g_removeEvents,
                ownedDistinct, unknownDistinct, (long long)g_nameRouteObject,
                (long long)g_nameRouteReplica, (long long)g_nameRouteNone, live);
    return g_status;
}

}  // namespace ut
