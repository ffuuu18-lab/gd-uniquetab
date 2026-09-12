// test_config.cpp - the settings file, offline. No game, no engine.
//
//   tools\build_test_config.bat        (needs a vcvars64 shell; the .bat calls it itself)
//
// Everything it expects is DERIVED FROM THE KEY TABLE in src\ut_config.h - there is no magic
// byte count and no second copy of the key list here. It proves, in a temp directory:
//   1. the table and the struct agree on every default (they are written in two places);
//   2. a fresh file is the whole rendered template and names every key exactly once;
//   3. a round trip: a value of each type survives file -> parser -> file;
//   4. a VERSION BUMP KEEPS the user's values, and takes the default for keys the old file did
//      not have - the thing a bump used to destroy;
//   5. a bump drops a key this build no longer has, and says which in the log;
//   6. a value outside its range is clamped and logged, and a word that is not one of the
//      choices is refused;
//   7. the one-shot write-back (rescue=1 -> the mod writes rescue=0 back) keeps the rest of the
//      file byte for byte, comments included;
//   8. the RESCUE-NOW trigger file arms rescue=1, is consumed, and stays latched until the
//      rescue writes rescue=0 back;
//   9. enabled=0 collapses the feature switches and nothing else.
//  10. THE PATH RULES (src\ut_paths.cpp), for both placements the loader allows: the .asi beside
//      the exe and the .asi in a scripts\ folder. The mod folder follows the .asi; the game
//      folder comes from the host exe and does not move with it.
// Exit code 0 = all of it held.
#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/ut_config.h"
#include "../src/ut_log.h"
#include "../src/ut_paths.h"

// ---- ut_log stubs, with a transcript the checks can search ---------------------------------
static char g_logBuf[65536];
static size_t g_logLen = 0;

static void logClear() {
    g_logBuf[0] = 0;
    g_logLen = 0;
}

static bool logHas(const char* needle) { return strstr(g_logBuf, needle) != nullptr; }

namespace ut {
volatile long g_logLevel = UT_LOG_TRACE;  // the harness wants every line, whatever the ini says
void logSetLevel(const char*) {}
void logAtV(int level, const char* fmt, va_list ap) {
    char line[2048];
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    const char* tags = "EWIDT";
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
    wchar_t ini[MAX_PATH], trigger[MAX_PATH], triggerTxt[MAX_PATH];
    _snwprintf_s(ini, _TRUNCATE, L"%s\\uniquetab.ini", dir);
    _snwprintf_s(trigger, _TRUNCATE, L"%s\\RESCUE-NOW", dir);
    _snwprintf_s(triggerTxt, _TRUNCATE, L"%s\\RESCUE-NOW.txt", dir);
    DeleteFileW(ini);
    DeleteFileW(trigger);
    DeleteFileW(triggerTxt);
    printf("settings file = %S\n", ini);
    printf("%d keys in %d sections, a full file is %u bytes\n\n", ut::kUtCfgKeyCount,
           ut::kUtCfgSectionCount, (unsigned)ut::configTemplateBytes());

    // ---- 1. the table and the struct agree ---------------------------------------------------
    printf("1. the table's defaults are the struct's defaults\n");
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
    }

