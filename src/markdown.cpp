// Markdown preview: md4c parses the text into blocks (paragraphs, headings, code, lists, quotes,
// tables); DirectWrite lays them out lazily, only as far as the view has scrolled, so opening a long
// document costs about the same as a short one. Raw HTML is not rendered (text between tags shows).
// Code blocks are coloured with Lexilla; local and embedded (data:) images are decoded when laid
// out. Remote images show their alt text: a previewer doesn't go to the network.
#include "app.h"
#include <shlwapi.h>
#include <wincrypt.h>
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
enum BlockKind { B_Para, B_Head, B_Code, B_Rule, B_Table, B_Image };
struct Table {
    std::vector<std::vector<Text>> rows;  // rows[0] is the header
    std::vector<float> colX, rowY;        // cell edges relative to the block origin; rowY: rows laid out so far
    size_t live0 = 0, live1 = 0;          // rows that may hold cell layouts (the rest were released or never made)
};
struct Block {  // kept small: a long document has hundreds of thousands
    BlockKind kind;
    int level = 0, indent = 0, quote = 0;  // heading level, list depth, quote depth
    bool tight = false;                    // text directly in a tight list item: small gap after
    UINT8 part = 0;                        // code split in chunks: 1 first, 2 middle, 3 last (0: whole)
    std::wstring bullet;
    Text text;
    std::unique_ptr<Table> tab;           // tables
    std::wstring src;                     // images: path or data: URI; code: language
    ComPtr<ID2D1Bitmap> bmp;              // images, decoded on first layout
    bool sized = false, tried = false;
    float imgW = 0, imgH = 0;             // images: natural size (pixels = DIPs, like a browser)
    float y = 0, h = 0, after = 0;        // laid-out position in the document (DIPs)
};

constexpr float kIndent = 26, kQuote = 18, kCodePad = 12, kCellPadX = 10, kCellPadY = 6, kMaxWidth = 860;
// Tables: column widths from the first kSampleRows rows, layouts kept kKeepRows around the view; code: lines per chunk.
constexpr size_t kSampleRows = 50, kKeepRows = 200, kCodeLines = 200;

