#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <stdint.h>
#include <stdio.h>
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

typedef struct {
    uint16_t wLeftMotorSpeed;    /* low-frequency, strong */
    uint16_t wRightMotorSpeed;   /* high-frequency, weak */
} ME_XINPUT_VIBRATION;

typedef DWORD (WINAPI *PFN_XInputGetState)(DWORD dwUserIndex, ME_XINPUT_STATE *pState);
typedef DWORD (WINAPI *PFN_XInputSetState)(DWORD dwUserIndex, ME_XINPUT_VIBRATION *pVibration);

/* ---- SDL2 types (SDL2.dll is loaded at runtime; no headers or import lib) -- */
typedef struct SDL_GameController SDL_GameController;
typedef struct SDL_RWops SDL_RWops;
typedef int32_t SDL_JoystickID;

#define ME_SDL_INIT_GAMECONTROLLER 0x00002000u
#define ME_SDL_IGNORE              0

/* SDL_GameControllerButton / SDL_GameControllerAxis values. */
enum {
    ME_SDL_BUTTON_A, ME_SDL_BUTTON_B, ME_SDL_BUTTON_X, ME_SDL_BUTTON_Y,
    ME_SDL_BUTTON_BACK, ME_SDL_BUTTON_GUIDE, ME_SDL_BUTTON_START,
    ME_SDL_BUTTON_LEFTSTICK, ME_SDL_BUTTON_RIGHTSTICK,
    ME_SDL_BUTTON_LEFTSHOULDER, ME_SDL_BUTTON_RIGHTSHOULDER,
    ME_SDL_BUTTON_DPAD_UP, ME_SDL_BUTTON_DPAD_DOWN,
    ME_SDL_BUTTON_DPAD_LEFT, ME_SDL_BUTTON_DPAD_RIGHT,
};
enum {
    ME_SDL_AXIS_LEFTX, ME_SDL_AXIS_LEFTY, ME_SDL_AXIS_RIGHTX, ME_SDL_AXIS_RIGHTY,
    ME_SDL_AXIS_TRIGGERLEFT, ME_SDL_AXIS_TRIGGERRIGHT,
};

static struct {
    int                 (__cdecl *SetHint)(const char *name, const char *value);
    int                 (__cdecl *Init)(uint32_t flags);
    int                 (__cdecl *JoystickEventState)(int state);
    int                 (__cdecl *GameControllerEventState)(int state);
    SDL_RWops          *(__cdecl *RWFromFile)(const char *file, const char *mode);
    int                 (__cdecl *GameControllerAddMappingsFromRW)(SDL_RWops *rw, int freerw);
    int                 (__cdecl *NumJoysticks)(void);
    int                 (__cdecl *IsGameController)(int device_index);
    const char         *(__cdecl *JoystickPathForIndex)(int device_index);
    SDL_JoystickID      (__cdecl *JoystickGetDeviceInstanceID)(int device_index);
    SDL_GameController *(__cdecl *GameControllerOpen)(int device_index);
    void                (__cdecl *GameControllerClose)(SDL_GameController *gc);
    int                 (__cdecl *GameControllerGetAttached)(SDL_GameController *gc);
    void                (__cdecl *GameControllerUpdate)(void);
    uint8_t             (__cdecl *GameControllerGetButton)(SDL_GameController *gc, int button);
    int16_t             (__cdecl *GameControllerGetAxis)(SDL_GameController *gc, int axis);
    int                 (__cdecl *GameControllerRumble)(SDL_GameController *gc, uint16_t low,
                                                        uint16_t high, uint32_t duration_ms);
    const char         *(__cdecl *GameControllerName)(SDL_GameController *gc);
    void                (__cdecl *GameControllerSetPlayerIndex)(SDL_GameController *gc, int index);
} g_sdl;

/* ---- module state --------------------------------------------------------- */
#define ME_XI_XINPUT_SLOTS 4  /* slots XInput can fill */

