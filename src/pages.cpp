// pptx slides and docx pages drawn for the PDF view. Slides: shapes, text, pictures, tables and backgrounds, with what a
// slide inherits from its layout, master and theme (positions, text styles, colours, fonts). Opened on the PDF
// worker (zip + XML, a few ms); each page is drawn with Direct2D + DirectWrite into a WIC bitmap at the width
// the view asks for, so slides and their thumbnails take the same path as PDF pages.
// shortcut: no charts, SmartArt, effects (shadows, glow), vertical text, EMF/WMF pictures or group rotation;
// uncommon preset shapes are drawn as rectangles and every table gets the default table style's look.
#include "app.h"
#include "xml.h"
#include <shlwapi.h>
#include <cmath>
#include <map>
#include <tuple>
#include <unordered_set>

using Microsoft::WRL::ComPtr;
using D2D1::Matrix3x2F;

namespace {

// ---- a small DOM over the tag scanner (string_views into the zip member's text)
struct Node {
    std::string_view name, attrs;
    std::string text;  // character data of <a:t>
    std::vector<Node> kids;
    const Node* operator[](std::string_view n) const {
        for (auto& k : kids)
            if (k.name == n) return &k;
        return nullptr;
    }
    std::string_view A(std::string_view a) const { return Attr(attrs, a); }
    double D(std::string_view a, double def) const {
        std::string_view v = A(a);
        return v.empty() ? def : atof(std::string(v).c_str());
    }
};
const Node* At(const Node* n, std::initializer_list<std::string_view> path) {
    for (auto p : path)
        if (!n || !(n = (*n)[p])) return nullptr;
    return n;
}
const Node* Top(const Node& root) { return root.kids.empty() ? nullptr : &root.kids[0]; }

Node Parse(std::string_view xml) {
    Node root;
    std::vector<Node*> open{&root};  // parents only grow once their open child has closed: pointers stay valid
    for (Xml x{xml}; int t = x.Next();) {
        if (t == Xml::Text) {
            if (open.back()->name == "t") x.cdata ? (void)open.back()->text.append(x.text) : AppendXml(open.back()->text, x.text);
        } else if (x.close) {
            if (open.size() > 1) open.pop_back();
        } else {
            open.back()->kids.push_back({x.name, x.attrs});
            if (!x.empty) open.push_back(&open.back()->kids.back());
        }
    }
    return root;
}

struct Part {
    Node root;
    std::unordered_map<std::string, std::wstring> rels;
    bool loaded = false;
};

// ---- colours
constexpr std::string_view kScheme[] = {"dk1", "lt1", "dk2", "lt2", "accent1", "accent2", "accent3",
                                        "accent4", "accent5", "accent6", "hlink", "folHlink"};
D2D1_COLOR_F Rgb(std::string_view hex) {
    unsigned v = (unsigned)strtoul(std::string(hex).c_str(), nullptr, 16);
    return {(v >> 16 & 255) / 255.f, (v >> 8 & 255) / 255.f, (v & 255) / 255.f, 1};
}
void ToHsl(const D2D1_COLOR_F& c, float& h, float& s, float& l) {
    float mx = std::max({c.r, c.g, c.b}), mn = std::min({c.r, c.g, c.b}), d = mx - mn;
    l = (mx + mn) / 2, h = s = 0;
    if (d <= 0) return;
    s = l > 0.5f ? d / (2 - mx - mn) : d / (mx + mn);
    h = mx == c.r ? (c.g - c.b) / d + (c.g < c.b ? 6 : 0) : mx == c.g ? (c.b - c.r) / d + 2 : (c.r - c.g) / d + 4;
    h /= 6;
}
float Hue(float p, float q, float t) {
    t = t < 0 ? t + 1 : t > 1 ? t - 1 : t;
    return t < 1 / 6.f ? p + (q - p) * 6 * t : t < 0.5f ? q : t < 2 / 3.f ? p + (q - p) * (2 / 3.f - t) * 6 : p;
}
void FromHsl(D2D1_COLOR_F& c, float h, float s, float l) {
    if (s <= 0) {
        c.r = c.g = c.b = l;
        return;
    }
    float q = l < 0.5f ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
    c.r = Hue(p, q, h + 1 / 3.f), c.g = Hue(p, q, h), c.b = Hue(p, q, h - 1 / 3.f);
}
// Colour transforms inside a colour element (lumMod/lumOff are what theme tints and shades use).
void Modify(D2D1_COLOR_F& c, const Node& n) {
    for (auto& m : n.kids) {
        float v = (float)m.D("val", 100000) / 100000;
        if (m.name == "alpha") c.a = v;
        else if (m.name == "shade") c.r *= v, c.g *= v, c.b *= v;
        else if (m.name == "tint") c.r = 1 - (1 - c.r) * v, c.g = 1 - (1 - c.g) * v, c.b = 1 - (1 - c.b) * v;
        else if (m.name == "lumMod" || m.name == "lumOff" || m.name == "satMod") {
            float h, s, l;
            ToHsl(c, h, s, l);
            if (m.name == "lumMod") l *= v;
            else if (m.name == "lumOff") l += v;
            else s *= v;
            FromHsl(c, h, std::clamp(s, 0.f, 1.f), std::clamp(l, 0.f, 1.f));
        }
    }
}

struct Theme {
    D2D1_COLOR_F c[12];
    std::wstring major = L"Calibri Light", minor = L"Calibri";
    Theme() {
        const char* def[] = {"000000", "FFFFFF", "44546A", "E7E6E6", "4472C4", "ED7D31",
                             "A5A5A5", "FFC000", "5B9BD5", "70AD47", "0563C1", "954F72"};
        for (int i = 0; i < 12; i++) c[i] = Rgb(def[i]);
    }
};

IDWriteFactory* DW() {
    static ComPtr<IDWriteFactory> f;
    if (!f) DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)f.GetAddressOf());
    return f.Get();
}

float Num(std::string_view v, double def) { return (float)(v.empty() ? def : atof(std::string(v).c_str())); }
std::string_view PhType(std::string_view t) {  // placeholder kinds that share a master placeholder
    return t.empty() || t == "obj" || t == "subTitle" ? "body" : t == "ctrTitle" ? "title" : t;
}
const Node* PhOf(const Node& sp) {
    for (auto& k : sp.kids)
        if (k.name.starts_with("nv")) return At(&k, {"nvPr", "ph"});
    return nullptr;
}
bool Hidden(const Node& sp) {
    for (auto& k : sp.kids)
        if (k.name.starts_with("nv")) return At(&k, {"cNvPr"}) && At(&k, {"cNvPr"})->A("hidden") == "1";
    return false;
}
const Node* XfrmOf(const Node* sp) {
    if (!sp) return nullptr;
    if (auto x = At(sp, {"spPr", "xfrm"})) return x;
    if (auto x = At(sp, {"grpSpPr", "xfrm"})) return x;
    return (*sp)["xfrm"];  // graphicFrame
}

// Group coordinates: EMU in the group's child space -> points on the slide.
struct Xf {
    float sx = 1 / 12700.f, sy = 1 / 12700.f, tx = 0, ty = 0;
    D2D1_RECT_F Rect(const Node& xfrm) const {
        const Node *off = xfrm["off"], *ext = xfrm["ext"];
        float x = off ? (float)off->D("x", 0) : 0, y = off ? (float)off->D("y", 0) : 0;
        float w = ext ? (float)ext->D("cx", 0) : 0, h = ext ? (float)ext->D("cy", 0) : 0;
        return {x * sx + tx, y * sy + ty, (x + w) * sx + tx, (y + h) * sy + ty};
    }
};

struct PageItem {  // docx: something laid out on a page (points)
    ComPtr<IDWriteTextLayout> layout, bullet;  // text: drawn at origin, clipped to `clip` (a paragraph split over pages)
    std::vector<std::pair<DWRITE_TEXT_RANGE, D2D1_COLOR_F>> colors;
    D2D1_POINT_2F origin{}, bulletOrigin{};
    D2D1_RECT_F clip{};
    D2D1_COLOR_F bulletColor{0, 0, 0, 1};  // also the fill colour
    std::wstring media;                    // picture: zip path, drawn into `clip`
    bool fill = false, frame = false;      // table cell shading / border
};

struct TextBox {  // what a text body inherits, nearest first
    std::vector<const Node*> lst;  // lstStyle-like nodes holding lvlNpPr
    float ins[4] = {7.2f, 3.6f, 7.2f, 3.6f};  // left top right bottom, points
    std::string_view anchor;
    bool nowrap = false, bold = false;
    float fontScale = 1, spcRed = 0;
    D2D1_COLOR_F color{0, 0, 0, 1};
};

}  // namespace

struct Pages {
    std::wstring path;
    Members m;  // owns the XML text the DOM points into: never changed after opening
    std::unordered_map<std::wstring, Part> parts;
    std::vector<std::wstring> slides;
    float w = 720, h = 405;  // points
    const Node* defaults = nullptr;  // presentation.xml defaultTextStyle
    bool word = false;                 // docx: pages laid out in `doc`
    std::vector<std::vector<PageItem>> doc;

    Part& Get(const std::wstring& name) {
        Part& p = parts[name];
        if (!p.loaded) {
            p.loaded = true;
            p.root = Parse(Member(m, name));
            size_t slash = name.rfind(L'/');
            std::wstring dir = name.substr(0, slash + 1);
            p.rels = Rels(Member(m, dir + L"_rels/" + name.substr(slash + 1) + L".rels"), dir);
        }
        return p;
    }
    Part* Related(Part& p, std::wstring_view kind) {
        for (auto& [id, t] : p.rels)
            if (t.find(kind) != t.npos) return &Get(t);
        return nullptr;
    }
};

