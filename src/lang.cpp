// Maps file extensions to Lexilla lexers, keyword lists and colour roles.
#include "app.h"
#include <shlwapi.h>
#include <iterator>
#include <SciLexer.h>

#define STYLES(a) a, std::size(a)

namespace {

const StyleRole kC[] = {
    {SCE_C_COMMENT, R_Comment}, {SCE_C_COMMENTLINE, R_Comment}, {SCE_C_COMMENTDOC, R_Comment},
    {SCE_C_COMMENTLINEDOC, R_Comment}, {SCE_C_COMMENTDOCKEYWORD, R_Comment}, {SCE_C_PREPROCESSORCOMMENT, R_Comment},
    {SCE_C_NUMBER, R_Number}, {SCE_C_USERLITERAL, R_Number}, {SCE_C_WORD, R_Keyword}, {SCE_C_WORD2, R_Type},
    {SCE_C_GLOBALCLASS, R_Type}, {SCE_C_STRING, R_String}, {SCE_C_CHARACTER, R_String}, {SCE_C_VERBATIM, R_String},
    {SCE_C_STRINGRAW, R_String}, {SCE_C_TRIPLEVERBATIM, R_String}, {SCE_C_REGEX, R_String}, {SCE_C_STRINGEOL, R_String},
    {SCE_C_HASHQUOTEDSTRING, R_String}, {SCE_C_ESCAPESEQUENCE, R_Preproc}, {SCE_C_PREPROCESSOR, R_Preproc},
    {SCE_C_OPERATOR, R_Operator}, {SCE_C_TASKMARKER, R_Keyword},
};
const StyleRole kPython[] = {
    {SCE_P_COMMENTLINE, R_Comment}, {SCE_P_COMMENTBLOCK, R_Comment}, {SCE_P_NUMBER, R_Number},
    {SCE_P_STRING, R_String}, {SCE_P_CHARACTER, R_String}, {SCE_P_TRIPLE, R_String}, {SCE_P_TRIPLEDOUBLE, R_String},
    {SCE_P_FSTRING, R_String}, {SCE_P_FCHARACTER, R_String}, {SCE_P_FTRIPLE, R_String}, {SCE_P_FTRIPLEDOUBLE, R_String},
    {SCE_P_STRINGEOL, R_String}, {SCE_P_WORD, R_Keyword}, {SCE_P_WORD2, R_Type}, {SCE_P_CLASSNAME, R_Type},
    {SCE_P_DEFNAME, R_Func}, {SCE_P_DECORATOR, R_Preproc}, {SCE_P_OPERATOR, R_Operator},
};
const StyleRole kRust[] = {
    {SCE_RUST_COMMENTBLOCK, R_Comment}, {SCE_RUST_COMMENTLINE, R_Comment}, {SCE_RUST_COMMENTBLOCKDOC, R_Comment},
    {SCE_RUST_COMMENTLINEDOC, R_Comment}, {SCE_RUST_NUMBER, R_Number}, {SCE_RUST_WORD, R_Keyword},
    {SCE_RUST_WORD2, R_Type}, {SCE_RUST_STRING, R_String}, {SCE_RUST_STRINGR, R_String},
    {SCE_RUST_CHARACTER, R_String}, {SCE_RUST_BYTESTRING, R_String}, {SCE_RUST_BYTESTRINGR, R_String},
    {SCE_RUST_BYTECHARACTER, R_String}, {SCE_RUST_CSTRING, R_String}, {SCE_RUST_CSTRINGR, R_String},
    {SCE_RUST_OPERATOR, R_Operator}, {SCE_RUST_LIFETIME, R_Preproc}, {SCE_RUST_MACRO, R_Func},
    {SCE_RUST_LEXERROR, R_Error},
};
const StyleRole kJson[] = {
    {SCE_JSON_NUMBER, R_Number}, {SCE_JSON_STRING, R_String}, {SCE_JSON_STRINGEOL, R_String},
    {SCE_JSON_PROPERTYNAME, R_Attr}, {SCE_JSON_ESCAPESEQUENCE, R_Preproc}, {SCE_JSON_LINECOMMENT, R_Comment},
    {SCE_JSON_BLOCKCOMMENT, R_Comment}, {SCE_JSON_OPERATOR, R_Operator}, {SCE_JSON_URI, R_String},
    {SCE_JSON_KEYWORD, R_Keyword}, {SCE_JSON_LDKEYWORD, R_Keyword}, {SCE_JSON_ERROR, R_Error},
};
const StyleRole kHtml[] = {
    {SCE_H_TAG, R_Tag}, {SCE_H_TAGUNKNOWN, R_Tag}, {SCE_H_TAGEND, R_Tag}, {SCE_H_XMLSTART, R_Tag},
    {SCE_H_XMLEND, R_Tag}, {SCE_H_ATTRIBUTE, R_Attr}, {SCE_H_ATTRIBUTEUNKNOWN, R_Attr}, {SCE_H_NUMBER, R_Number},
    {SCE_H_DOUBLESTRING, R_String}, {SCE_H_SINGLESTRING, R_String}, {SCE_H_VALUE, R_String}, {SCE_H_CDATA, R_String},
    {SCE_H_COMMENT, R_Comment}, {SCE_H_ENTITY, R_Preproc}, {SCE_H_SGML_DEFAULT, R_Preproc},
    {SCE_H_SGML_COMMAND, R_Keyword}, {SCE_H_SGML_COMMENT, R_Comment},
    // embedded <script>
    {SCE_HJ_COMMENT, R_Comment}, {SCE_HJ_COMMENTLINE, R_Comment}, {SCE_HJ_COMMENTDOC, R_Comment},
    {SCE_HJ_NUMBER, R_Number}, {SCE_HJ_KEYWORD, R_Keyword}, {SCE_HJ_DOUBLESTRING, R_String},
    {SCE_HJ_SINGLESTRING, R_String}, {SCE_HJ_TEMPLATELITERAL, R_String}, {SCE_HJ_REGEX, R_String},
};
const StyleRole kCss[] = {
    {SCE_CSS_TAG, R_Tag}, {SCE_CSS_CLASS, R_Type}, {SCE_CSS_ID, R_Type}, {SCE_CSS_PSEUDOCLASS, R_Type},
    {SCE_CSS_UNKNOWN_PSEUDOCLASS, R_Type}, {SCE_CSS_PSEUDOELEMENT, R_Type}, {SCE_CSS_EXTENDED_PSEUDOCLASS, R_Type},
    {SCE_CSS_EXTENDED_PSEUDOELEMENT, R_Type}, {SCE_CSS_OPERATOR, R_Operator}, {SCE_CSS_IDENTIFIER, R_Attr},
    {SCE_CSS_UNKNOWN_IDENTIFIER, R_Attr}, {SCE_CSS_IDENTIFIER2, R_Attr}, {SCE_CSS_IDENTIFIER3, R_Attr},
    {SCE_CSS_EXTENDED_IDENTIFIER, R_Attr}, {SCE_CSS_ATTRIBUTE, R_Attr}, {SCE_CSS_VALUE, R_String},
    {SCE_CSS_DOUBLESTRING, R_String}, {SCE_CSS_SINGLESTRING, R_String}, {SCE_CSS_COMMENT, R_Comment},
    {SCE_CSS_IMPORTANT, R_Keyword}, {SCE_CSS_DIRECTIVE, R_Keyword}, {SCE_CSS_GROUP_RULE, R_Keyword},
    {SCE_CSS_VARIABLE, R_Preproc},
};
const StyleRole kMarkdown[] = {
    {SCE_MARKDOWN_STRONG1, R_Strong}, {SCE_MARKDOWN_STRONG2, R_Strong}, {SCE_MARKDOWN_EM1, R_Em},
    {SCE_MARKDOWN_EM2, R_Em}, {SCE_MARKDOWN_HEADER1, R_Heading}, {SCE_MARKDOWN_HEADER2, R_Heading},
    {SCE_MARKDOWN_HEADER3, R_Heading}, {SCE_MARKDOWN_HEADER4, R_Heading}, {SCE_MARKDOWN_HEADER5, R_Heading},
    {SCE_MARKDOWN_HEADER6, R_Heading}, {SCE_MARKDOWN_ULIST_ITEM, R_Keyword}, {SCE_MARKDOWN_OLIST_ITEM, R_Keyword},
    {SCE_MARKDOWN_BLOCKQUOTE, R_Comment}, {SCE_MARKDOWN_STRIKEOUT, R_Comment}, {SCE_MARKDOWN_HRULE, R_Comment},
    {SCE_MARKDOWN_LINK, R_String}, {SCE_MARKDOWN_CODE, R_Type}, {SCE_MARKDOWN_CODE2, R_Type},
    {SCE_MARKDOWN_CODEBK, R_Type}, {SCE_MARKDOWN_FRONT_MARK, R_Comment}, {SCE_MARKDOWN_FRONT_KEY, R_Attr},
};
const StyleRole kYaml[] = {
    {SCE_YAML_COMMENT, R_Comment}, {SCE_YAML_IDENTIFIER, R_Attr}, {SCE_YAML_KEYWORD, R_Keyword},
    {SCE_YAML_NUMBER, R_Number}, {SCE_YAML_REFERENCE, R_Preproc}, {SCE_YAML_DOCUMENT, R_Heading},
    {SCE_YAML_TEXT, R_String}, {SCE_YAML_ERROR, R_Error}, {SCE_YAML_OPERATOR, R_Operator},
};
const StyleRole kBash[] = {
    {SCE_SH_ERROR, R_Error}, {SCE_SH_COMMENTLINE, R_Comment}, {SCE_SH_NUMBER, R_Number}, {SCE_SH_WORD, R_Keyword},
    {SCE_SH_STRING, R_String}, {SCE_SH_CHARACTER, R_String}, {SCE_SH_OPERATOR, R_Operator},
    {SCE_SH_SCALAR, R_Attr}, {SCE_SH_PARAM, R_Attr}, {SCE_SH_BACKTICKS, R_Preproc},
    {SCE_SH_HERE_DELIM, R_Preproc}, {SCE_SH_HERE_Q, R_String},
};
const StyleRole kPowerShell[] = {
    {SCE_POWERSHELL_COMMENT, R_Comment}, {SCE_POWERSHELL_COMMENTSTREAM, R_Comment},
    {SCE_POWERSHELL_COMMENTDOCKEYWORD, R_Comment}, {SCE_POWERSHELL_STRING, R_String},
    {SCE_POWERSHELL_CHARACTER, R_String}, {SCE_POWERSHELL_HERE_STRING, R_String},
    {SCE_POWERSHELL_HERE_CHARACTER, R_String}, {SCE_POWERSHELL_NUMBER, R_Number},
    {SCE_POWERSHELL_VARIABLE, R_Attr}, {SCE_POWERSHELL_OPERATOR, R_Operator}, {SCE_POWERSHELL_KEYWORD, R_Keyword},
    {SCE_POWERSHELL_CMDLET, R_Func}, {SCE_POWERSHELL_ALIAS, R_Func}, {SCE_POWERSHELL_FUNCTION, R_Func},
};
const StyleRole kBatch[] = {
    {SCE_BAT_COMMENT, R_Comment}, {SCE_BAT_WORD, R_Keyword}, {SCE_BAT_LABEL, R_Heading},
    {SCE_BAT_HIDE, R_Preproc}, {SCE_BAT_COMMAND, R_Func}, {SCE_BAT_IDENTIFIER, R_Attr}, {SCE_BAT_OPERATOR, R_Operator},
};
const StyleRole kSql[] = {
    {SCE_SQL_COMMENT, R_Comment}, {SCE_SQL_COMMENTLINE, R_Comment}, {SCE_SQL_COMMENTDOC, R_Comment},
    {SCE_SQL_COMMENTLINEDOC, R_Comment}, {SCE_SQL_NUMBER, R_Number}, {SCE_SQL_WORD, R_Keyword},
    {SCE_SQL_WORD2, R_Type}, {SCE_SQL_STRING, R_String}, {SCE_SQL_CHARACTER, R_String},
    {SCE_SQL_OPERATOR, R_Operator}, {SCE_SQL_QUOTEDIDENTIFIER, R_Attr},
};
const StyleRole kLua[] = {
    {SCE_LUA_COMMENT, R_Comment}, {SCE_LUA_COMMENTLINE, R_Comment}, {SCE_LUA_COMMENTDOC, R_Comment},
    {SCE_LUA_NUMBER, R_Number}, {SCE_LUA_WORD, R_Keyword}, {SCE_LUA_WORD2, R_Func}, {SCE_LUA_STRING, R_String},
    {SCE_LUA_CHARACTER, R_String}, {SCE_LUA_LITERALSTRING, R_String}, {SCE_LUA_STRINGEOL, R_String},
    {SCE_LUA_PREPROCESSOR, R_Preproc}, {SCE_LUA_OPERATOR, R_Operator}, {SCE_LUA_LABEL, R_Preproc},
};
const StyleRole kProps[] = {
    {SCE_PROPS_COMMENT, R_Comment}, {SCE_PROPS_SECTION, R_Heading}, {SCE_PROPS_ASSIGNMENT, R_Operator},
    {SCE_PROPS_DEFVAL, R_Preproc}, {SCE_PROPS_KEY, R_Attr},
};
const StyleRole kToml[] = {
    {SCE_TOML_COMMENT, R_Comment}, {SCE_TOML_KEYWORD, R_Keyword}, {SCE_TOML_NUMBER, R_Number},
    {SCE_TOML_TABLE, R_Heading}, {SCE_TOML_KEY, R_Attr}, {SCE_TOML_ERROR, R_Error}, {SCE_TOML_OPERATOR, R_Operator},
    {SCE_TOML_STRING_SQ, R_String}, {SCE_TOML_STRING_DQ, R_String}, {SCE_TOML_TRIPLE_STRING_SQ, R_String},
    {SCE_TOML_TRIPLE_STRING_DQ, R_String}, {SCE_TOML_ESCAPECHAR, R_Preproc}, {SCE_TOML_DATETIME, R_Number},
    {SCE_TOML_STRINGEOL, R_String},
};
const StyleRole kDiff[] = {
    {SCE_DIFF_COMMENT, R_Comment}, {SCE_DIFF_COMMAND, R_Keyword}, {SCE_DIFF_HEADER, R_Heading},
    {SCE_DIFF_POSITION, R_Preproc}, {SCE_DIFF_DELETED, R_Deleted}, {SCE_DIFF_ADDED, R_Added},
    {SCE_DIFF_CHANGED, R_Type}, {SCE_DIFF_PATCH_ADD, R_Added}, {SCE_DIFF_PATCH_DELETE, R_Deleted},
};
const StyleRole kMake[] = {
    {SCE_MAKE_COMMENT, R_Comment}, {SCE_MAKE_PREPROCESSOR, R_Preproc}, {SCE_MAKE_IDENTIFIER, R_Attr},
    {SCE_MAKE_OPERATOR, R_Operator}, {SCE_MAKE_TARGET, R_Func}, {SCE_MAKE_IDEOL, R_Error},
};
const StyleRole kCMake[] = {
    {SCE_CMAKE_COMMENT, R_Comment}, {SCE_CMAKE_STRINGDQ, R_String}, {SCE_CMAKE_STRINGLQ, R_String},
    {SCE_CMAKE_STRINGRQ, R_String}, {SCE_CMAKE_COMMANDS, R_Keyword}, {SCE_CMAKE_PARAMETERS, R_Attr},
    {SCE_CMAKE_VARIABLE, R_Preproc}, {SCE_CMAKE_USERDEFINED, R_Func}, {SCE_CMAKE_WHILEDEF, R_Keyword},
    {SCE_CMAKE_FOREACHDEF, R_Keyword}, {SCE_CMAKE_IFDEFINEDEF, R_Keyword}, {SCE_CMAKE_MACRODEF, R_Keyword},
    {SCE_CMAKE_STRINGVAR, R_Preproc}, {SCE_CMAKE_NUMBER, R_Number},
};

const char kCppKw[] =
    "alignas alignof auto bool break case catch char char8_t char16_t char32_t class co_await co_return co_yield "
    "concept const consteval constexpr constinit const_cast continue decltype default delete do double dynamic_cast "
    "else enum explicit export extern false final float for friend goto if inline int long mutable namespace new "
    "noexcept nullptr operator override private protected public register reinterpret_cast requires return short "
    "signed sizeof static static_assert static_cast struct switch template this thread_local throw true try typedef "
    "typeid typename union unsigned using virtual void volatile wchar_t while";
const char kCsKw[] =
    "abstract as async await base bool break byte case catch char checked class const continue decimal default "
    "delegate do double else enum event explicit extern false finally fixed float for foreach get goto if implicit "
    "in init int interface internal is lock long namespace new null object operator out override params private "
    "protected public readonly record ref return sbyte sealed set short sizeof stackalloc static string struct "
    "switch this throw true try typeof uint ulong unchecked unsafe ushort using var virtual void volatile where "
    "while yield";
const char kJavaKw[] =
    "abstract assert boolean break byte case catch char class const continue default do double else enum extends "
    "false final finally float for goto if implements import instanceof int interface long native new null package "
    "private protected public record return short static strictfp super switch synchronized this throw throws "
    "transient true try var void volatile while";
const char kJsKw[] =
    "abstract any as async await boolean break case catch class const constructor continue debugger declare default "
    "delete do else enum export extends false finally for from function get if implements import in instanceof "
    "interface keyof let namespace never new null number of package private protected public readonly return set "
    "static string super switch this throw true try type typeof undefined unknown var void while with yield";
const char kGoKw[] =
    "break case chan const continue default defer else fallthrough false for func go goto if import interface iota "
    "map nil package range return select struct switch true type var";
const char kGoTypes[] =
    "bool byte complex64 complex128 error float32 float64 int int8 int16 int32 int64 rune string uint uint8 uint16 "
    "uint32 uint64 uintptr any";
const char kPyKw[] =
    "False None True and as assert async await break case class continue def del elif else except finally for from "
    "global if import in is lambda match nonlocal not or pass raise return try while with yield";
const char kPyBuiltins[] = "self cls print len range int str float list dict set tuple bool bytes object type super";
const char kRustKw[] =
    "as async await break const continue crate dyn else enum extern false fn for if impl in let loop match mod move "
    "mut pub ref return self Self static struct super trait true type unsafe use where while";
const char kRustTypes[] =
    "bool char f32 f64 i8 i16 i32 i64 i128 isize str u8 u16 u32 u64 u128 usize String Vec Option Result Box Some "
    "None Ok Err";
const char kSqlKw[] =
    "add all alter and as asc begin between by case check column commit constraint create cross database default "
    "delete desc distinct drop else end exists foreign from full group having if in index inner insert into is join "
    "key left like limit not null offset on or order outer primary references right rollback select set table then "
    "transaction union unique update values view when where with";
const char kLuaKw[] =
    "and break do else elseif end false for function goto if in local nil not or repeat return then true until while";
const char kShKw[] =
    "if then else elif fi case esac for while until do done in function return exit export local readonly declare "
    "echo source alias unset shift break continue cd test";
const char kPsKw[] =
    "begin break catch class continue data define do dynamicparam else elseif end enum exit filter finally for "
    "foreach from function if in param process return switch throw trap try until using var while";
const char kBatKw[] =
    "rem set if else exist errorlevel for in do break call copy chcp cd chdir choice cls date del erase dir echo "
    "exit goto mkdir md move path pause prompt rename ren rmdir rd shift time type ver vol defined not setlocal "
    "endlocal pushd popd start title equ neq lss leq gtr geq";
const char kCMakeKw[] =
    "add_compile_definitions add_compile_options add_custom_command add_custom_target add_definitions "
    "add_dependencies add_executable add_library add_subdirectory add_test cmake_minimum_required configure_file "
    "else elseif endforeach endfunction endif endmacro endwhile file find_package find_library find_path foreach "
    "function get_filename_component if include include_directories install list macro message option project "
    "return set set_property set_target_properties string target_compile_definitions target_compile_features "
    "target_compile_options target_include_directories target_link_libraries target_sources while";

const Lang kLangC{"cpp", kCppKw, "", STYLES(kC)};
const Lang kLangCs{"cpp", kCsKw, "", STYLES(kC)};
const Lang kLangJava{"cpp", kJavaKw, "", STYLES(kC)};
const Lang kLangJs{"cpp", kJsKw, "", STYLES(kC)};
const Lang kLangGo{"cpp", kGoKw, kGoTypes, STYLES(kC)};
const Lang kLangCLike{"cpp", "", "", STYLES(kC)};  // strings/comments/numbers only
const Lang kLangPython{"python", kPyKw, kPyBuiltins, STYLES(kPython)};
const Lang kLangRust{"rust", kRustKw, kRustTypes, STYLES(kRust)};
const Lang kLangJson{"json", "true false null", "", STYLES(kJson)};
const Lang kLangXml{"xml", "", "", STYLES(kHtml)};
const Lang kLangHtml{"hypertext", "", kJsKw, STYLES(kHtml)};
const Lang kLangCss{"css", "", "", STYLES(kCss)};
const Lang kLangMarkdown{"markdown", "", "", STYLES(kMarkdown)};
const Lang kLangYaml{"yaml", "true false yes no null on off", "", STYLES(kYaml)};
const Lang kLangBash{"bash", kShKw, "", STYLES(kBash)};
const Lang kLangPs{"powershell", kPsKw, "", STYLES(kPowerShell)};
const Lang kLangBatch{"batch", kBatKw, "", STYLES(kBatch)};
const Lang kLangSql{"sql", kSqlKw, "", STYLES(kSql)};
const Lang kLangLua{"lua", kLuaKw, "", STYLES(kLua)};
const Lang kLangProps{"props", "", "", STYLES(kProps)};
const Lang kLangToml{"toml", "true false", "", STYLES(kToml)};
const Lang kLangDiff{"diff", "", "", STYLES(kDiff)};
const Lang kLangMake{"makefile", "", "", STYLES(kMake)};
const Lang kLangCMake{"cmake", kCMakeKw, "", STYLES(kCMake)};

// Space-delimited lowercase extensions (with dot) or full file names.
const struct { const wchar_t* names; const Lang* lang; } kMap[] = {
    {L" .c .h .cpp .cc .cxx .c++ .hpp .hh .hxx .h++ .inl .ipp .ixx .cppm .ino .cu .cuh .hlsl .glsl .frag .vert ", &kLangC},
    {L" .cs .csx ", &kLangCs},
    {L" .java .kt .kts .scala .groovy .gradle ", &kLangJava},
    {L" .js .mjs .cjs .jsx .ts .mts .cts .tsx ", &kLangJs},
    {L" .go ", &kLangGo},
    {L" .swift .dart .m .mm .d .zig .proto .fs .fsx .v .sv .vhd ", &kLangCLike},
    {L" .py .pyw .pyi ", &kLangPython},
    {L" .rs ", &kLangRust},
    {L" .json .jsonc .json5 .geojson .webmanifest .har .ipynb .babelrc .eslintrc .prettierrc ", &kLangJson},
    {L" .xml .xaml .xsd .xsl .xslt .svg .csproj .vbproj .fsproj .vcxproj .props .targets .nuspec .config "
     L".manifest .resx .plist .wsdl .rss .atom .kml .gpx .appxmanifest ", &kLangXml},
    {L" .html .htm .xhtml .vue .svelte .php .asp .aspx .cshtml .razor ", &kLangHtml},
    {L" .css .scss .sass .less ", &kLangCss},
    {L" .md .markdown .mdx .mkd ", &kLangMarkdown},
    {L" .yml .yaml ", &kLangYaml},
    {L" .sh .bash .zsh .ksh .fish .bashrc .zshrc .profile .bash_profile ", &kLangBash},
    {L" .ps1 .psm1 .psd1 ", &kLangPs},
    {L" .bat .cmd ", &kLangBatch},
    {L" .sql ", &kLangSql},
    {L" .lua ", &kLangLua},
    {L" .ini .cfg .conf .properties .editorconfig .gitconfig .gitattributes .env .reg .inf .desktop .npmrc ", &kLangProps},
    {L" .toml ", &kLangToml},
    {L" .diff .patch ", &kLangDiff},
    {L" makefile gnumakefile .mk .mak ", &kLangMake},
    {L" cmakelists.txt .cmake ", &kLangCMake},
    {L" dockerfile ", &kLangBash},
};

bool InList(const wchar_t* list, const std::wstring& word) {
    std::wstring w = L" " + word + L" ";
    return !word.empty() && wcsstr(list, w.c_str());
}

}  // namespace

