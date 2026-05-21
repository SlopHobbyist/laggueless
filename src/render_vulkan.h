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

/* -------------------------------------------------------------------------
 * B3: LSFG frame-gen integration.
 * Call me_vk_lsfg_init() after me_vk_init() to open the backend context and
 * wire up shared images + timeline semaphore.
 *
 * dll_path   — absolute path to Lossless.dll (as located by lsfg_loader)
 * width/height — frame dimensions (should match the core's output; can be
 *                set to max_w/max_h as a first approximation)
 * multiplier  — 2, 3, or 4 (number of output frames per real frame)
 * flow_scale  — optical-flow resolution scale, 0.25..1.0 (1.0 = full res, best quality)
 * perf_mode   — 1 = reduced-quality / lower GPU cost
 *
 * Returns 0 on success. On failure LSFG is silently disabled.
 * me_vk_lsfg_shutdown() must be called before me_vk_shutdown().
 * ------------------------------------------------------------------------- */
int  me_vk_lsfg_init(const char *dll_path, unsigned width, unsigned height,
                     int multiplier, float flow_scale, int perf_mode);
void me_vk_lsfg_shutdown(void);

#endif

