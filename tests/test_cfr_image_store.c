/* tests/test_cfr_image_store.c — shared image store (TDD)
 *
 * Tests for the generic image storage extracted from sixel.c and lottie.c.
 * Covers: buffer pool, image lifecycle, eviction, scroll/clear.
 */

#include "coffer_internal.h"
#include "test_helpers.h"
#include <coffer/coffer.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* --------------------------------------------------------------- */
/* Test harness                                                    */
/* --------------------------------------------------------------- */

/* Damage tracking: the host's only repaint trigger. A mutation that
 * changes the screen and reports no damage is a change the host never
 * draws (the "blank space" bug). */
static int g_damage_calls;
static int g_damage_min_row;
static int g_damage_max_row;

static void on_damage(CfrRect r, void *u)
{
    (void)u;
    g_damage_calls++;
    if (g_damage_calls == 1) {
        g_damage_min_row = r.start_row;
        g_damage_max_row = r.end_row;
        return;
    }
    if (r.start_row < g_damage_min_row)
        g_damage_min_row = r.start_row;
    if (r.end_row > g_damage_max_row)
        g_damage_max_row = r.end_row;
}

static CfrTerm *make_term(int rows, int cols)
{
    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.rows = rows;
    cfg.cols = cols;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 6;
    CfrTerm *vt = cfr_new(&cfg);
    if (!vt)
        return NULL;
    CfrCallbacks cb = { 0 };
    cb.damage = on_damage;
    cfr_set_callbacks(vt, &cb, NULL);
    cfr_set_cell_pixels(vt, 10, 6);
    g_damage_calls = 0;
    g_damage_min_row = 0;
    g_damage_max_row = 0;
    return vt;
}

static void feed(CfrTerm *vt, const char *s)
{
    cfr_input_write(vt, (const uint8_t *)s, strlen(s));
}

/* Make a small RGBA buffer filled with a solid color. */
static uint8_t *make_rgba(int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    uint8_t *buf = malloc((size_t)w * h * 4);
    for (int i = 0; i < w * h; i++) {
        buf[i * 4 + 0] = r;
        buf[i * 4 + 1] = g;
        buf[i * 4 + 2] = b;
        buf[i * 4 + 3] = a;
    }
    return buf;
}

/* --------------------------------------------------------------- */
/* 1. Buffer pool: alloc, release, reuse                          */
/* --------------------------------------------------------------- */

static void test_buf_alloc_basic(void)
{
    CfrTerm *vt = make_term(24, 80);
    ASSERT_NOT_NULL(vt);

    CfrImgStore *st = cfr_img_store_new(vt);
    ASSERT_NOT_NULL(st);

    size_t cap = 0;
    uint8_t *p1 = img_buf_alloc(vt, st, 100, &cap);
    ASSERT_NOT_NULL(p1);
    ASSERT_TRUE(cap >= 100);

    /* Fill with a marker */
    memset(p1, 0xAB, 100);

    /* Release it back to the pool */
    img_buf_release(vt, st, p1, cap);

    /* Alloc the same size — should reuse the pooled buffer */
    size_t cap2 = 0;
    uint8_t *p2 = img_buf_alloc(vt, st, 100, &cap2);
    ASSERT_NOT_NULL(p2);
    /* The pool does a best-fit pop; the retained buffer should be returned */
    ASSERT_TRUE(cap2 >= 100);

    img_buf_release(vt, st, p2, cap2);

    cfr_img_store_free(vt, st);
    cfr_free(vt);
}

static void test_buf_alloc_best_fit(void)
{
    CfrTerm *vt = make_term(24, 80);
    CfrImgStore *st = cfr_img_store_new(vt);

    /* Release three buffers of different sizes */
    size_t cap;
    uint8_t *p1 = img_buf_alloc(vt, st, 100, &cap);
    img_buf_release(vt, st, p1, cap);
    uint8_t *p2 = img_buf_alloc(vt, st, 200, &cap);
    img_buf_release(vt, st, p2, cap);
    uint8_t *p3 = img_buf_alloc(vt, st, 300, &cap);
    img_buf_release(vt, st, p3, cap);

    /* Now alloc 50 — best-fit should return the 100-byte buffer,
     * not the 300-byte one */
    size_t cap50 = 0;
    uint8_t *p50 = img_buf_alloc(vt, st, 50, &cap50);
    ASSERT_NOT_NULL(p50);
    ASSERT_TRUE(cap50 >= 50);
    /* The 100-byte buffer (smallest that fits) should be returned */
    ASSERT_EQ(cap50, (size_t)100);

    img_buf_release(vt, st, p50, cap50);

    cfr_img_store_free(vt, st);
    cfr_free(vt);
}

static void test_buf_pool_overflow_frees(void)
{
    CfrTerm *vt = make_term(24, 80);
    CfrImgStore *st = cfr_img_store_new(vt);

    /* Fill the pool beyond IMG_SPARE_MAX — excess should be freed,
     * not retained */
    size_t cap;
    uint8_t *ptrs[IMG_SPARE_MAX + 4];
    for (int i = 0; i < IMG_SPARE_MAX + 4; i++) {
        ptrs[i] = img_buf_alloc(vt, st, 100, &cap);
        ASSERT_NOT_NULL(ptrs[i]);
    }
    /* Release them all — only IMG_SPARE_MAX are retained */
    for (int i = 0; i < IMG_SPARE_MAX + 4; i++)
        img_buf_release(vt, st, ptrs[i], 100);

    /* spare_count should be capped at IMG_SPARE_MAX */
    ASSERT_EQ(st->spare_count, IMG_SPARE_MAX);

    cfr_img_store_free(vt, st);
    cfr_free(vt);
}