namespace {

struct Draw {
    Pages& d;
    ID2D1RenderTarget* rt;
    IWICImagingFactory* wic;
    float scale;  // pixels per point
    Matrix3x2F base;
    Part *slide, *layout = nullptr, *master = nullptr;
    Theme theme;
    const Node* clrMap = nullptr;
    const Node* txStyles = nullptr;
    std::vector<std::pair<std::wstring, std::string>> media;
    D2D1_COLOR_F ph{0, 0, 0, 1};  // phClr: the colour a style reference passes in

    const Node* TopOf(Part* p) { return p ? Top(p->root) : nullptr; }

    bool Color(const Node* n, D2D1_COLOR_F& out) {
        if (!n) return false;
        for (auto& k : n->kids) {
            if (k.name == "srgbClr") out = Rgb(k.A("val"));
            else if (k.name == "sysClr") out = Rgb(!k.A("lastClr").empty() ? k.A("lastClr") : k.A("val") == "window" ? "FFFFFF" : "000000");
            else if (k.name == "prstClr") out = Rgb(k.A("val") == "white" ? "FFFFFF" : "000000");
            else if (k.name == "scrgbClr") out = {Num(k.A("r"), 0) / 100000, Num(k.A("g"), 0) / 100000, Num(k.A("b"), 0) / 100000, 1};
            else if (k.name == "schemeClr") {
                std::string_view v = k.A("val");
                if (v == "phClr") out = ph;
                else {
                    std::string_view mapped = clrMap ? clrMap->A(v) : std::string_view{};
                    if (!mapped.empty()) v = mapped;
                    else if (v == "bg1") v = "lt1";
                    else if (v == "tx1") v = "dk1";
                    else if (v == "bg2") v = "lt2";
                    else if (v == "tx2") v = "dk2";
                    out = theme.c[0];
                    for (int i = 0; i < 12; i++)
                        if (kScheme[i] == v) out = theme.c[i];
                }
            } else continue;
            Modify(out, k);
            return true;
        }
        return false;
    }
    D2D1_COLOR_F Scheme(std::string_view name) {
        for (int i = 0; i < 12; i++)
            if (kScheme[i] == name) return theme.c[i];
        return theme.c[0];
    }
    std::wstring Font(std::string_view face) {
        if (face.empty() || face.starts_with("+mn")) return theme.minor;
        if (face.starts_with("+mj")) return theme.major;
        return Wide(face);
    }

