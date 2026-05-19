#ifndef ME_CORE_LOADER_H
#define ME_CORE_LOADER_H

#include "types.h"

/* Libretro core dynamic loader — fleshed out later. Stub interface for the scaffold. */

typedef struct me_core me_core;

me_core *me_core_load(const char *dll_path);
void     me_core_unload(me_core *c);
void     me_core_run_frame(me_core *c);

#endif
