/*
 * coffer — RFC 4648 base64 decoder and shared byte-buffer helpers.
 *
 * The decoder is shared by the kitty graphics (graphics.c), iTerm2
 * inline image (osc_1337.c) and Lottie (lottie.c) handlers. Whitespace
 * is stripped before decoding so payloads can be line-wrapped by the
 * client. cfr_buf_append backs the chunked-upload accumulators used by
 * the kitty and iTerm2 handlers.
 */

#include "coffer_internal.h"

#include <stdlib.h>
#include <string.h>

/* Append n bytes to a growing buffer, doubling capacity as needed. The
 * buffer is not NUL-terminated. Returns 0 on success, -1 on allocation
 * failure. */
int cfr_buf_append(uint8_t **buf, size_t *len, size_t *cap,
                   const void *data, size_t n)
{
    if (*len + n > *cap) {
        size_t ncap = *cap ? *cap : 256;
        while (ncap < *len + n)
            ncap *= 2;
        uint8_t *nb = realloc(*buf, ncap);
        if (!nb)
            return -1;
        *buf = nb;
        *cap = ncap;
    }
    memcpy(*buf + *len, data, n);
    *len += n;
    return 0;
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

uint8_t *cfr_base64_decode(const char *in, size_t in_len, size_t *out_len)
{
    size_t clean_len = 0;
    char *clean = malloc(in_len + 1);
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
    uint8_t *out = malloc(cap);
    if (!out) {
        free(clean);
        return NULL;
    }

    size_t pos = 0;
    for (size_t i = 0; i + 3 < clean_len; i += 4) {
        int a = b64_val(clean[i]);
        int b = b64_val(clean[i + 1]);
        int c = (clean[i + 2] == '=') ? 0 : b64_val(clean[i + 2]);
        int d = (clean[i + 3] == '=') ? 0 : b64_val(clean[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0) {
            free(out);
            free(clean);
            return NULL;
        }
        out[pos++] = (uint8_t)((a << 2) | (b >> 4));
        if (clean[i + 2] != '=')
            out[pos++] = (uint8_t)(((b & 0xf) << 4) | (c >> 2));
        if (clean[i + 3] != '=')
            out[pos++] = (uint8_t)(((c & 0x3) << 6) | d);
    }

    free(clean);
    *out_len = pos;
    return out;
}
