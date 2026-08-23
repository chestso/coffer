/*
 * coffer — generic grid-anchored image store.
 *
 * Extracted from sixel.c (sx_buf_alloc, sx_buf_release, sx_rec_release,
 * sx_evict_to_budget, sx_find_at, sx_advance_cursor, cfr_img_note_scroll,
 * cfr_img_clear_display_rows, cfr_img_clear_all, cfr_img_get).
 *
 * All four image protocols (sixel, lottie, iTerm2, kitty) share this store
 * for RGBA pixel data anchored to absolute grid lines. The source field
 * on each record distinguishes which protocol produced it.
 */

#include "coffer_internal.h"
#include "image_store.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Store lifecycle                                                    */
/* ------------------------------------------------------------------ */

CfrImgStore *cfr_img_store_new(void *vt)
{
    CfrTerm *cvt = vt;
    CfrImgStore *st = cfr_alloc(cvt, sizeof(*st));
    if (!st)
        return NULL;
    memset(st, 0, sizeof(*st));
    st->next_id = 1;
    return st;
}

/* Lazy-initialize the terminal's image store. Shared by the kitty
 * graphics, iTerm2 inline image, sixel and Lottie handlers. */
CfrImgStore *cfr_img_get_store(void *vt)
{
    CfrTerm *cvt = vt;
    if (cvt->images)
        return cvt->images;
    CfrImgStore *st = cfr_img_store_new(cvt);
    if (st)
        cvt->images = st;
    return st;
}

void cfr_img_store_free(void *vt, CfrImgStore *st)
{
    CfrTerm *cvt = vt;
    if (!st)
        return;
    for (int i = 0; i < st->img_count; i++)
        cfr_dealloc(cvt, st->imgs[i].rgba);
    for (int i = 0; i < st->spare_count; i++)
        cfr_dealloc(cvt, st->spares[i].ptr);
    cfr_dealloc(cvt, st->imgs);
    cfr_dealloc(cvt, st->places);
    cfr_dealloc(cvt, st->img_scratch);
    cfr_dealloc(cvt, st->place_scratch);
    cfr_dealloc(cvt, st);
}

/* ------------------------------------------------------------------ */
/* Buffer pool (tier 2)                                               */
/* ------------------------------------------------------------------ */

uint8_t *img_buf_alloc(void *vt, CfrImgStore *st, size_t need, size_t *out_cap)
{
    CfrTerm *cvt = vt;
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
        if (out_cap)
            *out_cap = cap;
        return p;
    }
    uint8_t *p = cfr_alloc(cvt, need);
    if (out_cap)
        *out_cap = p ? need : 0;
    return p;
}

void img_buf_release(void *vt, CfrImgStore *st, uint8_t *ptr, size_t cap)
{
    CfrTerm *cvt = vt;
    if (!ptr)
        return;
    if (st->spare_count < IMG_SPARE_MAX &&
        st->retain_bytes + cap <= IMG_RETAIN_MAX) {
        st->spares[st->spare_count].ptr = ptr;
        st->spares[st->spare_count].cap = cap;
        st->spare_count++;
        st->retain_bytes += cap;
        return;
    }
    cfr_dealloc(cvt, ptr);
}

/* ------------------------------------------------------------------ */
/* Image lifecycle (tier 1)                                          */
/* ------------------------------------------------------------------ */

/* Release an image record: return its buffer to the pool, swap-remove. */
static void img_rec_release(void *vt, CfrImgStore *st, int idx)
{
    CfrImg *r = &st->imgs[idx];
    uint64_t id = r->id;
    st->live_bytes -= r->cap;
    img_buf_release(vt, st, r->rgba, r->cap);
    st->imgs[idx] = st->imgs[--st->img_count];

    /* Remove any placements that referenced the released image. */
    for (int i = 0; i < st->place_count;) {
        if (st->places[i].image_id == id)
            st->places[i] = st->places[--st->place_count];
        else
            ++i;
    }
}

