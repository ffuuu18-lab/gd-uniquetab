# journal_guard.ps1 - the safety interlock undeploy.bat calls.
#
# `GameEngine::ReadPlayerReagents` drops every reagent-map
# entry whose record has craftingMaterial == 0, and `LoadPlayerReagents` can trigger a
# `SaveReagents` on its own. So removing the mod while items are still on the collection page
# DELETES them at the next character load, and the pruned reagents.gst goes to Steam Cloud.
#
# This script exits 1 (refuse) when either is true:
#   * either collection journal (softcore uniq-items.jsonl, hardcore uniq-items-hc.jsonl) says
#     the mod still holds at least one item, or
#   * rescue-report.txt is missing or OLDER than the journal (i.e. the last rescue run does
#     not cover what the journal now contains).
# Exit 0 = safe to uninstall.  It reads files and writes nothing.
#
# A journal is `uniq-items.jsonl` (softcore) or `uniq-items-hc.jsonl` (hardcore): JSON Lines,
# line 1 a header carrying
# `format`, then one flat JSON object per stored item. This script reads THAT, with
# ConvertFrom-Json per line - there is no hand-rolled parser here.
#
# THE THREE RULES THAT KEEP THE ONE-WAY DOOR SHUT:
#   1. It ALSO finds and refuses on a LEGACY `uniq-items.bin` - someone who installed a build
#      that writes the text journal but has never started it still has every item in the binary
#      file. A guard that only knew the new name would report "empty, nothing can be lost" and
#      let undeploy.bat walk the one-way door with a full page behind it.
#   2. It IGNORES the aside copies - *.migrated-*, *.rejected-*, *.bad-*, *.unreadable-* and
#      *.tmp. Those are history, not the live journal.
#   3. When the journal exists but cannot be parsed at all, it REFUSES (exit 1). "I cannot read
#      it" is never "there is nothing in it".
#
# IT COUNTS WHAT IS STORED, NOT HOW MANY LINES THERE ARE. The journal keeps an
# entry for ever - it is the only record of what a deposited item WAS - so after a session played
# without the mod the file legitimately holds hundreds of entries the collection page does NOT
# hold, and a line count would refuse with "hundreds of items are still stored" over a page that
# holds six. Format 3 marks each entry, and the mark is THREE-STATE:
#     "stored":true    the mod saw this record in the engine's reagent map at the last caravan
#                      open -> AT RISK, refuse.
#     "stored":false   the mod looked and it was not there -> not at risk, and not counted.
#     no "stored" key  NOBODY HAS LOOKED YET (every entry of a format-2 file, every entry of the
#                      legacy .bin, and any entry written before the first reconciliation of this
#                      character) -> treated as AT RISK, because "unknown" must never read as
#                      "safe to delete". The user clears it by opening the caravan once.
# So the interlock still refuses loudly whenever anything might really be on the page, and stops
# refusing on stale history alone.
#
# Also usable on its own as a unit-style check of the journal file:
#     powershell -File tools\journal_guard.ps1 -Dump
[CmdletBinding()]
param(
    [string]$OutDir = "",
    [string]$GameX64 = "",
    [switch]$Dump
)

$ErrorActionPreference = "Stop"

# The folder to look in - the mod folder, the same two places ut_paths.cpp resolves: the uniquetab
# folder beside the plugin (pass -GameX64 <game>\x64; undeploy.bat passes -OutDir <mod folder>) and the
# Documents fallback. %UNIQUETAB_OUT% comes first, exactly as it does in the DLL, so the offline
# harnesses point this script at their own scratch folder. Callers may pass -OutDir instead.
# Nothing here is written down as a literal path: the game folder is the caller's business.
if (-not $OutDir) {
    $candidates = @()
    if ($env:UNIQUETAB_OUT) { $candidates += $env:UNIQUETAB_OUT }
    if ($GameX64) { $candidates += (Join-Path $GameX64 'uniquetab') }
    foreach ($docs in @('Documents', 'OneDrive\Documents')) {
        $candidates += (Join-Path $env:USERPROFILE (Join-Path $docs 'My Games\Grim Dawn\uniquetab'))
    }
    if (-not $candidates) { $candidates += (Get-Location).Path }
    $OutDir = $candidates[0]
    $found = $false
    # First a folder that actually holds the mod's files, in ladder order; only then the first
    # folder that merely exists. (The old loop kept the LAST existing candidate, which is not the
    # order ut_log.cpp resolves in.)
    foreach ($c in $candidates) {
        if (-not (Test-Path -LiteralPath $c)) { continue }
        if ((Test-Path -LiteralPath (Join-Path $c 'uniq-items.jsonl')) -or
            (Test-Path -LiteralPath (Join-Path $c 'uniq-items.bin')) -or
            (Test-Path -LiteralPath (Join-Path $c 'uniquetab.ini'))) { $OutDir = $c; $found = $true; break }
    }
    if (-not $found) {
        foreach ($c in $candidates) { if (Test-Path -LiteralPath $c) { $OutDir = $c; break } }
    }
}

