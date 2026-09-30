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
   passed, advanced or not.

   The universal map puts the RetroPad's buttons where the RetroPad has
   them, and cores lay their console's controller out on the RetroPad by
   position, so most consoles need nothing more. Where the core's layout
   doesn't match where the real controller's buttons are (NES B and A side
   by side, GameCube A in the middle...), the console has default bindings
   of its own. The player's changes for a console's games are kept in its
   own map (settings.h `console_controls`), under `key`. */
typedef struct {
    int      curated;                  /* 1 = from the console table */
    char     name[64];                 /* console ("NES"), else the core, else "" */
    int      n;                        /* rows */
    unsigned char ids[ME_IN_COUNT];    /* me_input_id, display order */
    char     labels[ME_IN_COUNT][32];  /* indexed by me_input_id */
    unsigned live;                     /* bit per me_input_id passed to the core */
    unsigned advanced;                 /* bit per advanced row; passed only when
                                          show_advanced_inputs is on */

    char     key[32];                  /* console map key ("nes"); "" = universal */
    char     console[64];              /* that console's name, for the dialogs */
    int      stick;                    /* its controller has an analog stick */
    unsigned def_kb, def_xi;           /* bit per me_input_id with a console default */
    me_kb_bindings def_kbs[ME_IN_COUNT];
    me_xi_bindings def_xis[ME_IN_COUNT];
} me_input_layout;

#define ME_IN_ALL ((1u << ME_IN_COUNT) - 1u)

/* Layout for a game with no information yet: every input, live. */
void me_layout_unknown(me_input_layout *out);

/* Layout for `library_name` (retro_system_info) running `rom_path`, a game
   for the console `console_id` / `console_name` (rom_cores.h; NULL if
   unknown), whose controller has an analog stick if `stick`. Returns 1 when
   the console is in the table (a curated layout), else 0 with every input
   listed, for the core's descriptors to narrow. Either way `out` gets the
   console's key and default bindings. */
int  me_layout_for_game(const char *library_name, const char *rom_path,
                        const char *console_id, const char *console_name, int stick,
                        me_input_layout *out);

/* Layout from RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS (port 0 only), for
   cores with no curated layout. `core_name` is shown in the dialogs. The
   console key and defaults already in `out` are kept. */
void me_layout_from_descriptors(const char *core_name,
                                const struct retro_input_descriptor *d,
                                me_input_layout *out);

/* Replace a curated layout's advanced rows with whatever else the core's
   descriptors offer beyond the controller. */
void me_layout_set_advanced(me_input_layout *l, const struct retro_input_descriptor *d);

/* The player's bindings for a game with layout `l`, layer by layer: the
   universal map, the console's defaults, the player's changes for the
   console (with_console), then the core's own map (`core`: index into
   s->cores, used only when that core doesn't follow universal; -1 none).
   Advanced inputs aren't resolved here: they have their own map. */
void me_controls_effective(const me_settings *s, const me_input_layout *l, int core,
                           int with_console, int player, me_control_map *out);

/* Whether the player's left stick works the D-pad in a game with layout
   `l`. When it does, that is all the stick does: it doesn't move the
   console's analog stick, and bindings to its directions don't fire. The
   player's choice for the console if they made one, else off on consoles
   with an analog stick and the player's setting (on by default) elsewhere.
   `with_console` 0: the default, ignoring the choice for the console. */
int  me_lstick_as_dpad(const me_settings *s, const me_input_layout *l, int with_console,
                       int player);

#endif
