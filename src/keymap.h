#pragma once

#include <string>
#include "SDL.h"

/*
 * TrimUI -> Vector-06C key mapping ("Map Keys").
 *
 * Two levels, exactly like the PSP port:
 *
 *   Default Mapping  - the built-in table below, the single source
 *                      of truth for the factory assignments; it
 *                      works with no .key file at all and is never
 *                      modified by this module.
 *   ROM mapping      - an optional <rom>.key text file next to the
 *                      ROM; only the entries DIFFERENT from the
 *                      defaults are stored there (overrides and
 *                      explicit NONE disables).
 *
 *   Default Mapping -> <ROM>.key -> Effective Mapping
 *
 * Every source has one of three states:
 *   STATE_DEFAULT  - no entry in the .key, the default key applies;
 *   STATE_OVERRIDE - <SRC>=<KEY> in the .key;
 *   STATE_DISABLED - <SRC>=NONE in the .key (the button does
 *                    nothing for this ROM; the default must NOT
 *                    return until the entry is removed).
 *
 * The Vector key identifier is the same SDL scancode used by the
 * VKBD and the engine key queue - no second key table exists.
 * START and SELECT (BACK) are system buttons (menu / VKBD) and are
 * never part of the mapping. All functions run in the UI thread
 * only.
 */

/* Mapping sources: the D-pad, the four face buttons and the two
 * shoulder buttons. The Brick Pro has no analog stick; L2/R2 are
 * analog triggers and are not digital sources. */
enum {
    MAP_SRC_UP = 0,
    MAP_SRC_DOWN,
    MAP_SRC_LEFT,
    MAP_SRC_RIGHT,
    MAP_SRC_A,
    MAP_SRC_B,
    MAP_SRC_X,
    MAP_SRC_Y,
    MAP_SRC_L1,
    MAP_SRC_R1,
    MAP_SRC_COUNT
};

namespace KeyMap
{
    enum EntryState {
        STATE_DEFAULT = 0,
        STATE_OVERRIDE,
        STATE_DISABLED,
    };

    /* Back to the pure Default Mapping; the ROM-specific entries are
     * forgotten. First step of every ROM load. */
    void reset_to_default();

    /* The built-in assignment of a source (-1: the default itself
     * has none). */
    int default_key(int src);

    /* The assignment in effect right now (-1: nothing happens when
     * the source is pressed). */
    int effective_key(int src);

    EntryState entry_state(int src);

    /* ROM-specific change; the effective table updates at once. */
    void assign(int src, int scancode);
    void disable(int src);

    /* True when at least one source differs from the default
     * (override or disabled): a .key is worth writing. */
    bool has_custom();
    int custom_count();

    /* Parse <rom>.key over the current (default) table; unknown and
     * malformed lines are ignored, never fatal. True when the file
     * existed. */
    bool load_file(const std::string & path);

    /* Write only the entries differing from the default; when there
     * are none, delete the file instead (the ROM is fully default
     * again). True when a file was written. */
    bool save_or_cleanup(const std::string & path);

    /* Names. source_label: short human label for the window list
     * ("DPAD UP", "BUTTON A"); source_file_id: the .key key
     * ("PAD_UP", "BTN_A"); key_label: short display name of a
     * Vector key ("ENTER", "---" when none); key_file_name: the
     * .key value token ("ENTER", "NONE" is handled separately). */
    const char * source_label(int src);
    const char * source_file_id(int src);
    const char * key_label(int scancode);
    const char * key_file_name(int scancode);
    int key_by_file_name(const char * name);   /* -1 when unknown */
    bool is_assignable(int scancode);
}
