@echo off
rem build_test_store.bat - the PRIVATE TABLE's arithmetic, offline. NO GAME.
rem Run it from anywhere:  tools\build_test_store.bat   (it finds vcvars64 itself)
rem
rem Like tools\build_test_journal.bat, every pass runs ENTIRELY INSIDE THE REPOSITORY:
rem %UNIQUETAB_OUT% points at a folder under build\test, which the mod folder resolver reads
rem before anything else, so nothing here can reach a real installation or a Documents folder.
setlocal
set "ROOT=%~dp0.."
set "OUT=%ROOT%\build\test"
if not exist "%OUT%" mkdir "%OUT%"

if "%VSCMD_ARG_TGT_ARCH%"=="x64" goto :have_env
call "%ROOT%\tools\find_vcvars.bat"
if errorlevel 1 (echo [test] no x64 C++ toolchain - see the message above & exit /b 1)
call "%VCVARS%" >nul
:have_env

rem ut_rescue.cpp reads catalogue.bin for the "item" display name on each journal line, so the
rem offline harness carries model\catalogue.cpp exactly as the journal harness does.
cl /nologo /W4 /WX /EHsc /std:c++17 /GR- /MD /DNDEBUG /D_CRT_SECURE_NO_WARNINGS ^
   /Fo"%OUT%\\" /Fe"%OUT%\test_store.exe" ^
   "%ROOT%\tools\test_store.cpp" "%ROOT%\src\ut_rescue.cpp" "%ROOT%\src\ut_log.cpp" ^
   "%ROOT%\src\ut_paths.cpp" "%ROOT%\src\model\catalogue.cpp"
if errorlevel 1 (echo [test] BUILD FAILED & exit /b 1)

set RC=0

rem ---- passes 1 and 2: the life of a row, then a SEPARATE PROCESS reading the file back --------
set "SOUT=%OUT%\store-out"
if exist "%SOUT%" rd /s /q "%SOUT%"
mkdir "%SOUT%"
set "UNIQUETAB_OUT=%SOUT%"
"%OUT%\test_store.exe"
if errorlevel 1 set RC=1
"%OUT%\test_store.exe" --reread
if errorlevel 1 set RC=1

rem ---- pass 3: a FORMAT-3 file - the one on the user's disk right now --------------------------
set "SLEG=%OUT%\store-legacy"
if exist "%SLEG%" rd /s /q "%SLEG%"
mkdir "%SLEG%"
set "UNIQUETAB_OUT=%SLEG%"
"%OUT%\test_store.exe" --legacy "%SLEG%\uniq-items.jsonl"
if errorlevel 1 set RC=1

rem ---- pass 4: THE REHEARSAL - a COPY of a real collection journal ----------------------------
rem OPTIONAL, and driven by %UNIQUETAB_TEST_JSONLJOURNAL%: the path of a real uniq-items.jsonl.
rem The original is only ever READ - it is copied into the repository's own scratch folder and
rem never opened for writing, never moved and never deleted. This is the pass that proves a first
rem run on a machine that already has a collection is safe: a real file, the real reader, the real
rem writer. Without the variable it SKIPS LOUDLY.
set "SLIVE=%OUT%\store-live"
if exist "%SLIVE%" rd /s /q "%SLIVE%"
mkdir "%SLIVE%"
set "LIVEJSONL=%UNIQUETAB_TEST_JSONLJOURNAL%"
rem `if defined A if exist B (...) else (...)` binds the else to the SECOND if, so with the
rem variable unset NEITHER branch ran and the skip was SILENT. One flag, one if.
set "HAVELIVE="
if defined LIVEJSONL if exist "%LIVEJSONL%" set "HAVELIVE=1"
if defined HAVELIVE (
  copy /y "%LIVEJSONL%" "%SLIVE%\uniq-items.jsonl" >nul
  echo [test] rehearsal fixture: a COPY of %LIVEJSONL% - the original is READ ONLY
  set "UNIQUETAB_OUT=%SLIVE%"
  "%OUT%\test_store.exe" --live
  if errorlevel 2 (echo [test] ***** SKIPPED the rehearsal - the copy held no entries *****) else (if errorlevel 1 set RC=1)
) else (
  echo [test] ***** SKIPPED the rehearsal - set UNIQUETAB_TEST_JSONLJOURNAL to a real
  echo [test]       uniq-items.jsonl to run it. The file is only ever READ, and a COPY is what
  echo [test]       the test touches. *****
)

rem ---- pass 6: THE PAINT GATE (src\ut_paintgate.h) ---------------------------------------------
rem A row the mod's own file owns paints from a WRITABLE journal and not from a read-only one,
rem because everything that could balance the paint needs a journal that can own it. No file, no
rem engine, no globals.
"%OUT%\test_store.exe" --paint
if errorlevel 1 set RC=1

rem ---- pass 7: THE DEPOSIT GATE (src\ut_depositgate.h) -----------------------------------------
rem A deposit of one of our records goes into the private TABLE or is REFUSED; "hand it to the
rem engine" is not a value the verdict enum has. Every combination of the
rem eight facts is decided and exactly five of them may touch the journal, which is the offline
rem proof that a refusal leaves uniq-items.jsonl alone. No file, no engine, no globals.
"%OUT%\test_store.exe" --deposit
if errorlevel 1 set RC=1

rem ---- pass 8: ONE COLLECTION PER MODE ---------------------------------------------------------
rem Softcore keeps uniq-items.jsonl and hardcore gets uniq-items-hc.jsonl beside it. The pass
rem opens on softcore (start-up, no character), writes an entry, switches to hardcore, writes
rem another, switches back, and reads BOTH files to prove each holds its own entry and its own
rem saveVariant header. Its own folder, because it writes two journals.
set "SMODE=%OUT%\store-modes"
if exist "%SMODE%" rd /s /q "%SMODE%"
mkdir "%SMODE%"
set "UNIQUETAB_OUT=%SMODE%"
"%OUT%\test_store.exe" --modes
if errorlevel 1 set RC=1
set "UNIQUETAB_OUT=%SOUT%"

rem ---- the guard must still refuse to uninstall over a table that holds something --------------
rem The table IS the collection now, so a journal with a count >= 1 row is exactly what
rem journal_guard.ps1 exists to stop the user deleting. Pass 1 left A at count 3.
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\tools\journal_guard.ps1" -OutDir "%SOUT%"
set GRC=%ERRORLEVEL%
echo [test] guard exit code with a TABLE-OWNED row present = %GRC% (1 = refuses, correct)
if not "%GRC%"=="1" (echo [test] FAIL: the guard allowed an uninstall over the private table & set RC=1)

set "UNIQUETAB_OUT="
echo [test] every pass ran inside %OUT% - no installation and no Documents folder was touched
if not "%RC%"=="0" (echo [test] FAILURES) else (echo [test] ALL PASS)
exit /b %RC%
