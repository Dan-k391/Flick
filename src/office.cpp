// Office files without Office. docx/pptx/xlsx and odt/odp/ods are zips of XML (libarchive from archiveint.dll
// + the small tag scanner below); doc/ppt/xls are OLE compound files (IStorage, part of Windows).
// Documents and slides become Markdown (headings, paragraphs, bullets, tables, bold/italic), sheets CSV for
// the grid. What is written, in a few ms: no page layout, fonts, pictures or charts.
#include "app.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <tuple>
#include <unordered_map>

namespace {

constexpr size_t kMaxXml = 32 << 20;           // per zip member
constexpr size_t kMaxRows = 50000, kMaxCols = 512;

// ---- XML: a forgiving pull scanner. Names are local (prefix dropped); text is raw (see AppendXml).
struct Xml {
    std::string_view s;
    size_t i = 0;
    std::string_view name, attrs, text;
    bool close = false, empty = false, cdata = false;
    enum { End, Tag, Text };
    int Next() {
        while (i < s.size()) {
            if (s[i] != '<') {
                size_t j = std::min(s.find('<', i), s.size());
                text = s.substr(i, j - i), cdata = false, i = j;
                return Text;
            }
            if (s.compare(i, 9, "<![CDATA[") == 0) {
                size_t j = std::min(s.find("]]>", i + 9), s.size());
                text = s.substr(i + 9, j - i - 9), cdata = true, i = std::min(j + 3, s.size());
                return Text;
            }
            if (s.compare(i, 4, "<!--") == 0) {
                i = std::min(s.find("-->", i), s.size() - 3) + 3;
                continue;
            }
            if (i + 1 < s.size() && (s[i + 1] == '?' || s[i + 1] == '!')) {
                i = std::min(s.find('>', i), s.size() - 1) + 1;
                continue;
            }
            size_t j = i + 1;
            for (char q = 0; j < s.size(); j++)
                if (q) q = s[j] == q ? 0 : q;
                else if (s[j] == '"' || s[j] == '\'') q = s[j];
                else if (s[j] == '>') break;
            close = i + 1 < s.size() && s[i + 1] == '/';
            size_t b = i + 1 + close, e = b;
            empty = j < s.size() && s[j - 1] == '/';
            while (e < j && !strchr(" \t\r\n/", s[e])) e++;
            name = s.substr(b, e - b);
            if (size_t c = name.find(':'); c != name.npos) name.remove_prefix(c + 1);
            attrs = s.substr(e, j - e - (empty && j > e));
            i = std::min(j + 1, s.size());
            return Tag;
        }
        return End;
    }
    // Skips the element just opened (for a non-empty start tag).
    void Skip() {
        if (empty || close) return;
        for (int depth = 1, t; depth && (t = Next());)
            if (t == Tag && !empty) depth += close ? -1 : 1;
    }
};

// An attribute by local name (any prefix).
std::string_view Attr(std::string_view a, std::string_view local) {
    for (size_t k = 0; (k = a.find(local, k)) != a.npos; k += local.size()) {
        size_t e = k + local.size();
        char before = k ? a[k - 1] : ' ';
        if ((before == ' ' || before == ':' || before == '\t' || before == '\n' || before == '\r') && e + 1 < a.size() &&
            a[e] == '=' && (a[e + 1] == '"' || a[e + 1] == '\'')) {
            size_t end = a.find(a[e + 1], e + 2);
            return end == a.npos ? std::string_view{} : a.substr(e + 2, end - e - 2);
        }
    }
    return {};
}
int AttrInt(std::string_view a, std::string_view local, int def) {
    std::string_view v = Attr(a, local);
    return v.empty() ? def : atoi(std::string(v).c_str());
}

void AppendUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) out += (char)(0xC0 | cp >> 6), out += (char)(0x80 | (cp & 63));
    else if (cp < 0x10000) out += (char)(0xE0 | cp >> 12), out += (char)(0x80 | (cp >> 6 & 63)), out += (char)(0x80 | (cp & 63));
    else out += (char)(0xF0 | cp >> 18), out += (char)(0x80 | (cp >> 12 & 63)), out += (char)(0x80 | (cp >> 6 & 63)),
         out += (char)(0x80 | (cp & 63));
}