/* --------------------------------------------------------------- */
/* 2. Image lifecycle: add, find, replace                         */
/* --------------------------------------------------------------- */

static void test_img_add_basic(void)
{
    CfrTerm *vt = make_term(24, 80);
    CfrImgStore *st = cfr_img_store_new(vt);

    uint8_t *rgba = make_rgba(10, 10, 255, 0, 0, 255);
    int idx = cfr_img_add(vt, st, rgba, 10, 10, 0, 0, 0, IMG_SRC_SIXEL);
    free(rgba);

    ASSERT_TRUE(idx >= 0);
    ASSERT_EQ(st->img_count, 1);

    CfrImg *img = &st->imgs[idx];
    ASSERT_EQ(img->w, 10);
    ASSERT_EQ(img->h, 10);
    ASSERT_EQ(img->layer, 0);
    ASSERT_EQ(img->source, IMG_SRC_SIXEL);
    ASSERT_EQ(img->abs_line, vt->sixel_abs_top + 0); /* cursor row 0 */
    ASSERT_EQ(img->col, 0);
    ASSERT_TRUE(img->rgba != NULL);
    /* Pixel data should be the red we wrote */
    ASSERT_EQ(img->rgba[0], 255); /* R */
    ASSERT_EQ(img->rgba[3], 255); /* A */

    cfr_img_store_free(vt, st);
    cfr_free(vt);
}

static void test_img_add_advances_cursor(void)
{
    CfrTerm *vt = make_term(24, 80);
    CfrImgStore *st = cfr_img_store_new(vt);

    /* Image is 10px wide, 10px tall, cell is 10x6 → 1 col, 2 rows.
     * The cursor lands below the image's footprint, so the text that
     * follows it is never under the image's pixels. */
    uint8_t *rgba = make_rgba(10, 10, 0, 255, 0, 255);
    cfr_img_add(vt, st, rgba, 10, 10, 0, 0, 0, IMG_SRC_SIXEL);
    free(rgba);

    ASSERT_EQ(vt->cursor.row, 2);
    ASSERT_EQ(vt->cursor.col, 0);

    cfr_img_store_free(vt, st);
    cfr_free(vt);
}

/* iTerm2 1337 lands the same way: below the image. */
static void test_img_add_iterm2_advances_below(void)
{
    CfrTerm *vt = make_term(24, 80);
    CfrImgStore *st = cfr_img_store_new(vt);

    uint8_t *rgba = make_rgba(10, 10, 0, 255, 0, 255);
    cfr_img_add(vt, st, rgba, 10, 10, 0, 0, 0, IMG_SRC_ITERM);
    free(rgba);

    /* 10px tall in a 6px cell → 2 rows; the cursor goes below both. */
    ASSERT_EQ(vt->cursor.row, 2);
    ASSERT_EQ(vt->cursor.col, 0);

    cfr_img_store_free(vt, st);
    cfr_free(vt);
}

static void test_img_find_at(void)
{
    CfrTerm *vt = make_term(24, 80);
    CfrImgStore *st = cfr_img_store_new(vt);

    uint8_t *rgba = make_rgba(10, 6, 0, 0, 255, 255);
    int idx = cfr_img_add(vt, st, rgba, 10, 6, 0, 0, 0, IMG_SRC_SIXEL);
    free(rgba);

    long abs_line = st->imgs[idx].abs_line;
    int col = st->imgs[idx].col;

    /* Find the image we just added */
    int found = cfr_img_find_at(st, abs_line, col, 0);
    ASSERT_EQ(found, idx);

    /* Search for a non-existent anchor */
    int not_found = cfr_img_find_at(st, abs_line + 100, col, 0);
    ASSERT_EQ(not_found, -1);

    cfr_img_store_free(vt, st);
    cfr_free(vt);
}

static void test_img_replace(void)
{
    CfrTerm *vt = make_term(24, 80);
    CfrImgStore *st = cfr_img_store_new(vt);

    uint8_t *rgba1 = make_rgba(10, 6, 255, 0, 0, 255);
    int idx = cfr_img_add(vt, st, rgba1, 10, 6, 0, 0, 0, IMG_SRC_SIXEL);
    free(rgba1);

    uint32_t v1 = st->imgs[idx].version;

    /* Replace with green */
    uint8_t *rgba2 = make_rgba(10, 6, 0, 255, 0, 255);
    cfr_img_replace(vt, st, idx, rgba2, 10, 6);
    free(rgba2);

    /* Version should bump */
    ASSERT_EQ(st->imgs[idx].version, v1 + 1);
    /* Pixel data should be green now */
    ASSERT_EQ(st->imgs[idx].rgba[1], 255); /* G */

    cfr_img_store_free(vt, st);
    cfr_free(vt);
}

/* Storing new pixels under an existing named id must bump the version,
 * exactly like cfr_img_replace does — hosts key their texture cache on
 * (id, version) and re-upload only when the version changes. */
