#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include <math.h>
#include "platform_win32.h"
#include <mmsystem.h>
#include "integer_scaling.h"
#include "core_loader.h"
#include "audio_wasapi.h"
#include "render_d3d11.h"
#include "render_vulkan.h"
#include "gl_context.h"
#include "libretro.h"
#include "settings.h"
#include "xinput_pad.h"
#include "lsfg_loader.h"
#include "rom_cores.h"
#include "app.h"
#include "ui.h"
#include "wii_wfc.h"

/* ---- exe-relative path helpers -------------------------------------------- */
static char g_exedir[MAX_PATH];

static void init_exedir(void) {
    GetModuleFileNameA(NULL, g_exedir, sizeof(g_exedir));
    char *slash = strrchr(g_exedir, '\\');
    if (slash) slash[1] = '\0'; else g_exedir[0] = '\0';
}

/* Concatenate g_exedir + rel into buf; returns buf. */
static char *exepath(char *buf, size_t sz, const char *rel) {
    snprintf(buf, sz, "%s%s", g_exedir, rel);
    return buf;
}

/* Exact refresh rate for a GDI device (e.g. "\\.\DISPLAY1") as a real number.
   EnumDisplaySettings only reports integer Hz (240 for a 239.760 Hz mode),
   which is useless for refresh matching: 240/4 = 60.000 still beats against the
   true 59.940 sub-rate. QueryDisplayConfig returns the precise rational
   (240000/1001 = 239.760), so refresh/N lands exactly on 59.940. Returns 0 on
   failure so the caller can fall back to the integer path. */
static double me_query_exact_refresh_hz(const char *gdi_device) {
    UINT32 npath = 0, nmode = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &npath, &nmode) != ERROR_SUCCESS)
        return 0.0;
    DISPLAYCONFIG_PATH_INFO *paths = calloc(npath, sizeof(*paths));
    DISPLAYCONFIG_MODE_INFO *modes = calloc(nmode, sizeof(*modes));
    double hz = 0.0;
    if (paths && modes &&
        QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &npath, paths, &nmode, modes, NULL) == ERROR_SUCCESS) {
        for (UINT32 i = 0; i < npath; i++) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {0};
            src.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            src.header.size      = sizeof(src);
            src.header.adapterId = paths[i].sourceInfo.adapterId;
            src.header.id        = paths[i].sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
            char name[CCHDEVICENAME] = {0};
            wcstombs(name, src.viewGdiDeviceName, sizeof(name) - 1);
            if (strcmp(name, gdi_device) != 0) continue;
            DISPLAYCONFIG_RATIONAL r = paths[i].targetInfo.refreshRate;
            if (r.Denominator) hz = (double)r.Numerator / (double)r.Denominator;
            break;
        }
    }
    free(paths);
    free(modes);
    return hz;
}

/* ---- global state for callbacks (single core, single ROM) ----------------- */
static enum retro_pixel_format g_pixel_format = RETRO_PIXEL_FORMAT_0RGB1555;

/* BGRX backbuffer (32 bpp, top-down via negative bmi height). */
static u32 *g_back = NULL;
static unsigned g_back_max_w = 0, g_back_max_h = 0;
/* The core's timing the game is paced by: from its AV info at load, or
   SET_SYSTEM_AV_INFO since, which sets g_av_timing_changed for the frame
   loop to pick up. */
static double g_av_fps = 0.0, g_av_rate = 0.0;
static int    g_av_timing_changed = 0;
static int    g_av_refused = 0;   /* a bigger frame refused (logged once) */
static unsigned g_frame_w = 0, g_frame_h = 0;
/* The shape (width / height) the core says its whole frame is shown at:
   AV info at load, SET_GEOMETRY since (Dolphin switches between 4:3 and
   16:9 as the game does). 0 or less: square pixels. */
static float g_core_aspect = 0.0f;
static unsigned long g_video_calls = 0;

static HWND g_hwnd = NULL;
static int  g_use_d3d11 = 0;
static int  g_force_vulkan = 0; /* --vulkan requested on the CLI */
static int  g_no_vsync = 0;     /* --no-vsync (Vulkan: IMMEDIATE present mode) */

/* LSFG frame-gen (Plan B) */
static int               g_lsfg_enabled  = 0;    /* --lsfg flag */
static char              g_lsfg_dll_path[MAX_PATH] = {0}; /* --lsfg-dll= override */
static me_lsfg_shaders  *g_lsfg_shaders  = NULL; /* loaded shader table */

/* Latency telemetry: timestamp of the most recent input poll callback. Read
   by the main loop to split retro_run() into "before poll" and "after poll"
   stages. Cores call poll exactly once per retro_run(), so this is a clean
   split point. 0 means "no poll happened this retro_run()" (some cores
   may skip polls during run-ahead's muted advance). */
static int           g_latency_log = 0;
static LARGE_INTEGER g_poll_qpc = {0};

/* Aspect mode (g_settings.aspect, me_aspect_mode order): F1 cycles; View >
   Aspect Ratio picks one. Auto's shape comes from the core (g_core_aspect). */
static const char *g_aspect_names[ME_ASPECT_COUNT] = { "auto", "1:1", "4:3", "16:9" };
static const int g_aspect_x[ME_ASPECT_COUNT] = { 0, 1, 4, 16 };
static const int g_aspect_y[ME_ASPECT_COUNT] = { 0, 1, 3,  9 };

static int hk_pressed(const me_kb_bindings *b) {
    for (int i = 0; i < b->count; i++) {
        const me_kb_binding *k = &b->b[i];
        if (me_platform_key_pressed(k->vk, k->ctrl, k->alt, k->shift)) return 1;
    }
    return 0;
}

/* Edge-triggered XInput hotkey check. `prev` is a per-hotkey bit that tracks
   whether the chord was down last frame; updated in place each call. */
static int hk_xi_pressed(const me_xi_bindings *b, int player, int *prev) {
    int down = 0;
    for (int i = 0; i < b->count; i++) {
        if (me_xinput_button(player, b->b[i].buttons)) { down = 1; break; }
    }
    int fired = down && !*prev;
    *prev = down;
    return fired;
}

/* ---- log callback ---------------------------------------------------------
   Core messages are queued and written to stderr by a background thread.
   Cores log from inside retro_run (Dolphin: a kilobyte-long warning per
   shader compile, EFB warnings every frame), and each console write is an
   IPC round-trip to the console host, charged to the frame being run. */
#define ME_CORELOG_CAP (256 * 1024)
static char             g_corelog_buf[ME_CORELOG_CAP];
static size_t           g_corelog_len;
static unsigned         g_corelog_dropped;
static int              g_corelog_quit;
static CRITICAL_SECTION g_corelog_lock;
static HANDLE           g_corelog_event, g_corelog_thread;

static DWORD WINAPI corelog_thread(LPVOID arg) {
    (void)arg;
    static char out[ME_CORELOG_CAP];
    for (;;) {
        WaitForSingleObject(g_corelog_event, INFINITE);
        EnterCriticalSection(&g_corelog_lock);
        size_t n = g_corelog_len;
        memcpy(out, g_corelog_buf, n);
        g_corelog_len = 0;
        unsigned dropped = g_corelog_dropped;
        g_corelog_dropped = 0;
        int quit = g_corelog_quit;
        LeaveCriticalSection(&g_corelog_lock);
        if (n) fwrite(out, 1, n, stderr);
        if (dropped) fprintf(stderr, "[core] (%u log messages dropped)\n", dropped);
        fflush(stderr);
        if (quit) return 0;
    }
}

static void me_corelog_start(void) {
    InitializeCriticalSection(&g_corelog_lock);
    g_corelog_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (g_corelog_event)
        g_corelog_thread = CreateThread(NULL, 0, corelog_thread, NULL, 0, NULL);
}

/* Drain what's queued and stop the writer; later messages go direct. */
static void me_corelog_stop(void) {
    if (!g_corelog_thread) return;
    EnterCriticalSection(&g_corelog_lock);
    g_corelog_quit = 1;
    LeaveCriticalSection(&g_corelog_lock);
    SetEvent(g_corelog_event);
    WaitForSingleObject(g_corelog_thread, 2000);
    CloseHandle(g_corelog_thread);
    g_corelog_thread = NULL;
}

static void me_log_cb(enum retro_log_level level, const char *fmt, ...) {
    const char *tag = "?";
    switch (level) {
        case RETRO_LOG_DEBUG: tag = "DBG"; break;
        case RETRO_LOG_INFO:  tag = "INF"; break;
        case RETRO_LOG_WARN:  tag = "WRN"; break;
        case RETRO_LOG_ERROR: tag = "ERR"; break;
        default: break;
    }
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) { fprintf(stderr, "[core:%s] (format error)\n", tag); return; }
    char line[1040];
    int len = snprintf(line, sizeof(line), "[core:%s] %s", tag, buf);
    if (len < 0) return;
    if (len >= (int)sizeof(line)) len = (int)sizeof(line) - 1;
    if (!g_corelog_thread) { fwrite(line, 1, (size_t)len, stderr); return; }
    EnterCriticalSection(&g_corelog_lock);
    if (g_corelog_len + (size_t)len <= ME_CORELOG_CAP) {
        memcpy(g_corelog_buf + g_corelog_len, line, (size_t)len);
        g_corelog_len += (size_t)len;
    } else {
        g_corelog_dropped++;
    }
    LeaveCriticalSection(&g_corelog_lock);
    SetEvent(g_corelog_event);
}

/* ---- hardware-rendering state --------------------------------------------
   For GL cores, the core asks us via SET_HW_RENDER for a GL context, an FBO
   id (via get_current_framebuffer), and a way to look up GL entry points
   (via get_proc_address). It then renders into that FBO and signals frames
   by passing RETRO_HW_FRAME_BUFFER_VALID to retro_video_refresh.

   Step 1 just records the request and installs stub callbacks so we can see
   what cores ask for via --env-trace. The real GL context, FBO, and frame
   transport come in later steps. Until then, get_current_framebuffer returns
   0 (the default framebuffer) — cores that actually try to render will draw
   nowhere, which is fine for Step 1 (we reject the env call anyway if no
   GL backend is wired up yet, so cores fall back or refuse to load). */
static struct retro_hw_render_callback g_hw_render;
static int g_hw_render_requested = 0;  /* core called SET_HW_RENDER */
static int g_hw_render_accepted  = 0;  /* and we said yes */

static uintptr_t me_hw_get_current_framebuffer(void) {
    return (uintptr_t)me_gl_fbo_id();
}

static retro_proc_address_t me_hw_get_proc_address(const char *sym) {
    return (retro_proc_address_t)me_gl_get_proc_address(sym);
}

static const char *hw_context_name(enum retro_hw_context_type t) {
    switch (t) {
        case RETRO_HW_CONTEXT_NONE:             return "NONE";
        case RETRO_HW_CONTEXT_OPENGL:           return "OPENGL";
        case RETRO_HW_CONTEXT_OPENGLES2:        return "OPENGLES2";
        case RETRO_HW_CONTEXT_OPENGL_CORE:      return "OPENGL_CORE";
        case RETRO_HW_CONTEXT_OPENGLES3:        return "OPENGLES3";
        case RETRO_HW_CONTEXT_OPENGLES_VERSION: return "OPENGLES_VERSION";
        case RETRO_HW_CONTEXT_VULKAN:           return "VULKAN";
        case RETRO_HW_CONTEXT_D3D11:            return "D3D11";
        default:                                return "?";
    }
}

/* ---- environment callback ------------------------------------------------- */
/* Track which unhandled env cmd IDs we've already logged so the trace doesn't
   spam the same call hundreds of times per second. */
static unsigned char g_env_seen[256];
static int g_env_trace = 0; /* set by --env-trace flag */

/* Diagnostic log files. Hot-path messages (pace/timing/latency/env) used to
   go to stdout/stderr — but a printf to the Windows console host costs an
   IPC round-trip per call, enough to spike a frame past its deadline when
   several streams are enabled at once. Route each stream to its own file
   under ./logs/ with line buffering so completed lines land quickly without
   per-write flushes. NULL = closed; lazily opened on first message. */
typedef enum {
    ME_LOG_PACE = 0,
    ME_LOG_TIMING,
    ME_LOG_LATENCY,
    ME_LOG_ENV,
    ME_LOG_COUNT
} me_log_stream;
static const char *g_log_names[ME_LOG_COUNT] = { "pace", "timing", "latency", "env" };
static FILE *g_log_files[ME_LOG_COUNT] = {0};
static int   g_log_dir_tried = 0;
/* Absolute ./logs, fixed at startup: the Open ROM dialog changes the
   process's working directory while it is open. */
static char  g_log_dir[MAX_PATH] = "logs";

