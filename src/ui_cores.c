/* Cores > Download Cores and Cores > Set Cores.

   Download Cores asks first, then fetches RetroArch's nightly core bundle
   from the libretro buildbot and unpacks it into cores\ next to the exe. The
   download and unpack run on a background-priority thread; a small window
   on the UI thread shows progress and can cancel. The game keeps running
   throughout.

   Set Cores lists every console we know (rom_cores.c) with the core its
   games open in. Clicking a console drops down the cores that can run it.
   Save stores the picks that differ from ours in settings.yaml. */

#include "ui.h"
#include <commctrl.h>
#include <wininet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "archive_7z.h"
#include "platform_win32.h"
#include "rom_cores.h"

#define CORES_URL  "https://buildbot.libretro.com/nightly/windows/x86_64/RetroArch_cores.7z"
#define CORES_HOST "buildbot.libretro.com"

static void get_cores_dir(char *out, size_t out_sz) {
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, sizeof(exe));
    char *slash = n ? strrchr(exe, '\\') : NULL;
    if (slash) slash[1] = '\0'; else exe[0] = '\0';
    snprintf(out, out_sz, "%scores\\", exe);
}

/* ---- Download Cores ------------------------------------------------------- */
#define WM_DL_PROGRESS (WM_APP + 30)
#define WM_DL_DONE     (WM_APP + 31)   /* wp: me_7z_result, lp: malloc'd message or NULL */

enum { IDC_DL_STATUS = 1100, IDC_DL_BAR };

/* One download at a time. `dlg` and `thread` belong to the UI thread; the
   rest is shared with the worker. */
static struct {
    HWND   dlg;
    HANDLE thread;
    volatile long cancel;
    volatile LONG unpacking;           /* 0 = downloading, 1 = unpacking */
    volatile LONG64 done, total;       /* bytes; total 0 = unknown */
    volatile LONG progress_posted;     /* a WM_DL_PROGRESS is queued */
    char archive[MAX_PATH];
    char cores_dir[MAX_PATH];
} g_dl;

/* Worker: publish progress; at most one message waits in the queue. */
static void dl_progress(unsigned long long done, unsigned long long total) {
    InterlockedExchange64(&g_dl.done, (LONG64)done);
    InterlockedExchange64(&g_dl.total, (LONG64)total);
    if (InterlockedExchange(&g_dl.progress_posted, 1) == 0 &&
        !PostMessageA(g_dl.dlg, WM_DL_PROGRESS, 0, 0))
        InterlockedExchange(&g_dl.progress_posted, 0);
}

static void unpack_progress(void *ctx, unsigned long long done, unsigned long long total) {
    (void)ctx;
    dl_progress(done, total);
}

