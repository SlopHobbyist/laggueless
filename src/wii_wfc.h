#pragma once
/* Multiplayer > Fan Server for Wii games on Dolphin.

   Dolphin resolves the games' host names itself and ignores the Wii's DNS,
   so a fan server is reached the way its own Dolphin guide says: its patch
   for the game (a Gecko code that points the game at the server) runs with
   the core's cheats on. As a game loads we fetch the patch for its disc and
   revision and write it, enabled, to the game ini Dolphin reads for that
   revision alone (<user>\GameSettings\<ID6>r<rev>.ini), which we own: the
   player's own <ID6>.ini is never touched. With the server off, or no patch
   for the game, that file is removed again. */

#include "settings.h"

typedef struct {
    int  wii;        /* the disc is a Wii game's (its ID was readable) */
    int  patched;    /* its patch is installed: load it with cheats on */
    char note[96];   /* what happened, for the menu */
} me_wii_wfc;

/* Before retro_load_game, for a game Dolphin runs. `user_dir` is Dolphin's
   User folder. Blocks while the patch downloads (a few seconds at most). */
void me_wii_wfc_prepare(const char *rom_path, const char *user_dir, me_fan_server server,
                        me_wii_wfc *out);
