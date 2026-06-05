/*
 * bloom-vt — sixel graphics: DCS decoder + grid-anchored image store.
 *
 * The engine owns sixel end-to-end. A `DCS <P1;P2;P3> q <data> ST`
 * sequence is intercepted in dcs.c (final byte 'q') and streamed here:
 * bvt_sixel_begin / bvt_sixel_put / bvt_sixel_finish. The decoder turns
 * the payload into an RGBA bitmap; on finish the image is anchored to an
 * absolute grid line (so it scrolls and enters scrollback with the text
 * it sits on) and the cursor advances below it.
 *
 * Memory model — two tiers, different strategies:
 *
 *   Tier 1 (metadata): a dense, geometrically-growing array of records.
 *   Deletion is swap-remove (O(1), contiguous, no fragmentation). The
 *   stable uint64_t id decouples a record from its array slot so the
 *   host's id-keyed texture cache is immune to the shuffle.
 *
 *   Tier 2 (pixel buffers): a small best-fit free-list pool. Released
 *   buffers are retained (capped) and reused by the next alloc whose need
 *   fits — so an animation streaming same-size frames recycles one buffer
 *   with no malloc/free churn and no heap fragmentation.
 *
 * Budgets bound the worst case: a per-dimension clamp, a global live-byte
 * budget that evicts the oldest image first (which is also scroll-off
 * order), and a retained-pool ceiling.
 *
 * Single-threaded: all mutation happens under bvt_input_write and reads
 * under bvt_get_sixels, on the caller's one thread. No locking.
 */

#include "bloom_vt_internal.h"

#include <stdio.h>
#include <string.h>

typedef struct BvtSixelState BvtSixelState;

/* ------------------------------------------------------------------ */
/* Tunables                                                            */
/* ------------------------------------------------------------------ */

#define SX_MAX_COLORS 256
#define SX_BAND       6   /* pixels per sixel band */
#define SX_INIT_W     512 /* initial decode canvas */
#define SX_INIT_H     128
#define SX_MAX_DIM    10000                  /* per-dimension clamp */
#define SX_MAX_IMAGES 256                    /* live record cap */
#define SX_LIVE_MAX   (128u * 1024u * 1024u) /* live pixel-byte budget */
#define SX_SPARE_MAX  16                     /* retained free buffers */
#define SX_RETAIN_MAX (32u * 1024u * 1024u)  /* retained free bytes */

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

typedef struct
{
    uint8_t r, g, b;
} SxColor;

typedef enum
{
    SX_NORMAL,
    SX_COLOR,  /* after '#' */
    SX_REPEAT, /* after '!' */
    SX_RASTER, /* after '"' */
} SxSubState;

/* One stored image. */
typedef struct
{
    uint64_t id;
    uint32_t version;
    uint8_t layer; /* 0 = foreground */
    long abs_line; /* absolute line index of the image's top row */
    int col;       /* anchor column */
    int w, h;      /* pixels */
    int rows_tall; /* cells tall (cached for cull/clear) */
    uint8_t *rgba; /* pixel buffer */
    size_t cap;    /* allocated bytes of `rgba` */
} SxRec;

/* A retained free buffer. */
typedef struct
{
    uint8_t *ptr;
    size_t cap;
} SxSpare;

struct BvtSixelState
{
    /* Decode canvas (reused across images; grows, never handed out). */
    uint8_t *canvas;
    int canvas_w, canvas_h;
    size_t canvas_cap;

    /* Decode position / extent. */
    int x, y, max_x, max_y;

    /* Palette + current register. */
    SxColor palette[SX_MAX_COLORS];
    int cur_color;

    /* DCS params for the in-flight image. */
    int p2; /* background mode (0/2 opaque, 1 transparent) */

    /* Sub-state machine. */
    SxSubState sub;
    int params[5];
    int param_count;
    int acc;
    bool has_acc;

    /* Anchor captured at begin. */
    long anchor_abs_line;
    int anchor_row, anchor_col;
    bool active;  /* between begin and finish */
    bool dropped; /* image rejected (OOM / oversize) — finish is a no-op */

    /* Store (tier 1). */
    SxRec *recs;
    int rec_count, rec_cap;
    uint64_t next_id;
    size_t live_bytes;

