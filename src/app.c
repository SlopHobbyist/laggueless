#include "app.h"
#include <stdio.h>
#include <string.h>

static me_settings     *g_live;
static char             g_settings_path[MAX_PATH];
static CRITICAL_SECTION g_settings_cs;

/* Small ring of pending commands. Menu clicks arrive at human speed, so a
   full ring just drops the newest one. */
#define ME_CMD_RING 16
static CRITICAL_SECTION g_cmd_cs;
static me_cmd           g_cmds[ME_CMD_RING];
static unsigned         g_cmd_head, g_cmd_tail;
static HANDLE           g_wake;
static volatile LONG    g_quit;

static CRITICAL_SECTION g_status_cs;
static me_app_status    g_status;
static me_input_layout  g_layout;

void me_app_init(me_settings *live, const char *settings_path) {
    g_live = live;
    snprintf(g_settings_path, sizeof(g_settings_path), "%s", settings_path);
    InitializeCriticalSection(&g_settings_cs);
    InitializeCriticalSection(&g_cmd_cs);
    InitializeCriticalSection(&g_status_cs);
    me_layout_unknown(&g_layout);
    g_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
}

me_settings *me_app_settings(void)      { return g_live; }
const char  *me_app_settings_path(void) { return g_settings_path; }

void me_settings_lock(void)   { EnterCriticalSection(&g_settings_cs); }
void me_settings_unlock(void) { LeaveCriticalSection(&g_settings_cs); }

void me_cmd_post(me_cmd_type type, int arg, const char *path) {
    EnterCriticalSection(&g_cmd_cs);
    if (g_cmd_head - g_cmd_tail < ME_CMD_RING) {
        me_cmd *c = &g_cmds[g_cmd_head % ME_CMD_RING];
        c->type = type;
        c->arg  = arg;
        snprintf(c->path, sizeof(c->path), "%s", path ? path : "");
        g_cmd_head++;
    }
    LeaveCriticalSection(&g_cmd_cs);
    SetEvent(g_wake);
}

int me_cmd_take(me_cmd *out) {
    int got = 0;
    EnterCriticalSection(&g_cmd_cs);
    if (g_cmd_tail != g_cmd_head) {
        *out = g_cmds[g_cmd_tail % ME_CMD_RING];
        g_cmd_tail++;
        got = 1;
    }
    LeaveCriticalSection(&g_cmd_cs);
    return got;
}

HANDLE me_app_wake_event(void) { return g_wake; }

void me_app_request_quit(void) {
    InterlockedExchange(&g_quit, 1);
    SetEvent(g_wake);
}

int me_app_quit_requested(void) { return g_quit != 0; }

void me_status_set(const me_app_status *s) {
    EnterCriticalSection(&g_status_cs);
    g_status = *s;
    LeaveCriticalSection(&g_status_cs);
}

void me_status_get(me_app_status *out) {
    EnterCriticalSection(&g_status_cs);
    *out = g_status;
    LeaveCriticalSection(&g_status_cs);
}

void me_layout_publish(const me_input_layout *l) {
    EnterCriticalSection(&g_status_cs);
    g_layout = *l;
    LeaveCriticalSection(&g_status_cs);
}

void me_layout_get(me_input_layout *out) {
    EnterCriticalSection(&g_status_cs);
    *out = g_layout;
    LeaveCriticalSection(&g_status_cs);
}
