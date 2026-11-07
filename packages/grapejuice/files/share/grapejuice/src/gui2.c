/* A Win32 program with controls: a button, a label, an edit box, a check box and a message box. */
#include "win32.h"

#define ID_COUNT 101
#define ID_GREET 102
#define ID_EDIT 103
#define ID_LABEL 104
#define ID_CHECK 105

static int count;

static LRESULT wndproc(HANDLE h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_CREATE:
        CreateWindowExA(0, "STATIC", "Press the button, or type a name below.", WS_CHILD | WS_VISIBLE, 16, 16, 300, 20, h, (HANDLE)(unsigned long long)ID_LABEL, 0, 0);
        CreateWindowExA(0, "BUTTON", "Count clicks", WS_CHILD | WS_VISIBLE, 16, 48, 120, 28, h, (HANDLE)(unsigned long long)ID_COUNT, 0, 0);
        CreateWindowExA(0, "EDIT", "world", WS_CHILD | WS_VISIBLE, 16, 96, 200, 24, h, (HANDLE)(unsigned long long)ID_EDIT, 0, 0);
        CreateWindowExA(0, "BUTTON", "Greet", WS_CHILD | WS_VISIBLE, 230, 94, 80, 28, h, (HANDLE)(unsigned long long)ID_GREET, 0, 0);
        CreateWindowExA(0, "BUTTON", "Shout", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 16, 136, 120, 20, h, (HANDLE)(unsigned long long)ID_CHECK, 0, 0);
        return 0;
    case WM_COMMAND: {
        int id = (int)(wp & 0xFFFF);
        if (id == ID_COUNT) {
            char buf[64];
            wsprintfA(buf, "Button pressed %d time(s).", ++count);
            SetWindowTextA(GetDlgItem(h, ID_LABEL), buf);
        } else if (id == ID_GREET) {
            char name[64], msg[128];
            GetWindowTextA(GetDlgItem(h, ID_EDIT), name, sizeof name);
            int shout = (int)SendMessageA(GetDlgItem(h, ID_CHECK), BM_GETCHECK, 0, 0);
            wsprintfA(msg, shout ? "HELLO, %s!" : "Hello, %s!", name);
            MessageBoxA(h, msg, "Greeting", 0);
        }
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
    wc.hbrBackground = (HANDLE)(unsigned long long)(COLOR_WINDOW + 1);
    wc.lpszClassName = "GrapeControls";
    RegisterClassA(&wc);
    HANDLE w = CreateWindowExA(0, "GrapeControls", "GrapeJuice controls", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 360, 260, 0, 0, wc.hInstance, 0);
    UpdateWindow(w);
    MSG msg;
    while (GetMessageA(&msg, 0, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return (int)msg.wParam;
}
