#ifndef ME_SETTINGS_H
#define ME_SETTINGS_H

#include <stddef.h>

/* Inputs we know how to map. Per-core or per-universal bindings live in an
   array indexed by these values. Names match Xbox/XInput vocabulary so the
   settings file reads consistently. */
typedef enum {
    ME_IN_DPAD_UP = 0,
    ME_IN_DPAD_DOWN,
    ME_IN_DPAD_LEFT,
    ME_IN_DPAD_RIGHT,
    ME_IN_A,
    ME_IN_B,
    ME_IN_X,
    ME_IN_Y,
    ME_IN_LB,
    ME_IN_RB,
    ME_IN_LT,
    ME_IN_RT,
    ME_IN_LSTICK,           /* left stick click */
    ME_IN_RSTICK,           /* right stick click */
    ME_IN_START,
    ME_IN_BACK,
    ME_IN_LSTICK_UP,
    ME_IN_LSTICK_DOWN,
    ME_IN_LSTICK_LEFT,
    ME_IN_LSTICK_RIGHT,
    ME_IN_RSTICK_UP,
    ME_IN_RSTICK_DOWN,
    ME_IN_RSTICK_LEFT,
    ME_IN_RSTICK_RIGHT,
    ME_IN_COUNT
} me_input_id;

/* A single keyboard binding: a Win32 virtual-key code (0 = unbound) plus
   modifier flags. Modifiers cover Ctrl/Alt/Shift chords like "Ctrl+R". */
typedef struct {
    unsigned vk;
    unsigned char ctrl;
    unsigned char alt;
    unsigned char shift;
} me_kb_binding;

/* Up to ME_KB_MAX_BINDINGS chords for one hotkey action. */
#define ME_KB_MAX_BINDINGS 4
typedef struct {
    me_kb_binding b[ME_KB_MAX_BINDINGS];
    int           count;
} me_kb_bindings;

/* A single XInput binding: a button bitmask (ME_XI_* values from xinput_pad.h).
   0 = unbound. For hotkey chords all set bits must be held simultaneously. */
typedef struct {
    unsigned buttons; /* ME_XI_* bitmask */
} me_xi_binding;

#define ME_XI_MAX_BINDINGS 4
typedef struct {
    me_xi_binding b[ME_XI_MAX_BINDINGS];
    int           count;
} me_xi_bindings;

typedef struct {
    /* Indexed by me_input_id. */
    me_kb_bindings keys[ME_IN_COUNT];
    me_xi_bindings xi[ME_IN_COUNT];
} me_control_map;

/* libretro ports we feed input to (players 3/4 aren't supported). */
#define ME_MAX_PLAYERS 2

/* Which devices drive a player (or the hotkeys). */
typedef enum {
    ME_SRC_BOTH = 0,
    ME_SRC_KEYBOARD,
    ME_SRC_CONTROLLER,
} me_input_source;

typedef enum {
    ME_HK_CYCLE_ASPECT = 0,
    ME_HK_TOGGLE_FULLSCREEN,
    ME_HK_EXIT_FULLSCREEN,
    ME_HK_QUIT,
    ME_HK_HARD_RESET,
    ME_HK_COUNT
} me_hotkey_id;

/* File > Open Recent length. */
#define ME_RECENT_MAX 20

typedef enum {
    ME_ASPECT_1_1 = 0,
    ME_ASPECT_4_3 = 1,
    ME_ASPECT_16_9 = 2,
} me_aspect_mode;

