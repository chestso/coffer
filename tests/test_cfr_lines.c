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
#include <stdlib.h>
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
    /* Grow back: the head fragment ('01234') is in scrollback, but the
     * line straddles the boundary, so reflow pulls the fragment back and
     * restores the whole 24-char line to a single row. */
    cfr_resize(vt, 4, 30);
    ASSERT_EQ(cfr_get_scrollback_lines(vt), 0);
    ASSERT_FALSE(cfr_row_is_continuation(vt, 1));
    ASSERT_EQ(cfr_get_cell(vt, 0, 0)->cp, (uint32_t)'0');
    ASSERT_EQ(cfr_get_cell(vt, 0, 23)->cp, (uint32_t)'3');
    /* The 5-col row's trailing blank is not content, so the line is
     * exactly 24 cells long. */
    ASSERT_EQ(cfr_get_cell(vt, 0, 24)->cp, 0u);
    /* The cursor was at the end of the line and the join moved the line
     * under it: it stays after the last cell. */
    ASSERT_EQ(vt->cursor.row, 0);
    ASSERT_EQ(vt->cursor.col, 24);
    cfr_free(vt);
}

/* A fragment spanning several scrollback rows is joined whole, and the
 * older history above it keeps the column width it was written at. */
static void test_reflow_joins_multirow_scrollback_fragment(void)
{
    CfrTerm *vt = make_term(4, 10, true);
    feed(vt, "M\r\n");                    /* a hard line, stays above */
    feed(vt, "012345678901234567890123"); /* 24 chars, one logical line */
    cfr_resize(vt, 4, 6);                 /* 1 + 4 rows of 6: 4 rows to sb */
    cfr_resize(vt, 4, 3);                 /* 1 + 8 rows of 3: 5 rows to sb */
    ASSERT_EQ(cfr_get_scrollback_lines(vt), 5);
    /* The four newest scrollback rows are the straddling line's own
     * fragments; 'M' sits below them and is not part of the line. */
    ASSERT_TRUE(cfr_row_is_continuation(vt, -3));
    ASSERT_FALSE(cfr_row_is_continuation(vt, -4));
    ASSERT_EQ(cfr_get_cell(vt, 0, 0)->cp, (uint32_t)'2');

    cfr_resize(vt, 4, 20); /* whole line: 20 + 4 cells */
    ASSERT_TRUE(cfr_row_is_continuation(vt, 1));
    ASSERT_FALSE(cfr_row_is_continuation(vt, 2));
    ASSERT_EQ(cfr_get_cell(vt, 0, 0)->cp, (uint32_t)'0');
    ASSERT_EQ(cfr_get_cell(vt, 0, 19)->cp, (uint32_t)'9');
    ASSERT_EQ(cfr_get_cell(vt, 1, 0)->cp, (uint32_t)'0');
    ASSERT_EQ(cfr_get_cell(vt, 1, 3)->cp, (uint32_t)'3');

    /* One row remains in history: 'M', still at the width it was pushed
     * at (6 cols) — the join rewrote only its own fragments. */
    ASSERT_EQ(cfr_get_scrollback_lines(vt), 1);
    ASSERT_EQ(cfr_get_cell(vt, -1, 0)->cp, (uint32_t)'M');
    ASSERT_NOT_NULL(cfr_get_cell(vt, -1, 5));
    ASSERT_NULL(cfr_get_cell(vt, -1, 6));
    cfr_free(vt);
}

/* Pop and push in the same pass: the joined fragment needs fewer rows
 * than history holds, but the line still overflows the grid, so part of
 * it goes back into the ring at the new width. */
