#ifndef ME_APP_H
#define ME_APP_H

/* The contract between the two threads:
     emulation thread (main.c)  — runs frames, owns the core/renderer/audio.
     UI thread (platform_win32.c, ui.c, ui_bindings.c) — owns the window,
       menus and dialogs, and pumps its messages.
   Win32 menus, dialogs and window moves/resizes run modal loops that block
   their thread, so they live on the UI thread and the game never pauses.
   The UI thread never waits on the emulation thread; it only posts commands. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "consoles.h"
#include "settings.h"

typedef enum {
    ME_CMD_LOAD_ROM,    /* path: ROM to load (core from its console, rom_cores.h);
                           arg: the console's index + 1 if the player picked it,
                           0 to work it out (a remembered pick, else its type
                           and header), ME_LOAD_ASK to ask the player even if
                           that's known */
    ME_CMD_HARD_RESET,  /* reload core + game */
    ME_CMD_SOFT_RESET,  /* retro_reset */
    ME_CMD_POWER,       /* power off the running game, or back on */
    ME_CMD_FRAME_GEN,   /* arg: 1 = on, 0 = off */
    ME_CMD_ADAPTER,     /* the running console's adapter changed in the live
                           settings; plug it into the core */
    ME_CMD_CONTROLLER,  /* a player's controller for the running console
                           changed in the live settings; plug it in */
} me_cmd_type;

#define ME_LOAD_ASK (-1)

typedef struct {
    me_cmd_type type;
    int         arg;
    char        path[MAX_PATH];
} me_cmd;

/* `live` is the settings struct the emulation thread runs from; `path` is
   settings.yaml. Call once from the emulation thread before the UI starts. */
void me_app_init(me_settings *live, const char *settings_path);
me_settings *me_app_settings(void);
const char  *me_app_settings_path(void);

/* Guards the parts of the live settings both threads touch: control maps,
   hotkeys, input sources, controller slots and console core picks. The
   emulation thread holds it only for a few microseconds per frame (input
   poll, hotkey check) and when it looks up a ROM's core; the UI thread only
   to copy settings in or out. Never hold it across anything slow. */
void me_settings_lock(void);
void me_settings_unlock(void);

/* UI → emulation. Never blocks. Commands run between frames. */
void me_cmd_post(me_cmd_type type, int arg, const char *path);
int  me_cmd_take(me_cmd *out);

/* Signaled when a command or quit request arrives; the idle loop waits on it. */
HANDLE me_app_wake_event(void);

void me_app_request_quit(void);
int  me_app_quit_requested(void);

/* Emulation state the menus reflect. Written by the emulation thread after
   anything changes; read by the UI thread when a menu opens. */
typedef struct {
    int  game_running;
    int  can_power_on;         /* powered off, with a game to power back on */
    int  vulkan_active;
    int  frame_gen_active;
    int  frame_gen_supported;  /* build includes LSFG */
    char core_path[MAX_PATH];  /* current (or powered-off) game */
    char rom_path[MAX_PATH];
    /* Players the running game gets (ME_MAX_PLAYERS with no game), its
       console (rom_cores.h index, -1 if unknown) and, per entry in that
       console's adapter list, whether the core can use it (bit) and which
       one is plugged in (-1 = none). */
    int      players;
    int      console;
    unsigned adapters_usable;
    int      adapter;
    /* The same for the console's controllers, per player (-1 = the core's
       own). */
    unsigned controllers_usable;
    int      controller[ME_MAX_PLAYERS];
    /* The running game's core takes Multiplayer > Fan Server (0 with no
       game). */
    int      fan_server_usable;
} me_app_status;

void me_status_set(const me_app_status *s);
void me_status_get(me_app_status *out);

/* Each player's controller in the running game (consoles.h), for the
   binding dialogs. Written by the emulation thread when a game loads,
   whenever the core re-describes its inputs and when a player's controller
   changes; read by the UI thread. */
void me_layout_publish(int player, const me_input_layout *l);
void me_layout_get(int player, me_input_layout *out);

#endif
