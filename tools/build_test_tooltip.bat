@echo off
rem build_test_tooltip.bat - the borrowed N+1 swap, offline. NO GAME NEEDED.
rem   tools\build_test_tooltip.bat        (finds vcvars64 itself, like build.bat)
rem
rem It links the REAL src\ut_tooltip.cpp with a stubbed trampoline and stubbed journal/collection
rem probes, so the swap the test exercises is byte for byte the swap the game runs.
setlocal
set "ROOT=%~dp0.."
set "OUT=%ROOT%\build\test"
set "MINHOOK=%ROOT%\third_party\minhook"
if not exist "%OUT%" mkdir "%OUT%"

if "%VSCMD_ARG_TGT_ARCH%"=="x64" goto :have_env
call "%ROOT%\tools\find_vcvars.bat"
if errorlevel 1 (echo [test] no x64 C++ toolchain - see the message above & exit /b 1)
call "%VCVARS%" >nul
:have_env

cl /nologo /W4 /WX /EHsc /std:c++17 /GR- /MD /DNDEBUG /D_CRT_SECURE_NO_WARNINGS ^
   /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I"%MINHOOK%\include" /I"%ROOT%\src" ^
   /Fo"%OUT%\\" /Fe"%OUT%\test_tooltip.exe" ^
   "%ROOT%\tools\test_tooltip.cpp" "%ROOT%\src\ut_tooltip.cpp" "%ROOT%\src\ut_config.cpp"
if errorlevel 1 (echo [test] BUILD FAILED & exit /b 1)

"%OUT%\test_tooltip.exe"
exit /b %ERRORLEVEL%
