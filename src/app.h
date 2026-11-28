#pragma once
#include <windows.h>
#include <array>
#include <atomic>
#include <functional>
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
    WM_FLICK_SCRUB,                // sent by the scrub worker (wParam = gen, lParam = IWICBitmap* frame)
    WM_FLICK_POSTER,               // sent by the poster worker (wParam = gen, lParam = IWICBitmap* first frame)
    WM_FLICK_FIND,                 // Ctrl+F in Explorer while a text preview is open
    WM_FLICK_SUBS,                 // sent by the poster worker (wParam = gen, lParam = std::vector<SubCue>*)
    WM_FLICK_SHOT,                 // scripts/shot.ps1: save the frame as Paint draws it to %FLICK_SHOT% (PNG)
    WM_FLICK_WORD,                 // posted by the docx picture worker (wParam = gen, lParam = its message)
    WM_FLICK_SELECTALL,            // scripts/shot.ps1: Ctrl+A (WM_COPY: Ctrl+C)
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
// The same, lexed lazily: a long code block shown in parts lexes only as far as a part needs (continuing where it
// stopped, so a string or comment open across a part's border keeps its colour). Ranges start at line starts.
struct Lexing;
std::shared_ptr<Lexing> LexBegin(const std::wstring& code, const Lang* lang);  // nullptr: nothing to colour
std::vector<StyleRun> LexRange(Lexing&, UINT32 from, UINT32 to);  // runs in [from, to), relative to from

// ---- window.cpp: the preview window
struct Timing { double sel, load, ready, composed; const wchar_t* kind; };
bool PreviewInit(HINSTANCE);
Timing PreviewShow(const std::wstring& path, HWND source, LONGLONG t0);
void PreviewHide(bool restoreFocus);
bool PreviewVisible();
bool PreviewCanFind();  // a text preview is showing (Ctrl+F opens its search field)
bool PreviewKey(const MSG&);  // message-loop prefilter for Esc/Space/Enter
extern HWND g_hwnd;

// ---- pdf.cpp: PDF worker. Reply to WM_FLICK_PDF with (pixelWidth << 32 | page + 1), plus
// kPdfThumb for a sidebar thumbnail, or 0 for "nothing".
constexpr LRESULT kPdfThumb = 0x80000000;
struct SlideText {  // pptx: a paragraph as drawn on its slide
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;  // without drawing effects (no worker brushes): UI thread
    D2D1_MATRIX_3X2_F toPage;                          // layout coordinates -> page points
    std::wstring text;
};
struct PdfMsg {
    int gen, page;                                // page -1: opened (sizes empty = failed); -2: idle
    std::vector<std::pair<float, float>> sizes;   // page sizes in points
    Microsoft::WRL::ComPtr<IWICBitmap> bmp;       // rendered page (null if it failed)
    UINT32 pageCount = 0;                         // with page -1
    std::vector<SlideText> text;                  // pptx: the page's text, for selection
};
void PdfOpen(const std::wstring& path, int gen);  // "" closes the document
void PdfWake();                                   // worker idle and the UI needs a page

// ---- pages.cpp: pptx slides and docx pages drawn for the PDF view (on its worker thread)
struct Pages;
struct PagesFree {
    void operator()(Pages*) const;
};
using PagesPtr = std::unique_ptr<Pages, PagesFree>;
PagesPtr PagesOpen(const std::wstring& path);                   // nullptr: not a readable deck
std::vector<std::pair<float, float>> PagesSizes(const Pages&);  // points
Microsoft::WRL::ComPtr<IWICBitmap> PagesRender(Pages&, int index, UINT width, ID2D1Factory*, IWICImagingFactory*,
                                               std::vector<SlideText>* text = nullptr);
bool PagesFor(const std::wstring& path);                        // a file pages.cpp draws (pptx)

// ---- markdown.cpp: parsed into blocks, laid out lazily with DirectWrite
struct MdColors {
    D2D1_COLOR_F text, dim, link, codeBg, rule;
    const COLORREF* roles = nullptr;  // syntax colours per Role (code blocks)
    D2D1_COLOR_F sel{};               // selected text's background
};

