# package.ps1 - builds the release zip from a built tree.
#   powershell -ExecutionPolicy Bypass -File tools\package.ps1 [-Version 1.0.0] [-Out out\]
# Stages exactly what a player installs and zips it.
# Nothing generated ships: the plugin builds catalogue.bin and the uniq-*.txt files itself on
# the first launch and writes its own ini.
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
New-Item -ItemType Directory -Force "$stage\x64\uniquetab" | Out-Null
New-Item -ItemType Directory -Force "$stage\settings\ui\caravan" | Out-Null

Copy-Item $asi "$stage\x64\uniquetab.asi"
Copy-Item (Join-Path $root "data\uniq\uniq-pages.arz")   "$stage\x64\uniquetab\"
Copy-Item (Join-Path $root "data\plates\uniq_plate_*.tex") "$stage\settings\ui\caravan\"
foreach ($doc in "README.md", "LICENSE", "THIRD_PARTY.md") {
    $p = Join-Path $root $doc
    if (Test-Path $p) { Copy-Item $p "$stage\$doc" } else { Write-Warning "$doc is missing from the tree" }
}

$zip = Join-Path $Out "$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path "$stage\*" -DestinationPath $zip
$size = (Get-Item $zip).Length
Write-Host "[package] $zip ($size bytes)"
Get-ChildItem $stage -Recurse -File | ForEach-Object {
    Write-Host ("[package]   {0,-48} {1,9}" -f $_.FullName.Substring($stage.Length + 1), $_.Length)
}
