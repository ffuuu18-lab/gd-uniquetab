# Unique Collection Tab

A collection tab for Grim Dawn, built out of the caravan's Crafting Materials page.

Drop an Epic, a Legendary, a monster infrequent, a relic or a blueprint into it and the game keeps
a slot for that item the way it keeps one for an Aether Crystal: one box per item record, filled
or empty, sorted into category pages. It is a place to put the uniques you want to keep, and a way
to see at a glance what you have found and what you have not.

**Your items do not go into the game's save.** The collection lives in the mod's own file next to
the mod. Nothing the mod does is written into `player.gdc` or into any `.gst` stash file, so a
future patch, a broken mod or a plain uninstall cannot make the game prune what you stored. If
you remove the mod, your collection file stays on disk exactly as it was.

## New in 1.2.0

- **Monster infrequents.** Every monster infrequent - every Rare item the game gives a record of
  its own, the ones with the MI mark on the name plate, of every slot, faction-store and
  blacksmith-crafted rares included: 2,909 in the base game - is collected on the same pages as the
  uniques, after them. One box per item record (each level tier of an MI is a box of its own), and
  one roll per box: the box keeps the roll you put in exactly - its prefix, its suffix and its
  numbers -, a second roll of the same item is refused and the log names the one you already have,
  and hovering another roll of an MI you hold reads "In your collection (another roll)". The base
  game now has 196 pages (108 without them), Reign of Terror 109, DeathGrimDawn 282 with a new
  Artifacts page of 12 crystal weapons, Grimarillion 133. On by default; `collect_mi=0` turns it
  off, and the MIs you stored stay in the collection, hidden until it is 1 again (see *Settings*).
- **Custom games that add items get them in their collection.** Reign of Terror, DeathGrimDawn
  and games like them: a custom game that adds items gets its own collection list - its uniques,
  names and icons are read from the mod, and the buttons are that game's groups. The first entry
  after the mod (or this mod) was updated takes about 5-6 seconds.
