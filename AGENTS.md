# Flick — agent guide

Flick is a fast, minimal file previewer for Windows 11 (like macOS Quick Look / PowerToys Peek):
select a file in Explorer, press **Space**, a preview appears; **Space**/**Esc** closes it.

## Goals (in priority order)
1. **Feels instant**: target 10–15 ms from key press to frame on screen for text, Markdown, images, folders.
   PDF is bounded by the Windows PDF engine (~5–20 ms per page); its first frame waits for page 1.
   Measure every change that touches the open path (see *Speed test*); don't guess.
2. **Light on RAM**: idle private bytes ~20–25 MB. No GPU driver in-process (see lessons).
3. **Looks like Windows 11**: Mica, rounded corners, light/dark theme, slim title bar with
   file name, details, "Open with …" button and close button.
4. Small, readable native code. No frameworks, no runtime dependencies beyond Windows.

## Layout
```
src/main.cpp     entry, single instance, keyboard hook thread, Explorer selection (COM), --bench
src/window.cpp   preview window: Mica chrome (Direct2D), text (Scintilla), image (WIC), info (shell),
                 PDF page layout, scrolling
src/markdown.cpp Markdown: md4c -> blocks -> DirectWrite layouts, laid out lazily as far as scrolled;
                 images (local/data:), code blocks coloured via highlight.cpp
src/convert.cpp  ipynb (small JSON parser), AsciiDoc, rst -> Markdown text
src/highlight.cpp Lexilla lexing outside Scintilla (minimal IDocument) -> colour roles
src/pdf.cpp      PDF worker thread: Windows.Data.Pdf (C++/WinRT) -> IPdfRendererNative -> WIC bitmap
src/grid.cpp     table view: CSV/TSV parser; archive (libarchive in Windows' archiveint.dll) and folder trees
src/font.cpp     font specimen (private DirectWrite collection, WOFF unpacked by DirectWrite)
src/svg.cpp      SVG/SVGZ via lunasvg; Windows fonts memory-mapped for <text>
src/media.cpp    audio/video: Media Foundation media engine (frame server, CPU) + first-frame reader
src/fileinfo.cpp info-card lines for exe/dll/sys (version, PE header) and msi (summary info)
src/lang.cpp     extension -> Lexilla lexer, keywords, style->colour-role table; code-fence names
src/app.h        shared declarations, WM_FLICK_* messages
third_party/     vendored Scintilla 5.6.9 + Lexilla 5.5.5 (src/include/win32 only), md4c 0.6.0,
                 lunasvg 3.5.0 + plutovg; all unmodified
scripts/         make-samples.ps1, bench.ps1 (in-process), e2e.ps1 (real Explorer + screenshots)
.claude/skills/  repeatable workflows: build, speed-test, add-preview-type, add-language
```

## Build & run
- Needs Visual Studio 2022+/18 with C++ desktop workload (MSVC + Windows SDK). CMake/Ninja come with VS.
- `build.cmd` (Release) or `build.cmd Debug` → `build\<cfg>\Flick.exe`. It kills a running Flick first
  (the linker can't overwrite a running exe). From bash use PowerShell: `cmd /c .\build.cmd`.
- Run `build\Release\Flick.exe` (stays resident, no window until Space). `Flick.exe --exit` quits it.
  It sits in the notification area: its menu has "Start at sign-in" (HKCU `Run` value `Flick`) and "Quit".
- Log: `%LOCALAPPDATA%\Flick\flick.log` (one line per open with timings; truncated at 1 MB).
- Window size follows QuickLook's rules (`PlaceWindow`): each kind asks for a size (text 800×600,
  Markdown 1000×600, CSV 1000×700, trees 800×480, …); pictures/videos/PDFs ask for their own size,
  scaled down (never up) into 80% (PDF 90%) of the screen. Centred on first show; while open it resizes
  around its centre or stays against a screen edge it sits near. Nothing is remembered.

## Speed test
- `powershell -File scripts\bench.ps1` — in-process: opens each sample 20× (no Explorer), prints
  first/median `ready` (painted) and `on-screen` (after `DwmFlush`, i.e. composed by DWM) + memory.
- `powershell -File scripts\e2e.ps1 [-Shots] [-Repeat N] [-Names a,b]` — real Explorer window,
  real Space key via SendKeys; timings from the hook's timestamp, so they include the Explorer COM
  round trip. `-Shots` saves screenshots to `build\shots\` — look at them after visual changes.
- Both use samples generated in `build\samples\` (code, 5.6 MB log, 1080p/12 MP/24 MP JPEG, PNG,
  Markdown (`readme.md` = this file, `features.md`), a 30-page PDF, binary, folder).
- e2e drives the real keyboard: **don't run it while someone is using the PC** (Space/Esc land in the
  window in front, and the preview hides when another app takes the foreground). To look at a view
  without Explorer: `powershell -File scripts\shot.ps1 <file> [-Wheel n]` runs `Flick.exe --show <file>
  --mute` and saves `build\shots\<name>.png` via `PrintWindow` (no keyboard input, muted).
- `Flick.exe --show <file>` previews one file in its own process (exits when closed). `--selftest`
  checks languages, CSV, Markdown, converters and highlighting; failures are named in `bench.log`.
- Audio/video samples need ffmpeg (`winget install Gyan.FFmpeg`); `--bench` plays them muted.

### Latest numbers (2026-11-10, e2e median, 150% DPI, 960×680 DIP window)
| type | Explorer lookup | load | on screen |
|---|---|---|---|
| folder | 2.5 ms | 0.7 ms | ~9 ms |
| other file (info card) | 2.4 ms | 1.5 ms | ~9 ms (thumbnail fills in at +10–35 ms) |
| PNG 512² | 2.4 ms | 2.8 ms | ~10–13 ms |
| JPEG 1080p | 2.4 ms | 7 ms | ~14–17 ms |
| code (35 KB, C++) | 2.3 ms | 1.5 ms | ~13–17 ms |
| log 5.6 MB (2 MB shown) | 2.4 ms | 5 ms | ~20 ms |
| JPEG 12/24 MP | 2.4 ms | 19 ms | ~25–30 ms |
| Markdown (this file, 10 KB) | 2.2 ms | 0.9 ms | ~8–11 ms |
| PDF, simple 30 pages (page 1 included) | 2.3 ms | 0.7 ms | ~19–22 ms |
| PDF, real-world text page (in-process) | – | 0.8 ms | ~35 ms (engine ~20 ms/page) |

In-process medians (`bench.ps1`, add ~2.5 ms for the Explorer lookup), 2026-11-10:
| type | on screen |
|---|---|
| CSV 20k rows · zip · tar.gz listing | ~12.5 · ~8 · ~12 ms |
| folder tree: 50 files · this repo (780) · C:\Windows (first 10k) | ~9 · ~12 · ~53 ms |
| font (ttf/ttc) · SVG with text · notebook · AsciiDoc/rst | ~8 · ~12 · ~12 · ~8 ms |
| audio (tags + art async) | ~12 ms |
| video (1080p H.264, real first frame) | ~50 ms (low-latency source reader; engine alone 300 ms) |

First press after startup: ~35 ms. Idle: ~25 MB private, ~45–55 MB working set; after a session
of PDFs and big images it sits around ~35–55 MB private; after any video ~58 MB (Media Foundation
keeps ~33 MB of decoder/audio state; only a helper process would give it back).

2026-11-13 (`bench.ps1` only; the tables above were not re-measured): remembering the "Open with"
label took 0.9–2.4 ms off every warm open (median `ready`: code 7.2 → 5.5, CSV 11.0 → 8.7, 5.6 MB log
10.6 → 8.2, PNG 4.6 → 3.2, Markdown 4.6 → 3.6 ms); video first frame ~43 ms on every open (before,
about half the opens took ~140 ms).

## Conventions
- C++20, MSVC, `/utf-8`. Win32 + COM via `Microsoft::WRL::ComPtr`. No exceptions thrown by our code.
- Keep the open path synchronous and short; anything unbounded (shell thumbnails, folder counts)
  goes to a worker thread and posts back (`WM_FLICK_INFO` pattern).
- Coordinates: Direct2D draws in DIPs (`SetDpi`), Win32 calls use physical pixels.
- Colours live in the two `Palette`s in window.cpp; languages in lang.cpp tables.
- Don't modify `third_party/`; if you must, say so in this file.
- Commit in small, meaningful steps; end messages with the Co-Authored-By trailer.

## Lessons learned
- **Hardware Direct2D loads the GPU driver** (NVIDIA: ~90 MB private). Software render target +
  GDI Scintilla costs only ~1–2 ms per open. Software DirectWrite in Scintilla was *slower* than GDI.
- **GDI writes alpha 0, and DWM adds it onto Mica** wherever the frame is extended into the client
  (grey 28 became 60, every syntax colour got brightened). Text view extends the frame over the title bar
  only; image/info views use full-window Mica (Direct2D writes real alpha). See `Layout()`.
- **ShowWindow costs ~3 ms; DWM cloak/uncloak ~0.1 ms.** The window stays `WS_VISIBLE` and cloaked
  when idle. The very first uncloak costs ~130 ms (DWM builds visuals + Mica) → warm-up uncloaks
  once off-screen. If we're foreground when hiding, also `SW_HIDE` so focus never sits on an invisible window.
- **Finding the Explorer tab via `IShellWindows` is slow** (10–60 ms, worse while Explorer is busy).
  Cache the `IShellBrowser` of the focused tab; validate with `IsChild(tab, focus)`.
- **A visible Scintilla makes `SCI_SETTEXT` ~3 ms** (scrollbar/layout updates); hidden: 0.1 ms.
- **JPEG decode is dominated by entropy decoding**: DCT scaling (`IWICBitmapSourceTransform`) helps
  only ~30%. Decode to window size, not monitor size; re-decode on `WM_EXITSIZEMOVE`.
- Shell thumbnails are 5–60 ms and unbounded for slow handlers → async.
- **The software render target never returns the memory of its own bitmaps** (+50 MB private after a
  few PDF pages). `ToD2D` makes a WIC bitmap and wraps it with `CreateSharedBitmap(IWICBitmapLock)`:
  no copy, and the memory is freed with the bitmap (D2D holds a reference on the lock).
- **`PdfPage::RenderToStreamAsync` loads the GPU driver** (+200 MB) and encodes an image. Instead
  `IPdfRendererNative::RenderPageToDeviceContext` into a software WIC bitmap render target. The renderer
  still wants a DXGI device: give it WARP (never drawn on). Drawing on a WARP device context wasn't faster.
- **PDF cost is fixed per page** (content parsing, fonts): half the pixels or half the page
  (`SourceRect`) saved almost nothing. So the first frame waits (≤120 ms) for page 1 rather than flash
  a blank page; further pages render in the background as they scroll into view.
- PDF worker protocol: the worker `SendMessage`s each result and the reply names the next page, so the
  UI thread alone decides what is needed and never blocks on the worker. Waiting for a sent message:
  `PeekMessage(PM_QS_SENDMESSAGE)`, then re-check the condition *before* `MsgWaitForMultipleObjects`
  (the other order sleeps until the following page arrives, +20 ms).
- Markdown: md4c is built with `MD4C_USE_UTF16` (parses `wchar_t` directly). Laying out every block of
  a long file costs tens of ms, so blocks are laid out lazily down to the bottom of the view.
- **Windows 11 ships libarchive as `archiveint.dll`** (Explorer's 7z/rar/tar support): standard
  `archive_*` exports, loaded with `LoadLibraryEx(…SEARCH_SYSTEM32)`; zip/7z/rar/tar.*/cab/iso
  listing and gunzip for .svgz at zero added size. Undocumented: missing → info card.
