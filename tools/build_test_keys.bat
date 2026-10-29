@echo off
rem build_test_keys.bat - the [keys] section's grammar, refusals and matcher, offline. NO GAME NEEDED.
rem   tools\build_test_keys.bat        (finds vcvars64 itself, like build.bat)
rem
rem It compiles the REAL header the mod compiles - src\ut_keys.h - so every parse, refusal and
rem match it proves is what the settings file and the window procedure run. No inputs.
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
   /Fo"%OUT%\\" /Fe"%OUT%\test_keys.exe" ^
   "%ROOT%\tools\test_keys.cpp"
if errorlevel 1 (echo [test] BUILD FAILED & exit /b 1)

"%OUT%\test_keys.exe" "%ROOT%"
exit /b %ERRORLEVEL%
