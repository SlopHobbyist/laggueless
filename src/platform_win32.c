#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "platform_win32.h"

volatile int me_platform_f11_pressed = 0;

static LRESULT CALLBACK me_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CLOSE:   PostQuitMessage(0); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        case WM_ERASEBKGND: return 1; /* we paint every frame, no background erase */
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(h, &ps);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) PostQuitMessage(0);
            else if (wp == VK_F11) me_platform_f11_pressed = 1;
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
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
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

/* ---- fullscreen toggle ---------------------------------------------------- */
static struct {
    int active;
    DWORD style;
    DWORD exstyle;
    RECT  rect;
} g_fs = {0};

void me_platform_toggle_fullscreen(HWND hwnd) {
    if (!g_fs.active) {
        g_fs.style   = (DWORD)GetWindowLongA(hwnd, GWL_STYLE);
        g_fs.exstyle = (DWORD)GetWindowLongA(hwnd, GWL_EXSTYLE);
        GetWindowRect(hwnd, &g_fs.rect);

        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);

        SetWindowLongA(hwnd, GWL_STYLE,   (LONG)(g_fs.style & ~(WS_OVERLAPPEDWINDOW)));
        SetWindowLongA(hwnd, GWL_EXSTYLE, (LONG)(g_fs.exstyle & ~(WS_EX_DLGMODALFRAME|WS_EX_WINDOWEDGE|WS_EX_CLIENTEDGE|WS_EX_STATICEDGE)));
        SetWindowPos(hwnd, HWND_TOP,
                     mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        g_fs.active = 1;
    } else {
        SetWindowLongA(hwnd, GWL_STYLE,   (LONG)g_fs.style);
        SetWindowLongA(hwnd, GWL_EXSTYLE, (LONG)g_fs.exstyle);
        SetWindowPos(hwnd, NULL,
                     g_fs.rect.left, g_fs.rect.top,
                     g_fs.rect.right  - g_fs.rect.left,
                     g_fs.rect.bottom - g_fs.rect.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        g_fs.active = 0;
    }
}
