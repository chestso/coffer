/*
 * coffer — Kitty graphics protocol handler.
 *
 * Parses APC G key=value,...;payload sequences and routes to action
 * handlers. The kitty protocol supports RGBA image transmission with
 * full alpha, placement at the cursor, image IDs, z-index, animations,
 * and composition.
 *
 * This implementation handles the core actions: transmit (a=t/T), place
 * (a=p), query (a=q), and delete (a=d). Animation, compose, relative
 * and virtual placements are future work.
 */

#include "coffer_internal.h"
#include "image_store.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Base64 decoder (shared with osc_1337.c, but duplicated for now)     */
/* ------------------------------------------------------------------ */

static int k_b64_val(char c)
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

static uint8_t *k_b64_decode(const char *in, size_t in_len, size_t *out_len)
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
        int a = k_b64_val(clean[i]);
        int b = k_b64_val(clean[i + 1]);
        int c = (clean[i + 2] == '=') ? 0 : k_b64_val(clean[i + 2]);
        int d = (clean[i + 3] == '=') ? 0 : k_b64_val(clean[i + 3]);
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
    int action;       /* a= (t,T,q,p,d,f,a,c) */
    int image_id;     /* i= (0 = unassigned) */
    int image_num;    /* I= (non-unique) */
    int placement_id; /* p= */
    int format;       /* f= (32=RGBA, 24=RGB, 100=PNG) */
    int width;        /* s= (pixel width) */
    int height;       /* v= (pixel height) */
    int z_index;      /* z= */
    int quiet;        /* q= (0,1,2) */
    int more;         /* m= (chunked: 1=more, 0=last) */
    int delete_what;  /* d= (for a=d) */
    int delete_x;     /* x= (for delete ranges/positions) */
    int delete_y;     /* y= */
    int has_image_id;
    int has_image_num;
    int has_placement_id;
    int has_format;
    int has_width;
    int has_height;
    int has_z_index;
    int has_more;
} KittyParams;

static void k_parse_params(const char *ctrl, size_t len, KittyParams *p)
{
    memset(p, 0, sizeof(*p));

    const char *end = ctrl + len;
    const char *start = ctrl;

    while (start < end) {
        const char *sep = start;
        while (sep < end && *sep != ',' && *sep != ';')
            sep++;

        size_t kv_len = (size_t)(sep - start);
        const char *eq = memchr(start, '=', kv_len);

        if (eq) {
            size_t key_len = (size_t)(eq - start);
            size_t val_len = kv_len - key_len - 1;
            const char *val = eq + 1;
            char val_buf[32];
            size_t copy_len = val_len < sizeof(val_buf) - 1 ? val_len : sizeof(val_buf) - 1;
            memcpy(val_buf, val, copy_len);
            val_buf[copy_len] = '\0';

            /* Single-char keys are most common in kitty */
            if (key_len == 1) {
                switch (start[0]) {
                case 'a':
                    p->action = val_buf[0];
                    break;
                case 'i':
                    p->has_image_id = 1;
                    p->image_id = atoi(val_buf);
                    break;
                case 'I':
                    p->has_image_num = 1;
                    p->image_num = atoi(val_buf);
                    break;
                case 'p':
                    p->has_placement_id = 1;
                    p->placement_id = atoi(val_buf);
                    break;
                case 'f':
                    p->has_format = 1;
                    p->format = atoi(val_buf);
                    break;
                case 's':
                    p->has_width = 1;
                    p->width = atoi(val_buf);
                    break;
                case 'v':
                    p->has_height = 1;
                    p->height = atoi(val_buf);
                    break;
                case 'z':
                    p->has_z_index = 1;
                    p->z_index = atoi(val_buf);
                    break;
                case 'q':
                    p->quiet = atoi(val_buf);
                    break;
                case 'm':
                    p->has_more = 1;
                    p->more = atoi(val_buf);
                    break;
                case 'd':
                    p->delete_what = val_buf[0];
                    break;
                case 'x':
                    p->delete_x = atoi(val_buf);
                    break;
                case 'y':
                    p->delete_y = atoi(val_buf);
                    break;
                }
            }
        }

        start = sep + 1;
    }
}

/* ------------------------------------------------------------------ */
/* Response helper                                                    */
/* ------------------------------------------------------------------ */

static void k_emit_response(CfrTerm *vt, const char *msg)
{
    /* On POSIX: ESC _ G msg ESC \
     * On Windows: ESC ] 5556 ; base64(msg) BEL
     * For now, use the APC form (works on POSIX, and the OSC 5556
     * wrapping is handled by the output callback on Windows). */
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "\x1b_G%s\x1b\\", msg);
    if (n > 0)
        cfr_emit_bytes(vt, (const uint8_t *)buf, (size_t)n);
}

static void k_emit_ok(CfrTerm *vt, int image_id)
{
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "i=%d;OK", image_id);
    if (n > 0)
        k_emit_response(vt, buf);
}

static void k_emit_error(CfrTerm *vt, int image_id, const char *err)
{
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "i=%d;%s", image_id, err);
    if (n > 0)
        k_emit_response(vt, buf);
}

/* ------------------------------------------------------------------ */
/* Image store helper                                                 */
/* ------------------------------------------------------------------ */

