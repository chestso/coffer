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
/* Per-source policy                                                  */
/* ------------------------------------------------------------------ */

/* The lifetime and erase rules of one protocol family. The store's
 * generic paths (record kind, scroll cull, erase) branch only on this
 * table; a new protocol is a new row, not a new if-chain. */
typedef struct
{
    uint8_t kind;               /* IMG_KIND_* every record of the source gets */
    bool dies_with_placements;  /* 1:N: the record dies with its last placement
                                 * (a Lottie animation IS its placements) */
    bool rows_erase_placements; /* 1:N: a text erase of the display rows removes
                                 * the foreground placements in range (Lottie) */
    bool screen_clear_removes;  /* 1:N: ED 2 removes the image when any placement
                                 * is visible on screen (kitty's rule) */
} ImgSourcePolicy;

static const ImgSourcePolicy *img_policy(uint8_t source)
{
    static const ImgSourcePolicy policies[] = {
        [IMG_SRC_SIXEL] = { IMG_KIND_ANCHORED, false, false, false },
        [IMG_SRC_LOTTIE] = { IMG_KIND_REFERENCED, true, true, false },
        [IMG_SRC_ITERM] = { IMG_KIND_ANCHORED, false, false, false },
        [IMG_SRC_KITTY] = { IMG_KIND_REFERENCED, false, false, true },
    };
    return source < IMG_SRC_ANY ? &policies[source] : &policies[IMG_SRC_SIXEL];
}

/* ------------------------------------------------------------------ */
/* Store lifecycle                                                    */
/* ------------------------------------------------------------------ */

/* Per-image animation state release, defined below (after the buffer
 * pool they return bytes to); declared here because the store teardown
 * calls them. */
static void img_frames_release(void *vt, CfrImgStore *st, CfrImg *r);
static void img_root_release(void *vt, CfrImgStore *st, CfrImg *r);

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
    /* Every image's animation state dies with the store: the frames
     * array and the frozen root copy are per-image allocations outside
     * the buffer pool, so they must be released here, not just the
     * display buffer. */
    for (int i = 0; i < st->img_count; i++) {
        img_frames_release(cvt, st, &st->imgs[i]);
        img_root_release(cvt, st, &st->imgs[i]);
        cfr_dealloc(cvt, st->imgs[i].rgba);
    }
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

/* Damage the display rows covered by an image/placement ON the given
 * screen. Defined below (it needs the grid geometry); declared here
 * because the placement path calls it. */
static void img_damage(CfrTerm *vt, uint8_t screen, long abs_line,
                       int rows_tall);

/* Recompute a 1:N image's anchor from its placements. Defined below
 * (it needs the handle lookup); declared here because the placement
 * release path calls it. */
static void img_reanchor(CfrImgStore *st, uint64_t handle);

/* Drop an image's extra frames: return their bytes to the budget and
 * their buffers to the pool, and reset the playback clock. The root
 * frame's pixels stay — they are the image's data. */
static void img_frames_release(void *vt, CfrImgStore *st, CfrImg *r)
{
    st->live_bytes -= r->frame_bytes;
    for (int i = 0; i < r->frame_count; i++)
        img_buf_release(vt, st, r->frames[i].rgba, r->frames[i].cap);
    cfr_dealloc(vt, r->frames);
    r->frames = NULL;
    r->frame_count = 0;
    r->frame_cap = 0;
    r->frame_bytes = 0;
    r->current_frame = 1;
    r->anim_mode = 0;
    r->loops_left = 0;
    r->next_advance_us = 0;
    r->root_gap_ms = 0;
    r->root_has_gap = false;
}

/* Drop the root frame's kept copy. Only img_rec_release and the
 * re-store path call this; img_frames_release keeps it. */
static void img_root_release(void *vt, CfrImgStore *st, CfrImg *r)
{
    if (!r->root_rgba)
        return;
    st->live_bytes -= r->root_cap;
    img_buf_release(vt, st, r->root_rgba, r->root_cap);
    r->root_rgba = NULL;
    r->root_cap = 0;
}