static PFN_XInputGetState g_xi_GetState = NULL;
static PFN_XInputSetState g_xi_SetState = NULL;
static HMODULE            g_xi_dll      = NULL;
static int                g_xi_loaded   = 0;  /* -1=failed, 0=not tried, 1=ok */

static ME_XINPUT_STATE g_xi_state[ME_XI_SLOTS];
static int             g_xi_connected[ME_XI_SLOTS];
static unsigned        g_xi_buttons[ME_XI_SLOTS];   /* real + pseudo */
static DWORD           g_xi_next_probe[ME_XI_SLOTS];

/* SDL pads, by slot. The emulation thread polls them and the UI thread reads
   them, so the table and every SDL call are under g_sdl_lock. */
static int                g_sdl_ok;
static CRITICAL_SECTION   g_sdl_lock;
static SDL_GameController *g_sdl_pad[ME_XI_SLOTS];
static SDL_JoystickID     g_sdl_id[ME_XI_SLOTS];

/* XInputGetState on an empty slot is slow (it goes looking for the device),
   so a disconnected slot is only re-probed this often. */
#define ME_XI_PROBE_INTERVAL_MS 1000

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

/* Stick past half travel counts as a digital direction. */
#define ME_XI_STICK_DIGITAL 16384

static unsigned buttons_from_state(const ME_XINPUT_GAMEPAD *gp) {
    unsigned m = gp->wButtons;
    if (gp->bLeftTrigger  > ME_XI_DEADZONE_TRIGGER) m |= ME_XI_LT;
    if (gp->bRightTrigger > ME_XI_DEADZONE_TRIGGER) m |= ME_XI_RT;
    if (gp->sThumbLY >  ME_XI_STICK_DIGITAL) m |= ME_XI_LSTICK_UP;
    if (gp->sThumbLY < -ME_XI_STICK_DIGITAL) m |= ME_XI_LSTICK_DOWN;
    if (gp->sThumbLX < -ME_XI_STICK_DIGITAL) m |= ME_XI_LSTICK_LEFT;
    if (gp->sThumbLX >  ME_XI_STICK_DIGITAL) m |= ME_XI_LSTICK_RIGHT;
    if (gp->sThumbRY >  ME_XI_STICK_DIGITAL) m |= ME_XI_RSTICK_UP;
    if (gp->sThumbRY < -ME_XI_STICK_DIGITAL) m |= ME_XI_RSTICK_DOWN;
    if (gp->sThumbRX < -ME_XI_STICK_DIGITAL) m |= ME_XI_RSTICK_LEFT;
    if (gp->sThumbRX >  ME_XI_STICK_DIGITAL) m |= ME_XI_RSTICK_RIGHT;
    return m;
}

/* The XInput pad at `slot`, if one answers. */
static int xi_get(int slot, ME_XINPUT_GAMEPAD *gp) {
    if (g_xi_loaded != 1 || slot >= ME_XI_XINPUT_SLOTS) return 0;
    ME_XINPUT_STATE s;
    if (g_xi_GetState((DWORD)slot, &s) != 0 /* ERROR_SUCCESS */) return 0;
    *gp = s.Gamepad;
    return 1;
}

