// Word documents (docx) as one continuous column at the window's width, like a reading view: no pages.
// Paragraphs keep their fonts, sizes (1 pt = 1/72 in), colours, spacing, indents, shading, borders and
// numbering; tables their grid widths, borders, shading and merges; pictures their size (decoded on a worker
// once they come into view); math (OMML) is typeset in Cambria Math by a script box model. document.xml is
// parsed and laid out block by block, only as far as the view has scrolled (the Markdown view's pattern), so
// opening costs about one screen; the scrollbar follows an estimate until the end has been laid out.
// Colours: automatic (and near-black) text takes the theme's text colour; inside a shaded paragraph or cell
// it is black or white by the fill, as Word does. Explicit colours and pictures are kept.
// shortcut: no headers/footers, footnote bodies, comments, columns, explicit tab stops, table style
// conditional formats, floating positions (anchored pictures go after their paragraph's text) or EMF/WMF.
#include "app.h"
#include "xml.h"
#include <dwrite_3.h>
#include <cmath>
#include <deque>
#include <map>

using Microsoft::WRL::ComPtr;
using D2D1::Matrix3x2F;

namespace {

constexpr float kPt = 96.f / 72;             // DIPs per point (Word at 100%)
constexpr float kMargin = 12;                // around the column, DIPs
constexpr UINT32 kAuto = 1, kLink = 2;       // brush keys: the palette's text and link colours (real ones are 0xFFRRGGBB)
constexpr wchar_t kObj = 0xFFFC;             // stands for an inline object in the text

float Twips(std::string_view v, float def = 0) { return Num(v, def * 15) / 15; }  // twentieths of a point -> DIPs
UINT32 Hex(std::string_view v) {
    return v.size() == 6 && v != "auto" ? 0xFF000000 | (UINT32)strtoul(std::string(v).c_str(), nullptr, 16) : 0;
}
float Lum(UINT32 c) { return (0.299f * (c >> 16 & 255) + 0.587f * (c >> 8 & 255) + 0.114f * (c & 255)) / 255; }
bool On(const Node* n) {
    if (!n) return false;
    std::string_view v = n->A("val");
    return v.empty() || v == "1" || v == "true" || v == "on";
}
const Node* Find(const Node& n, std::string_view name) {  // first descendant called `name`
    for (auto& k : n.kids)
        if (const Node* f = k.name == name ? &k : Find(k, name)) return f;
    return nullptr;
}
UINT32 Highlight(std::string_view v) {
    static const std::pair<std::string_view, UINT32> names[] = {
        {"yellow", 0xFFFF00}, {"green", 0x00FF00}, {"cyan", 0x00FFFF}, {"magenta", 0xFF00FF}, {"blue", 0x0000FF},
        {"red", 0xFF0000}, {"darkBlue", 0x000080}, {"darkCyan", 0x008080}, {"darkGreen", 0x008000},
        {"darkMagenta", 0x800080}, {"darkRed", 0x800000}, {"darkYellow", 0x808000}, {"darkGray", 0x808080},
        {"lightGray", 0xC0C0C0}, {"black", 0x000000}, {"white", 0xFFFFFF}};
    for (auto& [n, c] : names)
        if (n == v) return 0xFF000000 | c;
    return 0;
}

// ---- drawing state (UI thread)
ID2D1RenderTarget* g_rt;  // the target being painted (inline objects draw into it)
ComPtr<ID2D1RenderTarget> g_brushRt;  // the brushes' target (held: a new one at the same address is still new)
float g_origin;  // document y 0 on screen during a paint
float g_top, g_bottom;  // the visible area on screen during a paint
int g_paint;            // paint counter
std::map<UINT32, ComPtr<ID2D1SolidColorBrush>> g_brush;
float g_dpi = 96;
bool g_dark;
ComPtr<ID2D1StrokeStyle> g_dash, g_dot;

ID2D1SolidColorBrush* Brush(UINT32 key) {
    auto& b = g_brush[key];
    if (!b) g_rt->CreateSolidColorBrush(Rgb(key), &b);
    return b.Get();
}

// ---- pictures: decoded on a worker at the size drawn, kept by zip path while near the view
struct Pic {
    ComPtr<ID2D1Bitmap> bmp;
    UINT px = 0;  // width decoded at
    bool pending = false, failed = false;
    float y = 0;    // document y where last drawn on screen
    int seen = -1;  // paint that last drew it on screen
};
std::map<std::wstring, Pic> g_pics;
std::vector<std::tuple<std::wstring, UINT, UINT>> g_want;  // drawn without pixels during this paint
std::atomic<int> g_gen;

// ---- math (OMML): a box per element around a baseline (dy > 0 is down), drawn with Cambria Math
struct Stroke {
    std::vector<D2D1_POINT_2F> pts;
    float w;
};
struct MBox {
    float w = 0, asc = 0, desc = 0;
    char kind = 'e';
    std::wstring str;                // a run's text (--selftest)
    ComPtr<IDWriteTextLayout> text;  // a run, drawn with its baseline on the box's
    float base = 0, size = 0, sy = 1, cy = 0;  // layout baseline, font size; delimiters: stretch, centre
    std::vector<std::pair<D2D1_POINT_2F, MBox>> kids;
    std::vector<Stroke> strokes;
    void Add(MBox b, float x, float dy) {
        asc = std::max(asc, b.asc - dy), desc = std::max(desc, b.desc + dy), w = std::max(w, x + b.w);
        kids.push_back({{x, dy}, std::move(b)});
    }
    void Line(std::vector<D2D1_POINT_2F> pts, float width) {
        for (auto& p : pts) asc = std::max(asc, -p.y + width), desc = std::max(desc, p.y + width), w = std::max(w, p.x);
        strokes.push_back({std::move(pts), width});
    }
    std::wstring Outline() const {  // --selftest
        std::wstring s(1, (wchar_t)kind);
        if (!str.empty()) return s + L":" + str;
        s += L"(";
        for (auto& [p, k] : kids) s += k.Outline() + L",";
        return s + L")";
    }
};

void DrawBox(ID2D1RenderTarget* rt, const MBox& b, float x, float y, ID2D1Brush* br) {
    if (b.text) {
        if (b.sy != 1) {  // a delimiter stretched around its centre, moved to the content's centre
            D2D1_MATRIX_3X2_F m;
            rt->GetTransform(&m);
            float c = y - 0.25f * b.size;
            rt->SetTransform(Matrix3x2F::Scale(1, b.sy, {x, c}) * Matrix3x2F::Translation(0, y + b.cy - c) * m);
            rt->DrawTextLayout({x, y - b.base}, b.text.Get(), br, D2D1_DRAW_TEXT_OPTIONS_NONE);
            rt->SetTransform(m);
        } else
            rt->DrawTextLayout({x, y - b.base}, b.text.Get(), br, D2D1_DRAW_TEXT_OPTIONS_NONE);
    }
    for (auto& [p, k] : b.kids) DrawBox(rt, k, x + p.x, y + p.y, br);
    for (auto& s : b.strokes)
        for (size_t i = 1; i < s.pts.size(); i++)
            rt->DrawLine({x + s.pts[i - 1].x, y + s.pts[i - 1].y}, {x + s.pts[i].x, y + s.pts[i].y}, br, s.w);
}

struct Math {
    std::wstring font;  // the paragraph's font, for m:nor text
    float min = 6;      // smallest script size

    // A run of math text: letters italic (as Unicode math italic, which Cambria Math draws), operators spaced.
    MBox Text(const std::wstring& raw, float s, int style, bool& op, bool plain = false) {
        std::wstring t;
        auto put = [&](UINT32 cp) {
            if (cp > 0xFFFF) cp -= 0x10000, t += (wchar_t)(0xD800 + (cp >> 10)), t += (wchar_t)(0xDC00 + (cp & 0x3FF));
            else t += (wchar_t)cp;
        };
        for (size_t i = 0; i < raw.size(); i++) {
            wchar_t c = raw[i] == L'-' && !plain ? L'\x2212' : raw[i];
            bool rel = !plain && wcschr(L"=<>\x2264\x2265\x2248\x2260\x2261\x2192\x2190\x21D2\x21D4\x221D\x223C\x2245", c);
            bool bin = !plain && wcschr(L"+\x2212\xB1\x2213\xD7\xF7\xB7\x2217*\x22C5", c);
            if ((rel || bin) && !op) {
                wchar_t pad = rel ? L'\x2005' : L'\x205F';  // 1/4 em around relations, 2/9 em around binary operators
                t += pad, t += c, t += pad, op = true;
                continue;
            }
            op = !plain && (wcschr(L"([{,", c) || rel || bin);
            UINT32 cp = c;
            if (style & 1) {
                if (c >= L'a' && c <= L'z') cp = c == L'h' && !(style & 2) ? 0x210E : (style & 2 ? 0x1D482 : 0x1D44E) + (c - L'a');
                else if (c >= L'A' && c <= L'Z') cp = (style & 2 ? 0x1D468 : 0x1D434) + (c - L'A');
                else if (c >= 0x3B1 && c <= 0x3C9) cp = (style & 2 ? 0x1D736 : 0x1D6FC) + (c - 0x3B1);
            }
            put(cp);
            if (c == L',' && !plain && i + 1 < raw.size()) t += L'\x2009';
        }
        MBox b;
        b.kind = 't', b.str = raw, b.size = s;
        ComPtr<IDWriteTextFormat> f;
        if (FAILED(DW()->CreateTextFormat(plain ? font.c_str() : L"Cambria Math", nullptr,
                                          style == 2 ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                          DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, s, L"", &f)))
            return b;
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        if (FAILED(DW()->CreateTextLayout(t.c_str(), (UINT32)t.size(), f.Get(), 1e5f, 1e5f, &b.text))) return b;
        DWRITE_TEXT_METRICS m{};
        DWRITE_LINE_METRICS lm{};
        UINT32 n = 0;
        b.text->GetMetrics(&m);
        b.text->GetLineMetrics(&lm, 1, &n);
        b.w = m.widthIncludingTrailingWhitespace, b.base = lm.baseline, b.asc = 0.75f * s, b.desc = 0.25f * s;
        return b;
    }
    MBox Char(wchar_t c, float s) {
        bool op = true;
        return Text(std::wstring(1, c), s, 0, op);
    }
    // A delimiter as tall as asc/desc: the glyph stretched once the content is taller than the text.
    MBox Delim(std::wstring_view ch, float s, float asc, float desc) {
        bool op = true;
        MBox b = Text(std::wstring(ch), s, 0, op);
        b.kind = 'd';
        float h = asc + desc;
        if (h > 1.15f * s) b.sy = h / s * 1.04f, b.cy = (desc - asc) / 2, b.asc = -b.cy + 0.5f * s * b.sy, b.desc = b.cy + 0.5f * s * b.sy;
        return b;
    }
    std::wstring Chr(const Node* pr, std::string_view name, std::wstring def) {
        const Node* c = pr ? (*pr)[name] : nullptr;
        return c ? Wide(XmlText(c->A("val"))) : def;
    }

