// test_logview.cpp - the per-view log and the custom-game view probe, offline. No game running.
//
//   tools\build_test_logview.bat       (needs a vcvars64 shell; the .bat calls it itself)
//
// 1. THE TEE (src\ut_log.cpp, the real logger): lines written while a view's file is open are in
//    that file AND in the main log; the main log holds exactly the lines it held without a tee
//    (no header, no marker); a WARN reaches both files before the call returns; closing writes the
//    view's own last line; a second entry into the same view in one session appends; a previous
//    session's file is archived and only the three newest archives are kept; switching views
//    closes one file and opens the other; the 4 MB session cap stops the view's file, not the
//    main log.
// 2. THE PROBE (src\ut_viewprobe.h) over the real mods folder (UNIQUETAB_TEST_DBMODS_DIR, read
//    only) and scratch custom games this test writes: the Crucible (survivalmode) gets no view,
//    Reign of Terror and DeathGrimDawn do; a scratch game that adds one unique and one that
//    overrides a base unique get a view although they leave the caravan alone; junk, common,
//    BLANK and LootRandomizer records do not.
// 3. THE PROBE AGAINST THE REAL RULE (gen::collectItems over the view, UNIQUETAB_TEST_GAME_DIR):
//    every record a game's layer wins that the catalogue collects is one the probe admits, so a
//    "no" never hides a collectible record; for the Crucible the rule collects none of its records.
// 4. THE DEAD TAB AFTER A VIEW SWITCH (src\ut_hudlife.h, pure; replayed from a recorded session):
//    basemods -> Reign of Terror (group 28 "Other" on screen) -> basemods -> DeathGrimDawn ->
//    basemods, replayed on the box-life rules. Without the drop at the HUD's death the view change
//    to basemods (24 groups) clamps the wanted group to -1 and the tick re-points the 160 boxes of
//    the destroyed Reign of Terror HUD - the fault that took the tab off for the session. With it
//    no tick touches a box between worlds, every next HUD build is live again, and a world that
//    starts with the tab off says why in one WARN. The wiring is checked in the sources too.
// Extra custom-game folders (e.g. a test arena) can be listed in UNIQUETAB_TEST_EXTRA_MODS,
// separated by ';' - each is probed and cross-checked the same way.
// Exit code 0 = all of it held.
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "gen/arz_reader.h"
#include "gen/arz_writer.h"
#include "gen/catalogue_gen.h"
#include "gen/ui_geometry.h"
#include "ut_hudlife.h"
#include "ut_log.h"
#include "ut_viewprobe.h"

static int g_fails = 0;
static void check(bool ok, const char* what) {
    printf("  %-66s %s\n", what, ok ? "OK" : "FAIL");
    if (!ok) ++g_fails;
}

static std::string readFile(const wchar_t* p) {
    std::string out;
    HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return out;
    char buf[65536];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got) out.append(buf, got);
    CloseHandle(h);
    return out;
}

static void writeFile(const wchar_t* p, const void* data, size_t n, bool append = false) {
    HANDLE h = CreateFileW(p, append ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, data, (DWORD)n, &w, nullptr);
    CloseHandle(h);
}

// The "mark-NNN" tokens of a log, in order.
static std::vector<std::string> marks(const std::string& text) {
    std::vector<std::string> out;
    for (size_t at = text.find("mark-"); at != std::string::npos; at = text.find("mark-", at + 1))
        out.push_back(text.substr(at, 8));
    return out;
}

static std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (const std::string& x : v) s += x + " ";
    return s;
}