    /* Query scratch — reused, grown, never freed between calls. */
    BvtSixel *scratch;
    int scratch_cap;

    /* Pixel-buffer pool (tier 2). */
    SxSpare spares[SX_SPARE_MAX];
    int spare_count;
    size_t retain_bytes;
};

/* VT340 default 16-color palette, RGB (DEC percentages → 0-255). Applies
 * only when a sequence selects a register it never defined; real encoders
 * define their own colors. */
/* clang-format off */
static const SxColor sx_default_palette[16] = {
    {   0,   0,   0 }, {  51,  51, 204 }, { 204,  33,  33 }, {  51, 204,  51 },
    { 204,  51, 204 }, {  51, 204, 204 }, { 204, 204,  51 }, { 135, 135, 135 },
    {  66,  66,  66 }, {  84,  84, 153 }, { 153,  66,  66 }, {  84, 153,  84 },
    { 153,  84, 153 }, {  84, 153, 153 }, { 153, 153,  84 }, { 204, 204, 204 },
};
/* clang-format on */

/* ------------------------------------------------------------------ */
/* Pixel-buffer pool (tier 2)                                          */
/* ------------------------------------------------------------------ */

/* Best-fit pop a retained buffer with cap >= need, else malloc exactly. */
static uint8_t *sx_buf_alloc(BvtTerm *vt, BvtSixelState *st, size_t need,
                             size_t *out_cap)
{
    int best = -1;
    for (int i = 0; i < st->spare_count; ++i) {
        if (st->spares[i].cap >= need &&
            (best < 0 || st->spares[i].cap < st->spares[best].cap))
            best = i;
    }
    if (best >= 0) {
        uint8_t *p = st->spares[best].ptr;
        size_t cap = st->spares[best].cap;
        st->retain_bytes -= cap;
        st->spares[best] = st->spares[--st->spare_count];
        *out_cap = cap;
        return p;
    }
    uint8_t *p = bvt_alloc(vt, need);
    *out_cap = p ? need : 0;
    return p;
}

/* Retain a freed buffer for reuse, or free it if the pool is full. */
static void sx_buf_release(BvtTerm *vt, BvtSixelState *st, uint8_t *ptr,
                           size_t cap)
{
    if (!ptr)
        return;
    if (st->spare_count < SX_SPARE_MAX &&
        st->retain_bytes + cap <= SX_RETAIN_MAX) {
        st->spares[st->spare_count].ptr = ptr;
        st->spares[st->spare_count].cap = cap;
        st->spare_count++;
        st->retain_bytes += cap;
        return;
    }
    bvt_dealloc(vt, ptr);
}

/* ------------------------------------------------------------------ */
/* Store (tier 1)                                                      */
/* ------------------------------------------------------------------ */

static void sx_rec_release(BvtTerm *vt, BvtSixelState *st, int idx)
{
    SxRec *r = &st->recs[idx];
    st->live_bytes -= r->cap;
    sx_buf_release(vt, st, r->rgba, r->cap);
    st->recs[idx] = st->recs[--st->rec_count]; /* swap-remove */
}

/* Evict oldest (lowest abs_line) images until `incoming` more bytes fit. */
static void sx_evict_to_budget(BvtTerm *vt, BvtSixelState *st, size_t incoming)
{
    while (st->rec_count > 0 && st->live_bytes + incoming > SX_LIVE_MAX) {
        int m = 0;
        for (int i = 1; i < st->rec_count; ++i)
            if (st->recs[i].abs_line < st->recs[m].abs_line)
                m = i;
        sx_rec_release(vt, st, m);
    }
}

/* Find an existing record at the same anchor + layer (for animation /
 * in-place frame replacement). Returns index or -1. */
static int sx_find_at(BvtSixelState *st, long abs_line, int col, uint8_t layer)
{
    for (int i = 0; i < st->rec_count; ++i)
        if (st->recs[i].abs_line == abs_line && st->recs[i].col == col &&
            st->recs[i].layer == layer)
            return i;
    return -1;
}

/* ------------------------------------------------------------------ */
/* Decode canvas                                                       */
/* ------------------------------------------------------------------ */

