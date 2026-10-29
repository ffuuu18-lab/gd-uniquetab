// ut_config.h - every setting the mod has, in one plain text file (uniquetab.ini, in the mod
// folder), plus one complete file of the same shape per custom game and a state file the mod owns.
//
// "key=value" per line, grouped into [sections] that exist for the reader only - a key is looked
// up by NAME, so moving a line between sections changes nothing. The file is re-read once a
// second by the worker thread. Values are plain ints written by the worker thread and read by the
// render thread; a torn read is impossible for an aligned int on x64 and a one-frame-stale value
// is harmless.
//
// ONE TABLE IS THE TRUTH. kUtCfgKeys below is the only list of keys there is: the parser walks
// it, the file the mod writes is RENDERED from it (configRender), and the test suite derives what
// it expects from it. Adding a setting means adding one row.
//
// THE SHAPE (ini_version 44). uniquetab.ini shows four keys ([general]: enabled, collect_mi,
// tooltip_mark, log_level) and the [layout] lines the in-game editor writes. 40 added collect_mi
// (the monster infrequents; default 1, a 39 file gains collect_mi=1, a 40 file keeps what it
// says). 41 moved max_per_record and rescue into the [advanced] blob as hidden rows marked
// "(untested)": a 40 file is rewritten in full, every value kept - one of the two at its default
// becomes the blob's commented line, one set to anything else (max_per_record=2, rescue=1) stays
// a LIVE line there. rescue still writes itself back to 0 when its run ends (configPersistInt),
// which comments it again. 42 made a custom
// game's own file a complete file of this same shape (PER CUSTOM GAME below); the main file only
// gains the new number. 44 added zoom_pct, the one hidden row of [layout] (a commented line under
// the section's blurb; refused outside 100..200, never clamped: kUtCfgRefuse), and [keys] (THE
// KEYS below): a 43 file is rewritten once and gains the zoom line and the seven commented [keys]
// rows. Every other
// key that is still parsed is a HIDDEN row (kUtCfgHidden): it is written as one commented line of
// the [advanced] blob at the end of the file - ";key=default<tabs>; what it does" - and a hidden
// key the user sets to a value other than its default is written LIVE in the same place (the ';'
// gone, its description kept). The persist keeps that shape: a hidden key written back at its
// default is commented again. A hidden row of another section ([keys]) is written the same way in
// its own section, on the blob's column.
//
// THE KEYS. [keys], between [layout] and [advanced], holds seven hidden text rows - rows_up,
// rows_down, button_prev, button_next, rows_wheel, button_wheel, editor_open - whose grammar,
// refusals and matching are ut_keys.h's. Each row's check is utKeyParse for its action: a value
// it refuses is ignored with one WARN per edit naming the reason, and the default stays; an empty
// value is the default. The parse then DERIVES UtConfig::keys (ut_keys.h UtKeys, a POD) from the
// seven texts under the settings lock, with the conflict pass (two identical chords of one site:
// the later row is none, one WARN naming both), so the window procedure reads plain ints
// (g_cfg.keys.rowsUp ...) and a change bumps configGeneration. MAIN FILE ONLY, like journal_dir:
// a custom game's file never renders the section, a [keys] line there is ignored with one WARN,
// and while a custom game is active g_cfg's [keys] texts and bindings are still the main file's
// (re-read within a second as ever).
//
// GEOMETRY UNITS. plate_label_size and the [layout] rectangles are RECORD px - the caravan
// window's own coordinates, which the mod multiplies by the UI scale it measures at run time. The
// rectangles are window-local and are checked against the live page where they are used
// (ut_layout.h).
//
// THE SCREEN KEYS. [layout] also holds pad_screen and label_screen = "<corner>,x,y,w,h"
// (ut_layout.h UtScreenRect: tl|tr|bl|br, then x,y,w,h in record px from that screen corner): the
// pad / the label pinned to a corner of the SCREEN instead of the page. ONE line each per
// settings file, for every caravan cover: the main uniquetab.ini's for vanilla and basemods, a
// custom game's own file's for that game (configPersistStr writes them to the active view's
// file, like every other key). Written by the in-game editor; ABSENT until set (kUtCfgWhenSet: a
// fresh file has no line, the self-check and the migration's counts leave them out). A value that does
// not parse is refused with one WARN per edit and the key stays unset; the text is kept as typed
// (trimmed) and parsed where it is used (ut_layout.h utLayoutFromSource).
//
// KEYED FAMILIES. Besides the fixed keys, a family is every key made of a fixed PREFIX and a free
// SUFFIX - `grid_438x627` is the family `grid_` with the suffix `438x627`. kUtCfgFamilies lists
// them: each names its [section], its prefix, the UtCfgFamilyStore inside UtConfig that holds its
// lines, and two checks the family's owner supplies - one for the suffix (a line whose suffix
// fails is a key this build does not have) and one for the value (a line whose value fails is
// refused with one WARN per edit and not stored). A stored line survives a version bump. Lines are
// kept in file order; a suffix given twice keeps the later value. Read one with configFamilyValue
// ("grid_438x627"), write any key - fixed or family - with configPersistStr.
//
// PER CUSTOM GAME. A custom game's view (custom-<name> and basemods+custom-<name> share it, like
// the journal) has its own mods\<name>\uniquetab.ini: a COMPLETE settings file rendered from the
// same table as the main one (configRenderView: its own [general], [layout] with its own screen
// keys and grid_ / pad_ / label_ lines, the [advanced] blob, its own ini_version), parsed the
// same way. THE SEED: the first time the view is entered the file is written with the main
// file's values then in force ([layout] lines and screen keys included; the one-shots rescue,
// journal_prune and layout_edit start at 0). NOTHING IS INHERITED afterwards: while the view is
// active that file is the whole configuration - the main file is not consulted, and a key the
// game's file does not name is the compiled default (never the main file's value). The one
// exceptions are journal_dir, read once at start-up from the main file before any custom game is
// known (it is where the mods\<name>\ journal folders live): a game's file shows it as a
// commented line saying so, and a live journal_dir line there is refused with one WARN; and the
// [keys] rows, which a game's file does not show at all (a line there: one WARN, ignored) and
// which stay the main file's while the game is active. Every
// key written back (the one-shots, the editor's SAVE) goes to the ACTIVE view's file
// (configPersistStr decides), journal_dir always to the main one. Both files are polled once a
// second: the game's is the one in force, the main file's stamp is still tracked so the vanilla
// and basemods views have its edits on return. A game file deleted while the game runs is
// written again from the seed. A game file from an older build (ini_version <= 41: the four
// per-view keys under [game] and a [layout] whose `default` value hid a main line) is migrated
// once: its lines kept (a `default` line drops the main line from the seed), every other value
// copied from the main file, rewritten whole - one INFO line per game file. The old
// [view:<name>] blocks of uniquetab.ini are migrated into these files on the 38 -> 39 bump; a
// block met afterwards is ignored with one WARN and dropped by the next rewrite.
//
// THE STATE FILE. uniq_group, uniq_row and owned_only are not settings: the mod writes them to
// uniquetab.state (beside uniquetab.ini for the start-up view, in the view's own folder
// otherwise - mods\<name>\, and mods\<name>\basemods\ for basemods+custom-<name> -, so a group
// index never crosses views) through configPersistInt / configPersistStr, which route those three
// keys there. The 38 -> 39 bump's copies of the keys seed the main state file once (when it is
// absent); in a 39 uniquetab.ini they are ignored with a WARN.
//
// DROPPED, MERGED, DERIVED (39). The feature switches, the timers, pad_y/pad_h/pad_gap,
// tooltip_class_*, text_language and live_cache_max are no longer keys: a line naming one is
// dropped by the bump (one INFO naming it) and is "a setting this build does not have" (WARN) in a
// 39 file. log_flush_each_line and tooltip_text_yes/no are dropped the same way, with one WARN
// naming their new home (log_level=trace; tooltip.yes/no under [tab] of uniquetab-buttons.ini) when the value
// was not the old default.
#pragma once

