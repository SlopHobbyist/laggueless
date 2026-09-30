#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <yaml.h>
#include "settings.h"
#include "xinput_pad.h"
#include "settings_yaml_default.h"

/* ---- key-name → VK table -------------------------------------------------- */
typedef struct { const char *name; unsigned vk; } me_keymap;

/* Names are matched case-insensitively. Order doesn't matter for parsing;
   for writing, the first name listed for a VK is the one used. Letters and
   digits are handled separately; anything not listed round-trips as VK_xx. */
static const me_keymap g_keymap[] = {
    { "Up",        VK_UP        },
    { "Down",      VK_DOWN      },
    { "Left",      VK_LEFT      },
    { "Right",     VK_RIGHT     },
    { "Return",    VK_RETURN    },
    { "Enter",     VK_RETURN    },
    { "Escape",    VK_ESCAPE    },
    { "Esc",       VK_ESCAPE    },
    { "Space",     VK_SPACE     },
    { "Tab",       VK_TAB       },
    { "Backspace", VK_BACK      },
    { "Insert",    VK_INSERT    },
    { "Delete",    VK_DELETE    },
    { "Home",      VK_HOME      },
    { "End",       VK_END       },
    { "PageUp",    VK_PRIOR     },
    { "PageDown",  VK_NEXT      },
    { "LShift",    VK_LSHIFT    },
    { "RShift",    VK_RSHIFT    },
    { "Shift",     VK_SHIFT     },
    { "LCtrl",     VK_LCONTROL  },
    { "RCtrl",     VK_RCONTROL  },
    { "Ctrl",      VK_CONTROL   },
    { "LAlt",      VK_LMENU     },
    { "RAlt",      VK_RMENU     },
    { "Alt",       VK_MENU      },
    { "CapsLock",  VK_CAPITAL   },
    { "Semicolon", VK_OEM_1     },
    { "Equals",    VK_OEM_PLUS  },
    { "Comma",     VK_OEM_COMMA },
    { "Minus",     VK_OEM_MINUS },
    { "Period",    VK_OEM_PERIOD },
    { "Slash",     VK_OEM_2     },
    { "Backquote", VK_OEM_3     },
    { "LBracket",  VK_OEM_4     },
    { "Backslash", VK_OEM_5     },
    { "RBracket",  VK_OEM_6     },
    { "Quote",     VK_OEM_7     },
    { "Num0", VK_NUMPAD0 }, { "Num1", VK_NUMPAD1 }, { "Num2", VK_NUMPAD2 },
    { "Num3", VK_NUMPAD3 }, { "Num4", VK_NUMPAD4 }, { "Num5", VK_NUMPAD5 },
    { "Num6", VK_NUMPAD6 }, { "Num7", VK_NUMPAD7 }, { "Num8", VK_NUMPAD8 },
    { "Num9", VK_NUMPAD9 },
    { "NumMultiply", VK_MULTIPLY }, { "NumAdd", VK_ADD }, { "NumSubtract", VK_SUBTRACT },
    { "NumDecimal",  VK_DECIMAL  }, { "NumDivide", VK_DIVIDE },
    { "F1",  VK_F1  }, { "F2",  VK_F2  }, { "F3",  VK_F3  }, { "F4",  VK_F4  },
    { "F5",  VK_F5  }, { "F6",  VK_F6  }, { "F7",  VK_F7  }, { "F8",  VK_F8  },
    { "F9",  VK_F9  }, { "F10", VK_F10 }, { "F11", VK_F11 }, { "F12", VK_F12 },
    { "Mouse1", VK_LBUTTON }, { "Mouse2", VK_RBUTTON }, { "Mouse3", VK_MBUTTON },
    { NULL, 0 }
};

static int iequals(const char *a, const char *b) {
    return _stricmp(a, b) == 0;
}

/* ---- controller button-name → ME_XI_* bitmask ----------------------------- */
typedef struct { const char *name; unsigned mask; } me_xi_btnmap;
static const me_xi_btnmap g_xi_btnmap[] = {
    { "DPadUp",      ME_XI_DPAD_UP      },
    { "DPadDown",    ME_XI_DPAD_DOWN    },
    { "DPadLeft",    ME_XI_DPAD_LEFT    },
    { "DPadRight",   ME_XI_DPAD_RIGHT   },
    { "Start",       ME_XI_START        },
    { "Back",        ME_XI_BACK         },
    { "LStick",      ME_XI_LSTICK       },
    { "RStick",      ME_XI_RSTICK       },
    { "LB",          ME_XI_LB           },
    { "RB",          ME_XI_RB           },
    { "A",           ME_XI_A            },
    { "B",           ME_XI_B            },
    { "X",           ME_XI_X            },
    { "Y",           ME_XI_Y            },
    { "LT",          ME_XI_LT           },
    { "RT",          ME_XI_RT           },
    { "LStickUp",    ME_XI_LSTICK_UP    },
    { "LStickDown",  ME_XI_LSTICK_DOWN  },
    { "LStickLeft",  ME_XI_LSTICK_LEFT  },
    { "LStickRight", ME_XI_LSTICK_RIGHT },
    { "RStickUp",    ME_XI_RSTICK_UP    },
    { "RStickDown",  ME_XI_RSTICK_DOWN  },
    { "RStickLeft",  ME_XI_RSTICK_LEFT  },
    { "RStickRight", ME_XI_RSTICK_RIGHT },
    { NULL, 0 }
};

/* Parse a controller chord like "Back+DPadUp" or "A".
   Returns a bitmask of all named buttons. 0 = no valid buttons found. */
static unsigned xi_chord_from_str(const char *s) {
    if (!s || !*s) return 0;
    unsigned mask = 0;
    char buf[64];
    size_t n = strlen(s);
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, s, n); buf[n] = '\0';

    char *p = buf;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        char *tok = p;
        while (*p && *p != '+') p++;
        char *end = p;
        while (end > tok && (end[-1] == ' ' || end[-1] == '\t')) end--;
        char saved = *end; *end = '\0';
        if (*tok) {
            for (const me_xi_btnmap *m = g_xi_btnmap; m->name; m++) {
                if (iequals(tok, m->name)) { mask |= m->mask; break; }
            }
        }
        *end = saved;
        if (*p == '+') p++;
    }
    return mask;
}

/* Parse a scalar or sequence YAML node into a me_xi_bindings. */
static void parse_xi_bindings(yaml_document_t *d, yaml_node_t *node, me_xi_bindings *out) {
    if (!node) return;
    if (node->type == YAML_SCALAR_NODE) {
        unsigned m = xi_chord_from_str((const char *)node->data.scalar.value);
        if (m) { out->b[0].buttons = m; out->count = 1; }
    } else if (node->type == YAML_SEQUENCE_NODE) {
        out->count = 0;
        for (yaml_node_item_t *it = node->data.sequence.items.start;
             it < node->data.sequence.items.top && out->count < ME_XI_MAX_BINDINGS;
             it++) {
            yaml_node_t *item = yaml_document_get_node(d, *it);
            if (!item || item->type != YAML_SCALAR_NODE) continue;
            unsigned m = xi_chord_from_str((const char *)item->data.scalar.value);
            if (m) out->b[out->count++].buttons = m;
        }
    }
}

