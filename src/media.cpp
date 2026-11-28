// Audio/video playback with Media Foundation's media engine in frame-server mode, without a DXGI
// device: decoding and scaling run on the CPU, so no GPU driver is loaded into the process. Frames
// are pulled at display refresh while playing and copied at the size shown. Audio has a slower
// progress tick. The pump sleeps when playback has no updates to show.
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
WPARAM g_mediaGen;  // the UI thread owns the engine and its generation
ULONGLONG g_frameUntil;  // deadline for fast frame probes, even when paused
bool g_error;
bool g_muted;  // --bench / --mute: tests stay silent
double g_volume = 1;  // the user's volume and mute, kept for the session
bool g_userMuted;

class Notify : public IMFMediaEngineNotify {
    std::atomic<LONG> ref_{1};
    const HWND hwnd_;
    const WPARAM gen_;

public:
    Notify(HWND hwnd, WPARAM gen) : hwnd_(hwnd), gen_(gen) {}
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
        switch (event) {
            case MF_MEDIA_ENGINE_EVENT_NOTIFYSTABLESTATE:
                SetEvent((HANDLE)param1);
                break;
            case MF_MEDIA_ENGINE_EVENT_ERROR:
            case MF_MEDIA_ENGINE_EVENT_PLAY:
            case MF_MEDIA_ENGINE_EVENT_PAUSE:
            case MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA:
            case MF_MEDIA_ENGINE_EVENT_LOADEDDATA:
            case MF_MEDIA_ENGINE_EVENT_WAITING:
            case MF_MEDIA_ENGINE_EVENT_PLAYING:
            case MF_MEDIA_ENGINE_EVENT_CANPLAY:
            case MF_MEDIA_ENGINE_EVENT_CANPLAYTHROUGH:
            case MF_MEDIA_ENGINE_EVENT_SEEKING:
            case MF_MEDIA_ENGINE_EVENT_SEEKED:
            case MF_MEDIA_ENGINE_EVENT_ENDED:
            case MF_MEDIA_ENGINE_EVENT_DURATIONCHANGE:
            case MF_MEDIA_ENGINE_EVENT_FORMATCHANGE:
            case MF_MEDIA_ENGINE_EVENT_FIRSTFRAMEREADY:
                PostMessageW(hwnd_, WM_FLICK_MEDIA, gen_, event);  // may run on an MF thread
                break;
        }
        return S_OK;
    }
};

enum class PumpMode { Parked, Audio, Video, Probe, SlowProbe, Closed };

struct FramePump {
    FramePump(HWND hwnd, WPARAM gen, PumpMode mode) : hwnd(hwnd), gen(gen), mode(mode) {}
    ~FramePump() { if (wake) CloseHandle(wake); }

    const HWND hwnd;
    const WPARAM gen;
    const HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::atomic<PumpMode> mode;
    std::atomic<bool> pending{false};
};

std::shared_ptr<FramePump> g_pump;

DWORD WINAPI PumpThread(void* p) {
    // The worker owns only its wake event and message token. All Media Engine and WIC calls stay
    // on the UI thread. Closing can signal this worker without waiting for a blocked DwmFlush.
    std::unique_ptr<std::shared_ptr<FramePump>> job(static_cast<std::shared_ptr<FramePump>*>(p));
    auto pump = std::move(*job);
    job.reset();
    for (;;) {
        PumpMode mode = pump->mode;
        if (mode == PumpMode::Closed) break;
        if (mode != PumpMode::Video || FAILED(DwmFlush())) {
            DWORD delay = INFINITE;
            switch (mode) {
                case PumpMode::Audio: delay = 50; break;
                case PumpMode::Video: delay = 16; break;  // DwmFlush failed
                case PumpMode::Probe: delay = 25; break;
                case PumpMode::SlowProbe: delay = 250; break;
            }
            DWORD wait = WaitForSingleObject(pump->wake, delay);
            if (wait == WAIT_OBJECT_0) continue;
            if (wait != WAIT_TIMEOUT) break;
        }
        if (pump->mode == mode && !pump->pending.exchange(true) &&
            !PostMessageW(pump->hwnd, WM_FLICK_FRAME, pump->gen, 0))
            pump->pending = false;
    }
    return 0;
}