#include <stddef.h>

#include <string>

#include "ut_keys.h"

// Bumped whenever the key set changes. A file written by an older build is MERGED, never reset:
// every key that still exists keeps the user's value, new keys take their default, keys this
// build no longer has are dropped with one log line naming them, and the file is rewritten in the
// current layout. See configReload().
#define UT_INI_VERSION 44

// One family's lines, in file order. Plain bytes, zero-filled when unused, so UtConfig stays a
// POD that the change test can memcmp.
enum {
    kUtCfgFamilySlots = 16,      // lines per family; one more is refused with a WARN
    kUtCfgFamilySuffixCap = 48,  // the suffix, NUL included
    kUtCfgFamilyValueCap = 64    // the value, NUL included
};
struct UtCfgFamilyEntry {
    char suffix[kUtCfgFamilySuffixCap];
    char value[kUtCfgFamilyValueCap];
};
struct UtCfgFamilyStore {
    int count;
    UtCfgFamilyEntry e[kUtCfgFamilySlots];
};

// A custom game's key (the part of a view name after "custom-") is 1..63 of [a-z0-9_.+-].
enum { kUtCfgViewNameCap = 64 };

// Since ini_version 39 a member whose key was DROPPED (the feature switches, the timers, pad_y/
// pad_h/pad_gap, tooltip_class_*, text_language) keeps its default as a fixed value: nothing
// parses it, the code reading it is untouched, and enabled=0 still collapses the switches.
// live_cache_max is derived (its member is the floor); log_flush_each_line follows
// log_level=trace; tooltip_text_yes/no moved to [tab] of uniquetab-buttons.ini (their members stay
// empty).
struct UtConfig {
    int iniVersion = 0;

