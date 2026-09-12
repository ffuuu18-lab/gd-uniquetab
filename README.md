# Unique Collection Tab

A collection tab for Grim Dawn, built out of the caravan's Crafting Materials page.

Drop an Epic, a Legendary or a relic into it and the game keeps a slot for that item the way it
keeps one for an Aether Crystal: one box per item record, filled or empty, sorted into category
pages. It is a place to put the uniques you want to keep, and a way to see at a glance what you
have found and what you have not.

**Your items do not go into the game's save.** The collection lives in the mod's own file next to
the mod. Nothing the mod does is written into `player.gdc` or into any `.gst` stash file, so a
future patch, a broken mod or a plain uninstall cannot make the game prune what you stored. If
you remove the mod, your collection file stays on disk exactly as it was.

## What you need

- Grim Dawn 1.3.0.8, 64-bit. The mod checks the game's shape, not its version number, and turns
  itself off if it does not recognise what it finds (see *When something goes wrong*).
- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader), the **x64** build.
  This mod is an ASI plugin: it does nothing until a loader loads it. The loader is not included
  here - download it once and it serves every ASI mod you use.
- Nothing else. No script extender, no launcher, no other mod.

## Install

Everything goes in the game's `x64` folder - the one that holds `Grim Dawn.exe`.

