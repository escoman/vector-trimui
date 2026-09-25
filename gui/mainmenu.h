#pragma once

#include <atomic>
#include <inttypes.h>
#include "popup.h"

/*
 * MAIN MENU: the PSP UI layer above the emulated Vector-06C.
 *
 * UI state machine of this port (independent of the Board state):
 *
 *     enum class UIState { GAME, MAIN_MENU, ROM_BROWSER };
 *
 *   GAME       - the machine runs, PSP pad feeds the Vector / VKBD.
 *   MAIN_MENU  - the machine is frozen via the Emulator pause flag,
 *                the last picture stays on screen, the menu is drawn
 *                above it (translucent backdrop + panel).
 *   ROM_BROWSER - ROM selection window opened from the Load ROM
 *                item (rombrowser.h); the machine stays frozen the
 *                whole time, the same backdrop scheme.
 *   CONFIG      - config.ini editing window opened from the Config
 *                item (configwindow.h); same frozen + backdrop
 *                scheme.
 *   STATE_BROWSER - save/load state grid opened from the Save State
 *                / Load State items (statewindow.h); same frozen +
 *                backdrop scheme.
 *   GAME_CENTER - online ROM catalog (gamecenter.h); same frozen +
 *                backdrop scheme.
 *
 * Threading, same split as the VKBD; the texture / repaint
 * machinery is shared through the Popup base class:
 *
 *   Worker thread: open()/close()/update() mutate the state; a
 *   visual change (selection) marks the popup dirty.
 *
 *   Main/display thread: needs_repaint()/paint() rasterize the
 *   panel; TV::render() presents it as an overlay quad in the
 *   480x272 UI coordinate space (never scaled with the Vector
 *   picture, never drawn into the Vector framebuffer).
 *
 * Stage 2: X on the Load ROM item opens the ROM Browser (handled by
 * the caller); Stage 3: X on the Config item opens the Config
 * window; the other items stay visual only.
 */

enum class UIState : int {
    GAME = 0,
    MAIN_MENU,
    ROM_BROWSER,
    STATE_BROWSER
};

/* Normalized pad state passed to MainMenu::update(): which buttons
 * are currently held. Keeps mainmenu free of pspctrl.h. */
enum {
    MENU_PAD_UP    = 0x01,
    MENU_PAD_DOWN  = 0x02,
    MENU_PAD_PRESS = 0x04, /* X: activate the item (caller handles) */
};

class MainMenu : public Popup
{
public:
    /* Item indices; Load ROM, Save Preview, Save/Load State
     * and Exit have actions, the rest stay visual at this stage. */
    static const int ITEM_LOAD_ROM = 0;
    static const int ITEM_SAVE_PREVIEW = 1;
    static const int ITEM_SAVE_STATE = 2;
    static const int ITEM_LOAD_STATE = 3;
    static const int ITEM_MAP_KEYS = 4;
    static const int ITEM_GAME_CENTER = 5;
    static const int ITEM_ABOUT = 6;
    static const int ITEM_EXIT = 7;

    static const int ITEM_COUNT = 8;

    /* Layout constants, PSP UI coordinate space (480x272). */
    static const int ITEM_W = 224;    /* item button width */
    static const int ITEM_H = 20;     /* item button height */
    static const int ITEM_GAP = 3;    /* gap between item buttons */
    static const int PAD_X = 16;      /* panel left/right padding */
    static const int PAD_Y = 12;      /* panel top/bottom padding */
    static const int TITLE_H = 16;    /* 8x8 font rasterized at 2x */
    static const int TITLE_GAP = 8;   /* gap between title and items */

    MainMenu();

    /* UI state. is_open() is the single source of truth both threads
     * look at (atomic: written by the worker, read by the display
     * thread). */
    bool is_open() const override { return this->open_flag.load(std::memory_order_acquire); }
    UIState ui_state() const
    {
        return is_open() ? UIState::MAIN_MENU : UIState::GAME;
    }

    /* Worker thread: GAME -> MAIN_MENU. focus_item selects the
     * initially highlighted item (default the first one; the Config
     * window returns here with focus back on Config). */
    void open(int focus_item = 0);
    /* Worker thread: MAIN_MENU -> GAME. */
    void close();

    /* One input step; called by the worker thread (~50 Hz) while
     * open. D-pad navigates cyclically, firing on the keyup edge
     * (one press = one step); X edges are handled by the caller
     * (Load ROM opens the ROM Browser, Exit shuts down).
     * START/O are handled by the caller too (close). */
    void update(unsigned pad);

    int selected_item() const { return this->selected; }
    static const char * item_label(int i);

    /* Worker thread: transient status shown in place of the title
     * while a slow action runs ("Saving..." during the preview
     * write on real hardware); empty string restores the title.
     * Cleared on the next open(). */
    void set_status(const char * msg);

    /* Rasterize the panel into the popup texture. Main thread only;
     * a selection change arriving from the worker while painting
     * forces one more pass. */
    void paint() override;

private:
    std::atomic<bool> open_flag;
    int selected;           /* worker thread only */
    char message[32];       /* transient status line, empty = none */

    static const char * const items[ITEM_COUNT];
};