static bool sx_canvas_ensure(BvtTerm *vt, BvtSixelState *st, int need_x,
                             int need_y)
{
    int nw = st->canvas_w;
    int nh = st->canvas_h;
    while (nw <= need_x)
        nw *= 2;
    while (nh <= need_y)
        nh += SX_INIT_H;
    if (nw > SX_MAX_DIM)
        nw = SX_MAX_DIM;
    if (nh > SX_MAX_DIM)
        nh = SX_MAX_DIM;
    if (nw == st->canvas_w && nh == st->canvas_h)
        return need_x < st->canvas_w && need_y < st->canvas_h;

    size_t need_bytes = (size_t)nw * (size_t)nh * 4u;
    if (need_bytes > st->canvas_cap) {
        uint8_t *nb = bvt_alloc(vt, need_bytes);
        if (!nb)
            return false;
        memset(nb, 0, need_bytes);
        /* Copy existing rows into the wider canvas. */
        for (int row = 0; row < st->canvas_h && row < nh; ++row)
            memcpy(nb + (size_t)row * nw * 4,
                   st->canvas + (size_t)row * st->canvas_w * 4,
                   (size_t)st->canvas_w * 4);
        bvt_dealloc(vt, st->canvas);
        st->canvas = nb;
        st->canvas_cap = need_bytes;
    } else {
        /* Reusing a larger existing allocation at a new (narrower or
         * equal-width) geometry: re-layout would be needed if width
         * changed. We only grow geometry, so width only increases — and
         * that path took the realloc branch above. Equal width here. */
    }
    st->canvas_w = nw;
    st->canvas_h = nh;
    return need_x < nw && need_y < nh;
}

/* HLS → RGB. The standard HSL helper expects hue 0°=red; the caller maps
 * the DEC sixel hue (0°=blue) before calling. */
static SxColor sx_hls_to_rgb(int h, int l, int s)
{
    SxColor out = { 0, 0, 0 };
    if (s == 0) {
        uint8_t v = (uint8_t)(l * 255 / 100);
        out.r = out.g = out.b = v;
        return out;
    }
    double hue = h / 360.0;
    double lum = l / 100.0;
    double sat = s / 100.0;
    double m2 = (lum <= 0.5) ? lum * (1.0 + sat) : lum + sat - lum * sat;
    double m1 = 2.0 * lum - m2;
    double t[3] = { hue + 1.0 / 3.0, hue, hue - 1.0 / 3.0 };
    uint8_t *o[3] = { &out.r, &out.g, &out.b };
    for (int i = 0; i < 3; ++i) {
        double tc = t[i];
        if (tc < 0)
            tc += 1.0;
        if (tc > 1)
            tc -= 1.0;
        double v;
        if (tc < 1.0 / 6.0)
            v = m1 + (m2 - m1) * 6.0 * tc;
        else if (tc < 0.5)
            v = m2;
        else if (tc < 2.0 / 3.0)
            v = m1 + (m2 - m1) * (2.0 / 3.0 - tc) * 6.0;
        else
            v = m1;
        *o[i] = (uint8_t)(v * 255.0 + 0.5);
    }
    return out;
}

static void sx_draw(BvtTerm *vt, BvtSixelState *st, uint8_t bits)
{
    if (!sx_canvas_ensure(vt, st, st->x, st->y + SX_BAND - 1)) {
        /* Canvas hit the clamp; advance position but drop the pixels so a
         * pathological sequence cannot run away. */
        st->x++;
        return;
    }
    SxColor c = st->palette[st->cur_color % SX_MAX_COLORS];
    for (int bit = 0; bit < SX_BAND; ++bit) {
        if (bits & (1u << bit)) {
            int py = st->y + bit;
            if (py < st->canvas_h && st->x < st->canvas_w) {
                size_t off = ((size_t)py * st->canvas_w + st->x) * 4;
                st->canvas[off + 0] = c.r;
                st->canvas[off + 1] = c.g;
                st->canvas[off + 2] = c.b;
                st->canvas[off + 3] = 255; /* set pixel = opaque */
            }
        }
    }
    if (st->x > st->max_x)
        st->max_x = st->x;
    int band_bottom = st->y + SX_BAND - 1;
    if (band_bottom > st->max_y)
        st->max_y = band_bottom;
    st->x++;
}

