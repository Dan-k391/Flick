// Syntax colouring outside Scintilla (Markdown code blocks, notebooks): runs a Lexilla lexer over a
// minimal in-memory IDocument and maps its styles to colour roles. Microseconds for a code block.
#include "app.h"
#include <ILexer.h>
#include <Lexilla.h>
#include <algorithm>

namespace {

class Doc : public Scintilla::IDocument {
public:
    explicit Doc(std::string t) : text_(std::move(t)), styles_(text_.size(), 0) {
        starts_.push_back(0);
        for (size_t i = 0; i < text_.size(); i++)
            if (text_[i] == '\n') starts_.push_back((Sci_Position)i + 1);
        states_.assign(starts_.size() + 1, 0);
        levels_.assign(starts_.size() + 1, 0x400);  // SC_FOLDLEVELBASE
    }
    const std::vector<char>& Styles() const { return styles_; }

    int SCI_METHOD Version() const override { return Scintilla::dvRelease4; }
    void SCI_METHOD SetErrorStatus(int) override {}
    Sci_Position SCI_METHOD Length() const override { return (Sci_Position)text_.size(); }
    void SCI_METHOD GetCharRange(char* buffer, Sci_Position pos, Sci_Position len) const override {
        pos = std::clamp<Sci_Position>(pos, 0, Length());
        len = std::clamp<Sci_Position>(len, 0, Length() - pos);
        memcpy(buffer, text_.data() + pos, (size_t)len);
    }
    char SCI_METHOD StyleAt(Sci_Position pos) const override {
        return pos >= 0 && pos < Length() ? styles_[(size_t)pos] : 0;
    }
    Sci_Position SCI_METHOD LineFromPosition(Sci_Position pos) const override {
        return std::upper_bound(starts_.begin(), starts_.end(), pos) - starts_.begin() - 1;
    }
    Sci_Position SCI_METHOD LineStart(Sci_Position line) const override {
        return line <= 0 ? 0 : line < (Sci_Position)starts_.size() ? starts_[(size_t)line] : Length();
    }
    int SCI_METHOD GetLevel(Sci_Position line) const override { return Ok(line) ? levels_[(size_t)line] : 0x400; }
    int SCI_METHOD SetLevel(Sci_Position line, int level) override {
        if (Ok(line)) levels_[(size_t)line] = level;
        return level;
    }
    int SCI_METHOD GetLineState(Sci_Position line) const override { return Ok(line) ? states_[(size_t)line] : 0; }
    int SCI_METHOD SetLineState(Sci_Position line, int state) override {
        if (Ok(line)) states_[(size_t)line] = state;
        return state;
    }
    void SCI_METHOD StartStyling(Sci_Position pos) override { pos_ = pos; }
    bool SCI_METHOD SetStyleFor(Sci_Position len, char style) override {
        for (; len > 0 && pos_ < Length(); len--) styles_[(size_t)pos_++] = style;
        return true;
    }
    bool SCI_METHOD SetStyles(Sci_Position len, const char* styles) override {
        for (Sci_Position i = 0; i < len && pos_ < Length(); i++) styles_[(size_t)pos_++] = styles[i];
        return true;
    }
    void SCI_METHOD DecorationSetCurrentIndicator(int) override {}
    void SCI_METHOD DecorationFillRange(Sci_Position, int, Sci_Position) override {}
    void SCI_METHOD ChangeLexerState(Sci_Position, Sci_Position) override {}
    int SCI_METHOD CodePage() const override { return 65001; }
    bool SCI_METHOD IsDBCSLeadByte(char) const override { return false; }
    const char* SCI_METHOD BufferPointer() override { return text_.c_str(); }
    int SCI_METHOD GetLineIndentation(Sci_Position line) override {
        int indent = 0;
        for (Sci_Position i = LineStart(line); i < Length() && (text_[(size_t)i] == ' ' || text_[(size_t)i] == '\t'); i++)
            indent = text_[(size_t)i] == '\t' ? (indent / 4 + 1) * 4 : indent + 1;
        return indent;
    }
    Sci_Position SCI_METHOD LineEnd(Sci_Position line) const override {
        Sci_Position end = LineStart(line + 1);
        if (end > LineStart(line) && end <= Length() && text_[(size_t)end - 1] == '\n') end--;
        if (end > LineStart(line) && text_[(size_t)end - 1] == '\r') end--;
        return end;
    }
    Sci_Position SCI_METHOD GetRelativePosition(Sci_Position start, Sci_Position chars) const override {
        Sci_Position pos = start;
        for (; chars > 0 && pos < Length(); chars--) pos += Width(pos);
        for (; chars < 0 && pos > 0; chars++)
            do pos--;
            while (pos > 0 && ((unsigned char)text_[(size_t)pos] & 0xC0) == 0x80);
        return chars ? -1 : pos;
    }
    int SCI_METHOD GetCharacterAndWidth(Sci_Position pos, Sci_Position* width) const override {
        if (pos < 0 || pos >= Length()) {  // like Scintilla: NUL of width 1 (0 would stall the lexer)
            if (width) *width = 1;
            return 0;
        }
        int w = Width(pos), c = (unsigned char)text_[(size_t)pos];
        c &= w == 1 ? 0x7F : w == 2 ? 0x1F : w == 3 ? 0x0F : 0x07;
        for (int i = 1; i < w; i++) c = (c << 6) | ((unsigned char)text_[(size_t)pos + i] & 0x3F);
        if (width) *width = w;
        return c;
    }

private:
    bool Ok(Sci_Position line) const { return line >= 0 && line < (Sci_Position)states_.size(); }
    int Width(Sci_Position pos) const {
        unsigned char c = (unsigned char)text_[(size_t)pos];
        int w = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        return (int)std::min<Sci_Position>(w, Length() - pos);
    }
    std::string text_;
    std::vector<char> styles_;
    std::vector<Sci_Position> starts_;
    std::vector<int> states_, levels_;
    Sci_Position pos_ = 0;
};

}  // namespace

