#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "consoles.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "xinput_pad.h"

/* How the frontend's inputs reach the core: me_input_id → RetroPad button.
   A = RetroPad A, B = RetroPad B, X/Y = X/Y, LB/RB = L/R, LT/RT = L2/R2,
   stick clicks = L3/R3, Back = Select. Each console below lists which of
   those RetroPad buttons its cores wire to a real controller button, and
   what that button is called. Taken from the cores' own input descriptors.

   The RetroPad is a SNES pad: B the bottom face button, A the right one, Y
   the left, X the top. The universal map binds the Xbox button in each of
   those places (Xbox A to RetroPad B...), and a console's defaults below
   rebind the buttons its controller has somewhere else. */

typedef struct { me_input_id id; const char *label; } row;

/* An input's default controller buttons (any of them presses it) and
   keyboard key, replacing universal's; 0s keep universal's. */
typedef struct {
    me_input_id id;
    unsigned    xi[ME_XI_MAX_BINDINGS];
    unsigned    vk;
} pad_default;

/* `rows` NULL: the core's descriptors list the inputs; only the defaults
   and key are ours. */
typedef struct {
    const char        *key;
    const char        *name;
    const row         *rows;
    const pad_default *defaults;
} console;

#define DPAD { ME_IN_DPAD_UP, "D-Pad Up" }, { ME_IN_DPAD_DOWN, "D-Pad Down" }, \
             { ME_IN_DPAD_LEFT, "D-Pad Left" }, { ME_IN_DPAD_RIGHT, "D-Pad Right" }
#define END  { 0, NULL }
#define DEFAULTS_END { ME_IN_COUNT, { 0 }, 0 }

/* Two face buttons side by side (NES, Game Boy, Master System, PC Engine,
   Lynx...): the left one on Xbox X and Y, the right one on A and B. */
#define TWO_BUTTONS(left, right) \
    { left,  { ME_XI_X, ME_XI_Y }, 0 }, { right, { ME_XI_A, ME_XI_B }, 0 }

static const pad_default k_two_buttons[] = {
    TWO_BUTTONS(ME_IN_B, ME_IN_A), DEFAULTS_END
};

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
/* The right D-pad on the right stick. */
static const pad_default k_vb_defaults[] = {
    TWO_BUTTONS(ME_IN_B, ME_IN_A),
    { ME_IN_LT, { ME_XI_RSTICK_UP },   0 }, { ME_IN_LSTICK, { ME_XI_RSTICK_DOWN },  0 },
    { ME_IN_RT, { ME_XI_RSTICK_LEFT }, 0 }, { ME_IN_RSTICK, { ME_XI_RSTICK_RIGHT }, 0 },
    DEFAULTS_END
};
static const row k_pokemini_rows[] = {
    DPAD, { ME_IN_A, "A" }, { ME_IN_B, "B" }, { ME_IN_RB, "C" },
    { ME_IN_LB, "Shake" }, { ME_IN_BACK, "Power" }, END
};
/* ParaLLEl N64 and Mupen64Plus-Next. The C buttons are the right stick.
   Their "C Buttons Mode" (hold R2 and the face buttons are C buttons) is
   left out, and with it RetroPad A and X, which only work in that mode. */
static const row k_n64_rows[] = {
    DPAD, { ME_IN_B, "A" }, { ME_IN_Y, "B" }, { ME_IN_LT, "Z" },
    { ME_IN_LB, "L" }, { ME_IN_RB, "R" }, { ME_IN_START, "Start" },
    { ME_IN_LSTICK_UP,   "Control Stick Up" },   { ME_IN_LSTICK_DOWN,  "Control Stick Down" },
    { ME_IN_LSTICK_LEFT, "Control Stick Left" }, { ME_IN_LSTICK_RIGHT, "Control Stick Right" },
    { ME_IN_RSTICK_UP,   "C-Up" },   { ME_IN_RSTICK_DOWN,  "C-Down" },
    { ME_IN_RSTICK_LEFT, "C-Left" }, { ME_IN_RSTICK_RIGHT, "C-Right" }, END
};
/* A on Xbox A and B left of it on X, as on the N64 pad; C-Left and
   C-Right also on the face buttons beside them (Y, B); Z on the left
   trigger, R on either right shoulder button. */
