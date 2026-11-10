---
name: build
description: Build Flick (Release or Debug) with MSVC/CMake/Ninja and report errors. Use whenever code changed and needs compiling, or before running or benchmarking.
---

# Build Flick

1. From the repo root run, via the PowerShell tool (cmd scripts don't run from bash):
   `cmd /c .\build.cmd 2>&1 | Select-String -Pattern "error|warning C|FAILED|Built"`
   Add `Debug` as an argument for a debug build. Output: `build\<cfg>\Flick.exe`.
2. `build.cmd` kills a running `Flick.exe` first (LNK1104 otherwise) — expect the resident instance to stop.
3. Compiler messages may be localized (e.g. Chinese). Look at the error code (C2001, LNK1104…).
4. A first build compiles all of Scintilla/Lexilla (~180 files, ~1 min); later builds are incremental.
5. To run afterwards: `Start-Process .\build\Release\Flick.exe` (resident; `Flick.exe --exit` quits).