void UpdatePump() {
    PumpMode mode = PumpMode::Parked;
    if (!g_error) {
        bool seeking = g_engine->IsSeeking() != FALSE;
        bool playing = !seeking && g_engine->GetReadyState() >= MF_MEDIA_ENGINE_READY_HAVE_FUTURE_DATA &&
                       !g_engine->IsPaused() && !g_engine->IsEnded();
        if (g_engine->HasVideo()) {
            bool fastProbe = g_frameUntil && GetTickCount64() < g_frameUntil;
            if (!fastProbe && !seeking) g_frameUntil = 0;
            if (playing) mode = PumpMode::Video;
            else if (fastProbe) mode = PumpMode::Probe;
            else if (g_frameUntil) mode = PumpMode::SlowProbe;  // an in-flight seek still needs a frame pull
        } else if (playing) mode = PumpMode::Audio;
    }
    if (g_pump) {
        if (g_pump->mode != mode) {
            g_pump->mode = mode;
            SetEvent(g_pump->wake);
        }
    } else if (mode != PumpMode::Parked) {
        auto pump = std::make_shared<FramePump>(g_hwnd, g_mediaGen, mode);
        if (!pump->wake) return;
        auto* job = new std::shared_ptr<FramePump>(pump);
        if (HANDLE thread = CreateThread(nullptr, 0, PumpThread, job, 0, nullptr)) {
            g_pump = std::move(pump);
            CloseHandle(thread);
        } else delete job;
    }
}

bool StartMediaFoundation() {
    static const bool started = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    return started;
}

}  // namespace

bool MediaOpen(const std::wstring& path) {
    MediaClose();
    static ComPtr<IMFMediaEngineClassFactory> factory;
    if (!StartMediaFoundation() ||
        (!factory && FAILED(CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER,
                                             IID_PPV_ARGS(&factory)))))
        return false;
    ComPtr<IMFAttributes> attrs;
    ComPtr<IMFMediaEngineNotify> notify;
    notify.Attach(new Notify(g_hwnd, g_mediaGen));
    if (FAILED(MFCreateAttributes(&attrs, 3)) || FAILED(attrs->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify.Get())) ||
        FAILED(attrs->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM)) ||  // frame-server mode
        FAILED(attrs->SetUINT32(MF_MEDIA_ENGINE_AUDIO_CATEGORY, 3 /* AudioCategory_Media */)) ||
        FAILED(factory->CreateInstance(0, attrs.Get(), &g_engine)))
        return false;
    g_engine.As(&g_engineEx);
    g_engine->SetAutoPlay(TRUE);
    g_engine->SetMuted(g_muted || g_userMuted);
    g_engine->SetVolume(g_volume);
    BSTR url = SysAllocString(path.c_str());
    if (!url) return MediaClose(), false;
    HRESULT hr = g_engine->SetSource(url);
    SysFreeString(url);
    if (FAILED(hr)) return MediaClose(), false;
    return true;
}

void MediaClose() {
    ++g_mediaGen;  // reject queued frame and callback messages before shutting down the old engine
    if (g_pump) {
        g_pump->mode = PumpMode::Closed;
        SetEvent(g_pump->wake);
        g_pump.reset();
    }
    g_frameUntil = 0;
    g_error = false;
    if (g_engine) g_engine->Shutdown();
    g_engineEx.Reset(), g_engine.Reset();
}

bool MediaOnTick(WPARAM gen, MediaState& state) {
    if (!g_pump || gen != g_mediaGen) return false;
    g_pump->pending = false;
    PumpMode mode = g_pump->mode;
    if (mode == PumpMode::Parked) return false;
    state = MediaGet();
    if ((g_frameUntil && GetTickCount64() >= g_frameUntil) ||
        ((mode == PumpMode::Video || mode == PumpMode::Audio) && (state.paused || state.ended)))
        UpdatePump();
    return true;
}

