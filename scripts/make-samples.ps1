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
int main() {
    return 0;  // a fenced code block
}
```

---

![an image](logo.png) shows its alt text. <b>Inline HTML</b> shows its text.
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
New-Item -ItemType Directory -Force "$dir\folder" | Out-Null
1..50 | ForEach-Object { Set-Content "$dir\folder\file$_.txt" "hello $_" }
Write-Output $dir
