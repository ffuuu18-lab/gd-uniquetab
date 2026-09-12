@echo off
rem build_test_journal.bat - the journal + identity-overlay round trip. NO GAME NEEDED.
rem Run from a vcvars64 prompt:  tools\build_test_journal.bat
rem
rem This harness runs ENTIRELY INSIDE THE REPOSITORY. Every pass points %UNIQUETAB_OUT% at a
rem folder under build\test, which the mod folder resolver reads before anything else, so nothing
rem here can reach a real installation's journal. The .pretest move-aside in test_journal.cpp is
rem kept as a second belt.
rem
rem THREE OPTIONAL FIXTURES, each a path in an environment variable. They are only ever READ, and
rem each is COPIED into build\test before any pass touches it. Without them the cases that need
rem them SKIP LOUDLY and the summary says so - a silent skip is how a harness comes to prove
rem nothing.
rem   UNIQUETAB_TEST_BINJOURNAL    a legacy binary uniq-items.bin   (the migration case)
rem   UNIQUETAB_TEST_JSONLJOURNAL  a real uniq-items.jsonl          (the upgrade / reconcile cases)
rem   UNIQUETAB_TEST_GDS           a .gds file exported by GD Stash (the byte-for-byte compare)
setlocal
set "ROOT=%~dp0.."
set "OUT=%ROOT%\build\test"
set "JOUT=%OUT%\journal-out"
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%ROOT%\out" mkdir "%ROOT%\out"

rem Find vcvars64 the way build.bat and build_test_config.bat do, so
rem this harness runs from any shell instead of only from a developer prompt.
if "%VSCMD_ARG_TGT_ARCH%"=="x64" goto :have_env
call "%ROOT%\tools\find_vcvars.bat"
if errorlevel 1 (echo [test] no x64 C++ toolchain - see the message above & exit /b 1)
call "%VCVARS%" >nul
:have_env

rem model\catalogue.cpp joins the link - ut_rescue.cpp reads catalogue.bin for
rem the "item" display name on each journal line, so the offline harness must carry it too.
cl /nologo /W4 /WX /EHsc /std:c++17 /GR- /MD /DNDEBUG /D_CRT_SECURE_NO_WARNINGS ^
   /Fo"%OUT%\\" /Fe"%OUT%\test_journal.exe" ^
   "%ROOT%\tools\test_journal.cpp" "%ROOT%\src\ut_rescue.cpp" "%ROOT%\src\ut_log.cpp" ^
   "%ROOT%\src\ut_paths.cpp" "%ROOT%\src\model\catalogue.cpp"
if errorlevel 1 (echo [test] BUILD FAILED & exit /b 1)

rem ---- the log flush rule --------------------------------------------------------------------
rem Its own process, its own file in %TEMP%, no journal anywhere near it: the case writes lines,
rem sleeps 1.5 s doing nothing at all, and reads its own log back through a second handle. A
rem force-killed game must not be able to leave a 0-byte log; this is that defect with no game
rem in it.
"%OUT%\test_journal.exe" --case-logflush
if errorlevel 1 (echo [test] FAIL: the log flush rule & set RCLOG=1) else (set RCLOG=0)

rem ---- passes 1 and 2: the identity round trip -------------------------------------------------
rem Pass 1 writes the journal (three entries, one of them the identity item) and checks the
rem overlay in memory. Pass 2 is a SEPARATE PROCESS: it reads that file back off the disk and
rem runs identityBuild again - the round trip a restored item depends on.
if exist "%JOUT%" rd /s /q "%JOUT%"
mkdir "%JOUT%"
set "UNIQUETAB_OUT=%JOUT%"
"%OUT%\test_journal.exe"
set RC=%ERRORLEVEL%
"%OUT%\test_journal.exe" --verify
set RC2=%ERRORLEVEL%
if not "%RC%"=="0" goto :guard
set RC=%RC2%
:guard