- **Lexilla outside Scintilla** needs only an `IDocument`. `GetCharacterAndWidth` past the end must
  return width 1 (like Scintilla); width 0 stalls `StyleContext` forever (100% CPU hang).
- **Media engine first frame takes 200–300 ms** even warm (pipeline + audio endpoint); byte-stream
  sources don't help. A source reader (`MF_LOW_LATENCY`) decodes one frame in ~40 ms on a worker → shown first. Media
  Foundation keeps ~33 MB after the first video; `MFShutdown` per preview made it worse.
- **The media engine starting up slows a source reader on the same file**: the poster took 240–320 ms
  instead of 40 in about half the opens (missing the 120 ms first-frame wait). So the engine is created
  without a source and `MediaStart` runs when the poster arrives; playback begins ~30 ms later (~335 ms).
- **`AssocQueryString(…FRIENDLYAPPNAME)` costs 0.5–2.5 ms on every call**, warm (.csv/.log ~2 ms). The
  "Open with …" label is remembered per extension and asked again after the frame is on screen.
- **Link-time code generation (`/GL`) gained nothing measurable** and grew the exe 3%: the open path's
  time is in WIC, GDI, DWM and the PDF engine, not in our code.
- **Workers must stop when a newer preview starts** (they get the current generation and their own):
  stale folder/archive listings piled up to 20 threads and 224 MB in the bench. Folders list
  breadth-first within 60 ms / 10k items so the result lands inside the 120 ms first-frame wait.
