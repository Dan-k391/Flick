// Markdown preview: md4c parses the text into blocks (paragraphs, headings, code, lists, quotes,
// tables); DirectWrite lays them out lazily, only as far as the view has scrolled, so opening a long
// document costs about the same as a short one. Raw HTML is not rendered (text between tags shows).
#include "app.h"
#include <algorithm>
#include <cmath>
#include "md4c.h"

using Microsoft::WRL::ComPtr;

namespace {

enum RunKind { RK_None = -1, RK_Strong, RK_Em, RK_Code, RK_Link, RK_Del };
struct Run { UINT32 start, len; RunKind kind; };
struct Text {
    std::wstring s;
    std::vector<Run> runs;
    int align = MD_ALIGN_DEFAULT;
    ComPtr<IDWriteTextLayout> layout;
    std::vector<D2D1_RECT_F> codeRects;  // inline-code backgrounds, layout coordinates
};
enum BlockKind { B_Para, B_Head, B_Code, B_Rule, B_Table };
struct Block {
    BlockKind kind;
    int level = 0, indent = 0, quote = 0;  // heading level, list depth, quote depth
    bool tight = false;                    // text directly in a tight list item: small gap after
    std::wstring bullet;
    Text text;
    std::vector<std::vector<Text>> rows;  // tables; rows[0] is the header
    std::vector<float> colX, rowY;        // tables: cell edges relative to the block origin
    float y = 0, h = 0, after = 0;        // laid-out position in the document (DIPs)
};

constexpr float kIndent = 26, kQuote = 18, kCodePad = 12, kCellPadX = 10, kCellPadY = 6, kMaxWidth = 860;

IDWriteFactory* g_dw;
std::wstring g_mono;
ComPtr<IDWriteTextFormat> g_fmtBody, g_fmtCode, g_fmtBullet, g_fmtHead[6];
std::vector<Block> g_blocks;
size_t g_laid;  // blocks [0, g_laid) are laid out for g_width
float g_width = -1, g_laidH, g_prevAfter;
ID2D1RenderTarget* g_brushRt;
ComPtr<ID2D1SolidColorBrush> g_link, g_dim;  // drawing effects stored in layouts

// ---- parsing
std::wstring Entity(std::wstring_view e) {
    if (e.size() > 3 && e[1] == L'#') {
        bool hex = e[2] == L'x' || e[2] == L'X';
        unsigned long cp = wcstoul(e.data() + (hex ? 3 : 2), nullptr, hex ? 16 : 10);
        if (cp == 0 || cp > 0x10FFFF) return L"�";
        if (cp < 0x10000) return std::wstring(1, (wchar_t)cp);
        cp -= 0x10000;
        return {(wchar_t)(0xD800 + (cp >> 10)), (wchar_t)(0xDC00 + (cp & 0x3FF))};
    }
    static const std::pair<std::wstring_view, wchar_t> named[] = {
        {L"&amp;", L'&'},     {L"&lt;", L'<'},     {L"&gt;", L'>'},      {L"&quot;", L'"'},     {L"&apos;", L'\''},
        {L"&nbsp;", L'\xA0'}, {L"&copy;", L'©'},   {L"&reg;", L'®'},     {L"&trade;", L'™'},    {L"&mdash;", L'—'},
        {L"&ndash;", L'–'},   {L"&hellip;", L'…'}, {L"&rarr;", L'→'},    {L"&larr;", L'←'},     {L"&times;", L'×'}};
    for (auto& [name, ch] : named)
        if (e == name) return std::wstring(1, ch);
    return std::wstring(e);
}

struct Parser {
    std::vector<int> lists;  // per nesting level: next item number, or -1 for bullets
    int quote = 0;
    bool open = false, table = false;  // a block (or table cell) is receiving text
    std::wstring bullet;               // marker for the next block started inside a list item
    std::vector<std::pair<UINT32, RunKind>> spans;

