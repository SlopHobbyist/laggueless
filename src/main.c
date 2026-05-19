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
static unsigned long g_video_calls = 0;
static unsigned g_last_w = 0, g_last_h = 0;
static size_t g_last_pitch = 0;
static unsigned long g_audio_sample_calls = 0;
static unsigned long g_audio_batch_calls = 0;
static unsigned long g_audio_frames_total = 0;

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
            if (data) {
                struct retro_log_callback *cb = (struct retro_log_callback *)data;
                cb->log = me_log_cb;
            }
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

/* ---- video / audio / input callbacks (stubs) ------------------------------ */
static void me_video_refresh_cb(const void *data, unsigned w, unsigned h, size_t pitch) {
    (void)data;
    g_video_calls++;
    g_last_w = w; g_last_h = h; g_last_pitch = pitch;
}
static void me_audio_sample_cb(int16_t l, int16_t r) {
    (void)l; (void)r;
    g_audio_sample_calls++;
}
static size_t me_audio_sample_batch_cb(const int16_t *data, size_t frames) {
    (void)data;
    g_audio_batch_calls++;
    g_audio_frames_total += (unsigned long)frames;
    return frames;
}
static void me_input_poll_cb(void) {}
static int16_t me_input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id) {
    (void)port; (void)device; (void)index; (void)id;
    return 0;
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
    printf("[core] library_name      = %s\n", info.library_name      ? info.library_name      : "(null)");
    printf("[core] library_version   = %s\n", info.library_version   ? info.library_version   : "(null)");
    printf("[core] valid_extensions  = %s\n", info.valid_extensions  ? info.valid_extensions  : "(null)");
    printf("[core] need_fullpath     = %s\n", info.need_fullpath ? "true" : "false");

    /* Install callbacks before retro_init (env), then the rest before load_game. */
    core->retro_set_environment(me_environment_cb);
    core->retro_set_video_refresh(me_video_refresh_cb);
    core->retro_set_audio_sample(me_audio_sample_cb);
    core->retro_set_audio_sample_batch(me_audio_sample_batch_cb);
    core->retro_set_input_poll(me_input_poll_cb);
    core->retro_set_input_state(me_input_state_cb);

    core->retro_init();

    /* Load ROM. need_fullpath=true cores read from disk themselves. */
    struct retro_game_info game = {0};
    game.path = argv[2];
    unsigned char *rom_data = NULL;
    size_t rom_size = 0;
    if (!info.need_fullpath) {
        rom_data = slurp(argv[2], &rom_size);
        if (!rom_data) {
            fprintf(stderr, "failed to read ROM: %s\n", argv[2]);
            core->retro_deinit();
            me_core_unload(core);
            return 1;
        }
        game.data = rom_data;
        game.size = rom_size;
    }

    if (!core->retro_load_game(&game)) {
        fprintf(stderr, "retro_load_game failed\n");
        free(rom_data);
        core->retro_deinit();
        me_core_unload(core);
        return 1;
    }
    printf("[core] retro_load_game OK (rom_size=%zu, need_fullpath=%d)\n",
           rom_size, (int)info.need_fullpath);

    struct retro_system_av_info av = {0};
    core->retro_get_system_av_info(&av);
    printf("[av] base   = %ux%u\n", av.geometry.base_width, av.geometry.base_height);
    printf("[av] max    = %ux%u\n", av.geometry.max_width, av.geometry.max_height);
    printf("[av] aspect = %.4f\n", av.geometry.aspect_ratio);
    printf("[av] fps    = %.4f\n", av.timing.fps);
    printf("[av] rate   = %.1f Hz\n", av.timing.sample_rate);

    for (int i = 0; i < 60; i++) core->retro_run();

    printf("[run] video_refresh calls = %lu (last %ux%u, pitch=%zu)\n",
           g_video_calls, g_last_w, g_last_h, g_last_pitch);
    printf("[run] audio_sample calls  = %lu\n", g_audio_sample_calls);
    printf("[run] audio_batch calls   = %lu (total frames = %lu)\n",
           g_audio_batch_calls, g_audio_frames_total);

    core->retro_unload_game();
    core->retro_deinit();
    free(rom_data);
    me_core_unload(core);
    return 0;
}
