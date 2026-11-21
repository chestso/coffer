/*
 * test_cfr_sgr — regression test for SGR attribute and colour handling,
 * especially the trailing-semicolon bug exposed by ConPTY on Windows.
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

    const CfrStyle *s = cfr_cell_style(vt, 0, 0);
    ASSERT_EQ(s->bg.kind, CFR_COLOR_KIND_RGB);
    ASSERT_EQ(s->bg.rgb, 0x008080FFu);
    ASSERT_EQ(cfr_style_bg_rgb(vt, s), 0x008080FFu);

    cfr_free(vt);
}

/* SGR 48;5;Nm keeps the palette index so an OSC 4 override repaints it. */
static void test_sgr_256_bg(void)
{
    CfrTerm *vt = make_term(24, 80);
    feed(vt, "\x1b[48;5;196mX");

    const CfrStyle *s = cfr_cell_style(vt, 0, 0);
    ASSERT_EQ(s->bg.kind, CFR_COLOR_KIND_INDEXED);
    ASSERT_EQ(s->bg.index, 196u);
    ASSERT_EQ(cfr_style_bg_rgb(vt, s), cfr_palette_rgb(vt, 196));

    cfr_free(vt);
}

/* SGR 41m should set ANSI red background as palette entry 1. */
static void test_sgr_ansi_bg(void)
{
    CfrTerm *vt = make_term(24, 80);
    feed(vt, "\x1b[41mX");

    const CfrStyle *s = cfr_cell_style(vt, 0, 0);
    ASSERT_EQ(s->bg.kind, CFR_COLOR_KIND_INDEXED);
    ASSERT_EQ(s->bg.index, 1u);
    ASSERT_EQ(cfr_style_bg_rgb(vt, s), cfr_palette_rgb(vt, 1));

    cfr_free(vt);
}

/* The live palette starts identical to the built-in one. */
static void test_palette_starts_builtin(void)
{
    CfrTerm *vt = make_term(24, 80);
    for (int i = 0; i < 256; i++)
        ASSERT_EQ(cfr_palette_rgb(vt, (uint8_t)i), cfr_default_palette_rgb((uint8_t)i));
    ASSERT_EQ(cfr_palette_rgb(NULL, 1), cfr_default_palette_rgb(1));
    cfr_free(vt);
}

/* Default style colors resolve through the live dynamic colors, never a
 * snapshot: the default underline follows the default foreground. */
static void test_style_color_resolution_defaults(void)
{
    CfrTerm *vt = make_term(24, 80);
    feed(vt, "X");
    const CfrStyle *s = cfr_cell_style(vt, 0, 0);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(s->fg.kind, CFR_COLOR_KIND_DEFAULT);
    ASSERT_EQ(s->bg.kind, CFR_COLOR_KIND_DEFAULT);
    ASSERT_EQ(s->ul.kind, CFR_COLOR_KIND_DEFAULT);
    ASSERT_EQ(cfr_style_fg_rgb(vt, s), cfr_default_fg_rgb());
    ASSERT_EQ(cfr_style_bg_rgb(vt, s), cfr_default_bg_rgb());
    ASSERT_EQ(cfr_style_ul_rgb(vt, s), cfr_default_fg_rgb());

    feed(vt, "\x1b]10;rgb:12/34/56\x1b\\");
    feed(vt, "\x1b]11;#abcdef\x1b\\");
    ASSERT_EQ(cfr_style_fg_rgb(vt, s), 0x123456u);
    ASSERT_EQ(cfr_style_bg_rgb(vt, s), 0xABCDEFu);
    ASSERT_EQ(cfr_style_ul_rgb(vt, s), 0x123456u);

    cfr_free(vt);
}

/* Indexed style colors resolve through the live palette. */
static void test_style_color_resolution_indexed(void)
{
    CfrTerm *vt = make_term(24, 80);
    feed(vt, "\x1b[31;44mX");
    const CfrStyle *s = cfr_cell_style(vt, 0, 0);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(cfr_style_fg_rgb(vt, s), cfr_palette_rgb(vt, 1));
    ASSERT_EQ(cfr_style_bg_rgb(vt, s), cfr_palette_rgb(vt, 4));
    cfr_free(vt);
}

/* Truecolor style colors bypass the palette. */
static void test_style_color_resolution_rgb(void)
{
    CfrTerm *vt = make_term(24, 80);
    feed(vt, "\x1b[38;2;1;2;3;48;2;4;5;6;58;2;7;8;9mX");
    const CfrStyle *s = cfr_cell_style(vt, 0, 0);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(cfr_style_fg_rgb(vt, s), 0x010203u);
    ASSERT_EQ(cfr_style_bg_rgb(vt, s), 0x040506u);
    ASSERT_EQ(cfr_style_ul_rgb(vt, s), 0x070809u);
    cfr_free(vt);
}

