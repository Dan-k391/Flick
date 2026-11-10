// PDF pages are rendered by Windows' own PDF engine (Windows.Data.Pdf) on one worker thread.
// The worker hands every result to the UI thread with SendMessage, and the reply names the next page
// to render: the UI thread alone decides what is needed (visible pages, current width) and never
// waits for the worker.
#include "app.h"
#include <shcore.h>
#include <d3d11.h>
#include <d2d1_1.h>
#include <windows.data.pdf.interop.h>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Data.Pdf.h>
#include <winrt/Windows.Storage.Streams.h>

using namespace winrt::Windows::Data::Pdf;
using namespace winrt::Windows::Storage::Streams;
using Microsoft::WRL::ComPtr;

namespace {

HANDLE g_wake;
std::mutex g_mu;
std::wstring g_wantPath;
int g_wantGen;

// Renders straight into a CPU bitmap through a software Direct2D target. The convenient
// PdfPage::RenderToStreamAsync uses a hardware device, which loads the GPU driver (~200 MB private).
ComPtr<IWICBitmap> Render(const PdfDocument& doc, int index, UINT width, IPdfRendererNative* renderer,
                          ID2D1Factory* d2d, IWICImagingFactory* wic) {
    PdfPage page = doc.GetPage(index);
    auto size = page.Size();
    UINT height = std::max(1u, (UINT)std::lround(width * size.Height / size.Width));
    ComPtr<IWICBitmap> bmp;
    ComPtr<ID2D1RenderTarget> rt;
    ComPtr<ID2D1DeviceContext> dc;
    winrt::check_hresult(wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp));
    winrt::check_hresult(d2d->CreateWicBitmapRenderTarget(
        bmp.Get(),
        D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                     D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
        &rt));
    winrt::check_hresult(rt.As(&dc));
    PDF_RENDER_PARAMS params{{0, 0, size.Width, size.Height}, width, height, {1, 1, 1, 1}, FALSE};
    dc->BeginDraw();
    HRESULT hr = renderer->RenderPageToDeviceContext(winrt::get_unknown(page), dc.Get(), &params);
    HRESULT end = dc->EndDraw();
    winrt::check_hresult(FAILED(hr) ? hr : end);
    return bmp;
}

DWORD WINAPI PdfThread(void*) {
    winrt::init_apartment();  // MTA: blocking .get() is not allowed on an STA
    ComPtr<IWICImagingFactory> wic;
    ComPtr<ID2D1Factory> d2d;
    ComPtr<ID3D11Device> d3d;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IPdfRendererNative> renderer;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf());
    // The renderer wants a DXGI device; WARP (software) keeps the GPU driver out of the process.
    auto createRenderer = [&] {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr,
                                     0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr)) ||
            FAILED(d3d.As(&dxgi)) || FAILED(PdfCreateRenderer(dxgi.Get(), &renderer)))
            Log(L"pdf: no renderer");
    };
    winrt::get_activation_factory<PdfDocument>();  // warm-up: loads Windows.Data.Pdf.dll
    PdfDocument doc{nullptr};
    std::vector<std::pair<float, float>> sizes;
    UINT32 pages = 0;
    int loaded = -1;
    // Page sizes in DIPs, up to page `n`. A few at first: in a 1000-page book reading them all takes ~50 ms.
    auto readSizes = [&](UINT32 n) {
        for (UINT32 i = (UINT32)sizes.size(); i < n; i++) {
            auto sz = doc.GetPage(i).Size();
            sizes.push_back({sz.Width, sz.Height});
        }
    };
    for (;;) {
        WaitForSingleObject(g_wake, INFINITE);
        std::wstring path;
        int gen;
        {
            std::lock_guard lock(g_mu);
            path = g_wantPath, gen = g_wantGen;
        }
        PdfMsg msg{gen, -2};  // -2: "idle, what next?"
        if (gen != loaded) {
            loaded = gen, doc = nullptr, sizes.clear(), msg.page = -1;  // -1: opened (sizes) or failed (none)
            if (path.empty()) {  // closed: release memory, including the renderer's caches
                renderer.Reset(), dxgi.Reset(), d3d.Reset(), d2d.Reset();
                continue;
            }
            if (!renderer) createRenderer();
            if (!d2d) D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf());
            try {
                IRandomAccessStream file{nullptr};
                winrt::check_hresult(CreateRandomAccessStreamOnFile(path.c_str(), 0 /* read */,
                                                                    winrt::guid_of<IRandomAccessStream>(),
                                                                    winrt::put_abi(file)));
                doc = PdfDocument::LoadFromStreamAsync(file).get();
                pages = doc.PageCount();
                readSizes(std::min(pages, 8u));
                msg.sizes = sizes, msg.pageCount = pages;
            } catch (...) {  // damaged or password-protected
                doc = nullptr;
            }
        }
        for (LRESULT next; (next = SendMessageW(g_hwnd, WM_FLICK_PDF, 0, (LPARAM)&msg)) > 0 && doc;) {
            msg = {gen, (int)(next & 0xFFFFFFFF) - 1};
            try {
                msg.bmp = Render(doc, msg.page, (UINT)(next >> 32), renderer.Get(), d2d.Get(), wic.Get());
                if (sizes.size() < pages) readSizes(pages), msg.sizes = sizes;  // the rest, after the first page
            } catch (...) {
            }
        }
    }
}

}  // namespace

void PdfOpen(const std::wstring& path, int gen) {
    if (!g_wake) {
        g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        CloseHandle(CreateThread(nullptr, 0, PdfThread, nullptr, 0, nullptr));
    }
    {
        std::lock_guard lock(g_mu);
        g_wantPath = path, g_wantGen = gen;
    }
    SetEvent(g_wake);
}

void PdfWake() { SetEvent(g_wake); }
