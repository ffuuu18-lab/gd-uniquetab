@echo off
rem build_test_catalogue.bat - the generator (src\gen) offline. Reads the game folder named by
rem UNIQUETAB_TEST_GAME_DIR, writes only under build\test. NO GAME is launched.
rem
rem pass 1: the archive readers against the Python reference (tools\dump_gen_sample.py):
rem         counts and a deterministic sample of 200 decoded records + 200 text tags, diffed.
rem pass 2: catalogue.bin generated from the game folder, byte for byte against data\oracle\catalogue.bin
rem         (the shipped default, collect_mi=1; the flag-off cut is data\oracle\nomi, pass 5).
rem pass 3: the three uniq txt files against data\oracle, and the generated overlay read back.
rem pass 3b: that overlay against the oracle data\oracle\uniq-pages.arz, record for record.
rem pass 5: monster infrequents - the vanilla counts and order (collect_mi=1, data\oracle), the not-shown
rem         case (no Retired group), the stamp and the flag-off cut (data\oracle\nomi); the database mods' views with the
rem         flag on (the budget) and off (ROT, DGD against data\oracle\nomi\views\<tag>).
rem pass 4: the DLL's start-up path (a fresh install, the stamp, temporary names, rename, the
rem         error paths) against a
rem         scratch mod folder under build\test\gen-mod.
rem Every pass starts from an EMPTY scratch folder (:fresh at the end removes it and makes it again;
rem the files passes 1 and 2 compare are deleted first), so a file an earlier run left behind - a
rem flag-off plate, say - can never be counted or compared by this run.
rem LINE ENDS: this file must be CRLF (.gitattributes checks every .bat out so). With LF-only
rem line ends cmd resumes after a CALL :fresh at the wrong offset: the database-mod FOR blocks
rem below ran as loose lines ('dbmod custom--m', '%%M: SKIPPED') and the passes after them ran a
rem second time after the first ALL PASS. Each dbmod / mi-dbmod pass prints its own lines naming
rem custom-reignofterror and custom-deathgrimdawn exactly once; ALL PASS is printed once.
setlocal
set "ROOT=%~dp0.."
set "OUT=%ROOT%\build\test"
if not exist "%OUT%" mkdir "%OUT%"

rem (the game path holds parentheses, so no parenthesised block names it)
if defined UNIQUETAB_TEST_GAME_DIR goto :have_dir
echo [test] set UNIQUETAB_TEST_GAME_DIR to the Grim Dawn folder ^(read only^)
exit /b 1
:have_dir
if exist "%UNIQUETAB_TEST_GAME_DIR%\database\database.arz" goto :have_arz
echo [test] the game folder has no database\database.arz
exit /b 1
:have_arz

if "%VSCMD_ARG_TGT_ARCH%"=="x64" goto :have_env
call "%ROOT%\tools\find_vcvars.bat"
if errorlevel 1 (echo [test] no x64 C++ toolchain - see the message above & exit /b 1)
call "%VCVARS%" >nul
:have_env

cl /nologo /W4 /WX /EHsc /O2 /std:c++17 /GR- /MD /DNDEBUG /D_CRT_SECURE_NO_WARNINGS ^
   /I"%ROOT%\src" /Fo"%OUT%\\" /Fe"%OUT%\test_catalogue.exe" ^
   "%ROOT%\tools\test_catalogue.cpp" "%ROOT%\src\gen\arz_reader.cpp" "%ROOT%\src\gen\arc_reader.cpp" ^
   "%ROOT%\src\gen\catalogue_gen.cpp" "%ROOT%\src\gen\pages_gen.cpp" "%ROOT%\src\gen\arz_writer.cpp" "%ROOT%\src\gen\ui_geometry.cpp" "%ROOT%\src\gen\plate_gen.cpp" "%ROOT%\src\gen\generate.cpp" "%ROOT%\src\gen\heap_meter.cpp" ^
   "%ROOT%\src\model\catalogue.cpp" psapi.lib
if errorlevel 1 (echo [test] BUILD FAILED & exit /b 1)

set RC=0

