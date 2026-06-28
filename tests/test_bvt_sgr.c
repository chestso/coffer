/*
 * test_bvt_sgr — regression test for SGR background/foreground colour
 * handling, especially the trailing-semicolon bug exposed by ConPTY on
 * Windows.
 *
 * ConPTY re-serialises SGR sequences with a trailing semicolon
 * (e.g. \e[48;2;R;G;B;m).  The parser's param_separator creates an
 * implicit zero-valued parameter for the empty slot after the final
 * ";", which sgr_dispatch treated as SGR 0 — immediately resetting
 * all pen attributes including the background colour just set.
 */

#include "test_helpers.h"

#include <bloom-vt/bloom_vt.h>
#include <stdlib.h>
#include <string.h>

static BvtTerm *make_term(int rows, int cols)
{
    BvtConfig cfg = BVT_CONFIG_DEFAULTS;
    cfg.rows = rows;
    cfg.cols = cols;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 6;
    BvtTerm *vt = bvt_new(&cfg);
    BvtCallbacks cb = { 0 };
    bvt_set_callbacks(vt, &cb, NULL);
    return vt;
}

static void feed(BvtTerm *vt, const char *s)
{
    bvt_input_write(vt, (const uint8_t *)s, strlen(s));
}

/* SGR 48;2;R;G;Bm should set a truecolor background. */
static void test_sgr_truecolor_bg(void)
{
    BvtTerm *vt = make_term(24, 80);
    feed(vt, "\x1b[48;2;128;128;255mX");

    const BvtCell *c = bvt_get_cell(vt, 0, 0);
    const BvtStyle *s = bvt_cell_style(vt, c);
    ASSERT_TRUE((s->color_flags & BVT_COLOR_DEFAULT_BG) == 0);
    ASSERT_EQ(s->bg_rgb, 0x008080FFu);

    bvt_free(vt);
}

/* SGR 48;5;Nm should set a 256-color background. */
static void test_sgr_256_bg(void)
{
    BvtTerm *vt = make_term(24, 80);
    feed(vt, "\x1b[48;5;196mX");

    const BvtCell *c = bvt_get_cell(vt, 0, 0);
    const BvtStyle *s = bvt_cell_style(vt, c);
    ASSERT_TRUE((s->color_flags & BVT_COLOR_DEFAULT_BG) == 0);

    bvt_free(vt);
}

/* SGR 41m should set ANSI red background. */
static void test_sgr_ansi_bg(void)
{
    BvtTerm *vt = make_term(24, 80);
    feed(vt, "\x1b[41mX");

    const BvtCell *c = bvt_get_cell(vt, 0, 0);
    const BvtStyle *s = bvt_cell_style(vt, c);
    ASSERT_TRUE((s->color_flags & BVT_COLOR_DEFAULT_BG) == 0);

    bvt_free(vt);
}

/* Combined fg+bg in a single SGR: \e[38;2;R;G;B;48;2;R;G;Bm */
static void test_sgr_combined_fg_bg(void)
{
    BvtTerm *vt = make_term(24, 80);
    feed(vt, "\x1b[38;2;255;0;0;48;2;40;40;40mX");

    const BvtCell *c = bvt_get_cell(vt, 0, 0);
    const BvtStyle *s = bvt_cell_style(vt, c);
    ASSERT_TRUE((s->color_flags & BVT_COLOR_DEFAULT_BG) == 0);
    ASSERT_EQ(s->bg_rgb, 0x00282828u);
    ASSERT_TRUE((s->color_flags & BVT_COLOR_DEFAULT_FG) == 0);
    ASSERT_EQ(s->fg_rgb, 0x00FF0000u);

    bvt_free(vt);
}

/* KEY REGRESSION TEST: Trailing semicolon before 'm'.
 *
 * ConPTY on Windows re-serialises SGR sequences and adds a trailing
 * semicolon: \e[48;2;128;128;255;m instead of \e[48;2;128;128;255m.
 * The trailing semicolon creates an implicit zero-valued parameter,
 * which sgr_dispatch was treating as SGR 0 (full pen reset) — wiping
 * the background colour that was just set.
 */
static void test_sgr_trailing_semicolon_bg(void)
{
    BvtTerm *vt = make_term(24, 80);
    feed(vt, "\x1b[48;2;128;128;255;mX");

    const BvtCell *c = bvt_get_cell(vt, 0, 0);
    const BvtStyle *s = bvt_cell_style(vt, c);
    ASSERT_TRUE((s->color_flags & BVT_COLOR_DEFAULT_BG) == 0);
    ASSERT_EQ(s->bg_rgb, 0x008080FFu);

    bvt_free(vt);
}

/* Same test for foreground colour with trailing semicolon. */
static void test_sgr_trailing_semicolon_fg(void)
{
    BvtTerm *vt = make_term(24, 80);
    feed(vt, "\x1b[38;2;255;0;0;mX");

    const BvtCell *c = bvt_get_cell(vt, 0, 0);
    const BvtStyle *s = bvt_cell_style(vt, c);
    ASSERT_TRUE((s->color_flags & BVT_COLOR_DEFAULT_FG) == 0);
    ASSERT_EQ(s->fg_rgb, 0x00FF0000u);

    bvt_free(vt);
}

