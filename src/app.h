#pragma once
#include <windows.h>
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

// Messages posted to the preview window by the keyboard hook thread.
// wParam = Explorer top-level HWND, lParam = QueryPerformanceCounter at key press.
enum : UINT {
    WM_FLICK_TOGGLE = WM_APP + 1,  // Space in Explorer
    WM_FLICK_FOLLOW,               // navigation key released in Explorer while preview open
    WM_FLICK_HIDE,                 // Esc in Explorer while preview open
    WM_FLICK_QUIT,                 // `Flick.exe --exit`
    WM_FLICK_INFO,                 // worker thread finished a thumbnail (lParam = InfoJob*)
    WM_FLICK_PDF,                  // sent by the PDF worker (lParam = PdfMsg*); reply = next page
    WM_FLICK_GRID,                 // sent by the archive worker (wParam = gen, lParam = GridData*)
    WM_FLICK_MEDIA,                // media engine event (wParam = MF_MEDIA_ENGINE_EVENT)
    WM_FLICK_FRAME,                // display refreshed while playing: pull a video frame
    WM_FLICK_SIZE,                 // sent by the folder-size worker (lParam = SizeMsg*)
    WM_FLICK_POSTER,               // sent by the poster worker (wParam = gen, lParam = IWICBitmap* first frame)
};

// ---- lang.cpp: extension -> Lexilla lexer + keywords + style roles
enum Role : unsigned char {
    R_Default, R_Comment, R_Keyword, R_Type, R_Func, R_String, R_Number, R_Preproc,
    R_Operator, R_Tag, R_Attr, R_Heading, R_Strong, R_Em, R_Error, R_Added, R_Deleted, R_Count
};
struct StyleRole { unsigned char style; Role role; };
struct Lang {
    const char* lexer;
    const char* keywords0;
    const char* keywords1;
    const StyleRole* styles;
    size_t styleCount;
};
const Lang* LangForFile(const std::wstring& path);  // nullptr = plain text
const Lang* LangForName(std::wstring name);         // "python", "c++", "sh", ... (code fences, notebooks)

// ---- highlight.cpp: Lexilla styling outside Scintilla, as UTF-16 ranges (R_Default left out)
struct StyleRun { UINT32 start, len; Role role; };
std::vector<StyleRun> Highlight(const std::wstring& code, const Lang* lang);

// ---- window.cpp: the preview window
struct Timing { double sel, load, ready, composed; const wchar_t* kind; };
bool PreviewInit(HINSTANCE);
Timing PreviewShow(const std::wstring& path, HWND source, LONGLONG t0);
void PreviewHide(bool restoreFocus);
bool PreviewVisible();
bool PreviewKey(const MSG&);  // message-loop prefilter for Esc/Space/Enter
extern HWND g_hwnd;

// ---- pdf.cpp: PDF worker. Reply to WM_FLICK_PDF with (pixelWidth << 32 | page + 1), or 0 for "nothing".
struct PdfMsg {
    int gen, page;                                // page -1: opened (sizes empty = failed); -2: idle
    std::vector<std::pair<float, float>> sizes;   // page sizes in points
    Microsoft::WRL::ComPtr<IWICBitmap> bmp;       // rendered page (null if it failed)
    UINT32 pageCount = 0;                         // with page -1
};
void PdfOpen(const std::wstring& path, int gen);  // "" closes the document
void PdfWake();                                   // worker idle and the UI needs a page

// ---- markdown.cpp: parsed into blocks, laid out lazily with DirectWrite
struct MdColors {
    D2D1_COLOR_F text, dim, link, codeBg, rule;
    const COLORREF* roles = nullptr;  // syntax colours per Role (code blocks)
};
void MdInit(IDWriteFactory*, const wchar_t* uiFont, const wchar_t* monoFont);
void MdLoad(const std::wstring& text, const std::wstring& baseDir = L"");
void MdClear();
float MdPaint(ID2D1RenderTarget*, const D2D1_RECT_F& area, float scroll, const MdColors&);  // -> document height
std::wstring MdOutline();
size_t MdHeadingCount();  // H1-H3, for the contents sidebar
const std::wstring& MdHeadingText(size_t i);
int MdHeadingLevel(size_t i);
float MdHeadingY(size_t i, bool layout);  // -1: not laid out yet (and layout == false)  // --selftest: one "<kind><level><bullet>:<text>|" per block