    void End() {
        if (!open || table) return;
        open = false;
        Block& b = g_blocks.back();
        while (b.kind == B_Code && !b.text.s.empty() && b.text.s.back() == L'\n') b.text.s.pop_back();
        if (b.kind == B_Para && b.text.s.empty() && b.bullet.empty()) g_blocks.pop_back();  // e.g. an HTML block
    }
    void Start(BlockKind k, int level = 0) {
        End();
        Block& b = g_blocks.emplace_back(Block{k, level, (int)lists.size(), quote});
        b.bullet = std::move(bullet);
        bullet.clear();
        open = true;
    }
    Text& Target() {  // tight list items have no paragraph block: start one on first text
        if (!open) Start(B_Para), g_blocks.back().tight = true;
        Block& b = g_blocks.back();
        return table ? b.rows.back().back() : b.text;
    }
};

int EnterBlock(MD_BLOCKTYPE type, void* d, void* user) {
    Parser& p = *static_cast<Parser*>(user);
    switch (type) {
        case MD_BLOCK_QUOTE: p.End(), p.quote++; break;
        case MD_BLOCK_UL: p.End(), p.lists.push_back(-1); break;
        case MD_BLOCK_OL: p.End(), p.lists.push_back(static_cast<MD_BLOCK_OL_DETAIL*>(d)->start); break;
        case MD_BLOCK_LI: {
            p.End();
            auto* li = static_cast<MD_BLOCK_LI_DETAIL*>(d);
            int& n = p.lists.back();
            p.bullet = li->is_task ? (li->task_mark == L' ' ? L"☐" : L"☑" + std::wstring(1, (wchar_t)0xFE0E))  // text, not emoji
                       : n >= 0    ? std::to_wstring(n++) + L"."
                                   : std::wstring(1, L"•◦▪"[(p.lists.size() - 1) % 3]);
            break;
        }
        case MD_BLOCK_HR: p.Start(B_Rule), p.open = false; break;
        case MD_BLOCK_H: p.Start(B_Head, static_cast<MD_BLOCK_H_DETAIL*>(d)->level); break;
        case MD_BLOCK_CODE: p.Start(B_Code); break;
        case MD_BLOCK_HTML:
        case MD_BLOCK_P: p.Start(B_Para); break;
        case MD_BLOCK_TABLE: p.Start(B_Table), p.table = true; break;
        case MD_BLOCK_TR: g_blocks.back().rows.emplace_back(); break;
        case MD_BLOCK_TH:
        case MD_BLOCK_TD: g_blocks.back().rows.back().emplace_back().align = static_cast<MD_BLOCK_TD_DETAIL*>(d)->align; break;
        default: break;
    }
    return 0;
}

int LeaveBlock(MD_BLOCKTYPE type, void*, void* user) {
    Parser& p = *static_cast<Parser*>(user);
    switch (type) {
        case MD_BLOCK_QUOTE: p.End(), p.quote--; break;
        case MD_BLOCK_UL:
        case MD_BLOCK_OL: p.End(), p.lists.pop_back(); break;
        case MD_BLOCK_LI: p.End(), p.bullet.clear(); break;
        case MD_BLOCK_TABLE: p.table = false, p.open = false; break;
        case MD_BLOCK_H:
        case MD_BLOCK_CODE:
        case MD_BLOCK_HTML:
        case MD_BLOCK_P: p.End(); break;
        default: break;
    }
    return 0;
}

int EnterSpan(MD_SPANTYPE type, void*, void* user) {
    Parser& p = *static_cast<Parser*>(user);
    RunKind k = type == MD_SPAN_STRONG                      ? RK_Strong
                : type == MD_SPAN_EM || type == MD_SPAN_IMG ? RK_Em  // images show their alt text
                : type == MD_SPAN_CODE                      ? RK_Code
                : type == MD_SPAN_A                         ? RK_Link
                : type == MD_SPAN_DEL                       ? RK_Del
                                                            : RK_None;
    p.spans.push_back({(UINT32)p.Target().s.size(), k});
    return 0;
}

int LeaveSpan(MD_SPANTYPE, void*, void* user) {
    Parser& p = *static_cast<Parser*>(user);
    auto [start, k] = p.spans.back();
    p.spans.pop_back();
    Text& t = p.Target();
    if (k != RK_None && t.s.size() > start) t.runs.push_back({start, (UINT32)t.s.size() - start, k});
    return 0;
}

int OnText(MD_TEXTTYPE type, const MD_CHAR* s, MD_SIZE n, void* user) {
    if (type == MD_TEXT_HTML) return 0;
    std::wstring& out = static_cast<Parser*>(user)->Target().s;
    switch (type) {
        case MD_TEXT_NULLCHAR: out += L'�'; break;
        case MD_TEXT_BR: out += L'\x2028'; break;  // line break within the paragraph
        case MD_TEXT_SOFTBR: out += L' '; break;
        case MD_TEXT_ENTITY: out += Entity({s, n}); break;
        default: out.append(s, n);
    }
    return 0;
}

// ---- layout
ComPtr<IDWriteTextFormat> MakeFormat(const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight, float lineH,
                                     DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
    ComPtr<IDWriteTextFormat> f;
    g_dw->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"",
                           &f);
    f->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, size * lineH, size * lineH * 0.78f);
    f->SetTextAlignment(align);
    return f;
}

