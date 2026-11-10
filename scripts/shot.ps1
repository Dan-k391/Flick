# Screenshots Flick's own window previewing a file, without Explorer or keyboard input (safe while
# someone uses the PC): runs `Flick.exe --show <file>`, captures the window with PrintWindow, closes it.
# Usage: shot.ps1 <file> [-Wait ms] [-Wheel notches] [-Click x,y]  -> build\shots\<file name>.png
# -Click: a left click at client pixel x,y (repeatable as x1,y1,x2,y2...).
param([Parameter(Mandatory)][string]$File, [int]$Wait = 1200, [int]$Wheel = 0, [int[]]$Click = @())
$ErrorActionPreference = 'Stop'
$exe = (Resolve-Path "$PSScriptRoot\..\build\Release\Flick.exe").Path
$out = Join-Path (New-Item -ItemType Directory -Force "$PSScriptRoot\..\build\shots").FullName ((Split-Path $File -Leaf) + '.png')
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices; using System.Text;
public static class Shot {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  public static IntPtr Find(uint pid) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); var s = new StringBuilder(64); GetClassName(h, s, 64);
      if (p == pid && s.ToString() == "FlickPreview") { found = h; return false; } return true; }, IntPtr.Zero);
    return found;
  }
}
'@
[void][Shot]::SetProcessDPIAware()
$p = Start-Process $exe -ArgumentList '--show', "`"$((Resolve-Path $File).Path)`"", '--mute' -PassThru
Start-Sleep -Milliseconds $Wait
$h = [Shot]::Find([uint32]$p.Id)
if ($h -eq [IntPtr]::Zero) { throw 'no Flick window' }
if ($Wheel) {  # WM_MOUSEWHEEL at the window centre; negative delta scrolls down
    $r = New-Object Shot+RECT; [void][Shot]::GetWindowRect($h, [ref]$r)
    $pos = [IntPtr]((((($r.T + $r.B) / 2) -band 0xFFFF) -shl 16) -bor ((($r.L + $r.R) / 2) -band 0xFFFF))
    for ($i = 0; $i -lt [math]::Abs($Wheel); $i++) {
        [void][Shot]::SendMessage($h, 0x20A, [IntPtr]([int](-[math]::Sign($Wheel) * 120) -shl 16), $pos)
    }
    Start-Sleep -Milliseconds 300
}
for ($i = 0; $i + 1 -lt $Click.Count; $i += 2) {
    $lp = [IntPtr](($Click[$i + 1] -shl 16) -bor $Click[$i])
    [void][Shot]::SendMessage($h, 0x201, [IntPtr]1, $lp); [void][Shot]::SendMessage($h, 0x202, [IntPtr]0, $lp)
    Start-Sleep -Milliseconds 100
}
$r = New-Object Shot+RECT; [void][Shot]::GetWindowRect($h, [ref]$r)
$bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc(); [void][Shot]::PrintWindow($h, $dc, 2); $g.ReleaseHdc($dc)
$bmp.Save($out); $g.Dispose(); $bmp.Dispose()
[void][Shot]::SendMessage($h, 0x10, [IntPtr]::Zero, [IntPtr]::Zero)  # WM_CLOSE
if (-not $p.WaitForExit(3000)) { $p.Kill() }
$out
