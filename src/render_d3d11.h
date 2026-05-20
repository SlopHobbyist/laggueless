#ifndef ME_RENDER_D3D11_H
#define ME_RENDER_D3D11_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "types.h"

/* Returns 0 on success. On failure, the caller should fall back to GDI. */
int  me_d3d11_init(HWND hwnd, unsigned max_w, unsigned max_h);
void me_d3d11_shutdown(void);

/* Accessors for the WGL_NV_DX_interop2 path. Returned as void* so callers
   don't need d3d11.h; the interop layer just passes them back through to
   WGL entry points. NULL if not initialised. */
void *me_d3d11_get_device(void);
void *me_d3d11_get_texture(void);

/* Create a second internal texture flagged D3D11_RESOURCE_MISC_SHARED so
   WGL_NV_DX_interop2 can register it. After this returns non-NULL, the
   interop layer registers the returned texture with GL; me_d3d11_present
   samples from it instead of the upload texture while me_d3d11_use_shared(1)
   is in effect. */
void *me_d3d11_create_shared_texture(unsigned w, unsigned h);
void  me_d3d11_use_shared(int yes);

/* Upload BGRX8 pixel data covering (0,0)-(frame_w,frame_h) from a buffer of
   stride `max_w` pixels (the GDI backbuffer layout from main.c). */
void me_d3d11_upload(const u32 *pixels, unsigned frame_w, unsigned frame_h, unsigned max_w);

/* Present the most recently uploaded frame, sized to dst rect in client coords.
   Surrounding area is cleared to black. Uses ALLOW_TEARING for low-latency. */
void me_d3d11_present(int client_w, int client_h, int dx, int dy, int dw, int dh,
                      unsigned frame_w, unsigned frame_h);

/* Block until the swap chain is ready to accept the next Present, with the
   maximum frame latency clamped to 1. Returns 1 if the wait actually
   happened (DXGI 1.3+ waitable swap chain available), 0 otherwise so the
   caller can fall back to its own pacing. `timeout_ms` is forwarded to
   WaitForSingleObjectEx; use INFINITE if you have no upper bound. */
int  me_d3d11_wait_for_present(unsigned timeout_ms);

#endif
