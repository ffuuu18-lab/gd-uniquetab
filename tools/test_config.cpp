// test_config.cpp - the settings files, offline. No game, no engine.
//
//   tools\build_test_config.bat        (needs a vcvars64 shell; the .bat calls it itself)
//
// Everything it expects is DERIVED FROM THE KEY TABLE in src\ut_config.h - there is no magic
// byte count and no second copy of the key list here. It proves, in a temp directory:
//   1. the table and the struct agree on every default; the four visible keys (collect_mi new in 40; max_per_record
//      and rescue hidden and "(untested)" since 41), the four a custom
//      game's own file holds, and none of the dropped/merged/derived/moved keys is a row; the seven
//      [keys] rows are ut_keys.h's actions (hidden text, utKeyParse the check, main file only);
//   2. a fresh file is the whole rendered template: the header, ini_version, [general] with the
//      four keys, [layout] with its two comment lines, [keys] with its seven commented rows on the
//      blob's column, then the [advanced] blob - one commented
//      row per hidden key, every description on one tab column - and the self-check counts the
//      visible keys only;
//   3. a round trip, and a hidden key set by hand goes LIVE in its own row;
//  3b. edit_button: a commented row at 1 right after layout_edit, 0 by hand live and in force;
//   4. a VERSION BUMP KEEPS the user's values (hidden ones too); the file is the table's rendering;
//   5. a bump drops a key this build no longer has, and says which in the log (INFO);
//  4c. 40 -> 44: max_per_record and rescue leave [general] - at their defaults into the blob's commented rows,
//      set (max_per_record=2, rescue=1) as LIVE blob rows; rescue's write-back comments it again;
//  5b. A REAL 38 FILE -> 44 (a 39 file the same way): log_level=debug kept in [general], the six layout lines kept,
//      no live [advanced] row, the 21 dropped keys named, the state keys seed uniquetab.state;
//      a merged key with a value -> one WARN naming its new home; a dropped key in a 39 file ->
//      the "names a setting this build does not have" WARN;
//   6. a value outside its range is clamped and logged, a word that is not a choice is refused;
//   7. the one-shot write-back keeps the file byte for byte; a hidden one goes back to its
//      commented row at the blob's column, and a hidden key written live takes its row in place;
//   8. the RESCUE-NOW trigger file arms rescue=1, is consumed, and stays latched;
//   9. enabled=0 collapses the feature switches and nothing else;
//  10. THE PATH RULES (src\ut_paths.cpp), for both placements the loader allows;
//  11. the keyed [layout] lines: read, refused, kept through a version bump, 16 per family; the
//      hand-only padmat_ lines live in the blob;
//  12. configPersistStr: several keys in one rewrite, inserted at the end of their section,
//      replaced, deleted; a state key goes to uniquetab.state;
//  13. the same family and persist engine over a caller's own table, struct and file;
//  14. a custom game's own mods\<name>\uniquetab.ini: the paths, written populated at
//      the first entry, polled, a [layout] line over the main one (`default` hides it), the
//      editor's SAVE routed there, shared by basemods+custom-<name>, the main values back on
//      vanilla, deleted and written again; a 42 game file bumped to 44 keeps its lines and takes the
//      compiled defaults for the rest (never the main file's values), edit_button a commented row at 1;
//  15. uniquetab.state: one per view folder, written by the mod, read per view;
//  16. the old [view:<name>] blocks: moved into the game's file on the bump (one INFO), a vanilla
//      block not carried over (one WARN); in a 39 file ignored with one WARN, never applied, and
//      dropped by the next rewrite;
//  17. the screen keys pad_screen / label_screen (written when set): absent from a fresh file,
//      written by the editor's SAVE at the end of [layout] with a family line in one rewrite, read
//      back (the text as typed, parsed by ut_layout.h), the rendering = the persisted file, kept
//      through a bump and left out of its counts, refused values (one WARN each, the key unset),
//      a NULL deletes the line, ONE value for every view: a custom game's SAVE writes them to the
//      main file and the game's own file refuses them;
//  18. [keys] (44): a remap by hand in force at the next reload (the generation moves), kept byte for
//      byte by a one-shot's write-back and LIVE through the 43 -> 44 rewrite; a 43 file without
//      the section rewritten once into the 44 template; refused values (two keys, a bare letter,
//      MiddleClick for the editor) one WARN each per edit, the default kept, an empty value the
//      default; the conflict pass's WARN naming both rows; MAIN FILE ONLY - a [keys] line in a
//      game's file one WARN and ignored, the main file's bindings in force in the game (and its
//      edits within a reload), a re-seeded game file without the section;
//  19. zoom_pct (44): a hidden int row of [layout], 100..200, default 100, REFUSED outside (one
//      WARN, 100 kept), its commented line in a fresh file, live by hand, the 43 -> 44 merge of the
//      main file (every value kept, the row added), one value per settings file.
// Exit code 0 = all of it held.
#include <windows.h>

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#include "../src/ut_config.h"
#include "../src/ut_folders.h"
#include "../src/ut_layout.h"
#include "../src/ut_log.h"
#include "../src/ut_paths.h"

// ---- ut_log stubs, with a transcript the checks can search ---------------------------------
static char g_logBuf[65536];
static size_t g_logLen = 0;
static int g_logWarns = 0;   // WARN lines since the last logClear

static void logClear() {
    g_logBuf[0] = 0;
    g_logLen = 0;
    g_logWarns = 0;
}

static bool logHas(const char* needle) { return strstr(g_logBuf, needle) != nullptr; }

namespace ut {
volatile long g_logLevel = UT_LOG_TRACE;  // the harness wants every line, whatever the ini says
static char g_mappedLevel[16] = "";   // what configReload / configSetView last mapped
void logSetLevel(const char* level) { _snprintf_s(g_mappedLevel, sizeof(g_mappedLevel), _TRUNCATE, "%s", level ? level : ""); }
void logAtV(int level, const char* fmt, va_list ap) {
    char line[2048];
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    const char* tags = "EWIDT";
    if (level == 1) ++g_logWarns;
    printf("    [log %c] %s\n", tags[(level < 0 || level > 4) ? 2 : level], line);
    const size_t n = strlen(line);
    if (g_logLen + n + 2 < sizeof(g_logBuf)) {
        memcpy(g_logBuf + g_logLen, line, n);
        g_logLen += n;
        g_logBuf[g_logLen++] = '\n';
        g_logBuf[g_logLen] = 0;
    }
}
void logSetFlushEachLine(int) {}
}  // namespace ut

// "ini_version=<current>" and a line end, so a file written here is never migrated.
#define UT_STR2(x) #x
#define UT_STRV(x) UT_STR2(x)
#define INI_CUR "ini_version=" UT_STRV(UT_INI_VERSION) "\r\n"
// zoom_pct's commented row (44): the first line of [layout] after its blurb, before the family lines.
#define ZOOM_ROW ";zoom_pct=100\t\t\t; the item icons bigger, in percent (100..200): fewer per screen, the icons magnified\r\n"

static int g_fails = 0;
static void check(bool ok, const char* what) {
    printf("  %-62s %s\n", what, ok ? "OK" : "FAIL");
    if (!ok) ++g_fails;
}

static DWORD fileSize(const wchar_t* p) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(p, GetFileExInfoStandard, &fad)) return 0;
    return fad.nFileSizeLow;
}

static char* readAll(const wchar_t* p, DWORD* len) {
    *len = 0;
    HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return nullptr;
    const DWORD size = GetFileSize(h, nullptr);
    char* buf = (char*)malloc((size_t)size + 1);
    if (!buf) {
        CloseHandle(h);
        return nullptr;
    }
    DWORD got = 0;
    ReadFile(h, buf, size, &got, nullptr);
    CloseHandle(h);
    buf[got] = 0;
    *len = got;
    return buf;
}

// ut_config.cpp skips the parse when the file's last-write time EQUALS the one it saw last, so
// every write here has to land on a stamp that differs from the one the file already carries -
// whoever wrote it, this harness or configEnsure. The clock cannot give that: it moves in ~15 ms
// steps, so two writes inside one step share a stamp and the reload is skipped (a rare 5-failure
// run of section 3, which is what the old Sleep(40) was guarding against). The stamp is set
// explicitly and forced strictly past both the file's own and the last one written here.
static ULONGLONG g_lastStamp = 0;   // 100 ns units: the stamp this harness last wrote

