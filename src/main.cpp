// Flick: resident process. A low-level keyboard hook (own thread) watches for Space in Explorer
// and posts to the pre-created preview window, which asks Explorer for the selection over COM.
#include "app.h"
#include <shlobj.h>
#include <exdisp.h>
#include <shlguid.h>
#include <psapi.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <vector>
#include <algorithm>

using Microsoft::WRL::ComPtr;

namespace {

LONGLONG g_freq;
HANDLE g_logFile = INVALID_HANDLE_VALUE;
bool g_spaceDown;

bool ClassIs(HWND h, const wchar_t* name) {
    wchar_t cls[64];
    return h && GetClassNameW(h, cls, 64) && !wcscmp(cls, name);
}

bool IsDesktop(HWND h) { return ClassIs(h, L"Progman") || ClassIs(h, L"WorkerW"); }

// True when the foreground window is Explorer/desktop and keyboard focus is on the item view
// (not the address bar, search box or a rename edit).
HWND ExplorerWithItemFocus() {
    HWND fg = GetForegroundWindow();
    if (!ClassIs(fg, L"CabinetWClass") && !IsDesktop(fg)) return nullptr;
    GUITHREADINFO gti{sizeof gti};
    if (!GetGUIThreadInfo(GetWindowThreadProcessId(fg, nullptr), &gti)) return nullptr;
    return ClassIs(gti.hwndFocus, L"DirectUIHWND") || ClassIs(gti.hwndFocus, L"SysListView32") ? fg : nullptr;
}

bool Down(int vk) { return GetAsyncKeyState(vk) & 0x8000; }

LRESULT CALLBACK KeyboardProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        auto* k = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
        bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
        if (k->vkCode == VK_SPACE) {
            // Ignore auto-repeat: act only on the first key-down.
            if (down && !g_spaceDown && !Down(VK_CONTROL) && !Down(VK_SHIFT) && !Down(VK_MENU) &&
                !Down(VK_LWIN) && !Down(VK_RWIN))
                if (HWND ex = ExplorerWithItemFocus()) PostMessageW(g_hwnd, WM_FLICK_TOGGLE, (WPARAM)ex, Now());
            g_spaceDown = down;
        } else if (PreviewVisible()) {
            if (k->vkCode == VK_ESCAPE && down) {
                if (ExplorerWithItemFocus()) PostMessageW(g_hwnd, WM_FLICK_HIDE, 0, 0);
            } else if (!down && ((k->vkCode >= VK_PRIOR && k->vkCode <= VK_DOWN) || k->vkCode == VK_TAB)) {
                // Explorer moved the selection on key-down; follow it on key-up.
                if (HWND ex = ExplorerWithItemFocus()) PostMessageW(g_hwnd, WM_FLICK_FOLLOW, (WPARAM)ex, Now());
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

// The hook lives on its own thread so a busy UI thread never delays system-wide keyboard input.
DWORD WINAPI HookThread(void*) {
    SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, GetModuleHandleW(nullptr), 0);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {}
    return 0;
}

// Finds the Explorer tab (IShellBrowser) of `explorer` that owns `focus`; costs ~2 COM round trips per window.
ComPtr<IShellBrowser> FindBrowser(IShellWindows* windows, HWND explorer, HWND focus) {
    ComPtr<IShellBrowser> browser;
    ComPtr<IDispatch> disp;
    if (IsDesktop(explorer)) {
        VARIANT empty{};
        long h;
        if (windows->FindWindowSW(&empty, &empty, SWC_DESKTOP, &h, SWFO_NEEDDISPATCH, &disp) == S_OK)
            IUnknown_QueryService(disp.Get(), SID_STopLevelBrowser, IID_PPV_ARGS(&browser));
        return browser;
    }
    long count = 0;
    windows->get_Count(&count);
    for (long i = 0; i < count; i++) {
        VARIANT vi{};
        vi.vt = VT_I4;
        vi.lVal = i;
        ComPtr<IWebBrowserApp> app;
        SHANDLE_PTR h = 0;
        if (FAILED(windows->Item(vi, &disp)) || !disp || FAILED(disp.As(&app)) || FAILED(app->get_HWND(&h)) ||
            (HWND)h != explorer)
            continue;
        ComPtr<IShellBrowser> b;
        if (FAILED(IUnknown_QueryService(app.Get(), SID_STopLevelBrowser, IID_PPV_ARGS(&b)))) continue;
        // Windows 11 tabs: several browsers share the top-level HWND; pick the one owning the focus.
        HWND tab = nullptr;
        b->GetWindow(&tab);
        bool focused = tab && IsChild(tab, focus);
        if (!browser || focused) browser = b;
        if (focused) break;
    }
    return browser;
}

void PrintMemory(const wchar_t* label) {
    PROCESS_MEMORY_COUNTERS_EX pmc{sizeof pmc};
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof pmc);
    Log(L"memory %s: working set %.1f MB, private %.1f MB", label, pmc.WorkingSetSize / 1048576.0,
        pmc.PrivateUsage / 1048576.0);
}

void Pump() {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
}

// `Flick.exe --bench file...`: open each file repeatedly in-process (no Explorer involved).
int Bench(int argc, wchar_t** argv) {
    constexpr int kRuns = 20;
    for (int i = 2; i < argc; i++) {
        wchar_t full[MAX_PATH];
        GetFullPathNameW(argv[i], MAX_PATH, full, nullptr);
        std::vector<double> ready, composed;
        Timing first{}, t{};
        for (int r = 0; r < kRuns; r++) {
            t = PreviewShow(full, nullptr, Now());
            if (r == 0) first = t;
            ready.push_back(t.ready);
            composed.push_back(t.composed);
            Pump();
            PreviewHide(false);
            Pump();
        }
        std::sort(ready.begin(), ready.end());
        std::sort(composed.begin(), composed.end());
        Log(L"BENCH %-5s %-28s first: ready %6.2f ms, on-screen %6.2f ms | median: load %5.2f, ready %6.2f ms, "
            L"on-screen %6.2f ms | min ready %5.2f ms",
            t.kind, PathFindFileNameW(full), first.ready, first.composed, t.load, ready[kRuns / 2],
            composed[kRuns / 2], ready[0]);
    }
    PrintMemory(L"after bench");
    return 0;
}

}  // namespace

