/* Controls > Player 1 / Player 2 / Hotkeys dialogs.

   Each dialog lists every control with its keyboard and controller bindings.
   Clicking a binding captures the next key / button; right-clicking clears it.
   Capture polls GetAsyncKeyState and XInput on a timer instead of reading
   WM_KEYDOWN, because the dialog manager eats Tab/Enter/Esc/arrows.

   Runs on the UI thread; the game keeps running behind the dialog. Save
   copies the edited maps into the live settings under the settings lock and
   rewrites settings.yaml. */

#include "ui.h"
#include <commctrl.h>
#include <stdio.h>
#include <string.h>
#include "app.h"
#include "xinput_pad.h"

enum {
    IDC_SCOPE = 1000,
    IDC_SRC_BOTH,
    IDC_SRC_KEYBOARD,
    IDC_SRC_CONTROLLER,
    IDC_SLOT,
    IDC_LIST,
    IDC_HINT,
    IDC_DEFAULT,
    IDC_SAVE,
};

#define CAPTURE_TIMER     1
#define CAPTURE_TICK_MS   10
#define CAPTURE_TIMEOUT   (8000 / CAPTURE_TICK_MS)

enum { COL_NAME = 0, COL_KEYBOARD = 1, COL_CONTROLLER = 2 };

/* Display order for the player dialogs: d-pad, face buttons, system,
   shoulders/triggers, stick clicks, stick directions. */
static const me_input_id k_player_rows[ME_IN_COUNT] = {
    ME_IN_DPAD_UP, ME_IN_DPAD_DOWN, ME_IN_DPAD_LEFT, ME_IN_DPAD_RIGHT,
    ME_IN_A, ME_IN_B, ME_IN_X, ME_IN_Y,
    ME_IN_START, ME_IN_BACK,
    ME_IN_LB, ME_IN_RB, ME_IN_LT, ME_IN_RT,
    ME_IN_LSTICK, ME_IN_RSTICK,
    ME_IN_LSTICK_UP, ME_IN_LSTICK_DOWN, ME_IN_LSTICK_LEFT, ME_IN_LSTICK_RIGHT,
    ME_IN_RSTICK_UP, ME_IN_RSTICK_DOWN, ME_IN_RSTICK_LEFT, ME_IN_RSTICK_RIGHT,
};

typedef struct {
    int is_hotkeys;
    int player;              /* player dialogs: 0 or 1 */
    int core_index;          /* player dialogs: per-core map being edited, -1 = universal */
    char core_name[64];
    int rows;
    int ids[ME_IN_COUNT];    /* row → me_input_id / me_hotkey_id */

    /* Working copy, indexed by me_input_id / me_hotkey_id. */
    me_kb_bindings kb[ME_IN_COUNT];
    me_xi_bindings xi[ME_IN_COUNT];
    me_input_source source;
    int slot;

    /* Capture state. cap_col == 0 means not capturing. */
    int cap_row, cap_col, cap_ticks;
    unsigned char cap_held[256];  /* keys down when capture began (or since) */
    unsigned cap_mod_vk;          /* hotkeys: lone modifier pressed so far */
    unsigned cap_xi_held;         /* buttons down when capture began */
    unsigned cap_xi_chord;        /* hotkeys: buttons pressed so far */
    unsigned last_vk;             /* last key captured (to ignore its Esc) */

    HWND list, hint, slot_combo;
} bind_dlg;

/* ---- dialog template ------------------------------------------------------ */
/* An empty in-memory dialog (caption + font); controls are created in
   WM_INITDIALOG so no resource script is needed. */
static INT_PTR run_dialog(HWND owner, const wchar_t *title, short cx, short cy,
                          DLGPROC proc, LPARAM param) {
    DWORD buf[128];  /* DWORD-aligned, as DLGTEMPLATE requires */
    memset(buf, 0, sizeof(buf));
    DLGTEMPLATE *t = (DLGTEMPLATE *)buf;
    t->style = DS_MODALFRAME | DS_CENTER | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU;
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
    return DialogBoxIndirectParamW(GetModuleHandleW(NULL), t, owner, proc, param);
}