// Text with entities decoded.
void AppendXml(std::string& out, std::string_view t) {
    for (size_t k = 0; k < t.size(); k++) {
        char c = t[k];
        size_t semi;
        if (c != '&' || (semi = t.find(';', k)) == t.npos || semi - k > 10) {
            if (c != '\r') out += c;
            continue;
        }
        std::string_view ent = t.substr(k + 1, semi - k - 1);
        if (ent == "amp") out += '&';
        else if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else if (ent.size() > 1 && ent[0] == '#')
            AppendUtf8(out, (unsigned)strtoul(std::string(ent.substr(1 + (ent[1] == 'x'))).c_str(), nullptr, ent[1] == 'x' ? 16 : 10));
        else {
            out += c;
            continue;
        }
        k = semi;
    }
}
std::string XmlText(std::string_view t) {
    std::string s;
    AppendXml(s, t);
    return s;
}

std::wstring Wide(std::string_view s) {
    std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), (int)w.size());
    return w;
}
std::string Utf8(std::wstring_view w) {
    std::string s(WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), (int)s.size(), nullptr, nullptr);
    return s;
}

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

// ---- sheets: rows of UTF-8 cells -> CSV
using Rows = std::vector<std::vector<std::string>>;
void Put(Rows& rows, size_t r, size_t c, std::string v) {
    if (r >= kMaxRows || c >= kMaxCols || v.empty()) return;
    if (rows.size() <= r) rows.resize(r + 1);
    if (rows[r].size() <= c) rows[r].resize(c + 1);
    rows[r][c] = std::move(v);
}
std::wstring ToCsv(const Rows& rows) {
    std::string s;
    for (auto& row : rows) {
        if (row.empty()) continue;  // the grid drops blank lines anyway
        for (size_t c = 0; c < row.size(); c++) {
            if (c) s += ',';
            const std::string& v = row[c];
            if (v.find_first_of(",\"\r\n") == v.npos) s += v;
            else {
                s += '"';
                for (char ch : v) s += ch, ch == '"' ? (void)(s += '"') : void();
                s += '"';
            }
        }
        s += '\n';
    }
    return Wide(s);
}

// Number formats that show dates: the built-in ids, or a code with d/m/y/h/s outside quotes and [..].
bool DateFormat(unsigned id, std::string_view code) {
    if ((id >= 14 && id <= 22) || (id >= 27 && id <= 36) || (id >= 45 && id <= 47) || (id >= 50 && id <= 58)) return true;
    bool quoted = false, bracket = false;
    for (size_t k = 0; k < code.size(); k++) {
        char c = code[k];
        if (c == '"') quoted = !quoted;
        else if (quoted) continue;
        else if (c == '[') bracket = true;
        else if (c == ']') bracket = false;
        else if (c == '\\' || c == '_' || c == '*') k++;
        else if (!bracket && strchr("dmyhsDMYHS", c)) return true;
    }
    return false;
}

// A cell number: dates as yyyy-mm-dd [hh:mm[:ss]] (serial days from 1899-12-30), others like Excel's General.
std::string Number(double v, bool date) {
    char b[40];
    if (date && v >= 0 && v < 2958466) {
        long long days = (long long)std::floor(v);
        long long secs = std::llround((v - days) * 86400);
        if (secs >= 86400) days++, secs -= 86400;
        long long z = days - 25569 + 719468;  // days since 1970-01-01, then Hinnant's civil_from_days
        long long era = (z >= 0 ? z : z - 146096) / 146097, doe = z - era * 146097;
        long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        long long mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
        long long y = yoe + era * 400 + (m <= 2);
        int n = 0;
        if (days || !secs) n = snprintf(b, sizeof b, "%04lld-%02lld-%02lld", y, m, d);
        if (secs) {
            n += snprintf(b + n, sizeof b - n, "%s%02lld:%02lld", n ? " " : "", secs / 3600, secs / 60 % 60);
            if (secs % 60) snprintf(b + n, sizeof b - n, ":%02lld", secs % 60);
        }
        return b;
    }
    snprintf(b, sizeof b, "%.15g", v);
    return b;
}

