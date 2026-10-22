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
    /* The handle counter starts at 1: handle 0 is reserved as "no
     * image" (a placement that names handle 0 refers to nothing). */
    st->next_handle = 1;
    st->next_seq = 1;
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

/* Damage the display rows covered by an image/placement. Defined
 * below (it needs the grid geometry); declared here because the
 * placement path calls it. */
static void img_damage(CfrTerm *vt, long abs_line, int rows_tall);

/* Recompute a 1:N image's anchor from its placements. Defined below
 * (it needs the handle lookup); declared here because the placement
 * release path calls it. */
static void img_reanchor(CfrImgStore *st, uint64_t handle);

/* Release an image record: return its buffer to the pool, swap-remove. */
static void img_rec_release(void *vt, CfrImgStore *st, int idx)
{
    CfrImg *r = &st->imgs[idx];
    uint64_t handle = r->handle;
    st->live_bytes -= r->cap;
    img_buf_release(vt, st, r->rgba, r->cap);
    st->imgs[idx] = st->imgs[--st->img_count];

    /* Remove any placements that referenced the released image. The
     * placement names the image by HANDLE, so a client id and an
     * engine id can never cross wires here. */
    for (int i = 0; i < st->place_count;) {
        if (st->places[i].image_handle == handle)
            st->places[i] = st->places[--st->place_count];
        else
            ++i;
    }
}

/* The eviction's victim: the OLDEST record, by creation order. The
 * order is total over every record (a 1:N image has no anchor of its
 * own to order it by, so an anchor-based order was arbitrary for it —
 * and the "lowest abs_line" comparison ties for every named image,
 * which made the byte-budget eviction take whichever record happened
 * to sit first in the dense array). */
static int img_evict_victim(CfrImgStore *st)
{
    int m = 0;
    for (int i = 1; i < st->img_count; ++i)
        if (st->imgs[i].seq < st->imgs[m].seq)
            m = i;
    return m;
}

void img_evict_to_budget(void *vt, CfrImgStore *st, size_t incoming)
{
    while (st->img_count > 0 && st->live_bytes + incoming > IMG_LIVE_MAX)
        img_rec_release(vt, st, img_evict_victim(st));
}

/* Drop the oldest record when the record cap is reached. The victim is
 * the OLDEST (creation order), not the record at index 0: the dense
 * array is swap-removed, so index 0 holds an arbitrary record (the
 * last one moved into the hole) — releasing it evicted the NEWEST
 * image. */
static void img_evict_for_cap(void *vt, CfrImgStore *st)
{
    if (st->img_count < IMG_MAX_IMAGES)
        return;
    img_rec_release(vt, st, img_evict_victim(st));
}

int cfr_img_find_at(CfrImgStore *st, long abs_line, int col, uint8_t layer)
{
    for (int i = 0; i < st->img_count; ++i)
        if (st->imgs[i].kind == IMG_KIND_ANCHORED &&
            st->imgs[i].abs_line == abs_line && st->imgs[i].col == col &&
            st->imgs[i].layer == layer)
            return i;
    return -1;
}

/* Find an image by its PUBLIC id. The id spaces of the 1:1 and the 1:N
 * protocols overlap by construction (kitty's client-chosen i=1 and the
 * engine's first id are both 1), so this lookup is only meaningful
 * within one protocol family — the callers that need an unambiguous
 * answer use the handle (cfr_img_find_by_handle). */
int cfr_img_find_by_id(CfrImgStore *st, uint64_t id)
{
    for (int i = 0; i < st->img_count; ++i)
        if (st->imgs[i].id == id)
            return i;
    return -1;
}

/* Find an image by its store-internal handle. Unambiguous across
 * protocols. */
int cfr_img_find_by_handle(CfrImgStore *st, uint64_t handle)
{
    for (int i = 0; i < st->img_count; ++i)
        if (st->imgs[i].handle == handle)
            return i;
    return -1;
}

/* Find an image by its PUBLIC id within one protocol family. */
int cfr_img_find_by_source_id(CfrImgStore *st, uint8_t source, uint64_t id)
{
    for (int i = 0; i < st->img_count; ++i)
        if (st->imgs[i].source == source && st->imgs[i].id == id)
            return i;
    return -1;
}

void cfr_img_remove(void *vt, CfrImgStore *st, int idx)
{
    if (idx < 0 || idx >= st->img_count)
        return;
    img_rec_release(vt, st, idx);
}