    MBox Row(const Node* n, float s, bool disp) {
        MBox b;
        bool op = true;  // at the start or after an operator: a sign is unary
        if (n)
            for (auto& k : n->kids)
                if (!k.name.ends_with("Pr")) b.Add(El(k, s, disp, op), b.w, 0);
        return b;
    }
    // Sub- and superscripts after `x` (or before, for sPre) of a base with base.asc / base.desc.
    void Scripts(MBox& out, float x, const MBox& base, const Node* sub, const Node* sup, float s) {
        float ss = std::max(min, s * 0.7f);
        MBox b = sub ? Row(sub, ss, false) : MBox{}, p = sup ? Row(sup, ss, false) : MBox{};
        float dn = std::max(0.18f * s, base.desc - 0.05f * s), up = std::max(0.38f * s, base.asc - 0.35f * s);
        if (sub && sup)
            if (float gap = (dn - b.asc) - (p.desc - up); gap < 0.1f * s) dn += (0.1f * s - gap) / 2, up += (0.1f * s - gap) / 2;
        if (sub) out.Add(std::move(b), x, dn);
        if (sup) out.Add(std::move(p), x, -up);
    }
    // Rows of cells (matrices, equation arrays), centred on the math axis.
    MBox Grid(const std::vector<std::vector<const Node*>>& rows, float s, bool left) {
        std::vector<std::vector<MBox>> cells;
        std::vector<float> colW, rowA, rowD;
        for (auto& r : rows) {
            auto& cr = cells.emplace_back();
            rowA.push_back(0.75f * s), rowD.push_back(0.25f * s);
            for (size_t j = 0; j < r.size(); j++) {
                cr.push_back(Row(r[j], s, false));
                if (colW.size() <= j) colW.push_back(0);
                colW[j] = std::max(colW[j], cr.back().w), rowA.back() = std::max(rowA.back(), cr.back().asc),
                rowD.back() = std::max(rowD.back(), cr.back().desc);
            }
        }
        float h = 0, gapY = 0.2f * s, gapX = 0.8f * s;
        for (size_t i = 0; i < rows.size(); i++) h += rowA[i] + rowD[i] + (i ? gapY : 0);
        MBox out;
        out.kind = 'm';
        float y = -0.25f * s - h / 2;
        for (size_t i = 0; i < cells.size(); i++) {
            y += rowA[i];
            float x = 0.1f * s;
            for (size_t j = 0; j < cells[i].size(); j++) {
                float cw = cells[i][j].w;
                out.Add(std::move(cells[i][j]), x + (left ? 0 : (colW[j] - cw) / 2), y);
                x += colW[j] + gapX;
            }
            y += rowD[i] + gapY;
        }
        out.w += 0.1f * s;
        return out;
    }

