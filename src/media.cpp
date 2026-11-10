// Audio/video playback with Media Foundation's media engine in frame-server mode, without a DXGI
// device: decoding and scaling run on the CPU, so no GPU driver is loaded into the process. Frames
// are pulled once per display refresh (a thread waiting on DwmFlush posts WM_FLICK_FRAME) and
// copied at the size shown.
#include "app.h"
#include <mfapi.h>
#include <mfmediaengine.h>
#include <mfreadwrite.h>
#include <dwmapi.h>
#include <atomic>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace {

ComPtr<IMFMediaEngine> g_engine;
ComPtr<IMFMediaEngineEx> g_engineEx;
std::atomic<int> g_pumpGen;  // a pump thread runs while the generation it started with is current
std::atomic<bool> g_framePending;
bool g_error;
bool g_muted;  // --bench / --mute: tests stay silent

class Notify : public IMFMediaEngineNotify {
    std::atomic<LONG> ref_{1};

public:
    STDMETHODIMP QueryInterface(REFIID riid, void** p) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEngineNotify)) {
            *p = static_cast<IMFMediaEngineNotify*>(this);
            AddRef();
            return S_OK;
        }
        *p = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override {
        LONG r = --ref_;
        if (!r) delete this;
        return r;
    }
    STDMETHODIMP EventNotify(DWORD event, DWORD_PTR param1, DWORD) override {
        if (event == MF_MEDIA_ENGINE_EVENT_NOTIFYSTABLESTATE) SetEvent((HANDLE)param1);
        else PostMessageW(g_hwnd, WM_FLICK_MEDIA, event, 0);  // never block an MF thread on the UI
        return S_OK;
    }
};

DWORD WINAPI PumpThread(void* gen) {
    while (g_pumpGen == (int)(INT_PTR)gen) {
        DwmFlush();  // once per composition = display refresh
        if (!g_framePending.exchange(true)) PostMessageW(g_hwnd, WM_FLICK_FRAME, 0, 0);
    }
    return 0;
}

}  // namespace

bool MediaOpen(const std::wstring& path) {
    MediaClose();
    static bool started = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    static ComPtr<IMFMediaEngineClassFactory> factory;
    if (!started || (!factory && FAILED(CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER,
                                                         IID_PPV_ARGS(&factory)))))
        return false;
    ComPtr<IMFAttributes> attrs;
    ComPtr<IMFMediaEngineNotify> notify;
    notify.Attach(new Notify);
    MFCreateAttributes(&attrs, 3);
    attrs->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify.Get());
    attrs->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);  // frame-server mode
    attrs->SetUINT32(MF_MEDIA_ENGINE_AUDIO_CATEGORY, 3 /* AudioCategory_Media */);
    if (FAILED(factory->CreateInstance(0, attrs.Get(), &g_engine))) return false;
    g_engine.As(&g_engineEx);
    g_error = false;
    BSTR url = SysAllocString(path.c_str());
    g_engine->SetAutoPlay(TRUE);
    g_engine->SetMuted(g_muted);
    HRESULT hr = g_engine->SetSource(url);
    SysFreeString(url);
    if (FAILED(hr)) return MediaClose(), false;
    g_framePending = false;
    CloseHandle(CreateThread(nullptr, 0, PumpThread, (void*)(INT_PTR)++g_pumpGen, 0, nullptr));
    return true;
}

void MediaClose() {
    g_pumpGen++;  // the pump thread exits after its current DwmFlush
    if (g_engine) g_engine->Shutdown();
    g_engineEx.Reset(), g_engine.Reset();
}

void MediaFrameHandled() { g_framePending = false; }

void MediaOnEvent(DWORD event) {
    if (event == MF_MEDIA_ENGINE_EVENT_ERROR) g_error = true;
}

MediaState MediaGet() {
    MediaState s{};
    if (!g_engine) return s;
    s.error = g_error;
    s.ready = g_engine->GetReadyState() >= MF_MEDIA_ENGINE_READY_HAVE_METADATA;
    s.paused = g_engine->IsPaused() != FALSE;
    s.ended = g_engine->IsEnded() != FALSE;
    s.pos = g_engine->GetCurrentTime();
    s.dur = g_engine->GetDuration();
    if (!std::isfinite(s.dur)) s.dur = 0;
    DWORD w = 0, h = 0, ax = 1, ay = 1;
    if (g_engine->HasVideo() && SUCCEEDED(g_engine->GetNativeVideoSize(&w, &h)) && w && h) {
        // GetVideoAspectRatio is the picture aspect ratio (16:9), which also covers anamorphic video.
        if (SUCCEEDED(g_engine->GetVideoAspectRatio(&ax, &ay)) && ax && ay) w = (DWORD)std::lround((double)h * ax / ay);
        s.videoW = w, s.videoH = h;
    }
    return s;
}

