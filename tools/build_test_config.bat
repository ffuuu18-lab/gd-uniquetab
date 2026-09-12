@echo off
rem build_test_config.bat - the ini reader and writer, and the path rules, offline. NO GAME NEEDED.
rem   tools\build_test_config.bat        (finds vcvars64 itself, like build.bat)
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
   /Fo"%OUT%\\" /Fe"%OUT%\test_config.exe" ^
   "%ROOT%\tools\test_config.cpp" "%ROOT%\src\ut_config.cpp" "%ROOT%\src\ut_paths.cpp"
if errorlevel 1 (echo [test] BUILD FAILED & exit /b 1)

rem The transcript is kept, so a failure that only shows on a loaded machine can be read
rem afterwards instead of re-run.
"%OUT%\test_config.exe" > "%OUT%\test_config.out.txt" 2>&1
set "RC=%ERRORLEVEL%"
type "%OUT%\test_config.out.txt"
if not "%RC%"=="0" echo [test] the transcript is in "%OUT%\test_config.out.txt"
exit /b %RC%