bool MediaOnEvent(WPARAM gen, DWORD event) {
    if (!g_engine || gen != g_mediaGen) return false;
    if (event == MF_MEDIA_ENGINE_EVENT_ERROR) g_error = true;
    // SEEKED can precede the actual frame. Retry briefly, stopping as soon as a frame is copied.
    if (event == MF_MEDIA_ENGINE_EVENT_LOADEDDATA || event == MF_MEDIA_ENGINE_EVENT_FIRSTFRAMEREADY ||
        event == MF_MEDIA_ENGINE_EVENT_SEEKED || event == MF_MEDIA_ENGINE_EVENT_FORMATCHANGE) {
        if (g_engine->HasVideo()) {
            auto until = GetTickCount64() + 300;
            g_frameUntil = g_engine->IsSeeking() ? std::max(g_frameUntil, until) : until;
        }
    }
    UpdatePump();
    return true;
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
    if (!std::isfinite(s.pos)) s.pos = 0;
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
    if (FAILED(g_engine->TransferVideoFrame(out.Get(), nullptr, &dst, &black))) return false;
    if (g_frameUntil && !g_engine->IsSeeking()) g_frameUntil = 0, UpdatePump();
    return true;
}

void MediaToggle() {
    if (!g_engine || g_error) return;
    if (g_engine->IsEnded()) {
        MediaSeek(0);
        MediaSetPlaying(true);
    } else MediaSetPlaying(g_engine->IsPaused() != FALSE);
}

void MediaSetPlaying(bool play) {
    if (!g_engine || g_error) return;
    if (play) g_engine->Play();
    else g_engine->Pause();
    UpdatePump();
}