    // ---- 2. a fresh file ---------------------------------------------------------------------
    printf("\n2. the first run writes the whole file\n");
    logClear();
    ut::configReload(ini);  // no file yet -> writes it, returns false
    const DWORD tpl = fileSize(ini);
    printf("    on disk = %u bytes\n", tpl);
    check(tpl == (DWORD)ut::configTemplateBytes(), "the file is exactly what the table renders");
    check(ut::configReload(ini), "the second reload parses it");
    check(ut::g_cfg.iniVersion == UT_INI_VERSION, "ini_version matches UT_INI_VERSION");
    {
        DWORD len = 0;
        char* text = readAll(ini, &len);
        int missing = 0, dupes = 0;
        for (int i = 0; i < ut::kUtCfgKeyCount; ++i) {
            const int n = countKeyLines(text, ut::kUtCfgKeys[i].name);
            if (n == 0) {
                printf("    %s is not in the file\n", ut::kUtCfgKeys[i].name);
                ++missing;
            } else if (n > 1) {
                printf("    %s is in the file %d times\n", ut::kUtCfgKeys[i].name, n);
                ++dupes;
            }
        }
        check(missing == 0 && dupes == 0, "every key is on exactly one line of its own");
        int sections = 0;
        for (int s = 0; s < ut::kUtCfgSectionCount; ++s) {
            char head[64];
            _snprintf_s(head, sizeof(head), _TRUNCATE, "\r\n[%s]\r\n", ut::kUtCfgSections[s].name);
            if (strstr(text, head)) ++sections;
        }
        check(sections == ut::kUtCfgSectionCount, "every section has its own [block] header");
        check(strstr(text, "; Unique Collection Tab") != nullptr, "the file says what it is");
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
        check(bad == 0, "a fresh file parses back to the defaults, key for key");
    }

    // ---- 3. a round trip ---------------------------------------------------------------------
    printf("\n3. a round trip through the file\n");
    {
        char edited[512];
        _snprintf_s(edited, sizeof(edited), _TRUNCATE,
                    "ini_version=%d\r\n"
                    "[collection]\r\n"
                    "  max_per_record = 4   ; a comment, and blanks around the '='\r\n"
                    "log_level=warn\r\n"
                    "tooltip_text_yes=Got one!   ; and one after a text\r\n"
                    "journal_dir=some folder\r\n",
                    UT_INI_VERSION);
        writeText(ini, edited);
        ut::configReload(ini);
        check(ut::g_cfg.maxPerRecord == 4, "max_per_record=4 came back");
        check(!strcmp(ut::g_cfg.logLevel, "warn"), "log_level=warn came back");
        check(!strcmp(ut::g_cfg.journalDir, "some folder"), "journal_dir kept its space");
        check(!strcmp(ut::g_cfg.tooltipTextYes, "Got one!"),
              "tooltip_text_yes kept its text and lost the comment and the padding");
        char again[16384];
        const size_t n = ut::configRender(again, sizeof(again), ut::g_cfg);
        check(n > 0 && strstr(again, "max_per_record=4") && strstr(again, "log_level=warn") &&
                  strstr(again, "tooltip_text_yes=Got one!"),
              "rendering what was parsed gives the same three values back");
    }

    // ---- 4. a version bump KEEPS the user's values -------------------------------------------
    printf("\n4. a version bump keeps what you changed\n");
    {
        DWORD len = 0;
        char* text = readAll(ini, &len);
        char older[32];
        _snprintf_s(older, sizeof(older), _TRUNCATE, "ini_version=%d", UT_INI_VERSION - 1);
        char current[32];
        _snprintf_s(current, sizeof(current), _TRUNCATE, "ini_version=%d", UT_INI_VERSION);
        check(patch(text, current, older), "pretend the file is one version old");
        writeAll(ini, text, len);
        free(text);
        logClear();
        ut::configReload(ini);
        check(ut::g_cfg.maxPerRecord == 4, "max_per_record=4 SURVIVED the bump");
        check(!strcmp(ut::g_cfg.logLevel, "warn"), "log_level=warn survived the bump");
        check(!strcmp(ut::g_cfg.tooltipTextYes, "Got one!"), "the text survived the bump");
        check(!strcmp(ut::g_cfg.journalDir, "some folder"), "so did the folder");
        check(ut::g_cfg.exportGds == 1 && ut::g_cfg.padH == 14,
              "the keys the old file did not have took their default");
        check(ut::g_cfg.iniVersion == UT_INI_VERSION, "the version is the current one now");
        check(logHas("migrated"), "the log says it migrated the file");
        text = readAll(ini, &len);
        check(strstr(text, current) != nullptr, "the file carries the new version");
        check(strstr(text, "max_per_record=4") != nullptr, "and the kept value");
        check(len == (DWORD)ut::configTemplateBytes(), "and is a complete file again");
        free(text);
    }

