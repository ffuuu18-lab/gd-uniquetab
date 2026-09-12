The export-name dumps of Game.dll and Engine.dll are not published with the repository: they are
tables copied out of the game's own binaries. gen_exports_header.py and gen_reagent_header.py read
them from this folder (or from --exports <folder>) when src\gd_exports*.h has to be regenerated;
the committed headers are complete, so nothing here is needed to build the mod.

To recreate the two files from your own copy of Grim Dawn 1.3.0.8 x64, from a Visual Studio
developer prompt (dumpbin ships with the MSVC Build Tools):

    dumpbin /exports "<game>\x64\Game.dll"   > tools\exports\Game-x64-exports.txt
    dumpbin /exports "<game>\x64\Engine.dll" > tools\exports\Engine-x64-exports.txt

The generators look each name up by its exact decorated spelling in the dump, so the plain
dumpbin listing is the expected form.
