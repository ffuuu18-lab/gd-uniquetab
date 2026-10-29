Flag-off fixtures (collect_mi=0), not shipped, not published.

uniquetab.ini's collect_mi defaults to 1, so the fixtures one folder up (data\oracle\ and
data\oracle\views\<tag>\) are the FLAG-ON cut - the shipped default. This folder keeps the
FLAG-OFF cut so that path stays pinned:

    nomi\                        vanilla: catalogue.bin, uniq-records.txt, uniq-pages.txt,
                                 uniq-groups.txt, the default uniquetab-buttons.ini, and
                                 catalogue-stats.md (build_catalogue.py's census, flag-off only)
    nomi\views\reignofterror\    the same five for Reign of Terror
    nomi\views\deathgrimdawn\    the same five for DeathGrimDawn
    nomi\plates\                 uniq_plate_32x128.tex - the one vanilla plate only the flag-off
                                 view paints (its six unique spears are 32 wide; with the flag on
                                 the MI spears make Spears 64x128, so the shipped view paints six
                                 plates, data\oracle\plates)

They are cut by the same tools as the flag-on files one folder up, without --collect-mi, so a
change to the MI rule cannot move the flag-off result unnoticed.

tools\build_test_catalogue.bat pass 5 compares against them: test_catalogue --mi generates the
vanilla view with the flag OFF through the DLL's start-up path (the stamp case) and compares its
four files byte for byte; --mi-dbmod regenerates Reign of Terror and DeathGrimDawn with the flag
off and compares their four files. The not-shown case (a journal owning two MIs, the flag off)
checks that neither is in the catalogue and that the pages are nomi\uniq-groups.txt byte for byte.

THE RULE (src\gen\catalogue_gen.cpp isMiCandidate = tools\build_catalogue.py is_mi_candidate):
with collect_mi=1 every equipment record of the 25 equipment classes (the 23 slot classes and the
per-class groups' WeaponMagical_Staff and WeaponHunting_Spear, which only a mod has) whose own
itemClassification is Rare is collected, through the shipped filter (a mod layer's: the mod
rule) - the faction-store rares (vanilla's records/items/faction/) and the blacksmith-crafted ones
included. The per-game junk folders stay [exclude] blocks of the built-in groups
(src\gen\ut_groups_default.h; Grimarillion's Replica items among them). Each MI carries flag bit 8
(the KIND mark) and sorts after the uniques of its slot group. With collect_mi=0 no MI is
admitted; a stored one keeps its journal row and is on no page (there is no Retired group).

To cut them again, with GD_DIR naming the 1.3.0.8 folder (the flag-off cut needs no switch; the
Python tools' default stays off):

    python tools\build_catalogue.py --out data\oracle\nomi
    python tools\pack_catalogue.py  --in data\oracle\nomi\catalogue.json --out data\oracle\nomi\catalogue.bin
    python tools\build_uniq_db.py   --out data\oracle\nomi

    python tools\build_catalogue.py --custom <mods>\ReignOfTerror --out data\oracle\nomi\views\reignofterror
    python tools\pack_catalogue.py  --in data\oracle\nomi\views\reignofterror\catalogue.json
                                    --out data\oracle\nomi\views\reignofterror\catalogue.bin
    python tools\build_uniq_db.py   --custom <mods>\ReignOfTerror --out data\oracle\nomi\views\reignofterror

(DeathGrimDawn likewise), then delete catalogue.json, uniq-pages.arz, uniq-pages.json and src\
(regenerable intermediates). The flag-on cut is the same commands with --collect-mi on
build_catalogue.py and build_uniq_db.py and --out data\oracle (data\oracle\views\<tag>).

Counts (catalogue items / MIs / paged boxes / pages / groups), flag OFF -> ON:
    vanilla          4,401 / 0 / 4,211 / 109 / 25   ->  7,316 / 2,915 / 7,126 / 197 / 25
    Reign of Terror  7,291 / 0 / 7,043 /  73 / 29   -> 10,523 / 3,232 / 10,262 / 109 / 29
    DeathGrimDawn    8,755 / 0 / 7,328 / 180 / 29   -> 11,865 / 3,110 / 10,431 / 282 / 30
The four Lokarr pieces (records/storyelements/signs/) are paged in every cut: their icons are in
gdx1\resources\Level Art.arc, and the reference folder holds the Level Art.arc of resources\
(847,666,093 bytes) and of gdx1\resources\ (293,908,330 bytes); gdx2's and gdx3's are not part
of the reference (..\ORACLE.txt lists the folder). Reign of Terror's Merman's Sprocket
(records/items/d2items/gearfeet/d005_feet.dbr) names the Lokarr boots' icon and is paged with
them; with the flag off DeathGrimDawn's Helms take an 11th page (401 boxes), so its view has 180.
DeathGrimDawn's 30th group with the flag on is spear1h, "Artifacts" (ART): its default
uniquetab-buttons.ini gains [ART]; vanilla's and Reign of Terror's are the same with or without.