// ---- text selection (word.cpp; docx, Markdown, pptx slides): positions are (paragraph, UTF-16 index), paragraphs
// numbered in document order. The unit first clicked (1 character, 2 word, 3 paragraph) is what a drag extends by.
struct TextSel {
    using Pos = std::pair<UINT64, UINT32>;
    Pos a, b, ua, ub;  // anchor, focus; the unit clicked
    bool all = false;  // Ctrl+A: everything, laid out or not
    int unit = 1;      // what the click that started it selected
    bool Empty() const { return !all && a == b; }
    void Click(Pos from, Pos to) { ua = a = from, ub = b = to, all = false; }
    void Drag(Pos from, Pos to) {
        if (to <= ua) a = ub, b = from;
        else a = ua, b = to;
    }
    bool Span(UINT64 para, UINT32 len, UINT32& from, UINT32& to) const;  // the part of `para` selected
};
UINT32 TextHit(IDWriteTextLayout*, float x, float y);  // caret position nearest the point (layout coordinates)
void TextUnit(const std::wstring& s, UINT32 i, int unit, UINT32& from, UINT32& to);  // the word/paragraph at i
void FillText(ID2D1RenderTarget*, IDWriteTextLayout*, UINT32 from, UINT32 to, D2D1_POINT_2F at, ID2D1Brush*);
// Copied text: a separator goes in only between pieces (the last one asked for wins: a cell's tab over a paragraph's CRLF).
struct CopyText {
    std::wstring s;
    const wchar_t* sep = nullptr;
    bool started = false;
    void Put(std::wstring_view v) {
        if (started && sep) s += sep;
        s += v, started = true, sep = nullptr;
    }
    void Sep(const wchar_t* p) {
        if (started) sep = p;
    }
};
void MdInit(IDWriteFactory*, const wchar_t* uiFont, const wchar_t* monoFont);
void MdLoad(const std::wstring& text, const std::wstring& baseDir = L"");
void MdClear();
float MdPaint(ID2D1RenderTarget*, const D2D1_RECT_F& area, float scroll, const MdColors&);  // -> document height
std::wstring MdOutline();
size_t MdHeadingCount();  // H1-H3, for the contents sidebar
// Selection, as WordSelect (window DIPs, the last paint's area). Copy: CRLF between blocks, tabs between table cells,
// code as written, list items with a plain "- " / "1. " marker, images as nothing.
bool MdSelect(D2D1_POINT_2F pt, float scroll, int clicks);
void MdSelectAll();
std::wstring MdCopy();
std::wstring MdCopy(const TextSel&);  // --selftest: a given selection (paragraph = block << 32 | table cell)
const std::wstring& MdHeadingText(size_t i);
int MdHeadingLevel(size_t i);
float MdHeadingY(size_t i, bool layout);  // -1: not laid out yet (and layout == false)  // --selftest: one "<kind><level><bullet>:<text>|" per block

// ---- word.cpp: docx as one continuous column (reading view), parsed and laid out as far as it is scrolled
bool WordLoad(const std::wstring& path, std::wstring& sub);
void WordClear();
float WordColumn();  // the document's text width at 100%, DIPs
float WordPaint(ID2D1RenderTarget*, const D2D1_RECT_F& area, float scroll, const MdColors&);  // -> document height
bool WordPicture(WPARAM gen, LPARAM msg);  // WM_FLICK_WORD: true = repaint
// Selection, in window DIPs against the last paint's area (scrolled to `scroll`). clicks 1-3: a click (character,
// word, paragraph), 0: dragged there; false: a click hit no text (nothing to drag from).
// Copy: plain text (CRLF between paragraphs, tabs between table cells).
bool WordSelect(D2D1_POINT_2F pt, float scroll, int clicks);
void WordSelectAll();
std::wstring WordCopy();
std::wstring WordSelfTest();  // "" = ok

