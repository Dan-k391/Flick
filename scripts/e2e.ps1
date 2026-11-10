# End-to-end test through real Explorer: opens one Explorer window on the samples folder, selects each
# sample, presses Space (real keyboard input), optionally screenshots the preview, presses Esc.
# Timings come from Flick's log: key press seen by the hook -> frame composed by DWM, so they include
# the Explorer COM round trip. Usage: e2e.ps1 [-Shots] [-Repeat N] [-Names a,b,c]
param([switch]$Shots, [int]$Repeat = 3, [string[]]$Names)
$ErrorActionPreference = 'Stop'
$exe = (Resolve-Path "$PSScriptRoot\..\build\Release\Flick.exe").Path
$dir = & "$PSScriptRoot\make-samples.ps1"
if (-not $Names) { $Names = 'code.cpp', 'big.log', 'photo-1920.jpg', 'photo-12mp.jpg', 'photo-24mp.jpg', 'icon-512.png', 'readme.md', 'doc.pdf', 'data.csv', 'archive.zip', 'archive.tar.gz', 'data.bin', 'folder' }
$shotDir = Join-Path $PSScriptRoot '..\build\shots'
New-Item -ItemType Directory -Force $shotDir | Out-Null

Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class W {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int a, out RECT r, int n);
}
'@

if (-not (Get-Process Flick -ErrorAction SilentlyContinue)) { Start-Process $exe; Start-Sleep -Milliseconds 800 }
$flick = [W]::FindWindow('FlickPreview', 'Flick')
$log = Join-Path $env:LOCALAPPDATA 'Flick\flick.log'
$before = if (Test-Path $log) { (Get-Content $log).Count } else { 0 }

$shell = New-Object -ComObject Shell.Application
$url = ([Uri]$dir).AbsoluteUri
Start-Process explorer.exe "`"$dir`""
$win = $null
for ($i = 0; $i -lt 50 -and -not $win; $i++) {
    Start-Sleep -Milliseconds 100
    $win = $shell.Windows() | Where-Object { $_.LocationURL -eq $url } | Select-Object -First 1
}
Start-Sleep -Milliseconds 1500  # let Explorer settle (thumbnails etc.)
$hwnd = [IntPtr]$win.HWND

for ($r = 0; $r -lt $Repeat; $r++) {
    foreach ($name in $Names) {
        [void][W]::SetForegroundWindow($hwnd)
        $win.Document.SelectItem($win.Document.Folder.ParseName($name), 29)  # select+deselect others+focus+visible
        Start-Sleep -Milliseconds 400
        [System.Windows.Forms.SendKeys]::SendWait(' ')
        Start-Sleep -Milliseconds 500
        if (-not [W]::IsWindowVisible($flick)) { Write-Warning "no preview for $name" }
        elseif ($Shots -and $r -eq 0) {
            $rc = New-Object W+RECT
            [void][W]::DwmGetWindowAttribute($flick, 9, [ref]$rc, 16)  # DWMWA_EXTENDED_FRAME_BOUNDS
            $bmp = New-Object System.Drawing.Bitmap ($rc.R - $rc.L), ($rc.B - $rc.T)
            $g = [System.Drawing.Graphics]::FromImage($bmp)
            $g.CopyFromScreen($rc.L, $rc.T, 0, 0, $bmp.Size)
            $bmp.Save((Join-Path $shotDir "$name.png"))
            $g.Dispose(); $bmp.Dispose()
        }
        [System.Windows.Forms.SendKeys]::SendWait('{ESC}')
        Start-Sleep -Milliseconds 300
    }
}
$win.Quit()
Get-Content $log | Select-Object -Skip $before | Where-Object { $_ -like 'open*' }
$p = Get-Process Flick
'Flick memory: working set {0:N1} MB, private {1:N1} MB' -f ($p.WorkingSet64 / 1MB), ($p.PrivateMemorySize64 / 1MB)