/* Remove a placement record (swap-remove) and re-anchor the image it
 * belonged to. The placement's rows are damaged: the host must repaint
 * them, or the image stays on the screen until something else dirties
 * those rows (the "blank space" bug's other half). */
static void place_rec_release(void *vt, CfrImgStore *st, int idx)
{
    uint64_t handle = st->places[idx].image_handle;
    long abs_line = st->places[idx].abs_line;
    int rows = st->places[idx].rows;
    st->places[idx] = st->places[--st->place_count];
    img_reanchor(st, handle);
    img_damage(vt, abs_line, rows);
}

/* The anchor of a 1:N image is the anchor of its OLDEST placement: the
 * image record carries no anchor of its own, so the store keeps the
 * minimum over the image's placements. That anchor is what the host's
 * "one image, many placements" view and the damage computation need.
 *
 * The handle (not the public id) is the reference: a client-assigned id
 * and an engine-assigned id can be the same number. */
static void img_reanchor(CfrImgStore *st, uint64_t handle)
{
    int idx = cfr_img_find_by_handle(st, handle);
    if (idx < 0)
        return;
    bool have = false;
    long best_line = 0;
    int best_col = 0;
    for (int i = 0; i < st->place_count; ++i) {
        if (st->places[i].image_handle != handle)
            continue;
        if (!have || st->places[i].abs_line < best_line) {
            have = true;
            best_line = st->places[i].abs_line;
            best_col = st->places[i].col;
        }
    }
    if (have) {
        st->imgs[idx].abs_line = best_line;
        st->imgs[idx].col = best_col;
    } else {
        st->imgs[idx].abs_line = 0;
        st->imgs[idx].col = 0;
    }
}

