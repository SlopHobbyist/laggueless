/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * lsfg_bridge.h — Step B3: C-callable wrapper for the C++ lsfg-vk backend.
 *
 * The backend is a C++20 library (liblsfg_backend.a). Since render_vulkan.c
 * is C11, we bridge them here. All Vulkan handles are passed as void* or
 * uint64_t to avoid pulling vulkan.h into plain C headers.
 *
 * Lifecycle:
 *   1. me_lsfg_backend_create()   — creates Instance (loads shaders, init Vulkan)
 *   2. me_lsfg_backend_open()     — opens Context (imports shared images/sema)
 *   3. me_lsfg_backend_schedule() — called each real frame to generate frames
 *   4. me_lsfg_backend_close()    — closes Context
 *   5. me_lsfg_backend_destroy()  — destroys Instance
 */
#pragma once

#include <windows.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque backend instance. */
typedef struct me_lsfg_instance me_lsfg_instance;
/* Opaque backend context (per-resolution session). */
typedef struct me_lsfg_context me_lsfg_context;

/* -------------------------------------------------------------------------
 * me_lsfg_backend_create
 *
 * Creates an lsfgvk Instance. This initializes the backend's own VkDevice,
 * loads and compiles shaders from Lossless.dll, and returns an opaque handle.
 *
 * dll_path  — absolute path to Lossless.dll
 * Returns NULL on failure (error printed to stderr).
 * ------------------------------------------------------------------------- */
me_lsfg_instance *me_lsfg_backend_create(
    const char *dll_path,
    VkInstance host_instance,
    VkPhysicalDevice host_phys_dev,
    VkDevice host_device,
    VkQueue host_queue,            /* currently unused, reserved for future use */
    uint32_t queue_family_idx);

/* -------------------------------------------------------------------------
 * me_lsfg_backend_open
 *
 * Opens a Context for the given resolution. Call after creating shared images
 * and exporting their Win32 HANDLEs.
 *
 * inst         — instance from me_lsfg_backend_create
 * src0_handle  — HANDLE for source image slot 0 (current frame memory)
 * src1_handle  — HANDLE for source image slot 1 (previous frame memory)
 * dest_handles — array of N HANDLE for destination (generated) frame memories
 * dest_count   — number of destination images (LSFG multiplier - 1, min 1)
 * sync_handle  — HANDLE for the exported timeline semaphore
 * width        — frame width in pixels
 * height       — frame height in pixels
 * hdr          — 0 = RGBA8, 1 = RGBA16F
 * flow_scale   — optical flow scale (1.0 = full res, typical 0.5)
 * perf_mode    — 1 = reduced quality for lower GPU cost
 *
 * Returns NULL on failure.
 * ------------------------------------------------------------------------- */
/* DEPRECATED: uses Win32 HANDLE export/import path.  Kept for reference;
 * the shared-device path (me_lsfg_backend_open_shared) is preferred when
 * the host and backend share a VkDevice (which avoids dedicated-allocate
 * mismatches across instances). */
me_lsfg_context *me_lsfg_backend_open(
    me_lsfg_instance *inst,
    HANDLE src0_handle,
    HANDLE src1_handle,
    const HANDLE *dest_handles, int dest_count,
    HANDLE sync_handle,
    unsigned width, unsigned height,
    int hdr, float flow_scale, int perf_mode);

/* -------------------------------------------------------------------------
 * me_lsfg_backend_open_shared
 *
 * Shared-VkDevice variant: pass pre-existing host-owned VkImages / VkDeviceMemory
 * / VkSemaphore directly. No HANDLE export/import. Avoids cross-instance
 * dedicated-allocate validation errors.
 *
 * src_images[2]   — source VkImage handles (curr, prev)
 * src_mems[2]     — VkDeviceMemory bound to each source image
 * dest_images[]   — N dest VkImage handles
 * dest_mems[]     — VkDeviceMemory bound to each dest image
 * sync_semaphore  — host-owned timeline VkSemaphore
 * Other args same as me_lsfg_backend_open.
 * ------------------------------------------------------------------------- */
me_lsfg_context *me_lsfg_backend_open_shared(
    me_lsfg_instance *inst,
    VkImage src_image0, VkImage src_image1,
    VkDeviceMemory src_mem0, VkDeviceMemory src_mem1,
    const VkImage *dest_images,
    const VkDeviceMemory *dest_mems,
    int dest_count,
    VkSemaphore sync_semaphore,
    unsigned width, unsigned height,
    int hdr, float flow_scale, int perf_mode);

/* -------------------------------------------------------------------------
 * me_lsfg_backend_schedule
 *
 * Schedules the next batch of generated frames on the backend GPU.
 * Call once per real frame, after uploading pixels into the current source
 * image and advancing the source slot.
 *
 * Returns 0 on success, -1 on error.
 * ------------------------------------------------------------------------- */
int me_lsfg_backend_schedule(me_lsfg_instance *inst, me_lsfg_context *ctx);

/* -------------------------------------------------------------------------
 * me_lsfg_backend_close
 *
 * Closes a context and waits for all in-flight work to complete.
 * The shared image and semaphore HANDLEs must remain valid until this returns.
 * ------------------------------------------------------------------------- */
void me_lsfg_backend_close(me_lsfg_instance *inst, me_lsfg_context *ctx);

/* -------------------------------------------------------------------------
 * me_lsfg_backend_destroy
 *
 * Destroys the instance. All contexts must be closed first.
 * ------------------------------------------------------------------------- */
void me_lsfg_backend_destroy(me_lsfg_instance *inst);

#ifdef __cplusplus
}
#endif
