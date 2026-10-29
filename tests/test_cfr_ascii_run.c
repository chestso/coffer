/* tests/test_cfr_ascii_run.c — the bulk ASCII print path.
 *
 * cfr_print_ascii_run() commits printable-ASCII runs straight into the
 * grid instead of routing every codepoint through the cluster buffer.
 * It must be indistinguishable from the per-codepoint path at every
 * boundary: a joiner that follows the run re-attaches to its last cell,
 * a Prepend base declines the run, the DEC graphics charset and IRM
 * decline it, wraps and the deferred phantom advance exactly as
 * commit_cluster() does, and the damage rect still covers the span. */

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
    cfg.scrollback = 100;
    cfg.reflow = false;
    return cfr_new(&cfg);
}

static void feed(CfrTerm *vt, const char *s)
{
    cfr_input_write(vt, (const uint8_t *)s, strlen(s));
}

static void capture_damage(CfrRect rect, void *user)
{
    *(CfrRect *)user = rect;
}

/* ------------------------------------------------------------------ */

static void test_run_writes_cells_and_cursor(void)
{
    CfrTerm *vt = make_term(4, 10);
    feed(vt, "hello");
    for (int c = 0; c < 5; c++) {
        ASSERT_EQ(cfr_get_cell(vt, 0, c)->cp, (uint32_t)("hello"[c]));
        ASSERT_EQ(cfr_get_cell(vt, 0, c)->width, 1);
        ASSERT_EQ(cfr_get_cell(vt, 0, c)->grapheme_id, 0u);
    }
    ASSERT_EQ(vt->cursor.row, 0);
    ASSERT_EQ(vt->cursor.col, 5);
    /* Nothing is pending: the run committed every cell eagerly. */
    ASSERT_EQ(vt->cursor.cluster_len, 0u);
    /* The damage rect covers the span the run wrote; the flush folds the
     * cursor's own cell in, so the rect reaches the cursor column. */
    CfrRect seen = { -1, -1, -1, -1 };
    CfrCallbacks cb = { 0 };
    cb.damage = capture_damage;
    cfr_set_callbacks(vt, &cb, &seen);
    cfr_damage_flush(vt);
    ASSERT_EQ(seen.start_row, 0);
    ASSERT_EQ(seen.start_col, 0);
    ASSERT_EQ(seen.end_row, 0);
    ASSERT_EQ(seen.end_col, 5);
    cfr_free(vt);
}

static void test_run_wraps_like_commit_cluster(void)
{
    CfrTerm *vt = make_term(4, 5);
    feed(vt, "abcdefghijkl"); /* 12 cells into 5 columns */
    /* Row 0: abcde, row 1: fghij, row 2: kl. */
    ASSERT_EQ(cfr_get_cell(vt, 0, 0)->cp, (uint32_t)'a');
    ASSERT_EQ(cfr_get_cell(vt, 1, 0)->cp, (uint32_t)'f');
    ASSERT_EQ(cfr_get_cell(vt, 2, 0)->cp, (uint32_t)'k');
    ASSERT_EQ(cfr_get_cell(vt, 2, 1)->cp, (uint32_t)'l');
    ASSERT_EQ(vt->cursor.row, 2);
    ASSERT_EQ(vt->cursor.col, 2);
    /* Wrap edges: the margin cells carry the join flags. */
    ASSERT_TRUE(cfr_get_cell(vt, 0, 4)->flags & CFR_CELL_WRAPLINE);
    ASSERT_TRUE(cfr_get_cell(vt, 1, 4)->flags & CFR_CELL_WRAPLINE);
    cfr_free(vt);
}

static void test_run_resolves_the_phantom_across_writes(void)
{
    CfrTerm *vt = make_term(4, 5);
    feed(vt, "abcde"); /* fills row 0; cursor parks on the phantom */
    ASSERT_EQ(vt->cursor.col, 5);
    feed(vt, "f"); /* the phantom resolves into a wrap */
    ASSERT_EQ(vt->cursor.row, 1);
    ASSERT_EQ(vt->cursor.col, 1);
    ASSERT_EQ(cfr_get_cell(vt, 1, 0)->cp, (uint32_t)'f');
    ASSERT_TRUE(cfr_get_cell(vt, 0, 4)->flags & CFR_CELL_WRAPLINE);
    cfr_free(vt);
}

static void test_run_stops_at_a_wide_cluster(void)
{
    CfrTerm *vt = make_term(4, 10);
    /* "abc" is the run, 中 U+4E2D is the per-codepoint path, then "z"
     * starts a second run. */
    feed(vt, "abc\xE4\xB8\xADz");
    ASSERT_EQ(cfr_get_cell(vt, 0, 0)->cp, (uint32_t)'a');
    ASSERT_EQ(cfr_get_cell(vt, 0, 2)->cp, (uint32_t)'c');
    ASSERT_EQ(cfr_get_cell(vt, 0, 3)->cp, 0x4E2Du);
    ASSERT_EQ(cfr_get_cell(vt, 0, 3)->width, 2);
    ASSERT_EQ(cfr_get_cell(vt, 0, 4)->width, 0); /* continuation */
    ASSERT_EQ(cfr_get_cell(vt, 0, 5)->cp, (uint32_t)'z');
    ASSERT_EQ(vt->cursor.col, 6);
    cfr_free(vt);
}

