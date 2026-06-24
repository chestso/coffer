/* tests/test_bvt_sixel.c — bloom-vt sixel decode + store + placement */

#include "bloom_vt_internal.h"
#include "test_helpers.h"
#include <bloom-vt/bloom_vt.h>

#include <stdio.h>
#include <string.h>

static char g_out[1024];
static size_t g_out_len;

static void on_output(const uint8_t *bytes, size_t len, void *u)
{
    (void)u;
    if (g_out_len + len >= sizeof(g_out))
        return;
    memcpy(g_out + g_out_len, bytes, len);
    g_out_len += len;
    g_out[g_out_len] = '\0';
}

static BvtTerm *make_term(int rows, int cols)
{
    BvtConfig cfg = BVT_CONFIG_DEFAULTS;
    cfg.rows = rows;
    cfg.cols = cols;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 6;
    BvtTerm *vt = bvt_new(&cfg);
    BvtCallbacks cb = { 0 };
    cb.output = on_output;
    bvt_set_callbacks(vt, &cb, NULL);
    /* Cell size so the engine can map pixels → rows. 10x6 → a single
     * 6px band is exactly one cell tall. */
    bvt_set_cell_pixels(vt, 10, 6);
    g_out_len = 0;
    g_out[0] = '\0';
    return vt;
}

static void feed(BvtTerm *vt, const char *s)
{
    bvt_input_write(vt, (const uint8_t *)s, strlen(s));
}

/* Pixel accessors over a returned BvtSixel. */
static const uint8_t *px(const BvtSixel *s, int x, int y)
{
    return s->rgba + ((size_t)y * s->width_px + x) * 4;
}

/* ------------------------------------------------------------------ */

/* A 2px-wide red square (top two pixels of the band set). Color 1 is
 * defined as RGB red; 'B' = 0x42 → bits 0b000011 (top two rows). */
static void test_decode_basic(void)
{
    BvtTerm *vt = make_term(24, 80);
    feed(vt, "\x1bPq#1;2;100;0;0#1BB\x1b\\");

    int n = -1;
    const BvtSixel *s = bvt_get_sixels(vt, &n);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(s[0].width_px, 2);
    ASSERT_EQ(s[0].height_px, 6); /* a band is always 6px tall */
    ASSERT_EQ(s[0].layer, 0);
    ASSERT_EQ(s[0].row, 0);
    ASSERT_EQ(s[0].col, 0);

    const uint8_t *p = px(&s[0], 0, 0);
    ASSERT_EQ(p[0], 255); /* R */
    ASSERT_EQ(p[1], 0);
    ASSERT_EQ(p[2], 0);
    ASSERT_EQ(p[3], 255); /* set pixel opaque */

    /* The sixel body must not leak into the text grid. */
    const BvtCell *c = bvt_get_cell(vt, 0, 0);
    ASSERT_NOT_NULL(c);
    ASSERT_EQ(c->cp, 0u);

    bvt_free(vt);
}

/* Unset pixels in a band are transparent (alpha 0). */
static void test_transparency(void)
{
    BvtTerm *vt = make_term(24, 80);
    /* '@' = 0x40 → bit 0 only (top pixel). */
    feed(vt, "\x1bPq#1;2;0;100;0#1@\x1b\\");

    int n = -1;
    const BvtSixel *s = bvt_get_sixels(vt, &n);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(px(&s[0], 0, 0)[3], 255); /* top set → opaque */
    ASSERT_EQ(px(&s[0], 0, 1)[3], 0);   /* below unset → transparent */
    ASSERT_EQ(px(&s[0], 0, 5)[3], 0);
    bvt_free(vt);
}

/* RLE: !4~ draws four full columns. */
static void test_rle(void)
{
    BvtTerm *vt = make_term(24, 80);
    feed(vt, "\x1bPq#1;2;0;100;0#1!4~\x1b\\");

    int n = -1;
    const BvtSixel *s = bvt_get_sixels(vt, &n);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(s[0].width_px, 4);
    ASSERT_EQ(s[0].height_px, 6);
    /* All pixels set (green) and opaque. */
    ASSERT_EQ(px(&s[0], 3, 5)[1], 255); /* G */
    ASSERT_EQ(px(&s[0], 3, 5)[3], 255);
    bvt_free(vt);
}

/* DEC HLS: hue 0° is blue (not red). Validates the +240° remap. */
static void test_hls_hue(void)
{
    BvtTerm *vt = make_term(24, 80);
    /* #2;1;0;50;100 → HLS hue=0 (blue), lum=50, sat=100. */
    feed(vt, "\x1bPq#2;1;0;50;100#2~\x1b\\");

    int n = -1;
    const BvtSixel *s = bvt_get_sixels(vt, &n);
    ASSERT_EQ(n, 1);
    const uint8_t *p = px(&s[0], 0, 0);
    ASSERT_EQ(p[0], 0);   /* R */
    ASSERT_EQ(p[1], 0);   /* G */
    ASSERT_EQ(p[2], 255); /* B — hue 0 is blue */
    bvt_free(vt);
}

