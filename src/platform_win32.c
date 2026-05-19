#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "platform_win32.h"

static LRESULT CALLBACK me_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CLOSE:   PostQuitMessage(0); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

HWND me_platform_create_window(const char *title, int w, int h) {
    HINSTANCE hi = GetModuleHandleA(NULL);
    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = me_wndproc;
    wc.hInstance     = hi;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "MultiEmuWnd";
    RegisterClassA(&wc);

    DWORD style = WS_OVERLAPPEDWINDOW;
    RECT r = { 0, 0, w, h };
    AdjustWindowRect(&r, style, FALSE);

    HWND hw = CreateWindowA("MultiEmuWnd", title, style,
                            CW_USEDEFAULT, CW_USEDEFAULT,
                            r.right - r.left, r.bottom - r.top,
                            NULL, NULL, hi, NULL);
    ShowWindow(hw, SW_SHOW);
    return hw;
}

int me_platform_pump(void) {
    MSG m;
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
        if (m.message == WM_QUIT) return 0;
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    return 1;
}