static void writeAll(const wchar_t* p, const char* text, DWORD len) {
    ULONGLONG prev = 0;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(p, GetFileExInfoStandard, &fa))
        prev = ((ULONGLONG)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
    HANDLE h = CreateFileW(p, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD w = 0;
    WriteFile(h, text, len, &w, nullptr);
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULONGLONG t = ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime;
    const ULONGLONG second = 10000000ULL;
    if (t <= prev) t = prev + second;
    if (t <= g_lastStamp) t = g_lastStamp + second;
    g_lastStamp = t;
    FILETIME ft;
    ft.dwLowDateTime = (DWORD)t;
    ft.dwHighDateTime = (DWORD)(t >> 32);
    SetFileTime(h, nullptr, nullptr, &ft);   // the system stops stamping this handle itself
    CloseHandle(h);
}

static void writeText(const wchar_t* p, const char* text) {
    writeAll(p, text, (DWORD)strlen(text));
}

// Replaces the first `find` with `repl` IN PLACE. Both must be the same length, so the rest of
// the file - and every offset in it - is untouched.
static bool patch(char* text, const char* find, const char* repl) {
    if (strlen(find) != strlen(repl)) return false;
    char* at = strstr(text, find);
    if (!at) return false;
    memcpy(at, repl, strlen(repl));
    return true;
}

// The value of one table row, out of a UtConfig.
static int intOf(const UtConfig& c, const ut::UtCfgKey& k) {
    return *(const int*)((const char*)&c + k.off);
}
static const char* strOf(const UtConfig& c, const ut::UtCfgKey& k) {
    return (const char*)&c + k.off;
}

// "\r\nname=" - a key at the start of a line, which is how the renderer writes every one of them.
static int countKeyLines(const char* text, const char* name) {
    char needle[96];
    _snprintf_s(needle, sizeof(needle), _TRUNCATE, "\r\n%s=", name);
    int n = 0;
    for (const char* p = strstr(text, needle); p; p = strstr(p + 1, needle)) ++n;
    return n;
}

static const char kUserIni38[] =
    "; ============================================================================\r\n"
    "; Unique Collection Tab - your settings.\r\n"
    ";\r\n"
    "; Edit this file while the game is running: it is re-read once a second. Lines\r\n"
    "; that start with ';' are comments, and so is anything after a ';' on a line.\r\n"
    "; The [blocks] are for reading only - a setting is found by its name.\r\n"
    "; Delete this file to get the defaults back. When the mod updates it for a new\r\n"
    "; version YOUR VALUES ARE KEPT: only the layout and any new setting change.\r\n"
    "; ============================================================================\r\n"
    "ini_version=38\r\n"
    "\r\n"
    "[general]\r\n"
    "; The basics. Everything below only matters while the mod is enabled.\r\n"
    "enabled=1                 ; 0 = the mod goes quiet: no tab, nothing new collected. Nothing is lost\r\n"
    "log_level=debug           ; how much goes in the log: error, warn, info, debug, trace\r\n"
    "ui_scale_pct=0            ; 0 = follow the game's own UI scale, or force one (50..300 percent)\r\n"
    "\r\n"
    "[collection]\r\n"
    "; What the collection accepts, and how the tab starts up.\r\n"
    "max_per_record=1          ; how many copies of one item the collection keeps (0 = no limit)\r\n"
    "soulbound_collect=1       ; 1 = a soulbound or untradeable unique can be dragged in as well\r\n"
    "mp_collect=1              ; 1 = the collection also works while you play multiplayer\r\n"
    "collect_pristine_only=0   ; 1 = refuse items with affixes, components, augments or rerolls\r\n"
    "collect_quick_pass=1      ; 1 = shift-click sends a listed item to your normal stash tab\r\n"
    "owned_only=0              ; the OWN filter at start-up: 1 = show only what you already have\r\n"
    "\r\n"
    "[display]\r\n"
    "; What the mod draws on the Crafting Materials page, and its controls.\r\n"
    "group_buttons=1           ; 1 = draw the category buttons over the page and let them be clicked\r\n"
    "page_hotkeys=1            ; 1 = the wheel scrolls the tab, Ctrl+PageUp/PageDown change group\r\n"
    "search_buttons=1          ; 1 = mark the category buttons that hold a match for your search\r\n"
    "search_buttons_unowned=1  ; 1 = mark groups for items you do NOT own yet as well\r\n"
    "compare_popup=1           ; 1 = hovering a stored item shows the game's own comparison box\r\n"
    "tooltip_mark=1            ; 1 = every item tooltip says whether it is in your collection\r\n"
    "plate_label=1             ; 1 = draw the group name, the counts and the row window over the page\r\n"
    "layout_outline=0          ; 1 = outline the grid, the buttons and the label with their [layout] numbers\r\n"
    "\r\n"
    "[layout]\r\n"
    "; Where the item grid, the buttons and the label sit, in page pixels: x,y,w,h. A key\r\n"
    "; ends in the caravan page size it is for (438x627 is the game's own). A rectangle that\r\n"
    "; does not fit is ignored and the log says why; delete the line to get the default back.\r\n"
    "pad_caption_max=14        ; largest caption size on the buttons; the button size limits it too (6..36)\r\n"
    "; grid_<W>x<H>: the item grid: whole cells from the top-left, the rest stays empty\r\n"
    "; grid_438x627=105,76,333,552\r\n"
    "; pad_<W>x<H>: the buttons: rows and columns are chosen for the largest captions\r\n"
    "; pad_438x627=102,25,320,44\r\n"
    "; label_<W>x<H>: the label line: its height limits plate_label_size\r\n"
    "; label_438x627=105,8,317,16\r\n"
    "; padmat_<W>x<H>: the buttons on the game's own materials page, when they would cover its boxes\r\n"
    "; padmat_438x627=102,25,320,44\r\n"
    "grid_438x627=99,73,320,552\r\n"
    "pad_438x627=98,27,320,44\r\n"
    "label_438x627=103,4,333,23\r\n"
    "grid_974x659=46,218,928,374\r\n"
    "pad_974x659=0,1,780,153\r\n"
    "label_974x659=428,165,317,23\r\n"
    "\r\n"
    "[files]\r\n"
    "; Where your collection is written, and what is exported beside it.\r\n"
    "journal_dir=              ; where the collection file is kept. Empty = the mod's own folder\r\n"
    "export_gds=1              ; write uniq-export.gds for GD Stash: 0 off, 1 the collection, 2 all\r\n"
    "export_csv=0              ; write uniq-export.csv for a spreadsheet: the same three settings\r\n"
    "\r\n"
    "[one-shot]\r\n"
    "; Commands. Set one to 1 with the caravan window open; the mod sets it back to 0.\r\n"
    "; The rescue has a second trigger that needs no editing: put an empty file called\r\n"
    "; RESCUE-NOW next to this one. The mod sees it, deletes it and runs the rescue.\r\n"
    "rescue=0                  ; 1 = hand every stored item back to the character, then set itself to 0\r\n"
    "journal_prune=0           ; 1 = drop the entries that are no longer stored, then set itself to 0\r\n"
    "layout_edit=0             ; 1 = open the in-game layout editor on the next collection group, then set itself to 0\r\n"
    "\r\n"
    "[advanced]\r\n"
    "; Tuning. A value outside the range in its line is clamped, and the log says so.\r\n"
    "live_cache_max=4096       ; item displays kept in memory while a character is loaded (64..65536)\r\n"
    "search_sweep=32           ; records your search checks per tick: higher marks sooner (1..256)\r\n"
    "plate_count_ms=1000       ; shortest gap between two counts of the collection, ms (100..60000)\r\n"
    "take_watch_ms=400         ; how often a take is looked for while the tab is open, ms (50..60000)\r\n"
    "identity_window_ms=250    ; how long the mod waits for the game to finish a take, ms (50..5000)\r\n"
    "pad_y=25                  ; top of the button pad, in page pixels (8..60)\r\n"
    "pad_h=14                  ; height of one button, in page pixels (12..20)\r\n"
    "pad_gap=1                 ; gap between two buttons, in page pixels (0..4)\r\n"
    "plate_label_size=13       ; height of the label text, in page pixels (8..32)\r\n"
    "plate_swap=1              ; 1 = the background picture follows the group, 0 = always the vanilla one\r\n"
    "plate_ground=             ; a modded page's flat background, hex RRGGBB. Empty = the vanilla tone\r\n"
    "tooltip_text_yes=         ; the tooltip line for an item you have. Empty = the built-in wording\r\n"
    "tooltip_text_no=          ; and the line for one you do not. Empty = the built-in wording\r\n"
    "tooltip_class_yes=44      ; colour of that line, a game text style number (1..84)\r\n"
    "tooltip_class_no=74       ; colour of the other line, a game text style number (1..84)\r\n"
    "log_flush_each_line=0     ; 1 = write every log line at once (slow; for chasing a crash)\r\n"
    "uniq_group=28             ; WRITTEN BY THE MOD: the group the tab last showed (-1 = vanilla)\r\n"
    "uniq_row=0                ; WRITTEN BY THE MOD: the row the tab was last scrolled to\r\n"
    "text_language=EN          ; the suffix of resources\\Text_<lang>.arc the catalogue takes item names from\r\n"
    "\r\n"
    "; ============================================================================\r\n"
    "; Per game. A [view:<name>] block holds settings for one game only: while that\r\n"
    "; game is played they replace the values above. A block ends at the next [header].\r\n"
    "; <name> is a folder under views\\ (custom-reignofterror), the game's own key\r\n"
    "; (reignofterror), vanilla or basemods. Only these settings may be set in one:\r\n"
    "; text_language, plate_ground, plate_label_size, pad_caption_max, pad_y, pad_h, pad_gap.\r\n"
    "; [view:reignofterror]\r\n"
    "; plate_ground=101010\r\n"
    "; ============================================================================\r\n";

// Every row of the [advanced] blob - commented or live - has its "; description" at one visual
// column (tabs stop every 8). Returns the number of rows, or -1 when one is off the column.
static int blobRows(const char* text, size_t* colOut) {
    const char* at = strstr(text, "\r\n[advanced]\r\n");
    if (!at) return -1;
    at += strlen("\r\n[advanced]\r\n");
    int rows = 0;
    size_t col = 0;
    while (*at) {
        const char* e = strstr(at, "\r\n");
        const size_t len = e ? (size_t)(e - at) : strlen(at);
        char line[512];
        _snprintf_s(line, sizeof(line), _TRUNCATE, "%.*s", (int)len, at);
        const char* eq = strchr(line, '=');
        const char* sep = eq ? strstr(eq, "\t; ") : nullptr;
        if (eq && sep) {
            const char* c = sep;
            while (*c == '\t') ++c;
            size_t v = 0;
            for (const char* p = line; p < c; ++p) v = *p == '\t' ? (v / 8 + 1) * 8 : v + 1;
            if (rows == 0) col = v;
            const char* t0 = sep;
            while (t0 > line && t0[-1] == '\t') --t0;
            if (v != col && (size_t)(t0 - line) < col) return -1;  // only a long live row: 1 tab
            ++rows;
        }
        if (!e) break;
        at = e + 2;
    }
    if (colOut) *colOut = col;
    return rows;
}

static bool sameFile(const wchar_t* p, const char* expect) {
    DWORD len = 0;
    char* text = readAll(p, &len);
    const bool ok = text && len == strlen(expect) && memcmp(text, expect, len) == 0;
    if (!ok && text) printf("    --- on disk (%u bytes):\n%s\n    ---\n", len, text);
    free(text);
    return ok;
}

static bool hasText(const wchar_t* p, const char* needle) {
    DWORD len = 0;
    char* text = readAll(p, &len);
    const bool ok = text && strstr(text, needle) != nullptr;
    free(text);
    return ok;
}

static bool exists(const wchar_t* p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

// The rows written only while set (pad_screen, label_screen), counted from the table.
static int whenSetRows() {
    int n = 0;
    for (int i = 0; i < ut::kUtCfgKeyCount; ++i)
        if (ut::kUtCfgKeys[i].flags & ut::kUtCfgWhenSet) ++n;
    return n;
}

// The hidden rows that live in [layout] (zoom_pct), not in the [advanced] blob.
static int layoutHiddenRows() {
    int n = 0;
    for (int i = 0; i < ut::kUtCfgKeyCount; ++i)
        if ((ut::kUtCfgKeys[i].flags & ut::kUtCfgHidden) && !strcmp(ut::kUtCfgKeys[i].section, "layout")) ++n;
    return n;
}

// The [keys] rows, counted from the table.
static int keysRows() {
    int n = 0;
    for (int i = 0; i < ut::kUtCfgKeyCount; ++i)
        if (!strcmp(ut::kUtCfgKeys[i].section, "keys")) ++n;
    return n;
}

// The [layout] lines of a real file, as a player's game wrote them.
static void userLayout(UtConfig* c) {
    ut::configFamilySet(&c->layGrid, "438x627", "99,73,320,552");
    ut::configFamilySet(&c->layPad, "438x627", "98,27,320,44");
    ut::configFamilySet(&c->layLabel, "438x627", "103,4,333,23");
    ut::configFamilySet(&c->layGrid, "974x659", "46,218,928,374");
    ut::configFamilySet(&c->layPad, "974x659", "0,1,780,153");
    ut::configFamilySet(&c->layLabel, "974x659", "428,165,317,23");
}

// ---- the custom games' folders: mods\<leaf>\ and mods\<leaf>\basemods\ ---------------
// Pure path arithmetic (ut_folders.h + configViewFiles): the game's own settings in mods\<leaf>\
// for both variants, each variant's state in its own folder, the start-up views in the root,
// and a name that is not a safe folder name never becomes one. Nothing here touches the disk.
static void folderTests(const wchar_t* ini, const wchar_t* dir, const std::wstring& rootState) {
    printf("\n14b. the custom games' folders: mods\\<leaf>\\, mods\\<leaf>\\basemods\\\n");
    check(ut::utCustomViewRel("custom-grimarillion") == "mods\\grimarillion" &&
              ut::utCustomViewRel("basemods+custom-grimarillion") == "mods\\grimarillion\\basemods",
          "custom-x -> mods\\x, basemods+custom-x -> mods\\x\\basemods");
    check(ut::utCustomGameRel("custom-grimarillion") == "mods\\grimarillion" &&
              ut::utCustomGameRel("basemods+custom-grimarillion") == "mods\\grimarillion" &&
              ut::utCustomGameRelLeaf("grimarillion") == "mods\\grimarillion",
          "the GAME folder is the same for both variants and for the bare key");
    check(ut::utCustomViewRel("vanilla").empty() && ut::utCustomViewRel("basemods").empty() &&
              ut::utCustomViewRel("").empty() && ut::utCustomViewRel(nullptr).empty(),
          "the start-up views have no folder - their files are in the root");
    check(ut::utCustomViewRel("custom-..").empty() && ut::utCustomViewRel("custom-.").empty() &&
              ut::utCustomViewRel("custom-a\\b").empty() && ut::utCustomViewRel("custom-").empty() &&
              ut::utCustomViewRel("custom-a b").empty() && ut::utCustomGameRelLeaf("..").empty(),
          "a name that is not a safe folder name never becomes a folder");
    std::wstring gi, gs, bi, bs, xi, xs;
    ut::configViewFiles(ini, "custom-grimarillion", &gi, &gs);
    ut::configViewFiles(ini, "basemods+custom-grimarillion", &bi, &bs);
    ut::configViewFiles(ini, "custom-..", &xi, &xs);
    wchar_t want[MAX_PATH];
    _snwprintf_s(want, _TRUNCATE, L"%s\\mods\\grimarillion\\uniquetab.ini", dir);
    check(gi == want && bi == want, "both variants' settings: mods\\grimarillion\\uniquetab.ini");
    _snwprintf_s(want, _TRUNCATE, L"%s\\mods\\grimarillion\\uniquetab.state", dir);
    check(gs == want, "custom-grimarillion's state: mods\\grimarillion\\uniquetab.state");
    _snwprintf_s(want, _TRUNCATE, L"%s\\mods\\grimarillion\\basemods\\uniquetab.state", dir);
    check(bs == want, "basemods+custom-grimarillion's state: mods\\grimarillion\\basemods\\uniquetab.state");
    check(xi.empty() && xs == rootState, "custom-.. has no file of its own: the main state file");
    check(gi.find(L"\\views\\") == std::wstring::npos && bs.find(L"\\views\\") == std::wstring::npos,
          "no path names views\\ any more");
}

// ---- 19. zoom_pct ([layout], hidden, 44) ------------------------------------------------------
// The row (an int of [layout], hidden, 100..200, default 100, REFUSED outside - not clamped), its
// commented line in [layout] of a fresh file, a value by hand live in that line and in force, the
// refusals (one WARN each, 100 kept), the 43 -> 44 merge of the main file (every value kept, the
// row added at 100) and one value per settings file (a custom game's own).
static void zoomTests(const wchar_t* ini, const std::wstring& gameIni) {
    printf("\n19. zoom_pct - a hidden row of [layout], 100..200, refused outside, per settings file\n");
    int idx = -1;
    for (int i = 0; i < ut::kUtCfgKeyCount; ++i)
        if (!strcmp(ut::kUtCfgKeys[i].name, "zoom_pct")) idx = i;
    check(idx >= 0, "zoom_pct is a row of the table");
    if (idx < 0) return;
    const ut::UtCfgKey& k = ut::kUtCfgKeys[idx];
    const UtConfig d;
    check(!strcmp(k.section, "layout") && k.type == ut::kUtCfgInt && k.def == 100 && k.lo == 100 &&
              k.hi == 200 && k.flags == (ut::kUtCfgHidden | ut::kUtCfgRefuse) && d.zoomPct == 100 &&
              k.off == offsetof(UtConfig, zoomPct),
          "an int of [layout], hidden, 100..200, default 100 (the member's), refused outside");
    // a fresh file: the commented row in [layout], once, nowhere else
    DeleteFileW(ini);
    ut::configReload(ini);
    logClear();
    ut::configReload(ini);
    {
        DWORD len = 0;
        char* text = readAll(ini, &len);
        const char* lay = strstr(text, "\r\n[layout]\r\n");
        const char* adv = strstr(text, "\r\n[advanced]\r\n");
        const char* row = strstr(text, "\r\n" ZOOM_ROW);
        int rows = 0;
        for (const char* q = strstr(text, "zoom_pct"); q; q = strstr(q + 1, "zoom_pct")) ++rows;
        check(lay && adv && row && lay < row && row < adv && rows == 1 && ut::g_cfg.zoomPct == 100,
              "a fresh file: ;zoom_pct=100 once, a commented row of [layout] (not of the blob); 100 in force");
        int visible = 0;
        for (int i = 0; i < ut::kUtCfgKeyCount; ++i)
            if (!(ut::kUtCfgKeys[i].flags & (ut::kUtCfgHidden | ut::kUtCfgWhenSet))) ++visible;
        check(visible == 4 && !strstr(text, "\r\nzoom_pct="), "the four visible keys stay four (zoom_pct is no live line)");
        free(text);
    }
    // by hand: live in its row, in force, the generation moves
    {
        DWORD len = 0;
        char* text = readAll(ini, &len);
        std::string t(text);
        free(text);
        const size_t at = t.find(";zoom_pct=100\t");
        if (at != std::string::npos) t.replace(at, 14, "zoom_pct=150\t");
        writeText(ini, t.c_str());
        const long g0 = ut::configGeneration();
        logClear();
        ut::configReload(ini);
        check(at != std::string::npos && ut::g_cfg.zoomPct == 150 && ut::configGeneration() != g0 &&
                  !logHas("zoom_pct=150 is outside"),
              "zoom_pct=150 by hand: in force at the next reload, the generation moves");
        char again[16384];
        const size_t n = ut::configRender(again, sizeof(again), ut::g_cfg);
        const char* live = strstr(again, "\r\nzoom_pct=150\t\t\t; the item icons bigger");
        check(n > 0 && live && live < strstr(again, "\r\n[advanced]\r\n") && !strstr(again, ";zoom_pct="),
              "rendered: a LIVE line in its [layout] row, description kept");
        writeText(ini, std::string(again, n).c_str());
        ut::configReload(ini);
        check(ut::g_cfg.zoomPct == 150, "and it reads back");
    }
    // the refusals: outside 100..200 (or not a number) is one WARN and 100, never a clamp
    struct Bad { const char* text; const char* warn; };
    const Bad bads[4] = {{"250", "zoom_pct=250 is outside 100..200 - ignored, using 100"},
                         {"99", "zoom_pct=99 is outside 100..200 - ignored, using 100"},
                         {"1500", "zoom_pct=1500 is outside 100..200 - ignored, using 100"},
                         {"big", "zoom_pct=big is outside 100..200 - ignored, using 100"}};
    for (const Bad& b : bads) {
        std::string t = INI_CUR "[layout]\r\nzoom_pct=";
        t += b.text;
        t += "\r\n";
        writeText(ini, t.c_str());
        logClear();
        ut::configReload(ini);
        int warns = 0;
        for (const char* q = strstr(g_logBuf, " is outside 100..200"); q; q = strstr(q + 1, " is outside 100..200")) ++warns;
        char what[128];
        _snprintf_s(what, sizeof(what), _TRUNCATE, "zoom_pct=%s: refused with one WARN, 100 in force", b.text);
        check(ut::g_cfg.zoomPct == 100 && logHas(b.warn) && warns == 1, what);
    }
    for (int v : {100, 125, 200}) {
        char t[96];
        _snprintf_s(t, sizeof(t), _TRUNCATE, INI_CUR "[layout]\r\nzoom_pct=%d\r\n", v);
        writeText(ini, t);
        logClear();
        ut::configReload(ini);
        char what[96];
        _snprintf_s(what, sizeof(what), _TRUNCATE, "zoom_pct=%d is taken as it is", v);
        check(ut::g_cfg.zoomPct == v && !logHas("zoom_pct=") , what);
    }
    // THE 43 -> 44 MERGE: a 43 main file (no zoom_pct row) keeps every value and gains the row
    {
        UtConfig c;
        c.tooltipMark = 0;
        c.editButton = 0;
        c.plateLabelSize = 20;
        ut::configFamilySet(&c.layGrid, "438x627", "105,120,333,460");
        static char r[16384];
        const size_t n = ut::configRender(r, sizeof(r), c);
        std::string t(r, n);
        const size_t v = t.find("\r\nini_version=" UT_STRV(UT_INI_VERSION) "\r\n");
        const size_t z = t.find("\r\n" ZOOM_ROW);
        const bool shaped = n > 0 && v != std::string::npos && z != std::string::npos;
        if (shaped) {
            t.erase(z, strlen("\r\n" ZOOM_ROW) - 2);   // 43 had no zoom_pct row
            const size_t ks = t.find("\r\n[keys]\r\n"), ke = t.find("\r\n[advanced]\r\n");
            if (ks != std::string::npos && ke != std::string::npos && ks < ke) t.erase(ks, ke - ks);   // nor [keys]
            t.replace(v, 18, "\r\nini_version=43\r\n");
        }
        check(shaped && t.find("zoom_pct") == std::string::npos && t.find("\r\n[keys]\r\n") == std::string::npos &&
                  t.find("\r\nini_version=43\r\n") != std::string::npos,
              "a 43 main file: tooltip_mark=0, edit_button=0, plate_label_size=20, a grid line, no zoom_pct, no [keys]");
        writeText(ini, t.c_str());
        logClear();
        ut::configReload(ini);
        check(logHas("migrated from ini_version=43 to " UT_STRV(UT_INI_VERSION)) && ut::g_cfg.zoomPct == 100 &&
                  ut::g_cfg.tooltipMark == 0 && ut::g_cfg.editButton == 0 && ut::g_cfg.plateLabelSize == 20 &&
                  !strcmp(ut::configFamilyValue(ut::g_cfg, "grid_438x627"), "105,120,333,460"),
              "it migrates: every value kept, zoom_pct 100");
        check(sameFile(ini, std::string(r, n).c_str()) && hasText(ini, "\r\n" ZOOM_ROW),
              "rewritten as the 44 file of its values byte for byte: ;zoom_pct=100 in [layout]");
        logClear();
        ut::configReload(ini);
        check(!logHas("migrated"), "once");
    }
    // ONE VALUE PER SETTINGS FILE: a custom game's own zoom, the main file's in vanilla
    {
        writeText(ini, INI_CUR "[layout]\r\nzoom_pct=120\r\n");
        ut::configReload(ini);
        writeText(gameIni.c_str(), INI_CUR "[layout]\r\nzoom_pct=175\r\n");
        ut::configSetView("custom-reignofterror");
        const int inGame = ut::g_cfg.zoomPct;
        ut::configSetView("vanilla");
        const int inVanilla = ut::g_cfg.zoomPct;
        writeText(gameIni.c_str(), INI_CUR "[general]\r\nenabled=1\r\n");
        ut::configSetView("custom-reignofterror");
        const int unnamed = ut::g_cfg.zoomPct;
        ut::configSetView("vanilla");
        char what[160];
        _snprintf_s(what, sizeof(what), _TRUNCATE, "a custom game's file says 175, the main 120: %d there, %d in "
                    "vanilla; a game file that names none: %d (the default, not the main file's)",
                    inGame, inVanilla, unnamed);
        check(inGame == 175 && inVanilla == 120 && unnamed == 100, what);
        writeText(ini, INI_CUR "[general]\r\nenabled=1\r\n");
        ut::configReload(ini);
    }
}

int main() {
    // A folder of this run's own: two harnesses started at once, or one left behind by a killed
    // run, must never read each other's ini.
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    wchar_t leaf[64];
    _snwprintf_s(leaf, _TRUNCATE, L"ut-config-test-%lu-%llu", GetCurrentProcessId(),
                 (unsigned long long)GetTickCount64());
    wcscat_s(dir, leaf);
    if (!CreateDirectoryW(dir, nullptr)) {
        printf("cannot create %S (error %lu)\n", dir, GetLastError());
        return 2;
    }
    wchar_t ini[MAX_PATH], trigger[MAX_PATH], triggerTxt[MAX_PATH], state[MAX_PATH];
    _snwprintf_s(ini, _TRUNCATE, L"%s\\uniquetab.ini", dir);
    _snwprintf_s(trigger, _TRUNCATE, L"%s\\RESCUE-NOW", dir);
    _snwprintf_s(triggerTxt, _TRUNCATE, L"%s\\RESCUE-NOW.txt", dir);
    _snwprintf_s(state, _TRUNCATE, L"%s\\uniquetab.state", dir);
    printf("settings file = %S\n", ini);
    printf("%d keys in %d sections, a fresh file is %u bytes\n\n", ut::kUtCfgKeyCount,
           ut::kUtCfgSectionCount, (unsigned)ut::configTemplateBytes());

    // ---- 1. the table and the struct agree ---------------------------------------------------
    printf("1. the table's defaults are the struct's defaults; the visible and per-game rows\n");
    {
        const UtConfig d;
        int bad = 0;
        for (int i = 0; i < ut::kUtCfgKeyCount; ++i) {
            const ut::UtCfgKey& k = ut::kUtCfgKeys[i];
            if (k.type == ut::kUtCfgStr) {
                if (strcmp(strOf(d, k), k.defStr) != 0) {
                    printf("    %s: table \"%s\" vs struct \"%s\"\n", k.name, k.defStr,
                           strOf(d, k));
                    ++bad;
                }
            } else {
                if (intOf(d, k) != k.def) {
                    printf("    %s: table %d vs struct %d\n", k.name, k.def, intOf(d, k));
                    ++bad;
                }
            }
            if (k.type != ut::kUtCfgStr && (k.def < k.lo || k.def > k.hi)) {
                printf("    %s: the default %d is outside its own range %d..%d\n", k.name, k.def,
                       k.lo, k.hi);
                ++bad;
            }
            for (int j = 0; j < i; ++j)
                if (!strcmp(ut::kUtCfgKeys[j].name, k.name)) {
                    printf("    %s: the table names it twice\n", k.name);
                    ++bad;
                }
        }
        check(bad == 0, "every row matches its member, and no name is used twice");
        char visible[256] = {0}, mainOnly[256] = {0}, whenSet[256] = {0};
        for (int i = 0; i < ut::kUtCfgKeyCount; ++i) {
            const ut::UtCfgKey& k = ut::kUtCfgKeys[i];
            if (k.flags & ut::kUtCfgWhenSet) {
                strcat_s(whenSet, k.name);
                strcat_s(whenSet, " ");
                if ((k.flags & ut::kUtCfgHidden) || k.type != ut::kUtCfgStr ||
                    !k.check || strcmp(k.section, "layout") != 0 || k.defStr[0])
                    ++bad;
            } else if (!(k.flags & ut::kUtCfgHidden)) {
                strcat_s(visible, k.name);
                strcat_s(visible, " ");
            }
            if (ut::configMainOnlyKey(k.name)) {
                strcat_s(mainOnly, k.name);
                strcat_s(mainOnly, " ");
            }
            if (k.flags & ~(ut::kUtCfgHidden | ut::kUtCfgWhenSet | ut::kUtCfgRefuse)) ++bad;   // no other flag exists
            if ((k.flags & ut::kUtCfgRefuse) && k.type != ut::kUtCfgInt) ++bad;   // an int row's only
        }
        printf("    visible: %s\n    main file only: %s\n", visible, mainOnly);
        check(!strcmp(visible, "enabled collect_mi tooltip_mark log_level "),
              "four keys are visible: enabled, collect_mi, tooltip_mark, log_level");
        int untested = 0;
        for (int i = 0; i < ut::kUtCfgKeyCount; ++i) {
            const ut::UtCfgKey& k = ut::kUtCfgKeys[i];
            const size_t cl = strlen(k.comment);
            const bool marked = cl > 11 && !strcmp(k.comment + cl - 11, " (untested)");
            const bool two = !strcmp(k.name, "max_per_record") || !strcmp(k.name, "rescue");
            if (two && marked && (k.flags & ut::kUtCfgHidden) && !strcmp(k.section, "advanced")) ++untested;
            else if (marked || two) ++bad;
        }
        check(untested == 2 && bad == 0,
              "max_per_record and rescue are hidden [advanced] rows, their descriptions end in \" (untested)\" "
              "(and no other row's)");
        check(!strcmp(mainOnly, "rows_up rows_down button_prev button_next rows_wheel button_wheel "
                                "editor_open journal_dir ") &&
                  bad == 0 && !ut::configMainOnlyKey("plate_label") && ut::configMainOnlyKey("ROWS_UP") &&
                  !ut::configMainOnlyKey("enabled") && !ut::configMainOnlyKey("grid_438x627") &&
                  !ut::configMainOnlyKey("rows"),
              "every key is a key of both files but journal_dir (read at start-up) and the seven [keys] rows");
        {
            // [keys]: one hidden text row per ut_keys.h action, in its order, its default the
            // action's, checked by utKeyParse (a refused value has a reason)
            int at = 0, keyBad = 0;
            for (int i = 0; i < ut::kUtCfgKeyCount; ++i) {
                const ut::UtCfgKey& k = ut::kUtCfgKeys[i];
                if (strcmp(k.section, "keys") != 0) continue;
                if (at >= kUtKeyActCount || strcmp(k.name, kUtKeyActions[at].name) != 0 ||
                    strcmp(k.defStr, kUtKeyActions[at].def) != 0 || k.type != ut::kUtCfgStr ||
                    k.flags != ut::kUtCfgHidden || !k.check || k.check(k.defStr) != nullptr ||
                    k.check("Ctrl+Q+W") == nullptr || k.cap < 32)
                    ++keyBad;
                ++at;
            }
            check(at == kUtKeyActCount && keyBad == 0,
                  "[keys]: seven hidden text rows, ut_keys.h's actions in order, their defaults, utKeyParse as the check");
            const UtConfig dk;
            const UtKeys kd;
            check(!memcmp(&dk.keys, &kd, sizeof(kd)), "UtConfig's keys member is UtKeys' defaults");
        }
        printf("    written when set: %s\n", whenSet);
        check(!strcmp(whenSet, "pad_screen label_screen ") && bad == 0,
              "pad_screen and label_screen: [layout] text, checked, empty = unset");
        static const char* const kDropped[] = {
            "soulbound_collect", "collect_quick_pass", "group_buttons", "page_hotkeys",
            "search_buttons", "search_buttons_unowned", "compare_popup", "search_sweep",
            "plate_count_ms", "take_watch_ms", "identity_window_ms", "pad_y", "pad_h", "pad_gap",
            "tooltip_class_yes", "tooltip_class_no", "text_language", "log_flush_each_line",
            "tooltip_text_yes", "tooltip_text_no", "live_cache_max", "uniq_group", "uniq_row",
            "owned_only"};
        int still = 0;
        for (const char* n : kDropped)
            for (int i = 0; i < ut::kUtCfgKeyCount; ++i)
                if (!strcmp(ut::kUtCfgKeys[i].name, n)) {
                    printf("    %s is still a row\n", n);
                    ++still;
                }
        check(still == 0, "the 24 dropped, merged, derived and moved keys are not rows");
        check(d.soulboundCollect == 1 && d.padY == 25 && d.padH == 14 && d.padGap == 1 &&
                  d.tooltipClassYes == 44 && d.tooltipClassNo == 74 && !strcmp(d.textLanguage, "EN") &&
                  d.liveCacheMax == 4096 && d.searchSweep == 32,
              "their members keep the fixed values the reading code uses");
        check(ut::configIsStateKey("uniq_group") && ut::configIsStateKey("OWNED_ONLY") &&
                  !ut::configIsStateKey("rescue"),
              "uniq_group, uniq_row, owned_only are the state keys");
    }

    // ---- 2. a fresh file ---------------------------------------------------------------------
    printf("\n2. the first run writes the whole file: four keys, [layout], the blob at the end\n");
    logClear();
    ut::configReload(ini);  // no file yet -> writes it, returns false
    const DWORD tpl = fileSize(ini);
    printf("    on disk = %u bytes\n", tpl);
    check(tpl == (DWORD)ut::configTemplateBytes(), "the file is exactly what the table renders");
    logClear();
    check(ut::configReload(ini), "the second reload parses it");
    check(logHas("4 of 4 settings are in the file"), "the self-check counts the visible keys only");
    check(!logHas("not in the file and use their default"), "and nags about nothing");
    check(ut::g_cfg.iniVersion == UT_INI_VERSION && UT_INI_VERSION == 44, "ini_version is 44");
    {
        DWORD len = 0;
        char* text = readAll(ini, &len);
        printf("%s", text);
        static const char* const kHead =
            "; ============================================================================\r\n"
            "; Unique Collection Tab - your settings.\r\n"
            "; Edit it while the game runs: it is re-read once a second. Anything after a\r\n"
            "; ';' is a comment. Delete this file to get the defaults back.\r\n"
            "; More settings are at the end, commented out. Your buttons (uniquetab-buttons.ini)\r\n"
            "; and your own groups (uniquetab-groups.ini): README.md, section \"Settings\".\r\n";
        check(!strncmp(text, kHead, strlen(kHead)),
              "the header says what the file is, and names the buttons file and the groups file");
        static const char* const kGeneral =
            "\r\nini_version=44\r\n"
            "\r\n[general]\r\n"
            "enabled=1                 ; 0 = the mod goes quiet: no tab, nothing new collected. Nothing is lost\r\n"
            "collect_mi=1              ; 1 = monster infrequents are collected, one roll per item; 0 = they are not, and the ones you stored stay in the collection, hidden until it is 1 again (from the next start)\r\n"
            "tooltip_mark=1            ; 1 = every item tooltip says whether it is in your collection\r\n"
            "log_level=info            ; how much goes in the log: error, warn, info, debug, trace\r\n"
            "\r\n[layout]\r\n"
            "; Where the item grid, the buttons and the label sit. The in-game editor writes these\r\n"
            "; lines: the EDIT button on the pad, or the editor_open chord of [keys] (Ctrl+right-click\r\n"
            "; unless you change it), opens the editor. Delete a line to get that part's default back.\r\n"
            ZOOM_ROW
            "\r\n[keys]\r\n";
        check(strstr(text, kGeneral) != nullptr,
              "ini_version, [general] with the four keys, [layout] with its three comment lines (the EDIT button and the "
              "editor_open chord named) "
              "and the commented zoom_pct row");
        static const char* const kKeys =
            "\r\n[keys]\r\n"
            "; The keys and clicks the tab answers to, commented out at their default. Remove the ';'\r\n"
            "; to change one: Ctrl, Shift or Alt and a key, joined by '+' (Alt+PageDown, Shift+Wheel,\r\n"
            "; F5), or none. A letter, a digit or Space needs Ctrl or Alt. They are read from this\r\n"
            "; file only, for every game.\r\n"
            ";rows_up=PageUp\t\t\t; one window of rows up on a collection page\r\n"
            ";rows_down=PageDown\t\t; one window of rows down\r\n"
            ";button_prev=Ctrl+PageUp\t; the button before the one on screen (MAT included)\r\n"
            ";button_next=Ctrl+PageDown\t; the button after it\r\n"
            ";rows_wheel=Wheel\t\t; the wheel over the page: one row\r\n"
            ";button_wheel=Ctrl+Wheel\t; the wheel anywhere: one button\r\n"
            ";editor_open=Ctrl+RightClick\t; on the buttons or a pinned label (or a key): the layout editor\r\n"
            "\r\n[advanced]\r\n";
        check(strstr(text, kKeys) != nullptr,
              "[keys] between [layout] and [advanced]: its blurb and the seven rows commented at their defaults, on the blob's column");
        {
            const UtConfig fresh;
            check(!memcmp(&ut::g_cfg.keys, &fresh.keys, sizeof(UtKeys)),
                  "a fresh file's bindings are the defaults");
        }
        int missing = 0, dupes = 0, live = 0;
        for (int i = 0; i < ut::kUtCfgKeyCount; ++i) {
            const ut::UtCfgKey& k = ut::kUtCfgKeys[i];
            char row[96];
            _snprintf_s(row, sizeof(row), _TRUNCATE, "\r\n;%s=", k.name);
            int rows = 0;
            for (const char* p = strstr(text, row); p; p = strstr(p + 1, row)) ++rows;
            const int lines = countKeyLines(text, k.name);
            if (k.flags & ut::kUtCfgWhenSet) {
                if (lines || rows) ++dupes;   // absent until set: no line, no row
            } else if (k.flags & ut::kUtCfgHidden) {
                if (lines) ++live;
                if (rows == 0) ++missing;
                if (rows > 1) ++dupes;
            } else {
                if (lines == 0) ++missing;
                if (lines > 1 || rows) ++dupes;
            }
        }
        check(missing == 0 && dupes == 0 && live == 0,
              "every visible key on one live line, every hidden key on one commented row, no "
              "screen key");
        const char* adv = strstr(text, "\r\n[advanced]\r\n");
        check(adv && !strstr(adv + 4, "\r\n[") && !strstr(text, "[view:"),
              "[advanced] is the last block, and there is no [view:] block");
        size_t col = 0;
        const int rows = blobRows(text, &col);
        printf("    the blob: %d rows, descriptions at column %u\n", rows, (unsigned)col);
        check(rows == ut::kUtCfgKeyCount - 4 - whenSetRows() - layoutHiddenRows() - keysRows() + 1 && col > 0 && col % 8 == 0,
              "one row per hidden key plus padmat_, every description on one tab column");
        check(strstr(adv, ";padmat_438x627=102,25,320,44\t") != nullptr,
              "the hand-only padmat_ family is a commented row of the blob");
        check(!strstr(text, ":\\") && !strstr(text, "Users"), "no drive letter, no user name");
        free(text);
    }
    {
        const UtConfig d;
        int bad = 0;
        for (int i = 0; i < ut::kUtCfgKeyCount; ++i) {
            const ut::UtCfgKey& k = ut::kUtCfgKeys[i];
            if (k.type == ut::kUtCfgStr) {
                if (strcmp(strOf(ut::g_cfg, k), strOf(d, k)) != 0) ++bad;
            } else if (intOf(ut::g_cfg, k) != intOf(d, k)) {
                ++bad;
            }
        }
        check(bad == 0, "a fresh file parses back to the defaults, key for key (a commented row "
                        "is not a value)");
    }

    // ---- 3. a round trip, and a hidden key set by the user -----------------------------------
    printf("\n3. a round trip through the file; a hidden key goes LIVE in its row\n");
    {
        DWORD len = 0;
        char* text = readAll(ini, &len);
        std::string t(text);
        free(text);
        auto swap = [&](const char* a, const char* b) {
            const size_t at = t.find(a);
            if (at == std::string::npos) return false;
            t.replace(at, strlen(a), b);
            return true;
        };
        check(swap("tooltip_mark=1 ", "tooltip_mark=0 ") && swap("log_level=info", "log_level=warn") &&
                  swap(";journal_dir=", "journal_dir=some folder") &&
                  swap(";plate_ground=", "  plate_ground = 101010   "),
              "edit two visible keys and uncomment two rows by hand");
        writeText(ini, t.c_str());
        ut::configReload(ini);
        check(ut::g_cfg.tooltipMark == 0 && !strcmp(ut::g_cfg.logLevel, "warn"),
              "tooltip_mark=0 and log_level=warn came back");
        check(!strcmp(ut::g_cfg.journalDir, "some folder"), "journal_dir kept its space");
        check(!strcmp(ut::g_cfg.plateGround, "101010"), "plate_ground lost the blanks and the comment");
        char again[16384];
        const size_t n = ut::configRender(again, sizeof(again), ut::g_cfg);
        size_t col = 0;
        check(n > 0 && strstr(again, "\r\njournal_dir=some folder\t") &&
                  strstr(again, "\r\nplate_ground=101010\t") && !strstr(again, ";plate_ground=") &&
                  blobRows(again, &col) == ut::kUtCfgKeyCount - 3 - whenSetRows() - layoutHiddenRows() - keysRows(),
              "rendered: both rows live in their place, descriptions kept, columns aligned");
        const char* jd = strstr(again, "\r\njournal_dir=some folder\t");
        const char* jp = strstr(again, "\r\n;export_gds=1\t");
        const char* ex = strstr(again, "\r\n;layout_edit=0\t");
        check(ex && jd && jp && ex < jd && jd < jp, "a live row keeps its row position");
    }
    {
        // edit_button ([advanced], hidden, default 1): its commented row right after layout_edit's;
        // 0 by hand goes LIVE in that row and is the value in force
        DWORD len = 0;
        char* text = readAll(ini, &len);
        std::string t(text);
        free(text);
        const size_t le = t.find("\r\n;layout_edit=0\t"), eb = t.find("\r\n;edit_button=1\t"),
                     jd = t.find("\r\njournal_dir=");
        check(le != std::string::npos && eb != std::string::npos && jd != std::string::npos && le < eb &&
                  eb < jd && t.find("\r\n", le + 2) == eb &&
                  t.find("1 = an EDIT button on the pad opens the layout editor; 0 = no button "
                         "(the editor_open chord of [keys] still opens it: Ctrl+right-click unless you change it)",
                         eb) < t.find("\r\n", eb + 2),
              "edit_button: a commented row at 1, the row right after layout_edit, its description on it");
        check(ut::g_cfg.editButton == 1, "edit_button is 1 by default");
        std::string t0 = t;
        t0.replace(eb, 16, "\r\nedit_button=0\t");
        writeText(ini, t0.c_str());
        const long gen0 = ut::configGeneration();
        ut::configReload(ini);
        char again[16384];
        const size_t n = ut::configRender(again, sizeof(again), ut::g_cfg);
        check(ut::g_cfg.editButton == 0 && ut::configGeneration() != gen0 && n > 0 &&
                  strstr(again, "\r\nedit_button=0\t") && !strstr(again, ";edit_button="),
              "edit_button=0 by hand: live in its row, in force, and the generation moves (the pad re-lays out)");
        writeText(ini, t.c_str());
        ut::configReload(ini);
        check(ut::g_cfg.editButton == 1, "and back to 1 with the row commented again");
    }

    // ---- 4. a version bump KEEPS the user's values -------------------------------------------
    printf("\n4. a version bump keeps what you changed\n");
    {
        DWORD len = 0;
        char* text = readAll(ini, &len);
        check(patch(text, "ini_version=44", "ini_version=38"), "pretend the file is one version old");
        writeAll(ini, text, len);
        free(text);
        logClear();
        ut::configReload(ini);
        check(ut::g_cfg.tooltipMark == 0 && !strcmp(ut::g_cfg.logLevel, "warn") &&
                  !strcmp(ut::g_cfg.journalDir, "some folder") &&
                  !strcmp(ut::g_cfg.plateGround, "101010"),
              "every value SURVIVED the bump, the hidden ones too");
        check(ut::g_cfg.iniVersion == UT_INI_VERSION && logHas("migrated"),
              "the version is the current one, and the log says it migrated");
        char again[16384];
        const size_t n = ut::configRender(again, sizeof(again), ut::g_cfg);
        check(sameFile(ini, std::string(again, n).c_str()), "the file IS the table's rendering of them");
    }

    // ---- 4b. 39 -> 44: the visible collect_mi line appears, nothing else moves ---------------
    printf("\n4b. a 39 file (no collect_mi line) -> 44\n");
    {
        DWORD len = 0;
        char* text = readAll(ini, &len);
        const std::string before(text, len);
        free(text);
        const char* kLine = "collect_mi=1              ; 1 = monster infrequents are collected, one roll per item; 0 = they are not, and the ones you stored stay in the collection, hidden until it is 1 again (from the next start)\r\n";
        std::string old39 = before;
        const size_t at = old39.find(kLine);
        const size_t ver = old39.find("ini_version=44\r\n");
        check(at != std::string::npos && ver != std::string::npos,
              "the 44 file shows collect_mi=1 (the default) in [general], after enabled");
        if (at != std::string::npos && ver != std::string::npos) {
            old39.erase(at, strlen(kLine));
            old39.replace(ver, 14, "ini_version=39");
            writeText(ini, old39.c_str());
            logClear();
            ut::configReload(ini);
            check(logHas("migrated from ini_version=39 to 44"),
                  "the 39 file migrates (its commented rows count as defaulted, as on any bump)");
            check(ut::g_cfg.collectMi == 1 && ut::g_cfg.tooltipMark == 0 &&
                      !strcmp(ut::g_cfg.logLevel, "warn") && !strcmp(ut::g_cfg.plateGround, "101010"),
                  "collect_mi=1 (the default: on), every other value kept");
            check(sameFile(ini, before.c_str()), "the rewritten file is the 44 file byte for byte");
            writeText(ini, before.c_str());
            {
                DWORD l3 = 0;
                char* t3 = readAll(ini, &l3);
                check(patch(t3, "collect_mi=1 ", "collect_mi=0 "), "switch collect_mi off by hand");
                writeAll(ini, t3, l3);
                free(t3);
            }
            logClear();
            ut::configReload(ini);
            check(ut::g_cfg.collectMi == 0 && !logHas("migrated"),
                  "collect_mi=0 reads back, no rewrite (a 44 file keeps what it says)");
            writeText(ini, before.c_str());
            ut::configReload(ini);
        }
    }

    // ---- 4c. 40 -> 44: max_per_record and rescue leave [general] for the blob -----------------
    printf("\n4c. a 40 file (max_per_record and rescue in [general]) -> 44\n");
    {
        DWORD len = 0;
        char* text = readAll(ini, &len);
        const std::string before(text, len);   // the 44 file: tooltip_mark=0, log_level=warn, two live rows
        free(text);
        // a 40 file of the same values: the two keys in [general], not in the blob
        auto make40 = [&](const char* mpr, const char* rescue) {
            std::string t = before;
            auto cutRow = [&](const char* row) {
                const size_t a = t.find(row);
                if (a == std::string::npos) return false;
                const size_t e = t.find("\r\n", a + 2);
                t.erase(a, e - a);
                return true;
            };
            bool ok = cutRow("\r\n;max_per_record=1\t") && cutRow("\r\n;rescue=0\t");
            const size_t v = t.find("ini_version=44\r\n");
            ok = ok && v != std::string::npos;
            if (ok) t.replace(v, 14, "ini_version=40");
            const size_t en = t.find("\r\ncollect_mi=");
            const size_t lg = t.find("\r\nlog_level=");
            ok = ok && en != std::string::npos && lg != std::string::npos && en < lg;
            if (ok) {
                t.insert(lg + 2, std::string("rescue=") + rescue +
                                     "                  ; 1 with the caravan open = hand every stored item back (sets itself to 0)\r\n");
                t.insert(en + 2, std::string("max_per_record=") + mpr +
                                     "          ; how many copies of one item the collection keeps (0 = no limit)\r\n");
            }
            return ok ? t : std::string();
        };
        const std::string atDefault = make40("1", "0");
        check(!atDefault.empty(), "a 40 file with max_per_record=1 and rescue=0 in [general]");
        writeText(ini, atDefault.c_str());
        logClear();
        ut::configReload(ini);
        check(logHas("migrated from ini_version=40 to 44") && ut::g_cfg.maxPerRecord == 1 &&
                  ut::g_cfg.rescue == 0 && ut::g_cfg.tooltipMark == 0 && !strcmp(ut::g_cfg.logLevel, "warn"),
              "it migrates, every value kept");
        check(sameFile(ini, before.c_str()),
              "at their defaults both go into the blob as commented rows: the 44 file byte for byte");
        const std::string set = make40("2", "1");
        writeText(ini, set.c_str());
        logClear();
        ut::configReload(ini);
        check(logHas("migrated from ini_version=40 to 44") && ut::g_cfg.maxPerRecord == 2 && ut::g_cfg.rescue == 1,
              "max_per_record=2 and rescue=1 in a 40 file: migrated, both kept");
        {
            DWORD l2 = 0;
            char* t2 = readAll(ini, &l2);
            const std::string now(t2, l2);
            free(t2);
            const size_t gen = now.find("\r\n[general]\r\n");
            const size_t lay = now.find("\r\n[layout]\r\n");
            const size_t adv = now.find("\r\n[advanced]\r\n");
            const size_t mpr = now.find("\r\nmax_per_record=2\t");
            const size_t res = now.find("\r\nrescue=1\t");
            check(gen != std::string::npos && lay != std::string::npos && adv != std::string::npos &&
                      mpr != std::string::npos && res != std::string::npos && mpr > adv && res > adv &&
                      now.find("max_per_record=", gen) > lay && now.find("rescue=", gen) > lay &&
                      now.find(";max_per_record=") == std::string::npos && now.find(";rescue=") == std::string::npos,
                  "set, both are LIVE rows of the blob (not in [general]), descriptions kept");
            check(now.find("hand every stored item back, sets itself to 0 (untested)") != std::string::npos &&
                      now.find("0 = no limit (untested)") != std::string::npos,
                  "the rows say (untested)");
        }
        check(ut::configPersistInt("rescue", 0), "the rescue run writes rescue=0 back (a hidden key)");
        ut::configReload(ini);
        {
            DWORD l3 = 0;
            char* t3 = readAll(ini, &l3);
            const std::string now(t3, l3);
            free(t3);
            check(ut::g_cfg.rescue == 0 && now.find("\r\n;rescue=0\t") != std::string::npos &&
                      now.find("\r\nrescue=") == std::string::npos && now.find("\r\nmax_per_record=2\t") != std::string::npos,
                  "rescue is a commented row again, max_per_record=2 stays live");
        }
        writeText(ini, before.c_str());
        ut::configReload(ini);
    }

    // ---- 5. a bump from a file that predates most of the keys --------------------------------
    printf("\n5. a bump from an old, short file\n");
    {
        writeText(ini, "ini_version=12\r\nmax_per_record=9\r\ndrop_trace=1\r\ntab_col_x=417\r\n");
        logClear();
        ut::configReload(ini);
        check(ut::g_cfg.maxPerRecord == 9 && ut::g_cfg.exportGds == 1,
              "the one setting it had was kept, the others defaulted");
        check(logHas("2 setting(s) this build no longer has were dropped: drop_trace, tab_col_x"),
              "the log names both keys this build does not have (INFO)");
        check(fileSize(ini) != 0 && !hasText(ini, "drop_trace") && hasText(ini, "max_per_record=9"),
              "they are gone from the rewritten file");
    }

    // ---- 5b. the migration THIS user will do (ini_version 38 -> 44) --------------------------
    printf("\n5b. a real 38 file (log_level=debug, six layout lines) -> 44\n");
    {
        DeleteFileW(state);
        writeText(ini, kUserIni38);
        logClear();
        ut::configReload(ini);
        UtConfig want;
        want.iniVersion = UT_INI_VERSION;
        strcpy_s(want.logLevel, "debug");
        userLayout(&want);
        char expect[16384];
        const size_t n = ut::configRender(expect, sizeof(expect), want);
        check(n > 0 && sameFile(ini, std::string(expect, n).c_str()),
              "the file is the fresh template with log_level=debug and the six lines");
        DWORD len = 0;
        char* text = readAll(ini, &len);
        printf("%s", text);
        check(strstr(text, "\r\n[general]\r\n") && strstr(text, "\r\nlog_level=debug           ; how much"),
              "log_level=debug is in [general], where it was");
        check(strstr(text,
                     "\r\n[layout]\r\n"
                     "; Where the item grid, the buttons and the label sit. The in-game editor writes these\r\n"
                     "; lines: the EDIT button on the pad, or the editor_open chord of [keys] (Ctrl+right-click\r\n"
                     "; unless you change it), opens the editor. Delete a line to get that part's default back.\r\n"
                     ZOOM_ROW
                     "grid_438x627=99,73,320,552\r\npad_438x627=98,27,320,44\r\nlabel_438x627=103,4,333,23\r\n"
                     "grid_974x659=46,218,928,374\r\npad_974x659=0,1,780,153\r\nlabel_974x659=428,165,317,23\r\n"
                     "\r\n[keys]\r\n") != nullptr,
              "the six layout lines under [layout], in their order");
        int liveRows = 0;
        for (const char* p = strstr(text, "\r\n[advanced]\r\n") + 14; p && *p;) {
            if (*p != ';' && *p != '\r') ++liveRows;
            const char* e = strstr(p, "\r\n");
            p = e ? e + 2 : nullptr;
        }
        check(liveRows == 0, "NO live row in [advanced]: everything else was at its default");
        check(countKeyLines(text, "uniq_group") == 0 && !strstr(text, "[view:"),
              "no state key, no [view:] block");
        free(text);
        check(logHas("21 setting(s) this build no longer has were dropped: soulbound_collect, "
                     "collect_quick_pass, group_buttons"),
              "INFO: the 21 dropped/merged keys, named");
        check(!logHas("is not read from uniquetab.ini any more"), "no merge WARN: all were defaults");
        check(logHas("19 setting(s) kept, 10 new one(s) defaulted"),
              "19 rows kept; collect_mi (new in 40), edit_button (43), zoom_pct and the seven [keys] rows (44) defaulted");
        check(exists(state) && sameFile(state,
                                        "; uniquetab.state - written by the mod: where the tab was left in this game.\r\n"
                                        "; Not a setting: delete it to start on the materials page.\r\n"
                                        "uniq_group=28\r\nuniq_row=0\r\nowned_only=0\r\n"),
              "uniq_group=28 uniq_row=0 owned_only=0 seeded uniquetab.state (two-line header)");
        check(logHas("moved to uniquetab.state (group=28 row=0 owned_only=0)"), "with one INFO line");
        check(ut::g_cfg.uniqGroup == 28 && ut::g_cfg.uniqRow == 0 && ut::g_cfg.ownedOnly == 0,
              "and g_cfg reads them from there");
        check(!strcmp(ut::g_cfg.logLevel, "debug") &&
                  !strcmp(ut::configFamilyValue(ut::g_cfg, "pad_974x659"), "0,1,780,153"),
              "the values in force are the user's");

        // The merged keys with a value: one WARN each naming the new home.
        writeText(ini, "ini_version=38\r\nlog_flush_each_line=1\r\ntooltip_text_no=Nope\r\n"
                       "tooltip_text_yes=\r\npad_y=30\r\n");
        logClear();
        ut::configReload(ini);
        check(logHas("log_flush_each_line=1 is not read from uniquetab.ini any more - its value "
                     "belongs in log_level=trace"),
              "log_flush_each_line=1 -> WARN naming log_level=trace");
        check(logHas("tooltip_text_no=Nope is not read from uniquetab.ini any more - its value "
                     "belongs in tooltip.no under [tab] of uniquetab-buttons.ini"),
              "tooltip_text_no=Nope -> WARN naming tooltip.no under [tab] of uniquetab-buttons.ini");
        check(!logHas("tooltip_text_yes= is not read"), "an empty tooltip_text_yes says nothing");
        check(logHas("dropped: log_flush_each_line, tooltip_text_no, tooltip_text_yes, pad_y"),
              "and all four are dropped (pad_y with an INFO only)");
        check(ut::g_cfg.logFlushEachLine == 0 && ut::g_cfg.padY == 25 && ut::g_cfg.tooltipTextNo[0] == 0,
              "none of them is applied");
        writeText(ini, INI_CUR "log_level=trace\r\n");
        ut::configReload(ini);
        check(ut::g_cfg.logFlushEachLine == 1, "log_level=trace writes every line at once");

        // A dropped key typed into a current file.
        writeText(ini, INI_CUR "pad_y=30\r\nuniq_group=4\r\n");
        logClear();
        ut::configReload(ini);
        check(logHas("1 line(s) name a setting this build does not have and are ignored: pad_y"),
              "a dropped key in a 39 file -> the existing WARN");
        check(logHas("uniq_group/uniq_row/owned_only are not settings") && ut::g_cfg.uniqGroup == 28,
              "a state key in a 39 file is ignored with a WARN; the state file wins");
    }

    // ---- 6. bad values -----------------------------------------------------------------------
    printf("\n6. a value outside its range, and a word that is not a choice\n");
    {
        writeText(ini, INI_CUR "plate_label_size=99\r\nlog_level=lou1\r\nui_scale_pct=-9\r\n");
        logClear();
        ut::configReload(ini);
        check(ut::g_cfg.plateLabelSize == 32 && logHas("plate_label_size=99 is outside 8..32"),
              "plate_label_size was clamped to 32, and the log says so");
        check(ut::g_cfg.uiScalePct == 0, "ui_scale_pct was clamped to the bottom");
        check(!strcmp(ut::g_cfg.logLevel, "info") && logHas("log_level=lou1 is not one of the words"),
              "the unknown log_level fell back to the default, with a WARN");
        check(hasText(ini, "plate_label_size=99"), "the file still says what the user typed");
    }

    // ---- 6b. a misspelled key at the current version -----------------------------------------
    printf("\n6b. a line that names nothing\n");
    {
        writeText(ini, INI_CUR "max_per_recrod=3\r\n");
        logClear();
        ut::configReload(ini);
        check(logHas("name a setting this build does not have and are ignored: max_per_recrod"),
              "the log names the line instead of swallowing it");
        check(ut::g_cfg.maxPerRecord == 1 && hasText(ini, "max_per_recrod=3"),
              "the real setting kept its default, the file is left alone");
    }

    // ---- 7. the one-shot write-back ----------------------------------------------------------
    printf("\n7. a one-shot writes itself back; a hidden one goes back to its commented row\n");
    {
        DeleteFileW(ini);
        ut::configReload(ini);
        DWORD before = 0;
        char* pristine = readAll(ini, &before);
        std::string t(pristine);
        t.replace(t.find(";rescue=0"), 9, "rescue=1");
        t.replace(t.find(";journal_prune=0"), 16, "journal_prune=1");
        writeText(ini, t.c_str());
        ut::configReload(ini);
        check(ut::g_cfg.rescue == 1 && ut::g_cfg.journalPrune == 1, "rescue=1 and an uncommented journal_prune=1");
        check(ut::configPersistInt("rescue", 0), "the run writes rescue=0 back");
        check(ut::configPersistInt("journal_prune", 0), "and journal_prune=0");
        check(sameFile(ini, pristine), "the file is the template again, byte for byte: the row is "
                                       "commented at its own place and column");
        ut::configReload(ini);
        check(ut::g_cfg.rescue == 0 && ut::g_cfg.journalPrune == 0, "both read back as 0");
        const char* ke[1] = {"layout_edit"};
        const char* v1[1] = {"1"};
        check(ut::configPersistStr(ke, v1, 1), "layout_edit=1 written into the commented row");
        UtConfig want;
        want.iniVersion = UT_INI_VERSION;
        want.layoutEdit = 1;
        char expect[16384];
        const size_t n = ut::configRender(expect, sizeof(expect), want);
        check(sameFile(ini, std::string(expect, n).c_str()), "the row goes live in place, aligned");
        ut::configReload(ini);
        check(ut::g_cfg.layoutEdit == 1, "layout_edit=1 reads");
        const char* v0[1] = {"0"};
        check(ut::configPersistStr(ke, v0, 1) && sameFile(ini, pristine),
              "the editor writes layout_edit=0 back: the commented row again");
        free(pristine);
        ut::configReload(ini);
    }

    // ---- 8. the RESCUE-NOW trigger -----------------------------------------------------------
    printf("\n8. the RESCUE-NOW trigger file\n");
    {
        writeAll(trigger, "go", 2);
        ut::configReload(ini);
        check(ut::g_cfg.rescue == 1, "RESCUE-NOW arms rescue=1 without touching the file");
        check(GetFileAttributesW(trigger) == INVALID_FILE_ATTRIBUTES, "the trigger was consumed");
        ut::configReload(ini);
        check(ut::g_cfg.rescue == 1, "it stays armed while the file still says rescue=0 (latch)");
        ut::configPersistInt("rescue", 0);  // what rescueTick() does when the run is finished
        ut::configReload(ini);
        check(ut::g_cfg.rescue == 0, "writing rescue=0 back disarms the latch");

        writeAll(triggerTxt, "go", 2);
        ut::configReload(ini);
        check(ut::g_cfg.rescue == 1, "RESCUE-NOW.txt (Windows hides extensions) arms it too");
        check(GetFileAttributesW(triggerTxt) == INVALID_FILE_ATTRIBUTES, ".txt was consumed");
        ut::configPersistInt("rescue", 0);
        ut::configReload(ini);
    }

    // ---- 9. the master switch ----------------------------------------------------------------
    printf("\n9. enabled=0 collapses the feature switches\n");
    {
        writeText(ini, INI_CUR "enabled=0\r\nmax_per_record=9\r\nlayout_outline=1\r\nlayout_edit=1\r\n");
        logClear();
        ut::configReload(ini);
        check(ut::g_cfg.enabled == 0, "enabled=0 is read");
        check(ut::g_cfg.journal == 0, "the private table may not take a row: no new deposits");
        check(ut::g_cfg.livePages == 0 && ut::g_cfg.groupButtons == 0 && ut::g_cfg.plateLabel == 0,
              "the tab, the buttons and the label are off");
        check(ut::g_cfg.tooltipMark == 0 && ut::g_cfg.comparePopup == 0, "so is the tooltip line");
        check(ut::g_cfg.layoutOutline == 0 && ut::g_cfg.layoutEdit == 0,
              "and the outline and the editor key");
        check(ut::g_cfg.maxPerRecord == 9 && ut::g_cfg.exportGds == 1,
              "nothing else in the file was touched");
        check(logHas("enabled=0"), "the log says the mod is off and why nothing is lost");
        writeText(ini, INI_CUR "enabled=1\r\n");
        ut::configReload(ini);
        check(ut::g_cfg.enabled == 1 && ut::g_cfg.journal == 1 && ut::g_cfg.livePages == 1,
              "enabled=1 gives every one of them back");
    }

    // ---- 10. the path rules ------------------------------------------------------------------
    // Pure string arithmetic, no disk: every case is a path the loader can really produce.
    printf("\n10. the game folder and the mod folder, for both .asi placements\n");
    {
        char game[MAX_PATH] = {0};
        wchar_t mod[MAX_PATH] = {0};

        // The .asi beside the exe: <game>\x64\uniquetab.asi.
        check(ut::utGameDirFromExeW(L"\\games\\Grim Dawn\\x64\\Grim Dawn.exe", game,
                                    sizeof(game)) &&
                  strcmp(game, "\\games\\Grim Dawn") == 0,
              "the game folder is the parent of the exe's own folder");
        check(ut::utModDirBesideW(L"\\games\\Grim Dawn\\x64\\uniquetab.asi", mod, MAX_PATH) &&
                  lstrcmpW(mod, L"\\games\\Grim Dawn\\x64\\uniquetab") == 0,
              "beside the exe: the mod folder is x64\\uniquetab");

        // The plugin placement: <game>\x64\scripts\uniquetab.asi. The mod folder moves with the
        // .asi; the game folder is read from the exe, so it does NOT.
        check(ut::utModDirBesideW(L"\\games\\Grim Dawn\\x64\\scripts\\uniquetab.asi", mod,
                                  MAX_PATH) &&
                  lstrcmpW(mod, L"\\games\\Grim Dawn\\x64\\scripts\\uniquetab") == 0,
              "in scripts\\: the mod folder is x64\\scripts\\uniquetab");
        check(ut::utGameDirFromExeW(L"\\games\\Grim Dawn\\x64\\Grim Dawn.exe", game,
                                    sizeof(game)) &&
                  strcmp(game, "\\games\\Grim Dawn") == 0,
              "the game folder is the same for either placement");

        // The live resolver reads the host exe, never this module: the answer is the parent of
        // the harness exe's own folder, whatever handle a caller might have passed in before.
        {
            wchar_t self[MAX_PATH] = {0};
            char expect[MAX_PATH] = {0};
            GetModuleFileNameW(nullptr, self, MAX_PATH);
            check(ut::utGameDirFromExeW(self, expect, sizeof(expect)) &&
                      ut::utGameDirA(game, sizeof(game)) && strcmp(game, expect) == 0,
                  "utGameDirA resolves the running exe's own grandparent folder");
        }

        // A path with nothing left to strip is refused rather than answered with a root.
        check(!ut::utGameDirFromExeW(L"Grim Dawn.exe", game, sizeof(game)),
              "a bare file name has no game folder");
        check(!ut::utModDirBesideW(nullptr, mod, MAX_PATH) &&
                  !ut::utGameDirFromExeW(nullptr, game, sizeof(game)),
              "a null path is refused by both rules");
    }

    // ---- 11. the keyed [layout] lines --------------------------------------------------------
    printf("\n11. the keyed [layout] lines\n");
    {
        DeleteFileW(ini);
        ut::configReload(ini);   // the template
        ut::configReload(ini);
        check(ut::g_cfg.layGrid.count == 0 && ut::g_cfg.layPadMat.count == 0,
              "a fresh file has no layout line, and a commented row is not a value");
        UtConfig zero;
        check(memcmp(&zero.layGrid, &ut::g_cfg.layGrid, sizeof(zero.layGrid)) == 0,
              "an empty family is all zero bytes (the change test memcmps UtConfig)");
        char edited[1024];
        _snprintf_s(edited, sizeof(edited), _TRUNCATE,
                    "ini_version=%d\r\n"
                    "[layout]\r\n"
                    "grid_438x627 = 105,120,333,460   ; my grid\r\n"
                    "pad_974x659=10,20,400,60\r\n"
                    "label_438x627=1,2,3\r\n"
                    "grid_438x62a=1,2,3,4\r\n"
                    "[advanced]\r\n"
                    "padmat_346x625=  5 , 6 , 7 , 8  \r\n"
                    "label_974x659=12,8,300,0\r\n"
                    "max_per_record=3\r\n",
                    UT_INI_VERSION);
        writeText(ini, edited);
        logClear();
        const long genBefore = ut::configGeneration();
        ut::configReload(ini);
        check(ut::configGeneration() > genBefore, "a reload that changes g_cfg bumps configGeneration");
        const char* g = ut::configFamilyValue(ut::g_cfg, "grid_438x627");
        check(g && !strcmp(g, "105,120,333,460"), "grid_438x627 is read, blanks and comment gone");
        const char* p = ut::configFamilyValue(ut::g_cfg, "pad_974x659");
        check(p && !strcmp(p, "10,20,400,60"), "pad_974x659 is read: one key per cover size");
        const char* m = ut::configFamilyValue(ut::g_cfg, "padmat_346x625");
        check(m && !strcmp(m, "5 , 6 , 7 , 8"), "padmat_346x625 keeps its inner blanks");
        check(ut::configFamilyValue(ut::g_cfg, "label_438x627") == nullptr &&
                  logHas("label_438x627=1,2,3 is not x,y,w,h"),
              "three numbers are refused, and the log says so");
        check(ut::configFamilyValue(ut::g_cfg, "label_974x659") == nullptr &&
                  logHas("label_974x659=12,8,300,0 is not x,y,w,h"),
              "a zero height is refused, whatever [section] the line is in");
        check(logHas("ignored: grid_438x62a"), "a bad suffix is a key this build does not have");
        check(ut::g_cfg.maxPerRecord == 3, "the fixed keys around them still read");
        {
            const long gen = ut::configGeneration();
            ut::configReload(ini);
            check(ut::configGeneration() == gen, "an unchanged reload keeps configGeneration");
        }
        DWORD len = 0;
        char* text = readAll(ini, &len);
        check(patch(text, "ini_version=44", "ini_version=38"), "pretend the file is one version old");
        writeAll(ini, text, len);
        free(text);
        logClear();
        ut::configReload(ini);
        g = ut::configFamilyValue(ut::g_cfg, "grid_438x627");
        p = ut::configFamilyValue(ut::g_cfg, "pad_974x659");
        m = ut::configFamilyValue(ut::g_cfg, "padmat_346x625");
        check(logHas("migrated") && g && !strcmp(g, "105,120,333,460") && p &&
                  !strcmp(p, "10,20,400,60") && m && !strcmp(m, "5 , 6 , 7 , 8"),
              "all three lines SURVIVED the bump");
        text = readAll(ini, &len);
        const char* layoutAt = strstr(text, "\r\n[layout]\r\n");
        const char* advAt = strstr(text, "\r\n[advanced]\r\n");
        const char* gridAt = strstr(text, "\r\ngrid_438x627=105,120,333,460\r\n");
        const char* matAt = strstr(text, "\r\npadmat_346x625=5 , 6 , 7 , 8\t");
        check(layoutAt && advAt && gridAt && layoutAt < gridAt && gridAt < advAt &&
                  countKeyLines(text, "grid_438x627") == 1,
              "the rewrite puts the editor's lines under [layout], once each");
        check(matAt && matAt > advAt && !strstr(text, ";padmat_438x627"),
              "a padmat_ line is live in the blob, in place of its commented row");
        check(strstr(text, "grid_438x62a") == nullptr && countKeyLines(text, "label_438x627") == 0,
              "the bad suffix and the refused value are not carried over");
        {
            char again[32768];
            const size_t n = ut::configRender(again, sizeof(again), ut::g_cfg);
            check(n == (size_t)len && memcmp(again, text, n) == 0,
                  "the rewritten file IS the table plus the stored lines");
        }
        free(text);
        char many[4096];
        int at = _snprintf_s(many, sizeof(many), _TRUNCATE, "ini_version=%d\r\n", UT_INI_VERSION);
        at += _snprintf_s(many + at, sizeof(many) - at, _TRUNCATE, "grid_438x627=1,1,1,1\r\n");
        at += _snprintf_s(many + at, sizeof(many) - at, _TRUNCATE, "grid_438x627=2,2,2,2\r\n");
        for (int i = 0; i < kUtCfgFamilySlots; ++i)
            at += _snprintf_s(many + at, sizeof(many) - at, _TRUNCATE, "pad_%dx100=1,2,3,4\r\n",
                              100 + i);
        writeText(ini, many);
        ut::configReload(ini);
        g = ut::configFamilyValue(ut::g_cfg, "grid_438x627");
        check(g && !strcmp(g, "2,2,2,2") && ut::g_cfg.layGrid.count == 1,
              "a suffix given twice keeps the later value, in one slot");
        check(ut::g_cfg.layPad.count == kUtCfgFamilySlots, "16 lines fill the pad_ family");
        at += _snprintf_s(many + at, sizeof(many) - at, _TRUNCATE, "pad_999x100=1,2,3,4\r\n");
        writeText(ini, many);
        logClear();
        ut::configReload(ini);
        check(ut::configFamilyValue(ut::g_cfg, "pad_999x100") == nullptr &&
                  logHas("pad_999x100 is ignored - pad_ holds at most 16 lines"),
              "a 17th line is refused with a WARN");
    }

    // ---- 12. configPersistStr ------------------------------------------------------------------
    printf("\n12. several text keys in one atomic rewrite\n");
    {
        DeleteFileW(ini);
        ut::configReload(ini);   // a fresh template
        ut::configReload(ini);
        DWORD before = 0;
        char* was = readAll(ini, &before);
        const char* k2[2] = {"grid_438x627", "pad_438x627"};
        const char* v2[2] = {"105,120,333,460", "102,30,320,80"};
        check(ut::configPersistStr(k2, v2, 2), "two family keys written at once");
        DWORD after = 0;
        char* text = readAll(ini, &after);
        const char* tail =
            "default back.\r\n"
            ZOOM_ROW
            "grid_438x627=105,120,333,460\r\n"
            "pad_438x627=102,30,320,80\r\n"
            "\r\n[keys]\r\n";
        check(strstr(text, tail) != nullptr,
              "both are inserted at the end of [layout], before its blank line");
        check(after == before + (DWORD)strlen("grid_438x627=105,120,333,460\r\n") +
                           (DWORD)strlen("pad_438x627=102,30,320,80\r\n"),
              "and nothing else in the file changed size");
        {
            const char* ins = strstr(text, "grid_438x627=");
            const size_t head = (size_t)(ins - text);
            check(head <= before && memcmp(was, text, head) == 0, "every line above is untouched");
        }
        free(was);
        free(text);
        ut::configReload(ini);
        const char* g = ut::configFamilyValue(ut::g_cfg, "grid_438x627");
        const char* p = ut::configFamilyValue(ut::g_cfg, "pad_438x627");
        check(g && !strcmp(g, "105,120,333,460") && p && !strcmp(p, "102,30,320,80"),
              "and both read back");
        const char* k3[3] = {"grid_438x627", "pad_438x627", "uniq_group"};
        const char* v3[3] = {"1,2,3,4", nullptr, "5"};
        check(ut::configPersistStr(k3, v3, 3), "replace one, delete one, and a state key with them");
        ut::configReload(ini);
        g = ut::configFamilyValue(ut::g_cfg, "grid_438x627");
        check(g && !strcmp(g, "1,2,3,4"), "the grid line was replaced");
        check(ut::configFamilyValue(ut::g_cfg, "pad_438x627") == nullptr, "the pad line is gone");
        check(ut::g_cfg.uniqGroup == 5 && hasText(state, "uniq_group=5\r\n") &&
                  !hasText(ini, "uniq_group"),
              "uniq_group=5 went to uniquetab.state, not into the settings");
        const char* kBad[1] = {"label_438x627"};
        const char* vBad[1] = {"a\r\nenabled=0"};
        check(!ut::configPersistStr(kBad, vBad, 1), "a value with a line break is refused");
        writeText(ini, INI_CUR "enabled=1\r\n");
        ut::configReload(ini);
        const char* k1[1] = {"label_438x627"};
        const char* vl[1] = {"105,8,317,16"};
        check(ut::configPersistStr(k1, vl, 1) &&
                  sameFile(ini, INI_CUR "enabled=1\r\nlabel_438x627=105,8,317,16\r\n"),
              "a key whose section the file does not have is appended as the last line");
    }

    // ---- 13. the same mechanism for another file ----------------------------------------------
    // A strings or groups file keeps its own struct, its own family table and its own sections;
    // only the engine is shared.
    printf("\n13. a family table and a persist of another file\n");
    {
        struct OtherCfg {
            UtCfgFamilyStore caps;
        };
        struct Checks {
            static bool idOk(const char* s) {
                if (!*s) return false;
                for (; *s; ++s)
                    if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_' ||
                          *s == '.' || *s == '-'))
                        return false;
                return true;
            }
            static const char* textOk(const char* v) { return *v ? nullptr : "empty"; }
            static const char* sectionOf(const char* key) {
                return strncmp(key, "cap_", 4) == 0 ? "text" : nullptr;
            }
        };
        const ut::UtCfgFamily table[1] = {{"text", "cap_", offsetof(OtherCfg, caps), Checks::idOk,
                                       Checks::textOk, "helms=Helms", "a caption by category id"}};
        OtherCfg other = {};
        const char* suffix = nullptr;
        const ut::UtCfgFamily* f = ut::configFamilyIn(table, 1, "cap_reignofterror.charms", &suffix);
        check(f == &table[0] && suffix && !strcmp(suffix, "reignofterror.charms"),
              "a table of the caller's own finds a namespaced id");
        check(ut::configFamilyIn(table, 1, "cap_Bad Id", nullptr) == nullptr,
              "and refuses a suffix its own check refuses");
        check(ut::configFamilySet(ut::configFamilyStore(*f, &other), suffix, "Charms") &&
                  !strcmp(ut::configFamilyGet(other.caps, "reignofterror.charms"), "Charms"),
              "its store lives in the caller's own struct");

        wchar_t path2[MAX_PATH];
        _snwprintf_s(path2, _TRUNCATE, L"%s\\other.ini", dir);
        writeText(path2, "[text]\r\ncap_helms=Helms\r\n\r\n[text.ru]\r\ncap_helms=X\r\n");
        const char* k[2] = {"cap_rings", "cap_helms"};
        const char* v[2] = {"Rings", nullptr};
        check(ut::configPersistFile(path2, k, v, 2, Checks::sectionOf), "persist two keys of that file");
        DWORD len = 0;
        char* text = readAll(path2, &len);
        check(!strcmp(text, "[text]\r\ncap_rings=Rings\r\n\r\n[text.ru]\r\n"),
              "both cap_helms lines deleted, cap_rings inserted at the end of [text]");
        free(text);
        DeleteFileW(path2);
    }

    // ---- 14. a custom game's own file -------------------------------------------------------
    printf("\n14. mods\\<name>\\uniquetab.ini: a complete file, seeded once, nothing inherited, migrated\n");
    std::wstring rotIni, rotState, dgdIni, dgdState, bmState, vanIni, vanState;
    ut::configViewFiles(ini, "custom-reignofterror", &rotIni, &rotState);
    ut::configViewFiles(ini, "custom-deathgrimdawn", &dgdIni, &dgdState);
    {
        std::wstring bmIni;
        ut::configViewFiles(ini, "basemods+custom-reignofterror", &bmIni, &bmState);
        ut::configViewFiles(ini, "vanilla", &vanIni, &vanState);
        wchar_t want[MAX_PATH];
        _snwprintf_s(want, _TRUNCATE, L"%s\\mods\\reignofterror\\uniquetab.ini", dir);
        check(rotIni == want && bmIni == want,
              "custom-x and basemods+custom-x share mods\\x\\uniquetab.ini");
        _snwprintf_s(want, _TRUNCATE, L"%s\\mods\\reignofterror\\basemods\\uniquetab.state", dir);
        check(bmState == want && rotState != bmState,
              "but each view folder has its own uniquetab.state");
        check(vanIni.empty() && vanState == state && ut::configCustomKey("basemods").empty() &&
                  ut::configCustomKey("custom-Bad Name").empty(),
              "vanilla/basemods: the main file and the main state file; a bad key has no file");
    }
    {
        // THE SEED: the first entry writes the whole template with the main file's values.
        writeText(ini, INI_CUR "[general]\r\ncollect_mi=0\r\nlog_level=debug\r\n"
                               "[layout]\r\ngrid_438x627=1,2,3,4\r\npad_screen=tr,8,8,320,44\r\n"
                               "[advanced]\r\nplate_label_size=20\r\nexport_csv=1\r\nrescue=1\r\n"
                               "journal_dir=some folder\r\n");
        ut::configSetView("vanilla");
        ut::configReload(ini);
        check(ut::g_cfg.plateLabelSize == 20 && ut::g_cfg.collectMi == 0 && !exists(rotIni.c_str()),
              "vanilla: the main file, no game file");
        logClear();
        ut::configSetView("custom-reignofterror");
        check(exists(rotIni.c_str()), "the first entry writes mods\\reignofterror\\uniquetab.ini");
        check(logHas("the custom game reignofterror has its own settings now") &&
                  logHas("a complete file with the main uniquetab.ini's values (enabled=1 collect_mi=0 "
                         "log_level=debug, 1 [layout] line(s))"),
              "with one INFO line");
        UtConfig src;   // the main file's values, the one-shots at 0, no journal_dir
        src.collectMi = 0;
        _snprintf_s(src.logLevel, sizeof(src.logLevel), _TRUNCATE, "debug");
        ut::configFamilySet(&src.layGrid, "438x627", "1,2,3,4");
        _snprintf_s(src.padScreen, sizeof(src.padScreen), _TRUNCATE, "tr,8,8,320,44");
        src.plateLabelSize = 20;
        src.exportCsv = 1;
        static char expect[16384], asMain[16384];
        const size_t n = ut::configRenderView(expect, sizeof(expect), "reignofterror", src);
        check(n > 0 && sameFile(rotIni.c_str(), std::string(expect, n).c_str()),
              "the seed: every value of the main file (rescue=1 and journal_dir not carried)");
        DWORD len = 0;
        char* text = readAll(rotIni.c_str(), &len);
        printf("%s", text);
        free(text);
        // ONE TEMPLATE: the game's file is the main file's rendering of the same values, but for
        // the header and the journal_dir row.
        {
            const size_t m = ut::configRender(asMain, sizeof(asMain), src);
            std::string g(expect, n), mm(asMain, m);
            auto body = [](std::string t, std::string* jd) {
                t = t.substr(t.find("\r\nini_version="));
                const size_t ka = t.find("\r\n[keys]\r\n");   // the main file's only
                if (ka != std::string::npos) t.erase(ka, t.find("\r\n[advanced]\r\n") - ka);
                const size_t a = t.find("\r\n;journal_dir=");
                const size_t b = a == std::string::npos ? a : t.find("\r\n", a + 2);
                if (b != std::string::npos) {
                    *jd = t.substr(a, b - a);
                    t.erase(a, b - a);
                }
                return t;
            };
            std::string jdGame, jdMain;
            const std::string bg = body(g, &jdGame), bm = body(mm, &jdMain);
            check(m > 0 && bg == bm && g.size() > 0,
                  "the game's file = the main template byte for byte (same values), but the header and [keys]");
            check(g.find("\r\n[keys]\r\n") == std::string::npos && g.find("rows_up") == std::string::npos &&
                      mm.find("\r\n[keys]\r\n") != std::string::npos &&
                      g.find("main uniquetab.ini but journal_dir and [keys]") != std::string::npos,
                  "a game's file never renders [keys]; its header says journal_dir and [keys] come from the main file");
            check(jdGame.find(";journal_dir=\t") != std::string::npos &&
                      jdGame.find("; read at start-up from the main uniquetab.ini only - set it there") != std::string::npos &&
                      jdMain.find("where the collection file is kept") != std::string::npos,
                  "journal_dir is a commented line there, saying where it is set");
            check(g.find("your settings for the custom game \"reignofterror\"") != std::string::npos &&
                      g.find("\r\n[general]\r\n") != std::string::npos && g.find("\r\n[layout]\r\n") != std::string::npos &&
                      g.find("\r\n[advanced]\r\n") != std::string::npos && g.find("[game]") == std::string::npos,
                  "its header names the game; [general], [layout], [advanced] - no [game] block");
        }
        check(ut::g_cfg.plateLabelSize == 20 && ut::g_cfg.collectMi == 0 && ut::g_cfg.exportCsv == 1 &&
                  !strcmp(ut::g_cfg.logLevel, "debug") && !strcmp(ut::g_mappedLevel, "debug") &&
                  !strcmp(ut::configFamilyValue(ut::g_cfg, "grid_438x627"), "1,2,3,4") &&
                  !strcmp(ut::g_cfg.padScreen, "tr,8,8,320,44") && ut::g_cfg.rescue == 0 &&
                  !strcmp(ut::g_cfg.journalDir, "some folder"),
              "in the game: its own file's values, journal_dir the main file's, the logger mapped");
        check(ut::configViewIntFor("custom-deathgrimdawn", "collect_mi") == 0 &&
                  ut::configViewIntFor("vanilla", "collect_mi") == 0 &&
                  ut::configViewIntFor("custom-reignofterror", "plate_label_size") == 20,
              "configViewIntFor: a game with no file yet = the main file's value (its seed)");

        // NOTHING IS INHERITED: a current file that lacks a line has the compiled default.
        writeText(rotIni.c_str(), INI_CUR "[general]\r\nenabled=1\r\n");
        logClear();
        ut::configReload(ini);
        check(logHas("the custom game reignofterror's own settings were re-read"), "its file is polled");
        check(ut::g_cfg.plateLabelSize == 13 && ut::g_cfg.collectMi == 1 && ut::g_cfg.exportCsv == 0 &&
                  !strcmp(ut::g_cfg.logLevel, "info") && !strcmp(ut::g_mappedLevel, "info") &&
                  ut::configFamilyValue(ut::g_cfg, "grid_438x627") == nullptr && !ut::g_cfg.padScreen[0] &&
                  !strcmp(ut::g_cfg.journalDir, "some folder"),
              "a key the game's file does not name is the DEFAULT, never the main file's value");
        check(ut::configViewIntFor("custom-reignofterror", "collect_mi") == 1,
              "configViewIntFor agrees: the compiled default");
        {
            const long gen0 = ut::configGeneration();
            writeText(ini, INI_CUR "[general]\r\nlog_level=trace\r\ncollect_mi=0\r\n"
                                   "[advanced]\r\nplate_label_size=25\r\njournal_dir=some folder\r\n");
            ut::configReload(ini);
            check(ut::configGeneration() == gen0 && ut::g_cfg.plateLabelSize == 13 &&
                      !strcmp(ut::g_mappedLevel, "info"),
                  "editing the main file while the game is played changes nothing in that game");
            ut::configSetView("vanilla");
            check(ut::g_cfg.plateLabelSize == 25 && !strcmp(ut::g_mappedLevel, "trace") && ut::g_cfg.collectMi == 0,
                  "both files are polled: back on vanilla the main file's edit is in force");
            ut::configSetView("custom-reignofterror");
            check(ut::g_cfg.plateLabelSize == 13 && ut::g_cfg.collectMi == 1 && !strcmp(ut::g_mappedLevel, "info"),
                  "and the game's own values again on the next entry");
        }

        // enabled, collect_mi, journal_dir in the game's file
        writeText(rotIni.c_str(), INI_CUR "[general]\r\nenabled=0\r\ncollect_mi=0\r\n"
                                          "[advanced]\r\njournal_dir=elsewhere\r\nrescue=1\r\n");
        logClear();
        ut::configReload(ini);
        check(ut::g_cfg.enabled == 0 && ut::g_cfg.journal == 0 && ut::g_cfg.tooltipMark == 0 &&
                  ut::g_cfg.groupButtons == 0,
              "enabled=0 in the game's file: the master switch is off in that game (no tab, nothing collected)");
        check(ut::g_cfg.collectMi == 0 && ut::configViewIntFor("custom-reignofterror", "collect_mi") == 0,
              "collect_mi=0 in the game's file is the value in force there");
        check(logHas("journal_dir is read at start-up from the main uniquetab.ini - the line in") &&
                  !strcmp(ut::g_cfg.journalDir, "some folder"),
              "a journal_dir line in the game's file: one WARN, the main file's value stays");
        check(ut::g_cfg.rescue == 1, "rescue=1 in the game's file arms the rescue in that game");

        // THE PERSIST ROUTE while the game is active
        DWORD mainBefore = 0;
        char* mainWas = readAll(ini, &mainBefore);
        check(ut::configPersistInt("rescue", 0) && hasText(rotIni.c_str(), "\r\n;rescue=0\r\n") &&
                  !hasText(rotIni.c_str(), "\r\nrescue=1") && sameFile(ini, mainWas),
              "the one-shot rescue=0 goes back to the game's file (commented again), not the main one");
        const char* ks[4] = {"grid_438x627", "pad_438x627", "label_438x627", "pad_screen"};
        const char* vs[4] = {"5,6,7,8", nullptr, "9,9,99,20", "bl,10,10,320,44"};
        check(ut::configPersistStr(ks, vs, 4), "the editor's SAVE in the custom game");
        check(sameFile(ini, mainWas), "the main uniquetab.ini is not touched");
        check(hasText(rotIni.c_str(), "\r\ngrid_438x627=5,6,7,8\r\n") &&
                  hasText(rotIni.c_str(), "\r\nlabel_438x627=9,9,99,20\r\n") &&
                  hasText(rotIni.c_str(), "\r\npad_screen=bl,10,10,320,44\r\n") &&
                  !hasText(rotIni.c_str(), "pad_438x627") && !hasText(rotIni.c_str(), "default"),
              "every key, the screen key included, goes to the game's own file; no `default` value");
        ut::configReload(ini);
        check(!strcmp(ut::configFamilyValue(ut::g_cfg, "grid_438x627"), "5,6,7,8") &&
                  !strcmp(ut::g_cfg.padScreen, "bl,10,10,320,44"),
              "and read back from there");
        const char* kr[1] = {"grid_438x627"};
        const char* vr[1] = {nullptr};
        check(ut::configPersistStr(kr, vr, 1) && !hasText(rotIni.c_str(), "grid_438x627"),
              "a part reset to its default deletes the game's line (the main file's is never read)");
        const char* kx[2] = {"layout_edit", "journal_dir"};
        const char* vx[2] = {"0", "other folder"};
        check(ut::configPersistStr(kx, vx, 2) && hasText(rotIni.c_str(), "\r\n;layout_edit=0\r\n") &&
                  hasText(rotIni.c_str(), "\r\njournal_dir=elsewhere\r\n") && hasText(ini, "journal_dir=other folder"),
              "layout_edit goes to the game's file, journal_dir always to the main one");
        free(mainWas);
        writeText(rotIni.c_str(), INI_CUR "[general]\r\nenabled=0\r\n[layout]\r\ngrid_438x627=default\r\n");
        logClear();
        ut::configReload(ini);
        check(logHas("grid_438x627=default is not x,y,w,h") &&
                  ut::configFamilyValue(ut::g_cfg, "grid_438x627") == nullptr,
              "`default` is no value any more in a current file: refused like any bad rectangle");

        logClear();
        ut::configSetView("basemods+custom-reignofterror");
        check(!logHas("has its own settings now") && ut::g_cfg.enabled == 0,
              "basemods+custom-reignofterror reads the same file, nothing created");
        ut::configSetView("vanilla");
        check(ut::g_cfg.enabled == 1 && ut::g_cfg.journal == 1 && ut::g_cfg.plateLabelSize == 25 &&
                  ut::g_cfg.collectMi == 0 && ut::configFamilyValue(ut::g_cfg, "grid_438x627") == nullptr,
              "back on vanilla: the main values again (the game's enabled=0 stays in the game)");
        check(logHas("the vanilla view uses the main settings"), "with one INFO line");
        ut::configSetView("custom-reignofterror");
        check(ut::g_cfg.enabled == 0, "and the game's own again on the next entry");
        DeleteFileW(rotIni.c_str());
        logClear();
        ut::configReload(ini);
        check(exists(rotIni.c_str()) && logHas("uniquetab.ini was deleted - written again from the main") &&
                  ut::g_cfg.enabled == 1 && ut::g_cfg.plateLabelSize == 25 && !strcmp(ut::g_cfg.logLevel, "trace"),
              "its file deleted while the game runs: written again from the main file's values");
        ut::configSetView("vanilla");

        // THE MIGRATION of a game file from an older build (ini_version <= 41: four keys under
        // [game], a [layout] whose `default` hid a main line): its lines kept, the rest from the
        // MAIN file's values (not the defaults), rewritten whole; once.
        writeText(ini, INI_CUR "[general]\r\ncollect_mi=0\r\nlog_level=debug\r\n"
                               "[layout]\r\ngrid_438x627=1,2,3,4\r\nlabel_438x627=105,8,317,16\r\n"
                               "[advanced]\r\nplate_label_size=20\r\nexport_csv=2\r\n");
        ut::configReload(ini);
        writeText(rotIni.c_str(),
                  "; an old game file\r\n[game]\r\nplate_label=1\r\nplate_label_size=24\r\n"
                  "pad_caption_max=14\r\nplate_ground=101010\r\n"
                  "[layout]\r\ngrid_438x627=default\r\npad_974x659=10,20,400,60\r\n");
        logClear();
        ut::configSetView("custom-reignofterror");
        check(logHas("mods\\reignofterror\\uniquetab.ini (no ini_version) migrated to a complete file, "
                     "ini_version=44 - 4 setting(s) and 2 [layout] line(s) kept, every other value "
                     "copied once from the main uniquetab.ini"),
              "one INFO line for the game file");
        check(ut::g_cfg.plateLabelSize == 24 && !strcmp(ut::g_cfg.plateGround, "101010") &&
                  !strcmp(ut::g_cfg.logLevel, "debug") && ut::g_cfg.collectMi == 0 && ut::g_cfg.exportCsv == 2 &&
                  ut::configFamilyValue(ut::g_cfg, "grid_438x627") == nullptr &&
                  !strcmp(ut::configFamilyValue(ut::g_cfg, "pad_974x659"), "10,20,400,60") &&
                  !strcmp(ut::configFamilyValue(ut::g_cfg, "label_438x627"), "105,8,317,16"),
              "its lines kept, `default` dropped the main grid line, the rest the main file's values");
        UtConfig mig;
        mig.collectMi = 0;
        _snprintf_s(mig.logLevel, sizeof(mig.logLevel), _TRUNCATE, "debug");
        ut::configFamilySet(&mig.layLabel, "438x627", "105,8,317,16");
        ut::configFamilySet(&mig.layPad, "974x659", "10,20,400,60");
        mig.plateLabelSize = 24;
        _snprintf_s(mig.plateGround, sizeof(mig.plateGround), _TRUNCATE, "101010");
        mig.exportCsv = 2;
        const size_t nm = ut::configRenderView(expect, sizeof(expect), "reignofterror", mig);
        check(nm > 0 && sameFile(rotIni.c_str(), std::string(expect, nm).c_str()),
              "rewritten whole in the current template");
        logClear();
        ut::configReload(ini);
        ut::configSetView("vanilla");
        ut::configSetView("custom-reignofterror");
        check(!logHas("migrated") && !logHas("re-read") && ut::g_cfg.plateLabelSize == 24,
              "once: the rewritten file is current (its new stamp taken, no re-read)");
        ut::configSetView("vanilla");

        // THE 42 -> 44 BUMP of a COMPLETE game file (it names its ini_version): every line it
        // has is kept, and every key it does not set is the COMPILED DEFAULT - a hidden key at its
        // default is a commented row there, "missing", and must never take the main file's value
        // at the bump - while edit_button (new in 43) appears as a commented row at 1.
        writeText(ini, INI_CUR "[general]\r\ncollect_mi=0\r\nlog_level=debug\r\n"
                               "[advanced]\r\nmax_per_record=2\r\nplate_label_size=20\r\nexport_csv=2\r\n"
                               "edit_button=0\r\n");
        ut::configReload(ini);
        check(ut::g_cfg.maxPerRecord == 2 && ut::g_cfg.plateLabelSize == 20 && ut::g_cfg.editButton == 0,
              "the main file: max_per_record=2, plate_label_size=20, edit_button=0");
        {
            UtConfig g42;   // the game's own values: the live rows of its 42 file
            g42.tooltipMark = 0;
            g42.padCaptionMax = 12;
            ut::configFamilySet(&g42.layGrid, "438x627", "5,6,7,8");
            static char r42[16384];
            const size_t n42 = ut::configRenderView(r42, sizeof(r42), "reignofterror", g42);
            std::string t42(r42, n42);
            const size_t v = t42.find("\r\nini_version=" UT_STRV(UT_INI_VERSION) "\r\n");
            const size_t eb = t42.find("\r\n;edit_button=");
            const size_t ebEnd = eb == std::string::npos ? eb : t42.find("\r\n", eb + 2);
            const bool shaped = n42 > 0 && v != std::string::npos && ebEnd != std::string::npos;
            if (shaped) {
                t42.erase(eb, ebEnd - eb);                       // 42 had no edit_button row
                t42.replace(v, 18, "\r\nini_version=42\r\n");
            }
            check(shaped && t42.find("\r\n;max_per_record=1\t") != std::string::npos &&
                      t42.find("\r\n;plate_label_size=13\t") != std::string::npos &&
                      t42.find("edit_button") == std::string::npos && t42.find("\r\nini_version=42\r\n") != std::string::npos,
                  "a 42 game file: complete, its hidden keys at their defaults commented rows, no edit_button");
            // configViewIntFor over a 42 file that has not been migrated yet: the defaults too
            writeText(rotIni.c_str(), t42.c_str());
            check(ut::configViewIntFor("custom-reignofterror", "max_per_record") == 1 &&
                      ut::configViewIntFor("custom-reignofterror", "edit_button") == 1 &&
                      ut::configViewIntFor("custom-reignofterror", "pad_caption_max") == 12,
                  "configViewIntFor on a 42 game file: a commented row is the default, not the main file's");
            logClear();
            ut::configSetView("custom-reignofterror");
            check(logHas("mods\\reignofterror\\uniquetab.ini migrated from ini_version=42 to " UT_STRV(UT_INI_VERSION)
                         " - ") && logHas("every key it does not set at its default (the main uniquetab.ini is not read)") &&
                      !logHas("copied once from the main"),
                  "one INFO line naming 42; nothing copied from the main file");
            check(ut::g_cfg.maxPerRecord == 1 && ut::g_cfg.plateLabelSize == 13 && ut::g_cfg.exportCsv == 0 &&
                      ut::g_cfg.collectMi == 1 && !strcmp(ut::g_cfg.logLevel, "info") && ut::g_cfg.editButton == 1 &&
                      ut::g_cfg.tooltipMark == 0 && ut::g_cfg.padCaptionMax == 12 &&
                      !strcmp(ut::configFamilyValue(ut::g_cfg, "grid_438x627"), "5,6,7,8"),
                  "its hidden rows survive the bump untouched (max_per_record 1, not the main file's 2; "
                  "edit_button 1, not the main file's 0); its own lines kept");
            const size_t ng = ut::configRenderView(expect, sizeof(expect), "reignofterror", g42);
            check(ng > 0 && sameFile(rotIni.c_str(), std::string(expect, ng).c_str()) &&
                      hasText(rotIni.c_str(), "\r\n;edit_button=1\t") &&
                      hasText(rotIni.c_str(), "\r\n;max_per_record=1\t"),
                  "rewritten as the 44 file of its own values byte for byte: edit_button a commented row at 1");
            logClear();
            ut::configReload(ini);
            ut::configSetView("vanilla");
            ut::configSetView("custom-reignofterror");
            check(!logHas("migrated") && ut::g_cfg.maxPerRecord == 1 && ut::g_cfg.editButton == 1,
                  "once: the rewritten file is current");
            ut::configSetView("vanilla");
        }
    }

    // ---- 15. the state file -------------------------------------------------------------------
    printf("\n15. uniquetab.state: one per view folder, written by the mod\n");
    {
        DeleteFileW(state);
        writeText(ini, INI_CUR "enabled=1\r\n");
        ut::configReload(ini);
        ut::configSetView("");
        ut::configSetView("vanilla");
        check(ut::g_cfg.uniqGroup == -1 && ut::g_cfg.uniqRow == 0 && ut::g_cfg.ownedOnly == 0,
              "no state file: the materials page, row 0, the filter off");
        DWORD before = 0;
        char* was = readAll(ini, &before);
        check(ut::configPersistInt("uniq_group", 7) && ut::configPersistInt("uniq_row", 3) &&
                  ut::configPersistInt("owned_only", 1),
              "the tab persists its group, row and filter");
        check(sameFile(ini, was), "uniquetab.ini is not touched");
        free(was);
        check(sameFile(state, "; uniquetab.state - written by the mod: where the tab was left in this game.\r\n"
                              "; Not a setting: delete it to start on the materials page.\r\n"
                              "uniq_group=7\r\nuniq_row=3\r\nowned_only=1\r\n"),
              "uniquetab.state holds the three keys under its two-line header");
        check(ut::g_cfg.uniqGroup == 7 && ut::g_cfg.uniqRow == 3 && ut::g_cfg.ownedOnly == 1,
              "g_cfg takes them at once");
        ut::configReload(ini);
        check(ut::g_cfg.uniqGroup == 7, "and a reload keeps them");
        ut::configSetView("custom-reignofterror");
        check(ut::g_cfg.uniqGroup == -1 && ut::g_cfg.ownedOnly == 0, "the custom game starts on its own state");
        ut::configPersistInt("uniq_group", 28);
        check(exists(rotState.c_str()) && hasText(rotState.c_str(), "uniq_group=28\r\n"),
              "written into mods\\reignofterror\\uniquetab.state");
        ut::configSetView("basemods+custom-reignofterror");
        check(ut::g_cfg.uniqGroup == -1, "basemods+custom-reignofterror has a state of its own");
        ut::configSetView("vanilla");
        check(ut::g_cfg.uniqGroup == 7 && ut::g_cfg.uniqRow == 3, "vanilla: 7 again - no index crosses views");
        ut::configSetView("custom-reignofterror");
        check(ut::g_cfg.uniqGroup == 28, "the custom game: 28 again");
        ut::configSetView("vanilla");
    }

    // ---- 16. the old [view:<name>] blocks ------------------------------------------------------
    printf("\n16. [view:<name>] blocks: moved on the bump, ignored and dropped afterwards\n");
    {
        DeleteFileW(rotIni.c_str());
        DeleteFileW(dgdIni.c_str());
        writeText(ini, "ini_version=38\r\nplate_label_size=15\r\n"
                       "[view:reignofterror]\r\nplate_ground=101010\r\npad_y=30\r\n"
                       "[view:custom-deathgrimdawn]\r\nplate_label_size=18\r\n"
                       "[view:vanilla]\r\npad_caption_max=10\r\n"
                       "[advanced]\r\nexport_csv=1\r\n");
        logClear();
        ut::configReload(ini);
        check(hasText(rotIni.c_str(), "\r\nplate_ground=101010\t") &&
                  hasText(rotIni.c_str(), "\r\nplate_label_size=15\t") &&
                  hasText(rotIni.c_str(), "\r\nini_version=" UT_STRV(UT_INI_VERSION) "\r\n"),
              "[view:reignofterror] -> mods\\reignofterror\\uniquetab.ini, a complete file: the seed + its value");
        check(logHas("[view:reignofterror] moved to mods\\reignofterror\\uniquetab.ini - 1 "
                     "setting(s) (dropped, not a setting of a game's file: pad_y)"),
              "one INFO line naming what moved and what was dropped");
        check(hasText(dgdIni.c_str(), "\r\nplate_label_size=18\t"), "[view:custom-deathgrimdawn] -> its file");
        check(logHas("[view:vanilla] (1 line(s): pad_caption_max) is not carried over"),
              "a vanilla block has no file to go to: one WARN");
        check(!hasText(ini, "[view:") && hasText(ini, "\r\nexport_csv=1\t") &&
                  ut::g_cfg.plateLabelSize == 15 && ut::g_cfg.plateGround[0] == 0 &&
                  ut::g_cfg.padCaptionMax == 14,
              "the main file has no block left, its own values stay, nothing applied globally");
        ut::configSetView("custom-reignofterror");
        check(!strcmp(ut::g_cfg.plateGround, "101010"), "and the game reads its moved value");
        ut::configSetView("vanilla");

        writeText(ini, INI_CUR "tooltip_mark=1            ; keep me\r\n[view:reignofterror]\r\n"
                               "plate_ground=202020\r\n; a note\r\n[advanced]\r\nexport_csv=2\r\n");
        logClear();
        ut::configReload(ini);
        check(logHas("1 [view:...] line(s) are ignored") && ut::g_cfg.plateGround[0] == 0 &&
                  ut::g_cfg.exportCsv == 2,
              "a block in a 39 file: one WARN, never applied, the lines after it still read");
        ut::configPersistInt("tooltip_mark", 1);
        check(sameFile(ini, INI_CUR "tooltip_mark=1            ; keep me\r\n[advanced]\r\nexport_csv=2\r\n"),
              "and the next rewrite drops it");
    }

    // ---- 17. the screen keys ----------------------------------------------------------------
    printf("\n17. pad_screen / label_screen: absent until set, the editor's lines, one pair per settings file\n");
    {
        DeleteFileW(rotIni.c_str());
        ut::configSetView("vanilla");
        DeleteFileW(ini);
        ut::configReload(ini);   // a fresh template
        ut::configReload(ini);
        check(!hasText(ini, "pad_screen") && !hasText(ini, "label_screen") &&
                  !ut::g_cfg.padScreen[0] && !ut::g_cfg.labelScreen[0],
              "a fresh file has no screen line, and both keys are unset");
        const char* ks[3] = {"pad_438x627", "pad_screen", "label_screen"};
        const char* vs[3] = {"102,30,320,80", "tr,8,8,320,44", "TL , 12, 6, 317, 16"};
        check(ut::configPersistStr(ks, vs, 3),
              "the editor's SAVE: a family line and both screen keys in one rewrite");
        DWORD len = 0;
        char* text = readAll(ini, &len);
        check(strstr(text, "default back.\r\n"
                           ZOOM_ROW
                           "pad_438x627=102,30,320,80\r\n"
                           "pad_screen=tr,8,8,320,44\r\n"
                           "label_screen=TL , 12, 6, 317, 16\r\n"
                           "\r\n[keys]\r\n") != nullptr,
              "all three land at the end of [layout], in the order given");
        free(text);
        ut::configReload(ini);
        ut::UtScreenRect pr = {}, lr = {};
        check(!strcmp(ut::g_cfg.padScreen, "tr,8,8,320,44") &&
                  ut::utScreenRectParse(ut::g_cfg.padScreen, &pr) && pr.corner == ut::kUtCornerTR &&
                  pr.x == 8 && pr.y == 8 && pr.w == 320 && pr.h == 44,
              "pad_screen=tr,8,8,320,44 reads back and parses");
        check(!strcmp(ut::g_cfg.labelScreen, "TL , 12, 6, 317, 16") &&
                  ut::utScreenRectParse(ut::g_cfg.labelScreen, &lr) &&
                  lr.corner == ut::kUtCornerTL && lr.x == 12 && lr.y == 6 && lr.w == 317 &&
                  lr.h == 16,
              "label_screen keeps the text as typed (trimmed); its parse is tl,12,6,317,16");
        char again[16384];
        const size_t n = ut::configRender(again, sizeof(again), ut::g_cfg);
        check(n > 0 && sameFile(ini, std::string(again, n).c_str()),
              "the persisted file IS the table's rendering: a rewrite would not move a line");
        {
            DWORD l2 = 0;
            char* t2 = readAll(ini, &l2);
            check(patch(t2, "ini_version=44", "ini_version=38"), "pretend the file is one version old");
            writeAll(ini, t2, l2);
            free(t2);
        }
        logClear();
        ut::configReload(ini);
        char kept[96];
        _snprintf_s(kept, sizeof(kept), _TRUNCATE, "4 setting(s) kept, %d new one(s) defaulted",
                    ut::kUtCfgKeyCount - whenSetRows() - 4);
        check(!strcmp(ut::g_cfg.padScreen, "tr,8,8,320,44") && hasText(ini, "\r\npad_screen=tr,8,8,320,44\r\n") &&
                  hasText(ini, "\r\nlabel_screen=TL , 12, 6, 317, 16\r\n") && logHas(kept),
              "a bump keeps both lines and leaves them out of its counts");

        writeText(ini, INI_CUR "[layout]\r\npad_screen=tr,-8,8,320,44\r\nlabel_screen=xx,1,2,3,4\r\n");
        logClear();
        ut::configReload(ini);
        check(logHas("pad_screen=tr,-8,8,320,44 is not <corner>,x,y,w,h") &&
                  logHas("label_screen=xx,1,2,3,4 is not <corner>,x,y,w,h") &&
                  !ut::g_cfg.padScreen[0] && !ut::g_cfg.labelScreen[0],
              "a negative offset and an unknown corner: one WARN each, both keys stay unset");
        {
            std::string longOne = INI_CUR "[layout]\r\npad_screen=tr,";
            longOne += std::string(60, ' ');
            longOne += "8,8,320,44\r\n";
            writeText(ini, longOne.c_str());
            logClear();
            ut::configReload(ini);
            check(logHas("is too long for this setting - ignored") && !ut::g_cfg.padScreen[0],
                  "a value longer than its member (63) is refused whole, never cut into another one");
        }
        writeText(ini, INI_CUR "[layout]\r\npad_screen=   \r\n");
        logClear();
        ut::configReload(ini);
        check(!ut::g_cfg.padScreen[0] && !logHas("pad_screen=") ,
              "an empty value is simply unset, no WARN");

        writeText(ini, INI_CUR "[layout]\r\npad_screen=br,4,4,200,40 ; mine\r\n[advanced]\r\n");
        ut::configReload(ini);
        check(!strcmp(ut::g_cfg.padScreen, "br,4,4,200,40"), "a hand-written line with a comment reads");
        const char* kd[1] = {"pad_screen"};
        const char* vd[1] = {nullptr};
        check(ut::configPersistStr(kd, vd, 1) && sameFile(ini, INI_CUR "[layout]\r\n[advanced]\r\n"),
              "a NULL value deletes the line (pin to window)");
        ut::configReload(ini);
        check(!ut::g_cfg.padScreen[0], "and the key is unset again");

        // ONE value per SETTINGS FILE: a custom game's SAVE puts its family line and its screen
        // key in the game's own file; vanilla keeps the main file's.
        logClear();
        ut::configSetView("custom-reignofterror");
        const char* kg[2] = {"pad_974x659", "pad_screen"};
        const char* vg[2] = {"0,1,780,153", "bl,10,10,320,44"};
        check(ut::configPersistStr(kg, vg, 2), "SAVE in a custom game: one family line, one screen key");
        check(hasText(rotIni.c_str(), "\r\npad_974x659=0,1,780,153\r\n") &&
                  hasText(rotIni.c_str(), "\r\npad_screen=bl,10,10,320,44\r\n") &&
                  !hasText(ini, "pad_screen") && !hasText(ini, "pad_974x659"),
              "both went to the game's own file, nothing to the main uniquetab.ini");
        ut::configReload(ini);
        check(!strcmp(ut::g_cfg.padScreen, "bl,10,10,320,44"), "in force in that game");
        ut::configSetView("vanilla");
        check(!ut::g_cfg.padScreen[0], "and not in vanilla: the main file has no screen line");
        const char* km[1] = {"pad_screen"};
        const char* vm[1] = {"tl,1,1,320,44"};
        check(ut::configPersistStr(km, vm, 1) && hasText(ini, "\r\npad_screen=tl,1,1,320,44\r\n") &&
                  hasText(rotIni.c_str(), "\r\npad_screen=bl,10,10,320,44\r\n"),
              "vanilla's SAVE writes the main file's line; the game's own is untouched");
        ut::configReload(ini);
        ut::configSetView("custom-reignofterror");
        check(!strcmp(ut::g_cfg.padScreen, "bl,10,10,320,44"), "each file's screen key in its own views");
        ut::configSetView("vanilla");
        check(!strcmp(ut::g_cfg.padScreen, "tl,1,1,320,44"), "and the main file's on return");
    }

    // ---- 18. [keys]: the main file's only ------------------------------------------------------
    printf("\n18. [keys]: a remap, the refusals, the conflict pass, 43 -> 44, the main file only\n");
    {
        ut::configSetView("vanilla");
        DeleteFileW(ini);
        ut::configReload(ini);   // a fresh template
        ut::configReload(ini);
        const UtKeys d;
        check(!memcmp(&ut::g_cfg.keys, &d, sizeof(d)), "a fresh file: the default bindings");
        DWORD len = 0;
        char* text = readAll(ini, &len);
        const std::string fresh(text, len);
        free(text);
        auto edit = [](std::string t, const char* a, const char* b) {
            const size_t at = t.find(a);
            if (at == std::string::npos) return std::string();
            t.replace(at, strlen(a), b);
            return t;
        };
        auto bindIs = [](const UtKeyBind& b, int kind, int code, int mods) {
            return b.kind == kind && b.code == code && b.mods == mods;
        };

        // a remap by hand: the row uncommented in place
        const std::string remap = edit(fresh, "\r\n;button_next=Ctrl+PageDown\t", "\r\nbutton_next=Ctrl+F3\t\t");
        check(!remap.empty(), "button_next's commented row is in the fresh file");
        writeText(ini, remap.c_str());
        long gen0 = ut::configGeneration();
        logClear();
        ut::configReload(ini);
        check(bindIs(ut::g_cfg.keys.buttonNext, kUtKeyKey, VK_F3, kUtKeyModCtrl) &&
                  !strcmp(ut::g_cfg.keyButtonNext, "Ctrl+F3") && ut::configGeneration() != gen0 &&
                  g_logWarns == 0 && bindIs(ut::g_cfg.keys.buttonPrev, kUtKeyKey, VK_PRIOR, kUtKeyModCtrl),
              "button_next=Ctrl+F3 by hand: in force at the next reload, the generation moves, no WARN");
        // a one-shot's write-back keeps the line byte for byte
        check(ut::configPersistInt("layout_edit", 1) && hasText(ini, "\r\nbutton_next=Ctrl+F3\t\t; the button after it\r\n"),
              "a one-shot's write-back (layout_edit=1) leaves the remapped line as it was");
        ut::configPersistInt("layout_edit", 0);
        ut::configReload(ini);
        // 43 -> 44: the bump's rewrite keeps the remapped line, live in its row
        {
            DWORD l = 0;
            char* t = readAll(ini, &l);
            check(patch(t, "ini_version=44", "ini_version=43"), "pretend the remapped file is a 43 one");
            writeAll(ini, t, l);
            free(t);
        }
        logClear();
        ut::configReload(ini);
        {
            static char again[16384];
            const size_t n = ut::configRender(again, sizeof(again), ut::g_cfg);
            check(logHas("migrated from ini_version=43 to 44") && n > 0 &&
                      sameFile(ini, std::string(again, n).c_str()) &&
                      hasText(ini, "\r\nbutton_next=Ctrl+F3\t\t; the button after it\r\n") &&
                      bindIs(ut::g_cfg.keys.buttonNext, kUtKeyKey, VK_F3, kUtKeyModCtrl),
                  "43 -> 44 rewrites the file: button_next=Ctrl+F3 stays a LIVE row of [keys], in force");
        }
        // a 43 file WITHOUT [keys] (what this release wrote before) gains the seven commented rows
        {
            std::string old43 = fresh;
            const size_t ka = old43.find("\r\n[keys]\r\n");
            const size_t kb = old43.find("\r\n[advanced]\r\n");
            const bool cut = ka != std::string::npos && kb != std::string::npos && ka < kb;
            if (cut) old43.erase(ka, kb - ka);
            old43 = edit(edit(old43, "ini_version=44", "ini_version=43"), "tooltip_mark=1 ", "tooltip_mark=0 ");
            check(cut && !old43.empty() && old43.find("rows_up") == std::string::npos,
                  "a 43 file: no [keys] section, tooltip_mark=0");
            writeText(ini, old43.c_str());
            logClear();
            ut::configReload(ini);
            const std::string want = edit(fresh, "tooltip_mark=1 ", "tooltip_mark=0 ");
            check(logHas("migrated from ini_version=43 to 44") && logHas("4 setting(s) kept") &&
                      sameFile(ini, want.c_str()) && ut::g_cfg.tooltipMark == 0 &&
                      !memcmp(&ut::g_cfg.keys, &d, sizeof(d)),
                  "rewritten once: the 44 template byte for byte, tooltip_mark=0 kept, the seven [keys] rows new and commented");
            logClear();
            ut::configReload(ini);
            ut::configReload(ini);
            check(!logHas("migrated"), "and only once");
        }
        // refused values: one WARN each, per edit; the default stays. An empty value is the default.
        {
            std::string bad = edit(fresh, "\r\n;rows_up=PageUp\t", "\r\nrows_up=Ctrl+Q+W\t");
            bad = edit(bad, "\r\n;button_prev=Ctrl+PageUp\t", "\r\nbutton_prev=Q\t\t");
            bad = edit(bad, "\r\n;rows_down=PageDown\t", "\r\nrows_down=\t");
            bad = edit(bad, "\r\n;editor_open=Ctrl+RightClick\t", "\r\neditor_open=MiddleClick\t");
            check(!bad.empty(), "four rows edited by hand");
            writeText(ini, bad.c_str());
            logClear();
            ut::configReload(ini);
            const int warns = g_logWarns;
            check(logHas("config: rows_up=Ctrl+Q+W is two keys in one chord") &&
                      logHas("- ignored, rows_up=PageUp stays"),
                  "rows_up=Ctrl+Q+W: one WARN naming the line and the reason, PageUp stays");
            check(logHas("config: button_prev=Q is a key that would also type into the stash search box - add Ctrl or Alt"),
                  "button_prev=Q: refused, the search box named");
            check(logHas("config: editor_open=MiddleClick is MiddleClick, which cannot open the editor in this release"),
                  "editor_open=MiddleClick: refused in this release");
            check(warns == 3 && !memcmp(&ut::g_cfg.keys, &d, sizeof(d)) && !strcmp(ut::g_cfg.keyRowsDown, "PageDown") &&
                      !strcmp(ut::g_cfg.keyRowsUp, "PageUp"),
                  "three WARNs, every binding the default; rows_down= (empty) is PageDown");
            logClear();
            ut::configReload(ini);
            check(g_logWarns == 0, "the next reload of the unchanged file says nothing again");
        }
        // the conflict pass: rows_up=Ctrl+PageUp while button_prev keeps Ctrl+PageUp
        {
            const std::string clash = edit(fresh, "\r\n;rows_up=PageUp\t\t\t", "\r\nrows_up=Ctrl+PageUp\t");
            writeText(ini, clash.c_str());
            logClear();
            ut::configReload(ini);
            check(logHas("config: button_prev=Ctrl+PageUp is the same Ctrl+PageUp as rows_up=Ctrl+PageUp - "
                         "button_prev is off (none) until one of them changes") &&
                      ut::g_cfg.keys.buttonPrev.kind == kUtKeyNone &&
                      bindIs(ut::g_cfg.keys.rowsUp, kUtKeyKey, VK_PRIOR, kUtKeyModCtrl) &&
                      !strcmp(ut::g_cfg.keyButtonPrev, "Ctrl+PageUp"),
                  "one WARN naming both; button_prev (the later row) is none, its text kept as the file says");
        }
        // MAIN FILE ONLY: a custom game's own file
        writeText(ini, remap.c_str());
        ut::configReload(ini);
        ut::configSetView("custom-reignofterror");
        writeText(rotIni.c_str(), INI_CUR "[general]\r\nenabled=1\r\n[keys]\r\nbutton_next=Ctrl+F5\r\nrows_up=Home\r\n");
        logClear();
        ut::configReload(ini);
        check(logHas("[keys] is read from the main uniquetab.ini only, for every game - 2 line(s) in") &&
                  logHas("are ignored: button_next, rows_up"),
              "a [keys] line in a game's file: one WARN naming them, ignored");
        check(bindIs(ut::g_cfg.keys.buttonNext, kUtKeyKey, VK_F3, kUtKeyModCtrl) &&
                  !strcmp(ut::g_cfg.keyButtonNext, "Ctrl+F3") &&
                  bindIs(ut::g_cfg.keys.rowsUp, kUtKeyKey, VK_PRIOR, 0),
              "in the game the main file's [keys] are in force (button_next=Ctrl+F3, rows_up=PageUp)");
        logClear();
        ut::configReload(ini);
        check(!logHas("[keys] is read from"), "said once per edit");
        gen0 = ut::configGeneration();
        writeText(ini, edit(fresh, "\r\n;button_next=Ctrl+PageDown\t", "\r\nbutton_next=Ctrl+F4\t\t").c_str());
        ut::configReload(ini);
        check(bindIs(ut::g_cfg.keys.buttonNext, kUtKeyKey, VK_F4, kUtKeyModCtrl) && ut::configGeneration() != gen0,
              "a [keys] edit of the main file applies while the game is played, at the next reload");
        DeleteFileW(rotIni.c_str());
        ut::configReload(ini);
        check(exists(rotIni.c_str()) && !hasText(rotIni.c_str(), "\r\n[keys]\r\n") &&
                  !hasText(rotIni.c_str(), "button_next") && !hasText(rotIni.c_str(), "editor_open=") &&
                  bindIs(ut::g_cfg.keys.buttonNext, kUtKeyKey, VK_F4, kUtKeyModCtrl),
              "the game's file seeded again from the main values: no [keys] section, the main's still in force");
        ut::configSetView("vanilla");
        check(bindIs(ut::g_cfg.keys.buttonNext, kUtKeyKey, VK_F4, kUtKeyModCtrl), "and on vanilla, the same");
        writeText(ini, fresh.c_str());
        ut::configReload(ini);
        check(!memcmp(&ut::g_cfg.keys, &d, sizeof(d)), "the fresh file back: the defaults");
    }

    folderTests(ini, dir, state);   // 14b
    zoomTests(ini, rotIni);         // 19

    // The run's own folder goes with it; nothing but this harness's files is in it.
    ut::configSetView("");
    DeleteFileW(ini);
    DeleteFileW(trigger);
    DeleteFileW(triggerTxt);
    DeleteFileW(state);
    for (const std::wstring* p : {&rotIni, &rotState, &dgdIni, &dgdState, &bmState}) DeleteFileW(p->c_str());
    for (const char* v : {"reignofterror\\basemods", "reignofterror", "deathgrimdawn"}) {
        wchar_t d[MAX_PATH];
        _snwprintf_s(d, _TRUNCATE, L"%s\\mods\\%S", dir, v);
        RemoveDirectoryW(d);
    }
    {
        wchar_t d[MAX_PATH];
        _snwprintf_s(d, _TRUNCATE, L"%s\\mods", dir);
        RemoveDirectoryW(d);
    }
    if (!RemoveDirectoryW(dir)) printf("note: %S was left behind\n", dir);

    printf("\n%s (%d failure(s))\n", g_fails ? "FAILED" : "PASSED", g_fails);
    return g_fails ? 1 : 0;
}
