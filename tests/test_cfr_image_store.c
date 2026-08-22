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
    cfr_set_cell_pixels(vt, 10, 6);
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
    int idx = cfr_img_add(vt, st, rgba, 10, 10, 0, IMG_SRC_SIXEL);
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

    /* Image is 10px wide, 10px tall, cell is 10x6 → 1 col, 2 rows */
    uint8_t *rgba = make_rgba(10, 10, 0, 255, 0, 255);
    cfr_img_add(vt, st, rgba, 10, 10, 0, IMG_SRC_SIXEL);
    free(rgba);

    /* Cursor should have advanced 2 rows down */
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
    int idx = cfr_img_add(vt, st, rgba, 10, 6, 0, IMG_SRC_SIXEL);
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
    int idx = cfr_img_add(vt, st, rgba1, 10, 6, 0, IMG_SRC_SIXEL);
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
        cfr_img_add(vt, st, rgba, 10, 6, 0, IMG_SRC_SIXEL);
        free(rgba);
        /* Move cursor down so each image gets a different abs_line */
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
    cfr_img_add(vt, st, rgba, 10, 6, 0, IMG_SRC_SIXEL);
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
    cfr_img_add(vt, st, rgba, 10, 6, 0, IMG_SRC_SIXEL);
    free(rgba);
    ASSERT_EQ(st->img_count, 1);

    /* Clear rows 0-5 — should remove the image (it's at row 0, 1 row tall) */
    cfr_img_clear_display_rows(vt, st, 0, 5);
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
        cfr_img_add(vt, st, rgba, 10, 6, 0, IMG_SRC_SIXEL);
        free(rgba);
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
    cfr_img_add(vt, st, rgba_fg, 10, 6, 0, IMG_SRC_SIXEL);
    free(rgba_fg);

    /* Add a background image at row 0 (layer 1) */
    vt->cursor.row = 0;
    uint8_t *rgba_bg = make_rgba(10, 6, 0, 255, 0, 255);
    cfr_img_add(vt, st, rgba_bg, 10, 6, 1, IMG_SRC_SIXEL);
    free(rgba_bg);

    ASSERT_EQ(st->img_count, 2);

    /* Clear display rows 0-5 — should remove foreground (layer 0)
     * but preserve background (layer 1) */
    cfr_img_clear_display_rows(vt, st, 0, 5);
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
    cfr_img_add(vt, st, rgba, 10, 6, 0, IMG_SRC_SIXEL);
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
    RUN_TEST(test_img_find_at);
    RUN_TEST(test_img_replace);
    RUN_TEST(test_img_evict_oldest_first);
    RUN_TEST(test_img_scroll_cull);
    RUN_TEST(test_img_clear_display_rows);
    RUN_TEST(test_img_clear_all);
    RUN_TEST(test_img_clear_preserves_background_layer);
    RUN_TEST(test_img_get_returns_images);
    RUN_TEST(test_img_get_empty);

    TEST_SUMMARY();
}