static me_7z_result download(char *err, size_t err_sz) {
    me_7z_result result = ME_7Z_FAILED;
    HINTERNET url = NULL;
    HANDLE f = INVALID_HANDLE_VALUE;
    char *buf = NULL;
    HINTERNET net = InternetOpenA("laggueless", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!net) {
        snprintf(err, err_sz, "Could not start the download (error %lu).", GetLastError());
        return ME_7Z_FAILED;
    }
    DWORD timeout = 30000;
    InternetSetOptionA(net, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionA(net, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
    /* No cache: the file is hundreds of MB and we keep our own copy. */
    url = InternetOpenUrlA(net, CORES_URL, NULL, 0,
                           INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
                           INTERNET_FLAG_NO_UI | INTERNET_FLAG_NO_COOKIES, 0);
    if (!url) {
        snprintf(err, err_sz, "Could not reach %s (error %lu).\nCheck your internet connection.",
                 CORES_HOST, GetLastError());
        goto out;
    }
    DWORD status = 0, len = sizeof(status);
    if (!HttpQueryInfoA(url, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &len, NULL) ||
        status != 200) {
        snprintf(err, err_sz, "%s answered with HTTP status %lu.", CORES_HOST, status);
        goto out;
    }
    char clen[32];
    unsigned long long total = 0, done = 0;
    len = sizeof(clen);
    if (HttpQueryInfoA(url, HTTP_QUERY_CONTENT_LENGTH, clen, &len, NULL))
        total = _strtoui64(clen, NULL, 10);

    f = CreateFileA(g_dl.archive, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        snprintf(err, err_sz, "Could not create %s (error %lu).", g_dl.archive, GetLastError());
        goto out;
    }
    enum { CHUNK = 1 << 18 };
    if (!(buf = (char *)malloc(CHUNK))) { snprintf(err, err_sz, "Out of memory."); goto out; }
    dl_progress(0, total);
    for (;;) {
        if (g_dl.cancel) { result = ME_7Z_CANCELLED; goto out; }
        DWORD got = 0, wrote = 0;
        if (!InternetReadFile(url, buf, CHUNK, &got)) {
            snprintf(err, err_sz, "The download stopped (error %lu).\nCheck your internet connection.",
                     GetLastError());
            goto out;
        }
        if (got == 0) break;
        if (!WriteFile(f, buf, got, &wrote, NULL) || wrote != got) {
            snprintf(err, err_sz, "Could not write %s (error %lu). Is the disk full?",
                     g_dl.archive, GetLastError());
            goto out;
        }
        done += got;
        dl_progress(done, total);
    }
    if (total && done != total) {
        snprintf(err, err_sz, "The download was cut short (%llu of %llu bytes).", done, total);
        goto out;
    }
    result = ME_7Z_OK;
out:
    free(buf);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (url) InternetCloseHandle(url);
    InternetCloseHandle(net);
    return result;
}

static DWORD WINAPI download_thread(LPVOID arg) {
    (void)arg;
    /* Background CPU and disk priority: the game beside us keeps its pacing. */
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    char err[512] = "", msg[1024] = "";
    me_7z_result r = download(err, sizeof(err));
    if (r == ME_7Z_OK) {
        InterlockedExchange(&g_dl.unpacking, 1);
        CreateDirectoryA(g_dl.cores_dir, NULL);
        int extracted = 0, in_use = 0;
        r = me_7z_extract_flat(g_dl.archive, g_dl.cores_dir, unpack_progress, NULL,
                               &g_dl.cancel, &extracted, &in_use, err, sizeof(err));
        if (r == ME_7Z_OK) {
            int n = snprintf(msg, sizeof(msg), "Installed %d cores in\n%s", extracted, g_dl.cores_dir);
            if (in_use && n > 0)
                snprintf(msg + n, sizeof(msg) - (size_t)n,
                         "\n\n%d cores could not be replaced because they are in use.", in_use);
        }
    }
    DeleteFileA(g_dl.archive);
    if (r == ME_7Z_FAILED) snprintf(msg, sizeof(msg), "Downloading cores failed.\n\n%s", err);
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);

    char *text = msg[0] ? _strdup(msg) : NULL;
    if (!PostMessageA(g_dl.dlg, WM_DL_DONE, (WPARAM)r, (LPARAM)text)) free(text);
    return 0;
}

static void dl_show_progress(HWND dlg) {
    InterlockedExchange(&g_dl.progress_posted, 0);
    if (g_dl.cancel) return;
    unsigned long long done = (unsigned long long)g_dl.done, total = (unsigned long long)g_dl.total;
    char text[160];
    const char *what = g_dl.unpacking ? "Unpacking cores" : "Downloading " CORES_HOST;
    if (total)
        snprintf(text, sizeof(text), "%s: %llu of %llu MB", what, done >> 20, total >> 20);
    else
        snprintf(text, sizeof(text), "%s: %llu MB", what, done >> 20);
    SetDlgItemTextA(dlg, IDC_DL_STATUS, text);
    int permille = total ? (int)(done * 1000 / total) : 0;
    SendDlgItemMessageA(dlg, IDC_DL_BAR, PBM_SETPOS, (WPARAM)permille, 0);
}

static void dl_cancel(HWND dlg) {
    if (g_dl.cancel) return;
    InterlockedExchange(&g_dl.cancel, 1);
    EnableWindow(GetDlgItem(dlg, IDCANCEL), FALSE);
    SetDlgItemTextA(dlg, IDC_DL_STATUS, "Cancelling...");
}

static INT_PTR CALLBACK download_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_INITDIALOG:
            me_ui_add_control(dlg, "STATIC", "Connecting to " CORES_HOST "...", SS_LEFT | SS_ENDELLIPSIS,
                              7, 7, 236, 10, IDC_DL_STATUS);
            me_ui_add_control(dlg, PROGRESS_CLASSA, "", 0, 7, 21, 236, 10, IDC_DL_BAR);
            SendDlgItemMessageA(dlg, IDC_DL_BAR, PBM_SETRANGE32, 0, 1000);
            me_ui_add_control(dlg, "BUTTON", "Cancel", BS_PUSHBUTTON | WS_TABSTOP, 188, 37, 55, 14, IDCANCEL);
            return TRUE;
        case WM_DL_PROGRESS:
            dl_show_progress(dlg);
            return TRUE;
        case WM_DL_DONE: {
            char *text = (char *)lp;
            WaitForSingleObject(g_dl.thread, INFINITE);   /* it has posted its last message */
            CloseHandle(g_dl.thread);
            g_dl.thread = NULL;
            DestroyWindow(dlg);
            g_dl.dlg = NULL;
            if (text && (me_7z_result)wp != ME_7Z_CANCELLED)
                MessageBoxA(me_platform_hwnd(), text, "Download Cores",
                            MB_OK | ((me_7z_result)wp == ME_7Z_OK ? MB_ICONINFORMATION : MB_ICONWARNING));
            free(text);
            return TRUE;
        }
        case WM_COMMAND:
            if (LOWORD(wp) == IDCANCEL) dl_cancel(dlg);
            return TRUE;
        case WM_CLOSE:
            dl_cancel(dlg);
            return TRUE;
    }
    return FALSE;
}

