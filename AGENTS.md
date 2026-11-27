# Flick — agent guide

Fast, minimal file previewer for Windows 11 (like Quick Look / PowerToys Peek): select a file in Explorer,
press **Space**; **Space**/**Esc** closes.

## Goals (in order)
1. **Instant**: 10–15 ms key press → frame on screen for text, Markdown, images, folders (PDF: page 1 is
   bounded by the Windows PDF engine). Measure every change to the open path (*Speed test*); don't guess.
2. **Light**: idle ~15–17 MB private. No GPU driver in-process.
3. **Windows 11 look**: Mica, rounded corners, light/dark, slim title bar (name, details, "Open with …", close).
4. Small native code; no frameworks or runtime dependencies beyond Windows.

## What it does (README keeps one line of this)
- Previews: code/text (Lexilla, 100+ languages, Ctrl+F find), Markdown (+ ipynb, AsciiDoc, rst), images (WIC: JPEG,
  PNG, animated GIF, BMP, TIFF, WebP, ICO; HEIC/AVIF with Store codecs; wheel zoom + pan), SVG/SVGZ, PDF (all
  pages, thumbnails), Office without Office installed (docx reading view with math, xlsx/xls/ods sheets, pptx slides;
  doc/ppt/odt/odp as text), audio/video with playback, seeking and subtitles (`name.srt`/`.vtt`/`name.<lang>.srt` beside the video), CSV/TSV, archives (zip, 7z, rar, tar.*, cab, iso,
  jar, apk, whl…) and folders as trees, fonts as a specimen, exe/dll/msi details, anything else: thumbnail + info.
- Keys: Space opens/closes, arrows follow the Explorer selection, Esc closes, Ctrl+F finds in text.
- Mouse selection + Ctrl+A/Ctrl+C (and a right-click Copy/Select all) in text, Markdown (+ ipynb, AsciiDoc, rst,
  doc/odt/ppt/odp), docx, sheets/CSV and pptx slides. Not in PDFs.
- Tray: Disable, Start at sign-in, Quit. Archives use Windows' own libarchive (`archiveint.dll`).
- Third-party licenses: `third_party/*/License.txt`, `third_party/md4c/LICENSE.md`, `third_party/lunasvg/`.

## Layout
```
src/main.cpp      entry, single instance, keyboard hook thread, Explorer selection (COM), tray icon, --bench
src/window.cpp    window: Mica chrome (Direct2D), text (Scintilla) + find, images (WIC) + zoom + GIF, info card,
                  PDF layout, scrolling, media controls
src/markdown.cpp  md4c -> blocks -> DirectWrite layouts (lazy); images; code via highlight.cpp
src/convert.cpp   ipynb, AsciiDoc, rst -> Markdown        src/highlight.cpp Lexilla outside Scintilla
src/pages.cpp     pptx slides drawn (D2D + DirectWrite into WIC) for the PDF view, on its worker
src/word.cpp      docx reading view: lazy blocks -> DirectWrite layouts; tables, pictures (worker), OMML math boxes
src/office.cpp    odt/odp/doc/ppt -> Markdown; xlsx (formats, number formats, merges, pictures)/ods/xls -> sheet
                  grid (OLE IStorage); xml.h: XML scanner + small DOM, zips
