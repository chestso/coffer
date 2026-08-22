/*
 * coffer — OSC 1337 (iTerm2 inline images) handler.
 *
 * Parses OSC 1337 ; File = params : base64-payload and decodes the
 * image to RGBA, storing it via the shared image_store. Supports
 * capability queries and cell-size reporting.
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
    int inline_display;  /* inline=1 → display; inline=0 → download */
    int width;           /* display width (cells, px, or %); 0 = auto */
    int height;          /* display height; 0 = auto */
    int width_is_px;     /* width was in pixels (Npx) */
    int height_is_px;    /* height was in pixels */
    int preserve_aspect; /* preserveAspectRatio=1 (default) */
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
                /* Check for "px" suffix */
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
/* Get the image store from the sixel state (lazy init)              */
/* ------------------------------------------------------------------ */

/* Lazy-init the image store if needed (same pattern as sixel's sx_state).
 * When the sixel state exists, it owns the store. If it doesn't, we
 * create it directly. The sixel state will adopt it when first created. */
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

    /* Parse params */
    ItermParams params;
    parse_params((const char *)body, params_len, &params);

    /* Only handle inline images (download mode = inline=0 is not supported) */
    if (!params.inline_display)
        return;

    /* Decode base64 to raw image bytes */
    size_t raw_len = 0;
    uint8_t *raw = b64_decode(b64, b64_len, &raw_len);
    if (!raw)
        return;

    /* Decode image (PNG, JPEG, etc.) to RGBA */
    int w = 0, h = 0;
    uint8_t *rgba = cfr_image_decode(raw, raw_len, &w, &h);
    free(raw);
    if (!rgba)
        return;

    /* Store the image */
    CfrImgStore *store = get_store(vt);
    if (store) {
        cfr_img_add(vt, store, rgba, w, h, 0, IMG_SRC_ITERM);
    }
    free(rgba);
}

static void handle_capabilities(CfrTerm *vt)
{
    /* Respond with "F" (inline file support) */
    const char *resp = "\x1b]1337;Capabilities=F\x07";
    cfr_emit_bytes(vt, (const uint8_t *)resp, strlen(resp));
}

static void handle_report_cell_size(CfrTerm *vt)
{
    char buf[64];
    int n = snprintf(buf, sizeof(buf),
                     "\x1b]1337;ReportCellSize=%d;%d;1.0\x07",
                     vt->cell_h_px, vt->cell_w_px);
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

    /* Match sub-command prefix */
    if (body_len >= 5 && memcmp(body, "File=", 5) == 0)
        handle_file(vt, body + 5, body_len - 5);
    else if (body_len >= 12 && memcmp(body, "Capabilities", 12) == 0)
        handle_capabilities(vt);
    else if (body_len >= 14 && memcmp(body, "ReportCellSize", 14) == 0)
        handle_report_cell_size(vt);
    /* MultipartFile, FilePart, FileEnd — TODO */
}