/* Create a child control positioned in dialog units. */
static HWND add_control(HWND dlg, const char *cls, const char *text, DWORD style,
                        int x, int y, int w, int h, int id) {
    RECT r = { x, y, x + w, y + h };
    MapDialogRect(dlg, &r);
    HWND c = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             r.left, r.top, r.right - r.left, r.bottom - r.top,
                             dlg, (HMENU)(INT_PTR)id, GetModuleHandleA(NULL), NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM)SendMessageA(dlg, WM_GETFONT, 0, 0), TRUE);
    return c;
}

/* ---- list contents -------------------------------------------------------- */
static void kb_cell_text(const me_kb_bindings *b, char *out, size_t sz) {
    out[0] = '\0';
    size_t len = 0;
    for (int i = 0; i < b->count; i++) {
        char one[64];
        me_kb_binding_str(&b->b[i], one, sizeof(one));
        int n = snprintf(out + len, sz - len, "%s%s", len ? ", " : "", one);
        if (n < 0 || (size_t)n >= sz - len) break;
        len += (size_t)n;
    }
}

static void xi_cell_text(const me_xi_bindings *b, char *out, size_t sz) {
    out[0] = '\0';
    size_t len = 0;
    for (int i = 0; i < b->count; i++) {
        char one[128];
        me_xi_chord_str(b->b[i].buttons, one, sizeof(one));
        int n = snprintf(out + len, sz - len, "%s%s", len ? ", " : "", one);
        if (n < 0 || (size_t)n >= sz - len) break;
        len += (size_t)n;
    }
}

static void set_cell(HWND list, int row, int col, const char *text) {
    LVITEMA it = { .iSubItem = col, .pszText = (char *)text };
    SendMessageA(list, LVM_SETITEMTEXTA, (WPARAM)row, (LPARAM)&it);
}

static void refresh_row(bind_dlg *d, int row) {
    char buf[256];
    int id = d->ids[row];
    kb_cell_text(&d->kb[id], buf, sizeof(buf));
    set_cell(d->list, row, COL_KEYBOARD, buf);
    xi_cell_text(&d->xi[id], buf, sizeof(buf));
    set_cell(d->list, row, COL_CONTROLLER, buf);
}

static void refresh_all(HWND dlg, bind_dlg *d) {
    for (int r = 0; r < d->rows; r++) refresh_row(d, r);
    CheckRadioButton(dlg, IDC_SRC_BOTH, IDC_SRC_CONTROLLER,
                     d->source == ME_SRC_KEYBOARD   ? IDC_SRC_KEYBOARD
                   : d->source == ME_SRC_CONTROLLER ? IDC_SRC_CONTROLLER : IDC_SRC_BOTH);
    SendMessageA(d->slot_combo, CB_SETCURSEL, (WPARAM)d->slot, 0);
}

static void set_hint(bind_dlg *d, const char *text) {
    SetWindowTextA(d->hint, text);
}

static void idle_hint(bind_dlg *d) {
    set_hint(d, d->is_hotkeys
        ? "Click a binding to change it; right-click to clear it. Chords like Ctrl+R or Back+Start work."
        : "Click a binding to change it; right-click to clear it.");
}

/* "Controller 2 (connected)" etc. Probing an empty slot is slow-ish, which
   is fine here on the UI thread. */
static void fill_slot_combo(bind_dlg *d) {
    SendMessageA(d->slot_combo, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < ME_XI_SLOTS; i++) {
        int connected = 0;
        me_xinput_read(i, &connected);
        char label[64];
        snprintf(label, sizeof(label), "Controller %d (%s)", i + 1,
                 connected ? "connected" : "not connected");
        SendMessageA(d->slot_combo, CB_ADDSTRING, 0, (LPARAM)label);
    }
    SendMessageA(d->slot_combo, CB_SETCURSEL, (WPARAM)d->slot, 0);
}

/* ---- capture -------------------------------------------------------------- */
static int is_modifier_vk(unsigned vk) {
    return vk >= VK_LSHIFT && vk <= VK_RMENU;
}

/* Keys capture never binds: mouse buttons, the generic modifier codes (the
   left/right-specific ones are used instead), and the Windows keys. */
static int capturable_vk(unsigned vk) {
    if (vk < 0x08) return 0;
    if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) return 0;
    if (vk == VK_LWIN || vk == VK_RWIN || vk == VK_APPS) return 0;
    return 1;
}