rem Show the file through the SAME parser undeploy.bat uses, then take it away again so the
rem interlock is not left armed by a test run.
"%OUT%\test_journal.exe" >nul 2>&1
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\tools\journal_guard.ps1" -OutDir "%JOUT%" -Dump
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\tools\journal_guard.ps1" -OutDir "%JOUT%"
set GRC=%ERRORLEVEL%
echo [test] guard exit code with entries and no report = %GRC% (1 = refuses, correct)
if not "%GRC%"=="1" (echo [test] FAIL: the guard did NOT refuse on a journal with entries & set RC=1)
del /q "%JOUT%\uniq-items.jsonl" 2>nul
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\tools\journal_guard.ps1" -OutDir "%JOUT%"
set GRC=%ERRORLEVEL%
echo [test] guard exit code with NO journal at all = %GRC% (0 = allows, correct)
if not "%GRC%"=="0" (echo [test] FAIL: the guard refused with no journal present & set RC=1)

rem ---- case (a): MIGRATION + the field-for-field round trip ------------------------------------
rem The fixture is a COPY of a real 356-entry binary journal. The backup is read ONLY - never
rem written, never moved - and the copy lives in the worktree's own scratch folder.
set "MIG=%OUT%\journal-migrate"
if exist "%MIG%" rd /s /q "%MIG%"
mkdir "%MIG%"
set "REALBIN=%UNIQUETAB_TEST_BINJOURNAL%"
if defined REALBIN (
  if exist "%REALBIN%" (
    copy /y "%REALBIN%" "%MIG%\uniq-items.bin" >nul
    echo [test] migration fixture: a COPY of %REALBIN% - the original is READ ONLY
  ) else (
    echo [test] ***** WARNING: UNIQUETAB_TEST_BINJOURNAL names %REALBIN%, which is not there *****
  )
) else (
  echo [test] UNIQUETAB_TEST_BINJOURNAL is not set - no binary journal fixture
)
set "UNIQUETAB_OUT=%MIG%"
rem A silent skip would leave RC untouched, so a machine without the fixture would print ALL PASS
rem having never exercised the migration at all. The skip is loud and the summary says SKIPS, so
rem "ALL PASS" always means every case really ran.
if not exist "%MIG%\uniq-items.bin" (
  echo [test] ***** SKIPPED case a - no binary fixture, the MIGRATION was NOT tested *****
  set "SKIPPED=a"
  goto :caseb
)
"%OUT%\test_journal.exe" --migrate "%OUT%\dump-from-bin.txt"
if errorlevel 1 set RC=1
"%OUT%\test_journal.exe" --reload "%OUT%\dump-from-bin.txt"
if errorlevel 1 set RC=1

:caseb
rem ---- case (b): a format NEWER than this build understands -> READ-ONLY --------------------
set "RO=%OUT%\journal-readonly"
if exist "%RO%" rd /s /q "%RO%"
mkdir "%RO%"
set "UNIQUETAB_OUT=%RO%"
"%OUT%\test_journal.exe" --make-newer
if errorlevel 1 set RC=1
"%OUT%\test_journal.exe" --case-readonly
if errorlevel 1 set RC=1

rem ---- case (c): ONE mangled line costs exactly ONE entry -----------------------------------
set "MG=%OUT%\journal-mangled"
if exist "%MG%" rd /s /q "%MG%"
mkdir "%MG%"
set "UNIQUETAB_OUT=%MG%"
"%OUT%\test_journal.exe" --make-mangled
if errorlevel 1 set RC=1
"%OUT%\test_journal.exe" --case-mangled
if errorlevel 1 set RC=1

rem ---- case (d): the journal EXISTS and cannot be read -> READ-ONLY, nothing overwritten -------
rem The dangerous shape: a file that is right there, unreadable, and a
rem uniq-items.bin beside it that must NOT be migrated over the top of it.
set "UR=%OUT%\journal-unreadable"
if exist "%UR%" rd /s /q "%UR%"
mkdir "%UR%"
set "UNIQUETAB_OUT=%UR%"
"%OUT%\test_journal.exe" --make-unreadable
if errorlevel 1 set RC=1
"%OUT%\test_journal.exe" --case-unreadable
if errorlevel 1 set RC=1

