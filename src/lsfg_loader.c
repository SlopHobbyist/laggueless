/* SPDX-License-Identifier: GPL-3.0-or-later */
/* lsfg_loader.c — Step B1: Lossless.dll PE resource extraction (pure C / Win32)
 *
 * We parse the PE resource section of Lossless.dll ourselves so we can:
 *   - avoid LoadLibrary (no code execution risk, no DLL-init side-effects)
 *   - stay in pure C without linking anything new
 *   - match the reference extractResourcesFromDLL() behaviour exactly
 *
 * Reference: reference/lsfg-vk/lsfg-vk-backend/src/extraction/dll_reader.cpp
 * That code uses std::ifstream + std::span; we replicate it with ReadFile
 * and pointer arithmetic. The PE structures are identical.
 */

#include "lsfg_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---- PE structures (matches dll_reader.cpp) -------------------------------- */
#pragma pack(push, 1)

typedef struct {
    uint16_t magic;           /* 0x5A4D */
    uint16_t pad[29];
    int32_t  pe_offset;       /* file offset to PE header */
} DOSHeader;

typedef struct {
    uint32_t signature;       /* "PE\0\0" = 0x00004550 */
    uint16_t machine;
    uint16_t sect_count;
    uint16_t pad2[6];
    uint16_t opt_hdr_size;
    uint16_t characteristics;
} PEHeader;

typedef struct {
    uint16_t magic;           /* 0x20B for PE32+ */
    uint16_t linker_ver;
    uint32_t code_size;
    uint32_t init_data_size;
    uint32_t uninit_data_size;
    uint32_t entry_point;
    uint32_t code_base;
    /* --- PE32+ only (no image_base split) --- */
    uint64_t image_base;
    uint32_t section_align;
    uint32_t file_align;
    uint16_t os_major, os_minor;
    uint16_t image_major, image_minor;
    uint16_t subsys_major, subsys_minor;
    uint32_t win32_ver;
    uint32_t image_size;
    uint32_t headers_size;
    uint32_t checksum;
    uint16_t subsystem;
    uint16_t dll_characteristics;
    uint64_t stack_reserve, stack_commit;
    uint64_t heap_reserve,  heap_commit;
    uint32_t loader_flags;
    uint32_t num_rva_sizes;
    /* Data directories start here (each is 8 bytes: RVA, Size) */
    /* [0]=export, [1]=import, [2]=resource, ... */
    struct { uint32_t rva; uint32_t size; } data_dirs[16];
} PEOptHeader;

typedef struct {
    uint8_t  name[8];
    uint32_t vsize;
    uint32_t vaddress;
    uint32_t fsize;
    uint32_t foffset;
    uint32_t reloc_ptr;
    uint32_t linenum_ptr;
    uint16_t reloc_cnt;
    uint16_t linenum_cnt;
    uint32_t characteristics;
} SectionHeader;

typedef struct {
    uint32_t characteristics;
    uint32_t time_date_stamp;
    uint16_t major_version;
    uint16_t minor_version;
    uint16_t name_count;
    uint16_t id_count;
    /* followed by (name_count + id_count) ResourceDirectoryEntry */
} ResourceDirectory;

typedef struct {
    uint32_t id;     /* name/id; high bit = named */
    uint32_t offset; /* high bit = points to subdirectory */
} ResourceDirectoryEntry;

typedef struct {
    uint32_t offset_to_data; /* RVA in file */
    uint32_t size;
    uint32_t code_page;
    uint32_t reserved;
} ResourceDataEntry;

#pragma pack(pop)

/* ---- shader table entry ---------------------------------------------------- */
typedef struct {
    uint32_t id;
    size_t   size;
    uint8_t *data;
} ShaderEntry;

/* ---- public opaque handle -------------------------------------------------- */
struct me_lsfg_shaders {
    char          dll_path[MAX_PATH];
    ShaderEntry  *entries;
    int           count;
    int           capacity;
};

/* ---- helper: read whole file into heap ------------------------------------ */
static uint8_t *read_file_all(const char *path, size_t *out_size) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;

    LARGE_INTEGER sz = {0};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 64*1024*1024) {
        CloseHandle(h);
        return NULL;
    }
    size_t file_size = (size_t)sz.QuadPart;
    uint8_t *buf = (uint8_t *)malloc(file_size);
    if (!buf) { CloseHandle(h); return NULL; }

    DWORD read = 0;
    if (!ReadFile(h, buf, (DWORD)file_size, &read, NULL) || read != (DWORD)file_size) {
        free(buf);
        CloseHandle(h);
        return NULL;
    }
    CloseHandle(h);
    *out_size = file_size;
    return buf;
}

/* ---- bounds-checked pointer cast ------------------------------------------ */
#define SAFE_AT(buf, bufsz, off, T) \
    (((off) + sizeof(T) <= (bufsz)) ? (const T*)((buf) + (off)) : NULL)

/* ---- extract all RT_RCDATA resources from a PE DLL ------------------------ */
/*
 * Returns 0 on success, -1 on parse error.
 * Each resource blob is heap-allocated and stored in *out.
 */
