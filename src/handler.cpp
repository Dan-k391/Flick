// Windows preview handlers (what Explorer's preview pane and QuickLook use for Office files): the real
// look, drawn by the application's own previewer in its own process, so none of it loads into Flick.
// Each preview runs on its own STA thread (DoPreview blocks for hundreds of ms); the handler's window is a
// child of `host`, which the UI thread shows when WM_FLICK_HANDLER reports it ready.
#include "app.h"
#include <shlobj.h>
#include <shlwapi.h>
#include <propsys.h>

namespace {

struct Job {
    std::wstring path;
    HWND host;
    int gen;
    COLORREF bg, fg;
    DWORD thread = 0;
};
std::atomic<int> g_gen;
DWORD g_thread;  // the current job's thread (0: none)

constexpr UINT kResize = WM_APP + 1;

DWORD WINAPI Thread(void* p) {
    std::unique_ptr<Job> job(static_cast<Job*>(p));
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    LONGLONG t0 = Now();
    Microsoft::WRL::ComPtr<IPreviewHandler> h;
    wchar_t clsid[64];
    DWORD n = 64;
    CLSID id;
    bool ok = SUCCEEDED(AssocQueryStringW(ASSOCF_INIT_DEFAULTTOSTAR, ASSOCSTR_SHELLEXTENSION, PathFindExtensionW(job->path.c_str()),
                                          L"{8895b1c6-b41f-4c1c-a562-0d564250836f}", clsid, &n)) &&
              SUCCEEDED(CLSIDFromString(clsid, &id)) &&
              SUCCEEDED(CoCreateInstance(id, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&h)));
    double created = MsSince(t0);
    if (ok) {
        Microsoft::WRL::ComPtr<IInitializeWithFile> f;
        Microsoft::WRL::ComPtr<IInitializeWithStream> s;
        Microsoft::WRL::ComPtr<IStream> stream;
        if (SUCCEEDED(h.As(&f))) ok = SUCCEEDED(f->Initialize(job->path.c_str(), STGM_READ));
        else if (SUCCEEDED(h.As(&s)))
            ok = SUCCEEDED(SHCreateStreamOnFileEx(job->path.c_str(), STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE, nullptr, &stream)) &&
                 SUCCEEDED(s->Initialize(stream.Get(), STGM_READ));
        else ok = false;
    }
    double inited = MsSince(t0);
    if (ok && job->gen == g_gen) {
        Microsoft::WRL::ComPtr<IPreviewHandlerVisuals> v;
        if (SUCCEEDED(h.As(&v))) v->SetBackgroundColor(job->bg), v->SetTextColor(job->fg);
        RECT rc;
        GetClientRect(job->host, &rc);
        ok = SUCCEEDED(h->SetWindow(job->host, &rc)) && SUCCEEDED(h->DoPreview());
    }
    Log(L"handler %s: create %.0f, init %.0f, shown %.0f ms  %s", ok ? L"ok" : L"failed", created, inited, MsSince(t0),
        job->path.c_str());
    PostMessageW(g_hwnd, WM_FLICK_HANDLER, job->gen, ok);
    MSG m;
    while (job->gen == g_gen && GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (m.message == kResize && h) {
            RECT rc;
            GetClientRect(job->host, &rc);
            h->SetRect(&rc);
        }
        DispatchMessageW(&m);
    }
    if (h) h->Unload();
    h.Reset();
    PostMessageW(g_hwnd, WM_FLICK_HANDLER, -1, (LPARAM)job->host);  // the UI thread destroys the host window
    CoUninitialize();
    return 0;
}

}  // namespace

bool g_handlersOff;

bool HandlerAvailable(const std::wstring& path) {
    if (g_handlersOff) return false;
    wchar_t clsid[64];
    DWORD n = 64;
    return SUCCEEDED(AssocQueryStringW(ASSOCF_INIT_DEFAULTTOSTAR, ASSOCSTR_SHELLEXTENSION, PathFindExtensionW(path.c_str()),
                                       L"{8895b1c6-b41f-4c1c-a562-0d564250836f}", clsid, &n));
}

int HandlerStart(const std::wstring& path, HWND host, COLORREF bg, COLORREF fg) {
    HandlerStop();
    auto job = new Job{path, host, ++g_gen, bg, fg};
    HANDLE t = CreateThread(nullptr, 0, Thread, job, 0, &g_thread);
    if (!t) {
        delete job;
        return 0;
    }
    CloseHandle(t);
    return g_gen;
}

void HandlerResize() {
    if (g_thread) PostThreadMessageW(g_thread, kResize, 0, 0);
}

void HandlerStop() {
    if (!g_thread) return;
    g_gen++;
    PostThreadMessageW(g_thread, WM_NULL, 0, 0);  // wakes GetMessage, which then sees the new generation
    g_thread = 0;
}