void img_evict_to_budget(void *vt, CfrImgStore *st, size_t incoming)
{
    while (st->img_count > 0 && st->live_bytes + incoming > IMG_LIVE_MAX) {
        int m = 0;
        for (int i = 1; i < st->img_count; ++i)
            if (st->imgs[i].abs_line < st->imgs[m].abs_line)
                m = i;
        img_rec_release(vt, st, m);
    }
}

int cfr_img_find_at(CfrImgStore *st, long abs_line, int col, uint8_t layer)
{
    for (int i = 0; i < st->img_count; ++i)
        if (st->imgs[i].abs_line == abs_line && st->imgs[i].col == col &&
            st->imgs[i].layer == layer)
            return i;
    return -1;
}

int cfr_img_find_by_id(CfrImgStore *st, uint64_t id)
{
    for (int i = 0; i < st->img_count; ++i)
        if (st->imgs[i].id == id)
            return i;
    return -1;
}

void cfr_img_remove(void *vt, CfrImgStore *st, int idx)
{
    if (idx < 0 || idx >= st->img_count)
        return;
    img_rec_release(vt, st, idx);
}

/* Remove a placement record (swap-remove). */
static void place_rec_release(CfrImgStore *st, int idx)
{
    st->places[idx] = st->places[--st->place_count];
}

int cfr_img_add_placement(void *vt, CfrImgStore *st, uint64_t image_id,
                          long abs_line, int col, int rows, int cols,
                          uint8_t layer, uint8_t opacity, int z_index)
{
    if (!st)
        return -1;

    /* The owning image must exist. */
    if (cfr_img_find_by_id(st, image_id) < 0)
        return -1;

    /* Dedup on image_id + abs_line + col. */
    for (int i = 0; i < st->place_count; ++i) {
        if (st->places[i].image_id == image_id &&
            st->places[i].abs_line == abs_line &&
            st->places[i].col == col) {
            CfrPlacement *pl = &st->places[i];
            pl->rows = rows;
            pl->cols = cols;
            pl->layer = layer;
            pl->opacity_x256 = opacity;
            pl->z_index = z_index;
            return i;
        }
    }

    if (st->place_count >= st->place_cap) {
        int ncap = st->place_cap ? st->place_cap * 2 : 8;
        CfrPlacement *np =
            cfr_realloc(vt, st->places, (size_t)ncap * sizeof(CfrPlacement));
        if (!np)
            return -1;
        st->places = np;
        st->place_cap = ncap;
    }

    CfrPlacement *pl = &st->places[st->place_count++];
    memset(pl, 0, sizeof(*pl));
    pl->id = st->next_place_id++;
    pl->image_id = image_id;
    pl->abs_line = abs_line;
    pl->col = col;
    pl->rows = rows;
    pl->cols = cols;
    pl->layer = layer;
    pl->opacity_x256 = opacity;
    pl->z_index = z_index;
    return st->place_count - 1;
}

/* Move the cursor below a placed image and scroll the grid as needed. */
static void img_advance_cursor(CfrTerm *vt, int rows_tall, int cols_wide)
{
    if (vt->modes[CFR_MODE_SIXEL_SCROLLING])
        return;
    if (vt->modes[CFR_MODE_SIXEL_CURSOR_RIGHT]) {
        int c = vt->cursor.col + cols_wide;
        if (c > vt->cols - 1)
            c = vt->cols - 1;
        vt->cursor.col = c;
        vt->cursor.pending_wrap = false;
        return;
    }
    int col = vt->cursor.col;
    for (int i = 0; i < rows_tall; ++i) {
        if (vt->cursor.row == vt->scroll_bottom)
            cfr_scroll_up(vt, 1);
        else if (vt->cursor.row < vt->rows - 1)
            vt->cursor.row++;
    }
    vt->cursor.col = col;
    vt->cursor.pending_wrap = false;
}

