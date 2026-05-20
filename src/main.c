#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include "platform_win32.h"
#include <mmsystem.h>
#include "integer_scaling.h"
#include "core_loader.h"
#include "audio_wasapi.h"
#include "render_d3d11.h"
#include "gl_context.h"
#include "libretro.h"
#include "settings.h"

/* ---- global state for callbacks (single core, single ROM) ----------------- */
static enum retro_pixel_format g_pixel_format = RETRO_PIXEL_FORMAT_0RGB1555;

/* BGRX backbuffer (32 bpp, top-down via negative bmi height). */
static u32 *g_back = NULL;
static unsigned g_back_max_w = 0, g_back_max_h = 0;
static unsigned g_frame_w = 0, g_frame_h = 0;
static unsigned long g_video_calls = 0;

static HWND g_hwnd = NULL;
static int  g_use_d3d11 = 0;
static int  g_force_vulkan = 0; /* --vulkan requested on the CLI */

/* Latency telemetry: timestamp of the most recent input poll callback. Read
   by the main loop to split retro_run() into "before poll" and "after poll"
   stages. Cores call poll exactly once per retro_run(), so this is a clean
   split point. 0 means "no poll happened this retro_run()" (some cores
   may skip polls during run-ahead's muted advance). */
static int           g_latency_log = 0;
static LARGE_INTEGER g_poll_qpc = {0};

/* Aspect mode: 0 = 1:1 (square pixels), 1 = 4:3, 2 = 16:9. F1 cycles. */
static int g_aspect_mode = 0;
static const char *g_aspect_names[3] = { "1:1", "4:3", "16:9" };
static const int g_aspect_x[3] = { 1, 4, 16 };
static const int g_aspect_y[3] = { 1, 3,  9 };

/* Set when a core's log message looks like a missing-firmware/BIOS error.
   Cores typically continue running but the game hangs at first use of the
   missing chip; surfacing this prominently makes the cause obvious. */
static int g_firmware_warned = 0;

static int looks_like_firmware_error(const char *s) {
    if (!s) return 0;
    /* Case-insensitive substring search for any of a few generic phrases.
       Different cores phrase it differently ("firmware file", "BIOS file
       not found", "missing BIOS", "required ROM"); these cover the common
       formulations across SNES coprocessors, GBA BIOS, PSX/Saturn BIOS,
       N64 64DD IPL, etc. */
    static const char *needles[] = {
        "firmware", "BIOS", "bios", "Could not find", "required ROM",
        "not found in system", NULL
    };
    for (const char **n = needles; *n; n++) {
        const char *p = s;
        size_t nl = strlen(*n);
        while (*p) {
            if (_strnicmp(p, *n, nl) == 0) return 1;
            p++;
        }
    }
    return 0;
}

/* ---- log callback --------------------------------------------------------- */
static void me_log_cb(enum retro_log_level level, const char *fmt, ...) {
    const char *tag = "?";
    switch (level) {
        case RETRO_LOG_DEBUG: tag = "DBG"; break;
        case RETRO_LOG_INFO:  tag = "INF"; break;
        case RETRO_LOG_WARN:  tag = "WRN"; break;
        case RETRO_LOG_ERROR: tag = "ERR"; break;
        default: break;
    }
    /* Format once into a buffer so we can both print it and scan it. */
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) { fprintf(stderr, "[core:%s] (format error)\n", tag); return; }
    fprintf(stderr, "[core:%s] %s", tag, buf);
    /* Heuristic: a "could not find ... .rom" / "firmware missing" style line.
       Print a prominent warning the first time and remember so we can also
       surface it at exit if the game gets stuck. */
    if (!g_firmware_warned && looks_like_firmware_error(buf)) {
        g_firmware_warned = 1;
        fprintf(stderr,
                "[firmware] WARNING: the core reported a missing firmware/BIOS file.\n"
                "[firmware] The game may freeze or behave incorrectly when it tries to use\n"
                "[firmware] the missing chip. Place the required file in the system\n"
                "[firmware] directory (./firmware) and restart.\n");
        fflush(stderr);
    }
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

static FILE *me_log_open(me_log_stream s) {
    if (s >= ME_LOG_COUNT) return NULL;
    if (g_log_files[s]) return g_log_files[s];
    if (!g_log_dir_tried) {
        g_log_dir_tried = 1;
        CreateDirectoryA("logs", NULL); /* ignore ALREADY_EXISTS */
    }
    char path[64];
    snprintf(path, sizeof(path), "logs\\%s.log", g_log_names[s]);
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
}

static void me_log_close_all(void) {
    for (int i = 0; i < ME_LOG_COUNT; i++) {
        if (g_log_files[i]) { fclose(g_log_files[i]); g_log_files[i] = NULL; }
    }
}

/* Core options storage. SET_VARIABLES hands us a { key, "Desc; v1|v2|v3" }
   array terminated by { NULL, NULL }. At SET time we walk it once, strdup
   each default ("v1") into a parallel array. GET_VARIABLE then returns the
   precomputed default string. */
struct me_var { char *key; char *def_value; };
static struct me_var *g_vars = NULL;
static size_t g_var_count = 0;

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