static FILE *me_log_open(me_log_stream s) {
    if (s >= ME_LOG_COUNT) return NULL;
    if (g_log_files[s]) return g_log_files[s];
    if (!g_log_dir_tried) {
        g_log_dir_tried = 1;
        CreateDirectoryA(g_log_dir, NULL); /* ignore ALREADY_EXISTS */
    }
    char path[MAX_PATH + 16];
    snprintf(path, sizeof(path), "%s\\%s.log", g_log_dir, g_log_names[s]);
    FILE *f = fopen(path, "a");
    if (!f) {
        fprintf(stderr, "[log] cannot open %s; falling back to stderr for %s stream\n",
                path, g_log_names[s]);
        return NULL;
    }
    setvbuf(f, NULL, _IOLBF, 4096);
    /* Append-mode files accumulate across runs; this header makes it obvious
       where one session ends and the next begins. */
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(f, "\n---- session %04u-%02u-%02u %02u:%02u:%02u ----\n",
            t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    g_log_files[s] = f;
    return f;
}

static void me_log(me_log_stream s, const char *fmt, ...) {
    FILE *f = me_log_open(s);
    if (!f) f = stderr;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    /* The Windows CRT treats _IOLBF as full buffering: flush completed lines
       ourselves, or a crash or hard exit loses the last 4 KB. */
    size_t n = strlen(fmt);
    if (n && fmt[n - 1] == '\n') fflush(f);
}

static void me_log_close_all(void) {
    for (int i = 0; i < ME_LOG_COUNT; i++) {
        if (g_log_files[i]) { fclose(g_log_files[i]); g_log_files[i] = NULL; }
    }
}

static me_settings g_settings;

/* Core options storage. SET_VARIABLES hands us a { key, "Desc; v1|v2|v3" }
   array terminated by { NULL, NULL }. At SET time we walk it once, strdup
   each default ("v1") into a parallel array. GET_VARIABLE then returns the
   precomputed default string. */
struct me_var { char *key; char *def_value; };
static struct me_var *g_vars = NULL;
static size_t g_var_count = 0;

/* Core options we pick over the core's default, used only when the core
   offers that exact value. Touch screens: the mouse is the stylus, at the
   real cursor, so cores take it as an absolute pointer and don't draw a
   cursor of their own. */
static const struct { const char *key, *value; } k_var_overrides[] = {
    { "melonds_show_cursor", "disabled" },  /* melonDS DS */
    { "melonds_touch_mode",  "Touch" },     /* melonDS (melonDS DS's auto already takes the pointer) */
    { "noods_touchCursor",   "disabled" },  /* NooDS */
    { "desmume_pointer_type", "touch" },    /* DeSmuME */
    /* Dolphin: Sync (UberShaders). Its libretro GL context can't make shared
       contexts, so shaders never compile in the background: the default
       (Synchronous) and both Async modes compile every new specialized
       shader inside retro_run, a ~70 ms hitch each. Ubershaders cover every
       pipeline from a small set compiled up front and cached to disk. */
    { "dolphin_shader_compilation_mode", "1" },
    /* Dolphin: don't copy each game's codes to cheats\ for RetroArch's cheat
       menu, which we don't have (it would keep the Wii Fan Server's patch). */
    { "dolphin_cheats_import", "disabled" },
};

/* The override for `key` if `values` ("v1|v2|v3") lists it, else NULL. */
static const char *me_var_override(const char *key, const char *values) {
    for (size_t i = 0; i < sizeof(k_var_overrides) / sizeof(k_var_overrides[0]); i++) {
        if (strcmp(k_var_overrides[i].key, key) != 0) continue;
        const char *want = k_var_overrides[i].value;
        size_t n = strlen(want);
        for (const char *p = values; *p; ) {
            while (*p == ' ') p++;
            size_t len = strcspn(p, "|");
            if (len == n && strncmp(p, want, n) == 0) return want;
            p += len;
            if (*p == '|') p++;
        }
    }
    return NULL;
}

static void me_vars_set(const struct retro_variable *src) {
    /* Free any previous set (some cores re-declare on retry). */
    for (size_t i = 0; i < g_var_count; i++) {
        free(g_vars[i].key); free(g_vars[i].def_value);
    }
    free(g_vars); g_vars = NULL; g_var_count = 0;
    if (!src) return;
    size_t n = 0; while (src[n].key) n++;
    g_vars = (struct me_var *)calloc(n, sizeof(*g_vars));
    if (!g_vars) return;
    g_var_count = n;
    for (size_t i = 0; i < n; i++) {
        g_vars[i].key = _strdup(src[i].key);
        const char *val = src[i].value ? src[i].value : "";
        const char *semi = strchr(val, ';');
        const char *p = semi ? semi + 1 : val;
        while (*p == ' ') p++;
        const char *over = me_var_override(src[i].key, p);
        if (over) p = over;
        size_t len = 0; while (p[len] && p[len] != '|') len++;
        char *d = (char *)malloc(len + 1);
        if (d) { memcpy(d, p, len); d[len] = '\0'; }
        g_vars[i].def_value = d;
    }
}

static const char *me_var_default_for(const char *key) {
    if (!key) return NULL;
    for (size_t i = 0; i < g_var_count; i++) {
        if (g_vars[i].key && strcmp(g_vars[i].key, key) == 0) return g_vars[i].def_value;
    }
    return NULL;
}

/* Core options that set the emulated console's DNS, which Multiplayer > Fan
   Server fills with its server's. Cores read them when they build the
   console, so a change applies at the next load or Hard Reset. `off` is the
   value that leaves the firmware's own DNS. */
static const struct { const char *key, *off; } k_dns_vars[] = {
    { "melonds_firmware_wfc_dns", "default" },   /* melonDS DS: any IPv4 address */
};

static const char *me_dns_var_value(const char *key) {
    for (size_t i = 0; i < sizeof(k_dns_vars) / sizeof(k_dns_vars[0]); i++) {
        if (strcmp(k_dns_vars[i].key, key) != 0) continue;
        const char *dns = me_fan_server_info_of(g_settings.fan_server)->dns;
        return dns ? dns : k_dns_vars[i].off;
    }
    return NULL;
}

/* Does the loaded core take a Fan Server? */
static int me_core_has_dns_var(void) {
    for (size_t i = 0; i < sizeof(k_dns_vars) / sizeof(k_dns_vars[0]); i++)
        if (me_var_default_for(k_dns_vars[i].key)) return 1;
    return 0;
}

/* A Wii game's Fan Server (wii_wfc.h): Dolphin runs its patch when the
   core's cheats are on, which they are for a patched game only. Dolphin
   declares its options inside retro_load_game, then reads them, then boots
   the game (reading its inis), so the patch is set up when they're declared.
   g_wii_wfc_rom is the game being loaded, NULL outside retro_load_game. */
#define ME_DOLPHIN_CHEATS_VAR "dolphin_cheats_enabled"
static me_wii_wfc  g_wii_wfc;
static const char *g_wii_wfc_rom;

static void me_wii_wfc_on_options(void) {
    if (!g_wii_wfc_rom || !me_var_default_for(ME_DOLPHIN_CHEATS_VAR)) return;
    char user_dir[MAX_PATH];   /* Dolphin's: <save directory>\User */
    exepath(user_dir, sizeof(user_dir), "saves\\User");
    me_wii_wfc_prepare(g_wii_wfc_rom, user_dir, g_settings.wii_fan_server, &g_wii_wfc);
    g_wii_wfc_rom = NULL;   /* once per load */
}

/* GET_VARIABLE: the core's default, or ours where we set it. */
static const char *me_var_value(const char *key) {
    const char *def = me_var_default_for(key);
    if (!def) return NULL;   /* not one of the core's options */
    const char *dns = me_dns_var_value(key);
    if (dns) return dns;
    if (g_wii_wfc.patched && strcmp(key, ME_DOLPHIN_CHEATS_VAR) == 0) return "enabled";
    /* View > Wii Display: the Wii's TV setting, read when the game boots. */
    if (strcmp(key, "dolphin_widescreen") == 0) return g_settings.wii_widescreen ? "enabled" : "disabled";
    return def;
}

/* Each player's controller in the running game (consoles.h). Inputs outside
   `live` never reach the core: many cores put turbo buttons, macros and
   disk/coin actions on RetroPad buttons their console doesn't have, and a
   leftover binding must not trigger them. Chosen from the core's
   library_name and the ROM in session_open; cores without a curated layout
   refine it through SET_INPUT_DESCRIPTORS. Players differ only where they
   picked different controllers (a Classic Controller, a Wii Remote...). */
static me_input_layout g_in_layout[ME_MAX_PLAYERS];
static char            g_core_name[64];   /* library_name, for the dialogs */

static void set_player_layout(int p, const me_input_layout *l) {
    g_in_layout[p] = *l;
    me_layout_publish(p, l);
}

static void set_input_layout(const me_input_layout *l) {
    for (int p = 0; p < ME_MAX_PLAYERS; p++) set_player_layout(p, l);
}

/* The controller types the core offers per port (SET_CONTROLLER_INFO),
   where multiplayer adapters are found. */
#define ME_CI_PORTS 8
#define ME_CI_TYPES 32
static struct { char desc[64]; unsigned id; } g_ci[ME_CI_PORTS][ME_CI_TYPES];
static int g_ci_n[ME_CI_PORTS];
static int g_ci_ports;   /* ports the core described */

static void store_controller_info(const struct retro_controller_info *ci) {
    memset(g_ci_n, 0, sizeof(g_ci_n));
    g_ci_ports = 0;
    for (int p = 0; p < ME_CI_PORTS && ci[p].types; p++) {
        g_ci_ports = p + 1;
        for (unsigned t = 0; t < ci[p].num_types && g_ci_n[p] < ME_CI_TYPES; t++) {
            const struct retro_controller_description *d = &ci[p].types[t];
            if (!d->desc) continue;
            int n = g_ci_n[p]++;
            snprintf(g_ci[p][n].desc, sizeof(g_ci[p][n].desc), "%s", d->desc);
            g_ci[p][n].id = d->id;
            if (g_env_trace) me_log(ME_LOG_ENV, "[env]   port %d: %s (0x%x)\n", p, d->desc, d->id);
        }
    }
}

/* The running game's console (rom_cores.h index, -1 if unknown), how many
   players it gets, and the multiplayer adapter plugged in. Emulation
   thread only. */
static int               g_console = -1;
static int               g_players = ME_MAX_PLAYERS;
static const me_adapter *g_adapter;
static unsigned          g_adapter_ports;   /* ports holding the adapter's device */

/* Does `desc` contain any of the '|'-separated `names` (any case)? */
static int desc_matches(const char *desc, const char *names) {
    while (*names) {
        const char *end = strchr(names, '|');
        size_t len = end ? (size_t)(end - names) : strlen(names);
        for (const char *p = desc; len && strlen(p) >= len; p++)
            if (_strnicmp(p, names, len) == 0) return 1;
        if (!end) break;
        names = end + 1;
    }
    return 0;
}

/* The core's first device on `port` named any of `names` (as desc_matches),
   or 0 (RETRO_DEVICE_NONE) if it has none. */
static unsigned core_device(const char *names, int port) {
    if (port >= ME_CI_PORTS) return 0;
    for (int t = 0; t < g_ci_n[port]; t++)
        if (desc_matches(g_ci[port][t].desc, names)) return g_ci[port][t].id;
    return 0;
}

/* The core's device for adapter `a` on `port`, or 0 if it has none. */
static unsigned adapter_device(const me_adapter *a, int port) {
    return core_device(a->devices, port);
}

static int adapter_usable(const me_core *core, const me_adapter *a) {
    if (!a->devices) return 1;
    if (!core || !core->retro_set_controller_port_device) return 0;
    for (int p = 0; p < 32; p++)
        if ((a->ports >> p & 1u) && !adapter_device(a, p)) return 0;
    return 1;
}

/* The controller in each port (rom_cores.h me_controller: the Wii's
   Classic Controller...), or NULL for a gamepad. Emulation thread only. */
static const me_controller *g_controller[ME_MAX_PLAYERS];

static int controller_usable(const me_core *core, const me_controller *ctl) {
    return core && core->retro_set_controller_port_device && core_device(ctl->devices, 0);
}

/* Bit per controller in console `c`'s list the core can use. */
static unsigned controllers_usable(const me_core *core, const me_console *c) {
    const me_controller *list = me_console_controllers(c);
    unsigned bits = 0;
    for (int i = 0; list && list[i].id && i < 32; i++)
        if (controller_usable(core, &list[i])) bits |= 1u << i;
    return bits;
}

/* The controller the settings pick for player `p` of the running console,
   or NULL when the core offers no choice (Dolphin running a GameCube game
   has only the GameCube controller). A pick the core lacks falls back to
   the first it has. */
static const me_controller *pick_controller(const me_core *core, int p) {
    const me_console *c = me_console_at(g_console);
    unsigned usable = controllers_usable(core, c);
    if ((usable & (usable - 1)) == 0) return NULL;   /* fewer than two */
    me_settings_lock();
    int i = me_console_controller(c, me_settings_console_controller(&g_settings, c->id, p));
    me_settings_unlock();
    if (!(usable >> i & 1u))
        for (i = 0; !(usable >> i & 1u); i++) {}
    return &me_console_controllers(c)[i];
}

/* What port `p` gets while no adapter has it. */
static unsigned port_device(int p) {
    unsigned d = p < ME_MAX_PLAYERS && g_controller[p] ? core_device(g_controller[p]->devices, p) : 0;
    return d ? d : RETRO_DEVICE_JOYPAD;
}

/* Plug port `p`'s device in. A port with a choice of controllers is
   emptied first, so the console sees the picked one alone: Dolphin plugs
   its GameCube controller into a Wii without taking the Wii Remote out of
   its config (Wii Remote 1 is in by default), and puts the Remote back on
   its next config change, where emptying the port takes both out. `was`,
   the controller it had (NULL if none), goes out its own way first where
   it has one (rom_cores.h me_controller.unplug). */
static void plug_port(me_core *core, int p, const me_controller *was) {
    unsigned out = was && was->unplug ? core_device(was->unplug, p) : 0;
    if (out)
        core->retro_set_controller_port_device((unsigned)p, out);
    if (p < ME_MAX_PLAYERS && g_controller[p])
        core->retro_set_controller_port_device((unsigned)p, RETRO_DEVICE_NONE);
    core->retro_set_controller_port_device((unsigned)p, port_device(p));
}

/* Player `p`'s controller's own buttons, in place of the console's. */
static void set_controller_layout(int p) {
    const me_console *c = me_console_at(g_console);
    me_input_layout l;
    if (g_controller[p] &&
        me_layout_for_controller(g_controller[p]->layout, c && (c->flags & ME_CONSOLE_ANALOG), &l)) {
        set_player_layout(p, &l);
        printf("[input] player %d: %s (%d inputs)\n", p + 1, g_controller[p]->name, l.n);
    }
}

/* Plug a gamepad (or the controller the player picked for the console)
   into each port the core described, as RetroArch does once a game is
   loaded: some cores (Dolphin) set a port's controller up only when told
   what's in it, and never read input otherwise. Cores that describe no
   ports keep their own defaults. The adapter, if any, goes in after. */
static void plug_gamepads(me_core *core) {
    if (!core->retro_set_controller_port_device) return;
    for (int p = 0; p < g_ci_ports && p < ME_MAX_PLAYERS; p++) {
        g_controller[p] = pick_controller(core, p);
        plug_port(core, p, NULL);
        set_controller_layout(p);
    }
}

/* Plug in the controllers the settings pick now, where they changed. Games
   notice a controller swapped in or out as they would on the console. */
static void apply_controllers(me_core *core) {
    for (int p = 0; p < g_ci_ports && p < ME_MAX_PLAYERS; p++) {
        const me_controller *was = g_controller[p];
        g_controller[p] = pick_controller(core, p);
        if (g_controller[p] == was) continue;
        if (!(g_adapter_ports >> p & 1u))
            plug_port(core, p, was);
        set_controller_layout(p);
    }
}

/* Plug in (or unplug) the console's adapter as the settings say, and give
   the game its players. Called once the game is loaded, and again when the
   checkbox changes; most games look for an adapter only when they boot. */
static void apply_adapter(me_core *core) {
    const me_console *c = me_console_at(g_console);
    const me_adapter *a = NULL;
    if (c) {
        me_settings_lock();
        a = me_console_adapter(c, me_settings_console_adapter(&g_settings, c->id));
        me_settings_unlock();
        if (a && !adapter_usable(core, a)) {
            printf("[input] this core can't use the %s; players 3-4 stay off\n", a->name);
            a = NULL;
        }
    }
    unsigned want = a && a->devices ? a->ports : 0;
    if (a != g_adapter) {
        for (int p = 0; p < 32; p++) {
            unsigned bit = 1u << p;
            /* Ports the adapter leaves get what the others have. */
            if (want & bit)
                core->retro_set_controller_port_device((unsigned)p, adapter_device(a, p));
            else if (g_adapter_ports & bit)
                plug_port(core, p, NULL);
        }
        g_adapter_ports = want;
        g_adapter = a;
    }
    g_players = !c || a ? ME_MAX_PLAYERS : c->players;
    if (g_players > ME_MAX_PLAYERS) g_players = ME_MAX_PLAYERS;
    printf("[input] %d player%s%s%s\n", g_players, g_players == 1 ? "" : "s",
           a ? " with the " : "", a ? a->name : "");
}

/* Rumble: each player's strong and weak motor, as the core last set them,
   on the controller slot that player uses, unless that player turned
   rumble off (Controls > Player N). Cores may call from their own
   threads. */
static volatile uint16_t g_rumble[ME_MAX_PLAYERS][2];

static bool me_set_rumble_state(unsigned port, enum retro_rumble_effect effect, uint16_t strength) {
    if (port >= ME_MAX_PLAYERS || (effect != RETRO_RUMBLE_STRONG && effect != RETRO_RUMBLE_WEAK))
        return false;
    g_rumble[port][effect] = strength;
    me_settings_lock();
    int slot = g_settings.input_source[port] != ME_SRC_KEYBOARD ? g_settings.xi_index[port] : -1;
    int on = g_settings.rumble[port];
    me_settings_unlock();
    if (slot >= 0 && on) me_xinput_rumble(slot, g_rumble[port][RETRO_RUMBLE_STRONG], g_rumble[port][RETRO_RUMBLE_WEAK]);
    return true;
}

/* Each input poll: a player with rumble off whose game has the motors
   running gets them stopped, so turning rumble off mid-rumble takes effect
   now rather than on the core's next change. me_xinput_rumble skips
   repeats. Settings lock held. */
static void rumble_mute(int p) {
    if (g_settings.rumble[p] || !(g_rumble[p][RETRO_RUMBLE_STRONG] | g_rumble[p][RETRO_RUMBLE_WEAK])) return;
    if (g_settings.input_source[p] != ME_SRC_KEYBOARD) me_xinput_rumble(g_settings.xi_index[p], 0, 0);
}

/* Motors off on every pad, when a game closes. */
static void rumble_stop(void) {
    memset((void *)g_rumble, 0, sizeof(g_rumble));
    for (int slot = 0; slot < ME_XI_SLOTS; slot++) me_xinput_rumble(slot, 0, 0);
}

static bool me_environment_cb(unsigned cmd, void *data) {
    /* The "experimental" bit is set on some env IDs; mask it for matching. */
    unsigned base = cmd & ~RETRO_ENVIRONMENT_EXPERIMENTAL;
    switch (base) {
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: {
            /* Resolve to absolute on first use. Some cores (mupen64plus-next
               in particular) pass this straight into LoadLibrary / fopen
               for plugins/INIs and break on relative paths. */
            static char sysdir[MAX_PATH] = {0};
            if (sysdir[0] == 0) exepath(sysdir, sizeof(sysdir), "firmware");
            if (data) *(const char **)data = sysdir;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: {
            static char savedir[MAX_PATH] = {0};
            if (savedir[0] == 0) exepath(savedir, sizeof(savedir), "saves");
            if (data) *(const char **)data = savedir;
            return true;
        }
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
            enum retro_pixel_format pf = *(enum retro_pixel_format *)data;
            if (pf == RETRO_PIXEL_FORMAT_XRGB8888 || pf == RETRO_PIXEL_FORMAT_RGB565) {
                g_pixel_format = pf;
                printf("[env] SET_PIXEL_FORMAT = %s\n",
                       pf == RETRO_PIXEL_FORMAT_XRGB8888 ? "XRGB8888" : "RGB565");
                return true;
            }
            fprintf(stderr, "[env] SET_PIXEL_FORMAT %d rejected\n", (int)pf);
            return false;
        }
        case RETRO_ENVIRONMENT_GET_CAN_DUPE:
            /* video_refresh treats data==NULL as "keep the previous frame"
               (g_back is left untouched), so duping is free. Gambatte
               refuses to load without this. */
            if (data) *(bool *)data = true;
            return true;
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
            if (data) ((struct retro_log_callback *)data)->log = me_log_cb;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            struct retro_variable *v = (struct retro_variable *)data;
            if (!v) return false;
            v->value = me_var_value(v->key);
            return v->value != NULL;
        }
        case RETRO_ENVIRONMENT_SET_VARIABLES: {     /* 16 */
            const struct retro_variable *arr = (const struct retro_variable *)data;
            if (g_env_trace) {
                me_log(ME_LOG_ENV, "[env] SET_VARIABLES data=%p\n", (const void *)arr);
                if (arr) {
                    for (const struct retro_variable *v = arr; v->key; v++) {
                        me_log(ME_LOG_ENV, "[env]   key=%s value=%s\n",
                               v->key, v->value ? v->value : "(null)");
                    }
                }
            }
            me_vars_set(arr);
            me_wii_wfc_on_options();
            if (g_env_trace) me_log(ME_LOG_ENV, "[env] SET_VARIABLES stored %zu\n", g_var_count);
            return true;
        }
        case RETRO_ENVIRONMENT_SET_HW_RENDER: {     /* 14 */
            struct retro_hw_render_callback *cb = (struct retro_hw_render_callback *)data;
            if (!cb) return false;
            fprintf(stderr,
                    "[hw] SET_HW_RENDER context_type=%u (%s) version=%u.%u "
                    "depth=%d stencil=%d bottom_left=%d cache=%d debug=%d\n",
                    (unsigned)cb->context_type, hw_context_name(cb->context_type),
                    cb->version_major, cb->version_minor,
                    (int)cb->depth, (int)cb->stencil, (int)cb->bottom_left_origin,
                    (int)cb->cache_context, (int)cb->debug_context);
            fflush(stderr);
            /* For now accept only OpenGL / OpenGL Core. Vulkan-only cores
               (e.g. the Vulkan build of mupen64plus-next) will get a clean
               rejection here and refuse to load — the user should grab a
               GL build of the core instead. The actual GL context, FBO,
               and frame transport are built in later steps; until then a
               core that gets past this point will render nowhere. */
            if (cb->context_type != RETRO_HW_CONTEXT_OPENGL &&
                cb->context_type != RETRO_HW_CONTEXT_OPENGL_CORE) {
                fprintf(stderr,
                        "[hw] core requires %s; this front-end currently only supports OpenGL.\n"
                        "[hw] If this is a Vulkan-only build, try the GL build of the same core.\n",
                        hw_context_name(cb->context_type));
                fflush(stderr);
                return false;
            }
            g_hw_render = *cb;
            g_hw_render_requested = 1;
            int core_profile = (cb->context_type == RETRO_HW_CONTEXT_OPENGL_CORE);
            if (me_gl_init(core_profile, cb->version_major, cb->version_minor) != 0) {
                fprintf(stderr, "[hw] GL context creation failed; rejecting SET_HW_RENDER\n");
                g_hw_render_requested = 0;
                return false;
            }
            g_hw_render_accepted = 1;
            /* Fill in our side of the contract. get_current_framebuffer is
               still a stub (returns 0) until Step 3 wires up the real FBO,
               but get_proc_address is live now — cores that only need to
               resolve entry points during SET_HW_RENDER (rare) work already. */
            cb->get_current_framebuffer = me_hw_get_current_framebuffer;
            cb->get_proc_address        = me_hw_get_proc_address;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER: {  /* 56 */
            /* Cores often query this first to pick which API to request.
               Steer them to OpenGL since that's what we'll support. */
            if (data) *(unsigned *)data = RETRO_HW_CONTEXT_OPENGL;
            if (g_env_trace) me_log(ME_LOG_ENV, "[env] GET_PREFERRED_HW_RENDER -> OPENGL\n");
            return true;
        }
        case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:    /* 11 */
            /* Consoles in the table keep their curated controller; the
               core's extras become its advanced inputs. */
            if (data) {
                const struct retro_input_descriptor *d = (const struct retro_input_descriptor *)data;
                for (int p = 0; p < ME_MAX_PLAYERS; p++) {
                    me_input_layout l = g_in_layout[p];
                    if (l.curated) me_layout_set_advanced(&l, d);
                    else           me_layout_from_descriptors(g_core_name, d, &l);
                    set_player_layout(p, &l);
                }
            }
            return true;
        case RETRO_ENVIRONMENT_GET_INPUT_DEVICE_CAPABILITIES: /* 24 */
            if (!data) return false;
            *(uint64_t *)data = (1u << RETRO_DEVICE_JOYPAD) | (1u << RETRO_DEVICE_ANALOG) |
                                (1u << RETRO_DEVICE_MOUSE)  | (1u << RETRO_DEVICE_POINTER);
            return true;
        case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:   /* 35 */
            if (g_env_trace) me_log(ME_LOG_ENV, "[env] SET_CONTROLLER_INFO -> true\n");
            if (data) store_controller_info((const struct retro_controller_info *)data);
            return true;
        case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE: { /* 23 */
            if (!data) return false;
            ((struct retro_rumble_interface *)data)->set_rumble_state = me_set_rumble_state;
            if (g_env_trace) me_log(ME_LOG_ENV, "[env] GET_RUMBLE_INTERFACE -> true\n");
            return true;
        }
        case RETRO_ENVIRONMENT_SET_GEOMETRY: {         /* 37 */
            /* Frames are shown at whatever size the core draws them, and
               frame gen follows that too; nothing else uses the base size. */
            const struct retro_game_geometry *g = (const struct retro_game_geometry *)data;
            if (g_env_trace && g)
                me_log(ME_LOG_ENV, "[env] SET_GEOMETRY %ux%u aspect=%.4f -> true\n",
                       g->base_width, g->base_height, g->aspect_ratio);
            if (g) g_core_aspect = g->aspect_ratio;   /* View > Aspect Ratio > Auto */
            return true;
        }
        case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO: {   /* 32 */
            const struct retro_system_av_info *av = (const struct retro_system_av_info *)data;
            if (!av) return false;
            /* While loading, nothing is built yet: session_open reads the AV
               info after retro_load_game. Once running, new timing re-paces
               the game from the next frame on (session_run_frame), and a new
               size is a geometry change. A frame bigger than the backbuffer
               would need the renderers rebuilt, which isn't done: refused,
               and the core keeps its old size. */
            unsigned max_w = av->geometry.max_width  ? av->geometry.max_width  : av->geometry.base_width;
            unsigned max_h = av->geometry.max_height ? av->geometry.max_height : av->geometry.base_height;
            int ok = !g_back || (max_w <= g_back_max_w && max_h <= g_back_max_h);
            int retimed = ok && g_back && (av->timing.fps != g_av_fps || av->timing.sample_rate != g_av_rate);
            if (g_env_trace || retimed || (!ok && !g_av_refused))
                me_log(ME_LOG_ENV, "[env] SET_SYSTEM_AV_INFO %s max=%ux%u fps=%.4f rate=%.1f -> %s\n",
                       g_back ? "(running)" : "(loading)", max_w, max_h,
                       av->timing.fps, av->timing.sample_rate, ok ? "true" : "false");
            if (!ok) g_av_refused = 1;
            if (ok) g_core_aspect = av->geometry.aspect_ratio;
            if (retimed) {
                g_av_fps  = av->timing.fps;
                g_av_rate = av->timing.sample_rate;
                g_av_timing_changed = 1;
            }
            return ok;
        }
        case RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE:
            /* We don't actually apply the override (we use the original
               need_fullpath from get_system_info), so report false. Saying
               true puts cores into a state where they expect us to honor it. */
            if (g_env_trace) me_log(ME_LOG_ENV, "[env] SET_CONTENT_INFO_OVERRIDE -> false\n");
            return false; /* 65 */
        /* Report we only support legacy core options (v0). Cores using newer
           option formats fall back to v0 SET_VARIABLES, which we accept above. */
        case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: { /* 52 */
            if (data) *(unsigned *)data = 0;
            if (g_env_trace) me_log(ME_LOG_ENV, "[env] GET_CORE_OPTIONS_VERSION -> 0\n");
            return true;
        }
        default:
            if (g_env_trace && base < 256 && !g_env_seen[base]) {
                g_env_seen[base] = 1;
                me_log(ME_LOG_ENV, "[env] unhandled cmd %u%s -> false\n",
                       base, (cmd & RETRO_ENVIRONMENT_EXPERIMENTAL) ? " (experimental)" : "");
            }
            return false;
    }
}

/* ---- video conversion → BGRX ---------------------------------------------- */
static void convert_xrgb8888(const u32 *src, size_t pitch_bytes, unsigned w, unsigned h) {
    size_t src_stride = pitch_bytes / 4;
    for (unsigned y = 0; y < h; y++) {
        const u32 *s = src + y * src_stride;
        u32 *d = g_back + y * g_back_max_w;
        /* libretro XRGB8888 already matches GDI's BI_RGB 32bpp (BGRX in memory). */
        memcpy(d, s, w * 4);
    }
}

static void convert_rgb565(const u16 *src, size_t pitch_bytes, unsigned w, unsigned h) {
    size_t src_stride = pitch_bytes / 2;
    for (unsigned y = 0; y < h; y++) {
        const u16 *s = src + y * src_stride;
        u32 *d = g_back + y * g_back_max_w;
        for (unsigned x = 0; x < w; x++) {
            u16 p = s[x];
            u32 r = (p >> 11) & 0x1F;
            u32 g = (p >> 5)  & 0x3F;
            u32 b =  p        & 0x1F;
            r = (r << 3) | (r >> 2);
            g = (g << 2) | (g >> 4);
            b = (b << 3) | (b >> 2);
            d[x] = (r << 16) | (g << 8) | b; /* 0x00RRGGBB == BGRX in memory */
        }
    }
}

/* Run-ahead: when set, video and audio callbacks discard their input. The core
   still runs its frame normally; we just don't show it or hear it. Used to
   silently advance the simulation N frames so the displayed frame is N frames
   in the "future" relative to a normal run. */
static int g_av_mute = 0;

/* ---- video / audio / input callbacks -------------------------------------- */
static void me_video_refresh_cb(const void *data, unsigned w, unsigned h, size_t pitch) {
    if (g_av_mute) return;
    g_video_calls++;
    if (!data || !g_back) return;
    if (w > g_back_max_w || h > g_back_max_h) return;
    g_frame_w = w; g_frame_h = h;
    /* HW path: the core drew into our FBO and passes the sentinel
       RETRO_HW_FRAME_BUFFER_VALID. Read pixels back into g_back so the
       existing present() path picks them up. Note GL is bottom-up, so
       the image will appear vertically flipped until Step 5 adds a
       flip-Y to the D3D11 shader (or we flip during the readback). */
    if (data == RETRO_HW_FRAME_BUFFER_VALID) {
        if (!g_hw_render_accepted) return;
        /* Interop path: the core wrote straight into the D3D11 shared
           texture. Nothing for us to do here — present() will sample
           that texture directly via the shared SRV.

           One snag: GL coords are bottom-up but our D3D11 shader assumes
           top-down. Without interop we flip during readback. With interop
           the image would be upside-down. Solution: tell the D3D11 path
           to flip Y when sampling the shared SRV. We can't reuse the
           swap-y trick from software cores because the *sampled texture*
           orientation differs between the two paths, not the destination.
           Handled below in the cbuffer math (see render_d3d11.c). */
        if (me_gl_interop_active()) return;
        /* Readback fallback. Read into a tightly-packed scratch buffer,
           then copy row-flipped into g_back (top-down @ g_back_max_w stride). */
        static unsigned char *scratch = NULL;
        static size_t scratch_cap = 0;
        size_t need = (size_t)w * h * 4;
        if (need > scratch_cap) {
            free(scratch);
            scratch = (unsigned char *)malloc(need);
            scratch_cap = need;
            if (!scratch) { scratch_cap = 0; return; }
        }
        me_gl_fbo_readback_bgra(w, h, scratch);
        for (unsigned y = 0; y < h; y++) {
            const u32 *src = (const u32 *)(scratch + (h - 1 - y) * w * 4);
            u32 *dst = g_back + y * g_back_max_w;
            memcpy(dst, src, (size_t)w * 4);
        }
        return;
    }
    if (g_pixel_format == RETRO_PIXEL_FORMAT_XRGB8888) {
        convert_xrgb8888((const u32 *)data, pitch, w, h);
    } else if (g_pixel_format == RETRO_PIXEL_FORMAT_RGB565) {
        convert_rgb565((const u16 *)data, pitch, w, h);
    }
}
/* 4-point cubic Hermite resampler from core rate -> device rate. State carries
   a 3-sample history (pp, p, n) across calls so the kernel always has 4 points
   available. Output sample position t∈[0,1) is between p and n. */
static unsigned g_core_rate = 0;
static unsigned g_dev_rate  = 0;
static int      g_audio_active = 0; /* 1 once me_audio_init has succeeded */
static double   g_resamp_phase = 0.0; /* 0..1 position between p and n */
static int16_t  g_resamp_pp_l = 0, g_resamp_pp_r = 0; /* sample at t-2 */
static int16_t  g_resamp_p_l  = 0, g_resamp_p_r  = 0; /* sample at t-1 */
static int16_t  g_resamp_n_l  = 0, g_resamp_n_r  = 0; /* sample at t (current right neighbor) */
static int      g_resamp_primed = 0; /* set once we have at least one valid n */
/* Dynamic rate control: integral bias (slow, persistent — tracks the true
   core-vs-device clock offset) plus a proportional bias (small, transient —
   set once per video frame for disturbance rejection). Total |bias| ≤ 0.65%
   = ~11 cents pitch shift, below audibility. Frames are never skipped or
   duplicated to maintain sync. */
static double   g_resamp_ratio_bias = 0.0; /* integral term, persisted */
static double   g_resamp_p_bias     = 0.0; /* proportional, updated per frame */

static inline float hermite4(float pp, float p, float n, float nn, float t) {
    /* Catmull-Rom flavor of 4-point cubic Hermite. */
    float c0 = p;
    float c1 = 0.5f * (n - pp);
    float c2 = pp - 2.5f*p + 2.0f*n - 0.5f*nn;
    float c3 = 0.5f*(nn - pp) + 1.5f*(p - n);
    return ((c3*t + c2)*t + c1)*t + c0;
}

static void resample_and_push(const int16_t *in, size_t in_frames) {
    if (!g_audio_active || !g_dev_rate || !g_core_rate || in_frames == 0) return;
    double step = ((double)g_core_rate / (double)g_dev_rate)
                  * (1.0 + g_resamp_ratio_bias + g_resamp_p_bias);
    enum { CHUNK = 512 };
    int16_t out[CHUNK * 2];
    size_t out_n = 0;
    double phase = g_resamp_phase;
    int16_t pp_l = g_resamp_pp_l, pp_r = g_resamp_pp_r;
    int16_t p_l  = g_resamp_p_l,  p_r  = g_resamp_p_r;
    int16_t n_l  = g_resamp_n_l,  n_r  = g_resamp_n_r;

    /* On the very first call we have no real n yet; seed it from in[0] so the
       loop below has a valid right neighbor before stepping nn forward. */
    size_t i0 = 0;
    if (!g_resamp_primed) {
        n_l = in[0]; n_r = in[1];
        g_resamp_primed = 1;
        i0 = 1; /* in[0] has been absorbed as n */
    }

    for (size_t i = i0; i < in_frames; i++) {
        /* nn = the new sample arriving now; it becomes n after we drain phase. */
        int16_t nn_l = in[i*2 + 0];
        int16_t nn_r = in[i*2 + 1];
        while (phase < 1.0) {
            float t = (float)phase;
            float fl = hermite4((float)pp_l, (float)p_l, (float)n_l, (float)nn_l, t);
            float fr = hermite4((float)pp_r, (float)p_r, (float)n_r, (float)nn_r, t);
            int il = (int)(fl + (fl >= 0.0f ? 0.5f : -0.5f));
            int ir = (int)(fr + (fr >= 0.0f ? 0.5f : -0.5f));
            if (il >  32767) il =  32767; else if (il < -32768) il = -32768;
            if (ir >  32767) ir =  32767; else if (ir < -32768) ir = -32768;
            out[out_n*2 + 0] = (int16_t)il;
            out[out_n*2 + 1] = (int16_t)ir;
            out_n++;
            if (out_n == CHUNK) { me_audio_push(out, out_n); out_n = 0; }
            phase += step;
        }
        phase -= 1.0;
        /* Shift the 4-point window forward by one input sample. */
        pp_l = p_l;  pp_r = p_r;
        p_l  = n_l;  p_r  = n_r;
        n_l  = nn_l; n_r  = nn_r;
    }
    if (out_n) me_audio_push(out, out_n);
    g_resamp_phase = phase;
    g_resamp_pp_l = pp_l; g_resamp_pp_r = pp_r;
    g_resamp_p_l  = p_l;  g_resamp_p_r  = p_r;
    g_resamp_n_l  = n_l;  g_resamp_n_r  = n_r;
}

static void me_audio_sample_cb(int16_t l, int16_t r) {
    if (g_av_mute) return;
    int16_t pair[2] = { l, r };
    resample_and_push(pair, 1);
}
static size_t me_audio_sample_batch_cb(const int16_t *data, size_t frames) {
    if (g_av_mute) return frames;
    resample_and_push(data, frames);
    return frames;
}
/* RetroPad state per player (libretro port), refreshed in input_poll. Each
   poll takes the player's bindings from settings.yaml's maps for the game
   (me_controls_effective: universal, the console's, the core's own); the
   UI thread edits those maps in place under me_settings_lock(). */
static int16_t g_pad[ME_MAX_PLAYERS][16];
/* Input devices (bit per RETRO_DEVICE_*) the core has read, for --env-trace. */
static unsigned g_devices_read;
/* Sticks per player, -32767..32767: left x/y, right x/y. */
static int16_t g_analog[ME_MAX_PLAYERS][4];
enum { AN_LX = 0, AN_LY, AN_RX, AN_RY };

/* The running core's entry in g_settings.cores, or -1. */
static int g_core_index = -1;

static int key_down_kb(const me_kb_binding *b) {
    if (b->vk == 0) return 0;
    if (b->ctrl  && !(GetAsyncKeyState(VK_CONTROL) & 0x8000)) return 0;
    if (b->alt   && !(GetAsyncKeyState(VK_MENU)    & 0x8000)) return 0;
    if (b->shift && !(GetAsyncKeyState(VK_SHIFT)   & 0x8000)) return 0;
    return (GetAsyncKeyState((int)b->vk) & 0x8000) ? 1 : 0;
}

/* Returns 1 if the given VK is claimed by any currently-active hotkey chord.
   A chord is "active" when its modifier keys are all held — at that point the
   main key belongs to the hotkey and must not bleed through to game controls.
   Caller holds the settings lock. */
static int vk_claimed_by_hotkey(unsigned vk) {
    if (vk == 0 || g_settings.hk_source == ME_SRC_CONTROLLER) return 0;
    int got_ctrl  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) ? 1 : 0;
    int got_alt   = (GetAsyncKeyState(VK_MENU)    & 0x8000) ? 1 : 0;
    int got_shift = (GetAsyncKeyState(VK_SHIFT)   & 0x8000) ? 1 : 0;
    for (int i = 0; i < ME_HK_COUNT; i++) {
        const me_kb_bindings *hk = &g_settings.hk[i];
        for (int j = 0; j < hk->count; j++) {
            const me_kb_binding *b = &hk->b[j];
            if (b->vk != vk) continue;
            /* Modifiers match → this chord is active, key is claimed. */
            if (b->ctrl  && !got_ctrl)  continue;
            if (b->alt   && !got_alt)   continue;
            if (b->shift && !got_shift) continue;
            return 1;
        }
    }
    return 0;
}

