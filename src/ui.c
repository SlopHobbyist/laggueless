#include "ui.h"
#include <commctrl.h>
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
#define WM_ME_PICK      (WM_APP + 13)   /* lParam = malloc'd pick_request */
#define WM_ME_CONSOLE_PICK (WM_APP + 14) /* lParam = malloc'd "fingerprint=console" */

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
    IDM_SHOW_CURSOR,
    IDM_FULLSCREEN = 130,
    IDM_FRAME_GEN,
    IDM_BACKEND_VULKAN = 140,
    IDM_BACKEND_GDI,
    IDM_BACKEND_D3D11,
    IDM_DOWNLOAD_CORES = 150,
    IDM_SET_CORES,
    IDM_FILE_TYPES,
    IDM_FIRMWARE,
    IDM_ADAPTER_FIRST = 160,  /* .. + ME_ADAPTERS_MAX - 1 */
    IDM_SCREEN_TOP = 170,     /* View > Screen, in me_screens order from here */
    IDM_SCREEN_BOTTOM,
    IDM_SCREEN_BOTH,
    IDM_ASPECT_1_1 = 180,     /* View > Aspect Ratio, in me_aspect_mode order */
    IDM_ASPECT_4_3,
    IDM_ASPECT_16_9,
    IDM_RECENT_FIRST = 200,   /* .. IDM_RECENT_FIRST + ME_RECENT_MAX - 1 */
    IDM_RECENT_CLEAR = 200 + ME_RECENT_MAX,
};

/* Most adapters a console has (the Mega Drive's two). */
#define ME_ADAPTERS_MAX 4
static HMENU g_file_menu, g_recent_menu, g_console_menu, g_controls_menu, g_view_menu, g_backend_menu,
             g_screen_menu, g_aspect_menu;

HMENU me_ui_create_menu(void) {
    HMENU bar = CreateMenu();

    HMENU file = g_file_menu = CreatePopupMenu();
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

    /* Players and the adapter checkboxes are filled in when it opens. */
    g_controls_menu = CreatePopupMenu();
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)g_controls_menu, "C&ontrols");

    HMENU cores = CreatePopupMenu();
    AppendMenuA(cores, MF_STRING, IDM_DOWNLOAD_CORES, "&Download Cores...");
    AppendMenuA(cores, MF_STRING, IDM_SET_CORES,      "&Set Cores...");
    AppendMenuA(cores, MF_STRING, IDM_FIRMWARE,       "F&irmware...");
    AppendMenuA(cores, MF_STRING, IDM_FILE_TYPES,     "&File Associations...");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)cores, "Co&res");

    g_view_menu = CreatePopupMenu();
    g_backend_menu = CreatePopupMenu();
    AppendMenuA(g_backend_menu, MF_STRING, IDM_BACKEND_VULKAN, "&Vulkan");
    AppendMenuA(g_backend_menu, MF_STRING, IDM_BACKEND_GDI,    "&GDI");
    AppendMenuA(g_backend_menu, MF_STRING, IDM_BACKEND_D3D11,  "&D3D11");
    g_screen_menu = CreatePopupMenu();
    AppendMenuA(g_screen_menu, MF_STRING, IDM_SCREEN_TOP,    "&Top Screen");
    AppendMenuA(g_screen_menu, MF_STRING, IDM_SCREEN_BOTTOM, "&Bottom Screen");
    AppendMenuA(g_screen_menu, MF_STRING, IDM_SCREEN_BOTH,   "B&oth");
    g_aspect_menu = CreatePopupMenu();
    AppendMenuA(g_aspect_menu, MF_STRING, IDM_ASPECT_1_1,  "&1:1 (Square Pixels)");
    AppendMenuA(g_aspect_menu, MF_STRING, IDM_ASPECT_4_3,  "&4:3");
    AppendMenuA(g_aspect_menu, MF_STRING, IDM_ASPECT_16_9, "1&6:9");
    AppendMenuA(g_view_menu, MF_STRING, IDM_FULLSCREEN, "Toggle &Full Screen");
    AppendMenuA(g_view_menu, MF_POPUP, (UINT_PTR)g_aspect_menu, "&Aspect Ratio");
    AppendMenuA(g_view_menu, MF_POPUP, (UINT_PTR)g_screen_menu, "&Screen");
    AppendMenuA(g_view_menu, MF_STRING, IDM_FRAME_GEN,  "Frame &Gen");
    AppendMenuA(g_view_menu, MF_POPUP, (UINT_PTR)g_backend_menu, "&Rendering Backend (requires restart)");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)g_view_menu, "&View");

    return bar;
}

