# Screenshots Flick's own window previewing a file, without Explorer or keyboard input (safe while
# someone uses the PC): runs `Flick.exe --show <file>`, captures the window with PrintWindow, closes it.
# Usage: shot.ps1 <file> [-Wait ms] [-Wheel notches] [-Click x,y]  -> build\shots\<file name>.png
# Default: Flick paints the frame into the PNG itself (no text view: it's a child window). -Window: PrintWindow.
# -Click: a left click at client pixel x,y (repeatable as x1,y1,x2,y2...; within the double-click time: a double click).
# -Find text: opens the text search, types text. -Drag x1,y1,x2,y2: left button down, moved, up (the child window
# there gets it: the text view). -SelectAll: Ctrl+A. -Copy: Ctrl+C, then prints the clipboard text (CR LF tab
# escaped) and puts back what was there; only when the clipboard is empty or holds text only, else it skips.
param([Parameter(Mandatory)][string]$File, [int]$Wait = 1200, [int]$Wheel = 0, [int[]]$Click = @(), [int[]]$Keys = @(), [string]$Find,
      [int[]]$Drag = @(), [switch]$SelectAll, [switch]$Copy, [switch]$Window)
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
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [DllImport("user32.dll")] public static extern IntPtr ChildWindowFromPointEx(IntPtr h, POINT p, uint f);
  [DllImport("user32.dll")] public static extern int MapWindowPoints(IntPtr from, IntPtr to, ref POINT p, uint n);
  [DllImport("user32.dll")] static extern bool OpenClipboard(IntPtr h);
  [DllImport("user32.dll")] static extern bool CloseClipboard();
  [DllImport("user32.dll")] static extern bool EmptyClipboard();
  [DllImport("user32.dll")] static extern uint EnumClipboardFormats(uint f);
  // The clipboard's formats: -1 can't open, 0 empty, 1 text only (text, OEM text, Unicode text, locale), 2 anything else.
  public static int ClipboardKind() {
    if (!OpenClipboard(IntPtr.Zero)) return -1;
    int kind = 0;
    for (uint f = EnumClipboardFormats(0); f != 0; f = EnumClipboardFormats(f)) kind = Math.Max(kind, f == 1 || f == 7 || f == 13 || f == 16 ? 1 : 2);
    CloseClipboard();
    return kind;
  }
  public static void ClearClipboard() { if (OpenClipboard(IntPtr.Zero)) { EmptyClipboard(); CloseClipboard(); } }
  public static IntPtr Find(uint pid) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); var s = new StringBuilder(64); GetClassName(h, s, 64);
      if (p == pid && s.ToString() == "FlickPreview") { found = h; return false; } return true; }, IntPtr.Zero);
    return found;
  }
}
'@
[void][Shot]::SetProcessDPIAware()
$env:FLICK_SHOT = $out
$p = Start-Process $exe -ArgumentList '--show', "`"$((Resolve-Path $File).Path)`"", '--mute' -PassThru
Start-Sleep -Milliseconds $Wait
$h = [Shot]::Find([uint32]$p.Id)
if ($h -eq [IntPtr]::Zero) { throw 'no Flick window' }
if ($Wheel) {  # WM_MOUSEWHEEL at the window centre; negative delta scrolls down
    $r = New-Object Shot+RECT; [void][Shot]::GetWindowRect($h, [ref]$r)
    $pos = [IntPtr][int64](((([int](($r.T + $r.B) / 2)) -band 0xFFFF) -shl 16) -bor (([int](($r.L + $r.R) / 2)) -band 0xFFFF))
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
if ($Drag.Count -ge 4) {  # WM_LBUTTONDOWN, WM_MOUSEMOVEs in 8 steps, WM_LBUTTONUP (client pixels of the window under x1,y1)
    $at = New-Object Shot+POINT; $at.X = $Drag[0]; $at.Y = $Drag[1]
    $t = [Shot]::ChildWindowFromPointEx($h, $at, 1)
    $lp = { param($x, $y) $q = New-Object Shot+POINT; $q.X = $x; $q.Y = $y; [void][Shot]::MapWindowPoints($h, $t, [ref]$q, 1)
            [IntPtr]((($q.Y -band 0xFFFF) -shl 16) -bor ($q.X -band 0xFFFF)) }
    [void][Shot]::SendMessage($t, 0x201, [IntPtr]1, (& $lp $Drag[0] $Drag[1]))
    for ($i = 1; $i -le 8; $i++) {
        [void][Shot]::SendMessage($t, 0x200, [IntPtr]1, (& $lp ($Drag[0] + ($Drag[2] - $Drag[0]) * $i / 8) ($Drag[1] + ($Drag[3] - $Drag[1]) * $i / 8)))
        Start-Sleep -Milliseconds 40
    }
    [void][Shot]::SendMessage($t, 0x202, [IntPtr]0, (& $lp $Drag[2] $Drag[3]))
    Start-Sleep -Milliseconds 200
}
if ($SelectAll) { [void][Shot]::SendMessage($h, 0x8011, [IntPtr]::Zero, [IntPtr]::Zero); Start-Sleep -Milliseconds 200 }  # WM_FLICK_SELECTALL
# WM_COPY (what Ctrl+C does), the clipboard printed, then given back. Only text can be given back: anything else on
# the clipboard (files copied in Explorer, pictures, rich text) skips the test instead of being lost.
if ($Copy) {
    $kind = [Shot]::ClipboardKind()
    if ($kind -ne 0 -and $kind -ne 1) { 'skipped: clipboard holds non-text data (or is busy)' }
    else {
        $saved = Get-Clipboard -Raw
        [void][Shot]::SendMessage($h, 0x301, [IntPtr]::Zero, [IntPtr]::Zero)
        $got = Get-Clipboard -Raw
        if ($kind -eq 1) { Set-Clipboard -Value $saved } else { [Shot]::ClearClipboard() }
        'clipboard: ' + ("$got" -replace "`r", '\r' -replace "`n", '\n' -replace "`t", '\t')
    }
}
if ($Find) {  # WM_FLICK_FIND, then the query as WM_CHARs
    [void][Shot]::PostMessage($h, 0x800D, [IntPtr]::Zero, [IntPtr]::Zero)
    foreach ($c in $Find.ToCharArray()) { [void][Shot]::PostMessage($h, 0x102, [IntPtr][int]$c, [IntPtr]1) }
    Start-Sleep -Milliseconds 300
}
foreach ($k in $Keys) { [void][Shot]::PostMessage($h, 0x100, [IntPtr]$k, [IntPtr]1); Start-Sleep -Milliseconds 150 }  # -Keys: virtual keys
if ($Keys) { Start-Sleep -Milliseconds 400 }
if ($Window) {  # PrintWindow: includes child windows (the text view), but DWM can hand back a stale frame
    $r = New-Object Shot+RECT; [void][Shot]::GetWindowRect($h, [ref]$r)
    $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $dc = $g.GetHdc(); [void][Shot]::PrintWindow($h, $dc, 2); $g.ReleaseHdc($dc)
    $bmp.Save($out); $g.Dispose(); $bmp.Dispose()
} else { [void][Shot]::SendMessage($h, 0x800F, [IntPtr]::Zero, [IntPtr]::Zero) }  # WM_FLICK_SHOT: Paint into $env:FLICK_SHOT
[void][Shot]::SendMessage($h, 0x10, [IntPtr]::Zero, [IntPtr]::Zero)  # WM_CLOSE
if (-not $p.WaitForExit(3000)) { $p.Kill() }
$out
