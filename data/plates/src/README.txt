The inputs of tools\make_plates.py are not published: the three caravan textures are the game's
own art, copied from resources\UI.arc unchanged, and the PNGs are renders of them. The seven
generated plates in data\plates\ are committed, so nothing here is needed to build or package the
mod - only to regenerate the plates.

To recreate the inputs from your own copy of Grim Dawn, extract these three entries of
<game>\resources\UI.arc into this folder with the shipped ARC reader (tools\arc.py, see its
docstring for the extraction call):

    ui/caravan/caravan_transfercoverimage.tex
    ui/caravan/caravan_transfercomponent1_bg.tex
    ui/caravan/caravan_transfercomponent2_bg.tex

make_plates.py then paints the plate for every cell size over them and writes data\plates\.