/* ---- dialog helpers ------------------------------------------------------- */
typedef struct { DWORD words[128]; } dlg_template;   /* DWORD-aligned, as DLGTEMPLATE requires */

static DLGTEMPLATE *build_template(dlg_template *buf, DWORD style, const wchar_t *title,
                                   short cx, short cy) {
    memset(buf, 0, sizeof(*buf));
    DLGTEMPLATE *t = (DLGTEMPLATE *)buf->words;
    t->style = style | DS_CENTER | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    t->cx = cx;
    t->cy = cy;
    WORD *p = (WORD *)(t + 1);
    *p++ = 0;  /* no menu */
    *p++ = 0;  /* default dialog class */
    for (const wchar_t *s = title; *s; s++) *p++ = (WORD)*s;
    *p++ = 0;
    *p++ = 9;  /* point size */
    for (const wchar_t *s = L"Segoe UI"; *s; s++) *p++ = (WORD)*s;
    *p++ = 0;
    return t;
}

INT_PTR me_ui_dialog(HWND owner, const wchar_t *title, short cx, short cy,
                     DLGPROC proc, LPARAM param) {
    dlg_template buf;
    DLGTEMPLATE *t = build_template(&buf, DS_MODALFRAME, title, cx, cy);
    return DialogBoxIndirectParamW(GetModuleHandleW(NULL), t, owner, proc, param);
}

HWND me_ui_dialog_modeless(HWND owner, const wchar_t *title, short cx, short cy,
                           DLGPROC proc, LPARAM param) {
    dlg_template buf;
    DLGTEMPLATE *t = build_template(&buf, DS_MODALFRAME | WS_MINIMIZEBOX, title, cx, cy);
    return CreateDialogIndirectParamW(GetModuleHandleW(NULL), t, owner, proc, param);
}

HWND me_ui_add_control(HWND dlg, const char *cls, const char *text, DWORD style,
                       int x, int y, int w, int h, int id) {
    RECT r = { x, y, x + w, y + h };
    MapDialogRect(dlg, &r);
    HWND c = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             r.left, r.top, r.right - r.left, r.bottom - r.top,
                             dlg, (HMENU)(INT_PTR)id, GetModuleHandleA(NULL), NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM)SendMessageA(dlg, WM_GETFONT, 0, 0), TRUE);
    return c;
}

void me_ui_init_common_controls(void) {
    static int done = 0;
    if (done) return;
    INITCOMMONCONTROLSEX icc = { sizeof(icc),
                                 ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);
    done = 1;
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

static void patch_recent_clear(me_settings *s, const void *ctx) { (void)ctx; s->recent_n = 0; }

static void patch_screens(me_settings *s, const void *ctx) {
    s->screens = *(const me_screens *)ctx;
}

static const UINT k_screen_ids[] = {
    [ME_SCREENS_BOTH] = IDM_SCREEN_BOTH, [ME_SCREENS_TOP] = IDM_SCREEN_TOP,
    [ME_SCREENS_BOTTOM] = IDM_SCREEN_BOTTOM,
};

static void patch_aspect(me_settings *s, const void *ctx) {
    s->aspect = *(const me_aspect_mode *)ctx;
}

static void patch_show_cursor(me_settings *s, const void *ctx) {
    s->show_cursor_fullscreen = *(const int *)ctx;
}

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
    /* No window yet (a game opened from the command line or by double-click
       that failed to start): nothing runs that it could hold up. */
    if (!me_platform_hwnd()) {
        MessageBoxA(NULL, buf, "laggueless", MB_OK | MB_ICONWARNING);
        return;
    }
    post_string(WM_ME_ERROR, 0, buf);
}

