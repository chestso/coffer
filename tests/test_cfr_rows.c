/* tests/test_cfr_rows.c — unified row addressing and scrollback storage.
 *
 * Rows are addressed by one scheme everywhere: row >= 0 is a visible row,
 * row < 0 is scrollback depth (-1 = the line most recently scrolled off).
 * A scrollback row keeps the width it was written at, and the page-scoped
 * ids in its cells (style, cluster, link) resolve only through the
 * (row, col) accessors. */

#include "coffer_internal.h"
#include "test_helpers.h"
#include <coffer/coffer.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static CfrTerm *make_term(int rows, int cols, int scrollback)
{
    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.rows = rows;
    cfg.cols = cols;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 6;
    cfg.reflow = true;
    cfg.scrollback = scrollback;
    return cfr_new(&cfg);
}

static void feed(CfrTerm *vt, const char *s)
{
    cfr_input_write(vt, (const uint8_t *)s, strlen(s));
}

/* Row -1 is the newest scrollback row, row -sb the oldest, and anything
 * beyond either end, or past the visible rows, does not exist. */
static void test_unified_rows_bounds(void)
{
    CfrTerm *vt = make_term(3, 8, 100);
    feed(vt, "a\r\nb\r\nc\r\nd\r\ne");
    int sb = cfr_get_scrollback_lines(vt);
    ASSERT_EQ(sb, 2);

    ASSERT_EQ(cfr_get_cell(vt, -1, 0)->cp, (uint32_t)'b'); /* newest */
    ASSERT_EQ(cfr_get_cell(vt, -2, 0)->cp, (uint32_t)'a'); /* oldest */
    ASSERT_NULL(cfr_get_cell(vt, -3, 0));
    ASSERT_EQ(cfr_get_cell(vt, 0, 0)->cp, (uint32_t)'c');
    ASSERT_EQ(cfr_get_cell(vt, 2, 0)->cp, (uint32_t)'e');
    ASSERT_NULL(cfr_get_cell(vt, 3, 0));
    ASSERT_NULL(cfr_get_cell(vt, 0, -1));
    ASSERT_NULL(cfr_get_cell(vt, 0, 8));
    ASSERT_NULL(cfr_get_cell(vt, -1, 8));

    int cols = 0;
    const CfrCell *row = cfr_get_row(vt, -1, &cols);
    ASSERT_NOT_NULL(row);
    ASSERT_EQ(cols, 8);
    ASSERT_TRUE(row == cfr_get_cell(vt, -1, 0));
    ASSERT_TRUE(row + 5 == cfr_get_cell(vt, -1, 5));
    ASSERT_NULL(cfr_get_row(vt, -3, &cols));
    ASSERT_NULL(cfr_get_row(vt, 3, &cols));
    cfr_free(vt);
}

/* History keeps the width it was written at: the column bound is the
 * row's own, not the grid's. */
static void test_row_width_is_per_row(void)
{
    CfrTerm *vt = make_term(3, 10, 100);
    feed(vt, "0123456789\r\nabcdefghij\r\nXYZ\r\n\r\n\r\n");
    int sb = cfr_get_scrollback_lines(vt);
    ASSERT_TRUE(sb >= 2);
    cfr_resize(vt, 3, 20); /* reflow leaves the old rows at 10 columns */

    int cols = 0;
    ASSERT_NOT_NULL(cfr_get_row(vt, -sb, &cols));
    ASSERT_EQ(cols, 10);
    ASSERT_NOT_NULL(cfr_get_cell(vt, -sb, 9));
    ASSERT_NULL(cfr_get_cell(vt, -sb, 10));

    ASSERT_NOT_NULL(cfr_get_row(vt, 0, &cols));
    ASSERT_EQ(cols, 20);
    ASSERT_NOT_NULL(cfr_get_cell(vt, 0, 19));
    cfr_free(vt);
}

/* Every page numbers its links (and styles) from 1, so the same id names
 * different links on different rows; only (row, col) resolves them. Lines
 * span several 64-row pages. */
static void test_page_scoped_ids_resolve_per_row(void)
{
    enum
    {
        LINES = 200
    };
    CfrTerm *vt = make_term(4, 40, 1000);
    char buf[160];
    for (int i = 0; i < LINES; ++i) {
        snprintf(buf, sizeof(buf),
                 "\x1b]8;;https://example.com/%d\x1b\\"
                 "\x1b[38;2;%d;0;0mL%d\x1b]8;;\x1b\\\x1b[0m\r\n",
                 i, i % 256, i);
        feed(vt, buf);
    }
    int sb = cfr_get_scrollback_lines(vt);
    ASSERT_TRUE(sb > 3 * (int)CFR_SB_PAGE_ROWS);

    bool id_collision = false;
    for (int s = 0; s < sb; ++s) {
        int row = -(s + 1);
        int line = LINES - 1 - s - 3; /* 4 visible rows hold the tail */
        if (line < 0 || line >= LINES)
            continue;
        char want[64];
        snprintf(want, sizeof(want), "https://example.com/%d", line);

        uint8_t uri[64];
        size_t n = cfr_cell_get_hyperlink(vt, row, 0, uri, sizeof(uri) - 1);
        uri[n] = '\0';
        ASSERT_STR_EQ((const char *)uri, want);

        const CfrStyle *st = cfr_cell_style(vt, row, 0);
        ASSERT_NOT_NULL(st);
        ASSERT_EQ(st->fg_rgb, (uint32_t)(line % 256) << 16);

        /* The page-scoped id repeats across pages for different links. */
        if (s >= (int)CFR_SB_PAGE_ROWS) {
            const CfrCell *a = cfr_get_cell(vt, row, 0);
            const CfrCell *b = cfr_get_cell(vt, row + (int)CFR_SB_PAGE_ROWS, 0);
            if (a->hyperlink_id == b->hyperlink_id)
                id_collision = true;
        }
    }
    ASSERT_TRUE(id_collision);
    cfr_free(vt);
}

