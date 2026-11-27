// pptx slides drawn for the PDF view. Slides: shapes, text, pictures, tables and backgrounds, with what a
// slide inherits from its layout, master and theme (positions, text styles, colours, fonts). Opened on the PDF
// worker (zip + XML, a few ms); each page is drawn with Direct2D + DirectWrite into a WIC bitmap at the width
// the view asks for, so slides and their thumbnails take the same path as PDF pages.
// shortcut: no charts, SmartArt, effects (shadows, glow), vertical text, EMF/WMF pictures or group rotation;
// uncommon preset shapes are drawn as rectangles and every table gets the default table style's look.
#include "app.h"
#include "xml.h"
#include <shlwapi.h>
#include <cmath>
#include <unordered_set>

using Microsoft::WRL::ComPtr;
using D2D1::Matrix3x2F;

namespace {

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


void PagesFree::operator()(Pages* p) const { delete p; }

bool PagesFor(const std::wstring& path) {
    const wchar_t* e = PathFindExtensionW(path.c_str());
    for (const wchar_t* x : {L".pptx", L".pptm", L".potx", L".potm", L".ppsx", L".ppsm"})
        if (!_wcsicmp(e, x)) return true;
    return false;
}

PagesPtr PagesOpen(const std::wstring& path) {
    PagesPtr d(new Pages);
    d->path = path;
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

std::vector<std::pair<float, float>> PagesSizes(const Pages& d) { return {d.slides.size(), {d.w, d.h}}; }

ComPtr<IWICBitmap> PagesRender(Pages& d, int index, UINT width, ID2D1Factory* d2d, IWICImagingFactory* wic) {
    if (index < 0 || (size_t)index >= d.slides.size() || !width) return nullptr;
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
    Draw draw{d, rt.Get(), wic, scale, Matrix3x2F::Scale(scale, scale), &d.Get(d.slides[index])};
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(D2D1::ColorF::White));
    if (Top(draw.slide->root)) draw.Run();
    return SUCCEEDED(rt->EndDraw()) ? bmp : nullptr;
}
