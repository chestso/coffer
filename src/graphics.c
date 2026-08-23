/*
 * coffer — Kitty graphics protocol handler.
 *
 * Parses APC G key=value,...;payload sequences and routes to action
 * handlers. The kitty protocol supports RGBA image transmission with
 * full alpha, placement at the cursor, image IDs, z-index, animations,
 * and composition.
 *
 * This implementation handles: transmit (a=t/T), place (a=p), query
 * (a=q), delete (a=d), animation frames (a=f), animation control (a=a),
 * compose (a=c, no-op), chunked transfer (m=1/0), zlib compression
 * (o=z), cursor advance (c=/r=), virtual placements (U=1), and relative
 * placements (P=/Q= with x=/y= offsets).
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
    int col;          /* x= (placement column offset) */
    int row;          /* y= (placement row offset) */
    int place_cols;   /* w= (placement cell width) */
    int place_rows;   /* h= (placement cell height) */
    int adv_cols;     /* c= (cursor advance columns) */
    int adv_rows;     /* r= (cursor advance rows) */
    int compression;  /* o= (z=zlib, else none) */
    int virtual;      /* U= (1 = virtual placement, no cursor move) */
    int parent_place; /* P= (parent placement id) */
    int parent_img;   /* Q= (parent image id) */
    int has_image_id;
    int has_image_num;
    int has_placement_id;
    int has_format;
    int has_width;
    int has_height;
    int has_z_index;
    int has_more;
    int has_col;
    int has_row;
    int has_place_cols;
    int has_place_rows;
    int has_adv_cols;
    int has_adv_rows;
    int has_compression;
    int has_virtual;
    int has_parent_place;
    int has_parent_img;
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
                    p->has_col = 1;
                    p->col = atoi(val_buf);
                    p->delete_x = p->col;
                    break;
                case 'y':
                    p->has_row = 1;
                    p->row = atoi(val_buf);
                    p->delete_y = p->row;
                    break;
                case 'w':
                    p->has_place_cols = 1;
                    p->place_cols = atoi(val_buf);
                    break;
                case 'h':
                    p->has_place_rows = 1;
                    p->place_rows = atoi(val_buf);
                    break;
                case 'c':
                    p->has_adv_cols = 1;
                    p->adv_cols = atoi(val_buf);
                    break;
                case 'r':
                    p->has_adv_rows = 1;
                    p->adv_rows = atoi(val_buf);
                    break;
                case 'U':
                    p->has_virtual = 1;
                    p->virtual = atoi(val_buf);
                    break;
                case 'P':
                    p->has_parent_place = 1;
                    p->parent_place = atoi(val_buf);
                    break;
                case 'Q':
                    p->has_parent_img = 1;
                    p->parent_img = atoi(val_buf);
                    break;
                case 'o':
                    p->has_compression = 1;
                    p->compression = val_buf[0]; /* 'z' = zlib */
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
     * On Windows: ESC ] 5556 ; G msg BEL
     *
     * ConPTY strips APC (ESC _) in both directions, so kitty graphics
     * responses must leave via the OSC 5556 carrier. The payload after
     * the semicolon is the raw "G" + msg (NOT base64 — kitty's OSC
     * carrier is raw, unlike Lottie's base64-encoded carrier). */
    char buf[288];
#ifdef _WIN32
    int n = snprintf(buf, sizeof(buf), "\x1b]5556;G%s\x07", msg);
#else
    int n = snprintf(buf, sizeof(buf), "\x1b_G%s\x1b\\", msg);
#endif
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
/* Action: transmit (a=t, a=T, a=f frame)                            */
/* ------------------------------------------------------------------ */

/* Chunked-upload accumulator for kitty transmit (m=1 ... m=0). */
typedef struct
{
    char *b64;
    size_t len;
    size_t cap;
    int active;
} KChunk;

static KChunk g_chunk = { 0 };

/* Decode a base64 payload to RGBA (f=32/24/100). Returns malloc'd RGBA
 * and sets *w and *h. On failure returns NULL. If decompress is true, the
 * base64-decoded bytes are first inflated as a zlib stream (o=z). */
static uint8_t *k_decode_rgba(const uint8_t *payload, size_t payload_len,
                              int format, int *w, int *h, bool decompress)
{
    size_t raw_len = 0;
    uint8_t *raw = k_b64_decode((const char *)payload, payload_len, &raw_len);
    if (!raw)
        return NULL;

    uint8_t *data = raw;
    size_t data_len = raw_len;

    if (decompress) {
        size_t out_len = 0;
        uint8_t *inf = cfr_zlib_decompress(raw, raw_len, &out_len);
        free(raw);
        if (!inf)
            return NULL;
        data = inf;
        data_len = out_len;
    }

    uint8_t *rgba = NULL;

    if (format == 100) {
        int dw = 0, dh = 0;
        rgba = cfr_image_decode(data, data_len, &dw, &dh);
        if (rgba) {
            *w = dw;
            *h = dh;
        }
    } else if (format == 32) {
        size_t need = (size_t)*w * *h * 4;
        if (data_len < need) {
            free(data);
            return NULL;
        }
        rgba = malloc(need);
        if (rgba)
            memcpy(rgba, data, need);
    } else if (format == 24) {
        size_t need = (size_t)*w * *h * 3;
        if (data_len < need) {
            free(data);
            return NULL;
        }
        rgba = malloc((size_t)*w * *h * 4);
        if (rgba) {
            for (int i = 0; i < *w * *h; i++) {
                rgba[i * 4 + 0] = data[i * 3 + 0];
                rgba[i * 4 + 1] = data[i * 3 + 1];
                rgba[i * 4 + 2] = data[i * 3 + 2];
                rgba[i * 4 + 3] = 255;
            }
        }
    }
    free(data);
    return rgba;
}

/* Store a fully decoded image under the client id, or replace in place
 * (a=f animation frame) when the id already exists. */
static void k_store_image(CfrTerm *vt, const KittyParams *p,
                          uint8_t *rgba, int w, int h)
{
    CfrImgStore *store = k_get_store(vt);
    if (!store)
        return;

    uint64_t id = (uint64_t)(p->has_image_id ? p->image_id : 0);
    int idx = cfr_img_find_by_id(store, id);
    if (idx >= 0) {
        cfr_img_replace(vt, store, idx, rgba, w, h);
    } else {
        cfr_img_add_named(vt, store, id, rgba, w, h, 0, IMG_SRC_KITTY);
    }
    if (!p->quiet)
        k_emit_ok(vt, p->image_id);
}

static void k_handle_transmit(CfrTerm *vt, const KittyParams *p,
                              const uint8_t *payload, size_t payload_len,
                              bool is_frame, bool place_at_cursor)
{
    if (!p->has_format || !p->has_width || !p->has_height) {
        if (!p->quiet)
            k_emit_error(vt, p->image_id, "EINVAL:missing format/dimensions");
        return;
    }

    /* Chunked transfer: m=1 accumulates base64 into a static buffer;
     * m=0 (or no m flag) finalizes and stores. */
    if (p->has_more && p->more == 1) {
        size_t need = g_chunk.len + payload_len + 1;
        if (need > g_chunk.cap) {
            size_t ncap = g_chunk.cap ? g_chunk.cap : 256;
            while (ncap < need)
                ncap *= 2;
            char *nb = realloc(g_chunk.b64, ncap);
            if (!nb)
                return;
            g_chunk.b64 = nb;
            g_chunk.cap = ncap;
        }
        memcpy(g_chunk.b64 + g_chunk.len, payload, payload_len);
        g_chunk.len += payload_len;
        g_chunk.b64[g_chunk.len] = '\0';
        g_chunk.active = 1;
        return;
    }

    /* Final chunk (m=0 or single-shot). */
    const uint8_t *data = payload;
    size_t data_len = payload_len;
    char *tmp_b64 = NULL;

    if (g_chunk.active) {
        size_t need = g_chunk.len + payload_len + 1;
        tmp_b64 = malloc(need);
        if (!tmp_b64)
            return;
        memcpy(tmp_b64, g_chunk.b64, g_chunk.len);
        memcpy(tmp_b64 + g_chunk.len, payload, payload_len);
        tmp_b64[g_chunk.len + payload_len] = '\0';
        data = (const uint8_t *)tmp_b64;
        data_len = g_chunk.len + payload_len;

        free(g_chunk.b64);
        memset(&g_chunk, 0, sizeof(g_chunk));
    }

    int w = p->width, h = p->height;
    bool decompress = p->has_compression && p->compression == 'z';
    uint8_t *rgba = k_decode_rgba(data, data_len, p->format, &w, &h,
                                  decompress);
    free(tmp_b64);

    if (!rgba) {
        if (!p->quiet)
            k_emit_error(vt, p->image_id, "EINVAL:decode failed");
        return;
    }

    (void)is_frame;
    k_store_image(vt, p, rgba, w, h);

    /* a=T also places at the cursor. */
    if (place_at_cursor) {
        CfrImgStore *store = k_get_store(vt);
        uint64_t id = (uint64_t)(p->has_image_id ? p->image_id : 0);
        int cell_h = vt->cell_h_px > 0 ? vt->cell_h_px : 1;
        int cell_w = vt->cell_w_px > 0 ? vt->cell_w_px : 1;
        float scale = vt->content_scale > 0.0f ? vt->content_scale : 1.0f;
        int cols = (((int)(w * scale) + cell_w - 1) / cell_w);
        int rows = (((int)(h * scale) + cell_h - 1) / cell_h);
        if (cols < 1)
            cols = 1;
        if (rows < 1)
            rows = 1;
        cfr_img_add_placement(vt, store, id, vt->sixel_abs_top + vt->cursor.row,
                              vt->cursor.col, rows, cols, 0, 255,
                              p->has_z_index ? p->z_index : 0);
    }
    free(rgba);
}

/* ------------------------------------------------------------------ */
/* Action: query (a=q)                                                */
/* ------------------------------------------------------------------ */

static void k_handle_query(CfrTerm *vt, const KittyParams *p)
{
    if (p->quiet)
        return;

    /* a=q,i=0 is the capability query; reply with our supported feature
     * flags (kitty protocol version + a subset of the capability set). */
    if (p->has_image_id && p->image_id == 0) {
        char buf[128];
        int n = snprintf(buf, sizeof(buf),
                         "i=0;OK;flags=0x00000103");
        if (n > 0) {
            /* Reject non-null terminated — k_emit_response wraps it. */
            k_emit_response(vt, buf);
        }
        return;
    }

    /* Always respond with OK — we support the protocol */
    k_emit_ok(vt, p->image_id);
}

/* ------------------------------------------------------------------ */
/* Action: place (a=p)                                                */
/* ------------------------------------------------------------------ */

static void k_handle_place(CfrTerm *vt, const KittyParams *p)
{
    CfrImgStore *store = k_get_store(vt);
    if (!store)
        return;

    uint64_t image_id = (uint64_t)(p->has_image_id ? p->image_id : 0);
    int img_idx = cfr_img_find_by_id(store, image_id);
    if (img_idx < 0) {
        if (!p->quiet)
            k_emit_error(vt, p->image_id, "ENOENT:unknown image id");
        return;
    }
    CfrImg *img = &store->imgs[img_idx];

    int col = p->has_col ? p->col : vt->cursor.col;
    int row = p->has_row ? p->row : vt->cursor.row;

    /* Relative placement: when P= (parent placement) or Q= (parent image)
     * is given, x=/y= are offsets relative to the parent's top-left,
     * not absolute cell coordinates. */
    uint64_t rel_place = 0, rel_img = 0;
    if (p->has_parent_place) {
        rel_place = (uint64_t)p->parent_place;
        /* Find the parent placement to anchor against. */
        for (int i = 0; i < store->place_count; i++) {
            if (store->places[i].id == rel_place) {
                int pcol = store->places[i].col;
                int prow = (int)(store->places[i].abs_line - vt->sixel_abs_top);
                col = pcol + (p->has_col ? p->col : 0);
                row = prow + (p->has_row ? p->row : 0);
                rel_img = store->places[i].image_id;
                break;
            }
        }
    } else if (p->has_parent_img) {
        rel_img = (uint64_t)p->parent_img;
        /* Anchor to the image's first placement, if any. */
        for (int i = 0; i < store->place_count; i++) {
            if (store->places[i].image_id == rel_img) {
                int pcol = store->places[i].col;
                int prow = (int)(store->places[i].abs_line - vt->sixel_abs_top);
                col = pcol + (p->has_col ? p->col : 0);
                row = prow + (p->has_row ? p->row : 0);
                rel_place = store->places[i].id;
                break;
            }
        }
    }

    /* Cursor advance (c=r rows down, c=c cols right) happens after the
     * placement; the placement box itself is w= (cols) x h= (rows). */
    int adv_cols = p->has_adv_cols ? p->adv_cols : 0;
    int adv_rows = p->has_adv_rows ? p->adv_rows : 0;

    /* Placement cell size: explicit w=/h= override, else derive from
     * image display dimensions (as a default 1:1). */
    int cell_h = vt->cell_h_px > 0 ? vt->cell_h_px : 1;
    int cell_w = vt->cell_w_px > 0 ? vt->cell_w_px : 1;
    float scale = vt->content_scale > 0.0f ? vt->content_scale : 1.0f;
    int cols = p->has_place_cols ? p->place_cols
                                 : (((int)(img->w * scale) + cell_w - 1) / cell_w);
    int rows = p->has_place_rows ? p->place_rows
                                 : (((int)(img->h * scale) + cell_h - 1) / cell_h);
    if (cols < 1)
        cols = 1;
    if (rows < 1)
        rows = 1;

    int z_index = p->has_z_index ? p->z_index : 0;

    long abs_line = vt->sixel_abs_top + row;
    int pi = cfr_img_add_placement(vt, store, image_id, abs_line, col,
                                   rows, cols, 0, 255, z_index);
    if (pi < 0)
        return;
    if (p->has_placement_id)
        store->places[pi].id = (uint64_t)p->placement_id;
    if (rel_place)
        store->places[pi].parent_place = rel_place;
    if (rel_img)
        store->places[pi].parent_img = rel_img;

    /* Move the cursor: virtual (U=1) leaves it; otherwise advance. */
    if (!p->has_virtual || !p->virtual) {
        if (adv_cols > 0 || adv_rows > 0) {
            int nc = vt->cursor.col + adv_cols;
            int nr = vt->cursor.row + adv_rows;
            if (nc > vt->cols - 1)
                nc = vt->cols - 1;
            if (nr > vt->rows - 1)
                nr = vt->rows - 1;
            vt->cursor.col = nc;
            vt->cursor.row = nr;
        } else {
            /* Default: cursor moves below the placement (like sixel). */
            int nc = col + cols;
            if (nc > vt->cols - 1)
                nc = vt->cols - 1;
            vt->cursor.col = nc;
            int nr = row + rows;
            if (nr > vt->rows - 1)
                nr = vt->rows - 1;
            vt->cursor.row = nr;
        }
    }

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
    case 'p':
    case 'P':
        /* Delete a single placement by placement id. */
        if (p->has_placement_id) {
            uint64_t pid = (uint64_t)p->placement_id;
            for (int i = 0; i < store->place_count; i++) {
                if (store->places[i].id == pid) {
                    store->places[i] = store->places[--store->place_count];
                    break;
                }
            }
        }
        break;
    case 'i':
    case 'I':
    {
        /* Delete images (and their placements) by image id. */
        uint64_t iid = (uint64_t)(p->has_image_id ? p->image_id : 0);
        int idx = cfr_img_find_by_id(store, iid);
        if (idx >= 0)
            cfr_img_remove(vt, store, idx);
        break;
    }
    default:
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
        k_handle_transmit(vt, &p, payload, payload_len, false, false);
        break;
    case 'T':
        k_handle_transmit(vt, &p, payload, payload_len, false, true);
        break;
    case 'f':
        /* a=f: animation frame data (replaces pixels for the id). */
        k_handle_transmit(vt, &p, payload, payload_len, true, false);
        break;
    case 'a':
        /* a=a: animation control (frame index is payload-form only in
         * advanced clients). Accept and acknowledge as OK. */
        if (!p.quiet)
            k_emit_ok(vt, p.image_id);
        break;
    case 'c':
        /* a=c: compose (frame visibility). We render the latest frame in
         * every visible placement, so compose is a no-op; acknowledge. */
        if (!p.quiet)
            k_emit_ok(vt, p.image_id);
        break;
    case 'q':
        k_handle_query(vt, &p);
        break;
    case 'd':
        k_handle_delete(vt, &p);
        break;
    case 'p':
        k_handle_place(vt, &p);
        break;
    default:
        /* Unknown action — respond with error */
        if (!p.quiet)
            k_emit_error(vt, p.image_id, "EINVAL:unknown action");
        break;
    }
}