/* ---- SDL pads ------------------------------------------------------------- */
static int sdl_load(void) {
    HMODULE dll = LoadLibraryA("SDL2.dll");
    if (!dll) {
        printf("[pad] SDL2.dll not found; only XInput controllers work\n");
        return 0;
    }
    struct { void **fn; const char *name; } fns[] = {
        { (void **)&g_sdl.SetHint,                         "SDL_SetHint" },
        { (void **)&g_sdl.Init,                            "SDL_Init" },
        { (void **)&g_sdl.JoystickEventState,              "SDL_JoystickEventState" },
        { (void **)&g_sdl.GameControllerEventState,        "SDL_GameControllerEventState" },
        { (void **)&g_sdl.RWFromFile,                      "SDL_RWFromFile" },
        { (void **)&g_sdl.GameControllerAddMappingsFromRW, "SDL_GameControllerAddMappingsFromRW" },
        { (void **)&g_sdl.NumJoysticks,                    "SDL_NumJoysticks" },
        { (void **)&g_sdl.IsGameController,                "SDL_IsGameController" },
        { (void **)&g_sdl.JoystickPathForIndex,            "SDL_JoystickPathForIndex" },
        { (void **)&g_sdl.JoystickGetDeviceInstanceID,     "SDL_JoystickGetDeviceInstanceID" },
        { (void **)&g_sdl.GameControllerOpen,              "SDL_GameControllerOpen" },
        { (void **)&g_sdl.GameControllerClose,             "SDL_GameControllerClose" },
        { (void **)&g_sdl.GameControllerGetAttached,       "SDL_GameControllerGetAttached" },
        { (void **)&g_sdl.GameControllerUpdate,            "SDL_GameControllerUpdate" },
        { (void **)&g_sdl.GameControllerGetButton,         "SDL_GameControllerGetButton" },
        { (void **)&g_sdl.GameControllerGetAxis,           "SDL_GameControllerGetAxis" },
        { (void **)&g_sdl.GameControllerRumble,            "SDL_GameControllerRumble" },
        { (void **)&g_sdl.GameControllerName,              "SDL_GameControllerName" },
        { (void **)&g_sdl.GameControllerSetPlayerIndex,    "SDL_GameControllerSetPlayerIndex" },
    };
    for (size_t i = 0; i < sizeof(fns) / sizeof(fns[0]); i++) {
        /* SDL_JoystickPathForIndex is SDL 2.24+; without it XInput pads
           can't be told apart and would show up twice. */
        *fns[i].fn = (void *)GetProcAddress(dll, fns[i].name);
        if (!*fns[i].fn) {
            printf("[pad] SDL2.dll is too old (no %s); only XInput controllers work\n", fns[i].name);
            FreeLibrary(dll);
            return 0;
        }
    }

    /* XInput pads stay on the XInput path above; SDL keeps its XInput driver
       only so its DirectInput driver skips them, and they're filtered out by
       path in sdl_scan. RawInput and Windows.Gaming.Input would report them
       again under other names. */
    g_sdl.SetHint("SDL_JOYSTICK_RAWINPUT", "0");
    g_sdl.SetHint("SDL_JOYSTICK_WGI", "0");
    g_sdl.SetHint("SDL_JOYSTICK_HIDAPI_XBOX", "0");
    /* Positional buttons: the bottom face button is A on every pad, as it is
       in XInput mode, so an 8BitDo's mode switch doesn't swap A/B and X/Y. */
    g_sdl.SetHint("SDL_GAMECONTROLLER_USE_BUTTON_LABELS", "0");
    /* Read pads with no SDL window focused (we have none), like XInput. */
    g_sdl.SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
    /* Device arrival on SDL's own thread rather than a window on ours. */
    g_sdl.SetHint("SDL_JOYSTICK_THREAD", "1");
    /* Leave Ctrl+C in the console to us. */
    g_sdl.SetHint("SDL_NO_SIGNAL_HANDLERS", "1");

    /* SDL's DirectInput would make this thread's COM single-threaded; WASAPI
       (audio_wasapi.c) wants it multithreaded, and SDL accepts that. */
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (g_sdl.Init(ME_SDL_INIT_GAMECONTROLLER) != 0) {
        printf("[pad] SDL2 failed to start; only XInput controllers work\n");
        return 0;
    }
    /* Polled, not evented: nothing reads SDL's event queue. */
    g_sdl.JoystickEventState(ME_SDL_IGNORE);
    g_sdl.GameControllerEventState(ME_SDL_IGNORE);

    /* Community mappings for pads SDL doesn't know, if the player has
       dropped gamecontrollerdb.txt next to the exe. */
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    char *slash = n && n < MAX_PATH ? strrchr(path, '\\') : NULL;
    if (slash && (size_t)(slash + 1 - path) + sizeof("gamecontrollerdb.txt") <= MAX_PATH) {
        strcpy(slash + 1, "gamecontrollerdb.txt");
        SDL_RWops *rw = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES
                      ? g_sdl.RWFromFile(path, "rb") : NULL;
        int added = rw ? g_sdl.GameControllerAddMappingsFromRW(rw, 1) : -1;
        if (added >= 0) printf("[pad] %d controller mappings from gamecontrollerdb.txt\n", added);
    }
    return 1;
}