    // ---- pictures
    ComPtr<ID2D1Bitmap> Picture(const Node* blip, Part& part, UINT w, UINT h) {
        if (!blip) return nullptr;
        auto rel = part.rels.find(std::string(blip->A("r:embed")));
        if (rel == part.rels.end()) return nullptr;
        const std::string* bytes = nullptr;
        for (auto& [n, b] : media)
            if (n == rel->second) bytes = &b;
        ComPtr<IStream> s;
        ComPtr<IWICBitmapDecoder> dec;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICBitmapScaler> scaler;
        ComPtr<IWICFormatConverter> conv;
        ComPtr<ID2D1Bitmap> bmp;
        if (!bytes || bytes->empty()) return nullptr;
        s.Attach(SHCreateMemStream((const BYTE*)bytes->data(), (UINT)bytes->size()));
        if (!s || FAILED(wic->CreateDecoderFromStream(s.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec)) ||
            FAILED(dec->GetFrame(0, &frame)))
            return nullptr;
        IWICBitmapSource* src = frame.Get();
        UINT fw = 0, fh = 0;
        frame->GetSize(&fw, &fh);
        if (w && h && fw > w && fh > h && SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
            SUCCEEDED(scaler->Initialize(frame.Get(), w, h, WICBitmapInterpolationModeFant)))
            src = scaler.Get();  // decode to the size drawn: big photos stay cheap
        if (FAILED(wic->CreateFormatConverter(&conv)) ||
            FAILED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) ||
            FAILED(rt->CreateBitmapFromWicBitmap(conv.Get(), &bmp)))
            return nullptr;
        return bmp;
    }
    // A blipFill into `r`, cropped by srcRect, clipped to the shape when it isn't a rectangle.
    void DrawBlip(const Node& blipFill, Part& part, D2D1_RECT_F r, ID2D1Geometry* clip) {
        const Node* crop = blipFill["srcRect"];
        UINT w = (UINT)std::lround((r.right - r.left) * scale), h = (UINT)std::lround((r.bottom - r.top) * scale);
        auto bmp = Picture(blipFill["blip"], part, crop ? 0 : w, crop ? 0 : h);
        if (!bmp) return;
        D2D1_SIZE_F sz = bmp->GetSize();
        D2D1_RECT_F src{0, 0, sz.width, sz.height};
        if (crop)
            src = {sz.width * Num(crop->A("l"), 0) / 100000, sz.height * Num(crop->A("t"), 0) / 100000,
                   sz.width * (1 - Num(crop->A("r"), 0) / 100000), sz.height * (1 - Num(crop->A("b"), 0) / 100000)};
        if (clip) rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip), nullptr);
        rt->DrawBitmap(bmp.Get(), r, 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, src);
        if (clip) rt->PopLayer();
    }

    // ---- fills and lines. True when `props` decides the fill (noFill included).
    bool Fill(const Node* props, Part& part, ID2D1Geometry* g, D2D1_RECT_F r) {
        if (!props) return false;
        for (auto& k : props->kids) {
            ComPtr<ID2D1SolidColorBrush> solid;
            D2D1_COLOR_F c{};
            if (k.name == "noFill") return true;
            if (k.name == "solidFill" || k.name == "pattFill") {
                if (k.name == "pattFill" ? Color(k["fgClr"], c) : Color(&k, c))
                    rt->CreateSolidColorBrush(c, &solid), rt->FillGeometry(g, solid.Get());
                return true;
            }
            if (k.name == "gradFill") {
                std::vector<D2D1_GRADIENT_STOP> stops;
                if (auto gs = k["gsLst"])
                    for (auto& s : gs->kids)
                        if (Color(&s, c)) stops.push_back({Num(s.A("pos"), 0) / 100000, c});
                if (stops.empty()) return true;
                std::sort(stops.begin(), stops.end(), [](auto& a, auto& b) { return a.position < b.position; });
                float ang = (k["lin"] ? Num(k["lin"]->A("ang"), 0) : 5400000) / 60000 * 3.14159265f / 180;
                D2D1_POINT_2F mid{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
                float dx = std::cos(ang) * (r.right - r.left) / 2, dy = std::sin(ang) * (r.bottom - r.top) / 2;
                ComPtr<ID2D1GradientStopCollection> coll;
                ComPtr<ID2D1LinearGradientBrush> brush;
                if (SUCCEEDED(rt->CreateGradientStopCollection(stops.data(), (UINT32)stops.size(), &coll)) &&
                    SUCCEEDED(rt->CreateLinearGradientBrush({{mid.x - dx, mid.y - dy}, {mid.x + dx, mid.y + dy}}, coll.Get(), &brush)))
                    rt->FillGeometry(g, brush.Get());
                return true;
            }
            if (k.name == "blipFill") {
                ComPtr<ID2D1RectangleGeometry> rg;
                DrawBlip(k, part, r, SUCCEEDED(g->QueryInterface(IID_PPV_ARGS(&rg))) ? nullptr : g);
                return true;
            }
            if (k.name == "grpFill") return true;  // shortcut: group fills not inherited
        }
        return false;
    }
    // Outline: <a:ln> of the shape, or the theme line its style refers to.
    void Line(const Node* ln, const Node* style, ID2D1Geometry* g) {
        D2D1_COLOR_F c{};
        float w = 0.75f;
        bool have = false;
        if (ln) {
            if ((*ln)["noFill"]) return;
            w = Num(ln->A("w"), 9525) / 12700;
            if (auto f = (*ln)["solidFill"]) have = Color(f, c);
            else if (auto gf = At(ln, {"gradFill", "gsLst"}); gf && !gf->kids.empty()) have = Color(&gf->kids[0], c);
        }
        if (!have && style)
            if (auto ref = (*style)["lnRef"]; ref && Num(ref->A("idx"), 0) > 0) have = Color(ref, c);
        if (!have) return;
        ComPtr<ID2D1SolidColorBrush> b;
        rt->CreateSolidColorBrush(c, &b);
        rt->DrawGeometry(g, b.Get(), std::max(w, 0.5f / scale));  // hairlines: half a pixel at least
    }

    // ---- geometry: preset shapes (common ones) and custom paths, in slide points
    ComPtr<ID2D1Geometry> Poly(std::initializer_list<D2D1_POINT_2F> pts) {
        ComPtr<ID2D1PathGeometry> path;
        ComPtr<ID2D1GeometrySink> sink;
        ComPtr<ID2D1Factory> f;
        rt->GetFactory(&f);
        f->CreatePathGeometry(&path);
        path->Open(&sink);
        auto it = pts.begin();
        sink->BeginFigure(*it, D2D1_FIGURE_BEGIN_FILLED);
        for (++it; it != pts.end(); ++it) sink->AddLine(*it);
        sink->EndFigure(pts.size() > 2 ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
        sink->Close();
        return path;
    }
    ComPtr<ID2D1Geometry> Geometry(const Node* spPr, D2D1_RECT_F r, const Node* xfrm) {
        ComPtr<ID2D1Factory> f;
        rt->GetFactory(&f);
        float w = r.right - r.left, h = r.bottom - r.top, m = std::min(w, h), mx = (r.left + r.right) / 2, my = (r.top + r.bottom) / 2;
        const Node* prst = spPr ? (*spPr)["prstGeom"] : nullptr;
        std::string_view kind = prst ? prst->A("prst") : "rect";
        auto adj = [&](std::string_view name, float def) {
            if (const Node* av = prst ? (*prst)["avLst"] : nullptr)
                for (auto& g : av->kids)
                    if (g.A("name") == name) {
                        std::string_view v = g.A("fmla");
                        if (v.starts_with("val ")) return Num(v.substr(4), def) / 100000;
                    }
            return def / 100000;
        };
        if (const Node* cust = spPr ? (*spPr)["custGeom"] : nullptr) return Custom(cust, r, xfrm);
        ComPtr<ID2D1Geometry> g;
        if (kind == "ellipse") {
            ComPtr<ID2D1EllipseGeometry> e;
            f->CreateEllipseGeometry({{mx, my}, w / 2, h / 2}, &e);
            g = e;
        } else if (kind == "roundRect") {
            ComPtr<ID2D1RoundedRectangleGeometry> rr;
            float rad = m * adj("adj", 16667);
            f->CreateRoundedRectangleGeometry({r, rad, rad}, &rr);
            g = rr;
        } else if (kind == "line" || kind.find("Connector") != kind.npos) g = Poly({{r.left, r.top}, {r.right, r.bottom}});
        else if (kind == "triangle") g = Poly({{r.left + w * adj("adj", 50000), r.top}, {r.right, r.bottom}, {r.left, r.bottom}});
        else if (kind == "rtTriangle") g = Poly({{r.left, r.top}, {r.right, r.bottom}, {r.left, r.bottom}});
        else if (kind == "diamond") g = Poly({{mx, r.top}, {r.right, my}, {mx, r.bottom}, {r.left, my}});
        else if (kind == "parallelogram") {
            float x = m * adj("adj", 25000);
            g = Poly({{r.left + x, r.top}, {r.right, r.top}, {r.right - x, r.bottom}, {r.left, r.bottom}});
        } else if (kind == "homePlate" || kind == "pentagon") {
            float x = r.right - m * adj("adj", 50000);
            g = Poly({{r.left, r.top}, {x, r.top}, {r.right, my}, {x, r.bottom}, {r.left, r.bottom}});
        } else if (kind == "chevron") {
            float x = m * adj("adj", 50000);
            g = Poly({{r.left, r.top}, {r.right - x, r.top}, {r.right, my}, {r.right - x, r.bottom}, {r.left, r.bottom}, {r.left + x, my}});
        } else if (kind == "rightArrow") {
            float t = h * adj("adj1", 50000) / 2, x = r.right - m * adj("adj2", 50000);
            g = Poly({{r.left, my - t}, {x, my - t}, {x, r.top}, {r.right, my}, {x, r.bottom}, {x, my + t}, {r.left, my + t}});
        } else if (kind == "leftArrow") {
            float t = h * adj("adj1", 50000) / 2, x = r.left + m * adj("adj2", 50000);
            g = Poly({{r.right, my - t}, {x, my - t}, {x, r.top}, {r.left, my}, {x, r.bottom}, {x, my + t}, {r.right, my + t}});
        } else {
            ComPtr<ID2D1RectangleGeometry> rg;
            f->CreateRectangleGeometry(r, &rg);
            g = rg;
        }
        return g;
    }
    ComPtr<ID2D1Geometry> Custom(const Node* cust, D2D1_RECT_F r, const Node* xfrm) {
        ComPtr<ID2D1Factory> f;
        rt->GetFactory(&f);
        ComPtr<ID2D1PathGeometry> path;
        ComPtr<ID2D1GeometrySink> sink;
        f->CreatePathGeometry(&path);
        path->Open(&sink);
        const Node* ext = xfrm ? (*xfrm)["ext"] : nullptr;
        bool open = false;
        D2D1_POINT_2F cur{};
        if (const Node* lst = cust ? (*cust)["pathLst"] : nullptr)
            for (auto& p : lst->kids) {
                float pw = Num(p.A("w"), ext ? ext->D("cx", 1) : 1), ph = Num(p.A("h"), ext ? ext->D("cy", 1) : 1);
                float sx = pw ? (r.right - r.left) / pw : 0, sy = ph ? (r.bottom - r.top) / ph : 0;
                auto pt = [&](const Node& n) { return D2D1_POINT_2F{r.left + Num(n.A("x"), 0) * sx, r.top + Num(n.A("y"), 0) * sy}; };
                for (auto& c : p.kids) {
                    if (c.name == "moveTo" && !c.kids.empty()) {
                        if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
                        sink->BeginFigure(cur = pt(c.kids[0]), D2D1_FIGURE_BEGIN_FILLED), open = true;
                    } else if (!open) continue;
                    else if (c.name == "lnTo" && !c.kids.empty()) sink->AddLine(cur = pt(c.kids[0]));
                    else if (c.name == "cubicBezTo" && c.kids.size() >= 3)
                        sink->AddBezier({pt(c.kids[0]), pt(c.kids[1]), cur = pt(c.kids[2])});
                    else if (c.name == "quadBezTo" && c.kids.size() >= 2) sink->AddQuadraticBezier({pt(c.kids[0]), cur = pt(c.kids[1])});
                    else if (c.name == "arcTo") {
                        float wr = Num(c.A("wR"), 0) * sx, hr = Num(c.A("hR"), 0) * sy, deg = 3.14159265f / 180;
                        float st = Num(c.A("stAng"), 0) / 60000, sw = Num(c.A("swAng"), 0) / 60000;
                        D2D1_POINT_2F ctr{cur.x - wr * std::cos(st * deg), cur.y - hr * std::sin(st * deg)};
                        for (int half = 0, n = std::abs(sw) > 180 ? 2 : 1; half < n; half++) {  // >180: two arcs (a full circle has no single arc)
                            float a = st + sw * (half + 1) / n;
                            cur = {ctr.x + wr * std::cos(a * deg), ctr.y + hr * std::sin(a * deg)};
                            sink->AddArc({cur, {wr, hr}, 0, sw >= 0 ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                                          D2D1_ARC_SIZE_SMALL});
                        }
                    } else if (c.name == "close") sink->EndFigure(D2D1_FIGURE_END_CLOSED), open = false;
                }
            }
        if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        return path;
    }

    // ---- text
    void Text(const Node* txBody, D2D1_RECT_F box, const TextBox& tb) {
        if (!txBody) return;
        IDWriteFactory* dw = DW();
        struct Para {
            ComPtr<IDWriteTextLayout> layout;
            float x, before, after, height;
            std::wstring bullet, bulletFont;
            float bulletX, bulletSize;
            D2D1_COLOR_F bulletColor;
        };
        std::vector<Para> paras;
        float width = std::max(1.f, box.right - box.left - tb.ins[0] - tb.ins[2]);
        int counter[10] = {};
        for (auto& p : txBody->kids) {
            if (p.name != "p") continue;
            const Node* pPr = p["pPr"];
            int lvl = std::clamp(pPr ? (int)pPr->D("lvl", 0) : 0, 0, 8);
            std::string lvlName = "lvl" + std::to_string(lvl + 1) + "pPr";
            std::vector<const Node*> pp{pPr};
            for (auto s : tb.lst)
                if (s) pp.push_back((*s)[lvlName]);
            auto pa = [&](std::string_view n) -> std::string_view {
                for (auto x : pp)
                    if (x && !x->A(n).empty()) return x->A(n);
                return {};
            };
            auto pc = [&](std::string_view n) -> const Node* {
                for (auto x : pp)
                    if (const Node* k = x ? (*x)[n] : nullptr) return k;
                return nullptr;
            };
            struct Run {
                UINT32 start, len;
                float size;
                bool b, i, u;
                D2D1_COLOR_F color;
                std::wstring font;
            };
            auto style = [&](const Node* rPr) {
                std::vector<const Node*> rr{rPr};
                for (auto x : pp)
                    if (x) rr.push_back((*x)["defRPr"]);
                auto ra = [&](std::string_view n) -> std::string_view {
                    for (auto x : rr)
                        if (x && !x->A(n).empty()) return x->A(n);
                    return {};
                };
                auto rc = [&](std::string_view n) -> const Node* {
                    for (auto x : rr)
                        if (const Node* k = x ? (*x)[n] : nullptr) return k;
                    return nullptr;
                };
                Run x{0, 0, Num(ra("sz"), 1800) / 100 * tb.fontScale, tb.bold || ra("b") == "1" || ra("b") == "true",
                      ra("i") == "1" || ra("i") == "true", !ra("u").empty() && ra("u") != "none", tb.color};
                if (const Node* fill = rc("solidFill")) Color(fill, x.color);
                const Node* latin = rc("latin");
                x.font = Font(latin ? latin->A("typeface") : "");
                return x;
            };
            std::wstring text;
            std::vector<Run> runs;
            for (auto& k : p.kids) {
                std::wstring w;
                if (k.name == "r" || k.name == "fld") w = k["t"] ? Wide(k["t"]->text) : L"";
                else if (k.name == "br") w = L" ";
                else continue;
                for (auto& ch : w) ch = ch == L'\t' ? L' ' : ch;
                Run x = style(k["rPr"]);
                x.start = (UINT32)text.size(), x.len = (UINT32)w.size();
                text += w;
                if (x.len) runs.push_back(x);
            }
            Run base = runs.empty() ? style(p["endParaRPr"]) : runs[0];
            float maxSize = base.size;
            for (auto& x : runs) maxSize = std::max(maxSize, x.size);

            Para out{};
            float marL = Num(pa("marL"), 0) / 12700, indent = Num(pa("indent"), 0) / 12700;
            out.x = marL;
            bool autoNum = false;
            for (auto x : pp) {  // bullet: the nearest level that says anything about one
                if (!x) continue;
                if ((*x)["buNone"]) break;
                if (const Node* bu = (*x)["buChar"]) {
                    out.bullet = Wide(XmlText(bu->A("char")));
                    break;
                }
                if (const Node* an = (*x)["buAutoNum"]) {
                    int n = (int)an->D("startAt", 1) + counter[lvl]++;
                    autoNum = true;
                    std::string_view type = an->A("type");
                    std::wstring s = type.starts_with("alphaLc") ? std::wstring(1, wchar_t(L'a' + (n - 1) % 26))
                                     : type.starts_with("alphaUc") ? std::wstring(1, wchar_t(L'A' + (n - 1) % 26))
                                                                   : std::to_wstring(n);
                    out.bullet = s + (type.ends_with("ParenR") ? L")" : type.ends_with("ParenBoth") ? L")" : L".");
                    if (type.ends_with("ParenBoth")) out.bullet = L"(" + out.bullet;
                    break;
                }
            }
            for (int k = lvl + 1; k < 10; k++) counter[k] = 0;
            if (!autoNum) counter[lvl] = 0;
            if (text.empty()) out.bullet.clear();  // no bullet on an empty line
            if (!out.bullet.empty()) {
                const Node* bf = pc("buFont");
                out.bulletFont = bf ? Font(bf->A("typeface")) : base.font;
                out.bulletX = marL + indent;
                out.bulletSize = base.size * (pc("buSzPct") ? Num(pc("buSzPct")->A("val"), 100000) / 100000 : 1);
                out.bulletColor = base.color;
                if (const Node* bc = pc("buClr")) Color(bc, out.bulletColor);
            }

            ComPtr<IDWriteTextFormat> fmt;
            if (FAILED(dw->CreateTextFormat(base.font.c_str(), nullptr, base.b ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                            base.i ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                                            DWRITE_FONT_STRETCH_NORMAL, base.size, L"", &fmt)))
                continue;
            std::string_view algn = pa("algn");
            fmt->SetTextAlignment(algn == "ctr" ? DWRITE_TEXT_ALIGNMENT_CENTER : algn == "r" ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                  : algn == "just" || algn == "dist" ? DWRITE_TEXT_ALIGNMENT_JUSTIFIED : DWRITE_TEXT_ALIGNMENT_LEADING);
            if (tb.nowrap) fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            if (FAILED(dw->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt.Get(), std::max(1.f, width - marL), 1e5f, &out.layout)))
                continue;
            for (auto& x : runs) {
                DWRITE_TEXT_RANGE rg{x.start, x.len};
                ComPtr<ID2D1SolidColorBrush> brush;
                rt->CreateSolidColorBrush(x.color, &brush);
                out.layout->SetFontSize(x.size, rg);
                out.layout->SetFontWeight(x.b ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL, rg);
                out.layout->SetFontStyle(x.i ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, rg);
                out.layout->SetUnderline(x.u, rg);
                out.layout->SetFontFamilyName(x.font.c_str(), rg);
                out.layout->SetDrawingEffect(brush.Get(), rg);
            }
            // Line spacing: a percentage of single (1.2 x size) or exact points.
            float line = maxSize * 1.2f;
            if (const Node* ls = pc("lnSpc")) {
                if (const Node* pct = (*ls)["spcPct"]) line *= Num(pct->A("val"), 100000) / 100000;
                else if (const Node* pts = (*ls)["spcPts"]) line = Num(pts->A("val"), 1200) / 100;
            }
            line *= 1 - tb.spcRed;
            out.layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, line, line * 0.8f);
            auto spc = [&](std::string_view n) -> float {
                const Node* s = pc(n);
                if (!s) return 0;
                if (const Node* pts = (*s)["spcPts"]) return Num(pts->A("val"), 0) / 100;
                if (const Node* pct = (*s)["spcPct"]) return Num(pct->A("val"), 0) / 100000 * maxSize * 1.2f;
                return 0;
            };
            out.before = paras.empty() ? 0 : spc("spcBef") * (1 - tb.spcRed);
            out.after = spc("spcAft") * (1 - tb.spcRed);
            DWRITE_TEXT_METRICS m{};
            out.layout->GetMetrics(&m);
            out.height = m.height;
            paras.push_back(std::move(out));
        }
        float total = 0;
        for (auto& p : paras) total += p.before + p.height + p.after;
        float avail = box.bottom - box.top - tb.ins[1] - tb.ins[3];
        float y = box.top + tb.ins[1] + (tb.anchor == "ctr" ? (avail - total) / 2 : tb.anchor == "b" ? avail - total : 0);
        ComPtr<ID2D1SolidColorBrush> brush;
        rt->CreateSolidColorBrush(tb.color, &brush);
        for (auto& p : paras) {
            y += p.before;
            float x = box.left + tb.ins[0];
            rt->DrawTextLayout({x + p.x, y}, p.layout.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            if (!p.bullet.empty()) {
                ComPtr<IDWriteTextFormat> bf;
                DWRITE_LINE_METRICS lm{};
                UINT32 n = 0;
                p.layout->GetLineMetrics(&lm, 1, &n);
                if (SUCCEEDED(DW()->CreateTextFormat(p.bulletFont.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                                     DWRITE_FONT_STRETCH_NORMAL, p.bulletSize, L"", &bf))) {
                    bf->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                    ComPtr<IDWriteTextLayout> bl;
                    if (SUCCEEDED(DW()->CreateTextLayout(p.bullet.c_str(), (UINT32)p.bullet.size(), bf.Get(), 1e4f, 1e4f, &bl))) {
                        DWRITE_LINE_METRICS bm{};
                        UINT32 bn = 0;
                        bl->GetLineMetrics(&bm, 1, &bn);
                        brush->SetColor(p.bulletColor);
                        rt->DrawTextLayout({x + p.bulletX, y + lm.baseline - bm.baseline}, bl.Get(), brush.Get());
                    }
                }
            }
            y += p.height + p.after;
        }
    }

    // Text box settings: bodyPr of the shape, then of the placeholders it inherits from.
    void Body(TextBox& tb, std::initializer_list<const Node*> bodies) {
        auto ba = [&](std::string_view n) -> std::string_view {
            for (auto b : bodies)
                if (b && !b->A(n).empty()) return b->A(n);
            return {};
        };
        tb.ins[0] = Num(ba("lIns"), 91440) / 12700, tb.ins[1] = Num(ba("tIns"), 45720) / 12700;
        tb.ins[2] = Num(ba("rIns"), 91440) / 12700, tb.ins[3] = Num(ba("bIns"), 45720) / 12700;
        tb.anchor = ba("anchor");
        tb.nowrap = ba("wrap") == "none";
        for (auto b : bodies)
            if (const Node* fit = b ? (*b)["normAutofit"] : nullptr) {
                tb.fontScale = Num(fit->A("fontScale"), 100000) / 100000, tb.spcRed = Num(fit->A("lnSpcReduction"), 0) / 100000;
                break;
            } else if (b && ((*b)["noAutofit"] || (*b)["spAutoFit"])) break;
    }

    // ---- shapes
    // The layout/master shape a placeholder inherits from: same idx, else the same kind.
    const Node* Match(Part* part, std::string_view type, std::string_view idx) {
        const Node* tree = At(TopOf(part), {"cSld", "spTree"});
        if (!tree) return nullptr;
        const Node* byType = nullptr;
        for (auto& k : tree->kids)
            if (const Node* ph = PhOf(k)) {
                if (!idx.empty() && ph->A("idx") == idx) return &k;
                if (!byType && PhType(ph->A("type")) == type) byType = &k;
            }
        return byType;
    }

    void Tree(const Node& tree, Part& part, const Xf& xf, bool placeholders) {
        for (auto& k : tree.kids) {
            if (k.name == "AlternateContent") {  // the fallback is what older readers show
                if (const Node* alt = k["Fallback"] ? k["Fallback"] : k["Choice"]) Tree(*alt, part, xf, placeholders);
                continue;
            }
            if (k.name != "sp" && k.name != "cxnSp" && k.name != "pic" && k.name != "grpSp" && k.name != "graphicFrame") continue;
            if (Hidden(k) || (!placeholders && PhOf(k))) continue;
            if (k.name == "grpSp") {
                const Node* x = XfrmOf(&k);
                const Node *off = x ? (*x)["off"] : nullptr, *ext = x ? (*x)["ext"] : nullptr, *choff = x ? (*x)["chOff"] : nullptr,
                           *chext = x ? (*x)["chExt"] : nullptr;
                Xf inner = xf;
                if (off && ext && choff && chext) {
                    float kx = (float)(chext->D("cx", 0) ? ext->D("cx", 0) / chext->D("cx", 0) : 1);
                    float ky = (float)(chext->D("cy", 0) ? ext->D("cy", 0) / chext->D("cy", 0) : 1);
                    inner.sx = xf.sx * kx, inner.sy = xf.sy * ky;
                    inner.tx = xf.sx * (float)(off->D("x", 0) - choff->D("x", 0) * kx) + xf.tx;
                    inner.ty = xf.sy * (float)(off->D("y", 0) - choff->D("y", 0) * ky) + xf.ty;
                }
                Tree(k, part, inner, placeholders);
                continue;
            }
            Shape(k, part, xf);
        }
    }

    void Shape(const Node& sp, Part& part, const Xf& xf) {
        const Node *phNode = PhOf(sp), *lph = nullptr, *mph = nullptr;
        std::string_view type;
        if (phNode) {
            type = PhType(phNode->A("type"));
            if (&part == slide) lph = Match(layout, type, phNode->A("idx"));
            if (&part != master) mph = Match(master, lph && PhOf(*lph) ? PhType(PhOf(*lph)->A("type")) : type, {});
        }
        const Node* xfrm = XfrmOf(&sp);
        if (!xfrm) xfrm = XfrmOf(lph);
        if (!xfrm) xfrm = XfrmOf(mph);
        if (!xfrm) return;
        D2D1_RECT_F r = xf.Rect(*xfrm);
        float rot = Num(xfrm->A("rot"), 0) / 60000;
        bool fh = xfrm->A("flipH") == "1", fv = xfrm->A("flipV") == "1";
        D2D1_POINT_2F ctr{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
        rt->SetTransform(Matrix3x2F::Scale(fh ? -1.f : 1.f, fv ? -1.f : 1.f, ctr) * Matrix3x2F::Rotation(rot, ctr) * base);

        const Node* spPr = sp["spPr"];
        const Node* geomPr = spPr && ((*spPr)["prstGeom"] || (*spPr)["custGeom"]) ? spPr : lph && (*lph)["spPr"] ? (*lph)["spPr"] : spPr;
        ComPtr<ID2D1Geometry> g = Geometry(geomPr, r, xfrm);
        const Node* style = sp["style"];
        if (sp.name == "graphicFrame") {
            if (const Node* tbl = At(&sp, {"graphic", "graphicData", "tbl"})) Table(*tbl, part, r, xf);
        } else if (sp.name == "pic") {
            if (const Node* bf = sp["blipFill"]) {
                ComPtr<ID2D1RectangleGeometry> rg;
                DrawBlip(*bf, part, r, SUCCEEDED(g->QueryInterface(IID_PPV_ARGS(&rg))) ? nullptr : g.Get());
            }
            Line(spPr ? (*spPr)["ln"] : nullptr, style, g.Get());
        } else {
            if (!Fill(spPr, part, g.Get(), r) && !(lph && Fill((*lph)["spPr"], *layout, g.Get(), r)) &&
                !(mph && Fill((*mph)["spPr"], *master, g.Get(), r)) && style)
                if (const Node* ref = (*style)["fillRef"]; ref && Num(ref->A("idx"), 0) > 0) {
                    D2D1_COLOR_F c;
                    ComPtr<ID2D1SolidColorBrush> b;
                    if (Color(ref, c)) rt->CreateSolidColorBrush(c, &b), rt->FillGeometry(g.Get(), b.Get());
                }
            const Node* ln = spPr ? (*spPr)["ln"] : nullptr;
            if (!ln && lph) ln = At(lph, {"spPr", "ln"});
            if (!ln && mph) ln = At(mph, {"spPr", "ln"});
            Line(ln, style, g.Get());
        }

        // Text turns with the shape but isn't mirrored (a vertical flip turns it upside down).
        if (const Node* tx = sp["txBody"]) {
            rt->SetTransform(Matrix3x2F::Rotation(rot + (fv ? 180 : 0), ctr) * base);
            TextBox tb;
            const Node* styles = type == "title" ? (txStyles ? (*txStyles)["titleStyle"] : nullptr)
                                 : type == "body" ? (txStyles ? (*txStyles)["bodyStyle"] : nullptr)
                                                  : (txStyles ? (*txStyles)["otherStyle"] : nullptr);
            tb.lst = {(*tx)["lstStyle"], At(lph, {"txBody", "lstStyle"}), At(mph, {"txBody", "lstStyle"}), styles, d.defaults};
            Body(tb, {(*tx)["bodyPr"], At(lph, {"txBody", "bodyPr"}), At(mph, {"txBody", "bodyPr"})});
            tb.color = Scheme("dk1");
            if (clrMap && !clrMap->A("tx1").empty()) tb.color = Scheme(clrMap->A("tx1"));
            if (const Node* ref = style ? (*style)["fontRef"] : nullptr) Color(ref, tb.color);
            Text(tx, r, tb);
        }
        rt->SetTransform(base);
    }

    // Tables in the default style's look (header row in accent 1, banded rows in its tints, white rules).
    void Table(const Node& tbl, Part& part, D2D1_RECT_F frame, const Xf& xf) {
        std::vector<float> colX{frame.left};
        if (const Node* grid = tbl["tblGrid"])
            for (auto& c : grid->kids) colX.push_back(colX.back() + Num(c.A("w"), 0) * xf.sx);
        const Node* pr = tbl["tblPr"];
        bool firstRow = pr && pr->A("firstRow") == "1", band = pr && pr->A("bandRow") == "1";
        D2D1_COLOR_F accent = Scheme("accent1"), light = accent, lighter = accent;
        auto tintTo = [](D2D1_COLOR_F& c, float mod, float off) {
            float h, s, l;
            ToHsl(c, h, s, l);
            FromHsl(c, h, s, std::clamp(l * mod + off, 0.f, 1.f));
        };
        tintTo(light, 0.4f, 0.6f), tintTo(lighter, 0.2f, 0.8f);
        float y = frame.top;
        int r = 0;
        for (auto& tr : tbl.kids) {
            if (tr.name != "tr") continue;
            float h = Num(tr.A("h"), 0) * xf.sy;
            size_t c = 0;
            for (auto& tc : tr.kids) {
                if (tc.name != "tc") continue;
                size_t span = std::max(1, (int)tc.D("gridSpan", 1));
                if (tc.A("hMerge") == "1" || tc.A("vMerge") == "1" || c + 1 >= colX.size()) {
                    c++;
                    continue;
                }
                // shortcut: rowSpan cells are one row tall (the rows below draw over them).
                D2D1_RECT_F cell{colX[c], y, colX[std::min(c + span, colX.size() - 1)], y + h};
                bool head = firstRow && r == 0;
                const Node* tcPr = tc["tcPr"];
                ComPtr<ID2D1Factory> f;
                ComPtr<ID2D1RectangleGeometry> g;
                rt->GetFactory(&f);
                f->CreateRectangleGeometry(cell, &g);
                if (!Fill(tcPr, part, g.Get(), cell)) {
                    ComPtr<ID2D1SolidColorBrush> b;
                    rt->CreateSolidColorBrush(head ? accent : band && (r - firstRow) % 2 == 0 ? light : lighter, &b);
                    rt->FillGeometry(g.Get(), b.Get());
                }
                ComPtr<ID2D1SolidColorBrush> white;
                rt->CreateSolidColorBrush(Scheme("lt1"), &white);
                rt->DrawRectangle(cell, white.Get(), 1);
                TextBox tb;
                tb.lst = {At(&tc, {"txBody", "lstStyle"}), d.defaults};
                tb.ins[0] = Num(tcPr ? tcPr->A("marL") : "", 91440) / 12700, tb.ins[2] = Num(tcPr ? tcPr->A("marR") : "", 91440) / 12700;
                tb.ins[1] = Num(tcPr ? tcPr->A("marT") : "", 45720) / 12700, tb.ins[3] = Num(tcPr ? tcPr->A("marB") : "", 45720) / 12700;
                tb.anchor = tcPr ? tcPr->A("anchor") : "";
                tb.color = head ? Scheme("lt1") : Scheme("dk1");
                tb.bold = head;
                Text(tc["txBody"], cell, tb);
                c += span;
            }
            y += h, r++;
        }
    }

    void Background() {
        D2D1_RECT_F all{0, 0, d.w, d.h};
        ComPtr<ID2D1Factory> f;
        ComPtr<ID2D1RectangleGeometry> g;
        rt->GetFactory(&f);
        f->CreateRectangleGeometry(all, &g);
        for (Part* p : {slide, layout, master}) {
            const Node* bg = At(TopOf(p), {"cSld", "bg"});
            if (!bg) continue;
            if (Fill((*bg)["bgPr"], *p, g.Get(), all)) return;
            D2D1_COLOR_F c;
            ComPtr<ID2D1SolidColorBrush> b;
            if (Color((*bg)["bgRef"], c)) rt->CreateSolidColorBrush(c, &b), rt->FillRectangle(all, b.Get());
            return;
        }
    }

    void Run() {
        rt->SetTransform(base);
        layout = d.Related(*slide, L"slideLayouts/");
        master = layout ? d.Related(*layout, L"slideMasters/") : nullptr;
        if (Part* th = master ? d.Related(*master, L"theme/") : nullptr) {
            const Node* el = At(TopOf(th), {"themeElements"});
            if (const Node* cs = el ? (*el)["clrScheme"] : nullptr)
                for (auto& k : cs->kids)
                    for (int i = 0; i < 12; i++)
                        if (kScheme[i] == k.name) Color(&k, theme.c[i]);
            if (const Node* f = At(el, {"fontScheme", "majorFont", "latin"})) theme.major = Wide(f->A("typeface"));
            if (const Node* f = At(el, {"fontScheme", "minorFont", "latin"})) theme.minor = Wide(f->A("typeface"));
        }
        clrMap = At(TopOf(master), {"clrMap"});
        txStyles = At(TopOf(master), {"txStyles"});

        // Pictures used on this page, read from the zip in one pass.
        std::unordered_set<std::wstring> want;
        auto collect = [&](auto& self, const Node& n, Part& p) -> void {
            if (n.name == "blip")
                if (auto it = p.rels.find(std::string(n.A("r:embed"))); it != p.rels.end()) want.insert(it->second);
            for (auto& k : n.kids) self(self, k, p);
        };
        for (Part* p : {slide, layout, master})
            if (p) collect(collect, p->root, *p);
        if (!want.empty())
            ArchiveReadMembers(d.path, [&](std::wstring_view n) -> size_t { return want.count(std::wstring(n)) ? 64 << 20 : 0; }, media);

        Background();
        bool showMaster = TopOf(slide)->A("showMasterSp") != "0";
        if (showMaster && master && TopOf(layout)->A("showMasterSp") != "0")
            if (const Node* t = At(TopOf(master), {"cSld", "spTree"})) Tree(*t, *master, {}, false);
        if (showMaster && layout)
            if (const Node* t = At(TopOf(layout), {"cSld", "spTree"})) Tree(*t, *layout, {}, false);
        if (const Node* t = At(TopOf(slide), {"cSld", "spTree"})) Tree(*t, *slide, {}, true);
    }
};

}  // namespace