/* Caller holds the settings lock. */
static int input_down(const me_control_map *map, int player, me_input_id id) {
    /* Advanced inputs have their own bindings (settings.h). */
    if (g_in_layout[player].advanced & (1u << id)) map = &g_settings.advanced[player];
    me_input_source src = g_settings.input_source[player];
    if (src != ME_SRC_CONTROLLER) {
        const me_kb_bindings *bs = &map->keys[id];
        for (int i = 0; i < bs->count; i++) {
            const me_kb_binding *b = &bs->b[i];
            if (vk_claimed_by_hotkey(b->vk)) continue;
            if (key_down_kb(b)) return 1;
        }
    }
    if (src != ME_SRC_KEYBOARD) {
        int slot = g_settings.xi_index[player];
        const me_xi_bindings *xbs = &map->xi[id];
        for (int i = 0; i < xbs->count; i++) {
            if (me_xinput_button(slot, xbs->b[i].buttons)) return 1;
        }
    }
    return 0;
}

/* input_down for inputs player `p`'s controller has; 0 for the rest. */
static unsigned live_inputs(int p) {
    return g_in_layout[p].live | (g_settings.show_advanced_inputs ? g_in_layout[p].advanced : 0);
}

static int live_down(const me_control_map *map, int p, me_input_id id) {
    return (live_inputs(p) & (1u << id)) ? input_down(map, p, id) : 0;
}

/* Caller holds the settings lock. */
static void poll_player(int p) {
    int16_t *pad = g_pad[p];
    me_control_map map;
    const me_input_layout *lay = &g_in_layout[p];
    me_controls_effective(&g_settings, lay, g_core_index, 1, p, &map);
    const me_control_map *m = &map;
    /* Left stick as D-pad (consoles.h): then that is all the left stick
       does, whatever else is bound to its directions. */
    int stick_dpad = me_lstick_as_dpad(&g_settings, lay, 1, p) &&
                     g_settings.input_source[p] != ME_SRC_KEYBOARD;
    if (stick_dpad) {
        const unsigned lstick_dirs = ME_XI_LSTICK_UP | ME_XI_LSTICK_DOWN |
                                     ME_XI_LSTICK_LEFT | ME_XI_LSTICK_RIGHT;
        for (int id = 0; id < ME_IN_COUNT; id++) {
            me_xi_bindings *xb = &map.xi[id];
            int n = 0;
            for (int i = 0; i < xb->count; i++)
                if (!(xb->b[i].buttons & lstick_dirs)) xb->b[n++] = xb->b[i];
            xb->count = n;
        }
    }
    int up    = live_down(m, p, ME_IN_DPAD_UP);
    int down  = live_down(m, p, ME_IN_DPAD_DOWN);
    int lf    = live_down(m, p, ME_IN_DPAD_LEFT);
    int right = live_down(m, p, ME_IN_DPAD_RIGHT);
    /* The stick adds to the D-pad bindings before SOCD, so stick and D-pad
       pushed opposite ways still cancel to neutral. */
    if (stick_dpad) {
        int slot = g_settings.xi_index[p];
        unsigned live = live_inputs(p);
        if (live & (1u << ME_IN_DPAD_UP))    up    |= me_xinput_button(slot, ME_XI_LSTICK_UP);
        if (live & (1u << ME_IN_DPAD_DOWN))  down  |= me_xinput_button(slot, ME_XI_LSTICK_DOWN);
        if (live & (1u << ME_IN_DPAD_LEFT))  lf    |= me_xinput_button(slot, ME_XI_LSTICK_LEFT);
        if (live & (1u << ME_IN_DPAD_RIGHT)) right |= me_xinput_button(slot, ME_XI_LSTICK_RIGHT);
    }
    /* SOCD: opposing directions cancel to neutral. */
    if (up && down)  { up = down = 0; }
    if (lf && right) { lf = right = 0; }
    pad[RETRO_DEVICE_ID_JOYPAD_UP]     = up;
    pad[RETRO_DEVICE_ID_JOYPAD_DOWN]   = down;
    pad[RETRO_DEVICE_ID_JOYPAD_LEFT]   = lf;
    pad[RETRO_DEVICE_ID_JOYPAD_RIGHT]  = right;
    pad[RETRO_DEVICE_ID_JOYPAD_B]      = live_down(m, p, ME_IN_B);
    pad[RETRO_DEVICE_ID_JOYPAD_A]      = live_down(m, p, ME_IN_A);
    pad[RETRO_DEVICE_ID_JOYPAD_Y]      = live_down(m, p, ME_IN_Y);
    pad[RETRO_DEVICE_ID_JOYPAD_X]      = live_down(m, p, ME_IN_X);
    pad[RETRO_DEVICE_ID_JOYPAD_START]  = live_down(m, p, ME_IN_START);
    pad[RETRO_DEVICE_ID_JOYPAD_SELECT] = live_down(m, p, ME_IN_BACK);
    pad[RETRO_DEVICE_ID_JOYPAD_L]      = live_down(m, p, ME_IN_LB);
    pad[RETRO_DEVICE_ID_JOYPAD_R]      = live_down(m, p, ME_IN_RB);
    pad[RETRO_DEVICE_ID_JOYPAD_L2]     = live_down(m, p, ME_IN_LT);
    pad[RETRO_DEVICE_ID_JOYPAD_R2]     = live_down(m, p, ME_IN_RT);
    pad[RETRO_DEVICE_ID_JOYPAD_L3]     = live_down(m, p, ME_IN_LSTICK);
    pad[RETRO_DEVICE_ID_JOYPAD_R3]     = live_down(m, p, ME_IN_RSTICK);

    /* Stick directions are also surfaced through RETRO_DEVICE_ANALOG so cores
       like mupen64plus_next that read the analog stick see motion. Opposing
       directions cancel; non-opposing produce full deflection in that axis.
       Each axis follows the real XInput stick while it is pushed, else its
       digital bindings (keys, or buttons like the N64's C-Left on Xbox Y,
       which must work while the other stick moves). A stick the console
       doesn't have stays centered; an advanced stick (PSP right stick, DS
       touch joystick) moves only through its bindings. The real left stick
       doesn't move it while it works the D-pad. */
    int16_t *an = g_analog[p];
    int slot = g_settings.xi_index[p];
    int use_pad = g_settings.input_source[p] != ME_SRC_KEYBOARD;
    const unsigned lstick = (1u << ME_IN_LSTICK_UP) | (1u << ME_IN_LSTICK_DOWN) |
                            (1u << ME_IN_LSTICK_LEFT) | (1u << ME_IN_LSTICK_RIGHT);
    const unsigned rstick = (1u << ME_IN_RSTICK_UP) | (1u << ME_IN_RSTICK_DOWN) |
                            (1u << ME_IN_RSTICK_LEFT) | (1u << ME_IN_RSTICK_RIGHT);
    int use_l = use_pad && !stick_dpad && (lay->live & lstick);
    int use_r = use_pad && (lay->live & rstick);
    int16_t xi_lx = use_l ? me_xinput_axis(slot, ME_XI_AXIS_LX) : 0;
    int16_t xi_ly = use_l ? me_xinput_axis(slot, ME_XI_AXIS_LY) : 0;
    int16_t xi_rx = use_r ? me_xinput_axis(slot, ME_XI_AXIS_RX) : 0;
    int16_t xi_ry = use_r ? me_xinput_axis(slot, ME_XI_AXIS_RY) : 0;
    int lu = live_down(m, p, ME_IN_LSTICK_UP),    ld = live_down(m, p, ME_IN_LSTICK_DOWN);
    int ll = live_down(m, p, ME_IN_LSTICK_LEFT),  lr = live_down(m, p, ME_IN_LSTICK_RIGHT);
    int ru = live_down(m, p, ME_IN_RSTICK_UP),    rd = live_down(m, p, ME_IN_RSTICK_DOWN);
    int rl = live_down(m, p, ME_IN_RSTICK_LEFT),  rr = live_down(m, p, ME_IN_RSTICK_RIGHT);
    if (lu && ld) lu = ld = 0;
    if (ll && lr) ll = lr = 0;
    if (ru && rd) ru = rd = 0;
    if (rl && rr) rl = rr = 0;
    /* XInput X: right=+32767, matches libretro. No inversion needed.
       XInput Y: up=+32767, but libretro expects down=+32767. Invert Y.
       Clamp -32768 → -32767 before negating to avoid int16_t overflow. */
    #define XI_CLAMP(v) ((v) < -32767 ? (int16_t)-32767 : (v))
    #define DIGITAL(pos, neg) (int16_t)(((pos) ? 32767 : 0) - ((neg) ? 32767 : 0))
    an[AN_LX] = xi_lx ?  XI_CLAMP(xi_lx) : DIGITAL(lr, ll);
    an[AN_LY] = xi_ly ? -XI_CLAMP(xi_ly) : DIGITAL(ld, lu);
    an[AN_RX] = xi_rx ?  XI_CLAMP(xi_rx) : DIGITAL(rr, rl);
    an[AN_RY] = xi_ry ? -XI_CLAMP(xi_ry) : DIGITAL(rd, ru);
    #undef XI_CLAMP
    #undef DIGITAL
}