static void test_img_add_named_replace_bumps_version(void)
{
    CfrTerm *vt = make_term(24, 80);
    CfrImgStore *st = cfr_img_store_new(vt);

    uint8_t *rgba1 = make_rgba(4, 4, 255, 0, 0, 255);
    int idx = cfr_img_add_named(vt, st, 42, rgba1, 4, 4, 0, IMG_SRC_KITTY);
    free(rgba1);
    ASSERT_TRUE(idx >= 0);
    uint32_t v1 = st->imgs[idx].version;

    /* Same id, same dimensions, new pixels. */
    uint8_t *rgba2 = make_rgba(4, 4, 0, 255, 0, 255);
    int idx2 = cfr_img_add_named(vt, st, 42, rgba2, 4, 4, 0, IMG_SRC_KITTY);
    free(rgba2);
    ASSERT_EQ(idx2, idx);
    ASSERT_TRUE(st->imgs[idx].version > v1);
    ASSERT_EQ(st->imgs[idx].rgba[1], 255); /* G */

    cfr_img_store_free(vt, st);
    cfr_free(vt);
}

/* --------------------------------------------------------------- */
/* 3. Eviction: oldest abs_line first under budget                */
/* --------------------------------------------------------------- */

static void test_img_evict_oldest_first(void)
{
    /* Use a small budget so eviction triggers easily.
     * We can't easily change IMG_LIVE_MAX at runtime, so we add
     * many images and verify the oldest (lowest abs_line) is
     * evicted first. With a 10x6 image at 10x6 cells = 40 bytes,
     * we need a lot to hit 128MB. Instead, test the behavior by
     * adding IMG_MAX_IMAGES + 1 images and verifying rec_count
     * stays at IMG_MAX_IMAGES. */
    CfrTerm *vt = make_term(100, 80);
    CfrImgStore *st = cfr_img_store_new(vt);

    /* Fill to capacity */
    for (int i = 0; i < IMG_MAX_IMAGES + 5; i++) {
        uint8_t *rgba = make_rgba(10, 6, (uint8_t)(i & 0xff), 0, 0, 255);
        cfr_img_add(vt, st, rgba, 10, 6, 0, 0, 0, IMG_SRC_SIXEL);
        free(rgba);
        /* Move the cursor down so each image gets a different abs_line
         * (the sixel landing leaves a 1-row image's cursor ON its own
         * row — same-anchor images coalesce, by design). */
        feed(vt, "\n");
    }

    /* Should be capped at IMG_MAX_IMAGES */
    ASSERT_EQ(st->img_count, IMG_MAX_IMAGES);

    /* All images should have valid abs_line and non-null rgba */
    for (int i = 0; i < st->img_count; i++) {
        ASSERT_NOT_NULL(st->imgs[i].rgba);
        ASSERT_TRUE(st->imgs[i].w == 10);
        ASSERT_TRUE(st->imgs[i].h == 6);
    }

    cfr_img_store_free(vt, st);
    cfr_free(vt);
}

/* --------------------------------------------------------------- */
/* 4. Scroll culling and clear                                     */
/* --------------------------------------------------------------- */

static void test_img_scroll_cull(void)
{
    CfrTerm *vt = make_term(24, 80);
    /* Set a small scrollback capacity so images scroll off quickly */
    vt->sb_capacity = 5;
    CfrImgStore *st = cfr_img_store_new(vt);

    /* Add an image at row 0 (abs_line = 0) */
    uint8_t *rgba = make_rgba(10, 6, 255, 0, 0, 255);
    cfr_img_add(vt, st, rgba, 10, 6, 0, 0, 0, IMG_SRC_SIXEL);
    free(rgba);
    ASSERT_EQ(st->img_count, 1);

    /* Simulate scrolling: advance sixel_abs_top past scrollback capacity.
     * The image was 1 row tall (6px / 6px cell = 1 row), anchored at
     * abs_line 0. When sixel_abs_top advances by more than sb_capacity
     * (5) + rows_tall (1), the image should be culled. */
    vt->sixel_abs_top = 10; /* 10 lines scrolled past */
    cfr_img_note_scroll(vt, st, 10);

    /* Image should be culled (depth = 10, bottom_depth = 10 - 1 + 1 = 10 > 5) */
    ASSERT_EQ(st->img_count, 0);

    cfr_img_store_free(vt, st);
    cfr_free(vt);
}

static void test_img_clear_display_rows(void)
{
    CfrTerm *vt = make_term(24, 80);
    CfrImgStore *st = cfr_img_store_new(vt);

    /* Add an image at row 0 */
    uint8_t *rgba = make_rgba(10, 6, 255, 0, 0, 255);
    cfr_img_add(vt, st, rgba, 10, 6, 0, 0, 0, IMG_SRC_SIXEL);
    free(rgba);
    ASSERT_EQ(st->img_count, 1);

    /* Clear rows 0-5 — should remove the image (it's at row 0, 1 row tall) */
    cfr_img_clear_display_rows(vt, st, 0, 5, false);
    ASSERT_EQ(st->img_count, 0);

    cfr_img_store_free(vt, st);
    cfr_free(vt);
}