static void test_reflow_join_repushes_scrollback(void)
{
    CfrTerm *vt = make_term(4, 10, true);
    /* 64 chars, index i printed as '0' + (i % 10). */
    feed(vt, "0123456789012345678901234567890123456789012345678901234567890123");
    cfr_resize(vt, 4, 5); /* 13 rows of 5: 9 to sb */
    ASSERT_EQ(cfr_get_scrollback_lines(vt), 9);

    cfr_resize(vt, 4, 8); /* 8 rows of 8: 4 to sb, 4 visible */
    ASSERT_EQ(cfr_get_scrollback_lines(vt), 4);
    ASSERT_TRUE(cfr_row_is_continuation(vt, 1));
    ASSERT_TRUE(cfr_row_is_continuation(vt, 2));
    ASSERT_TRUE(cfr_row_is_continuation(vt, 3));
    ASSERT_FALSE(cfr_row_is_continuation(vt, 4));
    /* Visible rows hold chars 32..63, history rows 0..31. */
    ASSERT_EQ(cfr_get_cell(vt, 0, 0)->cp, (uint32_t)'2');
    ASSERT_EQ(cfr_get_cell(vt, 3, 0)->cp, (uint32_t)'6');
    ASSERT_EQ(cfr_get_cell(vt, 3, 7)->cp, (uint32_t)'3');
    ASSERT_EQ(cfr_get_cell(vt, -1, 0)->cp, (uint32_t)'4');
    ASSERT_EQ(cfr_get_cell(vt, -4, 0)->cp, (uint32_t)'0');
    /* Re-pushed at 8 columns. */
    ASSERT_NOT_NULL(cfr_get_cell(vt, -1, 7));
    ASSERT_NULL(cfr_get_cell(vt, -1, 8));
    ASSERT_FALSE(cfr_row_is_continuation(vt, -4));
    ASSERT_TRUE(cfr_row_is_continuation(vt, -3));
    cfr_free(vt);
}

/* Page-scoped ids (style, grapheme cluster, hyperlink) survive every hop
 * a straddling line makes: ring page -> old grid -> ring page (re-push)
 * and ring page -> old grid -> new grid (visible). The first cell of the
 * line carries all three and lives in history for the shrunken widths. */
static void check_rich_cell(CfrTerm *vt, int row, int col)
{
    const CfrCell *cell = cfr_get_cell(vt, row, col);
    ASSERT_NOT_NULL(cell);
    ASSERT_EQ(cell->cp, (uint32_t)'e');

    uint32_t cps[4];
    ASSERT_EQ(cfr_cell_get_grapheme(vt, row, col, cps, 4), 2u);
    ASSERT_EQ(cps[1], 0x0301u);

    const CfrStyle *st = cfr_cell_style(vt, row, col);
    ASSERT_NOT_NULL(st);
    ASSERT_TRUE((st->attrs & CFR_ATTR_BOLD) != 0);
    ASSERT_EQ(st->fg_rgb, 0x010203u);

    uint8_t uri[64];
    size_t n = cfr_cell_get_hyperlink(vt, row, col, uri, sizeof(uri) - 1);
    uri[n] = '\0';
    ASSERT_STR_EQ((const char *)uri, "https://example.com/r");
}

static void test_reflow_straddle_carries_cell_attributes(void)
{
    CfrTerm *vt = make_term(4, 10, true);
    feed(vt, "\x1b]8;;https://example.com/r\x1b\\"
             "\x1b[1;38;2;1;2;3me\xcc\x81"
             "123456789012345678901234567890123456789"); /* 40 cells */
    ASSERT_TRUE(cfr_row_is_continuation(vt, 3));

    cfr_resize(vt, 4, 5); /* 8 rows of 5: the first four go to history */
    ASSERT_EQ(cfr_get_scrollback_lines(vt), 4);
    check_rich_cell(vt, -4, 0);

    cfr_resize(vt, 4, 8); /* 5 rows of 8: pop four, re-push one */
    ASSERT_EQ(cfr_get_scrollback_lines(vt), 1);
    check_rich_cell(vt, -1, 0);

    cfr_resize(vt, 4, 20); /* the whole line is visible again */
    ASSERT_EQ(cfr_get_scrollback_lines(vt), 0);
    check_rich_cell(vt, 0, 0);
    cfr_free(vt);
}

/* -------- sb_pushline: rows leaving the grid as a result of output -------
 *
 * Each row is announced as the grid row that is leaving, top to bottom, and
 * is readable there (through the ordinary accessors) during the call.
 * Resizes announce nothing: they rewrite history synchronously. */
