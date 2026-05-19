#ifndef ME_PLATFORM_WIN32_H
#define ME_PLATFORM_WIN32_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

HWND me_platform_create_window(const char *title, int w, int h);
int  me_platform_pump(void); /* returns 0 when WM_QUIT received */

#endif