rem ---- pass 1: readers vs the Python reference -----------------------------------------------
if exist "%OUT%\gen-sample-cpp.txt" del /q "%OUT%\gen-sample-cpp.txt"
if exist "%OUT%\gen-sample-py.txt" del /q "%OUT%\gen-sample-py.txt"
"%OUT%\test_catalogue.exe" --readers "%OUT%\gen-sample-cpp.txt"
if errorlevel 1 set RC=1
set "GD_DIR=%UNIQUETAB_TEST_GAME_DIR%"
python "%ROOT%\tools\dump_gen_sample.py" "%OUT%\gen-sample-py.txt"
if errorlevel 1 (echo [test] the Python reference dump failed & set RC=1)
fc /b "%OUT%\gen-sample-cpp.txt" "%OUT%\gen-sample-py.txt" >nul
if errorlevel 1 (
  echo [test] FAIL: the reader sample differs from the Python reference
  fc "%OUT%\gen-sample-cpp.txt" "%OUT%\gen-sample-py.txt" | findstr /n "^" | findstr /r "^[0-9]:\|^[1-3][0-9]:"
  set RC=1
) else (
  echo [test] readers: sample identical to the Python reference
)

rem ---- pass 2: catalogue.bin vs the oracle file ------------------------------------------------
if exist "%OUT%\gen-catalogue.bin" del /q "%OUT%\gen-catalogue.bin"
"%OUT%\test_catalogue.exe" --catalogue "%ROOT%\data\oracle\catalogue.bin" "%OUT%\gen-catalogue.bin"
if errorlevel 1 set RC=1

rem ---- pass 2b: the derived geometry ---------------------------------------------------------
"%OUT%\test_catalogue.exe" --geometry
if errorlevel 1 set RC=1

rem ---- pass 2c: the painted cover plates ------------------------------------------------------
call :fresh "%OUT%\gen-plates"
"%OUT%\test_catalogue.exe" --plates "%ROOT%\data" "%OUT%\gen-plates"
if errorlevel 1 set RC=1

rem ---- pass 2d: a user grid's own plates (layout plates) vs tools\make_plates.py --grid --------
call :fresh "%OUT%\gen-layout"
"%OUT%\test_catalogue.exe" --layout-plates "%ROOT%\data" "%OUT%\gen-layout"
if errorlevel 1 set RC=1
set "LH="
if exist "%OUT%\gen-layout\layout-hash.txt" set /p LH=<"%OUT%\gen-layout\layout-hash.txt"
if not defined LH (echo [test] FAIL: layout plates: no set to compare & set RC=1 & goto :layout_done)
set "GD_DIR=%UNIQUETAB_TEST_GAME_DIR%"
python "%ROOT%\tools\make_plates.py" --grid 105,120,333,460 --layout-hash %LH% --out "%OUT%\gen-layout\py" > "%OUT%\gen-layout\py.log" 2>&1
if errorlevel 1 (echo [test] FAIL: layout plates: tools\make_plates.py --grid failed & set RC=1 & goto :layout_done)
"%OUT%\test_catalogue.exe" --layout-compare "%OUT%\gen-layout\plateroot" "%OUT%\gen-layout\py" "%OUT%\gen-layout\layout-names.txt"
if errorlevel 1 set RC=1
rem zoom_pct=150 under the derived grid (a layout of its own): make_plates.py --grid --zoom, byte for byte
set "LZ="
if exist "%OUT%\gen-layout\layout-zoom-hash.txt" set /p LZ=<"%OUT%\gen-layout\layout-zoom-hash.txt"
if not defined LZ (echo [test] FAIL: layout plates: no zoom set to compare & set RC=1 & goto :zoom_done)
python "%ROOT%\tools\make_plates.py" --grid 105,76,333,552 --zoom 150 --layout-hash %LZ% --out "%OUT%\gen-layout\py-zoom" > "%OUT%\gen-layout\py-zoom.log" 2>&1
if errorlevel 1 (echo [test] FAIL: layout plates: tools\make_plates.py --grid --zoom failed & set RC=1 & goto :zoom_done)
"%OUT%\test_catalogue.exe" --layout-compare "%OUT%\gen-layout\plateroot-zoom" "%OUT%\gen-layout\py-zoom" "%OUT%\gen-layout\layout-zoom-names.txt"
if errorlevel 1 set RC=1
:zoom_done
rem the wide cover too (X-Mod 975x968, a flat ground): the same comparison
set "LX="
if exist "%OUT%\gen-layout\layout-975-hash.txt" set /p LX=<"%OUT%\gen-layout\layout-975-hash.txt"
if not defined LX (echo [test] layout plates: X-Mod comparison SKIPPED & goto :layout_done)
python "%ROOT%\tools\make_plates.py" --custom "%UNIQUETAB_TEST_MODS_DIR%\xmod\X-Mod" --plate-tag x-mod --groups "%OUT%\gen-layout\view-975\uniq-groups.txt" --grid 591,76,384,584 --layout-hash %LX% --out "%OUT%\gen-layout\py-975" > "%OUT%\gen-layout\py-975.log" 2>&1
if errorlevel 1 (echo [test] FAIL: layout plates: tools\make_plates.py --grid on X-Mod failed & set RC=1 & goto :layout_done)
"%OUT%\test_catalogue.exe" --layout-compare "%OUT%\gen-layout\plateroot" "%OUT%\gen-layout\py-975" "%OUT%\gen-layout\layout-975-names.txt"
if errorlevel 1 set RC=1
:layout_done

