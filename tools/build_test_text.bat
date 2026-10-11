@echo off
rem build_test_text.bat - the text engine's pure half, offline (src\ut_textmeasure.h): the pen rule
rem over a glyph cache against utTextWidthPx (the captions, the label lines, the seven UI scales,
rem random strings), the cross-check, UTF-8 for the wide call, the language font path.
rem NO GAME IS LAUNCHED; writes only under build\test.
rem   tools\build_test_text.bat        (finds vcvars64 itself, like build.bat)
rem
rem Optional, READ ONLY: UNIQUETAB_TEST_FONTS_ARC (a game's resources\Fonts.arc), else
rem UNIQUETAB_TEST_GAME_DIR\resources\Fonts.arc, else UNIQUETAB_TEST_DBMODS_DIR\..\resources\
rem Fonts.arc (the game that mods\ folder belongs to) - the font-file half runs the mod's per-world
rem cross-check on the real root font and measures Cyrillic with ru\savapromedium.fnt.
setlocal
set "ROOT=%~dp0.."
set "OUT=%ROOT%\build\test"
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OUT%\text" mkdir "%OUT%\text"

if "%VSCMD_ARG_TGT_ARCH%"=="x64" goto :have_env
call "%ROOT%\tools\find_vcvars.bat"
if errorlevel 1 (echo [test] no x64 C++ toolchain - see the message above & exit /b 1)
call "%VCVARS%" >nul
:have_env

cl /nologo /W4 /WX /EHsc /std:c++17 /GR- /MD /DNDEBUG /D_CRT_SECURE_NO_WARNINGS ^
   /I"%ROOT%\src" /Fo"%OUT%\text\\" /Fe"%OUT%\test_text.exe" ^
   "%ROOT%\tools\test_text.cpp" "%ROOT%\src\gen\arc_reader.cpp" "%ROOT%\src\gen\arz_reader.cpp"
if errorlevel 1 (echo [test] BUILD FAILED & exit /b 1)

"%OUT%\test_text.exe" "%ROOT%\src\ut_textmeasure.h"
exit /b %ERRORLEVEL%