/* Parse a single token like "X", "F11", "Up", "RShift", "VK_BA". Returns VK or 0. */
static unsigned vk_from_token(const char *tok) {
    if (!tok || !*tok) return 0;
    /* Single letter/digit: ASCII upper. */
    if (tok[1] == '\0') {
        unsigned char c = (unsigned char)tok[0];
        if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
    }
    for (const me_keymap *k = g_keymap; k->name; k++) {
        if (iequals(tok, k->name)) return k->vk;
    }
    /* Raw virtual-key code, for keys without a friendly name. */
    if (_strnicmp(tok, "VK_", 3) == 0) {
        char *end = NULL;
        unsigned long v = strtoul(tok + 3, &end, 16);
        if (end != tok + 3 && *end == '\0' && v > 0 && v < 256) return (unsigned)v;
    }
    return 0;
}

/* Inverse of vk_from_token. The generic Shift/Ctrl/Alt VKs are written as
   raw codes: their names mean "modifier flag" to parse_chord. */
static void vk_to_token(unsigned vk, char *out, size_t out_sz) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        snprintf(out, out_sz, "%c", (char)vk);
        return;
    }
    if (vk != VK_SHIFT && vk != VK_CONTROL && vk != VK_MENU) {
        for (const me_keymap *k = g_keymap; k->name; k++) {
            if (k->vk == vk) { snprintf(out, out_sz, "%s", k->name); return; }
        }
    }
    snprintf(out, out_sz, "VK_%02X", vk & 0xFFu);
}

/* Parse a chord spec like "Ctrl+R" / "F11" / "Shift+F1". Trims whitespace
   around each token. Sets `.vk` to the last non-modifier token's VK, and
   the ctrl/alt/shift flags based on the modifiers seen. */
static me_kb_binding parse_chord(const char *s) {
    me_kb_binding out = {0};
    if (!s) return out;
    char buf[128];
    size_t n = strlen(s);
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, s, n); buf[n] = '\0';

    char *p = buf;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        char *tok = p;
        while (*p && *p != '+') p++;
        char *end = p;
        while (end > tok && (end[-1] == ' ' || end[-1] == '\t')) end--;
        char saved = *end; *end = '\0';
        if (*tok) {
            if      (iequals(tok, "Ctrl")  || iequals(tok, "Control")) out.ctrl  = 1;
            else if (iequals(tok, "Alt"))                               out.alt   = 1;
            else if (iequals(tok, "Shift"))                             out.shift = 1;
            else {
                unsigned vk = vk_from_token(tok);
                if (vk) out.vk = vk;
            }
        }
        *end = saved;
        if (*p == '+') p++;
    }
    return out;
}

void me_kb_binding_str(const me_kb_binding *b, char *out, size_t out_sz) {
    if (!out_sz) return;
    out[0] = '\0';
    if (!b || !b->vk) return;
    char key[32];
    vk_to_token(b->vk, key, sizeof(key));
    snprintf(out, out_sz, "%s%s%s%s",
             b->ctrl ? "Ctrl+" : "", b->alt ? "Alt+" : "", b->shift ? "Shift+" : "", key);
}

void me_xi_chord_str(unsigned buttons, char *out, size_t out_sz) {
    if (!out_sz) return;
    out[0] = '\0';
    size_t len = 0;
    for (const me_xi_btnmap *m = g_xi_btnmap; m->name && buttons; m++) {
        if (!(buttons & m->mask)) continue;
        buttons &= ~m->mask;
        int n = snprintf(out + len, out_sz - len, "%s%s", len ? "+" : "", m->name);
        if (n < 0 || (size_t)n >= out_sz - len) break;
        len += (size_t)n;
    }
}

int me_kb_bindings_equal(const me_kb_bindings *a, const me_kb_bindings *b) {
    if (a->count != b->count) return 0;
    for (int i = 0; i < a->count; i++) {
        const me_kb_binding *x = &a->b[i], *y = &b->b[i];
        if (x->vk != y->vk || x->ctrl != y->ctrl || x->alt != y->alt || x->shift != y->shift)
            return 0;
    }
    return 1;
}

int me_xi_bindings_equal(const me_xi_bindings *a, const me_xi_bindings *b) {
    if (a->count != b->count) return 0;
    for (int i = 0; i < a->count; i++) {
        if (a->b[i].buttons != b->b[i].buttons) return 0;
    }
    return 1;
}

/* ---- input-name → me_input_id -------------------------------------------- */
typedef struct { const char *name; me_input_id id; const char *label; } me_input_name;
static const me_input_name g_input_names[] = {
    { "dpad_up",      ME_IN_DPAD_UP,      "D-Pad Up"           },
    { "dpad_down",    ME_IN_DPAD_DOWN,    "D-Pad Down"         },
    { "dpad_left",    ME_IN_DPAD_LEFT,    "D-Pad Left"         },
    { "dpad_right",   ME_IN_DPAD_RIGHT,   "D-Pad Right"        },
    { "a",            ME_IN_A,            "A"                  },
    { "b",            ME_IN_B,            "B"                  },
    { "x",            ME_IN_X,            "X"                  },
    { "y",            ME_IN_Y,            "Y"                  },
    { "lb",           ME_IN_LB,           "LB"                 },
    { "rb",           ME_IN_RB,           "RB"                 },
    { "lt",           ME_IN_LT,           "LT"                 },
    { "rt",           ME_IN_RT,           "RT"                 },
    { "lstick",       ME_IN_LSTICK,       "Left Stick Click"   },
    { "rstick",       ME_IN_RSTICK,       "Right Stick Click"  },
    { "start",        ME_IN_START,        "Start"              },
    { "back",         ME_IN_BACK,         "Back (Select)"      },
    { "lstick_up",    ME_IN_LSTICK_UP,    "Left Stick Up"      },
    { "lstick_down",  ME_IN_LSTICK_DOWN,  "Left Stick Down"    },
    { "lstick_left",  ME_IN_LSTICK_LEFT,  "Left Stick Left"    },
    { "lstick_right", ME_IN_LSTICK_RIGHT, "Left Stick Right"   },
    { "rstick_up",    ME_IN_RSTICK_UP,    "Right Stick Up"     },
    { "rstick_down",  ME_IN_RSTICK_DOWN,  "Right Stick Down"   },
    { "rstick_left",  ME_IN_RSTICK_LEFT,  "Right Stick Left"   },
    { "rstick_right", ME_IN_RSTICK_RIGHT, "Right Stick Right"  },
    { NULL, 0, NULL }
};

static int input_id_from_name(const char *name, me_input_id *out) {
    if (!name) return 0;
    for (const me_input_name *n = g_input_names; n->name; n++) {
        if (iequals(name, n->name)) { *out = n->id; return 1; }
    }
    return 0;
}

static const char *input_yaml_name(me_input_id id) {
    for (const me_input_name *n = g_input_names; n->name; n++) {
        if (n->id == id) return n->name;
    }
    return "?";
}

const char *me_input_label(me_input_id id) {
    for (const me_input_name *n = g_input_names; n->name; n++) {
        if (n->id == id) return n->label;
    }
    return "?";
}

/* ---- hotkeys ------------------------------------------------------------- */
static const struct { const char *name; const char *label; } g_hotkey_names[ME_HK_COUNT] = {
    [ME_HK_CYCLE_ASPECT]      = { "cycle_aspect_ratio", "Cycle Aspect Ratio" },
    [ME_HK_TOGGLE_FULLSCREEN] = { "toggle_fullscreen",  "Toggle Fullscreen"  },
    [ME_HK_EXIT_FULLSCREEN]   = { "exit_fullscreen",    "Exit Fullscreen"    },
    [ME_HK_QUIT]              = { "quit",               "Quit"               },
    [ME_HK_HARD_RESET]        = { "hard_reset",         "Hard Reset"         },
};