static void test_img_clear_all(void)
{
    CfrTerm *vt = make_term(24, 80);
    CfrImgStore *st = cfr_img_store_new(vt);

    for (int i = 0; i < 3; i++) {
        uint8_t *rgba = make_rgba(10, 6, 0, 0, 255, 255);
        cfr_img_add(vt, st, rgba, 10, 6, 0, 0, 0, IMG_SRC_SIXEL);
        free(rgba);
        feed(vt, "\n"); /* distinct anchors (see test_img_evict_oldest_first) */
    }
    ASSERT_EQ(st->img_count, 3);

    cfr_img_clear_all(vt, st);
    ASSERT_EQ(st->img_count, 0);

    cfr_img_store_free(vt, st);
    cfr_free(vt);
}

static void test_img_clear_preserves_background_layer(void)
{
    CfrTerm *vt = make_term(24, 80);
    CfrImgStore *st = cfr_img_store_new(vt);

    /* Add a foreground image at row 0 */
    uint8_t *rgba_fg = make_rgba(10, 6, 255, 0, 0, 255);
    cfr_img_add(vt, st, rgba_fg, 10, 6, 0, 0, 0, IMG_SRC_SIXEL);
    free(rgba_fg);

    /* Add a background image at row 0 (layer 1) */
    vt->cursor.row = 0;
    uint8_t *rgba_bg = make_rgba(10, 6, 0, 255, 0, 255);
    cfr_img_add(vt, st, rgba_bg, 10, 6, 0, 0, 1, IMG_SRC_SIXEL);
    free(rgba_bg);

    ASSERT_EQ(st->img_count, 2);

    /* Clear display rows 0-5 — should remove foreground (layer 0)
     * but preserve background (layer 1) */
    cfr_img_clear_display_rows(vt, st, 0, 5, false);
    ASSERT_EQ(st->img_count, 1);
    ASSERT_EQ(st->imgs[0].layer, 1);

    cfr_img_store_free(vt, st);
    cfr_free(vt);
}

/* --------------------------------------------------------------- */
/* 5. Public query API                                            */
/* --------------------------------------------------------------- */

static void test_img_get_returns_images(void)
{
    CfrTerm *vt = make_term(24, 80);
    CfrImgStore *st = cfr_img_store_new(vt);

    uint8_t *rgba = make_rgba(10, 6, 255, 128, 0, 200);
    cfr_img_add(vt, st, rgba, 10, 6, 0, 0, 0, IMG_SRC_SIXEL);
    free(rgba);

    int count = -1;
    const CfrImage *imgs = cfr_img_get(vt, st, &count);
    ASSERT_NOT_NULL(imgs);
    ASSERT_EQ(count, 1);
    ASSERT_EQ(imgs[0].width_px, 10);
    ASSERT_EQ(imgs[0].height_px, 6);
    ASSERT_EQ(imgs[0].source, IMG_SRC_SIXEL);
    ASSERT_EQ(imgs[0].rgba[0], 255); /* R */
    ASSERT_EQ(imgs[0].rgba[1], 128); /* G */
    ASSERT_EQ(imgs[0].rgba[3], 200); /* A (intermediate alpha!) */

    cfr_img_store_free(vt, st);
    cfr_free(vt);
}

static void test_img_get_empty(void)
{
    CfrTerm *vt = make_term(24, 80);
    CfrImgStore *st = cfr_img_store_new(vt);

    int count = -1;
    const CfrImage *imgs = cfr_img_get(vt, st, &count);
    ASSERT_EQ(count, 0);
    ASSERT_NULL(imgs);

    cfr_img_store_free(vt, st);
    cfr_free(vt);
}

/* --------------------------------------------------------------- */
/* 6. Placement records (1:N image-to-placement)                  */
/* --------------------------------------------------------------- */

static void test_img_add_placement_basic(void)
{
    CfrTerm *vt = make_term(24, 80);
    CfrImgStore *st = cfr_img_store_new(vt);

    uint8_t *rgba = make_rgba(10, 6, 255, 0, 0, 255);
    int img_idx = cfr_img_add(vt, st, rgba, 10, 6, 0, 0, 0, IMG_SRC_KITTY);
    free(rgba);
    ASSERT_TRUE(img_idx >= 0);
    uint64_t img_id = st->imgs[img_idx].id;
    uint64_t img_handle = st->imgs[img_idx].handle;

    int p = cfr_img_add_placement(vt, st, img_handle, vt->sixel_abs_top + 2,
                                  3, 4, 5, 0, 200, -1);
    ASSERT_TRUE(p >= 0);
    ASSERT_EQ(st->place_count, 1);

    CfrPlacement *pl = &st->places[p];
    ASSERT_EQ(pl->image_id, img_id);
    ASSERT_EQ(pl->image_handle, img_handle);
    ASSERT_EQ(pl->abs_line, vt->sixel_abs_top + 2);
    ASSERT_EQ(pl->col, 3);
    ASSERT_EQ(pl->rows, 4);
    ASSERT_EQ(pl->cols, 5);
    ASSERT_EQ(pl->layer, 0);
    ASSERT_EQ(pl->opacity_x256, 200);
    ASSERT_EQ(pl->z_index, -1);

    cfr_img_store_free(vt, st);
    cfr_free(vt);
}