1. Take `dinput8.dll` from the x64 build of Ultimate ASI Loader and put it in `x64\`, beside
   `Grim Dawn.exe`. The loader's `winmm.dll` variant works too. Use exactly one of them: two
   loaders in one folder would load every plugin twice.
2. Copy `uniquetab.asi` into that same `x64\` folder.
3. Copy the `uniquetab` folder into that same `x64\` folder, so you have `x64\uniquetab\`.
4. Copy the `settings` folder into the **game's root folder** - the one that holds `x64\`, not
   into `x64\` itself. You should end up with `settings\ui\caravan\uniq_plate_*.tex`. These are
   the page backgrounds. The engine reads them by resource name, so this is the one place they
   can go; without them the pages fall back to the vanilla background.

Then start the game and sit at the main menu for a few seconds.

**The first launch builds the catalogue.** The mod reads your own installation - your archives,
your language, your DLCs - and writes the item catalogue into `x64\uniquetab\`. It takes a few
seconds, once. It happens again after a game update, and never otherwise. The settings file
`x64\uniquetab\uniquetab.ini` is written at the same time, with every option and a line of
explanation for each.

Everything the mod reads and writes lives in that `uniquetab` folder beside the `.asi`. If you
put the `.asi` in `x64\scripts\` instead, its folder becomes `x64\scripts\uniquetab\`. If the game
folder cannot be written to, the mod uses `Documents\My Games\Grim Dawn\uniquetab` instead and
says so in its log.

### Steam Deck, SteamOS and Proton

The loader is a DLL override like any other. Set the game's **launch options** to:

```
WINEDLLOVERRIDES="dinput8=n,b" %command%
```

(or `winmm=n,b` if you took the `winmm.dll` variant of the loader), so Wine loads the loader
sitting in the folder instead of its own built-in DLL.

The files themselves go in the same places as on Windows - inside the game's own `x64\` and
`settings\` folders, **not** in the Proton prefix. The prefix under
`steamapps/compatdata/219990/pfx` holds only the fake Windows drive and nothing of this mod.

## Using it

Open the caravan and go to the **Crafting Materials** page. Your usual materials are page one,
untouched; the collection pages follow.

- **Mouse wheel** scrolls the page.
- **Ctrl+PageUp / Ctrl+PageDown** move between category groups (Helms, Rings, Relics, and so on).
- The **buttons across the top of the page** select a group too.
- The **stash search box** works: groups holding a match are marked, so you can see where a name
  lives before you go there.
- One of those buttons is **OWN**. It toggles between showing every item in the group and showing
  only the ones you already have.

### Putting an item in

Two ways, both of them the ordinary ones:

- **Drag** the item onto its box.
- **Shift-click** it in your bag.

The box takes the item and the collection remembers what it was - affixes, components, augments,
everything. The log says `ACCEPTED` for each one.

### What is refused, and why

- **Shift-clicking from your stash** does not deposit. The mod only accepts a click whose removal
  it can prove: the cursor drag removes the item from the cursor itself, and a bag shift-click
  removes it from the character's bags. A stash click is not one of those, and accepting it could
  leave you holding the item *and* a row in the collection.
- **The "deposit all" buttons** are refused on purpose. They would vacuum every unique out of your
  bags in one click, which is exactly what you do not want a collection tab to do.
- **An equipped item** is refused. Unequip it into a bag first, or drag it.
- **A refusal never moves anything.** The item stays exactly where it was, and you get the game's
  usual "cannot be placed here".

### Taking an item back

Click it, the way you take anything out of the Crafting Materials page. It comes back as the item
you put in, not as a fresh roll: the same affixes, the same components, the same augments. If it
was soulbound when it went in, it is soulbound when it comes out.

### Two separate collections

Hardcore and softcore each have their own collection, in their own files, side by side. A
hardcore character never sees softcore items and nothing crosses over.

### The Crucible and other custom games

They work. A custom game loads a database of its own, and the mod loads its pages into that one
as well. If a game mode ever loads a database the mod cannot join, that mode simply shows the
plain vanilla Crafting Materials page and says so in the log; nothing is lost and the campaign is
unaffected.

## Settings

`x64\uniquetab\uniquetab.ini` is written by the mod on the first launch and re-read about once a
second, so you can edit it while the game runs. A value outside its allowed range is clamped and
the log says so. Delete the file and it comes back with the defaults.

Each line in the file carries its own explanation. These are the ones worth knowing about.

**[general]** - the basics.

| key | what it does |
| --- | --- |
| `enabled` | 0 = the mod goes quiet: no tab, nothing new collected. Nothing is lost |
| `log_level` | how much goes in the log: error, warn, info, debug, trace |
| `ui_scale_pct` | 0 = follow the game's own UI scale, or force one (50..300 percent) |

**[collection]** - what the collection accepts, and how the tab starts up.

| key | what it does |
| --- | --- |
| `max_per_record` | how many copies of one item the collection keeps (0 = no limit) |
| `soulbound_collect` | 1 = a soulbound or untradeable unique can be dragged in as well |
| `mp_collect` | 1 = the collection also works while you play multiplayer |
| `collect_pristine_only` | 1 = refuse items with affixes, components, augments or rerolls |
| `collect_quick_pass` | 1 = shift-click sends a listed item to your normal stash tab |
| `owned_only` | the OWN filter at start-up: 1 = show only what you already have |

**[display]** - what the mod draws on the Crafting Materials page, and its controls.

| key | what it does |
| --- | --- |
| `group_buttons` | 1 = draw the category buttons over the page and let them be clicked |
| `page_hotkeys` | 1 = the wheel scrolls the tab, Ctrl+PageUp/PageDown change group |
| `search_buttons` | 1 = mark the category buttons that hold a match for your search |
| `search_buttons_unowned` | 1 = mark groups for items you do NOT own yet as well |
| `compare_popup` | 1 = hovering a stored item shows the game's own comparison box |
| `tooltip_mark` | 1 = every item tooltip says whether it is in your collection |
| `plate_label` | 1 = draw the group name, the counts and the row window over the page |

**[files]** - where your collection is written, and what is exported beside it.

| key | what it does |
| --- | --- |
| `journal_dir` | where the collection file is kept. Empty = the mod's own folder |
| `export_gds` | write uniq-export.gds for GD Stash: 0 off, 1 the collection, 2 all |
| `export_csv` | write uniq-export.csv for a spreadsheet: the same three settings |

**[one-shot]** - commands. Set one to 1 with the caravan window open; the mod sets it back to 0.

| key | what it does |
| --- | --- |
| `rescue` | 1 = hand every stored item back to the character, then set itself to 0 |
| `journal_prune` | 1 = drop the entries that are no longer stored, then set itself to 0 |

The rescue has a second trigger that needs no editing: put an empty file called `RESCUE-NOW` next
to the ini. The mod sees it, deletes it and runs the rescue.

**[advanced]** is tuning - cache sizes, timings, the pixel geometry of the buttons and labels, and
the two lines the tooltip adds. Leave it alone unless a line in the file tells you exactly what it
is for; every value is clamped to the range printed beside it.

## Multiplayer

The collection works in multiplayer, **as the host and as a client**, with `mp_collect=1` (the
default): deposits and takes behave as they do in single player, and every player keeps their own
collection file on their own machine. Set `mp_collect=0` if you would rather collect only in single
player.

The mod never changes the database checksum the game compares between players, so nobody is locked
out of a game because someone has it installed.

## Your collection, and backing it up

Everything lives in `x64\uniquetab\`:

| file | what it is |
| --- | --- |
| `uniq-items.jsonl` | **this is your collection** (softcore) |
| `uniq-items-hc.jsonl` | the same, hardcore |
| `uniq-export.gds`, `uniq-export-hc.gds` | a stash file GD Stash can import |
| `uniquetab.ini` | your settings |
| `uniquetab.log` (+ three older copies) | what happened last session |
| `catalogue.bin`, `catalogue.stamp`, `uniq-*.txt` | generated from your game; delete them and they come back |

**Back up `uniq-items*.jsonl`.** That file *is* the collection: every stored item, with its full
identity, one item per line. It is plain text - you can open it, search it, and delete a line to
forget an item. Copy it somewhere safe as often as you would back up a save.

`uniq-export*.gds` is written beside it for convenience: point **GD Stash** at it to browse your
collection outside the game. It is an export, not the original - the `.jsonl` is the one to keep.

## When something goes wrong

The first thing to do is read `x64\uniquetab\uniquetab.log`. It is plain text and it is written to
explain itself. If you report a problem anywhere, send that file.

- **The tab is not there at all.** The mod is not loaded. Check that the loader's `dinput8.dll`
  and `uniquetab.asi` are both in `x64\`, beside `Grim Dawn.exe`. If there is no log file at all,
  the loader never loaded the plugin.
- **The log says the mod is OFF.** That is the mod refusing to run on a game it does not
  recognise, and it is deliberate. It checks every single thing it needs to know about the game's
  memory before it installs anything at all, and if one of them cannot be found or no longer looks
  like itself, the whole mod stands down rather than running half-connected. The log names the one
  that failed. Usually this means the game has been patched and the mod needs an update; nothing
  is lost and your collection file is untouched.
- **The pages have the wrong background.** The plate textures are not in
  `settings\ui\caravan\` in the game's root folder. See step 4 of the install.
- **The tab is empty and the log mentions the catalogue.** Delete `catalogue.bin` and
  `catalogue.stamp` from `x64\uniquetab\` and start the game again; they are rebuilt from your
  installation.
- **A deposit was refused.** The log says which item and why, in a sentence, with what to do
  instead. Refusals never move anything.
- **You want everything back out.** Set `rescue=1` in the ini with the caravan open (or drop an
  empty `RESCUE-NOW` file next to it). Every stored item is handed back to the character and a
  report is written beside the log.

## Uninstall

Delete `uniquetab.asi` from `x64\`. The game runs, the tab is gone, and the Crafting Materials
page is back to normal.

The loader (`dinput8.dll`) is shared with any other ASI mod you use, so delete it only if this
was the only one.

You can delete `x64\uniquetab\` and `settings\ui\caravan\uniq_plate_*.tex` as well - but take
`uniq-items*.jsonl` out of the folder first if there is anything in your collection you would
like to keep, or hand the items back with `rescue=1` before you remove anything.

## Licence

MIT - see `LICENSE`. Third-party components and what comes from the game are listed in
`THIRD_PARTY.md`. This is an unofficial mod, not endorsed by or affiliated with the makers of
Grim Dawn.