void CodeRects(Text& t) {
    t.codeRects.clear();
    for (auto& r : t.runs) {
        if (r.kind != RK_Code) continue;
        UINT32 n = 0;
        t.layout->HitTestTextRange(r.start, r.len, 0, 0, nullptr, 0, &n);
        std::vector<DWRITE_HIT_TEST_METRICS> m(n);
        if (n && SUCCEEDED(t.layout->HitTestTextRange(r.start, r.len, 0, 0, m.data(), n, &n)))
            for (auto& h : m) t.codeRects.push_back({h.left - 3, h.top + 2, h.left + h.width + 3, h.top + h.height - 1});
    }
}

float LayoutText(Text& t, IDWriteTextFormat* f, float width, bool dim = false) {
    g_dw->CreateTextLayout(t.s.c_str(), (UINT32)t.s.size(), f, std::max(width, 1.f), 1e6f, &t.layout);
    if (t.align == MD_ALIGN_CENTER) t.layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    if (t.align == MD_ALIGN_RIGHT) t.layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    if (dim) t.layout->SetDrawingEffect(g_dim.Get(), {0, (UINT32)t.s.size()});
    for (auto& r : t.runs) {
        DWRITE_TEXT_RANGE range{r.start, r.len};
        switch (r.kind) {
            case RK_Strong: t.layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, range); break;
            case RK_Em: t.layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, range); break;
            case RK_Code:
                t.layout->SetFontFamilyName(g_mono.c_str(), range);
                t.layout->SetFontSize(f->GetFontSize() * 0.88f, range);
                break;
            case RK_Link: t.layout->SetDrawingEffect(g_link.Get(), range); break;
            case RK_Del: t.layout->SetStrikethrough(TRUE, range); break;
            default: break;
        }
    }
    CodeRects(t);
    DWRITE_TEXT_METRICS m{};
    t.layout->GetMetrics(&m);
    return m.height;
}

