# Builds our RS_ASIO: upstream v0.7.5 (git submodule in external/rs_asio) + the Note-by-Note GuitarTap.
#   1. reset the submodule to a clean v0.7.5
#   2. apply mod/rs_asio_tap/rs_asio_v0.7.5_guitartap.patch (a few small edits: one call + project entries)
#   3. copy GuitarTap.cpp/.h and GuitarTapShared.h next to the RS_ASIO sources
#   4. build Release|Win32 with the installed Visual Studio (v143 toolset instead of upstream's v142)
# Output: mod\build\rs_asio\RS_ASIO.dll   (install it with mod\install_rs_asio.ps1)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$sub = Join-Path $root "external\rs_asio"
$tap = Join-Path $PSScriptRoot "rs_asio_tap"

git -C $sub checkout -q -- .
git -C $sub clean -qfd RS_ASIO
git -C $sub apply (Join-Path $tap "rs_asio_v0.7.5_guitartap.patch")
Copy-Item (Join-Path $tap "GuitarTap.cpp"), (Join-Path $tap "GuitarTap.h"), (Join-Path $PSScriptRoot "common\GuitarTapShared.h") (Join-Path $sub "RS_ASIO")

$vs = & "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$msbuild = Join-Path $vs "MSBuild\Current\Bin\MSBuild.exe"
$out = Join-Path $PSScriptRoot "build\rs_asio\"
$int = Join-Path $PSScriptRoot "build\rs_asio_obj\"
# (paths end with "\" as MSBuild wants; they're passed as whole "/p:X=..." arguments so no quoting breaks)
$msArgs = @((Join-Path $sub "RS_ASIO\RS_ASIO.vcxproj"), "/nologo", "/v:minimal", "/p:Configuration=Release",
            "/p:Platform=Win32", "/p:PlatformToolset=v143", "/p:OutDir=$out", "/p:IntDir=$int")
& $msbuild @msArgs
if ($LASTEXITCODE -ne 0) { throw "RS_ASIO build failed" }
Get-ChildItem (Join-Path $out "*.dll") | ForEach-Object { "Built: " + $_.FullName }
