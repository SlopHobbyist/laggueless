#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <yaml.h>
#include "settings.h"

/* ---- key-name → VK table -------------------------------------------------- */
typedef struct { const char *name; unsigned vk; } me_keymap;

/* Names are matched case-insensitively. Order doesn't matter; linear scan. */
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
    { "F1",  VK_F1  }, { "F2",  VK_F2  }, { "F3",  VK_F3  }, { "F4",  VK_F4  },
    { "F5",  VK_F5  }, { "F6",  VK_F6  }, { "F7",  VK_F7  }, { "F8",  VK_F8  },
    { "F9",  VK_F9  }, { "F10", VK_F10 }, { "F11", VK_F11 }, { "F12", VK_F12 },
    { NULL, 0 }
};

static int iequals(const char *a, const char *b) {
    return _stricmp(a, b) == 0;
}

/* Parse a single token like "X", "F11", "Up", "RShift". Returns VK or 0. */
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
    return 0;
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

/* ---- input-name → me_input_id -------------------------------------------- */
typedef struct { const char *name; me_input_id id; } me_input_name;
static const me_input_name g_input_names[] = {
    { "dpad_up",      ME_IN_DPAD_UP      },
    { "dpad_down",    ME_IN_DPAD_DOWN    },
    { "dpad_left",    ME_IN_DPAD_LEFT    },
    { "dpad_right",   ME_IN_DPAD_RIGHT   },
    { "a",            ME_IN_A            },
    { "b",            ME_IN_B            },
    { "x",            ME_IN_X            },
    { "y",            ME_IN_Y            },
    { "lb",           ME_IN_LB           },
    { "rb",           ME_IN_RB           },
    { "lt",           ME_IN_LT           },
    { "rt",           ME_IN_RT           },
    { "lstick",       ME_IN_LSTICK       },
    { "rstick",       ME_IN_RSTICK       },
    { "start",        ME_IN_START        },
    { "back",         ME_IN_BACK         },
    { "lstick_up",    ME_IN_LSTICK_UP    },
    { "lstick_down",  ME_IN_LSTICK_DOWN  },
    { "lstick_left",  ME_IN_LSTICK_LEFT  },
    { "lstick_right", ME_IN_LSTICK_RIGHT },
    { "rstick_up",    ME_IN_RSTICK_UP    },
    { "rstick_down",  ME_IN_RSTICK_DOWN  },
    { "rstick_left",  ME_IN_RSTICK_LEFT  },
    { "rstick_right", ME_IN_RSTICK_RIGHT },
    { NULL, 0 }
};

static int input_id_from_name(const char *name, me_input_id *out) {
    if (!name) return 0;
    for (const me_input_name *n = g_input_names; n->name; n++) {
        if (iequals(name, n->name)) { *out = n->id; return 1; }
    }
    return 0;
}

/* ---- defaults ------------------------------------------------------------- */
void me_settings_defaults(me_settings *out) {
    memset(out, 0, sizeof(*out));
    out->aspect = ME_ASPECT_1_1;

    /* LSFG defaults: 2x multiplier, full-resolution optical flow, quality mode.
     * These match the "best quality, minimum latency cost" preset. */
    out->lsfg_multiplier = 2;
    out->lsfg_flow_scale = 1.0f;
    out->lsfg_perf_mode  = 0;

    out->hk_cycle_aspect      = parse_chord("F1");
    out->hk_toggle_fullscreen = parse_chord("F11");
    out->hk_exit_fullscreen   = parse_chord("Escape");
    out->hk_quit              = parse_chord("Alt+F4");
    out->hk_reset             = parse_chord("Ctrl+R");

    /* Universal defaults match what main.c used to hard-code in step 5. */
    out->universal.keys[ME_IN_DPAD_UP]    = parse_chord("Up");
    out->universal.keys[ME_IN_DPAD_DOWN]  = parse_chord("Down");
    out->universal.keys[ME_IN_DPAD_LEFT]  = parse_chord("Left");
    out->universal.keys[ME_IN_DPAD_RIGHT] = parse_chord("Right");
    out->universal.keys[ME_IN_A]          = parse_chord("X");
    out->universal.keys[ME_IN_B]          = parse_chord("Z");
    out->universal.keys[ME_IN_X]          = parse_chord("S");
    out->universal.keys[ME_IN_Y]          = parse_chord("A");
    out->universal.keys[ME_IN_LB]         = parse_chord("Q");
    out->universal.keys[ME_IN_RB]         = parse_chord("W");
    out->universal.keys[ME_IN_START]      = parse_chord("Return");
    out->universal.keys[ME_IN_BACK]       = parse_chord("RShift");
}

