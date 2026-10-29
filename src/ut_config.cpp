#include "ut_config.h"

#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ut_folders.h"
#include "ut_layout.h"
#include "ut_log.h"

namespace ut {

UtConfig g_cfg;

// ---- THE TABLE --------------------------------------------------------------------------------
// One row per setting: where it is written, what it is called, what it may hold, where it lands in
// UtConfig, and the one line the file says about it. The parser, the file the mod writes and the
// test suite all read THIS - there is no second list anywhere.
//
// The `def` column repeats the member's own default. tools\test_config.cpp proves the two agree
// for every row, so they cannot drift.

static const char* const kLogLevels[] = {"error", "warn", "info", "debug", "trace", nullptr};

#define UT_OFF(m) offsetof(UtConfig, m)
#define UT_CAP(m) sizeof(UtConfig::m)

// A switch: 0 or 1.
#define UT_BOOL(sec, name, member, def, fl, cmt) \
    {sec, name, kUtCfgBool, def, 0, 1, "", UT_OFF(member), 0, nullptr, cmt, fl}
// A number in an inclusive range.
#define UT_INT(sec, name, member, def, lo, hi, fl, cmt) \
    {sec, name, kUtCfgInt, def, lo, hi, "", UT_OFF(member), 0, nullptr, cmt, fl}
// Text. `choices` is null for free text, or a null-terminated list of the only legal words.
#define UT_STR(sec, name, member, def, choices, fl, cmt) \
    {sec, name, kUtCfgStr, 0, 0, 0, def, UT_OFF(member), UT_CAP(member), choices, cmt, fl}

#define UT_H kUtCfgHidden

// A screen key of [layout] (pad_screen, label_screen): text checked by utScreenRectParse, written
// only while set.
#define UT_SCREEN(name, member, cmt)                                                              \
    {"layout", name, kUtCfgStr, 0, 0, 0, "", UT_OFF(member), UT_CAP(member), nullptr, cmt,     \
     kUtCfgWhenSet, screenValueOk}

// A [keys] row: text checked by ut_keys.h's utKeyParse for its action, hidden (commented at its
// default, live when changed), the main file only (configMainOnlyKey).
#define UT_KEY(name, member, action, def, cmt)                                                  \
    {"keys", name, kUtCfgStr, 0, 0, 0, def, UT_OFF(member), UT_CAP(member), nullptr, cmt,      \
     kUtCfgHidden, keyValueOk<action>}

namespace {
const char* screenValueOk(const char* value) {
    UtScreenRect r;
    return utScreenRectParse(value, &r) ? nullptr
                                        : "not <corner>,x,y,w,h (tl, tr, bl or br, then four "
                                          "whole numbers from 0, w and h from 1)";
}
// Null when `value` is a legal binding for the action; else ut_keys.h's reason.
template <int A>
const char* keyValueOk(const char* value) {
    UtKeyBind b;
    return utKeyParse(value, A, &b);
}
}  // namespace

const UtCfgKey kUtCfgKeys[] = {
    // ---- [general] - the four keys the file shows ---------------------------------------------
    UT_BOOL("general", "enabled", enabled, 1, 0,
            "0 = the mod goes quiet: no tab, nothing new collected. Nothing is lost"),
    UT_BOOL("general", "collect_mi", collectMi, 1, 0,
            "1 = monster infrequents are collected, one roll per item; 0 = they are not, and the ones you stored stay in the collection, hidden until it is 1 again (from the next start)"),
    UT_BOOL("general", "tooltip_mark", tooltipMark, 1, 0,
            "1 = every item tooltip says whether it is in your collection"),
    UT_STR("general", "log_level", logLevel, "info", kLogLevels, 0,
           "how much goes in the log: error, warn, info, debug, trace"),

    // ---- [layout] - zoom_pct, a hidden row (commented at 100), then the two screen keys the
    // editor writes (absent until set) ---------------------------------------------------------
    UT_INT("layout", "zoom_pct", zoomPct, 100, 100, 200, UT_H | kUtCfgRefuse,
           "the item icons bigger, in percent (100..200): fewer per screen, the icons magnified"),
    UT_SCREEN("pad_screen", padScreen,
              "the buttons pinned to a screen corner: tl|tr|bl|br,x,y,w,h in page pixels"),
    UT_SCREEN("label_screen", labelScreen,
              "the label pinned to a screen corner: tl|tr|bl|br,x,y,w,h in page pixels"),

    // ---- [keys] - the keys and clicks the tab answers to; the main file only, one row each ----
    // In UtKeyAction order (ut_keys.h): the conflict pass's "later row" is the later one here.
    UT_KEY("rows_up", keyRowsUp, kUtKeyActRowsUp, "PageUp",
           "one window of rows up on a collection page"),
    UT_KEY("rows_down", keyRowsDown, kUtKeyActRowsDown, "PageDown",
           "one window of rows down"),
    UT_KEY("button_prev", keyButtonPrev, kUtKeyActButtonPrev, "Ctrl+PageUp",
           "the button before the one on screen (MAT included)"),
    UT_KEY("button_next", keyButtonNext, kUtKeyActButtonNext, "Ctrl+PageDown",
           "the button after it"),
    UT_KEY("rows_wheel", keyRowsWheel, kUtKeyActRowsWheel, "Wheel",
           "the wheel over the page: one row"),
    UT_KEY("button_wheel", keyButtonWheel, kUtKeyActButtonWheel, "Ctrl+Wheel",
           "the wheel anywhere: one button"),
    UT_KEY("editor_open", keyEditorOpen, kUtKeyActEditorOpen, "Ctrl+RightClick",
           "on the buttons or a pinned label (or a key): the layout editor"),

    // ---- [advanced] - the commented blob, one row each ---------------------------------------
    UT_INT("advanced", "ui_scale_pct", uiScalePct, 0, 0, 300, UT_H,
           "0 = follow the game's UI scale, else force one in percent (50..300)"),
    UT_BOOL("advanced", "mp_collect", mpCollect, 1, UT_H,
            "1 = the collection also works while you play multiplayer"),
    UT_BOOL("advanced", "collect_pristine_only", collectPristineOnly, 0, UT_H,
            "1 = refuse items with affixes, components, augments or rerolls"),
    UT_INT("advanced", "max_per_record", maxPerRecord, 1, 0, 99, UT_H,
           "how many copies of one item the collection keeps, 0 = no limit (untested)"),
    UT_BOOL("advanced", "plate_label", plateLabel, 1, UT_H,
            "1 = draw the group name, the counts and the row window over the page"),
    UT_INT("advanced", "plate_label_size", plateLabelSize, 13, 8, 32, UT_H,
           "height of the label text, in page pixels (8..32)"),
    UT_INT("advanced", "pad_caption_max", padCaptionMax, 14, 6, 36, UT_H,
           "largest caption size on the buttons; the button size limits it too (6..36)"),
    UT_STR("advanced", "plate_ground", plateGround, "", nullptr, UT_H,
           "a modded page's flat background, hex RRGGBB. Empty = the vanilla tone"),
    UT_BOOL("advanced", "plate_swap", plateSwap, 1, UT_H,
            "1 = the background picture follows the group, 0 = always the vanilla one"),
    UT_BOOL("advanced", "layout_outline", layoutOutline, 0, UT_H,
            "1 = outline the grid, the buttons and the label with their numbers"),
    UT_BOOL("advanced", "layout_edit", layoutEdit, 0, UT_H,
            "1 = open the in-game layout editor on the next collection group (sets itself to 0)"),
    UT_BOOL("advanced", "edit_button", editButton, 1, UT_H,
            "1 = an EDIT button on the pad opens the layout editor; 0 = no button (the editor_open chord of [keys] "
            "still opens it: Ctrl+right-click unless you change it)"),
    UT_STR("advanced", "journal_dir", journalDir, "", nullptr, UT_H,
           "where the collection file is kept (read at start-up). Empty = the mod's own folder"),
    UT_INT("advanced", "export_gds", exportGds, 1, 0, 2, UT_H,
           "write uniq-export.gds for GD Stash: 0 off, 1 the collection, 2 all"),
    UT_INT("advanced", "export_csv", exportCsv, 0, 0, 2, UT_H,
           "write uniq-export.csv for a spreadsheet: 0 off, 1 the collection, 2 all"),
    UT_BOOL("advanced", "journal_prune", journalPrune, 0, UT_H,
            "1 = drop the entries that are no longer stored (sets itself to 0)"),
    UT_BOOL("advanced", "rescue", rescue, 0, UT_H,
            "1 with the caravan open = hand every stored item back, sets itself to 0 (untested)"),
};

const int kUtCfgKeyCount = (int)(sizeof(kUtCfgKeys) / sizeof(kUtCfgKeys[0]));

const UtCfgSection kUtCfgSections[] = {
    {"general", ""},
    {"layout",
     "Where the item grid, the buttons and the label sit. The in-game editor writes these\n"
     "lines: the EDIT button on the pad, or the editor_open chord of [keys] (Ctrl+right-click\n"
     "unless you change it), opens the editor. Delete a line to get that part's default back."},
    {"keys",
     "The keys and clicks the tab answers to, commented out at their default. Remove the ';'\n"
     "to change one: Ctrl, Shift or Alt and a key, joined by '+' (Alt+PageDown, Shift+Wheel,\n"
     "F5), or none. A letter, a digit or Space needs Ctrl or Alt. They are read from this\n"
     "file only, for every game."},
    {"advanced",
     "Every other setting, commented out at its default. Remove the ';' in front of one to\n"
     "change it; the line keeps its place. A value outside its range is clamped."},
};

const int kUtCfgSectionCount = (int)(sizeof(kUtCfgSections) / sizeof(kUtCfgSections[0]));

// ---- THE FAMILIES -----------------------------------------------------------------------------
// The [layout] rectangles: the suffix is a caravan cover size, the value four whole numbers. Both
// checks are format only; whether a rectangle fits the page is decided where it is used, against
// the live page (utLayoutResolve), because this file is re-read once a second.

namespace {
bool coverSuffixOk(const char* suffix) { return utCoverKeyParse(suffix, nullptr, nullptr); }
const char* rectValueOk(const char* value) {
    UtRect r;
    return utRectParse(value, &r) ? nullptr : "not x,y,w,h (four whole numbers, w and h from 1)";
}
}  // namespace

const UtCfgFamily kUtCfgFamilies[] = {
    {"layout", "grid_", UT_OFF(layGrid), coverSuffixOk, rectValueOk, "438x627=105,76,333,552",
     "the item grid: whole cells from the top-left, the rest stays empty"},
    {"layout", "pad_", UT_OFF(layPad), coverSuffixOk, rectValueOk, "438x627=102,25,320,44",
     "the buttons: rows and columns are chosen for the largest captions"},
    {"layout", "label_", UT_OFF(layLabel), coverSuffixOk, rectValueOk, "438x627=105,8,317,16",
     "the label line: its height limits plate_label_size"},
    // Hand-only, hidden: a row of the [advanced] blob (a family is hidden when its section is).
    {"advanced", "padmat_", UT_OFF(layPadMat), coverSuffixOk, rectValueOk, "438x627=102,25,320,44",
     "the buttons on the game's own materials page, x,y,w,h for one page size"},
};

const int kUtCfgFamilyCount = (int)(sizeof(kUtCfgFamilies) / sizeof(kUtCfgFamilies[0]));

// ---- THE VIEW KEY ------------------------------------------------------------------------------
// A custom game's key: what follows "custom-" in the view's name (custom-x and basemods+custom-x
// share it, like the journal), 1..63 of [a-z0-9_.+-]. Anything else has no file of its own.
std::string configCustomKey(const char* viewName) {
    if (!viewName) return std::string();
    const char* c = strstr(viewName, "custom-");
    if (!c) return std::string();
    c += 7;
    const size_t n = strlen(c);
    if (n == 0 || n >= (size_t)kUtCfgViewNameCap) return std::string();
    for (const char* p = c; *p; ++p) {
        const char ch = *p;
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' ||
                        ch == '.' || ch == '+' || ch == '-';
        if (!ok) return std::string();
    }
    return std::string(c);
}

// journal_dir is read at start-up, before any custom game is known, and it is where the games'
// journal folders live - so a game's own file cannot carry it. The [keys] rows are the player's
// hands, the same in every game: the main file's only.
bool configMainOnlyKey(const char* key) {
    return key && (!_stricmp(key, "journal_dir") || utKeyActionOf(key) >= 0);
}

bool configIsStateKey(const char* key) {
    return key && (!_stricmp(key, "uniq_group") || !_stricmp(key, "uniq_row") ||
                   !_stricmp(key, "owned_only"));
}

namespace {
std::wstring widen(const char* s) {
    std::wstring w;
    for (const char* p = s; p && *p; ++p) w += (wchar_t)(unsigned char)*p;
    return w;
}
}  // namespace

void configViewFiles(const wchar_t* mainIni, const char* viewName, std::wstring* ini,
                     std::wstring* state) {
    if (ini) ini->clear();
    if (state) state->clear();
    if (!mainIni || !mainIni[0]) return;
    std::wstring dir(mainIni);
    const size_t slash = dir.find_last_of(L"\\/");
    dir = slash == std::wstring::npos ? std::wstring(L".") : dir.substr(0, slash);
    // mods\<leaf>\ for the game, mods\<leaf>\basemods\ for its /basemods view (ut_folders.h)
    const std::string key = configCustomKey(viewName);
    const std::string game = utCustomGameRelLeaf(key);
    const std::string view = utCustomViewRel(viewName);
    if (key.empty() || game.empty() || view.empty()) {
        if (state) *state = dir + L"\\uniquetab.state";
        return;
    }
    if (ini) *ini = dir + L"\\" + widen(game.c_str()) + L"\\uniquetab.ini";
    if (state) *state = dir + L"\\" + widen(view.c_str()) + L"\\uniquetab.state";
}

const UtCfgFamily* configFamilyIn(const UtCfgFamily* table, int count, const char* key,
                                  const char** suffix) {
    if (!table || !key) return nullptr;
    for (int i = 0; i < count; ++i) {
        const UtCfgFamily& f = table[i];
        const size_t n = strlen(f.prefix);
        if (strncmp(key, f.prefix, n) != 0) continue;
        if (!f.suffixOk || !f.suffixOk(key + n)) continue;
        if (suffix) *suffix = key + n;
        return &f;
    }
    return nullptr;
}

const UtCfgFamily* configFamilyOf(const char* key, const char** suffix) {
    return configFamilyIn(kUtCfgFamilies, kUtCfgFamilyCount, key, suffix);
}

const char* configFamilyGet(const UtCfgFamilyStore& s, const char* suffix) {
    if (!suffix) return nullptr;
    for (int i = 0; i < s.count && i < kUtCfgFamilySlots; ++i)
        if (!strcmp(s.e[i].suffix, suffix)) return s.e[i].value;
    return nullptr;
}

bool configFamilySet(UtCfgFamilyStore* s, const char* suffix, const char* value) {
    if (!s || !suffix || !value) return false;
    if (strlen(suffix) >= (size_t)kUtCfgFamilySuffixCap) return false;
    if (strlen(value) >= (size_t)kUtCfgFamilyValueCap) return false;
    int at = -1;
    for (int i = 0; i < s->count && i < kUtCfgFamilySlots; ++i)
        if (!strcmp(s->e[i].suffix, suffix)) at = i;
    if (at < 0) {
        if (s->count >= kUtCfgFamilySlots) return false;
        at = s->count++;
    }
    UtCfgFamilyEntry& e = s->e[at];
    memset(&e, 0, sizeof(e));   // every unused byte zero: the change test memcmps the struct
    memcpy(e.suffix, suffix, strlen(suffix));
    memcpy(e.value, value, strlen(value));
    return true;
}

const char* configFamilyValue(const UtConfig& c, const char* key) {
    const char* suffix = nullptr;
    const UtCfgFamily* f = configFamilyOf(key, &suffix);
    if (!f) return nullptr;
    return configFamilyGet(*(const UtCfgFamilyStore*)((const char*)&c + f->off), suffix);
}

namespace {

// The scan's `seen` array is indexed by table row.
const int kMaxKeys = 64;
const size_t kRenderCap = 32768;   // the rendered file: the table plus every family's lines
const size_t kViewCap = 160;       // a view's full name ("basemods+custom-<key>")
static_assert(sizeof(kUtCfgKeys) / sizeof(kUtCfgKeys[0]) <= (size_t)kMaxKeys,
              "kMaxKeys must cover the whole key table");

// THE SETTINGS STATE. Everything here is under g_applyLock (the worker's reload, the game
// thread's view change and the persists never interleave); g_cfg is written under it as well and
// read without it, as before.
FILETIME g_lastWrite = {0, 0};
bool g_haveStamp = false;
UtConfig g_base;                 // the main file as read: no game file, no state, no master switch
bool g_haveBase = false;
char g_view[kViewCap] = {0};     // the view g_cfg is derived for
bool g_viewFilesReady = false;   // the two paths below are computed for g_path and g_view
// The active custom game's own file (empty path = the view has none): its stamp and the WHOLE
// configuration it describes (a key it does not name is the compiled default, never the main
// file's value). g_ovHave = it was read, and it is what g_cfg is derived from.
wchar_t g_ovPath[MAX_PATH] = {0};
FILETIME g_ovStamp = {0, 0};
bool g_ovHaveStamp = false;
bool g_ovHave = false;
UtConfig g_ovVals;
char g_ovKey[kUtCfgViewNameCap] = {0};
// The active view's state file and its three values.
wchar_t g_statePath[MAX_PATH] = {0};
int g_stGroup = -1, g_stRow = 0, g_stOwned = 0;
SRWLOCK g_applyLock = SRWLOCK_INIT;
volatile LONG g_cfgGen = 0;
wchar_t g_path[MAX_PATH] = {0};

int* intAt(UtConfig* cfg, size_t off) { return (int*)((char*)cfg + off); }
char* strAt(UtConfig* cfg, size_t off) { return (char*)cfg + off; }
const int* intAt(const UtConfig* cfg, size_t off) { return (const int*)((const char*)cfg + off); }
const char* strAt(const UtConfig* cfg, size_t off) { return (const char*)cfg + off; }

bool keyHidden(const UtCfgKey& k) { return (k.flags & kUtCfgHidden) != 0; }
bool keyWhenSet(const UtCfgKey& k) { return (k.flags & kUtCfgWhenSet) != 0; }
bool familyHidden(const UtCfgFamily& f) { return !strcmp(f.section, "advanced"); }

// The visible keys a file is expected to show: not the hidden rows, not the written-when-set ones.
int visibleKeyCount() {
    int n = 0;
    for (int i = 0; i < kUtCfgKeyCount; ++i)
        if (!keyHidden(kUtCfgKeys[i]) && !keyWhenSet(kUtCfgKeys[i])) ++n;
    return n;
}

// The rows a migration counts as "kept" or "defaulted": every row but the written-when-set ones.
int countedKeyCount() {
    int n = 0;
    for (int i = 0; i < kUtCfgKeyCount; ++i)
        if (!keyWhenSet(kUtCfgKeys[i])) ++n;
    return n;
}

int findKey(const char* name) {
    for (int i = 0; i < kUtCfgKeyCount; ++i)
        if (!_stricmp(kUtCfgKeys[i].name, name)) return i;
    return -1;
}

// A row's value as the file writes it.
void valueText(const UtCfgKey& k, const UtConfig& src, char* out, size_t cap) {
    if (k.type == kUtCfgStr)
        _snprintf_s(out, cap, _TRUNCATE, "%s", strAt(&src, k.off));
    else
        _snprintf_s(out, cap, _TRUNCATE, "%d", *intAt(&src, k.off));
}
void defaultText(const UtCfgKey& k, char* out, size_t cap) {
    if (k.type == kUtCfgStr)
        _snprintf_s(out, cap, _TRUNCATE, "%s", k.defStr);
    else
        _snprintf_s(out, cap, _TRUNCATE, "%d", k.def);
}

// ---- rendering ---------------------------------------------------------------------------
// The file the mod writes IS the table, so the text and the parser cannot disagree.

struct Out {
    char* p;
    size_t cap;
    size_t n;
    bool bad;

