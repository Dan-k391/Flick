// Pictures WIC can't open by file name: TGA, QOI, PNM/PAM, Radiance HDR and PCX decoded here; EMF/WMF played by
// GDI; CUR/ANI through WIC's ICO decoder; the preview PNG inside sketch/xmind/kra/ora zips. All as 32bpp PBGRA for
// the image view. Files are untrusted: every read is checked against the end, sizes are capped, and a truncated
// file decodes as far as it goes (the rest stays transparent).
#include "app.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <shlwapi.h>

using Microsoft::WRL::ComPtr;

extern const wchar_t kImgOwnExt[] =
    L" .tga .qoi .pbm .pgm .ppm .pnm .pam .hdr .pcx .emf .wmf .cur .ani .sketch .xmind .kra .ora ";

namespace {

constexpr UINT kMaxSide = 30000;
constexpr UINT64 kMaxPixels = 64 << 20;  // as the GIF canvas: 256 MB of pixels

struct Img {
    UINT w = 0, h = 0;
    std::vector<BYTE> px;  // BGRA, straight alpha (premultiplied by ToWic)
    bool Init(UINT64 W, UINT64 H) {
        if (!W || !H || W > kMaxSide || H > kMaxSide || W * H > kMaxPixels) return false;
        w = (UINT)W, h = (UINT)H;
        px.assign((size_t)W * H * 4, 0);
        return true;
    }
    void Set(UINT x, UINT y, BYTE r, BYTE g, BYTE b, BYTE a = 255) {
        BYTE* p = &px[((size_t)y * w + x) * 4];
        p[0] = b, p[1] = g, p[2] = r, p[3] = a;
    }
    void OpaqueIfNoAlpha() {  // 32-bit files written with an empty alpha channel
        for (size_t i = 3; i < px.size(); i += 4)
            if (px[i]) return;
        for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
    }
};

UINT U16(std::string_view f, size_t i) { return (BYTE)f[i] | (BYTE)f[i + 1] << 8; }
UINT32 Be32(std::string_view f, size_t i) {
    return (UINT32)(BYTE)f[i] << 24 | (BYTE)f[i + 1] << 16 | (BYTE)f[i + 2] << 8 | (BYTE)f[i + 3];
}

// ---- TGA: types 1, 2, 3 and their RLE forms 9, 10, 11; 8/15/16/24/32 bit; colour maps of 15/16/24/32 bit.
bool Tga(std::string_view f, Img& im) {
    if (f.size() < 18) return false;
    UINT idLen = (BYTE)f[0], cmType = (BYTE)f[1], type = (BYTE)f[2], cmFirst = U16(f, 3), cmLen = U16(f, 5),
         cmBits = (BYTE)f[7], w = U16(f, 12), h = U16(f, 14), bpp = (BYTE)f[16], desc = (BYTE)f[17];
    UINT base = type & 7;
    bool rle = type & 8;
    if ((type & ~11u) || base < 1 || base > 3 || cmType > 1) return false;
    if (base == 1 ? cmType != 1 || (bpp != 8 && bpp != 16)
                  : base == 2 ? bpp != 15 && bpp != 16 && bpp != 24 && bpp != 32 : bpp != 8 && bpp != 16)
        return false;
    bool alpha16 = (desc & 15) == 1;
    auto color = [&](const BYTE* p, UINT n, bool gray) -> UINT32 {  // -> 0xAARRGGBB
        if (gray) return n == 1 ? 0xFF000000u | p[0] * 0x010101u : (UINT32)p[1] << 24 | p[0] * 0x010101u;
        if (n == 2) {
            UINT v = p[0] | p[1] << 8;
            UINT r = (v >> 10 & 31) * 255 / 31, g = (v >> 5 & 31) * 255 / 31, b = (v & 31) * 255 / 31;
            return (alpha16 && !(v >> 15) ? 0u : 0xFF000000u) | r << 16 | g << 8 | b;
        }
        return (n == 4 ? (UINT32)p[3] << 24 : 0xFF000000u) | p[2] << 16 | p[1] << 8 | p[0];
    };
    size_t pos = 18 + idLen;
    std::vector<UINT32> pal;
    if (cmType == 1) {
        UINT eb = (cmBits + 7) / 8;
        if (base == 1) {
            if (eb < 2 || eb > 4 || pos + (size_t)cmLen * eb > f.size()) return false;
            for (UINT i = 0; i < cmLen; i++) pal.push_back(color((const BYTE*)f.data() + pos + (size_t)i * eb, eb, false));
        }
        pos += (size_t)cmLen * eb;
    }
    if (pos >= f.size() || !im.Init(w, h)) return false;
    UINT pb = (bpp + 7) / 8;
    auto pixel = [&](const BYTE* p) {
        if (base != 1) return color(p, pb, base == 3);
        UINT i = (pb == 1 ? p[0] : p[0] | p[1] << 8) - cmFirst;
        return i < pal.size() ? pal[i] : 0u;
    };
    UINT64 total = (UINT64)w * h, i = 0;
    auto put = [&](UINT32 c) {
        UINT x = (UINT)(i % w), y = (UINT)(i / w);
        if (!(desc & 0x20)) y = h - 1 - y;  // bottom-up unless the top-left origin bit is set
        if (desc & 0x10) x = w - 1 - x;
        im.Set(x, y, (BYTE)(c >> 16), (BYTE)(c >> 8), (BYTE)c, (BYTE)(c >> 24));
        i++;
    };
    const BYTE* d = (const BYTE*)f.data();
    while (i < total && pos < f.size()) {
        UINT64 count = 1;
        bool run = false;
        if (rle) count = (d[pos] & 127) + 1, run = d[pos] & 128, pos++;
        if (run) {
            if (pos + pb > f.size()) break;
            UINT32 c = pixel(d + pos);
            pos += pb;
            for (UINT64 k = 0; k < count && i < total; k++) put(c);  // a run past the end is cut
        } else {
            for (UINT64 k = 0; k < count && i < total && pos + pb <= f.size(); k++, pos += pb) put(pixel(d + pos));
            if (pos + pb > f.size()) break;
        }
    }
    if (bpp == 32 || bpp == 16) im.OpaqueIfNoAlpha();
    return true;
}

// ---- QOI (qoiformat.org)
bool Qoi(std::string_view f, Img& im) {
    if (f.size() < 14 || f.compare(0, 4, "qoif") || !im.Init(Be32(f, 4), Be32(f, 8))) return false;
    const BYTE* d = (const BYTE*)f.data();
    BYTE index[64][4] = {}, c[4] = {0, 0, 0, 255};  // r g b a
    size_t pos = 14, end = f.size();
    UINT run = 0;
    for (UINT64 i = 0, total = (UINT64)im.w * im.h; i < total; i++) {
        if (run) run--;
        else {
            if (pos >= end) break;
            BYTE b = d[pos++];
            if (b == 0xFE || b == 0xFF) {
                size_t n = b == 0xFE ? 3 : 4;
                if (pos + n > end) break;
                memcpy(c, d + pos, n), pos += n;
            } else if (b >> 6 == 0) memcpy(c, index[b], 4);
            else if (b >> 6 == 1) c[0] += (b >> 4 & 3) - 2, c[1] += (b >> 2 & 3) - 2, c[2] += (b & 3) - 2;
            else if (b >> 6 == 2) {
                if (pos >= end) break;
                BYTE b2 = d[pos++];
                int vg = (b & 63) - 32;
                c[0] += vg - 8 + (b2 >> 4), c[1] += vg, c[2] += vg - 8 + (b2 & 15);
            } else run = b & 63;
            memcpy(index[(c[0] * 3 + c[1] * 5 + c[2] * 7 + c[3] * 11) % 64], c, 4);
        }
        im.Set((UINT)(i % im.w), (UINT)(i / im.w), c[0], c[1], c[2], c[3]);
    }
    return true;
}

// ---- PNM: P1-P6 (plain and raw bitmaps, greymaps, pixmaps) and P7 (PAM, depth 1-4)
bool Pnm(std::string_view f, Img& im) {
    if (f.size() < 3 || f[0] != 'P' || f[1] < '1' || f[1] > '7') return false;
    int kind = f[1] - '0';
    size_t pos = 2;
    auto space = [&] {
        while (pos < f.size())
            if (f[pos] == '#') while (pos < f.size() && f[pos] != '\n') pos++;
            else if (strchr(" \t\r\n\v\f", f[pos])) pos++;
            else break;
    };
    auto number = [&](UINT64& v) {
        space();
        if (pos >= f.size() || f[pos] < '0' || f[pos] > '9') return false;
        for (v = 0; pos < f.size() && f[pos] >= '0' && f[pos] <= '9'; pos++)
            if ((v = v * 10 + (f[pos] - '0')) > 1u << 30) return false;
        return true;
    };
    UINT64 w = 0, h = 0, maxval = 1, depth = kind == 3 || kind == 6 ? 3 : 1;
    if (kind == 7) {  // "WIDTH 4\nHEIGHT 2\nDEPTH 3\nMAXVAL 255\nTUPLTYPE RGB\nENDHDR\n"
        depth = 0;
        for (;;) {
            space();
            size_t e = pos;
            while (e < f.size() && !strchr(" \t\r\n", f[e])) e++;
            std::string_view key = f.substr(pos, e - pos);
            pos = e;
            if (key.empty()) return false;
            if (key == "ENDHDR") break;
            UINT64* v = key == "WIDTH" ? &w : key == "HEIGHT" ? &h : key == "DEPTH" ? &depth : key == "MAXVAL" ? &maxval : nullptr;
            if (v && !number(*v)) return false;
            if (!v) while (pos < f.size() && f[pos] != '\n') pos++;  // TUPLTYPE and others
        }
        if (depth < 1 || depth > 4) return false;
    } else if (!number(w) || !number(h) || (kind != 1 && kind != 4 && !number(maxval)))
        return false;
    if (maxval < 1 || maxval > 65535) return false;
    if (pos < f.size() && kind != 1 && kind != 2 && kind != 3) pos++;  // the single whitespace before the raster
    if (!im.Init(w, h)) return false;
    bool plain = kind <= 3, bits = kind == 1 || kind == 4, wide = maxval > 255;
    UINT ch = (UINT)depth;
    auto sample = [&](UINT64& v) {  // false at the end of the data
        if (kind == 1) {  // digits need no separators
            space();
            if (pos >= f.size() || (f[pos] != '0' && f[pos] != '1')) return false;
            v = f[pos++] == '1';
            return true;
        }
        if (plain) return number(v) && (v = std::min(v, maxval), true);
        if (pos + (wide ? 2 : 1) > f.size()) return false;
        v = wide ? (UINT)(BYTE)f[pos] << 8 | (BYTE)f[pos + 1] : (BYTE)f[pos];
        pos += wide ? 2 : 1;
        v = std::min(v, maxval);
        return true;
    };
    for (UINT y = 0; y < im.h; y++) {
        if (kind == 4) {  // packed rows, 1 = black
            size_t row = ((size_t)im.w + 7) / 8;
            for (UINT x = 0; x < im.w && pos + x / 8 < f.size(); x++) {
                BYTE v = (f[pos + x / 8] >> (7 - x % 8) & 1) ? 0 : 255;
                im.Set(x, y, v, v, v);
            }
            if ((pos += row) > f.size()) return true;
            continue;
        }
        for (UINT x = 0; x < im.w; x++) {
            UINT64 s[4] = {0, 0, 0, maxval};
            for (UINT k = 0; k < ch; k++)
                if (!sample(s[k])) return true;  // truncated: what was read stays
            auto c8 = [&](UINT64 v) { return (BYTE)(v * 255 / maxval); };
            if (bits && kind != 7) {
                BYTE v = s[0] ? 0 : 255;  // PBM: 1 = black (PAM BLACKANDWHITE: 1 = white, as greys)
                im.Set(x, y, v, v, v);
            } else if (ch <= 2) im.Set(x, y, c8(s[0]), c8(s[0]), c8(s[0]), ch == 2 ? c8(s[1]) : 255);
            else im.Set(x, y, c8(s[0]), c8(s[1]), c8(s[2]), ch == 4 ? c8(s[3]) : 255);
        }
    }
    return true;
}

// ---- Radiance HDR (RGBE): flat or new-style RLE scanlines; shown clamped with gamma 2.2.
bool Hdr(std::string_view f, Img& im) {
    if (f.compare(0, 2, "#?")) return false;
    size_t pos = 0;
    auto line = [&] {
        size_t e = std::min(f.find('\n', pos), f.size());
        std::string_view l = f.substr(pos, e - pos);
        pos = std::min(e + 1, f.size());
        return l;
    };
    for (std::string_view l; !(l = line()).empty();) {
        if (pos >= f.size()) return false;
        if (l.starts_with("FORMAT=") && l != "FORMAT=32-bit_rle_rgbe") return false;  // XYZE
    }
    std::string res(line());
    char ys = 0, xs = 0;
    unsigned h = 0, w = 0;
    if (sscanf_s(res.c_str(), "%cY %u %cX %u", &ys, 1, &h, &xs, 1, &w) != 4 || xs != '+' || (ys != '-' && ys != '+') ||
        !im.Init(w, h))
        return false;
    std::vector<BYTE> row((size_t)w * 4);
    std::vector<BYTE> lut(256 * 256);  // exponent, mantissa -> shown value
    bool made[256] = {};
    const BYTE* d = (const BYTE*)f.data();
    for (UINT y = 0; y < h && pos < f.size(); y++) {
        bool rle = w >= 8 && w < 32768 && pos + 4 <= f.size() && d[pos] == 2 && d[pos + 1] == 2 && !(d[pos + 2] & 0x80);
        if (rle) {
            if ((UINT)(d[pos + 2] << 8 | d[pos + 3]) != w) break;
            pos += 4;
            for (UINT c = 0; c < 4; c++)
                for (UINT x = 0; x < w;) {
                    if (pos >= f.size()) goto done;
                    UINT n = d[pos++];
                    if (n > 128) {
                        n -= 128;
                        if (n > w - x || pos >= f.size()) goto done;
                        BYTE v = d[pos++];
                        for (; n; n--) row[(size_t)x++ * 4 + c] = v;
                    } else {
                        if (!n || n > w - x || pos + n > f.size()) goto done;  // 0 would never end
                        for (; n; n--) row[(size_t)x++ * 4 + c] = d[pos++];
                    }
                }
        } else {
            if (pos + row.size() > f.size()) break;
            memcpy(row.data(), d + pos, row.size()), pos += row.size();
        }
        for (UINT x = 0; x < w; x++) {
            const BYTE* p = &row[(size_t)x * 4];
            BYTE* t = &lut[(size_t)p[3] * 256];
            if (!made[p[3]]) {  // pow per value would be ~40 ns x 3 per pixel; a file uses few exponents
                double k = p[3] ? std::ldexp(1.0, p[3] - 136) : 0;
                for (int m = 0; m < 256; m++) t[m] = (BYTE)std::lround(std::pow(std::min(1.0, m * k), 1 / 2.2) * 255);
                made[p[3]] = true;
            }
            im.Set(x, ys == '-' ? y : h - 1 - y, t[p[0]], t[p[1]], t[p[2]]);
        }
    }
done:
    return true;
}

// ---- PCX: 1/2/4 bit with the header palette (1 bit, 1 plane: black and white), 1-bit planes (EGA), 8 bit with
// the 256-colour palette at the end, 24/32 bit as 3/4 planes. RLE runs may cross scanlines.
bool Pcx(std::string_view f, Img& im) {
    if (f.size() < 128 || f[0] != 0x0A || (f[2] != 0 && f[2] != 1)) return false;
    UINT bpp = (BYTE)f[3], planes = (BYTE)f[65], bpl = U16(f, 66), x0 = U16(f, 4), y0 = U16(f, 6), x1 = U16(f, 8),
         y1 = U16(f, 10);
    if (x1 < x0 || y1 < y0) return false;
    UINT w = x1 - x0 + 1, h = y1 - y0 + 1;
    bool ok = (bpp == 1 && planes >= 1 && planes <= 4) || ((bpp == 2 || bpp == 4) && planes == 1) ||
              (bpp == 8 && (planes == 1 || planes == 3 || planes == 4));
    if (!ok || bpl < ((size_t)w * bpp + 7) / 8 || bpl > ((size_t)w * bpp + 7) / 8 + 8 || !im.Init(w, h)) return false;
    const BYTE* d = (const BYTE*)f.data();
    UINT32 pal[256];
    for (UINT i = 0; i < 16; i++) pal[i] = d[16 + i * 3] << 16 | d[17 + i * 3] << 8 | d[18 + i * 3];
    if (bpp == 8 && planes == 1) {
        bool tail = f.size() >= 128 + 769 && d[f.size() - 769] == 0x0C;
        for (UINT i = 0; i < 256; i++)
            pal[i] = tail ? d[f.size() - 768 + i * 3] << 16 | d[f.size() - 767 + i * 3] << 8 | d[f.size() - 766 + i * 3]
                          : i * 0x010101u;
    }
    size_t end = bpp == 8 && planes == 1 && f.size() >= 128 + 769 && d[f.size() - 769] == 0x0C ? f.size() - 769 : f.size();
    std::vector<BYTE> line((size_t)planes * bpl);
    size_t pos = 128;
    UINT run = 0;
    BYTE value = 0;
    for (UINT y = 0; y < h; y++) {
        size_t k = 0;
        for (; k < line.size(); k++) {
            if (!run) {
                if (pos >= end) break;
                BYTE c = d[pos++];
                if (f[2] == 1 && (c & 0xC0) == 0xC0) {
                    if (pos >= end) break;
                    run = c & 63, value = d[pos++];
                    if (!run) { k--; continue; }  // a zero-length run: nothing
                } else run = 1, value = c;
            }
            line[k] = value, run--;
        }
        if (k < line.size()) break;  // truncated: the rows decoded so far
        for (UINT x = 0; x < w; x++) {
            if (bpp == 8 && planes >= 3) {
                im.Set(x, y, line[x], line[bpl + x], line[2 * bpl + x], planes == 4 ? line[3 * bpl + x] : 255);
                continue;
            }
            UINT i = 0;
            if (bpp == 1)
                for (UINT p = 0; p < planes; p++) i |= (line[p * bpl + x / 8] >> (7 - x % 8) & 1) << p;
            else if (bpp == 8) i = line[x];
            else i = line[x * bpp / 8] >> (8 - bpp - x * bpp % 8) & ((1 << bpp) - 1);
            UINT32 c = bpp == 1 && planes == 1 ? (i ? 0xFFFFFF : 0) : pal[i];
            im.Set(x, y, (BYTE)(c >> 16), (BYTE)(c >> 8), (BYTE)c);
        }
    }
    return true;
}

bool Decode(std::string_view f, const std::wstring& ext, Img& im) {
    bool ok = ext == L".tga" ? Tga(f, im) : ext == L".qoi" ? Qoi(f, im) : ext == L".hdr" ? Hdr(f, im)
            : ext == L".pcx" ? Pcx(f, im) : Pnm(f, im);
    // A header and no pixels (all transparent black): not a picture to show; the next loader in line tries.
    return ok && std::any_of(im.px.begin(), im.px.end(), [](BYTE b) { return b != 0; });
}

ComPtr<IWICBitmap> ToWic(Img& im, IWICImagingFactory* wic) {
    for (size_t i = 0; i < im.px.size(); i += 4)
        if (BYTE a = im.px[i + 3]; a < 255)
            for (int c = 0; c < 3; c++) im.px[i + c] = (BYTE)(im.px[i + c] * a / 255);
    ComPtr<IWICBitmap> b;
    wic->CreateBitmapFromMemory(im.w, im.h, GUID_WICPixelFormat32bppPBGRA, im.w * 4, (UINT)im.px.size(), im.px.data(), &b);
    return b;
}

bool ReadAll(const std::wstring& path, std::string& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    out.resize((size_t)std::min<LONGLONG>(size.QuadPart, 320 << 20));  // > 64 MP of 32-bit pixels + headers
    DWORD read = 0;
    BOOL ok = ReadFile(f, out.data(), (DWORD)out.size(), &read, nullptr);
    CloseHandle(f);
    out.resize(read);
    return ok;
}

// Any WIC format from memory (the stream reads the bytes in place: decoded before they go).
ComPtr<IWICBitmap> FromBytes(std::string_view bytes, IWICImagingFactory* wic, const GUID* container = nullptr) {
    ComPtr<IWICStream> st;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> cv;
    ComPtr<IWICBitmap> out;
    if (FAILED(wic->CreateStream(&st)) || FAILED(st->InitializeFromMemory((BYTE*)bytes.data(), (DWORD)bytes.size())))
        return nullptr;
    if (container ? FAILED(wic->CreateDecoder(*container, nullptr, &dec)) || FAILED(dec->Initialize(st.Get(), WICDecodeMetadataCacheOnDemand))
                  : FAILED(wic->CreateDecoderFromStream(st.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec)))
        return nullptr;
    UINT frames = 0, best = 0;
    UINT64 most = 0;
    dec->GetFrameCount(&frames);
    for (UINT i = 0; i < frames && container; i++)  // icons: the largest image
        if (UINT w, h; SUCCEEDED(dec->GetFrame(i, &frame)) && SUCCEEDED(frame->GetSize(&w, &h)) && (UINT64)w * h > most)
            most = (UINT64)w * h, best = i;
    if (FAILED(dec->GetFrame(best, &frame)) || FAILED(wic->CreateFormatConverter(&cv)) ||
        FAILED(cv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) ||
        FAILED(wic->CreateBitmapFromSource(cv.Get(), WICBitmapCacheOnLoad, &out)))
        return nullptr;
    return out;
}

// CUR (or ICO) bytes: WIC's ICO decoder wants type 1, a cursor says 2 (the rest of the layout is the same).
ComPtr<IWICBitmap> Icon(std::string f, IWICImagingFactory* wic) {
    if (f.size() < 6 || U16(f, 0) != 0 || (U16(f, 2) != 1 && U16(f, 2) != 2)) return nullptr;
    f[2] = 1;
    return FromBytes(f, wic, &GUID_ContainerFormatIco);
}

// ANI: RIFF "ACON"; the first "icon" chunk of the LIST "fram" is a CUR/ICO. Only that first frame is shown.
ComPtr<IWICBitmap> Ani(std::string_view f, IWICImagingFactory* wic) {
    if (f.size() < 12 || f.compare(0, 4, "RIFF") || f.compare(8, 4, "ACON")) return nullptr;
    auto u32 = [&](size_t i) { return (UINT32)U16(f, i) | (UINT32)U16(f, i + 2) << 16; };
    for (size_t pos = 12; pos + 8 <= f.size();) {
        UINT32 n = u32(pos + 4);
        size_t body = pos + 8, end = body + std::min<size_t>(n, f.size() - body);
        if (!f.compare(pos, 4, "LIST") && end - body >= 4 && !f.compare(body, 4, "fram"))
            for (size_t q = body + 4; q + 8 <= end;) {
                UINT32 m = u32(q + 4);
                if (!f.compare(q, 4, "icon")) return Icon(std::string(f.substr(q + 8, std::min<size_t>(m, end - q - 8))), wic);
                q += 8 + (size_t)m + (m & 1);
            }
        pos = body + (size_t)n + (n & 1);
    }
    return nullptr;
}

// EMF, or WMF (with or without the placeable header), played by GDI over white at 96 DPI x scale.
ComPtr<IWICBitmap> Metafile(std::string_view f, float scale, IWICImagingFactory* wic) {
    HENHMETAFILE emf = nullptr;
    auto* eh = (const ENHMETAHEADER*)f.data();
    if (f.size() >= sizeof(ENHMETAHEADER) && eh->iType == EMR_HEADER && eh->dSignature == ENHMETA_SIGNATURE)
        emf = SetEnhMetaFileBits((UINT)f.size(), (const BYTE*)f.data());
    else if (f.size() >= 22 + 18 && U16(f, 0) == 0xCDD7 && U16(f, 2) == 0x9AC6) {  // placeable: bounds in units per inch
        int inch = (int)U16(f, 14), l = (short)U16(f, 6), t = (short)U16(f, 8), r = (short)U16(f, 10), b = (short)U16(f, 12);
        if (inch <= 0 || r <= l || b <= t) return nullptr;
        METAFILEPICT mp{MM_ANISOTROPIC, MulDiv(r - l, 2540, inch), MulDiv(b - t, 2540, inch), nullptr};  // HIMETRIC
        emf = SetWinMetaFileBits((UINT)f.size() - 22, (const BYTE*)f.data() + 22, nullptr, &mp);
    } else if (f.size() >= 18 && (U16(f, 0) == 1 || U16(f, 0) == 2) && U16(f, 2) == 9) {
        // No size in the file: its first SetWindowExt, taken as 96 DPI pixels (without a size GDI fills the screen).
        METAFILEPICT mp{MM_ANISOTROPIC, 0, 0, nullptr};
        for (size_t pos = 18; pos + 6 <= f.size();) {
            UINT32 words = U16(f, pos) | (UINT32)U16(f, pos + 2) << 16;
            UINT fn = U16(f, pos + 4);
            if (fn == 0x020C && pos + 10 <= f.size()) {  // META_SETWINDOWEXT: y, x
                mp.xExt = MulDiv(abs((short)U16(f, pos + 8)), 2540, 96), mp.yExt = MulDiv(abs((short)U16(f, pos + 6)), 2540, 96);
                break;
            }
            if (words < 3 || !fn) break;  // malformed, or the end record
            pos += (size_t)words * 2;
        }
        emf = SetWinMetaFileBits((UINT)f.size(), (const BYTE*)f.data(), nullptr, mp.xExt && mp.yExt ? &mp : nullptr);
    }
    if (!emf) return nullptr;
    ENHMETAHEADER hd{};
    GetEnhMetaFileHeader(emf, sizeof hd, &hd);
    double w = (hd.rclFrame.right - hd.rclFrame.left) * 96 / 2540.0, h = (hd.rclFrame.bottom - hd.rclFrame.top) * 96 / 2540.0;
    if (w < 1 || h < 1) w = hd.rclBounds.right - hd.rclBounds.left + 1.0, h = hd.rclBounds.bottom - hd.rclBounds.top + 1.0;
    double k = std::min({(double)scale, 8192 / std::max(1.0, w), 8192 / std::max(1.0, h)});
    int pw = (int)std::lround(w * k), ph = (int)std::lround(h * k);
    ComPtr<IWICBitmap> out;
    if (pw >= 1 && ph >= 1 && (UINT64)pw * ph <= kMaxPixels) {
        BITMAPINFO bi{{sizeof(BITMAPINFOHEADER), pw, -ph, 1, 32, BI_RGB}};
        void* bits = nullptr;
        HDC dc = CreateCompatibleDC(nullptr);
        if (HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0)) {
            HGDIOBJ old = SelectObject(dc, dib);
            PatBlt(dc, 0, 0, pw, ph, WHITENESS);
            RECT rc{0, 0, pw, ph};
            PlayEnhMetaFile(dc, emf, &rc);
            GdiFlush();
            BYTE* p = (BYTE*)bits;
            for (size_t i = 3; i < (size_t)pw * ph * 4; i += 4) p[i] = 255;  // GDI leaves alpha 0
            wic->CreateBitmapFromMemory(pw, ph, GUID_WICPixelFormat32bppPBGRA, pw * 4, (UINT)((size_t)pw * ph * 4), p, &out);
            SelectObject(dc, old);
            DeleteObject(dib);
        }
        DeleteDC(dc);
    }
    DeleteEnhMetaFile(emf);
    return out;
}

