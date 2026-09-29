#include "rom_cores.h"
#include <stdio.h>
#include <string.h>

/* Every extension belongs to exactly one console; ones several consoles
   share are given to the console most games in that format are for (.bin to
   the Mega Drive, disc images to the PlayStation, .iso to the PSP). Our pick
   of core comes first in each list: the accurate, speedrun-accepted choice
   where there is one. Candidates are the cores on the libretro buildbot that
   list the console's extensions. */
static const me_console k_consoles[] = {
    /* Nintendo */
    { "nes",       "NES / Famicom",            "nes|fds|unf|unif",
      "mesen|nestopia|fceumm|quicknes|mesen2" },
    { "snes",      "SNES / Super Famicom",     "sfc|smc|swc|fig|bs|st",
      "bsnes|mesen-s|mednafen_supafaust|bsnes_mercury_accuracy|bsnes_mercury_balanced|"
      "bsnes_mercury_performance|bsnes2014_accuracy|snes9x|bsnes_hd_beta|mesen2" },
    { "gb",        "Game Boy",                 "gb|dmg|sgb",
      "gambatte|sameboy|gearboy|mgba|tgbdual|vbam|mesen-s|mesen2" },
    { "gbc",       "Game Boy Color",           "gbc|cgb",
      "gambatte|sameboy|gearboy|mgba|tgbdual|vbam|mesen-s|mesen2" },
    { "gba",       "Game Boy Advance",         "gba|agb",
      "mgba|mednafen_gba|vbam|vba_next|gpsp|mesen2" },
    { "nds",       "Nintendo DS",              "nds|dsi|ids",
      "melondsds|melonds|desmume|noods" },
    { "3ds",       "Nintendo 3DS",             "3ds|3dsx|cci|cxi",
      "azahar|citra" },
    { "n64",       "Nintendo 64",              "n64|z64|v64|ndd",
      "parallel_n64|mupen64plus_next" },
    { "gamecube",  "GameCube / Wii",           "gcm|gcz|rvz|wbfs|ciso|wia",
      "dolphin" },
    { "vb",        "Virtual Boy",              "vb|vboy",
      "mednafen_vb" },
    { "pokemini",  "Pokemon Mini",             "min",
      "pokemini" },

    /* Sega */
    { "md",        "Mega Drive / Genesis",     "md|gen|smd|mdx|bin",
      "genesis_plus_gx|picodrive|blastem|clownmdemu|genesis_plus_gx_wide" },
    { "32x",       "Sega 32X",                 "32x",
      "picodrive" },
    { "sms",       "Master System",            "sms",
      "genesis_plus_gx|gearsystem|picodrive|smsplus|mesen2" },
    { "gg",        "Game Gear",                "gg",
      "genesis_plus_gx|gearsystem|picodrive|smsplus|mesen2" },
    { "sg1000",    "SG-1000",                  "sg|sc|mv",
      "genesis_plus_gx|gearsystem|picodrive|smsplus|bluemsx" },
    { "dreamcast", "Dreamcast",                "gdi|cdi",
      "flycast" },

    /* NEC */
    { "pce",       "PC Engine / TurboGrafx-16", "pce",
      "mednafen_pce|mednafen_pce_fast|geargrafx|mednafen_supergrafx|mesen2" },
    { "sgx",       "SuperGrafx",               "sgx",
      "mednafen_pce|mednafen_supergrafx|geargrafx" },

    /* Sony */
    { "psx",       "PlayStation",              "cue|chd|ccd|toc|m3u|pbp",
      "mednafen_psx|mednafen_psx_hw|swanstation|pcsx_rearmed" },
    { "psp",       "PlayStation Portable",     "iso|cso",
      "ppsspp" },

    /* Atari */
    { "a2600",     "Atari 2600",               "a26",
      "stella|stella2023|stella2014" },
    { "a5200",     "Atari 5200",               "a52",
      "a5200|atari800" },
    { "a7800",     "Atari 7800",               "a78",
      "prosystem" },
    { "lynx",      "Atari Lynx",               "lnx|lyx",
      "handy|mednafen_lynx|holani|gearlynx" },
    { "jaguar",    "Atari Jaguar",             "j64|jag",
      "virtualjaguar" },

    /* SNK, Bandai, others */
    { "ngp",       "Neo Geo Pocket",           "ngp",
      "mednafen_ngp|race" },
    { "ngpc",      "Neo Geo Pocket Color",     "ngc|ngpc|npc",
      "mednafen_ngp|race" },
    { "ws",        "WonderSwan",               "ws|pc2",
      "mednafen_wswan|mesen2" },
    { "wsc",       "WonderSwan Color",         "wsc|pcv2",
      "mednafen_wswan|mesen2" },
    { "coleco",    "ColecoVision",             "col|cv",
      "gearcoleco|bluemsx" },
    { "intv",      "Intellivision",            "int",
      "freeintv" },
    { "vectrex",   "Vectrex",                  "vec",
      "vecx" },
    { "msx",       "MSX",                      "mx1|mx2",
      "bluemsx|fmsx" },
};

