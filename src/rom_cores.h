#ifndef ME_ROM_CORES_H
#define ME_ROM_CORES_H

#include <stddef.h>
#include "settings.h"

/* Console table: which ROM extensions belong to which console, and the cores
   that can run it. Each console opens in our pick of core unless the player
   picked another in Cores > Set Cores (settings.yaml `console_cores:`). Used
   when a ROM is launched without an explicit core (single CLI argument,
   File > Open, Open Recent, or dropped on the window). Core DLLs live in the
   cores/ folder next to the exe. */

typedef struct {
    const char *id;     /* settings.yaml key ("nes") */
    const char *name;   /* shown in Cores > Set Cores ("NES / Famicom") */
    const char *exts;   /* '|'-separated, no dots */
    const char *cores;  /* '|'-separated core names, our pick first; the DLL is
                           <name>_libretro.dll */
} me_console;

int               me_console_count(void);
const me_console *me_console_at(int i);

/* Index of the console that claims this ROM's extension, or -1. */
int me_console_for_rom(const char *rom_path);

/* The console's `i`th candidate core as a DLL filename. Returns 0 past the
   last one. */
int me_console_candidate(const me_console *c, int i, char *dll, size_t dll_sz);

/* The DLL a console opens in: the player's pick if set, else ours. `s` must
   not change underneath (hold the settings lock for the live settings). */
void me_console_core_dll(const me_settings *s, const me_console *c, char *dll, size_t dll_sz);

/* Display name for a core DLL ("Mesen" for mesen_libretro.dll); the DLL
   filename itself for cores we don't know. */
void me_core_display_name(const char *dll, char *out, size_t out_sz);

/* Core DLL filename for a ROM, via me_console_core_dll. Returns 0, or -1 if
   no console claims the ROM's extension. */
int me_rom_core_for(const me_settings *s, const char *rom_path, char *dll, size_t dll_sz);

/* All extensions in the table as a file-dialog pattern: "*.nes;*.fds;...". */
void me_rom_patterns(char *out, size_t out_sz);

#endif
