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
#include "libretro.h"

/* ---- global state for callbacks (single core, single ROM) ----------------- */
static enum retro_pixel_format g_pixel_format = RETRO_PIXEL_FORMAT_0RGB1555;

/* BGRX backbuffer (32 bpp, top-down via negative bmi height). */
static u32 *g_back = NULL;
static unsigned g_back_max_w = 0, g_back_max_h = 0;
static unsigned g_frame_w = 0, g_frame_h = 0;
static unsigned long g_video_calls = 0;

static HWND g_hwnd = NULL;
static int  g_use_d3d11 = 0;

/* Aspect mode: 0 = 1:1 (square pixels), 1 = 4:3, 2 = 16:9. F1 cycles. */
static int g_aspect_mode = 0;
static const char *g_aspect_names[3] = { "1:1", "4:3", "16:9" };
static const int g_aspect_x[3] = { 1, 4, 16 };
static const int g_aspect_y[3] = { 1, 3,  9 };

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
    if (w > g_back_max_w || h > g_back_max_h) return;
    g_frame_w = w; g_frame_h = h;
    if (g_pixel_format == RETRO_PIXEL_FORMAT_XRGB8888) {
        convert_xrgb8888((const u32 *)data, pitch, w, h);
    } else if (g_pixel_format == RETRO_PIXEL_FORMAT_RGB565) {
        convert_rgb565((const u16 *)data, pitch, w, h);
    }
}
/* Linear resampler from core rate -> device rate. State carries the last input
   frame across calls, plus a fractional phase. */
static unsigned g_core_rate = 0;
static unsigned g_dev_rate  = 0;
static int      g_audio_active = 0; /* 1 once me_audio_init has succeeded */
static double   g_resamp_phase = 0.0; /* 0..1 position between prev and next input frame */
static int16_t  g_resamp_prev_l = 0, g_resamp_prev_r = 0;
/* Dynamic rate control: small bias on the resampler ratio so the ring
   converges to target depth without skipping/duplicating frames. Bounded to
   ±0.5% = ~8 cents of pitch shift, well below audibility. */
static double   g_resamp_ratio_bias = 0.0;

static void resample_and_push(const int16_t *in, size_t in_frames) {
    if (!g_audio_active || !g_dev_rate || !g_core_rate || in_frames == 0) return;
    double step = ((double)g_core_rate / (double)g_dev_rate) * (1.0 + g_resamp_ratio_bias);
    enum { CHUNK = 512 };
    int16_t out[CHUNK * 2];
    size_t out_n = 0;
    double phase = g_resamp_phase;
    int16_t pl = g_resamp_prev_l, pr = g_resamp_prev_r;
    for (size_t i = 0; i < in_frames; i++) {
        int16_t nl = in[i*2 + 0];
        int16_t nr = in[i*2 + 1];
        while (phase < 1.0) {
            float fl = pl + (nl - pl) * (float)phase;
            float fr = pr + (nr - pr) * (float)phase;
            int il = (int)fl, ir = (int)fr;
            if (il >  32767) il =  32767; else if (il < -32768) il = -32768;
            if (ir >  32767) ir =  32767; else if (ir < -32768) ir = -32768;
            out[out_n*2 + 0] = (int16_t)il;
            out[out_n*2 + 1] = (int16_t)ir;
            out_n++;
            if (out_n == CHUNK) { me_audio_push(out, out_n); out_n = 0; }
            phase += step;
        }
        phase -= 1.0;
        pl = nl; pr = nr;
    }
    if (out_n) me_audio_push(out, out_n);
    g_resamp_phase  = phase;
    g_resamp_prev_l = pl;
    g_resamp_prev_r = pr;
}

static void me_audio_sample_cb(int16_t l, int16_t r) {
    int16_t pair[2] = { l, r };
    resample_and_push(pair, 1);
}
static size_t me_audio_sample_batch_cb(const int16_t *data, size_t frames) {
    resample_and_push(data, frames);
    return frames;
}
/* Player 1 RetroPad state, refreshed in input_poll. */
static int16_t g_pad1[16];

static int key_down(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) ? 1 : 0;
}

static void me_input_poll_cb(void) {
    /* Don't read keys when our window isn't foreground. */
    if (GetForegroundWindow() != g_hwnd) {
        memset(g_pad1, 0, sizeof(g_pad1));
        return;
    }
    int up = key_down(VK_UP),   down  = key_down(VK_DOWN);
    int lf = key_down(VK_LEFT), right = key_down(VK_RIGHT);
    /* SOCD: opposing directions cancel to neutral. */
    if (up && down)  { up = down = 0; }
    if (lf && right) { lf = right = 0; }
    g_pad1[RETRO_DEVICE_ID_JOYPAD_UP]     = up;
    g_pad1[RETRO_DEVICE_ID_JOYPAD_DOWN]   = down;
    g_pad1[RETRO_DEVICE_ID_JOYPAD_LEFT]   = lf;
    g_pad1[RETRO_DEVICE_ID_JOYPAD_RIGHT]  = right;
    g_pad1[RETRO_DEVICE_ID_JOYPAD_B]      = key_down('Z');
    g_pad1[RETRO_DEVICE_ID_JOYPAD_A]      = key_down('X');
    g_pad1[RETRO_DEVICE_ID_JOYPAD_Y]      = key_down('A');
    g_pad1[RETRO_DEVICE_ID_JOYPAD_X]      = key_down('S');
    g_pad1[RETRO_DEVICE_ID_JOYPAD_START]  = key_down(VK_RETURN);
    g_pad1[RETRO_DEVICE_ID_JOYPAD_SELECT] = key_down(VK_RSHIFT);
    g_pad1[RETRO_DEVICE_ID_JOYPAD_L]      = key_down('Q');
    g_pad1[RETRO_DEVICE_ID_JOYPAD_R]      = key_down('W');
}

