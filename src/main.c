#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include "platform_win32.h"
#include "integer_scaling.h"
#include "core_loader.h"
#include "libretro.h"

/* ---- global state for callbacks (single core, single ROM) ----------------- */
static enum retro_pixel_format g_pixel_format = RETRO_PIXEL_FORMAT_0RGB1555;

/* BGRX backbuffer (32 bpp, top-down via negative bmi height). */
static u32 *g_back = NULL;
static unsigned g_back_max_w = 0, g_back_max_h = 0;
static unsigned g_frame_w = 0, g_frame_h = 0;
static unsigned long g_video_calls = 0;

static HWND g_hwnd = NULL;

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
    fprintf(stderr, "[core:%s] ", tag);
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
}

/* ---- environment callback ------------------------------------------------- */
static bool me_environment_cb(unsigned cmd, void *data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: {
            static const char *sysdir = ".";
            if (data) *(const char **)data = sysdir;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: {
            static const char *savedir = ".";
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
            if (data) ((struct retro_variable *)data)->value = NULL;
            return false;
        }
        default:
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

/* ---- video / audio / input callbacks -------------------------------------- */
static void me_video_refresh_cb(const void *data, unsigned w, unsigned h, size_t pitch) {
    g_video_calls++;
    if (!data || !g_back) return;
    if (w > g_back_max_w || h > g_back_max_h) return; /* skip frames bigger than allocation */
    g_frame_w = w; g_frame_h = h;
    if (g_pixel_format == RETRO_PIXEL_FORMAT_XRGB8888) {
        convert_xrgb8888((const u32 *)data, pitch, w, h);
    } else if (g_pixel_format == RETRO_PIXEL_FORMAT_RGB565) {
        convert_rgb565((const u16 *)data, pitch, w, h);
    }
}
static void me_audio_sample_cb(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t me_audio_sample_batch_cb(const int16_t *data, size_t frames) {
    (void)data; return frames;
}
static void me_input_poll_cb(void) {}
static int16_t me_input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id) {
    (void)port; (void)device; (void)index; (void)id;
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

    HDC hdc = GetDC(hwnd);

    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = (LONG)g_back_max_w;
    bmi.bmiHeader.biHeight      = -(LONG)g_back_max_h; /* top-down */
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    SetStretchBltMode(hdc, COLORONCOLOR);
    StretchDIBits(hdc,
                  0, 0, cw, ch,
                  0, 0, (int)g_frame_w, (int)g_frame_h,
                  g_back, &bmi, DIB_RGB_COLORS, SRCCOPY);

    ReleaseDC(hwnd, hdc);
}

/* ---- file slurp ----------------------------------------------------------- */
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

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <core.dll> <rom>\n", argv[0]);
        return 1;
    }

    me_core *core = me_core_load(argv[1]);
    if (!core) { fprintf(stderr, "failed to load core: %s\n", argv[1]); return 1; }

    unsigned api = core->retro_api_version();
    printf("[core] retro_api_version = %u\n", api);

    struct retro_system_info info = {0};
    core->retro_get_system_info(&info);
    printf("[core] %s %s (ext=%s, need_fullpath=%d)\n",
           info.library_name ? info.library_name : "?",
           info.library_version ? info.library_version : "?",
           info.valid_extensions ? info.valid_extensions : "?",
           (int)info.need_fullpath);

    core->retro_set_environment(me_environment_cb);
    core->retro_set_video_refresh(me_video_refresh_cb);
    core->retro_set_audio_sample(me_audio_sample_cb);
    core->retro_set_audio_sample_batch(me_audio_sample_batch_cb);
    core->retro_set_input_poll(me_input_poll_cb);
    core->retro_set_input_state(me_input_state_cb);

    core->retro_init();

    struct retro_game_info game = {0};
    game.path = argv[2];
    unsigned char *rom_data = NULL;
    size_t rom_size = 0;
    if (!info.need_fullpath) {
        rom_data = slurp(argv[2], &rom_size);
        if (!rom_data) { fprintf(stderr, "failed to read ROM: %s\n", argv[2]); return 1; }
        game.data = rom_data;
        game.size = rom_size;
    }

    if (!core->retro_load_game(&game)) {
        fprintf(stderr, "retro_load_game failed\n");
        return 1;
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
    g_frame_w = av.geometry.base_width;
    g_frame_h = av.geometry.base_height;

    /* Create window sized to a reasonable 2× of base geometry. */
    int win_w = (int)(av.geometry.base_width  * 2);
    int win_h = (int)(av.geometry.base_height * 2);
    if (win_w < 320) win_w = 640;
    if (win_h < 240) win_h = 480;
    g_hwnd = me_platform_create_window("multi-emulator", win_w, win_h);
    if (!g_hwnd) { fprintf(stderr, "window create failed\n"); return 1; }

    /* Frame pacing via Sleep. Real pacing in step 6. */
    double fps = av.timing.fps > 1.0 ? av.timing.fps : 60.0;
    DWORD frame_ms = (DWORD)(1000.0 / fps + 0.5);

    unsigned long frames = 0;
    DWORD last_report = GetTickCount();
    while (me_platform_pump()) {
        core->retro_run();
        present(g_hwnd);
        frames++;
        DWORD now = GetTickCount();
        if (now - last_report >= 1000) {
            printf("[run] frames/s=%lu video_refresh=%lu frame_dims=%ux%u back=%p\n",
                   frames, g_video_calls, g_frame_w, g_frame_h, (void *)g_back);
            frames = 0;
            last_report = now;
        }
        Sleep(frame_ms);
    }

    core->retro_unload_game();
    core->retro_deinit();
    free(rom_data);
    free(g_back);
    me_core_unload(core);
    return 0;
}
