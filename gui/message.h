#pragma once

#include <atomic>
#include <inttypes.h>
#include "layer.h"

/*
 * Universal modal message dialog with a text message and YES/NO or
 * Close buttons.  Rasterized into its own small indexed texture and
 * drawn by TV as a separate layer above all popup windows.
 *
 * The atomics/repaint handshake is inherited from the PSP port and
 * harmless in the single-threaded TrimUI UI loop.
 *
 * Usage:
 *   dialog.show("LOAD ROM?", MessageDialog::YES_NO);
 *   // ... later, after dialog.dismisses() ...
 *   if (dialog.result() == MessageDialog::RESULT_YES) { ... }
 */
class MessageDialog : public UILayer
{
public:
    enum Style { YES_NO, CLOSE };
    enum Result { RESULT_NONE = -1, RESULT_YES = 0, RESULT_NO = 1,
                  RESULT_CLOSE = 0 };

    /* Texture dimensions (power-of-two, layer_draw cache). */
    static const int DLG_TEX_W = 256;
    static const int DLG_TEX_H = 128;

    /* Dialog box within the texture. */
    static const int DLG_W = 200;
    static const int DLG_H = 80;
    static const int DLG_X = (DLG_TEX_W - DLG_W) / 2;   /* 28 */
    static const int DLG_Y = (DLG_TEX_H - DLG_H) / 2;   /* 24  */

    MessageDialog();

    /* Show the dialog with a message and button style.  Resets
     * result to RESULT_NONE. */
    void show(const char * message, Style style);
    /* Dismiss with a specific result. */
    void dismiss(Result r);

    bool is_active() const override
    {
        return active.load(std::memory_order_acquire);
    }
    Result result() const
    {
        return static_cast<Result>(
            result_val.load(std::memory_order_relaxed));
    }

    /* One input step (~50 Hz).  LEFT/RIGHT move the selection
     * (YES_NO only), A confirms, B/START cancels. */
    void update(unsigned pad);

    /* Repaint machinery (same pattern as Popup). */
    bool needs_repaint() const override
    {
        return paint_seq.load(std::memory_order_relaxed) != painted_seq;
    }
    bool consume_tex_upload() override
    {
        bool v = tex_upload;
        tex_upload = false;
        return v;
    }
    const uint8_t * tex_data() const override { return tex; }
    const uint32_t * clut_data() const override { return clut; }
    int tex_width() const override { return DLG_TEX_W; }
    int tex_height() const override { return DLG_TEX_H; }
    int display_width() const override { return DLG_TEX_W; }
    int display_height() const override { return DLG_TEX_H; }
    bool wants_dim() const override { return false; }
    void paint() override;
    void draw() override;

    /* Pad masks for update(). */
    enum {
        PAD_LEFT  = 0x10,
        PAD_RIGHT = 0x20,
        PAD_PRESS = 0x04,  /* A button */
        PAD_BACK  = 0x08,  /* B / START */
    };

private:
    /* Palette indices (local to this dialog). */
    enum Color : uint8_t {
        C_DIM = 0,       /* semi-transparent black (dim + padding) */
        C_BG,            /* dialog box background */
        C_BORDER,        /* dialog box border */
        C_TEXT_WHITE,    /* normal text */
        C_TEXT_BLACK,    /* selected button text */
        C_BTN_SEL,       /* selected button highlight */
    };

    void fill_rect(int x, int y, int w, int h, uint8_t color);
    void print_text2x(int x, int y, const char * text, uint8_t color);

    /* Keyup edge detection (same pattern as Popup). */
    bool keyup_edge(unsigned pad, unsigned mask) const
    {
        return (pad & mask) == 0 && (prev_pad & mask) != 0;
    }

    alignas(16) uint8_t tex[DLG_TEX_W * DLG_TEX_H];
    alignas(16) uint32_t clut[256];

    std::atomic<bool> active;
    std::atomic<int>  result_val;

    /* Repaint handshake (same as Popup). */
    std::atomic<unsigned> paint_seq;
    unsigned painted_seq;
    bool tex_upload;

    /* Input state. */
    unsigned prev_pad;
    int selection;       /* 0 = left button, 1 = right button */
    Style style;
    char message[128];
};