rem ---- case (e): the two hostile lines - the wrapping raw offset, and a len-0 entry -----------
set "HO=%OUT%\journal-hostile"
if exist "%HO%" rd /s /q "%HO%"
mkdir "%HO%"
set "UNIQUETAB_OUT=%HO%"
"%OUT%\test_journal.exe" --make-hostile
if errorlevel 1 set RC=1
"%OUT%\test_journal.exe" --case-hostile
if errorlevel 1 set RC=1

rem ---- case (f): a FORMAT-2 file is UPGRADED to format 3 without loss --------------------------
rem The fixture is a COPY of the user's live format-2 journal when this machine has one (that is
rem the file on their disk right now, 358 entries) and a small synthetic one otherwise. The
rem original is only ever READ - copied out, never opened for writing, never moved, never deleted.
set "UP=%OUT%\journal-upgrade"
if exist "%UP%" rd /s /q "%UP%"
mkdir "%UP%"
set "LIVEJSONL=%UNIQUETAB_TEST_JSONLJOURNAL%"
if defined LIVEJSONL if exist "%LIVEJSONL%" (
  copy /y "%LIVEJSONL%" "%UP%\uniq-items.jsonl" >nul
  echo [test] upgrade fixture: a COPY of %LIVEJSONL% - the original is READ ONLY
  echo [test]   - used only while that copy is still FORMAT 2; --make-v2 says SUBSTITUTED if not
) else (
  echo [test] upgrade fixture: UNIQUETAB_TEST_JSONLJOURNAL is unset or missing, so a synthetic
  echo [test]   format-2 file is used instead
)
set "UNIQUETAB_OUT=%UP%"
"%OUT%\test_journal.exe" --make-v2
if errorlevel 1 set RC=1
"%OUT%\test_journal.exe" --case-upgrade "%OUT%\dump-before-upgrade.txt"
if errorlevel 1 set RC=1
"%OUT%\test_journal.exe" --case-upgraded "%OUT%\dump-before-upgrade.txt"
if errorlevel 1 set RC=1

rem ---- case (g): the RECONCILIATION marks the right entries, and the prune drops only those ----
set "RE=%OUT%\journal-reconcile"
if exist "%RE%" rd /s /q "%RE%"
mkdir "%RE%"
if exist "%LIVEJSONL%" copy /y "%LIVEJSONL%" "%RE%\uniq-items.jsonl" >nul
set "UNIQUETAB_OUT=%RE%"
"%OUT%\test_journal.exe" --make-v2
if errorlevel 1 set RC=1
"%OUT%\test_journal.exe" --case-reconcile
if errorlevel 1 set RC=1

rem ---- case (h): a FAILED or EMPTY page read marks NOTHING - every entry stays UNKNOWN ---------
set "NR=%OUT%\journal-noreconcile"
if exist "%NR%" rd /s /q "%NR%"
mkdir "%NR%"
if exist "%LIVEJSONL%" copy /y "%LIVEJSONL%" "%NR%\uniq-items.jsonl" >nul
set "UNIQUETAB_OUT=%NR%"
"%OUT%\test_journal.exe" --make-v2
if errorlevel 1 set RC=1
"%OUT%\test_journal.exe" --case-noreconcile
if errorlevel 1 set RC=1

rem ---- case (i): THE USER'S BUG - a journal full of history and an EMPTY page ------------------
rem Every entry checked and none of them on the page: the guard must ALLOW the uninstall while the
rem file still holds every line. This is the regression for "356 item(s) are still stored" said of
rem a page that held six.
set "ST=%OUT%\journal-allstale"
if exist "%ST%" rd /s /q "%ST%"
mkdir "%ST%"
if exist "%LIVEJSONL%" copy /y "%LIVEJSONL%" "%ST%\uniq-items.jsonl" >nul
set "UNIQUETAB_OUT=%ST%"
"%OUT%\test_journal.exe" --make-v2
if errorlevel 1 set RC=1
"%OUT%\test_journal.exe" --case-allstale
if errorlevel 1 set RC=1
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\tools\journal_guard.ps1" -OutDir "%ST%"
set GRC=%ERRORLEVEL%
echo [test] guard exit code with a journal whose entries are ALL marked not-stored = %GRC% (0 = allows, correct)
if not "%GRC%"=="0" (echo [test] FAIL: the guard still refuses on stale entries alone & set RC=1)
rem ... and it must STILL refuse on the same file before anything has been checked.
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\tools\journal_guard.ps1" -OutDir "%NR%"
set GRC=%ERRORLEVEL%
echo [test] guard exit code on the never-reconciled journal = %GRC% (1 = refuses, correct)
if not "%GRC%"=="1" (echo [test] FAIL: the guard allowed an uninstall with UNCHECKED entries & set RC=1)
rem ... and on the reconciled one, which really does hold two.
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\tools\journal_guard.ps1" -OutDir "%RE%"
set GRC=%ERRORLEVEL%
echo [test] guard exit code with 2 stored entries = %GRC% (1 = refuses, correct)
if not "%GRC%"=="1" (echo [test] FAIL: the guard allowed an uninstall with items on the page & set RC=1)

