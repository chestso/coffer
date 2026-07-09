/*
 * test_cfr_altscreen — regression tests for alt-screen save/restore,
 * especially the grid-dimension mismatch that occurs when the terminal
 * is resized while on the alt screen.
 *
 * Motivating crash: leaving alt screen after a resize restored a grid
 * allocated at the old (smaller) dimensions while vt->cols reflected the
 * new (larger) size.  cfr_get_cell computes cells[row * vt->cols + col],
 * overflowing the page's cells array → heap-buffer-overflow (ASan) or
 * memory corruption.
 *
 * Second regression: the initial fix used cfr_resize_clamp (truncate)
 * instead of cfr_resize (reflow), so lines carried from the old width
 * were cut off instead of re-wrapped.
 */

#include "coffer_internal.h"
#include "test_helpers.h"

#include <coffer/coffer.h>
#include <stdio.h>
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
    cfr_set_scrollback_size(vt, 100);
    cfr_set_reflow(vt, true);
    return vt;
}

static void feed(CfrTerm *vt, const char *s)
{
    cfr_input_write(vt, (const uint8_t *)s, strlen(s));
}

/* ------------------------------------------------------------------ */
/* 1. Basic alt-screen enter/leave                                    */
/* ------------------------------------------------------------------ */

static void test_altscreen_enter_leave(void)
{
    CfrTerm *vt = make_term(24, 80);

    /* Write to main screen. */
    feed(vt, "main");
    const CfrCell *c = cfr_get_cell(vt, 0, 0);
    ASSERT_NOT_NULL(c);
    ASSERT_EQ(c->cp, (uint32_t)'m');

    /* Enter alt screen (DECSET 1049). */
    feed(vt, "\x1b[?1049h");
    ASSERT_TRUE(cfr_is_altscreen(vt));

    /* Alt screen should be blank. */
    c = cfr_get_cell(vt, 0, 0);
    ASSERT_NOT_NULL(c);
    ASSERT_EQ(c->cp, 0u);

    /* Write to alt screen. */
    feed(vt, "alt");
    c = cfr_get_cell(vt, 0, 0);
    ASSERT_EQ(c->cp, (uint32_t)'a');

    /* Leave alt screen (DECRST 1049). */
    feed(vt, "\x1b[?1049l");
    ASSERT_FALSE(cfr_is_altscreen(vt));

    /* Main screen content must be restored. */
    c = cfr_get_cell(vt, 0, 0);
    ASSERT_NOT_NULL(c);
    ASSERT_EQ(c->cp, (uint32_t)'m');

    cfr_free(vt);
}

/* ------------------------------------------------------------------ */
/* 2. Resize while on alt screen — the crash bug                      */
/*    Grid allocated at 10 cols, vt->cols = 20 after resize.           */
/*    cfr_get_cell(vt, 0, 19) used to overflow.                      */
/* ------------------------------------------------------------------ */

static void test_resize_on_altscreen_no_overflow(void)
{
    CfrTerm *vt = make_term(24, 10);

    /* Write a distinctive pattern to the main screen. */
    feed(vt, "ABCDEFGHIJ");

    /* Enter alt screen. */
    feed(vt, "\x1b[?1049h");
    ASSERT_TRUE(cfr_is_altscreen(vt));

    /* Resize wider while on alt screen. */
    cfr_resize(vt, 24, 20);

    /* Leave alt screen — this is where the crash happened. */
    feed(vt, "\x1b[?1049l");
    ASSERT_FALSE(cfr_is_altscreen(vt));

    /* The main grid must now be valid for the wider geometry.
     * Before the fix, cfr_get_cell with col >= 10 read past the
     * old page's cells allocation. */
    int rows, cols;
    cfr_get_dimensions(vt, &rows, &cols);
    ASSERT_EQ(rows, 24);
    ASSERT_EQ(cols, 20);

    /* Every cell in the grid must be safely readable. */
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            const CfrCell *cell = cfr_get_cell(vt, r, c);
            ASSERT_NOT_NULL(cell);
        }
    }

    /* The original content should have been reflowed into the wider
     * grid.  "ABCDEFGHIJ" fits in one row at 20 cols, so row 0 cols
     * 0-9 should carry the letters. */
    for (int i = 0; i < 10; i++) {
        const CfrCell *cell = cfr_get_cell(vt, 0, i);
        ASSERT_NOT_NULL(cell);
        ASSERT_EQ(cell->cp, (uint32_t)('A' + i));
    }

    cfr_free(vt);
}

/* ------------------------------------------------------------------ */
/* 3. Resize narrower on alt screen — same class of bug               */
/*    Grid allocated at 20 cols, shrink to 10 after resize.           */
/* ------------------------------------------------------------------ */

