// Office files without Office, as text: odt/odp/doc/ppt (and docx/pptx, which pages.cpp normally draws) become
// Markdown (headings, paragraphs, bullets, tables, bold/italic); xlsx/ods/xls become the sheet grid. Zips of XML
// go through xml.h; doc/ppt/xls are OLE compound files (IStorage, part of Windows).
#include "app.h"
#include "xml.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <tuple>
#include <unordered_map>

namespace {

constexpr size_t kMaxRows = 50000, kMaxCols = 512;

// ---- Markdown output shared by all document formats
struct Run {
    std::string text;
    bool b, i;
};
struct Para {
    std::vector<Run> runs;
    int heading = 0, list = -1;
    bool b = false, i = false;  // formatting of the next text
};

struct MdOut {
    std::string out;
    std::vector<Para> open;      // paragraphs being read (text boxes nest them)
    std::string* capture = nullptr;  // slide title: closed paragraphs go here instead
    int tbl = 0;
    std::vector<std::vector<std::string>> rows;
    std::string cell;
    bool inList = false;

    Para* Cur() { return open.empty() ? nullptr : &open.back(); }
    void Open(int heading = 0, int list = -1) { open.push_back({{}, heading, list}); }
    void Text(std::string_view s, bool xml = false) {
        Para* p = Cur();
        if (!p) return;
        if (p->runs.empty() || p->runs.back().b != p->b || p->runs.back().i != p->i) p->runs.push_back({{}, p->b, p->i});
        xml ? AppendXml(p->runs.back().text, s) : (void)p->runs.back().text.append(s);
    }
    // Runs as Markdown: punctuation escaped, bold/italic markers kept off the surrounding spaces.
    static std::string Inline(const std::vector<Run>& runs, bool styled, bool oneLine) {
        std::string out;
        for (size_t k = 0; k < runs.size();) {
            bool b = styled && runs[k].b, i = styled && runs[k].i;
            std::string e;
            for (; k < runs.size() && (styled && runs[k].b) == b && (styled && runs[k].i) == i; k++)
                for (char c : runs[k].text)
                    if (c == '\n') e += oneLine ? " " : "\\\n";
                    else if (c == '\t') e += ' ';
                    else if ((unsigned char)c < 0x80 && ispunct((unsigned char)c)) e += '\\', e += c;
                    else e += c;
            size_t s0 = e.find_first_not_of(' '), s1 = e.find_last_not_of(' ');
            if ((!b && !i) || s0 == e.npos || e.compare(s0, 2, "\\\n") == 0) {
                out += e;
                continue;
            }
            const char* m = b && i ? "***" : b ? "**" : "*";
            out.append(e, 0, s0).append(m).append(e, s0, s1 - s0 + 1).append(m).append(e, s1 + 1);
        }
        for (;;)  // no spaces or hard breaks at either end
            if (out.ends_with(' ')) out.pop_back();
            else if (out.ends_with("\\\n")) out.resize(out.size() - 2);
            else if (out.starts_with(' ')) out.erase(0, 1);
            else if (out.starts_with("\\\n")) out.erase(0, 2);
            else break;
        return out;
    }
    void Block(const std::string& line, bool item) {
        if (inList && !item) out += '\n';
        out += line;
        out += item ? "\n" : "\n\n";
        inList = item;
    }
    void Close() {
        if (open.empty()) return;
        Para p = std::move(open.back());
        open.pop_back();
        bool oneLine = tbl || capture || p.heading;
        std::string s = Inline(p.runs, !p.heading && !capture, oneLine);
        if (s.empty()) return;
        if (tbl) cell += (cell.empty() ? "" : " ") + s;
        else if (capture) *capture += (capture->empty() ? "" : " ") + s;
        else if (p.heading) Block(std::string(std::min(p.heading, 6), '#') + ' ' + s, false);
        else if (p.list >= 0) Block(std::string(2 * std::min(p.list, 8), ' ') + "- " + s, true);
        else Block(s, false);
    }
    void TableStart() {
        if (tbl++ == 0) rows.clear();
    }
    void RowStart() {
        if (tbl == 1) rows.emplace_back();
    }
    void CellStart() {
        if (tbl == 1) cell.clear();
    }
    void CellEnd(int span = 1) {
        if (tbl != 1 || rows.empty()) return;
        rows.back().push_back(std::move(cell));
        for (int k = 1; k < span && k < 64; k++) rows.back().emplace_back();
        cell.clear();
    }
    void TableEnd() {
        if (!tbl || --tbl) return;
        size_t cols = 0;
        for (auto& r : rows) cols = std::max(cols, r.size());
        if (!cols) return;
        if (inList) out += '\n', inList = false;
        for (size_t r = 0; r < rows.size(); r++) {
            out += '|';
            for (size_t c = 0; c < cols; c++) out += ' ', out += c < rows[r].size() ? rows[r][c] : "", out += " |";
            out += '\n';
            if (r == 0)
                for (size_t c = 0; c <= cols; c++) out += c < cols ? "| --- " : "|\n";
        }
        out += '\n';
    }
};

// ---- sheets: cells at their Excel positions (A1 = row 0, column 0) -> GridData
struct Cell {  // ods: a row being read
    std::string v;
    bool num = false;
};
struct Sheet {
    struct Val { UINT32 r, c, off, len; UINT16 xf; bool num; double x; };  // text at off/len in `text`; x: the number
    std::wstring text;
    std::vector<Val> vals;
};
// num: numbers and dates (right-aligned unless the format says otherwise). Formatted empty cells are kept.
void Put(Sheet& s, size_t r, size_t c, std::string_view v, bool num = false, unsigned xf = 0, double x = 0) {
    if (r >= kMaxRows || c >= kMaxCols || (v.empty() && !xf)) return;
    size_t at = s.text.size();
    s.text.resize(at + v.size());
    s.text.resize(at + MultiByteToWideChar(CP_UTF8, 0, v.data(), (int)v.size(), s.text.data() + at, (int)v.size()));
    s.vals.push_back({(UINT32)r, (UINT32)c, (UINT32)at, (UINT32)(s.text.size() - at), (UINT16)std::min(xf, 65535u), num, x});
}
// The values as the dense grid; cells without a value take their row's or column's format.
void Fill(GridData& g, Sheet& s, const std::vector<UINT16>& rowXf = {}, const std::vector<UINT16>& colXf = {}) {
    size_t rows = 0, cols = 1;
    for (auto& v : s.vals) rows = std::max<size_t>(rows, v.r + 1), cols = std::max<size_t>(cols, v.c + 1);
    rows = std::max<size_t>(rows, 1);
    cols = std::min(cols, std::max<size_t>(1, (4 << 20) / rows));  // a stray formatted far cell: not 25M cells
    g.cols = (UINT32)cols;
    g.cells.assign(rows * cols, {0, 0}), g.num.assign(rows * cols, false), g.xf.assign(rows * cols, 0);
    if (!rowXf.empty() || !colXf.empty())
        for (size_t r = 0; r < rows; r++)
            for (size_t c = 0; c < cols; c++)
                g.xf[r * cols + c] = r < rowXf.size() && rowXf[r] ? rowXf[r] : c < colXf.size() ? colXf[c] : 0;
    g.text = std::move(s.text);
    for (auto& v : s.vals) {
        if (v.c >= cols) continue;
        size_t i = (size_t)v.r * cols + v.c;
        UINT16 xf = v.xf < g.styles.size() ? v.xf : 0;
        g.cells[i] = {v.off, v.len}, g.num[i] = v.num;
        if (xf || !v.len) g.xf[i] = xf;
        if (!g.styles[xf].wrap)  // line breaks only show in wrapped cells
            for (UINT32 k = v.off; k < v.off + v.len; k++)
                if (g.text[k] == L'\n' || g.text[k] == L'\r' || g.text[k] == L'\t') g.text[k] = L' ';
    }
    g.sheet = true;
    g.rightAlign.assign(cols, false);
    g.sub = s.vals.empty() ? L"empty" : std::to_wstring(rows) + (rows == 1 ? L" row × " : L" rows × ") +
                                            std::to_wstring(cols) + (cols == 1 ? L" column" : L" columns");
}

// ---- number formats (Excel's codes)
const char* BuiltinFormat(unsigned id) {
    static std::string shortDate = [] {  // 14 and 22 follow the system's short date, like Excel
        wchar_t w[80] = L"";
        GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SSHORTDATE, w, 80);
        std::string s = Utf8(w);
        for (char& c : s) c = c == 'M' ? 'm' : c;  // Windows: M month, m minute
        return s.empty() ? std::string("m/d/yyyy") : s;
    }();
    static std::string dateTime = shortDate + " h:mm";
    switch (id) {
        case 1: return "0";
        case 2: return "0.00";
        case 3: return "#,##0";
        case 4: return "#,##0.00";
        case 5: case 37: return "#,##0_);(#,##0)";
        case 6: case 38: return "#,##0_);[Red](#,##0)";
        case 7: case 39: return "#,##0.00_);(#,##0.00)";
        case 8: case 40: return "#,##0.00_);[Red](#,##0.00)";
        case 9: return "0%";
        case 10: return "0.00%";
        case 11: return "0.00E+00";
        case 14: return shortDate.c_str();
        case 15: return "d-mmm-yy";
        case 16: return "d-mmm";
        case 17: return "mmm-yy";
        case 18: return "h:mm AM/PM";
        case 19: return "h:mm:ss AM/PM";
        case 20: return "h:mm";
        case 21: return "h:mm:ss";
        case 22: return dateTime.c_str();
        case 41: return "_(* #,##0_);_(* (#,##0);_(* \"-\"_);_(@_)";
        case 42: return "_(\"$\"* #,##0_);_(\"$\"* (#,##0);_(\"$\"* \"-\"_);_(@_)";
        case 43: return "_(* #,##0.00_);_(* (#,##0.00);_(* \"-\"??_);_(@_)";
        case 44: return "_(\"$\"* #,##0.00_);_(\"$\"* (#,##0.00);_(\"$\"* \"-\"??_);_(@_)";
        case 45: return "mm:ss";
        case 46: return "[h]:mm:ss";
        case 47: return "mm:ss.0";
        case 48: return "##0.0E+0";
        case 49: return "@";
        default: return "General";
    }
}

// Excel's General: at most 11 characters, scientific past that.
std::string General(double v) {
    if (v == 0) return "0";
    char b[40];
    for (int p = 11; p > 0; p--)
        if (snprintf(b, sizeof b, "%.*G", p, std::fabs(v)) <= 11) break;
    return (v < 0 ? "-" : "") + std::string(b);
}

// Days since 1899-12-30 -> y, m, d (Hinnant's civil_from_days) with Excel's 1900-02-29 (serial 60).
void Civil(long long days, int& y, int& m, int& d) {
    if (days == 60 || days == 0) {
        y = 1900, m = days ? 2 : 1, d = days ? 29 : 0;
        return;
    }
    if (days < 60) days++;  // before the phantom leap day Excel's serials are one day ahead
    long long z = days - 25569 + 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097, doe = z - era * 146097;
    long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long long mp = (5 * doy + 2) / 153;
    d = (int)(doy - (153 * mp + 2) / 5 + 1), m = (int)(mp < 10 ? mp + 3 : mp - 9), y = (int)(yoe + era * 400 + (m <= 2));
}

