@echo off
rem build_test_groups.bat - the groups as data, offline: the uniquetab-groups.ini parser and
rem matcher, the auto groups (no Retired group), the journal count scan, the G line of
rem uniq-groups.txt (both forms, and every G line of data\oracle\uniq-groups.txt), the user's
rem file merged in front of the built-in set, the class words, and the .old retirement of every
rem template an earlier build wrote (data\oracle\templates, copied by build_uniq_db.py
rem --stock-templates; without those fixtures that part says SKIPPED). NO GAME NEEDED; writes only
rem under build\test.
rem   tools\build_test_groups.bat        (finds vcvars64 itself, like build.bat)
rem
rem It compiles the REAL headers the generator compiles - src\gen\ut_groups.h and
rem src\gen\ut_groups_default.h.
setlocal
set "ROOT=%~dp0.."
set "OUT=%ROOT%\build\test"
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OUT%\groups" mkdir "%OUT%\groups"

if "%VSCMD_ARG_TGT_ARCH%"=="x64" goto :have_env
call "%ROOT%\tools\find_vcvars.bat"
if errorlevel 1 (echo [test] no x64 C++ toolchain - see the message above & exit /b 1)
call "%VCVARS%" >nul
:have_env

cl /nologo /W4 /WX /EHsc /std:c++17 /GR- /MD /DNDEBUG /D_CRT_SECURE_NO_WARNINGS ^
   /I"%ROOT%\src" /Fo"%OUT%\\" /Fe"%OUT%\test_groups.exe" ^
   "%ROOT%\tools\test_groups.cpp"
if errorlevel 1 (echo [test] BUILD FAILED & exit /b 1)

python "%ROOT%\tools\build_uniq_db.py" --stock-templates "%OUT%\groups\stock"
"%OUT%\test_groups.exe" "%ROOT%\data\oracle\uniq-groups.txt" "%OUT%\groups" "%OUT%\groups\stock"
exit /b %ERRORLEVEL%