static void test_img_add_placement_dedup(void)
{
    CfrTerm *vt = make_term(24, 80);
    CfrImgStore *st = cfr_img_store_new(vt);

    uint8_t *rgba = make_rgba(10, 6, 255, 0, 0, 255);
    int img_idx = cfr_img_add(vt, st, rgba, 10, 6, 0, 0, 0, IMG_SRC_KITTY);
    free(rgba);
    uint64_t img_handle = st->imgs[img_idx].handle;

    int p1 = cfr_img_add_placement(vt, st, img_handle, 100, 0, 2, 2, 0, 255, 0);
    int p2 = cfr_img_add_placement(vt, st, img_handle, 100, 0, 4, 4, 1, 100, 5);
    ASSERT_TRUE(p1 >= 0);
    ASSERT_EQ(p1, p2); /* same image + anchor + col dedups */
    ASSERT_EQ(st->place_count, 1);
    ASSERT_EQ(st->places[p1].rows, 4);  /* updated */
    ASSERT_EQ(st->places[p1].layer, 1); /* updated */
    ASSERT_EQ(st->places[p1].opacity_x256, 100);
    ASSERT_EQ(st->places[p1].z_index, 5);

    cfr_img_store_free(vt, st);
    cfr_free(vt);
}

static void test_img_add_placement_unknown_image(void)
{
    CfrTerm *vt = make_term(24, 80);
    CfrImgStore *st = cfr_img_store_new(vt);

    int p = cfr_img_add_placement(vt, st, 999999, 0, 0, 2, 2, 0, 255, 0);
    ASSERT_EQ(p, -1);
    ASSERT_EQ(st->place_count, 0);

    cfr_img_store_free(vt, st);
    cfr_free(vt);
}

static void test_img_get_placements(void)
{
    CfrTerm *vt = make_term(24, 80);
    CfrImgStore *st = cfr_img_store_new(vt);

    uint8_t *rgba = make_rgba(10, 6, 255, 0, 0, 255);
    int img_idx = cfr_img_add(vt, st, rgba, 10, 6, 0, 0, 0, IMG_SRC_KITTY);
    free(rgba);
    uint64_t img_id = st->imgs[img_idx].id;
    uint64_t img_handle = st->imgs[img_idx].handle;

    cfr_img_add_placement(vt, st, img_handle, vt->sixel_abs_top + 2, 3, 4, 5,
                          0, 200, -1);

    int count = -1;
    const CfrImagePlacement *pls = cfr_img_get_placements(vt, st, &count);
    ASSERT_NOT_NULL(pls);
    ASSERT_EQ(count, 1);
    ASSERT_EQ(pls[0].image_id, img_id);
    ASSERT_EQ(pls[0].image_handle, img_handle);
    ASSERT_EQ(pls[0].row, 2); /* abs_line - sixel_abs_top */
    ASSERT_EQ(pls[0].col, 3);
    ASSERT_EQ(pls[0].rows, 4);
    ASSERT_EQ(pls[0].cols, 5);
    ASSERT_EQ(pls[0].opacity_x256, 200);
    ASSERT_EQ(pls[0].z_index, -1);

    cfr_img_store_free(vt, st);
    cfr_free(vt);
}

static void test_img_clear_removes_placements(void)
{
    CfrTerm *vt = make_term(24, 80);
    CfrImgStore *st = cfr_img_store_new(vt);

    uint8_t *rgba = make_rgba(10, 6, 255, 0, 0, 255);
    int img_idx = cfr_img_add(vt, st, rgba, 10, 6, 0, 0, 0, IMG_SRC_KITTY);
    free(rgba);
    uint64_t img_handle = st->imgs[img_idx].handle;

    cfr_img_add_placement(vt, st, img_handle, vt->sixel_abs_top, 0, 1, 1, 0, 255, 0);
    ASSERT_EQ(st->place_count, 1);

    cfr_img_clear_all(vt, st);
    ASSERT_EQ(st->img_count, 0);
    ASSERT_EQ(st->place_count, 0);

    cfr_img_store_free(vt, st);
    cfr_free(vt);
}

static void test_img_scroll_cull_removes_placements(void)
{
    CfrTerm *vt = make_term(24, 80);
    vt->sb_capacity = 5;
    CfrImgStore *st = cfr_img_store_new(vt);

    uint8_t *rgba = make_rgba(10, 6, 255, 0, 0, 255);
    int img_idx = cfr_img_add(vt, st, rgba, 10, 6, 0, 0, 0, IMG_SRC_KITTY);
    free(rgba);
    uint64_t img_handle = st->imgs[img_idx].handle;

    cfr_img_add_placement(vt, st, img_handle, vt->sixel_abs_top, 0, 1, 1, 0, 255, 0);
    ASSERT_EQ(st->place_count, 1);

    vt->sixel_abs_top = 10;
    cfr_img_note_scroll(vt, st, 10);

    ASSERT_EQ(st->img_count, 0);
    ASSERT_EQ(st->place_count, 0);

    cfr_img_store_free(vt, st);
    cfr_free(vt);
}

/* --------------------------------------------------------------- */
/* 7. Placement-aware scroll cull + clear for 1:N (kitty/lottie)   */
/* --------------------------------------------------------------- */

/* A 1:N image not anchored at the cursor must be culled based on its
 * placement's abs_line, not its image record's abs_line (which is 0).
 *
 * The PLACEMENT is culled when it falls out of the scrollback; the
 * IMAGE is not: a kitty image lives until the client deletes it (the
 * store's lifetime rule per source), so the image record stays. */
