#include "rom_cores.h"
#include <stdio.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* Multiplayer adapters. Each core wants its adapter plugged into a port:
   the SNES Multitap into port 2 (bsnes, Snes9x, Mesen-S), Genesis Plus GX
   its Team Player into port 1 and the EA 4 Way Play (which takes both
   ports) into either, so both are listed to reset both on unplugging.
   The Team Player and 4 Way Play are the 3-button versions: every game
   for them works with those. Mesen and FCEUmm turn the
   Four Score on when ports 3-4 have a gamepad; Nestopia connects the pads
   but its Four Score follows its game database. The PC Engine cores keep
   their TurboTap in core options (on by default in Beetle PCE), so there
   the checkbox only opens players 2-4. */
static const me_adapter k_nes_adapters[] = {
    { "four_score", "Four Score", 0xCu, "standard controller|gamepad" },
    { NULL }
};
static const me_adapter k_snes_adapters[] = {
    { "multitap", "Super Multitap", 0x2u, "multitap" },
    { NULL }
};
static const me_adapter k_md_adapters[] = {
    { "team_player",   "Team Player",    0x1u, "teamplayer|team player" },
    { "ea_4_way_play", "EA 4 Way Play",  0x3u, "4-wayplay|4 way play|4-way play" },
    { NULL }
};
static const me_adapter k_pce_adapters[] = {
    { "turbotap", "TurboTap", 0, NULL },
    { NULL }
};

/* Every extension is in exactly one console's `exts`; ones several consoles
   share are in the one most games in that format are for (.bin the Mega
   Drive, .cue the PlayStation, .iso the PSP, .rom the MSX) and in the others'
   `also_exts`, and the ROM's header picks between them (header_is). Our pick
   of core comes first in each list: the accurate, speedrun-accepted choice
   where there is one. Candidates are the cores on the libretro buildbot that
   list the console's extensions. */