// ---- zips
using Members = std::vector<std::pair<std::wstring, std::string>>;
std::string_view Member(const Members& m, std::wstring_view name) {
    for (auto& [n, d] : m)
        if (n == name) return d;
    return {};
}
// Relationship id -> target path inside the zip (relative targets resolved against `dir`, e.g. L"ppt/").
std::unordered_map<std::string, std::wstring> Rels(std::string_view xml, const std::wstring& dir) {
    std::unordered_map<std::string, std::wstring> r;
    for (Xml x{xml}; int t = x.Next();)
        if (t == Xml::Tag && !x.close && x.name == "Relationship") {
            std::wstring target = Wide(XmlText(Attr(x.attrs, "Target")));
            r[std::string(Attr(x.attrs, "Id"))] = target.starts_with(L'/') ? target.substr(1) : dir + target;
        }
    return r;
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
                if (_strnicmp(v.data(), "heading ", 8) == 0 && v.size() > 8) level[id] = atoi(v.data() + 8);
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

// ---- pptx: one slide's shapes. Title placeholders go to `title`, body placeholders become bullets.
std::string SlideToMd(std::string_view xml, std::string& title) {
    MdOut md;
    enum { Other, Title, Body, Hidden } kind = Other;
    bool inText = false;
    for (Xml x{xml}; int t = x.Next();) {
        if (t == Xml::Text) {
            if (inText) md.Text(x.text, !x.cdata);
            continue;
        }
        std::string_view n = x.name;
        if (x.close) {
            if (n == "p") md.Close();
            else if (n == "t") inText = false;
            else if (n == "tc") md.CellEnd();  // spanned cells are there too (hMerge), empty
            else if (n == "tbl") md.TableEnd();
            else if (n == "sp") kind = Other, md.capture = nullptr;
            continue;
        }
        Para* p = md.Cur();
        if (n == "Fallback" || (kind == Hidden && !x.empty && n == "txBody")) x.Skip();
        else if (n == "sp" || n == "graphicFrame") kind = Other, md.capture = nullptr;
        else if (n == "ph") {
            std::string_view type = Attr(x.attrs, "type");
            if (type == "title" || type == "ctrTitle") kind = Title, md.capture = &title;
            else if (type == "dt" || type == "ftr" || type == "sldNum" || type == "hdr") kind = Hidden;
            else if (type.empty() || type == "body" || type == "obj") kind = Body;
        } else if (n == "p") md.Open(0, kind == Body ? 0 : -1);
        else if (n == "tbl") md.TableStart();
        else if (n == "tr") md.RowStart();
        else if (n == "tc") md.CellStart();
        else if (!p) continue;
        else if (n == "pPr" && p->list >= 0) p->list = AttrInt(x.attrs, "lvl", 0);
        else if (n == "buNone") p->list = -1;
        else if (n == "rPr") p->b = Attr(x.attrs, "b") == "1", p->i = Attr(x.attrs, "i") == "1";
        else if (n == "r" || n == "fld") p->b = p->i = false;
        else if (n == "t") inText = !x.empty;
        else if (n == "br") md.Text("\n");
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

// ---- xlsx
std::wstring XlsxToCsv(const std::wstring& path, std::wstring& sub) {
    Members m;
    auto meta = [](std::wstring_view n) -> size_t {
        return n == L"xl/workbook.xml" || n == L"xl/_rels/workbook.xml.rels" || n == L"xl/sharedStrings.xml" ||
                       n == L"xl/styles.xml"
                   ? kMaxXml : 0;
    };
    if (!ArchiveReadMembers(path, meta, m)) return L"";
    std::vector<std::pair<std::string, std::string>> sheets;  // name, relationship id
    size_t active = 0;
    for (Xml x{Member(m, L"xl/workbook.xml")}; int t = x.Next();)
        if (t == Xml::Tag && !x.close && x.name == "sheet") sheets.push_back({XmlText(Attr(x.attrs, "name")), std::string(Attr(x.attrs, "r:id"))});
        else if (t == Xml::Tag && !x.close && x.name == "workbookView") active = AttrInt(x.attrs, "activeTab", 0);
    if (sheets.empty()) return L"";
    active = std::min(active, sheets.size() - 1);
    std::wstring target = Rels(Member(m, L"xl/_rels/workbook.xml.rels"), L"xl/")[sheets[active].second];

    std::vector<std::string> strings;
    {
        bool inText = false;
        for (Xml x{Member(m, L"xl/sharedStrings.xml")}; int t = x.Next();)
            if (t == Xml::Text) {
                if (inText && !strings.empty()) AppendXml(strings.back(), x.text);
            } else if (x.name == "si" && !x.close) strings.emplace_back();
            else if (x.name == "t") inText = !x.close && !x.empty;
            else if (x.name == "rPh" && !x.close) x.Skip();  // phonetic guide
    }
    std::vector<bool> dateXf;
    {
        std::unordered_map<unsigned, std::string> codes;
        bool xfs = false;
        for (Xml x{Member(m, L"xl/styles.xml")}; int t = x.Next();) {
            if (t != Xml::Tag) continue;
            if (x.name == "cellXfs") xfs = !x.close && !x.empty;
            else if (x.close) continue;
            else if (x.name == "numFmt") codes[AttrInt(x.attrs, "numFmtId", 0)] = XmlText(Attr(x.attrs, "formatCode"));
            else if (x.name == "xf" && xfs) {
                unsigned id = AttrInt(x.attrs, "numFmtId", 0);
                dateXf.push_back(DateFormat(id, codes[id]));
            }
        }
    }
    m.clear();
    bool truncated = false;
    if (!ArchiveReadMembers(path, [&](std::wstring_view n) { return n == target ? kMaxXml : 0; }, m) || m.empty()) return L"";
    std::string_view xml = m[0].second;
    truncated = xml.size() >= kMaxXml;

    Rows rows;
    size_t r = 0, c = 0, nextRow = 0;
    std::string type, value;
    unsigned xf = 0;
    bool inValue = false;
    for (Xml x{xml}; int t = x.Next();) {
        if (t == Xml::Text) {
            if (inValue) AppendXml(value, x.text);
            continue;
        }
        std::string_view n = x.name;
        if (n == "row" && !x.close) r = AttrInt(x.attrs, "r", (int)nextRow + 1) - 1, nextRow = r + 1, c = (size_t)-1;
        else if (n == "c" && !x.close) {
            std::string_view ref = Attr(x.attrs, "r");
            size_t col = 0, k = 0;
            for (; k < ref.size() && ref[k] >= 'A' && ref[k] <= 'Z'; k++) col = col * 26 + (ref[k] - 'A' + 1);
            c = k ? col - 1 : c + 1;
            type = Attr(x.attrs, "t"), xf = AttrInt(x.attrs, "s", 0), value.clear();
            if (x.empty) continue;
        } else if (n == "v" || (n == "t" && type == "inlineStr")) inValue = !x.close && !x.empty;
        else if (n == "rPh" && !x.close) x.Skip();
        if (n != "c" || !(x.close || x.empty)) continue;
        std::string v;
        if (type == "s") {
            size_t k = (size_t)atoll(value.c_str());
            if (k < strings.size()) v = strings[k];
        } else if (type == "b") v = value == "1" ? "TRUE" : "FALSE";
        else if (type == "str" || type == "inlineStr" || type == "e") v = value;
        else if (!value.empty()) v = Number(atof(value.c_str()), xf < dateXf.size() && dateXf[xf]);
        Put(rows, r, c, std::move(v));
    }
    sub = Wide(sheets[active].first) + (sheets.size() > 1 ? L" (" + std::to_wstring(active + 1) + L" of " +
                                                               std::to_wstring(sheets.size()) + L" sheets)" : L"") +
          (truncated ? L" · first " + std::to_wstring(kMaxXml >> 20) + L" MB" : L"");
    return ToCsv(rows);
}

// ---- ods: first table, cells as displayed (text:p)
std::wstring OdsToCsv(std::string_view xml, std::wstring& sub) {
    Rows rows;
    std::vector<std::string> row;
    std::string cell;
    int tables = 0, rowRep = 1, cellRep = 1, depth = 0, tableDepth = -1;
    size_t r = 0, blank = 0;
    bool inCell = false, inPara = false;
    std::string name;
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
                for (int k = 0; k < cellRep && row.size() < kMaxCols; k++) row.push_back(cell);
                inCell = false;
            } else if (n == "table-row" && tables == 1) {
                while (!row.empty() && row.back().empty()) row.pop_back();
                if (row.empty()) blank += rowRep;  // repeated empty rows pad sheets to a million
                else {
                    r += std::min<size_t>(blank, 100), blank = 0;
                    for (int k = 0; k < rowRep && r < kMaxRows; k++, r++)
                        for (size_t c = 0; c < row.size(); c++) Put(rows, r, c, row[c]);
                }
                row.clear();
            } else if (n == "table" && depth == tableDepth) break;  // first sheet only
            continue;
        }
        if (!x.empty) depth++;
        if (n == "annotation") x.Skip(), depth -= !x.empty;
        else if (n == "table" && !tables++) name = XmlText(Attr(x.attrs, "name")), tableDepth = depth - !x.empty;
        else if (n == "table-row") rowRep = std::clamp(AttrInt(x.attrs, "number-rows-repeated", 1), 1, (int)kMaxRows);
        else if (n == "table-cell" || n == "covered-table-cell") {
            cellRep = std::clamp(AttrInt(x.attrs, "number-columns-repeated", 1), 1, (int)kMaxCols);
            cell.clear();
            inCell = !x.empty;
            if (x.empty)
                for (int k = 0; k < cellRep && row.size() < kMaxCols; k++) row.emplace_back();
        } else if (n == "p" && inCell) {
            if (!cell.empty()) cell += ' ';
            inPara = !x.empty;
        } else if (n == "s" && inPara) cell.append(std::clamp(AttrInt(x.attrs, "c", 1), 1, 100), ' ');
        else if ((n == "tab" || n == "line-break") && inPara) cell += ' ';
    }
    sub = Wide(name);
    return ToCsv(rows);
}

