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
            // PDF pages arrive from the worker: give it time so "page 1 ready" gets logged.
            for (LONGLONG s = Now(); (!wcscmp(t.kind, L"pdf") || !wcscmp(t.kind, L"video")) && MsSince(s) < 600;)
                MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT), Pump();
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

// Notification-area icon: right- or left-click opens a menu with "Start at sign-in" and "Quit".
// Sign-in start is the HKCU Run value "Flick", pointing at this exe.
constexpr UINT WM_TRAY = WM_APP + 100;
const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
NOTIFYICONDATAW g_tray{sizeof g_tray};

bool StartsAtSignIn() {
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"Flick", RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

void SetStartAtSignIn(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k)) return;
    if (on) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(exe) + L"\"";
        RegSetValueExW(k, L"Flick", 0, REG_SZ, (const BYTE*)cmd.c_str(), DWORD((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, L"Flick");
    }
    RegCloseKey(k);
}

LRESULT CALLBACK TrayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    static UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (m == taskbarCreated) Shell_NotifyIconW(NIM_ADD, &g_tray);  // Explorer restarted
    if (m == WM_TRAY && (l == WM_RBUTTONUP || l == WM_LBUTTONUP)) {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING | (StartsAtSignIn() ? MF_CHECKED : 0), 1, L"Start at sign-in");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 2, L"Quit Flick");
        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(h);  // else the menu doesn't close when clicking elsewhere
        int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, nullptr);
        DestroyMenu(menu);
        if (cmd == 1) SetStartAtSignIn(!StartsAtSignIn());
        if (cmd == 2) PostMessageW(g_hwnd, WM_FLICK_QUIT, 0, 0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void TrayAdd(HINSTANCE inst) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = TrayProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"FlickTray";
    RegisterClassW(&wc);
    g_tray.hWnd = CreateWindowW(L"FlickTray", L"", 0, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);  // hidden
    g_tray.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_tray.uCallbackMessage = WM_TRAY;
    g_tray.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                     GetSystemMetrics(SM_CYSMICON), 0);
    wcscpy_s(g_tray.szTip, L"Flick: select a file in Explorer and press Space");
    Shell_NotifyIconW(NIM_ADD, &g_tray);
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
    bool show = argc > 2 && !wcscmp(argv[1], L"--show");  // `--show file`: preview one file, exit when closed
    bool selftest = argc > 1 && !wcscmp(argv[1], L"--selftest");
    wchar_t logPath[MAX_PATH];
    ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\Flick", logPath, MAX_PATH);
    CreateDirectoryW(logPath, nullptr);
    wcscat_s(logPath, bench || show || selftest ?L"\\bench.log" : L"\\flick.log");
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    bool tooBig = GetFileAttributesExW(logPath, GetFileExInfoStandard, &fa) && fa.nFileSizeLow > (1 << 20);
    g_logFile = CreateFileW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            tooBig ? CREATE_ALWAYS : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);  // keep it < ~1 MB

    if (argc > 1 && !wcscmp(argv[1], L"--selftest")) {
        auto lexer = [](const wchar_t* f) { const Lang* l = LangForFile(f); return l ? l->lexer : ""; };
        bool ok = !strcmp(lexer(LR"(C:\a\Main.CPP)"), "cpp") && !strcmp(lexer(L"x.py"), "python") &&
                  !strcmp(lexer(L"CMakeLists.txt"), "cmake") && !strcmp(lexer(L"Makefile"), "makefile") &&
                  !strcmp(lexer(L"a.json"), "json") && !strcmp(lexer(L"notes.txt"), "") &&
                  !strcmp(lexer(L"a.c.bak"), "") && !strcmp(lexer(L"x.h"), "cpp") && !strcmp(lexer(L"noext"), "");
        if (auto g = GridParseCsv(L"id,name\n1,\"x, \"\"y\"\"\nz\"\n\n2,b\r\n3", 0)) {
            auto cell = [&](size_t r, size_t c) {
                auto [o, n] = g->cells[r * g->cols + c];
                return g->text.substr(o, n);
            };
            if (g->Rows() != 4 || g->cols != 2 || cell(1, 1) != L"x, \"y\" z" || cell(2, 1) != L"b" ||
                cell(3, 0) != L"3" || cell(3, 1) != L"" || !g->rightAlign[0] || g->rightAlign[1])
                ok = false, Log(L"selftest: csv");
        } else {
            ok = false;
        }
        {
            auto runs = Highlight(L"int x = 1; // c\n", LangForName(L"cpp"));
            bool kw = false, comment = false;
            for (auto& r : runs) kw |= r.start == 0 && r.len == 3, comment |= r.role == R_Comment;
            if (!kw || !comment) ok = false, Log(L"selftest: highlight cpp");
            Highlight(L"def f(x):\n    return f\"{x}\"  # c\n", LangForName(L"python"));
        }
        {
            auto has = [&](const std::wstring& s, const std::wstring& part, const wchar_t* what) {
                if (s.find(part) == std::wstring::npos) ok = false, Log(L"selftest: %s", what);
            };
            std::wstring nb = NotebookToMarkdown(LR"({"metadata": {}, "cells": [
                {"cell_type": "markdown", "source": ["# Nb\n", "text"]},
                {"cell_type": "code", "source": "x = 1", "outputs": [{"output_type": "stream", "text": ["1\n"]},
                 {"output_type": "display_data", "data": {"image/png": "iVBO\nRw=="}}]}]})");
            has(nb, L"# Nb\ntext", L"notebook markdown");
            has(nb, L"```python\nx = 1\n```", L"notebook code");
            has(nb, L"```\n1\n```", L"notebook stream");
            has(nb, L"(data:image/png;base64,iVBORw==)", L"notebook image");
            std::wstring adoc = AsciiDocToMarkdown(LR"(= Title
NOTE: hi *b* see https://x.org[site]
[source,py]
----
x
----
)");
            has(adoc, L"# Title", L"adoc heading");
            has(adoc, L"> **Note:** hi **b** see [site](https://x.org)", L"adoc inline");
            has(adoc, L"```py\nx\n```", L"adoc code");
            std::wstring rst = RstToMarkdown(LR"(Title
=====

Para ``code`` and `link <http://x>`_.

.. code-block:: python

   y = 2
)");
            has(rst, L"# Title", L"rst heading");
            has(rst, L"Para `code` and [link](http://x).", L"rst inline");
            has(rst, L"```python\ny = 2\n```", L"rst code");
        }
        {  // tree: a(dir){ b(dir){ c }, d }, e  -> collapsed shows a, e; expanding a shows a, b, d, e
            auto g = std::make_unique<GridData>();
            g->cols = 1, g->header = true;
            for (const wchar_t* n : {L"Name", L"a", L"b", L"c", L"d", L"e"}) g->cells.push_back({0, 0}), (void)n;
            g->indent = {0, 1, 2, 1, 0};
            g->folder = {true, true, false, false, false};
            g->rightAlign = {false};
            GridSet(std::move(g));
            size_t collapsed = GridShownRows();
            GridClick({0, 0, 100, 1000}, 0, {10, 28 + 14});  // first body row
            if (collapsed != 2 || GridShownRows() != 4) ok = false, Log(L"selftest: tree %zu %zu", collapsed, GridShownRows());
            GridSet(nullptr);
        }
        MdLoad(LR"(# Title
Para &amp; *x* <b>y</b>

- a
  - [x] b
1. one

> q

```
c
```
---
| a |
|---|
| 1 |
<div>html</div>
)");
        std::wstring want = L"H1:Title|P:Para & x y|P•:a|P☑" + std::wstring(1, (wchar_t)0xFE0E) +
                            L":b|P1.:one|P:q>|C:c|R:|T:|";
        if (MdOutline() != want) ok = false, Log(L"selftest: markdown outline %s", MdOutline().c_str());
        return ok ? 0 : 1;
    }
    if (argc > 1 && !wcscmp(argv[1], L"--exit")) {
        if (HWND h = FindWindowW(L"FlickPreview", nullptr)) PostMessageW(h, WM_FLICK_QUIT, 0, 0);
        return 0;
    }
    // One resident instance; benchmarks may run alongside it.
    if (!bench && !show) {
        CreateMutexW(nullptr, FALSE, L"Local\\Flick.SingleInstance");
        if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    LONGLONG t0 = Now();
    if (!PreviewInit(inst)) return 1;
    ExplorerSelection(GetShellWindow());  // warm up the Explorer COM proxies (desktop path)
    Log(L"started in %.1f ms (init + warm-up)", MsSince(t0));
    PrintMemory(L"idle");
    MediaMute(bench || (show && argc > 3 && !wcscmp(argv[3], L"--mute")));
    if (bench) return Bench(argc, argv);
    if (show) {
        wchar_t full[MAX_PATH];
        GetFullPathNameW(argv[2], MAX_PATH, full, nullptr);
        PreviewShow(full, nullptr, Now());
    } else {
        TrayAdd(inst);
    }

    CloseHandle(CreateThread(nullptr, 0, HookThread, nullptr, 0, nullptr));
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!PreviewKey(m)) TranslateMessage(&m), DispatchMessageW(&m);
        if (show && !PreviewVisible()) break;
    }
    if (g_tray.hWnd) Shell_NotifyIconW(NIM_DELETE, &g_tray);
    return 0;
}