// One number through a format code: sections pos;neg;zero, digits 0 # ?, grouping and scaling commas,
// %, E+, literals ("..", \x, _x, [$€-407]), General, and dates/times (y m d h s AM/PM [h], serial days).
// shortcut: literals between digit placeholders ("000-0000"), fractions (# ?/?), [Red] and conditions are
// dropped or put after the number; add when such files show up.
std::string FormatNumber(double v, std::string_view code, bool d1904 = false) {
    std::vector<std::string_view> sec;
    for (size_t k = 0, start = 0; k <= code.size(); k++) {
        if (k == code.size() || code[k] == ';') sec.push_back(code.substr(start, k - start)), start = k + 1;
        else if (code[k] == '"') k = std::min(code.find('"', k + 1), code.size() - 1);
        else if (code[k] == '\\' || code[k] == '_' || code[k] == '*') k++;
    }
    bool minus = v < 0;
    std::string_view f = sec[0];
    if (sec.size() >= 2 && v < 0) f = sec[1], minus = false, v = -v;
    else if (sec.size() >= 3 && v == 0) f = sec[2];
    // Tokens: literals as text, codes as their letters (lower case) or digit placeholders.
    struct Tok { char kind; std::string s; };  // 'l' literal, 'n' number part, 'd' date part, 'g' General
    std::vector<Tok> toks;
    bool date = false, ampm = false, text = false;
    int pct = 0;
    auto lit = [&](std::string_view s) {
        if (!toks.empty() && toks.back().kind == 'l') toks.back().s += s;
        else toks.push_back({'l', std::string(s)});
    };
    for (size_t k = 0; k < f.size(); k++) {
        char c = f[k], lc = (char)tolower((unsigned char)c);
        if (!c) break;
        if (c == '"') {
            size_t e = std::min(f.find('"', k + 1), f.size());
            lit(f.substr(k + 1, e - k - 1)), k = e;
        } else if (c == '\\' && k + 1 < f.size()) lit(f.substr(++k, 1));
        else if (c == '_' && k + 1 < f.size()) lit(" "), k++;
        else if (c == '*') k++;
        else if (c == '[') {
            size_t e = std::min(f.find(']', k), f.size());
            std::string_view in = f.substr(k + 1, e - k - 1);
            if (in.starts_with('$')) lit(in.substr(1, in.find('-') - 1));  // [$€-407]: the symbol
            else if (!in.empty() && strchr("hHmMsS", in[0]) && in.find_first_not_of(in[0]) == in.npos) toks.push_back({'d', "[" + std::string(1, (char)tolower(in[0])) + "]"}), date = true;
            k = e;
        } else if (f.compare(k, 7, "General") == 0 || f.compare(k, 7, "general") == 0) toks.push_back({'g', ""}), k += 6;
        else if (_strnicmp(f.data() + k, "AM/PM", 5) == 0) toks.push_back({'d', c == 'a' ? "am" : "AM"}), k += 4, ampm = true;
        else if (_strnicmp(f.data() + k, "A/P", 3) == 0) toks.push_back({'d', c == 'a' ? "ap" : "AP"}), k += 2, ampm = true;
        else if (strchr("ymdhs", lc)) {
            size_t e = k;
            while (e < f.size() && tolower((unsigned char)f[e]) == lc) e++;
            toks.push_back({'d', std::string(e - k, lc)}), k = e - 1, date = true;
        } else if (c == '%') lit("%"), pct++;
        else if (strchr("0#?.,", c) || ((c == 'E' || c == 'e') && k + 1 < f.size() && (f[k + 1] == '+' || f[k + 1] == '-'))) {
            if (c == 'E' || c == 'e') toks.push_back({'n', std::string(f.substr(k, 2))}), k++;
            else if (c == '.' && date && !toks.empty() && toks.back().kind == 'd') toks.push_back({'d', "."});
            else if (date && c == '0' && !toks.empty() && toks.back().kind == 'd' && toks.back().s[0] == '.') toks.back().s += '0';
            else if (!toks.empty() && toks.back().kind == 'n') toks.back().s += c;
            else toks.push_back({'n', std::string(1, c)});
        } else if (c == '@') text = true;
        else lit(f.substr(k, 1));
    }
    std::string out;
    if (date) {
        v += d1904 ? 1462 : 0;  // the 1904 date system counts from 1904-01-01
        if (!(v >= 0 && v <= 2958465)) return "########";  // NaN too
        bool frac = false;
        for (auto& t : toks) frac |= t.kind == 'd' && t.s[0] == '.';
        long long ms = std::llround(v * 86400000.0);
        if (!frac) ms = (ms + 500) / 1000 * 1000;
        long long days = ms / 86400000, secs = ms / 1000 % 86400;
        int y, mo, d;
        Civil(days, y, mo, d);
        static const char* kMonth[] = {"January", "February", "March", "April", "May", "June", "July",
                                       "August", "September", "October", "November", "December"};
        static const char* kDay[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
        char b[32];
        for (size_t i = 0; i < toks.size(); i++) {
            const std::string& s = toks[i].s;
            if (toks[i].kind != 'd') {  // literals; number characters (an unquoted comma) as they are
                out += s;
                continue;
            }
            size_t n = s.size();
            int h = (int)(secs / 3600);
            switch (s[0]) {
                case 'y': snprintf(b, sizeof b, n <= 2 ? "%02d" : "%d", n <= 2 ? y % 100 : y); break;
                case 'm': {  // minutes after an hour or before seconds, else month
                    bool minute = false;
                    for (size_t j = i; j-- > 0;)
                        if (toks[j].kind == 'd') { minute = toks[j].s[0] == 'h' || toks[j].s == "[h]"; break; }
                    for (size_t j = i + 1; !minute && j < toks.size(); j++)
                        if (toks[j].kind == 'd') { minute = toks[j].s[0] == 's' || toks[j].s == "[s]"; break; }
                    if (minute && n <= 2) snprintf(b, sizeof b, n == 2 ? "%02lld" : "%lld", secs / 60 % 60);
                    else if (n <= 2) snprintf(b, sizeof b, n == 2 ? "%02d" : "%d", mo);
                    else snprintf(b, sizeof b, "%.*s", n == 3 ? 3 : n == 4 ? 20 : 1, kMonth[(mo + 11) % 12]);
                    break;
                }
                case 'd':
                    if (n <= 2) snprintf(b, sizeof b, n == 2 ? "%02d" : "%d", d);
                    else snprintf(b, sizeof b, "%.*s", n == 3 ? 3 : 20, kDay[(days + 6) % 7]);
                    break;
                case 'h': snprintf(b, sizeof b, n >= 2 ? "%02d" : "%d", ampm ? (h + 11) % 12 + 1 : h); break;
                case 's': snprintf(b, sizeof b, n >= 2 ? "%02lld" : "%lld", secs % 60); break;
                case '[': snprintf(b, sizeof b, "%lld", s[1] == 'h' ? ms / 3600000 : s[1] == 'm' ? ms / 60000 : ms / 1000); break;
                case '.': n = std::min<size_t>(n, 4), snprintf(b, sizeof b, ".%0*lld", (int)n - 1, ms % 1000 / (long long)std::pow(10, 4 - (int)n)); break;
                default: snprintf(b, sizeof b, "%s", s == "AM" ? (h < 12 ? "AM" : "PM") : s == "am" ? (h < 12 ? "am" : "pm")
                                                    : s == "AP" ? (h < 12 ? "A" : "P") : (h < 12 ? "a" : "p"));
            }
            out += b;
        }
        return out;
    }
    // Numbers: the placeholders as one pattern, the number written where it starts.
    std::string pat;
    bool gen = false;
    for (auto& t : toks) pat += t.kind == 'n' ? t.s : "", gen |= t.kind == 'g';
    if (pat.empty() && !gen) {  // only literals (e.g. "-" for zero in accounting formats); "@" alone: General
        for (auto& t : toks) out += t.s;
        return out.empty() && text ? General(v) : out;
    }
    double x = std::fabs(v) * std::pow(100, pct);
    size_t e = pat.find_first_of("Ee"), dot = std::min(pat.find('.'), e);
    std::string ip = pat.substr(0, std::min(dot, pat.size())), fp = dot < pat.size() && dot < e ? pat.substr(dot + 1, e - dot - 1) : "";
    std::string& last = dot < pat.size() && dot < e ? fp : ip;  // commas after the last digit: thousands
    while (!last.empty() && last.back() == ',') x /= 1000, last.pop_back();
    bool group = ip.find(',') != ip.npos;
    std::erase(ip, ','), std::erase(fp, ',');
    std::string num;
    char b[400];
    if (gen) num = General(x);
    else {
        int exp = 0;
        if (e < pat.size() && x != 0) {
            exp = (int)std::floor(std::log10(x));
            if (ip.size() > 1 && ip.find('#') != ip.npos) exp = (int)std::floor(exp / (double)ip.size()) * (int)ip.size();
            x /= std::pow(10, exp);
        }
        x *= 1 + 0x1p-50;  // decimal ties (0.125, 2.675) round up as Excel does, not to the binary value below
        snprintf(b, sizeof b, "%.*f", (int)std::min<size_t>(fp.size(), 30), x);
        num = b;
        size_t p = num.find('.');
        std::string in = num.substr(0, p), fr = p == num.npos ? "" : num.substr(p + 1);
        size_t zeros = std::count(ip.begin(), ip.end(), '0');
        if (in == "0" && zeros == 0) in.clear();
        if (in.size() < zeros) in.insert(0, zeros - in.size(), '0');
        if (group)
            for (int k = (int)in.size() - 3; k > 0; k -= 3) in.insert(k, ",");
        for (size_t k = fr.size(); k-- > 0 && fp[k] != '0' && (fr[k] == '0' || fr[k] == ' ');)  // # drops trailing zeros, ? pads
            fp[k] == '?' ? (void)(fr[k] = ' ') : (void)fr.erase(k, 1);
        num = in + (fp.empty() ? "" : "." + fr);
        if (e < pat.size()) {
            std::string ep = pat.substr(e + 2);
            snprintf(b, sizeof b, "%0*d", (int)std::max<size_t>(1, std::count(ep.begin(), ep.end(), '0')), std::abs(exp));
            num += std::string(1, pat[e]) + (exp < 0 ? "-" : pat[e + 1] == '+' ? "+" : "") + b;
        }
    }
    bool placed = false;
    for (auto& t : toks)
        if (t.kind == 'l') out += t.s;
        else if (!placed) out += num, placed = true;
    if (minus && (gen || std::strtod(num.c_str(), nullptr) != 0)) out.insert(0, "-");
    return out;
}

// ---- docx
std::string DocxToMd(std::string_view doc, std::string_view styles) {
    std::unordered_map<std::string, int> level;  // paragraph style id -> heading level
    std::unordered_map<std::string, std::string> basedOn;
    std::unordered_map<std::string, bool> listStyle;  // style with numbering (List Bullet & co.)
    {
        std::string id;
        for (Xml x{styles}; int t = x.Next();) {
            if (t != Xml::Tag || x.close) continue;
            std::string_view v = Attr(x.attrs, "val");
            if (x.name == "style") id = Attr(x.attrs, "styleId");
            else if (id.empty()) continue;
            else if (x.name == "name") {  // built-in names are English whatever the UI language
                if (v.size() > 8 && _strnicmp(v.data(), "heading ", 8) == 0) level[id] = atoi(v.data() + 8);
                else if (v == "Title") level[id] = 1;
            } else if (x.name == "outlineLvl" && atoi(std::string(v).c_str()) < 9 && !level.count(id))
                level[id] = atoi(std::string(v).c_str()) + 1;
            else if (x.name == "basedOn") basedOn[id] = v;
            else if (x.name == "numId") listStyle[id] = v != "0";
        }
        for (auto& [id, base] : basedOn) {
            std::string b = base;
            for (int k = 0; k < 8 && !level.count(id); k++) {
                if (auto it = level.find(b); it != level.end()) level[id] = it->second;
                else if (auto up = basedOn.find(b); up != basedOn.end()) b = up->second;
                else break;
            }
        }
    }
    MdOut md;
    bool inRun = false, inText = false;
    int span = 1;
    for (Xml x{doc}; int t = x.Next();) {
        if (t == Xml::Text) {
            if (inText) md.Text(x.text, !x.cdata);
            continue;
        }
        std::string_view n = x.name;
        Para* p = md.Cur();
        if (x.close) {
            if (n == "p") md.Close();
            else if (n == "r") inRun = false;
            else if (n == "t") inText = false;
            else if (n == "tc") md.CellEnd(span);
            else if (n == "tbl") md.TableEnd();
            continue;
        }
        if (n == "Fallback") x.Skip();  // same content as the mc:Choice before it
        else if (n == "p") md.Open();
        else if (n == "tbl") md.TableStart();
        else if (n == "tr") md.RowStart();
        else if (n == "tc") md.CellStart(), span = 1;
        else if (n == "gridSpan") span = AttrInt(x.attrs, "val", 1);
        else if (!p) continue;
        else if (n == "pStyle") {
            std::string id(Attr(x.attrs, "val"));
            if (auto it = level.find(id); it != level.end()) p->heading = it->second;
            if (listStyle[id]) p->list = std::max(p->list, 0);
        } else if (n == "outlineLvl" && AttrInt(x.attrs, "val", 9) < 9) p->heading = AttrInt(x.attrs, "val", 0) + 1;
        else if (n == "numId") p->list = Attr(x.attrs, "val") == "0" ? -1 : std::max(p->list, 0);
        else if (n == "ilvl") p->list = AttrInt(x.attrs, "val", 0);
        else if (n == "r") inRun = true, p->b = p->i = false;
        else if (inRun && (n == "b" || n == "i")) {
            std::string_view v = Attr(x.attrs, "val");
            (n == "b" ? p->b : p->i) = v != "0" && v != "false";
        } else if (n == "t") inText = !x.empty;
        else if (n == "tab") md.Text(" ");
        else if (n == "br" || n == "cr") md.Text("\n");
        else if (n == "noBreakHyphen") md.Text("-");
    }
    return md.out;
}

// Slides as one document: "## title" (or "Slide n") per slide, a rule between them.
struct Deck {
    std::string out;
    int n = 0;
    void Add(const std::string& title, const std::string& body) {
        if (n++) out += "---\n\n";
        out += "## " + (title.empty() ? "Slide " + std::to_string(n) : title) + "\n\n" + body;
    }
};

// Documents shown as Markdown read at most this much (odt/odp XML, doc/ppt text) on the open path: ~5 ms.
constexpr size_t kQuickDoc = 512 << 10;

// ---- OpenDocument (odt/odp): content.xml. Slides mode: draw:page per slide, titles from presentation:class.
std::string OdfToMd(std::string_view xml, bool slides, int& count) {
    std::unordered_map<std::string, std::pair<bool, bool>> style;  // automatic style -> bold, italic
    std::string styleName;
    MdOut md;
    Deck deck;
    std::string title;
    std::vector<std::pair<bool, bool>> spans;
    int lists = 0, depth = 0, captureDepth = -1, pages = 0;
    auto flush = [&] {
        if (pages++) deck.Add(title, md.out);
        md.out.clear(), title.clear(), md.inList = false;
    };
    auto styleOf = [&](std::string_view a) {
        auto it = style.find(std::string(Attr(a, "style-name")));
        return it == style.end() ? std::pair{false, false} : it->second;
    };
    for (Xml x{xml}; int t = x.Next();) {
        if (t == Xml::Text) {
            if (!md.Cur()) continue;
            std::string s = x.cdata ? std::string(x.text) : XmlText(x.text);
            std::string c;  // ODF collapses white space; text:s carries real runs of spaces
            for (char ch : s)
                if (ch == '\n' || ch == '\r' || ch == '\t' || ch == ' ') {
                    if (c.empty() || c.back() != ' ') c += ' ';
                } else c += ch;
            md.Text(c);
            continue;
        }
        std::string_view n = x.name;
        if (x.close) {
            depth--;
            if (n == "p" || n == "h") md.Close();
            else if (n == "span" && !spans.empty()) {
                if (Para* p = md.Cur()) p->b = spans.back().first, p->i = spans.back().second;
                spans.pop_back();
            } else if (n == "list") lists--;
            else if (n == "table-cell" || n == "covered-table-cell") md.CellEnd();
            else if (n == "table") md.TableEnd();
            if (depth == captureDepth) md.capture = nullptr, captureDepth = -1;
            continue;
        }
        if (!x.empty) depth++;
        if (n == "note" || n == "annotation" || n == "tracked-changes" || n == "notes") x.Skip(), depth -= !x.empty;
        else if (n == "style") styleName = Attr(x.attrs, "name");
        else if (n == "text-properties" && !styleName.empty())
            style[styleName] = {Attr(x.attrs, "font-weight") == "bold", Attr(x.attrs, "font-style") == "italic"};
        else if (n == "page" && slides) flush();
        else if (n == "frame" && slides) {
            std::string_view cls = Attr(x.attrs, "class");
            if (cls == "title") md.capture = &title, captureDepth = depth - !x.empty;
            else if (cls == "page-number" || cls == "footer" || cls == "date-time" || cls == "header")
                x.Skip(), depth -= !x.empty;
        } else if (n == "p" || n == "h") {
            md.Open(n == "h" ? AttrInt(x.attrs, "outline-level", 1) : 0, lists ? lists - 1 : -1);
            std::tie(md.Cur()->b, md.Cur()->i) = styleOf(x.attrs);
            if (x.empty) md.Close();
        } else if (n == "span" && md.Cur()) {
            Para* p = md.Cur();
            spans.push_back({p->b, p->i});
            auto [b, i] = styleOf(x.attrs);
            p->b |= b, p->i |= i;
            if (x.empty) p->b = spans.back().first, p->i = spans.back().second, spans.pop_back();
        } else if (n == "s") md.Text(std::string(std::clamp(AttrInt(x.attrs, "c", 1), 1, 100), ' '));
        else if (n == "tab") md.Text(" ");
        else if (n == "line-break") md.Text("\n");
        else if (n == "list") lists += !x.empty;
        else if (n == "table") md.TableStart();
        else if (n == "table-row") md.RowStart();
        else if (n == "table-cell" || n == "covered-table-cell") {
            md.CellStart();
            if (x.empty) md.CellEnd();
        }
    }
    if (slides) {
        if (pages) flush();
        count = deck.n;
        return deck.out;
    }
    return md.out;
}

// ---- xlsx: the active sheet with its formats, column widths, row heights, merges and pictures
constexpr size_t kQuickRows = 100, kQuickXml = 256 << 10;  // open path: enough for the first screen
constexpr UINT32 kIndexed[64] = {
    0x000000, 0xFFFFFF, 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00, 0xFF00FF, 0x00FFFF, 0x000000, 0xFFFFFF, 0xFF0000,
    0x00FF00, 0x0000FF, 0xFFFF00, 0xFF00FF, 0x00FFFF, 0x800000, 0x008000, 0x000080, 0x808000, 0x800080, 0x008080,
    0xC0C0C0, 0x808080, 0x9999FF, 0x993366, 0xFFFFCC, 0xCCFFFF, 0x660066, 0xFF8080, 0x0066CC, 0xCCCCFF, 0x000080,
    0xFF00FF, 0xFFFF00, 0x00FFFF, 0x800080, 0x800000, 0x008080, 0x0000FF, 0x00CCFF, 0xCCFFFF, 0xCCFFCC, 0xFFFF99,
    0x99CCFF, 0xFF99CC, 0xCC99FF, 0xFFCC99, 0x3366FF, 0x33CCCC, 0x99CC00, 0xFFCC00, 0xFF9900, 0xFF6600, 0x666699,
    0x969696, 0x003366, 0x339966, 0x003300, 0x333300, 0x993300, 0x993366, 0x333399, 0x333333};

// A colour element (rgb, theme + tint, indexed) -> 0xFFRRGGBB; 0 = automatic.
// shortcut: tint moves RGB toward white/black; Excel moves HSL lightness (close enough for fills).
UINT32 XlColor(std::string_view a, const std::vector<UINT32>& theme) {
    UINT32 c = 0;
    std::string_view rgb = Attr(a, "rgb"), th = Attr(a, "theme"), ix = Attr(a, "indexed");
    if (Attr(a, "auto") == "1") return 0;
    if (rgb.size() >= 6) c = 0xFF000000 | (UINT32)strtoul(std::string(rgb.substr(rgb.size() - 6)).c_str(), nullptr, 16);
    else if (!th.empty()) {
        size_t i = (size_t)atoi(std::string(th).c_str());
        i = i < 4 ? i ^ 1 : i;  // 0 lt1, 1 dk1, 2 lt2, 3 dk2; the theme lists dk1 lt1 dk2 lt2
        if (i < theme.size()) c = theme[i];
    } else if (int k = atoi(std::string(ix).c_str()); !ix.empty() && (unsigned)k < 64) c = 0xFF000000 | kIndexed[k];
    double t = atof(std::string(Attr(a, "tint")).c_str());
    if (c && t) {
        UINT32 out = 0xFF000000;
        for (int s = 0; s < 24; s += 8) {
            double v = (c >> s & 255) / 255.0;
            v = t < 0 ? v * (1 + t) : v + (1 - v) * t;
            out |= (UINT32)std::lround(v * 255) << s;
        }
        c = out;
    }
    return c;
}

std::vector<UINT32> XlTheme(std::string_view xml) {  // dk1 lt1 dk2 lt2 accent1-6 hlink folHlink
    std::vector<UINT32> c;
    bool in = false;
    for (Xml x{xml}; int t = x.Next();)
        if (t != Xml::Tag) continue;
        else if (x.name == "clrScheme") {
            if (x.close) break;
            in = true;
        } else if (in && !x.close && (x.name == "srgbClr" || x.name == "sysClr"))
            c.push_back(0xFF000000 | (UINT32)strtoul(std::string(Attr(x.attrs, x.name == "srgbClr" ? "val" : "lastClr")).c_str(), nullptr, 16));
    return c;
}

UINT8 LineStyle(std::string_view s) {
    if (s.empty() || s == "none") return 0;
    if (s == "thin" || s == "hair") return 1;
    if (s == "medium") return 2;
    if (s == "thick") return 3;
    return s == "double" ? 4 : 5;  // the dashed and dotted kinds
}

// styles.xml: one SheetStyle and number format code per cellXfs entry.
// dxfs: conditional formats (text colour, bold, fill; 0 / false = unchanged).
void XlStyles(std::string_view xml, const std::vector<UINT32>& theme, std::vector<SheetStyle>& styles, std::vector<std::string>& fmts,
              std::vector<SheetStyle>& dxfs) {
    std::vector<SheetStyle> fonts, borders;  // only their font / border fields
    std::vector<UINT32> fills;
    std::unordered_map<unsigned, std::string> codes;
    enum { None, Fonts, Fills, Borders, Xfs, Dxfs } sec = None;
    int side = -1;
    bool pattern = false, inFill = false;
    styles.clear(), fmts.clear(), dxfs.clear();
    for (Xml x{xml}; int t = x.Next();) {
        if (t != Xml::Tag) continue;
        std::string_view n = x.name, a = x.attrs, val = Attr(a, "val");
        int k = n == "left" || n == "start" ? 0 : n == "top" ? 1 : n == "right" || n == "end" ? 2 : n == "bottom" ? 3 : -1;
        if (n == "fonts" || n == "fills" || n == "borders" || n == "cellXfs" || n == "dxfs") {
            sec = x.close || x.empty ? None : n == "fonts" ? Fonts : n == "fills" ? Fills : n == "borders" ? Borders : n == "dxfs" ? Dxfs : Xfs;
            continue;
        }
        if (x.close) {
            if (k >= 0) side = -1;
            if (n == "fill") inFill = false;
            continue;
        }
        if (n == "numFmt") codes[AttrInt(a, "numFmtId", 0)] = XmlText(Attr(a, "formatCode"));
        else if (sec == Fonts) {
            if (n == "font") fonts.emplace_back();
            if (fonts.empty()) continue;
            SheetStyle& f = fonts.back();
            bool on = val != "0" && val != "false";
            if (n == "b") f.bold = on;
            else if (n == "i") f.italic = on;
            else if (n == "strike") f.strike = on;
            else if (n == "u") f.underline = val != "none";
            else if (n == "sz") f.size = (float)atof(std::string(val).c_str()) * 96 / 72;
            else if (n == "color") f.color = XlColor(a, theme);
            else if (n == "name") f.font = Wide(XmlText(val));
        } else if (sec == Fills) {
            if (n == "fill") fills.push_back(0);
            else if (fills.empty()) continue;
            else if (n == "patternFill") pattern = !Attr(a, "patternType").empty() && Attr(a, "patternType") != "none";
            else if (n == "fgColor" && pattern) fills.back() = XlColor(a, theme);
            else if (n == "color" && !fills.back()) fills.back() = XlColor(a, theme);  // gradient: its first stop
        } else if (sec == Borders) {
            if (n == "border") borders.emplace_back();
            else if (borders.empty()) continue;
            else if (k >= 0) borders.back().line[k] = LineStyle(Attr(a, "style")), side = x.empty ? -1 : k;
            else if (n == "color" && side >= 0) borders.back().border[side] = XlColor(a, theme);
        } else if (sec == Xfs && n == "xf") {
            SheetStyle s;
            if (size_t i = AttrInt(a, "fontId", 0); i < fonts.size()) s = fonts[i];
            if (size_t i = AttrInt(a, "fillId", 0); i < fills.size()) s.fill = fills[i];
            if (size_t i = AttrInt(a, "borderId", 0); i < borders.size())
                std::copy_n(borders[i].border, 4, s.border), std::copy_n(borders[i].line, 4, s.line);
            styles.push_back(s);
            unsigned id = AttrInt(a, "numFmtId", 0);
            auto it = codes.find(id);
            fmts.push_back(it != codes.end() ? it->second : BuiltinFormat(id));
        } else if (sec == Dxfs) {
            if (n == "dxf") dxfs.emplace_back();
            else if (dxfs.empty()) continue;
            else if (n == "fill") inFill = !x.empty;
            else if (n == "b") dxfs.back().bold = val != "0" && val != "false";
            else if (n == "color" && !inFill) dxfs.back().color = XlColor(a, theme);
            else if ((n == "bgColor" || (n == "fgColor" && !dxfs.back().fill)) && inFill) dxfs.back().fill = XlColor(a, theme);
        } else if (sec == Xfs && n == "alignment" && !styles.empty()) {
            SheetStyle& s = styles.back();
            std::string_view h = Attr(a, "horizontal"), v = Attr(a, "vertical"), w = Attr(a, "wrapText");
            s.h = h == "left" || h == "fill" || h == "justify" ? 1 : h == "center" || h == "centerContinuous" || h == "distributed" ? 2
                : h == "right" ? 3 : 0;
            s.v = v == "top" ? 0 : v == "center" || v == "justify" || v == "distributed" ? 1 : 2;
            s.wrap = w == "1" || w == "true";
            s.indent = (UINT8)std::clamp(AttrInt(a, "indent", 0), 0, 15);
        }
    }
    if (styles.empty()) styles.emplace_back(), fmts.push_back("General");
}

// "AB12" -> row 11, column 27 (column -1 without letters, row -1 without digits).
void Ref(std::string_view ref, long long& r, long long& c) {
    size_t k = 0;
    for (c = 0; k < ref.size() && ref[k] >= 'A' && ref[k] <= 'Z'; k++) c = c * 26 + (ref[k] - 'A' + 1);
    c--, r = k < ref.size() ? atoll(std::string(ref.substr(k)).c_str()) - 1 : -1;
}

// Column width in Excel's character units -> DIPs (Excel's rule; mdw: the default font's widest digit).
float ColPx(double w, float mdw) { return (float)std::trunc((256 * w + std::trunc(128 / mdw)) / 256 * mdw); }

}  // namespace

