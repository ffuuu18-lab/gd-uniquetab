@echo off
rem build_test_bindings.bat - THE BINDINGS, OFFLINE. NO GAME IS LAUNCHED.
rem Run it from anywhere:  tools\build_test_bindings.bat   (it finds vcvars64 itself)
rem
rem Two optional, READ-ONLY inputs; each half skips loudly without its own:
rem   UNIQUETAB_TEST_EXE_IMAGE   a DECRYPTED Grim Dawn.exe image (the shipped exe's .text is
rem                              Steam-DRM encrypted, so the file on disk cannot be scanned)
rem   UNIQUETAB_TEST_GAME_DIR    the installed game folder - Game.dll is opened READ ONLY
rem Neither path is ever written to, and nothing is copied out of either.
setlocal
set "ROOT=%~dp0.."
set "OUT=%ROOT%\build\test"
if not exist "%OUT%" mkdir "%OUT%"

if "%VSCMD_ARG_TGT_ARCH%"=="x64" goto :have_env
call "%ROOT%\tools\find_vcvars.bat"
if errorlevel 1 (echo [test] no x64 C++ toolchain - see the message above & exit /b 1)
call "%VCVARS%" >nul
:have_env

rem ut_bindings.cpp is linked in, not copied: the two converted deposit-site patterns the test
rem scans for are THE ONES THE MOD USES. The other five patterns are read out of the mod's own
rem sources at run time, which is why src is handed to the exe as its first argument.
cl /nologo /W4 /WX /EHsc /std:c++17 /GR- /MD /DNDEBUG /D_CRT_SECURE_NO_WARNINGS ^
   /Fo"%OUT%\\" /Fe"%OUT%\test_bindings.exe" ^
   "%ROOT%\tools\test_bindings.cpp" "%ROOT%\src\ut_bindings.cpp" "%ROOT%\src\ut_log.cpp" ^
   "%ROOT%\src\ut_paths.cpp"
if errorlevel 1 (echo [test] BUILD FAILED & exit /b 1)

"%OUT%\test_bindings.exe" "%ROOT%\src"
set RC=%ERRORLEVEL%
if not "%RC%"=="0" (echo [test] FAILURES) else (echo [test] ALL PASS)
exit /b %RC%
