/* tests/test_cfr_sixel.c — coffer sixel decode + store + placement */

#include "coffer_internal.h"
#include "test_helpers.h"
#include <coffer/coffer.h>

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

static CfrTerm *make_term(int rows, int cols)
{
    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.rows = rows;
    cfg.cols = cols;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 6;
    CfrTerm *vt = cfr_new(&cfg);
    CfrCallbacks cb = { 0 };
    cb.output = on_output;
    cfr_set_callbacks(vt, &cb, NULL);
    /* Cell size so the engine can map pixels → rows. 10x6 → a single
     * 6px band is exactly one cell tall. */
    cfr_set_cell_pixels(vt, 10, 6);
    g_out_len = 0;
    g_out[0] = '\0';
    return vt;
}

static void feed(CfrTerm *vt, const char *s)
{
    cfr_input_write(vt, (const uint8_t *)s, strlen(s));
}

/* Pixel accessors over a returned CfrSixel. */
static const uint8_t *px(const CfrSixel *s, int x, int y)
{
    return s->rgba + ((size_t)y * s->width_px + x) * 4;
}

/* ------------------------------------------------------------------ */

/* A 2px-wide red square (top two pixels of the band set). Color 1 is
 * defined as RGB red; 'B' = 0x42 → bits 0b000011 (top two rows). */
static void test_decode_basic(void)
{
    CfrTerm *vt = make_term(24, 80);
    feed(vt, "\x1bPq#1;2;100;0;0#1BB\x1b\\");

    int n = -1;
    const CfrSixel *s = cfr_get_sixels(vt, &n);
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
    const CfrCell *c = cfr_get_cell(vt, 0, 0);
    ASSERT_NOT_NULL(c);
    ASSERT_EQ(c->cp, 0u);

    cfr_free(vt);
}

/* Unset pixels in a band are transparent (alpha 0). */
static void test_transparency(void)
{
    CfrTerm *vt = make_term(24, 80);
    /* '@' = 0x40 → bit 0 only (top pixel). */
    feed(vt, "\x1bPq#1;2;0;100;0#1@\x1b\\");

    int n = -1;
    const CfrSixel *s = cfr_get_sixels(vt, &n);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(px(&s[0], 0, 0)[3], 255); /* top set → opaque */
    ASSERT_EQ(px(&s[0], 0, 1)[3], 0);   /* below unset → transparent */
    ASSERT_EQ(px(&s[0], 0, 5)[3], 0);
    cfr_free(vt);
}

/* RLE: !4~ draws four full columns. */
static void test_rle(void)
{
    CfrTerm *vt = make_term(24, 80);
    feed(vt, "\x1bPq#1;2;0;100;0#1!4~\x1b\\");

    int n = -1;
    const CfrSixel *s = cfr_get_sixels(vt, &n);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(s[0].width_px, 4);
    ASSERT_EQ(s[0].height_px, 6);
    /* All pixels set (green) and opaque. */
    ASSERT_EQ(px(&s[0], 3, 5)[1], 255); /* G */
    ASSERT_EQ(px(&s[0], 3, 5)[3], 255);
    cfr_free(vt);
}

/* DEC HLS: hue 0° is blue (not red). Validates the +240° remap. */
static void test_hls_hue(void)
{
    CfrTerm *vt = make_term(24, 80);
    /* #2;1;0;50;100 → HLS hue=0 (blue), lum=50, sat=100. */
    feed(vt, "\x1bPq#2;1;0;50;100#2~\x1b\\");

    int n = -1;
    const CfrSixel *s = cfr_get_sixels(vt, &n);
    ASSERT_EQ(n, 1);
    const uint8_t *p = px(&s[0], 0, 0);
    ASSERT_EQ(p[0], 0);   /* R */
    ASSERT_EQ(p[1], 0);   /* G */
    ASSERT_EQ(p[2], 255); /* B — hue 0 is blue */
    cfr_free(vt);
}

/* Cursor advances below the image by ceil(height/cell_h) rows. */
static void test_cursor_advance(void)
{
    CfrTerm *vt = make_term(24, 80);
    cfr_set_cell_pixels(vt, 10, 6);
    /* Two bands tall: ~ then graphics-NL then ~ → 12px = 2 cells. */
    feed(vt, "\x1bPq#1;2;0;100;0#1~-#1~\x1b\\");

    int n = -1;
    const CfrSixel *s = cfr_get_sixels(vt, &n);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(s[0].height_px, 12);

    CfrCursor cur = cfr_get_cursor(vt);
    ASSERT_EQ(cur.row, 2); /* advanced two rows */
    ASSERT_EQ(cur.col, 0);
    cfr_free(vt);
}