/* Core name (DLL minus "_libretro.dll") -> what the dialog shows. */
static const struct { const char *core; const char *name; } k_core_names[] = {
    { "mesen", "Mesen" },               { "mesen2", "Mesen 2" },
    { "mesen-s", "Mesen-S" },           { "nestopia", "Nestopia UE" },
    { "fceumm", "FCEUmm" },             { "quicknes", "QuickNES" },
    { "bsnes", "bsnes" },               { "bsnes_hd_beta", "bsnes-hd beta" },
    { "bsnes_mercury_accuracy", "bsnes-mercury Accuracy" },
    { "bsnes_mercury_balanced", "bsnes-mercury Balanced" },
    { "bsnes_mercury_performance", "bsnes-mercury Performance" },
    { "bsnes2014_accuracy", "bsnes 2014 Accuracy" },
    { "mednafen_supafaust", "Beetle Supafaust" },
    { "snes9x", "Snes9x" },             { "gambatte", "Gambatte" },
    { "sameboy", "SameBoy" },           { "gearboy", "Gearboy" },
    { "mgba", "mGBA" },                 { "tgbdual", "TGB Dual" },
    { "vbam", "VBA-M" },                { "vba_next", "VBA Next" },
    { "gpsp", "gpSP" },                 { "mednafen_gba", "Beetle GBA" },
    { "melondsds", "melonDS DS" },      { "melonds", "melonDS" },
    { "desmume", "DeSmuME" },           { "noods", "NooDS" },
    { "azahar", "Azahar" },             { "citra", "Citra" },
    { "parallel_n64", "ParaLLEl N64" }, { "mupen64plus_next", "Mupen64Plus-Next" },
    { "dolphin", "Dolphin" },           { "mednafen_vb", "Beetle VB" },
    { "pokemini", "PokeMini" },         { "genesis_plus_gx", "Genesis Plus GX" },
    { "genesis_plus_gx_wide", "Genesis Plus GX Wide" },
    { "picodrive", "PicoDrive" },       { "blastem", "BlastEm" },
    { "clownmdemu", "ClownMDEmu" },     { "gearsystem", "Gearsystem" },
    { "smsplus", "SMS Plus GX" },       { "flycast", "Flycast" },
    { "mednafen_pce", "Beetle PCE" },   { "mednafen_pce_fast", "Beetle PCE FAST" },
    { "mednafen_supergrafx", "Beetle SuperGrafx" },
    { "geargrafx", "Geargrafx" },       { "mednafen_psx", "Beetle PSX" },
    { "mednafen_psx_hw", "Beetle PSX HW" },
    { "swanstation", "SwanStation" },   { "pcsx_rearmed", "PCSX ReARMed" },
    { "ppsspp", "PPSSPP" },             { "stella", "Stella" },
    { "stella2023", "Stella 2023" },    { "stella2014", "Stella 2014" },
    { "a5200", "a5200" },               { "atari800", "Atari800" },
    { "prosystem", "ProSystem" },       { "handy", "Handy" },
    { "mednafen_lynx", "Beetle Lynx" }, { "holani", "Holani" },
    { "gearlynx", "Gearlynx" },         { "virtualjaguar", "Virtual Jaguar" },
    { "mednafen_ngp", "Beetle NeoPop" }, { "race", "RACE" },
    { "mednafen_wswan", "Beetle WonderSwan" },
    { "gearcoleco", "Gearcoleco" },     { "bluemsx", "blueMSX" },
    { "fmsx", "fMSX" },                 { "freeintv", "FreeIntv" },
    { "vecx", "vecx" },
};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define DLL_SUFFIX "_libretro.dll"