// ---- docx: paragraphs and tables flowed onto pages at open (DirectWrite measures the lines), drawn per page.
// Styles (docDefaults, paragraph/character styles with basedOn), numbering (bullets, numbers), inline pictures.
// shortcut: no headers/footers, floating objects (anchored pictures flow inline), columns, text boxes, table
// styles beyond borders, or widow control; a table row taller than a page is cut off.
namespace {

struct Doc {
    Pages& d;
    Part* doc = nullptr;
    const Node *styles = nullptr, *numbering = nullptr;
    std::wstring major = L"Calibri Light", minor = L"Calibri";
    float W = 612, H = 792, ml = 72, mr = 72, mt = 72, mb = 72;
    float y = 0;
    std::map<std::pair<std::string, int>, int> counters;  // (numId, level) -> last number

    const Node* Style(std::string_view id) {
        if (styles)
            for (auto& s : styles->kids)
                if (s.name == "style" && s.A("styleId") == id) return &s;
        return nullptr;
    }
    const Node* DefaultStyle(std::string_view type) {
        if (styles)
            for (auto& s : styles->kids)
                if (s.name == "style" && s.A("type") == type && (s.A("default") == "1" || s.A("default") == "true")) return &s;
        return nullptr;
    }
    // Property nodes of a style and the styles it is based on (nearest first).
    void Chain(const Node* s, std::string_view prop, std::vector<const Node*>& out) {
        for (int guard = 0; s && guard < 20; guard++) {
            out.push_back((*s)[prop]);
            const Node* based = (*s)["basedOn"];
            s = based ? Style(based->A("val")) : nullptr;
        }
    }
    std::wstring Font(const Node* rFonts) {
        if (!rFonts) return minor;
        std::string_view t = rFonts->A("asciiTheme");
        if (!t.empty()) return t.starts_with("major") ? major : minor;
        std::string_view f = rFonts->A("ascii");
        return f.empty() ? minor : Wide(f);
    }
    static bool On(const Node* n) {
        if (!n) return false;
        std::string_view v = n->A("val");
        return v.empty() || v == "1" || v == "true" || v == "on";
    }

