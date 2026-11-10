// SVG: lunasvg renders straight to a premultiplied BGRA bitmap at the size it is shown (vector:
// re-rendered after a resize). Text uses Windows fonts, memory-mapped from disk on first use, so
// they cost shared file pages instead of private memory.
#include "app.h"
#include <lunasvg.h>
#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace {

void MapFont(const char* family, bool bold, bool italic, const wchar_t* file) {
    wchar_t path[MAX_PATH];
    ExpandEnvironmentStringsW((std::wstring(L"%WINDIR%\\Fonts\\") + file).c_str(), path, MAX_PATH);
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    HANDLE m = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
    CloseHandle(f);
    if (!m) return;
    void* view = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(m);
    if (view &&
        !lunasvg_add_font_face_from_data(family, bold, italic, view, (size_t)size.QuadPart,
                                         [](void* v) { UnmapViewOfFile(v); }, view))
        UnmapViewOfFile(view);
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
        for (const char* name : f.names)
            for (int i = 0; i < 4; i++) MapFont(name, i & 1, i & 2, f.files[i]);
}

}  // namespace

ComPtr<IWICBitmap> SvgRender(const std::wstring& path, float maxW, float maxH, IWICImagingFactory* wic, UINT& w,
                             UINT& h) {
    std::string data;
    if (path.size() > 5 && !_wcsicmp(path.c_str() + path.size() - 5, L".svgz")) {
        if (!ArchiveReadRaw(path, data, 64 << 20)) return nullptr;
    } else {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (f == INVALID_HANDLE_VALUE) return nullptr;
        LARGE_INTEGER size{};
        GetFileSizeEx(f, &size);
        data.resize((size_t)std::min<LONGLONG>(size.QuadPart, 64 << 20));
        DWORD read = 0;
        ReadFile(f, data.data(), (DWORD)data.size(), &read, nullptr);
        CloseHandle(f);
        data.resize(read);
    }
    if (data.starts_with("\xEF\xBB\xBF")) data.erase(0, 3);  // UTF-8 BOM: lunasvg rejects it
    if (data.find("<text") != std::string::npos) LoadFonts();
    auto doc = lunasvg::Document::loadFromData(data.data(), data.size());
    if (!doc) return nullptr;
    float dw = doc->width(), dh = doc->height();
    if (dw <= 0 || dh <= 0) dw = dh = 512;  // no size and no viewBox
    w = (UINT)std::lround(dw), h = (UINT)std::lround(dh);
    if (maxW <= 0) return nullptr;  // size only
    float s = std::min(maxW / dw, maxH / dh);  // vector: scaled to fit, up or down
    int bw = std::max(1, (int)std::lround(dw * s)), bh = std::max(1, (int)std::lround(dh * s));
    lunasvg::Bitmap bmp = doc->renderToBitmap(bw, bh, 0);
    ComPtr<IWICBitmap> out;
    if (bmp.isNull() || FAILED(wic->CreateBitmapFromMemory(bw, bh, GUID_WICPixelFormat32bppPBGRA, bmp.stride(),
                                                             bmp.stride() * bh, bmp.data(), &out)))
        return nullptr;
    return out;
}