static const pad_default k_n64_defaults[] = {
    { ME_IN_B,  { ME_XI_A },  0 }, { ME_IN_Y,  { ME_XI_X }, 0 },
    { ME_IN_LT, { ME_XI_LT }, VK_SPACE }, { ME_IN_RB, { ME_XI_RB, ME_XI_RT }, 0 },
    { ME_IN_LSTICK_UP,    { ME_XI_LSTICK_UP },    VK_UP },
    { ME_IN_LSTICK_DOWN,  { ME_XI_LSTICK_DOWN },  VK_DOWN },
    { ME_IN_LSTICK_LEFT,  { ME_XI_LSTICK_LEFT },  VK_LEFT },
    { ME_IN_LSTICK_RIGHT, { ME_XI_LSTICK_RIGHT }, VK_RIGHT },
    { ME_IN_RSTICK_UP,    { ME_XI_RSTICK_UP },             'I' },
    { ME_IN_RSTICK_DOWN,  { ME_XI_RSTICK_DOWN },           'K' },
    { ME_IN_RSTICK_LEFT,  { ME_XI_RSTICK_LEFT,  ME_XI_Y }, 'J' },
    { ME_IN_RSTICK_RIGHT, { ME_XI_RSTICK_RIGHT, ME_XI_B }, 'L' },
    DEFAULTS_END
};
/* Dolphin names the RetroPad's buttons after the GameCube's (RetroPad A is
   GameCube A...). The stick clicks press L and R lightly, as the analog
   triggers can (a light R sprays on the move in Super Mario Sunshine); its
   Triforce arcade Test and Coin buttons are advanced. The big A in the
   middle goes on Xbox A, B left of it on X, X right of it on B, Y above on
   Y. */
static const row k_gamecube_rows[] = {
    DPAD, { ME_IN_A, "A" }, { ME_IN_B, "B" }, { ME_IN_X, "X" }, { ME_IN_Y, "Y" },
    { ME_IN_LT, "L" }, { ME_IN_RT, "R" }, { ME_IN_RB, "Z" }, { ME_IN_START, "Start" },
    { ME_IN_LSTICK, "L (light press)" }, { ME_IN_RSTICK, "R (light press)" },
    { ME_IN_LSTICK_UP,   "Control Stick Up" },   { ME_IN_LSTICK_DOWN,  "Control Stick Down" },
    { ME_IN_LSTICK_LEFT, "Control Stick Left" }, { ME_IN_LSTICK_RIGHT, "Control Stick Right" },
    { ME_IN_RSTICK_UP,   "C-Stick Up" },   { ME_IN_RSTICK_DOWN,  "C-Stick Down" },
    { ME_IN_RSTICK_LEFT, "C-Stick Left" }, { ME_IN_RSTICK_RIGHT, "C-Stick Right" }, END
};
static const pad_default k_gamecube_defaults[] = {
    { ME_IN_A, { ME_XI_A }, 0 }, { ME_IN_B, { ME_XI_X }, 0 },
    { ME_IN_X, { ME_XI_B }, 0 }, { ME_IN_Y, { ME_XI_Y }, 0 }, DEFAULTS_END
};

/* The Wii's controllers as Dolphin lays them on the RetroPad (rom_cores.c
   lists them). A Wii Remote's tilt is the left stick and its pointer the
   right stick. Dolphin also turns a remote sideways or upright on L3 (its
   default hotkeys), so L3 is never passed. */
