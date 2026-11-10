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
#include <windowsx.h>
#include <wrl/client.h>
#include <atomic>
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

enum Kind { K_None, K_Text, K_Image, K_Info };
enum Hot { H_None, H_Open, H_Close };

struct Palette {
    COLORREF role[R_Count];
    COLORREF bg, lineFg, sel;
    D2D1_COLOR_F text, sub, btn, btnHot, btnBorder;
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
    C(0xFFFFFF), C(0xFFFFFF, 0.55f), C(0xFFFFFF, 0.06f), C(0xFFFFFF, 0.10f), C(0xFFFFFF, 0.08f)};
const Palette kLight{
    {Hex(0x1F1F1F), Hex(0x008000), Hex(0x0000FF), Hex(0x267F99), Hex(0x795E26), Hex(0xA31515), Hex(0x098658),
     Hex(0xAF00DB), Hex(0x1F1F1F), Hex(0x800000), Hex(0x0451A5), Hex(0x0000FF), Hex(0x1F1F1F), Hex(0x1F1F1F),
     Hex(0xCD3131), Hex(0x098658), Hex(0xA31515)},
    Hex(0xFFFFFF), Hex(0x8C8C8C), Hex(0xADD6FF),
    C(0x1A1A1A), C(0x000000, 0.55f), C(0xFFFFFF, 0.7f), C(0xF9F9F9, 0.5f), C(0x000000, 0.08f)};

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
SIZE g_sizeDip{960, 680};

Kind g_kind;
const Lang* g_lang;
std::wstring g_path, g_title, g_sub, g_open;
std::vector<std::wstring> g_info;
ComPtr<ID2D1Bitmap> g_bmp;
UINT g_orient = 1;  // EXIF orientation
bool g_imgReduced;  // image decoded below full size to fit the window
Hot g_hot;
D2D1_RECT_F g_openRc, g_closeRc;

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
ComPtr<ID2D1Bitmap> ToD2D(IWICBitmapSource* src) {
    ComPtr<IWICFormatConverter> conv;
    ComPtr<ID2D1Bitmap> bmp;
    if (SUCCEEDED(g_wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom)))
        g_rt->CreateBitmapFromWicBitmap(
            conv.Get(),
            D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                   (float)g_dpi, (float)g_dpi),
            &bmp);
    return bmp;
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

    // Decode no more pixels than the window shows; WM_EXITSIZEMOVE re-decodes after a resize.
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    double maxW = std::max(64.f, rc.right - 2 * kPad * g_dpi / 96);
    double maxH = std::max(64.f, rc.bottom - (kTitleH + kPad) * g_dpi / 96);
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

bool IsImageExt(const std::wstring& path) {
    std::wstring ext = PathFindExtensionW(path.c_str());
    for (auto& c : ext) c = towlower(c);
    return !ext.empty() && wcsstr(L" .jpg .jpeg .jpe .jfif .png .bmp .dib .gif .tif .tiff .ico .webp .heic .heif "
                                  L".avif .jxr .wdp .dds ",
                                  (L" " + ext + L" ").c_str());
}

std::string ToUtf8(const wchar_t* w, int n) {
    std::string s(WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, n, s.data(), (int)s.size(), nullptr, nullptr);
    return s;
}

bool LoadText(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    std::string buf((size_t)std::min<LONGLONG>(size.QuadPart, kMaxText), '\0');
    DWORD read = 0;
    ReadFile(f, buf.data(), (DWORD)buf.size(), &read, nullptr);
    CloseHandle(f);
    buf.resize(read);
    bool truncated = (LONGLONG)read < size.QuadPart;

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
};
std::atomic<int> g_infoGen;

