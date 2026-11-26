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
    int action;           /* a= (t,T,q,p,d,f,a,c) */
    int report_supported; /* e= (feature query: report keys for this action) */
    int image_id;         /* i= (0 = unassigned) */
    int image_num;        /* I= (non-unique) */
    int placement_id;     /* p= */
    int format;           /* f= (32=RGBA, 24=RGB, 100=PNG) */
    int width;            /* s= (pixel width) */
    int height;           /* v= (pixel height) */
    int z_index;          /* z= */
    int quiet;            /* q= (0,1,2) */
    int more;             /* m= (chunked: 1=more, 0=last) */
    int delete_what;      /* d= (for a=d) */
    int src_x;            /* x= (source rect origin, pixels; delete cell x) */
    int src_y;            /* y= (source rect origin, pixels; delete cell y) */
    int src_w;            /* w= (source rect size, pixels) */
    int src_h;            /* h= (source rect size, pixels) */
    int cell_off_x;       /* X= (offset within the first cell, pixels) */
    int cell_off_y;       /* Y= (offset within the first cell, pixels) */
    uint32_t bg_rgba;     /* Y= (a=f frame canvas background, 32-bit RGBA) */
    int rel_h;            /* H= (relative placement offset, cells) */
    int rel_v;            /* V= */
    int disp_cols;        /* c= (display cols; placement cell width) */
    int disp_rows;        /* r= (display rows; placement cell height) */
    int no_cursor_move;   /* C= (1 = leave the cursor where it is) */
    int compression;      /* o= (z=zlib, else none) */
    int virtual;          /* U= (1 = virtual placement, no cursor move) */
    int parent_place;     /* P= (parent placement id) */
    int parent_img;       /* Q= (parent image id) */
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
    int has_bg_rgba;
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
                case 'e':
                    p->report_supported = val_buf[0];
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
                    break;
                case 'y':
                    p->has_src_y = 1;
                    p->src_y = atoi(val_buf);
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
                    /* Y= is overloaded: a place reads it as the cell
                     * offset, a=f as the frame canvas background. */
                    p->has_cell_off_y = 1;
                    p->cell_off_y = atoi(val_buf);
                    p->has_bg_rgba = 1;
                    p->bg_rgba = (uint32_t)strtoul(val_buf, NULL, 10);
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
     * The system conhost fallback strips APC (ESC _) in both directions,
     * so kitty graphics responses leave via the OSC 5556 carrier.  The
     * bundled OpenConsole accepts APC, but OSC 5556 remains compatible
     * with both backends. The payload after the semicolon is the raw
     * "G" + msg (NOT base64 — kitty's OSC carrier is raw, unlike
     * Lottie's base64-encoded carrier). */
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

/* The success reply for a command that named an image: the image's own
 * id, plus its I= number when the client addressed it by number, so the
 * client learns the id the number resolved to. */
static void k_emit_image_ok(CfrTerm *vt, const CfrImg *img)
{
    char buf[96];
    int n;
    if (img->has_image_num)
        n = snprintf(buf, sizeof(buf), "i=%llu,I=%u;OK",
                     (unsigned long long)img->id,
                     (unsigned int)img->image_num);
    else
        n = snprintf(buf, sizeof(buf), "i=%llu;OK",
                     (unsigned long long)img->id);
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
 * src_w/src_h are the source rect's size in the image's own DEVICE
 * pixels (the whole image when the client sends no rect), the same unit
 * as the physical cell — the natural-size box is a plain ceiling
 * division. */
static void k_placement_cells(CfrTerm *vt, const KittyParams *p,
                              int src_w, int src_h,
                              int *out_cols, int *out_rows)
{
    int cell_w = vt->cell_w_px > 0 ? vt->cell_w_px : 1;
    int cell_h = vt->cell_h_px > 0 ? vt->cell_h_px : 1;
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
        cols = (src_w + cell_w - 1) / cell_w;
        rows = (src_h + cell_h - 1) / cell_h;
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

/* Decode a base64 payload to RGBA (f=32/24/100). Returns a freshly
 * allocated RGBA buffer (the caller's allocator) and sets *w and *h.
 * On failure returns NULL. If decompress is true, the base64-decoded
 * bytes are first inflated as a zlib stream (o=z). */
static uint8_t *k_decode_rgba(CfrTerm *vt, const uint8_t *payload,
                              size_t payload_len, int format, int *w, int *h,
                              bool decompress)
{
    size_t raw_len = 0;
    uint8_t *raw = cfr_base64_decode(vt, (const char *)payload, payload_len,
                                     &raw_len);
    if (!raw)
        return NULL;

    uint8_t *data = raw;
    size_t data_len = raw_len;

    if (decompress) {
        size_t out_len = 0;
        uint8_t *inf = cfr_zlib_decompress(vt, raw, raw_len, &out_len);
        cfr_dealloc(vt, raw);
        if (!inf)
            return NULL;
        data = inf;
        data_len = out_len;
    }

    uint8_t *rgba = NULL;

    if (format == 100) {
        int dw = 0, dh = 0;
        /* cfr_image_decode returns C-library memory (stb_image); move
         * it onto the terminal's allocator like the raw formats, so
         * the caller frees uniformly. */
        uint8_t *dec = cfr_image_decode(data, data_len, &dw, &dh);
        if (dec) {
            size_t need = (size_t)dw * (size_t)dh * 4u;
            rgba = cfr_alloc(vt, need);
            if (rgba)
                memcpy(rgba, dec, need);
            free(dec);
            *w = dw;
            *h = dh;
        }
    } else if (format == 32) {
        size_t need = (size_t)*w * (size_t)*h * 4;
        if (data_len < need) {
            cfr_dealloc(vt, data);
            return NULL;
        }
        rgba = cfr_alloc(vt, need);
        if (rgba)
            memcpy(rgba, data, need);
    } else if (format == 24) {
        size_t need = (size_t)*w * (size_t)*h * 3;
        if (data_len < need) {
            cfr_dealloc(vt, data);
            return NULL;
        }
        rgba = cfr_alloc(vt, (size_t)*w * (size_t)*h * 4);
        if (rgba) {
            for (int i = 0; i < *w * *h; i++) {
                rgba[i * 4 + 0] = data[i * 3 + 0];
                rgba[i * 4 + 1] = data[i * 3 + 1];
                rgba[i * 4 + 2] = data[i * 3 + 2];
                rgba[i * 4 + 3] = 255;
            }
        }
    }
    cfr_dealloc(vt, data);
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
 *    transmit must create a *new* image under a fresh engine id. An
 *    I= key transmits under an image NUMBER: the terminal assigns the
 *    id and replies with it, and later commands naming that number act
 *    on its newest image.
 */

/* Resolve the image an i=- or I=-keyed command names, scoped to kitty's
 * id space and the active screen: the id wins, else the NEWEST record
 * carrying the number. Returns the store index or -1. */
static int k_resolve_image(CfrTerm *vt, CfrImgStore *store,
                           const KittyParams *p)
{
    uint8_t screen = (uint8_t)vt->in_altscreen;
    if (p->has_image_id)
        return cfr_img_find_by_source_id(store, IMG_SRC_KITTY,
                                         (uint64_t)p->image_id, screen);
    if (p->has_image_num) {
        int best = -1;
        for (int i = 0; i < store->img_count; i++) {
            if (store->imgs[i].source != IMG_SRC_KITTY ||
                store->imgs[i].screen != screen ||
                !store->imgs[i].has_image_num ||
                store->imgs[i].image_num != (uint32_t)p->image_num)
                continue;
            if (best < 0 || store->imgs[i].seq > store->imgs[best].seq)
                best = i;
        }
        return best;
    }
    return -1;
}

static uint64_t k_store_image(CfrTerm *vt, const KittyParams *p,
                              uint8_t *rgba, int w, int h)
{
    CfrImgStore *store = cfr_img_get_store(vt);
    if (!store)
        return 0;

    if (p->has_image_id) {
        uint64_t id = (uint64_t)p->image_id;
        /* The lookup is scoped to kitty's id space: a Lottie animation
         * with the same number is a different image. It is also scoped
         * to the active screen: the two buffers have separate graphics
         * sets. */
        int idx = cfr_img_find_by_source_id(store, IMG_SRC_KITTY, id,
                                            (uint8_t)vt->in_altscreen);
        if (idx >= 0) {
            /* Re-transmit: delete the image and all its placements. */
            cfr_img_remove(vt, store, idx);
        }
        int ni = cfr_img_put_named(vt, store, id, rgba, w, h, 0, IMG_SRC_KITTY);
        if (ni < 0)
            return 0;
        store->imgs[ni].has_client_id = 1;
        if (!p->quiet)
            k_emit_image_ok(vt, &store->imgs[ni]);
        return store->imgs[ni].handle;
    }

    /* No i= key: always a fresh image under a unique engine id so
     * successive id-less transmits (chafa, kitten icat) don't collide
     * on id=0. The engine id is what placements must reference. An
     * I= transmit is the same, and the reply carries the assigned id
     * alongside the number. */
    uint64_t id = store->next_id++;
    int ni = cfr_img_put_named(vt, store, id, rgba, w, h, 0, IMG_SRC_KITTY);
    if (ni < 0)
        return 0;
    if (p->has_image_num) {
        store->imgs[ni].image_num = (uint32_t)p->image_num;
        store->imgs[ni].has_image_num = 1;
    }
    if (!p->quiet)
        k_emit_image_ok(vt, &store->imgs[ni]);
    return store->imgs[ni].handle;
}

/* a=f: frame data for an existing image. The rect is x=/y= + the
 * decoded s=/v=; c= is the base frame, r= the frame edited (0 = append
 * a new one), X=1 replaces instead of blending, Y= the canvas
 * background, z= the frame's gap. The frame is stored; showing it is
 * a=a,c= or the playback clock. */
static uint64_t k_store_frame(CfrTerm *vt, const KittyParams *p,
                              uint8_t *rgba, int w, int h)
{
    CfrImgStore *store = cfr_img_get_store(vt);
    if (!store)
        return 0;
    int idx = k_resolve_image(vt, store, p);
    if (idx < 0) {
        if (!p->quiet)
            k_emit_error(vt, p->image_id, "ENOENT:no image for frame");
        return 0;
    }
    CfrImg *img = &store->imgs[idx];
    int fx = (p->has_src_x && p->src_x > 0) ? p->src_x : 0;
    int fy = (p->has_src_y && p->src_y > 0) ? p->src_y : 0;
    int base_n = p->has_disp_cols ? p->disp_cols : 0;         /* c= */
    int edit_n = p->has_disp_rows ? p->disp_rows : 0;         /* r= */
    bool overwrite = p->has_cell_off_x && p->cell_off_x == 1; /* X= */
    uint32_t bg = p->has_bg_rgba ? p->bg_rgba : 0u;           /* Y= */
    int gap = p->has_z_index ? p->z_index : 0;                /* z= */
    if (cfr_img_frame_put(vt, store, idx, rgba, w, h, fx, fy,
                          edit_n, base_n, overwrite, bg, gap) < 0) {
        if (!p->quiet)
            k_emit_error(vt, p->image_id, "EINVAL:frame data rejected");
        return 0;
    }
    if (!p->quiet)
        k_emit_image_ok(vt, img);
    return img->handle;
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
                              const char *ctrl, size_t ctrl_len,
                              const uint8_t *payload, size_t payload_len,
                              bool is_frame, bool place_at_cursor)
{
    CfrKChunk *ch = &vt->kchunk;

    /* Chunked transfer: m=1 accumulates base64 into the terminal's
     * accumulator; m=0 (or no m flag) finalizes and stores. Continuation
     * chunks carry no action/format/dimensions, so the first chunk's
     * control data is stashed and re-parsed for the final decode. */
    if (p->has_more && p->more == 1) {
        if (!ch->active) {
            /* First chunk must carry the metadata for the decode. */
            k_default_format(p);
            if (k_needs_dimensions(p) &&
                (!p->has_width || !p->has_height)) {
                if (!p->quiet)
                    k_emit_error(vt, p->image_id, "EINVAL:missing format/dimensions");
                return;
            }
            if (!ch->ctrl || ch->ctrl_cap < ctrl_len + 1) {
                size_t ncap = ctrl_len + 1;
                char *nc = cfr_alloc(vt, ncap);
                if (!nc)
                    return;
                cfr_dealloc(vt, ch->ctrl);
                ch->ctrl = nc;
                ch->ctrl_cap = ncap;
            }
            memcpy(ch->ctrl, ctrl, ctrl_len);
            ch->ctrl[ctrl_len] = '\0';
            ch->ctrl_len = ctrl_len;
        }
        if (cfr_buf_append(vt, (uint8_t **)&ch->b64, &ch->len,
                           &ch->cap, payload, payload_len) != 0)
            return;
        /* cfr_buf_append keeps the accumulator NUL-terminated; a first
         * chunk carrying only control data (no ';' — chafa emits
         * "ESC _ G a=T,...,m=1,q=2 ESC \") appends no bytes at all and
         * leaves it unallocated. */
        ch->active = 1;
        return;
    }

    /* Final chunk (m=0 or single-shot). */
    const uint8_t *data = payload;
    size_t data_len = payload_len;
    char *tmp_b64 = NULL;

    /* Continuation chunks carry no action/format/dimensions; recover
     * them by re-parsing the first chunk's control data, letting keys
     * the final chunk does set (q=, m=) win. */
    if (ch->active) {
        KittyParams first;
        k_parse_params(ch->ctrl, ch->ctrl_len, &first);
#define MERGE(val, has)         \
    do {                        \
        if (!p->has) {          \
            p->val = first.val; \
            p->has = first.has; \
        }                       \
    } while (0)
        MERGE(format, has_format);
        MERGE(width, has_width);
        MERGE(height, has_height);
        MERGE(compression, has_compression);
        MERGE(image_id, has_image_id);
        MERGE(image_num, has_image_num);
        if (!p->quiet)
            p->quiet = first.quiet;
        MERGE(disp_cols, has_disp_cols);
        MERGE(disp_rows, has_disp_rows);
        MERGE(z_index, has_z_index);
        MERGE(no_cursor_move, has_no_cursor_move);
        MERGE(src_x, has_src_x);
        MERGE(src_y, has_src_y);
        MERGE(src_w, has_src_w);
        MERGE(src_h, has_src_h);
        MERGE(cell_off_x, has_cell_off_x);
        MERGE(cell_off_y, has_cell_off_y);
        MERGE(bg_rgba, has_bg_rgba);
        MERGE(rel_h, has_rel_h);
        MERGE(rel_v, has_rel_v);
#undef MERGE

        size_t need = ch->len + payload_len + 1;
        tmp_b64 = cfr_alloc(vt, need);
        if (!tmp_b64)
            return;
        if (ch->len > 0)
            memcpy(tmp_b64, ch->b64, ch->len);
        if (payload_len > 0)
            memcpy(tmp_b64 + ch->len, payload, payload_len);
        tmp_b64[ch->len + payload_len] = '\0';
        data = (const uint8_t *)tmp_b64;
        data_len = ch->len + payload_len;

        cfr_dealloc(vt, ch->b64);
        cfr_dealloc(vt, ch->ctrl);
        memset(ch, 0, sizeof(*ch));
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
    uint8_t *rgba = k_decode_rgba(vt, data, data_len, p->format, &w, &h,
                                  decompress);
    cfr_dealloc(vt, tmp_b64);

    if (!rgba) {
        if (!p->quiet)
            k_emit_error(vt, p->image_id, "EINVAL:decode failed");
        return;
    }

    uint64_t stored_handle = is_frame ? k_store_frame(vt, p, rgba, w, h)
                                      : k_store_image(vt, p, rgba, w, h);

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
    cfr_dealloc(vt, rgba);
}

/* ------------------------------------------------------------------ */
/* Action: query (a=q)                                                */
/* ------------------------------------------------------------------ */

/* A feature query (a=q with e=<action>): echo the queried action plus
 * every non-default key we support for it, so clients can detect
 * features. Keys we do not implement are omitted — the reply is the
 * capability list, never a promise. */
static void k_handle_feature_query(CfrTerm *vt, const KittyParams *p)
{
    char buf[512];
    size_t pos = 0;

#define ECHO(...)                                                 \
    do {                                                          \
        if (sizeof(buf) - 1 > pos)                                \
            pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, \
                                    __VA_ARGS__);                 \
    } while (0)

    ECHO(",e=%c", p->report_supported);
    switch (p->report_supported) {
    case 't':
    case 'q':
        if (p->has_format && (p->format == 32 || p->format == 24 ||
                              p->format == 100))
            ECHO(",f=%d", p->format);
        if (p->has_compression && p->compression == 'z')
            ECHO(",o=z");
        if (p->has_width)
            ECHO(",s=%d", p->width);
        if (p->has_height)
            ECHO(",v=%d", p->height);
        break;
    case 'T':
        if (p->has_format && (p->format == 32 || p->format == 24 ||
                              p->format == 100))
            ECHO(",f=%d", p->format);
        if (p->has_compression && p->compression == 'z')
            ECHO(",o=z");
        if (p->has_width)
            ECHO(",s=%d", p->width);
        if (p->has_height)
            ECHO(",v=%d", p->height);
        if (p->has_no_cursor_move)
            ECHO(",C=%d", p->no_cursor_move);
        /* U= is deliberately NOT echoed: Unicode placeholders are not
         * implemented, and advertising them would break clients. */
        break;
    case 'f':
        if (p->has_format && (p->format == 32 || p->format == 24 ||
                              p->format == 100))
            ECHO(",f=%d", p->format);
        if (p->has_compression && p->compression == 'z')
            ECHO(",o=z");
        if (p->has_width)
            ECHO(",s=%d", p->width);
        if (p->has_height)
            ECHO(",v=%d", p->height);
        if (p->has_src_x)
            ECHO(",x=%d", p->src_x);
        if (p->has_src_y)
            ECHO(",y=%d", p->src_y);
        if (p->has_disp_cols)
            ECHO(",c=%d", p->disp_cols);
        if (p->has_disp_rows)
            ECHO(",r=%d", p->disp_rows);
        if (p->has_z_index)
            ECHO(",z=%d", p->z_index);
        if (p->has_cell_off_x)
            ECHO(",X=%d", p->cell_off_x);
        if (p->has_bg_rgba)
            ECHO(",Y=%u", p->bg_rgba);
        break;
    case 'p':
        if (p->has_src_x)
            ECHO(",x=%d", p->src_x);
        if (p->has_src_y)
            ECHO(",y=%d", p->src_y);
        if (p->has_src_w)
            ECHO(",w=%d", p->src_w);
        if (p->has_src_h)
            ECHO(",h=%d", p->src_h);
        if (p->has_cell_off_x)
            ECHO(",X=%d", p->cell_off_x);
        if (p->has_cell_off_y)
            ECHO(",Y=%d", p->cell_off_y);
        if (p->has_disp_cols)
            ECHO(",c=%d", p->disp_cols);
        if (p->has_disp_rows)
            ECHO(",r=%d", p->disp_rows);
        if (p->has_z_index)
            ECHO(",z=%d", p->z_index);
        if (p->has_no_cursor_move)
            ECHO(",C=%d", p->no_cursor_move);
        /* U= is deliberately NOT echoed: Unicode placeholders are not
         * implemented, and advertising them would break clients. */
        break;
    case 'a':
        if (p->has_disp_cols)
            ECHO(",c=%d", p->disp_cols);
        if (p->has_disp_rows)
            ECHO(",r=%d", p->disp_rows);
        if (p->has_z_index)
            ECHO(",z=%d", p->z_index);
        if (p->has_width)
            ECHO(",s=%d", p->width);
        if (p->has_height)
            ECHO(",v=%d", p->height);
        break;
    case 'c':
        if (p->has_disp_rows)
            ECHO(",r=%d", p->disp_rows);
        if (p->has_disp_cols)
            ECHO(",c=%d", p->disp_cols);
        if (p->has_src_x)
            ECHO(",x=%d", p->src_x);
        if (p->has_src_y)
            ECHO(",y=%d", p->src_y);
        if (p->has_src_w)
            ECHO(",w=%d", p->src_w);
        if (p->has_src_h)
            ECHO(",h=%d", p->src_h);
        if (p->has_cell_off_x)
            ECHO(",X=%d", p->cell_off_x);
        if (p->has_cell_off_y)
            ECHO(",Y=%d", p->cell_off_y);
        if (p->has_no_cursor_move)
            ECHO(",C=%d", p->no_cursor_move);
        break;
    case 'd':
        if (p->delete_what &&
            strchr("aAcCfFiInNpPqQrRxXyYzZ", p->delete_what))
            ECHO(",d=%c", p->delete_what);
        break;
    default:
        break;
    }
#undef ECHO

    char full[640];
    int n = snprintf(full, sizeof(full), "i=%d%s;OK", p->image_id, buf);
    if (n > 0)
        k_emit_response(vt, full);
}

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

    /* A feature query (e=) reports exactly what we implement. */
    if (p->report_supported) {
        if (!p->has_image_id)
            return; /* the spec requires the query to name an id */
        k_handle_feature_query(vt, p);
        return;
    }

    /* Always respond with OK — we support the protocol */
    k_emit_ok(vt, p->image_id);
}

/* ------------------------------------------------------------------ */
/* Action: animation control (a=a)                                   */
/* ------------------------------------------------------------------ */

static void k_handle_animate(CfrTerm *vt, const KittyParams *p)
{
    CfrImgStore *store = cfr_img_get_store(vt);
    if (!store) {
        if (!p->quiet)
            k_emit_ok(vt, p->image_id);
        return;
    }
    int idx = k_resolve_image(vt, store, p);
    if (idx < 0) {
        if (!p->quiet)
            k_emit_error(vt, p->image_id, "ENOENT:no image for animation");
        return;
    }
    CfrImg *img = &store->imgs[idx];

    /* c= makes that frame the current one (client-driven animation). */
    if (p->has_disp_cols && p->disp_cols >= 1)
        cfr_img_frame_set_current(vt, store, idx, p->disp_cols);

    /* r= with z= sets one frame's gap (r=1 is the root frame). */
    if (p->has_disp_rows && p->has_z_index)
        cfr_img_frame_set_gap(store, idx, p->disp_rows, p->z_index);

    /* s= is the playback state, v= the loop count (1 = infinite). */
    int mode = p->has_width ? p->width : 0;
    int loops = p->has_height ? p->height : 0;
    if (mode >= 1 && mode <= 3)
        cfr_img_anim_control(store, idx, mode, loops);

    if (!p->quiet)
        k_emit_image_ok(vt, img);
}

/* ------------------------------------------------------------------ */
/* Action: place (a=p)                                                */
/* ------------------------------------------------------------------ */

static void k_handle_place(CfrTerm *vt, const KittyParams *p)
{
    CfrImgStore *store = cfr_img_get_store(vt);
    if (!store)
        return;

    /* The lookup is scoped to kitty's id space (a Lottie animation with
     * the same number is a different image) and to the active screen
     * (the two buffers have separate graphics sets). An I= number names
     * the newest image carrying it. */
    int img_idx = k_resolve_image(vt, store, p);
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
        k_emit_image_ok(vt, img);
}

/* ------------------------------------------------------------------ */
/* Action: compose (a=c)                                             */
/* ------------------------------------------------------------------ */

static void k_handle_compose(CfrTerm *vt, const KittyParams *p)
{
    CfrImgStore *store = cfr_img_get_store(vt);
    if (!store) {
        if (!p->quiet)
            k_emit_ok(vt, p->image_id);
        return;
    }
    int idx = k_resolve_image(vt, store, p);
    if (idx < 0) {
        if (!p->quiet)
            k_emit_error(vt, p->image_id, "ENOENT:no image for compose");
        return;
    }
    CfrImg *img = &store->imgs[idx];

    /* r= is the source frame, c= the destination; x=/y= the destination
     * rect origin, X=/Y= the source's; w=/h= the rect size (defaulting
     * to the full image); C=1 overwrites instead of blending. */
    int src_n = p->has_disp_rows ? p->disp_rows : 0;
    int dst_n = p->has_disp_cols ? p->disp_cols : 0;
    if (src_n < 1 || src_n > img->frame_count + 1 ||
        dst_n < 1 || dst_n > img->frame_count + 1) {
        if (!p->quiet)
            k_emit_error(vt, p->image_id, "ENOENT:unknown frame");
        return;
    }
    int sx = (p->has_cell_off_x && p->cell_off_x > 0) ? p->cell_off_x : 0;
    int sy = (p->has_cell_off_y && p->cell_off_y > 0) ? p->cell_off_y : 0;
    int dx = (p->has_src_x && p->src_x > 0) ? p->src_x : 0;
    int dy = (p->has_src_y && p->src_y > 0) ? p->src_y : 0;
    int w = p->has_src_w ? p->src_w : 0;
    int h = p->has_src_h ? p->src_h : 0;
    bool overwrite = p->has_no_cursor_move && p->no_cursor_move == 1;

    if (!cfr_img_frame_compose(vt, store, idx, src_n, dst_n, sx, sy,
                               dx, dy, w, h, overwrite)) {
        if (!p->quiet)
            k_emit_error(vt, p->image_id, "EINVAL:compose rejected");
        return;
    }
    if (!p->quiet)
        k_emit_image_ok(vt, img);
}

/* ------------------------------------------------------------------ */
/* Action: delete (a=d)                                              */
/* ------------------------------------------------------------------ */

/* Does a placement match one delete predicate? The lowercase/uppercase
 * pair shares the predicate; the uppercase form additionally frees the
 * image data of matched images left without placements. x=/y= are
 * 1-based cells (kitty's rule), so the lookup converts them. */
static bool k_place_delete_matches(CfrTerm *vt, const KittyParams *p,
                                   int lower, const CfrPlacement *pl)
{
    int prow = (int)(pl->abs_line - vt->sixel_abs_top);
    switch (lower) {
    case 'a':
        /* Visible on screen: the placement's row extent pokes into the
         * viewport (kitty's clear_filter_func rule). */
        return prow <= vt->rows - 1 && prow + pl->rows - 1 >= 0;
    case 'c':
        return pl->col <= vt->cursor.col &&
               vt->cursor.col < pl->col + pl->cols &&
               prow <= vt->cursor.row && vt->cursor.row < prow + pl->rows;
    case 'p':
    {
        int cx = (p->has_src_x ? p->src_x : 1) - 1;
        int cy = (p->has_src_y ? p->src_y : 1) - 1;
        return pl->col <= cx && cx < pl->col + pl->cols &&
               prow <= cy && cy < prow + pl->rows;
    }
    case 'q':
    {
        int cx = (p->has_src_x ? p->src_x : 1) - 1;
        int cy = (p->has_src_y ? p->src_y : 1) - 1;
        return pl->col <= cx && cx < pl->col + pl->cols &&
               prow <= cy && cy < prow + pl->rows &&
               pl->z_index == p->z_index;
    }
    case 'x':
    {
        int cx = (p->has_src_x ? p->src_x : 1) - 1;
        return pl->col <= cx && cx < pl->col + pl->cols;
    }
    case 'y':
    {
        int cy = (p->has_src_y ? p->src_y : 1) - 1;
        return prow <= cy && cy < prow + pl->rows;
    }
    case 'z':
        return pl->z_index == p->z_index;
    default:
        return false;
    }
}

static void k_handle_delete(CfrTerm *vt, const KittyParams *p)
{
    /* A delete aborts a chunked upload in flight: kitty mandates that
     * "the partial upload must be aborted". */
    cfr_dealloc(vt, vt->kchunk.b64);
    cfr_dealloc(vt, vt->kchunk.ctrl);
    memset(&vt->kchunk, 0, sizeof(vt->kchunk));

    CfrImgStore *store = cfr_img_get_store(vt);
    if (!store) {
        if (!p->quiet)
            k_emit_ok(vt, p->image_id);
        return;
    }

    uint8_t screen = (uint8_t)vt->in_altscreen;
    int what = p->delete_what ? p->delete_what : 'a';
    int lower = what;
    bool free_data = false;
    if (lower >= 'A' && lower <= 'Z') {
        lower = lower - 'A' + 'a';
        free_data = true;
    }

    /* Handles of the images the command matched, for the data-freeing
     * pass. Bounded by the placement count; a delete is rare, so a
     * growing list beats a fixed cap that can silently miss victims. */
    uint64_t *matched = NULL;
    size_t matched_len = 0, matched_cap = 0;

#define RECORD_MATCH(handle)                                                  \
    do {                                                                      \
        if (matched_len == matched_cap) {                                     \
            size_t ncap = matched_cap ? matched_cap * 2 : 16;                 \
            uint64_t *nm = cfr_realloc(vt, matched, ncap * sizeof(uint64_t)); \
            if (!nm)                                                          \
                goto out;                                                     \
            matched = nm;                                                     \
            matched_cap = ncap;                                               \
        }                                                                     \
        matched[matched_len++] = (handle);                                    \
    } while (0)

    /* The image-naming deletes (d=i by id, d=n by number, d=f frames)
     * resolve once; the geometric ones need no image. */
    int named_idx = -1;
    bool named_freed = false;
    if (lower == 'i' || lower == 'n' || lower == 'f') {
        named_idx = k_resolve_image(vt, store, p);
        if (named_idx < 0)
            goto out;
    }

    if (named_idx >= 0 && free_data && !p->has_placement_id) {
        /* The uppercase form frees a never-placed image outright (kitty
         * special-cases I/N for images no placement filter can see). */
        bool placed = false;
        for (int j = 0; j < store->place_count; j++) {
            if (store->places[j].image_handle ==
                store->imgs[named_idx].handle) {
                placed = true;
                break;
            }
        }
        if (!placed) {
            cfr_img_remove(vt, store, named_idx);
            named_freed = true;
            goto out;
        }
    }

    switch (lower) {
    case 'i':
    case 'n':
        /* All placements of the named image — or, with p=, the one
         * placement carrying that id (placement-by-id deletion is
         * d=i,i=..,p=.., not d=p). */
        for (int i = 0; i < store->place_count;) {
            if (store->places[i].image_handle !=
                    store->imgs[named_idx].handle ||
                (p->has_placement_id &&
                 store->places[i].id != (uint64_t)p->placement_id)) {
                i++;
                continue;
            }
            RECORD_MATCH(store->places[i].image_handle);
            cfr_img_remove_placement(vt, store, i);
        }
        break;
    case 'r':
        /* Every image whose client id lies in the x=..y= range. */
        for (int i = 0; i < store->img_count; i++) {
            CfrImg *img = &store->imgs[i];
            if (img->source != IMG_SRC_KITTY || img->screen != screen ||
                !img->has_client_id)
                continue;
            uint64_t lo = (uint64_t)(p->has_src_x ? p->src_x : 0);
            uint64_t hi = (uint64_t)(p->has_src_y ? p->src_y : 0);
            if (img->id < lo || img->id > hi)
                continue;
            RECORD_MATCH(img->handle);
            for (int j = 0; j < store->place_count;) {
                if (store->places[j].image_handle == img->handle)
                    cfr_img_remove_placement(vt, store, j);
                else
                    j++;
            }
        }
        break;
    case 'f':
        /* Delete animation frames: r= names the frame (1 is the root
         * image itself and is never deleted); no r= clears them all. */
        cfr_img_frames_delete(vt, store, named_idx,
                              p->has_disp_rows ? p->disp_rows : 0);
        break;
    default:
        /* The geometric predicates (a, c, p, q, x, y, z) over the
         * placements of the active screen's kitty set. */
        for (int i = 0; i < store->place_count;) {
            CfrPlacement *pl = &store->places[i];
            if (pl->source != IMG_SRC_KITTY || pl->screen != screen ||
                !k_place_delete_matches(vt, p, lower, pl)) {
                i++;
                continue;
            }
            RECORD_MATCH(pl->image_handle);
            cfr_img_remove_placement(vt, store, i);
        }
        break;
    }

    /* The data-freeing pass: matched images left without placements are
     * released when the command was uppercase — or when the image has
     * no client id, since an id-less image can never be named again and
     * its data dies with its last placement (kitty's rule). */
    for (int i = 0; i < store->img_count;) {
        CfrImg *img = &store->imgs[i];
        if (img->source != IMG_SRC_KITTY || img->screen != screen) {
            i++;
            continue;
        }
        bool placed = false;
        for (int j = 0; j < store->place_count; j++) {
            if (store->places[j].image_handle == img->handle) {
                placed = true;
                break;
            }
        }
        if (placed) {
            i++;
            continue;
        }
        bool was_matched = false;
        for (size_t m = 0; m < matched_len; m++) {
            if (matched[m] == img->handle) {
                was_matched = true;
                break;
            }
        }
        if (was_matched &&
            (free_data || (!img->has_client_id && !img->has_image_num)))
            cfr_img_remove(vt, store, i);
        else
            i++;
    }

out:
#undef RECORD_MATCH
    cfr_dealloc(vt, matched);

    if (!p->quiet) {
        if (named_idx >= 0 && !named_freed)
            k_emit_image_ok(vt, &store->imgs[named_idx]);
        else
            k_emit_ok(vt, p->image_id);
    }
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

    /* Specifying both an image id and an image number in any command is
     * an error (kitty's rule). */
    if (p.has_image_id && p.has_image_num) {
        if (!p.quiet)
            k_emit_error(vt, p.image_id,
                         "EINVAL:must not specify both image id and image number");
        return;
    }

    /* Continuation chunks (m=1/m=0) carry no action key; they inherit
     * the action from the first chunk of the transfer. Without this
     * the dispatcher would fall through to the unknown-action error for
     * every chunk (chafa sends exactly this form). The final m=0 chunk
     * also needs the stashed action so a=T (transmit-and-place) still
     * places after the chunked upload completes. */
    if (p.action == 0 && p.has_more) {
        p.action = 't';
        if (vt->kchunk.active && vt->kchunk.ctrl) {
            KittyParams first;
            k_parse_params(vt->kchunk.ctrl, vt->kchunk.ctrl_len, &first);
            if (first.action)
                p.action = first.action;
        }
    }

    switch (p.action) {
    case 't':
        k_handle_transmit(vt, &p, (const char *)buf, ctrl_len,
                          payload, payload_len, false, false);
        break;
    case 'T':
        k_handle_transmit(vt, &p, (const char *)buf, ctrl_len,
                          payload, payload_len, false, true);
        break;
    case 'f':
        /* a=f: animation frame data (replaces pixels for the id). */
        k_handle_transmit(vt, &p, (const char *)buf, ctrl_len,
                          payload, payload_len, true, false);
        break;
    case 'a':
        /* a=a: animation control (current frame, gaps, playback). */
        k_handle_animate(vt, &p);
        break;
    case 'c':
        /* a=c: compose pixels from one frame onto another. */
        k_handle_compose(vt, &p);
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

/* ------------------------------------------------------------------ */
/* Public animation clock                                             */
/* ------------------------------------------------------------------ */

bool cfr_kitty_tick(CfrTerm *vt, uint64_t now_us)
{
    if (!vt || !vt->images)
        return false;
    cfr_img_tick(vt, vt->images, now_us);

    /* Report whether any kitty animation is still playing, so the host
     * can pace its frames (the lottie pattern). */
    for (int i = 0; i < vt->images->img_count; i++) {
        if (vt->images->imgs[i].source == IMG_SRC_KITTY &&
            vt->images->imgs[i].anim_mode != 0)
            return true;
    }
    return false;
}

bool cfr_kitty_present(CfrTerm *vt, uint64_t present_us, uint64_t hold_us)
{
    (void)hold_us; /* the kitty clock is frame-gap driven, not phase-sampled */
    return cfr_kitty_tick(vt, present_us);
}

int cfr_kitty_playing_count(CfrTerm *vt)
{
    if (!vt || !vt->images)
        return 0;
    int n = 0;
    for (int i = 0; i < vt->images->img_count; i++) {
        if (vt->images->imgs[i].source == IMG_SRC_KITTY &&
            vt->images->imgs[i].anim_mode != 0)
            n++;
    }
    return n;
}
