# HANDOFF - for whoever maintains this next

Written for a maintainer, human or machine, who has never seen this code. It describes what is
here, why it is shaped this way, and the handful of rules that must survive any change. Read
sections 1 and 2 before you touch anything.

The mod is C++17, MSVC, built by `build.bat` into `bin\uniquetab.asi` - a plain DLL with the
extension Ultimate ASI Loader looks for. It exports nothing. `README.md` is the player's
document; this one is the code's.

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
   `uniq-records.txt`, `uniq-pages.txt`, `uniq-groups.txt`, `rescue-report.txt`. Never a `.gst`,
   never `player.gdc`, never `options.txt`, never anything in the game folder proper.
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
6. **Nothing generated is shipped.** `catalogue.bin`, `catalogue.stamp` and the three `uniq-*.txt`
   lists are built on the player's machine. They belong in `data\oracle\` as test fixtures, never
   in the package.
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
generateEnsure()       catalogue.bin + the three lists, current with the game's archives
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
| `ut_generate.cpp/.h` | the DLL side of `src\gen`: keep the generated files current before anything reads them |
| `src\gen\` | the catalogue generator: `arz_reader`, `arc_reader`, `catalogue_gen`, `pages_gen`, `generate` |
| `src\model\` | `catalogue` (the binary format), `collection` (ownership), `layout` (rows and pages) - pure, no Windows |
| `ut_panel.cpp/.h` | the collection view: the catalogue, the ownership state fed by the sack detours, the group label, the 26-button pad |
| `ut_reagent.cpp/.h` | the big one. The database overlay, the `craftingMaterial` gate, the deposit and take detours, the bag proof, the identity capture, the census and the crash diagnostics |
| `ut_store.cpp/.h` | the private table: the mod's own `record -> {count, identity}`, and the display prototypes `showBox` points boxes at |
| `ut_live.cpp/.h` | the page itself: the group table, the box capture, the relayout, the owned-only filter, the wheel and button input |
| `ut_plate.cpp/.h` | the page *background*: the captured `ReagentWindow`, the plate swap per group, the visibility test, the label draw, the search marks |
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

## 4. The two mechanisms that make the tab exist

There is no way to add a third caravan tab, so the collection **shares the Crafting Materials
page**.

1. **Database.** `uniq-pages.arz` adds new records - `records/ui/caravan/uniq_pNN.dbr` and forty
   boxes each - and the mod hands one of *those* back when the engine asks
   `ObjectManager::LoadTableFile` / `GetLoadTable` for a reagent window. The vanilla
   `caravan_materialwindow.dbr` is **never overridden**; that is why the player keeps their real
   materials page. The archive is loaded by calling `Engine::LoadDatabase` from a post-detour on
   `Engine::LoadMainDatabase`, i.e. after the checksum is already computed (invariant 5).
2. **The gate.** What may enter a reagent box is one bool at `Item + 0xC64`, loaded from the
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
the player's own archives
        |
        |  tools\build_catalogue.py   -> data\oracle\catalogue.json (+ catalogue-stats.md)
        |  tools\build_uniq_db.py     -> data\uniq\uniq-pages.arz                     (SHIPPED)
        |                             -> data\oracle\uniq-records.txt, uniq-pages.txt,
        |                                uniq-groups.txt, uniq-pages.json             (FIXTURES)
        |  tools\pack_catalogue.py    -> data\oracle\catalogue.bin                     (FIXTURE)
        v
src\gen  (the same job, in C++, inside the DLL)
        -> <mod folder>\catalogue.bin, uniq-records.txt, uniq-pages.txt, uniq-groups.txt
```

