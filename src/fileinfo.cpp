// Extra info-card lines for programs and installers: version resource and PE header (exe, dll,
// sys, ...), MSI summary information, ELF/Mach-O headers, certificates. Reads headers only (~1 ms).
#include "app.h"
#include <wincrypt.h>
#include <msi.h>
#include <msiquery.h>

namespace {

std::wstring VersionString(const void* block, const wchar_t* sub) {
    wchar_t* v = nullptr;
    UINT n = 0;
    return VerQueryValueW(block, sub, (void**)&v, &n) && v && n > 1 ? std::wstring(v) : L"";
}

void PeInfo(const std::wstring& path, std::vector<std::wstring>& out) {
    DWORD handle = 0, size = GetFileVersionInfoSizeExW(FILE_VER_GET_NEUTRAL, path.c_str(), &handle);
    std::string block(size, '\0');
    if (size && GetFileVersionInfoExW(FILE_VER_GET_NEUTRAL, path.c_str(), 0, size, block.data())) {
        struct { WORD lang, cp; }* tr = nullptr;
        UINT n = 0;
        wchar_t prefix[64] = L"\\StringFileInfo\\040904b0\\";
        if (VerQueryValueW(block.data(), L"\\VarFileInfo\\Translation", (void**)&tr, &n) && n >= 4)
            swprintf_s(prefix, L"\\StringFileInfo\\%04x%04x\\", tr->lang, tr->cp);
        auto str = [&](const wchar_t* key) { return VersionString(block.data(), (std::wstring(prefix) + key).c_str()); };
        std::wstring desc = str(L"FileDescription"), product = str(L"ProductName"), company = str(L"CompanyName");
        std::wstring version = str(L"ProductVersion");
        if (version.empty()) version = str(L"FileVersion");
        if (version.empty()) {
            VS_FIXEDFILEINFO* fi = nullptr;
            if (VerQueryValueW(block.data(), L"\\", (void**)&fi, &n) && fi)
                version = std::to_wstring(HIWORD(fi->dwFileVersionMS)) + L"." + std::to_wstring(LOWORD(fi->dwFileVersionMS)) +
                          L"." + std::to_wstring(HIWORD(fi->dwFileVersionLS)) + L"." + std::to_wstring(LOWORD(fi->dwFileVersionLS));
        }
        if (!desc.empty()) out.push_back(desc);
        if (!product.empty() && product != desc) out.push_back(product);
        if (!company.empty()) out.push_back(company);
        if (!version.empty()) out.push_back(L"Version " + version);
    }
    // Architecture and kind from the PE header.
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    BYTE buf[4096];
    DWORD read = 0;
    ReadFile(f, buf, sizeof buf, &read, nullptr);
    CloseHandle(f);
    auto* dos = (IMAGE_DOS_HEADER*)buf;
    if (read < sizeof *dos || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        (DWORD)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > read)
        return;
    auto* nt = (IMAGE_NT_HEADERS64*)(buf + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    const wchar_t* arch = L"";
    switch (nt->FileHeader.Machine) {
        case IMAGE_FILE_MACHINE_AMD64: arch = L"x64"; break;
        case IMAGE_FILE_MACHINE_I386: arch = L"x86"; break;
        case IMAGE_FILE_MACHINE_ARM64: arch = L"ARM64"; break;
        case IMAGE_FILE_MACHINE_ARMNT: arch = L"ARM"; break;
        case 0xA641: arch = L"ARM64EC"; break;
    }
    bool pe32 = nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    WORD subsystem = pe32 ? ((IMAGE_NT_HEADERS32*)nt)->OptionalHeader.Subsystem : nt->OptionalHeader.Subsystem;
    DWORD clr = pe32 ? ((IMAGE_NT_HEADERS32*)nt)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR].VirtualAddress
                     : nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR].VirtualAddress;
    const wchar_t* kind = nt->FileHeader.Characteristics & IMAGE_FILE_DLL ? L"library"
                          : subsystem == IMAGE_SUBSYSTEM_WINDOWS_GUI   ? L"desktop app"
                          : subsystem == IMAGE_SUBSYSTEM_WINDOWS_CUI   ? L"console app"
                          : subsystem == IMAGE_SUBSYSTEM_NATIVE        ? L"driver"
                          : subsystem >= IMAGE_SUBSYSTEM_EFI_APPLICATION && subsystem <= IMAGE_SUBSYSTEM_EFI_ROM ? L"EFI image"
                                                                                                               : L"program";
    std::wstring line = std::wstring(arch) + (*arch ? L" " : L"") + kind + (clr ? L" · .NET" : L"");
    out.push_back(line);
}

