# Installs (or with -Uninstall, removes) our RS_ASIO build with the Note-by-Note GuitarTap.
# The game must be closed. The original DLL is kept as RS_ASIO.dll.original and restored by -Uninstall.
# Only RS_ASIO.dll changes. avrt.dll (the loader) and RS_ASIO.ini stay as they are.
param([switch]$Uninstall, [string]$GameDir = "D:\SteamLibrary\steamapps\common\Rocksmith2014")
$ErrorActionPreference = "Stop"
if (Get-Process Rocksmith2014 -ErrorAction SilentlyContinue) { throw "Close Rocksmith first (the DLL is in use)." }

$dll = Join-Path $GameDir "RS_ASIO.dll"
$orig = Join-Path $GameDir "RS_ASIO.dll.original"

if ($Uninstall) {
    if (-not (Test-Path $orig)) { throw "No RS_ASIO.dll.original found; nothing to restore." }
    Copy-Item $orig $dll -Force
    Remove-Item $orig
    "Restored the original RS_ASIO.dll"
    return
}

$ours = Join-Path $PSScriptRoot "build\rs_asio\RS_ASIO.dll"
if (-not (Test-Path $ours)) { throw "Build it first: mod\build_rs_asio.ps1" }
if (-not (Test-Path $orig)) { Copy-Item $dll $orig; "Saved the original as RS_ASIO.dll.original" }
Copy-Item $ours $dll -Force
"Installed the Note-by-Note RS_ASIO build. The RS_ASIO log will say 'v0.7.5 + Note-by-Note GuitarTap'."
