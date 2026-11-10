// Font preview: the file is loaded into a private DirectWrite collection (WOFF/WOFF2 unpacked by
// DirectWrite itself) and shown as a specimen. System font fallback is switched off, so characters
// the font lacks show as missing instead of silently borrowing from another font.
#include "app.h"
#include <dwrite_3.h>
#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace {

ComPtr<IDWriteFactory5> g_dw5;
ComPtr<IDWriteFontCollection1> g_coll;
ComPtr<IDWriteFontFallback> g_noFallback;
ComPtr<IDWriteInMemoryFontFileLoader> g_mem;  // holds unpacked WOFF data while previewing
std::wstring g_family;
DWRITE_FONT_WEIGHT g_weight;
DWRITE_FONT_STYLE g_style;
DWRITE_FONT_STRETCH g_stretch;
std::wstring g_lines[5];  // specimen text in the scripts the font covers ([3]: the sentence)

std::wstring Name(IDWriteLocalizedStrings* s) {
    UINT32 i = 0, len = 0;
    BOOL found = FALSE;
    wchar_t locale[LOCALE_NAME_MAX_LENGTH];
    if (GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH)) s->FindLocaleName(locale, &i, &found);
    if (!found) s->FindLocaleName(L"en-us", &i, &found);
    if (!found) i = 0;
    s->GetStringLength(i, &len);
    std::wstring n(len, L'\0');
    s->GetString(i, n.data(), len + 1);
    return n;
}

}  // namespace

bool FontLoad(IDWriteFactory* dw, const std::wstring& path, std::wstring& sub) {
    FontClear();
    if (!g_dw5 && FAILED(dw->QueryInterface(IID_PPV_ARGS(&g_dw5)))) return false;
    std::wstring ext = path.substr(path.find_last_of(L'.') + 1);
    for (auto& c : ext) c = towlower(c);
    ComPtr<IDWriteFontFile> file;
    if (ext == L"woff" || ext == L"woff2") {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{};
        GetFileSizeEx(f, &size);
        std::string data(size.QuadPart < (64 << 20) ? (size_t)size.QuadPart : 0, '\0');
        DWORD read = 0;
        ReadFile(f, data.data(), (DWORD)data.size(), &read, nullptr);
        CloseHandle(f);
        ComPtr<IDWriteFontFileStream> stream;
        UINT64 len = 0;
        const void* bytes = nullptr;
        void* ctx = nullptr;
        if (data.empty() ||
            FAILED(g_dw5->UnpackFontFile(ext == L"woff" ? DWRITE_CONTAINER_TYPE_WOFF : DWRITE_CONTAINER_TYPE_WOFF2,
                                         data.data(), read, &stream)) ||
            FAILED(stream->GetFileSize(&len)) || FAILED(stream->ReadFileFragment(&bytes, 0, len, &ctx)))
            return false;
        g_dw5->CreateInMemoryFontFileLoader(&g_mem);
        g_dw5->RegisterFontFileLoader(g_mem.Get());
        HRESULT hr = g_mem->CreateInMemoryFontFileReference(g_dw5.Get(), bytes, (UINT32)len, nullptr, &file);  // copies
        stream->ReleaseFileFragment(ctx);
        if (FAILED(hr)) return false;
    } else if (FAILED(g_dw5->CreateFontFileReference(path.c_str(), nullptr, &file))) {  // memory-mapped, no copy
        return false;
    }
    ComPtr<IDWriteFontSetBuilder1> builder;
    ComPtr<IDWriteFontSet> set;
    ComPtr<IDWriteFontFamily1> family;
    ComPtr<IDWriteFont> font;
    ComPtr<IDWriteFontFace> face;
    ComPtr<IDWriteLocalizedStrings> familyNames, faceNames;
    if (FAILED(g_dw5->CreateFontSetBuilder(&builder)) || FAILED(builder->AddFontFile(file.Get())) ||
        FAILED(builder->CreateFontSet(&set)) || FAILED(g_dw5->CreateFontCollectionFromFontSet(set.Get(), &g_coll)) ||
        g_coll->GetFontFamilyCount() == 0 || FAILED(g_coll->GetFontFamily(0, &family)) ||
        FAILED(family->GetFont(0, &font)) || FAILED(font->CreateFontFace(&face)) ||
        FAILED(family->GetFamilyNames(&familyNames)) || FAILED(font->GetFaceNames(&faceNames))) {
        FontClear();
        return false;
    }
    g_family = Name(familyNames.Get());
    g_weight = font->GetWeight(), g_style = font->GetStyle(), g_stretch = font->GetStretch();
    if (!g_noFallback) {
        ComPtr<IDWriteFontFallbackBuilder> fb;
        if (SUCCEEDED(g_dw5->CreateFontFallbackBuilder(&fb))) fb->CreateFontFallback(&g_noFallback);
    }
    // Specimen in a script the font covers (a CJK or symbol font gets its own characters).
    auto has = [&](UINT32 c) { BOOL b = FALSE; return SUCCEEDED(font->HasCharacter(c, &b)) && b; };
    if (has(L'a')) {
        g_lines[0] = L"ABCDEFGHIJKLMNOPQRSTUVWXYZ", g_lines[1] = L"abcdefghijklmnopqrstuvwxyz";
        g_lines[2] = L"0123456789 .,:;!?()[]{}&@#%*+-=/\"'",
        g_lines[3] = L"The quick brown fox jumps over the lazy dog.";
        g_lines[4] = has(0x4E00) ? L"永和九年，岁在癸丑，暮春之初" : L"";
    } else if (has(0x4E00)) {
        g_lines[0] = L"永和九年，岁在癸丑，暮春之初", g_lines[1] = L"天地玄黄 宇宙洪荒 日月盈昃";
        g_lines[2] = has(0x3042) ? L"あいうえお アイウエオ" : has(0xAC00) ? L"가나다라마바사" : L"一二三四五六七八九十";
        g_lines[3] = L"敏捷的棕色狐狸跳过了懒狗。", g_lines[4] = L"";
    } else {  // show the first characters the font maps
        std::wstring s;
        for (UINT32 c = 0x21; c < 0x30000 && s.size() < 120; c++)
            if (has(c)) {
                if (c > 0xFFFF) s += (wchar_t)(0xD800 + ((c - 0x10000) >> 10)), s += (wchar_t)(0xDC00 + ((c - 0x10000) & 0x3FF));
                else s += (wchar_t)c;
                s += L' ';
            }
        g_lines[0] = g_lines[1] = g_lines[2] = g_lines[4] = L"", g_lines[3] = s;
    }
    std::wstring style = Name(faceNames.Get());
    UINT32 glyphs = face->GetGlyphCount();
    sub = style + L" · " + std::to_wstring(glyphs) + L" glyphs" +
          (set->GetFontCount() > 1 ? L" · " + std::to_wstring(set->GetFontCount()) + L" fonts in file" : L"");
    return true;
}