src/pdf.cpp       PDF worker: Windows.Data.Pdf -> IPdfRendererNative -> WIC bitmap
src/grid.cpp      CSV/TSV; archive (Windows' archiveint.dll = libarchive) and folder trees; sheets drawn like Excel
src/font.cpp      font specimen        src/svg.cpp SVG/SVGZ (lunasvg)        src/fileinfo.cpp exe/dll/msi lines
src/media.cpp     audio/video: Media Foundation engine (frame server, CPU) + source-reader poster/scrub frames
src/lang.cpp      extension -> lexer, keywords, colour roles      src/app.h shared declarations, WM_FLICK_*
third_party/      Scintilla 5.6.9, Lexilla 5.5.5, md4c 0.6.0, lunasvg 3.5.0 + plutovg — unmodified
scripts/          make-samples.ps1, make-office-samples.ps1 (needs Office), bench.ps1, e2e.ps1, shot.ps1,
                  make-icon.py
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
- `scripts\shot.ps1 <file> [-Wheel n] [-Click x,y] [-Keys vk] [-Find text] [-Drag x1,y1,x2,y2] [-SelectAll] [-Copy] [-Window]`: `--show`, then Flick paints
  the frame into `build\shots\<name>.png` itself (`WM_FLICK_SHOT`; PrintWindow can return stale DWM frames, and
  misses the text view, a child window: `-Window`). Safe while the PC is in use. Arrays: `powershell -Command "& ..."`.
  `-Copy` prints what Ctrl+C put on the clipboard and restores it, so it runs only when the clipboard is empty or
  holds text only (else prints `skipped: ...`: files copied in Explorer can't be put back; don't work around it).
- `--selftest` checks languages, CSV, Markdown, converters; samples: `make-samples.ps1` (video needs ffmpeg).

Typical on-screen medians (e2e add ~2.5 ms Explorer lookup; first press after start ~35 ms):
| folder, info card, Markdown, PNG, font | 7–12 ms | code, 5.6 MB log, CSV 20k, zip/tar.gz | 8–20 ms |
|---|---|---|---|
| JPEG 1080p / 12 MP / 24 MP | 13 / 22 / 31 ms | PDF page 1 (simple / real text) | ~20 / ~35 ms |
| folder tree C:\Windows (10k) | ~53 ms | video first frame (engine alone ~300 ms) | ~45–50 ms |
| sheets xlsx (formatted / 50k×20) / xls / ods, doc/ppt/odt/odp text | 4–11 ms | pptx page 1 (Compiler / real.pptx) | ~13 / ~21 ms |
| docx (lecture.docx, 2.4 MB Lec16-18 with math + pictures) | 5–6 ms | docx table 3000 rows / 12 MB xls | ~20 / ~11 ms |

Memory: idle ~15–17 MB private; ~58 MB after any video (Media Foundation keeps ~33 MB).

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
- **Selection / Ctrl+C**: selecting needs a click, and a click activates the preview (Explorer stays the source:
  no hide), so Ctrl+A/Ctrl+C arrive at `PreviewKey` like Esc/Space after a click; the hook is untouched. While
  Explorer has the keyboard Ctrl+C stays Explorer's (copies the file). Esc closes (doesn't clear first). docx: a
  position is (paragraph number in document order, `Item::leaf`, index): stable across re-layouts; Ctrl+A over a
  document not laid out to the end copies from the XML (`NodeBlock`). pptx: the worker hands each drawn
  paragraph's layout (drawing effects cleared: no worker brushes on the UI thread) + transform with the page;
  highlights are an overlay. Sheets keep the selection when the worker's full copy of the same tab arrives.
  Markdown: a paragraph is (block << 32 | table cell); every block's text is parsed at load, so copying never
  lays out and positions survive re-layout. Copied lists get a plain "- " / "1. " / "- [x] " marker.
- **Images**: JPEG cost is entropy decoding; decode to the window size (DCT scaling for ≥2×), re-decode after
  resize and when zoom passes the decoded size (`Sharpen`, UI thread). EXIF orientation via a transform.
  GIF: frames composed on a canvas per disposal (0/1 keep, 2 clear, 3 restore) on a timer.
- Shell thumbnails 5–60 ms → async. `AssocQueryString` 0.5–2.5 ms → "Open with" label cached per extension.
- **PDF**: `RenderToStreamAsync` loads the GPU driver (+200 MB) → `RenderPageToDeviceContext` into a WIC
  target with a WARP device. Cost is per page, not per pixel; first frame waits ≤120 ms for page 1. The worker
  `SendMessage`s each page; the reply names the next one (UI thread decides). Wait: `PeekMessage
  (PM_QS_SENDMESSAGE)` then re-check *before* `MsgWaitForMultipleObjects`.
- **Markdown**: md4c with `MD4C_USE_UTF16`; blocks laid out lazily; images measured at layout, decoded when drawn.
  Tables: column widths from the first 50 rows + each column's longest text, rows laid out lazily, cell layouts
  far from the view released (5000 rows: 300 -> 8 ms). Code blocks of 51+ lines are split into 50-line chunks (the
  cheapest first frame of the sizes measured): one layout with ~240k coloured runs took 21 s (`SetDrawingEffect`
  per run grows with the runs already set); chunks highlight separately. Table data sits behind a pointer: a long
  document has 200k blocks, and Block's size shows in its open time.
