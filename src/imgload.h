#pragma once

#include <inttypes.h>
#include <stddef.h>

/*
 * Preview image loader and saver (PNG via stb_image / stb_image_write).
 *
 * Preview files live next to the ROM with the same base name
 * ("riseout.rom" -> "riseout.png") and as state screenshots
 * (SAVES/<rom>/stateN.png). The format is PNG: typically 10-20x
 * smaller than the old uncompressed TGA, which matters a lot on
 * PSP memory cards and load times.
 *
 * The pixels are converted straight into the PSP GE layout (memory
 * bytes A B G R, i.e. uint32 0xAABBGGRR of GU_PSM_8888) and written
 * into a caller-owned power-of-two texture buffer. Images larger
 * than the destination are shrunk with a box filter, aspect ratio
 * always preserved; smaller images are copied 1:1 (the GE does any
 * upscaling when the quad is drawn).
 */

/* Load path into dst (dst_w x dst_h pixels, PSP 8888 layout).
 * Returns true when a preview was decoded: *out_w and *out_h hold
 * the stored image size (<= dst_w/dst_h, aspect preserved). Returns
 * false on any missing/undecodable/oversized file — the dst content
 * is undefined then and the caller must treat it as "no preview". */
bool img_load(const char * path,
              uint32_t * dst, int dst_w, int dst_h,
              int * out_w, int * out_h);

/* Load PNG from memory buffer into dst (dst_w x dst_h pixels, PSP 8888 layout).
 * Returns true when an image was decoded: *out_w and *out_h hold
 * the stored image size (<= dst_w/dst_h, aspect preserved). Returns
 * false on any undecodable/oversized image — the dst content
 * is undefined then and the caller must treat it as "no image". */
bool img_load_from_memory(const uint8_t * data, size_t data_size,
                          uint32_t * dst, int dst_w, int dst_h,
                          int * out_w, int * out_h);

/* Save pixels (w x h, PSP 8888 layout 0xAABBGGRR) as a PNG file.
 * Used for the save-state screenshots and ROM previews. Returns true
 * when the file was written. */
bool img_save(const char * path, const uint32_t * pixels, int w, int h);