/* Damage the display rows covered by a newly placed/updated image. */
static void img_damage(CfrTerm *vt, long abs_line, int rows_tall)
{
    int disp_row = (int)(abs_line - vt->sixel_abs_top);
    int a = disp_row < 0 ? 0 : disp_row;
    int b = disp_row + rows_tall - 1;
    if (b >= vt->rows)
        b = vt->rows - 1;
    for (int r = a; r <= b; ++r)
        cfr_damage_row(vt, r);
}

int cfr_img_add(void *vt, CfrImgStore *st,
                const uint8_t *rgba, int w, int h,
                int disp_w, int disp_h,
                uint8_t layer, uint8_t source)
{
    CfrTerm *cvt = vt;

    if (!rgba || w <= 0 || h <= 0 || w > IMG_MAX_DIM || h > IMG_MAX_DIM)
        return -1;

    /* Display dimensions default to pixel dimensions (sixel case). */
    if (disp_w <= 0)
        disp_w = w;
    if (disp_h <= 0)
        disp_h = h;
    if (disp_w > IMG_MAX_DIM)
        disp_w = IMG_MAX_DIM;
    if (disp_h > IMG_MAX_DIM)
        disp_h = IMG_MAX_DIM;

    size_t need = (size_t)w * (size_t)h * 4u;
    if (need == 0 || need > IMG_LIVE_MAX)
        return -1;

    /* Compute grid occupancy from logical display dimensions.
     * cell_w_px/cell_h_px are physical (include content_scale), so
     * we scale disp to physical before dividing. */
    int cell_h = cvt->cell_h_px;
    int cell_w = cvt->cell_w_px;
    float scale = cvt->content_scale > 0.0f ? cvt->content_scale : 1.0f;
    int scaled_w = (int)(disp_w * scale + 0.5f);
    int scaled_h = (int)(disp_h * scale + 0.5f);
    int rows_tall = (scaled_h + cell_h - 1) / cell_h;
    int cols_wide = (scaled_w + cell_w - 1) / cell_w;
    if (rows_tall < 1)
        rows_tall = 1;
    if (cols_wide < 1)
        cols_wide = 1;

    /* Capture anchor */
    long abs_line = cvt->sixel_abs_top + cvt->cursor.row;
    int col = cvt->cursor.col;

    /* Animation / in-place replacement */
    int existing = cfr_img_find_at(st, abs_line, col, layer);
    if (existing >= 0) {
        CfrImg *r = &st->imgs[existing];
        if (r->cap < need) {
            st->live_bytes -= r->cap;
            img_buf_release(vt, st, r->rgba, r->cap);
            size_t cap = 0;
            uint8_t *buf = img_buf_alloc(vt, st, need, &cap);
            if (!buf) {
                st->imgs[existing] = st->imgs[--st->img_count];
                return -1;
            }
            r->rgba = buf;
            r->cap = cap;
            st->live_bytes += cap;
        }
        memcpy(r->rgba, rgba, need);
        r->version++;
        r->w = disp_w;
        r->h = disp_h;
        r->buf_w = w;
        r->buf_h = h;
        r->rows_tall = rows_tall;
        r->cols_wide = cols_wide;
        img_damage(cvt, abs_line, rows_tall);
        img_advance_cursor(cvt, rows_tall, cols_wide);
        return existing;
    }

    /* New record */
    if (st->img_count >= IMG_MAX_IMAGES)
        img_rec_release(vt, st, 0);
    img_evict_to_budget(vt, st, need);

    if (st->img_count >= st->img_cap) {
        int ncap = st->img_cap ? st->img_cap * 2 : 8;
        if (ncap > IMG_MAX_IMAGES)
            ncap = IMG_MAX_IMAGES;
        CfrImg *nr = cfr_realloc(vt, st->imgs, (size_t)ncap * sizeof(CfrImg));
        if (!nr)
            return -1;
        st->imgs = nr;
        st->img_cap = ncap;
    }

    size_t cap = 0;
    uint8_t *buf = img_buf_alloc(vt, st, need, &cap);
    if (!buf)
        return -1;

    CfrImg *r = &st->imgs[st->img_count++];
    r->id = st->next_id++;
    r->version = 1;
    r->layer = layer;
    r->source = source;
    r->abs_line = abs_line;
    r->col = col;
    r->w = disp_w;
    r->h = disp_h;
    r->buf_w = w;
    r->buf_h = h;
    r->rows_tall = rows_tall;
    r->cols_wide = cols_wide;
    r->rgba = buf;
    r->cap = cap;
    st->live_bytes += cap;

    memcpy(buf, rgba, need);

    img_damage(cvt, abs_line, rows_tall);
    img_advance_cursor(cvt, rows_tall, cols_wide);
    return st->img_count - 1;
}