- **Blueprints have a page.** Every blueprint of the game, of every rarity - 914 in the base game,
  on 12 pages after Relics - is collected on the Blueprints page (BLU), and a custom game's own
  blueprints land on the same page (Reign of Terror's runeword blueprints included). Put one in,
  take it back, and learn it after taking it, as with any other item. Crate's own 11 test
  blueprints are left out. A buttons file you already have gets the BLU button once (see *Your
  buttons*).
- **`+N not on these pages`.** The label line ends in `+N not on these pages` while your
  collection holds items no page of the game you are in shows - items that game does not have,
  items none of its groups takes, the MIs `collect_mi=0` hides. They stay in the collection and are
  back the moment a page takes them again.
- **Fixed: the collection line on a blueprint.** In 1.1.0 the line on a blueprint's tooltip
  described the item it crafts, so a blueprint read "In your collection" or "Not in your
  collection" according to that item. It now says whether you have the blueprint itself.
- **Also fixed.** Ctrl+PageUp / Ctrl+PageDown moved two buttons per press (and PageUp / PageDown
  two pages of rows); now one. The tab no longer stops for the rest of the session after a switch
  between custom games with a group open that the next game lacks. `CHANGELOG.md` lists everything.
- **Your buttons.** `uniquetab-buttons.ini` - one per game, written with that game's own buttons
  the first time you open the tab there - holds every button's caption, the page name, the label
  line and the tooltip lines. A button can hold several groups - all of their items on one page -,
  a group can be left off the pad, and captions and names in any alphabet draw without a font
  setting (see *Your buttons*).
- **Your own groups.** The groups are built in; an optional `uniquetab-groups.ini` adds pages of
  your own (see *Your own groups*).
- **Your own layout.** The `[layout]` lines move and resize the item grid, the buttons and the
  label line, and `layout_outline` shows their numbers on the page. Or drag them into place in the
  game: Ctrl+right-click on the buttons (*Edit the layout in the game*). The buttons and the label
  line can leave the caravan window and sit in a corner of the screen, and come back into it, as
  often as you like in one edit. Ctrl+wheel and Ctrl+PageUp / Ctrl+PageDown follow your buttons in
  the order the pad shows them.
- **Bigger pages on big caravans.** A page holds as many boxes as the caravan cover fits, up to
  512: Reign of Terror's rings show 30 x 17 instead of 30 x 5.
- **A shorter settings file.** `uniquetab.ini` shows four settings; every other one waits
  commented out at its end. Each custom game gets a complete settings file of its own (copied from
  the main one when you first enter the game, independent of it afterwards), and where you
  left the tab (the group, the row, OWN) is remembered per game in `uniquetab.state`.
- **Each custom game has its own log** (`mods\<name>\uniquetab.log`; with `/basemods`,
  `mods\<name>\basemods\uniquetab.log`).
- **One folder per custom game.** Everything the mod keeps for a custom game is in
  `x64\uniquetab\mods\<name>\` now (the name of the game's own folder under `mods\`), with the
  `/basemods` variant's generated files in a `basemods\` folder inside it. The first start of this
  version moves the old `views\custom-<name>\` and `views\basemods+custom-<name>\` folders
  there by itself and says so in the log.
- **English by default, in every language of the game.** The tab's buttons and label are English
  in the game's English font whatever language the game runs in; your buttons file changes that,
  nothing else does.
- **Bows, gems and runes.** Every group is named with the game's own word for its items: Reign of
  Terror's staff-class uniques are all "Bow" there, so they are one Bows page; its javelins are
  Hunting Spears. DeathGrimDawn keeps Bows & Crossbows, Gems & Runes and Augments. A mod's own
  gems, runes and jewels are collected at every rarity.
- **The materials page keeps every button.** On the game's own Crafting Materials page the whole
  button pad stays where you put it, drawn over the page's slots if they lie under it, and every
  button works there.

## New in 1.1.0

- **Works with stash-UI mods.** X-Mod, Falendar's UI Overhaul, Loot Ascension's Baul UI and mods
  like them that redraw the caravan: the collection pages are laid out for the window the mod
  draws, on a plain background that sits inside its frame, with no profile to pick.
- **Works in custom games, each with its own collection.** A custom game that brings its own
  caravan gets its pages built for it the first time you enter it, and keeps a collection of its
  own beside them, so what you store there stays there. The main campaign's collection is never
  touched while you play one.
- **Works on 1.3.1.1 unchanged**, and items a game update adds show up on the first launch after
  it.
- **Easier on memory.** Building the catalogue now takes about 100 MB and never holds a whole game
  archive, which matters on a Steam Deck.
- **Nothing to copy but the plugin.** The page database and the page backgrounds are now made by
  the mod on your machine, out of your own game files, so the package is the `.asi` and nothing
  else. Leftovers from an older version are replaced on the first launch.
- **`plate_ground`** in the ini sets the colour a stash-UI mod's collection background is filled
  with (see *Settings*).

## What you need

- Grim Dawn 1.3.0.8 or 1.3.1.1, 64-bit. The mod checks the game's shape, not its version
  number, and turns itself off if it does not recognise what it finds (see *When something goes
  wrong*).
- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader), the **x64** build.
  This mod is an ASI plugin: it does nothing until a loader loads it. The loader is not included
  here - download it once and it serves every ASI mod you use.
- Nothing else. No script extender, no launcher, no other mod.

## Install

Everything goes in the game's `x64` folder - the one that holds `Grim Dawn.exe`.

1. Take `dinput8.dll` from the x64 build of Ultimate ASI Loader and put it in `x64\`, beside
   `Grim Dawn.exe`. The loader's `winmm.dll` variant works too. Use exactly one of them: two
   loaders in one folder would load every plugin twice.
2. Copy `uniquetab.asi` from the package's `x64` folder into that same `x64\` folder.

That is all. Start the game and sit at the main menu for a few seconds.

**The first launch builds everything else.** The mod creates its folder, `x64\uniquetab\`, reads
your own installation - your archives, your language, your DLCs - and writes three things from
it: the item catalogue and the collection's page database, into `x64\uniquetab\`, and the page
backgrounds, into `settings\ui\caravan\` in the game's root folder (the one that holds `x64\`).
It takes a few seconds, once. It happens again after a game update, and never otherwise. The mod
writes only files named `uniq_plate_*.tex` in `settings\ui\caravan\` (a custom game's carry its
folder name, as in `uniq_plate_64x96_438x627_deathgrimdawn.tex`, and the frames for a grid of
your own end in `_L` and eight letters and digits), and only those, because that
folder is the one place the engine looks up a texture by resource name. They are painted on the
background a normal stash tab shows, so a stash-UI mod that changes the caravan gets backgrounds
that match it. If that folder cannot be written - a read-only install - the pages keep the plain
vanilla background and the log says so. The settings file `x64\uniquetab\uniquetab.ini` is
written at the same time: the four everyday settings, each with a line of explanation, and every
other one commented out at its end. The buttons file `uniquetab-buttons.ini` follows the first
time you open the tab.

**Upgrading from 1.1.x (or 1.0.x)** is the same two steps, with GD Stash closed: replace
`uniquetab.asi` and start the game. There is nothing to do by hand. Your collection, your settings
and your exports stay where they are, and the first start does the rest, once:

- `uniquetab.ini` is rewritten as `ini_version` 42 with every value kept - the four everyday
  settings on top, `collect_mi=1` the new one, everything else commented out at its end - and the
  log lists every line it dropped (see *Settings*);
- each custom game's folder moves from `views\` to `mods\<name>\` (see *Where each game's data
  is*); a folder GD Stash holds open is moved at the next start instead - close it and restart
  before you enter that game;
- every game's page database and backgrounds are rebuilt from your game files: the main
  campaign's at that start, each custom game's at its first entry, which the log calls a mod
  update;
- a custom game's first entry writes its own complete settings file, `mods\<name>\uniquetab.ini`,
  from the main file's values, and the first time you open the tab in a game its buttons file is
  written.

The folder move is one way: to go back to 1.1.x, move the folders back by hand first (*When
something goes wrong*).

**A custom game gets its own build.** A custom game - anything you pick from *Custom Game* in the
main menu, the Crucible included - loads its own database on top of the main one, and a stash-UI
mod bundled with it (Loot Ascension bundles Baul's) moves the caravan. The mod follows: the first
time you enter that game it builds the collection once more for that database, about three
seconds on the loading screen, and from then on it is instant. Those files go in
`x64\uniquetab\mods\<name>\` (with `/basemods`, `mods\<name>\basemods\`), so the main campaign's
own build is never touched and leaving the custom game costs nothing. The page backgrounds for it go into `settings\ui\caravan\` like the
others, under names that carry the game's own name so two mods cannot collide.

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

The plugin goes in the same place as on Windows - the game's own `x64\` folder - and the mod
writes its own files into `x64\uniquetab\` and the game's `settings\` folder there, **not** into
the Proton prefix. The prefix under `steamapps/compatdata/219990/pfx` holds only the fake Windows
drive and nothing of this mod.

## Using it

Open the caravan and go to the **Crafting Materials** page. Your usual materials are page one,
untouched; the collection pages follow.

- **Mouse wheel** scrolls the page.
- **Ctrl+wheel** and **Ctrl+PageUp / Ctrl+PageDown** walk the category buttons (Helms, Rings,
  Relics, Blueprints, and so on) in the pad's order - your buttons file's order -, MAT included:
  one stop per button, a button holding several groups once, one step per key press. A group your
  buttons file leaves off the pad is skipped.
- The **buttons across the top of the page** select a group too: the buttons of the game you are
  in - one per group unless your buttons file says otherwise -, plus MAT and OWN. MAT opens the
  game's own materials page, from the buttons in the window and from buttons you put on the
  screen alike. A button that holds several groups shows all of their items on one page. Should a
  game ever have more buttons than the pad can show readably, they come a page at a time and a
  `>>` button shows the next ones.
- The **stash search box** works: buttons holding a match are marked, so you can see where a name
  lives before you go there.
- One of those buttons is **OWN**. It toggles between showing every item in the group and showing
  only the ones you already have.
- **Hovering an item** the collection can hold - on a collection page or in your bags - adds one
  line to its tooltip: "In your collection" or "Not in your collection" (`tooltip_mark`; the words
  are yours to change in the buttons file). On a blueprint the line is about that blueprint, not
  the item it crafts: a blueprint you stored reads "In your collection" whether or not you have
  the crafted item, and one you have not stored reads "Not in your collection" even when you have
  that item.

**Blueprints** are items like any other here: the Blueprints page (BLU, right after Relics) holds
every blueprint of the game you are in - all four rarities, 914 in the base game on 12 pages, each
one the game's blueprint scroll in its rarity's colour. Put one in and take it back as below; to
learn it, take it back and right-click it as usual. Learning a blueprint uses it up, so the page
holds the ones you chose to keep rather than learn. Crate's own 11 test blueprints are left out.

**Monster infrequents** sit on their slot's page, after its uniques: the Helms page holds the 288
unique helms and then the MI helms (608 boxes in all), and the base game has 196 pages. Every Rare
item the game gives a record of its own is one: the ones dropped by monsters, the faction stores'
rares and the blacksmith's crafted ones alike. Each level tier is its own box (Incendiary Casque's
tiers sit side by side), and a box holds ONE roll, whatever `max_per_record` says: the roll you put
in comes back exactly - the same prefix, suffix and numbers. A second roll of the same item is
refused (it stays on the cursor or in your bag) and the log names the roll the box holds; to swap
them, take the stored one out first, then put the other in. Hovering an MI you hold in another roll
reads "In your collection (another roll)" - the words after "In your collection" are
`tooltip.roll` under `[tab]` of your buttons file. The Spears page has 64-wide cells now: the MI
spears are wider than the six unique ones. `collect_mi=0` takes the MIs off the pages (see
*Settings*).

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
- **A second roll of a monster infrequent** is refused: its box keeps one roll. The log line
  `deposit REFUSED: <record> - a monster infrequent keeps ONE roll per record ...` names the roll
  the box holds (its name, level, prefix, suffix and seed).
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

A custom game that brings its own caravan window (Loot Ascension's Baul UI, for example) gets a
page laid out for that window - generated once, a few seconds on the loading screen the first
time you enter it - and **a collection of its own**: its journal, its GD Stash export and its
rescue report live in `mods\<name>\` beside that page, and the main campaign's
`uniq-items.jsonl` is never read or written while you play it. The same mod with or without
`/basemods` is the same collection. A custom game that leaves both the caravan and the items
alone (the Crucible, a map or a class mod) uses the main campaign's page and collection; one that
adds or changes uniques gets its own page set and collection even when its caravan is the game's
own.

A custom game that adds items gets its own collection list: its uniques, names and icons are read
from the mod. The first entry after the mod (or this mod) was updated takes about 5-6 seconds. In
such a game the buttons are that game's groups, and a group without a name of its own is named
with that game's word for its items. Reign of Terror: 29 groups - Bows (every staff-class unique:
that is what the game calls them), Hunting Spears, Blueprints, Jewels, Gems & Runes, Other and the
slot groups - so 31 buttons with MAT and OWN; its Blueprints page holds 1,425 on 6 pages, its
runeword-craft blueprints included. DeathGrimDawn: 29 groups - Bows & Crossbows, Blueprints, Gems &
Runes, Augments, Other and the slot groups - so 31 buttons; its Blueprints page holds 1,805 on 23
pages. The built-in groups know those two by name; any other custom game (GrimArmory,
Grimarillion) gets the slot groups, the Blueprints and the generic rules, and your own groups file
can give its families pages of their own. The label and the search use
the mod's own item names. With monster infrequents (the default) Reign of Terror has 109 pages
(its Rare bows and javelins join Bows and Hunting Spears), DeathGrimDawn 282 with one group more,
**Artifacts** (ART: its 12 Rare crystal weapons, the game's own word for that class), and
Grimarillion 133; each game's junk Rare gear - Reign of Terror's NPC gear, DeathGrimDawn's
cosmetics, Grimarillion's "Replica" items - stays off the pages. An item you own that no page of
the game you are in shows - the game does not have it, or none of its groups takes it (a monster
infrequent with `collect_mi=0`, say) - stays in the collection and is not shown; the label ends in
`+N not on these pages` while there are such items, and they are back the moment a page takes
them again.

**Its own settings.** Each custom game has a complete settings file of its own,
`mods\<name>\uniquetab.ini`: the same `[general]`, `[layout]` and `[advanced]` as the main
`uniquetab.ini` (*Settings*, below), every line of it. The first time you enter the game it is
written with the main file's values as they are then - your `log_level`, `collect_mi`, rectangles
and the rest carry over once (a one-shot such as `rescue=1` does not). From then on the two files
are separate: while you play that game ONLY its own file is read - nothing is inherited from the
main one, a setting missing from it has its default, and editing the main file changes nothing in
that game (it applies again when you are back in the main campaign). Edit it while playing; it is
re-read within a second. `enabled=0` there turns the tab off in that game only, `log_level` sets
that game's logging, `collect_mi` its monster infrequents (its pages are generated again once, at
its next entry; the main campaign's are not), and the layout editor and every one-shot that sets
itself back write into it. The one exception is `journal_dir`: it is read once at start-up from
the main file, before any custom game is known, so the game's file shows it as a commented line
saying so, and a `journal_dir` line you add there is ignored with one line in the log. Delete the
file and it is written again from the main file's values.

### Where each game's data is

Everything the mod keeps for a custom game is in ONE folder, `x64\uniquetab\mods\<name>\` -
`<name>` is the game's own folder under the game's `mods\` (`mods\reignofterror\`). It holds the
game's collection (`uniq-items.jsonl`, `uniq-items-hc.jsonl`), its exports (`uniq-export*.gds`,
`uniq-export.csv`), its rescue report, its own settings (`uniquetab.ini`) and its own buttons
(`uniquetab-buttons.ini`) - one set, with or without `/basemods`. Beside them are the generated set
of the game played WITHOUT `/basemods` (`catalogue.bin`, `catalogue.stamp`, `uniq-*.txt`,
`uniq-pages.arz` - delete them and they come back), **that game's own log**, `uniquetab.log` (+
three older copies): every line written from the moment you entered the game until you left to
the main menu - and `uniquetab.state`, where you left the tab in that game. With `/basemods` the
game loads the base mods' database under it, so its list of items differs: that variant's
generated set, log and `uniquetab.state` are in `mods\<name>\basemods\`, so switching never
forces a rebuild; nothing of yours is in there. The main `uniquetab.log` has every line too, so
either log is enough for a report. The main `uniquetab.ini` and `uniquetab-groups.ini` are one set
for every game. The page backgrounds go into the game's `settings\ui\caravan\` folder with the
game's name in theirs. (1.1.0 kept these files in `views\custom-<name>\` and
`views\basemods+custom-<name>\`; the first start of a newer version moves them here, see *When
something goes wrong*.) When `journal_dir` is set, the collection files are in
`<journal_dir>\mods\<name>\` instead.

## Settings

`x64\uniquetab\uniquetab.ini` is written by the mod on the first launch and re-read about once a
second, so you can edit it while the game runs. A value outside its allowed range is clamped and
the log says so. Delete the file and it comes back with the defaults. It is the settings of the
main campaign (with or without `/basemods`); each custom game you enter gets a complete copy of
its own, `mods\<name>\uniquetab.ini`, with every key below but `journal_dir` - see *Its own
settings* under *The Crucible and other custom games*.

It has three parts: the four everyday settings under `[general]`, your rectangles under
`[layout]`, and every other setting under `[advanced]` at the end, commented out at its default.

**[general]**

| key | what it does |
| --- | --- |
| `enabled` | 0 = the mod goes quiet: no tab, nothing new collected. Nothing is lost |
| `collect_mi` | 1 (the default) = monster infrequents are collected too: every Rare item record of a slot, faction-store and crafted rares included, one box per record and one roll per box, after the uniques on that slot's page. 0 = they are not, and the ones you stored stay in the collection, hidden until it is 1 again (the label counts them as `+N not on these pages`). Takes effect at the next start (the pages are generated again once); in a custom game's own file, at that game's next entry, for that game only |
| `tooltip_mark` | 1 = every item tooltip says whether it is in your collection |
| `log_level` | how much goes in the log: error, warn, info, debug, trace (trace also writes every line at once) |

The file says `ini_version=42`. An older file is carried over on the first start with every value
kept: a file older than 40 gets the `collect_mi=1` line, and `max_per_record` and `rescue` move from
`[general]` into `[advanced]` - commented out at their default, or as a live line there if you had
set them (`max_per_record=2` stays `max_per_record=2`). A custom game's own file says the same
number and is carried over the same way, on that game's first entry.

The rescue (`rescue`, an untested setting under `[advanced]`) has a second trigger that needs no
editing: put an empty file called `RESCUE-NOW` next to the ini. The mod sees it, deletes it and runs the rescue. Items the current game's database
does not have are skipped (and listed in `rescue-report.txt`) - their records stay in the
collection untouched, for the game that has them.

**[advanced]** - every other setting, one line each, commented out at its default with what it
does beside it. Remove the `;` in front of one to change it; the line keeps its place, and a
one-shot that has run goes back to its commented line (in the file that set it - a custom game's
own while you play one).

| key | default | what it does |
| --- | --- | --- |
| `ui_scale_pct` | 0 | 0 = follow the game's UI scale, else force one in percent (50..300) |
| `mp_collect` | 1 | 1 = the collection also works while you play multiplayer |
| `collect_pristine_only` | 0 | 1 = refuse items with affixes, components, augments or rerolls |
| `max_per_record` | 1 | how many copies of one item the collection keeps, 0 = no limit (untested). A monster infrequent keeps one roll whatever it says |
| `plate_label` | 1 | 1 = draw the group name, the counts and the row window over the page |
| `plate_label_size` | 13 | height of the label text, in page pixels (8..32) |
| `pad_caption_max` | 14 | largest caption size on the buttons; the button size limits it too (6..36) |
| `plate_ground` | empty | a modded page's flat background, hex `RRGGBB`. Empty = the vanilla tone |
| `plate_swap` | 1 | 1 = the background picture follows the group, 0 = always the vanilla one |
| `layout_outline` | 0 | 1 = outline the grid, the buttons and the label with their numbers - green where your own rectangle is in force, red where one was refused and why |
| `layout_edit` | 0 | 1 = open the in-game layout editor on the next collection group (sets itself to 0) |
| `journal_dir` | empty | where the collection file is kept (read at start-up). Empty = the mod's own folder. Main `uniquetab.ini` only |
| `export_gds` | 1 | write uniq-export.gds for GD Stash: 0 off, 1 the collection, 2 all |
| `export_csv` | 0 | write uniq-export.csv for a spreadsheet: 0 off, 1 the collection, 2 all |
| `journal_prune` | 0 | 1 = drop the entries that are no longer stored (sets itself to 0) |
| `rescue` | 0 | 1 with the caravan open = hand every stored item back to the character, sets itself to 0 (untested) |
| `padmat_<W>x<H>` | none | the buttons on the game's own materials page, `x,y,w,h` for one page size (see below) |

One of them is a matter of taste: **`plate_ground`**, six hex digits `RRGGBB`. Under a stash-UI
mod the collection pages are painted on one flat colour, because the mod's own background carries
a grid of its own that ours would not line up with. Empty (the default) takes the tone of the
vanilla stash background, a near-black `060606`. Changing it repaints those backgrounds once, at
the next launch; the vanilla pages never use it.

**Where you left the tab** - the group, the row and the OWN filter - is not a setting: the mod
keeps it in `uniquetab.state` beside `uniquetab.ini` (and a custom game's in its own view folder),
so each game opens where you left it. Deleting it only makes the tab start on the materials page.

### [layout]

Where the grid, the buttons and the label line go. Every line is optional; without them the page
is laid out as it always was. The in-game editor writes these lines for you.

| key | what it does |
| --- | --- |
| `grid_<W>x<H>`, `pad_<W>x<H>`, `label_<W>x<H>` = `x,y,w,h` | your own rectangles for the caravan cover of that size, in the materials window's own pixels at UI scale 1 (the outline prints them) |
| `pad_screen`, `label_screen` = `<corner>,x,y,w,h` | the buttons or the label line pinned to a corner of the SCREEN instead of the window (below) |

`<W>x<H>` is the cover: `438x627` for the base game and DeathGrimDawn, `346x625` Loot Ascension,
`974x659` Reign of Terror, `975x968` X-Mod. The grid takes as many whole cells as fit from its
top-left - a page holds as many boxes as the cover fits, at most 512 -; the buttons are
re-arranged for the largest readable captions; the label's height limits `plate_label_size`.
Saved changes apply within a second. A rectangle that leaves the page, is too small, or overlaps
another part is ignored with one line in the log, and the automatic one is used. When you move
the grid, the page shows the plain background for a second or two while the mod paints frames
for the new grid - one under every box, a column at the window's very edge included -, then
switches to them; they are kept for the next start. Removing the line
brings the original frames back.

**Off the window.** `pad_screen=tr,8,8,320,44` puts the buttons in the top-right corner of the
screen: the corner is `tl`, `tr`, `bl` or `br`, `x,y` the distance from that corner inwards and
`w,h` the size, in the same page pixels as the other lines, scaled with the UI scale like the
page. `label_screen` does the same for the label line. One line each serves every caravan and
stash mod of its file: the main `uniquetab.ini`'s in the main campaign, a custom game's own
file's in that game. The pinned parts are drawn above the
HUD (the minimap, the quest tracker) and below every tooltip, on a dark ground with a thin frame,
and only while the caravan is open on the Crafting Materials page; their clicks are theirs - every
button does there what it does in the window (MAT, OWN and `>>` included), a click on them never
moves the character, and a plain right-click on them fires no skill. On the game's own materials
page a pinned pad stays in its corner. A line that would cover the item grid, has no room for the
buttons or the text, or is as large as the screen is refused with one line in the log, and the
part stays in the window. The grid always stays in the window. Delete the line to put the part
back. While a part is on the screen its `pad_<W>x<H>` / `label_<W>x<H>` line is not used and not
judged - no warning about it -, and it is in force again when the part goes back into the window.

**The game's own materials page.** There the buttons are drawn where your pad is, over the page's
own slots if they lie there: every button works, and a click or a hover on the pad never reaches
a slot under it (slots beside the pad work as always). `padmat_<W>x<H>` under `[advanced]` moves
the buttons somewhere else on that page only; it has to fit inside the page and hold the buttons,
or it is ignored with one line in the log.

**Edit the layout in the game.** With the caravan open on a collection group, hold Ctrl and
right-click the button pad (or a pinned label line, or set `layout_edit=1`; it sets itself back to
0 when the editor opens). The items are hidden while you edit; the page shows the item grid (with
the current group's cells as lines), the button pad and the label line as outlined rectangles with
handles. Drag a rectangle to move it, or a corner or an edge to resize it: the grid stays on the
page and snaps to whole cells; drag the buttons or the label line off the window to put them
anywhere on the screen (the status line says "on the screen - SAVE writes pad_screen"), and back
onto the page to return them. The buttons and the label line move freely between the caravan and
the screen, in both directions, as often as you like in one edit - also the first time, before
anything is pinned: a part you bring back into the window can be moved, resized and saved there
like any other. A drag begun in the window may carry them straight out over the
world: the character does not move while you drag, hold still or let go there. A rectangle
turns red when it cannot be used - it overlaps another one, it is too small for the buttons or
the text, or, on the screen, it would cover the item grid or is as large as the screen - and SAVE
is off while one is red. SAVE writes `pad_screen` /
`label_screen` for a part on the screen (pinned to the nearest corner)
and `grid_/pad_/label_<W>x<H>` for the parts in the window, all into the custom game's own file
while you play one (a part you left where it was by default has its line removed); the page follows
within a second or two (a moved grid gets its frames repainted, as for a key you type yourself).
The SAVE / CANCEL / RESET buttons sit at the top right of the button pad and move with it. CANCEL
leaves without writing anything, and it always answers: in the middle of a drag, with an item on
the cursor (the item stays on it), and right after SAVE (what SAVE wrote stays written; the page
follows the file a moment later). RESET puts all three back in the window where the mod places
them (SAVE then removes their lines, the screen ones too). A click on the editor after a drag
whose release the game missed ends that drag first, then counts as the click. Esc - closing the
caravan -, switching to another caravan page or to the game's own materials page, alt-tabbing,
changing the UI scale or the resolution cancels the edit. An item on the cursor is never taken
by the editor. It does not move the buttons of the game's own materials page (`padmat_`). You can
put the buttons anywhere on the page, even above the label line or over the materials page's
slots - there they are drawn over the slots and take the clicks.

### Your buttons

`uniquetab-buttons.ini` holds the tab's buttons and words. There is one per game: the main
campaign's beside `uniquetab.ini` (for the base game with or without `/basemods`), and a custom
game's in `mods\<name>\`. The mod writes it the first time you open the tab in that game,
filled in with that game's buttons - one per group, in the group order (the base game's last two
are `[REL]` Relics and `[BLU]` Blueprints) - and after that changes only its `[unassigned]` section,
plus, once, the button of a built-in group an update brings (below). Edit it while you play: it is
re-read within a second. Delete it and it is written again with the defaults.

- **`[TAG]`** is one button; the section name is its tag, 1 to 3 characters (`MAT`, `OWN` and
  `TAB` are taken). The buttons appear in the order of the file. Its lines:
  - `groups =` the groups on it, by id (required). Several ids, separated by commas, make one
    button with ONE page of all their items: the first group's items, then the next group's, in
    the group order, in one grid with one label (its counts summed), one scroll and one OWN
    filter; Ctrl+wheel stops at the button once. The cells are the smallest cell size of this
    game's pages that holds every group's items, so a smaller item sits at the top-left of a
    larger cell (a ring in a 64 x 96 cell, say). Should no page of the game have such a cell, the
    page shows the plain background without frames, and the log says so once. A group can be on
    one button only.
  - `name =` the page name in the label line (default: the first group's name).
  - `caption =` the button's text when it should not be the tag (up to 12 characters).
  - `size =` this button's largest caption size, 6..36; `font =` this button's font, a font of
    the game by its path (`fonts/ru/savapromedium.fnt`).
- **`[tab]`** holds the words of the whole tab:

  | key | what it is |
  | --- | --- |
  | `label.format.1` .. `label.format.4` | the label line, up to four templates: the first that fits is drawn. Placeholders: `{mode}` `{group}` `{owned}` `{entries}` `{first}` `{last}` `{rows}` `{all}` `{coll}` |
  | `label.mode_unknown`, `label.unknown` | the words for "mode unknown - " and an unknown number (`?`) |
  | `tooltip.yes`, `tooltip.no` | the two lines every item tooltip gets |
  | `tooltip.roll` | the words after `tooltip.yes` when you hover a monster infrequent you hold in another roll (default ` (another roll)`). They are appended as written, so quote them to keep the leading blank: `tooltip.roll = " (noch ein Wurf)"`. Not in the generated file - add the line yourself |
  | `mat.caption`, `own.caption`, `next.caption` | the captions of MAT, OWN and `>>` |
  | `font.caption`, `font.label` | the font of the captions and of the label line (a font of the game by its path) |
  | `size.label`, `size.caption_max`, `size.caption` | the label size and the largest caption size (they win over `plate_label_size` and `pad_caption_max`), one fixed caption size |
  | `color.caption`, `color.label`, `color.ring`, `color.mark` | colours, `RRGGBB` |

- **`[unassigned]`** lists the groups no button holds - their items have no page. The mod rewrites
  this section (only this one) whenever that set changes: move an id from it into a button's
  `groups =` line to show it.
- **A new built-in group gets its button once.** When an update brings a built-in group your file
  has no button for - in 1.2.0 the Blueprints, and in a game that gains one with the monster
  infrequents the Staves / Bows or Hunting Spears / Artifacts group (DeathGrimDawn's `[ART]`) - the
  mod adds that button to your existing file ONCE: a `[BLU]` section (`name = Blueprints`,
  `groups = blueprint`) after your last button, and the id on the line
  `; added once by the mod - delete its button and it stays deleted: blueprint` under
  `[unassigned]` (an Artifacts button: `[ART]`, `groups = spear1h`, and the id on the same line).
  Nothing else in the file changes, and the log says so in one INFO line. That comment line is the
  mod's memory: delete the `[BLU]` section and the group stays under
  `[unassigned]` and is never added again (delete the line as well and it is added once more). A
  group you had already put under `[unassigned]` yourself is left there. A file the mod writes
  fresh has `[BLU]` and the line already.
- A line starting with `;` or `#` is a comment, and so is the rest of a line after a blank and a
  `;`. Put a value in "double quotes" to keep blanks or a `;` in it. UTF-8.
- **Alphabets.** Every alphabet draws, with no font line. Latin, Cyrillic and Greek letters draw
  in the game's own fonts (the copy of the game's font that has them). Any other letter draws in
  the game's Chinese, Japanese or Korean font (`fonts/zh/`, `fonts/ja/` or
  `fonts/ko/savapromedium.fnt`), chosen by the letters: kana -> Japanese, Hangul -> Korean, other
  Han -> Chinese. A letter none of the game's fonts has - Thai, Arabic, Hebrew, Devanagari ... - is
  drawn by Windows' own fallback font, in Windows' style. The font is chosen once for all the
  captions and once for the label line, by all of their letters together: one Chinese caption puts
  every caption in the Chinese font (a button's own `font =` keeps that button apart). A
  `font.caption` / `font.label` line, or a button's `font =`, names a font the game ships
  (`fonts/ru/savapromedium.fnt`) and wins. The limits: no right-to-left order and no joined or
  shaped letters (an Arabic or Hindi word draws as separate letters, left to right); nothing above
  U+FFFF (emoji): a caption holding one falls back to the tag, a name to the group's own name, and
  the log says so once at WARN; under Wine/Proton the Windows fallback may be missing, so letters
  none of the game's fonts has may draw blank there.
- A file with a mistake - an unknown key, an id on two buttons, an id no group of this game has -
  is ignored as a whole and the log names the line; the buttons in force stay (or the built-in
  ones, one per group).

Example - in Reign of Terror, bows and hunting spears on one button: in
`mods\reignofterror\uniquetab-buttons.ini` change the `[BOW]` section to

```
[BOW]
name    = Bows
groups  = staff, spear1h
```

and delete the `[SPE]` section. Within a second the pad has one BOW button, and its page holds the
76 bows and then the 19 hunting spears in one grid (each group's monster infrequents after its
uniques), in cells of 64 x 160 (the hunting spears'), and the label counts them all.

### Your own groups

The tab's groups are built in. To change them, create `uniquetab-groups.ini` beside
`uniquetab.ini` (UTF-8). It ADDS to the built-in groups: your blocks are tried first, so a block of
yours takes its items off their usual page (and can collect what a built-in `[exclude]` leaves
out - keep your filters narrow). Your new groups come first in the group order; the buttons file
decides where their buttons go.

- `[group <id>]` - a block of rules for one group; the id is `a-z 0-9 _ . -`, up to 40. Keys:
  `label` (the group's name), `tag` (the 3-character button caption), and the filters `class`,
  `path` (record path patterns: `*` one folder level or part of one, `**` any depth), `rank`
  (Legendary, Epic, Rare, Common, any), `flag` (set, relic, equipment, faction, blueprint,
  augment, extra), `source` (base or mod). Values in one key are alternatives; every key of a
  block must match. Without `label`/`tag` a group is named with the game's own word for its
  class in that game (Reign of Terror's staff class is "Bows").
- `[exclude]` - a block whose items stay off every page (built-in groups included).
- `[view:<key>]` - the blocks after it apply only in that game: `vanilla`, `basemods`, or a
  custom game's folder name in lower case (`reignofterror`); `[view:*]` ends it. The key
  `collect_mi` is every game while `collect_mi=1`: the built-in groups put the monster infrequents
  on their pages there (DeathGrimDawn's crossbow folders first, then one Rare rule per slot group
  and the Bows / Hunting Spears classes). An `[exclude]` with `rank = Rare` and `flag = equipment`
  (and a `class` or `path`) keeps MIs of your choice off the pages.
- `[auto]` - `min = N`, `enabled = yes|no`: a class nothing names gets its own group when it has
  at least `min` items (yours overrides the built-in setting).
- A block with a built-in id (`[group ring]`, `[group staff]` ...) adds rules to that group; its
  `label`/`tag` replace the built-in ones. Two groups with the same name stay two buttons -
  putting groups under one button is the buttons file's job.
- A file with an error is ignored as a whole; the log names the file and the line, and the
  built-in groups are used.
- An unedited copy of a groups file an older version wrote is renamed `uniquetab-groups.ini.old`.
- A change regenerates the pages of each game once, at its next start or entry.

Example - Reign of Terror's runewords on a page of their own:

```
[view:reignofterror]
[group my.runewords]
label = Runewords
tag   = RWD
path  = records/0_rot/items/runeword/**
```

## Multiplayer

The collection works in multiplayer, **as the host and as a client**, with `mp_collect=1` (the
default): deposits and takes behave as they do in single player, and every player keeps their own
collection file on their own machine. Set `mp_collect=0` if you would rather collect only in single
player.

The mod never changes the database checksum the game compares between players, so nobody is locked
out of a game because someone has it installed.

## Your collection, and backing it up

Everything lives in `x64\uniquetab\`:

| file | what it is | whose |
| --- | --- | --- |
| `uniq-items.jsonl` | **this is your collection** (softcore) | yours - back it up |
| `uniq-items-hc.jsonl` | the same, hardcore | yours - back it up |
| `uniq-export.gds`, `uniq-export-hc.gds`, `uniq-export.csv` | a stash file GD Stash can import, a spreadsheet | the mod's, rewritten from the collection |
| `uniquetab.ini` | your settings | yours |
| `uniquetab-buttons.ini` | the main campaign's buttons and words | yours (the mod wrote it once; it rewrites only `[unassigned]`, and adds a new built-in group's button once) |
| `uniquetab-groups.ini` | your own groups, if you made one | yours (the mod never writes it) |
| `uniquetab-groups.ini.old`, `uniquetab-text.ini.old` | an older version's groups file and words, renamed | yours to delete |
| `uniquetab.state` | where you left the tab | the mod's; deleting it only resets that |
| `uniquetab.log` (+ three older copies) | what happened last session | the mod's |
| `rescue-report.txt` | what the last rescue handed back or skipped | the mod's |
| `catalogue.bin`, `catalogue.stamp`, `uniq-*.txt`, `uniq-pages.arz` | generated from your game | the mod's; delete them and they come back |
| `mods\<name>\uniq-items*.jsonl` | **that custom game's collection** | yours - back it up like the main one; never deleted by the mod |
| `mods\<name>\uniquetab.ini`, `uniquetab-buttons.ini` | that custom game's own settings and buttons | yours |
| `mods\<name>\` (the rest), `mods\<name>\basemods\` | the generated set again for a custom game you have entered (without and with `/basemods`), its `uniquetab.state`, its own log (+ three older copies), its exports and rescue report | the mod's |

**Back up `uniq-items*.jsonl`** (and `mods\*\uniq-items*.jsonl` if you play a custom game
with its own collection). That file *is* the collection: every stored item, with its full
identity, one item per line. It is plain text - you can open it, search it, and delete a line to
forget an item. Copy it somewhere safe as often as you would back up a save. An item you own that
no group takes any more stays in it, not shown (the label counts it as `+N not on these pages`)
until a group takes it again - nothing you deposited ever leaves the collection.

`uniq-export*.gds` is written beside it for convenience: point **GD Stash** at it to browse your
collection outside the game. It is an export, not the original - the `.jsonl` is the one to keep.

## The files the mod writes

Four folders, and in each the mod writes only the names below. *Yours* is read by the mod and
not rewritten by it (except the part named); *the mod's* is rewritten or rebuilt at will, and
deleting it loses nothing you made.

| folder | yours | the mod's |
| --- | --- | --- |
| `x64\uniquetab\` | the journals `uniq-items.jsonl` and `uniq-items-hc.jsonl` (the collection; in `journal_dir` when that is set); `uniquetab.ini` (the mod writes it once, then only a one-shot setting going back, the editor's SAVE and an upgrade); `uniquetab-buttons.ini` (written once, then only `[unassigned]` and, once, a new built-in group's button); the optional `uniquetab-groups.ini` (never written) | `uniquetab.state`; the logs `uniquetab.log` (+ three older copies); `rescue-report.txt`; the exports `uniq-export*.gds`, `uniq-export.csv`; the generated set `catalogue.bin`, `catalogue.stamp`, `uniq-records.txt`, `uniq-pages.txt`, `uniq-pages.arz`, `uniq-groups.txt` |
| `x64\uniquetab\mods\<name>\` (a custom game you entered; `<name>` is its folder under the game's `mods\`) | that game's journals (in `<journal_dir>\mods\<name>\` when `journal_dir` is set), its own complete `uniquetab.ini` (written once from the main file's values, then only a one-shot going back, the editor's SAVE and an upgrade) and its `uniquetab-buttons.ini` | its exports and `rescue-report.txt`; the generated set (the same six files), log (+ three older copies) and `uniquetab.state` of the game run without `/basemods` |
| `x64\uniquetab\mods\<name>\basemods\` (the same game run with `/basemods`) | - | that variant's generated set, its log (+ three older copies) and its `uniquetab.state` |
| `settings\ui\caravan\` in the game's root folder | - | the page backgrounds, `uniq_plate_*.tex`, and nothing else |

Turning `collect_mi` off or on changes no name here: in the main `uniquetab.ini` it regenerates the
main campaign's sets once, at the next start; in a custom game's own file, that game's sets only,
at its next entry.

The generated set: `catalogue.bin` is every collectible item of that game with its name and icon,
and `catalogue.stamp` what it was built from (a change rebuilds it); `uniq-records.txt` the item
records in box order, one per line; `uniq-pages.txt` one line per page (its index, its record, its
title, its box count); `uniq-pages.arz` the page database the game loads; `uniq-groups.txt` the
pages as the tab reads them. That last one is plain text, one line per thing, its fields separated
by tabs:

| line | fields | how many |
| --- | --- | --- |
| `F` | `<frame record> <box count> <vanilla box count> <x0> <y0> <row gap>` | one: the page frame, its boxes, the grid's origin and row gap |
| `P` | `<x0> <y0> <usable w> <usable h> <plate w> <plate h> <plate bitmap>` | one: the area the grid may use and the background it sits on |
| `V` | `<box record>` | one per vanilla box (the materials page's own) |
| `G` | `<index> <label> <cols> <rows> <cell w> <cell h> <item count> <id> <tag>` | one per group, in page order |
| `E` | `<box record> <item record>` | one per item of the group above |

To see exactly what is in a group, read its `G` line and the `E` lines below it: the `<id>` is the
one a button's `groups =` names, the `<tag>` its default button, and each item is named by its own
record, as the game's database names it (`records/items/gearhead/d004_head.dbr`).

## When something goes wrong

The first thing to do is read `x64\uniquetab\uniquetab.log`. It is plain text and it is written to
explain itself. If you report a problem anywhere, send that file.

- **The tab is not there at all.** The mod is not loaded. Check that the loader's `dinput8.dll`
  and `uniquetab.asi` are both in `x64\`, beside `Grim Dawn.exe`. If there is no log file at all,
  the loader never loaded the plugin.
- **The tab is not there and the log says `uniq-pages.arz NOT FOUND`.** The first launch could not
  build the page database; the `catalogue:` lines above it say why. Your usual Crafting Materials
  page is untouched and nothing is lost. Fix what the log names and start the game again.
- **The log says the mod is OFF.** That is the mod refusing to run on a game it does not
  recognise, and it is deliberate. It checks every single thing it needs to know about the game's
  memory before it installs anything at all, and if one of them cannot be found or no longer looks
  like itself, the whole mod stands down rather than running half-connected. The log names the one
  that failed. Usually this means the game has been patched and the mod needs an update; nothing
  is lost and your collection file is untouched.
- **The tab is gone in a world** (no category buttons, Ctrl+wheel does nothing, only the rollover
  line still works). Look for `live: the tab is off in this world (the <view> view) because ...` in
  `uniquetab.log`: it names the reason and whether a restart or the next world brings the tab
  back. Send that line and the ERROR line above it.
- **The pages have the plain background.** The mod could not write its page backgrounds into
  `settings\ui\caravan\` in the game's root folder; the log says which plates were painted and
  where they went, or why they could not be.
- **A box shows a grey checker (or another placeholder) instead of the item's picture.** The
  item comes from a mod whose item record names no icon, or names one the mod does not ship. The
  tab asks the game to draw the icon the record names, exactly as the game's own windows do, so
  the same item looks the same in that mod's shops and in your inventory. It is the mod's data:
  nothing in the tab fixes it, and the box works like any other.
- **The tab is empty and the log mentions the catalogue.** Delete `catalogue.bin` and
  `catalogue.stamp` from `x64\uniquetab\` and start the game again; they are rebuilt from your
  installation.
- **A deposit was refused.** The log says which item and why, in a sentence, with what to do
  instead. Refusals never move anything.
- **On the game's own materials page the buttons cover some slots.** That is by design: the pad
  stays where you put it and is drawn over them, and a click or a hover on it is the pad's. Use the
  slots beside the pad, move the pad with the editor, or give that page a place of its own with
  `padmat_<W>x<H>` (see *[layout]*). The log has one line naming the pad's rectangle and how many
  of the page's slots it covers.
- **The buttons are on the screen but a click does nothing, or walks the character.** Look for
  `screen pad input:` in the log: `not installed` or `OFF for this session` says why the pinned
  buttons take no click this session (the buttons in the window are not affected). Pinned buttons
  that do not show at all: they show only while the caravan is open on the Crafting Materials
  page; a `layout: pad_screen=... - the pad stays in the window` line says why the line was
  refused, and `buttons: the screen pad's draw site could not be installed` means the pad and the
  label stay in the window for this session.
- **The layout editor's buttons do not answer.** CANCEL always does - mid-drag, with an item on
  the cursor, right after SAVE - and Esc (closing the caravan) cancels too. SAVE is off while a
  rectangle is red; move it until it is not. A drag that ended out of the game's sight is ended by
  the next click (the log says `its release was never seen, the next click ended the drag`).
- **A change to the buttons file does nothing.** A file with a mistake is ignored whole: look for
  `buttons: ... is IGNORED - line N: ...`. Remember the file is per game - a custom game's is in
  `mods\<name>\`.
- **A second copy of a monster infrequent is refused.** By design: an MI box keeps one roll. The
  log says `deposit REFUSED: <record> - a monster infrequent keeps ONE roll per record and this box
  already holds it - "<name>" level N: prefix ..., suffix ..., seed ...`. Take the stored one out
  first if you want to keep the other roll instead.
- **My pages doubled after the update.** That is the monster infrequents (`collect_mi=1`, the
  default): every Rare item of a slot has a box after the uniques - 196 pages in the base game
  instead of 108. Set `collect_mi=0` in `uniquetab.ini` and restart to go back: the pages are
  generated again once, and any MI you had stored stays in the collection, hidden (the label says
  `+N not on these pages`) until you set it back to 1. Nothing is lost either way.
- **There is no ART button (DeathGrimDawn).** The Artifacts group comes with the monster
  infrequents; a buttons file you already have gets `[ART]` once (the log says `a new built-in group
  got its button once: spear1h`). If `spear1h` sits under `[unassigned]`, add a section `[ART]` with
  `groups = spear1h`, or move the id into another button's `groups =` line.
- **There is no BLU button.** The mod adds it once to a buttons file you already have, and the log
  says `a new built-in group got its button once: blueprint` that time. If `blueprint` is under
  `[unassigned]` - you deleted the button, or had put the group there yourself - add a `[BLU]`
  section with `groups = blueprint`, or move the id into another button's `groups =` line.
- **A button of several groups shows its page without frames.** No page of that game has a cell
  that holds every group's items; the log says `live: one grid per button - ... no plate of this
  view is painted for that cell, so the page shows the plain cover`. The items work all the same;
  put groups of one cell size together for framed cells.
- **You want everything back out.** Set `rescue=1` in the ini with the caravan open (or drop an
  empty `RESCUE-NOW` file next to it). Every stored item the game you are in knows is handed back
  to the character and a report is written beside the log; `rescue-report.txt` names the roll of
  every affixed item it hands back.
- **The tab stays English on a game in another language.** That is the default: the buttons and
  the label are English, in the game's English font, in every language of the game. Write your
  words into the game's `uniquetab-buttons.ini`: a button's `caption =` and `name =`, the label
  templates and the tooltip lines under `[tab]`. Letters the English font lacks are drawn in the
  game's font that has them, and letters no font of the game has are drawn by Windows' fallback
  font (see *Your buttons*, "Alphabets"); the log's `buttons:` lines name the font each part is
  drawn in and any letter that could not be drawn, with the line to add when a font line helps.
  The two rollover lines are drawn by the game in its own font, so their letters need a game
  running in a language whose font has them.
- **The log says `files: the custom games' folders moved from views\ to mods\`.** Once, on the
  first start after the update: each custom game's folder `views\custom-<name>\` became
  `mods\<name>\` and `views\basemods+custom-<name>\` became `mods\<name>\basemods\` - renamed, not
  copied, nothing merged or overwritten. If a `mods\<name>\` was already there, the old folder is
  left where it is with a `files: ... was NOT moved` line naming both, and the mod uses the new one:
  compare the two and move what you want to keep by hand. A folder that could not be moved because
  a program held a file in it open (GD Stash) says `could not be moved`: close that program and
  restart the game before you enter that custom game, and the move is tried again. `views\` is
  deleted only when it is empty.
- **I went back to an older build and my custom game's collection is gone.** Older versions
  (1.1.0 and before) look in `views\`, not in `mods\`; nothing is lost. Move the folders back
  before you start the old build - `mods\<name>\` to `views\custom-<name>\` (the `basemods\`
  folder inside it can simply be deleted; the old build regenerates it). The next newer build
  moves them forward again.

## Uninstall

Delete `uniquetab.asi` from `x64\`. The game runs, the tab is gone, and the Crafting Materials
page is back to normal.

The loader (`dinput8.dll`) is shared with any other ASI mod you use, so delete it only if this
was the only one.

You can delete `x64\uniquetab\` and `settings\ui\caravan\uniq_plate_*.tex` as well (every page
background the mod painted, a custom game's and the frames for a grid of your own included,
matches that name) - but take `uniq-items*.jsonl` out of the folder first if there is anything in
your collection you would like to keep, or hand the items back with `rescue=1` before you remove
anything.

| what | where | `undeploy.bat` |
| --- | --- | --- |
| the plugin | `x64\uniquetab.asi` | removes it |
| the page backgrounds | `settings\ui\caravan\uniq_plate_*.tex` | removes them (nothing else in that folder) |
| the generated set | `catalogue.bin`, `catalogue.stamp`, `uniq-*.txt`, `uniq-pages.arz` | removes them |
| your collection and its exports | `uniq-items*.jsonl`, `uniq-export*` | keeps them - and refuses to run while the mod still holds items |
| your settings, buttons and groups | `uniquetab.ini`, `uniquetab-buttons.ini`, `uniquetab-groups.ini`, the `.old` copies | keeps them |
| the mod's notes | `uniquetab.state`, `uniquetab.log` (+ copies), `rescue-report.txt` | keeps them |
| every custom game's folder | `mods\` (and an old `views\`) | never touches it |

A custom game with a collection of its own keeps it in `mods\<name>\`: take that
folder's `uniq-items*.jsonl` out as well, or hand its items back with `rescue=1` while you play
that custom game - a rescue returns the collection of the game you are in, and writes its
`rescue-report.txt` in the same folder. `undeploy.bat` checks every one of these collections
before it removes anything (in `mods\` and in an old `views\` alike), and neither it nor the mod
ever deletes the `mods\` folder: it holds your data (and each custom game's own settings, buttons
and log), so it stays until you remove it yourself.

## Licence

MIT - see `LICENSE`. Third-party components and what comes from the game are listed in
`THIRD_PARTY.md`. This is an unofficial mod, not endorsed by or affiliated with the makers of
Grim Dawn.