    MBox El(const Node& k, float s, bool disp, bool& op) {
        float t = std::max(0.05f * s, 0.75f), ss = std::max(min, s * 0.7f);
        const Node* pr = nullptr;
        for (auto& c : k.kids)
            if (c.name.ends_with("Pr") && c.name != "rPr") pr = &c;
        auto hidden = [&](std::string_view n) { return pr && On((*pr)[n]); };
        std::string_view n = k.name;
        if (n == "r") {
            std::wstring raw;
            int style = 1;
            bool plain = false;
            for (auto& c : k.kids)
                if (c.name == "t") raw += Wide(c.text);
                else if (c.name == "rPr") {
                    if (const Node* sty = c["sty"]) {
                        std::string_view v = sty->A("val");
                        style = v == "p" ? 0 : v == "b" ? 2 : v == "bi" ? 3 : 1;
                    }
                    if (On(c["nor"])) plain = true, style = 0;
                }
            return Text(raw, s, style, op, plain);
        }
        MBox out;
        if (n == "sSub" || n == "sSup" || n == "sSubSup") {
            out.kind = 's';
            MBox base = Row(k["e"], s, disp);
            float x = base.w + 0.04f * s;
            MBox copy;
            copy.asc = base.asc, copy.desc = base.desc;
            out.Add(std::move(base), 0, 0);
            Scripts(out, x, copy, k["sub"], k["sup"], s);
        } else if (n == "sPre") {
            out.kind = 's';
            MBox base = Row(k["e"], s, disp), copy;
            copy.asc = base.asc, copy.desc = base.desc;
            Scripts(out, 0, copy, k["sub"], k["sup"], s);
            out.Add(std::move(base), out.w + 0.04f * s, 0);
        } else if (n == "f") {
            out.kind = 'f';
            std::string_view type = pr && (*pr)["type"] ? (*pr)["type"]->A("val") : "bar";
            float cs = disp ? s : std::max(min, s * 0.75f);
            if (type == "lin") {
                out.Add(Row(k["num"], s, false), 0, 0);
                out.Add(Char(L'/', s), out.w, 0);
                out.Add(Row(k["den"], s, false), out.w, 0);
            } else {
                MBox a = Row(k["num"], cs, false), b = Row(k["den"], cs, false);
                float axis = 0.25f * s, g = 0.12f * s, w = std::max(a.w, b.w) + 0.2f * s, x0 = 0.08f * s;
                float aw = a.w, bw = b.w, ad = a.desc, ba = b.asc;
                out.Add(std::move(a), x0 + (w - aw) / 2, -(axis + t / 2 + g + ad));
                out.Add(std::move(b), x0 + (w - bw) / 2, -axis + t / 2 + g + ba);
                if (type != "noBar") out.Line({{x0, -axis}, {x0 + w, -axis}}, t);
                out.w = x0 + w + 0.08f * s;
            }
        } else if (n == "rad") {
            out.kind = 'r';
            MBox e = Row(k["e"], s, disp);
            const Node* degN = k["deg"];
            bool deg = !hidden("degHide") && degN && !degN->kids.empty();
            MBox d = deg ? Row(degN, std::max(min, s * 0.55f), false) : MBox{};
            float top = -(e.asc + 0.12f * s + t / 2), bottom = e.desc + 0.05f * s, h = bottom - top;
            float sx = deg ? std::max(0.f, d.w - 0.3f * s) : 0, ew = e.w;
            if (deg) out.Add(std::move(d), 0, top + 0.5f * h - 0.05f * s);
            out.Add(std::move(e), sx + 0.62f * s, 0);
            out.Line({{sx, top + 0.62f * h}, {sx + 0.12f * s, top + 0.55f * h}, {sx + 0.3f * s, bottom}, {sx + 0.55f * s, top},
                      {sx + 0.72f * s + ew, top}},
                     t);
            out.w += 0.06f * s;
        } else if (n == "d") {
            out.kind = 'D';
            std::wstring beg = Chr(pr, "begChr", L"("), end = Chr(pr, "endChr", L")"), sep = Chr(pr, "sepChr", L"|");
            MBox in;
            for (auto& c : k.kids)
                if (c.name == "e") {
                    if (!in.kids.empty()) in.Add(Delim(sep, s, 0, 0), in.w + 0.05f * s, 0), in.w += 0.05f * s;
                    in.Add(Row(&c, s, disp), in.w, 0);
                }
            float a = std::max(in.asc, 0.7f * s), dd = std::max(in.desc, 0.2f * s);
            if (!beg.empty()) out.Add(Delim(beg, s, a, dd), 0, 0);
            out.Add(std::move(in), out.w + 0.03f * s, 0);
            if (!end.empty()) out.Add(Delim(end, s, a, dd), out.w + 0.03f * s, 0);
        } else if (n == "nary") {
            out.kind = 'n';
            std::wstring chr = Chr(pr, "chr", L"\x222B");
            bool integral = chr.size() == 1 && ((chr[0] >= 0x222B && chr[0] <= 0x2233) || chr[0] == 0x2A0C);
            std::string_view loc = pr && (*pr)["limLoc"] ? (*pr)["limLoc"]->A("val") : integral ? "subSup" : "undOvr";
            float S = disp ? 1.45f * s : 1.15f * s, dy = 0.25f * (S - s);
            bool op2 = true;
            MBox o = Text(chr, S, 0, op2);
            o.asc = 0.85f * S, o.desc = 0.3f * S;
            const Node* sub = hidden("subHide") ? nullptr : k["sub"];
            const Node* sup = hidden("supHide") ? nullptr : k["sup"];
            if (sub && sub->kids.empty()) sub = nullptr;
            if (sup && sup->kids.empty()) sup = nullptr;
            float ow = o.w, oa = o.asc, od = o.desc;
            if (loc == "undOvr") {
                MBox b = sub ? Row(sub, ss, false) : MBox{}, p = sup ? Row(sup, ss, false) : MBox{};
                float w = std::max({ow, b.w, p.w}), bw = b.w, pw = p.w, ba = b.asc, pd = p.desc;
                out.Add(std::move(o), (w - ow) / 2, dy);
                if (sup) out.Add(std::move(p), (w - pw) / 2, dy - oa - 0.05f * s - pd);
                if (sub) out.Add(std::move(b), (w - bw) / 2, dy + od + 0.05f * s + ba);
            } else {
                out.Add(std::move(o), 0, dy);
                if (sup) {
                    MBox p = Row(sup, ss, false);
                    float pa = p.asc;
                    out.Add(std::move(p), ow + 0.05f * s, dy - oa + pa * 0.9f);
                }
                if (sub) out.Add(Row(sub, ss, false), std::max(0.f, ow - 0.25f * S), dy + od);
            }
            out.Add(Row(k["e"], s, disp), out.w + 0.12f * s, 0);
        } else if (n == "acc") {
            out.kind = 'a';
            std::wstring chr = Chr(pr, "chr", L"\x0302");
            MBox base = Row(k["e"], s, disp);
            float w = base.w, top = -std::max(base.asc, 0.7f * s) - 0.05f * s;
            out.Add(std::move(base), 0, 0);
            wchar_t c = chr.empty() ? 0x302 : chr[0];
            if (c == 0x20D7 || c == 0x20D6 || c == 0x20E1 || c == 0x2192 || c == 0x2190) {  // vector arrow over the whole base
                float y = top - 0.1f * s, l = 0.04f * s, r = std::max(w - 0.02f * s, l + 0.3f * s), hd = 0.11f * s;
                out.Line({{l, y}, {r, y}}, t);
                if (c != 0x20D6 && c != 0x2190) out.Line({{r - hd, y - hd * 0.7f}, {r, y}, {r - hd, y + hd * 0.7f}}, t);
                if (c == 0x20D6 || c == 0x2190 || c == 0x20E1) out.Line({{l + hd, y - hd * 0.7f}, {l, y}, {l + hd, y + hd * 0.7f}}, t);
            } else if (c == 0x304 || c == 0x305 || c == 0xAF || c == 0x203E) {
                out.Line({{0.04f * s, top - 0.06f * s}, {std::max(w - 0.02f * s, 0.3f * s), top - 0.06f * s}}, t);
            } else {
                static const std::pair<wchar_t, wchar_t> spacing[] = {{0x302, 0x2C6}, {0x303, 0x2DC}, {0x307, 0x2D9}, {0x308, 0xA8},
                                                                      {0x301, 0xB4}, {0x300, 0x60}, {0x30C, 0x2C7}, {0x306, 0x2D8}};
                for (auto& [comb, sp] : spacing)
                    if (c == comb) c = sp;
                MBox a = Char(c, s);
                float aw = a.w;
                out.Add(std::move(a), (w - aw) / 2, top + 0.62f * s);
            }
        } else if (n == "bar") {
            out.kind = 'b';
            MBox base = Row(k["e"], s, disp);
            float y = pr && (*pr)["pos"] && (*pr)["pos"]->A("val") == "top" ? -base.asc - 0.08f * s : base.desc + 0.06f * s;
            float w = base.w;
            out.Add(std::move(base), 0, 0);
            out.Line({{0, y}, {w, y}}, t);
        } else if (n == "func") {
            out.kind = 'F';
            out.Add(Row(k["fName"], s, disp), 0, 0);
            out.Add(Row(k["e"], s, disp), out.w + 0.17f * s, 0);
        } else if (n == "limLow" || n == "limUpp") {
            out.kind = 'l';
            MBox base = Row(k["e"], s, disp), lim = Row(k["lim"], ss, false);
            float w = std::max(base.w, lim.w), bw = base.w, lw = lim.w;
            float dy = n == "limLow" ? base.desc + 0.05f * s + lim.asc : -base.asc - 0.05f * s - lim.desc;
            out.Add(std::move(base), (w - bw) / 2, 0);
            out.Add(std::move(lim), (w - lw) / 2, dy);
        } else if (n == "groupChr") {
            out.kind = 'g';
            MBox base = Row(k["e"], s, disp);
            bool top = pr && (*pr)["pos"] && (*pr)["pos"]->A("val") == "top";
            MBox c = Char(Chr(pr, "chr", L"\x23DF").c_str()[0], s);
            float w = std::max(base.w, c.w), bw = base.w, cw = c.w, dy = top ? -base.asc - 0.3f * s : base.desc + 0.75f * s;
            out.Add(std::move(base), (w - bw) / 2, 0);
            out.Add(std::move(c), (w - cw) / 2, dy);
        } else if (n == "borderBox") {
            out.kind = 'B';
            MBox e = Row(k["e"], s, disp);
            float p = 0.1f * s, w = e.w + 2 * p, a = e.asc + p, d = e.desc + p;
            out.Add(std::move(e), p, 0);
            out.Line({{0, -a}, {w, -a}, {w, d}, {0, d}, {0, -a}}, t);
        } else if (n == "m") {
            std::vector<std::vector<const Node*>> rows;
            for (auto& r : k.kids)
                if (r.name == "mr") {
                    auto& row = rows.emplace_back();
                    for (auto& c : r.kids)
                        if (c.name == "e") row.push_back(&c);
                }
            out = Grid(rows, s, false);
        } else if (n == "eqArr") {
            std::vector<std::vector<const Node*>> rows;
            for (auto& c : k.kids)
                if (c.name == "e") rows.push_back({&c});
            out = Grid(rows, s, true);
        } else {  // box, phant, e and anything unknown: its content in a row (never dropped)
            out = Row(&k, s, disp);
            out.kind = 'e';
        }
        op = false;
        return out;
    }
};

// ---- inline objects: pictures, math, super/subscript text, a first-line indent
struct Obj final : IDWriteInlineObject {
    ULONG refs = 1;
    float w = 0, asc = 0, desc = 0;
    std::wstring media;  // picture: zip path
    bool placeholder = false;  // chart, SmartArt, unreadable picture
    MBox math;
    bool isMath = false;
    float align = 0;  // display math: as wide as the line, the equation at 0 left, 0.5 centred, 1 right
    ComPtr<IDWriteTextLayout> script;  // super/subscript

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (id == __uuidof(IUnknown) || id == __uuidof(IDWriteInlineObject)) return AddRef(), *out = this, S_OK;
        return *out = nullptr, E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG n = --refs;
        if (!n) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE Draw(void*, IDWriteTextRenderer*, FLOAT x, FLOAT y, BOOL, BOOL, IUnknown* effect) override {
        ComPtr<ID2D1Brush> br;
        if (!effect || FAILED(effect->QueryInterface(IID_PPV_ARGS(&br)))) br = Brush(kAuto);
        if (isMath) DrawBox(g_rt, math, x + align * (w - math.w), y + asc, br.Get());
        else if (script) g_rt->DrawTextLayout({x, y}, script.Get(), br.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        else if (!media.empty() || placeholder) {
            D2D1_RECT_F r{x, y, x + w, y + asc};
            Pic* p = media.empty() ? nullptr : &g_pics[media];
            // DirectWrite draws every object of a visible paragraph, also far off screen: only those on screen count.
            bool visible = r.bottom >= g_top && r.top <= g_bottom;
            if (p && visible) p->y = y - g_origin, p->seen = g_paint;
            if (p && p->bmp) g_rt->DrawBitmap(p->bmp.Get(), r, 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            if (!p || p->failed) {  // quiet placeholder
                ID2D1SolidColorBrush* b = Brush(kAuto);
                float a = b->GetOpacity();
                b->SetOpacity(0.25f);
                g_rt->DrawRectangle({r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f}, b, 1);
                b->SetOpacity(a);
            }
            UINT px = std::max(1u, (UINT)std::lround(w * g_dpi / 96)), py = std::max(1u, (UINT)std::lround(asc * g_dpi / 96));
            if (p && visible && !p->pending && !p->failed && (!p->bmp || px > p->px * 5 / 4)) g_want.push_back({media, px, py}), p->pending = true;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMetrics(DWRITE_INLINE_OBJECT_METRICS* m) override {
        *m = {w, asc + desc, asc, FALSE};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetOverhangMetrics(DWRITE_OVERHANG_METRICS* m) override {
        *m = {};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetBreakConditions(DWRITE_BREAK_CONDITION* before, DWRITE_BREAK_CONDITION* after) override {
        *before = *after = media.empty() && !placeholder ? DWRITE_BREAK_CONDITION_NEUTRAL : DWRITE_BREAK_CONDITION_CAN_BREAK;
        return S_OK;
    }
};

// ---- the document
struct Bd {  // a border line
    float w = 0, space = 0;
    UINT32 color = 0;  // 0: automatic
    UINT8 style = 0;   // 0 none, 1 solid, 2 dashed, 3 dotted
};
Bd Border(const Node* b) {
    Bd d;
    std::string_view v = b ? b->A("val") : "";
    if (v.empty() || v == "nil" || v == "none") return d;
    d.w = std::max(0.75f, Num(b->A("sz"), 4) / 8 * kPt), d.space = Num(b->A("space")) * kPt, d.color = Hex(b->A("color"));
    d.style = v.find("dot") != v.npos ? (v.find("Dash") != v.npos ? 2 : 3) : v.find("ash") != v.npos ? 2 : 1;
    return d;
}

struct Table;
struct Item {  // a paragraph or a table, laid out; positions relative to the item's top-left
    float y = 0, h = 0, after = 0;
    ComPtr<IDWriteTextLayout> text, bullet;
    float tx = 0, ty = 0, bx = 0;
    D2D1_RECT_F band{};  // shading / borders
    UINT32 fill = 0;
    Bd bd[4];  // left top right bottom
    std::vector<std::pair<D2D1_RECT_F, UINT32>> marks;  // highlights, relative to the text origin
    std::unique_ptr<Table> table;
    std::string_view style;
    std::vector<const Node*> boxes;  // text box paragraphs: laid out after this one
};
struct Cell {
    float x, w, ml, mt, contentH = 0;
    size_t row, span = 1;
    UINT32 fill = 0;
    Bd bd[4];
    int valign = 0;  // 0 top, 1 centre, 2 bottom
    std::vector<Item> items;
    const Node* tc = nullptr;  // laid out when its row is reached
    float mr = 0, mb = 0;
};
// Cells (positions, spans, borders) are known up front; rows are laid out as far as the view needs.
struct Table {
    float x = 0;
    std::vector<float> rowY{0};  // edges of the rows laid out so far
    std::vector<Cell> cells;     // in row order
    std::vector<float> rowMin;   // per row
    std::vector<bool> exact;
    size_t next = 0;             // first cell not laid out
    std::vector<size_t> spans;   // laid-out cells spanning rows below
    UINT32 bg = 0;
    const Node* ts = nullptr;
    size_t Laid() const { return rowY.size() - 1; }
    bool Done() const { return Laid() >= rowMin.size(); }
};

struct Ctx {
    UINT32 bg = 0;                 // fill behind the text (cell shading); 0: the window
    const Node* tstyle = nullptr;  // table style of the cell's table
};

struct Word {
    std::wstring path;
    Members m;
    Node stylesRoot, numRoot;
    const Node *styles = nullptr, *numbering = nullptr, *defPara = nullptr;
    std::map<std::string_view, const Node*> byId;
    std::unordered_map<std::string, std::wstring> rels;
    std::wstring major = L"Calibri Light", minor = L"Calibri";
    float column = 624;
    float pitch = 0;  // document grid (East Asian layouts): lines snap to multiples of this, DIPs
    std::map<std::pair<std::string, int>, int> counters;  // (numId, level) -> last number
    int notes = 0;
    // body: parsed one block at a time
    std::string_view xml;
    Xml scan;
    size_t bodyStart = 0, bodyEnd = 1;
    bool done = false;
    std::deque<Node> parsed;
    std::vector<const Node*> blocks;

    const Node* Style(std::string_view id) {
        auto it = byId.find(id);
        return it == byId.end() ? nullptr : it->second;
    }
    void Chain(const Node* s, std::string_view prop, std::vector<const Node*>& out) {  // a style and its bases
        for (int guard = 0; s && guard < 20; guard++) {
            out.push_back((*s)[prop]);
            const Node* based = (*s)["basedOn"];
            s = based ? Style(based->A("val")) : nullptr;
        }
    }
    static void Flatten(const Node& n, std::vector<const Node*>& out) {  // block-level content controls and the like
        for (auto& k : n.kids)
            if (k.name == "p" || k.name == "tbl") out.push_back(&k);
            else if (k.name == "sdt" || k.name == "sdtContent" || k.name == "customXml" || k.name == "ins" || k.name == "moveTo")
                Flatten(k, out);
    }
    // Makes block i available, parsing body children as needed.
    bool Block(size_t i) {
        while (i >= blocks.size() && !done) {
            int t = scan.Next();
            if (!t || (t == Xml::Tag && scan.close)) done = true;
            else if (t == Xml::Tag) {
                Node& n = parsed.emplace_back(Node{scan.name, scan.attrs});
                ParseKids(scan, n);
                if (n.name == "p" || n.name == "tbl") blocks.push_back(&n);
                else Flatten(n, blocks);
            }
        }
        return i < blocks.size();
    }
    float Parsed() const { return done ? 1 : std::clamp((float)(scan.i - bodyStart) / (bodyEnd - bodyStart), 0.01f, 1.f); }
};
std::unique_ptr<Word> g_w;
std::vector<Item> g_items;  // top-level blocks laid out at g_width
size_t g_next;              // next block of g_w->blocks to lay out
float g_width = -1, g_laidH;

void LayoutAll(Word& d, const std::vector<const Node*>& nodes, float width, const Ctx& ctx, std::vector<Item>& out);

// ---- paragraphs
struct RunFmt {
    std::wstring font;
    float size;
    bool b, i, u, strike, caps, hidden;
    UINT32 color, mark;
    int va;  // 1 superscript, 2 subscript
    DWRITE_FONT_WEIGHT w = DWRITE_FONT_WEIGHT_NORMAL;  // of the face ("Calibri Light"), when not bold
    DWRITE_FONT_STRETCH st = DWRITE_FONT_STRETCH_NORMAL;
    DWRITE_FONT_WEIGHT Weight() const { return b ? std::max(w, DWRITE_FONT_WEIGHT_BOLD) : w; }  // Arial Black stays 900
};

struct Para {
    Word& d;
    const Node& p;
    Item& it;
    float width;
    Ctx ctx;
    std::vector<const Node*> pp, pr;  // paragraph properties, run properties of the paragraph style (nearest first)
    std::wstring text;
    struct Run {
        UINT32 start, len;
        RunFmt f;
        UINT32 ink;
        bool link;
    };
    std::vector<Run> runs;
    std::vector<std::pair<UINT32, ComPtr<Obj>>> objs;
    std::vector<ComPtr<Obj>> anchors;
    UINT32 bg = 0;  // paragraph shading, else the context's
    int link = 0;
    float avail = 1, gap = 0;  // object width limit; extra space after (page breaks)
    bool breakNext = false;  // after display math: what follows starts a new line

    const Node* PC(std::string_view n, std::string_view attr = {}) {
        for (auto x : pp)
            if (const Node* k = x ? (*x)[n] : nullptr; k && (attr.empty() || !k->A(attr).empty())) return k;
        return nullptr;
    }
    std::string_view PA(std::string_view n, std::string_view a) {
        const Node* k = PC(n, a);
        return k ? k->A(a) : std::string_view{};
    }

    RunFmt Props(const Node* rPr) {
        std::vector<const Node*> rr{rPr};
        if (const Node* rs = rPr ? (*rPr)["rStyle"] : nullptr) d.Chain(d.Style(rs->A("val")), "rPr", rr);
        rr.insert(rr.end(), pr.begin(), pr.end());
        auto rc = [&](std::string_view n, std::string_view a = {}) -> const Node* {
            for (auto x : rr)
                if (const Node* k = x ? (*x)[n] : nullptr; k && (a.empty() || !k->A(a).empty())) return k;
            return nullptr;
        };
        RunFmt f{d.minor, Num(rc("sz", "val") ? rc("sz", "val")->A("val") : "", 20) / 2 * kPt, On(rc("b")), On(rc("i")),
                 rc("u") && rc("u")->A("val") != "none", On(rc("strike")) || On(rc("dstrike")), On(rc("caps")) || On(rc("smallCaps")),
                 On(rc("vanish")), 0, 0, 0};
        for (auto x : rr)
            if (const Node* k = x ? (*x)["rFonts"] : nullptr) {
                if (!k->A("ascii").empty()) {
                    f.font = Wide(k->A("ascii"));
                    break;
                }
                if (!k->A("asciiTheme").empty()) {
                    f.font = k->A("asciiTheme").starts_with("major") ? d.major : d.minor;
                    break;
                }
            }
        FontFace face = ResolveFont(f.font);
        f.font = face.family, f.w = face.weight, f.st = face.stretch;
        if (const Node* c = rc("color", "val")) f.color = Hex(c->A("val"));
        if (const Node* h = rc("highlight", "val")) f.mark = Highlight(h->A("val"));
        if (const Node* s = rc("shd", "fill"); s && !f.mark) f.mark = Hex(s->A("fill"));
        if (const Node* v = rc("vertAlign", "val")) f.va = v->A("val") == "superscript" ? 1 : v->A("val") == "subscript" ? 2 : 0;
        return f;
    }
    // The colour drawn: automatic is black or white on a fill, the palette's text colour on the window.
    // Explicit colours stay, except where they would vanish into the window: near-white in light mode takes the
    // palette's colour, on the dark window DarkInk decides (near-black: the palette's; dark: lightness mirrored).
    UINT32 Ink(UINT32 c, UINT32 under) {
        if (under) return c ? c : Lum(under) < 0.5f ? 0xFFFFFFFF : 0xFF000000;
        if (link) return kLink;
        if (g_dark && c) c = DarkInk(c);
        return !c || (!g_dark && Lum(c) > 0.85f) ? kAuto : c;
    }
    void Break() {  // a line break, unless the line is empty
        if (!text.empty() && text.back() != L'\u2028') text += L'\u2028';
    }
    void Add(std::wstring s, const RunFmt& f) {
        if (f.hidden || s.empty()) return;
        if (breakNext) breakNext = false, Break();
        if (f.caps) CharUpperBuffW(s.data(), (DWORD)s.size());
        if (f.va) {  // super/subscript: smaller text raised or lowered, as an object
            auto o = Make<Obj>();
            ComPtr<IDWriteTextFormat> fmt;
            float size = f.size * 0.65f, shift = f.va == 1 ? -0.33f * f.size : 0.14f * f.size;
            if (FAILED(DW()->CreateTextFormat(f.font.c_str(), nullptr, f.Weight(), f.i ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, f.st,
                                              size, L"", &fmt)) ||
                FAILED(DW()->CreateTextLayout(s.c_str(), (UINT32)s.size(), fmt.Get(), 1e5f, 1e5f, &o->script)))
                return;
            o->script->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            DWRITE_TEXT_METRICS m{};
            DWRITE_LINE_METRICS lm{};
            UINT32 n = 0;
            o->script->GetMetrics(&m);
            o->script->GetLineMetrics(&lm, 1, &n);
            o->w = m.widthIncludingTrailingWhitespace, o->asc = lm.baseline - shift, o->desc = std::max(0.f, m.height - lm.baseline + shift);
            Object(o, f);
            return;
        }
        runs.push_back({(UINT32)text.size(), (UINT32)s.size(), f, Ink(f.color, f.mark ? f.mark : bg), link > 0});
        text += s;
    }
    template <class T> static ComPtr<T> Make() {
        ComPtr<T> o;
        o.Attach(new T);
        return o;
    }
    void Object(const ComPtr<Obj>& o, const RunFmt& f) {
        if (breakNext) breakNext = false, Break();
        objs.push_back({(UINT32)text.size(), o});
        runs.push_back({(UINT32)text.size(), 1, f, Ink(f.color, f.mark ? f.mark : bg), link > 0});
        text += kObj;
    }
    void Picture(const std::wstring& media, float w, float h, bool anchored, const RunFmt& f, bool placeholder = false) {
        if (w <= 0 || h <= 0) return;
        if (w > avail) h *= avail / w, w = avail;
        auto o = Make<Obj>();
        o->media = media, o->placeholder = placeholder, o->w = w, o->asc = h;
        if (anchored) anchors.push_back(o);
        else Object(o, f);
    }
    void Boxes(const Node* content) {
        if (content) Word::Flatten(*content, it.boxes);
    }
    void Drawing(const Node& dr, const RunFmt& f) {
        const Node* holder = dr.kids.empty() ? nullptr : &dr.kids[0];  // wp:inline / wp:anchor
        const Node* ext = holder ? (*holder)["extent"] : nullptr;
        const Node* gd = At(holder, {"graphic", "graphicData"});
        if (!ext || !gd) return;
        float w = (float)ext->D("cx", 0) / 9525, h = (float)ext->D("cy", 0) / 9525;  // EMU -> DIPs
        bool anchored = holder->name == "anchor";
        if (const Node* blip = At(gd, {"pic", "blipFill", "blip"})) {
            auto rel = d.rels.find(std::string(blip->A("embed")));
            Picture(rel == d.rels.end() ? L"" : rel->second, w, h, anchored, f, rel == d.rels.end());
        } else if (const Node* tb = Find(*gd, "txbxContent")) Boxes(tb);
        else Picture(L"", w, h, anchored, f, true);  // chart, SmartArt, shapes
    }
    void Vml(const Node& pict, const RunFmt& f) {
        if (const Node* tb = Find(pict, "txbxContent")) return Boxes(tb);
        const Node* img = Find(pict, "imagedata");
        const Node* shape = Find(pict, "shape");
        if (!img || !shape) return;
        std::string_view st = shape->A("style");
        auto dim = [&](std::string_view key) {  // "width:52.5pt" -> DIPs
            size_t k = st.find(key);
            if (k == st.npos) return 0.f;
            std::string_view v = st.substr(k + key.size());
            float n = Num(v);
            size_t e = v.find_first_not_of("0123456789.-");
            std::string_view unit = e == v.npos ? "" : v.substr(e, 2);
            return n * (unit == "in" ? 96 : unit == "px" ? 1 : unit == "cm" ? 96 / 2.54f : unit == "mm" ? 96 / 25.4f : kPt);
        };
        auto rel = d.rels.find(std::string(img->A("id")));
        bool anchored = st.find("absolute") != st.npos;
        Picture(rel == d.rels.end() ? L"" : rel->second, dim("width:"), dim("height:"), anchored, f, rel == d.rels.end());
    }
    void AddMath(const Node& m, bool disp, float size, float align = 0.5f) {
        auto o = Make<Obj>();
        o->isMath = true;
        Math mt{Props(nullptr).font, std::max(6.f, size * 0.5f)};
        o->math = mt.Row(&m, size, disp);
        o->w = disp ? std::max(avail, o->math.w) : o->math.w, o->asc = o->math.asc, o->desc = o->math.desc, o->align = align;
        if (disp) Break();
        RunFmt f = Props(nullptr);
        Object(o, f);
    }
    void Runs(const Node& r) {
        const Node* rPr = r["rPr"];
        RunFmt f = Props(rPr);
        for (auto& c : r.kids) {
            if (c.name == "t") Add(Wide(c.text), f);
            else if (c.name == "tab" || c.name == "ptab") Add(L"\t", f);
            else if (c.name == "br" || c.name == "cr") {
                if (c.A("type") == "page" || c.A("type") == "column") gap += 12;
                else Add(L"\u2028", f);
            } else if (c.name == "noBreakHyphen") Add(L"‑", f);
            else if (c.name == "sym") {
                RunFmt g = f;
                if (!c.A("font").empty()) g.font = Wide(c.A("font"));
                Add(std::wstring(1, (wchar_t)strtoul(std::string(c.A("char")).c_str(), nullptr, 16)), g);
            } else if (c.name == "footnoteReference" || c.name == "endnoteReference") {
                RunFmt g = f;
                g.va = 1;
                Add(std::to_wstring(++d.notes), g);
            } else if (c.name == "drawing") Drawing(c, f);
            else if (c.name == "pict" || c.name == "object") Vml(c, f);
            else if (c.name == "AlternateContent") {  // the picture when the preferred form is one, else the fallback
                const Node* ch = c["Choice"];
                if (const Node* dr = ch ? (*ch)["drawing"] : nullptr; dr && Find(*dr, "pic")) Drawing(*dr, f);
                else if (const Node* fb = c["Fallback"])
                    for (auto& k : fb->kids)
                        if (k.name == "drawing") Drawing(k, f);
                        else if (k.name == "pict" || k.name == "object") Vml(k, f);
            }
        }
    }
    void Walk(const Node& n) {  // runs, also inside hyperlinks, fields, content controls and insertions
        for (auto& k : n.kids) {
            if (k.name == "r") Runs(k);
            else if (k.name == "hyperlink") link++, Walk(k), link--;
            else if (k.name == "smartTag" || k.name == "sdt" || k.name == "sdtContent" || k.name == "ins" || k.name == "fldSimple" ||
                     k.name == "customXml" || k.name == "moveTo" || k.name == "bdo" || k.name == "dir")
                Walk(k);
            else if (k.name == "oMath") AddMath(k, false, Props(nullptr).size);
            else if (k.name == "oMathPara") {
                const Node* jc = At(&k, {"oMathParaPr", "jc"});
                std::string_view a = jc ? jc->A("val") : "";
                for (auto& m : k.kids)
                    if (m.name == "oMath") AddMath(m, true, Props(nullptr).size, a == "left" ? 0 : a == "right" ? 1 : 0.5f), breakNext = true;
            }
        }
    }

    // Numbering: the list level's text ("%1.", a bullet), counted per list; its indent unless the paragraph has one.
    std::wstring Number(float& left, float& hanging) {
        const Node* numPr = PC("numPr");
        if (!numPr) return {};
        std::string numId((*numPr)["numId"] ? (*numPr)["numId"]->A("val") : std::string_view{});
        int ilvl = (*numPr)["ilvl"] ? (int)(*numPr)["ilvl"]->D("val", 0) : 0;
        const Node* lvl = nullptr;
        if (d.numbering && !numId.empty() && numId != "0")
            for (auto& n : d.numbering->kids)
                if (n.name == "num" && n.A("numId") == numId && n["abstractNumId"])
                    for (auto& a : d.numbering->kids)
                        if (a.name == "abstractNum" && a.A("abstractNumId") == n["abstractNumId"]->A("val"))
                            for (auto& l : a.kids)
                                if (l.name == "lvl" && (int)l.D("ilvl", 0) == ilvl) lvl = &l;
        if (!lvl) return {};
        std::string_view fmt = (*lvl)["numFmt"] ? (*lvl)["numFmt"]->A("val") : "decimal";
        std::wstring tmpl = (*lvl)["lvlText"] ? Wide(XmlText((*lvl)["lvlText"]->A("val"))) : L"%1.";
        for (auto& [key, v] : d.counters)
            if (key.first == numId && key.second > ilvl) v = 0;
        int& n = d.counters[{numId, ilvl}];
        n = n ? n + 1 : (*lvl)["start"] ? (int)(*lvl)["start"]->D("val", 1) : 1;
        std::wstring out;
        if (fmt == "bullet") out = tmpl.empty() || tmpl[0] < 0x20 || tmpl[0] >= 0xF000 ? L"•" : tmpl;
        else if (fmt != "none") {
            for (size_t k; (k = tmpl.find(L'%')) != tmpl.npos && k + 1 < tmpl.size();) {
                int which = tmpl[k + 1] - L'1';
                int v = which == ilvl ? n : std::max(1, d.counters[{numId, which}]);
                std::wstring s = fmt == "lowerLetter" ? std::wstring(1, wchar_t(L'a' + (v - 1) % 26))
                                 : fmt == "upperLetter" ? std::wstring(1, wchar_t(L'A' + (v - 1) % 26))
                                 : fmt == "lowerRoman" || fmt == "upperRoman" ? Roman(v, fmt == "upperRoman")
                                                                               : std::to_wstring(v);
                tmpl.replace(k, 2, s);
            }
            out = tmpl;
        }
        if (const Node* ind = At(lvl, {"pPr", "ind"}); ind && !At(p["pPr"], {"ind"})) {
            left = Twips(ind->A("left").empty() ? ind->A("start") : ind->A("left"), left);
            hanging = Twips(ind->A("hanging"), hanging);
        }
        return out;
    }
    static std::wstring Roman(int v, bool upper) {
        if (v > 3999) return std::to_wstring(v);  // a start value from the file: not millions of "m"
        static const std::pair<int, const wchar_t*> r[] = {{1000, L"m"}, {900, L"cm"}, {500, L"d"}, {400, L"cd"}, {100, L"c"}, {90, L"xc"},
                                                           {50, L"l"},   {40, L"xl"},  {10, L"x"},  {9, L"ix"},   {5, L"v"},   {4, L"iv"}, {1, L"i"}};
        std::wstring s;
        for (auto& [n, t] : r)
            while (v >= n) s += t, v -= n;
        if (upper) CharUpperBuffW(s.data(), (DWORD)s.size());
        return s;
    }

    void Build(const Item* prev) {
        const Node* pPr = p["pPr"];
        const Node* ps = pPr && (*pPr)["pStyle"] ? d.Style((*pPr)["pStyle"]->A("val")) : d.defPara;
        pp = {pPr};
        d.Chain(ps, "pPr", pp);
        if (ctx.tstyle) d.Chain(ctx.tstyle, "pPr", pp);
        pp.push_back(At(d.styles, {"docDefaults", "pPrDefault", "pPr"}));
        d.Chain(ps, "rPr", pr);
        if (ctx.tstyle) d.Chain(ctx.tstyle, "rPr", pr);
        pr.push_back(At(d.styles, {"docDefaults", "rPrDefault", "rPr"}));
        it.style = ps ? ps->A("styleId") : "";

        float left = Twips(PA("ind", "left").empty() ? PA("ind", "start") : PA("ind", "left"));
        float right = Twips(PA("ind", "right").empty() ? PA("ind", "end") : PA("ind", "right"));
        float hanging = Twips(PA("ind", "hanging")), first = Twips(PA("ind", "firstLine"));
        std::wstring bullet = Number(left, hanging);
        if (const Node* shd = PC("shd")) {
            UINT32 f = shd->A("val") == "solid" ? Hex(shd->A("color")) : Hex(shd->A("fill"));
            if (f) it.fill = f;
        }
        static const char* sides[] = {"left", "top", "right", "bottom"};
        if (const Node* b = PC("pBdr"))
            for (int s = 0; s < 4; s++) it.bd[s] = Border((*b)[sides[s]]);
        bg = it.fill ? it.fill : ctx.bg;
        float tw = std::max(1.f, width - left - right);
        avail = tw;
        if (first > 0 && bullet.empty()) {  // first-line indent: an empty object that wide
            auto o = Make<Obj>();
            o->w = first;
            Object(o, Props(nullptr));
        }
        Walk(p);
        if (!anchors.empty()) {  // floating pictures: after the paragraph's text
            if (!text.empty()) text += L'\u2028';
            for (auto& o : anchors) Object(o, Props(nullptr));
        }

        // Spacing (twips); a page break or section end leaves some room.
        bool contextual = On(PC("contextualSpacing"));
        float before = PA("spacing", "beforeAutospacing") == "1" ? 14 * kPt : Twips(PA("spacing", "before"));
        it.after = (PA("spacing", "afterAutospacing") == "1" ? 14 * kPt : Twips(PA("spacing", "after"))) + gap;
        float lineUnit = d.pitch > 0 ? d.pitch : 12 * kPt;  // beforeLines/afterLines: hundredths of a line
        if (!PA("spacing", "beforeLines").empty()) before = Num(PA("spacing", "beforeLines")) / 100 * lineUnit;
        if (!PA("spacing", "afterLines").empty()) it.after = Num(PA("spacing", "afterLines")) / 100 * lineUnit + gap;
        if (On(PC("pageBreakBefore"))) before += 12;
        if (pPr && (*pPr)["sectPr"]) it.after += 12;
        if (contextual && prev && !prev->table && prev->style == it.style) before = 0, it.y -= prev->after;

        RunFmt base = runs.empty() ? Props(At(pPr, {"rPr"})) : runs[0].f;
        ComPtr<IDWriteTextFormat> fmt;
        if (FAILED(DW()->CreateTextFormat(base.font.c_str(), nullptr, base.Weight(), base.i ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, base.st,
                                          base.size, L"", &fmt)))
            return;
        std::string_view jc = PA("jc", "val");
        fmt->SetTextAlignment(jc == "center" ? DWRITE_TEXT_ALIGNMENT_CENTER
                              : jc == "right" || jc == "end"                  ? DWRITE_TEXT_ALIGNMENT_TRAILING
                              : jc == "both" || jc == "distribute"            ? DWRITE_TEXT_ALIGNMENT_JUSTIFIED
                                                                              : DWRITE_TEXT_ALIGNMENT_LEADING);
        fmt->SetIncrementalTabStop(36 * kPt);  // Word's default tab stops: every half inch
        float tx = left;
        if (!bullet.empty() && !hanging) tx = left + 18 * kPt;
        if (FAILED(DW()->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt.Get(), std::max(1.f, width - tx - right), 1e6f, &it.text)))
            return;
        for (auto& r : runs) {
            DWRITE_TEXT_RANGE rg{r.start, r.len};
            it.text->SetFontFamilyName(r.f.font.c_str(), rg);
            it.text->SetFontSize(r.f.size, rg);
            it.text->SetFontWeight(r.f.Weight(), rg);
            it.text->SetFontStretch(r.f.st, rg);
            it.text->SetFontStyle(r.f.i ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, rg);
            it.text->SetUnderline(r.f.u || r.link, rg);
            it.text->SetStrikethrough(r.f.strike, rg);
            it.text->SetDrawingEffect(Brush(r.ink), rg);
        }
        for (auto& [pos, o] : objs) it.text->SetInlineObject(o.Get(), {pos, 1});
        // Line spacing: "auto" is a multiple of the font's own line height (inline objects included), exact is fixed.
        std::string_view rule = PA("spacing", "lineRule");
        float line = Num(PA("spacing", "line"), 240);
        ComPtr<IDWriteTextLayout3> l3;
        float maxSize = base.size;
        for (auto& r : runs) maxSize = std::max(maxSize, r.f.size);
        if (rule == "exact") it.text->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, line / 15, line / 15 * 0.8f);
        else if (d.pitch > 0 && objs.empty() && (rule.empty() || rule == "auto") && PA("snapToGrid", "val") != "0") {
            // the grid: a whole number of grid lines, text centred in them (a shortcut for Word's rule)
            float h = std::ceil(maxSize * 1.15f * line / 240 / d.pitch - 0.05f) * d.pitch;
            it.text->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, h, (h - 1.15f * maxSize) / 2 + 0.9f * maxSize);
        } else if (SUCCEEDED(it.text.As(&l3))) {
            float k = rule == "atLeast" ? 1 : line / 240;
            DWRITE_LINE_SPACING ls{DWRITE_LINE_SPACING_METHOD_PROPORTIONAL, k, k, 0, DWRITE_FONT_LINE_GAP_USAGE_DEFAULT};
            l3->SetLineSpacing(&ls);
        }
        DWRITE_TEXT_METRICS m{};
        it.text->GetMetrics(&m);
        for (auto& r : runs)
            if (r.f.mark) {
                UINT32 n = 0;
                it.text->HitTestTextRange(r.start, r.len, 0, 0, nullptr, 0, &n);
                std::vector<DWRITE_HIT_TEST_METRICS> hm(n);
                if (n && SUCCEEDED(it.text->HitTestTextRange(r.start, r.len, 0, 0, hm.data(), n, &n)))
                    for (auto& h : hm) it.marks.push_back({{h.left, h.top, h.left + h.width, h.top + h.height}, r.f.mark});
            }
        if (!bullet.empty()) {
            ComPtr<IDWriteTextFormat> bf;
            DW()->CreateTextFormat(base.font.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                   base.size, L"", &bf);
            if (bf) {
                bf->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                DW()->CreateTextLayout(bullet.c_str(), (UINT32)bullet.size(), bf.Get(), 1e4f, 1e4f, &it.bullet);
                if (it.bullet) it.bullet->SetDrawingEffect(Brush(Ink(base.color, bg)), {0, (UINT32)bullet.size()});
            }
            it.bx = hanging ? left - hanging : left;
        }
        // Shading and borders: a band from indent to indent, borders outside the text by their spacing.
        const Bd* b = it.bd;
        float padT = b[1].style ? b[1].space + b[1].w : 0, padB = b[3].style ? b[3].space + b[3].w : 0;
        it.band = {left - (b[0].style ? b[0].space + b[0].w : 0), before, width - right + (b[2].style ? b[2].space + b[2].w : 0),
                   before + padT + m.height + padB};
        it.tx = tx, it.ty = before + padT;
        it.h = it.band.bottom;
    }
};

// ---- tables
// Lays out rows until the table is `until` tall: a row's cells, then its height (the tallest single-row cell;
// a cell spanning rows adds what it still needs to its last row).
void TableRows(Word& d, Item& it, float until) {
    Table& t = *it.table;
    while (!t.Done() && t.rowY.back() < until) {
        size_t r = t.Laid();
        float h = t.rowMin[r];
        for (; t.next < t.cells.size() && t.cells[t.next].row == r; t.next++) {
            Cell& c = t.cells[t.next];
            std::vector<const Node*> nodes;
            Word::Flatten(*c.tc, nodes);
            LayoutAll(d, nodes, std::max(1.f, c.w - c.ml - c.mr), {c.fill ? c.fill : t.bg, t.ts}, c.items);
            for (auto& i : c.items) c.contentH = std::max(c.contentH, i.y + i.h + i.after);
            if (!c.items.empty()) c.contentH -= c.items.back().after;  // no space after the last paragraph
            c.contentH += c.mt + c.mb;
            if (c.span > 1) t.spans.push_back(t.next);
            else if (!t.exact[r]) h = std::max(h, c.contentH);
        }
        t.rowY.push_back(t.rowY.back() + h);
        std::erase_if(t.spans, [&](size_t i) {
            const Cell& c = t.cells[i];
            if (c.row + c.span - 1 > r && !t.Done()) return false;
            t.rowY.back() += std::max(0.f, c.contentH - (t.rowY.back() - t.rowY[c.row]));
            return true;
        });
    }
    it.h = t.rowY.back();
}

void LayoutTable(Word& d, const Node& tbl, float width, const Ctx& ctx, Item& it, float until) {
    auto t = std::make_unique<Table>();
    const Node* pr = tbl["tblPr"];
    const Node* ts = pr && (*pr)["tblStyle"] ? d.Style((*pr)["tblStyle"]->A("val")) : nullptr;
    if (!ts && d.styles)
        for (auto& s : d.styles->kids)
            if (s.name == "style" && s.A("type") == "table" && (s.A("default") == "1" || s.A("default") == "true")) ts = &s;
    auto tp = [&](std::string_view n) -> const Node* {  // table property: direct, else from the style chain
        if (const Node* x = pr ? (*pr)[n] : nullptr) return x;
        const Node* s = ts;
        for (int k = 0; s && k < 20; k++, s = (*s)["basedOn"] ? d.Style((*s)["basedOn"]->A("val")) : nullptr)
            if (const Node* x = At(s, {"tblPr", n})) return x;
        return nullptr;
    };
    std::vector<float> colX{0};
    if (const Node* g = tbl["tblGrid"])
        for (auto& c : g->kids)
            if (c.name == "gridCol") colX.push_back(colX.back() + Twips(c.A("w")));
    size_t most = 0;
    for (auto& tr : tbl.kids) {
        size_t n = 0;
        for (auto& tc : tr.kids) n += tc.name == "tc";
        most = std::max(most, n);
    }
    if (colX.size() < 2 || colX.back() <= 0) {
        colX = {0};
        for (size_t i = 0; i < std::max<size_t>(most, 1); i++) colX.push_back(width * (i + 1) / std::max<size_t>(most, 1));
    }
    size_t cols = colX.size() - 1;
    float ind = tp("tblInd") ? Twips(tp("tblInd")->A("w")) : 0;
    if (colX.back() + ind > width) {
        float k = std::max(0.f, width - std::max(ind, 0.f)) / colX.back();
        for (auto& c : colX) c *= k;
    }
    std::string_view jc = tp("jc") ? tp("jc")->A("val") : "";
    t->x = jc == "center" ? (width - colX.back()) / 2 : jc == "right" || jc == "end" ? width - colX.back() : ind;
    const Node* borders = tp("tblBorders");
    const Node* mar = tp("tblCellMar");
    auto margin = [&](const Node* m, std::initializer_list<std::string_view> names, float def) {
        for (auto n : names)
            if (const Node* x = m ? (*m)[n] : nullptr) return Twips(x->A("w"));
        return def;
    };
    float ml = margin(mar, {"left", "start"}, 108 / 15.f), mr = margin(mar, {"right", "end"}, 108 / 15.f);
    float mt = margin(mar, {"top"}, 0), mb = margin(mar, {"bottom"}, 0);

    std::vector<float>& rowMin = t->rowMin;
    std::vector<bool>& exact = t->exact;
    std::vector<int> owner(cols, -1);  // cell a vertical merge started in, per grid column
    size_t r = 0;
    for (auto& tr : tbl.kids) {
        if (tr.name != "tr") continue;
        const Node* trPr = tr["trPr"];
        const Node* th = trPr ? (*trPr)["trHeight"] : nullptr;
        rowMin.push_back(th ? Twips(th->A("val")) : 0), exact.push_back(th && th->A("hRule") == "exact");
        const Node* rowMar = At(&tr, {"tblPrEx", "tblCellMar"});  // a row's own cell margins
        float rl = margin(rowMar, {"left", "start"}, ml), rr = margin(rowMar, {"right", "end"}, mr);
        float rt = margin(rowMar, {"top"}, mt), rb = margin(rowMar, {"bottom"}, mb);
        size_t c = trPr && (*trPr)["gridBefore"] ? (size_t)(*trPr)["gridBefore"]->D("val", 0) : 0;
        for (auto& tc : tr.kids) {
            if (tc.name != "tc" || c >= cols) continue;
            const Node* tcPr = tc["tcPr"];
            size_t span = std::clamp<size_t>(tcPr && (*tcPr)["gridSpan"] ? (size_t)(*tcPr)["gridSpan"]->D("val", 1) : 1, 1, cols - c);
            const Node* vm = tcPr ? (*tcPr)["vMerge"] : nullptr;
            if (vm && vm->A("val") != "restart") {  // continues the cell above
                if (owner[c] >= 0) t->cells[owner[c]].span = r - t->cells[owner[c]].row + 1;
                c += span;
                continue;
            }
            const Node* tm = tcPr ? (*tcPr)["tcMar"] : nullptr;
            Cell cell{colX[c], colX[c + span] - colX[c], margin(tm, {"left", "start"}, rl), margin(tm, {"top"}, rt), 0, r};
            cell.tc = &tc, cell.mr = margin(tm, {"right", "end"}, rr), cell.mb = margin(tm, {"bottom"}, rb);
            if (const Node* shd = tcPr ? (*tcPr)["shd"] : nullptr)
                cell.fill = shd->A("val") == "solid" ? Hex(shd->A("color")) : Hex(shd->A("fill"));
            std::string_view va = tcPr && (*tcPr)["vAlign"] ? (*tcPr)["vAlign"]->A("val") : "";
            cell.valign = va == "center" ? 1 : va == "bottom" ? 2 : 0;
            // Borders: the cell's own, else the table's outer or inside lines (resolved below, once spans are known).
            static const char* own[4][2] = {{"left", "start"}, {"top", "top"}, {"right", "end"}, {"bottom", "bottom"}};
            const Node* tcb = tcPr ? (*tcPr)["tcBorders"] : nullptr;
            for (int s = 0; s < 4; s++) {
                const Node* b = tcb ? (*tcb)[own[s][0]] : nullptr;
                if (!b && tcb) b = (*tcb)[own[s][1]];
                cell.bd[s] = Border(b);
                if (b) cell.bd[s].space = -1;  // decided by the cell
            }
            for (size_t k = c; k < c + span; k++) owner[k] = vm ? (int)t->cells.size() : -1;
            t->cells.push_back(std::move(cell));
            c += span;
        }
        r++;
    }
    auto tb = [&](std::string_view n) { return Border(borders ? (*borders)[n] : nullptr); };
    for (auto& c : t->cells) {
        bool atL = c.x <= colX[0] + 0.01f, atR = c.x + c.w >= colX.back() - 0.01f, atT = c.row == 0, atB = c.row + c.span >= rowMin.size();
        Bd def[4] = {atL ? (borders && (*borders)["left"] ? tb("left") : tb("start")) : tb("insideV"), atT ? tb("top") : tb("insideH"),
                     atR ? (borders && (*borders)["right"] ? tb("right") : tb("end")) : tb("insideV"), atB ? tb("bottom") : tb("insideH")};
        for (int s = 0; s < 4; s++)
            if (c.bd[s].space != -1) c.bd[s] = def[s];
            else c.bd[s].space = 0;
    }
    t->bg = ctx.bg, t->ts = ts;
    it.after = 0;
    it.table = std::move(t);
    TableRows(d, it, until);
}

// A block (and the text boxes it holds) laid out at `y` into `out`; returns the y after it. A table stops
// once it reaches `until` (the rest: TableRows).
float LayoutBlock(Word& d, const Node& n, float width, const Ctx& ctx, std::vector<Item>& out, float y, float until = INFINITY) {
    Item& it = out.emplace_back();
    it.y = y;
    if (n.name == "tbl") LayoutTable(d, n, width, ctx, it, until - y);
    else Para{d, n, it, width, ctx}.Build(out.size() > 1 ? &out[out.size() - 2] : nullptr);
    y = it.y + it.h + it.after;
    for (const Node* b : std::vector<const Node*>(it.boxes)) {  // text box paragraphs
        Item& bi = out.emplace_back();
        bi.y = y;
        Para{d, *b, bi, width, ctx}.Build(nullptr);
        y = bi.y + bi.h + bi.after;
    }
    return y;
}

void LayoutAll(Word& d, const std::vector<const Node*>& nodes, float width, const Ctx& ctx, std::vector<Item>& out) {
    float y = 0;
    for (const Node* n : nodes) y = LayoutBlock(d, *n, width, ctx, out, y);
}

// ---- drawing
void Line(ID2D1RenderTarget* rt, D2D1_POINT_2F a, D2D1_POINT_2F b, const Bd& bd) {
    if (!bd.style) return;
    if (!g_dash) {
        ComPtr<ID2D1Factory> f;
        rt->GetFactory(&f);
        f->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                         D2D1_LINE_JOIN_MITER, 10, D2D1_DASH_STYLE_DASH), nullptr, 0, &g_dash);
        f->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                         D2D1_LINE_JOIN_MITER, 10, D2D1_DASH_STYLE_DOT), nullptr, 0, &g_dot);
    }
    float w = std::max(bd.w, 96 / g_dpi);  // at least a pixel
    rt->DrawLine(a, b, Brush(bd.color ? bd.color : kAuto), w, bd.style == 2 ? g_dash.Get() : bd.style == 3 ? g_dot.Get() : nullptr);
}