static int countArchives(const wchar_t* dir) {
    wchar_t pat[MAX_PATH];
    _snwprintf_s(pat, _TRUNCATE, L"%s\\uniquetab-*.log", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do ++n; while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

static bool archiveHolds(const wchar_t* dir, const char* needle) {
    wchar_t pat[MAX_PATH];
    _snwprintf_s(pat, _TRUNCATE, L"%s\\uniquetab-*.log", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        wchar_t p[MAX_PATH];
        _snwprintf_s(p, _TRUNCATE, L"%s\\%s", dir, fd.cFileName);
        if (readFile(p).find(needle) != std::string::npos) found = true;
    } while (!found && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

// ---- 1. the tee ----------------------------------------------------------------------------------
static void testTee(const wchar_t* root) {
    printf("1. the per-view log\n");
    wchar_t mainLog[MAX_PATH], dirA[MAX_PATH], dirB[MAX_PATH], logA[MAX_PATH], logB[MAX_PATH];
    _snwprintf_s(mainLog, _TRUNCATE, L"%s\\uniquetab.log", root);
    _snwprintf_s(dirA, _TRUNCATE, L"%s\\views\\custom-alpha", root);
    _snwprintf_s(dirB, _TRUNCATE, L"%s\\views\\custom-beta", root);
    _snwprintf_s(logA, _TRUNCATE, L"%s\\uniquetab.log", dirA);
    _snwprintf_s(logB, _TRUNCATE, L"%s\\uniquetab.log", dirB);

    // a previous session's view log and five old archives beside it
    CreateDirectoryW((std::wstring(root) + L"\\views").c_str(), nullptr);
    CreateDirectoryW(dirA, nullptr);
    const char old[] = "previous session of alpha\r\n";
    writeFile(logA, old, sizeof(old) - 1);
    for (int i = 1; i <= 5; ++i) {
        wchar_t p[MAX_PATH];
        _snwprintf_s(p, _TRUNCATE, L"%s\\uniquetab-2020010%d-000000.log", dirA, i);
        writeFile(p, "x\r\n", 3);
    }

    check(ut::logInit(mainLog), "the main log opens");
    ut::logSetLevel("info");
    ut::logI("mark-001 before any view");
    check(ut::logTeeOpen(logA, "custom-alpha"), "the view's file opens");
    wchar_t tp[MAX_PATH];
    ut::logTeePath(tp, MAX_PATH);
    check(!_wcsicmp(tp, logA), "logTeePath names it");
    ut::logI("mark-002 in alpha");
    ut::logD("mark-099 a debug line below the threshold");
    ut::logW("mark-003 a warning in alpha");
    {
        // no flush: a WARN is on both disks before logW returns
        const std::string a = readFile(logA), m = readFile(mainLog);
        check(a.find("mark-003") != std::string::npos && m.find("mark-003") != std::string::npos,
              "a WARN reaches both files at once");
    }
    ut::logI("mark-004 still alpha");
    ut::logTeeClose();
    ut::logI("mark-005 after alpha");
    ut::logFlush();
    std::string a = readFile(logA);
    std::string m = readFile(mainLog);
    check(join(marks(a)) == "mark-002 mark-003 mark-004 ", "the view's file holds the view's lines only");
    check(a.find("==== the custom-alpha view's log, opened by pid") == 0 ||
              a.find("] ==== the custom-alpha view's log, opened by pid") != std::string::npos,
          "it starts with its own header");
    check(a.find("previous log archived") != std::string::npos, "which says the previous session was archived");
    check(a.find("==== the custom-alpha view is left - the main log goes on ====") != std::string::npos,
          "and ends with its own last line");
    check(join(marks(m)) == "mark-001 mark-002 mark-003 mark-004 mark-005 ",
          "the main log holds every line, in order");
    check(m.find("view's log, opened") == std::string::npos && m.find("view is left") == std::string::npos,
          "and nothing of the tee's own (unchanged in content)");
    check(a.find("mark-099") == std::string::npos && m.find("mark-099") == std::string::npos,
          "the level threshold applies to both");
    check(countArchives(dirA) == 3 && archiveHolds(dirA, "previous session of alpha"),
          "rotation: the previous session archived, three archives kept");

    // the same view again in this session: appended, not archived
    check(ut::logTeeOpen(logA, "custom-alpha"), "re-entering the view");
    ut::logI("mark-006 alpha again");
    // straight to another view: alpha is closed, beta opened
    check(ut::logTeeOpen(logB, "custom-beta"), "switching to another view");
    ut::logI("mark-007 in beta");
    ut::logTeeClose();
    ut::logFlush();
    a = readFile(logA);
    const std::string b = readFile(logB);
    m = readFile(mainLog);
    check(join(marks(a)) == "mark-002 mark-003 mark-004 mark-006 " &&
              a.find("appended to: the view was entered before in this session") != std::string::npos,
          "the second entry appended to the first one's file");
    check(countArchives(dirA) == 3, "and archived nothing");
    check(join(marks(b)) == "mark-007 " && b.find("fresh file") != std::string::npos,
          "beta's file: its own line, a fresh file");
    size_t left = 0;
    for (size_t at = a.find("view is left"); at != std::string::npos; at = a.find("view is left", at + 1)) ++left;
    check(left == 2, "alpha was left twice: by the close and by the switch");
    check(join(marks(m)) == "mark-001 mark-002 mark-003 mark-004 mark-005 mark-006 mark-007 ",
          "the main log still holds every line");

    // the 4 MB session cap stops the view's file, not the main log
    std::string big(4 * 1024 * 1024, 'x');
    writeFile(logB, big.data(), big.size(), true);
    check(ut::logTeeOpen(logB, "custom-beta"), "beta again with 4 MB already in its file");
    ut::logI("mark-008 past the cap");
    ut::logI("mark-009 past the cap");
    ut::logTeeClose();
    ut::logFlush();
    const std::string b2 = readFile(logB);
    m = readFile(mainLog);
    check(b2.find("mark-008") == std::string::npos && b2.find("this view's file writes nothing more") != std::string::npos,
          "the view's file says it is capped and takes no more lines");
    check(m.find("mark-008") != std::string::npos && m.find("mark-009") != std::string::npos,
          "the main log goes on");
    check(b2.rfind("view is left") > b2.rfind("writes nothing more"), "the closing line is still written");
    check(!ut::logTeeOpen(L"", "x") && !ut::logTeeOpen(nullptr, "x"), "an empty path opens nothing");
    ut::logShutdown();
}

// ---- 2./3. the probe ---------------------------------------------------------------------------
struct Rec {
    std::string path, cls, cla, tag, desc;
};

static bool writeMod(const std::wstring& dir, const char* leaf, const std::vector<Rec>& recs) {
    const std::wstring wleaf(leaf, leaf + strlen(leaf));
    CreateDirectoryW(dir.c_str(), nullptr);
    CreateDirectoryW((dir + L"\\database").c_str(), nullptr);
    CreateDirectoryW((dir + L"\\resources").c_str(), nullptr);
    gen::ArzWriter w;
    for (const Rec& r : recs) {
        std::vector<gen::ArzOutField> f;
        f.push_back(gen::outStr("Class", r.cls));
        if (!r.cla.empty()) f.push_back(gen::outStr("itemClassification", r.cla));
        if (!r.tag.empty()) f.push_back(gen::outStr("itemNameTag", r.tag));
        if (!r.desc.empty()) f.push_back(gen::outStr("FileDescription", r.desc));
        f.push_back(gen::outInt("levelRequirement", 50));
        w.add(r.path, f);
    }
    const std::vector<std::uint8_t> bytes = w.build();
    writeFile((dir + L"\\database\\" + wleaf + L".arz").c_str(), bytes.data(), bytes.size());
    return !bytes.empty();
}

static std::string narrow(const std::wstring& w) {
    char buf[MAX_PATH * 2];
    WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, buf, sizeof(buf), nullptr, nullptr);
    return buf;
}

// The probe over one custom-game folder; with a game folder, the cross-check against the rule.
// Returns the probe's verdict; `ruleMod` gets how many records the game's layer wins that the
// real rule collects (-1 without a game folder).
static bool probeOne(const std::string& modDir, const char* gameDir, ut::UtViewProbe& quick,
                     ut::UtViewProbe& census, long* ruleMod, int* missed) {
    *ruleMod = -1;
    *missed = 0;
    gen::ModView v;
    std::string err;
    if (!v.loadCustom(modDir, &err)) {
        printf("    %s: %s\n", modDir.c_str(), err.c_str());
        return false;
    }
    const ULONGLONG t0 = GetTickCount64();
    ut::utProbeView(v, quick);
    const unsigned ms = (unsigned)(GetTickCount64() - t0);
    ut::utProbeView(v, census, true);
    printf("    %-60s caravan=%d items=%zu (census %zu of %zu records) first=\"%s\" %u ms\n",
           modDir.c_str(), quick.caravan ? 1 : 0, quick.items, census.items, census.records,
           quick.sample.c_str(), ms);
    if (!gameDir) return quick.needsView();
    gen::GameData g;
    if (!g.load(gameDir, "EN", &err, &v)) {
        printf("    the game data does not load: %s\n", err.c_str());
        ++g_fails;
        return quick.needsView();
    }
    std::vector<gen::CatalogueItem> items;
    std::vector<std::string> warnings;
    if (!gen::collectItems(g, items, warnings, &err)) {
        printf("    collectItems failed: %s\n", err.c_str());
        ++g_fails;
        return quick.needsView();
    }
    long won = 0, wonBp = 0;
    const std::vector<gen::ArzArchive>& layers = v.arzLayers();
    for (const gen::CatalogueItem& it : items) {
        for (size_t li = layers.size(); li-- > 0;) {
            const gen::ArzEntry* e = layers[li].entry(it.record);
            if (!e) continue;
            ++won;
            if (it.cls == "ItemArtifactFormula") ++wonBp;
            gen::ArzRecord rec;
            const bool ok = layers[li].decode(*e, rec) &&
                            ut::utProbeItemRecord(e->key, rec.str("Class"), rec.str("itemClassification"),
                                                  rec.str("FileDescription"));
            if (!ok && (*missed)++ < 5) printf("    the probe misses %s\n", it.record.c_str());
            break;
        }
    }
    *ruleMod = won;
    printf("    the rule collects %zu record(s), %ld of them won by this game's layer (%ld blueprints); the probe "
           "misses %d\n", items.size(), won, wonBp, *missed);
    // collect_mi=1: the rule admits the monster infrequents too, and the probe with the flag must
    // admit every one this game's layer wins (the same gen::isMiCandidate)
    {
        std::vector<gen::CatalogueItem> withMi;
        std::vector<std::string> w2;
        if (!gen::collectItems(g, withMi, w2, &err, true)) {
            printf("    collectItems (collect_mi=1) failed: %s\n", err.c_str());
            ++g_fails;
            return quick.needsView();
        }
        long wonMi = 0;
        int missedMi = 0;
        for (const gen::CatalogueItem& it : withMi) {
            if (!it.isMi) continue;
            for (size_t li = layers.size(); li-- > 0;) {
                const gen::ArzEntry* e = layers[li].entry(it.record);
                if (!e) continue;
                ++wonMi;
                gen::ArzRecord rec;
                const bool ok = layers[li].decode(*e, rec) &&
                                ut::utProbeItemRecord(e->key, rec.str("Class"), rec.str("itemClassification"),
                                                      rec.str("FileDescription"), true);
                if (!ok && missedMi++ < 5) printf("    the probe (collect_mi=1) misses %s\n", it.record.c_str());
                break;
            }
        }
        ut::UtViewProbe miCensus;
        ut::utProbeView(v, miCensus, true, true);
        printf("    collect_mi=1: the rule collects %zu record(s), %ld monster infrequent(s) won by this game's layer; "
               "the probe admits %zu record(s) and misses %d\n", withMi.size(), wonMi, miCensus.items, missedMi);
        *missed += missedMi;
    }
    return quick.needsView();
}

static void testProbe(const wchar_t* root) {
    printf("\n2. the view probe\n");
    const char* gameDir = getenv("UNIQUETAB_TEST_GAME_DIR");
    if (gameDir && GetFileAttributesA((std::string(gameDir) + "\\database\\database.arz").c_str()) ==
                       INVALID_FILE_ATTRIBUTES)
        gameDir = nullptr;
    if (!gameDir) printf("    UNIQUETAB_TEST_GAME_DIR not set: the cross-check against the rule is SKIPPED\n");

    // the predicate itself
    check(ut::utProbeItemRecord("records/items/x/ring.dbr", "ArmorJewelry_Ring", "Legendary", ""),
          "a Legendary ring is an item record");
    check(ut::utProbeItemRecord("records/mod/relic.dbr", "ItemArtifact", "Common", "") &&
              ut::utProbeItemRecord("records/mod/aug.dbr", "ItemEnchantment", "Rare", "") &&
              ut::utProbeItemRecord("records/mod/bow.dbr", "WeaponMagical_Staff", "Epic", ""),
          "a relic, an augment, an Epic of an unknown class");
    check(!ut::utProbeItemRecord("records/items/x/s.dbr", "WeaponMelee_Sword", "Rare", "") &&
              !ut::utProbeItemRecord("records/items/x/s.dbr", "WeaponMelee_Sword", "Common", ""),
          "a Rare or Common sword is not");
    check(!ut::utProbeItemRecord("records/creatures/x/s.dbr", "WeaponMelee_Sword", "Legendary", "") &&
              !ut::utProbeItemRecord("records/items/enemygear/s.dbr", "WeaponMelee_Sword", "Legendary", "") &&
              !ut::utProbeItemRecord("records/sandbox/s.dbr", "ItemArtifact", "Legendary", ""),
          "nor anything on the junk list");
    check(!ut::utProbeItemRecord("records/items/x/l.dbr", "LootRandomizer", "Legendary", "") &&
              !ut::utProbeItemRecord("records/items/x/l.dbr", "", "Legendary", "") &&
              !ut::utProbeItemRecord("records/items/x/b.dbr", "ArmorJewelry_Ring", "Legendary", "a blank ring"),
          "nor an affix, a class-less record or a BLANK one");
    check(ut::utProbeItemRecord("records/mod/bp_m.dbr", "ItemArtifactFormula", "Magical", "") &&
              ut::utProbeItemRecord("records/mod/bp_r.dbr", "ItemArtifactFormula", "Rare", "") &&
              ut::utProbeItemRecord("records/mod/bp_c.dbr", "ItemArtifactFormula", "Common", "") &&
              ut::utProbeItemRecord("records/mod/bp_l.dbr", "ItemArtifactFormula", "Legendary", ""),
          "a blueprint at every classification (the rule collects every ItemArtifactFormula)");
    // collect_mi=1: a Rare item record of an equipment class (the 23 slots and the per-class
    // groups' staff / hunting spear) is collectible - the faction-store and the crafted rares
    // included; the Rare record of any other class is not (gen::isMiCandidate)
    check(ut::utProbeItemRecord("records/items/x/s.dbr", "WeaponMelee_Sword", "Rare", "", true) &&
              ut::utProbeItemRecord("records/mod/h.dbr", "ArmorProtective_Head", "Rare", "mi helm", true),
          "with collect_mi a Rare sword / helm is an item record");
    check(ut::utProbeItemRecord("records/mod/s.dbr", "WeaponMagical_Staff", "Rare", "", true) &&
              ut::utProbeItemRecord("records/mod/j.dbr", "WeaponHunting_Spear", "Rare", "", true) &&
              !ut::utProbeItemRecord("records/mod/s.dbr", "WeaponMagical_Staff", "Rare", "") &&
              !ut::utProbeItemRecord("records/mod/j.dbr", "WeaponHunting_Spear", "Rare", ""),
          "a Rare staff / hunting spear (Reign of Terror's bows and javelins) is one with collect_mi only");
    check(ut::utProbeItemRecord("records/items/faction/f001_head.dbr", "ArmorProtective_Head", "Rare", "", true) &&
              ut::utProbeItemRecord("records/mod/owlheart/faction/x.dbr", "WeaponArmor_Shield", "Rare", "", true) &&
              ut::utProbeItemRecord("records/items/gearweapons/b001_gun1h.dbr", "WeaponHunting_Ranged1h", "Rare",
                                    "Crafted pistol", true) &&
              !ut::utProbeItemRecord("records/items/faction/f001_head.dbr", "ArmorProtective_Head", "Rare", "") &&
              !ut::utProbeItemRecord("records/mod/owlheart/faction/x.dbr", "WeaponArmor_Shield", "Rare", "") &&
              !ut::utProbeItemRecord("records/items/gearweapons/b001_gun1h.dbr", "WeaponHunting_Ranged1h", "Rare",
                                     "Crafted pistol"),
          "a faction-store and a crafted rare are item records with collect_mi (every Rare), none without");
    check(!ut::utProbeItemRecord("records/mod/s.dbr", "ItemFactionBooster", "Rare", "", true) &&
              !ut::utProbeItemRecord("records/mod/q.dbr", "QuestItem", "Rare", "", true) &&
              !ut::utProbeItemRecord("records/creatures/x/s.dbr", "WeaponMelee_Sword", "Rare", "", true) &&
              !ut::utProbeItemRecord("records/items/x/b.dbr", "ArmorJewelry_Ring", "Rare", "BLANK RARE RING", true),
          "but not a Rare of another class, junk or BLANK");
    check(!ut::utProbeItemRecord("records/mod/f.dbr", "AscendantAltarFormula", "Epic", "") &&
              !ut::utProbeItemRecord("records/mod/f.dbr", "ItemAscensionFormula", "Legendary", "") &&
              !ut::utProbeItemRecord("records/mod/f.dbr", "ItemRandomSetFormula", "Epic", "") &&
              !ut::utProbeItemRecord("records/mod/f.dbr", "ItemRerollFormula", "Epic", "") &&
              !ut::utProbeItemRecord("records/mod/f.dbr", "ItemSetFormula", "Legendary", ""),
          "nor the five *Formula classes that are not recipes, at any classification");

    // the real mods folder
    const char* modsDir = getenv("UNIQUETAB_TEST_DBMODS_DIR");
    std::vector<std::string> folders;
    if (modsDir && *modsDir) {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((std::string(modsDir) + "\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
                folders.push_back(std::string(modsDir) + "\\" + fd.cFileName);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    } else {
        printf("    UNIQUETAB_TEST_DBMODS_DIR not set: the real mods folder is SKIPPED\n");
    }
    if (const char* extra = getenv("UNIQUETAB_TEST_EXTRA_MODS")) {
        std::string list(extra);
        size_t at = 0;
        while (at <= list.size()) {
            size_t semi = list.find(';', at);
            if (semi == std::string::npos) semi = list.size();
            if (semi > at) folders.push_back(list.substr(at, semi - at));
            at = semi + 1;
        }
    }
    bool sawCrucible = false, sawRot = false, sawDgd = false;
    for (const std::string& f : folders) {
        std::string leaf = f.substr(f.find_last_of("\\/") + 1);
        for (char& c : leaf) c = (char)tolower((unsigned char)c);
        if (GetFileAttributesA((f + "\\database").c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        ut::UtViewProbe q, c;
        long ruleMod = -1;
        int missed = 0;
        const bool yes = probeOne(f, gameDir, q, c, &ruleMod, &missed);
        char what[160];
        if (gameDir) {
            _snprintf_s(what, sizeof(what), _TRUNCATE, "%s: the probe admits every record the rule collects from it", leaf.c_str());
            check(missed == 0, what);
            _snprintf_s(what, sizeof(what), _TRUNCATE, "%s: a no means the rule collects none of its records", leaf.c_str());
            check(yes || ruleMod == 0, what);
        }
        if (leaf == "survivalmode") {
            sawCrucible = true;
            check(!yes && !q.caravan && c.items == 0, "the Crucible gets no view (its weapons are Common/Rare)");
            if (gameDir) check(ruleMod == 0, "and the real rule collects none of its records");
        } else if (leaf == "reignofterror") {
            sawRot = true;
            check(yes && q.caravan && c.items > 1000, "Reign of Terror gets a view (caravan; >1000 item records)");
        } else if (leaf == "deathgrimdawn") {
            sawDgd = true;
            check(yes && c.items > 1000, "DeathGrimDawn gets a view (>1000 item records)");
        } else {
            printf("    %s: %s\n", leaf.c_str(), yes ? "gets a view" : "gets no view");
        }
    }
    if (modsDir && *modsDir) {
        check(sawCrucible, "the mods folder holds the Crucible (survivalmode)");
        if (!sawRot || !sawDgd) printf("    (Reign of Terror / DeathGrimDawn not installed: not checked)\n");
    }

    // scratch custom games
    printf("\n3. scratch custom games\n");
    const std::wstring mods = std::wstring(root) + L"\\mods";
    CreateDirectoryW(mods.c_str(), nullptr);
    // a real base unique's name tag, so the rule (which needs the tag in the text) can collect
    // the scratch records too; any tag will do for the probe
    std::string baseRing = "records/items/gearaccessories/rings/a001_ring.dbr", baseTag = "tagScratchRing";
    if (gameDir) {
        gen::ArzDatabase db;
        std::string err;
        if (db.loadGame(gameDir, &err)) {
            gen::ArzRecord r;
            for (const auto& w : db.winners()) {
                const gen::ArzEntry& e = *w.second;
                if (e.key.compare(0, 14, "records/items/") != 0) continue;
                if (!db.archives()[w.first].decode(e, r)) continue;
                if (r.str("Class") != "ArmorJewelry_Ring" || r.str("itemClassification") != "Legendary") continue;
                if (r.str("itemNameTag").empty()) continue;
                baseRing = e.name;
                baseTag = r.str("itemNameTag");
                break;
            }
        }
        printf("    base unique used: %s (%s)\n", baseRing.c_str(), baseTag.c_str());
    }
    struct Scratch {
        const char* leaf;
        std::vector<Rec> recs;
        bool wantView;
        const char* what;
    };
    const std::vector<Scratch> scratch = {
        {"scratchadd",
         {{"records/scratch/items/a001_ring.dbr", "ArmorJewelry_Ring", "Legendary", baseTag, ""},
          {"records/creatures/scratch/m001.dbr", "Monster", "", "", ""}},
         true, "a game that ADDS one unique (and a monster) gets a view"},
        {"scratchoverride",
         {{baseRing, "ArmorJewelry_Ring", "Legendary", baseTag, ""}},
         true, "a game that OVERRIDES a base unique gets a view"},
        {"scratchnone",
         {{"records/items/scratch/s001.dbr", "WeaponMelee_Sword", "Common", baseTag, ""},
          {"records/items/scratch/s002.dbr", "WeaponMelee_Sword", "Rare", baseTag, ""},
          {"records/items/enemygear/s003.dbr", "WeaponMelee_Sword", "Legendary", baseTag, ""},
          {"records/creatures/scratch/s004.dbr", "ArmorJewelry_Ring", "Legendary", baseTag, ""},
          {"records/items/scratch/s005.dbr", "ArmorJewelry_Ring", "Legendary", baseTag, "BLANK"},
          {"records/items/scratch/s006.dbr", "LootRandomizer", "Legendary", baseTag, ""},
          {"records/items/scratch/p001.dbr", "OneShot_PotionHealth", "Common", "", ""}},
         false, "commons, rares, junk, BLANK, an affix and a potion get no view"},
    };
    for (const Scratch& s : scratch) {
        const std::wstring dir = mods + L"\\" + std::wstring(s.leaf, s.leaf + strlen(s.leaf));
        check(writeMod(dir, s.leaf, s.recs), "  the scratch archive is written");
        ut::UtViewProbe q, c;
        long ruleMod = -1;
        int missed = 0;
        const bool yes = probeOne(narrow(dir), gameDir, q, c, &ruleMod, &missed);
        check(yes == s.wantView && !q.caravan, s.what);
        if (gameDir) {
            check(missed == 0 && (s.wantView ? ruleMod == 1 : ruleMod == 0),
                  s.wantView ? "  the rule collects the record, the probe admits it"
                             : "  the rule collects none of them either");
        }
    }
    // collect_mi=1: the game whose only collectible-looking record is a Rare sword (an MI) gets a
    // view with the flag on - and still none with it off (above)
    {
        gen::ModView v;
        std::string err;
        ut::UtViewProbe p;
        const bool loaded = v.loadCustom(narrow(mods + L"\\scratchnone"), &err);
        if (loaded) ut::utProbeView(v, p, false, true);
        check(loaded && p.needsView() && p.items == 1 &&
                  p.sample.find("s002.dbr") != std::string::npos,
              "with collect_mi=1 the Rare sword of scratchnone is a monster infrequent: it gets a view");
    }
}

// ---- 4. the dead tab after a view switch -------------------------------------------------------
// One world's HUD build in the model: the frame record substituted, ReagentWindow::Load appends
// the frame's boxes, the capture closes (hk_BoxLoad: "the frame is complete").
static void hudBuild(ut::UtHudBoxes* b) {
    ut::utBoxesBeginCapture(b);
    b->captured = b->nmax;
    b->capturing = false;
}

// liveTick's relayout in the model: what applyLayout leaves shown.
static bool tick(ut::UtHudBoxes* b, int* wantGroup, int wantRow, int* curRow, bool forced) {
    if (!ut::utTickRelayouts(*b, true, *wantGroup, wantRow, *curRow, forced)) return false;
    b->curGroup = *wantGroup;
    *curRow = wantRow;
    return true;
}

static std::string srcText(const char* leaf) {
    // __FILE__ is ...\tools\test_logview.cpp as build_test_logview.bat hands it to cl
    std::string dir = __FILE__;
    const size_t cut = dir.find_last_of("\\/");
    dir = cut == std::string::npos ? std::string(".") : dir.substr(0, cut);
    const std::string path = dir + "\\..\\src\\" + leaf;
    std::wstring w(path.begin(), path.end());
    return readFile(w.c_str());
}

// The text between `from` and the next `to` after it ("" when either is missing).
static std::string between(const std::string& s, const char* from, const char* to) {
    const size_t a = s.find(from);
    if (a == std::string::npos) return std::string();
    const size_t b = s.find(to, a + strlen(from));
    return b == std::string::npos ? std::string() : s.substr(a, b - a);
}

static void testHudLife() {
    using namespace ut;
    printf("\n4. the dead tab after a view switch (src\\ut_hudlife.h, a recorded session)\n");
    const int kBase = 24, kRot = 29, kDgd = 28;
    for (int fixed = 0; fixed < 2; ++fixed) {
        printf("  -- %s\n", fixed ? "with the drop at the HUD's death (the fix)"
                                  : "without it (the recorded session)");
        UtHudBoxes b = {0, 160, false, -2};
        int want = 16, curRow = 0;   // "live: the tab is armed - restored group=16 row=0"
        const int row = 0;
        bool deadTouched = false;    // a tick re-pointed a box of a destroyed HUD
        // world 1, the main campaign
        hudBuild(&b);
        check(tick(&b, &want, row, &curRow, false) && b.curGroup == 16,
              "world 1 (basemods): the first tick lays out the restored group 16");
        // ExitPlayingMode -> the ROT database: the HUD dies, the view changes to 29 groups
        if (fixed) utBoxesHudGone(&b);
        want = utWantedAfterTable(want, kRot);
        if (fixed) utBoxesHudGone(&b);   // liveReloadView drops them too
        if (tick(&b, &want, row, &curRow, false)) deadTouched = true;   // group 16 is in range
        // world 2, Reign of Terror: built, then [OTH] clicked
        hudBuild(&b);
        check(tick(&b, &want, row, &curRow, false) && b.curGroup == 16,
              "world 2 (ROT): its own HUD build lays out group 16 again");
        want = 28;
        check(tick(&b, &want, row, &curRow, false) && b.curGroup == 28,
              "world 2: [OTH] clicked -> group 28 \"Other\" on screen");
        // ExitPlayingMode at 18:56:03.779, then the basemods view (24 groups) at 18:56:04.322
        if (fixed) utBoxesHudGone(&b);
        want = utWantedAfterTable(want, kBase);
        check(want == -1, "the basemods table has no group 28: the wanted group is -1");
        if (fixed) utBoxesHudGone(&b);
        const bool menuTick = tick(&b, &want, row, &curRow, false);
        if (menuTick) deadTouched = true;
        if (fixed) {
            check(!menuTick, "the menu tick after the view change re-points nothing");
        } else {
            check(menuTick, "the menu tick re-points the 160 boxes of the dead ROT HUD (the fault)");
        }
        // the main-menu basemods world and DGD: the view changes again (28 groups)
        if (fixed) utBoxesHudGone(&b);
        want = utWantedAfterTable(want, kDgd);
        if (tick(&b, &want, row, &curRow, true)) deadTouched = true;
        // the fault took the tab off for the session: the model's verdict at DGD's world start
        UtTabFacts f = {};
        f.livePages = true;
        f.disabled = deadTouched;   // applyLayout's fault -> disable() for the session
        f.disabledWorld = false;
        f.dataOk = f.codeOk = f.frameNamed = true;
        f.unservable = false;
        const int v = utTabVerdict(f);
        char line[640];
        const int n = utTabOffLine(line, sizeof(line), v, "basemods+custom-deathgrimdawn",
                                   "an engine call faulted while restoring the vanilla layout");
        if (fixed) {
            check(!deadTouched, "no tick touched a box of a destroyed HUD in the whole session");
            check(v == kUtTabLive && n == 0, "DGD's world start: the tab is live, no WARN");
            hudBuild(&b);
            check(tick(&b, &want, row, &curRow, false) && b.curGroup == -1,
                  "world 3 (DGD): its HUD build lays the page out (the vanilla page, group -1)");
            want = 5;
            check(tick(&b, &want, row, &curRow, false) && b.curGroup == 5,
                  "world 3: a category button pages a group (the tab is alive)");
            utBoxesHudGone(&b);
            want = utWantedAfterTable(want, kBase);
            check(want == 5 && !tick(&b, &want, row, &curRow, true),
                  "back to basemods: group 5 kept as wanted, nothing touched in the menu");
            hudBuild(&b);
            check(tick(&b, &want, row, &curRow, false) && b.curGroup == 5,
                  "world 4 (the main campaign again): group 5 on screen");
        } else {
            check(deadTouched && v == kUtTabOffSession,
                  "the dead-box relayout leaves the tab off for the session");
            check(n > 0 && strstr(line, "live: the tab is off in this world (the "
                                        "basemods+custom-deathgrimdawn view) because an engine "
                                        "call faulted while restoring the vanilla layout") == line,
                  "DGD's world start now says so: the WARN names the view and the reason");
            check(strstr(line, "until the game is restarted") != nullptr,
                  "  ... and that only a restart re-arms it");
            printf("    WARN text: %s\n", line);
        }
    }
    // any other relayout between worlds - a forced repaint of the group "on screen" - is refused
    // the same way once the HUD is gone, and was not before
    {
        UtHudBoxes b = {160, 160, false, 28};
        int want = 28, curRow = 0;
        check(tick(&b, &want, 0, &curRow, true),
              "a forced repaint between worlds re-points the dead boxes too");
        b.curGroup = 28;
        utBoxesHudGone(&b);
        check(!tick(&b, &want, 0, &curRow, true) && b.captured == 0 && !b.capturing &&
                  b.curGroup == -2,
              "after the HUD's death: nothing to re-point, nothing shown");
        UtHudBoxes c = {37, 160, true, -2};   // the HUD died in the middle of its own build
        utBoxesHudGone(&c);
        check(!c.capturing && c.captured == 0 && !utBoxesUsable(c, true),
              "a capture still open at the HUD's death is closed and empty");
        hudBuild(&c);
        check(utBoxesUsable(c, true) && !utBoxesUsable(c, false),
              "the next build is usable again (and only with the vtable checked)");
    }
    // the verdict, case by case
    {
        UtTabFacts f = {};
        f.livePages = true;
        f.dataOk = f.codeOk = f.frameNamed = true;
        char line[640];
        check(utTabVerdict(f) == kUtTabLive &&
                  utTabOffLine(line, sizeof(line), utTabVerdict(f), "basemods", "armed") == 0 &&
                  line[0] == 0,
              "everything in place: live, no line");
        UtTabFacts g = f;
        g.livePages = false;
        g.disabled = true;
        check(utTabVerdict(g) == kUtTabOffByChoice && !utTabWarns(utTabVerdict(g)),
              "live_pages=0 is the user's choice: no WARN");
        g = f;
        g.disabled = true;
        g.disabledWorld = true;
        check(utTabVerdict(g) == kUtTabOffWorld &&
                  utTabOffLine(line, sizeof(line), utTabVerdict(g), "basemods",
                               "the box vtable does not match the three located functions") > 0 &&
                  strstr(line, "because the box vtable does not match") != nullptr &&
                  strstr(line, "the next world gets its own answer") != nullptr,
              "a stop for this world only: named, and the next world re-asks");
        g = f;
        g.dataOk = false;
        check(utTabVerdict(g) == kUtTabOffNoTable &&
                  utTabOffLine(line, sizeof(line), utTabVerdict(g), "basemods", "") > 0 &&
                  strstr(line, "uniq-groups.txt is missing or unreadable") != nullptr,
              "no group table: the WARN says which file");
        g = f;
        g.codeOk = false;
        check(utTabVerdict(g) == kUtTabOffNoCode &&
                  utTabOffLine(line, sizeof(line), utTabVerdict(g), "basemods",
                               "the exe signatures are not resolved yet") > 0 &&
                  strstr(line, "late scan") != nullptr,
              "the exe signatures not resolved: the WARN says the scan still retries");
        g = f;
        g.frameNamed = false;
        check(utTabVerdict(g) == kUtTabOffNoFrame &&
                  utTabOffLine(line, sizeof(line), utTabVerdict(g), "basemods", "armed") > 0 &&
                  strstr(line, "names no frame record") != nullptr,
              "no frame record named: the WARN says so");
        g = f;
        g.unservable = true;
        check(utTabVerdict(g) == kUtTabOffUnservable &&
                  utTabOffLine(line, sizeof(line), utTabVerdict(g), "basemods+custom-x",
                               "armed") > 0 &&
                  strstr(line, "cannot serve the view's page records") != nullptr,
              "a database that cannot serve the pages: the WARN says so");
        g = f;
        g.disabled = true;
        check(utTabOffLine(line, sizeof(line), utTabVerdict(g), nullptr, nullptr) > 0 &&
                  strstr(line, "(the ? view) because of an earlier error") != nullptr,
              "no view and no reason: still one readable line");
        char tiny[16];
        check(utTabOffLine(tiny, sizeof(tiny), utTabVerdict(g), "basemods", "x") == 15 &&
                  strlen(tiny) == 15,
              "a short buffer truncates, never overruns");
    }
    // the wiring: the real sources call the rules (a revert of the fix fails here)
    {
        const std::string live = srcText("ut_live.cpp");
        const std::string reagent = srcText("ut_reagent.cpp");
        const std::string hdr = srcText("ut_hudlife.h");
        check(!live.empty() && !reagent.empty() && !hdr.empty(), "the three sources are readable");
        const std::string tickBody = between(live, "void liveTick(bool gameThread) {", "\n}\n");
        check(tickBody.find("utBoxesUsable(") != std::string::npos,
              "liveTick gates every relayout on utBoxesUsable");
        const std::string reload =
            between(live, "bool liveReloadView(HMODULE selfModule) {", "\n}\n");
        check(reload.find("liveOnHudGone(") != std::string::npos &&
                  reload.find("utWantedAfterTable(") != std::string::npos,
              "liveReloadView drops the boxes and clamps through the rule");
        const std::string gone = between(live, "void liveOnHudGone(const char* why) {", "\n}\n");
        check(gone.find("utBoxesHudGone(") != std::string::npos &&
                  gone.find("g_boxes->clear()") != std::string::npos,
              "liveOnHudGone empties the list the rule empties");
        const std::string exitSeh = between(reagent, "void exitPlateSeh() {", "\n}\n");
        check(exitSeh.find("liveOnHudGone(") != std::string::npos,
              "GameEngine::ExitPlayingMode drops the boxes (exitPlateSeh)");
        const std::string mainPlayer =
            between(reagent, "void reagentOnMainPlayer(GdGameEngine* engine) {", "\n}\n");
        check(mainPlayer.find("utTabOffLine(") != std::string::npos &&
                  mainPlayer.find("logW(") != std::string::npos,
              "the world start logs the verdict as a WARN");
        size_t inc = 0;
        int includes = 0;
        bool onlyStdio = true;
        while ((inc = hdr.find("#include", inc)) != std::string::npos) {
            ++includes;
            if (hdr.compare(inc, 18, "#include <stdio.h>") != 0) onlyStdio = false;
            inc += 8;
        }
        check(includes == 1 && onlyStdio, "ut_hudlife.h is pure (stdio.h only)");
    }
}

// This run's own scratch folder, and nothing outside it.
static void removeTree(const wchar_t* dir) {
    if (!wcsstr(dir, L"ut-logview-test-")) return;
    wchar_t pat[MAX_PATH];
    _snwprintf_s(pat, _TRUNCATE, L"%s\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            wchar_t p[MAX_PATH];
            _snwprintf_s(p, _TRUNCATE, L"%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) removeTree(p);
            else DeleteFileW(p);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

int main() {
    wchar_t root[MAX_PATH];
    GetTempPathW(MAX_PATH, root);
    wchar_t leaf[64];
    _snwprintf_s(leaf, _TRUNCATE, L"ut-logview-test-%lu-%llu", GetCurrentProcessId(),
                 (unsigned long long)GetTickCount64());
    wcscat_s(root, leaf);
    if (!CreateDirectoryW(root, nullptr)) {
        printf("cannot create %S\n", root);
        return 2;
    }
    printf("scratch folder = %S\n\n", root);
    testTee(root);
    testProbe(root);
    testHudLife();
    // the scratch folder is left for a failed run's reader; a passing run removes it
    if (!g_fails) removeTree(root);
    printf("\n%s (%d failure(s))\n", g_fails ? "FAILED" : "ALL PASS", g_fails);
    return g_fails ? 1 : 0;
}