/* Where one screen of a two-screen console is in the core's (fw x fh)
   frame. Cores draw its screens, the same height, top screen first: one
   above the other (maybe with a gap between, a narrower bottom screen
   centered) or side by side. The DS's are both 4:3; the 3DS's top screen
   is 5:3. Returns 0 and leaves the rect alone for other consoles and
   layouts (hybrid, large screen, rotated). */
static int find_screen(unsigned fw, unsigned fh, int bottom,
                       unsigned *x, unsigned *y, unsigned *w, unsigned *h) {
    const me_console *c = me_console_at(g_console);
    if (!c || !(c->flags & ME_CONSOLE_TWO_SCREENS)) return 0;
    unsigned tn = (c->flags & ME_CONSOLE_WIDE_TOP) ? 5 : 4;  /* top screen tn:3 */
    if ((fw * 3) % tn == 0 && fw * 3 / tn * 2 <= fh) {        /* stacked */
        *h = fw * 3 / tn;
        *w = bottom ? *h * 4 / 3 : fw;
        *x = (fw - *w) / 2;
        *y = bottom ? fh - *h : 0;
        return 1;
    }
    if ((fh * tn) % 3 == 0 && (fh * 4) % 3 == 0 &&
        fh * tn / 3 + fh * 4 / 3 <= fw) {                      /* side by side */
        *y = 0;
        *h = fh;
        *w = fh * (bottom ? 4 : tn) / 3;
        *x = bottom ? fw - *w : 0;
        return 1;
    }
    return 0;
}

/* What present() last drew: the (sx, sy, sw, sh) part of the core's
   (fw x fh) frame, scaled into the client-area rect (dx, dy, dw, dh). The
   touch screen maps the cursor back through it. Emulation thread only. */
static struct {
    int valid;
    int dx, dy, dw, dh;
    unsigned sx, sy, sw, sh, fw, fh;
} g_view;

/* Touch screen: the mouse over the game image, for consoles that have one
   (and unknown ones, which get every input). Cores read it as
   RETRO_DEVICE_POINTER, absolute across the core's whole frame so the core
   maps it through its own screen layout, or as RETRO_DEVICE_MOUSE, relative
   in frame pixels. A mouse core keeps its own cursor on the touch screen, so
   the mouse moves as if clamped to it (the bottom screen of a two-screen
   console, else the image shown): once pushed against an edge the core's
   cursor lines up with the real one. */
static struct {
    int      inside;     /* cursor over the image */
    unsigned buttons;    /* 1 left, 2 right, 4 middle; only while inside */
    int16_t  x, y;       /* pointer, -0x7fff..0x7fff */
    int      px, py;     /* cursor in frame pixels */
    int      primed;     /* px/py hold a previous position */
    int16_t  mx, my;     /* mouse motion since the last poll */
} g_touch;

/* v clamped to the pixels [start, start + len). */
static double clampd(double v, unsigned start, unsigned len) {
    if (v < start) return start;
    if (v > start + len - 0.5) return start + len - 0.5;
    return v;
}

static int console_has_touch(void) {
    const me_console *c = me_console_at(g_console);
    return !c || (c->flags & ME_CONSOLE_TOUCH);
}

static void poll_touch(void) {
    unsigned buttons = me_platform_mouse_buttons();  /* taken every poll so old clicks don't linger */
    int cx, cy;
    g_touch.mx = g_touch.my = 0;
    g_touch.inside = 0;
    g_touch.buttons = 0;
    if (!console_has_touch() || !g_view.valid || !me_platform_cursor_pos(&cx, &cy)) return;

    /* The touch screen: the bottom screen of a two-screen console, else the
       image shown. */
    unsigned tx = g_view.sx, ty = g_view.sy, tw = g_view.sw, th = g_view.sh;
    find_screen(g_view.fw, g_view.fh, 1, &tx, &ty, &tw, &th);

    /* Touch screen hidden (View > Screen > Top Screen): nothing to touch.
       Park the pointer mid-screen, where the core's cursor can't show at
       the edge of the screen that is shown, and hold the mouse still. */
    if (tx >= g_view.sx + g_view.sw || g_view.sx >= tx + tw ||
        ty >= g_view.sy + g_view.sh || g_view.sy >= ty + th) {
        g_touch.x = (int16_t)((int)((tx + tw / 2.0) * 65534.0 / g_view.fw) - 32767);
        g_touch.y = (int16_t)((int)((ty + th / 2.0) * 65534.0 / g_view.fh) - 32767);
        return;
    }

    g_touch.inside = cx >= g_view.dx && cx < g_view.dx + g_view.dw &&
                     cy >= g_view.dy && cy < g_view.dy + g_view.dh;
    if (g_touch.inside) g_touch.buttons = buttons;

    /* Client pixel -> frame pixel. The pointer stays on the part of the
       frame shown. */
    double fx = g_view.sx + (cx - g_view.dx + 0.5) * g_view.sw / g_view.dw;
    double fy = g_view.sy + (cy - g_view.dy + 0.5) * g_view.sh / g_view.dh;
    g_touch.x = (int16_t)((int)(clampd(fx, g_view.sx, g_view.sw) * 65534.0 / g_view.fw) - 32767);
    g_touch.y = (int16_t)((int)(clampd(fy, g_view.sy, g_view.sh) * 65534.0 / g_view.fh) - 32767);

    int px = (int)clampd(fx, tx, tw), py = (int)clampd(fy, ty, th);
    if (g_touch.primed) {
        g_touch.mx = (int16_t)(px - g_touch.px);
        g_touch.my = (int16_t)(py - g_touch.py);
    }
    g_touch.px = px;
    g_touch.py = py;
    g_touch.primed = 1;
}

static int16_t touch_state(unsigned port, unsigned device, unsigned index, unsigned id) {
    if (port != 0) return 0;
    if (device == RETRO_DEVICE_POINTER) {
        if (index != 0) return 0;  /* one finger */
        switch (id) {
            case RETRO_DEVICE_ID_POINTER_X:            return g_touch.x;
            case RETRO_DEVICE_ID_POINTER_Y:            return g_touch.y;
            case RETRO_DEVICE_ID_POINTER_PRESSED:      return (g_touch.buttons & 1) ? 1 : 0;
            case RETRO_DEVICE_ID_POINTER_COUNT:        return (g_touch.buttons & 1) ? 1 : 0;
            case RETRO_DEVICE_ID_POINTER_IS_OFFSCREEN: return g_touch.inside ? 0 : 1;
        }
        return 0;
    }
    switch (id) {  /* RETRO_DEVICE_MOUSE */
        case RETRO_DEVICE_ID_MOUSE_X:      return g_touch.mx;
        case RETRO_DEVICE_ID_MOUSE_Y:      return g_touch.my;
        case RETRO_DEVICE_ID_MOUSE_LEFT:   return (g_touch.buttons & 1) ? 1 : 0;
        case RETRO_DEVICE_ID_MOUSE_RIGHT:  return (g_touch.buttons & 2) ? 1 : 0;
        case RETRO_DEVICE_ID_MOUSE_MIDDLE: return (g_touch.buttons & 4) ? 1 : 0;
    }
    return 0;
}

static void me_input_poll_cb(void) {
    if (g_latency_log) QueryPerformanceCounter(&g_poll_qpc);
    /* Don't read keys when our window isn't foreground. */
    if (GetForegroundWindow() != g_hwnd) {
        memset(g_pad, 0, sizeof(g_pad));
        memset(g_analog, 0, sizeof(g_analog));
        g_touch.inside = 0;
        g_touch.buttons = 0;
        g_touch.mx = g_touch.my = 0;
        me_platform_mouse_buttons();
        return;
    }
    poll_touch();
    me_settings_lock();
    /* Each distinct pad slot once, for the players the game has. */
    for (int p = 0; p < g_players; p++) {
        int slot = g_settings.xi_index[p], seen = 0;
        for (int q = 0; q < p; q++) seen |= (g_settings.xi_index[q] == slot);
        if (!seen && g_settings.input_source[p] != ME_SRC_KEYBOARD) me_xinput_poll(slot);
    }
    for (int p = 0; p < g_players; p++) {
        poll_player(p);
        rumble_mute(p);
    }
    me_settings_unlock();
}

static int16_t me_input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id) {
    /* Ports the console doesn't have (without its adapter) stay empty. */
    if (port >= (unsigned)g_players) return 0;
    /* A subclass (a multitap, a Wii Remote...) reads like its base device. */
    unsigned base = device & RETRO_DEVICE_MASK;
    if (g_env_trace && base < 32 && !(g_devices_read & (1u << base))) {
        g_devices_read |= 1u << base;
        me_log(ME_LOG_ENV, "[input] core reads device %u (0x%x, port %u)\n", base, device, port);
    }
    const unsigned n_ids = sizeof(g_pad[0]) / sizeof(g_pad[0][0]);
    if (base == RETRO_DEVICE_POINTER || base == RETRO_DEVICE_MOUSE)
        return touch_state(port, base, index, id);
    if (base == RETRO_DEVICE_JOYPAD) {
        if (id == RETRO_DEVICE_ID_JOYPAD_MASK) {
            int16_t mask = 0;
            for (unsigned i = 0; i < n_ids; i++) if (g_pad[port][i]) mask |= (int16_t)(1 << i);
            return mask;
        }
        return id < n_ids ? g_pad[port][id] : 0;
    }
    if (base == RETRO_DEVICE_ANALOG) {
        if (index == RETRO_DEVICE_INDEX_ANALOG_LEFT) {
            if (id == RETRO_DEVICE_ID_ANALOG_X) return g_analog[port][AN_LX];
            if (id == RETRO_DEVICE_ID_ANALOG_Y) return g_analog[port][AN_LY];
        } else if (index == RETRO_DEVICE_INDEX_ANALOG_RIGHT) {
            if (id == RETRO_DEVICE_ID_ANALOG_X) return g_analog[port][AN_RX];
            if (id == RETRO_DEVICE_ID_ANALOG_Y) return g_analog[port][AN_RY];
        } else if (index == RETRO_DEVICE_INDEX_ANALOG_BUTTON) {
            /* How far a button is pressed (analog triggers): the bound
               input is on or off, so all the way or not at all. */
            return id < n_ids && g_pad[port][id] ? 0x7fff : 0;
        }
        return 0;
    }
    return 0;
}

/* ---- present -------------------------------------------------------------- */
/* The part of the core's (fw x fh) frame View > Screen shows; the whole
   frame unless it picks one screen of a layout find_screen knows. */
static void view_source(unsigned fw, unsigned fh,
                        unsigned *sx, unsigned *sy, unsigned *sw, unsigned *sh) {
    *sx = *sy = 0;
    *sw = fw;
    *sh = fh;
    me_screens which = g_settings.screens;
    if (which != ME_SCREENS_BOTH) find_screen(fw, fh, which == ME_SCREENS_BOTTOM, sx, sy, sw, sh);
}

/* How big the (sw x sh) part of the frame is drawn in a (cw x ch) client
   area. Square pixels and the forced 4:3 / 16:9 scale by whole numbers, so
   every source pixel is the same size. Auto shows the core's shape: by
   whole numbers when its pixels are square, else exactly, since a frame
   like the Wii's 640x528 has too few steps for whole numbers to come near
   4:3 (2x2 in a 1080p window is still square). A frame bigger than the
   window, or whole numbers that overshoot it, is fitted exactly too. */
static void view_size(int cw, int ch, unsigned sw, unsigned sh, int *dw, int *dh) {
    int aspect = (unsigned)g_settings.aspect < ME_ASPECT_COUNT ? (int)g_settings.aspect
                                                               : ME_ASPECT_AUTO;
    double want = (double)sw / sh;   /* the picture's width over its height */
    int square = aspect == ME_ASPECT_1_1;
    if (aspect == ME_ASPECT_AUTO) {
        /* The core's shape is the whole frame's; a part of it (View >
           Screen) keeps the frame's pixel shape. */
        if (g_core_aspect > 0.0f && g_frame_w && g_frame_h)
            want = g_core_aspect * ((double)sw / g_frame_w) / ((double)sh / g_frame_h);
        square = fabs(want * sh / sw - 1.0) < 0.01;
    } else if (!square) {
        want = (double)g_aspect_x[aspect] / g_aspect_y[aspect];
    }

    if ((int)sw <= cw && (int)sh <= ch && (square || aspect != ME_ASPECT_AUTO)) {
        i32 rx, ry;
        if (square)
            rx = ry = me_iscale_ratio(cw, ch, (i32)sw, (i32)sh);
        else
            me_iscale_ratios(cw, ch, (i32)sw, (i32)sh,
                             g_aspect_x[aspect], g_aspect_y[aspect], &rx, &ry);
        *dw = (int)sw * rx;
        *dh = (int)sh * ry;
        if (*dw <= cw && *dh <= ch) return;
    }
    if (cw < ch * want) {
        *dw = cw;
        *dh = (int)(cw / want + 0.5);
    } else {
        *dh = ch;
        *dw = (int)(ch * want + 0.5);
    }
    if (*dw < 1) *dw = 1;
    if (*dh < 1) *dh = 1;
}

static void present(HWND hwnd) {
    g_view.valid = 0;
    if (!g_back || g_frame_w == 0 || g_frame_h == 0) return;

    RECT cr;
    GetClientRect(hwnd, &cr);
    int cw = cr.right - cr.left;
    int ch = cr.bottom - cr.top;
    if (cw <= 0 || ch <= 0) return;

    /* The GL interop path samples the core's own texture, which can't be
       cropped here; it always shows the whole frame. */
    unsigned sx = 0, sy = 0, sw = g_frame_w, sh = g_frame_h;
    if (!(g_hw_render_accepted && me_gl_interop_active()))
        view_source(g_frame_w, g_frame_h, &sx, &sy, &sw, &sh);
    const u32 *src = g_back + (size_t)sy * g_back_max_w + sx;

    int dw, dh;
    view_size(cw, ch, sw, sh, &dw, &dh);
    int dx = (cw - dw) / 2;
    int dy = (ch - dh) / 2;

    g_view.dx = dx; g_view.dy = dy; g_view.dw = dw; g_view.dh = dh;
    g_view.sx = sx; g_view.sy = sy; g_view.sw = sw; g_view.sh = sh;
    g_view.fw = g_frame_w; g_view.fh = g_frame_h;
    g_view.valid = 1;

    /* Vulkan present path: upload the active region of the BGRX backbuffer
       and draw a textured quad sized to the integer-scaled rect. */
    if (me_vk_is_active()) {
        me_vk_present(src, sw, sh, g_back_max_w,
                      cw, ch, dx, dy, dw, dh);
        return;
    }

    /* D3D11 flip-model path: HW (GL) cores always, software cores when the
       user opts in via --d3d11. Software cores default to GDI because DWM
       composites it at the desktop refresh — no visible tearing on a
       non-VRR display, and frame-perfect inputs land as expected. */
    if (g_use_d3d11) {
        me_d3d11_upload(src, sw, sh, g_back_max_w);
        me_d3d11_present(cw, ch, dx, dy, dw, dh, sw, sh);
        return;
    }

    HDC hdc = GetDC(hwnd);
    if (!hdc) return;

    /* Black bars around the image. */
    HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
    if (dy > 0)       { RECT r = {0, 0, cw, dy};                   FillRect(hdc, &r, black); }
    if (dy + dh < ch) { RECT r = {0, dy + dh, cw, ch};             FillRect(hdc, &r, black); }
    if (dx > 0)       { RECT r = {0, dy, dx, dy + dh};             FillRect(hdc, &r, black); }
    if (dx + dw < cw) { RECT r = {dx + dw, dy, cw, dy + dh};       FillRect(hdc, &r, black); }

    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = (LONG)sw;
    bmi.bmiHeader.biHeight      = -(LONG)sh;
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    /* Tightly pack the active region (StretchDIBits with src rect smaller
       than DIB width is unreliable on some GDI paths). */
    static u32 *tight = NULL;
    static size_t tight_cap = 0;
    size_t need = (size_t)sw * sh;
    if (need > tight_cap) {
        free(tight);
        tight = (u32 *)malloc(need * sizeof(u32));
        tight_cap = need;
    }
    if (tight) {
        for (unsigned y = 0; y < sh; y++) {
            memcpy(tight + y * sw, src + y * g_back_max_w, sw * sizeof(u32));
        }
        SetStretchBltMode(hdc, COLORONCOLOR);
        StretchDIBits(hdc,
                      dx, dy, dw, dh,
                      0, 0, (int)sw, (int)sh,
                      tight, &bmi, DIB_RGB_COLORS, SRCCOPY);
    }

    ReleaseDC(hwnd, hdc);
}

/* ---- file slurp ----------------------------------------------------------- */
/* Extract the basename (no directory, no extension) from a path into out.
   out must hold at least MAX_PATH bytes. */
static void me_basename_noext(const char *path, char *out, size_t out_sz) {
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '\\' || *p == '/') base = p + 1;
    }
    size_t n = 0;
    while (base[n] && base[n] != '.' && n + 1 < out_sz) {
        out[n] = base[n];
        n++;
    }
    /* If the file has multiple dots, the above stops at the first one. That's
       fine for our purposes — "Super Mario Bros. (World).nes" becomes
       "Super Mario Bros", which is unique enough per core. */
    out[n] = '\0';
}

/* Build saves\<core>\<rom>.srm. Returns 0 on success. Creates the per-core
   subdirectory if missing. */
static int me_build_save_path(const char *core_path, const char *rom_path,
                              char *out, size_t out_sz) {
    char core_base[MAX_PATH], rom_base[MAX_PATH];
    me_basename_noext(core_path, core_base, sizeof(core_base));
    me_basename_noext(rom_path,  rom_base,  sizeof(rom_base));
    if (!core_base[0] || !rom_base[0]) return -1;

    char dir[MAX_PATH];
    int n = snprintf(dir, sizeof(dir), "%ssaves\\%s", g_exedir, core_base);
    if (n < 0 || (size_t)n >= sizeof(dir)) return -1;
    char saves_base[MAX_PATH];
    exepath(saves_base, sizeof(saves_base), "saves");
    CreateDirectoryA(saves_base, NULL);
    CreateDirectoryA(dir, NULL);

    n = snprintf(out, out_sz, "%s\\%s.srm", dir, rom_base);
    if (n < 0 || (size_t)n >= out_sz) return -1;
    return 0;
}

/* FNV-1a 64-bit. SRAM blocks are small (8KB–128KB typical) so a non-SIMD hash
   is plenty fast — sub-100µs even at 128KB. */
static uint64_t me_fnv1a64(const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

/* Load SRAM from disk into the core's save-RAM buffer. Silently no-ops if the
   core exposes no save-RAM or the file doesn't exist yet (first-time launch). */
static void me_sram_load(me_core *core, const char *save_path) {
    if (!core->retro_get_memory_data || !core->retro_get_memory_size) return;
    void *mem = core->retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    size_t sz = core->retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (!mem || sz == 0) {
        printf("[save] core has no save-RAM\n");
        return;
    }
    FILE *f = fopen(save_path, "rb");
    if (!f) {
        printf("[save] no existing save at %s (new game)\n", save_path);
        return;
    }
    size_t got = fread(mem, 1, sz, f);
    fclose(f);
    printf("[save] loaded %zu/%zu bytes from %s\n", got, sz, save_path);
}

/* Write SRAM to disk. Writes to a .tmp file and renames to avoid leaving a
   half-written .srm if we die mid-write. */
static void me_sram_save(me_core *core, const char *save_path) {
    if (!core->retro_get_memory_data || !core->retro_get_memory_size) return;
    void *mem = core->retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    size_t sz = core->retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (!mem || sz == 0) return;

    char tmp[MAX_PATH];
    if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp", save_path) >= sizeof(tmp)) return;
    FILE *f = fopen(tmp, "wb");
    if (!f) { fprintf(stderr, "[save] open failed: %s\n", tmp); return; }
    size_t wrote = fwrite(mem, 1, sz, f);
    fclose(f);
    if (wrote != sz) { fprintf(stderr, "[save] short write: %zu/%zu\n", wrote, sz); return; }
    /* MoveFileEx with REPLACE_EXISTING is the atomic swap on Windows. */
    if (!MoveFileExA(tmp, save_path, MOVEFILE_REPLACE_EXISTING)) {
        fprintf(stderr, "[save] rename failed (err=%lu)\n", GetLastError());
        return;
    }
    printf("[save] wrote %zu bytes to %s\n", sz, save_path);
}

static unsigned char *slurp(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    unsigned char *buf = (unsigned char *)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(buf); return NULL; }
    *out_size = (size_t)sz;
    return buf;
}

