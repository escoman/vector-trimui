#include "message.h"
#include "font.h"
#include "layer_draw.h"

#include <cstring>

/*
 * Universal modal message dialog.  Own indexed texture, same visual
 * style as the Popup windows (palette colours, 2x overlay font).
 *
 * The texture covers the full 256x128 area.  Outside the dialog box
 * every pixel is C_DIM (semi-transparent black), which dims the
 * popup window underneath and provides the modal "dark overlay"
 * effect.  Inside the dialog box the pixels are opaque panel
 * background — the box itself is solid.
 */

/* 0xAABBGGRR, the RGBA layout the layer_draw backend expects. */
static uint32_t msg_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return 0xff000000u |
           ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
}

MessageDialog::MessageDialog() :
    active(false),
    result_val(RESULT_NONE),
    paint_seq(0), painted_seq(0), tex_upload(false),
    prev_pad(0), selection(0), style(YES_NO)
{
    memset(tex, 0, sizeof(tex));
    message[0] = '\0';

    memset(clut, 0, sizeof(clut));
    clut[C_DIM]        = 0x80000000u;  /* semi-transparent black */
    clut[C_BG]         = msg_rgb(0x18, 0x18, 0x18);
    clut[C_BORDER]     = msg_rgb(0xc0, 0xc0, 0xc0);
    clut[C_TEXT_WHITE] = msg_rgb(0xff, 0xff, 0xff);
    clut[C_TEXT_BLACK] = msg_rgb(0x00, 0x00, 0x00);
    clut[C_BTN_SEL]    = msg_rgb(0xb0, 0xb0, 0xb0);
}

/* ---- Input / state ---- */

void MessageDialog::show(const char * msg, Style s)
{
    strncpy(message, msg, sizeof(message) - 1);
    message[sizeof(message) - 1] = '\0';
    style = s;
    selection = (s == YES_NO) ? 1 : 0;  /* default to NO / Close */
    result_val.store(RESULT_NONE, std::memory_order_relaxed);
    prev_pad = 0;
    active.store(true, std::memory_order_release);
    paint_seq.fetch_add(1, std::memory_order_relaxed);
}

void MessageDialog::dismiss(Result r)
{
    result_val.store(static_cast<int>(r), std::memory_order_relaxed);
    active.store(false, std::memory_order_release);
    paint_seq.fetch_add(1, std::memory_order_relaxed);
}