typedef struct
{
    CfrTerm *vt;
    int pushes;
    int rows[32];
    uint32_t lineage[32];
    uint32_t cp0[32];
    bool same_as_newest[32]; /* row -1 held the same text during the call */
    bool rich_resolved;      /* a leaving 'e' cell resolved its bold style */
} Pushes;

static void record_push(int row, uint32_t lineage, void *user)
{
    Pushes *p = user;
    int i = p->pushes++;
    if (i >= 32)
        return;
    p->rows[i] = row;
    p->lineage[i] = lineage;
    const CfrCell *cell = cfr_get_cell(p->vt, row, 0);
    p->cp0[i] = cell ? cell->cp : 0xFFFFFFFFu;

    char here[64], newest[64];
    cfr_row_text(p->vt, row, here, sizeof(here));
    cfr_row_text(p->vt, -1, newest, sizeof(newest));
    p->same_as_newest[i] = strcmp(here, newest) == 0;

    if (cell && cell->cp == (uint32_t)'e') {
        const CfrStyle *st = cfr_cell_style(p->vt, row, 0);
        if (st && (st->attrs & CFR_ATTR_BOLD) != 0 && st->fg_rgb == 0x010203u)
            p->rich_resolved = true;
    }
}

static CfrTerm *make_pushes_term(Pushes *p, int rows, int cols, int scrollback)
{
    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.rows = rows;
    cfg.cols = cols;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 6;
    cfg.reflow = true;
    cfg.scrollback = scrollback;
    CfrTerm *vt = cfr_new(&cfg);
    memset(p, 0, sizeof(*p));
    p->vt = vt;
    CfrCallbacks cb = { 0 };
    cb.sb_pushline = record_push;
    cfr_set_callbacks(vt, &cb, p);
    return vt;
}

/* One call per row scrolled off by output, with the row readable at its
 * grid row, the row's style resolvable, and lineage joining wrapped rows. */
static void test_pushline_announces_scrolled_rows(void)
{
    Pushes p;
    CfrTerm *vt = make_pushes_term(&p, 3, 10, 100);

    feed(vt, "M\r\n");
    feed(vt, "\x1b[1;38;2;1;2;3me\x1b[0m"); /* a styled 'e' on row 1 */
    feed(vt, "123456789ABCDEF");            /* wraps: rows 1 and 2 */
    ASSERT_EQ(p.pushes, 0);                 /* nothing has scrolled yet */
    feed(vt, "\r\n\r\n");                   /* two scrolls: 'M', then "e12…" */
    ASSERT_EQ(p.pushes, 2);

    ASSERT_EQ(p.rows[0], 0);
    ASSERT_EQ(p.cp0[0], (uint32_t)'M');
    ASSERT_TRUE(p.same_as_newest[0]); /* also readable at row -1 */
    ASSERT_EQ(p.rows[1], 0);
    ASSERT_EQ(p.cp0[1], (uint32_t)'e');
    ASSERT_TRUE(p.same_as_newest[1]);
    ASSERT_TRUE(p.rich_resolved);
    ASSERT_TRUE(p.lineage[0] != p.lineage[1]);

    feed(vt, "\r\n"); /* the wrapped tail "6789ABCDEF" leaves: same line */
    ASSERT_EQ(p.pushes, 3);
    ASSERT_EQ(p.lineage[2], p.lineage[1]);
    ASSERT_TRUE(p.lineage[1] != 0);
    cfr_free(vt);
}

/* Several rows leave at once (SU): announced top to bottom, each still in
 * place at its own grid row. */
static void test_pushline_announces_a_multi_row_scroll(void)
{
    Pushes p;
    CfrTerm *vt = make_pushes_term(&p, 4, 10, 100);
    feed(vt, "a\r\nb\r\nc\r\nd");
    feed(vt, "\x1b[3S"); /* scroll up three rows */
    ASSERT_EQ(p.pushes, 3);
    for (int i = 0; i < 3; ++i) {
        ASSERT_EQ(p.rows[i], i);
        ASSERT_EQ(p.cp0[i], (uint32_t)('a' + i));
    }
    ASSERT_EQ(cfr_get_scrollback_lines(vt), 3);
    cfr_free(vt);
}

