# Phase 05 verification: photograph the running port at intervals.
#
# The state transcript in the log says which screen the game thinks it is on.
# It cannot say whether that screen is drawn correctly, and the two failures are
# different: a menu that advances with nothing on it is a renderer problem, not
# a game-logic one. This grabs the window's pixels so the answer is visible.
#
# It captures the desktop region the window occupies rather than asking the
# window to redraw itself: RT64 presents through D3D12, so PrintWindow returns
# an empty bitmap. That means the window has to be unobscured, which is the one
# thing this script cannot check for you.
#
#   powershell -File tools/capture_window.ps1 -OutDir shots -Count 12 -IntervalSeconds 5

param(
    [string]$Title = "Wave Race 64: Recompiled",
    [string]$OutDir = "shots",
    [int]$Count = 10,
    [double]$IntervalSeconds = 5.0,
    [double]$StartDelaySeconds = 0.0
)

# Capture in physical pixels. Without this, on a display running at a scale
# other than 100% (a 1920x1080 panel at 125% reports itself as 1536x864), the
# capture is the top-left 1536x864 *physical* pixels of the screen -- a crop
# that looks like the picture is zoomed in on its top-left corner. Days were
# nearly lost to that.
Add-Type -TypeDefinition @"
using System.Runtime.InteropServices;
public static class Dpi { [DllImport("user32.dll")] public static extern bool SetProcessDPIAware(); }
"@
[void][Dpi]::SetProcessDPIAware()

Add-Type -AssemblyName System.Drawing

# The window is found by enumerating top-level windows rather than with
# FindWindow. FindWindow matched nothing here even while enumeration reported
# the exact title, and a capture that silently finds no window is worse than a
# slightly longer search.
Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class Win32Window {
    delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT p);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    public static IntPtr ByTitle(string wanted) {
        IntPtr result = IntPtr.Zero;
        EnumWindows(delegate(IntPtr h, IntPtr l) {
            if (!IsWindowVisible(h)) { return true; }
            StringBuilder sb = new StringBuilder(512);
            GetWindowText(h, sb, 512);
            if (sb.ToString() == wanted) { result = h; return false; }
            return true;
        }, IntPtr.Zero);
        return result;
    }
}
"@

if ($StartDelaySeconds -gt 0) { Start-Sleep -Seconds $StartDelaySeconds }

# The window does not exist the moment the process starts: RT64 sets up a
# device and compiles shaders first, which on a laptop GPU takes several
# seconds. Waiting for it beats guessing a delay and failing intermittently.
$handle = [IntPtr]::Zero
$deadline = (Get-Date).AddSeconds(60)
while ($handle -eq [IntPtr]::Zero -and (Get-Date) -lt $deadline) {
    $handle = [Win32Window]::ByTitle($Title)
    if ($handle -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 250 }
}
if ($handle -eq [IntPtr]::Zero) {
    Write-Error "no window titled '$Title' appeared within 60s -- is the port running?"
    exit 1
}

# Bring it up front once. Anything covering the window would otherwise be
# captured instead of the game, silently.
[void][Win32Window]::SetForegroundWindow($handle)

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

for ($i = 0; $i -lt $Count; $i++) {
    # Raised before every shot, not once: this captures the desktop where the
    # window sits, so anything that comes to the front in between is what would
    # be photographed instead of the game.
    [void][Win32Window]::SetForegroundWindow($handle)
    Start-Sleep -Milliseconds 150

    $rect = New-Object Win32Window+RECT
    if (-not [Win32Window]::GetClientRect($handle, [ref]$rect)) { break }

    $origin = New-Object Win32Window+POINT
    $origin.X = 0; $origin.Y = 0
    [void][Win32Window]::ClientToScreen($handle, [ref]$origin)

    $width = $rect.R - $rect.L
    $height = $rect.B - $rect.T
    if ($width -le 0 -or $height -le 0) { break }

    $bitmap = New-Object System.Drawing.Bitmap $width, $height
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.CopyFromScreen($origin.X, $origin.Y, 0, 0, $bitmap.Size)

    $name = Join-Path $OutDir ("shot_{0:d3}.png" -f $i)
    $bitmap.Save($name, [System.Drawing.Imaging.ImageFormat]::Png)
    $graphics.Dispose()
    $bitmap.Dispose()

    Write-Output ("{0}  ({1}x{2})" -f $name, $width, $height)
    if ($i -lt $Count - 1) { Start-Sleep -Milliseconds ([int]($IntervalSeconds * 1000)) }
}