DWORD WINAPI InfoThread(void* p) {
    auto* job = static_cast<InfoJob*>(p);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    {
        ComPtr<IShellItemImageFactory> factory;
        if (SUCCEEDED(SHCreateItemFromParsingName(job->path.c_str(), nullptr, IID_PPV_ARGS(&factory))))
            factory->GetImage({job->px, job->px}, SIIGBF_RESIZETOFIT, &job->bmp);
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
    if (job->gen == g_infoGen && g_kind == K_Info && g_visible) {
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
}

void LoadContent(const std::wstring& path, LONGLONG t0) {
    g_bmp.Reset();
    g_info.clear();
    g_sub.clear();
    g_path = path;
    g_title = PathFindFileNameW(path.c_str());
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa);
    bool dir = fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
    if (!dir && IsImageExt(path) && LoadImage(path)) g_kind = K_Image;
    else if (!dir && LoadText(path)) g_kind = K_Text;
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
    if (g_kind == K_Text)
        SetWindowPos(g_sci, nullptr, 0, top, rc.right, rc.bottom - top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    else
        ShowWindow(g_sci, SW_HIDE);
}

void DrawImage(const D2D1_RECT_F& area) {
    D2D1_SIZE_F s = g_bmp->GetSize();
    bool swap = g_orient >= 5;
    float dw = swap ? s.height : s.width, dh = swap ? s.width : s.height;
    float k = std::min({1.f, (area.right - area.left) / dw, (area.bottom - area.top) / dh});
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
    std::wstring title = g_title + (g_sub.empty() ? L"" : L"    " + g_sub);
    ComPtr<IDWriteTextLayout> tl;
    g_dw->CreateTextLayout(title.c_str(), (UINT32)title.size(), g_fmtTitle.Get(),
                           std::max(0.f, g_openRc.left - 16 - kPad), kTitleH, &tl);
    if (!g_sub.empty()) tl->SetDrawingEffect(sub.Get(), {(UINT32)g_title.size(), (UINT32)(title.size() - g_title.size())});
    g_rt->DrawTextLayout({kPad, 0}, tl.Get(), text.Get());

    D2D1_RECT_F area{kPad, kTitleH, sz.width - kPad, sz.height - kPad};
    if (g_kind == K_Image && g_bmp) DrawImage(area);
    if (g_kind == K_Info) DrawInfo(area, text.Get(), sub.Get());

    HRESULT hrEnd = g_rt->EndDraw();
    if (hrEnd == D2DERR_RECREATE_TARGET) {  // device lost: rebuild and reload bitmaps
        g_rt.Reset();
        CreateTarget();
        if (g_kind != K_Text) LoadContent(g_path, Now());
        InvalidateRect(g_hwnd, nullptr, FALSE);
    }
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

void SaveSize() {
    RECT r;
    GetWindowRect(g_hwnd, &r);
    SIZE s{MulDiv(r.right - r.left, 96, g_dpi), MulDiv(r.bottom - r.top, 96, g_dpi)};
    if (s.cx == g_sizeDip.cx && s.cy == g_sizeDip.cy) return;
    g_sizeDip = s;
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Flick", L"Width", REG_DWORD, &s.cx, sizeof(DWORD));
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Flick", L"Height", REG_DWORD, &s.cy, sizeof(DWORD));
}

// Hide when the user switches to anything other than the source Explorer window or us.
void CALLBACK OnForeground(HWINEVENTHOOK, DWORD, HWND h, LONG, LONG, DWORD, DWORD) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (g_visible && h && pid != GetCurrentProcessId() && GetAncestor(h, GA_ROOT) != g_source) {
        wchar_t cls[64] = L"";
        GetClassNameW(h, cls, 64);
        Log(L"hide: foreground -> %s %p (source %p)", cls, h, g_source);
        PreviewHide(false);
    }
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
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
        case WM_EXITSIZEMOVE:
            if (g_kind == K_Image && g_imgReduced) LoadContent(g_path, Now()), InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_MOUSEMOVE: {
            Hot hot = HitButton(l);
            if (hot != g_hot) g_hot = hot, InvalidateRect(h, nullptr, FALSE);
            TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, h};
            TrackMouseEvent(&tme);
            return 0;
        }
        case WM_MOUSELEAVE:
            g_hot = H_None;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP:
            if (HitButton(l) == H_Close) PreviewHide(true);
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
        case WM_FLICK_TOGGLE:
            if (g_visible) PreviewHide(false);
            else if (auto path = ExplorerSelection((HWND)w); !path.empty()) PreviewShow(path, (HWND)w, (LONGLONG)l);
            return 0;
        case WM_FLICK_FOLLOW:
            if (auto path = ExplorerSelection((HWND)w); g_visible && !path.empty() && path != g_path)
                PreviewShow(path, (HWND)w, (LONGLONG)l);
            return 0;
        case WM_FLICK_HIDE:
            PreviewHide(false);
            return 0;
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
    g_fmtName = Format(ui, 20, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_fmtInfo = Format(ui, 13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_codeFont = HasFont(L"Cascadia Mono") ? "Cascadia Mono" : "Consolas";

    DWORD v, sz = sizeof v;
    if (!RegGetValueW(HKEY_CURRENT_USER, L"Software\\Flick", L"Width", RRF_RT_REG_DWORD, nullptr, &v, &sz))
        g_sizeDip.cx = std::max(320ul, v);
    if (!RegGetValueW(HKEY_CURRENT_USER, L"Software\\Flick", L"Height", RRF_RT_REG_DWORD, nullptr, &v, &sz))
        g_sizeDip.cy = std::max(240ul, v);

    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"FlickPreview";
    RegisterClassExW(&wc);
    // Tool window: no taskbar button flashing in and out on every preview.
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"FlickPreview", L"Flick",
                             WS_OVERLAPPED | WS_CAPTION | WS_THICKFRAME | WS_CLIPCHILDREN, 0, 0,
                             g_sizeDip.cx, g_sizeDip.cy, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) return false;
    g_dpi = GetDpiForWindow(g_hwnd);
    MARGINS margins{-1};
    DwmExtendFrameIntoClientArea(g_hwnd, &margins);
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
    Sci(SCI_SETTECHNOLOGY, SC_TECHNOLOGY_DEFAULT);
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
        // Centre on the source window's monitor at the remembered size.
        HMONITOR mon = MonitorFromWindow(source ? source : GetForegroundWindow(), MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(mon, &mi);
        UINT dx = 96, dy = 96;
        GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
        RECT wa = mi.rcWork;
        int w = std::min(MulDiv(g_sizeDip.cx, dx, 96), (int)(wa.right - wa.left));
        int h = std::min(MulDiv(g_sizeDip.cy, dx, 96), (int)(wa.bottom - wa.top));
        g_placing = true;
        SetWindowPos(g_hwnd, nullptr, (wa.left + wa.right - w) / 2, (wa.top + wa.bottom - h) / 2, w, h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        g_placing = false;
        g_dpi = GetDpiForWindow(g_hwnd);
    }
    LoadContent(path, t0);
    t.load = MsSince(start);
    Layout();
    if (!g_visible)
        SetWindowPos(g_hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    RedrawWindow(g_hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    if (!g_visible && !g_warming) Cloak(FALSE);  // paint first, then reveal: no flash of old content
    g_visible = true;
    t.ready = MsSince(t0);
    DwmFlush();  // returns after DWM composed the frame containing our window
    t.composed = MsSince(t0);
    t.kind = g_kind == K_Text ? L"text" : g_kind == K_Image ? L"image" : L"info";
    Log(L"open %-5s explorer %5.2f + load %5.2f -> ready %5.2f ms, on-screen %5.2f ms  %s", t.kind, t.sel, t.load,
        t.ready, t.composed, path.c_str());
    return t;
}

void PreviewHide(bool restoreFocus) {
    if (!g_visible) return;
    bool wasForeground = GetForegroundWindow() == g_hwnd;
    SaveSize();
    Cloak(TRUE);
    if (wasForeground) ShowWindow(g_hwnd, SW_HIDE);  // never leave keyboard focus on an invisible window
    g_visible = false;
    g_hot = H_None;
    // Release content memory while idle.
    g_bmp.Reset();
    g_info.clear();
    Sci(SCI_SETREADONLY, 0);
    Sci(SCI_CLEARALL);
    ShowWindow(g_sci, SW_HIDE);  // a hidden Scintilla loads text ~3x faster (no scrollbar/layout updates)
    if (restoreFocus && wasForeground && IsWindow(g_source)) SetForegroundWindow(g_source);
}

bool PreviewKey(const MSG& m) {
    if (m.message != WM_KEYDOWN || !g_visible) return false;
    if (m.wParam == VK_ESCAPE || m.wParam == VK_SPACE) PreviewHide(true);
    else if (m.wParam == VK_RETURN) OpenFile(false);
    else return false;
    return true;
}