void me_ui_download_cores(HWND owner) {
    if (g_dl.dlg) {
        ShowWindow(g_dl.dlg, SW_RESTORE);
        SetForegroundWindow(g_dl.dlg);
        return;
    }
    char cores_dir[MAX_PATH], question[MAX_PATH + 512];
    get_cores_dir(cores_dir, sizeof(cores_dir));
    snprintf(question, sizeof(question),
             "Download cores from the internet?\n\n"
             "This downloads RetroArch_cores.7z (a few hundred MB) from %s and unpacks "
             "every core in it (about 2 GB) into:\n%s\n\n"
             "Cores already there with the same name are replaced. "
             "Your game keeps running while this happens.",
             CORES_HOST, cores_dir);
    if (MessageBoxA(owner, question, "Download Cores", MB_YESNO | MB_ICONQUESTION) != IDYES) return;

    me_ui_init_common_controls();
    memset(&g_dl, 0, sizeof(g_dl));
    snprintf(g_dl.cores_dir, sizeof(g_dl.cores_dir), "%s", cores_dir);
    char temp[MAX_PATH];
    DWORD n = GetTempPathA(sizeof(temp), temp);
    if (n == 0 || n >= sizeof(temp)) temp[0] = '\0';
    snprintf(g_dl.archive, sizeof(g_dl.archive), "%slaggueless_RetroArch_cores.7z", temp);

    /* Not owned by the game window, so it never sits on top of the game;
       it has its own taskbar button instead. */
    g_dl.dlg = me_ui_dialog_modeless(NULL, L"Download Cores", 250, 58, download_proc, 0);
    if (!g_dl.dlg) return;
    g_dl.thread = CreateThread(NULL, 0, download_thread, NULL, 0, NULL);
    if (!g_dl.thread) {
        DestroyWindow(g_dl.dlg);
        g_dl.dlg = NULL;
        MessageBoxA(owner, "Could not start the download.", "Download Cores", MB_OK | MB_ICONWARNING);
        return;
    }
    ShowWindow(g_dl.dlg, SW_SHOW);
}

/* ---- Set Cores ------------------------------------------------------------ */
#define WM_SC_END_EDIT (WM_APP + 40)

