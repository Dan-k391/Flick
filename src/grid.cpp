// Table view: CSV/TSV files and archive listings. Rows are virtualised (only visible cells are drawn),
// column widths come from character counts, so opening a 2 MB CSV costs a parse and nothing else.
// Archives are read with libarchive from Windows' own archiveint.dll (Windows 11; what Explorer uses).
#include "app.h"
#include <shlwapi.h>
#include <algorithm>
#include <cmath>
#include <ctime>

using Microsoft::WRL::ComPtr;

namespace {

constexpr float kRowH = 28, kCellPad = 10, kIndent = 18, kIconW = 22, kMinCol = 56, kMaxCol = 420;

IDWriteFactory* g_dw;
ComPtr<IDWriteTextFormat> g_fmtCell, g_fmtCellR, g_fmtHead, g_fmtHeadR, g_fmtIcon;
float g_charW = 7;  // average character width of the cell font, for column sizing
std::unique_ptr<GridData> g_grid;
std::vector<float> g_colX;  // column edges

ComPtr<IDWriteTextFormat> CellFormat(const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight,
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

std::wstring_view Cell(const GridData& g, size_t row, size_t col) {
    auto [off, len] = g.cells[row * g.cols + col];
    return {g.text.data() + off, len};
}

// Numbers right-aligned: a column is numeric when most of its non-empty cells parse as numbers.
void DetectNumeric(GridData& g) {
    g.rightAlign.assign(g.cols, false);
    size_t rows = std::min<size_t>(g.Rows(), 500);
    for (size_t c = 0; c < g.cols; c++) {
        size_t num = 0, total = 0;
        for (size_t r = g.header; r < rows; r++) {  // the header is text, whatever the column holds
            std::wstring_view v = Cell(g, r, c);
            if (v.empty()) continue;
            total++;
            wchar_t buf[64], *end = nullptr;
            if (v.size() >= 64) continue;
            wmemcpy(buf, v.data(), v.size());
            buf[v.size()] = 0;
            wcstod(buf, &end);
            num += end != buf && !*end;
        }
        g.rightAlign[c] = total && num * 10 >= total * 8;
    }
}

void AddCell(GridData& g, std::wstring_view v) {
    g.cells.push_back({(UINT32)g.text.size(), (UINT32)v.size()});
    g.text.append(v);
}

// ---- archives (libarchive in archiveint.dll, loaded on first use)
struct Archive {
    void* (*read_new)();
    int (*support_filter_all)(void*);
    int (*support_format_all)(void*);
    int (*open_filename_w)(void*, const wchar_t*, size_t);
    int (*next_header)(void*, void**);
    int (*free)(void*);
    const wchar_t* (*pathname_w)(void*);
    long long (*size)(void*);
    int (*size_is_set)(void*);
    time_t (*mtime)(void*);
    unsigned (*filetype)(void*);
    int (*is_encrypted)(void*);
    bool Load() {
        HMODULE m = LoadLibraryExW(L"archiveint.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!m) return false;
        auto get = [&](auto& fn, const char* name) { return (fn = (std::remove_reference_t<decltype(fn)>)GetProcAddress(m, name)); };
        return get(read_new, "archive_read_new") && get(support_filter_all, "archive_read_support_filter_all") &&
               get(support_format_all, "archive_read_support_format_all") &&
               get(open_filename_w, "archive_read_open_filename_w") && get(next_header, "archive_read_next_header") &&
               get(free, "archive_read_free") && get(pathname_w, "archive_entry_pathname_w") &&
               get(size, "archive_entry_size") && get(size_is_set, "archive_entry_size_is_set") &&
               get(mtime, "archive_entry_mtime") && get(filetype, "archive_entry_filetype") &&
               get(is_encrypted, "archive_entry_is_encrypted");
    }
};

struct Entry {
    std::wstring path;
    bool dir;
    long long size;  // -1: unknown
    time_t mtime;
};

// Directory-tree order: parents before children, folders before files, names in natural order.
bool TreeLess(const Entry& a, const Entry& b) {
    size_t i = 0, j = 0;
    for (;;) {
        size_t ei = a.path.find(L'/', i), ej = b.path.find(L'/', j);
        std::wstring ca = a.path.substr(i, ei == std::wstring::npos ? std::wstring::npos : ei - i);
        std::wstring cb = b.path.substr(j, ej == std::wstring::npos ? std::wstring::npos : ej - j);
        bool aDir = ei != std::wstring::npos || a.dir, bDir = ej != std::wstring::npos || b.dir;
        if (int c = StrCmpLogicalW(ca.c_str(), cb.c_str()); c || ca != cb) {
            if (aDir != bDir) return aDir;
            return c ? c < 0 : ca < cb;
        }
        if (ei == std::wstring::npos || ej == std::wstring::npos) return ei == std::wstring::npos && ej != std::wstring::npos;
        i = ei + 1, j = ej + 1;
    }
}

std::wstring FormatDate(time_t t) {
    if (t <= 0) return L"";
    FILETIME ft;
    ULONGLONG v = ((ULONGLONG)t + 11644473600ULL) * 10000000ULL;
    ft.dwLowDateTime = (DWORD)v, ft.dwHighDateTime = (DWORD)(v >> 32);
    SYSTEMTIME utc, local;
    FileTimeToSystemTime(&ft, &utc);
    SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
    wchar_t d[64], tm[64];
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &local, nullptr, d, 64, nullptr);
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &local, nullptr, tm, 64);
    return std::wstring(d) + L" " + tm;
}

std::wstring Bytes(long long n) {
    wchar_t s[64];
    StrFormatByteSizeEx((ULONGLONG)n, SFBS_FLAGS_ROUND_TO_NEAREST_DISPLAYED_DIGIT, s, 64);
    return s;
}

}  // namespace

