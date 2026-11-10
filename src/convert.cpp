// Jupyter notebooks, AsciiDoc and reStructuredText are converted to Markdown text and shown by the
// Markdown view (coloured code, images, tables). The markup converters cover the common subset.
#include "app.h"
#include <algorithm>
#include <cwctype>

namespace {

// ---- minimal JSON (notebooks)
struct Json {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    std::wstring str;
    std::vector<Json> arr;
    std::vector<std::pair<std::wstring, Json>> obj;
    const Json& operator[](const wchar_t* key) const {
        static const Json none;
        for (auto& [k, v] : obj)
            if (k == key) return v;
        return none;
    }
    std::wstring Text() const {  // a string, or an array of strings joined (notebook "source")
        if (type == Str) return str;
        std::wstring s;
        for (auto& v : arr) s += v.str;
        return s;
    }
};

struct JsonParser {
    const std::wstring& s;
    size_t i = 0;
    int depth = 0;
    void Ws() {
        while (i < s.size() && iswspace(s[i])) i++;
    }
    bool Parse(Json& out) {
        Ws();
        if (i >= s.size() || ++depth > 200) return false;  // nesting limit: untrusted input
        bool ok = true;
        wchar_t c = s[i];
        if (c == L'{') {
            out.type = Json::Obj, i++;
            for (Ws(); ok && i < s.size() && s[i] != L'}';) {
                Json key;
                ok = Parse(key) && key.type == Json::Str;
                Ws();
                if (!ok || i >= s.size() || s[i++] != L':') return false;
                out.obj.emplace_back(std::move(key.str), Json{});
                ok = Parse(out.obj.back().second);
                Ws();
                if (i < s.size() && s[i] == L',') i++, Ws();
            }
            i++;
        } else if (c == L'[') {
            out.type = Json::Arr, i++;
            for (Ws(); ok && i < s.size() && s[i] != L']';) {
                ok = Parse(out.arr.emplace_back());
                Ws();
                if (i < s.size() && s[i] == L',') i++, Ws();
            }
            i++;
        } else if (c == L'"') {
            out.type = Json::Str, i++;
            while (i < s.size() && s[i] != L'"') {
                if (s[i] != L'\\') {
                    out.str += s[i++];
                    continue;
                }
                if (++i >= s.size()) return false;
                switch (wchar_t e = s[i++]) {
                    case L'n': out.str += L'\n'; break;
                    case L't': out.str += L'\t'; break;
                    case L'r': out.str += L'\r'; break;
                    case L'b': out.str += L'\b'; break;
                    case L'f': out.str += L'\f'; break;
                    case L'u':
                        if (i + 4 > s.size()) return false;
                        out.str += (wchar_t)wcstoul(s.substr(i, 4).c_str(), nullptr, 16), i += 4;
                        break;
                    default: out.str += e;
                }
            }
            i++;
        } else {  // number, true, false, null: kept as text
            size_t start = i;
            while (i < s.size() && !wcschr(L",]} \t\r\n", s[i])) i++;
            out.str = s.substr(start, i - start);
            out.type = out.str == L"null" ? Json::Null : out.str == L"true" || out.str == L"false" ? Json::Bool : Json::Num;
            ok = i > start;
        }
        depth--;
        return ok;
    }
};

std::wstring Fence(const std::wstring& body, const std::wstring& lang = L"") {
    size_t first = 0;
    while (first < body.size() && (body[first] == L'\n' || body[first] == L'\r')) first++;
    std::wstring b = body.substr(first);
    while (!b.empty() && (b.back() == L'\n' || b.back() == L'\r')) b.pop_back();
    return L"```" + lang + L"\n" + b + L"\n```\n\n";
}

std::vector<std::wstring> Lines(const std::wstring& text) {
    std::vector<std::wstring> lines;
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); i++)
        if (i == text.size() || text[i] == L'\n') {
            std::wstring l = text.substr(start, i - start);
            if (!l.empty() && l.back() == L'\r') l.pop_back();
            lines.push_back(std::move(l));
            start = i + 1;
        }
    return lines;
}

std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t"), b = s.find_last_not_of(L" \t");
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}

size_t Indent(const std::wstring& s) { return std::min(s.find_first_not_of(L" \t"), s.size()); }

bool StartsWith(const std::wstring& s, const wchar_t* p) { return s.rfind(p, 0) == 0; }