static void sx_finish_color(BvtSixelState *st)
{
    if (st->param_count == 0)
        return;
    int idx = st->params[0] % SX_MAX_COLORS;
    if (idx < 0)
        idx += SX_MAX_COLORS;
    if (st->param_count == 1) {
        st->cur_color = idx;
        return;
    }
    if (st->param_count >= 5) {
        int pu = st->params[1];
        int a = st->params[2], b = st->params[3], c = st->params[4];
        if (pu == 1) {
            /* DEC HLS: hue 0°=blue, 120°=red, 240°=green. Map to the
             * standard-HSL convention (0°=red) the helper expects. */
            int hue = ((a % 360) + 240) % 360;
            st->palette[idx] = sx_hls_to_rgb(hue, b, c);
        } else if (pu == 2) {
            /* RGB, each component 0-100. */
            st->palette[idx].r = (uint8_t)(a * 255 / 100);
            st->palette[idx].g = (uint8_t)(b * 255 / 100);
            st->palette[idx].b = (uint8_t)(c * 255 / 100);
        }
    }
    st->cur_color = idx;
}

static void sx_finish_raster(BvtTerm *vt, BvtSixelState *st)
{
    /* "Pan;Pad;Ph;Pv — aspect num/den + width/height. We render 1:1 like
     * xterm/foot/wezterm (aspect parsed but not applied) and use Ph/Pv as
     * a canvas pre-allocation hint. */
    if (st->param_count >= 4) {
        int hw = st->params[2], hh = st->params[3];
        if (hw > 0 && hw < SX_MAX_DIM && hh > 0 && hh < SX_MAX_DIM)
            sx_canvas_ensure(vt, st, hw - 1, hh - 1);
    }
}

/* ------------------------------------------------------------------ */
/* DCS lifecycle                                                       */
/* ------------------------------------------------------------------ */

static BvtSixelState *sx_state(BvtTerm *vt)
{
    if (vt->sixel)
        return vt->sixel;
    BvtSixelState *st = bvt_alloc(vt, sizeof(*st));
    if (!st)
        return NULL;
    memset(st, 0, sizeof(*st));
    st->next_id = 1;
    vt->sixel = st;
    return st;
}

void bvt_sixel_begin(BvtTerm *vt, const uint32_t *params, int nparams)
{
    BvtSixelState *st = sx_state(vt);
    if (!st)
        return;

    /* Settle the cursor and make sure the grid exists — placement,
     * scroll, and clearing all operate on it. */
    bvt_flush_cluster(vt);
    bvt_grid_ensure(vt);

    /* Fresh decode canvas (transparent). Private color registers (mode
     * 1070) are the default; we reset the palette per image. Shared
     * registers (1070 off) are accepted-but-not-differentiated for now —
     * real encoders define their colors each image. */
    if (!st->canvas) {
        size_t bytes = (size_t)SX_INIT_W * SX_INIT_H * 4u;
        st->canvas = bvt_alloc(vt, bytes);
        st->canvas_cap = st->canvas ? bytes : 0;
        st->canvas_w = SX_INIT_W;
        st->canvas_h = SX_INIT_H;
    }
    if (!st->canvas) {
        st->dropped = true;
        st->active = true;
        return;
    }
    memset(st->canvas, 0, st->canvas_cap);

    st->x = st->y = st->max_x = st->max_y = 0;
    st->cur_color = 0;
    st->sub = SX_NORMAL;
    st->param_count = 0;
    st->acc = 0;
    st->has_acc = false;
    st->dropped = false;
    st->active = true;

    /* P2: 0/2 = opaque background, 1 = transparent. We always leave unset
     * pixels transparent (alpha 0) and set pixels opaque — the image
     * composites over live terminal content, which supplies the
     * background; painting an opaque register-0 rectangle would wrongly
     * occlude it. P2 is captured for completeness. */
    st->p2 = (nparams >= 2) ? (int)params[1] : 0;

    for (int i = 0; i < 16; ++i)
        st->palette[i] = sx_default_palette[i];
    for (int i = 16; i < SX_MAX_COLORS; ++i)
        st->palette[i] = (SxColor){ 0, 0, 0 };

    st->anchor_abs_line = vt->sixel_abs_top + vt->cursor.row;
    st->anchor_row = vt->cursor.row;
    st->anchor_col = vt->cursor.col;
}

