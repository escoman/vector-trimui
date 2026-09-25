#include "keymap.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

/*
 * Registry of every assignable Vector key: the same SDL scancodes
 * the VKBD and Keyboard know (keyboard.h apply_key / vkbd.cpp
 * scancodes). file_name is the .key value token, label the short
 * name shown in the Map Keys list. Machine-level keys are
 * intentionally absent.
 */
struct key_entry_t {
    int scancode;
    const char * file_name;
    const char * label;
};

static const key_entry_t key_registry[] = {
    { SDL_SCANCODE_RETURN,        "ENTER",    "ENTER"     },
    { SDL_SCANCODE_SPACE,         "SPACE",    "SPACE"     },
    { SDL_SCANCODE_BACKSPACE,     "BACKSPACE","BACKSPACE" },
    { SDL_SCANCODE_TAB,           "TAB",      "TAB"       },
    { SDL_SCANCODE_UP,            "UP",       "UP"        },
    { SDL_SCANCODE_DOWN,          "DOWN",     "DOWN"      },
    { SDL_SCANCODE_LEFT,          "LEFT",     "LEFT"      },
    { SDL_SCANCODE_RIGHT,         "RIGHT",    "RIGHT"     },
    { SDL_SCANCODE_LSHIFT,        "SHIFT",    "SHIFT"     },
    { SDL_SCANCODE_RSHIFT,        "RSHIFT",   "RSHIFT"    },
    { SDL_SCANCODE_LCTRL,         "CTRL",     "CTRL"      },
    { SDL_SCANCODE_F6,            "RUSLAT",   "RUS/LAT"   },
    { SDL_SCANCODE_LALT,          "PS",       "PS"        },
    { SDL_SCANCODE_ESCAPE,        "AR2",      "AR2"       },
    { SDL_SCANCODE_F1,            "F1",       "F1"        },
    { SDL_SCANCODE_F2,            "F2",       "F2"        },
    { SDL_SCANCODE_F3,            "F3",       "F3"        },
    { SDL_SCANCODE_F4,            "F4",       "F4"        },
    { SDL_SCANCODE_F5,            "F5",       "F5"        },
    { SDL_SCANCODE_F7,            "F7",       "F7"        },
    { SDL_SCANCODE_F8,            "F8",       "F8"        },
    { SDL_SCANCODE_HOME,          "HOME",     "HOME"      },
    { SDL_SCANCODE_END,           "END",      "END"       },
    { SDL_SCANCODE_MINUS,         "MINUS",    "MINUS"     },
    { SDL_SCANCODE_EQUALS,        "EQUALS",   "EQUALS"    },
    { SDL_SCANCODE_SLASH,         "SLASH",    "SLASH"     },
    { SDL_SCANCODE_BACKSLASH,     "BSLASH",   "BSLASH"    },
    { SDL_SCANCODE_COMMA,         "COMMA",    "COMMA"     },
    { SDL_SCANCODE_PERIOD,        "PERIOD",   "PERIOD"    },
    { SDL_SCANCODE_SEMICOLON,     "SEMI",     "SEMI"      },
    { SDL_SCANCODE_APOSTROPHE,    "APOSTR",   "APOSTR"    },
    { SDL_SCANCODE_GRAVE,         "GRAVE",    "GRAVE"     },
    { SDL_SCANCODE_LEFTBRACKET,   "LBRACK",   "LBRACK"    },
    { SDL_SCANCODE_RIGHTBRACKET,  "RBRACK",   "RBRACK"    },
    { SDL_SCANCODE_0, "0", "0" }, { SDL_SCANCODE_1, "1", "1" },
    { SDL_SCANCODE_2, "2", "2" }, { SDL_SCANCODE_3, "3", "3" },
    { SDL_SCANCODE_4, "4", "4" }, { SDL_SCANCODE_5, "5", "5" },
    { SDL_SCANCODE_6, "6", "6" }, { SDL_SCANCODE_7, "7", "7" },
    { SDL_SCANCODE_8, "8", "8" }, { SDL_SCANCODE_9, "9", "9" },
    { SDL_SCANCODE_A, "A", "A" }, { SDL_SCANCODE_B, "B", "B" },
    { SDL_SCANCODE_C, "C", "C" }, { SDL_SCANCODE_D, "D", "D" },
    { SDL_SCANCODE_E, "E", "E" }, { SDL_SCANCODE_F, "F", "F" },
    { SDL_SCANCODE_G, "G", "G" }, { SDL_SCANCODE_H, "H", "H" },
    { SDL_SCANCODE_I, "I", "I" }, { SDL_SCANCODE_J, "J", "J" },
    { SDL_SCANCODE_K, "K", "K" }, { SDL_SCANCODE_L, "L", "L" },
    { SDL_SCANCODE_M, "M", "M" }, { SDL_SCANCODE_N, "N", "N" },
    { SDL_SCANCODE_O, "O", "O" }, { SDL_SCANCODE_P, "P", "P" },
    { SDL_SCANCODE_Q, "Q", "Q" }, { SDL_SCANCODE_R, "R", "R" },
    { SDL_SCANCODE_S, "S", "S" }, { SDL_SCANCODE_T, "T", "T" },
    { SDL_SCANCODE_U, "U", "U" }, { SDL_SCANCODE_V, "V", "V" },
    { SDL_SCANCODE_W, "W", "W" }, { SDL_SCANCODE_X, "X", "X" },
    { SDL_SCANCODE_Y, "Y", "Y" }, { SDL_SCANCODE_Z, "Z", "Z" },
};

