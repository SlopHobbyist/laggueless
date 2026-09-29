#include "consoles.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* How the frontend's inputs reach the core: me_input_id → RetroPad button.
   A = RetroPad A, B = RetroPad B, X/Y = X/Y, LB/RB = L/R, LT/RT = L2/R2,
   stick clicks = L3/R3, Back = Select. Each console below lists which of
   those RetroPad buttons its cores wire to a real controller button, and
   what that button is called. Taken from the cores' own input descriptors. */

typedef struct { me_input_id id; const char *label; } row;
typedef struct { const char *name; const row *rows; } console;

#define DPAD { ME_IN_DPAD_UP, "D-Pad Up" }, { ME_IN_DPAD_DOWN, "D-Pad Down" }, \
             { ME_IN_DPAD_LEFT, "D-Pad Left" }, { ME_IN_DPAD_RIGHT, "D-Pad Right" }
#define END  { 0, NULL }

/* Nintendo. Every NES, SNES, Game Boy, GBA and DS core uses these. NES
   cores also offer turbo A/B, A+B, FDS disk and VS coin buttons; GB/GBA
   cores turbo and solar-sensor buttons; DS cores lid/mic/screen-swap. */
static const row k_nes_rows[] = {
    DPAD, { ME_IN_A, "A" }, { ME_IN_B, "B" },
    { ME_IN_BACK, "Select" }, { ME_IN_START, "Start" }, END
};
static const row k_snes_rows[] = {
    DPAD, { ME_IN_A, "A" }, { ME_IN_B, "B" }, { ME_IN_X, "X" }, { ME_IN_Y, "Y" },
    { ME_IN_LB, "L" }, { ME_IN_RB, "R" },
    { ME_IN_BACK, "Select" }, { ME_IN_START, "Start" }, END
};
static const row k_gba_rows[] = {
    DPAD, { ME_IN_A, "A" }, { ME_IN_B, "B" }, { ME_IN_LB, "L" }, { ME_IN_RB, "R" },
    { ME_IN_BACK, "Select" }, { ME_IN_START, "Start" }, END
};
static const row k_vb_rows[] = {
    { ME_IN_DPAD_UP,    "Left D-Pad Up" },    { ME_IN_DPAD_DOWN,  "Left D-Pad Down" },
    { ME_IN_DPAD_LEFT,  "Left D-Pad Left" },  { ME_IN_DPAD_RIGHT, "Left D-Pad Right" },
    { ME_IN_LT,         "Right D-Pad Up" },   { ME_IN_LSTICK,     "Right D-Pad Down" },
    { ME_IN_RT,         "Right D-Pad Left" }, { ME_IN_RSTICK,     "Right D-Pad Right" },
    { ME_IN_A, "A" }, { ME_IN_B, "B" }, { ME_IN_LB, "L" }, { ME_IN_RB, "R" },
    { ME_IN_BACK, "Select" }, { ME_IN_START, "Start" }, END
};
static const row k_pokemini_rows[] = {
    DPAD, { ME_IN_A, "A" }, { ME_IN_B, "B" }, { ME_IN_RB, "C" },
    { ME_IN_LB, "Shake" }, { ME_IN_BACK, "Power" }, END
};

/* Sega. Genesis Plus GX, PicoDrive and ClownMDEmu share one 6-button
   mapping; BlastEm has its own. GPGX Wide adds turbo A/B/C. */
static const row k_md_rows[] = {
    DPAD, { ME_IN_Y, "A" }, { ME_IN_B, "B" }, { ME_IN_A, "C" },
    { ME_IN_LB, "X" }, { ME_IN_X, "Y" }, { ME_IN_RB, "Z" },
    { ME_IN_START, "Start" }, { ME_IN_BACK, "Mode" }, END
};
static const row k_md_blastem_rows[] = {
    DPAD, { ME_IN_B, "A" }, { ME_IN_A, "B" }, { ME_IN_RB, "C" },
    { ME_IN_Y, "X" }, { ME_IN_X, "Y" }, { ME_IN_LB, "Z" },
    { ME_IN_START, "Start" }, { ME_IN_BACK, "Mode" }, END
};
static const row k_sms_rows[] = {
    DPAD, { ME_IN_B, "1" }, { ME_IN_A, "2" }, { ME_IN_START, "Pause" }, END
};
static const row k_gg_rows[] = {
    DPAD, { ME_IN_B, "1" }, { ME_IN_A, "2" }, { ME_IN_START, "Start" }, END
};