const char *me_hotkey_label(me_hotkey_id id) {
    return (unsigned)id < ME_HK_COUNT ? g_hotkey_names[id].label : "?";
}

/* ---- input source -------------------------------------------------------- */
static const char *source_name(me_input_source s) {
    switch (s) {
        case ME_SRC_KEYBOARD:   return "keyboard";
        case ME_SRC_CONTROLLER: return "controller";
        default:                return "both";
    }
}

static me_input_source parse_source(const char *s, me_input_source defv) {
    if (!s) return defv;
    if (iequals(s, "both"))       return ME_SRC_BOTH;
    if (iequals(s, "keyboard"))   return ME_SRC_KEYBOARD;
    if (iequals(s, "controller")) return ME_SRC_CONTROLLER;
    return defv;
}

static int clamp_slot(int v) {
    return v < 0 ? 0 : (v >= ME_XI_SLOTS ? ME_XI_SLOTS - 1 : v);
}

/* ---- defaults ------------------------------------------------------------- */
void me_settings_defaults(me_settings *out) {
    memset(out, 0, sizeof(*out));
    out->aspect = ME_ASPECT_1_1;
    out->force_vulkan     = 1;
    out->vk_no_vsync      = 1;
    out->vk_exclusive_fullscreen = 1;
    out->match_display_hz = 1;
    out->thread_affinity  = 1;
    out->exclusive_mode   = 0;
    out->low_latency      = 1;
    out->match_strict     = 1;  /* competition-safe by default */

    /* LSFG defaults: 2x multiplier, full-resolution optical flow, quality mode.
     * These match the "best quality, minimum latency cost" preset. */
    out->lsfg_multiplier = 2;
    out->lsfg_flow_scale = 1.0f;
    out->lsfg_perf_mode  = 0;

    out->hk[ME_HK_CYCLE_ASPECT].b[0]      = parse_chord("F1");     out->hk[ME_HK_CYCLE_ASPECT].count      = 1;
    out->hk[ME_HK_TOGGLE_FULLSCREEN].b[0] = parse_chord("F11");
    out->hk[ME_HK_TOGGLE_FULLSCREEN].b[1] = parse_chord("Alt+Return"); out->hk[ME_HK_TOGGLE_FULLSCREEN].count = 2;
    out->hk[ME_HK_EXIT_FULLSCREEN].b[0]   = parse_chord("Escape"); out->hk[ME_HK_EXIT_FULLSCREEN].count   = 1;
    out->hk[ME_HK_QUIT].b[0]              = parse_chord("Alt+F4"); out->hk[ME_HK_QUIT].count              = 1;
    out->hk[ME_HK_HARD_RESET].b[0]        = parse_chord("Ctrl+R"); out->hk[ME_HK_HARD_RESET].count        = 1;

    for (int p = 0; p < ME_MAX_PLAYERS; p++) {
        out->input_source[p] = ME_SRC_BOTH;
        out->xi_index[p] = p;
        out->lstick_as_dpad[p] = 1;
        out->rumble[p] = 1;
    }

/* Helper: assign a single default into a bindings slot. */
#define SET1(slot, name) do { (slot).b[0] = parse_chord(name); (slot).count = 1; } while(0)
#define XI1(slot, mask)  do { (slot).b[0].buttons = (mask); (slot).count = 1; } while(0)

    /* Universal defaults. Players 2-4 get the controller layout only; the
       keyboard is player 1's. The controller's face buttons go where the
       RetroPad has them, not by name: RetroPad B is the bottom button (Xbox
       A), A the right one (Xbox B), Y the left (X), X the top (Y). Cores lay
       their console's controller out on the RetroPad by position; consoles
       where that isn't enough have their own defaults (consoles.c). */
    SET1(out->universal[0].keys[ME_IN_DPAD_UP],    "Up");
    SET1(out->universal[0].keys[ME_IN_DPAD_DOWN],  "Down");
    SET1(out->universal[0].keys[ME_IN_DPAD_LEFT],  "Left");
    SET1(out->universal[0].keys[ME_IN_DPAD_RIGHT], "Right");
    SET1(out->universal[0].keys[ME_IN_A],          "X");
    SET1(out->universal[0].keys[ME_IN_B],          "Z");
    SET1(out->universal[0].keys[ME_IN_X],          "S");
    SET1(out->universal[0].keys[ME_IN_Y],          "A");
    SET1(out->universal[0].keys[ME_IN_LB],         "Q");
    SET1(out->universal[0].keys[ME_IN_RB],         "W");
    SET1(out->universal[0].keys[ME_IN_START],      "Return");
    SET1(out->universal[0].keys[ME_IN_BACK],       "RShift");
    for (int p = 0; p < ME_MAX_PLAYERS; p++) {
        me_control_map *m = &out->universal[p];
        XI1(m->xi[ME_IN_DPAD_UP],    ME_XI_DPAD_UP);
        XI1(m->xi[ME_IN_DPAD_DOWN],  ME_XI_DPAD_DOWN);
        XI1(m->xi[ME_IN_DPAD_LEFT],  ME_XI_DPAD_LEFT);
        XI1(m->xi[ME_IN_DPAD_RIGHT], ME_XI_DPAD_RIGHT);
        XI1(m->xi[ME_IN_A],          ME_XI_B);
        XI1(m->xi[ME_IN_B],          ME_XI_A);
        XI1(m->xi[ME_IN_X],          ME_XI_Y);
        XI1(m->xi[ME_IN_Y],          ME_XI_X);
        XI1(m->xi[ME_IN_LB],         ME_XI_LB);
        XI1(m->xi[ME_IN_RB],         ME_XI_RB);
        XI1(m->xi[ME_IN_LT],         ME_XI_LT);
        XI1(m->xi[ME_IN_RT],         ME_XI_RT);
        XI1(m->xi[ME_IN_LSTICK],     ME_XI_LSTICK);
        XI1(m->xi[ME_IN_RSTICK],     ME_XI_RSTICK);
        XI1(m->xi[ME_IN_START],      ME_XI_START);
        XI1(m->xi[ME_IN_BACK],       ME_XI_BACK);
    }
#undef SET1
#undef XI1
}

void me_settings_free(me_settings *s) {
    if (!s) return;
    free(s->cores);
    s->cores = NULL;
    s->cores_n = 0;
    free(s->console_controls);
    s->console_controls = NULL;
    s->console_controls_n = 0;
}

/* ---- libyaml DOM walk ----------------------------------------------------- */
/* We use the libyaml document API (yaml_parser_load → yaml_document_t).
   It builds a node graph we can walk by index, much simpler than the streaming
   event API for a small file like ours. */

static yaml_node_t *doc_get(yaml_document_t *d, int idx) {
    return idx ? yaml_document_get_node(d, idx) : NULL;
}

