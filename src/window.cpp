// The preview window: custom Mica title bar drawn with Direct2D; content is Scintilla (text),
// a WIC-decoded Direct2D bitmap (images) or a shell thumbnail + file info (everything else).
// Created once at startup and only shown/hidden afterwards, so opening a preview is cheap.
#include "app.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <uxtheme.h>
#include <shellscalingapi.h>
#include <propvarutil.h>
#include <propkey.h>
#include <mfmediaengine.h>
#include <windowsx.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <unordered_map>
#include <unordered_set>
#include <cmath>
#include <vector>
#include <Scintilla.h>
#include <ILexer.h>
#include <Lexilla.h>

using Microsoft::WRL::ComPtr;

extern "C" int Scintilla_RegisterClasses(void* hInstance);

HWND g_hwnd;

namespace {

constexpr float kTitleH = 40, kPad = 16, kBtnH = 28, kCloseW = 46, kThumb = 256;
constexpr size_t kMaxText = 2 << 20;  // larger files show their first 2 MB (SCI_SETTEXT ~2 ms/MB)

enum Kind { K_None, K_Text, K_Image, K_Info, K_Md, K_Pdf, K_Grid, K_Font, K_Media };
enum Hot { H_None, H_Open, H_Close };

struct Palette {
    COLORREF role[R_Count];
    COLORREF bg, lineFg, sel;
    D2D1_COLOR_F text, sub, btn, btnHot, btnBorder;
    D2D1_COLOR_F link, codeBg, rule;  // Markdown
};
constexpr COLORREF Hex(unsigned rgb) { return RGB(rgb >> 16, (rgb >> 8) & 0xFF, rgb & 0xFF); }
constexpr D2D1_COLOR_F C(unsigned rgb, float a = 1) {
    return {(rgb >> 16) / 255.f, ((rgb >> 8) & 0xFF) / 255.f, (rgb & 0xFF) / 255.f, a};
}
const Palette kDark{
    {Hex(0xD4D4D4), Hex(0x6A9955), Hex(0x569CD6), Hex(0x4EC9B0), Hex(0xDCDCAA), Hex(0xCE9178), Hex(0xB5CEA8),
     Hex(0xC586C0), Hex(0xD4D4D4), Hex(0x569CD6), Hex(0x9CDCFE), Hex(0x569CD6), Hex(0xD4D4D4), Hex(0xD4D4D4),
     Hex(0xF44747), Hex(0x81B88B), Hex(0xF14C4C)},
    Hex(0x1C1C1C), Hex(0x6E7681), Hex(0x264F78),
    C(0xFFFFFF), C(0xFFFFFF, 0.55f), C(0xFFFFFF, 0.06f), C(0xFFFFFF, 0.10f), C(0xFFFFFF, 0.08f),
    C(0x4CC2FF), C(0xFFFFFF, 0.07f), C(0xFFFFFF, 0.13f)};
const Palette kLight{
    {Hex(0x1F1F1F), Hex(0x008000), Hex(0x0000FF), Hex(0x267F99), Hex(0x795E26), Hex(0xA31515), Hex(0x098658),
     Hex(0xAF00DB), Hex(0x1F1F1F), Hex(0x800000), Hex(0x0451A5), Hex(0x0000FF), Hex(0x1F1F1F), Hex(0x1F1F1F),
     Hex(0xCD3131), Hex(0x098658), Hex(0xA31515)},
    Hex(0xFFFFFF), Hex(0x8C8C8C), Hex(0xADD6FF),
    C(0x1A1A1A), C(0x000000, 0.55f), C(0xFFFFFF, 0.7f), C(0xF9F9F9, 0.5f), C(0x000000, 0.08f),
    C(0x005FB8), C(0x000000, 0.05f), C(0x000000, 0.12f)};

HWND g_sci, g_source;
SciFnDirect g_sciFn;
sptr_t g_sciPtr;
ComPtr<ID2D1Factory> g_d2d;
ComPtr<IDWriteFactory> g_dw;
ComPtr<IWICImagingFactory> g_wic;
ComPtr<ID2D1HwndRenderTarget> g_rt;
ComPtr<IDWriteTextFormat> g_fmtTitle, g_fmtIcon, g_fmtName, g_fmtInfo;
std::string g_codeFont;
UINT g_dpi = 96;
bool g_dark, g_placing;
std::atomic<bool> g_visible;
float g_natW, g_natH;   // size a picture is shown at, in DIPs: 1 image pixel = 1 DIP, like QuickLook
bool g_decodeForClient;  // re-decoding after a manual resize: fit the current window, not the screen
void PlaceWindow();

Kind g_kind;
const Lang* g_lang;
std::wstring g_path, g_title, g_sub, g_open;
std::vector<std::wstring> g_info;
ComPtr<ID2D1Bitmap> g_bmp;
UINT g_orient = 1;  // EXIF orientation
bool g_imgReduced;  // image decoded below full size to fit the window
Hot g_hot;
D2D1_RECT_F g_openRc, g_closeRc;

// Scrollable documents (Markdown, PDF, tables), in DIPs. Index 0 = horizontal, 1 = vertical,
// 2 = the sidebar.
float g_scroll[3], g_doc[3];  // g_doc[0] = 0: nothing scrolls sideways
bool g_sideFocus;             // last click was in the sidebar: arrow keys step through its items
bool g_sizing;  // inside a move/size loop: don't re-render PDF pages for every intermediate width
constexpr float kBarHit = 14;  // thickness of a scrollbar's mouse area along the window edge
int g_dragAxis = -1;           // scrollbar being dragged
float g_dragGrab;              // mouse offset within the dragged thumb
int g_barHot = -1;             // scrollbar under the mouse

// Audio/video: the frame shown, the control bar's hit areas.
constexpr wchar_t kVideoExt[] = L" .mp4 .m4v .mov .mkv .avi .wmv .webm .mpg .mpeg .m2ts .mts .ts .3gp .3g2 .asf ";
constexpr wchar_t kAudioExt[] = L" .mp3 .m4a .m4b .aac .wav .flac .wma .ogg .oga .opus .aiff .aif .amr .ac3 .ec3 .mka .weba ";
bool g_mediaVideo, g_mouseIn, g_seekDrag, g_volDrag;
LONGLONG g_mediaT0;
int g_posterGen;
bool g_posterDone;      // the poster worker answered (with or without a frame)
UINT g_posterW, g_posterH;  // size of the poster frame, until the engine knows the video size
ComPtr<IWICBitmap> g_frameWic;
ComPtr<ID2D1Bitmap> g_frame;
D2D1_RECT_F g_playRc, g_trackRc, g_muteRc, g_volRc;
double g_shownPos = -1;
float g_seekFrac = -1;  // while dragging the seek bar: where the knob is (0..1)
bool g_seekSent = true;  // that position has been sent to the engine
ComPtr<IDWriteTextFormat> g_fmtGlyph;  // media control glyphs

// Archives are listed on a worker thread (WM_FLICK_GRID).
std::atomic<int> g_gridGen;
std::atomic<int> g_sizeGen;  // folder-size worker
bool g_gridFolder;           // the tree being listed is a folder (its size gets counted)
bool g_gridPending;

// Sidebar for Markdown (contents) and PDF (page thumbnails), when the window is wide enough.
constexpr float kSideW = 160, kSideMinWindow = 700, kThumbW = 120, kHeadRowH = 28, kThumbLabel = 22;
int g_sideCurrent = -1;  // item the reading position is in, last time it was drawn
std::vector<ComPtr<ID2D1Bitmap>> g_thumbBmp;
std::vector<UINT> g_thumbW;  // pixel width each thumbnail was rendered at (0 = never)
bool g_pdfReqThumb;          // the page the worker is rendering is a thumbnail

// PDF: page sizes arrive from the worker (pdf.cpp), then pages as they are rendered.
constexpr float kPageGap = 12;
int g_pdfGen;
bool g_pdfIdle = true, g_pdfFailed;
LONGLONG g_pdfT0;
std::vector<std::pair<float, float>> g_pages;  // points
std::vector<ComPtr<ID2D1Bitmap>> g_pageBmp;
std::vector<UINT> g_pageW;  // pixel width each page was last rendered at (0 = never)

bool g_warming = true;
void Cloak(BOOL on) { DwmSetWindowAttribute(g_hwnd, DWMWA_CLOAK, &on, sizeof on); }

const Palette& Pal() { return g_dark ? kDark : kLight; }
sptr_t Sci(UINT m, uptr_t w = 0, sptr_t l = 0) { return g_sciFn(g_sciPtr, m, w, l); }
float Dip(int px) { return px * 96.f / g_dpi; }

bool ReadDarkMode() {
    DWORD v = 1, sz = sizeof v;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v == 0;
}

void StyleSci() {
    const Palette& p = Pal();
    Sci(SCI_STYLERESETDEFAULT);
    Sci(SCI_STYLESETFONT, STYLE_DEFAULT, (sptr_t)g_codeFont.c_str());
    Sci(SCI_STYLESETSIZEFRACTIONAL, STYLE_DEFAULT, 1000);
    Sci(SCI_STYLESETFORE, STYLE_DEFAULT, p.role[R_Default]);
    Sci(SCI_STYLESETBACK, STYLE_DEFAULT, p.bg);
    Sci(SCI_STYLECLEARALL);
    Sci(SCI_STYLESETFORE, STYLE_LINENUMBER, p.lineFg);
    Sci(SCI_STYLESETBACK, STYLE_LINENUMBER, p.bg);
    if (g_lang)
        for (size_t i = 0; i < g_lang->styleCount; i++) {
            auto [style, role] = g_lang->styles[i];
            Sci(SCI_STYLESETFORE, style, p.role[role]);
            if (role == R_Heading || role == R_Strong) Sci(SCI_STYLESETBOLD, style, 1);
            if (role == R_Em) Sci(SCI_STYLESETITALIC, style, 1);
        }
    Sci(SCI_SETMARGINRIGHT, 0, MulDiv(12, g_dpi, 96));  // gap between line numbers and text
    Sci(SCI_SETELEMENTCOLOUR, SC_ELEMENT_SELECTION_BACK, p.sel | 0xFF000000);
    Sci(SCI_SETELEMENTCOLOUR, SC_ELEMENT_SELECTION_INACTIVE_BACK, p.sel | 0xFF000000);
    Sci(SCI_SETELEMENTCOLOUR, SC_ELEMENT_CARET, p.role[R_Default] | 0xFF000000);
}

void ApplyTheme() {
    g_dark = ReadDarkMode();
    BOOL dark = g_dark;
    DwmSetWindowAttribute(g_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof dark);
    SetWindowTheme(g_sci, g_dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);  // dark scrollbars
    StyleSci();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

void CreateTarget() {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    g_d2d->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                     D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
        D2D1::HwndRenderTargetProperties(g_hwnd, D2D1::SizeU(rc.right, rc.bottom), D2D1_PRESENT_OPTIONS_IMMEDIATELY),
        &g_rt);
    g_rt->SetDpi((float)g_dpi, (float)g_dpi);
}

// D2D bitmap at the window DPI, so GetSize() in DIPs maps 1 bitmap pixel to 1 screen pixel.
// Shares the pixels of a WIC bitmap instead of copying them: the software render target never gives
// the memory of its own bitmaps back to the OS (~50 MB stayed private after a few PDF pages).
ComPtr<ID2D1Bitmap> ToD2D(IWICBitmapSource* src) {
    ComPtr<IWICFormatConverter> conv;
    ComPtr<IWICBitmap> pixels;
    ComPtr<IWICBitmapLock> lock;
    ComPtr<ID2D1Bitmap> bmp;
    UINT w, h;
    if (SUCCEEDED(g_wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(g_wic->CreateBitmapFromSource(conv.Get(), WICBitmapCacheOnLoad, &pixels)) &&
        SUCCEEDED(pixels->GetSize(&w, &h))) {
        WICRect all{0, 0, (INT)w, (INT)h};
        if (SUCCEEDED(pixels->Lock(&all, WICBitmapLockRead, &lock))) {
            D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), (float)g_dpi, (float)g_dpi);
            g_rt->CreateSharedBitmap(__uuidof(IWICBitmapLock), lock.Get(), &props, &bmp);
        }
    }
    return bmp;
}