static void test_resize_narrower_on_altscreen(void)
{
    CfrTerm *vt = make_term(24, 20);

    feed(vt, "ABCDEFGHIJKLMNOPQRST");
    feed(vt, "\x1b[?1049h");
    ASSERT_TRUE(cfr_is_altscreen(vt));

    /* Resize narrower while on alt screen. */
    cfr_resize(vt, 24, 10);

    feed(vt, "\x1b[?1049l");
    ASSERT_FALSE(cfr_is_altscreen(vt));

    int rows, cols;
    cfr_get_dimensions(vt, &rows, &cols);
    ASSERT_EQ(rows, 24);
    ASSERT_EQ(cols, 10);

    /* Every cell must be readable. */
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            const CfrCell *cell = cfr_get_cell(vt, r, c);
            ASSERT_NOT_NULL(cell);
        }
    }

    cfr_free(vt);
}

/* ------------------------------------------------------------------ */
/* 4. Reflow, not truncation — content must re-wrap after resize       */
/*    A 20-char line in a 20-col grid, resized to 10 cols on alt      */
/*    screen, then returned: the content must wrap across two rows     */
/*    instead of being clamped to the first 10 chars.                 */
/*                                                                     */
/*    With scrollback enabled, the 1-row-at-20-cols line wraps to     */
/*    2 rows at 10 cols.  Verify that 'A' is on row 0 and 'K'       */
/*    appears somewhere (row 1 or scrollback) — the key assertion is   */
/*    that the content was not simply truncated (row 0 cols 10-19     */
/*    go blank).                                                       */
/* ------------------------------------------------------------------ */

static void test_altscreen_resize_reflows(void)
{
    CfrTerm *vt = make_term(5, 20);

    /* Write 20 characters — fits one row at 20 cols. */
    feed(vt, "ABCDEFGHIJKLMNOPQRST");

    /* Enter alt screen, resize narrower, exit alt screen. */
    feed(vt, "\x1b[?1049h");
    cfr_resize(vt, 5, 10);
    feed(vt, "\x1b[?1049l");

    int rows, cols;
    cfr_get_dimensions(vt, &rows, &cols);
    ASSERT_EQ(cols, 10);

    /* 'K' (the 11th character) must be present in the grid or recent
     * scrollback — it cannot have been silently dropped by truncation. */
    bool found_k = false;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            const CfrCell *cell = cfr_get_cell(vt, r, c);
            ASSERT_NOT_NULL(cell);
            if (cell->cp == (uint32_t)'K') {
                found_k = true;
            }
        }
    }
    /* If not on screen, check scrollback (wrapped rows may have pushed
     * earlier content up). */
    if (!found_k) {
        int sb_lines = cfr_get_scrollback_lines(vt);
        for (int sb = 0; sb < sb_lines && !found_k; sb++) {
            for (int c = 0; c < cols; c++) {
                const CfrCell *cell = cfr_get_scrollback_cell(vt, sb, c);
                if (cell && cell->cp == (uint32_t)'K') {
                    found_k = true;
                    break;
                }
            }
        }
    }
    ASSERT_TRUE(found_k);

    /* 'A' must also survive. */
    bool found_a = false;
    for (int r = 0; r < rows && !found_a; r++) {
        for (int c = 0; c < cols && !found_a; c++) {
            const CfrCell *cell = cfr_get_cell(vt, r, c);
            if (cell && cell->cp == (uint32_t)'A')
                found_a = true;
        }
    }
    ASSERT_TRUE(found_a);

    cfr_free(vt);
}

/* ------------------------------------------------------------------ */
/* 5. No stale mismatch when dimensions haven't changed               */
/*    Enter/leave alt screen without any resize — must be a no-op.    */
/* ------------------------------------------------------------------ */

static void test_altscreen_no_resize_is_noop(void)
{
    CfrTerm *vt = make_term(24, 80);

    feed(vt, "Hello");
    feed(vt, "\x1b[?1049h");
    feed(vt, "alt_content");
    feed(vt, "\x1b[?1049l");

    /* Main screen content must be intact. */
    const CfrCell *c = cfr_get_cell(vt, 0, 0);
    ASSERT_NOT_NULL(c);
    ASSERT_EQ(c->cp, (uint32_t)'H');

    cfr_free(vt);
}

/* ------------------------------------------------------------------ */
/* 6. Double resize on alt screen                                      */
/*    Enter at 10 cols, resize to 20, then to 30, exit.                */
/* ------------------------------------------------------------------ */

