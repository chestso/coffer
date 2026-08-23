/*
 * coffer — OSC 1337 (iTerm2 inline images) handler.
 *
 * Parses OSC 1337 ; File = params : base64-payload and decodes the
 * image to RGBA, storing it via the shared image_store. Supports
 * capability queries, cell-size reporting, multipart (chunked)
 * transfer, and width/height scaling.
 *
 * The protocol is OSC-based, so it passes through ConPTY without
 * any carrier workaround (unlike APC-based kitty graphics or
 * DCS-based sixel).
 */

#include "coffer_internal.h"
#include "image_store.h"

#include <string.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Base64 decoder (RFC 4648)                                          */
/* ------------------------------------------------------------------ */

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

static uint8_t *b64_decode(const char *in, size_t in_len, size_t *out_len)
{
    /* Strip whitespace */
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

/* ------------------------------------------------------------------ */
/* Parameter parsing                                                  */
/* ------------------------------------------------------------------ */

typedef struct
{
    int inline_display;
    int width;  /* display width; 0 = auto */
    int height; /* display height; 0 = auto */
    int width_is_px;
    int height_is_px;
    int preserve_aspect;
    int has_width;
    int has_height;
} ItermParams;

static void parse_params(const char *params, size_t len, ItermParams *p)
{
    memset(p, 0, sizeof(*p));
    p->preserve_aspect = 1;
    p->inline_display = 0;

    const char *end = params + len;
    const char *start = params;

    while (start < end) {
        const char *sep = start;
        while (sep < end && *sep != ';')
            sep++;

        size_t kv_len = (size_t)(sep - start);
        const char *eq = memchr(start, '=', kv_len);

        if (eq) {
            size_t key_len = (size_t)(eq - start);
            size_t val_len = kv_len - key_len - 1;
            const char *val = eq + 1;

            if (key_len == 6 && memcmp(start, "inline", 6) == 0) {
                p->inline_display = (val_len >= 1 && val[0] == '1');
            } else if (key_len == 5 && memcmp(start, "width", 5) == 0) {
                p->has_width = 1;
                if (val_len >= 2 && val[val_len - 1] == 'x' &&
                    val[val_len - 2] == 'p') {
                    p->width_is_px = 1;
                    p->width = atoi(val);
                } else {
                    p->width = atoi(val);
                }
            } else if (key_len == 6 && memcmp(start, "height", 6) == 0) {
                p->has_height = 1;
                if (val_len >= 2 && val[val_len - 1] == 'x' &&
                    val[val_len - 2] == 'p') {
                    p->height_is_px = 1;
                    p->height = atoi(val);
                } else {
                    p->height = atoi(val);
                }
            } else if (key_len == 20 &&
                       memcmp(start, "preserveAspectRatio", 20) == 0) {
                p->preserve_aspect = (val_len >= 1 && val[0] == '1');
            }
        }

        start = sep + 1;
    }
}

/* ------------------------------------------------------------------ */
/* Multipart accumulator (single-threaded, static)                    */
/* ------------------------------------------------------------------ */

typedef struct
{
    ItermParams params;
    uint8_t *b64_buf;
    size_t b64_len;
    size_t b64_cap;
    int active;
} MultipartState;

static MultipartState g_multipart = { 0 };

/* ------------------------------------------------------------------ */
/* Image store lazy init                                              */
/* ------------------------------------------------------------------ */

static CfrImgStore *get_store(CfrTerm *vt)
{
    if (vt->images)
        return vt->images;
    CfrImgStore *st = cfr_img_store_new(vt);
    if (st)
        vt->images = st;
    return st;
}

/* ------------------------------------------------------------------ */
/* Process decoded image (shared by single and multipart paths)      */
/* ------------------------------------------------------------------ */

static void process_image(CfrTerm *vt, const uint8_t *raw, size_t raw_len,
                          const ItermParams *params)
{
    if (!params->inline_display)
        return;

    /* Decode image (PNG, JPEG, etc.) to RGBA */
    int w = 0, h = 0;
    uint8_t *rgba = cfr_image_decode(raw, raw_len, &w, &h);
    if (!rgba)
        return;

    /* Compute display dimensions in LOGICAL pixels from the protocol's
     * width/height hints.  vt->cell_w_px is physical (includes
     * content_scale), so we divide to get the logical cell size before
     * multiplying by the cell count.  cfr_img_add and cfr_img_get each
     * apply content_scale once to convert logical→physical. */
    float cscale = vt->content_scale > 0.0f ? vt->content_scale : 1.0f;
    int logical_cw = (int)(vt->cell_w_px / cscale + 0.5f);
    int logical_ch = (int)(vt->cell_h_px / cscale + 0.5f);
    if (logical_cw < 1)
        logical_cw = 1;
    if (logical_ch < 1)
        logical_ch = 1;

    int disp_w = w;
    int disp_h = h;

    if (params->has_width) {
        if (params->width_is_px) {
            disp_w = params->width;
        } else {
            disp_w = params->width * logical_cw;
        }
        if (params->preserve_aspect && disp_w > 0 && w > 0) {
            disp_h = (int)((long)disp_w * h / w);
        }
    }
    if (params->has_height) {
        if (params->height_is_px) {
            disp_h = params->height;
        } else {
            disp_h = params->height * logical_ch;
        }
        if (params->preserve_aspect && disp_h > 0 && h > 0) {
            if (!params->has_width)
                disp_w = (int)((long)disp_h * w / h);
        }
    }

    /* Clamp */
    if (disp_w < 1)
        disp_w = 1;
    if (disp_h < 1)
        disp_h = 1;
    if (disp_w > IMG_MAX_DIM)
        disp_w = IMG_MAX_DIM;
    if (disp_h > IMG_MAX_DIM)
        disp_h = IMG_MAX_DIM;

    /* Store: pixel buffer at native w×h, display at disp_w×disp_h (both logical) */
    CfrImgStore *store = get_store(vt);
    if (store)
        cfr_img_add(vt, store, rgba, w, h, disp_w, disp_h, 0, IMG_SRC_ITERM);
    free(rgba);
}

/* ------------------------------------------------------------------ */
/* Sub-command handlers                                              */
/* ------------------------------------------------------------------ */

static void handle_file(CfrTerm *vt, const uint8_t *body, size_t body_len)
{
    /* Find the ':' separating params from base64 payload */
    const uint8_t *colon = memchr(body, ':', body_len);
    if (!colon)
        return;

    size_t params_len = (size_t)(colon - body);
    size_t b64_len = body_len - params_len - 1;
    const char *b64 = (const char *)(colon + 1);

    ItermParams params;
    parse_params((const char *)body, params_len, &params);

    size_t raw_len = 0;
    uint8_t *raw = b64_decode(b64, b64_len, &raw_len);
    if (!raw)
        return;

    process_image(vt, raw, raw_len, &params);
    free(raw);
}

static void handle_multipart_start(CfrTerm *vt, const uint8_t *body, size_t body_len)
{
    (void)vt;
    /* Reset accumulator and parse params */
    free(g_multipart.b64_buf);
    memset(&g_multipart, 0, sizeof(g_multipart));
    parse_params((const char *)body, body_len, &g_multipart.params);
    g_multipart.active = 1;
}

static void handle_multipart_part(CfrTerm *vt, const uint8_t *body, size_t body_len)
{
    (void)vt;
    if (!g_multipart.active)
        return;

    /* Append base64 chunk to the accumulator */
    if (g_multipart.b64_len + body_len > g_multipart.b64_cap) {
        size_t ncap = g_multipart.b64_cap ? g_multipart.b64_cap * 2 : 256;
        while (ncap < g_multipart.b64_len + body_len)
            ncap *= 2;
        uint8_t *nb = realloc(g_multipart.b64_buf, ncap);
        if (!nb)
            return;
        g_multipart.b64_buf = nb;
        g_multipart.b64_cap = ncap;
    }
    memcpy(g_multipart.b64_buf + g_multipart.b64_len, body, body_len);
    g_multipart.b64_len += body_len;
}

static void handle_multipart_end(CfrTerm *vt)
{
    if (!g_multipart.active || !g_multipart.b64_buf)
        goto cleanup;

    /* Decode accumulated base64 to raw image bytes */
    size_t raw_len = 0;
    uint8_t *raw = b64_decode((const char *)g_multipart.b64_buf,
                              g_multipart.b64_len, &raw_len);
    if (raw) {
        process_image(vt, raw, raw_len, &g_multipart.params);
        free(raw);
    }

cleanup:
    free(g_multipart.b64_buf);
    memset(&g_multipart, 0, sizeof(g_multipart));
}

static void handle_capabilities(CfrTerm *vt)
{
    const char *resp = "\x1b]1337;Capabilities=F\x07";
    cfr_emit_bytes(vt, (const uint8_t *)resp, strlen(resp));
}

static void handle_report_cell_size(CfrTerm *vt)
{
    /* Report LOGICAL cell pixels (physical / content_scale) so chafa
     * scales images to logical dimensions.  cfr_img_get then converts
     * to physical for the renderer.  Reporting physical here would
     * make chafa pack too many pixels per cell, and process_image's
     * logical cell conversion would then shrink the display. */
    float cscale = vt->content_scale > 0.0f ? vt->content_scale : 1.0f;
    int logical_cw = (int)(vt->cell_w_px / cscale + 0.5f);
    int logical_ch = (int)(vt->cell_h_px / cscale + 0.5f);
    if (logical_cw < 1)
        logical_cw = 1;
    if (logical_ch < 1)
        logical_ch = 1;
    char buf[64];
    int n = snprintf(buf, sizeof(buf),
                     "\x1b]1337;ReportCellSize=%d;%d;1.0\x07",
                     logical_ch, logical_cw);
    if (n > 0)
        cfr_emit_bytes(vt, (const uint8_t *)buf, (size_t)n);
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                           */
/* ------------------------------------------------------------------ */

void cfr_osc_1337_dispatch(CfrTerm *vt, const uint8_t *body, size_t body_len)
{
    if (!body || body_len == 0)
        return;

    if (body_len >= 5 && memcmp(body, "File=", 5) == 0)
        handle_file(vt, body + 5, body_len - 5);
    else if (body_len >= 14 && memcmp(body, "MultipartFile=", 14) == 0)
        handle_multipart_start(vt, body + 14, body_len - 14);
    else if (body_len >= 9 && memcmp(body, "FilePart=", 9) == 0)
        handle_multipart_part(vt, body + 9, body_len - 9);
    else if (body_len >= 7 && memcmp(body, "FileEnd", 7) == 0)
        handle_multipart_end(vt);
    else if (body_len >= 12 && memcmp(body, "Capabilities", 12) == 0)
        handle_capabilities(vt);
    else if (body_len >= 14 && memcmp(body, "ReportCellSize", 14) == 0)
        handle_report_cell_size(vt);
}
