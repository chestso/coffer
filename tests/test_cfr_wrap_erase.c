/* tests/test_cfr_wrap_erase.c — soft-wrap vs erase/overwrite regressions */

#include "coffer_internal.h"
#include "test_helpers.h"
#include "wrap_cases.h"
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

/* 25 A's fill row 0 (20 cols) and wrap five onto row 1; the trailing
 * CRLFs put "short" on row 2. Rows 0 and 1 are one logical line. */
#define PRIME_WRAPPED_PAIR "AAAAAAAAAAAAAAAAAAAAAAAAA\r\nshort\r\n"

/* The §5.6 edge/lineage rules: any rewrite of the margin cell
 * (EL/ECH/print) severs the join; ICH re-stamps the edge, DCH does not;
 * a scroll moves the edge with the row; the phantom is not a join until
 * a print commits it. Cases that also need cursor or cell assertions stay
 * as functions below. */
static const JoinCase JOIN_CASES[] = {
    { "wrapped pair baseline", 8, 20, PRIME_WRAPPED_PAIR, NULL, 1, true },
    { "EL0 from the wrapped row start severs", 8, 20,
      PRIME_WRAPPED_PAIR, "\x1b[1;1H\x1b[K", 1, false },
    { "EL2 severs", 8, 20, PRIME_WRAPPED_PAIR, "\x1b[1;1H\x1b[2K", 1,
      false },
    { "EL0 at the margin severs", 8, 20, PRIME_WRAPPED_PAIR,
      "\x1b[1;20H\x1b[K", 1, false },
    { "ECH over the margin severs", 8, 20, PRIME_WRAPPED_PAIR,
      "\x1b[1;20H\x1b[1X", 1, false },
    { "ED0 from the wrapped row severs", 8, 20, PRIME_WRAPPED_PAIR,
      "\x1b[1;1H\x1b[J", 1, false },
    { "rewriting re-establishes the wrap", 8, 20, PRIME_WRAPPED_PAIR,
      "\x1b[1;1H\x1b[2K"
      "YYYYYYYYYYYYYYYYYYYYYYYYY",
      1, true },
    { "interior overwrite keeps the wrap", 8, 20, PRIME_WRAPPED_PAIR,
      "\x1b[2;10HZ", 1, true },
    { "21 A's commit a wrap", 8, 20, "AAAAAAAAAAAAAAAAAAAAA", NULL, 1,
      true },
    { "LF preserves a committed wrap", 8, 20, "AAAAAAAAAAAAAAAAAAAAA",
      "B\n", 1, true },
    { "margin overwrite severs", 8, 20, "AAAAAAAAAAAAAAAAAAAAA",
      "\x1b[1;20HZ", 1, false },
    { "printing through the phantom joins", 8, 20,
      "AAAAAAAAAAAAAAAAAAAA", "B", 1, true },
    { "ICH re-stamps the edge", 8, 20, PRIME_WRAPPED_PAIR,
      "\x1b[1;1H\x1b[20@", 1, true },
    { "DCH to the margin severs", 8, 20, PRIME_WRAPPED_PAIR,
      "\x1b[1;1H\x1b[20P", 1, false },
};

static void test_join_cases(void)
{
    ASSERT_TRUE(run_join_cases("erase", JOIN_CASES,
                               sizeof(JOIN_CASES) / sizeof(JOIN_CASES[0])));
}

/* ED3 (Erase Saved Lines) clears scrollback only; the visible grid — and
 * therefore its wrap flags — must be left alone. */
static void test_ed3_leaves_grid_and_wrap(void)
{
    CfrTerm *vt = make_term(8, 20);
    feed(vt, PRIME_WRAPPED_PAIR);
    feed(vt, "\x1b[2;1H\x1b[3J");
    ASSERT_TRUE(cfr_row_is_continuation(vt, 1));
    ASSERT_EQ(cfr_get_cell(vt, 0, 0)->cp, (uint32_t)'A');
    cfr_free(vt);
}

/* A line feed while the cursor carries a deferred phantom (the margin cell
 * was just written) resolves the wrap as "no join": the phantom is not a
 * committed wrap, so no edge was ever stamped and the next row is not a
 * continuation. tmux moves down (here scrolling, since the cursor is on the
 * bottom row) with the column reset to 0. */
static void test_linefeed_resolves_phantom_without_join(void)
{
    CfrTerm *vt = make_term(8, 20);
    feed(vt, "AAAAAAAAAAAAAAAAAAAA");
    ASSERT_TRUE(PENDING_WRAP(vt));
    feed(vt, "\n");
    ASSERT_FALSE(PENDING_WRAP(vt));
    ASSERT_FALSE(cfr_row_is_continuation(vt, 1));
    cfr_free(vt);
}

