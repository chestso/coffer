/*
 * coffer — image decoding (PNG, JPEG, etc.) via stb_image.
 *
 * Used by OSC 1337 (iTerm2 inline images) and kitty graphics (f=100 PNG).
 * Decodes raw image bytes to RGBA pixel data.
 */

#include "coffer_internal.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ASSERT(x)
#include "stb_image.h"

/* Decode raw image bytes to RGBA. Returns a malloc'd buffer (caller owns)
 * or NULL on failure. Sets *width and *height. */
uint8_t *cfr_image_decode(const uint8_t *data, size_t len,
                          int *width, int *height)
{
    if (!data || len == 0 || !width || !height)
        return NULL;

    int w = 0, h = 0, channels = 0;
    unsigned char *pixels = stbi_load_from_memory(data, (int)len,
                                                  &w, &h, &channels, 4);
    if (!pixels)
        return NULL;

    *width = w;
    *height = h;
    return pixels;
}

/* Decompress a zlib stream (RFC 1950 header) to a malloc'd buffer.
 * Returns NULL on failure; sets *out_len. Used by kitty graphics f=32/24
 * payloads carrying the o=z compression flag. */
uint8_t *cfr_zlib_decompress(const uint8_t *data, size_t len, size_t *out_len)
{
    if (!data || len == 0 || !out_len)
        return NULL;

    int n = 0;
    char *out = stbi_zlib_decode_malloc((const char *)data, (int)len, &n);
    if (!out || n <= 0)
        return NULL;

    *out_len = (size_t)n;
    return (uint8_t *)out;
}