    void put(const char* s) {
        const size_t len = strlen(s);
        if (bad || !p || n + len + 1 > cap) {
            bad = true;
            return;
        }
        memcpy(p + n, s, len);
        n += len;
        p[n] = 0;
    }

    void line(const char* fmt, ...) {
        char tmp[512];
        va_list ap;
        va_start(ap, fmt);
        const int k = _vsnprintf_s(tmp, sizeof(tmp), _TRUNCATE, fmt, ap);
        va_end(ap);
        if (k < 0) {
            bad = true;
            return;
        }
        put(tmp);
        put("\r\n");
    }
};

// "name=value" padded with blanks out to the comment column, then "; comment". One line.
void renderKey(Out* o, const UtCfgKey& k, const UtConfig& src) {
    char head[352], val[300];
    valueText(k, src, val, sizeof(val));
    _snprintf_s(head, sizeof(head), _TRUNCATE, "%s=%s", k.name, val);
    const size_t kComment = 26;  // the column every comment starts in
    char pad[40];
    size_t p = 0;
    const size_t hn = strlen(head);
    while (p + 1 < sizeof(pad) && hn + p < kComment) pad[p++] = ' ';
    if (p == 0) pad[p++] = ' ';
    pad[p] = 0;
    o->line("%s%s; %s", head, pad, k.comment);
}

// THE BLOB. Its rows are aligned with TABS (stops every 8 columns) on one column: the first stop
// past the longest commented default row, so the template never moves; a longer live row gets one
// tab.
size_t tabbedTo(char* pad, size_t cap, size_t from, size_t col) {
    size_t n = 0, at = from;
    do {
        if (n + 1 >= cap) break;
        pad[n++] = '\t';
        at = (at / 8 + 1) * 8;
    } while (at < col);
    pad[n] = 0;
    return n;
}

size_t blobColumn() {
    size_t m = 0;
    for (int i = 0; i < kUtCfgKeyCount; ++i) {
        const UtCfgKey& k = kUtCfgKeys[i];
        if (!keyHidden(k)) continue;
        char d[300];
        defaultText(k, d, sizeof(d));
        const size_t len = 1 + strlen(k.name) + 1 + strlen(d);
        if (len > m) m = len;
    }
    for (int i = 0; i < kUtCfgFamilyCount; ++i) {
        const UtCfgFamily& f = kUtCfgFamilies[i];
        if (!familyHidden(f)) continue;
        const size_t len = 1 + strlen(f.prefix) + strlen(f.example);
        if (len > m) m = len;
    }
    return (m / 8 + 1) * 8;
}

void blobRow(Out* o, const char* head, const char* comment, size_t col) {
    char pad[16];
    tabbedTo(pad, sizeof(pad), strlen(head), col);
    o->line("%s%s; %s", head, pad, comment);
}

// A section's comment lines; a blurb may be several lines, each written as its own comment.
bool renderBlurb(Out* o, const char* blurb) {
    for (const char* b = blurb; b && *b;) {
        const char* nl = strchr(b, '\n');
        const size_t len = nl ? (size_t)(nl - b) : strlen(b);
        char one[200];
        if (len >= sizeof(one)) return false;
        memcpy(one, b, len);
        one[len] = 0;
        o->line("; %s", one);
        b = nl ? nl + 1 : b + len;
    }
    return true;
}

// The VISIBLE family lines of `section` (null = every family), one page size at a time: the
// suffixes in the order they first appear (the grid's first), and for each one its grid_, pad_
// and label_ line - so the lines of one page size stay together, as the editor writes them.
void renderLayoutLines(Out* o, const UtConfig& src, const char* section) {
    const char* order[kUtCfgFamilySlots * 8];
    int n = 0;
    for (int i = 0; i < kUtCfgFamilyCount; ++i) {
        const UtCfgFamily& f = kUtCfgFamilies[i];
        if (section ? (strcmp(f.section, section) != 0 || familyHidden(f)) : false) continue;
        const UtCfgFamilyStore& st = *(const UtCfgFamilyStore*)((const char*)&src + f.off);
        for (int j = 0; j < st.count && j < kUtCfgFamilySlots; ++j) {
            bool have = false;
            for (int q = 0; q < n && !have; ++q) have = !strcmp(order[q], st.e[j].suffix);
            if (!have && n < (int)(sizeof(order) / sizeof(order[0]))) order[n++] = st.e[j].suffix;
        }
    }
    for (int q = 0; q < n; ++q)
        for (int i = 0; i < kUtCfgFamilyCount; ++i) {
            const UtCfgFamily& f = kUtCfgFamilies[i];
            if (section ? (strcmp(f.section, section) != 0 || familyHidden(f)) : false) continue;
            const char* v =
                configFamilyGet(*(const UtCfgFamilyStore*)((const char*)&src + f.off), order[q]);
            if (v) o->line("%s%s=%s", f.prefix, order[q], v);
        }
}

// journal_dir's line in a custom game's own file.
const char* const kMainOnlyNote =
    "read at start-up from the main uniquetab.ini only - set it there";

// ONE RENDERER for both files: the main uniquetab.ini (`game` null) and a custom game's own
// mods\<game>\uniquetab.ini. They differ in the header and in journal_dir, which a game's file
// shows as a commented line naming its one home; every other line is the same template.
size_t renderSettings(char* out, size_t cap, const UtConfig& src, const char* game) {
    Out o = {out, cap, 0, false};
    o.put("");
    o.line("; ============================================================================");
    if (game) {
        o.line("; Unique Collection Tab - your settings for the custom game \"%s\".", game);
        o.line("; While this game is played this file is the only one read: nothing comes from the");
        o.line("; main uniquetab.ini but journal_dir and [keys]. It was copied from the main file's");
        o.line("; values once, when it was made. Delete it to copy them again.");
        o.line("; Edit it while the game runs: it is re-read once a second. Anything after a");
        o.line("; ';' is a comment.");
    } else {
        o.line("; Unique Collection Tab - your settings.");
        o.line("; Edit it while the game runs: it is re-read once a second. Anything after a");
        o.line("; ';' is a comment. Delete this file to get the defaults back.");
    }
    o.line("; More settings are at the end, commented out. Your buttons (uniquetab-buttons.ini)");
    o.line("; and your own groups (uniquetab-groups.ini): README.md, section \"Settings\".");
    o.line("; ============================================================================");
    o.line("ini_version=%d", UT_INI_VERSION);
    const size_t col = blobColumn();
    for (int s = 0; s < kUtCfgSectionCount; ++s) {
        const char* name = kUtCfgSections[s].name;
        if (game && !strcmp(name, "keys")) continue;   // the main file's only (configMainOnlyKey)
        o.line("");
        o.line("[%s]", name);
        if (!renderBlurb(&o, kUtCfgSections[s].blurb)) return 0;
        for (int i = 0; i < kUtCfgKeyCount; ++i) {
            const UtCfgKey& k = kUtCfgKeys[i];
            if (strcmp(k.section, name) != 0) continue;
            if (keyWhenSet(k)) continue;   // after the family lines, below
            if (game && configMainOnlyKey(k.name)) {
                // never a value here: the line says where it is set (parseInto refuses one)
                char head[64];
                _snprintf_s(head, sizeof(head), _TRUNCATE, ";%s=", k.name);
                blobRow(&o, head, kMainOnlyNote, col);
                continue;
            }
            if (!keyHidden(k)) {
                renderKey(&o, k, src);
                continue;
            }
            // A hidden row: commented at its default, LIVE (the ';' gone) when the value differs.
            char val[300], def[300], head[352];
            valueText(k, src, val, sizeof(val));
            defaultText(k, def, sizeof(def));
            _snprintf_s(head, sizeof(head), _TRUNCATE, "%s%s=%s", strcmp(val, def) ? "" : ";",
                        k.name, val);
            blobRow(&o, head, k.comment, col);
        }
        renderLayoutLines(&o, src, name);
        // The written-when-set keys (pad_screen, label_screen): "key=value" while set, where
        // configPersistStr inserts them (the end of their section), no line otherwise.
        for (int i = 0; i < kUtCfgKeyCount; ++i) {
            const UtCfgKey& k = kUtCfgKeys[i];
            if (strcmp(k.section, name) != 0 || !keyWhenSet(k)) continue;
            char val[300], def[300];
            valueText(k, src, val, sizeof(val));
            defaultText(k, def, sizeof(def));
            if (strcmp(val, def) != 0) o.line("%s=%s", k.name, val);
        }
        for (int i = 0; i < kUtCfgFamilyCount; ++i) {
            const UtCfgFamily& f = kUtCfgFamilies[i];
            if (strcmp(f.section, name) != 0 || !familyHidden(f)) continue;
            const UtCfgFamilyStore& st = *(const UtCfgFamilyStore*)((const char*)&src + f.off);
            char head[160];
            if (st.count == 0) {
                _snprintf_s(head, sizeof(head), _TRUNCATE, ";%s%s", f.prefix, f.example);
                blobRow(&o, head, f.comment, col);
            }
            for (int j = 0; j < st.count && j < kUtCfgFamilySlots; ++j) {
                _snprintf_s(head, sizeof(head), _TRUNCATE, "%s%s=%s", f.prefix, st.e[j].suffix,
                            st.e[j].value);
                blobRow(&o, head, f.comment, col);
            }
        }
    }
    return o.bad ? 0 : o.n;
}

}  // namespace

size_t configRender(char* out, size_t cap, const UtConfig& src) {
    return renderSettings(out, cap, src, nullptr);
}

size_t configRenderView(char* out, size_t cap, const char* key, const UtConfig& src) {
    return renderSettings(out, cap, src, key ? key : "");
}

size_t configRenderState(char* out, size_t cap, const UtConfig& src) {
    Out o = {out, cap, 0, false};
    o.put("");
    o.line("; uniquetab.state - written by the mod: where the tab was left in this game.");
    o.line("; Not a setting: delete it to start on the materials page.");
    o.line("uniq_group=%d", src.uniqGroup);
    o.line("uniq_row=%d", src.uniqRow);
    o.line("owned_only=%d", src.ownedOnly);
    return o.bad ? 0 : o.n;
}

size_t configTemplateBytes() {
    static size_t cached = 0;
    if (!cached) {
        static char buf[kRenderCap];
        const UtConfig defaults;
        cached = configRender(buf, sizeof(buf), defaults);
    }
    return cached;
}

namespace {

// ---- files -------------------------------------------------------------------------------

const unsigned kMaxIniBytes = 1u << 20;  // our own few-KB file; anything bigger is not ours

// Reads all of `h` into a malloc'd NUL-terminated buffer (caller frees). Null on any failure.
char* readAllA(HANDLE h, DWORD* outLen) {
    *outLen = 0;
    LARGE_INTEGER li;
    li.QuadPart = 0;
    if (!GetFileSizeEx(h, &li)) return nullptr;
    if (li.QuadPart < 0 || (unsigned long long)li.QuadPart > kMaxIniBytes) return nullptr;
    const DWORD want = (DWORD)li.QuadPart;
    char* buf = (char*)malloc((size_t)want + 2);
    if (!buf) return nullptr;
    DWORD got = 0;
    if (want && !ReadFile(h, buf, want, &got, nullptr)) {
        free(buf);
        return nullptr;
    }
    if (got > want) got = want;
    buf[got] = 0;
    *outLen = got;
    return buf;
}

// The whole of `path` and its last-write time; null when it cannot be read.
char* readFileW(const wchar_t* path, DWORD* len, FILETIME* ft) {
    *len = 0;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return nullptr;
    if (ft && !GetFileTime(h, nullptr, nullptr, ft)) ft->dwLowDateTime = ft->dwHighDateTime = 0;
    char* buf = readAllA(h, len);
    CloseHandle(h);
    return buf;
}

bool fileExists(const wchar_t* path) {
    return path && path[0] && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

// The folders above `path` (the file's own folder and its parent), so a view folder's file can
// be written before the view's generation made the folder.
void makeParents(const wchar_t* path) {
    std::wstring dir(path);
    size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    dir = dir.substr(0, slash);
    slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) CreateDirectoryW(dir.substr(0, slash).c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
}

bool writeBytes(const wchar_t* path, const char* buf, size_t n, bool overwrite) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           overwrite ? CREATE_ALWAYS : CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(h, buf, (DWORD)n, &written, nullptr);
    CloseHandle(h);
    return ok && written == (DWORD)n;
}

// A temp file next to `path`, moved over it: a reader never sees half a file.
bool writeAtomic(const wchar_t* path, const char* buf, size_t n) {
    wchar_t tmpPath[MAX_PATH];
    _snwprintf_s(tmpPath, _TRUNCATE, L"%s.tmp", path);
    if (!writeBytes(tmpPath, buf, n, true)) {
        DeleteFileW(tmpPath);
        return false;
    }
    // The worker thread re-reads the settings once a second; if the replace lands in that
    // window, one retry is enough (the reader closes the handle immediately).
    if (!MoveFileExW(tmpPath, path, MOVEFILE_REPLACE_EXISTING)) {
        Sleep(30);
        if (!MoveFileExW(tmpPath, path, MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW(tmpPath);
            return false;
        }
    }
    return true;
}

bool writeIni(const wchar_t* path, const UtConfig& src, bool overwrite) {
    char* buf = (char*)malloc(kRenderCap);
    if (!buf) return false;
    const size_t n = configRender(buf, kRenderCap, src);
    if (!n) {
        logE("config: the settings file could not be rendered - it is not written");
        free(buf);
        return false;
    }
    const bool ok = writeBytes(path, buf, n, overwrite);
    free(buf);
    return ok;
}

// A custom game's own file, the whole template rendered from `src`. A new one never replaces a
// file (CREATE_NEW); `overwrite` (the per-file migration) replaces it atomically.
bool writeViewFile(const wchar_t* path, const char* key, const UtConfig& src, bool overwrite) {
    char* buf = (char*)malloc(kRenderCap);
    if (!buf) return false;
    const size_t n = configRenderView(buf, kRenderCap, key, src);
    makeParents(path);
    const bool ok = n && (overwrite ? writeAtomic(path, buf, n) : writeBytes(path, buf, n, false));
    free(buf);
    return ok;
}

bool writeStateFile(const wchar_t* path, int group, int row, int owned) {
    UtConfig s;
    s.uniqGroup = group;
    s.uniqRow = row;
    s.ownedOnly = owned;
    char buf[512];
    const size_t n = configRenderState(buf, sizeof(buf), s);
    if (!n) return false;
    makeParents(path);
    return writeAtomic(path, buf, n);
}

// ---- parsing -----------------------------------------------------------------------------

enum ParseMode {
    kParseMain = 0,   // uniquetab.ini
    kParseView,       // a custom game's own file: every key but journal_dir, every family
    kParseViewOld,    // the same, from an older build (the migration): a [layout] line whose
                      // value is `default` removes that line from the seed
    kParseState       // uniquetab.state: the three state keys
};

struct ViewLine {
    char view[kViewCap];
    char key[32];
    char value[kUtCfgFamilyValueCap];
};
enum { kMaxViewLines = 32 };

struct UtIniScan {
    bool seen[kMaxKeys];  // which table rows the file actually had
    int known;            // how many distinct VISIBLE rows that is (the self-check's count)
    int knownAll;         // how many distinct rows
    int unknown;          // lines naming a key this build does not have
    char unknownNames[512];
    int family;           // family lines stored
    int refused;          // family lines whose value failed the family's check, or did not fit
    int iniVersionSeen;
    int stateSeen;        // bit 0 uniq_group, 1 uniq_row, 2 owned_only
    int stateVals[3];
    int notHere;          // a game's own file: journal_dir, which only the main file sets
    char notHereNames[200];
    int keysHere;         // a game's own file: [keys] rows, which only the main file sets
    char keysHereNames[200];
    int viewLines;        // [view:<name>] lines (the main file's old blocks), kept for the move
    int viewOverflow;
    ViewLine views[kMaxViewLines];
};

void scanInit(UtIniScan* s) {
    memset(s, 0, sizeof(*s));
    s->iniVersionSeen = -1;
}

void addName(char* list, size_t cap, const char* name) {
    const size_t at = strlen(list);
    if (at + strlen(name) + 3 < cap)
        _snprintf_s(list + at, cap - at, _TRUNCATE, "%s%s", at ? ", " : "", name);
}

// Trailing blanks off a value the caller has already cut at the comment and trimmed on the left.
size_t trimmedLen(const char* value) {
    size_t n = strlen(value);
    while (n > 0 && (value[n - 1] == ' ' || value[n - 1] == '\t' || value[n - 1] == '\r' ||
                     value[n - 1] == '\n')) {
        --n;
    }
    return n;
}

void trimmedCopy(const char* value, char* out, size_t cap) {
    size_t n = trimmedLen(value);
    if (n >= cap) n = cap - 1;
    memcpy(out, value, n);
    out[n] = 0;
}

// One key, validated. A number outside its range is CLAMPED and the log says so; a word that is
// not one of the choices is refused and the default kept. Neither is written back to the file -
// the file keeps whatever the user typed, so a silly value never sticks.
void applyOne(const UtCfgKey& k, const char* value, UtConfig* cfg) {
    if (k.type == kUtCfgStr) {
        char tmp[300];
        trimmedCopy(value, tmp, sizeof(tmp));
        if (k.choices && tmp[0]) {
            bool ok = false;
            for (const char* const* c = k.choices; *c && !ok; ++c)
                if (!_stricmp(tmp, *c)) {
                    _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "%s", *c);
                    ok = true;
                }
            if (!ok) {
                logW("config: %s=%s is not one of the words it accepts - using \"%s\"", k.name,
                     tmp, k.defStr);
                _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "%s", k.defStr);
            }
        }
        if (k.check && tmp[0]) {
            // Too long for its member is refused too: a cut value could read as another one.
            const char* why = strlen(tmp) >= k.cap ? "too long for this setting" : k.check(tmp);
            if (why) {
                if (k.defStr[0])
                    logW("config: %s=%s is %s - ignored, %s=%s stays", k.name, tmp, why, k.name,
                         k.defStr);
                else
                    logW("config: %s=%s is %s - ignored", k.name, tmp, why);
                _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "%s", k.defStr);
            }
        } else if (k.check && k.defStr[0]) {
            // an empty value of a checked row with a default (a [keys] row) is that default
            _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "%s", k.defStr);
        }
        char* dst = strAt(cfg, k.off);
        _snprintf_s(dst, k.cap, _TRUNCATE, "%s", tmp);
        return;
    }
    const int raw = atoi(value);
    int v = raw;
    if (v < k.lo) v = k.lo;
    if (v > k.hi) v = k.hi;
    if (v != raw && (k.flags & kUtCfgRefuse)) {
        char tmp[64];
        trimmedCopy(value, tmp, sizeof(tmp));
        logW("config: %s=%s is outside %d..%d - ignored, using %d", k.name, tmp, k.lo, k.hi, k.def);
        v = k.def;
    } else if (v != raw) {
        logW("config: %s=%d is outside %d..%d - using %d", k.name, raw, k.lo, k.hi, v);
    }
    *intAt(cfg, k.off) = v;
}