static void test_trailing_combining_mark_attaches_to_the_run(void)
{
    CfrTerm *vt = make_term(4, 10);
    /* The run commits "ab" eagerly; U+0301 then arrives with no pending
     * cluster and must re-attach to the "b" cell as one grapheme. */
    feed(vt, "ab\xCC\x81");
    const CfrCell *b = cfr_get_cell(vt, 0, 1);
    ASSERT_EQ(b->cp, (uint32_t)'b');
    ASSERT_EQ(b->width, 1);
    ASSERT_NEQ(b->grapheme_id, 0u);
    uint32_t cps[CFR_CLUSTER_MAX];
    size_t n = cfr_cell_get_grapheme(vt, b, cps, CFR_CLUSTER_MAX);
    ASSERT_EQ(n, (size_t)2);
    ASSERT_EQ(cps[0], (uint32_t)'b');
    ASSERT_EQ(cps[1], 0x0301u);
    ASSERT_EQ(vt->cursor.col, 2);
    cfr_free(vt);
}

static void test_trailing_vs16_doubles_the_last_cell(void)
{
    CfrTerm *vt = make_term(4, 10);
    /* VS16 forces a 2-cell cluster even over an ASCII base. The eager
     * commit wrote "x" at width 1; the attach must re-commit the
     * combined cluster at the recomputed width, stranding nothing. */
    feed(vt, "x\xEF\xB8\x8F");
    const CfrCell *x = cfr_get_cell(vt, 0, 0);
    ASSERT_EQ(x->cp, (uint32_t)'x');
    ASSERT_EQ(x->width, 2);
    ASSERT_EQ(cfr_get_cell(vt, 0, 1)->width, 0); /* continuation */
    ASSERT_EQ(cfr_get_cell(vt, 0, 2)->cp, 0u);   /* untouched */
    ASSERT_EQ(vt->cursor.col, 2);
    cfr_free(vt);
}

static void test_run_declines_for_prepend_base(void)
{
    CfrTerm *vt = make_term(4, 10);
    /* U+0600 is a GB9b Prepend base: it captures the next codepoint, so
     * the run must not flush it and print the 'x' on its own. The
     * write-end flush commits the pair as one cluster on the base. */
    feed(vt, "\xD8\x80"
             "x");
    ASSERT_EQ(vt->cursor.cluster_len, 0u);
    ASSERT_EQ(cfr_get_cell(vt, 0, 0)->cp, 0x0600u);
    ASSERT_EQ(cfr_get_cell(vt, 0, 0)->width, 1);
    ASSERT_NEQ(cfr_get_cell(vt, 0, 0)->grapheme_id, 0u);
    /* The 'x' never printed as its own cell. */
    ASSERT_EQ(cfr_get_cell(vt, 0, 1)->cp, 0u);
    ASSERT_EQ(vt->cursor.col, 1);
    cfr_free(vt);
}

static void test_run_declines_for_dec_graphics_charset(void)
{
    CfrTerm *vt = make_term(4, 10);
    feed(vt, "\x1B(0"
             "lqqk");
    ASSERT_EQ(cfr_get_cell(vt, 0, 0)->cp, 0x250Cu); /* ┌ */
    ASSERT_EQ(cfr_get_cell(vt, 0, 1)->cp, 0x2500u); /* ─ */
    ASSERT_EQ(cfr_get_cell(vt, 0, 2)->cp, 0x2500u);
    ASSERT_EQ(cfr_get_cell(vt, 0, 3)->cp, 0x2510u); /* ┐ */
    feed(vt, "\x1B(B");                             /* back to ASCII */
    feed(vt, "q");
    ASSERT_EQ(cfr_get_cell(vt, 0, 4)->cp, (uint32_t)'q');
    cfr_free(vt);
}

static void test_run_declines_for_insert_mode(void)
{
    CfrTerm *vt = make_term(4, 10);
    feed(vt, "ab");
    feed(vt, "\x1B[4h"); /* IRM on */
    feed(vt, "\x1B[1;1H");
    feed(vt, "XY");
    ASSERT_EQ(cfr_get_cell(vt, 0, 0)->cp, (uint32_t)'X');
    ASSERT_EQ(cfr_get_cell(vt, 0, 1)->cp, (uint32_t)'Y');
    ASSERT_EQ(cfr_get_cell(vt, 0, 2)->cp, (uint32_t)'a');
    ASSERT_EQ(cfr_get_cell(vt, 0, 3)->cp, (uint32_t)'b');
    cfr_free(vt);
}

