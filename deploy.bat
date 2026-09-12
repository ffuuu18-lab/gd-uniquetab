@echo off
rem deploy.bat - copy the built mod into the game folder. Exactly two things are added to the
rem installation: x64\uniquetab.asi, and the x64\uniquetab\ folder holding everything else the mod
rem reads and writes. undeploy.bat removes both again.
rem
rem The .asi is loaded by Ultimate ASI Loader, which is NOT part of this mod and is never
rem installed, replaced or removed by these scripts. It has to be in x64\ already; this script
rem refuses to deploy without it unless --no-loader-check is passed.
rem
rem   deploy.bat                       find the game through %GD_DIR% or the Steam registry
rem   deploy.bat "<path to Grim Dawn>" use this installation (the folder holding x64\, or the
rem                                    x64 folder itself - either is accepted)
rem   deploy.bat --no-loader-check     deploy even though no loader is in x64\ yet
setlocal enabledelayedexpansion

set "ROOT=%~dp0"
set "SRC=%ROOT%bin\uniquetab.asi"

rem ---- the arguments ---------------------------------------------------------------------------
rem Read as %~1 / %~2 and not with a for loop, so a path with spaces or brackets stays one word.
set "NOLOADER="
set "ARG=%~1"
if /i "%ARG%"=="--no-loader-check" (set "NOLOADER=1" & set "ARG=")
if /i "%~2"=="--no-loader-check" set "NOLOADER=1"

rem ---- where the game is -----------------------------------------------------------------------
rem 1. the argument, 2. %GD_DIR%, 3. the Steam client's own InstallPath in the registry. No drive
rem letter and no user folder is written down anywhere in this file.
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
    echo [deploy] ERROR: the Grim Dawn folder was not found.
    echo          Pass it:  deploy.bat "<the folder that holds x64\Grim Dawn.exe>"
    echo          or set GD_DIR to it, or install Steam so the registry knows where it is.
    exit /b 1
)
if "%GAME:~-1%"=="\" set "GAME=%GAME:~0,-1%"

rem The x64 folder is as good an answer as the game root: that is where the exe, the loader and
rem the .asi all live, so it is the folder a reader is most likely to have in hand.
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

if not exist "%GAMEDIR%\Grim Dawn.exe" (
    echo [deploy] ERROR: "%GAMEDIR%\Grim Dawn.exe" not found - that is not a Grim Dawn folder.
    exit /b 1
)
if not exist "%SRC%" (
    echo [deploy] ERROR: %SRC% not built. Run build.bat first.
    exit /b 1
)
echo [deploy] game "%GAME%"

rem ---- the loader ------------------------------------------------------------------------------
rem Ultimate ASI Loader ships under one of several system DLL names; the version resource is
rem what says which of them is the loader and which is somebody else's file. Nothing here writes
rem to it - the loader is the user's to install and to remove.
set "LOADER="
for /f "usebackq delims=" %%L in (`powershell -NoProfile -ExecutionPolicy Bypass -Command "$d='%GAMEDIR%'; foreach($n in 'dinput8.dll','winmm.dll','version.dll','dsound.dll','dinput.dll','d3d9.dll','d3d11.dll','dxgi.dll'){$p=Join-Path $d $n; if(Test-Path -LiteralPath $p){$pn=(Get-Item -LiteralPath $p).VersionInfo.ProductName; if($pn -and $pn -match 'ASI.?Loader'){$n; break}}}" 2^>nul`) do set "LOADER=%%L"
if defined LOADER (
    echo [deploy] loader "%GAMEDIR%\%LOADER%" - left exactly as it is
) else (
    if defined NOLOADER (
        echo [deploy] --no-loader-check: no Ultimate ASI Loader in "%GAMEDIR%" - deploying anyway.
        echo [deploy]                    Nothing will load the .asi until one is put there.
    ) else (
        echo [deploy] ERROR: no Ultimate ASI Loader in "%GAMEDIR%".
        echo          The mod is an .asi plugin: something has to load it. Get the x64 build of
        echo          Ultimate ASI Loader from https://github.com/ThirteenAG/Ultimate-ASI-Loader
        echo          and put its dinput8.dll next to "Grim Dawn.exe" in the x64 folder.
        echo          Then run this script again, or pass --no-loader-check to deploy without it.
        exit /b 1
    )
)

rem Archive the .asi that is in the game folder RIGHT NOW, before it is replaced, so a crash dump
rem can always be matched byte for byte against the binary that ran. The copy is named after that
rem file's own last-write time, so bin\history reads as a timeline.
if exist "%DST%" (
    if not exist "%ROOT%bin\history" mkdir "%ROOT%bin\history"
    for %%F in ("%DST%") do set "STAMP=%%~tF"
    set "STAMP=!STAMP::=!"
    set "STAMP=!STAMP: =-!"
    set "STAMP=!STAMP:.=!"
    set "STAMP=!STAMP:/=!"
    copy /y "%DST%" "%ROOT%bin\history\uniquetab-!STAMP!.asi" >nul
    if errorlevel 1 (
        echo [deploy] ERROR: could not archive the .asi already in the game folder - refusing to
        echo          overwrite it, because a crash dump could then never be matched to it.
        exit /b 1
    )
    echo [deploy] archived the previous .asi -^> bin\history\uniquetab-!STAMP!.asi
)

