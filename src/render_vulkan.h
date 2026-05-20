#ifndef ME_RENDER_VULKAN_H
#define ME_RENDER_VULKAN_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "types.h"

/* Vulkan present path. Mirrors the shape of render_d3d11.h so main.c can
   swap between the two with minimal branching. All entry points are no-ops
   (and return failure where applicable) when the build was produced without
   ME_HAVE_VULKAN. */

int  me_vk_init(HWND hwnd, unsigned max_w, unsigned max_h);
void me_vk_shutdown(void);
int  me_vk_is_active(void);

/* Block until the previous frame's submission has completed on the GPU
   (analogous to the DXGI waitable swap chain's WaitForSingleObject when the
   max frame latency is pinned to 1). Returns 1 if the wait actually happened,
   0 otherwise. Should be called once at the top of the main loop before any
   new CPU-side frame work. */
int  me_vk_wait_for_present(unsigned timeout_ms);

/* Present one frame from CPU BGRX pixels. The source buffer is `max_w` pixels
   per row; the (frame_w, frame_h) sub-region in the top-left is copied. The
   destination is the integer-scaled rect (dx, dy, dw, dh) inside the client
   area (cw, ch). Returns 0 on success. Areas outside the dst rect are cleared
   to black. */
int  me_vk_present(const u32 *pixels, unsigned frame_w, unsigned frame_h, unsigned max_w,
                   int cw, int ch, int dx, int dy, int dw, int dh);

#endif