void MediaSeek(double seconds) {
    if (!g_engine || g_error || !std::isfinite(seconds)) return;
    HRESULT hr = g_engineEx ? g_engineEx->SetCurrentTimeEx(seconds, MF_MEDIA_ENGINE_SEEK_MODE_NORMAL)
                           : g_engine->SetCurrentTime(seconds);
    // In frame-server mode a paused seek can wait for OnVideoStreamTick before sending SEEKED.
    if (SUCCEEDED(hr) && g_engine->HasVideo()) g_frameUntil = GetTickCount64() + 2000;
    UpdatePump();
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

namespace {

// "01:02:03,456" or (WebVTT, no hours) "02:03.456" -> seconds; -1 if it isn't one.
double SubTime(std::wstring_view& p) {
    auto digit = [&] { return !p.empty() && p[0] >= L'0' && p[0] <= L'9'; };
    while (!p.empty() && p[0] == L' ') p.remove_prefix(1);
    double part[3];
    int n = 0;
    for (;;) {
        if (!digit()) return -1;
        double v = 0;
        while (digit()) v = v * 10 + (p[0] - L'0'), p.remove_prefix(1);
        part[n++] = v;
        if (n < 3 && !p.empty() && p[0] == L':') p.remove_prefix(1);
        else break;
    }
    if (n < 2) return -1;
    double t = n == 3 ? part[0] * 3600 + part[1] * 60 + part[2] : part[0] * 60 + part[1];
    if (!p.empty() && (p[0] == L',' || p[0] == L'.')) {
        p.remove_prefix(1);
        for (double k = 0.1; digit(); k /= 10) t += (p[0] - L'0') * k, p.remove_prefix(1);
    }
    return t;
}

// "start --> end [WebVTT settings]"
bool SubTiming(std::wstring_view l, double& a, double& b) {
    size_t arrow = l.find(L"-->");
    if (arrow == l.npos) return false;
    std::wstring_view pa = l.substr(0, arrow), pb = l.substr(arrow + 3);
    a = SubTime(pa), b = SubTime(pb);
    return a >= 0 && b > a;
}

}  // namespace

std::vector<SubCue> ParseSubtitles(const std::wstring& s) {
    std::vector<SubCue> cues;
    size_t i = 0;
    auto line = [&](std::wstring_view& l) {
        if (i >= s.size()) return false;
        size_t e = std::min(s.find(L'\n', i), s.size());
        l = std::wstring_view(s).substr(i, e - i), i = e + 1;
        if (!l.empty() && l.back() == L'\r') l.remove_suffix(1);
        return true;
    };
    std::wstring_view l;
    bool have = line(l);
    while (have) {
        SubCue c{};
        if (!SubTiming(l, c.start, c.end)) {  // cue numbers, WEBVTT header, NOTE/STYLE blocks
            have = line(l);
            continue;
        }
        size_t open[2] = {SIZE_MAX, SIZE_MAX};  // italic, bold: where the open tag was
        auto tag = [&](int k, bool on) {
            auto& o = open[k];
            if (on && o == SIZE_MAX) o = c.text.size();
            else if (!on && o != SIZE_MAX) {
                if (c.text.size() > o) (k ? c.bold : c.italic).push_back({(UINT32)o, (UINT32)(c.text.size() - o)});
                o = SIZE_MAX;
            }
        };
        double a, b;
        // Text lines up to a blank line (or, in a file missing it, the next timing line).
        while ((have = line(l)) && l.find_first_not_of(L" \t") != l.npos && !SubTiming(l, a, b)) {
            if (!c.text.empty()) c.text += L'\n';
            for (size_t k = 0; k < l.size(); k++) {
                wchar_t ch = l[k];
                size_t e = ch == L'<' ? l.find(L'>', k) : ch == L'{' ? l.find(L'}', k) : l.npos;
                if (e != l.npos) {  // <i>, </b>, <font ...>, <c.x>, <v Name>; {\i1} (ASS overrides) and other tags dropped
                    std::wstring_view t = l.substr(k + 1, e - k - 1);
                    bool ass = ch == L'{';
                    if (ass && (t.empty() || t[0] != L'\\')) e = l.npos;  // a plain brace
                    else if (ass) {
                        for (size_t q = 0; (q = t.find(L'\\', q)) != t.npos; q++)
                            if (q + 2 < t.size() && (t[q + 1] == L'i' || t[q + 1] == L'b') && (t[q + 2] == L'0' || t[q + 2] == L'1'))
                                tag(t[q + 1] == L'b', t[q + 2] == L'1');
                    } else {
                        bool close = !t.empty() && t[0] == L'/';
                        if (close) t.remove_prefix(1);
                        if (t.size() == 1 || (t.size() > 1 && (t[1] == L'.' || t[1] == L' ')))
                            if (wchar_t n = (wchar_t)towlower(t[0]); n == L'i' || n == L'b') tag(n == L'b', !close);
                    }
                    if (e != l.npos) {
                        k = e;
                        continue;
                    }
                }
                if (ch == L'&') {  // WebVTT escapes
                    static const std::pair<std::wstring_view, wchar_t> ent[] = {
                        {L"&amp;", L'&'}, {L"&lt;", L'<'}, {L"&gt;", L'>'}, {L"&nbsp;", 0xA0}, {L"&lrm;", 0x200E}, {L"&rlm;", 0x200F}};
                    bool hit = false;
                    for (auto& [name, c2] : ent)
                        if (l.substr(k, name.size()) == name) c.text += c2, k += name.size() - 1, hit = true;
                    if (hit) continue;
                }
                c.text += ch;
            }
        }
        tag(0, false), tag(1, false);
        if (c.text.find_first_not_of(L" \n") != c.text.npos) cues.push_back(std::move(c));
        // `l` is now a blank line (skipped by the outer loop) or the next cue's timing line.
    }
    std::stable_sort(cues.begin(), cues.end(), [](const SubCue& x, const SubCue& y) { return x.start < y.start; });
    return cues;
}

std::vector<SubCue> ParseLyrics(const std::wstring& s) {
    // Bounds (the file chooses how often a line repeats): the first 1M characters, 10,000 cues, 300 characters a line.
    constexpr size_t kChars = 1 << 20, kCues = 10000, kLine = 300;
    std::vector<SubCue> cues;
    for (size_t i = 0, end = std::min(s.size(), kChars); i < end && cues.size() < kCues;) {
        size_t e = std::min(s.find(L'\n', i), end);
        std::wstring_view l = std::wstring_view(s).substr(i, e - i);
        i = e + 1;
        std::vector<double> at;
        for (std::wstring_view p; !l.empty() && l[0] == L'[' && at.size() < kCues;) {
            double t = SubTime(p = l.substr(1));
            if (t < 0 || p.empty() || p[0] != L']') break;  // [ar:...], [offset:...]
            at.push_back(t), l = p.substr(1);
        }
        while (!l.empty() && (l.back() == L'\r' || l.back() == L' ')) l.remove_suffix(1);
        while (!l.empty() && l[0] == L' ') l.remove_prefix(1);
        for (size_t k = 0; k < at.size() && cues.size() < kCues; k++) cues.push_back({at[k], 0, std::wstring(l.substr(0, kLine))});
    }
    std::stable_sort(cues.begin(), cues.end(), [](const SubCue& x, const SubCue& y) { return x.start < y.start; });
    for (size_t k = 0; k < cues.size(); k++) cues[k].end = k + 1 < cues.size() ? cues[k + 1].start : cues[k].start + 10;
    std::erase_if(cues, [](const SubCue& c) { return c.text.empty() || c.end <= c.start; });  // blank lines end the one before
    return cues;
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
    ComPtr<IMFAttributes> attrs;
    ComPtr<IMFMediaType> type, actual;
    if (!StartMediaFoundation() || FAILED(MFCreateAttributes(&attrs, 2)) ||
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

int g_scrubGen;
constexpr double kScrubIdle = -1, kScrubStopped = -2;

struct ScrubJob {
    HWND hwnd;
    std::wstring path;
    UINT w, h;
    int gen;
    std::atomic<double> to{kScrubIdle};
};

std::shared_ptr<ScrubJob> g_scrub;

// Decodes the key frame for the latest drag position whenever it changes; positions passed while a frame
// decodes are skipped, so the picture never falls behind the mouse.
DWORD WINAPI ScrubThread(void* p) {
    std::unique_ptr<std::shared_ptr<ScrubJob>> arg(static_cast<std::shared_ptr<ScrubJob>*>(p));
    auto job = std::move(*arg);
    arg.reset();
    if (job->to == kScrubStopped || FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 0;
    {
        ComPtr<IWICImagingFactory> wic;
        Reader r;
        double done = kScrubIdle;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
            OpenReader(job->path, r))
            for (;;) {
                double to = job->to;
                if (to == kScrubStopped) break;
                if (to == done) {
                    job->to.wait(done);
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
                if (frame && job->to != kScrubStopped) SendMessageW(job->hwnd, WM_FLICK_SCRUB, job->gen, (LPARAM)frame.Get());
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
    MediaScrubStop();
    int gen = ++g_scrubGen;
    auto job = std::make_shared<ScrubJob>(g_hwnd, path, w, h, gen);
    auto* arg = new std::shared_ptr<ScrubJob>(job);
    if (HANDLE thread = CreateThread(nullptr, 0, ScrubThread, arg, 0, nullptr)) {
        g_scrub = std::move(job);
        CloseHandle(thread);
        return gen;
    }
    delete arg;
    return 0;
}

void MediaScrubTo(double seconds) {
    if (!g_scrub || !std::isfinite(seconds) || seconds < 0) return;
    g_scrub->to = std::max(0.0, seconds);  // keep one representation of zero for atomic wait
    g_scrub->to.notify_one();
}

void MediaScrubStop() {
    if (g_scrub) {
        g_scrub->to = kScrubStopped;
        g_scrub->to.notify_one();
        g_scrub.reset();
    }
}
