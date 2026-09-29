#include "ui.h"
#include <commdlg.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "platform_win32.h"
#include "rom_cores.h"

/* Notifications from the emulation thread (lParam = malloc'd string). */
#define WM_ME_LOADED    (WM_APP + 10)
#define WM_ME_ERROR     (WM_APP + 11)
#define WM_ME_FRAME_GEN (WM_APP + 12)   /* wp: 1 = on */

enum {
    IDM_OPEN = 100,
    IDM_EXIT,
    IDM_RECENT_EMPTY,
    IDM_HARD_RESET = 110,
    IDM_SOFT_RESET,
    IDM_POWER,
    IDM_PLAYER1 = 120,
    IDM_PLAYER2,
    IDM_PLAYER3,
    IDM_PLAYER4,
    IDM_HOTKEYS,
    IDM_FULLSCREEN = 130,
    IDM_FRAME_GEN,
    IDM_BACKEND_VULKAN = 140,
    IDM_BACKEND_GDI,
    IDM_BACKEND_D3D11,
    IDM_RECENT_FIRST = 200,   /* .. IDM_RECENT_FIRST + ME_RECENT_MAX - 1 */
};

static HMENU g_recent_menu, g_console_menu, g_view_menu, g_backend_menu;

HMENU me_ui_create_menu(void) {
    HMENU bar = CreateMenu();

    HMENU file = CreatePopupMenu();
    g_recent_menu = CreatePopupMenu();
    AppendMenuA(file, MF_STRING, IDM_OPEN, "&Open ROM...");
    AppendMenuA(file, MF_POPUP, (UINT_PTR)g_recent_menu, "Open &Recent");
    AppendMenuA(file, MF_SEPARATOR, 0, NULL);
    AppendMenuA(file, MF_STRING, IDM_EXIT, "E&xit");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)file, "&File");

    g_console_menu = CreatePopupMenu();
    AppendMenuA(g_console_menu, MF_STRING, IDM_HARD_RESET, "&Hard Reset");
    AppendMenuA(g_console_menu, MF_STRING, IDM_SOFT_RESET, "&Soft Reset");
    AppendMenuA(g_console_menu, MF_STRING, IDM_POWER,      "&Power Off");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)g_console_menu, "&Console");

    HMENU controls = CreatePopupMenu();
    AppendMenuA(controls, MF_STRING, IDM_PLAYER1, "Player &1...");
    AppendMenuA(controls, MF_STRING, IDM_PLAYER2, "Player &2...");
    AppendMenuA(controls, MF_STRING | MF_GRAYED, IDM_PLAYER3, "Player 3 (n/a)");
    AppendMenuA(controls, MF_STRING | MF_GRAYED, IDM_PLAYER4, "Player 4 (n/a)");
    AppendMenuA(controls, MF_SEPARATOR, 0, NULL);
    AppendMenuA(controls, MF_STRING, IDM_HOTKEYS, "&Hotkeys...");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)controls, "C&ontrols");

    g_view_menu = CreatePopupMenu();
    g_backend_menu = CreatePopupMenu();
    AppendMenuA(g_backend_menu, MF_STRING, IDM_BACKEND_VULKAN, "&Vulkan");
    AppendMenuA(g_backend_menu, MF_STRING, IDM_BACKEND_GDI,    "&GDI");
    AppendMenuA(g_backend_menu, MF_STRING, IDM_BACKEND_D3D11,  "&D3D11");
    AppendMenuA(g_view_menu, MF_STRING, IDM_FULLSCREEN, "Toggle &Full Screen");
    AppendMenuA(g_view_menu, MF_STRING, IDM_FRAME_GEN,  "Frame &Gen");
    AppendMenuA(g_view_menu, MF_POPUP, (UINT_PTR)g_backend_menu, "&Rendering Backend (requires restart)");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)g_view_menu, "&View");

    return bar;
}