    // ---- 5. a bump from a file that predates most of the keys --------------------------------
    printf("\n5. a bump from an old, short file\n");
    {
        char old[512];
        _snprintf_s(old, sizeof(old), _TRUNCATE,
                    "ini_version=%d\r\n"
                    "max_per_record=9\r\n"
                    "drop_trace=1\r\n"
                    "tab_col_x=417\r\n",
                    UT_INI_VERSION - 1);
        writeText(ini, old);
        logClear();
        ut::configReload(ini);
        check(ut::g_cfg.maxPerRecord == 9, "the one setting it had was kept");
        check(ut::g_cfg.exportGds == 1 && ut::g_cfg.padH == 14,
              "every key the old file lacked took its default");
        check(logHas("drop_trace") && logHas("tab_col_x"),
              "the log names both keys this build does not have");
        check(logHas("2 setting(s) this build no longer has"), "and says how many were dropped");
        DWORD len = 0;
        char* text = readAll(ini, &len);
        check(strstr(text, "drop_trace") == nullptr && strstr(text, "tab_col_x") == nullptr,
              "they are gone from the rewritten file");
        check(len == (DWORD)ut::configTemplateBytes(), "which is a complete file");
        check(strstr(text, "max_per_record=9") != nullptr, "holding the value that was kept");
        free(text);
    }

    // ---- 5b. the migration a player will actually do -----------------------------------------
    // The complete key set of the build before this one, in the order its own template wrote
    // them. 43 keys: 34 of them are still settings, and 9 became fixed behaviour. The settings
    // this build adds are not in it, so they are exactly the keys that take their default.
    printf("\n5b. the whole previous key set, migrated\n");
    {
        char old[2048];
        _snprintf_s(old, sizeof(old), _TRUNCATE,
                    "ini_version=%d\r\n"
                    "db_load=1\r\n"
                    "uniq_page=-1\r\n"
                    "page_hotkeys=1\r\n"
                    "live_pages=1\r\n"
                    "uniq_group=-1\r\n"
                    "uniq_row=0\r\n"
                    "owned_only=1\r\n"
                    "search_buttons=1\r\n"
                    "search_buttons_unowned=1\r\n"
                    "live_cache_max=4096\r\n"
                    "plate_swap=1\r\n"
                    "plate_label=1\r\n"
                    "plate_label_size=13\r\n"
                    "group_buttons=1\r\n"
                    "group_buttons_observe=0\r\n"
                    "pad_y=25\r\n"
                    "pad_h=14\r\n"
                    "pad_gap=1\r\n"
                    "button_icons=0\r\n"
                    "compare_popup=1\r\n"
                    "tooltip_mark=1\r\n"
                    "tooltip_text_yes=\r\n"
                    "tooltip_text_no=Nope\r\n"
                    "tooltip_class_yes=44\r\n"
                    "tooltip_class_no=74\r\n"
                    "plate_count_ms=1000\r\n"
                    "journal=1\r\n"
                    "journal_dir=\r\n"
                    "journal_prune=0\r\n"
                    "rescue=0\r\n"
                    "export_gds=1\r\n"
                    "export_csv=2\r\n"
                    "take_watch=1\r\n"
                    "take_watch_ms=400\r\n"
                    "max_per_record=9\r\n"
                    "collect_pristine_only=0\r\n"
                    "collect_quick_pass=1\r\n"
                    "log_flush_each_line=0\r\n"
                    "identity=1\r\n"
                    "identity_window_ms=250\r\n"
                    "identity_require_callsite=0\r\n"
                    "soulbound_collect=1\r\n"
                    "mp_collect=1\r\n",
                    UT_INI_VERSION - 1);
        writeText(ini, old);
        logClear();
        ut::configReload(ini);
        check(logHas("34 setting(s) kept, 5 new one(s) defaulted"),
              "34 of the 43 old keys are still settings, and 5 are new");
        check(logHas("9 setting(s) this build no longer has"), "the other 9 became fixed");
        check(logHas("db_load, uniq_page, live_pages, group_buttons_observe, button_icons, "
                     "journal, take_watch, identity, identity_require_callsite"),
              "and the log names every one of them, in the order they were read");
        check(ut::g_cfg.maxPerRecord == 9 && ut::g_cfg.ownedOnly == 1 && ut::g_cfg.exportCsv == 2,
              "the values the player had changed are still there");
        check(!strcmp(ut::g_cfg.tooltipTextNo, "Nope"), "so is the text they wrote");
        check(ut::g_cfg.enabled == 1 && !strcmp(ut::g_cfg.logLevel, "info") &&
                  ut::g_cfg.uiScalePct == 0 && ut::g_cfg.searchSweep == 32,
              "the four new settings took their default");
        check(ut::g_cfg.journal == 1 && ut::g_cfg.livePages == 1 && ut::g_cfg.dbLoad == 1,
              "the fixed behaviour is at the value it always defaulted to");
        DWORD len = 0;
        char* text = readAll(ini, &len);
        int left = 0;
        static const char* const kGone[9] = {"db_load",    "uniq_page",  "live_pages",
                                             "group_buttons_observe",   "button_icons",
                                             "journal",    "take_watch", "identity",
                                             "identity_require_callsite"};
        for (int i = 0; i < 9; ++i)
            if (countKeyLines(text, kGone[i]) != 0) {
                printf("    %s is still in the file\n", kGone[i]);
                ++left;
            }
        check(left == 0, "none of the 9 is in the rewritten file");
        check(countKeyLines(text, "journal_dir") == 1 && countKeyLines(text, "journal_prune") == 1,
              "and dropping journal did not take journal_dir or journal_prune with it");
        check(len == (DWORD)ut::configTemplateBytes(), "the file is a complete one");
        free(text);
    }

