/*
 * test_cfr_rep — tests for REP (CSI Ps b).
 *
 * REP repeats the last printed character Ps times (default 1).
 * If no character has been printed yet, REP is a no-op (matching xterm).
 *
 * ECMA-48 definition: "REP — Repeat the preceding graphic character
 * communication control or SPACE if no graphic character has been sent."
 * We follow xterm's interpretation: no-op if nothing printed yet (not SPACE).
 */

#include "test_helpers.h"

#include <coffer/coffer.h>
#include <string.h>

static CfrTerm *make_term(void)
{
    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.rows = 24;
    cfg.cols = 80;
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

/* Print 'X' then REP 3 → should produce 4 X's total (the original + 3). */
static void test_rep_basic(void)
{
    CfrTerm *vt = make_term();
    feed(vt, "X\x1b[3b");

    /* Cell 0: original X */
    const CfrCell *c0 = cfr_get_cell(vt, 0, 0);
    ASSERT_EQ(c0->cp, (uint32_t)'X');

    /* Cells 1-3: repeated X's */
    const CfrCell *c1 = cfr_get_cell(vt, 0, 1);
    ASSERT_EQ(c1->cp, (uint32_t)'X');
    const CfrCell *c2 = cfr_get_cell(vt, 0, 2);
    ASSERT_EQ(c2->cp, (uint32_t)'X');
    const CfrCell *c3 = cfr_get_cell(vt, 0, 3);
    ASSERT_EQ(c3->cp, (uint32_t)'X');

    /* Cell 4: should be empty */
    const CfrCell *c4 = cfr_get_cell(vt, 0, 4);
    ASSERT_EQ(c4->cp, 0u);

    cfr_free(vt);
}

/* REP with default parameter (Ps=1) repeats once. */
static void test_rep_default(void)
{
    CfrTerm *vt = make_term();
    feed(vt, "A\x1b[b");

    const CfrCell *c0 = cfr_get_cell(vt, 0, 0);
    ASSERT_EQ(c0->cp, (uint32_t)'A');
    const CfrCell *c1 = cfr_get_cell(vt, 0, 1);
    ASSERT_EQ(c1->cp, (uint32_t)'A');
    const CfrCell *c2 = cfr_get_cell(vt, 0, 2);
    ASSERT_EQ(c2->cp, 0u);

    cfr_free(vt);
}

/* REP before any printing → no-op. Grid is lazily allocated on first
 * print, so after a no-op REP the grid is still NULL. */
static void test_rep_no_prior_char(void)
{
    CfrTerm *vt = make_term();
    feed(vt, "\x1b[5b");

    /* Grid not allocated yet (no print happened) → no cells to check.
     * The key assertion is that we didn't crash. */
    ASSERT_NULL(cfr_get_cell(vt, 0, 0));

    cfr_free(vt);
}

/* REP repeats the *last* printed char, even after cursor movement.
 * Print Z at col 0 (cursor → col 1), move right 2 (cursor → col 3),
 * REP 2 → Z's at cols 3 and 4. */
static void test_rep_after_move(void)
{
    CfrTerm *vt = make_term();
    feed(vt, "Z\x1b[2C\x1b[2b");

    const CfrCell *c0 = cfr_get_cell(vt, 0, 0);
    ASSERT_EQ(c0->cp, (uint32_t)'Z');
    const CfrCell *c1 = cfr_get_cell(vt, 0, 1);
    ASSERT_EQ(c1->cp, 0u);
    const CfrCell *c2 = cfr_get_cell(vt, 0, 2);
    ASSERT_EQ(c2->cp, 0u);
    const CfrCell *c3 = cfr_get_cell(vt, 0, 3);
    ASSERT_EQ(c3->cp, (uint32_t)'Z');
    const CfrCell *c4 = cfr_get_cell(vt, 0, 4);
    ASSERT_EQ(c4->cp, (uint32_t)'Z');

    cfr_free(vt);
}

/* REP 0 → still repeats once (default behavior, matching xterm). */
static void test_rep_zero(void)
{
    CfrTerm *vt = make_term();
    feed(vt, "Q\x1b[0b");

    const CfrCell *c0 = cfr_get_cell(vt, 0, 0);
    ASSERT_EQ(c0->cp, (uint32_t)'Q');
    const CfrCell *c1 = cfr_get_cell(vt, 0, 1);
    ASSERT_EQ(c1->cp, (uint32_t)'Q');
    const CfrCell *c2 = cfr_get_cell(vt, 0, 2);
    ASSERT_EQ(c2->cp, 0u);

    cfr_free(vt);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    RUN_TEST(test_rep_basic);
    RUN_TEST(test_rep_default);
    RUN_TEST(test_rep_no_prior_char);
    RUN_TEST(test_rep_after_move);
    RUN_TEST(test_rep_zero);

    TEST_SUMMARY();
}
