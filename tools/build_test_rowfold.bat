@echo off
rem build_test_rowfold.bat - the paging arithmetic and the tier-C case fold, offline.
rem NO GAME NEEDED.
rem   tools\build_test_rowfold.bat        (finds vcvars64 itself, like build.bat)
rem
rem It compiles the REAL headers the mod compiles - src\ut_rowmath.h and src\ut_textfold.h - so
rem the row clamp the regression case replays and the fold tier C matches with are byte for byte
rem what the game runs. Nothing here touches Windows, the engine or a global.
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
   /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I"%ROOT%\src" ^
   /Fo"%OUT%\\" /Fe"%OUT%\test_rowfold.exe" ^
   "%ROOT%\tools\test_rowfold.cpp"
if errorlevel 1 (echo [test] BUILD FAILED & exit /b 1)

"%OUT%\test_rowfold.exe"
exit /b %ERRORLEVEL%
