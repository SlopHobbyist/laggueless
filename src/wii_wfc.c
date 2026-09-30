/* Multiplayer > Fan Server for Wii games on Dolphin (wii_wfc.h). */

#include "wii_wfc.h"
#include <windows.h>
#include <wininet.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* The first line of the game inis we write; a file without it is the
   player's, and left alone. */
#define INI_MARK "# Written by laggueless for Multiplayer > Fan Server."

static int read_at(FILE *f, long off, void *buf, size_t n) {
    return fseek(f, off, SEEK_SET) == 0 && fread(buf, 1, n, f) == n;
}

/* The start of a Wii disc's header (game ID, disc number, revision, and at
   0x18 the Wii's magic word), from a plain image or one of the containers
   Dolphin reads that keep a copy uncompressed. GCZ keeps it deflated, so
   isn't read. 0 if it's not a Wii disc. */
static int read_wii_header(const char *path, unsigned char h[0x20]) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char head[16];
    long at = 0;
    int ok = read_at(f, 0, head, sizeof(head));
    if (ok && memcmp(head, "WBFS", 4) == 0) {
        /* Disc slot 0's info, one HD sector in, starts with its header. */
        ok = head[8] >= 9 && head[8] <= 16;
        at = 1L << head[8];
    } else if (ok && (memcmp(head, "WIA\x01", 4) == 0 || memcmp(head, "RVZ\x01", 4) == 0)) {
        at = 0x58;     /* header 2's copy */
    } else if (ok && memcmp(head, "CISO", 4) == 0) {
        at = 0x8000;   /* after the block map; block 0 is always stored */
    }
    ok = ok && read_at(f, at, h, 0x20);
    fclose(f);
    if (!ok || memcmp(h + 0x18, "\x5D\x1C\x9E\xA3", 4) != 0) return 0;
    for (int i = 0; i < 6; i++)   /* it names files */
        if (!isalnum(h[i])) return 0;
    return 1;
}

/* GET `url` into `buf` (NUL-terminated). The HTTP status, or 0 when the
   server couldn't be reached or the reply didn't fit. */
