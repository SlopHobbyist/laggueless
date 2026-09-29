#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <stdio.h>
#include "platform_win32.h"
#include "app.h"
#include "ui.h"

/* Private messages to the UI thread. */
#define WM_ME_FULLSCREEN (WM_APP + 1)   /* wp: 0 = toggle, 1 = exit */
#define WM_ME_DESTROY    (WM_APP + 2)

/* Edge-detection state: set in g_pending the moment a WM_KEYDOWN arrives
   (only if not already considered held), cleared by me_platform_key_pressed
   when the consumer takes the event. Released keys clear g_held so the next
   WM_KEYDOWN counts as a fresh press (Windows auto-repeats WM_KEYDOWN).
   Written by the UI thread, consumed by the emulation thread. */
static volatile LONG g_pending[256];
static unsigned char g_held[256];

/* Fullscreen + activation state shared with the cursor-visibility logic below.
   UI thread only, except `active` which others read. */
static struct {
    volatile LONG active;
    DWORD style;
    DWORD exstyle;
    RECT  rect;
} g_fs = {0};
static int g_app_active = 1;
static int g_cursor_hidden = 0;
static volatile LONG g_idle = 0;

static HWND   g_hwnd;
static HMENU  g_menu;
static HANDLE g_ui_thread;

/* ShowCursor maintains an internal counter; only call it when the desired state
   actually differs from what we last set, so we never stack up hides/shows.
   The cursor belongs to the window's thread, so this runs on the UI thread. */
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