/* Find a mapping child by key name. Returns the value node or NULL. */
static yaml_node_t *map_get(yaml_document_t *d, yaml_node_t *m, const char *key) {
    if (!m || m->type != YAML_MAPPING_NODE) return NULL;
    for (yaml_node_pair_t *p = m->data.mapping.pairs.start;
         p < m->data.mapping.pairs.top; p++) {
        yaml_node_t *k = doc_get(d, p->key);
        if (!k || k->type != YAML_SCALAR_NODE) continue;
        const char *kn = (const char *)k->data.scalar.value;
        if (iequals(kn, key)) return doc_get(d, p->value);
    }
    return NULL;
}

static const char *scalar_str(yaml_node_t *n) {
    if (!n || n->type != YAML_SCALAR_NODE) return NULL;
    return (const char *)n->data.scalar.value;
}

static int scalar_bool(yaml_node_t *n, int defv) {
    const char *s = scalar_str(n);
    if (!s) return defv;
    if (iequals(s, "true") || iequals(s, "yes") || iequals(s, "on") || strcmp(s, "1") == 0) return 1;
    if (iequals(s, "false") || iequals(s, "no") || iequals(s, "off") || strcmp(s, "0") == 0) return 0;
    return defv;
}

static int scalar_int(yaml_node_t *n, int defv) {
    const char *s = scalar_str(n);
    if (!s) return defv;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s) return defv;
    return (int)v;
}

static float scalar_float(yaml_node_t *n, float defv) {
    const char *s = scalar_str(n);
    if (!s) return defv;
    char *end = NULL;
    float v = strtof(s, &end);
    if (end == s) return defv;
    return v;
}

static const char *const g_screens_names[] = { "both", "top", "bottom" };

static me_screens parse_screens(const char *s, me_screens defv) {
    if (!s) return defv;
    for (int i = 0; i < 3; i++)
        if (_stricmp(s, g_screens_names[i]) == 0) return (me_screens)i;
    return defv;
}

static me_aspect_mode parse_aspect(const char *s, me_aspect_mode defv) {
    if (!s) return defv;
    if (strcmp(s, "1:1") == 0)  return ME_ASPECT_1_1;
    if (strcmp(s, "4:3") == 0)  return ME_ASPECT_4_3;
    if (strcmp(s, "16:9") == 0) return ME_ASPECT_16_9;
    return defv;
}

/* Keyboard bindings: a scalar chord or a sequence of chords. An empty
   sequence unbinds. */
static void parse_kb_bindings(yaml_document_t *d, yaml_node_t *kb_node, me_kb_bindings *out) {
    if (!kb_node) return;
    if (kb_node->type == YAML_SCALAR_NODE) {
        const char *kb = scalar_str(kb_node);
        if (kb && *kb) { out->b[0] = parse_chord(kb); out->count = 1; }
    } else if (kb_node->type == YAML_SEQUENCE_NODE) {
        out->count = 0;
        for (yaml_node_item_t *it = kb_node->data.sequence.items.start;
             it < kb_node->data.sequence.items.top && out->count < ME_KB_MAX_BINDINGS;
             it++) {
            const char *kb = scalar_str(doc_get(d, *it));
            if (kb && *kb) out->b[out->count++] = parse_chord(kb);
        }
    }
}

/* Walk a `playerN:` block: each child key is an input name (a, b, dpad_up,
   lstick_up, ...). Each value is itself a mapping with `keyboard:` and
   `controller:` bindings. Inputs not listed keep their current value.
   With `overrides`, the bit of every listed input is set. */
static void parse_player(yaml_document_t *d, yaml_node_t *pn, me_control_map *out,
                         unsigned *overrides) {
    if (!pn || pn->type != YAML_MAPPING_NODE) return;
    for (yaml_node_pair_t *p = pn->data.mapping.pairs.start;
         p < pn->data.mapping.pairs.top; p++) {
        yaml_node_t *k = doc_get(d, p->key);
        if (!k || k->type != YAML_SCALAR_NODE) continue;
        me_input_id id;
        if (!input_id_from_name((const char *)k->data.scalar.value, &id)) continue;
        yaml_node_t *v = doc_get(d, p->value);
        if (!v || v->type != YAML_MAPPING_NODE) continue;
        parse_kb_bindings(d, map_get(d, v, "keyboard"), &out->keys[id]);
        parse_xi_bindings(d, map_get(d, v, "controller"), &out->xi[id]);
        if (overrides) *overrides |= 1u << id;
    }
}

static const char *const g_player_keys[ME_MAX_PLAYERS] = { "player1", "player2", "player3", "player4" };

static void parse_players(yaml_document_t *d, yaml_node_t *parent, me_control_map *maps,
                          unsigned *overrides) {
    for (int pl = 0; pl < ME_MAX_PLAYERS; pl++)
        parse_player(d, map_get(d, parent, g_player_keys[pl]), &maps[pl],
                     overrides ? &overrides[pl] : NULL);
}

static int parse_hotkey(yaml_document_t *d, yaml_node_t *root, const char *key,
                        me_kb_bindings *kb_out, me_xi_bindings *xi_out) {
    yaml_node_t *m = map_get(d, root, key);
    if (!m || m->type != YAML_MAPPING_NODE) return 0;
    parse_kb_bindings(d, map_get(d, m, "keyboard"), kb_out);
    parse_xi_bindings(d, map_get(d, m, "controller"), xi_out);
    return 1;
}