IDWriteFactory* g_dw;
std::wstring g_mono;
ComPtr<IDWriteTextFormat> g_fmtBody, g_fmtCode, g_fmtBullet, g_fmtHead[6];
std::vector<Block> g_blocks;
std::vector<size_t> g_heads;  // blocks that are H1-H3: the contents sidebar
std::wstring g_base;  // folder of the Markdown file, for relative image paths
float g_dpi = 96;
size_t g_laid;  // blocks [0, g_laid) are laid out for g_width
float g_width = -1, g_laidH, g_prevAfter;
ID2D1RenderTarget* g_brushRt;
ComPtr<ID2D1SolidColorBrush> g_link, g_dim, g_role[R_Count];  // drawing effects stored in layouts
// Selection: a paragraph is (block << 32 | table cell, row-major); text comes from the parsed blocks, so copying
// never needs layout, and positions stay valid when blocks are laid out again (resize, theme, pictures arriving).
TextSel g_sel;
bool g_selOn;               // during a paint: something is selected
D2D1_COLOR_F g_selColor;
float g_x0, g_top;          // last paint: the column's left edge, the area's top

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
    int image = 0;                     // inside an image block's alt text (nesting depth)
    bool afterImage = false;           // paragraph text continuing after a picture: drop leading spaces

    void End() {
        if (!open || table) return;
        open = false;
        Block& b = g_blocks.back();
        while (b.kind == B_Code && !b.text.s.empty() && b.text.s.back() == L'\n') b.text.s.pop_back();
        if (b.kind == B_Para && b.text.s.empty() && b.bullet.empty()) g_blocks.pop_back();  // e.g. an HTML block
        else if (b.kind == B_Code) Split();
    }
    // A long code block becomes chunks of kCodeLines lines, each laid out (and coloured) like a block of its own:
    // one layout with tens of thousands of coloured runs took seconds (setting a run's colour isn't constant-time).
    // Copying joins the chunks with CRLF, the line break they were split at.
    // shortcut: each chunk is highlighted on its own (a string or comment open across the split loses its colour).
    void Split() {
        Block whole = std::move(g_blocks.back());
        std::wstring_view s = whole.text.s;
        size_t lines = std::count(s.begin(), s.end(), L'\n');
        if (lines < 2 * kCodeLines) return void(g_blocks.back() = std::move(whole));
        g_blocks.pop_back();
        for (size_t at = 0, n = 0; at <= s.size(); n++) {
            size_t end = at;
            for (size_t k = 0; k < kCodeLines && end != s.npos; k++) end = s.find(L'\n', k ? end + 1 : at);  // its last line's end
            Block& c = g_blocks.emplace_back(Block{B_Code, whole.level, whole.indent, whole.quote});
            c.src = whole.src, c.part = n ? 2 : 1;
            if (!n) c.bullet = whole.bullet;
            c.text.s = s.substr(at, end == s.npos ? s.npos : end - at);
            if (end == s.npos) {
                c.part = 3;
                break;
            }
            at = end + 1;
        }
    }
    void Start(BlockKind k, int level = 0) {
        End();
        afterImage = false;
        Block& b = g_blocks.emplace_back(Block{k, level, (int)lists.size(), quote});
        b.bullet = std::move(bullet);
        bullet.clear();
        open = true;
    }
    Text& Target() {  // tight list items have no paragraph block: start one on first text
        if (!open) Start(B_Para), g_blocks.back().tight = !lists.empty();
        Block& b = g_blocks.back();
        return table ? b.tab->rows.back().back() : b.text;
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
        case MD_BLOCK_CODE: {
            p.Start(B_Code);
            auto& lang = static_cast<MD_BLOCK_CODE_DETAIL*>(d)->lang;
            g_blocks.back().src.assign(lang.text ? lang.text : L"", lang.size);
            break;
        }
        case MD_BLOCK_HTML:
        case MD_BLOCK_P: p.Start(B_Para); break;
        case MD_BLOCK_TABLE: p.Start(B_Table), g_blocks.back().tab = std::make_unique<Table>(), p.table = true; break;
        case MD_BLOCK_TR: g_blocks.back().tab->rows.emplace_back(); break;
        case MD_BLOCK_TH:
        case MD_BLOCK_TD: g_blocks.back().tab->rows.back().emplace_back().align = static_cast<MD_BLOCK_TD_DETAIL*>(d)->align; break;
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

float CodeTop(const Block& b) { return b.part <= 1 ? kCodePad : 0; }  // the text's offset in a code block (chunk)

bool Remote(const std::wstring& src) { return src.find(L"://") != std::wstring::npos || src.starts_with(L"//"); }

int EnterSpan(MD_SPANTYPE type, void* d, void* user) {
    Parser& p = *static_cast<Parser*>(user);
    if (type == MD_SPAN_IMG && p.image) p.image++;
    if (type == MD_SPAN_IMG && !p.image && !p.table) {  // a picture we can show: its own block
        auto& a = static_cast<MD_SPAN_IMG_DETAIL*>(d)->src;
        std::wstring src(a.text ? a.text : L"", a.size);
        if (!src.empty() && !Remote(src)) {
            p.Start(B_Image);
            g_blocks.back().src = std::move(src);
            p.image = 1;
            p.spans.push_back({0, RK_None});
            return 0;
        }
    }
    RunKind k = type == MD_SPAN_STRONG                      ? RK_Strong
                : type == MD_SPAN_EM || type == MD_SPAN_IMG ? RK_Em  // images show their alt text
                : type == MD_SPAN_CODE                      ? RK_Code
                : type == MD_SPAN_A                         ? RK_Link
                : type == MD_SPAN_DEL                       ? RK_Del
                                                            : RK_None;
    p.spans.push_back({(UINT32)p.Target().s.size(), k});
    return 0;
}

int LeaveSpan(MD_SPANTYPE type, void*, void* user) {
    Parser& p = *static_cast<Parser*>(user);
    auto [start, k] = p.spans.back();
    p.spans.pop_back();
    if (type == MD_SPAN_IMG && p.image && --p.image == 0) {  // text after the picture: a new paragraph
        p.open = false, p.afterImage = true;
        return 0;
    }
    Text& t = p.Target();
    if (k != RK_None && t.s.size() > start) t.runs.push_back({start, (UINT32)t.s.size() - start, k});
    return 0;
}

int OnText(MD_TEXTTYPE type, const MD_CHAR* s, MD_SIZE n, void* user) {
    if (type == MD_TEXT_HTML) return 0;
    Parser& p = *static_cast<Parser*>(user);
    if (p.afterImage && p.open) p.afterImage = false;  // the paragraph after the picture has begun
    if (p.afterImage) {
        while (n && (*s == L' ' || type == MD_TEXT_SOFTBR)) s++, n--, type = type == MD_TEXT_SOFTBR ? MD_TEXT_NORMAL : type;
        if (!n) return 0;
        p.afterImage = false;
    }
    std::wstring& out = p.Target().s;
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

// Reads a Markdown image's natural size (px == 0: cheap, headers only) or decodes it to at most
// `px` pixels wide.
void LoadImageBlock(Block& b, UINT px) {
    IWICImagingFactory* wic = WicFactory();
    ComPtr<IWICBitmapSource> src;
    std::string bytes;  // data: URI payload, alive while decoding
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (b.src.starts_with(L"data:")) {
        size_t comma = b.src.find(L',');
        if (comma == std::wstring::npos || b.src.find(L";base64") > comma) return;
        std::string b64(b.src.begin() + comma + 1, b.src.end());
        DWORD n = 0;
        if (!CryptStringToBinaryA(b64.c_str(), (DWORD)b64.size(), CRYPT_STRING_BASE64, nullptr, &n, nullptr, nullptr)) return;
        bytes.resize(n);
        if (!CryptStringToBinaryA(b64.c_str(), (DWORD)b64.size(), CRYPT_STRING_BASE64, (BYTE*)bytes.data(), &n, nullptr, nullptr) ||
            FAILED(wic->CreateStream(&stream)) || FAILED(stream->InitializeFromMemory((BYTE*)bytes.data(), n)) ||
            FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec)))
            return;
    } else {
        std::wstring rel = b.src.substr(0, b.src.find_first_of(L"?#"));
        wchar_t unescaped[MAX_PATH * 2];
        DWORD n = MAX_PATH * 2;
        if (SUCCEEDED(UrlUnescapeW(rel.data(), unescaped, &n, 0))) rel = unescaped;
        std::replace(rel.begin(), rel.end(), L'/', L'\\');
        if (rel.starts_with(L"file:\\\\\\")) rel.erase(0, 8);
        wchar_t full[MAX_PATH * 2];
        std::wstring joined = PathIsRelativeW(rel.c_str()) ? g_base + L"\\" + rel : rel;
        if (!PathCanonicalizeW(full, joined.c_str())) return;
        std::wstring ext = PathFindExtensionW(full);
        if (!_wcsicmp(ext.c_str(), L".svg") || !_wcsicmp(ext.c_str(), L".svgz")) {
            UINT w = 0, h = 0;
            auto svg = SvgRender(full, (float)px, 1e5f, wic, w, h);
            if (svg) b.bmp = SharedBitmap(svg.Get());
            if (!px) b.imgW = (float)w, b.imgH = (float)h;
            return;
        }
        if (FAILED(wic->CreateDecoderFromFilename(full, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) return;
    }
    UINT w, h;
    if (FAILED(dec->GetFrame(0, &frame)) || FAILED(frame->GetSize(&w, &h)) || !w || !h) return;
    if (!px) {
        b.imgW = (float)w, b.imgH = (float)h;
        return;
    }
    src = frame;
    ComPtr<IWICBitmapScaler> scaler;
    if (w > px && SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
        SUCCEEDED(scaler->Initialize(frame.Get(), px, std::max(1u, (UINT)((UINT64)h * px / w)), WICBitmapInterpolationModeFant)))
        src = scaler;
    b.bmp = SharedBitmap(src.Get());
}

// A table cell's layout at its column's width (made again after it was released).
Text& Cell(Block& b, size_t ri, size_t ci) {
    Text& c = b.tab->rows[ri][ci];
    float w = b.tab->colX[ci + 1] - b.tab->colX[ci] - 2 * kCellPadX;
    if (!c.layout) {
        LayoutText(c, g_fmtBody.Get(), w, b.quote > 0);
        if (ri == 0) c.layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, {0, (UINT32)c.s.size()});
    } else if (c.layout->GetMaxWidth() != w) c.layout->SetMaxWidth(w), CodeRects(c);
    return c;
}

bool Open(const Block& b) { return b.kind == B_Table && b.tab->rowY.size() <= b.tab->rows.size(); }  // rows still to lay out

// Lays out rows until the table is `until` tall. Only rows near `until` keep their layouts (a jump far down measures
// every row before it; without a limit, a jump past the table, none).
void TableRows(Block& b, float until) {
    Table& t = *b.tab;
    for (size_t first = t.rowY.size() - 1; Open(b) && t.rowY.back() < until;) {
        size_t ri = t.rowY.size() - 1;
        float rh = 0;
        for (size_t ci = 0; ci < t.rows[ri].size(); ci++) {
            DWRITE_TEXT_METRICS m{};
            Cell(b, ri, ci).layout->GetMetrics(&m);
            rh = std::max(rh, m.height);
            if (until == INFINITY && ri >= t.live1) t.rows[ri][ci].layout.Reset();
        }
        if (until != INFINITY) t.live1 = std::max(t.live1, ri + 1);
        if (size_t old = ri - kKeepRows; until != INFINITY && ri >= first + kKeepRows && old >= t.live0)
            for (auto& c : t.rows[old]) c.layout.Reset();  // measured: its height is all that's needed
        t.rowY.push_back(t.rowY.back() + std::ceil(rh) + 2 * kCellPadY);
    }
    b.h = t.rowY.back();
}

void LayoutBlock(Block& b, float width, float until = INFINITY) {
    float w = width - b.indent * kIndent - b.quote * kQuote, before = 0;
    bool dim = b.quote > 0;
    switch (b.kind) {
        case B_Para: b.h = LayoutText(b.text, g_fmtBody.Get(), w, dim), b.after = b.tight ? 4 : 14; break;
        case B_Head:
            b.h = LayoutText(b.text, g_fmtHead[std::clamp(b.level, 1, 6) - 1].Get(), w, dim || b.level == 6);
            if (b.level <= 2) b.h += 9;  // room for the rule underneath
            before = b.level <= 2 ? 24 : 18, b.after = 10;
            break;
        case B_Code:
            b.h = LayoutText(b.text, g_fmtCode.Get(), w - 2 * kCodePad) + CodeTop(b) + (b.part % 3 == 0 ? kCodePad : 0);
            b.after = b.part % 3 == 0 ? 14 : 0;  // chunks of one block: no gap between them
            for (auto& r : Highlight(b.text.s, LangForName(b.src)))
                b.text.layout->SetDrawingEffect(g_role[r.role].Get(), {r.start, r.len});
            break;
        case B_Image:
            if (!b.sized) b.sized = true, LoadImageBlock(b, 0);  // pixels are decoded when first drawn
            if (b.imgW > 0) {
                float k = std::min(1.f, w / b.imgW);
                b.h = std::round(b.imgH * k);
            } else {  // missing or undecodable: show the alt text
                if (b.text.s.empty()) b.text.s = PathFindFileNameW(b.src.c_str());
                b.text.runs = {{0, (UINT32)b.text.s.size(), RK_Em}};
                b.h = LayoutText(b.text, g_fmtBody.Get(), w, true);
            }
            b.after = 14;
            break;
        case B_Rule: b.h = 2, before = 10, b.after = 22; break;
        case B_Table: {
            size_t cols = 0;
            for (auto& r : b.tab->rows) cols = std::max(cols, r.size());
            // Widths from the first rows and, per column, the longest text of any row: they don't change as rows are
            // laid out (a layout per column, not per row).
            std::vector<float> natural(cols, 0);
            std::vector<const Text*> longest(cols, nullptr);
            for (auto& r : b.tab->rows)
                for (size_t ci = 0; ci < r.size(); ci++)
                    if (!longest[ci] || r[ci].s.size() > longest[ci]->s.size()) longest[ci] = &r[ci];
            auto measure = [&](Text& c, size_t ci, bool head, float slack) {
                LayoutText(c, g_fmtBody.Get(), 1e5f, dim);
                if (head) c.layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, {0, (UINT32)c.s.size()});
                DWRITE_TEXT_METRICS m{};
                c.layout->GetMetrics(&m);
                natural[ci] = std::max(natural[ci], std::ceil(m.widthIncludingTrailingWhitespace * slack) + 2 * kCellPadX);
            };
            size_t sample = std::min(b.tab->rows.size(), kSampleRows);
            for (size_t ri = 0; ri < sample; ri++)
                for (size_t ci = 0; ci < b.tab->rows[ri].size(); ci++) measure(b.tab->rows[ri][ci], ci, ri == 0, 1);
            for (size_t ci = 0; ci < cols; ci++) {  // a copy: that row keeps no layout
                Text t{longest[ci]->s, longest[ci]->runs};
                measure(t, ci, false, 1.15f);  // as long, but wider letters ("r4990" vs "r1000"): some room
            }
            float total = 0;
            for (float n : natural) total += n;
            float k = total > w ? w / total : 1;  // shrink every column alike when the table is too wide
            b.tab->colX.assign(1, 0);
            for (float n : natural) b.tab->colX.push_back(b.tab->colX.back() + std::floor(std::max(48.f, n * k)));
            b.tab->rowY.assign(1, 0), b.tab->live0 = 0, b.tab->live1 = std::min(b.tab->rows.size(), kSampleRows);
            b.h = 0, b.after = 14;  // rows: TableRows, below
            break;
        }
    }
    b.y = g_laidH + std::max(g_prevAfter, before);
    if (b.kind == B_Table) TableRows(b, until - b.y);
    g_laidH = b.y + b.h, g_prevAfter = b.after;
}

