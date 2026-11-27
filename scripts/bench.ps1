# In-process open-time benchmark (no Explorer round trip). Prints per-file timings and memory.
$ErrorActionPreference = 'Stop'
$exe = Join-Path $PSScriptRoot '..\build\Release\Flick.exe'
$dir = & "$PSScriptRoot\make-samples.ps1"
$files = 'code.cpp', 'big.log', 'photo-1920.jpg', 'photo-12mp.jpg', 'photo-24mp.jpg', 'icon-512.png', 'readme.md', 'doc.pdf', 'data.csv', 'archive.zip', 'archive.tar.gz', 'drawing.svg', 'font.ttf', 'notebook.ipynb', 'guide.adoc', 'guide.rst', 'video.mp4', 'audio.mp3', 'data.bin', 'folder' |
    ForEach-Object { Join-Path $dir $_ }
# Office samples from make-office-samples.ps1 (needs Office), when they exist: a formatted workbook, 50k rows x 20
# columns, lecture notes (equations, pictures, a table), legacy/ODF sheets, slides; and the real course files
# (Lec16-18.docx, Lab2_All_Students.xlsx, Compiler.pptx, real.pptx) if they are there.
$files += 'office\lab.xlsx', 'office\big.xlsx', 'office\sheet.xls', 'office\sheet.ods', 'office\Lab2_All_Students.xlsx',
    'office\lecture.docx', 'office\Lec16-18.docx', 'office\slides.pptx', 'office\Compiler.pptx', 'office\real.pptx' | ForEach-Object { Join-Path $dir $_ } | Where-Object { Test-Path $_ }
& $exe --bench @files | Write-Output