static void test_double_resize_on_altscreen(void)
{
    CfrTerm *vt = make_term(24, 10);

    feed(vt, "ABCDEFGHIJ");
    feed(vt, "\x1b[?1049h");

    cfr_resize(vt, 24, 20);
    cfr_resize(vt, 24, 30);

    feed(vt, "\x1b[?1049l");

    int rows, cols;
    cfr_get_dimensions(vt, &rows, &cols);
    ASSERT_EQ(cols, 30);

    /* Must not crash — every cell readable. */
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            const CfrCell *cell = cfr_get_cell(vt, r, c);
            ASSERT_NOT_NULL(cell);
        }
    }

    /* Content reflowed into 30-col grid: "ABCDEFGHIJ" on row 0 cols 0-9. */
    const CfrCell *c = cfr_get_cell(vt, 0, 0);
    ASSERT_NOT_NULL(c);
    ASSERT_EQ(c->cp, (uint32_t)'A');

    c = cfr_get_cell(vt, 0, 9);
    ASSERT_NOT_NULL(c);
    ASSERT_EQ(c->cp, (uint32_t)'J');

    cfr_free(vt);
}

/* ------------------------------------------------------------------ */
/* 7. Row mismatch — resize adds rows while on alt screen             */
/* ------------------------------------------------------------------ */

static void test_altscreen_resize_rows(void)
{
    CfrTerm *vt = make_term(10, 80);

    feed(vt, "X");
    feed(vt, "\x1b[?1049h");

    /* Resize to more rows. */
    cfr_resize(vt, 30, 80);

    feed(vt, "\x1b[?1049l");

    int rows, cols;
    cfr_get_dimensions(vt, &rows, &cols);
    ASSERT_EQ(rows, 30);
    ASSERT_EQ(cols, 80);

    /* Every cell must be safely readable, including the new rows. */
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            const CfrCell *cell = cfr_get_cell(vt, r, c);
            ASSERT_NOT_NULL(cell);
        }
    }

    /* Original content preserved. */
    const CfrCell *c = cfr_get_cell(vt, 0, 0);
    ASSERT_NOT_NULL(c);
    ASSERT_EQ(c->cp, (uint32_t)'X');

    cfr_free(vt);
}

/* ------------------------------------------------------------------ */
/* 8. Alt screen content must not accumulate across enter/exit cycles  */
/*    Each entry must start with a blank alt grid.  Before the fix,    */
/*    the altgrid was reused without clearing, so content from the     */
/*    previous alt-screen session persisted into the next one.         */
/* ------------------------------------------------------------------ */

static void test_altscreen_no_content_accumulation(void)
{
    CfrTerm *vt = make_term(24, 80);

    /* Cycle 1: enter alt, write content at the bottom (like less does),
     * then exit.  less moves cursor to the bottom row and writes upward
     * via scroll; it does NOT send a clear-screen on entry. */
    feed(vt, "\x1b[?1049h");
    feed(vt, "\x1b[24;1H"); /* move to last row */
    feed(vt, "FIRST\r\n");  /* writes + scroll */
    feed(vt, "\x1b[?1049l");

    /* Cycle 2: enter alt again.  The altgrid must be blank — if the
     * previous content persists, old text appears below the new. */
    feed(vt, "\x1b[?1049h");
    ASSERT_TRUE(cfr_is_altscreen(vt));

    /* Check that "FIRST" is not lingering in the grid. */
    bool found_first = false;
    int rows, cols;
    cfr_get_dimensions(vt, &rows, &cols);
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            const CfrCell *cell = cfr_get_cell(vt, r, c);
            if (cell && cell->cp == (uint32_t)'F') {
                /* Check if this is "FIRST" */
                const CfrCell *n1 = cfr_get_cell(vt, r, c + 1);
                const CfrCell *n2 = cfr_get_cell(vt, r, c + 2);
                if (n1 && n1->cp == (uint32_t)'I' &&
                    n2 && n2->cp == (uint32_t)'R') {
                    found_first = true;
                }
            }
        }
    }
    ASSERT_FALSE(found_first);

    feed(vt, "\x1b[?1049l");

    /* Cycle 3: same check. */
    feed(vt, "\x1b[?1049h");
    found_first = false;
    cfr_get_dimensions(vt, &rows, &cols);
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            const CfrCell *cell = cfr_get_cell(vt, r, c);
            if (cell && cell->cp == (uint32_t)'F') {
                const CfrCell *n1 = cfr_get_cell(vt, r, c + 1);
                const CfrCell *n2 = cfr_get_cell(vt, r, c + 2);
                if (n1 && n1->cp == (uint32_t)'I' &&
                    n2 && n2->cp == (uint32_t)'R') {
                    found_first = true;
                }
            }
        }
    }
    ASSERT_FALSE(found_first);

    feed(vt, "\x1b[?1049l");

    cfr_free(vt);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    RUN_TEST(test_altscreen_enter_leave);
    RUN_TEST(test_resize_on_altscreen_no_overflow);
    RUN_TEST(test_resize_narrower_on_altscreen);
    RUN_TEST(test_altscreen_resize_reflows);
    RUN_TEST(test_altscreen_no_resize_is_noop);
    RUN_TEST(test_double_resize_on_altscreen);
    RUN_TEST(test_altscreen_resize_rows);
    RUN_TEST(test_altscreen_no_content_accumulation);

    TEST_SUMMARY();
}