// ---- grid.cpp: table view for CSV/TSV and archive listings
// Sheets: an Excel cell format, resolved. Colours 0xFFRRGGBB; 0 = automatic (text, borders) / none (fill).
struct SheetStyle {
    std::wstring font = L"Calibri";
    float size = 11 * 96 / 72.f;  // DIPs
    bool bold = false, italic = false, underline = false, strike = false, wrap = false;
    UINT32 color = 0, fill = 0;
    UINT32 border[4] = {};        // left, top, right, bottom
    UINT8 line[4] = {};           // 0 none, 1 thin, 2 medium, 3 thick, 4 double, 5 dashed/dotted
    UINT8 h = 0, v = 2, indent = 0;  // h: 0 general, 1 left, 2 centre, 3 right; v: 0 top, 1 centre, 2 bottom
};
struct SheetPic {
    D2D1_RECT_F rc;                           // DIPs from A1's top-left corner
    Microsoft::WRL::ComPtr<IWICBitmap> wic;   // decoded at drawn size; null: chart or undecodable (placeholder)
};
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
    bool sheet = false;                            // spreadsheet: A B C / 1 2 3 headers, gridlines, tabs
    std::vector<bool> num;                         // sheets, per cell: right-aligned (numbers, dates)
    std::vector<std::wstring> tabs;                // sheets: names
    int tab = 0;                                   // sheets: the one shown
    std::vector<SheetStyle> styles = std::vector<SheetStyle>(1);  // sheets: cell formats
    std::vector<UINT16> xf;                        // sheets, per cell: index into styles
    std::vector<float> colW, rowH;                 // sheets: DIPs (0 = hidden); empty: from the text / default
    float defColW = 64, defRowH = 20;
    std::vector<std::array<UINT32, 4>> merges;     // sheets: first row, first column, last row, last column
    std::vector<SheetPic> pics;
    UINT32 frozenRows = 0, frozenCols = 0;
    bool gridLines = true;
    bool more = false;  // sheets: first rows only, or pictures to decode: OfficeToGrid on a worker completes it
    size_t Rows() const { return cols ? cells.size() / cols : 0; }
};
void GridInit(IDWriteFactory*, const wchar_t* uiFont, const wchar_t* iconFont);
std::unique_ptr<GridData> GridParseCsv(const std::wstring& text, wchar_t sep);  // sep 0: guess
// Worker threads; they stop early once `gen` moves on from `mine` (a newer preview started).
std::unique_ptr<GridData> GridReadArchive(const std::wstring& path, const std::atomic<int>& gen, int mine);
std::unique_ptr<GridData> GridReadFolder(const std::wstring& path, const std::atomic<int>& gen, int mine);
bool ArchiveReadRaw(const std::wstring& path, std::string& out, size_t cap);   // gunzip & co. (svgz)
bool ArchiveReadMembers(const std::wstring& path, const std::function<size_t(std::wstring_view)>& want,
                        std::vector<std::pair<std::wstring, std::string>>& out);  // want: byte limit, 0 = skip
void GridSet(std::unique_ptr<GridData>);  // nullptr clears
const GridData* GridGet();
void GridSetCell(size_t bodyRow, size_t col, const std::wstring& value);
bool GridClick(const D2D1_RECT_F& area, float scrollY, D2D1_POINT_2F pt);  // toggles a folder row
int GridTabAt(D2D1_POINT_2F pt);  // sheets: tab under the point, -1 none, -2 a tab-strip arrow (scrolled)
bool GridTabWheel(D2D1_POINT_2F pt, float delta);  // sheets: wheel over the tab strip scrolls it
// An explicit text/line colour (0xFFRRGGBB) on the dark window: 0 when near-black (draw it in the theme's
// text colour), a dark colour with its lightness mirrored and hue kept (navy -> light blue), else as it is.
UINT32 DarkInk(UINT32 c);
float GridDigitWidth(const std::wstring& font, float size);  // widest digit in whole DIPs (Excel's column unit); any thread
size_t GridShownRows();  // body rows visible (--selftest)
// Sheets and CSV (not trees): a cell range, selected as in WordSelect (clicks 0: dragged there); a column letter,
// row number or the corner selects columns, rows, all. Copy: cells as shown, tab-separated rows ending in CRLF.
bool GridSelect(D2D1_POINT_2F pt, float scrollX, float scrollY, int clicks);  // false: not on a cell or header
D2D1_RECT_F GridCells();  // the cells' area as last painted (window DIPs): headers, tab strip outside
void GridSelectAll();
std::wstring GridCopy();
std::wstring GridRangeText(const GridData&, size_t r0, size_t c0, size_t r1, size_t c1);  // inclusive, clamped
D2D1_SIZE_F GridPaint(ID2D1RenderTarget*, const D2D1_RECT_F& area, float scrollX, float scrollY, const MdColors&);

// ---- font.cpp: font specimen (ttf, otf, ttc, woff, woff2)
bool FontLoad(IDWriteFactory*, const std::wstring& path, std::wstring& sub);
void FontClear();
float FontPaint(ID2D1RenderTarget*, const D2D1_RECT_F& area, float scroll, const MdColors&);  // -> height

// ---- svg.cpp: SVG/SVGZ rendered to a bitmap fitting maxW x maxH pixels; w, h = intrinsic size
Microsoft::WRL::ComPtr<IWICBitmap> SvgRender(const std::wstring& path, float maxW, float maxH, IWICImagingFactory*,
                                             UINT& w, UINT& h);

// ---- fileinfo.cpp: info-card lines for programs (exe, dll, sys, ...), installers (msi) and certificates (cer, pem,
// p7b, ...: none for a file that isn't one or holds a private key); sniff: also ELF/Mach-O by their first bytes
std::vector<std::wstring> ProgramInfo(const std::wstring& path, bool sniff);
std::wstring FileInfoSelfTest();  // "" = ok