    // ---- [general] --------------------------------------------------------------------------
    // 0 = the mod goes quiet: no tab, no buttons, no tooltip line, and nothing new is collected.
    // Everything already stored stays in the mod's own journal file, and rescue=1 still hands it
    // back. It is a switch, not an uninstall. Applied by collapsing the feature switches below
    // (configReload), so an off mod is exactly the configuration "everything off" - no second
    // code path of its own.
    int enabled = 1;
    // error | warn | info | debug | trace. Parsed and validated here; the logging itself still
    // treats every line the same in this build.
    char logLevel[8] = "info";
    // 0 = follow the UI scale the game reports. Anything above 0 is a percentage (clamped to
    // 50..300 where it is used) and overrides it - for a display where the measured scale is
    // wrong.
    int uiScalePct = 0;

    // ---- [collection] - what the collection accepts ------------------------------------------
    int maxPerRecord = 1;         // copies of one record the collection keeps; 0 = no limit
    // 1 = a DRAG of a soulbound / untradeable unique is let through:
    // CursorHandlerItemMove::PrimaryReagentActivate refuses those two bytes before the deposit is
    // ever attempted, while the exe's own quick-move site has no such test - so without this the
    // same item goes in on shift-click and never on a drag. The mod clears the two bytes for the
    // duration of that ONE call and restores them unless the deposit succeeded. Barred in a
    // multiplayer session whatever this says; a shift-click still gets the same item in.
    int soulboundCollect = 1;
    // 1 = the collection works in a MULTIPLAYER session exactly as it does in single player, for
    // the local player. Other players need nothing installed and can see nothing. 0 = refuse
    // every deposit while the session is multiplayer, or while the mod cannot READ the mode.
    int mpCollect = 1;
    // 1 = refuse an item that is not pristine: any non-empty string in its ItemReplicaInfo other
    // than the base record (prefix, suffix, modifier, transmute, component, augment, ascendant)
    // or a non-zero seed / affix reroll counter. Off, because the stored row carries the whole
    // ItemReplicaInfo and hands it back on the take.
    int collectPristineOnly = 0;
    int collectQuickPass = 1;     // 1 = shift-click on one of our records goes to the normal tab
    // The OWN filter's start state. 1 = show only the records the collection already holds; every
    // other box is parked off-grid, which also hides it from the game's own search. The OWN
    // button toggles it and the mod writes the new value back into the view's uniquetab.state.
    int ownedOnly = 0;

