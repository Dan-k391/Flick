#pragma once
#include <windows.h>
#include <string>

// Messages posted to the preview window by the keyboard hook thread.
// wParam = Explorer top-level HWND, lParam = QueryPerformanceCounter at key press.
enum : UINT {
    WM_FLICK_TOGGLE = WM_APP + 1,  // Space in Explorer
    WM_FLICK_FOLLOW,               // navigation key released in Explorer while preview open
    WM_FLICK_HIDE,                 // Esc in Explorer while preview open
    WM_FLICK_QUIT,                 // `Flick.exe --exit`
    WM_FLICK_INFO,                 // worker thread finished a thumbnail (lParam = InfoJob*)
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

// ---- main.cpp
std::wstring ExplorerSelection(HWND explorer);  // first selected item's file-system path, or ""
LONGLONG Now();
double MsSince(LONGLONG t0);
void Log(const wchar_t* fmt, ...);