LONGLONG Now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

double MsSince(LONGLONG t0) { return (Now() - t0) * 1000.0 / g_freq; }

void Log(const wchar_t* fmt, ...) {
    wchar_t w[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnwprintf_s(w, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) n = (int)wcslen(w);
    w[n++] = L'\n';
    char u[3072];
    int len = WideCharToMultiByte(CP_UTF8, 0, w, n, u, sizeof u, nullptr, nullptr);
    DWORD written;
    WriteFile(g_logFile, u, len, &written, nullptr);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE) WriteFile(out, u, len, &written, nullptr);
}

std::wstring ExplorerSelection(HWND explorer) {
    // Cached across presses: finding the tab is the slow part (cross-process enumeration),
    // asking a known tab for its selection is ~2 round trips.
    static ComPtr<IShellWindows> windows;
    static ComPtr<IShellBrowser> browser;
    static HWND browserTop, browserTab;
    GUITHREADINFO gti{sizeof gti};
    GetGUIThreadInfo(GetWindowThreadProcessId(explorer, nullptr), &gti);
    bool cached = browser && browserTop == explorer && IsWindow(browserTab) &&
                  (IsDesktop(explorer) || IsChild(browserTab, gti.hwndFocus));
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!cached) {
            if (!windows && FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&windows))))
                return {};
            browser = FindBrowser(windows.Get(), explorer, gti.hwndFocus);
            browserTop = explorer;
            browserTab = nullptr;
            if (!browser) {
                windows.Reset();  // Explorer may have restarted; retry with a fresh proxy
                continue;
            }
            browser->GetWindow(&browserTab);
        }
        cached = false;
        ComPtr<IShellView> view;
        ComPtr<IFolderView2> folderView;
        ComPtr<IShellItemArray> items;
        ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (FAILED(browser->QueryActiveShellView(&view)) || FAILED(view.As(&folderView))) continue;  // stale cache
        if (folderView->GetSelection(FALSE, &items) != S_OK || FAILED(items->GetItemAt(0, &item)) ||
            FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
            return {};
        std::wstring result = path;
        CoTaskMemFree(path);
        return result;
    }
    browser.Reset();
    return {};
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_freq = f.QuadPart;
    int argc;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool bench = argc > 1 && !wcscmp(argv[1], L"--bench");

    if (argc > 1 && !wcscmp(argv[1], L"--exit")) {
        if (HWND h = FindWindowW(L"FlickPreview", nullptr)) PostMessageW(h, WM_FLICK_QUIT, 0, 0);
        return 0;
    }
    // One resident instance; benchmarks may run alongside it.
    if (!bench) {
        CreateMutexW(nullptr, FALSE, L"Local\\Flick.SingleInstance");
        if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    }

    wchar_t logPath[MAX_PATH];
    ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\Flick", logPath, MAX_PATH);
    CreateDirectoryW(logPath, nullptr);
    wcscat_s(logPath, bench ? L"\\bench.log" : L"\\flick.log");
    g_logFile = CreateFileW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    LONGLONG t0 = Now();
    if (!PreviewInit(inst)) return 1;
    Log(L"started in %.1f ms (init + warm-up)", MsSince(t0));
    PrintMemory(L"idle");
    if (bench) return Bench(argc, argv);

    CloseHandle(CreateThread(nullptr, 0, HookThread, nullptr, 0, nullptr));
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (PreviewKey(m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