void DrawItems(ID2D1RenderTarget* rt, std::vector<Item>& items, float x, float y, float top, float bottom);

void DrawItem(ID2D1RenderTarget* rt, Item& it, float x, float y, float top, float bottom) {
    if (it.table) {
        Table& t = *it.table;
        x += t.x;
        auto shown = [&](const Cell& c) {  // laid out and on screen
            return c.row < t.Laid() && y + t.rowY[c.row] <= bottom && y + t.rowY[std::min(c.row + c.span, t.Laid())] >= top;
        };
        for (auto& c : t.cells) {
            if (!shown(c)) continue;
            D2D1_RECT_F r{x + c.x, y + t.rowY[c.row], x + c.x + c.w, y + t.rowY[std::min(c.row + c.span, t.Laid())]};
            if (c.fill) rt->FillRectangle(r, Brush(c.fill));
            float off = c.valign ? std::max(0.f, (r.bottom - r.top) - c.contentH) / (c.valign == 1 ? 2 : 1) : 0;
            DrawItems(rt, c.items, r.left + c.ml, r.top + c.mt + off, top, bottom);
        }
        for (auto& c : t.cells) {
            if (!shown(c)) continue;
            float l = x + c.x, r = l + c.w, tp = y + t.rowY[c.row], b = y + t.rowY[std::min(c.row + c.span, t.Laid())];
            Line(rt, {l, tp}, {l, b}, c.bd[0]), Line(rt, {l, tp}, {r, tp}, c.bd[1]), Line(rt, {r, tp}, {r, b}, c.bd[2]),
                Line(rt, {l, b}, {r, b}, c.bd[3]);
        }
        return;
    }
    D2D1_RECT_F band{x + it.band.left, y + it.band.top, x + it.band.right, y + it.band.bottom};
    if (it.fill) rt->FillRectangle(band, Brush(it.fill));
    Line(rt, {band.left, band.top}, {band.left, band.bottom}, it.bd[0]), Line(rt, {band.left, band.top}, {band.right, band.top}, it.bd[1]);
    Line(rt, {band.right, band.top}, {band.right, band.bottom}, it.bd[2]), Line(rt, {band.left, band.bottom}, {band.right, band.bottom}, it.bd[3]);
    float tx = x + it.tx, ty = y + it.ty;
    for (auto& [r, c] : it.marks) rt->FillRectangle({tx + r.left, ty + r.top, tx + r.right, ty + r.bottom}, Brush(c));
    if (it.text) rt->DrawTextLayout({tx, ty}, it.text.Get(), Brush(kAuto), D2D1_DRAW_TEXT_OPTIONS_NONE);
    if (it.bullet) rt->DrawTextLayout({x + it.bx, ty}, it.bullet.Get(), Brush(kAuto), D2D1_DRAW_TEXT_OPTIONS_NONE);
}