void MsiInfo(const std::wstring& path, std::vector<std::wstring>& out) {
    MSIHANDLE h = 0;
    if (MsiGetSummaryInformationW(0, path.c_str(), 0, &h) != ERROR_SUCCESS) return;
    auto prop = [&](UINT id) {
        UINT type = 0;
        INT i = 0;
        FILETIME ft;
        wchar_t buf[512];
        DWORD n = 512;
        return MsiSummaryInfoGetPropertyW(h, id, &type, &i, &ft, buf, &n) == ERROR_SUCCESS && type == VT_LPSTR
                   ? std::wstring(buf) : L"";
    };
    std::wstring subject = prop(3 /* PID_SUBJECT */), author = prop(4 /* PID_AUTHOR */), platform = prop(7 /* PID_TEMPLATE */);
    MsiCloseHandle(h);
    if (!subject.empty()) out.push_back(subject);
    if (!author.empty()) out.push_back(author);
    if (size_t semi = platform.find(L';'); semi != std::wstring::npos) platform.resize(semi);
    if (!platform.empty()) out.push_back(L"Installer for " + (platform == L"Intel" ? L"x86" : platform));
}

std::wstring ShortDate(const FILETIME& ft) {
    SYSTEMTIME utc, local;
    FileTimeToSystemTime(&ft, &utc);
    SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
    wchar_t d[64] = L"";
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &local, nullptr, d, 64, nullptr);
    return d;
}

// Certificates (DER, PEM, PKCS #7): the first one's subject, issuer, validity and thumbprint.
void CertInfo(const std::wstring& path, const std::wstring& ext, std::vector<std::wstring>& out) {
    if (ext == L"pfx" || ext == L"p12") {
        // shortcut: not opened: it needs the password, and even an empty one costs key-derivation rounds the file
        // chooses (unbounded, on the UI thread); PFXImportCertStore + PKCS12_NO_PERSIST_KEY on a worker if wanted.
        out.push_back(L"Password-protected certificate store");
        return;
    }
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    std::string buf(1 << 20, '\0');
    DWORD read = 0;
    ReadFile(f, buf.data(), (DWORD)buf.size(), &read, nullptr);
    CloseHandle(f);
    buf.resize(read);
    if (buf.find("PRIVATE KEY") != buf.npos) return;  // a key (maybe with its certificate): shown as text, as always
    // Not CryptQueryObject: trying every format it took ~5 ms. PEM -> DER (the first block), then a certificate or PKCS #7.
    std::string der;
    DWORD n = 0;
    if (buf.find("-----BEGIN") != buf.npos &&
        CryptStringToBinaryA(buf.data(), read, CRYPT_STRING_BASE64HEADER, nullptr, &n, nullptr, nullptr)) {
        der.resize(n);
        CryptStringToBinaryA(buf.data(), read, CRYPT_STRING_BASE64HEADER, (BYTE*)der.data(), &n, nullptr, nullptr);
        der.resize(n);
    } else der = buf;
    PCCERT_CONTEXT cert = CertCreateCertificateContext(X509_ASN_ENCODING, (const BYTE*)der.data(), (DWORD)der.size());
    HCERTSTORE store = nullptr;
    size_t count = cert ? 1 : 0;
    CRYPT_DATA_BLOB blob{(DWORD)der.size(), (BYTE*)der.data()};
    if (!cert && (store = CertOpenStore(CERT_STORE_PROV_PKCS7, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, 0, &blob)))
        for (PCCERT_CONTEXT c = nullptr; (c = CertEnumCertificatesInStore(store, c));)
            if (!count++) cert = CertDuplicateCertificateContext(c);
    size_t pem = 0;  // a PEM bundle: its first one is shown
    for (size_t k = 0; (k = buf.find("-----BEGIN CERTIFICATE-----", k)) != buf.npos; k++) pem++;
    count = std::max(count, pem);
    if (cert) {
        auto name = [&](DWORD flags) {
            wchar_t n[512] = L"";
            CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, flags, nullptr, n, 512);
            return std::wstring(n);
        };
        std::wstring subject = name(0), issuer = name(CERT_NAME_ISSUER_FLAG);
        if (!subject.empty()) out.push_back(subject);
        out.push_back(issuer == subject ? L"Self-signed" : L"Issued by " + issuer);
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        const CERT_INFO* ci = cert->pCertInfo;
        out.push_back(L"Valid " + ShortDate(ci->NotBefore) + L" – " + ShortDate(ci->NotAfter) +
                      (CompareFileTime(&now, &ci->NotAfter) > 0    ? L" · Expired"
                       : CompareFileTime(&now, &ci->NotBefore) < 0 ? L" · Not yet valid" : L""));
        BYTE sha1[20];
        DWORD n = sizeof sha1;
        if (CertGetCertificateContextProperty(cert, CERT_SHA1_HASH_PROP_ID, sha1, &n)) {
            std::wstring hex = L"SHA-1 ";
            for (DWORD i = 0; i < n; i++) hex += L"0123456789ABCDEF"[sha1[i] >> 4], hex += L"0123456789ABCDEF"[sha1[i] & 15];
            out.push_back(hex);
        }
        if (count > 1) out.push_back(std::to_wstring(count) + L" certificates");
        CertFreeCertificateContext(cert);
    }
    if (store) CertCloseStore(store, 0);
}