const Lang* LangForFile(const std::wstring& path) {
    std::wstring name = PathFindFileNameW(path.c_str());
    for (auto& c : name) c = towlower(c);
    std::wstring ext = PathFindExtensionW(name.c_str());
    for (auto& m : kMap)
        if (InList(m.names, name) || InList(m.names, ext)) return m.lang;
    return nullptr;
}

// Fenced-code info strings and notebook languages ("python", "c++", "shell", ...) to a language.
const Lang* LangForName(std::wstring name) {
    for (auto& c : name) c = towlower(c);
    static const std::pair<const wchar_t*, const wchar_t*> alias[] = {
        {L"python", L"py"},     {L"python3", L"py"},   {L"ipython", L"py"},     {L"ipython3", L"py"},
        {L"javascript", L"js"}, {L"node", L"js"},      {L"typescript", L"ts"},  {L"shell", L"sh"},
        {L"console", L"sh"},    {L"shellsession", L"sh"}, {L"zsh", L"sh"},      {L"powershell", L"ps1"},
        {L"pwsh", L"ps1"},      {L"ps", L"ps1"},       {L"csharp", L"cs"},      {L"c#", L"cs"},
        {L"rust", L"rs"},       {L"golang", L"go"},    {L"kotlin", L"kt"},      {L"objective-c", L"m"},
        {L"objc", L"m"},        {L"docker", L"sh"},    {L"dockerfile", L"sh"},  {L"batch", L"bat"},
        {L"make", L"mk"},       {L"makefile", L"mk"},  {L"markdown", L"md"},    {L"ini", L"ini"},
    };
    for (auto [n, ext] : alias)
        if (name == n) {
            name = ext;
            break;
        }
    return name.empty() ? nullptr : LangForFile(L"x." + name);
}