/* Combined fg+bg in a single SGR: \e[38;2;R;G;B;48;2;R;G;Bm */
static void test_sgr_combined_fg_bg(void)
{
    CfrTerm *vt = make_term(24, 80);
    feed(vt, "\x1b[38;2;255;0;0;48;2;40;40;40mX");

    const CfrStyle *s = cfr_cell_style(vt, 0, 0);
    ASSERT_EQ(s->bg.kind, CFR_COLOR_KIND_RGB);
    ASSERT_EQ(s->bg.rgb, 0x00282828u);
    ASSERT_EQ(s->fg.kind, CFR_COLOR_KIND_RGB);
    ASSERT_EQ(s->fg.rgb, 0x00FF0000u);

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

    const CfrStyle *s = cfr_cell_style(vt, 0, 0);
    ASSERT_EQ(s->bg.kind, CFR_COLOR_KIND_RGB);
    ASSERT_EQ(s->bg.rgb, 0x008080FFu);

    cfr_free(vt);
}

/* Same test for foreground colour with trailing semicolon. */
static void test_sgr_trailing_semicolon_fg(void)
{
    CfrTerm *vt = make_term(24, 80);
    feed(vt, "\x1b[38;2;255;0;0;mX");

    const CfrStyle *s = cfr_cell_style(vt, 0, 0);
    ASSERT_EQ(s->fg.kind, CFR_COLOR_KIND_RGB);
    ASSERT_EQ(s->fg.rgb, 0x00FF0000u);

    cfr_free(vt);
}

/* Combined fg+bg with trailing semicolon. */
static void test_sgr_combined_trailing_semicolon(void)
{
    CfrTerm *vt = make_term(24, 80);
    feed(vt, "\x1b[38;2;255;0;0;48;2;40;40;40;mX");

    const CfrStyle *s = cfr_cell_style(vt, 0, 0);
    ASSERT_EQ(s->bg.kind, CFR_COLOR_KIND_RGB);
    ASSERT_EQ(s->bg.rgb, 0x00282828u);
    ASSERT_EQ(s->fg.kind, CFR_COLOR_KIND_RGB);
    ASSERT_EQ(s->fg.rgb, 0x00FF0000u);

    cfr_free(vt);
}

/* SGR 49 should reset background to default. */
static void test_sgr_reset_bg(void)
{
    CfrTerm *vt = make_term(24, 80);
    feed(vt, "\x1b[48;2;128;128;255mA\x1b[49mB");

    const CfrStyle *sa = cfr_cell_style(vt, 0, 0);
    const CfrStyle *sb = cfr_cell_style(vt, 0, 1);
    ASSERT_EQ(sa->bg.kind, CFR_COLOR_KIND_RGB);
    ASSERT_EQ(sb->bg.kind, CFR_COLOR_KIND_DEFAULT);

    cfr_free(vt);
}

/* Multiple SGR attributes in one sequence with trailing semicolon:
 * bold + fg + bg + trailing semicolon */
static void test_sgr_multi_attr_trailing_semicolon(void)
{
    CfrTerm *vt = make_term(24, 80);
    feed(vt, "\x1b[1;38;2;255;0;0;48;2;40;40;40;mX");

    const CfrStyle *s = cfr_cell_style(vt, 0, 0);
    ASSERT_TRUE((s->attrs & CFR_ATTR_BOLD) != 0);
    ASSERT_EQ(s->fg.kind, CFR_COLOR_KIND_RGB);
    ASSERT_EQ(s->fg.rgb, 0x00FF0000u);
    ASSERT_EQ(s->bg.kind, CFR_COLOR_KIND_RGB);
    ASSERT_EQ(s->bg.rgb, 0x00282828u);

    cfr_free(vt);
}

/* SGR 21 selects the double underline and 24 clears it. It is not a bold
 * reset — 22 owns bold/dim. */
static void test_sgr_double_underline(void)
{
    CfrTerm *vt = make_term(24, 80);
    feed(vt, "\x1b[1;21mX");

    const CfrStyle *s = cfr_cell_style(vt, 0, 0);
    ASSERT_EQ(s->underline, CFR_UL_DOUBLE);
    ASSERT_TRUE((s->attrs & CFR_ATTR_BOLD) != 0);

    feed(vt, "\x1b[24mY");
    const CfrStyle *s1 = cfr_cell_style(vt, 0, 1);
    ASSERT_EQ(s1->underline, CFR_UL_NONE);
    ASSERT_TRUE((s1->attrs & CFR_ATTR_BOLD) != 0);

    cfr_free(vt);
}

