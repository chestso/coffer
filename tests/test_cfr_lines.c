/* tests/test_cfr_lines.c — wrapped-line identity regressions.
 *
 * Probes A–E from docs/wrapped-lines-design.md §3 step 4: the representation
 * (margin-cell wrap edge + per-row lineage id) must make cross-line false
 * joins structurally impossible, and must preserve a genuine join across
 * scrolls, IL/DL, and scrollback. */

#include "coffer_internal.h"
#include "test_helpers.h"
#include <coffer/coffer.h>

#include <stdio.h>
#include <string.h>

static CfrTerm *make_term(int rows, int cols, bool reflow)
{
    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.rows = rows;
    cfg.cols = cols;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 6;
    cfg.reflow = reflow;
    cfg.scrollback = 100;
    return cfr_new(&cfg);
}

static void feed(CfrTerm *vt, const char *s)
{
    cfr_input_write(vt, (const uint8_t *)s, strlen(s));
}

/* Build the design's 4-column example: twelve A's (wrapping rows 0→1→2),
 * then LF and "BBBB" on row 3. */
static void prime_aaaa_bbbb(CfrTerm *vt)
{
    feed(vt, "AAAAAAAAAAAA"); /* 12 A's: rows 0,1,2 in a 4-col grid */
    feed(vt, "\nBBBB");
}

/* -------- A: DL of a *different* line must not create a false join ------
 *
 * The §2.3 example, DL at row 2. Row 1's margin bit survives untouched (a
 * row-flag design would false-join); lineage catches it because the row
 * that slid up carries a different id. */
static void test_dl_foreign_line_no_false_join(void)
{
    CfrTerm *vt = make_term(8, 4, false);
    prime_aaaa_bbbb(vt);
    ASSERT_TRUE(cfr_row_is_continuation(vt, 1));
    ASSERT_TRUE(cfr_row_is_continuation(vt, 2));
    /* Delete row 2 (the last A fragment); BBBB slides up under row 1. */
    feed(vt, "\x1b[3;1H\x1b[1M");
    /* Row 1 still ends in a wrap, but the row below it is now BBBB, a
     * different logical line. No join. */
    ASSERT_FALSE(cfr_row_is_continuation(vt, 2));
    cfr_free(vt);
}

/* -------- B: EL truncating the row must sever the join ------------------
 *
 * Erase row 0 to end of line: the margin cell is rewritten, so its wrap
 * edge dies automatically even though row 1 still shares row 0's lineage
 * (the lineage-alone failure mode). */
static void test_el_truncation_severs(void)
{
    CfrTerm *vt = make_term(8, 20, false);
    feed(vt, "AAAAAAAAAAAAAAAAAAAAA"); /* row 0 wraps 'A' onto row 1 */
    ASSERT_TRUE(cfr_row_is_continuation(vt, 1));
    feed(vt, "\x1b[1;1H\x1b[K"); /* EL0 over the whole wrapped row */
    ASSERT_FALSE(cfr_row_is_continuation(vt, 1));
    cfr_free(vt);
}

/* -------- C: IL inserts a blank with lineage 0 --------------------------
 *
 * An inserted blank row arrives with lineage 0, so it severs on its own
 * — no explicit clear-call at the IL site. */
static void test_il_blank_severs(void)
{
    CfrTerm *vt = make_term(8, 4, false);
    feed(vt, "AAAAAAAAAAAA");
    ASSERT_TRUE(cfr_row_is_continuation(vt, 1));
    /* Insert a blank line at row 1: the previous row 1 slides to row 2. */
    feed(vt, "\x1b[2;1H\x1b[1L");
    ASSERT_FALSE(cfr_row_is_continuation(vt, 1)); /* blank row, lineage 0 */
    cfr_free(vt);
}

/* -------- D: SU under a live edge ---------------------------------------
 *
 * A full-screen scroll moves the wrapped pair and its ids together; the
 * join survives, and the newly exposed bottom row (lineage 0) joins
 * nothing. */
static void test_scroll_preserves_join(void)
{
    CfrTerm *vt = make_term(4, 4, false);
    feed(vt, "AAAAAAAAAAAA"); /* rows 0,1,2 filled; 0→1→2 wrapped */
    ASSERT_TRUE(cfr_row_is_continuation(vt, 1));
    ASSERT_TRUE(cfr_row_is_continuation(vt, 2));
    feed(vt, "\x1b[4;1H"); /* move to bottom row */
    feed(vt, "\x1b[1S");   /* SU 1: rows shift up, blank enters at bottom */
    /* Rows 0 and 1 still form the pair (now holding the old rows 1 and 2). */
    ASSERT_TRUE(cfr_row_is_continuation(vt, 1));
    ASSERT_FALSE(cfr_row_is_continuation(vt, 3));
    cfr_free(vt);
}

