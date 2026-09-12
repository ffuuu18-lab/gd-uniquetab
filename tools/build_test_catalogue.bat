@echo off
rem build_test_catalogue.bat - the generator (src\gen) offline. Reads the game folder named by
rem UNIQUETAB_TEST_GAME_DIR, writes only under build\test. NO GAME is launched.
rem
rem pass 1: the archive readers against the Python reference (tools\dump_gen_sample.py):
rem         counts and a deterministic sample of 200 decoded records + 200 text tags, diffed.
rem pass 2: catalogue.bin generated from the game folder, byte for byte against data\oracle\catalogue.bin.
rem pass 3: the three uniq txt files against data\oracle, and the capacity check on uniq-pages.arz.
rem pass 4: the DLL's start-up path (stamp, temporary names, rename, the error paths) against a
rem         scratch mod folder under build\test\gen-mod.
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
   "%ROOT%\src\gen\catalogue_gen.cpp" "%ROOT%\src\gen\pages_gen.cpp" "%ROOT%\src\gen\generate.cpp" ^
   "%ROOT%\src\model\catalogue.cpp"
if errorlevel 1 (echo [test] BUILD FAILED & exit /b 1)

set RC=0

rem ---- pass 1: readers vs the Python reference -----------------------------------------------
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
"%OUT%\test_catalogue.exe" --catalogue "%ROOT%\data\oracle\catalogue.bin" "%OUT%\gen-catalogue.bin"
if errorlevel 1 set RC=1

rem ---- pass 3: the three uniq txt files vs the oracle ones, capacity vs the shipped arz -------
if not exist "%OUT%\gen-uniq" mkdir "%OUT%\gen-uniq"
"%OUT%\test_catalogue.exe" --pages "%ROOT%\data" "%OUT%\gen-uniq"
if errorlevel 1 set RC=1

rem ---- pass 4: the DLL's start-up path against a scratch mod folder ---------------------------
if not exist "%OUT%\gen-mod" mkdir "%OUT%\gen-mod"
"%OUT%\test_catalogue.exe" --ensure "%ROOT%\data" "%OUT%\gen-mod"
if errorlevel 1 set RC=1

if not "%RC%"=="0" (echo [test] FAILURES) else (echo [test] ALL PASS)
exit /b %RC%