void me_settings_free(me_settings *s) {
    if (!s) return;
    free(s->cores);
    s->cores = NULL;
    s->cores_n = 0;
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

static me_aspect_mode parse_aspect(const char *s, me_aspect_mode defv) {
    if (!s) return defv;
    if (strcmp(s, "1:1") == 0)  return ME_ASPECT_1_1;
    if (strcmp(s, "4:3") == 0)  return ME_ASPECT_4_3;
    if (strcmp(s, "16:9") == 0) return ME_ASPECT_16_9;
    return defv;
}

/* Walk a `player1:` block: each child key is an input name (a, b, dpad_up,
   lstick_up, ...). Each value is itself a mapping with a `keyboard:` scalar. */
static void parse_player1(yaml_document_t *d, yaml_node_t *p1, me_control_map *out) {
    if (!p1 || p1->type != YAML_MAPPING_NODE) return;
    for (yaml_node_pair_t *p = p1->data.mapping.pairs.start;
         p < p1->data.mapping.pairs.top; p++) {
        yaml_node_t *k = doc_get(d, p->key);
        if (!k || k->type != YAML_SCALAR_NODE) continue;
        me_input_id id;
        if (!input_id_from_name((const char *)k->data.scalar.value, &id)) continue;
        yaml_node_t *v = doc_get(d, p->value);
        if (!v || v->type != YAML_MAPPING_NODE) continue;
        const char *kb = scalar_str(map_get(d, v, "keyboard"));
        if (kb && *kb) out->keys[id] = parse_chord(kb);
    }
}

static void parse_hotkey(yaml_document_t *d, yaml_node_t *root, const char *key,
                         me_kb_binding *out) {
    yaml_node_t *m = map_get(d, root, key);
    if (!m || m->type != YAML_MAPPING_NODE) return;
    const char *kb = scalar_str(map_get(d, m, "keyboard"));
    if (kb && *kb) *out = parse_chord(kb);
}

int me_settings_load(const char *path, me_settings *out) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[settings] %s not found; using defaults\n", path);
        return 1;
    }

    yaml_parser_t parser;
    yaml_document_t doc;
    if (!yaml_parser_initialize(&parser)) {
        fclose(f);
        fprintf(stderr, "[settings] yaml_parser_initialize failed\n");
        return -1;
    }
    yaml_parser_set_input_file(&parser, f);
    if (!yaml_parser_load(&parser, &doc)) {
        fprintf(stderr, "[settings] parse error in %s at line %zu col %zu: %s\n",
                path, parser.problem_mark.line + 1, parser.problem_mark.column + 1,
                parser.problem ? parser.problem : "(unknown)");
        yaml_parser_delete(&parser);
        fclose(f);
        return -1;
    }
    yaml_parser_delete(&parser);
    fclose(f);

    yaml_node_t *root = yaml_document_get_root_node(&doc);
    if (!root || root->type != YAML_MAPPING_NODE) {
        fprintf(stderr, "[settings] %s: root is not a mapping\n", path);
        yaml_document_delete(&doc);
        return -1;
    }

    /* video */
    yaml_node_t *video = map_get(&doc, root, "video");
    if (video) {
        out->fullscreen_on_launch = scalar_bool(map_get(&doc, video, "fullscreen_on_launch"),
                                                out->fullscreen_on_launch);
        out->aspect      = parse_aspect(scalar_str(map_get(&doc, video, "aspect")), out->aspect);
        out->force_gdi   = scalar_bool(map_get(&doc, video, "force_gdi"),   out->force_gdi);
        out->force_d3d11 = scalar_bool(map_get(&doc, video, "force_d3d11"), out->force_d3d11);
        out->force_vulkan = scalar_bool(map_get(&doc, video, "force_vulkan"), out->force_vulkan);
        out->vk_no_vsync = scalar_bool(map_get(&doc, video, "vk_no_vsync"), out->vk_no_vsync);
        out->vk_mailbox  = scalar_bool(map_get(&doc, video, "vk_mailbox"),  out->vk_mailbox);
        out->vk_validate = scalar_bool(map_get(&doc, video, "vk_validate"), out->vk_validate);
        out->match_display_hz = scalar_bool(map_get(&doc, video, "match_display_hz"),
                                            out->match_display_hz);
    }

    /* lsfg */
    yaml_node_t *lsfg = map_get(&doc, root, "lsfg");
    if (lsfg) {
        out->lsfg_multiplier = scalar_int  (map_get(&doc, lsfg, "multiplier"),  out->lsfg_multiplier);
        out->lsfg_flow_scale = scalar_float(map_get(&doc, lsfg, "flow_scale"),  out->lsfg_flow_scale);
        out->lsfg_perf_mode  = scalar_bool (map_get(&doc, lsfg, "performance"), out->lsfg_perf_mode);
    }

    /* audio */
    yaml_node_t *audio = map_get(&doc, root, "audio");
    if (audio) {
        out->no_audio        = scalar_bool(map_get(&doc, audio, "no_audio"),       out->no_audio);
    }

    /* system */
    yaml_node_t *sys = map_get(&doc, root, "system");
    if (sys) {
        out->thread_affinity = scalar_bool(map_get(&doc, sys, "thread_affinity"), out->thread_affinity);
    }

    /* run-ahead */
    yaml_node_t *ra = map_get(&doc, root, "runahead");
    if (ra) {
        const char *s = scalar_str(map_get(&doc, ra, "frames"));
        if (s) {
            int n = atoi(s);
            if (n < 0) n = 0;
            if (n > 10) n = 10;
            out->runahead_frames = n;
        }
    }

    /* log */
    yaml_node_t *logn = map_get(&doc, root, "log");
    if (logn) {
        out->pace_log   = scalar_bool(map_get(&doc, logn, "pace_log"),   out->pace_log);
        out->timing_log = scalar_bool(map_get(&doc, logn, "timing_log"), out->timing_log);
        out->latency_log = scalar_bool(map_get(&doc, logn, "latency_log"), out->latency_log);
        out->env_trace  = scalar_bool(map_get(&doc, logn, "env_trace"),  out->env_trace);
    }

    /* hotkeys */
    yaml_node_t *hk = map_get(&doc, root, "hotkeys");
    if (hk) {
        parse_hotkey(&doc, hk, "cycle_aspect_ratio", &out->hk_cycle_aspect);
        parse_hotkey(&doc, hk, "toggle_fullscreen",  &out->hk_toggle_fullscreen);
        parse_hotkey(&doc, hk, "exit_fullscreen",    &out->hk_exit_fullscreen);
        parse_hotkey(&doc, hk, "quit",               &out->hk_quit);
        parse_hotkey(&doc, hk, "reset",              &out->hk_reset);
    }

    /* controls: universal */
    yaml_node_t *controls = map_get(&doc, root, "controls");
    if (controls) {
        yaml_node_t *uni = map_get(&doc, controls, "universal");
        if (uni) parse_player1(&doc, map_get(&doc, uni, "player1"), &out->universal);
    }

    /* cores */
    yaml_node_t *cores = map_get(&doc, root, "cores");
    if (cores && cores->type == YAML_MAPPING_NODE) {
        size_t n = (size_t)(cores->data.mapping.pairs.top - cores->data.mapping.pairs.start);
        out->cores = (struct me_core_entry *)calloc(n, sizeof(*out->cores));
        out->cores_n = 0;
        if (out->cores) {
            for (yaml_node_pair_t *p = cores->data.mapping.pairs.start;
                 p < cores->data.mapping.pairs.top; p++) {
                yaml_node_t *k = doc_get(&doc, p->key);
                yaml_node_t *v = doc_get(&doc, p->value);
                if (!k || k->type != YAML_SCALAR_NODE || !v || v->type != YAML_MAPPING_NODE) continue;
                struct me_core_entry *e = &out->cores[out->cores_n++];
                strncpy(e->name, (const char *)k->data.scalar.value, sizeof(e->name) - 1);
                /* Inherit universal as baseline, then per-core overrides apply. */
                e->controls = out->universal;
                e->use_universal = 1;

                const char *dll = scalar_str(map_get(&doc, v, "dll"));
                if (dll) {
                    strncpy(e->dll, dll, sizeof(e->dll) - 1);
                }
                e->use_universal = scalar_bool(map_get(&doc, v, "use_universal"), 1);
                yaml_node_t *cc = map_get(&doc, v, "controls");
                if (cc) parse_player1(&doc, map_get(&doc, cc, "player1"), &e->controls);
            }
        }
    }

    yaml_document_delete(&doc);
    return 0;
}

