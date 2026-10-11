@echo off
rem build_test_layout.bat - the layout model, offline. NO GAME NEEDED.
rem   tools\build_test_layout.bat        (finds vcvars64 itself, like build.bat)
rem
rem It compiles the REAL header the mod compiles - src\ut_layout.h - so the grid, pad and label
rem arithmetic it proves is byte for byte what the game runs. The grid half reads the G lines of
rem data\oracle\uniq-groups.txt; without the fixtures that half says SKIPPED.
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
   /Fo"%OUT%\\" /Fe"%OUT%\test_layout.exe" ^
   "%ROOT%\tools\test_layout.cpp"
if errorlevel 1 (echo [test] BUILD FAILED & exit /b 1)

"%OUT%\test_layout.exe" "%ROOT%"
exit /b %ERRORLEVEL%