/* ---- persistence ---------------------------------------------------------- */
int me_ui_persist(me_settings_patch patch, const void *ctx) {
    const char *path = me_app_settings_path();
    me_settings file;
    me_settings_defaults(&file);
    if (me_settings_load(path, &file) < 0) {
        me_settings_free(&file);
        MessageBoxA(me_platform_hwnd(),
                    "settings.yaml has an error, so the change was not saved.\n"
                    "Fix or delete settings.yaml (it is recreated with defaults).",
                    "laggueless", MB_OK | MB_ICONWARNING);
        return -1;
    }
    patch(&file, ctx);
    int rc = me_settings_save(path, &file);
    me_settings_free(&file);
    if (rc != 0) {
        MessageBoxA(me_platform_hwnd(), "Could not write settings.yaml.",
                    "laggueless", MB_OK | MB_ICONWARNING);
    }
    return rc;
}

/* Most recent first, no duplicates (paths compare case-insensitively). */
static void recent_push(me_settings *s, const char *path) {
    char old[ME_RECENT_MAX][260];
    int old_n = s->recent_n;
    memcpy(old, s->recent, sizeof(old));
    snprintf(s->recent[0], sizeof(s->recent[0]), "%s", path);
    s->recent_n = 1;
    for (int i = 0; i < old_n && s->recent_n < ME_RECENT_MAX; i++) {
        if (_stricmp(old[i], path) == 0) continue;
        memcpy(s->recent[s->recent_n++], old[i], sizeof(old[i]));
    }
}

static void patch_recent(me_settings *s, const void *ctx) { recent_push(s, (const char *)ctx); }

static void patch_frame_gen(me_settings *s, const void *ctx) {
    s->lsfg_enabled = *(const int *)ctx;
}

typedef enum { BACKEND_OTHER, BACKEND_VULKAN, BACKEND_GDI, BACKEND_D3D11 } backend_choice;

/* Mirrors the renderer selection in main.c: GDI overrides everything, then
   Vulkan, then D3D11. */
static backend_choice backend_of(const me_settings *s) {
    if (s->force_gdi)    return BACKEND_GDI;
    if (s->force_vulkan) return BACKEND_VULKAN;
    if (s->force_d3d11)  return BACKEND_D3D11;
    return BACKEND_OTHER;
}

static void patch_backend(me_settings *s, const void *ctx) {
    backend_choice b = *(const backend_choice *)ctx;
    s->force_gdi    = (b == BACKEND_GDI);
    s->force_vulkan = (b == BACKEND_VULKAN);
    s->force_d3d11  = (b == BACKEND_D3D11);
}

/* ---- notifications (any thread) ------------------------------------------- */
static void post_string(UINT msg, WPARAM wp, const char *s) {
    HWND h = me_platform_hwnd();
    char *copy = h ? _strdup(s) : NULL;
    if (!copy) return;
    if (!PostMessageA(h, msg, wp, (LPARAM)copy)) free(copy);
}

void me_ui_notify_loaded(const char *rom_path) {
    char full[MAX_PATH];
    if (!GetFullPathNameA(rom_path, sizeof(full), full, NULL)) return;
    post_string(WM_ME_LOADED, 0, full);
}

void me_ui_notify_frame_gen(int on) {
    HWND h = me_platform_hwnd();
    if (h) PostMessageA(h, WM_ME_FRAME_GEN, on ? 1 : 0, 0);
}

void me_ui_notify_error(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    post_string(WM_ME_ERROR, 0, buf);
}

/* ---- menu refresh ----------------------------------------------------------- */
static void set_item_text(HMENU m, UINT id, const char *text) {
    MENUITEMINFOA mi = { .cbSize = sizeof(mi), .fMask = MIIM_STRING, .dwTypeData = (char *)text };
    SetMenuItemInfoA(m, id, FALSE, &mi);
}

/* Menu text treats '&' as a mnemonic marker; double it to show it literally. */
static void escape_amp(const char *in, char *out, size_t out_sz) {
    size_t n = 0;
    for (; *in && n + 2 < out_sz; in++) {
        if (*in == '&') out[n++] = '&';
        out[n++] = *in;
    }
    out[n] = '\0';
}

