/* Cores > File Associations: make laggueless the program Windows opens a
   console's games in when they're double-clicked.

   Each console gets a ProgID, laggueless.<id> ("laggueless.nes"), whose open
   command is this exe with the ROM as its one argument, and each of the
   console's extensions points at it. That always goes in the current user's
   classes (HKCU\Software\Classes), which need no permission and beat the
   machine's, where other programs often register per user too. With "Also
   for other users" ticked it goes in the machine's (HKLM) as well, which
   needs an administrator: Save then runs a copy of this exe elevated
   (Windows shows the UAC prompt) with --file-types-all-users, which does just
   that part and exits. laggueless itself never has to run as administrator.
   Another user's own per-user registrations still beat the machine's for
   them.

   A type the player picked a program for with Explorer's "Open with" has a
   UserChoice, which beats both; Save deletes the current user's UserChoice
   where it names another program. Anything Windows still won't give us
   afterwards (a policy, a protected type) is listed, with a way to Default
   apps.

   The previous default of each extension is kept beside ours and put back
   when its console is unticked. Some extensions belong to other programs
   too (.md is Markdown, .iso a disc image Windows mounts...); those are left
   alone unless the player asks for them. */

#include "ui.h"
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include "rom_cores.h"

#define CLASSES    L"Software\\Classes\\"
#define FILE_EXTS  L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\"
#define PROGID_PFX L"laggueless."
#define BACKUP     L"laggueless.previous"   /* value on .ext: its default before ours */
#define APP_NAME   L"laggueless"

/* Extensions other programs commonly use as well. */
static const char k_shared_exts[] = "md|mdx|bin|iso|cue|m3u|toc|dmg|3ds|vb|fig|st|sc|lyx";

#define FT_MAX ME_CONSOLE_CORES_MAX

/* ---- names ----------------------------------------------------------------- */
static const wchar_t *exe_path(void) {
    static wchar_t exe[MAX_PATH * 2];
    if (!exe[0]) {
        wchar_t raw[MAX_PATH * 2];
        DWORD n = GetModuleFileNameW(NULL, raw, MAX_PATH * 2);
        if (n == 0 || n >= MAX_PATH * 2) return L"";
        if (!GetLongPathNameW(raw, exe, MAX_PATH * 2)) wcscpy(exe, raw);
    }
    return exe;
}

/* "Applications\laggueless.exe": the ProgId Explorer's "Open with" gives
   us when the player browses to the exe. */
static void open_with_progid(wchar_t *out, size_t out_n) {
    const wchar_t *exe = exe_path(), *base = exe;
    for (const wchar_t *p = exe; *p; p++) if (*p == L'\\' || *p == L'/') base = p + 1;
    swprintf(out, out_n, L"Applications\\%ls", base);
}

static void open_command(wchar_t *out, size_t out_n) {
    swprintf(out, out_n, L"\"%ls\" \"%%1\"", exe_path());
}

static void widen(const char *s, wchar_t *out, size_t out_n) {
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, out, (int)out_n)) out[0] = 0;
}

static void progid_of(const me_console *c, wchar_t *out, size_t out_n) {
    wchar_t id[32];
    widen(c->id, id, 32);
    swprintf(out, out_n, PROGID_PFX L"%ls", id);
}

/* The console's `i`th extension with its dot (".nes"). Returns 0 past the last. */
static int console_ext(const me_console *c, int i, wchar_t *out, size_t out_n) {
    const char *p = c->exts;
    for (; i > 0 && p; i--) {
        p = strchr(p, '|');
        if (p) p++;
    }
    if (!p || !*p) return 0;
    size_t len = strcspn(p, "|");
    if (len + 2 > out_n) len = out_n - 2;
    out[0] = L'.';
    for (size_t k = 0; k < len; k++) out[k + 1] = (wchar_t)(unsigned char)p[k];
    out[len + 1] = 0;
    return 1;
}

