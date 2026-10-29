# HANDOFF - the maintainer's handbook

Unique Collection Tab is a Grim Dawn mod that turns the caravan's Crafting Materials page into a
collection of unique items (and, by default, of monster infrequents and blueprints): an item
dropped on one of the page's boxes is kept in the mod's own journal, and handed back the same way.
This file is for whoever maintains it next - after a game update, or to add a feature - and has
never seen this code. It describes what is here, why it is shaped this way, and the rules that
must survive any change. Read sections 1 and 2 before you touch anything.

The mod is C++17, MSVC, built into `bin\uniquetab.asi` - a plain DLL with the extension Ultimate
ASI Loader looks for. It exports nothing. `README.md` is the player's document; this one is the
code's.

- **The version** is written in one place, `src\ut_version.h` (`UT_VERSION`, "1.2.1").
  `tools\package.ps1` reads it for the zip's name, and the start-up banner prints it at the top of
  every session in `uniquetab.log`, `=== unique collection tab 1.2.1, built <date> <time> ===`
  (`logIdentity` in `src\dllmain.cpp`). The settings file has its own number, `ini_version`
  (section 3, "The settings file and its versions").
- **The game.** The mod runs on Grim Dawn 1.3.0.8 and 1.3.1.1, 64-bit. It checks the game's
  shape, never its version (section 5). Its offline evidence - the oracle and the bindings
  suite's expected values - is 1.3.0.8's (sections 7 and 9). After a game update, follow the
  checklist at the end of section 6.
- **Build.** `build.bat`, from any folder: it finds the x64 MSVC 2022 toolchain itself
  (`tools\find_vcvars.bat`), compiles the mod's own sources with /W4 /WX (MinHook without /WX)
  and writes `bin\uniquetab.asi`.
- **Test.** Sixteen offline suites, section 9, which also names the inputs they read. All sixteen
  must be green before a commit that touches `src\` or `tools\`. No suite launches the game.
- **Deploy for a test.** `deploy.bat [<game folder>]` copies the `.asi` into the game's `x64\`
  (the game is found through the argument, `%GD_DIR%` or Steam's registry entry; Ultimate ASI
  Loader must be there already) and keeps the `.asi` it replaces under `bin\history\`.
  `undeploy.bat` takes the mod out again and refuses while a collection would be left behind
  (`tools\journal_guard.ps1`). Section 10 is the by-hand menu smoke that proves a build loads.
- **Release.** Raise `UT_VERSION`, add the release to `CHANGELOG.md` and to the README's "New in"
  section, build, run the suites, then: `python tools\release_check.py` must print CLEAN (no
  absolute path, no trace of a machine and no development history in any tracked file;
  `--strict` / `--list` also print the words a person should read, and personal patterns go in
  the gitignored `tools\release_check.local`); `tools\package.ps1` writes the player's zip to
  `out\` (`x64\uniquetab.asi` and the four documents); `tools\publish.ps1` writes the public source
  tree beside the checkout (`..\gd-uniquetab-public`: the tracked tree with this file, minus what
  is derived from the game and minus the two release scripts) and runs `release_check.py --root`
  over it; then an annotated tag, `git tag -a v<version> -m "Unique Collection Tab <version>"`.
  `tools\release_check.py` and `tools\publish.ps1` are not in the published tree; a maintainer who
  starts from it has `package.ps1` and the tag, and keeps their own check that no absolute path
  or personal token is in what they publish.
- **The one rule** is section 1: an item must never be lost or duplicated.

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
   `uniquetab.state`, `uniquetab-buttons.ini` (written once per game when absent, populated with
   that game's groups; only its `[unassigned]` section is ever rewritten by the mod, and a
   built-in group added after 1.1.0 gets its `[TAG]` section inserted once - `utbOfferNew`),
   `uniquetab.log` (+3 archives), `uniq-items.jsonl` / `uniq-items-hc.jsonl`, `uniq-export.gds` /
   `uniq-export-hc.gds`, `uniq-export.csv` (optional), `catalogue.bin`, `catalogue.stamp`,
   `uniq-records.txt`, `uniq-pages.txt`, `uniq-groups.txt`, `uniq-pages.arz`, `rescue-report.txt`,
   and, for each custom game that has been entered, ONE folder `mods\<leaf>\` (`<leaf>` = the
   game's own folder name under `<game>\mods\`; `ut_folders.h`): the game's journals, exports,
   prune copies and rescue report, its own `uniquetab.ini` and `uniquetab-buttons.ini`, and the
   generated set + `uniquetab.log` (+3 archives) + `uniquetab.state` of the view `custom-<leaf>`;
   the view `basemods+custom-<leaf>` has its generated set, log and state in
   `mods\<leaf>\basemods\`. (Older builds kept these in `views\custom-<leaf>\` and
   `views\basemods+custom-<leaf>\`; `utFoldersMigrate` renames them once, at the worker's start.) It
   never writes `uniquetab-groups.ini` (the player's, optional); it only RENAMES two retired files:
   an unedited stock copy of the groups file to `uniquetab-groups.ini.old`, and an old
   `uniquetab-text.ini` to `uniquetab-text.ini.old` after folding its words into the first
   buttons file (`.old1` .. `.old9` when taken).
   Never a `.gst`, never `player.gdc`, never `options.txt`, never anything in the game folder
   proper - with the one documented exception of `settings\ui\caravan\uniq_plate_*.tex`, which
   section 7 explains and which is the only name pattern ever written there. A user grid adds its
   own set beside it, `uniq_plate_<cell>_<cover>[_<tag>]_L<hash8>.tex`; only names of that form
   for the same cover and tag with another hash are ever deleted there.
2. **Never lose or duplicate an item.** A deposit that cannot be recorded is refused *before* the
   engine moves anything. A refusal is a true no-op: the item stays where it was. A monster
   infrequent record holds ONE roll: the journal row keeps one identity per record, so a second
   roll would replace the first. The deposit gate refuses it (`ut_miroll.h` `utRecordCapDecide`,
   before the max_per_record test) and `journalDepositCommit` refuses count 2 of a known MI as a
   belt.
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
   package is `uniquetab.asi` and four documents (README, CHANGELOG, LICENSE, THIRD_PARTY).
   Their reference copies belong in `data\oracle\` as test fixtures, which neither the package
   nor the public tree carries.
7. **Nothing in the tree names a machine.** No absolute path, no user profile folder, no account
   id, in code, comments, scripts or documents; the mod finds everything relative to the game
   folder and the plugin's own location.
8. **No bound a collection can reach is a fixed number.** A buffer, cap or walk that holds one
   entry per journal row, catalogue record or engine map node is sized from the count it holds
   and grows with it (`src\ut_caps.h`, section 4 "Every bound is sized from the data"); the only
   fixed numbers left are ceilings that protect memory or end a walk over a corrupt structure,
   each with its bytes stated and each far above Grimarillion's 19,089 records. A function that
   copies rows into a caller's buffer returns the WHOLE count (`journalCollectStored`,
   `reagentCollectNodes`), so a too-small buffer is visible and never a wrong number. A fixed
   4,096 counted a 7,169-record collection as 4,096, and which 4,096 followed the order of the
   journal's lines.

## 3. The architecture, by file

### Start-up order (`src\dllmain.cpp`)

`DllMain` does the minimum: resolve the mod folder, open the log, install the fault watchdog
(a vectored handler that only *logs* an access violation with this DLL on the stack and always
returns `EXCEPTION_CONTINUE_SEARCH`), start one worker thread, return. Nothing that can block runs
under the loader lock.

The worker then does, in this order:

```
configReload(ini)      log_level must be in force before the export lines are written; also
                       reads the start-up view's uniquetab.state
waitForGameModules()   Engine.dll + Game.dll, 60 s ceiling
resolveExports()       by decorated name; a missing REQUIRED symbol turns the mod off
generateEnsure()       catalogue.bin, the three lists, the overlay and the plates, current
                       with the game's archives; then one layout-plates pass for the start-up
                       view and their own thread (section 7, "Layout plates")
