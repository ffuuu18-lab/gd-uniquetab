@echo off
rem undeploy.bat - remove the mod from the game folder, restoring a stock installation.
rem
rem The Ultimate ASI Loader in x64\ is NOT this mod and is never removed here: other .asi plugins
rem may depend on it. Delete it by hand once nothing else needs it.
rem
rem   undeploy.bat                       find the game through %GD_DIR% or the Steam registry
rem   undeploy.bat "<path to Grim Dawn>" use this installation (the folder holding x64\, or the
rem                                      x64 folder itself - either is accepted)
rem   undeploy.bat --force               remove it even while the collection still holds items
setlocal enabledelayedexpansion

set "ROOT=%~dp0"
set "FORCE="
set "ARG=%~1"
if /i "%ARG%"=="--force" (set "FORCE=1" & set "ARG=")
if /i "%ARG%"=="-force" (set "FORCE=1" & set "ARG=")
if /i "%ARG%"=="/force" (set "FORCE=1" & set "ARG=")
if /i "%~2"=="--force" set "FORCE=1"

rem ---- where the game is (same rule as deploy.bat) ----------------------------------------------
set "GAME=%ARG%"
if not defined GAME set "GAME=%GD_DIR%"
if not defined GAME (
    for /f "usebackq tokens=2,*" %%A in (`reg query "HKCU\Software\Valve\Steam" /v SteamPath 2^>nul ^| find "SteamPath"`) do set "STEAM=%%B"
    if not defined STEAM (
        for /f "usebackq tokens=2,*" %%A in (`reg query "HKCU\Software\Valve\Steam" /v InstallPath 2^>nul ^| find "InstallPath"`) do set "STEAM=%%B"
    )
    if defined STEAM (
        set "STEAM=!STEAM:/=\!"
        set "GAME=!STEAM!\steamapps\common\Grim Dawn"
    )
)
if not defined GAME (
    echo [undeploy] ERROR: the Grim Dawn folder was not found.
    echo            Pass it:  undeploy.bat "<the folder that holds x64\Grim Dawn.exe>"
    echo            or set GD_DIR to it.
    exit /b 1
)
if "%GAME:~-1%"=="\" set "GAME=%GAME:~0,-1%"
rem The game root carries a 32-bit Grim Dawn.exe of its own, so the x64 subfolder is tested first.
if exist "%GAME%\x64\Grim Dawn.exe" (
    set "GAMEDIR=%GAME%\x64"
) else if exist "%GAME%\Grim Dawn.exe" (
    set "GAMEDIR=%GAME%"
    for %%I in ("%GAME%") do set "GAME=%%~dpI"
    if "!GAME:~-1!"=="\" set "GAME=!GAME:~0,-1!"
) else (
    set "GAMEDIR=%GAME%\x64"
)
set "MODDIR=%GAMEDIR%\uniquetab"
set "DST=%GAMEDIR%\uniquetab.asi"
echo [undeploy] game "%GAME%"

rem ---- THE SAFETY INTERLOCK --------------------------------------------------------------------
rem The collection is uniquetab\uniq-items.jsonl and the items it names are held in the mod's own
rem private table - with the mod gone they are not in the game at all. tools\journal_guard.ps1
rem exits 1 while that journal still holds entries and no NEWER rescue-report.txt sits beside it.
rem Use "undeploy.bat --force" to accept the loss.
if defined FORCE (
    echo [undeploy] --force: skipping the collection check. Anything the mod still holds will be
    echo [undeploy]          gone once the mod is removed.
) else (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%tools\journal_guard.ps1" -OutDir "%MODDIR%"
    if errorlevel 1 (
        echo [undeploy] ABORTED. Nothing was removed.
        exit /b 1
    )
)

rem ---- the mod folder ---------------------------------------------------------------------------
rem Only the files the mod SHIPS are deleted. The ini, the log, the collection and the exports are
rem the user's, so the folder is left in place holding them; delete it by hand when you are done.
for %%N in (catalogue.bin uniq-pages.arz uniq-vanilla.arz uniq-records.txt uniq-pages.txt uniq-groups.txt) do (
    if exist "%MODDIR%\%%N" (
        del /q "%MODDIR%\%%N"
        if exist "%MODDIR%\%%N" (echo [undeploy] WARNING: could not delete "%MODDIR%\%%N") else (echo [undeploy] removed "%MODDIR%\%%N")
    )
)
if exist "%GAME%\settings\ui\caravan\uniq_plate_*.tex" (
    del /q "%GAME%\settings\ui\caravan\uniq_plate_*.tex" 2>nul
    echo [undeploy] removed the plates from "%GAME%\settings\ui\caravan" ^(only uniq_plate_*.tex; the folder stays^)
)
if exist "%MODDIR%\plates" (
    del /q "%MODDIR%\plates\*.tex" 2>nul
    rd "%MODDIR%\plates" 2>nul
    if exist "%MODDIR%\plates" (echo [undeploy] WARNING: could not remove "%MODDIR%\plates") else (echo [undeploy] removed "%MODDIR%\plates")
)
rd "%MODDIR%" 2>nul
if exist "%MODDIR%" (
    echo [undeploy] "%MODDIR%" kept - it still holds your own files (the collection, the ini, the log^).
) else (
    echo [undeploy] removed "%MODDIR%"
)

rem Anything an old build left loose in x64\ goes too.
for %%N in (catalogue.bin uniq.arz uniq-pages.arz uniq-vanilla.arz uniq-records.txt uniq-pages.txt uniq-groups.txt) do (
    if exist "%GAMEDIR%\%%N" (
        del /q "%GAMEDIR%\%%N"
        if exist "%GAMEDIR%\%%N" (echo [undeploy] WARNING: could not delete "%GAMEDIR%\%%N") else (echo [undeploy] removed "%GAMEDIR%\%%N")
    )
)

rem An installation that once ran the old build still has that build's own winmm.dll. It is
rem taken away ONLY when it carries the old marker string; the loader, ReShade and anything else
rem under that name are left alone. This is the last use of that marker.
if exist "%GAMEDIR%\winmm.dll" (
    findstr /m /c:GDUNIQUETAB_LANEA_HOOKDLL "%GAMEDIR%\winmm.dll" >nul 2>&1
    if errorlevel 1 (
        echo [undeploy] "%GAMEDIR%\winmm.dll" is not this mod - left alone
    ) else (
        del /q "%GAMEDIR%\winmm.dll"
        if exist "%GAMEDIR%\winmm.dll" (echo [undeploy] WARNING: could not delete the older winmm.dll build of this mod) else (echo [undeploy] removed the older winmm.dll build of this mod)
    )
)

if not exist "%DST%" (
    echo [undeploy] nothing more to do: "%DST%" does not exist
    exit /b 0
)

del /q "%DST%"
if exist "%DST%" (
    echo [undeploy] ERROR: could not delete "%DST%" - is the game running?
    exit /b 1
)
echo [undeploy] removed "%DST%"
echo [undeploy] the ASI loader in "%GAMEDIR%" was not touched - remove it by hand if nothing
echo [undeploy] else needs it.
exit /b 0