static int sdl_slot_of(SDL_JoystickID id) {
    for (int s = 0; s < ME_XI_SLOTS; s++)
        if (g_sdl_pad[s] && g_sdl_id[s] == id) return s;
    return -1;
}

/* The lowest slot with neither an SDL pad nor an XInput pad in it. Probing
   XInput here is slow, but only happens when a pad is plugged in. */
static int sdl_free_slot(void) {
    for (int s = 0; s < ME_XI_SLOTS; s++) {
        ME_XINPUT_GAMEPAD gp;
        if (g_sdl_pad[s]) continue;
        if (s < ME_XI_XINPUT_SLOTS && (g_xi_connected[s] || xi_get(s, &gp))) continue;
        return s;
    }
    return -1;
}

/* Give each newly plugged-in, non-XInput pad a slot. Lock held. */
static void sdl_scan(void) {
    int n = g_sdl.NumJoysticks();
    for (int i = 0; i < n; i++) {
        SDL_JoystickID id = g_sdl.JoystickGetDeviceInstanceID(i);
        if (id < 0 || sdl_slot_of(id) >= 0 || !g_sdl.IsGameController(i)) continue;
        const char *path = g_sdl.JoystickPathForIndex(i);
        if (path && (!strncmp(path, "XInput#", 7) || strstr(path, "IG_"))) continue;
        int s = sdl_free_slot();
        if (s < 0) return;
        SDL_GameController *gc = g_sdl.GameControllerOpen(i);
        if (!gc) continue;
        g_sdl_pad[s] = gc;
        g_sdl_id[s] = id;
        g_sdl.GameControllerSetPlayerIndex(gc, s);   /* player LEDs */
        const char *name = g_sdl.GameControllerName(gc);
        printf("[pad] %s connected as controller %d\n", name ? name : "controller", s + 1);
    }
}

/* Fresh state from SDL; drop pads that were unplugged. Lock held. */
static void sdl_update(int scan) {
    g_sdl.GameControllerUpdate();
    for (int s = 0; s < ME_XI_SLOTS; s++) {
        if (!g_sdl_pad[s] || g_sdl.GameControllerGetAttached(g_sdl_pad[s])) continue;
        g_sdl.GameControllerClose(g_sdl_pad[s]);
        g_sdl_pad[s] = NULL;
        printf("[pad] controller %d disconnected\n", s + 1);
    }
    if (scan) sdl_scan();
}