/* DECSDM (mode 80) draws in place: cursor does not move, and a second
 * image at the same anchor replaces the first (animation), reusing the
 * buffer and bumping the version. */
static void test_inplace_animation(void)
{
    CfrTerm *vt = make_term(24, 80);
    feed(vt, "\x1b[?80h"); /* in-place mode */
    ASSERT_TRUE(cfr_get_mode(vt, CFR_MODE_SIXEL_SCROLLING));

    feed(vt, "\x1bPq#1;2;100;0;0#1~\x1b\\");
    int n = -1;
    const CfrSixel *s = cfr_get_sixels(vt, &n);
    ASSERT_EQ(n, 1);
    uint64_t id0 = s[0].id;
    uint32_t v0 = s[0].version;
    const uint8_t *buf0 = s[0].rgba;

    CfrCursor cur = cfr_get_cursor(vt);
    ASSERT_EQ(cur.row, 0); /* in place — no advance */
    ASSERT_EQ(cur.col, 0);

    /* Second frame, same size, same anchor → coalesce. */
    feed(vt, "\x1bPq#1;2;0;100;0#1~\x1b\\");
    s = cfr_get_sixels(vt, &n);
    ASSERT_EQ(n, 1);                 /* not two stacked images */
    ASSERT_EQ(s[0].id, id0);         /* stable id */
    ASSERT_EQ(s[0].version, v0 + 1); /* version bumped */
    ASSERT_TRUE(s[0].rgba == buf0);  /* buffer reused in place */
    /* New color took effect (green). */
    ASSERT_EQ(px(&s[0], 0, 0)[1], 255);
    cfr_free(vt);
}

/* Scrolling carries an image up and into scrollback (negative row). */
static void test_scroll_into_scrollback(void)
{
    CfrTerm *vt = make_term(4, 20);
    cfr_set_cell_pixels(vt, 10, 6);
    feed(vt, "\x1bPq#1;2;100;0;0#1~\x1b\\"); /* image anchored at row 0 */

    int n = -1;
    const CfrSixel *s = cfr_get_sixels(vt, &n);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(s[0].row, 0);

    /* Drive the cursor to the last row and scroll several times. */
    feed(vt, "\x1b[4;1H");  /* CUP to bottom-left */
    feed(vt, "\n\n\n\n\n"); /* five linefeeds → five scrolls */

    s = cfr_get_sixels(vt, &n);
    ASSERT_EQ(n, 1);
    ASSERT_TRUE(s[0].row < 0); /* now in scrollback */
    cfr_free(vt);
}

/* Erase-display (ED 2) removes overlapping images. */
static void test_clear_on_ed(void)
{
    CfrTerm *vt = make_term(24, 80);
    feed(vt, "\x1bPq#1;2;100;0;0#1~\x1b\\");
    int n = -1;
    cfr_get_sixels(vt, &n);
    ASSERT_EQ(n, 1);

    feed(vt, "\x1b[2J"); /* erase whole display */
    cfr_get_sixels(vt, &n);
    ASSERT_EQ(n, 0);
    cfr_free(vt);
}

/* RIS clears all images. */
static void test_clear_on_ris(void)
{
    CfrTerm *vt = make_term(24, 80);
    feed(vt, "\x1bPq#1;2;100;0;0#1~\x1b\\");
    int n = -1;
    cfr_get_sixels(vt, &n);
    ASSERT_EQ(n, 1);

    feed(vt, "\x1b"
             "c"); /* RIS */
    cfr_get_sixels(vt, &n);
    ASSERT_EQ(n, 0);
    cfr_free(vt);
}

/* Entering altscreen clears primary-grid images. */
static void test_clear_on_altscreen(void)
{
    CfrTerm *vt = make_term(24, 80);
    feed(vt, "\x1bPq#1;2;100;0;0#1~\x1b\\");
    int n = -1;
    cfr_get_sixels(vt, &n);
    ASSERT_EQ(n, 1);

    feed(vt, "\x1b[?1049h"); /* enter altscreen */
    cfr_get_sixels(vt, &n);
    ASSERT_EQ(n, 0);
    cfr_free(vt);
}