/* SGR 53 raises the overline, 55 lowers it, and 0 clears everything. */
static void test_sgr_overline(void)
{
    CfrTerm *vt = make_term(24, 80);
    feed(vt, "\x1b[53mX");

    const CfrStyle *s = cfr_cell_style(vt, 0, 0);
    ASSERT_TRUE((s->attrs & CFR_ATTR_OVERLINE) != 0);
    ASSERT_TRUE((s->attrs & CFR_ATTR_STRIKETHROUGH) == 0);

    /* 55 drops the overline without touching the cell already written. */
    feed(vt, "\x1b[55mY");
    const CfrStyle *s0 = cfr_cell_style(vt, 0, 0);
    ASSERT_TRUE((s0->attrs & CFR_ATTR_OVERLINE) != 0);
    const CfrStyle *s1 = cfr_cell_style(vt, 0, 1);
    ASSERT_TRUE((s1->attrs & CFR_ATTR_OVERLINE) == 0);

    /* A reset clears it like any other attribute bit. */
    feed(vt, "\x1b[53mZ\x1b[0mW");
    const CfrStyle *s2 = cfr_cell_style(vt, 0, 2);
    ASSERT_TRUE((s2->attrs & CFR_ATTR_OVERLINE) != 0);
    const CfrStyle *s3 = cfr_cell_style(vt, 0, 3);
    ASSERT_TRUE((s3->attrs & CFR_ATTR_OVERLINE) == 0);

    cfr_free(vt);
}

/* Overline composes with the underline family: the intern key must carry
 * both the new attribute bit and the underline style/colour. */
static void test_sgr_overline_combined(void)
{
    CfrTerm *vt = make_term(24, 80);
    feed(vt, "\x1b[53;4:3;58:2::255:0:0mX");

    const CfrStyle *s = cfr_cell_style(vt, 0, 0);
    ASSERT_TRUE((s->attrs & CFR_ATTR_OVERLINE) != 0);
    ASSERT_EQ(s->underline, CFR_UL_CURLY);
    ASSERT_EQ(s->ul.kind, CFR_COLOR_KIND_RGB);
    ASSERT_EQ(s->ul.rgb, 0x00FF0000u);

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

    const CfrStyle *s3 = cfr_cell_style(vt, 0, 3);
    ASSERT_EQ(s3->bg.kind, CFR_COLOR_KIND_RGB);
    ASSERT_EQ(s3->bg.rgb, 0x008080FFu);

    const CfrStyle *s79 = cfr_cell_style(vt, 0, 79);
    ASSERT_EQ(s79->bg.kind, CFR_COLOR_KIND_RGB);
    ASSERT_EQ(s79->bg.rgb, 0x008080FFu);

    cfr_free(vt);
}

/* BCE: erase-in-display mode 0 should extend background to end of
 * display.  The rows below the cursor are filled entirely. */
static void test_bce_erase_in_display(void)
{
    CfrTerm *vt = make_term(4, 10);
    feed(vt, "\x1b[48;5;196mX\x1b[J");

    /* Row 0: cell 1..9 should have the 256-col background. */
    const CfrStyle *s1 = cfr_cell_style(vt, 0, 1);
    ASSERT_EQ(s1->bg.kind, CFR_COLOR_KIND_INDEXED);
    ASSERT_EQ(s1->bg.index, 196u);

    /* Row 1: cell 0 should have the 256-col background. */
    const CfrStyle *s10 = cfr_cell_style(vt, 1, 0);
    ASSERT_EQ(s10->bg.kind, CFR_COLOR_KIND_INDEXED);
    ASSERT_EQ(s10->bg.index, 196u);

    cfr_free(vt);
}

int main(int argc, char **argv)
{
    test_parse_args(argc, argv);

    RUN_TEST(test_sgr_truecolor_bg);
    RUN_TEST(test_sgr_256_bg);
    RUN_TEST(test_sgr_ansi_bg);
    RUN_TEST(test_palette_starts_builtin);
    RUN_TEST(test_style_color_resolution_defaults);
    RUN_TEST(test_style_color_resolution_indexed);
    RUN_TEST(test_style_color_resolution_rgb);
    RUN_TEST(test_sgr_combined_fg_bg);
    RUN_TEST(test_sgr_trailing_semicolon_bg);
    RUN_TEST(test_sgr_trailing_semicolon_fg);
    RUN_TEST(test_sgr_combined_trailing_semicolon);
    RUN_TEST(test_sgr_reset_bg);
    RUN_TEST(test_sgr_multi_attr_trailing_semicolon);
    RUN_TEST(test_sgr_double_underline);
    RUN_TEST(test_sgr_overline);
    RUN_TEST(test_sgr_overline_combined);
    RUN_TEST(test_bce_erase_in_line);
    RUN_TEST(test_bce_erase_in_display);

    TEST_SUMMARY();
}
