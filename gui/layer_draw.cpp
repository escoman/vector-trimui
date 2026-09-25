#include "layer_draw.h"
#include "layer.h"

#include "SDL.h"

#include <vector>
#include <cmath>
#include <cstring>

/*
 * SDL_Renderer backend for the shared UILayer drawing helpers.
 *
 * The PSP port drew every popup through the Graphics Engine: an
 * indexed (8-bit + 256-entry CLUT) or 32-bit RGBA texture blitted as
 * one quad. On TrimUI the same window classes rasterize into the very
 * same tex[]/clut[] buffers, so this file only replaces the GE with
 * SDL_Texture/SDL_RenderCopy while keeping the public API identical.
 *
 * Coordinate space: GUI windows are authored in 480x272
 * (LAYER_SCREEN_W/H). layer_draw_begin() is called once per frame from
 * TV::render() and derives a uniform letterbox transform that maps
 * 480x272 onto the renderer logical size, so the overlay is centered
 * and never distorted.
 *
 * Pixel format: both the CLUT entries and the RGBA textures are packed
 * as 0xAABBGGRR, which is exactly SDL_PIXELFORMAT_ABGR8888 (uint32 MSB
 * -> LSB = A,B,G,R). Indexed textures are expanded through the CLUT
 * into a scratch RGBA buffer and uploaded with SDL_UpdateTexture.
 * Blending is non-premultiplied (SDL_BLENDMODE_BLEND): C_HOLE=0 is
 * fully transparent, C_DIM=0x80000000 half, opaque colors 0xff......
 */

/* ---- Per-frame renderer + letterbox transform ---- */

static SDL_Renderer * g_renderer = nullptr;
static float g_scale = 1.0f;
static float g_off_x = 0.0f;
static float g_off_y = 0.0f;
static int g_logical_w = LAYER_SCREEN_W;
static int g_logical_h = LAYER_SCREEN_H;

void layer_draw_begin(SDL_Renderer * renderer)
{
    g_renderer = renderer;
    if (!renderer) {
        return;
    }

    int lw = 0, lh = 0;
    SDL_RenderGetLogicalSize(renderer, &lw, &lh);
    if (lw <= 0 || lh <= 0) {
        SDL_GetRendererOutputSize(renderer, &lw, &lh);
    }
    if (lw <= 0 || lh <= 0) {
        lw = LAYER_SCREEN_W;
        lh = LAYER_SCREEN_H;
    }
    g_logical_w = lw;
    g_logical_h = lh;

    g_scale = (float)lw / (float)LAYER_SCREEN_W;
    float sy = (float)lh / (float)LAYER_SCREEN_H;
    if (sy < g_scale) g_scale = sy;

    g_off_x = ((float)lw - (float)LAYER_SCREEN_W * g_scale) * 0.5f;
    g_off_y = ((float)lh - (float)LAYER_SCREEN_H * g_scale) * 0.5f;
}

/* ---- Texture cache ----
 *
 * Every UILayer owns a stable tex[] buffer, so the buffer pointer is a
 * good cache key. Textures are created once (STATIC + BLEND) and only
 * re-uploaded when the layer signals a repaint (upload_flag) or on
 * first use. A small round-robin table with eviction is plenty: the
 * port never shows more than a handful of layers at once. */

struct TexEntry {
    const void * key;
    SDL_Texture * tex;
    int w, h;
};

static const int TEX_CACHE_SLOTS = 16;
static TexEntry g_cache[TEX_CACHE_SLOTS];
static int g_cache_count = 0;   /* number of filled slots (grows to SLOTS) */
static int g_cache_next = 0;    /* round-robin eviction cursor */

/* Find or create the texture for key. *is_new reports whether the
 * texture was just created (caller must upload pixel data). */
static SDL_Texture * tex_cache_get(const void * key, int w, int h,
                                   bool bilinear, bool * is_new)
{
    for (int i = 0; i < g_cache_count; ++i) {
        if (g_cache[i].key == key) {
            *is_new = false;
            return g_cache[i].tex;
        }
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, bilinear ? "1" : "0");
    SDL_Texture * tex = SDL_CreateTexture(
        g_renderer, SDL_PIXELFORMAT_ABGR8888,
        SDL_TEXTUREACCESS_STATIC, w, h);
    if (!tex) {
        *is_new = false;
        return nullptr;
    }
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);

    int slot;
    if (g_cache_count < TEX_CACHE_SLOTS) {
        slot = g_cache_count++;
    } else {
        slot = g_cache_next;
        g_cache_next = (g_cache_next + 1) % TEX_CACHE_SLOTS;
        if (g_cache[slot].tex) {
            SDL_DestroyTexture(g_cache[slot].tex);
        }
    }
    g_cache[slot].key = key;
    g_cache[slot].tex = tex;
    g_cache[slot].w = w;
    g_cache[slot].h = h;

    *is_new = true;
    return tex;
}