// Pixels available for content: 80% of the screen's work area (QuickLook's image rule) minus the
// window chrome, or the current content area when re-decoding after the user resized the window.
void PictureArea(double& w, double& h) {
    RECT rc;
    if (g_decodeForClient) {
        GetClientRect(g_hwnd, &rc);
        w = rc.right, h = rc.bottom - kTitleH * g_dpi / 96;
    } else {
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        rc = mi.rcWork;
        w = (rc.right - rc.left) * 0.8, h = (rc.bottom - rc.top) * 0.8 - kTitleH * g_dpi / 96;
    }
    w = std::max(64.0, w), h = std::max(64.0, h);
}

bool LoadImage(const std::wstring& path) {
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(g_wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                &dec)) ||
        FAILED(dec->GetFrame(0, &frame)))
        return false;
    UINT w, h;
    frame->GetSize(&w, &h);
    g_orient = 1;
    ComPtr<IWICMetadataQueryReader> meta;
    PROPVARIANT pv;
    PropVariantInit(&pv);
    if (SUCCEEDED(frame->GetMetadataQueryReader(&meta)) &&
        SUCCEEDED(meta->GetMetadataByName(L"System.Photo.Orientation", &pv)) && pv.vt == VT_UI2 && pv.uiVal <= 8)
        g_orient = pv.uiVal;
    PropVariantClear(&pv);
    g_natW = (float)(g_orient >= 5 ? h : w), g_natH = (float)(g_orient >= 5 ? w : h);

    // Decode no more pixels than the window shows; WM_EXITSIZEMOVE re-decodes after a resize.
    double maxW, maxH;
    PictureArea(maxW, maxH);
    if (g_orient >= 5) std::swap(maxW, maxH);
    double s = std::min({1.0, maxW / w, maxH / h});
    g_imgReduced = s < 1;
    ComPtr<IWICBitmapSource> src = frame;
    if (s < 1) {
        UINT sw = std::max(1u, (UINT)(w * s)), sh = std::max(1u, (UINT)(h * s));
        // JPEG (and some other codecs) can decode straight to 1/2, 1/4, 1/8 size: far cheaper than a
        // full decode followed by resampling. Use the smallest native size that is still >= target.
        ComPtr<IWICBitmapSourceTransform> xf;
        if (SUCCEEDED(frame.As(&xf))) {
            UINT cw = w, ch = h;
            for (UINT d = 2; d <= 8; d *= 2) {
                UINT tw = (w + d - 1) / d, th = (h + d - 1) / d;
                if (tw < sw || th < sh) break;
                UINT qw = tw, qh = th;
                if (SUCCEEDED(xf->GetClosestSize(&qw, &qh)) && qw == tw && qh == th) cw = tw, ch = th;
            }
            WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
            ComPtr<IWICBitmap> reduced;
            ComPtr<IWICBitmapLock> lock;
            UINT stride = 0, size = 0;
            BYTE* px = nullptr;
            WICRect all{0, 0, (INT)cw, (INT)ch};
            if (cw < w && SUCCEEDED(xf->GetClosestPixelFormat(&fmt)) &&
                SUCCEEDED(g_wic->CreateBitmap(cw, ch, fmt, WICBitmapCacheOnLoad, &reduced)) &&
                SUCCEEDED(reduced->Lock(&all, WICBitmapLockWrite, &lock)) && SUCCEEDED(lock->GetStride(&stride)) &&
                SUCCEEDED(lock->GetDataPointer(&size, &px)) &&
                SUCCEEDED(xf->CopyPixels(nullptr, cw, ch, &fmt, WICBitmapTransformRotate0, stride, size, px))) {
                lock.Reset();
                src = reduced;
            }
        }
        // Under 2x left to go, Direct2D's bilinear filter at draw time looks fine and costs less.
        UINT rw, rh;
        src->GetSize(&rw, &rh);
        ComPtr<IWICBitmapScaler> scaler;
        if (rw > 2 * sw && SUCCEEDED(g_wic->CreateBitmapScaler(&scaler)) &&
            SUCCEEDED(scaler->Initialize(src.Get(), sw, sh, WICBitmapInterpolationModeFant)))
            src = scaler;
    }
    g_bmp = ToD2D(src.Get());
    if (!g_bmp) return false;
    g_sub = std::to_wstring(w) + L" × " + std::to_wstring(h);
    return true;
}

std::string ToUtf8(const wchar_t* w, int n) {
    std::string s(WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, n, s.data(), (int)s.size(), nullptr, nullptr);
    return s;
}

// Reads up to kMaxText bytes as UTF-8 (from UTF-8, UTF-16 or the ANSI code page); false for binary files.
bool ReadText(const std::wstring& path, std::string& buf, bool& truncated) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    buf.assign((size_t)std::min<LONGLONG>(size.QuadPart, kMaxText), '\0');
    DWORD read = 0;
    ReadFile(f, buf.data(), (DWORD)buf.size(), &read, nullptr);
    CloseHandle(f);
    buf.resize(read);
    truncated = (LONGLONG)read < size.QuadPart;

    auto bom = [&](const char* b, size_t n) { return buf.size() >= n && !memcmp(buf.data(), b, n); };
    if (bom("\xFF\xFE", 2) || bom("\xFE\xFF", 2)) {  // UTF-16
        std::wstring w((buf.size() - 2) / 2, L'\0');
        memcpy(w.data(), buf.data() + 2, w.size() * 2);
        if (buf[0] == '\xFE')
            for (auto& c : w) c = _byteswap_ushort(c);
        buf = ToUtf8(w.data(), (int)w.size());
    } else {
        if (bom("\xEF\xBB\xBF", 3)) buf.erase(0, 3);
        size_t probe = std::min<size_t>(buf.size(), 64 * 1024);
        if (memchr(buf.data(), 0, probe)) return false;  // binary
        // Not UTF-8 (allowing a code point cut at the probe edge)? Treat as the ANSI code page.
        bool utf8 = probe == 0;
        for (size_t cut = 0; cut < 4 && cut < probe && !utf8; cut++)
            utf8 = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, buf.data(), (int)(probe - cut), nullptr, 0);
        if (!utf8) {
            std::wstring w(MultiByteToWideChar(CP_ACP, 0, buf.data(), (int)buf.size(), nullptr, 0), L'\0');
            MultiByteToWideChar(CP_ACP, 0, buf.data(), (int)buf.size(), w.data(), (int)w.size());
            buf = ToUtf8(w.data(), (int)w.size());
        }
    }
    return true;
}

bool LoadText(const std::wstring& path) {
    std::string buf;
    bool truncated;
    if (!ReadText(path, buf, truncated)) return false;
    g_lang = LangForFile(path);
    Sci(SCI_SETILEXER, 0, (sptr_t)(g_lang ? CreateLexer(g_lang->lexer) : nullptr));
    if (g_lang) {
        Sci(SCI_SETKEYWORDS, 0, (sptr_t)g_lang->keywords0);
        Sci(SCI_SETKEYWORDS, 1, (sptr_t)g_lang->keywords1);
    }
    StyleSci();
    Sci(SCI_SETREADONLY, 0);
    Sci(SCI_SETTEXT, 0, (sptr_t)buf.c_str());
    Sci(SCI_SETREADONLY, 1);
    sptr_t lines = Sci(SCI_GETLINECOUNT);
    std::string digits = "_" + std::to_string(lines);
    Sci(SCI_SETMARGINWIDTHN, 0, Sci(SCI_TEXTWIDTH, STYLE_LINENUMBER, (sptr_t)digits.c_str()) + 8);
    g_sub = std::to_wstring(lines) + (lines == 1 ? L" line" : L" lines") + (truncated ? L" · first " + std::to_wstring(kMaxText >> 20) + L" MB" : L"");
    return true;
}

bool LoadSvg(const std::wstring& path) {
    double maxW, maxH;
    PictureArea(maxW, maxH);
    UINT w = 0, h = 0;
    SvgRender(path, 0, 0, g_wic.Get(), w, h);  // intrinsic size first: shown at it, never scaled up
    if (!g_decodeForClient && w && h) {
        double k = std::min({1.0, maxW / (w * g_dpi / 96.0), maxH / (h * g_dpi / 96.0)});
        maxW = w * g_dpi / 96.0 * k, maxH = h * g_dpi / 96.0 * k;
    }
    auto bmp = SvgRender(path, (float)maxW, (float)maxH, g_wic.Get(), w, h);
    if (!bmp || !(g_bmp = ToD2D(bmp.Get()))) return false;
    g_orient = 1;
    g_imgReduced = true;  // vector: render again at the new size after a resize
    UINT bw, bh;
    bmp->GetSize(&bw, &bh);
    g_natW = bw * 96.f / g_dpi, g_natH = bh * 96.f / g_dpi;  // rendered at its intrinsic CSS size
    g_sub = std::to_wstring(w) + L" × " + std::to_wstring(h);
    return true;
}

bool HasExt(const std::wstring& path, const wchar_t* list) {
    std::wstring ext = PathFindExtensionW(path.c_str());
    for (auto& c : ext) c = towlower(c);
    return !ext.empty() && wcsstr(list, (L" " + ext + L" ").c_str());
}

