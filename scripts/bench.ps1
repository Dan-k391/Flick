# In-process open-time benchmark (no Explorer round trip). Prints per-file timings and memory.
$ErrorActionPreference = 'Stop'
$exe = Join-Path $PSScriptRoot '..\build\Release\Flick.exe'
$dir = & "$PSScriptRoot\make-samples.ps1"
$files = 'code.cpp', 'big.log', 'photo-1920.jpg', 'photo-12mp.jpg', 'photo-24mp.jpg', 'icon-512.png', 'readme.md', 'doc.pdf', 'data.csv', 'archive.zip', 'archive.tar.gz', 'drawing.svg', 'font.ttf', 'notebook.ipynb', 'guide.adoc', 'guide.rst', 'data.bin', 'folder' |
    ForEach-Object { Join-Path $dir $_ }
& $exe --bench @files | Write-Output