std::vector<StyleRun> Highlight(const std::wstring& code, const Lang* lang) {
    std::vector<StyleRun> runs;
    if (!lang || code.empty()) return runs;
    // UTF-8 for the lexer, remembering the UTF-16 index of every byte.
    std::string utf8;
    std::vector<UINT32> index;
    utf8.reserve(code.size()), index.reserve(code.size() + 1);
    for (size_t i = 0; i < code.size(); i++) {
        UINT32 c = code[i], at = (UINT32)i;
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < code.size()) c = 0x10000 + ((c - 0xD800) << 10) + (code[++i] - 0xDC00);
        char b[4];
        int n = c < 0x80 ? (b[0] = (char)c, 1)
                : c < 0x800 ? (b[0] = (char)(0xC0 | c >> 6), b[1] = (char)(0x80 | (c & 0x3F)), 2)
                : c < 0x10000 ? (b[0] = (char)(0xE0 | c >> 12), b[1] = (char)(0x80 | ((c >> 6) & 0x3F)), b[2] = (char)(0x80 | (c & 0x3F)), 3)
                              : (b[0] = (char)(0xF0 | c >> 18), b[1] = (char)(0x80 | ((c >> 12) & 0x3F)),
                                 b[2] = (char)(0x80 | ((c >> 6) & 0x3F)), b[3] = (char)(0x80 | (c & 0x3F)), 4);
        utf8.append(b, n);
        index.insert(index.end(), n, at);
    }
    index.push_back((UINT32)code.size());
    Scintilla::ILexer5* lexer = CreateLexer(lang->lexer);
    if (!lexer) return runs;
    lexer->WordListSet(0, lang->keywords0);
    lexer->WordListSet(1, lang->keywords1);
    Doc doc(std::move(utf8));
    lexer->Lex(0, doc.Length(), 0, &doc);
    lexer->Release();
    const std::vector<char>& styles = doc.Styles();
    auto roleOf = [&](char s) {
        for (size_t i = 0; i < lang->styleCount; i++)
            if (lang->styles[i].style == (unsigned char)s) return lang->styles[i].role;
        return R_Default;
    };
    for (size_t i = 0; i < styles.size();) {
        size_t j = i;
        while (j < styles.size() && styles[j] == styles[i]) j++;
        if (Role r = roleOf(styles[i]); r != R_Default) runs.push_back({index[i], index[j] - index[i], r});
        i = j;
    }
    return runs;
}