static void rebuild_recent_menu(void) {
    while (GetMenuItemCount(g_recent_menu) > 0) DeleteMenu(g_recent_menu, 0, MF_BYPOSITION);
    const me_settings *s = me_app_settings();
    if (s->recent_n == 0) {
        AppendMenuA(g_recent_menu, MF_STRING | MF_GRAYED, IDM_RECENT_EMPTY, "(empty)");
        return;
    }
    for (int i = 0; i < s->recent_n; i++) {
        /* "Game.nes<tab>C:\folder": the file name, with its folder in the
           right-hand column (where accelerators normally go). */
        const char *path = s->recent[i];
        const char *base = path;
        for (const char *p = path; *p; p++) if (*p == '\\' || *p == '/') base = p + 1;
        char dir[MAX_PATH];
        size_t dir_len = (size_t)(base - path);
        if (dir_len > 0) dir_len--;  /* drop the trailing separator */
        if (dir_len >= sizeof(dir)) dir_len = sizeof(dir) - 1;
        memcpy(dir, path, dir_len); dir[dir_len] = '\0';

        char name_esc[MAX_PATH * 2], dir_esc[MAX_PATH * 2], label[MAX_PATH * 4 + 8];
        escape_amp(base, name_esc, sizeof(name_esc));
        escape_amp(dir, dir_esc, sizeof(dir_esc));
        snprintf(label, sizeof(label), "%s\t%s", name_esc, dir_esc);
        AppendMenuA(g_recent_menu, MF_STRING, IDM_RECENT_FIRST + i, label);
    }
}

static void refresh_menu(HMENU m) {
    me_app_status st;
    me_status_get(&st);
    const me_settings *s = me_app_settings();

    if (m == g_recent_menu) {
        rebuild_recent_menu();
    } else if (m == g_console_menu) {
        UINT running = st.game_running ? MF_ENABLED : MF_GRAYED;
        EnableMenuItem(m, IDM_HARD_RESET, MF_BYCOMMAND | running);
        EnableMenuItem(m, IDM_SOFT_RESET, MF_BYCOMMAND | running);
        set_item_text(m, IDM_POWER, st.game_running ? "&Power Off" : "&Power On");
        EnableMenuItem(m, IDM_POWER, MF_BYCOMMAND |
                       ((st.game_running || st.can_power_on) ? MF_ENABLED : MF_GRAYED));
    } else if (m == g_view_menu) {
        CheckMenuItem(m, IDM_FULLSCREEN, MF_BYCOMMAND |
                      (me_platform_is_fullscreen() ? MF_CHECKED : MF_UNCHECKED));
        set_item_text(m, IDM_FRAME_GEN, !st.frame_gen_supported ? "Frame &Gen\tnot in this build"
                                        : st.vulkan_active      ? "Frame &Gen"
                                                                : "Frame &Gen\tVulkan only");
        EnableMenuItem(m, IDM_FRAME_GEN, MF_BYCOMMAND |
                       (st.frame_gen_supported ? MF_ENABLED : MF_GRAYED));
        CheckMenuItem(m, IDM_FRAME_GEN, MF_BYCOMMAND |
                      (s->lsfg_enabled ? MF_CHECKED : MF_UNCHECKED));
    } else if (m == g_backend_menu) {
        static const UINT ids[] = { 0, IDM_BACKEND_VULKAN, IDM_BACKEND_GDI, IDM_BACKEND_D3D11 };
        backend_choice b = backend_of(s);
        for (int i = 1; i < 4; i++) CheckMenuItem(m, ids[i], MF_BYCOMMAND | MF_UNCHECKED);
        if (b != BACKEND_OTHER)
            CheckMenuRadioItem(m, IDM_BACKEND_VULKAN, IDM_BACKEND_D3D11, ids[b], MF_BYCOMMAND);
    }
}

