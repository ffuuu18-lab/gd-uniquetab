@echo off
rem ============================================================================================
rem  build.bat - the mod's hook DLL
rem
rem  Produces bin\uniquetab.asi : x64 Release, /MD (the game uses the VC14x runtime, so an
rem  std::string handed to Engine.dll must be MSVC's).  Toolchain: MSVC 2022 Build Tools.
rem
rem  The .asi extension is what Ultimate ASI Loader looks for: it LoadLibrary's every *.asi
rem  beside the exe and in scripts\ / plugins\ under it. The file is an ordinary DLL - it
rem  exports nothing and only DllMain runs.
rem
rem  Layers:
rem    third_party\minhook\src\*.c        MinHook (MIT), compiled as C
rem    src\*.cpp                          the mod itself
rem    src\model\*.cpp                    the catalogue / collection / layout model
rem ============================================================================================
setlocal enabledelayedexpansion

set "ROOT=%~dp0"

call "%ROOT%tools\find_vcvars.bat"
if errorlevel 1 (
    echo [build] ERROR: no x64 C++ toolchain - see the message above
    exit /b 1
)
if not defined VSCMD_ARG_TGT_ARCH call "%VCVARS%" >nul
if errorlevel 1 (
    echo [build] ERROR: vcvars64.bat failed
    exit /b 1
)

set "OBJ=%ROOT%build\obj"
set "BIN=%ROOT%bin"
if not exist "%OBJ%" mkdir "%OBJ%"
if not exist "%BIN%" mkdir "%BIN%"
del /q "%OBJ%\*.obj" 2>nul

set "MINHOOK=%ROOT%third_party\minhook"
if not exist "%MINHOOK%\include\MinHook.h" (
    echo [build] ERROR: MinHook missing. Run:
    echo         git clone https://github.com/TsudaKageyu/minhook "%MINHOOK%"
    exit /b 1
)

rem ---- 1. MinHook (C, third-party: no /WX) ---------------------------------------------------
echo [build] MinHook...
cl /nologo /c /O2 /MD /W3 /GS- /DNDEBUG /D_CRT_SECURE_NO_WARNINGS ^
   /I"%MINHOOK%\include" /Fo"%OBJ%\\" ^
   "%MINHOOK%\src\buffer.c" "%MINHOOK%\src\hook.c" "%MINHOOK%\src\trampoline.c" ^
   "%MINHOOK%\src\hde\hde64.c"
if errorlevel 1 goto :fail

rem ---- 2. the mod --------------------------------------------------------------------------
echo [build] mod sources...
cl /nologo /c /O2 /MD /W4 /WX /EHsc /std:c++17 /GR- /DNDEBUG ^
   /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
   /I"%MINHOOK%\include" /I"%ROOT%src" /Fo"%OBJ%\\" ^
   "%ROOT%src\dllmain.cpp" "%ROOT%src\ut_log.cpp" "%ROOT%src\ut_paths.cpp" ^
   "%ROOT%src\ut_config.cpp" ^
   "%ROOT%src\gd_runtime.cpp" "%ROOT%src\hooks.cpp" "%ROOT%src\ut_bindings.cpp" ^
   "%ROOT%src\ut_panel.cpp" "%ROOT%src\ut_reagent.cpp" ^
   "%ROOT%src\ut_live.cpp" "%ROOT%src\ut_rescue.cpp" "%ROOT%src\ut_plate.cpp" ^
   "%ROOT%src\ut_tooltip.cpp" "%ROOT%src\ut_store.cpp" "%ROOT%src\ut_generate.cpp" ^
   "%ROOT%src\model\catalogue.cpp" "%ROOT%src\model\collection.cpp" ^
   "%ROOT%src\model\layout.cpp" ^
   "%ROOT%src\gen\arz_reader.cpp" "%ROOT%src\gen\arc_reader.cpp" ^
   "%ROOT%src\gen\catalogue_gen.cpp" "%ROOT%src\gen\pages_gen.cpp" "%ROOT%src\gen\generate.cpp"
if errorlevel 1 goto :fail

rem ---- 3. link -------------------------------------------------------------------------------
echo [build] link...
rem EVERY build carries symbols. Without them a crash dump has to be read structurally, frame by
rem frame, and by then a deploy has usually overwritten the binary that crashed. /MAP gives the
rem rva -> function table, /DEBUG puts uniquetab.pdb next to the .asi, and deploy.bat archives the
rem file it replaces, so a dump can always be matched to the binary that ran.
rem /PDBALTPATH writes only the bare file name "uniquetab.pdb" into the binary's debug record, so
rem the folder this was built in never travels with the .asi; the local .pdb still resolves.
link /nologo /DLL /MACHINE:X64 /OPT:REF /OPT:ICF /INCREMENTAL:NO /DEBUG /PDBALTPATH:%%_PDB%% ^
     /MAP:"%ROOT%build\uniquetab.map" /PDB:"%BIN%\uniquetab.pdb" ^
     /OUT:"%BIN%\uniquetab.asi" ^
     "%OBJ%\*.obj" kernel32.lib user32.lib
if errorlevel 1 goto :fail

echo.
echo [build] OK -^> %BIN%\uniquetab.asi
for %%F in ("%BIN%\uniquetab.asi") do echo [build] size %%~zF bytes, %%~tF
exit /b 0

:fail
echo.
echo [build] FAILED
exit /b 1