/* History written at 80 columns and then outlived by a 40-column grid:
 * every consumer must read those rows at their own width. Ten lines of
 * "xxxx…x END-n" (66 columns used) scroll into a 4-row grid; after the
 * shrink the oldest seven rows are still 80 wide. */
static CfrTerm *make_wide_history(int *out_sb)
{
    CfrTerm *vt = make_term(4, 80, 1000);
    char line[96];
    for (int n = 0; n < 10; ++n) {
        snprintf(line, sizeof(line), "%s END-%d\r\n",
                 "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", n);
        feed(vt, line);
    }
    cfr_resize(vt, 4, 40);
    *out_sb = cfr_get_scrollback_lines(vt);
    return vt;
}

static void test_selection_text_reads_rows_at_their_width(void)
{
    int sb = 0;
    CfrTerm *vt = make_wide_history(&sb);
    int cols = 0;
    ASSERT_NOT_NULL(cfr_get_row(vt, -sb, &cols));
    ASSERT_EQ(cols, 80);

    /* The three oldest rows, whole: 66 characters each, joined by hard
     * line breaks — not cut at the grid's 40 columns. */
    cfr_selection_start(vt, -sb, 0, CFR_SEL_CHAR);
    cfr_selection_update(vt, -(sb - 2), 79);
    char *text = cfr_selection_get_text(vt);
    ASSERT_NOT_NULL(text);
    char want[3 * 80];
    size_t pos = 0;
    for (int n = 0; n < 3; ++n)
        pos += (size_t)snprintf(want + pos, sizeof(want) - pos, "%s%s END-%d",
                                n ? "\n" : "",
                                "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
                                n);
    ASSERT_STR_EQ(text, want);
    free(text);

    /* Triple-click selects the row to its own last column. */
    cfr_selection_start(vt, -sb, 3, CFR_SEL_LINE);
    ASSERT_EQ(cfr_selection_get(vt)->end.col, 79);
    cfr_free(vt);
}

static void test_row_text_and_search_use_row_width(void)
{
    int sb = 0;
    CfrTerm *vt = make_wide_history(&sb);

    size_t need = cfr_row_text_size(vt, -sb);
    ASSERT_TRUE(need >= 80);
    char *buf = malloc(need);
    ASSERT_NOT_NULL(buf);
    ASSERT_EQ(cfr_row_text(vt, -sb, buf, need), strlen(buf));
    ASSERT_EQ(strlen(buf), (size_t)66);
    ASSERT_TRUE(strstr(buf, "END-0") != NULL);
    free(buf);
    ASSERT_EQ(cfr_row_text_size(vt, -sb - 1), (size_t)0);

    /* The marker sits beyond column 40 of a row older than the grid's
     * width: the search must still see it. */
    int found_row = 0;
    ASSERT_TRUE(cfr_find_text(vt, "END-0", false, &found_row));
    ASSERT_EQ(found_row, -sb);
    cfr_free(vt);
}

/* Shift-click extend picks the nearer endpoint by a row/column distance;
 * across thousands of rows that distance overflowed an int, so the far
 * endpoint moved instead. */
static void test_selection_extend_across_long_history(void)
{
    CfrTerm *vt = make_term(4, 20, 6000);
    for (int i = 0; i < 5100; ++i)
        feed(vt, "x\r\n");
    ASSERT_TRUE(cfr_get_scrollback_lines(vt) > 5000);

    cfr_selection_start(vt, -5000, 0, CFR_SEL_CHAR);
    cfr_selection_update(vt, 0, 0);
    cfr_selection_extend(vt, -1500, 0); /* nearer the end (1500 < 3500 rows) */
    const CfrSelection *sel = cfr_selection_get(vt);
    ASSERT_NOT_NULL(sel);
    ASSERT_EQ(sel->start.row, -5000);
    ASSERT_EQ(sel->end.row, -1500);
    cfr_free(vt);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);
    RUN_TEST(test_unified_rows_bounds);
    RUN_TEST(test_row_width_is_per_row);
    RUN_TEST(test_page_scoped_ids_resolve_per_row);
    RUN_TEST(test_selection_text_reads_rows_at_their_width);
    RUN_TEST(test_row_text_and_search_use_row_width);
    RUN_TEST(test_selection_extend_across_long_history);
    TEST_SUMMARY();
}