static const me_console k_consoles[] = {
    /* Nintendo */
    { "nes",       "NES / Famicom",            "nes|fds|unf|unif", NULL,
      "mesen|nestopia|fceumm|quicknes|mesen2", 2, k_nes_adapters, 0 },
    { "snes",      "SNES / Super Famicom",     "sfc|smc|swc|fig|bs|st", NULL,
      "bsnes|mesen-s|mednafen_supafaust|bsnes_mercury_accuracy|bsnes_mercury_balanced|"
      "bsnes_mercury_performance|bsnes2014_accuracy|snes9x|bsnes_hd_beta|mesen2", 2, k_snes_adapters, 0 },
    { "gb",        "Game Boy",                 "gb|dmg|sgb", NULL,
      "gambatte|sameboy|gearboy|mgba|tgbdual|vbam|mesen-s|mesen2", 1, NULL, 0 },
    { "gbc",       "Game Boy Color",           "gbc|cgb", NULL,
      "gambatte|sameboy|gearboy|mgba|tgbdual|vbam|mesen-s|mesen2", 1, NULL, 0 },
    { "gba",       "Game Boy Advance",         "gba|agb", NULL,
      "mgba|mednafen_gba|vbam|vba_next|gpsp|mesen2", 1, NULL, 0 },
    { "nds",       "Nintendo DS",              "nds|dsi|ids", NULL,
      "melondsds|melonds|desmume|noods", 1, NULL, ME_CONSOLE_TWO_SCREENS | ME_CONSOLE_TOUCH },
    { "3ds",       "Nintendo 3DS",             "3ds|3dsx|cci|cxi", NULL,
      "azahar|citra", 1, NULL, ME_CONSOLE_TOUCH },
    { "n64",       "Nintendo 64",              "n64|z64|v64|ndd", NULL,
      "parallel_n64|mupen64plus_next", 4, NULL, 0 },
    { "gamecube",  "GameCube / Wii",           "gcm|gcz|rvz|wbfs|ciso|wia", "iso",
      "dolphin", 4, NULL, 0 },
    { "vb",        "Virtual Boy",              "vb|vboy", NULL,
      "mednafen_vb", 1, NULL, 0 },
    { "pokemini",  "Pokemon Mini",             "min", NULL,
      "pokemini", 1, NULL, 0 },

    /* Sega */
    { "md",        "Mega Drive / Genesis",     "md|gen|smd|mdx|bin", NULL,
      "genesis_plus_gx|picodrive|blastem|clownmdemu|genesis_plus_gx_wide", 2, k_md_adapters, 0 },
    { "32x",       "Sega 32X",                 "32x", NULL,
      "picodrive", 2, NULL, 0 },
    { "sms",       "Master System",            "sms", NULL,
      "genesis_plus_gx|gearsystem|picodrive|smsplus|mesen2", 2, NULL, 0 },
    { "gg",        "Game Gear",                "gg", NULL,
      "genesis_plus_gx|gearsystem|picodrive|smsplus|mesen2", 1, NULL, 0 },
    { "sg1000",    "SG-1000",                  "sg|sc|mv", NULL,
      "genesis_plus_gx|gearsystem|picodrive|smsplus|bluemsx", 2, NULL, 0 },
    { "dreamcast", "Dreamcast",                "gdi|cdi", "cue",
      "flycast", 4, NULL, 0 },

    /* NEC */
    { "pce",       "PC Engine / TurboGrafx-16", "pce", NULL,
      "mednafen_pce|mednafen_pce_fast|geargrafx|mednafen_supergrafx|mesen2", 1, k_pce_adapters, 0 },
    { "sgx",       "SuperGrafx",               "sgx", NULL,
      "mednafen_pce|mednafen_supergrafx|geargrafx", 1, k_pce_adapters, 0 },

    /* Sony */
    { "psx",       "PlayStation",              "cue|chd|ccd|toc|m3u|pbp", "iso",
      "mednafen_psx|mednafen_psx_hw|swanstation|pcsx_rearmed", 2, NULL, 0 },
    { "psp",       "PlayStation Portable",     "iso|cso", NULL,
      "ppsspp", 1, NULL, 0 },

    /* Atari */
    { "a2600",     "Atari 2600",               "a26", "bin",
      "stella|stella2023|stella2014", 2, NULL, 0 },
    { "a5200",     "Atari 5200",               "a52", "bin",
      "a5200|atari800", 4, NULL, 0 },
    { "a7800",     "Atari 7800",               "a78", "bin",
      "prosystem", 2, NULL, 0 },
    { "lynx",      "Atari Lynx",               "lnx|lyx", NULL,
      "handy|mednafen_lynx|holani|gearlynx", 1, NULL, 0 },
    { "jaguar",    "Atari Jaguar",             "j64|jag", NULL,
      "virtualjaguar", 2, NULL, 0 },

    /* SNK, Bandai, others */
    { "ngp",       "Neo Geo Pocket",           "ngp", NULL,
      "mednafen_ngp|race", 1, NULL, 0 },
    { "ngpc",      "Neo Geo Pocket Color",     "ngc|ngpc|npc", NULL,
      "mednafen_ngp|race", 1, NULL, 0 },
    { "ws",        "WonderSwan",               "ws|pc2", NULL,
      "mednafen_wswan|mesen2", 1, NULL, 0 },
    { "wsc",       "WonderSwan Color",         "wsc|pcv2", NULL,
      "mednafen_wswan|mesen2", 1, NULL, 0 },
    { "coleco",    "ColecoVision",             "col|cv", "bin|rom",
      "gearcoleco|bluemsx", 2, NULL, 0 },
    { "intv",      "Intellivision",            "int", "bin|rom",
      "freeintv", 2, NULL, 0 },
    { "vectrex",   "Vectrex",                  "vec", NULL,
      "vecx", 2, NULL, 0 },
    { "msx",       "MSX",                      "mx1|mx2|rom", NULL,
      "bluemsx|fmsx", 2, NULL, 0 },
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

_Static_assert(COUNT(k_consoles) <= 64, "me_console_set has a bit per console");

/* ---- telling consoles apart by header ------------------------------------ */
/* The start of a ROM file and, read as a disc image, its first sector and its
   ISO 9660 system identifier. A cartridge ROM just has neither. */
typedef struct {
    unsigned char head[0x200];
    size_t        head_n;
    unsigned char sector0[16];    /* start of sector 0's user data */
    char          system_id[33];  /* primary volume descriptor's; "" if none */
} rom_header;

/* The file a cue sheet's first data track is in, relative to the sheet
   unless absolute. Falls back to its first file. */
static int cue_data_file(const char *cue_path, char *out, size_t out_sz) {
    FILE *f = fopen(cue_path, "rb");
    if (!f) return 0;
    char text[16384];
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = '\0';

    char file[MAX_PATH] = "", first[MAX_PATH] = "";
    int data = 0;
    for (char *line = text; line && *line && !data; ) {
        char *next = strpbrk(line, "\r\n");
        if (next) *next++ = '\0';
        while (*line == ' ' || *line == '\t') line++;
        if (_strnicmp(line, "FILE", 4) == 0 && (line[4] == ' ' || line[4] == '\t')) {
            char *p = line + 5;
            while (*p == ' ' || *p == '\t') p++;
            size_t len = *p == '"' ? strcspn(++p, "\"") : strcspn(p, " \t");
            snprintf(file, sizeof(file), "%.*s", (int)len, p);
            if (!first[0]) memcpy(first, file, sizeof(first));
        } else if (_strnicmp(line, "TRACK", 5) == 0 && file[0]) {
            for (const char *p = line; *p && !data; p++) data = _strnicmp(p, "MODE", 4) == 0;
        }
        line = next;
    }
    const char *name = data ? file : first;
    if (!name[0]) return 0;
    if (name[0] == '\\' || name[0] == '/' || (name[0] && name[1] == ':')) {
        snprintf(out, out_sz, "%s", name);
    } else {
        size_t dir = 0;
        for (size_t i = 0; cue_path[i]; i++) if (cue_path[i] == '\\' || cue_path[i] == '/') dir = i + 1;
        snprintf(out, out_sz, "%.*s%s", (int)dir, cue_path, name);
    }
    return 1;
}

static int read_header(const char *rom_path, const char *ext, rom_header *h) {
    memset(h, 0, sizeof(*h));
    char track[MAX_PATH];
    if (_stricmp(ext, "cue") == 0) {
        if (!cue_data_file(rom_path, track, sizeof(track))) return 0;
        rom_path = track;
    }
    FILE *f = fopen(rom_path, "rb");
    if (!f) return 0;
    h->head_n = fread(h->head, 1, sizeof(h->head), f);

    /* Raw 2352-byte sectors start with a sync pattern, then a header whose
       last byte is the mode: user data follows it (mode 1), or an 8-byte
       subheader after it (mode 2, the PlayStation's). Else 2048-byte ones. */
    static const unsigned char sync[12] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    long sector = 2048, data = 0;
    if (h->head_n >= 16 && memcmp(h->head, sync, sizeof(sync)) == 0) {
        sector = 2352;
        data = h->head[15] == 2 ? 24 : 16;
    }
    if (h->head_n >= (size_t)data + sizeof(h->sector0))
        memcpy(h->sector0, h->head + data, sizeof(h->sector0));
    /* The primary volume descriptor is sector 16: type 1, "CD001", version,
       then the system identifier. */
    unsigned char pvd[40];
    if (fseek(f, 16 * sector + data, SEEK_SET) == 0 && fread(pvd, 1, sizeof(pvd), f) == sizeof(pvd) &&
        memcmp(pvd, "\1CD001", 6) == 0)
        memcpy(h->system_id, pvd + 8, 32);
    fclose(f);
    return 1;
}

static int bytes_at(const rom_header *h, size_t off, const void *sig, size_t n) {
    return h->head_n >= off + n && memcmp(h->head + off, sig, n) == 0;
}

/* Does the header say it's a game for this console? Only asked of the
   consoles an extension could be for, so each needs to tell its games apart
   from the others' only. Consoles whose ROMs have no header never say so. */
static int header_is(const me_console *c, const rom_header *h) {
    const char *id = c->id;
    if (strcmp(id, "md") == 0)         /* "SEGA GENESIS", " SEGA MEGA DRIVE"... */
        return bytes_at(h, 0x100, "SEGA", 4) || bytes_at(h, 0x101, "SEGA", 4);
    if (strcmp(id, "a7800") == 0)      /* the .a78 header, left on a .bin */
        return bytes_at(h, 1, "ATARI7800", 9);
    if (strcmp(id, "coleco") == 0)     /* cartridge present: game, or test mode */
        return bytes_at(h, 0, "\xAA\x55", 2) || bytes_at(h, 0, "\x55\xAA", 2);
    if (strcmp(id, "msx") == 0)        /* ROM cartridge header */
        return bytes_at(h, 0, "AB", 2);
    if (strcmp(id, "intv") == 0)       /* .rom format: 0xA8, segments, ~segments */
        return h->head_n >= 3 && h->head[0] == 0xA8 && (h->head[1] ^ h->head[2]) == 0xFF;
    if (strcmp(id, "gamecube") == 0)   /* Wii magic word, GameCube magic word */
        return bytes_at(h, 0x18, "\x5D\x1C\x9E\xA3", 4) || bytes_at(h, 0x1C, "\xC2\x33\x9F\x3D", 4);
    if (strcmp(id, "psp") == 0)
        return strncmp(h->system_id, "PSP GAME", 8) == 0;
    if (strcmp(id, "psx") == 0)
        return strncmp(h->system_id, "PLAYSTATION", 11) == 0;
    if (strcmp(id, "dreamcast") == 0)  /* IP.BIN, on both of a GD-ROM's areas */
        return memcmp(h->sector0, "SEGA SEGAKATANA", 15) == 0;
    return 0;
}

static int only_one(me_console_set s) { return s && !(s & (s - 1)); }

static int first_of(me_console_set s) {
    int i = 0;
    while (!(s & 1)) { s >>= 1; i++; }
    return i;
}

int me_console_for_rom(const char *rom_path, me_console_set *candidates) {
    if (candidates) *candidates = 0;
    if (!rom_path) return ME_ROM_UNKNOWN;
    const char *ext = NULL;
    for (const char *p = rom_path; *p; p++) {
        if (*p == '\\' || *p == '/') ext = NULL;
        else if (*p == '.')          ext = p + 1;
    }
    if (!ext || !*ext) return ME_ROM_UNKNOWN;

    me_console_set all = 0;
    for (size_t i = 0; i < COUNT(k_consoles); i++) {
        const me_console *c = &k_consoles[i];
        if (in_list(c->exts, ext) || (c->also_exts && in_list(c->also_exts, ext)))
            all |= 1ull << i;
    }
    if (candidates) *candidates = all;
    if (!all) return ME_ROM_UNKNOWN;
    if (only_one(all)) return first_of(all);

    me_console_set says = 0;
    rom_header h;
    if (read_header(rom_path, ext, &h)) {
        for (size_t i = 0; i < COUNT(k_consoles); i++)
            if ((all >> i & 1) && header_is(&k_consoles[i], &h)) says |= 1ull << i;
    }
    if (only_one(says)) return first_of(says);
    if (says && candidates) *candidates = says;   /* several claim it: pick among those */
    return ME_ROM_AMBIGUOUS;
}

const me_adapter *me_console_adapter(const me_console *c, const char *id) {
    if (!c || !c->adapters || !id) return NULL;
    for (const me_adapter *a = c->adapters; a->id; a++)
        if (_stricmp(a->id, id) == 0) return a;
    return NULL;
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
