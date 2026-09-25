/*
 * Single compilation unit for the stb_image / stb_image_write
 * implementations. Every other translation unit sees only the
 * thin img_load / img_save API declared in imgload.h; the heavy
 * decoder/encoder code lives here and nowhere else.
 *
 * PSP-specific trimming:
 *   STBI_ONLY_PNG      - compile only the PNG decoder (no JPEG/BMP/
 *                         PSD/GIF/HDR/PIC/PNM/TGA), keeps the binary
 *                         small;
 *   STBI_NO_HDR        - skip HDR/RGBE support (float pixels unused);
 *   STBI_NO_LINEAR     - skip linear-to-sRGB conversion helpers;
 *   STBI_NO_STDIO      - not defined: we need FILE-based loading;
 *   STBIW_NO_*         - not needed, the write side stays compact.
 */

#define STBI_ONLY_PNG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