static CfrImgStore *k_get_store(CfrTerm *vt)
{
    if (vt->images)
        return vt->images;
    CfrImgStore *st = cfr_img_store_new(vt);
    if (st)
        vt->images = st;
    return st;
}

/* ------------------------------------------------------------------ */
/* Action: transmit (a=t, a=T)                                       */
/* ------------------------------------------------------------------ */

static void k_handle_transmit(CfrTerm *vt, const KittyParams *p,
                              const uint8_t *payload, size_t payload_len)
{
    if (!p->has_format || !p->has_width || !p->has_height) {
        if (!p->quiet)
            k_emit_error(vt, p->image_id, "EINVAL:missing format/dimensions");
        return;
    }

    /* Decode base64 payload */
    size_t raw_len = 0;
    uint8_t *raw = k_b64_decode((const char *)payload, payload_len, &raw_len);
    if (!raw) {
        if (!p->quiet)
            k_emit_error(vt, p->image_id, "EINVAL:base64 decode failed");
        return;
    }

    uint8_t *rgba = NULL;
    int w = p->width, h = p->height;

    if (p->format == 100) {
        /* PNG: decode via stb_image */
        int dw = 0, dh = 0;
        rgba = cfr_image_decode(raw, raw_len, &dw, &dh);
        if (rgba) {
            w = dw;
            h = dh;
        }
    } else if (p->format == 32) {
        /* RGBA: use directly (copy) */
        size_t need = (size_t)w * h * 4;
        if (raw_len < need) {
            free(raw);
            if (!p->quiet)
                k_emit_error(vt, p->image_id, "EINVAL:payload too small");
            return;
        }
        rgba = malloc(need);
        if (rgba)
            memcpy(rgba, raw, need);
    } else if (p->format == 24) {
        /* RGB: expand to RGBA with alpha=255 */
        size_t need = (size_t)w * h * 3;
        if (raw_len < need) {
            free(raw);
            if (!p->quiet)
                k_emit_error(vt, p->image_id, "EINVAL:payload too small");
            return;
        }
        rgba = malloc((size_t)w * h * 4);
        if (rgba) {
            for (int i = 0; i < w * h; i++) {
                rgba[i * 4 + 0] = raw[i * 3 + 0];
                rgba[i * 4 + 1] = raw[i * 3 + 1];
                rgba[i * 4 + 2] = raw[i * 3 + 2];
                rgba[i * 4 + 3] = 255;
            }
        }
    }

    free(raw);

    if (!rgba) {
        if (!p->quiet)
            k_emit_error(vt, p->image_id, "EINVAL:decode failed");
        return;
    }

    /* Store the image */
    CfrImgStore *store = k_get_store(vt);
    if (store) {
        cfr_img_add(vt, store, rgba, w, h, 0, IMG_SRC_KITTY);
        if (!p->quiet)
            k_emit_ok(vt, p->image_id);
    }
    free(rgba);
}

/* ------------------------------------------------------------------ */
/* Action: query (a=q)                                                */
/* ------------------------------------------------------------------ */

static void k_handle_query(CfrTerm *vt, const KittyParams *p)
{
    /* Always respond with OK — we support the protocol */
    if (!p->quiet)
        k_emit_ok(vt, p->image_id);
}

/* ------------------------------------------------------------------ */
/* Action: delete (a=d)                                              */
/* ------------------------------------------------------------------ */

static void k_handle_delete(CfrTerm *vt, const KittyParams *p)
{
    CfrImgStore *store = k_get_store(vt);
    if (!store) {
        if (!p->quiet)
            k_emit_ok(vt, p->image_id);
        return;
    }

    switch (p->delete_what) {
    case 'a':
    case 'A':
        cfr_img_clear_all(vt, store);
        break;
    default:
        /* Other delete modes (i, I, c, p, etc.) — TODO */
        break;
    }

    if (!p->quiet)
        k_emit_ok(vt, p->image_id);
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                           */
/* ------------------------------------------------------------------ */

void cfr_graphics_apc_dispatch(CfrTerm *vt, const uint8_t *buf, size_t len)
{
    if (!buf || len == 0)
        return;

    /* Split control data from payload on ';' */
    const uint8_t *semi = memchr(buf, ';', len);
    size_t ctrl_len;
    const uint8_t *payload;
    size_t payload_len;

    if (semi) {
        ctrl_len = (size_t)(semi - buf);
        payload = semi + 1;
        payload_len = len - ctrl_len - 1;
    } else {
        ctrl_len = len;
        payload = NULL;
        payload_len = 0;
    }

    KittyParams p;
    k_parse_params((const char *)buf, ctrl_len, &p);

    switch (p.action) {
    case 't':
    case 'T':
        k_handle_transmit(vt, &p, payload, payload_len);
        /* a=T also places at cursor — for now just transmit */
        break;
    case 'q':
        k_handle_query(vt, &p);
        break;
    case 'd':
        k_handle_delete(vt, &p);
        break;
    case 'p':
        /* Place: for now, just respond OK (transmit already stored) */
        if (!p.quiet)
            k_emit_ok(vt, p.image_id);
        break;
    default:
        /* Unknown action — respond with error */
        if (!p.quiet)
            k_emit_error(vt, p.image_id, "EINVAL:unknown action");
        break;
    }
}
