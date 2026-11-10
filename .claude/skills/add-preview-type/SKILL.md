---
name: add-preview-type
description: Add a new kind of preview to Flick (e.g. PDF, video, Markdown rendering, archives). Use when the user asks Flick to preview a new file format beyond text, images and the generic info card.
---

# Add a preview type

All preview code is in `src/window.cpp`.

1. Add a value to `enum Kind` (`K_Text`, `K_Image`, `K_Info`, …).
2. Write `bool LoadXxx(const std::wstring& path)` next to `LoadImage`/`LoadText`. Return false to fall
   through to the next type. Set `g_sub` (dimmed title-bar details) if useful.
   - Keep it fast and synchronous only if bounded (< ~5 ms). Otherwise follow the `InfoJob` /
     `WM_FLICK_INFO` pattern: start a worker thread, post the result back, ignore stale results via a
     generation counter, show a placeholder immediately.
   - Never decode more than the window shows (see `LoadImage`).
   - A long-lived worker producing pieces on demand (e.g. pages): copy the PDF pattern (`pdf.cpp` +
     `OnPdf`): the worker `SendMessage`s each result, the reply says what to do next.
   - Bitmaps always via `ToD2D` (shares WIC memory; bitmaps owned by D2D keep their memory forever).
3. Dispatch it in `LoadContent` (order matters: images, then text sniffing, then info card).
4. Draw it in `Paint()` (Direct2D, DIP coordinates, `area` = content rect); long content scrolls with
   `g_scroll`/`g_docH` + `Scrollable()` (wheel; keys when focused) like Markdown/PDF. Or, for a child HWND
   control, show/size it in `Layout()` like Scintilla and hide it when idle (`PreviewHide`).
5. Free its memory in `PreviewHide` (content is released whenever the preview closes).
6. Add a sample to `scripts/make-samples.ps1` and its name to `bench.ps1` and `e2e.ps1`.
7. Run the `speed-test` skill with `-Shots`, check the screenshot, update AGENTS.md (numbers + lessons).
8. Commit.
