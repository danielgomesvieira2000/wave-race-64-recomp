<#
    Reports which parts of the Wave Race 64: Recompiled toolchain are present.
    Installs nothing. See docs/BUILDING.md for the install commands.

    The MIPS tools are not expected on the Windows PATH: on Windows they live
    inside WSL, so this script looks for them there.
#>

$WslDistro = 'Ubuntu'

$tools = @(
    @{ Name = 'git';     Cmd = 'git';    Args = '--version'; Phase = '00'; Where = 'win'; Note = 'submodules' }
    @{ Name = 'cmake';   Cmd = 'cmake';  Args = '--version'; Phase = '00'; Where = 'win'; Note = '3.20 or newer' }
    @{ Name = 'ninja';   Cmd = 'ninja';  Args = '--version'; Phase = '00'; Where = 'win'; Note = 'generator' }
    @{ Name = 'clang';   Cmd = 'clang';  Args = '--version'; Phase = '00'; Where = 'win'; Note = 'preferred over MSVC, required over GCC' }
    @{ Name = 'python';  Cmd = 'python'; Args = '--version'; Phase = '01'; Where = 'win'; Note = 'splat and tools/, 3.10+' }
    @{ Name = 'mips as'; Cmd = 'mips-linux-gnu-as'; Args = '--version'; Phase = '01'; Where = 'wsl'; Note = 'apt install binutils-mips-linux-gnu' }
    @{ Name = 'mips ld'; Cmd = 'mips-linux-gnu-ld'; Args = '--version'; Phase = '01'; Where = 'wsl'; Note = 'ships with binutils-mips-linux-gnu' }
)

function Get-WindowsTool($tool) {
    $found = Get-Command $tool.Cmd -ErrorAction SilentlyContinue
    if (-not $found) { return $null }

    # The Windows Store python stub resolves but is not Python.
    if ($tool.Cmd -eq 'python' -and $found.Source -like '*WindowsApps*') {
        $probe = ''
        try { $probe = (& $tool.Cmd --version 2>&1 | Out-String).Trim() } catch { }
        if ($probe -notmatch 'Python \d') { return $null }
    }

    try { return ((& $tool.Cmd $tool.Args 2>&1 | Select-Object -First 1) | Out-String).Trim() }
    catch { return '' }
}

function Get-WslTool($tool) {
    if (-not (Get-Command wsl -ErrorAction SilentlyContinue)) { return $null }
    $probe = (wsl -d $WslDistro -- $tool.Cmd $tool.Args 2>&1 | Out-String) -replace "`0", ''
    if ($LASTEXITCODE -ne 0) { return $null }
    return ($probe -split "`n" | Select-Object -First 1).Trim()
}

$missing = 0

foreach ($tool in $tools) {
    $version = if ($tool.Where -eq 'wsl') { Get-WslTool $tool } else { Get-WindowsTool $tool }
    $label = if ($tool.Where -eq 'wsl') { "wsl:$WslDistro" } else { 'windows' }

    if ($null -ne $version) {
        "{0,-9} {1,-4} {2,-12} OK    {3}" -f $tool.Name, $tool.Phase, $label, $version
    }
    else {
        $missing++
        "{0,-9} {1,-4} {2,-12} MISSING  ({3})" -f $tool.Name, $tool.Phase, $label, $tool.Note
    }
}

''
if ($missing -eq 0) {
    'Toolchain complete.'
}
else {
    "$missing tool(s) missing. Phase 00 needs the ones marked 00; phase 01 needs the rest."
    'Install commands are in docs/BUILDING.md.'
}
