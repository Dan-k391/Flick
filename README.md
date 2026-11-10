# Flick

Instant Space-to-preview for Windows 11 Explorer, like macOS Quick Look.
Select a file, press **Space**: a preview appears in ~10 ms. **Space**/**Esc** closes it; arrow keys
move the Explorer selection and the preview follows.

- **Code and text** with syntax highlighting (Scintilla/Lexilla)
- **Images** (WIC: JPEG, PNG, GIF, BMP, TIFF, WebP, …; HEIC/AVIF with the Store codecs) and **SVG/SVGZ**
- **Markdown**, rendered with images and coloured code blocks; also **Jupyter notebooks**, **AsciiDoc**
  and **reStructuredText**
- **PDF** (Windows' built-in PDF engine), scrollable
- **CSV/TSV** as a table; **archives** (zip, 7z, rar, tar.gz/xz/bz2, cab, iso, jar, apk, nupkg, whl, …) as
  a file tree
- **Fonts** (ttf, otf, ttc, woff, woff2) as a specimen
- **Audio and video** (mp4, mkv, mov, webm, avi, wmv, mp3, flac, m4a, wav, ogg, …) with playback
- **Programs and installers**: version, publisher, architecture
- **Folders** as a file tree (like archives); **other files**: thumbnail, type, size, date

Native C++ (Win32 + Direct2D), no runtime dependencies, ~25 MB RAM idle, no GPU needed, nothing
fetched from the network.
Mica, rounded corners, light/dark theme, "Open with" button, remembers window size.

## Build and run

Needs Visual Studio 2022+ with the C++ desktop workload.

```
build.cmd
build\Release\Flick.exe          (stays in the background; Flick.exe --exit quits)
```

To start it with Windows, put a shortcut to `Flick.exe` in `shell:startup`.

## Speed

| Preview | key press → on screen |
|---|---|
| folder, other files, fonts, archives | ~8–12 ms |
| Markdown, notebooks, AsciiDoc, rst | ~8–12 ms |
| PNG, 1080p JPEG, SVG, CSV, audio | ~11–17 ms |
| code | ~13–17 ms |
| PDF (page 1 included) | ~20–35 ms |
| 12–24 MP JPEG | ~25–30 ms |
| video (first frame included) | ~100–135 ms |

Measured through real Explorer at 150% scaling; see [AGENTS.md](AGENTS.md) for how, and for the design
notes and lessons learned.

## Third-party code

[Scintilla](https://www.scintilla.org/) and [Lexilla](https://www.scintilla.org/Lexilla.html)
(`third_party/*/License.txt`), [md4c](https://github.com/mity/md4c) (MIT, `third_party/md4c/LICENSE.md`),
[lunasvg](https://github.com/sammycage/lunasvg) and plutovg (MIT, `third_party/lunasvg/`).
Archives use the libarchive that ships with Windows 11 (`archiveint.dll`).
