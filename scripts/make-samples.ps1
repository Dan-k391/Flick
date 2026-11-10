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
Copy-Item "$PSScriptRoot\..\AGENTS.md" "$dir\readme.md" -Force
Set-Content -Encoding utf8 "$dir\features.md" @'
# Markdown features

Some **bold**, *italic*, ***both***, ~~struck~~, `inline code` and a [link](https://example.com).
Entities: &copy; &amp; &#x2192; and a hard line break here\
on the next line.

## Lists

- First item with `code`
- Second item
  - Nested bullet
    - Third level
1. Ordered one
2. Ordered two
- [x] Done task
- [ ] Open task

> A quote with **emphasis**.
> > Nested quote.

### Table

| Format | Engine | Open time |
|:-------|:------:|----------:|
| Markdown | md4c + DirectWrite | fast |
| PDF | Windows.Data.Pdf | async pages |

```cpp
#include <cstdio>
int main() {
    printf("%d\n", 42);  // a fenced code block
}
```

```python
def greet(name: str) -> str:
    return f"Hello, {name}!"  # coloured too
```

---

![missing picture](logo.png) shows its alt text. <b>Inline HTML</b> shows its text.

![a drawing](drawing.svg)

![badge](https://img.shields.io/badge/remote-alt%20text-blue)
'@
# A 30-page Letter PDF with text and a coloured bar on every page (ASCII, so offsets are char counts).
if (-not (Test-Path "$dir\doc.pdf")) {
    $n = 30
    $kids = (0..($n - 1) | ForEach-Object { "$(4 + 2 * $_) 0 R" }) -join ' '
    $objs = [Collections.Generic.List[string]]@('<< /Type /Catalog /Pages 2 0 R >>', "<< /Type /Pages /Kids [$kids] /Count $n >>",
        '<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>')
    $para = 'Flick renders PDF pages with the Windows PDF engine on a worker thread, page by page, at the window width.'
    for ($p = 1; $p -le $n; $p++) {
        $body = (1..34 | ForEach-Object { "($_. $para) '" }) -join "`n"
        $stream = "0.15 0.45 0.85 rg 72 690 468 6 re f`nBT /F1 26 Tf 72 712 Td (Sample document - page $p) Tj ET`nBT /F1 10 Tf 72 660 Td 16 TL`n$body`nET"
        $objs.Add("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 3 0 R >> >> /Contents $(5 + 2 * ($p - 1)) 0 R >>")
        $objs.Add("<< /Length $($stream.Length) >>`nstream`n$stream`nendstream")
    }
    $sb = [Text.StringBuilder]::new("%PDF-1.4`n")
    $offsets = foreach ($i in 0..($objs.Count - 1)) { $sb.Length; [void]$sb.Append("$($i + 1) 0 obj`n$($objs[$i])`nendobj`n") }
    $xref = $sb.Length
    [void]$sb.Append("xref`n0 $($objs.Count + 1)`n0000000000 65535 f `n")
    foreach ($o in $offsets) { [void]$sb.Append(("{0:D10} 00000 n `n" -f $o)) }
    [void]$sb.Append("trailer`n<< /Size $($objs.Count + 1) /Root 1 0 R >>`nstartxref`n$xref`n%%EOF`n")
    [IO.File]::WriteAllText("$dir\doc.pdf", $sb.ToString(), [Text.Encoding]::ASCII)
}
if (-not (Test-Path "$dir\data.csv")) {
    $sb = [Text.StringBuilder]::new("id,name,city,amount,date,notes`n")
    $cities = 'Berlin', 'Tokyo', "S$([char]0xE3)o Paulo", 'Toronto', 'Nairobi', 'Sydney'
    for ($i = 1; $i -le 20000; $i++) {
        [void]$sb.Append("$i,Customer $i,$($cities[$i % 6]),$([math]::Round($i * 1.37, 2)),2026-$('{0:D2}' -f ($i % 12 + 1))-$('{0:D2}' -f ($i % 28 + 1)),`"note, with comma $i`"`n")
    }
    [IO.File]::WriteAllText("$dir\data.csv", $sb.ToString())
}
$lexilla = (Resolve-Path "$PSScriptRoot\..\third_party\lexilla").Path
if (-not (Test-Path "$dir\archive.zip")) { Compress-Archive -Path $lexilla -DestinationPath "$dir\archive.zip" }
if (-not (Test-Path "$dir\archive.tar.gz")) { tar.exe -czf "$dir\archive.tar.gz" -C (Split-Path $lexilla) lexilla }
if (-not (Test-Path "$dir\drawing.svg")) {
    Set-Content -Encoding utf8 "$dir\drawing.svg" @'
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 800 500" width="800" height="500">
  <defs>
    <linearGradient id="sky" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#1e3c72"/><stop offset="1" stop-color="#2a5298"/>
    </linearGradient>
    <radialGradient id="sun" cx="0.5" cy="0.5" r="0.5">
      <stop offset="0" stop-color="#fff6b0"/><stop offset="1" stop-color="#ffb300"/>
    </radialGradient>
  </defs>
  <rect width="800" height="500" rx="24" fill="url(#sky)"/>
  <circle cx="620" cy="130" r="70" fill="url(#sun)"/>
  <path d="M0 380 Q 200 280 400 360 T 800 330 V 500 H 0 Z" fill="#2e7d32"/>
  <path d="M0 420 Q 250 360 500 420 T 800 410 V 500 H 0 Z" fill="#1b5e20" opacity="0.9"/>
  <g stroke="#fff" stroke-width="3" fill="none" stroke-linecap="round">
    <path d="M120 120 l20 -10 l20 10"/><path d="M180 90 l15 -8 l15 8"/>
  </g>
  <text x="60" y="250" font-family="Segoe UI, sans-serif" font-size="56" font-weight="bold" fill="#fff">Flick SVG preview</text>
  <text x="62" y="295" font-family="Georgia, serif" font-size="24" font-style="italic" fill="#cfe3ff">gradients, paths, opacity and text</text>
</svg>
'@
    $in = [IO.File]::ReadAllBytes("$dir\drawing.svg")
    $out = [IO.File]::Create("$dir\drawing.svgz")
    $gz = New-Object IO.Compression.GZipStream $out, ([IO.Compression.CompressionMode]::Compress)
    $gz.Write($in, 0, $in.Length); $gz.Dispose(); $out.Dispose()
}
Copy-Item "$env:WINDIR\Fonts\segoeui.ttf" "$dir\font.ttf" -Force
New-Item -ItemType Directory -Force "$dir\folder" | Out-Null
1..50 | ForEach-Object { Set-Content "$dir\folder\file$_.txt" "hello $_" }
Write-Output $dir
