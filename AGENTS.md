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
src/markdown.cpp Markdown: md4c -> blocks -> DirectWrite layouts, laid out lazily as far as scrolled
src/pdf.cpp      PDF worker thread: Windows.Data.Pdf (C++/WinRT) -> IPdfRendererNative -> WIC bitmap
src/lang.cpp     extension -> Lexilla lexer, keywords, style->colour-role table
src/app.h        shared declarations, WM_FLICK_* messages
third_party/     vendored Scintilla 5.6.9 + Lexilla 5.5.5 (src/include/win32 only), md4c 0.6.0; unmodified
scripts/         make-samples.ps1, bench.ps1 (in-process), e2e.ps1 (real Explorer + screenshots)
.claude/skills/  repeatable workflows: build, speed-test, add-preview-type, add-language
```

## Build & run
- Needs Visual Studio 2022+/18 with C++ desktop workload (MSVC + Windows SDK). CMake/Ninja come with VS.
- `build.cmd` (Release) or `build.cmd Debug` → `build\<cfg>\Flick.exe`. It kills a running Flick first
  (the linker can't overwrite a running exe). From bash use PowerShell: `cmd /c .\build.cmd`.
- Run `build\Release\Flick.exe` (stays resident, no window until Space). `Flick.exe --exit` quits it.
  Autostart: put a shortcut in `shell:startup`.
- Log: `%LOCALAPPDATA%\Flick\flick.log` (one line per open with timings; truncated at 1 MB).
- Window size is stored in DIPs at `HKCU\Software\Flick` (`Width`, `Height`).

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
  without Explorer, run `--bench` on the file and capture Flick's own window with `PrintWindow(h, dc, 2)`.

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
First press after startup: ~35 ms. Idle: ~20–25 MB private, ~45–55 MB working set; after a session
of PDFs and big images it sits around ~35–55 MB private.

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
- Tool quirks: Python on Windows writes CRLF unless `newline='\n'`; backslash escapes in shell heredocs
  get mangled on the way, so use raw strings, `(wchar_t)0x…` or the editor tools.
- The keyboard hook runs on its own thread so a busy UI thread never lags system-wide typing.
  We don't swallow Space: Explorer ignores it, and swallowing would need slow COM work in the hook.
- Show with `SWP_NOACTIVATE` so Explorer keeps focus: arrow keys move the selection and the
  preview follows (on key-up). Inactive windows show Mica as a flat colour — expected.
- `WS_SYSMENU` + extended frame makes DWM draw its own tiny close button; don't set it.
- `small` is a Windows macro (`rpcndr.h`). Don't name variables `small`/`near`/`far`.
- Don't edit sources with PowerShell `Get-Content`/`Set-Content`: it re-encodes UTF-8 and breaks
  non-ASCII literals. Use the editor tools or Python with explicit `encoding='utf-8'`.
- PowerShell passes `$null` to a `string` P/Invoke parameter as `""` (FindWindow then fails).
- PowerShell is DPI-unaware: window sizes it sets/reads are virtualized (×1.5 at 150%).

## Not done yet / ideas
- More formats: video/audio, archives, Office (via preview handlers?).
- Markdown: clickable links, syntax-highlighted code blocks, images, raw HTML, a "show source" toggle.
- PDF: zoom, text selection/copy, password prompt; dragging the overlay scrollbar.
- Progressive images (show cached shell thumbnail, decode full image on a worker) for >10 MP photos.
- Animated GIF, HEIC/AVIF need the Store codecs; multi-selection; "Open with" dropdown
  (right-click the button opens the system chooser today).
