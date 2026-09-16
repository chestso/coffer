/* tests/test_cfr_wrap_erase.c — soft-wrap vs erase/overwrite regressions */

#include "coffer_internal.h"
#include "test_helpers.h"
#include <coffer/coffer.h>

#include <stdio.h>
#include <string.h>

static CfrTerm *make_term(int rows, int cols)
{
    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.rows = rows;
    cfg.cols = cols;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 6;
    CfrTerm *vt = cfr_new(&cfg);
    return vt;
}

static void feed(CfrTerm *vt, const char *s)
{
    cfr_input_write(vt, (const uint8_t *)s, strlen(s));
}

/* Build "AAAA...(20)" which fills row 0 and wraps a 5-char tail onto
 * row 1, then "short\r\n" on row 2. The row-0/row-1 pair is a single
 * logical line; row 1 must be a continuation of row 0. */
static void prime_wrapped_pair(CfrTerm *vt)
{
    feed(vt, "AAAAAAAAAAAAAAAAAAAAAAAAA");
    feed(vt, "\r\nshort\r\n");
}

/* Reference: the untouched pair is wrapped. */
static void test_wrapped_pair_baseline(void)
{
    CfrTerm *vt = make_term(8, 20);
    prime_wrapped_pair(vt);
    ASSERT_TRUE(cfr_get_line_continuation(vt, 0));
    ASSERT_TRUE(cfr_row_is_continuation(vt, 1));
    cfr_free(vt);
}

/* EL0 from the first cell of the wrapped row erases the whole row.
 * The logical line is now just the five chars on row 1, so row 1 is no
 * longer a continuation. xterm and libvterm both clear the wrap; the
 * WRAPLINE flag on row 0 must not survive when the row's last cell is
 * erased. */
static void test_el0_at_wrapped_row_start_clears_wrap(void)
{
    CfrTerm *vt = make_term(8, 20);
    prime_wrapped_pair(vt);
    feed(vt, "\x1b[1;1H\x1b[K");
    ASSERT_FALSE(cfr_row_is_continuation(vt, 1));
    /* Row 0 itself is also no longer a wrap of an empty line. */
    ASSERT_FALSE(cfr_get_line_continuation(vt, 0));
    cfr_free(vt);
}

/* EL2 (erase whole line) and EL0 at the last column both remove the last
 * cell of the wrapped row, so both must drop the WRAPLINE flag. */
static void test_erase_whole_wrapped_row_clears_wrap(void)
{
    CfrTerm *vt = make_term(8, 20);
    prime_wrapped_pair(vt);
    feed(vt, "\x1b[1;1H\x1b[2K");
    ASSERT_FALSE(cfr_row_is_continuation(vt, 1));
    cfr_free(vt);

    vt = make_term(8, 20);
    prime_wrapped_pair(vt);
    feed(vt, "\x1b[1;20H\x1b[K"); /* EL0 at last column */
    ASSERT_FALSE(cfr_row_is_continuation(vt, 1));
    cfr_free(vt);
}

/* ECH covering the final cell behaves like EL there. */
static void test_ech_last_cell_clears_wrap(void)
{
    CfrTerm *vt = make_term(8, 20);
    prime_wrapped_pair(vt);
    feed(vt, "\x1b[1;20H\x1b[1X");
    ASSERT_FALSE(cfr_row_is_continuation(vt, 1));
    cfr_free(vt);
}

/* ED0 from the wrapped row's start erases it and everything below, so the
 * next row is no longer a continuation. */
static void test_ed0_on_wrapped_row_clears_wrap(void)
{
    CfrTerm *vt = make_term(8, 20);
    prime_wrapped_pair(vt);
    feed(vt, "\x1b[1;1H\x1b[J");
    ASSERT_FALSE(cfr_row_is_continuation(vt, 1));
    cfr_free(vt);
}

/* Writing again after the erase must re-establish the wrap naturally. */
static void test_rewrite_after_erase_restores_wrap(void)
{
    CfrTerm *vt = make_term(8, 20);
    prime_wrapped_pair(vt);
    feed(vt, "\x1b[1;1H\x1b[2K");
    feed(vt, "YYYYYYYYYYYYYYYYYYYYYYYYY"); /* 25 chars -> wraps into row 1 */
    ASSERT_TRUE(cfr_get_line_continuation(vt, 0));
    ASSERT_TRUE(cfr_row_is_continuation(vt, 1));
    cfr_free(vt);
}