/* Resize drops images (reflow rebuilds the grid). */
static void test_clear_on_resize(void)
{
    CfrTerm *vt = make_term(24, 80);
    feed(vt, "\x1bPq#1;2;100;0;0#1~\x1b\\");
    int n = -1;
    cfr_get_sixels(vt, &n);
    ASSERT_EQ(n, 1);

    cfr_resize(vt, 30, 100);
    cfr_get_sixels(vt, &n);
    ASSERT_EQ(n, 0);
    cfr_free(vt);
}

/* Capability advertisement: DA1, modes, XTSMGRAPHICS. */
static void test_capabilities(void)
{
    CfrTerm *vt = make_term(24, 80);

    g_out_len = 0;
    feed(vt, "\x1b[c");
    ASSERT_NOT_NULL(strstr(g_out, ";4;")); /* sixel bit in DA1 */

    feed(vt, "\x1b[?1070h");
    ASSERT_TRUE(cfr_get_mode(vt, CFR_MODE_SIXEL_PRIVATE_REGS));
    feed(vt, "\x1b[?8452h");
    ASSERT_TRUE(cfr_get_mode(vt, CFR_MODE_SIXEL_CURSOR_RIGHT));

    /* XTSMGRAPHICS: color register count. */
    g_out_len = 0;
    feed(vt, "\x1b[?1;1;0S");
    ASSERT_STR_EQ(g_out, "\x1b[?1;0;256S");

    /* XTSMGRAPHICS: geometry (80*10 x 24*6). */
    g_out_len = 0;
    feed(vt, "\x1b[?2;1;0S");
    ASSERT_STR_EQ(g_out, "\x1b[?2;0;800;144S");

    cfr_free(vt);
}

/* Hammer the animation path: many same-anchor frames must stay a single
 * image with a reused buffer and a monotonic version. (Run under ASan in
 * the sanitized build to catch any use-after-free in the reuse path.) */
static void test_animation_stress(void)
{
    CfrTerm *vt = make_term(24, 80);
    feed(vt, "\x1b[?80h"); /* in-place */

    feed(vt, "\x1bPq#1;2;100;0;0#1~\x1b\\");
    int n = -1;
    const CfrSixel *s = cfr_get_sixels(vt, &n);
    ASSERT_EQ(n, 1);
    uint64_t id0 = s[0].id;
    const uint8_t *buf0 = s[0].rgba;

    for (int i = 0; i < 500; ++i)
        feed(vt, "\x1bPq#1;2;0;100;0#1~\x1b\\");

    s = cfr_get_sixels(vt, &n);
    ASSERT_EQ(n, 1);                /* never piles up */
    ASSERT_EQ(s[0].id, id0);        /* same image */
    ASSERT_EQ(s[0].version, 501u);  /* 1 + 500 replacements */
    ASSERT_TRUE(s[0].rgba == buf0); /* one buffer recycled throughout */
    cfr_free(vt);
}

/* Distinct-anchor images are capped; the store never grows without bound. */
static void test_image_count_cap(void)
{
    CfrTerm *vt = make_term(24, 80);
    cfr_set_cell_pixels(vt, 10, 6);
    /* Each sixel advances the cursor one row (scrolling), so every image
     * gets a fresh anchor. Feed well past the cap. */
    for (int i = 0; i < 400; ++i)
        feed(vt, "\x1bPq#1;2;100;0;0#1~\x1b\\");

    int n = -1;
    cfr_get_sixels(vt, &n);
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(n <= 256); /* SX_MAX_IMAGES */
    cfr_free(vt);
}

/* A plain CSI S still scrolls (no '?' intermediate → not XTSMGRAPHICS). */
static void test_csi_s_still_scrolls(void)
{
    CfrTerm *vt = make_term(5, 10);
    feed(vt, "A\r\nB\r\nC"); /* rows 0,1,2 */
    feed(vt, "\x1b[2S");     /* scroll up 2 */
    const CfrCell *c = cfr_get_cell(vt, 0, 0);
    ASSERT_NOT_NULL(c);
    ASSERT_EQ(c->cp, (uint32_t)'C'); /* C moved to row 0 */
    cfr_free(vt);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);
    printf("test_cfr_sixel\n");
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