/* Release an image record: return its buffer to the pool, swap-remove. */
static void img_rec_release(void *vt, CfrImgStore *st, int idx)
{
    CfrImg *r = &st->imgs[idx];
    uint64_t handle = r->handle;
    long abs_line = r->abs_line;
    int rows_tall = r->rows_tall;
    uint8_t kind = r->kind;
    uint8_t screen = r->screen;
    st->live_bytes -= r->cap;
    img_frames_release(vt, st, r);
    img_root_release(vt, st, r);
    img_buf_release(vt, st, r->rgba, r->cap);
    st->imgs[idx] = st->imgs[--st->img_count];

    /* Remove any placements that referenced the released image. The
     * placement names the image by HANDLE, so a client id and an
     * engine id can never cross wires here. Each placement's rows
     * change on the screen — the host must repaint them. */
    for (int i = 0; i < st->place_count;) {
        if (st->places[i].image_handle == handle) {
            img_damage(vt, st->places[i].screen, st->places[i].abs_line,
                       st->places[i].rows);
            st->places[i] = st->places[--st->place_count];
        } else
            ++i;
    }

    /* An anchored image IS its placement: the rows it covered change
     * on the screen when it goes. */
    if (kind == IMG_KIND_ANCHORED)
        img_damage(vt, screen, abs_line, rows_tall);
}

/* Is the image on the ACTIVE screen, inside the visible rows? Used by
 * the byte-budget eviction: a visible image is the last thing to throw
 * away, because the client cannot know its id was reclaimed. */
static bool img_is_visible(CfrTerm *vt, const CfrImgStore *st, const CfrImg *r)
{
    if (r->screen != (uint8_t)vt->in_altscreen)
        return false;
    if (r->kind == IMG_KIND_ANCHORED) {
        long top = r->abs_line - vt->sixel_abs_top;
        return top <= vt->rows - 1 && top + r->rows_tall - 1 >= 0;
    }
    for (int i = 0; i < st->place_count; ++i) {
        const CfrPlacement *p = &st->places[i];
        if (p->image_handle != r->handle || p->screen != r->screen)
            continue;
        long top = p->abs_line - vt->sixel_abs_top;
        if (top <= vt->rows - 1 && top + p->rows - 1 >= 0)
            return true;
    }
    return false;
}

/* The oldest record, by creation order. The order is total over every
 * record (a 1:N image has no anchor of its own to order it by, so an
 * anchor-based order was arbitrary for it — and the "lowest abs_line"
 * comparison ties for every named image, which made the evictions take
 * whichever record happened to sit first in the dense array). */
static int img_oldest(const CfrImgStore *st)
{
    int m = 0;
    for (int i = 1; i < st->img_count; ++i)
        if (st->imgs[i].seq < st->imgs[m].seq)
            m = i;
    return m;
}

/* The byte-budget eviction's victim: the OLDEST record that is not on
 * the screen. A kitty image the client placed and still wants must not
 * be reclaimed by a flood of transmissions while an invisible record
 * (a transmit it never placed, a scrolled-away image, the other
 * screen's set) could go instead. When every record is visible, the
 * oldest one still goes: the budget has to be honoured. */
static int img_evict_victim(CfrTerm *vt, CfrImgStore *st)
{
    int oldest = img_oldest(st);
    int oldest_hidden = -1;
    for (int i = 0; i < st->img_count; ++i) {
        if (img_is_visible(vt, st, &st->imgs[i]))
            continue;
        if (oldest_hidden < 0 ||
            st->imgs[i].seq < st->imgs[oldest_hidden].seq)
            oldest_hidden = i;
    }
    return oldest_hidden >= 0 ? oldest_hidden : oldest;
}

void img_evict_to_budget(void *vt, CfrImgStore *st, size_t incoming)
{
    while (st->img_count > 0 && st->live_bytes + incoming > IMG_LIVE_MAX)
        img_rec_release(vt, st, img_evict_victim(vt, st));
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
    img_rec_release(vt, st, img_oldest(st));
}

