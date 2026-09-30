/* Cores > Firmware: the system files (BIOS...) the cores the player's
   consoles open in need to start games, and whether each is in the firmware
   folder. Only required files are listed (rom_cores.c k_core_files), for
   the core each console opens in now (Cores > Set Cores): a console whose
   core needs none isn't listed. Modal to the UI thread only. */

#include "ui.h"
#include <commctrl.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>
#include "app.h"
#include "rom_cores.h"

enum { IDC_FW_HINT = 1500, IDC_FW_DIR, IDC_FW_LIST, IDC_FW_DETAIL, IDC_FW_SUMMARY, IDC_FW_OPEN,
       IDC_FW_CHECK };
enum { COL_CONSOLE, COL_WHAT, COL_FILES, COL_STATUS };

typedef struct {
    char dir[MAX_PATH];   /* the firmware folder */
    HWND list;
} fw_dlg;

static void set_cell(HWND list, int row, int col, const char *text) {
    LVITEMA it = { .iSubItem = col, .pszText = (char *)text };
    SendMessageA(list, LVM_SETITEMTEXTA, (WPARAM)row, (LPARAM)&it);
}

/* The selected row in full, under the list: the columns cut long names. */
static void fw_show_detail(HWND dlg, fw_dlg *d, int row) {
    char cols[4][192], text[900] = "";
    for (int c = 0; row >= 0 && c < 4; c++) {
        LVITEMA it = { .iSubItem = c, .pszText = cols[c], .cchTextMax = sizeof(cols[c]) };
        SendMessageA(d->list, LVM_GETITEMTEXTA, (WPARAM)row, (LPARAM)&it);
    }
    if (row >= 0)
        snprintf(text, sizeof(text), "%s, for %s: %s.\n%s.", cols[COL_WHAT], cols[COL_CONSOLE],
                 cols[COL_FILES], cols[COL_STATUS]);
    SetDlgItemTextA(dlg, IDC_FW_DETAIL, text);
}

/* One row per file; the row's lParam is 1 when it's missing. */
static void fw_fill(HWND dlg, fw_dlg *d) {
    SendMessageA(d->list, LVM_DELETEALLITEMS, 0, 0);
    int rows = 0, missing = 0, first_missing = -1;
    me_settings *live = me_app_settings();
    for (int i = 0; i < me_console_count(); i++) {
        const me_console *c = me_console_at(i);
        char dll[MAX_PATH], core[128], console[192];
        me_settings_lock();   /* the emulation thread reads the core picks */
        me_console_core_dll(live, c, dll, sizeof(dll));
        me_settings_unlock();
        me_firmware_file files[8];
        int n = me_core_firmware(dll, c->id, d->dir, files, 8);
        if (!n) continue;
        me_core_display_name(dll, core, sizeof(core));
        snprintf(console, sizeof(console), "%s (%s)", c->name, core);
        for (int f = 0; f < n; f++) {
            /* A file only some games need isn't "missing" (nor red). */
            int lacking = !files[f].found[0] && !files[f].some_games;
            char status[160];
            if (files[f].found[0])   snprintf(status, sizeof(status), "Found: %s", files[f].found);
            else if (lacking)        snprintf(status, sizeof(status), "Missing");
            else                     snprintf(status, sizeof(status), "Not there (for those games)");
            LVITEMA it = { .mask = LVIF_TEXT | LVIF_PARAM, .iItem = rows,
                           .pszText = console, .lParam = lacking };
            SendMessageA(d->list, LVM_INSERTITEMA, 0, (LPARAM)&it);
            set_cell(d->list, rows, COL_WHAT, files[f].label);
            set_cell(d->list, rows, COL_FILES, files[f].show);
            set_cell(d->list, rows, COL_STATUS, status);
            if (lacking && first_missing < 0) first_missing = rows;
            rows++;
            missing += lacking;
        }
    }
    /* Start on the first missing file (else the first row). */
    int sel = first_missing >= 0 ? first_missing : (rows ? 0 : -1);
    if (sel >= 0) {
        LVITEMA it = { .stateMask = LVIS_SELECTED | LVIS_FOCUSED, .state = LVIS_SELECTED | LVIS_FOCUSED };
        SendMessageA(d->list, LVM_SETITEMSTATE, (WPARAM)sel, (LPARAM)&it);
    }
    fw_show_detail(dlg, d, sel);
    char summary[160];
    if (!rows)
        snprintf(summary, sizeof(summary), "The cores your consoles open in need no system files.");
    else if (!missing)
        snprintf(summary, sizeof(summary), "Everything is there.");
    else
        snprintf(summary, sizeof(summary), "%d file%s missing: those consoles' games won't start, "
                 "or won't run right, until %s added.", missing, missing == 1 ? " is" : "s are",
                 missing == 1 ? "it's" : "they're");
    SetDlgItemTextA(dlg, IDC_FW_SUMMARY, summary);
}