static void load_document(yaml_document_t *doc, yaml_node_t *root, me_settings *out) {
    /* video */
    yaml_node_t *video = map_get(doc, root, "video");
    if (video) {
        out->fullscreen_on_launch = scalar_bool(map_get(doc, video, "fullscreen_on_launch"),
                                                out->fullscreen_on_launch);
        out->aspect      = parse_aspect(scalar_str(map_get(doc, video, "aspect")), out->aspect);
        out->screens     = parse_screens(scalar_str(map_get(doc, video, "screens")), out->screens);
        out->force_gdi   = scalar_bool(map_get(doc, video, "force_gdi"),   out->force_gdi);
        out->force_d3d11 = scalar_bool(map_get(doc, video, "force_d3d11"), out->force_d3d11);
        out->force_vulkan = scalar_bool(map_get(doc, video, "force_vulkan"), out->force_vulkan);
        out->vk_no_vsync = scalar_bool(map_get(doc, video, "vk_no_vsync"), out->vk_no_vsync);
        out->vk_mailbox  = scalar_bool(map_get(doc, video, "vk_mailbox"),  out->vk_mailbox);
        out->vk_validate = scalar_bool(map_get(doc, video, "vk_validate"), out->vk_validate);
        out->vk_exclusive_fullscreen = scalar_bool(map_get(doc, video, "vk_exclusive_fullscreen"),
                                                   out->vk_exclusive_fullscreen);
        out->match_display_hz = scalar_bool(map_get(doc, video, "match_display_hz"),
                                            out->match_display_hz);
        out->match_strict = scalar_bool(map_get(doc, video, "match_strict"),
                                        out->match_strict);
    }

    /* lsfg */
    yaml_node_t *lsfg = map_get(doc, root, "lsfg");
    if (lsfg) {
        out->lsfg_enabled    = scalar_bool (map_get(doc, lsfg, "enabled"),     out->lsfg_enabled);
        out->lsfg_multiplier = scalar_int  (map_get(doc, lsfg, "multiplier"),  out->lsfg_multiplier);
        out->lsfg_flow_scale = scalar_float(map_get(doc, lsfg, "flow_scale"),  out->lsfg_flow_scale);
        out->lsfg_perf_mode  = scalar_bool (map_get(doc, lsfg, "performance"), out->lsfg_perf_mode);
    }

    /* audio */
    yaml_node_t *audio = map_get(doc, root, "audio");
    if (audio) {
        out->no_audio        = scalar_bool(map_get(doc, audio, "no_audio"),       out->no_audio);
        out->exclusive_mode  = scalar_bool(map_get(doc, audio, "exclusive_mode"),  out->exclusive_mode);
        out->low_latency     = scalar_bool(map_get(doc, audio, "low_latency"),     out->low_latency);
    }

    /* system */
    yaml_node_t *sys = map_get(doc, root, "system");
    if (sys) {
        out->thread_affinity = scalar_bool(map_get(doc, sys, "thread_affinity"), out->thread_affinity);
    }

    /* run-ahead */
    yaml_node_t *ra = map_get(doc, root, "runahead");
    if (ra) {
        const char *s = scalar_str(map_get(doc, ra, "frames"));
        if (s) {
            int n = atoi(s);
            if (n < 0) n = 0;
            if (n > 10) n = 10;
            out->runahead_frames = n;
        }
    }

    /* log */
    yaml_node_t *logn = map_get(doc, root, "log");
    if (logn) {
        out->pace_log   = scalar_bool(map_get(doc, logn, "pace_log"),   out->pace_log);
        out->timing_log = scalar_bool(map_get(doc, logn, "timing_log"), out->timing_log);
        out->latency_log = scalar_bool(map_get(doc, logn, "latency_log"), out->latency_log);
        out->env_trace  = scalar_bool(map_get(doc, logn, "env_trace"),  out->env_trace);
    }

    /* hotkeys */
    yaml_node_t *hk = map_get(doc, root, "hotkeys");
    if (hk) {
        out->hk_source   = parse_source(scalar_str(map_get(doc, hk, "input")), out->hk_source);
        out->hk_xi_index = clamp_slot(scalar_int(map_get(doc, hk, "controller_index"), out->hk_xi_index));
        for (int i = 0; i < ME_HK_COUNT; i++)
            parse_hotkey(doc, hk, g_hotkey_names[i].name, &out->hk[i], &out->hk_xi[i]);
        /* Older files call hard_reset "reset". */
        if (!map_get(doc, hk, "hard_reset"))
            parse_hotkey(doc, hk, "reset", &out->hk[ME_HK_HARD_RESET], &out->hk_xi[ME_HK_HARD_RESET]);
    }

    /* controls */
    yaml_node_t *controls = map_get(doc, root, "controls");
    /* Legacy controller slot: root `controller:` or `controls: controller:`. */
    yaml_node_t *legacy_ctrl[2] = { map_get(doc, root, "controller"), map_get(doc, controls, "controller") };
    for (int i = 0; i < 2; i++) {
        if (legacy_ctrl[i])
            out->xi_index[0] = scalar_int(map_get(doc, legacy_ctrl[i], "player1_index"), out->xi_index[0]);
    }
    if (controls) {
        char key[32];
        for (int pl = 0; pl < ME_MAX_PLAYERS; pl++) {
            snprintf(key, sizeof(key), "player%d_input", pl + 1);
            out->input_source[pl] = parse_source(scalar_str(map_get(doc, controls, key)), out->input_source[pl]);
            snprintf(key, sizeof(key), "player%d_controller", pl + 1);
            out->xi_index[pl] = scalar_int(map_get(doc, controls, key), out->xi_index[pl]);
            snprintf(key, sizeof(key), "player%d_lstick_as_dpad", pl + 1);
            out->lstick_as_dpad[pl] = scalar_bool(map_get(doc, controls, key), out->lstick_as_dpad[pl]);
            snprintf(key, sizeof(key), "player%d_rumble", pl + 1);
            out->rumble[pl] = scalar_bool(map_get(doc, controls, key), out->rumble[pl]);
        }
        yaml_node_t *uni = map_get(doc, controls, "universal");
        if (uni) parse_players(doc, uni, out->universal, NULL);
        out->show_advanced_inputs = scalar_bool(map_get(doc, controls, "show_advanced"),
                                                out->show_advanced_inputs);
        yaml_node_t *adv = map_get(doc, controls, "advanced");
        if (adv) parse_players(doc, adv, out->advanced, NULL);
        out->show_cursor_fullscreen = scalar_bool(map_get(doc, controls, "show_cursor_fullscreen"),
                                                  out->show_cursor_fullscreen);
    }
    for (int pl = 0; pl < ME_MAX_PLAYERS; pl++) out->xi_index[pl] = clamp_slot(out->xi_index[pl]);

    /* cores */
    yaml_node_t *cores = map_get(doc, root, "cores");
    if (cores && cores->type == YAML_MAPPING_NODE) {
        size_t n = (size_t)(cores->data.mapping.pairs.top - cores->data.mapping.pairs.start);
        free(out->cores);
        out->cores = (struct me_core_entry *)calloc(n ? n : 1, sizeof(*out->cores));
        out->cores_n = 0;
        if (out->cores) {
            for (yaml_node_pair_t *p = cores->data.mapping.pairs.start;
                 p < cores->data.mapping.pairs.top; p++) {
                yaml_node_t *k = doc_get(doc, p->key);
                yaml_node_t *v = doc_get(doc, p->value);
                if (!k || k->type != YAML_SCALAR_NODE || !v || v->type != YAML_MAPPING_NODE) continue;
                struct me_core_entry *e = &out->cores[out->cores_n++];
                strncpy(e->name, (const char *)k->data.scalar.value, sizeof(e->name) - 1);
                /* Inherit universal as baseline, then per-core overrides apply. */
                memcpy(e->controls, out->universal, sizeof(e->controls));
                e->use_universal = 1;

                const char *dll = scalar_str(map_get(doc, v, "dll"));
                if (dll) {
                    strncpy(e->dll, dll, sizeof(e->dll) - 1);
                }
                e->use_universal = scalar_bool(map_get(doc, v, "use_universal"), 1);
                yaml_node_t *cc = map_get(doc, v, "controls");
                if (cc) parse_players(doc, cc, e->controls, e->overrides);
            }
        }
    }

    /* console -> the player's controls for its games */
    yaml_node_t *ctl = map_get(doc, root, "console_controls");
    if (ctl && ctl->type == YAML_MAPPING_NODE) {
        size_t n = (size_t)(ctl->data.mapping.pairs.top - ctl->data.mapping.pairs.start);
        free(out->console_controls);
        out->console_controls = (struct me_console_controls *)calloc(n ? n : 1, sizeof(*out->console_controls));
        out->console_controls_n = 0;
        for (yaml_node_pair_t *p = ctl->data.mapping.pairs.start;
             out->console_controls && p < ctl->data.mapping.pairs.top; p++) {
            const char *key = scalar_str(doc_get(doc, p->key));
            yaml_node_t *v = doc_get(doc, p->value);
            if (!key || !*key || !v || v->type != YAML_MAPPING_NODE) continue;
            struct me_console_controls *e = &out->console_controls[out->console_controls_n++];
            snprintf(e->key, sizeof(e->key), "%s", key);
            parse_players(doc, v, e->controls, e->overrides);
        }
    }

    /* console -> core picks */
    yaml_node_t *cc = map_get(doc, root, "console_cores");
    if (cc && cc->type == YAML_MAPPING_NODE) {
        out->console_cores_n = 0;
        for (yaml_node_pair_t *p = cc->data.mapping.pairs.start;
             p < cc->data.mapping.pairs.top; p++) {
            const char *console = scalar_str(doc_get(doc, p->key));
            const char *dll = scalar_str(doc_get(doc, p->value));
            if (console && *console) me_settings_set_console_core(out, console, dll);
        }
    }

    /* console -> multiplayer adapter */
    yaml_node_t *ca = map_get(doc, root, "console_adapters");
    if (ca && ca->type == YAML_MAPPING_NODE) {
        out->console_adapters_n = 0;
        for (yaml_node_pair_t *p = ca->data.mapping.pairs.start;
             p < ca->data.mapping.pairs.top; p++) {
            const char *console = scalar_str(doc_get(doc, p->key));
            const char *adapter = scalar_str(doc_get(doc, p->value));
            if (console && *console) me_settings_set_console_adapter(out, console, adapter);
        }
    }

    /* recent ROMs */
    yaml_node_t *recent = map_get(doc, root, "recent");
    if (recent && recent->type == YAML_SEQUENCE_NODE) {
        out->recent_n = 0;
        for (yaml_node_item_t *it = recent->data.sequence.items.start;
             it < recent->data.sequence.items.top && out->recent_n < ME_RECENT_MAX; it++) {
            const char *path = scalar_str(doc_get(doc, *it));
            if (path && *path)
                snprintf(out->recent[out->recent_n++], sizeof(out->recent[0]), "%s", path);
        }
    }

    /* ROM fingerprint -> picked console, most recent first */
    yaml_node_t *cp = map_get(doc, root, "console_picks");
    if (cp && cp->type == YAML_MAPPING_NODE) {
        out->console_picks_n = 0;
        for (yaml_node_pair_t *p = cp->data.mapping.pairs.start;
             p < cp->data.mapping.pairs.top && out->console_picks_n < ME_CONSOLE_PICKS_MAX; p++) {
            const char *rom = scalar_str(doc_get(doc, p->key));
            const char *console = scalar_str(doc_get(doc, p->value));
            if (!rom || !*rom || !console || !*console) continue;
            struct me_console_pick *e = &out->console_picks[out->console_picks_n++];
            snprintf(e->rom, sizeof(e->rom), "%s", rom);
            snprintf(e->console, sizeof(e->console), "%s", console);
        }
    }
}