static DWORD http_get(const char *url, char *buf, size_t cap) {
    size_t len = 0;
    DWORD status = 0;
    HINTERNET net = InternetOpenA("laggueless", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (net) {
        DWORD timeout = 5000;
        InternetSetOptionA(net, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
        InternetSetOptionA(net, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
        HINTERNET u = InternetOpenUrlA(net, url, NULL, 0,
                                       INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
                                       INTERNET_FLAG_NO_UI | INTERNET_FLAG_NO_COOKIES, 0);
        if (u) {
            DWORD n = sizeof(status);
            if (!HttpQueryInfoA(u, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &n, NULL))
                status = 0;
            while (status == 200) {
                DWORD got = 0;
                if (len + 1 >= cap || !InternetReadFile(u, buf + len, (DWORD)(cap - 1 - len), &got)) {
                    status = 0;
                    break;
                }
                if (!got) break;
                len += got;
            }
            InternetCloseHandle(u);
        }
        InternetCloseHandle(net);
    }
    buf[len] = '\0';
    return status;
}

/* A patch's code lines ("XXXXXXXX XXXXXXXX"), one per line, into `out`.
   0 if it has any other line (an error page), or none. */
static int parse_gecko(const char *txt, char *out, size_t cap) {
    size_t n = 0;
    for (const char *p = txt; *p; ) {
        size_t len = strcspn(p, "\r\n");
        size_t end = len;
        while (end && (p[end - 1] == ' ' || p[end - 1] == '\t')) end--;
        if (end) {
            if (end != 17 || p[8] != ' ' || n + 19 > cap) return 0;
            for (int i = 0; i < 17; i++)
                if (i != 8 && !isxdigit((unsigned char)p[i])) return 0;
            memcpy(out + n, p, 17);
            out[n + 17] = '\n';
            n += 18;
        }
        p += len;
        p += strspn(p, "\r\n");
    }
    out[n] = '\0';
    return n > 0;
}

/* Up to cap-1 bytes of a file, NUL-terminated; 0 if it can't be read. */
static int read_small(const char *path, char *buf, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = '\0';
    return 1;
}

static int write_ini(const char *dir, const char *path, const char *server, const char *code) {
    char tmp[MAX_PATH];
    if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= sizeof(tmp)) return 0;
    CreateDirectoryA(dir, NULL);
    FILE *f = fopen(tmp, "wb");
    if (!f) return 0;
    /* Dolphin reads the other inis for the game too (<ID6>.ini...) and adds
       their [Gecko] codes and [Gecko_Enabled] lines to these. */
    fprintf(f, INI_MARK "\n"
               "# Replaced or removed each time the game loads.\n"
               "[Gecko]\n"
               "$Fan Server: %s\n"
               "%s"
               "[Gecko_Enabled]\n"
               "$Fan Server: %s\n", server, code, server);
    int ok = fclose(f) == 0 && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
    if (!ok) DeleteFileA(tmp);
    return ok;
}

void me_wii_wfc_prepare(const char *rom_path, const char *user_dir, me_fan_server server,
                        me_wii_wfc *out) {
    memset(out, 0, sizeof(*out));
    unsigned char h[0x20];
    if (!rom_path || !read_wii_header(rom_path, h)) return;
    out->wii = 1;

    /* The ini Dolphin reads for this revision alone, and who wrote it. */
    char dir[MAX_PATH], ini[MAX_PATH + 16];
    if ((size_t)snprintf(dir, sizeof(dir), "%s\\GameSettings", user_dir) >= sizeof(dir) ||
        (size_t)snprintf(ini, sizeof(ini), "%s\\%.6sr%u.ini", dir, (const char *)h, h[7]) >= MAX_PATH) {
        snprintf(out->note, sizeof(out->note), "Not patched: the save folder's path is too long");
        return;
    }
    static char body[1 << 16], code[1 << 16];   /* the emulation thread's */
    int exists = read_small(ini, body, sizeof(body));
    int ours = exists && strncmp(body, INI_MARK, strlen(INI_MARK)) == 0;

    const me_fan_server_info *fs = me_fan_server_info_of(server);
    if (!fs->wii_patches) {
        if (ours) DeleteFileA(ini);
        return;
    }
    if (exists && !ours) {
        snprintf(out->note, sizeof(out->note), "Not patched: GameSettings\\%.6sr%u.ini is yours",
                 (const char *)h, h[7]);
        printf("[wfc] %s is not ours; left alone\n", ini);
        return;
    }
    /* A saved patch for this server, should it be unreachable. */
    char line[64];
    snprintf(line, sizeof(line), "$Fan Server: %s\n", fs->name);
    int saved = ours && strstr(body, line) != NULL;

    /* Named for the disc (D) and its revision, in hex: RMCED00. */
    char patch[16], url[256];
    snprintf(patch, sizeof(patch), "%.4sD%02X", (const char *)h, h[7]);
    snprintf(url, sizeof(url), "%s%s.txt", fs->wii_patches, patch);
    DWORD status = http_get(url, body, sizeof(body));
    printf("[wfc] %s -> HTTP %lu\n", url, status);
    if (status == 200 && parse_gecko(body, code, sizeof(code))) {
        out->patched = write_ini(dir, ini, fs->name, code);
        if (out->patched) snprintf(out->note, sizeof(out->note), "Patched for %s (%s)", fs->name, patch);
        else              snprintf(out->note, sizeof(out->note), "Not patched: could not write its game ini");
    } else if (status == 404) {
        if (ours) DeleteFileA(ini);
        snprintf(out->note, sizeof(out->note), "%s has no patch for this game (%s)", fs->name, patch);
    } else if (saved) {
        out->patched = 1;
        snprintf(out->note, sizeof(out->note), "%s unreachable: using its saved patch", fs->name);
    } else {
        snprintf(out->note, sizeof(out->note), "Not patched: could not get %s's patch", fs->name);
    }
    printf("[wfc] %s\n", out->note);
}