static void end_capture(HWND dlg, bind_dlg *d) {
    KillTimer(dlg, CAPTURE_TIMER);
    int row = d->cap_row;
    d->cap_col = 0;
    refresh_row(d, row);
    idle_hint(d);
}

static void begin_capture(HWND dlg, bind_dlg *d, int row, int col) {
    if (d->cap_col) end_capture(dlg, d);
    d->cap_row = row;
    d->cap_col = col;
    d->cap_ticks = 0;
    d->cap_mod_vk = 0;
    d->cap_xi_chord = 0;
    /* Anything already held (e.g. the mouse click, a modifier) must be
       released and pressed again to count. */
    for (unsigned vk = 0; vk < 256; vk++)
        d->cap_held[vk] = (GetAsyncKeyState((int)vk) & 0x8000) ? 1 : 0;
    d->cap_xi_held = me_xinput_read(d->slot, NULL);

    char label[160];
    const char *name = d->is_hotkeys ? me_hotkey_label((me_hotkey_id)d->ids[row])
                                     : me_input_label((me_input_id)d->ids[row]);
    if (col == COL_KEYBOARD) {
        set_cell(d->list, row, col, "Press a key...");
        snprintf(label, sizeof(label), "Press a key for %s. Click the cell again to cancel.", name);
    } else {
        int connected = 0;
        me_xinput_read(d->slot, &connected);
        set_cell(d->list, row, col, "Press a button...");
        snprintf(label, sizeof(label), "%s %s on Controller %d.%s Click the cell again to cancel.",
                 d->is_hotkeys ? "Press the button(s) for" : "Press a button for", name,
                 d->slot + 1, connected ? "" : " (not connected)");
    }
    set_hint(d, label);
    /* Keep Space/Enter away from the buttons while capturing. */
    SetFocus(d->list);
    SetTimer(dlg, CAPTURE_TIMER, CAPTURE_TICK_MS, NULL);
}

static void bind_key(HWND dlg, bind_dlg *d, unsigned vk, int ctrl, int alt, int shift) {
    me_kb_bindings *b = &d->kb[d->ids[d->cap_row]];
    memset(b, 0, sizeof(*b));
    b->b[0].vk    = vk;
    b->b[0].ctrl  = (unsigned char)ctrl;
    b->b[0].alt   = (unsigned char)alt;
    b->b[0].shift = (unsigned char)shift;
    b->count = 1;
    d->last_vk = vk;
    end_capture(dlg, d);
}

static void bind_buttons(HWND dlg, bind_dlg *d, unsigned buttons) {
    me_xi_bindings *b = &d->xi[d->ids[d->cap_row]];
    memset(b, 0, sizeof(*b));
    b->b[0].buttons = buttons;
    b->count = 1;
    end_capture(dlg, d);
}

