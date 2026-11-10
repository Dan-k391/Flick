// Extra info-card lines for programs and installers: version resource and PE header (exe, dll,
// sys, ...) or MSI summary information. Reads headers only (~1 ms).
#include "app.h"
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

}  // namespace

std::vector<std::wstring> ProgramInfo(const std::wstring& path) {
    std::vector<std::wstring> out;
    std::wstring ext = path.substr(path.find_last_of(L'.') + 1);
    for (auto& c : ext) c = towlower(c);
    if (ext == L"msi" || ext == L"msp") MsiInfo(path, out);
    else if (std::wstring(L" exe dll sys ocx cpl scr drv efi mui ax com ").find(L" " + ext + L" ") != std::wstring::npos) PeInfo(path, out);
    return out;
}
