# Builds a release of Note-by-Note: dist\NoteByNote-<version>.zip with
#   Note-by-Note Setup.exe   the installer (the mod's files are embedded in it)
#   README.txt, LICENSES.txt
# Steps: build the mod and RS_ASIO (+ avrt), stage the files the setup embeds in dist\payload,
# build the setup (installer\), zip. Only our own builds and licence texts go in: no game files,
# song data or charts.
#   .\package.ps1 -Version 0.1.0 [-SkipBuild]   (-SkipBuild: use the DLLs already in mod\build)
param([Parameter(Mandatory = $true)][string]$Version, [switch]$SkipBuild)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$dist = Join-Path $root "dist"
$payload = Join-Path $dist "payload"

if (-not $SkipBuild) {
    & (Join-Path $root "mod\build.ps1")
    & (Join-Path $root "mod\build_rs_asio.ps1")
}

# The files the setup installs, and the documents that go with it.
Remove-Item $payload -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $payload | Out-Null
$files = @{
    "NoteByNote.dll" = "mod\build\Release\NoteByNote.dll"
    "RS_ASIO.dll"    = "mod\build\rs_asio\RS_ASIO.dll"
    "avrt.dll"       = "mod\build\rs_asio\avrt.dll"
    "RS_ASIO.ini"    = "external\rs_asio\dist\RS_ASIO.ini"   # RS_ASIO's default settings
    "README.txt"     = "installer\README.txt"
}
foreach ($f in $files.GetEnumerator()) {
    $src = Join-Path $root $f.Value
    if (-not (Test-Path $src)) { throw "Missing $src (build first)." }
    Copy-Item $src (Join-Path $payload $f.Key)
}
# LICENSES.txt: Note-by-Note's own licence, then the third-party code inside the DLLs.
$lic = @(
    @("Note-by-Note (NoteByNote.dll, the GuitarTap part of RS_ASIO.dll, the setup)", "LICENSE"),
    @("RS_ASIO (RS_ASIO.dll, avrt.dll) - https://github.com/mdias/rs_asio", "external\rs_asio\LICENSE"),
    @("MinHook (in NoteByNote.dll) - https://github.com/TsudaKageyu/minhook", "external\minhook\LICENSE.txt"),
    @("Dear ImGui (in NoteByNote.dll) - https://github.com/ocornut/imgui", "external\imgui\LICENSE.txt"))
$text = "Licences of Note-by-Note and of the third-party software it uses`r`n`r`n"
foreach ($l in $lic) {
    $text += ("=" * 78) + "`r`n" + $l[0] + "`r`n" + ("=" * 78) + "`r`n`r`n"
    $text += (Get-Content (Join-Path $root $l[1]) -Raw) + "`r`n`r`n"
}
Set-Content (Join-Path $payload "LICENSES.txt") $text -Encoding utf8

# The setup (single exe, .NET Framework 4.8).
$out = Join-Path $dist "setup_build"
dotnet build (Join-Path $root "installer\NoteByNoteSetup.csproj") -c Release -o $out -nologo -v:minimal `
    "-p:Version=$Version" "-p:PayloadDir=$payload"
if ($LASTEXITCODE -ne 0) { throw "Setup build failed" }

# The zip.
$stage = Join-Path $dist "NoteByNote-$Version"
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item (Join-Path $out "NoteByNoteSetup.exe") (Join-Path $stage "Note-by-Note Setup.exe")
Copy-Item (Join-Path $payload "README.txt"), (Join-Path $payload "LICENSES.txt") $stage
$zip = Join-Path $dist "NoteByNote-$Version.zip"
Remove-Item $zip -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip
"Release: $zip"
Get-ChildItem $stage | Select-Object Name, Length | Format-Table -AutoSize | Out-String
