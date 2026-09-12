@echo off
rem find_vcvars.bat - locate vcvars64.bat without naming a drive, a user folder or a VS edition.
rem
rem CALL it (do not run it in a sub-shell): it sets VCVARS in the caller's environment and exits
rem non-zero with a readable message when no x64 C++ toolchain is installed.
rem
rem   call "%~dp0tools\find_vcvars.bat"
rem   if errorlevel 1 exit /b 1
rem   if not defined VSCMD_ARG_TGT_ARCH call "%VCVARS%" >nul
rem
rem vswhere.exe ships with every Visual Studio 2017+ installer and is the supported way to ask
rem where the toolchain is; the fixed 2022 locations below are only a fallback for an install
rem whose vswhere is missing.
rem
rem Everything here stays OUT of parenthesised blocks and out of `for /f` backquotes on purpose:
rem the Program Files (x86) folder name contains a closing parenthesis, and cmd ends a block or an
rem `in (...)` list at the first one it sees, even one that came out of a variable.
set "VCVARS="
set "PF86=%ProgramFiles(x86)%"
set "PF64=%ProgramFiles%"
set "VSTMP=%TEMP%\ut_vcvars_path.txt"

set "VSWHERE=%PF86%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%PF64%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :fallback

"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find VC\Auxiliary\Build\vcvars64.bat > "%VSTMP%" 2>nul
for /f "usebackq delims=" %%I in ("%VSTMP%") do set "VCVARS=%%I"
del /q "%VSTMP%" 2>nul
if defined VCVARS if exist "%VCVARS%" goto :done
set "VCVARS="

:fallback
set "VSBASE=%PF86%\Microsoft Visual Studio\2022"
set "VSTAIL=VC\Auxiliary\Build\vcvars64.bat"
if exist "%VSBASE%\BuildTools\%VSTAIL%"   set "VCVARS=%VSBASE%\BuildTools\%VSTAIL%"   & goto :done
if exist "%VSBASE%\Community\%VSTAIL%"    set "VCVARS=%VSBASE%\Community\%VSTAIL%"    & goto :done
if exist "%VSBASE%\Professional\%VSTAIL%" set "VCVARS=%VSBASE%\Professional\%VSTAIL%" & goto :done
if exist "%VSBASE%\Enterprise\%VSTAIL%"   set "VCVARS=%VSBASE%\Enterprise\%VSTAIL%"   & goto :done
set "VSBASE=%PF64%\Microsoft Visual Studio\2022"
if exist "%VSBASE%\BuildTools\%VSTAIL%"   set "VCVARS=%VSBASE%\BuildTools\%VSTAIL%"   & goto :done
if exist "%VSBASE%\Community\%VSTAIL%"    set "VCVARS=%VSBASE%\Community\%VSTAIL%"    & goto :done
if exist "%VSBASE%\Professional\%VSTAIL%" set "VCVARS=%VSBASE%\Professional\%VSTAIL%" & goto :done
if exist "%VSBASE%\Enterprise\%VSTAIL%"   set "VCVARS=%VSBASE%\Enterprise\%VSTAIL%"   & goto :done

echo ERROR: vcvars64.bat was not found.
echo        Install "Visual Studio 2022 Build Tools" with the "Desktop development with C++"
echo        workload, or start this from an "x64 Native Tools Command Prompt".
exit /b 1

:done
exit /b 0
