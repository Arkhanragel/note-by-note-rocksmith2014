# Installs (or with -Uninstall, removes) Note-by-Note in the game folder:
#   RS_ASIO.dll          our build (upstream v0.7.5 + GuitarTap + loader for NoteByNote.dll)
#                        (the original is kept as RS_ASIO.dll.original and restored by -Uninstall)
#   NoteByNote.dll       the mod
#   NoteByNote_charts\   charts exported from YOUR songs (by ChartDump export)
#   NoteByNote.ini       created by the mod on first start (kept on uninstall, so settings survive)
# The game must be closed. avrt.dll and RS_ASIO.ini are not touched.
param([switch]$Uninstall, [switch]$SkipCharts, [string]$GameDir = "D:\SteamLibrary\steamapps\common\Rocksmith2014")
$ErrorActionPreference = "Stop"
if (Get-Process Rocksmith2014 -ErrorAction SilentlyContinue) { throw "Close Rocksmith first (the DLLs are in use)." }
$root = Split-Path $PSScriptRoot -Parent

$dll = Join-Path $GameDir "RS_ASIO.dll"
$orig = Join-Path $GameDir "RS_ASIO.dll.original"
$mod = Join-Path $GameDir "NoteByNote.dll"

if ($Uninstall) {
    if (Test-Path $orig) { Copy-Item $orig $dll -Force; Remove-Item $orig; "Restored the original RS_ASIO.dll" }
    if (Test-Path $mod) { Remove-Item $mod; "Removed NoteByNote.dll" }
    "Charts folder and NoteByNote.ini were kept (delete them by hand if you want)."
    return
}

$oursRs = Join-Path $PSScriptRoot "build\rs_asio\RS_ASIO.dll"
$oursMod = Join-Path $PSScriptRoot "build\Release\NoteByNote.dll"
foreach ($f in $oursRs, $oursMod) { if (-not (Test-Path $f)) { throw "Missing $f. Build first (mod\build_rs_asio.ps1 and mod\build.ps1)." } }

if (-not (Test-Path $orig)) { Copy-Item $dll $orig; "Saved the original as RS_ASIO.dll.original" }
Copy-Item $oursRs $dll -Force
Copy-Item $oursMod $mod -Force
"Installed RS_ASIO (v0.7.5 + Note-by-Note) and NoteByNote.dll"

if (-not $SkipCharts) {
    $exporter = Join-Path $root "tools\ChartDump\bin\Release\net10.0\ChartDump.exe"
    $charts = Join-Path $GameDir "NoteByNote_charts"
    & $exporter export (Join-Path $GameDir "dlc") (Join-Path $GameDir "songs.psarc") (Join-Path $GameDir "etudes.psarc") $charts | Select-Object -Last 1
}