static void test_named_image_scroll_culls_by_placement(void)
{
    CfrTerm *vt = make_term(24, 80);
    vt->sb_capacity = 5;
    CfrImgStore *st = cfr_img_store_new(vt);

    uint8_t *rgba = make_rgba(10, 6, 255, 0, 0, 255);
    int img_idx = cfr_img_add_named(vt, st, 7, rgba, 10, 6, 0, IMG_SRC_KITTY);
    free(rgba);
    ASSERT_TRUE(img_idx >= 0);
    uint64_t img_handle = st->imgs[img_idx].handle;

    /* Placement anchored at abs_line 0 (row 0), 1 cell tall. */
    int pi = cfr_img_add_placement(vt, st, img_handle, vt->sixel_abs_top, 0,
                                   1, 1, 0, 255, 0);
    ASSERT_TRUE(pi >= 0);
    ASSERT_EQ(st->place_count, 1);
    ASSERT_EQ(st->img_count, 1);

    /* Advance the scroll top so the placement scrolls past capacity. */
    vt->sixel_abs_top = 10;
    cfr_img_note_scroll(vt, st, 10);

    /* The placement is gone; the kitty image lives on (the client owns
     * it — kitty keeps images until they are deleted). */
    ASSERT_EQ(st->img_count, 1);
    ASSERT_EQ(st->place_count, 0);

    cfr_img_store_free(vt, st);
    cfr_free(vt);
}

/* A placement clears rows based on its own abs_line, even though the
 * image record's abs_line is 0 (1:N model).
 *
 * The source decides the rule: a Lottie animation's foreground
 * placements go with the erased rows (the Lottie spec), while a kitty
 * image ignores the text erases altogether ("the other commands to
 * erase text must have no effect on graphics"). */
static void test_named_image_clear_rows_by_placement(void)
{
    CfrTerm *vt = make_term(24, 80);
    CfrImgStore *st = cfr_img_store_new(vt);

    uint8_t *rgba = make_rgba(10, 6, 255, 0, 0, 255);
    int img_idx = cfr_img_add_named(vt, st, 7, rgba, 10, 6, 0, IMG_SRC_LOTTIE);
    free(rgba);
    ASSERT_TRUE(img_idx >= 0);
    uint64_t img_handle = st->imgs[img_idx].handle;

    /* Placement at display row 5. */
    int pi = cfr_img_add_placement(vt, st, img_handle, vt->sixel_abs_top + 5,
                                   0, 1, 1, 0, 255, 0);
    ASSERT_TRUE(pi >= 0);

    /* Clear rows 0-4 — placement at 5 survives. */
    cfr_img_clear_display_rows(vt, st, 0, 4, false);
    ASSERT_EQ(st->img_count, 1);
    ASSERT_EQ(st->place_count, 1);

    /* Clear rows 5-5 — placement at 5 removed (image + placement: a
     * Lottie animation dies with its last placement). */
    cfr_img_clear_display_rows(vt, st, 5, 5, false);
    ASSERT_EQ(st->img_count, 0);
    ASSERT_EQ(st->place_count, 0);

    cfr_img_store_free(vt, st);
    cfr_free(vt);
}

/* --------------------------------------------------------------- */
/* 8. The invariants (I1–I5)                                       */
/* --------------------------------------------------------------- */

/* I1: the kind is set at creation and never inferred. A 1:1 image
 * (the record carries the anchor) and a 1:N image (the placements
 * carry the anchor) are told apart by the record, not by asking
 * "does it happen to have a placement?". */
static void test_img_kind_is_set_at_creation(void)
{
    CfrTerm *vt = make_term(24, 80);
    CfrImgStore *st = cfr_img_store_new(vt);

    uint8_t *rgba = make_rgba(10, 6, 255, 0, 0, 255);
    int anchored = cfr_img_add(vt, st, rgba, 10, 6, 0, 0, 0, IMG_SRC_SIXEL);
    int referenced = cfr_img_add_named(vt, st, 7, rgba, 10, 6, 0,
                                       IMG_SRC_KITTY);
    int blanked = cfr_img_blank_named(vt, st, 8, 10, 6, 0, IMG_SRC_KITTY);
    free(rgba);
    ASSERT_TRUE(anchored >= 0);
    ASSERT_TRUE(referenced >= 0);
    ASSERT_TRUE(blanked >= 0);

    ASSERT_EQ(st->imgs[anchored].kind, IMG_KIND_ANCHORED);
    ASSERT_EQ(st->imgs[referenced].kind, IMG_KIND_REFERENCED);
    ASSERT_EQ(st->imgs[blanked].kind, IMG_KIND_REFERENCED);

    cfr_img_store_free(vt, st);
    cfr_free(vt);
}

/* I2: a client-assigned id and an engine-assigned id live in
 * different spaces. A kitty image with the client's id 1 and an
 * engine image with the engine's id 1 are two images, and the
 * placement of one never resolves to the other. */
