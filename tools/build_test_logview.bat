@echo off
rem build_test_logview.bat - the per-view log (src\ut_log.cpp, the real logger) and the custom-game
rem view probe (src\ut_viewprobe.h), offline. NO GAME RUNNING; writes only under build\test and a
rem scratch folder in %%TEMP%% that a passing run removes.
rem   tools\build_test_logview.bat       (finds vcvars64 itself, like build.bat)
rem
rem   UNIQUETAB_TEST_DBMODS_DIR   the game's mods folder, READ ONLY: every custom game in it is
rem                               probed (the Crucible must get no view); unset = skipped
rem   UNIQUETAB_TEST_GAME_DIR     the game folder, READ ONLY: the probe is checked against the
rem                               catalogue's real rule; unset = skipped
rem   UNIQUETAB_TEST_EXTRA_MODS   more custom-game folders, ';'-separated (optional)
setlocal
set "ROOT=%~dp0.."
set "OUT=%ROOT%\build\test"
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OUT%\logview" mkdir "%OUT%\logview"

if "%VSCMD_ARG_TGT_ARCH%"=="x64" goto :have_env
call "%ROOT%\tools\find_vcvars.bat"
if errorlevel 1 (echo [test] no x64 C++ toolchain - see the message above & exit /b 1)
call "%VCVARS%" >nul
:have_env

cl /nologo /W4 /WX /EHsc /O2 /std:c++17 /GR- /MD /DNDEBUG /D_CRT_SECURE_NO_WARNINGS ^
   /I"%ROOT%\src" /Fo"%OUT%\logview\\" /Fe"%OUT%\test_logview.exe" ^
   "%ROOT%\tools\test_logview.cpp" "%ROOT%\src\ut_log.cpp" ^
   "%ROOT%\src\gen\arz_reader.cpp" "%ROOT%\src\gen\arc_reader.cpp" "%ROOT%\src\gen\arz_writer.cpp" ^
   "%ROOT%\src\gen\catalogue_gen.cpp" "%ROOT%\src\gen\pages_gen.cpp" "%ROOT%\src\gen\ui_geometry.cpp" ^
   "%ROOT%\src\gen\plate_gen.cpp" "%ROOT%\src\gen\generate.cpp" "%ROOT%\src\gen\heap_meter.cpp" ^
   "%ROOT%\src\model\catalogue.cpp" psapi.lib
if errorlevel 1 (echo [test] BUILD FAILED & exit /b 1)

rem The transcript is kept, so a failure can be read afterwards instead of re-run.
"%OUT%\test_logview.exe" > "%OUT%\test_logview.out.txt" 2>&1
set "RC=%ERRORLEVEL%"
type "%OUT%\test_logview.out.txt"
if not "%RC%"=="0" echo [test] the transcript is in "%OUT%\test_logview.out.txt"
exit /b %RC%
