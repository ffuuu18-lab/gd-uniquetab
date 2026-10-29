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
#include "ut_padmat.h"   // the band and the materials-page rule, tested offline
#include "ut_reagent.h"
#include "ut_rescue.h"   // journalModeKnown(): the label says when the mode is not known
#include "ut_fontmetrics.h"  // savapromedium's own glyph metrics (generated)
#include "ut_layout.h"       // the pen rule, the pad and label fits, the layout in force
#include "ut_layoutedit.h"   // the layout editor: handles, drags, its state machine
#include "ut_padpage.h"      // the pad for a group table of any size
#include "ut_screendraw.h"   // the screen pad's frame and the rectangles its input reads
#include "ut_generate.h"     // utViewFile: the active view's catalogue.bin
#include "ut_bindings.h"     // the text engine: wide draw, language font, live metrics
#include "ut_textmeasure.h"  // the pen rule over the live font, UTF-8 for the wide call
#include "ut_sfnt.h"         // the FNTD copies' TrueType cmaps (the font follows the letters)
#include "ut_textcfg.h"      // the words: the label templates and the fallback chain
#include "ut_buttons.h"      // the buttons file uniquetab-buttons.ini
#include "ut_folders.h"      // a custom game's folder, mods\<key>
#include "gen/ut_groups_default.h"   // the built-in group ids (an id any game may lack)
#include "ut_tooltip.h"      // the rollover lines follow the strings file
#include "gen/arc_reader.h"   // the [style] font keys checked against the game's Fonts.arc