- **Lexilla outside Scintilla**: `GetCharacterAndWidth` past the end must return width 1 (0 hangs).
- **Office**: Windows preview handlers (Office's own previewers, what QuickLook uses) were tried and dropped:
  0.1 s warm, 0.6–1.1 s cold, 200–300 MB Office processes stay resident. pptx are drawn by pages.cpp instead:
  slides follow placeholder inheritance (slide -> layout -> master: idx, then type), text styles chain lstStyle ->
  master txStyles -> presentation defaultTextStyle, colours through clrMap + theme with lumMod/lumOff. Opening
  reads presentation.xml + the .rels files only; a page reads its slide/layout/master/theme and pictures in one
  zip pass when first drawn. Page 1 is mostly the 1920 px software bitmap (~2.5 ms to create, ~2.6 ms to
  rasterise) and picture decoding. Table rows grow to their text (`a:tr h` is a minimum).
- **Font names** in Office files are GDI faces ("Calibri Light", "等线 Light", "Arial Narrow"): `ResolveFont`
  (GDI interop, cached) gives DirectWrite family + weight + stretch; as a family name they fell back too heavy.
- **Untrusted XML**: the DOM drops elements deeper than 256 (`kMaxDepth`): every walker over it recurses.
- **docx** (word.cpp): imitation pages looked bad (tiny text on a fake sheet) → one column at the window width,
  1 pt = 96/72 DIP. document.xml is inflated at open (~1 ms/MB; streaming it would need the final sectPr for the
  column width and the archive kept open: not done), but body children are DOM-parsed and laid out only as far as
  the view scrolls (Markdown's pattern); height = laid height / fraction of the XML consumed until the end.
  Tables: cell grid (spans, borders) for the whole table, rows laid out lazily too. Title: the word count.
  Pictures, math, super/subscripts and first-line indents are `IDWriteInlineObject`s (math/pictures stretch the
  line; `IDWriteTextLayout3` PROPORTIONAL spacing keeps that for Word's "auto" line rule; East Asian `docGrid`
  lines snap to the pitch). Display math is an object as wide as the line (centred inside). Math: Unicode math
  italic letters in Cambria Math, a box per OMML element, delimiters stretched by a transform. Pictures: decoded
  on a worker at the drawn size (`WM_FLICK_WORD` + generation), requested while drawing, released 3 screens away.
  Colours: auto text is the palette's on the window, black/white by luminance on fills; in dark mode `DarkInk`
  (shared with sheets): near-black only below 60 in every channel (by luminance navy/dark red went white),
  other dark colours get lightness mirrored, hue kept. Anchored pictures go after their paragraph's text (ink overlays appear below the picture).
- **Sheets** (xlsx): styles.xml resolved per cellXfs (theme colours + tint, indexed palette), `cellIs` conditional
  formats, Excel number formats (`FormatNumber`, selftested), column widths from the default font's digit width
  (DengXian is 8 px, not Calibri's 7). The open path reads the first 100 rows (256 KB of XML/strings); a worker
  (`more`) re-reads the whole sheet and decodes pictures at drawn size, then replaces the grid. Text drawing is
  the cost: `D2D1_DRAW_TEXT_OPTIONS_CLIP` or a clip per cell doubled it (50k×20: 18 → 8 ms), so only overflowing
  text gets a clip. Dark mode: no-fill cells show the window, near-black text/borders take the palette text,
  white fills count as none. Tabs scroll (wheel, arrows) to the active one. xls/ods: first 100 rows on the open
  path too (xls: direct-mode `StgOpenStorageEx`, 0.2 vs ~8 ms transacted; globals + 256 KB of the sheet; shared
  strings decoded lazily); ods rows repeated with content cap at 1000 copies.
- **Office** text formats: what's written, not the layout. OOXML/ODF are zips
  read with archiveint (members by name, others skipped by seeking); headings from styles.xml names
  ("heading N" is English in every UI language). `.ppt` text sits in slide drawings (persist directory chain),
  older files also in SlideListWithText. `.doc` via the piece table: plain paragraphs. `.xls` BIFF8 only.
  The open path reads 512 KB (XML or text) and cuts the rest with a note: a worker would re-lay out it all anyway.
- **archiveint.dll** (undocumented) gives zip/7z/rar/tar/cab/iso listing and gunzip; missing → info card.
- **Folders** list breadth-first ≤60 ms / 10k items; sizes on a worker. Sort with `CompareStringEx(DIGITSASNUMBERS)`.
- **Video**: the engine takes ~300 ms to a first frame (~200 ms to metadata) and slows a reader on the same file,
  so a `MF_LOW_LATENCY` source reader decodes the poster (~40 ms) first, then the engine starts. Seek-bar drag
  pauses and shows key frames from another reader (~28 ms 1080p, ~110 ms 4K); one exact seek on release.
  `GetVideoAspectRatio` is the picture aspect. `MFShutdown` per preview made memory worse.
- **Subtitles**: the engine's timed-text API doesn't draw in frame-server mode, so SRT/WebVTT are parsed by us
  (`ParseSubtitles`, after the poster on its worker) and drawn with DirectWrite over the frame. Embedded tracks: not done.
- `/GL` gained nothing. lunasvg rejects a UTF-8 BOM. `small`/`near`/`far` are macros.
- PowerShell: 5.x reads BOM-less scripts as ANSI (write `[char]0xE3`); `$null` → `""` in P/Invoke strings;
  DPI-unaware (sizes ×1.5 at 150%); piping into `git credential` fails — use bash. Python writes CRLF unless
  `newline='\n'`. No `gh` CLI: releases via curl + the git credential token.

## Ideas / not done
eml/CAD via preview handlers; charts/SmartArt (placeholder box), docx headers/footers/footnote bodies/columns/
explicit tab stops/floating positions/table style conditional formats/EMF pictures, odt/odp/doc/ppt as pages, sheet charts (placeholder box)/zoom/rich-text runs/other conditional formats/[Red] and fractions in
number formats, ods/xls formatting, .doc headings/tables, xlsb; HTML (WebView2, heavy); PSD/RAW/EPUB/3D; video in a helper process;
Markdown links/raw HTML/find; PDF zoom/text selection/find; selection across slides, Shift+click/keyboard selection; sidebar toggle; progressive >10 MP images;
SVG zoom re-render; APNG/animated WebP; HEIC/AVIF need Store codecs; multi-selection.
