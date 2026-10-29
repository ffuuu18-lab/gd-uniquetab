Database-mod view fixtures, not shipped, not published.

One folder per custom game, named by its view tag (the mod folder's leaf in lower case):
catalogue.bin, uniq-records.txt, uniq-pages.txt and uniq-groups.txt as the Python reference
writes them for the view "custom-<tag>" - the base game's four databases with the mod's
database\*.arz on top, the mod's text_EN*.arc names over the base ones, the mod's icons first,
the built-in groups template (src\gen\ut_groups_default.h). tools\build_test_catalogue.bat's
pass 3d (test_catalogue --dbmod) generates the same view in C++ and compares the four files
byte for byte. Only the lists are kept; the overlay, its manifest and catalogue.json are
regenerable intermediates.

The base game is the 1.3.0.8 reference folder (UNIQUETAB_TEST_GAME_DIR; ..\ORACLE.txt lists what
it holds - among it the Level Art.arc of resources\ (847,666,093 bytes) and of gdx1\resources\
(293,908,330 bytes), where the Lokarr set's icons are; gdx2's and gdx3's Level Art.arc are not
part of the reference); the mod folders are an installed game's mods\ReignOfTerror and
mods\DeathGrimDawn (UNIQUETAB_TEST_DBMODS_DIR names the folder that holds them; read only). To cut one again, with GD_DIR naming the 1.3.0.8 folder:

    python tools\build_catalogue.py --custom <mods>\ReignOfTerror --out data\oracle\views\reignofterror
    python tools\pack_catalogue.py  --in data\oracle\views\reignofterror\catalogue.json
                                    --out data\oracle\views\reignofterror\catalogue.bin
    python tools\build_uniq_db.py   --custom <mods>\ReignOfTerror --out data\oracle\views\reignofterror

The groups the two per-class blocks (staff, spear1h) make are named by the view's own text
(build_uniq_db.py reads the base Text_EN.arc and the mod's text_en*.arc for the class words), so
the text archives are inputs too.

The cut is the FLAG-ON one (collect_mi=1 is the default: build_catalogue.py and
build_uniq_db.py with --collect-mi; every Rare equipment record of the view, faction-store and
crafted rares included). Every blueprint (ItemArtifactFormula) at every classification is
collected, on the built-in Blueprints page after Relics; a Quest-classified one is packed as
Epic, like a Magical one (Reign of Terror's 170 runeword-craft blueprints), and one with no
artifactFormulaBitmapName takes its rarity's scroll (no blueprint of either mod has an empty
one). Reign of Terror: 10,523 items (3,232 MIs), 10,262 paged on 109 pages, 29 groups, 1,425
blueprints on 6 pages (with the Quest-classified ones collected, no record its "cannot store"
warning names is a blueprint). DeathGrimDawn: 11,865 items (3,110 MIs), 10,431 paged on 282
pages, 30 groups, 1,805 blueprints on 23 pages - its 30th group, spear1h "Artifacts" (ART, the
12 Rare crystal weapons of WeaponHunting_Spear), exists only with the flag on, so its
uniquetab-buttons.ini has [ART]. The flag-off cuts are under data\oracle\nomi\views\<tag>\
(see data\oracle\nomi\README.txt).

Both views page the four Lokarr pieces (their icons are in gdx1's Level Art.arc), and Reign of
Terror also Merman's Sprocket (records/items/d2items/gearfeet/d005_feet.dbr), which names the
Lokarr boots' icon; each view keeps one record off its pages, as before (Reign of Terror's
Nadaan's Reach names an icon the mod does not ship).

A mod update changes these files by design: cut them again from the new mod and read the diff.