# `journal_dir=` in uniquetab.ini moves the JOURNAL (and only the journal) out
# of the out-dir. ut_rescue.cpp reads it once at start-up; a guard that does not read it would
# look in the wrong folder and report an empty collection. The ini itself always lives in the
# out-dir, so it is found first and then consulted.
$ini = Join-Path $OutDir "uniquetab.ini"
$haveIni = Test-Path -LiteralPath $ini -PathType Leaf
$JournalDir = $OutDir
if ($haveIni) {
    $hit = Select-String -LiteralPath $ini -Pattern '^\s*journal_dir\s*=' | Select-Object -First 1
    if ($hit) {
        $v = ($hit.Line -replace '^\s*journal_dir\s*=', '') -replace ';.*$', ''
        $v = $v.Trim().Trim('"').TrimEnd('\', '/').Trim()
        if ($v -and (Test-Path -LiteralPath $v -PathType Container)) {
            $JournalDir = $v
            Write-Host "[guard] journal_dir=$v in $ini - looking for the journal THERE, not in $OutDir"
        }
    }
}
$report = Join-Path $OutDir "rescue-report.txt"

# ---- one journal ------------------------------------------------------------------------------
# The whole check, for ONE journal. There are two collections - softcore and hardcore - in the
# same folder, and each has its own journal, so the caller runs this once per mode and refuses if
# either does. Returns 0 (safe) or 1 (refuse); it writes nothing.
function Invoke-GuardOne([string]$mode, [string]$journal, [string]$legacy) {
    Write-Host ("[guard] ---- the {0} collection: {1} ----" -f $mode, (Split-Path -Leaf $journal))

    # ---- the readable journal ---------------------------------------------------------------------
    # One line = one item, so a damaged line costs that item only. A line without a `record` (line 1,
    # the header) is not an item. Anything that is not valid JSON is counted and REPORTED - it is
    # never silently treated as "nothing stored".
    function Read-TextJournal([string]$path) {
        $lines = [System.IO.File]::ReadAllLines($path)
        if ($lines.Count -eq 0) { throw "the journal $path is empty" }
        $header = $null
        try { $header = $lines[0] | ConvertFrom-Json } catch {
            throw "line 1 of $path is not a JSON object - the journal cannot be read"
        }
        if ($null -eq $header.format) { throw "line 1 of $path carries no format number" }
        $records = New-Object System.Collections.ArrayList
        $bad = 0
        for ($i = 1; $i -lt $lines.Count; $i++) {
            $line = $lines[$i].Trim()
            if (-not $line) { continue }
            $o = $null
            try { $o = $line | ConvertFrom-Json } catch { $bad++; continue }
            if ($null -eq $o.record) { $bad++; continue }
            # Three states. $null = nobody has looked (a format-2 line, or an entry
            # written before this character's first reconciliation) and that is NOT the same as false.
            $stored = $null
            if ($null -ne $o.stored) { $stored = [bool]$o.stored }
            [void]$records.Add([pscustomobject]@{
                    Record       = [string]$o.record
                    Item         = $(if ($null -ne $o.item) { [string]$o.item } else { "" })
                    Stored       = $stored
                    # format 2 keys are sOOO, format 3 keys are <name>@OOO. Both are slots.
                    StringSlots  = @($o.PSObject.Properties | Where-Object {
                            $_.Name -match '^s[0-9A-Fa-f]{3}$' -or $_.Name -match '^[A-Za-z0-9_]+@[0-9A-Fa-f]{3}$'
                        }).Count
                    ReplicaBytes = [int]$o.len
                    Flags        = [int]$o.flags
                })
        }
        [pscustomobject]@{
            Path       = $path
            Kind       = "uniq-items.jsonl (readable, format $($header.format))"
            Format     = [int]$header.format
            EntryCount = $records.Count
            HeaderSays = $(if ($null -ne $header.entries) { [int]$header.entries } else { -1 })
            # Line 1 also carries the mod's own count of how
            # many entries were stored when it last wrote the file. The mod always writes the header
            # and the lines in the same pass, so header-stored > counted-stored can only mean the file
            # was truncated, half-copied or partially restored - and that is a refusal, not an exit 0.
            HeaderStored = $(if ($null -ne $header.stored) { [int]$header.stored } else { -1 })
            BadLines   = $bad
            Records    = $records
        }
    }

    # ---- the LEGACY binary journal ----------------------------------------------------------------
    # Kept so a user who has not yet started the new build is still protected. Only the header's
    # entry count and the record strings are needed; nothing here writes.
    function Read-BinaryJournal([string]$path) {
        $bytes = [System.IO.File]::ReadAllBytes($path)
        if ($bytes.Length -lt 64) { throw "the legacy journal $path is only $($bytes.Length) bytes" }
        $magic = [System.Text.Encoding]::ASCII.GetString($bytes, 0, 8)
        if ($magic -ne "UNIQITM1") { throw "the legacy journal $path has magic '$magic', expected UNIQITM1" }
        $count = [BitConverter]::ToUInt32($bytes, 0x0C)
        $stride = [BitConverter]::ToUInt32($bytes, 0x10)
        $entriesOff = [BitConverter]::ToUInt32($bytes, 0x14)
        $stringsOff = [BitConverter]::ToUInt32($bytes, 0x28)
        $records = New-Object System.Collections.ArrayList
        for ($i = 0; $i -lt $count; $i++) {
            $at = $entriesOff + $i * $stride
            $recOff = [BitConverter]::ToUInt32($bytes, $at + 0x00)
            $recLen = [BitConverter]::ToUInt32($bytes, $at + 0x04)
            $repLen = [BitConverter]::ToUInt32($bytes, $at + 0x0C)
            $slots = [BitConverter]::ToUInt32($bytes, $at + 0x14)
            $name = [System.Text.Encoding]::ASCII.GetString($bytes, $stringsOff + $recOff, $recLen)
            # Stored = $null: the binary format has no such field and no reconciliation has ever run
            # against it, so every legacy entry is UNKNOWN and therefore AT RISK. That is the point.
            [void]$records.Add([pscustomobject]@{
                    Record = $name; Item = ""; Stored = $null
                    StringSlots = [int]$slots; ReplicaBytes = [int]$repLen; Flags = 0
                })
        }
        [pscustomobject]@{
            Path       = $path
            Kind       = "uniq-items.bin (LEGACY binary - this build has not started yet)"
            Format     = 1
            EntryCount = [int]$count
            HeaderSays = [int]$count
            HeaderStored = -1   # the legacy binary format has no such field
            BadLines   = 0
            Records    = $records
        }
    }

    $j = $null
    $parseError = $null
    try {
        if (Test-Path -LiteralPath $journal -PathType Leaf) {
            $j = Read-TextJournal $journal
        }
        elseif (Test-Path -LiteralPath $legacy -PathType Leaf) {
            $j = Read-BinaryJournal $legacy
            Write-Host "[guard] NOTE: only the OLD binary journal is here. The new build converts it to"
            Write-Host "[guard]       uniq-items.jsonl the first time it runs; until then this is the record."
        }
    } catch {
        $parseError = $_.Exception.Message
    }

    if ($parseError) {
        Write-Host "[guard] ERROR: $parseError"
        Write-Host "[guard] REFUSING: a journal file exists but cannot be parsed. 'I cannot read it' is"
        Write-Host "[guard] not the same as 'there is nothing in it' - it may hold every unique you own."
        Write-Host "[guard] Override (you accept the loss):  undeploy.bat --force"
        return 1
    }

    # ---- the three counts, computed from the LINES (the header is a comment) -----------------------
    # AtRisk is the whole decision: stored + never-checked + damaged. NotStored is the only class this
    # guard is now willing to ignore, and only because the mod itself looked at the engine's reagent
    # map and did not find the record - see the failure ladder in ut_reagent.cpp's journalReconcile,
    # which marks NOTHING unless that walk actually succeeded.
    $storedCount = 0
    $notStoredCount = 0
    $unknownCount = 0
    $atRisk = @()
    if ($j) {
        foreach ($r in $j.Records) {
            if ($null -eq $r.Stored) { $unknownCount++; $atRisk += $r }
            elseif ($r.Stored) { $storedCount++; $atRisk += $r }
            else { $notStoredCount++ }
        }
    }
    $atRiskCount = $atRisk.Count + $(if ($j) { $j.BadLines } else { 0 })

    if ($Dump) {
        if (-not $j) {
            Write-Host ("[guard] no {0} journal in {1}" -f $mode, $JournalDir)
            return 0
        }
        Write-Host ("[guard] {0}: {1}, {2} entries ({3} stored, {4} not stored, {5} never checked)" -f `
                $j.Path, $j.Kind, $j.EntryCount, $storedCount, $notStoredCount, $unknownCount)
        if ($j.HeaderSays -ge 0 -and $j.HeaderSays -ne $j.EntryCount) {
            Write-Host ("[guard] WARNING: line 1 says {0} entries and {1} were read" -f $j.HeaderSays, $j.EntryCount)
        }
        if ($j.BadLines -gt 0) {
            Write-Host ("[guard] WARNING: {0} line(s) could not be parsed and were skipped" -f $j.BadLines)
        }
        $j.Records | Format-Table -AutoSize | Out-String | Write-Host
        return 0
    }

    if (-not $j) {
        # Say WHERE it looked. "There is no journal in the folder I chose" and
        # "there is nothing stored" are different statements, and only the first one is provable here.
        Write-Host ("[guard] no {0} journal file in {1} (looked for {2} and {3})." -f $mode, $JournalDir,
            (Split-Path -Leaf $journal), (Split-Path -Leaf $legacy))
        if ($haveIni) {
            Write-Host ("[guard] {0} IS there, so this is the folder the mod uses - that collection is empty." -f $ini)
        } else {
            Write-Host "[guard] no uniquetab.ini there either, so the mod has probably never run from this folder."
            Write-Host "[guard] If you know it ran elsewhere, re-run with -OutDir <that folder> before uninstalling."
        }
        Write-Host "[guard] Nothing can be lost."
        return 0
    }
    # A journal whose line 1 says N entries but that carries no item lines at all (truncated by a
    # disk-full save, a half-copied file, a partial restore) reads as EntryCount 0 / BadLines 0,
    # which is indistinguishable from an empty collection unless the header is consulted. It is
    # DAMAGED, not empty: refuse, and name the header count.
    if ($j.EntryCount -eq 0 -and $j.BadLines -eq 0) {
        if ($j.HeaderSays -le 0) {
            Write-Host ("[guard] the {0} collection journal is empty - nothing can be lost." -f $mode)
            return 0
        }
        Write-Host ("[guard] REFUSING: {0} carries no item lines at all, but line 1 says {1} entries." -f `
                $j.Path, $j.HeaderSays)
        Write-Host "[guard] That is a TRUNCATED or half-copied journal, not an empty one - each missing"
        Write-Host "[guard] line could be a stored unique. Restore the file (or a backup) first."
        Write-Host "[guard] Override (you accept the loss):  undeploy.bat --force"
        return 1
    }
    if ($j.EntryCount -eq 0 -and $j.BadLines -gt 0) {
        Write-Host ("[guard] REFUSING: the journal holds no readable entries but {0} line(s) are damaged." -f $j.BadLines)
        Write-Host "[guard] Those lines may each be a stored unique. Fix or restore the file first."
        Write-Host "[guard] Override (you accept the loss):  undeploy.bat --force"
        return 1
    }

    # The count that decides. A journal full of history whose every entry the mod has checked and
    # found NOT on the page is not a reason to refuse; counting lines instead would raise a false
    # alarm over every item the player has ever deposited and taken back out.
    Write-Host ("[guard] {0}: {1} entries - {2} stored, {3} not stored any more, {4} never checked{5}." -f `
            $j.Path, $j.EntryCount, $storedCount, $notStoredCount, $unknownCount,
            $(if ($j.BadLines -gt 0) { ", $($j.BadLines) damaged" } else { "" }))
    # Cross-check line 1 against the lines BEFORE the exit 0.
    # The mod writes `"stored":S` in the header and the S matching `"stored":true` lines in the same
    # atomic write, so a header claiming more stored entries than the file actually carries is a
    # damaged file - exactly the case the truncation refusal above was added for, which the
    # at-risk-count shortcut would otherwise walk straight past whenever ANY line still parses.
    if ($j.HeaderStored -gt $storedCount) {
        Write-Host (("[guard] REFUSING: line 1 of {0} says {1} entr(y/ies) were stored, but only {2} " +
                "'stored':true line(s) are in the file.") -f $j.Path, $j.HeaderStored, $storedCount)
        Write-Host "[guard] The mod writes the header and the lines in one atomic write, so they cannot"
        Write-Host "[guard] disagree unless the file was truncated, half-copied or partially restored."
        Write-Host "[guard] Each missing line could be a stored unique. Restore the file (or a backup) first."
        Write-Host "[guard] Override (you accept the loss):  undeploy.bat --force"
        return 1
    }
    if ($atRiskCount -eq 0) {
        Write-Host "[guard] Every entry has been checked against the engine's own reagent map and NONE of"
        Write-Host "[guard] them is on the collection page any more. The entries are kept as the record of"
        Write-Host "[guard] what those items were; nothing can be lost by uninstalling."
        return 0
    }

    $jTime = (Get-Item -LiteralPath $j.Path).LastWriteTimeUtc
    if (Test-Path -LiteralPath $report) {
        $rTime = (Get-Item -LiteralPath $report).LastWriteTimeUtc
        if ($rTime -ge $jTime) {
            Write-Host ("[guard] {0} item(s) at risk, but rescue-report.txt is newer " +
                "({1:u} >= {2:u}) - treating the collection as already rescued." -f `
                    $atRiskCount, $rTime, $jTime)
            return 0
        }
        Write-Host ("[guard] rescue-report.txt is OLDER than the journal ({0:u} < {1:u})." -f $rTime, $jTime)
    } else {
        Write-Host ("[guard] there is no rescue-report.txt in {0} at all." -f $OutDir)
    }

    Write-Host ""
    Write-Host ("[guard] REFUSING TO UNINSTALL - the {0} collection." -f $mode)
    if ($unknownCount -gt 0 -and $storedCount -eq 0) {
        Write-Host ("[guard] {0} item(s) MAY still be stored on the collection page ({1})." -f $atRiskCount, $j.Kind)
        Write-Host "[guard] The mod has not checked them against the engine yet - that happens when you start"
        Write-Host "[guard] the game and OPEN THE CARAVAN once. Do that and run this again: entries the page"
        Write-Host "[guard] no longer holds are marked and stop counting."
    } else {
        Write-Host ("[guard] {0} item(s) are still stored on the collection page ({1}):" -f $atRiskCount, $j.Kind)
    }
    $atRisk | Select-Object -First 20 | ForEach-Object {
        Write-Host ("          " + $_.Record + $(if ($_.Item) { "   ($($_.Item))" } else { "" }) +
            $(if ($null -eq $_.Stored) { "   [never checked]" } else { "" }))
    }
    if ($atRisk.Count -gt 20) { Write-Host ("          ... and {0} more" -f ($atRisk.Count - 20)) }
    if ($j.BadLines -gt 0) {
        Write-Host ("[guard] plus {0} damaged line(s) that could each be another item." -f $j.BadLines)
    }
    Write-Host ""
    Write-Host "[guard] Removing the mod STRIPS them: at the next character load the engine hands every stored"
    Write-Host "[guard] unique back to the inventory as a STOCK copy (components, augments, rolls lost) and"
    Write-Host "[guard] re-saves at once; Steam Cloud then spreads the stripped save."
    Write-Host "[guard] Get them back first: start the game, open the caravan window, then EITHER"
    Write-Host "[guard]   a) create an empty file  $OutDir\RESCUE-NOW   (the mod deletes it and runs), or"
    Write-Host "[guard]   b) set rescue=1 in $OutDir\uniquetab.ini"
    Write-Host "[guard] and wait for $OutDir\rescue-report.txt. Then run undeploy again."
    Write-Host "[guard] (An entry can remain after the item was taken back out - the mod keeps it until the identity"
    Write-Host "[guard]  is restored - so this can fire for an EMPTY page; the rescue then restores nothing but still writes the report.)"
    Write-Host "[guard] Override (you accept the loss):  undeploy.bat --force"
    return 1

}

# ---- both collections -------------------------------------------------------------------------
# softcore first, then hardcore. Both run even when the first refuses, so one call lists
# everything at risk; the verdict is the worst of the two.
$verdict = 0
foreach ($m in @(
        @{ Mode = "softcore"; Journal = "uniq-items.jsonl"; Legacy = "uniq-items.bin" },
        @{ Mode = "hardcore"; Journal = "uniq-items-hc.jsonl"; Legacy = "uniq-items-hc.bin" })) {
    $rc = Invoke-GuardOne $m.Mode (Join-Path $JournalDir $m.Journal) (Join-Path $JournalDir $m.Legacy)
    if ($rc -ne 0) { $verdict = 1 }
}
exit $verdict