void familyRemove(UtCfgFamilyStore* s, const char* suffix) {
    for (int i = 0; i < s->count && i < kUtCfgFamilySlots; ++i) {
        if (strcmp(s->e[i].suffix, suffix) != 0) continue;
        for (int j = i; j + 1 < s->count && j + 1 < kUtCfgFamilySlots; ++j) s->e[j] = s->e[j + 1];
        --s->count;
        memset(&s->e[s->count], 0, sizeof(s->e[0]));   // the change test memcmps the struct
        return;
    }
}

// One family line. A value the family's check refuses, or one that does not fit the store, is
// not stored and the log says so; the parse only runs when the file changed, so once per edit.
// `oldGameFile`: the value `default` of a game's file from an older build (it hid the main
// file's line in that game) removes the line from the seed the migration starts from. Anywhere
// else `default` is a value like any other, and the family's check refuses it.
void applyFamily(const UtCfgFamily& f, const char* key, const char* value, UtConfig* cfg,
                 UtIniScan* scan, bool oldGameFile) {
    char tmp[300];
    trimmedCopy(value, tmp, sizeof(tmp));
    UtCfgFamilyStore* st = (UtCfgFamilyStore*)((char*)cfg + f.off);
    if (oldGameFile && !_stricmp(tmp, "default")) {
        familyRemove(st, key + strlen(f.prefix));
        ++scan->family;
        return;
    }
    const char* why = f.valueOk ? f.valueOk(tmp) : nullptr;
    if (why) {
        logW("config: %s=%s is %s - ignored", key, tmp, why);
        ++scan->refused;
        return;
    }
    if (!configFamilySet(st, key + strlen(f.prefix), tmp)) {
        logW("config: %s is ignored - %s holds at most %d lines of at most %d characters", key,
             f.prefix, kUtCfgFamilySlots, kUtCfgFamilyValueCap - 1);
        ++scan->refused;
        return;
    }
    ++scan->family;
}