/* NEC. The standard 2-button pad; the cores' III-VI buttons need the
   6-button pad switched on, and Geargrafx's L2/R2 are turbo toggles. */
static const row k_pce_rows[] = {
    DPAD, { ME_IN_A, "I" }, { ME_IN_B, "II" },
    { ME_IN_BACK, "Select" }, { ME_IN_START, "Run" }, END
};

/* Atari. The 2600 and 7800 console switches are on the RetroPad too; games
   need Reset/Select to start and the difficulty switches change play. */
static const row k_2600_rows[] = {
    { ME_IN_DPAD_UP,    "Joystick Up" },   { ME_IN_DPAD_DOWN,  "Joystick Down" },
    { ME_IN_DPAD_LEFT,  "Joystick Left" }, { ME_IN_DPAD_RIGHT, "Joystick Right" },
    { ME_IN_B, "Fire" },
    { ME_IN_START, "Game Reset" }, { ME_IN_BACK, "Game Select" },
    { ME_IN_LB, "Left Difficulty A" },  { ME_IN_LT, "Left Difficulty B" },
    { ME_IN_RB, "Right Difficulty A" }, { ME_IN_RT, "Right Difficulty B" },
    { ME_IN_LSTICK, "TV Type Color" },  { ME_IN_RSTICK, "TV Type B/W" }, END
};
static const row k_7800_rows[] = {
    DPAD, { ME_IN_B, "1" }, { ME_IN_A, "2" },
    { ME_IN_START, "Pause" }, { ME_IN_BACK, "Select" }, { ME_IN_X, "Reset" },
    { ME_IN_LB, "Left Difficulty" }, { ME_IN_RB, "Right Difficulty" }, END
};
/* Handy and Gearlynx; Beetle Lynx swaps A and B. Screen rotation is left out. */
static const row k_lynx_rows[] = {
    DPAD, { ME_IN_A, "A" }, { ME_IN_B, "B" },
    { ME_IN_LB, "Option 1" }, { ME_IN_RB, "Option 2" }, { ME_IN_START, "Pause" }, END
};
static const row k_lynx_beetle_rows[] = {
    DPAD, { ME_IN_B, "A" }, { ME_IN_A, "B" },
    { ME_IN_LB, "Option 1" }, { ME_IN_RB, "Option 2" }, { ME_IN_START, "Pause" }, END
};

/* SNK and Bandai. */
static const row k_ngp_rows[] = {
    DPAD, { ME_IN_B, "A" }, { ME_IN_A, "B" }, { ME_IN_START, "Option" }, END
};
static const row k_ws_rows[] = {
    { ME_IN_DPAD_UP,   "X1 (Up)" },    { ME_IN_DPAD_RIGHT, "X2 (Right)" },
    { ME_IN_DPAD_DOWN, "X3 (Down)" },  { ME_IN_DPAD_LEFT,  "X4 (Left)" },
    { ME_IN_RT,        "Y1 (Up)" },    { ME_IN_RB,         "Y2 (Right)" },
    { ME_IN_LT,        "Y3 (Down)" },  { ME_IN_LB,         "Y4 (Left)" },
    { ME_IN_A, "A" }, { ME_IN_B, "B" }, { ME_IN_START, "Start" }, END
};

/* Sony. PPSSPP also describes a right stick the PSP doesn't have. */
static const row k_psp_rows[] = {
    DPAD, { ME_IN_B, "Cross" }, { ME_IN_A, "Circle" }, { ME_IN_Y, "Square" }, { ME_IN_X, "Triangle" },
    { ME_IN_LB, "L" }, { ME_IN_RB, "R" },
    { ME_IN_BACK, "Select" }, { ME_IN_START, "Start" },
    { ME_IN_LSTICK_UP,   "Analog Up" },   { ME_IN_LSTICK_DOWN,  "Analog Down" },
    { ME_IN_LSTICK_LEFT, "Analog Left" }, { ME_IN_LSTICK_RIGHT, "Analog Right" }, END
};