// ---- imgfmt.cpp: pictures WIC can't open by name, as WIC sources (null: not readable). TGA, QOI, PNM/PAM, HDR, PCX
// decoded there; CUR/ANI (first frame); the preview PNG inside sketch/xmind/kra/ora zips.
extern const wchar_t kImgOwnExt[];
Microsoft::WRL::ComPtr<IWICBitmapSource> ImgLoad(const std::wstring& path);
std::wstring ImgSelfTest();  // "" = ok
UINT TiffOrientation(const std::wstring& path);  // EXIF-style 1-8 from a TIFF-based file's first IFD (camera raw); 1 if none

// ---- convert.cpp: other formats as Markdown text ("" if not parseable)
std::wstring NotebookToMarkdown(const std::wstring& json);
std::wstring AsciiDocToMarkdown(const std::wstring& text);
std::wstring RstToMarkdown(const std::wstring& text);

// ---- office.cpp: Word/PowerPoint documents as Markdown, Excel/Calc sheets as CSV ("" if not readable);
// sub = title-bar details
std::wstring OfficeToMarkdown(const std::wstring& path, std::wstring& sub);
// sheet -1: the active one. picScale 0: the open path (first rows, no pictures; `more` asks for a worker that
// calls again with the window's pixels per DIP; it gives up once `gen` moves on from `mine`).
std::unique_ptr<GridData> OfficeToGrid(const std::wstring& path, int sheet, float picScale = 0,
                                       const std::atomic<int>* gen = nullptr, int mine = 0);
std::wstring OfficeSelfTest();  // "" = ok
// Office font names are GDI face names ("Calibri Light", "Arial Narrow", "等线 Light"), not DirectWrite families:
// the family, weight and stretch they stand for (unknown fonts: the name as is). Cached per name; any thread.
struct FontFace {
    std::wstring family;
    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
    DWRITE_FONT_STRETCH stretch = DWRITE_FONT_STRETCH_NORMAL;
};
FontFace ResolveFont(const std::wstring& face);
// A picture (PNG, JPEG, ...) decoded and scaled to w x h pixels; null if WIC can't read it. Any thread.
Microsoft::WRL::ComPtr<IWICBitmap> DecodePicture(std::string_view bytes, UINT w, UINT h);
bool HasExt(const std::wstring& path, const wchar_t* list);  // list: " .a .b ", lower case
inline D2D1_COLOR_F Rgb(UINT32 c, float a = 1) { return {(c >> 16 & 255) / 255.f, (c >> 8 & 255) / 255.f, (c & 255) / 255.f, a}; }

// ---- media.cpp: audio/video playback (Media Foundation, CPU decoding)
struct MediaState {
    bool ready, paused, ended, error;
    double pos, dur;      // seconds
    UINT videoW, videoH;  // display size of the video (aspect-corrected); 0 for audio
};
bool MediaOpen(const std::wstring& path, bool start = true);  // false if Media Foundation is unavailable
bool MediaStart();  // after MediaOpen(path, false): starts loading and playing
void MediaClose();
MediaState MediaGet();
bool MediaFrame(UINT w, UINT h, IWICImagingFactory*, Microsoft::WRL::ComPtr<IWICBitmap>& out);
void MediaFrameHandled();
void MediaOnEvent(DWORD event);
void MediaToggle();
void MediaSeek(double seconds);
void MediaMute(bool muted);  // for tests
void MediaSetVolume(double v);  // 0..1, kept for the session
double MediaVolume();           // 0 while muted
void MediaToggleMute();
Microsoft::WRL::ComPtr<IWICBitmap> MediaFirstFrame(const std::wstring& path, IWICImagingFactory*);
int MediaScrubStart(const std::wstring& path, UINT w, UINT h);  // frames while the seek bar is dragged; returns gen
void MediaScrubTo(double seconds);
void MediaScrubStop();
// Subtitles (SRT, WebVTT): cues sorted by start, text without tags; italic/bold as (start, length) ranges.
struct SubCue {
    double start, end;
    std::wstring text;
    std::vector<std::pair<UINT32, UINT32>> italic, bold;
};
std::vector<SubCue> ParseSubtitles(const std::wstring& s);
// LRC lyrics ("[01:02.50][01:30]words"): a line lasts until the next one starts; [ar:..] tags are skipped.
std::vector<SubCue> ParseLyrics(const std::wstring& s);

// window.cpp helpers for the views
Microsoft::WRL::ComPtr<ID2D1Bitmap> SharedBitmap(IWICBitmapSource*);  // at window DPI, sharing WIC memory
IWICImagingFactory* WicFactory();

// ---- main.cpp
std::wstring ExplorerSelection(HWND explorer);  // first selected item's file-system path, or ""
LONGLONG Now();
double MsSince(LONGLONG t0);
void Log(const wchar_t* fmt, ...);