void DrawItems(ID2D1RenderTarget* rt, std::vector<Item>& items, float x, float y, float top, float bottom) {
    for (auto& it : items) {
        float iy = y + it.y;
        if (iy > bottom) break;
        if (iy + it.h >= top) DrawItem(rt, it, x, iy, top, bottom);
    }
}

struct PicJob {
    std::wstring path;
    int gen;
    std::vector<std::tuple<std::wstring, UINT, UINT>> want;
};
struct PicMsg {
    std::wstring media;
    ComPtr<IWICBitmap> bmp;
    UINT px;
};
DWORD WINAPI PicThread(void* arg) {
    std::unique_ptr<PicJob> job(static_cast<PicJob*>(arg));
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Members m;
    ArchiveReadMembers(job->path, [&](std::wstring_view n) -> size_t {
        for (auto& w : job->want)
            if (std::get<0>(w) == n) return 64 << 20;
        return 0;
    }, m);
    for (auto& [name, w, h] : job->want) {
        if (g_gen != job->gen) break;
        auto* msg = new PicMsg{name, DecodePicture(Member(m, name), w, h), w};
        if (!PostMessageW(g_hwnd, WM_FLICK_WORD, job->gen, (LPARAM)msg)) delete msg;
    }
    CoUninitialize();
    return 0;
}

