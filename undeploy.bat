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
rem exits 1 while that journal, or a custom game's own under mods\<name>\ (or the old
rem views\custom-<name>\), still holds
rem entries and no NEWER rescue-report.txt sits beside it.
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
rem Only the files the mod GENERATES (and older builds shipped) are deleted: the catalogue, its
rem stamp, the three lists and the page overlay. KEPT, because they are the user's: uniquetab.ini
rem (the settings), uniquetab-buttons.ini (the buttons, written once with the game's groups, then
rem yours - only its [unassigned] section is ever rewritten), uniquetab-groups.ini (optional,
rem yours: the mod never writes it), uniquetab-groups.ini.old* (an unedited copy an older build
rem wrote, renamed), uniquetab-text.ini.old* (an older build's words, folded into the buttons file
rem and renamed; uniquetab-text.ini itself is no longer written), the collection
rem (uniq-items*.jsonl, their prune copies), the exports (uniq-export*.gds, uniq-export.csv),
rem rescue-report.txt, uniquetab.log and its archives. Also kept, though the mod writes it:
rem uniquetab.state (where the tab was left - the group, the row, the OWN filter; deleting it only
rem resets that). So the folder is left in place holding them; delete it by hand when you are done.
rem mods\ holds one folder per custom game, mods\<name>\: its collection and exports, its own
rem settings (uniquetab.ini) and buttons (uniquetab-buttons.ini), and the generated set,
rem uniquetab.state and uniquetab.log (+ archives) of the game without /basemods; mods\<name>\basemods\
rem holds the same game's /basemods generated set, state and log. It is never touched (nor is an
rem old views\ folder an older build left).
for %%N in (catalogue.bin catalogue.stamp uniq-pages.arz uniq-vanilla.arz uniq-records.txt uniq-pages.txt uniq-groups.txt) do (
    if exist "%MODDIR%\%%N" (
        del /q "%MODDIR%\%%N"
        if exist "%MODDIR%\%%N" (echo [undeploy] WARNING: could not delete "%MODDIR%\%%N") else (echo [undeploy] removed "%MODDIR%\%%N")
    )
)
rem The plates the mod painted, every view's: they are all named uniq_plate_*.tex, and no other
rem name in that folder is ever written or removed.
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
    echo [undeploy] "%MODDIR%" kept - it still holds your own files (the collection, the ini files, the logs^).
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