static const console k_nes      = { "NES",               k_nes_rows };
static const console k_snes     = { "SNES",              k_snes_rows };
static const console k_gb       = { "Game Boy",          k_nes_rows };
static const console k_gbc      = { "Game Boy Color",    k_nes_rows };
static const console k_gba      = { "Game Boy Advance",  k_gba_rows };
static const console k_nds      = { "Nintendo DS",       k_snes_rows };
static const console k_vb       = { "Virtual Boy",       k_vb_rows };
static const console k_pokemini = { "Pokemon Mini",      k_pokemini_rows };
static const console k_md       = { "Mega Drive / Genesis", k_md_rows };
static const console k_md_blastem = { "Mega Drive / Genesis", k_md_blastem_rows };
static const console k_sms      = { "Master System",     k_sms_rows };
static const console k_sg1000   = { "SG-1000",           k_sms_rows };
static const console k_gg       = { "Game Gear",         k_gg_rows };
static const console k_pce      = { "PC Engine",         k_pce_rows };
static const console k_sgx      = { "SuperGrafx",        k_pce_rows };
static const console k_2600     = { "Atari 2600",        k_2600_rows };
static const console k_7800     = { "Atari 7800",        k_7800_rows };
static const console k_lynx     = { "Atari Lynx",        k_lynx_rows };
static const console k_lynx_beetle = { "Atari Lynx",     k_lynx_beetle_rows };
static const console k_ngp      = { "Neo Geo Pocket",    k_ngp_rows };
static const console k_ngpc     = { "Neo Geo Pocket Color", k_ngp_rows };
static const console k_ws       = { "WonderSwan",        k_ws_rows };
static const console k_wsc      = { "WonderSwan Color",  k_ws_rows };
static const console k_psp      = { "PSP",               k_psp_rows };

/* First match wins. `cores`: '|'-separated library_name prefixes, NULL = any
   core. `exts`: '|'-separated ROM extensions, NULL = any. A NULL console
   means no curated layout (the core's descriptors are used). */
static const struct {
    const char *cores;
    const char *exts;
    const console *c;
} k_rules[] = {
    /* Nintendo: button names are the same in every core. */
    { NULL, "nes|fds|unf|unif|qd",                       &k_nes },
    { NULL, "sfc|smc|swc|fig|bs|st|gd3|gd7|dx2|bsx",     &k_snes },
    { NULL, "gb|dmg|sgb",                                &k_gb },
    { NULL, "gbc|cgb",                                   &k_gbc },
    { NULL, "gba|agb",                                   &k_gba },
    { NULL, "nds|dsi|ids",                               &k_nds },
    { "Beetle VB",                          NULL,        &k_vb },
    { "PokeMini",                           NULL,        &k_pokemini },

    /* Sega. */
    { "BlastEm", "md|gen|smd|32x|bin|cue|iso|chd|toc",   &k_md_blastem },
    { "BlastEm",                            NULL,        NULL },
    { "Genesis Plus GX|PicoDrive|Gearsystem|SMS Plus GX", "sms", &k_sms },
    { "Genesis Plus GX|PicoDrive|Gearsystem|SMS Plus GX", "sg|sc|mv", &k_sg1000 },
    { "Genesis Plus GX|PicoDrive|Gearsystem|SMS Plus GX", "gg",  &k_gg },
    { "Gearsystem|SMS Plus GX",             "bin|rom",   &k_sms },
    { "PicoDrive",                          "pco|vgm|vgz", NULL },
    { "Genesis Plus GX|PicoDrive|ClownMDEmu", NULL,      &k_md },

    /* NEC. */
    { "Beetle PCE|Beetle SuperGrafx|Mednafen PCE|Mednafen SuperGrafx|Geargrafx", "sgx", &k_sgx },
    { "Beetle PCE|Beetle SuperGrafx|Mednafen PCE|Mednafen SuperGrafx|Geargrafx", "hes", NULL },
    { "Beetle PCE|Beetle SuperGrafx|Mednafen PCE|Mednafen SuperGrafx|Geargrafx", NULL,  &k_pce },

    /* Atari. */
    { "Stella",                             NULL,        &k_2600 },
    { "ProSystem",                          NULL,        &k_7800 },
    { "Handy|Gearlynx",                     NULL,        &k_lynx },
    { "Beetle Lynx",                        NULL,        &k_lynx_beetle },

    /* SNK, Bandai, Sony. */
    { "Beetle NeoPop|RACE",                 "ngc|ngpc",  &k_ngpc },
    { "Beetle NeoPop|RACE",                 NULL,        &k_ngp },
    { "Beetle WonderSwan",                  "wsc",       &k_wsc },
    { "Beetle WonderSwan",                  NULL,        &k_ws },
    { "PPSSPP",                             NULL,        &k_psp },
};