void bvt_sixel_put(BvtTerm *vt, const uint8_t *data, size_t len)
{
    BvtSixelState *st = vt->sixel;
    if (!st || !st->active || st->dropped || !data)
        return;

    for (size_t i = 0; i < len; ++i) {
        unsigned char ch = data[i];
        switch (st->sub) {
        case SX_COLOR:
            if (ch >= '0' && ch <= '9') {
                st->acc = st->acc * 10 + (ch - '0');
                st->has_acc = true;
            } else if (ch == ';') {
                if (st->param_count < 5)
                    st->params[st->param_count++] = st->acc;
                st->acc = 0;
                st->has_acc = false;
            } else {
                if (st->has_acc && st->param_count < 5)
                    st->params[st->param_count++] = st->acc;
                sx_finish_color(st);
                st->sub = SX_NORMAL;
                st->acc = 0;
                st->has_acc = false;
                --i; /* reprocess in NORMAL */
            }
            break;

        case SX_REPEAT:
            if (ch >= '0' && ch <= '9') {
                st->acc = st->acc * 10 + (ch - '0');
                st->has_acc = true;
            } else {
                int count = st->has_acc ? st->acc : 1;
                st->sub = SX_NORMAL;
                st->acc = 0;
                st->has_acc = false;
                if (ch >= '?' && ch <= '~') {
                    uint8_t bits = (uint8_t)(ch - '?');
                    for (int r = 0; r < count; ++r)
                        sx_draw(vt, st, bits);
                }
                /* a non-data byte after !Pn is malformed → consumed */
            }
            break;

        case SX_RASTER:
            if (ch >= '0' && ch <= '9') {
                st->acc = st->acc * 10 + (ch - '0');
                st->has_acc = true;
            } else if (ch == ';') {
                if (st->param_count < 5)
                    st->params[st->param_count++] = st->acc;
                st->acc = 0;
                st->has_acc = false;
            } else {
                if (st->has_acc && st->param_count < 5)
                    st->params[st->param_count++] = st->acc;
                sx_finish_raster(vt, st);
                st->sub = SX_NORMAL;
                st->acc = 0;
                st->has_acc = false;
                --i;
            }
            break;

        case SX_NORMAL:
        default:
            if (ch >= '?' && ch <= '~') {
                sx_draw(vt, st, (uint8_t)(ch - '?'));
            } else if (ch == '#') {
                st->sub = SX_COLOR;
                st->param_count = 0;
                st->acc = 0;
                st->has_acc = false;
            } else if (ch == '!') {
                st->sub = SX_REPEAT;
                st->acc = 0;
                st->has_acc = false;
            } else if (ch == '"') {
                st->sub = SX_RASTER;
                st->param_count = 0;
                st->acc = 0;
                st->has_acc = false;
            } else if (ch == '$') {
                st->x = 0; /* graphics CR */
            } else if (ch == '-') {
                st->x = 0; /* graphics NL */
                st->y += SX_BAND;
            }
            /* else: ignore (whitespace, control bytes) */
            break;
        }
    }
}

/* Damage the visible rows an image at display row `top` spanning
 * `rows_tall` rows occupies, so the host redraws and re-queries. */
static void sx_damage_image(BvtTerm *vt, int top, int rows_tall)
{
    int a = top < 0 ? 0 : top;
    int b = top + rows_tall - 1;
    if (b >= vt->rows)
        b = vt->rows - 1;
    for (int r = a; r <= b; ++r)
        bvt_damage_row(vt, r);
}

/* Move the cursor below a placed image and scroll the grid as needed,
 * mirroring linefeed semantics so abs_top + scrollback stay consistent. */