typedef struct {
    /* video */
    int fullscreen_on_launch;
    me_aspect_mode aspect;
    int force_gdi;
    int force_d3d11;
    int force_vulkan;       /* use the Vulkan present path */
    int vk_no_vsync;        /* Vulkan IMMEDIATE present mode */
    int vk_mailbox;         /* Vulkan MAILBOX present mode (ignored if vk_no_vsync) */
    int vk_validate;        /* enable VK_LAYER_KHRONOS_validation */
    int vk_exclusive_fullscreen; /* VK_EXT_full_screen_exclusive: bypass DWM for lowest latency */
    int match_display_hz;   /* snap pacing to monitor refresh when within tolerance */
    int match_strict;       /* 1=competition (snap only at <0.05% speed error),
                               0=casual (allow up to 0.3% error for smoothness) */

    /* LSFG 3.1 frame generation (View > Frame Gen, or --lsfg). */
    int   lsfg_enabled;
    int   lsfg_multiplier;  /* 2/3/4 — number of presented frames per real frame */
    float lsfg_flow_scale;  /* 0.25..1.0 — optical-flow resolution scale */
    int   lsfg_perf_mode;   /* 1 = reduced-quality / lower GPU cost */

    /* audio */
    int no_audio;
    int exclusive_mode;  /* WASAPI exclusive — bypasses mixer, ~3ms buffer, locks device */
    int low_latency;     /* IAudioClient3 shared low-latency (Win10 1607+) */

    /* thread affinity: pin emulation and audio threads to isolated cores.
       Reduces context-switch jitter on hybrid CPUs (Alder Lake+, Ryzen). */
    int thread_affinity; /* 0 = off, 1 = auto-detect P-cores, else leave OS default */

    /* run-ahead: number of frames to simulate ahead each iteration. 0 disables.
       Reduces visible input latency by displaying a future frame. Each ghost
       frame costs one extra retro_run() call plus a serialize/unserialize. */
    int runahead_frames;

    /* log */
    int pace_log;
    int timing_log;
    int latency_log;       /* per-stage latency breakdown: poll/core/present/wait */
    int env_trace;

    /* hotkeys — keyboard and controller bindings, indexed by me_hotkey_id */
    me_kb_bindings  hk[ME_HK_COUNT];
    me_xi_bindings  hk_xi[ME_HK_COUNT];
    me_input_source hk_source;
    int             hk_xi_index;   /* XInput slot 0..3 that triggers hotkeys */

    /* Per player: devices in use and XInput slot (0..3). */
    me_input_source input_source[ME_MAX_PLAYERS];
    int             xi_index[ME_MAX_PLAYERS];

    /* Universal control maps, one per player. */
    me_control_map universal[ME_MAX_PLAYERS];

    /* Advanced inputs: what a core offers beyond its console's controller
       (FDS disk swap, VS coins, DS lid/mic, PC Engine III-VI, ...; see
       consoles.h). Off by default. They sit on RetroPad buttons the console
       doesn't have, so they get their own maps, unbound by default, instead
       of inheriting another console's button there. Not per-core. */
    int            show_advanced_inputs;
    me_control_map advanced[ME_MAX_PLAYERS];

    /* Per-core entry. */
    struct me_core_entry {
        char  name[64];          /* short name from yaml (e.g. "snes9x") */
        char  dll[260];          /* dll filename or path */
        int   use_universal;
        me_control_map controls[ME_MAX_PLAYERS]; /* used when use_universal == 0 */
        /* Bit per me_input_id: set when this core overrides the input; the
           rest follow the universal map. */
        unsigned overrides[ME_MAX_PLAYERS];
    } *cores;
    size_t cores_n;

    /* Recently played ROMs, most recent first, no duplicates. */
    char recent[ME_RECENT_MAX][260];
    int  recent_n;
} me_settings;

/* Fill `out` with hard-coded defaults (used when settings.yaml is missing or
   any individual field is absent). Safe to call repeatedly. */
void me_settings_defaults(me_settings *out);

/* Load settings from `path`. On success returns 0 and fills `out`. On a
   missing file returns 1 with defaults already in `out` (call defaults
   yourself before loading). On parse error returns -1 and leaves whatever
   was loaded so far. Prints a warning on stderr in either error case. */
int  me_settings_load(const char *path, me_settings *out);

/* Defaults as a fresh install gets them: hard-coded defaults overlaid with
   the settings.yaml template embedded in the exe. Used by the "Default"
   buttons in the binding dialogs. */
void me_settings_load_template(me_settings *out);

/* Write every setting to `path` (replacing the file and its comments).
   Returns 0 on success. */
int  me_settings_save(const char *path, const me_settings *s);

/* Generate a default settings.yaml file if it doesn't exist.
   Returns 0 on success, -1 on error. */
int  me_settings_generate_default(const char *path);

/* Free heap-allocated members (cores array). The struct itself is owned by
   the caller (typically static or stack). */
void me_settings_free(me_settings *s);

/* Look up a per-core entry by short name (e.g. "snes9x") or by DLL filename
   match (case-insensitive, trailing path stripped). Returns NULL if no
   entry matches. */
const struct me_core_entry *me_settings_find_core(const me_settings *s,
                                                  const char *core_path);

/* Replace player's universal map; cores follow it for the inputs they don't
   override. */
void me_settings_set_universal(me_settings *s, int player, const me_control_map *m);

/* Replace a core's own map for `player`; inputs that differ from universal
   become overrides. */
void me_settings_set_core_map(me_settings *s, int core, int player, const me_control_map *m);

/* Same match as me_settings_find_core, as an index into s->cores (-1 if none). */
int me_settings_find_core_index(const me_settings *s, const char *core_path);

/* Display names for the binding dialogs. */
const char *me_input_label(me_input_id id);
const char *me_hotkey_label(me_hotkey_id id);

/* Human/YAML text for bindings, e.g. "Ctrl+R", "RShift", "Back+Start".
   Empty string for an unbound slot. */
void me_kb_binding_str(const me_kb_binding *b, char *out, size_t out_sz);
void me_xi_chord_str(unsigned buttons, char *out, size_t out_sz);

int  me_kb_bindings_equal(const me_kb_bindings *a, const me_kb_bindings *b);
int  me_xi_bindings_equal(const me_xi_bindings *a, const me_xi_bindings *b);

#endif