void FontClear() {
    g_coll.Reset();
    if (g_mem) g_dw5->UnregisterFontFileLoader(g_mem.Get()), g_mem.Reset();
}

float FontPaint(ID2D1RenderTarget* rt, const D2D1_RECT_F& area, float scroll, const MdColors& c) {
    ComPtr<ID2D1SolidColorBrush> text, dim;
    rt->CreateSolidColorBrush(c.text, &text);
    rt->CreateSolidColorBrush(c.dim, &dim);
    float width = std::min(area.right - area.left, 1100.f), x = std::round(area.left + (area.right - area.left - width) / 2);
    float y = area.top + 16 - scroll;
    auto line = [&](const std::wstring& s, float size, ID2D1SolidColorBrush* brush, IDWriteFontCollection* coll,
                    const wchar_t* family, float gap) {
        ComPtr<IDWriteTextFormat> f;
        ComPtr<IDWriteTextLayout> l;
        ComPtr<IDWriteTextLayout2> l2;
        g_dw5->CreateTextFormat(family, coll, coll ? g_weight : DWRITE_FONT_WEIGHT_NORMAL,
                                coll ? g_style : DWRITE_FONT_STYLE_NORMAL, coll ? g_stretch : DWRITE_FONT_STRETCH_NORMAL,
                                size, L"", &f);
        if (!f || FAILED(g_dw5->CreateTextLayout(s.c_str(), (UINT32)s.size(), f.Get(), width, 1e5f, &l))) return;
        if (coll && SUCCEEDED(l.As(&l2))) l2->SetFontFallback(g_noFallback.Get());
        DWRITE_TEXT_METRICS m{};
        l->GetMetrics(&m);
        if (y + m.height > area.top - 50 && y < area.bottom) rt->DrawTextLayout({x, y}, l.Get(), brush);
        y += m.height + gap;
    };
    line(g_family, 44, text.Get(), g_coll.Get(), g_family.c_str(), 20);
    for (int i : {0, 1, 2, 4})
        if (!g_lines[i].empty()) line(g_lines[i], 28, text.Get(), g_coll.Get(), g_family.c_str(), 6);
    y += 18;
    for (float size : {12.f, 16.f, 20.f, 28.f, 36.f, 48.f, 64.f, 80.f}) {
        line(std::to_wstring((int)size), 11, dim.Get(), nullptr, L"Segoe UI", 2);  // label in the UI font
        line(g_lines[3], size, text.Get(), g_coll.Get(), g_family.c_str(), 14);
    }
    return y + scroll - area.top + 16;
}
