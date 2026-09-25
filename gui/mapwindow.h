#pragma once

#include <atomic>
#include <inttypes.h>
#include "popup.h"

/*
 * Map Keys window: per-ROM assignment of TrimUI buttons / D-pad
 * directions to Vector-06C keys, opened from the MAIN MENU
 * "Map Keys" item.
 *
 *   MAIN_MENU --A on Map Keys--> MAP_KEYS   (menu closes, still paused)
 *   MAP_KEYS  --B/START-------> MAIN_MENU   (focus back on Map Keys;
 *                                            on close the .key is
 *                                            saved next to the ROM)
 *
 * The window lists every mapping source with its CURRENT effective
 * assignment and origin tag (DEFAULT / ROM / DISABLED). The VKBD
 * serves as the key picker and is visible only while the assignment
 * mode is active: A on a source enters it ("PRESS VECTOR KEY"), the
 * next VKBD key press becomes the assignment (handled by the caller
 * through the VKBD sink), B cancels. X writes an explicit NONE
 * (disabled) entry. START/SELECT are system buttons and never
 * appear as sources.
 *
 * The mapping data itself lives in the KeyMap module (keymap.h):
 * this window only edits it and shows the state. The popup paint /
 * repaint handshake is inherited from the PSP port and harmless in
 * the single-threaded TrimUI UI loop.
 */

/* Normalized pad state passed to MapWindow::update(): which
 * buttons are currently held. Keeps mapwindow free of SDL. */
enum {
    MK_PAD_UP   = 0x01,
    MK_PAD_DOWN = 0x02,
};

class MapWindow : public Popup
{
public:
    /* Window size, UI coordinate space (480x272); centered by
     * the renderer, leaving 16 px above and below. */
    static const int PANEL_W = 480;
    static const int PANEL_H = 240;

    /* Layout constants. */
    static const int PAD_X = 8;        /* window left/right padding */
    static const int PAD_Y = 8;        /* window top/bottom padding */
    static const int TITLE_H = 16;     /* header row, 8x8 font at 2x */
    static const int ROW_H = 19;       /* one mapping row */
    static const int VISIBLE_ROWS = 9; /* rows fitting the list area */
    static const int FOOTER_H = 16;    /* bottom hint row */

    MapWindow();

    /* Atomic: written and read on the UI thread. */
    bool is_open() const override { return this->open_flag.load(std::memory_order_acquire); }

    /* Open the window for the ROM named rom_label
     * ("PUTUP.ROM"; "BOOT LOADER" when no ROM is loaded - editing
     * still works, only the .key save is skipped). */
    void open(const char * rom_label);
    /* MAP_KEYS -> MAIN_MENU. */
    void close();

    /* One input step; called on the UI thread (~50 Hz) while
     * open and NOT in the assignment mode (during the assignment
     * the caller feeds the pad to the VKBD instead, which is the
     * only time the VKBD is visible here). UP/DOWN move the source
     * selection cyclically, firing on the keyup edge. */
    void update(unsigned pad);

    /* Assignment mode: A on the selected source starts it, the
     * next VKBD key press ends it, B cancels. */
    bool is_waiting() const { return this->wait_src >= 0; }
    int waiting_src() const { return this->wait_src; }
    void start_assign();
    void cancel_assign();
    /* The VKBD sink reports the picked key here. */
    void assign_selected(int scancode);

    /* X: explicit NONE entry for the selected source; the default
     * does NOT return on its own afterwards. */
    void disable_selected();

    /* Rasterize the window into the popup texture; a state change
     * arriving mid-paint forces one more pass. */
    void paint() override;

private:
    std::atomic<bool> open_flag;

    int selected;       /* highlighted source row */
    int top;            /* first visible row (scrolling) */
    int wait_src;       /* source waiting for a VKBD key, -1 none */
    char rom_label[64]; /* shown in the header */
};
