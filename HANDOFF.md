# HANDOFF - for whoever maintains this next

Written for a maintainer, human or machine, who has never seen this code. It describes what is
here, why it is shaped this way, and the handful of rules that must survive any change. Read
sections 1 and 2 before you touch anything.

The mod is C++17, MSVC, built by `build.bat` into `bin\uniquetab.asi` - a plain DLL with the
extension Ultimate ASI Loader looks for. It exports nothing. `README.md` is the player's
document; this one is the code's. This text describes version 1.1.0; the version is written in
one place, `src\ut_version.h`, which the start-up banner and `tools\package.ps1` both read.

---

## 1. The one rule

**An item must never be lost or duplicated.** Everything below is downstream of that.

The danger is specific and worth stating in full, because it is the reason for most of the
design. The engine's `ReadPlayerReagents` drops every reagent-map entry whose record has
`craftingMaterial == 0`, and `LoadPlayerReagents` can trigger a `SaveReagents` by itself. So if
the mod's items lived in the engine's reagent map, then a removed mod, a patch that broke the
gate, or a missing `uniq-pages.arz` would prune them at the next character load - and Steam Cloud
would push the pruned save to every machine on the account. That is a one-way door.

Two things stand in front of it:

- **The private table.** The mod's records never enter the engine's reagent map at all. The
  collection is the mod's own `record -> {count, identity}` table, loaded from the journal.
- **The journal.** `uniq-items.jsonl` is the authority. Every accepted deposit is written to it
  atomically, with the item's full identity, before anything else is believed.

## 2. Invariants

These are release blockers, not preferences.

1. **The mod writes only its own files**, all inside its own folder: `uniquetab.ini`,
   `uniquetab.log` (+3 archives), `uniq-items.jsonl` / `uniq-items-hc.jsonl`, `uniq-export.gds` /
   `uniq-export-hc.gds`, `uniq-export.csv` (optional), `catalogue.bin`, `catalogue.stamp`,
   `uniq-records.txt`, `uniq-pages.txt`, `uniq-groups.txt`, `uniq-pages.arz`, `rescue-report.txt`,
   and the same generated set once more under `views\<name>\` for each custom game that has
   been entered.
   Never a `.gst`, never `player.gdc`, never `options.txt`, never anything in the game folder
   proper - with the one documented exception of `settings\ui\caravan\uniq_plate_*.tex`, which
   section 7 explains and which is the only name pattern ever written there.
2. **Never lose or duplicate an item.** A deposit that cannot be recorded is refused *before* the
   engine moves anything. A refusal is a true no-op: the item stays where it was.
3. **One journal per mode.** Softcore and hardcore are separate files and never mix. When the mode
   cannot be read, the mod refuses rather than guessing - guessing writes a hardcore item into the
   softcore collection.
4. **All or nothing on the bindings.** If one early binding cannot be found or confirmed, nothing
   at all is installed: no detour, no tab, no deposit. A half-bound mod is the one thing that
   could damage a save. See section 5.
5. **Multiplayer stays neutral.** The overlay is loaded *after* `Engine::GetDatabaseArchiveChecksum`
   has been computed, so the multiplayer database checksum is unchanged and
   `HasLoadedCustomDatabase` stays false. Collection is tested as host and as client (`mp_collect`).
6. **Nothing generated is shipped.** `catalogue.bin`, `catalogue.stamp`, the three `uniq-*.txt`
   lists, the `uniq-pages.arz` overlay and the cover plates are built on the player's machine. The
   package is `uniquetab.asi` and three documents. Their reference copies belong in
   `data\oracle\` as test fixtures, which neither the package nor the public tree carries.
7. **Nothing in the tree names a machine.** No absolute path, no user profile folder, no account
   id, in code, comments, scripts or documents; the mod finds everything relative to the game
   folder and the plugin's own location.

## 3. The architecture, by file

### Start-up order (`src\dllmain.cpp`)

`DllMain` does the minimum: resolve the mod folder, open the log, install the fault watchdog
(a vectored handler that only *logs* an access violation with this DLL on the stack and always
returns `EXCEPTION_CONTINUE_SEARCH`), start one worker thread, return. Nothing that can block runs
under the loader lock.

The worker then does, in this order:

```
configReload(ini)      log_level must be in force before the export lines are written
waitForGameModules()   Engine.dll + Game.dll, 60 s ceiling
resolveExports()       by decorated name; a missing REQUIRED symbol turns the mod off
generateEnsure()       catalogue.bin, the three lists, the overlay and the plates, current
                       with the game's archives