/* ---- commands ------------------------------------------------------------- */
static void open_rom_dialog(HWND owner) {
    char patterns[256], filter[600], path[MAX_PATH] = "", initial_dir[MAX_PATH];
    me_rom_patterns(patterns, sizeof(patterns));
    /* Filter is a list of NUL-separated description/pattern pairs. */
    int n = snprintf(filter, sizeof(filter), "Games (%s)%c%s%cAll files (*.*)%c*.*%c",
                     patterns, 0, patterns, 0, 0, 0);
    if (n < 0 || (size_t)n >= sizeof(filter) - 1) return;
    filter[n] = '\0';

    GetModuleFileNameA(NULL, initial_dir, sizeof(initial_dir));
    char *slash = strrchr(initial_dir, '\\');
    if (slash) snprintf(slash + 1, sizeof(initial_dir) - (size_t)(slash + 1 - initial_dir), "roms");

    OPENFILENAMEA ofn = {0};
    ofn.lStructSize     = sizeof(ofn);
    ofn.hwndOwner       = owner;
    ofn.lpstrFilter     = filter;
    ofn.lpstrFile       = path;
    ofn.nMaxFile        = sizeof(path);
    ofn.lpstrInitialDir = initial_dir;
    ofn.lpstrTitle      = "Open ROM";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    /* The dialog changes the process's working directory while it browses
       (OFN_NOCHANGEDIR doesn't apply to GetOpenFileName), so put it back.
       Paths the emulation thread uses are absolute for the time it's open. */
    char cwd[MAX_PATH];
    DWORD cwd_len = GetCurrentDirectoryA(sizeof(cwd), cwd);
    BOOL picked = GetOpenFileNameA(&ofn);
    if (cwd_len && cwd_len < sizeof(cwd)) SetCurrentDirectoryA(cwd);
    if (picked) me_cmd_post(ME_CMD_LOAD_ROM, 0, path);
}

static void on_command(HWND h, UINT id) {
    me_settings *s = me_app_settings();
    if (id >= IDM_RECENT_FIRST && id < IDM_RECENT_FIRST + ME_RECENT_MAX) {
        int i = (int)(id - IDM_RECENT_FIRST);
        if (i < s->recent_n) me_cmd_post(ME_CMD_LOAD_ROM, 0, s->recent[i]);
        return;
    }
    switch (id) {
        case IDM_OPEN:       open_rom_dialog(h); break;
        case IDM_EXIT:       PostMessageA(h, WM_CLOSE, 0, 0); break;
        case IDM_HARD_RESET: me_cmd_post(ME_CMD_HARD_RESET, 0, NULL); break;
        case IDM_SOFT_RESET: me_cmd_post(ME_CMD_SOFT_RESET, 0, NULL); break;
        case IDM_POWER:      me_cmd_post(ME_CMD_POWER, 0, NULL); break;
        case IDM_PLAYER1:    me_ui_player_dialog(h, 0); break;
        case IDM_PLAYER2:    me_ui_player_dialog(h, 1); break;
        case IDM_HOTKEYS:    me_ui_hotkeys_dialog(h); break;
        case IDM_FULLSCREEN: me_platform_toggle_fullscreen(h); break;
        case IDM_FRAME_GEN:  me_cmd_post(ME_CMD_FRAME_GEN, !s->lsfg_enabled, NULL); break;
        case IDM_BACKEND_VULKAN:
        case IDM_BACKEND_GDI:
        case IDM_BACKEND_D3D11: {
            backend_choice b = id == IDM_BACKEND_VULKAN ? BACKEND_VULKAN
                             : id == IDM_BACKEND_GDI    ? BACKEND_GDI : BACKEND_D3D11;
            /* The running renderer is untouched; this is read at startup. */
            patch_backend(s, &b);
            me_ui_persist(patch_backend, &b);
            break;
        }
    }
}

LRESULT me_ui_handle(HWND h, UINT msg, WPARAM wp, LPARAM lp, int *handled) {
    *handled = 1;
    switch (msg) {
        case WM_INITMENUPOPUP:
            refresh_menu((HMENU)wp);
            return 0;
        case WM_COMMAND:
            if (HIWORD(wp) == 0 && lp == 0) { on_command(h, LOWORD(wp)); return 0; }
            break;
        case WM_ME_LOADED: {
            char *path = (char *)lp;
            recent_push(me_app_settings(), path);
            me_ui_persist(patch_recent, path);
            free(path);
            return 0;
        }
        case WM_ME_FRAME_GEN: {
            int on = (int)wp;
            me_app_settings()->lsfg_enabled = on;
            me_ui_persist(patch_frame_gen, &on);
            return 0;
        }
        case WM_ME_ERROR: {
            char *text = (char *)lp;
            MessageBoxA(h, text, "laggueless", MB_OK | MB_ICONWARNING);
            free(text);
            return 0;
        }
    }
    *handled = 0;
    return 0;
}
