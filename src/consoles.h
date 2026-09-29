#ifndef ME_CONSOLES_H
#define ME_CONSOLES_H

#include "libretro.h"
#include "settings.h"

/* The controller the running game has: which of our inputs exist, the order
   the binding dialogs list them in, and what the console calls them. The
   input callback only passes `live` inputs to the core, so an input the
   dialog hides can't reach the game through a leftover binding.

   Consoles we know get a curated layout: just the buttons on the real
   controller, named the way the console names them. Cores also put extras
   on the RetroPad buttons their console doesn't have: FDS disk swap, VS
   coins, DS lid/mic, PC Engine III-VI, screen rotation... Those become
   `advanced` rows, listed after the controller and hidden, unbound and
   never passed to the core unless the player turns advanced inputs on.

   Any other core gets its layout from its own input descriptors. A core
   that describes nothing (and the dialogs when no game is loaded) gets
   every input under its Xbox name.

   Turbo buttons and multi-button macros ("A+B") are never listed or
   passed, advanced or not. */
typedef struct {
    int      curated;                  /* 1 = from the console table */
    char     name[64];                 /* console ("NES"), else the core, else "" */
    int      n;                        /* rows */
    unsigned char ids[ME_IN_COUNT];    /* me_input_id, display order */
    char     labels[ME_IN_COUNT][32];  /* indexed by me_input_id */
    unsigned live;                     /* bit per me_input_id passed to the core */
    unsigned advanced;                 /* bit per advanced row; passed only when
                                          show_advanced_inputs is on */
} me_input_layout;

#define ME_IN_ALL ((1u << ME_IN_COUNT) - 1u)

/* Layout for a game with no information yet: every input, live. */
void me_layout_unknown(me_input_layout *out);

/* Curated layout for `library_name` (retro_system_info) running `rom_path`.
   Returns 1 and fills `out` when the console is in the table, else 0. */
int  me_layout_for_game(const char *library_name, const char *rom_path, me_input_layout *out);

/* Layout from RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS (port 0 only), for
   cores with no curated layout. `core_name` is shown in the dialogs. */
void me_layout_from_descriptors(const char *core_name,
                                const struct retro_input_descriptor *d,
                                me_input_layout *out);

/* Replace a curated layout's advanced rows with whatever else the core's
   descriptors offer beyond the controller. */
void me_layout_set_advanced(me_input_layout *l, const struct retro_input_descriptor *d);

#endif
