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
#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace {

ComPtr<IMFMediaEngine> g_engine;
ComPtr<IMFMediaEngineEx> g_engineEx;
std::atomic<int> g_pumpGen;  // a pump thread runs while the generation it started with is current
std::atomic<bool> g_framePending;
bool g_error;
std::wstring g_url;  // source of g_engine
bool g_muted;  // --bench / --mute: tests stay silent
double g_volume = 1;  // the user's volume and mute, kept for the session
bool g_userMuted;

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

bool MediaOpen(const std::wstring& path, bool start) {
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
    g_engine->SetAutoPlay(TRUE);
    g_engine->SetMuted(g_muted || g_userMuted);
    g_engine->SetVolume(g_volume);
    g_url = path;
    return !start || MediaStart();
}

bool MediaStart() {
    if (!g_engine) return false;
    BSTR url = SysAllocString(g_url.c_str());
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

void MediaSetVolume(double v) {
    g_volume = std::clamp(v, 0.0, 1.0);
    if (g_volume > 0 && g_userMuted) MediaToggleMute();  // turning it up unmutes
    if (g_engine) g_engine->SetVolume(g_volume);
}

double MediaVolume() { return g_userMuted ? 0 : g_volume; }

void MediaToggleMute() {
    g_userMuted = !g_userMuted;
    if (g_engine) g_engine->SetMuted(g_muted || g_userMuted);
}

// Frames straight from a source reader (no playback pipeline): the poster shown while the media engine
// starts (200-300 ms), and the frames shown while the seek bar is dragged. Worker threads.
namespace {

struct Reader {
    ComPtr<IMFSourceReader> reader;
    UINT32 w = 0, h = 0;
    LONG stride = 0;
};

bool OpenReader(const std::wstring& path, Reader& r) {
    static bool started = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    ComPtr<IMFAttributes> attrs;
    ComPtr<IMFMediaType> type, actual;
    if (!started || FAILED(MFCreateAttributes(&attrs, 2)) ||
        FAILED(attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE)) ||
        FAILED(attrs->SetUINT32(MF_LOW_LATENCY, TRUE)) ||  // decoder hands out a frame at once instead of buffering
        FAILED(MFCreateSourceReaderFromURL(path.c_str(), attrs.Get(), &r.reader)) ||
        FAILED(r.reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE)) ||
        FAILED(r.reader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE)) || FAILED(MFCreateMediaType(&type)) ||
        FAILED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) || FAILED(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32)) ||
        FAILED(r.reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type.Get())) ||
        FAILED(r.reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &actual)))
        return false;
    MFGetAttributeSize(actual.Get(), MF_MT_FRAME_SIZE, &r.w, &r.h);
    r.stride = (LONG)MFGetAttributeUINT32(actual.Get(), MF_MT_DEFAULT_STRIDE, r.w * 4);
    return r.w && r.h;
}

// The next decoded frame; after SetCurrentPosition that is the key frame at or before the position.
ComPtr<IWICBitmap> ReadFrame(Reader& r, IWICImagingFactory* wic) {
    ComPtr<IMFSample> sample;
    ComPtr<IMFMediaBuffer> buffer;
    DWORD stream, flags = 0;
    LONGLONG ts;
    for (int i = 0; i < 8 && !sample; i++)  // decoders may need a few reads before the first frame
        if (FAILED(r.reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream, &flags, &ts, &sample)) ||
            (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM)))
            return nullptr;
    BYTE* data = nullptr;
    DWORD len = 0;
    ComPtr<IWICBitmap> bmp;
    UINT w = r.w, h = r.h;
    LONG stride = r.stride;
    if (!sample || FAILED(sample->ConvertToContiguousBuffer(&buffer)) || FAILED(buffer->Lock(&data, nullptr, &len))) return nullptr;
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

std::atomic<int> g_scrubGen;
std::atomic<double> g_scrubTo{-1};
HANDLE g_scrubWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);

struct ScrubJob {
    std::wstring path;
    UINT w, h;
    int gen;
};

// Decodes the key frame for the latest g_scrubTo whenever it changes; positions passed while a frame
// decodes are skipped, so the picture never falls behind the mouse.
DWORD WINAPI ScrubThread(void* p) {
    std::unique_ptr<ScrubJob> job(static_cast<ScrubJob*>(p));
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        ComPtr<IWICImagingFactory> wic;
        Reader r;
        double done = -1;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
            OpenReader(job->path, r))
            while (g_scrubGen == job->gen) {
                double to = g_scrubTo;
                if (to < 0 || to == done) {
                    WaitForSingleObject(g_scrubWake, 200);
                    continue;
                }
                done = to;
                PROPVARIANT pos{};
                pos.vt = VT_I8;
                pos.hVal.QuadPart = (LONGLONG)(to * 1e7);
                if (FAILED(r.reader->SetCurrentPosition(GUID_NULL, pos))) continue;
                ComPtr<IWICBitmap> frame = ReadFrame(r, wic.Get()), scaled;
                ComPtr<IWICBitmapScaler> scaler;  // to the size shown: less to hand over and to draw
                if (frame && job->w && job->h && job->w < r.w && SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
                    SUCCEEDED(scaler->Initialize(frame.Get(), job->w, job->h, WICBitmapInterpolationModeLinear)) &&
                    SUCCEEDED(wic->CreateBitmapFromSource(scaler.Get(), WICBitmapCacheOnLoad, &scaled)))
                    frame = scaled;
                if (frame && g_scrubGen == job->gen) SendMessageW(g_hwnd, WM_FLICK_SCRUB, job->gen, (LPARAM)frame.Get());
            }
    }
    CoUninitialize();
    return 0;
}

}  // namespace

ComPtr<IWICBitmap> MediaFirstFrame(const std::wstring& path, IWICImagingFactory* wic) {
    Reader r;
    return OpenReader(path, r) ? ReadFrame(r, wic) : nullptr;
}

int MediaScrubStart(const std::wstring& path, UINT w, UINT h) {
    g_scrubTo = -1;
    auto* job = new ScrubJob{path, w, h, ++g_scrubGen};
    CloseHandle(CreateThread(nullptr, 0, ScrubThread, job, 0, nullptr));
    return job->gen;
}

void MediaScrubTo(double seconds) {
    g_scrubTo = seconds;
    SetEvent(g_scrubWake);
}

void MediaScrubStop() {
    g_scrubGen++;
    SetEvent(g_scrubWake);
}