/* -------- E: phantom + SU -----------------------------------------------
 *
 * Printing a phantom-carrying row and then scrolling: the phantom was never
 * committed, so no join appears, and the lineage id rides the scroll. */
static void test_phantom_then_scroll(void)
{
    CfrTerm *vt = make_term(3, 4, false);
    feed(vt, "AAAA"); /* row 0 full, phantom pending, nothing committed */
    ASSERT_TRUE(PENDING_WRAP(vt));
    ASSERT_FALSE(cfr_row_is_continuation(vt, 1));
    feed(vt, "\x1b[3;1H\x1b[1S"); /* SU 1 from a different row */
    ASSERT_FALSE(cfr_row_is_continuation(vt, 1));
    cfr_free(vt);
}

/* A wrapped line that scrolls into scrollback keeps its join across the
 * grid/scrollback boundary. */
static void test_join_into_scrollback(void)
{
    CfrTerm *vt = make_term(2, 10, false);
    feed(vt, "abcdefghijklmnopqrst"); /* fills row 0, wraps 'k'.. onto row 1 */
    ASSERT_TRUE(cfr_row_is_continuation(vt, 1));
    /* Push content off the top until the wrapped pair is in history. */
    feed(vt, "\r\n\r\n\r\n\r\n");
    int sb = cfr_get_scrollback_lines(vt);
    ASSERT_TRUE(sb > 0);
    /* Exactly one boundary in scrollback is a join (the soft-wrapped pair);
     * the blank rows pushed after it are not. */
    int joins = 0;
    for (int u = -sb; u <= 0; ++u)
        if (cfr_row_is_continuation(vt, u))
            joins++;
    ASSERT_EQ(joins, 1);
    cfr_free(vt);
}

/* -------- Known imperfection: DL *inside* one wrapped line --------------
 *
 * Fragment 1 and fragment 3 of the same line share an id, so deleting the
 * middle fragment leaves a join that the representation cannot detect
 * (§2.3). Documented: wrong text, never wrong structure. */
static void test_dl_inside_line_known_join(void)
{
    CfrTerm *vt = make_term(8, 4, false);
    feed(vt, "AAAAAAAAAAAA");     /* rows 0,1,2 one logical line */
    feed(vt, "\x1b[2;1H\x1b[1M"); /* delete the middle fragment */
    /* The known-imperfect answer: still joined. */
    ASSERT_TRUE(cfr_row_is_continuation(vt, 1));
    cfr_free(vt);
}

/* Reflow must keep a soft wrap soft and a hard line hard, propagating the
 * lineage across the resize. */
static void test_reflow_preserves_line_identity(void)
{
    CfrTerm *vt = make_term(4, 10, true);
    feed(vt, "012345678901234567890123"); /* one 24-char logical line */
    ASSERT_TRUE(cfr_row_is_continuation(vt, 1));
    cfr_resize(vt, 4, 5); /* rewrap to 5 cols */
    ASSERT_TRUE(cfr_row_is_continuation(vt, 1));
    ASSERT_TRUE(cfr_row_is_continuation(vt, 2));
    /* 24 chars / 5 cols = 5 rows; the oldest one scrolled into history, so
     * the visible grid starts at '5'. */
    ASSERT_EQ(cfr_get_cell(vt, 0, 0)->cp, (uint32_t)'5');
    /* Grow back: the visible remainder unwraps to a single row. The first
     * fragment ('01234') is in scrollback, which reflow does not join back
     * in (§3 step 5 lists carrying ids across the sb/grid split); the
     * visible line is the 19 chars '5'..'3'. */
    cfr_resize(vt, 4, 30);
    ASSERT_FALSE(cfr_row_is_continuation(vt, 1));
    ASSERT_EQ(cfr_get_cell(vt, 0, 0)->cp, (uint32_t)'5');
    ASSERT_EQ(cfr_get_cell(vt, 0, 18)->cp, (uint32_t)'3');
    cfr_free(vt);
}

/* -------- Reflow round trip: shrink (with scroll) then grow back --------
 *
 * Fill the screen with wrapped lines, shrink to half width so content
 * scrolls into history, then grow back. This pins two behaviors:
 *
 *  1. The visible portion round-trips losslessly. Content that stays on
 *     screen across the shrink is re-expanded to the original width, and
 *     its joins are intact.
 *  2. Content pushed into scrollback during the shrink does NOT
 *     re-expand on grow-back. Scrollback pages keep the column width
 *     they were written at (design §5: reflow scope is the active grid
 *     only), the scrollback line count is unchanged by the grow, and
 *     reflow does not carry lineage across the scrollback/grid split.
 *
 * If the FOLLOWUPS "carry lineage across the scrollback/grid split" item
 * is implemented, part 2 starts failing — which is the intended signal
 * that this test needs updating.
 *
 * Setup: 8x20 grid, four 40-char lines (A/B/C/D), each a soft-wrapped
 * pair of rows. Shrinking to 10 cols makes each line four rows (16
 * total), so A and B scroll off; C and D remain visible.
 */