/* Overwriting a single interior cell of the continuation row must not
 * disturb the row's continuation status. */
static void test_overwrite_interior_keeps_wrap(void)
{
    CfrTerm *vt = make_term(8, 20);
    prime_wrapped_pair(vt);
    feed(vt, "\x1b[2;10HZ");
    ASSERT_TRUE(cfr_row_is_continuation(vt, 1));
    cfr_free(vt);
}

/* ED3 (Erase Saved Lines) clears scrollback only; the visible grid — and
 * therefore its wrap flags — must be left alone. */
static void test_ed3_leaves_grid_and_wrap(void)
{
    CfrTerm *vt = make_term(8, 20);
    prime_wrapped_pair(vt);
    feed(vt, "\x1b[2;1H\x1b[3J");
    ASSERT_TRUE(cfr_get_line_continuation(vt, 0));
    ASSERT_TRUE(cfr_row_is_continuation(vt, 1));
    ASSERT_EQ(cfr_get_cell(vt, 0, 0)->cp, (uint32_t)'A');
    cfr_free(vt);
}

/* A line feed while the cursor sits on the last column with a deferred wrap
 * (the cell at the margin was just written) resolves the wrap as "no join":
 * the next row is not a continuation. xterm clears the phantom here rather
 * than wrapping, and the wrapped row must lose its WRAPLINE flag. */
static void test_linefeed_clears_deferred_wrap(void)
{
    CfrTerm *vt = make_term(8, 20);
    feed(vt, "AAAAAAAAAAAAAAAAAAAA");
    ASSERT_TRUE(vt->cursor.pending_wrap);
    feed(vt, "\n");
    ASSERT_FALSE(vt->cursor.pending_wrap);
    ASSERT_FALSE(cfr_get_line_continuation(vt, 0));
    ASSERT_FALSE(cfr_row_is_continuation(vt, 1));
    cfr_free(vt);
}

/* The same deferred wrap printed through still joins the rows. */
static void test_print_through_deferred_wrap_keeps_wrap(void)
{
    CfrTerm *vt = make_term(8, 20);
    feed(vt, "AAAAAAAAAAAAAAAAAAAA");
    feed(vt, "B");
    ASSERT_TRUE(cfr_get_line_continuation(vt, 0));
    ASSERT_TRUE(cfr_row_is_continuation(vt, 1));
    cfr_free(vt);
}

/* ICH shifts the row's cells right instead of removing them, so the wrap
 * survives; DCH that reaches the margin removes the margin cell and
 * truncates the logical line. */
static void test_ich_keeps_wrap_dch_to_margin_clears(void)
{
    CfrTerm *vt = make_term(8, 20);
    prime_wrapped_pair(vt);
    feed(vt, "\x1b[1;1H\x1b[20@");
    ASSERT_TRUE(cfr_row_is_continuation(vt, 1));
    cfr_free(vt);

    vt = make_term(8, 20);
    prime_wrapped_pair(vt);
    feed(vt, "\x1b[1;1H\x1b[20P");
    ASSERT_FALSE(cfr_row_is_continuation(vt, 1));
    cfr_free(vt);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    RUN_TEST(test_wrapped_pair_baseline);
    RUN_TEST(test_el0_at_wrapped_row_start_clears_wrap);
    RUN_TEST(test_erase_whole_wrapped_row_clears_wrap);
    RUN_TEST(test_ech_last_cell_clears_wrap);
    RUN_TEST(test_ed0_on_wrapped_row_clears_wrap);
    RUN_TEST(test_ed3_leaves_grid_and_wrap);
    RUN_TEST(test_linefeed_clears_deferred_wrap);
    RUN_TEST(test_print_through_deferred_wrap_keeps_wrap);
    RUN_TEST(test_rewrite_after_erase_restores_wrap);
    RUN_TEST(test_overwrite_interior_keeps_wrap);
    RUN_TEST(test_ich_keeps_wrap_dch_to_margin_clears);
    TEST_SUMMARY();
}
