<#
    Reports which parts of the Wave Race 64: Recompiled toolchain are present.
    Installs nothing. See docs/BUILDING.md for the winget commands.
#>

$required = @(
    @{ Name = 'git';    Cmd = 'git';      Args = '--version'; Phase = '00'; Note = 'submodules' }
    @{ Name = 'cmake';  Cmd = 'cmake';    Args = '--version'; Phase = '00'; Note = '3.20 or newer' }
    @{ Name = 'ninja';  Cmd = 'ninja';    Args = '--version'; Phase = '00'; Note = 'generator' }
    @{ Name = 'clang';  Cmd = 'clang';    Args = '--version'; Phase = '00'; Note = 'preferred over MSVC, required over GCC' }
    @{ Name = 'python'; Cmd = 'python';   Args = '--version'; Phase = '01'; Note = 'splat and tools/, 3.10+' }
    @{ Name = 'mips as';Cmd = 'mips-linux-gnu-as'; Args = '--version'; Phase = '01'; Note = 'or a mips64-elf toolchain, or WSL' }
    @{ Name = 'mips ld';Cmd = 'mips-linux-gnu-ld'; Args = '--version'; Phase = '01'; Note = 'links the assembled ELF' }
)

$missing = 0

foreach ($tool in $required) {
    $found = Get-Command $tool.Cmd -ErrorAction SilentlyContinue

    # The Windows Store python stub resolves but is not Python.
    if ($found -and $tool.Cmd -eq 'python' -and $found.Source -like '*WindowsApps*') {
        $version = ''
        try { $version = (& $tool.Cmd --version 2>&1 | Out-String).Trim() } catch { }
        if ($version -notmatch 'Python \d') { $found = $null }
    }

    if ($found) {
        $version = ''
        try { $version = ((& $tool.Cmd $tool.Args 2>&1 | Select-Object -First 1) | Out-String).Trim() } catch { }
        "{0,-9} {1,-4} OK    {2}" -f $tool.Name, $tool.Phase, $version
    }
    else {
        $missing++
        "{0,-9} {1,-4} MISSING  ({2})" -f $tool.Name, $tool.Phase, $tool.Note
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
