# Flick — agent guide

Fast, minimal file previewer for Windows 11 (like Quick Look / PowerToys Peek): select a file in Explorer,
press **Space**; **Space**/**Esc** closes.

## Goals (in order)
1. **Instant**: 10–15 ms key press → frame on screen for text, Markdown, images, folders (PDF: page 1 is
   bounded by the Windows PDF engine). Measure every change to the open path (*Speed test*); don't guess.
2. **Light**: idle ~20–25 MB private. No GPU driver in-process.
3. **Windows 11 look**: Mica, rounded corners, light/dark, slim title bar (name, details, "Open with …", close).
4. Small native code; no frameworks or runtime dependencies beyond Windows.

## Layout
```
src/main.cpp      entry, single instance, keyboard hook thread, Explorer selection (COM), tray icon, --bench
src/window.cpp    window: Mica chrome (Direct2D), text (Scintilla) + find, images (WIC) + zoom + GIF, info card,
                  PDF layout, scrolling, media controls
src/markdown.cpp  md4c -> blocks -> DirectWrite layouts (lazy); images; code via highlight.cpp
src/convert.cpp   ipynb, AsciiDoc, rst -> Markdown        src/highlight.cpp Lexilla outside Scintilla
src/pdf.cpp       PDF worker: Windows.Data.Pdf -> IPdfRendererNative -> WIC bitmap
src/grid.cpp      CSV/TSV; archive (Windows' archiveint.dll = libarchive) and folder trees
src/font.cpp      font specimen        src/svg.cpp SVG/SVGZ (lunasvg)        src/fileinfo.cpp exe/dll/msi lines
src/media.cpp     audio/video: Media Foundation engine (frame server, CPU) + source-reader poster/scrub frames
src/lang.cpp      extension -> lexer, keywords, colour roles      src/app.h shared declarations, WM_FLICK_*
third_party/      Scintilla 5.6.9, Lexilla 5.5.5, md4c 0.6.0, lunasvg 3.5.0 + plutovg — unmodified
scripts/          make-samples.ps1, bench.ps1, e2e.ps1, shot.ps1, make-icon.py
.claude/skills/   build, speed-test, add-preview-type, add-language
```

## Build & run
- VS 2022+/18 with C++ desktop workload. `build.cmd [Debug]` → `build\<cfg>\Flick.exe`; it kills a running
  Flick first. From bash: `powershell -c "cmd /c .\build.cmd"`. **Restart the resident Flick afterwards.**
- `Flick.exe` stays resident (tray icon: Disable, Start at sign-in = HKCU `Run` value `Flick`, Quit);
  `--exit` quits it. Log: `%LOCALAPPDATA%\Flick\flick.log` (one line per open with timings).
- Window size follows QuickLook (`PlaceWindow`): each kind asks for a size; pictures/videos/PDFs their own,
  scaled down into 80% (PDF 90%) of the screen. Centred on first show, then resized around its centre.

## Speed test
- `scripts\bench.ps1`: in-process, each sample 20×, median `ready` (painted) / `on-screen` (after `DwmFlush`).
- `scripts\e2e.ps1 [-Shots]`: real Explorer + SendKeys. **Never while someone uses the PC.**
- `scripts\shot.ps1 <file> [-Wheel n] [-Click x,y] [-Keys vk] [-Find text]`: `--show` + `PrintWindow` screenshot
  to `build\shots\` — safe while the PC is in use. Look at shots after visual changes.
- `--selftest` checks languages, CSV, Markdown, converters; samples: `make-samples.ps1` (video needs ffmpeg).

Typical on-screen medians (e2e add ~2.5 ms Explorer lookup; first press after start ~35 ms):
| folder, info card, Markdown, PNG, font | 7–12 ms | code, 5.6 MB log, CSV 20k, zip/tar.gz | 8–20 ms |
|---|---|---|---|
| JPEG 1080p / 12 MP / 24 MP | 13 / 22 / 31 ms | PDF page 1 (simple / real text) | ~20 / ~35 ms |
| folder tree C:\Windows (10k) | ~53 ms | video first frame (engine alone ~300 ms) | ~45–50 ms |

Memory: idle ~20–25 MB private; ~58 MB after any video (Media Foundation keeps ~33 MB).

## Conventions
- C++20, MSVC, `/utf-8`, Win32 + COM (`ComPtr`), no exceptions. Direct2D draws in DIPs; Win32 uses pixels.
- Open path synchronous and short; anything unbounded goes to a worker that posts back with a generation
  number and stops when a newer preview starts.
- Colours in the two `Palette`s (window.cpp); languages in lang.cpp. Don't modify `third_party/`.
- Small commits ending with the Co-Authored-By trailer. Edit sources with editor tools or Python
  (`encoding='utf-8'`, `newline='\n'`), never PowerShell `Set-Content`.

## Lessons (why the code is the way it is)
- **Rendering**: hardware Direct2D loads the GPU driver (+90 MB) → software render target + GDI Scintilla.
  Its own bitmaps never return memory → `ToD2D` wraps WIC bitmaps (`CreateSharedBitmap`).
- **GDI writes alpha 0, DWM adds it onto Mica**: text view extends the frame over the title bar only.
- **Showing**: `ShowWindow` ~3 ms, DWM uncloak ~0.1 ms → window stays visible+cloaked; first uncloak (~130 ms) is
  done off-screen at start. Shown `SWP_NOACTIVATE` + `HWND_TOPMOST` so Explorer keeps the keyboard; the hook
  thread watches Space/Esc/arrows (never swallows Space). Hide on foreground change, except shell UI
  (Alt+Tab, Start, search). If foreground when hiding, also `SW_HIDE`. One resize per open (~2 ms each).
- **Keyboard hook** is re-installed each time Explorer/desktop takes the foreground: a game's own low-level hook
  (GTA running in the background) swallowed Space, and Windows drops a hook that once timed out.
- **Explorer lookup**: `IShellWindows` is 10–60 ms → cache the focused tab's `IShellBrowser`.
- **Scintilla**: load text hidden (`SCI_SETTEXT` 0.1 vs 3 ms), but hidden it skips scrollbar updates. The scroll
  width is set per file from its first 200 lines (tracking only grows), and a scroll-width change after the
  frame makes it update the bars (~2.5 ms visible; `WM_SIZE` in `Layout` cost +3 ms per open).
- **Find (Ctrl+F)**: the preview doesn't have the keyboard, so the hook swallows Ctrl+F in Explorer and the
  window takes the foreground with a synthetic Alt (`SendInput`; `SC_KEYMENU` ignored); Esc gives it back.
  The field is drawn in the title bar (a child over Scintilla would be another window).
- **Images**: JPEG cost is entropy decoding; decode to the window size (DCT scaling for ≥2×), re-decode after
  resize and when zoom passes the decoded size (`Sharpen`, UI thread). EXIF orientation via a transform.
  GIF: frames composed on a canvas per disposal (0/1 keep, 2 clear, 3 restore) on a timer.
- Shell thumbnails 5–60 ms → async. `AssocQueryString` 0.5–2.5 ms → "Open with" label cached per extension.
- **PDF**: `RenderToStreamAsync` loads the GPU driver (+200 MB) → `RenderPageToDeviceContext` into a WIC
  target with a WARP device. Cost is per page, not per pixel; first frame waits ≤120 ms for page 1. The worker
  `SendMessage`s each page; the reply names the next one (UI thread decides). Wait: `PeekMessage
  (PM_QS_SENDMESSAGE)` then re-check *before* `MsgWaitForMultipleObjects`.
- **Markdown**: md4c with `MD4C_USE_UTF16`; blocks laid out lazily; images measured at layout, decoded when drawn.
- **Lexilla outside Scintilla**: `GetCharacterAndWidth` past the end must return width 1 (0 hangs).
- **archiveint.dll** (undocumented) gives zip/7z/rar/tar/cab/iso listing and gunzip; missing → info card.
- **Folders** list breadth-first ≤60 ms / 10k items; sizes on a worker. Sort with `CompareStringEx(DIGITSASNUMBERS)`.
- **Video**: the engine takes ~300 ms to a first frame (~200 ms to metadata) and slows a reader on the same file,
  so a `MF_LOW_LATENCY` source reader decodes the poster (~40 ms) first, then the engine starts. Seek-bar drag
  pauses and shows key frames from another reader (~28 ms 1080p, ~110 ms 4K); one exact seek on release.
  `GetVideoAspectRatio` is the picture aspect. `MFShutdown` per preview made memory worse.
- `/GL` gained nothing. lunasvg rejects a UTF-8 BOM. `small`/`near`/`far` are macros.
- PowerShell: 5.x reads BOM-less scripts as ANSI (write `[char]0xE3`); `$null` → `""` in P/Invoke strings;
  DPI-unaware (sizes ×1.5 at 150%); piping into `git credential` fails — use bash. Python writes CRLF unless
  `newline='\n'`. No `gh` CLI: releases via curl + the git credential token.

## Ideas / not done
Office/eml/CAD via preview handlers; HTML (WebView2, heavy); PSD/RAW/EPUB/3D; video in a helper process;
Markdown links/raw HTML/find; PDF zoom/text selection/find; sidebar toggle; progressive >10 MP images;
SVG zoom re-render; APNG/animated WebP; HEIC/AVIF need Store codecs; multi-selection.