static void capture_tick(HWND dlg, bind_dlg *d) {
    if (++d->cap_ticks > CAPTURE_TIMEOUT) { end_capture(dlg, d); return; }

    if (d->cap_col == COL_KEYBOARD) {
        int any_mod_down = 0;
        for (unsigned vk = 0; vk < 256; vk++) {
            if (!capturable_vk(vk)) continue;
            int down = (GetAsyncKeyState((int)vk) & 0x8000) ? 1 : 0;
            if (is_modifier_vk(vk) && down) any_mod_down = 1;
            if (!down) { d->cap_held[vk] = 0; continue; }
            if (d->cap_held[vk]) continue;
            d->cap_held[vk] = 1;
            if (d->is_hotkeys && is_modifier_vk(vk)) {
                /* Could be the start of a chord; decide once something else
                   is pressed or the modifier is let go. */
                if (!d->cap_mod_vk) d->cap_mod_vk = vk;
                continue;
            }
            if (d->is_hotkeys) {
                bind_key(dlg, d, vk,
                         (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0,
                         (GetAsyncKeyState(VK_MENU)    & 0x8000) != 0,
                         (GetAsyncKeyState(VK_SHIFT)   & 0x8000) != 0);
            } else {
                bind_key(dlg, d, vk, 0, 0, 0);
            }
            return;
        }
        /* Hotkeys: a modifier pressed and released on its own binds by itself. */
        if (d->cap_mod_vk && !any_mod_down) bind_key(dlg, d, d->cap_mod_vk, 0, 0, 0);
        return;
    }

    unsigned cur = me_xinput_read(d->slot, NULL);
    unsigned fresh = cur & ~d->cap_xi_held;
    d->cap_xi_held &= cur;  /* released buttons become eligible again */
    if (!d->is_hotkeys) {
        if (fresh) bind_buttons(dlg, d, fresh & (~fresh + 1u));  /* lowest new button */
        return;
    }
    /* Hotkeys: collect everything pressed, bind once all are released. */
    if (fresh) {
        d->cap_xi_chord |= fresh;
        d->cap_xi_held |= fresh;
        char text[128];
        me_xi_chord_str(d->cap_xi_chord, text, sizeof(text));
        set_cell(d->list, d->cap_row, COL_CONTROLLER, text);
    }
    if (d->cap_xi_chord && cur == 0) bind_buttons(dlg, d, d->cap_xi_chord);
}

/* ---- load / save ---------------------------------------------------------- */
static void load_from(bind_dlg *d, const me_settings *s, int core_index) {
    if (d->is_hotkeys) {
        memcpy(d->kb, s->hk,    sizeof(s->hk));
        memcpy(d->xi, s->hk_xi, sizeof(s->hk_xi));
        d->source = s->hk_source;
        d->slot   = s->hk_xi_index;
        return;
    }
    const me_control_map *m = core_index >= 0 ? &s->cores[core_index].controls[d->player]
                                              : &s->universal[d->player];
    memcpy(d->kb, m->keys, sizeof(m->keys));
    memcpy(d->xi, m->xi,   sizeof(m->xi));
    d->source = s->input_source[d->player];
    d->slot   = s->xi_index[d->player];
}

static void store_into(const bind_dlg *d, me_settings *s, int core_index) {
    if (d->is_hotkeys) {
        memcpy(s->hk,    d->kb, sizeof(s->hk));
        memcpy(s->hk_xi, d->xi, sizeof(s->hk_xi));
        s->hk_source   = d->source;
        s->hk_xi_index = d->slot;
        return;
    }
    me_control_map m;
    memcpy(m.keys, d->kb, sizeof(m.keys));
    memcpy(m.xi,   d->xi, sizeof(m.xi));
    if (core_index >= 0) me_settings_set_core_map(s, core_index, d->player, &m);
    else                 me_settings_set_universal(s, d->player, &m);
    s->input_source[d->player] = d->source;
    s->xi_index[d->player]     = d->slot;
}

/* The per-core entry in `s` matching the one being edited (by name), or -1. */
static int core_in(const bind_dlg *d, const me_settings *s) {
    if (d->core_index < 0) return -1;
    for (size_t i = 0; i < s->cores_n; i++) {
        if (_stricmp(s->cores[i].name, d->core_name) == 0) return (int)i;
    }
    return -1;
}

static void patch_bindings(me_settings *s, const void *ctx) {
    const bind_dlg *d = (const bind_dlg *)ctx;
    store_into(d, s, core_in(d, s));
}

static void apply_defaults(HWND dlg, bind_dlg *d) {
    me_settings t;
    me_settings_load_template(&t);
    int ci = core_in(d, &t);
    if (ci >= 0 && t.cores[ci].use_universal) ci = -1;
    load_from(d, &t, ci);
    me_settings_free(&t);
    refresh_all(dlg, d);
}

static void save(bind_dlg *d) {
    me_settings *live = me_app_settings();
    me_settings_lock();
    store_into(d, live, d->core_index);
    me_settings_unlock();
    me_ui_persist(patch_bindings, d);
}

/* ---- dialog procedure ----------------------------------------------------- */
static void create_controls(HWND dlg, bind_dlg *d) {
    char scope[200];
    if (d->is_hotkeys)
        snprintf(scope, sizeof(scope), "Hotkeys work in every core.");
    else if (d->core_index >= 0)
        snprintf(scope, sizeof(scope), "Editing Player %d controls for %s only (this core has its own map).",
                 d->player + 1, d->core_name);
    else
        snprintf(scope, sizeof(scope), "Editing Player %d controls for all cores.", d->player + 1);
    add_control(dlg, "STATIC", scope, SS_LEFT, 7, 7, 306, 10, IDC_SCOPE);

    add_control(dlg, "STATIC", "Input:", SS_LEFT, 7, 23, 40, 10, 0);
    add_control(dlg, "BUTTON", "Keyboard and controller",
                BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 50, 22, 105, 11, IDC_SRC_BOTH);
    add_control(dlg, "BUTTON", "Keyboard only",   BS_AUTORADIOBUTTON, 160, 22, 70, 11, IDC_SRC_KEYBOARD);
    add_control(dlg, "BUTTON", "Controller only", BS_AUTORADIOBUTTON, 233, 22, 80, 11, IDC_SRC_CONTROLLER);

    add_control(dlg, "STATIC", "Controller:", SS_LEFT, 7, 40, 40, 10, 0);
    d->slot_combo = add_control(dlg, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP | WS_GROUP,
                                50, 38, 130, 80, IDC_SLOT);
    fill_slot_combo(d);

    d->list = add_control(dlg, WC_LISTVIEWA, "",
                          LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER |
                          WS_BORDER | WS_TABSTOP,
                          7, 56, 306, 160, IDC_LIST);
    SendMessageA(d->list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                 LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    RECT lr;
    GetClientRect(d->list, &lr);
    int lw = lr.right - GetSystemMetrics(SM_CXVSCROLL);
    static const char *const heads[3] = { "Control", "Keyboard", "Controller" };
    const int widths[3] = { lw * 34 / 100, lw * 33 / 100, lw - lw * 34 / 100 - lw * 33 / 100 };
    for (int c = 0; c < 3; c++) {
        LVCOLUMNA col = { .mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM,
                          .cx = widths[c], .pszText = (char *)heads[c], .iSubItem = c };
        SendMessageA(d->list, LVM_INSERTCOLUMNA, (WPARAM)c, (LPARAM)&col);
    }
    for (int r = 0; r < d->rows; r++) {
        const char *name = d->is_hotkeys ? me_hotkey_label((me_hotkey_id)d->ids[r])
                                         : me_input_label((me_input_id)d->ids[r]);
        LVITEMA it = { .mask = LVIF_TEXT, .iItem = r, .pszText = (char *)name };
        SendMessageA(d->list, LVM_INSERTITEMA, 0, (LPARAM)&it);
    }

    d->hint = add_control(dlg, "STATIC", "", SS_LEFT, 7, 220, 306, 18, IDC_HINT);
    idle_hint(d);

    add_control(dlg, "BUTTON", "Default", BS_PUSHBUTTON | WS_TABSTOP | WS_GROUP, 7, 243, 55, 14, IDC_DEFAULT);
    add_control(dlg, "BUTTON", "Cancel",  BS_PUSHBUTTON | WS_TABSTOP, 199, 243, 55, 14, IDCANCEL);
    add_control(dlg, "BUTTON", "Save",    BS_PUSHBUTTON | WS_TABSTOP, 258, 243, 55, 14, IDC_SAVE);
    refresh_all(dlg, d);
}

/* Which list cell a click landed on; returns 0 for none. */
static int hit_cell(bind_dlg *d, POINT pt, int *row) {
    LVHITTESTINFO hit = { .pt = pt };
    if (SendMessageA(d->list, LVM_SUBITEMHITTEST, 0, (LPARAM)&hit) < 0 || hit.iItem < 0) return 0;
    *row = hit.iItem;
    return hit.iSubItem;
}

static INT_PTR CALLBACK bind_dlg_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    bind_dlg *d = (bind_dlg *)GetWindowLongPtrW(dlg, DWLP_USER);
    switch (msg) {
        case WM_INITDIALOG:
            d = (bind_dlg *)lp;
            SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)d);
            create_controls(dlg, d);
            return TRUE;

        case WM_TIMER:
            if (wp == CAPTURE_TIMER && d->cap_col) capture_tick(dlg, d);
            return TRUE;

        case WM_NOTIFY: {
            NMHDR *nh = (NMHDR *)lp;
            if (nh->idFrom != IDC_LIST) break;
            if (nh->code == NM_CLICK || nh->code == NM_RCLICK) {
                NMITEMACTIVATE *ia = (NMITEMACTIVATE *)lp;
                int row = -1;
                int col = hit_cell(d, ia->ptAction, &row);
                if (col != COL_KEYBOARD && col != COL_CONTROLLER) return TRUE;
                int id = d->ids[row];
                if (nh->code == NM_RCLICK) {
                    if (d->cap_col) end_capture(dlg, d);
                    if (col == COL_KEYBOARD) memset(&d->kb[id], 0, sizeof(d->kb[id]));
                    else                     memset(&d->xi[id], 0, sizeof(d->xi[id]));
                    refresh_row(d, row);
                } else if (d->cap_col == col && d->cap_row == row) {
                    end_capture(dlg, d);   /* clicking the same cell cancels */
                } else {
                    begin_capture(dlg, d, row, col);
                }
                return TRUE;
            }
            break;
        }

        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_SRC_BOTH:       d->source = ME_SRC_BOTH;       return TRUE;
                case IDC_SRC_KEYBOARD:   d->source = ME_SRC_KEYBOARD;   return TRUE;
                case IDC_SRC_CONTROLLER: d->source = ME_SRC_CONTROLLER; return TRUE;
                case IDC_SLOT:
                    if (HIWORD(wp) == CBN_DROPDOWN) fill_slot_combo(d);
                    else if (HIWORD(wp) == CBN_SELCHANGE) {
                        LRESULT sel = SendMessageA(d->slot_combo, CB_GETCURSEL, 0, 0);
                        if (sel >= 0 && sel < ME_XI_SLOTS) d->slot = (int)sel;
                    }
                    return TRUE;
                case IDC_DEFAULT:
                    if (d->cap_col) end_capture(dlg, d);
                    apply_defaults(dlg, d);
                    return TRUE;
                case IDC_SAVE:
                    if (d->cap_col) end_capture(dlg, d);
                    save(d);
                    EndDialog(dlg, IDOK);
                    return TRUE;
                case IDOK:
                    /* Enter: no default button, so a captured Enter never saves. */
                    return TRUE;
                case IDCANCEL:
                    /* Esc may be the key being bound (or just bound and still
                       held); only a deliberate Esc / Cancel / close cancels. */
                    if (d->cap_col) return TRUE;
                    if (d->last_vk == VK_ESCAPE && (GetAsyncKeyState(VK_ESCAPE) & 0x8000)) return TRUE;
                    EndDialog(dlg, IDCANCEL);
                    return TRUE;
            }
            break;

        case WM_DESTROY:
            KillTimer(dlg, CAPTURE_TIMER);
            break;
    }
    return FALSE;
}

