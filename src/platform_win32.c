#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "platform_win32.h"

/* Edge-detection state: bit set in g_pending the moment a WM_KEYDOWN arrives
   (only if not already considered held), cleared by me_platform_key_pressed
   when the consumer takes the event. Released keys clear g_held so the next
   WM_KEYDOWN counts as a fresh press (Windows auto-repeats WM_KEYDOWN). */
static unsigned char g_pending[256];
static unsigned char g_held[256];

/* Fullscreen + activation state shared with the cursor-visibility logic below. */
static struct {
    int active;
    DWORD style;
    DWORD exstyle;
    RECT  rect;
} g_fs = {0};
static int g_app_active = 1;
static int g_cursor_hidden = 0;

/* ShowCursor maintains an internal counter; only call it when the desired state
   actually differs from what we last set, so we never stack up hides/shows. */
static void me_update_cursor_visibility(void) {
    int should_hide = g_fs.active && g_app_active;
    if (should_hide && !g_cursor_hidden) {
        while (ShowCursor(FALSE) >= 0) {}
        g_cursor_hidden = 1;
    } else if (!should_hide && g_cursor_hidden) {
        while (ShowCursor(TRUE) < 0) {}
        g_cursor_hidden = 0;
    }
}

static LRESULT CALLBACK me_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CLOSE:   PostQuitMessage(0); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        case WM_ACTIVATEAPP:
            g_app_active = wp ? 1 : 0;
            me_update_cursor_visibility();
            break;
        case WM_SETCURSOR:
            /* While fullscreen + active, suppress the class cursor over the client
               area so it stays hidden even as the mouse moves. */
            if (g_fs.active && g_app_active && LOWORD(lp) == HTCLIENT) {
                SetCursor(NULL);
                return TRUE;
            }
            break;
        case WM_ERASEBKGND: return 1; /* we paint every frame, no background erase */
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(h, &ps);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_SYSCHAR: return 0; /* suppress Alt+key bell */
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (wp < 256 && !g_held[wp]) {
                g_held[wp] = 1;
                g_pending[wp] = 1;
            }
            return 0;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (wp < 256) g_held[wp] = 0;
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

int me_platform_key_pressed(unsigned vk, unsigned ctrl, unsigned alt, unsigned shift) {
    if (vk == 0 || vk >= 256) return 0;
    if (!g_pending[vk]) return 0;
    /* Match the chord's modifier set against current state. We consume the
       event even if modifiers mismatch — pressing X without Ctrl shouldn't
       leave a stale "X pressed" event around for the next frame's check. */
    int got_ctrl  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) ? 1 : 0;
    int got_alt   = (GetAsyncKeyState(VK_MENU)    & 0x8000) ? 1 : 0;
    int got_shift = (GetAsyncKeyState(VK_SHIFT)   & 0x8000) ? 1 : 0;
    g_pending[vk] = 0;
    if ((ctrl  && !got_ctrl)  || (!ctrl  && got_ctrl  && (vk != VK_CONTROL && vk != VK_LCONTROL && vk != VK_RCONTROL))) return 0;
    if ((alt   && !got_alt)   || (!alt   && got_alt   && (vk != VK_MENU    && vk != VK_LMENU    && vk != VK_RMENU)))    return 0;
    if ((shift && !got_shift) || (!shift && got_shift && (vk != VK_SHIFT   && vk != VK_LSHIFT   && vk != VK_RSHIFT)))   return 0;
    return 1;
}

void me_platform_request_quit(void) {
    PostQuitMessage(0);
}

/* ---- fullscreen toggle ---------------------------------------------------- */
void me_platform_exit_fullscreen(HWND hwnd) {
    if (!g_fs.active) return;
    SetWindowLongA(hwnd, GWL_STYLE,   (LONG)g_fs.style);
    SetWindowLongA(hwnd, GWL_EXSTYLE, (LONG)g_fs.exstyle);
    SetWindowPos(hwnd, NULL,
                 g_fs.rect.left, g_fs.rect.top,
                 g_fs.rect.right  - g_fs.rect.left,
                 g_fs.rect.bottom - g_fs.rect.top,
                 SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    g_fs.active = 0;
    me_update_cursor_visibility();
}

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
        me_update_cursor_visibility();
    } else {
        SetWindowLongA(hwnd, GWL_STYLE,   (LONG)g_fs.style);
        SetWindowLongA(hwnd, GWL_EXSTYLE, (LONG)g_fs.exstyle);
        SetWindowPos(hwnd, NULL,
                     g_fs.rect.left, g_fs.rect.top,
                     g_fs.rect.right  - g_fs.rect.left,
                     g_fs.rect.bottom - g_fs.rect.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        g_fs.active = 0;
        me_update_cursor_visibility();
    }
}
