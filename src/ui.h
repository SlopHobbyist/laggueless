#ifndef ME_UI_H
#define ME_UI_H

/* Menu bar and dialogs. Everything here runs on the UI thread unless marked
   "any thread". See app.h for the threading model. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "settings.h"

HMENU me_ui_create_menu(void);

/* Called first by the window procedure. Sets *handled when it consumed the
   message (menu commands, menu refresh, notifications below). */
LRESULT me_ui_handle(HWND h, UINT msg, WPARAM wp, LPARAM lp, int *handled);

/* Emulation thread → UI. Any thread; never blocks. */
void me_ui_notify_loaded(const char *rom_path);   /* add to Open Recent */
void me_ui_notify_frame_gen(int on);              /* result of ME_CMD_FRAME_GEN */
void me_ui_notify_error(const char *fmt, ...);    /* message box */

/* Re-read settings.yaml, apply `patch`, and write it back. Re-reading (rather
   than saving the live struct) keeps command-line overrides out of the file.
   Shows a message box and returns -1 if the file can't be parsed or written. */
typedef void (*me_settings_patch)(me_settings *s, const void *ctx);
int  me_ui_persist(me_settings_patch patch, const void *ctx);

/* Dialogs are empty in-memory templates (caption + font); controls are
   created in WM_INITDIALOG so no resource script is needed. `cx`/`cy` are
   dialog units. me_ui_dialog is modal to the UI thread only;
   me_ui_dialog_modeless returns the window (hidden until shown). */
INT_PTR me_ui_dialog(HWND owner, const wchar_t *title, short cx, short cy,
                     DLGPROC proc, LPARAM param);
HWND me_ui_dialog_modeless(HWND owner, const wchar_t *title, short cx, short cy,
                           DLGPROC proc, LPARAM param);
/* Child control positioned in dialog units, in the dialog's font. */
HWND me_ui_add_control(HWND dlg, const char *cls, const char *text, DWORD style,
                       int x, int y, int w, int h, int id);
void me_ui_init_common_controls(void);

/* Binding dialogs (ui_bindings.c). Modal to the UI thread only. */
void me_ui_player_dialog(HWND owner, int player);
void me_ui_hotkeys_dialog(HWND owner);

/* Cores menu (ui_cores.c). Download runs on its own thread, with a progress
   window on the UI thread; Set Cores is modal to the UI thread only. */
void me_ui_download_cores(HWND owner);
void me_ui_set_cores_dialog(HWND owner);

/* Cores > File Associations (ui_assoc.c): which consoles' ROMs Windows opens
   in laggueless. Modal to the UI thread only. Changing them for all users
   runs this exe elevated with ME_FILE_TYPES_ARG first; main() hands that
   straight to me_file_types_all_users_main (the arguments after it), whose
   return value is the exit code: 0, or how many changes failed. */
#define ME_FILE_TYPES_ARG "--file-types-all-users"
void me_ui_file_types_dialog(HWND owner);
int  me_file_types_all_users_main(int argc, char **argv);

#endif