static void sx_advance_cursor(BvtTerm *vt, int rows_tall, int cols_wide)
{
    if (vt->modes[BVT_MODE_SIXEL_SCROLLING]) {
        /* DECSDM on: draw in place, leave the cursor put (animation). */
        return;
    }
    if (vt->modes[BVT_MODE_SIXEL_CURSOR_RIGHT]) {
        /* Mode 8452: cursor to the upper-right of the graphic. */
        int c = vt->cursor.col + cols_wide;
        if (c > vt->cols - 1)
            c = vt->cols - 1;
        vt->cursor.col = c;
        vt->cursor.pending_wrap = false;
        return;
    }
    /* Default: drop to the line below the image, same starting column. */
    int col = vt->cursor.col;
    for (int i = 0; i < rows_tall; ++i) {
        if (vt->cursor.row == vt->scroll_bottom)
            bvt_scroll_up(vt, 1);
        else if (vt->cursor.row < vt->rows - 1)
            vt->cursor.row++;
    }
    vt->cursor.col = col;
    vt->cursor.pending_wrap = false;
}

void bvt_sixel_finish(BvtTerm *vt)
{
    BvtSixelState *st = vt->sixel;
    if (!st || !st->active)
        return;
    st->active = false;
    if (st->dropped)
        return;

    int w = st->max_x + 1;
    int h = st->max_y + 1;
    if (w <= 0 || h <= 0 || !st->canvas)
        return;
    if (w > SX_MAX_DIM)
        w = SX_MAX_DIM;
    if (h > SX_MAX_DIM)
        h = SX_MAX_DIM;

    size_t need = (size_t)w * (size_t)h * 4u;
    if (need == 0 || need > SX_LIVE_MAX)
        return; /* single image larger than the whole budget → drop */

    long abs_line = st->anchor_abs_line;
    int col = st->anchor_col;
    uint8_t layer = 0;

    int cell_h = vt->sixel_cell_h > 0 ? vt->sixel_cell_h : h;
    int cell_w = vt->sixel_cell_w > 0 ? vt->sixel_cell_w : w;
    int rows_tall = (h + cell_h - 1) / cell_h;
    int cols_wide = (w + cell_w - 1) / cell_w;
    if (rows_tall < 1)
        rows_tall = 1;
    if (cols_wide < 1)
        cols_wide = 1;

    /* Animation / in-place replacement: a new image at the same anchor
     * replaces the old one, reusing its buffer when it fits. */
    int existing = sx_find_at(st, abs_line, col, layer);
    SxRec *r;
    if (existing >= 0) {
        r = &st->recs[existing];
        if (r->cap < need) {
            st->live_bytes -= r->cap;
            sx_buf_release(vt, st, r->rgba, r->cap);
            size_t cap = 0;
            uint8_t *buf = sx_buf_alloc(vt, st, need, &cap);
            if (!buf) {
                /* lost the old buffer; drop the record */
                st->recs[existing] = st->recs[--st->rec_count];
                return;
            }
            r->rgba = buf;
            r->cap = cap;
            st->live_bytes += cap;
        }
        r->version++;
    } else {
        if (st->rec_count >= SX_MAX_IMAGES)
            sx_rec_release(vt, st, 0); /* drop one to make room */
        sx_evict_to_budget(vt, st, need);
        if (st->rec_count >= st->rec_cap) {
            int ncap = st->rec_cap ? st->rec_cap * 2 : 8;
            if (ncap > SX_MAX_IMAGES)
                ncap = SX_MAX_IMAGES;
            SxRec *nr = bvt_realloc(vt, st->recs, (size_t)ncap * sizeof(SxRec));
            if (!nr)
                return;
            st->recs = nr;
            st->rec_cap = ncap;
        }
        size_t cap = 0;
        uint8_t *buf = sx_buf_alloc(vt, st, need, &cap);
        if (!buf)
            return;
        r = &st->recs[st->rec_count++];
        r->id = st->next_id++;
        r->version = 1;
        r->rgba = buf;
        r->cap = cap;
        st->live_bytes += cap;
    }

    r->layer = layer;
    r->abs_line = abs_line;
    r->col = col;
    r->w = w;
    r->h = h;
    r->rows_tall = rows_tall;

    /* Copy the cropped image out of the (wider) canvas. */
    for (int row = 0; row < h; ++row)
        memcpy(r->rgba + (size_t)row * w * 4,
               st->canvas + (size_t)row * st->canvas_w * 4, (size_t)w * 4);

    int disp_row = (int)(abs_line - vt->sixel_abs_top);
    sx_advance_cursor(vt, rows_tall, cols_wide);
    sx_damage_image(vt, disp_row, rows_tall);
}