int me_console_count(void) { return (int)COUNT(k_consoles); }

const me_console *me_console_at(int i) {
    return i >= 0 && i < (int)COUNT(k_consoles) ? &k_consoles[i] : NULL;
}

/* Is `s` one of the '|'-separated items in `list`? */
static int in_list(const char *list, const char *s) {
    size_t n = strlen(s);
    while (*list) {
        const char *end = strchr(list, '|');
        size_t len = end ? (size_t)(end - list) : strlen(list);
        if (len == n && _strnicmp(list, s, len) == 0) return 1;
        if (!end) break;
        list = end + 1;
    }
    return 0;
}

int me_console_for_rom(const char *rom_path) {
    if (!rom_path) return -1;
    const char *ext = NULL;
    for (const char *p = rom_path; *p; p++) {
        if (*p == '\\' || *p == '/') ext = NULL;
        else if (*p == '.')          ext = p + 1;
    }
    if (!ext || !*ext) return -1;
    for (size_t i = 0; i < COUNT(k_consoles); i++)
        if (in_list(k_consoles[i].exts, ext)) return (int)i;
    return -1;
}

int me_console_candidate(const me_console *c, int i, char *dll, size_t dll_sz) {
    const char *p = c->cores;
    for (; i > 0 && p; i--) {
        p = strchr(p, '|');
        if (p) p++;
    }
    if (!p || !*p) return 0;
    const char *end = strchr(p, '|');
    int len = (int)(end ? (size_t)(end - p) : strlen(p));
    snprintf(dll, dll_sz, "%.*s" DLL_SUFFIX, len, p);
    return 1;
}

void me_console_core_dll(const me_settings *s, const me_console *c, char *dll, size_t dll_sz) {
    const char *pick = me_settings_console_core(s, c->id);
    if (pick) snprintf(dll, dll_sz, "%s", pick);
    else      me_console_candidate(c, 0, dll, dll_sz);
}

void me_core_display_name(const char *dll, char *out, size_t out_sz) {
    size_t n = strlen(dll), sfx = strlen(DLL_SUFFIX);
    if (n > sfx && _stricmp(dll + n - sfx, DLL_SUFFIX) == 0) {
        for (size_t i = 0; i < COUNT(k_core_names); i++) {
            if (strlen(k_core_names[i].core) == n - sfx &&
                _strnicmp(dll, k_core_names[i].core, n - sfx) == 0) {
                snprintf(out, out_sz, "%s", k_core_names[i].name);
                return;
            }
        }
    }
    snprintf(out, out_sz, "%s", dll);
}

int me_rom_core_for(const me_settings *s, const char *rom_path, char *dll, size_t dll_sz) {
    int i = me_console_for_rom(rom_path);
    if (i < 0) return -1;
    me_console_core_dll(s, &k_consoles[i], dll, dll_sz);
    return 0;
}

void me_rom_patterns(char *out, size_t out_sz) {
    if (!out_sz) return;
    out[0] = '\0';
    size_t len = 0;
    for (size_t i = 0; i < COUNT(k_consoles); i++) {
        const char *p = k_consoles[i].exts;
        while (*p) {
            const char *end = strchr(p, '|');
            int n = (int)(end ? (size_t)(end - p) : strlen(p));
            int w = snprintf(out + len, out_sz - len, "%s*.%.*s", len ? ";" : "", n, p);
            if (w < 0 || (size_t)w >= out_sz - len) { out[len] = '\0'; return; }
            len += (size_t)w;
            if (!end) break;
            p = end + 1;
        }
    }
}