// Lays out blocks (a table part way first) until the document is `until` tall.
void LayOut(float width, float until) {
    for (;;) {
        if (Block* last = g_laid ? &g_blocks[g_laid - 1] : nullptr; last && Open(*last)) {
            if (g_laidH >= until) break;
            TableRows(*last, until - last->y);
            g_laidH = last->y + last->h;
        } else if (g_laid < g_blocks.size() && g_laidH < until) LayoutBlock(g_blocks[g_laid++], width, until);
        else break;
    }
}

// The selected part of text `unit` behind it (fill's colour changes).
void DrawSel(ID2D1RenderTarget* rt, const Text& t, UINT64 unit, float x, float y, ID2D1SolidColorBrush* fill) {
    if (UINT32 a, b; g_selOn && unit != ~0ull && t.layout && g_sel.Span(unit, (UINT32)t.s.size(), a, b) && b > a)
        fill->SetColor(g_selColor), FillText(rt, t.layout.Get(), a, b, {x, y}, fill);
}

void DrawRich(ID2D1RenderTarget* rt, const Text& t, UINT64 unit, float x, float y, ID2D1SolidColorBrush* text,
              ID2D1SolidColorBrush* fill, const MdColors& c) {
    fill->SetColor(c.codeBg);
    for (auto& r : t.codeRects) rt->FillRoundedRectangle({{x + r.left, y + r.top, x + r.right, y + r.bottom}, 4, 4}, fill);
    DrawSel(rt, t, unit, x, y, fill);
    rt->DrawTextLayout({x, y}, t.layout.Get(), text);
}