/* Parse whatever input `parser` was given into `out`. Returns 0 on success. */
static int load_from_parser(yaml_parser_t *parser, const char *what, me_settings *out) {
    yaml_document_t doc;
    if (!yaml_parser_load(parser, &doc)) {
        fprintf(stderr, "[settings] parse error in %s at line %zu col %zu: %s\n",
                what, parser->problem_mark.line + 1, parser->problem_mark.column + 1,
                parser->problem ? parser->problem : "(unknown)");
        return -1;
    }
    yaml_node_t *root = yaml_document_get_root_node(&doc);
    if (!root || root->type != YAML_MAPPING_NODE) {
        fprintf(stderr, "[settings] %s: root is not a mapping\n", what);
        yaml_document_delete(&doc);
        return -1;
    }
    load_document(&doc, root, out);
    yaml_document_delete(&doc);
    return 0;
}

int me_settings_load(const char *path, me_settings *out) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[settings] %s not found; using defaults\n", path);
        return 1;
    }

    yaml_parser_t parser;
    if (!yaml_parser_initialize(&parser)) {
        fclose(f);
        fprintf(stderr, "[settings] yaml_parser_initialize failed\n");
        return -1;
    }
    yaml_parser_set_input_file(&parser, f);
    int rc = load_from_parser(&parser, path, out);
    yaml_parser_delete(&parser);
    fclose(f);
    return rc;
}

void me_settings_load_template(me_settings *out) {
    me_settings_defaults(out);
    yaml_parser_t parser;
    if (!yaml_parser_initialize(&parser)) return;
    yaml_parser_set_input_string(&parser, (const unsigned char *)k_settings_yaml_default,
                                 strlen(k_settings_yaml_default));
    load_from_parser(&parser, "built-in settings template", out);
    yaml_parser_delete(&parser);
}

/* ---- writer --------------------------------------------------------------- */
static const char *yn(int v) { return v ? "true" : "false"; }

/* Double-quoted YAML scalar (paths can contain ':', '#', quotes, ...). */
static void write_quoted(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') fputc('\\', f);
        fputc(*s, f);
    }
    fputc('"', f);
}

static void write_kb_list(FILE *f, const me_kb_bindings *b) {
    char buf[64];
    fputc('[', f);
    for (int i = 0; i < b->count; i++) {
        me_kb_binding_str(&b->b[i], buf, sizeof(buf));
        fprintf(f, "%s%s", i ? ", " : "", buf);
    }
    fputc(']', f);
}

static void write_xi_list(FILE *f, const me_xi_bindings *b) {
    char buf[256];
    fputc('[', f);
    for (int i = 0; i < b->count; i++) {
        me_xi_chord_str(b->b[i].buttons, buf, sizeof(buf));
        fprintf(f, "%s%s", i ? ", " : "", buf);
    }
    fputc(']', f);
}

/* One `playerN:` block. With `overrides`, only those inputs are written
   (a core's block lists just what it overrides; the rest follows universal). */
static void write_player(FILE *f, const char *indent, const char *key,
                         const me_control_map *m, const unsigned *overrides) {
    int any = 0;
    for (int id = 0; id < ME_IN_COUNT; id++) {
        if (overrides && !(*overrides & (1u << id))) continue;
        if (!any) { fprintf(f, "%s%s:\n", indent, key); any = 1; }
        fprintf(f, "%s  %s:\n%s    keyboard:   ", indent, input_yaml_name((me_input_id)id), indent);
        write_kb_list(f, &m->keys[id]);
        fprintf(f, "\n%s    controller: ", indent);
        write_xi_list(f, &m->xi[id]);
        fputc('\n', f);
    }
    if (!any) fprintf(f, "%s%s: {}\n", indent, key);
}