// The preview picture a zip-based format keeps (first one found in this order).
ComPtr<IWICBitmap> ZipPreview(const std::wstring& path, std::initializer_list<const wchar_t*> names, IWICImagingFactory* wic) {
    std::vector<std::pair<std::wstring, std::string>> m;
    ArchiveReadMembers(path, [&](std::wstring_view n) -> size_t {
        for (auto* k : names)
            if (n == k) return 64 << 20;
        return 0;
    }, m);
    for (auto* k : names)
        for (auto& [n, d] : m)
            if (n == k)
                if (auto b = FromBytes(d, wic)) return b;
    return nullptr;
}

}  // namespace

ComPtr<IWICBitmap> ImgLoad(const std::wstring& path, float scale, bool& vector) {
    IWICImagingFactory* wic = WicFactory();
    std::wstring ext = PathFindExtensionW(path.c_str());
    for (auto& c : ext) c = towlower(c);
    vector = false;
    if (ext == L".sketch") return ZipPreview(path, {L"previews/preview.png"}, wic);
    if (ext == L".xmind") return ZipPreview(path, {L"Thumbnails/thumbnail.png"}, wic);
    if (ext == L".kra") return ZipPreview(path, {L"mergedimage.png", L"preview.png"}, wic);
    if (ext == L".ora") return ZipPreview(path, {L"mergedimage.png", L"Thumbnails/thumbnail.png"}, wic);
    std::string f;
    if (!ReadAll(path, f)) return nullptr;
    if (ext == L".cur") return Icon(std::move(f), wic);
    if (ext == L".ani") return Ani(f, wic);
    if (ext == L".emf" || ext == L".wmf") return vector = true, Metafile(f, scale, wic);
    Img im;
    return Decode(f, ext, im) ? ToWic(im, wic) : nullptr;
}

