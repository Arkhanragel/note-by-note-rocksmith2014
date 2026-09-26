# Builds the native part (probe DLL + injector) for Win32, the game's 32-bit architecture.
# Output: mod\build\Release\NoteByNoteProbe.dll and nbn_inject.exe
$ErrorActionPreference = "Stop"
$vs = & "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$src = $PSScriptRoot
$build = Join-Path $PSScriptRoot "build"

& $cmake -S $src -B $build -G "Visual Studio 17 2022" -A Win32 | Out-Null
& $cmake --build $build --config Release
if ($LASTEXITCODE -ne 0) { throw "Build failed" }
Write-Host "`nBuilt:" -ForegroundColor Green
Get-ChildItem "$build\Release\*.dll", "$build\Release\*.exe" | ForEach-Object { "  " + $_.FullName }