/* Is `s` (length n) one of the '|'-separated items in `list`? Prefix match
   when `prefix` is set (library names carry suffixes like "Wide"). */
static int in_list(const char *list, const char *s, int prefix) {
    size_t n = strlen(s);
    while (*list) {
        const char *end = strchr(list, '|');
        size_t len = end ? (size_t)(end - list) : strlen(list);
        if (len && (prefix ? n >= len : n == len) && _strnicmp(list, s, len) == 0) return 1;
        if (!end) break;
        list = end + 1;
    }
    return 0;
}

static const char *extension_of(const char *path) {
    const char *ext = "";
    for (const char *p = path; *p; p++) {
        if (*p == '\\' || *p == '/') ext = "";
        else if (*p == '.')          ext = p + 1;
    }
    return ext;
}

static int has_row(const me_input_layout *l, me_input_id id) {
    for (int i = 0; i < l->n; i++) if (l->ids[i] == id) return 1;
    return 0;
}

/* Append a row; an input already listed keeps its first name. */
static void add_row(me_input_layout *l, me_input_id id, const char *label, int advanced) {
    if (has_row(l, id)) return;
    l->ids[l->n++] = (unsigned char)id;
    snprintf(l->labels[id], sizeof(l->labels[id]), "%s", label);
    if (advanced) l->advanced |= 1u << id;
    else          l->live     |= 1u << id;
}

/* Rows for the unknown layout: d-pad, face buttons, system,
   shoulders/triggers, stick clicks, stick directions. */
static const me_input_id k_default_order[ME_IN_COUNT] = {
    ME_IN_DPAD_UP, ME_IN_DPAD_DOWN, ME_IN_DPAD_LEFT, ME_IN_DPAD_RIGHT,
    ME_IN_A, ME_IN_B, ME_IN_X, ME_IN_Y,
    ME_IN_START, ME_IN_BACK,
    ME_IN_LB, ME_IN_RB, ME_IN_LT, ME_IN_RT,
    ME_IN_LSTICK, ME_IN_RSTICK,
    ME_IN_LSTICK_UP, ME_IN_LSTICK_DOWN, ME_IN_LSTICK_LEFT, ME_IN_LSTICK_RIGHT,
    ME_IN_RSTICK_UP, ME_IN_RSTICK_DOWN, ME_IN_RSTICK_LEFT, ME_IN_RSTICK_RIGHT,
};

void me_layout_unknown(me_input_layout *out) {
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < ME_IN_COUNT; i++)
        add_row(out, k_default_order[i], me_input_label(k_default_order[i]), 0);
}

int me_layout_for_game(const char *library_name, const char *rom_path, me_input_layout *out) {
    const char *lib = library_name ? library_name : "";
    const char *ext = extension_of(rom_path ? rom_path : "");
    for (size_t i = 0; i < sizeof(k_rules) / sizeof(k_rules[0]); i++) {
        if (k_rules[i].cores && !in_list(k_rules[i].cores, lib, 1)) continue;
        if (k_rules[i].exts  && !in_list(k_rules[i].exts,  ext, 0)) continue;
        const console *c = k_rules[i].c;
        if (!c) return 0;
        memset(out, 0, sizeof(*out));
        out->curated = 1;
        snprintf(out->name, sizeof(out->name), "%s", c->name);
        for (const row *r = c->rows; r->label; r++) add_row(out, r->id, r->label, 0);
        return 1;
    }
    return 0;
}

/* RetroPad button → our input. */
static const me_input_id k_from_retropad[16] = {
    [RETRO_DEVICE_ID_JOYPAD_B]      = ME_IN_B,
    [RETRO_DEVICE_ID_JOYPAD_Y]      = ME_IN_Y,
    [RETRO_DEVICE_ID_JOYPAD_SELECT] = ME_IN_BACK,
    [RETRO_DEVICE_ID_JOYPAD_START]  = ME_IN_START,
    [RETRO_DEVICE_ID_JOYPAD_UP]     = ME_IN_DPAD_UP,
    [RETRO_DEVICE_ID_JOYPAD_DOWN]   = ME_IN_DPAD_DOWN,
    [RETRO_DEVICE_ID_JOYPAD_LEFT]   = ME_IN_DPAD_LEFT,
    [RETRO_DEVICE_ID_JOYPAD_RIGHT]  = ME_IN_DPAD_RIGHT,
    [RETRO_DEVICE_ID_JOYPAD_A]      = ME_IN_A,
    [RETRO_DEVICE_ID_JOYPAD_X]      = ME_IN_X,
    [RETRO_DEVICE_ID_JOYPAD_L]      = ME_IN_LB,
    [RETRO_DEVICE_ID_JOYPAD_R]      = ME_IN_RB,
    [RETRO_DEVICE_ID_JOYPAD_L2]     = ME_IN_LT,
    [RETRO_DEVICE_ID_JOYPAD_R2]     = ME_IN_RT,
    [RETRO_DEVICE_ID_JOYPAD_L3]     = ME_IN_LSTICK,
    [RETRO_DEVICE_ID_JOYPAD_R3]     = ME_IN_RSTICK,
};