/* The announcement does not depend on the engine keeping the row: a host
 * owning its own history works with scrollback off. */
static void test_pushline_fires_with_scrollback_off(void)
{
    Pushes p;
    CfrTerm *vt = make_pushes_term(&p, 2, 10, 0);
    feed(vt, "a\r\nb\r\nc\r\n");
    ASSERT_EQ(cfr_get_scrollback_lines(vt), 0);
    ASSERT_EQ(p.pushes, 2);
    ASSERT_EQ(p.rows[0], 0);
    ASSERT_EQ(p.cp0[0], (uint32_t)'a'); /* still readable at its grid row */
    ASSERT_EQ(p.cp0[1], (uint32_t)'b');
    ASSERT_NULL(cfr_get_cell(vt, -1, 0));
    cfr_free(vt);
}

/* A resize rewrites history (rows pushed, rows pulled back for a rewrap)
 * but announces nothing: callbacks describe output. */
static void test_resize_announces_nothing(void)
{
    Pushes p;
    CfrTerm *vt = make_pushes_term(&p, 4, 10, 100);
    feed(vt, "012345678901234567890123456789012345678901234567890123456789");
    ASSERT_EQ(p.pushes, 2); /* the line scrolled two rows off as it wrapped */

    cfr_resize(vt, 4, 5); /* shrink: rows go to history */
    ASSERT_TRUE(cfr_get_scrollback_lines(vt) > 2);
    cfr_resize(vt, 4, 8); /* straddling line pulled back, part re-pushed */
    cfr_resize(vt, 4, 30);
    ASSERT_EQ(p.pushes, 2);
    cfr_free(vt);
}

/* One reflow pulls back at most CFR_REFLOW_PREFIX_ROWS history rows of the
 * straddling line, however long the line is. The rows beyond the cap keep
 * their old width and still join the rewritten ones: the text reads back
 * unbroken and every row continues the one before it. */
static void test_reflow_prefix_is_capped(void)
{
    enum
    {
        COLS = 10,
        EXTRA = 300,
        LINE_ROWS = CFR_REFLOW_PREFIX_ROWS + EXTRA
    };
    CfrTerm *vt = make_term(4, COLS, true);
    cfr_set_scrollback_size(vt, 10 * LINE_ROWS);

    /* One logical line of LINE_ROWS rows; char i is 'a' + i % 26. */
    char *line = malloc((size_t)LINE_ROWS * COLS + 1);
    ASSERT_NOT_NULL(line);
    for (int i = 0; i < LINE_ROWS * COLS; ++i)
        line[i] = (char)('a' + i % 26);
    line[LINE_ROWS * COLS] = '\0';
    feed(vt, line);
    int sb_before = cfr_get_scrollback_lines(vt);
    ASSERT_EQ(sb_before, LINE_ROWS - 4);

    cfr_resize(vt, 4, 2 * COLS); /* twice as wide: half as many rows */

    /* The pulled rows (cap rows of history + the 4 visible ones) re-emit
     * at 20 columns; all but the last 4 rows of that go back to history. */
    int rewritten = (CFR_REFLOW_PREFIX_ROWS * COLS + 4 * COLS) / (2 * COLS) - 4;
    int untouched = sb_before - CFR_REFLOW_PREFIX_ROWS;
    int sb = cfr_get_scrollback_lines(vt);
    ASSERT_EQ(sb, untouched + rewritten);
    for (int s = 0; s < sb; ++s) {
        int cols = 0;
        ASSERT_NOT_NULL(cfr_get_row(vt, -(s + 1), &cols));
        ASSERT_EQ(cols, s < rewritten ? 2 * COLS : COLS);
    }

    /* Unbroken join across the cap boundary, oldest row to last visible. */
    for (int s = 1; s < sb; ++s)
        ASSERT_TRUE(cfr_row_is_continuation(vt, -s));
    ASSERT_TRUE(cfr_row_is_continuation(vt, 0));
    int idx = 0;
    for (int s = sb - 1; s >= 0; --s) {
        int cols = 0;
        const CfrCell *row = cfr_get_row(vt, -(s + 1), &cols);
        for (int c = 0; c < cols; ++c, ++idx)
            ASSERT_EQ(row[c].cp, (uint32_t)line[idx]);
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 2 * COLS; ++c, ++idx)
            ASSERT_EQ(cfr_get_cell(vt, r, c)->cp, (uint32_t)line[idx]);
    ASSERT_EQ(idx, LINE_ROWS * COLS);
    free(line);
    cfr_free(vt);
}