/* Generate a default settings.yaml file if it doesn't exist.
   Returns 0 on success, -1 on error. */
int me_settings_generate_default(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "[settings] cannot create %s\n", path);
        return -1;
    }

    const char *content =
        "## laggueless settings\n"
        "\n"
        "video:\n"
        "  fullscreen_on_launch: false\n"
        "  aspect: 1:1            # 1:1 | 4:3 | 16:9\n"
        "  force_gdi: false       # force GDI for all cores (overrides force_d3d11)\n"
        "  force_d3d11: false     # use D3D11 present path for 2D cores too\n"
        "  force_vulkan: true     # use the Vulkan present path\n"
        "  vk_no_vsync: false     # (Vulkan only) IMMEDIATE present mode\n"
        "  vk_mailbox: false      # (Vulkan only) prefer MAILBOX present mode\n"
        "  vk_validate: false     # (Vulkan only) enable VK_LAYER_KHRONOS_validation\n"
        "  match_display_hz: false # snap pacing to monitor refresh when within ~5%%\n"
        "\n"
        "audio:\n"
        "  no_audio: false        # disable audio output\n"
        "\n"
        "lsfg:\n"
        "  multiplier: 2          # 2 / 3 / 4 — frames per real frame\n"
        "  flow_scale: 1.0        # 0.25..1.0 — optical-flow resolution scale\n"
        "  performance: false     # reduced quality / lower GPU cost\n"
        "\n"
        "system:\n"
        "  thread_affinity: true  # pin emu/audio threads to isolated cores\n"
        "\n"
        "runahead:\n"
        "  frames: 1              # 0 disables; 1-10 recommended\n"
        "\n"
        "log:\n"
        "  pace_log: false        # log audio pacing diagnostics\n"
        "  timing_log: false      # log frame timing diagnostics\n"
        "  latency_log: false     # log per-stage latency breakdown\n"
        "  env_trace: false       # log libretro environment calls\n"
        "\n"
        "hotkeys:\n"
        "  cycle_aspect_ratio:\n"
        "    keyboard:   F1\n"
        "  toggle_fullscreen:\n"
        "    keyboard:   F11\n"
        "  quit:\n"
        "    keyboard:   Alt+F4\n"
        "  reset:\n"
        "    keyboard:   Ctrl+R\n"
        "\n"
        "controls:\n"
        "  universal:\n"
        "    player1:\n"
        "      dpad_up:\n"
        "        keyboard:   Up\n"
        "      dpad_down:\n"
        "        keyboard:   Down\n"
        "      dpad_left:\n"
        "        keyboard:   Left\n"
        "      dpad_right:\n"
        "        keyboard:   Right\n"
        "      a:\n"
        "        keyboard:   X\n"
        "      b:\n"
        "        keyboard:   Z\n"
        "      x:\n"
        "        keyboard:   S\n"
        "      y:\n"
        "        keyboard:   A\n"
        "      lb:\n"
        "        keyboard:   Q\n"
        "      rb:\n"
        "        keyboard:   W\n"
        "      start:\n"
        "        keyboard:   Return\n"
        "      back:\n"
        "        keyboard:   RShift\n"
        "\n"
        "cores:\n"
        "  snes9x:\n"
        "    dll: snes9x_libretro.dll\n"
        "    use_universal: true\n"
        "  mesen:\n"
        "    dll: mesen_libretro.dll\n"
        "    use_universal: true\n"
        "  mupen64plus_next:\n"
        "    dll: mupen64plus_next_libretro.dll\n"
        "    use_universal: false\n"
        "    controls:\n"
        "      player1:\n"
        "        lstick_up:\n"
        "          keyboard:   Up\n"
        "        lstick_down:\n"
        "          keyboard:   Down\n"
        "        lstick_left:\n"
        "          keyboard:   Left\n"
        "        lstick_right:\n"
        "          keyboard:   Right\n"
        "        a:\n"
        "          keyboard:   X\n"
        "        b:\n"
        "          keyboard:   Z\n"
        "        lb:\n"
        "          keyboard:   Q\n"
        "        rb:\n"
        "          keyboard:   W\n"
        "        lt:\n"
        "          keyboard:   Space\n"
        "        rstick_up:\n"
        "          keyboard:   I\n"
        "        rstick_down:\n"
        "          keyboard:   K\n"
        "        rstick_left:\n"
        "          keyboard:   J\n"
        "        rstick_right:\n"
        "          keyboard:   L\n"
        "        start:\n"
        "          keyboard:   Return\n";

    if (fputs(content, f) < 0) {
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
const struct me_core_entry *me_settings_find_core(const me_settings *s,
                                                  const char *core_path) {
    if (!s || !core_path || !s->cores) return NULL;
    /* Extract the bare filename from core_path. */
    const char *base = core_path;
    for (const char *p = core_path; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    for (size_t i = 0; i < s->cores_n; i++) {
        const struct me_core_entry *e = &s->cores[i];
        if (iequals(base, e->dll)) return e;
        if (iequals(core_path, e->name)) return e;
    }
    return NULL;
}