void DrawBlock(ID2D1RenderTarget* rt, Block& b, UINT64 unit, float x0, float y, float width, float top, float bottom,
               ID2D1SolidColorBrush* text, ID2D1SolidColorBrush* fill, const MdColors& c) {
    float x = x0 + b.indent * kIndent + b.quote * kQuote, right = x0 + width;
    fill->SetColor(c.rule);
    for (int q = 0; q < b.quote; q++) {
        float qx = x - (b.quote - q) * kQuote;
        rt->FillRectangle({qx, y, qx + 3, y + b.h + b.after}, fill);
    }
    if (!b.bullet.empty())
        rt->DrawText(b.bullet.c_str(), (UINT32)b.bullet.size(), g_fmtBullet.Get(), {x - 60, y, x - 8, y + 100}, text);
    switch (b.kind) {
        case B_Para: DrawRich(rt, b.text, unit, x, y, text, fill, c); break;
        case B_Head:
            DrawRich(rt, b.text, unit, x, y, text, fill, c);
            fill->SetColor(c.rule);
            if (b.level <= 2) rt->FillRectangle({x, y + b.h - 1, right, y + b.h}, fill);
            break;
        case B_Code:
            fill->SetColor(c.codeBg);
            if (b.part) {  // a chunk: round corners at the block's ends only (the background is translucent: no overlap)
                rt->PushAxisAlignedClip({x, y, right, y + b.h}, D2D1_ANTIALIAS_MODE_ALIASED);
                rt->FillRoundedRectangle({{x, b.part == 1 ? y : y - 8, right, b.part == 3 ? y + b.h : y + b.h + 8}, 6, 6}, fill);
                rt->PopAxisAlignedClip();
            } else rt->FillRoundedRectangle({{x, y, right, y + b.h}, 6, 6}, fill);
            DrawSel(rt, b.text, unit, x + kCodePad, y + CodeTop(b), fill);
            rt->DrawTextLayout({x + kCodePad, y + CodeTop(b)}, b.text.layout.Get(), text);
            break;
        case B_Rule: rt->FillRectangle({x, y, right, y + b.h}, fill); break;
        case B_Image:
            if (b.imgW > 0 && !b.tried) b.tried = true, LoadImageBlock(b, (UINT)std::lround((right - x) * g_dpi / 96));
            if (b.bmp) {
                float k = std::min(1.f, (right - x) / b.imgW);
                rt->DrawBitmap(b.bmp.Get(), {x, y, x + std::round(b.imgW * k), y + b.h}, 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            } else if (b.imgW <= 0) {
                DrawRich(rt, b.text, ~0ull, x, y, text, fill, c);  // alt text: not selectable (copied as nothing)
            }
            break;
        case B_Table: {
            if (b.tab->rowY.size() < 2) break;
            fill->SetColor(c.codeBg);
            rt->FillRectangle({x, y, x + b.tab->colX.back(), y + b.tab->rowY[1]}, fill);
            fill->SetColor(c.rule);
            for (float cx : b.tab->colX) rt->FillRectangle({x + cx, y, x + cx + 1, y + b.h + 1}, fill);
            // Only the rows in view (a long table: thousands of rows off screen); layouts far from them are released.
            auto row = [&](float v) { return (size_t)std::max<ptrdiff_t>(0, std::upper_bound(b.tab->rowY.begin(), b.tab->rowY.end(), v) - b.tab->rowY.begin() - 1); };
            size_t laid = b.tab->rowY.size() - 1, r0 = std::min(laid, row(top - y)), r1 = std::min(laid, row(bottom - y) + 1), k = 0;
            size_t keep0 = r0 > kKeepRows ? r0 - kKeepRows : 0, keep1 = std::min(laid, r1 + kKeepRows);
            for (size_t ri = b.tab->live0; ri < std::min(b.tab->live1, laid); ri++)
                if (ri < keep0 || ri >= keep1)
                    for (auto& cell : b.tab->rows[ri]) cell.layout.Reset();
            b.tab->live0 = keep0, b.tab->live1 = b.tab->live1 > laid ? std::max(keep1, b.tab->live1) : keep1;  // unlaid sample rows: later
            for (size_t ri = r0; ri <= r1 && ri < b.tab->rowY.size(); ri++)
                rt->FillRectangle({x, y + b.tab->rowY[ri], x + b.tab->colX.back() + 1, y + b.tab->rowY[ri] + 1}, fill);
            for (size_t ri = 0; ri < r0; ri++) k += b.tab->rows[ri].size();
            for (size_t ri = r0; ri < r1; ri++)
                for (size_t ci = 0; ci < b.tab->rows[ri].size(); ci++, k++)
                    DrawRich(rt, Cell(b, ri, ci), unit + k, x + b.tab->colX[ci] + kCellPadX, y + b.tab->rowY[ri] + kCellPadY, text, fill, c);
            break;
        }
    }
}

// Plain text of [from, to) of a block's text: line breaks (code lines too) as CRLF.
std::wstring Plain(const std::wstring& s, UINT32 from, UINT32 to) {
    std::wstring v;
    for (UINT32 k = from; k < to; k++) v += s[k] == L'\n' || s[k] == L'\x2028' ? std::wstring(L"\r\n") : std::wstring(1, s[k]);
    return v;
}

// A list item's marker as it pastes well: "- ", "1. ", "- [ ] ", indented two spaces a level.
std::wstring Marker(const Block& b) {
    if (b.bullet.empty()) return {};
    std::wstring m = iswdigit(b.bullet[0]) ? b.bullet + L" " : b.bullet[0] == L'☐' ? L"- [ ] " : b.bullet[0] == L'☑' ? L"- [x] " : L"- ";
    return std::wstring(2 * std::max(0, b.indent - 1), L' ') + m;
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
    g_sel = {};
    g_heads.clear();
    g_laid = 0, g_width = -1;
}

void MdLoad(const std::wstring& text, const std::wstring& baseDir) {
    MdClear();
    g_base = baseDir;
    Parser p;
    MD_PARSER parser{0, MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS | MD_FLAG_PERMISSIVEAUTOLINKS,
                     EnterBlock, LeaveBlock, EnterSpan, LeaveSpan, OnText, nullptr, nullptr};
    md_parse(text.data(), (MD_SIZE)text.size(), &parser, &p);
    for (size_t i = 0; i < g_blocks.size(); i++)
        if (g_blocks[i].kind == B_Head && g_blocks[i].level <= 3 && !g_blocks[i].text.s.empty()) g_heads.push_back(i);
}

size_t MdHeadingCount() { return g_heads.size(); }
const std::wstring& MdHeadingText(size_t i) { return g_blocks[g_heads[i]].text.s; }
int MdHeadingLevel(size_t i) { return g_blocks[g_heads[i]].level; }

// Document position of heading i; lays out the blocks before it if `layout` (a jump far ahead).
float MdHeadingY(size_t i, bool layout) {
    size_t b = g_heads[i];
    if (b >= g_laid) {
        if (!layout || g_width < 0) return -1;
        if (g_laid && Open(g_blocks[g_laid - 1])) TableRows(g_blocks[g_laid - 1], INFINITY), g_laidH = g_blocks[g_laid - 1].y + g_blocks[g_laid - 1].h;
        while (g_laid <= b) LayoutBlock(g_blocks[g_laid++], g_width);
    }
    return g_blocks[b].y;
}

float MdPaint(ID2D1RenderTarget* rt, const D2D1_RECT_F& area, float scroll, const MdColors& c) {
    if (rt != g_brushRt) {  // layouts hold brushes of one render target
        rt->CreateSolidColorBrush(c.link, g_link.ReleaseAndGetAddressOf());
        rt->CreateSolidColorBrush(c.dim, g_dim.ReleaseAndGetAddressOf());
        for (auto& b : g_role) rt->CreateSolidColorBrush(c.text, b.ReleaseAndGetAddressOf());
        g_brushRt = rt, g_width = -1;
    }
    g_link->SetColor(c.link);
    g_dim->SetColor(c.dim);
    for (int i = 0; i < R_Count && c.roles; i++)
        g_role[i]->SetColor({GetRValue(c.roles[i]) / 255.f, GetGValue(c.roles[i]) / 255.f, GetBValue(c.roles[i]) / 255.f, 1});
    float dpiY;
    rt->GetDpi(&g_dpi, &dpiY);
    g_selOn = !g_sel.Empty(), g_selColor = c.sel;
    float width = std::min(kMaxWidth, area.right - area.left), viewH = area.bottom - area.top;
    if (width != g_width) g_width = width, g_laid = 0, g_laidH = 0, g_prevAfter = 8;
    LayOut(width, scroll + viewH + 400);

    ComPtr<ID2D1SolidColorBrush> text, fill;
    rt->CreateSolidColorBrush(c.text, &text);
    rt->CreateSolidColorBrush(c.rule, &fill);
    float x0 = std::round(area.left + (area.right - area.left - width) / 2);
    g_x0 = x0, g_top = area.top;
    for (size_t i = 0; i < g_laid; i++) {
        Block& b = g_blocks[i];
        float y = area.top + b.y - scroll;
        if (y > area.bottom) break;
        if (y + b.h + b.after >= area.top) DrawBlock(rt, b, (UINT64)i << 32, x0, y, width, area.top, area.bottom, text.Get(), fill.Get(), c);
    }
    // Estimates until all is laid out: blocks at their average height so far; an open table's rows at theirs (the blocks
    // after it at the average of those before it).
    float h = g_laidH + 24;  // bottom padding
    if (const Block* t = g_laid ? &g_blocks[g_laid - 1] : nullptr; t && Open(*t))
        return h + t->h / std::max<size_t>(1, t->tab->rowY.size() - 1) * (t->tab->rows.size() + 1 - t->tab->rowY.size()) +
               (g_laid > 1 ? t->y / (g_laid - 1) : 40) * (g_blocks.size() - g_laid);
    return g_laid == g_blocks.size() ? h : h * g_blocks.size() / std::max<size_t>(g_laid, 1);
}

std::wstring MdOutline() {
    std::wstring s;
    for (auto& b : g_blocks) {
        s += L"PHCRTI"[b.kind];
        if (b.kind == B_Head) s += (wchar_t)(L'0' + b.level);
        s += b.bullet + L":" + b.text.s + (b.quote ? L">" : L"") + L"|";
    }
    return s;
}

bool MdSelect(D2D1_POINT_2F pt, float scroll, int clicks) {
    auto texty = [](const Block& b) { return (b.kind == B_Para || b.kind == B_Head || b.kind == B_Code || b.kind == B_Table) && !(b.kind == B_Table && b.tab->rows.empty()); };
    if (!g_laid) return clicks ? (g_sel = {}, false) : false;
    size_t k = std::upper_bound(g_blocks.begin(), g_blocks.begin() + g_laid, pt.y - g_top + scroll,
                                [](float v, const Block& b) { return v < b.y; }) - g_blocks.begin();
    size_t i = SIZE_MAX;  // the block at the point, else the nearest one with text before it, else after it
    for (size_t j = std::min(k ? k - 1 : 0, g_laid - 1) + 1; j-- > 0 && i == SIZE_MAX;)
        if (texty(g_blocks[j])) i = j;
    for (size_t j = k; j < g_laid && i == SIZE_MAX; j++)
        if (texty(g_blocks[j])) i = j;
    if (i == SIZE_MAX) {
        if (clicks) g_sel = {};
        return false;
    }
    Block& b = g_blocks[i];
    float ox = g_x0 + b.indent * kIndent + b.quote * kQuote, oy = g_top - scroll + b.y;
    const Text* t = &b.text;
    UINT32 cell = 0;
    if (b.kind == B_Table) {  // the cell under the point (the nearest in its row)
        auto at = [](const std::vector<float>& e, float v) { return (size_t)std::max<ptrdiff_t>(0, std::upper_bound(e.begin(), e.end(), v) - e.begin() - 1); };
        if (b.tab->rowY.size() < 2) return false;
        size_t ri = std::min(at(b.tab->rowY, pt.y - oy), b.tab->rowY.size() - 2);  // laid out rows only
        while (ri > 0 && b.tab->rows[ri].empty()) ri--;
        if (b.tab->rows[ri].empty()) return false;
        size_t ci = std::min(at(b.tab->colX, pt.x - ox), b.tab->rows[ri].size() - 1);
        for (size_t r = 0; r < ri; r++) cell += (UINT32)b.tab->rows[r].size();
        cell += (UINT32)ci, t = &Cell(b, ri, ci);
        ox += b.tab->colX[ci] + kCellPadX, oy += b.tab->rowY[ri] + kCellPadY;
    } else if (b.kind == B_Code) ox += kCodePad, oy += CodeTop(b);
    if (!t->layout) return false;
    if (clicks) g_sel.unit = std::min(clicks, 3);
    UINT32 from, to;
    TextUnit(t->s, TextHit(t->layout.Get(), pt.x - ox, pt.y - oy), g_sel.unit, from, to);
    UINT64 unit = (UINT64)i << 32 | cell;
    if (clicks) g_sel.Click({unit, from}, {unit, to});
    else g_sel.Drag({unit, from}, {unit, to});
    return true;
}

void MdSelectAll() {
    g_sel = {};
    g_sel.all = !g_blocks.empty();
}

std::wstring MdCopy() { return MdCopy(g_sel); }

std::wstring MdCopy(const TextSel& sel) {
    CopyText o;
    if (sel.Empty()) return {};
    UINT64 first = std::min(sel.a, sel.b).first;
    for (size_t i = 0; i < g_blocks.size(); i++) {
        const Block& b = g_blocks[i];
        UINT64 unit = (UINT64)i << 32;
        UINT32 from, to;
        if (b.kind == B_Table) {
            for (size_t ri = 0, k = 0; ri < b.tab->rows.size(); ri++) {
                for (size_t ci = 0; ci < b.tab->rows[ri].size(); ci++, k++) {
                    if (ci) o.Sep(L"\t");
                    if (sel.Span(unit + k, (UINT32)b.tab->rows[ri][ci].s.size(), from, to)) o.Put(Plain(b.tab->rows[ri][ci].s, from, to));
                }
                o.Sep(L"\r\n");
            }
        } else if ((b.kind == B_Para || b.kind == B_Head || b.kind == B_Code) && sel.Span(unit, (UINT32)b.text.s.size(), from, to)) {
            // The list marker only with the item's text from its start: selected whole, or the selection began earlier.
            bool marker = !from && to && (to == b.text.s.size() || first < unit || sel.all);
            if (from == to && unit == std::max(sel.a, sel.b).first && !sel.all) continue;  // ends at the start of this block
            o.Put((marker ? Marker(b) : L"") + Plain(b.text.s, from, to));
            o.Sep(L"\r\n");
        }
    }
    return o.s;
}