- Tree sorting: `CompareStringEx(…SORT_DIGITSASNUMBERS)` on string views; `StrCmpLogicalW` needed a
  copy of both names per comparison.
- **Sizing the window costs one resize**: when shown, the window is only *moved* to the monitor; pictures
  decode for 80% of that screen (`PictureArea`), then `PlaceWindow` sizes it once (PDF: in `OnPdf`
  before page 1 is requested). Two resizes per open cost ~4 ms.
- **Seek-bar dragging**: a playing media engine shows almost no frames during a stream of seeks (5 frames
  for 25 seeks) and needs 250–750 ms per seek even approximate. So the drag pauses playback and a source
  reader on a worker decodes the key frame under the knob (`MediaScrub*`, ~28 ms 1080p, ~110 ms 4K); the
  engine seeks exactly once on release. `MF_LOW_LATENCY` on the reader halves that (decoder stops buffering).
- `GetVideoAspectRatio` is the *picture* aspect (16:9), not the pixel aspect: width = height × ax/ay.
- lunasvg rejects a UTF-8 BOM; strip it. D2D's own SVG renderer can't draw `<text>`, hence lunasvg.
- Markdown images: read only the size at layout (headers), decode pixels when first drawn — else an
  image below the fold costs the open (8 → 15 ms).