int cfr_img_add_placement(void *vt, CfrImgStore *st, uint64_t image_handle,
                          long abs_line, int col, int rows, int cols,
                          uint8_t layer, uint8_t opacity, int z_index)
{
    if (!st)
        return -1;

    /* The owning image must exist. The handle is the unambiguous
     * reference: a client's i= id and an engine id can be the same
     * number without either shadowing the other. */
    int img_idx = cfr_img_find_by_handle(st, image_handle);
    if (img_idx < 0)
        return -1;
    uint64_t image_id = st->imgs[img_idx].id;
    uint8_t source = st->imgs[img_idx].source;

    /* Dedup on image_handle + abs_line + col. */
    for (int i = 0; i < st->place_count; ++i) {
        if (st->places[i].image_handle == image_handle &&
            st->places[i].abs_line == abs_line &&
            st->places[i].col == col) {
            CfrPlacement *pl = &st->places[i];
            pl->rows = rows;
            pl->cols = cols;
            pl->layer = layer;
            pl->opacity_x256 = opacity;
            pl->z_index = z_index;
            img_reanchor(st, image_handle);
            img_damage(vt, abs_line, rows);
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
    pl->image_handle = image_handle;
    pl->source = source;
    pl->abs_line = abs_line;
    pl->col = col;
    pl->rows = rows;
    pl->cols = cols;
    pl->layer = layer;
    pl->opacity_x256 = opacity;
    pl->z_index = z_index;
    st->imgs[img_idx].ever_placed = 1;

    /* The image's own record takes the anchor of its oldest placement,
     * so the store can order, cull and damage a 1:N image without
     * walking the placements first. */
    img_reanchor(st, image_handle);

    /* The placement is a change of the screen: the host only repaints
     * the rows coffer reports as damaged, so a placement that reports
     * nothing is a placement the host never draws (the image lands as
     * a blank space). */
    img_damage(vt, abs_line, rows);
    return st->place_count - 1;
}

/* Move the text cursor after a placed image and scroll the grid as
 * needed. `land_on_last` selects WHERE it lands:
 *
 *   - sixel (1): the VT340 spec (ch14, "Sixel Scrolling Mode": "When
 *     sixel mode is exited, the text cursor is set to the current sixel
 *     cursor position") puts the text cursor on the image's LAST row —
 *     the row holding the top of the bottom-most sixel band — so
 *     advance rows_tall - 1. The app then prints its own break to get
 *     below the image (nevermore's one \r\n).
 *   - iTerm2 1337 (0): the cursor goes BELOW the image, so advance
 *     rows_tall.
 *
 * The loop still scrolls when it runs out of screen, so an image
 * anchored at the bottom margin scrolls into view and the cursor ends
 * on the image's last row (or below it, for iTerm2). */
static void img_advance_cursor(CfrTerm *vt, int rows_tall, int cols_wide,
                               int land_on_last)
{
    if (vt->modes[CFR_MODE_SIXEL_SCROLLING])
        return;
    if (vt->modes[CFR_MODE_SIXEL_CURSOR_RIGHT]) {
        int c = vt->cursor.col + cols_wide;
        if (c > vt->cols - 1)
            c = vt->cols - 1;
        vt->cursor.col = c;
        return;
    }
    int rows = land_on_last ? rows_tall - 1 : rows_tall;
    if (rows < 0)
        rows = 0;
    int col = vt->cursor.col;
    for (int i = 0; i < rows; ++i) {
        if (vt->cursor.row == vt->scroll_bottom)
            cfr_scroll_up(vt, 1);
        else if (vt->cursor.row < vt->rows - 1)
            vt->cursor.row++;
    }
    vt->cursor.col = col;
    cfr_cursor_kill_phantom(vt);
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
        r->version = ++st->version_seq;
        r->w = disp_w;
        r->h = disp_h;
        r->buf_w = w;
        r->buf_h = h;
        r->rows_tall = rows_tall;
        r->cols_wide = cols_wide;
        img_damage(cvt, abs_line, rows_tall);
        img_advance_cursor(cvt, rows_tall, cols_wide,
                           source == IMG_SRC_SIXEL);
        return existing;
    }

    /* New record */
    img_evict_for_cap(vt, st);
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
    r->handle = st->next_handle++;
    r->seq = st->next_seq++;
    r->version = ++st->version_seq;
    r->layer = layer;
    r->source = source;
    r->kind = IMG_KIND_ANCHORED;
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
    img_advance_cursor(cvt, rows_tall, cols_wide, source == IMG_SRC_SIXEL);
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

    int existing = cfr_img_find_by_source_id(st, source, id);
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
        r->version = ++st->version_seq;
        return existing;
    }

    img_evict_for_cap(vt, st);
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
    r->handle = st->next_handle++;
    r->seq = st->next_seq++;
    r->version = ++st->version_seq;
    r->layer = layer;
    r->source = source;
    r->kind = IMG_KIND_REFERENCED;
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

    int existing = cfr_img_find_by_source_id(st, source, id);
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

    img_evict_for_cap(vt, st);
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
    r->handle = st->next_handle++;
    r->seq = st->next_seq++;
    r->version = ++st->version_seq;
    r->layer = layer;
    r->source = source;
    r->kind = IMG_KIND_REFERENCED;
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
    st->imgs[idx].version = ++st->version_seq;
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
    r->version = ++st->version_seq;

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

/* True if the image with the given HANDLE has at least one placement.
 * The handle (not the public id) is the reference: a client-assigned
 * id and an engine-assigned id can be the same number. */
static bool img_has_placements(const CfrImgStore *st, uint64_t handle)
{
    for (int i = 0; i < st->place_count; ++i)
        if (st->places[i].image_handle == handle)
            return true;
    return false;
}

/* The lifetime rule of a 1:N image, per source:
 *
 *   - kitty: the image lives until the CLIENT deletes it (a=d), until
 *     the id is re-transmitted, or until the budget evicts it. It does
 *     not die with the screen: kitty's rule is "the other commands to
 *     erase text must have no effect on graphics", and an image whose
 *     placement scrolled away is still a live image (kitty keeps
 *     images until they are deleted).
 *   - Lottie: an animation IS a placement — with its last placement
 *     gone, the animation is over, and its record (the ThorVG
 *     animation, the JSON arena) goes with it.
 *
 * The 1:1 protocols (sixel, iTerm2) are not 1:N; their rule is the
 * screen's own (see cfr_img_clear_display_rows). */
static bool img_dies_with_placements(uint8_t source)
{
    return source == IMG_SRC_LOTTIE;
}

/* Does an erase of the screen touch this 1:N image?
 *
 *   - kitty: no. "The other commands to erase text must have no effect
 *     on graphics." The one screen-wide clear kitty sanctions is ED 2
 *     ("the clear screen escape code should also clear all images"),
 *     and that one clears the VISIBLE images — an image that was
 *     transmitted but never placed is not on the screen, so it
 *     survives.
 *   - Lottie: yes — an erase removes the foreground placements in the
 *     erased rows (the Lottie spec's clear behaviour). */
static bool img_erase_affects(uint8_t source, bool full)
{
    if (source == IMG_SRC_KITTY)
        return full;
    return true;
}

void cfr_img_note_scroll(void *vt, CfrImgStore *st, int lines)
{
    CfrTerm *cvt = vt;
    if (!st || st->img_count == 0)
        return;
    (void)lines;
    int cap = cvt->sb_capacity;

    for (int i = 0; i < st->img_count;) {
        uint64_t handle = st->imgs[i].handle;
        uint8_t source = st->imgs[i].source;

        if (st->imgs[i].kind == IMG_KIND_REFERENCED) {
            /* 1:N (kitty, Lottie) — the anchor lives in the placement,
             * so the placement is what scrolls. A placement that fell
             * out of the scrollback is dropped; the IMAGE is not (the
             * source's rule decides: a kitty image lives until the
             * client deletes it, a Lottie animation dies with its
             * last placement). */
            for (int j = 0; j < st->place_count;) {
                if (st->places[j].image_handle != handle) {
                    ++j;
                    continue;
                }
                long depth = cvt->sixel_abs_top - st->places[j].abs_line;
                long bottom_depth = depth - st->places[j].rows + 1;
                if (bottom_depth > cap)
                    place_rec_release(vt, st, j);
                else
                    ++j;
            }
            if (img_dies_with_placements(source) && st->imgs[i].ever_placed &&
                !img_has_placements(st, handle))
                img_rec_release(vt, st, i);
            else
                ++i;
            continue;
        }

        /* 1:1 (sixel, iTerm2) — the record carries the anchor. */
        long depth = cvt->sixel_abs_top - st->imgs[i].abs_line;
        long bottom_depth = depth - st->imgs[i].rows_tall + 1;
        if (bottom_depth > cap)
            img_rec_release(vt, st, i);
        else
            ++i;
    }
}

/* Remove the placements of ONE image that an erase touches, and the
 * image itself when its source's rule says it dies with them.
 *
 * The rule per source:
 *   - kitty: ED 2 (the screen-wide clear) removes the placements that
 *     are ON the screen; the off-screen ones (a placement that
 *     scrolled into the scrollback) are not visible, so they stay.
 *   - Lottie: the erased rows take their foreground placements with
 *     them (the Lottie spec's clear behaviour).
 *
 * `full` marks the screen-wide clear (ED 2).
 *
 * Returns true when the image itself was released (the dense array
 * swap-removes, so the slot now holds another record — the caller must
 * look at it again). */
static bool img_clear_placements_for(void *vt, CfrImgStore *st, int i,
                                     int top, int bot, bool full)
{
    CfrTerm *cvt = vt;
    uint64_t handle = st->imgs[i].handle;
    uint8_t source = st->imgs[i].source;

    if (source == IMG_SRC_KITTY) {
        /* kitty: ED 2 ("the clear screen escape code should also clear
         * all images") clears the images that are ON the screen. An
         * image with no on-screen placement is not visible — it was
         * transmitted but never placed, or its placement scrolled into
         * the scrollback — so it survives. */
        if (!full)
            return false;
        bool on_screen = false;
        for (int j = 0; j < st->place_count; ++j) {
            if (st->places[j].image_handle != handle)
                continue;
            int ptop = (int)(st->places[j].abs_line - cvt->sixel_abs_top);
            if (ptop >= 0 && ptop < cvt->rows)
                on_screen = true;
        }
        if (!on_screen)
            return false;
        img_rec_release(vt, st, i);
        return true;
    }

    bool removed = false;
    for (int j = 0; j < st->place_count;) {
        if (st->places[j].image_handle != handle) {
            ++j;
            continue;
        }
        if (st->places[j].layer != 0) {
            /* Background placements survive a text erase. */
            ++j;
            continue;
        }
        int ptop = (int)(st->places[j].abs_line - cvt->sixel_abs_top);
        int pbot = ptop + st->places[j].rows - 1;
        if (ptop <= bot && pbot >= top) {
            place_rec_release(vt, st, j);
            removed = true;
        } else {
            ++j;
        }
    }
    if (removed && img_dies_with_placements(source) &&
        !img_has_placements(st, handle)) {
        img_rec_release(vt, st, i);
        return true;
    }
    return false;
}

void cfr_img_clear_display_rows(void *vt, CfrImgStore *st, int top, int bot,
                                bool full)
{
    CfrTerm *cvt = vt;
    if (!st || st->img_count == 0)
        return;

    for (int i = 0; i < st->img_count;) {
        uint8_t source = st->imgs[i].source;

        if (st->imgs[i].kind == IMG_KIND_ANCHORED) {
            /* 1:1 (sixel, iTerm2) — the image is part of the screen:
             * an erase of the rows it covers removes it. */
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
            continue;
        }

        /* 1:N (kitty, Lottie). */
        if (!img_erase_affects(source, full)) {
            ++i;
            continue;
        }
        if (img_clear_placements_for(vt, st, i, top, bot, full))
            continue; /* the record was released — the slot moved */
        ++i;
    }
}

/* Remove the foreground placements of ONE protocol in the display row
 * range [top, bot], and the images that die with them. The Lottie
 * handler uses it: a Lottie animation's clear behaviour is its own
 * (the Lottie spec), so it must not be decided by the kitty rules. */
void cfr_img_clear_source_rows(void *vt, CfrImgStore *st, uint8_t source,
                               int top, int bot)
{
    if (!st || st->img_count == 0)
        return;

    for (int i = 0; i < st->img_count;) {
        if (st->imgs[i].source != source ||
            st->imgs[i].kind != IMG_KIND_REFERENCED) {
            ++i;
            continue;
        }
        if (img_clear_placements_for(vt, st, i, top, bot, false))
            continue; /* the record was released — the slot moved */
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

/* Clear the images that die with the SCREEN: the anchored (1:1) ones,
 * whose anchors are the grid's own rows. A resize re-anchors the grid
 * and an altscreen switch swaps it, so those images cannot be followed
 * — but the client-owned 1:N images (kitty) live on: they are anchored
 * by their own placements, and the client deletes them when it wants
 * them gone. */
void cfr_img_clear_anchored(void *vt, CfrImgStore *st)
{
    if (!st)
        return;
    for (int i = 0; i < st->img_count;) {
        if (st->imgs[i].kind == IMG_KIND_ANCHORED)
            img_rec_release(vt, st, i);
        else
            ++i;
    }
}

/* Clear every image of ONE protocol (kitty's "delete all images"
 * deletes the kitty images — not the sixels and not the Lottie
 * animations, which live in their own id spaces). */
void cfr_img_clear_source(void *vt, CfrImgStore *st, uint8_t source)
{
    if (!st)
        return;
    for (int i = 0; i < st->img_count;) {
        if (st->imgs[i].source == source)
            img_rec_release(vt, st, i);
        else
            ++i;
    }
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
     * The renderer receives physical values and does zero conversions.
     *
     * The conversion applies to the DISPLAY dimensions only. The
     * buffer dimensions (buf_w/buf_h) are the REAL dimensions of the
     * pixel buffer the host is about to read: the buffer holds the
     * decoded (native) pixels, so scaling them told the host to read
     * past the end of the allocation on a scaled display. */
    float cscale = cvt->content_scale > 0.0f ? cvt->content_scale : 1.0f;

    for (int i = 0; i < st->img_count; ++i) {
        CfrImg *r = &st->imgs[i];
        CfrImage *v = &st->img_scratch[i];
        v->id = r->id;
        v->handle = r->handle;
        v->version = r->version;
        v->layer = r->layer;
        v->source = r->source;
        v->row = (int)(r->abs_line - cvt->sixel_abs_top);
        v->col = r->col;
        v->width_px = (int)(r->w * cscale + 0.5f);
        v->height_px = (int)(r->h * cscale + 0.5f);
        v->buf_w = r->buf_w;
        v->buf_h = r->buf_h;
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
        v->image_handle = r->image_handle;
        v->source = r->source;
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
                                                    uint8_t source,
                                                    uint64_t image_id,
                                                    int *out_count)
{
    CfrTerm *cvt = vt;
    if (out_count)
        *out_count = 0;
    if (!st || st->place_count == 0)
        return NULL;

    /* The query names the image by its PUBLIC id, and the public ids
     * of the 1:N protocols are chosen by their clients — kitty's i=1
     * and a Lottie id of 1 are the same number in different id
     * spaces. The answer is therefore scoped to ONE protocol (the
     * source), or to all of them (IMG_SRC_ANY). */
    int want = 0;
    for (int i = 0; i < st->place_count; ++i) {
        if (st->places[i].image_id != image_id)
            continue;
        if (source != IMG_SRC_ANY && st->places[i].source != source)
            continue;
        want++;
    }
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
        if (source != IMG_SRC_ANY && r->source != source)
            continue;
        CfrImagePlacement *v = &st->place_scratch[w++];
        v->id = r->id;
        v->image_id = r->image_id;
        v->image_handle = r->image_handle;
        v->source = r->source;
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
