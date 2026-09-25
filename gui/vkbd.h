#pragma once

#include <array>
#include <atomic>
#include <string>
#include <functional>
#include <inttypes.h>

#include "event.h"
#include "layer.h"

/*
 * On-screen Vector-06C keyboard (VKBD) for the PSP port.
 *
 * Adapted from the vector06sdl libretro core (libretro/vkbd.h): the
 * same key layout, Russian/Latin legends, sticky keys, autorepeat and
 * selection logic, but rasterized directly into an 8-bit indexed PSP
 * GU texture instead of the libretro Graphics32 overlay.
 *
 * The VKBD is driven from two threads:
 *
 *   Worker thread (emulation): polls the PSP pad and calls
 *   show/hide/move/update(); a visual change bumps paint_seq.
 *
 *         VKBD  ---- SDL scancode ---->  Emulator::keydown()/keyup()
 *                                              |
 *                                              v
 *                                     Keyboard (Vector matrix)
 *
 *   Main thread (display): needs_repaint()/paint() rasterize the
 *   texture and the GE renders it as an overlay quad above or below
 *   the machine picture (see TV::render).
 *
 * It never touches the Vector framebuffer owned by the worker
 * thread. The texture is repainted only when the visual state
 * changes (selection, pressed keys, РУС/LAT).
 */

/* Normalized pad state passed to VirtualKeyboard::update(): which
 * buttons are currently held. Keeps vkbd free of pspctrl.h. */
enum {
    VKBD_PAD_LEFT  = 0x01,
    VKBD_PAD_RIGHT = 0x02,
    VKBD_PAD_UP    = 0x04,
    VKBD_PAD_DOWN  = 0x08,
    VKBD_PAD_PRESS = 0x10, /* X: press the selected key */
};

class VirtualKeyboard : public UILayer
{
public:
    static const int NUM_ROWS = 5;
    static const int NUM_COLS = 17;

    /* GE texture for the overlay: power-of-two dimensions, 8-bit
     * indices into clut[]. The keyboard itself occupies the top-left
     * kb_width x kb_height window of it. */
    static const int VKBD_TEX_W = 512;
    static const int VKBD_TEX_H = 128;

    /* Palette indices used by the rasterizer. 11..14 are the pressed
     * (lightened) variants of ALPHA/BROWN/GREEN/FN. */
    enum Color : uint8_t {
        C_BACKGROUND = 0,
        C_KEY_BORDER,
        C_KEY_ALPHA,
        C_KEY_BROWN,
        C_KEY_GREEN,
        C_KEY_FN,
        C_KEY_TEXT,
        C_KEY_TEXT_BROWN,
        C_KEY_BORDER_SELECT,
        C_LED_ON,
        C_LED_OFF,
        C_PRESSED_BASE = 11,
    };

    VirtualKeyboard();

    /* Build the key layout once (call before first use). */
    void prepare();

    /* Machine РУС/ЛАТ mode latch (IO::PC bit 3, delivered via
     * IO::onruslat); lit = Russian input mode. The ROM toggles it on
     * every РУС/ЛАТ press; the VKBD shows it as the LED instead of
     * keeping its own copy. */
    void set_ruslat_source(const bool * rus) { this->ruslat_src = rus; }

    bool is_visible() const { return this->visible.load(std::memory_order_acquire); }
    bool is_active() const override { return is_visible(); }
    bool is_top() const { return this->top.load(std::memory_order_acquire); }

    /* SELECT: show/hide. The pad snapshot keeps held buttons from
     * producing spurious edges right after the toggle. hide()
     * releases every active virtual key first. The top/bottom
     * position survives both. */
    void show(unsigned pad_snapshot);
    void hide(unsigned pad_snapshot);

    /* O: move the keyboard top <-> bottom (visible state only). */
    void move() { this->top = !this->top.load(); }

    /* One input step; called by the worker thread once per machine
     * frame (50 Hz) while visible, with the currently held pad
     * buttons. D-pad navigates (with autorepeat), VKBD_PAD_PRESS
     * presses the selected key: momentary for normal keys, sticky
     * toggle for the hold-modifiers (СС/УС). */
    void update(unsigned pad);

    /* Release every pressed/sticky virtual key (keyup into the
     * emulator). Used when the VKBD is hidden. */
    void release_all();

    int get_width() const { return this->kb_width; }
    int get_height() const { return this->kb_height; }