static int parse_pe_resources(const uint8_t *data, size_t data_size,
                               me_lsfg_shaders *out) {
    /* DOS header */
    const DOSHeader *dos = SAFE_AT(data, data_size, 0, DOSHeader);
    if (!dos || dos->magic != 0x5A4D) {
        fprintf(stderr, "[lsfg] bad DOS magic\n");
        return -1;
    }

    /* PE header */
    size_t pe_off = (size_t)(uint32_t)dos->pe_offset;
    const PEHeader *pe = SAFE_AT(data, data_size, pe_off, PEHeader);
    if (!pe || pe->signature != 0x00004550) {
        fprintf(stderr, "[lsfg] bad PE signature\n");
        return -1;
    }

    /* Optional header — must be PE32+ */
    size_t opt_off = pe_off + sizeof(PEHeader);
    const PEOptHeader *opt = SAFE_AT(data, data_size, opt_off, PEOptHeader);
    if (!opt || opt->magic != 0x020B) {
        fprintf(stderr, "[lsfg] not PE32+\n");
        return -1;
    }
    uint32_t rsrc_rva  = opt->data_dirs[2].rva;
    uint32_t rsrc_size_pe = opt->data_dirs[2].size;
    if (rsrc_rva == 0) {
        fprintf(stderr, "[lsfg] no resource section\n");
        return -1;
    }
    (void)rsrc_size_pe;

    /* Section headers */
    size_t sect_off = opt_off + pe->opt_hdr_size;
    uint16_t sect_count = pe->sect_count;

    /* Find the section that contains the resource RVA */
    size_t rsrc_file_off = 0;
    for (int s = 0; s < sect_count; s++) {
        size_t sh_off = sect_off + (size_t)s * sizeof(SectionHeader);
        const SectionHeader *sh = SAFE_AT(data, data_size, sh_off, SectionHeader);
        if (!sh) { fprintf(stderr, "[lsfg] section header OOB\n"); return -1; }
        if (rsrc_rva >= sh->vaddress && rsrc_rva < sh->vaddress + sh->vsize) {
            rsrc_file_off = (rsrc_rva - sh->vaddress) + sh->foffset;
            break;
        }
    }
    if (rsrc_file_off == 0) {
        fprintf(stderr, "[lsfg] resource section not mapped\n");
        return -1;
    }

    /* Root resource directory */
    const ResourceDirectory *root = SAFE_AT(data, data_size, rsrc_file_off, ResourceDirectory);
    if (!root) { fprintf(stderr, "[lsfg] resource dir OOB\n"); return -1; }

    uint32_t total_entries = root->name_count + root->id_count;
    size_t entries_off = rsrc_file_off + sizeof(ResourceDirectory);

    /* Scan root entries for RT_RCDATA (id == 10) */
    size_t rcdata_subdir_off = 0;
    for (uint32_t i = 0; i < total_entries; i++) {
        size_t entry_off = entries_off + (size_t)i * sizeof(ResourceDirectoryEntry);
        const ResourceDirectoryEntry *e = SAFE_AT(data, data_size, entry_off, ResourceDirectoryEntry);
        if (!e) break;
        if (e->id == 10 && (e->offset & 0x80000000)) {
            rcdata_subdir_off = rsrc_file_off + (e->offset & 0x7FFFFFFF);
            break;
        }
    }
    if (rcdata_subdir_off == 0) {
        fprintf(stderr, "[lsfg] RT_RCDATA not found\n");
        return -1;
    }

    /* RT_RCDATA sub-directory (contains individual shader entries by ID) */
    const ResourceDirectory *rcdata_dir = SAFE_AT(data, data_size, rcdata_subdir_off, ResourceDirectory);
    if (!rcdata_dir) { fprintf(stderr, "[lsfg] RT_RCDATA dir OOB\n"); return -1; }

    uint32_t shader_count = rcdata_dir->name_count + rcdata_dir->id_count;
    size_t shader_entries_off = rcdata_subdir_off + sizeof(ResourceDirectory);

    /* Allocate entry table */
    out->entries = (ShaderEntry *)calloc(shader_count, sizeof(ShaderEntry));
    if (!out->entries) return -1;
    out->count = 0;
    out->capacity = (int)shader_count;

    for (uint32_t i = 0; i < shader_count; i++) {
        size_t entry_off = shader_entries_off + (size_t)i * sizeof(ResourceDirectoryEntry);
        const ResourceDirectoryEntry *se = SAFE_AT(data, data_size, entry_off, ResourceDirectoryEntry);
        if (!se) break;

        uint32_t shader_id = se->id;

        /* Each shader entry points to a language sub-directory */
        if (!(se->offset & 0x80000000)) continue; /* skip non-directory */
        size_t lang_dir_off = rsrc_file_off + (se->offset & 0x7FFFFFFF);
        const ResourceDirectory *lang_dir = SAFE_AT(data, data_size, lang_dir_off, ResourceDirectory);
        if (!lang_dir) continue;

        /* Language directory has at least one entry pointing to the data entry */
        size_t lang_entry_off = lang_dir_off + sizeof(ResourceDirectory);
        const ResourceDirectoryEntry *le = SAFE_AT(data, data_size, lang_entry_off, ResourceDirectoryEntry);
        if (!le || (le->offset & 0x80000000)) continue; /* must be a data leaf */

        /* Resource data entry (holds RVA + size of raw bytes) */
        size_t data_entry_off = rsrc_file_off + (le->offset & 0x7FFFFFFF);
        const ResourceDataEntry *de = SAFE_AT(data, data_size, data_entry_off, ResourceDataEntry);
        if (!de) continue;

        /* Convert RVA to file offset using the rsrc section base */
        if (de->offset_to_data < rsrc_rva) continue;
        size_t blob_off = (de->offset_to_data - rsrc_rva) + rsrc_file_off;
        if (blob_off + de->size > data_size) continue;

        uint8_t *blob = (uint8_t *)malloc(de->size);
        if (!blob) continue;
        memcpy(blob, data + blob_off, de->size);

        out->entries[out->count].id   = shader_id;
        out->entries[out->count].size = de->size;
        out->entries[out->count].data = blob;
        out->count++;
    }

    return 0;
}