#define WII_TILT \
    { ME_IN_LSTICK_UP,   "Tilt Forward" }, { ME_IN_LSTICK_DOWN,  "Tilt Backward" }, \
    { ME_IN_LSTICK_LEFT, "Tilt Left" },    { ME_IN_LSTICK_RIGHT, "Tilt Right" }
#define WII_POINTER \
    { ME_IN_RSTICK_UP,   "Pointer Up" },   { ME_IN_RSTICK_DOWN,  "Pointer Down" }, \
    { ME_IN_RSTICK_LEFT, "Pointer Left" }, { ME_IN_RSTICK_RIGHT, "Pointer Right" }
static const row k_wii_remote_rows[] = {
    DPAD, { ME_IN_A, "A" }, { ME_IN_B, "B" }, { ME_IN_X, "1" }, { ME_IN_Y, "2" },
    { ME_IN_START, "+" }, { ME_IN_BACK, "-" }, { ME_IN_RSTICK, "Home" }, { ME_IN_RT, "Shake" },
    WII_TILT, WII_POINTER, END
};
/* A on Xbox A, the B trigger underneath on the right trigger (or RB), 1 and
   2 on X and Y, a shake on B. */
static const pad_default k_wii_remote_defaults[] = {
    { ME_IN_A, { ME_XI_A }, 0 }, { ME_IN_B, { ME_XI_RT, ME_XI_RB }, 0 },
    { ME_IN_X, { ME_XI_X }, 0 }, { ME_IN_Y, { ME_XI_Y }, 0 }, { ME_IN_RT, { ME_XI_B }, 0 },
    DEFAULTS_END
};
/* Held like an NES pad: Dolphin turns the D-pad with it, and 1 and 2 are
   the face buttons. */
static const row k_wii_sideways_rows[] = {
    DPAD, { ME_IN_B, "1" }, { ME_IN_A, "2" }, { ME_IN_X, "A" }, { ME_IN_Y, "B" },
    { ME_IN_START, "+" }, { ME_IN_BACK, "-" }, { ME_IN_RSTICK, "Home" }, { ME_IN_RT, "Shake" },
    WII_TILT, WII_POINTER, END
};
/* 2 (Mario Kart's accelerator, New Super Mario Bros.' jump) on Xbox A and 1
   left of it on X; A on Y, the B trigger on the right trigger (or RB), a
   shake on B. */
static const pad_default k_wii_sideways_defaults[] = {
    { ME_IN_A, { ME_XI_A }, 0 }, { ME_IN_B, { ME_XI_X }, 0 },
    { ME_IN_X, { ME_XI_Y }, 0 }, { ME_IN_Y, { ME_XI_RT, ME_XI_RB }, 0 }, { ME_IN_RT, { ME_XI_B }, 0 },
    DEFAULTS_END
};
static const row k_wii_nunchuk_rows[] = {
    DPAD, { ME_IN_A, "A" }, { ME_IN_B, "B" }, { ME_IN_X, "C" }, { ME_IN_Y, "Z" },
    { ME_IN_RB, "+" }, { ME_IN_LB, "-" }, { ME_IN_START, "1" }, { ME_IN_BACK, "2" },
    { ME_IN_RSTICK, "Home" }, { ME_IN_RT, "Shake Wii Remote" }, { ME_IN_LT, "Shake Nunchuk" },
    { ME_IN_LSTICK_UP,   "Nunchuk Stick Up" },   { ME_IN_LSTICK_DOWN,  "Nunchuk Stick Down" },
    { ME_IN_LSTICK_LEFT, "Nunchuk Stick Left" }, { ME_IN_LSTICK_RIGHT, "Nunchuk Stick Right" },
    WII_POINTER, END
};
/* The remote in the right hand (A on Xbox A, B trigger on the right
   trigger, a shake on B), the Nunchuk in the left (Z trigger on the left
   trigger, C on LB); + and - on Start and Back, 1 and 2 on X and Y, a
   Nunchuk shake on RB. */