    /* True when the texture must be repainted; also latches РУС/LAT
     * changes from the source keyboard. Main thread only. */
    bool needs_repaint() const override;
    /* Rasterize the keyboard into tex[]. Main thread only; a change
     * arriving from the worker while painting forces one more pass. */
    void paint() override;

    /* True once after paint(): the GE must write the texture back to
     * main memory before it is sampled. Consumed by the renderer. */
    bool consume_tex_upload() override
    {
        bool v = this->tex_upload;
        this->tex_upload = false;
        return v;
    }

    const uint8_t * tex_data() const override { return this->tex; }
    const uint32_t * clut_data() const override { return this->clut; }
    int tex_width() const override { return VKBD_TEX_W; }
    int tex_height() const override { return VKBD_TEX_H; }
    int display_width() const override { return this->kb_width; }
    int display_height() const override { return this->kb_height; }
    bool wants_dim() const override { return false; }
    void draw() override;

    /* Scancode sinks, wired to Emulator::keydown/keyup. */
    std::function<void(int)> on_keydown;
    std::function<void(int)> on_keyup;

private:
    enum class unit_width_t { U0_5, U1, U1_5, U7 };

    struct key_info_t {
        int x, y;
        int width;
        uint8_t color;       /* palette index */
        uint8_t text_color;  /* palette index */
        int row, col;        /* normal row, col (col = 0..16) */
        int coord;           /* row * 100 + col, cols >= 50 are numpad */
        std::string legend_1; /* CP866 bytes */
        std::string legend_2; /* CP866 bytes */
        int scancode;
        bool pressed;
    };

    key_info_t & selected();
    void key_down(key_info_t & ki, bool sticky);
    void key_up(key_info_t & ki, bool unstick);
    void unstick_stickies();
    void move_finger(int dx, int dy);

    void make_key_info(key_info_t & ki, int col, int row,
                       const std::string & L1, const std::string & L2);
    int pixel_width(unit_width_t uwidth) const;

    /* Rasterizers into the indexed texture */
    void fill_rect(int x, int y, int w, int h, uint8_t color);
    void fill_ellipse(int cx, int cy, int rx, int ry, uint8_t color);
    void key_rect(int x, int y, int w, int h, uint8_t color);
    void print_text(int x, int y, const std::string & text, uint8_t color,
                    int char_w = 8 /* VKBD_FONT_W, or 4 for narrow */);
    void draw_key(const key_info_t & ki);
    void draw_ruslat();

    static bool is_sticky_scancode(int scancode);

private:
    int unit_w, unit_h;  /* unit key width and height */
    int xgap, ygap;      /* key gap */
    int kb_width, kb_height;

    int select_row, select_col;
    int finger_row, finger_col;

    /* Atomic: written by the worker thread (show/hide/move), read by
     * the display thread (render, repaint check). The top/bottom
     * position survives hiding and is what the MAIN MENU reopening
     * relies on. */
    std::atomic<bool> visible;
    std::atomic<bool> top;   /* false = bottom of the screen (default) */

    unsigned prev_pad;
    int autorepeat_count;

    /* Mirror of the last seen РУС/LAT state, to notice changes. */
    mutable bool last_ruslat;
    const bool * ruslat_src;

    /* Repaint handshake across threads: the worker bumps paint_seq
     * on every visual change, the main thread repaints once it sees
     * a value newer than painted_seq. A plain flag would be lost in
     * a set/clear race between the two threads. */
    std::atomic<unsigned> paint_seq;
    unsigned painted_seq;  /* main thread only */
    bool tex_upload;       /* repainted, cache writeback needed */

    std::array<int, 16> keys_down;
    std::array<int, 16> sticky_down;

    std::array<key_info_t, NUM_COLS * NUM_ROWS> key_map;

    alignas(16) uint8_t tex[VKBD_TEX_W * VKBD_TEX_H];
    alignas(16) uint32_t clut[256];

    static const int autorepeat_delay = 8;  /* frames */
    static const int autorepeat_rate = 4;   /* frames */

    static const int TOP_BORDER = 1;
    static const int BOTTOM_BORDER = 0;
    static const int LED_RADIUS = 3;

    static const char * const top_text[];
    static const char * const bottom_text[];
    static const char * const num_text[];
    static const int longKeys[6];     /* also brown */
    static const int greenishKeys[6];
    static const int mustardKeys[6];
    static const int spaceKey;
    static const int scancodes[5][14];
    static const int scancodes_num[5][3];
};