    // ---- [display] - what the mod draws, and its controls ------------------------------------
    int groupButtons = 1;         // the category pad over the page, and its clicks
    int pageHotkeys = 1;          // the wheel scroll and Ctrl+PageUp/PageDown group switch
    // 1 = while the caravan's own search box has text in it, the category buttons whose group
    // holds a match are marked, using the engine's own Item::SearchText over the stored prototype
    // of every owned record, swept a few records per tick from a needle CHANGE.
    int searchButtons = 1;
    // 1 = ALSO mark a group for records you do NOT own. A record that has been shown at any point
    // this session is matched EXACTLY (the same engine predicate over the display prototype it
    // left behind); one that has never been on screen is matched against its catalogue display
    // NAME only, so that half UNDER-reports and never over-reports. Skipped while ownedOnly=1.
    int searchButtonsUnowned = 1;
    // 1 = write the one byte the shared item-box rollover tests before it builds the "Currently
    // Equipped" comparison, on the boxes of a collection group only (never on the vanilla
    // crafting-materials page). No detour and no engine call.
    int comparePopup = 1;
    // 1 = append one line to every item tooltip in the game saying whether that item is already
    // in the collection. Items that are not collectible get no extra line at all. Off = the
    // vanilla tooltip, and so is any failure to hook.
    int tooltipMark = 1;
    // 1 = the monster infrequents are collected too: every Rare item record of a slot (the game's
    // MI symbol; the faction-store and the crafted rares included), one box per record, after the
    // uniques of its slot's page. A generation input (the stamp): a flip regenerates every view
    // once, at the next generation. Default 1. Turned off with MIs
    // stored, those stay in the collection (the journal and the store keep them) but no page shows
    // them - the label's "+N not on these pages" counts them - until the flag is 1 again.
    int collectMi = 1;
    int plateLabel = 1;           // the group / owned / row-window line over the page
    // 1 = outline the grid, the pad and the label rectangles in force, with their numbers and
    // each group's cols x rows, so a [layout] key can be set by eye.
    int layoutOutline = 0;
    // 1 = an EDIT button on the pad, drawn just before OWN, opens the layout editor; 0 = no
    // button (the pad re-lays out at the reload; Ctrl+right-click and layout_edit=1 still open it).
    int editButton = 1;

    // ---- [layout] - where the grid, the pad and the label sit, per caravan cover size ----------
    // The largest caption size on the buttons, screen px; the cell bounds it as well.
    int padCaptionMax = 14;
    // zoom_pct (44): the collection's item icons drawn bigger, in percent - every cell times
    // zoom / 100, fewer items per screen, the icons magnified. 100..200, 100 = as generated; a
    // value outside the range is refused (one WARN, 100 kept). A hidden row of [layout], per
    // settings file like the layout lines. Clamped per group where a small grid cannot hold one
    // magnified cell (ut_layout.h utZoomFit).
    int zoomPct = 100;
    // grid_<W>x<H>, pad_<W>x<H>, label_<W>x<H>, padmat_<W>x<H> = x,y,w,h: the user's rectangles
    // for the caravan cover of that size (vanilla: 438x627). padmat_ is the pad on the game's own
    // materials page when the pad in force would cover real material boxes there.
    UtCfgFamilyStore layGrid = {};
    UtCfgFamilyStore layPad = {};
    UtCfgFamilyStore layLabel = {};
    UtCfgFamilyStore layPadMat = {};
    // pad_screen / label_screen = "<corner>,x,y,w,h": the pad / the label pinned to a SCREEN
    // corner, one value for every view and cover; empty = not set (the page's own place).
    char padScreen[64] = {0};     // the canonical value is at most 26 characters; the rest is
    char labelScreen[64] = {0};   // room for blanks typed by hand (never cut into another value)