// Body text width at 100%: the last section's page width minus its margins.
float Column(std::string_view xml) {
    float w = 612 * kPt, l = 72 * kPt, r = 72 * kPt;
    for (auto [tag, which] : {std::pair{"<w:pgSz ", 0}, std::pair{"<w:pgMar ", 1}}) {
        size_t k = xml.rfind(tag);
        if (k == xml.npos) continue;
        Xml x{xml, k};
        if (x.Next() != Xml::Tag) continue;
        if (which == 0) w = Twips(Attr(x.attrs, "w"), w);
        else l = Twips(Attr(x.attrs, "left"), l), r = Twips(Attr(x.attrs, "right"), r);
    }
    return std::max(200.f, w - l - r);
}

bool Open(Word& d) {
    for (Xml x{d.xml}; int t = x.Next();)
        if (t == Xml::Tag && !x.close && x.name == "body") {
            d.scan = x, d.bodyStart = x.i;
            break;
        }
    if (!d.bodyStart) return false;
    size_t end = d.xml.rfind("</w:body>");
    d.bodyEnd = std::max(d.bodyStart + 1, end == d.xml.npos ? d.xml.size() : end);
    d.column = Column(d.xml.substr(d.bodyStart));
    if (size_t k = d.xml.rfind("<w:docGrid "); k != d.xml.npos) {
        Xml x{d.xml, k};
        x.Next();
        if (Attr(x.attrs, "type").starts_with("lines")) d.pitch = Twips(Attr(x.attrs, "linePitch"));
    }
    if ((d.styles = Top(d.stylesRoot)))
        for (auto& s : d.styles->kids)
            if (s.name == "style") {
                d.byId[s.A("styleId")] = &s;
                if (s.A("type") == "paragraph" && (s.A("default") == "1" || s.A("default") == "true")) d.defPara = &s;
            }
    d.numbering = Top(d.numRoot);
    return true;
}

}  // namespace