/* Cursor advances below the image by ceil(height/cell_h) rows. */
static void test_cursor_advance(void)
{
    BvtTerm *vt = make_term(24, 80);
    bvt_set_cell_pixels(vt, 10, 6);
    /* Two bands tall: ~ then graphics-NL then ~ → 12px = 2 cells. */
    feed(vt, "\x1bPq#1;2;0;100;0#1~-#1~\x1b\\");

    int n = -1;
    const BvtSixel *s = bvt_get_sixels(vt, &n);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(s[0].height_px, 12);

    BvtCursor cur = bvt_get_cursor(vt);
    ASSERT_EQ(cur.row, 2); /* advanced two rows */
    ASSERT_EQ(cur.col, 0);
    bvt_free(vt);
}

/* DECSDM (mode 80) draws in place: cursor does not move, and a second
 * image at the same anchor replaces the first (animation), reusing the
 * buffer and bumping the version. */
static void test_inplace_animation(void)
{
    BvtTerm *vt = make_term(24, 80);
    feed(vt, "\x1b[?80h"); /* in-place mode */
    ASSERT_TRUE(bvt_get_mode(vt, BVT_MODE_SIXEL_SCROLLING));

    feed(vt, "\x1bPq#1;2;100;0;0#1~\x1b\\");
    int n = -1;
    const BvtSixel *s = bvt_get_sixels(vt, &n);
    ASSERT_EQ(n, 1);
    uint64_t id0 = s[0].id;
    uint32_t v0 = s[0].version;
    const uint8_t *buf0 = s[0].rgba;

    BvtCursor cur = bvt_get_cursor(vt);
    ASSERT_EQ(cur.row, 0); /* in place — no advance */
    ASSERT_EQ(cur.col, 0);

    /* Second frame, same size, same anchor → coalesce. */
    feed(vt, "\x1bPq#1;2;0;100;0#1~\x1b\\");
    s = bvt_get_sixels(vt, &n);
    ASSERT_EQ(n, 1);                 /* not two stacked images */
    ASSERT_EQ(s[0].id, id0);         /* stable id */
    ASSERT_EQ(s[0].version, v0 + 1); /* version bumped */
    ASSERT_TRUE(s[0].rgba == buf0);  /* buffer reused in place */
    /* New color took effect (green). */
    ASSERT_EQ(px(&s[0], 0, 0)[1], 255);
    bvt_free(vt);
}

/* Scrolling carries an image up and into scrollback (negative row). */
static void test_scroll_into_scrollback(void)
{
    BvtTerm *vt = make_term(4, 20);
    bvt_set_cell_pixels(vt, 10, 6);
    feed(vt, "\x1bPq#1;2;100;0;0#1~\x1b\\"); /* image anchored at row 0 */

    int n = -1;
    const BvtSixel *s = bvt_get_sixels(vt, &n);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(s[0].row, 0);

    /* Drive the cursor to the last row and scroll several times. */
    feed(vt, "\x1b[4;1H");  /* CUP to bottom-left */
    feed(vt, "\n\n\n\n\n"); /* five linefeeds → five scrolls */

    s = bvt_get_sixels(vt, &n);
    ASSERT_EQ(n, 1);
    ASSERT_TRUE(s[0].row < 0); /* now in scrollback */
    bvt_free(vt);
}

/* Erase-display (ED 2) removes overlapping images. */
static void test_clear_on_ed(void)
{
    BvtTerm *vt = make_term(24, 80);
    feed(vt, "\x1bPq#1;2;100;0;0#1~\x1b\\");
    int n = -1;
    bvt_get_sixels(vt, &n);
    ASSERT_EQ(n, 1);

    feed(vt, "\x1b[2J"); /* erase whole display */
    bvt_get_sixels(vt, &n);
    ASSERT_EQ(n, 0);
    bvt_free(vt);
}

/* RIS clears all images. */
static void test_clear_on_ris(void)
{
    BvtTerm *vt = make_term(24, 80);
    feed(vt, "\x1bPq#1;2;100;0;0#1~\x1b\\");
    int n = -1;
    bvt_get_sixels(vt, &n);
    ASSERT_EQ(n, 1);

    feed(vt, "\x1b"
             "c"); /* RIS */
    bvt_get_sixels(vt, &n);
    ASSERT_EQ(n, 0);
    bvt_free(vt);
}

/* Entering altscreen clears primary-grid images. */
static void test_clear_on_altscreen(void)
{
    BvtTerm *vt = make_term(24, 80);
    feed(vt, "\x1bPq#1;2;100;0;0#1~\x1b\\");
    int n = -1;
    bvt_get_sixels(vt, &n);
    ASSERT_EQ(n, 1);

    feed(vt, "\x1b[?1049h"); /* enter altscreen */
    bvt_get_sixels(vt, &n);
    ASSERT_EQ(n, 0);
    bvt_free(vt);
}