    // ---- 6. bad values -----------------------------------------------------------------------
    printf("\n6. a value outside its range, and a word that is not a choice\n");
    {
        DWORD len = 0;
        char* text = readAll(ini, &len);
        check(patch(text, "pad_h=14", "pad_h=99"), "write pad_h=99 (the range is 12..20)");
        check(patch(text, "log_level=info", "log_level=lou1"), "write a log_level that is not one");
        check(patch(text, "tooltip_class_no=74", "tooltip_class_no=-9"), "and a negative class");
        writeAll(ini, text, len);
        free(text);
        logClear();
        ut::configReload(ini);
        check(ut::g_cfg.padH == 20, "pad_h was clamped to the top of its range");
        check(logHas("pad_h=99 is outside 12..20"), "and the log says so");
        check(ut::g_cfg.tooltipClassNo == 1, "tooltip_class_no was clamped to the bottom");
        check(!strcmp(ut::g_cfg.logLevel, "info"), "the unknown log_level fell back to the default");
        check(logHas("log_level=lou1 is not one of the words"), "and the log says that too");
        text = readAll(ini, &len);
        check(strstr(text, "pad_h=99") != nullptr,
              "the file still says what the user typed - a clamp is not written back");
        free(text);
    }

    // ---- 6b. a misspelled key at the current version -----------------------------------------
    printf("\n6b. a line that names nothing\n");
    {
        DWORD before = 0;
        char* text = readAll(ini, &before);
        check(patch(text, "pad_gap=1", "pad_gpa=1"), "misspell pad_gap");
        writeAll(ini, text, before);
        free(text);
        logClear();
        ut::configReload(ini);
        check(logHas("name a setting this build does not have and are ignored: pad_gpa"),
              "the log names the line instead of swallowing it");
        check(ut::g_cfg.padGap == 1, "the real setting kept its default");
        DWORD after = 0;
        text = readAll(ini, &after);
        check(after == before && strstr(text, "pad_gpa=1") != nullptr,
              "the file is left alone - the typo is the user's line to fix");
        free(text);
        text = readAll(ini, &after);
        check(patch(text, "pad_gpa=1", "pad_gap=1"), "spell it properly again");
        writeAll(ini, text, after);
        free(text);
        logClear();
        ut::configReload(ini);
        check(!logHas("does not have"), "and then the log says nothing about it");
    }

