---
name: add-language
description: Add or fix syntax highlighting for a file extension in Flick's text preview (Lexilla lexer, keywords, colours). Use when a code/config file type shows as plain text or is coloured wrong.
---

# Add a language

Everything is in `src/lang.cpp`.

1. Find the Lexilla lexer name: `grep -n 'LexerModule lm' third_party/lexilla/lexers/Lex*.cxx`
   (the quoted string, e.g. `"cpp"`, `"python"`, `"rust"`).
2. Find its style constants in `third_party/lexilla/include/SciLexer.h` (`SCE_<PREFIX>_*`).
3. Add a `StyleRole` table mapping each style to a role (`R_Comment`, `R_Keyword`, `R_String`, …).
   Colours per role live in the `kDark`/`kLight` palettes in `window.cpp`; don't add colours here.
4. Add keyword strings if the lexer uses word lists (check the lexer's `WordListDesc`; Lexilla
   keyword lists are space-separated; some lexers expect lowercase).
5. Add a `const Lang` and an entry in `kMap` with space-delimited lowercase extensions
   (or full file names like `makefile`), e.g. `{L" .zig .zon ", &kLangZig}`.
6. Build, then preview a sample file (e2e `-Names` or Explorer + Space) and check the screenshot.