/* Query the OS for CPU set information to split P-cores from E-cores.
   Returns 1 if hybrid topology was detected and masks were filled.
   On any failure (pre-Win10, homogeneous CPU, API not present) returns 0.

   The Win32 SYSTEM_CPU_SET_INFORMATION type exposes an EfficiencyClass field:
   0 = E-core (efficiency), higher = P-core (performance). We collect the two
   highest distinct classes and assign the top class to emu_mask and a secondary
   core from the top class (or falling back to the next class) to audio_mask. */
static int me_pick_affinity_masks(DWORD_PTR *emu_mask, DWORD_PTR *audio_mask) {
    /* Dynamically resolve — not available on Win7/8. */
    typedef BOOL (WINAPI *PFN_GetSystemCpuSetInformation)(
        void *, ULONG, PULONG, HANDLE, ULONG);
    HMODULE kern = GetModuleHandleA("kernel32.dll");
    if (!kern) return 0;
    PFN_GetSystemCpuSetInformation pfn = (PFN_GetSystemCpuSetInformation)
        GetProcAddress(kern, "GetSystemCpuSetInformation");
    if (!pfn) return 0;

    ULONG needed = 0;
    pfn(NULL, 0, &needed, GetCurrentProcess(), 0);
    if (!needed) return 0;
    BYTE *buf = (BYTE *)malloc(needed);
    if (!buf) return 0;
    if (!pfn(buf, needed, &needed, GetCurrentProcess(), 0)) { free(buf); return 0; }

    /* Walk entries, collecting per-EfficiencyClass masks.
       Intel hybrid: P-cores have class > 0, E-cores have class 0.
       AMD / homogeneous Intel: all cores have class 0. */
    DWORD_PTR class_mask[256] = {0};
    BYTE max_class = 0;
    int  num_lp = 0;
    ULONG offset = 0;
    while (offset < needed) {
        DWORD *entry = (DWORD *)(buf + offset);
        DWORD  sz    = entry[0];
        DWORD  type  = entry[1];
        if (sz < 8 || offset + sz > needed) break;
        if (type == 0 /* CpuSet */) {
            /* Layout after Size+Type (8 bytes):
               Id(4), Group(2), LogicalProcessorIndex(1), CoreIndex(1),
               LastLevelCacheIndex(1), NumaNodeIndex(1), EfficiencyClass(1) */
            BYTE *cs       = (BYTE *)(buf + offset + 8);
            BYTE lp_idx    = cs[6];
            BYTE eff_class = cs[10];
            if (lp_idx < 64) { /* DWORD_PTR is 64-bit on x64 */
                class_mask[eff_class] |= (DWORD_PTR)1 << lp_idx;
                if (eff_class > max_class) max_class = eff_class;
                num_lp++;
            }
        }
        offset += sz;
    }
    free(buf);

    if (num_lp < 2) return 0;

    if (max_class > 0) {
        /* Intel hybrid: P-cores are in max_class, E-cores in lower classes. */
        DWORD_PTR pcores = class_mask[max_class];
        DWORD_PTR ecores = 0;
        for (int c = 0; c < max_class; c++) ecores |= class_mask[c];

        /* Emu gets all P-cores. Audio gets one spare P-core if >=2 exist,
           otherwise one E-core. */
        DWORD_PTR lo = pcores & (DWORD_PTR)(-(DWORD_PTR)pcores);
        DWORD_PTR rest_pcores = pcores & ~lo;
        if (rest_pcores) {
            *emu_mask   = pcores;
            *audio_mask = rest_pcores & (DWORD_PTR)(-(DWORD_PTR)rest_pcores);
        } else if (ecores) {
            *emu_mask   = pcores;
            *audio_mask = ecores & (DWORD_PTR)(-(DWORD_PTR)ecores);
        } else {
            return 0;
        }
    } else {
        /* Homogeneous CPU (AMD, or all-E Intel): split by logical index.
           Give emu the lower half of cores, audio one core from the upper half.
           This keeps them on different physical cores / CCDs. */
        DWORD_PTR all = class_mask[0];
        int count = 0;
        for (DWORD_PTR m = all; m; m &= m - 1) count++;
        if (count < 2) return 0;

        /* Lower half → emu; pick one bit from upper half → audio. */
        DWORD_PTR emu = 0, upper = 0;
        int seen = 0, half = count / 2;
        for (int b = 0; b < 64; b++) {
            if (!((all >> b) & 1)) continue;
            if (seen < half) emu |= (DWORD_PTR)1 << b;
            else             upper |= (DWORD_PTR)1 << b;
            seen++;
        }
        *emu_mask   = emu;
        *audio_mask = upper & (DWORD_PTR)(-(DWORD_PTR)upper); /* lowest bit of upper half */
    }
    return 1;
}

static DWORD g_main_tid;   /* the emulation thread (main's) */

static LONG WINAPI me_unhandled_exception(EXCEPTION_POINTERS *ep) {
    DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    void *addr = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : NULL;
    /* Figure out which module the faulting address is in. Helps tell apart
       a frontend bug from a core bug. */
    HMODULE mod = NULL;
    char modname[MAX_PATH] = "?";
    if (addr && GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)addr, &mod) && mod) {
        GetModuleFileNameA(mod, modname, sizeof(modname));
    }
    uintptr_t off = mod ? (uintptr_t)addr - (uintptr_t)mod : 0;
    /* Which thread: ours, or one a core started (its render thread...). */
    DWORD tid = GetCurrentThreadId();
    char line[MAX_PATH + 160];
    int n = snprintf(line, sizeof(line),
                     "[crash] unhandled exception 0x%08lx at %p (%s+0x%llx) on %s thread %lu\n",
                     (unsigned long)code, addr, modname, (unsigned long long)off,
                     tid == g_main_tid ? "the emulation" : "another", (unsigned long)tid);
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof(line)) n = (int)sizeof(line) - 1;
    fputs(line, stderr);
    fflush(stderr);
    /* Also in logs\crash.log, for games opened without a console window.
       Plain Win32 calls: the heap may be what broke. */
    char path[MAX_PATH + 16];
    snprintf(path, sizeof(path), "%s\\crash.log", g_log_dir);
    CreateDirectoryA(g_log_dir, NULL);
    HANDLE f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        SYSTEMTIME t; GetLocalTime(&t);
        char when[40];
        int wn = snprintf(when, sizeof(when), "%04u-%02u-%02u %02u:%02u:%02u ",
                          t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        DWORD wrote;
        if (wn > 0) WriteFile(f, when, (DWORD)wn, &wrote, NULL);
        WriteFile(f, line, (DWORD)n, &wrote, NULL);
        CloseHandle(f);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

/* Options resolved from settings.yaml + CLI flags; read each time a game is
   loaded. */
static int g_no_audio    = 0;
static int g_force_gdi   = 0;
static int g_force_d3d11 = 0;
static int g_pace_log    = 0;
static int g_timing_log  = 0;

/* ---- game session ----------------------------------------------------------
   Everything tied to one loaded core + ROM. The window outlives sessions:
   dropping a ROM onto it closes the current session (if any) and opens a
   new one. Renderers and audio are sized to the core's AV info, so they are
   torn down and rebuilt per session too. */
typedef struct me_session {
    int      active;          /* game loaded, frames running */
    me_core *core;
    int      core_inited;     /* retro_init called */
    int      game_loaded;     /* retro_load_game succeeded */
    int      hw_context_live; /* context_reset called on a HW core */
    unsigned base_w, base_h;  /* AV base geometry (LSFG frame size) */
    char     core_path[MAX_PATH];
    char     rom_path[MAX_PATH];
    int      console;         /* rom_cores.h index, or -1 if unknown */
    unsigned char *rom_data;
    char     save_path[MAX_PATH];

    /* SRAM dirty-poll state. We hash SRAM every ~1s and, once the hash
       stabilizes for one additional poll, flush to disk. The stability check
       avoids writing mid-update when the core is still mutating the buffer
       (e.g. a multi-byte checksum being recomputed by the cart). */
    uint64_t sram_disk_hash;  /* hash of last bytes we wrote to disk */
    uint64_t sram_last_hash;  /* hash from previous poll tick */
    int      sram_pending;    /* hash changed; waiting for it to settle */
    DWORD    sram_last_poll_ms;

    int    audio_ok;
    size_t target_buffered;   /* DRC ring-fill target, in device frames */
    size_t fill_hist[60];     /* 60-frame moving average of ring fill */
    int    fill_hist_idx, fill_hist_filled;

    double        frame_period_ms;
    LARGE_INTEGER qpf, qstart;
    unsigned long frame_count;
    /* Frame pace_base_frame is due pace_base_ms after qstart, and each one
       after it frame_period_ms later: both move when the core's rate
       changes (SET_SYSTEM_AV_INFO), so no deadline jumps. */
    double        pace_base_ms;
    unsigned long pace_base_frame;

    int    ra_frames;
    size_t ra_state_size;
    void  *ra_state_buf;

    /* Per-second rollup for --pace-log. */
    double   pl_gap_min, pl_gap_max, pl_gap_sum;
    size_t   pl_fill_before_min, pl_fill_before_max, pl_fill_before_sum;
    size_t   pl_fill_after_min,  pl_fill_after_max,  pl_fill_after_sum;
    unsigned pl_iters;
    unsigned pl_resyncs;      /* stalls that re-based the pace schedule */
    LARGE_INTEGER pl_last_qpc, pl_window_start;

    /* Per-second rollup for --latency-log. Each iteration is split into
         backpressure_wait (DXGI waitable)
         pre_poll          (top of retro_run until the core polls input)
         post_poll         (rest of retro_run after the poll callback)
         present           (frame upload + Present)
         pace_wait         (Sleep + spin to absolute QPC deadline)
       Sums and a frame-N marker are printed each second. Hot-path cost
       when disabled: one branch in me_input_poll_cb and at each
       checkpoint — negligible. */
    double   ll_pre_sum, ll_post_sum, ll_present_sum, ll_pace_sum, ll_back_sum;
    double   ll_iter_max;
    unsigned ll_iters, ll_missed_polls;
    LARGE_INTEGER ll_window_start;
    unsigned long ll_window_first_frame;
} me_session;

/* Return callback-side globals to their pre-load state so the next core
   starts clean. */
static void session_reset_globals(void) {
    g_pixel_format = RETRO_PIXEL_FORMAT_0RGB1555;
    g_back = NULL;
    g_back_max_w = g_back_max_h = 0;
    g_av_fps = g_av_rate = 0.0;
    g_av_timing_changed = g_av_refused = 0;
    g_frame_w = g_frame_h = 0;
    g_core_aspect = 0.0f;
    g_video_calls = 0;
    g_use_d3d11 = 0;
    memset(&g_hw_render, 0, sizeof(g_hw_render));
    g_hw_render_requested = g_hw_render_accepted = 0;
    me_vars_set(NULL);
    memset(g_env_seen, 0, sizeof(g_env_seen));
    g_av_mute = 0;
    g_core_rate = g_dev_rate = 0;
    g_audio_active = 0;
    g_resamp_phase = 0.0;
    g_resamp_pp_l = g_resamp_pp_r = 0;
    g_resamp_p_l  = g_resamp_p_r  = 0;
    g_resamp_n_l  = g_resamp_n_r  = 0;
    g_resamp_primed = 0;
    g_resamp_ratio_bias = g_resamp_p_bias = 0.0;
    for (int p = 0; p < ME_MAX_PLAYERS; p++) me_layout_unknown(&g_in_layout[p]);
    g_core_name[0] = '\0';
    memset(g_ci_n, 0, sizeof(g_ci_n));
    g_ci_ports = 0;
    g_console = -1;
    g_players = ME_MAX_PLAYERS;
    g_adapter = NULL;
    g_adapter_ports = 0;
    memset(g_controller, 0, sizeof(g_controller));
    g_core_index = -1;
    memset(g_pad, 0, sizeof(g_pad));
    memset(g_analog, 0, sizeof(g_analog));
    g_devices_read = 0;
    memset(&g_view, 0, sizeof(g_view));
    memset(&g_touch, 0, sizeof(g_touch));
}

/* Tear down a session. Safe on a partially opened one (session_open's
   failure path) and on an empty one. Leaves the window up and idle. */
static void session_close(me_session *s) {
    me_core *core = s->core;
    if (s->audio_ok) me_audio_shutdown();
    me_vk_lsfg_shutdown();   /* B3: must come before me_vk_shutdown */
    me_vk_shutdown();

    if (s->hw_context_live && g_hw_render.context_destroy) {
        g_hw_render.context_destroy();
    }
    free(s->ra_state_buf);
    if (s->game_loaded) {
        if (s->save_path[0]) me_sram_save(core, s->save_path);
        core->retro_unload_game();
    }
    if (s->core_inited) core->retro_deinit();
    rumble_stop();
    /* GL before D3D11: GL interop holds a registration on the D3D11 device. */
    me_gl_shutdown();
    me_d3d11_shutdown();
    free(s->rom_data);
    free(g_back);
    me_core_unload(core);

    session_reset_globals();
    memset(s, 0, sizeof(*s));
    if (g_hwnd) me_platform_set_idle(g_hwnd, 1);
}

static int create_main_window(int w, int h) {
    g_hwnd = me_platform_create_window("laggueless", w, h);
    if (!g_hwnd) { fprintf(stderr, "window create failed\n"); return -1; }
    if (g_settings.fullscreen_on_launch) {
        me_platform_toggle_fullscreen(g_hwnd);
    }
    return 0;
}

/* The console the player picked for this ROM before (settings.yaml
   console_picks), or -1. Only ROMs whose type several consoles use have one;
   for the rest, nothing is read. */
static int remembered_console(const char *rom_path) {
    me_console_set candidates = me_rom_candidates(rom_path);
    char fp[17];
    if (!candidates || !(candidates & (candidates - 1)) ||
        !me_rom_fingerprint(rom_path, fp, sizeof(fp)))
        return -1;
    int console = -1;
    me_settings_lock();   /* the UI thread writes the picks */
    const char *id = me_settings_console_pick(&g_settings, fp);
    for (int i = 0; id && i < me_console_count(); i++)
        if ((candidates >> i & 1) && _stricmp(me_console_at(i)->id, id) == 0) console = i;
    me_settings_unlock();
    return console;
}

/* Once a ROM has opened as `console` by the player's pick (now or before),
   remember it as their most recent; or forget it where its header decides
   the same anyway, so settings.yaml keeps only picks that matter. */
static void remember_console(const char *rom_path, int console) {
    char fp[17];
    const me_console *c = me_console_at(console);
    if (!c || !me_rom_fingerprint(rom_path, fp, sizeof(fp))) return;
    me_ui_notify_console_pick(fp, me_console_for_rom(rom_path, NULL) == console ? "" : c->id);
}

/* Work out a ROM's console and its core (Cores > Set Cores, else our pick)
   as cores\<dll> next to the exe. `*console` is the console the player
   picked, or -1 to go by the ROM's extension and header. Returns 0 with
   *console and core_path filled; ME_ROM_AMBIGUOUS when the player has to pick
   the console from *candidates; otherwise -1 with a user-facing reason in
   `err`. */
static int resolve_rom_core(const char *rom_path, int *console, me_console_set *candidates,
                            char *core_path, size_t core_path_sz, char *err, size_t err_sz) {
    if (GetFileAttributesA(rom_path) == INVALID_FILE_ATTRIBUTES) {
        snprintf(err, err_sz, "ROM not found:\n%s", rom_path);
        return -1;
    }
    const me_console *c = me_console_at(*console);
    if (!c) {
        *console = me_console_for_rom(rom_path, candidates);
        if (*console == ME_ROM_AMBIGUOUS) return ME_ROM_AMBIGUOUS;
        c = me_console_at(*console);
        if (!c) {
            snprintf(err, err_sz, "No core is assigned to this file type:\n%s", rom_path);
            return -1;
        }
    }
    char dll[MAX_PATH];
    me_settings_lock();   /* the UI thread writes the console picks */
    me_console_core_dll(&g_settings, c, dll, sizeof(dll));
    me_settings_unlock();
    printf("[load] %s game, core %s\n", c->name, dll);
    snprintf(core_path, core_path_sz, "%scores\\%s", g_exedir, dll);
    if (GetFileAttributesA(core_path) == INVALID_FILE_ATTRIBUTES) {
        snprintf(err, err_sz, "Core %s not found.\nUse Cores > Download Cores, or place it in %scores\\",
                 dll, g_exedir);
        return -1;
    }
    return 0;
}

/* Vulkan presenter (+ LSFG frame gen when enabled) for the current session.
   Also used to rebuild it when Frame Gen is toggled mid-game. Returns 1 if
   Vulkan is up. */
static int init_vulkan(me_session *s) {
#ifdef ME_HAVE_VULKAN
    /* CLI flag → env var consumed by me_vk_init. */
    if (g_no_vsync) _putenv("LAGGUELESS_VK_NO_VSYNC=1");
    if (g_settings.vk_exclusive_fullscreen && !getenv("LAGGUELESS_VK_EXCLUSIVE"))
        _putenv("LAGGUELESS_VK_EXCLUSIVE=1");
    if (g_pace_log) _putenv("LAGGUELESS_VK_PACE_LOG=1");
    if (me_vk_init(g_hwnd, g_back_max_w, g_back_max_h) != 0) {
        fprintf(stderr, "[render] Vulkan init failed; falling back to D3D11/GDI\n");
        return 0;
    }
#ifdef ME_HAVE_LSFG
    /* B3: Wire up the LSFG backend now that Vulkan is ready.
     * We use the core's base frame size from av_info. At context-open time
     * this is max_w x max_h (the backend accepts any size <= that). */
    if (g_lsfg_enabled && g_lsfg_shaders) {
        unsigned lsfg_w = s->base_w > 0 ? s->base_w : g_back_max_w;
        unsigned lsfg_h = s->base_h > 0 ? s->base_h : g_back_max_h;
        if (me_vk_lsfg_init(me_lsfg_dll_path(g_lsfg_shaders),
                            lsfg_w, lsfg_h,
                            g_settings.lsfg_multiplier,
                            g_settings.lsfg_flow_scale,
                            g_settings.lsfg_perf_mode) != 0) {
            fprintf(stderr, "[lsfg] B3 init failed - LSFG disabled, continuing with normal Vulkan\n");
        }
    }
#else
    (void)s;
#endif
    return 1;
#else
    (void)s;
    printf("[render] --vulkan requested but build has no Vulkan support (set VULKAN_SDK and rebuild)\n");
    return 0;
#endif
}

/* The rate to pace a core running at `fps` at: its own, or snapped to the
   display refresh when settings.yaml asks and the speed error allows. */
static double paced_fps(double fps) {
    /* Refresh-rate matching: if the monitor's actual rate is a near-exact
       integer multiple of the core's nominal fps, snap the pacing deadline to
       refresh/N so FIFO holds every frame for exactly N refreshes (no judder).
       Removes residual judder when core Hz ≠ display Hz (e.g. 60.10 vs 59.94)
       at the cost of a tiny core-vs-device clock mismatch the audio DRC
       already absorbs. fps here is the per-core rate from the AV info, so the
       target adapts to whatever console/region is loaded. Opt-in via
       settings.yaml. */
    if (g_settings.match_display_hz) {
        HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFOEXA mi = {0};
        mi.cbSize = sizeof(mi);
        double disp_hz = 0.0;
        if (GetMonitorInfoA(mon, (MONITORINFO *)&mi)) {
            /* Prefer the exact rational refresh; fall back to integer Hz. */
            disp_hz = me_query_exact_refresh_hz(mi.szDevice);
            if (disp_hz <= 1.0) {
                DEVMODEA dm = {0};
                dm.dmSize = sizeof(dm);
                if (EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)
                    && dm.dmDisplayFrequency > 1) {
                    /* dmDisplayFrequency is integer Hz. Treat 59 as 59.94 (NTSC). */
                    disp_hz = (dm.dmDisplayFrequency == 59) ? 59.94 : (double)dm.dmDisplayFrequency;
                }
            }
        }
        if (disp_hz > 1.0) {
            /* The display may run at an integer multiple of the core rate
               (e.g. 239.760 Hz panel vs 60.0998 Hz NES = ~4x). Pace to
               refresh/N so FIFO holds every frame for exactly N refreshes —
               no judder, no tearing, no VRR. N==1 reduces to the plain
               near-match case. */
            long n = (long)(disp_hz / fps + 0.5);
            if (n < 1) n = 1;
            double target = disp_hz / (double)n;
            double ratio = target / fps;
            double dev_pct = (target / fps - 1.0) * 100.0;

            /* Snapping the game to refresh/N kills judder but changes its speed
               by dev_pct, because a standard panel is never an exact multiple of
               the console rate (a "240 Hz" panel is 59.940x4, not 60.0998x4).
               competition mode (match_strict) only snaps when that error is
               negligible — i.e. a true exact-multiple refresh; casual mode
               tolerates the ~0.27% NTSC offset for plug-and-play smoothness.
               Past ~0.3% no clean hold count exists at all (144 Hz, 700 Hz, ...). */
            const double TOL_STRICT = 0.0005; /* 0.05% — competition */
            const double TOL_CASUAL = 0.003;  /* 0.30% — casual smoothness */
            double tol = g_settings.match_strict ? TOL_STRICT : TOL_CASUAL;

            if (ratio > 1.0 - tol && ratio < 1.0 + tol) {
                printf("[pace] match_display_hz: core %.4f Hz -> %.4f Hz "
                       "(display %.4f / %ld), game speed %+.3f%% [%s]\n",
                       fps, target, disp_hz, n, dev_pct,
                       g_settings.match_strict ? "competition" : "casual");
                fps = target;
            } else if (ratio > 1.0 - TOL_CASUAL && ratio < 1.0 + TOL_CASUAL) {
                /* A clean multiple, but the speed error exceeds competition
                   tolerance — only reachable in strict mode. */
                fprintf(stderr,
                    "[pace] competition mode: display %.4f Hz / %ld = %.4f Hz is a clean multiple\n"
                    "[pace]   but %+.2f%% off the %.4f Hz core rate. Keeping NATIVE speed (legal);\n"
                    "[pace]   expect minor judder. For smooth+legal: create a custom refresh of\n"
                    "[pace]   %.3f Hz (= core x%ld). For smooth+casual: set match_strict: false.\n",
                    disp_hz, n, target, dev_pct, fps, fps * (double)n, n);
            } else {
                fprintf(stderr,
                    "[pace] WARNING: display %.4f Hz is not an integer multiple of the\n"
                    "[pace]          %.4f Hz core rate (closest is /%ld = %.4f Hz, %+.2f%% off).\n"
                    "[pace]          Running NATIVE core rate for speed accuracy; expect judder\n"
                    "[pace]          during scrolling. For smooth playback use a refresh that\n"
                    "[pace]          divides the core rate evenly (e.g. 60/120/240/360 for ~60fps\n"
                    "[pace]          cores; 50/100/150/200 for ~50fps PAL) or enable VRR.\n",
                    disp_hz, fps, n, target, dev_pct);
            }
        } else {
            printf("[pace] match_display_hz: could not query display refresh; keeping core rate\n");
        }
    }
    return fps;
}

