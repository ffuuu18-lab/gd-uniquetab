@echo off
rem build_test_buttons.bat - the buttons file, offline (src\ut_buttons.h): the grammar and every
rem refusal, buttons -> groups, [unassigned] and its byte-preserving rewrite, the generated file
rem against the Python twin's fixtures (data\oracle and its views) and the identity gates, the
rem fold of an old uniquetab-text.ini, the font that follows the letters; and the words it shares
rem (src\ut_textcfg.h): the label templates against 1.1.0's printf output, the fallback chain with
rem its glyph and tag callbacks, the UTF-8 helper. NO GAME NEEDED; writes only under build\test.
rem   tools\build_test_buttons.bat        (finds vcvars64 itself, like build.bat)
rem
rem It compiles the REAL header the mod will compile - src\ut_buttons.h, ut_textcfg.h - and checks the built-in
rem table against src\gen\ut_groups.h; the label equivalence uses src\ut_layout.h's pen rule.
setlocal
set "ROOT=%~dp0.."
set "OUT=%ROOT%\build\test"
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OUT%\buttons" mkdir "%OUT%\buttons"

if "%VSCMD_ARG_TGT_ARCH%"=="x64" goto :have_env
call "%ROOT%\tools\find_vcvars.bat"
if errorlevel 1 (echo [test] no x64 C++ toolchain - see the message above & exit /b 1)
call "%VCVARS%" >nul
:have_env

cl /nologo /W4 /WX /EHsc /std:c++17 /GR- /MD /DNDEBUG /D_CRT_SECURE_NO_WARNINGS ^
   /I"%ROOT%\src" /Fo"%OUT%\\" /Fe"%OUT%\test_buttons.exe" ^
   "%ROOT%\tools\test_buttons.cpp"
if errorlevel 1 (echo [test] BUILD FAILED & exit /b 1)

"%OUT%\test_buttons.exe" "%OUT%\buttons" "%ROOT%\src\ut_textcfg.h" "%ROOT%\src\ut_buttons.h" "%ROOT%"
exit /b %ERRORLEVEL%