typedef struct {
    me_console_set consoles;
    int            current;   /* console index to start on, or -1 */
    char           path[MAX_PATH];
} pick_request;

void me_ui_pick_console(const char *rom_path, me_console_set consoles, int current) {
    HWND h = me_platform_hwnd();
    pick_request *r = h ? malloc(sizeof(*r)) : NULL;
    if (!r) return;
    r->consoles = consoles;
    r->current  = current;
    snprintf(r->path, sizeof(r->path), "%s", rom_path);
    if (!PostMessageA(h, WM_ME_PICK, 0, (LPARAM)r)) free(r);
}

void me_ui_notify_console_pick(const char *rom_fingerprint, const char *console) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s=%s", rom_fingerprint, console ? console : "");
    post_string(WM_ME_CONSOLE_PICK, 0, buf);
}

static void patch_console_pick(me_settings *s, const void *ctx) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", (const char *)ctx);
    char *eq = strchr(buf, '=');
    if (!eq) return;
    *eq = '\0';
    me_settings_set_console_pick(s, buf, eq + 1);
}

/* ---- Pick Console ------------------------------------------------------------ */
enum { IDC_PICK_FILE = 1400, IDC_PICK_HINT, IDC_PICK_LIST, IDC_PICK_NOTE };

typedef struct {
    const pick_request *req;
    int n, console[64];   /* list row -> console index */
} pick_dlg;

/* Ends with the picked console's index + 1, or 0. */
static INT_PTR CALLBACK pick_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    pick_dlg *d = (pick_dlg *)GetWindowLongPtrW(dlg, DWLP_USER);
    switch (msg) {
        case WM_INITDIALOG: {
            d = (pick_dlg *)lp;
            SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)d);
            const char *base = d->req->path;
            for (const char *p = base; *p; p++) if (*p == '\\' || *p == '/') base = p + 1;
            me_ui_add_control(dlg, "STATIC", base, SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS,
                              7, 7, 226, 9, IDC_PICK_FILE);
            me_ui_add_control(dlg, "STATIC",
                              d->req->current >= 0
                                  ? "Several consoles use this type of file. Pick the one it's for "
                                    "(it opens as the selected one now):"
                                  : "Several consoles use this type of file, and the file doesn't say "
                                    "which one it's for. Pick its console:",
                              SS_LEFT, 7, 19, 226, 18, IDC_PICK_HINT);
            int sel = 0;
            HWND list = me_ui_add_control(dlg, "LISTBOX", "",
                                          LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_BORDER |
                                          WS_VSCROLL | WS_TABSTOP,
                                          7, 41, 226, 72, IDC_PICK_LIST);
            me_settings_lock();   /* the emulation thread reads the core picks */
            for (int i = 0; i < me_console_count() && d->n < 64; i++) {
                if (!(d->req->consoles >> i & 1)) continue;
                const me_console *c = me_console_at(i);
                char dll[MAX_PATH], core[128], label[256];
                me_console_core_dll(me_app_settings(), c, dll, sizeof(dll));
                me_core_display_name(dll, core, sizeof(core));
                snprintf(label, sizeof(label), "%s  (%s)", c->name, core);
                SendMessageA(list, LB_ADDSTRING, 0, (LPARAM)label);
                if (i == d->req->current) sel = d->n;
                d->console[d->n++] = i;
            }
            me_settings_unlock();
            SendMessageA(list, LB_SETCURSEL, (WPARAM)sel, 0);
            me_ui_add_control(dlg, "STATIC",
                              "It opens as this console from now on. To pick again, hold Shift "
                              "while dropping it on the window or clicking it in Open Recent.",
                              SS_LEFT, 7, 118, 226, 18, IDC_PICK_NOTE);
            me_ui_add_control(dlg, "BUTTON", "Open",   BS_DEFPUSHBUTTON | WS_TABSTOP | WS_GROUP,
                              119, 141, 55, 14, IDOK);
            me_ui_add_control(dlg, "BUTTON", "Cancel", BS_PUSHBUTTON | WS_TABSTOP,
                              178, 141, 55, 14, IDCANCEL);
            SetFocus(list);
            return FALSE;   /* focus set */
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_PICK_LIST:
                    if (HIWORD(wp) != LBN_DBLCLK) break;
                    /* fall through - a double-click opens */
                case IDOK: {
                    LRESULT sel = SendDlgItemMessageA(dlg, IDC_PICK_LIST, LB_GETCURSEL, 0, 0);
                    if (sel >= 0 && sel < d->n) EndDialog(dlg, d->console[sel] + 1);
                    return TRUE;
                }
                case IDCANCEL:
                    EndDialog(dlg, 0);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

