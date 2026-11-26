# Records the README demo: an Explorer window on ~45 sample files. Space opens Flick, three files at a calm pace
# (~3 s), then the Down arrow every ~130 ms flashes through the rest (~6 s), Space closes. Real keyboard input and a
# screen recording (ffmpeg gdigrab): hands off the PC for ~20 s. Output: docs\demo.gif (and build\demo.mp4).
$ErrorActionPreference = 'Stop'
$root = Resolve-Path "$PSScriptRoot\.."
$samples = & "$PSScriptRoot\make-samples.ps1"
$dir = Join-Path $root 'build\demo'
Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $dir, (Join-Path $root 'docs') | Out-Null
# Numbered so Explorer's name order mixes the kinds: the first three are the slow part.
$kinds = 'photo-1920.jpg', 'readme.md', 'doc.pdf', 'code.cpp', 'data.csv', 'archive.zip', 'drawing.svg', 'font.ttf',
         'notebook.ipynb', 'icon-512.png', 'anim.gif', 'guide.rst', 'video.mp4', 'audio.mp3', 'folder'
$count = 45
for ($i = 0; $i -lt $count; $i++) {
    $src = Join-Path $samples $kinds[$i % $kinds.Count]
    $name = '{0:D2}-{1}' -f ($i + 1), (Split-Path $src -Leaf)
    Copy-Item -Recurse $src (Join-Path $dir $name)
}
$first = (Get-ChildItem $dir | Sort-Object Name | Select-Object -First 1).Name

Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class D {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int hh, bool r);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
}
'@
[void][D]::SetProcessDPIAware()
$screen = [System.Windows.Forms.Screen]::PrimaryScreen
$wa = $screen.WorkingArea  # physical pixels (DPI aware): the screen without the taskbar

$exe = Join-Path $root 'build\Release\Flick.exe'
# A fresh Flick: Windows silently drops a low-level keyboard hook that was once too slow, after which Space does nothing.
if (Get-Process Flick -ErrorAction SilentlyContinue) { & $exe --exit; Start-Sleep -Milliseconds 800 }
Start-Process $exe
Start-Sleep -Milliseconds 1500

# A plain backdrop over the desktop and other windows, so only Explorer and Flick are in the recording.
$back = New-Object System.Windows.Forms.Form
$back.FormBorderStyle = 'None'; $back.StartPosition = 'Manual'; $back.ShowInTaskbar = $false
$back.Bounds = $wa; $back.BackColor = [System.Drawing.Color]::FromArgb(32, 39, 56)
$back.Show(); [System.Windows.Forms.Application]::DoEvents()

$shell = New-Object -ComObject Shell.Application
$url = ([Uri]$dir).AbsoluteUri
$shell.Windows() | Where-Object { $_.LocationURL -eq $url } | ForEach-Object { $_.Quit() }  # left from an earlier take
Start-Sleep -Milliseconds 300
Start-Process explorer.exe "`"$dir`""
$win = $null
for ($i = 0; $i -lt 50 -and -not $win; $i++) {
    Start-Sleep -Milliseconds 100
    $win = $shell.Windows() | Where-Object { $_.LocationURL -eq $url } | Select-Object -First 1
}
if (-not $win) { throw 'no Explorer window' }
$hwnd = [IntPtr]$win.HWND
[void][D]::ShowWindow($hwnd, 1)
$w = [int]($wa.Width * 0.62); $h = [int]($wa.Height * 0.62)
[void][D]::MoveWindow($hwnd, $wa.X + ($wa.Width - $w) / 2, $wa.Y + ($wa.Height - $h) / 2, $w, $h, $true)
[void][D]::SetForegroundWindow($hwnd)
$win.Document.SelectItem($win.Document.Folder.ParseName($first), 29)
Start-Sleep -Milliseconds 2500  # thumbnails, details

$mp4 = Join-Path $root 'build\demo.mp4'
$fast = $count - 3
$seconds = 0.6 + 3 * 1.0 + $fast * 0.13 + 0.5 + 0.6  # the key presses below, then the closed window
$ff = Start-Process ffmpeg -PassThru -WindowStyle Hidden -ArgumentList `
    '-y', '-loglevel', 'error', '-f', 'gdigrab', '-t', $seconds, '-framerate', '30', '-draw_mouse', '0', '-offset_x', $wa.X, '-offset_y', $wa.Y,
    '-video_size', "$($wa.Width)x$($wa.Height)", '-i', 'desktop', '-c:v', 'libx264', '-preset', 'ultrafast', '-crf', '12', '-pix_fmt', 'yuv420p',  # 4:4:4 doesn't play on Windows "`"$mp4`""
Start-Sleep -Milliseconds 600

[void][D]::SetForegroundWindow($hwnd)
[System.Windows.Forms.SendKeys]::SendWait(' ')
Start-Sleep -Milliseconds 1000
foreach ($i in 1..2) { [System.Windows.Forms.SendKeys]::SendWait('{DOWN}'); Start-Sleep -Milliseconds 1000 }
foreach ($i in 1..$fast) { [System.Windows.Forms.SendKeys]::SendWait('{DOWN}'); Start-Sleep -Milliseconds 130 }
Start-Sleep -Milliseconds 500
[System.Windows.Forms.SendKeys]::SendWait(' ')
$ff.WaitForExit()

# GIF: 25 fps (the fast part shows each file for ~3 frames), 1100 px wide, one palette for the whole clip (diff stats keep text crisp).
$back.Close()
$win.Quit()
$gif = Join-Path $root 'docs\demo.gif'
# Cropped to the middle of the screen: Explorer and the largest preview, not the taskbar.
$cw = [int]($wa.Width * 0.94) -band -2; $ch = [int]($wa.Height * 0.84) -band -2
$crop = "crop=${cw}:${ch}:$([int](($wa.Width - $cw) / 2)):$([int]($wa.Height * 0.075))"
ffmpeg -y -loglevel error -i $mp4 -vf "$crop,fps=25,scale=1100:-1:flags=lanczos,split[a][b];[a]palettegen=stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=5:diff_mode=rectangle" $gif
$gif