    // ---- [keys] - the keys and clicks the tab answers to; the MAIN uniquetab.ini only ----------
    // The seven rows' text as the file says it (trimmed; a refused or empty value is the default),
    // and the bindings in force, DERIVED from them at the parse (ut_keys.h utKeysDerive: the
    // conflict pass applied). While a custom game is active both are the main file's.
    char keyRowsUp[48] = "PageUp";
    char keyRowsDown[48] = "PageDown";
    char keyButtonPrev[48] = "Ctrl+PageUp";
    char keyButtonNext[48] = "Ctrl+PageDown";
    char keyRowsWheel[48] = "Wheel";
    char keyButtonWheel[48] = "Ctrl+Wheel";
    char keyEditorOpen[48] = "Ctrl+RightClick";
    UtKeys keys;

    // ---- [files] -----------------------------------------------------------------------------
    // Where the JOURNAL file lives. Empty = the mod folder (the folder beside the .asi). Only the
    // journal and its exports move - the log, this file and the rescue report always stay in the
    // mod folder, because this file LIVES there and its path is resolved before a key has been
    // read. READ ONCE, at start-up, and never re-read. The main uniquetab.ini's only: g_cfg keeps
    // the main file's value while a custom game is active.
    char journalDir[260] = {0};
    // A GD Stash import file beside the journal, rewritten on every journal write.
    // 0 = off (an existing file is left alone), 1 = the collection plus anything not yet checked
    // against the page, 2 = every journal entry, the not-stored history included.
    int exportGds = 1;
    int exportCsv = 0;            // the same three modes, one RFC-4180 row per entry

    // ---- [one-shot] - commands that set themselves back to 0 ---------------------------------
    // 1 (with the caravan window open and a character loaded) = hand EVERY stored item back
    // through the engine's own take path, write the rescue report and set this back to 0. A
    // SECOND trigger does not depend on this file at all: create an empty RESCUE-NOW file beside
    // it. configReload notices it, DELETES it and latches rescue=1 until the run writes 0 back.
    int rescue = 0;
    // 1 = with the caravan open, drop every journal entry the reconciliation has just marked
    // "stored":false (never an unmarked one), after copying the whole file aside and logging
    // every record it drops. The only setting in the mod that deliberately removes entries.
    int journalPrune = 0;
    // 1 (with the caravan open on a collection group) = open the in-game layout editor; set back
    // to 0 when it opens. The pad's EDIT button and Ctrl+right-click on the pad open it as well.
    int layoutEdit = 0;

    // ---- [advanced] - tuning. Every one of these is clamped to the range in its ini line ------
    int liveCacheMax = 4096;      // display prototypes kept per HUD; past it boxes re-Load
    // How many records the search sweep checks per game-thread tick. It runs from a needle
    // CHANGE, never per frame, and stops as soon as it has walked the collection once; higher
    // marks the buttons sooner and does more work in one tick.
    int searchSweep = 32;
    int plateCountMs = 1000;      // shortest gap between two reagent-map walks for the counters
    int takeWatchMs = 400;        // how often the take watch may walk the map (game thread)
    // How long the take observer's arm stays valid. The exe runs GetMainPlayer ->
    // IsInventorySpaceAvailable -> GetItemReplicaInfo -> GetItemMaxStackSize -> CreateItem inside
    // one frame, so this is a sanity bound, not a tuning knob.
    int identityWindowMs = 250;
    // The button pad is a fixed 9-column x 3-row grid of 34 x padH cells with a padGap gutter and
    // a 3 px side margin, so it is 3 + 9*34 + 8*padGap + 3 = 320 px wide at the default gap and
    // spans record x 102..421 - one px inside the joint limit 422. The RECTANGLE is refused whole
    // when it would leave the band that is flat and opaque on the game's own materials plate AND
    // on all seven of the mod's own plates: record x 102..422, y 25..69.
    int padY = 25;
    int padH = 14;
    int padGap = 1;
    int plateLabelSize = 13;      // label height in record px (scaled like everything else)
    // 1 = the painted cover plate follows the group: a generated per-cell-size plate behind a
    // collection group, the ORIGINAL vanilla plate on group -1. 0 = never touch it.
    int plateSwap = 1;
    // The flat ground a cover plate gets when the caravan is a MOD's: six hex digits RRGGBB,
    // empty = the vanilla ground's own tone, sampled from the vanilla stash cover at generation
    // (06 06 06). A mod's cover carries its own baked box lattice that our frames cannot be
    // aligned to, so no pixel of it is used; only the mod's page is affected, never a vanilla
    // plate. Changing it regenerates the set on the next launch (it is part of catalogue.stamp).
    char plateGround[8] = {0};
    // The two tooltip texts, ASCII, read ONCE at start-up (they become engine-owned strings, so a
    // change applies from the next launch). Empty = the built-in wording.
    char tooltipTextYes[96] = {0};
    char tooltipTextNo[96] = {0};
    // The suffix of resources\Text_<lang>.arc the generated catalogue takes item names from.
    char textLanguage[8] = "EN";
    // A GameTextClass. The engine keeps a map<GameTextClass, style-record-name> and paints each
    // line in its class's style, so choosing a class IS choosing a colour - the mod still never
    // draws a pixel. 44 = 0x2C ItemEnchantmentStats, the mild olive green (145,203,0) the game
    // uses for component text. 74 = 0x4A EffectHeading, a warm orange (224,153,61). 1..84 are the
    // classes this build measured as registered; anything else resolves to an EMPTY style name
    // and the line would lose its colour entirely, so it is refused.
    int tooltipClassYes = 0x2C;
    int tooltipClassNo = 0x4A;
    // 0 = buffer log lines and flush once a second from the worker. 1 = write each line at once,
    // for debugging a crash where the very last lines matter. No key: it is 1 exactly while
    // log_level=trace.
    int logFlushEachLine = 0;
    // WRITTEN BY THE MOD, not by the user: where the tab was last left in the ACTIVE view. They
    // come from that view's uniquetab.state (see the top of this file), never from a settings
    // file, and configPersistInt writes them there.
    int uniqGroup = -1;           // -1 = the vanilla Crafting Materials layout, else the group
    int uniqRow = 0;              // the first visible row inside that group (virtual scroll)