static const pad_default k_wii_nunchuk_defaults[] = {
    { ME_IN_A, { ME_XI_A }, 0 },  { ME_IN_B, { ME_XI_RT }, 0 },
    { ME_IN_X, { ME_XI_LB }, 0 }, { ME_IN_Y, { ME_XI_LT }, 0 },
    { ME_IN_RB, { ME_XI_START }, VK_RETURN }, { ME_IN_LB, { ME_XI_BACK }, VK_RSHIFT },
    { ME_IN_START, { ME_XI_X }, 'Q' }, { ME_IN_BACK, { ME_XI_Y }, 'W' },
    { ME_IN_RT, { ME_XI_B }, 0 }, { ME_IN_LT, { ME_XI_RB }, 0 },
    DEFAULTS_END
};
/* The Classic Controller is laid out like the RetroPad (and a SNES pad), so
   universal fits it. L and R are its analog triggers, ZL and ZR the
   buttons above them; the Pro has them the other way up. */
#define WII_CLASSIC_STICKS \
    { ME_IN_LSTICK_UP,   "Left Stick Up" },    { ME_IN_LSTICK_DOWN,  "Left Stick Down" }, \
    { ME_IN_LSTICK_LEFT, "Left Stick Left" },  { ME_IN_LSTICK_RIGHT, "Left Stick Right" }, \
    { ME_IN_RSTICK_UP,   "Right Stick Up" },   { ME_IN_RSTICK_DOWN,  "Right Stick Down" }, \
    { ME_IN_RSTICK_LEFT, "Right Stick Left" }, { ME_IN_RSTICK_RIGHT, "Right Stick Right" }