// ELF and Mach-O (thin or universal): word size, CPU, kind, OS ABI.
// b: the file's first n bytes (up to 4 KB).
void NativeLines(const BYTE* b, DWORD n, std::vector<std::wstring>& out) {
    if (n < 8) return;
    if (!memcmp(b, "\x7F" "ELF", 4)) {
        bool is64 = b[4] == 2, be = b[5] == 2;
        if (n < (is64 ? 64u : 52u)) return;
        auto u16 = [&](size_t i) -> UINT { return be ? b[i] << 8 | b[i + 1] : b[i] | b[i + 1] << 8; };
        auto u32 = [&](size_t i) -> UINT64 { return be ? (UINT64)u16(i) << 16 | u16(i + 2) : u16(i) | (UINT64)u16(i + 2) << 16; };
        auto u64 = [&](size_t i) { return be ? u32(i) << 32 | u32(i + 4) : u32(i) | u32(i + 4) << 32; };
        UINT type = u16(16), machine = u16(18);
        static const std::pair<UINT, const wchar_t*> cpus[] = {
            {3, L"x86"}, {62, L"x64"}, {40, L"ARM"}, {183, L"ARM64"}, {243, L"RISC-V"}, {8, L"MIPS"}, {10, L"MIPS"},
            {20, L"PowerPC"}, {21, L"PowerPC 64"}, {2, L"SPARC"}, {43, L"SPARC 64"}, {22, L"S/390"}, {50, L"IA-64"},
            {258, L"LoongArch"}, {247, L"BPF"}, {93, L"ARC"}, {105, L"MSP430"}, {83, L"AVR"}, {94, L"Xtensa"}};
        std::wstring line = is64 ? L"ELF 64-bit" : L"ELF 32-bit";
        bool known = false;
        for (auto [m, name] : cpus)
            if (m == machine && !known) line += L" " + std::wstring(name), known = true;
        if (!known) line += L" machine " + std::to_wstring(machine);
        const wchar_t* kind = type == 1 ? L"object file" : type == 2 ? L"executable" : type == 4 ? L"core dump" : L"";
        if (type == 3) {  // a shared object with an interpreter is a position-independent executable
            UINT64 ph = is64 ? u64(32) : u32(28);
            UINT size = u16(is64 ? 54 : 42), count = u16(is64 ? 56 : 44);
            kind = L"shared library";
            for (UINT i = 0; i < count && size >= 4 && ph <= n && i < (n - ph) / size; i++)  // whole entries in b
                if (u32((size_t)(ph + (UINT64)i * size)) == 3 /* PT_INTERP */) kind = L"executable (PIE)";
        }
        if (*kind) line += L" " + std::wstring(kind);
        if (be) line += L" · big-endian";
        static const std::pair<UINT, const wchar_t*> abis[] = {
            {1, L"HP-UX"}, {2, L"NetBSD"}, {3, L"Linux"}, {6, L"Solaris"}, {7, L"AIX"}, {9, L"FreeBSD"}, {12, L"OpenBSD"},
            {64, L"ARM EABI"}, {97, L"ARM"}, {255, L"standalone"}};
        for (auto [a, name] : abis)
            if (a == b[7]) line += L" · " + std::wstring(name);
        out.push_back(line);
        return;
    }
    auto be32 = [&](size_t i) { return (UINT32)b[i] << 24 | b[i + 1] << 16 | b[i + 2] << 8 | b[i + 3]; };
    auto cpu = [](UINT32 t, UINT32 sub) -> std::wstring {
        switch (t) {
            case 7: return L"x86";
            case 0x01000007: return L"x64";
            case 12: return L"ARM";
            case 0x0100000C: return (sub & 0xFF) == 2 ? L"ARM64e" : L"ARM64";
            case 0x0200000C: return L"ARM64_32";
            case 18: return L"PowerPC";
            case 0x01000012: return L"PowerPC 64";
        }
        return L"CPU " + std::to_wstring(t);
    };
    UINT32 magic = be32(0);
    if ((magic == 0xCAFEBABE || magic == 0xCAFEBABF) && be32(4) >= 1 && be32(4) <= 20) {  // more: a Java class file
        UINT32 entry = magic == 0xCAFEBABF ? 32 : 20;
        std::wstring line = L"Universal binary:";
        for (UINT32 i = 0, count = be32(4); i < count && 8 + (i + 1) * entry <= n; i++)
            line += (i ? L", " : L" ") + cpu(be32(8 + i * entry), be32(12 + i * entry));
        out.push_back(line);
        return;
    }
    bool swap = magic == 0xCEFAEDFE || magic == 0xCFFAEDFE;  // a little-endian file (all current ones)
    if (n < 16 || (!swap && magic != 0xFEEDFACE && magic != 0xFEEDFACF)) return;
    auto rd = [&](size_t i) { return swap ? _byteswap_ulong(be32(i)) : be32(i); };
    UINT32 type = rd(12);
    static const wchar_t* kinds[] = {L"", L"object file", L"executable", L"", L"core dump", L"", L"dynamic library",
                                     L"dynamic linker", L"bundle", L"library stub", L"debug symbols", L"kernel extension"};
    out.push_back(std::wstring(magic == 0xFEEDFACF || magic == 0xCFFAEDFE ? L"Mach-O 64-bit " : L"Mach-O 32-bit ") +
                  cpu(rd(4), rd(8)) + (type < 12 && *kinds[type] ? L" " + std::wstring(kinds[type]) : L""));
}