    // ---- FIXED BEHAVIOUR - these are no longer settings --------------------------------------
    // Nothing parses these, so they always hold the value below; they stay in the struct so that
    // the code reading them is untouched, and so that a future build can make one a setting again
    // by adding one row to the table. The one exception is `enabled=0`, which collapses the
    // display and collection switches (and these) to off - see configReload.
    int dbLoad = 1;               // load the collection pages beside the game database
    int journal = 1;              // the private table may own rows: the collection itself
    int identity = 1;             // an item comes back exactly as it went in
    int identityRequireCallsite = 0;  // the exe call-site RVA is LOGGED, never required
    int takeWatch = 1;            // notice a take the exe does inline and cannot be hooked
    int livePages = 1;            // the page is re-pointed in place, so a switch is immediate
    int uniqPage = -1;            // the built-once page selector the live tab replaced
};

namespace ut {

extern UtConfig g_cfg;

// ---- THE TABLE ------------------------------------------------------------------------------
enum UtCfgType {
    kUtCfgBool = 0,   // an int that may only be 0 or 1
    kUtCfgInt,        // an int in [lo, hi]
    kUtCfgStr         // text, at most `cap` - 1 bytes
};

// Null when `value` (trimmed, comment cut) is legal; else a few words saying what it should be.
typedef const char* (*UtCfgValueCheck)(const char* value);

struct UtCfgKey {
    const char* section;   // the [block] it is written under; the parser ignores blocks
    const char* name;      // the key as it appears in the file
    int type;              // UtCfgType
    int def;               // default for kUtCfgBool / kUtCfgInt
    int lo, hi;            // inclusive range; a value outside it is clamped and logged
    const char* defStr;    // default for kUtCfgStr (never null)
    size_t off;            // offsetof(UtConfig, member)
    size_t cap;            // sizeof(member) for kUtCfgStr
    const char* const* choices;  // null-terminated list of legal texts, or null
    const char* comment;   // ONE line, in plain words, written after the value
    int flags;             // kUtCfgHidden | kUtCfgWhenSet
    // kUtCfgStr only, optional: a non-empty value it refuses is ignored with one WARN (the
    // default kept), like a word that is not one of `choices`.
    UtCfgValueCheck check;
};
// kUtCfgRefuse on a kUtCfgInt row: a value outside [lo, hi] is REFUSED - one WARN, the default
// kept - instead of clamped (zoom_pct: a typo of 1500 must not become the largest zoom).

enum {
    kUtCfgHidden = 1,      // a commented blob row of its section (live only when not default):
                           // the [advanced] blob, or one row of [layout] (zoom_pct)
    kUtCfgWhenSet = 4,     // a visible line written only while it holds a value (the editor's
                           // pad_screen / label_screen); never counted as a missing setting
    kUtCfgRefuse = 8       // kUtCfgInt: outside the range = refused (the default), not clamped
};

struct UtCfgSection {
    const char* name;      // "general"
    const char* blurb;     // one line under the [block] header
};

extern const UtCfgKey kUtCfgKeys[];
extern const int kUtCfgKeyCount;
extern const UtCfgSection kUtCfgSections[];
extern const int kUtCfgSectionCount;

// ---- KEYED FAMILIES (see the top of this file) ----------------------------------------------
// True when `suffix` names a member of the family (else the line is an unknown key).
typedef bool (*UtCfgSuffixCheck)(const char* suffix);

struct UtCfgFamily {
    const char* section;        // the [block] its lines are written under
    const char* prefix;         // "grid_"
    size_t off;                 // offsetof(UtConfig, <its UtCfgFamilyStore>)
    UtCfgSuffixCheck suffixOk;
    UtCfgValueCheck valueOk;
    const char* example;        // suffix=value, written commented out: "438x627=105,76,333,552"
    const char* comment;        // ONE line, written above the example
};

extern const UtCfgFamily kUtCfgFamilies[];
extern const int kUtCfgFamilyCount;

// The family of `table` that `key` belongs to (its suffix passing that family's own check), or
// null. `suffix` (optional) receives a pointer into `key`. A family table and its stores can live
// in any struct and any file: `off` is relative to whatever base the caller pairs it with.
const UtCfgFamily* configFamilyIn(const UtCfgFamily* table, int count, const char* key,
                                  const char** suffix);
// The same over this file's own kUtCfgFamilies.
const UtCfgFamily* configFamilyOf(const char* key, const char** suffix);
// The store a family describes inside `base` (a UtConfig for kUtCfgFamilies).
inline UtCfgFamilyStore* configFamilyStore(const UtCfgFamily& f, void* base) {
    return (UtCfgFamilyStore*)((char*)base + f.off);
}
// The value stored for `suffix`, or null.
const char* configFamilyGet(const UtCfgFamilyStore& s, const char* suffix);
// Stores (or replaces) one line. False when the store is full or either text is too long.
bool configFamilySet(UtCfgFamilyStore* s, const char* suffix, const char* value);
// The value of a whole family key in `c` - configFamilyValue(g_cfg, "grid_438x627") - or null.
const char* configFamilyValue(const UtConfig& c, const char* key);

// ---- PER CUSTOM GAME AND THE STATE FILE (see the top of this file) ---------------------------
// True for journal_dir and the seven [keys] rows, the keys a custom game's own file does not hold
// (every other row and every family line is a key of both files).
bool configMainOnlyKey(const char* key);
// The custom game's key of a view name: "reignofterror" for custom-reignofterror and for
// basemods+custom-reignofterror; "" for vanilla, basemods or anything that is not a legal key.
std::string configCustomKey(const char* viewName);
// The files of the view `viewName` beside the main settings file `mainIni` (pure path
// arithmetic, ut_folders.h): `ini` = <dir>\mods\<key>\uniquetab.ini, shared by custom-<key> and
// basemods+custom-<key> (empty for a view that is not a custom game's); `state` = the view
// folder's uniquetab.state - <dir>\uniquetab.state for the start-up view (vanilla, basemods or
// ""), <dir>\mods\<key>\uniquetab.state for custom-<key> and
// <dir>\mods\<key>\basemods\uniquetab.state for basemods+custom-<key>.
void configViewFiles(const wchar_t* mainIni, const char* viewName, std::wstring* ini,
                     std::wstring* state);
// The active view changed (ut_generate, before the view's generation reads a setting). A custom
// game's own file is written when absent (the seed: the main file's values in force), migrated
// when an older build wrote it, and read; the view's state file is read; g_cfg becomes the game's
// file (journal_dir and [keys] from the main one) or, for vanilla/basemods, the main file - with the state
// applied - and the logger's level follows it. One INFO line when the game's own file is created
// or starts or stops applying; configGeneration() bumps when the result differs. Any thread; file
// I/O under the settings lock.
void configSetView(const char* viewName);
// The view g_cfg was derived for ("" until the first configSetView).
void configActiveView(char* out, size_t cap);
// The value the int row `key` WILL have in the view `viewName`, without making it active (the
// custom-game probe runs before its configSetView): the main file's for vanilla/basemods; for a
// custom game its own file's line, the compiled default when a current file does not name it,
// or the main file's value when the game has no file yet or one from an older build (the seed /
// the migration). Clamped to the row's range; 0 for a key that is not an int row.
int configViewIntFor(const char* viewName, const char* key);
// Renders a custom game's own file for `key`: the main file's template (configRender) with every
// value from `src`, a header naming the game, and journal_dir as a commented line saying it is
// set in the main file only. Returns the bytes written (the NUL is not counted), or 0 if it did
// not fit.
size_t configRenderView(char* out, size_t cap, const char* key, const UtConfig& src);
// Renders a state file holding the three state values of `src`. Same return rule.
size_t configRenderState(char* out, size_t cap, const UtConfig& src);
// True for uniq_group, uniq_row and owned_only.
bool configIsStateKey(const char* key);

// Renders the whole file - header, sections, one line per key - taking every value from `src`.
// Returns the bytes written (the NUL is not counted), or 0 if it did not fit in `cap`.
size_t configRender(char* out, size_t cap, const UtConfig& src);

// Bytes configRender produces for the DEFAULTS. What a fresh file weighs.
size_t configTemplateBytes();

// Re-reads the file; writes it with the defaults if it does not exist. A file from another
// ini_version is MERGED into the current layout - kept values, defaulted new keys, dropped
// unknown keys - and rewritten. Returns true if the file was parsed.
bool configReload(const wchar_t* path);

// Rewrites ONE integer key in the active view's settings file (configPersistStr's rule), keeping
// every other line untouched. Atomic: a temp file next to it is written in full and then moved
// over the original, so a reader never sees a half-written file. Used by the one-shot commands
// (rescue=0 goes back to the file that said rescue=1); uniq_group, uniq_row and owned_only go to the
// ACTIVE view's state file instead (created when absent), and g_cfg takes the value at once.
bool configPersistInt(const char* key, int value);

// Rewrites up to 8 keys in ONE atomic rewrite, fixed or family keys alike. values[i] is the text
// written after the '='; a NULL value REMOVES that key's line. Every other line stays byte for
// byte. A key the file does not have yet is inserted at the end of its own [section] (the last
// line of the file when the section is not there). The value is written as given: the next
// reload checks it like a line typed by hand. THE FILE: the active view's - while a custom game's
// view is active every key goes to that game's own file (written again from the seed first when
// it was deleted), else to uniquetab.ini; journal_dir always to uniquetab.ini. In both a hidden
// key keeps its row in the [advanced] blob.
bool configPersistStr(const char* const* keys, const char* const* values, int n);

// The [section] a key belongs under, or null (then a new key is appended at the end of the file).
typedef const char* (*UtCfgSectionOf)(const char* key);

// configPersistStr's engine for ANOTHER key=value file (a strings or groups file that keeps its
// own keys and families): the same atomic rewrite, the same NULL-deletes rule, with the file's own
// section rule. The settings file's own reload is only forced when `path` is that file.
bool configPersistFile(const wchar_t* path, const char* const* keys, const char* const* values, int n,
                       UtCfgSectionOf sectionOf);

// Bumped every time a reload changes g_cfg. A reader that caches something derived from g_cfg
// compares this instead of the struct; g_cfg is replaced by a struct copy on the worker thread,
// so a cache built from a torn read is rebuilt on the next call.
long configGeneration();

}  // namespace ut