/* Load core + ROM, build the presenter/audio around its AV info, and create
   the window if this is the first game. `console` is the ROM's (rom_cores.h
   index, or -1 if unknown). Returns 0 on success; on failure the session is
   closed again and -1 is returned. */
static int session_open(me_session *s, const char *core_path_in, const char *rom_path_in,
                        int console) {
    memset(s, 0, sizeof(*s));
    s->console = console;
    /* Own the paths: cores may keep game.path, and callers pass scratch
       buffers. Absolute, because the working directory can change under us
       (the Open ROM dialog) before a hard reset or power-on reuses them. */
    if (!GetFullPathNameA(core_path_in, sizeof(s->core_path), s->core_path, NULL))
        snprintf(s->core_path, sizeof(s->core_path), "%s", core_path_in);
    if (!GetFullPathNameA(rom_path_in, sizeof(s->rom_path), s->rom_path, NULL))
        snprintf(s->rom_path, sizeof(s->rom_path), "%s", rom_path_in);
    const char *core_path = s->core_path;
    const char *rom_path  = s->rom_path;

    /* The core's own control map, if it has one (use_universal=false); the
       console's and universal fill in the rest (poll_player). */
    {
        g_core_index = me_settings_find_core_index(&g_settings, core_path);
        const struct me_core_entry *ce = g_core_index >= 0 ? &g_settings.cores[g_core_index] : NULL;
        if (ce && !ce->use_universal)
            printf("[settings] using per-core controls for %s\n", ce->name);
    }

    me_core *core = me_core_load(core_path);
    if (!core) { fprintf(stderr, "failed to load core: %s\n", core_path); goto fail; }
    s->core = core;

    unsigned api = core->retro_api_version();
    printf("[core] retro_api_version = %u\n", api);

    struct retro_system_info info = {0};
    core->retro_get_system_info(&info);
    printf("[core] %s %s (ext=%s, need_fullpath=%d)\n",
           info.library_name ? info.library_name : "?",
           info.library_version ? info.library_version : "?",
           info.valid_extensions ? info.valid_extensions : "?",
           (int)info.need_fullpath);

    snprintf(g_core_name, sizeof(g_core_name), "%s", info.library_name ? info.library_name : "");
    {
        /* Uncurated: every input, for the core's input descriptors to narrow. */
        me_input_layout l;
        const me_console *c = me_console_at(console);
        if (me_layout_for_game(g_core_name, rom_path, c ? c->id : NULL, c ? c->name : NULL,
                               c && (c->flags & ME_CONSOLE_ANALOG), &l))
            printf("[input] %s controller (%d inputs)\n", l.name, l.n);
        if (l.key[0]) printf("[input] controls: %s\n", l.key);
        set_input_layout(&l);
    }
    g_console = console;

    fprintf(stderr, "[load] set_environment\n"); fflush(stderr);
    core->retro_set_environment(me_environment_cb);
    /* Some cores (Mesen) allocate internal state in retro_init() that the
       callback setters dereference. Call retro_init before the setters.
       Compliant cores treat the setters as pointer stores so order is safe. */
    fprintf(stderr, "[load] retro_init()\n"); fflush(stderr);
    core->retro_init();
    s->core_inited = 1;
    fprintf(stderr, "[load] retro_init returned\n"); fflush(stderr);
    fprintf(stderr, "[load] set_video_refresh\n"); fflush(stderr);
    core->retro_set_video_refresh(me_video_refresh_cb);
    fprintf(stderr, "[load] set_audio_sample\n"); fflush(stderr);
    core->retro_set_audio_sample(me_audio_sample_cb);
    fprintf(stderr, "[load] set_audio_sample_batch\n"); fflush(stderr);
    core->retro_set_audio_sample_batch(me_audio_sample_batch_cb);
    fprintf(stderr, "[load] set_input_poll\n"); fflush(stderr);
    core->retro_set_input_poll(me_input_poll_cb);
    fprintf(stderr, "[load] set_input_state\n"); fflush(stderr);
    core->retro_set_input_state(me_input_state_cb);

    struct retro_game_info game = {0};
    game.path = rom_path;
    if (!info.need_fullpath) {
        size_t rom_size = 0;
        s->rom_data = slurp(rom_path, &rom_size);
        if (!s->rom_data) { fprintf(stderr, "failed to read ROM: %s\n", rom_path); goto fail; }
        game.data = s->rom_data;
        game.size = rom_size;
    }
    fprintf(stderr, "[load] retro_load_game(path=%s, data=%p, size=%zu)\n",
            game.path ? game.path : "(null)", game.data, game.size);
    fflush(stderr);

    memset(&g_wii_wfc, 0, sizeof(g_wii_wfc));
    g_wii_wfc_rom = rom_path;
    me_wii_wfc_on_options();   /* a core that declared its options already */
    bool loaded = core->retro_load_game(&game);
    g_wii_wfc_rom = NULL;
    fprintf(stderr, "[load] retro_load_game -> %s\n", loaded ? "true" : "false");
    fflush(stderr);
    if (!loaded) {
        fprintf(stderr, "retro_load_game failed\n");
        fflush(stderr);
        goto fail;
    }
    s->game_loaded = 1;
    plug_gamepads(core);
    apply_adapter(core);

    if (me_build_save_path(core_path, rom_path, s->save_path, sizeof(s->save_path)) == 0) {
        me_sram_load(core, s->save_path);
    } else {
        s->save_path[0] = '\0';
        fprintf(stderr, "[save] could not derive save path; saves disabled\n");
    }

    s->sram_last_poll_ms = GetTickCount();
    if (s->save_path[0] && core->retro_get_memory_data && core->retro_get_memory_size) {
        void *mem = core->retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
        size_t sz = core->retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
        if (mem && sz) {
            s->sram_disk_hash = s->sram_last_hash = me_fnv1a64(mem, sz);
        }
    }

    struct retro_system_av_info av = {0};
    core->retro_get_system_av_info(&av);
    printf("[av] base=%ux%u max=%ux%u aspect=%.4f fps=%.4f rate=%.1f\n",
           av.geometry.base_width, av.geometry.base_height,
           av.geometry.max_width,  av.geometry.max_height,
           av.geometry.aspect_ratio, av.timing.fps, av.timing.sample_rate);

    /* Allocate backbuffer sized to max geometry. */
    g_av_fps  = av.timing.fps;
    g_av_rate = av.timing.sample_rate;
    g_back_max_w = av.geometry.max_width  ? av.geometry.max_width  : av.geometry.base_width;
    g_back_max_h = av.geometry.max_height ? av.geometry.max_height : av.geometry.base_height;
    g_back = (u32 *)calloc((size_t)g_back_max_w * g_back_max_h, sizeof(u32));
    if (!g_back) { fprintf(stderr, "backbuffer alloc failed\n"); goto fail; }

    /* HW path: now that we know max geometry, build the FBO and fire the
       core's context_reset so it can upload its shaders/VBOs. retro_load_game
       already returned, but cores designed around hw_render defer all GL
       resource creation until context_reset — exactly because the frontend
       may not have a context ready at load time. */
    if (g_hw_render_accepted) {
        if (me_gl_fbo_create(g_back_max_w, g_back_max_h,
                             g_hw_render.depth, g_hw_render.stencil) != 0) {
            fprintf(stderr, "[hw] FBO creation failed\n");
            goto fail;
        }
        if (g_hw_render.context_reset) {
            fprintf(stderr, "[hw] calling context_reset\n"); fflush(stderr);
            g_hw_render.context_reset();
        }
        s->hw_context_live = 1;
    }
    g_frame_w = av.geometry.base_width;
    g_frame_h = av.geometry.base_height;
    g_core_aspect = av.geometry.aspect_ratio;
    s->base_w = av.geometry.base_width;
    s->base_h = av.geometry.base_height;

    /* First game of the run: create the window sized to a reasonable 2× of
       base geometry. Later games reuse the window as the user left it. */
    if (!g_hwnd) {
        int win_w = (int)(av.geometry.base_width  * 2);
        int win_h = (int)(av.geometry.base_height * 2);
        if (win_w < 320) win_w = 640;
        if (win_h < 240) win_h = 480;
        if (create_main_window(win_w, win_h) != 0) goto fail;
    }
    me_platform_set_idle(g_hwnd, 0);

    /* Vulkan path takes priority when --vulkan succeeds. If Vulkan init
       fails, fall through to the normal D3D11/GDI selection below. */
    int vk_initialized = g_force_vulkan ? init_vulkan(s) : 0;

    /* D3D11 flip-model is used for HW (GL) cores by default and for software
       cores when --d3d11 is set. Otherwise software cores stay on GDI: lower
       visible tearing on non-VRR displays. --gdi overrides everything. */
    if (!vk_initialized && !g_force_gdi && (g_hw_render_accepted || g_force_d3d11)) {
        if (me_d3d11_init(g_hwnd, g_back_max_w, g_back_max_h) == 0) {
            g_use_d3d11 = 1;
        } else {
            fprintf(stderr, "[render] D3D11 init failed, falling back to GDI\n");
        }
    } else if (g_force_gdi) {
        printf("[render] --gdi forced\n");
    }

    /* Try the WGL_NV_DX_interop2 zero-copy transport. If it fails (driver
       doesn't support it, or registration errors), the readback path stays
       in effect — no functional regression, just slightly higher transport
       cost. */
    if (g_use_d3d11 && g_hw_render_accepted) {
        void *shared = me_d3d11_create_shared_texture(g_back_max_w, g_back_max_h);
        if (shared) {
            if (me_gl_interop_attach(me_d3d11_get_device(), shared) == 0) {
                me_d3d11_use_shared(1);
            }
        }
    }

    /* Audio drives pacing. WASAPI runs at the device's mix rate; we resample
       core output up to that rate. */
    g_core_rate = (unsigned)(av.timing.sample_rate > 0 ? av.timing.sample_rate : 48000);
    double fps = av.timing.fps > 1.0 ? av.timing.fps : 60.0;

    fps = paced_fps(fps);

    if (g_no_audio) {
        printf("[audio] disabled via --no-audio; using Sleep-based pacing\n");
        g_dev_rate = g_core_rate;
    } else {
        int audio_mode = ME_AUDIO_MODE_SHARED;
        if (g_settings.exclusive_mode)
            audio_mode = ME_AUDIO_MODE_EXCLUSIVE;
        else if (g_settings.low_latency)
            audio_mode = ME_AUDIO_MODE_LOW_LATENCY;
        s->audio_ok = (me_audio_init(&g_dev_rate, audio_mode) == 0);
        if (!s->audio_ok) {
            fprintf(stderr, "[audio] init failed; falling back to Sleep pacing\n");
            me_audio_shutdown(); /* release whatever the failed init acquired */
            g_dev_rate = g_core_rate;
        } else {
            g_audio_active = 1;
        }
    }
    /* Pacing target: keep about 30 ms buffered in the ring. Cores deliver
       audio in per-frame bursts (~16.6 ms at 60 fps), and the WASAPI buffer
       drains in ~20 ms cycles. 30 ms gives ~13 ms of headroom over the
       largest single drain/push event, which keeps the ring above empty
       across jittery delivery without bloating latency. Lower than this
       (e.g. 20 ms) causes underruns in cores like Mesen that produce one
       big batch per frame at exactly the device rate. */
    s->target_buffered = (size_t)(g_dev_rate * 0.030);

    /* Video pacing is QPC absolute-deadline + spin-wait, regardless of audio.
       Audio is kept in sync via a small bias on the resampler ratio (dynamic
       rate control), NOT by skipping or duplicating frames. */
    s->frame_period_ms = 1000.0 / fps;
    QueryPerformanceFrequency(&s->qpf);

    /* Run-ahead setup. Disabled for HW (GL) cores: savestates don't capture GL
       context state, and re-running a frame with GL side-effects (FBO writes,
       texture uploads) would corrupt visible output. Software cores serialize
       to a flat byte buffer that round-trips cleanly. */
    s->ra_frames = g_settings.runahead_frames;
    if (s->ra_frames > 0 && g_hw_render_accepted) {
        printf("[runahead] disabled for hardware-rendered cores\n");
        s->ra_frames = 0;
    }
    if (s->ra_frames > 0) {
        if (!core->retro_serialize_size || !core->retro_serialize || !core->retro_unserialize) {
            printf("[runahead] core lacks serialize support; disabling\n");
            s->ra_frames = 0;
        } else {
            s->ra_state_size = core->retro_serialize_size();
            if (s->ra_state_size == 0) {
                printf("[runahead] core reports zero state size; disabling\n");
                s->ra_frames = 0;
            } else {
                s->ra_state_buf = malloc(s->ra_state_size);
                if (!s->ra_state_buf) {
                    fprintf(stderr, "[runahead] state buffer alloc failed (%zu bytes); disabling\n",
                            s->ra_state_size);
                    s->ra_frames = 0;
                } else {
                    printf("[runahead] enabled: %d frame%s ahead, state=%zu bytes\n",
                           s->ra_frames, s->ra_frames == 1 ? "" : "s", s->ra_state_size);
                }
            }
        }
    }

    /* Thread affinity + priority isolation.
       Emulation thread (this thread) → THREAD_PRIORITY_HIGHEST + P-cores.
       Audio render thread              → THREAD_PRIORITY_TIME_CRITICAL + a
       separate core (second P-core, or an E-core if there's only one P-core).
       Falls back gracefully to just setting priority when affinity detection
       fails (homogeneous CPU, pre-Win10, single-core). */
    if (g_settings.thread_affinity) {
        DWORD_PTR emu_mask = 0, audio_mask = 0;
        int hybrid = me_pick_affinity_masks(&emu_mask, &audio_mask);
        if (hybrid) {
            SetThreadAffinityMask(GetCurrentThread(), emu_mask);
            printf("[affinity] emu thread pinned to mask 0x%llx\n",
                   (unsigned long long)emu_mask);
            if (s->audio_ok) {
                me_audio_set_thread_affinity((unsigned long long)audio_mask,
                                            THREAD_PRIORITY_TIME_CRITICAL);
                printf("[affinity] audio thread pinned to mask 0x%llx, priority=TIME_CRITICAL\n",
                       (unsigned long long)audio_mask);
            }
        } else {
            printf("[affinity] could not split cores (single-core or API unavailable); priority-only\n");
            if (s->audio_ok)
                me_audio_set_thread_affinity(0, THREAD_PRIORITY_TIME_CRITICAL);
        }
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        printf("[affinity] emu thread priority=HIGHEST\n");
    }

    s->pl_gap_min = 1e9;
    s->pl_fill_before_min = s->pl_fill_after_min = (size_t)-1;

    /* Anchor qstart AFTER all one-time setup so the very first frame's
       deadline doesn't start out late. */
    QueryPerformanceCounter(&s->qstart);
    s->pl_last_qpc = s->pl_window_start = s->ll_window_start = s->qstart;
    s->active = 1;
    return 0;

fail:
    session_close(s);
    return -1;
}

/* Hotkeys are live with or without a game loaded. Anything that restarts the
   game is posted as a command so it runs between frames. */
static void handle_hotkeys(void) {
    static int xi_prev[ME_HK_COUNT];
    int fire[ME_HK_COUNT] = {0};
    int foreground = GetForegroundWindow() == g_hwnd;

    me_settings_lock();
    int use_kb = g_settings.hk_source != ME_SRC_CONTROLLER;
    int use_xi = g_settings.hk_source != ME_SRC_KEYBOARD && foreground;
    int slot = g_settings.hk_xi_index;
    if (use_xi) me_xinput_poll(slot);
    for (int i = 0; i < ME_HK_COUNT; i++) {
        /* Always consume the key edge so a stale press can't fire later. */
        int kb = hk_pressed(&g_settings.hk[i]) && use_kb;
        int xi = use_xi ? hk_xi_pressed(&g_settings.hk_xi[i], slot, &xi_prev[i])
                        : (xi_prev[i] = 0);
        fire[i] = kb || xi;
    }
    me_settings_unlock();

    if (fire[ME_HK_TOGGLE_FULLSCREEN]) me_platform_toggle_fullscreen(g_hwnd);
    if (fire[ME_HK_EXIT_FULLSCREEN])   me_platform_exit_fullscreen(g_hwnd);
    if (fire[ME_HK_CYCLE_ASPECT]) {
        g_settings.aspect = (me_aspect_mode)(((int)g_settings.aspect + 1) % ME_ASPECT_COUNT);
        printf("[aspect] %s\n", g_aspect_names[g_settings.aspect]);
    }
    if (fire[ME_HK_QUIT])       me_platform_request_quit();
    if (fire[ME_HK_HARD_RESET]) me_cmd_post(ME_CMD_HARD_RESET, 0, NULL);
    if (fire[ME_HK_SWAP_SCREENS] || fire[ME_HK_TOGGLE_BOTH_SCREENS]) {
        /* Like View > Screen: only two-screen consoles have a choice, and
           with no game the pick is kept for the next one. Not saved, like
           F1's aspect. */
        const me_console *c = me_console_at(g_console);
        if (!c || (c->flags & ME_CONSOLE_TWO_SCREENS)) {
            if (fire[ME_HK_SWAP_SCREENS])
                me_settings_set_screens(&g_settings, me_screens_swapped(&g_settings));
            if (fire[ME_HK_TOGGLE_BOTH_SCREENS])
                me_settings_set_screens(&g_settings, me_screens_both_toggled(&g_settings));
            printf("[screens] %s\n", g_settings.screens == ME_SCREENS_TOP    ? "top"
                                   : g_settings.screens == ME_SCREENS_BOTTOM ? "bottom" : "both");
        }
    }
}