int cfr_img_add_named(void *vt, CfrImgStore *st,
                      uint64_t id, const uint8_t *rgba, int w, int h,
                      uint8_t layer, uint8_t source)
{
    if (!rgba || w <= 0 || h <= 0 || w > IMG_MAX_DIM || h > IMG_MAX_DIM)
        return -1;

    size_t need = (size_t)w * (size_t)h * 4u;
    if (need == 0 || need > IMG_LIVE_MAX)
        return -1;

    int existing = cfr_img_find_by_id(st, id);
    if (existing >= 0) {
        CfrImg *r = &st->imgs[existing];
        if (r->cap < need) {
            st->live_bytes -= r->cap;
            img_buf_release(vt, st, r->rgba, r->cap);
            size_t cap = 0;
            uint8_t *buf = img_buf_alloc(vt, st, need, &cap);
            if (!buf) {
                st->imgs[existing] = st->imgs[--st->img_count];
                return -1;
            }
            r->rgba = buf;
            r->cap = cap;
            st->live_bytes += cap;
        }
        memcpy(r->rgba, rgba, need);
        r->w = w;
        r->h = h;
        r->layer = layer;
        r->source = source;
        return existing;
    }

    if (st->img_count >= IMG_MAX_IMAGES)
        img_rec_release(vt, st, 0);
    img_evict_to_budget(vt, st, need);

    if (st->img_count >= st->img_cap) {
        int ncap = st->img_cap ? st->img_cap * 2 : 8;
        if (ncap > IMG_MAX_IMAGES)
            ncap = IMG_MAX_IMAGES;
        CfrImg *nr = cfr_realloc(vt, st->imgs, (size_t)ncap * sizeof(CfrImg));
        if (!nr)
            return -1;
        st->imgs = nr;
        st->img_cap = ncap;
    }

    size_t cap = 0;
    uint8_t *buf = img_buf_alloc(vt, st, need, &cap);
    if (!buf)
        return -1;

    CfrImg *r = &st->imgs[st->img_count++];
    r->id = id;
    r->version = 1;
    r->layer = layer;
    r->source = source;
    r->abs_line = 0;
    r->col = 0;
    r->w = w;
    r->h = h;
    r->buf_w = w;
    r->buf_h = h;
    r->rows_tall = 1;
    r->cols_wide = 1;
    r->rgba = buf;
    r->cap = cap;
    st->live_bytes += cap;
    memcpy(buf, rgba, need);
    return st->img_count - 1;
}