static void test_autowrap_off_overwrites_the_margin(void)
{
    CfrTerm *vt = make_term(4, 5);
    feed(vt, "\x1B[?7l"); /* DECAWM off */
    feed(vt, "abcdefghijkl");
    ASSERT_EQ(vt->cursor.row, 0);
    ASSERT_EQ(vt->cursor.col, 4);
    ASSERT_EQ(cfr_get_cell(vt, 0, 0)->cp, (uint32_t)'a');
    ASSERT_EQ(cfr_get_cell(vt, 0, 4)->cp, (uint32_t)'l');
    ASSERT_EQ(cfr_get_cell(vt, 1, 0)->cp, 0u); /* nothing wrapped */
    cfr_free(vt);
}

static void test_run_over_a_wide_cluster_blanks_the_continuation(void)
{
    CfrTerm *vt = make_term(4, 10);
    feed(vt, "\xE4\xB8\xAD"); /* 中, width 2 at 0..1 */
    feed(vt, "\x1B[1;1H");
    /* A one-cell run over the lead must blank the stranded continuation
     * behind it, exactly as the per-codepoint path does. */
    feed(vt, "x");
    ASSERT_EQ(cfr_get_cell(vt, 0, 0)->cp, (uint32_t)'x');
    ASSERT_EQ(cfr_get_cell(vt, 0, 1)->cp, 0x20u); /* blanked continuation */
    ASSERT_EQ(cfr_get_cell(vt, 0, 1)->width, 1);
    /* The next print lands after the blank, not inside the old span. */
    feed(vt, "y");
    ASSERT_EQ(cfr_get_cell(vt, 0, 1)->cp, (uint32_t)'y');
    cfr_free(vt);
}

static void test_rep_repeats_the_run_tail(void)
{
    CfrTerm *vt = make_term(4, 10);
    feed(vt, "ab");
    feed(vt, "\x1B[3b"); /* REP 3: repeat the last printed char */
    ASSERT_EQ(cfr_get_cell(vt, 0, 0)->cp, (uint32_t)'a');
    ASSERT_EQ(cfr_get_cell(vt, 0, 1)->cp, (uint32_t)'b');
    ASSERT_EQ(cfr_get_cell(vt, 0, 2)->cp, (uint32_t)'b');
    ASSERT_EQ(cfr_get_cell(vt, 0, 3)->cp, (uint32_t)'b');
    ASSERT_EQ(cfr_get_cell(vt, 0, 4)->cp, (uint32_t)'b');
    ASSERT_EQ(vt->cursor.col, 5);
    cfr_free(vt);
}

static void test_scroll_at_bottom_pushes_run_rows_to_scrollback(void)
{
    CfrTerm *vt = make_term(2, 5);
    feed(vt, "abcdefghijkl"); /* 12 cells into 2 rows of 5 */
    /* "abcde" fills row 0, "fghij" row 1; the wrap onto "k" scrolls
     * "abcde" off. Screen: "fghij" / "kl". */
    ASSERT_EQ(cfr_get_scrollback_lines(vt), 1);
    ASSERT_EQ(cfr_get_scrollback_cell(vt, 0, 0)->cp, (uint32_t)'a');
    ASSERT_EQ(cfr_get_scrollback_cell(vt, 0, 4)->cp, (uint32_t)'e');
    ASSERT_EQ(cfr_get_cell(vt, 0, 0)->cp, (uint32_t)'f');
    ASSERT_EQ(cfr_get_cell(vt, 1, 0)->cp, (uint32_t)'k');
    ASSERT_EQ(cfr_get_cell(vt, 1, 1)->cp, (uint32_t)'l');
    ASSERT_EQ(vt->cursor.row, 1);
    ASSERT_EQ(vt->cursor.col, 2);
    cfr_free(vt);
}

/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);
    RUN_TEST(test_run_writes_cells_and_cursor);
    RUN_TEST(test_run_wraps_like_commit_cluster);
    RUN_TEST(test_run_resolves_the_phantom_across_writes);
    RUN_TEST(test_run_stops_at_a_wide_cluster);
    RUN_TEST(test_trailing_combining_mark_attaches_to_the_run);
    RUN_TEST(test_trailing_vs16_doubles_the_last_cell);
    RUN_TEST(test_run_declines_for_prepend_base);
    RUN_TEST(test_run_declines_for_dec_graphics_charset);
    RUN_TEST(test_run_declines_for_insert_mode);
    RUN_TEST(test_autowrap_off_overwrites_the_margin);
    RUN_TEST(test_run_over_a_wide_cluster_blanks_the_continuation);
    RUN_TEST(test_rep_repeats_the_run_tail);
    RUN_TEST(test_scroll_at_bottom_pushes_run_rows_to_scrollback);
    TEST_SUMMARY();
}