copy /y "%SRC%" "%DST%" >nul
if errorlevel 1 (
    echo [deploy] ERROR: copy failed - is the game running?
    exit /b 1
)
echo [deploy] OK -^> "%DST%"
for %%F in ("%DST%") do echo [deploy] %%~zF bytes, %%~tF

rem An installation that ran an older build still has that build's own winmm.dll in x64\, and
rem two copies of the mod in one folder would both load. It is taken away ONLY when it carries the
rem old build's own marker string: anything else under that name - the loader itself, ReShade,
rem somebody's else shim - is left alone. This is the last use of that marker.
if exist "%GAMEDIR%\winmm.dll" (
    findstr /m /c:GDUNIQUETAB_LANEA_HOOKDLL "%GAMEDIR%\winmm.dll" >nul 2>&1
    if errorlevel 1 (
        echo [deploy] "%GAMEDIR%\winmm.dll" is not this mod - left alone
    ) else (
        del /q "%GAMEDIR%\winmm.dll"
        if exist "%GAMEDIR%\winmm.dll" (
            echo [deploy] WARNING: an older build of this mod is still at "%GAMEDIR%\winmm.dll"
            echo [deploy]          and could not be deleted. Remove it by hand: it would load a
            echo [deploy]          second copy of the mod.
        ) else (
            echo [deploy] removed the older winmm.dll build of this mod
        )
    )
)

rem ---- the mod folder ---------------------------------------------------------------------------
rem Everything else the mod ships goes into x64\uniquetab\, which is also where the .asi creates
rem the ini, the log, the collection and the exports. One folder to back up, one folder to delete.
rem   uniq-pages.arz   the database overlay: the tab's own pages and boxes (never overrides a
rem                    vanilla record; it is handed to Engine::LoadDatabase by path, it is not a
rem                    /basemods mod and it is not installed into the game's database folder)
rem   plates\*.tex     the generated cover plates
if not exist "%MODDIR%" mkdir "%MODDIR%"
if not exist "%MODDIR%" (
    echo [deploy] ERROR: could not create "%MODDIR%"
    exit /b 1
)

rem An old build left its data loose in x64\; take it away so nothing stale is ever loaded.
for %%N in (catalogue.bin uniq.arz uniq-pages.arz uniq-vanilla.arz uniq-records.txt uniq-pages.txt uniq-groups.txt) do (
    if exist "%GAMEDIR%\%%N" (
        del /q "%GAMEDIR%\%%N"
        echo [deploy] removed the old loose "%GAMEDIR%\%%N"
    )
)

rem catalogue.bin and the three uniq txt files are NOT copied: the mod generates them into
rem "%MODDIR%" itself on the first launch, from the game's own database.
set "FAILED="
for %%N in (uniq-pages.arz) do (
    if exist "%ROOT%data\uniq\%%N" (
        copy /y "%ROOT%data\uniq\%%N" "%MODDIR%\%%N" >nul || set "FAILED=%%N"
        echo [deploy] OK -^> "%MODDIR%\%%N"
    ) else (
        echo [deploy] WARNING: data\uniq\%%N missing - run tools\build_uniq_db.py
    )
)
if not exist "%MODDIR%\plates" mkdir "%MODDIR%\plates"
copy /y "%ROOT%data\plates\*.tex" "%MODDIR%\plates\" >nul 2>&1
if errorlevel 1 (echo [deploy] WARNING: data\plates\*.tex missing - run tools\make_plates.py) else (echo [deploy] OK -^> "%MODDIR%\plates\")

rem The engine loads textures by resource name through its override roots, and the game folder's
rem own settings\ tree is one of them. The plates go there too, under the name the mod asks for
rem (ui/caravan/uniq_plate_WxH.tex); only files of that name are ever written or removed.
set "PLATEDIR=%GAME%\settings\ui\caravan"
if not exist "%PLATEDIR%" mkdir "%PLATEDIR%"
copy /y "%ROOT%data\plates\uniq_plate_*.tex" "%PLATEDIR%\" >nul 2>&1
if errorlevel 1 (echo [deploy] WARNING: could not copy the plates into "%PLATEDIR%") else (echo [deploy] OK -^> "%PLATEDIR%\uniq_plate_*.tex")

if defined FAILED (
    echo [deploy] ERROR: could not copy %FAILED% into "%MODDIR%"
    exit /b 1
)
echo [deploy] done. Everything this mod reads or writes is in "%MODDIR%".
exit /b 0
