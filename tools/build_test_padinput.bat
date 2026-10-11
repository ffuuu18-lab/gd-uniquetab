@echo off
rem build_test_padinput.bat - the screen pad's press rules, offline. NO GAME NEEDED.
rem   tools\build_test_padinput.bat        (finds vcvars64 itself, like build.bat)
rem
rem It compiles the REAL header the mod compiles - src\ut_padinput.h - so every rewrite, press and
rem hand-over it proves is what the GetMouseEvent detour runs. Optional, read only: with
rem UNIQUETAB_TEST_GAME_DIR or UNIQUETAB_TEST_DBMODS_DIR naming a game whose x64\DirectInput.dll is
rem there, the export, its stride and the device vtable's slots are checked in the file itself.
setlocal
set "ROOT=%~dp0.."
set "OUT=%ROOT%\build\test"
if not exist "%OUT%" mkdir "%OUT%"

if "%VSCMD_ARG_TGT_ARCH%"=="x64" goto :have_env
call "%ROOT%\tools\find_vcvars.bat"
if errorlevel 1 (echo [test] no x64 C++ toolchain - see the message above & exit /b 1)
call "%VCVARS%" >nul
:have_env

cl /nologo /W4 /WX /EHsc /std:c++17 /GR- /MD /DNDEBUG /D_CRT_SECURE_NO_WARNINGS ^
   /I"%ROOT%\src" ^
   /Fo"%OUT%\\" /Fe"%OUT%\test_padinput.exe" ^
   "%ROOT%\tools\test_padinput.cpp"
if errorlevel 1 (echo [test] BUILD FAILED & exit /b 1)

"%OUT%\test_padinput.exe" "%ROOT%"
exit /b %ERRORLEVEL%