- PowerShell 5 reads BOM-less scripts in the ANSI code page: write non-ASCII as `[char]0xE3`.
- Tool quirks: Python on Windows writes CRLF unless `newline='\n'`; backslash escapes in shell heredocs
  get mangled on the way, so use raw strings, `(wchar_t)0x…` or the editor tools.
- The keyboard hook runs on its own thread so a busy UI thread never lags system-wide typing.
  We don't swallow Space: Explorer ignores it, and swallowing would need slow COM work in the hook.
- Show with `SWP_NOACTIVATE` so Explorer keeps focus: arrow keys move the selection and the
  preview follows (on key-up). Inactive windows show Mica as a flat colour — expected.
- **Alt+Tab, Start and search briefly take the foreground** (`XamlExplorerHostIslandWindow`,
  `Windows.UI.Core.CoreWindow`, …). The hide-on-foreground hook ignores those shell classes; the preview
  is `HWND_TOPMOST` so Explorer doesn't cover it when re-activated.
- `WS_SYSMENU` + extended frame makes DWM draw its own tiny close button; don't set it.
- `small` is a Windows macro (`rpcndr.h`). Don't name variables `small`/`near`/`far`.
- Don't edit sources with PowerShell `Get-Content`/`Set-Content`: it re-encodes UTF-8 and breaks
  non-ASCII literals. Use the editor tools or Python with explicit `encoding='utf-8'`.
- PowerShell passes `$null` to a `string` P/Invoke parameter as `""` (FindWindow then fails).
- PowerShell is DPI-unaware: window sizes it sets/reads are virtualized (×1.5 at 150%).

## Not done yet / ideas
- More formats: Office/eml/msg/CAD via preview handlers (out of process, slow), HTML (WebView2,
  heavy), PSD/RAW without codecs, EPUB, 3D models.
- Video in a helper process (gives Media Foundation's ~33 MB back); cached shell thumbnail as an
  instant poster.
- Markdown: clickable links, remote images (deliberately off: no network), raw HTML, "show source".
- PDF: zoom, text selection/copy, password prompt.
- Sidebar (Markdown contents, PDF thumbnails) is fixed-width and auto-hides below 700 DIP; no toggle yet.
- Progressive images (show cached shell thumbnail, decode full image on a worker) for >10 MP photos.
- Animated GIF, HEIC/AVIF need the Store codecs; multi-selection; "Open with" dropdown
  (right-click the button opens the system chooser today).
