// SVG: lunasvg renders straight to a premultiplied BGRA bitmap at the size it is shown (vector:
// re-rendered after a resize). Text uses Windows fonts, memory-mapped from disk on first use, so
// they cost shared file pages instead of private memory.
#include "app.h"
#include <lunasvg.h>
#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace {

void MapFont(std::initializer_list<const char*> families, bool bold, bool italic, const wchar_t* file) {
    wchar_t path[MAX_PATH];
    ExpandEnvironmentStringsW((std::wstring(L"%WINDIR%\\Fonts\\") + file).c_str(), path, MAX_PATH);
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    HANDLE m = GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart <= UINT_MAX
                   ? CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr) : nullptr;
    CloseHandle(f);
    if (!m) return;
    void* view = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(m);
    if (!view) return;
    // Each alias gets its own face, but all aliases share the file mapping.
    using Mapping = std::shared_ptr<void>;
    Mapping mapping(view, [](void* v) { UnmapViewOfFile(v); });
    for (const char* family : families)
        lunasvg_add_font_face_from_data(family, bold, italic, view, (size_t)size.QuadPart,
                                       [](void* v) { delete static_cast<Mapping*>(v); }, new Mapping(mapping));
}

// Generic CSS families and the fonts SVGs ask for most, mapped to what Windows ships.
void LoadFonts() {
    static bool done;
    if (done) return;
    done = true;
    struct Family {
        std::initializer_list<const char*> names;
        const wchar_t* files[4];  // regular, bold, italic, bold italic
    };
    const Family families[] = {
        {{"", "Segoe UI", "system-ui"}, {L"segoeui.ttf", L"segoeuib.ttf", L"segoeuii.ttf", L"segoeuiz.ttf"}},
        {{"sans-serif", "Arial", "Helvetica", "Liberation Sans"}, {L"arial.ttf", L"arialbd.ttf", L"ariali.ttf", L"arialbi.ttf"}},
        {{"serif", "Times New Roman", "Times", "Liberation Serif"}, {L"times.ttf", L"timesbd.ttf", L"timesi.ttf", L"timesbi.ttf"}},
        {{"monospace", "Consolas", "Courier New", "Courier", "Menlo", "Monaco"},
         {L"consola.ttf", L"consolab.ttf", L"consolai.ttf", L"consolaz.ttf"}},
        {{"Verdana"}, {L"verdana.ttf", L"verdanab.ttf", L"verdanai.ttf", L"verdanaz.ttf"}},
        {{"Georgia"}, {L"georgia.ttf", L"georgiab.ttf", L"georgiai.ttf", L"georgiaz.ttf"}},
    };
    for (auto& f : families)
        for (int i = 0; i < 4; i++) MapFont(f.names, i & 1, i & 2, f.files[i]);
}

}  // namespace

ComPtr<IWICBitmap> SvgRender(const std::wstring& path, float maxW, float maxH, IWICImagingFactory* wic, UINT& w,
                             UINT& h, float scaleLimit) {
    std::string data;
    if (path.size() > 5 && !_wcsicmp(path.c_str() + path.size() - 5, L".svgz")) {
        if (!ArchiveReadRaw(path, data, 64 << 20)) return nullptr;
    } else {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (f == INVALID_HANDLE_VALUE) return nullptr;
        LARGE_INTEGER size{};
        DWORD read = 0;
        bool ok = GetFileSizeEx(f, &size);
        if (ok) {
            data.resize((size_t)std::min<LONGLONG>(size.QuadPart, 64 << 20));
            ok = ReadFile(f, data.data(), (DWORD)data.size(), &read, nullptr);
        }
        CloseHandle(f);
        if (!ok) return nullptr;
        data.resize(read);
    }
    if (data.starts_with("\xEF\xBB\xBF")) data.erase(0, 3);  // UTF-8 BOM: lunasvg rejects it
    if (data.find("<text") != std::string::npos) LoadFonts();
    auto doc = lunasvg::Document::loadFromData(data.data(), data.size());
    if (!doc) return nullptr;
    double dw = doc->width(), dh = doc->height();
    if (!std::isfinite(dw) || !std::isfinite(dh) || dw <= 0 || dh <= 0 || dw > UINT_MAX || dh > UINT_MAX)
        return nullptr;
    w = (UINT)std::llround(dw), h = (UINT)std::llround(dh);
    if (maxW <= 0) return nullptr;  // size only
    // Bound the raster even for SVGs with an extreme aspect ratio embedded in a document.
    double s = std::min({std::min<double>(maxW, 30000) / dw, std::min<double>(maxH, 30000) / dh,
                         std::sqrt((64.0 * (1 << 20)) / (dw * dh))});
    if (scaleLimit > 0) s = std::min(s, (double)scaleLimit);
    int bw = std::max(1, (int)std::lround(dw * s)), bh = std::max(1, (int)std::lround(dh * s));
    ComPtr<IWICBitmap> out;
    ComPtr<IWICBitmapLock> lock;
    WICRect all{0, 0, bw, bh};
    UINT stride, size;
    BYTE* pixels;
    if (FAILED(wic->CreateBitmap(bw, bh, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &out)) ||
        FAILED(out->Lock(&all, WICBitmapLockWrite, &lock)) || FAILED(lock->GetStride(&stride)) ||
        FAILED(lock->GetDataPointer(&size, &pixels)))
        return nullptr;
    lunasvg::Bitmap bmp(pixels, bw, bh, stride);
    if (bmp.isNull()) return nullptr;
    bmp.clear(0);
    doc->render(bmp, lunasvg::Matrix((float)(bw / dw), 0, 0, (float)(bh / dh), 0, 0));
    return out;
}
