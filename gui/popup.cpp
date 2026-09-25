#include "popup.h"
#include "font.h"

#include <cstring>

/*
 * Shared rasterization machinery of the popup windows (MAIN MENU,
 * ROM Browser), see popup.h. The per-window content lives in the
 * subclasses' paint().
 */

static uint32_t popup_rgb_to_psp(uint8_t r, uint8_t g, uint8_t b)
{
    /* Same layout as the GE CLUT built in TV::init(): memory order
     * A B G R. Popup colors are fully opaque. */
    return 0xff000000u |
           ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
}

Popup::Popup() :
    prev_pad(0),
    panel_w(0), panel_h(0),
    paint_seq(1), painted_seq(0), tex_upload(false)
{
    memset(tex, 0, sizeof(tex));

    memset(clut, 0, sizeof(clut));
    clut[C_PANEL_BG]     = popup_rgb_to_psp(0x18, 0x18, 0x18);
    clut[C_PANEL_BORDER] = popup_rgb_to_psp(0xc0, 0xc0, 0xc0);
    clut[C_ITEM_BG]      = popup_rgb_to_psp(0x40, 0x40, 0x40);
    clut[C_ITEM_BG_SEL]  = popup_rgb_to_psp(0xb0, 0xb0, 0xb0);
    clut[C_TEXT_WHITE]   = popup_rgb_to_psp(0xff, 0xff, 0xff);
    clut[C_TEXT_BLACK]   = popup_rgb_to_psp(0x00, 0x00, 0x00);
    /* Fully transparent window: lets an underlaid GE quad (state
     * slot thumbnails) show through the panel. All other entries
     * stay opaque, so existing popups render unchanged. */
    clut[C_HOLE]         = 0x00000000u;
    clut[C_DIM]          = 0x80000000u; /* semi-transparent black */

    /* The table never changes after the constructor. On the SDL
     * backend the CLUT is expanded into an RGBA scratch buffer at
     * upload time (layer_draw.cpp), so no cache writeback is needed. */
}

Popup::~Popup() {}

void Popup::fill_rect(int x, int y, int w, int h, uint8_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > POPUP_TEX_W) w = POPUP_TEX_W - x;
    if (y + h > POPUP_TEX_H) h = POPUP_TEX_H - y;

    for (int yy = y; yy < y + h; ++yy) {
        uint8_t * dst = tex + (size_t)yy * POPUP_TEX_W + x;
        memset(dst, color, (size_t)w);
    }
}

/* Overlay font (font.h, 8x8) rasterized at double size so the popup
 * text stays readable at display resolution. */
void Popup::print_text2x(int x, int y, const char * text, uint8_t color)
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

/* The same font at native 8x8 size: compact labels (state slot
 * titles and dates). Direct pixel writes instead of fill_rect: the
 * glyphs are small and the clipping overhead would dominate. */
void Popup::print_text(int x, int y, const char * text, uint8_t color)
{
    print_text(x, y, text, color, OVERLAY_FONT_W);
}

/* Same font with custom character advance for tighter spacing. */
void Popup::print_text(int x, int y, const char * text, uint8_t color, int advance)
{
    int cx = x;
    for (const char * p = text; *p != '\0'; ++p) {
        const uint8_t * g = overlay_font_glyph(*p);
        if (g != nullptr) {
            for (int gy = 0; gy < OVERLAY_FONT_H; ++gy) {
                const int py = y + gy;
                if (py < 0 || py >= POPUP_TEX_H)
                    continue;
                const uint8_t row = g[gy];
                for (int gx = 0; gx < OVERLAY_FONT_W; ++gx) {
                    const int px = cx + gx;
                    if ((row & (0x80u >> gx))
                            && px >= 0 && px < POPUP_TEX_W) {
                        tex[(size_t)py * POPUP_TEX_W + px] = color;
                    }
                }
            }
        }
        cx += advance;
    }
}
