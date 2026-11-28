// Flick uses the static CreateLexer entry point from Lexilla's API.
#include <cstddef>
#include <cstring>
#include <ILexer.h>
#include <Lexilla.h>
#include "LexerModule.h"

#define FLICK_LEXER(file, module) extern const Lexilla::LexerModule module;
#include "lexers.inc"
#undef FLICK_LEXER

namespace {
constexpr const Lexilla::LexerModule* kModules[] = {
#define FLICK_LEXER(file, module) &module,
#include "lexers.inc"
#undef FLICK_LEXER
};
}

ILexer5* LEXILLA_CALL CreateLexer(const char* name) {
    for (auto* module : kModules)
        if (std::strcmp(module->languageName, name) == 0) return module->Create();
    return nullptr;
}
