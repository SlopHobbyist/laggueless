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

typedef struct {
    /* Indexed by me_input_id. */
    me_kb_binding keys[ME_IN_COUNT];
} me_control_map;

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
    int match_display_hz;   /* snap pacing to monitor refresh when within tolerance */

    /* audio */
    int no_audio;
    int audio_exclusive; /* opt-in: WASAPI exclusive mode (~3 ms vs ~10 ms latency) */

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

    /* hotkeys (keyboard only — controller bindings parsed but unused for now) */
    me_kb_binding hk_cycle_aspect;
    me_kb_binding hk_toggle_fullscreen;
    me_kb_binding hk_quit;
    me_kb_binding hk_reset;

    /* Universal player-1 control map. */
    me_control_map universal;

    /* Per-core entry. */
    struct me_core_entry {
        char  name[64];          /* short name from yaml (e.g. "snes9x") */
        char  dll[260];          /* dll filename or path */
        int   use_universal;
        me_control_map controls; /* used when use_universal == 0 */
    } *cores;
    size_t cores_n;
} me_settings;

/* Fill `out` with hard-coded defaults (used when settings.yaml is missing or
   any individual field is absent). Safe to call repeatedly. */
void me_settings_defaults(me_settings *out);

/* Load settings from `path`. On success returns 0 and fills `out`. On a
   missing file returns 1 with defaults already in `out` (call defaults
   yourself before loading). On parse error returns -1 and leaves whatever
   was loaded so far. Prints a warning on stderr in either error case. */
int  me_settings_load(const char *path, me_settings *out);

/* Free heap-allocated members (cores array). The struct itself is owned by
   the caller (typically static or stack). */
void me_settings_free(me_settings *s);

/* Look up a per-core entry by short name (e.g. "snes9x") or by DLL filename
   match (case-insensitive, trailing path stripped). Returns NULL if no
   entry matches. */
const struct me_core_entry *me_settings_find_core(const me_settings *s,
                                                  const char *core_path);

#endif
