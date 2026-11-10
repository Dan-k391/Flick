#pragma once
#include <windows.h>
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
struct MdColors { D2D1_COLOR_F text, dim, link, codeBg, rule; };
void MdInit(IDWriteFactory*, const wchar_t* uiFont, const wchar_t* monoFont);
void MdLoad(const std::wstring& text);
void MdClear();
float MdPaint(ID2D1RenderTarget*, const D2D1_RECT_F& area, float scroll, const MdColors&);  // -> document height
std::wstring MdOutline();  // --selftest: one "<kind><level><bullet>:<text>|" per block

// ---- main.cpp
std::wstring ExplorerSelection(HWND explorer);  // first selected item's file-system path, or ""
LONGLONG Now();
double MsSince(LONGLONG t0);
void Log(const wchar_t* fmt, ...);