// THE KEYS THAT MOVED (ini_version 39). Their old line is dropped like any other; one with a
// value other than its old default gets a WARN naming where that value lives now.
struct MergedKey {
    const char* name;
    const char* def;
    const char* home;
};
const MergedKey kMerged[] = {
    {"log_flush_each_line", "0", "log_level=trace, which writes every line at once"},
    {"tooltip_text_yes", "", "tooltip.yes under [tab] of uniquetab-buttons.ini"},
    {"tooltip_text_no", "", "tooltip.no under [tab] of uniquetab-buttons.ini"},
};

void checkMerged(const char* key, const char* value) {
    for (const MergedKey& m : kMerged) {
        if (_stricmp(key, m.name) != 0) continue;
        char tmp[300];
        trimmedCopy(value, tmp, sizeof(tmp));
        const bool isDefault = !strcmp(m.def, "0") ? atoi(tmp) == 0 : !strcmp(tmp, m.def);
        if (!isDefault) {
            logW("config: %s=%s is not read from uniquetab.ini any more - its value belongs in "
                 "%s; set it there",
                 m.name, tmp, m.home);
        }
        return;
    }
}

int stateIndex(const char* key) {
    if (!_stricmp(key, "uniq_group")) return 0;
    if (!_stricmp(key, "uniq_row")) return 1;
    if (!_stricmp(key, "owned_only")) return 2;
    return -1;
}

