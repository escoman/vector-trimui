#pragma once

#include <atomic>
#include <inttypes.h>
#include "layer.h"

/*
 * Common base of the PSP popup UI windows (MAIN MENU, ROM Browser).
 * Every popup owns an indexed GE texture rasterized by the display
 * thread and presented by TV as one quad above the shared dim
 * backdrop, in the 480x272 UI coordinate space.
 *
 * Shared machinery living here:
 *   - tex/clut storage and the common palette;
 *   - the paint_seq/painted_seq repaint handshake between the worker
 *     thread (state changes) and the display thread (rasterizer);
 *   - rasterization helpers (fill_rect, print_text2x);
 *   - the keyup edge input helper for list navigation.
 *
 * Threading split, as before: the worker mutates the popup state and
 * calls mark_dirty(); the display thread polls needs_repaint() and
 * calls the subclass paint().
 *
 * Popup inherits UILayer so TV can draw any popup generically via
 * draw() without knowing the concrete type.
 */
class Popup : public UILayer
{
public:
    /* GE texture shared by every popup: power-of-two dimensions,
     * 8-bit indices into clut[]. The panel occupies its own
     * panel_w x panel_h window in the top-left corner; both existing
     * popups (256x212 menu, 440x220 browser) fit. */
    static const int POPUP_TEX_W = 512;
    static const int POPUP_TEX_H = 256;

    virtual ~Popup();

    /* UILayer interface — each subclass must provide is_open(). */
    virtual bool is_open() const = 0;
    bool is_active() const override { return is_open(); }

    bool needs_repaint() const override
    {
        return paint_seq.load(std::memory_order_relaxed) != painted_seq;
    }

    virtual void paint() override = 0;

    bool consume_tex_upload() override
    {
        bool v = tex_upload;
        tex_upload = false;
        return v;
    }

    const uint8_t * tex_data() const override { return tex; }
    const uint32_t * clut_data() const override { return clut; }

    int tex_width() const override { return POPUP_TEX_W; }
    int tex_height() const override { return POPUP_TEX_H; }
    int display_width() const override { return panel_w; }
    int display_height() const override { return panel_h; }

    int get_width() const { return panel_w; }
    int get_height() const { return panel_h; }

protected:
    Popup();

    /* Palette indices used by the rasterizers. */
    enum Color : uint8_t {
        C_PANEL_BG = 0,     /* panel background */
        C_PANEL_BORDER,     /* inner dividers (browser header line) */
        C_ITEM_BG,          /* unselected row: dark gray */
        C_ITEM_BG_SEL,      /* selected row: light gray */
        C_TEXT_WHITE,       /* unselected text / titles / hints */
        C_TEXT_BLACK,       /* selected text */
        C_HOLE,             /* fully transparent: a window in the
                             * panel letting an underlaid GE quad
                             * (state slot thumbnail) show through */
        C_DIM               /* semi-transparent black: dims the
                             * panel behind confirmation dialogs */
    };

    void fill_rect(int x, int y, int w, int h, uint8_t color);
    /* overlay font (font.h) at double size: 16x16 per glyph. */
    void print_text2x(int x, int y, const char * text, uint8_t color);
    /* overlay font at native size: 8x8 per glyph, for compact
     * labels where the 2x variant is too big (state grid cells). */
    void print_text(int x, int y, const char * text, uint8_t color);
    /* Same font with custom character advance (pixels). Use for
     * tighter spacing when the default 8px is too wide. */
    void print_text(int x, int y, const char * text, uint8_t color, int advance);

    /* Worker thread: bump on every visual state change so the
     * display thread repaints. */
    void mark_dirty()
    {
        paint_seq.fetch_add(1, std::memory_order_relaxed);
    }
    /* Subclass paint(): snapshot the sequence first (a state change
     * arriving mid-paint forces another pass), rasterize, then call
     * finish_paint() with the snapshot. */
    unsigned snapshot_seq() const
    {
        return paint_seq.load(std::memory_order_relaxed);
    }
    void finish_paint(unsigned seq)
    {
        painted_seq = seq;
        tex_upload = true;
    }

    /* Worker thread: reset prev_pad on open(). */
    void reset_input_state()
    {
        prev_pad = 0;
    }
    /* Keyup edge: fires once when the button is released (held on the
     * previous poll, gone on this one). Every press moves exactly one
     * step — the next one needs a fresh press; holding the button
     * down never repeats. */
    bool keyup_edge(unsigned pad, unsigned mask) const
    {
        return (pad & mask) == 0 && (prev_pad & mask) != 0;
    }

    unsigned prev_pad;      /* worker thread only */

    int panel_w, panel_h;

    /* Repaint handshake across threads, same scheme as the VKBD:
     * the worker bumps paint_seq on every visual change, the main
     * thread repaints once it sees a value newer than painted_seq.
     * A plain flag would be lost in a set/clear race. */
    std::atomic<unsigned> paint_seq;
    unsigned painted_seq;   /* main thread only */
    bool tex_upload;        /* repainted, cache writeback needed */

    alignas(16) uint8_t tex[POPUP_TEX_W * POPUP_TEX_H];
    alignas(16) uint32_t clut[256];
};