    struct Block {  // one paragraph, measured
        ComPtr<IDWriteTextLayout> layout, bullet;
        std::vector<std::pair<DWRITE_TEXT_RANGE, D2D1_COLOR_F>> colors;
        D2D1_COLOR_F bulletColor{0, 0, 0, 1};
        float x = 0, bulletX = 0, before = 0, after = 0;
        bool pageBefore = false, pageAfter = false;
        std::vector<std::tuple<std::wstring, float, float>> pictures;  // zip path, width, height (points)
        int align = 0;
    };

    Block Para(const Node& p, float width) {
        Block b;
        const Node* pPr = p["pPr"];
        const Node* ps = pPr && (*pPr)["pStyle"] ? Style((*pPr)["pStyle"]->A("val")) : DefaultStyle("paragraph");
        std::vector<const Node*> pp{pPr};
        Chain(ps, "pPr", pp);
        pp.push_back(At(styles, {"docDefaults", "pPrDefault", "pPr"}));
        auto pc = [&](std::string_view n) -> const Node* {
            for (auto x : pp)
                if (const Node* k = x ? (*x)[n] : nullptr) return k;
            return nullptr;
        };
        auto pattr = [&](std::string_view n, std::string_view a) -> std::string_view {  // first node whose child has the attribute
            for (auto x : pp)
                if (const Node* k = x ? (*x)[n] : nullptr; k && !k->A(a).empty()) return k->A(a);
            return {};
        };
        std::vector<const Node*> paraRuns;
        Chain(ps, "rPr", paraRuns);
        paraRuns.push_back(At(styles, {"docDefaults", "rPrDefault", "rPr"}));

        struct Run {
            UINT32 start, len;
            float size;
            bool b, i, u, caps;
            D2D1_COLOR_F color;
            std::wstring font;
        };
        auto style = [&](const Node* rPr) {
            std::vector<const Node*> rr{rPr};
            if (const Node* rs = rPr ? (*rPr)["rStyle"] : nullptr) Chain(Style(rs->A("val")), "rPr", rr);
            rr.insert(rr.end(), paraRuns.begin(), paraRuns.end());
            auto rc = [&](std::string_view n) -> const Node* {
                for (auto x : rr)
                    if (const Node* k = x ? (*x)[n] : nullptr) return k;
                return nullptr;
            };
            Run r{0, 0, Num(rc("sz") ? rc("sz")->A("val") : "", 22) / 2, On(rc("b")), On(rc("i")),
                  rc("u") && rc("u")->A("val") != "none", On(rc("caps")), {0, 0, 0, 1}, Font(rc("rFonts"))};
            if (const Node* c = rc("color"); c && c->A("val") != "auto" && !c->A("val").empty()) r.color = Rgb(c->A("val"));
            return r;
        };
        std::wstring text;
        std::vector<Run> runs;
        auto add = [&](const std::wstring& w, const Node* rPr) {
            Run r = style(rPr);
            r.start = (UINT32)text.size(), r.len = (UINT32)w.size();
            text += r.caps ? [&] { std::wstring u = w; CharUpperBuffW(u.data(), (DWORD)u.size()); return u; }() : w;
            if (r.len) runs.push_back(r);
        };
        // Runs, also inside hyperlinks, fields, content controls and insertions.
        auto walk = [&](auto& self, const Node& n) -> void {
            for (auto& k : n.kids) {
                if (k.name == "r") {
                    const Node* rPr = k["rPr"];
                    for (auto& c : k.kids) {
                        if (c.name == "t") add(Wide(c.text), rPr);
                        else if (c.name == "tab") add(L"\t", rPr);
                        else if (c.name == "br" || c.name == "cr") {
                            if (c.A("type") == "page") b.pageAfter = true;
                            else add(L"\u2028", rPr);
                        } else if (c.name == "noBreakHyphen") add(L"-", rPr);
                        else if (c.name == "drawing" || c.name == "AlternateContent") {
                            const Node* dr = c.name == "drawing" ? &c : At(&c, {"Choice", "drawing"});
                            const Node* holder = dr ? (dr->kids.empty() ? nullptr : &dr->kids[0]) : nullptr;  // inline / anchor
                            const Node* ext = holder ? (*holder)["extent"] : nullptr;
                            const Node* blip = holder ? At(holder, {"graphic", "graphicData", "pic", "blipFill", "blip"}) : nullptr;
                            if (ext && blip)
                                if (auto it = doc->rels.find(std::string(blip->A("r:embed"))); it != doc->rels.end())
                                    b.pictures.push_back({it->second, (float)ext->D("cx", 0) / 12700, (float)ext->D("cy", 0) / 12700});
                        }
                    }
                } else if (k.name == "hyperlink" || k.name == "smartTag" || k.name == "sdt" || k.name == "sdtContent" ||
                           k.name == "ins" || k.name == "fldSimple" || k.name == "customXml")
                    self(self, k);
            }
        };
        walk(walk, p);
        b.pageBefore = On(pc("pageBreakBefore"));

        // Indents (twips) and the list bullet or number.
        float left = Num(pattr("ind", "left").empty() ? pattr("ind", "start") : pattr("ind", "left"), 0) / 20;
        float hanging = Num(pattr("ind", "hanging"), 0) / 20, first = Num(pattr("ind", "firstLine"), 0) / 20;
        std::wstring bullet;
        if (const Node* numPr = pc("numPr")) {
            std::string numId((*numPr)["numId"] ? (*numPr)["numId"]->A("val") : std::string_view{});
            int ilvl = (*numPr)["ilvl"] ? (int)(*numPr)["ilvl"]->D("val", 0) : 0;
            const Node* lvl = nullptr;
            if (numbering && !numId.empty() && numId != "0")
                for (auto& n : numbering->kids)
                    if (n.name == "num" && n.A("numId") == numId && n["abstractNumId"])
                        for (auto& a : numbering->kids)
                            if (a.name == "abstractNum" && a.A("abstractNumId") == n["abstractNumId"]->A("val"))
                                for (auto& l : a.kids)
                                    if (l.name == "lvl" && (int)l.D("ilvl", 0) == ilvl) lvl = &l;
            if (lvl) {
                std::string_view fmt = (*lvl)["numFmt"] ? (*lvl)["numFmt"]->A("val") : "decimal";
                std::wstring tmpl = (*lvl)["lvlText"] ? Wide(XmlText((*lvl)["lvlText"]->A("val"))) : L"%1.";
                for (auto& [key, v] : counters)
                    if (key.first == numId && key.second > ilvl) v = 0;
                int& n = counters[{numId, ilvl}];
                n = n ? n + 1 : (*lvl)["start"] ? (int)(*lvl)["start"]->D("val", 1) : 1;
                if (fmt == "bullet") bullet = tmpl.empty() || tmpl[0] < 0x20 || tmpl[0] >= 0xF000 ? L"\u2022" : tmpl;
                else if (fmt != "none") {
                    for (size_t k; (k = tmpl.find(L'%')) != tmpl.npos && k + 1 < tmpl.size();) {
                        int which = tmpl[k + 1] - L'1';
                        int v = which == ilvl ? n : std::max(1, counters[{numId, which}]);
                        std::wstring s = fmt == "lowerLetter" ? std::wstring(1, wchar_t(L'a' + (v - 1) % 26))
                                         : fmt == "upperLetter" ? std::wstring(1, wchar_t(L'A' + (v - 1) % 26))
                                                                : std::to_wstring(v);
                        tmpl.replace(k, 2, s);
                    }
                    bullet = tmpl;
                }
                if (const Node* ind = At(lvl, {"pPr", "ind"}); ind && !At(pPr, {"ind"})) {  // the paragraph's own indent wins
                    left = Num(ind->A("left").empty() ? ind->A("start") : ind->A("left"), left * 20) / 20;
                    hanging = Num(ind->A("hanging"), hanging * 20) / 20;
                }
            }
        }
        b.x = left;
        Run base = runs.empty() ? style(At(pPr, {"rPr"})) : runs[0];
        float maxSize = base.size;
        for (auto& r : runs) maxSize = std::max(maxSize, r.size);

        // Spacing (twips); line: 240ths of single with lineRule auto, else points.
        b.before = Num(pattr("spacing", "before"), 0) / 20, b.after = Num(pattr("spacing", "after"), 0) / 20;
        float line = maxSize * 1.17f;
        if (std::string_view l = pattr("spacing", "line"); !l.empty()) {
            std::string_view rule = pattr("spacing", "lineRule");
            line = rule.empty() || rule == "auto" ? line * Num(l, 240) / 240 : rule == "exact" ? Num(l, 240) / 20 : std::max(line, Num(l, 240) / 20);
        }
        std::string_view jc = pc("jc") ? pc("jc")->A("val") : "";
        b.align = jc == "center" ? 1 : jc == "right" || jc == "end" ? 2 : 0;

        IDWriteFactory* dw = DW();
        ComPtr<IDWriteTextFormat> fmt;
        if (FAILED(dw->CreateTextFormat(base.font.c_str(), nullptr, base.b ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                        base.i ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                        base.size, L"", &fmt)))
            return b;
        fmt->SetTextAlignment(b.align == 1 ? DWRITE_TEXT_ALIGNMENT_CENTER : b.align == 2 ? DWRITE_TEXT_ALIGNMENT_TRAILING
                              : jc == "both" || jc == "distribute" ? DWRITE_TEXT_ALIGNMENT_JUSTIFIED : DWRITE_TEXT_ALIGNMENT_LEADING);
        fmt->SetIncrementalTabStop(36);  // Word's default tab stops: every half inch
        // shortcut: a first-line indent is approximated by leading spaces' worth of width (DirectWrite has no per-line indent).
        if (first > 0 && bullet.empty()) {
            UINT32 pad = (UINT32)std::max(1L, std::lround(first / (base.size * 0.25f)));  // a space is about 1/4 em
            text.insert(0, pad, L' ');
            for (auto& r : runs) r.start += pad;
        }
        if (FAILED(dw->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt.Get(), std::max(1.f, width - left - Num(pattr("ind", "right"), 0) / 20),
                                        1e6f, &b.layout)))
            return b;
        for (auto& r : runs) {
            DWRITE_TEXT_RANGE rg{r.start, r.len};
            b.layout->SetFontSize(r.size, rg);
            b.layout->SetFontWeight(r.b ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL, rg);
            b.layout->SetFontStyle(r.i ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, rg);
            b.layout->SetUnderline(r.u, rg);
            b.layout->SetFontFamilyName(r.font.c_str(), rg);
            b.colors.push_back({rg, r.color});
        }
        b.layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, line, line * 0.8f);
        if (!bullet.empty() && !text.empty()) {
            ComPtr<IDWriteTextFormat> bf;
            dw->CreateTextFormat(base.font.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                 DWRITE_FONT_STRETCH_NORMAL, base.size, L"", &bf);
            if (bf) {
                bf->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                dw->CreateTextLayout(bullet.c_str(), (UINT32)bullet.size(), bf.Get(), 1e4f, 1e4f, &b.bullet);
                if (b.bullet) b.bullet->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, line, line * 0.8f);
                b.bulletX = left - (hanging ? hanging : 18), b.bulletColor = base.color;
            }
        }
        return b;
    }

    std::vector<PageItem>& Page() { return d.doc.back(); }
    void NewPage() { d.doc.emplace_back(), y = mt; }

    // A paragraph onto the pages, split between lines where a page ends.
    void Flow(Block& b, float x0, float width) {
        if (b.pageBefore && y > mt) NewPage();
        if (y > mt) y += b.before;
        for (auto& [path, w, h] : b.pictures) {
            float s = std::min({1.f, width / std::max(w, 1.f), (H - mt - mb) / std::max(h, 1.f)});
            if (y + h * s > H - mb && y > mt) NewPage();
            float px = x0 + (b.align == 1 ? (width - w * s) / 2 : b.align == 2 ? width - w * s : b.x);
            PageItem it;
            it.media = path, it.clip = {px, y, px + w * s, y + h * s};
            Page().push_back(std::move(it));
            y += h * s;
        }
        if (!b.layout) return;
        std::vector<DWRITE_LINE_METRICS> lines(64);
        UINT32 n = 0;
        if (b.layout->GetLineMetrics(lines.data(), (UINT32)lines.size(), &n) == E_NOT_SUFFICIENT_BUFFER)
            lines.resize(n), b.layout->GetLineMetrics(lines.data(), n, &n);
        if (b.pictures.size() && n == 1 && lines[0].length <= 1) n = 0;  // a picture's own paragraph: no empty line under it
        float top = 0;  // layout position of the chunk's first line
        UINT32 k = 0;
        while (k < n) {
            float h = 0;
            UINT32 j = k;
            while (j < n && (y + h + lines[j].height <= H - mb || (j == k && y <= mt))) h += lines[j++].height;
            if (j == k) {
                NewPage();
                continue;
            }
            PageItem it;
            it.layout = b.layout, it.colors = b.colors, it.origin = {x0 + b.x, y - top};
            it.clip = {x0 - 2 * 72, y, x0 + width + 2 * 72, y + h};
            if (k == 0 && b.bullet) it.bullet = b.bullet, it.bulletOrigin = {x0 + b.bulletX, y}, it.bulletColor = b.bulletColor;
            Page().push_back(std::move(it));
            y += h, top += h, k = j;
            if (k < n) NewPage();
        }
        y += b.after;
        if (b.pageAfter) NewPage();
    }

    // Tables: rows kept whole; columns from tblGrid; borders when the table or its style has them.
    void Table(const Node& tbl, float x0, float width) {
        std::vector<float> cols{0};
        if (const Node* g = tbl["tblGrid"])
            for (auto& c : g->kids)
                if (c.name == "gridCol") cols.push_back(cols.back() + Num(c.A("w"), 0) / 20);
        if (cols.size() < 2) return;
        if (cols.back() > width) for (auto& c : cols) c *= width / cols.back();
        const Node* pr = tbl["tblPr"];
        const Node* ts = pr && (*pr)["tblStyle"] ? Style((*pr)["tblStyle"]->A("val")) : nullptr;
        const Node* borders = pr ? (*pr)["tblBorders"] : nullptr;
        for (const Node* s = ts; !borders && s; s = (*s)["basedOn"] ? Style((*s)["basedOn"]->A("val")) : nullptr)
            borders = At(s, {"tblPr", "tblBorders"});
        bool lines = borders && (*borders)["insideH"] && (*borders)["insideH"]->A("val") != "none" && (*borders)["insideH"]->A("val") != "nil";
        bool box = borders && (*borders)["top"] && (*borders)["top"]->A("val") != "none" && (*borders)["top"]->A("val") != "nil";
        if (y > mt) y += 4;
        for (auto& tr : tbl.kids) {
            if (tr.name != "tr") continue;
            struct Cell { float x, w; std::vector<Block> paras; float h; D2D1_COLOR_F fill; bool filled; };
            std::vector<Cell> cells;
            size_t c = 0;
            for (auto& tc : tr.kids) {
                if (tc.name != "tc" || c + 1 >= cols.size()) continue;
                const Node* tcPr = tc["tcPr"];
                size_t span = tcPr && (*tcPr)["gridSpan"] ? std::max(1, (int)(*tcPr)["gridSpan"]->D("val", 1)) : 1;
                Cell cell{cols[c], cols[std::min(c + span, cols.size() - 1)] - cols[c], {}, 0, {}, false};
                if (const Node* shd = tcPr ? (*tcPr)["shd"] : nullptr; shd && !shd->A("fill").empty() && shd->A("fill") != "auto")
                    cell.fill = Rgb(shd->A("fill")), cell.filled = true;
                bool merged = tcPr && (*tcPr)["vMerge"] && (*tcPr)["vMerge"]->A("val") != "restart";
                if (!merged)
                    for (auto& p : tc.kids)
                        if (p.name == "p") {
                            cell.paras.push_back(Para(p, cell.w - 10.8f));
                            DWRITE_TEXT_METRICS m{};
                            if (cell.paras.back().layout) cell.paras.back().layout->GetMetrics(&m);
                            cell.h += (cell.paras.size() > 1 ? cell.paras.back().before : 0) + m.height + cell.paras.back().after;
                        }
                cells.push_back(std::move(cell));
                c += span;
            }
            float rowH = std::max(Num(At(&tr, {"trPr", "trHeight"}) ? At(&tr, {"trPr", "trHeight"})->A("val") : "", 0) / 20, 14.f);
            for (auto& cell : cells) rowH = std::max(rowH, cell.h + 2);
            if (y + rowH > H - mb && y > mt) NewPage();
            for (auto& cell : cells) {
                D2D1_RECT_F r{x0 + cell.x, y, x0 + cell.x + cell.w, y + rowH};
                if (cell.filled) {
                    PageItem f;
                    f.fill = true, f.clip = r, f.bulletColor = cell.fill;
                    Page().push_back(std::move(f));
                }
                if (lines || box) {
                    PageItem f;
                    f.frame = true, f.clip = r;
                    Page().push_back(std::move(f));
                }
                float cy = y + 1;
                for (size_t i = 0; i < cell.paras.size(); i++) {
                    Block& b = cell.paras[i];
                    if (!b.layout) continue;
                    if (i) cy += b.before;
                    DWRITE_TEXT_METRICS m{};
                    b.layout->GetMetrics(&m);
                    PageItem it;
                    it.layout = b.layout, it.colors = b.colors, it.origin = {r.left + 5.4f + b.x, cy};
                    it.clip = {r.left, cy, r.right, cy + m.height};
                    if (b.bullet) it.bullet = b.bullet, it.bulletOrigin = {r.left + 5.4f + b.bulletX, cy}, it.bulletColor = b.bulletColor;
                    Page().push_back(std::move(it));
                    cy += m.height + b.after;
                }
            }
            y += rowH;
        }
        y += 8;
    }

    void Body(const Node& body) {
        for (auto& k : body.kids) {
            if (k.name == "p") {
                Block b = Para(k, W - ml - mr);
                Flow(b, ml, W - ml - mr);
                if (const Node* sect = At(&k, {"pPr", "sectPr"}); sect && (!(*sect)["type"] || (*sect)["type"]->A("val") != "continuous"))
                    NewPage();
            } else if (k.name == "tbl") Table(k, ml, W - ml - mr);
            else if (k.name == "sdt") {
                if (const Node* c = k["sdtContent"]) Body(*c);
            }
        }
    }
};

}  // namespace

