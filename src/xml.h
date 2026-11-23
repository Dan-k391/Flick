// Office XML helpers shared by office.cpp (text) and pages.cpp (drawn pages): a forgiving tag scanner,
// entities, UTF-8 <-> UTF-16, zip members and relationships. Internal linkage: each file gets its own copy.
#pragma once
#include "app.h"
#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>

namespace {

constexpr size_t kMaxXml = 32 << 20;  // per zip member

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

}  // namespace