int me_settings_save(const char *path, const me_settings *s) {
    /* Write a sibling temp file and swap it in, so a crash mid-write can't
       leave a truncated settings.yaml. */
    char tmp[MAX_PATH];
    if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= sizeof(tmp)) return -1;
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        fprintf(stderr, "[settings] cannot write %s\n", tmp);
        return -1;
    }

    static const char *const aspect_names[] = { "1:1", "4:3", "16:9" };
    fprintf(f,
        "# laggueless settings.\n"
        "# laggueless rewrites this file when settings are changed from its menus.\n"
        "# Hand edits are fine while laggueless is closed.\n"
        "\n");
    fprintf(f, "video:\n");
    fprintf(f, "  fullscreen_on_launch: %s\n", yn(s->fullscreen_on_launch));
    fprintf(f, "  aspect: \"%s\"\n", aspect_names[(int)s->aspect <= 2 ? (int)s->aspect : 0]);
    fprintf(f, "  screens: %s\n", g_screens_names[(unsigned)s->screens <= 2 ? (int)s->screens : 0]);
    fprintf(f, "  force_gdi: %s\n", yn(s->force_gdi));
    fprintf(f, "  force_d3d11: %s\n", yn(s->force_d3d11));
    fprintf(f, "  force_vulkan: %s\n", yn(s->force_vulkan));
    fprintf(f, "  vk_no_vsync: %s\n", yn(s->vk_no_vsync));
    fprintf(f, "  vk_mailbox: %s\n", yn(s->vk_mailbox));
    fprintf(f, "  vk_validate: %s\n", yn(s->vk_validate));
    fprintf(f, "  vk_exclusive_fullscreen: %s\n", yn(s->vk_exclusive_fullscreen));
    fprintf(f, "  match_display_hz: %s\n", yn(s->match_display_hz));
    fprintf(f, "  match_strict: %s\n", yn(s->match_strict));

    fprintf(f, "\naudio:\n");
    fprintf(f, "  no_audio: %s\n", yn(s->no_audio));
    fprintf(f, "  exclusive_mode: %s\n", yn(s->exclusive_mode));
    fprintf(f, "  low_latency: %s\n", yn(s->low_latency));

    fprintf(f, "\nlsfg:\n");
    fprintf(f, "  enabled: %s\n", yn(s->lsfg_enabled));
    fprintf(f, "  multiplier: %d\n", s->lsfg_multiplier);
    fprintf(f, "  flow_scale: %.2f\n", (double)s->lsfg_flow_scale);
    fprintf(f, "  performance: %s\n", yn(s->lsfg_perf_mode));

    fprintf(f, "\nsystem:\n");
    fprintf(f, "  thread_affinity: %s\n", yn(s->thread_affinity));

    fprintf(f, "\nrunahead:\n");
    fprintf(f, "  frames: %d\n", s->runahead_frames);

    fprintf(f, "\nlog:\n");
    fprintf(f, "  pace_log: %s\n", yn(s->pace_log));
    fprintf(f, "  timing_log: %s\n", yn(s->timing_log));
    fprintf(f, "  latency_log: %s\n", yn(s->latency_log));
    fprintf(f, "  env_trace: %s\n", yn(s->env_trace));

    fprintf(f, "\nhotkeys:\n");
    fprintf(f, "  input: %s\n", source_name(s->hk_source));
    fprintf(f, "  controller_index: %d\n", s->hk_xi_index);
    for (int i = 0; i < ME_HK_COUNT; i++) {
        fprintf(f, "  %s:\n    keyboard:   ", g_hotkey_names[i].name);
        write_kb_list(f, &s->hk[i]);
        fprintf(f, "\n    controller: ");
        write_xi_list(f, &s->hk_xi[i]);
        fputc('\n', f);
    }

    fprintf(f, "\ncontrols:\n");
    for (int pl = 0; pl < ME_MAX_PLAYERS; pl++) {
        fprintf(f, "  player%d_input: %s\n", pl + 1, source_name(s->input_source[pl]));
        fprintf(f, "  player%d_controller: %d\n", pl + 1, s->xi_index[pl]);
        fprintf(f, "  player%d_lstick_as_dpad: %s\n", pl + 1, yn(s->lstick_as_dpad[pl]));
        fprintf(f, "  player%d_rumble: %s\n", pl + 1, yn(s->rumble[pl]));
    }
    fprintf(f, "  universal:\n");
    for (int pl = 0; pl < ME_MAX_PLAYERS; pl++)
        write_player(f, "    ", g_player_keys[pl], &s->universal[pl], NULL);
    fprintf(f, "  show_cursor_fullscreen: %s\n", yn(s->show_cursor_fullscreen));
    fprintf(f, "  show_advanced: %s\n", yn(s->show_advanced_inputs));
    fprintf(f, "  advanced:\n");
    for (int pl = 0; pl < ME_MAX_PLAYERS; pl++) {
        unsigned bound = 0;   /* just the bound ones; the rest stay unbound */
        for (int id = 0; id < ME_IN_COUNT; id++)
            if (s->advanced[pl].keys[id].count || s->advanced[pl].xi[id].count) bound |= 1u << id;
        write_player(f, "    ", g_player_keys[pl], &s->advanced[pl], &bound);
    }

    fprintf(f, "\ncores:\n");
    for (size_t i = 0; i < s->cores_n; i++) {
        const struct me_core_entry *e = &s->cores[i];
        fprintf(f, "  %s:\n", e->name);
        fprintf(f, "    dll: ");
        write_quoted(f, e->dll);
        fprintf(f, "\n    use_universal: %s\n", yn(e->use_universal));
        fprintf(f, "    controls:\n");
        for (int pl = 0; pl < ME_MAX_PLAYERS; pl++)
            write_player(f, "      ", g_player_keys[pl], &e->controls[pl], &e->overrides[pl]);
    }

    fprintf(f, "\nconsole_controls:");
    if (s->console_controls_n == 0) fprintf(f, " {}");
    fputc('\n', f);
    for (size_t i = 0; i < s->console_controls_n; i++) {
        const struct me_console_controls *e = &s->console_controls[i];
        fprintf(f, "  ");
        write_quoted(f, e->key);
        fprintf(f, ":\n");
        for (int pl = 0; pl < ME_MAX_PLAYERS; pl++)
            write_player(f, "    ", g_player_keys[pl], &e->controls[pl], &e->overrides[pl]);
    }

    fprintf(f, "\nconsole_cores:");
    if (s->console_cores_n == 0) fprintf(f, " {}");
    fputc('\n', f);
    for (int i = 0; i < s->console_cores_n; i++) {
        fprintf(f, "  ");
        write_quoted(f, s->console_cores[i].console);
        fprintf(f, ": ");
        write_quoted(f, s->console_cores[i].dll);
        fputc('\n', f);
    }

    fprintf(f, "\nconsole_adapters:");
    if (s->console_adapters_n == 0) fprintf(f, " {}");
    fputc('\n', f);
    for (int i = 0; i < s->console_adapters_n; i++) {
        fprintf(f, "  ");
        write_quoted(f, s->console_adapters[i].console);
        fprintf(f, ": ");
        write_quoted(f, s->console_adapters[i].adapter);
        fputc('\n', f);
    }

    fprintf(f, "\nrecent:");
    if (s->recent_n == 0) fprintf(f, " []");
    fputc('\n', f);
    for (int i = 0; i < s->recent_n; i++) {
        fprintf(f, "  - ");
        write_quoted(f, s->recent[i]);
        fputc('\n', f);
    }

    fprintf(f, "\nconsole_picks:");
    if (s->console_picks_n == 0) fprintf(f, " {}");
    fputc('\n', f);
    for (int i = 0; i < s->console_picks_n; i++) {
        fprintf(f, "  ");
        write_quoted(f, s->console_picks[i].rom);
        fprintf(f, ": ");
        write_quoted(f, s->console_picks[i].console);
        fputc('\n', f);
    }

    int bad = ferror(f);
    if (fclose(f) != 0) bad = 1;
    if (bad) {
        fprintf(stderr, "[settings] write failed: %s\n", tmp);
        DeleteFileA(tmp);
        return -1;
    }
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
        fprintf(stderr, "[settings] could not replace %s (err=%lu)\n", path, GetLastError());
        DeleteFileA(tmp);
        return -1;
    }
    return 0;
}

void me_settings_set_universal(me_settings *s, int player, const me_control_map *m) {
    s->universal[player] = *m;
    for (size_t i = 0; i < s->cores_n; i++) {
        struct me_core_entry *e = &s->cores[i];
        for (int id = 0; id < ME_IN_COUNT; id++) {
            if (e->overrides[player] & (1u << id)) continue;
            e->controls[player].keys[id] = m->keys[id];
            e->controls[player].xi[id]   = m->xi[id];
        }
    }
}

