#include "imgload.h"
#include "stb_image.h"
#include "stb_image_write.h"

#include <cstdio>
#include <cstring>
#include <vector>

/*
 * Preview image loader and saver, see imgload.h.
 *
 * Load pipeline: stbi_load reads the PNG into RGBA8888 -> convert
 * every pixel to the PSP GE layout (0xAABBGGRR) -> copy or
 * box-shrink into the caller's texture buffer.
 *
 * Save pipeline: convert PSP GE pixels to RGBA8888 -> stbi_write_png
 * writes a compressed PNG file.
 *
 * Runs in the worker thread while the machine is paused (ROM Browser
 * open, state save/load), so a blocking read and a few milliseconds
 * of decoding are fine.
 */

/* Decoding cap: the scratch buffer is w*h uint32s; a preview bigger
 * than this is refused (empty area) instead of eating heap. */
static const int MAX_SRC_DIM = 1024;

/* Convert one RGBA pixel (stb_image output) to the PSP GE layout
 * (memory bytes A B G R = uint32 0xAABBGGRR). */
static uint32_t rgba_to_ge(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return ((uint32_t)a << 24) | ((uint32_t)b << 16)
         | ((uint32_t)g << 8)  |  (uint32_t)r;
}

/* Aspect-preserving box-filter shrink of src (sw x sh) into dst
 * (tw x th); the source rows are contiguous. Integer per-channel
 * sums, no float. */
static void box_shrink(const std::vector<uint32_t> & src, int sw, int sh,
                       uint32_t * dst, int tw, int th)
{
    for (int dy = 0; dy < th; ++dy) {
        const int sy0 = (int)((size_t)dy * sh / th);
        const int sy1 = (int)((size_t)(dy + 1) * sh / th);
        for (int dx = 0; dx < tw; ++dx) {
            const int sx0 = (int)((size_t)dx * sw / tw);
            const int sx1 = (int)((size_t)(dx + 1) * sw / tw);

            unsigned r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int sy = sy0; sy < sy1; ++sy) {
                const uint32_t * row =
                    src.data() + (size_t)sy * (size_t)sw;
                for (int sx = sx0; sx < sx1; ++sx) {
                    const uint32_t p = row[sx];
                    r += p & 0xff;
                    g += (p >> 8) & 0xff;
                    b += (p >> 16) & 0xff;
                    a += (p >> 24) & 0xff;
                    ++n;
                }
            }
            if (n == 0)
                n = 1;
            dst[dy * tw + dx] =
                ((a / n) << 24) | ((b / n) << 16)
              | ((g / n) << 8)  |  (r / n);
        }
    }
}

bool img_load(const char * path,
              uint32_t * dst, int dst_w, int dst_h,
              int * out_w, int * out_h)
{
    int w = 0, h = 0, ch = 0;
    /* stbi_load decodes PNG (and other formats) into RGBA8888.
     * The caller owns the returned buffer; stbi_image_free releases
     * it. A NULL return means "file missing or undecodable". */
    uint8_t * rgba = stbi_load(path, &w, &h, &ch, 4);
    if (rgba == nullptr)
        return false;

    if (w == 0 || h == 0 || w > MAX_SRC_DIM || h > MAX_SRC_DIM) {
        stbi_image_free(rgba);
        return false;
    }

    /* Convert RGBA -> PSP GE 0xAABBGGRR. */
    const size_t total = (size_t)w * (size_t)h;
    std::vector<uint32_t> pixels;
    pixels.resize(total);
    for (size_t i = 0; i < total; ++i) {
        pixels[i] = rgba_to_ge(rgba[i * 4],     rgba[i * 4 + 1],
                               rgba[i * 4 + 2], rgba[i * 4 + 3]);
    }
    stbi_image_free(rgba);

    if (w <= dst_w && h <= dst_h) {
        /* Fits as is: straight copy, the GE upscales the quad. */
        memcpy(dst, pixels.data(), total * sizeof(uint32_t));
        *out_w = w;
        *out_h = h;
        return true;
    }

    /* Too big for the texture: shrink preserving the aspect ratio
     * (scale = min(dst_w/w, dst_h/h)). */
    int tw, th;
    if ((size_t)dst_w * h <= (size_t)dst_h * w) {
        tw = dst_w;
        th = (int)((size_t)h * dst_w / w);
    } else {
        th = dst_h;
        tw = (int)((size_t)w * dst_h / h);
    }
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;

    box_shrink(pixels, w, h, dst, tw, th);
    *out_w = tw;
    *out_h = th;
    return true;
}

bool img_load_from_memory(const uint8_t * data, size_t data_size,
                          uint32_t * dst, int dst_w, int dst_h,
                          int * out_w, int * out_h)
{
    if (data == nullptr || data_size == 0)
        return false;

    int w = 0, h = 0, ch = 0;
    /* stbi_load_from_memory decodes PNG from memory buffer. */
    uint8_t * rgba = stbi_load_from_memory(data, (int)data_size, &w, &h, &ch, 4);
    if (rgba == nullptr)
        return false;

    if (w == 0 || h == 0 || w > MAX_SRC_DIM || h > MAX_SRC_DIM) {
        stbi_image_free(rgba);
        return false;
    }

    /* Convert RGBA -> PSP GE 0xAABBGGRR. */
    const size_t total = (size_t)w * (size_t)h;
    std::vector<uint32_t> pixels;
    pixels.resize(total);
    for (size_t i = 0; i < total; ++i) {
        pixels[i] = rgba_to_ge(rgba[i * 4],     rgba[i * 4 + 1],
                               rgba[i * 4 + 2], rgba[i * 4 + 3]);
    }
    stbi_image_free(rgba);

    if (w <= dst_w && h <= dst_h) {
        /* Fits as is: straight copy, the GE upscales the quad. */
        memcpy(dst, pixels.data(), total * sizeof(uint32_t));
        *out_w = w;
        *out_h = h;
        return true;
    }

    /* Too big for the texture: shrink preserving the aspect ratio. */
    int tw, th;
    if ((size_t)dst_w * h <= (size_t)dst_h * w) {
        tw = dst_w;
        th = (int)((size_t)h * dst_w / w);
    } else {
        th = dst_h;
        tw = (int)((size_t)w * dst_h / h);
    }
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;

    box_shrink(pixels, w, h, dst, tw, th);
    *out_w = tw;
    *out_h = th;
    return true;
}

bool img_save(const char * path, const uint32_t * pixels, int w, int h)
{
    if (pixels == nullptr || w <= 0 || h <= 0)
        return false;

    /* Convert PSP GE 0xAABBGGRR -> RGBA8888 for the PNG writer. */
    const size_t total = (size_t)w * (size_t)h;
    std::vector<uint8_t> rgba(total * 4);
    for (size_t i = 0; i < total; ++i) {
        const uint32_t p = pixels[i];
        rgba[i * 4 + 0] = (uint8_t)( p        & 0xff);  /* R */
        rgba[i * 4 + 1] = (uint8_t)((p >> 8)  & 0xff);  /* G */
        rgba[i * 4 + 2] = (uint8_t)((p >> 16) & 0xff);  /* B */
        rgba[i * 4 + 3] = (uint8_t)((p >> 24) & 0xff);  /* A */
    }

    /* stride = w * 4 bytes (tightly packed RGBA rows). */
    if (!stbi_write_png(path, w, h, 4, rgba.data(), w * 4)) {
        std::remove(path);      /* never leave a truncated screenshot */
        return false;
    }
    return true;
}