void NativeInfo(const std::wstring& path, std::vector<std::wstring>& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    BYTE b[4096];
    DWORD n = 0;
    ReadFile(f, b, sizeof b, &n, nullptr);
    CloseHandle(f);
    NativeLines(b, n, out);
}

}  // namespace

std::wstring FileInfoSelfTest() {
    // ELF64 shared object: program headers at 2^64 - 0xFFFF, 0xFFFF bytes each (an offset check that wraps reads
    // below the buffer); then the same with one PT_INTERP entry in range: a PIE.
    BYTE b[128] = {0x7F, 'E', 'L', 'F', 2, 1, 1};
    b[16] = 3, b[18] = 62;
    memset(b + 32, 0xFF, 8), b[32] = 1, b[54] = b[55] = 0xFF, b[56] = 1;
    std::vector<std::wstring> out;
    NativeLines(b, sizeof b, out);
    if (out.size() != 1 || out[0] != L"ELF 64-bit x64 shared library") return L"elf wrap";
    memset(b + 32, 0, 8), b[32] = 64, b[54] = 56, b[55] = 0, b[64] = 3;
    out.clear();
    NativeLines(b, sizeof b, out);
    if (out.size() != 1 || out[0] != L"ELF 64-bit x64 executable (PIE)") return L"elf pie";
    return L"";
}

std::vector<std::wstring> ProgramInfo(const std::wstring& path, bool sniff) {
    std::vector<std::wstring> out;
    std::wstring ext = path.substr(path.find_last_of(L'.') + 1);
    for (auto& c : ext) c = towlower(c);
    if (ext == L"msi" || ext == L"msp") MsiInfo(path, out);
    else if (std::wstring(L" exe dll sys ocx cpl scr drv efi mui ax com ").find(L" " + ext + L" ") != std::wstring::npos) PeInfo(path, out);
    else if (std::wstring(L" cer crt der pem p7b p7c p7s pfx p12 ").find(L" " + ext + L" ") != std::wstring::npos) CertInfo(path, ext, out);
    else if (sniff) NativeInfo(path, out);
    return out;
}