enum { IDC_SC_LIST = 1200, IDC_SC_HINT, IDC_SC_DEFAULT, IDC_SC_SAVE, IDC_SC_COMBO };
enum { COL_CONSOLE = 0, COL_CORE = 1, COL_TYPES = 2 };

#define SC_MAX_CONSOLES   ME_CONSOLE_CORES_MAX
#define SC_MAX_CANDIDATES 32

typedef struct {
    int  n;                                    /* consoles */
    char dll[SC_MAX_CONSOLES][128];            /* working copy, by console */
    char cores_dir[MAX_PATH];
    HWND list, combo;
    int  edit_row;                             /* console the combo edits */
    int  n_items;                              /* combo item -> DLL */
    char items[SC_MAX_CANDIDATES][128];
} cores_dlg;

static int core_installed(const cores_dlg *d, const char *dll) {
    char path[MAX_PATH * 2];
    snprintf(path, sizeof(path), "%s%s", d->cores_dir, dll);
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

static void core_label(const cores_dlg *d, const char *dll, char *out, size_t out_sz) {
    char name[128];
    me_core_display_name(dll, name, sizeof(name));
    snprintf(out, out_sz, "%s%s", name, core_installed(d, dll) ? "" : "  (not installed)");
}

static void sc_set_cell(HWND list, int row, int col, const char *text) {
    LVITEMA it = { .iSubItem = col, .pszText = (char *)text };
    SendMessageA(list, LVM_SETITEMTEXTA, (WPARAM)row, (LPARAM)&it);
}

static void sc_refresh_row(cores_dlg *d, int row) {
    char label[200];
    core_label(d, d->dll[row], label, sizeof(label));
    sc_set_cell(d->list, row, COL_CORE, label);
}

static void sc_fill_list(cores_dlg *d) {
    SendMessageA(d->list, LVM_DELETEALLITEMS, 0, 0);
    for (int i = 0; i < d->n; i++) {
        const me_console *c = me_console_at(i);
        LVITEMA it = { .mask = LVIF_TEXT, .iItem = i, .pszText = (char *)c->name };
        SendMessageA(d->list, LVM_INSERTITEMA, 0, (LPARAM)&it);
        char types[128];
        size_t len = 0;
        types[0] = '\0';
        const char *lists[2] = { c->exts, c->also_exts ? c->also_exts : "" };
        for (int l = 0; l < 2; l++) {
            for (const char *p = lists[l]; *p && len + 3 < sizeof(types); p++) {
                if (p == lists[l] || p[-1] == '|') { if (len) types[len++] = ' '; types[len++] = '.'; }
                if (*p != '|') types[len++] = *p;
            }
        }
        types[len] = '\0';
        sc_set_cell(d->list, i, COL_TYPES, types);
        sc_refresh_row(d, i);
    }
}

static void sc_end_edit(cores_dlg *d) {
    if (!d->combo) return;
    DestroyWindow(d->combo);
    d->combo = NULL;
    SetFocus(d->list);
}

/* Drop down the console's cores over its Core cell. */
static void sc_begin_edit(HWND dlg, cores_dlg *d, int row) {
    if (row < 0 || row >= d->n) return;
    sc_end_edit(d);
    const me_console *c = me_console_at(row);

    d->n_items = 0;
    int sel = -1;
    char dll[128];
    for (int i = 0; d->n_items < SC_MAX_CANDIDATES && me_console_candidate(c, i, dll, sizeof(dll)); i++) {
        if (_stricmp(dll, d->dll[row]) == 0) sel = d->n_items;
        snprintf(d->items[d->n_items++], sizeof(d->items[0]), "%s", dll);
    }
    /* A core set by hand in settings.yaml stays pickable. */
    if (sel < 0 && d->n_items < SC_MAX_CANDIDATES) {
        sel = d->n_items;
        memcpy(d->items[d->n_items++], d->dll[row], sizeof(d->items[0]));
    }

    RECT r = { .top = COL_CORE, .left = LVIR_BOUNDS };
    SendMessageA(d->list, LVM_ENSUREVISIBLE, (WPARAM)row, FALSE);
    SendMessageA(d->list, LVM_GETSUBITEMRECT, (WPARAM)row, (LPARAM)&r);
    MapWindowPoints(d->list, dlg, (POINT *)&r, 2);
    d->combo = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST,
                               r.left, r.top, r.right - r.left, 300, dlg,
                               (HMENU)(INT_PTR)IDC_SC_COMBO, GetModuleHandleA(NULL), NULL);
    if (!d->combo) return;
    HFONT font = (HFONT)SendMessageA(dlg, WM_GETFONT, 0, 0);
    SendMessageA(d->combo, WM_SETFONT, (WPARAM)font, TRUE);
    /* The list drops down wider than the cell when a name needs it. */
    HDC dc = GetDC(d->combo);
    HGDIOBJ old_font = SelectObject(dc, font);
    int widest = r.right - r.left;
    for (int i = 0; i < d->n_items; i++) {
        char label[200];
        SIZE sz;
        core_label(d, d->items[i], label, sizeof(label));
        SendMessageA(d->combo, CB_ADDSTRING, 0, (LPARAM)label);
        if (GetTextExtentPoint32A(dc, label, (int)strlen(label), &sz)) {
            int w = sz.cx + GetSystemMetrics(SM_CXVSCROLL) + 12;
            if (w > widest) widest = w;
        }
    }
    SelectObject(dc, old_font);
    ReleaseDC(d->combo, dc);
    SendMessageA(d->combo, CB_SETDROPPEDWIDTH, (WPARAM)widest, 0);
    SendMessageA(d->combo, CB_SETCURSEL, (WPARAM)sel, 0);
    d->edit_row = row;
    SetWindowPos(d->combo, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetFocus(d->combo);
    SendMessageA(d->combo, CB_SHOWDROPDOWN, TRUE, 0);
}

static void sc_apply(me_settings *s, const cores_dlg *d) {
    for (int i = 0; i < d->n; i++) {
        const me_console *c = me_console_at(i);
        char ours[128];
        me_console_candidate(c, 0, ours, sizeof(ours));
        me_settings_set_console_core(s, c->id, _stricmp(d->dll[i], ours) == 0 ? NULL : d->dll[i]);
    }
}

static void patch_console_cores(me_settings *s, const void *ctx) {
    sc_apply(s, (const cores_dlg *)ctx);
}

static int sc_selected_row(const cores_dlg *d) {
    return (int)SendMessageA(d->list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
}

static void sc_create_controls(HWND dlg, cores_dlg *d) {
    d->list = me_ui_add_control(dlg, WC_LISTVIEWA, "",
                                LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER |
                                WS_BORDER | WS_TABSTOP | WS_CLIPSIBLINGS,
                                7, 7, 306, 200, IDC_SC_LIST);
    SendMessageA(d->list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                 LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    RECT lr;
    GetClientRect(d->list, &lr);
    int lw = lr.right - GetSystemMetrics(SM_CXVSCROLL);
    static const char *const heads[3] = { "Console", "Core", "ROM types" };
    const int widths[3] = { lw * 34 / 100, lw * 38 / 100, lw - lw * 34 / 100 - lw * 38 / 100 };
    for (int c = 0; c < 3; c++) {
        LVCOLUMNA col = { .mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM,
                          .cx = widths[c], .pszText = (char *)heads[c], .iSubItem = c };
        SendMessageA(d->list, LVM_INSERTCOLUMNA, (WPARAM)c, (LPARAM)&col);
    }
    sc_fill_list(d);

    me_ui_add_control(dlg, "STATIC",
                      "Click a console to pick the core its games open in. Changes apply the next "
                      "time a game is opened. Cores > Download Cores gets ones not installed.",
                      SS_LEFT, 7, 212, 306, 26, IDC_SC_HINT);
    me_ui_add_control(dlg, "BUTTON", "Default", BS_PUSHBUTTON | WS_TABSTOP | WS_GROUP, 7, 243, 55, 14, IDC_SC_DEFAULT);
    me_ui_add_control(dlg, "BUTTON", "Cancel",  BS_PUSHBUTTON | WS_TABSTOP, 199, 243, 55, 14, IDCANCEL);
    me_ui_add_control(dlg, "BUTTON", "Save",    BS_PUSHBUTTON | WS_TABSTOP, 258, 243, 55, 14, IDC_SC_SAVE);
}

static INT_PTR CALLBACK set_cores_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    cores_dlg *d = (cores_dlg *)GetWindowLongPtrW(dlg, DWLP_USER);
    switch (msg) {
        case WM_INITDIALOG:
            d = (cores_dlg *)lp;
            SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)d);
            sc_create_controls(dlg, d);
            return TRUE;

        case WM_NOTIFY: {
            NMHDR *nh = (NMHDR *)lp;
            if (nh->idFrom != IDC_SC_LIST) break;
            if (nh->code == NM_CLICK || nh->code == NM_DBLCLK) {
                const NMITEMACTIVATE *ia = (const NMITEMACTIVATE *)lp;
                if (ia->iItem >= 0) sc_begin_edit(dlg, d, ia->iItem);
                return TRUE;
            }
            if (nh->code == LVN_KEYDOWN) {
                WORD vk = ((const NMLVKEYDOWN *)lp)->wVKey;
                if (vk == VK_SPACE || vk == VK_F2) sc_begin_edit(dlg, d, sc_selected_row(d));
                return TRUE;
            }
            if (nh->code == LVN_BEGINSCROLL) sc_end_edit(d);
            break;
        }

        case WM_SC_END_EDIT:
            sc_end_edit(d);
            return TRUE;

        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_SC_COMBO:
                    if (HIWORD(wp) == CBN_SELENDOK && d->combo) {
                        LRESULT sel = SendMessageA(d->combo, CB_GETCURSEL, 0, 0);
                        if (sel >= 0 && sel < d->n_items) {
                            memcpy(d->dll[d->edit_row], d->items[sel], sizeof(d->dll[0]));
                            sc_refresh_row(d, d->edit_row);
                        }
                    } else if (HIWORD(wp) == CBN_CLOSEUP || HIWORD(wp) == CBN_KILLFOCUS) {
                        /* Not from inside the combo's own notification. */
                        PostMessageA(dlg, WM_SC_END_EDIT, 0, 0);
                    }
                    return TRUE;
                case IDC_SC_DEFAULT:
                    sc_end_edit(d);
                    for (int i = 0; i < d->n; i++)
                        me_console_candidate(me_console_at(i), 0, d->dll[i], sizeof(d->dll[0]));
                    sc_fill_list(d);
                    return TRUE;
                case IDC_SC_SAVE:
                    sc_end_edit(d);
                    me_settings_lock();
                    sc_apply(me_app_settings(), d);
                    me_settings_unlock();
                    me_ui_persist(patch_console_cores, d);
                    EndDialog(dlg, IDOK);
                    return TRUE;
                case IDOK:
                    /* Enter: open the selected console's cores. */
                    if (GetFocus() == d->list) sc_begin_edit(dlg, d, sc_selected_row(d));
                    return TRUE;
                case IDCANCEL:
                    EndDialog(dlg, IDCANCEL);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

void me_ui_set_cores_dialog(HWND owner) {
    me_ui_init_common_controls();
    static cores_dlg d;  /* one dialog at a time on the UI thread */
    memset(&d, 0, sizeof(d));
    get_cores_dir(d.cores_dir, sizeof(d.cores_dir));
    d.n = me_console_count();
    if (d.n > SC_MAX_CONSOLES) d.n = SC_MAX_CONSOLES;

    me_settings *live = me_app_settings();
    me_settings_lock();
    for (int i = 0; i < d.n; i++) me_console_core_dll(live, me_console_at(i), d.dll[i], sizeof(d.dll[0]));
    me_settings_unlock();

    me_ui_dialog(owner, L"Set Cores", 320, 264, set_cores_proc, (LPARAM)&d);
}