/* Bit per input whose bindings in `m` differ from `base`. */
static unsigned changed_inputs(const me_control_map *m, const me_control_map *base) {
    unsigned bits = 0;
    for (int id = 0; id < ME_IN_COUNT; id++) {
        if (!me_kb_bindings_equal(&m->keys[id], &base->keys[id]) ||
            !me_xi_bindings_equal(&m->xi[id], &base->xi[id]))
            bits |= 1u << id;
    }
    return bits;
}

void me_settings_set_core_map(me_settings *s, int core, int player, const me_control_map *m,
                              const me_control_map *base) {
    struct me_core_entry *e = &s->cores[core];
    e->controls[player] = *m;
    e->overrides[player] = changed_inputs(m, base);
}

int me_settings_find_console_controls(const me_settings *s, const char *key) {
    if (!key || !*key) return -1;
    for (size_t i = 0; i < s->console_controls_n; i++)
        if (iequals(s->console_controls[i].key, key)) return (int)i;
    return -1;
}

void me_settings_set_console_map(me_settings *s, const char *key, int player,
                                 const me_control_map *m, const me_control_map *base) {
    if (!key || !*key) return;
    unsigned bits = changed_inputs(m, base);
    int i = me_settings_find_console_controls(s, key);
    if (i < 0) {
        if (!bits) return;
        struct me_console_controls *grown = (struct me_console_controls *)realloc(
            s->console_controls, (s->console_controls_n + 1) * sizeof(*grown));
        if (!grown) return;
        s->console_controls = grown;
        i = (int)s->console_controls_n++;
        memset(&grown[i], 0, sizeof(grown[i]));
        snprintf(grown[i].key, sizeof(grown[i].key), "%s", key);
    }
    struct me_console_controls *e = &s->console_controls[i];
    e->controls[player] = *m;
    e->overrides[player] = bits;
    for (int pl = 0; pl < ME_MAX_PLAYERS; pl++)
        if (e->overrides[pl]) return;
    memmove(e, e + 1, (s->console_controls_n - (size_t)i - 1) * sizeof(*e));
    s->console_controls_n--;
}

/* Generate a default settings.yaml file if it doesn't exist.
   Returns 0 on success, -1 on error. */
int me_settings_generate_default(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "[settings] cannot create %s\n", path);
        return -1;
    }

    if (fputs(k_settings_yaml_default, f) < 0) {
        fprintf(stderr, "[settings] write failed: %s\n", path);
        fclose(f);
        return -1;
    }
    fclose(f);
    printf("[settings] generated default %s\n", path);
    return 0;
}

/* Match a core_path argv against the settings.yaml cores: table. We accept
   either the short name ("snes9x") or any path ending in the configured dll
   filename, so the user can keep using full paths on the command line. */
int me_settings_find_core_index(const me_settings *s, const char *core_path) {
    if (!s || !core_path || !s->cores) return -1;
    /* Extract the bare filename from core_path. */
    const char *base = core_path;
    for (const char *p = core_path; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    for (size_t i = 0; i < s->cores_n; i++) {
        const struct me_core_entry *e = &s->cores[i];
        if (iequals(base, e->dll)) return (int)i;
        if (iequals(core_path, e->name)) return (int)i;
    }
    return -1;
}

const struct me_core_entry *me_settings_find_core(const me_settings *s,
                                                  const char *core_path) {
    int i = me_settings_find_core_index(s, core_path);
    return i >= 0 ? &s->cores[i] : NULL;
}

static int console_core_index(const me_settings *s, const char *console) {
    for (int i = 0; i < s->console_cores_n; i++)
        if (iequals(s->console_cores[i].console, console)) return i;
    return -1;
}

const char *me_settings_console_core(const me_settings *s, const char *console) {
    int i = console_core_index(s, console);
    return i >= 0 ? s->console_cores[i].dll : NULL;
}

void me_settings_set_console_core(me_settings *s, const char *console, const char *dll) {
    int i = console_core_index(s, console);
    if (!dll || !*dll) {
        if (i < 0) return;
        memmove(&s->console_cores[i], &s->console_cores[i + 1],
                (size_t)(s->console_cores_n - i - 1) * sizeof(s->console_cores[0]));
        s->console_cores_n--;
        return;
    }
    if (i < 0) {
        if (s->console_cores_n >= ME_CONSOLE_CORES_MAX) return;
        i = s->console_cores_n++;
        snprintf(s->console_cores[i].console, sizeof(s->console_cores[i].console), "%s", console);
    }
    snprintf(s->console_cores[i].dll, sizeof(s->console_cores[i].dll), "%s", dll);
}

static int console_pick_index(const me_settings *s, const char *rom) {
    for (int i = 0; i < s->console_picks_n; i++)
        if (iequals(s->console_picks[i].rom, rom)) return i;
    return -1;
}

const char *me_settings_console_pick(const me_settings *s, const char *rom) {
    int i = console_pick_index(s, rom);
    return i >= 0 ? s->console_picks[i].console : NULL;
}

void me_settings_set_console_pick(me_settings *s, const char *rom, const char *console) {
    if (!rom || !*rom) return;
    int i = console_pick_index(s, rom);
    if (i >= 0) {   /* take it out; it goes back in at the front */
        memmove(&s->console_picks[i], &s->console_picks[i + 1],
                (size_t)(s->console_picks_n - i - 1) * sizeof(s->console_picks[0]));
        s->console_picks_n--;
    }
    if (!console || !*console) return;
    if (s->console_picks_n >= ME_CONSOLE_PICKS_MAX) s->console_picks_n = ME_CONSOLE_PICKS_MAX - 1;
    memmove(&s->console_picks[1], &s->console_picks[0],
            (size_t)s->console_picks_n * sizeof(s->console_picks[0]));
    s->console_picks_n++;
    snprintf(s->console_picks[0].rom, sizeof(s->console_picks[0].rom), "%s", rom);
    snprintf(s->console_picks[0].console, sizeof(s->console_picks[0].console), "%s", console);
}

static int console_adapter_index(const me_settings *s, const char *console) {
    for (int i = 0; i < s->console_adapters_n; i++)
        if (iequals(s->console_adapters[i].console, console)) return i;
    return -1;
}

const char *me_settings_console_adapter(const me_settings *s, const char *console) {
    int i = console_adapter_index(s, console);
    return i >= 0 ? s->console_adapters[i].adapter : NULL;
}

void me_settings_set_console_adapter(me_settings *s, const char *console, const char *adapter) {
    int i = console_adapter_index(s, console);
    if (!adapter || !*adapter) {
        if (i < 0) return;
        memmove(&s->console_adapters[i], &s->console_adapters[i + 1],
                (size_t)(s->console_adapters_n - i - 1) * sizeof(s->console_adapters[0]));
        s->console_adapters_n--;
        return;
    }
    if (i < 0) {
        if (s->console_adapters_n >= ME_CONSOLE_CORES_MAX) return;
        i = s->console_adapters_n++;
        snprintf(s->console_adapters[i].console, sizeof(s->console_adapters[i].console), "%s", console);
    }
    snprintf(s->console_adapters[i].adapter, sizeof(s->console_adapters[i].adapter), "%s", adapter);
}