/* ---- fullscreen (UI thread) ----------------------------------------------- */
static void fs_exit(HWND hwnd) {
    if (!g_fs.active) return;
    SetMenu(hwnd, g_menu);
    SetWindowLongA(hwnd, GWL_STYLE,   (LONG)g_fs.style);
    SetWindowLongA(hwnd, GWL_EXSTYLE, (LONG)g_fs.exstyle);
    SetWindowPos(hwnd, NULL,
                 g_fs.rect.left, g_fs.rect.top,
                 g_fs.rect.right  - g_fs.rect.left,
                 g_fs.rect.bottom - g_fs.rect.top,
                 SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    InterlockedExchange(&g_fs.active, 0);
    me_update_cursor_visibility();
}

static void fs_enter(HWND hwnd) {
    if (g_fs.active) return;
    g_fs.style   = (DWORD)GetWindowLongA(hwnd, GWL_STYLE);
    g_fs.exstyle = (DWORD)GetWindowLongA(hwnd, GWL_EXSTYLE);
    GetWindowRect(hwnd, &g_fs.rect);

    MONITORINFO mi = { .cbSize = sizeof(mi) };
    GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);

    SetMenu(hwnd, NULL);
    SetWindowLongA(hwnd, GWL_STYLE,   (LONG)(g_fs.style & ~(WS_OVERLAPPEDWINDOW)));
    SetWindowLongA(hwnd, GWL_EXSTYLE, (LONG)(g_fs.exstyle & ~(WS_EX_DLGMODALFRAME|WS_EX_WINDOWEDGE|WS_EX_CLIENTEDGE|WS_EX_STATICEDGE)));
    SetWindowPos(hwnd, HWND_TOP,
                 mi.rcMonitor.left, mi.rcMonitor.top,
                 mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top,
                 SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    InterlockedExchange(&g_fs.active, 1);
    me_update_cursor_visibility();
}

static LRESULT CALLBACK me_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    int handled = 0;
    LRESULT r = me_ui_handle(h, msg, wp, lp, &handled);
    if (handled) return r;

    switch (msg) {
        /* Closing only asks the emulation thread to stop; it tears the game
           down (it may be presenting to this window) and then destroys us. */
        case WM_CLOSE:   me_app_request_quit(); return 0;
        case WM_ME_DESTROY: DestroyWindow(h); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        case WM_ME_FULLSCREEN:
            if (wp == 1)             fs_exit(h);
            else if (g_fs.active)    fs_exit(h);
            else                     fs_enter(h);
            return 0;
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
        case WM_ERASEBKGND:
            if (g_idle) break;  /* default erase fills with the black class brush */
            return 1;           /* we paint every frame, no background erase */
        case WM_DROPFILES: {
            HDROP drop = (HDROP)wp;
            char path[MAX_PATH];
            if (DragQueryFileA(drop, 0, path, sizeof(path)) > 0)
                me_cmd_post(ME_CMD_LOAD_ROM, 0, path);
            DragFinish(drop);
            /* Take focus so the game gets input right away (input is ignored
               while another window is foreground). May be refused by the OS
               foreground lock, in which case the taskbar button flashes. */
            SetForegroundWindow(h);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(h, &ps);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_SYSCHAR: return 0; /* suppress Alt+key bell */
        /* Keys never reach DefWindowProc: Alt/F10 must not open the menu bar
           mid-game (the menu is mouse-only). */
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (wp < 256 && !g_held[wp]) {
                g_held[wp] = 1;
                InterlockedExchange(&g_pending[wp], 1);
            }
            return 0;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (wp < 256) g_held[wp] = 0;
            return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

typedef struct {
    const char *title;
    int w, h;
    HANDLE ready;
} me_ui_start;

static DWORD WINAPI ui_thread_main(LPVOID arg) {
    me_ui_start *st = (me_ui_start *)arg;
    /* STA for the common file dialog. */
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    HINSTANCE hi = GetModuleHandleA(NULL);
    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = me_wndproc;
    wc.hInstance     = hi;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "MultiEmuWnd";
    RegisterClassA(&wc);

    g_menu = me_ui_create_menu();
    DWORD style = WS_OVERLAPPEDWINDOW;
    RECT r = { 0, 0, st->w, st->h };
    AdjustWindowRect(&r, style, TRUE);

    g_hwnd = CreateWindowA("MultiEmuWnd", st->title, style,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top,
                           NULL, g_menu, hi, NULL);
    if (g_hwnd) {
        DragAcceptFiles(g_hwnd, TRUE);
        ShowWindow(g_hwnd, SW_SHOW);
    }
    SetEvent(st->ready);  /* st is dead after this */

    if (g_hwnd) {
        MSG m;
        while (GetMessageA(&m, NULL, 0, 0) > 0) {
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
    }
    CoUninitialize();
    return 0;
}

HWND me_platform_create_window(const char *title, int w, int h) {
    me_ui_start st = { title, w, h, CreateEventA(NULL, TRUE, FALSE, NULL) };
    if (!st.ready) return NULL;
    g_ui_thread = CreateThread(NULL, 0, ui_thread_main, &st, 0, NULL);
    if (g_ui_thread) WaitForSingleObject(st.ready, INFINITE);
    CloseHandle(st.ready);
    return g_hwnd;
}

void me_platform_destroy_window(void) {
    if (!g_ui_thread) return;
    if (g_hwnd) PostMessageA(g_hwnd, WM_ME_DESTROY, 0, 0);
    WaitForSingleObject(g_ui_thread, 2000);
    CloseHandle(g_ui_thread);
    g_ui_thread = NULL;
}

HWND me_platform_hwnd(void) { return g_hwnd; }

int me_platform_pump(void) {
    MSG m;
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    return !me_app_quit_requested();
}

int me_platform_key_pressed(unsigned vk, unsigned ctrl, unsigned alt, unsigned shift) {
    if (vk == 0 || vk >= 256) return 0;
    /* Match the chord's modifier set against current state. We consume the
       event even if modifiers mismatch — pressing X without Ctrl shouldn't
       leave a stale "X pressed" event around for the next frame's check. */
    if (!InterlockedExchange(&g_pending[vk], 0)) return 0;
    int got_ctrl  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) ? 1 : 0;
    int got_alt   = (GetAsyncKeyState(VK_MENU)    & 0x8000) ? 1 : 0;
    int got_shift = (GetAsyncKeyState(VK_SHIFT)   & 0x8000) ? 1 : 0;
    if ((ctrl  && !got_ctrl)  || (!ctrl  && got_ctrl  && (vk != VK_CONTROL && vk != VK_LCONTROL && vk != VK_RCONTROL))) return 0;
    if ((alt   && !got_alt)   || (!alt   && got_alt   && (vk != VK_MENU    && vk != VK_LMENU    && vk != VK_RMENU)))    return 0;
    if ((shift && !got_shift) || (!shift && got_shift && (vk != VK_SHIFT   && vk != VK_LSHIFT   && vk != VK_RSHIFT)))   return 0;
    return 1;
}

void me_platform_request_quit(void) {
    me_app_request_quit();
}

void me_platform_set_idle(HWND hwnd, int idle) {
    InterlockedExchange(&g_idle, idle);
    if (idle && hwnd) InvalidateRect(hwnd, NULL, TRUE);
}

/* ---- fullscreen toggle ---------------------------------------------------- */
void me_platform_exit_fullscreen(HWND hwnd) {
    if (hwnd) PostMessageA(hwnd, WM_ME_FULLSCREEN, 1, 0);
}

void me_platform_toggle_fullscreen(HWND hwnd) {
    if (hwnd) PostMessageA(hwnd, WM_ME_FULLSCREEN, 0, 0);
}

int me_platform_is_fullscreen(void) { return g_fs.active != 0; }