/* DCH severs the row's wrap edge, and the row must stay severed. The
 * left shift moves the old margin cell — with its WRAPLINE bit — into the
 * row's interior; if the bit is allowed to ride there, repeated ICH can
 * walk it back to the margin and resurrect a join DCH already severed.
 * Only the margin cell may carry the bit (spec §5.6). */
static void test_dch_then_ich_does_not_resurrect_wrap(void)
{
    CfrTerm *vt = make_term(8, 6);
    feed(vt, "f46w95");
    feed(vt, "n"); /* wrap: edge on row 0's margin, 'n' on row 1 */
    ASSERT_TRUE(cfr_row_is_continuation(vt, 1));

    feed(vt, "\x1b[1;2H\x1b[3P"); /* DCH 3: severs row 0 */
    ASSERT_FALSE(cfr_row_is_continuation(vt, 1));

    /* Walk the row's interior right: nothing may bring the severed edge
     * back onto the margin. */
    for (int i = 0; i < 3; ++i) {
        feed(vt, "\x1b[1;2H\x1b[@");
        ASSERT_FALSE(cfr_row_is_continuation(vt, 1));
    }
    cfr_free(vt);
}

/* DECSC/DECRC round-trip the deferred-wrap phantom: the saved column is
 * the logical one (one past the margin), so restoring it and printing
 * commits the wrap. Spec §5.5. */
static void test_decsc_roundtrips_phantom(void)
{
    CfrTerm *vt = make_term(8, 6);
    feed(vt, "abcdef"); /* row 0 exactly full: the phantom */
    ASSERT_TRUE(PENDING_WRAP(vt));
    feed(vt, "\x1b"
             "7");         /* DECSC saves the phantom */
    feed(vt, "\x1b[3;1H"); /* explicit move kills it */
    ASSERT_FALSE(PENDING_WRAP(vt));
    feed(vt, "\x1b"
             "8"); /* DECRC restores it */
    ASSERT_TRUE(PENDING_WRAP(vt));
    feed(vt, "x"); /* the next print wraps */
    ASSERT_EQ(cfr_get_cell(vt, 1, 0)->cp, (uint32_t)'x');
    ASSERT_TRUE(cfr_row_is_continuation(vt, 1));
    cfr_free(vt);
}

/* A print at the phantom on the last row but *below* the scroll region's
 * bottom has no wrap: the print overwrites the margin cell. The ASCII run
 * fast path used to skip the clamp after a failed cfr_wrap_commit() and
 * write one cell past the row (a heap overflow). It must land on the
 * margin, exactly as commit_cluster() does. */
static void test_ascii_run_below_scroll_region_clamps(void)
{
    CfrTerm *vt = make_term(3, 4);
    feed(vt, "\x1b[1;2r"); /* region rows 0..1; row 2 is outside it */
    feed(vt, "\x1b[3;3H"); /* row index 2, col index 2 */
    feed(vt, "ab");        /* fills the row; cursor is the phantom */
    ASSERT_TRUE(PENDING_WRAP(vt));
    feed(vt, "c"); /* no wrap possible; overwrite the margin */
    ASSERT_EQ(cfr_get_cell(vt, 2, 3)->cp, (uint32_t)'c');
    char buf[16];
    cfr_row_text(vt, 2, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "ac");
    cfr_free(vt);
}

/* DECRC restores a saved cursor without clamping, so a register saved
 * before a shrink can put the cursor row outside the grid. ED 0/1 must
 * treat that as a no-op instead of clearing rows past the end. */
static void test_ed_with_stale_cursor_row_is_noop(void)
{
    CfrTerm *vt = make_term(4, 6);
    feed(vt, "top\r\nabc");
    feed(vt, "\x1b[4;2H\x1b"
             "7"); /* cursor row 3, saved */
    cfr_resize(vt, 2, 6);
    feed(vt, "\x1b"
             "8"); /* restore row 3: outside the 2-row grid */
    feed(vt, "\x1b[1J");

    /* The visible rows are untouched (ED1 was a no-op) and the cursor
     * still reports the restored position. A print then clamps it. */
    char buf[16];
    cfr_row_text(vt, 0, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "top");
    cfr_row_text(vt, 1, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "abc");
    ASSERT_EQ(cfr_get_cursor(vt).row, 3);
    feed(vt, "Z");
    ASSERT_EQ(cfr_get_cursor(vt).row, 1);
    ASSERT_EQ(cfr_get_cell(vt, 1, 1)->cp, (uint32_t)'Z');
    cfr_free(vt);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    RUN_TEST(test_join_cases);
    RUN_TEST(test_ed3_leaves_grid_and_wrap);
    RUN_TEST(test_linefeed_resolves_phantom_without_join);
    RUN_TEST(test_dch_then_ich_does_not_resurrect_wrap);
    RUN_TEST(test_decsc_roundtrips_phantom);
    RUN_TEST(test_ascii_run_below_scroll_region_clamps);
    RUN_TEST(test_ed_with_stale_cursor_row_is_noop);
    TEST_SUMMARY();
}