static int is_shared(const wchar_t *ext) {
    const char *p = k_shared_exts;
    size_t n = wcslen(ext + 1);
    while (*p) {
        size_t len = strcspn(p, "|");
        if (len == n) {
            size_t k = 0;
            while (k < n && towlower(ext[1 + k]) == (wchar_t)(unsigned char)p[k]) k++;
            if (k == n) return 1;
        }
        p += len;
        if (*p) p++;
    }
    return 0;
}

/* Does ticking the console take `ext`? */
static int ext_wanted(const wchar_t *ext, int shared) { return shared || !is_shared(ext); }

/* ---- registry ---------------------------------------------------------------- */
static void reg_get(HKEY root, const wchar_t *path, const wchar_t *name, wchar_t *out, DWORD out_n) {
    DWORD sz = out_n * sizeof(wchar_t);
    if (RegGetValueW(root, path, name, RRF_RT_REG_SZ, NULL, out, &sz) != ERROR_SUCCESS) out[0] = 0;
}

static LSTATUS set_sz(HKEY k, const wchar_t *sub, const wchar_t *name, const wchar_t *v) {
    return RegSetKeyValueW(k, sub, name, REG_SZ, v, (DWORD)((wcslen(v) + 1) * sizeof(wchar_t)));
}

static int key_exists(HKEY root, const wchar_t *path) {
    HKEY k;
    if (RegOpenKeyExW(root, path, 0, KEY_READ, &k) != ERROR_SUCCESS) return 0;
    RegCloseKey(k);
    return 1;
}