/* Source names: the short label for the window list and the .key
 * key. Index == MAP_SRC_* order. */
static const char * const source_labels[MAP_SRC_COUNT] = {
    "DPAD UP", "DPAD DOWN", "DPAD LEFT", "DPAD RIGHT",
    "BUTTON A", "BUTTON B", "BUTTON X", "BUTTON Y",
    "L1", "R1",
};

static const char * const source_file_ids[MAP_SRC_COUNT] = {
    "PAD_UP", "PAD_DOWN", "PAD_LEFT", "PAD_RIGHT",
    "BTN_A", "BTN_B", "BTN_X", "BTN_Y",
    "BTN_L1", "BTN_R1",
};

/* The Default Mapping. The D-pad is the Vector keyboard arrows, X and Y
 * are the two game buttons (ПРОБЕЛ and ВК), the shoulders are РУС/ЛАТ and
 * СС in the PSP L/R spirit. A and B are left unassigned on purpose: on
 * TrimUI they confirm a menu item and go back, so binding them to a Vector
 * key would fire that key every time a window is closed with the button
 * still held. Never written to a .key on its own. */
static const int default_table[MAP_SRC_COUNT] = {
    SDL_SCANCODE_UP,                       /* MAP_SRC_UP */
    SDL_SCANCODE_DOWN,                     /* MAP_SRC_DOWN */
    SDL_SCANCODE_LEFT,                     /* MAP_SRC_LEFT */
    SDL_SCANCODE_RIGHT,                    /* MAP_SRC_RIGHT */
    -1,                                    /* MAP_SRC_A: menu confirm only */
    -1,                                    /* MAP_SRC_B: menu back only */
    SDL_SCANCODE_SPACE,                    /* MAP_SRC_X: Space (ПРОБЕЛ) */
    SDL_SCANCODE_RETURN,                   /* MAP_SRC_Y: Enter (ВК) */
    SDL_SCANCODE_F6,                       /* MAP_SRC_L1: РУС/ЛАТ */
    SDL_SCANCODE_LSHIFT,                   /* MAP_SRC_R1: Shift (СС) */
};

/* ROM-specific layer on top of the defaults: an override scancode
 * (or -1 when there is none) plus the explicit-disable flag.
 * -1-initialized explicitly: the BSS zero would read as a valid
 * override scancode 0 before the first reset_to_default() (the
 * boot loader phase), showing NULL entries for every source. */
static int override_key[MAP_SRC_COUNT] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
};
static bool disabled[MAP_SRC_COUNT];