// Replaces each `open…close` pair with `newOpen…newClose` (inline markup).
std::wstring Swap(const std::wstring& s, const std::wstring& open, const std::wstring& close, const std::wstring& newOpen,
                  const std::wstring& newClose) {
    std::wstring out;
    for (size_t i = 0; i < s.size();) {
        size_t a = s.find(open, i);
        size_t b = a == std::wstring::npos ? a : s.find(close, a + open.size());
        if (b == std::wstring::npos) {
            out += s.substr(i);
            break;
        }
        if (b == a + open.size()) {  // empty pair (e.g. "**"): keep as is
            out += s.substr(i, b + close.size() - i);
            i = b + close.size();
            continue;
        }
        out += s.substr(i, a - i) + newOpen + s.substr(a + open.size(), b - a - open.size()) + newClose;
        i = b + close.size();
    }
    return out;
}

}  // namespace

std::wstring NotebookToMarkdown(const std::wstring& json) {
    Json nb;
    JsonParser p{json};
    if (!p.Parse(nb) || nb.type != Json::Obj) return L"";
    std::wstring lang = nb[L"metadata"][L"kernelspec"][L"language"].str;
    if (lang.empty()) lang = nb[L"metadata"][L"language_info"][L"name"].str;
    if (lang.empty()) lang = L"python";
    std::wstring md;
    for (auto& cell : nb[L"cells"].arr) {
        std::wstring type = cell[L"cell_type"].str, src = cell[L"source"].Text();
        if (type == L"markdown") {
            md += src + L"\n\n";
            continue;
        }
        if (type != L"code") {
            md += Fence(src);
            continue;
        }
        md += Fence(src, lang);
        for (auto& out : cell[L"outputs"].arr) {
            const Json& data = out[L"data"];
            if (out[L"output_type"].str == L"stream") {
                md += Fence(out[L"text"].Text());
            } else if (out[L"output_type"].str == L"error") {
                md += Fence(out[L"ename"].str + L": " + out[L"evalue"].str);
            } else if (std::wstring img = data[L"image/png"].Text(); !img.empty() || !data[L"image/jpeg"].Text().empty()) {
                bool png = !img.empty();
                if (!png) img = data[L"image/jpeg"].Text();
                std::erase_if(img, [](wchar_t c) { return iswspace(c); });
                md += std::wstring(L"![output](data:image/") + (png ? L"png" : L"jpeg") + L";base64," + img + L")\n\n";
            } else if (std::wstring m = data[L"text/markdown"].Text(); !m.empty()) {
                md += m + L"\n\n";
            } else if (std::wstring t = data[L"text/plain"].Text(); !t.empty()) {
                md += Fence(t);
            }
        }
    }
    return md;
}