// Markdown, and formats shown through it: notebooks (whole file: outputs embed images), AsciiDoc, rst.
bool LoadMarkdown(const std::wstring& path) {
    std::string buf;
    bool truncated = false, notebook = HasExt(path, L" .ipynb ");
    if (notebook) {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{};
        GetFileSizeEx(f, &size);
        buf.resize((size_t)std::min<LONGLONG>(size.QuadPart, 64 << 20));
        DWORD read = 0;
        ReadFile(f, buf.data(), (DWORD)buf.size(), &read, nullptr);
        CloseHandle(f);
        buf.resize(read);
    } else if (!ReadText(path, buf, truncated)) {
        return false;
    }
    std::wstring w(MultiByteToWideChar(CP_UTF8, 0, buf.data(), (int)buf.size(), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, buf.data(), (int)buf.size(), w.data(), (int)w.size());
    if (notebook && (w = NotebookToMarkdown(w)).empty()) return false;
    if (HasExt(path, L" .adoc .asciidoc ")) w = AsciiDocToMarkdown(w);
    if (HasExt(path, L" .rst .rest ")) w = RstToMarkdown(w);
    std::wstring dir = path.substr(0, path.find_last_of(L'\\'));
    MdLoad(w, dir);
    if (truncated) g_sub = L"first " + std::to_wstring(kMaxText >> 20) + L" MB";
    return true;
}

void LoadPdf(const std::wstring& path, LONGLONG t0) {
    g_pdfT0 = t0;
    g_pdfFailed = false;
    g_pdfIdle = false;
    PdfOpen(path, ++g_pdfGen);
}

size_t SideCount() { return g_kind == K_Md ? MdHeadingCount() : g_kind == K_Pdf ? g_pages.size() : 0; }

float SideW() {
    if (SideCount() < 2) return 0;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    return Dip(rc.right) >= kSideMinWindow ? kSideW : 0;
}

// Sidebar items: top (unscrolled, relative to the sidebar) and height.
void SideItems(std::vector<float>& top, std::vector<float>& height) {
    top.clear(), height.clear();
    float y = 10;
    for (size_t i = 0, n = SideCount(); i < n; i++) {
        float h = g_kind == K_Md ? kHeadRowH : std::round(kThumbW * g_pages[i].second / g_pages[i].first) + kThumbLabel;
        top.push_back(y), height.push_back(h);
        y += h + (g_kind == K_Md ? 0 : 12);
    }
}

float PageWidth() {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    return Dip(rc.right) - 2 * kPad - SideW();
}

// Document position of PDF page i (as DrawPdf lays the pages out).
float PageTop(size_t i) {
    float w = PageWidth(), y = kPad / 2;
    for (size_t k = 0; k < i && k < g_pages.size(); k++) y += std::round(w * g_pages[k].second / g_pages[k].first) + kPageGap;
    return y;
}

// Item the reading position is in: the heading above the top of the view, or the page a third down.
int CurrentItem() {
    if (g_kind == K_Md) {
        int cur = 0;
        for (size_t i = 0; i < MdHeadingCount(); i++) {
            float y = MdHeadingY(i, false);
            if (y < 0 || y > g_scroll[1] + 30) break;
            cur = (int)i;
        }
        return cur;
    }
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    float at = g_scroll[1] + (Dip(rc.bottom) - kTitleH) / 3, w = PageWidth(), y = kPad / 2;
    for (size_t i = 0; i < g_pages.size(); i++) {
        y += std::round(w * g_pages[i].second / g_pages[i].first) + kPageGap;
        if (at < y) return (int)i;
    }
    return g_pages.empty() ? 0 : (int)g_pages.size() - 1;
}

// Next page the PDF worker should render, in WM_FLICK_PDF reply format: the first page in or just
// below the view that hasn't been rendered at the current width.
LRESULT NextPdfPage(bool* thumb = nullptr) {
    if (thumb) *thumb = false;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    float w = PageWidth(), viewH = Dip(rc.bottom) - kTitleH, y = kPad / 2;
    UINT px = (UINT)std::lround(w * g_dpi / 96);
    for (size_t i = 0; i < g_pages.size() && y < g_scroll[1] + 2 * viewH; i++) {
        float h = std::round(w * g_pages[i].second / g_pages[i].first);
        if (y + h > g_scroll[1] && (g_pageW[i] == 0 || (g_pageW[i] != px && !g_sizing)))
            return ((LRESULT)px << 32) | (i + 1);
        y += h + kPageGap;
    }
    // Then the thumbnails in view in the sidebar.
    if (SideW() > 0 && g_thumbW.size() == g_pages.size()) {
        std::vector<float> top, height;
        SideItems(top, height);
        UINT tpx = (UINT)std::lround(kThumbW * g_dpi / 96);
        for (size_t i = 0; i < top.size(); i++)
            if (top[i] + height[i] > g_scroll[2] && top[i] < g_scroll[2] + viewH && !g_thumbW[i]) {
                if (thumb) *thumb = true;
                return ((LRESULT)tpx << 32) | (i + 1);
            }
    }
    return 0;
}

LRESULT OnPdf(PdfMsg& m) {
    if (m.gen != g_pdfGen || g_kind != K_Pdf) return g_pdfIdle = true, 0;
    if (m.page == -1 || !m.sizes.empty()) {  // sizes come in two steps: the first few, then all
        g_pages = m.sizes;
        g_pageBmp.resize(g_pages.size());
        g_pageW.resize(g_pages.size());
        g_thumbBmp.resize(g_pages.size());
        g_thumbW.resize(g_pages.size());
        g_pdfFailed = g_pages.empty();
        if (m.page == -1)
            g_sub = g_pdfFailed ? L"can't open (damaged or password-protected)"
                                : std::to_wstring(m.pageCount) + (m.pageCount == 1 ? L" page" : L" pages");
        if (m.page == -1 && !g_pdfFailed) PlaceWindow();  // before page 1 is requested: rendered at the final width
        InvalidateRect(g_hwnd, nullptr, FALSE);
    }
    if (m.page >= 0 && (size_t)m.page < g_pages.size() && g_pdfReqThumb) {
        g_thumbW[m.page] = (UINT)std::lround(kThumbW * g_dpi / 96);
        if (m.bmp) g_thumbBmp[m.page] = ToD2D(m.bmp.Get());
        InvalidateRect(g_hwnd, nullptr, FALSE);
    } else if (m.page >= 0 && (size_t)m.page < g_pages.size()) {
        g_pageW[m.page] = (UINT)std::lround(PageWidth() * g_dpi / 96);
        if (m.bmp) g_pageBmp[m.page] = ToD2D(m.bmp.Get());
        if (m.page == 0) Log(L"pdf   page 1 ready %.2f ms after key press", MsSince(g_pdfT0));
        InvalidateRect(g_hwnd, nullptr, FALSE);
    }
    LRESULT next = NextPdfPage(&g_pdfReqThumb);
    g_pdfIdle = !next;
    return next;
}

bool TryFrame();
bool Pending() {
    if (g_kind == K_Media && g_mediaVideo && !g_frame && !g_posterDone && !MediaGet().error && !TryFrame()) return true;
    return (g_kind == K_Pdf && !g_pdfFailed && (g_pageW.empty() || !g_pageW[0])) || (g_kind == K_Grid && g_gridPending);
}

// Waits (bounded) for content a worker produces (PDF page 1, archive listing), so the first frame
// shows it instead of an empty placeholder.
void WaitForContent() {
    for (LONGLONG start = Now();;) {
        MSG m;
        PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);  // runs the worker's pending SendMessage
        double left = 120 - MsSince(start);  // slow content shows the placeholder, then fills in
        if (!Pending() || left <= 0) break;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, (DWORD)left + 1, QS_SENDMESSAGE);
    }
}