// ---- grid.cpp: table view for CSV/TSV and archive listings
struct GridData {
    std::wstring text;                             // all cell text
    std::vector<std::pair<UINT32, UINT32>> cells;  // (offset, length) into text, row-major
    UINT32 cols = 0;
    bool header = false;                           // row 0 is a header
    std::vector<bool> rightAlign;                  // per column
    std::vector<UINT8> indent;                     // archives, per body row: tree depth
    std::vector<bool> folder;                      // archives, per body row
    std::vector<std::wstring> paths;               // trees, per body row: path under the root, '/'-separated
    std::wstring sub;                              // title-bar details
    size_t Rows() const { return cols ? cells.size() / cols : 0; }
};
void GridInit(IDWriteFactory*, const wchar_t* uiFont, const wchar_t* iconFont);
std::unique_ptr<GridData> GridParseCsv(const std::wstring& text, wchar_t sep);  // sep 0: guess
// Worker threads; they stop early once `gen` moves on from `mine` (a newer preview started).
std::unique_ptr<GridData> GridReadArchive(const std::wstring& path, const std::atomic<int>& gen, int mine);
std::unique_ptr<GridData> GridReadFolder(const std::wstring& path, const std::atomic<int>& gen, int mine);
bool ArchiveReadRaw(const std::wstring& path, std::string& out, size_t cap);   // gunzip & co. (svgz)
void GridSet(std::unique_ptr<GridData>);  // nullptr clears
const GridData* GridGet();
void GridSetCell(size_t bodyRow, size_t col, const std::wstring& value);
bool GridClick(const D2D1_RECT_F& area, float scrollY, D2D1_POINT_2F pt);  // toggles a folder row
size_t GridShownRows();  // body rows visible (--selftest)
D2D1_SIZE_F GridPaint(ID2D1RenderTarget*, const D2D1_RECT_F& area, float scrollX, float scrollY, const MdColors&);

// ---- font.cpp: font specimen (ttf, otf, ttc, woff, woff2)
bool FontLoad(IDWriteFactory*, const std::wstring& path, std::wstring& sub);
void FontClear();
float FontPaint(ID2D1RenderTarget*, const D2D1_RECT_F& area, float scroll, const MdColors&);  // -> height

// ---- svg.cpp: SVG/SVGZ rendered to a bitmap fitting maxW x maxH pixels; w, h = intrinsic size
Microsoft::WRL::ComPtr<IWICBitmap> SvgRender(const std::wstring& path, float maxW, float maxH, IWICImagingFactory*,
                                             UINT& w, UINT& h);

// ---- fileinfo.cpp: info-card lines for programs (exe, dll, sys, ...) and installers (msi)
std::vector<std::wstring> ProgramInfo(const std::wstring& path);

// ---- convert.cpp: other formats as Markdown text ("" if not parseable)
std::wstring NotebookToMarkdown(const std::wstring& json);
std::wstring AsciiDocToMarkdown(const std::wstring& text);
std::wstring RstToMarkdown(const std::wstring& text);

// ---- media.cpp: audio/video playback (Media Foundation, CPU decoding)
struct MediaState {
    bool ready, paused, ended, error;
    double pos, dur;      // seconds
    UINT videoW, videoH;  // display size of the video (aspect-corrected); 0 for audio
};
bool MediaOpen(const std::wstring& path);  // starts playing; false if Media Foundation is unavailable
void MediaClose();
MediaState MediaGet();
bool MediaFrame(UINT w, UINT h, IWICImagingFactory*, Microsoft::WRL::ComPtr<IWICBitmap>& out);
void MediaFrameHandled();
void MediaOnEvent(DWORD event);
void MediaToggle();
void MediaSeek(double seconds, bool fast = false);  // fast: nearest key frame (while dragging)
bool MediaSeeking();
void MediaMute(bool muted);  // for tests
void MediaSetVolume(double v);  // 0..1, kept for the session
double MediaVolume();           // 0 while muted
void MediaToggleMute();
Microsoft::WRL::ComPtr<IWICBitmap> MediaFirstFrame(const std::wstring& path, IWICImagingFactory*);

// window.cpp helpers for the views
Microsoft::WRL::ComPtr<ID2D1Bitmap> SharedBitmap(IWICBitmapSource*);  // at window DPI, sharing WIC memory
IWICImagingFactory* WicFactory();

// ---- main.cpp
std::wstring ExplorerSelection(HWND explorer);  // first selected item's file-system path, or ""
LONGLONG Now();
double MsSince(LONGLONG t0);
void Log(const wchar_t* fmt, ...);