static void test_img_id_spaces_are_separate(void)
{
    CfrTerm *vt = make_term(24, 80);
    CfrImgStore *st = cfr_img_store_new(vt);

    uint8_t *rgba = make_rgba(10, 6, 255, 0, 0, 255);
    /* The kitty image takes the CLIENT's id (1). */
    int kitty = cfr_img_add_named(vt, st, 1, rgba, 10, 6, 0, IMG_SRC_KITTY);
    /* The engine's counter also hands out 1 — a different image. */
    int engine = cfr_img_add(vt, st, rgba, 10, 6, 0, 0, 0, IMG_SRC_ITERM);
    free(rgba);
    ASSERT_TRUE(kitty >= 0);
    ASSERT_TRUE(engine >= 0);
    ASSERT_EQ(st->imgs[kitty].id, 1);
    ASSERT_EQ(st->imgs[engine].id, 1);
    ASSERT_TRUE(st->imgs[kitty].handle != st->imgs[engine].handle);

    /* The scoped lookup finds each one in its own space. */
    ASSERT_EQ(cfr_img_find_by_source_id(st, IMG_SRC_KITTY, 1), kitty);
    ASSERT_EQ(cfr_img_find_by_source_id(st, IMG_SRC_ITERM, 1), engine);
    ASSERT_EQ(cfr_img_find_by_source_id(st, IMG_SRC_LOTTIE, 1), -1);

    /* The handle lookup is unambiguous. */
    ASSERT_EQ(cfr_img_find_by_handle(st, st->imgs[kitty].handle), kitty);
    ASSERT_EQ(cfr_img_find_by_handle(st, st->imgs[engine].handle), engine);

    /* A placement names its image by the handle, so it follows the
     * kitty image and not the engine one. */
    int p = cfr_img_add_placement(vt, st, st->imgs[kitty].handle,
                                  vt->sixel_abs_top, 0, 1, 1, 0, 255, 0);
    ASSERT_TRUE(p >= 0);
    ASSERT_EQ(st->places[p].image_id, 1);
    ASSERT_EQ(st->places[p].image_handle, st->imgs[kitty].handle);
    ASSERT_EQ(st->places[p].source, IMG_SRC_KITTY);

    /* The public query can be scoped to one protocol. */
    int count = -1;
    const CfrImagePlacement *pls =
        cfr_img_get_placements_for(vt, st, IMG_SRC_KITTY, 1, &count);
    ASSERT_NOT_NULL(pls);
    ASSERT_EQ(count, 1);
    count = -1;
    pls = cfr_img_get_placements_for(vt, st, IMG_SRC_ITERM, 1, &count);
    ASSERT_NULL(pls);
    ASSERT_EQ(count, 0);

    cfr_img_store_free(vt, st);
    cfr_free(vt);
}

/* I5: the eviction's order is the creation order — oldest first, over
 * EVERY record. The dense array is swap-removed, so the record at
 * index 0 is an arbitrary one: evicting it took the NEWEST image
 * (the "random eviction" the report saw). */
static void test_img_evict_takes_the_oldest(void)
{
    CfrTerm *vt = make_term(100, 80);
    CfrImgStore *st = cfr_img_store_new(vt);

    uint8_t *rgba = make_rgba(10, 6, 0, 0, 0, 255);
    uint64_t first_handle = 0;
    uint64_t last_handle = 0;
    for (int i = 0; i < IMG_MAX_IMAGES + 1; i++) {
        int idx = cfr_img_add_named(vt, st, (uint64_t)(i + 1), rgba, 10, 6, 0,
                                    IMG_SRC_KITTY);
        ASSERT_TRUE(idx >= 0);
        if (i == 0)
            first_handle = st->imgs[idx].handle;
        last_handle = st->imgs[idx].handle;
        if (i > 0)
            ASSERT_TRUE(st->imgs[idx].seq > 0);
    }
    free(rgba);

    ASSERT_EQ(st->img_count, IMG_MAX_IMAGES);
    /* The oldest is gone, the newest is alive. */
    ASSERT_EQ(cfr_img_find_by_handle(st, first_handle), -1);
    ASSERT_TRUE(cfr_img_find_by_handle(st, last_handle) >= 0);

    cfr_img_store_free(vt, st);
    cfr_free(vt);
}

/* I3: a mutation that changes the screen reports damage. A placement
 * that reports nothing is a placement the host never draws — the
 * image lands as a blank space. */
static void test_img_placement_damages_rows(void)
{
    CfrTerm *vt = make_term(24, 80);
    CfrImgStore *st = cfr_img_store_new(vt);

    uint8_t *rgba = make_rgba(10, 6, 255, 0, 0, 255);
    int idx = cfr_img_add_named(vt, st, 7, rgba, 10, 6, 0, IMG_SRC_KITTY);
    free(rgba);
    ASSERT_TRUE(idx >= 0);
    uint64_t handle = st->imgs[idx].handle;

    g_damage_calls = 0;
    cfr_damage_flush(vt);

    /* A placement at row 4, three rows tall. */
    int p = cfr_img_add_placement(vt, st, handle, vt->sixel_abs_top + 4, 0,
                                  3, 1, 0, 255, 0);
    ASSERT_TRUE(p >= 0);
    cfr_damage_flush(vt);
    ASSERT_TRUE(g_damage_calls > 0);
    ASSERT_TRUE(g_damage_min_row <= 4);
    ASSERT_TRUE(g_damage_max_row >= 6);

    /* Removing the placement dirties the rows it used to cover: the
     * host must repaint them, or the image stays on the screen. */
    g_damage_calls = 0;
    cfr_damage_flush(vt);
    cfr_img_remove(vt, st, idx);
    cfr_damage_flush(vt);
    ASSERT_TRUE(g_damage_calls > 0);
    ASSERT_TRUE(g_damage_min_row <= 4);
    ASSERT_TRUE(g_damage_max_row >= 6);

    cfr_img_store_free(vt, st);
    cfr_free(vt);
}