int cfr_img_blank_named(void *vt, CfrImgStore *st, uint64_t id,
                        int w, int h, uint8_t layer, uint8_t source)
{
    if (w <= 0 || h <= 0 || w > IMG_MAX_DIM || h > IMG_MAX_DIM)
        return -1;

    size_t need = (size_t)w * (size_t)h * 4u;
    if (need == 0 || need > IMG_LIVE_MAX)
        return -1;

    int existing = cfr_img_find_by_id(st, id);
    if (existing >= 0) {
        CfrImg *r = &st->imgs[existing];
        if (r->cap < need) {
            st->live_bytes -= r->cap;
            img_buf_release(vt, st, r->rgba, r->cap);
            size_t cap = 0;
            uint8_t *buf = img_buf_alloc(vt, st, need, &cap);
            if (!buf) {
                st->imgs[existing] = st->imgs[--st->img_count];
                return -1;
            }
            r->rgba = buf;
            r->cap = cap;
            st->live_bytes += cap;
        }
        r->w = w;
        r->h = h;
        r->layer = layer;
        r->source = source;
        return existing;
    }

    if (st->img_count >= IMG_MAX_IMAGES)
        img_rec_release(vt, st, 0);
    img_evict_to_budget(vt, st, need);

    if (st->img_count >= st->img_cap) {
        int ncap = st->img_cap ? st->img_cap * 2 : 8;
        if (ncap > IMG_MAX_IMAGES)
            ncap = IMG_MAX_IMAGES;
        CfrImg *nr = cfr_realloc(vt, st->imgs, (size_t)ncap * sizeof(CfrImg));
        if (!nr)
            return -1;
        st->imgs = nr;
        st->img_cap = ncap;
    }

    size_t cap = 0;
    uint8_t *buf = img_buf_alloc(vt, st, need, &cap);
    if (!buf)
        return -1;

    CfrImg *r = &st->imgs[st->img_count++];
    r->id = id;
    r->version = 1;
    r->layer = layer;
    r->source = source;
    r->abs_line = 0;
    r->col = 0;
    r->w = w;
    r->h = h;
    r->buf_w = w;
    r->buf_h = h;
    r->rows_tall = 1;
    r->cols_wide = 1;
    r->rgba = buf;
    r->cap = cap;
    st->live_bytes += cap;
    return st->img_count - 1;
}

void cfr_img_mark_dirty(CfrImgStore *st, int idx)
{
    if (idx < 0 || idx >= st->img_count)
        return;
    st->imgs[idx].version++;
}

void cfr_img_replace(void *vt, CfrImgStore *st, int idx,
                     const uint8_t *rgba, int w, int h)
{
    if (idx < 0 || idx >= st->img_count)
        return;
    CfrImg *r = &st->imgs[idx];
    size_t need = (size_t)w * (size_t)h * 4u;

    if ((size_t)r->cap < need) {
        st->live_bytes -= r->cap;
        img_buf_release(vt, st, r->rgba, r->cap);
        size_t cap = 0;
        uint8_t *buf = img_buf_alloc(vt, st, need, &cap);
        if (!buf)
            return;
        r->rgba = buf;
        r->cap = cap;
        st->live_bytes += cap;
    }

    memcpy(r->rgba, rgba, need);
    r->w = w;
    r->h = h;
    r->buf_w = w;
    r->buf_h = h;
    r->version++;

    int cell_h = ((CfrTerm *)vt)->cell_h_px;
    int cell_w = ((CfrTerm *)vt)->cell_w_px;
    float scale = ((CfrTerm *)vt)->content_scale > 0.0f ? ((CfrTerm *)vt)->content_scale : 1.0f;
    int scaled_h = (int)(h * scale);
    int scaled_w = (int)(w * scale);
    r->rows_tall = (scaled_h + cell_h - 1) / cell_h;
    r->cols_wide = (scaled_w + cell_w - 1) / cell_w;
    if (r->rows_tall < 1)
        r->rows_tall = 1;
    if (r->cols_wide < 1)
        r->cols_wide = 1;

    img_damage(vt, r->abs_line, r->rows_tall);
}

/* ------------------------------------------------------------------ */
/* Grid maintenance                                                   */
/* ------------------------------------------------------------------ */

/* True if the image with the given id has at least one placement
 * (1:N model, kitty/lottie). 1:1 (sixel/iTerm2) images have none. */
static bool img_has_placements(const CfrImgStore *st, uint64_t image_id)
{
    for (int i = 0; i < st->place_count; ++i)
        if (st->places[i].image_id == image_id)
            return true;
    return false;
}

