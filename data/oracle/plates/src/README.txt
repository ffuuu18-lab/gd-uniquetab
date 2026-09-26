The inputs of tools\make_plates.py are not published: the three caravan textures are the game's
own art, copied from resources\UI.arc unchanged, and the PNGs are renders of them. They are only
needed to regenerate the oracle plates in data\oracle\plates\; the DLL paints its own set at run
time from the player's own UI.arc, and nothing of this folder ships.

To recreate the inputs from your own copy of Grim Dawn 1.3.0.8, extract these three entries of
<game>\resources\UI.arc into this folder with the shipped ARC reader (tools\arc.py, see its
docstring for the extraction call):

    ui/caravan/caravan_transfercoverimage.tex
    ui/caravan/caravan_transfercomponent1_bg.tex
    ui/caravan/caravan_transfercomponent2_bg.tex

make_plates.py then paints the plate for every cell size over them and writes data\oracle\plates\.