namespace KeyMap
{

void reset_to_default()
{
    for (int i = 0; i < MAP_SRC_COUNT; ++i) {
        override_key[i] = -1;
        disabled[i] = false;
    }
}

int default_key(int src)
{
    return (src >= 0 && src < MAP_SRC_COUNT) ? default_table[src] : -1;
}

int effective_key(int src)
{
    if (src < 0 || src >= MAP_SRC_COUNT)
        return -1;
    if (disabled[src])
        return -1;
    if (override_key[src] >= 0)
        return override_key[src];
    return default_table[src];
}

EntryState entry_state(int src)
{
    if (src < 0 || src >= MAP_SRC_COUNT)
        return STATE_DEFAULT;
    if (disabled[src])
        return STATE_DISABLED;
    if (override_key[src] >= 0)
        return STATE_OVERRIDE;
    return STATE_DEFAULT;
}

void assign(int src, int scancode)
{
    if (src < 0 || src >= MAP_SRC_COUNT || !is_assignable(scancode))
        return;
    override_key[src] = scancode;
    disabled[src] = false;
}

void disable(int src)
{
    if (src < 0 || src >= MAP_SRC_COUNT)
        return;
    override_key[src] = -1;
    disabled[src] = true;
}

bool has_custom()
{
    for (int i = 0; i < MAP_SRC_COUNT; ++i) {
        if (disabled[i] || override_key[i] >= 0)
            return true;
    }
    return false;
}

int custom_count()
{
    int n = 0;
    for (int i = 0; i < MAP_SRC_COUNT; ++i) {
        if (disabled[i] || override_key[i] >= 0)
            ++n;
    }
    return n;
}

const char * source_label(int src)
{
    return (src >= 0 && src < MAP_SRC_COUNT)
        ? source_labels[src] : "?";
}

const char * source_file_id(int src)
{
    return (src >= 0 && src < MAP_SRC_COUNT)
        ? source_file_ids[src] : "?";
}

const char * key_label(int scancode)
{
    for (const key_entry_t & ke : key_registry) {
        if (ke.scancode == scancode)
            return ke.label;
    }
    return "---";
}

const char * key_file_name(int scancode)
{
    for (const key_entry_t & ke : key_registry) {
        if (ke.scancode == scancode)
            return ke.file_name;
    }
    return nullptr;
}

int key_by_file_name(const char * name)
{
    if (name == nullptr || name[0] == '\0')
        return -1;
    for (const key_entry_t & ke : key_registry) {
        if (strcasecmp(ke.file_name, name) == 0)
            return ke.scancode;
    }
    return -1;
}

bool is_assignable(int scancode)
{
    for (const key_entry_t & ke : key_registry) {
        if (ke.scancode == scancode)
            return true;
    }
    return false;
}

/* --- .key file ---------------------------------------------------- */

static char * trim(char * s)
{
    while (*s == ' ' || *s == '\t')
        ++s;
    char * end = s + strlen(s);
    while (end > s && (end[-1] == ' '  || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n'))
        *--end = '\0';
    return s;
}

static int source_by_file_id(const char * id)
{
    for (int i = 0; i < MAP_SRC_COUNT; ++i) {
        if (strcasecmp(source_file_ids[i], id) == 0)
            return i;
    }
    return -1;
}

bool load_file(const std::string & path)
{
    FILE * f = fopen(path.c_str(), "r");
    if (f == nullptr)
        return false;

    char line[128];
    while (fgets(line, sizeof(line), f) != nullptr) {
        char * s = trim(line);
        if (s[0] == '\0' || s[0] == '#')
            continue;                       /* comment / blank */
        if (strncasecmp(s, "version=", 8) == 0)
            continue;                       /* format version: noted,
                                             * nothing to apply */
        char * eq = strchr(s, '=');
        if (eq == nullptr)
            continue;                       /* malformed: ignore */
        *eq = '\0';
        const char * id = trim(s);
        const char * value = trim(eq + 1);

        const int src = source_by_file_id(id);
        if (src < 0)
            continue;                       /* unknown source: ignore */
        if (strcasecmp(value, "NONE") == 0) {
            disable(src);
            continue;
        }
        const int key = key_by_file_name(value);
        if (key < 0)
            continue;                       /* unknown key: ignore */
        assign(src, key);
    }

    fclose(f);
    return true;
}

bool save_or_cleanup(const std::string & path)
{
    if (!has_custom()) {
        /* Fully default again: the .key (if any) would only repeat
         * the defaults, remove it. */
        remove(path.c_str());
        return false;
    }

    FILE * f = fopen(path.c_str(), "w");
    if (f == nullptr) {
        printf("KeyMap: cannot write %s\n", path.c_str());
        return false;
    }

    fprintf(f, "# Vector-06C TrimUI key mapping\n");
    fprintf(f, "version=1\n");
    fprintf(f, "\n");
    for (int i = 0; i < MAP_SRC_COUNT; ++i) {
        if (disabled[i]) {
            fprintf(f, "%s=NONE\n", source_file_ids[i]);
        } else if (override_key[i] >= 0) {
            fprintf(f, "%s=%s\n", source_file_ids[i],
                    key_file_name(override_key[i]));
        }
    }

    fclose(f);
    return true;
}

} /* namespace KeyMap */