void cfr_img_note_scroll(void *vt, CfrImgStore *st, int lines)
{
    CfrTerm *cvt = vt;
    if (!st || st->img_count == 0)
        return;
    (void)lines;
    int cap = cvt->sb_capacity;

    for (int i = 0; i < st->img_count;) {
        uint64_t id = st->imgs[i].id;

        if (img_has_placements(st, id)) {
            /* 1:N — cull placements whose bottom scrolled off. */
            for (int j = 0; j < st->place_count;) {
                if (st->places[j].image_id != id) {
                    ++j;
                    continue;
                }
                long depth = cvt->sixel_abs_top - st->places[j].abs_line;
                long bottom_depth = depth - st->places[j].rows + 1;
                if (bottom_depth > cap)
                    place_rec_release(st, j);
                else
                    ++j;
            }
            /* Remove the image if no placements remain. */
            if (!img_has_placements(st, id))
                img_rec_release(vt, st, i);
            else
                ++i;
            continue;
        }

        /* 1:1 — cull based on the record's own anchor. */
        long depth = cvt->sixel_abs_top - st->imgs[i].abs_line;
        long bottom_depth = depth - st->imgs[i].rows_tall + 1;
        if (bottom_depth > cap)
            img_rec_release(vt, st, i);
        else
            ++i;
    }
}

void cfr_img_clear_display_rows(void *vt, CfrImgStore *st, int top, int bot)
{
    CfrTerm *cvt = vt;
    if (!st || st->img_count == 0)
        return;

    for (int i = 0; i < st->img_count;) {
        uint64_t id = st->imgs[i].id;

        if (img_has_placements(st, id)) {
            /* 1:N — remove foreground placements overlapping [top,bot]. */
            bool removed = false;
            for (int j = 0; j < st->place_count;) {
                if (st->places[j].image_id != id ||
                    st->places[j].layer != 0) {
                    ++j;
                    continue;
                }
                int ptop = (int)(st->places[j].abs_line - cvt->sixel_abs_top);
                int pbot = ptop + st->places[j].rows - 1;
                if (ptop <= bot && pbot >= top) {
                    place_rec_release(st, j);
                    removed = true;
                } else {
                    ++j;
                }
            }
            /* Remove the image if no placements remain. */
            if (removed && !img_has_placements(st, id))
                img_rec_release(vt, st, i);
            else
                ++i;
            continue;
        }

        /* 1:1 — clear based on the record's own anchor. */
        if (st->imgs[i].layer != 0) {
            ++i;
            continue;
        }
        int rtop = (int)(st->imgs[i].abs_line - cvt->sixel_abs_top);
        int rbot = rtop + st->imgs[i].rows_tall - 1;
        if (rtop <= bot && rbot >= top)
            img_rec_release(vt, st, i);
        else
            ++i;
    }
}

void cfr_img_clear_all(void *vt, CfrImgStore *st)
{
    if (!st)
        return;
    while (st->img_count > 0)
        img_rec_release(vt, st, st->img_count - 1);
}

/* ------------------------------------------------------------------ */
/* Public query                                                       */
/* ------------------------------------------------------------------ */

const CfrImage *cfr_img_get(void *vt, CfrImgStore *st, int *out_count)
{
    CfrTerm *cvt = vt;
    if (out_count)
        *out_count = 0;
    if (!st || st->img_count == 0)
        return NULL;

    if (st->img_scratch_cap < st->img_count) {
        int ncap = st->img_scratch_cap ? st->img_scratch_cap * 2 : 8;
        while (ncap < st->img_count)
            ncap *= 2;
        CfrImage *ns = cfr_realloc(cvt, st->img_scratch,
                                   (size_t)ncap * sizeof(CfrImage));
        if (!ns)
            return NULL;
        st->img_scratch = ns;
        st->img_scratch_cap = ncap;
    }

    /* Single logical→physical conversion point for the whole pipeline.
     * The renderer receives physical values and does zero conversions. */
    float cscale = cvt->content_scale > 0.0f ? cvt->content_scale : 1.0f;

    for (int i = 0; i < st->img_count; ++i) {
        CfrImg *r = &st->imgs[i];
        CfrImage *v = &st->img_scratch[i];
        v->id = r->id;
        v->version = r->version;
        v->layer = r->layer;
        v->source = r->source;
        v->row = (int)(r->abs_line - cvt->sixel_abs_top);
        v->col = r->col;
        v->width_px = (int)(r->w * cscale + 0.5f);
        v->height_px = (int)(r->h * cscale + 0.5f);
        v->buf_w = (int)(r->buf_w * cscale + 0.5f);
        v->buf_h = (int)(r->buf_h * cscale + 0.5f);
        v->rgba = r->rgba;
    }
    if (out_count)
        *out_count = st->img_count;
    return st->img_scratch;
}

