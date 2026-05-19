#include "core_loader.h"
#include <stdlib.h>

/* Scaffold stub. Real implementation will: LoadLibraryA, GetProcAddress for the
   retro_* symbols, install env/video/audio/input callbacks, then call
   retro_init / retro_load_game / retro_run. */

struct me_core { int placeholder; };

me_core *me_core_load(const char *dll_path) {
    (void)dll_path;
    return NULL; /* not yet implemented */
}

void me_core_unload(me_core *c) { (void)c; }
void me_core_run_frame(me_core *c) { (void)c; }