Microsoft::WRL::ComPtr<IWICBitmap> DecodePicture(std::string_view bytes, UINT w, UINT h) {
    using Microsoft::WRL::ComPtr;
    IWICImagingFactory* wic = WicFactory();
    ComPtr<IWICStream> st;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapScaler> sc;
    ComPtr<IWICFormatConverter> cv;
    ComPtr<IWICBitmap> out;
    if (FAILED(wic->CreateStream(&st)) || FAILED(st->InitializeFromMemory((BYTE*)bytes.data(), (DWORD)bytes.size())) ||
        FAILED(wic->CreateDecoderFromStream(st.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &frame)) || FAILED(wic->CreateBitmapScaler(&sc)) ||
        FAILED(sc->Initialize(frame.Get(), w, h, WICBitmapInterpolationModeFant)) || FAILED(wic->CreateFormatConverter(&cv)) ||
        FAILED(cv->Initialize(sc.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) ||
        FAILED(wic->CreateBitmapFromSource(cv.Get(), WICBitmapCacheOnLoad, &out)))
        return nullptr;
    return out;
}

namespace {

// Pictures (decoded at drawn size x scale) and charts (placeholders) of a drawing, at their cell anchors.
void XlPictures(const std::wstring& path, const std::wstring& drawing, GridData& g, float scale) {
    size_t slash = drawing.rfind(L'/');
    std::wstring dir = drawing.substr(0, slash + 1), relsName = dir + L"_rels/" + drawing.substr(slash + 1) + L".rels";
    Members m;
    ArchiveReadMembers(path, [&](std::wstring_view n) { return n == drawing || n == relsName ? kMaxXml : 0; }, m);
    auto rels = Rels(Member(m, relsName), dir);
    auto edge = [](const std::vector<float>& sizes, float def, long long i) {  // i from the file: any value
        float at = 0;
        for (long long k = 0; k < i && k < (long long)sizes.size(); k++) at += sizes[k];
        return at + def * (float)std::max(0LL, i - (long long)sizes.size());
    };
    std::vector<std::wstring> media;  // per picture
    long long v[2][4] = {}, ext[2] = {}, pos[2] = {}, *field = nullptr;  // from/to: col, colOff, row, rowOff
    int which = -1;
    std::string embed;
    bool chart = false, gotExt = false;
    for (Xml x{Member(m, drawing)}; int t = x.Next();) {
        if (t == Xml::Text) {
            if (field) *field = atoll(std::string(x.text).c_str());
            continue;
        }
        std::string_view n = x.name;
        bool anchor = n == "twoCellAnchor" || n == "oneCellAnchor" || n == "absoluteAnchor";
        if (x.close) {
            if (n == "from" || n == "to") which = -1;
            field = nullptr;
            if (!anchor || (embed.empty() && !chart)) continue;
            const double emu = 9525;  // per DIP
            D2D1_RECT_F rc;
            rc.left = edge(g.colW, g.defColW, v[0][0]) + float(v[0][1] / emu), rc.top = edge(g.rowH, g.defRowH, v[0][2]) + float(v[0][3] / emu);
            if (n == "absoluteAnchor") rc.left = float(pos[0] / emu), rc.top = float(pos[1] / emu);
            if (n == "twoCellAnchor")
                rc.right = edge(g.colW, g.defColW, v[1][0]) + float(v[1][1] / emu), rc.bottom = edge(g.rowH, g.defRowH, v[1][2]) + float(v[1][3] / emu);
            else rc.right = rc.left + float(ext[0] / emu), rc.bottom = rc.top + float(ext[1] / emu);
            if (rc.right > rc.left && rc.bottom > rc.top) g.pics.push_back({rc}), media.push_back(embed.empty() ? L"" : rels[embed]);
            continue;
        }
        if (anchor) memset(v, 0, sizeof v), which = -1, embed.clear(), chart = gotExt = false;
        else if (n == "from" || n == "to") which = n == "to";
        else if (which >= 0 && (n == "col" || n == "colOff" || n == "row" || n == "rowOff"))
            field = &v[which][n == "col" ? 0 : n == "colOff" ? 1 : n == "row" ? 2 : 3];
        else if (n == "ext" && !gotExt)
            ext[0] = atoll(std::string(Attr(x.attrs, "cx")).c_str()), ext[1] = atoll(std::string(Attr(x.attrs, "cy")).c_str()), gotExt = true;
        else if (n == "pos") pos[0] = atoll(std::string(Attr(x.attrs, "x")).c_str()), pos[1] = atoll(std::string(Attr(x.attrs, "y")).c_str());
        else if (n == "blip") embed = Attr(x.attrs, "embed");
        else if (n == "graphicFrame") chart = true;
    }
    m.clear();
    ArchiveReadMembers(path, [&](std::wstring_view n) { return std::find(media.begin(), media.end(), n) != media.end() ? kMaxXml : 0; }, m);
    for (size_t i = 0; i < g.pics.size(); i++)
        if (std::string_view bytes = media[i].empty() ? std::string_view{} : Member(m, media[i]); !bytes.empty()) {
            D2D1_RECT_F rc = g.pics[i].rc;
            g.pics[i].wic = DecodePicture(bytes, std::max(1u, (UINT)std::lround((rc.right - rc.left) * scale)),
                                      std::max(1u, (UINT)std::lround((rc.bottom - rc.top) * scale)));
        }
}

std::unique_ptr<GridData> XlsxGrid(const std::wstring& path, int want, float picScale, const std::atomic<int>* gen, int mine) {
    bool quick = picScale <= 0;
    size_t cap = quick ? kQuickXml : kMaxXml;
    Members m;
    auto meta = [&](std::wstring_view n) -> size_t {
        if (n == L"xl/sharedStrings.xml") return cap;
        return n == L"xl/workbook.xml" || n == L"xl/_rels/workbook.xml.rels" || n == L"xl/styles.xml" ||
                       (n.starts_with(L"xl/theme/theme") && n.ends_with(L".xml"))
                   ? kMaxXml : 0;
    };
    if (!ArchiveReadMembers(path, meta, m)) return nullptr;
    auto g = std::make_unique<GridData>();
    std::vector<std::string> rids;
    bool d1904 = false;
    for (Xml x{Member(m, L"xl/workbook.xml")}; int t = x.Next();)
        if (t != Xml::Tag || x.close) continue;
        else if (x.name == "sheet") g->tabs.push_back(Wide(XmlText(Attr(x.attrs, "name")))), rids.emplace_back(Attr(x.attrs, "id"));
        else if (x.name == "workbookView") g->tab = AttrInt(x.attrs, "activeTab", 0);
        else if (x.name == "workbookPr") d1904 = Attr(x.attrs, "date1904") == "1" || Attr(x.attrs, "date1904") == "true";
    if (rids.empty()) return nullptr;
    if (want >= 0) g->tab = want;
    g->tab = std::clamp(g->tab, 0, (int)rids.size() - 1);
    std::wstring target = Rels(Member(m, L"xl/_rels/workbook.xml.rels"), L"xl/")[rids[g->tab]];
    std::vector<UINT32> theme;
    for (auto& [name, data] : m)
        if (name.starts_with(L"xl/theme/")) theme = XlTheme(data);
    std::vector<std::string> fmts;
    std::vector<SheetStyle> dxfs;
    XlStyles(Member(m, L"xl/styles.xml"), theme, g->styles, fmts, dxfs);
    float mdw = GridDigitWidth(g->styles[0].font, g->styles[0].size);
    std::vector<std::string> strings;
    std::string_view sst = Member(m, L"xl/sharedStrings.xml");
    bool more = sst.size() >= cap;  // open path: strings past the first part come with the worker
    {
        bool inText = false;
        for (Xml x{sst}; int t = x.Next();)
            if (t == Xml::Text) {
                if (inText && !strings.empty()) AppendXml(strings.back(), x.text);
            } else if (x.name == "si" && !x.close) strings.emplace_back();
            else if (x.name == "t") inText = !x.close && !x.empty;
            else if (x.name == "rPh" && !x.close) x.Skip();  // phonetic guide
    }
    size_t slash = target.rfind(L'/');
    std::wstring dir = target.substr(0, slash + 1), relsName = dir + L"_rels/" + target.substr(slash + 1) + L".rels";
    m.clear();
    size_t sheetCap = quick ? kQuickXml : 3 * kMaxXml;  // 50k rows of 20 numbers are ~40 MB of XML
    if (!ArchiveReadMembers(path, [&](std::wstring_view n) { return n == target ? sheetCap : n == relsName ? kMaxXml : 0; }, m)) return nullptr;
    std::string_view xml = Member(m, target);
    bool cut = xml.size() >= sheetCap;

    Sheet s;
    std::vector<UINT16> rowXf, colXf;
    auto& colW = g->colW;
    auto& rowH = g->rowH;
    long long r = -1, c = -1;
    std::string type, value, drawingId, dim, sqref;
    struct Cf { std::string ranges, op; int dxf, priority; std::vector<double> f; std::vector<std::array<long long, 4>> rects; };
    std::vector<Cf> cfs;  // conditional formats: "cellIs" rules against constants
    unsigned xf = 0;
    bool inValue = false;
    double defW = 0, baseW = 8;
    for (Xml x{xml}; int t = x.Next();) {
        if (t == Xml::Text) {
            if (inValue) AppendXml(value, x.text);
            continue;
        }
        std::string_view n = x.name, a = x.attrs;
        if (n == "c") {
            if (!x.close) {
                long long row, col;
                Ref(Attr(a, "r"), row, col);
                c = col >= 0 ? col : c + 1;
                type = Attr(a, "t"), xf = AttrInt(a, "s", 0), value.clear();
                if (xf >= fmts.size()) xf = 0;
                if (!x.empty) continue;
            }
            std::string v;
            bool num = false;
            if (type == "s") {
                if (size_t k = (size_t)atoll(value.c_str()); k < strings.size()) v = strings[k];
            } else if (type == "b") v = value == "1" ? "TRUE" : "FALSE";
            else if (type == "str" || type == "inlineStr" || type == "e") v = value;
            else if (!value.empty()) v = FormatNumber(atof(value.c_str()), fmts[xf], d1904), num = true;
            if (r >= 0 && c >= 0) Put(s, (size_t)r, (size_t)c, v, num, xf, num ? atof(value.c_str()) : 0);
        } else if (n == "v" || (n == "t" && type == "inlineStr")) inValue = !x.close && !x.empty;
        else if (n == "formula" && !cfs.empty()) {
            if (!x.close && !x.empty) {
                Xml y = x;  // a constant number, else the rule is skipped
                std::string f = y.Next() == Xml::Text ? XmlText(y.text) : "";
                char* end = nullptr;
                double d = strtod(f.c_str(), &end);
                if (f.empty() || *end) cfs.back().dxf = -1;
                cfs.back().f.push_back(d);
            }
        } else if (x.close) continue;
        else if (n == "conditionalFormatting") sqref = Attr(a, "sqref");
        else if (n == "cfRule")
            cfs.push_back({sqref, std::string(Attr(a, "operator")), Attr(a, "type") == "cellIs" ? AttrInt(a, "dxfId", -1) : -1, AttrInt(a, "priority", 0)});
        else if (n == "row") {
            r = AttrInt(a, "r", (int)r + 2) - 1, c = -1;
            if (quick && r >= (long long)kQuickRows) {
                more = true;
                break;
            }
            if ((r & 1023) == 0 && gen && *gen != mine) return nullptr;
            if (r >= (long long)kMaxRows) break;
            if (r < 0) continue;  // r="0" or negative: its cells are dropped too (Put needs r >= 0)
            std::string_view ht = Attr(a, "ht"), hidden = Attr(a, "hidden");
            if (!ht.empty() || hidden == "1" || hidden == "true") {
                if (rowH.size() <= (size_t)r) rowH.resize(r + 1, -1);
                rowH[r] = hidden == "1" || hidden == "true" ? 0 : (float)atof(std::string(ht).c_str()) * 96 / 72;
            }
            if (Attr(a, "customFormat") == "1") {
                if (rowXf.size() <= (size_t)r) rowXf.resize(r + 1);
                rowXf[r] = (UINT16)AttrInt(a, "s", 0);
            }
        } else if (n == "rPh") x.Skip();
        else if (n == "dimension") dim = Attr(a, "ref");
        else if (n == "sheetView") g->gridLines = Attr(a, "showGridLines") != "0" && Attr(a, "showGridLines") != "false";
        else if (n == "pane" && Attr(a, "state").starts_with("frozen"))
            g->frozenCols = (UINT32)std::max(0, AttrInt(a, "xSplit", 0)), g->frozenRows = (UINT32)std::max(0, AttrInt(a, "ySplit", 0));
        else if (n == "sheetFormatPr") {
            if (std::string_view h = Attr(a, "defaultRowHeight"); !h.empty()) g->defRowH = (float)atof(std::string(h).c_str()) * 96 / 72;
            defW = atof(std::string(Attr(a, "defaultColWidth")).c_str());
            baseW = std::max(0, AttrInt(a, "baseColWidth", 8));
        } else if (n == "col") {
            size_t from = std::max(1, AttrInt(a, "min", 1)) - 1, to = std::min<size_t>(std::max(1, AttrInt(a, "max", 1)), kMaxCols);
            std::string_view w = Attr(a, "width"), hidden = Attr(a, "hidden");
            int st = AttrInt(a, "style", 0);
            for (size_t k = from; k < to; k++) {
                if (colW.size() <= k) colW.resize(k + 1, -1);
                colW[k] = hidden == "1" || hidden == "true" ? 0 : w.empty() ? -1 : ColPx(atof(std::string(w).c_str()), mdw);
                if (st > 0 && colXf.size() <= k) colXf.resize(k + 1);
                if (st > 0) colXf[k] = (UINT16)st;
            }
        } else if (n == "mergeCell") {
            std::string_view ref = Attr(a, "ref");
            size_t colon = std::min(ref.find(':'), ref.size());
            long long r0, c0, r1, c1;
            Ref(ref.substr(0, colon), r0, c0), Ref(ref.substr(std::min(colon + 1, ref.size())), r1, c1);
            if (r0 >= 0 && c0 >= 0 && r1 >= r0 && c1 >= c0)
                g->merges.push_back({(UINT32)r0, (UINT32)c0, (UINT32)std::min<long long>(r1, kMaxRows), (UINT32)std::min<long long>(c1, kMaxCols)});
        } else if (n == "drawing") drawingId = Attr(a, "id");
    }
    more |= cut;
    if (more) std::erase_if(s.vals, [&](const Sheet::Val& v) { return (long long)v.r >= r; });  // a row maybe cut short
    g->defColW = defW > 0 ? ColPx(defW, mdw) : std::ceil((float)(baseW * mdw + 5) / 8) * 8;
    for (float& w : colW) w = w < 0 ? g->defColW : w;
    for (float& h : rowH) h = h < 0 ? g->defRowH : h;
    for (UINT16& k : rowXf) k = k < g->styles.size() ? k : 0;
    for (UINT16& k : colXf) k = k < g->styles.size() ? k : 0;
    // Conditional formats: the first matching rule (by priority) gives the number a derived style.
    std::stable_sort(cfs.begin(), cfs.end(), [](const Cf& p, const Cf& q) { return p.priority < q.priority; });
    std::erase_if(cfs, [&](const Cf& f) { return f.dxf < 0 || f.dxf >= (int)dxfs.size() || f.f.empty(); });
    for (auto& f : cfs) {  // "A1:B9 D4" -> rows/columns, once (not per cell)
        std::string_view rs = f.ranges;
        for (size_t k = 0; k < rs.size();) {
            size_t e = std::min(rs.find(' ', k), rs.size()), colon = std::min(rs.find(':', k), e);
            long long r0, c0, r1, c1;
            Ref(rs.substr(k, colon - k), r0, c0);
            Ref(colon < e ? rs.substr(colon + 1, e - colon - 1) : rs.substr(k, e - k), r1, c1);
            f.rects.push_back({r0, c0, r1, c1});
            k = e + 1;
        }
    }
    std::map<std::pair<UINT16, int>, UINT16> derived;
    for (auto& v : s.vals)
        for (auto& f : cfs) {
            if (!v.num) break;
            bool in = false;
            for (auto& [r0, c0, r1, c1] : f.rects) in |= v.r >= r0 && v.r <= r1 && v.c >= c0 && v.c <= c1;
            double a = f.f[0], b = f.f.size() > 1 ? f.f[1] : a, x = v.x;
            const std::string& op = f.op;
            bool hit = op == "lessThan" ? x < a : op == "lessThanOrEqual" ? x <= a : op == "greaterThan" ? x > a
                     : op == "greaterThanOrEqual" ? x >= a : op == "equal" ? x == a : op == "notEqual" ? x != a
                     : op == "between" ? x >= std::min(a, b) && x <= std::max(a, b)
                     : op == "notBetween" ? x < std::min(a, b) || x > std::max(a, b) : false;
            if (!in || !hit) continue;
            auto [it, added] = derived.try_emplace({v.xf, f.dxf}, (UINT16)std::min<size_t>(g->styles.size(), 65535));
            if (added && g->styles.size() < 65535) {
                SheetStyle st = g->styles[v.xf];
                const SheetStyle& d = dxfs[f.dxf];
                st.color = d.color ? d.color : st.color, st.fill = d.fill ? d.fill : st.fill, st.bold |= d.bold;
                g->styles.push_back(st);
            }
            if (it->second < g->styles.size()) v.xf = it->second;
            break;
        }
    Fill(*g, s, rowXf, colXf);
    if (colW.size() < g->cols) colW.resize(g->cols, g->defColW);  // widths from the sheet, not the text
    if (quick) g->more = more || !drawingId.empty();
    else if (!drawingId.empty()) XlPictures(path, Rels(Member(m, relsName), dir)[drawingId], *g, picScale);
    long long r1, c1;  // the size the sheet says it has, while only its first rows are read
    Ref(dim.substr(std::min(dim.find(':') + 1, dim.size())), r1, c1);
    if (quick && more && r1 > 0 && c1 >= 0)
        g->sub = std::to_wstring(r1 + 1) + L" rows × " + std::to_wstring(c1 + 1) + (c1 ? L" columns" : L" column");
    else if (cut) g->sub += L" · first " + std::to_wstring(sheetCap >> 20) + L" MB";
    return g;
}

// ---- ods: the table `want`, cells as displayed (text:p); stops after maxRows rows (more = true).
// `more` comes in true when the XML was cut: then the table may lie beyond it, and `want` stays.
Sheet OdsSheet(std::string_view xml, int& want, std::vector<std::wstring>& tabs, size_t maxRows, bool& more,
               const std::atomic<int>* gen, int mine) {
    Sheet rows;
    std::vector<Cell> row;
    std::string cell;
    int rowRep = 1, cellRep = 1, depth = 0, tableDepth = -1;
    size_t r = 0, blank = 0;
    bool inCell = false, inPara = false, num = false, ours = false;
    if (want < 0) want = 0;
    for (Xml x{xml}; int t = x.Next();) {
        if (t == Xml::Text) {
            if (inPara) AppendXml(cell, x.text);
            continue;
        }
        std::string_view n = x.name;
        if (x.close) {
            depth--;
            if (n == "p") inPara = false;
            else if ((n == "table-cell" || n == "covered-table-cell") && inCell) {
                for (int k = 0; k < cellRep && row.size() < kMaxCols; k++) row.push_back({cell, num});
                inCell = false;
            } else if (n == "table-row" && !ours) row.clear();
            else if (n == "table-row") {
                while (!row.empty() && row.back().v.empty()) row.pop_back();
                if (row.empty()) blank += rowRep;  // repeated empty rows pad sheets to a million
                else {
                    r += std::min<size_t>(blank, 100), blank = 0;
                    // shortcut: a row with content repeated more than 1000 times shows 1000 (a 476-byte file can ask for 25M cells)
                    for (int k = 0; k < std::min(rowRep, 1000) && r < std::min(kMaxRows, maxRows); k++, r++)
                        for (size_t c = 0; c < row.size(); c++) Put(rows, r, c, row[c].v, row[c].num);
                }
                row.clear();
                if ((r & 255) == 0 && gen && *gen != mine) return {};  // a newer preview: stop
                if (ours && r >= maxRows) {
                    more = true;
                    break;
                }
            } else if (n == "table" && depth == tableDepth) ours = false, tableDepth = -1;
            continue;
        }
        if (!x.empty) depth++;
        if (n == "annotation") x.Skip(), depth -= !x.empty;
        else if (n == "table" && tableDepth < 0) {
            ours = (int)tabs.size() == want, tableDepth = depth - !x.empty;
            tabs.push_back(Wide(XmlText(Attr(x.attrs, "name"))));
        }
        else if (n == "table-row") rowRep = std::clamp(AttrInt(x.attrs, "number-rows-repeated", 1), 1, (int)kMaxRows);
        else if (n == "table-cell" || n == "covered-table-cell") {
            cellRep = std::clamp(AttrInt(x.attrs, "number-columns-repeated", 1), 1, (int)kMaxCols);
            cell.clear();
            inCell = !x.empty;
            std::string_view type = Attr(x.attrs, "value-type");
            num = type == "float" || type == "percentage" || type == "currency" || type == "date" || type == "time";
            if (x.empty)
                for (int k = 0; k < cellRep && row.size() < kMaxCols; k++) row.emplace_back();
        } else if (n == "p" && inCell) {
            if (!cell.empty()) cell += ' ';
            inPara = !x.empty;
        } else if (n == "s" && inPara) cell.append(std::clamp(AttrInt(x.attrs, "c", 1), 1, 100), ' ');
        else if ((n == "tab" || n == "line-break") && inPara) cell += ' ';
    }
    if (want >= (int)tabs.size() && !more) want = 0;
    return rows;
}

// ---- OLE compound files (doc, xls, ppt)
Microsoft::WRL::ComPtr<IStorage> OpenOle(const std::wstring& path) {
    Microsoft::WRL::ComPtr<IStorage> stg;
    // Direct mode opens in 0.2 ms (transacted: ~8 ms for 12 MB); a file open for writing (Excel) needs transacted.
    if (FAILED(StgOpenStorageEx(path.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE, STGFMT_STORAGE, 0, nullptr, nullptr, IID_PPV_ARGS(&stg))))
        StgOpenStorageEx(path.c_str(), STGM_READ | STGM_SHARE_DENY_NONE | STGM_TRANSACTED, STGFMT_STORAGE, 0, nullptr,
                     nullptr, IID_PPV_ARGS(&stg));
    return stg;
}
bool ReadStream(IStorage* stg, const wchar_t* name, std::string& out, size_t cap = 256 << 20) {
    Microsoft::WRL::ComPtr<IStream> s;
    STATSTG st{};
    if (FAILED(stg->OpenStream(name, nullptr, STGM_READ | STGM_SHARE_EXCLUSIVE, 0, &s)) || FAILED(s->Stat(&st, STATFLAG_NONAME)))
        return false;
    out.resize((size_t)std::min<ULONGLONG>(st.cbSize.QuadPart, cap));
    ULONG read = 0;
    s->Read(out.data(), (ULONG)out.size(), &read);
    out.resize(read);
    return true;
}
unsigned U16(std::string_view s, size_t o) { return o + 2 <= s.size() ? (unsigned char)s[o] | (unsigned char)s[o + 1] << 8 : 0; }
unsigned U32(std::string_view s, size_t o) { return o + 4 <= s.size() ? U16(s, o) | U16(s, o + 2) << 16 : 0; }

// .doc (Word 97+): the main text through the piece table. Paragraphs only.
// shortcut: no headings, bold or table structure (needs the style sheet and FKPs); cells become paragraphs.
std::string DocToMd(IStorage* stg, bool& cut) {
    std::string wd, table;
    if (!ReadStream(stg, L"WordDocument", wd) || U16(wd, 0) != 0xA5EC) return "";
    unsigned flags = U16(wd, 0x0A);
    if (flags & 0x0100) return "";  // encrypted
    size_t p = 32 + 2 + U16(wd, 32) * 2;  // FibRgW97
    size_t lw = p + 2;
    p = lw + U16(wd, p) * 4;              // FibRgLw97
    size_t fc = p + 2;
    if (U16(wd, p) < 34) return "";
    unsigned ccpText = U32(wd, lw + 3 * 4), fcClx = U32(wd, fc + 33 * 8), lcbClx = U32(wd, fc + 33 * 8 + 4);
    if (!ReadStream(stg, flags & 0x0200 ? L"1Table" : L"0Table", table) || (size_t)fcClx + lcbClx > table.size()) return "";
    size_t k = fcClx, end = fcClx + lcbClx;
    while (k < end && table[k] == 1) k += 3 + U16(table, k + 1);  // Prc
    if (k >= end || table[k] != 2) return "";
    size_t lcb = std::min<size_t>(U32(table, k + 1), end - std::min(end, k + 5));  // PlcPcd: within the Clx
    k += 5;
    size_t pieces = lcb >= 4 ? (lcb - 4) / 12 : 0;
    std::wstring text;
    for (size_t j = 0; j < pieces && !(cut = text.size() >= kQuickDoc / 2); j++) {
        unsigned cp0 = U32(table, k + j * 4), cp1 = std::min(U32(table, k + j * 4 + 4), ccpText);
        if (cp0 >= cp1) continue;
        unsigned fcv = U32(table, k + (pieces + 1) * 4 + j * 8 + 2), n = cp1 - cp0;
        if (fcv & 0x40000000) {  // 8-bit text (cp1252)
            size_t off = (fcv & 0x3FFFFFFF) / 2;
            if (off + n > wd.size()) break;
            size_t at = text.size();
            text.resize(at + n);
            MultiByteToWideChar(1252, 0, wd.data() + off, (int)n, text.data() + at, (int)n);
        } else {
            size_t off = fcv & 0x3FFFFFFF;
            if (off + n * 2 > wd.size()) break;
            text.append((const wchar_t*)(wd.data() + off), n);
        }
    }
    cut = text.size() >= kQuickDoc / 2;  // (one piece: the loop above never tests it)
    text.resize(std::min(text.size(), kQuickDoc / 2));
    MdOut md;
    md.Open();
    std::vector<bool> field;  // per open field: still in its code (shown from the separator on)
    std::wstring run;         // plain characters, converted together
    for (wchar_t c : text) {
        bool hide = !field.empty() && field.back();
        if (c >= 0x20 && !hide) {
            run += c;
            continue;
        }
        if (!run.empty()) md.Text(Utf8(run)), run.clear();
        if (c == 0x13) field.push_back(true);
        else if (c == 0x14 && !field.empty()) field.back() = false;
        else if (c == 0x15 && !field.empty()) field.pop_back();
        else if (hide) continue;
        else if (c == 0x0D || c == 0x07 || c == 0x0C) md.Close(), md.Open();
        else if (c == 0x0B) md.Text("\n");
        else if (c == 0x09) md.Text(" ");
        else if (c == 0x1E) md.Text("-");
    }
    if (!run.empty()) md.Text(Utf8(run));
    md.Close();
    return md.out;
}

// .ppt: slides in the order of the document's SlideListWithText; per slide its text there (older files) and the
// text atoms in its drawing (shapes, text boxes), typed by TextHeaderAtom (title / body / other).
// Records are found through the persist directory of the newest edit (Current User -> UserEditAtom chain).
std::string PptToMd(IStorage* stg, int& count, bool& cut) {
    std::string s, cu;
    if (!ReadStream(stg, L"PowerPoint Document", s) || !ReadStream(stg, L"Current User", cu)) return "";
    std::unordered_map<unsigned, unsigned> persist;  // persist id -> stream offset (newest edit wins)
    unsigned docRef = 0;
    for (unsigned edit = U32(cu, 16), guard = 0; edit && U16(s, edit + 2) == 0x0FF5 && guard < 1000; guard++) {
        if (!docRef) docRef = U32(s, edit + 24);
        unsigned dir = U32(s, edit + 20);
        if (U16(s, dir + 2) == 0x1772)
            for (size_t q = dir + 8, end = std::min(s.size(), dir + 8 + (size_t)U32(s, dir + 4)); q + 4 <= end;) {
                unsigned v = U32(s, q), id = v & 0xFFFFF, n = v >> 20;
                q += 4;
                for (unsigned j = 0; j < n && q + 4 <= end; j++, q += 4) persist.try_emplace(id + j, U32(s, q));
            }
        unsigned last = U32(s, edit + 16);
        if (last >= edit) break;
        edit = last;
    }
    auto at = [&](unsigned ref) { auto it = persist.find(ref); return it == persist.end() ? s.size() : (size_t)it->second; };
    size_t doc = at(docRef);
    if (U16(s, doc + 2) != 0x03E8) return "";

    Deck deck;
    std::string title, body;
    unsigned type = 4;
    auto text = [&](size_t a, unsigned rt, unsigned len) {
        std::wstring w;
        if (rt == 0x0FA0) w.assign((const wchar_t*)(s.data() + a + 8), len / 2);
        else for (unsigned j = 0; j < len; j++) w += (wchar_t)(unsigned char)s[a + 8 + j];  // low bytes of UTF-16
        MdOut md;
        if (type == 0 || type == 6) md.capture = &title;
        else if (type == 2) return;  // notes
        bool bullets = type == 1 || type == 7 || type == 8;
        md.Open(0, bullets ? 0 : -1);
        for (size_t j = 0, e; j <= w.size(); j = e + 1) {  // paragraphs end with \r, lines with 0x0B
            e = std::min(w.find_first_of(L"\r\x0B", j), w.size());
            std::wstring part = w.substr(j, e - j);
            std::erase_if(part, [](wchar_t c) { return c < 0x20 && c != L'\t'; });
            md.Text(Utf8(part));
            if (e < w.size()) w[e] == L'\r' ? (md.Close(), md.Open(0, bullets ? 0 : -1)) : md.Text("\n");
        }
        md.Close();
        if (md.inList) md.out += '\n';
        body += md.out;
    };
    // Text atoms anywhere under a container (OfficeArt shapes nest), typed by the TextHeaderAtom before them.
    auto walk = [&](auto& self, size_t from, size_t end, int depth) -> void {
        for (size_t a = from; a + 8 <= end && depth < 16; a += 8 + (size_t)U32(s, a + 4)) {
            unsigned rt = U16(s, a + 2), len = U32(s, a + 4);
            if (a + 8 + len > end) break;
            if ((U16(s, a) & 0xF) == 0xF) self(self, a + 8, a + 8 + len, depth + 1);
            else if (rt == 0x0F9F) type = U32(s, a + 8);
            else if (rt == 0x0FA0 || rt == 0x0FA8) text(a, rt, len);
        }
    };
    size_t docEnd = std::min(s.size(), doc + 8 + (size_t)U32(s, doc + 4));
    for (size_t k = doc + 8; k + 8 <= docEnd; k += 8 + (size_t)U32(s, k + 4)) {
        if (U16(s, k + 2) != 0x0FF0 || U16(s, k) >> 4 != 0) continue;  // the slides' SlideListWithText
        size_t listEnd = std::min(docEnd, k + 8 + (size_t)U32(s, k + 4)), slide = s.size();
        auto finish = [&] {
            if (slide == s.size()) return;
            if (U16(s, slide + 2) == 0x03EE) walk(walk, slide + 8, std::min(s.size(), slide + 8 + (size_t)U32(s, slide + 4)), 0);
            deck.Add(title, body);
            title.clear(), body.clear();
        };
        for (size_t a = k + 8; a + 8 <= listEnd && !(cut = deck.out.size() >= kQuickDoc); a += 8 + (size_t)U32(s, a + 4)) {
            unsigned rt = U16(s, a + 2), len = U32(s, a + 4);
            if (a + 8 + len > listEnd) break;
            if (rt == 0x03F3) finish(), slide = at(U32(s, a + 8));
            else if (rt == 0x0F9F) type = U32(s, a + 8);
            else if (rt == 0x0FA0 || rt == 0x0FA8) text(a, rt, len);
        }
        finish();
    }
    count = deck.n;
    return deck.out;
}

// .xls (BIFF8): the active sheet's cells; strings from the SST, dates from the XF number formats.
struct Segs {  // record data split over CONTINUE records
    std::vector<std::string_view> seg;
    size_t s = 0, o = 0;
    bool Byte(unsigned& b) {
        while (s < seg.size() && o >= seg[s].size()) s++, o = 0;
        if (s >= seg.size()) return false;
        b = (unsigned char)seg[s][o++];
        return true;
    }
    unsigned Get(int bytes) {
        unsigned v = 0, b = 0;
        for (int k = 0; k < bytes && Byte(b); k++) v |= b << (8 * k);
        return v;
    }
    void Skip(size_t n) {
        unsigned b;
        while (n-- && Byte(b)) {}
    }
    // Characters may continue in the next record, which then starts with a new flags byte.
    std::string Chars(size_t cch, bool high) {
        std::wstring w;
        while (cch && s < seg.size()) {
            if (o >= seg[s].size()) {
                if (++s >= seg.size()) break;
                o = 0, high = seg[s][o++] & 1;
                continue;
            }
            w += (wchar_t)(high ? Get(2) : Get(1)), cch--;
        }
        return Utf8(w);
    }
    std::string String(bool wideLen) {  // XLUnicodeRichExtendedString / XLUnicodeString / ShortXLUnicodeString
        size_t cch = Get(wideLen ? 2 : 1);
        unsigned flags = Get(1);
        size_t runs = flags & 8 ? Get(2) : 0, ext = flags & 4 ? Get(4) : 0;
        std::string r = Chars(cch, flags & 1);
        Skip(runs * 4 + ext);
        return r;
    }
};

Sheet XlsSheet(IStorage* stg, int& want, std::vector<std::wstring>& tabs, size_t maxRows, bool& more,
              const std::atomic<int>* gen, int mine) {
    // The open path reads the globals and the start of the sheet, not the whole stream (12 MB: 5 ms), and
    // decodes shared strings only as far as its cells use them (50k strings: 5 ms).
    bool quick = maxRows < kMaxRows;
    Microsoft::WRL::ComPtr<IStream> st;
    STATSTG info{};
    if (FAILED(stg->OpenStream(L"Workbook", nullptr, STGM_READ | STGM_SHARE_EXCLUSIVE, 0, &st)) || FAILED(st->Stat(&info, STATFLAG_NONAME)))
        return {};
    size_t total = (size_t)std::min<ULONGLONG>(info.cbSize.QuadPart, 256 << 20);
    auto read = [&](size_t at, size_t n, std::string& out) {
        out.resize(std::min(n, total - std::min(at, total)));
        ULONG got = 0;
        LARGE_INTEGER li{};
        li.QuadPart = (LONGLONG)at;
        if (FAILED(st->Seek(li, STREAM_SEEK_SET, nullptr)) || FAILED(st->Read(out.data(), (ULONG)out.size(), &got))) got = 0;
        out.resize(got);
    };
    std::string s, part;
    std::vector<std::string> sst;
    Segs sstData;
    unsigned sstLeft = 0;
    std::unordered_map<unsigned, std::string> codes;
    std::vector<unsigned> xfFormat;
    struct Bound { size_t pos; bool work; std::string name; };
    std::vector<Bound> sheets;
    size_t active = 0, k = 0;
    auto rec = [](std::string_view b, size_t at) { return b.substr(std::min(at + 4, b.size()), U16(b, at + 2)); };
    for (size_t first = quick ? 1 << 20 : total;; first = total) {  // globals past the first MB: read it all
        read(0, first, s);
        if (U16(s, 0) != 0x0809 || U16(s, 4) != 0x0600) return {};  // BIFF8 only
        sstData = {}, sstLeft = 0, codes.clear(), xfFormat.clear(), sheets.clear(), active = 0;
        for (k = 0; k + 4 <= s.size() && U16(s, k) != 0x000A; k += 4 + U16(s, k + 2)) {
            unsigned type = U16(s, k);
            std::string_view d = rec(s, k);
            if (type == 0x00FC) {  // SST
                sstData = {{d.substr(std::min<size_t>(8, d.size()))}}, sstLeft = U32(d, 4);
                for (size_t c = k + 4 + d.size(); c + 4 <= s.size() && U16(s, c) == 0x003C; c += 4 + U16(s, c + 2)) sstData.seg.push_back(rec(s, c));
            } else if (type == 0x041E) {  // FORMAT
                Segs g{{d.substr(std::min<size_t>(2, d.size()))}};
                codes[U16(d, 0)] = g.String(true);
            } else if (type == 0x00E0) xfFormat.push_back(U16(d, 2));
            else if (type == 0x0085) {  // BOUNDSHEET
                Segs g{{d.substr(std::min<size_t>(6, d.size()))}};
                sheets.push_back({U32(d, 0), d.size() > 5 && d[5] == 0, g.String(false)});
            } else if (type == 0x003D) active = U16(d, 10);
            else if (type == 0x002F) return {};  // FILEPASS: encrypted
        }
        if (k + 4 <= s.size() || s.size() >= total) break;
    }
    auto str = [&](size_t i) -> std::string {
        for (; sst.size() <= i && sstLeft && sstData.s < sstData.seg.size(); sstLeft--) sst.push_back(sstData.String(true));
        return i < sst.size() ? sst[i] : std::string();
    };
    size_t nth = 0;  // the active sheet among the worksheets (charts and macros sheets dropped)
    for (size_t j = 0; j < active && j < sheets.size(); j++) nth += sheets[j].work;
    if (active >= sheets.size() || !sheets[active].work) nth = 0;
    std::erase_if(sheets, [](const Bound& h) { return !h.work; });
    active = nth;
    if (sheets.empty()) return {};
    if (want >= 0) active = want;
    active = std::min(active, sheets.size() - 1);
    want = (int)active;
    for (auto& h : sheets) tabs.push_back(Wide(h.name));
    auto fmt = [&](unsigned xf) -> std::string_view {
        unsigned id = xf < xfFormat.size() ? xfFormat[xf] : 0;
        auto it = codes.find(id);
        if (it != codes.end()) return it->second;  // (a ?: with the char* would return a dangling temporary)
        return BuiltinFormat(id);
    };
    auto rk = [](unsigned v) {
        double d;
        if (v & 2) d = (double)((int)v >> 2);
        else {
            unsigned long long bits = (unsigned long long)(v & ~3u) << 32;
            memcpy(&d, &bits, 8);
        }
        return v & 1 ? d / 100 : d;
    };
    // The sheet's records: in the buffer when it holds them, else read from the stream (the open path: a part).
    constexpr size_t kPart = 256 << 10;
    size_t pos = sheets[active].pos;
    std::string_view t;
    if (s.size() >= total || (quick && pos + kPart <= s.size())) t = std::string_view(s).substr(std::min(pos, s.size()));
    else read(pos, quick ? kPart : total, part), t = part;
    bool cut = pos + t.size() < total;  // the sheet may go on past `t`
    Sheet rows;
    int depth = 0;
    bool pendingString = false;
    size_t pr = 0, pc = 0, records = 0;
    for (k = 0; k + 4 + U16(t, k + 2) <= t.size(); k += 4 + U16(t, k + 2)) {  // a record cut at the end of `t`: not read
        unsigned type = U16(t, k);
        std::string_view d = rec(t, k);
        size_t r = U16(d, 0), c = U16(d, 2);
        unsigned xf = U16(d, 4);
        if ((++records & 4095) == 0 && gen && *gen != mine) return {};  // a newer preview: stop
        if (type == 0x0809) depth++;
        else if (type == 0x000A && --depth <= 0) {
            cut = false;
            break;
        } else if (depth != 1) continue;
        else if (r >= maxRows && (type == 0x00FD || type == 0x0203 || type == 0x027E || type == 0x00BD || type == 0x0204 ||
                                  type == 0x0205 || type == 0x0006)) {  // cells come in row order
            more = true;
            break;
        } else if (type == 0x00FD) Put(rows, r, c, str(U32(d, 6)));
        else if (type == 0x0203 && d.size() >= 14) {
            double v;
            memcpy(&v, d.data() + 6, 8);
            Put(rows, r, c, FormatNumber(v, fmt(xf)), true);
        } else if (type == 0x027E) Put(rows, r, c, FormatNumber(rk(U32(d, 6)), fmt(xf)), true);
        else if (type == 0x00BD && d.size() >= 6)  // MULRK
            for (size_t o = 4; o + 6 <= d.size() - 2; o += 6, c++) Put(rows, r, c, FormatNumber(rk(U32(d, o + 2)), fmt(U16(d, o))), true);
        else if (type == 0x0204) {  // LABEL
            Segs g{{d.substr(std::min<size_t>(6, d.size()))}};
            Put(rows, r, c, g.String(true));
        } else if (type == 0x0205 && d.size() >= 8) Put(rows, r, c, d[7] ? "#ERROR" : d[6] ? "TRUE" : "FALSE");
        else if (type == 0x0006 && d.size() >= 14) {  // FORMULA: the cached result
            if (U16(d, 12) != 0xFFFF) {
                double v;
                memcpy(&v, d.data() + 6, 8);
                Put(rows, r, c, FormatNumber(v, fmt(xf)), true);
            } else if (d[6] == 0) pendingString = true, pr = r, pc = c;
            else if (d[6] == 1) Put(rows, r, c, d[8] ? "TRUE" : "FALSE");
        } else if (type == 0x0207 && pendingString) {  // STRING after a string formula
            Segs g{{d}};
            Put(rows, pr, pc, g.String(true));
            pendingString = false;
        }
    }
    more |= quick && cut;
    return rows;
}

}  // namespace

std::wstring OfficeToMarkdown(const std::wstring& path, std::wstring& sub) {
    std::string md;
    int slides = -1;
    bool cut = false;
    if (HasExt(path, L" .docx .docm .dotx .dotm ")) {
        Members m;
        ArchiveReadMembers(path, [](std::wstring_view n) { return n == L"word/document.xml" || n == L"word/styles.xml" ? kMaxXml : 0; }, m);
        md = DocxToMd(Member(m, L"word/document.xml"), Member(m, L"word/styles.xml"));
    } else if (HasExt(path, L" .odt .ott .odp .otp ")) {
        Members m;
        ArchiveReadMembers(path, [](std::wstring_view n) { return n == L"content.xml" ? kQuickDoc : 0; }, m);
        if (!m.empty()) md = OdfToMd(m[0].second, HasExt(path, L" .odp .otp "), slides), cut = m[0].second.size() >= kQuickDoc;
    } else if (auto stg = OpenOle(path)) {
        if (HasExt(path, L" .doc .dot ")) md = DocToMd(stg.Get(), cut);
        else if (HasExt(path, L" .ppt .pps .pot ")) md = PptToMd(stg.Get(), slides, cut);
    }
    if (slides > 0) sub = std::to_wstring(slides) + (slides == 1 ? L" slide" : L" slides");
    if (cut) {  // shortcut: the rest is not shown; a worker would have to re-lay out the whole document anyway
        md += "\n\n---\n\n*The rest is not shown in the preview.*\n";
        sub += (sub.empty() ? L"" : L" · ") + std::wstring(L"first ") + std::to_wstring(kQuickDoc >> 10) + L" KB";
    }
    return Wide(md);
}

bool HasExt(const std::wstring& path, const wchar_t* list) {
    const wchar_t* e = wcsrchr(path.c_str(), L'.');
    if (!e || wcschr(e, L'\\')) return false;
    std::wstring key = L" " + std::wstring(e) + L" ";
    for (auto& ch : key) ch = towlower(ch);
    return wcsstr(list, key.c_str()) != nullptr;
}

std::unique_ptr<GridData> OfficeToGrid(const std::wstring& path, int sheet, float picScale, const std::atomic<int>* gen, int mine) {
    if (HasExt(path, L" .xlsx .xlsm .xltx .xltm ")) return XlsxGrid(path, sheet, picScale, gen, mine);
    // ods/xls: the open path reads the first rows (and at most kQuickXml of ods XML); the worker the rest.
    bool quick = picScale <= 0, more = false, truncated = false;
    size_t maxRows = quick ? kQuickRows : kMaxRows;
    std::vector<std::wstring> tabs;
    Sheet rows;
    if (HasExt(path, L" .ods .ots ")) {
        // No table within the first part (styles before it): the open path reads the whole XML after all.
        for (size_t cap = quick ? kQuickXml : kMaxXml;; cap = kMaxXml) {
            Members m;
            ArchiveReadMembers(path, [&](std::wstring_view n) { return n == L"content.xml" ? cap : 0; }, m);
            if (m.empty()) break;
            bool cut = m[0].second.size() >= cap;
            more = quick && cut && cap < kMaxXml, truncated = cut && cap == kMaxXml;
            rows = OdsSheet(m[0].second, sheet, tabs, maxRows, more, gen, mine);
            if (!tabs.empty() || !cut || cap == kMaxXml) break;
            more = false;
        }
    } else if (auto stg = OpenOle(path)) rows = XlsSheet(stg.Get(), sheet, tabs, maxRows, more, gen, mine);
    if (tabs.empty()) return nullptr;
    bool capped = more && !quick;  // the worker stopped at kMaxRows: nothing more is coming
    more &= quick;
    auto g = std::make_unique<GridData>();
    Fill(*g, rows);
    g->tabs = std::move(tabs), g->tab = sheet, g->more = more;
    if (more) g->sub = L"first " + std::to_wstring(kQuickRows) + L" rows · reading…";
    if (capped) g->sub += L" · first " + std::to_wstring(kMaxRows) + L" rows";
    if (truncated) g->sub += L" · first " + std::to_wstring(kMaxXml >> 20) + L" MB";
    return g;
}

FontFace ResolveFont(const std::wstring& face) {
    static SRWLOCK lock = SRWLOCK_INIT;
    static std::unordered_map<std::wstring, FontFace> cache;
    AcquireSRWLockExclusive(&lock);
    auto [it, added] = cache.try_emplace(face, FontFace{face});
    if (added && !face.empty() && face.size() < LF_FACESIZE) {
        static Microsoft::WRL::ComPtr<IDWriteGdiInterop> gdi;
        if (!gdi) {
            Microsoft::WRL::ComPtr<IDWriteFactory> f;
            if (SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)f.GetAddressOf())))
                f->GetGdiInterop(&gdi);
        }
        LOGFONTW lf{};
        lf.lfCharSet = DEFAULT_CHARSET;
        wcscpy_s(lf.lfFaceName, face.c_str());
        Microsoft::WRL::ComPtr<IDWriteFont> font;
        Microsoft::WRL::ComPtr<IDWriteFontFamily> family;
        Microsoft::WRL::ComPtr<IDWriteLocalizedStrings> names;
        UINT32 i = 0, n = 0;
        BOOL found = FALSE;
        if (gdi && SUCCEEDED(gdi->CreateFontFromLOGFONT(&lf, &font)) && SUCCEEDED(font->GetFontFamily(&family)) &&
            SUCCEEDED(family->GetFamilyNames(&names)) && (names->FindLocaleName(L"en-us", &i, &found), true) &&
            SUCCEEDED(names->GetStringLength(found ? i : 0, &n))) {
            std::wstring name(n + 1, L'\0');
            if (SUCCEEDED(names->GetString(found ? i : 0, name.data(), n + 1)))
                name.resize(n), it->second = {name, font->GetWeight(), font->GetStretch()};
        }
    }
    FontFace r = it->second;
    ReleaseSRWLockExclusive(&lock);
    return r;
}

