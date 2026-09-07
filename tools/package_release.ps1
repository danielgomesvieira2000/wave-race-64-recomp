# Stage a built tree into a release folder and zip it.
#
#   powershell -ExecutionPolicy Bypass -File tools/package_release.ps1 -BuildDir build-fe -Version 0.1.0
#
# What goes in: the executable, the three DLLs it cannot start without (SDL2,
# dxcompiler, dxil), assets/ (menus, replacement music, HD textures), this project's
# LICENSE, the third-party notices and the README. The debug symbols go into a
# second zip: at 80 MB they are five times the rest of the package, and only a
# crash report needs them. Nothing here is a ROM, and the script refuses to
# continue if it finds one where it is staging.
#
# What the executable is. It contains the game's code, statically recompiled
# from the cartridge into C and compiled into the binary; that is what a
# recompiled port is. Original cartridge data is imported from the player's
# own dump at run time. Reviewed replacement music and textures are distributed
# separately in assets/ with their credits; see the README's Licensing section.

param(
    [string]$BuildDir = "build-fe",
    [string]$Version = "0.1.0",
    [string]$OutDir = "dist"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root

$exe = Join-Path $BuildDir "WaveRace64Recomp.exe"
if (-not (Test-Path $exe)) {
    throw "no executable at $exe -- build first (see docs/BUILDING.md)"
}

# Python 3.10+ is already required by the source-build toolchain. Validate the
# built assets against this checkout before creating or replacing a release.
& python "tools/bundled_assets.py" --assets (Join-Path $BuildDir "assets") --reference "assets"
if ($LASTEXITCODE -ne 0) { throw "built music/texture assets are missing, corrupt, or stale; rebuild first" }

$name = "WaveRace64Recomp-$Version-windows-x64"
$stage = Join-Path $OutDir $name
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null

$files = @(
    "$BuildDir\WaveRace64Recomp.exe",
    "$BuildDir\SDL2.dll",
    "$BuildDir\dxcompiler.dll",
    "$BuildDir\dxil.dll",
    "LICENSE",
    "THIRD_PARTY_NOTICES.md",
    "README.md"
)
foreach ($f in $files) {
    if (-not (Test-Path $f)) { throw "missing $f" }
    Copy-Item $f $stage
}
Copy-Item -Recurse "$BuildDir\assets" (Join-Path $stage "assets")
& python "tools/bundled_assets.py" --assets (Join-Path $stage "assets") --reference "assets"
if ($LASTEXITCODE -ne 0) { throw "staged music/texture assets failed validation" }

# Belt and braces: the staging folder must contain no dump and no save.
$forbidden = Get-ChildItem -Recurse $stage | Where-Object {
    $_.Extension -in ".z64", ".n64", ".v64", ".rom", ".bin", ".eep", ".sra", ".fla"
}
if ($forbidden) {
    $forbidden | ForEach-Object { Write-Error "refusing to package $($_.FullName)" }
    throw "game data found in the staging folder"
}

$zip = Join-Path $OutDir "$name.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip

$pdb = Join-Path $BuildDir "WaveRace64Recomp.pdb"
if (Test-Path $pdb) {
    $symbols = Join-Path $OutDir "$name-debug-symbols.zip"
    if (Test-Path $symbols) { Remove-Item $symbols }
    Compress-Archive -Path $pdb -DestinationPath $symbols
    Write-Host "symbols: $symbols"
}

Write-Host "package: $zip"
$stageFull = (Resolve-Path $stage).Path
Get-ChildItem -Recurse $stage | Where-Object { -not $_.PSIsContainer } |
    ForEach-Object { "{0,10:N0}  {1}" -f $_.Length, $_.FullName.Substring($stageFull.Length + 1) }