int cfr_img_find_at(CfrImgStore *st, long abs_line, int col, uint8_t layer,
                    uint8_t screen)
{
    for (int i = 0; i < st->img_count; ++i)
        if (st->imgs[i].kind == IMG_KIND_ANCHORED &&
            st->imgs[i].screen == screen &&
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

/* Find an image by its PUBLIC id within one protocol family and one
 * screen. */
int cfr_img_find_by_source_id(CfrImgStore *st, uint8_t source, uint64_t id,
                              uint8_t screen)
{
    for (int i = 0; i < st->img_count; ++i)
        if (st->imgs[i].source == source && st->imgs[i].id == id &&
            st->imgs[i].screen == screen)
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
    uint8_t screen = st->places[idx].screen;
    st->places[idx] = st->places[--st->place_count];
    img_reanchor(st, handle);
    img_damage(vt, screen, abs_line, rows);
}

void cfr_img_remove_placement(void *vt, CfrImgStore *st, int idx)
{
    if (idx < 0 || idx >= st->place_count)
        return;
    place_rec_release(vt, st, idx);
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
    uint8_t screen = st->imgs[img_idx].screen;

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
            img_damage(vt, pl->screen, abs_line, rows);
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
    pl->screen = screen;
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
    img_damage(vt, screen, abs_line, rows);
    return st->place_count - 1;
}

/* Move the text cursor after a placed image and scroll the grid as
 * needed.
 *
 * The cursor lands on the FIRST ROW BELOW the image's cell footprint
 * (rows_tall rows down), so the text an app prints after an image is
 * never covered by the image's pixels. An image whose pixel height is
 * not a multiple of the cell height spills into the row under its
 * footprint (a 512px image in 21px cells covers 25 rows, the last one
 * 8px deep), and a cursor on that row prints its text under the
 * spill — the text a producer (img2sixel) prints after the image
 * comes out cut.
 *
 * The loop still scrolls when it runs out of screen, so an image
 * anchored at the bottom margin scrolls into view and the cursor ends
 * below it. */
static void img_advance_cursor(CfrTerm *vt, int rows_tall, int cols_wide)
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
    int rows = rows_tall;
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

/* Damage the display rows covered by a newly placed/updated image. A
 * record of the other screen is not visible, so its changes do not
 * dirty the screen being shown. */
static void img_damage(CfrTerm *vt, uint8_t screen, long abs_line,
                       int rows_tall)
{
    if (screen != (uint8_t)vt->in_altscreen)
        return;
    int disp_row = (int)(abs_line - vt->sixel_abs_top);
    int a = disp_row < 0 ? 0 : disp_row;
    int b = disp_row + rows_tall - 1;
    if (b >= vt->rows)
        b = vt->rows - 1;
    for (int r = a; r <= b; ++r)
        cfr_damage_row(vt, r);
}

void cfr_img_damage_placements(void *vt, CfrImgStore *st, uint64_t handle)
{
    for (int i = 0; i < st->place_count; ++i) {
        if (st->places[i].image_handle != handle)
            continue;
        img_damage(vt, st->places[i].screen, st->places[i].abs_line,
                   st->places[i].rows);
    }
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

    /* Grid occupancy: the display dimensions are device pixels, the
     * same unit as the physical cell, so the box is a plain ceiling
     * division — no scale conversion. */
    int cell_h = cvt->cell_h_px;
    int cell_w = cvt->cell_w_px;
    int rows_tall = (disp_h + cell_h - 1) / cell_h;
    int cols_wide = (disp_w + cell_w - 1) / cell_w;
    if (rows_tall < 1)
        rows_tall = 1;
    if (cols_wide < 1)
        cols_wide = 1;

    /* Capture anchor */
    uint8_t screen = (uint8_t)cvt->in_altscreen;
    long abs_line = cvt->sixel_abs_top + cvt->cursor.row;
    int col = cvt->cursor.col;

    /* Animation / in-place replacement */
    int existing = cfr_img_find_at(st, abs_line, col, layer, screen);
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
        img_damage(cvt, r->screen, abs_line, rows_tall);
        img_advance_cursor(cvt, rows_tall, cols_wide);
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
    r->image_num = 0;
    r->has_image_num = 0;
    r->has_client_id = 0;
    r->layer = layer;
    r->source = source;
    r->kind = IMG_KIND_ANCHORED;
    r->screen = screen;
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
    r->frames = NULL;
    r->frame_count = 0;
    r->frame_cap = 0;
    r->current_frame = 1;
    r->anim_mode = 0;
    r->loops_left = 0;
    r->next_advance_us = 0;
    r->frame_bytes = 0;
    r->root_gap_ms = 0;
    r->root_has_gap = false;
    r->root_rgba = NULL;
    r->root_cap = 0;

    memcpy(buf, rgba, need);

    img_damage(cvt, screen, abs_line, rows_tall);
    img_advance_cursor(cvt, rows_tall, cols_wide);
    return st->img_count - 1;
}

int cfr_img_put_named(void *vt, CfrImgStore *st,
                      uint64_t id, const uint8_t *rgba, int w, int h,
                      uint8_t layer, uint8_t source)
{
    if (w <= 0 || h <= 0 || w > IMG_MAX_DIM || h > IMG_MAX_DIM)
        return -1;

    size_t need = (size_t)w * (size_t)h * 4u;
    if (need == 0 || need > IMG_LIVE_MAX)
        return -1;

    uint8_t screen = (uint8_t)((CfrTerm *)vt)->in_altscreen;
    int existing = cfr_img_find_by_source_id(st, source, id, screen);
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
        if (rgba) {
            memcpy(r->rgba, rgba, need);
            r->version = ++st->version_seq;
        } else {
            /* A caller-owned raster target: zeroed, not versioned — the
             * caller reports the pixel change when it is done. */
            memset(r->rgba, 0, need);
        }
        r->w = w;
        r->h = h;
        r->buf_w = w;
        r->buf_h = h;
        r->layer = layer;
        r->source = source;
        /* Re-storing pixels is a new image identity: any I= number the
         * old record carried is gone, and so are its animation frames. */
        r->image_num = 0;
        r->has_image_num = 0;
        r->has_client_id = 0;
        img_frames_release(vt, st, r);
        img_root_release(vt, st, r);
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
    r->image_num = 0;
    r->has_image_num = 0;
    r->has_client_id = 0;
    r->layer = layer;
    r->source = source;
    r->kind = IMG_KIND_REFERENCED;
    r->screen = screen;
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
    r->frames = NULL;
    r->frame_count = 0;
    r->frame_cap = 0;
    r->current_frame = 1;
    r->anim_mode = 0;
    r->loops_left = 0;
    r->next_advance_us = 0;
    r->frame_bytes = 0;
    r->root_gap_ms = 0;
    r->root_has_gap = false;
    r->root_rgba = NULL;
    r->root_cap = 0;
    if (rgba)
        memcpy(buf, rgba, need);
    else
        memset(buf, 0, need);
    return st->img_count - 1;
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

    /* The raster's device pixels against the physical cell — the same
     * unit, a plain ceiling division. */
    CfrTerm *cvt = vt;
    int cell_h = cvt->cell_h_px;
    int cell_w = cvt->cell_w_px;
    r->rows_tall = (h + cell_h - 1) / cell_h;
    r->cols_wide = (w + cell_w - 1) / cell_w;
    if (r->rows_tall < 1)
        r->rows_tall = 1;
    if (r->cols_wide < 1)
        r->cols_wide = 1;

    /* A 1:N image is visible at every placement, so the rows at the
     * record's anchor alone are not enough: a frame switch or a
     * re-transmit must repaint each placement's rows. */
    if (r->kind == IMG_KIND_REFERENCED)
        cfr_img_damage_placements(vt, st, r->handle);
    else
        img_damage(vt, r->screen, r->abs_line, r->rows_tall);
}

/* ------------------------------------------------------------------ */
/* Animation frames (kitty)                                           */
/* ------------------------------------------------------------------ */

/* One alpha blend (src-over) of an RGBA rectangle. */
static void img_blend_px(uint8_t *d, const uint8_t *s)
{
    unsigned sa = s[3];
    if (sa == 255) {
        memcpy(d, s, 4);
        return;
    }
    if (sa == 0)
        return;
    unsigned da = d[3];
    unsigned out_a = sa + da * (255u - sa) / 255u;
    if (out_a == 0) {
        memset(d, 0, 4);
        return;
    }
    for (int c = 0; c < 3; c++)
        d[c] = (uint8_t)(((unsigned)s[c] * sa +
                          (unsigned)d[c] * da * (255u - sa) / 255u) /
                         out_a);
    d[3] = (uint8_t)out_a;
}

/* Frames use kitty's 1-based numbering: 1 is the ROOT image (the
 * record's own rgba buffer), extra frames are 2..frame_count + 1 and
 * live in frames[frame_n - 2]. */
int cfr_img_frame_put(void *vt, CfrImgStore *st, int idx,
                      const uint8_t *rgba, int w, int h, int fx, int fy,
                      int edit_n, int base_n, bool overwrite,
                      uint32_t bg_rgba, int gap_ms)
{
    if (idx < 0 || idx >= st->img_count || !rgba || w <= 0 || h <= 0)
        return -1;
    CfrImg *img = &st->imgs[idx];
    int img_w = img->buf_w > 0 ? img->buf_w : img->w;
    int img_h = img->buf_h > 0 ? img->buf_h : img->h;
    if (fx < 0 || fy < 0 || fx + w > img_w || fy + h > img_h)
        return -1;
    if (edit_n > img->frame_count + 1 || base_n > img->frame_count + 1)
        return -1;
    if (edit_n == 0)
        edit_n = img->frame_count + 2; /* append */

    size_t need = (size_t)img_w * (size_t)img_h * 4u;

    /* The first frame freezes the root image's pixels: rgba becomes
     * the DISPLAY buffer that frame switches copy into, and the root
     * data lives on in root_rgba so a switch back restores it. */
    if (img->frame_count == 0 && !img->root_rgba) {
        size_t rcap = 0;
        uint8_t *rb = img_buf_alloc(vt, st, need, &rcap);
        if (!rb)
            return -1;
        memcpy(rb, img->rgba, need);
        img->root_rgba = rb;
        img->root_cap = rcap;
        st->live_bytes += rcap;
    }

    /* The frame canvas, creating it when the put appends a frame. */
    CfrFrame *frame;
    if (edit_n >= 2 && edit_n <= img->frame_count + 1) {
        frame = &img->frames[edit_n - 2];
    } else {
        if (img->frame_count >= IMG_MAX_FRAMES)
            return -1;
        if (img->frame_count == img->frame_cap) {
            int ncap = img->frame_cap ? img->frame_cap * 2 : 4;
            if (ncap > IMG_MAX_FRAMES)
                ncap = IMG_MAX_FRAMES;
            CfrFrame *nf = cfr_realloc(vt, img->frames,
                                       (size_t)ncap * sizeof(CfrFrame));
            if (!nf)
                return -1;
            img->frames = nf;
            img->frame_cap = ncap;
        }
        frame = &img->frames[img->frame_count++];
        memset(frame, 0, sizeof(*frame));
    }

    if (frame->cap < need) {
        size_t cap = 0;
        uint8_t *buf = img_buf_alloc(vt, st, need, &cap);
        if (!buf)
            return -1;
        if (frame->rgba) {
            st->live_bytes -= frame->cap;
            img->frame_bytes -= frame->cap;
            img_buf_release(vt, st, frame->rgba, frame->cap);
        }
        frame->rgba = buf;
        frame->cap = cap;
        img->frame_bytes += cap;
        st->live_bytes += cap;
    }

    /* Fill the canvas: the base frame's pixels, else the Y= background
     * (transparent black by default). */
    if (base_n > 0) {
        const uint8_t *base;
        if (base_n == 1)
            base = img->root_rgba ? img->root_rgba : img->rgba;
        else
            base = img->frames[base_n - 2].rgba;
        memcpy(frame->rgba, base, need);
    } else if (bg_rgba != 0) {
        uint8_t b = (uint8_t)(bg_rgba >> 16);
        uint8_t g = (uint8_t)(bg_rgba >> 8);
        uint8_t r = (uint8_t)bg_rgba;
        uint8_t a = (uint8_t)(bg_rgba >> 24);
        for (int i = 0; i < img_w * img_h; i++) {
            frame->rgba[i * 4 + 0] = r;
            frame->rgba[i * 4 + 1] = g;
            frame->rgba[i * 4 + 2] = b;
            frame->rgba[i * 4 + 3] = a;
        }
    } else {
        memset(frame->rgba, 0, need);
    }

    /* Compose the rect: X=1 replaces, the default alpha blends. */
    for (int y = 0; y < h; y++) {
        uint8_t *drow = frame->rgba +
                        ((size_t)(fy + y) * (size_t)img_w + (size_t)fx) * 4u;
        const uint8_t *srow = rgba + (size_t)y * (size_t)w * 4u;
        if (overwrite) {
            memcpy(drow, srow, (size_t)w * 4u);
            continue;
        }
        for (int x = 0; x < w; x++)
            img_blend_px(drow + (size_t)x * 4u, srow + (size_t)x * 4u);
    }

    if (gap_ms != 0) {
        frame->gap_ms = gap_ms;
        frame->has_gap = true;
    }

    /* A frame arriving resumes a loading animation parked at the end. */
    if (img->anim_mode == 1)
        img->next_advance_us = 0;

    return edit_n;
}

bool cfr_img_frame_set_current(void *vt, CfrImgStore *st, int idx,
                               int frame_n)
{
    if (idx < 0 || idx >= st->img_count)
        return false;
    CfrImg *img = &st->imgs[idx];
    if (frame_n < 1 || frame_n > img->frame_count + 1)
        return false;
    img->current_frame = frame_n;
    if (frame_n > 1) {
        const CfrFrame *f = &img->frames[frame_n - 2];
        cfr_img_replace(vt, st, idx, f->rgba, img->buf_w, img->buf_h);
    } else if (img->root_rgba) {
        /* Back to the root frame: restore its kept pixels. */
        cfr_img_replace(vt, st, idx, img->root_rgba, img->buf_w,
                        img->buf_h);
    }
    return true;
}

bool cfr_img_frame_set_gap(CfrImgStore *st, int idx, int frame_n,
                           int gap_ms)
{
    if (idx < 0 || idx >= st->img_count)
        return false;
    CfrImg *img = &st->imgs[idx];
    if (frame_n < 1 || frame_n > img->frame_count + 1)
        return false;
    if (frame_n == 1) {
        img->root_gap_ms = gap_ms;
        img->root_has_gap = gap_ms != 0;
        return true;
    }
    img->frames[frame_n - 2].gap_ms = gap_ms;
    img->frames[frame_n - 2].has_gap = gap_ms != 0;
    return true;
}

void cfr_img_frames_delete(void *vt, CfrImgStore *st, int idx, int frame_n)
{
    if (idx < 0 || idx >= st->img_count)
        return;
    CfrImg *img = &st->imgs[idx];
    if (frame_n == 0) {
        /* Delete every extra frame; the display falls back to the
         * root image. */
        img_frames_release(vt, st, img);
        if (img->root_rgba)
            cfr_img_replace(vt, st, idx, img->root_rgba, img->buf_w,
                            img->buf_h);
        return;
    }
    if (frame_n < 2 || frame_n > img->frame_count + 1)
        return; /* frame 1 is the root image itself */
    CfrFrame *f = &img->frames[frame_n - 2];
    st->live_bytes -= f->cap;
    img->frame_bytes -= f->cap;
    img_buf_release(vt, st, f->rgba, f->cap);
    memmove(&img->frames[frame_n - 2], &img->frames[frame_n - 1],
            (size_t)(img->frame_count + 1 - frame_n) * sizeof(CfrFrame));
    img->frame_count--;
    bool was_current = img->current_frame == frame_n;
    if (img->current_frame > frame_n)
        img->current_frame--;
    if (was_current && img->current_frame > 1)
        cfr_img_frame_set_current(vt, st, idx, img->current_frame);
}

/* The pixel buffer of one frame by its 1-based number (1 = root). */
static const uint8_t *img_frame_pixels(const CfrImg *img, int frame_n)
{
    if (frame_n == 1)
        return img->root_rgba ? img->root_rgba : img->rgba;
    return img->frames[frame_n - 2].rgba;
}

bool cfr_img_frame_compose(void *vt, CfrImgStore *st, int idx,
                           int src_n, int dst_n, int sx, int sy,
                           int dx, int dy, int w, int h, bool overwrite)
{
    if (idx < 0 || idx >= st->img_count || src_n < 1 || dst_n < 1)
        return false;
    CfrImg *img = &st->imgs[idx];
    if (src_n > img->frame_count + 1 || dst_n > img->frame_count + 1)
        return false;

    int img_w = img->buf_w > 0 ? img->buf_w : img->w;
    int img_h = img->buf_h > 0 ? img->buf_h : img->h;
    if (w <= 0)
        w = img_w;
    if (h <= 0)
        h = img_h;
    if (sx < 0 || sy < 0 || dx < 0 || dy < 0 ||
        sx + w > img_w || sy + h > img_h ||
        dx + w > img_w || dy + h > img_h)
        return false;
    if (src_n == dst_n && sx + w > dx && dx + w > sx &&
        sy + h > dy && dy + h > sy)
        return false; /* same frame, overlapping rects */

    const uint8_t *src = img_frame_pixels(img, src_n);
    uint8_t *dst = dst_n == 1 ? (img->root_rgba ? img->root_rgba
                                                : img->rgba)
                              : img->frames[dst_n - 2].rgba;
    if (!src || !dst)
        return false;

    for (int y = 0; y < h; y++) {
        const uint8_t *srow =
            src + ((size_t)(sy + y) * (size_t)img_w + (size_t)sx) * 4u;
        uint8_t *drow =
            dst + ((size_t)(dy + y) * (size_t)img_w + (size_t)dx) * 4u;
        if (overwrite) {
            memcpy(drow, srow, (size_t)w * 4u);
            continue;
        }
        for (int x = 0; x < w; x++)
            img_blend_px(drow + (size_t)x * 4u, srow + (size_t)x * 4u);
    }

    /* A compose that changes what is displayed must bump the version
     * and damage the placements; one that changes a stored frame that
     * is not showing changes nothing visible. */
    if (dst_n == 1 && img->root_rgba && img->current_frame == 1)
        cfr_img_replace(vt, st, idx, img->root_rgba, img->buf_w,
                        img->buf_h);
    else if (dst_n > 1 && img->current_frame == dst_n)
        cfr_img_frame_set_current(vt, st, idx, dst_n);
    return true;
}

void cfr_img_anim_control(CfrImgStore *st, int idx, int mode, int loops)
{
    if (idx < 0 || idx >= st->img_count)
        return;
    CfrImg *img = &st->imgs[idx];
    if (mode >= 1 && mode <= 3)
        img->anim_mode = (uint8_t)(mode - 1); /* 1 stop, 2 loading, 3 run */
    if (loops > 0)
        img->loops_left = loops == 1 ? -1 : loops - 1;
    /* The next tick picks the new state up immediately. */
    img->next_advance_us = 0;
}

void cfr_img_tick(void *vt, CfrImgStore *st, uint64_t now_us)
{
    if (!st || st->img_count == 0)
        return;
    for (int i = 0; i < st->img_count; i++) {
        CfrImg *img = &st->imgs[i];
        if (img->source != IMG_SRC_KITTY || img->anim_mode == 0)
            continue;
        if (img->next_advance_us > now_us)
            continue;

        /* One frame step per tick: the host calls the clock per frame,
         * and a gapless run renders as fast as the host paints instead
         * of spinning the engine. The loop is still bounded — at most
         * one pass around the frame list. */
        for (;;) {
            int next = img->current_frame + 1;
            if (next > img->frame_count + 1) {
                if (img->anim_mode == 1) {
                    /* Loading mode: wait for more frames to arrive. */
                    img->next_advance_us = UINT64_MAX;
                    break;
                }
                /* Running: loop back to the root frame. */
                if (img->loops_left > 0 && --img->loops_left == 0) {
                    img->anim_mode = 0;
                    break;
                }
                next = 1;
            }
            cfr_img_frame_set_current(vt, st, i, next);
            int gap;
            if (next == 1)
                gap = img->root_has_gap ? img->root_gap_ms : 0;
            else {
                const CfrFrame *f = &img->frames[next - 2];
                gap = f->has_gap ? f->gap_ms : 40;
            }
            if (gap < 0)
                continue; /* gapless: step on immediately */
            img->next_advance_us =
                now_us + (gap > 0 ? (uint64_t)gap * 1000u : 0u);
            break;
        }
    }
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

/* The per-source lifetime and erase rules live in img_policy() at the
 * top of this file: the 1:1 protocols (sixel, iTerm2) are the screen's
 * own, and the 1:N protocols split on kitty's "the client owns the
 * image" rule vs Lottie's "the animation IS its placement" rule. */

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
            if (img_policy(source)->dies_with_placements &&
                st->imgs[i].ever_placed &&
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

/* How a text erase of the display rows [top, bot] affects one 1:N
 * image, per the source's policy. `full` marks the screen-wide clear
 * (ED 2).
 *
 *   - kitty (screen_clear_removes): ED 2 clears the images that are ON
 *     the screen; the other erases never touch it. An image with no
 *     on-screen placement is not visible — it was transmitted but
 *     never placed, or its placement scrolled into the scrollback — so
 *     it survives. "On the screen" is the placement's row EXTENT, not
 *     its first row: kitty's rule is "the image's bottom edge is below
 *     the top of the screen" (kitty/graphics.c clear_filter_func:
 *     start_row + effective_num_rows > 0), so a picture that scrolled
 *     one line up — chafa's label line under a full-height picture
 *     does exactly that — is still on the screen and must go.
 *   - Lottie (rows_erase_placements): the erased rows take their
 *     foreground placements with them, and the record follows its last
 *     placement when the policy says it dies with them.
 *
 * Returns true when the image itself was released (the dense array
 * swap-removes, so the slot now holds another record — the caller must
 * look at it again). */
static bool img_clear_referenced_for(void *vt, CfrImgStore *st, int i,
                                     int top, int bot, bool full)
{
    CfrTerm *cvt = vt;
    const ImgSourcePolicy *pol = img_policy(st->imgs[i].source);
    uint64_t handle = st->imgs[i].handle;

    if (pol->screen_clear_removes && full) {
        bool on_screen = false;
        for (int j = 0; j < st->place_count; ++j) {
            if (st->places[j].image_handle != handle)
                continue;
            int ptop = (int)(st->places[j].abs_line - cvt->sixel_abs_top);
            int pbot = ptop + st->places[j].rows - 1;
            if (ptop <= cvt->rows - 1 && pbot >= 0)
                on_screen = true;
        }
        if (!on_screen)
            return false;
        img_rec_release(vt, st, i);
        return true;
    }

    if (!pol->rows_erase_placements)
        return false;

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
    if (removed && pol->dies_with_placements &&
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
        /* An erase of the active screen cannot see (or touch) the other
         * screen's graphics set: kitty keeps them apart, and a main
         * image must survive a visit to an editor. */
        if (st->imgs[i].screen != (uint8_t)cvt->in_altscreen) {
            ++i;
            continue;
        }

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

        /* 1:N — the source's policy decides (img_clear_referenced_for). */
        if (img_clear_referenced_for(vt, st, i, top, bot, full))
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
            st->imgs[i].kind != IMG_KIND_REFERENCED ||
            st->imgs[i].screen != (uint8_t)((CfrTerm *)vt)->in_altscreen) {
            ++i;
            continue;
        }
        if (img_clear_referenced_for(vt, st, i, top, bot, false))
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
 * whose anchors are the grid's own rows. A resize re-anchors the grid,
 * so those images cannot be followed — but the client-owned 1:N
 * images (kitty) live on: they are anchored by their own placements,
 * and the client deletes them when it wants them gone. */
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

/* kitty: "When switching from the main screen to the alternate screen
 * buffer (1049 private mode) all images in the alternate screen must be
 * cleared, just as all text is cleared." The main screen's images are
 * preserved across the visit — they are still the main screen's, and
 * the public queries hide them while the alt screen is active. Since
 * the alt screen is a blank buffer in both directions, dropping its
 * records on every switch is the same rule read from either end. */
void cfr_img_screen_switch(void *vt, CfrImgStore *st)
{
    if (!st)
        return;
    for (int i = 0; i < st->img_count;) {
        if (st->imgs[i].screen == IMG_SCREEN_ALT)
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

    /* The display dimensions are the raster's own device pixels: the
     * renderer draws them 1:1 and does zero conversions. buf_w/buf_h
     * are the REAL dimensions of the pixel buffer the host reads; for
     * an unscaled raster display == buffer, so a whole-texture upload
     * is exactly the raster (no read-past-end, no stretch). */
    uint8_t active = (uint8_t)cvt->in_altscreen;
    int n = 0;

    for (int i = 0; i < st->img_count; ++i) {
        CfrImg *r = &st->imgs[i];
        if (r->screen != active)
            continue;
        CfrImage *v = &st->img_scratch[n++];
        v->id = r->id;
        v->handle = r->handle;
        v->version = r->version;
        v->layer = r->layer;
        v->source = r->source;
        v->row = (int)(r->abs_line - cvt->sixel_abs_top);
        v->col = r->col;
        v->width_px = r->w;
        v->height_px = r->h;
        v->buf_w = r->buf_w;
        v->buf_h = r->buf_h;
        v->rgba = r->rgba;
    }
    if (n == 0)
        return NULL;
    if (out_count)
        *out_count = n;
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

    uint8_t active = (uint8_t)cvt->in_altscreen;
    int n = 0;
    for (int i = 0; i < st->place_count; ++i) {
        CfrPlacement *r = &st->places[i];
        if (r->screen != active)
            continue;
        CfrImagePlacement *v = &st->place_scratch[n++];
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
        v->cell_off_x = r->cell_off_x;
        v->cell_off_y = r->cell_off_y;
    }
    if (n == 0)
        return NULL;
    if (out_count)
        *out_count = n;
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
     * source), or to all of them (IMG_SRC_ANY), and to the active
     * screen. */
    uint8_t active = (uint8_t)cvt->in_altscreen;
    int want = 0;
    for (int i = 0; i < st->place_count; ++i) {
        if (st->places[i].image_id != image_id ||
            st->places[i].screen != active)
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
        if (r->image_id != image_id || r->screen != active)
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
        v->cell_off_x = r->cell_off_x;
        v->cell_off_y = r->cell_off_y;
    }
    if (out_count)
        *out_count = want;
    return st->place_scratch;
}