/* When frame `n` is due, in ms after qstart. */
static double frame_deadline_ms(const me_session *s, unsigned long n) {
    return s->pace_base_ms + (double)(n - s->pace_base_frame) * s->frame_period_ms;
}

/* One main-loop iteration with a game running: backpressure wait, hotkeys,
   retro_run (+ run-ahead), present, SRAM flush, audio DRC, deadline pacing. */
static void session_run_frame(me_session *s) {
    me_core *core = s->core;
    const LARGE_INTEGER qpf = s->qpf;
    LARGE_INTEGER ll_t0; if (g_latency_log) QueryPerformanceCounter(&ll_t0);
    /* DXGI 1.3 waitable swap chain backpressure: with max frame latency
       pinned to 1, this blocks until the previous Present has been
       consumed by the compositor. Keeps CPU exactly one frame ahead of
       GPU instead of letting Present queue frames silently. Pre-Win8.1
       and the GDI present path return 0 here and rely on the QPC tail
       below for pacing. */
    if (g_use_d3d11) me_d3d11_wait_for_present(1000);
    else if (me_vk_is_active()) me_vk_wait_for_present(1000);
    LARGE_INTEGER ll_t_after_wait; if (g_latency_log) QueryPerformanceCounter(&ll_t_after_wait);
    handle_hotkeys();
    size_t fill_before = 0, fill_after = 0;
    if (s->audio_ok && g_pace_log) fill_before = ME_RING_TOTAL - me_audio_writable_frames();

    /* GL is per-thread; make sure our context is current on this thread
       before the core does any GL work. Cheap if already current. */
    if (g_hw_render_accepted) {
        me_gl_make_current();
        /* Interop: lock the shared texture so GL has exclusive access
           while the core renders. No-op if interop is inactive. */
        me_gl_interop_lock();
    }
    if (g_latency_log) g_poll_qpc.QuadPart = 0;
    LARGE_INTEGER ll_t_before_run; if (g_latency_log) QueryPerformanceCounter(&ll_t_before_run);
    if (s->ra_frames > 0) {
        /* Run-ahead, single-instance technique. The core is currently at
           frame F (the displayed frame from last iteration). To show the
           user frame F+ra_frames worth of latency reduction:
             1. save state at F
             2. silently advance ra_frames more frames (A/V muted) so the
                sim is "looking ahead"
             3. run one more frame with A/V on — this is what we show
             4. load the saved state — undo the ahead+visible frames
             5. advance exactly one real frame (muted) so next iter starts
                at F+1 — net forward progress is one frame per iteration.
           The displayed frame is ra_frames ahead of the underlying sim
           clock, which is exactly the input-latency reduction the user
           feels: their input applies "earlier" relative to what they see. */
        if (!core->retro_serialize(s->ra_state_buf, s->ra_state_size)) {
            fprintf(stderr, "[runahead] serialize failed; disabling for rest of session\n");
            free(s->ra_state_buf); s->ra_state_buf = NULL;
            s->ra_frames = 0;
            core->retro_run();
        } else {
            g_av_mute = 1;
            for (int i = 0; i < s->ra_frames; i++) core->retro_run();
            g_av_mute = 0;
            core->retro_run();  /* this one is shown */
            if (!core->retro_unserialize(s->ra_state_buf, s->ra_state_size)) {
                fprintf(stderr, "[runahead] unserialize failed; disabling for rest of session\n");
                free(s->ra_state_buf); s->ra_state_buf = NULL;
                s->ra_frames = 0;
            } else {
                g_av_mute = 1;
                core->retro_run();
                g_av_mute = 0;
            }
        }
    } else {
        core->retro_run();
    }
    if (g_hw_render_accepted) me_gl_interop_unlock();
    LARGE_INTEGER ll_t_after_run; if (g_latency_log) QueryPerformanceCounter(&ll_t_after_run);
    present(g_hwnd);
    LARGE_INTEGER ll_t_after_present; if (g_latency_log) QueryPerformanceCounter(&ll_t_after_present);

    /* SRAM dirty-poll: catches in-game saves so a force-quit shortly after
       the user hits "Save" still persists the write. 1s cadence + one-tick
       debounce → worst case ~2s to land on disk. */
    if (s->save_path[0] && core->retro_get_memory_data && core->retro_get_memory_size) {
        DWORD now_ms = GetTickCount();
        if (now_ms - s->sram_last_poll_ms >= 1000) {
            s->sram_last_poll_ms = now_ms;
            void *mem = core->retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
            size_t sz = core->retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
            if (mem && sz) {
                uint64_t h = me_fnv1a64(mem, sz);
                if (h != s->sram_disk_hash) {
                    if (s->sram_pending && h == s->sram_last_hash) {
                        /* Settled — write it out. */
                        me_sram_save(core, s->save_path);
                        s->sram_disk_hash = h;
                        s->sram_pending = 0;
                    } else {
                        /* Still changing (or first time we noticed) — wait one more tick. */
                        s->sram_pending = 1;
                    }
                } else {
                    s->sram_pending = 0;
                }
                s->sram_last_hash = h;
            }
        }
    }

    /* Dynamic rate control: PI controller on ring-fill error. The integral
       term `g_resamp_ratio_bias` absorbs the long-term core-vs-device
       clock mismatch; the proportional term adds a tiny instantaneous
       response to keep the ring near target. Bias clamped to ±0.5% so
       pitch shift stays below audibility (~8 cents). */
    double drc_p_term = 0.0;
    if (s->audio_ok) {
        size_t fill = ME_RING_TOTAL - me_audio_writable_frames();
        if (g_pace_log) fill_after = fill;

        /* 60-frame moving average of ring fill. Reacting to instantaneous
           fill makes the rate bias chase per-frame noise and produces an
           audible pitch wobble. Averaging over ~1 second smooths that out
           while still tracking the true core-vs-device clock drift. */
        s->fill_hist[s->fill_hist_idx] = fill;
        s->fill_hist_idx = (s->fill_hist_idx + 1) % 60;
        if (s->fill_hist_idx == 0) s->fill_hist_filled = 1;

        if (s->fill_hist_filled) {
            uint64_t sum = 0;
            for (int k = 0; k < 60; k++) sum += s->fill_hist[k];
            double avg_fill = (double)sum / 60.0;
            /* Normalized error: -1 = ring empty, 0 = at target, +1 = double target. */
            double err = (avg_fill - (double)s->target_buffered) / (double)s->target_buffered;
            /* Integral term: tracks the true core-vs-device clock mismatch.
               Bound to ±0.25% (~4 cents, still inaudible — Mesen targets
               the same window). Wider than the original ±0.1% because
               cores at identity ratio (Mesen NES @ 48000) need more
               headroom to drain a too-full ring within reasonable time. */
            g_resamp_ratio_bias += 1.0e-6 * err;
            if (g_resamp_ratio_bias >  0.0025) g_resamp_ratio_bias =  0.0025;
            if (g_resamp_ratio_bias < -0.0025) g_resamp_ratio_bias = -0.0025;
            /* Proportional term: very gentle for inaudible transient response.
               Max ±0.05% (~0.9 cents) and applied only this frame. */
            drc_p_term = 0.0001 * err;
            if (drc_p_term >  0.0005) drc_p_term =  0.0005;
            if (drc_p_term < -0.0005) drc_p_term = -0.0005;
        }
    }
    g_resamp_p_bias = drc_p_term;

    /* The core asked for new timing during this frame: pace from here on at
       its new rate, and resample its audio from its new sample rate. */
    if (g_av_timing_changed) {
        g_av_timing_changed = 0;
        s->pace_base_ms    = frame_deadline_ms(s, s->frame_count);
        s->pace_base_frame = s->frame_count;
        s->frame_period_ms = 1000.0 / paced_fps(g_av_fps > 1.0 ? g_av_fps : 60.0);
        if (g_av_rate > 0) g_core_rate = (unsigned)g_av_rate;
        printf("[pace] core timing now %.4f fps, %.1f Hz audio\n", g_av_fps, g_av_rate);
    }

    /* QPC absolute-deadline pace (frame_deadline_ms). Sleep most of the
       wait at 1ms resolution, then spin the last bit. */
    s->frame_count++;
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double elapsed_ms = (double)(now.QuadPart - s->qstart.QuadPart) * 1000.0 / (double)qpf.QuadPart;
        double deadline_ms = frame_deadline_ms(s, s->frame_count);
        /* A frame or more behind (a stall: shader compile, disc read, window
           drag): the time is lost. Re-pace from now instead of running the
           backlog of frames unthrottled, which plays the game visibly fast
           until the schedule catches up. Sub-frame lateness keeps the
           absolute schedule so the long-run rate stays exact. */
        if (elapsed_ms - deadline_ms >= s->frame_period_ms) {
            s->pace_base_ms    = elapsed_ms;
            s->pace_base_frame = s->frame_count;
            deadline_ms        = elapsed_ms;
            s->pl_resyncs++;
        }
        double wait_ms = deadline_ms - elapsed_ms;
        if (wait_ms > 1.5) Sleep((DWORD)(wait_ms - 1.0));
        while (1) {
            QueryPerformanceCounter(&now);
            elapsed_ms = (double)(now.QuadPart - s->qstart.QuadPart) * 1000.0 / (double)qpf.QuadPart;
            if (elapsed_ms >= deadline_ms) break;
        }
    }
    if (g_timing_log && (s->frame_count % 1000) == 0) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double elapsed_ms = (double)(now.QuadPart - s->qstart.QuadPart) * 1000.0 / (double)qpf.QuadPart;
        double expected_ms = frame_deadline_ms(s, s->frame_count);
        double drift_ms = elapsed_ms - expected_ms;
        me_log(ME_LOG_TIMING,
               "[timing] frame %lu expected=%.3f ms actual=%.3f ms drift=%+.3f ms (%+.3f us/frame) bias=%+.4f%%\n",
               s->frame_count, expected_ms, elapsed_ms, drift_ms,
               (drift_ms * 1000.0) / (double)s->frame_count,
               g_resamp_ratio_bias * 100.0);
    }
    if (g_latency_log) {
        LARGE_INTEGER ll_t_end; QueryPerformanceCounter(&ll_t_end);
        double q = 1000.0 / (double)qpf.QuadPart;
        double d_back    = (double)(ll_t_after_wait.QuadPart    - ll_t0.QuadPart)            * q;
        double d_run     = (double)(ll_t_after_run.QuadPart     - ll_t_before_run.QuadPart)  * q;
        double d_present = (double)(ll_t_after_present.QuadPart - ll_t_after_run.QuadPart)   * q;
        double d_pace    = (double)(ll_t_end.QuadPart           - ll_t_after_present.QuadPart) * q;
        double d_iter    = (double)(ll_t_end.QuadPart           - ll_t0.QuadPart)            * q;
        double d_pre = d_run, d_post = 0;
        if (g_poll_qpc.QuadPart != 0
            && g_poll_qpc.QuadPart >= ll_t_before_run.QuadPart
            && g_poll_qpc.QuadPart <= ll_t_after_run.QuadPart) {
            d_pre  = (double)(g_poll_qpc.QuadPart      - ll_t_before_run.QuadPart) * q;
            d_post = (double)(ll_t_after_run.QuadPart  - g_poll_qpc.QuadPart)      * q;
        } else {
            s->ll_missed_polls++;
        }
        s->ll_back_sum    += d_back;
        s->ll_pre_sum     += d_pre;
        s->ll_post_sum    += d_post;
        s->ll_present_sum += d_present;
        s->ll_pace_sum    += d_pace;
        if (d_iter > s->ll_iter_max) s->ll_iter_max = d_iter;
        if (s->ll_iters == 0) s->ll_window_first_frame = s->frame_count;
        s->ll_iters++;
        double window_ms = (double)(ll_t_end.QuadPart - s->ll_window_start.QuadPart) * q;
        if (window_ms >= 1000.0 && s->ll_iters > 0) {
            double n = (double)s->ll_iters;
            me_log(ME_LOG_LATENCY,
                   "[latency] frame %lu..%lu (n=%u) avg ms: back=%.3f pre_poll=%.3f post_poll=%.3f present=%.3f pace=%.3f | iter_max=%.3f missed_polls=%u\n",
                   s->ll_window_first_frame, s->frame_count, s->ll_iters,
                   s->ll_back_sum / n, s->ll_pre_sum / n, s->ll_post_sum / n,
                   s->ll_present_sum / n, s->ll_pace_sum / n,
                   s->ll_iter_max, s->ll_missed_polls);
            s->ll_back_sum = s->ll_pre_sum = s->ll_post_sum = 0;
            s->ll_present_sum = s->ll_pace_sum = 0;
            s->ll_iter_max = 0;
            s->ll_iters = 0; s->ll_missed_polls = 0;
            s->ll_window_start = ll_t_end;
        }
    }
    if (g_pace_log) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double gap_ms = (double)(now.QuadPart - s->pl_last_qpc.QuadPart) * 1000.0 / (double)qpf.QuadPart;
        s->pl_last_qpc = now;
        if (gap_ms < s->pl_gap_min) s->pl_gap_min = gap_ms;
        if (gap_ms > s->pl_gap_max) s->pl_gap_max = gap_ms;
        s->pl_gap_sum += gap_ms;
        if (fill_before < s->pl_fill_before_min) s->pl_fill_before_min = fill_before;
        if (fill_before > s->pl_fill_before_max) s->pl_fill_before_max = fill_before;
        s->pl_fill_before_sum += fill_before;
        if (fill_after  < s->pl_fill_after_min)  s->pl_fill_after_min  = fill_after;
        if (fill_after  > s->pl_fill_after_max)  s->pl_fill_after_max  = fill_after;
        s->pl_fill_after_sum  += fill_after;
        s->pl_iters++;
        double window_ms = (double)(now.QuadPart - s->pl_window_start.QuadPart) * 1000.0 / (double)qpf.QuadPart;
        if (window_ms >= 1000.0 && s->pl_iters > 0) {
            me_log(ME_LOG_PACE,
                   "[pace] %ufps gap min/avg/max=%.1f/%.1f/%.1f ms fill_before(min/avg/max)=%zu/%zu/%zu fill_after=%zu/%zu/%zu bias=%+.4f%% resyncs=%u\n",
                   s->pl_iters,
                   s->pl_gap_min, s->pl_gap_sum / s->pl_iters, s->pl_gap_max,
                   s->pl_fill_before_min, s->pl_fill_before_sum / s->pl_iters, s->pl_fill_before_max,
                   s->pl_fill_after_min,  s->pl_fill_after_sum  / s->pl_iters, s->pl_fill_after_max,
                   g_resamp_ratio_bias * 100.0, s->pl_resyncs);
            s->pl_resyncs = 0;
            s->pl_gap_min = 1e9; s->pl_gap_max = 0; s->pl_gap_sum = 0;
            s->pl_fill_before_min = (size_t)-1; s->pl_fill_before_max = 0; s->pl_fill_before_sum = 0;
            s->pl_fill_after_min  = (size_t)-1; s->pl_fill_after_max  = 0; s->pl_fill_after_sum  = 0;
            s->pl_iters = 0;
            s->pl_window_start = now;
        }
    }
}

/* ---- commands from the UI thread -----------------------------------------
   Everything here runs between frames on the emulation thread. */

/* The game Console > Power On brings back after Power Off. */
static char g_last_core[MAX_PATH];
static char g_last_rom[MAX_PATH];
static int  g_last_console = -1;
static int  g_powered_off = 0;

static void publish_status(const me_session *s) {
    me_app_status st;
    memset(&st, 0, sizeof(st));
    st.game_running     = s->active;
    st.can_power_on     = !s->active && g_powered_off && g_last_rom[0];
    st.vulkan_active    = me_vk_is_active();
    st.frame_gen_active = st.vulkan_active && g_lsfg_enabled && g_lsfg_shaders;
#ifdef ME_HAVE_LSFG
    st.frame_gen_supported = 1;
#endif
    if (s->active || st.can_power_on) {
        snprintf(st.core_path, sizeof(st.core_path), "%s", g_last_core);
        snprintf(st.rom_path,  sizeof(st.rom_path),  "%s", g_last_rom);
    }
    st.players = ME_MAX_PLAYERS;
    st.console = -1;
    st.adapter = -1;
    for (int p = 0; p < ME_MAX_PLAYERS; p++) st.controller[p] = -1;
    const me_console *c = s->active ? me_console_at(g_console) : NULL;
    if (c) {
        st.players = g_players;
        st.console = g_console;
        for (int i = 0; c->adapters && c->adapters[i].id; i++) {
            if (adapter_usable(s->core, &c->adapters[i])) st.adapters_usable |= 1u << i;
            if (g_adapter == &c->adapters[i]) st.adapter = i;
        }
        st.controllers_usable = controllers_usable(s->core, c);
        st.fan_servers = me_core_has_dns_var() ? ME_FAN_SERVERS_DS
                       : g_wii_wfc.wii         ? ME_FAN_SERVERS_WII : ME_FAN_SERVERS_NONE;
        snprintf(st.fan_server_note, sizeof(st.fan_server_note), "%s", g_wii_wfc.note);
        for (int p = 0; p < ME_MAX_PLAYERS; p++)
            if (g_controller[p]) st.controller[p] = (int)(g_controller[p] - me_console_controllers(c));
    }
    me_status_set(&st);
}

static const char *path_basename(const char *path) {
    const char *base = path;
    for (const char *p = path; *p; p++) if (*p == '\\' || *p == '/') base = p + 1;
    return base;
}

/* session_open plus the bookkeeping every way of starting a game shares. */
static int open_game(me_session *s, const char *core_path, const char *rom_path, int console) {
    /* System files the core can't do without: said up front, since cores
       report them only in their log, if at all, and some then hang. */
    char sysdir[MAX_PATH], missing[512] = "", why[1024] = "";
    exepath(sysdir, sizeof(sysdir), "firmware");
    const me_console *c = me_console_at(console);
    me_core_missing_files(path_basename(core_path), c ? c->id : NULL, sysdir, missing, sizeof(missing));
    if (missing[0]) {
        char name[128];
        me_core_display_name(path_basename(core_path), name, sizeof(name));
        snprintf(why, sizeof(why), "%s needs these system files, which aren't in %s:\n\n%s",
                 name, sysdir, missing);
        fprintf(stderr, "[firmware] %s", why);
    }

    int rc = session_open(s, core_path, rom_path, console);
    if (rc == 0) {
        snprintf(g_last_core, sizeof(g_last_core), "%s", s->core_path);
        snprintf(g_last_rom,  sizeof(g_last_rom),  "%s", s->rom_path);
        g_last_console = s->console;
        g_powered_off = 0;
        me_ui_notify_loaded(s->rom_path);
        if (why[0])
            me_ui_notify_error("%s\nThe game may not start, or may misbehave, until they're added.\n"
                               "Cores > Firmware shows what each console needs.", why);
    } else if (why[0]) {
        me_ui_notify_error("Could not load %s.\n\n%s\nCores > Firmware shows what each console needs.",
                           path_basename(rom_path), why);
    } else {
        /* Firmware only some games need (a cartridge's extra chip): likely
           why, when it's missing. */
        me_firmware_file files[8];
        int n = me_core_firmware(path_basename(core_path), c ? c->id : NULL, sysdir, files, 8);
        const me_firmware_file *chip = NULL;
        for (int i = 0; i < n && !chip; i++)
            if (files[i].some_games && !files[i].found[0]) chip = &files[i];
        if (chip)
            me_ui_notify_error("Could not load %s.\n\nIf the game has an extra chip, it needs %s, "
                               "which isn't in %s:\n\n%s\n\nCores > Firmware shows what each console needs.",
                               path_basename(rom_path), chip->label, sysdir, chip->show);
        else
            me_ui_notify_error("Could not load %s.\nSee the console window for details.",
                               path_basename(rom_path));
    }
    publish_status(s);
    return rc;
}

/* Close and reopen the running game (hard reset) or the powered-off one. */
static void restart_game(me_session *s, const char *core_path_in, const char *rom_path_in,
                         int console) {
    char core_path[MAX_PATH], rom_path[MAX_PATH];
    snprintf(core_path, sizeof(core_path), "%s", core_path_in);
    snprintf(rom_path,  sizeof(rom_path),  "%s", rom_path_in);
    if (s->active) session_close(s);
    open_game(s, core_path, rom_path, console);
}