static void init_common_controls(void) {
    static int done = 0;
    if (done) return;
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    done = 1;
}

void me_ui_player_dialog(HWND owner, int player) {
    init_common_controls();
    static bind_dlg d;  /* large; one dialog at a time on the UI thread */
    memset(&d, 0, sizeof(d));
    d.player = player;
    d.rows = ME_IN_COUNT;
    for (int r = 0; r < ME_IN_COUNT; r++) d.ids[r] = (int)k_player_rows[r];

    /* Edit the running core's own map if it has one, else the universal map. */
    me_settings *live = me_app_settings();
    me_app_status st;
    me_status_get(&st);
    d.core_index = -1;
    if (st.core_path[0]) {
        int ci = me_settings_find_core_index(live, st.core_path);
        if (ci >= 0 && !live->cores[ci].use_universal) {
            d.core_index = ci;
            snprintf(d.core_name, sizeof(d.core_name), "%s", live->cores[ci].name);
        }
    }
    me_settings_lock();
    load_from(&d, live, d.core_index);
    me_settings_unlock();

    run_dialog(owner, player == 0 ? L"Player 1 Controls" : L"Player 2 Controls",
               320, 264, bind_dlg_proc, (LPARAM)&d);
}

void me_ui_hotkeys_dialog(HWND owner) {
    init_common_controls();
    static bind_dlg d;
    memset(&d, 0, sizeof(d));
    d.is_hotkeys = 1;
    d.core_index = -1;
    d.rows = ME_HK_COUNT;
    for (int r = 0; r < ME_HK_COUNT; r++) d.ids[r] = r;

    me_settings_lock();
    load_from(&d, me_app_settings(), -1);
    me_settings_unlock();

    run_dialog(owner, L"Hotkeys", 320, 264, bind_dlg_proc, (LPARAM)&d);
}
