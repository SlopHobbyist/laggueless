#include <stdio.h>
#include "platform_win32.h"
#include "integer_scaling.h"
#include "core_loader.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <core.dll>\n", argv[0]);
        return 1;
    }

    me_core *core = me_core_load(argv[1]);
    if (!core) {
        fprintf(stderr, "failed to load core: %s\n", argv[1]);
        return 1;
    }

    unsigned api = core->retro_api_version();
    printf("[core] retro_api_version = %u\n", api);

    struct retro_system_info info = {0};
    core->retro_get_system_info(&info);
    printf("[core] library_name      = %s\n", info.library_name      ? info.library_name      : "(null)");
    printf("[core] library_version   = %s\n", info.library_version   ? info.library_version   : "(null)");
    printf("[core] valid_extensions  = %s\n", info.valid_extensions  ? info.valid_extensions  : "(null)");
    printf("[core] need_fullpath     = %s\n", info.need_fullpath ? "true" : "false");
    printf("[core] block_extract     = %s\n", info.block_extract ? "true" : "false");

    me_core_unload(core);
    return 0;
}
