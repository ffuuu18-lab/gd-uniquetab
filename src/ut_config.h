// ut_config.h - every setting the mod has, in one plain text file (uniquetab.ini, in the mod
// folder).
//
// "key=value" per line, grouped into [sections] that exist for the reader only - a key is looked
// up by NAME, so moving a line between sections changes nothing. The file is re-read once a
// second by the worker thread. Values are plain ints written by the worker thread and read by the
// render thread; a torn read is impossible for an aligned int on x64 and a one-frame-stale value
// is harmless.
//
// ONE TABLE IS THE TRUTH. kUtCfgKeys below is the only list of keys there is: the parser walks
// it, the file the mod writes is RENDERED from it (configRender), and the test suite derives what
// it expects from it. Adding a setting means adding one row - the text and the parser cannot
// drift apart any more, because there is only one of each.
//
// GEOMETRY UNITS. The numbers in [advanced] that place something on the page (pad_y, pad_h,
// pad_gap, plate_label_size) are RECORD px - the caravan window's own coordinates, which the mod
// multiplies by the UI scale it measures at run time. They are clamped to their safe range when
// they are read here AND clamped again where they are used.
#pragma once

#include <stddef.h>

// Bumped whenever the key set changes. A file written by an older build is MERGED, never reset:
// every key that still exists keeps the user's value, new keys take their default, keys this
// build no longer has are dropped with one log line naming them, and the file is rewritten in the
// current layout. See configReload().
#define UT_INI_VERSION 35

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
    // button toggles it and the mod writes the new value back into the file.
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
    int plateLabel = 1;           // the group / owned / row-window line over the page

    // ---- [files] -----------------------------------------------------------------------------
    // Where the JOURNAL file lives. Empty = the mod folder (the folder beside the .asi). Only the
    // journal and its exports move - the log, this file and the rescue report always stay in the
    // mod folder, because this file LIVES there and its path is resolved before a key has been
    // read. READ ONCE, at start-up, and never re-read.
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
    // for debugging a crash where the very last lines matter.
    int logFlushEachLine = 0;
    // WRITTEN BY THE MOD, not by the user: the view the tab was last left in. They live here
    // rather than in a file of their own because a setting the user can see and reset by hand is
    // worth more than a tidier split, and a version bump now KEEPS them like any other key.
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
};

struct UtCfgSection {
    const char* name;      // "general"
    const char* blurb;     // one line under the [block] header
};

extern const UtCfgKey kUtCfgKeys[];
extern const int kUtCfgKeyCount;
extern const UtCfgSection kUtCfgSections[];
extern const int kUtCfgSectionCount;

// Renders the whole file - header, sections, one line per key - taking every value from `src`.
// Returns the bytes written (the NUL is not counted), or 0 if it did not fit in `cap`.
size_t configRender(char* out, size_t cap, const UtConfig& src);

// Bytes configRender produces for the DEFAULTS. What a fresh file weighs.
size_t configTemplateBytes();

// Re-reads the file; writes it with the defaults if it does not exist. A file from another
// ini_version is MERGED into the current layout - kept values, defaulted new keys, dropped
// unknown keys - and rewritten. Returns true if the file was parsed.
bool configReload(const wchar_t* path);

// Rewrites ONE integer key in the file, keeping every other line untouched. Atomic: a temp file
// next to it is written in full and then moved over the original, so a reader never sees a
// half-written file. Used by the tab's view keys and by the one-shot commands.
bool configPersistInt(const char* key, int value);

}  // namespace ut