static void set_frame_gen(me_session *s, int on) {
#ifdef ME_HAVE_LSFG
    if (on && !g_lsfg_shaders) {
        g_lsfg_shaders = me_lsfg_load(g_lsfg_dll_path[0] ? g_lsfg_dll_path : NULL);
        if (!g_lsfg_shaders) {
            me_ui_notify_error("Frame Gen needs Lossless.dll from Lossless Scaling (Steam),\n"
                               "which wasn't found in any Steam library.\n"
                               "Install it, or copy Lossless.dll into %slsfg\\", g_exedir);
            me_ui_notify_frame_gen(0);
            return;
        }
    }
    g_lsfg_enabled = on;
    if (s->active && me_vk_is_active()) {
        /* Rebuild the Vulkan presenter: LSFG switches the present mode on
           the way in, and only a fresh swapchain switches it back. The core
           and audio are untouched; this costs a few frames' worth of time
           (frames are delayed, never dropped). If Vulkan fails to come back,
           present() falls back to GDI. */
        me_vk_lsfg_shutdown();
        me_vk_shutdown();
        init_vulkan(s);
    }
    printf("[lsfg] frame gen %s\n", on ? "on" : "off");
    me_ui_notify_frame_gen(on);
    publish_status(s);
#else
    (void)s; (void)on;
    me_ui_notify_frame_gen(0);
#endif
}

static void run_command(me_session *s, const me_cmd *c) {
    switch (c->type) {
        case ME_CMD_LOAD_ROM: {
            char core_path[MAX_PATH], err[MAX_PATH * 2];
            me_console_set candidates;
            printf("[load] %s\n", c->path);
            /* The player's pick of console: just now, or remembered. */
            int picked = c->arg > 0 ? c->arg - 1 : remembered_console(c->path);
            if (c->arg == ME_LOAD_ASK) {
                /* Shift: ask, starting on what it would open as. A type only
                   one console uses has nothing to ask. */
                candidates = me_rom_candidates(c->path);
                if (candidates & (candidates - 1)) {
                    int now = picked >= 0 ? picked : me_console_for_rom(c->path, NULL);
                    printf("[load] Shift held; asking which console\n");
                    me_ui_pick_console(c->path, candidates, now >= 0 ? now : -1);
                    return;
                }
            }
            if (picked >= 0 && c->arg <= 0)
                printf("[load] opening as %s, as picked before\n", me_console_at(picked)->name);
            int console = picked;
            /* The current game (if any) keeps running untouched until the
               new one is ready to open, or for good if it can't be. */
            int rc = resolve_rom_core(c->path, &console, &candidates,
                                      core_path, sizeof(core_path), err, sizeof(err));
            if (rc == ME_ROM_AMBIGUOUS) {
                printf("[load] several consoles use this file type; asking which\n");
                me_ui_pick_console(c->path, candidates, -1);
                return;
            }
            if (rc != 0) {
                fprintf(stderr, "[load] %s\n", err);
                me_ui_notify_error("%s", err);
                return;
            }
            if (s->active) session_close(s);
            /* Remembered only once it opens: a wrong pick that fails isn't. */
            if (open_game(s, core_path, c->path, console) == 0 && picked >= 0)
                remember_console(c->path, console);
            return;
        }
        case ME_CMD_HARD_RESET:
            if (!s->active) return;
            printf("[console] hard reset\n");
            restart_game(s, s->core_path, s->rom_path, s->console);
            return;
        case ME_CMD_SOFT_RESET:
            if (s->active && s->core->retro_reset) {
                s->core->retro_reset();
                printf("[console] soft reset\n");
            }
            return;
        case ME_CMD_POWER:
            if (s->active) {
                printf("[console] power off\n");
                session_close(s);
                g_powered_off = 1;
                publish_status(s);
            } else if (g_powered_off && g_last_rom[0]) {
                printf("[console] power on\n");
                restart_game(s, g_last_core, g_last_rom, g_last_console);
            }
            return;
        case ME_CMD_FRAME_GEN:
            set_frame_gen(s, c->arg);
            return;
        case ME_CMD_ADAPTER:
            if (!s->active) return;
            apply_adapter(s->core);
            publish_status(s);
            return;
        case ME_CMD_CONTROLLER:
            if (!s->active) return;
            apply_controllers(s->core);
            publish_status(s);
            return;
    }
}

/* The console of a ROM opened with a core named on the command line: when
   its header doesn't settle it, the one candidate that core is listed for.
   -1 if none or several. */
static int console_for_core(const char *rom_path, const char *core_path) {
    me_console_set candidates;
    int console = me_console_for_rom(rom_path, &candidates);
    if (console != ME_ROM_AMBIGUOUS) return console;
    console = -1;
    for (int i = 0; i < me_console_count(); i++) {
        if (!(candidates >> i & 1)) continue;
        char dll[MAX_PATH];
        for (int k = 0; me_console_candidate(me_console_at(i), k, dll, sizeof(dll)); k++) {
            if (_stricmp(dll, path_basename(core_path)) != 0) continue;
            if (console >= 0) return -1;
            console = i;
            break;
        }
    }
    return console;
}

int main(int argc, char **argv) {
    g_main_tid = GetCurrentThreadId();
    SetUnhandledExceptionFilter(me_unhandled_exception);
    /* The elevated copy Cores > File Associations runs: registry only, no
       window, no folders or settings.yaml of its own. */
    if (argc >= 2 && strcmp(argv[1], ME_FILE_TYPES_ARG) == 0)
        return me_file_types_all_users_main(argc - 2, argv + 2);
    init_exedir();
    GetFullPathNameA("logs", sizeof(g_log_dir), g_log_dir, NULL);

    /* Create essential directories on first run (relative to exe). */
    char _tmp[MAX_PATH];
    CreateDirectoryA(exepath(_tmp, sizeof(_tmp), "roms"),     NULL);
    CreateDirectoryA(exepath(_tmp, sizeof(_tmp), "cores"),    NULL);
    CreateDirectoryA(exepath(_tmp, sizeof(_tmp), "saves"),    NULL);
    CreateDirectoryA(exepath(_tmp, sizeof(_tmp), "firmware"), NULL);
    CreateDirectoryA(exepath(_tmp, sizeof(_tmp), "lsfg"),     NULL);

    /* settings.yaml is the base layer: load defaults, then YAML overrides them,
       then CLI flags override YAML. Generate default if missing. */
    me_settings_defaults(&g_settings);
    char _settings_path[MAX_PATH];
    exepath(_settings_path, sizeof(_settings_path), "settings.yaml");
    if (me_settings_load(_settings_path, &g_settings) == 1) {
        /* File not found; generate default. */
        me_settings_generate_default(_settings_path);
        /* Reload to pick up the defaults we just wrote. */
        me_settings_load(_settings_path, &g_settings);
    }
    me_app_init(&g_settings, _settings_path);
    if (me_xinput_init())
        printf("[xinput] controller support active (player 1 in slot %d)\n", g_settings.xi_index[0]);
    else
        printf("[xinput] XInput not available; controller input disabled\n");

    g_no_audio      = g_settings.no_audio;
    g_force_gdi     = g_settings.force_gdi;
    g_force_d3d11   = g_settings.force_d3d11;
    g_pace_log      = g_settings.pace_log;
    g_timing_log    = g_settings.timing_log;
    g_latency_log   = g_settings.latency_log;
    g_force_vulkan  = g_settings.force_vulkan;
    g_no_vsync      = g_settings.vk_no_vsync;
    g_env_trace     = g_settings.env_trace;

    /* Vulkan options come from three sources, lowest → highest priority:
       settings.yaml → CLI flag → user-set env var. Settings-derived values are
       pushed into env vars only when the user hasn't already set them; the CLI
       loop and explicit env vars always win because they overwrite. */
    if (g_settings.vk_mailbox  && !getenv("LAGGUELESS_VK_MAILBOX"))  _putenv("LAGGUELESS_VK_MAILBOX=1");
    if (g_settings.vk_validate && !getenv("LAGGUELESS_VK_VALIDATE")) _putenv("LAGGUELESS_VK_VALIDATE=1");

    const char *positional[2] = { NULL, NULL };
    int npos = 0;
    const char *exe = argv[0] ? argv[0] : "laggueless.exe";
    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "--no-audio") == 0) g_no_audio = 1;
        else if (strcmp(argv[i], "--thread-affinity") == 0) g_settings.thread_affinity = 1;
        else if (strcmp(argv[i], "--gdi")      == 0) g_force_gdi = 1;
        else if (strcmp(argv[i], "--d3d11")    == 0) g_force_d3d11 = 1;
        else if (strcmp(argv[i], "--vulkan")   == 0) g_force_vulkan = 1;
        else if (strcmp(argv[i], "--no-vsync") == 0) g_no_vsync = 1;
        else if (strcmp(argv[i], "--vk-exclusive")    == 0) g_settings.vk_exclusive_fullscreen = 1;
        else if (strcmp(argv[i], "--no-vk-exclusive") == 0) g_settings.vk_exclusive_fullscreen = 0;
        else if (strcmp(argv[i], "--lsfg")     == 0) g_lsfg_enabled = 1;
        else if (strncmp(argv[i], "--lsfg-dll=", 11) == 0) {
            if (!GetFullPathNameA(argv[i] + 11, sizeof(g_lsfg_dll_path), g_lsfg_dll_path, NULL))
                snprintf(g_lsfg_dll_path, sizeof(g_lsfg_dll_path), "%s", argv[i] + 11);
            g_lsfg_enabled = 1; /* --lsfg-dll= implies --lsfg */
        }
        else if (strncmp(argv[i], "--lsfg-multiplier=", 18) == 0) {
            g_settings.lsfg_multiplier = atoi(argv[i] + 18);
        }
        else if (strncmp(argv[i], "--lsfg-flow=", 12) == 0) {
            g_settings.lsfg_flow_scale = (float)atof(argv[i] + 12);
        }
        else if (strcmp(argv[i], "--lsfg-perf") == 0) g_settings.lsfg_perf_mode = 1;
        else if (strcmp(argv[i], "--pace-log") == 0) g_pace_log  = 1;
        else if (strcmp(argv[i], "--timing-log") == 0) g_timing_log = 1;
        else if (strcmp(argv[i], "--latency-log") == 0) g_latency_log = 1;
        else if (strcmp(argv[i], "--env-trace") == 0) g_env_trace = 1;
        else if (strcmp(argv[i], "-h")      == 0 || strcmp(argv[i], "--h")     == 0 ||
                strcmp(argv[i], "-help")    == 0 || strcmp(argv[i], "--help")   == 0 ||
                strcmp(argv[i], "---help")  == 0 || strcmp(argv[i], "-Help")    == 0 ||
                strcmp(argv[i], "--Help")   == 0 || strcmp(argv[i], "-HELP")    == 0 ||
                strcmp(argv[i], "--HELP")   == 0 || strcmp(argv[i], "/h")       == 0 ||
                strcmp(argv[i], "/H")       == 0 || strcmp(argv[i], "/help")    == 0 ||
                strcmp(argv[i], "/Help")    == 0 || strcmp(argv[i], "/HELP")    == 0 ||
                strcmp(argv[i], "-?")       == 0 || strcmp(argv[i], "--?")      == 0 ||
                strcmp(argv[i], "/?")       == 0 || strcmp(argv[i], "?")        == 0 ||
                strcmp(argv[i], "help")     == 0 || strcmp(argv[i], "HELP")     == 0 ||
                strcmp(argv[i], "Help")     == 0 || strcmp(argv[i], "-usage")   == 0 ||
                strcmp(argv[i], "--usage")  == 0 || strcmp(argv[i], "/usage")   == 0) {
            printf(
                "laggueless - libretro core front-end\n"
                "\n"
                "usage: %s [options] [[<core.dll>] <rom>]\n"
                "\n"
                "options:\n"
                "  -h, --help, -?, /?, /help    show this help and exit\n"
                "  --no-audio                   disable audio output\n"
                "  --thread-affinity            pin emu thread to P-cores, audio to a separate\n"
                "                                 core; both get elevated OS priority\n"
                "  --gdi                        force GDI for all cores (overrides --d3d11)\n"
                "  --d3d11                      use D3D11 present path for 2D cores too\n"
                "                                 (enables VRR / lower latency, but may tear\n"
                "                                  on non-GSync/FreeSync displays)\n"
                "  --vulkan                     use the Vulkan present path (work in progress;\n"
                "                                 required for LSFG frame generation)\n"
                "  --no-vsync                   (Vulkan only) use IMMEDIATE present mode\n"
                "                                 (allows tearing, lowest latency)\n"
                "  --vk-exclusive               (Vulkan only) acquire exclusive fullscreen\n"
                "                                 (VK_EXT_full_screen_exclusive): bypasses the\n"
                "                                 DWM compositor for the lowest input-to-pixel\n"
                "                                 latency. Engages only while the window covers\n"
                "                                 the whole monitor (fullscreen). (default on)\n"
                "  --no-vk-exclusive            disable exclusive fullscreen (composited swapchain)\n"
                "  --lsfg                       enable LSFG 3.1 frame generation (requires\n"
                "                                 --vulkan and Lossless Scaling on Steam;\n"
                "                                 Lossless.dll is copied into lsfg/ from Steam)\n"
                "  --lsfg-dll=<path>            path to Lossless.dll (overrides lsfg/ folder)\n"
                "  --lsfg-multiplier=N          LSFG output multiplier: 2, 3, or 4 (default 2)\n"
                "  --lsfg-flow=F                LSFG optical-flow scale 0.25..1.0 (default 1.0)\n"
                "  --lsfg-perf                  LSFG performance mode (lower quality, lower GPU cost)\n"
                "  --pace-log                   log audio pacing diagnostics\n"
                "  --timing-log                 log frame timing diagnostics\n"
                "  --latency-log                log per-stage latency (poll/core/present/wait)\n"
                "  --env-trace                  log libretro environment calls\n"
                "\n"
                "arguments:\n"
                "  (none)       open an empty window; drop a ROM onto it to play\n"
                "  <rom>        play a ROM with the core assigned to its file type\n"
                "                 (e.g. .nes -> cores\\mesen_libretro.dll)\n"
                "  <core.dll> <rom>\n"
                "               play a ROM with a specific libretro core DLL\n"
                "\n"
                "Use the window's menu bar (File, Console, Controls, View) to open ROMs,\n"
                "reset, remap controls and hotkeys, and pick the renderer. Dropping a ROM\n"
                "file onto the window also loads it, replacing the current game.\n"
                "\n"
                "default hotkeys (change them in Controls > Hotkeys):\n"
                "  F1      cycle aspect ratio (auto / 1:1 / 4:3 / 16:9)\n"
                "  F11     toggle fullscreen\n"
                "  Ctrl+R  hard reset\n"
                "\n"
                "example:\n"
                "  %s \"roms\\Super Mario Bros. (World).nes\"\n",
                exe, exe);
            return 0;
        }
        else if (argv[i][0] == '-') {
            fprintf(stderr, "unknown flag: %s (try --help)\n", argv[i]); return 1;
        } else if (npos < 2) {
            positional[npos++] = argv[i];
        } else {
            fprintf(stderr, "extra argument: %s (try --help)\n", argv[i]); return 1;
        }
    }

    /* 0 args: empty window. 1 arg: ROM, core from the console table (an
       empty window asking for its console, when its type is shared and the
       header doesn't say). 2 args: explicit core + ROM. */
    char table_core_path[MAX_PATH], pick_path[MAX_PATH] = "";
    const char *core_path = NULL;
    const char *rom_path  = NULL;
    int console = -1, remembered = -1;
    me_console_set candidates = 0;
    if (npos == 2) {
        core_path = positional[0];
        rom_path  = positional[1];
        console   = console_for_core(rom_path, core_path);
    } else if (npos == 1) {
        char err[MAX_PATH * 2];
        console = remembered = remembered_console(positional[0]);
        int rc = resolve_rom_core(positional[0], &console, &candidates,
                                  table_core_path, sizeof(table_core_path), err, sizeof(err));
        if (rc == ME_ROM_AMBIGUOUS) {
            if (!GetFullPathNameA(positional[0], sizeof(pick_path), pick_path, NULL))
                snprintf(pick_path, sizeof(pick_path), "%s", positional[0]);
        } else if (rc != 0) {
            fprintf(stderr, "[rom] %s\n", err);
            me_ui_notify_error("%s", err);
            return 1;
        } else {
            rom_path  = positional[0];
            core_path = table_core_path;
        }
    }

    /* ---- Step B1: Load Lossless.dll + extract shaders --------------------- */
    /* --lsfg forces Vulkan and fails hard; frame gen switched on from the
       View menu (settings.yaml) only applies when Vulkan is the backend, and
       a missing Lossless.dll just leaves it off. */
    int lsfg_from_cli = g_lsfg_enabled;
    if (!g_lsfg_enabled && g_settings.lsfg_enabled) g_lsfg_enabled = 1;
    if (g_lsfg_enabled) {
        if (lsfg_from_cli && !g_force_vulkan) {
            fprintf(stderr,
                "[lsfg] WARNING: --lsfg requires --vulkan. Enabling Vulkan automatically.\n");
            g_force_vulkan = 1;
        }
        g_lsfg_shaders = me_lsfg_load(g_lsfg_dll_path[0] ? g_lsfg_dll_path : NULL);
        if (!g_lsfg_shaders && lsfg_from_cli) {
            /* Error already printed by me_lsfg_load(). Exit cleanly. */
            return 1;
        }
    }
    if (g_lsfg_enabled && !g_lsfg_shaders) {
        fprintf(stderr, "[lsfg] frame gen is on in settings.yaml but Lossless.dll wasn't found; leaving it off\n");
        g_lsfg_enabled = 0;
    }
    /* The View menu's checkmark shows what's actually in effect. */
    g_settings.lsfg_enabled = g_lsfg_enabled;
    if (g_lsfg_shaders) {
        fprintf(stderr, "[lsfg] DLL: %s\n", me_lsfg_dll_path(g_lsfg_shaders));
        fprintf(stderr, "[lsfg] shaders extracted: %d\n",
                me_lsfg_shader_count(g_lsfg_shaders));
        /* Sanity check: Lossless.dll should have at least 30 shader resources */
        if (me_lsfg_shader_count(g_lsfg_shaders) < 30) {
            fprintf(stderr,
                "[lsfg] WARNING: only %d shader resources found (expected 30+).\n"
                "[lsfg]          This may not be a valid Lossless Scaling DLL.\n",
                me_lsfg_shader_count(g_lsfg_shaders));
        }
        fprintf(stderr, "[lsfg] Step B1 OK — DLL loaded, shaders ready\n");
    }

    /* Windows' default Sleep granularity is ~15.6 ms; frame pacing needs
       1 ms resolution. */
    timeBeginPeriod(1);
    me_corelog_start();

    me_session session;
    memset(&session, 0, sizeof(session));
    int exit_code = 0;
    if (rom_path) {
        /* A failure after the window is up leaves it idle, showing why. */
        int rc = open_game(&session, core_path, rom_path, console);
        if (rc != 0 && !g_hwnd) exit_code = 1;
        if (rc == 0 && remembered >= 0) remember_console(rom_path, console);   /* most recent */
    } else if (create_main_window(640, 480) == 0) {
        me_platform_set_idle(g_hwnd, 1);
        publish_status(&session);
        if (pick_path[0]) {
            printf("[main] several consoles use this file type; asking which\n");
            me_ui_pick_console(pick_path, candidates, -1);
        } else {
            printf("[main] no game loaded; use File > Open ROM or drop a ROM onto the window\n");
        }
    } else {
        exit_code = 1;
    }

    while (exit_code == 0 && me_platform_pump()) {
        me_cmd cmd;
        while (me_cmd_take(&cmd)) run_command(&session, &cmd);
        if (session.active) {
            session_run_frame(&session);
        } else {
            /* Nothing to emulate: keep hotkeys (fullscreen, quit) working and
               sleep until a command arrives or ~one frame passes. */
            handle_hotkeys();
            WaitForSingleObject(me_app_wake_event(), 16);
        }
    }

    if (session.active) session_close(&session);
    me_corelog_stop();
    me_platform_destroy_window();
    timeEndPeriod(1);
    me_lsfg_free(g_lsfg_shaders); g_lsfg_shaders = NULL;
    me_settings_free(&g_settings);
    me_log_close_all();
    return exit_code;
}