/* Scrollback disabled: nothing can be pulled back, so the visible
 * remainder re-emits on its own (a row that would have gone to history is
 * dropped, as before). */
static void test_reflow_without_scrollback(void)
{
    CfrTerm *vt = make_term(4, 10, true);
    cfr_set_scrollback_size(vt, 0);
    feed(vt, "012345678901234567890123");
    cfr_resize(vt, 4, 5);
    ASSERT_EQ(cfr_get_scrollback_lines(vt), 0);
    ASSERT_EQ(cfr_get_cell(vt, 0, 0)->cp, (uint32_t)'5');

    cfr_resize(vt, 4, 30);
    ASSERT_EQ(cfr_get_scrollback_lines(vt), 0);
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
 *     they were written at, and the scrollback line count is unchanged
 *     by the grow.
 *
 * Part 2 holds because nothing straddles the boundary in this fixture:
 * the top visible row is the start of line C, a hard line break, so
 * there is no scrollback fragment joined to it. A line whose head is in
 * history is pulled back and re-emitted whole — see
 * test_reflow_joins_scrollback_fragment.
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
    ASSERT_NOT_NULL(cfr_get_cell(vt, -1, 9));
    ASSERT_NULL(cfr_get_cell(vt, -1, 10));

    cfr_free(vt);
}

/* Hyperlink ids are page-scoped, so every cross-page cell move must
 * re-intern the URI. Both resize paths used to copy the raw id: reflow's
 * visible-row loop and the clamp resize interned style + grapheme only,
 * so a link silently vanished (or, with a colliding id, changed target)
 * on any resize. */
static void test_resize_carries_hyperlink(void)
{
    const char *uri = "https://example.com/a";
    uint8_t buf[64];

    CfrTerm *vt = make_term(6, 10, true);
    feed(vt, "\x1b]8;;https://example.com/a\x1b\\0123456789012345\x1b]8;;\x1b\\");
    ASSERT_TRUE(cfr_row_is_continuation(vt, 1));

    /* Reflow: shrink, then grow back. */
    cfr_resize(vt, 6, 5);
    size_t n = cfr_cell_get_hyperlink(vt, 0, 0, buf, sizeof(buf) - 1);
    buf[n] = '\0';
    ASSERT_EQ(n, strlen(uri));
    ASSERT_STR_EQ((const char *)buf, uri);

    cfr_resize(vt, 6, 40);
    n = cfr_cell_get_hyperlink(vt, 0, 0, buf, sizeof(buf) - 1);
    buf[n] = '\0';
    ASSERT_STR_EQ((const char *)buf, uri);
    /* A cell outside the link run still has no link. */
    ASSERT_EQ(cfr_cell_get_hyperlink(vt, 0, 30, buf, sizeof(buf)),
              0u);

    /* Clamp resize (reflow disabled) takes the other path. */
    cfr_set_reflow(vt, false);
    cfr_resize(vt, 6, 20);
    n = cfr_cell_get_hyperlink(vt, 0, 0, buf, sizeof(buf) - 1);
    buf[n] = '\0';
    ASSERT_STR_EQ((const char *)buf, uri);

    /* And the scrollback push path, which used to read the URI through a
     * fixed 2 KiB stack buffer. */
    cfr_set_reflow(vt, true);
    cfr_resize(vt, 6, 10);
    feed(vt, "\x1b[6;1H\n\n"); /* two rows scroll off; the link is in sb */
    ASSERT_EQ(cfr_get_scrollback_lines(vt), 2);
    ASSERT_EQ(cfr_get_cell(vt, -2, 0)->cp, (uint32_t)'0');
    n = cfr_cell_get_hyperlink(vt, -2, 0, buf, sizeof(buf) - 1);
    buf[n] = '\0';
    ASSERT_STR_EQ((const char *)buf, uri);
    cfr_free(vt);
}

