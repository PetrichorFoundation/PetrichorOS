/* A Win32 program with a window: drawing, mouse, keyboard, a timer. */
#include "win32.h"

static int clicks, seconds, last_key;
static int mx, my;

static LRESULT wndproc(HANDLE h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_CREATE:
        SetTimer(h, 1, 1000, 0);
        return 0;
    case WM_TIMER:
        seconds++;
        InvalidateRect(h, 0, 1);
        return 0;
    case WM_LBUTTONDOWN:
        clicks++;
        mx = (short)(lp & 0xFFFF);
        my = (short)((lp >> 16) & 0xFFFF);
        InvalidateRect(h, 0, 1);
        return 0;
    case WM_CHAR:
        last_key = (int)wp;
        InvalidateRect(h, 0, 1);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HANDLE dc = BeginPaint(h, &ps);
        char buf[96];
        SetBkMode(dc, 1);
        TextOutA(dc, 16, 12, "Hello from a Windows program on Petrichor!", 42);
        int n = wsprintfA(buf, "Seconds: %d    Clicks: %d (last at %d,%d)", seconds, clicks, mx, my);
        TextOutA(dc, 16, 36, buf, n);
        n = wsprintfA(buf, "Last key typed: %c", last_key ? last_key : '-');
        TextOutA(dc, 16, 56, buf, n);
        HANDLE red = CreateSolidBrush(RGB(220, 50, 50)), blue = CreateSolidBrush(RGB(40, 90, 220)), old = SelectObject(dc, red);
        Rectangle(dc, 16, 90, 116, 160);
        SelectObject(dc, blue);
        Ellipse(dc, 136, 90, 236, 160);
        HANDLE pen = CreatePen(0, 3, RGB(0, 150, 60)), oldpen = SelectObject(dc, pen);
        MoveToEx(dc, 256, 90, 0);
        LineTo(dc, 350, 160);
        LineTo(dc, 256, 160);
        LineTo(dc, 256, 90);
        SelectObject(dc, oldpen);
        SelectObject(dc, old);
        DeleteObject(pen);
        DeleteObject(red);
        DeleteObject(blue);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, wp, lp);
}

int main(void)
{
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(0);
    wc.hCursor = LoadCursorA(0, (const char *)32512);
    wc.hbrBackground = GetStockObject(0);
    wc.lpszClassName = "GrapeDemo";
    RegisterClassA(&wc);
    HANDLE w = CreateWindowExA(0, "GrapeDemo", "GrapeJuice demo", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 400, 260, 0, 0, wc.hInstance, 0);
    UpdateWindow(w);
    MSG msg;
    while (GetMessageA(&msg, 0, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return (int)msg.wParam;
}