static void delete_if_empty(HKEY root, const wchar_t *path) {
    HKEY k;
    DWORD subkeys = 1, values = 1;
    if (RegOpenKeyExW(root, path, 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    RegQueryInfoKeyW(k, NULL, NULL, NULL, &subkeys, NULL, NULL, &values, NULL, NULL, NULL, NULL);
    RegCloseKey(k);
    if (subkeys == 0 && values == 0) RegDeleteKeyW(root, path);
}

/* Point `ext` at `progid`, keeping what it pointed at before. */
static int claim_ext(HKEY root, const wchar_t *ext, const wchar_t *progid) {
    wchar_t path[64], cur[256];
    swprintf(path, 64, CLASSES L"%ls", ext);
    HKEY k;
    if (RegCreateKeyExW(root, path, 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS)
        return 0;
    reg_get(k, NULL, NULL, cur, 256);
    LSTATUS r = ERROR_SUCCESS;
    if (_wcsicmp(cur, progid) != 0) {
        if (cur[0]) r = set_sz(k, NULL, BACKUP, cur);
        else        RegDeleteValueW(k, BACKUP);
        if (r == ERROR_SUCCESS) r = set_sz(k, NULL, NULL, progid);
    }
    if (r == ERROR_SUCCESS) r = RegSetKeyValueW(k, L"OpenWithProgids", progid, REG_NONE, NULL, 0);
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

/* Undo claim_ext: put the previous default back if `ext` still points at us. */
static int release_ext(HKEY root, const wchar_t *ext, const wchar_t *progid) {
    wchar_t path[64], cur[256], prev[256];
    swprintf(path, 64, CLASSES L"%ls", ext);
    HKEY k;
    LSTATUS r = RegOpenKeyExW(root, path, 0, KEY_READ | KEY_WRITE, &k);
    if (r == ERROR_FILE_NOT_FOUND) return 1;
    if (r != ERROR_SUCCESS) return 0;
    RegDeleteKeyValueW(k, L"OpenWithProgids", progid);
    reg_get(k, NULL, NULL, cur, 256);
    if (_wcsicmp(cur, progid) == 0) {
        reg_get(k, NULL, BACKUP, prev, 256);
        r = prev[0] ? set_sz(k, NULL, NULL, prev) : RegDeleteValueW(k, NULL);
        if (r == ERROR_FILE_NOT_FOUND) r = ERROR_SUCCESS;
    }
    RegDeleteValueW(k, BACKUP);
    delete_if_empty(k, L"OpenWithProgids");
    RegCloseKey(k);
    delete_if_empty(root, path);
    return r == ERROR_SUCCESS;
}

static int write_progid(HKEY root, const me_console *c, const wchar_t *progid) {
    wchar_t path[64], name[96], type_name[128], cmd[MAX_PATH * 2 + 16];
    swprintf(path, 64, CLASSES L"%ls", progid);
    widen(c->name, name, 96);
    swprintf(type_name, 128, L"%ls game", name);
    open_command(cmd, MAX_PATH * 2 + 16);
    HKEY k;
    if (RegCreateKeyExW(root, path, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS) return 0;
    LSTATUS r = set_sz(k, NULL, NULL, type_name);
    if (r == ERROR_SUCCESS) r = set_sz(k, L"shell\\open", L"FriendlyAppName", APP_NAME);
    if (r == ERROR_SUCCESS) r = set_sz(k, L"shell\\open\\command", NULL, cmd);
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

/* Make `root`'s classes match: console ticked (`on`) = its ProgID written and
   the extensions it takes pointed at it; everything else of ours released.
   Returns how many writes failed. */
static int sync_console(HKEY root, const me_console *c, int on, int shared) {
    wchar_t progid[48], ext[16], path[64];
    progid_of(c, progid, 48);
    int any = 0, fails = 0;
    for (int i = 0; console_ext(c, i, ext, 16); i++) any |= on && ext_wanted(ext, shared);
    if (any) fails += !write_progid(root, c, progid);
    for (int i = 0; console_ext(c, i, ext, 16); i++) {
        int want = on && ext_wanted(ext, shared);
        fails += !(want ? claim_ext(root, ext, progid) : release_ext(root, ext, progid));
    }
    if (!any) {
        swprintf(path, 64, CLASSES L"%ls", progid);
        LSTATUS r = RegDeleteTreeW(root, path);
        if (r != ERROR_SUCCESS && r != ERROR_FILE_NOT_FOUND) fails++;
        else RegDeleteKeyW(root, path);
    }
    return fails;
}

/* Would sync_console change anything? Reads only. */
static int in_sync(HKEY root, const me_console *c, int on, int shared) {
    wchar_t progid[48], ext[16], path[96], cur[MAX_PATH * 2 + 16], cmd[MAX_PATH * 2 + 16];
    progid_of(c, progid, 48);
    int any = 0;
    for (int i = 0; console_ext(c, i, ext, 16); i++) {
        int want = on && ext_wanted(ext, shared);
        any |= want;
        swprintf(path, 96, CLASSES L"%ls", ext);
        reg_get(root, path, NULL, cur, 256);
        if ((_wcsicmp(cur, progid) == 0) != want) return 0;
    }
    swprintf(path, 96, CLASSES L"%ls", progid);
    if (!any) return !key_exists(root, path);
    swprintf(path, 96, CLASSES L"%ls\\shell\\open\\command", progid);
    reg_get(root, path, NULL, cur, MAX_PATH * 2 + 16);
    open_command(cmd, MAX_PATH * 2 + 16);
    return _wcsicmp(cur, cmd) == 0;
}

/* The current user's "Open with" pick for `ext` wins over both classes
   roots: delete it where it disagrees with what we want. The key denies
   setting values but not deleting it. Windows 11 keeps a second one. */
static void fix_user_choice(const wchar_t *ext, const wchar_t *progid, int want) {
    static const wchar_t *const keys[] = { L"UserChoice", L"UserChoiceLatest" };
    wchar_t path[160], cur[256], app[MAX_PATH + 16];
    open_with_progid(app, MAX_PATH + 16);
    for (int i = 0; i < 2; i++) {
        swprintf(path, 160, FILE_EXTS L"%ls\\%ls", ext, keys[i]);
        if (!key_exists(HKEY_CURRENT_USER, path)) continue;
        reg_get(HKEY_CURRENT_USER, path, L"ProgId", cur, 256);
        int ours = cur[0] && (_wcsicmp(cur, progid) == 0 || _wcsicmp(cur, app) == 0);
        if (want == ours) continue;
        if (RegDeleteKeyW(HKEY_CURRENT_USER, path) != ERROR_SUCCESS)
            RegDeleteTreeW(HKEY_CURRENT_USER, path);
    }
}

/* ---- what Windows opens a type with, for the current user ------------------ */
/* 1 = us; 0 = another program, named in `app`; -1 = nothing. */
static int ext_opener(const wchar_t *ext, wchar_t *app, DWORD app_n) {
    wchar_t exe[MAX_PATH * 2], exe_long[MAX_PATH * 2];
    DWORD n = MAX_PATH * 2;
    app[0] = 0;
    int have_exe = SUCCEEDED(AssocQueryStringW(ASSOCF_INIT_IGNOREUNKNOWN, ASSOCSTR_EXECUTABLE,
                                               ext, NULL, exe, &n));
    if (have_exe) {
        if (!GetLongPathNameW(exe, exe_long, MAX_PATH * 2)) wcscpy(exe_long, exe);
        if (_wcsicmp(exe_long, exe_path()) == 0) return 1;
    }
    n = app_n;
    if (SUCCEEDED(AssocQueryStringW(ASSOCF_INIT_IGNOREUNKNOWN, ASSOCSTR_FRIENDLYAPPNAME,
                                    ext, NULL, app, &n)) && app[0]) {
        /* Our ProgID, but a copy of laggueless in another folder. */
        if (have_exe && _wcsicmp(app, APP_NAME) == 0) swprintf(app, app_n, L"laggueless (%ls)", exe);
        return 0;
    }
    if (!have_exe) return -1;
    const wchar_t *base = exe;
    for (const wchar_t *p = exe; *p; p++) if (*p == L'\\' || *p == L'/') base = p + 1;
    swprintf(app, app_n, L"%ls", base);
    return 0;
}

/* The "Opens in now" cell for the extensions ticking the console would take.
   Returns 1 when all of them (and at least one) open in us. */
static int console_opener(const me_console *c, int shared, wchar_t *label, size_t label_n) {
    wchar_t ext[16], app[160], other[160] = L"";
    int n = 0, ours = 0, none = 0, differ = 0;
    for (int i = 0; console_ext(c, i, ext, 16); i++) {
        if (!ext_wanted(ext, shared)) continue;
        n++;
        int r = ext_opener(ext, app, 160);
        if (r > 0) ours++;
        else if (r < 0) none++;
        else if (!other[0]) wcscpy(other, app);
        else if (_wcsicmp(other, app) != 0) differ = 1;
    }
    if (n == 0)             label[0] = 0;
    else if (ours == n)     swprintf(label, label_n, L"laggueless");
    else if (ours > 0)      swprintf(label, label_n, L"laggueless for %d of %d types", ours, n);
    else if (none == n)     swprintf(label, label_n, L"nothing");
    else if (!differ)       swprintf(label, label_n, L"%ls", other);
    else                    swprintf(label, label_n, L"several programs");
    return n > 0 && ours == n;
}

/* ---- elevated copy ------------------------------------------------------------ */
int me_file_types_all_users_main(int argc, char **argv) {
    int n = me_console_count(), shared = 0;
    unsigned char on[FT_MAX] = {0};
    if (n > FT_MAX) n = FT_MAX;
    for (int a = 0; a < argc; a++) {
        if (strcmp(argv[a], "--shared") == 0) { shared = 1; continue; }
        int found = 0;
        for (int i = 0; i < n && !found; i++)
            if (strcmp(argv[a], me_console_at(i)->id) == 0) on[i] = found = 1;
        if (!found) return 1000;   /* nothing changed */
    }
    int fails = 0;
    for (int i = 0; i < n; i++) fails += sync_console(HKEY_LOCAL_MACHINE, me_console_at(i), on[i], shared);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
    return fails;
}

/* ---- dialog ------------------------------------------------------------------- */
#define WM_FT_ELEVATED (WM_APP + 50)   /* wp: exit code of the elevated copy, or ELEV_* */
#define ELEV_DECLINED  (-1)
#define ELEV_FAILED    (-2)

enum { IDC_FT_HINT = 1300, IDC_FT_LIST, IDC_FT_SHARED, IDC_FT_ALL_USERS, IDC_FT_ALL, IDC_FT_NONE };
enum { COL_CONSOLE = 0, COL_TYPES = 1, COL_OPENS = 2 };

static const char k_hint[] =
    "Tick the consoles whose games should open in laggueless when you double-click them "
    "in Explorer. Types in brackets are also used by other programs.";

typedef struct {
    int    n;
    HWND   list;
    int    filling;              /* list being filled: its notifications aren't the player */
    int    elevated;             /* this process already runs as administrator */
    int    busy;                 /* waiting on the elevated copy */
    /* What Save applies, read from the controls. */
    int    shared, all_users;
    unsigned char on[FT_MAX];
    HWND   dlg;
    HANDLE thread;
    wchar_t params[1024];
} ft_dlg;

static int process_elevated(void) {
    HANDLE tok;
    TOKEN_ELEVATION e;
    DWORD n;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return 0;
    int r = GetTokenInformation(tok, TokenElevation, &e, sizeof(e), &n) && e.TokenIsElevated;
    CloseHandle(tok);
    return r;
}

static void ft_read_controls(HWND dlg, ft_dlg *d) {
    d->shared    = IsDlgButtonChecked(dlg, IDC_FT_SHARED) == BST_CHECKED;
    d->all_users = IsDlgButtonChecked(dlg, IDC_FT_ALL_USERS) == BST_CHECKED;
    for (int i = 0; i < d->n; i++) d->on[i] = ListView_GetCheckState(d->list, i) ? 1 : 0;
}

/* Does Save need to change HKLM (and so an administrator)? */
static int ft_machine_needed(const ft_dlg *d) {
    for (int i = 0; i < d->n; i++)
        if (!in_sync(HKEY_LOCAL_MACHINE, me_console_at(i), d->all_users && d->on[i], d->shared))
            return 1;
    return 0;
}

/* The UAC shield on Save when saving will ask for permission. */
static void ft_update_shield(HWND dlg, ft_dlg *d) {
    if (d->filling || d->elevated) return;
    ft_read_controls(dlg, d);
    SendDlgItemMessageW(dlg, IDOK, BCM_SETSHIELD, 0, ft_machine_needed(d));
}

static void ft_set_cell(HWND list, int row, int col, const wchar_t *text) {
    LVITEMW it = { .iSubItem = col, .pszText = (wchar_t *)text };
    SendMessageW(list, LVM_SETITEMTEXTW, (WPARAM)row, (LPARAM)&it);
}

/* ROM types and Opens-in columns for the Shared checkbox as it is now.
   Returns 1 when the console's types already all open in us. */
static int ft_refresh_row(HWND dlg, ft_dlg *d, int row) {
    const me_console *c = me_console_at(row);
    int shared = IsDlgButtonChecked(dlg, IDC_FT_SHARED) == BST_CHECKED;
    wchar_t types[160] = L"", ext[16], opens[200];
    size_t len = 0;
    for (int i = 0; console_ext(c, i, ext, 16) && len + 24 < 160; i++) {
        int w = swprintf(types + len, 160 - len, ext_wanted(ext, shared) ? L"%ls%ls" : L"%ls(%ls)",
                         len ? L" " : L"", ext);
        if (w > 0) len += (size_t)w;
    }
    ft_set_cell(d->list, row, COL_TYPES, types);
    int ours = console_opener(c, shared, opens, 200);
    ft_set_cell(d->list, row, COL_OPENS, opens);
    return ours;
}

static void ft_create_controls(HWND dlg, ft_dlg *d) {
    me_ui_add_control(dlg, "STATIC", k_hint, SS_LEFT, 7, 7, 356, 18, IDC_FT_HINT);
    d->list = me_ui_add_control(dlg, WC_LISTVIEWA, "",
                                LVS_REPORT | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER |
                                WS_BORDER | WS_TABSTOP,
                                7, 28, 356, 190, IDC_FT_LIST);
    SendMessageW(d->list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                 LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    RECT lr;
    GetClientRect(d->list, &lr);
    int lw = lr.right - GetSystemMetrics(SM_CXVSCROLL);
    static const wchar_t *const heads[3] = { L"Console", L"ROM types", L"Opens in now" };
    const int widths[3] = { lw * 33 / 100, lw * 35 / 100, lw - lw * 33 / 100 - lw * 35 / 100 };
    for (int c = 0; c < 3; c++) {
        LVCOLUMNW col = { .mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM,
                          .cx = widths[c], .pszText = (wchar_t *)heads[c], .iSubItem = c };
        SendMessageW(d->list, LVM_INSERTCOLUMNW, (WPARAM)c, (LPARAM)&col);
    }

    me_ui_add_control(dlg, "BUTTON", "Also take the types in &brackets",
                      BS_AUTOCHECKBOX | WS_TABSTOP, 7, 223, 356, 10, IDC_FT_SHARED);
    me_ui_add_control(dlg, "BUTTON", "Also for other &users of this PC (Windows asks for administrator permission)",
                      BS_AUTOCHECKBOX | WS_TABSTOP, 7, 236, 356, 10, IDC_FT_ALL_USERS);
    me_ui_add_control(dlg, "BUTTON", "&All",   BS_PUSHBUTTON | WS_TABSTOP | WS_GROUP, 7, 253, 45, 14, IDC_FT_ALL);
    me_ui_add_control(dlg, "BUTTON", "&None",  BS_PUSHBUTTON | WS_TABSTOP, 56, 253, 45, 14, IDC_FT_NONE);
    me_ui_add_control(dlg, "BUTTON", "Cancel", BS_PUSHBUTTON | WS_TABSTOP, 249, 253, 55, 14, IDCANCEL);
    me_ui_add_control(dlg, "BUTTON", "Save",   BS_DEFPUSHBUTTON | WS_TABSTOP, 308, 253, 55, 14, IDOK);

    /* Start from what Windows does now: the bracketed types ticked if any of
       them opens in us, all users if the machine has any of our ProgIDs. */
    int shared = 0, machine = 0;
    wchar_t ext[16], app[160], progid[48], path[64];
    for (int i = 0; i < d->n; i++) {
        const me_console *c = me_console_at(i);
        for (int k = 0; console_ext(c, k, ext, 16); k++)
            if (!shared && is_shared(ext) && ext_opener(ext, app, 160) > 0) shared = 1;
        progid_of(c, progid, 48);
        swprintf(path, 64, CLASSES L"%ls", progid);
        if (!machine && key_exists(HKEY_LOCAL_MACHINE, path)) machine = 1;
    }
    CheckDlgButton(dlg, IDC_FT_SHARED, shared ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_FT_ALL_USERS, machine ? BST_CHECKED : BST_UNCHECKED);

    d->filling = 1;
    for (int i = 0; i < d->n; i++) {
        wchar_t name[96];
        widen(me_console_at(i)->name, name, 96);
        LVITEMW it = { .mask = LVIF_TEXT, .iItem = i, .pszText = name };
        SendMessageW(d->list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        ListView_SetCheckState(d->list, i, ft_refresh_row(dlg, d, i));
    }
    d->filling = 0;
    ft_update_shield(dlg, d);
}

static void ft_set_busy(HWND dlg, ft_dlg *d, int busy) {
    static const int ids[] = { IDC_FT_LIST, IDC_FT_SHARED, IDC_FT_ALL_USERS, IDC_FT_ALL, IDC_FT_NONE,
                               IDCANCEL, IDOK };
    d->busy = busy;
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) EnableWindow(GetDlgItem(dlg, ids[i]), !busy);
    SetDlgItemTextA(dlg, IDC_FT_HINT, busy ? "Waiting for administrator permission..." : k_hint);
}

/* Runs the elevated copy. ShellExecuteEx blocks while Windows asks, so it
   isn't done on the UI thread: the game window stays responsive. */
static DWORD WINAPI elevate_thread(LPVOID arg) {
    ft_dlg *d = (ft_dlg *)arg;
    HRESULT co = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    SHELLEXECUTEINFOW sei = { .cbSize = sizeof(sei), .fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC,
                              .hwnd = d->dlg, .lpVerb = L"runas", .lpFile = exe_path(),
                              .lpParameters = d->params, .nShow = SW_HIDE };
    LONG r;
    if (!ShellExecuteExW(&sei)) {
        r = GetLastError() == ERROR_CANCELLED ? ELEV_DECLINED : ELEV_FAILED;
    } else if (!sei.hProcess) {
        r = ELEV_FAILED;
    } else {
        DWORD code = 0;
        WaitForSingleObject(sei.hProcess, INFINITE);
        r = GetExitCodeProcess(sei.hProcess, &code) ? (LONG)code : ELEV_FAILED;
        CloseHandle(sei.hProcess);
    }
    if (SUCCEEDED(co)) CoUninitialize();
    PostMessageW(d->dlg, WM_FT_ELEVATED, (WPARAM)r, 0);
    return 0;
}

/* The current user's part, after the machine's: their classes, their
   UserChoices; then report what Windows still opens elsewhere. */
static void ft_finish(HWND dlg, ft_dlg *d) {
    for (int i = 0; i < d->n; i++)
        sync_console(HKEY_CURRENT_USER, me_console_at(i), d->on[i], d->shared);
    wchar_t ext[16], progid[48], app[160];
    for (int i = 0; i < d->n; i++) {
        const me_console *c = me_console_at(i);
        progid_of(c, progid, 48);
        for (int k = 0; console_ext(c, k, ext, 16); k++)
            fix_user_choice(ext, progid, d->on[i] && ext_wanted(ext, d->shared));
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);

    char missed[512] = "";
    size_t len = 0;
    int n_missed = 0;
    for (int i = 0; i < d->n; i++) {
        if (!d->on[i]) continue;
        const me_console *c = me_console_at(i);
        for (int k = 0; console_ext(c, k, ext, 16); k++) {
            if (!ext_wanted(ext, d->shared) || ext_opener(ext, app, 160) > 0) continue;
            n_missed++;
            if (len + 40 < sizeof(missed)) {
                int w = snprintf(missed + len, sizeof(missed) - len, "%s%ls", len ? " " : "", ext);
                if (w > 0) len += (size_t)w;
            }
        }
    }
    if (n_missed) {
        char text[800];
        snprintf(text, sizeof(text),
                 "Windows still opens %d of the types with another program:\n%s\n\n"
                 "You can pick laggueless for them in Settings > Apps > Default apps > "
                 "Choose default apps by file type.\n\nOpen Default apps now?",
                 n_missed, missed);
        if (MessageBoxA(dlg, text, "File Associations", MB_YESNO | MB_ICONWARNING) == IDYES)
            ShellExecuteW(NULL, L"open", L"ms-settings:defaultapps", NULL, NULL, SW_SHOWNORMAL);
    }
    EndDialog(dlg, IDOK);
}

static void ft_save(HWND dlg, ft_dlg *d) {
    ft_read_controls(dlg, d);
    if (!ft_machine_needed(d)) { ft_finish(dlg, d); return; }

    if (d->elevated) {
        int fails = 0;
        for (int i = 0; i < d->n; i++)
            fails += sync_console(HKEY_LOCAL_MACHINE, me_console_at(i), d->all_users && d->on[i], d->shared);
        if (fails) {
            MessageBoxA(dlg, "Some file types could not be changed for all users.", "File Associations",
                        MB_OK | MB_ICONWARNING);
            return;
        }
        ft_finish(dlg, d);
        return;
    }

    /* The elevated copy gets the ticked consoles; it releases the rest. */
    size_t len = (size_t)swprintf(d->params, 1024, L"--file-types-all-users%ls",
                                  d->shared ? L" --shared" : L"");
    for (int i = 0; i < d->n && d->all_users; i++) {
        if (!d->on[i]) continue;
        wchar_t id[32];
        widen(me_console_at(i)->id, id, 32);
        int w = swprintf(d->params + len, 1024 - len, L" %ls", id);
        if (w > 0) len += (size_t)w;
    }
    d->dlg = dlg;
    ft_set_busy(dlg, d, 1);
    d->thread = CreateThread(NULL, 0, elevate_thread, d, 0, NULL);
    if (!d->thread) {
        ft_set_busy(dlg, d, 0);
        MessageBoxA(dlg, "Could not ask Windows for administrator permission.", "File Associations",
                    MB_OK | MB_ICONWARNING);
    }
}

static void ft_elevated_done(HWND dlg, ft_dlg *d, LONG r) {
    WaitForSingleObject(d->thread, INFINITE);   /* it has posted its last message */
    CloseHandle(d->thread);
    d->thread = NULL;
    ft_set_busy(dlg, d, 0);
    if (r == 0) { ft_finish(dlg, d); return; }
    const char *text =
        r == ELEV_DECLINED
            ? "Windows didn't get administrator permission, so nothing was changed.\n\n"
              "To change file associations just for you, untick \"Also for other users\" and save again."
            : r == ELEV_FAILED
                ? "Could not ask Windows for administrator permission, so nothing was changed."
                : "Some file types could not be changed for all users, so nothing was changed "
                  "for you.";
    MessageBoxA(dlg, text, "File Associations", MB_OK | MB_ICONWARNING);
}

static INT_PTR CALLBACK file_types_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    ft_dlg *d = (ft_dlg *)GetWindowLongPtrW(dlg, DWLP_USER);
    switch (msg) {
        case WM_INITDIALOG:
            d = (ft_dlg *)lp;
            SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)d);
            ft_create_controls(dlg, d);
            return TRUE;

        case WM_NOTIFY: {
            const NMLISTVIEW *nl = (const NMLISTVIEW *)lp;
            if (nl->hdr.idFrom == IDC_FT_LIST && nl->hdr.code == LVN_ITEMCHANGED &&
                (nl->uChanged & LVIF_STATE) && ((nl->uNewState ^ nl->uOldState) & LVIS_STATEIMAGEMASK))
                ft_update_shield(dlg, d);
            break;
        }

        case WM_FT_ELEVATED:
            ft_elevated_done(dlg, d, (LONG)wp);
            return TRUE;

        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_FT_SHARED:
                    d->filling = 1;
                    for (int i = 0; i < d->n; i++) ft_refresh_row(dlg, d, i);
                    d->filling = 0;
                    ft_update_shield(dlg, d);
                    return TRUE;
                case IDC_FT_ALL_USERS:
                    ft_update_shield(dlg, d);
                    return TRUE;
                case IDC_FT_ALL:
                case IDC_FT_NONE:
                    d->filling = 1;
                    for (int i = 0; i < d->n; i++) ListView_SetCheckState(d->list, i, LOWORD(wp) == IDC_FT_ALL);
                    d->filling = 0;
                    ft_update_shield(dlg, d);
                    return TRUE;
                case IDOK:
                    if (!d->busy) ft_save(dlg, d);
                    return TRUE;
                case IDCANCEL:
                    if (!d->busy) EndDialog(dlg, IDCANCEL);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

void me_ui_file_types_dialog(HWND owner) {
    me_ui_init_common_controls();
    static ft_dlg d;  /* one dialog at a time on the UI thread */
    if (d.busy) return;
    memset(&d, 0, sizeof(d));
    d.n = me_console_count();
    if (d.n > FT_MAX) d.n = FT_MAX;
    d.elevated = process_elevated();
    me_ui_dialog(owner, L"File Associations", 370, 274, file_types_proc, (LPARAM)&d);
}
