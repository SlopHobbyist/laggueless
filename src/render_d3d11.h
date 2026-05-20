#ifndef ME_RENDER_D3D11_H
#define ME_RENDER_D3D11_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "types.h"

/* Returns 0 on success. On failure, the caller should fall back to GDI. */
int  me_d3d11_init(HWND hwnd, unsigned max_w, unsigned max_h);
void me_d3d11_shutdown(void);

/* Upload BGRX8 pixel data covering (0,0)-(frame_w,frame_h) from a buffer of
   stride `max_w` pixels (the GDI backbuffer layout from main.c). */
void me_d3d11_upload(const u32 *pixels, unsigned frame_w, unsigned frame_h, unsigned max_w);

/* Present the most recently uploaded frame, sized to dst rect in client coords.
   Surrounding area is cleared to black. Uses ALLOW_TEARING for low-latency. */
void me_d3d11_present(int client_w, int client_h, int dx, int dy, int dw, int dh,
                      unsigned frame_w, unsigned frame_h);

#endif