static void fw_create_controls(HWND dlg, fw_dlg *d) {
    me_ui_add_control(dlg, "STATIC",
                      "Some consoles need their own system files (BIOS) to start games. Those come "
                      "from the consoles, so laggueless can't include them: copy yours into this "
                      "folder, named as below. Only what the cores you use (Cores > Set Cores) need "
                      "is listed.",
                      SS_LEFT, 7, 7, 486, 18, IDC_FW_HINT);
    me_ui_add_control(dlg, "STATIC", d->dir, SS_LEFT | SS_NOPREFIX | SS_PATHELLIPSIS,
                      7, 29, 486, 9, IDC_FW_DIR);
    d->list = me_ui_add_control(dlg, WC_LISTVIEWA, "",
                                LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER |
                                WS_BORDER | WS_TABSTOP,
                                7, 42, 486, 124, IDC_FW_LIST);
    SendMessageA(d->list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                 LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    RECT lr;
    GetClientRect(d->list, &lr);
    int lw = lr.right - GetSystemMetrics(SM_CXVSCROLL);
    static const char *const heads[4] = { "Console", "Needs", "File", "Status" };
    const int widths[4] = { lw * 23 / 100, lw * 20 / 100, lw * 35 / 100,
                            lw - lw * 23 / 100 - lw * 20 / 100 - lw * 35 / 100 };
    for (int c = 0; c < 4; c++) {
        LVCOLUMNA col = { .mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM,
                          .cx = widths[c], .pszText = (char *)heads[c], .iSubItem = c };
        SendMessageA(d->list, LVM_INSERTCOLUMNA, (WPARAM)c, (LPARAM)&col);
    }
    me_ui_add_control(dlg, "STATIC", "", SS_LEFT | SS_NOPREFIX, 7, 170, 486, 18, IDC_FW_DETAIL);
    me_ui_add_control(dlg, "STATIC", "", SS_LEFT, 7, 194, 486, 18, IDC_FW_SUMMARY);
    me_ui_add_control(dlg, "BUTTON", "Open Folder", BS_PUSHBUTTON | WS_TABSTOP | WS_GROUP,
                      7, 219, 60, 14, IDC_FW_OPEN);
    me_ui_add_control(dlg, "BUTTON", "Check Again", BS_PUSHBUTTON | WS_TABSTOP, 71, 219, 60, 14, IDC_FW_CHECK);
    me_ui_add_control(dlg, "BUTTON", "Close", BS_DEFPUSHBUTTON | WS_TABSTOP, 438, 219, 55, 14, IDCANCEL);
    fw_fill(dlg, d);
}

static INT_PTR CALLBACK firmware_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    fw_dlg *d = (fw_dlg *)GetWindowLongPtrW(dlg, DWLP_USER);
    switch (msg) {
        case WM_INITDIALOG:
            d = (fw_dlg *)lp;
            SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)d);
            fw_create_controls(dlg, d);
            return TRUE;

        case WM_NOTIFY: {
            const NMHDR *nh = (const NMHDR *)lp;
            if (nh->idFrom != IDC_FW_LIST) break;
            if (nh->code == LVN_ITEMCHANGED) {
                const NMLISTVIEW *lv = (const NMLISTVIEW *)lp;
                if ((lv->uChanged & LVIF_STATE) && (lv->uNewState & LVIS_SELECTED))
                    fw_show_detail(dlg, d, lv->iItem);
                return TRUE;
            }
            /* Missing files in red. */
            NMLVCUSTOMDRAW *cd = (NMLVCUSTOMDRAW *)lp;
            if (nh->code != NM_CUSTOMDRAW) break;
            LRESULT r = CDRF_DODEFAULT;
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) {
                r = CDRF_NOTIFYITEMDRAW;
            } else if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT && cd->nmcd.lItemlParam) {
                cd->clrText = RGB(192, 0, 0);
                r = CDRF_NEWFONT;
            }
            SetWindowLongPtrW(dlg, DWLP_MSGRESULT, r);
            return TRUE;
        }

        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_FW_OPEN:
                    CreateDirectoryA(d->dir, NULL);
                    ShellExecuteA(dlg, "open", d->dir, NULL, NULL, SW_SHOWNORMAL);
                    return TRUE;
                case IDC_FW_CHECK:
                    fw_fill(dlg, d);
                    return TRUE;
                case IDOK:
                case IDCANCEL:
                    EndDialog(dlg, IDCANCEL);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

void me_ui_firmware_dialog(HWND owner) {
    me_ui_init_common_controls();
    fw_dlg d = {0};
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, sizeof(exe));
    char *slash = n ? strrchr(exe, '\\') : NULL;
    if (slash) slash[1] = '\0'; else exe[0] = '\0';
    snprintf(d.dir, sizeof(d.dir), "%sfirmware", exe);
    me_ui_dialog(owner, L"Firmware", 500, 240, firmware_proc, (LPARAM)&d);
}