// "[view:<name>]" -> true and the name, lower-cased; "[anything else]" -> false.
bool viewHeader(const char* trimmed, char* name, size_t cap) {
    if (_strnicmp(trimmed, "[view:", 6) != 0) return false;
    const char* close = strchr(trimmed, ']');
    if (!close) return false;
    const char* b = trimmed + 6;
    while (*b == ' ' || *b == '\t') ++b;
    const char* e = close;
    while (e > b && (e[-1] == ' ' || e[-1] == '\t')) --e;
    size_t n = (size_t)(e - b);
    if (n + 1 > cap) n = cap - 1;
    for (size_t i = 0; i < n; ++i) {
        char c = b[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        name[i] = c;
    }
    name[n] = 0;
    return true;
}

// Parses `buf` in place into `parsed`, recording what was there in `scan`. Values after a ';' or
// '#' on the same line are comments; a "[section]" line is skipped (a key is found by name),
// except that the main file's old [view:<name>] blocks are collected, never applied.
void parseInto(char* buf, int mode, UtConfig* parsed, UtIniScan* scan) {
    scanInit(scan);
    char view[kViewCap] = {0};
    char* line = buf;
    while (line && *line) {
        char* next = strpbrk(line, "\r\n");
        if (next) {
            *next = 0;
            ++next;
            while (*next == '\r' || *next == '\n') ++next;
        }
        while (*line == ' ' || *line == '\t') ++line;
        if (*line == '[') {
            if (!viewHeader(line, view, sizeof(view))) view[0] = 0;
            if (mode != kParseMain) view[0] = 0;
        }
        char* eq = (*line && *line != ';' && *line != '#' && *line != '[') ? strchr(line, '=')
                                                                           : nullptr;
        if (eq) {
            *eq = 0;
            char* key = line;
            char* value = eq + 1;
            for (char* p = eq - 1; p >= key && (*p == ' ' || *p == '\t'); --p) *p = 0;
            while (*value == ' ' || *value == '\t') ++value;
            char* cut = strpbrk(value, ";#");
            if (cut) *cut = 0;
            const int st = stateIndex(key);
            if (mode == kParseState) {
                if (st >= 0) {
                    scan->stateSeen |= 1 << st;
                    scan->stateVals[st] = atoi(value);
                }
            } else if (!_stricmp(key, "ini_version")) {
                parsed->iniVersion = atoi(value);
                scan->iniVersionSeen = parsed->iniVersion;
            } else if (mode == kParseMain && view[0]) {
                if (scan->viewLines < kMaxViewLines) {
                    ViewLine& v = scan->views[scan->viewLines++];
                    _snprintf_s(v.view, sizeof(v.view), _TRUNCATE, "%s", view);
                    _snprintf_s(v.key, sizeof(v.key), _TRUNCATE, "%s", key);
                    trimmedCopy(value, v.value, sizeof(v.value));
                } else {
                    ++scan->viewOverflow;
                }
            } else {
                const int idx = findKey(key);
                const UtCfgFamily* fam = idx < 0 ? configFamilyOf(key, nullptr) : nullptr;
                const bool gameFile = mode == kParseView || mode == kParseViewOld;
                if (idx >= 0 && gameFile && configMainOnlyKey(kUtCfgKeys[idx].name)) {
                    if (utKeyActionOf(kUtCfgKeys[idx].name) >= 0) {
                        ++scan->keysHere;
                        addName(scan->keysHereNames, sizeof(scan->keysHereNames),
                                kUtCfgKeys[idx].name);
                    } else {
                        ++scan->notHere;
                        addName(scan->notHereNames, sizeof(scan->notHereNames),
                                kUtCfgKeys[idx].name);
                    }
                } else if (idx >= 0) {
                    applyOne(kUtCfgKeys[idx], value, parsed);
                    if (!scan->seen[idx]) {
                        scan->seen[idx] = true;
                        if (!keyWhenSet(kUtCfgKeys[idx])) {
                            ++scan->knownAll;
                            if (!keyHidden(kUtCfgKeys[idx])) ++scan->known;
                        }
                    }
                } else if (fam) {
                    applyFamily(*fam, key, value, parsed, scan, mode == kParseViewOld);
                } else if (st >= 0) {
                    scan->stateSeen |= 1 << st;
                    scan->stateVals[st] = atoi(value);
                } else if (*key) {
                    if (mode == kParseMain) checkMerged(key, value);
                    ++scan->unknown;
                    addName(scan->unknownNames, sizeof(scan->unknownNames), key);
                }
            }
        }
        line = next;
    }
}

// ---- [keys]: the bindings in force, derived from the seven texts -----------------------------
// The texts are already checked (applyOne kept the default for a refused one); this parses them
// into c->keys and runs the conflict pass - two identical chords of one site: the later row is
// none, one WARN naming both. Runs where the main file is parsed, so once per edit.
const char* keyTextOf(const UtConfig& c, int action) {
    switch (action) {
        case kUtKeyActRowsUp: return c.keyRowsUp;
        case kUtKeyActRowsDown: return c.keyRowsDown;
        case kUtKeyActButtonPrev: return c.keyButtonPrev;
        case kUtKeyActButtonNext: return c.keyButtonNext;
        case kUtKeyActRowsWheel: return c.keyRowsWheel;
        case kUtKeyActButtonWheel: return c.keyButtonWheel;
        case kUtKeyActEditorOpen: return c.keyEditorOpen;
        default: return "";
    }
}

void deriveKeys(UtConfig* c) {
    const char* texts[kUtKeyActCount];
    for (int a = 0; a < kUtKeyActCount; ++a) texts[a] = keyTextOf(*c, a);
    UtKeyConflict conf[kUtKeyActCount];
    const int n = utKeysDerive(texts, &c->keys, conf, kUtKeyActCount);
    for (int i = 0; i < n && i < kUtKeyActCount; ++i) {
        char chord[64];
        utKeyText(conf[i].chord, chord, sizeof(chord));
        logW("config: %s=%s is the same %s as %s=%s - %s is off (none) until one of them changes",
             kUtKeyActions[conf[i].later].name, texts[conf[i].later], chord,
             kUtKeyActions[conf[i].earlier].name, texts[conf[i].earlier],
             kUtKeyActions[conf[i].later].name);
    }
}

// The main file's own rows (journal_dir, [keys]) and the bindings derived from them, from `main`.
void copyMainOnly(UtConfig* dst, const UtConfig& main) {
    for (int i = 0; i < kUtCfgKeyCount; ++i) {
        const UtCfgKey& k = kUtCfgKeys[i];
        if (!configMainOnlyKey(k.name)) continue;
        if (k.type == kUtCfgStr) memcpy(strAt(dst, k.off), strAt(&main, k.off), k.cap);
        else *intAt(dst, k.off) = *intAt(&main, k.off);
    }
    dst->keys = main.keys;
}

// ---- the master switch ----------------------------------------------------------------------
// enabled=0 is not a code path of its own: it is the configuration "everything off", which every
// one of these switches already supports on its own. journal=0 is what stops a deposit - under
// the private table a row the mod may not write is a deposit the mod refuses - so nothing new is
// collected, while the journal file, its exports and rescue=1 are untouched.
int g_enabledSaid = -1;

void applyMasterSwitch(UtConfig* c) {
    if (c->enabled != g_enabledSaid) {
        g_enabledSaid = c->enabled;
        if (!c->enabled) {
            logI("config: enabled=0 - no tab, no marks, nothing new is collected");
            logD("what is already stored is untouched, and rescue=1 still hands it back");
        }
    }
    if (c->enabled) return;
    c->journal = 0;
    c->livePages = 0;
    c->pageHotkeys = 0;
    c->groupButtons = 0;
    c->plateSwap = 0;
    c->plateLabel = 0;
    c->layoutOutline = 0;
    c->layoutEdit = 0;
    c->searchButtons = 0;
    c->searchButtonsUnowned = 0;
    c->comparePopup = 0;
    c->tooltipMark = 0;
}

bool g_selfCheckDone = false;

// Logged exactly once per process, on the first file that was actually read. It counts the
// VISIBLE keys only: a hidden key is missing by design. `migrated` says the file has just been
// rewritten in full, so "what is missing" is a statement about the OLD file.
void configSelfCheck(DWORD fileBytes, const UtIniScan& scan, int iniVersion, bool migrated) {
    if (g_selfCheckDone) return;
    g_selfCheckDone = true;
    const int visible = visibleKeyCount();
    if (migrated) {
        logI("config: %d of %d settings kept from the old file, the rest defaulted "
             "(rewritten in full, %u bytes, ini_version=%d)",
             scan.knownAll, countedKeyCount(), (unsigned)configTemplateBytes(), iniVersion);
        return;
    }
    logI("config: %d of %d settings are in the file (%u bytes, a fresh one is %u; ini_version=%d)",
         scan.known, visible, (unsigned)fileBytes, (unsigned)configTemplateBytes(), iniVersion);
    if (scan.known < visible) {
        logI("config: %d setting(s) are not in the file and use their default - delete the file "
             "to get a complete one back",
             visible - scan.known);
    }
}

// The rescue trigger that does not depend on the settings file at all: RESCUE-NOW next to it.
// Presence = "run the rescue"; the file is deleted the moment it is seen (the run has started)
// and the arming is LATCHED, so re-reading a rescue=0 file a second later cannot disarm it before
// rescueTick() has had its frame. The latch is cleared where the rescue itself writes rescue=0
// back (configPersistInt below).
volatile LONG g_rescueLatch = 0;
bool g_rescueTriggerStuck = false;

// `leaf` next to the settings file. Empty on failure.
void rescueTriggerPath(wchar_t* out, size_t cap, const wchar_t* leaf) {
    out[0] = 0;
    if (!g_path[0]) return;
    wcsncpy_s(out, cap, g_path, _TRUNCATE);
    wchar_t* slash = wcsrchr(out, L'\\');
    if (!slash) {
        out[0] = 0;
        return;
    }
    slash[1] = 0;
    wcsncat_s(out, cap, leaf, _TRUNCATE);
}

void checkRescueTrigger() {
    if (g_rescueLatch || g_rescueTriggerStuck || !g_path[0]) return;
    // Windows hides extensions, so the "New > Text Document" spelling is accepted as well.
    static const wchar_t* const kLeaves[2] = {L"RESCUE-NOW", L"RESCUE-NOW.txt"};
    wchar_t p[MAX_PATH];
    p[0] = 0;
    for (int i = 0; i < 2; ++i) {
        rescueTriggerPath(p, MAX_PATH, kLeaves[i]);
        if (p[0] && GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) break;
        p[0] = 0;
    }
    if (!p[0]) return;
    const BOOL gone = DeleteFileW(p);
    if (!gone) g_rescueTriggerStuck = true;  // do not re-arm from a file we cannot consume
    InterlockedExchange(&g_rescueLatch, 1);
    logI("rescue: RESCUE-NOW seen - rescue=1 is armed for this run");
    logD("rescue: the trigger file is \"%S\"", p);
    if (!gone) {
        logW("rescue: the RESCUE-NOW file could not be deleted - delete it by hand once the "
             "rescue has run, or it arms again");
    }
}

// True when `line` (already trimmed at the left) opens a [view:<name>] block.
bool isViewHeader(const char* line) { return _strnicmp(line, "[view:", 6) == 0; }

// True when `line` (already trimmed at the left) assigns exactly `key`.
bool lineIsKey(const char* line, const char* key) {
    const size_t n = strlen(key);
    if (_strnicmp(line, key, n) != 0) return false;
    const char* p = line + n;
    while (*p == ' ' || *p == '\t') ++p;
    return *p == '=';
}

// A hidden row of the [advanced] blob (the main file's and a game's own alike since 42): the table
// row of `key` when it is hidden, else null.
const UtCfgKey* hiddenRow(const char* key) {
    const int idx = findKey(key);
    return (idx >= 0 && keyHidden(kUtCfgKeys[idx])) ? &kUtCfgKeys[idx] : nullptr;
}

// The [section] a key is written under in a settings file (both files have the same sections since
// 42): its table row's, its family's, or null.
const char* sectionOfKey(const char* key) {
    const int idx = findKey(key);
    if (idx >= 0) return kUtCfgKeys[idx].section;
    const UtCfgFamily* f = configFamilyOf(key, nullptr);
    return f ? f->section : nullptr;
}

// "[name]" (blanks around it allowed) -> true and the name in `out`.
bool sectionHeader(const char* trimmed, char* out, size_t cap) {
    if (*trimmed != '[') return false;
    const char* close = strchr(trimmed, ']');
    if (!close) return false;
    const size_t n = (size_t)(close - trimmed - 1);
    if (n + 1 > cap) return false;
    memcpy(out, trimmed + 1, n);
    out[n] = 0;
    return true;
}

// The column `upto` bytes of `s` reach on screen, tabs stopping every 8.
size_t visualCol(const char* s, size_t upto) {
    size_t col = 0;
    for (size_t i = 0; i < upto && s[i]; ++i) col = s[i] == '\t' ? (col / 8 + 1) * 8 : col + 1;
    return col;
}

// The one line-rewriting engine the persist path uses: read the file, replace the assignment of
// every key in `keys` with `values[i]` (a NULL value deletes the line), insert the keys the file
// does not have at the end of their own [section] (at the end of the file when it has none),
// write a temp file next to it and MoveFileEx it over the original, so a reader never sees half a
// file. `blob` (a settings file: uniquetab.ini or a custom game's own) adds the blob's rules: a
// hidden key's commented row `;key=...` is its line when no live line exists, and a hidden key
// written at its default is commented again. `mainFile` (uniquetab.ini itself): an old
// [view:<name>] block is dropped (its lines were ignored with a WARN). In any other file a line
// inside a [view:<name>] block is never matched.
bool persistLines(const wchar_t* path, const char* const* keys, const char* const* values, int n,
                  UtCfgSectionOf sectionOf, bool mainFile, bool blob) {
    if (!path || !path[0] || n <= 0 || n > 8) return false;
    for (int i = 0; i < n; ++i) {
        if (!keys[i] || !*keys[i]) return false;
        if (values[i] && (strchr(values[i], '\r') || strchr(values[i], '\n'))) return false;
    }

    DWORD got = 0;
    char* in = readFileW(path, &got, nullptr);
    if (!in) return false;

    const UtCfgKey* hidden[8];
    for (int i = 0; i < n; ++i) hidden[i] = blob ? hiddenRow(keys[i]) : nullptr;

    // Which keys the file already assigns (live), and which have only a commented blob row.
    bool live[8] = {false, false, false, false, false, false, false, false};
    bool row[8] = {false, false, false, false, false, false, false, false};
    bool scanInView = false;
    for (const char* p = in; p && *p;) {
        const char* t = p;
        while (*t == ' ' || *t == '\t') ++t;
        if (*t == '[') scanInView = isViewHeader(t);
        for (int i = 0; i < n && !scanInView; ++i) {
            if (lineIsKey(t, keys[i])) live[i] = true;
            if (hidden[i] && t[0] == ';' && lineIsKey(t + 1, keys[i])) row[i] = true;
        }
        const char* e = strpbrk(p, "\r\n");
        if (!e) break;
        p = e + ((e[0] == '\r' && e[1] == '\n') ? 2 : 1);
    }
    bool pending[8], rowUsed[8];
    for (int i = 0; i < n; ++i) {
        pending[i] = !live[i] && !row[i] && values[i] != nullptr;
        rowUsed[i] = false;
    }
    // The text a key is written with: ";key=value" for a hidden key at its default.
    auto headOf = [&](int i, char* out, size_t cap) -> int {
        bool comment = false;
        if (hidden[i] && values[i]) {
            char def[300];
            defaultText(*hidden[i], def, sizeof(def));
            comment = !strcmp(values[i], def);
        }
        return _snprintf_s(out, cap, _TRUNCATE, "%s%s=%s", comment ? ";" : "", keys[i],
                           values[i] ? values[i] : "");
    };

    // Rebuild the file line by line: only the listed assignments change. Every line either
    // survives byte for byte, becomes "key=value", or goes; so input + n inserted lines is an
    // upper bound - and if anything ever did not fit we write NOTHING.
    size_t extra = 1024;
    for (int i = 0; i < n; ++i) extra += strlen(keys[i]) + (values[i] ? strlen(values[i]) : 0) + 256;
    const size_t outCap = (size_t)got + extra;
    char* out = (char*)malloc(outCap);
    if (!out) {
        free(in);
        return false;
    }
    size_t o = 0;
    size_t removed = 0;          // bytes of deleted lines, for the "shorter" guard below
    bool overflow = false;
    char section[64] = {0};      // the [section] the rewrite is in
    size_t sectionEnd = 0;       // `out` offset just past its last non-blank line
    bool inSection = false;
    bool inView = false;         // inside a [view:<name>] block

    // Inserts every pending key of `section` at `sectionEnd`, moving the blank lines after it.
    auto flushSection = [&]() {
        if (!inSection || overflow) return;
        for (int i = 0; i < n && !overflow; ++i) {
            if (!pending[i]) continue;
            const char* sec = sectionOf ? sectionOf(keys[i]) : nullptr;
            if (!sec || strcmp(sec, section) != 0) continue;
            char kv[400], line[512];
            if (headOf(i, kv, sizeof(kv)) < 0) {
                overflow = true;
                break;
            }
            const int len = _snprintf_s(line, sizeof(line), _TRUNCATE, "%s\r\n", kv);
            if (len < 0 || o + (size_t)len + 1 > outCap) {
                overflow = true;
                break;
            }
            memmove(out + sectionEnd + len, out + sectionEnd, o - sectionEnd);
            memcpy(out + sectionEnd, line, (size_t)len);
            sectionEnd += (size_t)len;
            o += (size_t)len;
            pending[i] = false;
        }
    };

    char* line = in;
    while (line && *line && !overflow) {
        // Exactly one line ending is consumed, so blank separator lines survive the rewrite.
        char* next = strpbrk(line, "\r\n");
        size_t rawLen = next ? (size_t)(next - line) : strlen(line);
        if (next) {
            const bool crlf = (next[0] == '\r' && next[1] == '\n');
            rawLen += crlf ? 2 : 1;
            *next = 0;
            next += crlf ? 2 : 1;
        }
        const char* trimmed = line;
        while (*trimmed == ' ' || *trimmed == '\t') ++trimmed;
        char head[64];
        if (*trimmed == '[') inView = isViewHeader(trimmed);
        if (!inView && sectionHeader(trimmed, head, sizeof(head))) {
            flushSection();
            _snprintf_s(section, sizeof(section), _TRUNCATE, "%s", head);
            inSection = true;
        }
        if (inView && mainFile) {   // the main file's old blocks go, header and all
            removed += rawLen;
            line = next;
            continue;
        }
        int hit = -1;
        bool hitRow = false;
        for (int i = 0; i < n && !inView && hit < 0; ++i) {
            if (lineIsKey(trimmed, keys[i])) {
                hit = i;
            } else if (hidden[i] && !live[i] && !rowUsed[i] && trimmed[0] == ';' &&
                       lineIsKey(trimmed + 1, keys[i])) {
                hit = i;
                hitRow = true;
            }
        }
        int wrote;
        if (hit >= 0 && !values[hit]) {
            removed += rawLen;   // a NULL value deletes the line
            line = next;
            continue;
        }
        if (hit >= 0) {
            if (hitRow) rowUsed[hit] = true;
            // Keep the line's trailing comment at its original column where it still fits: the
            // file documents itself and persisting a key must not delete its own explanation. A
            // row aligned with tabs stays aligned with tabs.
            const char* eq = strchr(trimmed, '=');
            const char* cmt = eq ? strpbrk(eq, ";#") : nullptr;
            char kv[400];
            const int hn = headOf(hit, kv, sizeof(kv));
            if (hn < 0) { overflow = true; break; }
            if (cmt) {
                bool tabs = false;
                for (const char* q = cmt; q > eq && (q[-1] == ' ' || q[-1] == '\t'); --q)
                    if (q[-1] == '\t') tabs = true;
                char pad[160];
                if (tabs) {
                    // A blob row goes back to the blob's own column, however it was edited.
                    const size_t col = hidden[hit] ? blobColumn()
                                                   : visualCol(trimmed, (size_t)(cmt - trimmed));
                    tabbedTo(pad, sizeof(pad), (size_t)hn, col);
                } else {
                    const size_t col = (size_t)(cmt - trimmed);
                    size_t p = 0;
                    while (p + 1 < sizeof(pad) && (size_t)hn + p < col) pad[p++] = ' ';
                    if (p == 0) pad[p++] = ' ';
                    pad[p] = 0;
                }
                wrote = _snprintf_s(out + o, outCap - o, _TRUNCATE, "%s%s%s\r\n", kv, pad, cmt);
            } else {
                wrote = _snprintf_s(out + o, outCap - o, _TRUNCATE, "%s\r\n", kv);
            }
        } else {
            wrote = _snprintf_s(out + o, outCap - o, _TRUNCATE, "%s\r\n", line);
        }
        if (wrote < 0) { overflow = true; break; }
        o += (size_t)wrote;
        if (*trimmed) sectionEnd = o;   // blank lines stay after anything inserted
        line = next;
    }
    flushSection();
    for (int i = 0; i < n && !overflow; ++i) {
        if (!pending[i]) continue;
        // The file ends inside a [view:<name>] block: the key gets its own [section] header first,
        // or the next read would take it for that view's line.
        const char* sec = sectionOf ? sectionOf(keys[i]) : nullptr;
        if (inView && !mainFile && sec) {
            const int w = _snprintf_s(out + o, outCap - o, _TRUNCATE, "\r\n[%s]\r\n", sec);
            if (w < 0) { overflow = true; break; }
            o += (size_t)w;
            inView = false;
        }
        char kv[400];
        if (headOf(i, kv, sizeof(kv)) < 0) { overflow = true; break; }
        const int w = _snprintf_s(out + o, outCap - o, _TRUNCATE, "%s\r\n", kv);
        if (w < 0) { overflow = true; break; }
        o += (size_t)w;
    }
    free(in);
    // A rewrite may differ from the input only by the values it changed and the lines it deleted.
    // If it ever came out materially shorter than that, something went wrong - write NOTHING
    // rather than replace the file with a truncated one.
    if (overflow || o + removed + 512 < (size_t)got) {
        logW("config: persist REFUSED - the rewrite would shorten \"%S\" (%u -> %u bytes)", path,
             (unsigned)got, (unsigned)o);
        free(out);
        return false;
    }
    const bool ok = writeAtomic(path, out, o);
    free(out);
    if (!ok) return false;
    // Force the next reload to parse what was just written, when it is one of the settings files.
    if (!_wcsicmp(path, g_path)) g_haveStamp = false;
    if (g_ovPath[0] && !_wcsicmp(path, g_ovPath)) g_ovHaveStamp = false;
    return true;
}

// ---- the views: a custom game's own file and the state file ----------------------------------

// THE SEED: what a custom game's own file is written with the first time it is entered, and what
// a pre-complete game file (no ini_version line: the four-key files of ini_version <= 41) is
// completed from (once): the main file's values. The one-shot
// commands start at 0 (a command written in the main file is for the main views), and journal_dir
// is never a game file's value (its line there is rendered commented).
UtConfig seedFrom(const UtConfig& main) {
    UtConfig s = main;
    s.iniVersion = UT_INI_VERSION;
    s.rescue = 0;
    s.journalPrune = 0;
    s.layoutEdit = 0;
    memset(s.journalDir, 0, sizeof(s.journalDir));
    return s;
}
UtConfig seedFromMain() { return seedFrom(g_haveBase ? g_base : UtConfig()); }

// g_cfg from its pieces. Under g_applyLock. True when it changed. While a custom game's own file
// is read, it is the WHOLE configuration - the main file is not consulted, except for journal_dir
// (read at start-up; the generation's journal inputs follow it too) and [keys] (the player's
// hands, the same in every game).
bool publishLocked() {
    UtConfig next = g_ovHave ? g_ovVals : g_base;
    copyMainOnly(&next, g_base);   // journal_dir and [keys]: always the main file's
    next.uniqGroup = g_stGroup;
    next.uniqRow = g_stRow;
    next.ownedOnly = g_stOwned;
    if (g_rescueLatch) next.rescue = 1;
    next.logFlushEachLine = !_stricmp(next.logLevel, "trace") ? 1 : 0;
    applyMasterSwitch(&next);
    const bool changed = memcmp(&next, &g_cfg, sizeof(UtConfig)) != 0;
    g_cfg = next;
    if (changed) InterlockedIncrement(&g_cfgGen);
    return changed;
}

int familyLines(const UtConfig& c) {
    int n = 0;
    for (int i = 0; i < kUtCfgFamilyCount; ++i)
        n += ((const UtCfgFamilyStore*)((const char*)&c + kUtCfgFamilies[i].off))->count;
    return n;
}

// The ini_version a settings file's text names, -1 when it has no such line.
int sniffVersion(const char* buf) {
    for (const char* p = buf; p && *p;) {
        const char* t = p;
        while (*t == ' ' || *t == '\t') ++t;
        if (lineIsKey(t, "ini_version")) return atoi(strchr(t, '=') + 1);
        const char* e = strpbrk(p, "\r\n");
        if (!e) break;
        p = e + 1;
    }
    return -1;
}

// The stamp of the active game's own file as it is now (after the mod rewrote it).
void restampOverlayLocked() {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(g_ovPath, GetFileExInfoStandard, &fa)) {
        g_ovStamp = fa.ftLastWriteTime;
        g_ovHaveStamp = true;
    } else {
        g_ovHaveStamp = false;
    }
}

