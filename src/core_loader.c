#include "core_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define ME_RESOLVE(c, name) do { \
    void *p = (void *)(uintptr_t)GetProcAddress((c)->dll, #name); \
    if (!p) { \
        fprintf(stderr, "[core_loader] missing symbol: %s\n", #name); \
        FreeLibrary((c)->dll); \
        free(c); \
        return NULL; \
    } \
    memcpy(&(c)->name, &p, sizeof(p)); \
} while (0)

me_core *me_core_load(const char *dll_path) {
    HMODULE dll = LoadLibraryA(dll_path);
    if (!dll) {
        fprintf(stderr, "[core_loader] LoadLibraryA failed for %s (err=%lu)\n",
                dll_path, GetLastError());
        return NULL;
    }

    me_core *c = (me_core *)calloc(1, sizeof(*c));
    if (!c) { FreeLibrary(dll); return NULL; }
    c->dll = dll;

    ME_RESOLVE(c, retro_api_version);
    ME_RESOLVE(c, retro_get_system_info);
    ME_RESOLVE(c, retro_get_system_av_info);
    ME_RESOLVE(c, retro_set_environment);
    ME_RESOLVE(c, retro_set_video_refresh);
    ME_RESOLVE(c, retro_set_audio_sample);
    ME_RESOLVE(c, retro_set_audio_sample_batch);
    ME_RESOLVE(c, retro_set_input_poll);
    ME_RESOLVE(c, retro_set_input_state);
    ME_RESOLVE(c, retro_init);
    ME_RESOLVE(c, retro_deinit);
    ME_RESOLVE(c, retro_load_game);
    ME_RESOLVE(c, retro_unload_game);
    ME_RESOLVE(c, retro_run);
    ME_RESOLVE(c, retro_reset);
    ME_RESOLVE(c, retro_serialize_size);
    ME_RESOLVE(c, retro_serialize);
    ME_RESOLVE(c, retro_unserialize);
    ME_RESOLVE(c, retro_get_memory_data);
    ME_RESOLVE(c, retro_get_memory_size);

    return c;
}

void me_core_unload(me_core *c) {
    if (!c) return;
    if (c->dll) FreeLibrary(c->dll);
    free(c);
}

void me_core_run_frame(me_core *c) {
    if (c && c->retro_run) c->retro_run();
}
