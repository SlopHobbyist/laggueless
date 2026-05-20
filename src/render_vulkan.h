#ifndef ME_RENDER_VULKAN_H
#define ME_RENDER_VULKAN_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "types.h"

/* Vulkan present path. Mirrors the shape of render_d3d11.h so main.c can
   swap between the two with minimal branching. All entry points are no-ops
   (and return failure where applicable) when the build was produced without
   ME_HAVE_VULKAN. */

/* Returns 0 on success, non-zero on failure. On failure, callers should fall
   back to GDI or D3D11. max_w/max_h reserve sizing for the eventual upload
   image; A2 does not allocate it yet. */
int  me_vk_init(HWND hwnd, unsigned max_w, unsigned max_h);

/* Tear down all Vulkan resources. Safe to call even if init failed. */
void me_vk_shutdown(void);

/* True after a successful me_vk_init. */
int  me_vk_is_active(void);

#endif