std::wstring ImgSelfTest() {
    using namespace std::string_literals;
    auto px = [](const Img& im, UINT x, UINT y) {  // 0xAARRGGBB
        const BYTE* p = &im.px[((size_t)y * im.w + x) * 4];
        return (UINT32)p[3] << 24 | p[2] << 16 | p[1] << 8 | p[0];
    };
    auto dec = [](const std::string& f, const wchar_t* ext, Img& im) { return Decode(f, ext, im = Img{}); };
    Img im;
    auto tga = [](int type, int w, int h, int bpp, int desc, const std::string& body, const std::string& cmap = "",
                  int cmBits = 0) {
        std::string s(18, '\0');
        s[1] = !cmap.empty(), s[2] = (char)type, s[5] = (char)(cmap.size() / std::max(1, cmBits / 8)), s[7] = (char)cmBits;
        s[12] = (char)w, s[13] = (char)(w >> 8), s[14] = (char)h, s[15] = (char)(h >> 8), s[16] = (char)bpp, s[17] = (char)desc;
        return s + cmap + body;
    };
    // TGA 24-bit 2x2, bottom-up: the file's first row is the picture's last.
    if (!dec(tga(2, 2, 2, 24, 0, "\x00\x00\xFF\x00\xFF\x00\xFF\x00\x00\xFF\xFF\xFF"s), L".tga", im) || im.w != 2 ||
        px(im, 0, 1) != 0xFFFF0000 || px(im, 1, 1) != 0xFF00FF00 || px(im, 0, 0) != 0xFF0000FF || px(im, 1, 0) != 0xFFFFFFFF)
        return L"tga 24";
    // RLE, top-left origin: a run longer than the picture is cut; 32-bit with empty alpha shows opaque.
    if (!dec(tga(10, 2, 1, 32, 0x20, "\x85\x10\x20\x30\x00"s), L".tga", im) || px(im, 1, 0) != 0xFF302010) return L"tga rle";
    // Colour-mapped, RLE raw packet running past the end of the data.
    if (!dec(tga(9, 3, 1, 8, 0x20, "\x02\x01\x00"s, "\x00\x00\xFF\x00\xFF\x00"s, 24), L".tga", im) ||
        px(im, 0, 0) != 0xFF00FF00 || px(im, 1, 0) != 0xFFFF0000 || px(im, 2, 0) != 0)
        return L"tga map";
    if (dec(tga(2, 0, 0, 24, 0, "x"), L".tga", im) || dec(tga(2, 40000, 1, 24, 0, "x"), L".tga", im) ||
        dec(tga(2, 2, 2, 24, 0, ""), L".tga", im) || dec("", L".tga", im) || dec(tga(7, 1, 1, 24, 0, "abc"), L".tga", im))
        return L"tga bad";
    // QOI 3x1: an RGB pixel, a run of 2; a truncated file (header only) decodes nothing: no picture.
    std::string qoi = "qoif\0\0\0\x03\0\0\0\x01\x04\x00\xFE\x11\x22\x33\xC1"s + "\0\0\0\0\0\0\0\x01"s;
    if (!dec(qoi, L".qoi", im) || im.w != 3 || px(im, 2, 0) != 0xFF112233) return L"qoi";
    if (dec(qoi.substr(0, 14), L".qoi", im) || dec("qoif\0\0\x80\0\0\0\x80\0\4\0"s, L".qoi", im))
        return L"qoi bad";
    // PNM: plain pixmap, plain bitmap (unseparated digits, comment), 16-bit raw greymap, truncated raw pixmap, PAM.
    if (!dec("P3\n# c\n2 1\n255\n255 0 0  0 255 0\n", L".ppm", im) || px(im, 0, 0) != 0xFFFF0000 || px(im, 1, 0) != 0xFF00FF00)
        return L"ppm plain";
    if (!dec("P1 2 1 #x\n01", L".pbm", im) || px(im, 0, 0) != 0xFFFFFFFF || px(im, 1, 0) != 0xFF000000) return L"pbm";
    if (!dec("P5 2 1 65535\n\xFF\xFF\x00\x00"s, L".pgm", im) || px(im, 0, 0) != 0xFFFFFFFF || px(im, 1, 0) != 0xFF000000)
        return L"pgm 16";
    if (!dec("P6 2 2 255\n\xFF\x00\x00\x00"s, L".ppm", im) || px(im, 0, 0) != 0xFFFF0000 || px(im, 1, 0) != 0) return L"ppm cut";
    if (!dec("P7\nWIDTH 1\nHEIGHT 1\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n\x01\x02\x03\x80"s, L".pam", im) ||
        px(im, 0, 0) != 0x80010203)
        return L"pam";
    if (dec("P6 99999 99999 255\n", L".ppm", im) || dec("P6 2", L".ppm", im) || dec("P2 1 1 0 0", L".pgm", im)) return L"pnm bad";
    // HDR: flat (mantissa 128, exponent 129 = 1.0 -> 255; 64 -> 0.5 -> gamma 2.2 -> 186), then RLE (w 8) with a
    // zero-length literal that must stop decoding.
    std::string hdr = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 2\n";
    if (!dec(hdr + "\x80\x40\x00\x81\x00\x00\x00\x00"s, L".hdr", im) || px(im, 0, 0) != 0xFFFFBA00 || px(im, 1, 0) != 0xFF000000)
        return L"hdr flat";
    std::string hdr8 = "#?RGBE\n\n-Y 2 +X 8\n\x02\x02\x00\x08\x88\x80\x88\x40\x88\x00\x88\x81"s + "\x02\x02\x00\x08\x00"s;
    if (!dec(hdr8, L".hdr", im) || px(im, 7, 0) != 0xFFFFBA00 || px(im, 0, 1) != 0) return L"hdr rle";
    if (dec("#?RADIANCE\n\n+X 2 -Y 1\n", L".hdr", im) || dec("#?RADIANCE\n", L".hdr", im)) return L"hdr bad";
    // PCX 24-bit 2x1 (3 planes, 2 bytes per line): runs crossing planes, a value >= 0xC0 as a run of one.
    std::string pcx(128, '\0');
    pcx[0] = 0x0A, pcx[1] = 5, pcx[2] = 1, pcx[3] = 8, pcx[8] = 1, pcx[65] = 3, pcx[66] = 2;
    if (!dec(pcx + "\xC3\x10\xC1\xC5\x30\x40"s, L".pcx", im) || px(im, 0, 0) != 0xFF101030 || px(im, 1, 0) != 0xFF10C540)
        return L"pcx 24";
    if (dec(pcx + "\xC3"s, L".pcx", im)) return L"pcx cut";
    // EGA: 1 bit x 4 planes, index 5 (planes 0 and 2) from the header palette.
    std::string ega(128, '\0');
    ega[0] = 0x0A, ega[1] = 5, ega[2] = 1, ega[3] = 1, ega[65] = 4, ega[66] = 1, ega[31] = 0x11, ega[32] = 0x22, ega[33] = 0x33;
    if (!dec(ega + "\x80\x00\x80\x00"s, L".pcx", im) || im.w != 1 || px(im, 0, 0) != 0xFF112233) return L"pcx ega";
    pcx[66] = (char)200;
    if (dec(pcx, L".pcx", im)) return L"pcx bad";
    return L"";
}
