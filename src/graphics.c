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
#include <math.h>

/* ------------------------------------------------------------------ */
/* Parameter parsing                                                  */
/* ------------------------------------------------------------------ */

typedef struct
{
    int action;         /* a= (t,T,q,p,d,f,a,c) */
    int image_id;       /* i= (0 = unassigned) */
    int image_num;      /* I= (non-unique) */
    int placement_id;   /* p= */
    int format;         /* f= (32=RGBA, 24=RGB, 100=PNG) */
    int width;          /* s= (pixel width) */
    int height;         /* v= (pixel height) */
    int z_index;        /* z= */
    int quiet;          /* q= (0,1,2) */
    int more;           /* m= (chunked: 1=more, 0=last) */
    int delete_what;    /* d= (for a=d) */
    int delete_x;       /* x= (for delete ranges/positions) */
    int delete_y;       /* y= */
    int src_x;          /* x= (source rect origin, pixels) */
    int src_y;          /* y= (source rect origin, pixels) */
    int src_w;          /* w= (source rect size, pixels) */
    int src_h;          /* h= (source rect size, pixels) */
    int cell_off_x;     /* X= (offset within the first cell, pixels) */
    int cell_off_y;     /* Y= */
    int rel_h;          /* H= (relative placement offset, cells) */
    int rel_v;          /* V= */
    int disp_cols;      /* c= (display cols; placement cell width) */
    int disp_rows;      /* r= (display rows; placement cell height) */
    int no_cursor_move; /* C= (1 = leave the cursor where it is) */
    int compression;    /* o= (z=zlib, else none) */
    int virtual;        /* U= (1 = virtual placement, no cursor move) */
    int parent_place;   /* P= (parent placement id) */
    int parent_img;     /* Q= (parent image id) */
    int has_image_id;
    int has_image_num;
    int has_placement_id;
    int has_format;
    int has_width;
    int has_height;
    int has_z_index;
    int has_more;
    int has_src_x;
    int has_src_y;
    int has_src_w;
    int has_src_h;
    int has_cell_off_x;
    int has_cell_off_y;
    int has_rel_h;
    int has_rel_v;
    int has_disp_cols;
    int has_disp_rows;
    int has_no_cursor_move;
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
                    p->has_src_x = 1;
                    p->src_x = atoi(val_buf);
                    p->delete_x = p->src_x;
                    break;
                case 'y':
                    p->has_src_y = 1;
                    p->src_y = atoi(val_buf);
                    p->delete_y = p->src_y;
                    break;
                case 'w':
                    p->has_src_w = 1;
                    p->src_w = atoi(val_buf);
                    break;
                case 'h':
                    p->has_src_h = 1;
                    p->src_h = atoi(val_buf);
                    break;
                case 'X':
                    p->has_cell_off_x = 1;
                    p->cell_off_x = atoi(val_buf);
                    break;
                case 'Y':
                    p->has_cell_off_y = 1;
                    p->cell_off_y = atoi(val_buf);
                    break;
                case 'H':
                    p->has_rel_h = 1;
                    p->rel_h = atoi(val_buf);
                    break;
                case 'V':
                    p->has_rel_v = 1;
                    p->rel_v = atoi(val_buf);
                    break;
                case 'c':
                    p->has_disp_cols = 1;
                    p->disp_cols = atoi(val_buf);
                    break;
                case 'r':
                    p->has_disp_rows = 1;
                    p->disp_rows = atoi(val_buf);
                    break;
                case 'C':
                    p->has_no_cursor_move = 1;
                    p->no_cursor_move = atoi(val_buf);
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
/* Cursor advance                                                      */
/* ------------------------------------------------------------------ */

/* Move the cursor after a placement: kitty's rule is
 * col += cols, row += rows - 1, then wrap col to the next line start
 * when it runs past the right edge. The wrap matters for full-width
 * placements: leaving the cursor at the last column makes a following
 * newline scroll the grid, shifting the image up a row. */
static void k_advance_cursor(CfrTerm *vt, int start_col, int cols, int rows)
{
    int nc = start_col + cols;
    int nr = vt->cursor.row + rows - 1;
    if (nc >= vt->cols) {
        nc -= vt->cols;
        nr++;
    }
    vt->cursor.col = nc < vt->cols - 1 ? nc : vt->cols - 1;

    /* Rows past the bottom scroll margin scroll the grid with the
     * cursor pinned on the margin, so the advance clamps the cursor
     * there and scrolls the remainder: the image's bottom row lands on
     * the screen bottom, as if the cursor had walked down one row per
     * step. The scroll count is bounded by the placement height, which
     * keeps an oversized a=T from spinning. */
    int target = nr;
    if (target > vt->scroll_bottom) {
        for (int i = 0; i < target - vt->scroll_bottom; ++i)
            cfr_scroll_up(vt, 1);
        target = vt->scroll_bottom;
    }
    if (vt->cursor.row < target)
        vt->cursor.row = target;
    /* The advance is an explicit cursor move, so it kills the phantom. */
    cfr_cursor_kill_phantom(vt);
}

/* ------------------------------------------------------------------ */
/* Placement geometry                                                  */
/* ------------------------------------------------------------------ */

/* The placement's cell size, by kitty's rule: c= and r= give the box; a
 * missing dimension is derived from the source rect's aspect ratio; with
 * neither, the image's natural size in cells.
 *
 * src_w/src_h are the source rect's size in the image's own pixels (the
 * whole image when the client sends no rect). The cell size is physical
 * (content scale included), so the image's pixels are scaled before the
 * division. */
static void k_placement_cells(CfrTerm *vt, const KittyParams *p,
                              int src_w, int src_h,
                              int *out_cols, int *out_rows)
{
    int cell_w = vt->cell_w_px > 0 ? vt->cell_w_px : 1;
    int cell_h = vt->cell_h_px > 0 ? vt->cell_h_px : 1;
    float scale = vt->content_scale > 0.0f ? vt->content_scale : 1.0f;
    if (src_w <= 0)
        src_w = 1;
    if (src_h <= 0)
        src_h = 1;

    int cols = (p->has_disp_cols && p->disp_cols > 0) ? p->disp_cols : 0;
    int rows = (p->has_disp_rows && p->disp_rows > 0) ? p->disp_rows : 0;

    if (cols > 0 && rows <= 0) {
        /* Only the width is given: the height follows the aspect ratio. */
        double width_px = (double)cell_w * cols;
        double height_px = width_px * (double)src_h / (double)src_w;
        rows = (int)ceil(height_px / (double)cell_h);
    } else if (rows > 0 && cols <= 0) {
        /* Only the height is given: the width follows the aspect ratio. */
        double height_px = (double)cell_h * rows;
        double width_px = height_px * (double)src_w / (double)src_h;
        cols = (int)ceil(width_px / (double)cell_w);
    } else if (cols <= 0 && rows <= 0) {
        /* Neither: the image's natural size, as close as it fits. */
        cols = ((int)(src_w * scale) + cell_w - 1) / cell_w;
        rows = ((int)(src_h * scale) + cell_h - 1) / cell_h;
    }
    if (cols < 1)
        cols = 1;
    if (rows < 1)
        rows = 1;
    *out_cols = cols;
    *out_rows = rows;
}

/* The source rect a placement shows: the client's x=/y=/w=/h= (pixels),
 * else the whole image. The rect is clamped to the image. */
static void k_source_rect(const KittyParams *p, int img_w, int img_h,
                          int *out_x, int *out_y, int *out_w, int *out_h)
{
    if (img_w < 1)
        img_w = 1;
    if (img_h < 1)
        img_h = 1;
    *out_x = (p->has_src_x && p->src_x > 0) ? p->src_x : 0;
    *out_y = (p->has_src_y && p->src_y > 0) ? p->src_y : 0;
    *out_w = (p->has_src_w && p->src_w > 0) ? p->src_w : img_w;
    *out_h = (p->has_src_h && p->src_h > 0) ? p->src_h : img_h;
    if (*out_x >= img_w)
        *out_x = img_w - 1;
    if (*out_y >= img_h)
        *out_y = img_h - 1;
    if (*out_x + *out_w > img_w)
        *out_w = img_w - *out_x;
    if (*out_y + *out_h > img_h)
        *out_h = img_h - *out_y;
    if (*out_w < 1)
        *out_w = 1;
    if (*out_h < 1)
        *out_h = 1;
}

/* ------------------------------------------------------------------ */
/* Action: transmit (a=t, a=T, a=f frame)                            */
/* ------------------------------------------------------------------ */

/* Chunked-upload accumulator for kitty transmit (m=1 ... m=0).
 * The first chunk carries the full control data (action, format,
 * dimensions, id); continuation chunks repeat only m= and payload, so
 * the accumulated header is stashed here for the final decode. */
typedef struct
{
    char *b64;
    size_t len;
    size_t cap;
    int active;
    int format;      /* f= from the first chunk */
    int width;       /* s= from the first chunk */
    int height;      /* v= from the first chunk */
    int compression; /* o= from the first chunk */
    int image_id;    /* i= from the first chunk */
    int has_image_id;
    int quiet;
    int action;    /* a= from the first chunk (t/T/f) */
    int disp_cols; /* c= from the first chunk (display cols) */
    int disp_rows; /* r= from the first chunk (display rows) */
    int has_disp_cols;
    int has_disp_rows;
    int z_index; /* z= from the first chunk (placement z-order) */
    int has_z_index;
    int no_cursor_move; /* C= from the first chunk (1 = leave cursor) */
    int has_no_cursor_move;
    int src_x, src_y, src_w, src_h; /* x=/y=/w=/h= (source rect) */
    int has_src_x, has_src_y, has_src_w, has_src_h;
    int cell_off_x, cell_off_y; /* X=/Y= (offset in the first cell) */
    int has_cell_off_x, has_cell_off_y;
    int rel_h, rel_v; /* H=/V= (relative placement offset) */
    int has_rel_h, has_rel_v;
} KChunk;

static KChunk g_chunk = { 0 };

/* Decode a base64 payload to RGBA (f=32/24/100). Returns malloc'd RGBA
 * and sets *w and *h. On failure returns NULL. If decompress is true, the
 * base64-decoded bytes are first inflated as a zlib stream (o=z). */
static uint8_t *k_decode_rgba(const uint8_t *payload, size_t payload_len,
                              int format, int *w, int *h, bool decompress)
{
    size_t raw_len = 0;
    uint8_t *raw = cfr_base64_decode((const char *)payload, payload_len, &raw_len);
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

/* Store a fully decoded image. Returns the store-internal HANDLE of
 * the image (0 on failure) — the handle is what a placement must
 * reference: the public id (the client's i=) is only meaningful
 * within kitty's own id space.
 *
 * Id semantics (kitty graphics spec):
 *  - a=f (animation frame): replaces the pixels of the existing image
 *    in place; its placements stay alive and now show the new frame.
 *  - a=t / a=T with an explicit id: re-transmitting an existing id
 *    deletes the image *and all its placements*; the new data is not
 *    displayed until a new placement is created.
 *  - No i= key: multiple images with id=0 may exist, so each id-less
 *    transmit must create a *new* image under a fresh engine id.
 */
static uint64_t k_store_image(CfrTerm *vt, const KittyParams *p,
                              uint8_t *rgba, int w, int h, bool is_frame)
{
    CfrImgStore *store = cfr_img_get_store(vt);
    if (!store)
        return 0;

    if (p->has_image_id) {
        uint64_t id = (uint64_t)p->image_id;
        /* The lookup is scoped to kitty's id space: a Lottie animation
         * with the same number is a different image. */
        int idx = cfr_img_find_by_source_id(store, IMG_SRC_KITTY, id);
        if (idx >= 0 && is_frame) {
            /* Animation frame: replace pixels in place, keep placements. */
            cfr_img_replace(vt, store, idx, rgba, w, h);
            if (!p->quiet)
                k_emit_ok(vt, p->image_id);
            return store->imgs[idx].handle;
        }
        if (idx >= 0 && !is_frame) {
            /* Re-transmit: delete the image and all its placements. */
            cfr_img_remove(vt, store, idx);
        }
        int ni = cfr_img_add_named(vt, store, id, rgba, w, h, 0, IMG_SRC_KITTY);
        if (!p->quiet)
            k_emit_ok(vt, p->image_id);
        return ni >= 0 ? store->imgs[ni].handle : 0;
    }

    /* No i= key: always a fresh image under a unique engine id so
     * successive id-less transmits (chafa, kitten icat) don't collide
     * on id=0. The engine id is what placements must reference. */
    uint64_t id = store->next_id++;
    int ni = cfr_img_add_named(vt, store, id, rgba, w, h, 0, IMG_SRC_KITTY);
    if (!p->quiet)
        k_emit_ok(vt, p->image_id);
    return ni >= 0 ? store->imgs[ni].handle : 0;
}

/* An omitted f= key is not "unknown format": kitty's default is 32
 * (raw RGBA), so normalize it before anything reads p->format. */
static void k_default_format(KittyParams *p)
{
    if (!p->has_format) {
        p->format = 32;
        p->has_format = 1;
    }
}

/* The raw formats carry no size of their own — their payload length is
 * derived from s/v, so the pair is REQUIRED there (kitty: "Zero
 * width/height not allowed"). f=100 (PNG) is exempt: the size lives in
 * the container, which the protocol's own PNG example shows ("ESC _ G
 * f=100 ; <payload> ESC \", and its minimal chunked sender puts only
 * "a=T,f=100," on the first chunk), and kitty reads the dimensions from
 * the decoded PNG. Demanding s/v for f=100 rejected spec-conformant
 * clients outright — silently, since q=2 suppresses the error reply. */
static bool k_needs_dimensions(const KittyParams *p)
{
    return p->format != 100;
}

static void k_handle_transmit(CfrTerm *vt, KittyParams *p,
                              const uint8_t *payload, size_t payload_len,
                              bool is_frame, bool place_at_cursor)
{
    /* Chunked transfer: m=1 accumulates base64 into a static buffer;
     * m=0 (or no m flag) finalizes and stores. Continuation chunks
     * carry no action/format/dimensions, so stash the first chunk's
     * control data for the final decode. */
    if (p->has_more && p->more == 1) {
        if (!g_chunk.active) {
            /* First chunk must carry the metadata for the decode. */
            k_default_format(p);
            if (k_needs_dimensions(p) &&
                (!p->has_width || !p->has_height)) {
                if (!p->quiet)
                    k_emit_error(vt, p->image_id, "EINVAL:missing format/dimensions");
                return;
            }
            g_chunk.format = p->format;
            g_chunk.width = p->width;
            g_chunk.height = p->height;
            g_chunk.compression = p->compression;
            g_chunk.image_id = p->image_id;
            g_chunk.has_image_id = p->has_image_id;
            g_chunk.quiet = p->quiet;
            g_chunk.action = p->action;
            g_chunk.disp_cols = p->disp_cols;
            g_chunk.disp_rows = p->disp_rows;
            g_chunk.has_disp_cols = p->has_disp_cols;
            g_chunk.has_disp_rows = p->has_disp_rows;
            g_chunk.z_index = p->z_index;
            g_chunk.has_z_index = p->has_z_index;
            g_chunk.no_cursor_move = p->no_cursor_move;
            g_chunk.has_no_cursor_move = p->has_no_cursor_move;
            g_chunk.src_x = p->src_x;
            g_chunk.src_y = p->src_y;
            g_chunk.src_w = p->src_w;
            g_chunk.src_h = p->src_h;
            g_chunk.has_src_x = p->has_src_x;
            g_chunk.has_src_y = p->has_src_y;
            g_chunk.has_src_w = p->has_src_w;
            g_chunk.has_src_h = p->has_src_h;
            g_chunk.cell_off_x = p->cell_off_x;
            g_chunk.cell_off_y = p->cell_off_y;
            g_chunk.has_cell_off_x = p->has_cell_off_x;
            g_chunk.has_cell_off_y = p->has_cell_off_y;
            g_chunk.rel_h = p->rel_h;
            g_chunk.rel_v = p->rel_v;
            g_chunk.has_rel_h = p->has_rel_h;
            g_chunk.has_rel_v = p->has_rel_v;
        }
        if (cfr_buf_append((uint8_t **)&g_chunk.b64, &g_chunk.len,
                           &g_chunk.cap, payload, payload_len) != 0)
            return;
        /* A transfer whose first chunk carries only control data (no
         * ';' — chafa emits "ESC _ G a=T,...,m=1,q=2 ESC \") never
         * appends any bytes, so the buffer may still be unallocated. */
        if (g_chunk.b64)
            g_chunk.b64[g_chunk.len] = '\0';
        g_chunk.active = 1;
        return;
    }

    /* Final chunk (m=0 or single-shot). */
    const uint8_t *data = payload;
    size_t data_len = payload_len;
    char *tmp_b64 = NULL;

    /* Continuation chunks carry no action/format/dimensions; recover
     * them from the first chunk of the transfer. */
    if (g_chunk.active) {
        if (!p->has_format) {
            p->format = g_chunk.format;
            p->has_format = 1;
        }
        if (!p->has_width) {
            p->width = g_chunk.width;
            p->has_width = 1;
        }
        if (!p->has_height) {
            p->height = g_chunk.height;
            p->has_height = 1;
        }
        if (!p->has_compression) {
            p->compression = g_chunk.compression;
            p->has_compression = 1;
        }
        if (!p->has_image_id) {
            p->image_id = g_chunk.image_id;
            p->has_image_id = g_chunk.has_image_id;
        }
        if (!p->quiet)
            p->quiet = g_chunk.quiet;
        if (!p->has_disp_cols) {
            p->disp_cols = g_chunk.disp_cols;
            p->has_disp_cols = g_chunk.has_disp_cols;
        }
        if (!p->has_disp_rows) {
            p->disp_rows = g_chunk.disp_rows;
            p->has_disp_rows = g_chunk.has_disp_rows;
        }
        if (!p->has_z_index) {
            p->z_index = g_chunk.z_index;
            p->has_z_index = g_chunk.has_z_index;
        }
        if (!p->has_no_cursor_move) {
            p->no_cursor_move = g_chunk.no_cursor_move;
            p->has_no_cursor_move = g_chunk.has_no_cursor_move;
        }
        if (!p->has_src_x) {
            p->src_x = g_chunk.src_x;
            p->has_src_x = g_chunk.has_src_x;
        }
        if (!p->has_src_y) {
            p->src_y = g_chunk.src_y;
            p->has_src_y = g_chunk.has_src_y;
        }
        if (!p->has_src_w) {
            p->src_w = g_chunk.src_w;
            p->has_src_w = g_chunk.has_src_w;
        }
        if (!p->has_src_h) {
            p->src_h = g_chunk.src_h;
            p->has_src_h = g_chunk.has_src_h;
        }
        if (!p->has_cell_off_x) {
            p->cell_off_x = g_chunk.cell_off_x;
            p->has_cell_off_x = g_chunk.has_cell_off_x;
        }
        if (!p->has_cell_off_y) {
            p->cell_off_y = g_chunk.cell_off_y;
            p->has_cell_off_y = g_chunk.has_cell_off_y;
        }
        if (!p->has_rel_h) {
            p->rel_h = g_chunk.rel_h;
            p->has_rel_h = g_chunk.has_rel_h;
        }
        if (!p->has_rel_v) {
            p->rel_v = g_chunk.rel_v;
            p->has_rel_v = g_chunk.has_rel_v;
        }

        size_t need = g_chunk.len + payload_len + 1;
        tmp_b64 = malloc(need);
        if (!tmp_b64)
            return;
        if (g_chunk.len > 0)
            memcpy(tmp_b64, g_chunk.b64, g_chunk.len);
        if (payload_len > 0)
            memcpy(tmp_b64 + g_chunk.len, payload, payload_len);
        tmp_b64[g_chunk.len + payload_len] = '\0';
        data = (const uint8_t *)tmp_b64;
        data_len = g_chunk.len + payload_len;

        free(g_chunk.b64);
        memset(&g_chunk, 0, sizeof(g_chunk));
    }

    /* Single-shot transmit must carry the metadata itself. The format
     * defaults to RGBA when f= is omitted (kitty's rule); the s/v pair
     * is required only for the raw formats (see k_needs_dimensions). */
    k_default_format(p);
    if (k_needs_dimensions(p) && (!p->has_width || !p->has_height)) {
        if (!p->quiet)
            k_emit_error(vt, p->image_id, "EINVAL:missing format/dimensions");
        return;
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

    uint64_t stored_handle = k_store_image(vt, p, rgba, w, h, is_frame);

    /* a=T also places at the cursor. */
    if (place_at_cursor && stored_handle) {
        CfrImgStore *store = cfr_img_get_store(vt);
        int src_x, src_y, src_w, src_h;
        k_source_rect(p, w, h, &src_x, &src_y, &src_w, &src_h);
        int cols, rows;
        k_placement_cells(vt, p, src_w, src_h, &cols, &rows);
        int place_row = vt->cursor.row;
        int place_col = vt->cursor.col;
        int pi = cfr_img_add_placement(vt, store, stored_handle,
                                       vt->sixel_abs_top + place_row,
                                       place_col, rows, cols, 0, 255,
                                       p->has_z_index ? p->z_index : 0);
        if (pi >= 0) {
            store->places[pi].src_x = src_x;
            store->places[pi].src_y = src_y;
            store->places[pi].src_w = src_w;
            store->places[pi].src_h = src_h;
            store->places[pi].cell_off_x =
                (p->has_cell_off_x && p->cell_off_x > 0) ? p->cell_off_x : 0;
            store->places[pi].cell_off_y =
                (p->has_cell_off_y && p->cell_off_y > 0) ? p->cell_off_y : 0;
        }

        /* Cursor advance by the placement size, unless C=1 or U=1. */
        if (!(p->has_no_cursor_move && p->no_cursor_move) &&
            !(p->has_virtual && p->virtual))
            k_advance_cursor(vt, place_col, cols, rows);
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
    CfrImgStore *store = cfr_img_get_store(vt);
    if (!store)
        return;

    uint64_t image_id = (uint64_t)(p->has_image_id ? p->image_id : 0);
    /* The lookup is scoped to kitty's id space (a Lottie animation with
     * the same number is a different image). */
    int img_idx = cfr_img_find_by_source_id(store, IMG_SRC_KITTY, image_id);
    if (img_idx < 0) {
        if (!p->quiet)
            k_emit_error(vt, p->image_id, "ENOENT:unknown image id");
        return;
    }
    CfrImg *img = &store->imgs[img_idx];
    uint64_t image_handle = img->handle;

    /* The placement sits at the cursor; a relative placement takes its
     * position from its parent instead. */
    int col = vt->cursor.col;
    int row = vt->cursor.row;

    /* Relative placement: when P= (parent placement) or Q= (parent image)
     * is given, the position is the parent's top-left cell offset by
     * H=/V= (cells). */
    uint64_t rel_place = 0, rel_img = 0;
    bool relative = p->has_parent_place || p->has_parent_img;
    if (relative) {
        uint64_t want = (uint64_t)(p->has_parent_place ? p->parent_place
                                                       : p->parent_img);
        /* Find the parent placement (by placement id, or the image's
         * first placement) to anchor against. */
        for (int i = 0; i < store->place_count; i++) {
            bool match = p->has_parent_place
                             ? store->places[i].id == want
                             : store->places[i].image_id == want &&
                                   store->places[i].source == IMG_SRC_KITTY;
            if (match) {
                int pcol = store->places[i].col;
                int prow = (int)(store->places[i].abs_line - vt->sixel_abs_top);
                col = pcol + (p->has_rel_h ? p->rel_h : 0);
                row = prow + (p->has_rel_v ? p->rel_v : 0);
                if (p->has_parent_place)
                    rel_img = store->places[i].image_id;
                else
                    rel_place = store->places[i].id;
                break;
            }
        }
    }

    /* The source rect (x=/y=/w=/h=, pixels) and the cell offsets
     * (X=/Y=, pixels within the first cell). */
    int src_x, src_y, src_w, src_h;
    k_source_rect(p, img->buf_w > 0 ? img->buf_w : img->w,
                  img->buf_h > 0 ? img->buf_h : img->h,
                  &src_x, &src_y, &src_w, &src_h);

    /* Placement cell size: c=/r= (display cols/rows); a missing
     * dimension follows the source rect's aspect ratio, and with
     * neither the image's natural size. */
    int cols, rows;
    k_placement_cells(vt, p, src_w, src_h, &cols, &rows);

    int z_index = p->has_z_index ? p->z_index : 0;

    long abs_line = vt->sixel_abs_top + row;
    int pi = cfr_img_add_placement(vt, store, image_handle, abs_line, col,
                                   rows, cols, 0, 255, z_index);
    if (pi < 0)
        return;
    if (p->has_placement_id)
        store->places[pi].id = (uint64_t)p->placement_id;
    if (rel_place)
        store->places[pi].parent_place = rel_place;
    if (rel_img)
        store->places[pi].parent_img = rel_img;
    store->places[pi].src_x = src_x;
    store->places[pi].src_y = src_y;
    store->places[pi].src_w = src_w;
    store->places[pi].src_h = src_h;
    store->places[pi].cell_off_x =
        (p->has_cell_off_x && p->cell_off_x > 0) ? p->cell_off_x : 0;
    store->places[pi].cell_off_y =
        (p->has_cell_off_y && p->cell_off_y > 0) ? p->cell_off_y : 0;

    /* Move the cursor: virtual (U=1), C=1 and a relative placement leave
     * it; otherwise advance by the placement size. */
    if ((!p->has_virtual || !p->virtual) && !relative &&
        !(p->has_no_cursor_move && p->no_cursor_move))
        k_advance_cursor(vt, col, cols, rows);

    if (!p->quiet)
        k_emit_ok(vt, p->image_id);
}

/* ------------------------------------------------------------------ */
/* Action: delete (a=d)                                              */
/* ------------------------------------------------------------------ */

static void k_handle_delete(CfrTerm *vt, const KittyParams *p)
{
    CfrImgStore *store = cfr_img_get_store(vt);
    if (!store) {
        if (!p->quiet)
            k_emit_ok(vt, p->image_id);
        return;
    }

    switch (p->delete_what) {
    case 'a':
    case 'A':
        /* Delete all the kitty images. The other protocols' images live
         * in their own id spaces and are not kitty's to delete. */
        cfr_img_clear_source(vt, store, IMG_SRC_KITTY);
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
        /* Delete images (and their placements) by image id. The lookup
         * is scoped to kitty's id space: a Lottie animation with the
         * same number is a different image. */
        uint64_t iid = (uint64_t)(p->has_image_id ? p->image_id : 0);
        int idx = cfr_img_find_by_source_id(store, IMG_SRC_KITTY, iid);
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

    /* Continuation chunks (m=1/m=0) carry no action key; they inherit
     * the action from the first chunk of the transfer. Without this
     * the dispatcher would fall through to the unknown-action error for
     * every chunk (chafa sends exactly this form). The final m=0 chunk
     * also needs the stashed action so a=T (transmit-and-place) still
     * places after the chunked upload completes. */
    if (p.action == 0 && p.has_more) {
        if (g_chunk.active)
            p.action = g_chunk.action ? g_chunk.action : 't';
        else
            p.action = 't';
    }

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