    // ---- 7. the one-shot write-back ----------------------------------------------------------
    printf("\n7. a one-shot command writes itself back to 0\n");
    {
        DWORD before = 0;
        char* text = readAll(ini, &before);
        check(patch(text, "rescue=0", "rescue=1"), "set rescue=1 by hand");
        writeAll(ini, text, before);
        free(text);
        ut::configReload(ini);
        check(ut::g_cfg.rescue == 1, "rescue=1 is read");
        check(ut::configPersistInt("rescue", 0), "the run writes rescue=0 back");
        DWORD after = 0;
        text = readAll(ini, &after);
        check(after == before, "the file is exactly as long as it was");
        check(strstr(text, "rescue=0") != nullptr, "rescue is 0 in the file");
        check(strstr(text, "; 1 = hand every stored item back") != nullptr,
              "the rewritten line kept its own comment");
        check(strstr(text, "pad_h=99") != nullptr, "every other line is untouched");
        free(text);
        ut::configReload(ini);
        check(ut::g_cfg.rescue == 0, "and it reads back as 0");
        check(ut::configPersistInt("uniq_group", 7), "the tab persists its view the same way");
        ut::configReload(ini);
        check(ut::g_cfg.uniqGroup == 7, "uniq_group=7 is read back");
        check(ut::g_cfg.maxPerRecord == 9, "a key far from it is untouched");
        ut::configPersistInt("uniq_group", -1);
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
        DWORD len = 0;
        char* text = readAll(ini, &len);
        check(patch(text, "enabled=1", "enabled=0"), "set enabled=0");
        writeAll(ini, text, len);
        free(text);
        logClear();
        ut::configReload(ini);
        check(ut::g_cfg.enabled == 0, "enabled=0 is read");
        check(ut::g_cfg.journal == 0, "the private table may not take a row: no new deposits");
        check(ut::g_cfg.livePages == 0 && ut::g_cfg.groupButtons == 0 && ut::g_cfg.plateLabel == 0,
              "the tab, the buttons and the label are off");
        check(ut::g_cfg.tooltipMark == 0 && ut::g_cfg.comparePopup == 0, "so is the tooltip line");
        check(ut::g_cfg.maxPerRecord == 9 && ut::g_cfg.exportGds == 1,
              "nothing else in the file was touched");
        check(logHas("enabled=0"), "the log says the mod is off and why nothing is lost");
        text = readAll(ini, &len);
        check(strstr(text, "enabled=0") != nullptr, "the file still says enabled=0");
        free(text);
        char back[64];
        _snprintf_s(back, sizeof(back), _TRUNCATE, "ini_version=%d\r\nenabled=1\r\n",
                    UT_INI_VERSION);
        writeText(ini, back);  // and back on
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

    // The run's own folder goes with it; nothing but this harness's files is in it.
    DeleteFileW(ini);
    DeleteFileW(trigger);
    DeleteFileW(triggerTxt);
    if (!RemoveDirectoryW(dir)) printf("note: %S was left behind\n", dir);

    printf("\n%s (%d failure(s))\n", g_fails ? "FAILED" : "PASSED", g_fails);
    return g_fails ? 1 : 0;
}
