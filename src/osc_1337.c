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

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Parameter parsing                                                  */
/* ------------------------------------------------------------------ */

static void parse_params(const char *params, size_t len, CfrItermParams *p)
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
/* Multipart accumulator (per-terminal state in CfrTerm)             */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Process decoded image (shared by single and multipart paths)      */
/* ------------------------------------------------------------------ */

static void process_image(CfrTerm *vt, const uint8_t *raw, size_t raw_len,
                          const CfrItermParams *params)
{
    if (!params->inline_display)
        return;

    /* Decode image (PNG, JPEG, etc.) to RGBA */
    int w = 0, h = 0;
    uint8_t *rgba = cfr_image_decode(raw, raw_len, &w, &h);
    if (!rgba)
        return;

    /* Display dimensions in DEVICE pixels from the protocol's
     * width/height hints: a cell-count hint multiplies the physical
     * cell, a px hint is device pixels as-is. The store draws them
     * 1:1 — no scale conversion anywhere in the pipeline. */
    int disp_w = w;
    int disp_h = h;

    if (params->has_width) {
        if (params->width_is_px) {
            disp_w = params->width;
        } else {
            disp_w = params->width * vt->cell_w_px;
        }
        if (params->preserve_aspect && disp_w > 0 && w > 0) {
            disp_h = (int)((long)disp_w * h / w);
        }
    }
    if (params->has_height) {
        if (params->height_is_px) {
            disp_h = params->height;
        } else {
            disp_h = params->height * vt->cell_h_px;
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

    /* Store: pixel buffer at native w×h, display at disp_w×disp_h
     * (device pixels, drawn 1:1). */
    CfrImgStore *store = cfr_img_get_store(vt);
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

    CfrItermParams params;
    parse_params((const char *)body, params_len, &params);

    size_t raw_len = 0;
    uint8_t *raw = cfr_base64_decode(vt, b64, b64_len, &raw_len);
    if (!raw)
        return;

    process_image(vt, raw, raw_len, &params);
    cfr_dealloc(vt, raw);
}

static void handle_multipart_start(CfrTerm *vt, const uint8_t *body, size_t body_len)
{
    /* Reset accumulator and parse params */
    cfr_dealloc(vt, vt->iterm_mp.b64_buf);
    memset(&vt->iterm_mp, 0, sizeof(vt->iterm_mp));
    parse_params((const char *)body, body_len, &vt->iterm_mp.params);
    vt->iterm_mp.active = 1;
}

static void handle_multipart_part(CfrTerm *vt, const uint8_t *body, size_t body_len)
{
    if (!vt->iterm_mp.active)
        return;

    /* Append base64 chunk to the accumulator */
    if (cfr_buf_append(vt, &vt->iterm_mp.b64_buf, &vt->iterm_mp.b64_len,
                       &vt->iterm_mp.b64_cap, body, body_len) != 0)
        return;
}

static void handle_multipart_end(CfrTerm *vt)
{
    if (!vt->iterm_mp.active || !vt->iterm_mp.b64_buf)
        goto cleanup;

    /* Decode accumulated base64 to raw image bytes */
    size_t raw_len = 0;
    uint8_t *raw = cfr_base64_decode(vt, (const char *)vt->iterm_mp.b64_buf,
                                     vt->iterm_mp.b64_len, &raw_len);
    if (raw) {
        process_image(vt, raw, raw_len, &vt->iterm_mp.params);
        cfr_dealloc(vt, raw);
    }

cleanup:
    cfr_dealloc(vt, vt->iterm_mp.b64_buf);
    memset(&vt->iterm_mp, 0, sizeof(vt->iterm_mp));
}

static void handle_capabilities(CfrTerm *vt)
{
    const char *resp = "\x1b]1337;Capabilities=F\x07";
    cfr_emit_bytes(vt, (const uint8_t *)resp, strlen(resp));
}

static void handle_report_cell_size(CfrTerm *vt)
{
    /* iTerm2's format: ReportCellSize=[height];[width];[scale] — height
     * and width in POINTS (logical pixels), scale the number of device
     * pixels per point. This is the ONE client-facing report that defines
     * its own unit, kept for iTerm2 compatibility: every other pixel
     * figure the engine reports is device pixels. The two agree —
     * physical cell (CSI 16 t) == these points × scale. */
    int logical_cw = 1;
    int logical_ch = 1;
    cfr_cell_logical_px(vt, &logical_cw, &logical_ch);
    float scale = vt->content_scale > 0.0f ? vt->content_scale : 1.0f;
    char buf[64];
    int n = snprintf(buf, sizeof(buf),
                     "\x1b]1337;ReportCellSize=%d;%d;%.1f\x07",
                     logical_ch, logical_cw, scale);
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
