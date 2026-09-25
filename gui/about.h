#pragma once

#include <atomic>
#include <inttypes.h>
#include "popup.h"

/*
 * ABOUT: simple information window showing the application logo,
 * version, developer info and website.
 *
 * Opened from MAIN MENU item "About". The machine stays frozen via
 * the Emulator pause flag; the window is drawn above the dim overlay.
 *
 * Layout: logo on the left (128x128), text info on the right.
 * Close with CIRCLE or START.
 */

class AboutWindow : public Popup
{
public:
    /* Window size, PSP UI coordinate space (480x272). */
    static const int PANEL_W = 428;
    static const int PANEL_H = 200;

    /* Layout constants. */
    static const int PAD_X = 12;
    static const int PAD_Y = 24;
    static const int TITLE_H = 16;
    static const int LOGO_SIZE = 128;
    static const int TEXT_X = PAD_X + LOGO_SIZE + 32;
    static const int TEXT_W = PANEL_W - TEXT_X - PAD_X;

    /* Logo texture: power-of-two GE dimensions. */
    static const int LOGO_TEX_W = 128;
    static const int LOGO_TEX_H = 128;

    AboutWindow();

    bool is_open() const override { return this->open_flag.load(std::memory_order_acquire); }

    /* Worker thread: GAME -> ABOUT. */
    void open();
    /* Worker thread: ABOUT -> GAME. */
    void close();

    /* One input step; called by the worker thread (~50 Hz). */
    void update(unsigned pad);

    /* Rasterize the window. Main thread only. */
    void paint() override;

    /* Display thread: draw logo quad. */
    void draw() override;

private:
    void draw_logo();
    void init_logo();

    std::atomic<bool> open_flag;

    /* Logo state. */
    uint32_t logo_tex[LOGO_TEX_W * LOGO_TEX_H];
    int logo_w, logo_h;
    bool logo_loaded;
    bool logo_upload;
};
