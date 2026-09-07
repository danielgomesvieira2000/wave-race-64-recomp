# Build the native Windows game with the launcher, HD textures, music and water.
# Generate game sources first using the WSL toolchain in docs/BUILDING.md.
param(
    [string]$BuildDir = 'build-windows',
    [int]$Jobs = 12,
    [string]$VisualStudioPath = '',
    [switch]$Package,
    [string]$Version = '0.4.0'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    if (-not (Test-Path 'RecompiledFuncs/aspMain_rsp.cpp')) {
        throw 'Generate the game AND audio sources from your USA Rev A ROM first. See docs/BUILDING.md.'
    }
    if (-not $VisualStudioPath) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
        if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio C++ Build Tools are required.' }
        # Prefer the validated VS 2022 toolset when multiple releases coexist.
        $candidates = @(& $vswhere -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
        if (-not $candidates.Count) {
            $candidates = @(& $vswhere -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
        }
        $VisualStudioPath = $candidates | Where-Object { Test-Path (Join-Path $_ 'VC/Tools/Llvm/x64/bin/clang-cl.exe') } | Select-Object -First 1
        if (-not $VisualStudioPath -and $candidates.Count) { $VisualStudioPath = $candidates[0] }
        if (-not $VisualStudioPath) { throw 'Install the Visual Studio Desktop development with C++ workload.' }
    }
    # Quoted PATH entries otherwise break the developer shell's batch setup.
    $env:PATH = ($env:PATH -split ';' | ForEach-Object { $_.Trim('"') }) -join ';'
    Import-Module (Join-Path $VisualStudioPath 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $VisualStudioPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
    $clangDir = Join-Path $VisualStudioPath 'VC/Tools/Llvm/x64/bin'
    if (Test-Path (Join-Path $clangDir 'clang-cl.exe')) {
        $env:PATH = "$clangDir;$env:PATH"
    } elseif (Test-Path 'C:/Program Files/LLVM/bin/clang-cl.exe') {
        $env:PATH = "C:\Program Files\LLVM\bin;$env:PATH"
    }
    foreach ($tool in @('clang-cl', 'cmake', 'ninja', 'python')) {
        if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "Required tool not found: $tool" }
    }
    foreach ($patch in @('patch_rt64.py', 'patch_n64recomp.py', 'patch_rsprecomp.py',
                         'patch_librecomp.py', 'patch_water.py', 'patch_runtime_shutdown.py', 'patch_texture_packs.py')) {
        & python (Join-Path 'tools' $patch)
        if ($LASTEXITCODE -ne 0) { throw "Patch failed: $patch" }
    }
    & cmake -S . -B $BuildDir -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl `
        -DCMAKE_BUILD_TYPE=RelWithDebInfo -DWR64_WITH_RUNTIME=ON -DWR64_WITH_RECOMPILED=ON -DWR64_WITH_FRONTEND=ON
    if ($LASTEXITCODE -ne 0) { throw 'Windows CMake configuration failed.' }
    & cmake --build $BuildDir --target WaveRace64Recomp -j $Jobs
    if ($LASTEXITCODE -ne 0) { throw 'Windows build failed.' }
    & python tools/bundled_assets.py --assets (Join-Path $BuildDir 'assets') --reference assets
    if ($LASTEXITCODE -ne 0) { throw 'Built assets failed verification.' }
    if ($Package) {
        & "$PSScriptRoot/package_release.ps1" -BuildDir $BuildDir -Version $Version
    }
    Write-Host "Built: $(Join-Path $BuildDir 'WaveRace64Recomp.exe')"
} finally {
    Pop-Location
}