/* Resize drops images (reflow rebuilds the grid). */
static void test_clear_on_resize(void)
{
    BvtTerm *vt = make_term(24, 80);
    feed(vt, "\x1bPq#1;2;100;0;0#1~\x1b\\");
    int n = -1;
    bvt_get_sixels(vt, &n);
    ASSERT_EQ(n, 1);

    bvt_resize(vt, 30, 100);
    bvt_get_sixels(vt, &n);
    ASSERT_EQ(n, 0);
    bvt_free(vt);
}

/* Capability advertisement: DA1, modes, XTSMGRAPHICS. */
static void test_capabilities(void)
{
    BvtTerm *vt = make_term(24, 80);

    g_out_len = 0;
    feed(vt, "\x1b[c");
    ASSERT_NOT_NULL(strstr(g_out, ";4;")); /* sixel bit in DA1 */

    feed(vt, "\x1b[?1070h");
    ASSERT_TRUE(bvt_get_mode(vt, BVT_MODE_SIXEL_PRIVATE_REGS));
    feed(vt, "\x1b[?8452h");
    ASSERT_TRUE(bvt_get_mode(vt, BVT_MODE_SIXEL_CURSOR_RIGHT));

    /* XTSMGRAPHICS: color register count. */
    g_out_len = 0;
    feed(vt, "\x1b[?1;1;0S");
    ASSERT_STR_EQ(g_out, "\x1b[?1;0;256S");

    /* XTSMGRAPHICS: geometry (80*10 x 24*6). */
    g_out_len = 0;
    feed(vt, "\x1b[?2;1;0S");
    ASSERT_STR_EQ(g_out, "\x1b[?2;0;800;144S");

    bvt_free(vt);
}

/* Hammer the animation path: many same-anchor frames must stay a single
 * image with a reused buffer and a monotonic version. (Run under ASan in
 * the sanitized build to catch any use-after-free in the reuse path.) */
static void test_animation_stress(void)
{
    BvtTerm *vt = make_term(24, 80);
    feed(vt, "\x1b[?80h"); /* in-place */

    feed(vt, "\x1bPq#1;2;100;0;0#1~\x1b\\");
    int n = -1;
    const BvtSixel *s = bvt_get_sixels(vt, &n);
    ASSERT_EQ(n, 1);
    uint64_t id0 = s[0].id;
    const uint8_t *buf0 = s[0].rgba;

    for (int i = 0; i < 500; ++i)
        feed(vt, "\x1bPq#1;2;0;100;0#1~\x1b\\");

    s = bvt_get_sixels(vt, &n);
    ASSERT_EQ(n, 1);                /* never piles up */
    ASSERT_EQ(s[0].id, id0);        /* same image */
    ASSERT_EQ(s[0].version, 501u);  /* 1 + 500 replacements */
    ASSERT_TRUE(s[0].rgba == buf0); /* one buffer recycled throughout */
    bvt_free(vt);
}

/* Distinct-anchor images are capped; the store never grows without bound. */
static void test_image_count_cap(void)
{
    BvtTerm *vt = make_term(24, 80);
    bvt_set_cell_pixels(vt, 10, 6);
    /* Each sixel advances the cursor one row (scrolling), so every image
     * gets a fresh anchor. Feed well past the cap. */
    for (int i = 0; i < 400; ++i)
        feed(vt, "\x1bPq#1;2;100;0;0#1~\x1b\\");

    int n = -1;
    bvt_get_sixels(vt, &n);
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(n <= 256); /* SX_MAX_IMAGES */
    bvt_free(vt);
}

/* A plain CSI S still scrolls (no '?' intermediate → not XTSMGRAPHICS). */
static void test_csi_s_still_scrolls(void)
{
    BvtTerm *vt = make_term(5, 10);
    feed(vt, "A\r\nB\r\nC"); /* rows 0,1,2 */
    feed(vt, "\x1b[2S");     /* scroll up 2 */
    const BvtCell *c = bvt_get_cell(vt, 0, 0);
    ASSERT_NOT_NULL(c);
    ASSERT_EQ(c->cp, (uint32_t)'C'); /* C moved to row 0 */
    bvt_free(vt);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);
    printf("test_bvt_sixel\n");
    RUN_TEST(test_decode_basic);
    RUN_TEST(test_transparency);
    RUN_TEST(test_rle);
    RUN_TEST(test_hls_hue);
    RUN_TEST(test_cursor_advance);
    RUN_TEST(test_inplace_animation);
    RUN_TEST(test_scroll_into_scrollback);
    RUN_TEST(test_clear_on_ed);
    RUN_TEST(test_clear_on_ris);
    RUN_TEST(test_clear_on_altscreen);
    RUN_TEST(test_clear_on_resize);
    RUN_TEST(test_animation_stress);
    RUN_TEST(test_image_count_cap);
    RUN_TEST(test_capabilities);
    RUN_TEST(test_csi_s_still_scrolls);
    TEST_SUMMARY();
}