bool OpenDocx(Pages& d) {
    auto want = [](std::wstring_view n) -> size_t {
        return n == L"word/document.xml" || n == L"word/styles.xml" || n == L"word/numbering.xml" ||
                       n == L"word/_rels/document.xml.rels" || (n.starts_with(L"word/theme/") && n.ends_with(L".xml"))
                   ? kMaxXml : 0;
    };
    if (!ArchiveReadMembers(d.path, want, d.m)) return false;
    Doc doc{d};
    doc.doc = &d.Get(L"word/document.xml");
    const Node* body = At(Top(doc.doc->root), {"body"});
    if (!body) return false;
    doc.styles = Top(d.Get(L"word/styles.xml").root);
    doc.numbering = Top(d.Get(L"word/numbering.xml").root);
    for (auto& [name, data] : d.m)
        if (name.starts_with(L"word/theme/")) {
            const Node* el = At(Top(d.Get(name).root), {"themeElements", "fontScheme"});
            if (const Node* f = At(el, {"majorFont", "latin"})) doc.major = Wide(f->A("typeface"));
            if (const Node* f = At(el, {"minorFont", "latin"})) doc.minor = Wide(f->A("typeface"));
            break;
        }
    if (const Node* sect = (*body)["sectPr"]) {  // page size and margins (twips)
        if (const Node* sz = (*sect)["pgSz"]) doc.W = Num(sz->A("w"), 12240) / 20, doc.H = Num(sz->A("h"), 15840) / 20;
        if (const Node* m = (*sect)["pgMar"])
            doc.ml = Num(m->A("left"), 1440) / 20, doc.mr = Num(m->A("right"), 1440) / 20, doc.mt = Num(m->A("top"), 1440) / 20,
            doc.mb = Num(m->A("bottom"), 1440) / 20;
    }
    doc.W = std::max(doc.W, doc.ml + doc.mr + 72), doc.H = std::max(doc.H, doc.mt + doc.mb + 72);
    d.word = true, d.w = doc.W, d.h = doc.H;
    doc.NewPage();
    doc.Body(*body);
    while (d.doc.size() > 1 && d.doc.back().empty()) d.doc.pop_back();
    return true;
}

