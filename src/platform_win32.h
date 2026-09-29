#ifndef ME_PLATFORM_WIN32_H
#define ME_PLATFORM_WIN32_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* The game window lives on its own UI thread (see app.h for why). These
   functions are called from the emulation thread unless noted. */

/* Start the UI thread, which creates the window (with the menu bar) and
   pumps its messages. Returns once the window exists. `w`/`h` is the client
   size. Call once. */
HWND me_platform_create_window(const char *title, int w, int h);

/* Close the window and wait for the UI thread to finish. Call at exit. */
void me_platform_destroy_window(void);

HWND me_platform_hwnd(void);

/* Pump the calling (emulation) thread's own messages — e.g. the hidden GL
   context window. Returns 0 once quit has been requested. */
int  me_platform_pump(void);

/* Toggle borderless fullscreen / exit fullscreen. Safe from any thread: the
   work is posted to the UI thread. The menu bar is hidden while fullscreen. */
void me_platform_toggle_fullscreen(HWND hwnd);
void me_platform_exit_fullscreen(HWND hwnd);
int  me_platform_is_fullscreen(void);

/* Edge-triggered key-press detection. Returns 1 the first time the key is
   queried after a WM_KEYDOWN; subsequent calls return 0 until the key is
   released and pressed again. Modifier flags (ctrl/alt/shift) must all match
   the current modifier state — use this to handle chords like Ctrl+R. */
int  me_platform_key_pressed(unsigned vk, unsigned ctrl, unsigned alt, unsigned shift);

/* Mouse buttons (bits: 1 left, 2 right, 4 middle) held over the client
   area, plus any pressed since the last call even if already released.
   Clicks on the menu bar or its menus, and the click that activates the
   window, don't count. */
unsigned me_platform_mouse_buttons(void);

/* Cursor position in client coordinates. Returns 0 if unknown. */
int  me_platform_cursor_pos(int *x, int *y);

/* Request graceful shutdown. Safe from any thread. */
void me_platform_request_quit(void);

/* Idle = no game loaded. While idle the window paints itself black on
   WM_PAINT; while a game runs the presenter owns every pixel. Entering idle
   invalidates the window so the last game frame is cleared. */
void me_platform_set_idle(HWND hwnd, int idle);

#endif