/* The pop contract reflow relies on: rows come off the newest end, the
 * oldest survive, an emptied page leaves the ring consistent, and a
 * later push reuses the freed slot. */
static void test_scrollback_pop_rows(void)
{
    CfrTerm *vt = make_term(2, 10, true);
    feed(vt, "a\r\nb\r\nc\r\nd\r\n");
    ASSERT_EQ(cfr_get_scrollback_lines(vt), 3);
    ASSERT_EQ(cfr_get_cell(vt, -1, 0)->cp, (uint32_t)'c');
    ASSERT_EQ(cfr_get_cell(vt, -3, 0)->cp, (uint32_t)'a');

    cfr_scrollback_pop_rows(vt, 2);
    ASSERT_EQ(cfr_get_scrollback_lines(vt), 1);
    ASSERT_EQ(cfr_get_cell(vt, -1, 0)->cp, (uint32_t)'a');

    /* Over-popping clamps; the ring empties cleanly. */
    cfr_scrollback_pop_rows(vt, 5);
    ASSERT_EQ(cfr_get_scrollback_lines(vt), 0);

    feed(vt, "e\r\n");
    ASSERT_EQ(cfr_get_scrollback_lines(vt), 1);
    ASSERT_EQ(cfr_get_cell(vt, -1, 0)->cp, (uint32_t)'d');
    cfr_free(vt);
}

/* A logical line whose head is already in scrollback is rewrapped as a
 * whole: the head fragment is pulled back out of the ring, joined with
 * the visible remainder, and re-emitted. Before this, the fragment was
 * re-emitted as its own line ("5"-"3" split from "01234"). */
static void test_reflow_joins_scrollback_fragment(void)
{
    CfrTerm *vt = make_term(4, 10, true);
    feed(vt, "012345678901234567890123"); /* one 24-char logical line */
    cfr_resize(vt, 4, 5);                 /* 5 rows of 5; oldest -> sb */
    ASSERT_EQ(cfr_get_scrollback_lines(vt), 1);
    ASSERT_EQ(cfr_get_cell(vt, 0, 0)->cp, (uint32_t)'5');

    cfr_resize(vt, 4, 30); /* grow: the whole line fits one row again */
    ASSERT_EQ(cfr_get_scrollback_lines(vt), 0);
    ASSERT_EQ(cfr_get_cell(vt, 0, 0)->cp, (uint32_t)'0');
    ASSERT_EQ(cfr_get_cell(vt, 0, 23)->cp, (uint32_t)'3');
    ASSERT_FALSE(cfr_row_is_continuation(vt, 1));
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
    RUN_TEST(test_resize_carries_hyperlink);
    RUN_TEST(test_scrollback_pop_rows);
    RUN_TEST(test_reflow_joins_scrollback_fragment);
    RUN_TEST(test_reflow_joins_multirow_scrollback_fragment);
    RUN_TEST(test_reflow_join_repushes_scrollback);
    RUN_TEST(test_reflow_straddle_carries_cell_attributes);
    RUN_TEST(test_pushline_announces_scrolled_rows);
    RUN_TEST(test_pushline_announces_a_multi_row_scroll);
    RUN_TEST(test_pushline_fires_with_scrollback_off);
    RUN_TEST(test_resize_announces_nothing);
    RUN_TEST(test_reflow_prefix_is_capped);
    RUN_TEST(test_reflow_without_scrollback);
    TEST_SUMMARY();
}