/* ---- resolve exe directory ------------------------------------------------ */
static void get_exe_dir(char *out, size_t out_sz) {
    GetModuleFileNameA(NULL, out, (DWORD)out_sz);
    char *last = out;
    for (char *p = out; *p; p++) {
        if (*p == '\\' || *p == '/') last = p;
    }
    *last = '\0'; /* truncate at last separator */
}

/* ---- public API ----------------------------------------------------------- */

me_lsfg_shaders *me_lsfg_load(const char *explicit_dll_path) {
    char dll_path[MAX_PATH] = {0};

    if (explicit_dll_path && explicit_dll_path[0]) {
        /* Explicit path from --lsfg-dll= */
        snprintf(dll_path, sizeof(dll_path), "%s", explicit_dll_path);
    } else {
        /* Default: <exe_dir>/lsfg/Lossless.dll */
        char exe_dir[MAX_PATH] = {0};
        get_exe_dir(exe_dir, sizeof(exe_dir));
        snprintf(dll_path, sizeof(dll_path), "%s\\lsfg\\Lossless.dll", exe_dir);
    }

    /* Check the file actually exists before trying to parse it */
    DWORD attr = GetFileAttributesA(dll_path);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        char exe_dir[MAX_PATH] = {0};
        get_exe_dir(exe_dir, sizeof(exe_dir));
        fprintf(stderr,
            "[lsfg] ERROR: Lossless.dll not found.\n"
            "[lsfg]   Searched: %s\n"
            "[lsfg]\n"
            "[lsfg]   To use LSFG frame generation you must own Lossless Scaling on Steam.\n"
            "[lsfg]   Copy Lossless.dll from your Lossless Scaling installation to:\n"
            "[lsfg]     %s\\lsfg\\Lossless.dll\n"
            "[lsfg]   Or use --lsfg-dll=<path> to specify the DLL location explicitly.\n",
            dll_path, exe_dir);
        return NULL;
    }

    fprintf(stderr, "[lsfg] loading shaders from: %s\n", dll_path);

    size_t file_size = 0;
    uint8_t *file_data = read_file_all(dll_path, &file_size);
    if (!file_data) {
        fprintf(stderr, "[lsfg] ERROR: could not read %s (error %lu)\n",
                dll_path, (unsigned long)GetLastError());
        return NULL;
    }

    me_lsfg_shaders *shaders = (me_lsfg_shaders *)calloc(1, sizeof(me_lsfg_shaders));
    if (!shaders) { free(file_data); return NULL; }

    snprintf(shaders->dll_path, sizeof(shaders->dll_path), "%s", dll_path);

    if (parse_pe_resources(file_data, file_size, shaders) != 0) {
        free(file_data);
        me_lsfg_free(shaders);
        fprintf(stderr, "[lsfg] ERROR: failed to parse PE resources from %s\n", dll_path);
        return NULL;
    }

    free(file_data);

    fprintf(stderr, "[lsfg] extracted %d shader resources from Lossless.dll\n",
            shaders->count);

    return shaders;
}

int me_lsfg_shader_count(const me_lsfg_shaders *shaders) {
    if (!shaders) return 0;
    return shaders->count;
}

const uint8_t *me_lsfg_shader_get(const me_lsfg_shaders *shaders,
                                   uint32_t resource_id,
                                   size_t *size_out) {
    if (!shaders) return NULL;
    for (int i = 0; i < shaders->count; i++) {
        if (shaders->entries[i].id == resource_id) {
            if (size_out) *size_out = shaders->entries[i].size;
            return shaders->entries[i].data;
        }
    }
    return NULL;
}

const char *me_lsfg_dll_path(const me_lsfg_shaders *shaders) {
    if (!shaders) return NULL;
    return shaders->dll_path;
}

void me_lsfg_free(me_lsfg_shaders *shaders) {
    if (!shaders) return;
    if (shaders->entries) {
        for (int i = 0; i < shaders->count; i++) {
            free(shaders->entries[i].data);
        }
        free(shaders->entries);
    }
    free(shaders);
}