std::wstring AsciiDocToMarkdown(const std::wstring& text) {
    std::wstring md, lang, fence;  // fence: delimiter of the open listing block
    std::vector<std::vector<std::wstring>> table;
    bool inTable = false;
    for (auto& raw : Lines(text)) {
        std::wstring line = raw, t = Trim(line);
        if (!fence.empty()) {
            if (t == fence) md += L"```\n\n", fence.clear();
            else md += line + L"\n";
            continue;
        }
        if (t == L"|===") {
            if (inTable && !table.empty()) {  // header = first row
                size_t cols = table[0].size();
                std::vector<std::wstring> cells;
                for (auto& r : table) cells.insert(cells.end(), r.begin(), r.end());
                for (size_t i = 0; i < cells.size(); i++) {
                    md += L"| " + cells[i] + L" ";
                    if ((i + 1) % cols == 0) {
                        md += L"|\n";
                        if (i + 1 == cols) {
                            for (size_t c = 0; c < cols; c++) md += L"|---";
                            md += L"|\n";
                        }
                    }
                }
                md += L"\n";
            }
            inTable = !inTable, table.clear();
            continue;
        }
        if (inTable) {
            if (t.empty() || t[0] != L'|') continue;
            std::vector<std::wstring> row;
            for (size_t a = 1, b; a <= t.size(); a = b + 1) {
                b = std::min(t.find(L'|', a), t.size());
                row.push_back(Trim(t.substr(a, b - a)));
            }
            table.push_back(row);
            continue;
        }
        if (t.size() >= 4 && (t.find_first_not_of(L'-') == std::wstring::npos || t.find_first_not_of(L'.') == std::wstring::npos)) {
            md += L"```" + lang + L"\n", fence = t, lang.clear();
            continue;
        }
        if (t.size() >= 4 && (t.find_first_not_of(L'*') == std::wstring::npos || t.find_first_not_of(L'=') == std::wstring::npos ||
                              t.find_first_not_of(L'_') == std::wstring::npos))
            continue;  // sidebar/example/quote delimiters
        if (StartsWith(t, L"[source")) {  // [source,python]
            size_t c = t.find(L',');
            lang = c == std::wstring::npos ? L"" : Trim(t.substr(c + 1, t.find_first_of(L",]", c + 1) - c - 1));
            continue;
        }
        if ((t.size() > 1 && t[0] == L'[' && t.back() == L']') || StartsWith(t, L"//") ||
            (t.size() > 2 && t[0] == L':' && t.find(L':', 1) != std::wstring::npos))
            continue;  // attributes, comments, block attributes
        if (t == L"'''") {
            md += L"---\n\n";
            continue;
        }
        if (size_t n = t.find_first_not_of(L'='); n && n <= 6 && n < t.size() && t[n] == L' ') {
            md += std::wstring(n, L'#') + t.substr(n) + L"\n\n";
            continue;
        }
        if (StartsWith(t, L"image::")) {
            size_t b = t.find(L'[');
            std::wstring path = t.substr(7, b - 7), alt = b == std::wstring::npos ? L"" : t.substr(b + 1, t.size() - b - 2);
            md += L"![" + alt + L"](" + path + L")\n\n";
            continue;
        }
        for (auto [tag, label] : {std::pair{L"NOTE: ", L"Note"}, {L"TIP: ", L"Tip"}, {L"IMPORTANT: ", L"Important"},
                                  {L"WARNING: ", L"Warning"}, {L"CAUTION: ", L"Caution"}})
            if (StartsWith(t, tag)) t = std::wstring(L"> **") + label + L":** " + t.substr(wcslen(tag));
        if (size_t n = t.find_first_not_of(L'*'); n && n < t.size() && t[n] == L' ') t = std::wstring((n - 1) * 2, L' ') + L"-" + t.substr(n);
        else if (size_t m = t.find_first_not_of(L'.'); m && m < t.size() && t[m] == L' ') t = std::wstring((m - 1) * 3, L' ') + L"1." + t.substr(m);
        else if (Indent(line) && !t.empty()) t = line;  // keep indentation (literal paragraphs)
        // inline: *bold* -> **bold**, links url[text] -> [text](url)
        t = Swap(t, L"*", L"*", L"**", L"**");
        for (size_t at = 0; (at = t.find(L"://", at)) != std::wstring::npos;) {
            size_t start = t.find_last_of(L" (", at), b = t.find(L'[', at), sp = t.find(L' ', at);
            start = start == std::wstring::npos ? 0 : start + 1;
            if (b == std::wstring::npos || (sp != std::wstring::npos && sp < b)) {
                at += 3;
                continue;
            }
            size_t e = t.find(L']', b);
            if (e == std::wstring::npos) break;
            std::wstring url = t.substr(start, b - start), label = t.substr(b + 1, e - b - 1);
            if (StartsWith(url, L"link:")) url.erase(0, 5);
            std::wstring repl = L"[" + (label.empty() ? url : label) + L"](" + url + L")";
            t.replace(start, e + 1 - start, repl);
            at = start + repl.size();
        }
        md += t + L"\n";
    }
    if (!fence.empty()) md += L"```\n";
    return md;
}