static const row k_wii_classic_rows[] = {
    DPAD, { ME_IN_A, "A" }, { ME_IN_B, "B" }, { ME_IN_X, "X" }, { ME_IN_Y, "Y" },
    { ME_IN_LT, "L" }, { ME_IN_RT, "R" }, { ME_IN_LB, "ZL" }, { ME_IN_RB, "ZR" },
    { ME_IN_START, "+" }, { ME_IN_BACK, "-" }, { ME_IN_RSTICK, "Home" },
    WII_CLASSIC_STICKS, END
};
static const row k_wii_classic_pro_rows[] = {
    DPAD, { ME_IN_A, "A" }, { ME_IN_B, "B" }, { ME_IN_X, "X" }, { ME_IN_Y, "Y" },
    { ME_IN_LB, "L" }, { ME_IN_RB, "R" }, { ME_IN_LT, "ZL" }, { ME_IN_RT, "ZR" },
    { ME_IN_START, "+" }, { ME_IN_BACK, "-" }, { ME_IN_RSTICK, "Home" },
    WII_CLASSIC_STICKS, END
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
/* The other cores' layout, which the universal map gives them: A, B, C on
   Xbox X, A, B and X, Y, Z on LB, Y, RB. */
static const pad_default k_md_blastem_defaults[] = {
    { ME_IN_B, { ME_XI_X },  0 }, { ME_IN_A, { ME_XI_A }, 0 }, { ME_IN_RB, { ME_XI_B },  0 },
    { ME_IN_Y, { ME_XI_LB }, 0 }, { ME_IN_X, { ME_XI_Y }, 0 }, { ME_IN_LB, { ME_XI_RB }, 0 },
    DEFAULTS_END
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
/* One fire button: any face button. */
static const pad_default k_2600_defaults[] = {
    { ME_IN_B, { ME_XI_A, ME_XI_B, ME_XI_X, ME_XI_Y }, 0 }, DEFAULTS_END
};
static const row k_7800_rows[] = {
    DPAD, { ME_IN_B, "1" }, { ME_IN_A, "2" },
    { ME_IN_START, "Pause" }, { ME_IN_BACK, "Select" }, { ME_IN_X, "Reset" },
    { ME_IN_LB, "Left Difficulty" }, { ME_IN_RB, "Right Difficulty" }, END
};
/* Reset moves off Xbox Y (button 1 here) to the right stick click. */
static const pad_default k_7800_defaults[] = {
    TWO_BUTTONS(ME_IN_B, ME_IN_A), { ME_IN_X, { ME_XI_RSTICK }, 0 }, DEFAULTS_END
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
static const pad_default k_lynx_beetle_defaults[] = {
    TWO_BUTTONS(ME_IN_A, ME_IN_B), DEFAULTS_END
};

/* Atari800 as a 5200: its fire buttons where a5200 has them (Fire 1, the
   upper one, on Xbox B; Fire 2 on A). It lists its own inputs. */
static const pad_default k_5200_atari800_defaults[] = {
    { ME_IN_B, { ME_XI_B }, 0 }, { ME_IN_A, { ME_XI_A }, 0 }, DEFAULTS_END
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
/* The Y pad (the second D-pad) on the right stick. */
static const pad_default k_ws_defaults[] = {
    TWO_BUTTONS(ME_IN_B, ME_IN_A),
    { ME_IN_RT, { ME_XI_RSTICK_UP },   0 }, { ME_IN_RB, { ME_XI_RSTICK_RIGHT }, 0 },
    { ME_IN_LT, { ME_XI_RSTICK_DOWN }, 0 }, { ME_IN_LB, { ME_XI_RSTICK_LEFT },  0 },
    DEFAULTS_END
};

/* Sony. PPSSPP also describes a right stick the PSP doesn't have. */
static const row k_psp_rows[] = {
    DPAD, { ME_IN_B, "Cross" }, { ME_IN_A, "Circle" }, { ME_IN_Y, "Square" }, { ME_IN_X, "Triangle" },
    { ME_IN_LB, "L" }, { ME_IN_RB, "R" },
    { ME_IN_BACK, "Select" }, { ME_IN_START, "Start" },
    { ME_IN_LSTICK_UP,   "Analog Up" },   { ME_IN_LSTICK_DOWN,  "Analog Down" },
    { ME_IN_LSTICK_LEFT, "Analog Left" }, { ME_IN_LSTICK_RIGHT, "Analog Right" }, END
};

/* Others; these cores list their own inputs. blueMSX as a ColecoVision:
   the fire buttons and keypad 1 and 2 where Gearcoleco has them (left fire
   on Xbox A, right fire on B, 1 on X, 2 on Y). */
static const pad_default k_coleco_bluemsx_defaults[] = {
    { ME_IN_A, { ME_XI_A }, 0 }, { ME_IN_B, { ME_XI_B }, 0 },
    { ME_IN_X, { ME_XI_X }, 0 }, { ME_IN_Y, { ME_XI_Y }, 0 }, DEFAULTS_END
};
/* FreeIntv: the Intellivision's left and right side buttons on Xbox X and
   B, its top buttons on Y; A repeats the last keypad button. */
static const pad_default k_intv_defaults[] = {
    { ME_IN_A, { ME_XI_X }, 0 }, { ME_IN_B, { ME_XI_B }, 0 },
    { ME_IN_Y, { ME_XI_Y }, 0 }, { ME_IN_X, { ME_XI_A }, 0 }, DEFAULTS_END
};
/* VecX: the Vectrex's four buttons in a row, 1 to 4, on Xbox X, A, B, Y. */
static const pad_default k_vectrex_defaults[] = {
    { ME_IN_A, { ME_XI_X }, 0 }, { ME_IN_B, { ME_XI_A }, 0 },
    { ME_IN_X, { ME_XI_B }, 0 }, { ME_IN_Y, { ME_XI_Y }, 0 }, DEFAULTS_END
};

/* Keys are rom_cores.h console ids, with a suffix where a core lays the
   controller out differently from the console's other cores. */
static const console k_nes      = { "nes",      "NES",               k_nes_rows,      k_two_buttons };
static const console k_snes     = { "snes",     "SNES",              k_snes_rows,     NULL };
static const console k_gb       = { "gb",       "Game Boy",          k_nes_rows,      k_two_buttons };
static const console k_gbc      = { "gbc",      "Game Boy Color",    k_nes_rows,      k_two_buttons };
static const console k_gba      = { "gba",      "Game Boy Advance",  k_gba_rows,      k_two_buttons };
static const console k_nds      = { "nds",      "Nintendo DS",       k_snes_rows,     NULL };
static const console k_n64      = { "n64",      "Nintendo 64",       k_n64_rows,      k_n64_defaults };
static const console k_gamecube = { "gamecube", "GameCube",          k_gamecube_rows, k_gamecube_defaults };
static const console k_wii_remote   = { "wii_remote",   "Wii Remote",            k_wii_remote_rows,
                                        k_wii_remote_defaults };
static const console k_wii_sideways = { "wii_sideways", "Wii Remote (sideways)", k_wii_sideways_rows,
                                        k_wii_sideways_defaults };
static const console k_wii_nunchuk  = { "wii_nunchuk",  "Wii Remote + Nunchuk",  k_wii_nunchuk_rows,
                                        k_wii_nunchuk_defaults };
static const console k_wii_classic  = { "wii_classic",  "Classic Controller",    k_wii_classic_rows, NULL };
static const console k_wii_classic_pro = { "wii_classic_pro", "Classic Controller Pro",
                                           k_wii_classic_pro_rows, NULL };

/* The controllers a player can pick for a console's games (rom_cores.h
   me_controller), by key, and whose games the dialogs say their maps are
   for. */
static const struct {
    const console *c;
    const char    *games;
} k_controllers[] = {
    { &k_gamecube, "GameCube / Wii" },
    { &k_wii_remote, "Wii" }, { &k_wii_sideways, "Wii" }, { &k_wii_nunchuk, "Wii" },
    { &k_wii_classic, "Wii" }, { &k_wii_classic_pro, "Wii" },
};
static const console k_vb       = { "vb",       "Virtual Boy",       k_vb_rows,       k_vb_defaults };
static const console k_pokemini = { "pokemini", "Pokemon Mini",      k_pokemini_rows, k_two_buttons };
static const console k_md       = { "md",       "Mega Drive / Genesis", k_md_rows,    NULL };
static const console k_md_blastem = { "md_blastem", "Mega Drive / Genesis", k_md_blastem_rows,
                                      k_md_blastem_defaults };
static const console k_sms      = { "sms",      "Master System",     k_sms_rows,      k_two_buttons };
static const console k_sg1000   = { "sg1000",   "SG-1000",           k_sms_rows,      k_two_buttons };
static const console k_gg       = { "gg",       "Game Gear",         k_gg_rows,       k_two_buttons };
static const console k_pce      = { "pce",      "PC Engine",         k_pce_rows,      k_two_buttons };
static const console k_sgx      = { "sgx",      "SuperGrafx",        k_pce_rows,      k_two_buttons };
static const console k_2600     = { "a2600",    "Atari 2600",        k_2600_rows,     k_2600_defaults };
static const console k_5200_atari800 = { "a5200_atari800", "Atari 5200", NULL,    k_5200_atari800_defaults };
static const console k_7800     = { "a7800",    "Atari 7800",        k_7800_rows,     k_7800_defaults };
static const console k_lynx     = { "lynx",     "Atari Lynx",        k_lynx_rows,     k_two_buttons };
static const console k_lynx_beetle = { "lynx_beetle", "Atari Lynx",  k_lynx_beetle_rows,
                                       k_lynx_beetle_defaults };
static const console k_ngp      = { "ngp",      "Neo Geo Pocket",    k_ngp_rows,      k_two_buttons };
static const console k_ngpc     = { "ngpc",     "Neo Geo Pocket Color", k_ngp_rows,   k_two_buttons };
static const console k_ws       = { "ws",       "WonderSwan",        k_ws_rows,       k_ws_defaults };
static const console k_wsc      = { "wsc",      "WonderSwan Color",  k_ws_rows,       k_ws_defaults };
static const console k_psp      = { "psp",      "PSP",               k_psp_rows,      NULL };
static const console k_coleco_bluemsx = { "coleco_bluemsx", "ColecoVision", NULL, k_coleco_bluemsx_defaults };
static const console k_intv     = { "intv",     "Intellivision",     NULL,            k_intv_defaults };
static const console k_vectrex  = { "vectrex",  "Vectrex",           NULL,            k_vectrex_defaults };

/* First match wins. `cores`: '|'-separated library_name prefixes, NULL = any
   core. `exts`: '|'-separated ROM extensions, NULL = any. A NULL console
   means no curated layout (the core's descriptors are used), and neither
   does a console without rows, which only brings default bindings. Games
   no rule matches get the default bindings of no console: universal's. */
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
    { "ParaLLEl N64|Mupen64Plus-Next",      NULL,        &k_n64 },
    { "dolphin",                            NULL,        &k_gamecube },
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
    { "Atari800",                           "a52",       &k_5200_atari800 },
    { "ProSystem",                          NULL,        &k_7800 },
    { "Handy|Gearlynx",                     NULL,        &k_lynx },
    { "Beetle Lynx",                        NULL,        &k_lynx_beetle },

    /* SNK, Bandai, Sony, others. */
    { "Beetle NeoPop|RACE",                 "ngc|ngpc",  &k_ngpc },
    { "Beetle NeoPop|RACE",                 NULL,        &k_ngp },
    { "Beetle WonderSwan",                  "wsc",       &k_wsc },
    { "Beetle WonderSwan",                  NULL,        &k_ws },
    { "PPSSPP",                             NULL,        &k_psp },
    { "blueMSX",                            "col|cv",    &k_coleco_bluemsx },
    { "freeintv",                           NULL,        &k_intv },
    { "VecX",                               NULL,        &k_vectrex },
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

int me_lstick_as_dpad(const me_settings *s, const me_input_layout *l, int with_console,
                      int player) {
    int ci = with_console ? me_settings_find_console_controls(s, l->key) : -1;
    if (ci >= 0 && s->console_controls[ci].lstick_as_dpad[player] >= 0)
        return s->console_controls[ci].lstick_as_dpad[player];
    return l->stick ? 0 : s->lstick_as_dpad[player];
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

static void set_defaults(me_input_layout *l, const pad_default *d) {
    for (; d && d->id < ME_IN_COUNT; d++) {
        me_xi_bindings *xi = &l->def_xis[d->id];
        memset(xi, 0, sizeof(*xi));
        for (int i = 0; i < ME_XI_MAX_BINDINGS && d->xi[i]; i++) xi->b[xi->count++].buttons = d->xi[i];
        if (xi->count) l->def_xi |= 1u << d->id;
        if (d->vk) {
            me_kb_bindings *kb = &l->def_kbs[d->id];
            memset(kb, 0, sizeof(*kb));
            kb->b[0].vk = d->vk;
            kb->count = 1;
            l->def_kb |= 1u << d->id;
        }
    }
}

/* Give `out` (every input, so far) console `c`'s key, defaults and, if it
   lists them, its rows. `games`: whose games the dialogs say the map is
   for, NULL for `c`'s. Returns 1 for a curated layout. */
static int apply_console(const console *c, const char *games, me_input_layout *out) {
    snprintf(out->key, sizeof(out->key), "%s", c->key);
    snprintf(out->console, sizeof(out->console), "%s", games ? games : c->name);
    set_defaults(out, c->defaults);
    if (!c->rows) return 0;
    out->curated = 1;
    snprintf(out->name, sizeof(out->name), "%s", c->name);
    out->n = 0;
    out->live = out->advanced = 0;
    memset(out->labels, 0, sizeof(out->labels));
    for (const row *r = c->rows; r->label; r++) add_row(out, r->id, r->label, 0);
    return 1;
}

int me_layout_for_game(const char *library_name, const char *rom_path,
                       const char *console_id, const char *console_name, int stick,
                       me_input_layout *out) {
    const char *lib = library_name ? library_name : "";
    const char *ext = extension_of(rom_path ? rom_path : "");
    me_layout_unknown(out);
    out->stick = stick;
    snprintf(out->key, sizeof(out->key), "%s", console_id ? console_id : "");
    snprintf(out->console, sizeof(out->console), "%s", console_name ? console_name : "");
    for (size_t i = 0; i < sizeof(k_rules) / sizeof(k_rules[0]); i++) {
        if (k_rules[i].cores && !in_list(k_rules[i].cores, lib, 1)) continue;
        if (k_rules[i].exts  && !in_list(k_rules[i].exts,  ext, 0)) continue;
        return k_rules[i].c ? apply_console(k_rules[i].c, NULL, out) : 0;
    }
    return 0;
}

int me_layout_for_controller(const char *key, int stick, me_input_layout *out) {
    for (size_t i = 0; key && i < sizeof(k_controllers) / sizeof(k_controllers[0]); i++) {
        if (_stricmp(k_controllers[i].c->key, key) != 0) continue;
        me_layout_unknown(out);
        out->stick = stick;
        return apply_console(k_controllers[i].c, k_controllers[i].games, out);
    }
    return 0;
}

/* Copy what `src` knows about the console (not its rows) into `dst`. */
static void keep_console(me_input_layout *dst, const me_input_layout *src) {
    memcpy(dst->key, src->key, sizeof(dst->key));
    memcpy(dst->console, src->console, sizeof(dst->console));
    dst->stick  = src->stick;
    dst->def_kb = src->def_kb;
    dst->def_xi = src->def_xi;
    memcpy(dst->def_kbs, src->def_kbs, sizeof(dst->def_kbs));
    memcpy(dst->def_xis, src->def_xis, sizeof(dst->def_xis));
}

void me_controls_effective(const me_settings *s, const me_input_layout *l, int core,
                           int with_console, int player, me_control_map *out) {
    *out = s->universal[player];
    for (int id = 0; id < ME_IN_COUNT; id++) {
        if (l->def_kb & (1u << id)) out->keys[id] = l->def_kbs[id];
        if (l->def_xi & (1u << id)) out->xi[id]   = l->def_xis[id];
    }
    const me_control_map *over[2] = { NULL, NULL };
    unsigned bits[2] = { 0, 0 };
    int ci = with_console ? me_settings_find_console_controls(s, l->key) : -1;
    if (ci >= 0) {
        over[0] = &s->console_controls[ci].controls[player];
        bits[0] = s->console_controls[ci].overrides[player];
    }
    if (core >= 0 && (size_t)core < s->cores_n && !s->cores[core].use_universal) {
        over[1] = &s->cores[core].controls[player];
        bits[1] = s->cores[core].overrides[player];
    }
    for (int k = 0; k < 2; k++) {
        for (int id = 0; id < ME_IN_COUNT && over[k]; id++) {
            if (!(bits[k] & (1u << id))) continue;
            out->keys[id] = over[k]->keys[id];
            out->xi[id]   = over[k]->xi[id];
        }
    }
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
    me_input_layout was = *out;
    memset(out, 0, sizeof(*out));
    add_described(out, d, banned, 0);
    if (out->n == 0) me_layout_unknown(out);
    else snprintf(out->name, sizeof(out->name), "%s", core_name ? core_name : "");
    /* Only banned inputs are held back: a core may read inputs it never
       described (analog sticks especially), and those must keep working. */
    out->live = ME_IN_ALL & ~banned;
    keep_console(out, &was);
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
