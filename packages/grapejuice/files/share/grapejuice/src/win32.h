/* The few Win32 declarations the sample GUI programs need (no Windows SDK required). */
typedef void *HANDLE;
typedef unsigned int UINT, DWORD;
typedef int BOOL;
typedef unsigned long long WPARAM;
typedef long long LPARAM, LRESULT;
typedef LRESULT (*WNDPROC)(HANDLE, UINT, WPARAM, LPARAM);
typedef struct { int left, top, right, bottom; } RECT;
typedef struct { int x, y; } POINT;
typedef struct { UINT style; WNDPROC lpfnWndProc; int cbClsExtra, cbWndExtra; HANDLE hInstance, hIcon, hCursor, hbrBackground; const char *lpszMenuName, *lpszClassName; } WNDCLASSA;
typedef struct { HANDLE hwnd; UINT message; WPARAM wParam; LPARAM lParam; DWORD time; POINT pt; DWORD lPrivate; } MSG;
typedef struct { HANDLE hdc; BOOL fErase; RECT rcPaint; BOOL fRestore, fIncUpdate; unsigned char rgb[32]; } PAINTSTRUCT;

#define DECL __declspec(dllimport)
DECL HANDLE GetModuleHandleA(const char *);
DECL void ExitProcess(UINT);
DECL unsigned short RegisterClassA(const WNDCLASSA *);
DECL HANDLE CreateWindowExA(DWORD, const char *, const char *, DWORD, int, int, int, int, HANDLE, HANDLE, HANDLE, void *);
DECL BOOL ShowWindow(HANDLE, int);
DECL BOOL UpdateWindow(HANDLE);
DECL BOOL GetMessageA(MSG *, HANDLE, UINT, UINT);
DECL BOOL TranslateMessage(const MSG *);
DECL LRESULT DispatchMessageA(const MSG *);
DECL void PostQuitMessage(int);
DECL LRESULT DefWindowProcA(HANDLE, UINT, WPARAM, LPARAM);
DECL HANDLE BeginPaint(HANDLE, PAINTSTRUCT *);
DECL BOOL EndPaint(HANDLE, const PAINTSTRUCT *);
DECL BOOL InvalidateRect(HANDLE, const RECT *, BOOL);
DECL unsigned long long SetTimer(HANDLE, unsigned long long, UINT, void *);
DECL BOOL KillTimer(HANDLE, unsigned long long);
DECL HANDLE LoadCursorA(HANDLE, const char *);
DECL BOOL GetClientRect(HANDLE, RECT *);
DECL int MessageBoxA(HANDLE, const char *, const char *, UINT);
DECL BOOL SetWindowTextA(HANDLE, const char *);
DECL int GetWindowTextA(HANDLE, char *, int);
DECL HANDLE GetDlgItem(HANDLE, int);
DECL LRESULT SendMessageA(HANDLE, UINT, WPARAM, LPARAM);
DECL int wsprintfA(char *, const char *, ...);
DECL BOOL TextOutA(HANDLE, int, int, const char *, int);
DECL BOOL Rectangle(HANDLE, int, int, int, int);
DECL BOOL Ellipse(HANDLE, int, int, int, int);
DECL HANDLE CreateSolidBrush(DWORD);
DECL HANDLE CreatePen(int, int, DWORD);
DECL HANDLE SelectObject(HANDLE, HANDLE);
DECL BOOL DeleteObject(HANDLE);
DECL HANDLE GetStockObject(int);
DECL BOOL MoveToEx(HANDLE, int, int, POINT *);
DECL BOOL LineTo(HANDLE, int, int);
DECL DWORD SetTextColor(HANDLE, DWORD);
DECL int SetBkMode(HANDLE, int);
DECL int FillRect(HANDLE, const RECT *, HANDLE);

#define RGB(r, g, b) ((DWORD)((r) | (g) << 8 | (b) << 16))
#define WM_CREATE 1
#define WM_DESTROY 2
#define WM_PAINT 0xF
#define WM_CLOSE 0x10
#define WM_ERASEBKGND 0x14
#define WM_KEYDOWN 0x100
#define WM_CHAR 0x102
#define WM_COMMAND 0x111
#define WM_TIMER 0x113
#define WM_MOUSEMOVE 0x200
#define WM_LBUTTONDOWN 0x201
#define WS_OVERLAPPEDWINDOW 0x00CF0000
#define WS_VISIBLE 0x10000000
#define WS_CHILD 0x40000000
#define BS_AUTOCHECKBOX 3
#define BM_GETCHECK 0xF0
#define CW_USEDEFAULT 0x80000000
#define COLOR_WINDOW 5