// Pages stacked at the content width. Pages far from the view drop their bitmaps.
float DrawPdf(float top, float bottom, ID2D1SolidColorBrush* fill) {
    float w = PageWidth(), y = top + kPad / 2 - g_scroll[1], viewH = bottom - top;
    // Until the worker reports the real sizes, show one blank Letter-shaped page.
    const std::vector<std::pair<float, float>> placeholder{{612.f, 792.f}};
    const auto& pages = g_pages.empty() && !g_pdfFailed ? placeholder : g_pages;
    fill->SetColor(C(0xFFFFFF));
    for (size_t i = 0; i < pages.size(); i++) {
        float h = std::round(w * pages[i].second / pages[i].first);
        D2D1_RECT_F r{kPad + SideW(), y, kPad + SideW() + w, y + h};
        if (r.bottom > top && r.top < bottom) {
            g_rt->FillRectangle(r, fill);
            if (i < g_pageBmp.size() && g_pageBmp[i])
                g_rt->DrawBitmap(g_pageBmp[i].Get(), r, 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else if (i < g_pageBmp.size() && (r.bottom < top - 2 * viewH || r.top > bottom + 2 * viewH)) {
            g_pageBmp[i].Reset(), g_pageW[i] = 0;
        }
        y += h + kPageGap;
    }
    return y + g_scroll[1] - top - kPageGap + kPad / 2;
}

// QuickLook-style sizing. Each kind asks for a content size; pictures, videos and PDFs ask for their
// own size, scaled down (never up) to fit 80% (PDF 90%) of the screen. On first show the window is
// centred on the screen; while open (arrowing through files) it is resized around its centre, or kept
// against a screen edge it sits near (10% zones). Sizes are not remembered.
void PlaceWindow() {
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    RECT wa = mi.rcWork, wr, cr;
    GetWindowRect(g_hwnd, &wr);
    GetClientRect(g_hwnd, &cr);
    float px = g_dpi / 96.f, screenW = (wa.right - wa.left) / px, screenH = (wa.bottom - wa.top) / px;
    float chromeW = 2 * kPad, chromeH = kTitleH + kPad;  // around the content, in DIPs
    // Content size the kind asks for (DIPs); `ratio` > 0: own size, fitted into that share of the screen.
    float cw = 800, ch = 600 - chromeH, ratio = 0;
    switch (g_kind) {
        case K_Image:  // edge to edge below the title bar
            if (g_natW > 0) cw = g_natW, ch = g_natH, ratio = 0.8f, chromeW = 0, chromeH = kTitleH;
            break;
        case K_Media:
            if (g_mediaVideo) {
                MediaState ms = MediaGet();
                UINT vw = ms.videoW ? ms.videoW : g_posterW, vh = ms.videoW ? ms.videoH : g_posterH;
                cw = std::max(100.f, (float)(vw ? vw : 1366)), ch = std::max(100.f, (float)(vh ? vh : 768)), ratio = 0.8f;
                chromeW = 0, chromeH = kTitleH;  // edge to edge below the title bar
            } else {
                cw = 560 - chromeW, ch = 560 - chromeH;
            }
            break;
        case K_Pdf:
            if (!g_pages.empty()) {  // first page at 100% (points -> DIPs), plus the sidebar
                cw = g_pages[0].first * 96 / 72, ch = g_pages[0].second * 96 / 72, ratio = 0.9f;
                chromeH = kTitleH + kPad;
                if (g_pages.size() > 1) chromeW += kSideW;
            }
            break;
        case K_Md: cw = 1000 - chromeW, ch = 600 - chromeH; break;
        case K_Grid: cw = (GridGet() && GridGet()->folder.empty() ? 1000 : 800) - chromeW,
                     ch = (GridGet() && GridGet()->folder.empty() ? 700 : 480) - chromeH; break;
        case K_Font: cw = 900 - chromeW, ch = 650 - chromeH; break;
        case K_Info: cw = 560 - chromeW, ch = 520 - chromeH; break;
        default: break;  // text: 800 x 600
    }
    if (ratio > 0) {
        float k = std::min({1.f, (screenW * ratio - chromeW) / cw, (screenH * ratio - chromeH) / ch});
        cw *= k, ch *= k;
    }
    float frameW = (float)((wr.right - wr.left) - cr.right), frameH = (float)((wr.bottom - wr.top) - cr.bottom);
    int w = (int)std::lround(std::max(400.f, cw + chromeW) * px + frameW);
    int h = (int)std::lround(std::max(300.f, ch + chromeH) * px + frameH);
    w = std::min(w, (int)(wa.right - wa.left)), h = std::min(h, (int)(wa.bottom - wa.top));
    // Anchor: against the left/right (top/bottom) edge if the window sits within 10% of it, else centred.
    auto place = [](LONG lo, LONG hi, LONG waLo, LONG waHi, int size) {
        LONG zone = (waHi - waLo) / 10, pos = (lo + hi - size) / 2;
        if (lo < waLo + zone && hi < waHi - zone) pos = lo;
        else if (hi > waHi - zone && lo > waLo + zone) pos = hi - size;
        return std::clamp(pos, waLo, std::max(waLo, waHi - size));
    };
    int x = place(wr.left, wr.right, wa.left, wa.right, w), y = place(wr.top, wr.bottom, wa.top, wa.bottom, h);
    if (x == wr.left && y == wr.top && w == wr.right - wr.left && h == wr.bottom - wr.top) return;
    g_placing = true;
    SetWindowPos(g_hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    g_placing = false;
}

bool LoadCsv(const std::wstring& path) {
    std::string buf;
    bool truncated;
    if (!ReadText(path, buf, truncated)) return false;
    std::wstring w(MultiByteToWideChar(CP_UTF8, 0, buf.data(), (int)buf.size(), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, buf.data(), (int)buf.size(), w.data(), (int)w.size());
    auto g = GridParseCsv(w, HasExt(path, L" .tsv .tab ") ? L'\t' : HasExt(path, L" .psv ") ? L'|' : 0);
    if (!g) return false;
    g_sub = g->sub + (truncated ? L" · first " + std::to_wstring(kMaxText >> 20) + L" MB" : L"");
    GridSet(std::move(g));
    return true;
}

struct GridJob {
    std::wstring path;
    int gen;
    bool folder;
};

DWORD WINAPI GridThread(void* p) {
    std::unique_ptr<GridJob> job(static_cast<GridJob*>(p));
    GridData* g = (job->folder ? GridReadFolder(job->path, g_gridGen, job->gen) : GridReadArchive(job->path, g_gridGen, job->gen)).release();
    if (!SendMessageW(g_hwnd, WM_FLICK_GRID, job->gen, (LPARAM)g)) delete g;  // reply 1: the UI took it
    return 0;
}

// Archive or folder contents as a tree, listed on a worker thread.
void LoadTree(const std::wstring& path, bool folder) {
    g_gridPending = true;
    g_gridFolder = folder;
    g_sub = L"reading…";
    CloseHandle(CreateThread(nullptr, 0, GridThread, new GridJob{path, ++g_gridGen, folder}, 0, nullptr));
}

void ShowInfo();
// Folder size: every file below the folder (hidden ones too, links not followed), counted on a worker.
// The title counts up every 100 ms; at the end each folder row in the tree gets its own total.
struct SizeMsg {
    int gen;
    bool done = false;
    ULONGLONG bytes = 0, files = 0, dirs = 0;
    std::unordered_map<std::wstring, ULONGLONG> dirSizes;  // folder rows of the tree only
};

struct SizeJob {
    int gen;
    std::unordered_set<std::wstring> wanted;  // folder paths shown in the tree
    LONGLONG lastReport;
    SizeMsg msg;
};

ULONGLONG ScanDir(SizeJob& j, const std::wstring& dir, const std::wstring& rel) {
    ULONGLONG sum = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (g_sizeGen != j.gen) break;  // a newer preview: stop
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;  // junctions could loop
            std::wstring r = rel.empty() ? fd.cFileName : rel + L"/" + fd.cFileName;
            ULONGLONG s = ScanDir(j, dir + L"\\" + fd.cFileName, r);
            j.msg.dirs++, sum += s;
            if (j.wanted.count(r)) j.msg.dirSizes[r] = s;
        } else {
            ULONGLONG s = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            sum += s, j.msg.bytes += s, j.msg.files++;
        }
        if (MsSince(j.lastReport) > 100) {
            j.lastReport = Now();
            SizeMsg partial{j.gen, false, j.msg.bytes, j.msg.files, j.msg.dirs};
            SendMessageW(g_hwnd, WM_FLICK_SIZE, 0, (LPARAM)&partial);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return sum;
}

DWORD WINAPI SizeThread(void* p) {
    std::unique_ptr<std::pair<std::wstring, SizeJob>> job(static_cast<std::pair<std::wstring, SizeJob>*>(p));
    std::wstring root = job->first;  // \\?\ so paths longer than MAX_PATH still count
    root = root.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + root.substr(2) : L"\\\\?\\" + root;
    SizeJob& j = job->second;
    j.msg.gen = j.gen;
    j.lastReport = Now();
    ScanDir(j, root, L"");
    if (g_sizeGen == j.gen) {
        j.msg.done = true;
        SendMessageW(g_hwnd, WM_FLICK_SIZE, 0, (LPARAM)&j.msg);
    }
    return 0;
}

std::wstring ByteSize(ULONGLONG n) {
    wchar_t s[64];
    StrFormatByteSizeEx(n, SFBS_FLAGS_ROUND_TO_NEAREST_DISPLAYED_DIGIT, s, 64);
    return s;
}

void OnFolderSize(const SizeMsg& m) {
    const GridData* g = GridGet();
    if (m.gen != g_sizeGen || g_kind != K_Grid || !g) return;
    g_sub = std::to_wstring(m.dirs) + (m.dirs == 1 ? L" folder, " : L" folders, ") + std::to_wstring(m.files) +
            (m.files == 1 ? L" file · " : L" files · ") + ByteSize(m.bytes) + (m.done ? L"" : L"…");
    if (m.done)
        for (size_t i = 0; i < g->paths.size(); i++)
            if (g->folder[i])
                if (auto it = m.dirSizes.find(g->paths[i]); it != m.dirSizes.end()) GridSetCell(i, 1, ByteSize(it->second));
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

LRESULT OnGrid(int gen, GridData* g) {
    if (gen != g_gridGen || g_kind != K_Grid) return 0;
    g_gridPending = false;
    InvalidateRect(g_hwnd, nullptr, FALSE);
    if (!g) return ShowInfo(), 0;  // not readable as an archive
    g_sub = g->sub;
    if (g_gridFolder) {  // count the whole folder's size in the background
        auto* job = new std::pair<std::wstring, SizeJob>{g_path, SizeJob{++g_sizeGen}};
        for (size_t i = 0; i < g->paths.size(); i++)
            if (g->folder[i]) job->second.wanted.insert(g->paths[i]);
        CloseHandle(CreateThread(nullptr, 0, SizeThread, job, 0, nullptr));
    }
    GridSet(std::unique_ptr<GridData>(g));
    return 1;
}

std::wstring FormatTime(const FILETIME& ft) {
    SYSTEMTIME utc, local;
    FileTimeToSystemTime(&ft, &utc);
    SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
    wchar_t d[64], t[64];
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, &local, nullptr, d, 64, nullptr);
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &local, nullptr, t, 64);
    return std::wstring(d) + L", " + t;
}

// Shell thumbnails (5-60 ms, unbounded for slow handlers) and folder item counts run on a worker
// thread; the info card is shown immediately and the result is filled in when it arrives.
struct InfoJob {
    std::wstring path;
    int px, gen;
    LONGLONG t0;
    HBITMAP bmp = nullptr;
    int items = -1;  // folders only
    bool tags = false;
    std::vector<std::wstring> lines;  // audio: title/artist/album
};
std::atomic<int> g_infoGen;

DWORD WINAPI InfoThread(void* p) {
    auto* job = static_cast<InfoJob*>(p);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    {
        ComPtr<IShellItemImageFactory> factory;
        if (SUCCEEDED(SHCreateItemFromParsingName(job->path.c_str(), nullptr, IID_PPV_ARGS(&factory))))
            factory->GetImage({job->px, job->px}, SIIGBF_RESIZETOFIT, &job->bmp);
        ComPtr<IPropertyStore> props;
        if (job->tags && SUCCEEDED(SHGetPropertyStoreFromParsingName(job->path.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(&props)))) {
            auto get = [&](const PROPERTYKEY& key) {
                PROPVARIANT pv;
                PropVariantInit(&pv);
                PWSTR str = nullptr;
                std::wstring out;
                if (SUCCEEDED(props->GetValue(key, &pv)) && pv.vt != VT_EMPTY && SUCCEEDED(PropVariantToStringAlloc(pv, &str)))
                    out = str, CoTaskMemFree(str);
                PropVariantClear(&pv);
                return out;
            };
            std::wstring title = get(PKEY_Title), artist = get(PKEY_Music_Artist), album = get(PKEY_Music_AlbumTitle);
            if (!title.empty()) job->lines.push_back(title + (artist.empty() ? L"" : L" — " + artist));
            else if (!artist.empty()) job->lines.push_back(artist);
            if (!album.empty()) job->lines.push_back(album);
        }
    }
    CoUninitialize();
    if (job->items >= 0) {
        constexpr int kCap = 100000;  // don't count forever in gigantic folders
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW((job->path + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                    nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h != INVALID_HANDLE_VALUE) {
            do job->items += wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..");
            while (job->items < kCap && FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    if (!PostMessageW(g_hwnd, WM_FLICK_INFO, 0, (LPARAM)job)) {
        if (job->bmp) DeleteObject(job->bmp);
        delete job;
    }
    return 0;
}

void OnInfo(InfoJob* job) {
    if (job->gen == g_infoGen && (g_kind == K_Info || g_kind == K_Media) && g_visible) {
        g_info.insert(g_info.begin(), job->lines.begin(), job->lines.end());
        if (job->bmp) {
            ComPtr<IWICBitmap> wb;
            if (SUCCEEDED(g_wic->CreateBitmapFromHBITMAP(job->bmp, nullptr, WICBitmapUsePremultipliedAlpha, &wb)))
                g_bmp = ToD2D(wb.Get());
        }
        if (job->items >= 0)
            g_info[1] = std::to_wstring(job->items) + (job->items >= 100000 ? L"+ items"
                                                       : job->items == 1 ? L" item" : L" items");
        InvalidateRect(g_hwnd, nullptr, FALSE);
        Log(L"info  thumbnail/count ready %.2f ms after key press", MsSince(job->t0));
    }
    if (job->bmp) DeleteObject(job->bmp);
    delete job;
}

void LoadInfo(const std::wstring& path, const WIN32_FILE_ATTRIBUTE_DATA& fa, LONGLONG t0) {
    bool dir = fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
    auto* job = new InfoJob{path, (int)(kThumb * g_dpi / 96), ++g_infoGen, t0};
    job->items = dir ? 0 : -1;
    job->tags = HasExt(path, kAudioExt);
    CloseHandle(CreateThread(nullptr, 0, InfoThread, job, 0, nullptr));
    SHFILEINFOW sfi{};
    SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof sfi, SHGFI_TYPENAME);
    g_info.push_back(sfi.szTypeName);
    if (dir) {
        g_info.push_back(L"… items");
    } else {
        wchar_t sz[64];
        StrFormatByteSizeEx(((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow,
                            SFBS_FLAGS_ROUND_TO_NEAREST_DISPLAYED_DIGIT, sz, 64);
        g_info.push_back(sz);
    }
    g_info.push_back(L"Modified " + FormatTime(fa.ftLastWriteTime));
    if (!dir) {  // programs: description, publisher, version, architecture after the type name
        auto extra = ProgramInfo(path);
        g_info.insert(g_info.begin() + 1, extra.begin(), extra.end());
    }
}

std::wstring Clock(double t) {
    int s = (int)std::max(0.0, t), h = s / 3600, m = s / 60 % 60;
    wchar_t buf[32];
    if (h) swprintf_s(buf, L"%d:%02d:%02d", h, m, s % 60);
    else swprintf_s(buf, L"%d:%02d", m, s % 60);
    return buf;
}

// Where the video goes: fitted (up or down) into the content area. Before the engine knows the
// video size, the poster frame's size is used.
D2D1_RECT_F VideoRect(const MediaState& s) {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    float vw = (float)(s.videoW ? s.videoW : g_posterW), vh = (float)(s.videoW ? s.videoH : g_posterH);
    float aw = Dip(rc.right), ah = Dip(rc.bottom) - kTitleH;  // edge to edge below the title bar
    float k = std::min(aw / vw, ah / vh), w = std::round(vw * k), h = std::round(vh * k);
    float x = std::round((aw - w) / 2), y = std::round(kTitleH + (ah - h) / 2);
    return {x, y, x + w, y + h};
}

// Pulls the next video frame if one is due, at the pixel size it is shown.
bool TryFrame() {
    MediaState s = MediaGet();
    if (!s.videoW) return false;
    D2D1_RECT_F r = VideoRect(s);
    UINT w = (UINT)std::lround((r.right - r.left) * g_dpi / 96), h = (UINT)std::lround((r.bottom - r.top) * g_dpi / 96);
    if (!MediaFrame(w, h, g_wic.Get(), g_frameWic)) return false;
    g_frame = ToD2D(g_frameWic.Get());
    return true;
}

void OnMediaEvent(DWORD event) {
    if (g_kind != K_Media) return;
    MediaOnEvent(event);
    MediaState s = MediaGet();
    if (s.error) {
        if (g_mediaVideo) ShowInfo();
        else g_sub = L"can't play this file";
    } else if (event == MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA) {
        g_sub = (s.videoW ? std::to_wstring(s.videoW) + L" × " + std::to_wstring(s.videoH) + L" · " : L"") + Clock(s.dur);
        if (g_mediaVideo) PlaceWindow();
    }
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

struct PosterJob {
    std::wstring path;
    int gen;
};

DWORD WINAPI PosterThread(void* p) {
    std::unique_ptr<PosterJob> job(static_cast<PosterJob*>(p));
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        ComPtr<IWICImagingFactory> wic;
        ComPtr<IWICBitmap> frame;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))))
            frame = MediaFirstFrame(job->path, wic.Get());
        SendMessageW(g_hwnd, WM_FLICK_POSTER, job->gen, (LPARAM)frame.Get());
    }
    CoUninitialize();
    return 0;
}

void OnPoster(int gen, IWICBitmap* frame) {
    if (gen != g_posterGen || g_kind != K_Media) return;
    g_posterDone = true;
    if (frame && !g_frame) {  // the engine's own frames win once they flow
        frame->GetSize(&g_posterW, &g_posterH);
        PlaceWindow();
        g_frame = ToD2D(frame);
        Log(L"video poster ready %.2f ms after key press", MsSince(g_mediaT0));
        InvalidateRect(g_hwnd, nullptr, FALSE);
    }
}

void SendSeek(bool fast);

// Dragging: the knob follows the mouse at once; the engine gets a fast (key frame) seek whenever it has
// finished the previous one, so seeks never queue up. Releasing seeks exactly (EndSeek).
void SeekAt(float x) {
    if (g_trackRc.right <= g_trackRc.left) return;
    g_seekFrac = std::clamp((x - g_trackRc.left) / (g_trackRc.right - g_trackRc.left), 0.f, 1.f);
    g_seekSent = false;
    SendSeek(true);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

void SendSeek(bool fast) {
    MediaState s = MediaGet();
    if (g_seekFrac < 0 || g_seekSent || s.dur <= 0 || (fast && MediaSeeking())) return;
    MediaSeek(g_seekFrac * s.dur, fast);
    g_seekSent = fast;  // an exact seek is final
}

void EndSeek() {
    g_seekSent = false;
    SendSeek(false);
    g_seekFrac = -1;
}

void VolumeAt(float x) {
    if (g_volRc.right > g_volRc.left) MediaSetVolume((x - g_volRc.left - 7) / (g_volRc.right - g_volRc.left - 14));
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// Control bar: play/pause, time, seek track, mute, volume. Always shown for audio; for video while the
// mouse is over the window or playback is paused.
void DrawControls(D2D1_SIZE_F sz, ID2D1SolidColorBrush* text, ID2D1SolidColorBrush* fill) {
    MediaState s = MediaGet();
    g_playRc = g_trackRc = g_muteRc = g_volRc = {};
    if (g_mediaVideo && !g_mouseIn && !s.paused && !s.ended && !g_seekDrag && !g_volDrag) return;
    const Palette& p = Pal();
    float h = 44, y = sz.height - kPad - h, x0 = kPad + 8, x1 = sz.width - kPad - 8;
    fill->SetColor(g_dark ? C(0x2B2B2B, 0.94f) : C(0xF3F3F3, 0.94f));
    g_rt->FillRoundedRectangle({{x0, y, x1, y + h}, h / 2, h / 2}, fill);
    g_playRc = {x0 + 6, y + 4, x0 + 42, y + 40};
    const wchar_t glyph = s.paused || s.ended ? 0xE768 : 0xE769;  // Segoe Fluent Icons: Play, Pause
    g_rt->DrawText(&glyph, 1, g_fmtGlyph.Get(), g_playRc, text);
    if (g_seekFrac >= 0) s.pos = g_seekFrac * s.dur;  // dragging: show where the knob is
    std::wstring time = Clock(s.pos) + L" / " + Clock(s.dur);
    g_rt->DrawText(time.c_str(), (UINT32)time.size(), g_fmtTitle.Get(), {x0 + 48, y, x0 + 160, y + h}, text);
    g_trackRc = {x0 + 168, y, x1 - 156, y + h};
    // Mute button and volume slider at the right end.
    double vol = MediaVolume();
    g_muteRc = {x1 - 140, y + 4, x1 - 104, y + 40};
    const wchar_t speaker = vol <= 0 ? 0xE74F : vol < 0.34 ? 0xE993 : vol < 0.67 ? 0xE994 : 0xE995;  // Mute, Volume1-3
    g_rt->DrawText(&speaker, 1, g_fmtGlyph.Get(), g_muteRc, text);
    g_volRc = {x1 - 104, y, x1 - 14, y + h};
    float vy = y + h / 2, vx = g_volRc.left + 7 + (g_volRc.right - g_volRc.left - 14) * (float)vol;
    fill->SetColor(p.btnBorder);
    g_rt->FillRoundedRectangle({{g_volRc.left + 7, vy - 2, g_volRc.right - 7, vy + 2}, 2, 2}, fill);
    fill->SetColor(p.link);
    g_rt->FillRoundedRectangle({{g_volRc.left + 7, vy - 2, vx, vy + 2}, 2, 2}, fill);
    g_rt->FillEllipse({{vx, vy}, 6, 6}, fill);
    float cy = y + h / 2, frac = s.dur > 0 ? (float)std::clamp(s.pos / s.dur, 0.0, 1.0) : 0.f;
    float kx = g_trackRc.left + (g_trackRc.right - g_trackRc.left) * frac;
    fill->SetColor(p.btnBorder);
    g_rt->FillRoundedRectangle({{g_trackRc.left, cy - 2, g_trackRc.right, cy + 2}, 2, 2}, fill);
    fill->SetColor(p.link);
    g_rt->FillRoundedRectangle({{g_trackRc.left, cy - 2, kx, cy + 2}, 2, 2}, fill);
    g_rt->FillEllipse({{kx, cy}, 7, 7}, fill);
}

// Releases everything the current preview holds (memory is given back whenever the preview closes).
void ClearContent() {
    g_bmp.Reset();
    g_info.clear();
    MdClear();
    if (g_kind == K_Pdf) PdfOpen({}, ++g_pdfGen);
    g_pages.clear(), g_pageBmp.clear(), g_pageW.clear(), g_thumbBmp.clear(), g_thumbW.clear();
    g_sideCurrent = -1, g_sideFocus = false;
    GridSet(nullptr);
    FontClear();
    MediaClose();
    g_frame.Reset(), g_frameWic.Reset();
    g_mediaVideo = g_seekDrag = false, g_shownPos = -1, g_seekFrac = -1;
    g_posterGen++, g_posterW = g_posterH = 0;
    g_natW = g_natH = 0;
    g_gridGen++, g_gridPending = false, g_sizeGen++;
    for (int a = 0; a < 3; a++) g_scroll[a] = g_doc[a] = 0;
    g_kind = K_None;
}

void ShowInfo() {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    GetFileAttributesExW(g_path.c_str(), GetFileExInfoStandard, &fa);
    ClearContent();
    g_sub.clear();
    g_kind = K_Info;
    LoadInfo(g_path, fa, Now());
}

void LoadContent(const std::wstring& path, LONGLONG t0) {
    ClearContent();
    g_sub.clear();
    g_path = path;
    g_title = PathFindFileNameW(path.c_str());
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa);
    bool dir = fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
    if (dir) g_kind = K_Grid, LoadTree(path, true);  // empty or unreadable: info card (ShowInfo)
    else if (HasExt(path, L" .pdf ")) g_kind = K_Pdf, LoadPdf(path, t0);
    else if (HasExt(path, L" .md .markdown .mdown .mkd .mkdn .mdx .rmd .qmd .ipynb .adoc .asciidoc .rst .rest ") &&
             LoadMarkdown(path)) g_kind = K_Md;
    else if (HasExt(path, kVideoExt) && MediaOpen(path)) {
        g_kind = K_Media, g_mediaVideo = true, g_mediaT0 = t0, g_posterDone = false;
        CloseHandle(CreateThread(nullptr, 0, PosterThread, new PosterJob{path, ++g_posterGen}, 0, nullptr));
    }
    else if (HasExt(path, kAudioExt) && MediaOpen(path)) g_kind = K_Media, LoadInfo(path, fa, t0);
    else if (HasExt(path, L" .csv .tsv .tab .psv ") && LoadCsv(path)) g_kind = K_Grid;
    else if (HasExt(path, L" .svg .svgz ") && LoadSvg(path)) g_kind = K_Image;
    else if (HasExt(path, L" .ttf .otf .ttc .otc .woff .woff2 ") && FontLoad(g_dw.Get(), path, g_sub)) g_kind = K_Font;
    else if (HasExt(path, L" .zip .7z .rar .tar .gz .tgz .bz2 .tbz .tbz2 .xz .txz .zst .tzst .lz .lzma .lz4 .cab "
                          L".iso .cpio .ar .deb .rpm .jar .war .ear .apk .aab .xapk .nupkg .snupkg .vsix .whl "
                          L".egg .crx .xpi .cbz .cbr .cb7 .cbt .appx .appxbundle .msix .msixbundle ")) g_kind = K_Grid, LoadTree(path, false);
    else if (HasExt(path, L" .jpg .jpeg .jpe .jfif .png .bmp .dib .gif .tif .tiff .ico .webp .heic .heif .avif .jxr "
                                  L".wdp .dds ") &&
             LoadImage(path)) g_kind = K_Image;
    else if (LoadText(path)) g_kind = K_Text;
    else g_kind = K_Info, LoadInfo(path, fa, t0);

    g_open = L"Open";
    if (!dir) {
        wchar_t app[128];
        DWORD n = 128;
        if (SUCCEEDED(AssocQueryStringW(ASSOCF_INIT_IGNOREUNKNOWN, ASSOCSTR_FRIENDLYAPPNAME,
                                        PathFindExtensionW(path.c_str()), nullptr, app, &n)) && *app)
            g_open = std::wstring(L"Open with ") + app;
    }
}

void Layout() {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int top = (int)std::ceil(kTitleH * g_dpi / 96);
    // Mica everywhere, except under Scintilla: GDI writes alpha 0, which DWM would add onto the backdrop.
    static int glassTop = -2;
    int want = g_kind == K_Text ? top : -1;
    if (want != glassTop) {
        MARGINS m{0, 0, want, 0};
        if (want < 0) m = {-1};
        DwmExtendFrameIntoClientArea(g_hwnd, &m);
        glassTop = want;
    }
    if (g_kind == K_Text)
        SetWindowPos(g_sci, nullptr, 0, top, rc.right, rc.bottom - top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    else
        ShowWindow(g_sci, SW_HIDE);
}

void DrawImage(const D2D1_RECT_F& area) {
    D2D1_SIZE_F s = g_bmp->GetSize();
    bool swap = g_orient >= 5;
    // Shown at 1 image pixel = 1 DIP (so scaled up with the display scaling), shrunk to fit the area.
    float dw = g_natW > 0 ? g_natW : swap ? s.height : s.width, dh = g_natH > 0 ? g_natH : swap ? s.width : s.height;
    float fit = std::min({1.f, (area.right - area.left) / dw, (area.bottom - area.top) / dh});
    float k = dw * fit / (swap ? s.height : s.width);  // bitmap DIPs -> shown DIPs
    D2D1::Matrix3x2F m = D2D1::Matrix3x2F::Identity();
    switch (g_orient) {
        case 2: m = D2D1::Matrix3x2F::Scale(-1, 1); break;
        case 3: m = D2D1::Matrix3x2F::Rotation(180); break;
        case 4: m = D2D1::Matrix3x2F::Scale(1, -1); break;
        case 5: m = D2D1::Matrix3x2F(0, 1, 1, 0, 0, 0); break;
        case 6: m = D2D1::Matrix3x2F::Rotation(90); break;
        case 7: m = D2D1::Matrix3x2F(0, -1, -1, 0, 0, 0); break;
        case 8: m = D2D1::Matrix3x2F::Rotation(270); break;
    }
    float cx = std::round((area.left + area.right) / 2), cy = std::round((area.top + area.bottom) / 2);
    g_rt->SetTransform(m * D2D1::Matrix3x2F::Translation(cx, cy));
    float hw = s.width * k / 2, hh = s.height * k / 2;
    g_rt->DrawBitmap(g_bmp.Get(), {-hw, -hh, hw, hh}, 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    g_rt->SetTransform(D2D1::Matrix3x2F::Identity());
}

void DrawInfo(const D2D1_RECT_F& area, ID2D1SolidColorBrush* text, ID2D1SolidColorBrush* sub) {
    D2D1_SIZE_F t = g_bmp ? g_bmp->GetSize() : D2D1_SIZE_F{0, 0};
    float lineH = 22, height = kThumb + 20 + 30 + lineH * g_info.size();
    float y = std::max(area.top, (area.top + area.bottom - height) / 2);
    float cx = (area.left + area.right) / 2;
    if (g_bmp) {
        float bx = std::round(cx - t.width / 2), by = std::round(y + (kThumb - t.height) / 2);
        g_rt->DrawBitmap(g_bmp.Get(), {bx, by, bx + t.width, by + t.height});
    }
    y += kThumb + 20;
    g_rt->DrawText(g_title.c_str(), (UINT32)g_title.size(), g_fmtName.Get(), {area.left, y, area.right, y + 30}, text);
    y += 30;
    for (auto& line : g_info) {
        g_rt->DrawText(line.c_str(), (UINT32)line.size(), g_fmtInfo.Get(), {area.left, y, area.right, y + lineH}, sub);
        y += lineH;
    }
}

bool Scrollable();
bool Thumb(int axis, float& at, float& len);

// Contents (Markdown headings) or page thumbnails (PDF) down the left side; follows the reading position.
void DrawSidebar(D2D1_SIZE_F sz, ID2D1SolidColorBrush* text, ID2D1SolidColorBrush* sub, ID2D1SolidColorBrush* fill) {
    const Palette& p = Pal();
    std::vector<float> top, height;
    SideItems(top, height);
    if (top.empty()) return;
    float viewH = sz.height - kTitleH;
    int cur = CurrentItem();
    if (cur != g_sideCurrent) {  // keep the current item in view as the document scrolls
        g_sideCurrent = cur;
        if (top[cur] < g_scroll[2]) g_scroll[2] = top[cur] - 10;
        else if (top[cur] + height[cur] > g_scroll[2] + viewH) g_scroll[2] = top[cur] + height[cur] - viewH + 10;
    }
    g_doc[2] = top.back() + height.back() + 10;
    g_scroll[2] = std::clamp(g_scroll[2], 0.f, std::max(0.f, g_doc[2] - viewH));
    fill->SetColor(p.codeBg);
    g_rt->FillRectangle({0, kTitleH, kSideW, sz.height}, fill);
    g_rt->PushAxisAlignedClip({0, kTitleH, kSideW, sz.height}, D2D1_ANTIALIAS_MODE_ALIASED);
    for (size_t i = 0; i < top.size(); i++) {
        float y = kTitleH + top[i] - g_scroll[2];
        bool current = (int)i == cur;
        if (g_kind == K_Pdf && i < g_thumbBmp.size() && (y + height[i] < kTitleH - 2 * viewH || y > sz.height + 2 * viewH))
            g_thumbBmp[i].Reset(), g_thumbW[i] = 0;  // far out of view: drop
        if (y > sz.height || y + height[i] < kTitleH) continue;
        if (g_kind == K_Md) {
            float x = 16 + (MdHeadingLevel(i) - 1) * 10.f;
            if (current) {
                fill->SetColor(p.link);
                g_rt->FillRoundedRectangle({{6, y + 8, 9, y + kHeadRowH - 8}, 1.5f, 1.5f}, fill);
            }
            const std::wstring& t = MdHeadingText(i);
            g_rt->DrawText(t.c_str(), (UINT32)t.size(), g_fmtTitle.Get(), {x, y, kSideW - 10, y + kHeadRowH},
                           current ? text : sub);
        } else {
            float x = std::round((kSideW - kThumbW) / 2), h = height[i] - kThumbLabel;
            D2D1_RECT_F r{x, y, x + kThumbW, y + h};
            fill->SetColor(C(0xFFFFFF));
            g_rt->FillRectangle(r, fill);
            if (i < g_thumbBmp.size() && g_thumbBmp[i])
                g_rt->DrawBitmap(g_thumbBmp[i].Get(), r, 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            fill->SetColor(current ? p.link : p.btnBorder);
            g_rt->DrawRectangle({r.left - 1, r.top - 1, r.right + 1, r.bottom + 1}, fill, current ? 2.f : 1.f);
            std::wstring n = std::to_wstring(i + 1);
            g_rt->DrawText(n.c_str(), (UINT32)n.size(), g_fmtInfo.Get(), {0, r.bottom, kSideW, r.bottom + kThumbLabel},
                           current ? text : sub);
        }
    }
    g_rt->PopAxisAlignedClip();
}

// Jumps the document to sidebar item i: a heading or a page.
void GoToItem(int i) {
    if (i < 0 || i >= (int)SideCount()) return;
    float target = g_kind == K_Md ? MdHeadingY(i, true) - 8 : PageTop(i) - kPad / 2;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    g_doc[1] = std::max(g_doc[1], target + Dip(rc.bottom) - kTitleH);  // Markdown's height is an estimate
    g_scroll[1] = std::max(0.f, target);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// Sidebar click at y (DIPs): jumps there; the arrow keys then step through the items.
void SideClick(float y) {
    std::vector<float> top, height;
    SideItems(top, height);
    g_sideFocus = true;
    for (size_t i = 0; i < top.size(); i++) {
        float t = kTitleH + top[i] - g_scroll[2];
        if (y >= t && y < t + height[i]) return GoToItem((int)i);
    }
}

void Paint() {
    if (!g_rt) CreateTarget();
    const Palette& p = Pal();
    g_rt->BeginDraw();
    g_rt->Clear({0, 0, 0, 0});  // transparent: Mica shows through
    D2D1_SIZE_F sz = g_rt->GetSize();
    ComPtr<ID2D1SolidColorBrush> text, sub, fill;
    g_rt->CreateSolidColorBrush(p.text, &text);
    g_rt->CreateSolidColorBrush(p.sub, &sub);
    g_rt->CreateSolidColorBrush(p.btn, &fill);

    // Close button (Windows 11 caption style).
    g_closeRc = {sz.width - kCloseW, 0, sz.width, kTitleH};
    if (g_hot == H_Close) {
        fill->SetColor(C(0xC42B1C));
        g_rt->FillRectangle(g_closeRc, fill.Get());
    }
    if (g_hot == H_Close) text->SetColor(C(0xFFFFFF));
    const wchar_t closeGlyph = 0xE8BB;  // Segoe Fluent Icons "ChromeClose"
    g_rt->DrawText(&closeGlyph, 1, g_fmtIcon.Get(), g_closeRc, text.Get());
    text->SetColor(p.text);

    // "Open with ..." button.
    ComPtr<IDWriteTextLayout> label;
    g_dw->CreateTextLayout(g_open.c_str(), (UINT32)g_open.size(), g_fmtTitle.Get(), 400, kBtnH, &label);
    DWRITE_TEXT_METRICS tm{};
    label->GetMetrics(&tm);
    float bw = std::ceil(tm.width) + 24, top = (kTitleH - kBtnH) / 2;
    g_openRc = {g_closeRc.left - 6 - bw, top, g_closeRc.left - 6, top + kBtnH};
    D2D1_ROUNDED_RECT rr{{g_openRc.left + .5f, g_openRc.top + .5f, g_openRc.right - .5f, g_openRc.bottom - .5f}, 4, 4};
    fill->SetColor(g_hot == H_Open ? p.btnHot : p.btn);
    g_rt->FillRoundedRectangle(rr, fill.Get());
    fill->SetColor(p.btnBorder);
    g_rt->DrawRoundedRectangle(rr, fill.Get());
    g_rt->DrawTextLayout({g_openRc.left + 12, top}, label.Get(), text.Get());

    // Title: file name + dimmed details, trimmed with an ellipsis.
    if (g_kind == K_Pdf && !g_pdfFailed && g_pages.size() > 1)  // reading position: "3 / 94"
        g_sub = std::to_wstring(CurrentItem() + 1) + L" / " + std::to_wstring(g_pages.size());
    std::wstring title = g_title + (g_sub.empty() ? L"" : L"    " + g_sub);
    ComPtr<IDWriteTextLayout> tl;
    g_dw->CreateTextLayout(title.c_str(), (UINT32)title.size(), g_fmtTitle.Get(),
                           std::max(0.f, g_openRc.left - 16 - kPad), kTitleH, &tl);
    if (!g_sub.empty()) tl->SetDrawingEffect(sub.Get(), {(UINT32)g_title.size(), (UINT32)(title.size() - g_title.size())});
    g_rt->DrawTextLayout({kPad, 0}, tl.Get(), text.Get());

    D2D1_RECT_F area{kPad, kTitleH, sz.width - kPad, sz.height - kPad};
    if (g_kind == K_Image && g_bmp) DrawImage({0, kTitleH, sz.width, sz.height});
    if (g_kind == K_Info) DrawInfo(area, text.Get(), sub.Get());
    if (g_kind == K_Media) {
        MediaState ms = MediaGet();
        if (!g_mediaVideo) DrawInfo({area.left, area.top, area.right, area.bottom - 52}, text.Get(), sub.Get());
        else if (g_frame && (ms.videoW || g_posterW)) g_rt->DrawBitmap(g_frame.Get(), VideoRect(ms), 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        DrawControls(sz, text.Get(), fill.Get());
    }
    if (Scrollable()) {
        D2D1_RECT_F doc{kPad + SideW(), kTitleH, sz.width - kPad, sz.height};
        MdColors mc{p.text, p.sub, p.link, p.codeBg, p.rule, p.role};
        g_rt->PushAxisAlignedClip({0, kTitleH, sz.width, sz.height}, D2D1_ANTIALIAS_MODE_ALIASED);
        if (g_kind == K_Md) g_doc[1] = MdPaint(g_rt.Get(), doc, g_scroll[1], mc);
        if (g_kind == K_Pdf) g_doc[1] = DrawPdf(kTitleH, sz.height, fill.Get());
        if (g_kind == K_Font) g_doc[1] = FontPaint(g_rt.Get(), doc, g_scroll[1], mc);
        if (g_kind == K_Grid && GridGet()) {
            D2D1_SIZE_F size = GridPaint(g_rt.Get(), doc, g_scroll[0], g_scroll[1], mc);
            g_doc[0] = size.width, g_doc[1] = size.height;
            // A folder was collapsed below the end: pull the view back up (repaints once more).
            if (float max = std::max(0.f, g_doc[1] - (sz.height - kTitleH)); g_scroll[1] > max)
                g_scroll[1] = max, InvalidateRect(g_hwnd, nullptr, FALSE);
        }
        g_rt->PopAxisAlignedClip();
        if (SideW() > 0) DrawSidebar(sz, text.Get(), sub.Get(), fill.Get());
        fill->SetColor(p.sub);
        for (int axis = 0; axis < 3; axis++)
            if (float at, len; Thumb(axis, at, len)) {  // overlay scrollbars: thin, wider under the mouse
                float bw = g_barHot == axis || g_dragAxis == axis ? 6 : 3, right = axis == 2 ? SideW() : sz.width;
                D2D1_RECT_F r = axis ? D2D1_RECT_F{right - 3 - bw, at + 2, right - 3, at + len - 2}
                                     : D2D1_RECT_F{at + 2, sz.height - 3 - bw, at + len - 2, sz.height - 3};
                g_rt->FillRoundedRectangle({r, bw / 2, bw / 2}, fill.Get());
            }
    }

    HRESULT hrEnd = g_rt->EndDraw();
    if (g_kind == K_Pdf && g_pdfIdle && NextPdfPage()) g_pdfIdle = false, PdfWake();
    if (hrEnd == D2DERR_RECREATE_TARGET) {  // device lost: rebuild and reload bitmaps
        g_rt.Reset();
        CreateTarget();
        if (g_kind != K_Text) LoadContent(g_path, Now());
        InvalidateRect(g_hwnd, nullptr, FALSE);
    }
}

bool Scrollable() { return g_kind == K_Md || g_kind == K_Pdf || g_kind == K_Grid || g_kind == K_Font; }

// Visible extent of the document along an axis, in DIPs.
float View(int axis) {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    return axis ? Dip(rc.bottom) - kTitleH : Dip(rc.right) - 2 * kPad;
}

// Scrollbar thumb along an axis (position from the window edge, length) in DIPs; false: nothing to scroll.
bool Thumb(int axis, float& at, float& len) {
    float view = View(axis);
    if (!Scrollable() || g_doc[axis] <= view || (axis == 2 && SideW() <= 0)) return false;
    len = std::max(24.f, view * view / g_doc[axis]);
    at = (axis ? kTitleH : kPad) + (view - len) * g_scroll[axis] / (g_doc[axis] - view);
    return true;
}

// Scrollbar under the mouse: 1 vertical (right edge), 0 horizontal (bottom edge), 2 sidebar, -1 none.
int OnBar(LPARAM l) {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    float x = Dip(GET_X_LPARAM(l)), y = Dip(GET_Y_LPARAM(l)), at, len;
    if (Thumb(1, at, len) && x >= Dip(rc.right) - kBarHit && y >= kTitleH) return 1;
    if (Thumb(2, at, len) && x >= SideW() - kBarHit && x < SideW() && y >= kTitleH) return 2;
    if (Thumb(0, at, len) && y >= Dip(rc.bottom) - kBarHit && x < Dip(rc.right) - kBarHit) return 0;
    return -1;
}

void ScrollTo(int axis, float s) {
    s = std::clamp(s, 0.f, std::max(0.f, g_doc[axis] - View(axis)));
    if (s != g_scroll[axis]) g_scroll[axis] = s, InvalidateRect(g_hwnd, nullptr, FALSE);
}

void ScrollBy(int axis, float d) { ScrollTo(axis, g_scroll[axis] + d); }

// Moves the dragged thumb so that its point g_dragGrab is under the mouse.
void DragThumb(LPARAM l) {
    float at, len;
    int a = g_dragAxis;
    if (a < 0 || !Thumb(a, at, len)) return;
    float view = View(a), mouse = a ? Dip(GET_Y_LPARAM(l)) - kTitleH : Dip(GET_X_LPARAM(l)) - kPad;
    ScrollTo(a, (mouse - g_dragGrab) / (view - len) * (g_doc[a] - view));
}

Hot HitButton(LPARAM l) {
    D2D1_POINT_2F pt{Dip(GET_X_LPARAM(l)), Dip(GET_Y_LPARAM(l))};
    auto in = [&](const D2D1_RECT_F& r) {
        return pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom;
    };
    return in(g_closeRc) ? H_Close : in(g_openRc) ? H_Open : H_None;
}

void OpenFile(bool chooseApp) {
    if (chooseApp) {
        OPENASINFO oi{g_path.c_str(), nullptr, OAIF_ALLOW_REGISTRATION | OAIF_REGISTER_EXT | OAIF_EXEC};
        if (FAILED(SHOpenWithDialog(g_hwnd, &oi))) return;
    } else {
        ShellExecuteW(g_hwnd, nullptr, g_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    PreviewHide(false);
}


// Hide when the user switches to anything other than the source Explorer window or us.
// Transient shell UI (the Alt+Tab switcher, Start, search, taskbar) is ignored: the user hasn't
// switched apps (yet); if they do, the app they pick takes the foreground next.
void CALLBACK OnForeground(HWINEVENTHOOK, DWORD, HWND h, LONG, LONG, DWORD, DWORD) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (g_visible && h && pid != GetCurrentProcessId() && GetAncestor(h, GA_ROOT) != g_source) {
        wchar_t cls[64] = L"";
        GetClassNameW(h, cls, 64);
        for (const wchar_t* shell : {L"XamlExplorerHostIslandWindow", L"MultitaskingViewFrame", L"TaskSwitcherWnd",
                                     L"ForegroundStaging", L"Windows.UI.Core.CoreWindow", L"Shell_TrayWnd",
                                     L"Shell_SecondaryTrayWnd", L"NotifyIconOverflowWindow",
                                     L"TopLevelWindowForOverflowXamlIsland"})
            if (!wcscmp(cls, shell)) return;
        Log(L"hide: foreground -> %s %p (source %p)", cls, h, g_source);
        PreviewHide(false);
    }
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_NCACTIVATE:  // always "active": Mica keeps its tinted look instead of going flat grey
            return DefWindowProcW(h, m, TRUE, l);
        case WM_NCCALCSIZE:
            if (w) {  // drop the system caption, keep the side/bottom resize borders
                auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(l);
                LONG top = p->rgrc[0].top;
                DefWindowProcW(h, m, w, l);
                p->rgrc[0].top = top;
                return 0;
            }
            break;
        case WM_NCHITTEST: {
            LRESULT r = DefWindowProcW(h, m, w, l);
            if (r != HTCLIENT) return r;
            POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            ScreenToClient(h, &pt);
            if (pt.y < GetSystemMetricsForDpi(SM_CYSIZEFRAME, g_dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, g_dpi))
                return HTTOP;
            if (Dip(pt.y) < kTitleH && HitButton(MAKELPARAM(pt.x, pt.y)) == H_None) return HTCAPTION;
            return HTCLIENT;
        }
        case WM_PAINT:
            Paint();
            ValidateRect(h, nullptr);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            if (g_rt) g_rt->Resize(D2D1::SizeU(LOWORD(l), HIWORD(l)));
            Layout();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_DPICHANGED: {
            g_dpi = HIWORD(w);
            if (g_rt) g_rt->SetDpi((float)g_dpi, (float)g_dpi);
            if (!g_placing) {  // PreviewShow sizes for the target monitor itself
                auto* r = reinterpret_cast<RECT*>(l);
                SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        }
        case WM_ENTERSIZEMOVE:
            g_sizing = true;
            return 0;
        case WM_EXITSIZEMOVE:
            g_sizing = false;
            if (g_kind == K_Image && g_imgReduced) {
                g_decodeForClient = true;
                LoadContent(g_path, Now());
                g_decodeForClient = false;
            }
            InvalidateRect(h, nullptr, FALSE);  // PDF: re-render pages at the final width
            return 0;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            if (g_kind == K_Media && m == WM_MOUSEWHEEL) {  // wheel: volume, 5% a notch
                MediaSetVolume(MediaVolume() + GET_WHEEL_DELTA_WPARAM(w) / (double)WHEEL_DELTA * 0.05);
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (Scrollable()) {  // Shift+wheel or a tilt wheel scrolls sideways
                bool sideways = m == WM_MOUSEHWHEEL || (GET_KEYSTATE_WPARAM(w) & MK_SHIFT);
                UINT lines = 3;
                SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
                float d = GET_WHEEL_DELTA_WPARAM(w) / (float)WHEEL_DELTA * std::min(lines, 10u) * 36;
                POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};  // wheel positions are in screen coordinates
                ScreenToClient(h, &pt);
                if (m == WM_MOUSEWHEEL && Dip(pt.x) < SideW()) return ScrollBy(2, -d), 0;  // over the sidebar
                ScrollBy(sideways ? 0 : 1, m == WM_MOUSEHWHEEL ? d : -d);
            }
            return 0;
        case WM_MOUSEMOVE: {
            if (g_dragAxis >= 0) return DragThumb(l), 0;
            if (g_seekDrag) return SeekAt(Dip(GET_X_LPARAM(l))), 0;
            if (g_volDrag) return VolumeAt(Dip(GET_X_LPARAM(l))), 0;
            if (!g_mouseIn && g_kind == K_Media) g_mouseIn = true, InvalidateRect(h, nullptr, FALSE);
            Hot hot = HitButton(l);
            if (hot != g_hot) g_hot = hot, InvalidateRect(h, nullptr, FALSE);
            if (int bar = OnBar(l); bar != g_barHot) g_barHot = bar, InvalidateRect(h, nullptr, FALSE);
            TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, h};
            TrackMouseEvent(&tme);
            return 0;
        }
        case WM_MOUSELEAVE:
            g_hot = H_None, g_barHot = -1, g_mouseIn = false;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            if (Dip(GET_X_LPARAM(l)) < SideW() - kBarHit && Dip(GET_Y_LPARAM(l)) > kTitleH)
                return SideClick(Dip(GET_Y_LPARAM(l))), 0;
            g_sideFocus = false;
            if (g_kind == K_Media) {
                D2D1_POINT_2F pt{Dip(GET_X_LPARAM(l)), Dip(GET_Y_LPARAM(l))};
                auto in = [&](const D2D1_RECT_F& r) { return pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom; };
                if (in(g_trackRc)) g_seekDrag = true, SetCapture(h), SeekAt(pt.x);
                else if (in(g_volRc)) g_volDrag = true, SetCapture(h), VolumeAt(pt.x);
                else if (in(g_muteRc)) MediaToggleMute(), InvalidateRect(h, nullptr, FALSE);
                else if (in(g_playRc) || (g_mediaVideo && pt.y > kTitleH)) MediaToggle(), InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (g_kind == K_Grid && OnBar(l) < 0) {  // trees: click a folder to expand/collapse it
                RECT rc;
                GetClientRect(h, &rc);
                if (GridClick({kPad, kTitleH, Dip(rc.right) - kPad, Dip(rc.bottom)}, g_scroll[1],
                              {Dip(GET_X_LPARAM(l)), Dip(GET_Y_LPARAM(l))}))
                    InvalidateRect(h, nullptr, FALSE);  // Paint re-measures; ScrollTo clamps on the next scroll
                return 0;
            }
            if (int a = OnBar(l); a >= 0) {
                float at, len, mouse = a ? Dip(GET_Y_LPARAM(l)) : Dip(GET_X_LPARAM(l));
                Thumb(a, at, len);
                g_dragGrab = mouse >= at && mouse < at + len ? mouse - at : len / 2;  // track click: jump there
                g_dragAxis = a;
                SetCapture(h);
                DragThumb(l);
            }
            return 0;
        case WM_CAPTURECHANGED:
            if (g_seekDrag) EndSeek();
            g_seekDrag = g_volDrag = false;
            if (g_dragAxis >= 0) g_dragAxis = -1, InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP:
            if (g_dragAxis >= 0 || g_seekDrag || g_volDrag) ReleaseCapture();  // -> WM_CAPTURECHANGED ends the drag
            else if (HitButton(l) == H_Close) PreviewHide(true);
            else if (HitButton(l) == H_Open) OpenFile(false);
            return 0;
        case WM_RBUTTONUP:
            if (HitButton(l) == H_Open && g_kind != K_Info) OpenFile(true);
            return 0;
        case WM_SETFOCUS:
            if (g_kind == K_Text) SetFocus(g_sci);
            return 0;
        case WM_CLOSE:
            PreviewHide(true);
            return 0;
        case WM_SETTINGCHANGE:
            if (l && !wcscmp((const wchar_t*)l, L"ImmersiveColorSet")) ApplyTheme();
            break;
        case WM_FLICK_TOGGLE: {  // Space: open; on the file already shown, close; on another file, show that
            std::wstring path = ExplorerSelection((HWND)w);
            if (!path.empty() && (!g_visible || path != g_path)) PreviewShow(path, (HWND)w, (LONGLONG)l);
            else PreviewHide(false);
            return 0;
        }
        case WM_FLICK_FOLLOW:
            if (auto path = ExplorerSelection((HWND)w); g_visible && !path.empty() && path != g_path)
                PreviewShow(path, (HWND)w, (LONGLONG)l);
            return 0;
        case WM_FLICK_HIDE:
            PreviewHide(false);
            return 0;
        case WM_FLICK_PDF:
            return OnPdf(*reinterpret_cast<PdfMsg*>(l));
        case WM_FLICK_MEDIA:
            OnMediaEvent((DWORD)w);
            return 0;
        case WM_FLICK_FRAME: {
            MediaFrameHandled();
            if (g_kind != K_Media || !g_visible) return 0;
            bool repaint = g_mediaVideo && TryFrame();
            if (g_seekDrag) SendSeek(true);  // the last drag position, once the engine is free
            // Progress: repaint whenever the knob moves a pixel (or the clock a second), not on a fixed tick.
            MediaState s = MediaGet();
            double frac = g_seekFrac >= 0 ? g_seekFrac : s.dur > 0 ? s.pos / s.dur : 0;
            double shown = std::floor(frac * (g_trackRc.right - g_trackRc.left) * g_dpi / 96) * 1e6 + std::floor(s.pos);
            if (shown != g_shownPos) g_shownPos = shown, repaint = true;
            if (repaint) InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_FLICK_SIZE:
            OnFolderSize(*reinterpret_cast<SizeMsg*>(l));
            return 0;
        case WM_FLICK_POSTER:
            OnPoster((int)w, reinterpret_cast<IWICBitmap*>(l));
            return 0;
        case WM_FLICK_GRID:
            return OnGrid((int)w, reinterpret_cast<GridData*>(l));
        case WM_FLICK_INFO:
            OnInfo(reinterpret_cast<InfoJob*>(l));
            return 0;
        case WM_FLICK_QUIT:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

ComPtr<IDWriteTextFormat> Format(const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight,
                                 DWRITE_TEXT_ALIGNMENT align) {
    ComPtr<IDWriteTextFormat> f;
    g_dw->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"",
                           &f);
    f->SetTextAlignment(align);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER};
    ComPtr<IDWriteInlineObject> ellipsis;
    g_dw->CreateEllipsisTrimmingSign(f.Get(), &ellipsis);
    f->SetTrimming(&trim, ellipsis.Get());
    return f;
}

bool HasFont(const wchar_t* family) {
    ComPtr<IDWriteFontCollection> fonts;
    UINT32 i;
    BOOL exists = FALSE;
    g_dw->GetSystemFontCollection(&fonts);
    fonts->FindFamilyName(family, &i, &exists);
    return exists;
}

}  // namespace

bool PreviewVisible() { return g_visible; }
ComPtr<ID2D1Bitmap> SharedBitmap(IWICBitmapSource* src) { return ToD2D(src); }
IWICImagingFactory* WicFactory() { return g_wic.Get(); }

bool PreviewInit(HINSTANCE inst) {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, g_d2d.GetAddressOf())) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(g_dw.GetAddressOf()))) ||
        FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic))))
        return false;
    const wchar_t* ui = HasFont(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
    g_fmtTitle = Format(ui, 13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    g_fmtIcon = Format(HasFont(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets", 10,
                       DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_fmtGlyph = Format(HasFont(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets", 16,
                        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_fmtName = Format(ui, 20, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_fmtInfo = Format(ui, 13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_codeFont = HasFont(L"Cascadia Mono") ? "Cascadia Mono" : "Consolas";
    MdInit(g_dw.Get(), ui, g_codeFont == "Consolas" ? L"Consolas" : L"Cascadia Mono");
    GridInit(g_dw.Get(), ui, HasFont(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets");

    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"FlickPreview";
    RegisterClassExW(&wc);
    // Tool window: no taskbar button flashing in and out on every preview.
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"FlickPreview", L"Flick",
                             WS_OVERLAPPED | WS_CAPTION | WS_THICKFRAME | WS_CLIPCHILDREN, 0, 0,
                             800, 600, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) return false;
    g_dpi = GetDpiForWindow(g_hwnd);
    DWM_SYSTEMBACKDROP_TYPE backdrop = DWMSBT_MAINWINDOW;  // Mica
    DwmSetWindowAttribute(g_hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof backdrop);
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(g_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof corner);
    SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);

    // Undocumented but stable since 1903: lets "DarkMode_Explorer" theme the Scintilla scrollbars.
    if (HMODULE ux = GetModuleHandleW(L"uxtheme.dll"))
        if (auto setAppMode = (int(WINAPI*)(int))GetProcAddress(ux, MAKEINTRESOURCEA(135))) setAppMode(1);

    Scintilla_RegisterClasses(inst);
    g_sci = CreateWindowExW(0, L"Scintilla", L"", WS_CHILD | WS_VSCROLL | WS_HSCROLL | WS_CLIPCHILDREN, 0, 0, 0, 0,
                            g_hwnd, nullptr, inst, nullptr);
    g_sciFn = (SciFnDirect)SendMessageW(g_sci, SCI_GETDIRECTFUNCTION, 0, 0);
    g_sciPtr = (sptr_t)SendMessageW(g_sci, SCI_GETDIRECTPOINTER, 0, 0);
    Sci(SCI_SETTECHNOLOGY, SC_TECHNOLOGY_DEFAULT);  // GDI: faster than software DirectWrite, no GPU driver
    Sci(SCI_SETBUFFEREDDRAW, 0);
    Sci(SCI_SETCODEPAGE, SC_CP_UTF8);
    Sci(SCI_SETUNDOCOLLECTION, 0);
    Sci(SCI_SETMARGINWIDTHN, 1, 0);
    Sci(SCI_SETMARGINLEFT, 0, 4);
    Sci(SCI_SETTABWIDTH, 4);
    Sci(SCI_SETEXTRAASCENT, 1);
    Sci(SCI_SETEXTRADESCENT, 1);
    Sci(SCI_SETSCROLLWIDTH, 1);
    Sci(SCI_SETSCROLLWIDTHTRACKING, 1);
    Sci(SCI_SETCARETSTYLE, CARETSTYLE_INVISIBLE);
    Sci(SCI_ASSIGNCMDKEY, SCK_UP, SCI_LINESCROLLUP);  // viewer: arrows scroll instead of moving a caret
    Sci(SCI_ASSIGNCMDKEY, SCK_DOWN, SCI_LINESCROLLDOWN);
    Sci(SCI_ASSIGNCMDKEY, SCK_HOME, SCI_DOCUMENTSTART);
    Sci(SCI_ASSIGNCMDKEY, SCK_END, SCI_DOCUMENTEND);

    ApplyTheme();
    CreateTarget();
    SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, OnForeground, 0, 0,
                    WINEVENT_OUTOFCONTEXT);

    // Warm-up: show once while cloaked so DWM, Direct2D, Scintilla and the shell pay their first-use
    // costs now instead of on the first Space press. The window then stays WS_VISIBLE but cloaked:
    // uncloaking is much cheaper than ShowWindow.
    Cloak(TRUE);
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    PreviewShow(exe, nullptr, Now());  // info view (shell thumbnail)
    g_kind = K_Text;
    g_lang = LangForFile(L"x.cpp");
    Sci(SCI_SETILEXER, 0, (sptr_t)CreateLexer(g_lang->lexer));
    StyleSci();
    Sci(SCI_SETTEXT, 0, (sptr_t)"int main() { return 0; } // warm-up");
    Layout();
    RedrawWindow(g_hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    g_kind = K_Md;  // loads the fonts and faces Markdown uses (~30 ms cold)
    MdLoad(LR"(# H
## H
### H
**b** *i* ***bi*** `c` [l](u) ~~s~~

```
x
```

| a |
|---|
| b |
)");
    Layout();
    RedrawWindow(g_hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    PdfOpen({}, 0);  // starts the PDF worker and loads the PDF engine
    // The first uncloak makes DWM build the window's visuals and Mica backdrop (~130 ms); do it off-screen.
    RECT r;
    GetWindowRect(g_hwnd, &r);
    g_placing = true;
    SetWindowPos(g_hwnd, nullptr, GetSystemMetrics(SM_XVIRTUALSCREEN) - (r.right - r.left) - 64, r.top, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    g_placing = false;
    Cloak(FALSE);
    DwmFlush();
    PreviewHide(false);
    g_warming = false;
    return true;
}

Timing PreviewShow(const std::wstring& path, HWND source, LONGLONG t0) {
    Timing t{MsSince(t0)};
    LONGLONG start = Now();
    g_source = source;
    if (!g_visible) {
        // Centre on the source window's monitor; PlaceWindow sets the size once the content is known.
        HMONITOR mon = MonitorFromWindow(source ? source : GetForegroundWindow(), MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(mon, &mi);
        RECT wa = mi.rcWork, wr;
        GetWindowRect(g_hwnd, &wr);
        int w = wr.right - wr.left, h = wr.bottom - wr.top;
        g_placing = true;  // a move, not a resize: cheap
        SetWindowPos(g_hwnd, nullptr, (wa.left + wa.right - w) / 2, (wa.top + wa.bottom - h) / 2, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        g_placing = false;
        g_dpi = GetDpiForWindow(g_hwnd);
    }
    LoadContent(path, t0);
    t.load = MsSince(start);
    WaitForContent();
    PlaceWindow();
    Layout();
    if (!g_visible)
        // Topmost: stays above Explorer when Explorer is re-activated (click, Alt+Tab back).
        SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    RedrawWindow(g_hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    if (!g_visible) DefWindowProcW(g_hwnd, WM_NCACTIVATE, TRUE, 0);  // shown without focus: active look anyway
    if (!g_visible && !g_warming) Cloak(FALSE);  // paint first, then reveal: no flash of old content
    g_visible = true;
    t.ready = MsSince(t0);
    DwmFlush();  // returns after DWM composed the frame containing our window
    t.composed = MsSince(t0);
    t.kind = g_kind == K_Text ? L"text" : g_kind == K_Image ? L"image" : g_kind == K_Md ? L"md" : g_kind == K_Pdf ? L"pdf"
             : g_kind == K_Grid ? L"table" : g_kind == K_Font ? L"font" : g_kind == K_Media ? (g_mediaVideo ? L"video" : L"audio") : L"info";
    Log(L"open %-5s explorer %5.2f + load %5.2f -> ready %5.2f ms, on-screen %5.2f ms  %s", t.kind, t.sel, t.load,
        t.ready, t.composed, path.c_str());
    return t;
}

void PreviewHide(bool restoreFocus) {
    if (!g_visible) return;
    bool wasForeground = GetForegroundWindow() == g_hwnd;
    Cloak(TRUE);
    if (wasForeground) ShowWindow(g_hwnd, SW_HIDE);  // never leave keyboard focus on an invisible window
    g_visible = false;
    g_hot = H_None;
    ClearContent();  // release content memory while idle
    Sci(SCI_SETREADONLY, 0);
    Sci(SCI_CLEARALL);
    ShowWindow(g_sci, SW_HIDE);  // a hidden Scintilla loads text ~3x faster (no scrollbar/layout updates)
    if (restoreFocus && wasForeground && IsWindow(g_source)) SetForegroundWindow(g_source);
}

bool PreviewKey(const MSG& m) {
    if (m.message != WM_KEYDOWN || !g_visible) return false;
    if (m.wParam == VK_ESCAPE || m.wParam == VK_SPACE) PreviewHide(true);
    else if (m.wParam == VK_RETURN) OpenFile(false);
    else if (g_kind == K_Media && m.hwnd == g_hwnd && (m.wParam == VK_UP || m.wParam == VK_DOWN))
        MediaSetVolume(MediaVolume() + (m.wParam == VK_UP ? 0.1 : -0.1)), InvalidateRect(g_hwnd, nullptr, FALSE);
    else if (g_kind == K_Media && m.hwnd == g_hwnd && (m.wParam == VK_LEFT || m.wParam == VK_RIGHT))
        MediaSeek(MediaGet().pos + (m.wParam == VK_LEFT ? -5 : 5));
    else if (g_sideFocus && SideW() > 0 && m.hwnd == g_hwnd && m.wParam >= VK_PRIOR && m.wParam <= VK_DOWN) {
        bool back = m.wParam == VK_UP || m.wParam == VK_LEFT || m.wParam == VK_PRIOR;  // sidebar: step page/heading
        GoToItem(m.wParam == VK_HOME ? 0 : m.wParam == VK_END ? (int)SideCount() - 1 : CurrentItem() + (back ? -1 : 1));
    } else if (Scrollable() && m.hwnd == g_hwnd && m.wParam >= VK_PRIOR && m.wParam <= VK_DOWN) {
        float page = View(1) * 0.9f;
        switch (m.wParam) {
            case VK_LEFT: ScrollBy(0, -40); break;
            case VK_RIGHT: ScrollBy(0, 40); break;
            case VK_UP: ScrollBy(1, -40); break;
            case VK_DOWN: ScrollBy(1, 40); break;
            case VK_PRIOR: ScrollBy(1, -page); break;
            case VK_NEXT: ScrollBy(1, page); break;
            case VK_HOME: ScrollTo(1, 0); break;
            case VK_END: ScrollTo(1, 1e9f); break;
        }
    }
    else return false;
    return true;
}