void GridInit(IDWriteFactory* dw, const wchar_t* uiFont, const wchar_t* iconFont) {
    g_dw = dw;
    g_fmtCell = CellFormat(uiFont, 13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    g_fmtCellR = CellFormat(uiFont, 13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
    g_fmtHead = CellFormat(uiFont, 13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
    g_fmtHeadR = CellFormat(uiFont, 13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING);
    g_fmtIcon = CellFormat(iconFont, 13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    const wchar_t sample[] = L"abcdefghijklmnopqrstuvwxyz ABCDEFGHIJKLM 0123456789";
    ComPtr<IDWriteTextLayout> l;
    DWRITE_TEXT_METRICS m{};
    if (SUCCEEDED(dw->CreateTextLayout(sample, (UINT32)wcslen(sample), g_fmtCell.Get(), 1e4f, 100, &l)) &&
        SUCCEEDED(l->GetMetrics(&m)))
        g_charW = m.widthIncludingTrailingWhitespace / wcslen(sample);
}

// RFC 4180: quoted fields may contain the separator, "" and line breaks (shown as spaces).
// Copies runs of characters at once and keeps cells as offsets: ~2 ms for 1.4 MB.
std::unique_ptr<GridData> GridParseCsv(const std::wstring& text, wchar_t sep) {
    auto g = std::make_unique<GridData>();
    size_t n = text.size();
    if (!sep) {  // guess from the first line: the most frequent of , ; tab |
        std::wstring_view first(text.data(), std::min(text.find(L'\n'), n));
        size_t best = 0;
        sep = L',';
        for (wchar_t c : {L',', L';', L'\t', L'|'})
            if (size_t k = std::count(first.begin(), first.end(), c); k > best) best = k, sep = c;
    }
    g->text.reserve(n);
    std::vector<UINT32> rowEnd;  // cells index after each row
    auto endField = [&](size_t start) { g->cells.push_back({(UINT32)start, (UINT32)(g->text.size() - start)}); };
    auto endRow = [&] {
        size_t begin = rowEnd.empty() ? 0 : rowEnd.back();
        if (g->cells.size() - begin == 1 && g->cells.back().second == 0) g->cells.pop_back();  // blank line
        else rowEnd.push_back((UINT32)g->cells.size());
    };
    for (size_t i = 0; i < n;) {
        size_t start = g->text.size();
        if (text[i] == L'"') {
            for (i++; i < n;) {
                size_t q = text.find(L'"', i);
                if (q == std::wstring::npos) q = n;
                g->text.append(text, i, q - i);
                i = q + 1;
                if (i < n && text[i] == L'"') g->text += L'"', i++;  // "" inside quotes
                else break;
            }
            for (size_t k = start; k < g->text.size(); k++)
                if (g->text[k] == L'\r' || g->text[k] == L'\n') g->text[k] = L' ';
        }
        size_t j = i;  // unquoted (rest of the) field
        while (j < n && text[j] != sep && text[j] != L'\n' && text[j] != L'\r') j++;
        g->text.append(text, std::min(i, n), j - std::min(i, n));
        i = j;
        endField(start);
        if (i >= n) break;
        if (text[i] == sep) {
            if (++i == n) endField(g->text.size());  // trailing separator: empty last field
            continue;
        }
        i += text[i] == L'\r' && i + 1 < n && text[i + 1] == L'\n' ? 2 : 1;
        endRow();
    }
    if (rowEnd.empty() || rowEnd.back() != g->cells.size()) endRow();
    if (rowEnd.empty()) return nullptr;
    // Uneven rows (rare): pad every row to the widest.
    UINT32 prev = 0;
    for (UINT32 e : rowEnd) g->cols = std::max(g->cols, e - prev), prev = e;
    if (g->cells.size() != (size_t)g->cols * rowEnd.size()) {
        std::vector<std::pair<UINT32, UINT32>> cells;
        cells.reserve((size_t)g->cols * rowEnd.size());
        prev = 0;
        for (UINT32 e : rowEnd) {
            cells.insert(cells.end(), g->cells.begin() + prev, g->cells.begin() + e);
            cells.resize(cells.size() + g->cols - (e - prev), {0, 0});
            prev = e;
        }
        g->cells = std::move(cells);
    }
    g->header = true;
    DetectNumeric(*g);
    if (g->Rows() < 2) g->rightAlign.assign(g->cols, false);
    size_t rows = g->Rows() - 1;
    g->sub = std::to_wstring(rows) + (rows == 1 ? L" row × " : L" rows × ") + std::to_wstring(g->cols) +
             (g->cols == 1 ? L" column" : L" columns");
    return g;
}

// Worker thread: lists an archive. nullptr when libarchive is missing or can't read the file.
std::unique_ptr<GridData> GridReadArchive(const std::wstring& path) {
    static Archive api;
    static bool ok = api.Load();
    if (!ok) return nullptr;
    void* a = api.read_new();
    api.support_filter_all(a);
    api.support_format_all(a);
    std::vector<Entry> entries;
    bool encrypted = false, partial = false;
    LONGLONG start = Now();
    if (api.open_filename_w(a, path.c_str(), 1 << 16) == 0) {
        void* e;
        for (int r; (r = api.next_header(a, &e)) == 0 || r == -20 /* ARCHIVE_WARN */;) {
            const wchar_t* p = api.pathname_w(e);
            if (!p) continue;
            std::wstring s = p;
            std::replace(s.begin(), s.end(), L'\\', L'/');
            while (s.starts_with(L"./")) s.erase(0, 2);
            bool dir = api.filetype(e) == 0040000 || s.ends_with(L'/');
            while (!s.empty() && s.back() == L'/') s.pop_back();
            if (s.empty()) continue;
            encrypted |= api.is_encrypted(e) > 0;
            entries.push_back({std::move(s), dir, dir || !api.size_is_set(e) ? -1 : api.size(e), api.mtime(e)});
            // Compressed tarballs must be decompressed to reach every header: bound the work.
            if (entries.size() >= 200000 || MsSince(start) > 3000) {
                partial = true;
                break;
            }
        }
    }
    api.free(a);
    if (entries.empty()) return nullptr;

    // Add folders that only appear as part of a path, then sort into a tree.
    std::vector<std::wstring> known;
    for (auto& e : entries) known.push_back(e.path);
    std::sort(known.begin(), known.end());
    for (size_t i = 0, n = entries.size(); i < n; i++)
        for (size_t s = entries[i].path.rfind(L'/'); s != std::wstring::npos && s > 0; s = entries[i].path.rfind(L'/', s - 1)) {
            std::wstring parent = entries[i].path.substr(0, s);
            auto it = std::lower_bound(known.begin(), known.end(), parent);
            if (it != known.end() && *it == parent) break;
            known.insert(it, parent);
            entries.push_back({parent, true, -1, 0});
        }
    std::sort(entries.begin(), entries.end(), TreeLess);

    auto g = std::make_unique<GridData>();
    g->cols = 3;
    AddCell(*g, L"Name"), AddCell(*g, L"Size"), AddCell(*g, L"Modified");
    long long total = 0;
    size_t files = 0;
    for (auto& e : entries) {
        size_t slash = e.path.rfind(L'/');
        AddCell(*g, slash == std::wstring::npos ? e.path : std::wstring_view(e.path).substr(slash + 1));
        AddCell(*g, e.size >= 0 ? Bytes(e.size) : L"");
        AddCell(*g, FormatDate(e.mtime));
        g->indent.push_back((UINT8)std::min<size_t>(std::count(e.path.begin(), e.path.end(), L'/'), 255));
        g->folder.push_back(e.dir);
        if (!e.dir) files++, total += std::max(0LL, e.size);
    }
    g->header = true;
    g->rightAlign = {false, true, false};
    g->sub = std::to_wstring(files) + (files == 1 ? L" file, " : L" files, ") + Bytes(total) +
             (partial ? L" · first " + std::to_wstring(entries.size()) + L" items" : L"") +
             (encrypted ? L" · encrypted" : L"");
    return g;
}

void GridSet(std::unique_ptr<GridData> g) {
    g_grid = std::move(g);
    g_colX.clear();
    if (!g_grid) return;
    std::vector<size_t> chars(g_grid->cols, 0);
    for (size_t r = 0, n = std::min<size_t>(g_grid->Rows(), 1000); r < n; r++)
        for (size_t c = 0; c < g_grid->cols; c++) chars[c] = std::max(chars[c], Cell(*g_grid, r, c).size());
    float extra0 = g_grid->folder.empty() ? 0 : kIconW + kIndent * 3;  // tree indent + icon
    g_colX.push_back(0);
    for (size_t c = 0; c < g_grid->cols; c++)
        g_colX.push_back(g_colX.back() + std::clamp(std::ceil(chars[c] * g_charW * 1.1f) + 2 * kCellPad + (c ? 0 : extra0),
                                                    kMinCol, c ? kMaxCol : kMaxCol + extra0));
}

const GridData* GridGet() { return g_grid.get(); }

D2D1_SIZE_F GridPaint(ID2D1RenderTarget* rt, const D2D1_RECT_F& area, float scrollX, float scrollY, const MdColors& c) {
    const GridData& g = *g_grid;
    ComPtr<ID2D1SolidColorBrush> text, fill;
    rt->CreateSolidColorBrush(c.text, &text);
    rt->CreateSolidColorBrush(c.codeBg, &fill);
    size_t body = g.Rows() - (g.header ? 1 : 0);
    float top = area.top + (g.header ? kRowH : 0);
    // Stretch the last column to the edge when the table is narrower than the view.
    float lastW = std::max(g_colX.back(), area.right - area.left) - g_colX[g.cols - 1];
    float total = g_colX[g.cols - 1] + lastW;

    auto drawRow = [&](size_t r, float y, bool head) {
        for (size_t col = 0; col < g.cols; col++) {
            float x0 = area.left + g_colX[col] - scrollX, x1 = col + 1 == g.cols ? x0 + lastW : area.left + g_colX[col + 1] - scrollX;
            if (x1 < area.left || x0 > area.right) continue;
            std::wstring_view v = Cell(g, r, col);
            float left = x0 + kCellPad;
            if (col == 0 && !g.folder.empty() && !head) {  // archive tree: indent + folder/file icon
                size_t i = r - 1;
                left += g.indent[i] * kIndent;
                wchar_t icon = g.folder[i] ? 0xE8B7 : 0xE8A5;  // Segoe Fluent Icons: Folder, Page
                text->SetColor(g.folder[i] ? c.link : c.dim);
                rt->DrawText(&icon, 1, g_fmtIcon.Get(), {left, y, left + kIconW, y + kRowH}, text.Get());
                text->SetColor(c.text);
                left += kIconW;
            }
            bool right = g.rightAlign[col];
            IDWriteTextFormat* f = head ? (right ? g_fmtHeadR : g_fmtHead).Get() : (right ? g_fmtCellR : g_fmtCell).Get();
            rt->DrawText(v.data(), (UINT32)v.size(), f, {left, y, std::max(left, x1 - kCellPad), y + kRowH}, text.Get());
        }
    };

    rt->PushAxisAlignedClip({area.left, top, area.right, area.bottom}, D2D1_ANTIALIAS_MODE_ALIASED);
    size_t first = (size_t)std::max(0.f, std::floor(scrollY / kRowH));
    for (size_t i = first; i < body; i++) {
        float y = top + i * kRowH - scrollY;
        if (y > area.bottom) break;
        if (i % 2) rt->FillRectangle({area.left - scrollX, y, area.left - scrollX + total, y + kRowH}, fill.Get());
        drawRow(i + (g.header ? 1 : 0), y, false);
    }
    rt->PopAxisAlignedClip();
    if (g.header) {  // sticky header
        rt->PushAxisAlignedClip({area.left, area.top, area.right, top}, D2D1_ANTIALIAS_MODE_ALIASED);
        rt->FillRectangle({area.left, area.top, area.right, top}, fill.Get());
        drawRow(0, area.top, true);
        fill->SetColor(c.rule);
        for (size_t col = 1; col < g.cols; col++) {
            float x = area.left + g_colX[col] - scrollX;
            rt->FillRectangle({x, area.top + 6, x + 1, top - 6}, fill.Get());
        }
        rt->FillRectangle({area.left, top - 1, area.right, top}, fill.Get());
        rt->PopAxisAlignedClip();
    }
    return {total, top - area.top + body * kRowH + 8};
}