panelInit()            loads catalogue.bin
reagentInit()          exports, the craftingMaterial offset, the record lists, the arz path
tooltipInit()          the six rollover exports; every part optional
bindingsGate()         <- ALL OR NOTHING. A failure here returns before any hook exists
hooksInstall()         MinHook: 7 core detours + the reagent set; the 5 tooltip ones separately
READY
```

then a once-a-second loop: re-read the ini, flush the log, the late-load tick, a heartbeat, and a
frames-not-advancing stall check (which also opens the named event `CRASHREPORT`, because a
"freeze" is usually the game's own crash reporter waiting behind a fullscreen window).

**The loader decides when all this happens.** A `winmm.dll` loader is mapped at process start,
through the exe's import table; a `dinput8.dll` loader is mapped when the engine initialises
input - *after* the engine has read its database. Every assumption in the start-up path has to
work both ways. That is what `reagentLateLoadTick` is for: a non-zero
`Engine::GetDatabaseArchiveChecksum` means the `LoadMainDatabase` detour missed its moment, and
the tick loads the overlay itself. `g_dbTried` lets exactly one of the frame tick and the worker
tick win.

### The files

| file | what it owns |
| --- | --- |
| `dllmain.cpp` | entry point, the worker thread, the fault watchdog, the heartbeat and stall check |
| `hooks.cpp/.h` | MinHook. 7 core detours: `Engine::PresentSurface` (the frame tick), `GameEngine::Update`, `GameEngine::SetTransferOpen`, the two `InventorySack::AddItem` overloads, `InventorySack::RemoveItem`, `WinWindow::WindowProc` |
| `gd_runtime.cpp/.h` | every engine function resolved by name, and the opaque handles passed around |
| `gd_exports.h` | generated from the real export tables. **Zero addresses.** |
| `gd_exports_extra.h` | the same, hand-kept, for the few names the generator does not select |
| `gd_exports_reagent.h` | generated, the reagent/item half |
| `ut_paths.cpp/.h` | **the only** place that decides where files live. Never call shell32 from here |
| `ut_log.cpp/.h` | the line logger; buffered, five levels, 3 rotated archives |
| `ut_config.cpp/.h` | one table of settings. The parser, the written file and the test all read that table |
| `ut_generate.cpp/.h` | the DLL side of `src\gen`: keep the generated files current before anything reads them, and hold the active VIEW - which database the generated files describe, and therefore which folder they are read from |
| `src\gen\` | the catalogue generator: `arz_reader`, `arc_reader`, `catalogue_gen`, `pages_gen`, `generate` |
| `src\model\` | `catalogue` (the binary format), `collection` (ownership), `layout` (rows and pages) - pure, no Windows |
| `ut_panel.cpp/.h` | the collection view: the catalogue, the ownership state fed by the sack detours, the group label, the 26-button pad |
| `ut_reagent.cpp/.h` | the big one. The database overlay, the `craftingMaterial` gate, the deposit and take detours, the bag proof, the identity capture, the census and the crash diagnostics |
| `ut_store.cpp/.h` | the private table: the mod's own `record -> {count, identity}`, and the display prototypes `showBox` points boxes at |
| `ut_live.cpp/.h` | the page itself: the group table, the box capture, the relayout, the owned-only filter, the wheel and button input |
| `ut_plate.cpp/.h` | the page *background*: the captured `ReagentWindow`, the plate swap per group, the visibility test, the label draw, the search marks |
| `gen\plate_gen.cpp/.h` | the plates themselves: the TEX reader and writer, the ground rule, the 9-slice frame stamp - the port of `tools\make_plates.py`, byte for byte |
| `ut_tooltip.cpp/.h` | the item rollover: the compare popup (one byte, no detour) and the "already collected" line (five Game.dll detours, all-or-nothing) |
| `ut_rescue.cpp/.h` | the journal, the identity overlay a take is rebuilt from, the CSV and GDS exports, the rescue report. Pure file I/O - not one engine call |
| `ut_bindings.cpp/.h` | **every fact the mod knows about the game's memory**, in one place, with how it is obtained and how it is confirmed |
| `ut_replicasize.h` | `sizeof(ItemReplicaInfo)`, decoded from two independent sources that must agree |
| `ut_depositgate.h`, `ut_pagegate.h`, `ut_paintgate.h`, `ut_ownedfold.h`, `ut_rowmath.h`, `ut_textfold.h`, `ut_fontmetrics.h`, `ut_gds.h` | the pure-logic headers: a decision extracted from its engine context so an offline test can run exactly the code the mod runs |

Those last headers are the pattern worth keeping. Whenever a decision is dangerous - which slot
inside a replica blob is a `std::string`, whether a page may paint, which row a scroll lands on -
it is written as a pure function in a header, the engine side supplies an SEH-guarded probe, and
the test supplies a plain one. Both run the same rule.

### Threads

- **Game thread** - everything that touches engine memory. Reached from the
  `Engine::PresentSurface` detour, from `GameEngine::Update`, and from the detours themselves.
- **Render thread** - `panelDraw` and the plate/label draw.
- **Worker thread** - the ini, the log flush, the journal writes, the exports. It reads engine
  state only through published atomics.

The rule: if a function can call into the engine, its comment says GAME THREAD ONLY and it means
it. `storeTick(gameThread)` and `reagentLateLoadTick(gameThread)` both carry the flag explicitly
because they are called from both.

## 4. The three mechanisms that make the tab exist

There is no way to add a third caravan tab, so the collection **shares the Crafting Materials
page**.

1. **Database.** `uniq-pages.arz` adds new records - `records/ui/caravan/uniq_pNN.dbr` and its
   boxes, plus the frame `uniq_frame.dbr` - and the mod hands one of *those* back when the
   engine asks `ObjectManager::LoadTableFile` / `GetLoadTable` for a reagent window. The vanilla
   `caravan_materialwindow.dbr` is **never overridden**; that is why the player keeps their real
   materials page. The archive is loaded by calling `Engine::LoadDatabase` from a post-detour on
   `Engine::LoadMainDatabase`, i.e. after the checksum is already computed (invariant 5).
   The archive is **generated**, like the four text files: `src\gen\arz_writer` writes it
   from the same derived geometry, so the frame's `WindowLocationX/Y` and its box count
   follow the database a stash-UI mod merged instead of a file built once on a developer's
   machine. Nothing of it ships: a fresh install has no overlay until the first generation writes
   one, and a generation that fails leaves none, so the tab is off with one error line
   (`uniq-pages.arz NOT FOUND`) and the player's own materials page is untouched.
2. **The background.** The page's plate is painted at generation time by `src\gen\plate_gen`,
   on the ground a normal stash tab shows, and written into `<game>\settings\ui\caravan\`; a
   stash-UI mod therefore gets a grid on its own background instead of ours over its art.
   Section 7 has the rule.
3. **The gate.** What may enter a reagent box is one bool at `Item + 0xC64`, loaded from the
   record field `craftingMaterial`. The exported getter `Item::IsReagentCompatible` has *zero*
   callers - the engine reads the byte inline - so the byte is the only lever. The mod reads the
   offset out of that getter's own seven code bytes (never hard-coded), sets the byte to 1 on
   items whose record is on our page, and clears it again for the whole registry whenever any of
   `DepositReagents` / `DepositSackIntoReagents` / `DepositTransferReagents` is on the stack, so
   the auto-deposit buttons cannot vacuum uniques out of an inventory.

The byte lives only in the narrowest window in which it is needed: caravan open, Crafting
Materials on screen, a collection group shown. The moment any of that stops being true it is
written back to 0 on every item the mod armed.

**The mod creates, moves and deletes nothing.** The engine does all of that itself when the user
drops an item on a box. The mod's job is to decide whether the drop is allowed and to record what
happened.

### The view

All three of those mechanisms are derived from a database, and the process sees more than one.
`Engine::LoadMainDatabase` builds the base one - the base game, the expansions and, when
`/basemods` is on the command line, `<game>\mods\database.arz`; a custom game chosen from the
main menu then loads `<game>\mods\<name>\database\*.arz` **on top of that** through
`Engine::LoadDatabase`, after `LoadMainDatabase` has returned. So the custom layer does not
replace the basemod one, and the four views are `vanilla`, `basemods`, `custom-<name>` and
`basemods+custom-<name>` (`gen::ViewSpec`).

- **Generation takes the view.** `gen::ensureOutputs(..., view)` builds the override layers in
  that order (`gen::loadView`), fingerprints the custom game's own archives into the stamp
  beside the game's, and writes the same six outputs. The start-up view keeps the mod folder
  itself, so an installed copy that never enters a custom game is unchanged; a custom view's go
  to `<mod folder>\views\<name>\`, which is why entering a custom game and leaving it again
  never rewrites what the main campaign reads. The plate names carry the view's tag when it has
  one (`uniq_plate_64x96_346x625_lootascension.tex`), because the cover size alone collides when
  two mods share it, and the vanilla and `/basemods` names are byte-for-byte what they were.
- **The switch is the database detour.** `hk_LoadDatabase` recognises a custom game's archive
  and calls `ut::viewEnterCustom`. The engine names that archive relative to the game folder
  with forward slashes - `mods/LootAscension/database/LootAscension.arz` - so
  `gen::customGameFolder` accepts `[<dir>/]mods/<name>/database/<file>.arz` in either separator
  and any case, relative or absolute, resolves it against the game folder (then the current
  directory) and keeps it only where that archive exists; the main database's own archives, the
  Crucible's DLC parts at the game root and `mods/database.arz` name no custom game.
  `test_catalogue --view` feeds the engine's exact string. `viewEnterCustom` then reads the mod's
  own archive index first and asks `ModView::touchesCaravan()`: a custom game that holds no
  `records/ui/caravan/*` record and no caravan texture - the Crucible, and every loot or
  class mod - has the start-up view's geometry already, so it gets one info line and no view
  of its own. Otherwise the view is generated
  SYNCHRONOUSLY on the game thread before it does anything else: the overlay a custom view needs
  is that view's own `uniq-pages.arz`, and the overlay is loaded from inside this same detour, so
  the files have to exist by then. `hk_LoadMainDatabase` calls `ut::viewEnterStartup` before the
  original runs, because that call rebuilds the base database and takes any custom layer with
  it - that is the whole return path. `applyViewChange` then re-reads `uniq-records.txt`,
  `uniq-pages.txt` and the group table, re-points `g_arzPath`, drops the plate probe
  (`plateOnViewChanged`) and reloads the overlay. Everything the tab reads goes through
  `ut::utViewFile`, which is `utModFile` on the start-up view.
- **No record of ours is read before a custom view's overlay is in.** `ObjectManager::LoadTableFile`
  caches every record it hands out (the table depot at `ObjectManager+0x170`) until the next
  `Engine::LoadMainDatabase` empties it (Engine.dll `0x1C4EF0`, called at its start); a later
  `Engine::LoadDatabase` replaces the archive's entry (`0x27FE70`, unconditionally, last load
  wins) but never the cached copy. Every view writes its frame as `records/ui/caravan/uniq_frame.dbr`,
  so a read of that record between the main database and the custom view's overlay pins the
  start-up copy: the first in-game run of the switch built Baul's page from it - `boxvec=160`
  where the view wrote 144, at the vanilla `0,88` instead of `513,70`. That read was the
  "are our pages still in the database" probe in `hk_LoadDatabase`, which ran before the view
  switch. The switch now runs first, and `checkServedFrame` reads the frame back once the view's
  overlay is in and compares its box count and `WindowLocationX/Y` (`LoadTableBinary::GetInt`)
  with the record read out of the view's own `uniq-pages.arz` - never after a `LoadMainDatabase`
  reload, where the read itself would pin the start-up copy ahead of a custom game's archives.
  `ut_plate`'s verifier checks the same thing from the live window: `window+0x40/+0x44` is
  `WindowLocationX/Y * UI scale` (written by `ReagentWindow::Load` at exe `0x13218F`), so the
  page's location over the scale must equal the written record's within 4 units, and the box
  vector's size must equal its box count.
- **The catalogue is shared; the journal is per custom game.** Which items the collection can
  hold is not a view decision: `collectItems` reads the base game's archives only, and the mod
  layer is read for the geometry and for nothing else, so `catalogue.bin` and `uniq-records.txt`
  come out byte-identical for a custom view and the start-up one. Where the items ARE is: a
  custom view keeps its own journal set, `<journal dir>\views\custom-<name>\uniq-items[-hc].jsonl`
  with its `.gds`/`.csv`, `journal_prune` copies and `rescue-report.txt` in the same folder
  (section 8). The key is the custom mod's name, not the view's, so `custom-<name>` and
  `basemods+custom-<name>` share one journal (`ut::journalViewKeyFor`). The files follow the view
  where the hardcore/softcore mode does - `followSaveVariant`, on the new world's first tick,
  after the previous world's teardown (which the log shows running AFTER the database reload, so
  switching inside the detour would let that teardown touch the new file) - and before the
  caravan's open edge runs the reconcile, so the reconcile reads and rewrites only the active
  view's file. A switch flushes only what is owed and a view change alone arms no export, so the
  start-up files come out of a custom game byte for byte as they went in. A custom game with no
  view (the Crucible) stays on the start-up files. A record that only a custom game's
  database has is in no catalogue at all: it cannot be deposited (the `craftingMaterial` gate
  arms records off `uniq-records.txt`) and it owns no box. A line for one in the journal - hand
  written, or from a later build that makes the item set view-aware - is not dropped by anything:
  the file is only appended to, an unrecognised record is an unknown event the ownership model
  ignores, `journalItemName` simply finds no name, and the only code that removes a line is the
  explicit, off-by-default `journal_prune`, which copies the file aside first. Nothing in
  `reagents.gst` or any save file is touched, ever.

## 5. The bindings - the five classes and the gate

`src\ut_bindings.h` is the single inventory. Read its header comment; it is the authoritative
version of this section.

The mod **never asks what version the game is**. It cannot: the exe's `FileVersion` resource reads
`0.3.0.0` on 1.3.0.8. It asks a *capability* question instead, one binding at a time - can this
still be found, and does what was found still look like the thing it is supposed to be?

| class | how it is obtained | what a game patch does to it |
| --- | --- | --- |
| **EXPORT** | `GetProcAddress` by decorated name in Game.dll / Engine.dll | survives anything that keeps the name, which is every minor patch - the names come out of the compiler, not out of a table someone maintains. `gd_exports*.h` hold zero addresses, so there is nothing to go stale |
| **SIGNATURE** | a byte pattern scanned in a module's `.text` at run time | survives a data patch; does **not** survive a recompile of that function. Every pattern must match an exact expected count (once, or twice for the take pair) and the run-time scan re-counts and refuses anything else |
| **DECODED** | an offset or call target read out of an *exported* function's own instruction bytes | survives everything an export survives, plus any patch that only moves the field |
| **STRUCTURAL** | a layout the *compiler* guarantees - MSVC's `std::map` node, MSVC's `std::string`, a `std::vector`'s stride | nothing to drift while the game is still built with MSVC. Documented, never scanned |
| **LITERAL** | a number compiled in | the fragile ones. Every survivor is a *field offset inside an object the mod already proved it holds*, read under SEH with a plausibility test. **None is a code address any more** |

Two orthogonal flags:

- **EARLY vs LATE.** Early bindings must all be resolved and confirmed before any hook is
  installed. Late ones need the exe's `.text`, which the Steam DRM stub only decrypts once the
  game is running - so they are scanned from the game thread, retried (about 40 attempts), and
  each turns only its own route off on failure. `bindingsLateReport()` emits the second summary.
- **CRITICAL vs ADVISORY.** A critical row has a consumer: something reads it, so a failure turns
  the mod off. An advisory row is decoded, confirmed and reported like the others, but nothing
  reads it, so a failure is one WARN. The offline test asserts the flag row by row.

`bindingsGate()` logs the module identity (size and PE timestamp of the exe, Game.dll and
Engine.dll) **for the record, never as a gate**, one INFO line with the counts per class, and the
whole table at DEBUG. It returns false if any early row failed, and `workerMain` then returns
without installing anything. `bindingsGateFailure()` names the row, and that name is what a player
sends you.

The cross-check pattern is the important half. A signature match is not trusted on its own: the
located `ReagentWindow::Load` must also equal slot `+0x18` of the live window's vtable; the three
`UIReagentItem` functions must each occupy their own slot (`+0x18` / `+0xA8` / `+0xB8`) in a real
box widget's vtable. Only then are the other slots in that vtable believed.

## 6. How to add a binding, a decoder or a signature

**A binding.** Add a row to the inventory comment at the top of `ut_bindings.h` - what it is, how
it is obtained, how it is confirmed, and the 1.3.0.8 evidence value. Call `bindingsNote(name,
value, ok, why)` from wherever it resolves. Choose the phase honestly: **early means obtainable
without the game thread**. A row that quietly becomes lazily-decoded while staying early would
turn the mod off on a healthy game - `tools\test_bindings.cpp` asserts the classification for
exactly this reason. Choose the gate honestly too: critical only if something actually reads it.

**A decoder.** Put the byte-walking in a header with no Windows, no SEH and no mod state, the way
`ut_replicasize.h` does, so the offline test can run the same code over a file on disk. Decode
from an **exported** function's bytes - the name is the anchor and is what makes the binding
survive. Confirm with a plausibility test (non-zero, in range, correctly aligned, lands inside the
block it should). Where two independent sources exist, decode both and require that they agree;
never fall back to the 1.3.0.8 constant when they disagree - that is one ERROR line, not a guess.

**A signature.** Write the pattern as `bytes[]` + a parallel `mask[]` (`0` = wildcard) in
`ut_bindings.cpp`, so the mod and `tools\test_bindings.cpp` share one copy. Wildcard every
address-bearing byte: `call [rip+disp32]` displacements, `E8 rel32` targets, RIP-relative
`mov rcx,[rip+disp32]`. Include in the pattern the instruction that makes the behaviour what you
claim it is - the drag pattern is 61 bytes long because it has to reach the `mov [rdi+0x30],ebp`
that clears the cursor slot, which is what makes that removal unconditional. State the expected
hit count and let the scanner refuse anything else. If the site is a *return-address window*,
express the window as offsets from the start of the match (`kUtSiteAWindowLo/Hi` and friends), not
as RVAs. Then run `build_test_bindings.bat` against a decrypted exe image and confirm the count
and the 1.3.0.8 RVA.

A signature in the **exe** is late (DRM). A signature in **Game.dll** is early - the module is
plain on disk.

## 7. The data pipeline, and the byte-identical oracle

```
a Grim Dawn 1.3.0.8 game folder
        |
        |  tools\build_catalogue.py   -> data\oracle\catalogue.json (+ catalogue-stats.md)
        |  tools\build_uniq_db.py     -> data\oracle\uniq-pages.arz, uniq-records.txt,
        |                                uniq-pages.txt, uniq-groups.txt, uniq-pages.json
        |  tools\pack_catalogue.py    -> data\oracle\catalogue.bin
        |  tools\make_plates.py       -> data\oracle\plates\uniq_plate_WxH.tex
        v                                                          (ALL OF IT: FIXTURES)
src\gen  (the same job, in C++, inside the DLL, on the player's own archives)
        -> <mod folder>\catalogue.bin, uniq-records.txt, uniq-pages.txt, uniq-groups.txt,
           uniq-pages.arz
        -> <game>\settings\ui\caravan\uniq_plate_*.tex
```

Nothing on the left ships. The package is the `.asi` and three documents; `data\oracle\` is the
harness's and is left out of both the zip and the public tree (`tools\package.ps1`,
`tools\publish.ps1`). `data\oracle\ORACLE.txt` gives the order to rebuild it from a game folder.

**The oracle is a 1.3.0.8 artefact.** It was cut from a 1.3.0.8 installation, so the harness
must read one: `UNIQUETAB_TEST_GAME_DIR` names a 1.3.0.8 game folder, not whatever the installed
game has become - a later patch changes the database, and every byte comparison then fails by
design. That folder needs `x64\` (Game.dll, for the bindings suite), the four databases, the
`Items`, `UI` and `Text_EN` archives of `resources\` AND of each `gdxN\resources\` (the
expansions' items take their bitmaps and names from there; without them the catalogue loses
about half its items), and at least one custom game under `mods\`. To move the oracle to a newer
game, rebuild all of `data\oracle\` from that game with the Python tools and review the diff.

The Python tools under `tools\` are the **reference implementation**. The DLL does the same work
itself at run time, and the contract is that **the four text files it writes are byte-identical to
the Python ones and the overlay it writes decodes to the same records** - not the same bytes: an
LZ4 block has many valid encodings and the C++ writer stores literals where `tools\arzw.py`
compresses, which costs about 4% of file size and nothing else. `tools\test_catalogue.cpp` (via `build_test_catalogue.bat`) is the oracle: it runs
the C++ generator over a real installation and compares all four outputs byte for byte against
`data\oracle\`. If you change either side, that test is what tells you the other side moved.

Two traps that have already bitten:

- The fixtures are **CRLF**, because that is what the DLL writes. `.gitattributes` marks
  `data\oracle\` as `-text` so git stores the real bytes. Undo that and a fresh clone fails all
  three text comparisons.
- The stamp. `ensureOutputs` fingerprints every input archive (size + mtime of the four `.arz` and
  the Items / UI / `Text_<lang>` `.arc` of the base game and each expansion) into
  `catalogue.stamp`. A matching stamp costs one stat per archive. Regeneration writes everything
  to temporary names, re-reads `catalogue.bin` through the model loader, and only then renames all
  four into place. On any failure the folder keeps the files it already had.

`uniq-pages.arz` is written by the same pass. `checkPageCapacity` is the read-back now: the
archive that was just written is re-opened and must hold the frame, its box count, and every box
record the text names, before anything is renamed into place. A failure anywhere leaves the mod
folder exactly as it was - empty on a fresh install, and on an upgrade the previous generation's
overlay and the text that matches it.

**The plates are generated too.** `src\gen\plate_gen.cpp` is the port of `tools\make_plates.py`
and the contract there is stricter than the overlay's: for the same game files the two produce
**byte-identical** files, on the vanilla view and on both modded ones. The set is painted after
the geometry and the layout are known, one surface at a time, and written into the engine's own
texture override root `<game>\settings\ui\caravan\` - the only place a relative `ui/caravan/...`
name resolves from, since `LoadTexture` hands back a 64x64 placeholder for an absolute path. Only
files named `uniq_plate_*.tex` are ever written there, a plate whose bytes are already on disk is
not rewritten, and each goes down as `.tex.tmp` first and is renamed with the rest of the
outputs, the stamp last.

**The ground rule.** The ground a plate is painted on is what a NORMAL STASH TAB shows under the
same screen rectangle: the stash cover (`caravan_window` -> `TransferWindow` -> `TransferPlate`,
i.e. `caravan_transfercoverimage.dbr`) through the merged view - never the materials cover, whose
art is Crate's 24 hand-placed frames on vanilla and a whole baked reagent cluster under X-Mod.
When the two covers cover the same rectangle it is that whole texture (vanilla, X-Mod); when they
do not, the stash cover's pixels under the materials cover's rectangle, translated through the
two windows' `WindowLocationX/Y` and the two `bitmapPositionX/Y` (Baul: a 346x625 cut of the
952x1007 whole-window art at 605,71); and when that rectangle is not inside the stash cover, or
the surface is one the reader does not handle, nothing of it is used, with the reason in the log.
The lattice erasure and the ornament pass are measured on the vanilla texture and run only when
the ground IS that texture. `plate_gen.h` states the rule; `make_plates.py`'s header is the
authoritative description of the painting itself.

**A modded ground is a flat fill.** Whenever the rule above does not land on the vanilla stash
texture, not one pixel of the mod's art is copied: the plate is filled with a single opaque
colour and the frames are stamped on that. A mod's cover carries its own baked box lattice
(X-Mod's is 32 px) that our frames are not aligned to and cannot be, so the two grids interleave;
the mod's own window frame surrounds the page anyway. The colour is the vanilla ground's own
tone, sampled at generation from the vanilla `ui/caravan/caravan_transfercoverimage.tex` at
312,180 - the centre of a lattice cell and the calmest such point on the texture - which reads
`060606`. `plate_ground` in `uniquetab.ini` (six hex digits `RRGGBB`) replaces it; it is part of
`catalogue.stamp`, so changing it regenerates the set once, and it never touches a vanilla plate.

**There is no shipped fallback.** A fresh install has no plate until the first generation paints
the set; a plate that is missing, or not the size the live cover wants, is rejected by
`ut_plate`'s size check and the plain cover draws. A folder that cannot be written is one WARN
line naming it, never a failed generation. An install of an earlier version keeps the plates it
already has until the generation replaces or confirms them.

**The fresh install is proved offline.** `test_catalogue --ensure` starts from an EMPTY mod
folder and an EMPTY override root and requires the first launch to leave exactly the six outputs
in the one and the seven vanilla plates in the other - the lists byte-identical to the oracle,
the overlay record for record the oracle's, the plates byte-identical - and the same first launch
over a folder with no archives to leave both empty.

**The outputs are per view.** `gen::ensureOutputs` takes a `gen::ViewSpec` - `vanilla`,
`basemods`, `custom-<name>` or `basemods+custom-<name>` - builds the override layers in the
engine's own order (the basemod layer, then the custom game's on top) and fingerprints the custom
game's `database\*.arz` and `resources\*.arc` into the stamp beside the game's own archives.
Stamp format 7. The start-up view writes where it always did, `<mod folder>`; a custom view
writes the same six outputs to `<mod folder>\views\<name>\`, so an installed copy that never
enters a custom game is unchanged and leaving one costs nothing. A plate carries the view's tag
when it has one (`uniq_plate_64x96_346x625_lootascension.tex`); `ut_plate.cpp` builds that name
from the live view and `make_plates.py --plate-tag` is the reference for it. The vanilla and
`/basemods` names are exactly what they were, because installs of earlier versions already hold
their plates under those names.

`test_catalogue --view` asserts the **written** records, not the generation report: it reads
`uniq_frame.dbr` back out of both archives (the Baul view: `513,70`, 144 boxes, 115 pages, every
page box on the derived `128,76` 310x550 grid; the start-up view: `0,88`, 160 boxes on `105,76`).

The readers **stream**. An `ArzArchive` or `ArcArchive` keeps its string, record, part and
entry tables - the index - and holds the file open; bodies are read and decompressed one at a
time. Nothing the size of an archive is ever resident, and `--ensure` fails above a 200 MB
peak so it stays that way: a Steam Deck cannot spare the 1.7 GB the first version needed.
`--ensure` also generates the `/basemods` start-up view against the real game folder, and fails
when that folder's `mods\` holds no custom game, so the budget is proved on the layout players
have. In the game the process working set cannot isolate the generation - the engine builds its
main menu on the other threads in the same seconds - so `src\gen\heap_meter.cpp` counts the
heap the generating thread holds (it replaces the module's global `operator new`/`delete`; off
that thread they are `malloc` and `free`). `N MB for this` in the `catalogue: built` line is that
count, and a debug line beside it gives the process working set and peak before and after.

## 8. The journal format

`uniq-items.jsonl` (and `-hc`), JSON Lines, UTF-8, LF. The full spec is the file header of
`src\ut_rescue.cpp`; this is the shape.

**Where the files are.** One set per (collection mode, custom game):

| view | journal | exports, prune copies, rescue report |
| --- | --- | --- |
| `vanilla`, `basemods` | `<journal dir>\uniq-items.jsonl`, `-hc` | beside it; the report in the mod folder, as always |
| `custom-<name>`, `basemods+custom-<name>` | `<journal dir>\views\custom-<name>\uniq-items.jsonl`, `-hc` | beside it, report included |

`<journal dir>` is the mod folder, or `journal_dir`. The format is the same in every file. A
row is written to exactly one of them - the one the deposit's world had open - and nothing sweeps
`views\`: it holds user data (neither the generator, which only deletes its own `.tmp` names,
nor `undeploy.bat`). `ut::journalFollow(mode, viewKey)` is the one switch; the key is
refused unless `journalViewKeyFor` could have produced it (`[a-z0-9._-]`, no `..`). A rescue
empties the active view's journal only, so a custom game's collection is handed back from inside
that custom game. `tools\journal_guard.ps1` checks every set, each against the
`rescue-report.txt` of its own folder, and refuses on the worst of them.

```
line 1   {"journal":"grim dawn uniquetab","format":4,"written":"<ISO-8601 UTC>",
          "entries":N,"tableCopies":N, ...}
line n   one stored item, flat, keys in canonical order:
          "record"      the lower-cased DBR path - THE KEY (case-insensitive, last wins)
          "deposited"   ISO-8601 UTC, for the human
          "count"       format 4: how many copies the PRIVATE TABLE holds (absent = 0)
          "stack"       u32, the box count
          "flags"       u32 bitmask (soulbound / untradeable / synthesized) - AUTHORITATIVE
          "flagsText"   a comment. The reader ignores it
          "len"         the ItemReplicaInfo length (400 = 0x190 on 1.3.0.8)
          "<name>@OOO"  one per recorded std::string slot, INCLUDING empty ones; OOO is the
                        3-hex-digit offset inside the replica. The name is a comment
          "raw"         the rest of the replica: space-separated OOO:XXXXXXXX (hex offset :
                        hex little-endian u32). Only non-zero words, only words outside every
                        recorded slot window
```

**The replica blob.** Each entry carries the item's full `ItemReplicaInfo` - `0x190` bytes on
1.3.0.8, and the length is *decoded*, never assumed (`ut_replicasize.h`, two independent sources
that must agree). It is captured on the game thread, before `AddItemToReagents` builds its own
zeroed copy. Every MSVC `std::string` slot inside the blob is **deep-copied into the file's own
string table**, because a raw copy keeps SSO strings but leaves every heap-allocated one dangling.
That is what makes the file self-contained.

**Reconstruction order is fixed:** zero the `len`-byte buffer, apply every `raw` word, then (in
`identityBuild`) rebuild the slots. Do not reorder it.

**The version gate.** `UT_JOURNAL_FORMAT` is 4. A reader that meets a **higher** number loads what
it can and then goes **read-only for the session**. This is not cosmetic: `entryFromKVs` skips
unknown keys by design, so a format-3 build would read a format-4 file happily, lose every
`count`, and write them away on its next deposit. Bump the number whenever the *meaning* of a
field changes. A lower-numbered file is read in full and upgraded by the next write, never
rejected.

**A file that is there and cannot be used is never treated as empty.** Share-locked, truncated,
over the size cap, unreadable header, a failed migration verify - all of them mean read-only, and
nothing is written or migrated on top. "I cannot read it" must never become "there is nothing in
it".

`"len":0` is legal and keeps the entry: it is the honest shape for a record the engine still holds
whose prototype could not be read. `identityBuild` refuses a zero-length blob, so the entry is
inert for identity and exists only to say "this record is in the collection" - which `journalHas`,
`journalCount` and `tools\journal_guard.ps1` all depend on.

Every bound in the reader is written so it **cannot wrap** (`off > len || len - off < 4`, never
`off + 4 > len`): the `raw` scanner accepts eight hex digits, so a hand-edited `0xFFFFFFFC` is
reachable, and the wrapping form let it through into a `memcpy` far past the buffer.

## 9. Tests

Eight offline suites. **All eight must be green before any commit that touches `src\` or
`tools\`.** Run them sequentially - they share build output directories.

| suite | what it covers |
| --- | --- |
| `tools\build_test_config.bat` | the settings table: every row's default matches its member, the writer, the parser, the clamp, and the path rules (including the two `.asi` placements) |
| `tools\build_test_journal.bat` | the JSON Lines reader and writer, the format gate, the migration, the wrap-proof bounds |
| `tools\build_test_store.bat` | the private table |
| `tools\build_test_rowfold.bat` | the row and page arithmetic |
| `tools\build_test_tooltip.bat` | the text fold and the rollover line |
| `tools\build_test_bindings.bat` | every pattern scanned over a decrypted exe image: exact hit counts, the 1.3.0.8 RVAs, the decoders, and the class/phase/gate classification row by row |
| `tools\build_test_catalogue.bat` | the whole generator end to end against a 1.3.0.8 game folder: all four outputs byte-compared with `data\oracle\`, the overlay record for record against `data\oracle\uniq-pages.arz`, the derived geometry, the painted plates on all three views (`--plates`: byte-identical to `data\oracle\plates\` on vanilla, a flat ground with nothing but our own frame lines on the two modded ones), a fresh install into empty folders, a custom game's view, and its peak working set against a budget |
| `build_model.bat` (repo root) | `tests\model_test.cpp` - the pure model |

Three environment variables:

- `UNIQUETAB_TEST_EXE_IMAGE` - a **decrypted** `Grim Dawn.exe` image (the shipped exe's `.text` is
  DRM-encrypted on disk, so the patterns cannot be found in it). Needed by `bindings`; the RVAs it
  asserts are 1.3.0.8's, so the image is a 1.3.0.8 one.
- `UNIQUETAB_TEST_GAME_DIR` - **a 1.3.0.8 game folder**, opened **read only**. Needed by
  `bindings` (Game.dll) and `catalogue` (the archives). The oracle is 1.3.0.8's (section 7), so
  this is not "the installed game" once the game has been patched past it; a copy of a 1.3.0.8
  install with the parts section 7 lists is enough.
- `UNIQUETAB_TEST_MODS_DIR` - the unpacked stash-UI mods (`xmod\X-Mod`,
  `lootascension\LootAscension`) the modded views are generated from. Optional: without it those
  views are skipped and say so.

Without the fixtures - the public tree carries none of `data\oracle\` - `catalogue` and
`build_model.bat` cannot compare anything; `data\oracle\ORACLE.txt` gives the order to rebuild
them from the game folder first.

And one the mod itself honours: `%UNIQUETAB_OUT%` overrides the mod folder. It is not a fallback -
it is how each harness gets its own empty directory, and it is read before anything else so a test
can never reach a real installation.

No test launches the game. None writes outside `build\test\`.

Also useful: `tools\journal_guard.ps1`, which checks both journals of the start-up view and of
every `views\custom-<name>\` set, and is what `undeploy.bat` consults before it removes anything;
`build_test_journal.bat` drives it over a custom game's journal (refuses) and over none (allows).

## 10. The menu smoke procedure

The offline suites cannot tell you the mod loads. This does, without a world and without risking a
save. **Do it by hand, not with `deploy.bat`**, so the archive-and-rollback story stays yours.

1. Confirm the game is not running.
2. Back up whatever is already in the game's `x64\` folder, by hash, and note where the backup is.
3. Copy in the loader's `dinput8.dll` and `bin\uniquetab.asi`. Leave the `uniquetab\` data folder
   where it is. Only one loader in the folder - two would load the plugin twice.
4. Launch, **sit at the main menu about 50 seconds**, then kill the process.
5. **Kill it by NAME**, not by the handle you started it with. Launching through Steam hands off
   to a second process, so the object you started is not the game; killing that one leaves the
   game running.
6. Read `x64\uniquetab\uniquetab.log` and check:
   - the banner, then `mod dll = "...\x64\uniquetab.asi"`;
   - `bindings: ... confirmed` for every class, and the late report as well;
   - `detours installed: N of N`;
   - `READY: exports resolved, detours installed`;
   - the overlay went in - either `Engine::LoadDatabase("...uniq-pages.arz") via LoadMainDatabase
     detour -> OK`, or `... via game-thread fallback -> OK` preceded by `LoadMainDatabase detour
     never fired (checksum ... already set)`. With a `dinput8` loader the **fallback** line is the
     expected one; with a `winmm` loader the **detour** line is;
   - no `[E]` and no `[W]`.
7. Repeat with the other loader variant (delete the first one first).
8. Restore the folder from step 2 and hash-verify.

## 11. Open questions

Two that were here are answered and have moved into the text above: **which database the
generator generates for** (section 4's "The view" - the start-up view from the command line, a
custom game's from the path its own database load names, and a custom layer sits ON TOP of a
basemod one rather than replacing it), and **which page our frame is handed back for under a mod
that overrides both caravan windows** - exactly one record is ever substituted,
`records/ui/caravan/caravan_materialwindow.dbr`, compared by length and `_stricmp` in
`hk_LoadTableFile`; `caravan_relicwindow.dbr` is a different path and never reaches the
substitution, the generator never reads it, and the captured window is caravan page 3, the
materials slot. Baul overriding both windows changes neither.

Closed in 1.1.0: **the shipped overlay and plates.** Neither `uniq-pages.arz` nor a plate ships
any more - the package is the `.asi` and three documents, `deploy.bat` copies the `.asi` alone,
and the harness proves the first launch builds the complete set into empty folders (section 7).
Their reference copies are fixtures under `data\oracle\`, outside the package and the public tree,
and `THIRD_PARTY.md` says nothing Crate-derived ships.

Still open, and listed so they are not rediscovered:

- **The filler boxes still carry a real item.** The frame's boxes beyond the ones the materials
  page opens with are parked at -4000,-4000 and name the first vanilla box's reagent, because it
  has never been established that the engine's box `Load` accepts an empty `reagentName`. If it
  does, the fillers could name nothing and the overlay would shrink slightly.
- **The plates cannot be loaded out of the mod folder.** `GraphicsEngine::LoadTexture` resolves a
  name through the engine's resource roots; given an absolute path it hands back a 64x64
  placeholder, not null. That is why the plates are written under `settings\ui\caravan\` in the
  game root and why `ensurePlate` accepts a mod-folder texture only at the plate's real size. The
  clean way to keep everything under `uniquetab\` is to mount the mod folder as a resource source
  (`FileSystem::AddSource` / `AddSourceArchive` are exported) before the first plate load -
  untested.
- **`data\oracle\catalogue.json` is absent** from the tree (4 MB of derived JSON).
  `build_model.bat` skips the repack when it is missing, and `build_uniq_db.py` needs it for a
  full rebuild; `data\oracle\ORACLE.txt` makes regenerating it step one. The public tree carries
  none of `data\oracle\` and none of `tools\exports\` (game-derived), and the README.txt left in
  each place says how to recreate them from a copy of the game.
- **`tools\build_catalogue.py` still writes `catalogue-stats.md`** into `data\oracle\`. Keep it in
  step with the generator or stop emitting it.
- **The fixtures are still committed files.** `data\oracle\` holds item names, record paths
  and caravan art extracted from a 1.3.0.8 game (`THIRD_PARTY.md` says so). They could become
  committed sizes and hashes instead, with the harness comparing against a freshly rebuilt set.
- **The oracle needs a 1.3.0.8 folder, and 1.3.1 replaces it.** The 1.3.1 beta rewrites
  expansion archives the catalogue reads (by their file times: `gdx1\resources\Items.arc` and
  all of `gdx3\resources\`), so once the installed game has taken it, the only way to keep the
  catalogue suite's byte gates is a kept 1.3.0.8 copy with every part section 7 lists; the other
  way is to re-cut the whole of `data\oracle\` on 1.3.1 and review what moved. `build_catalogue.py`
  asserts the 1.3.0.8 relic and collection counts (`RELIC_EXPECT`), so it refuses any other or
  incomplete database outright - that assertion moves with a re-cut.
- **The packaged build is a person's to QA.** The first-launch generation, the stash-UI mods and
  the custom games were exercised in a world on the development install; a clean install from
  the 1.1.0 zip itself has not been, and neither will any later version's be by the offline
  suites.

## 12. What NOT to do

- **Do not write the game's save files.** Not `player.gdc`, not any `.gst`, not `options.txt`. If
  a change could possibly do this, it does not ship.
- **Do not put the mod's records into the engine's reagent map.** That is the one-way door of
  section 1. The private table exists to keep them out of it.
- **Do not install a hook before the gate.** `bindingsGate()` runs first, and a failure means
  *nothing* is installed. Do not add a detour that installs itself earlier "because it is
  harmless".
- **Do not hard-code a code address.** No RVA, no absolute. A field offset inside an object the
  mod already proved it holds is acceptable *with* a plausibility test; a code address is not.
- **Do not compare against a game version.** The exe does not carry a usable one, and a version
  test would refuse a build that works and accept one that does not. Ask the capability question.
- **Do not ship a generated file.** `catalogue.bin`, `catalogue.stamp`, the three `uniq-*.txt`,
  `uniq-pages.arz`, a plate, the ini, the log, the journals, anything under `data\oracle\`. Ever.
- **Do not disable hooks in `DllMain`.** `MH_DisableHook` / `MH_Uninitialize` suspend every thread
  in the process; under the loader lock that is a textbook deadlock, and unmapping while a detour
  is live jumps into nothing. The detours are deliberately left in place until process exit.
  `hooksRemove()` exists for a future explicit, non-`DllMain` shutdown and is not called.
- **Do not call shell32 from `ut_paths.cpp`.** No `SHGetFolderPathW`, no `SHGetKnownFolderPath`,
  no `CoInitialize`, no `LoadLibrary`. `utModDir()` is first called from `DllMain`, under the
  loader lock. Documents is reached through the environment instead.
- **Do not let a "cannot read" become an "it is empty".** Applies to the journal above all, but it
  is the general rule: fail closed, say why, and change nothing.
- **Do not accept a deposit whose source removal you cannot prove.** The private table answers
  "taken" only to a caller whose own removal path has been disassembled: the cursor drag and the
  bag shift-click. Adding a caller means proving its removal, not assuming it.
- **Do not launch the game from an automated run**, and do not deploy from one. Copy files by
  hand, or use `deploy.bat` knowingly. Nothing automated should write inside a game, Steam,
  Documents or GD Stash folder.