void MessageDialog::update(unsigned pad)
{
    if (!active.load(std::memory_order_acquire))
        return;

    if (style == YES_NO) {
        if (keyup_edge(pad, PAD_LEFT)) {
            selection = 0;  /* YES */
            paint_seq.fetch_add(1, std::memory_order_relaxed);
        }
        if (keyup_edge(pad, PAD_RIGHT)) {
            selection = 1;  /* NO */
            paint_seq.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (keyup_edge(pad, PAD_PRESS)) {
        if (style == YES_NO) {
            dismiss(selection == 0 ? RESULT_YES : RESULT_NO);
        } else {
            dismiss(RESULT_CLOSE);
        }
    }

    if (keyup_edge(pad, PAD_BACK)) {
        if (style == YES_NO) {
            dismiss(RESULT_NO);
        } else {
            dismiss(RESULT_CLOSE);
        }
    }

    prev_pad = pad;
}

/* ---- Rasterization ---- */

void MessageDialog::fill_rect(int x, int y, int w, int h, uint8_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > DLG_TEX_W) w = DLG_TEX_W - x;
    if (y + h > DLG_TEX_H) h = DLG_TEX_H - y;

    for (int yy = y; yy < y + h; ++yy) {
        uint8_t * dst = tex + (size_t)yy * DLG_TEX_W + x;
        memset(dst, color, (size_t)w);
    }
}

void MessageDialog::print_text2x(int x, int y, const char * text,
                                  uint8_t color)
{
    int cx = x;
    for (const char * p = text; *p != '\0'; ++p) {
        const uint8_t * g = overlay_font_glyph(*p);
        if (g != nullptr) {
            for (int gy = 0; gy < OVERLAY_FONT_H; ++gy) {
                const uint8_t row = g[gy];
                for (int gx = 0; gx < OVERLAY_FONT_W; ++gx) {
                    if (row & (0x80u >> gx)) {
                        fill_rect(cx + gx * 2, y + gy * 2, 2, 2, color);
                    }
                }
            }
        }
        cx += OVERLAY_FONT_W * 2;
    }
}

void MessageDialog::paint()
{
    if (!active.load(std::memory_order_acquire))
        return;

    const unsigned seq = paint_seq.load(std::memory_order_relaxed);

    /* Dim fill: semi-transparent black over the whole texture.
     * Outside the dialog box this acts as the modal dark overlay;
     * inside the dialog box the opaque bg drawn next will cover it. */
    fill_rect(0, 0, DLG_TEX_W, DLG_TEX_H, C_DIM);

    /* Dialog box (opaque panel background). */
    fill_rect(DLG_X, DLG_Y, DLG_W, DLG_H, C_BG);

    /* Border. */
    fill_rect(DLG_X, DLG_Y, DLG_W, 1, C_BORDER);
    fill_rect(DLG_X, DLG_Y + DLG_H - 1, DLG_W, 1, C_BORDER);
    fill_rect(DLG_X, DLG_Y, 1, DLG_H, C_BORDER);
    fill_rect(DLG_X + DLG_W - 1, DLG_Y, 1, DLG_H, C_BORDER);

    /* Message text, centered. */
    const int tw = (int)strlen(message) * OVERLAY_FONT_W * 2;
    print_text2x(DLG_X + (DLG_W - tw) / 2, DLG_Y + 10,
                 message, C_TEXT_WHITE);

    /* Buttons. */
    if (style == YES_NO) {
        const char * yes_text = "YES";
        const char * no_text  = "NO";
        const int yw = (int)strlen(yes_text) * OVERLAY_FONT_W * 2;
        const int nw = (int)strlen(no_text)  * OVERLAY_FONT_W * 2;
        const int opt_y = DLG_Y + 40;

        if (selection == 0) {
            fill_rect(DLG_X + 30, opt_y - 2,
                      yw + 8, OVERLAY_FONT_H * 2 + 4, C_BTN_SEL);
            print_text2x(DLG_X + 34, opt_y, yes_text, C_TEXT_BLACK);
        } else {
            print_text2x(DLG_X + 34, opt_y, yes_text, C_TEXT_WHITE);
        }

        if (selection == 1) {
            fill_rect(DLG_X + 120, opt_y - 2,
                      nw + 8, OVERLAY_FONT_H * 2 + 4, C_BTN_SEL);
            print_text2x(DLG_X + 124, opt_y, no_text, C_TEXT_BLACK);
        } else {
            print_text2x(DLG_X + 124, opt_y, no_text, C_TEXT_WHITE);
        }
    } else {
        /* CLOSE style: single centered button. */
        const char * close_text = "CLOSE";
        const int cw = (int)strlen(close_text) * OVERLAY_FONT_W * 2;
        const int opt_y = DLG_Y + 40;

        fill_rect(DLG_X + (DLG_W - cw) / 2 - 4, opt_y - 2,
                  cw + 8, OVERLAY_FONT_H * 2 + 4, C_BTN_SEL);
        print_text2x(DLG_X + (DLG_W - cw) / 2, opt_y,
                     close_text, C_TEXT_BLACK);
    }

    painted_seq = seq;
    tex_upload = true;
}

void MessageDialog::draw()
{
    layer_draw_centered_quad(
        tex_data(), DLG_TEX_W, DLG_TEX_H,
        clut_data(),
        (float)DLG_TEX_W, (float)DLG_TEX_H,
        consume_tex_upload());
}
