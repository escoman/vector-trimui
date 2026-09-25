#pragma once

#include <inttypes.h>

/*
 * Shared GE drawing utilities for UILayer subclasses.  Every layer
 * draws textured quads through the same PSP Graphics Engine; these
 * helpers factor out the recurring setup so each layer's draw() stays
 * concise.
 *
 * The frame vertex pool lives here: a small static 16-byte-aligned
 * buffer consumed during TV::render() and reset at the start of each
 * frame.  All GE vertex data is allocated from this pool.
 */

/* SDL_Renderer backend (TrimUI). layer_draw_begin() is called once per
 * frame from TV::render() with the active renderer; it latches the
 * renderer and derives the uniform letterbox transform that maps the
 * 480x272 GUI space onto the renderer logical size. All other helpers
 * below draw through that renderer. */
struct SDL_Renderer;
void layer_draw_begin(struct SDL_Renderer * renderer);

/* Translucent black backdrop over the whole logical screen, drawn under
 * the popup windows (alpha 0x80, matching the PSP MENU_DIM_ALPHA). */
void layer_draw_dim_overlay();

/* Reset the per-frame vertex pool (called once at the top of
 * TV::render, before any layer draws). No-op on the SDL backend. */
void layer_draw_vertex_reset();

/* Allocate aligned vertex memory from the per-frame pool.
 * Unused on the SDL backend; returns nullptr. */
void * layer_draw_alloc_vertices(unsigned bytes);

/* PSP display dimensions (also used by TV internally). */
#define LAYER_SCREEN_W 480
#define LAYER_SCREEN_H 272

/*
 * Draw a single centered textured quad from an indexed (8-bit CLUT)
 * texture.  This is the standard "popup panel" rendering:
 *
 *   - cache writeback if upload_flag is true (one-time per repaint);
 *   - CLUT + texture mode setup (T8 indexed, RGBA, NEAREST);
 *   - alpha blend enabled;
 *   - one screen-centered quad of disp_w x disp_h.
 *
 * Parameters:
 *   tex         — 8-bit indexed texture data
 *   tex_w/h     — power-of-two texture dimensions
 *   clut        — 256-entry RGBA palette
 *   disp_w/h    — on-screen quad size in pixels
 *   upload_flag — consume_tex_upload() result; triggers cache writeback
 */
void layer_draw_centered_quad(
    const uint8_t * tex, int tex_w, int tex_h,
    const uint32_t * clut,
    float disp_w, float disp_h,
    bool upload_flag);

/*
 * Draw a textured quad at an explicit screen position with explicit
 * UV coordinates.  For supplementary rendering (thumbnails, previews)
 * that need custom placement or sub-rectangle sampling.
 *
 *   bilinear — true for GU_LINEAR filtering (scaled images);
 *              false for GU_NEAREST (pixel-crisp text/icons).
 */
void layer_draw_quad(
    const uint8_t * tex, int tex_w, int tex_h,
    const uint32_t * clut,
    float screen_x, float screen_y,
    float disp_w, float disp_h,
    float u0, float v0, float u1, float v1,
    bool bilinear,
    bool upload_flag);

/*
 * Same as layer_draw_quad but for a 32-bit RGBA texture (no CLUT).
 * Used by StateWindow thumbnails and RomBrowser/GameCenter previews.
 */
void layer_draw_rgba_quad(
    const uint32_t * tex, int tex_w, int tex_h,
    float screen_x, float screen_y,
    float disp_w, float disp_h,
    float u0, float v0, float u1, float v1,
    bool bilinear,
    bool upload_flag);
