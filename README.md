# Flick

Quick Look for Windows 11. Select a file in Explorer, press **Space**: it's on screen in ~10 ms.

![Flick demo](docs/demo.gif)

Code, Markdown, images, PDF, Word, Excel, PowerPoint, video, audio, CSV, archives, folders, fonts, programs.
Native C++, ~25 MB RAM, no dependencies.

**[Download Flick.exe](https://github.com/Dan-k391/Flick/releases/latest)**, run it, press Space.
Arrows move between files, Space/Esc closes.

## Supported formats

| Kind | Extensions |
|---|---|
| Code and text | 100+ languages with syntax colours (C/C++, C#, Java, JS/TS, Python, Go, Rust, JSON, XML, HTML, CSS, YAML, shell, SQL, …); any other text file as plain text |
| Markdown | `.md .markdown .mdown .mkd .mkdn .mdx .rmd .qmd`, Jupyter `.ipynb`, AsciiDoc `.adoc .asciidoc`, reStructuredText `.rst .rest` |
| Images | `.jpg .jpeg .jpe .jfif .png .gif .bmp .dib .tif .tiff .ico .webp .jxr .wdp .dds .svg .svgz`; `.heic .heif .avif` with the codecs from the Microsoft Store |
| PDF | `.pdf` |
| Word | `.docx .docm .dotx .dotm`; text only: `.doc .dot .odt .ott` |
| Excel | `.xlsx .xlsm .xltx .xltm .xls .xlt .ods .ots` |
| PowerPoint | `.pptx .pptm .potx .potm .ppsx .ppsm`; text only: `.ppt .pps .pot .odp .otp` |
| Tables | `.csv .tsv .tab .psv` |
| Video | `.mp4 .m4v .mov .mkv .avi .wmv .webm .mpg .mpeg .m2ts .mts .ts .3gp .3g2 .asf`, with `.srt`/`.vtt` subtitles beside the video |
| Audio | `.mp3 .m4a .m4b .aac .wav .flac .wma .ogg .oga .opus .aiff .aif .amr .ac3 .ec3 .mka .weba` |
| Archives | `.zip .7z .rar .tar .gz .tgz .bz2 .tbz .tbz2 .xz .txz .zst .tzst .lz .lzma .lz4 .cab .iso .cpio .ar .deb .rpm .jar .war .ear .apk .aab .xapk .nupkg .snupkg .vsix .whl .egg .crx .xpi .cbz .cbr .cb7 .cbt .appx .appxbundle .msix .msixbundle` |
| Fonts | `.ttf .otf .ttc .otc .woff .woff2` |
| Programs | `.exe .dll .sys .ocx .cpl .scr .drv .efi .mui .ax .com .msi .msp` (description, company, version) |
| Folders | contents as a tree with sizes |
| Anything else | thumbnail and file details |

Video and audio play what Windows can decode. Text can be selected and copied in every text view except PDF.

## Build

Visual Studio 2022+ (C++ desktop): `build.cmd` → `build\Release\Flick.exe`. See [AGENTS.md](AGENTS.md).

Uses [Scintilla/Lexilla](https://www.scintilla.org/), [md4c](https://github.com/mity/md4c),
[lunasvg](https://github.com/sammycage/lunasvg); licenses in `third_party/`.