// ---- OLE compound files (doc, xls, ppt)
Microsoft::WRL::ComPtr<IStorage> OpenOle(const std::wstring& path) {
    Microsoft::WRL::ComPtr<IStorage> stg;
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
std::string DocToMd(IStorage* stg) {
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
    unsigned lcb = U32(table, k + 1);
    k += 5;
    size_t pieces = lcb >= 4 ? (lcb - 4) / 12 : 0;
    std::wstring text;
    for (size_t j = 0; j < pieces; j++) {
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
    MdOut md;
    md.Open();
    std::vector<bool> field;  // per open field: still in its code (shown from the separator on)
    for (wchar_t c : text) {
        bool hide = !field.empty() && field.back();
        if (c == 0x13) field.push_back(true);
        else if (c == 0x14 && !field.empty()) field.back() = false;
        else if (c == 0x15 && !field.empty()) field.pop_back();
        else if (hide) continue;
        else if (c == 0x0D || c == 0x07 || c == 0x0C) md.Close(), md.Open();
        else if (c == 0x0B) md.Text("\n");
        else if (c == 0x09) md.Text(" ");
        else if (c == 0x1E) md.Text("-");
        else if (c >= 0x20) md.Text(Utf8({&c, 1}));
    }
    md.Close();
    return md.out;
}

// .ppt: slides in the order of the document's SlideListWithText; per slide its text there (older files) and the
// text atoms in its drawing (shapes, text boxes), typed by TextHeaderAtom (title / body / other).
// Records are found through the persist directory of the newest edit (Current User -> UserEditAtom chain).
std::string PptToMd(IStorage* stg, int& count) {
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
        for (wchar_t c : w)
            if (c == L'\r') md.Close(), md.Open(0, bullets ? 0 : -1);
            else if (c == 0x0B) md.Text("\n");
            else if (c >= 0x20 || c == L'\t') md.Text(Utf8({&c, 1}));
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
        for (size_t a = k + 8; a + 8 <= listEnd; a += 8 + (size_t)U32(s, a + 4)) {
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

std::wstring XlsToCsv(IStorage* stg, std::wstring& sub) {
    std::string s;
    if (!ReadStream(stg, L"Workbook", s) || U16(s, 0) != 0x0809 || U16(s, 4) != 0x0600) return L"";  // BIFF8 only
    std::vector<std::string> sst;
    std::unordered_map<unsigned, std::string> codes;
    std::vector<unsigned> xfFormat;
    struct Sheet { size_t pos; bool work; std::string name; };
    std::vector<Sheet> sheets;
    size_t active = 0, k = 0;
    auto rec = [&](size_t at) { return std::string_view(s).substr(std::min(at + 4, s.size()), U16(s, at + 2)); };
    for (; k + 4 <= s.size() && U16(s, k) != 0x000A; k += 4 + U16(s, k + 2)) {
        unsigned type = U16(s, k);
        std::string_view d = rec(k);
        if (type == 0x00FC) {  // SST
            Segs g{{d.substr(std::min<size_t>(8, d.size()))}};
            for (size_t c = k + 4 + d.size(); c + 4 <= s.size() && U16(s, c) == 0x003C; c += 4 + U16(s, c + 2)) g.seg.push_back(rec(c));
            for (unsigned n = U32(d, 4); n-- && g.s < g.seg.size();) sst.push_back(g.String(true));
        } else if (type == 0x041E) {  // FORMAT
            Segs g{{d.substr(std::min<size_t>(2, d.size()))}};
            codes[U16(d, 0)] = g.String(true);
        } else if (type == 0x00E0) xfFormat.push_back(U16(d, 2));
        else if (type == 0x0085) {  // BOUNDSHEET
            Segs g{{d.substr(std::min<size_t>(6, d.size()))}};
            sheets.push_back({U32(d, 0), d.size() > 5 && d[5] == 0, g.String(false)});
        } else if (type == 0x003D) active = U16(d, 10);
        else if (type == 0x002F) return L"";  // FILEPASS: encrypted
    }
    if (active >= sheets.size() || !sheets[active].work)
        for (active = 0; active < sheets.size() && !sheets[active].work;) active++;
    if (active >= sheets.size()) return L"";
    auto isDate = [&](unsigned xf) {
        if (xf >= xfFormat.size()) return false;
        unsigned id = xfFormat[xf];
        auto it = codes.find(id);
        return DateFormat(id, it == codes.end() ? "" : it->second);
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
    Rows rows;
    int depth = 0;
    bool pendingString = false;
    size_t pr = 0, pc = 0;
    for (k = sheets[active].pos; k + 4 <= s.size(); k += 4 + U16(s, k + 2)) {
        unsigned type = U16(s, k);
        std::string_view d = rec(k);
        size_t r = U16(d, 0), c = U16(d, 2);
        unsigned xf = U16(d, 4);
        if (type == 0x0809) depth++;
        else if (type == 0x000A && --depth <= 0) break;
        else if (depth != 1) continue;
        else if (type == 0x00FD) {
            if (size_t i = U32(d, 6); i < sst.size()) Put(rows, r, c, sst[i]);
        } else if (type == 0x0203 && d.size() >= 14) {
            double v;
            memcpy(&v, d.data() + 6, 8);
            Put(rows, r, c, Number(v, isDate(xf)));
        } else if (type == 0x027E) Put(rows, r, c, Number(rk(U32(d, 6)), isDate(xf)));
        else if (type == 0x00BD && d.size() >= 6)  // MULRK
            for (size_t o = 4; o + 6 <= d.size() - 2; o += 6, c++) Put(rows, r, c, Number(rk(U32(d, o + 2)), isDate(U16(d, o))));
        else if (type == 0x0204) {  // LABEL
            Segs g{{d.substr(std::min<size_t>(6, d.size()))}};
            Put(rows, r, c, g.String(true));
        } else if (type == 0x0205 && d.size() >= 8) Put(rows, r, c, d[7] ? "#ERROR" : d[6] ? "TRUE" : "FALSE");
        else if (type == 0x0006 && d.size() >= 14) {  // FORMULA: the cached result
            if (U16(d, 12) != 0xFFFF) {
                double v;
                memcpy(&v, d.data() + 6, 8);
                Put(rows, r, c, Number(v, isDate(xf)));
            } else if (d[6] == 0) pendingString = true, pr = r, pc = c;
            else if (d[6] == 1) Put(rows, r, c, d[8] ? "TRUE" : "FALSE");
        } else if (type == 0x0207 && pendingString) {  // STRING after a string formula
            Segs g{{d}};
            Put(rows, pr, pc, g.String(true));
            pendingString = false;
        }
    }
    size_t work = std::count_if(sheets.begin(), sheets.end(), [](auto& h) { return h.work; }),
           nth = std::count_if(sheets.begin(), sheets.begin() + active + 1, [](auto& h) { return h.work; });
    sub = Wide(sheets[active].name) +
          (work > 1 ? L" (" + std::to_wstring(nth) + L" of " + std::to_wstring(work) + L" sheets)" : L"");
    return ToCsv(rows);
}

bool Has(const std::wstring& path, const wchar_t* list) {
    const wchar_t* e = wcsrchr(path.c_str(), L'.');
    if (!e) return false;
    std::wstring key = L" " + std::wstring(e) + L" ";
    for (auto& ch : key) ch = towlower(ch);
    return wcsstr(list, key.c_str()) != nullptr;
}

}  // namespace

std::wstring OfficeToMarkdown(const std::wstring& path, std::wstring& sub) {
    std::string md;
    int slides = -1;
    if (Has(path, L" .docx .docm .dotx .dotm ")) {
        Members m;
        ArchiveReadMembers(path, [](std::wstring_view n) { return n == L"word/document.xml" || n == L"word/styles.xml" ? kMaxXml : 0; }, m);
        md = DocxToMd(Member(m, L"word/document.xml"), Member(m, L"word/styles.xml"));
    } else if (Has(path, L" .pptx .pptm .potx .potm .ppsx .ppsm ")) {
        Members m;
        ArchiveReadMembers(path, [](std::wstring_view n) {
            return n == L"ppt/presentation.xml" || n == L"ppt/_rels/presentation.xml.rels" ||
                           (n.starts_with(L"ppt/slides/slide") && n.ends_with(L".xml"))
                       ? kMaxXml : 0;
        }, m);
        auto rels = Rels(Member(m, L"ppt/_rels/presentation.xml.rels"), L"ppt/");
        Deck deck;
        for (Xml x{Member(m, L"ppt/presentation.xml")}; int t = x.Next();)
            if (t == Xml::Tag && !x.close && x.name == "sldId") {
                std::string title, body = SlideToMd(Member(m, rels[std::string(Attr(x.attrs, "r:id"))]), title);
                deck.Add(title, body);
            }
        md = std::move(deck.out), slides = deck.n;
    } else if (Has(path, L" .odt .ott .odp .otp ")) {
        Members m;
        ArchiveReadMembers(path, [](std::wstring_view n) { return n == L"content.xml" ? kMaxXml : 0; }, m);
        if (!m.empty()) md = OdfToMd(m[0].second, Has(path, L" .odp .otp "), slides);
    } else if (auto stg = OpenOle(path)) {
        if (Has(path, L" .doc .dot ")) md = DocToMd(stg.Get());
        else if (Has(path, L" .ppt .pps .pot ")) md = PptToMd(stg.Get(), slides);
    }
    if (slides > 0) sub = std::to_wstring(slides) + (slides == 1 ? L" slide" : L" slides");
    return Wide(md);
}

std::wstring OfficeToCsv(const std::wstring& path, std::wstring& sub) {
    if (Has(path, L" .xlsx .xlsm .xltx .xltm ")) return XlsxToCsv(path, sub);
    if (Has(path, L" .ods .ots ")) {
        Members m;
        ArchiveReadMembers(path, [](std::wstring_view n) { return n == L"content.xml" ? kMaxXml : 0; }, m);
        return m.empty() ? L"" : OdsToCsv(m[0].second, sub);
    }
    auto stg = OpenOle(path);
    return stg ? XlsToCsv(stg.Get(), sub) : L"";
}

// --selftest: "" when the converters behave.
std::wstring OfficeSelfTest() {
    std::string md = DocxToMd(R"(<w:body><w:p><w:pPr><w:pStyle w:val="T1"/></w:pPr><w:r><w:t>Title</w:t></w:r></w:p>
<w:p><w:r><w:rPr><w:b/></w:rPr><w:t xml:space="preserve">bold </w:t></w:r><w:r><w:t>a*b &amp; c</w:t></w:r></w:p>
<w:p><w:pPr><w:numPr><w:ilvl w:val="1"/><w:numId w:val="2"/></w:numPr></w:pPr><w:r><w:t>item</w:t></w:r></w:p>
<w:tbl><w:tr><w:tc><w:p><w:r><w:t>x</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>y</w:t></w:r></w:p></w:tc></w:tr></w:tbl></w:body>)",
                              R"(<w:style w:styleId="T1"><w:name w:val="heading 1"/></w:style>)");
    if (md != "# Title\n\n**bold** a\\*b \\& c\n\n  - item\n\n| x | y |\n| --- | --- |\n\n") return L"docx: " + Wide(md);
    if (Number(46037, true) != "2026-01-15" || Number(0.5, true) != "12:00" || Number(0.1 + 0.2, false) != "0.3")
        return L"numbers";
    if (!DateFormat(164, "yyyy\\-mm\\-dd") || DateFormat(164, "0.00\"days\"") || DateFormat(0, "General")) return L"date formats";
    return L"";
}
