/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * lsfg_bridge.cpp — Step B3: C++ implementation of the C bridge to the
 *                   lsfg-vk backend static library.
 *
 * This file is compiled with g++ and linked into the final laggueless.exe
 * alongside the C sources (gcc handles mixed-language link fine via g++ as
 * the final linker, or by passing -lstdc++ explicitly to gcc).
 */

#include "lsfg_bridge.h"

/* Pull in the backend public header. It uses HANDLE from windows.h. */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "lsfg-vk-backend/lsfgvk.hpp"

#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <vector>

/* -------------------------------------------------------------------------
 * Internal structs (hidden from C callers via opaque typedefs)
 * ------------------------------------------------------------------------- */

struct me_lsfg_instance {
    std::unique_ptr<lsfgvk::backend::Instance> inst;
};

struct me_lsfg_context {
    lsfgvk::backend::Context *ctx; /* owned by Instance */
};

/* -------------------------------------------------------------------------
 * me_lsfg_backend_create
 * ------------------------------------------------------------------------- */

extern "C"
me_lsfg_instance *me_lsfg_backend_create(
        const char *dll_path,
        VkInstance host_instance,
        VkPhysicalDevice host_phys_dev,
        VkDevice host_device,
        VkQueue /*host_queue*/,
        uint32_t queue_family_idx) {
    if (!dll_path) {
        fprintf(stderr, "[lsfg-bridge] dll_path is NULL\n");
        return nullptr;
    }
    if (!host_instance || !host_phys_dev || !host_device) {
        fprintf(stderr, "[lsfg-bridge] host VkInstance/VkPhysicalDevice/VkDevice required\n");
        return nullptr;
    }

    auto *handle = new (std::nothrow) me_lsfg_instance;
    if (!handle) return nullptr;

    try {
        handle->inst = std::make_unique<lsfgvk::backend::Instance>(
            host_instance, host_phys_dev, host_device, queue_family_idx,
            std::filesystem::path(dll_path),
            true /* allow FP16 if supported */
        );
        fprintf(stderr, "[lsfg-bridge] Instance created OK (shared host VkInstance/VkDevice)\n");
        return handle;
    } catch (const lsfgvk::backend::error& e) {
        fprintf(stderr, "[lsfg-bridge] backend::error creating Instance: %s\n", e.what());
    } catch (const std::exception& e) {
        fprintf(stderr, "[lsfg-bridge] std::exception creating Instance: %s\n", e.what());
    }
    delete handle;
    return nullptr;
}

/* -------------------------------------------------------------------------
 * me_lsfg_backend_open
 * ------------------------------------------------------------------------- */

extern "C"
me_lsfg_context *me_lsfg_backend_open(
    me_lsfg_instance *inst,
    HANDLE src0_handle,
    HANDLE src1_handle,
    const HANDLE *dest_handles, int dest_count,
    HANDLE sync_handle,
    unsigned width, unsigned height,
    int hdr, float flow_scale, int perf_mode)
{
    if (!inst || !inst->inst) return nullptr;
    if (dest_count <= 0 || !dest_handles) return nullptr;

    std::vector<HANDLE> dests(dest_handles, dest_handles + dest_count);

    try {
        lsfgvk::backend::Context& ctx_ref = inst->inst->openContext(
            { src0_handle, src1_handle },
            dests,
            sync_handle,
            width, height,
            hdr ? true : false,
            flow_scale,
            perf_mode ? true : false
        );
        auto *handle = new (std::nothrow) me_lsfg_context;
        if (!handle) {
            inst->inst->closeContext(ctx_ref);
            return nullptr;
        }
        handle->ctx = &ctx_ref;
        fprintf(stderr, "[lsfg-bridge] Context opened (%ux%u, x%d)\n",
                width, height, dest_count + 1);
        return handle;
    } catch (const lsfgvk::backend::error& e) {
        fprintf(stderr, "[lsfg-bridge] backend::error opening Context: %s\n", e.what());
    } catch (const std::exception& e) {
        fprintf(stderr, "[lsfg-bridge] std::exception opening Context: %s\n", e.what());
    }
    return nullptr;
}

/* -------------------------------------------------------------------------
 * me_lsfg_backend_schedule
 * ------------------------------------------------------------------------- */

extern "C"
int me_lsfg_backend_schedule(me_lsfg_instance *inst, me_lsfg_context *ctx) {
    if (!inst || !ctx || !ctx->ctx) return -1;
    try {
        inst->inst->scheduleFrames(*ctx->ctx);
        return 0;
    } catch (const lsfgvk::backend::error& e) {
        fprintf(stderr, "[lsfg-bridge] scheduleFrames error: %s\n", e.what());
    } catch (const std::exception& e) {
        fprintf(stderr, "[lsfg-bridge] scheduleFrames exception: %s\n", e.what());
    }
    return -1;
}

/* -------------------------------------------------------------------------
 * me_lsfg_backend_close
 * ------------------------------------------------------------------------- */

extern "C"
void me_lsfg_backend_close(me_lsfg_instance *inst, me_lsfg_context *ctx) {
    if (!inst || !ctx || !ctx->ctx) return;
    try {
        inst->inst->closeContext(*ctx->ctx);
    } catch (const std::exception& e) {
        fprintf(stderr, "[lsfg-bridge] closeContext exception: %s\n", e.what());
    }
    delete ctx;
}

/* -------------------------------------------------------------------------
 * me_lsfg_backend_destroy
 * ------------------------------------------------------------------------- */

extern "C"
void me_lsfg_backend_destroy(me_lsfg_instance *inst) {
    delete inst; /* ~me_lsfg_instance → ~unique_ptr<Instance> */
}
