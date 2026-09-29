#ifndef ME_PLATFORM_WIN32_H
#define ME_PLATFORM_WIN32_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

HWND me_platform_create_window(const char *title, int w, int h);
int  me_platform_pump(void); /* returns 0 when WM_QUIT received */

/* Toggle borderless fullscreen on the given window. */
void me_platform_toggle_fullscreen(HWND hwnd);

/* Exit fullscreen unconditionally (no-op if already windowed). */
void me_platform_exit_fullscreen(HWND hwnd);

/* Edge-triggered key-press detection. Returns 1 the first time the key is
   queried after a WM_KEYDOWN; subsequent calls return 0 until the key is
   released and pressed again. Modifier flags (ctrl/alt/shift) must all match
   the current modifier state — use this to handle chords like Ctrl+R. */
int  me_platform_key_pressed(unsigned vk, unsigned ctrl, unsigned alt, unsigned shift);

/* Request graceful shutdown (posts WM_QUIT). */
void me_platform_request_quit(void);

/* Files dropped onto the window (drag-and-drop from Explorer). If one or more
   files were dropped since the last call, copies the first one's path into
   `out` and returns 1; otherwise returns 0. Only the most recent drop is kept. */
int  me_platform_take_dropped_file(char *out, size_t out_sz);

/* Idle = no game loaded. While idle the window paints itself black on
   WM_PAINT; while a game runs the presenter owns every pixel. Entering idle
   invalidates the window so the last game frame is cleared. */
void me_platform_set_idle(HWND hwnd, int idle);

#endif
