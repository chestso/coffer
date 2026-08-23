/*
 * coffer — sixel graphics: DCS decoder + grid-anchored image store.
 *
 * The engine owns sixel end-to-end. A `DCS <P1;P2;P3> q <data> ST`
 * sequence is intercepted in dcs.c (final byte 'q') and streamed here:
 * cfr_sixel_begin / cfr_sixel_put / cfr_sixel_finish. The decoder turns
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
 * Single-threaded: all mutation happens under cfr_input_write and reads
 * under cfr_get_images, on the caller's one thread. No locking.
 */

#include "coffer_internal.h"

#include <stdio.h>
#include <string.h>

typedef struct CfrSixelState CfrSixelState;

/* ------------------------------------------------------------------ */
/* Tunables                                                            */
/* ------------------------------------------------------------------ */

#define SX_MAX_COLORS 256
#define SX_BAND       6   /* pixels per sixel band */
#define SX_INIT_W     512 /* initial decode canvas */
#define SX_INIT_H     128
/* Storage budgets (IMG_MAX_DIM, IMG_LIVE_MAX, etc.) are in image_store.h. */

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

/* SxRec and SxSpare moved to image_store.h as CfrImg and ImgSpare. */

struct CfrSixelState
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

    /* Shared image store — owns all image records, buffer pool, and
     * query scratch. Separated from the DCS decode state so that iTerm2
     * and kitty graphics can share the same store. */
    CfrImgStore *store;
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

/* Buffer pool, record lifecycle, eviction, and find_at moved to
 * image_store.c (img_buf_alloc, img_buf_release, img_rec_release,
 * img_evict_to_budget, cfr_img_find_at). */

/* ------------------------------------------------------------------ */
/* Decode canvas                                                       */
/* ------------------------------------------------------------------ */