const CfrImagePlacement *cfr_img_get_placements(void *vt, CfrImgStore *st,
                                                int *out_count)
{
    CfrTerm *cvt = vt;
    if (out_count)
        *out_count = 0;
    if (!st || st->place_count == 0)
        return NULL;

    if (st->place_scratch_cap < st->place_count) {
        int ncap = st->place_scratch_cap ? st->place_scratch_cap * 2 : 8;
        while (ncap < st->place_count)
            ncap *= 2;
        CfrImagePlacement *ns = cfr_realloc(
            cvt, st->place_scratch, (size_t)ncap * sizeof(CfrImagePlacement));
        if (!ns)
            return NULL;
        st->place_scratch = ns;
        st->place_scratch_cap = ncap;
    }

    for (int i = 0; i < st->place_count; ++i) {
        CfrPlacement *r = &st->places[i];
        CfrImagePlacement *v = &st->place_scratch[i];
        v->id = r->id;
        v->image_id = r->image_id;
        v->row = (int)(r->abs_line - cvt->sixel_abs_top);
        v->col = r->col;
        v->rows = r->rows;
        v->cols = r->cols;
        v->layer = r->layer;
        v->opacity_x256 = r->opacity_x256;
        v->z_index = r->z_index;
        v->src_x = r->src_x;
        v->src_y = r->src_y;
        v->src_w = r->src_w;
        v->src_h = r->src_h;
    }
    if (out_count)
        *out_count = st->place_count;
    return st->place_scratch;
}

const CfrImagePlacement *cfr_img_get_placements_for(void *vt, CfrImgStore *st,
                                                    uint64_t image_id,
                                                    int *out_count)
{
    CfrTerm *cvt = vt;
    if (out_count)
        *out_count = 0;
    if (!st || st->place_count == 0)
        return NULL;

    int want = 0;
    for (int i = 0; i < st->place_count; ++i)
        if (st->places[i].image_id == image_id)
            want++;
    if (want == 0)
        return NULL;

    if (st->place_scratch_cap < want) {
        int ncap = st->place_scratch_cap ? st->place_scratch_cap * 2 : 8;
        while (ncap < want)
            ncap *= 2;
        CfrImagePlacement *ns = cfr_realloc(
            cvt, st->place_scratch, (size_t)ncap * sizeof(CfrImagePlacement));
        if (!ns)
            return NULL;
        st->place_scratch = ns;
        st->place_scratch_cap = ncap;
    }

    int w = 0;
    for (int i = 0; i < st->place_count; ++i) {
        CfrPlacement *r = &st->places[i];
        if (r->image_id != image_id)
            continue;
        CfrImagePlacement *v = &st->place_scratch[w++];
        v->id = r->id;
        v->image_id = r->image_id;
        v->row = (int)(r->abs_line - cvt->sixel_abs_top);
        v->col = r->col;
        v->rows = r->rows;
        v->cols = r->cols;
        v->layer = r->layer;
        v->opacity_x256 = r->opacity_x256;
        v->z_index = r->z_index;
        v->src_x = r->src_x;
        v->src_y = r->src_y;
        v->src_w = r->src_w;
        v->src_h = r->src_h;
    }
    if (out_count)
        *out_count = want;
    return st->place_scratch;
}