/* Combined fg+bg with trailing semicolon. */
static void test_sgr_combined_trailing_semicolon(void)
{
    BvtTerm *vt = make_term(24, 80);
    feed(vt, "\x1b[38;2;255;0;0;48;2;40;40;40;mX");

    const BvtCell *c = bvt_get_cell(vt, 0, 0);
    const BvtStyle *s = bvt_cell_style(vt, c);
    ASSERT_TRUE((s->color_flags & BVT_COLOR_DEFAULT_BG) == 0);
    ASSERT_EQ(s->bg_rgb, 0x00282828u);
    ASSERT_TRUE((s->color_flags & BVT_COLOR_DEFAULT_FG) == 0);
    ASSERT_EQ(s->fg_rgb, 0x00FF0000u);

    bvt_free(vt);
}

/* SGR 49 should reset background to default. */
static void test_sgr_reset_bg(void)
{
    BvtTerm *vt = make_term(24, 80);
    feed(vt, "\x1b[48;2;128;128;255mA\x1b[49mB");

    const BvtCell *a = bvt_get_cell(vt, 0, 0);
    const BvtStyle *sa = bvt_cell_style(vt, a);
    const BvtCell *b = bvt_get_cell(vt, 0, 1);
    const BvtStyle *sb = bvt_cell_style(vt, b);
    ASSERT_TRUE((sa->color_flags & BVT_COLOR_DEFAULT_BG) == 0);
    ASSERT_TRUE((sb->color_flags & BVT_COLOR_DEFAULT_BG) != 0);

    bvt_free(vt);
}

/* Multiple SGR attributes in one sequence with trailing semicolon:
 * bold + fg + bg + trailing semicolon */
static void test_sgr_multi_attr_trailing_semicolon(void)
{
    BvtTerm *vt = make_term(24, 80);
    feed(vt, "\x1b[1;38;2;255;0;0;48;2;40;40;40;mX");

    const BvtCell *c = bvt_get_cell(vt, 0, 0);
    const BvtStyle *s = bvt_cell_style(vt, c);
    ASSERT_TRUE((s->attrs & BVT_ATTR_BOLD) != 0);
    ASSERT_TRUE((s->color_flags & BVT_COLOR_DEFAULT_FG) == 0);
    ASSERT_EQ(s->fg_rgb, 0x00FF0000u);
    ASSERT_TRUE((s->color_flags & BVT_COLOR_DEFAULT_BG) == 0);
    ASSERT_EQ(s->bg_rgb, 0x00282828u);

    bvt_free(vt);
}

/* BCE (Back Color Erase): \e[K should extend the background colour to
 * the end of the line, not produce default-background cells.  ConPTY
 * on Windows emits \e[K after text with a truecolour background, so
 * erased cells must carry the active pen's style. */
static void test_bce_erase_in_line(void)
{
    BvtTerm *vt = make_term(24, 80);
    feed(vt, "\x1b[48;2;128;128;255mHi\x1b[K");

    const BvtCell *c3 = bvt_get_cell(vt, 0, 3);
    const BvtStyle *s3 = bvt_cell_style(vt, c3);
    ASSERT_TRUE((s3->color_flags & BVT_COLOR_DEFAULT_BG) == 0);
    ASSERT_EQ(s3->bg_rgb, 0x008080FFu);

    const BvtCell *c79 = bvt_get_cell(vt, 0, 79);
    const BvtStyle *s79 = bvt_cell_style(vt, c79);
    ASSERT_TRUE((s79->color_flags & BVT_COLOR_DEFAULT_BG) == 0);
    ASSERT_EQ(s79->bg_rgb, 0x008080FFu);

    bvt_free(vt);
}

/* BCE: erase-in-display mode 0 should extend background to end of
 * display.  The rows below the cursor are filled entirely. */
static void test_bce_erase_in_display(void)
{
    BvtTerm *vt = make_term(4, 10);
    feed(vt, "\x1b[48;5;196mX\x1b[J");

    /* Row 0: cell 1..9 should have the 256-col background. */
    const BvtCell *c1 = bvt_get_cell(vt, 0, 1);
    const BvtStyle *s1 = bvt_cell_style(vt, c1);
    ASSERT_TRUE((s1->color_flags & BVT_COLOR_DEFAULT_BG) == 0);

    /* Row 1: cell 0 should have the 256-col background. */
    const BvtCell *c10 = bvt_get_cell(vt, 1, 0);
    const BvtStyle *s10 = bvt_cell_style(vt, c10);
    ASSERT_TRUE((s10->color_flags & BVT_COLOR_DEFAULT_BG) == 0);

    bvt_free(vt);
}

int main(int argc, char **argv)
{
    test_parse_args(argc, argv);

    RUN_TEST(test_sgr_truecolor_bg);
    RUN_TEST(test_sgr_256_bg);
    RUN_TEST(test_sgr_ansi_bg);
    RUN_TEST(test_sgr_combined_fg_bg);
    RUN_TEST(test_sgr_trailing_semicolon_bg);
    RUN_TEST(test_sgr_trailing_semicolon_fg);
    RUN_TEST(test_sgr_combined_trailing_semicolon);
    RUN_TEST(test_sgr_reset_bg);
    RUN_TEST(test_sgr_multi_attr_trailing_semicolon);
    RUN_TEST(test_bce_erase_in_line);
    RUN_TEST(test_bce_erase_in_display);

    TEST_SUMMARY();
}