// Reads the active game's own file into g_ov*. Under g_applyLock. A key the file does not name
// is the compiled default. A file from an OLDER BUILD is migrated once and rewritten whole in the
// current template - one INFO line per game file: every line it has is kept, and every other key
// is
//   - the COMPILED DEFAULT when the file names an ini_version (a complete file, 42 on: its
//     hidden keys at their defaults are commented rows - "missing" - and must stay the defaults,
//     never become the main file's values at the bump);
//   - the main file's value (the seed) when it names none (the pre-complete four-key files of
//     ini_version <= 41, which were completed from the main file by design).
void loadOverlayLocked() {
    g_ovHave = false;
    g_ovVals = UtConfig();
    if (!g_ovPath[0]) return;
    DWORD len = 0;
    FILETIME ft = {0, 0};
    char* buf = readFileW(g_ovPath, &len, &ft);
    if (!buf) {
        g_ovHaveStamp = false;
        return;
    }
    g_ovStamp = ft;
    g_ovHaveStamp = true;
    const int ver = sniffVersion(buf);
    const bool old = ver != UT_INI_VERSION;
    UtConfig parsed = old && ver < 0 ? seedFromMain() : UtConfig();
    UtIniScan scan;
    parseInto(buf, old ? kParseViewOld : kParseView, &parsed, &scan);
    free(buf);
    if (scan.notHere > 0)
        logW("config: journal_dir is read at start-up from the main uniquetab.ini - the line in "
             "\"%S\" is ignored", g_ovPath);
    if (scan.keysHere > 0)
        logW("config: [keys] is read from the main uniquetab.ini only, for every game - %d line(s) "
             "in \"%S\" are ignored: %s", scan.keysHere, g_ovPath, scan.keysHereNames);
    if (scan.stateSeen)
        logW("config: uniq_group/uniq_row/owned_only are not settings - the mod keeps them in "
             "uniquetab.state; the line(s) in \"%S\" are ignored", g_ovPath);
    if (old) {
        parsed.iniVersion = UT_INI_VERSION;
        const bool wrote = writeViewFile(g_ovPath, g_ovKey, parsed, true);
        char from[32];
        if (ver < 0) _snprintf_s(from, sizeof(from), _TRUNCATE, "no ini_version");
        else _snprintf_s(from, sizeof(from), _TRUNCATE, "ini_version=%d", ver);
        if (ver < 0)
            logI("config: mods\\%s\\uniquetab.ini (%s) migrated to a complete file, ini_version=%d - "
                 "%d setting(s) and %d [layout] line(s) kept, every other value copied once from the "
                 "main uniquetab.ini",
                 g_ovKey, from, UT_INI_VERSION, scan.knownAll, scan.family);
        else
            logI("config: mods\\%s\\uniquetab.ini migrated from %s to %d - %d setting(s) and %d "
                 "[layout] line(s) kept, every key it does not set at its default (the main "
                 "uniquetab.ini is not read)",
                 g_ovKey, from, UT_INI_VERSION, scan.knownAll, scan.family);
        if (scan.unknown > 0)
            logI("config: %d line(s) of mods\\%s\\uniquetab.ini this build does not have were "
                 "dropped: %s", scan.unknown, g_ovKey, scan.unknownNames);
        if (wrote)
            restampOverlayLocked();
        else
            logW("config: \"%S\" could NOT be rewritten - the migrated values are in use for this "
                 "run only", g_ovPath);
    } else if (scan.unknown > 0) {
        logW("config: \"%S\": %d line(s) name a setting this build does not have and are "
             "ignored: %s", g_ovPath, scan.unknown, scan.unknownNames);
    }
    g_ovVals = parsed;
    g_ovHave = true;
}

