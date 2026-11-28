#include "app.h"
#include <ILexer.h>
#include <Lexilla.h>
#include <SciLexer.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace {
struct Sample {
    const char* lexer;
    const wchar_t* file;
    const wchar_t* fence;
    const wchar_t* code;
    Role role;
};

constexpr Sample kSamples[] = {
    {"bash", L"Dockerfile", L"shell", L"# comment\n", R_Comment},
    {"batch", L"a.cmd", L"batch", L"REM comment\n", R_Comment},
    {"cmake", L"CMakeLists.txt", L"cmake", L"# comment\n", R_Comment},
    {"cpp", L"a.hpp", L"C++", L"int n = 1;\n", R_Keyword},
    {"cpp", L"a.cs", L"csharp", L"namespace N {}\n", R_Keyword},
    {"cpp", L"a.kt", L"kotlin", L"// comment\n", R_Comment},
    {"cpp", L"a.ts", L"typescript", L"const n = 1;\n", R_Keyword},
    {"cpp", L"a.go", L"golang", L"package main\n", R_Keyword},
    {"cpp", L"a.mm", L"objective-c", L"// comment\n", R_Comment},
    {"css", L"a.less", L"css", L"/* comment */\n", R_Comment},
    {"diff", L"a.patch", L"diff", L"+added\n", R_Added},
    {"hypertext", L"a.php", L"html", L"<div>text</div>\n", R_Tag},
    {"xml", L"a.xsd", L"xml", L"<node/>\n", R_Tag},
    {"json", L"a.jsonc", L"json", L"[17]\n", R_Number},
    {"lua", L"a.lua", L"lua", L"-- comment\n", R_Comment},
    {"makefile", L"GNUmakefile", L"make", L"# comment\n", R_Comment},
    {"markdown", L"a.mdx", L"markdown", L"# Heading\n", R_Heading},
    {"powershell", L"a.psm1", L"pwsh", L"# comment\n", R_Comment},
    {"props", L".editorconfig", L"ini", L"; comment\n", R_Comment},
    {"python", L"a.pyi", L"IPython3", L"# comment\n", R_Comment},
    {"rust", L"a.rs", L"rust", L"// comment\n", R_Comment},
    {"sql", L"a.sql", L"sql", L"-- comment\n", R_Comment},
    {"toml", L"a.toml", L"toml", L"# comment\n", R_Comment},
    {"yaml", L"a.yml", L"yaml", L"# comment\n", R_Comment},
};

bool HasRole(const std::vector<StyleRun>& runs, Role role) {
    return std::ranges::any_of(runs, [role](const StyleRun& run) { return run.role == role && run.len > 0; });
}

bool Check(const Sample& sample) {
    const Lang* file = LangForFile(sample.file);
    const Lang* fence = LangForName(sample.fence);
    if (!file || file != fence || std::strcmp(file->lexer, sample.lexer)) return false;
    auto* lexer = CreateLexer(file->lexer);
    if (!lexer) return false;
    int id = lexer->GetIdentifier();
    lexer->Release();
    // The two names share an upstream source file but create different lexer modes.
    if ((!std::strcmp(sample.lexer, "xml") && id != SCLEX_XML) ||
        (!std::strcmp(sample.lexer, "hypertext") && id != SCLEX_HTML)) return false;
    return HasRole(Highlight(sample.code, fence), sample.role);
}

bool CheckSkippedChunks(const wchar_t* fence, const wchar_t* open, const wchar_t* close, Role role) {
    auto* lang = LangForName(fence);
    std::wstring code = open;
    for (int i = 0; i < 98; i++) code += L"still quoted\n";
    UINT32 earlier = (UINT32)code.size();
    code += L"still quoted\n";
    UINT32 jump = (UINT32)code.size();
    code += L"visible quoted line\n";
    UINT32 end = (UINT32)code.size();
    code += close;
    auto lex = LexBegin(code, lang);
    if (!lex || !HasRole(LexRange(*lex, jump, end), role)) return false;
    // A backscroll reads earlier styles from the same lexer document. Forwards, the state closes again.
    return HasRole(LexRange(*lex, earlier, jump), role) &&
           HasRole(LexRange(*lex, end, (UINT32)code.size()), R_Keyword);
}
}

int main() {
    bool ok = true;
    for (auto& sample : kSamples)
        if (!Check(sample)) {
            std::fwprintf(stderr, L"Lexer failed for %ls or fence %ls\n", sample.file, sample.fence);
            ok = false;
        }
    if (!CheckSkippedChunks(L"cpp", L"/*\n", L"*/\nint n = 1;\n", R_Comment) ||
        !CheckSkippedChunks(L"python", L"\"\"\"\n", L"\"\"\"\ndef f(): pass\n", R_String)) {
        std::fputs("Lexer state failed across skipped chunks\n", stderr);
        ok = false;
    }
    return ok ? 0 : 1;
}