static int16_t me_input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id) {
    (void)index;
    if (port != 0 || device != RETRO_DEVICE_JOYPAD) return 0;
    if (id >= sizeof(g_pad1) / sizeof(g_pad1[0])) return 0;
    return g_pad1[id];
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
    int no_audio = 0;
    int force_gdi = 0;
    int pace_log = 0;
    int timing_log = 0;
    const char *positional[2] = { NULL, NULL };
    int npos = 0;
    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "--no-audio") == 0) no_audio  = 1;
        else if (strcmp(argv[i], "--gdi")      == 0) force_gdi = 1;
        else if (strcmp(argv[i], "--pace-log") == 0) pace_log  = 1;
        else if (strcmp(argv[i], "--timing-log") == 0) timing_log = 1;
        else if (argv[i][0] == '-') {
            fprintf(stderr, "unknown flag: %s\n", argv[i]); return 1;
        } else if (npos < 2) {
            positional[npos++] = argv[i];
        } else {
            fprintf(stderr, "extra argument: %s\n", argv[i]); return 1;
        }
    }
    if (npos < 2) {
        fprintf(stderr, "usage: %s [--no-audio] [--gdi] [--pace-log] [--timing-log] <core.dll> <rom>\n", argv[0]);
        return 1;
    }
    const char *core_path = positional[0];
    const char *rom_path  = positional[1];

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

    core->retro_set_environment(me_environment_cb);
    core->retro_set_video_refresh(me_video_refresh_cb);
    core->retro_set_audio_sample(me_audio_sample_cb);
    core->retro_set_audio_sample_batch(me_audio_sample_batch_cb);
    core->retro_set_input_poll(me_input_poll_cb);
    core->retro_set_input_state(me_input_state_cb);

    core->retro_init();

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

    /* Try D3D11 flip-model unless the user forced GDI. */
    if (!force_gdi) {
        if (me_d3d11_init(g_hwnd, g_back_max_w, g_back_max_h) == 0) {
            g_use_d3d11 = 1;
        } else {
            fprintf(stderr, "[render] D3D11 init failed, falling back to GDI\n");
        }
    } else {
        printf("[render] --gdi forced\n");
    }

    /* Audio drives pacing. WASAPI runs at the device's mix rate; we resample
       core output up to that rate. */
    g_core_rate = (unsigned)(av.timing.sample_rate > 0 ? av.timing.sample_rate : 48000);
    double fps = av.timing.fps > 1.0 ? av.timing.fps : 60.0;
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
    /* Pacing target: keep about 50 ms buffered in the ring. The WASAPI buffer
       drains in ~50 ms bursts, so a 50 ms pre-buffer keeps the ring above
       empty across drain events and reduces per-frame wait-loop jitter. */
    size_t target_buffered = (size_t)(g_dev_rate * 0.050);

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

    while (me_platform_pump()) {
        if (me_platform_f11_pressed) {
            me_platform_f11_pressed = 0;
            me_platform_toggle_fullscreen(g_hwnd);
        }
        if (me_platform_f1_pressed) {
            me_platform_f1_pressed = 0;
            g_aspect_mode = (g_aspect_mode + 1) % 3;
            printf("[aspect] %s\n", g_aspect_names[g_aspect_mode]);
        }
        size_t fill_before = 0, fill_after = 0;
        int timed_out = 0;
        (void)frame_audio; (void)timed_out;
        if (audio_ok && pace_log) fill_before = ME_RING_TOTAL - me_audio_writable_frames();

        core->retro_run();
        present(g_hwnd);

        /* Dynamic rate control: nudge resampler ratio toward keeping the ring
           at target_buffered. Bias ∈ [-0.005, +0.005] = ±0.5% pitch. We update
           once per video frame; the correction is gentle so audio doesn't
           audibly wobble. */
        if (audio_ok) {
            size_t fill = ME_RING_TOTAL - me_audio_writable_frames();
            if (pace_log) fill_after = fill;
            /* Error in frames, positive = too much buffered. Normalize by
               target so the gain is rate-independent. */
            double err = ((double)fill - (double)target_buffered) / (double)target_buffered;
            /* Proportional control with tight bounds. Gain 0.0005 means a 100%
               fill error pulls bias by 0.0005/frame; converges over ~1 second. */
            g_resamp_ratio_bias += 0.0005 * err;
            if (g_resamp_ratio_bias >  0.005) g_resamp_ratio_bias =  0.005;
            if (g_resamp_ratio_bias < -0.005) g_resamp_ratio_bias = -0.005;
        }

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
            printf("[timing] frame %lu expected=%.3f ms actual=%.3f ms drift=%+.3f ms (%+.3f us/frame) bias=%+.4f%%\n",
                   frame_count, expected_ms, elapsed_ms, drift_ms,
                   (drift_ms * 1000.0) / (double)frame_count,
                   g_resamp_ratio_bias * 100.0);
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
                printf("[pace] %ufps gap min/avg/max=%.1f/%.1f/%.1f ms fill_before(min/avg/max)=%zu/%zu/%zu fill_after=%zu/%zu/%zu bias=%+.4f%%\n",
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

    core->retro_unload_game();
    core->retro_deinit();
    free(rom_data);
    free(g_back);
    me_core_unload(core);
    return 0;
}