// --selftest: "" when the converters behave.
std::wstring OfficeSelfTest() {
    std::string md = DocxToMd(R"(<w:body><w:p><w:pPr><w:pStyle w:val="T1"/></w:pPr><w:r><w:t>Title</w:t></w:r></w:p>
<w:p><w:r><w:rPr><w:b/></w:rPr><w:t xml:space="preserve">bold </w:t></w:r><w:r><w:t>a*b &amp; c</w:t></w:r></w:p>
<w:p><w:pPr><w:numPr><w:ilvl w:val="1"/><w:numId w:val="2"/></w:numPr></w:pPr><w:r><w:t>item</w:t></w:r></w:p>
<w:tbl><w:tr><w:tc><w:p><w:r><w:t>x</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>y</w:t></w:r></w:p></w:tc></w:tr></w:tbl></w:body>)",
                              R"(<w:style w:styleId="T1"><w:name w:val="heading 1"/></w:style>)");
    if (md != "# Title\n\n**bold** a\\*b \\& c\n\n  - item\n\n| x | y |\n| --- | --- |\n\n") return L"docx: " + Wide(md);
    struct { double v; const char *code, *want; } nums[] = {
        {0.1 + 0.2, "General", "0.3"}, {1 / 3.0, "General", "0.333333333"}, {123456789012.0, "General", "1.23457E+11"},
        {-12345678901.0, "General", "-12345678901"}, {0.00001234, "General", "1.234E-05"}, {0.875, "0.00", "0.88"},
        {0.125, "0.00", "0.13"}, {1, "0.00", "1.00"}, {1234567.891, "#,##0.00", "1,234,567.89"}, {-5, "0", "-5"},
        {0.979, "0.0%", "97.9%"}, {-1234.5, "$#,##0.00_);($#,##0.00)", "($1,234.50)"},
        {1234.5, "$#,##0.00_);($#,##0.00)", "$1,234.50 "}, {0.000123, "0.00E+00", "1.23E-04"}, {12345, "##0.0E+0", "12.3E+3"},
        {1.5, "0.0##", "1.5"}, {1.5, "0.0??", "1.5  "}, {0, "#,##0;-#,##0;\"zero\"", "zero"}, {1234567, "#,##0,\"k\"", "1,235k"},
        {12, "[$\xE2\x82\xAC-407] 0", "\xE2\x82\xAC 12"}, {12, "General\" kg\"", "12 kg"}, {12, "@", "12"},
        {46037, "yyyy\\-mm\\-dd", "2026-01-15"}, {46037.75, "dddd, mmmm d, yyyy h:mm AM/PM", "Thursday, January 15, 2026 6:00 PM"},
        {46037, "d-mmm-yy", "15-Jan-26"}, {0.5, "h:mm:ss", "12:00:00"}, {1.5, "[h]:mm", "36:00"}, {60, "yyyy-mm-dd", "1900-02-29"},
        {59, "yyyy-mm-dd", "1900-02-28"}, {61, "yyyy-mm-dd", "1900-03-01"}, {0.000694444, "mm:ss.0", "01:00.0"}};
    for (auto& n : nums)
        if (std::string got = FormatNumber(n.v, n.code); got != n.want) return L"number format " + Wide(n.code) + L": " + Wide(got);
    if (FormatNumber(0, "yyyy-mm-dd", true) != "1904-01-01") return L"1904 dates";
    // Malformed input: no crash, no unbounded work.
    if (FormatNumber(NAN, "dddd") != "########") return L"NaN date";
    if (XlColor(R"(indexed="-2000000000")", {}) != 0) return L"negative indexed colour";
    if (DocxToMd("<w:p><w:r><w:t>x</w:t></w:r></w:p>", R"(<w:style w:styleId="a"><w:name/></w:style>)") != "x\n\n") return L"docx: style without a name";
    std::string deep;
    for (int k = 0; k < 5000; k++) deep += "<a>";
    Node root = Parse(deep);
    size_t depth = 0;
    for (const Node* n = &root; !n->kids.empty(); n = &n->kids[0]) depth++;
    if (depth != kMaxDepth) return L"xml depth cap: " + std::to_wstring(depth);
    if (FontFace f = ResolveFont(L"Calibri Light"); f.family != L"Calibri" || f.weight != DWRITE_FONT_WEIGHT_LIGHT) return L"font: " + f.family;
    if (FontFace f = ResolveFont(L"Segoe UI Semibold"); f.family != L"Segoe UI" || f.weight != DWRITE_FONT_WEIGHT_SEMI_BOLD) return L"font: " + f.family;
    if (FontFace f = ResolveFont(L"No Such Font 123"); f.family != L"No Such Font 123" || f.weight != DWRITE_FONT_WEIGHT_NORMAL) return L"font: unknown";
    return L"";
}