/* I4: the buffer dimensions the host reads are the buffer's REAL
 * dimensions. The logical→physical scaling belongs to the display
 * fields; scaling the buffer fields told the host to read past the
 * end of the allocation on a scaled display. */
static void test_img_buffer_dims_are_the_buffers(void)
{
    CfrTerm *vt = make_term(24, 80);
    cfr_set_content_scale(vt, 2.0f);
    CfrImgStore *st = cfr_img_store_new(vt);

    uint8_t *rgba = make_rgba(4, 4, 0, 255, 0, 255);
    int idx = cfr_img_add(vt, st, rgba, 4, 4, 0, 0, 0, IMG_SRC_SIXEL);
    free(rgba);
    ASSERT_TRUE(idx >= 0);

    int count = -1;
    const CfrImage *imgs = cfr_img_get(vt, st, &count);
    ASSERT_NOT_NULL(imgs);
    ASSERT_EQ(count, 1);
    /* The buffer holds the decoded 4x4 pixels. */
    ASSERT_EQ(imgs[0].buf_w, 4);
    ASSERT_EQ(imgs[0].buf_h, 4);
    /* The display is scaled. */
    ASSERT_EQ(imgs[0].width_px, 8);
    ASSERT_EQ(imgs[0].height_px, 8);

    cfr_img_store_free(vt, st);
    cfr_free(vt);
}

/* I2: the handle is the image's identity for its whole life — it
 * does not change when the pixels are replaced, and it is never
 * reused. */
static void test_img_handle_is_stable(void)
{
    CfrTerm *vt = make_term(24, 80);
    CfrImgStore *st = cfr_img_store_new(vt);

    uint8_t *rgba = make_rgba(10, 6, 255, 0, 0, 255);
    int a = cfr_img_add_named(vt, st, 1, rgba, 10, 6, 0, IMG_SRC_KITTY);
    int b = cfr_img_add_named(vt, st, 2, rgba, 10, 6, 0, IMG_SRC_KITTY);
    ASSERT_TRUE(a >= 0);
    ASSERT_TRUE(b >= 0);

    uint64_t ha = st->imgs[a].handle;
    uint64_t hb = st->imgs[b].handle;
    ASSERT_TRUE(ha != hb);

    /* A frame replacement keeps the identity. */
    cfr_img_replace(vt, st, a, rgba, 10, 6);
    ASSERT_EQ(st->imgs[a].handle, ha);
    ASSERT_EQ(st->imgs[b].handle, hb);

    /* A new image never reuses a handle. */
    int c = cfr_img_add_named(vt, st, 3, rgba, 10, 6, 0, IMG_SRC_KITTY);
    ASSERT_TRUE(c >= 0);
    ASSERT_TRUE(st->imgs[c].handle != ha);
    ASSERT_TRUE(st->imgs[c].handle != hb);

    free(rgba);
    cfr_img_store_free(vt, st);
    cfr_free(vt);
}

/* --------------------------------------------------------------- */
/* main                                                           */
/* --------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);
    printf("Running image store tests:\n");

    RUN_TEST(test_buf_alloc_basic);
    RUN_TEST(test_buf_alloc_best_fit);
    RUN_TEST(test_buf_pool_overflow_frees);
    RUN_TEST(test_img_add_basic);
    RUN_TEST(test_img_add_advances_cursor);
    RUN_TEST(test_img_add_iterm2_advances_below);
    RUN_TEST(test_img_find_at);
    RUN_TEST(test_img_replace);
    RUN_TEST(test_img_add_named_replace_bumps_version);
    RUN_TEST(test_img_evict_oldest_first);
    RUN_TEST(test_img_scroll_cull);
    RUN_TEST(test_img_clear_display_rows);
    RUN_TEST(test_img_clear_all);
    RUN_TEST(test_img_clear_preserves_background_layer);
    RUN_TEST(test_img_get_returns_images);
    RUN_TEST(test_img_get_empty);
    RUN_TEST(test_img_add_placement_basic);
    RUN_TEST(test_img_add_placement_dedup);
    RUN_TEST(test_img_add_placement_unknown_image);
    RUN_TEST(test_img_get_placements);
    RUN_TEST(test_img_clear_removes_placements);
    RUN_TEST(test_img_scroll_cull_removes_placements);
    RUN_TEST(test_named_image_scroll_culls_by_placement);
    RUN_TEST(test_named_image_clear_rows_by_placement);
    RUN_TEST(test_img_kind_is_set_at_creation);
    RUN_TEST(test_img_id_spaces_are_separate);
    RUN_TEST(test_img_evict_takes_the_oldest);
    RUN_TEST(test_img_placement_damages_rows);
    RUN_TEST(test_img_buffer_dims_are_the_buffers);
    RUN_TEST(test_img_handle_is_stable);

    TEST_SUMMARY();
}
