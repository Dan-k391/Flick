# Flick — agent guide

Fast, minimal file previewer for Windows 11 (like Quick Look): select a file in Explorer, press **Space**;
**Space**/**Esc** closes.

## Goals (in order)
1. **Instant**: 10–15 ms key press → frame on screen. Measure every change to the open path; don't guess.
2. **Light**: idle ~15–17 MB private. No GPU driver in-process.
3. **Windows 11 look**: Mica, rounded corners, light/dark, slim title bar (name, details, "Open with …", close).
4. Small native code; no frameworks or runtime dependencies beyond Windows.

## What it does
- Formats: the table in README.md (keep it current). Office files are drawn without Office installed.
- Keys: Space opens/closes, arrows follow the Explorer selection, Esc closes, Ctrl+F finds in text.
- Mouse selection, Ctrl+A/Ctrl+C, right-click Copy/Select all: everywhere there is text except PDF.
- Tray: Disable, Start at sign-in (HKCU `Run` value `Flick`), Quit.

## Layout
```
src/main.cpp      entry, single instance, keyboard hook thread, Explorer selection (COM), tray, --bench
src/window.cpp    window, Mica chrome, text (Scintilla) + find, images (WIC), info card, PDF layout, media controls
src/markdown.cpp  md4c -> blocks -> DirectWrite layouts (lazy)     src/convert.cpp   ipynb/AsciiDoc/rst -> Markdown
src/word.cpp      docx reading view, OMML math, shared selection   src/pages.cpp     pptx slides (on the PDF worker)
src/office.cpp    odt/odp/doc/ppt -> Markdown; xlsx/xls/ods -> grid src/xml.h        XML scanner, small DOM, zips
src/grid.cpp      CSV, sheets, archive and folder trees            src/pdf.cpp       PDF worker (Windows.Data.Pdf)
src/media.cpp     audio/video (Media Foundation), subtitles        src/highlight.cpp Lexilla outside Scintilla
src/imgfmt.cpp    TGA/QOI/PNM/HDR/PCX decoders, CUR/ANI, previews inside sketch/xmind/kra/ora
src/font.cpp, svg.cpp (lunasvg), fileinfo.cpp (exe/dll/msi/ELF/Mach-O/certificates), lang.cpp (extension -> lexer), app.h
third_party/      Scintilla, Lexilla, md4c, lunasvg + plutovg: unmodified, licenses beside them
scripts/          make-samples.ps1, make-office-samples.ps1 (needs Office), bench.ps1, e2e.ps1, shot.ps1
.claude/skills/   build, speed-test, add-preview-type, add-language
```

## Build, run, test
- `build.cmd [Debug]` → `build\<cfg>\Flick.exe` (VS 2022+, C++ desktop). From bash: `powershell -c "cmd /c .\build.cmd"`.
  It kills the running Flick: **restart the resident one afterwards**. `--exit` quits it.
- Log: `%LOCALAPPDATA%\Flick\flick.log`, one line per open with timings.
- `scripts\bench.ps1`: in-process, each sample 20×, median `ready` / `on-screen`. The A/B tool for speed.
- `scripts\e2e.ps1 [-Shots] [-Repeat n] [-Names a,b]`: real Explorer + SendKeys, +5–10 ms over bench. **Never
  while someone uses the PC.**
- `scripts\shot.ps1 <file> [-Wheel n] [-Click x,y] [-Drag x1,y1,x2,y2] [-Keys vk] [-Find text] [-SelectAll] [-Copy]
  [-Window]`: Flick paints the frame into `build\shots\<name>.png` itself. Safe while the PC is in use. Arrays need
  `powershell -Command "& ..."`. `-Copy` runs only when the clipboard is empty or text-only: don't work around it
  (copied files can't be put back).
- `--selftest`: languages, CSV, Markdown, converters, number formats, selection. Samples: `make-samples.ps1`.

Typical on-screen medians (bench):
| folder, info card, Markdown, PNG, font, sheets, docx, doc/ppt text | 4–12 ms |
|---|---|
| code, 5.6 MB log, CSV 20k rows, zip | 8–20 ms |
| JPEG 1080p / 12 MP / 24 MP | 13 / 22 / 31 ms |
| pptx page 1, PDF page 1 (simple / real text) | 13–21 / ~20 / ~35 ms |
| video first poster, folder tree of 10k items | ~41 / ~28 ms |

Memory: idle ~15–17 MB private; ~58 MB after any video (Media Foundation keeps ~33 MB).

## Conventions
- C++20, MSVC, `/utf-8`, Win32 + COM (`ComPtr`), no exceptions. Direct2D draws in DIPs; Win32 uses pixels.
- Open path synchronous and short. Anything unbounded goes to a worker that posts back with a generation number
  and stops when a newer preview starts.
- Files are untrusted: bound every size, check every read; a bad file gives the info card, never a crash or hang.
- Colours in the two `Palette`s (window.cpp); languages in lang.cpp. Don't modify `third_party/`.
- Small commits ending with the Co-Authored-By trailer. Never commit anything under `build\`.
- Edit sources with editor tools or Python (`encoding='utf-8'`, `newline='\n'`), never PowerShell `Set-Content`.

## Lessons (why the code is the way it is)
**Window**
- Hardware Direct2D loads the GPU driver (+90 MB) → software render target, GDI Scintilla. D2D's own bitmaps
  never return memory → `ToD2D` wraps WIC bitmaps.
- GDI writes alpha 0 and DWM adds it onto Mica → the text view extends the frame over the title bar only.
- `ShowWindow` costs ~3 ms, DWM uncloak ~0.1 ms → the window stays visible and cloaked; the first uncloak
  (~130 ms) happens off-screen at start. One resize per open (~2 ms each). Size per kind follows QuickLook
  (`PlaceWindow`).
- Shown `SWP_NOACTIVATE` + topmost so Explorer keeps the keyboard; the hook thread watches Space/Esc/arrows and
  never swallows Space. Hidden on foreground change, except shell UI (Alt+Tab, Start, search).
- Escape and changed Explorer selections can supersede an open while it waits for its first worker result.
  The UI handles those requests without painting the superseded preview.
- The hook is re-installed whenever Explorer takes the foreground: games' hooks swallow Space, and Windows drops
  a hook that once timed out.
- `IShellWindows` takes 10–60 ms → the focused tab's `IShellBrowser` is cached. Shell thumbnails (5–60 ms) are
  async; the "Open with" label is cached per extension.
- Ctrl+F: the hook swallows it in Explorer and the window takes the foreground with a synthetic Alt; Esc gives it
  back. The field is drawn in the title bar.
- Selection: a click activates the preview (no hide), so Ctrl+A/Ctrl+C arrive like Esc; the hook is untouched and
  Ctrl+C in Explorer still copies the file. Positions are (paragraph, index) that survive re-layout; copying
  never lays out (docx copies from the XML, Markdown from text parsed at load).

**Text and Markdown**
- Scintilla: text is loaded hidden (0.1 vs 3 ms), but hidden it skips scrollbar updates → scroll width is set
  from the first 200 lines and changed after the frame. `WM_SIZE` in `Layout` cost +3 ms per open.
- Lexilla outside Scintilla: `GetCharacterAndWidth` past the end must return width 1 (0 hangs).
- Markdown blocks, table rows and code are laid out lazily. Table column widths come from the first 50 rows +
  each column's longest cells. Code blocks are split into 50-line chunks but lexed as one block
  (`LexBegin`/`LexRange`): one layout with 240k coloured runs took 21 s. `Block` stays small (table data behind
  a pointer): long documents have 200k blocks.

**Images, PDF, media**
- JPEG cost is entropy decoding → decode at window size (DCT scaling). A worker refines the raster after
  resize or zoom and keeps the latest request. Moving a window at the same size and DPI reuses its bitmap.
- Standalone SVGs are parsed once and drawn directly into premultiplied WIC bitmaps. SVG font aliases share
  file mappings. A real SVG resize still rerenders on the UI thread.
- PDF: `RenderToStreamAsync` loads the GPU driver (+200 MB) → `RenderPageToDeviceContext` into a WIC target with
  WARP. Cost is per page. The worker `SendMessage`s each page and the reply names the next one; wait with
  `PeekMessage(PM_QS_SENDMESSAGE)`, then re-check *before* `MsgWaitForMultipleObjects`.
  The worker sends page one before reading the remaining page sizes.
- Video: the engine needs ~300 ms for a first frame → a low-latency source reader decodes the poster (~40 ms)
  first. Playback starts after the poster paints. Seek-bar drags show key frames from another reader.
  `MFShutdown` per preview made memory worse.
- Subtitles: the engine doesn't draw them in frame-server mode → parsed and drawn by us (`.lrc` beside audio too).
- Camera RAW: WIC's own DNG decoder shows only the small first IFD → the Raw codec by container GUID, showing
  `GetPreview` (0.6 ms) turned by the TIFF orientation tag; `GetFrame` demosaics (24 MP ~440 ms): no preview, info card.
- Certificates: `CryptQueryObject` with every format took ~5 ms → PEM to DER by hand, then a certificate or PKCS #7.

**Office**
- Office's own preview handlers (what QuickLook uses) were dropped: 0.6–1.1 s cold, 200–300 MB stays resident.
- Opening reads little, the rest comes later: pptx reads presentation.xml + rels, a slide's parts when first
  drawn; sheets read the first 100 rows and a worker replaces the grid with the whole sheet; docx inflates
  document.xml but parses and lays out only as far as the view scrolls; doc/ppt/odt/odp read 512 KB and cut.
- docx is one column at window width: imitation pages looked bad. Pictures, math, super/subscripts and indents
  are `IDWriteInlineObject`s; pictures decode on a worker at drawn size.
- pptx: placeholder inheritance slide → layout → master; text styles lstStyle → master → presentation defaults;
  colours through clrMap + theme. Worker brushes must not reach the UI thread (layouts handed over cleared).
- Font names in Office files are GDI faces ("Calibri Light") → `ResolveFont` gives family + weight + stretch.
- Dark mode `DarkInk`: only near-black (below 60 per channel) becomes palette text; other dark colours get
  lightness mirrored, hue kept. No-fill cells show the window; white fills count as none.
- Sheet text: a clip per cell doubled draw time → only overflowing text is clipped. Column widths use the
  default font's digit width.
- xls: direct-mode `StgOpenStorageEx` (0.2 vs 8 ms transacted). BIFF8 only.
- The XML DOM drops elements deeper than 256 (`kMaxDepth`): every walker recurses.

**Misc**
- `archiveint.dll` (Windows' libarchive, undocumented) lists archives and inflates zips; missing → info card.
- Gzip-filtered TAR drains member data with `archive_read_data_block` before the next header, checking generation
  and deadline between native blocks. ZIP and plain TAR keep native header skipping. That path also applies if
  the optional format, filter or block APIs are missing. Opening, parsing a header (including PAX/GNU metadata),
  and each native data-block call are still indivisible, including any blocking I/O inside them.
- Folders scan breadth-first for 60 ms or 10k rows. Archives scan for 3 s or 200k headers and keep at most
  200k rows including omitted parents. Both limit relative paths to 4,096 UTF-16 units and 255 levels below
  the top row, and keep at most 8 Mi UTF-16 units of row paths. Archive directory-index keys are bounded by
  those same paths. Listings sort short runs, then merge and format rows with generation checks. Truncated
  trees show how many rows were kept alongside the details. Folder totals still use a separate walk and include hidden files.
- `/GL` gained nothing. lunasvg rejects a UTF-8 BOM. `small`/`near`/`far` are macros.
- PowerShell 5 reads BOM-less scripts as ANSI, is DPI-unaware, and can't pipe into `git credential` (use bash).
- No `gh` CLI: releases via curl + the git credential token.

## Not done
PDF text selection/zoom/find; selection across slides, Shift+click; charts/SmartArt (placeholder box); docx
headers/footers/footnotes/columns; doc/ppt/odt/odp as pages; xlsb; ods/xls formatting; Markdown links/find;
HTML; PSD/EPUB/3D; mail; APNG/animated WebP/ANI; RAW without an embedded preview (or past its size); pfx contents (password); EMF/WMF (GDI
playback can't be bounded in-process; needs a helper process);
embedded subtitle tracks; multi-selection.