std::wstring RstToMarkdown(const std::wstring& text) {
    std::vector<std::wstring> lines = Lines(text);
    std::wstring md, levels;  // underline characters in order of first use = heading levels
    auto isRule = [](const std::wstring& s) {
        return s.size() >= 2 && wcschr(L"=-~^\"'`#*+:._", s[0]) && s.find_first_not_of(s[0]) == std::wstring::npos;
    };
    for (size_t i = 0; i < lines.size(); i++) {
        std::wstring line = lines[i], t = Trim(line);
        // Heading: text underlined (optionally also overlined) by a rule at least as long.
        size_t at = i;
        if (isRule(t) && i + 2 < lines.size() && isRule(Trim(lines[i + 2])) && !Trim(lines[i + 1]).empty()) at = i + 1;
        if (!Trim(lines[at]).empty() && !isRule(Trim(lines[at])) && at + 1 < lines.size()) {
            std::wstring under = Trim(lines[at + 1]);
            if (isRule(under) && under.size() >= Trim(lines[at]).size() && !Indent(lines[at])) {
                wchar_t key = at > i ? (wchar_t)(under[0] + 0x100) : under[0];  // overlined is its own level
                size_t level = levels.find(key);
                if (level == std::wstring::npos) level = levels.size(), levels += key;
                md += std::wstring(std::min<size_t>(level + 1, 6), L'#') + L" " + Trim(lines[at]) + L"\n\n";
                i = at + 1;
                continue;
            }
        }
        // Directives and literal blocks: the indented lines that follow.
        bool literal = !t.empty() && t.size() >= 2 && t.compare(t.size() - 2, 2, L"::") == 0 && !StartsWith(t, L"..");
        std::wstring directive, arg;
        if (StartsWith(t, L".. ") && t.find(L"::") != std::wstring::npos) {
            size_t c = t.find(L"::");
            directive = Trim(t.substr(3, c - 3)), arg = Trim(t.substr(c + 2));
        }
        if (literal || !directive.empty() || StartsWith(t, L"..")) {
            size_t base = Indent(line), j = i + 1, inner = std::wstring::npos;
            std::wstring body;
            for (; j < lines.size(); j++) {
                if (Trim(lines[j]).empty()) {
                    body += L"\n";
                    continue;
                }
                if (Indent(lines[j]) <= base) break;
                if (Trim(lines[j])[0] == L':' && body.find_first_not_of(L'\n') == std::wstring::npos) continue;  // options
                inner = std::min(inner, Indent(lines[j]));
                body += lines[j] + L"\n";
            }
            std::wstring dedented;
            for (auto& l : Lines(body)) dedented += (l.size() >= inner && inner != std::wstring::npos ? l.substr(inner) : Trim(l)) + L"\n";
            if (literal) {
                std::wstring para = t.substr(0, t.size() - (t.size() > 2 && t[t.size() - 3] == L' ' ? 3 : 1));
                if (!Trim(para).empty()) md += para + L"\n\n";
                md += Fence(dedented);
            } else if (directive == L"code-block" || directive == L"code" || directive == L"sourcecode") {
                md += Fence(dedented, arg);
            } else if (directive == L"image" || directive == L"figure") {
                md += L"![](" + arg + L")\n\n";
            } else if (directive == L"note" || directive == L"warning" || directive == L"tip" || directive == L"important" ||
                       directive == L"attention" || directive == L"caution" || directive == L"danger" || directive == L"hint") {
                std::wstring label = directive;
                label[0] = towupper(label[0]);
                md += L"> **" + label + L":** " + Trim(arg) + L"\n";
                for (auto& l : Lines(dedented)) md += L"> " + l + L"\n";
                md += L"\n";
            }  // other directives and comments are dropped
            i = j - 1;
            continue;
        }
        if (!t.empty() && (t[0] == L'+' || t[0] == L'|') && t.size() > 1) {  // grid/line tables: keep alignment
            std::wstring block;
            for (; i < lines.size() && !Trim(lines[i]).empty(); i++) block += lines[i] + L"\n";
            md += Fence(block);
            continue;
        }
        if (StartsWith(t, L"#. ")) t = L"1. " + t.substr(3);
        // inline: ``code`` -> `code`, `text <url>`_ -> [text](url)
        t = Swap(t, L"``", L"``", L"`", L"`");
        for (size_t end; (end = t.find(L">`_")) != std::wstring::npos;) {
            size_t lt = t.rfind(L" <", end), a = lt == std::wstring::npos ? lt : t.rfind(L'`', lt);
            if (a == std::wstring::npos) break;
            std::wstring label = t.substr(a + 1, lt - a - 1), url = t.substr(lt + 2, end - lt - 2);
            size_t tail = end + 3 + (end + 3 < t.size() && t[end + 3] == L'_');
            t.replace(a, tail - a, L"[" + label + L"](" + url + L")");
        }
        bool item = StartsWith(t, L"- ") || StartsWith(t, L"* ") || (!t.empty() && iswdigit(t[0]) && t.find(L". ") < 4);
        md += (item ? line.substr(0, Indent(line)) : std::wstring()) + t + L"\n";  // nested lists keep their indent
    }
    return md;
}
