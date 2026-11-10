# Flick

Instant Space-to-preview for Windows 11 Explorer, like macOS Quick Look.
Select a file, press **Space**: a preview appears in ~10 ms. **Space**/**Esc** closes it; arrow keys
move the Explorer selection and the preview follows.

- **Code and text** with syntax highlighting (Scintilla/Lexilla)
- **Images** (WIC: JPEG, PNG, GIF, BMP, TIFF, WebP, …; HEIC/AVIF with the Store codecs)
- **Markdown**, rendered (md4c + DirectWrite)
- **PDF** (Windows' built-in PDF engine), scrollable
- **Folders and other files**: thumbnail, type, size, date

Native C++ (Win32 + Direct2D), no runtime dependencies, ~25 MB RAM idle, no GPU needed.
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
| folder, other files | ~9 ms |
| Markdown | ~8–11 ms |
| PNG, 1080p JPEG | ~11–17 ms |
| code | ~13–17 ms |
| PDF (page 1 included) | ~20–35 ms |
| 12–24 MP JPEG | ~25–30 ms |

Measured through real Explorer at 150% scaling; see [AGENTS.md](AGENTS.md) for how, and for the design
notes and lessons learned.

## Third-party code

[Scintilla](https://www.scintilla.org/) and [Lexilla](https://www.scintilla.org/Lexilla.html)
(`third_party/*/License.txt`), [md4c](https://github.com/mity/md4c) (MIT, `third_party/md4c/LICENSE.md`).