// Copies the current frame into `out` (w x h pixels, recreated when the size changes) when a new one
// is due. False: no new frame.
bool MediaFrame(UINT w, UINT h, IWICImagingFactory* wic, ComPtr<IWICBitmap>& out) {
    LONGLONG pts;
    if (!g_engine || !w || !h || g_engine->OnVideoStreamTick(&pts) != S_OK) return false;
    UINT cw = 0, ch = 0;
    if (out) out->GetSize(&cw, &ch);
    if ((cw != w || ch != h) && FAILED(wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &out)))
        return false;
    RECT dst{0, 0, (LONG)w, (LONG)h};
    MFARGB black{0, 0, 0, 255};
    return SUCCEEDED(g_engine->TransferVideoFrame(out.Get(), nullptr, &dst, &black));
}

void MediaToggle() {
    if (!g_engine) return;
    if (g_engine->IsEnded()) g_engine->SetCurrentTime(0), g_engine->Play();
    else if (g_engine->IsPaused()) g_engine->Play();
    else g_engine->Pause();
}

void MediaSeek(double seconds) {
    if (!g_engine) return;
    if (g_engineEx) g_engineEx->SetCurrentTimeEx(seconds, MF_MEDIA_ENGINE_SEEK_MODE_NORMAL);
    else g_engine->SetCurrentTime(seconds);
}

void MediaMute(bool muted) { g_muted = muted; }

// The first frame straight from a source reader (no playback pipeline, ~75 ms): shown while the media
// engine is still starting, which takes 200-300 ms. Worker thread.
ComPtr<IWICBitmap> MediaFirstFrame(const std::wstring& path, IWICImagingFactory* wic) {
    static bool started = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    ComPtr<IMFAttributes> attrs;
    ComPtr<IMFSourceReader> reader;
    ComPtr<IMFMediaType> type, actual;
    ComPtr<IMFSample> sample;
    ComPtr<IMFMediaBuffer> buffer;
    DWORD stream, flags = 0;
    LONGLONG ts;
    if (!started || FAILED(MFCreateAttributes(&attrs, 1)) ||
        FAILED(attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE)) ||
        FAILED(MFCreateSourceReaderFromURL(path.c_str(), attrs.Get(), &reader)) ||
        FAILED(reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE)) ||
        FAILED(reader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE)) || FAILED(MFCreateMediaType(&type)) ||
        FAILED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) || FAILED(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32)) ||
        FAILED(reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type.Get())) ||
        FAILED(reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &actual)))
        return nullptr;
    for (int i = 0; i < 8 && !sample; i++)  // decoders may need a few reads before the first frame
        if (FAILED(reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream, &flags, &ts, &sample)) ||
            (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM)))
            return nullptr;
    UINT32 w = 0, h = 0;
    MFGetAttributeSize(actual.Get(), MF_MT_FRAME_SIZE, &w, &h);
    LONG stride = (LONG)MFGetAttributeUINT32(actual.Get(), MF_MT_DEFAULT_STRIDE, w * 4);
    BYTE* data = nullptr;
    DWORD len = 0;
    ComPtr<IWICBitmap> bmp;
    if (!w || !h || FAILED(sample->ConvertToContiguousBuffer(&buffer)) || FAILED(buffer->Lock(&data, nullptr, &len))) return nullptr;
    if ((DWORD)std::abs(stride) * h <= len && SUCCEEDED(wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppBGR, WICBitmapCacheOnLoad, &bmp))) {
        WICRect all{0, 0, (INT)w, (INT)h};
        ComPtr<IWICBitmapLock> lock;
        UINT outStride = 0, outSize = 0;
        BYTE* out = nullptr;
        if (SUCCEEDED(bmp->Lock(&all, WICBitmapLockWrite, &lock)) && SUCCEEDED(lock->GetStride(&outStride)) &&
            SUCCEEDED(lock->GetDataPointer(&outSize, &out)))
            for (UINT y = 0; y < h; y++)  // negative stride: bottom-up rows
                memcpy(out + y * outStride, data + (stride < 0 ? (h - 1 - y) * (size_t)-stride : y * (size_t)stride), w * 4);
    }
    buffer->Unlock();
    return bmp;
}