namespace {

void DrawDoc(Pages& d, int index, ID2D1RenderTarget* rt, IWICImagingFactory* wic, float scale) {
    auto& items = d.doc[index];
    std::unordered_set<std::wstring> want;
    for (auto& it : items)
        if (!it.media.empty()) want.insert(it.media);
    std::vector<std::pair<std::wstring, std::string>> media;
    if (!want.empty())
        ArchiveReadMembers(d.path, [&](std::wstring_view n) -> size_t { return want.count(std::wstring(n)) ? 64 << 20 : 0; }, media);
    ComPtr<ID2D1SolidColorBrush> brush;
    rt->CreateSolidColorBrush({0, 0, 0, 1}, &brush);
    for (auto& it : items) {
        if (it.fill) {
            brush->SetColor(it.bulletColor);
            rt->FillRectangle(it.clip, brush.Get());
        } else if (it.frame) {
            brush->SetColor({0, 0, 0, 1});
            rt->DrawRectangle(it.clip, brush.Get(), 0.5f);
        } else if (!it.media.empty()) {
            for (auto& [n, bytes] : media) {
                if (n != it.media) continue;
                ComPtr<IStream> s;
                ComPtr<IWICBitmapDecoder> dec;
                ComPtr<IWICBitmapFrameDecode> frame;
                ComPtr<IWICBitmapScaler> scaler;
                ComPtr<IWICFormatConverter> conv;
                ComPtr<ID2D1Bitmap> bmp;
                s.Attach(SHCreateMemStream((const BYTE*)bytes.data(), (UINT)bytes.size()));
                UINT w = std::max(1u, (UINT)std::lround((it.clip.right - it.clip.left) * scale)),
                     h = std::max(1u, (UINT)std::lround((it.clip.bottom - it.clip.top) * scale)), fw = 0, fh = 0;
                if (!s || FAILED(wic->CreateDecoderFromStream(s.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec)) ||
                    FAILED(dec->GetFrame(0, &frame)))
                    break;
                IWICBitmapSource* src = frame.Get();
                frame->GetSize(&fw, &fh);
                if (fw > w && fh > h && SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
                    SUCCEEDED(scaler->Initialize(frame.Get(), w, h, WICBitmapInterpolationModeFant)))
                    src = scaler.Get();
                if (SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
                    SUCCEEDED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
                    SUCCEEDED(rt->CreateBitmapFromWicBitmap(conv.Get(), &bmp)))
                    rt->DrawBitmap(bmp.Get(), it.clip, 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                break;
            }
        } else if (it.layout) {
            for (auto& [rg, c] : it.colors) {
                ComPtr<ID2D1SolidColorBrush> b;
                rt->CreateSolidColorBrush(c, &b);
                it.layout->SetDrawingEffect(b.Get(), rg);
            }
            brush->SetColor({0, 0, 0, 1});
            rt->PushAxisAlignedClip(it.clip, D2D1_ANTIALIAS_MODE_ALIASED);
            rt->DrawTextLayout(it.origin, it.layout.Get(), brush.Get());
            rt->PopAxisAlignedClip();
            if (it.bullet) brush->SetColor(it.bulletColor), rt->DrawTextLayout(it.bulletOrigin, it.bullet.Get(), brush.Get());
        }
    }
}

}  // namespace

void PagesFree::operator()(Pages* p) const { delete p; }

bool PagesFor(const std::wstring& path) {
    const wchar_t* e = PathFindExtensionW(path.c_str());
    for (const wchar_t* x : {L".pptx", L".pptm", L".potx", L".potm", L".ppsx", L".ppsm", L".docx", L".docm", L".dotx", L".dotm"})
        if (!_wcsicmp(e, x)) return true;
    return false;
}

PagesPtr PagesOpen(const std::wstring& path) {
    PagesPtr d(new Pages);
    d->path = path;
    const wchar_t* ext = PathFindExtensionW(path.c_str());
    if (!_wcsnicmp(ext, L".do", 3)) return OpenDocx(*d) ? std::move(d) : nullptr;
    auto want = [](std::wstring_view n) -> size_t {
        if (!n.starts_with(L"ppt/") || !(n.ends_with(L".xml") || n.ends_with(L".rels"))) return 0;
        for (const wchar_t* skip : {L"ppt/notes", L"ppt/handout", L"ppt/charts", L"ppt/diagrams", L"ppt/embeddings", L"ppt/comments"})
            if (n.starts_with(skip)) return 0;
        return kMaxXml;
    };
    if (!ArchiveReadMembers(path, want, d->m)) return nullptr;
    Part& pres = d->Get(L"ppt/presentation.xml");
    const Node* p = Top(pres.root);
    if (!p || p->name != "presentation") return nullptr;
    if (const Node* sz = (*p)["sldSz"]) d->w = (float)sz->D("cx", 9144000) / 12700, d->h = (float)sz->D("cy", 5143500) / 12700;
    d->defaults = (*p)["defaultTextStyle"];
    if (const Node* ids = (*p)["sldIdLst"])
        for (auto& s : ids->kids)
            if (auto it = pres.rels.find(std::string(s.A("r:id"))); it != pres.rels.end()) d->slides.push_back(it->second);
    if (d->slides.empty() || d->w <= 0 || d->h <= 0) return nullptr;
    return d;
}

std::vector<std::pair<float, float>> PagesSizes(const Pages& d) { return {d.word ? d.doc.size() : d.slides.size(), {d.w, d.h}}; }

ComPtr<IWICBitmap> PagesRender(Pages& d, int index, UINT width, ID2D1Factory* d2d, IWICImagingFactory* wic) {
    if (index < 0 || (size_t)index >= (d.word ? d.doc.size() : d.slides.size()) || !width) return nullptr;
    UINT height = std::max(1u, (UINT)std::lround(width * d.h / d.w));
    ComPtr<IWICBitmap> bmp;
    ComPtr<ID2D1RenderTarget> rt;
    if (FAILED(wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp)) ||
        FAILED(d2d->CreateWicBitmapRenderTarget(
            bmp.Get(), D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
            &rt)))
        return nullptr;
    rt->SetDpi(96, 96);
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    float scale = width / d.w;
    if (d.word) {
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(D2D1::ColorF::White));
        rt->SetTransform(Matrix3x2F::Scale(scale, scale));
        DrawDoc(d, index, rt.Get(), wic, scale);
        return SUCCEEDED(rt->EndDraw()) ? bmp : nullptr;
    }
    Draw draw{d, rt.Get(), wic, scale, Matrix3x2F::Scale(scale, scale), &d.Get(d.slides[index])};
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(D2D1::ColorF::White));
    if (Top(draw.slide->root)) draw.Run();
    return SUCCEEDED(rt->EndDraw()) ? bmp : nullptr;
}
