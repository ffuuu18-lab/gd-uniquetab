# package.ps1 - builds the release zip from a built tree.
#   powershell -ExecutionPolicy Bypass -File tools\package.ps1 [-Version 1.1.0] [-Out out\]
# Stages exactly what a player installs and zips it: x64\uniquetab.asi and the three documents.
# Nothing generated or derived from the game ships. The plugin creates x64\uniquetab\ itself and
# on the first launch generates catalogue.bin, the uniq-*.txt lists and the uniq-pages.arz
# overlay there, paints the cover plates into <game>\settings\ui\caravan\ and writes its own ini.
# data\oracle\ is the test harness's fixtures and never enters the zip.
param(
    [string]$Version = "",
    [string]$Out = ""
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $Version) {
    $hdr = Get-Content (Join-Path $root "src\ut_version.h") -Raw
    if ($hdr -match 'UT_VERSION\s+"([^"]+)"') { $Version = $Matches[1] } else { throw "UT_VERSION not found in src\ut_version.h" }
}
if (-not $Out) { $Out = Join-Path $root "out" }
$asi = Join-Path $root "bin\uniquetab.asi"
if (-not (Test-Path $asi)) { throw "bin\uniquetab.asi is missing - run build.bat first" }

$name = "UniqueCollectionTab-$Version"
$stage = Join-Path $Out $name
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force "$stage\x64" | Out-Null

Copy-Item $asi "$stage\x64\uniquetab.asi"
foreach ($doc in "README.md", "LICENSE", "THIRD_PARTY.md") {
    $p = Join-Path $root $doc
    if (Test-Path $p) { Copy-Item $p "$stage\$doc" } else { Write-Warning "$doc is missing from the tree" }
}

# What must never be in the zip: a generated output, a plate, the overlay, a fixture.
$bad = Get-ChildItem $stage -Recurse -File | Where-Object {
    $_.Extension -in ".tex", ".arz", ".arc", ".bin", ".txt", ".jsonl", ".gds", ".ini", ".stamp"
}
if ($bad) { throw ("derived files staged: " + (($bad | ForEach-Object { $_.Name }) -join ", ")) }

$zip = Join-Path $Out "$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path "$stage\*" -DestinationPath $zip
$size = (Get-Item $zip).Length
Write-Host "[package] $zip ($size bytes)"
Get-ChildItem $stage -Recurse -File | ForEach-Object {
    Write-Host ("[package]   {0,-48} {1,9}" -f $_.FullName.Substring($stage.Length + 1), $_.Length)
}