namespace ut {
namespace {

// =============================================================================================
// state
// =============================================================================================

std::mutex g_modelMx;      // guards g_own, g_unknownSamples, g_liveBitmaps, g_liveIds
// The active view's catalogue. Replaced by panelReloadCatalogue on a view change and never freed:
// the game thread reads it without the lock, and a view change happens a few times a session.
gdut::Catalogue* volatile g_catP = nullptr;
gdut::Collection g_own;
std::unordered_map<unsigned int, std::string> g_liveIds;  // live item id -> record, for a recount
volatile LONG g_notInDb = -1;   // owned journal rows the active catalogue lacks; -1 = not counted
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

// Fonts named by the game's own text styles (records/ui/styles/text/*.dbr), loaded by their ROOT
// path in every game language (English by default; only a [style] font key changes a role's font).
const GdFont* g_fontTitle = nullptr;    // style_windowtitle01  : nevisnooutlinespaced, 20
const GdFont* g_fontHeader = nullptr;   // style_rollover_title : savapromedium, 18
const GdFont* g_fontBody = nullptr;     // style_textwhite_sizen: jura, 18
// The game language's copy of savapromedium (StyleManager::LoadFontDirect), never drawn by the
// mod: the engine draws its rollover lines in it, so the glyph check of tooltip.yes/no asks it.
// On an English game it is g_fontHeader itself.
const GdFont* g_fontRollover = nullptr;
bool g_fontsTried = false;
volatile LONG g_fontTid = 0;   // the thread ensureFonts loaded this world's fonts on (lazy fills)

// The live measure: g_fontHeader's styles and ASCII glyphs, filled once per world by ensureFonts
// and used by textWidthPx only while the cross-check against the compiled table agreed.
UtFontCache g_liveHeader;
const GdFont* volatile g_liveFont = nullptr;   // the font g_liveHeader was filled from
volatile bool g_liveMeasure = false;
volatile LONG g_measureGen = 0;                // bumps when textWidthPx's route may have changed
bool g_textSaid = false;                       // the language line, once per process

// The [style] font keys of the strings file: the pad's captions (font.caption) and the label line
// (font.label) each in a font of their own, loaded by its exact path once per world and when the
// key changes, measured by its own cache. A role without one draws g_fontHeader and measures with
// textWidthPx. Game thread (roleFontsSync).
struct RoleFont {
    std::string path;              // the key this world was synced to ("" = none)
    const GdFont* font = nullptr;  // in force while `on`
    UtFontCache cache;             // lazy (utTmProbePlan) unless the font is known to be baked
    int kind = -1;                 // 1 = an FNTD font (GDI on demand), 0 = FNTX, -1 = not known
    volatile bool on = false;
    std::string said;              // the last INFO/WARN for this role (one line per change)
};
RoleFont g_role[utt::kStyleFontCount];
long g_roleFileGen = -1;

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

// `s` without the game's colour codes ({^X} and ^X - the generator's stripColourCodes rule).
void stripColourCodesTo(const char* s, char* out, size_t cap) {
    if (!out || !cap) return;
    size_t o = 0;
    for (size_t i = 0; s && s[i] && o + 1 < cap; ++i) {
        if (s[i] == '{' && s[i + 1] == '^' && s[i + 2] && s[i + 3] == '}') {
            i += 3;
            continue;
        }
        if (s[i] == '^' && s[i + 1]) {
            ++i;
            continue;
        }
        out[o++] = s[i];
    }
    out[o] = 0;
}

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

// RenderText2d(int x, int y, Color const&, unsigned short const*, GraphicsFont2 const*, int size,
// xAlign, yAlign, styleFlag, layout) - the char* call's wide twin (ut_bindings.h, text.renderWide).
typedef void(__cdecl* PfnCanvasRenderText2dW)(GdCanvas*, int, int, const GdColor*,
                                              const unsigned short*, const GdFont*, int, int, int,
                                              int, int);

// `s` is UTF-8 and is drawn through the wide call, so any character the font has shows; ASCII
// reaches the engine as the same code units the char* call produced. Without the wide export the
// char* call draws it as before. Returns whether it drew, so the pad can count the captions that
// really landed.
bool drawText(GdCanvas* c, int x, int y, int size, const GdColor& col, const char* s,
              const GdFont* font) {
    if (!font) font = g_fontBody;
    if (!font || !s || !*s || size <= 0) return false;
    const UtTextApi& tx = bindingsText();
    if (tx.draw) {
        unsigned short wide[512];
        if (utWidenUtf8(s, wide, sizeof(wide) / sizeof(wide[0])) == 0) return false;
        ((PfnCanvasRenderText2dW)tx.renderText2dW)(c, x, y, &col, wide, font, size, 0, 0, 0, 0);
        return true;
    }
    if (!g_gd.CanvasRenderText2d) return false;
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

// The pen rule itself is ut_layout.h's utTextWidthPx / utFontStyleFor (Engine.dll 0xAE550 picks
// the baked size nearest the requested one); the pad fitter measures with the same code.
//
// With the live metrics (ut_textmeasure.h) the same rule runs over the font the engine really
// loaded - the root font, a mod's override of it, a [style] font key's font - once ensureFonts has
// proved, for this world, that the live route reproduces the table on the root font. A glyph outside printable ASCII is asked of
// the live font (this thread draws with it); one the font lacks is measured as '?'.
bool buttonFontKnown(const void* font, bool* lazy);
bool liveExtraGlyph(const void* style, char32_t cp, UtGlyph* out, void* font) {
    if (!font) return false;
    bool known = font == (const void*)g_fontHeader, lazy = false;
    for (int r = 0; !known && r < utt::kStyleFontCount; ++r) {
        known = g_role[r].on && font == (const void*)g_role[r].font;
        lazy = known && g_role[r].cache.lazy;
    }
    if (!known) known = buttonFontKnown(font, &lazy);   // a button's own font = (its letters measure too)
    if (!known) return false;
    // A dynamic (FNTD) font rasterises a letter it has not drawn yet: only on the fonts' own thread.
    if (lazy && GetCurrentThreadId() != (DWORD)InterlockedCompareExchange(&g_fontTid, 0, 0)) return false;
    return bindingsTextGlyph(font, style, (unsigned)cp, &out->xOff, &out->rb, &out->w);
}

int textWidthPx(const char* s, int size) {
    if (g_liveMeasure) {
        const int w = utFontCacheWidth(g_liveHeader, s, size, liveExtraGlyph, (void*)g_liveFont);
        if (w >= 0) return w;
    }
    return utTextWidthPx(s, size);
}

// A role's font and width: its own font while one is in force for this world, else the header
// font and textWidthPx - so with no font key every number is today's.
bool roleOn(int r) { return g_fontsTried && g_role[r].on && g_role[r].font; }
const GdFont* roleFont(int r) { return roleOn(r) ? g_role[r].font : g_fontHeader; }
bool fontCacheSizeSeh(UtFontCache* c, const GdFont* f, int size);
int roleTextPx(int r, const char* s, int size) {
    if (roleOn(r)) {
        RoleFont& rf = g_role[r];
        if (rf.cache.lazy && s && *s && size >= 1 && size <= kUtTmMaxSize && rf.cache.sizeStyle[size] == -1)
            fontCacheSizeSeh(&rf.cache, rf.font, size);   // a dynamic font: this size, once
        const int w = utFontCacheWidth(rf.cache, s, size, liveExtraGlyph, (void*)rf.font);
        if (w >= 0) return w;
    }
    return textWidthPx(s, size);
}
const GdFont* captionFont() { return roleFont(utt::kStyleFontCaption); }
const GdFont* labelFont() { return roleFont(utt::kStyleFontLabel); }
int captionTextPx(const char* s, int size) { return roleTextPx(utt::kStyleFontCaption, s, size); }
int labelTextPx(const char* s, int size) { return roleTextPx(utt::kStyleFontLabel, s, size); }

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

bool liveStyleOf(int size, const void** key, int* styleSize, void* font) {
    return bindingsTextStyle(font, size, key, styleSize);
}
bool liveGlyphOf(const void* style, char32_t cp, UtGlyph* out, void* font) {
    return bindingsTextGlyph(font, style, (unsigned)cp, &out->xOff, &out->rb, &out->w);
}

// A role or button font's cache by the live probe's plan (utTmProbePlan: every size of a baked
// font; only the nominal size of a dynamic one), SEH-guarded as a whole - a fault leaves the
// cache empty, so the role keeps the root font with its one WARN. GAME THREAD.
bool fontCacheFillSeh(UtFontCache* c, const GdFont* f, UtTmProbePlan plan) {
    const UtFontProbe p = {liveStyleOf, liveGlyphOf, (void*)f};
    __try {
        return utFontCacheFillPlan(c, p, plan) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        utFontCacheReset(c);
        return false;
    }
}

// One more size into a LAZY cache, the first time the pad or the label measures at it - only on
// the thread that loaded the fonts this world (the one that draws them: a dynamic font rasterises
// the style's glyphs now, as the engine's own draw would); any other thread measures as before.
bool fontCacheSizeSeh(UtFontCache* c, const GdFont* f, int size) {
    if (!c || !f || !c->lazy || size < 1 || size > kUtTmMaxSize || c->sizeStyle[size] != -1) return false;
    if (GetCurrentThreadId() != (DWORD)InterlockedCompareExchange(&g_fontTid, 0, 0)) return false;
    const UtFontProbe p = {liveStyleOf, liveGlyphOf, (void*)f};
    __try {
        return utFontCacheFillSize(c, p, size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        c->sizeStyle[size] = -2;
        return false;
    }
}

// The size a role font is proved at when the world starts (the probe plan's nominal size): the
// caption size cap in force, the label size in force.
int roleNominalSize(int r) {
    return r == utt::kStyleFontCaption ? panelCaptionMaxInForce() : panelLabelSizeInForce();
}

// =============================================================================================
// THE BUTTONS FILE - uniquetab-buttons.ini, one per game (ut_buttons.h)
// =============================================================================================
// The main campaign's beside uniquetab.ini (vanilla and basemods), a custom game's in
// mods\<name>\ (custom-x and basemods+custom-x share it). Written POPULATED the first time
// the game thread has resolved the tab's words for that game's group table (the first file written
// also folds an old uniquetab-text.ini in, which is then renamed .old). Polled once a second by
// the worker (panelTextFileTick - a stat; a read, a parse and a resolve against the game's groups
// only after a change of the file, of the game or of its group table), published by pointer swap:
// never parsed per frame. A refused file is one WARN naming its line; the buttons in force stay
// while they were this game's, else the built-in ones (one button per group). [unassigned] is
// rewritten in place by the worker whenever the set differs. What is drawn is resolved on the
// game thread after ensureFonts, once per world and again when the file or the group table
// changes (TextWorld): captions, names and words checked against the font that draws them, the
// [tab] font keys (or the font the letters derive) per role. Neither object is ever freed -
// other threads read them unlocked, and they change a few times a session.

// The group table as the game thread last resolved it, and the game it belongs to; the worker
// writes the default file and rewrites [unassigned] from it. Never freed.
struct TableSnap {
    long gen = 0;
    std::string customKey;                 // "" = the main campaign's file
    std::vector<utb::UtbGroupIn> groups;
};
const TableSnap* volatile g_tableSnap = nullptr;

struct TextFileState {
    utt::UtTextFile file;                  // [tab] as the old [text] + [style]: the chain, sizes, fonts
    utb::UtbFile buttons;                  // the parsed file
    utb::UtbSet set;                       // resolved against tableGen (ok)
    long tableGen = 0;
    bool ok = false;
    std::string path, customKey;
    int fontInArc[utt::kStyleFontCount] = {-1, -1};   // the font keys in the game's Fonts.arc: 1/0/-1 unknown
    bool derived[utt::kStyleFontCount] = {false, false};   // the role's font follows its letters
    // Per font path the file draws in (the role keys, derived or not, and the buttons' font =
    // lines): 1 = an FNTD font (rasterised through GDI at run time), 0 = FNTX, as read by the worker
    // from the head of its Fonts.arc entry. A path not listed is not known (-1).
    std::vector<std::pair<std::string, int>> fontKinds;
};
TextFileState* volatile g_textFile = nullptr;   // nullptr = no file (the built-in buttons)
volatile LONG g_textFileGen = 0;
SRWLOCK g_textPollLock = SRWLOCK_INIT;
utt::UtFileStamp g_textStamp;                   // under g_textPollLock
ULONGLONG g_textPollAt = 0;                     // under g_textPollLock
std::string g_textPath;                         // under g_textPollLock: the file g_textStamp is of
long g_textSnapGen = -1;                        // under g_textPollLock: the table last resolved against
bool g_textFoldTried = false;                   // under g_textPollLock: the old text file, once

struct TextWorld {
    long gen;                         // CapTable keys on it (unchanged captions keep the number)
    long fileGen, tableGen;
    int groups;                       // the group table's size (or the 24 built-in)
    int buttons;                      // the pad's buttons between MAT and OWN
    std::vector<std::string> caps;    // MAT, the buttons, OWN, NEXT
    std::vector<std::string> names;   // per GROUP: its button's name (or its own label)
    std::vector<int> buttonOf;        // per group: its button (0-based), -1 = on none
    std::vector<std::vector<int>> groupsOf;   // per button: its groups, in group order
    std::vector<int> sizes;           // per logical button: its own size cap, -1 = none
    std::vector<std::string> fonts;   // per logical button: its own font, "" = the caption role's
    std::vector<std::string> formats;
    std::string modeUnknown, unknown;
};
TextWorld* volatile g_textWorld = nullptr;
volatile LONG g_textWorldGen = 0;
std::string g_textWarned;   // game thread: what the last WARN named

// The TextWorld when it was resolved for this very group table, else nullptr.
const TextWorld* textWorldFor(long tableGen, int groups) {
    const TextWorld* w = g_textWorld;
    const int want = groups > 0 ? groups : kUtButtonCount - 2;
    if (!w || w->tableGen != tableGen || w->groups != want || (int)w->caps.size() != w->buttons + 4)
        return nullptr;
    return w;
}
const TextWorld* textWorldNow() {
    const long gen = liveGroupTableGen();
    return textWorldFor(gen, gen > 0 ? liveGroupCount() : 0);
}

const utt::UtTextStyle* textStyle() {
    const TextFileState* f = g_textFile;
    return f ? &f->file.style : nullptr;
}

// A [tab] colour when the file sets it, else the design's float constant.
GdColor textColorOr(int which, const GdColor& dflt) {
    const utt::UtTextStyle* st = textStyle();
    if (!st || which < 0 || which >= utt::kStyleColorCount || st->color[which] < 0) return dflt;
    const long rgb = st->color[which];
    const GdColor c = {(float)((rgb >> 16) & 0xFF) / 255.0f, (float)((rgb >> 8) & 0xFF) / 255.0f,
                       (float)(rgb & 0xFF) / 255.0f, dflt.a};
    return c;
}

bool readTextFileBytes(const char* path, std::string* out, std::string* why) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        *why = "it cannot be opened";
        return false;
    }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart > 256 * 1024) {
        CloseHandle(h);
        *why = "it is larger than 256 KB";
        return false;
    }
    out->assign((size_t)sz.QuadPart, '\0');
    DWORD got = 0;
    const bool ok = out->empty() ||
                    (ReadFile(h, &(*out)[0], (DWORD)out->size(), &got, nullptr) && got == out->size());
    CloseHandle(h);
    if (!ok) *why = "it could not be read";
    return ok;
}

// Whole and atomically (a temporary file, then a replacing rename).
bool writeTextFileBytes(const std::string& path, const std::string& bytes, std::string* why) {
    const std::string tmp = path + ".tmp";
    HANDLE h = CreateFileA(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        *why = "cannot create " + tmp;
        return false;
    }
    DWORD put = 0;
    const bool ok = bytes.empty() || (WriteFile(h, bytes.data(), (DWORD)bytes.size(), &put, nullptr) &&
                                      put == bytes.size());
    CloseHandle(h);
    if (!ok || !MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileA(tmp.c_str());
        *why = "cannot write " + path;
        return false;
    }
    return true;
}

utt::UtFileStamp statFile(const std::string& path) {
    utt::UtFileStamp st;
    st.known = true;
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &a) &&
        !(a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        st.exists = true;
        st.mtime = ((unsigned long long)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
        st.size = ((unsigned long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    }
    return st;
}

// The game in force and its file: the main one, or mods\<key>\uniquetab-buttons.ini.
std::string activeCustomKey() {
    char view[192] = {0};
    configActiveView(view, sizeof(view));
    return configCustomKey(view);
}
std::string buttonsPathFor(const std::string& customKey) {
    char path[MAX_PATH];
    const std::string game = utCustomGameRelLeaf(customKey);   // mods\<key> (ut_folders.h)
    const std::string leaf = game.empty() ? std::string("uniquetab-buttons.ini")
                                          : game + "\\uniquetab-buttons.ini";
    utModPathA(nullptr, leaf.c_str(), path, sizeof(path));
    return path;
}

// The ids no game is wrong to name although it may not have them (a built-in group that received
// no record there, Other): such an id on a button holds nothing in that game. `retired` is the
// group older builds had; a buttons file that still names it loads, and it holds nothing.
const std::vector<std::string>& knownIds() {
    static const std::vector<std::string> ids = [] {
        std::vector<std::string> v;
        try {
            std::string why;
            if (const utg::GroupSet* gs = utg::builtinGroupSet(&why))
                for (const utg::GroupDef& g : gs->groups) v.push_back(g.id);
        } catch (...) {
        }
        v.push_back("other");
        v.push_back("retired");
        return v;
    }();
    return ids;
}

// The glyph tables of the game's copies of savapromedium (the root one, then the folders of
// utt::utTextFontFolders - FNTX, baked - then the FNTD copies of utb::utbDynamicFolderList, whose
// TrueType cmaps say which letters the font itself has; Windows draws the rest), read out of
// resources\Fonts.arc ONCE per process on the worker - only when a file has a letter outside
// printable ASCII for a role without a font line - so each role's font can follow its letters
// (utb::utbDeriveFolder) without asking the engine.
struct FontGlyphSets {
    bool ok = false;
    bool fntd = false;                            // the FNTD copies' cmaps were read (zh at least)
    std::string why;
    std::vector<std::string> folder;              // "" (the root font), the FNTX folders, the FNTD ones
    std::vector<std::vector<char32_t>> glyphs;    // per folder, sorted; empty = not read
    std::vector<int> kind;                        // per folder: 0 FNTX, 1 FNTD, -1 not read
};
const FontGlyphSets* volatile g_glyphSets = nullptr;

bool glyphSetCovers(const std::string& folder, char32_t cp, const void* ctx) {
    const FontGlyphSets* g = (const FontGlyphSets*)ctx;
    for (size_t i = 0; g && i < g->folder.size(); ++i) {
        if (g->folder[i] != folder) continue;
        return utCodePointIn(g->glyphs[i], cp);
    }
    return false;
}

// Under g_textPollLock (worker): the glyph tables, the first time a file needs them.
const FontGlyphSets* glyphSetsLoad() {
    if (g_glyphSets) return g_glyphSets;
    FontGlyphSets* g = new FontGlyphSets();
    char game[MAX_PATH];
    gen::ArcSet arc;
    std::string err;
    std::string dynSaid;
    if (!utGameDirA(game, sizeof(game)) || !arc.load(game, "resources\\Fonts.arc", &err)) {
        g->why = err.empty() ? "no game folder" : err;
    } else {
        std::vector<std::string> folders(1, std::string());
        for (const std::string& x : utb::utbFontFolderList()) folders.push_back(x);
        for (const std::string& fo : folders) {
            std::vector<std::uint8_t> d;
            std::vector<char32_t> gl;
            if (!arc.read((fo.empty() ? std::string() : fo + "/") + "savapromedium.fnt", d) ||
                !utFntxGlyphs(d.data(), d.size(), 13, &gl))
                gl.clear();
            g->folder.push_back(fo);
            g->kind.push_back(gl.empty() ? -1 : 0);
            g->glyphs.push_back(gl);
        }
        g->ok = !g->glyphs.empty() && !g->glyphs[0].empty();
        if (!g->ok) g->why = "the root savapromedium.fnt has no glyph table";
        // The FNTD copies (a few MB each, read once and dropped): only their cmaps are kept.
        for (const std::string& fo : utb::utbDynamicFolderList()) {
            std::vector<std::uint8_t> d;
            std::vector<char32_t> cm;
            if (!arc.read(fo + "/savapromedium.fnt", d) || !utFntdIs(d.data(), d.size()) ||
                !utSfntCodePoints(d.data(), d.size(), &cm))
                cm.clear();
            g->folder.push_back(fo);
            g->kind.push_back(cm.empty() ? -1 : 1);
            dynSaid += (dynSaid.empty() ? "" : ", ") + fo + " " + std::to_string(cm.size());
            g->glyphs.push_back(std::move(cm));
            if (fo == "zh") g->fntd = !g->glyphs.back().empty();
        }
    }
    if (g->ok) {
        int baked = 0;
        for (size_t i = 0; i < g->kind.size(); ++i) baked += g->kind[i] == 0;
        logD("buttons: the glyph tables of %d copies of savapromedium read from the game's Fonts.arc, and "
             "the code points of its TrueType (FNTD) copies: %s - a role's font follows the letters it "
             "draws%s", baked, dynSaid.c_str(),
             g->fntd ? "" : " (no FNTD copy: letters no baked copy has fall back to the tag)");
    } else {
        logW("buttons: the game's font files could not be read (%s) - the fonts cannot follow the "
             "letters; a font.caption / font.label line under [tab] still works", g->why.c_str());
    }
    g_glyphSets = g;   // never freed
    return g;
}

// 1 = an FNTD font (the engine rasterises it through GDI at run time: the live probe is lazy and
// Windows draws the letters it lacks), 0 = FNTX (baked), -1 = not known: what the worker read for
// `path` - the file's own list (its font lines, the derived copies), else the glyph tables' folder.
// Any thread: both are published once and never freed.
int fontKindOf(const std::string& path) {
    if (path.empty()) return -1;
    if (const TextFileState* fs = g_textFile)
        for (const auto& kv : fs->fontKinds)
            if (kv.first == path) return kv.second;
    if (const FontGlyphSets* gs = g_glyphSets)
        for (size_t i = 0; i < gs->folder.size(); ++i)
            if (utb::utbFolderFont(gs->folder[i]) == path) return gs->kind[i];
    return -1;
}

// The code points of a font path when it is one of the copies read (else nullptr): an FNTD
// copy's cmap, for the per-world INFO that counts the letters Windows draws.
const std::vector<char32_t>* fontCodePointsOf(const std::string& path) {
    const FontGlyphSets* gs = g_glyphSets;
    for (size_t i = 0; gs && i < gs->folder.size(); ++i)
        if (gs->kind[i] >= 0 && utb::utbFolderFont(gs->folder[i]) == path) return &gs->glyphs[i];
    return nullptr;
}

void fontKindSet(TextFileState* n, const std::string& path, int kind) {
    for (auto& kv : n->fontKinds)
        if (kv.first == path) {
            kv.second = kind;
            return;
        }
    n->fontKinds.emplace_back(path, kind);
}

// Under g_textPollLock. A role without a font line whose strings have letters outside ASCII gets
// the copy of savapromedium those letters derive (the root font while it has them all - English
// is untouched). Written into the role's font key, marked derived. Letters no baked copy has take
// an FNTD copy (zh/ja/ko by their script; zh when no copy has them all, Windows filling what its
// TrueType font lacks) with one INFO line; only without the FNTD copies is that a WARN.
void textFontsDerive(TextFileState* n) {
    static const char* const kRole[utt::kStyleFontCount] = {"the captions", "the label line"};
    for (int r = 0; r < utt::kStyleFontCount; ++r) {
        if (!n->file.style.font[r].empty()) continue;   // a font line wins
        const std::u32string cps = utb::utbRoleCodePoints(n->buttons, (utt::UtTextFont)r);
        if (cps.empty()) continue;
        const FontGlyphSets* gs = glyphSetsLoad();
        if (!gs || !gs->ok) continue;
        const utb::UtbDerived d = utb::utbDeriveFolder(cps, glyphSetCovers, gs, gs->fntd);
        if (d.folder.empty()) continue;   // the root font has them all
        n->file.style.font[r] = utb::utbFolderFont(d.folder);
        n->derived[r] = true;
        n->fontInArc[r] = 1;   // read out of it just now
        fontKindSet(n, n->file.style.font[r], d.dynamic ? 1 : 0);
        if (d.coversAll) {
            logI("buttons: %s draw in %s - their letters need it (no font line)", kRole[r],
                 n->file.style.font[r].c_str());
        } else if (d.dynamic) {
            logI("buttons: %s (no font line)", utb::utbDerivedFallbackText(d, kRole[r]).c_str());
        } else {
            logW("buttons: no font of the game has every letter of %s (%s is missing in the best one, "
                 "%s); a font.%s line under [tab] chooses another", kRole[r],
                 utt::utTextCodePointName(d.missing).c_str(), n->file.style.font[r].c_str(),
                 r == 0 ? "caption" : "label");
        }
    }
}

// Under g_textPollLock (worker). Whether each font line names a font the game has: the game's
// resources\Fonts.arc (and its expansions') index, read once per file load - so the game thread
// never asks the engine for a path that is not there. Unknown (-1) when the index cannot be read.
// Also the KIND of every font the file names (the role lines and the buttons' font = lines): the
// first 8 bytes of its entry, FNTD or not - so the game thread probes a dynamic font lazily.
void textFontsInArc(TextFileState* n) {
    std::vector<std::string> paths;
    for (int i = 0; i < utt::kStyleFontCount; ++i)
        if (!n->file.style.font[i].empty() && !n->derived[i]) paths.push_back(n->file.style.font[i]);
    for (const utb::UtbButton& b : n->buttons.buttons)
        if (!b.font.empty()) paths.push_back(b.font);
    if (paths.empty()) return;
    char game[MAX_PATH];
    gen::ArcSet arc;
    std::string err;
    if (!utGameDirA(game, sizeof(game)) || !arc.load(game, "resources\\Fonts.arc", &err)) {
        logD("buttons: the game's Fonts.arc could not be read (%s) - the font lines are tried as they are",
             err.empty() ? "no game folder" : err.c_str());
        return;
    }
    for (int i = 0; i < utt::kStyleFontCount; ++i) {
        const std::string& p = n->file.style.font[i];
        char entry[128];
        if (p.empty() || n->derived[i]) continue;
        n->fontInArc[i] = utFontArcEntry(p.c_str(), entry, sizeof(entry)) && arc.contains(entry) ? 1 : 0;
    }
    for (const std::string& p : paths) {
        char entry[128];
        std::vector<std::uint8_t> head;
        if (!utFontArcEntry(p.c_str(), entry, sizeof(entry)) || !arc.contains(entry) || !arc.read(entry, head, 8))
            continue;
        fontKindSet(n, p, utFntdIs(head.data(), head.size()) ? 1 : 0);
    }
}

// Under g_textPollLock. The default file for the game of `snap`, written when it has none; the
// first one also folds an old uniquetab-text.ini in (then renamed .old, one INFO line).
void buttonsWriteDefault(const std::string& path, const TableSnap* snap) {
    utb::UtbDefaults d = utb::utbBuiltinDefaults();
    std::string foldLine;
    if (!g_textFoldTried) {
        g_textFoldTried = true;
        char old[MAX_PATH];
        utModPathA(nullptr, "uniquetab-text.ini", old, sizeof(old));
        if (GetFileAttributesA(old) != INVALID_FILE_ATTRIBUTES) {
            std::string bytes, why;
            utt::UtTextFile f;
            std::vector<std::string> notes;
            const bool parsed = readTextFileBytes(old, &bytes, &why) && utt::utTextParse(bytes, f, &why);
            if (parsed) d = utb::utbFoldText(f, snap->groups, &notes);
            std::string to = std::string(old) + ".old";
            for (int i = 1; i <= 9 && GetFileAttributesA(to.c_str()) != INVALID_FILE_ATTRIBUTES; ++i)
                to = std::string(old) + ".old" + std::to_string(i);
            const bool moved = MoveFileExA(old, to.c_str(), 0) != 0;
            std::string left;
            for (const std::string& n : notes) left += (left.empty() ? "" : "; ") + n;
            if (parsed) {
                foldLine = "the words of uniquetab-text.ini (" + std::to_string(f.keys) +
                           " key(s)) are folded into it" + (left.empty() ? "" : " - not carried: " + left);
            } else {
                logW("buttons: uniquetab-text.ini could not be folded in (%s) - the default words are used",
                     why.c_str());
            }
            if (moved) logI("buttons: uniquetab-text.ini is retired - renamed %s", to.c_str());
            else logW("buttons: uniquetab-text.ini could not be renamed to %s - it is not read any more", to.c_str());
        }
    }
    if (!snap->customKey.empty()) {
        char dir[MAX_PATH];
        utModDirA(nullptr, dir, sizeof(dir));
        utMakeRelDirs(dir, utCustomGameRelLeaf(snap->customKey));   // mods\<key>
    }
    std::string why;
    const std::string text = utb::utbCrlf(utb::utbDefaultText(utb::utbViewTitle(snap->customKey), snap->groups, d));
    if (writeTextFileBytes(path, text, &why)) {
        logI("buttons: %s written - %zu button(s), one per group of %s%s%s", path.c_str(), snap->groups.size(),
             utb::utbViewTitle(snap->customKey).c_str(), foldLine.empty() ? "" : "; ", foldLine.c_str());
    } else {
        logW("buttons: the buttons file could not be written (%s) - the built-in buttons are used", why.c_str());
    }
}

// Under g_textPollLock. Reads, parses and resolves the file; publishes it, or keeps the buttons in
// force when they were this game's (a refused file); rewrites [unassigned] when the set differs.
void buttonsLoad(const std::string& path, const std::string& key, bool exists, bool first, const TableSnap* snap) {
    const TextFileState* was = g_textFile;
    const bool keep = was && was->path == path && (!snap || was->tableGen == snap->gen);
    TextFileState* n = nullptr;
    std::string bytes;
    if (exists) {
        std::string why;
        utb::UtbFile f;
        if (!readTextFileBytes(path.c_str(), &bytes, &why) || !utb::utbParse(bytes, f, &why)) {
            logW("buttons: %s is IGNORED - %s; %s", path.c_str(), why.c_str(),
                 keep ? "the buttons in force stay" : "the built-in buttons are used");
            if (keep) return;
        } else {
            n = new TextFileState();
            n->buttons = std::move(f);
            n->file = n->buttons.words;
            n->path = path;
            n->customKey = key;
            if (snap && snap->customKey == key) {
                n->tableGen = snap->gen;
                n->ok = utb::utbResolve(n->buttons, snap->groups, knownIds(), n->set, &why);
                if (!n->ok) {
                    logW("buttons: %s is IGNORED - %s; %s", path.c_str(), why.c_str(),
                         keep ? "the buttons in force stay" : "the built-in buttons are used");
                    delete n;
                    n = nullptr;
                    if (keep) return;
                }
            }
            if (n) {
                textFontsDerive(n);
                textFontsInArc(n);
                if (n->ok) {
                    logI("buttons: %s read - %zu button(s) over %zu group(s), %zu on no button%s%s; [tab] sets %d "
                         "word(s); the pad and the label follow within a second",
                         path.c_str(), n->set.buttons.size(), snap->groups.size(), n->set.unassigned.size(),
                         n->set.dropped ? ", buttons of groups this game does not have: " : "",
                         n->set.dropped ? std::to_string(n->set.dropped).c_str() : "", n->file.keys);
                } else {
                    logD("buttons: %s read - %zu button(s); resolved once the game's groups are known",
                         path.c_str(), n->buttons.buttons.size());
                }
            }
        }
    } else if (!first) {
        logI("buttons: %s is gone - the built-in buttons are used (it is written again with this game's groups)",
             path.c_str());
    }
    if (!was && !n) return;
    bool sizes = true;
    if (was && n) {
        sizes = false;
        for (int i = 0; i < utt::kStyleSizeCount; ++i)
            if (was->file.style.size[i] != n->file.style.size[i]) sizes = true;
    } else {
        const utt::UtTextStyle& s = n ? n->file.style : was->file.style;
        sizes = s.size[0] >= 0 || s.size[1] >= 0 || s.size[2] >= 0;
    }
    g_textFile = n;   // the previous one is never freed
    InterlockedIncrement(&g_textFileGen);
    if (sizes) InterlockedIncrement(&g_measureGen);   // the caption fit and the label size follow
    // A built-in group that came after 1.1.0 (the blueprints) gets its button ONCE in a file written
    // before it existed (utbOfferNew: after the last button, byte-preserving; the "added once" line
    // of [unassigned] remembers it, so a user who deletes the section keeps it deleted). The stamp
    // is left alone: the file is read again within a second, and the [unassigned] rewrite runs then.
    std::string out, why;
    if (n && n->ok && snap) {
        std::vector<std::string> added, refused;
        if (utb::utbOfferNew(bytes, n->buttons, snap->groups, utb::utbLateBuiltinIds(), &out, &added,
                             &refused)) {
            std::string a, r;
            for (const std::string& id : added) a += (a.empty() ? "" : ", ") + id;
            for (const std::string& id : refused) r += (r.empty() ? "" : ", ") + id;
            if (writeTextFileBytes(path, out, &why)) {
                logI("buttons: %s - a new built-in group got its button once: %s%s%s (delete the section to keep "
                     "it off - it then stays under [unassigned])",
                     path.c_str(), a.empty() ? "none" : a.c_str(), r.empty() ? "" : "; no button, its tag is taken: ",
                     r.c_str());
            } else {
                logW("buttons: %s - the new group's button could not be added (%s)", path.c_str(), why.c_str());
            }
            return;
        }
    }
    // [unassigned]: what no button holds, rewritten in place (only that section) when it differs.
    if (n && n->ok && utb::utbRewriteUnassigned(bytes, n->set.unassigned, &out)) {
        if (writeTextFileBytes(path, out, &why)) {
            std::string list;
            for (const std::string& id : n->set.unassigned) list += (list.empty() ? "" : ", ") + id;
            logI("buttons: [unassigned] of %s rewritten - %zu group(s) on no button%s%s", path.c_str(),
                 n->set.unassigned.size(), list.empty() ? "" : ": ", list.c_str());
            g_textStamp = statFile(path);   // our own write is not a change to read again
        } else {
            logW("buttons: [unassigned] of %s could not be rewritten (%s)", path.c_str(), why.c_str());
        }
    }
}

// Once a second at most (the start-up look and the worker's loop): the file of the game in force;
// a read only after a change of the file, of the game or of its group table.
void textFilePoll() {
    if (!TryAcquireSRWLockExclusive(&g_textPollLock)) return;
    const ULONGLONG now = GetTickCount64();
    if (!g_textStamp.known || now - g_textPollAt >= 1000) {
        g_textPollAt = now;
        try {
            const std::string key = activeCustomKey();
            const std::string path = buttonsPathFor(key);
            const TableSnap* snap = g_tableSnap;
            if (snap && snap->customKey != key) snap = nullptr;   // the game thread has not seen this game yet
            utt::UtFileStamp st = statFile(path);
            if (!st.exists && snap) {
                buttonsWriteDefault(path, snap);
                st = statFile(path);
            }
            const bool pathMoved = path != g_textPath;
            const bool snapMoved = snap && snap->gen != g_textSnapGen;
            if (pathMoved || snapMoved || utt::utTextChanged(g_textStamp, st)) {
                const bool first = !g_textStamp.known || pathMoved;
                g_textPath = path;
                g_textStamp = st;
                if (snap) g_textSnapGen = snap->gen;
                buttonsLoad(path, key, st.exists, first, snap);
            }
        } catch (...) {
        }
    }
    ReleaseSRWLockExclusive(&g_textPollLock);
}

// The glyph check, per role over the font that draws it: the caption and the label font (their
// [style] key's font, else g_fontHeader, the root font); the rollover line is drawn by the engine
// in its own font from the game's language folder, so g_fontRollover (that copy) is asked for it.
// Printable ASCII is always there; any other code point only through the wide call, with the live
// metrics proved for this world, and when GetMetric answers for it.
// A role on an FNTD font (RoleFont::kind 1) is not asked: the engine rasterises any UTF-16 letter
// there, drawing the ones its TrueType font lacks with Windows' own fallback font (no WARN for
// them - one INFO per world counts them); only a letter above U+FFFF cannot be drawn.
struct TextGlyphProbe {
    const void* font[3];    // utt::kFontCaption, kFontLabel, kFontTooltip
    const void* style[3];
    bool dynamic[3];
};
bool textGlyph(utt::UtTextFont role, char32_t cp, const void* ctx) {
    if (cp >= 0x20 && cp <= 0x7E) return true;
    const TextGlyphProbe* p = (const TextGlyphProbe*)ctx;
    const int r = (int)role;
    if (!p || r < 0 || r > 2 || !p->font[r]) return false;
    if (p->dynamic[r]) return cp <= 0xFFFF;
    if (!p->style[r]) return false;
    int x = 0, rb = 0, w = 0;
    return bindingsTextGlyph(p->font[r], p->style[r], (unsigned)cp, &x, &rb, &w);
}
bool textLocalize(const char* tag, std::string* out, const void*) {
    char buf[256];
    if (!bindingsTextLocalize(tag, buf, sizeof(buf))) return false;
    *out = buf;
    return true;
}
// The game's own word for a group name - asked by the chain only for a name.<id> / name.* whose
// value is @game (an explicit opt-in; never automatic).
bool textTag(utt::UtTextKind kind, const std::string& id, std::string* out, const void*) {
    return utt::utTextGameWord(kind, id, textLocalize, nullptr, out);
}

struct TextWorld;
void buttonFontsSync(const TextWorld* w, bool newWorld);

// GAME THREAD. One INFO per world (and per change) for each role drawn in an FNTD copy: how many
// of its letters that font lacks - Windows' own fallback font draws them, so the glyph check
// does not WARN about them. Reads the code points the worker published; nothing is loaded here.
std::string g_dynSaid[utt::kStyleFontCount];
void dynamicRolesSay(const TextFileState* fs, bool newWorld) {
    static const char* const kRole[utt::kStyleFontCount] = {"the captions", "the label line"};
    for (int r = 0; r < utt::kStyleFontCount; ++r) {
        if (newWorld) g_dynSaid[r].clear();
        if (!fs || !roleOn(r) || g_role[r].kind != 1) continue;
        const std::vector<char32_t>* cm = fontCodePointsOf(g_role[r].path);
        if (!cm) continue;
        utb::UtbDerived d;
        d.dynamic = true;
        for (char32_t cp : utb::utbRoleCodePoints(fs->buttons, (utt::UtTextFont)r)) {
            if (cp > 0xFFFF || utCodePointIn(*cm, cp)) continue;
            if (!d.lacking) d.missing = cp;
            ++d.lacking;
        }
        d.coversAll = d.lacking == 0;
        const std::string line = utb::utbDerivedFallbackText(d, kRole[r], g_role[r].path);
        if (line.empty() || line == g_dynSaid[r]) continue;
        g_dynSaid[r] = line;
        logI("buttons: %s (this world)", line.c_str());
    }
}

// GAME THREAD ONLY (the glyph check is an engine call). Resolves the buttons - the file's when it
// is this game's and resolves against this table, else one per group - their captions and names,
// the label words and the rollover lines; publishes the table for the worker (the default file,
// [unassigned]); one WARN per world (or per changed file) names what was passed over, one DEBUG
// line says where the strings came from.
void textWorldBuild(bool newWorld) {
    try {
        const TextFileState* fs = g_textFile;
        const long fileGen = InterlockedCompareExchange(&g_textFileGen, 0, 0);
        const long tableGen = liveGroupTableGen();
        const int groups = tableGen > 0 ? liveGroupCount() : 0;
        TextGlyphProbe probe = {};
        if (bindingsText().draw && g_liveMeasure && g_fontHeader) {
            const GdFont* fonts[3] = {captionFont(), labelFont(),
                                      g_fontRollover ? g_fontRollover : g_fontHeader};
            for (int r = 0; r < 3; ++r) {
                int ss = 0;
                // a dynamic role font builds no style for the check (each is a GDI font)
                probe.dynamic[r] = r < utt::kStyleFontCount && roleOn(r) && g_role[r].kind == 1;
                if (probe.dynamic[r]) probe.font[r] = fonts[r];
                else if (fonts[r] && bindingsTextStyle(fonts[r], 13, &probe.style[r], &ss)) probe.font[r] = fonts[r];
            }
        }
        const UtTextLanguage& lang = bindingsTextLanguage();
        const std::string key = activeCustomKey();
        if (fs && fs->customKey != key) fs = nullptr;   // the worker has not read this game's file yet
        utt::UtTextContext c;
        c.file = fs ? &fs->file : nullptr;
        c.language = lang.known ? utt::utTextLower(lang.language) : std::string();
        c.english = !lang.known || lang.english != 0;
        c.glyph = textGlyph;
        c.glyphCtx = &probe;
        std::vector<utb::UtbGroupIn> in;
        if (groups > 0) {
            for (int g = 0; g < groups; ++g) in.push_back({liveGroupId(g), liveGroupTag(g), utb::utbGroupLabel(liveGroupLabel(g))});
            const TableSnap* snap = g_tableSnap;
            if (!snap || snap->gen != tableGen || snap->customKey != key) {
                TableSnap* s = new TableSnap();
                s->gen = tableGen;
                s->customKey = key;
                s->groups = in;
                g_tableSnap = s;   // never freed; the worker writes the default file from it
            }
        } else {
            size_t nb = 0;
            const utt::UtTextBuiltin* b = utt::utTextBuiltins(&nb);
            for (int g = 1; g <= kUtButtonCount - 2 && (size_t)g < nb; ++g)
                in.push_back({b[g].id, b[g].caption, b[g].name});
        }
        utb::UtbSet set;
        bool fromFile = false;
        std::string resolveWhy;
        if (fs && groups > 0) {
            if (fs->ok && fs->tableGen == tableGen) {
                set = fs->set;
                fromFile = true;
            } else {
                fromFile = utb::utbResolve(fs->buttons, in, knownIds(), set, &resolveWhy);
            }
        }
        if (!fromFile) set = utb::utbBuiltinSet(in);
        utb::UtbPad pad = utb::utbResolvePad(c, set, in);
        TextWorld* w = new TextWorld();
        w->fileGen = fileGen;
        w->tableGen = tableGen;
        w->groups = (int)in.size();
        w->buttons = (int)set.buttons.size();
        w->caps = pad.caps;
        w->sizes = pad.sizes;
        w->fonts = pad.fonts;
        w->buttonOf = set.buttonOfGroup;
        for (const utb::UtbResolved& b : set.buttons) w->groupsOf.push_back(b.groups);
        for (size_t g = 0; g < in.size(); ++g) {
            const int b = g < set.buttonOfGroup.size() ? set.buttonOfGroup[g] : -1;
            w->names.push_back(b >= 0 && (size_t)b < pad.names.size() ? pad.names[(size_t)b]
                                                                       : (in[g].label.empty() ? utt::utTextDerivedName(in[g].id) : in[g].label));
        }
        std::string fskip;
        const utt::UtTextSource fsrc = utt::utTextResolveFormats(c, w->formats, &fskip);
        if (!fskip.empty()) pad.skipped.push_back(fskip);
        const utt::UtTextPick mode = utt::utTextResolve(c, utt::kTextModeUnknown, "");
        const utt::UtTextPick unk = utt::utTextResolve(c, utt::kTextUnknown, "");
        const utt::UtTextPick yes = utt::utTextResolve(c, utt::kTextTooltipYes, "");
        const utt::UtTextPick no = utt::utTextResolve(c, utt::kTextTooltipNo, "");
        const utt::UtTextPick roll = utt::utTextResolve(c, utt::kTextTooltipRoll, "");
        for (const utt::UtTextPick* p : {&mode, &unk, &yes, &no, &roll})
            if (!p->skipped.empty()) pad.skipped.push_back(p->skipped);
        w->modeUnknown = mode.text;
        w->unknown = unk.text;
        const TextWorld* old = g_textWorld;
        const bool capsSame = old && old->tableGen == tableGen && old->caps == w->caps &&
                              old->buttonOf == w->buttonOf && old->sizes == w->sizes && old->fonts == w->fonts;
        w->gen = capsSame ? old->gen : (long)InterlockedIncrement(&g_textWorldGen);
        g_textWorld = w;   // the previous one is never freed
        buttonFontsSync(w, newWorld);
        // the caption fit and the layout (whose pad holds w->buttons buttons) follow
        if (!capsSame && (!newWorld || w->buttons != w->groups)) InterlockedIncrement(&g_measureGen);
        tooltipTextRefresh(yes.text.c_str(), yes.source <= utt::kSrcCommon, no.text.c_str(),
                           no.source <= utt::kSrcCommon, roll.text.c_str());

        logD("buttons: the strings for %s (%d groups): %d button(s) %s, %d caption(s) and %d name(s) "
             "from the file, the label templates from %s",
             tableGen > 0 ? "the group table in force" : "the built-in groups", w->groups, w->buttons,
             fromFile ? "from the buttons file" : "built in (one per group)", pad.capFile, pad.nameFile,
             fsrc <= utt::kSrcCommon ? "the buttons file" : utt::utTextSourceName(fsrc));
        std::string warn;
        if (!pad.skipped.empty()) {
            warn = pad.skipped[0];
            if (pad.skipped.size() > 1) warn += " (and " + std::to_string(pad.skipped.size() - 1) + " more)";
        }
        if (newWorld) g_textWarned.clear();
        if (!warn.empty() && warn != g_textWarned) {
            // What to do: only when the check really asked a font (the wide call and the live
            // metrics are on), else every letter outside ASCII is refused whatever the font.
            // The advice reads the glyph tables the worker published (never Fonts.arc here): a
            // letter no baked copy has names the FNTD copy that draws it; a letter above U+FFFF
            // says no font line can draw it (utb::utbFontAdvice, test_buttons).
            const FontGlyphSets* gs = g_glyphSets;
            const bool known = gs && gs->ok;
            const std::string advice =
                probe.font[0] ? utb::utbFontAdvice(pad.skipped, glyphSetCovers, gs, known, known && gs->fntd)
                              : std::string();
            logW("buttons: %s - the next source is used instead (the tag, the group's own name, else the "
                 "built-in English)%s%s",
                 warn.c_str(), advice.empty() ? "" : "; ", advice.c_str());
        }
        g_textWarned = warn;
        dynamicRolesSay(fs, newWorld);
        const auto dup = utt::utTextDuplicates(w->caps);
        if (!dup.empty() && pad.capFile > 0 && !capsSame) {
            logW("buttons: the captions \"%s\" of pad buttons %zu and %zu are the same - both still "
                 "click right, but they cannot be told apart",
                 w->caps[dup[0].first].c_str(), dup[0].first, dup[0].second);
        }
    } catch (...) {
    }
}

// GraphicsEngine::LoadFont on the exact path, SEH-guarded (a font key is the user's text).
const GdFont* loadFontExactSeh(GdGraphicsEngine* gfx, const std::string* path) {
    __try {
        return g_gd.GfxLoadFont(gfx, path);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// GAME THREAD ONLY. The [style] font keys in force: once per world (newWorld, after the live
// metrics were proved on the root font) and whenever the file changes. A role whose key is unset,
// missing, unloadable or not measurable draws g_fontHeader; one line per change says which.
void roleFontsSync(bool newWorld) {
    const long gen = InterlockedCompareExchange(&g_textFileGen, 0, 0);
    if (!newWorld && gen == g_roleFileGen) return;
    g_roleFileGen = gen;
    const TextFileState* fs = g_textFile;
    static const char* const kWhat[utt::kStyleFontCount] = {"the pad's captions are", "the label line is"};
    bool moved = false;
    for (int r = 0; r < utt::kStyleFontCount; ++r) {
        RoleFont& rf = g_role[r];
        const std::string want = fs ? fs->file.style.font[r] : std::string();
        if (!newWorld && want == rf.path) continue;
        const bool was = rf.on;
        rf.on = false;
        rf.font = nullptr;
        rf.kind = -1;
        rf.path = want;
        std::string line;
        bool warn = false;
        if (!want.empty()) {
            const int inArc = fs->fontInArc[r];
            const bool proved = g_liveMeasure && bindingsText().metrics;
            const GdFont* f = nullptr;
            bool valid = false;
            GdGraphicsEngine* gfx = graphicsEngine();
            rf.kind = fontKindOf(want);
            if (inArc != 0 && proved && gfx && g_gd.GfxLoadFont) {
                f = loadFontExactSeh(gfx, &want);
                if (f) valid = fontCacheFillSeh(&rf.cache, f, utTmProbePlan(rf.kind, roleNominalSize(r)));
            }
            const UtRoleFont st = utRoleFontState(true, inArc, proved, f != nullptr, valid);
            if (st == kRoleFontOwn) {
                rf.font = f;
                rf.on = true;
                line = "buttons: [tab] " + std::string(utt::utStyleFontKey(r)) + " = " + want +
                       (fs->derived[r] ? " (what its letters need)" : "") + " is in force - " +
                       kWhat[r] + " drawn and measured in it (" +
                       (rf.cache.lazy ? std::string(rf.kind == 1 ? "a dynamic font" : "a font of unknown kind") +
                                            ": size " + std::to_string(roleNominalSize(r)) +
                                            " now, any other the first time it is drawn"
                                      : std::to_string(rf.cache.nStyles) + " styles") + ")";
            } else {
                warn = true;
                line = "buttons: [tab] " + std::string(utt::utStyleFontKey(r)) + " = " + want + " is not used - " +
                       utRoleFontWhy(st) + "; " + kWhat[r] + " drawn in the game's font";
            }
        } else if (!newWorld && !rf.said.empty()) {
            line = std::string("buttons: [tab] ") + utt::utStyleFontKey(r) + " is unset - " + kWhat[r] +
                   " drawn in the game's font again";
        }
        moved |= was || rf.on;
        if (line.empty()) {
            rf.said.clear();
        } else if (line != rf.said) {
            rf.said = line;
            if (warn) logW("%s", line.c_str());
            else logI("%s", line.c_str());
        } else {
            logD("%s", line.c_str());
        }
    }
    if (moved) InterlockedIncrement(&g_measureGen);   // the caption fit and the label follow
}

// GAME THREAD ONLY. The buttons' own fonts (font = on a [TAG]): up to kBtnFonts distinct paths,
// each loaded by its exact path (GraphicsEngine::LoadFont, as the [tab] font keys) once per world
// and whenever a TextWorld names a new one, measured by its own cache through the live probe the
// per-world cross-check proved. A font that does not load or measure is one WARN; that button then
// keeps the caption font. Read by drawButtonsInner through buttonCaptionFont/buttonCaptionTextPx.
const int kBtnFonts = 8;
struct ButtonFont {
    std::string path;
    const GdFont* font = nullptr;
    UtFontCache cache;
    bool on = false;
};
ButtonFont g_btnFont[kBtnFonts];
std::string g_btnFontSaid;

bool buttonFontKnown(const void* font, bool* lazy) {
    for (int i = 0; i < kBtnFonts; ++i)
        if (g_btnFont[i].on && font == (const void*)g_btnFont[i].font) {
            *lazy = g_btnFont[i].cache.lazy != 0;
            return true;
        }
    return false;
}

void buttonFontsSync(const TextWorld* w, bool newWorld) {
    std::vector<std::string> want;
    for (const std::string& f : w->fonts)
        if (!f.empty() && want.size() < (size_t)kBtnFonts) {
            bool dup = false;
            for (const std::string& x : want) dup |= x == f;
            if (!dup) want.push_back(f);
        }
    bool same = !newWorld;
    for (int i = 0; i < kBtnFonts && same; ++i)
        same = g_btnFont[i].path == (i < (int)want.size() ? want[(size_t)i] : std::string());
    if (same) return;
    std::string bad;
    bool moved = false;
    for (int i = 0; i < kBtnFonts; ++i) {
        ButtonFont& bf = g_btnFont[i];
        moved |= bf.on;
        bf.on = false;
        bf.font = nullptr;
        bf.path = i < (int)want.size() ? want[(size_t)i] : std::string();
        if (bf.path.empty()) continue;
        const bool proved = g_liveMeasure && bindingsText().metrics;
        GdGraphicsEngine* gfx = graphicsEngine();
        const GdFont* f = proved && gfx && g_gd.GfxLoadFont ? loadFontExactSeh(gfx, &bf.path) : nullptr;
        if (f) {
            const int capMax = panelCaptionMaxInForce();
            fontCacheFillSeh(&bf.cache, f, utTmProbePlan(fontKindOf(bf.path), capMax));
            if (bf.cache.valid) {
                bf.font = f;
                bf.on = true;
                moved = true;
                continue;
            }
        }
        bad += (bad.empty() ? "" : ", ") + bf.path;
    }
    const std::string line = bad.empty() ? std::string() : bad;
    if (!bad.empty() && line != g_btnFontSaid)
        logW("buttons: the font = line(s) %s could not be loaded or measured - those buttons keep the "
             "caption font", bad.c_str());
    g_btnFontSaid = line;
    if (moved) InterlockedIncrement(&g_measureGen);
}

int buttonFontOf(int logical) {
    const TextWorld* w = textWorldNow();
    if (!w || logical < 0 || (size_t)logical >= w->fonts.size() || w->fonts[(size_t)logical].empty()) return -1;
    for (int i = 0; i < kBtnFonts; ++i)
        if (g_btnFont[i].on && g_btnFont[i].path == w->fonts[(size_t)logical]) return i;
    return -1;
}

// Every call of ensureFonts after the first in a world: a rebuild when the file or the group
// table moved. Two reads when nothing did.
void textTick() {
    roleFontsSync(false);
    const TextWorld* w = g_textWorld;
    const long tableGen = liveGroupTableGen();
    const int groups = tableGen > 0 ? liveGroupCount() : kUtButtonCount - 2;
    if (!w || w->fileGen != InterlockedCompareExchange(&g_textFileGen, 0, 0) ||
        w->tableGen != tableGen || w->groups != groups) {
        textWorldBuild(false);
    }
}

// The label line's width for the chain (ctx = the font size), in the label's font.
int labelWidthPx(const std::string& s, const void* ctx) {
    return labelTextPx(s.c_str(), *(const int*)ctx);
}
int labelClipWidthPx(const char* s, const void* ctx) { return labelTextPx(s, *(const int*)ctx); }

// Once per world, after the fonts are loaded: the language line (once per process), then the
// cross-check - the live route over the ROOT savapromedium must reproduce the compiled table
// (every style the engine picks for sizes 1..64, every ASCII glyph, the 26 captions and the label
// lines) before textWidthPx measures g_fontHeader with it.
void liveMeasureBegin(GdGraphicsEngine* gfx) {
    g_liveMeasure = false;
    g_liveFont = nullptr;
    const UtTextApi& tx = bindingsText();
    if (!g_textSaid) {
        g_textSaid = true;
        const UtTextLanguage& lang = bindingsTextLanguage();
        char path[96] = "";
        utLocalizedFontPath(kUtRootFontPath, lang.known ? lang.folder : "", path, sizeof(path));
        if (lang.known) {
            logI("text: the game's language is \"%s\" (English: %s), its special font folder "
                 "\"%s\" - the tab draws in %s (the root font) in every language unless "
                 "uniquetab-text.ini's [style] font keys say otherwise; the game's rollover lines "
                 "use %s%s; wide text %s, live metrics %s",
                 lang.language, lang.english ? "yes" : "no", lang.folder, kUtRootFontPath, path,
                 tx.font ? "" : " (LoadFontDirect is OFF: the root font stands in for the glyph check)",
                 tx.draw ? "on" : "OFF", tx.metrics ? "bound" : "OFF");
        } else {
            logI("text: the game's language could not be read (localization exports %s) - the tab "
                 "draws in %s (the root font); wide text %s, live metrics %s",
                 tx.loc ? "bound, the read failed" : "OFF", kUtRootFontPath, tx.draw ? "on" : "OFF",
                 tx.metrics ? "bound" : "OFF");
        }
        // One tag through the game's own localisation, so the log shows that route works (the
        // game's words are used only where uniquetab-text.ini says @game).
        char word[128];
        if (bindingsTextLocalize("tagCraftingHelms", word, sizeof(word))) {
            logD("text: the game's own word for tagCraftingHelms is \"%s\"", word);
        } else if (tx.loc) {
            logD("text: tagCraftingHelms could not be localized (not in the map, or empty)");
        }
    }
    if (!g_fontHeader || !g_gd.GfxLoadFont || !tx.metrics) return;
    const std::string rp(kUtRootFontPath);
    const GdFont* rootFont = g_gd.GfxLoadFont(gfx, &rp);
    if (!rootFont) return;
    static UtFontCache table, root;
    static bool tableDone = false;
    if (!tableDone) {
        utFontCacheFromTable(&table);
        tableDone = true;
    }
    const UtFontProbe rootProbe = {liveStyleOf, liveGlyphOf, (void*)rootFont};
    utFontCacheFill(&root, rootProbe);
    static char labels[64][128];
    const int nl = utTmCheckLabels(labels, 64);
    const char* strs[96];
    int n = 0;
    const UtCaptions* caps = panelButtonCaptions();
    for (int i = 0; caps && i < caps->n && n < 96; ++i) strs[n++] = caps->tags[i];
    for (int i = 0; i < nl && n < 96; ++i) strs[n++] = labels[i];
    const UtTmCheck r = utFontCacheCompare(root, table, strs, n, 1, kUtTmMaxSize);
    if (!r.agree) {
        logE("text: the live font metrics differ from the compiled table on the root font - %s "
             "(%d of %d sizes, %d of %d glyphs, %d of %d widths differ, largest %d px); the live "
             "measure is OFF for this world and the table measures as before",
             r.first[0] ? r.first : "the font gave no styles", r.sizeDiff, r.sizes, r.glyphDiff,
             r.glyphs, r.widthDiff, r.widths, r.maxDelta);
        return;
    }
    // g_fontHeader was loaded by the same root path, so it is this font; should the engine ever
    // hand out another object, it is measured with the route just proved.
    if ((const void*)g_fontHeader == (const void*)rootFont) {
        g_liveHeader = root;
    } else {
        const UtFontProbe hp = {liveStyleOf, liveGlyphOf, (void*)g_fontHeader};
        utFontCacheFill(&g_liveHeader, hp);
    }
    if (!g_liveHeader.valid) return;
    g_liveFont = g_fontHeader;
    g_liveMeasure = true;
    static bool said = false;
    if (!said) {
        logI("text: live metrics agree with the table - %d styles, %d glyphs, %d widths (%d "
             "captions and %d label lines at sizes 1..64), largest delta %d px; the pad and the "
             "label measure the root font the engine loaded",
             r.styles, r.glyphs, r.widths, caps ? caps->n : 0, nl, r.maxDelta);
    } else {
        logD("text: live metrics agree with the table again (%d styles, %d glyphs, %d widths)",
             r.styles, r.glyphs, r.widths);
    }
    said = true;
}

// The fonts are the ROOT fonts in every game language, loaded as 1.1.0 loaded them
// (GraphicsEngine::LoadFont on the root path): the tab is English by default, and only a [style]
// font key changes a role's font (roleFontsSync). The game language's copy of savapromedium
// (StyleManager::LoadFontDirect, when bound) is loaded beside them for one purpose - the glyph
// check of the rollover lines, which the engine draws in it; on an English game it is the root
// font itself (no special folder: the same call on the same path).
void ensureFonts() {
    if (g_fontsTried) {
        textTick();
        return;
    }
    g_fontsTried = true;
    g_liveMeasure = false;
    InterlockedExchange(&g_fontTid, (LONG)GetCurrentThreadId());
    for (RoleFont& rf : g_role) rf.on = false;   // last world's fonts are gone
    InterlockedIncrement(&g_measureGen);
    GdGraphicsEngine* gfx = graphicsEngine();
    if (!gfx || !g_gd.GfxLoadFont) return;
    struct { const char* path; const GdFont** slot; } wanted[] = {
        {"fonts/nevisnooutlinespaced.fnt", &g_fontTitle},   // style_windowtitle01
        {kUtRootFontPath, &g_fontHeader},                   // style_rollover_title
        {"fonts/jura.fnt", &g_fontBody},                    // style_textwhite_sizen
    };
    for (int i = 0; i < 3; ++i) {
        const std::string p(wanted[i].path);
        const GdFont* f = g_gd.GfxLoadFont(gfx, &p);
        *wanted[i].slot = f;
        logD("panel: font \"%s\" -> %p", wanted[i].path, (void*)f);
    }
    if (!g_fontBody) g_fontBody = g_fontTitle ? g_fontTitle : g_fontHeader;
    if (!g_fontTitle) g_fontTitle = g_fontBody;
    if (!g_fontHeader) g_fontHeader = g_fontBody;
    g_fontRollover = bindingsText().font ? (const GdFont*)bindingsTextLoadFontDirect(kUtRootFontPath) : nullptr;
    if (!g_fontRollover) g_fontRollover = g_fontHeader;
    logD("panel: the rollover lines' font (the game language's copy of savapromedium, for the glyph "
         "check only) -> %p%s",
         (void*)g_fontRollover, g_fontRollover == g_fontHeader ? " (the root font itself)" : "");
    liveMeasureBegin(gfx);
    // The [style] font keys, measured with the route just proved; then the strings for this
    // world: the glyph check per role over the fonts that draw it.
    roleFontsSync(true);
    textWorldBuild(true);
    InterlockedIncrement(&g_measureGen);   // the layout's caption fit measures again
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

namespace {

// catalogue.bin at `path` as a new Catalogue, its counts logged; null (reason logged) when unusable.
gdut::Catalogue* catalogueLoad(const std::string& p) {
    gdut::Catalogue* c = nullptr;
    try {
        c = new gdut::Catalogue();
    } catch (...) {
        return nullptr;
    }
    std::string err;
    if (c->loadFromFile(p, &err)) {
        logI("catalogue loaded: %llu items, %llu sets (format v%u)",
             (unsigned long long)c->itemCount(), (unsigned long long)c->setCount(),
             c->formatVersion());
        logD("catalogue: \"%s\" - %u equipment, %u relics, %u default-visible, %llu bytes",
             p.c_str(), c->equipmentCount(), c->relicCount(), c->defaultVisibleCount(),
             (unsigned long long)c->byteSize());
        int nonEmpty = 0;
        for (int gi = 0; gi < gdut::kSlotGroupCount; ++gi) {
            const gdut::GroupRange r = c->groupRange((gdut::SlotGroup)gi);
            if (r.count == 0) continue;
            ++nonEmpty;
        }
        logD("catalogue: %d non-empty groups of %d", nonEmpty, gdut::kSlotGroupCount);
        return c;
    }
    logE("catalogue: \"%s\" not usable: %s", p.c_str(), err.c_str());
    delete c;
    return nullptr;
}

}  // namespace

bool panelInit(HMODULE selfModule) {
    if (g_initialised || g_loadFailed) return g_initialised;

    // The active view's catalogue.bin - ut_generate.h decides which folder that is (the mod folder
    // on the start-up view), and nothing else in this file may name a path.
    char catPath[MAX_PATH] = {0};
    if (!utViewFile(selfModule, "catalogue.bin", catPath, sizeof(catPath)))
        utModFile(selfModule, "catalogue.bin", catPath, sizeof(catPath));
    gdut::Catalogue* c = catalogueLoad(std::string(catPath));
    if (c) {
        {
            std::lock_guard<std::mutex> lock(g_modelMx);
            g_catP = c;
            g_own.reset(c);
        }
        g_initialised = true;
        return true;
    }
    g_loadFailed = true;
    logE("catalogue: NO usable catalogue.bin - the mod tab will say NO DATA and draw no grid");
    return false;
}

bool panelReloadCatalogue(HMODULE selfModule) {
    char catPath[MAX_PATH] = {0};
    if (!utViewFile(selfModule, "catalogue.bin", catPath, sizeof(catPath))) {
        logW("catalogue: the view has no catalogue.bin - the previous catalogue stays");
        return false;
    }
    gdut::Catalogue* c = catalogueLoad(std::string(catPath));
    if (!c) {
        logW("catalogue: the view's catalogue.bin could not be read - the previous catalogue stays");
        return false;
    }
    int replayed = 0, owned = 0;
    {
        std::lock_guard<std::mutex> lock(g_modelMx);
        g_catP = c;
        g_own.reset(c);
        g_liveBitmaps.clear();      // keyed by the previous catalogue's indexes
        g_unknownSamples.clear();
        for (const auto& kv : g_liveIds) {
            g_own.onItemAdded(kv.first, kv.second);
            ++replayed;
        }
        owned = g_own.ownedCount();
    }
    InterlockedExchange(&g_notInDb, -1);   // the next reconcile counts against this catalogue
    g_loadFailed = false;
    g_initialised = true;
    logI("catalogue: the view's catalogue is in (\"%s\") - ownership recounted over %d live "
         "item(s), %d distinct record(s) owned", catPath, replayed, owned);
    return true;
}

int panelCatalogueHas(const char* record) {
    gdut::Catalogue* c = g_catP;
    if (!c || !record || !*record) return -1;
    try {
        return c->indexOfRecord(std::string_view(record)) >= 0 ? 1 : 0;
    } catch (...) {
        return -1;
    }
}

void panelSetNotInDatabase(int n) { InterlockedExchange(&g_notInDb, (LONG)n); }

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

    gdut::Catalogue* const cat = g_catP;
    if (!cat) return;
    const int catIndex = cat->indexOfRecord(record);

    // Learn the engine's ItemClassification numbering from an item whose classification the
    // catalogue already knows. Before the lock: no engine call is made while it is held.
    const GdTexture* liveBitmap = nullptr;
    if (catIndex >= 0) {
        const int want = (int)cat->item((size_t)catIndex).classification;
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
    try {
        g_liveIds[id] = record;
    } catch (...) {
    }
    if (cat != g_catP) return;   // the view changed under us: the index belongs to the old one
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
    g_liveIds.erase(itemId);
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

// ---- the button captions ----------------------------------------------------------------------
// The pad is keyed by the BUTTONS of the buttons file (ut_buttons.h): button 0 is the vanilla
// crafting-materials page, then one button per [TAG] of the file in file order - each holding one
// or more groups, its pages theirs in group order - then the OWN filter toggle; a paged pad adds
// NEXT (ut_padpage.h). With no file (or the generated one) that is one button per G line of
// uniq-groups.txt in file order, captioned by the line's tag: exactly the pad before the file.
// Until a table has loaded the captions are the built-in table below - the default groups file's
// own 24 tags, so the menu lines read as 1.1.0's.
const char* const kButtonTags[kUtButtonCount] = {
    "MAT",                                                         // the game's own page
    "HEL", "SHO", "CHE", "GLO", "BEL", "PAN", "BOO", "AMU", "MED",  // the 24 built-in groups
    "RNG", "OFF", "SHD", "AX1", "DAG", "MC1", "SCP", "SW1", "GUN",
    "AX2", "MC2", "SPR", "SW2", "GN2", "REL",
    "OWN",                                                         // the owned-only filter
};
const UtCaptions kDefaultCaps = {kButtonTags, kUtButtonCount};
const char* const kNextTag = ">>";
const char* const kEditTag = "EDIT";

// The logical button that shows `group` (-1 = the vanilla page -> MAT); -1 for a group on no
// button ([unassigned]). Without a TextWorld for this table: one button per group.
int buttonOfGroup(int group) {
    if (group < 0) return group + 1;
    const TextWorld* w = textWorldNow();
    if (!w) return group + 1;
    if ((size_t)group >= w->buttonOf.size() || w->buttonOf[(size_t)group] < 0) return -1;
    return w->buttonOf[(size_t)group] + 1;
}
// The pad page index of a group (ut_padpage.h counts buttons): its button's, 0 for none.
int padIndexOfGroup(int group) {
    const int b = buttonOfGroup(group) - 1;
    return b < 0 ? 0 : b;
}

// One group table's captions in logical order: MAT, the groups, OWN, NEXT, EDIT. Built once per
// table (liveGroupTableGen) by whichever thread asks first; a replaced table is never freed,
// because the layout in force keeps a pointer into it.
struct CapTable {
    long gen;
    int groups;
    long textGen;       // the TextWorld its captions came from, 0 = the G lines' tags
    std::vector<std::string> text;
    std::vector<const char*> ptr;
    std::vector<const char*> ptrAllEdit;   // MAT, the groups, OWN, EDIT
    UtCaptions all;        // MAT, the groups, OWN: the unpaged pad's fit
    UtCaptions paged;      // the same plus NEXT
    UtCaptions allEdit;    // the unpaged pad with the EDIT button
    UtCaptions pagedEdit;  // the paged pad with the EDIT button (every caption)
};
CapTable* volatile g_capTable = nullptr;
SRWLOCK g_capLock = SRWLOCK_INIT;

const CapTable* capTable() {
    const long gen = liveGroupTableGen();
    const int groups = gen > 0 ? liveGroupCount() : 0;
    // The buttons' captions (the buttons file through the chain), when the game thread has
    // resolved them for this very table; else one button per group, captioned by its tag.
    const TextWorld* w = textWorldFor(gen, groups);
    const int want = w ? w->buttons : (groups > 0 ? groups : kUtButtonCount - 2);
    const long wgen = w ? w->gen : 0;
    CapTable* t = g_capTable;
    if (t && t->gen == gen && t->groups == want && t->textGen == wgen) return t;
    AcquireSRWLockExclusive(&g_capLock);
    t = g_capTable;
    if (!t || t->gen != gen || t->groups != want || t->textGen != wgen) {
        CapTable* n = nullptr;
        try {
            n = new CapTable();
            n->gen = gen;
            n->groups = want;
            n->textGen = wgen;
            if (w) {
                for (const std::string& s : w->caps) n->text.push_back(s);   // MAT .. EDIT
            } else {
                n->text.push_back(kButtonTags[0]);
                for (int g = 0; g < want; ++g)
                    n->text.push_back(groups > 0 ? liveGroupTag(g) : kButtonTags[g + 1]);
                n->text.push_back(kButtonTags[kUtButtonCount - 1]);
                n->text.push_back(kNextTag);
                n->text.push_back(kEditTag);
            }
            for (const std::string& s : n->text) n->ptr.push_back(s.c_str());
            const int all = (int)n->ptr.size() - 2;   // MAT .. OWN
            n->ptrAllEdit.assign(n->ptr.begin(), n->ptr.begin() + all);
            n->ptrAllEdit.push_back(n->ptr.back());
            n->all = UtCaptions{&n->ptr[0], all};
            n->paged = UtCaptions{&n->ptr[0], all + 1};
            n->allEdit = UtCaptions{&n->ptrAllEdit[0], all + 1};
            n->pagedEdit = UtCaptions{&n->ptr[0], all + 2};
            g_capTable = n;
            t = n;
        } catch (...) {
            delete n;   // whatever table there was stays
        }
    }
    ReleaseSRWLockExclusive(&g_capLock);
    return t;
}

// The caption of logical button `logical` (ut_padpage.h). Never null.
const char* capText(int logical) {
    const CapTable* t = capTable();
    if (!t) return (logical >= 0 && logical < kUtButtonCount) ? kButtonTags[logical] : "?";
    if (logical < 0 || (size_t)logical >= t->ptr.size()) return "?";
    return t->ptr[(size_t)logical];
}

// ---- geometry: the BUTTON PAD ------------------------------------------------------------------
// THE JOINT FREE RECTANGLE, measured by decoding both plates (they are
// TEX\x02 + 8 bytes + a DDS whose magic is DDSR, 438x627 32-bit BGRA):
//   the GAME's caravan_transfercomponent1_bg.tex is flat over x 102..427 for rows 25..69, and
//   flat over x 102..247 + x 280..418 for rows 8..24 (a central medallion owns 248..279);
//   all seven of the mod's own plates are flat over x 102..422 continuously from row 2 to 72.
// So the rectangle free on BOTH is x 102..422, y 25..69 (321 x 45) - and the text-only band
// above it is y 8..24, where the ONE-line label lives.  x 423..427 is legal on the vanilla
// plate and NOT on the mod plates, which is why the refusal limit is 422 and not 427.
// src\ut_padmat.h holds those numbers for the harness.
//
// THE PAD IN FORCE comes from the layout (ut_live.h liveLayoutGet -> ut_layout.h): the user's
// pad_<W>x<H> rectangle when it is legal, else today's pad - 3 + 9*34 + 8*pad_gap + 3 wide and
// 3*pad_h + 2*pad_gap tall at x 102, y pad_y (320 x 44 at x 102..421, y 25..68 with the shipped
// keys), refused WHOLE when it would leave the joint band. utPadFit lays the 26 buttons out in
// it: for today's rectangle that is the 9 x 3 grid of 34 x pad_h cells it has always been, with
// OWN spanning the last two cells of row 3. The draw, the hit test, the click band and the hover
// all read the one PadPlan below, so they can never disagree.
//
// THE MATERIALS-PAGE RULE (ut_layout.h utMatPadResolve, the only copy): on the game's own page
// the pad is the user's padmat_ rectangle when that is set, inside the page and fits the buttons,
// else the pad in force (pad_ or the derived pad), drawn WHERE IT IS - over whatever live boxes
// lie under it (the draw runs from the Draw tail, after the engine drew the page). A point inside
// it is the pad's: ut_plate.cpp's mouse slot keeps a LEFT DOWN there from the engine (ut_padmat.h
// utPadLeftDown: a button acts, the ground between the buttons does nothing), and puts back the
// engine's hovered-box answer for every other event there, so a covered box neither takes a click
// nor shows its tooltip (panelMatPadAt). The decision is logged once it has held 250 ms, and again
// when the pad's rectangle, the cover or the covered count changes - one line naming the
// rectangle and the live boxes it covers (INFO when it covers some, DEBUG when they are clear).
int g_lastCollectionGroup = 0;          // game thread only

struct MatMemo {
    UtRect rect;              // the decision being held
    int under, boxes;
    char cover[16];
    DWORD since;              // when it began
    int logged;               // 1 = last* below is the decision the log holds
    UtRect lastRect;
    int lastUnder, lastBoxes;
    char lastCover[16];
    int lines;                // decisions logged in this world
};
MatMemo g_mat = {};
SRWLOCK g_matLock = SRWLOCK_INIT;
volatile LONG g_matReset = 1;   // set at a world teardown: the next decision is logged again
const int kMatLogLines = 12;    // lines per world; then one line says the rest are not
// A decision is logged once it has held this long, so the frame or two of a page switch (the
// collection's boxes not yet moved back) never reaches the log.
const DWORD kMatHoldMs = 250;

// The rule for these boxes (cheap: one overlap count). *logLine: 0 = nothing to log; else a
// decision other than the last one logged has held kMatHoldMs and this is its line number in the
// world (kMatLogLines + 1 = the line that says no more are logged; past it, 0 again). n < 0 = the
// boxes could not be read: decided over none, never logged.
UtMatPad matDecide(const LiveLayout& lay, const UtRectF* boxes, int n, float s, int* logLine) {
    *logLine = 0;
    const UtMatPad m = utMatPadResolve(lay.in, lay.L, lay.padmatSet ? &lay.padmat : nullptr,
                                       boxes, n > 0 ? n : 0, s);
    if (n <= 0 || m.mode != kUtMatPadFull) return m;
    const DWORD now = GetTickCount();
    AcquireSRWLockExclusive(&g_matLock);
    if (InterlockedExchange(&g_matReset, 0)) {
        g_mat.logged = 0;
        g_mat.lines = 0;
        g_mat.since = 0;
        g_mat.cover[0] = 0;
    }
    if (!g_mat.since || !utRectEq(g_mat.rect, m.rect) || g_mat.under != m.under ||
        g_mat.boxes != n || strcmp(g_mat.cover, lay.cover) != 0) {
        g_mat.rect = m.rect;
        g_mat.under = m.under;
        g_mat.boxes = n;
        strcpy_s(g_mat.cover, sizeof(g_mat.cover), lay.cover);
        g_mat.since = now ? now : 1;
    }
    if (now - g_mat.since >= kMatHoldMs &&
        (!g_mat.logged || !utRectEq(g_mat.lastRect, m.rect) || g_mat.lastUnder != m.under ||
         g_mat.lastBoxes != n || strcmp(g_mat.lastCover, lay.cover) != 0)) {
        g_mat.logged = 1;
        g_mat.lastRect = m.rect;
        g_mat.lastUnder = m.under;
        g_mat.lastBoxes = n;
        strcpy_s(g_mat.lastCover, sizeof(g_mat.lastCover), lay.cover);
        if (g_mat.lines <= kMatLogLines) *logLine = ++g_mat.lines;
    }
    ReleaseSRWLockExclusive(&g_matLock);
    return m;
}

// One decision of the rule, as the log says it.
void matLog(const UtMatPad& m, const LiveLayout& lay, int nBoxes, int line) {
    if (line > kMatLogLines) {
        logI("buttons: the materials-page pad changed %d times in this world - further changes "
             "are not logged until the next one", kMatLogLines);
        return;
    }
    char text[320];
    utMatPadText(m, lay.cover, nBoxes, lay.padmatSet ? &lay.padmat : nullptr, text, sizeof(text));
    if (m.under > 0)
        logI("buttons: the materials page: %s - the pad is drawn over them, and a click or a hover "
             "on it is the pad's", text);
    else
        logD("buttons: the materials page: %s - the pad is drawn where it is", text);
}

struct PadPlan {
    UtButtonRect r[kUtPadButtonsMax];   // per drawn SLOT, window-local SCALED px; 0 x 0 = not drawn
    int logical[kUtPadButtonsMax];      // the slot's logical button (ut_padpage.h), -1 = none
    int n;                              // slots
    UtPadPages pages;                   // the group table's buttons and pages
    int page;                           // the pad page on screen
    UtRect ground;                      // record px: the pad's rectangle
    UtPadFit fit;                       // the cells the buttons sit in
    int mat;                            // 1 = on the game's own materials page (its rule decided)
    int bandX0, bandX1;                 // record px: the click probe's x band (x1 exclusive)
    int user;                           // 1 = a user rectangle (pad_ or padmat_)
    int screen;                         // 1 = pad_screen is in force: drawn from the screen site
    char cover[16];
};

// Fills `P` for UI scale `s`; false = no pad at all (an unusable scale, no layout yet, or the pad
// in force refused). Pure arithmetic plus one read of the live boxes on the materials page.
bool padPlan(PadPlan* P, float s) {
    memset(P, 0, sizeof(*P));
    // (1) the scale guard stays even with the exported GetUIScaleFactor: a hooked or odd value
    //     must still refuse the pad, never scale it.
    if (!(s >= 0.2f && s <= 8.0f)) return false;
    LiveLayout lay;
    if (!liveLayoutGet(&lay)) return false;
    const UtLayout& L = lay.L;
    if (!L.pad.ok) return false;
    strcpy_s(P->cover, sizeof(P->cover), lay.cover);
    P->pages = lay.pages;
    P->fit = L.pad;
    P->ground = L.rect[kUtLayPad];
    P->user = L.user[kUtLayPad];
    P->screen = L.screen[kUtLayPad] ? 1 : 0;
    P->bandX0 = P->user ? P->ground.x : kUtPadBandX0;
    P->bandX1 = P->user ? P->ground.x + P->ground.w : kUtPadBandX1;
    const int shown = liveShownGroup(nullptr);
    if (shown >= 0) {
        g_lastCollectionGroup = shown;
    } else if (shown == -1) {
        // The game's own materials page: padmat_ or the pad in force, where it is, over the boxes.
        // every live box, however many the view's frame carries (160..512)
        int want = liveCapturedBoxes(nullptr, 0);
        if (want < 1) want = 1;
        std::vector<float> xywh((size_t)want * 4);
        int n = plateBoxRectsLocal(xywh.data(), want);
        if (n < 0) n = 0;
        std::vector<UtRectF> boxes((size_t)(n > 0 ? n : 1));
        for (int i = 0; i < n; ++i)
            boxes[(size_t)i] =
                UtRectF{xywh[4 * i], xywh[4 * i + 1], xywh[4 * i + 2], xywh[4 * i + 3]};
        int logLine = 0;
        const UtMatPad m = matDecide(lay, boxes.data(), n, s, &logLine);
        if (logLine) matLog(m, lay, n, logLine);
        if (m.mode == kUtMatPadNone) return false;
        if (!utRectEq(m.rect, P->ground)) {
            // the user's padmat_ rectangle
            P->fit = utPadFit(m.rect, lay.in.buttons, lay.in.padGap, s, lay.in.captionMax,
                              lay.in.widest, lay.in.widestCtx);
            if (!P->fit.ok) return false;
            P->ground = m.rect;
            P->user = 1;
            P->bandX0 = m.rect.x;
            P->bandX1 = m.rect.x + m.rect.w;
        }
        P->mat = 1;
    }
    P->page = utPadPageOf(P->pages, padIndexOfGroup(shown >= 0 ? shown : g_lastCollectionGroup));
    for (int i = 0; i < kUtPadButtonsMax; ++i) P->logical[i] = -1;
    P->n = P->pages.buttons < kUtPadButtonsMax ? P->pages.buttons : kUtPadButtonsMax;
    for (int i = 0; i < P->n; ++i) {
        P->logical[i] = utPadSlotLogical(P->pages, P->page, i);
        if (P->logical[i] < 0) continue;   // the last page's empty cells: not drawn, never hit
        // Both EDGES are rounded (not width * scale), so neighbouring cells stay gutter-exact
        // and hit-test-exact at every UI scale - the same floorf(v*s+0.5f) applyLayout uses.
        const UtRectF f = utScaledRectF(utPadCellRect(P->fit, i), s);
        P->r[i] = UtButtonRect{f.x, f.y, f.w, f.h};
    }
    return true;
}

// The pad in force for the log and the menu test: the resolved one once a UI scale is known,
// else today's pad from the three [advanced] keys (false when 1.1.0 would refuse it).
bool padInForce(UtRect* r, UtPadFit* fit) {
    LiveLayout lay;
    if (liveLayoutGet(&lay)) {
        if (!lay.L.pad.ok) return false;
        *r = lay.L.rect[kUtLayPad];
        *fit = lay.L.pad;
        return true;
    }
    if (!utPadDefaultInBand(g_cfg.padY, g_cfg.padH, g_cfg.padGap, r)) return false;
    int gap = g_cfg.padGap;
    if (gap < 0) gap = 0;
    if (gap > 4) gap = 4;
    const UtCaptions* caps = panelButtonCaptions();
    *fit = utPadFit(*r, caps->n, gap, 1.0f, panelCaptionMaxInForce(), widestCaptionLivePx, caps);
    return fit->ok != 0;
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

// The widest caption at `size`, measured in the font the pad draws (widestCaptionLivePx).
int widestTagPx(int size, const UtCaptions* caps) { return widestCaptionLivePx(size, caps); }

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
// The ceiling is pad_caption_max (14 by default), or size.caption_max of uniquetab-text.ini when
// set; the arithmetic is ut_layout.h's utCaptionSize. size.caption > 0 asks for one fixed size,
// still bounded by the cell's height; the draw then clips each caption to its own button.
int captionFixedSize() {
    const utt::UtTextStyle* st = textStyle();
    return st && st->size[utt::kStyleSizeCaption] > 0 ? st->size[utt::kStyleSizeCaption] : 0;
}
// The largest size the cell's HEIGHT allows (three quarters of it, the two edge lines, the ceiling).
int captionTopSize(int boxH, int capMax) {
    int top = (boxH * 3) / 4;
    if (top > boxH - 2) top = boxH - 2;
    if (top > capMax) top = capMax;
    return top;
}
int buttonTagSize(int boxW, int boxH, const UtCaptions* caps) {
    static int lastW = -1;
    static int lastH = -1;
    static int lastMax = -1;
    static int lastFixed = -1;
    static const UtCaptions* lastCaps = nullptr;
    static long lastGen = -1;
    static int lastSize = 0;
    const int capMax = panelCaptionMaxInForce();
    const int fixed = captionFixedSize();
    const long gen = panelMeasureGen();
    if (boxW == lastW && boxH == lastH && capMax == lastMax && fixed == lastFixed &&
        caps == lastCaps && gen == lastGen)
        return lastSize;
    if (fixed > 0) {
        const int top = captionTopSize(boxH, fixed);
        lastSize = top >= kUtCaptionMin ? top : 0;
    } else {
        lastSize = utCaptionSize(boxW, boxH, capMax, widestCaptionLivePx, caps);
    }
    lastW = boxW;
    lastH = boxH;
    lastMax = capMax;
    lastFixed = fixed;
    lastCaps = caps;
    lastGen = gen;
    return lastSize;
}

// 1 px of the DESIGN, in screen px, never 0 - at UI scale 0.700 a 1 px line rounds to 1 and at
// 2.000 to 2, and a 0 px line would silently delete the whole look.
int padPx(float s, int recordPx) {
    int v = iround((float)recordPx * s);
    if (v < 1) v = 1;
    return v;
}

// ---- the screen parts: pad_screen / label_screen drawn off the caravan ------------------------
volatile LONG g_scrPadLogged = 0;     // the once-per-world "the pad is on the screen" INFO line
volatile LONG g_scrLabelLogged = 0;   // the same for the label

// The 1-px raised frame around a screen part's ground (light top/left, dark bottom/right).
void drawScreenFrame(GdCanvas* canvas, const UtScreenFrame& f) {
    if (f.w <= 0 || f.h <= 0) return;
    fillRect(canvas, {f.top[0], f.top[1], f.top[2], f.top[3]}, kLineTop);
    fillRect(canvas, {f.left[0], f.left[1], f.left[2], f.left[3]}, kLineTop);
    fillRect(canvas, {f.bottom[0], f.bottom[1], f.bottom[2], f.bottom[3]}, kLineBot);
    fillRect(canvas, {f.right[0], f.right[1], f.right[2], f.right[3]}, kLineBot);
}

// A caption's width in ITS button's font at ITS size (utTmClipToWidth's measure for one button).
struct ButtonMeasure {
    int logical;
    int size;
};
int buttonCaptionWidthPx(const char* s, const void* ctx) {
    const ButtonMeasure* m = (const ButtonMeasure*)ctx;
    return panelButtonCaptionTextPx(m->logical, s, m->size);
}

// `screen` = the call from the screen site (ut_plate.cpp): it draws the pad only while pad_screen is
// in force, the window call (the Draw tail) only while it is not - one pad, one place. The screen
// pad adds a 1-px frame around the same opaque ground and fills `pub` (client px) for its input.
bool drawButtonsInner(GdCanvas* canvas, float ox, float oy, bool screen, UtPadScreenRects* pub) {
    // The pad is solid quads and nothing else.  CanvasRenderRectSolid is a REQUIRED export (a
    // miss installs no detour at all, gd_runtime.cpp), so this can only be null in a build that
    // never armed - but the whole `painted` proof below rests on it, so it is tested here rather
    // than assumed: with it gone NOTHING can appear and no click may be authorised.
    if (!g_gd.CanvasRenderRectSolid) return false;
    const float s = liveUiScale();          // never 1.0 by default; 0 = draw nothing
    PadPlan P;
    if (!padPlan(&P, s)) return false;
    if ((P.screen != 0) != screen) return false;   // the other site draws this pad
    const UtButtonRect* r = P.r;
    const int active = buttonOfGroup(liveShownGroup(nullptr));
    const int ownIdx = utPadOwnIndex(P.pages);
    const int nextIdx = utPadNextIndex(P.pages);
    const int editIdx = utPadEditIndex(P.pages);
    const UtCaptions* caps = panelButtonCaptionsFor(P.pages.pages > 1 ? 1 : 0, P.pages.edit);
    // The OWN button is not a group, so it can never be `active` (buttonOfGroup never returns
    // its index).  It gets its own ON look from its own flag.
    const bool filterOn = liveOwnedOnly();
    const DWORD now = GetTickCount();
    const LONG downIdx = InterlockedCompareExchange(&g_btnDownIdx, -1, -1);
    const DWORD downAt = (DWORD)InterlockedCompareExchange(&g_btnDownAt, 0, 0);
    const bool downLive = downIdx >= 0 && downAt != 0 && (now - downAt) < 150;
    const int line = padPx(s, 1);

    // The pad GROUND, drawn first: it is what the 1 px gutters and the 3 px side margins are
    // made of, exactly as the HTML's `background:#0e0f11` shows through its grid gaps.  Without
    // it a 1 px gutter would be a 1 px window onto the plate art and the grid would not read.
    const int px0 = iround(ox + floorf((float)P.ground.x * s + 0.5f));
    const int py0 = iround(oy + floorf((float)P.ground.y * s + 0.5f));
    const int px1 = iround(ox + floorf((float)(P.ground.x + P.ground.w) * s + 0.5f));
    const int py1 = iround(oy + floorf((float)(P.ground.y + P.ground.h) * s + 0.5f));
    const gdut::Rect ground = {px0, py0, px1 - px0, py1 - py0};
    // Off the caravan the pad has no plate around it: a 1-px raised frame OUTSIDE the ground
    // (ut_screendraw.h), so the ground and every cell keep the in-window pixels exactly.
    const UtScreenFrame frame = utScreenFrame(
        UtRectF{(float)ground.x, (float)ground.y, (float)ground.w, (float)ground.h}, line);
    if (screen) drawScreenFrame(canvas, frame);
    fillRect(canvas, ground, kPadBack);

    // The caption size is fitted to the SMALLEST NARROW cell.  The OWN toggle is deliberately
    // left out of the minimum: it is two cells wide, so it can never be the one that overflows,
    // and letting it in would only ever raise the minimum and never lower it.
    int minW = iround(r[0].w);
    int minH = iround(r[0].h);
    for (int i = 1; i < P.n; ++i) {
        if (P.logical[i] == ownIdx || P.logical[i] < 0) continue;
        if (!(r[i].w > 0.0f && r[i].h > 0.0f)) continue;
        if (iround(r[i].w) < minW) minW = iround(r[i].w);
        if (iround(r[i].h) < minH) minH = iround(r[i].h);
    }
    int fs = buttonTagSize(minW, minH, caps);
    // When not even size 6 fits the widest caption (or size.caption fixes the size), every caption
    // is drawn at the size the HEIGHT allows and cut to its own button - never a blank pad.
    const bool clipEach = fs == 0 || captionFixedSize() > 0;
    if (fs == 0 && captionTopSize(minH, panelCaptionMaxInForce()) >= kUtCaptionMin) fs = kUtCaptionMin;
    const GdColor capColor = textColorOr(utt::kStyleColorCaption, kCaption);
    const GdColor ringColor = textColorOr(utt::kStyleColorRing, kRing);
    const GdColor markColor = textColorOr(utt::kStyleColorMark, kMarkBg);
    // A user's mark colour keeps the design's hover step (#3a6288 -> #456c93).
    auto lift = [](float c, float d) { return c + d > 1.0f ? 1.0f : c + d; };
    const GdColor markHover =
        textStyle() && textStyle()->color[utt::kStyleColorMark] >= 0
            ? GdColor{lift(markColor.r, kMarkHover.r - kMarkBg.r),
                      lift(markColor.g, kMarkHover.g - kMarkBg.g),
                      lift(markColor.b, kMarkHover.b - kMarkBg.b), kMarkHover.a}
            : kMarkHover;
    // A cell too short for even a size-6 caption draws INDISTINGUISHABLE buttons, and
    // `painted` would still authorise consuming clicks on them.
    // pad_h's clamp floor of 12 makes that unreachable from the ini at any UI scale the game
    // offers; this says so once per world for the scales below 0.700 that only a hooked or
    // mis-measured GetUIScaleFactor can produce.  Once-per-world, never per frame.
    if (fs == 0 && !InterlockedExchange(&g_btnBlankLogged, 1)) {
        logW("buttons: the %dx%d px cell (pad_h=%d at UI scale %.3f) is too short for a caption - "
             "%d buttons are drawn UNLABELLED",
             minW, minH, P.fit.cellH, s, P.n);
    }
    int tags = 0, painted = 0;
    for (int i = 0; i < P.n; ++i) {
        const int b = P.logical[i];   // the logical button in this slot (ut_padpage.h)
        if (b < 0) continue;
        const gdut::Rect dst = {iround(ox + r[i].x), iround(oy + r[i].y), iround(r[i].w),
                                iround(r[i].h)};
        if (dst.w <= 0 || dst.h <= 0) continue;
        const bool toggle = (b == ownIdx);
        // "this button is ON" - the shown group, or the OWN toggle while the owned-only filter
        // is in force.  ONE flag, so the background, the caption colour and the ring can never
        // disagree about it.
        const bool on = toggle ? filterOn : (b == active);
        // MAT, OWN, NEXT and EDIT are not collection groups, so none of them can carry a search
        // mark; a button carries it when ANY of its groups does (buttons file: several groups per
        // button). EDIT is never `on` either: buttonOfGroup never returns its index.
        const bool match =
            b > 0 && b != ownIdx && b != nextIdx && b != editIdx && panelButtonSearchMarked(b);
        const bool hot = (b == g_btnHover);
        const bool down = downLive && (int)downIdx == b;
        const char* tag = capText(b);
        // This button's caption size and font (the buttons file's size = / font =); with neither
        // line they are the pad's shared size and the caption font (identity).
        const int bfs = panelButtonCaptionSize(b, fs, dst.w, dst.h);
        char cut[64];
        if (clipEach && bfs >= kUtCaptionMin) {
            strncpy_s(cut, sizeof(cut), tag, _TRUNCATE);
            const int room = toggle ? dst.w - padPx(s, 6) - padPx(s, 5) - 2 * line : dst.w - 1;
            const ButtonMeasure bm = {b, bfs};
            utTmClipToWidth(cut, room, buttonCaptionWidthPx, &bm);
            tag = cut;
        }
        // The HTML's cascade, in order.  `.tgl[aria-pressed=true]` beats :active and :hover;
        // `:active` beats `:hover`; `.pad button:hover` beats `.pad .tgl`'s own background.
        GdColor bg = kBtnBg, top = kLineTop, bot = kLineBot, cap = capColor;
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
            bg = hot ? markHover : markColor;
        } else if (hot) {
            bg = kBtnHover;
        } else if (toggle) {
            bg = kTglBg;
        }
        fillRect(canvas, dst, bg);
        ++painted;
        if (pub) utScreenPubButton(pub, ox, oy, r[i].x, r[i].y, r[i].w, r[i].h, b);
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
        const int ty = dst.y + (dst.h - bfs) / 2;
        if (toggle) {
            const int led = padPx(s, 6);
            const int lgap = padPx(s, 5);
            const int tw = bfs >= 6 ? panelButtonCaptionTextPx(b, tag, bfs) : 0;
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
        } else if (bfs >= 6) {
            const int tw = panelButtonCaptionTextPx(b, tag, bfs);
            tx = dst.x + (dst.w - tw) / 2;
            if (tx < dst.x) tx = dst.x;
        }
        if (bfs >= 6 && drawText(canvas, tx, ty, bfs, cap, tag, panelButtonCaptionFont(b))) ++tags;
        // The selected group's INSET ring: drawn INSIDE the button rect, so it changes no size
        // at all - it replaces the button's own two edge lines rather than sitting between them
        // and the caption, which at the 9 px cell of UI scale 0.700 would cross the glyphs.
        // The OWN toggle never wears it: its ON look is the amber plate and the lit lamp, and a
        // second ON mark on the same button would only be one mark too many.
        if (on && !toggle) {
            outlineRect(canvas, dst, line, ringColor);
        }
    }
    // Only a pad the user can SEE may authorise a consumed click, so the "drawn this frame-pair"
    // stamp the click path reads is set from what was really painted: every quad above went
    // through CanvasRenderRectSolid, which was proved non-null at the top of this function, so
    // `painted > 0` means the buttons really are on the screen.
    if (painted <= 0) return false;
    InterlockedExchange(&g_btnDrawnAt, (LONG)now);
    if (screen) {
        utScreenPubPad(pub, frame);
        if (!InterlockedExchange(&g_scrPadLogged, 1)) {
            LiveLayout lay;
            const bool have = liveLayoutGet(&lay);
            logI("buttons: the pad is on the screen at %s -> client %d,%d %dx%d (window-local "
                 "%d,%d,%d,%d at the window origin %.1f,%.1f, UI scale %.3f%s), %d buttons; drawn "
                 "from %s",
                 g_cfg.padScreen, ground.x, ground.y, ground.w, ground.h, P.ground.x, P.ground.y,
                 P.ground.w, P.ground.h, ox, oy, s,
                 have && lay.L.screenShifted[kUtLayPad] ? ", shifted onto the screen" : "", painted,
                 plateScreenSiteName());
        }
    }
    if (!InterlockedExchange(&g_btnStripLogged, 1)) {
        logT("buttons: strip drawn at origin=(%.1f,%.1f) scale=%.3f - the %d x %d pad, %d "
             "buttons in %d cells: screen x=%d..%d y=%d..%d; record x %d..%d y %d..%d %s; "
             "caption size %d, widest caption %d px in a %dx%d px cell; %d caption(s), %d "
             "button(s), 0 textures",
             ox, oy, s, P.fit.cols, P.fit.rows, P.n, P.fit.cols * P.fit.rows, ground.x,
             ground.x + ground.w - 1, ground.y, ground.y + ground.h - 1, P.ground.x,
             P.ground.x + P.ground.w - 1, P.ground.y, P.ground.y + P.ground.h - 1,
             P.user ? "in the user's rectangle" : "inside the joint free rectangle 102..422 / 25..69",
             fs, fs >= 6 ? widestTagPx(fs, caps) : 0, minW, minH, tags, painted);
    }
    return true;
}

// ---- layout_outline=1: the three rectangles in force, with their numbers ------------------------
// The overlay the [layout] keys are set against: each part's rectangle as its key takes it
// ("x,y,w,h", record px, window-local), white while derived, green when it is the user's, and a
// refused user rectangle in red with the reason. Solid quads and the game's font only; nothing is
// hit-tested and no engine memory is written.
const GdColor kOutDerived = {0.850f, 0.850f, 0.850f, 1.00f};
const GdColor kOutUser    = {0.247f, 0.831f, 0.416f, 1.00f};
const GdColor kOutRefused = {0.950f, 0.300f, 0.250f, 1.00f};

void outlineCaption(GdCanvas* canvas, int x, int y, int fs, const GdColor& col, const char* text) {
    const int w = textWidthPx(text, fs);
    fillRect(canvas, {x - 1, y - 1, w + 2, fs + 2}, kPadBack);
    drawText(canvas, x, y, fs, col, text, g_fontHeader);
}

gdut::Rect outlineScreen(const UtRect& r, float s, float ox, float oy) {
    const UtRectF f = utScaledRectF(r, s);
    return {iround(ox + f.x), iround(oy + f.y), iround(f.w), iround(f.h)};
}

// `screen` = the call from the screen site: it outlines only the parts pinned to the screen, with
// their key text; the window call (the Draw tail) outlines everything else, and a screen key that
// was refused in red at the part that stays in the window.
bool drawOutlineInner(GdCanvas* canvas, float ox, float oy, bool screen) {
    LiveLayout lay;
    if (!liveLayoutGet(&lay)) return false;
    const float s = lay.scale;
    const UtLayout& L = lay.L;
    const int line = padPx(s, 1);
    int fs = iround(10.0f * s);
    if (fs < 9) fs = 9;
    LiveView v;
    const bool view = liveViewInfo(&v);
    for (int p = 0; p < kUtLayParts; ++p) {
        const UtRect& r = L.rect[p];
        const bool user = L.user[p] != 0;
        const char* whose = user ? "user" : "derived";
        if ((L.screen[p] != 0) != screen) continue;   // the other site outlines this part
        if (screen) {
            // The screen key's own text, where it is drawn (L.screenPx: client px).
            const UtRectF& c = L.screenPx[p];
            const gdut::Rect dst = {iround(c.x), iround(c.y), iround(c.w), iround(c.h)};
            outlineRect(canvas, dst, line, kOutUser);
            char text[200];
            _snprintf_s(text, sizeof(text), _TRUNCATE,
                        "%s_screen=%s -> client %d,%d %dx%d%s (window-local %d,%d,%d,%d)",
                        utLayoutPartName(p), p == kUtLayPad ? g_cfg.padScreen : g_cfg.labelScreen,
                        dst.x, dst.y, dst.w, dst.h, L.screenShifted[p] ? " shifted" : "", r.x, r.y,
                        r.w, r.h);
            int ty = dst.y + dst.h + line + 2;
            if (ty + fs > lay.in.screenH && lay.in.screenH > 0) ty = dst.y - fs - line - 2;
            int tx = dst.x;
            const int tw = textWidthPx(text, fs);
            if (lay.in.screenW > 0 && tx + tw > lay.in.screenW) tx = lay.in.screenW - tw - 2;
            if (tx < 2) tx = 2;
            outlineCaption(canvas, tx, ty, fs, kOutUser, text);
            continue;
        }
        if (lay.in.screenSet[p] && L.screenWhy[p] != kUtLayoutOk &&
            L.screenWhy[p] != kUtLayoutUnset && r.w > 0 && r.h > 0) {
            // the screen key was refused: say so in red at the part that stayed in the window
            const gdut::Rect dst = outlineScreen(r, s, ox, oy);
            char text[200];
            _snprintf_s(text, sizeof(text), _TRUNCATE, "%s_screen=%s refused: it %s",
                        utLayoutPartName(p), p == kUtLayPad ? g_cfg.padScreen : g_cfg.labelScreen,
                        utLayoutWhyText(L.screenWhy[p]));
            outlineCaption(canvas, dst.x + 2, dst.y - fs - line - 2, fs, kOutRefused, text);
        }
        if (r.w > 0 && r.h > 0 && (p != kUtLayPad || L.pad.ok)) {
            const gdut::Rect dst = outlineScreen(r, s, ox, oy);
            outlineRect(canvas, dst, line, user ? kOutUser : kOutDerived);
            char text[160];
            int tx = dst.x + 2, ty = dst.y + 2;
            if (p == kUtLayGrid) {
                if (view) {
                    _snprintf_s(text, sizeof(text), _TRUNCATE,
                                "grid_%s %d,%d,%d,%d (%s) - %d x %d cells of %d x %d", lay.cover,
                                r.x, r.y, r.w, r.h, whose, v.cols, v.rows, v.cellW, v.cellH);
                } else {
                    _snprintf_s(text, sizeof(text), _TRUNCATE, "grid_%s %d,%d,%d,%d (%s)",
                                lay.cover, r.x, r.y, r.w, r.h, whose);
                }
                ty = dst.y + dst.h - fs - 2;
            } else if (p == kUtLayPad) {
                _snprintf_s(text, sizeof(text), _TRUNCATE,
                            "pad_%s %d,%d,%d,%d (%s) - %d x %d cells of %d x %d, caption %d",
                            lay.cover, r.x, r.y, r.w, r.h, whose, L.pad.cols, L.pad.rows,
                            L.pad.cellW, L.pad.cellH, L.pad.caption);
                ty = dst.y + dst.h + line + 1;
            } else {
                _snprintf_s(text, sizeof(text), _TRUNCATE, "label_%s %d,%d,%d,%d (%s) - size %d%s",
                            lay.cover, r.x, r.y, r.w, r.h, whose, L.label.size,
                            L.label.ok ? (L.label.bounded ? ", brought down" : "") : ", not drawn");
                tx = dst.x + dst.w - textWidthPx(text, fs) - 2;
                if (tx < dst.x + 2) tx = dst.x + 2;
                ty = dst.y + dst.h + line + 1;
            }
            outlineCaption(canvas, tx, ty, fs, user ? kOutUser : kOutDerived, text);
        }
        if (lay.in.user[p] && L.why[p] != kUtLayoutOk) {
            const UtRect& u = lay.user[p];
            const gdut::Rect dst = outlineScreen(u, s, ox, oy);
            outlineRect(canvas, dst, line, kOutRefused);
            char text[160];
            _snprintf_s(text, sizeof(text), _TRUNCATE, "%s_%s %d,%d,%d,%d refused: it %s%s%s",
                        utLayoutPartName(p), lay.cover, u.x, u.y, u.w, u.h,
                        utLayoutWhyText(L.why[p]), L.against[p] >= 0 ? " - the " : "",
                        L.against[p] >= 0 ? utLayoutPartName(L.against[p]) : "");
            outlineCaption(canvas, dst.x + 2, dst.y + 2, fs, kOutRefused, text);
        }
    }
    return true;
}

// ---- the layout editor (ut_layoutedit.h) ----------------------------------------------------------
// Opened by Ctrl+right-click on the pad or by the one-shot layout_edit=1, on a collection group.
// While it is open the page's boxes are parked (ut_live's applyLayout asks panelLayoutEditing),
// the plate is covered by a plain ground, and the grid (the shown group's cells as lines), the pad
// (its cells) and the label (a sample line) are drawn as rectangles with handles, with a
// SAVE / CANCEL / RESET toolbar at the top right of the pad that travels with it. The pad and the
// label can be dragged across the plate's edge onto the SCREEN (ut_layoutedit.h "the screen
// mode"): drawn there over a plain ground, their clicks handed over by the screen input
// (ut_plate's GetMouseEvent detour, over the grounds editPublishScreen publishes). SAVE writes
// grid_/pad_/label_<cover> (the active view's file) and pad_screen / label_screen (the main
// uniquetab.ini) in one configPersistStr call; a part left at its derived rectangle has its key
// removed, a part back in the window its screen key. The state lives on the thread that runs the
// Draw tail and the mouse slot (as the pad's own state does); panelLayoutEditing reads the two
// published values below from any thread.
// Two routes, one border (ut_layoutedit.h "TWO ROUTES, ONE BORDER"): a click INSIDE the caravan
// window reaches the editor through the window's mouse slot only - editPublishScreen publishes the
// window's client rect with the screen grounds every frame, and the screen input leaves the inside
// alone - and a click OUTSIDE through the screen input only. The window route's event space is
// proven by the in-window pad's probe or, in a world where the pad is pinned to the screen and that
// probe never runs, by the screen input's note of the same click (g_editNote).
UtEdit g_edit = {};
int g_editLatch = 0;                   // the one-shot key's latch (utEditOneShot)
char g_editCover[16] = {0};            // the cover key the editor writes, "438x627"
bool g_editRefusedSaid = false;        // one "SAVE refused" line per edit
volatile LONG g_editParkState = 0;     // 1 = not closed
volatile LONG g_editTickAt = 0;        // GetTickCount of the last tick
bool g_editPublished = false;          // the editor's screen grounds are published (ut_plate)
const int kEdMouseMove = 0;            // the engine's MOVE (ut_padinput.h kUtEvtMove)
UtEdPumpNote g_editNote = {};          // the screen input's copy of the last window DOWN
bool g_editWinProven = false;          // the window route's space is proven this edit
bool g_editWinRefusedSaid = false;     // one "a window click was refused" line per edit

// The screen the editor places a screen part against: the canvas the page is drawn on and the
// window's origin on it. The live layout's own (the one the page was resolved for) when it has
// one, else what the editor saw at the last Draw tail (editNoteView).
struct EditView {
    int known;
    int w, h;
    float ox, oy;
};
EditView g_editView = {};

const GdColor kEdGround = {0.024f, 0.024f, 0.024f, 1.00f};   // the plain ground, #060606
const GdColor kEdGhost  = {0.560f, 0.590f, 0.630f, 0.45f};   // the grid's cell lines
const GdColor kEdCell   = {0.227f, 0.247f, 0.278f, 0.80f};   // the pad's cells
const GdColor kEdDim    = {0.560f, 0.580f, 0.600f, 1.00f};   // a disabled caption

void editPublish() {
    InterlockedExchange(&g_editParkState, g_edit.state != kUtEdClosed ? 1 : 0);
    InterlockedExchange(&g_editTickAt, (LONG)g_edit.lastTick);
}

void editRectText(const UtRect& r, char* out, size_t cap) {
    _snprintf_s(out, cap, _TRUNCATE, "%d,%d,%d,%d", r.x, r.y, r.w, r.h);
}

void editNoteView(GdCanvas* canvas, float ox, float oy) {
    int w = 0, h = 0;
    if (canvas && g_gd.CanvasGetWidth && g_gd.CanvasGetHeight) {
        w = g_gd.CanvasGetWidth(canvas);
        h = g_gd.CanvasGetHeight(canvas);
    }
    g_editView.known = (w > 0 && h > 0) ? 1 : 0;
    g_editView.w = w;
    g_editView.h = h;
    g_editView.ox = ox;
    g_editView.oy = oy;
}

EditView editViewFor(const LiveLayout* lay) {
    if (lay && lay->in.screenKnown && lay->in.screenW > 0 && lay->in.screenH > 0)
        return EditView{1, lay->in.screenW, lay->in.screenH, lay->in.originX, lay->in.originY};
    return g_editView;
}

// Where a screen part takes the clicks outside the window, for the screen input (ut_plate's
// GetMouseEvent detour hands the clicks inside them to panelLayoutEditMouse while the editor is
// open): the screen pad with its toolbar, the screen label, in client px - and the
// caravan window's client rect, every frame the editor is open (the parts in the window included):
// the screen input leaves every point inside it to the window's own mouse slot. Retracted at the
// close.
void editRetractScreen() {
    if (!g_editPublished) return;
    g_editPublished = false;
    plateScreenRectsPublish(nullptr);
}

void editPublishScreen(float ox, float oy) {
    UtRectF pad, label;
    utEditScreenGrounds(g_edit, &pad, &label);
    const UtRectF win = utEditWindowClient(g_edit);
    if (!(win.w > 0.0f) && !(pad.w > 0.0f) && !(label.w > 0.0f)) {
        editRetractScreen();
        return;
    }
    static UtPadScreenRects r;   // the Draw tail's thread only; ~2.5 KB off the stack
    memset(&r, 0, sizeof(r));
    r.shown = 1;
    if (pad.w > 0.0f) r.pad = UtPadScreenRect{pad.x + ox, pad.y + oy, pad.w, pad.h};
    if (label.w > 0.0f) r.label = UtPadScreenRect{label.x + ox, label.y + oy, label.w, label.h};
    if (win.w > 0.0f) r.window = UtPadScreenRect{win.x + ox, win.y + oy, win.w, win.h};
    r.buttonCount = 0;   // the editor has no pad buttons: every click there is the editor's
    plateScreenRectsPublish(&r);
    g_editPublished = true;
}

void editProofReset() {
    g_editNote = UtEdPumpNote{};
    g_editWinProven = false;
    g_editWinRefusedSaid = false;
}

void editClosed(int why) {
    editPublish();
    editRetractScreen();
    editProofReset();
    g_editView.known = 0;   // the next open learns the screen afresh (a resolution may change)
    liveRelayoutVisible();   // the boxes come back on the next game-thread tick
    logI("layout editor: closed - %s", utEditWhyText(why));
}

// A part's numbers for the log and the page: "402,25,320,44" in the window, plus the key on the
// screen - "402,25,320,44 on the screen: pad_screen=tr,29,476,320,44".
void editPartText(int p, char* out, size_t cap) {
    const UtRect& r = g_edit.rect[p];
    if (p != kUtLayGrid && g_edit.scr[p]) {
        char k[48];
        if (utScreenRectFormat(utEditScreenKey(g_edit, p), k, sizeof(k)) <= 0) k[0] = 0;
        _snprintf_s(out, cap, _TRUNCATE, "%d,%d,%d,%d on the screen: %s_screen=%s", r.x, r.y, r.w,
                    r.h, utLayoutPartName(p), k);
        return;
    }
    editRectText(r, out, cap);
}

bool editRecheck() {
    LiveLayout lay;
    if (!liveLayoutGet(&lay)) return false;
    return utEditCheck(&g_edit, lay.in);
}

bool editOpen(const char* how, DWORD now) {
    if (g_edit.state != kUtEdClosed) return false;
    LiveLayout lay;
    if (!liveLayoutGet(&lay)) return false;
    LiveView v;
    if (!liveViewInfo(&v) || liveShownGroup(nullptr) < 0) return false;
    int cw = 0, ch = 0;
    if (!utCoverKeyParse(lay.cover, &cw, &ch)) return false;
    UtEditOpenIn o = {};
    for (int p = 0; p < kUtLayParts; ++p) {
        o.inForce[p] = lay.L.rect[p];
        o.def[p] = lay.in.def[p];
    }
    if (!lay.L.pad.ok) o.inForce[kUtLayPad] = UtRect{0, 0, 0, 0};
    o.padFallback = utPadDefault(g_cfg.padY, g_cfg.padH, g_cfg.padGap);
    o.plate = lay.in.plate;
    o.maxCellW = lay.in.maxCellW;
    o.maxCellH = lay.in.maxCellH;
    o.rowGap = lay.in.rowGap;
    o.cellW = v.cellW;
    o.cellH = v.cellH;
    o.scale = lay.scale;
    o.coverW = cw;
    o.coverH = ch;
    o.group = v.group;
    o.now = (unsigned)now;
    // a part pinned to the screen opens in screen mode; the screen it is placed against
    for (int p = kUtLayPad; p < kUtLayParts; ++p) o.screenIn[p] = lay.L.screen[p];
    const EditView ev = editViewFor(&lay);
    o.screenKnown = ev.known;
    o.screenW = ev.w;
    o.screenH = ev.h;
    o.originX = ev.ox;
    o.originY = ev.oy;
    if (!utEditOpen(&g_edit, o)) return false;
    strcpy_s(g_editCover, sizeof(g_editCover), lay.cover);
    g_editRefusedSaid = false;
    editProofReset();
    utEditCheck(&g_edit, lay.in);
    editPublish();
    liveRelayoutVisible();   // parks the boxes
    char gr[48], pd[96], lb[96];
    editPartText(kUtLayGrid, gr, sizeof(gr));
    editPartText(kUtLayPad, pd, sizeof(pd));
    editPartText(kUtLayLabel, lb, sizeof(lb));
    logI("layout editor: open (%s) for the %s cover at UI scale %.3f - grid %s, pad %s, label %s; "
         "drag a rectangle or a handle (the pad and the label also off the window, onto the "
         "screen), then SAVE, CANCEL or RESET beside the pad",
         how, g_editCover, lay.scale, gr, pd, lb);
    return true;
}

// SAVE: one configPersistStr call (utEditSavePlanOf): the grid's and every window part's
// <part>_<cover> line and the screen keys pad_screen / label_screen all go to the ACTIVE view's
// file (a custom game's own uniquetab.ini while it is active - ut_config routes them; the plan's
// `mainOnly` marks a screen key, whose removal pins the part to the window). The log names the
// file.
void editSave(DWORD now) {
    UtEditSavePlan plan;
    utEditSavePlanOf(g_edit, g_editCover, &plan);
    const char* k[kUtEdSaveKeysMax];
    const char* v[kUtEdSaveKeysMax];
    for (int i = 0; i < plan.n; ++i) {
        k[i] = plan.k[i].key;
        v[i] = plan.k[i].set ? plan.k[i].value : nullptr;
    }
    const bool written = configPersistStr(k, v, plan.n);
    utEditSaved(&g_edit, written ? 1 : 0, (unsigned)now);
    editPublish();
    // the active view's file: the game's own while a custom game is active, else the main
    char view[64];
    configActiveView(view, sizeof(view));
    const std::string game = configCustomKey(view);
    char viewFile[96];
    if (game.empty())
        _snprintf_s(viewFile, sizeof(viewFile), _TRUNCATE, "uniquetab.ini");
    else
        _snprintf_s(viewFile, sizeof(viewFile), _TRUNCATE, "mods\\%s\\uniquetab.ini",
                    game.c_str());
    if (!written) {
        logW("layout editor: SAVE could not rewrite %s - the editor stays open (CANCEL leaves it)",
             viewFile);
        return;
    }
    // "key=value, key=(removed: ...)"
    char lines[640] = {0};
    for (int i = 0; i < plan.n; ++i) {
        const UtEditSaveKey& s = plan.k[i];
        const size_t len = strlen(lines);
        _snprintf_s(lines + len, sizeof(lines) - len, _TRUNCATE, "%s%s=%s", len ? ", " : "",
                    s.key,
                    s.set ? s.value
                          : s.mainOnly ? "(removed: in the window)"
                                       : "(removed: the derived rectangle)");
    }
    logI("layout editor: SAVE - %s in one rewrite of %s%s; the page follows once the file is read "
         "back",
         lines, viewFile, game.empty() ? "" : " (the game's own)");
}

// The polled cursor, window-local screen px (the hover's own route).
bool editCursor(float ox, float oy, float* x, float* y) {
    POINT pt;
    HWND h = GetActiveWindow();
    if (!h) h = GetForegroundWindow();
    if (!h || !GetCursorPos(&pt) || !ScreenToClient(h, &pt)) return false;
    *x = (float)pt.x - ox;
    *y = (float)pt.y - oy;
    return true;
}

bool gameHasFocus() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// The one-shot key: consumed (written back to 0) when the editor really opens; on the game's own
// materials page it waits for a collection group.
void editKeyCheck(DWORD now) {
    const int key = g_cfg.layoutEdit;
    if (!utEditOneShot(key, &g_editLatch)) return;
    if (g_edit.state != kUtEdClosed || liveShownGroup(nullptr) < 0) return;
    if (!editOpen("layout_edit=1", now)) return;
    g_editLatch = 1;
    const char* k[1] = {"layout_edit"};
    const char* v[1] = {"0"};
    if (!configPersistStr(k, v, 1))
        logW("layout editor: layout_edit=1 could not be written back to 0 - set it by hand");
}

// THE EDIT BUTTON'S LATCH: a click on the pad's EDIT button (either route, either thread) stores
// its tick here; the pad's draw opens the editor on its next frame - the one-shot key's path, so
// both routes end on the thread the editor's state lives on. On the game's own materials page
// panelEditButton has asked for the last collection group first, and the latch waits until a
// collection group is on screen; it drops after kEditButtonWaitMs either way.
// THE EDITOR KEY'S LATCH ([keys] editor_open bound to a key): the window procedure's KEYDOWN
// stores its tick here (panelEditKey) and the same check opens the editor - the EDIT button's
// path and rules, its own latch so the log names which one asked.
volatile LONG g_editButtonAt = 0;   // GetTickCount of the click (never 0), 0 = none
volatile LONG g_editKeyAt = 0;      // GetTickCount of the editor_open key (never 0), 0 = none
const DWORD kEditButtonWaitMs = 2000;

void editLatchCheck(volatile LONG* latch, const char* who, const char* whose, DWORD now) {
    const LONG at = InterlockedCompareExchange(latch, 0, 0);
    if (!at) return;
    if (g_edit.state != kUtEdClosed) {   // open already (the editor chord the same frame)
        InterlockedCompareExchange(latch, 0, at);
        return;
    }
    if (now - (DWORD)at >= 2 * kEditButtonWaitMs) {   // set, then the pad stopped drawing
        if (InterlockedCompareExchange(latch, 0, at) == at)
            logD("layout editor: %s is %lu ms old at the pad's next frame - dropped", whose,
                 (unsigned long)(now - (DWORD)at));
        return;
    }
    if (liveShownGroup(nullptr) >= 0 && editOpen(who, now)) {
        InterlockedCompareExchange(latch, 0, at);
        return;
    }
    if (now - (DWORD)at >= kEditButtonWaitMs && InterlockedCompareExchange(latch, 0, at) == at)
        logD("layout editor: %s waited %lu ms and the editor did not open (%s) - dropped", whose,
             (unsigned long)(now - (DWORD)at),
             liveShownGroup(nullptr) < 0 ? "no collection group came on screen"
                                         : "no layout or cover to edit yet");
}

void editButtonCheck(DWORD now) {
    editLatchCheck(&g_editButtonAt, "the EDIT button", "the EDIT button's click", now);
    editLatchCheck(&g_editKeyAt, "the editor_open key", "the editor_open key", now);
}

// Either latch: its tick, and on the game's own materials page the last collection group first.
int editLatchSet(volatile LONG* latch) {
    const DWORD now = GetTickCount();
    InterlockedExchange(latch, (LONG)(now ? now : 1));
    if (liveShownGroup(nullptr) >= 0) return -1;
    // The game's own materials page: the editor edits a collection page's layout (never
    // padmat_), so the latch first goes back to the last collection group the pad showed.
    const int g = g_lastCollectionGroup;
    if (liveSelectGroup(g)) return g;
    InterlockedExchange(latch, 0);
    return -2;
}

int edPx(int rec, float s, float o) { return iround(o + floorf((float)rec * s + 0.5f)); }

void editDrawInner(GdCanvas* canvas, float ox, float oy) {
    const UtEdit& e = g_edit;
    const float s = e.scale;
    const int line = padPx(s, 1);
    int fs = iround(10.0f * s);
    if (fs < 9) fs = 9;
    fillRect(canvas, outlineScreen(e.plate, s, ox, oy), kEdGround);
    LiveLayout lay;
    const bool haveLay = liveLayoutGet(&lay);
    const int gap = haveLay ? lay.in.rowGap : kUtRowGap;
    // the grid: the shown group's cells as lines (cols + 1 verticals, two per row)
    char gridNote[96];
    gridNote[0] = 0;
    LiveView v;
    if (liveViewInfo(&v) && v.cellW > 0 && v.cellH > 0) {
        int cap = liveCapturedBoxes(nullptr, 0);
        if (cap < 1) cap = kUtFrameBoxFloor;
        const UtRect& gr = e.rect[kUtLayGrid];
        const UtGrid gf = utGridFit(gr, v.cellW, v.cellH, gap, cap);
        const int pitch = v.cellH + gap;
        const int y0 = edPx(gr.y, s, oy), y1 = edPx(gr.y + gf.rows * pitch - gap, s, oy);
        const int x0 = edPx(gr.x, s, ox), x1 = edPx(gr.x + gf.cols * v.cellW, s, ox);
        for (int c = 0; c <= gf.cols; ++c)
            fillRect(canvas, {edPx(gr.x + c * v.cellW, s, ox), y0, line, y1 - y0}, kEdGhost);
        for (int r = 0; r < gf.rows; ++r) {
            fillRect(canvas, {x0, edPx(gr.y + r * pitch, s, oy), x1 - x0, line}, kEdGhost);
            fillRect(canvas, {x0, edPx(gr.y + r * pitch + v.cellH, s, oy) - line, x1 - x0, line},
                     kEdGhost);
        }
        _snprintf_s(gridNote, sizeof(gridNote), _TRUNCATE, " - %d x %d cells of %d x %d%s",
                    gf.cols, gf.rows, v.cellW, v.cellH, gf.capped ? " (capped)" : "");
    }
    // a part on the screen gets the plain ground under it too (it is drawn over the world)
    for (int p = kUtLayPad; p < kUtLayParts; ++p)
        if (e.scr[p]) fillRect(canvas, outlineScreen(e.rect[p], s, ox, oy), kEdGround);
    // the pad: its cells
    if (haveLay) {
        const UtPadFit f = utPadFit(e.rect[kUtLayPad], lay.in.buttons, lay.in.padGap, s,
                                    lay.in.captionMax, lay.in.widest, lay.in.widestCtx);
        for (int i = 0; f.ok && i < f.buttons; ++i)
            fillRect(canvas, outlineScreen(utPadCellRect(f, i), s, ox, oy), kEdCell);
    }
    // the label: a sample line at the size it would get
    const UtLabelFit lf = utLabelFit(e.rect[kUtLayLabel], panelLabelSizeInForce(), s);
    if (lf.ok) {
        char sample[160];
        stripColourCodesTo(liveGroupLabel(e.group), sample, sizeof(sample));
        utTmClipToWidth(sample, lf.limitPx, labelClipWidthPx, &lf.size);
        drawText(canvas, edPx(lf.x, s, ox), edPx(lf.top, s, oy), lf.size, kCaption, sample,
                 labelFont());
    }
    // the outlines and their handles; their numbers go to the block at the bottom of the page
    const int hs = utEditHandlePx(s);
    char note[kUtLayParts][200];
    const GdColor* noteCol[kUtLayParts];
    for (int p = 0; p < kUtLayParts; ++p) {
        const UtRect& r = e.rect[p];
        const bool bad = e.partWhy[p] != kUtLayoutOk;
        const bool user = !utRectEq(r, e.def[p]);
        const GdColor& col = bad ? kOutRefused : user ? kOutUser : kOutDerived;
        const gdut::Rect dst = outlineScreen(r, s, ox, oy);
        const bool dragged = e.state == kUtEdDrag && e.part == p;
        outlineRect(canvas, dst, dragged ? 2 * line : line, col);
        const int xs[3] = {dst.x, dst.x + dst.w / 2, dst.x + dst.w};
        const int ys[3] = {dst.y, dst.y + dst.h / 2, dst.y + dst.h};
        for (int j = 0; j < 3; ++j)
            for (int i = 0; i < 3; ++i)
                if (i != 1 || j != 1) fillRect(canvas, {xs[i] - hs / 2, ys[j] - hs / 2, hs, hs}, col);
        char* t = note[p];
        noteCol[p] = &col;
        if (p != kUtLayGrid && e.scr[p]) {
            char k[48], words[160];
            if (utScreenRectFormat(utEditScreenKey(e, p), k, sizeof(k)) <= 0) k[0] = 0;
            utEditRefusal(e, p, words, sizeof(words));
            if (bad)
                _snprintf_s(t, sizeof(note[p]), _TRUNCATE, "%s_screen %s - %s", utLayoutPartName(p),
                            k, words);
            else
                _snprintf_s(t, sizeof(note[p]), _TRUNCATE, "%s_screen %s (on the screen)",
                            utLayoutPartName(p), k);
        } else if (bad) {
            _snprintf_s(t, sizeof(note[p]), _TRUNCATE, "%s_%s %d,%d,%d,%d - it %s%s%s",
                        utLayoutPartName(p), g_editCover, r.x, r.y, r.w, r.h,
                        utLayoutWhyText(e.partWhy[p]), e.against[p] >= 0 ? ": the " : "",
                        e.against[p] >= 0 ? utLayoutPartName(e.against[p]) : "");
        } else {
            _snprintf_s(t, sizeof(note[p]), _TRUNCATE, "%s_%s %d,%d,%d,%d%s%s", utLayoutPartName(p),
                        g_editCover, r.x, r.y, r.w, r.h, user ? "" : " (derived)",
                        p == kUtLayGrid ? gridNote : "");
        }
    }
    // the toolbar
    static const char* const kEdTags[kUtEdButtons] = {"SAVE", "CANCEL", "RESET"};
    static const UtCaptions kEdCaps = {kEdTags, kUtEdButtons};
    float cx = -1.0f, cy = -1.0f;
    UtEdHit hot = {kUtEdHitNone, -1, 0, -1};
    const bool haveCur = editCursor(ox, oy, &cx, &cy);
    if (haveCur) hot = utEditHitTest(e, cx, cy);
    for (int b = 0; b < kUtEdButtons; ++b) {
        const gdut::Rect dst = outlineScreen(e.tool[b], s, ox, oy);
        // CANCEL is never off: it answers in every state (utEditLeftDown)
        const bool off = b != kUtEdCancel && (e.state != kUtEdOpen || (b == kUtEdSave && !e.ok));
        const bool over = (hot.kind == kUtEdHitButton && hot.button == b) ||
                          (b == kUtEdCancel && e.state != kUtEdOpen && haveCur &&
                           utEditToolbarAt(e, cx, cy) == kUtEdCancel);
        fillRect(canvas, dst, off ? kBtnDown : over ? kBtnHover : kBtnBg);
        if (dst.h > 2 * line) {
            fillRect(canvas, {dst.x, dst.y, dst.w, line}, kLineTop);
            fillRect(canvas, {dst.x, dst.y + dst.h - line, dst.w, line}, kLineBot);
        }
        const int cs =
            utCaptionSize(dst.w, dst.h, panelCaptionMaxInForce(), widestCaptionLivePx, &kEdCaps);
        if (cs >= kUtCaptionMin) {
            const int tw = captionTextPx(kEdTags[b], cs);
            drawText(canvas, dst.x + (dst.w - tw) / 2, dst.y + (dst.h - cs) / 2, cs,
                     off ? kEdDim : kCaption, kEdTags[b], captionFont());
        }
    }
    // the hint line, at the bottom of the page
    char hint[160];
    if (e.state == kUtEdSaving)
        _snprintf_s(hint, sizeof(hint), _TRUNCATE, "saved - waiting for uniquetab.ini to be read back");
    else if (!e.ok)
        _snprintf_s(hint, sizeof(hint), _TRUNCATE,
                    "SAVE is off while a rectangle is red - move or resize it");
    else if (utEditScreenHint(e))
        _snprintf_s(hint, sizeof(hint), _TRUNCATE, "%s", utEditScreenHint(e));
    else
        _snprintf_s(hint, sizeof(hint), _TRUNCATE,
                    "layout editor - drag a rectangle or a handle; SAVE writes the %s layout",
                    g_editCover);
    // the bottom block: grid, pad and label with their numbers (in their outline's colour), then
    // the hint line
    const int bx = edPx(e.plate.x, s, ox) + hs;
    int by = edPx(e.plate.y + e.plate.h, s, oy) - fs - hs;
    outlineCaption(canvas, bx, by, fs, kCaption, hint);
    for (int p = kUtLayParts - 1; p >= 0; --p) {
        by -= fs + 3;
        outlineCaption(canvas, bx, by, fs, *noteCol[p], note[p]);
    }
}

// Once a frame from the pad's draw: the one-shot key, then (open) the tick and the editor's own
// draw. True = the editor drew this frame and the pad does not.
bool editTickDraw(GdCanvas* canvas, float ox, float oy) {
    const DWORD now = GetTickCount();
    // The screen a part may be dragged onto: read only while the editor is open (or the one-shot
    // key may open it this frame), so a player who never edits gets no extra engine call per
    // frame. A Ctrl+right-click open learns it at the first frame (utEditTick takes it then).
    if (g_cfg.layoutEdit || InterlockedCompareExchange(&g_editButtonAt, 0, 0) ||
        InterlockedCompareExchange(&g_editKeyAt, 0, 0))
        editNoteView(canvas, ox, oy);
    editKeyCheck(now);
    editButtonCheck(now);
    if (g_edit.state == kUtEdClosed) return false;
    editNoteView(canvas, ox, oy);
    LiveLayout lay;
    const bool layOk = liveLayoutGet(&lay);
    UtEdTick t = {};
    t.now = (unsigned)now;
    t.group = liveShownGroup(nullptr);
    t.scale = layOk ? lay.scale : 0.0f;
    t.focused = gameHasFocus() ? 1 : 0;
    t.layoutOk = layOk ? 1 : 0;
    if (layOk) utCoverKeyParse(lay.cover, &t.coverW, &t.coverH);
    t.physL = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 1 : 0;
    t.physR = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) ? 1 : 0;
    t.haveCursor = editCursor(ox, oy, &t.cx, &t.cy) ? 1 : 0;
    t.matches = layOk && utEditMatches(g_edit, lay.L.rect, lay.L.screen) ? 1 : 0;
    const EditView ev = editViewFor(layOk ? &lay : nullptr);
    t.screenKnown = ev.known;
    t.screenW = ev.w;
    t.screenH = ev.h;
    t.originX = ev.ox;
    t.originY = ev.oy;
    const int was = g_edit.state;
    const int part = g_edit.part;
    const UtRect before = g_edit.start;
    int moved = 0;
    const int why = utEditTick(&g_edit, t, &moved);
    editPublish();
    if (why != kUtEdWhyNone) {
        editClosed(why);
        return false;
    }
    LiveView v;
    if (liveViewInfo(&v)) utEditSetCell(&g_edit, v.cellW, v.cellH, layOk ? lay.in.rowGap : kUtRowGap);
    if (moved && layOk) utEditCheck(&g_edit, lay.in);
    if (was == kUtEdDrag && g_edit.state == kUtEdOpen && part >= 0 && part < kUtLayParts) {
        char a[48], b[48];
        editRectText(before, a, sizeof(a));
        editRectText(g_edit.rect[part], b, sizeof(b));
        logD("layout editor: the %s %s -> %s%s%s", utLayoutPartName(part), a, b,
             part != kUtLayGrid && g_edit.scr[part] ? " (on the screen)" : "",
             g_edit.ok ? "" : " (a rectangle is refused, SAVE is off)");
    }
    editDrawInner(canvas, ox, oy);
    editPublishScreen(ox, oy);
    return true;
}

// SEH cannot share a function with C++ unwinding (C2712), so the guard is its own frame and
// everything it touches is a call. A fault latches the MOD-OWNED session switch and reports
// itself to ut_plate exactly the way the label does.
bool groupButtonsGuarded(GdCanvas* canvas, float ox, float oy) {
    bool drew = false;
    __try {
        if (editTickDraw(canvas, ox, oy)) {
            drew = true;   // the layout editor is open: it draws instead of the pad
        } else {
            drew = drawButtonsInner(canvas, ox, oy, false, nullptr);
            if (g_cfg.layoutOutline) drawOutlineInner(canvas, ox, oy, false);
        }
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
    UtRect r;
    UtPadFit f;
    return padInForce(&r, &f) ? f.cols : 0;
}
int panelButtonRecX0() {
    UtRect r;
    UtPadFit f;
    return padInForce(&r, &f) ? r.x : 0;
}
int panelButtonRecX1() {
    UtRect r;
    UtPadFit f;
    return padInForce(&r, &f) ? r.x + r.w - 1 : 0;
}
int panelButtonRecY0() {
    UtRect r;
    UtPadFit f;
    return padInForce(&r, &f) ? r.y : 0;
}
int panelButtonRecY1() {
    UtRect r;
    UtPadFit f;
    return padInForce(&r, &f) ? r.y + r.h - 1 : 0;
}

const UtCaptions* panelButtonCaptionsFor(int paged, int edit) {
    const CapTable* t = capTable();
    if (!t) return &kDefaultCaps;
    if (paged) return edit ? &t->pagedEdit : &t->paged;
    return edit ? &t->allEdit : &t->all;
}

const UtCaptions* panelButtonCaptions() { return panelButtonCaptionsFor(0, g_cfg.editButton ? 1 : 0); }

int widestCaptionLivePx(int size, const void* ctx) {
    const UtCaptions* c = (const UtCaptions*)ctx;
    int w = 0;
    for (int i = 0; c && i < c->n; ++i) {
        const int t = captionTextPx(c->tags[i], size);
        if (t > w) w = t;
    }
    return w;
}

long panelMeasureGen() { return InterlockedCompareExchange(&g_measureGen, 0, 0); }

int panelPadButtonCount() {
    const TextWorld* w = textWorldNow();
    if (w) return w->buttons;
    const int g = liveGroupTableGen() > 0 ? liveGroupCount() : 0;
    return g > 0 ? g : kUtButtonCount - 2;
}

int panelButtonOfGroup(int group) {
    const int b = group >= 0 ? buttonOfGroup(group) : -1;   // 1-based on the pad, -1 = on none
    return b > 0 ? b - 1 : -1;
}

int panelButtonGroupsOf(int group, int* out, int cap) {
    if (group < 0) return 0;
    const TextWorld* w = textWorldNow();
    if (!w) {   // one button per group
        if (out && cap > 0) out[0] = group;
        return 1;
    }
    const int b = buttonOfGroup(group) - 1;
    if (b < 0 || (size_t)b >= w->groupsOf.size()) return 0;
    const std::vector<int>& gs = w->groupsOf[(size_t)b];
    for (size_t i = 0; out && i < gs.size() && (int)i < cap; ++i) out[i] = gs[i];
    return (int)gs.size();
}

long panelButtonMapGen() {
    const TextWorld* w = textWorldNow();
    return w ? w->gen : 0;
}

int panelButtonSizeCap(int logical, int capMax) {
    const TextWorld* w = textWorldNow();
    if (!w || logical < 0 || (size_t)logical >= w->sizes.size() || w->sizes[(size_t)logical] < 0) return capMax;
    return w->sizes[(size_t)logical];
}

const char* panelButtonFontPath(int logical) {
    const TextWorld* w = textWorldNow();
    if (!w || logical < 0 || (size_t)logical >= w->fonts.size()) return "";
    return w->fonts[(size_t)logical].c_str();
}
// The font that draws logical button `logical`'s caption, and its width there.
const GdFont* panelButtonCaptionFont(int logical) {
    const int i = g_fontsTried ? buttonFontOf(logical) : -1;
    return i >= 0 ? g_btnFont[i].font : captionFont();
}
int panelButtonCaptionTextPx(int logical, const char* s, int size) {
    const int i = g_fontsTried ? buttonFontOf(logical) : -1;
    if (i >= 0) {
        ButtonFont& bf = g_btnFont[i];
        if (bf.cache.lazy && s && *s && size >= 1 && size <= kUtTmMaxSize && bf.cache.sizeStyle[size] == -1)
            fontCacheSizeSeh(&bf.cache, bf.font, size);   // a dynamic font: this size, once
        const int px = utFontCacheWidth(bf.cache, s, size, liveExtraGlyph, (void*)bf.font);
        if (px >= 0) return px;
    }
    return captionTextPx(s, size);
}
// A button's caption size: the pad's shared size `fs`, or - with a size = line - the largest that
// fits this one caption in the cell (boxW x boxH screen px, the pad's own rule) up to that size.
int panelButtonCaptionSize(int logical, int fs, int boxW, int boxH) {
    const int own = panelButtonSizeCap(logical, -1);
    if (own < 0) return fs;
    const char* one[1] = {capText(logical)};
    const UtCaptions c1 = {one, 1};
    const int fit = utCaptionSize(boxW, boxH, own, widestCaptionLivePx, &c1);
    return fit >= kUtCaptionMin ? fit : fs;
}
// Does any group of logical button `logical` carry the game's search mark?
bool panelButtonSearchMarked(int logical) {
    const TextWorld* w = textWorldNow();
    if (!w) return logical > 0 && plateSearchMarked(logical - 1);
    const int b = logical - 1;
    if (b < 0 || (size_t)b >= w->groupsOf.size()) return false;
    const std::vector<int>& gs = w->groupsOf[(size_t)b];
    return utUnionMarked(gs.data(), (int)gs.size(), plateSearchMarked);
}

void panelTextFileTick() { textFilePoll(); }

void panelTextStartup(char* yes, size_t yesCap, char* no, size_t noCap, bool* yesFromFile,
                      bool* noFromFile, char* roll, size_t rollCap) {
    textFilePoll();
    try {
        const TextFileState* fs = g_textFile;
        utt::UtTextContext c;
        c.file = fs ? &fs->file : nullptr;
        const TextGlyphProbe none = {};   // before a world: printable ASCII only
        c.glyph = textGlyph;
        c.glyphCtx = &none;
        const utt::UtTextPick y = utt::utTextResolve(c, utt::kTextTooltipYes, "");
        const utt::UtTextPick n = utt::utTextResolve(c, utt::kTextTooltipNo, "");
        const utt::UtTextPick r = utt::utTextResolve(c, utt::kTextTooltipRoll, "");
        if (yes && yesCap) strcpy_s(yes, yesCap, y.text.size() < yesCap ? y.text.c_str() : "");
        if (no && noCap) strcpy_s(no, noCap, n.text.size() < noCap ? n.text.c_str() : "");
        if (roll && rollCap) strcpy_s(roll, rollCap, r.text.size() < rollCap ? r.text.c_str() : "");
        if (yesFromFile) *yesFromFile = y.source <= utt::kSrcCommon;
        if (noFromFile) *noFromFile = n.source <= utt::kSrcCommon;
    } catch (...) {
        if (yes && yesCap) yes[0] = 0;
        if (no && noCap) no[0] = 0;
        if (roll && rollCap) roll[0] = 0;
    }
}

int panelCaptionMaxInForce() {
    const utt::UtTextStyle* st = textStyle();
    return st ? utt::utStyleSizeOr(*st, utt::kStyleSizeCaptionMax, g_cfg.padCaptionMax)
              : g_cfg.padCaptionMax;
}

int panelLabelSizeInForce() {
    const utt::UtTextStyle* st = textStyle();
    return st ? utt::utStyleSizeOr(*st, utt::kStyleSizeLabel, g_cfg.plateLabelSize)
              : g_cfg.plateLabelSize;
}

const UtCaptions* panelButtonCaptionsPaged() {
    return panelButtonCaptionsFor(1, g_cfg.editButton ? 1 : 0);
}

int panelButtonRects(UtButtonRect* out, int* logical, int cap, float s) {
    if (!out || cap <= 0) return 0;
    PadPlan P;
    if (!padPlan(&P, s)) return 0;
    const int n = P.n < cap ? P.n : cap;
    for (int i = 0; i < n; ++i) {
        out[i] = P.r[i];
        if (logical) logical[i] = P.logical[i];
    }
    return n;
}

// The probe band is the pad's ROW BANDS in y - never the whole plate - or the pad starts eating
// clicks in empty plate area. In x it is the joint rectangle for today's pad (the user's own
// rectangle for a user pad), deliberately wider than the buttons, because a probe has to see the
// events that MISS them too.
bool panelButtonsInBand(float lx, float ly, float s) {
    PadPlan P;
    if (!padPlan(&P, s)) return false;
    if (!(lx >= (float)P.bandX0 * s && lx < (float)P.bandX1 * s)) return false;
    for (int i = 0; i < P.n; ++i) {
        if (P.logical[i] < 0) continue;
        if (ly >= P.r[i].y && ly < P.r[i].y + P.r[i].h) return true;   // y only: the row bands
    }
    return false;
}

// The LOGICAL button (ut_padpage.h) under the window-local scaled point, or -1.
int panelButtonHit(float lx, float ly, float s) {
    if (g_edit.state != kUtEdClosed) return -1;   // the layout editor takes the page's clicks
    PadPlan P;
    if (!padPlan(&P, s)) return -1;
    for (int i = 0; i < P.n; ++i) {
        const UtButtonRect& r = P.r[i];
        if (P.logical[i] < 0) continue;
        if (lx >= r.x && lx < r.x + r.w && ly >= r.y && ly < r.y + r.h) return P.logical[i];
    }
    return -1;
}

// The game's own materials page: true when the pad is on screen there and the window-local scaled
// point lies inside its rectangle (the ground between the buttons included) - the point is the
// pad's, not the box's under it (ut_padmat.h utPadLeftDown; ut_plate.cpp also puts back the
// engine's hovered-box answer there, so a covered box shows no tooltip).
bool panelMatPadAt(float lx, float ly, float s) {
    if (g_edit.state != kUtEdClosed) return false;
    if (!panelButtonsDrawnRecently() || liveShownGroup(nullptr) != -1) return false;
    PadPlan P;
    if (!padPlan(&P, s) || !P.mat) return false;
    return utRectFHas(utScaledRectF(P.ground, s), lx, ly);
}

// ---- the layout editor's input and state (see "the layout editor" above) ------------------------
bool panelLayoutEditOpen() { return InterlockedCompareExchange(&g_editParkState, 0, 0) != 0; }

// The editor is mid-drag and was ticked recently (ut_plate's screen input adopts a drag that began
// in the window: it clears that drag's held bytes everywhere and hands its MOVEs and its UP to the
// editor). Read on the thread of the Draw tail and the mouse slot, where g_edit lives.
bool panelLayoutEditDragging() {
    return g_edit.state == kUtEdDrag && GetTickCount() - (DWORD)g_edit.lastTick <= kUtEdGoneMs;
}

bool panelLayoutEditing() {
    if (!InterlockedCompareExchange(&g_editParkState, 0, 0)) return false;
    const DWORD at = (DWORD)InterlockedCompareExchange(&g_editTickAt, 0, 0);
    return GetTickCount() - at <= kUtEdGoneMs;
}

// The point is WINDOW-LOCAL screen px in both routes: the window's mouse slot (ut_plate editMouse,
// the event minus the window's offset) and the screen input (ut_plate scrEditor, the event minus
// the Draw origin) - a part on the screen has a window-local rectangle too (off the plate,
// negative as well), so one hit test serves both.
// The window route's LEFT DOWN with the in-window probe unproven (the pad pinned to the
// screen all world): proven by the screen input's note of the same click, once per edit.
bool editWindowSpaceOk(float lx, float ly, DWORD now) {
    if (g_editWinProven) return true;
    float dx = 0.0f, dy = 0.0f;
    const bool had = g_editNote.valid != 0;
    if (utEditNoteProves(&g_editNote, lx, ly, (unsigned)now, &dx, &dy)) {
        g_editWinProven = true;
        logD("layout editor: a click in the caravan window is proven by the screen input's copy of "
             "it (delta=(%.1f,%.1f) px) - the window's clicks reach the editor for the rest of this "
             "edit (the in-window pad's own probe has not run this world)", dx, dy);
        return true;
    }
    if (!g_editWinRefusedSaid) {
        g_editWinRefusedSaid = true;
        if (had)
            logD("layout editor: a click in the caravan window was refused - its point is not the "
                 "screen input's copy of it (delta=(%.1f,%.1f) px, or older than %u ms); only "
                 "CANCEL answers until a click is proven", dx, dy, kUtEdNoteMs);
        else
            logD("layout editor: a click in the caravan window was refused - the window's event "
                 "space is not proven this world (no in-window pad probe, no screen-input copy of "
                 "the click); only CANCEL answers until a click is proven");
    }
    return false;
}

bool panelLayoutEditMouse(int type, float lx, float ly, bool cursorEmpty, bool inBox, bool proven) {
    const DWORD now = GetTickCount();
    if (type == kUtEvtEditorNote) {
        // The screen input passed a LEFT DOWN inside the window untouched (proven client
        // px minus the Draw origin); the window's mouse slot gets the same click next.
        if (g_edit.state != kUtEdClosed) utEditNoteDown(&g_editNote, lx, ly, (unsigned)now);
        return false;
    }
    if (type == kUtMouseRightDown) {
        // The editor chord ([keys] editor_open, Ctrl+right-click by default) on the pad's ground
        // (in the window or pinned to the screen), or on a label pinned to the screen, on a
        // collection group, with the same rules as a pad click: the pad drawn, an empty cursor,
        // no live box under it, the space proven. Both routes decided the chord already; this is
        // the recheck at the moment of opening (editor_open bound to a key: no click opens).
        if (g_edit.state != kUtEdClosed || !cursorEmpty || inBox || !proven) return false;
        const UtKeyBind chord = g_cfg.keys.editorOpen;
        const int mods = utKeyModsOf((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0,
                                     (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0,
                                     (GetAsyncKeyState(VK_MENU) & 0x8000) != 0);
        if (!utKeyClickMatches(chord, type, mods)) return false;
        if (!panelButtonsDrawnRecently() || liveShownGroup(nullptr) < 0) return false;
        const float s = liveUiScale();
        char how[64], text[40];
        utKeyText(chord, text, sizeof(text));
        PadPlan P;
        if (padPlan(&P, s) && !P.mat && utRectFHas(utScaledRectF(P.ground, s), lx, ly)) {
            _snprintf_s(how, _TRUNCATE, "%s on the pad", text);
            return editOpen(how, now);
        }
        LiveLayout lay;
        if (liveLayoutGet(&lay) && lay.L.screen[kUtLayLabel] &&
            utRectFHas(utScaledRectF(lay.L.rect[kUtLayLabel], s), lx, ly)) {
            _snprintf_s(how, _TRUNCATE, "%s on the label", text);
            return editOpen(how, now);
        }
        return false;
    }
    if (g_edit.state == kUtEdClosed) return false;
    if (type == kEdMouseMove) {
        // The screen input hands over the MOVEs of an editor press (outside the window too): the
        // drag follows them as it follows the polled cursor. Never consumed here.
        if (g_edit.state == kUtEdDrag && utEditDragTo(&g_edit, lx, ly)) editRecheck();
        return false;
    }
    if (type == kUtMouseLeftUp) {
        const int part = g_edit.part;
        const UtRect before = g_edit.start;
        if (utEditLeftUp(&g_edit, lx, ly)) {
            editRecheck();
            if (part >= 0 && part < kUtLayParts) {
                char a[48], b[48];
                editRectText(before, a, sizeof(a));
                editRectText(g_edit.rect[part], b, sizeof(b));
                logD("layout editor: the %s %s -> %s%s%s", utLayoutPartName(part), a, b,
                     part != kUtLayGrid && g_edit.scr[part] ? " (on the screen)" : "",
                     g_edit.ok ? "" : " (a rectangle is refused, SAVE is off)");
            }
        }
        return false;   // a LEFT UP is never consumed
    }
    if (type != kUtMouseLeftDown) return false;
    // A drag still open at a LEFT DOWN lost its release: utEditLeftDown ends it - said here, as
    // the tick would have said it.
    const int dragPart = g_edit.state == kUtEdDrag ? g_edit.part : -1;
    const UtRect dragFrom = g_edit.start;
    // The space: the caller's proof (the in-window probe; the screen input always proves its own),
    // else the window route proven against the screen input's note. Asked only for a
    // click that could act - CANCEL needs no proof.
    const bool spaceOk = proven || (cursorEmpty && !inBox && editWindowSpaceOk(lx, ly, now));
    const int act = utEditLeftDown(&g_edit, lx, ly, cursorEmpty && !inBox && spaceOk ? 1 : 0,
                                   (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 1 : 0,
                                   (GetAsyncKeyState(VK_RBUTTON) & 0x8000) ? 1 : 0,
                                   GetSystemMetrics(SM_SWAPBUTTON) ? 1 : 0);
    if (dragPart >= 0 && dragPart < kUtLayParts && g_edit.state != kUtEdDrag) {
        char a[48], b[48];
        editRectText(dragFrom, a, sizeof(a));
        editRectText(g_edit.rect[dragPart], b, sizeof(b));
        logD("layout editor: the %s %s -> %s%s - its release was never seen, the next click ended "
             "the drag", utLayoutPartName(dragPart), a, b,
             g_edit.scr[dragPart] ? " (on the screen)" : "");
    }
    switch (act) {
        case kUtEdActDrag:
            return true;
        case kUtEdActSave:
            editSave(now);
            return true;
        case kUtEdActSaveRefused:
            if (!g_editRefusedSaid) {
                g_editRefusedSaid = true;
                for (int p = 0; p < kUtLayParts; ++p) {
                    char words[160];
                    if (utEditRefusal(g_edit, p, words, sizeof(words)) <= 0) continue;
                    logI("layout editor: SAVE is off - %s%s", words,
                         p != kUtLayGrid && g_edit.scr[p] ? " (on the screen)" : "");
                    break;
                }
            }
            return true;
        case kUtEdActCancel:
            editClosed(g_edit.why);   // CANCEL, or (while saving) "saved, not read back yet"
            return true;
        case kUtEdActReset:
            editRecheck();
            logI("layout editor: RESET - the derived grid, pad and label are back in the window "
                 "(SAVE removes the three %s keys and pad_screen / label_screen)", g_editCover);
            return true;
        default:
            return false;
    }
}

// A click is ABSOLUTE - one button, one group - so this is a lookup and nothing else. NEXT is a
// group click too: the first group of the next pad page.
int panelButtonTarget(int index, const char** label) {
    if (label) *label = "?";
    UtPadPages pg;
    LiveLayout lay;
    if (liveLayoutGet(&lay)) {
        pg = lay.pages;
    } else {
        const int g = panelPadButtonCount();
        pg = UtPadPages{g, g, 1, g + 2, 0};
    }
    const TextWorld* w = textWorldNow();
    if (index < 0 || index >= utPadLogicalCount(pg)) return -2;
    // The OWN button toggles the owned-only filter and selects no group.
    if (index == utPadOwnIndex(pg)) {
        if (label) *label = "owned-only filter";
        return kUtButtonFilterTarget;
    }
    // The EDIT button opens the layout editor and selects no group (panelEditButton).
    if (index == utPadEditIndex(pg)) {
        if (!pg.edit) return -2;
        if (label) *label = "the layout editor";
        return kUtButtonEditTarget;
    }
    if (index == utPadNextIndex(pg)) {
        if (pg.pages <= 1) return -2;   // an unpaged pad has no NEXT
        const int shown = liveShownGroup(nullptr);
        const int b =
            utPadNextTarget(pg, utPadPageOf(pg, padIndexOfGroup(shown >= 0 ? shown : g_lastCollectionGroup)));
        const int g = w && (size_t)b < w->groupsOf.size() && !w->groupsOf[(size_t)b].empty()
                          ? w->groupsOf[(size_t)b][0]
                          : b;
        if (label) *label = liveGroupLabel(g);
        return g;
    }
    // A button of several groups shows them as ONE page (ut_live's union, keyed by its first
    // group): a click selects that first group, every time - nothing to cycle through.
    int group = index - 1;
    if (w) {
        if ((size_t)group >= w->groupsOf.size() || w->groupsOf[(size_t)group].empty()) return -2;
        const std::vector<int>& gs = w->groupsOf[(size_t)group];
        group = gs[0];
        if (gs.size() > 1 && (size_t)group < w->names.size()) {
            if (label) *label = w->names[(size_t)group].c_str();   // the button's name
            return group;
        }
    }
    if (label) *label = liveGroupLabel(group);
    return group;
}

// The button's own caption, for a log line. Never null.
const char* panelButtonTag(int index) { return capText(index); }

int panelEditButton() { return editLatchSet(&g_editButtonAt); }

int panelEditKey() {
    // The pad's draw consumes the latch: with no pad on screen (group_buttons=0, the pad off)
    // the key is not the mod's.
    if (!panelButtonsDrawnRecently()) return -3;
    return editLatchSet(&g_editKeyAt);
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
    g_fontRollover = nullptr;
    g_btnHover = -1;
    InterlockedExchange(&g_btnStripLogged, 0);
    InterlockedExchange(&g_btnBlankLogged, 0);
    InterlockedExchange(&g_matReset, 1);
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
    UtRect pr = {0, 0, 0, 0};
    UtPadFit pf = {};
    const bool ok = padInForce(&pr, &pf);
    const UtCaptions* caps = panelButtonCaptions();
    logD("buttons: group_buttons=%d pad_y=%d pad_h=%d pad_gap=%d - %d category buttons in a "
         "%d x %d pad of %d px cells (the strip arms in game only)",
         g_cfg.groupButtons, g_cfg.padY, g_cfg.padH, g_cfg.padGap, caps->n, ok ? pf.cols : 9,
         ok ? pf.rows : 3, ok ? pf.cellW : 34);
    try {
        std::string tags;
        for (int i = 0; i < caps->n; ++i) {
            if (i) tags += ' ';
            tags += caps->tags[i];
        }
        const int edit = g_cfg.editButton ? 1 : 0;
        logD("buttons: strip order (index 0 = the vanilla materials page, then groups 0..%d, "
             "then OWN = the owned-only filter%s): %s",
             caps->n - 3 - edit, edit ? ", EDIT = the layout editor (drawn before OWN)" : "",
             tags.c_str());
    } catch (...) {
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
        // n1 > 0 already means padInForce succeeded, but the zero-init makes that independent
        // of this function's control flow rather than a fact a reader has to re-derive. Before a
        // UI scale is known (the main menu) this is today's pad from the three [advanced] keys.
        const int pitch = pf.cellH + pf.gap;
        logD("buttons: geometry accepted - record x %d..%d y %d..%d, %d rows of %d cells at "
             "y %d, %d and %d, %d x %d px buttons on a %d px gap (the joint free rectangle is "
             "x 102..422 y 25..69)",
             panelButtonRecX0(), panelButtonRecX1(), panelButtonRecY0(), panelButtonRecY1(),
             ok ? pf.rows : 0, ok ? pf.cols : 0, ok ? pf.y0 : 0, ok ? pf.y0 + pitch : 0,
             ok ? pf.y0 + 2 * pitch : 0, ok ? pf.cellW : 0, ok ? pf.cellH : 0, ok ? pf.gap : 0);
    }
    {
        // The [layout] keys are resolved in game, against the live page and its UI scale; at the
        // menu only their presence for the view's cover is said.
        LiveGeometry g;
        liveGeometry(&g);
        char cover[16];
        utCoverKeyFormat(g.plateW, g.plateH, cover, sizeof(cover));
        const char* const fam[4] = {"grid_", "pad_", "label_", "padmat_"};
        for (int i = 0; i < 4; ++i) {
            char key[64];
            _snprintf_s(key, sizeof(key), _TRUNCATE, "%s%s", fam[i], cover);
            char val[kUtCfgFamilyValueCap];
            val[0] = 0;
            const char* v = configFamilyValue(g_cfg, key);
            if (v) strcpy_s(val, sizeof(val), v);
            if (val[0]) {
                logI("layout: %s=%s is set for this cover - it is checked in game, against the "
                     "page and its UI scale (layout_outline=1 draws it)", key, val);
            }
        }
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
//
// The rectangle is the layout's (ut_live.h liveLayoutGet): today's 105,8,317,16 (x 105, the band
// y 8..23, the right limit 422 - utLabelDefault) or the user's label_<W>x<H>. The size is
// utLabelFit's: plate_label_size at the scale, never under 9 px, and BROUGHT DOWN until the text
// ends inside the rectangle - 1.1.0 dropped the label there instead; with today's rectangle both
// agree for every size up to 15, so only plate_label_size 16..32 draws where 1.1.0 drew nothing.
// The drawn string is clamped by WIDTH, never by a character count.
// Over the GAME's own plate today's label stops at x 245: its medallion owns x 248..279 there.
const int kLabelGamePlateMaxX = 245;

// The anchor is passed in: route 2 (PresentSurface) supplies plateWindowRect()'s origin, route 1
// (the ReagentWindow::Draw detour) supplies the origin the engine's own Draw computed for this
// very frame.  Returns true when something was actually painted.  The label describes what is
// on screen and nothing else - it is not a hover line, and it has nothing to say on the VANILLA
// page (there is no collection view there), so it is not drawn there at all.
// `screen` = the call from the screen site: the label is drawn there only while label_screen is
// in force (on its own opaque ground with the pad's 1-px frame, its frame published in `pub`), and
// from the window routes only while it is not.
bool drawLiveLabel(GdCanvas* canvas, float wx, float wy, bool screen, UtPadScreenRects* pub) {
    if (!g_cfg.plateLabel) return false;
    if (!plateMaterialsVisible()) return false;   // Materials is not the page on screen

    // NEVER 1.0: an unknown scale would put the label in the screen's top-left corner, so it is
    // skipped for those few frames exactly the way the pad is.
    const float s = liveUiScale();
    if (!(s >= 0.2f && s <= 8.0f)) return false;
    LiveLayout lay;
    if (!liveLayoutGet(&lay)) return false;
    if ((lay.L.screen[kUtLayLabel] != 0) != screen) return false;   // the other site draws it
    // The size: size.label of uniquetab-text.ini when set (the same fit over the same rectangle),
    // else the layout's, which is plate_label_size's.
    const int sizeReq = panelLabelSizeInForce();
    const UtLabelFit lf = sizeReq == g_cfg.plateLabelSize
                              ? lay.L.label
                              : utLabelFit(lay.L.rect[kUtLayLabel], sizeReq, s);
    // Refusal 9: skip the LABEL (the buttons still draw) when not even 9 px of text, or 8 px of
    // width, fit its rectangle at this scale. A size the rectangle brings down is said once.
    if (!lf.ok) return false;
    const int fs = lf.size;
    {
        static int saidReq = -1, saidSize = -1;
        if (lf.bounded && (saidReq != sizeReq || saidSize != fs)) {
            saidReq = sizeReq;
            saidSize = fs;
            logD("plate label: plate_label_size=%d is taller than the label rectangle at UI scale "
                 "%.3f - drawn at %d px", sizeReq, s, fs);
        }
    }

    LiveView v;
    if (!liveViewInfo(&v)) return false;   // the vanilla page (group -1) has nothing to describe

    // The budget is a WIDTH in screen px measured with the engine's own pen rule (textWidthPx),
    // never a character count.  The clip limit is 422 over one of the mod's own plates, and 245
    // when a group is drawn over the GAME's plate (plate_swap=0, or the loose-file plates never
    // installed), whose central medallion owns record x 248..279 in this very band.
    int limitPx = lf.limitPx;
    if (!lay.L.user[kUtLayLabel] && !plateModPlateShown())
        limitPx = iround((float)(kLabelGamePlateMaxX - lf.x) * s);
    if (limitPx < 8) return false;

    // ONE format, in every case - no marker for the owned-only filter and no sentence for an
    // empty group, because either would push the counts the label exists for off the end of the
    // line.  With the owned-only filter on the row numbers are filtered rows, and a group where
    // nothing is owned reads `<group> - 0/N owned - row 0-0/0 - coll c/t`: `liveViewInfo`
    // publishes shown=0 for that layout, and its totalRows/firstRow/lastRow are then a real 0
    // apiece, never -1 and never a stale row from the layout before.
    //
    // The line is the strings file's (ut_textcfg.h utLabelCompose): the label templates in order -
    // with no file 1.1.0's two, "<group> - o/e owned - row f-l/r - coll a/c" and the same without
    // " owned" - the first that fits the width measured with the engine's own pen, else the first
    // cut by WIDTH (never by a character budget), a dangling " -" trimmed. The concession is a
    // WORD: on the widest groups (57 characters, 367 px against a 317 px band at UI scale 1.000)
    // losing " owned" keeps every count on screen at most scales; where it does not, the cut eats
    // the tail of "coll 3288/3288". {mode} is the one thing in FRONT of the group name, and only
    // while the mod does not know whether this character is hardcore or softcore (with no mode
    // nothing is painted, and the label is the only place that says why). A count the mod does not
    // have prints as label.unknown ('?') in the same slot. The group's name is name.<id> through
    // the chain (the G line's label without colour codes in an English game with no file); only
    // while the journal owns records no page of this view shows, how many is appended at the very
    // end (utNotShownSuffix) - the first part a narrow line gives up.
    // Composed only when an input changes (nothing is allocated on an unchanged frame).
    const LONG nid = InterlockedCompareExchange(&g_notInDb, -1, -1);
    const TextWorld* tw = g_textWorld;
    const long tableGen = liveGroupTableGen();
    if (tw && (tw->tableGen != tableGen || v.group >= (int)tw->names.size())) tw = nullptr;
    // A button holding several groups (the buttons file) shows ONE page, the union of its groups
    // (ut_live): liveViewInfo already reports that page - {owned} and {entries} summed over its
    // groups, its rows the union's - so the label prints it as it prints any group.
    long key[16];
    memset(key, 0, sizeof(key));
    key[0] = v.group;
    key[1] = v.owned;
    key[2] = v.entries;
    key[3] = v.firstRow;
    key[4] = v.lastRow;
    key[5] = v.totalRows;
    key[6] = v.ownedAll;
    key[7] = v.collection;
    key[8] = fs;
    key[9] = limitPx;
    key[10] = nid;
    key[11] = journalModeKnown() ? 1 : 0;
    key[12] = tw ? tw->gen : -1;
    key[13] = tableGen;
    key[14] = panelMeasureGen();
    key[15] = (long)(InterlockedCompareExchange(&g_textFileGen, 0, 0) & 0x7FFFFFFF);
    static long lastKey[16] = {-99};
    static char line[640];
    if (memcmp(key, lastKey, sizeof(key)) != 0) {
        memcpy(lastKey, key, sizeof(key));
        line[0] = 0;
        try {
            char name[256];   // a name is at most 48 UTF-16 units, 144 bytes of UTF-8
            if (tw) {
                strncpy_s(name, sizeof(name), tw->names[(size_t)v.group].c_str(), _TRUNCATE);
            } else {
                stripColourCodesTo(v.label, name, sizeof(name));
            }
            const std::string notInDb = utt::utNotShownSuffix(nid);
            const utt::UtLabelValues lv = utt::utLabelValues(
                key[11] != 0, tw ? tw->modeUnknown : utt::utTextBuiltinPlain(utt::kTextModeUnknown),
                name, v.owned, v.entries, v.firstRow, v.lastRow, v.totalRows, v.ownedAll,
                v.collection, tw ? tw->unknown : utt::utTextBuiltinPlain(utt::kTextUnknown));
            std::vector<std::string> builtin;
            if (!tw) {
                for (int i = 0; i < utt::kTextDefaultFormats; ++i)
                    builtin.push_back(utt::utTextDefaultFormat(i));
            }
            int widthCtx = fs;
            const std::string composed = utt::utLabelCompose(tw ? tw->formats : builtin, lv, notInDb,
                                                             limitPx, labelWidthPx, &widthCtx);
            strcpy_s(line, sizeof(line), composed.size() < sizeof(line) ? composed.c_str() : "");
        } catch (...) {
            line[0] = 0;
        }
    }
    if (!line[0]) return false;

    const int x = iround(wx + (float)lf.x * s);
    const int y = iround(wy + (float)lf.top * s);
    // The caravan's own parchment gold, or color.label of the strings file.
    const GdColor head = textColorOr(utt::kStyleColorLabel, {0.86f, 0.78f, 0.58f, 1.0f});
    if (screen) {
        // Off the caravan the label has no plate under it: its own opaque ground, the pad's.
        const UtScreenFrame f = utScreenFrame(lay.L.screenPx[kUtLayLabel], padPx(s, 1));
        drawScreenFrame(canvas, f);
        fillRect(canvas, {f.x, f.y, f.w, f.h}, kPadBack);
        utScreenPubLabel(pub, f);
        if (!InterlockedExchange(&g_scrLabelLogged, 1)) {
            logI("label: the label is on the screen at %s -> client %d,%d %dx%d (window-local "
                 "%d,%d,%d,%d at the window origin %.1f,%.1f, UI scale %.3f%s), size %d; drawn "
                 "from %s",
                 g_cfg.labelScreen, f.x, f.y, f.w, f.h, lay.L.rect[kUtLayLabel].x,
                 lay.L.rect[kUtLayLabel].y, lay.L.rect[kUtLayLabel].w, lay.L.rect[kUtLayLabel].h, wx,
                 wy, s, lay.L.screenShifted[kUtLayLabel] ? ", shifted onto the screen" : "", fs,
                 plateScreenSiteName());
        }
    }
    if (!drawText(canvas, x, y, fs, head, line, labelFont())) return false;

    static int lastGroup = -99;
    static int lastRow = -99;
    if (lastGroup != v.group || lastRow != v.firstRow) {
        lastGroup = v.group;
        lastRow = v.firstRow;
        logT("plate label: \"%s\" at (%d,%d) uiScale=%.3f fs=%d width=%d/%d px window "
             "origin=(%.1f,%.1f) route %d",
             line, x, y, s, fs, labelTextPx(line, fs), limitPx, wx, wy,
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
    if (drawLiveLabel(canvas, wx, wy, false, nullptr)) plateNoteLabelRoute(2);
}

}  // namespace


GdGameEngine* panelGameEngine() { return g_lastGameEngine; }

namespace {

// SEH cannot share a function with C++ objects that need unwinding, so the guard is its own
// frame and everything it touches is a call.
bool groupLabelGuarded(GdCanvas* canvas, float ox, float oy) {
    bool drew = false;
    __try {
        drew = drawLiveLabel(canvas, ox, oy, false, nullptr);
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

// ---- the screen parts -------------------------------------------------------------------------
namespace {
volatile LONG g_scrDrawOff = 0;   // a fault in the screen draw: the screen parts are off

// The SEH frame of the screen draw: nothing in it needs unwinding (UtPadScreenRects is plain).
bool screenPartsGuarded(GdCanvas* canvas, float ox, float oy, bool pad, bool label,
                        UtPadScreenRects* pub) {
    bool drew = false;
    __try {
        if (pad && drawButtonsInner(canvas, ox, oy, true, pub)) drew = true;
        if (label && drawLiveLabel(canvas, ox, oy, true, pub)) drew = true;
        if (g_cfg.layoutOutline) drawOutlineInner(canvas, ox, oy, true);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (!InterlockedExchange(&g_scrDrawOff, 1)) {
            logE("buttons: FAULT 0x%08lX drawing the pad / label pinned to the screen - they go "
                 "back into the caravan window for the rest of this session", GetExceptionCode());
        }
        utScreenPubReset(pub);
        drew = false;
    }
    return drew;
}
}  // namespace

bool panelDrawScreenParts(GdCanvas* canvas, UtPadScreenRects* pub) {
    utScreenPubReset(pub);
    if (!canvas) return false;
    // The layout editor open: it draws the pad and the label itself, at the EDITED place (in the
    // window or on the screen), and publishes its own grounds (editPublishScreen) - neither screen
    // part is drawn here then (ut_plate's screenDrawHere publishes nothing either).
    if (panelLayoutEditOpen()) return false;
    if (InterlockedCompareExchange(&g_scrDrawOff, 0, 0)) return false;
    if (!plateMaterialsVisible()) return false;   // the caravan open on the materials window
    LiveLayout lay;
    if (!liveLayoutGet(&lay)) return false;
    const UtLayout& L = lay.L;
    // The same switches as the in-window pad and label.
    const bool pad = L.screen[kUtLayPad] && g_cfg.groupButtons &&
                     !InterlockedCompareExchange(&g_buttonsOff, 0, 0) && liveActive();
    const bool label = L.screen[kUtLayLabel] && g_cfg.plateLabel &&
                       !InterlockedCompareExchange(&g_labelOff, 0, 0);
    if (!pad && !label) return false;
    ensureFonts();
    return screenPartsGuarded(canvas, lay.in.originX, lay.in.originY, pad, label, pub);
}

bool panelScreenDrawFaulted() { return InterlockedCompareExchange(&g_scrDrawOff, 0, 0) != 0; }

void panelScreenWorldReset() {
    InterlockedExchange(&g_scrPadLogged, 0);
    InterlockedExchange(&g_scrLabelLogged, 0);
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