void loadStateLocked() {
    g_stGroup = -1;
    g_stRow = 0;
    g_stOwned = 0;
    if (!g_statePath[0]) return;
    DWORD len = 0;
    char* buf = readFileW(g_statePath, &len, nullptr);
    if (!buf) return;
    UtConfig unused;
    UtIniScan scan;
    parseInto(buf, kParseState, &unused, &scan);
    free(buf);
    if (scan.stateSeen & 1) g_stGroup = scan.stateVals[0] < -1 ? -1 : (scan.stateVals[0] > 255 ? 255 : scan.stateVals[0]);
    if (scan.stateSeen & 2) g_stRow = scan.stateVals[1] < 0 ? 0 : (scan.stateVals[1] > 65535 ? 65535 : scan.stateVals[1]);
    if (scan.stateSeen & 4) g_stOwned = scan.stateVals[2] ? 1 : 0;
}

// The game's own file, written when absent: the whole template with the main file's values in
// force (THE SEED - once; nothing is inherited afterwards). True when it was created now.
bool ensureViewFileLocked(const wchar_t* path, const char* key) {
    if (fileExists(path)) return false;
    if (!writeViewFile(path, key, seedFromMain(), false)) {
        logW("config: \"%S\" could not be written - the custom game %s uses the main settings",
             path, key);
        return false;
    }
    return true;
}

// The paths for g_path + g_view, the game's own file (created when absent) and the state, read.
// Under g_applyLock. True when the game's own file was created now.
bool refreshViewFilesLocked() {
    std::wstring ini, state;
    configViewFiles(g_path, g_view, &ini, &state);
    wcsncpy_s(g_ovPath, ini.c_str(), _TRUNCATE);
    wcsncpy_s(g_statePath, state.c_str(), _TRUNCATE);
    const std::string key = configCustomKey(g_view);
    _snprintf_s(g_ovKey, sizeof(g_ovKey), _TRUNCATE, "%s", key.c_str());
    g_ovHaveStamp = false;
    bool created = false;
    if (g_ovPath[0]) created = ensureViewFileLocked(g_ovPath, g_ovKey);
    loadOverlayLocked();
    loadStateLocked();
    g_viewFilesReady = true;
    return created;
}

// THE MIGRATION OF THE OLD [view:<name>] BLOCKS (ini_version < 39). A block that names a custom
// game moves into that game's own file (created when absent, seeded from the main file's values);
// a vanilla/basemods block has no file to go to and is dropped with a WARN. A line that is not a
// setting of a game's file (a dropped key, journal_dir) is dropped and named.
void migrateViewBlocks(const UtIniScan& scan, const UtConfig& parsed) {
    bool done[kMaxViewLines] = {false};
    for (int i = 0; i < scan.viewLines; ++i) {
        if (done[i]) continue;
        const char* block = scan.views[i].view;
        std::string key = configCustomKey(block);
        if (key.empty() && !strstr(block, "custom-") && strcmp(block, "vanilla") != 0 &&
            strcmp(block, "basemods") != 0) {
            std::string as = std::string("custom-") + block;   // the bare key form
            key = configCustomKey(as.c_str());
        }
        const char* keys[8];
        const char* values[8];
        int n = 0, lines = 0;
        char droppedNames[200] = {0};
        for (int j = i; j < scan.viewLines; ++j) {
            if (strcmp(scan.views[j].view, block) != 0) continue;
            done[j] = true;
            ++lines;
            const ViewLine& v = scan.views[j];
            if (key.empty() || findKey(v.key) < 0 || configMainOnlyKey(v.key)) {
                addName(droppedNames, sizeof(droppedNames), v.key);
                continue;
            }
            int at = -1;
            for (int q = 0; q < n; ++q)
                if (!_stricmp(keys[q], v.key)) at = q;
            if (at < 0 && n < 8) at = n++;
            if (at >= 0) {
                keys[at] = kUtCfgKeys[findKey(v.key)].name;
                values[at] = v.value;
            }
        }
        if (key.empty()) {
            logW("config: [view:%s] (%d line(s): %s) is not carried over - only a custom game has "
                 "a file of its own; set the value in the main file instead",
                 block, lines, droppedNames);
            continue;
        }
        std::wstring ini, state;
        const std::string viewName = "custom-" + key;
        configViewFiles(g_path, viewName.c_str(), &ini, &state);
        if (!fileExists(ini.c_str()) &&
            !writeViewFile(ini.c_str(), key.c_str(), seedFrom(parsed), false)) {
            logW("config: [view:%s] could not be moved - \"%S\" could not be written", block,
                 ini.c_str());
            continue;
        }
        const bool ok =
            n == 0 || persistLines(ini.c_str(), keys, values, n, sectionOfKey, false, true);
        logI("config: [view:%s] moved to mods\\%s\\uniquetab.ini - %d setting(s)%s%s%s",
             block, key.c_str(), n,
             droppedNames[0] ? " (dropped, not a setting of a game's file: " : "",
             droppedNames, droppedNames[0] ? ")" : "");
        if (!ok) logW("config: \"%S\" could not be rewritten - the block's values are lost", ini.c_str());
    }
    if (scan.viewOverflow > 0)
        logW("config: %d more [view:...] line(s) than the move can carry were dropped",
             scan.viewOverflow);
}

// THE STATE KEYS of an old file seed the start-up view's state file once.
void seedStateLocked(const UtIniScan& scan) {
    if (!scan.stateSeen) return;
    std::wstring ini, state;
    configViewFiles(g_path, "", &ini, &state);
    if (fileExists(state.c_str())) {
        logI("config: uniq_group/uniq_row/owned_only left uniquetab.ini; uniquetab.state already "
             "exists and is kept");
        return;
    }
    const int group = (scan.stateSeen & 1) ? scan.stateVals[0] : -1;
    const int row = (scan.stateSeen & 2) ? scan.stateVals[1] : 0;
    const int owned = (scan.stateSeen & 4) ? (scan.stateVals[2] ? 1 : 0) : 0;
    if (writeStateFile(state.c_str(), group, row, owned)) {
        logI("config: uniq_group, uniq_row and owned_only moved to uniquetab.state (group=%d "
             "row=%d owned_only=%d) - they are not settings",
             group, row, owned);
    } else {
        logW("config: \"%S\" could not be written - the tab starts on the materials page",
             state.c_str());
    }
}

}  // namespace