/* ------------------------------------------------------------------ */
/* Grid maintenance                                                    */
/* ------------------------------------------------------------------ */

void bvt_sixel_note_scroll(BvtTerm *vt, int lines)
{
    BvtSixelState *st = vt->sixel;
    if (!st || st->rec_count == 0)
        return;
    (void)lines;
    /* Cull images whose bottom row has scrolled past the oldest retained
     * scrollback line. depth = lines of `abs_line` above grid row 0. */
    int cap = vt->sb_capacity;
    for (int i = 0; i < st->rec_count;) {
        long depth = vt->sixel_abs_top - st->recs[i].abs_line;
        long bottom_depth = depth - st->recs[i].rows_tall + 1;
        if (bottom_depth > cap)
            sx_rec_release(vt, st, i); /* swap-remove: re-test index i */
        else
            ++i;
    }
}

void bvt_sixel_clear_display_rows(BvtTerm *vt, int top, int bot)
{
    BvtSixelState *st = vt->sixel;
    if (!st || st->rec_count == 0)
        return;
    for (int i = 0; i < st->rec_count;) {
        if (st->recs[i].layer != 0) {
            ++i;
            continue;
        }
        int rtop = (int)(st->recs[i].abs_line - vt->sixel_abs_top);
        int rbot = rtop + st->recs[i].rows_tall - 1;
        if (rtop <= bot && rbot >= top)
            sx_rec_release(vt, st, i);
        else
            ++i;
    }
}

void bvt_sixel_clear_all(BvtTerm *vt)
{
    BvtSixelState *st = vt->sixel;
    if (!st)
        return;
    while (st->rec_count > 0)
        sx_rec_release(vt, st, st->rec_count - 1);
}

void bvt_sixel_state_free(BvtTerm *vt)
{
    BvtSixelState *st = vt->sixel;
    if (!st)
        return;
    for (int i = 0; i < st->rec_count; ++i)
        bvt_dealloc(vt, st->recs[i].rgba);
    for (int i = 0; i < st->spare_count; ++i)
        bvt_dealloc(vt, st->spares[i].ptr);
    bvt_dealloc(vt, st->recs);
    bvt_dealloc(vt, st->scratch);
    bvt_dealloc(vt, st->canvas);
    bvt_dealloc(vt, st);
    vt->sixel = NULL;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void bvt_set_cell_pixels(BvtTerm *vt, int cell_w_px, int cell_h_px)
{
    if (!vt)
        return;
    vt->sixel_cell_w = cell_w_px > 0 ? cell_w_px : 0;
    vt->sixel_cell_h = cell_h_px > 0 ? cell_h_px : 0;
}

const BvtSixel *bvt_get_sixels(BvtTerm *vt, int *out_count)
{
    if (out_count)
        *out_count = 0;
    if (!vt || !vt->sixel || vt->sixel->rec_count == 0)
        return NULL;
    BvtSixelState *st = vt->sixel;

    if (st->scratch_cap < st->rec_count) {
        int ncap = st->scratch_cap ? st->scratch_cap * 2 : 8;
        while (ncap < st->rec_count)
            ncap *= 2;
        BvtSixel *ns = bvt_realloc(vt, st->scratch, (size_t)ncap * sizeof(BvtSixel));
        if (!ns)
            return NULL;
        st->scratch = ns;
        st->scratch_cap = ncap;
    }

    for (int i = 0; i < st->rec_count; ++i) {
        SxRec *r = &st->recs[i];
        BvtSixel *v = &st->scratch[i];
        v->id = r->id;
        v->version = r->version;
        v->layer = r->layer;
        v->row = (int)(r->abs_line - vt->sixel_abs_top);
        v->col = r->col;
        v->width_px = r->w;
        v->height_px = r->h;
        v->rgba = r->rgba;
    }
    if (out_count)
        *out_count = st->rec_count;
    return st->scratch;
}
