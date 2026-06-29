/*
 * test_cfr_sgr — regression test for SGR background/foreground colour
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

#include <coffer/coffer.h>
#include <stdlib.h>
#include <string.h>

static CfrTerm *make_term(int rows, int cols)
{
    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.rows = rows;
    cfg.cols = cols;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 6;
    CfrTerm *vt = cfr_new(&cfg);
    CfrCallbacks cb = { 0 };
    cfr_set_callbacks(vt, &cb, NULL);
    return vt;
}

static void feed(CfrTerm *vt, const char *s)
{
    cfr_input_write(vt, (const uint8_t *)s, strlen(s));
}

/* SGR 48;2;R;G;Bm should set a truecolor background. */
static void test_sgr_truecolor_bg(void)
{
    CfrTerm *vt = make_term(24, 80);
    feed(vt, "\x1b[48;2;128;128;255mX");

    const CfrCell *c = cfr_get_cell(vt, 0, 0);
    const CfrStyle *s = cfr_cell_style(vt, c);
    ASSERT_TRUE((s->color_flags & CFR_COLOR_DEFAULT_BG) == 0);
    ASSERT_EQ(s->bg_rgb, 0x008080FFu);

    cfr_free(vt);
}

/* SGR 48;5;Nm should set a 256-color background. */
static void test_sgr_256_bg(void)
{
    CfrTerm *vt = make_term(24, 80);
    feed(vt, "\x1b[48;5;196mX");

    const CfrCell *c = cfr_get_cell(vt, 0, 0);
    const CfrStyle *s = cfr_cell_style(vt, c);
    ASSERT_TRUE((s->color_flags & CFR_COLOR_DEFAULT_BG) == 0);

    cfr_free(vt);
}

/* SGR 41m should set ANSI red background. */
static void test_sgr_ansi_bg(void)
{
    CfrTerm *vt = make_term(24, 80);
    feed(vt, "\x1b[41mX");

    const CfrCell *c = cfr_get_cell(vt, 0, 0);
    const CfrStyle *s = cfr_cell_style(vt, c);
    ASSERT_TRUE((s->color_flags & CFR_COLOR_DEFAULT_BG) == 0);

    cfr_free(vt);
}

/* Combined fg+bg in a single SGR: \e[38;2;R;G;B;48;2;R;G;Bm */
static void test_sgr_combined_fg_bg(void)
{
    CfrTerm *vt = make_term(24, 80);
    feed(vt, "\x1b[38;2;255;0;0;48;2;40;40;40mX");

    const CfrCell *c = cfr_get_cell(vt, 0, 0);
    const CfrStyle *s = cfr_cell_style(vt, c);
    ASSERT_TRUE((s->color_flags & CFR_COLOR_DEFAULT_BG) == 0);
    ASSERT_EQ(s->bg_rgb, 0x00282828u);
    ASSERT_TRUE((s->color_flags & CFR_COLOR_DEFAULT_FG) == 0);
    ASSERT_EQ(s->fg_rgb, 0x00FF0000u);

    cfr_free(vt);
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
    CfrTerm *vt = make_term(24, 80);
    feed(vt, "\x1b[48;2;128;128;255;mX");

    const CfrCell *c = cfr_get_cell(vt, 0, 0);
    const CfrStyle *s = cfr_cell_style(vt, c);
    ASSERT_TRUE((s->color_flags & CFR_COLOR_DEFAULT_BG) == 0);
    ASSERT_EQ(s->bg_rgb, 0x008080FFu);

    cfr_free(vt);
}

/* Same test for foreground colour with trailing semicolon. */
static void test_sgr_trailing_semicolon_fg(void)
{
    CfrTerm *vt = make_term(24, 80);
    feed(vt, "\x1b[38;2;255;0;0;mX");

    const CfrCell *c = cfr_get_cell(vt, 0, 0);
    const CfrStyle *s = cfr_cell_style(vt, c);
    ASSERT_TRUE((s->color_flags & CFR_COLOR_DEFAULT_FG) == 0);
    ASSERT_EQ(s->fg_rgb, 0x00FF0000u);

    cfr_free(vt);
}