bool WordLoad(const std::wstring& path, std::wstring& sub) {
    WordClear();
    auto d = std::make_unique<Word>();
    d->path = path;
    if (!ArchiveReadMembers(path, [](std::wstring_view n) -> size_t {
            return n == L"word/document.xml" || n == L"word/styles.xml" || n == L"word/numbering.xml" ||
                           n == L"word/_rels/document.xml.rels" || n == L"word/theme/theme1.xml" || n == L"docProps/app.xml"
                       ? kMaxXml : 0;
        }, d->m))
        return false;
    d->xml = Member(d->m, L"word/document.xml");
    d->stylesRoot = Parse(Member(d->m, L"word/styles.xml"));
    d->numRoot = Parse(Member(d->m, L"word/numbering.xml"));
    d->rels = Rels(Member(d->m, L"word/_rels/document.xml.rels"), L"word/");
    if (!Open(*d)) return false;
    std::string_view theme = Member(d->m, L"word/theme/theme1.xml");
    for (auto [tag, font] : {std::pair{"<a:majorFont>", &d->major}, std::pair{"<a:minorFont>", &d->minor}})
        if (size_t k = theme.find(tag); k != theme.npos)
            for (Xml x{theme, k}; int t = x.Next();)
                if (t == Xml::Tag && x.name == "latin") {
                    if (!Attr(x.attrs, "typeface").empty()) *font = Wide(Attr(x.attrs, "typeface"));
                    break;
                }
    std::string_view app = Member(d->m, L"docProps/app.xml");
    if (size_t k = app.find("<Words>"); k != app.npos)  // Word's saved page count has no meaning in a view without pages
        if (int n = atoi(std::string(app.substr(k + 7, 10)).c_str()); n > 0) sub = std::to_wstring(n) + (n == 1 ? L" word" : L" words");
    g_w = std::move(d);
    return true;
}