static bool sx_canvas_ensure(CfrTerm *vt, CfrSixelState *st, int need_x,
                             int need_y)
{
    int nw = st->canvas_w;
    int nh = st->canvas_h;
    while (nw <= need_x)
        nw *= 2;
    while (nh <= need_y)
        nh += SX_INIT_H;
    if (nw > IMG_MAX_DIM)
        nw = IMG_MAX_DIM;
    if (nh > IMG_MAX_DIM)
        nh = IMG_MAX_DIM;
    if (nw == st->canvas_w && nh == st->canvas_h)
        return need_x < st->canvas_w && need_y < st->canvas_h;

    size_t need_bytes = (size_t)nw * (size_t)nh * 4u;
    if (need_bytes > st->canvas_cap) {
        uint8_t *nb = cfr_alloc(vt, need_bytes);
        if (!nb)
            return false;
        memset(nb, 0, need_bytes);
        /* Copy existing rows into the wider canvas. */
        for (int row = 0; row < st->canvas_h && row < nh; ++row)
            memcpy(nb + (size_t)row * nw * 4,
                   st->canvas + (size_t)row * st->canvas_w * 4,
                   (size_t)st->canvas_w * 4);
        cfr_dealloc(vt, st->canvas);
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

static void sx_draw(CfrTerm *vt, CfrSixelState *st, uint8_t bits)
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

static void sx_finish_color(CfrSixelState *st)
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

static void sx_finish_raster(CfrTerm *vt, CfrSixelState *st)
{
    /* "Pan;Pad;Ph;Pv — aspect num/den + width/height. We render 1:1 like
     * xterm/foot/wezterm (aspect parsed but not applied) and use Ph/Pv as
     * a canvas pre-allocation hint. */
    if (st->param_count >= 4) {
        int hw = st->params[2], hh = st->params[3];
        if (hw > 0 && hw < IMG_MAX_DIM && hh > 0 && hh < IMG_MAX_DIM)
            sx_canvas_ensure(vt, st, hw - 1, hh - 1);
    }
}

/* ------------------------------------------------------------------ */
/* DCS lifecycle                                                       */
/* ------------------------------------------------------------------ */

static CfrSixelState *sx_state(CfrTerm *vt)
{
    if (vt->sixel)
        return vt->sixel;
    CfrSixelState *st = cfr_alloc(vt, sizeof(*st));
    if (!st)
        return NULL;
    memset(st, 0, sizeof(*st));
    /* Adopt the shared store if it was already created by OSC 1337,
     * otherwise create a new one. */
    if (vt->images) {
        st->store = vt->images;
    } else {
        st->store = cfr_img_store_new(vt);
        if (!st->store) {
            cfr_dealloc(vt, st);
            return NULL;
        }
        vt->images = st->store;
    }
    vt->sixel = st;
    return st;
}

void cfr_sixel_begin(CfrTerm *vt, const uint32_t *params, int nparams)
{
    CfrSixelState *st = sx_state(vt);
    if (!st)
        return;

    /* Settle the cursor and make sure the grid exists — placement,
     * scroll, and clearing all operate on it. */
    cfr_flush_cluster(vt);
    cfr_grid_ensure(vt);

    /* Fresh decode canvas (transparent). Private color registers (mode
     * 1070) are the default; we reset the palette per image. Shared
     * registers (1070 off) are accepted-but-not-differentiated for now —
     * real encoders define their colors each image. */
    if (!st->canvas) {
        size_t bytes = (size_t)SX_INIT_W * SX_INIT_H * 4u;
        st->canvas = cfr_alloc(vt, bytes);
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

void cfr_sixel_put(CfrTerm *vt, const uint8_t *data, size_t len)
{
    CfrSixelState *st = vt->sixel;
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

/* sx_damage_image and sx_advance_cursor moved to image_store.c
 * (img_damage, img_advance_cursor). cfr_img_add handles both. */

void cfr_sixel_finish(CfrTerm *vt)
{
    CfrSixelState *st = vt->sixel;
    if (!st || !st->active)
        return;
    st->active = false;
    if (st->dropped)
        return;

    int w = st->max_x + 1;
    int h = st->max_y + 1;
    if (w <= 0 || h <= 0 || !st->canvas)
        return;
    if (w > IMG_MAX_DIM)
        w = IMG_MAX_DIM;
    if (h > IMG_MAX_DIM)
        h = IMG_MAX_DIM;

    size_t need = (size_t)w * (size_t)h * 4u;
    if (need == 0 || need > IMG_LIVE_MAX)
        return;

    /* Crop the image out of the (wider) canvas into a contiguous buffer. */
    uint8_t *rgba = cfr_alloc(vt, need);
    if (!rgba)
        return;
    for (int row = 0; row < h; ++row)
        memcpy(rgba + (size_t)row * w * 4,
               st->canvas + (size_t)row * st->canvas_w * 4, (size_t)w * 4);

    /* Delegate to the shared store: handles buffer pool, eviction,
     * cursor advancement, and damage. */
    cfr_img_add(vt, st->store, rgba, w, h, 0, IMG_SRC_SIXEL);
    cfr_dealloc(vt, rgba);
}

/* ------------------------------------------------------------------ */
/* Grid maintenance                                                    */
/* ------------------------------------------------------------------ */

void cfr_sixel_state_free(CfrTerm *vt)
{
    CfrSixelState *st = vt->sixel;
    if (!st)
        return;
    /* The store (vt->images) is freed separately in cfr_free() since it
     * may have been created by OSC 1337 before the sixel state existed. */
    cfr_dealloc(vt, st->canvas);
    cfr_dealloc(vt, st);
    vt->sixel = NULL;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void cfr_set_cell_pixels(CfrTerm *vt, int cell_w_px, int cell_h_px)
{
    if (!vt || cell_w_px <= 0 || cell_h_px <= 0)
        return;
    vt->cell_w_px = cell_w_px;
    vt->cell_h_px = cell_h_px;
}

void cfr_set_content_scale(CfrTerm *vt, float scale)
{
    if (!vt)
        return;
    vt->content_scale = scale > 0.0f ? scale : 1.0f;
}

const CfrImage *cfr_get_images(CfrTerm *vt, int *out_count)
{
    if (!vt || !vt->images)
        return NULL;
    return (const CfrImage *)cfr_img_get(vt, vt->images, out_count);
}

const CfrImagePlacement *cfr_get_image_placements(CfrTerm *vt, int *out_count)
{
    if (!vt || !vt->images)
        return NULL;
    return cfr_img_get_placements(vt, vt->images, out_count);
}

const CfrImagePlacement *cfr_get_image_placements_for(CfrTerm *vt,
                                                      uint64_t image_id,
                                                      int *out_count)
{
    if (!vt || !vt->images)
        return NULL;
    return cfr_img_get_placements_for(vt, vt->images, image_id, out_count);
}
