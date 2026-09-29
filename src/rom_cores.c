#include "rom_cores.h"
#include <string.h>

/* Extension (no dot, case-insensitive) -> core DLL filename in cores/. */
static const struct { const char *ext; const char *core_dll; } k_rom_cores[] = {
    /* NES / Famicom / Famicom Disk System */
    { "nes",  "mesen_libretro.dll" },
    { "fds",  "mesen_libretro.dll" },
    { "unf",  "mesen_libretro.dll" },
    { "unif", "mesen_libretro.dll" },
};

const char *me_rom_core_for(const char *rom_path) {
    if (!rom_path) return NULL;
    const char *ext = NULL;
    for (const char *p = rom_path; *p; p++) {
        if (*p == '\\' || *p == '/') ext = NULL;
        else if (*p == '.')          ext = p + 1;
    }
    if (!ext || !*ext) return NULL;
    for (size_t i = 0; i < sizeof(k_rom_cores) / sizeof(k_rom_cores[0]); i++) {
        if (_stricmp(ext, k_rom_cores[i].ext) == 0) return k_rom_cores[i].core_dll;
    }
    return NULL;
}