The Python tools under `tools\` are the **reference implementation**. The DLL does the same work
itself at run time, and the contract is that **the four files it writes are byte-identical to the
Python ones**. `tools\test_catalogue.cpp` (via `build_test_catalogue.bat`) is the oracle: it runs
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

`uniq-pages.arz` stays a shipped file: a layout needing a box that does not exist is refused
(`checkPageCapacity`), not silently truncated.

## 8. The journal format

`uniq-items.jsonl` (and `-hc`), JSON Lines, UTF-8, LF. The full spec is the file header of
`src\ut_rescue.cpp`; this is the shape.

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
| `tools\build_test_catalogue.bat` | the whole generator end to end against a real installation, all four outputs byte-compared with `data\oracle\` |
| `build_model.bat` (repo root) | `tests\model_test.cpp` - the pure model |

Two environment variables:

- `UNIQUETAB_TEST_EXE_IMAGE` - a **decrypted** `Grim Dawn.exe` image (the shipped exe's `.text` is
  DRM-encrypted on disk, so the patterns cannot be found in it). Needed by `bindings`.
- `UNIQUETAB_TEST_GAME_DIR` - the installed game folder, opened **read only**. Needed by
  `bindings` (Game.dll) and `catalogue` (the archives).

And one the mod itself honours: `%UNIQUETAB_OUT%` overrides the mod folder. It is not a fallback -
it is how each harness gets its own empty directory, and it is read before anything else so a test
can never reach a real installation.

No test launches the game. None writes outside `build\test\`.

Also useful: `tools\journal_guard.ps1`, which checks both journals and is what `undeploy.bat`
consults before it removes anything.

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

Still open, and listed so they are not rediscovered:

- **Spare boxes.** `uniq-pages.arz` has exactly as many boxes as there are items today, so a game
  patch that adds a unique has nowhere to put it. Two ways out: regenerate the archive with spare
  boxes per slot group (`build_uniq_db.py --spare N`), or port `arzw.py` (131 lines plus a
  literal-only LZ4 block writer, about 20 more) into `src\gen` so the DLL writes the overlay
  itself and no spare capacity is ever needed. **Check first** whether the engine's box `Load`
  accepts an empty `reagentName`: every box today carries one, a spare has no item, and if the
  engine refuses it a spare needs a harmless placeholder record. Either way `checkPageCapacity`
  and all four oracle files must be re-verified byte for byte. This is its own work package.
- **The plates cannot be loaded out of the mod folder.** `GraphicsEngine::LoadTexture` resolves a
  name through the engine's resource roots; given an absolute path it hands back a 64x64
  placeholder, not null. That is why the plates ship under `settings\ui\caravan\` in the game
  root and why `ensurePlate` accepts a mod-folder texture only at the plate's real size. The
  clean way to keep everything under `uniquetab\` is to mount the mod folder as a resource source
  (`FileSystem::AddSource` / `AddSourceArchive` are exported) before the first plate load -
  untested.
- **`data\oracle\catalogue.json` is absent** from the tree (4 MB of derived JSON).
  `build_model.bat` skips the repack when it is missing, and `build_uniq_db.py` needs it for a
  full rebuild. Decide whether to ship it, or to document regenerating it as step one of any data
  change. The same goes for `data\oracle\uniq-pages.json`, `data\plates\src\` and
  `tools\exports\`: they are not published (game-derived inputs), and the README.txt in each
  place says how to recreate them from a copy of the game.
- **`tools\build_catalogue.py` still writes `catalogue-stats.md`** into `data\oracle\`. Keep it in
  step with the generator or stop emitting it.
- **Derived game content.** The plates are painted over vanilla caravan textures, and the test
  fixtures under `data\oracle\` hold item names and record paths extracted from the game.
  `THIRD_PARTY.md` says so plainly. Both can go: generate the plates at run time from the
  player's own archives (the textures are uncompressed 32-bit, the compositing is plain pixel
  work already written in `tools\make_plates.py`), and compare the generator's output against
  committed sizes and hashes instead of committed files.
- **Only the menu has been seen on the packaged 1.0.0 install.** The plugin, the loader, the
  first-launch generation, the campaign in both modes and the Crucible were all exercised in a
  world on a clean install during development of this version, but the QA sheet on the packaged
  build itself is a person's job, and so is every later version.

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
- **Do not ship a generated file.** `catalogue.bin`, `catalogue.stamp`, the three `uniq-*.txt`, the
  ini, the log, the journals. Ever.
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
