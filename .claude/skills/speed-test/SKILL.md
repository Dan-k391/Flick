---
name: speed-test
description: Measure Flick's open time (key press to frame on screen) and memory, in-process and through real Explorer. Use after any change to the open path, rendering, or loading, and when asked for performance numbers.
---

# Speed test

Build first (see the `build` skill). Then, via the PowerShell tool from the repo root:

1. **In-process bench** (no Explorer; isolates load + render):
   `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\bench.ps1 | Select-String "BENCH|memory|started"`
   Each sample opens 20×. Report the *median on-screen* column, and *first* separately.
2. **End-to-end** (real Explorer, real Space key; includes the COM lookup):
   `Start-Process .\build\Release\Flick.exe; Start-Sleep 2; & .\scripts\e2e.ps1 -Repeat 3`
   Add `-Shots` to save screenshots to `build\shots\<name>.png` and *look at them* after visual changes.
   Add `-Names code.cpp,folder` to restrict samples. The first line of a fresh process is the cold open.
   Don't touch mouse/keyboard while it runs (it drives Explorer with SendKeys).
3. Read log lines: `open <kind> explorer A + load B -> ready C ms, on-screen D ms`.
   `ready` = painted, `on-screen` = after `DwmFlush` (composed). `info thumbnail/count ready` lines
   give the async thumbnail time.
4. Memory: the scripts print working set and private bytes; compare idle private bytes (~20–25 MB).
5. To find where time goes, temporarily add `Log(L"PROF ...")` with `Now()`/`MsSince()` around
   the suspect steps, measure, then remove the PROF lines before committing.
6. Update the numbers table in AGENTS.md when results change meaningfully.
