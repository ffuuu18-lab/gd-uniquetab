# Unique Collection Tab - changelog

Newest first.

## 1.2.1.1

Requires Grim Dawn 1.3.0.8 or 1.3.1.1, 64-bit, with Ultimate ASI Loader.

### Fixed

- **A caravan whose stash grid is wider than its materials cover (Loot Ascension's Baul UI) now
  gets the whole window.** The collection grid used to sit in the 346 x 625 corner the materials
  cover occupies, and the rest of Baul's window went unused. The grid now spans the stash grid's
  rectangle (929 x 861 of the 952 x 1007 window), a page holds up to 512 boxes, the black lattice
  background covers all of it, the layout editor works across the whole window, and the game's
  own Crafting Materials page keeps Baul's cover where it was. Nothing changes for the base game
  or for the mods whose cover is the page (X-Mod, Falendar's UI Overhaul, Reign of Terror,
  Grimarillion, DeathGrimDawn): their pages, plates and settings are the same as in 1.2.1. The
  pages are generated again once, at the first start after the update.
- For such a mod the `<W>x<H>` in the `[layout]` keys is the page's size now (`grid_941x936`,
  `pad_941x936`, `label_941x936` for Loot Ascension). A `grid_346x625` line saved with 1.2.1 stays
  in the file and is no longer read; open the editor again to place the grid on the whole window.

## 1.2.1

Requires Grim Dawn 1.3.0.8 or 1.3.1.1, 64-bit, with Ultimate ASI Loader.

### New

- **An EDIT button on the pad opens the layout editor** (`edit_button=0` under `[advanced]`
  removes it). It sits just before OWN, which stays the last, double-wide button; in the base game
  the pad keeps its 10 x 3 cells and its caption size, and only OWN moves one cell along. On the
  game's own materials page EDIT first goes back to the last collection group you had open, then
  the editor opens there. Ctrl+right-click (`editor_open` under `[keys]`, below) and
  `layout_edit=1` still open the editor, with or without the button. Its word is `edit.caption`
  under `[tab]` of the buttons file.
- **Bigger item icons on the collection pages: `zoom_pct`** under `[layout]`, 100 to 200 (a
  commented line at 100). `zoom_pct=150` draws every box one and a half times as big, the game's
  own icon magnified - in the base game Helms in 96-pixel cells, 3 x 5 to a screen instead of
  5 x 8 -; hovering, the tooltip, picking up and the corner badges follow the bigger boxes, the
  frames under them are painted again for the new size, and the game's own materials page keeps
  its normal size. Live within a second, per settings file (a custom game has its own value); a
  group your own grid cannot hold at that size is shown at the largest size that fits. 200 is
  twice the size and visibly soft. The game's own interface scale enlarges everything, the tab
  included, but shows no more items per screen.
- **Your own keys: `[keys]`** in the main `uniquetab.ini`, between `[layout]` and `[advanced]`:
  `rows_up`, `rows_down`, `button_prev`, `button_next`, `rows_wheel`, `button_wheel` and
  `editor_open`, each a commented line at the key it always was (PageUp, PageDown, Ctrl+PageUp,
  Ctrl+PageDown, Wheel, Ctrl+Wheel, Ctrl+RightClick). Write Ctrl, Shift or Alt and a key joined by
  `+` (`button_next=Ctrl+F3`, `rows_wheel=Alt+Wheel`), MiddleClick, Mouse4 or Mouse5 for a step, a
  key for `editor_open` (it then opens the editor as EDIT does), or `none`. Live within a second;
  read from the main file only and in force in every game - a `[keys]` line in a custom game's own
  file is ignored with one line in the log. A letter, a digit, Space, Enter, Tab, Backspace or a
  number-pad key needs Ctrl or Alt (the stash search box gets it too); Esc, Alt+F4, Alt+Tab,
  Alt+Space and LeftClick are refused, and a line that does not read keeps its default with one
  line in the log. The game still sees every key, so a key the game binds itself does both. With
  the defaults the keys, the wheel and the click behave exactly as before.

### Fixed

