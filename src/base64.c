/*
 * coffer — RFC 4648 base64 decoder and shared byte-buffer helpers.
 *
 * The decoder is shared by the kitty graphics (graphics.c), iTerm2
 * inline image (osc_1337.c) and Lottie (lottie.c) handlers. Whitespace
 * is stripped before decoding so payloads can be line-wrapped by the
 * client. cfr_buf_append backs the chunked-upload accumulators used by
 * the kitty and iTerm2 handlers.
 *
 * Everything goes through the terminal's allocator, so a custom
 * CfrAllocator sees the same traffic the rest of the engine does.
 */

#include "coffer_internal.h"

#include <string.h>

/* Append n bytes to a growing buffer, doubling capacity as needed. One
 * byte is always kept past the appended data and set to '\0', so a
 * caller can treat the accumulator as a C string without reserving the
 * terminator itself (the payload is still counted in *len, which is
 * what cfr_base64_decode is given). Returns 0 on success, -1 on
 * allocation failure. */
int cfr_buf_append(CfrTerm *vt, uint8_t **buf, size_t *len, size_t *cap,
                   const void *data, size_t n)
{
    /* Zero-byte append is a no-op. Callers may pass a NULL buffer with
     * n=0 (kitty control-only chunks carry no payload at all); growing
     * the buffer would leave it NULL and the memcpy below would be UB
     * with a NULL destination. */
    if (n == 0)
        return 0;
    if (*len + n + 1 > *cap) {
        size_t ncap = *cap ? *cap : 256;
        while (ncap < *len + n + 1)
            ncap *= 2;
        uint8_t *nb = cfr_realloc(vt, *buf, ncap);
        if (!nb)
            return -1;
        *buf = nb;
        *cap = ncap;
    }
    memcpy(*buf + *len, data, n);
    *len += n;
    (*buf)[*len] = '\0';
    return 0;
}

char *cfr_base64_encode(CfrTerm *vt, const uint8_t *in, size_t in_len,
                        size_t *out_len)
{
    static const char chars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t cap = ((in_len + 2) / 3) * 4 + 1;
    char *out = cfr_alloc(vt, cap);
    if (!out)
        return NULL;
    size_t n = 0;
    for (size_t i = 0; i < in_len; i += 3) {
        uint32_t a = in[i];
        uint32_t b = i + 1 < in_len ? in[i + 1] : 0;
        uint32_t c = i + 2 < in_len ? in[i + 2] : 0;
        uint32_t triple = (a << 16) | (b << 8) | c;
        out[n++] = chars[(triple >> 18) & 0x3f];
        out[n++] = chars[(triple >> 12) & 0x3f];
        out[n++] = i + 1 < in_len ? chars[(triple >> 6) & 0x3f] : '=';
        out[n++] = i + 2 < in_len ? chars[triple & 0x3f] : '=';
    }
    out[n] = '\0';
    if (out_len)
        *out_len = n;
    return out;
}

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

uint8_t *cfr_base64_decode(CfrTerm *vt, const char *in, size_t in_len,
                           size_t *out_len)
{
    size_t clean_len = 0;
    char *clean = cfr_alloc(vt, in_len + 1);
    if (!clean)
        return NULL;
    for (size_t i = 0; i < in_len; i++) {
        char c = in[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            continue;
        clean[clean_len++] = c;
    }
    clean[clean_len] = '\0';

    size_t cap = (clean_len / 4) * 3 + 3;
    uint8_t *out = cfr_alloc(vt, cap);
    if (!out) {
        cfr_dealloc(vt, clean);
        return NULL;
    }

    size_t pos = 0;
    for (size_t i = 0; i + 3 < clean_len; i += 4) {
        int a = b64_val(clean[i]);
        int b = b64_val(clean[i + 1]);
        int c = (clean[i + 2] == '=') ? 0 : b64_val(clean[i + 2]);
        int d = (clean[i + 3] == '=') ? 0 : b64_val(clean[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0) {
            cfr_dealloc(vt, out);
            cfr_dealloc(vt, clean);
            return NULL;
        }
        out[pos++] = (uint8_t)((a << 2) | (b >> 4));
        if (clean[i + 2] != '=')
            out[pos++] = (uint8_t)(((b & 0xf) << 4) | (c >> 2));
        if (clean[i + 3] != '=')
            out[pos++] = (uint8_t)(((c & 0x3) << 6) | d);
    }

    cfr_dealloc(vt, clean);
    *out_len = pos;
    return out;
}