static bool me_environment_cb(unsigned cmd, void *data) {
    /* The "experimental" bit is set on some env IDs; mask it for matching. */
    unsigned base = cmd & ~RETRO_ENVIRONMENT_EXPERIMENTAL;
    switch (base) {
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: {
            /* Resolve to absolute on first use. Some cores (mupen64plus-next
               in particular) pass this straight into LoadLibrary / fopen
               for plugins/INIs and break on relative paths. */
            static char sysdir[MAX_PATH] = {0};
            if (sysdir[0] == 0) GetFullPathNameA("firmware", MAX_PATH, sysdir, NULL);
            if (data) *(const char **)data = sysdir;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: {
            static char savedir[MAX_PATH] = {0};
            if (savedir[0] == 0) GetFullPathNameA("firmware", MAX_PATH, savedir, NULL);
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
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
            if (data) ((struct retro_log_callback *)data)->log = me_log_cb;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            struct retro_variable *v = (struct retro_variable *)data;
            if (!v) return false;
            v->value = me_var_default_for(v->key);
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
        case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
            if (g_env_trace) me_log(ME_LOG_ENV, "[env] SET_CONTROLLER_INFO -> true\n");
            return true;  /* 35 */
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
/* Player 1 RetroPad state, refreshed in input_poll. The active control map
   is selected at startup based on settings.yaml: either `g_settings.universal`
   or the per-core entry's controls. */
static int16_t g_pad1[16];
static int16_t g_analog_lx = 0, g_analog_ly = 0;  /* left stick, -32767..32767 */
static int16_t g_analog_rx = 0, g_analog_ry = 0;  /* right stick */

static const me_control_map *g_active_map = NULL;
static me_settings g_settings;

/* Some cores (NES, GB/GBC, PCE, SMS, ...) only have 2 face buttons. Several of
   them repurpose RetroPad X/Y as turbo-A/turbo-B, which we never want. Set
   based on the core's library_name after retro_get_system_info. */
static int g_suppress_xy = 0;

static int core_lacks_xy(const char *library_name) {
    if (!library_name) return 0;
    static const char *const names[] = {
        "Mesen", "Nestopia", "FCEUmm", "QuickNES",          /* NES */
        "Gambatte", "SameBoy", "TGB Dual", "VBA-M",         /* GB/GBC */
        "Mednafen PCE", "Mednafen SuperGrafx", "Beetle PCE",/* PC Engine */
        /* Genesis Plus GX / PicoDrive intentionally omitted: they also run
           Mega Drive, which has 6-button pads using X/Y. */
    };
    for (size_t i = 0; i < sizeof(names)/sizeof(names[0]); i++) {
        if (strstr(library_name, names[i])) return 1;
    }
    return 0;
}

static int key_down_kb(const me_kb_binding *b) {
    if (b->vk == 0) return 0;
    if (b->ctrl  && !(GetAsyncKeyState(VK_CONTROL) & 0x8000)) return 0;
    if (b->alt   && !(GetAsyncKeyState(VK_MENU)    & 0x8000)) return 0;
    if (b->shift && !(GetAsyncKeyState(VK_SHIFT)   & 0x8000)) return 0;
    return (GetAsyncKeyState((int)b->vk) & 0x8000) ? 1 : 0;
}

static int input_down(me_input_id id) {
    if (!g_active_map) return 0;
    return key_down_kb(&g_active_map->keys[id]);
}

static void me_input_poll_cb(void) {
    if (g_latency_log) QueryPerformanceCounter(&g_poll_qpc);
    /* Don't read keys when our window isn't foreground. */
    if (GetForegroundWindow() != g_hwnd) {
        memset(g_pad1, 0, sizeof(g_pad1));
        g_analog_lx = g_analog_ly = g_analog_rx = g_analog_ry = 0;
        return;
    }
    int up    = input_down(ME_IN_DPAD_UP);
    int down  = input_down(ME_IN_DPAD_DOWN);
    int lf    = input_down(ME_IN_DPAD_LEFT);
    int right = input_down(ME_IN_DPAD_RIGHT);
    /* SOCD: opposing directions cancel to neutral. */
    if (up && down)  { up = down = 0; }
    if (lf && right) { lf = right = 0; }
    g_pad1[RETRO_DEVICE_ID_JOYPAD_UP]     = up;
    g_pad1[RETRO_DEVICE_ID_JOYPAD_DOWN]   = down;
    g_pad1[RETRO_DEVICE_ID_JOYPAD_LEFT]   = lf;
    g_pad1[RETRO_DEVICE_ID_JOYPAD_RIGHT]  = right;
    g_pad1[RETRO_DEVICE_ID_JOYPAD_B]      = input_down(ME_IN_B);
    g_pad1[RETRO_DEVICE_ID_JOYPAD_A]      = input_down(ME_IN_A);
    g_pad1[RETRO_DEVICE_ID_JOYPAD_Y]      = g_suppress_xy ? 0 : input_down(ME_IN_Y);
    g_pad1[RETRO_DEVICE_ID_JOYPAD_X]      = g_suppress_xy ? 0 : input_down(ME_IN_X);
    g_pad1[RETRO_DEVICE_ID_JOYPAD_START]  = input_down(ME_IN_START);
    g_pad1[RETRO_DEVICE_ID_JOYPAD_SELECT] = input_down(ME_IN_BACK);
    g_pad1[RETRO_DEVICE_ID_JOYPAD_L]      = input_down(ME_IN_LB);
    g_pad1[RETRO_DEVICE_ID_JOYPAD_R]      = input_down(ME_IN_RB);
    g_pad1[RETRO_DEVICE_ID_JOYPAD_L2]     = input_down(ME_IN_LT);
    g_pad1[RETRO_DEVICE_ID_JOYPAD_R2]     = input_down(ME_IN_RT);
    g_pad1[RETRO_DEVICE_ID_JOYPAD_L3]     = input_down(ME_IN_LSTICK);
    g_pad1[RETRO_DEVICE_ID_JOYPAD_R3]     = input_down(ME_IN_RSTICK);

    /* Stick directions are also surfaced through RETRO_DEVICE_ANALOG so cores
       like mupen64plus_next that read the analog stick see motion. Opposing
       directions cancel; non-opposing produce full deflection in that axis. */
    int lu = input_down(ME_IN_LSTICK_UP),    ld = input_down(ME_IN_LSTICK_DOWN);
    int ll = input_down(ME_IN_LSTICK_LEFT),  lr = input_down(ME_IN_LSTICK_RIGHT);
    int ru = input_down(ME_IN_RSTICK_UP),    rd = input_down(ME_IN_RSTICK_DOWN);
    int rl = input_down(ME_IN_RSTICK_LEFT),  rr = input_down(ME_IN_RSTICK_RIGHT);
    if (lu && ld) lu = ld = 0;
    if (ll && lr) ll = lr = 0;
    if (ru && rd) ru = rd = 0;
    if (rl && rr) rl = rr = 0;
    g_analog_lx = (int16_t)((lr ? 32767 : 0) - (ll ? 32767 : 0));
    g_analog_ly = (int16_t)((ld ? 32767 : 0) - (lu ? 32767 : 0));
    g_analog_rx = (int16_t)((rr ? 32767 : 0) - (rl ? 32767 : 0));
    g_analog_ry = (int16_t)((rd ? 32767 : 0) - (ru ? 32767 : 0));
}

static int16_t me_input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id) {
    if (port != 0) return 0;
    if (device == RETRO_DEVICE_JOYPAD) {
        if (id >= sizeof(g_pad1) / sizeof(g_pad1[0])) return 0;
        return g_pad1[id];
    }
    if (device == RETRO_DEVICE_ANALOG) {
        if (index == RETRO_DEVICE_INDEX_ANALOG_LEFT) {
            if (id == RETRO_DEVICE_ID_ANALOG_X) return g_analog_lx;
            if (id == RETRO_DEVICE_ID_ANALOG_Y) return g_analog_ly;
        } else if (index == RETRO_DEVICE_INDEX_ANALOG_RIGHT) {
            if (id == RETRO_DEVICE_ID_ANALOG_X) return g_analog_rx;
            if (id == RETRO_DEVICE_ID_ANALOG_Y) return g_analog_ry;
        }
        return 0;
    }
    return 0;
}

/* ---- present -------------------------------------------------------------- */
static void present(HWND hwnd) {
    if (!g_back || g_frame_w == 0 || g_frame_h == 0) return;

    RECT cr;
    GetClientRect(hwnd, &cr);
    int cw = cr.right - cr.left;
    int ch = cr.bottom - cr.top;
    if (cw <= 0 || ch <= 0) return;

    /* Integer-scale to the current target aspect inside the client area. */
    i32 rx, ry;
    me_iscale_ratios(cw, ch, (i32)g_frame_w, (i32)g_frame_h,
                     g_aspect_x[g_aspect_mode], g_aspect_y[g_aspect_mode], &rx, &ry);
    int dw = (int)g_frame_w * rx;
    int dh = (int)g_frame_h * ry;
    if (dw > cw) dw = cw;
    if (dh > ch) dh = ch;
    int dx = (cw - dw) / 2;
    int dy = (ch - dh) / 2;

    /* D3D11 flip-model path: HW (GL) cores always, software cores when the
       user opts in via --d3d11. Software cores default to GDI because DWM
       composites it at the desktop refresh — no visible tearing on a
       non-VRR display, and frame-perfect inputs land as expected. */
    if (g_use_d3d11) {
        me_d3d11_upload(g_back, g_frame_w, g_frame_h, g_back_max_w);
        me_d3d11_present(cw, ch, dx, dy, dw, dh, g_frame_w, g_frame_h);
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
    bmi.bmiHeader.biWidth       = (LONG)g_frame_w;
    bmi.bmiHeader.biHeight      = -(LONG)g_frame_h;
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    /* Tightly pack the active region (StretchDIBits with src rect smaller
       than DIB width is unreliable on some GDI paths). */
    static u32 *tight = NULL;
    static size_t tight_cap = 0;
    size_t need = (size_t)g_frame_w * g_frame_h;
    if (need > tight_cap) {
        free(tight);
        tight = (u32 *)malloc(need * sizeof(u32));
        tight_cap = need;
    }
    if (tight) {
        for (unsigned y = 0; y < g_frame_h; y++) {
            memcpy(tight + y * g_frame_w, g_back + y * g_back_max_w, g_frame_w * sizeof(u32));
        }
        SetStretchBltMode(hdc, COLORONCOLOR);
        StretchDIBits(hdc,
                      dx, dy, dw, dh,
                      0, 0, (int)g_frame_w, (int)g_frame_h,
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
    int n = snprintf(dir, sizeof(dir), "saves\\%s", core_base);
    if (n < 0 || (size_t)n >= sizeof(dir)) return -1;
    CreateDirectoryA("saves", NULL);
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
    fprintf(stderr, "[crash] unhandled exception 0x%08lx at %p (%s+0x%llx)\n",
            (unsigned long)code, addr, modname, (unsigned long long)off);
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

int main(int argc, char **argv) {
    SetUnhandledExceptionFilter(me_unhandled_exception);

    /* settings.yaml is the base layer: load defaults, then YAML overrides them,
       then CLI flags override YAML. */
    me_settings_defaults(&g_settings);
    me_settings_load("settings.yaml", &g_settings);
    int no_audio       = g_settings.no_audio;
    int force_gdi      = g_settings.force_gdi;
    int force_d3d11 = g_settings.force_d3d11;
    int pace_log    = g_settings.pace_log;
    int timing_log  = g_settings.timing_log;
    int latency_log = g_settings.latency_log;
    g_aspect_mode   = (int)g_settings.aspect;
    g_env_trace     = g_settings.env_trace;

    const char *positional[2] = { NULL, NULL };
    int npos = 0;
    const char *exe = argv[0] ? argv[0] : "laggueless.exe";
    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "--no-audio") == 0) no_audio  = 1;
        else if (strcmp(argv[i], "--thread-affinity") == 0) g_settings.thread_affinity = 1;
        else if (strcmp(argv[i], "--gdi")      == 0) force_gdi = 1;
        else if (strcmp(argv[i], "--d3d11")    == 0) force_d3d11 = 1;
        else if (strcmp(argv[i], "--vulkan")   == 0) g_force_vulkan = 1;
        else if (strcmp(argv[i], "--pace-log") == 0) pace_log  = 1;
        else if (strcmp(argv[i], "--timing-log") == 0) timing_log = 1;
        else if (strcmp(argv[i], "--latency-log") == 0) latency_log = 1;
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
                "usage: %s [options] <core.dll> <rom>\n"
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
                "  --pace-log                   log audio pacing diagnostics\n"
                "  --timing-log                 log frame timing diagnostics\n"
                "  --latency-log                log per-stage latency (poll/core/present/wait)\n"
                "  --env-trace                  log libretro environment calls\n"
                "\n"
                "arguments:\n"
                "  <core.dll>   path to a libretro core DLL (see example-cores/)\n"
                "  <rom>        path to a ROM file the core supports\n"
                "\n"
                "hotkeys (while running):\n"
                "  F1   cycle aspect ratio (1:1 / 4:3 / 16:9)\n"
                "  F11  toggle fullscreen\n"
                "\n"
                "example:\n"
                "  %s example-cores\\mesen_libretro.dll \"example-roms\\Super Mario Bros. (World).nes\"\n",
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
    if (npos < 2) {
        fprintf(stderr, "usage: %s [options] <core.dll> <rom>\n", exe);
        fprintf(stderr, "try '%s --help' for more information\n", exe);
        return 1;
    }
    const char *core_path = positional[0];
    const char *rom_path  = positional[1];

    /* Pick the active per-player-1 control map: per-core entry if it exists
       and has use_universal=false, otherwise the universal map. */
    {
        const struct me_core_entry *ce = me_settings_find_core(&g_settings, core_path);
        if (ce && !ce->use_universal) {
            g_active_map = &ce->controls;
            printf("[settings] using per-core controls for %s\n", ce->name);
        } else {
            g_active_map = &g_settings.universal;
            printf("[settings] using universal controls%s%s\n",
                   ce ? " for " : "", ce ? ce->name : "");
        }
    }

    me_core *core = me_core_load(core_path);
    if (!core) { fprintf(stderr, "failed to load core: %s\n", core_path); return 1; }

    unsigned api = core->retro_api_version();
    printf("[core] retro_api_version = %u\n", api);

    struct retro_system_info info = {0};
    core->retro_get_system_info(&info);
    printf("[core] %s %s (ext=%s, need_fullpath=%d)\n",
           info.library_name ? info.library_name : "?",
           info.library_version ? info.library_version : "?",
           info.valid_extensions ? info.valid_extensions : "?",
           (int)info.need_fullpath);

    g_suppress_xy = core_lacks_xy(info.library_name);
    if (g_suppress_xy) {
        printf("[input] suppressing RetroPad X/Y for this core (no turbo)\n");
    }

    fprintf(stderr, "[load] set_environment\n"); fflush(stderr);
    core->retro_set_environment(me_environment_cb);
    /* Some cores (Mesen) allocate internal state in retro_init() that the
       callback setters dereference. Call retro_init before the setters.
       Compliant cores treat the setters as pointer stores so order is safe. */
    fprintf(stderr, "[load] retro_init()\n"); fflush(stderr);
    core->retro_init();
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
    unsigned char *rom_data = NULL;
    size_t rom_size = 0;
    if (!info.need_fullpath) {
        rom_data = slurp(rom_path, &rom_size);
        if (!rom_data) { fprintf(stderr, "failed to read ROM: %s\n", rom_path); return 1; }
        game.data = rom_data;
        game.size = rom_size;
    }
    fprintf(stderr, "[load] retro_load_game(path=%s, data=%p, size=%zu)\n",
            game.path ? game.path : "(null)", game.data, game.size);
    fflush(stderr);

    bool loaded = core->retro_load_game(&game);
    fprintf(stderr, "[load] retro_load_game -> %s\n", loaded ? "true" : "false");
    fflush(stderr);
    if (!loaded) {
        fprintf(stderr, "retro_load_game failed\n");
        fflush(stderr);
        return 1;
    }

    char save_path[MAX_PATH] = {0};
    if (me_build_save_path(core_path, rom_path, save_path, sizeof(save_path)) == 0) {
        me_sram_load(core, save_path);
    } else {
        fprintf(stderr, "[save] could not derive save path; saves disabled\n");
    }

    /* SRAM dirty-poll state. We hash SRAM every ~1s and, once the hash
       stabilizes for one additional poll, flush to disk. The stability check
       avoids writing mid-update when the core is still mutating the buffer
       (e.g. a multi-byte checksum being recomputed by the cart). */
    uint64_t sram_disk_hash = 0;     /* hash of last bytes we wrote to disk */
    uint64_t sram_last_hash = 0;     /* hash from previous poll tick */
    int      sram_pending  = 0;      /* hash changed; waiting for it to settle */
    DWORD    sram_last_poll_ms = GetTickCount();
    if (save_path[0] && core->retro_get_memory_data && core->retro_get_memory_size) {
        void *mem = core->retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
        size_t sz = core->retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
        if (mem && sz) {
            sram_disk_hash = sram_last_hash = me_fnv1a64(mem, sz);
        }
    }

    struct retro_system_av_info av = {0};
    core->retro_get_system_av_info(&av);
    printf("[av] base=%ux%u max=%ux%u aspect=%.4f fps=%.4f rate=%.1f\n",
           av.geometry.base_width, av.geometry.base_height,
           av.geometry.max_width,  av.geometry.max_height,
           av.geometry.aspect_ratio, av.timing.fps, av.timing.sample_rate);

    /* Allocate backbuffer sized to max geometry. */
    g_back_max_w = av.geometry.max_width  ? av.geometry.max_width  : av.geometry.base_width;
    g_back_max_h = av.geometry.max_height ? av.geometry.max_height : av.geometry.base_height;
    g_back = (u32 *)calloc((size_t)g_back_max_w * g_back_max_h, sizeof(u32));
    if (!g_back) { fprintf(stderr, "backbuffer alloc failed\n"); return 1; }

    /* HW path: now that we know max geometry, build the FBO and fire the
       core's context_reset so it can upload its shaders/VBOs. retro_load_game
       already returned, but cores designed around hw_render defer all GL
       resource creation until context_reset — exactly because the frontend
       may not have a context ready at load time. */
    if (g_hw_render_accepted) {
        if (me_gl_fbo_create(g_back_max_w, g_back_max_h,
                             g_hw_render.depth, g_hw_render.stencil) != 0) {
            fprintf(stderr, "[hw] FBO creation failed\n");
            return 1;
        }
        if (g_hw_render.context_reset) {
            fprintf(stderr, "[hw] calling context_reset\n"); fflush(stderr);
            g_hw_render.context_reset();
        }
    }
    g_frame_w = av.geometry.base_width;
    g_frame_h = av.geometry.base_height;

    /* Create window sized to a reasonable 2× of base geometry. */
    int win_w = (int)(av.geometry.base_width  * 2);
    int win_h = (int)(av.geometry.base_height * 2);
    if (win_w < 320) win_w = 640;
    if (win_h < 240) win_h = 480;
    g_hwnd = me_platform_create_window("laggueless", win_w, win_h);
    if (!g_hwnd) { fprintf(stderr, "window create failed\n"); return 1; }
    if (g_settings.fullscreen_on_launch) {
        me_platform_toggle_fullscreen(g_hwnd);
    }

    /* D3D11 flip-model is used for HW (GL) cores by default and for software
       cores when --d3d11 is set. Otherwise software cores stay on GDI: lower
       visible tearing on non-VRR displays. --gdi overrides everything. */
    if (!force_gdi && (g_hw_render_accepted || force_d3d11)) {
        if (me_d3d11_init(g_hwnd, g_back_max_w, g_back_max_h) == 0) {
            g_use_d3d11 = 1;
        } else {
            fprintf(stderr, "[render] D3D11 init failed, falling back to GDI\n");
        }
    } else if (force_gdi) {
        printf("[render] --gdi forced\n");
    }

    if (g_force_vulkan) {
#ifdef ME_HAVE_VULKAN
        printf("[render] --vulkan requested (Vulkan path not yet implemented, falling back to default render path)\n");
#else
        printf("[render] --vulkan requested but build has no Vulkan support (set VULKAN_SDK and rebuild)\n");
#endif
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

    /* Refresh-rate matching: if the monitor's actual rate is within 5% of the
       core's nominal fps, snap the pacing deadline to the display's period.
       Removes residual judder when core Hz ≠ display Hz (e.g. 60.10 vs 59.94)
       at the cost of a tiny core-vs-device clock mismatch the audio DRC
       already absorbs. Opt-in via settings.yaml. */
    if (g_settings.match_display_hz) {
        HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFOEXA mi = {0};
        mi.cbSize = sizeof(mi);
        double disp_hz = 0.0;
        if (GetMonitorInfoA(mon, (MONITORINFO *)&mi)) {
            DEVMODEA dm = {0};
            dm.dmSize = sizeof(dm);
            if (EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)
                && dm.dmDisplayFrequency > 1) {
                /* dmDisplayFrequency is integer Hz. Treat 59 as 59.94 (NTSC). */
                disp_hz = (dm.dmDisplayFrequency == 59) ? 59.94 : (double)dm.dmDisplayFrequency;
            }
        }
        if (disp_hz > 1.0) {
            double ratio = disp_hz / fps;
            if (ratio > 0.95 && ratio < 1.05) {
                printf("[pace] match_display_hz: core %.4f -> display %.4f Hz\n", fps, disp_hz);
                fps = disp_hz;
            } else {
                printf("[pace] match_display_hz: display %.4f Hz outside 5%% of core %.4f; keeping core rate\n",
                       disp_hz, fps);
            }
        } else {
            printf("[pace] match_display_hz: could not query display refresh; keeping core rate\n");
        }
    }
    int audio_ok = 0;
    if (no_audio) {
        printf("[audio] disabled via --no-audio; using Sleep-based pacing\n");
        g_dev_rate = g_core_rate;
    } else {
        audio_ok = (me_audio_init(&g_dev_rate) == 0);
        if (!audio_ok) {
            fprintf(stderr, "[audio] init failed; falling back to Sleep pacing\n");
            g_dev_rate = g_core_rate;
        } else {
            g_audio_active = 1;
        }
    }
    size_t frame_audio = (size_t)(g_dev_rate / fps + 0.5);
    /* Pacing target: keep about 30 ms buffered in the ring. Cores deliver
       audio in per-frame bursts (~16.6 ms at 60 fps), and the WASAPI buffer
       drains in ~20 ms cycles. 30 ms gives ~13 ms of headroom over the
       largest single drain/push event, which keeps the ring above empty
       across jittery delivery without bloating latency. Lower than this
       (e.g. 20 ms) causes underruns in cores like Mesen that produce one
       big batch per frame at exactly the device rate. */
    size_t target_buffered = (size_t)(g_dev_rate * 0.030);

    /* Video pacing is QPC absolute-deadline + spin-wait, regardless of audio.
       Audio is kept in sync via a small bias on the resampler ratio (dynamic
       rate control), NOT by skipping or duplicating frames. Windows' default
       Sleep granularity is ~15.6 ms, so we request 1 ms resolution. */
    double frame_period_ms = 1000.0 / fps;
    LARGE_INTEGER qpf, qstart;
    QueryPerformanceFrequency(&qpf);
    timeBeginPeriod(1);
    /* Anchor qstart AFTER any one-time setup so the very first frame's
       deadline doesn't start out late. */
    QueryPerformanceCounter(&qstart);
    unsigned long frame_count = 0;

    /* Per-second rollup for --pace-log. */
    double pl_gap_min = 1e9, pl_gap_max = 0, pl_gap_sum = 0;
    size_t pl_fill_before_min = (size_t)-1, pl_fill_before_max = 0, pl_fill_before_sum = 0;
    size_t pl_fill_after_min  = (size_t)-1, pl_fill_after_max  = 0, pl_fill_after_sum  = 0;
    unsigned pl_iters = 0, pl_timeouts = 0;
    LARGE_INTEGER pl_last_qpc; QueryPerformanceCounter(&pl_last_qpc);
    LARGE_INTEGER pl_window_start = pl_last_qpc;

    /* Run-ahead setup. Disabled for HW (GL) cores: savestates don't capture GL
       context state, and re-running a frame with GL side-effects (FBO writes,
       texture uploads) would corrupt visible output. Software cores serialize
       to a flat byte buffer that round-trips cleanly. */
    int ra_frames = g_settings.runahead_frames;
    if (ra_frames > 0 && g_hw_render_accepted) {
        printf("[runahead] disabled for hardware-rendered cores\n");
        ra_frames = 0;
    }
    size_t ra_state_size = 0;
    void  *ra_state_buf  = NULL;
    if (ra_frames > 0) {
        if (!core->retro_serialize_size || !core->retro_serialize || !core->retro_unserialize) {
            printf("[runahead] core lacks serialize support; disabling\n");
            ra_frames = 0;
        } else {
            ra_state_size = core->retro_serialize_size();
            if (ra_state_size == 0) {
                printf("[runahead] core reports zero state size; disabling\n");
                ra_frames = 0;
            } else {
                ra_state_buf = malloc(ra_state_size);
                if (!ra_state_buf) {
                    fprintf(stderr, "[runahead] state buffer alloc failed (%zu bytes); disabling\n",
                            ra_state_size);
                    ra_frames = 0;
                } else {
                    printf("[runahead] enabled: %d frame%s ahead, state=%zu bytes\n",
                           ra_frames, ra_frames == 1 ? "" : "s", ra_state_size);
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
            if (audio_ok) {
                me_audio_set_thread_affinity((unsigned long long)audio_mask,
                                            THREAD_PRIORITY_TIME_CRITICAL);
                printf("[affinity] audio thread pinned to mask 0x%llx, priority=TIME_CRITICAL\n",
                       (unsigned long long)audio_mask);
            }
        } else {
            printf("[affinity] could not split cores (single-core or API unavailable); priority-only\n");
            if (audio_ok)
                me_audio_set_thread_affinity(0, THREAD_PRIORITY_TIME_CRITICAL);
        }
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        printf("[affinity] emu thread priority=HIGHEST\n");
    }

    g_latency_log = latency_log;
    /* Per-stage latency telemetry: split each iteration into
         backpressure_wait (DXGI waitable)
         pre_poll          (top of retro_run until the core polls input)
         post_poll         (rest of retro_run after the poll callback)
         present           (frame upload + Present)
         pace_wait         (Sleep + spin to absolute QPC deadline)
       Sums and a frame-N marker are printed each second. Hot-path cost
       when disabled: one branch in me_input_poll_cb and at each
       checkpoint — negligible. */
    double ll_pre_sum = 0, ll_post_sum = 0, ll_present_sum = 0;
    double ll_pace_sum = 0, ll_back_sum = 0;
    double ll_iter_max = 0;
    unsigned ll_iters = 0, ll_missed_polls = 0;
    LARGE_INTEGER ll_window_start; QueryPerformanceCounter(&ll_window_start);
    unsigned long ll_window_first_frame = 0;

    while (me_platform_pump()) {
        LARGE_INTEGER ll_t0; if (latency_log) QueryPerformanceCounter(&ll_t0);
        /* DXGI 1.3 waitable swap chain backpressure: with max frame latency
           pinned to 1, this blocks until the previous Present has been
           consumed by the compositor. Keeps CPU exactly one frame ahead of
           GPU instead of letting Present queue frames silently. Pre-Win8.1
           and the GDI present path return 0 here and rely on the QPC tail
           below for pacing. */
        if (g_use_d3d11) me_d3d11_wait_for_present(1000);
        LARGE_INTEGER ll_t_after_wait; if (latency_log) QueryPerformanceCounter(&ll_t_after_wait);
        const me_kb_binding *hk;
        hk = &g_settings.hk_toggle_fullscreen;
        if (me_platform_key_pressed(hk->vk, hk->ctrl, hk->alt, hk->shift)) {
            me_platform_toggle_fullscreen(g_hwnd);
        }
        hk = &g_settings.hk_cycle_aspect;
        if (me_platform_key_pressed(hk->vk, hk->ctrl, hk->alt, hk->shift)) {
            g_aspect_mode = (g_aspect_mode + 1) % 3;
            printf("[aspect] %s\n", g_aspect_names[g_aspect_mode]);
        }
        hk = &g_settings.hk_quit;
        if (me_platform_key_pressed(hk->vk, hk->ctrl, hk->alt, hk->shift)) {
            me_platform_request_quit();
        }
        hk = &g_settings.hk_reset;
        if (me_platform_key_pressed(hk->vk, hk->ctrl, hk->alt, hk->shift)) {
            if (core->retro_reset) {
                core->retro_reset();
                printf("[hotkey] reset\n");
            }
        }
        size_t fill_before = 0, fill_after = 0;
        int timed_out = 0;
        (void)frame_audio; (void)timed_out;
        if (audio_ok && pace_log) fill_before = ME_RING_TOTAL - me_audio_writable_frames();

        /* GL is per-thread; make sure our context is current on this thread
           before the core does any GL work. Cheap if already current. */
        if (g_hw_render_accepted) {
            me_gl_make_current();
            /* Interop: lock the shared texture so GL has exclusive access
               while the core renders. No-op if interop is inactive. */
            me_gl_interop_lock();
        }
        if (latency_log) g_poll_qpc.QuadPart = 0;
        LARGE_INTEGER ll_t_before_run; if (latency_log) QueryPerformanceCounter(&ll_t_before_run);
        if (ra_frames > 0) {
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
            if (!core->retro_serialize(ra_state_buf, ra_state_size)) {
                fprintf(stderr, "[runahead] serialize failed; disabling for rest of session\n");
                free(ra_state_buf); ra_state_buf = NULL;
                ra_frames = 0;
                core->retro_run();
            } else {
                g_av_mute = 1;
                for (int i = 0; i < ra_frames; i++) core->retro_run();
                g_av_mute = 0;
                core->retro_run();  /* this one is shown */
                if (!core->retro_unserialize(ra_state_buf, ra_state_size)) {
                    fprintf(stderr, "[runahead] unserialize failed; disabling for rest of session\n");
                    free(ra_state_buf); ra_state_buf = NULL;
                    ra_frames = 0;
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
        LARGE_INTEGER ll_t_after_run; if (latency_log) QueryPerformanceCounter(&ll_t_after_run);
        present(g_hwnd);
        LARGE_INTEGER ll_t_after_present; if (latency_log) QueryPerformanceCounter(&ll_t_after_present);

        /* SRAM dirty-poll: catches in-game saves so a force-quit shortly after
           the user hits "Save" still persists the write. 1s cadence + one-tick
           debounce → worst case ~2s to land on disk. */
        if (save_path[0] && core->retro_get_memory_data && core->retro_get_memory_size) {
            DWORD now_ms = GetTickCount();
            if (now_ms - sram_last_poll_ms >= 1000) {
                sram_last_poll_ms = now_ms;
                void *mem = core->retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
                size_t sz = core->retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
                if (mem && sz) {
                    uint64_t h = me_fnv1a64(mem, sz);
                    if (h != sram_disk_hash) {
                        if (sram_pending && h == sram_last_hash) {
                            /* Settled — write it out. */
                            me_sram_save(core, save_path);
                            sram_disk_hash = h;
                            sram_pending = 0;
                        } else {
                            /* Still changing (or first time we noticed) — wait one more tick. */
                            sram_pending = 1;
                        }
                    } else {
                        sram_pending = 0;
                    }
                    sram_last_hash = h;
                }
            }
        }

        /* Dynamic rate control: PI controller on ring-fill error. The integral
           term `g_resamp_ratio_bias` absorbs the long-term core-vs-device
           clock mismatch; the proportional term adds a tiny instantaneous
           response to keep the ring near target. Bias clamped to ±0.5% so
           pitch shift stays below audibility (~8 cents). */
        double drc_p_term = 0.0;
        if (audio_ok) {
            size_t fill = ME_RING_TOTAL - me_audio_writable_frames();
            if (pace_log) fill_after = fill;

            /* 60-frame moving average of ring fill. Reacting to instantaneous
               fill makes the rate bias chase per-frame noise and produces an
               audible pitch wobble. Averaging over ~1 second smooths that out
               while still tracking the true core-vs-device clock drift. */
            static size_t fill_hist[60];
            static int    fill_hist_idx = 0;
            static int    fill_hist_filled = 0;
            fill_hist[fill_hist_idx] = fill;
            fill_hist_idx = (fill_hist_idx + 1) % 60;
            if (fill_hist_idx == 0) fill_hist_filled = 1;

            if (fill_hist_filled) {
                uint64_t sum = 0;
                for (int k = 0; k < 60; k++) sum += fill_hist[k];
                double avg_fill = (double)sum / 60.0;
                /* Normalized error: -1 = ring empty, 0 = at target, +1 = double target. */
                double err = (avg_fill - (double)target_buffered) / (double)target_buffered;
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

        /* QPC absolute-deadline pace. Frame N must land at qstart + N*period.
           Sleep most of the wait at 1ms resolution, then spin the last bit. */
        frame_count++;
        {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            double elapsed_ms = (double)(now.QuadPart - qstart.QuadPart) * 1000.0 / (double)qpf.QuadPart;
            double deadline_ms = (double)frame_count * frame_period_ms;
            double wait_ms = deadline_ms - elapsed_ms;
            if (wait_ms > 1.5) Sleep((DWORD)(wait_ms - 1.0));
            while (1) {
                QueryPerformanceCounter(&now);
                elapsed_ms = (double)(now.QuadPart - qstart.QuadPart) * 1000.0 / (double)qpf.QuadPart;
                if (elapsed_ms >= deadline_ms) break;
            }
        }
        if (timing_log && (frame_count % 1000) == 0) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            double elapsed_ms = (double)(now.QuadPart - qstart.QuadPart) * 1000.0 / (double)qpf.QuadPart;
            double expected_ms = (double)frame_count * frame_period_ms;
            double drift_ms = elapsed_ms - expected_ms;
            me_log(ME_LOG_TIMING,
                   "[timing] frame %lu expected=%.3f ms actual=%.3f ms drift=%+.3f ms (%+.3f us/frame) bias=%+.4f%%\n",
                   frame_count, expected_ms, elapsed_ms, drift_ms,
                   (drift_ms * 1000.0) / (double)frame_count,
                   g_resamp_ratio_bias * 100.0);
        }
        if (latency_log) {
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
                ll_missed_polls++;
            }
            ll_back_sum    += d_back;
            ll_pre_sum     += d_pre;
            ll_post_sum    += d_post;
            ll_present_sum += d_present;
            ll_pace_sum    += d_pace;
            if (d_iter > ll_iter_max) ll_iter_max = d_iter;
            if (ll_iters == 0) ll_window_first_frame = frame_count;
            ll_iters++;
            double window_ms = (double)(ll_t_end.QuadPart - ll_window_start.QuadPart) * q;
            if (window_ms >= 1000.0 && ll_iters > 0) {
                double n = (double)ll_iters;
                me_log(ME_LOG_LATENCY,
                       "[latency] frame %lu..%lu (n=%u) avg ms: back=%.3f pre_poll=%.3f post_poll=%.3f present=%.3f pace=%.3f | iter_max=%.3f missed_polls=%u\n",
                       ll_window_first_frame, frame_count, ll_iters,
                       ll_back_sum / n, ll_pre_sum / n, ll_post_sum / n,
                       ll_present_sum / n, ll_pace_sum / n,
                       ll_iter_max, ll_missed_polls);
                ll_back_sum = ll_pre_sum = ll_post_sum = 0;
                ll_present_sum = ll_pace_sum = 0;
                ll_iter_max = 0;
                ll_iters = 0; ll_missed_polls = 0;
                ll_window_start = ll_t_end;
            }
        }
        if (pace_log) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            double gap_ms = (double)(now.QuadPart - pl_last_qpc.QuadPart) * 1000.0 / (double)qpf.QuadPart;
            pl_last_qpc = now;
            if (gap_ms < pl_gap_min) pl_gap_min = gap_ms;
            if (gap_ms > pl_gap_max) pl_gap_max = gap_ms;
            pl_gap_sum += gap_ms;
            if (fill_before < pl_fill_before_min) pl_fill_before_min = fill_before;
            if (fill_before > pl_fill_before_max) pl_fill_before_max = fill_before;
            pl_fill_before_sum += fill_before;
            if (fill_after  < pl_fill_after_min)  pl_fill_after_min  = fill_after;
            if (fill_after  > pl_fill_after_max)  pl_fill_after_max  = fill_after;
            pl_fill_after_sum  += fill_after;
            pl_iters++;
            if (timed_out) pl_timeouts++;
            double window_ms = (double)(now.QuadPart - pl_window_start.QuadPart) * 1000.0 / (double)qpf.QuadPart;
            if (window_ms >= 1000.0 && pl_iters > 0) {
                me_log(ME_LOG_PACE,
                       "[pace] %ufps gap min/avg/max=%.1f/%.1f/%.1f ms fill_before(min/avg/max)=%zu/%zu/%zu fill_after=%zu/%zu/%zu bias=%+.4f%%\n",
                       pl_iters,
                       pl_gap_min, pl_gap_sum / pl_iters, pl_gap_max,
                       pl_fill_before_min, pl_fill_before_sum / pl_iters, pl_fill_before_max,
                       pl_fill_after_min,  pl_fill_after_sum  / pl_iters, pl_fill_after_max,
                       g_resamp_ratio_bias * 100.0);
                pl_gap_min = 1e9; pl_gap_max = 0; pl_gap_sum = 0;
                pl_fill_before_min = (size_t)-1; pl_fill_before_max = 0; pl_fill_before_sum = 0;
                pl_fill_after_min  = (size_t)-1; pl_fill_after_max  = 0; pl_fill_after_sum  = 0;
                pl_iters = 0; pl_timeouts = 0;
                pl_window_start = now;
            }
        }
    }

    timeEndPeriod(1);
    if (audio_ok) me_audio_shutdown();
    if (g_use_d3d11) me_d3d11_shutdown();

    if (g_hw_render_accepted && g_hw_render.context_destroy) {
        g_hw_render.context_destroy();
    }
    free(ra_state_buf);
    if (save_path[0]) me_sram_save(core, save_path);
    core->retro_unload_game();
    core->retro_deinit();
    if (g_hw_render_accepted) me_gl_shutdown();
    free(rom_data);
    free(g_back);
    me_core_unload(core);
    me_settings_free(&g_settings);
    me_log_close_all();
    return 0;
}
