# Creates benchmark sample files in build\samples (idempotent).
$ErrorActionPreference = 'Stop'
$dir = Join-Path $PSScriptRoot '..\build\samples'
New-Item -ItemType Directory -Force $dir | Out-Null
$dir = (Resolve-Path $dir).Path
Add-Type -AssemblyName System.Drawing

function New-Image($path, $w, $h, $format) {
    if (Test-Path $path) { return }
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $rect = New-Object System.Drawing.Rectangle 0, 0, $w, $h
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush $rect, ([System.Drawing.Color]::SteelBlue), ([System.Drawing.Color]::Orange), 45
    $g.FillRectangle($brush, $rect)
    $rng = New-Object System.Random 1
    for ($i = 0; $i -lt 400; $i++) {
        $c = [System.Drawing.Color]::FromArgb(120, $rng.Next(256), $rng.Next(256), $rng.Next(256))
        $g.FillEllipse((New-Object System.Drawing.SolidBrush $c), $rng.Next($w), $rng.Next($h), $w / 10, $h / 10)
    }
    $bmp.Save($path, $format)
    $g.Dispose(); $bmp.Dispose()
}

New-Image "$dir\photo-1920.jpg" 1920 1080 ([System.Drawing.Imaging.ImageFormat]::Jpeg)
New-Image "$dir\photo-12mp.jpg" 4000 3000 ([System.Drawing.Imaging.ImageFormat]::Jpeg)
New-Image "$dir\photo-24mp.jpg" 6000 4000 ([System.Drawing.Imaging.ImageFormat]::Jpeg)
New-Image "$dir\icon-512.png" 512 512 ([System.Drawing.Imaging.ImageFormat]::Png)

Copy-Item "$PSScriptRoot\..\src\window.cpp" "$dir\code.cpp" -Force
if (-not (Test-Path "$dir\big.log")) {
    $line = '2026-11-10 12:00:00.000 INFO  [worker-7] request handled in 12 ms path=/api/v1/items?id=42'
    [IO.File]::WriteAllText("$dir\big.log", (($line + "`n") * 60000))  # ~5.6 MB, exceeds the 2 MB text cap
}
if (-not (Test-Path "$dir\data.bin")) { [IO.File]::WriteAllBytes("$dir\data.bin", [byte[]]::new(100000)) }
New-Item -ItemType Directory -Force "$dir\folder" | Out-Null
1..50 | ForEach-Object { Set-Content "$dir\folder\file$_.txt" "hello $_" }
Write-Output $dir