void WordClear() {
    g_gen++;
    g_items.clear(), g_next = 0, g_width = -1, g_laidH = 0;
    g_pics.clear(), g_want.clear();
    g_brush.clear(), g_brushRt.Reset();
    g_w.reset();
}

float WordColumn() { return g_w ? g_w->column : 624; }

float WordPaint(ID2D1RenderTarget* rt, const D2D1_RECT_F& area, float scroll, const MdColors& c) {
    if (!g_w) return 0;
    Word& d = *g_w;
    g_rt = rt;
    float dpiY;
    rt->GetDpi(&g_dpi, &dpiY);
    if (rt != g_brushRt.Get()) g_brush.clear(), g_brushRt = rt, g_width = -1;  // layouts hold brushes of one target
    bool dark = Lum(((UINT32)(c.text.r * 255) << 16) | ((UINT32)(c.text.g * 255) << 8) | (UINT32)(c.text.b * 255)) > 0.5f;
    if (dark != g_dark) g_dark = dark, g_width = -1;
    Brush(kAuto)->SetColor(c.text), Brush(kLink)->SetColor(c.link);
    float width = area.right - area.left - 2 * kMargin, viewH = area.bottom - area.top;
    if (width != g_width) g_width = width, g_items.clear(), g_next = 0, g_laidH = kMargin, d.counters.clear(), d.notes = 0;
    float until = scroll + viewH + 400;
    for (;;) {
        Item* last = g_items.empty() ? nullptr : &g_items.back();
        if (last && last->table && !last->table->Done()) {  // a table laid out part way: more rows first
            if (g_laidH >= until) break;
            TableRows(d, *last, until - last->y);
            g_laidH = last->y + last->h + last->after;
        } else if (g_laidH < until && d.Block(g_next)) g_laidH = LayoutBlock(d, *d.blocks[g_next++], width, {}, g_items, g_laidH, until);
        else break;
    }
    g_want.clear();
    g_origin = area.top - scroll, g_top = area.top, g_bottom = area.bottom, g_paint++;
    DrawItems(rt, g_items, area.left + kMargin, g_origin, area.top, area.bottom);
    for (auto& [name, p] : g_pics)  // pictures more than 3 screens away give their pixels back (decoded again on return)
        if (p.bmp && p.seen != g_paint && std::abs(p.y - (scroll + viewH / 2)) > 3 * viewH) p.bmp.Reset(), p.px = 0;
    if (!g_want.empty())
        CloseHandle(CreateThread(nullptr, 0, PicThread, new PicJob{d.path, g_gen, std::move(g_want)}, 0, nullptr)), g_want.clear();
    const Table* open = g_items.empty() || !g_items.back().table || g_items.back().table->Done() ? nullptr : g_items.back().table.get();
    bool all = d.done && g_next >= d.blocks.size() && !open;
    float h = g_laidH + kMargin;
    if (open) h += open->rowY.back() / std::max<size_t>(1, open->Laid()) * (open->rowMin.size() - open->Laid());  // the rows to come, at the average so far
    return all ? h : std::max(h, h / d.Parsed());  // estimate until the end is laid out
}

bool WordPicture(WPARAM gen, LPARAM lp) {
    std::unique_ptr<PicMsg> msg(reinterpret_cast<PicMsg*>(lp));
    if ((int)gen != g_gen || !g_w) return false;
    Pic& p = g_pics[msg->media];
    p.pending = false;
    if (msg->bmp) p.bmp = SharedBitmap(msg->bmp.Get()), p.px = msg->px;
    else p.failed = true;
    return true;
}

// --selftest: style inheritance (basedOn, docDefaults) and the math box tree.
std::wstring WordSelfTest() {
    Word d;
    d.stylesRoot = Parse(R"(<w:styles><w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:ascii="Times New Roman"/><w:sz w:val="20"/></w:rPr></w:rPrDefault></w:docDefaults>
<w:style w:type="paragraph" w:default="1" w:styleId="N"><w:rPr><w:sz w:val="24"/></w:rPr></w:style>
<w:style w:type="paragraph" w:styleId="H"><w:basedOn w:val="N"/><w:rPr><w:b/><w:rFonts w:hint="eastAsia"/></w:rPr></w:style></w:styles>)");
    std::string doc = R"(<w:document><w:body><w:p><w:pPr><w:pStyle w:val="H"/></w:pPr><w:r><w:rPr><w:i/></w:rPr><w:t>x</w:t></w:r></w:p></w:body></w:document>)";
    d.xml = doc;
    if (!Open(d) || !d.Block(0) || d.Block(1)) return L"word: body";
    Item it;
    Para p{d, *d.blocks[0], it, 500, {}};
    const Node* pPr = (*d.blocks[0])["pPr"];
    d.Chain(d.Style((*pPr)["pStyle"]->A("val")), "rPr", p.pr);
    p.pr.push_back(At(d.styles, {"docDefaults", "rPrDefault", "rPr"}));
    RunFmt f = p.Props((*(*d.blocks[0])["r"])["rPr"]);
    if (f.size != 12 * kPt || !f.b || !f.i || f.font != L"Times New Roman") return L"word: style inheritance";
    Node m = Parse(R"(<m:oMath><m:f><m:num><m:r><m:t>1</m:t></m:r></m:num><m:den><m:sSub><m:e><m:r><m:t>x</m:t></m:r></m:e><m:sub><m:r><m:t>i</m:t></m:r></m:sub></m:sSub></m:den></m:f><m:r><m:t>=y</m:t></m:r><m:d><m:e><m:rad><m:radPr><m:degHide m:val="1"/></m:radPr><m:deg/><m:e><m:r><m:t>a</m:t></m:r></m:e></m:rad></m:e></m:d><m:foo><m:r><m:t>z</m:t></m:r></m:foo></m:oMath>)");
    Math mt{L"Calibri", 6};
    std::wstring o = mt.Row(Top(m), 16, true).Outline();
    if (o != L"e(f(e(t:1,),e(s(e(t:x,),e(t:i,),),),),t:=y,D(d:(,e(e(r(e(t:a,),),),),d:),),e(t:z,),)") return L"word: math " + o;
    return L"";
}