static void test_reflow_roundtrip_shrink_grow(void)
{
    CfrTerm *vt = make_term(8, 20, true);
    const char *lines[4] = { "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
                             "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB",
                             "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC",
                             "DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD" };
    for (int i = 0; i < 4; ++i) {
        feed(vt, lines[i]);
        if (i < 3)
            feed(vt, "\r\n");
    }

    /* Initial: four wrapped pairs, everything visible, no scrollback. */
    ASSERT_EQ(cfr_get_scrollback_lines(vt), 0);
    ASSERT_FALSE(cfr_row_is_continuation(vt, 0));
    ASSERT_TRUE(cfr_row_is_continuation(vt, 1)); /* A pair */
    ASSERT_FALSE(cfr_row_is_continuation(vt, 2));
    ASSERT_TRUE(cfr_row_is_continuation(vt, 3)); /* B pair */
    ASSERT_EQ(cfr_get_cell(vt, 0, 0)->cp, (uint32_t)'A');

    /* Shrink to half width: each line becomes four 10-col rows, A and B
     * scroll into history, C and D stay on screen. */
    cfr_resize(vt, 8, 10);
    ASSERT_EQ(cfr_get_scrollback_lines(vt), 8); /* A + B, four rows each */
    ASSERT_EQ(cfr_get_cell(vt, 0, 0)->cp, (uint32_t)'C');
    ASSERT_TRUE(cfr_row_is_continuation(vt, 1));
    ASSERT_TRUE(cfr_row_is_continuation(vt, 2));
    ASSERT_TRUE(cfr_row_is_continuation(vt, 3));
    ASSERT_FALSE(cfr_row_is_continuation(vt, 4));
    ASSERT_EQ(cfr_get_cell(vt, 4, 0)->cp, (uint32_t)'D');

    /* Scrollback keeps the wraps it was pushed with. */
    ASSERT_FALSE(cfr_row_is_continuation(vt, -8)); /* A's first fragment */
    ASSERT_TRUE(cfr_row_is_continuation(vt, -7));
    ASSERT_TRUE(cfr_row_is_continuation(vt, -6));
    ASSERT_TRUE(cfr_row_is_continuation(vt, -5));
    ASSERT_FALSE(cfr_row_is_continuation(vt, -4)); /* B's first fragment */
    ASSERT_TRUE(cfr_row_is_continuation(vt, -3));

    /* Grow back to 20 cols. */
    cfr_resize(vt, 8, 20);

    /* Part 1 — the visible lines are restored to their full 40-char,
     * two-row form with joins intact. */
    ASSERT_EQ(cfr_get_cell(vt, 0, 0)->cp, (uint32_t)'C');
    ASSERT_EQ(cfr_get_cell(vt, 0, 19)->cp, (uint32_t)'C');
    ASSERT_EQ(cfr_get_cell(vt, 1, 0)->cp, (uint32_t)'C');
    ASSERT_TRUE(cfr_row_is_continuation(vt, 1));
    ASSERT_FALSE(cfr_row_is_continuation(vt, 2));
    ASSERT_EQ(cfr_get_cell(vt, 2, 0)->cp, (uint32_t)'D');
    ASSERT_EQ(cfr_get_cell(vt, 3, 0)->cp, (uint32_t)'D');
    ASSERT_TRUE(cfr_row_is_continuation(vt, 3));
    ASSERT_FALSE(cfr_row_is_continuation(vt, 4));

    /* Part 2 — scrollback is left untouched at the old column width: the
     * line count is unchanged, its joins survive, and the pages are still
     * 10 columns wide (cell 9 readable, cell 10 out of range). */
    ASSERT_EQ(cfr_get_scrollback_lines(vt), 8);
    ASSERT_FALSE(cfr_row_is_continuation(vt, -8));
    ASSERT_TRUE(cfr_row_is_continuation(vt, -7));
    ASSERT_NOT_NULL(cfr_get_scrollback_cell(vt, 0, 9));
    ASSERT_NULL(cfr_get_scrollback_cell(vt, 0, 10));

    cfr_free(vt);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    RUN_TEST(test_dl_foreign_line_no_false_join);
    RUN_TEST(test_el_truncation_severs);
    RUN_TEST(test_il_blank_severs);
    RUN_TEST(test_scroll_preserves_join);
    RUN_TEST(test_phantom_then_scroll);
    RUN_TEST(test_join_into_scrollback);
    RUN_TEST(test_dl_inside_line_known_join);
    RUN_TEST(test_reflow_preserves_line_identity);
    RUN_TEST(test_reflow_roundtrip_shrink_grow);
    TEST_SUMMARY();
}
