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

/* A multiplayer adapter (Four Score, Multitap...): with it plugged in, a
   game gets ME_MAX_PLAYERS players instead of the console's own ports.
   Libretro cores number every player as its own port either way. */
typedef struct {
    const char *id;       /* settings.yaml value ("four_score"); NULL ends a list */
    const char *name;     /* Controls menu ("Four Score") */
    unsigned    ports;    /* bit per libretro port the adapter's device goes on */
    const char *devices;  /* '|'-separated names cores give that device in
                             SET_CONTROLLER_INFO, matched as case-insensitive
                             substrings. The first of the core's devices that
                             matches is used; a core with none can't use the
                             adapter. NULL: the cores need no device (the
                             adapter is always there, or set up in core options). */
} me_adapter;

typedef struct {
    const char *id;     /* settings.yaml key ("nes") */
    const char *name;   /* shown in Cores > Set Cores ("NES / Famicom") */
    const char *exts;   /* '|'-separated, no dots. Each extension is in exactly
                           one console's list: the console File Associations
                           gives it to. */
    const char *also_exts;  /* NULL, or extensions in other consoles' `exts`
                               that this console's games come in too ("bin"
                               for the Atari 7800). The ROM's header tells
                               them apart, or the player picks. */
    const char *cores;  /* '|'-separated core names, our pick first; the DLL is
                           <name>_libretro.dll */
    int players;        /* controller ports on the console */
    const me_adapter *adapters;  /* NULL, or a list ended by a NULL id */
    unsigned flags;     /* ME_CONSOLE_* */
} me_console;

enum {
    /* Two 4:3 screens the cores draw into one frame, top screen first (DS):
       View > Screen can show just one. */
    ME_CONSOLE_TWO_SCREENS = 1u << 0,
    /* A touch screen: the mouse reaches the core as RETRO_DEVICE_POINTER
       and RETRO_DEVICE_MOUSE. */
    ME_CONSOLE_TOUCH       = 1u << 1,
    /* The controller has an analog stick: the left stick is that stick,
       not the D-pad, unless the player switches it (consoles.h). */
    ME_CONSOLE_ANALOG      = 1u << 2,
};

int               me_console_count(void);
const me_console *me_console_at(int i);

/* A set of consoles, a bit per index (there are at most 64). */
typedef unsigned long long me_console_set;

/* me_console_for_rom's results that aren't a console index. */
enum { ME_ROM_UNKNOWN = -1, ME_ROM_AMBIGUOUS = -2 };

/* Index of the console a ROM is for. Its extension gives the candidates (the
   console whose `exts` has it, and those whose `also_exts` do); with more
   than one, the file's header decides, reading a cue sheet's data track for
   a .cue. ME_ROM_UNKNOWN: no console takes the extension. ME_ROM_AMBIGUOUS:
   the header doesn't settle it, and the player has to pick. `candidates`
   (may be NULL) gets the consoles it could be. */
int me_console_for_rom(const char *rom_path, me_console_set *candidates);

/* Every console whose extension lists (`exts` or `also_exts`) have the
   ROM's extension, header aside. */
me_console_set me_rom_candidates(const char *rom_path);

/* A ROM's fingerprint, 16 hex digits, for remembering things about it that
   survive moving or renaming the file: a hash of its size and its first
   and last 64 KB (a .cue's data track's). Returns 0 if it can't be read. */
int me_rom_fingerprint(const char *rom_path, char *out, size_t out_sz);

/* The console's adapter with this id, or NULL. */
const me_adapter *me_console_adapter(const me_console *c, const char *id);

/* The console's `i`th candidate core as a DLL filename. Returns 0 past the
   last one. */
int me_console_candidate(const me_console *c, int i, char *dll, size_t dll_sz);

/* The DLL a console opens in: the player's pick if set, else ours. `s` must
   not change underneath (hold the settings lock for the live settings). */
void me_console_core_dll(const me_settings *s, const me_console *c, char *dll, size_t dll_sz);

/* Display name for a core DLL ("Mesen" for mesen_libretro.dll); the DLL
   filename itself for cores we don't know. */
void me_core_display_name(const char *dll, char *out, size_t out_sz);

/* A system file a core needs to start games (a BIOS...), and whether it's
   in the firmware folder. */
typedef struct {
    const char *label;     /* what it is ("PlayStation BIOS") */
    const char *show;      /* the file names to ask the player for */
    int         some_games;/* only games with an extra chip need it */
    char        found[128];/* the names it's there under, "" if missing */
} me_firmware_file;

/* The system files core `dll` needs to start `console_id`'s games (NULL:
   any console's), checked in `system_dir`. Fills up to `max`; returns how
   many (0: it needs none). Optional files (ones that only add extras) are
   never listed. */
int me_core_firmware(const char *dll, const char *console_id, const char *system_dir,
                     me_firmware_file *out, int max);

/* The files me_core_firmware reports missing, one "label: names" line
   each; "" when none are. Ones only some games need aren't counted. */
void me_core_missing_files(const char *dll, const char *console_id, const char *system_dir,
                           char *out, size_t out_sz);

/* All extensions in the table as a file-dialog pattern: "*.nes;*.fds;...". */
void me_rom_patterns(char *out, size_t out_sz);

#endif
