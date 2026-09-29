#ifndef ME_XINPUT_PAD_H
#define ME_XINPUT_PAD_H

#include <stdint.h>

/* XInput button bitmasks (XINPUT_GAMEPAD_* values). */
#define ME_XI_DPAD_UP        0x0001
#define ME_XI_DPAD_DOWN      0x0002
#define ME_XI_DPAD_LEFT      0x0004
#define ME_XI_DPAD_RIGHT     0x0008
#define ME_XI_START          0x0010
#define ME_XI_BACK           0x0020
#define ME_XI_LSTICK         0x0040
#define ME_XI_RSTICK         0x0080
#define ME_XI_LB             0x0100
#define ME_XI_RB             0x0200
#define ME_XI_A              0x1000
#define ME_XI_B              0x2000
#define ME_XI_X              0x4000
#define ME_XI_Y              0x8000

/* Pseudo-buttons above XInput's 16 real ones: triggers pulled past the
   trigger threshold and sticks pushed past half travel read as digital
   buttons, so they can be bound like any other button. */
#define ME_XI_LT             0x00010000u
#define ME_XI_RT             0x00020000u
#define ME_XI_LSTICK_UP      0x00040000u
#define ME_XI_LSTICK_DOWN    0x00080000u
#define ME_XI_LSTICK_LEFT    0x00100000u
#define ME_XI_LSTICK_RIGHT   0x00200000u
#define ME_XI_RSTICK_UP      0x00400000u
#define ME_XI_RSTICK_DOWN    0x00800000u
#define ME_XI_RSTICK_LEFT    0x01000000u
#define ME_XI_RSTICK_RIGHT   0x02000000u

#define ME_XI_SLOTS 4  /* XInput user indices 0..3 */

/* Axis IDs for me_xinput_axis(). */
typedef enum {
    ME_XI_AXIS_LX = 0,
    ME_XI_AXIS_LY,
    ME_XI_AXIS_RX,
    ME_XI_AXIS_RY,
    ME_XI_AXIS_LT,   /* 0..255, returned as 0..32767 */
    ME_XI_AXIS_RT,
} me_xi_axis;

/* Load xinput1_4.dll (falls back to xinput9_1_0.dll). Returns 1 on success.
   Safe to call multiple times; only loads once. */
int  me_xinput_init(void);

/* Poll the physical pad at `player_index` (0-based). Call once per frame
   before querying buttons/axes. No-op if XInput failed to load. */
void me_xinput_poll(int player_index);

/* Returns 1 if all of the `buttons` bitmask bits (real or pseudo) are held
   in the last poll for `player_index`. */
int  me_xinput_button(int player_index, unsigned buttons);

/* Returns the axis value in [-32767..32767] (or [0..32767] for triggers)
   from the last poll for `player_index`. */
int16_t me_xinput_axis(int player_index, me_xi_axis axis);

/* Returns 1 if the controller at `player_index` is connected. */
int  me_xinput_connected(int player_index);

/* Read a slot directly, bypassing the per-frame poll state. Returns the
   ME_XI_* mask (real + pseudo buttons) currently held; *connected (optional)
   receives whether the pad answered. Safe to call from any thread — used by
   the UI to capture bindings and list connected controllers while the
   emulation thread keeps polling. */
unsigned me_xinput_read(int slot, int *connected);

#endif