/* Modal to the UI thread only: the running game (if any) keeps going, and is
   replaced once a console is picked. */
static void pick_console(HWND owner, const pick_request *r) {
    pick_dlg d = { .req = r };
    INT_PTR picked = me_ui_dialog(owner, L"Pick Console", 240, 162, pick_proc, (LPARAM)&d);
    if (picked > 0) me_cmd_post(ME_CMD_LOAD_ROM, (int)picked, r->path);
}

/* ---- menu refresh ----------------------------------------------------------- */
static void set_item_text(HMENU m, UINT id, const char *text) {
    MENUITEMINFOA mi = { .cbSize = sizeof(mi), .fMask = MIIM_STRING, .dwTypeData = (char *)text };
    SetMenuItemInfoA(m, id, FALSE, &mi);
}

/* A submenu's item has no command id to look it up by. */
static void set_popup_text(HMENU m, HMENU sub, const char *text) {
    for (int i = 0, n = GetMenuItemCount(m); i < n; i++) {
        if (GetSubMenu(m, i) != sub) continue;
        MENUITEMINFOA mi = { .cbSize = sizeof(mi), .fMask = MIIM_STRING, .dwTypeData = (char *)text };
        SetMenuItemInfoA(m, (UINT)i, TRUE, &mi);
        return;
    }
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

/* "label<tab>F11, Alt+Return": the item with what hotkey `hk` is bound to
   right now (only the devices Hotkeys > input has on), in the accelerator
   column. Just the label when it's unbound. */
static void hotkey_label(const char *label, me_hotkey_id hk, char *out, size_t out_sz) {
    const me_settings *s = me_app_settings();
    char keys[160] = "";
    size_t len = 0;
    if (s->hk_source != ME_SRC_CONTROLLER) {
        const me_kb_bindings *kb = &s->hk[hk];
        for (int i = 0; i < kb->count; i++) {
            char one[64];
            me_kb_binding_str(&kb->b[i], one, sizeof(one));
            if (!one[0]) continue;
            int n = snprintf(keys + len, sizeof(keys) - len, "%s%s", len ? ", " : "", one);
            if (n < 0 || (size_t)n >= sizeof(keys) - len) break;
            len += (size_t)n;
        }
    }
    if (s->hk_source != ME_SRC_KEYBOARD) {
        const me_xi_bindings *xi = &s->hk_xi[hk];
        for (int i = 0; i < xi->count; i++) {
            char one[128];
            me_xi_chord_str(xi->b[i].buttons, one, sizeof(one));
            if (!one[0]) continue;
            int n = snprintf(keys + len, sizeof(keys) - len, "%s%s", len ? ", " : "", one);
            if (n < 0 || (size_t)n >= sizeof(keys) - len) break;
            len += (size_t)n;
        }
    }
    char esc[sizeof(keys) * 2];
    escape_amp(keys, esc, sizeof(esc));
    snprintf(out, out_sz, esc[0] ? "%s	%s" : "%s", label, esc);
}

static void set_hotkey_item(HMENU m, UINT id, const char *label, me_hotkey_id hk) {
    char text[512];
    hotkey_label(label, hk, text, sizeof(text));
    set_item_text(m, id, text);
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
    AppendMenuA(g_recent_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(g_recent_menu, MF_STRING, IDM_RECENT_CLEAR, "Clear Recent");
}

/* Players 1-4, greyed past what the running game has, each with the
   controller it plays with where the core offers a choice (Wii games in
   Dolphin); then its console's multiplayer adapters as checkboxes (only for
   a console that has them); then Hotkeys. Rebuilt each time it opens. */
static void rebuild_controls_menu(const me_app_status *st) {
    HMENU m = g_controls_menu;
    while (GetMenuItemCount(m) > 0) DeleteMenu(m, 0, MF_BYPOSITION);

    const me_console *c = me_console_at(st->console);
    const me_adapter *adapters = c ? c->adapters : NULL;
    const char *unlock = NULL;   /* the adapter that would add players */
    for (int i = 0; adapters && adapters[i].id && !unlock; i++)
        if (st->adapters_usable & (1u << i)) unlock = adapters[i].name;

    const me_controller *controllers = me_console_controllers(c);
    for (int p = 0; p < ME_MAX_PLAYERS; p++) {
        char label[96];
        int on = p < st->players;
        if (on && controllers && st->controller[p] >= 0)
            snprintf(label, sizeof(label), "Player &%d...\t%s", p + 1, controllers[st->controller[p]].name);
        else if (on || !c)
            snprintf(label, sizeof(label), "Player &%d...", p + 1);
        else if (unlock)
            snprintf(label, sizeof(label), "Player %d\tneeds %s", p + 1, unlock);
        else
            snprintf(label, sizeof(label), "Player %d\t%s: %d player%s", p + 1, c->name,
                     c->players, c->players == 1 ? "" : "s");
        AppendMenuA(m, MF_STRING | (on ? MF_ENABLED : MF_GRAYED), IDM_PLAYER1 + p, label);
    }

    if (adapters && adapters[0].id) {
        AppendMenuA(m, MF_SEPARATOR, 0, NULL);
        for (int i = 0; adapters[i].id && i < ME_ADAPTERS_MAX; i++) {
            char label[96];
            int usable = (st->adapters_usable >> i) & 1u;
            snprintf(label, sizeof(label), "%s (Players %d-%d)%s", adapters[i].name,
                     c->players + 1, ME_MAX_PLAYERS, usable ? "" : "\tnot in this core");
            UINT flags = MF_STRING | (usable ? MF_ENABLED : MF_GRAYED) |
                         (st->adapter == i ? MF_CHECKED : MF_UNCHECKED);
            AppendMenuA(m, flags, IDM_ADAPTER_FIRST + i, label);
        }
    }

    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_STRING | (me_app_settings()->show_cursor_fullscreen ? MF_CHECKED : MF_UNCHECKED),
                IDM_SHOW_CURSOR, "Show &Cursor in Fullscreen");
    AppendMenuA(m, MF_STRING, IDM_HOTKEYS, "&Hotkeys...");
}

typedef struct { char console[32]; char adapter[32]; } adapter_choice;

static void patch_adapter(me_settings *s, const void *ctx) {
    const adapter_choice *a = (const adapter_choice *)ctx;
    me_settings_set_console_adapter(s, a->console, a->adapter);
}

/* The checkbox: plug adapter `i` of the running console in, or unplug it
   if it's the one plugged in. The core gets it between frames. */
static void toggle_adapter(int i) {
    me_app_status st;
    me_status_get(&st);
    const me_console *c = me_console_at(st.console);
    if (!c || !c->adapters) return;
    for (int k = 0; k <= i; k++) if (!c->adapters[k].id) return;
    adapter_choice a;
    snprintf(a.console, sizeof(a.console), "%s", c->id);
    snprintf(a.adapter, sizeof(a.adapter), "%s", st.adapter == i ? "" : c->adapters[i].id);
    me_settings_lock();
    patch_adapter(me_app_settings(), &a);
    me_settings_unlock();
    me_ui_persist(patch_adapter, &a);
    me_cmd_post(ME_CMD_ADAPTER, 0, NULL);
}

static void refresh_menu(HMENU m) {
    me_app_status st;
    me_status_get(&st);
    const me_settings *s = me_app_settings();

    if (m == g_file_menu) {
        set_hotkey_item(m, IDM_EXIT, "E&xit", ME_HK_QUIT);
    } else if (m == g_recent_menu) {
        rebuild_recent_menu();
    } else if (m == g_controls_menu) {
        rebuild_controls_menu(&st);
    } else if (m == g_console_menu) {
        UINT running = st.game_running ? MF_ENABLED : MF_GRAYED;
        set_hotkey_item(m, IDM_HARD_RESET, "&Hard Reset", ME_HK_HARD_RESET);
        EnableMenuItem(m, IDM_HARD_RESET, MF_BYCOMMAND | running);
        EnableMenuItem(m, IDM_SOFT_RESET, MF_BYCOMMAND | running);
        set_item_text(m, IDM_POWER, st.game_running ? "&Power Off" : "&Power On");
        EnableMenuItem(m, IDM_POWER, MF_BYCOMMAND |
                       ((st.game_running || st.can_power_on) ? MF_ENABLED : MF_GRAYED));
    } else if (m == g_view_menu) {
        char text[512];
        hotkey_label("&Aspect Ratio", ME_HK_CYCLE_ASPECT, text, sizeof(text));
        set_popup_text(m, g_aspect_menu, text);
        set_hotkey_item(m, IDM_FULLSCREEN, "Toggle &Full Screen", ME_HK_TOGGLE_FULLSCREEN);
        CheckMenuItem(m, IDM_FULLSCREEN, MF_BYCOMMAND |
                      (me_platform_is_fullscreen() ? MF_CHECKED : MF_UNCHECKED));
        set_item_text(m, IDM_FRAME_GEN, !st.frame_gen_supported ? "Frame &Gen\tnot in this build"
                                        : st.vulkan_active      ? "Frame &Gen"
                                                                : "Frame &Gen\tVulkan only");
        EnableMenuItem(m, IDM_FRAME_GEN, MF_BYCOMMAND |
                       (st.frame_gen_supported ? MF_ENABLED : MF_GRAYED));
        CheckMenuItem(m, IDM_FRAME_GEN, MF_BYCOMMAND |
                      (s->lsfg_enabled ? MF_CHECKED : MF_UNCHECKED));
    } else if (m == g_screen_menu) {
        /* Only two-screen consoles (DS, 3DS) have a choice; the pick is kept for
           the next one. */
        const me_console *c = me_console_at(st.console);
        UINT on = (!st.game_running || (c && (c->flags & ME_CONSOLE_TWO_SCREENS))) ? MF_ENABLED : MF_GRAYED;
        for (int i = 0; i < 3; i++) EnableMenuItem(m, k_screen_ids[i], MF_BYCOMMAND | on);
        CheckMenuRadioItem(m, IDM_SCREEN_TOP, IDM_SCREEN_BOTH,
                           k_screen_ids[(unsigned)s->screens <= 2 ? s->screens : 0], MF_BYCOMMAND);
    } else if (m == g_aspect_menu) {
        /* Also changed by F1, so it shows the live value. */
        unsigned a = (unsigned)s->aspect <= 2 ? (unsigned)s->aspect : 0;
        CheckMenuRadioItem(m, IDM_ASPECT_1_1, IDM_ASPECT_16_9, IDM_ASPECT_1_1 + a, MF_BYCOMMAND);
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
    char patterns[1024], filter[2200], path[MAX_PATH] = "", initial_dir[MAX_PATH];
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
    if (id >= IDM_ADAPTER_FIRST && id < IDM_ADAPTER_FIRST + ME_ADAPTERS_MAX) {
        toggle_adapter((int)(id - IDM_ADAPTER_FIRST));
        return;
    }
    if (id >= IDM_RECENT_FIRST && id < IDM_RECENT_FIRST + ME_RECENT_MAX) {
        int i = (int)(id - IDM_RECENT_FIRST);
        /* Shift-click: ask which console, as Shift does on a drop. */
        int ask = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (i < s->recent_n) me_cmd_post(ME_CMD_LOAD_ROM, ask ? ME_LOAD_ASK : 0, s->recent[i]);
        return;
    }
    switch (id) {
        case IDM_OPEN:       open_rom_dialog(h); break;
        case IDM_RECENT_CLEAR:
            s->recent_n = 0;
            me_ui_persist(patch_recent_clear, NULL);
            break;
        case IDM_EXIT:       PostMessageA(h, WM_CLOSE, 0, 0); break;
        case IDM_HARD_RESET: me_cmd_post(ME_CMD_HARD_RESET, 0, NULL); break;
        case IDM_SOFT_RESET: me_cmd_post(ME_CMD_SOFT_RESET, 0, NULL); break;
        case IDM_POWER:      me_cmd_post(ME_CMD_POWER, 0, NULL); break;
        case IDM_PLAYER1:
        case IDM_PLAYER2:
        case IDM_PLAYER3:
        case IDM_PLAYER4:    me_ui_player_dialog(h, (int)(id - IDM_PLAYER1)); break;
        case IDM_HOTKEYS:    me_ui_hotkeys_dialog(h); break;
        case IDM_SHOW_CURSOR: {
            int on = !s->show_cursor_fullscreen;
            s->show_cursor_fullscreen = on;
            me_ui_persist(patch_show_cursor, &on);
            me_platform_update_cursor();
            break;
        }
        case IDM_DOWNLOAD_CORES: me_ui_download_cores(h); break;
        case IDM_SET_CORES:  me_ui_set_cores_dialog(h); break;
        case IDM_FILE_TYPES: me_ui_file_types_dialog(h); break;
        case IDM_FIRMWARE:   me_ui_firmware_dialog(h); break;
        case IDM_FULLSCREEN: me_platform_toggle_fullscreen(h); break;
        case IDM_FRAME_GEN:  me_cmd_post(ME_CMD_FRAME_GEN, !s->lsfg_enabled, NULL); break;
        case IDM_SCREEN_TOP:
        case IDM_SCREEN_BOTTOM:
        case IDM_SCREEN_BOTH: {
            /* The emulation thread reads it at the next present. */
            me_screens v = id == IDM_SCREEN_TOP    ? ME_SCREENS_TOP
                         : id == IDM_SCREEN_BOTTOM ? ME_SCREENS_BOTTOM : ME_SCREENS_BOTH;
            s->screens = v;
            me_ui_persist(patch_screens, &v);
            break;
        }
        case IDM_ASPECT_1_1:
        case IDM_ASPECT_4_3:
        case IDM_ASPECT_16_9: {
            /* The emulation thread reads it at the next present. */
            me_aspect_mode v = (me_aspect_mode)(id - IDM_ASPECT_1_1);
            s->aspect = v;
            me_ui_persist(patch_aspect, &v);
            break;
        }
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
        case WM_ME_PICK: {
            pick_request *r = (pick_request *)lp;
            pick_console(h, r);
            free(r);
            return 0;
        }
        case WM_ME_CONSOLE_PICK: {
            char *pick = (char *)lp;
            me_settings_lock();   /* the emulation thread looks picks up */
            patch_console_pick(me_app_settings(), pick);
            me_settings_unlock();
            me_ui_persist(patch_console_pick, pick);
            free(pick);
            return 0;
        }
    }
    *handled = 0;
    return 0;
}