bool configReload(const wchar_t* path) {
    if (path && path != g_path && wcscmp(path, g_path) != 0) {
        AcquireSRWLockExclusive(&g_applyLock);
        wcsncpy_s(g_path, path, _TRUNCATE);
        g_viewFilesReady = false;   // the view's files sit beside this one
        g_haveStamp = false;
        ReleaseSRWLockExclusive(&g_applyLock);
    }

    // BEFORE the "file unchanged" short-cut below: RESCUE-NOW is a separate file and nothing about
    // the settings file changes when it appears, so a check that only ran on a re-parse would
    // never see it (the file is untouched for whole sessions).
    checkRescueTrigger();
    if (g_rescueLatch) g_cfg.rescue = 1;

    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const UtConfig defaults;
        writeIni(path, defaults, false);
        g_haveStamp = false;
        return false;
    }

    AcquireSRWLockExclusive(&g_applyLock);
    // Skip the parse when nothing changed, so this can run every second for free: the main file's
    // stamp, and the active custom game's own file (polled the same way).
    FILETIME ft = {0, 0};
    const bool haveFt = GetFileTime(h, nullptr, nullptr, &ft) != 0;
    const bool mainChanged = !(haveFt && g_haveStamp && ft.dwLowDateTime == g_lastWrite.dwLowDateTime &&
                               ft.dwHighDateTime == g_lastWrite.dwHighDateTime);
    bool ovChanged = false;
    if (g_viewFilesReady && g_ovPath[0]) {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExW(g_ovPath, GetFileExInfoStandard, &fa)) {
            ovChanged = !g_ovHaveStamp ||
                        fa.ftLastWriteTime.dwLowDateTime != g_ovStamp.dwLowDateTime ||
                        fa.ftLastWriteTime.dwHighDateTime != g_ovStamp.dwHighDateTime;
        } else {
            ovChanged = g_ovHave;   // deleted while the game runs: the main values apply again
        }
    }
    if (!mainChanged && !ovChanged && g_viewFilesReady) {
        ReleaseSRWLockExclusive(&g_applyLock);
        CloseHandle(h);
        return true;
    }

    if (mainChanged) {
        g_lastWrite = ft;
        g_haveStamp = true;
        DWORD got = 0;
        char* buf = readAllA(h, &got);
        CloseHandle(h);
        h = INVALID_HANDLE_VALUE;
        if (!buf) {
            ReleaseSRWLockExclusive(&g_applyLock);
            return false;
        }
        UtConfig parsed;  // starts from the defaults, so a key the file does not name takes its own
        UtIniScan scan;
        parseInto(buf, kParseMain, &parsed, &scan);
        free(buf);
        deriveKeys(&parsed);   // [keys] -> parsed.keys, the conflict pass (its WARNs once per edit)
        if (g_rescueLatch) parsed.rescue = 1;

        // A VERSION BUMP MERGES. `parsed` already holds the user's value for every key that still
        // exists and the default for every key that is new, so the migration is: say what
        // happened, move what moved (the state keys, the [view:] blocks), write the same values
        // out in the current layout, and carry on. Nothing is reset.
        const bool migrated = parsed.iniVersion != UT_INI_VERSION;
        if (migrated) {
            const int from = parsed.iniVersion;
            parsed.iniVersion = UT_INI_VERSION;
            if (scan.unknown > 0) {
                logI("config: %d setting(s) this build no longer has were dropped: %s",
                     scan.unknown, scan.unknownNames);
            }
            seedStateLocked(scan);
            if (scan.viewLines > 0) migrateViewBlocks(scan, parsed);
            const bool wrote = writeIni(path, parsed, true);
            g_haveStamp = false;
            if (scan.family > 0 || scan.refused > 0) {
                logD("config: %d keyed line(s) kept through the rewrite, %d refused", scan.family,
                     scan.refused);
            }
            logI("config: migrated from ini_version=%d to %d - %d setting(s) kept, %d new one(s) "
                 "defaulted",
                 from, UT_INI_VERSION, scan.knownAll, countedKeyCount() - scan.knownAll);
            if (!wrote) {
                logW("config: \"%S\" could NOT be rewritten - the migrated values are in use for "
                     "this run only", path);
            }
            // The state file may have just been seeded: read it again for the active view.
            if (g_viewFilesReady) loadStateLocked();
        } else {
            // Same version, so the file is NOT rewritten - a misspelled key is the user's line to
            // fix, and a mod that silently swallowed it would leave them editing a setting that
            // does not exist. Said once per edit: the file is only re-parsed when it changes.
            if (scan.unknown > 0) {
                logW("config: %d line(s) name a setting this build does not have and are ignored: "
                     "%s", scan.unknown, scan.unknownNames);
            }
            if (scan.stateSeen) {
                logW("config: uniq_group/uniq_row/owned_only are not settings - the mod keeps them "
                     "in uniquetab.state; the line(s) here are ignored");
            }
            if (scan.viewLines > 0 || scan.viewOverflow > 0) {
                logW("config: %d [view:...] line(s) are ignored - a custom game's settings live in "
                     "mods\\<name>\\uniquetab.ini now; the block is dropped on the next "
                     "rewrite", scan.viewLines + scan.viewOverflow);
            }
        }
        configSelfCheck(got, scan, parsed.iniVersion, migrated);
        g_base = parsed;
        g_haveBase = true;
    } else {
        CloseHandle(h);
        h = INVALID_HANDLE_VALUE;
    }
    if (!g_viewFilesReady) {
        refreshViewFilesLocked();
    } else if (ovChanged) {
        // Deleted while the game runs: written again from the main file's values (the seed), as
        // on a first entry - "delete it to copy them again".
        const bool gone = !fileExists(g_ovPath);
        if (gone) ensureViewFileLocked(g_ovPath, g_ovKey);
        loadOverlayLocked();
        if (gone)
            logI("config: mods\\%s\\uniquetab.ini was deleted - %s", g_ovKey,
                 g_ovHave ? "written again from the main uniquetab.ini's values"
                          : "it could not be written again, the main settings apply");
        else
            logI("config: the custom game %s's own settings were re-read", g_ovKey);
    }
    const bool changed = publishLocked();
    ReleaseSRWLockExclusive(&g_applyLock);
    // The two settings the logger itself owns, mapped once per reload: the threshold the level
    // wrappers test on every line, and the per-line flush (trace).
    logSetLevel(g_cfg.logLevel);
    logSetFlushEachLine(g_cfg.logFlushEachLine);
    if (changed) {
        logI("config: enabled=%d group=%d row=%d owned_only=%d log_level=%s", g_cfg.enabled,
             g_cfg.uniqGroup, g_cfg.uniqRow, g_cfg.ownedOnly, g_cfg.logLevel);
    }
    return true;
}

long configGeneration() { return InterlockedCompareExchange(&g_cfgGen, 0, 0); }

void configSetView(const char* viewName) {
    const char* name = viewName ? viewName : "";
    AcquireSRWLockExclusive(&g_applyLock);
    if (!strcmp(g_view, name)) {
        ReleaseSRWLockExclusive(&g_applyLock);
        return;
    }
    const bool hadOwn = g_ovHave;
    _snprintf_s(g_view, sizeof(g_view), _TRUNCATE, "%s", name);
    if (!g_path[0]) {   // no file read yet: the first reload reads the view's files
        g_viewFilesReady = false;
        ReleaseSRWLockExclusive(&g_applyLock);
        return;
    }
    const bool created = refreshViewFilesLocked();
    if (g_haveBase) publishLocked();
    const bool own = g_ovHave;
    char key[kUtCfgViewNameCap];
    _snprintf_s(key, sizeof(key), _TRUNCATE, "%s", g_ovKey);
    const int lines = familyLines(g_ovVals);
    ReleaseSRWLockExclusive(&g_applyLock);
    // The logger's two settings follow the view in force at once (configReload maps them only
    // when a file changes): a custom game's log_level is that game's.
    logSetLevel(g_cfg.logLevel);
    logSetFlushEachLine(g_cfg.logFlushEachLine);
    if (created) {
        logI("config: the custom game %s has its own settings now - mods\\%s\\uniquetab.ini, "
             "a complete file with the main uniquetab.ini's values (enabled=%d collect_mi=%d "
             "log_level=%s, %d [layout] line(s)); while it is played only that file is read",
             key, key, g_cfg.enabled, g_cfg.collectMi, g_cfg.logLevel, lines);
    } else if (own) {
        logI("config: the %s view's own settings are in force - mods\\%s\\uniquetab.ini "
             "(enabled=%d collect_mi=%d log_level=%s); the main uniquetab.ini is not read while "
             "it is played",
             name, key, g_cfg.enabled, g_cfg.collectMi, g_cfg.logLevel);
    } else if (hadOwn) {
        logI("config: the %s view uses the main settings", name);
    }
}

void configActiveView(char* out, size_t cap) {
    if (!out || !cap) return;
    AcquireSRWLockShared(&g_applyLock);
    _snprintf_s(out, cap, _TRUNCATE, "%s", g_view);
    ReleaseSRWLockShared(&g_applyLock);
}

int configViewIntFor(const char* viewName, const char* key) {
    const int idx = findKey(key ? key : "");
    if (idx < 0 || kUtCfgKeys[idx].type == kUtCfgStr) return 0;
    const UtCfgKey& k = kUtCfgKeys[idx];
    AcquireSRWLockShared(&g_applyLock);
    int v = *intAt(g_haveBase ? &g_base : &g_cfg, k.off);   // the main file's: the seed
    std::wstring ini;
    configViewFiles(g_path, viewName, &ini, nullptr);
    DWORD len = 0;
    char* buf = ini.empty() ? nullptr : readFileW(ini.c_str(), &len, nullptr);
    if (buf) {
        bool have = false;
        for (const char* p = buf; p && *p;) {
            const char* t = p;
            while (*t == ' ' || *t == '\t') ++t;
            if (lineIsKey(t, k.name)) {   // a commented row (";key=") is not a line
                v = atoi(strchr(t, '=') + 1);
                have = true;
            }
            const char* e = strpbrk(p, "\r\n");
            if (!e) break;
            p = e + 1;
        }
        // a file with an ini_version that does not name it: the compiled default (a file without
        // one is completed from the main file's values when it is migrated, so `v` stays the
        // main file's - loadOverlayLocked's rule)
        if (!have && sniffVersion(buf) >= 0) v = k.def;
        free(buf);
    }
    ReleaseSRWLockShared(&g_applyLock);
    if ((k.flags & kUtCfgRefuse) && (v < k.lo || v > k.hi)) return k.def;   // applyOne's rule
    return v < k.lo ? k.lo : (v > k.hi ? k.hi : v);
}

namespace {

// The state keys: g_cfg at once, and the active view's uniquetab.state rewritten whole (it is the
// mod's own file). Under g_applyLock.
bool persistStateLocked(const char* key, int value) {
    const int st = stateIndex(key);
    if (st == 0) g_stGroup = value;
    if (st == 1) g_stRow = value;
    if (st == 2) g_stOwned = value ? 1 : 0;
    g_cfg.uniqGroup = g_stGroup;
    g_cfg.uniqRow = g_stRow;
    g_cfg.ownedOnly = g_stOwned;
    if (!g_viewFilesReady) {
        std::wstring ini, state;
        configViewFiles(g_path, g_view, &ini, &state);
        wcsncpy_s(g_statePath, state.c_str(), _TRUNCATE);
    }
    if (!g_statePath[0]) return false;
    return writeStateFile(g_statePath, g_stGroup, g_stRow, g_stOwned);
}

// configPersistStr's router. Under g_applyLock. Every key goes to the file whose values are in
// force: a custom game's own file while one is active (written again from the seed first when it
// was deleted), else uniquetab.ini - except journal_dir, which only the main file holds, and the
// state keys, which go to the view's state file.
bool persistRoutedLocked(const char* const* keys, const char* const* values, int n) {
    if (n <= 0 || n > 8) return false;
    for (int i = 0; i < n; ++i) {
        if (!keys[i] || !*keys[i]) return false;
        if (values[i] && (strchr(values[i], '\r') || strchr(values[i], '\n'))) return false;
    }
    bool custom = g_viewFilesReady && g_ovPath[0];
    if (custom && !fileExists(g_ovPath)) ensureViewFileLocked(g_ovPath, g_ovKey);
    if (custom && !fileExists(g_ovPath)) custom = false;   // not writable: the main is in force
    const char* mk[8];
    const char* mv[8];
    const char* gk[8];
    const char* gv[8];
    int mn = 0, gn = 0;
    bool ok = true;
    for (int i = 0; i < n; ++i) {
        if (configIsStateKey(keys[i])) {
            ok = persistStateLocked(keys[i], values[i] ? atoi(values[i]) : 0) && ok;
            continue;
        }
        if (custom && !configMainOnlyKey(keys[i])) {
            gk[gn] = keys[i];
            gv[gn] = values[i];
            ++gn;
        } else {
            mk[mn] = keys[i];
            mv[mn] = values[i];
            ++mn;
        }
    }
    if (gn > 0) ok = persistLines(g_ovPath, gk, gv, gn, sectionOfKey, false, true) && ok;
    if (mn > 0) ok = persistLines(g_path, mk, mv, mn, sectionOfKey, true, true) && ok;
    return ok;
}

}  // namespace

bool configPersistInt(const char* key, int value) {
    if (!key || !*key) return false;
    // The rescue run itself writes rescue=0 back when it is finished; that is what disarms a
    // RESCUE-NOW trigger (see checkRescueTrigger). Nothing else clears the latch.
    if (!_stricmp(key, "rescue") && value == 0 && g_rescueLatch) {
        InterlockedExchange(&g_rescueLatch, 0);
        logD("rescue: RESCUE-NOW trigger disarmed (the rescue run wrote rescue=0 back)");
    }
    char text[16];
    _snprintf_s(text, sizeof(text), _TRUNCATE, "%d", value);
    const char* keys[1] = {key};
    const char* values[1] = {text};
    AcquireSRWLockExclusive(&g_applyLock);
    const bool ok = persistRoutedLocked(keys, values, 1);
    ReleaseSRWLockExclusive(&g_applyLock);
    return ok;
}

bool configPersistStr(const char* const* keys, const char* const* values, int n) {
    if (!keys || !values) return false;
    AcquireSRWLockExclusive(&g_applyLock);
    const bool ok = persistRoutedLocked(keys, values, n);
    ReleaseSRWLockExclusive(&g_applyLock);
    return ok;
}

bool configPersistFile(const wchar_t* path, const char* const* keys, const char* const* values, int n,
                       UtCfgSectionOf sectionOf) {
    if (!keys || !values) return false;
    return persistLines(path, keys, values, n, sectionOf, false, false);
}

}  // namespace ut