void LayoutBlock(Block& b, float width) {
    float w = width - b.indent * kIndent - b.quote * kQuote, before = 0;
    bool dim = b.quote > 0;
    switch (b.kind) {
        case B_Para: b.h = LayoutText(b.text, g_fmtBody.Get(), w, dim), b.after = b.tight ? 4 : 14; break;
        case B_Head:
            b.h = LayoutText(b.text, g_fmtHead[std::clamp(b.level, 1, 6) - 1].Get(), w, dim || b.level == 6);
            if (b.level <= 2) b.h += 9;  // room for the rule underneath
            before = b.level <= 2 ? 24 : 18, b.after = 10;
            break;
        case B_Code: b.h = LayoutText(b.text, g_fmtCode.Get(), w - 2 * kCodePad) + 2 * kCodePad, b.after = 14; break;
        case B_Rule: b.h = 2, before = 10, b.after = 22; break;
        case B_Table: {
            size_t cols = 0;
            for (auto& r : b.rows) cols = std::max(cols, r.size());
            std::vector<float> natural(cols, 0);
            for (size_t ri = 0; ri < b.rows.size(); ri++)
                for (size_t ci = 0; ci < b.rows[ri].size(); ci++) {
                    Text& c = b.rows[ri][ci];
                    LayoutText(c, g_fmtBody.Get(), 1e5f, dim);
                    if (ri == 0) c.layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, {0, (UINT32)c.s.size()});
                    DWRITE_TEXT_METRICS m{};
                    c.layout->GetMetrics(&m);
                    natural[ci] = std::max(natural[ci], std::ceil(m.widthIncludingTrailingWhitespace) + 2 * kCellPadX);
                }
            float total = 0;
            for (float n : natural) total += n;
            float k = total > w ? w / total : 1;  // shrink every column alike when the table is too wide
            b.colX.assign(1, 0);
            for (float n : natural) b.colX.push_back(b.colX.back() + std::floor(std::max(48.f, n * k)));
            b.rowY.assign(1, 0);
            for (auto& r : b.rows) {
                float rh = 0;
                for (size_t ci = 0; ci < r.size(); ci++) {
                    r[ci].layout->SetMaxWidth(b.colX[ci + 1] - b.colX[ci] - 2 * kCellPadX);
                    CodeRects(r[ci]);
                    DWRITE_TEXT_METRICS m{};
                    r[ci].layout->GetMetrics(&m);
                    rh = std::max(rh, m.height);
                }
                b.rowY.push_back(b.rowY.back() + std::ceil(rh) + 2 * kCellPadY);
            }
            b.h = b.rowY.back(), b.after = 14;
            break;
        }
    }
    b.y = g_laidH + std::max(g_prevAfter, before);
    g_laidH = b.y + b.h, g_prevAfter = b.after;
}

void DrawRich(ID2D1RenderTarget* rt, const Text& t, float x, float y, ID2D1SolidColorBrush* text,
              ID2D1SolidColorBrush* fill, const MdColors& c) {
    fill->SetColor(c.codeBg);
    for (auto& r : t.codeRects) rt->FillRoundedRectangle({{x + r.left, y + r.top, x + r.right, y + r.bottom}, 4, 4}, fill);
    rt->DrawTextLayout({x, y}, t.layout.Get(), text);
}

void DrawBlock(ID2D1RenderTarget* rt, const Block& b, float x0, float y, float width, ID2D1SolidColorBrush* text,
               ID2D1SolidColorBrush* fill, const MdColors& c) {
    float x = x0 + b.indent * kIndent + b.quote * kQuote, right = x0 + width;
    fill->SetColor(c.rule);
    for (int q = 0; q < b.quote; q++) {
        float qx = x - (b.quote - q) * kQuote;
        rt->FillRectangle({qx, y, qx + 3, y + b.h + b.after}, fill);
    }
    if (!b.bullet.empty())
        rt->DrawText(b.bullet.c_str(), (UINT32)b.bullet.size(), g_fmtBullet.Get(), {x - 60, y, x - 8, y + 100}, text);
    switch (b.kind) {
        case B_Para: DrawRich(rt, b.text, x, y, text, fill, c); break;
        case B_Head:
            DrawRich(rt, b.text, x, y, text, fill, c);
            fill->SetColor(c.rule);
            if (b.level <= 2) rt->FillRectangle({x, y + b.h - 1, right, y + b.h}, fill);
            break;
        case B_Code:
            fill->SetColor(c.codeBg);
            rt->FillRoundedRectangle({{x, y, right, y + b.h}, 6, 6}, fill);
            rt->DrawTextLayout({x + kCodePad, y + kCodePad}, b.text.layout.Get(), text);
            break;
        case B_Rule: rt->FillRectangle({x, y, right, y + b.h}, fill); break;
        case B_Table: {
            fill->SetColor(c.codeBg);
            rt->FillRectangle({x, y, x + b.colX.back(), y + b.rowY[1]}, fill);
            fill->SetColor(c.rule);
            for (float cx : b.colX) rt->FillRectangle({x + cx, y, x + cx + 1, y + b.h + 1}, fill);
            for (float ry : b.rowY) rt->FillRectangle({x, y + ry, x + b.colX.back() + 1, y + ry + 1}, fill);
            for (size_t ri = 0; ri < b.rows.size(); ri++)
                for (size_t ci = 0; ci < b.rows[ri].size(); ci++)
                    DrawRich(rt, b.rows[ri][ci], x + b.colX[ci] + kCellPadX, y + b.rowY[ri] + kCellPadY, text, fill, c);
            break;
        }
    }
}

}  // namespace

