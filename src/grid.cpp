// Table view: CSV/TSV files and archive listings. Rows are virtualised (only visible cells are drawn),
// column widths come from character counts, so opening a 2 MB CSV costs a parse and nothing else.
// Archives are read with libarchive from Windows' own archiveint.dll (Windows 11; what Explorer uses).
#include "app.h"
#include <shlwapi.h>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <functional>
#include <map>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

namespace {

constexpr float kRowH = 28, kCellPad = 10, kIndent = 18, kIconW = 22, kChevronW = 16, kMinCol = 56, kMaxCol = 420;

IDWriteFactory* g_dw;
ComPtr<IDWriteTextFormat> g_fmtCell, g_fmtCellR, g_fmtHead, g_fmtHeadR, g_fmtIcon, g_fmtChevron, g_fmtCenter;
std::vector<D2D1_RECT_F> g_tabRc;  // sheets: tab rectangles as last painted (clipped to the strip)
D2D1_RECT_F g_tabStrip, g_tabPrev, g_tabNext;  // sheets: the tab strip, its arrow buttons (empty: all tabs fit)
float g_tabScroll, g_tabMax;
bool g_tabReveal;  // scroll the active tab into view on the next paint
std::vector<float> g_rowY;  // sheets: row edges
std::unordered_map<UINT64, UINT32> g_mergeAt;  // sheets: cell (row << 32 | column) inside a merge -> merge index
std::vector<IDWriteTextFormat*> g_styleFmt;     // sheets: per style, from g_fonts
std::map<std::wstring, ComPtr<IDWriteTextFormat>> g_fonts;  // family|size|bold|italic, kept across previews
std::vector<ComPtr<ID2D1Bitmap>> g_picBmp;
ComPtr<ID2D1StrokeStyle> g_dash;
float g_charW = 7;  // average character width of the cell font, for column sizing
std::unique_ptr<GridData> g_grid;
std::vector<float> g_colX;  // column edges
std::vector<bool> g_open;   // trees: per body row, folder expanded (all collapsed at first)
std::vector<UINT32> g_shown;  // body rows currently visible (children of collapsed folders hidden)

bool HasChildren(const GridData& g, size_t i) { return i + 1 < g.indent.size() && g.indent[i + 1] > g.indent[i]; }

void Rebuild() {
    const GridData& g = *g_grid;
    size_t body = g.Rows() - (g.header ? 1 : 0);
    g_shown.clear();
    for (size_t i = 0; i < body; i++) {
        g_shown.push_back((UINT32)i);
        if (!g.folder.empty() && g.folder[i] && !g_open[i])  // skip the collapsed subtree
            for (UINT8 depth = g.indent[i]; i + 1 < body && g.indent[i + 1] > depth;) i++;
    }
}

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
    int (*support_format_raw)(void*);
    long long (*read_data)(void*, void*, size_t);
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
               get(is_encrypted, "archive_entry_is_encrypted") &&
               get(support_format_raw, "archive_read_support_format_raw") && get(read_data, "archive_read_data");
    }
};

Archive* Api() {
    static Archive api;
    static bool ok = api.Load();
    return ok ? &api : nullptr;
}

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
        size_t ei = std::min(a.path.find(L'/', i), a.path.size()), ej = std::min(b.path.find(L'/', j), b.path.size());
        std::wstring_view ca(a.path.data() + i, ei - i), cb(b.path.data() + j, ej - j);
        bool aDir = ei < a.path.size() || a.dir, bDir = ej < b.path.size() || b.dir;
        if (ca != cb) {
            if (aDir != bDir) return aDir;
            // Natural order ("file2" < "file10") without copying the names.
            int c = CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS, ca.data(), (int)ca.size(),
                                    cb.data(), (int)cb.size(), nullptr, nullptr, 0);
            return c == CSTR_EQUAL ? ca < cb : c == CSTR_LESS_THAN;
        }
        if (ei == a.path.size() || ej == b.path.size()) return ei == a.path.size() && ej < b.path.size();
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
    g_fmtChevron = CellFormat(iconFont, 9, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_fmtCenter = CellFormat(uiFont, 12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
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
namespace {
std::unique_ptr<GridData> BuildTree(std::vector<Entry> entries, bool partial, bool encrypted);
}

std::unique_ptr<GridData> GridReadArchive(const std::wstring& path, const std::atomic<int>& gen, int mine) {
    Archive* pa = Api();
    if (!pa) return nullptr;
    Archive& api = *pa;
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
            if (entries.size() >= 200000 || MsSince(start) > 3000 || gen != mine) {
                partial = true;
                break;
            }
        }
    }
    api.free(a);
    if (entries.empty()) return nullptr;
    return BuildTree(std::move(entries), partial, encrypted);
}