/* ---- Internal: blit a source rect (texels) to a GUI-space dst ---- */

static void blit(SDL_Texture * tex,
                 float u0, float v0, float u1, float v1,
                 float sx, float sy, float sw, float sh)
{
    if (!tex || !g_renderer) {
        return;
    }
    SDL_Rect src;
    src.x = (int)lroundf(u0);
    src.y = (int)lroundf(v0);
    src.w = (int)lroundf(u1 - u0);
    src.h = (int)lroundf(v1 - v0);
    if (src.w <= 0 || src.h <= 0) {
        return;
    }

    SDL_Rect dst;
    dst.x = (int)lroundf(g_off_x + sx * g_scale);
    dst.y = (int)lroundf(g_off_y + sy * g_scale);
    dst.w = (int)lroundf(sw * g_scale);
    dst.h = (int)lroundf(sh * g_scale);
    if (dst.w <= 0 || dst.h <= 0) {
        return;
    }

    SDL_RenderCopy(g_renderer, tex, &src, &dst);
}

/* ---- Internal: indexed (8-bit CLUT) texture draw ---- */

static void draw_indexed(const uint8_t * tex, int tex_w, int tex_h,
                         const uint32_t * clut,
                         float u0, float v0, float u1, float v1,
                         float sx, float sy, float sw, float sh,
                         bool bilinear, bool upload_flag)
{
    bool is_new = false;
    SDL_Texture * sdl_tex = tex_cache_get(tex, tex_w, tex_h, bilinear, &is_new);
    if (!sdl_tex) {
        return;
    }

    if (is_new || upload_flag) {
        static std::vector<uint32_t> scratch;
        size_t n = (size_t)tex_w * (size_t)tex_h;
        if (scratch.size() < n) {
            scratch.resize(n);
        }
        for (size_t i = 0; i < n; ++i) {
            scratch[i] = clut[tex[i]];
        }
        SDL_UpdateTexture(sdl_tex, nullptr, scratch.data(), tex_w * 4);
    }

    blit(sdl_tex, u0, v0, u1, v1, sx, sy, sw, sh);
}

/* ---- Public API ---- */

/* The GE vertex pool is meaningless on SDL; keep the entry points as
 * no-op stubs so the (unported) window classes still link. */
void layer_draw_vertex_reset() {}

void * layer_draw_alloc_vertices(unsigned) { return nullptr; }

void layer_draw_dim_overlay()
{
    if (!g_renderer) {
        return;
    }
    SDL_BlendMode prev;
    SDL_GetRenderDrawBlendMode(g_renderer, &prev);
    SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 0x80);
    SDL_Rect full = { 0, 0, g_logical_w, g_logical_h };
    SDL_RenderFillRect(g_renderer, &full);
    SDL_SetRenderDrawBlendMode(g_renderer, prev);
}

void layer_draw_centered_quad(
    const uint8_t * tex, int tex_w, int tex_h,
    const uint32_t * clut,
    float disp_w, float disp_h,
    bool upload_flag)
{
    const float x = ((float)LAYER_SCREEN_W - disp_w) / 2.0f;
    const float y = ((float)LAYER_SCREEN_H - disp_h) / 2.0f;

    draw_indexed(tex, tex_w, tex_h, clut,
                 0.0f, 0.0f, disp_w, disp_h,
                 x, y, disp_w, disp_h,
                 false, upload_flag);
}

void layer_draw_quad(
    const uint8_t * tex, int tex_w, int tex_h,
    const uint32_t * clut,
    float screen_x, float screen_y,
    float disp_w, float disp_h,
    float u0, float v0, float u1, float v1,
    bool bilinear,
    bool upload_flag)
{
    draw_indexed(tex, tex_w, tex_h, clut,
                 u0, v0, u1, v1,
                 screen_x, screen_y, disp_w, disp_h,
                 bilinear, upload_flag);
}

void layer_draw_rgba_quad(
    const uint32_t * tex, int tex_w, int tex_h,
    float screen_x, float screen_y,
    float disp_w, float disp_h,
    float u0, float v0, float u1, float v1,
    bool bilinear,
    bool upload_flag)
{
    bool is_new = false;
    SDL_Texture * sdl_tex = tex_cache_get(tex, tex_w, tex_h, bilinear, &is_new);
    if (!sdl_tex) {
        return;
    }

    if (is_new || upload_flag) {
        SDL_UpdateTexture(sdl_tex, nullptr, tex, tex_w * 4);
    }

    blit(sdl_tex, u0, v0, u1, v1, screen_x, screen_y, disp_w, disp_h);
}

/* ---- UILayer default draw() ---- */

UILayer::~UILayer() {}

void UILayer::draw()
{
    layer_draw_centered_quad(
        tex_data(), tex_width(), tex_height(),
        clut_data(),
        (float)display_width(), (float)display_height(),
        consume_tex_upload());
}