void MdInit(IDWriteFactory* dw, const wchar_t* uiFont, const wchar_t* monoFont) {
    g_dw = dw;
    g_mono = monoFont;
    g_fmtBody = MakeFormat(uiFont, 15, DWRITE_FONT_WEIGHT_NORMAL, 1.55f);
    g_fmtBullet = MakeFormat(uiFont, 15, DWRITE_FONT_WEIGHT_NORMAL, 1.55f, DWRITE_TEXT_ALIGNMENT_TRAILING);
    g_fmtCode = MakeFormat(monoFont, 13, DWRITE_FONT_WEIGHT_NORMAL, 1.5f);
    const float sizes[6] = {28, 22, 18, 16, 15, 14};
    for (int i = 0; i < 6; i++) g_fmtHead[i] = MakeFormat(uiFont, sizes[i], DWRITE_FONT_WEIGHT_SEMI_BOLD, 1.3f);
}

void MdClear() {
    std::vector<Block>().swap(g_blocks);
    g_laid = 0, g_width = -1;
}

void MdLoad(const std::wstring& text) {
    MdClear();
    Parser p;
    MD_PARSER parser{0, MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS | MD_FLAG_PERMISSIVEAUTOLINKS,
                     EnterBlock, LeaveBlock, EnterSpan, LeaveSpan, OnText, nullptr, nullptr};
    md_parse(text.data(), (MD_SIZE)text.size(), &parser, &p);
}

float MdPaint(ID2D1RenderTarget* rt, const D2D1_RECT_F& area, float scroll, const MdColors& c) {
    if (rt != g_brushRt) {  // layouts hold brushes of one render target
        rt->CreateSolidColorBrush(c.link, g_link.ReleaseAndGetAddressOf());
        rt->CreateSolidColorBrush(c.dim, g_dim.ReleaseAndGetAddressOf());
        g_brushRt = rt, g_width = -1;
    }
    g_link->SetColor(c.link);
    g_dim->SetColor(c.dim);
    float width = std::min(kMaxWidth, area.right - area.left), viewH = area.bottom - area.top;
    if (width != g_width) g_width = width, g_laid = 0, g_laidH = 0, g_prevAfter = 8;
    while (g_laid < g_blocks.size() && g_laidH < scroll + viewH + 400) LayoutBlock(g_blocks[g_laid++], width);

    ComPtr<ID2D1SolidColorBrush> text, fill;
    rt->CreateSolidColorBrush(c.text, &text);
    rt->CreateSolidColorBrush(c.rule, &fill);
    float x0 = std::round(area.left + (area.right - area.left - width) / 2);
    for (size_t i = 0; i < g_laid; i++) {
        const Block& b = g_blocks[i];
        float y = area.top + b.y - scroll;
        if (y > area.bottom) break;
        if (y + b.h + b.after >= area.top) DrawBlock(rt, b, x0, y, width, text.Get(), fill.Get(), c);
    }
    float h = g_laidH + 24;  // bottom padding
    return g_laid == g_blocks.size() ? h : h * g_blocks.size() / std::max<size_t>(g_laid, 1);  // estimate
}

std::wstring MdOutline() {
    std::wstring s;
    for (auto& b : g_blocks) {
        s += L"PHCRT"[b.kind];
        if (b.kind == B_Head) s += (wchar_t)(L'0' + b.level);
        s += b.bullet + L":" + b.text.s + (b.quote ? L">" : L"") + L"|";
    }
    return s;
}