rem ---- pass 3: the three uniq txt files vs the oracle ones, and the overlay read back --------
call :fresh "%OUT%\gen-uniq"
"%OUT%\test_catalogue.exe" --pages "%ROOT%\data" "%OUT%\gen-uniq"
if errorlevel 1 set RC=1

rem ---- pass 3b: the generated overlay vs the oracle uniq-pages.arz, record for record --------
"%OUT%\test_catalogue.exe" --overlay "%ROOT%\data" "%OUT%\gen-uniq"
if errorlevel 1 set RC=1

rem ---- pass 3c: a custom game's own view ------------------------------------------------------
call :fresh "%OUT%\gen-view"
"%OUT%\test_catalogue.exe" --view "%OUT%\gen-view"
if errorlevel 1 set RC=1

rem ---- pass 3d: database mods - the catalogue from the view, against data\oracle\views\<tag> ---
rem UNIQUETAB_TEST_DBMODS_DIR names a folder holding ReignOfTerror and/or DeathGrimDawn (an
rem installed game's mods folder; read only). Unset: the pass says SKIPPED.
if not defined UNIQUETAB_TEST_DBMODS_DIR goto :no_dbmods
for %%M in (ReignOfTerror DeathGrimDawn) do (
  if exist "%UNIQUETAB_TEST_DBMODS_DIR%\%%M\database" (
    call :fresh "%OUT%\gen-dbmod-%%M"
    "%OUT%\test_catalogue.exe" --dbmod "%UNIQUETAB_TEST_DBMODS_DIR%\%%M" "%ROOT%\data" "%OUT%\gen-dbmod-%%M"
    if errorlevel 1 set RC=1
  ) else (
    echo [test] dbmod %%M: SKIPPED - not in UNIQUETAB_TEST_DBMODS_DIR
  )
)
goto :dbmods_done
:no_dbmods
echo [test] dbmod: SKIPPED - UNIQUETAB_TEST_DBMODS_DIR is not set
:dbmods_done

rem ---- pass 5: monster infrequents - the vanilla flag-on cut (data\oracle) counted and ordered, the
rem      not-shown case with a synthetic journal, the flag in the stamp, the flag-off four files against
rem      data\oracle\nomi; then the database mods with the flag on (the own-heap budget, the flag-on
rem      cut) and off (ROT and DGD against data\oracle\nomi\views\<tag>)
call :fresh "%OUT%\gen-mi"
"%OUT%\test_catalogue.exe" --mi "%ROOT%\data" "%OUT%\gen-mi"
if errorlevel 1 set RC=1
if not defined UNIQUETAB_TEST_DBMODS_DIR goto :no_mi_dbmods
for %%M in (ReignOfTerror DeathGrimDawn grimarillion) do (
  if exist "%UNIQUETAB_TEST_DBMODS_DIR%\%%M\database" (
    call :fresh "%OUT%\gen-mi-%%M"
    "%OUT%\test_catalogue.exe" --mi-dbmod "%UNIQUETAB_TEST_DBMODS_DIR%\%%M" "%ROOT%\data" "%OUT%\gen-mi-%%M"
    if errorlevel 1 set RC=1
  ) else (
    echo [test] mi-dbmod %%M: SKIPPED - not in UNIQUETAB_TEST_DBMODS_DIR
  )
)
goto :mi_dbmods_done
:no_mi_dbmods
echo [test] mi-dbmod: SKIPPED - UNIQUETAB_TEST_DBMODS_DIR is not set
:mi_dbmods_done

rem ---- pass 4: the DLL's start-up path against a scratch mod folder ---------------------------
call :fresh "%OUT%\gen-mod"
"%OUT%\test_catalogue.exe" --ensure "%ROOT%\data" "%OUT%\gen-mod"
if errorlevel 1 set RC=1

if not "%RC%"=="0" (echo [test] FAILURES) else (echo [test] ALL PASS)
exit /b %RC%

rem %1 = a scratch folder under build\test: removed with everything in it, then made again, empty
:fresh
if exist "%~1" rmdir /s /q "%~1"
mkdir "%~1"
exit /b 0
