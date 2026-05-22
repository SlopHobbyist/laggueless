#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "xinput_pad.h"

/* ---- XInput types (avoid linking against xinput.h / xinput.lib) ----------- */
typedef struct {
    uint16_t wButtons;
    uint8_t  bLeftTrigger;
    uint8_t  bRightTrigger;
    int16_t  sThumbLX;
    int16_t  sThumbLY;
    int16_t  sThumbRX;
    int16_t  sThumbRY;
} ME_XINPUT_GAMEPAD;

typedef struct {
    uint32_t          dwPacketNumber;
    ME_XINPUT_GAMEPAD Gamepad;
} ME_XINPUT_STATE;

typedef DWORD (WINAPI *PFN_XInputGetState)(DWORD dwUserIndex, ME_XINPUT_STATE *pState);

/* ---- module state --------------------------------------------------------- */
#define ME_XI_MAX_PLAYERS 4

static PFN_XInputGetState g_xi_GetState = NULL;
static HMODULE            g_xi_dll      = NULL;
static int                g_xi_loaded   = 0;  /* -1=failed, 0=not tried, 1=ok */

static ME_XINPUT_STATE g_xi_state[ME_XI_MAX_PLAYERS];
static int             g_xi_connected[ME_XI_MAX_PLAYERS];

/* Default deadzone: ~24% of full scale, matching the XInput recommended value. */
#define ME_XI_DEADZONE_STICK    7849
#define ME_XI_DEADZONE_TRIGGER  30

static int16_t apply_deadzone(int16_t v, int16_t dead) {
    if (v >  dead) return v;
    if (v < -dead) return v;
    return 0;
}

/* Scale trigger byte [0..255] → [0..32767]. */
static int16_t trigger_to_axis(uint8_t t) {
    return (t > ME_XI_DEADZONE_TRIGGER) ? (int16_t)((int)t * 32767 / 255) : 0;
}

/* ---- public API ----------------------------------------------------------- */

int me_xinput_init(void) {
    if (g_xi_loaded != 0) return g_xi_loaded > 0;

    static const char *const dlls[] = {
        "xinput1_4.dll",
        "xinput1_3.dll",
        "xinput9_1_0.dll",
        NULL
    };
    for (const char *const *d = dlls; *d; d++) {
        g_xi_dll = LoadLibraryA(*d);
        if (g_xi_dll) break;
    }
    if (!g_xi_dll) { g_xi_loaded = -1; return 0; }

    g_xi_GetState = (PFN_XInputGetState)GetProcAddress(g_xi_dll, "XInputGetState");
    if (!g_xi_GetState) {
        FreeLibrary(g_xi_dll);
        g_xi_dll = NULL;
        g_xi_loaded = -1;
        return 0;
    }

    memset(g_xi_state,     0, sizeof(g_xi_state));
    memset(g_xi_connected, 0, sizeof(g_xi_connected));
    g_xi_loaded = 1;
    return 1;
}

void me_xinput_poll(int player_index) {
    if (g_xi_loaded != 1) return;
    if (player_index < 0 || player_index >= ME_XI_MAX_PLAYERS) return;

    ME_XINPUT_STATE s;
    DWORD r = g_xi_GetState((DWORD)player_index, &s);
    if (r == 0 /* ERROR_SUCCESS */) {
        g_xi_connected[player_index] = 1;
        g_xi_state[player_index] = s;
    } else {
        g_xi_connected[player_index] = 0;
        memset(&g_xi_state[player_index], 0, sizeof(g_xi_state[player_index]));
    }
}

int me_xinput_button(int player_index, unsigned buttons) {
    if (!g_xi_connected[player_index]) return 0;
    return (g_xi_state[player_index].Gamepad.wButtons & (uint16_t)buttons) ? 1 : 0;
}

int16_t me_xinput_axis(int player_index, me_xi_axis axis) {
    if (!g_xi_connected[player_index]) return 0;
    const ME_XINPUT_GAMEPAD *gp = &g_xi_state[player_index].Gamepad;
    switch (axis) {
        case ME_XI_AXIS_LX: return apply_deadzone(gp->sThumbLX, ME_XI_DEADZONE_STICK);
        case ME_XI_AXIS_LY: return apply_deadzone(gp->sThumbLY, ME_XI_DEADZONE_STICK);
        case ME_XI_AXIS_RX: return apply_deadzone(gp->sThumbRX, ME_XI_DEADZONE_STICK);
        case ME_XI_AXIS_RY: return apply_deadzone(gp->sThumbRY, ME_XI_DEADZONE_STICK);
        case ME_XI_AXIS_LT: return trigger_to_axis(gp->bLeftTrigger);
        case ME_XI_AXIS_RT: return trigger_to_axis(gp->bRightTrigger);
        default: return 0;
    }
}

int me_xinput_connected(int player_index) {
    if (player_index < 0 || player_index >= ME_XI_MAX_PLAYERS) return 0;
    return g_xi_connected[player_index];
}