/* An SDL pad's state, in XInput's form. Lock held. */
static void sdl_state(SDL_GameController *gc, ME_XINPUT_GAMEPAD *gp) {
    static const struct { int button; uint16_t mask; } map[] = {
        { ME_SDL_BUTTON_DPAD_UP,       ME_XI_DPAD_UP },
        { ME_SDL_BUTTON_DPAD_DOWN,     ME_XI_DPAD_DOWN },
        { ME_SDL_BUTTON_DPAD_LEFT,     ME_XI_DPAD_LEFT },
        { ME_SDL_BUTTON_DPAD_RIGHT,    ME_XI_DPAD_RIGHT },
        { ME_SDL_BUTTON_START,         ME_XI_START },
        { ME_SDL_BUTTON_BACK,          ME_XI_BACK },
        { ME_SDL_BUTTON_LEFTSTICK,     ME_XI_LSTICK },
        { ME_SDL_BUTTON_RIGHTSTICK,    ME_XI_RSTICK },
        { ME_SDL_BUTTON_LEFTSHOULDER,  ME_XI_LB },
        { ME_SDL_BUTTON_RIGHTSHOULDER, ME_XI_RB },
        { ME_SDL_BUTTON_A,             ME_XI_A },
        { ME_SDL_BUTTON_B,             ME_XI_B },
        { ME_SDL_BUTTON_X,             ME_XI_X },
        { ME_SDL_BUTTON_Y,             ME_XI_Y },
    };
    memset(gp, 0, sizeof(*gp));
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (g_sdl.GameControllerGetButton(gc, map[i].button)) gp->wButtons |= map[i].mask;
    /* SDL's Y axes point down, XInput's up; -(-32768) is clamped to 32767. */
    int ly = -(int)g_sdl.GameControllerGetAxis(gc, ME_SDL_AXIS_LEFTY);
    int ry = -(int)g_sdl.GameControllerGetAxis(gc, ME_SDL_AXIS_RIGHTY);
    gp->sThumbLX = g_sdl.GameControllerGetAxis(gc, ME_SDL_AXIS_LEFTX);
    gp->sThumbRX = g_sdl.GameControllerGetAxis(gc, ME_SDL_AXIS_RIGHTX);
    gp->sThumbLY = (int16_t)(ly > 32767 ? 32767 : ly);
    gp->sThumbRY = (int16_t)(ry > 32767 ? 32767 : ry);
    /* Triggers: SDL 0..32767 → XInput 0..255. */
    int lt = g_sdl.GameControllerGetAxis(gc, ME_SDL_AXIS_TRIGGERLEFT);
    int rt = g_sdl.GameControllerGetAxis(gc, ME_SDL_AXIS_TRIGGERRIGHT);
    gp->bLeftTrigger  = (uint8_t)(lt > 0 ? lt * 255 / 32767 : 0);
    gp->bRightTrigger = (uint8_t)(rt > 0 ? rt * 255 / 32767 : 0);
}

/* The SDL pad in `slot`, if there is one. `scan` also looks for pads
   plugged in since the last scan, which may land in `slot`. */
static int sdl_read(int slot, ME_XINPUT_GAMEPAD *gp, int scan) {
    if (!g_sdl_ok) return 0;
    EnterCriticalSection(&g_sdl_lock);
    if (g_sdl_pad[slot] || scan) sdl_update(scan);
    int got = g_sdl_pad[slot] != NULL;
    if (got) sdl_state(g_sdl_pad[slot], gp);
    LeaveCriticalSection(&g_sdl_lock);
    return got;
}

/* ---- public API ----------------------------------------------------------- */

int me_xinput_init(void) {
    if (g_xi_loaded != 0) return g_xi_loaded > 0 || g_sdl_ok;

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
    if (g_xi_dll) {
        g_xi_GetState = (PFN_XInputGetState)GetProcAddress(g_xi_dll, "XInputGetState");
        g_xi_SetState = (PFN_XInputSetState)(void (*)(void))GetProcAddress(g_xi_dll, "XInputSetState");
        if (!g_xi_GetState) {
            FreeLibrary(g_xi_dll);
            g_xi_dll = NULL;
        }
    }
    g_xi_loaded = g_xi_dll ? 1 : -1;

    memset(g_xi_state,     0, sizeof(g_xi_state));
    memset(g_xi_connected, 0, sizeof(g_xi_connected));

    InitializeCriticalSection(&g_sdl_lock);
    if (sdl_load()) {
        g_sdl_ok = 1;
        EnterCriticalSection(&g_sdl_lock);
        sdl_update(1);
        LeaveCriticalSection(&g_sdl_lock);
        printf("[pad] SDL2 active: DirectInput, Switch and PlayStation controllers work too\n");
    }
    return g_xi_loaded > 0 || g_sdl_ok;
}

