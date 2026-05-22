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

/* Returns 1 if any of the `buttons` bitmask bits are set in the last poll
   for `player_index`. */
int  me_xinput_button(int player_index, unsigned buttons);

/* Returns the axis value in [-32767..32767] (or [0..32767] for triggers)
   from the last poll for `player_index`. */
int16_t me_xinput_axis(int player_index, me_xi_axis axis);

/* Returns 1 if the controller at `player_index` is connected. */
int  me_xinput_connected(int player_index);

#endif