/* Never passed to a core, not even as an advanced input: turbo/autofire,
   and macros that press several buttons at once ("A+B"). Every input the
   game sees must be one real button press. */
static int is_banned(const char *desc) {
    for (const char *p = desc; *p; p++) {
        if (_strnicmp(p, "turbo", 5) == 0) return 1;
        if (*p == '+' && p > desc && p[-1] != ' ' && p[1] && p[1] != ' ') return 1;
    }
    return 0;
}

static int is_pad_button(const struct retro_input_descriptor *d) {
    return d->port == 0 && (d->device & RETRO_DEVICE_MASK) == RETRO_DEVICE_JOYPAD && d->id < 16;
}

/* Inputs `d` describes as banned. First, so a button described twice
   can't sneak back in under its other description. */
static unsigned banned_ids(const struct retro_input_descriptor *d) {
    unsigned banned = 0;
    for (; d && d->description; d++)
        if (is_pad_button(d) && is_banned(d->description)) banned |= 1u << k_from_retropad[d->id];
    return banned;
}

/* Rows for what `d` describes on port 0, skipping banned inputs and ones
   already listed: d-pad first in the usual order, then the rest as the
   core lists them. */
static void add_described(me_input_layout *l, const struct retro_input_descriptor *d,
                          unsigned banned, int advanced) {
    for (me_input_id dir = ME_IN_DPAD_UP; dir <= ME_IN_DPAD_RIGHT; dir++) {
        for (const struct retro_input_descriptor *t = d; t && t->description; t++) {
            if (is_pad_button(t) && k_from_retropad[t->id] == dir && !(banned & (1u << dir)))
                add_row(l, dir, t->description, advanced);
        }
    }
    for (; d && d->description; d++) {
        if (d->port != 0) continue;
        unsigned device = d->device & RETRO_DEVICE_MASK;
        if (is_pad_button(d)) {
            me_input_id id = k_from_retropad[d->id];
            if (!(banned & (1u << id))) add_row(l, id, d->description, advanced);
        } else if (device == RETRO_DEVICE_ANALOG && d->index <= RETRO_DEVICE_INDEX_ANALOG_RIGHT) {
            me_input_id first = d->index == RETRO_DEVICE_INDEX_ANALOG_LEFT ? ME_IN_LSTICK_UP
                                                                          : ME_IN_RSTICK_UP;
            for (int k = 0; k < 4; k++)
                add_row(l, (me_input_id)(first + k), me_input_label((me_input_id)(first + k)), advanced);
        }
    }
}

void me_layout_from_descriptors(const char *core_name,
                                const struct retro_input_descriptor *d,
                                me_input_layout *out) {
    unsigned banned = banned_ids(d);
    memset(out, 0, sizeof(*out));
    add_described(out, d, banned, 0);
    if (out->n == 0) me_layout_unknown(out);
    else snprintf(out->name, sizeof(out->name), "%s", core_name ? core_name : "");
    /* Only banned inputs are held back: a core may read inputs it never
       described (analog sticks especially), and those must keep working. */
    out->live = ME_IN_ALL & ~banned;
}

void me_layout_set_advanced(me_input_layout *l, const struct retro_input_descriptor *d) {
    /* Drop the previous advanced rows; cores may re-describe their inputs. */
    int n = 0;
    for (int i = 0; i < l->n; i++)
        if (!(l->advanced & (1u << l->ids[i]))) l->ids[n++] = l->ids[i];
    l->n = n;
    l->advanced = 0;
    add_described(l, d, banned_ids(d), 1);
}