void me_xinput_poll(int player_index) {
    if (g_xi_loaded == 0) return;
    if (player_index < 0 || player_index >= ME_XI_SLOTS) return;

    ME_XINPUT_GAMEPAD gp;
    int got = sdl_read(player_index, &gp, 0);
    if (!got) {
        if (!g_xi_connected[player_index] &&
            (LONG)(GetTickCount() - g_xi_next_probe[player_index]) < 0) return;
        /* Nothing on XInput: a newly plugged-in pad may take the slot. */
        got = xi_get(player_index, &gp) || sdl_read(player_index, &gp, 1);
    }
    if (got) {
        g_xi_connected[player_index] = 1;
        g_xi_state[player_index].Gamepad = gp;
        g_xi_buttons[player_index] = buttons_from_state(&gp);
    } else {
        g_xi_connected[player_index] = 0;
        memset(&g_xi_state[player_index], 0, sizeof(g_xi_state[player_index]));
        g_xi_buttons[player_index] = 0;
        g_xi_next_probe[player_index] = GetTickCount() + ME_XI_PROBE_INTERVAL_MS;
    }
}

int me_xinput_button(int player_index, unsigned buttons) {
    if (player_index < 0 || player_index >= ME_XI_SLOTS) return 0;
    if (!g_xi_connected[player_index] || !buttons) return 0;
    return (g_xi_buttons[player_index] & buttons) == buttons ? 1 : 0;
}

int16_t me_xinput_axis(int player_index, me_xi_axis axis) {
    if (player_index < 0 || player_index >= ME_XI_SLOTS) return 0;
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
    if (player_index < 0 || player_index >= ME_XI_SLOTS) return 0;
    return g_xi_connected[player_index];
}

void me_xinput_rumble(int slot, uint16_t strong, uint16_t weak) {
    /* Last speeds sent per slot: the driver call only happens on a change,
       and cores set the same strength every frame. */
    static volatile LONG sent[ME_XI_SLOTS];
    if (slot < 0 || slot >= ME_XI_SLOTS) return;
    LONG v = (LONG)(((uint32_t)strong << 16) | weak);
    if (InterlockedExchange(&sent[slot], v) == v) return;
    if (g_sdl_ok) {
        EnterCriticalSection(&g_sdl_lock);
        SDL_GameController *gc = g_sdl_pad[slot];
        /* Duration 0: until the next change, as with XInput. */
        if (gc) g_sdl.GameControllerRumble(gc, strong, weak, 0);
        LeaveCriticalSection(&g_sdl_lock);
        if (gc) return;
    }
    if (!g_xi_SetState || slot >= ME_XI_XINPUT_SLOTS) return;
    ME_XINPUT_VIBRATION vib = { strong, weak };
    g_xi_SetState((DWORD)slot, &vib);
}

unsigned me_xinput_read(int slot, int *connected) {
    if (connected) *connected = 0;
    if (g_xi_loaded == 0 || slot < 0 || slot >= ME_XI_SLOTS) return 0;
    ME_XINPUT_GAMEPAD gp;
    if (!sdl_read(slot, &gp, 1) && !xi_get(slot, &gp)) return 0;
    if (connected) *connected = 1;
    return buttons_from_state(&gp);
}

int me_xinput_name(int slot, char *out, size_t out_sz) {
    if (out_sz) out[0] = '\0';
    if (g_xi_loaded == 0 || slot < 0 || slot >= ME_XI_SLOTS) return 0;
    if (g_sdl_ok) {
        EnterCriticalSection(&g_sdl_lock);
        sdl_update(1);
        SDL_GameController *gc = g_sdl_pad[slot];
        const char *name = gc ? g_sdl.GameControllerName(gc) : NULL;
        if (gc) snprintf(out, out_sz, "%s", name ? name : "controller");
        LeaveCriticalSection(&g_sdl_lock);
        if (gc) return 1;
    }
    ME_XINPUT_GAMEPAD gp;
    if (!xi_get(slot, &gp)) return 0;
    snprintf(out, out_sz, "XInput");
    return 1;
}
