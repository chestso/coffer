/*
 * coffer — image decoding (PNG, JPEG, etc.) via stb_image, plus a
 * baseline-uncompressed TIFF reader for iTerm2 inline-image payloads.
 *
 * Used by OSC 1337 (iTerm2 inline images) and kitty graphics (f=100 PNG).
 *
 * chafa's `-f iterm2` output carries the pixels in a minimal,
 * uncompressed, packed-4-channel TIFF (single strip, top-down rows).
 * stb_image does not load TIFF, so those payloads decode here by hand.
 */

#include "coffer_internal.h"

#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ASSERT(x)
#include "stb_image.h"

/* ------------------------------------------------------------------ */
/* Minimal baseline-uncompressed TIFF reader                          */
/*                                                                    */
/* chafa's `-f iterm2` emits pixels as a TIFF blob (not PNG, as the   */
/* OSC 1337 spec assumes). stb_image has no TIFF loader, so we decode */
/* the specific layout chafa produces: little/big-endian, one strip,  */
/* uncompressed, chunky, 8 bits/sample, photometric RGB with an alpha */
/* extra sample (RGBA). Rows are stored top-down (orientation 1).     */
/* ------------------------------------------------------------------ */

static uint16_t tiff_u16(const uint8_t *d, int le, size_t o)
{
    if (le)
        return (uint16_t)(d[o] | (d[o + 1] << 8));
    return (uint16_t)((d[o] << 8) | d[o + 1]);
}

static uint32_t tiff_u32(const uint8_t *d, int le, size_t o)
{
    if (le)
        return (uint32_t)d[o] | ((uint32_t)d[o + 1] << 8) |
               ((uint32_t)d[o + 2] << 16) | ((uint32_t)d[o + 3] << 24);
    return ((uint32_t)d[o] << 24) | ((uint32_t)d[o + 1] << 16) |
           ((uint32_t)d[o + 2] << 8) | (uint32_t)d[o + 3];
}

/* Read a SHORT/LONG field value. `vp` points at the 4-byte value slot;
 * if the payload doesn't fit inline it returns the first element from the
 * referenced offset. Returns 0 on out-of-range. */
static uint32_t tiff_field(const uint8_t *d, size_t len, int le,
                           uint16_t type, uint32_t count, const uint8_t *vp)
{
    uint32_t esize = (type == 3) ? 2u : 4u;
    uint32_t total = esize * count;

    if (total <= 4) {
        size_t o = (size_t)(vp - d);
        return (type == 3) ? tiff_u16(d, le, o) : tiff_u32(d, le, o);
    }

    uint32_t off = tiff_u32(d, le, (size_t)(vp - d));
    if ((size_t)off + esize > len)
        return 0;
    return (type == 3) ? tiff_u16(d, le, off) : tiff_u32(d, le, off);
}

/* Read `count` SHORTs (inline or offset) into `out`. Returns 0 on error. */
static int tiff_shorts(const uint8_t *d, size_t len, int le,
                       uint32_t count, const uint8_t *vp, uint16_t *out)
{
    const uint8_t *src;
    if (count * 2u <= 4) {
        src = vp;
    } else {
        uint32_t off = tiff_u32(d, le, (size_t)(vp - d));
        if ((size_t)off + count * 2u > len)
            return -1;
        src = d + off;
    }
    for (uint32_t i = 0; i < count; i++)
        out[i] = tiff_u16(d, le, (size_t)(src - d) + i * 2u);
    return 0;
}

static uint8_t *cfr_tiff_load(const uint8_t *d, size_t len, int *out_w, int *out_h)
{
    if (len < 8 || !d || !out_w || !out_h)
        return NULL;

    int le;
    if (d[0] == 'I' && d[1] == 'I')
        le = 1;
    else if (d[0] == 'M' && d[1] == 'M')
        le = 0;
    else
        return NULL;

    if (tiff_u16(d, le, 2) != 42)
        return NULL;

    uint32_t ifd = tiff_u32(d, le, 4);
    if ((size_t)ifd + 2 > len)
        return NULL;
    uint16_t n = tiff_u16(d, le, ifd);
    if ((size_t)ifd + 2 + (size_t)n * 12 > len)
        return NULL;

    int width = 0, height = 0, spp = 1, photo = 1, compression = 1, planar = 1;
    int bps = 8;
    uint32_t strip_off = 0, strip_bytes = 0;

    for (uint32_t i = 0; i < n; i++) {
        size_t e = (size_t)ifd + 2 + (size_t)i * 12;
        uint16_t tag = tiff_u16(d, le, e);
        uint16_t type = tiff_u16(d, le, e + 2);
        uint32_t count = tiff_u32(d, le, e + 4);
        const uint8_t *vp = d + e + 8;

        switch (tag) {
        case 256:
            width = (int)tiff_field(d, len, le, type, count, vp);
            break;
        case 257:
            height = (int)tiff_field(d, len, le, type, count, vp);
            break;
        case 258:
        {
            uint16_t samples[4] = { 0 };
            if (count > 4 ||
                tiff_shorts(d, len, le, count, vp, samples) < 0)
                return NULL;
            for (uint32_t s = 0; s < count; s++) {
                if (samples[s] != 8)
                    return NULL;
            }
            bps = 8;
            break;
        }
        case 259:
            compression = (int)tiff_field(d, len, le, type, count, vp);
            break;
        case 262:
            photo = (int)tiff_field(d, len, le, type, count, vp);
            break;
        case 273:
            strip_off = tiff_field(d, len, le, type, count, vp);
            break;
        case 277:
            spp = (int)tiff_field(d, len, le, type, count, vp);
            break;
        case 279:
            strip_bytes = tiff_field(d, len, le, type, count, vp);
            break;
        case 284:
            planar = (int)tiff_field(d, len, le, type, count, vp);
            break;
        default:
            break;
        }
    }

    if (width <= 0 || height <= 0 || spp != 4 || photo != 2 ||
        compression != 1 || planar != 1 || bps != 8)
        return NULL;

    uint64_t npix = (uint64_t)width * (uint64_t)height;
    if (npix == 0 || npix > IMG_MAX_DIM * (uint64_t)IMG_MAX_DIM)
        return NULL;
    uint64_t need = npix * 4u;
    if (strip_bytes < need)
        return NULL;
    if ((size_t)strip_off + (size_t)need > len)
        return NULL;

    uint8_t *rgba = malloc((size_t)need);
    if (!rgba)
        return NULL;

    memcpy(rgba, d + strip_off, (size_t)need);

    *out_w = width;
    *out_h = height;
    return rgba;
}

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
    if (pixels) {
        *width = w;
        *height = h;
        return pixels;
    }

    /* Fall back to the TIFF reader (chafa -f iterm2 emits TIFF). */
    uint8_t *tiff = cfr_tiff_load(data, len, &w, &h);
    if (!tiff)
        return NULL;
    *width = w;
    *height = h;
    return tiff;
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
