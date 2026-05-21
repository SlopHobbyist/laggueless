/* SPDX-License-Identifier: GPL-3.0-or-later */
/* lsfg_loader.h — Step B1: Lossless.dll loader + PE shader extraction
 *
 * Locates Lossless.dll via:
 *   1. lsfg/Lossless.dll  (folder next to the exe)
 *   2. --lsfg-dll=<path>  (explicit override from CLI)
 *
 * After locating the DLL, we parse its PE resource section to extract
 * embedded SPIR-V shaders (RT_RCDATA resources). This is the same data
 * the reference lsfg-vk-backend reads via extractResourcesFromDLL(), but
 * implemented in pure C using ReadFile / Windows PE structures.
 *
 * We do NOT LoadLibrary the DLL (no code execution). We open it with
 * LOAD_LIBRARY_AS_DATAFILE so Windows maps it read-only. We then walk
 * the resource directory ourselves to count / enumerate the shaders.
 * The actual shader bytes are held in memory for later use in Step B2.
 */
#pragma once

#include <windows.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Opaque handle to the extracted shader table.
 * NULL means the loader hasn't run or failed.
 * -------------------------------------------------------------------------- */
typedef struct me_lsfg_shaders me_lsfg_shaders;

/* --------------------------------------------------------------------------
 * me_lsfg_load
 *
 * Locate and open Lossless.dll, parse its PE RT_RCDATA resource section,
 * and extract all shader blobs into an in-memory table.
 *
 * Search order:
 *   1. explicit_dll_path  (non-NULL → use exactly this path)
 *   2. <exe_dir>/lsfg/Lossless.dll
 *
 * On success returns a heap-allocated me_lsfg_shaders that must be freed
 * with me_lsfg_free().  On failure returns NULL and prints a diagnostic to
 * stderr explaining what went wrong and where the user should put the DLL.
 * -------------------------------------------------------------------------- */
me_lsfg_shaders *me_lsfg_load(const char *explicit_dll_path);

/* --------------------------------------------------------------------------
 * me_lsfg_shader_count
 * Returns the total number of RT_RCDATA resources extracted from the DLL.
 * This is a quick sanity-check: Lossless.dll embeds many dozens of shaders.
 * -------------------------------------------------------------------------- */
int me_lsfg_shader_count(const me_lsfg_shaders *shaders);

/* --------------------------------------------------------------------------
 * me_lsfg_shader_get
 *
 * Retrieve a shader blob by its resource ID.
 * Returns a pointer to the blob bytes and writes *size_out = its size.
 * Returns NULL if resource_id is not present.
 *
 * The pointer is valid as long as the me_lsfg_shaders handle is alive.
 * -------------------------------------------------------------------------- */
const uint8_t *me_lsfg_shader_get(const me_lsfg_shaders *shaders,
                                  uint32_t resource_id,
                                  size_t *size_out);

/* --------------------------------------------------------------------------
 * me_lsfg_dll_path
 * Returns the resolved absolute path to the Lossless.dll that was loaded.
 * -------------------------------------------------------------------------- */
const char *me_lsfg_dll_path(const me_lsfg_shaders *shaders);

/* --------------------------------------------------------------------------
 * me_lsfg_free
 * Release all memory associated with a me_lsfg_shaders handle.
 * Safe to call with NULL.
 * -------------------------------------------------------------------------- */
void me_lsfg_free(me_lsfg_shaders *shaders);

#ifdef __cplusplus
}
#endif