- **The Lokarr set could not be stored.** Lokarr's Gaze, Coat, Mantle and Boots keep their records
  outside the game's item folder, and the collection skipped everything there in the base game, so
  the four had no box and a dropped piece could not be deposited. The base game's records are now
  judged the way a custom game's always were - anywhere in the database, except the test records,
  the gear monsters and townsfolk wear and the designers' blank templates - so these items are now
  on their pages: the Lokarr set (Helms, Chest, Shoulders, Boots; its icons come from the game's
  `Level Art.arc`), Wilhelm's Wondrous Wargem (Amulets), Gazer Man (Chest), the Totally Normal
  Shield (One-Handed Maces, its three versions) and, with `collect_mi=1`, the quest rewards
  Sister's Amulet of Lifegiving (Amulets) and Slith Primal Ring (Rings), one box per difficulty's
  record. Leovinus' Ring stays on Rings. The base game has 197 pages with `collect_mi=1` (109
  without). Reign of Terror and Grimarillion gain the same fifteen; DeathGrimDawn, whose own
  database already had the Lokarr set and the Wargem, gains the other ten, and its Lokarr set -
  storable there but on no page until now - is shown too. In Reign of Terror, Merman's Sprocket
  (it uses the Lokarr boots' icon) is on the Boots pages now too.
- **A collection of more than 4,096 records was counted and shown only up to 4,096**, and which
  records depended on the order of the journal's lines: the label stopped at `coll 4096/...`, and
  the records past the first 4,096 lines of `uniq-items.jsonl` were left out of the owned-only
  filter and the search; a long session also stopped drawing owned boxes as owned after 4,096 of
  them. Every bound is sized from the data now - from your collection file and from the game's
  own pages -, so a full collection of any game, Grimarillion's 19,089 records included, is
  counted and shown whole, in any order. Nothing in your collection file changes.
- **Opening the caravan with a very large collection no longer hitches.** The journal's lookups
  are indexed: each opening looked every row of `uniq-items.jsonl` up by searching the whole file
  for it, which took about half a second with the base game's full collection and several seconds
  at 20,000 records, and reading the file at start took as long again. Both take milliseconds now.
  Nothing in your collection file changes.

### Known

- A box that shows the game's grey checker (or another placeholder) instead of an item picture
  belongs to a mod's item record that names no icon, or names one the mod does not ship: the tab
  draws the icon the record names, as the game's own windows do, so the item looks the same in
  that mod's shops and in your inventory, and nothing in the tab can fix it.
- When the pages are generated again because `collect_mi`, `plate_ground` or your groups file
  changed, the log gives the reason as `the game was updated`; the regeneration is the expected
  one, only the words are wrong.
- "In your collection (another roll)" compares the prefix, the suffix and the seed only: the same
  roll with another component or augment reads "In your collection".

### Upgrading

Nothing to do - the pages regenerate once at the next start (the log gives the reason: the mod was
updated). Your collection is untouched; on a page that gained an item, the boxes after it move one
place along. `uniquetab.ini` is rewritten once as `ini_version=44` with every value kept, gaining
the commented `edit_button` and `zoom_pct` lines and the commented `[keys]` section; a custom
game's own `mods\<name>\uniquetab.ini` keeps every line it has at its first entry, and a setting it
does not set keeps its default.

## 1.2.0

Requires Grim Dawn 1.3.0.8 or 1.3.1.1, 64-bit, with Ultimate ASI Loader.

### What's new

- **Every rare is collectable: monster infrequents, on by default.** Every Rare item the game gives
  a record of its own - the monster infrequents, and the faction stores' and the blacksmith's
  rares alike: 2,909 in the base game - is collected on its slot's page, after the uniques, in the
  same tabs as the other items. One box per item record (each level tier its own box), one roll
  per box: the box keeps the roll you put in exactly - prefix, suffix and numbers -, a second roll
  of the same item is refused and the log names the one you already have, and hovering another
  roll of an MI you hold reads "In your collection (another roll)" (its words are `tooltip.roll`
  under `[tab]` of the buttons file). The base game has 196 pages now (108 without them), Reign of
  Terror 109, DeathGrimDawn 282 with a new Artifacts page of 12 crystal weapons, Grimarillion 133.
- **One switch for it: `collect_mi`.** `collect_mi=1` (the default) collects them; `collect_mi=0`
  takes them off the pages, and the ones you stored stay in the collection, hidden until it is 1
  again. Either way the pages are generated again once, at the next start (in a custom game's own
  settings file: that game's pages only, at its next entry).
- **`+N not on these pages`.** The label ends in `+N not on these pages` while the collection
  holds items no page of the game you are in shows: items that game's database does not have,
  items no group of that game takes, and the MIs you stored while `collect_mi=0` hides them.
  Nothing you deposited ever leaves the collection.
- **Database mods are collectible.** Reign of Terror, DeathGrimDawn and other custom games that
  add items (GrimArmory, Grimarillion ...) get their own catalogue: their uniques, sets,
  runewords, bows, gems, runes, jewels and augments get pages and buttons, with the mod's own
  names and icons. Each custom game keeps its own collection and its own log. Everything the
  mod keeps for a custom game is in one folder, `x64\uniquetab\mods\<name>\` (the name of the
  game's own folder under `mods\`), with the `/basemods` variant's generated files in
  `mods\<name>\basemods\`.
- **Blueprints have a page.** Every blueprint - all four rarities, 914 in the base game on 12
  pages of 80, and a custom game's own too (Reign of Terror 1,425 with its runeword blueprints,
  DeathGrimDawn 1,805) - is collected on the new Blueprints page (BLU, after Relics), shown with
  the scroll the game draws for it: put one in, take it back, learn it after taking it. Crate's
  own test blueprints are left out.
- **Your buttons.** `uniquetab-buttons.ini`, one per game, is written with that game's buttons
  the first time you open the tab there. Rename a button, give it a caption, size or font, put
  several groups on one button (all of their items on one page, in one grid, with one label and
  one scroll) or leave a group off the pad - the `[unassigned]` section shows what no button
  holds. The label line and the tooltip words live there too. Ctrl+wheel and Ctrl+PageUp /
  Ctrl+PageDown walk the buttons in the order the pad shows them.
- **Every alphabet draws.** The tab is English in every language of the game until your buttons
  file says otherwise, and captions and names in any alphabet draw without a font setting:
  Latin, Cyrillic and Greek in the game's own fonts, everything else in the game's Chinese,
  Japanese or Korean font (chosen by the letters), with Windows' own font filling the letters none
  of them has. A font line still wins. Not drawn: right-to-left order, joined letters, and anything
  above U+FFFF (emoji) - such a caption shows the button's tag, such a name the group's own name.
- **Your own groups.** The groups are built in. An optional `uniquetab-groups.ini` adds pages of
  your own in front of them.
- **Groups named by the game.** A group is called what the game calls its items: in Reign of
  Terror every staff-class unique is a "Bow", so Bows is one page of 76 (Memory, Mist and Leaf
  included), and its javelins are Hunting Spears. DeathGrimDawn keeps Bows & Crossbows, Gems &
  Runes and Augments.
- **Move and resize the tab in the game.** Ctrl+right-click the button pad to open the layout
  editor: drag the item grid, the buttons and the label line, SAVE, CANCEL or RESET - or type
  the rectangles under `[layout]` yourself (`layout_outline=1` shows them on the page). The
  editor works on both sides of the window: the buttons and the label line move between the
  caravan and the screen in both directions, as often as you like in one edit. A grid of your
  own gets a frame under every box, the column at the window's very edge included, wherever the
  buttons and the label line are.
- **Buttons and label anywhere on the screen.** Drag the buttons or the label line off the
  caravan window in the editor (or set `pad_screen` / `label_screen`): they stay pinned to a
  screen corner, above the minimap and below the tooltips, and the buttons work there without
  moving your character - MAT included, which opens the game's own materials page from there as
  from the window.
- **Bigger pages on big caravans.** A page holds as many boxes as the caravan fits, up to 512:
  Reign of Terror's rings and amulets show 30 x 17 instead of 30 x 5, and your own grid shows
  every row it fits.
- **The materials page keeps all buttons.** On the game's own Crafting Materials page the whole
  button pad stays where you put it, drawn over the slots under it, and every button works; a
  click or a hover on the pad never reaches the slot under it.
- **A shorter settings file.** Four settings on top (`enabled`, `collect_mi` - the newest -,
  `tooltip_mark`, `log_level`); everything else is one commented line at the end - remove the `;`
  to change it. `max_per_record` and `rescue` are among those lines now, marked untested. Each
  custom game gets a complete settings file of its own, `mods\<name>\uniquetab.ini` - every
  setting the main file has but `journal_dir`, copied from the main file once when you first enter
  the game and independent of it afterwards (the layout editor, `log_level`, `collect_mi`, even
  `enabled` are per game) - and each game remembers where you left the tab.

### Fixed

- The collection line on a blueprint's tooltip described the item the blueprint crafts, so a
  blueprint read "In your collection" or "Not in your collection" according to that item. It now
  says whether you have the blueprint itself.
- Ctrl+PageUp / Ctrl+PageDown moved two buttons per press (and PageUp / PageDown two pages of
  rows); now one.
- The tab no longer dies for the whole session after leaving one custom game with a group open
  that the next one lacks; and when the tab is off in a world, the log now says why, once.
- The start-up log's policy line printed a garbage `reconcile=` value.

### Known

- A box that shows the game's grey checker (or another placeholder) instead of an item picture
  belongs to a mod's item record that names no icon, or names one the mod does not ship: the tab
  draws the icon the record names, as the game's own windows do, so the item looks the same in
  that mod's shops and in your inventory, and nothing in the tab can fix it.
- When the pages are generated again because `collect_mi`, `plate_ground` or your groups file
  changed, the log gives the reason as `the game was updated`; the regeneration is the expected
  one, only the words are wrong.
- "In your collection (another roll)" compares the prefix, the suffix and the seed only: the same
  roll with another component or augment reads "In your collection".

### Upgrading from 1.1.x

Close GD Stash if it is open on the mod's exports, replace `uniquetab.asi` and start the game.
There is nothing to do by hand; the first start does the rest, once:

- **The settings file** is rewritten as `ini_version` 42 with every value kept: four settings on
  top, the new `collect_mi=1` among them, and every other one a commented line under `[advanced]`
  at the end; `max_per_record` and `rescue` move there, marked untested (a value you changed stays
  set, as a live line there). The log lists every line it dropped: the rarely used switches such
  as `soulbound_collect`, `collect_quick_pass` and the display switches are always on now, and the
  pad geometry keys are fixed - the layout editor replaces them.
- **The custom games' folders** move from `views\` to `mods\` by themselves - the log says
  `files: the custom games' folders moved from views\ to mods\`. A folder whose new place is
  already taken, or that could not be moved because a file in it was held open (GD Stash), is
  left where it is with a log line saying why; a held one is moved at the next start (close the
  program and restart before you enter that game).
- **Every game's pages are generated again once**: the main campaign's at the first start, each
  custom game's at its first entry (the log says why - usually `the mod was updated`). After
  that, only a game update, a mod update, your groups file or a setting that changes the pages
  does it again.
- **Each custom game gets its own complete settings file**, `mods\<name>\uniquetab.ini`, at its
  first entry, with the main file's values as they are then.
- **The buttons file** `uniquetab-buttons.ini` is written the first time you open the tab in each
  game.

The move is one way: going back to 1.1.x afterwards needs the folders moved back by hand (README,
*When something goes wrong*).

### Uninstall

`undeploy.bat` in the repository, or by hand: remove `x64\uniquetab.asi`, `x64\uniquetab\` and
`settings\ui\caravan\uniq_plate_*.tex`. Keep `x64\uniquetab\uniq-items*.jsonl` and
`x64\uniquetab\mods\` if you want your collections; your settings, buttons and groups files are in
`x64\uniquetab\` (and each custom game's in `mods\<name>\`).

## 1.1.0

Requires Grim Dawn 1.3.0.8 or 1.3.1.1, 64-bit, with Ultimate ASI Loader.

### What's new

- **Stash-UI mods just work.** X-Mod, Falendar's UI Overhaul, Loot Ascension's Baul UI and mods like them that redraw the caravan: the collection pages are laid out for the window the mod draws, on a plain background inside its frame. Nothing to configure.
- **Custom games, each with its own collection.** A custom game that changes the caravan gets its pages built the first time you enter it (a few seconds on the loading screen, once), and keeps a collection of its own. Your main-campaign collection is never touched while you play one.
- **Works on 1.3.1.1 unchanged**, and items a game update adds show up on the first launch after it.
- **Lighter.** Building the catalogue now takes about 100 MB instead of holding whole game archives in memory, which matters on a Steam Deck.
- **The plugin is the whole package.** The page database and the page backgrounds are now generated on your machine from your own game files. The download is `uniquetab.asi` plus the notes.
- **New ini option `plate_ground`.** Six hex digits (`RRGGBB`) for the flat colour the collection pages use under a stash-UI mod. Empty means the vanilla near-black.
- Clearer log lines at start-up and after a game update.

### Upgrading from 1.0.x

Replace `uniquetab.asi` and start the game. Your collection, ini and exports stay where they are. The page database and backgrounds an older version installed are rebuilt on the first launch.

### Uninstall

`undeploy.bat` in the repository, or by hand: remove `x64\uniquetab.asi`, `x64\uniquetab\` and `settings\ui\caravan\uniq_plate_*.tex`. Move your collection out first if you want to keep it: `x64\uniquetab\uniq-items.jsonl` (and `-hc`), plus `x64\uniquetab\views\` for custom games.