rem ---- case (j): a TRUNCATED file whose header still claims stored entries -------------------
rem Line 1 and the lines are written in ONE atomic pass, so a header saying more stored than the
rem file carries can only be a truncated / half-copied / partly restored journal. Without the
rem header cross-check the at-risk shortcut exits 0 on exactly this file (0 stored lines, 0
rem unknown lines -> nothing at risk) and lets undeploy.bat run.
set "TR=%OUT%\journal-truncated"
if exist "%TR%" rd /s /q "%TR%"
mkdir "%TR%"
rem The fixture is the ALL-STALE file (358 lines, every one "stored":false, header "stored":0)
rem with line 1 rewritten to claim two stored entries - i.e. exactly the shape a journal takes
rem when the two lines that WERE stored are lost and the header is not. Every line still parses,
rem so the truncation refusal above cannot see it: only the header cross-check can.
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$l = Get-Content -LiteralPath '%ST%\uniq-items.jsonl';" ^
  "$l[0] = $l[0] -replace '\"stored\":0', '\"stored\":2';" ^
  "Set-Content -LiteralPath '%TR%\uniq-items.jsonl' -Value $l -Encoding utf8"
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\tools\journal_guard.ps1" -OutDir "%TR%"
set GRC=%ERRORLEVEL%
echo [test] guard exit code on a journal whose header claims stored lines it does not have = %GRC% (1 = refuses, correct)
if not "%GRC%"=="1" (echo [test] FAIL: the guard allowed an uninstall on a TRUNCATED journal & set RC=1)

rem ---- the uniq-export.csv round trip ---------------------------------------------------------
rem Writes a journal that contains the two things RFC-4180 quoting exists for (a comma and a
rem double quote inside a field), then decodes uniq-export.csv with a reader in the test that
rem knows nothing about the writer. Also proves export_csv=0 writes no CSV at all.
set "CSVD=%OUT%\journal-csv"
if exist "%CSVD%" rd /s /q "%CSVD%"
mkdir "%CSVD%"
set "UNIQUETAB_OUT=%CSVD%"
"%OUT%\test_journal.exe" --case-csv
if errorlevel 1 set RC=1

rem ---- case (k): export_csv AS A MODE ---------------------------------------------------------
rem The fixture is the shape of the user's own journal - one entry stored, one not stored (the
rem stale history a refund left behind) and one nobody has reconciled yet. Proves export_csv=1
rem exports the collection INCLUDING the unreconciled entry, that =2 still exports everything
rem with the not-stored rows last, that =0 writes nothing and deletes nothing, and that an out of
rem range value clamps outwards instead of turning the export off.
set "CSVM=%OUT%\journal-csvmode"
if exist "%CSVM%" rd /s /q "%CSVM%"
mkdir "%CSVM%"
set "UNIQUETAB_OUT=%CSVM%"
"%OUT%\test_journal.exe" --make-csvmode
if errorlevel 1 set RC=1
"%OUT%\test_journal.exe" --case-csvmode
if errorlevel 1 set RC=1