/* Combined fg+bg with trailing semicolon. */
static void test_sgr_combined_trailing_semicolon(void)
{
    CfrTerm *vt = make_term(24, 80);
    feed(vt, "\x1b[38;2;255;0;0;48;2;40;40;40;mX");

    const CfrCell *c = cfr_get_cell(vt, 0, 0);
    const CfrStyle *s = cfr_cell_style(vt, c);
    ASSERT_TRUE((s->color_flags & CFR_COLOR_DEFAULT_BG) == 0);
    ASSERT_EQ(s->bg_rgb, 0x00282828u);
    ASSERT_TRUE((s->color_flags & CFR_COLOR_DEFAULT_FG) == 0);
    ASSERT_EQ(s->fg_rgb, 0x00FF0000u);

    cfr_free(vt);
}

/* SGR 49 should reset background to default. */
static void test_sgr_reset_bg(void)
{
    CfrTerm *vt = make_term(24, 80);
    feed(vt, "\x1b[48;2;128;128;255mA\x1b[49mB");

    const CfrCell *a = cfr_get_cell(vt, 0, 0);
    const CfrStyle *sa = cfr_cell_style(vt, a);
    const CfrCell *b = cfr_get_cell(vt, 0, 1);
    const CfrStyle *sb = cfr_cell_style(vt, b);
    ASSERT_TRUE((sa->color_flags & CFR_COLOR_DEFAULT_BG) == 0);
    ASSERT_TRUE((sb->color_flags & CFR_COLOR_DEFAULT_BG) != 0);

    cfr_free(vt);
}

/* Multiple SGR attributes in one sequence with trailing semicolon:
 * bold + fg + bg + trailing semicolon */
static void test_sgr_multi_attr_trailing_semicolon(void)
{
    CfrTerm *vt = make_term(24, 80);
    feed(vt, "\x1b[1;38;2;255;0;0;48;2;40;40;40;mX");

    const CfrCell *c = cfr_get_cell(vt, 0, 0);
    const CfrStyle *s = cfr_cell_style(vt, c);
    ASSERT_TRUE((s->attrs & CFR_ATTR_BOLD) != 0);
    ASSERT_TRUE((s->color_flags & CFR_COLOR_DEFAULT_FG) == 0);
    ASSERT_EQ(s->fg_rgb, 0x00FF0000u);
    ASSERT_TRUE((s->color_flags & CFR_COLOR_DEFAULT_BG) == 0);
    ASSERT_EQ(s->bg_rgb, 0x00282828u);

    cfr_free(vt);
}

/* BCE (Back Color Erase): \e[K should extend the background colour to
 * the end of the line, not produce default-background cells.  ConPTY
 * on Windows emits \e[K after text with a truecolour background, so
 * erased cells must carry the active pen's style. */
static void test_bce_erase_in_line(void)
{
    CfrTerm *vt = make_term(24, 80);
    feed(vt, "\x1b[48;2;128;128;255mHi\x1b[K");

    const CfrCell *c3 = cfr_get_cell(vt, 0, 3);
    const CfrStyle *s3 = cfr_cell_style(vt, c3);
    ASSERT_TRUE((s3->color_flags & CFR_COLOR_DEFAULT_BG) == 0);
    ASSERT_EQ(s3->bg_rgb, 0x008080FFu);

    const CfrCell *c79 = cfr_get_cell(vt, 0, 79);
    const CfrStyle *s79 = cfr_cell_style(vt, c79);
    ASSERT_TRUE((s79->color_flags & CFR_COLOR_DEFAULT_BG) == 0);
    ASSERT_EQ(s79->bg_rgb, 0x008080FFu);

    cfr_free(vt);
}

/* BCE: erase-in-display mode 0 should extend background to end of
 * display.  The rows below the cursor are filled entirely. */
static void test_bce_erase_in_display(void)
{
    CfrTerm *vt = make_term(4, 10);
    feed(vt, "\x1b[48;5;196mX\x1b[J");

    /* Row 0: cell 1..9 should have the 256-col background. */
    const CfrCell *c1 = cfr_get_cell(vt, 0, 1);
    const CfrStyle *s1 = cfr_cell_style(vt, c1);
    ASSERT_TRUE((s1->color_flags & CFR_COLOR_DEFAULT_BG) == 0);

    /* Row 1: cell 0 should have the 256-col background. */
    const CfrCell *c10 = cfr_get_cell(vt, 1, 0);
    const CfrStyle *s10 = cfr_cell_style(vt, c10);
    ASSERT_TRUE((s10->color_flags & CFR_COLOR_DEFAULT_BG) == 0);

    cfr_free(vt);
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