// Worker thread: a folder's contents as a tree, breadth-first so a huge folder still shows its top
// levels within the time budget (it must arrive while the window waits for it, ~100 ms). Hidden and
// system items are left out (like Explorer's default view); links are not followed.
std::unique_ptr<GridData> GridReadFolder(const std::wstring& root, const std::atomic<int>& gen, int mine) {
    std::vector<Entry> entries;
    std::vector<std::wstring> queue{L""};  // folders to list, relative, '/'-separated
    bool partial = false;
    LONGLONG start = Now();
    for (size_t q = 0; q < queue.size() && !partial; q++) {
        std::wstring rel = queue[q], dir = root + L"\\" + rel;
        std::replace(dir.begin(), dir.end(), L'/', L'\\');
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW((dir + (rel.empty() ? L"*" : L"\\*")).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                    nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..") ||
                (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)))
                continue;
            std::wstring path = rel.empty() ? fd.cFileName : rel + L"/" + fd.cFileName;
            bool isDir = fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
            ULONGLONG ft = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
            time_t mtime = ft > 116444736000000000ULL ? (time_t)((ft - 116444736000000000ULL) / 10000000ULL) : 0;
            long long size = isDir ? -1 : (long long)(((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
            if (isDir && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) queue.push_back(path);
            entries.push_back({std::move(path), isDir, size, mtime});
            if (entries.size() >= 10000 || MsSince(start) > 60 || gen != mine) partial = true;
        } while (!partial && FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (entries.empty()) return nullptr;
    return BuildTree(std::move(entries), partial, false);
}

namespace {
// Tree order (folders first, natural names), indent per depth, Name/Size/Modified columns.
std::unique_ptr<GridData> BuildTree(std::vector<Entry> entries, bool partial, bool encrypted) {
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
    size_t files = 0, folders = 0;
    for (auto& e : entries) {
        size_t slash = e.path.rfind(L'/');
        AddCell(*g, slash == std::wstring::npos ? e.path : std::wstring_view(e.path).substr(slash + 1));
        AddCell(*g, e.size >= 0 ? Bytes(e.size) : L"");
        AddCell(*g, FormatDate(e.mtime));
        g->indent.push_back((UINT8)std::min<size_t>(std::count(e.path.begin(), e.path.end(), L'/'), 255));
        g->paths.push_back(e.path);
        g->folder.push_back(e.dir);
        if (e.dir) folders++;
        else files++, total += std::max(0LL, e.size);
    }
    g->header = true;
    g->rightAlign = {false, true, false};
    g->sub = (folders ? std::to_wstring(folders) + (folders == 1 ? L" folder, " : L" folders, ") : L"") +
             std::to_wstring(files) + (files == 1 ? L" file, " : L" files, ") + Bytes(total) +
             (partial ? L" · first " + std::to_wstring(entries.size()) + L" items" : L"") +
             (encrypted ? L" · encrypted" : L"");
    return g;
}
}  // namespace

void GridSet(std::unique_ptr<GridData> g) {
    g_grid = std::move(g);
    g_colX.clear(), g_rowY.clear(), g_mergeAt.clear(), g_styleFmt.clear(), g_picBmp.clear();
    if (!g_grid) return;
    g_open.assign(g_grid->folder.size(), false);
    // A single top-level folder (typical for archives) starts expanded: nothing to see otherwise.
    if (!g_open.empty() && g_grid->folder[0] && std::count(g_grid->indent.begin(), g_grid->indent.end(), 0) == 1)
        g_open[0] = true;
    Rebuild();
    const GridData& d = *g_grid;
    if (d.sheet) {
        g_rowY.assign(1, 0);
        for (size_t r = 0, n = std::max(d.Rows(), d.rowH.size()); r < n; r++) g_rowY.push_back(g_rowY.back() + (r < d.rowH.size() ? d.rowH[r] : d.defRowH));
        for (UINT32 k = 0; k < d.merges.size(); k++)  // cells with text only: merges past the grid change nothing here
            for (UINT64 r = d.merges[k][0]; r <= d.merges[k][2] && r < d.Rows() && d.merges[k][1] < d.cols + 64 && g_mergeAt.size() < 1000000; r++)
                for (UINT64 c = d.merges[k][1]; c <= d.merges[k][3] && c < d.cols + 64; c++) g_mergeAt[r << 32 | c] = k;
        g_styleFmt.assign(d.styles.size(), nullptr);
        g_picBmp.assign(d.pics.size(), nullptr);
        g_tabReveal = true;
        if (!d.colW.empty()) {
            g_colX.push_back(0);
            for (size_t c = 0, n = std::max<size_t>(d.cols, d.colW.size()); c < n; c++)
                g_colX.push_back(g_colX.back() + (c < d.colW.size() ? d.colW[c] : d.defColW));
            return;
        }
    }
    std::vector<size_t> chars(g_grid->cols, 0);
    for (size_t r = 0, n = std::min<size_t>(g_grid->Rows(), 1000); r < n; r++)
        for (size_t c = 0; c < g_grid->cols; c++) chars[c] = std::max(chars[c], Cell(*g_grid, r, c).size());
    float extra0 = g_grid->folder.empty() ? 0 : kChevronW + kIconW + kIndent * 3;  // tree indent + chevron + icon
    g_colX.push_back(0);
    for (size_t c = 0; c < g_grid->cols; c++)
        g_colX.push_back(g_colX.back() + std::clamp(std::ceil(chars[c] * g_charW * 1.1f) + 2 * kCellPad + (c ? 0 : extra0),
                                                    kMinCol, c ? kMaxCol : kMaxCol + extra0));
}

const GridData* GridGet() { return g_grid.get(); }

void GridSetCell(size_t bodyRow, size_t col, const std::wstring& value) {
    if (!g_grid || col >= g_grid->cols) return;
    size_t i = (bodyRow + (g_grid->header ? 1 : 0)) * g_grid->cols + col;
    if (i >= g_grid->cells.size()) return;
    g_grid->cells[i] = {(UINT32)g_grid->text.size(), (UINT32)value.size()};
    g_grid->text += value;
}
size_t GridShownRows() { return g_shown.size(); }

// A click in the table: expands/collapses the folder row under it. True if the view changed.
bool GridClick(const D2D1_RECT_F& area, float scrollY, D2D1_POINT_2F pt) {
    if (!g_grid || g_grid->folder.empty()) return false;
    float top = area.top + (g_grid->header ? kRowH : 0);
    if (pt.y < top || pt.y >= area.bottom) return false;
    size_t k = (size_t)((pt.y - top + scrollY) / kRowH);
    if (k >= g_shown.size()) return false;
    size_t i = g_shown[k];
    if (!g_grid->folder[i] || !HasChildren(*g_grid, i)) return false;
    g_open[i] = !g_open[i];
    Rebuild();
    return true;
}

// ---- sheets: geometry in DIPs from A1's top-left corner; past the grid's extent, default sizes
namespace {
float ColX(const GridData& g, size_t c) { return c < g_colX.size() ? g_colX[c] : g_colX.back() + (c - (g_colX.size() - 1)) * g.defColW; }
float RowY(const GridData& g, size_t r) { return r < g_rowY.size() ? g_rowY[r] : g_rowY.back() + (r - (g_rowY.size() - 1)) * g.defRowH; }
size_t At(const std::vector<float>& edges, float def, float v) {  // index of the column/row containing v
    if (v >= edges.back()) return edges.size() - 1 + (size_t)((v - edges.back()) / std::max(def, 1.f));
    return (size_t)std::max<ptrdiff_t>(0, std::upper_bound(edges.begin(), edges.end(), v) - edges.begin() - 1);
}

IDWriteTextFormat* StyleFormat(const GridData& g, size_t i) {
    if (g_styleFmt[i]) return g_styleFmt[i];
    const SheetStyle& s = g.styles[i];
    float size = s.size >= 1 ? s.size : 11 * 96 / 72.f;
    std::wstring key = s.font + L"|" + std::to_wstring(std::lround(size * 100)) + (s.bold ? L"b" : L"") + (s.italic ? L"i" : L"");
    ComPtr<IDWriteTextFormat>& f = g_fonts[key];
    if (!f) {
        FontFace face = ResolveFont(s.font);
        g_dw->CreateTextFormat(face.family.c_str(), nullptr, s.bold ? DWRITE_FONT_WEIGHT_BOLD : face.weight,
                               s.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, face.stretch, size, L"", &f);
    }
    return g_styleFmt[i] = f ? f.Get() : g_fmtCell.Get();
}
}  // namespace

// Spreadsheets as Excel shows them: column letters, row numbers, cells with their formats (fills, borders, fonts,
// alignment, wrapping, merges, text running over empty neighbours), pictures, frozen panes, sheet tabs below.
// Cells without a fill show the window; automatic (and black) text and borders take the theme's text colour.
D2D1_SIZE_F SheetPaint(ID2D1RenderTarget* rt, const D2D1_RECT_F& area, float sx, float sy, const MdColors& c) {
    constexpr float kHead = 22, kTabH = 30, kArrowW = 28;
    const GridData& g = *g_grid;
    bool dark = c.text.r > 0.5f;
    ComPtr<ID2D1SolidColorBrush> text, fill, line;
    rt->CreateSolidColorBrush(c.text, &text);
    rt->CreateSolidColorBrush(c.codeBg, &fill);
    rt->CreateSolidColorBrush(c.rule, &line);
    if (!g_dash) {
        ComPtr<ID2D1Factory> f;
        rt->GetFactory(&f);
        f->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                         D2D1_LINE_JOIN_MITER, 10, D2D1_DASH_STYLE_DASH), nullptr, 0, &g_dash);
    }
    size_t rows = g.Rows();
    float y0 = area.top + kHead, bottom = area.bottom - kTabH;
    size_t lastRow = At(g_rowY, g.defRowH, sy + bottom - y0) + 1;
    float headW = std::max(40.f, std::to_wstring(lastRow).size() * g_charW + 16), x0 = area.left + headW;
    auto cellText = [&](size_t r, size_t col) { return r < rows && col < g.cols ? Cell(g, r, col) : std::wstring_view{}; };
    auto styleAt = [&](size_t r, size_t col) -> size_t { return r < rows && col < g.cols && !g.xf.empty() ? g.xf[r * g.cols + col] : 0; };
    auto mergeAt = [&](size_t r, size_t col) -> const std::array<UINT32, 4>* {
        if (g_mergeAt.empty()) return nullptr;
        auto it = g_mergeAt.find((UINT64)r << 32 | col);
        return it == g_mergeAt.end() ? nullptr : &g.merges[it->second];
    };
    auto empty = [&](size_t r, size_t col) { return cellText(r, col).empty() && !mergeAt(r, col); };
    // White fills in dark mode are the sheet's "no gridlines" look, not meant as white blocks.
    auto fillOf = [&](const SheetStyle& s) { return dark && (s.fill & 0xFFFFFF) == 0xFFFFFF ? 0 : s.fill; };
    D2D1_COLOR_F autoLine = dark ? D2D1_COLOR_F{c.text.r, c.text.g, c.text.b, 0.7f} : D2D1_COLOR_F{0, 0, 0, 1};
    // The colour drawn; 0: automatic (unset or black, or in dark mode near-black: the theme's colour).
    auto ink = [&](UINT32 col) -> UINT32 { return !(col & 0xFFFFFF) ? 0 : dark ? DarkInk(col) : col; };

    // Frozen panes: columns/rows before the split stay put (unless they don't leave room for the rest).
    size_t fc = g.frozenCols, fr = g.frozenRows;
    if (x0 + ColX(g, fc) > area.right - 100) fc = 0;
    if (y0 + RowY(g, fr) > bottom - 60) fr = 0;
    float fx = ColX(g, fc), fy = RowY(g, fr);
    struct Span { bool on; float lo, hi, d; size_t i0, i1; };  // screen range, offset, cells
    auto span = [&](bool scrolled, size_t frozen, float f, float origin, float end, float scroll, const std::vector<float>& edges, float def) {
        Span s{scrolled || frozen, scrolled ? origin + f : origin, scrolled ? end : std::min(end, origin + f), origin - (scrolled ? scroll : 0)};
        s.on = s.on && s.hi > s.lo;
        s.i0 = scrolled ? At(edges, def, f + scroll) : 0;
        s.i1 = scrolled ? At(edges, def, s.hi - s.d) + 1 : frozen;
        return s;
    };
    Span xs[2] = {span(false, fc, fx, x0, area.right, sx, g_colX, g.defColW), span(true, fc, fx, x0, area.right, sx, g_colX, g.defColW)};
    Span ys[2] = {span(false, fr, fy, y0, bottom, sy, g_rowY, g.defRowH), span(true, fr, fy, y0, bottom, sy, g_rowY, g.defRowH)};

    // A border side: 1 thin, 2 medium, 3 thick, 4 double, 5 dashed. Centred on the gridline [p, p + 1].
    auto edge = [&](UINT8 kind, UINT32 col, bool horiz, float p, float a, float b) {
        if (!kind) return;
        UINT32 k = ink(col);
        line->SetColor(k ? Rgb(k) : autoLine);
        if (kind == 5) {
            D2D1_POINT_2F p0 = horiz ? D2D1_POINT_2F{a, p + .5f} : D2D1_POINT_2F{p + .5f, a}, p1 = horiz ? D2D1_POINT_2F{b, p + .5f} : D2D1_POINT_2F{p + .5f, b};
            rt->DrawLine(p0, p1, line.Get(), 1, g_dash.Get());
            return;
        }
        float o = kind == 1 ? 0 : -1, w = kind == 2 ? 2 : kind == 3 ? 3 : 1;
        for (int k = 0; k < (kind == 4 ? 2 : 1); k++, o += 2)
            rt->FillRectangle(horiz ? D2D1_RECT_F{a, p + o, b + 1, p + o + w} : D2D1_RECT_F{p + o, a, p + o + w, b + 1}, line.Get());
    };
    // Text of one cell (or merge) in its box, aligned and clipped like Excel.
    auto drawText = [&](std::wstring_view v, size_t si, bool num, D2D1_RECT_F box, D2D1_RECT_F cell) {
        const SheetStyle& s = g.styles[si];
        int h = s.h ? s.h : num ? 3 : 1;
        float pad = 2 + s.indent * 9.f;
        box.left += h == 1 ? pad : 2, box.right -= h == 3 ? pad : 2;
        ComPtr<IDWriteTextLayout> l;
        if (box.right <= box.left || box.bottom <= box.top ||
            FAILED(g_dw->CreateTextLayout(v.data(), (UINT32)v.size(), StyleFormat(g, si), box.right - box.left, box.bottom - box.top - 1, &l)))
            return;
        l->SetTextAlignment(h == 2 ? DWRITE_TEXT_ALIGNMENT_CENTER : h == 3 ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
        l->SetParagraphAlignment(s.v == 0 ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR : s.v == 1 ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER : DWRITE_PARAGRAPH_ALIGNMENT_FAR);
        l->SetWordWrapping(s.wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        if (s.underline) l->SetUnderline(TRUE, {0, (UINT32)v.size()});
        if (s.strike) l->SetStrikethrough(TRUE, {0, (UINT32)v.size()});
        UINT32 k = ink(s.color);
        text->SetColor(fillOf(s) ? (s.color & 0xFFFFFF ? Rgb(s.color) : D2D1_COLOR_F{0, 0, 0, 1}) : k ? Rgb(k) : c.text);
        // A clip per text doubles the cost of drawing it (D2D1_DRAW_TEXT_OPTIONS_CLIP too): only text sticking out gets one.
        DWRITE_TEXT_METRICS m{};
        l->GetMetrics(&m);
        bool clip = m.left < 0 || m.top < 0 || m.left + m.width > m.layoutWidth + .5f || m.top + m.height > m.layoutHeight + .5f;
        if (clip) rt->PushAxisAlignedClip(cell, D2D1_ANTIALIAS_MODE_ALIASED);
        rt->DrawTextLayout({box.left, box.top}, l.Get(), text.Get());
        if (clip) rt->PopAxisAlignedClip();
    };

    for (const Span& ya : ys)
        for (const Span& xa : xs) {
            if (!xa.on || !ya.on) continue;
            D2D1_RECT_F clip{xa.lo, ya.lo, xa.hi, ya.hi};
            auto X = [&](size_t col) { return xa.d + ColX(g, col); };
            auto Y = [&](size_t r) { return ya.d + RowY(g, r); };
            rt->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
            line->SetColor(c.rule);
            if (g.gridLines) {
                for (size_t col = xa.i0; col <= xa.i1; col++) rt->FillRectangle({X(col), clip.top, X(col) + 1, clip.bottom}, line.Get());
                for (size_t r = ya.i0; r <= ya.i1; r++) rt->FillRectangle({clip.left, Y(r), clip.right, Y(r) + 1}, line.Get());
            }
            std::vector<const std::array<UINT32, 4>*> ms;
            for (auto& m : g.merges)
                if (m[0] < ya.i1 && m[2] >= ya.i0 && m[1] < xa.i1 && m[3] >= xa.i0) ms.push_back(&m);
            auto mergeRc = [&](const std::array<UINT32, 4>& m) { return D2D1_RECT_F{X(m[1]), Y(m[0]), X(m[3] + 1), Y(m[2] + 1)}; };
            // Fills (they hide the gridlines around them); merges without one hide the gridlines inside.
            for (auto* m : ms) {
                D2D1_RECT_F rc = mergeRc(*m);
                if (UINT32 f = fillOf(g.styles[styleAt((*m)[0], (*m)[1])])) fill->SetColor(Rgb(f)), rt->FillRectangle({rc.left, rc.top, rc.right + 1, rc.bottom + 1}, fill.Get());
                else {
                    rt->PushAxisAlignedClip({rc.left + 1, rc.top + 1, rc.right, rc.bottom}, D2D1_ANTIALIAS_MODE_ALIASED);
                    rt->Clear({0, 0, 0, 0});
                    rt->PopAxisAlignedClip();
                }
            }
            for (size_t r = ya.i0; r < ya.i1; r++)
                for (size_t col = xa.i0; col < xa.i1; col++)
                    if (UINT32 f = fillOf(g.styles[styleAt(r, col)]); f && !mergeAt(r, col))
                        fill->SetColor(Rgb(f)), rt->FillRectangle({X(col), Y(r), X(col + 1) + 1, Y(r + 1) + 1}, fill.Get());
            fill->SetColor(c.codeBg);
            // Text. Unwrapped text runs over empty neighbours (left-aligned to the right, right-aligned to the left,
            // centred both ways); numbers stay in their cell.
            for (auto* m : ms) {
                std::wstring_view v = cellText((*m)[0], (*m)[1]);
                size_t i = (size_t)(*m)[0] * g.cols + (*m)[1];
                if (!v.empty()) drawText(v, styleAt((*m)[0], (*m)[1]), i < g.num.size() && g.num[i], mergeRc(*m), mergeRc(*m));
            }
            size_t minCol = xa.d == x0 ? 0 : fc;  // the scrolled pane doesn't show frozen columns' text
            for (size_t r = ya.i0; r < ya.i1 && r < rows; r++) {
                size_t from = xa.i0;
                while (from > minCol && from + 64 > xa.i0 && empty(r, from - 1)) from--;
                from = from > minCol ? from - 1 : from;
                for (size_t col = from; col < xa.i1 && col < g.cols; col++) {
                    std::wstring_view v = Cell(g, r, col);
                    if (v.empty() || mergeAt(r, col)) continue;
                    size_t si = styleAt(r, col);
                    const SheetStyle& s = g.styles[si];
                    bool num = g.num[r * g.cols + col];
                    int h = s.h ? s.h : num ? 3 : 1;
                    D2D1_RECT_F cell{X(col), Y(r), X(col + 1), Y(r + 1)}, box = cell;
                    if (!s.wrap && !num) {
                        if (h != 3)
                            for (size_t k = col + 1; k < col + 64 && X(k) < clip.right && empty(r, k); k++) box.right = X(k + 1);
                        if (h != 1)
                            for (size_t k = col; k > minCol && k + 64 > col && X(k) > clip.left && empty(r, k - 1); k--) box.left = X(k - 1);
                        if (h == 2) {
                            float mid = (cell.left + cell.right) / 2, half = std::min(mid - box.left, box.right - mid);
                            box.left = mid - half, box.right = mid + half;
                        }
                    }
                    drawText(v, si, num, box, {box.left, cell.top, box.right, cell.bottom});
                }
            }
            // Borders: each cell's own sides; a merge's from its edge cells.
            for (auto* m : ms) {
                auto [m0, m1, m2, m3] = *m;
                for (UINT32 col = m1; col <= m3 && col <= xa.i1; col++) {
                    const SheetStyle &t = g.styles[styleAt(m0, col)], &b = g.styles[styleAt(m2, col)];
                    edge(t.line[1], t.border[1], true, Y(m0), X(col), X(col + 1));
                    edge(b.line[3], b.border[3], true, Y(m2 + 1), X(col), X(col + 1));
                }
                for (UINT32 r = m0; r <= m2 && r <= ya.i1; r++) {
                    const SheetStyle &l = g.styles[styleAt(r, m1)], &rr = g.styles[styleAt(r, m3)];
                    edge(l.line[0], l.border[0], false, X(m1), Y(r), Y(r + 1));
                    edge(rr.line[2], rr.border[2], false, X(m3 + 1), Y(r), Y(r + 1));
                }
            }
            for (size_t r = ya.i0; r < ya.i1 && r < rows; r++)
                for (size_t col = xa.i0; col < xa.i1 && col < g.cols; col++) {
                    const SheetStyle& s = g.styles[styleAt(r, col)];
                    if (!(s.line[0] | s.line[1] | s.line[2] | s.line[3]) || mergeAt(r, col)) continue;
                    edge(s.line[0], s.border[0], false, X(col), Y(r), Y(r + 1));
                    edge(s.line[2], s.border[2], false, X(col + 1), Y(r), Y(r + 1));
                    edge(s.line[1], s.border[1], true, Y(r), X(col), X(col + 1));
                    edge(s.line[3], s.border[3], true, Y(r + 1), X(col), X(col + 1));
                }
            // Pictures over the cells; charts (not drawn) as a quiet box.
            for (size_t i = 0; i < g.pics.size(); i++) {
                D2D1_RECT_F p = g.pics[i].rc;
                p = {xa.d + p.left, ya.d + p.top, xa.d + p.right, ya.d + p.bottom};
                if (p.right < clip.left || p.left > clip.right || p.bottom < clip.top || p.top > clip.bottom) continue;
                if (!g_picBmp[i] && g.pics[i].wic) g_picBmp[i] = SharedBitmap(g.pics[i].wic.Get());
                if (g_picBmp[i]) rt->DrawBitmap(g_picBmp[i].Get(), p, 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                else {
                    rt->PushAxisAlignedClip(p, D2D1_ANTIALIAS_MODE_ALIASED);
                    rt->Clear({0, 0, 0, 0});  // no gridlines through it
                    rt->PopAxisAlignedClip();
                    rt->FillRectangle(p, fill.Get());
                    line->SetColor(c.rule);
                    rt->DrawRectangle({p.left + .5f, p.top + .5f, p.right - .5f, p.bottom - .5f}, line.Get());
                    text->SetColor(c.dim);
                    rt->DrawText(L"Chart", 5, g_fmtCenter.Get(), p, text.Get());
                }
            }
            rt->PopAxisAlignedClip();
        }
    // Frozen-pane dividers.
    line->SetColor(c.dim);
    if (fc) rt->FillRectangle({x0 + fx, y0, x0 + fx + 1, bottom}, line.Get());
    if (fr) rt->FillRectangle({x0, y0 + fy, area.right, y0 + fy + 1}, line.Get());

    // Headers: letters across, numbers down, a corner box.
    auto letters = [](size_t i) {
        std::wstring s;
        for (i++; i; i = (i - 1) / 26) s.insert(s.begin(), wchar_t(L'A' + (i - 1) % 26));
        return s;
    };
    text->SetColor(c.dim);
    line->SetColor(c.rule);
    rt->FillRectangle({area.left, area.top, area.right, y0}, fill.Get());
    rt->FillRectangle({area.left, y0, x0, bottom}, fill.Get());
    for (const Span& xa : xs) {
        if (!xa.on) continue;
        rt->PushAxisAlignedClip({xa.lo, area.top, xa.hi, y0}, D2D1_ANTIALIAS_MODE_ALIASED);
        for (size_t col = xa.i0; col < xa.i1; col++) {
            float x = xa.d + ColX(g, col), e = xa.d + ColX(g, col + 1);
            if (e - x < 1) continue;
            std::wstring l = letters(col);
            rt->DrawText(l.data(), (UINT32)l.size(), g_fmtCenter.Get(), {x, area.top, e, y0}, text.Get());
            rt->FillRectangle({x, area.top, x + 1, y0}, line.Get());
        }
        rt->PopAxisAlignedClip();
    }
    for (const Span& ya : ys) {
        if (!ya.on) continue;
        rt->PushAxisAlignedClip({area.left, ya.lo, x0, ya.hi}, D2D1_ANTIALIAS_MODE_ALIASED);
        for (size_t r = ya.i0; r < ya.i1; r++) {
            float y = ya.d + RowY(g, r), e = ya.d + RowY(g, r + 1);
            if (e - y < 1) continue;
            std::wstring n = std::to_wstring(r + 1);
            rt->DrawText(n.data(), (UINT32)n.size(), g_fmtCenter.Get(), {area.left, y, x0, e}, text.Get());
            rt->FillRectangle({area.left, y, x0, y + 1}, line.Get());
        }
        rt->PopAxisAlignedClip();
    }
    rt->FillRectangle({area.left, y0 - 1, area.right, y0}, line.Get());
    rt->FillRectangle({x0 - 1, area.top, x0, bottom}, line.Get());

    // Sheet tabs; when they don't fit, arrows and the wheel scroll them, and the active one is scrolled into view.
    rt->FillRectangle({area.left, bottom, area.right, area.bottom}, fill.Get());
    rt->FillRectangle({area.left, bottom, area.right, bottom + 1}, line.Get());
    std::vector<float> tx{0};
    for (auto& name : g.tabs) tx.push_back(tx.back() + std::clamp(name.size() * g_charW + 28, 64.f, 240.f));
    bool arrows = tx.back() > area.right - area.left - 16;
    g_tabStrip = {area.left + 8 + (arrows ? 2 * kArrowW : 0), bottom + 1, area.right - 8, area.bottom};
    float vis = g_tabStrip.right - g_tabStrip.left;
    g_tabMax = std::max(0.f, tx.back() - vis);
    if (g_tabReveal && g.tab >= 0 && g.tab + 1 < (int)tx.size()) {
        if (tx[g.tab] < g_tabScroll) g_tabScroll = tx[g.tab];
        if (tx[g.tab + 1] > g_tabScroll + vis) g_tabScroll = tx[g.tab + 1] - vis;
        g_tabReveal = false;
    }
    g_tabScroll = std::clamp(g_tabScroll, 0.f, g_tabMax);
    g_tabPrev = g_tabNext = {};
    if (arrows) {
        g_tabPrev = {area.left + 8, bottom + 1, area.left + 8 + kArrowW, area.bottom};
        g_tabNext = {g_tabPrev.right, bottom + 1, g_tabPrev.right + kArrowW, area.bottom};
        const wchar_t prev = 0xE76B, next = 0xE76C;  // ChevronLeft, ChevronRight
        text->SetColor(g_tabScroll > 0 ? c.text : c.dim);
        rt->DrawText(&prev, 1, g_fmtChevron.Get(), g_tabPrev, text.Get());
        text->SetColor(g_tabScroll < g_tabMax ? c.text : c.dim);
        rt->DrawText(&next, 1, g_fmtChevron.Get(), g_tabNext, text.Get());
    }
    g_tabRc.clear();
    rt->PushAxisAlignedClip(g_tabStrip, D2D1_ANTIALIAS_MODE_ALIASED);
    for (size_t i = 0; i < g.tabs.size(); i++) {
        D2D1_RECT_F r{g_tabStrip.left + tx[i] - g_tabScroll, bottom + 1, g_tabStrip.left + tx[i + 1] - g_tabScroll, area.bottom};
        g_tabRc.push_back({std::max(r.left, g_tabStrip.left), r.top, std::min(r.right, g_tabStrip.right), r.bottom});
        if (r.right < g_tabStrip.left || r.left > g_tabStrip.right) continue;
        bool on = (int)i == g.tab;
        text->SetColor(on ? c.text : c.dim);
        rt->DrawText(g.tabs[i].data(), (UINT32)g.tabs[i].size(), (on ? g_fmtHead : g_fmtCell).Get(), {r.left + 14, r.top, r.right - 8, r.bottom},
                     text.Get());
        if (i) rt->FillRectangle({r.left, r.top + 8, r.left + 1, r.bottom - 8}, line.Get());
        if (on) {
            line->SetColor(c.link);
            rt->FillRectangle({r.left + 10, r.bottom - 5, r.right - 10, r.bottom - 3}, line.Get());
            line->SetColor(c.rule);
        }
    }
    rt->PopAxisAlignedClip();

    float w = ColX(g, g.cols), h = RowY(g, rows);
    for (auto& p : g.pics) w = std::max(w, p.rc.right), h = std::max(h, p.rc.bottom);
    return {headW + w, kHead + h + kTabH + g.defRowH};
}

int GridTabAt(D2D1_POINT_2F pt) {
    if (!g_grid || !g_grid->sheet || pt.y < g_tabStrip.top || pt.y >= g_tabStrip.bottom) return -1;
    if (pt.x >= g_tabPrev.left && pt.x < g_tabPrev.right) return g_tabScroll -= 200, -2;
    if (pt.x >= g_tabNext.left && pt.x < g_tabNext.right) return g_tabScroll += 200, -2;
    for (size_t i = 0; i < g_tabRc.size(); i++)
        if (pt.x >= g_tabRc[i].left && pt.x < g_tabRc[i].right) return (int)i;
    return -1;
}

UINT32 DarkInk(UINT32 c) {
    int r = c >> 16 & 255, g = c >> 8 & 255, b = c & 255, hi = std::max({r, g, b}), lo = std::min({r, g, b});
    if (hi < 60) return 0;  // black-ish: by luminance, 0000FF or C00000 would count as black too
    if (0.299f * r + 0.587f * g + 0.114f * b >= 0.35f * 255) return c;
    int k = 255 - hi - lo;  // HSL lightness l -> 1 - l
    r += k, g += k, b += k;
    // Saturated blues stay dark at any lightness that keeps them blue (0000FF is l = 0.5): toward white until readable.
    float y = 0.299f * r + 0.587f * g + 0.114f * b, t = std::max(0.f, (0.45f * 255 - y) / (255 - y));
    auto up = [&](int v) { return (UINT32)std::lround(v + t * (255 - v)); };
    return 0xFF000000 | up(r) << 16 | up(g) << 8 | up(b);
}

float GridDigitWidth(const std::wstring& font, float size) {
    ComPtr<IDWriteTextFormat> f;
    ComPtr<IDWriteTextLayout> l;
    DWRITE_TEXT_METRICS m{};
    float w = 0;
    if (SUCCEEDED(g_dw->CreateTextFormat(font.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                         DWRITE_FONT_STRETCH_NORMAL, size, L"", &f)))
        for (wchar_t d = L'0'; d <= L'9'; d++)
            if (SUCCEEDED(g_dw->CreateTextLayout(&d, 1, f.Get(), 1000, 100, &l)) && SUCCEEDED(l->GetMetrics(&m)))
                w = std::max(w, m.widthIncludingTrailingWhitespace);
    return w > 0 ? std::round(w) : 7;
}

bool GridTabWheel(D2D1_POINT_2F pt, float delta) {
    if (!g_grid || !g_grid->sheet || pt.y < g_tabStrip.top || pt.y >= g_tabStrip.bottom || g_tabMax <= 0) return false;
    g_tabScroll = std::clamp(g_tabScroll - delta, 0.f, g_tabMax);
    return true;
}

D2D1_SIZE_F GridPaint(ID2D1RenderTarget* rt, const D2D1_RECT_F& area, float scrollX, float scrollY, const MdColors& c) {
    if (g_grid->sheet) return SheetPaint(rt, area, scrollX, scrollY, c);
    const GridData& g = *g_grid;
    ComPtr<ID2D1SolidColorBrush> text, fill;
    rt->CreateSolidColorBrush(c.text, &text);
    rt->CreateSolidColorBrush(c.codeBg, &fill);
    size_t body = g_shown.size();
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
            if (col == 0 && !g.folder.empty() && !head) {  // tree: indent + chevron + folder/file icon
                size_t i = r - 1;
                left += g.indent[i] * kIndent;
                if (g.folder[i] && HasChildren(g, i)) {
                    wchar_t chevron = g_open[i] ? 0xE70D : 0xE76C;  // ChevronDown, ChevronRight
                    text->SetColor(c.dim);
                    rt->DrawText(&chevron, 1, g_fmtChevron.Get(), {left, y, left + kChevronW, y + kRowH}, text.Get());
                }
                left += kChevronW;
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
        drawRow(g_shown[i] + (g.header ? 1 : 0), y, false);
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

// Decompresses a single compressed file (gzip, xz, bzip2, ...), e.g. .svgz.
bool ArchiveReadRaw(const std::wstring& path, std::string& out, size_t cap) {
    Archive* api = Api();
    if (!api) return false;
    void* a = api->read_new();
    api->support_filter_all(a);
    api->support_format_raw(a);
    void* e;
    bool ok = api->open_filename_w(a, path.c_str(), 1 << 16) == 0 && api->next_header(a, &e) == 0;
    char buf[1 << 16];
    for (long long n; ok && out.size() < cap && (n = api->read_data(a, buf, sizeof buf)) > 0;) out.append(buf, (size_t)n);
    api->free(a);
    return ok && !out.empty();
}

// Reads the members `want` accepts (it returns a byte limit, 0 = skip) in one pass: Office files are zips.
bool ArchiveReadMembers(const std::wstring& path, const std::function<size_t(std::wstring_view)>& want,
                        std::vector<std::pair<std::wstring, std::string>>& out) {
    Archive* api = Api();
    if (!api) return false;
    void* a = api->read_new();
    api->support_format_all(a);
    void* e;
    bool ok = api->open_filename_w(a, path.c_str(), 1 << 16) == 0;
    while (ok && api->next_header(a, &e) == 0) {
        const wchar_t* name = api->pathname_w(e);
        size_t cap = name ? want(name) : 0;
        if (!cap) continue;  // skipped by seeking, not decompressed
        std::string data;
        if (api->size_is_set(e)) data.reserve((size_t)std::min<long long>(api->size(e), cap));
        char buf[1 << 16];
        for (long long n; data.size() < cap && (n = api->read_data(a, buf, sizeof buf)) > 0;) data.append(buf, (size_t)n);
        out.emplace_back(name, std::move(data));
    }
    api->free(a);
    return ok;
}