panelInit()            loads catalogue.bin
reagentInit()          exports, the craftingMaterial offset, the record lists, the arz path
tooltipInit()          the seven rollover exports; every part optional; the words' first look
                       (panelTextStartup, the rollover lines; nothing is written - a game's
                       buttons file waits for that game's group table)
bindingsGate()         <- ALL OR NOTHING. A failure here returns before any hook exists
hooksInstall()         MinHook: 7 core detours + the reagent set; the 6 tooltip ones separately
                       (the late ones - the screen pad's DirectInput pair and the HUD item
                       rollover's Draw - are ut_plate's, from plateTick, in a world, only while
                       pad_screen or label_screen is set; the DirectInput pair also while the
                       layout editor is open)
READY
```

then a once-a-second loop: re-read the ini (and the active custom game's own file), poll the
buttons file of the game in force (`panelTextFileTick`), the late-load tick, flush the log, a
heartbeat, and a frames-not-advancing stall check (which also opens the named event
`CRASHREPORT`, because a
"freeze" is usually the game's own crash reporter waiting behind a fullscreen window). Its verdict
is `src\ut_startorder.h`'s: CRASHED only when that event exists, and "no frame yet" while at most
one hooked frame was counted - a slow first start is not a crash.

**No engine resource load at the main menu.** The plates (and the plain cover fallback) are
loaded through `GraphicsEngine::LoadTexture` only once a materials window has been captured in a
world (`utPlateProbeAllowed`), where the engine loads its own plate through the same call. On a
fresh install the generation delays the hooks, so their first frame can fall in the engine's
start-up streaming, and a probe there can be followed by a read fault on the engine's loader
thread in `LZ4_decompress_fast+0xA0` (Engine.dll+0x278A10, called only from the unexported
GAME::Archive part reader at Engine.dll+0x39440, between `Archive::Close` and
`Archive::FindMatchingFiles`), after which the game presents no further frame. The overlay's
`Engine::LoadDatabase` stays on the first frame.

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
| `hooks.cpp/.h` | MinHook. 7 core detours: `Engine::PresentSurface` (the frame tick), `GameEngine::Update`, `GameEngine::SetTransferOpen`, the two `InventorySack::AddItem` overloads, `InventorySack::RemoveItem`, `WinWindow::WindowProc`. The late detours are ut_plate's (the screen pad's `DirectInputDevice::GetMouseEvent` / `::Update` pair and the HUD item rollover's Draw), installed from `plateTick` |
| `gd_runtime.cpp/.h` | every engine function resolved by name, and the opaque handles passed around |
| `gd_exports.h` | generated from the real export tables. **Zero addresses.** |
| `gd_exports_extra.h` | the same, hand-kept, for the few names the generator does not select |
| `gd_exports_reagent.h` | generated, the reagent/item half |
| `ut_paths.cpp/.h` | **the only** place that decides where files live. Never call shell32 from here |
| `ut_log.cpp/.h` | the line logger; buffered, five levels, 3 rotated archives; while a custom view is active every line also goes to the view folder's `uniquetab.log` (`mods\<leaf>\`, `mods\<leaf>\basemods\`) (`logTeeOpen`/`logTeeClose`: its own buffer, rotation and 4 MB cap; the main log keeps every line) |
| `ut_config.cpp/.h` | one table of settings. The parser, the written file and the test all read that table. Four visible keys under `[general]` (`enabled`, `collect_mi` since ini_version 40 - default 1, member `collectMi` -, `tooltip_mark`, `log_level`; `max_per_record` and `rescue` are hidden "(untested)" rows of the blob since 41), the `[layout]` families (`grid_/pad_/label_<W>x<H>`) and two keys written only while set (`kUtCfgWhenSet`: `pad_screen`, `label_screen`, checked by `utScreenRectParse`, one pair per settings file), every other key one commented row of the `[advanced]` blob at the end (a hidden key set by hand is live in its own row; a hidden one-shot goes back to its commented row). The dropped / merged / derived keys are not rows (their members keep fixed values; a merged one with a value is one WARN naming its new home). Per-game files `mods\<name>\uniquetab.ini` since 42: the SAME template (`configRenderView` = `configRender`'s renderer with a game header and `journal_dir` as a commented "main file only" line; one parser mode but a live `journal_dir` refused with a WARN), seeded once from the main file's values (`seedFrom`: one-shots at 0), NOTHING inherited afterwards - while a game is active `g_cfg` IS its file (`publishLocked`: `g_ovVals`, only `journalDir` from `g_base`), a missing key = the compiled default; a game file deleted while played is seeded again; a game file with NO `ini_version` line (the four-key shape of ini_version <= 41: four keys under `[game]`, a `[layout]` whose `default` hid a main line) is migrated once from the seed (`kParseViewOld`: its lines win, `default` drops the main line) and rewritten whole, while a complete game file of an older number (42 on: it names its `ini_version`) keeps every line it has and takes the COMPILED DEFAULTS for the rest (`loadOverlayLocked`, since 43 - its commented rows are hidden keys at their defaults and must not become the main file's values at a bump; `configViewIntFor` reads the same rule); `configViewIntFor` peeks a view's int key before it is active (the custom-game probe's `collect_mi`) - and the state file `uniquetab.state` per view folder (`mods\<name>\` or `mods\<name>\basemods\`; `uniq_group`, `uniq_row`, `owned_only`): `configSetView` creates and reads them on a view change, so `g_cfg` is always the active view's; `configPersistStr` / `configPersistInt` route each key to its file (a state key to the view's state, `journal_dir` to the main file, every other key to the ACTIVE view's settings file - a custom game's own while it is active) in one call; `configSetView` also maps the view's `log_level` into the logger; the one-shot `layout_edit` opens the in-game layout editor; `edit_button` (hidden, default 1, since 43) puts the pad's EDIT button on |
| `ut_generate.cpp/.h` | the DLL side of `src\gen`: keep the generated files current before anything reads them, and hold the active VIEW - which database the generated files describe, and therefore which folder they are read from. `generationInputs` hands the generator what is not an archive: `uniquetab-groups.ini` beside `uniquetab.ini`, and the view's journal pair (read only); and the LAYOUT PLATES - a user grid's own plate set, painted off the game thread (at start-up on the worker, then 1 s after a settings or view change on their own thread) under hashed names and published to ut_plate (`layoutPlatesState`/`layoutPlatesGen`); the probe that decides whether a custom game gets a view (`ut_viewprobe.h`), the per-view log and settings switched with the view; `collect_mi` into `GenInputs::collectMi` (`generationInputs`, read after `configSetView`, so each view's generation gets the value of ITS settings file: the main file's for the start-up view, the game's own for a custom game) and into the custom-game probe (`utProbeView(..., configViewIntFor(name, "collect_mi") != 0)`, before that view is active) - a flip regenerates the views of the file that flipped, once (each view's own `catalogue.stamp` carries its `collect_mi=1` line) |
| `ut_viewprobe.h` | does a custom game need a view: the caravan, or a record the catalogue's mod rule could collect (its record-local half; a pure header over `gen::ModView`) - with collect_mi=1 also a monster infrequent (`gen::isMiCandidate`, the generator's own function) |
| `src\gen\` | the catalogue generator: `arz_reader`, `arc_reader`, `catalogue_gen`, `pages_gen`, `generate`. `catalogue_gen` builds the catalogue from the VIEW - the base databases plus the override layers a `ModView` carries, names from the mod's own text archives, icons from its archives first; a base record is judged by the base rule (a slot-table class at Epic or Legendary - a relic at any classification - past the junk list, `isShipped`), so a view that overrides no item record writes the vanilla oracle's bytes; a mod layer's records get the mod rules (junk list, unknown class as `other`, a mod's own augments and components at every classification, unstorable classification skipped with a warning); blueprints - every shipped `ItemArtifactFormula`, base and mod alike - at every classification (Magical and Quest packed as Epic, `storable`; never the five sibling `*Formula` classes), their icon `artifactFormulaBitmapName`, else the rarity's scroll (`scrollOf`), never the crafted item's; `classWords` resolves the engine's 25 class type tags through the view's text (the game's word for a class, for the groups). THE MONSTER INFREQUENTS (collect_mi=1): `gen::isMiCandidate(key, cls, classification, fileDescription)` = `classification == "Rare" && isEquipmentClass(cls)` - every Rare record of the 25 equipment classes (`isEquipmentClass`: the 23 slot classes + `WeaponMagical_Staff` + `WeaponHunting_Spear`; `CatalogueItem::isEquipment` stays the 23), faction-store and crafted rares included, through the shipped filter / mod rule; every entry made gets the KIND mark (`isMi`, catalogue.bin flag bit 8, format 1 kept), Rare packed as Epic; `pages_gen` reads an MI as equipment to the group rules (`isEquipment || isMi` -> `kFlagEquipment`). `pages_gen` sizes the frame from the cover (`utFrameBoxes`, the F line's N). `generate.cpp` also holds the plates-only entry `ensureLayoutPlates` (its hash `layoutPlateHash` carries the lattice rule, `lattice=2`), the one-generation lock and the groups file's lifecycle (the built-in set, a user file merged in front, a stock copy retired) |
| `gen\ut_groups.h` | the groups as data: the groups-file parser and matcher, the merge of a player's file IN FRONT of the built-in set (`mergeGroupSets`: its rules tried first, its new groups first in the order, a built-in id given more rules keeps its place, its `[auto]` last), the class words (`classTypeTags` - the engine's class -> type-tag table -, `resolveNames`: a group with no label takes the game's word for its first class, plural, a family word on a clash, a derived tag; `viewGroupTable`: id/label/tag per emitted group, the buttons file's defaults), the auto groups (no Retired group), the journal count scan (the generator's count of owned records no page shows), the G line (`formatGLine` writes 9 fields, `parseGLine` reads 7 or 9), the reserved ids (`vanilla`, `owned`, `next`, `other`, `retired`, `auto.*`). A pure header; `tools\test_groups.cpp` links it |
| `gen\ut_groups_default.h` | the built-in groups template (the 24 slot groups - 1.1.0's set; `utbLateBuiltinIds` counts from it -, the Blueprints - `blueprint` / BLU, every `ItemArtifactFormula`, right after Relics; Crate's 11 test blueprints `blueprints/blueprint_*test*` under one `[exclude]` -, the two per-class groups `staff` / `spear1h` - empty in vanilla -, and the Reign of Terror / DeathGrimDawn data blocks; one `[exclude] rank = Rare, flag = equipment` block per game for its junk Rare gear - ROT NPC gear and fluffy, DGD cosmetics / playtest / skills / `f001a_sword`, GrimArmory `crafting_x*`, Grimarillion's tempest skills and `records/items/replicas/**`, DoM's -; the last section `[view:collect_mi]` - DGD's crossbow folders' Rare rule first, then one `rank = Rare` rule per slot group and per per-class group, 26 in all - live only while collect_mi=1 (generate.cpp's view key `collect_mi`), so with the flag off no MI is on a page; `build_uniq_db.py` reads the same raw string out of this header), compiled in and never written; its loader (`loadGroupsFile`: Default / Loaded - the player's file merged in front - / Refused naming the file and the line / Retired - a stock copy renamed `.old`), and beside it the stock-template test (`isStockGroupsText`) |
| `src\model\` | `catalogue` (the binary format; `ItemFlag::MonsterInfrequent` = bit 8 and the accessors `gdut::catIsMi(entry)` / `catIsMi(catalogue, record)`), `collection` (ownership), `layout` (rows and pages) - pure, no Windows |
| `ut_panel.cpp/.h` | the collection view: the active view's catalogue (`panelReloadCatalogue` on a view change, the ownership recounted over the live items), the ownership state fed by the sack detours, the group label, the category pad keyed by the BUTTONS of the game's buttons file (MAT first, one button per `[TAG]` - by default one per G line, its tag as the caption -, EDIT - the layout editor, logical G + 3, `edit_button` - just before OWN, OWN last; paged with a NEXT button when the captions would fall below size 6; laid out by `utPadFit` in the pad rectangle in force; the draw, the hit test, the click band and the hover all read one `PadPlan`) and the `layout_outline` preview. The label and the pad are drawn as UTF-8 through the wide text call in the ROOT font `fonts/savapromedium.fnt` unless the letters or a font line say otherwise, measured with the font the engine loaded once a per-world cross-check has proved the live metrics. The buttons file (`ut_buttons.h`): the game's file polled once a second by the worker, resolved against the game thread's table snapshot, written populated at first sight (the old `uniquetab-text.ini` folded in once), `[unassigned]` rewritten by the worker; on the game thread (`textWorldBuild`, after `ensureFonts`, once per world and when the file or the table moves) the pad keyed by buttons (`capTable`, `buttonOfGroup`, `padIndexOfGroup`, `panelButtonTarget` - a click selects the button's first group: a several-group button's page is ut_live's union -, `panelButtonGroupsOf` / `panelButtonMapGen` - the button map ut_live's unions read -, `panelPadButtonCount`, `panelButtonOfGroup` - a group's button in pad order, the order Ctrl+wheel walks), the per-group names, the label templates and words, the rollover lines; the role fonts follow the letters unless `[tab]` names one: the worker's `glyphSetsLoad` reads, once per process, the 15 FNTX glyph tables and the code points of the FNTD copies `zh` / `ja` / `ko` (`ut_sfnt.h`; their ~5 MB bodies dropped, only the code points kept) out of Fonts.arc, `textFontsDerive` runs `utbDeriveFolder` over them at each file load (root, the script's folder, the baked copies, zh/ja/ko, else zh with Windows filling the rest - one INFO, `utbDerivedFallbackText`), and `textFontsInArc` reads the KIND of every font a line names from the first 8 bytes of its Fonts.arc entry (`utFntdIs`: FNTD = dynamic; `fontKindSet` / `fontKindOf`, `RoleFont::kind` 1 FNTD, 0 FNTX, -1 not known) - never Fonts.arc on the game thread; the world-start fill (`roleFontsSync` / `buttonFontsSync`) runs the probe plan `utTmProbePlan` under one SEH guard (`fontCacheFillSeh`): a baked font fills sizes 1..64, a dynamic or unknown one only its nominal size (`roleNominalSize`), every other size filled LAZILY the first time it is measured (`roleTextPx` / `panelButtonCaptionTextPx` -> `fontCacheSizeSeh`, SEH-guarded) and its non-ASCII glyphs asked through `liveExtraGlyph` (a button's own font too) - both ONLY on the thread that ran `ensureFonts` this world (`g_fontTid`, the draw's thread; any other thread measures from the table, '?'); a role on an FNTD font is asked nothing by the glyph check (`TextGlyphProbe::dynamic`: every UTF-16 letter draws there, a letter above U+FFFF is refused) and says once per world which of its letters Windows draws (`dynamicRolesSay`, INFO); a font that faults or gives no style or no '?' leaves the role on the root font with one WARN (`kRoleFontNoStyles`); the glyph check WARN's advice is `utbFontAdvice` (pure, `ut_buttons.h`); per-button `size =` / `font =` and the search mark per button (`panelButtonCaptionSize` / `panelButtonCaptionFont` / `panelButtonCaptionTextPx` / `panelButtonSearchMarked`, a button marked when any of its groups is: `utUnionMarked`); the label prints `liveViewInfo`, which for a several-group button is the union's (its counts summed). The materials page (`padPlan`'s MAT branch, `matDecide`): `padmat_` or the pad in force drawn where it is over the live boxes, `panelMatPadAt` for the mouse slot. The pad and the label pinned to the SCREEN: the same draw with `screen = true` on an opaque ground with a 1-px frame, published for the screen input (`panelDrawScreenParts`). The in-game layout editor (the pad's EDIT button - `panelEditButton` sets the latch `g_editButtonAt` and `editTickDraw` opens the editor on the next frame -, `[keys]` editor_open bound to a key - `panelEditKey`, the same path with its own latch `g_editKeyAt` -, the editor chord on the pad or a pinned label - Ctrl+right-click by default, rechecked by `utKeyClickMatches` -, or `layout_edit=1`): drawn in place of the pad over a plain ground (a part dragged off the window is drawn on the screen and its grounds published through `plateScreenRectsPublish` so its clicks reach the editor), its clicks taken through `panelLayoutEditMouse` (CANCEL in every state but closed - mid-drag, while saving, with an item on the cursor; a LEFT DOWN during a drag whose release was missed ends that drag first, one DEBUG line), its drags followed by polling the cursor from the Draw tail (`panelLayoutEditDragging` tells the screen input that a drag is running, for the adopted drag), SAVE through one `configPersistStr` call (the view's file for `<part>_<W>x<H>`, the main file for `pad_screen` / `label_screen`). While the editor is open `editPublishScreen` publishes the caravan window's client rect with the grounds (`utEditWindowClient`): a click inside the caravan window reaches the editor through the window's mouse slot only, a click outside through the screen input only; in a world whose pad is pinned (the in-window pad's probe never runs) the window route is proven by the screen input's note of the same click (`utEditNoteProves`, `g_editWinProven`, once per edit; `editProofReset` at open and close). `panelTextStartup` / `tooltipTextRefresh` also carry `[tab] tooltip.roll` |
| `ut_reagent.cpp/.h` | the big one. The database overlay, the `craftingMaterial` gate, the deposit and take detours, the bag proof, the identity capture, the census and the crash diagnostics. `collectionAccepts` carries the monster infrequents' cap (`utRecordCapDecide`, asked only when the box already holds something: an MI box holding a roll refuses a second one through the existing refusal path, the reason naming the stored roll - name, level, prefix, suffix, seed); `reagentHandleMessage` is the `[keys]` site (section 4, *The keys are a table*): a step acts on its DOWN, the UP is only swallowed and only when that VK's DOWN was consumed (so one Ctrl+PageDown is one step); `reagentInit` logs `mi: the catalogue's KIND mark is BUILT IN`; `runRescue` writes the roll line |
| `ut_store.cpp/.h` | the private table: the mod's own `record -> {count, identity}`, and the display prototypes `showBox` points boxes at |
| `ut_live.cpp/.h` | the page itself: the group table (each G line's id and tag, `liveGroupTableGen`, the view's V count for the VERIFY line), the box capture (N boxes, the F line's count; one DEBUG line with the capture's time), the boxes' life (`liveOnHudGone` empties the list at `GameEngine::ExitPlayingMode` and on a view change; `liveTick` re-points only a complete capture of a living HUD - `ut_hudlife.h`) and the world-start verdict (`liveTabFacts`: the "tab is off in this world" WARN), the relayout, the layout in force (`liveLayoutGet`: the `[layout]` keys resolved against the live page, the grid re-fitted on a change with the first visible item kept; while a screen key is set the screen size and the window origin the Draw tail publishes - `liveSetScreenView` - join the layout's signature), the owned-only filter and the per-group owned counts (`liveGroupOwned`), the wheel and step input as `[keys]` matched it (`liveHandleWheel(ticks, buttons)` - the cursor test is the row wheel's whatever its chord -, `liveHandleStep(action)`; the button wheel and `button_prev` / `button_next` walk the pad's buttons in the buttons file's order - `utPadWheelButton` over `panelButtonOfGroup` -, one stop per button at its first group, the groups on no button none, one step per key press); one grid per button (`UnionTable` / `unionsSync`, from `liveTick` right after the layout in force: a button holding several groups is ONE page - a virtual `Group` of their entries concatenated in group order, its cell the smallest view cell holding every member's (`utUnionCell`), fitted to the grid in force - keyed by its FIRST group, the rep, so `g_wantGroup` / `g_curGroup` / `uniq_group` stay real group indexes and every per-group structure (the deposit gate, the take, the search tiers, the owned counts) accepts it unchanged; `g_unions` is published by pointer and never freed; `pageGroup` / `unionRep` / `onPage`; one INFO line per change of the unions, and a restart restores a union page's saved row); the pad pages BUTTONS (`panelPadButtonCount`); the layout is redone when the text measure changes (`panelMeasureGen`); the view change starts the new view on ITS state (`uniquetab.state`); `applyLayout` parks every box while the layout editor is open (`panelLayoutEditing`); at `zoom_pct` above 100 the cells in force (`zCellW/zCellH`) and the box widget's own size, written from its texture and re-asserted per tick (`boxZoomApply` / `zoomReassert`, section 4 *The zoom*) |
| `ut_plate.cpp/.h` | the page *background*: the captured `ReagentWindow`, the plate swap per group, the visibility test, the label draw, the search marks (a bitset over any number of groups); renames its cached plates on a layout-plates edge and releases superseded textures (at once, or at the world teardown when one may still be shown); the hit tests over all N captured boxes (vectors, `capturedBoxesAll`); the mouse slot hands the editor chord (`editMouse`: `[keys]` editor_open's click type with its modifiers held, `utKeyClickMatches`), and LEFT DOWN / LEFT UP while the layout editor is open, to ut_panel first, and on the game's own materials page keeps a LEFT or RIGHT DOWN inside the pad from the box under it (`utPadLeftDown`) and puts the handler's hovered-box answer back; the screen pad's INPUT (the `DirectInputDevice::GetMouseEvent` / `::Update` detours, resolved by name in DirectInput.dll and cross-checked against `Engine::GetInputDevice()`'s vtable, inert until the screen draw publishes its rectangles - `plateScreenRectsPublish` - except for an editor drag begun in the window, which it adopts); the pad click's action for BOTH pads, `padClickTarget` (MAT -> the game's own materials page, `liveSelectGroup(-1)`; every other button -> `panelButtonTarget`), from `scrButton` (the screen pad) and `mouseDecide` (the window pad); the screen parts' DRAW SITE (a detour on the HUD's item-rollover Draw, read from the live Hud around the captured caravan and proved by the HUD Draw's own call order, drawing the pinned pad and label at its head - one level below the tooltips); the draw site installed only while a screen key is set; the input pair also while the layout editor is open (so a player with no screen key yet can drag a part out of the window and reach SAVE on the screen), and once installed it stays for the session; `scrBody` NOTES to the editor a LEFT DOWN it passes untouched inside the published caravan window (`kUtPadActEditorNote` -> `scrEditor(kUtEvtEditorNote, ...)`), the window route's proof |
| `gen\plate_gen.cpp/.h` | the plates themselves: the TEX reader and writer, the ground rule, the 9-slice frame stamp - the port of `tools\make_plates.py`, byte for byte; the lattice is `utGridFit`'s cols x rows, every cell framed by `ut::utLatticeFrame` (cut at the plate's edge, never dropped; the row gap is an argument, the plan's F-line gap; `make_plates.py`'s `lattice_frame()` is the same rule line for line); `plateLeafName` is the one name rule (`uniq_plate_<cw>x<ch>[_<pw>x<ph>][_<tag>]`, and `uniq_plate_<cw>x<ch>_<pw>x<ph>[_<tag>]_L<hash8>` for a user grid's set) |
| `ut_tooltip.cpp/.h` | the item rollover: the compare popup (one byte, no detour) and the "already collected" line (six Game.dll detours over seven exports, all-or-nothing: `Item` / `ItemEquipment` / `ItemArtifact` / `ItemRelic` / `ItemArtifactFormula::GetUIDisplayText` and `GameTextLineToString`, the `GameTextLine` ctor called, not hooked; the export-alias check over all seven). A blueprint's own detour, `hk_FormulaGetUIDisplayText`, raises the per-thread `t_formulaDepth` around its trampoline (lowered in a `__finally`), so the crafted item's latch inside it is suppressed - counted as `innerSuppressed` on the status line - and latches the blueprint's own record on the way out (section 4, "The blueprint page and its tooltip"); `tooltipTestArm` (test only) plants stub trampolines so `test_tooltip` drives the real detour bodies; the two lines' text comes from `[tab]` of the buttons file (`tooltip.yes` / `tooltip.no`; the ini has no keys for them); a changed text is rebuilt into the spare buffer of a pair and published by pointer. A THIRD line, "In your collection (another roll)" - the collected text + `[tab] tooltip.roll` (default " (another roll)"), its own buffer pair, rebuilt when either changes -, when the hovered item is a monster infrequent whose roll (its replica's prefix +0x28, suffix +0x48, seed +0x68, read inside the SEH frame) differs from the stored one (`journalRollOf`, memoised 250 ms; `rollState`, never a catalogue load on the game thread); the heartbeat counts `anotherRoll=N` |
| `ut_rescue.cpp/.h` | the journal, the identity overlay a take is rebuilt from, the CSV and GDS exports, the rescue report, the rescue's plan (`journalRescuePlan`: a row the active database has no record of is skipped, never zeroed); the monster infrequents' side - `journalRecordIsMi` (the view's catalogue, loaded by the worker), `journalKindMarkBuilt`, `journalRollOf` / `journalRollText`, the commit's belt (count 2 of a known MI refused, one WARN), the rescue report's `roll handed back:` line. Pure file I/O - not one engine call |
| `ut_bindings.cpp/.h` | **every fact the mod knows about the game's memory**, in one place, with how it is obtained and how it is confirmed - and the text engine (Engine.dll, all advisory): the wide text call, `StyleManager::LoadFontDirect`, GraphicsFont2's style and glyph metrics with the decoded style size, the language and the game's own tags - resolved at the gate, each route off on its own when missing |
| `ut_replicasize.h` | `sizeof(ItemReplicaInfo)`, decoded from two independent sources that must agree |
| `ut_caps.h` | every bound a collection can reach, pure: the owned fold's walk buffer (`utTableWalk`: sized from the journal, grown on demand, 65,536-row / 17 MB ceiling), the engine-map node walk (`utCollectAll`: every node, 1,048,576-node walk ceiling) and the per-world prototype cap (`utProtoCapFor`: from the journal, ceilinged by the view's catalogue). `test_store --caps` proves it |
| `ut_depositgate.h`, `ut_pagegate.h`, `ut_paintgate.h`, `ut_ownedfold.h`, `ut_rowmath.h`, `ut_textfold.h`, `ut_fontmetrics.h`, `ut_gds.h`, `ut_padmat.h`, `ut_padpage.h`, `ut_startorder.h` | the pure-logic headers: a decision extracted from its engine context so an offline test can run exactly the code the mod runs (`ut_padmat.h`: the pad's joint band, the scaled-rectangle overlap test and the pad's click rule `utPadLeftDown` - inside the materials page's pad a button acts and any other point keeps the click, an item on the cursor is the engine's; `ut_padpage.h`: the pad for any number of buttons - logical buttons MAT / one per button / OWN / NEXT / EDIT (G + 3, drawn in the slot before OWN while `UtPadPages::edit`), and its pages; the wheel's walk over the pad's buttons in file order (`utPadWheelStops` / `utPadWheelButton`: one stop per button at its first group = a union's rep; one button per group = the group-order walk) and a several-group button's summed counters (`utPadButtonCounts`, the tests' reference); one grid per button - `utUnionPlan` (per group its rep, the union index, the members in group order), `utUnionRep`, `utUnionMembers`, `utUnionCell` (the smallest view cell, by area then width, that holds every member's cell; none -> the largest width by the largest height with `exact` 0: no plate, the plain cover), `utUnionLocate` (entry k of the concatenation), `utUnionRowOf` (the union row of a restored non-first member's row), `utUnionMarked` (a button's search mark); `ut_startorder.h`: when the plates may be probed, and what the stall check calls a crash) |
| `ut_miroll.h` | the monster infrequents' store rules, pure: the roll {prefix +0x28, suffix +0x48, seed +0x68} and its words, the cap (`utRecordCapDecide(held, max_per_record, kind)`: an MI holding a roll is refused naming it, before the max_per_record test; a unique keeps max_per_record; an UNKNOWN kind holding a copy fails closed), the KIND seam (`utCatIsMiOf` / `utCatHasKindOf` find `gdut::catIsMi` by argument-dependent lookup and answer false when it is not declared - `test_store --mi` prints DECLARED while the seam holds), and the transfer stash's save chain that establishes the prefix/suffix slots (section 8). `test_store --mi`, `test_journal` and `test_tooltip` prove it |
| `ut_hudlife.h` | the captured boxes' life and the world-start verdict, pure: `utBoxesUsable` (a complete capture, no capture open, vtable checked), `utBoxesHudGone`, `utWantedAfterTable` (the view-change clamp), `utTickRelayouts`; `UtTabFacts` / `utTabVerdict` / `utTabWarns` / `utTabOffLine` (the "the tab is off in this world" WARN). Proved by `test_logview` section 4 (a replay of a dead tab's log) |
| `ut_padinput.h` | the screen pad's input rules, a pure header over the engine's 0x1C-byte MouseEvent: rewrite / clear / hand-over per event, the press and its own UP, the polled release, the item-on-cursor rule, a right click that is not the editor chord consumed (the chord in the gates: `editorType` / `editorMods` from `[keys]` editor_open, `mods` held - `scrBody` fills them, `editorChord` is `utKeyClickMatches`; editor_open bound to a key = type 0, every right click on the pad only consumed), idempotence per Update generation, the editor hand-over (the editor before the item rule: with an item on the cursor a LEFT DOWN in the grounds is still the editor's, consumed, the item stays), the published rectangles (`UtPadScreenRects`) and their freshness. THE ADOPTED DRAG - with no press open and the gate `editorDrag` set (the editor mid-drag and ticked within 400 ms), the first event of a drag begun in the window becomes this machine's editor press (`adopted`): every event cleared wherever it is, its MOVEs and LEFT UP handed to the editor wherever they land, that UP cleared, not rewritten (the caravan saw the DOWN); `scrBody` evaluates the gate before its cheap exit, so it runs while nothing is published. A LEFT DOWN ends ANY open press (a right press whose RIGHT UP was lost too) and is judged on its own, as is a DOWN on which the polled release ends a press. `utPadButtonKind(index, buttons, paged, edit)`: MAT 0, the buttons 1..G, OWN G + 1, NEXT G + 2 only while paged, EDIT G + 3 only while `edit`. THE WINDOW IS THE WINDOW'S - while the editor is open it publishes the caravan window's client rect (`UtPadScreenRects::window`, `utPadScreenWindowFresh`, gate `windowFresh`); a point inside it starts no press and is handed nothing (`kUtPadWhyWindow`; an adopted drag's events there pass untouched, its UP there is the caravan's), a LEFT DOWN there is only NOTED to the editor (`kUtPadActEditorNote`) - the window route's proof; a press begun on the screen still runs to its UP wherever it lands. `tools\test_padinput.cpp` proves it |
| `ut_keys.h` | the `[keys]` model, pure (standard headers only; the message numbers, VKs and click types it spells out are checked against `windows.h` in `test_keys`): the grammar and `utKeyParse` (every refusal a static reason), `utKeyText` (the canonical text), `UtKeyBind {kind, code, mods}` and `UtKeys` (the seven bindings, the defaults = the input before `[keys]`; a POD in `UtConfig`, `g_cfg.keys`, derived at the main file's parse with the conflict pass `utKeysDerive`), `utKeyFind` (among a site's actions whose code matches and whose modifiers are all held the most modifiers wins, a tie the earlier), `utKeyClickMatches` (editor_open's click: its type, all its modifiers held, extra ones allowed), `utKeysWantSys` (WM_SYSKEY* looked at only when a key binding names Alt or F10), `utKeyDecode` (a window message -> DOWN / UP / WHEEL and its VK: WM_MBUTTON* 0x04, WM_XBUTTON* 0x05 / 0x06), `UtKeyEaten` (the KEYUP rule per VK, 256 bits) |
| `ut_screendraw.h` | the screen pad's draw, its pure half: the ground's 1-px frame (`utScreenFrame`), the rectangles the screen input reads, filled from the draw (`utScreenPub*`), the proof that the HUD draws the item rollover once and after the caravan (`utHudSiteProof`), the site switch (`kUtScreenSiteChosen`: the rollover head, or the Draw tail as the fallback). `tools\test_layout.cpp` section 11 proves it, over the exe image too |
| `ut_buttons.h` | the buttons file `uniquetab-buttons.ini`, pure: the grammar (`utbParse`: `[TAG]` with `name` / `caption` / `groups` / `size` / `font`, `[tab]`, `[unassigned]`; every refusal naming its line), `utbResolve` against a game's group table (buttons in file order, `buttonOfGroup`, the unassigned set), `utbResolvePad`, the populated default (`utbDefaultText` = `build_uniq_db.py buttons_default_text`, byte for byte), the byte-preserving `[unassigned]` rewrite (`utbRewriteUnassigned`), the button a built-in group added after 1.1.0 gets ONCE (`utbLateBuiltinIds` = the built-ins after 1.1.0's 24, then the template's two per-class groups: `{blueprint, staff, spear1h}`, so DeathGrimDawn's collect_mi page `spear1h` "Artifacts" reaches a file written before it; `utbOfferNew`: for a late id no button lists, not under `[unassigned]` and not on its `; added once by the mod - delete its button and it stays deleted: <ids>` comment line, a `[TAG]` section - the default file's `name` / `groups` lines - inserted after the LAST button section and the id added to that line; every other byte kept, CRLF or LF as the file has it; `buttonsLoad` in ut_panel.cpp runs it after a resolved load, writes atomically with one INFO line and leaves the stamp alone so the `[unassigned]` rewrite follows within a second), the one-time fold of an old `uniquetab-text.ini` (`utbFoldText`), the font folder the letters need (`utbRoleCodePoints`; `utbDeriveFolder(cps, covers, ctx, dynamicFonts)` in its derivation order: a letter above U+FFFF never decides it; the ROOT font when it has every letter (no font key, nothing below runs); the script folder of the letters it lacks - any kana `ja`, else any Hangul `ko`, else any Han, CJK punctuation, Bopomofo or fullwidth form `zh` (`utbCjkFolderFor`), else the first letter's script, Cyrillic `ru` / Greek `el`; the baked FNTX copies in `utbFontFolderList` order; the dynamic FNTD copies `utbDynamicFolderList` = `zh ja ko` (`th` and `vi` carry zh's font); else `zh` with Windows' own fallback drawing the rest (`UtbDerived::dynamic` / `lacking`, one INFO line, `utbDerivedFallbackText`) - without the FNTD cmaps, the copy with the most letters and the WARN), and what the glyph check's WARN advises (`utbFontAdvice`, over `utt::utTextFontAdvice`: the FNTD copy for a letter no baked copy has, "no font line can draw it" above U+FFFF). `tools\test_buttons.cpp` proves it |
| `ut_layout.h` | one more pure header: the layout model - `x,y,w,h` rectangles and `<W>x<H>` cover keys, `utGridFit` (pages_gen's grid arithmetic), the frame's box count (`utFrameBoxes`: the cover's largest uncapped fit over the view's cell sizes, clamped to 160..512), the pad, caption and label fits, `utLayoutResolve` (which rectangle is in force per part, and why a user one fell back), `utMatPadResolve` (THE materials-page rule: `padmat_`, else the pad in force where it is; `utMatPadText`), `utLayoutFromSource` (the P line and the ini to the layout in force), and the screen-anchored pad and label (`UtScreenRect`, `utScreenPlace` / `utScreenToLocal` / `utScreenLocate` / `utScreenFromClient`, the screen part of `utLayoutResolve` and its refusals). THE PINNED PART: while its screen key is in force or waits to be placed (`kUtLayoutScreenNoView`), a pad or label takes no part in the in-window pair rules and its `<part>_<W>x<H>` line is not judged at all (`L.ignored[p]`; `utLayoutFromSource` drops it from the input it returns - the parsed value stays in `userStore` - so ut_live's refusal WARN and ut_panel's red outline stay silent; `utLayoutIgnoredText` is the one DEBUG line per change); a screen key refused for its own reason puts the part back among the in-window parts, its line judged like any in-window line. THE LATTICE: `utLatticeFrame` - the frame of one cell in plate px, the cell grown by `kUtFrameOutset` (2) and cut to the plate, false only when the cell itself is off the plate; `kUtLatticeRule` (2) names that rule in the layout-plate hash. `ut_live.cpp` (`liveLayoutGet`, the grid) and `ut_panel.cpp` (the pad, the label, the outline) run it; `tools\test_layout.cpp` proves it; the zoom - `utZoomCell`, the per-group clamp `utZoomFit`, `utLiveGroupGrid` at a zoom, `utZoomLargestCell` (the editor's smallest grid), `utLiveGridKind` (section 4 *The zoom*) |
| `ut_layoutedit.h` | the in-game layout editor's decisions, a pure header over `ut_layout.h`: handles and the toolbar, a drag clamped to the page (the grid snapped to whole cells), the check through `utLayoutResolve`, the state machine (open, drag, save, cancel), the one-shot key; the screen mode (the pad and the label across the plate's edge both ways, clamped to the screen, the grid never; the toolbar travelling with the pad; the save plan per mode and file, `utEditSavePlanOf`; the grounds a screen part takes clicks on, `utEditScreenGrounds`). `utEditToolbarAt` - the toolbar button under a point in ANY state but closed, so `utEditLeftDown` answers CANCEL mid-drag, while saving (it stops the wait and closes "saved; the settings file has not been read back yet" - the file IS written) and with an item on the cursor; a LEFT DOWN during a drag ends that drag first and is then judged as a click of an open editor. TWO ROUTES, ONE BORDER: `utEditWindowClient` (the border the editor publishes: the plate scaled + the Draw origin), grounds only for screen-mode parts and a toolbar off the plate, and the window route's proof `utEditNoteProves` (the screen input's note of the same click: within `kUtEdProofPx` 2 px, at most `kUtEdNoteMs` 250 ms old, used up on a match). `ut_panel.cpp` runs it; `tools\test_layoutedit.cpp` proves it |
| `ut_textcfg.h` | the words' engine: the label templates with named placeholders (never a printf format), the fallback chain (`utTextResolve`, over a glyph callback: a word the font in force cannot draw falls back down the chain - the tag, the group's own name, the built-in English), the `[tab]` keys' value rules (`utTextApplyKey`, shared with `ut_buttons.h`), the UTF-8 helper, the label line (`utLabelCompose`), what the missing-letter WARN tells the player to set (`utTextFontAdvice`, the baked-font text that `utbFontAdvice` wraps and makes exact), the baked language folders (`utTextFontFolders`); and the parser of the RETIRED text file `uniquetab-text.ini` (`[text]`, `[text.<lang>]`, `[style]`), kept only for the one-time fold - no template, no writer. A pure header, standard headers only; `tools\test_buttons.cpp` links it; `ut_panel.cpp` runs it and `ut_tooltip.cpp` takes its two lines and `tooltip.roll` (`kTextTooltipRoll`: built in " (another roll)", 1..96 UTF-16 units, appended to `tooltip.yes` as written - a translation quotes its leading blank; not in `utbTabOrder`, so the generated file does not list it) |
| `ut_textmeasure.h` | the engine's pen rule over a glyph cache filled from any font (the live one through ut_bindings, the compiled table offline), the per-world cross-check of the live route against `ut_fontmetrics.h` on the root font, whether the table bounds a language copy's captions (a report for `test_text`; nothing is gated on it), UTF-8 to UTF-16 for the wide text call (nothing allocated), the language font path; the root font every role draws without a key (`kUtRootFontPath`, `utRoleFontPath`); a role's own font: its Fonts.arc entry and when it is used (`utRoleFontState`); the FNTX glyph tables read out of Fonts.arc (`utFntxGlyphs`, for the font that follows the letters); the live probe's plan, `utTmProbePlan(kind, nominal)`: a baked font (kind 0) fills every size 1..64 when the world starts, byte for byte the eager fill (`utTmAddSize`, `utFontCacheReset`, `utFontCacheFill` on them), a dynamic (1) or unknown (-1) one is a LAZY cache (`UtFontCache::lazy`): its nominal size at once (`utFontCacheFillPlan`), any other the first time it is measured (`utFontCacheFillSize` / `utFontCacheWidthLazy`; a size that gives nothing is marked -2 and never asked again), so a world builds only the GDI styles a draw asks for; the no-styles reason text. A pure header; `tools\test_text.cpp` links it; `ut_panel.cpp` (drawText, textWidthPx, ensureFonts) and `ut_tooltip.cpp` (makeWide) run it |
| `ut_sfnt.h` | the code points an sfnt font, a TrueType collection (face 0) or the game's FNTD file (the sfnt after its 28-byte header) maps to a glyph: cmap formats 4 and 12, the subtable fontTools' `getBestCmap` would pick, every offset and length checked against the buffer (a file pointing outside it is refused, never read past) - `utFntdIs`, `utSfntCodePoints`, `utCodePointIn`. Pure, standard headers only; the worker's `glyphSetsLoad` runs it over the FNTD copies of savapromedium; `tools\test_text.cpp` section 6 proves it over synthetic, corrupted and the real Fonts.arc files |

Those last headers are the pattern worth keeping. Whenever a decision is dangerous - which slot
inside a replica blob is a `std::string`, whether a page may paint, which row a scroll lands on -
it is written as a pure function in a header, the engine side supplies an SEH-guarded probe, and
the test supplies a plain one. Both run the same rule.

**The packers.** `tools\pack_catalogue.py --if-newer [--dry-run]` packs
`data\oracle\catalogue.json` into `catalogue.bin` only when the json is NEWER than the committed
bin and packs to different bytes; otherwise it prints `catalogue.json is older than catalogue.bin -
not repacked` without opening the json (absent: `catalogue.bin is used as it is`), and `--dry-run`
writes nothing. `build_model.bat`'s step 1 calls it that way, so a leftover json cannot pack an older
catalogue back over the committed one. A checkout gives the bin a fresh
mtime, so a json written before it counts as stale: regenerate (or touch) it to repack on purpose.
The C++ twin of the catalogue rule is `catalogue_gen.cpp` (`storable`, `scrollOf`), the Python one
`build_catalogue.py` (`storable`, `scroll_of`) with `pack_catalogue.py`'s
`CLASSIFICATION_NARROWED_FORMULA`. With the monster infrequents both packers sort a group's entries
by group, KIND, level, name, record - the uniques first, then the MIs (`packCatalogue` /
`pack_catalogue.py`); `is_mi_candidate` / `isMiCandidate` and `MI_CLASSES` / `isEquipmentClass`
are the twin rule.

### The settings file and its versions

`uniquetab.ini` carries `ini_version` (`UT_INI_VERSION` in `src\ut_config.h`, 44). A file of an
older number is migrated once through the merge-on-bump, every value the player set kept, with
INFO lines naming what was dropped or moved. The shapes the code reads and migrates:

- 36 is 1.1.0's file. 37 adds the `[layout]` block (`pad_caption_max` and the keyed `grid_` /
  `pad_` / `label_` / `padmat_<W>x<H>` lines) and `layout_outline`; 38 per-game values (the
  `[view:<name>]` blocks, moved out on the next bump) and the one-shot `layout_edit` (the in-game
  layout editor).
- 39 is the reworked file: the visible keys under `[general]`, the `[layout]` lines, and every
  other parsed key as one commented row of an `[advanced]` blob at the end
  (`;key=default<tabs>; what it does`; a key set by hand goes live in its own row). The feature
  switches, the timers and the pad geometry are not keys (their members keep fixed values); the
  remembered group / row / OWN filter live in `uniquetab.state`; `pad_screen` / `label_screen` are
  written only while set. A 38 file's shared `uniq_group` seeds the start-up view's state (vanilla
  clamps an out-of-range one to MAT).
- 40 adds the visible key `collect_mi` (the monster infrequents, default 1): a 39 file gains
  `collect_mi=1`, every value kept; a 40 file keeps the value it has, 0 included.
- 41 leaves four visible keys (`enabled`, `collect_mi`, `tooltip_mark`, `log_level`):
  `max_per_record` and `rescue` are hidden `[advanced]` rows marked "(untested)". A 40 file is
  rewritten in full, each of the two at its default as the blob's commented row, set to anything
  else as a live row there.
- 42 makes a custom game's own `mods\<name>\uniquetab.ini` a COMPLETE file of the same template,
  seeded once from the main file's values and never inheriting afterwards (`journal_dir` is the
  one main-only key). The seed copies the main file's `[layout]` lines and screen keys, so a
  game's first entry looks as the main campaign does, and starts the one-shots (`rescue`,
  `journal_prune`, `layout_edit`) at 0: a command written in the main file is for the main views
  (a RESCUE-NOW latch still arms whatever view is active). A game file of 41 or older - it has no
  `ini_version` line (the four-key `[game]` shape) - is completed from the main file's values
  once, on the game's first entry: its missing keys come from the main file, not from the
  defaults. The main file only gains the number. There is no per-key "follow the main file"
  switch.
- 43 adds the hidden `[advanced]` key `edit_button` (default 1: the pad's EDIT button). A 42 file
  gains its commented row, every value kept. THE SEEDING RULE for a game file of an older number
  that NAMES its `ini_version` (42 on - a complete file): every line it has is kept and every key
  it does not set takes the COMPILED DEFAULT, never the main file's value (a hidden key at its
  default is a commented row, so "missing"; seeding it from the main file would have given a game
  whose file says `;max_per_record=1` the main file's `max_per_record=2` at the bump). Only a file
  with no `ini_version` line is completed from the main file. `test_config` section 14 proves it
  (a 42 game file under a main file with `max_per_record=2` and `edit_button=0`).
- 44 adds two things, and a 43 or 42 main file gains both, commented, every value kept (a game
  file gains the zoom line only). `zoom_pct` (the collection's cells magnified, section 4 *The
  zoom*): an int row of `[layout]`, HIDDEN - a commented `;zoom_pct=100` line right under the
  section's blurb, before the family lines, live (the `;` gone) when set; the four visible keys
  stay four - and the first row with `kUtCfgRefuse`: a value outside 100..200 (or not a number) is
  REFUSED with one WARN and the default kept, never clamped (a typo of 1500 must not become the
  largest zoom); per settings file like every key. And the `[keys]` section between `[layout]` and
  `[advanced]` - seven hidden rows, `rows_up` .. `editor_open` (section 4, *The keys are a
  table*) - in the MAIN file only: `journal_dir` and the `[keys]` rows are the main-only keys
  (`configMainOnlyKey`), a game file never renders the section, a `[keys]` line in one is one
  WARN and ignored, and `copyMainOnly` puts the main file's bindings in force in every game.

The buttons file (`uniquetab-buttons.ini`, one per game) and the optional groups file
(`uniquetab-groups.ini`, main-only: a generation input with per-game blocks, not a setting) are
files of their own.

### Threads

- **Game thread** - everything that touches engine memory. Reached from the
  `Engine::PresentSurface` detour, from `GameEngine::Update`, and from the detours themselves.
  It resolves the buttons file's set against the game's group table (`textWorldBuild`, glyph
  metrics, after `ensureFonts`), and puts a view's own settings file and state in force
  (`configSetView`) when the view changes - file I/O under the settings lock (a custom game's
  `uniquetab.ini` is created at its first entry, inside the database detour). The screen pad's
  binding row is resolved here too (`scrResolveTick`, 40 tries from `plateTick`), and the screen
  site's proof (`siteNote`, `g_siteRoll` written and read here).
- **Render thread** - `panelDraw` and the plate/label draw. The layout editor's state lives on the
  thread of the Draw tail and the mouse slot, like the pad's own; `panelLayoutEditing` publishes
  two interlocked values that `applyLayout` reads on the game thread. The screen parts' draw site
  (the head of the HUD's item-rollover Draw) runs inside the HUD's Draw on the thread of the
  Draw tail, and the `DirectInputDevice::GetMouseEvent` detour inside `Engine::ProcessUserInput`
  on that same thread (one DEBUG line per session names it). The screen rectangles are published
  under a sequence lock (`plateScreenRectsPublish`, any thread); the detour copies a whole
  snapshot. The Draw tail publishes the canvas size and the window origin (`liveSetScreenView`,
  interlocked) while a screen key is set.
- **Worker thread** - the ini, the log flush, the journal writes, the exports, and the buttons
  file's poll (`panelTextFileTick`, right after the ini), which also writes a game's populated
  default when its file is absent and rewrites the `[unassigned]` section (tmp + replacing
  rename; its own write is not re-read) - after inserting, once, the button of a built-in group
  added after 1.1.0 (`utbOfferNew`, re-read on purpose so the rewrite follows). It reads engine
  state only through published atomics. The ini's writers (its reload, the game thread's view
  change) serialise on a lock; readers read `g_cfg` directly.
- **Layout-plates thread** (`ut_generate.cpp`) - polls the settings generation and the active view
  every 250 ms; on a change it publishes the plate set in force at once (file reads only) and
  paints it after 1 s of quiet. It never touches the engine.

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

**The layout is live, the overlay is not.** The player's `grid_/pad_/label_<W>x<H>` rectangles are
resolved against the view's cover (`liveLayoutGet`) whenever the ini, the UI scale or the view
changes; the boxes are positioned by `SetLocalPosition` on every relayout, so a moved grid is one
relayout away and never needs the overlay regenerated - the frame's box count N (the F line: the
cover's fit, 160..512, section 7) is the only overlay number it reads, as the cap. The plates
follow in a second step: the derived set stays; a
user grid gets a set of its own named `uniq_plate_<cell>_<cover>[_<tag>]_L<hash8>.tex`, painted
off the game thread (0.25-0.9 s, about 100 MB of heap, no catalogue, no overlay) and switched to
when ready - the page shows the plain cover meanwhile. Its lattice is the page's own fit:
`utGridFit`'s cols x rows, every cell framed by `utLatticeFrame` (cut at the plate's edge, never
dropped: a cell whose 2 px outset leaves the plate still gets its frame, so Reign of Terror's
`46,72,928,520` shows a frame under every one of its 29 columns). The hash covers
catalogue.stamp, the override, the fitted cells, plate_ground and the lattice rule (`lattice=2`),
so a game update repaints the set, a set painted under another lattice rule is painted again once
(that one removed as superseded), and a restart finds it on disk.
Both resolvers see the screen keys: the live page (`layoutCompute`, screen known) and the
layout-plates thread (`layWantNow` -> `utLayoutPlatesGrid`, `screenKnown = 0`, so a set screen key
pins its part out of the in-window rules, as on the live page before its first draw; a thread
that judged a user grid against the pinned pad's in-window line would refuse it and paint no set
while the page has it in force - the derived lattice under moved items). The page
shows the layout plates only while the live layout has the user grid in force and the derived
plates only while it has the derived one; a user grid without a set shows the plain cover
(`utPlateSetChoice`, compared in `ut_plate.cpp plateSetNow`; `plateLayoutEdge` renames on a
change of either side). The in-game editor writes the
same keys: while it is open the boxes are parked
(`applyLayout`) and the editor paints a plain ground over the plate; SAVE writes the keys of the
edited layout in one call: `grid_/pad_/label_<W>x<H>` for the parts in the window (the active
view's file; a part left at its derived rectangle loses its key), `pad_screen` / `label_screen`
for a part dragged off the window (the same file, the nearest screen corner; a part brought back
loses its screen key). The editor stays up, boxes parked, until the re-read ini resolves to the
edited rectangles (3 s at most). Any of caravan closed, another caravan page, the materials page,
a world change, focus lost, a UI-scale change or a screen-size change closes it without a write;
a page that stops being drawn has its boxes back at the next relayout even before the editor's
next tick says so (400 ms). It opens only on a collection group (the one-shot key waits for one)
and never edits `padmat_`. Its toolbar sits at the top right of the pad and travels with it, onto
the screen too; CANCEL answers in every state but closed (mid-drag, during the save handover,
with an item on the cursor), and a click during a drag whose release was missed ends the drag
first. On the game's own materials page the pad is drawn where it is, over the page's
boxes, and its clicks are its own (section 7).

**The captured boxes belong to one HUD.** `GameEngine::ExitPlayingMode` and a view change empty
the list (`liveOnHudGone`, `ut_hudlife.h`), and `liveTick` re-points only a complete capture of a
living HUD: a view change in the menu clamps the wanted group (Reign of Terror's group 28 is -1
in basemods' 24), and a relayout of a dead HUD's boxes faults, which would stop live paging for
the session. A fault in a relayout of a LIVE HUD is a session stop (an engine call that faults is
not retried), and every world start whose tab is off says so once, with the reason and whether a
restart or the next world re-arms it (`live: the tab is off in this world (the <view> view)
because ...`).

**The pad and the label off the window.** `pad_screen` / `label_screen` (`<corner>,x,y,w,h`, record
px from a SCREEN corner inwards, one line each per settings file - the main one's for vanilla and basemods, a custom game's own for that game) are resolved by
`utLayoutResolve` against the canvas size and the materials window's origin the Draw tail publishes
(`liveSetScreenView`; both join the layout's signature only while a key is set, and are kept
across worlds). `L.rect` of a pinned part is its WINDOW-LOCAL record rectangle (off the plate,
negative too), so the pad fit, the PadPlan, the hit band and the outline read it unchanged. A
pinned part is refused per frame-place (no screen size yet, as large as the screen, no room, over
the grid in force, a label over a pinned pad) and falls back to the in-window rectangle, with one
WARN per change (`ScreenNoView`, the state before the first frame, at DEBUG). The grid is never a
screen part. On the materials page a pinned pad stays whole at its corner (`padmat_` is not used).
A PINNED part - its screen key in force, or waiting to be placed (`ScreenNoView`) - takes no part
in the in-window pair rules, and its per-cover `<part>_<W>x<H>` line is not judged (`L.ignored`):
no overlap or outside refusal, no red outline, never in force; one DEBUG line per change,
`layout: label_438x627=103,4,333,23 is ignored while the label is on the screen
(label_screen=tl,330,168,333,23)` (`layoutCompute`). While it waits it sits at its derived
rectangle. Without this rule a stale `label_438x627` line would refuse a good `pad_438x627`
line against it. A screen key refused for its own reason
puts the part back in the window, and its line is judged like any in-window line.

**Where the screen parts are drawn.** One level below the tooltips: at the HEAD of the HUD's
item-rollover Draw (1.3.0.8 exe 0x236D00, slot +0x20 of the object at `hud+0xB800`). In a world
`Display::Render` draws the world view first and the UI root after it; the HUD is drawn by the
world view, its window children in a fixed order - the caravan (`+0x4FD08`), nineteen more, then
the item rollover `+0xB800` (the item tooltip and its compare boxes), then the modal dialog, the
cursor item and the fades; the UI root's layers and its button-hover tooltips come after all of
it. So the rollover head is after every HUD child that is not a tooltip, a dialog or the cursor
item, and before every tooltip. The site is proved once per session from the live HUD (= the
captured caravan - 0x4FD08): both vtables in the image outside `.text`, both Draws inside it, and
the HUD Draw's first 0x1C00 bytes holding exactly one caravan draw call and exactly one rollover
draw call, the rollover's after the caravan's (`utHudSiteProof`, `ut_screendraw.h`; offline over
the exe image in `test_layout` 11d), so the literal 0xB800 is decoded-confirmed, not trusted. The
detour (`hk_RollDraw`, MinHook, once per session, only while a screen key is set) passes all four
arguments through and draws only for THIS world's rollover, only in a frame whose Draw tail drew
the page; a fault is one ERROR and the site off for the session (both parts back in the window).
`kUtScreenSiteChosen` switches to the Draw tail as a fallback site at build time; no ini key.

**The screen pad's input.** A click outside the caravan never reaches the window's mouse slot, and
every consumer - the world view included - reads the input pump's copy of the event, so the one
point is the export `DirectInputDevice::GetMouseEvent` (DirectInput.dll, by name; confirmed on the
game thread as slot +0x38 of `Engine::GetInputDevice()`'s vtable, `::Update` as +0x10, the event
stride 0x1C from the export's own `imul`). The detour runs the original, then the pure rules of
`ut_padinput.h` over the copy: inside the published pad / label rectangles a LEFT DOWN is
REWRITTEN to a move with its held bytes cleared (a button acts; the ground, the label or an item
on the cursor - no action, the item stays on the cursor), a plain or Ctrl RIGHT DOWN likewise
(Ctrl hands it to the editor); while a press is open every other event is CLEARED until its own
UP or the polled release; a LEFT DOWN ends any open press (a lost RIGHT UP included) and is judged
on its own; outside, everything is untouched. A button's action is `padClickTarget`, the same
function the window pad's `mouseDecide` calls: MAT selects the game's own materials page (its
"no group" answer is not "a button that maps to no group", whose click is consumed with no
action), every other button `panelButtonTarget` (a group's
page, OWN's filter, NEXT's page). THE ADOPTED DRAG: an editor drag begun IN the window (its LEFT
DOWN was the caravan's, so this machine had no press) and carried out over the world by the
polled cursor is adopted at its first event while the editor says it is mid-drag
(`panelLayoutEditDragging`, ticked within 400 ms): every event of it cleared wherever it is, its
MOVEs and its LEFT UP handed to the editor wherever they land, that UP cleared but not rewritten.
Without the adoption its held MOVEs over the world would reach the world view (a "hold to move"
under the drag) and its UP the world. Inert
while nothing fresh is published (four frames) - but for an adopted drag -, until the event space
is proven, and while the caravan is closed. In
multiplayer nothing is network-visible: a consumed click simply never becomes a command. The pair
installs only in a world, and only while a screen key is set or the layout editor is open (so a
first drag out of the window finds SAVE answering on the screen; once installed it stays for the session, inert as above); a resolve failure turns only this
route off (one WARN), a fault turns it off for the session (one ERROR, the detours pass through).

**The editor's two routes.** A part the editor brings back into the caravan window must be
clicked through the WINDOW's mouse slot (`editMouse` -> `panelLayoutEditMouse`), whose LEFT DOWN
is accepted only in a proven event space - and that proof, `g_btnSpaceProven`, is the in-window
pad's own probe, which runs only for points inside the in-window pad's band. With `pad_screen` in
force from the world's start the live pad is off the plate and the probe never runs, so on that
proof alone the window route would refuse every body, handle, SAVE and RESET click (only CANCEL
needs no proof; such a world's log has `screen pad input: consumption ENABLED` and no
`buttons: consumption ENABLED`). The rule: THE WINDOW IS THE WINDOW'S - while the editor is
open it publishes the caravan window's client rect with the grounds (`UtPadScreenRects::window`),
the screen input starts nothing and hands nothing inside it, and a LEFT DOWN it passes untouched
there is NOTED to the editor (`kUtPadActEditorNote`, window-local = the event minus the Draw
origin). The window slot gets the same click next (the pump dispatches each event right after
GetMouseEvent copies it out); its LEFT DOWN, unproven by the in-window probe, is accepted when its
point equals the note within 2 px and the note is at most 250 ms old (`utEditNoteProves`) - the
in-window probe's own equation checked on the click itself. One proven click latches the window
route for the rest of the edit; two DEBUG lines once per edit say `a click in the caravan window is
proven by the screen input's copy of it (delta=...)` or `... was refused`. A screen-mode part
straddling the edge takes clicks on its in-window portion through the window slot and on its
outside portion through the screen input. **A trap for any future window-route feature: every
rule gated on `g_btnSpaceProven` is dead while the pad is pinned.**

**The EDIT button.** A fourth fixed built-in (`utTextBuiltinEdit`: id `edit`, caption `EDIT`,
name "Layout editor"), on the pad while `[advanced] edit_button=1` (the default, per settings
file). Its LOGICAL index is G + 3, after NEXT, so OWN (G + 1) and NEXT (G + 2) keep theirs; its
SLOT is the one before OWN, because the last button is the double-wide one (`utPadCellRect`):
unpaged MAT, the buttons, EDIT, OWN; paged MAT, K buttons, NEXT, EDIT, OWN (`utPadPaginate(groups,
edit, fits)`, `utPadSlotLogical`, `utPadLogicalCount` = G + 4 while on - an unpaged pad with EDIT
has no G + 2). The fixed captions are appended in logical order (`fixed("edit")` / `cap("edit")`
after NEXT in `utbResolvePad` / `utTextResolvePad`; a TextWorld holds buttons + 4 captions), so
`capText(G + 3)` is EDIT; the caption fit measures it only while it is on
(`panelButtonCaptionsFor(paged, edit)`: the CapTable's four sets). ut_live paginates with the
key's value, and the reload's `configGeneration` is in the layout signature, so `edit_button=0`
re-lays the pad out within a second. Measured with the real fitter: vanilla stays 10 x 3 cells of
30 x 14, caption 10 at scale 1 (EDIT takes one of the two empty cells; OWN moves one cell right,
to 322,55), Reign of Terror 11 x 3 of 27 x 14, DeathGrimDawn 12 x 3 of 25 x 14 and one caption
size smaller (9 -> 8). THE CLICK: `padClickTarget` answers `kUtButtonEditTarget` (-4) for the
kind (`panelButtonTarget` too); `hk_WndMouse` and `scrButton` call `padEditClick` ->
`panelEditButton`, which stores the click's tick in the interlocked latch `g_editButtonAt` and,
on the game's own materials page, selects the last collection group the pad showed
(`g_lastCollectionGroup`, as NEXT reads it) - the click is always consumed, one INFO line.
`editTickDraw` (the pad's draw, every frame the caravan draws, also while the pad is pinned)
consumes the latch with `editOpen("the EDIT button")` once a collection group is on screen and
drops it after 2 s with one DEBUG line: the one-shot key's path, so both routes end on the thread
the editor's state lives on. While the editor is open the pad is neither drawn nor hit, so EDIT
needs no state; it is never `on` and never search-marked. `[tab] edit.caption` (old text key
`caption.edit`) is read like `tooltip.roll` and never written, so the generated buttons file and
the Python twin keep their bytes; no tag is reserved for it (`[EDT]` stays a player's tag).

**The zoom.** `zoom_pct` (`[layout]`, 100..200, default 100) draws the collection's icons bigger by
making the BOX WIDGETS bigger, because the engine already stretches: `ReagentWindow::Draw`'s item
draw (exe 0x1EDE20) maps the whole texture onto the box rectangle `box+0x6C..+0x78`, and the hit
test, the tooltip's rollover copy, the corner badges, the count text, the rarity background and
the dimming all read that rectangle (the drop is window-level and reads none of it). No record
can ask for it - `UIReagentItem::Load`
reads only `itemBoxX/Y` and `reagentName`, and `SetTexture` (slot +0xB0) sets `w,h` = the
texture's size times `GetUIScaleFactor` whenever the texture POINTER at `+0x40` changes, and
returns at once for the same one. So it is all live, in four places. THE CELLS (`ut_layout.h`):
the cell in force is `utZoomCell` = round(cell x zoom / 100) with a PER-GROUP clamp,
`utZoomFit` - the zoom lowered (never below 100) until one magnified cell fits the grid in force
(width, and height + row gap) - so a small user grid lowers the zoom of its tall groups only, one
INFO line naming them (`live: zoom_pct=200 - N group(s) shown at a lower zoom, so one magnified
cell fits the grid WxH: Helms 157% (100x100), ...`); `utLiveGroupGrid` fits the magnified cell
with `utGridFit` over the grid in force (derived or user) under the frame's N, and gives exactly
the old answer (the G line, or the user fit) for a group at 100. The resolver still judges a user
grid against the GENERATED largest cell. THE GROUPS (`ut_live.cpp applyGridLayout`): each `Group`
and each union page gets `zCellW x zCellH` and its zoom; `applyLayout` lays the boxes on that
pitch, `plateApply` asks for that cell's plate, `liveViewInfo` reports it (the editor snaps to it,
the outline names it), `liveCellSizes` returns the cells in force; `LiveLayout.zoom` is in the
layout's `same` test, so a zoom edit alone is a new layout (one relayout, the first visible item
kept by `utRowKeep`), and above 100 `LiveLayout.in.maxCellW/H` are the largest cell as shown
(`utZoomLargestCell`, never larger than the grid in force): the editor's smallest grid
(`utEditMinSizes`) and its SAVE check. THE WRITE (`showBox` -> `boxZoomApply`): after the
SetItem / SetLocalPosition of a shown box its `+0x74/+0x78` = floor(texW x scale x zoom / 100 +
0.5) - from the box's own texture (`g_gd.TextureGetWidth/Height`), `scale` the factor SetTexture
uses (`GetUIScaleFactor`, else the filler-box measurement; never `ui_scale_pct`) - NEVER the
current w,h multiplied (a SetTexture that early-outs would compound it). Each box remembers
{widget, texture, w, h, zoom}; a box shown at 100, the materials page's 24 and every parked box
(`hideBox`, BEFORE its SetItem(0), while the texture is there to measure) get the native size
back (what SetTexture computes) and lose the entry, so the materials page and `zoom_pct=100` are
the engine's own sizes; with no entry at 100 nothing is read or written at all. THE RE-ASSERT
(`zoomReassert`): once per `liveTick` (the game thread, never the Draw detour, which stays
paint-neutral) and right after `reagentAfterRelayout` (the mod's own Sync): every entry's widget
is read - another texture under it is measured again, a size the engine reset is written back;
two interlocked reads per tick while nothing is zoomed. The heartbeat's `zoom=<pct>/<written>/
<re-asserted>/<re-measured>` counts them (only once a zoom was set), and one DEBUG line per HUD
says the first write-back. A fault in any of it is the existing session disable. These two floats
and the `+0x7E` compare byte are the file's only direct engine writes (its invariant header).
THE PLATES: a zoom above 100 is a layout of its own. `utLayoutPlatesGrid` (the layout-plates
thread's resolver) answers 1 for the derived grid too, with the text `x,y,w,h zoom=<pct>`
appended to the grid's canonical text (`grid_438x627=105,76,333,552 zoom=150`, the hash input,
so every zoom is its own `_L<hash>` set and the old sets keep their hashes at 100);
`gen::LayoutGrid::zoom` makes `planLayoutPlates` paint the MAGNIFIED cells (`utZoomFit` per cell
size over the rectangle; two sizes that come out the same are one plate, ascending), so Helms at
150 asks for `uniq_plate_96x96_438x627_L<hash>.tex`. On the page `plateLiveGrid` answers
`utLiveGridKind(user grid, zoom)` - 1 for a user grid OR any zoom - so `utPlateSetChoice` shows
the derived set only for the derived grid at 100, the zoom's set when it is ready and the plain
cover meanwhile (0.25-0.9 s, as for a moved grid); back at 100 the thread removes the zoom's set
as superseded. `tools\make_plates.py --grid x,y,w,h --zoom <pct>` paints the same set, byte for
byte (`test_catalogue --layout-plates`). What only the game shows: how soft the magnified icons
look (the canvas sampler), and how often the re-assert fires (the heartbeat's counters).

**The keys are a table.** `[keys]` (main file only, section 3) binds seven actions: `rows_up` /
`rows_down` / `button_prev` / `button_next` (the key site: a key, or MiddleClick / Mouse4 / Mouse5),
`rows_wheel` / `button_wheel` (the wheel site), `editor_open` (the click site on the pad, or the key
site). The parse result is `g_cfg.keys` (`ut_keys.h`); nothing is parsed on the input paths. THE
WINDOW PROCEDURE (`reagentHandleMessage`, from `hk_WindowProc`) looks at nine messages only - the
four key ones, the wheel, the middle and X buttons - and copies `g_cfg.keys` once per message:
`utKeyDecode` (WM_SYSKEY* only while `utKeysWantSys`), the modifiers by `GetKeyState` of
`VK_CONTROL` / `VK_SHIFT` / `VK_MENU` at the message's own time, `utKeyFind(keys, site, vk, mods)`,
then `keysAct` behind the gates the hotkeys always had (the caravan open, the Crafting Materials
page the visible caravan page): with live paging armed every step and wheel action
(`liveHandleStep`, `liveHandleWheel(ticks, buttons)`) and `editor_open` from a key (`panelEditKey`:
the EDIT button's latch path with its own latch `g_editKeyAt`, consumed only while the pad drew in
the last 500 ms, an auto-repeat consumed with nothing more; `editTickDraw` opens it, a latch 4 s
old is dropped); the fallback (the tab did not arm) only `button_prev` / `button_next` /
`button_wheel` (`switchPage`). `test_keys` proves that the defaults decide exactly what the
window procedure decided before `[keys]` (every message x VK x Ctrl/Shift/Alt, both branches).
THE KEYUP RULE per VK (`UtKeyEaten`, 256 bits): a DOWN the mod consumed sets its VK's bit, one it
let through - or one the gates refused - clears it, and the UP of that VK is swallowed only when
its bit is set; the UP is decided before the gates, so the UP of a consumed DOWN is swallowed even
if the caravan closed in between, and the fallback swallows a Ctrl+PageUp/Down KEYUP only after
its own consumed DOWN (it used to swallow every one). The mouse buttons' UPs the same; an X
button's message answers TRUE. THE CLICK SITE: `editMouse` (the window's mouse slot), `scrBody`
(the screen input's gates `editorType` / `editorMods` / `mods`) and `panelLayoutEditMouse`'s
recheck all ask `utKeyClickMatches(g_cfg.keys.editorOpen, type, mods)` with `GetAsyncKeyState`
modifiers; editor_open bound to a key makes every right click on the pad a plain one (consumed,
no action). The game reads its own keys and the wheel through DirectInput, so nothing here hides
a key from the game - only from `WinWindow::WindowProc`'s other consumers (README: a key the game
binds does both).

**The buttons are a file.** `uniquetab-buttons.ini` (`ut_buttons.h`), one per game: the main
campaign's beside `uniquetab.ini` (it serves vanilla and basemods), a custom game's in
`mods\<name>\` (`custom-x` and `basemods+custom-x` share it). It is written populated -
one `[TAG]` per G line (id, tag, label), the default words under `[tab]`, an empty
`[unassigned]` - when the game's file is absent and the game thread has snapshotted that game's
group table (the
first caravan visit there, not the database load) - its `[unassigned]` carries the comment line
`; added once by the mod - delete its button and it stays deleted: blueprint` (Reign of Terror's:
`blueprint, staff, spear1h`), naming the late built-ins the game has; the first one written in a
process also folds an old `uniquetab-text.ini` in (its `[text]`, the `[text.<lang>]` its `[style] language` named and
the `[style]` font/size/colour keys; `@game` names and other games' ids are named in the log as
not carried) and renames that file `.old`. The worker polls the file of the game in force once a
second (a stat; a read, `utbParse` and `utbResolve` only after a change of the file, the game or
its table) and publishes it by pointer; a refused file is one WARN naming its line and keeps the
buttons in force when they were this file's for this table, else the built-in ones (one button per
group). The id `retired` is accepted on a button as a legacy id that holds nothing (`knownIds()` in
`ut_panel.cpp`), so a file that names it is not refused whole. After a load it rewrites only `[unassigned]` (byte-preserving: the section's key lines
become one `groups =` line; CRLF, comments and every other byte kept). Before that, ONCE per
built-in group added after 1.1.0 (`utbLateBuiltinIds`: `blueprint`, then the template's per-class
`staff` and `spear1h` - DeathGrimDawn's `spear1h` is its "Artifacts" page under collect_mi=1): a
file written before the group existed gets its `[TAG]` section after the last button
and the id on the "added once" line (`utbOfferNew`, one INFO line); a group already under `[unassigned]` or on that line is never
offered - the line is what keeps a deleted `[BLU]` deleted between the player's edit and the next
rewrite, when the group is on no button and not yet under `[unassigned]`. On the game thread
(`textWorldBuild`, after `ensureFonts`, once per world and when the file or the table moves) the
pad is keyed by BUTTONS: the captions MAT .. NEXT, EDIT, the per-group names (the button's), the group
<-> button maps, a click selecting the button's page (its first group), size and font per button,
the label templates and words, the rollover lines; a changed pad bumps `panelMeasureGen` so the
layout re-paginates. ONE GRID PER BUTTON: a button holding several
groups shows ONE page - their entries concatenated in group order, one row window, one label with
the summed counts, one owned filter, one Ctrl+wheel stop (at its place in the file's button
order) - built live by ut_live (`unionsSync`,
keyed by the first group; no generator, no restart). Its cell is the smallest cell of the view
that holds every member's, so the page keeps a plate painted for its lattice (ROT Bows 64x128 +
Hunting Spears 64x160 -> 64x160); a smaller item sits top-left in a larger cell; a union whose
cell no group of the view has shows the plain cover, said in the INFO line `live: one grid per
button - "Bows + Hunting Spears" (groups 23 staff, 24 spear1h, 95 items) is one page: cells
64x160, 14 x 3`. A one-group button is that group's own page; the union path does not run for
it. **The tab is English by default
in every language of the game; the player's file changes that, nothing else does.** A role (the
captions, the label line) whose strings have a letter outside printable ASCII and no `[tab]` font
line draws in the copy of savapromedium the letters derive (`utbDeriveFolder` over the FNTX glyph
tables and the FNTD copies' cmaps, read once per process out of the game's Fonts.arc by the
worker; the derivation order: a letter above U+FFFF never decides it - it falls back to the tag /
the group's name with the WARN -; the root font while it has them all; else the script's folder -
any kana `ja`, else any Hangul `ko`, else any Han `zh`, else Cyrillic `ru` / Greek `el` -; else
the first shipped copy with them all, the baked ones, then `zh` / `ja` / `ko`; else `zh` with
Windows' fallback for the rest, one INFO line, not a WARN), loaded by `GraphicsEngine::LoadFont` on the exact path; a
`font.caption` / `font.label` line wins, and a button's `font =` wins for that button.
`size.caption_max` / `size.label` override `pad_caption_max` / `plate_label_size` in the draw and
in the layout's own fit. THE PROBE RULE: a baked (FNTX) font fills its glyph cache at every size
1..64 when the world starts; an FNTD font (TrueType, rasterised by GDI at run time, so
each size built is a GDI font) is measured only at the sizes drawn - its nominal size at the
world's start (the proof that it measures), any other the first time the pad or the label
measures at it, and only on the thread that ran `ensureFonts` (`utTmProbePlan`); its kind is the
`FNTD` magic of its Fonts.arc entry, and a font whose entry cannot be read is treated as dynamic
(lazy measures a baked font identically). The glyph check asks an FNTD role nothing: every UTF-16
letter draws there, Windows drawing what the font lacks. A font the game does not have, cannot
load or the live route cannot measure is one WARN and the caption font stays.
The rollover lines are the game's font always. With no file, a refused one or the generated
default, every game draws exactly the built-in pad (`test_buttons`' identity gates).

**The mod creates, moves and deletes nothing.** The engine does all of that itself when the player
drops an item on a box. The mod's job is to decide whether the drop is allowed and to record what
happened.

**A monster infrequent holds one roll.** `collectionAccepts` asks the kind only when the box
already holds something (`journalRecordIsMi(key, true)`) and decides with `utRecordCapDecide`: an
MI with a roll stored is refused FIRST - at any `max_per_record`, so the reason names the stored
roll (`deposit REFUSED: <record> - a monster infrequent keeps ONE roll per record and this box
already holds it - "<name>" level N: prefix ..., suffix ..., seed .... Take that one out first to
store this roll`), through the existing refusal path (the item stays on the cursor / in the bag);
then the max_per_record test; an unknown kind holding a copy fails closed. The belt:
`journalDepositCommit` refuses count 2 of a known MI. A count-1 commit into a box a take emptied
replaces the row's identity (the old roll went back with that take). The identity round trip is
the one every item has (section 8): prefix +0x28 and suffix +0x48 are seen set. Hovering an MI
whose prefix / suffix / seed differ from the stored row's adds the third line "In your collection
(another roll)" (its words `[tab] tooltip.roll`); the same roll with another component or augment
reads "In your collection". The KIND is read through `gdut::catIsMi(entry)` by argument-dependent
lookup (`ut_miroll.h`); a catalogue without the accessor would make every record a non-MI, so the
seam is proved (`test_store --mi` prints DECLARED; a `static_assert` in `ut_rescue.cpp`'s
include order compiles). With `collect_mi=0` no MI is in the catalogue and none is on a page: a
stored one keeps its journal row, hidden, until the flag is 1 again.

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
  to `<mod folder>\mods\<leaf>\` (`basemods+custom-<leaf>`: `mods\<leaf>\basemods\`;
  `utCustomViewRel`, `ut_folders.h`), which is why entering a custom game and leaving it again
  never rewrites what the main campaign reads. A custom view's plate names always carry the
  cover size and the view's tag (`uniq_plate_64x96_346x625_lootascension.tex`,
  `uniq_plate_64x96_438x627_deathgrimdawn.tex` on a vanilla-sized cover), because every view's
  plates share one folder; the vanilla and `/basemods` names carry no tag (section 7).
- **The switch is the database detour.** `hk_LoadDatabase` recognises a custom game's archive
  and calls `ut::viewEnterCustom`. The engine names that archive relative to the game folder
  with forward slashes - `mods/LootAscension/database/LootAscension.arz` - so
  `gen::customGameFolder` accepts `[<dir>/]mods/<name>/database/<file>.arz` in either separator
  and any case, relative or absolute, resolves it against the game folder (then the current
  directory) and keeps it only where that archive exists; the main database's own archives, the
  Crucible's DLC parts at the game root and `mods/database.arz` name no custom game.
  `test_catalogue --view` feeds the engine's exact string. `viewEnterCustom` then reads the mod's
  own archives and asks `utProbeView` (`ut_viewprobe.h`): a custom game that holds no
  `records/ui/caravan/*` record, no caravan texture and no record the catalogue's mod rule could
  collect (junk list, class, relic/augment/component/blueprint/Epic/Legendary, not BLANK; with
  collect_mi=1, the default, every Rare equipment record - `gen::isMiCandidate`) - the
  Crucible, a map or a class mod - has the start-up view's geometry AND catalogue already, so it
  gets one info line
  and no view of its own; a loot mod that leaves the caravan alone gets one. From the view
  folder's creation to `viewEnterStartup` the log is teed into the view folder's `uniquetab.log`, and
  `configSetView` puts the game's own settings file (created at the first entry) and the view
  folder's `uniquetab.state` in force before its generation reads them. Otherwise the view is
  generated
  SYNCHRONOUSLY on the game thread before it does anything else: the overlay a custom view needs
  is that view's own `uniq-pages.arz`, and the overlay is loaded from inside this same detour, so
  the files have to exist by then. `hk_LoadMainDatabase` calls `ut::viewEnterStartup` before the
  original runs, because that call rebuilds the base database and takes any custom layer with
  it - that is the whole return path. `applyViewChange` then re-reads `uniq-records.txt`,
  `uniq-pages.txt`, the group table AND `catalogue.bin` (the ownership model recounted over the
  live items under the model lock, the journal's display names following), re-points
  `g_arzPath`, drops the plate probe
  (`plateOnViewChanged`) and reloads the overlay. Everything the tab reads goes through
  `ut::utViewFile`, which is `utModFile` on the start-up view.
- **No record of ours is read before a custom view's overlay is in.** `ObjectManager::LoadTableFile`
  caches every record it hands out (the table depot at `ObjectManager+0x170`) until the next
  `Engine::LoadMainDatabase` empties it (Engine.dll `0x1C4EF0`, called at its start); a later
  `Engine::LoadDatabase` replaces the archive's entry (`0x27FE70`, unconditionally, last load
  wins) but never the cached copy. Every view writes its frame as `records/ui/caravan/uniq_frame.dbr`,
  so a read of that record between the main database and the custom view's overlay pins the
  start-up copy, and the custom page is built from it - the start-up box count at the vanilla
  `0,88` instead of the view's own (Loot Ascension's `513,70`). So the view switch runs FIRST in
  `hk_LoadDatabase`, before any read of our records, and `checkServedFrame` reads the frame back once the view's
  overlay is in and compares its box count and `WindowLocationX/Y` (`LoadTableBinary::GetInt`)
  with the record read out of the view's own `uniq-pages.arz` - never after a `LoadMainDatabase`
  reload, where the read itself would pin the start-up copy ahead of a custom game's archives.
  `ut_plate`'s verifier checks the same thing from the live window: `window+0x40/+0x44` is
  `WindowLocationX/Y * UI scale` (written by `ReagentWindow::Load` at exe `0x13218F`), so the
  page's location over the scale must equal the written record's within 4 units, and the box
  vector's size must equal its box count.
- **The catalogue is the view's; the journal is per custom game.** A custom view's catalogue
  carries the mod's own records (section 7), and the runtime reads the active view's
  `catalogue.bin`, so its pages have names, icons and search, and the pad has the buttons of
  that game's buttons file - one per group of the view's table by default, plus MAT and OWN (with
  the default collect_mi=1: Reign of Terror 29 groups, 31 buttons, 109 pages; DeathGrimDawn 30,
  32, 282 pages; the base game 25, 27, 197; with collect_mi=0: 29 / 31 / 73, 29 / 31 / 180 and
  25 / 27 / 109).
  Where the items ARE is per custom game too: a custom view keeps its own journal set,
  `<journal dir>\mods\<name>\uniq-items[-hc].jsonl`
  with its `.gds`/`.csv`, `journal_prune` copies and `rescue-report.txt` in the same folder
  (section 8). The key is the custom mod's name, not the view's, so `custom-<name>` and
  `basemods+custom-<name>` share one journal (`ut::journalViewKeyFor`). The files follow the view
  where the hardcore/softcore mode does - `followSaveVariant`, on the new world's first tick,
  after the previous world's teardown (which the log shows running AFTER the database reload, so
  switching inside the detour would let that teardown touch the new file) - and before the
  caravan's open edge runs the reconcile, so the reconcile reads and rewrites only the active
  view's file. A switch flushes only what is owed and a view change alone arms no export, so the
  start-up files come out of a custom game byte for byte as they went in. A custom game with no
  view (the Crucible) stays on the start-up files. A record the ACTIVE database has no record of
  (a mod's record seen from the main campaign, or one a mod update removed) has no box: the label
  ends in `+N not on these pages` (the reconcile's count also takes an owned record the database
  has and no group of the view takes - `liveGroupOfItem` -, e.g. an MI with `collect_mi=0` or a
  record a group's `[exclude]` drops; `utNotShownSuffix`; a group the buttons file leaves under
  `[unassigned]` is not counted - its records are on a page of the game, reachable by adding a
  button), the rescue skips its row (one line in the log and the report;
  the count is never touched) and hands it back only in a game whose database has it, and nothing
  else drops it - the file is only appended to and only `journal_prune` (explicit, off by default,
  copies the file aside first) removes a line. Nothing in
  `reagents.gst` or any save file is touched, ever.

### The blueprint page and its tooltip

Blueprints (`ItemArtifactFormula`) are collected the way relics are. The built-in group
`blueprint` (Blueprints, BLU) sits right after Relics in every view, and the catalogue admits every
shipped blueprint at every classification (section 7). The base game's page holds 914 (Legendary
479, Epic 131, Rare 117, Magical 187) on 12 pages (`uniq_p184` .. `uniq_p195` with the default
collect_mi=1, `uniq_p96` .. `uniq_p107` with collect_mi=0): 5 x 16 cells of
64 x 32, the scroll texture's own size and the Belts cell, so it needs no plate of its own. Reign of Terror holds 1,425
on 6 pages (15 x 17), DeathGrimDawn 1,805 on 23 (5 x 16; 447 records under
`records/shatteredaffixes/items/crafting/` stay behind DGD's own `[exclude]`). Nothing on the
deposit path is blueprint-specific: a blueprint is an `Item` (the gate byte `Item+0xC64` is on the base,
`GetMaxStackSize` = 1 like a relic), so the reagent path, the journal identity, the take and the
rescue are the relics'. The mod never reads or writes the engine's learned set (the map at
`GameEngine+0x36D60`, saved as `formulas.gst` / `.gsh`): learning stays the engine's right-click on
a blueprint taken back (`ItemArtifactFormula::Use`, possible with the caravan open). Crate's 11
test records at the folder's root stay in `catalogue.bin` and off every page (the template's
`[exclude]`): ten "Blueprint: Item Name of the Awesomesauce" with a 64 x 64
`items/misc/blueprint01.tex` that alone would double the cell, and `blueprint_test_potion.dbr`, a
second copy of the Elixir recipe.

**The line on a blueprint.** `ItemArtifactFormula::GetUIDisplayText` (Game.dll 0x3199B0) builds a
blueprint's rollover and, part-way through, calls the CRAFTED item's own `GetUIDisplayText` at
0x31A4CF: `mov rcx,[r15+0x1108]` (r15 = this), `mov r8,r13` (the formula's own line vector, kept
by the prologue at 0x3199E1), `call [rax+0x458]` (Item vtable +0x458). Without a detour on the
formula, the crafted item's hooked override (ItemEquipment, ItemArtifact, Item ...) runs inside
every blueprint tooltip and latches the crafted item's state, and the prefix tier puts that line
under the blueprint: the blueprint would read "In your collection" or "Not in your collection" by
the item it crafts. The sixth detour,
`hk_FormulaGetUIDisplayText`, drops any older latch, raises the per-thread `t_formulaDepth` around
its trampoline (lowered in a `__finally`, so an engine C++ exception cannot leave it raised) -
while it is raised `latchBody` latches nothing and counts `innerSuppressed` - and on the way out
latches the blueprint's OWN record over the complete vector (the exact tier, the shape of
ItemEquipment's detour). A blueprint no page holds (Crate's tests) gets no line at all, not the
crafted item's. `test_tooltip` case 6 drives the real detour bodies through stub trampolines
(`tooltipTestArm`): both ways round, the formula left unhooked reproducing the wrong line, the
exception path;
`test_bindings` case 8 pins the prologue at 0x3199E1 and the 31 bytes of the call at 0x31A4B6.
Proven offline only: the line on a page box and in
the bags, a blueprint's chat-link rollover, and whether the engine runs a `GameTextLineToString`
inside the formula's own text (the detour clears an older latch first, so nothing stale can be
claimed either way).

### Every bound is sized from the data

Invariant 8, in the three places a collection's size reaches (`src\ut_caps.h`, pure; `test_store
--caps` and the fixed rehearsal prove it):

- **The owned fold's walk buffer** (`ut_plate.cpp` `plateTableFold`, game thread). The label's
  `coll N/M`, the owned-only filter and the search sweep all read `g_owned`, the union of the
  engine map's held records and the private table's rows; the table half is one
  `journalCollectStored` walk into `g_tableBuf`. `journalCollectStored` returns the table's
  whole row count whatever it copies, and the buffer is sized from it: the first walk of a
  session counts, allocates the table's rows + 256 (260 B a row: 1.9 MB for the base game's
  7,111-row all-items journal) and copies; a walk that finds more rows than it holds - deposits
  since - grows it and walks again (`utTableWalk`, three tries), with one `owned: the private
  table's walk buffer holds N rows now` line per growth; a table that holds nothing allocates
  nothing. The ceiling, 65,536 rows = 17,039,360 bytes, protects memory only:
  past it the walk still COUNTS every row, the uncopied rows are added to the label's number
  and to `ownedTable=` as table-only rows (so the count stays the truth), the filter and the
  search see the copied rows, and one WARN says so. The journal reader's own limit is 64 MB
  (~120,000 rows of ~560 B), so the ceiling is reachable only by a hand-made file.
- **The engine-map walks** (`reagentCollectNodes`, `collectNodesSeh`; `refreshMapOwnedFromMap`,
  `reagentWalkPageRecords`, the store census, `plateOwnedRefresh`). The map holds the vanilla
  materials and whatever rows of ours an older build stored there - a format-3 journal's rows
  are map-owned, and the custom games' page sets were past 4,096 records before monster
  infrequents existed - so every walk returns the map's whole node count and `utCollectAll`
  grows the node vector to it (from 4,096). The in-order walks give up after 1,048,576 nodes
  (`kUtMapWalkCeiling`), so a corrupt or cyclic tree cannot spin the game thread.
- **The per-world prototype cap** (`ut_store.cpp` `protoCapNow`). `max(320, paintable x 2 + 64)`
  from the journal, ceilinged at `max(320, paged x 2 + 64)` from the view's catalogue (its
  `uniq-records.txt`, published by `reagentPageRecordCount`) and at 65,536 when that is not read
  yet; latched per world only once the catalogue is in. One prototype is an engine `Item`, at
  least 1,736 bytes (its `ItemReplicaInfo` ends at +0x538 + 0x190): base game 7,184 records ->
  ceiling 14,432, Grimarillion 19,089 -> 38,242, and a world builds one only per stored record
  it shows. The churn discount and the cap's one log line are unchanged; the heartbeat's store
  line reads `protos=<spent>/<cap>` (0 = not derived in this world yet) beside the display
  cache's `cached=N/CAP derived=D`.

The other fixed numbers in `src\` bound something no collection grows: text and line buffers,
pixel sizes, the frame's 160..512 boxes (`kMaxBoxes` 8192), a settings file (1 MB), a log, a
tooltip's lines, one row's count (65,536). Two record-sized bounds are kept on purpose: the
take watch's 256 (section 11) and the store census's 600 named rows, which only limits the
names a DEFECT line lists (its row counts are exact).

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
| **LITERAL** | a number compiled in | the fragile ones. Every survivor is a *field offset inside an object the mod already proved it holds*, read under SEH with a plausibility test. **None is a code address** |

Two orthogonal flags:

- **EARLY vs LATE.** Early bindings must all be resolved and confirmed before any hook is
  installed. Late ones need the exe's `.text`, which the Steam DRM stub only decrypts once the
  game is running - so they are scanned from the game thread, retried (about 40 attempts), and
  each turns only its own route off on failure. `bindingsLateReport()` emits the second summary.
- **CRITICAL vs ADVISORY.** A critical row has a consumer: something reads it, so a failure turns
  the mod off. An advisory row is decoded, confirmed and reported like the others, but nothing
  reads it, so a failure is one WARN. The offline test asserts the flag row by row.

**The text engine** (`text.renderWide`, `text.loadFontDirect`, `text.fontMetrics`,
`text.localization` by export, `font.styleSize` decoded from GetStyle's finder, `fntMetric.fields`
literal) is all ADVISORY: a miss turns off the wide draw, the rollover lines' language font (the
root font then stands in for their glyph check), the live metrics or the language line, and
the mod falls back to the narrow text call and the root font. The literal glyph-record offsets are confirmed
per world by the cross-check on the root font.

**The screen pad's input** (`dinput.getMouseEvent`, EXPORT, LATE, CRITICAL for its own route) is
the first LATE row that is not an exe signature: `DirectInputDevice::GetMouseEvent` / `::Update`
by decorated name in DirectInput.dll (1.3.0.8 rva 0x4840 / 0x31A0), confirmed on the game thread
against the LIVE device - slot +0x38 / +0x10 of `Engine::GetInputDevice()`'s vtable, and the
export's own `imul` saying 0x1C - with 40 tries 500 ms apart. Late because the device exists only
once the engine has initialised input; a failure turns only the screen pad's input off (one WARN,
and the late report names the row). The table has 46 rows. The screen draw site's literal
`hud+0xB800` (the item rollover, beside `hud.caravanWindow`'s `+0x4FD08`) has no row: it is
decoded per session from the HUD Draw's own call order (section 4, "Where the screen parts are
drawn"), and a refusal leaves the pad and the label in the window.

**The rollover exports are not rows.** The "already collected" line's seven Game.dll exports -
`Item`, `ItemEquipment`, `ItemArtifact`, `ItemRelic` and
`ItemArtifactFormula::GetUIDisplayText`, then `GameTextLineToString` and the `GameTextLine` ctor -
come by decorated name from `gd_exports_reagent.h` (generated by `tools\gen_reagent_header.py`;
`GD_ITEMARTIFACTFORMULA_GETUIDISPLAYTEXT` is the formula's name) and are resolved by `tooltipInit`,
outside the bindings table and its gate. They are optional as one group and all or nothing: a miss
turns off only the mark (the log names why; the compare byte is independent), and none of them is
counted in the INFO line's "N by export" or in `detours installed: N of N`. Six are detoured; the
ctor is only called. Before any detour the export-alias check (`exportAliasCount`) reads Game.dll's
own export address table: exactly one entry may point at each of the seven, because MSVC's
identical-COMDAT folding makes many exports share one address and a detour there would change every
one of them (none of the seven is folded on 1.3.0.8; the check runs anyway). `test_bindings` case 8
pins them offline, Game.dll read only: the seven resolve unfolded - Item 0x3118A0, ItemArtifact
0x316150, ItemArtifactFormula 0x3199B0, GameTextLineToString 0x302A90, the ctor 0x2FEC10 - and the
formula's prologue at 0x3199E1 and its crafted-item call at 0x31A4B6 are the bytes section 4
describes (45 checks in the suite).

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

**A binding.** Add a row to the table `kRows[]` in `src\ut_bindings.cpp` - name, class, phase,
how it is obtained, how it is confirmed, and the gate (critical unless the row says otherwise);
the gate and `tools\test_bindings.cpp` read that table, so a row only in a comment changes
nothing. Describe it in the inventory comment at the top of `ut_bindings.h` too - what it is, how
it is obtained, how it is confirmed, and the 1.3.0.8 evidence value. Call `bindingsNote(name,
value, ok, why)` from wherever it resolves. For an EXPORT, the decorated name has to reach a
header first: `tools\gen_exports_header.py` writes `src\gd_exports.h` (and
`tools\gen_reagent_header.py` `src\gd_exports_reagent.h`) from the `dumpbin /exports` listings
of Game.dll and Engine.dll that `tools\exports\README.txt` says how to make. Add a pattern to
`gen_exports_header.py`'s `SELECTION` (or an entry to `gen_reagent_header.py`'s table, which
must find it in the dump) and run the generator; a name neither selects goes into
`gd_exports_extra.h`, copied out of the dump by script. Never type a mangled name by hand.
Choose the phase honestly: **early means obtainable
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
as RVAs. Then run `tools\build_test_bindings.bat` with `UNIQUETAB_TEST_EXE_IMAGE` set (section
9) and confirm the count and the 1.3.0.8 RVA.

A signature in the **exe** is late (DRM). A signature in **Game.dll** is early - the module is
plain on disk.

**After a game update.**

1. The items the update adds need nothing: the stamp fingerprints every input archive (section
   7, "The stamp"), so the first start on the new game regenerates the catalogue, the pages and
   the plates.
2. Run the menu smoke (section 10) on the updated game. The log names any binding that broke:
   an early row is one `[E] the mod is OFF: the binding "<name>" could not be confirmed` line
   and nothing is installed (`bindingsGateFailure()` holds that name); a late row is one
   `bindings: the late binding "<name>" failed` line and only its route is off. A player's log
   names the same row.
3. Fix that row by its class (section 5's table, and the steps above): a renamed export needs
   new `dumpbin` listings and the generators; a decoder or a signature that no longer matches is
   decoded or cut again from the new bytes, with its exact hit count.
4. The suites stay pinned to 1.3.0.8: `tools\test_bindings.cpp` asserts 1.3.0.8's RVAs and hit
   counts over a 1.3.0.8 exe image, and `data\oracle\` is cut from a 1.3.0.8 game folder. Keep
   both (`UNIQUETAB_TEST_EXE_IMAGE`, `UNIQUETAB_TEST_GAME_DIR`, section 9) for as long as they
   do. To move the evidence to the new game instead, capture its exe image, change the expected
   values in `tools\test_bindings.cpp` and the evidence values in `ut_bindings.h`'s inventory,
   and cut `data\oracle\` again from the new game (`ORACLE.txt`; section 11, "The oracle is
   1.3.0.8's"): every byte that moves must be one the update explains.
5. The caravan window: the P and F lines of the generated `uniq-groups.txt` and the plates come
   from the game's caravan records and textures. A P line that differs from the oracle's means
   the window moved, and the cover, the frame and the plates move with it.

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

Nothing on the left ships. The package is the `.asi` and four documents; `data\oracle\` is the
harness's and is left out of the zip (`tools\package.ps1`), and its game-derived files are left
out of the public tree (`tools\publish.ps1` keeps only `ORACLE.txt`, the `README.txt` files of
`views\`, `nomi\` and `plates\src\`, `views\.gitignore` and `templates\`).
`data\oracle\ORACLE.txt` gives the order to rebuild it from a game folder. **The Python tools
find the game through `%GD_DIR%`, then
Steam's registry** (`tools\gdpath.py`): set `GD_DIR` to the 1.3.0.8 folder before running any
`tools\*.py`, or they read the installed game, whatever version it is. They need Python 3 with
`lz4`, `numpy` and `Pillow`.
`build_model.bat` repacks `catalogue.bin` only from a `catalogue.json` NEWER than it
(`pack_catalogue.py --if-newer`, section 3), so a stale json never packs over the committed
fixture.

**The oracle is a 1.3.0.8 artefact.** It is cut from a 1.3.0.8 installation, so the harness
must read one: `UNIQUETAB_TEST_GAME_DIR` names a 1.3.0.8 game folder, not whatever the installed
game has become - a later patch changes the database, and every byte comparison then fails by
design. That folder needs `x64\` (Game.dll, for the bindings suite), the four databases, the
`Items`, `UI` and `Text_EN` archives of `resources\` AND of each `gdxN\resources\` (the
expansions' items take their bitmaps and names from there; without them the catalogue loses
about half its items), and at least one custom game under `mods\`. `Level Art.arc` is optional to
the loader: it holds the icons of the four Lokarr pieces only (`level art/buildings/signs/sign_*01a_dif.tex`,
in `gdx1\resources\`), and a folder without it catalogues the four but leaves them off the pages
(`4 skipped`), the C++ and the Python alike. The reference folder therefore holds the 1.3.0.8
`Level Art.arc` of `resources\` (847,666,093 bytes) and of `gdx1\resources\` (293,908,330 bytes),
and the cuts page the four (vanilla: `0 skipped`); gdx2's and gdx3's `Level Art.arc` are not part
of the reference (the installed ones are no longer the 1.3.0.8 files; the loader treats a missing
one as absent). To move the oracle to a newer
game, rebuild all of `data\oracle\` from that game with the Python tools (`GD_DIR` naming it;
`ORACLE.txt` gives the order) and read the diff: every change in it must be one the game update
explains.

The Python tools under `tools\` are the **reference implementation**. The DLL does the same work
itself at run time, and the contract is that **the four text files it writes are byte-identical to
the Python ones and the overlay it writes decodes to the same records** - not the same bytes: an
LZ4 block has many valid encodings and the C++ writer stores literals where `tools\arzw.py`
compresses, which costs about 4% of file size and nothing else. `tools\test_catalogue.cpp` (via `build_test_catalogue.bat`) is the oracle: it runs
the C++ generator over a real installation and compares all four outputs byte for byte against
`data\oracle\`. If you change either side, that test is what tells you the other side moved.

Which group a record belongs to is data. Every catalogue record is matched against a rule set
(`src\gen\ut_groups.h`); the built-in one (`ut_groups_default.h`, compiled in, never written) is
the 24 slot groups (1.1.0's set; `utbLateBuiltinIds` counts from it), the Blueprints
(`blueprint`, right after Relics, every
`ItemArtifactFormula`; Crate's 11 test blueprints under one `[exclude]` above everything), the two
per-class groups `staff` and `spear1h` (empty in vanilla) and the Reign of Terror / DeathGrimDawn
data blocks. A player's
`uniquetab-groups.ini` is optional and ADDITIVE: parsed on its own (an error names its file and
line and refuses only it), its rules tried before every built-in rule, its new groups first in the
group order, a block of a built-in id adding rules to that group, its `[auto]`
last. A file byte-identical (CRLF folded) to a template an older build wrote is renamed `.old`
and not read (`isStockGroupsText`: the five templates older builds wrote, by LF size and FNV-1a
64). A
group with no label of its own is named with the game's word for its first class in that view
(`classWords` over the engine's class -> type-tag table; Reign of Terror's staff class is "Bow":
Bows / BOW, its one-handed spears Hunting Spears / SPE), plural, a family word in front on a
clash, and a derived tag. A G line of `uniq-groups.txt` carries the group's id and its
3-character tag after the seven older fields:
`G <n> <label> <cols> <rows> <cellW> <cellH> <count> <id> <tag>`; every reader takes both forms.
`build_uniq_db.py` reads the same template out of the header and carries a port of the matcher
(`--groups` merges a player's file in front like the DLL, `--journal`, `--stock-templates` copies
the templates older builds wrote out of `data\oracle\templates\`, `--buttons-from <folder>` writes the default buttons file beside
a `uniq-groups.txt`, byte for byte `utbDefaultText`).

The catalogue is the view's. `GameData` loads the base game and the view's layers; a record a mod
layer wins is collected by the mod rules (`catalogue_gen.h`). A record a journal of the view owns
that the rule or the groups file does not take keeps its journal row and is on no page (there is
no Retired group): the report has one warning, `N owned record(s) no group of this game takes -
kept in the collection, not shown: <up to 5>` (it names records the database lacks too, as the
label counts both), and the groups line
ends in `N owned not shown`; the journals feed that count only (nothing generated depends on them,
they are no stamp input), and one that cannot be read is a warning and a `?` count. The frame carries N boxes, whatever the layout needs, so a live
layout change never needs a new overlay: N = `utFrameBoxes` - the largest uncapped `utGridFit` of
the whole COVER over the view's distinct cell sizes, clamped to 160..512 (vanilla and DeathGrimDawn
234, Loot Ascension 180, Reign of Terror and X-Mod 512) - written on the F line
(`F <frame> N V x0 y0 2`) and padded with N - V `hide_NNN` fillers; every derived grid and a user
grid's plate lattice fit under it, and so does the live page (`g_nmax`). The engine has no box
limit (`ReagentWindow::Load` push_backs every entry); the mod's own arrays over the captured boxes
are vectors. `make_plates.py` takes N from the F line.

**Blueprints.** The catalogue admits every shipped `ItemArtifactFormula` at EVERY classification,
base and mod alike. Shipped is the existing filter: a record of any layer past the junk list
(`records/sandbox|creatures|mod_testgrounds/`, `/enemygear/`), no
`BLANK`, and its name tag - or the crafted item's - resolves. `catalogue.bin`'s classification byte knows only Epic and Legendary, so Common,
Magical, Rare and Quest blueprints are packed as Epic; Quest is storable for blueprints ONLY
(`storable`; Reign of Terror's 170 runeword-craft blueprints `rot_rw_*_craft` are Quest, and sort
with the Magical ones, rank 9 on the page), and any other record of a classification the bin cannot
store is skipped with the one existing warning - never a failed generation (ROT: 32 records, runes
and augments, no blueprint). `AscendantAltarFormula`, `ItemAscensionFormula`, `ItemRerollFormula`,
`ItemRandomSetFormula` and `ItemSetFormula` are dropped before the classification test. The icon is
`artifactFormulaBitmapName` - the 64 x 32 scroll in its rarity's colour,
`items/craftingparts/materials/blueprint_a001_magical` .. `a004_legendary.tex`; an empty one takes
the scroll of its classification (`scrollOf`; `items/misc/blueprint01.tex` for any other word),
never the crafted item's bitmap, which would make the cell as large as the largest crafted item. No
vanilla, ROT or DGD blueprint has an empty one; Grimarillion has about 1,985 (with the fallback its
Blueprints are 28 pages of 64 x 64 instead of 60 of 64 x 128 - see section 11). C++ = Python byte
for byte on vanilla, ROT and DGD; with collect_mi=0: vanilla 4,401 catalogue items, 4,211 paged,
109 pages; Reign of Terror 7,291 items, 7,043 paged, 73 pages; DeathGrimDawn 8,755, 7,328, 180
("Monster infrequents" below has the flag-on counts); Loot Ascension (Baul) 132 pages. A change of
the built-in template changes its FNV, so every view regenerates once.

Which custom game gets a view at all is `ut_viewprobe.h`'s answer: one that touches the caravan
(a `records/ui/caravan/*` record or a caravan texture - read from the index only, first) OR whose
layers add or override a record the mod rule could collect - the rule's record-local half: not on
the junk list (`records/sandbox|creatures|mod_testgrounds/`,
`/enemygear/`), a non-empty class other than `LootRandomizer`, a relic, an augment, a component, a
blueprint (`ItemArtifactFormula` at any classification; never one of the five sibling `*Formula`
classes) or Epic/Legendary, no `BLANK` in FileDescription. The name-tag half needs the text and
is not read, so the probe errs towards yes; the walk stops at the first hit. The Crucible (3,147
records, about 30 ms) gets none; Reign of Terror and DeathGrimDawn answer on the caravan.
`test_logview` checks the probe against `gen::collectItems` over each view: every record a game's
layer wins that the
rule collects is one the probe admits (measured, records won / blueprints among them: DGD
4,676 / 1,338, ROT 5,459 / 513, Grimarillion 10,807 / 4,455 - none missed), and the rule collects
none of the Crucible's records. A game that only DEMOTES a base unique and adds nothing gets no
view (the start-up catalogue still lists that unique).

`data\oracle\views\<tag>\` holds the Reign of Terror and DeathGrimDawn views' `catalogue.bin`
and three lists, cut by the Python tools from the 1.3.0.8 folder plus the installed mod
(`--custom`, `--out`); `views\README.txt` says how. The vanilla oracle beside them is untouched by
them.

Two traps:

- The fixtures are **CRLF**, because that is what the DLL writes. `.gitattributes` marks
  `data\oracle\` as `-text` so git stores the real bytes. Undo that and a fresh clone fails all
  three text comparisons.
- The stamp. `ensureOutputs` fingerprints every input archive (size + mtime of the four `.arz` and
  the Items / UI / `Text_<lang>` `.arc` of the base game and each expansion) into
  `catalogue.stamp`. A matching stamp costs one stat per archive. Regeneration writes everything
  to temporary names, re-reads `catalogue.bin` through the model loader, and only then renames all
  four into place. On any failure the folder keeps the files it already had.

`uniq-pages.arz` is written by the same pass. `checkPageCapacity` is the read-back: the
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

**Layout plates.** `gen::ensureLayoutPlates` paints a user grid's set from the view's own
`catalogue.stamp` and `uniq-groups.txt` (no catalogue walk, no overlay, no stamp written), from the
grid's origin with each cell size fitted to the rectangle (`utGridFit`, under the F line's N), and
removes the superseded `_L` names of that cover and tag. `ensureOutputs` and it share one lock:
one generation at a time (the view switch waits for a running repaint, at most about a second).
The hash
(`gen::layoutPlateHash`) is the low 32 bits of FNV-1a 64 over the stamp text, the canonical grid
override, the fitted cells, `plate_ground` and the lattice rule line `lattice=2`
(`ut::kUtLatticeRule`; the catalogue suite's golden value is `604a59c8`, `048edde9` without the
line). The painter frames exactly `utGridFit`'s cols x rows with `ut::utLatticeFrame` (a frame cut
at the plate's edge, never dropped), the row gap the plan fitted with. `tools\make_plates.py
--grid x,y,w,h --layout-hash H` is its reference, byte for byte.

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
`ut_plate`'s size check and the plain cover draws.

**The pad on the materials page** (button MAT) is the pad pinned to the screen when `pad_screen`
is in force (whole, at its corner; `padmat_` is not used then), else the player's `padmat_<W>x<H>`
rectangle when that is set, inside the page and fits the buttons, else the pad in force (`pad_` or
the derived band), drawn WHERE IT IS - over whatever live boxes of the game's own page lie under it
(the pad draws from the Draw tail, after the engine drew the page). A point inside it is the
pad's: ut_plate's mouse slot runs `utPadLeftDown` (`src\ut_padmat.h`) and keeps a LEFT DOWN there
from the engine - a button acts, the ground between the buttons does nothing - and a RIGHT DOWN
too (the box path takes an item on type 2 as on type 1, exe 0x132DD9); with an item on the cursor
the click is the engine's deposit, as on every page. For every event it passes there it puts the
handler's hovered-box answer (`*out`, exe 0x133104 -> 0x135E23) back to the caravan's, so a
covered box shows no tooltip. The boxes under the pad are simply covered. One line per change
names the rectangle and the boxes it covers (INFO when it covers some; `utMatPadResolve` /
`utMatPadText` in `src\ut_layout.h`, `matDecide` in ut_panel.cpp). Vanilla, X-Mod and Loot
Ascension keep every materials
box below the band (`test_catalogue --geometry` reads their box records). A folder that cannot be written is one WARN
line naming it, never a failed generation. An install of an older version keeps the plates it
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
Stamp format 9 (the frame's size is in it; a new format regenerates every view once): the lines also carry
`groups=builtin:<FNV of the built-in template>[+user:<FNV of the player's groups file>]`, so a new
build's built-in groups and an edit of the player's file each regenerate every view once; a refused
user file leaves `builtin` alone.
The stamp has NO `layout=` line: the grid overrides are not inputs - they move only the layout
plates, which carry their own hash (`gen::layoutPlateHash`) - and the journals are not inputs
either. The start-up view writes to `<mod folder>` itself; a custom view
writes the same six outputs to `<mod folder>\mods\<leaf>\` (`\basemods\` for the /basemods variant), so an installed copy that never
enters a custom game is unchanged and leaving one costs nothing. A custom view's plate always
carries the cover size and the view's tag, a vanilla-sized cover included
(`uniq_plate_64x96_346x625_lootascension.tex`, `uniq_plate_64x96_438x627_deathgrimdawn.tex`), and
its stamp carries a `plates=tagged` line so a view generated without it regenerates once;
the unreadable `.arc` of a custom game is skipped with one warning naming it (DeathGrimDawn's
`text_ko.arc`), in `ModView` and in `ui_geometry.py` alike; `ut_plate.cpp` builds the name
from the live view and `make_plates.py --plate-tag` is the reference for it. The vanilla and
`/basemods` names carry no tag, because installs of older versions already hold their plates
under those names.

`test_catalogue --view` asserts the **written** records, not the generation report: it reads
`uniq_frame.dbr` back out of both archives (the Baul view: `513,70`, 180 boxes, 237 pages with the flag on, every
page box on the derived `128,76` 310x550 grid; the start-up view: `0,88`, 234 boxes on `105,76`).

The readers **stream**. An `ArzArchive` or `ArcArchive` keeps its string, record, part and
entry tables - the index - and holds the file open; bodies are read and decompressed one at a
time. Nothing the size of an archive is ever resident, and `--ensure` fails above a 200 MB
peak so it stays that way: a Steam Deck cannot spare the 1.7 GB a whole-archive read takes. The
budget is a test's, not the game's: the generation's OWN heap (`heap_meter`, below - the count
the DLL reports in the game, where nothing enforces a limit) held to 200 MB (`kPeakBudgetMb` in
`tools\test_catalogue.cpp`), judged in `--ensure`, the view pass and every `--dbmod`; the harness's
process peak is printed as information only, because the harness adds about 30 MB of its own and
its peak only grows across passes. Measured with collect_mi=0 (own / process MB): `--ensure`
96.3 / 121.2, the Baul view 106.1 / 136.5, Reign of Terror 144.7 / 176.6, DeathGrimDawn 132.0 /
159.1, and two big mods read with the C++ harness only, Grimarillion 172.4 / 203.8 and Dawn of
Masteries 170.0 / 200.3 - every own heap inside the budget; the process line is information
only. About 10..18 MB of each is the overlay's in-memory box records (Grimarillion's 14,483).
`--ensure` also generates the `/basemods` start-up view against the real game folder, and fails
when that folder's `mods\` holds no custom game, so the budget is proved on the layout players
have. In the game the process working set cannot isolate the generation - the engine builds its
main menu on the other threads in the same seconds - so `src\gen\heap_meter.cpp` counts the
heap the generating thread holds (it replaces the module's global `operator new`/`delete`; off
that thread they are `malloc` and `free`). `N MB for this` in the `catalogue: built` line is that
count, and a debug line beside it gives the process working set and peak before and after.

**Monster infrequents.** `collect_mi=1` (the default; `GenInputs::collectMi` defaults true, the DLL
always sets it from the ini) admits every equipment record of the 25 equipment classes whose OWN
`itemClassification` is Rare (the engine's MI symbol rule, `Item::GetSymbolBitmapName`; random
rares have no record) - faction-store and blacksmith-crafted rares included - through the shipped filter
(base) or the junk list (a mod layer), minus the per-game `[exclude]` blocks (data, section 3's
`ut_groups_default.h` row; Grimarillion's 863 "Replica" items OUT). Every entry gets the KIND mark
(catalogue.bin flag bit 8, `ItemFlag::MonsterInfrequent`; the format is 1: the loader does not
validate unknown flag bits and no older file has the bit set); Rare is packed as Epic (the
classification byte knows Epic / Legendary only, the same narrowing as the 21 Rare relics). The
groups place them through `[view:collect_mi]` (class + `rank = Rare`, after the uniques: the page
order is rank then name, Rare after Epic). With the flag off no MI is admitted - the catalogue byte
for byte the flag-off fixture - and the stamp has no `collect_mi` line; with it on the stamp says
`collect_mi=1`, so a flip either way regenerates every view once (at its next generation: start-up,
a custom view's entry). With the flag going off an owned MI is in no catalogue and on no page -
its journal row and its stored copy stay, the label counts it in `+N not on these pages`, and it is
back on its page when the flag is 1 again. Counts, flag off -> on (C++ = Python byte for byte on all six cuts): vanilla 4,401
-> 7,316 catalogue items (2,915 MIs = 2,393 monster drops + 466 faction + 50 crafted + the 6 quest
rewards of `records/storyelements/rewards/`), 109 -> 197
pages, 25 groups, six plates (the MI spears make Spears 64x128, so the flag-on set has no
`uniq_plate_32x128.tex`);
Reign of Terror 7,291 -> 10,523 (3,232 MIs), 73 -> 109 pages, 29 groups; DeathGrimDawn 8,755 ->
11,865 (3,110), 180 -> 282 pages, 29 -> 30 groups (`spear1h` "Artifacts", its 12 Rare crystal
weapons); Grimarillion 15,172 -> 21,214 (6,042), 19,089 paged on 133 pages with the flag on. Every
cut holds the Totally Normal Shield's three versions on 1H Maces
(`records/endlessdungeon/scriptentities/portal_s01..03.dbr`, Legendary one-handed maces of gdx2
that no mod overrides; the folder's other 64 records are ScriptEntity and Decoration props, and
none of the eight mod databases measured has a record there). These are the
reference folder's, whose `Level Art.arc` (base and gdx1) gives the four Lokarr pieces their
icons: vanilla Helms 609, Shoulders 601 (its 16th page), Chest 455, Boots 123 with the flag on,
7,126 paged on 197 pages, 0 skipped (flag off 289 / 210 / 236 / 107, 4,211 paged, 109 pages);
Reign of Terror also pages Merman's Sprocket (`records/items/d2items/gearfeet/d005_feet.dbr`,
which names the Lokarr boots' icon), and ROT and DGD keep one record each off their pages as
before (ROT's Nadaan's Reach names an icon the mod does not ship); and on the
installed 1.3.1.1 the C++ and the Python agree byte for byte with all sixteen records outside
`records/items/` paged (7,374 items, 7,184 paged, 197 pages, none skipped). Own heap with
collect_mi=1, on the reference folder with its two `Level Art.arc`: `--ensure` 103.5, ROT 154.9,
DGD 140.7, Grimarillion **188.1 MB of 200** (11.9 MB spare - the tightest; 189.8 MB on a full
1.3.1.1 installation, whose four `Level Art.arc` indexes hold about 8,000 entries; GrimArmory ~110 expected, not measured). **The fixtures:** `data\oracle\` and
`data\oracle\views\{reignofterror,deathgrimdawn}\` are the FLAG-ON cuts (the shipped default;
Python `--collect-mi` on `build_catalogue.py` and `build_uniq_db.py`), `data\oracle\nomi\` holds
the flag-off cuts (vanilla's five files + `catalogue-stats.md` + `plates\uniq_plate_32x128.tex` +
the two views), so the flag-off path stays pinned. `ORACLE.txt`,
`views\README.txt` and `nomi\README.txt` (all three in the public tree) give the cut commands.

## 8. The journal format

`uniq-items.jsonl` (and `-hc`), JSON Lines, UTF-8, LF. The full spec is the file header of
`src\ut_rescue.cpp`; this is the shape.

**Where the files are.** One set per (collection mode, custom game):

| view | journal | exports, prune copies, rescue report |
| --- | --- | --- |
| `vanilla`, `basemods` | `<journal dir>\uniq-items.jsonl`, `-hc` | beside it; the report in the mod folder |
| `custom-<name>`, `basemods+custom-<name>` | `<journal dir>\mods\<name>\uniq-items.jsonl`, `-hc` | beside it, report included |

`<journal dir>` is the mod folder, or `journal_dir`. The format is the same in every file. A
row is written to exactly one of them - the one the deposit's world had open - and nothing sweeps
`mods\`: it holds user data (neither the generator, which only deletes its own `.tmp` names,
nor `undeploy.bat`). The key stays `custom-<name>`; only the folder takes the bare name
(`utCustomGameRel`). THE MOVE: older builds used `views\custom-<name>\` (and
`views\basemods+custom-<name>\` for the /basemods variant's generated set). `utFoldersMigrate`
(`ut_folders.h`, called once in `workerMain` right after `configReload`, before `generateEnsure`,
`journalInit` or any view entry) renames them under the mod folder and under `journal_dir`:
`views\custom-<leaf>` -> `mods\<leaf>` first, then `views\basemods+custom-<leaf>` ->
`mods\<leaf>\basemods` (creating `mods\<leaf>` when absent; a /basemods folder whose plain folder
did not move waits for it, so a rename that failed on an open file never leaves a `mods\<leaf>`
behind to block the retry at the next start); `MoveFileExW` without
`MOVEFILE_REPLACE_EXISTING`, so a target that exists refuses - both folders kept, one WARN naming
both, the new one used; never a merge, never an overwrite; `views\` removed only when empty
(otherwise one WARN listing what is left); one INFO line per base folder,
`files: the custom games' folders moved from views\ to mods\ - N folder(s): ...`; silent when
there is no `views\`. A rename that failed on a held file is retried at the next start only;
nothing retries it where `journalFollow` / `viewEnterCustom` create the folder (section 11). One-way: an older build does not read `mods\` (README, *When something goes
wrong*, gives the move back). `ut::journalFollow(mode, viewKey)` is the one switch; the key is
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

**The record index.** A row is found by its record through a hash beside the entry list
(`ut_rescue.cpp`, "the record index"). `findEntry` - every accessor, every mutation, the
caravan-open reconcile's `storeCount` / `journalMarkStored` pair on the game thread - and the
reader's duplicate check ask it instead of scanning, so both are linear in the rows (offline,
`test_store` passes 12 and 20: 7,111 lookups 376 ms scanned, 1 ms indexed; 20,000 rows read in
4.1-5.2 s, now 58 ms; 20,000 lookups 4.0-4.9 s, now 4-6 ms). The key is the record folded as
`_stricmp` folds in the "C" locale - ASCII A-Z -> a-z, nothing else; a slot holds the row's
position and the hash, and equality is `_stricmp` itself. The first row of a record wins (a list
with two rows of one record - an old binary journal can hold that - answers with the first, as the
scan did). A key with a byte >= 0x80, or a list holding such a record, is answered by the scan,
unchanged. A failed allocation leaves the index unbuilt and the scan answers until the next
mutation rebuilds it: it can be missing, never wrong. **Its lock is `g_cs`**, the entry list's:
every index operation runs with it held, and the reader parses into a private list with that
list's own index and publishes the pair under the lock (`readTextFile`), so a lookup during a mode
or view switch sees the old pair or the new one, never half of either. An append adds one slot
(the array doubles at half full: 8 bytes a slot, 512 KB at 20,000 rows), an overwrite in place in
another spelling of the same key moves nothing, and every change of length or order - a remove,
the deposit's rollback, the prune, the switch's clear, the binary migration - rebuilds it. A
lookup allocates nothing. **The invariant: after every mutation the index answers exactly what a
scan of the rows answers** - `journalIndexCheck`, which `test_journal` runs after every mutation of
every case (the `journalTestIndexAudit` latch, which the mod never sets) and `test_store` at
7,111 and 20,000 rows. The index is never written: the file, the row order on disk and the
identity rules are what they were, and the journal is still the only truth.

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

**The affix slots are seen set.** They need no format change. `ItemReplicaInfo::WriteProperties`
(Game.dll 1.3.0.8 rva 0x570810) writes the transfer stash (the save at 0x2C5F01 names its file
through 0x2B4AC0 - `"transfer"` -, serialises through 0x2CA4B0 and calls `SaveManager::DirectWrite`),
so a `transfer.gst` item's fields are WriteProperties' fields in order: strings +0x08 (base),
+0x28 (prefix), +0x48 (suffix), +0x70 (modifier), u32 +0x68 (seed), strings +0x90, +0xB0, u32
+0xD0, string +0xD8, u32 +0xF8, +0xFC, strings +0x120, +0x140, then +0x160 / +0x178 / +0x180 /
+0x17C. Two decoded `transfer.gst` files hold six affixed items with field 2 under
`lootaffixes/prefix/` and field 3 under `.../suffix/` - one of them the MI `b305c_ring` (Chthonian
Devourer, prefix `b_ar007_ar_b`, suffix `a030b_off_dmg%fire_05_je`, seed 2115467035) - and
`test_journal` round-trips that roll slot for slot (all ten string slots, all seven u32s, the take's
`identityBuild` and the rescue's `utBuildSwapOverlay`, the file re-read in a new process, the
`.gds`). The ascendant pair stays POSITION ONLY. An identity-less row (a synthetic test
journal's MI: only `baseRecord@008` and a seed) is a plain item, count 1: a take hands back an MI
without affixes, and the cap applies to it like any other.

## 9. Tests

Sixteen offline suites. **All sixteen must be green before any commit that touches `src\` or
`tools\`.** Run them sequentially - they share build output directories.

| suite | what it covers |
| --- | --- |
| `tools\build_test_config.bat` | the settings files, the nineteen sections at the top of `tools\test_config.cpp`: every row's default matches its member, the four visible keys (`collect_mi=1` the default; `max_per_record` and `rescue` hidden and "(untested)") and `journal_dir` and the seven `[keys]` rows the main-only keys (no other flag; no dropped / merged / derived / moved key is a row; the `[keys]` rows are `ut_keys.h`'s actions in order, `utKeyParse` their check); the fresh template byte for byte (the header, `[general]`, `[layout]`, `[keys]` with its seven commented rows, the `[advanced]` blob, both on one tab column); a round trip and a hidden key live in its own row; the version bump keeping the player's values, and a real 38 file -> 42 (the 21 dropped keys named, the state keys seeding `uniquetab.state`, a merged key's WARN naming its new home); 4b a 39 file -> 42 gaining `collect_mi=1` (40's step; the current file byte for byte), `collect_mi=0` by hand read back with no rewrite; 4c a 40 file -> 42 (41's step, `max_per_record` and `rescue` out of `[general]`: at their defaults the current file byte for byte, `max_per_record=2` / `rescue=1` live blob rows, rescue's write-back commenting it again); the clamp; the one-shot write-back; RESCUE-NOW; `enabled=0`; the path rules (both `.asi` placements); the keyed `[layout]` families and the multi-key persist; a custom game's own `mods\<name>\uniquetab.ini` since 42 (the seed: the main file's values byte for byte through `configRenderView`; the same template as the main file but for the header and the commented `journal_dir` line; NO inheritance - a current game file without a line has the default, a main-file edit changes nothing in the game, both files polled; `enabled=0` / `collect_mi=0` / `rescue=1` per game; `journal_dir` refused with a WARN; the persist route - `rescue=0`, the editor's SAVE with a screen key, `layout_edit` into the game's file, `journal_dir` into the main one; `default` refused in a current file; shared by `basemods+custom-<name>`; deleted -> seeded again; the 41 -> 44 migration of a four-key file: lines kept, `default` dropping the main line, the rest from the main file, one INFO line, once; the 42 -> 44 bump of a complete game file: its hidden rows stay the DEFAULTS under a main file with `max_per_record=2` / `edit_button=0`, `edit_button` a commented row at 1, `configViewIntFor` the same, once); 14b the custom games' folders (`mods\<leaf>\`, `mods\<leaf>\basemods\`; illegal names never a folder); `uniquetab.state` per view folder; the old `[view:<name>]` blocks moved on the bump and ignored in a 39 file; the screen keys `pad_screen` / `label_screen` (written only while set, a bad value refused with one WARN, one pair per settings file: a custom game's SAVE into its own file, vanilla's into the main one); 18 `[keys]` (a remap by hand in force at the next reload, kept by a one-shot's write-back and LIVE through the 43 -> 44 rewrite; a 43 file without the section rewritten once into the 44 template; a refused value one WARN per edit naming the reason, the default kept, an empty value the default; the conflict pass's WARN naming both rows; a `[keys]` line in a game's file one WARN and ignored, the main file's bindings in force in the game and its edits within a reload, a re-seeded game file without the section); 19 `zoom_pct`: the hidden int row of `[layout]` (100..200, default 100, `kUtCfgRefuse`), its commented line in a fresh file, live by hand, 250 / 99 / 1500 / `big` refused with one WARN each (100 kept), the 43 -> 44 merge of the main file byte for byte, one value per settings file |
| `tools\build_test_journal.bat` | the JSON Lines reader and writer, the format gate, the migration, the wrap-proof bounds; the affixed MI round trip (108 checks: `b305c_ring`'s decoded prefix / suffix / seed and every other slot filled, the shipped slot scan, the deposit commit, the file re-read in a new process by `--verify`, the take's and the rescue's builds slot for slot, the `.gds`); `--case-folders` (the `views\` -> `mods\` move: both folder kinds, `journal_dir`, a taken target refused with a WARN, a held file's failed rename retried at the next start, `views\` kept only when not empty, nothing else moved, the collection re-read from `mods\<leaf>\`); `journal_guard.ps1` over an old-layout `views\custom-legacy\` journal (refuses); `--case-dupcase` (one record in three spellings on disk: one row, in the first line's place, holding the last line's content; a re-deposit, a count, a mark, a remove and the prune through other spellings; 100 rows grown in one case and found in the other, 34 removed); THE RECORD INDEX audit around every mode: each mutation of every case checks the index against a scan of the rows (section 8), and a failure fails the process |
| `tools\build_test_store.bat` | the private table; the FIXED rehearsal (pass 4b, its own `build\test\store-allitems`): a copy of `data\oracle\journals\all-items-mi.jsonl` - every collectible record of a 1.3.1.1 base game with monster infrequents, count 1, 7,111 rows - read, its record index checked against a scan, counted exactly as its header says, its 7,111 lookups timed, every row walked by the owned fold's own walk and written back whole (FAILS when `data\oracle` is there without it, skips loudly without `data\oracle`); `--caps` (case 20, `build\test\store-caps-ordered` and `-shuffled`): a 20,000-row synthetic journal in record order and shuffled - the walk buffer grows from the data, a 100-row buffer still gets the whole count, the copies add up exactly, the same order-independent digest both ways, a 4,096 ceiling copies the file's first rows while counting all, 300 deposits grow it again, the record index checked against a scan after the read and after the deposits, 20,000 lookups and the reconcile's two a row timed with every row found at its own count -, the prototype cap's derivation and the map walk's growth; the database follows the view (`--dbviews`): rows on both sides of a view change, a record the database cannot build skipped by the rescue and never zeroed; `--mi` (case 19, its own `build\test\store-mi`): the KIND seam (`catIsMi(entry) is DECLARED`), the cap against an independent oracle over 75 (held, max, kind) combinations, a second roll refused with the first named, the belt (`journalTestSetKind`) |
| `tools\build_test_rowfold.bat` | the row and page arithmetic; section 12: `utRowKeep` across `zoom_pct` 100 -> 150 -> 100 with the owned-only filter on (Helms 5 x 8 / 3 x 5, every first visible item kept on screen, the clamp after a take) |
| `tools\build_test_layout.bat` | the layout model (`src\ut_layout.h`): the rectangle and cover-key text forms, the caption arithmetic, the derived pad fitted back cell for cell, every G line of `data\oracle\uniq-groups.txt` reproduced by `utGridFit` (7- and 9-field lines), the frame size per cover (`utFrameBoxes`: vanilla 234, Baul 180, ROT and X-Mod 512, the floor for big-only cells, no cells or a tiny cover; the vanilla F line = the cover's fit while its largest group stays 160; a player's ROT grid 29 x 11, 29 x 5 under a 160 cap; ROT belts 15 x 17), the label bound, every `utLayoutResolve` refusal and fall-back, the materials-page rule at seven UI scales (the pad in force where it is over any boxes, vanilla clear of its 24 boxes at every pad key, `padmat_` winning or refused off the plate / too small), a player's Reign of Terror rectangles at nine UI scales over two box models (the top 15 boxes, all 116 box records) - always the whole pad where it is, the boxes it covers counted (29 of 116) - and the log's text, a real ini reproduced exactly (every legal pad key, the caption and the label), a user grid and pad moving the page, the pad keyed by the G lines (vanilla (25 G lines: 27 buttons, 10 x 3 cells of 30 x 14 at scale 1, caption 10), ROT (29 groups, 31 buttons: 11 x 3 of 27 x 14, caption 9) and DGD (29 groups) tables at seven UI scales, Other as a button of its own, a paged 120-group table); the screen parts (the text form, the corner placement at three resolutions and scales, the shift, the record <-> client round trip, every screen refusal and the fall-back, the label's own key, the materials page keeping a pinned pad whole, the editor's nearest corner); the screen draw (the frame tiles exactly, the published rectangles as drawn and as the input reads them, the site proof - with the exe image, over 1.3.0.8's HUD Draw); 4b the painted lattice (`utLatticeFrame`: a player's ROT grids `46,72,928,520` / `46,218,928,374` at the seven ROT cell sizes - every fitted cell framed, 29 columns at 32 px, the 29th flush with the plate and framed, a rule that drops a cell whose outset leaves the plate pinned as giving 28; a corner cut, a cell off the plate, vanilla's derived grid never cut); 10f2 a pinned part's per-cover line ignored (a logged world replayed, the label placed, its key refused for its own reason - judged again -, both parts pinned, both waiting, the source path, the DEBUG text); 12 the layout plates' resolver `utLayoutPlatesGrid` seeing the screen keys - a logged DGD SAVE replayed (`grid_438x627=101,33,320,594`, `pad_438x627=10,4,413,90`, `pad_screen` set: the thread paints the user grid, a screen passed in changes nothing; without the key the grid is refused and no set named), the live page and the thread agreeing at UI scale 0.878 on 2560x1440 (also before the first draw and without the key), a screen pad over the grid (live derived, thread user), and `utPlateSetChoice` / the set names over every thread x live pair. Without the fixtures the grid half says SKIPPED; section 13, `zoom_pct`: the arithmetic, zoom 100 = every G line of vanilla, ROT and DGD (cell and grid) and the user fit (the identity gate), the fits at 125 / 150 / 200 (vanilla Helms 4 x 6, 3 x 5, 2 x 4; Chest 2 x 2 at 200; two user grids), the per-group clamp (a 100 x 300 grid: Helms 157 %, Rings 200 %; ROT's 64x160 at 186 % in a 300 px grid) and 8,820 clamp cases, the union cell, the editor's smallest grid, the layout plates' text at a zoom, `utLiveGridKind` |
| `tools\build_test_layoutedit.bat` | the in-game layout editor's pure half (`src\ut_layoutedit.h`): the handle size and the toolbar, every clamp and the grid's whole-cell snapping, the polled button (swapped too), the one-shot key, the hit-test order, every transition (pass, drag, SAVE, SAVE refused, CANCEL, RESET, the polled and the event release), every cancel rule mid-drag (gone, layout, cover, scale, materials page, focus), the save handover and its timeout, the parking rule, every refusal through `utLayoutResolve`, the values SAVE writes, a wide cover, the header's purity; a pad above the label band passes; the pad's click rule (`utPadLeftDown`: inside the materials page's pad a button acts and the rest keeps the click, an item on the cursor is the engine's, the collection pages' order); the screen mode: the pad and the label across the plate's edge both ways (move and resize), clamped to the screen, the grid never; the toolbar with the pad in both modes; the keys and files SAVE writes per mode; RESET per mode; the refusal words; every cancel rule mid screen-drag; the screen unknown; a scale under 1; section 10: CANCEL in every state - mid-drag, while saving, with an item on the cursor - through `utEditToolbarAt`, and a LEFT DOWN during a drag ending that drag before it is judged; section 11: the border `utEditWindowClient` (vanilla's plate 0,1,438,627), no ground with every part in the window, the toolbar-off-the-plate ground, the screen pad's ground, the note (2 px, the age, used up, NaN), and a logged in-window click refused unproven / accepted with the note - 224 checks in all |
| `tools\build_test_tooltip.bat` | the text fold and the rollover line, and the two lines' text: from `[tab]` of the buttons file, UTF-8; case 6 (11 checks) the REAL detour bodies through stub trampolines (`tooltipTestArm`): a stored blueprint whose crafted sword is not stored reads "In your collection", an unstored one whose crafted ring is stored "Not in your collection", the formula unhooked reproducing the wrong line both ways, one inner latch suppressed per blueprint, a plain item right after keeping its own line, an excluded test blueprint getting no line, and a C++ exception out of the formula's trampoline unwinding untouched with no suppression left; case 7 the third line "(another roll)" (12 checks) and `tooltip.roll` from `[tab]` reaching it with its leading blank |
| `tools\build_test_bindings.bat` | every pattern scanned over a decrypted exe image: exact hit counts, the 1.3.0.8 RVAs, the decoders, and the class/phase/gate classification row by row (46 rows; only the exe signatures and `dinput.getMouseEvent` may be LATE); the twelve text-engine names in Engine.dll and the style-size decoder; case 8: the seven tooltip exports by name in Game.dll, unfolded, at their 1.3.0.8 RVAs, the formula's prologue at 0x3199E1 and the crafted-item call at 0x31A4B6 - 45 checks in all |
| `tools\build_test_groups.bat` | the groups file parser (every error refuses the whole file, naming the line), globs, first-match precedence, view sections, auto/Other groups and no Retired (`retired` an ordinary id), the reserved ids (`next` included), the journal count scan, both G line forms against every G line of the oracle; the template loader (no writer) and the player's file merged in front (an `[exclude]` on a built-in group, a built-in id given a rule and a label - place and tag kept -, the same label on two groups = two groups, the player's `[auto]` last, a user view block); the class words over six views' words (plural, the clean word, the family word, the tag ladder, the 25-class table; ROT Bows / Hunting Spears, a label taken twice, BOW and SPE taken); the `.old` retirement of this build's template and the five older ones (`data\oracle\templates\`); the files (none, a user file added in front, a bad file refused naming `<path>: line N:`); the ROT and DGD view groups (Bows 76 with Memory, Mist and Leaf; javelins in Hunting Spears; DGD's crossbow-class bows before Guns, gems and runes at every rank); the Blueprints built-in (row 25: Blueprints / BLU / `ItemArtifactFormula`, right after Relics in every view, one block, every rank, never derived; ten placements - the four base rarities, the Elixir recipe, the three test records on no page, a ROT Magical and a DGD Legendary mod blueprint on Blueprints; the oracle's 25 G lines); the monster infrequents (`[view:collect_mi]`'s DGD rule + 25 class rules in order, no group added or moved; each slot class at Rare off the pages with the flag off, on its group with it; the per-game excludes - 16 placements -, a ROT Rare bow / javelin -> staff / spear1h with the flag, a base Rare rifle -> 2H Guns, an MI on no page off, Helms on) - 583 checks |
| `tools\build_test_buttons.bat` | the buttons file (`src\ut_buttons.h`): the grammar and every refusal naming its line, buttons -> groups and its errors (an id on two buttons, an id no group of the game can have), the union in group order, `[unassigned]` computed and rewritten byte for byte (LF / CRLF, trailing blanks, a user comment, no section, an unterminated last line), the generated file = the Python twin's fixtures `data\oracle\**\uniquetab-buttons.ini` for vanilla / ROT / DGD and the identity gates (the generated file = the built-in pad, captions, names, templates and rollover lines), the fold of an old `uniquetab-text.ini`, the font folder the letters derive; the pad by buttons (the wheel's walk in the pad's order - a DGD file with `[BLU]` appended after OTH, a reordered file, a player's ROT file -, skipping `[unassigned]` groups, a several-group button's summed counters); `tooltip.roll` (not written, built in, quoted from `[tab]`, unquoted trimmed, empty refused); one grid per button (`src\ut_padpage.h`'s `utUnion*`: the plan, the rep and the members, its cell - the smallest view cell, the next one, none -, the concatenation order, the rows on the derived and a player's grid; ROT's oracle groups with a player's file - `[SPE] groups = spear1h, staff` - as one union of 95 entries, 76 bows then 19 hunting spears, cells 64x160, deposit/take by record, the OWN filter, Ctrl+wheel one stop, the search marking SPE from either group, a restored `uniq_group=24` row); and the words' engine (`src\ut_textcfg.h`): the label 4000/4000 identical to a copy of 1.1.0's label code over the real pen rule, the chain with its glyph callback, UTF-8 edge cases, the retired text file's parser for the fold, the header's purity; the font that follows the letters through the FNTD copies (`testDeriveDynamic`: the derivation order, the identity gates with and without the FNTD copies, Han / kana / Hangul / Thai, the INFO text, every case of the WARN's advice); 1.1.0's identity gates over 1.1.0's 24 groups (`kGroups110`), BLU's caption and name from its G line, the ROT one-grid case over 29 groups; `testOfferNew` (81 checks): the late list = the built-in rows after 1.1.0's 24 and then the template's per-class groups (`staff`, `spear1h`: no label or tag of their own), per fixture (vanilla, ROT, DGD) a default file naming the game's late ids on its "added once" line and offered nothing and its deleted `[BLU]` never offered back, an old file (the fixture minus `[BLU]` and the line) given `[BLU]` last before `[unassigned]` and byte for byte otherwise, a second load adding nothing, LF, no `[unassigned]` and an unterminated last line, a group already listed, a taken tag (`[blu]`), `[unassigned]` before the buttons, a game without the group; `testFlagOnArtifacts` (28 checks): DGD's flag-on table (30 groups, 24 ART before 25 REL), its generated file with `[ART]`, a file written with the flag off offered `[ART]` ONCE (after the last button, the line `blueprint, spear1h`, every other byte kept), a second load nothing, a deleted `[ART]` never back, a file whose `[unassigned]` already lists spear1h left alone; ROT's default file naming `blueprint, staff, spear1h`, a file without BLU / BOW / SPE and the line given all three once; `[tab] edit.caption` (read, never written, applied to the pad's fourth fixed caption, refused like the others; `[EDT]` a legal tag) - 989 checks in all |
| `tools\build_test_text.bat` | the text engine's pure half (`src\ut_textmeasure.h`; the language sections below are the retired text file's, which the one-time fold still parses): the pen rule over a glyph cache equals `utTextWidthPx` for the 26 captions and the label lines at sizes 1..64, at the seven UI scales and for random strings (the "nothing moved" proof); the cross-check finds every kind of difference; with a Fonts.arc (`UNIQUETAB_TEST_FONTS_ARC`, else the game folder's or the dbmods folder's game) the real root font passes the mod's per-world cross-check, every baked language copy is bounded by the table, Cyrillic measures with ru/; UTF-8 to UTF-16 edge cases; the language font path; the header's purity; the pad's per-caption clip; with Fonts.arc the live chain's glyph check over the real ru/ and root fonts; a role's own font (the arc entry, when it is used, the root font without a key); with Fonts.arc every folder the template names holds a baked savapromedium, and the fonts per role over the real ru/ and root copies (an English game with Cyrillic under [text]; a key naming the root font = the root font's widths); the language policy over the real fonts (no file on a Russian game = the built-in English on the root font; `language = ru` + `[text.ru]` + the font keys = Cyrillic in ru/ on either game language); with Fonts.arc the font that follows the letters over the real glyph tables (Cyrillic -> ru, Greek -> el, sz / u-umlaut -> de); section 6 the sfnt cmap reader (`src\ut_sfnt.h`) over synthetic formats 4 and 12, a TTC and an FNTD header, every truncation refused with a guard page behind the buffer and 4000 corrupted files, and with Fonts.arc the real FNTD copies (zh 34,600 / ja 16,732 / ko 17,666 code points - fontTools' counts -, th = vi = zh, `misc.fnt`, the root FNTX refused) and the derivation with them (Han -> zh, kana -> ja, Hangul -> ko, Thai -> zh with Windows drawing the rest); section 7 the probe plan (an eager fill on a dynamic font would build 64 sizes; the plan builds ONE size and its 95 ASCII glyphs at the world's start, the caption fit in a 34x20 cell then only the sizes it asks - 2 sizes, 190 glyphs -, a second measure nothing; a lazy cache over a baked font measures every caption at every size as the eager one); the purity of `ut_sfnt.h` - 133 checks in all |
| `tools\build_test_logview.bat` | the per-view log over the real logger (only the view's lines in its file, every line in the main log, WARN on both disks at once, rotation, re-entry appends, switch, the cap) and the custom-game probe over the real mods folder, extra folders and scratch games, checked against the catalogue's real rule (`UNIQUETAB_TEST_DBMODS_DIR`, `UNIQUETAB_TEST_GAME_DIR`, optional `UNIQUETAB_TEST_EXTRA_MODS`; the Crucible gets no view); the dead-tab replay (`src\ut_hudlife.h`): basemods -> ROT on group 28 -> basemods -> DGD -> basemods with and without the drop at the HUD's death, every world-start verdict and its WARN text, and a source check of the wiring; the probe at every blueprint classification, the five sibling `*Formula` classes refused at any, the blueprints each game's layer wins counted in the cross-check line; the probe with collect_mi (a faction-store rare, a ROT `owlheart/faction` shield, a "Crafted" pistol and a Rare staff / hunting spear admitted only with the flag; the probe misses 0 MIs with and without it over ROT / DGD / Grimarillion / GrimArmory and the scratch games) |
| `tools\build_test_catalogue.bat` | the whole generator end to end against a 1.3.0.8 game folder: all four outputs byte-compared with `data\oracle\`, the overlay record for record against `data\oracle\uniq-pages.arz`, the derived geometry, the painted plates on all three views (`--plates`: byte-identical to `data\oracle\plates\` on vanilla, a flat ground with nothing but our own frame lines on the two modded ones), a fresh install into empty folders, a custom game's view, and its own heap against the budget; the layout plates (`--layout-plates`: a user grid's set painted - its grid resolved by `utLayoutPlatesGrid`, the thread's own function -, found again, superseded, removed with nothing else, the derived plates / overlay / stamp untouched, the generation lock; the plates-only time and heap on vanilla, Baul and X-Mod; and `make_plates.py --grid` byte-identical on vanilla and X-Mod - the X-Mod grid `591,76,384,584` flush with the plate's right edge, its 12th column's frame drawn at x 973..974; the hash carries the lattice rule, `604a59c8`); and, with `UNIQUETAB_TEST_DBMODS_DIR` naming a folder that holds ReignOfTerror / DeathGrimDawn (optional, read only), each database mod's view against its fixtures (ROT Bows 76, Hunting Spears 19; with the flag off vanilla 25 groups / 109 pages, ROT 29 / 73 pages / 7,291 items with its Quest blueprints, DGD 29 / 180; Baul 132 pages) plus the off-page path (23 slot groups + Blueprints and no Retired, the owned relics on no page, the owned blueprint on Blueprints), the groups file retired (a stock copy renamed `.old`) / added (a user block taking its records off Rings, `+user:` in the stamp) / refused, and the 200 MB budget on the generation's OWN heap (the process peak printed as information); the frames written (vanilla 234, Baul 180, X-Mod and ROT 512; stamp 11) and the layout plan fitted under the F line. `data\oracle\` is the flag-on cut (the shipped default), `nomi\` the flag-off one (`kVanillaPlates` = 6); pass 5 `--mi`: the vanilla counts (2,915 = 2,393 + 466 faction + 50 crafted + the 6 quest rewards; 7,126 boxes on 197 pages, 0 skipped - the Lokarr set paged) and order, the KIND mark, the not-shown case with a synthetic journal (no MI in the flag-off catalogue, the pages `nomi\uniq-groups.txt` byte for byte), the stamp (flag off = `nomi\` byte for byte); `--mi-dbmod`: flag on (the budget, the counts, the flag-on cut where present) then flag off (one regeneration, the four files = `nomi\views\<tag>`; Grimarillion budget only). Every pass starts from an EMPTY scratch folder (`:fresh` removes and recreates `gen-plates`, `gen-layout`, `gen-uniq`, `gen-view`, `gen-dbmod-*`, `gen-mi`, `gen-mi-*`, `gen-mod`; the files passes 1 and 2 compare are deleted first; the view pass also deletes its tagged plates itself) - so a plate an earlier run left behind is never counted; `--layout-plates` 7b / 8b, `zoom_pct`: the derived grid at 100 has no set, at 150 and 200 a set of its own (`grid_438x627=105,76,333,552 zoom=150`, the six cells magnified - Helms 96x96 3 x 5 -, the zoom in the hash), a 64 px wide user grid at 200 clamped to five plates for six sizes, painted, found again, superseded by 200, removed back at 100 with the derived plates still the oracle's; the 150 set byte-identical to `make_plates.py --grid 105,76,333,552 --zoom 150` |
| `tools\build_test_padinput.bat` | the screen pad's press rules (`src\ut_padinput.h`): the record edit, the rectangles and their freshness, INERT with nothing published (1200 events), the button / ground / label / item-on-cursor presses, a plain right click consumed with no action and the wheel passing, outside untouched, the polled release, a lost UP, idempotence per Update generation, the editor hand-over and its chord from a `[keys]` binding (the machine's `editorChord` = `utKeyClickMatches` over 8 bindings x every event type x Ctrl/Shift/Alt, every RIGHT DOWN on the pad consumed; `RightClick`, `Alt+RightClick`, a key), purity; section 11 every logical button kind (`utPadButtonKind`: MAT, the buttons, OWN, NEXT only while paged, EDIT only while on - a screen click on its rect is the button action with its index), the adopted drag, a LEFT DOWN ending a right press whose UP was lost; section 12 logged worlds replayed through the machine AND the editor (the pad dragged out of the window, held still, released with its UP lost - no held event reaches the world view - then SAVE and CANCEL on the screen; the Ctrl+right open with its RIGHT UP lost followed by CANCEL); section 13 the window rule unit by unit; section 14 logged worlds replayed through both routes (a part brought back into the window moved, resized and saved, out and in again, the toolbar inside and outside; without the window rule the window route refuses every click but CANCEL) - 214 checks with the DirectInput.dll part; with `UNIQUETAB_TEST_GAME_DIR` / `UNIQUETAB_TEST_DBMODS_DIR` naming a game with `x64\DirectInput.dll`, the export by name, its stride, its prologues and the device vtable's slots offline |
| `tools\build_test_keys.bat` | the `[keys]` model (`src\ut_keys.h`, pure): the numbers it spells out against `windows.h`; the grammar - every key name and alias, any case, blanks around `+`, `none`, `""` = the default, every modifier set in either order; the canonical text (`utKeyText`: Ctrl, Shift, Alt, the key's own name) round-tripping every legal binding of every action, the defaults' text = the rows' default text; every refusal with its reason (an unknown word, a modifier twice, two keys, a key first, a stray `+`, modifiers alone, `none` with more, the wrong kind per action, `LeftClick` anywhere, `MiddleClick` for `editor_open`, a text key bare or with Shift only, `Esc` with anything, Alt with F4 / Tab / Space); the defaults (`UtKeys{}` = the parse of the seven texts = `utKeysDerive` of nothing); the conflict pass (the later of two identical chords of one site is none, both named; a refused text keeps the default); the matcher (`utKeyFind`: the most modifiers wins, a tie the earlier action, remaps, `utKeyClickMatches`, the SYSKEY gate `utKeysWantSys`); THE EQUIVALENCE - with the defaults, every message (`WM_KEYDOWN`, `WM_SYSKEYDOWN`, `WM_KEYUP`, `WM_SYSKEYUP`, `WM_MOUSEWHEEL`) x VK 0..255 x Ctrl/Shift/Alt decides exactly what `reagentHandleMessage` decided before `[keys]` (the live branch and the fallback, the wheel's ticks), and over 20,000 pseudo-random DOWNs and UPs the per-VK eaten set (`UtKeyEaten`) swallows exactly the KEYUPs the two-bit rule did; the eaten set per VK (a modifier released in between, a let-through DOWN clearing it, the middle button); the message decode (`utKeyDecode`: the button messages and the XBUTTON word, the wheel's sign, a sub-notch delta 0); the header's purity - 75 checks |
| `build_model.bat` (repo root) | `tests\model_test.cpp` - the pure model over the flag-on vanilla `catalogue.bin` (7,316 items, 6,121 equipment, 6,212 default visible, 202 sets), 47,177 checks; the KIND section loads `nomi\catalogue.bin` beside it (0 MIs, on = off + 2,915; Chosen Cowl f011a and Ranger's Helm b008 are MIs, Incendiary Casque b002f packed Epic); its step 1 repacks only a NEWER `catalogue.json` (`pack_catalogue.py --if-newer`) |

**Prerequisites.** MSVC 2022 x64 (the Build Tools are enough); Python 3 with `lz4`, `numpy` and
`Pillow` (the catalogue suite runs `make_plates.py` and `dump_gen_sample.py`, the groups suite
`build_uniq_db.py`); `capstone` and `pefile` only for `tools\census_textclass.py`.

**Running them.** Each suite is one script that finds the x64 MSVC toolchain itself
(`tools\find_vcvars.bat`), builds its test into `build\test\` (`build_model.bat` into
`build\model\`) and prints its verdict; it exits non-zero on a failure. From the tree's root, at
a `cmd` prompt (in a `.bat` file write `%%S`):

```
for %S in (config journal store rowfold tooltip bindings layout layoutedit groups buttons text logview catalogue padinput keys) do @call tools\build_test_%S.bat || echo SUITE FAILED: %S
build_model.bat
```

`build\` is the build output folder and is ignored by git; a runner script kept there belongs
to one checkout, so any folder it `cd`s to has to be edited when the checkout moves.

**Line ends: every `.bat` is CRLF.** `.gitattributes` checks them out so (`*.bat text eol=crlf`;
the blobs stay LF). cmd.exe misreads a batch file with LF-only line ends: after a `call :label`
returns it can resume at the wrong offset, so a later parenthesised block runs as loose lines and
the rest of the file runs a second time after its own last line. `build_test_catalogue.bat`
showed it: its database-mod `for %%M` blocks ran as `dbmod custom--m` (a vanilla catalogue
generated from the literal `...\mods\%M`) and `dbmod %M: SKIPPED`, and the real Reign of Terror
and DeathGrimDawn comparisons ran only in the accidental second pass after the first `ALL PASS`
(the parentheses of a `Program Files (x86)` path are harmless: every use of it is quoted). A
healthy catalogue log prints `ALL PASS` once and names `dbmod custom-reignofterror` and `dbmod
custom-deathgrimdawn` (and their `mi-dbmod` passes) once each, with their byte-identical lines
against `data\oracle\views\<tag>` and `nomi\views\<tag>`. A runner script you write yourself
under `build\` needs CRLF too.

**The inputs** are environment variables, each naming something that is opened READ ONLY and
never copied. A suite whose input is missing skips that half and says SKIPPED; only `catalogue`
refuses to run at all without the game folder.

- `UNIQUETAB_TEST_GAME_DIR` - **a Grim Dawn 1.3.0.8 game folder** (the folder holding `x64\`).
  Read by `catalogue` (the archives), `bindings` (Game.dll and Engine.dll, the decoders),
  `logview`, `text` and `padinput`. The oracle is a 1.3.0.8 artefact (section 7,
  `data\oracle\ORACLE.txt`), so once the installed game is patched past 1.3.0.8 this is a kept
  copy of a 1.3.0.8 install with the parts section 7 lists, not the installed game.
- `UNIQUETAB_TEST_EXE_IMAGE` - a **decrypted** 1.3.0.8 `Grim Dawn.exe` image. The exe's `.text`
  is Steam-DRM encrypted on disk and decrypted only in memory, so the image is read out of a
  running game (at the main menu is enough): open the process with `PROCESS_VM_READ` and
  `PROCESS_QUERY_INFORMATION`, take the exe module's base and size from a Toolhelp32 module
  snapshot, and write its `SizeOfImage` bytes as they are mapped - the headers and every section
  at its RVA, so a byte's file offset is its RVA. The tree carries no dumper; any tool that
  writes the mapped image does. Read by `bindings` (the exe signatures, their exact hit counts
  and their 1.3.0.8 RVAs) and `layout` section 11d (the screen draw site's proof).
- `UNIQUETAB_TEST_MODS_DIR` - the unpacked stash-UI mods the modded views are generated from:
  `xmod\X-Mod` and `lootascension\LootAscension` under it. Read by `catalogue`; without it those
  views are skipped.
- `UNIQUETAB_TEST_DBMODS_DIR` - a game's `mods\` folder holding the installed custom games
  `ReignOfTerror` and `DeathGrimDawn` (it may hold others; GrimArmory and grimarillion are probed
  by `logview`). Read by `catalogue` (each database mod's view against its fixtures), `logview`,
  `text` (the Fonts.arc of the game it belongs to) and `padinput` (`x64\DirectInput.dll` beside
  it, `..\x64`, when the game folder has none).
- `UNIQUETAB_TEST_FONTS_ARC` - a game's `resources\Fonts.arc` (a 1.3.0.8 reference folder need
  not carry it). Read by `text`, which falls back to the game folder's, then to the dbmods
  folder's game; without any it runs the pure half only.
- `UNIQUETAB_TEST_EXTRA_MODS` - more custom-game folders for `logview`'s probe, separated by
  `;` (a test arena's build folder, say), each probed and checked against the rule like the
  `mods\` ones.

`build_test_journal.bat` reads three optional fixtures of its own, named in its header:
`UNIQUETAB_TEST_BINJOURNAL` (a legacy binary `uniq-items.bin`, the migration case),
`UNIQUETAB_TEST_JSONLJOURNAL` (a real `uniq-items.jsonl`, the upgrade case; a synthetic one
stands in without it) and `UNIQUETAB_TEST_GDS` (a `.gds` exported by GD Stash, the
byte-for-byte compare). `build_test_store.bat` reads `UNIQUETAB_TEST_JSONLJOURNAL` too, for its
optional rehearsal pass over a copy of that file (skipped, loudly, without it); its fixed
rehearsal needs no variable - it reads the tree's own `data\oracle\journals\all-items-mi.jsonl`.

Without the fixtures - the public tree carries none of the game-derived files of
`data\oracle\` - `catalogue` and
`build_model.bat` cannot compare anything, `groups` fails its oracle G-line check and `layout`
skips its grid half; `data\oracle\ORACLE.txt` gives the order to rebuild them from the game
folder first.

And one the mod itself honours: `%UNIQUETAB_OUT%` overrides the mod folder. It is not a fallback -
it is how each harness gets its own empty directory, and it is read before anything else so a test
can never reach a real installation.

No test launches the game. The fourteen `tools\build_test_*.bat` write only under
`build\test\` (and Python's git-ignored `__pycache__`); `build_model.bat` writes `build\model\`,
its transcript `tests\model_test.out.txt` (git-ignored) and, from a `catalogue.json` NEWER than
it, `data\oracle\catalogue.bin`.

Also useful: `tools\journal_guard.ps1`, which checks both journals of the start-up view and of
every `mods\<name>\` set - and of every old `views\custom-<name>\` one, forever (a refused move or
a downgrade leaves a collection there) -, and is what `undeploy.bat` consults before it removes
anything; `build_test_journal.bat` drives it over a custom game's journal (refuses), over none
(allows) and over an old-layout `views\custom-legacy\` journal (refuses).

## 10. The menu smoke procedure

The offline suites cannot tell you the mod loads. This does, without a world and without risking a
save. **Do it by hand, not with `deploy.bat`**, so the archive-and-rollback story stays yours.

1. Confirm the game is not running.
2. Back up whatever is already in the game's `x64\` folder, by hash, and note where the backup is.
3. Copy in the loader's `dinput8.dll` and `bin\uniquetab.asi`. Leave the `uniquetab\` data folder
   where it is. Only one loader in the folder - two would load the plugin twice. Set
   `log_level=debug` under `[general]` in `x64\uniquetab\uniquetab.ini` (the overlay lines of
   step 6 are DEBUG lines; a first start writes the file, so on a fresh folder run steps 4-5
   once first), and set it back to `info` after step 6.
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
   - the settings file is the current shape (`ini_version=44`, four keys under `[general]`, the
     commented `zoom_pct` line under `[layout]`, the commented `[keys]` section, `[advanced]` at
     the end) with `uniquetab.state` beside it; no `uniquetab-buttons.ini` is written at the menu (it waits
     for the first caravan visit of a game);
   - the late report includes `dinput.getMouseEvent` (resolved on the first game-thread tick
     that has the input device - at the menu or in the first world); no screen-site or
     DirectInput hook line at the menu (they install only in a world, and only while
     `pad_screen` / `label_screen` is set);
   - no `[E]` and no `[W]` (a `[W] screen pad input: not installed` names a refused input row).
7. Repeat with the other loader variant (delete the first one first).
8. Restore the folder from step 2 and hash-verify.

## 11. Known limitations and open items

Each item is true of the code as it stands: what it is, what it costs, and where known, what a
fix would take.

**Proven offline only.** The suites run the pure rules and the generator; no suite covers these
run-time halves. Check them in a game when a change touches them.

- **The monster infrequents in a world.** The vanilla MI pages, the deposit of an affixed MI and
  its take after a restart (the live read of a filled prefix / suffix / seed at
  `Item + 0x538 + 0x28 / 0x48 / 0x68`, and `Item::CreateItem` with re-pointed affix strings - the
  risk class `ut_rescue.h` warns about), the second-roll refusal with a real name and level, the
  third rollover line and `tooltip.roll`, `collect_mi=0` -> 1 hiding and showing the stored MIs, a
  flip's regeneration at start-up, the rescue's roll line, a double-rare MI, hardcore, a mod's MI.
- **The blueprints in a world.** The Blueprints page (914 scrolls on 12 pages with
  `collect_mi=0`), a deposit / take / learn round trip, the sixth tooltip detour around the real
  0x31A4CF call (the status line's `innerSuppressed=N` grows while blueprints are hovered), the
  line on a page box, in the bags and on a chat link, the `[BLU]` offer on a real buttons file,
  Reign of Terror's `rot_rw_*_craft` Quest blueprints, the search marking BLU.
- **The run-time fonts.** An FNTD copy loaded by its exact path on an English game drawing through
  `RenderText2d` as on a Chinese one; the per-size dynamic styles across a world change; the
  Windows fallback's look and advance in the engine's atlas; whether `GetMetric` on a dynamic font
  is safe outside a frame. The live probe's game-thread half (`fontCacheFillSeh`,
  `fontCacheSizeSeh`, the thread rule in `liveExtraGlyph`, `dynamicRolesSay`, `textFontsInArc`)
  is compiled by no suite, because none compiles `ut_panel.cpp`; the pure halves it runs are
  tested. `LoadFont` of a copy the game does not otherwise load (`fonts/ru/` on an English game,
  `fonts/zh|ja|ko/`) and a button's own `font =` (up to 8 per world) are in the same state. Under
  Wine / Proton GDI may not fall back, so letters outside WenQuanYi may draw blank.
- **The layout editor's run-time half.** The draw, the parking, the consumption, the polled drag,
  the cancel by the 400 ms frame gap, the save handover, the screen mode's draw and grounds, the
  adopted drag, the window route's proof by the screen input's note, and the in-window MAT click.
  Assumed, not measured: the Draw tail and the mouse slot run on one thread; the RIGHT UP after a
  consumed Ctrl+RIGHT DOWN is harmless to the engine; the engine's event types follow
  SM_SWAPBUTTON (the polled button follows whichever is down); the world view acts on a held byte
  under an editor drag; `panelLayoutEditDragging` is read on the thread that writes it.
- **The screen pad and label.** The live HUD read (`caravan - 0x4FD08`), MinHook on the item
  rollover's Draw with its four arguments passed through (the xmm3 float), the frame gate, the
  drawn look; the DirectInput export's live address, the vtable cross-check against the live
  device, the event-space probe and every press rule end to end; which HUD draw is the minimap
  (anything drawn after the HUD covers the pad); whether the window origin is stable frame to
  frame (an unstable one would relayout every frame while a screen key is set).
- **The materials page's pad over boxes.** The kept LEFT and RIGHT DOWN, the hover answer put
  back and a box beside the pad taking an item (`test_layoutedit` and the exe's three type
  compares). A LEFT UP inside the pad is passed (the box path never tests type 9), not swallowed.
- **Frames of more than 160 boxes.** The HUD build with 234 / 512 boxes (time and memory per
  filler; the capture's DEBUG line measures the time), a relayout and a click past box 256, Reign
  of Terror's 29 x 11 page.
- **`max_per_record` and `rescue`.** Both are hidden `[advanced]` rows whose descriptions end in
  "(untested)": the suites cover the parse, the migration and rescue's write-back, nothing more.
- **The release zip itself.** The suites prove a first launch into empty folders; a clean
  install from the packaged zip, in a game, is a person's check for every release.

**Limitations.**

- **One identity per record.** At `max_per_record` > 1, two copies of a unique with different
  seeds are count 2 with the newer seed, and both takes return that one (MIs are capped at 1,
  uniques are not); the fix is a per-copy identity list (journal format 5). "(another roll)"
  compares prefix, suffix and seed only, so an MI with the same roll but another component or
  augment reads "In your collection". An unknown kind fails closed: with no catalogue loaded, a
  second copy of ANY record is refused at `max_per_record` 0 or above 1.
- **The take watch sees 256 prototypes.** `kTableWatchCap = 256` in `ut_reagent.cpp`
  (`tableTakeWatch`) sizes the arrays `storeCollectBuilt` fills from the per-world prototype map
  (`g_protoTable`, `ut_store.cpp`, one prototype per STORED record a relayout painted this world;
  never a filler). The map itself is capped by `protoCapNow` (max(320, 2 x paintable + 64),
  ceilinged by the view's catalogue, section 4 "Every bound is sized from the data") - the
  one record-sized bound kept fixed on purpose -, so with more than 256 distinct stored records
  painted in one world, those past the
  256th in the map's iteration order are not watched. A take from such a box is still recorded by
  `identityAfterCreate` when the identity path ran (the default, `max_per_record=1`); not recorded
  are a whole-stack take of more than one copy and a take whose identity restore missed - the item
  reaches the bag while the row still claims it, a duplicate. The fix: size grow-only vectors
  from `storeCollectBuilt`'s counting mode (`storeCollectBuilt(nullptr, nullptr, nullptr, 0)`),
  bounded by `protoCapNow`, with the per-tick cost measured; a test needs the watch's compare loop
  pulled out of `ut_reagent.cpp` into a pure helper `test_store` can drive past 256 rows.
- **A relayout fault on a live HUD stops the tab for the session.** An engine call that faults
  is never retried; the world-start verdict announces the stop. The verdict assumes
  the HUD is built before the first Update with a main player; otherwise it could miss a
  per-world stop, never invent one.
- **The frame's box count.** A materials page with V >= N real boxes would have no filler, and
  the UI-scale probe reads the last filler; only V > N is refused. None is known (the largest is
  Reign of Terror's 116 under 512); `max(N, V + 1)` would cover it. The 512 ceiling is one
  constant (`kUtFrameBoxMax`); X-Mod's cover would fit 840 at 32 px, so its 32 px groups paginate.
- **The fillers name a real reagent.** The frame's boxes beyond the materials page's own are
  parked at -4000,-4000 and name the first vanilla box's reagent, because whether the engine's box
  `Load` accepts an empty `reagentName` is not established. If it does, the overlay can shrink.
- **The plates live in the game folder.** `GraphicsEngine::LoadTexture` resolves a name through
  the engine's resource roots and hands back a 64x64 placeholder for an absolute path, so the
  plates are written under `<game>\settings\ui\caravan\`. Mounting the mod folder as a resource
  source (`FileSystem::AddSource` / `AddSourceArchive` are exported) before the first plate load
  would keep everything under `uniquetab\`; untried.
- **The layout-plates window.** After a relayout that moves the boxes, the derived plate stays
  under them until the plates thread's next poll (at most 250 ms), then the plain cover until the
  set is painted (1 s of quiet, then 0.25-0.9 s). A move from one user grid to another can show
  the previous `_L` set for up to that 250 ms, because the published state carries the hash, not
  the grid; a grid check in `layoutPlatesState` (`ut_generate.cpp`) would close it. A cell flush
  with the plate's edge has its outer frame line on its own last 2 px.
- **A several-group button.** A union whose cell no group of the view has (`groups = scepter,
  head` on a view without 64x96) shows the plain cover - plates for it would need the generator to
  read the buttons file; a union of mixed cell sizes puts the smaller items top-left in the larger
  cell (the engine's page is one lattice); in a session's FIRST world the button map exists only
  after the page's first draw, so the first relayout may show the first group alone for a frame
  or two.
- **The buttons file follows the game thread.** It is written at the first caravan visit of a
  game, not at the database load (only the game thread can snapshot the group table), and after a
  view change the previous game's file stays in force until the worker reads the new one (at most
  a second; the built-in buttons show meanwhile). The main file serves vanilla AND basemods, so a
  basemods-only group lands in `[unassigned]` and the section is rewritten at each switch between
  the two (byte-preserving).
- **The late built-in groups.** Only the listed ones are offered to an existing buttons file: a
  future built-in group has to be appended to `utbLateBuiltinIds` (`ut_buttons.h`) and
  `build_uniq_db.py`'s `BUTTONS_LATE`, or it lands under `[unassigned]` in every existing file.
  The "added once" memory is a comment line in `[unassigned]`, so a player who deletes the whole
  section and the `[BLU]` button gets `[BLU]` offered once more; a memory outside the file (the
  view's `uniquetab.state`) would end that.
- **`uniquetab.state` keeps the group by index.** A new built-in group shifts every group after
  it, so a saved state reopens one group earlier, once (it only decides which page opens first).
  Keying the state by group id would end it.
- **A player's groups come first.** A player's new groups lead the group order, so the generated
  buttons file puts their buttons first. An installed groups file that is an EDITED copy of an
  older template is read as the player's, and its blocks win over the built-in class groups.
- **`+N not on these pages` is the label's last part,** so at small UI scales on the widest
  groups it is the first thing clipped; it appears after a world's first caravan open (the
  reconcile counts it).
- **No shipped view draws the NEXT button.** None pages its buttons at any of the seven UI
  scales; only its arithmetic is tested (a synthetic 120-group table).
- **The EDIT button is proven offline only.** The slot maps, the kinds, the captions, the key and
  the measured pads are tested; its look in game, the click through both routes, the materials
  page's switch-and-open and DeathGrimDawn's one-size-smaller captions with its real tags are not.
- **The zoom is proven offline only.** The cells, the per-group clamp, the plates (byte-identical
  to `make_plates.py --zoom`) and the settings row are tested; the box widget's size write and its
  re-assert (`boxZoomApply` / `zoomReassert`), how soft the magnified icons look, the hit test,
  tooltip, compare popup, pick-up and deposit on enlarged boxes, the badges and the count text, the
  caravan's search highlight, the materials page back at its native size and a UI-scale change
  while zoomed are not. The box's flag bytes `+0x14/+0x15` (SetTexture skips the scale for a box
  that has them) are assumed 0 for the mod's boxes. A rule kept on purpose: the layout editor's
  smallest grid is one zoomed largest cell, so a hand-written grid smaller than that is clamped on
  the page (`utZoomFit`) but refused by the editor's SAVE until it is enlarged.
- **The keys are proven offline only.** `test_keys` proves the decode / find / eaten wiring equal
  to the old decision with the defaults and `test_padinput` the editor chord;
  `reagentHandleMessage`, `editMouse`, `scrBody`, the editor key's latch and the recheck have run
  only in the compiler. Unproven: that `WM_MBUTTONDOWN` / `WM_XBUTTONDOWN` reach
  `WinWindow::WindowProc` at all (a MiddleClick / Mouse4 / Mouse5 step), that swallowing
  `WM_SYSKEYDOWN` for an Alt chord disturbs nothing of the game's own Alt handling, and
  `GetKeyState(VK_MENU)` at the message's time. The rules, decided and kept: a remap can hand a
  formerly chorded key to a plainer action (with `button_next=Ctrl+F3`, Ctrl+PageDown matches
  `rows_down`, whose modifiers - none - are all held, and pages rows; README says so); when two
  different chords both match the keys held, the most modifiers win and a tie goes to the earlier
  action; `editor_open` as a click matches when every modifier it names is held, extra ones
  allowed (Ctrl+Shift+RightClick opens with the default), as `ut_keys.h` and `test_padinput`
  state; a KEYUP is decided before the gates - swallowed exactly when the last DOWN of that VK was
  consumed - and the fallback branch's KEYUP goes through the same eaten set; `editor_open` bound
  to a key on the game's own materials page first goes back to the last collection group, as EDIT
  does. The `[layout]` blurb and `edit_button`'s description name the EDIT button and the
  `editor_open` chord ("Ctrl+right-click unless you change it"), never the click alone.
- **Fonts and alphabets.** One glyph per UTF-16 unit: right-to-left order, shaping and letters
  above U+FFFF are not drawn. Letters that are only Han draw in `zh` (Chinese glyph shapes); a
  Japanese player writes `font.caption = fonts/ja/savapromedium.fnt`. A font whose Fonts.arc entry
  cannot be read is probed lazily as if dynamic (harmless for a baked font; its INFO line says "a
  font of unknown kind"); a binding row for the engine's `font+0xF0` flag would decide it. The
  FNTD read costs about 15 MB once per process (three ~5 MB bodies, only their code points kept).
  With an explicit `font.caption = fonts/zh/savapromedium.fnt` and no role left to derive, the
  per-world "drawn by Windows' own fallback font" count line is not written. A custom game's own
  fonts are not in the base Fonts.arc and are refused by the index check. The buttons file has no
  `name = @game` value for the game's own word for a group (the fold names such a line as not
  carried).
- **Custom games.** A game that only DEMOTES a base unique and adds nothing gets no view, so the
  start-up catalogue still lists that unique there. A custom game entered between a failed
  `views\` -> `mods\` folder move (a file held open, e.g. by GD Stash: error 5) and the next start
  gets a new, empty `mods\<leaf>\` from `journalFollow` / `viewEnterCustom`; from the next start
  on the old folder is "taken" - one WARN every start, nothing merged, `journal_guard.ps1` still
  protecting its items. The fix by hand is to move its files into `mods\<leaf>\` with the game
  closed; a per-game retry before those two create the folder is not built. DeathGrimDawn's
  staff-class word is "Bow" and its hunting-spear word "Artifact". DeathGrimDawn's crossbow-folder
  rule at the head of `[view:collect_mi]` moves no record (the groups come out the same without
  it): dead data, harmless; dropping it changes the template's FNV, so every view regenerates
  once.
- **Blueprints.** The learned-recipes page is not built (it needs the engine's learned set,
  `GameEngine+0x36D60`, read, and a read-only kind of group). Reign of Terror's 170 Quest
  blueprints sort with the Magical ones (rank 9); a rank of their own is one table entry.
  Grimarillion's Blueprints cell is 64 x 64 (about 28 pages): 214 of its blueprints name their own
  arcane-formula icons (`xpack/items/artifacts/arcaneformulae/x_recipe01..03.tex`, inferred
  64 x 64), and an icon a blueprint names is kept; mapping a non-scroll formula icon to the
  rarity's scroll would give 64 x 32 cells, about 21 pages.
- **Grimarillion's headroom.** Its generation takes 188.1 MB on the reference folder (189.8 MB on a full
  1.3.1.1 installation) of the catalogue suite's 200 MB
  own-heap budget with every Rare (the game enforces no limit, so what grows is the memory a
  Steam Deck has to spare); an update adding about 10% more records would reach it. Excluding
  its Rare faction / crafted gear (data, `ut_groups_default.h`) would make room. Grimarillion and Dawn of Masteries
  are generated by the C++ harness only (no Python fixture); Dawn of Masteries shows no tab (it
  has no materials page).
- **The oracle is 1.3.0.8's.** Game version 1.3.1.1 changes all four databases and most of the
  archives the catalogue reads (the base `Items.arc` and `Text_EN.arc`, `gdx1\resources\Items.arc`,
  `gdx2\resources\Text_EN.arc` and all three of `gdx3\resources\`), so the catalogue suite's
  byte gates need a kept 1.3.0.8 copy; the other way is to re-cut the whole of `data\oracle\` on
  the new game and read what moved. On the flag-off vanilla cut (no `--collect-mi`, no
  `--custom`) `build_catalogue.py` asserts the 1.3.0.8 relic and collection counts
  (`RELIC_EXPECT`, `COLLECTION_EXPECT_TOTAL`) and refuses a database that differs or is
  incomplete - those assertions move with a re-cut. The flag-on and custom cuts assert no
  counts, so only `GD_DIR` keeps them on the 1.3.0.8 folder.
- **The fixtures are game-derived.** `data\oracle\` holds item names, record paths and caravan art
  extracted from the game, so the public tree carries none of it (nor `tools\exports\`'s dumps);
  the `README.txt` left in each place says how to recreate them, `data\oracle\ORACLE.txt` gives
  the order, and `data\oracle\templates\` - the mod's own text - is published as it is.
  `data\oracle\catalogue.json` (4 MB) is never committed: `build_model.bat` skips the repack
  without it, and `build_uniq_db.py` needs it for a full rebuild. `tools\build_catalogue.py` also
  writes `catalogue-stats.md` there; keep it in step with the generator or stop emitting it. The
  fixtures could become committed sizes and hashes, with the harness comparing against a freshly
  rebuilt set.

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
- **Do not act on a WM_KEYUP as on its WM_KEYDOWN** in `reagentHandleMessage`; swallow it only
  when its KEYDOWN was consumed - per VK (`UtKeyEaten`), whatever the binding (a remapped key, a
  WM_SYSKEYUP of an Alt chord, a mouse button's UP), and a modifier released in between does not
  matter. Do not parse `[keys]` there either: read `g_cfg.keys`.
- **Do not gate a window-route rule on `g_btnSpaceProven` alone.** That probe runs only inside the
  in-window pad's band, so it never runs while the pad is pinned to the screen.
- **Do not launch the game from an automated run**, and do not deploy from one. Copy files by
  hand, or use `deploy.bat` knowingly. Nothing automated should write inside a game, Steam,
  Documents or GD Stash folder.
