#ifndef ME_PLATFORM_WIN32_H
#define ME_PLATFORM_WIN32_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

HWND me_platform_create_window(const char *title, int w, int h);
int  me_platform_pump(void); /* returns 0 when WM_QUIT received */

/* Toggle borderless fullscreen on the given window. */
void me_platform_toggle_fullscreen(HWND hwnd);

/* Set to 1 by wndproc on F11; main loop reads & clears. */
extern volatile int me_platform_f11_pressed;
/* Set to 1 by wndproc on F1; main loop reads & clears. */
extern volatile int me_platform_f1_pressed;

#endif