rem ---- case (l): the FRESH INSTALL ------------------------------------------------------------
rem No fixture: an empty folder, i.e. a machine that has never deposited anything. Proves that
rem changing export_csv writes the CSV and does NOT create a uniq-items.jsonl (the mode change
rem arms the CSV-only pass, never the journal writer), and that an empty collection really is
rem the header line on its own - both of them promises the USER-GUIDE makes about this exact
rem machine state.
set "CSVE=%OUT%\journal-csvempty"
if exist "%CSVE%" rd /s /q "%CSVE%"
mkdir "%CSVE%"
set "UNIQUETAB_OUT=%CSVE%"
"%OUT%\test_journal.exe" --case-csvempty
if errorlevel 1 set RC=1
if exist "%CSVE%\uniq-items.jsonl" (echo [test] FAIL: the empty-journal case created a journal file & set RC=1)

rem ---- THE GD STASH IMPORT FILE ---------------------------------------------------------------
rem The strongest proof available without running GD Stash: a journal holding exactly the item GD
rem Stash itself exported must come out of the mod's writer BYTE FOR BYTE identical
rem to that file. Then a multi-item file, decoded by a reader in the test that knows nothing about
rem the writer, and modes 1 / 2 / 0 / out-of-range. The artefact is READ ONLY - it is passed as a
rem path and only ever opened for reading, to cross-check the copy embedded in the test.
set "GDSD=%OUT%\journal-gds"
if exist "%GDSD%" rd /s /q "%GDSD%"
mkdir "%GDSD%"
set "UNIQUETAB_OUT=%GDSD%"
if defined UNIQUETAB_TEST_GDS if exist "%UNIQUETAB_TEST_GDS%" (
  echo [test] .gds artefact: %UNIQUETAB_TEST_GDS% - READ ONLY, cross-checked against the copy
  echo [test]   embedded in the test
  "%OUT%\test_journal.exe" --case-gds "%UNIQUETAB_TEST_GDS%"
) else (
  echo [test] UNIQUETAB_TEST_GDS is unset or missing - the embedded copy of the artefact is used
  echo [test]   on its own, so the "it still matches the file on disk" check does NOT run
  "%OUT%\test_journal.exe" --case-gds
)
if errorlevel 1 set RC=1

rem ... and the FRESH INSTALL: no fixture, so an empty collection must be the 8-byte header on its
rem own (version 3, count 0 - a file GD Stash reads as "no items"), and changing the key must not
rem create a uniq-items.jsonl.
set "GDSE=%OUT%\journal-gdsempty"
if exist "%GDSE%" rd /s /q "%GDSE%"
mkdir "%GDSE%"
set "UNIQUETAB_OUT=%GDSE%"
"%OUT%\test_journal.exe" --case-gdsempty
if errorlevel 1 set RC=1
if exist "%GDSE%\uniq-items.jsonl" (echo [test] FAIL: the empty-journal .gds case created a journal file & set RC=1)

rem ---- case (m): a FORMAT-3 file, the shape an existing install has -----------------------------
rem Format 3 with 7 entries, every one of them an item the ENGINE reagent map holds. The upgrade
rem rule must read it whole, leave every row at count 0 (the private table holds none of it) and
rem only then accept a count written on purpose. A rule of "stored:true -> count 1" would invent
rem 7 copies here.
set "V3=%OUT%\journal-v3"
if exist "%V3%" rd /s /q "%V3%"
mkdir "%V3%"
set "UNIQUETAB_OUT=%V3%"
"%OUT%\test_journal.exe" --make-v3
if errorlevel 1 set RC=1
"%OUT%\test_journal.exe" --case-v3upgrade
if errorlevel 1 set RC=1

if "%RCLOG%"=="1" set RC=1
set "UNIQUETAB_OUT="
if exist "%ROOT%\out\uniq-items.jsonl.pretest" echo [test] WARNING: a .pretest file is still there - a pass did not restore the real journal; put it back by hand
if exist "%ROOT%\out\uniq-items.bin.pretest" echo [test] WARNING: an OLD .bin.pretest file is still there - put it back by hand
echo [test] every pass ran inside %OUT% - no installation and no Documents folder was touched
if not "%RC%"=="0" (
  echo [test] FAILURES: pass1/pass2=%RC2% overall=%RC%
) else (
  if defined SKIPPED (echo [test] PASS, BUT SKIPPED: %SKIPPED% - that case was NOT tested) else (echo [test] ALL PASS)
)
exit /b %RC%
