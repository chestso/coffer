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

/* Same, but with an explicit byte budget and reflow off so history keeps
 * the width it was written at across resizes. */
static CfrTerm *make_term_capped(int rows, int cols, int scrollback,
                                 int scrollback_bytes)
{
    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.rows = rows;
    cfg.cols = cols;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 6;
    cfg.reflow = false;
    cfg.scrollback = scrollback;
    cfg.scrollback_bytes = scrollback_bytes;
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
            /* ...but the key tells the two links apart. */
            ASSERT_TRUE(cfr_cell_hyperlink_key(vt, row, 0) != 0);
            ASSERT_TRUE(cfr_cell_hyperlink_key(vt, row, 0) !=
                        cfr_cell_hyperlink_key(vt, row + (int)CFR_SB_PAGE_ROWS, 0));
        }
        /* The key depends on the URI alone: a fresh terminal that only
         * ever saw this link agrees, whichever page the row lives on. */
        if (s % 23 == 0) {
            CfrTerm *ref = make_term(2, 40, 0);
            char osc[96];
            snprintf(osc, sizeof(osc), "\x1b]8;;%s\x1b\\L", want);
            feed(ref, osc);
            ASSERT_TRUE(cfr_cell_hyperlink_key(ref, 0, 0) ==
                        cfr_cell_hyperlink_key(vt, row, 0));
            cfr_free(ref);
        }
    }
    ASSERT_TRUE(id_collision);
    cfr_free(vt);
}

/* One link left open across many pages of scrollback: every row of it
 * reads back the same key, however many tables it is spread over, and the
 * unlinked rows around it read 0. */
static void test_link_key_spans_pages(void)
{
    enum
    {
        LINES = 200
    };
    CfrTerm *vt = make_term(4, 40, 1000);
    feed(vt, "plain\r\n\x1b]8;;https://example.com/long\x1b\\");
    for (int i = 0; i < LINES; ++i)
        feed(vt, "link\r\n");
    feed(vt, "\x1b]8;;\x1b\\tail\r\n");

    int sb = cfr_get_scrollback_lines(vt);
    ASSERT_TRUE(sb > 3 * (int)CFR_SB_PAGE_ROWS);
    uint64_t key = cfr_cell_hyperlink_key(vt, -sb + 1, 0);
    ASSERT_TRUE(key != 0);
    ASSERT_EQ(cfr_cell_hyperlink_key(vt, -sb, 0), 0u); /* "plain" */
    for (int i = 1; i <= LINES; ++i)
        ASSERT_TRUE(cfr_cell_hyperlink_key(vt, -sb + i, 0) == key);
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

/* -------- the scrollback ring ---------------------------------------- */

/* Deterministic text of line `n`: "L<n>:" then letters, 3..45 columns. */
static void gen_line(char *out, size_t cap, int n)
{
    uint32_t h = (uint32_t)n * 2654435761u;
    int len = 3 + (int)((h >> 8) % 43);
    int k = snprintf(out, cap, "L%d:", n);
    if (len < k)
        len = k;
    for (int i = k; i < len; ++i)
        out[i] = (char)('a' + (i + n) % 26);
    out[len] = '\0';
}

static void feed_line(CfrTerm *vt, int n)
{
    char buf[64];
    gen_line(buf, sizeof(buf), n);
    feed(vt, buf);
    feed(vt, "\r\n");
}

/* A line that fits one row (width >= 8): exactly one row of history each. */
static void feed_short(CfrTerm *vt, int n)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "L%d:\r\n", n);
    feed(vt, buf);
}

/* The line number a unified row's text starts with, or -1. */
static int row_line_no(const CfrTerm *vt, int row)
{
    char buf[256];
    cfr_row_text(vt, row, buf, sizeof(buf));
    int n = -1;
    return sscanf(buf, "L%d:", &n) == 1 ? n : -1;
}

/* Structural invariants of the ring, independent of what it holds: rows are
 * contiguous per page, each page's rows run newest-down, an older page is
 * sealed at its fill, and no page reappears after another one began. */
static void check_ring(const CfrTerm *vt)
{
    int n = vt->sb_lines;
    ASSERT_TRUE(n >= 0 && n <= vt->sb_capacity);
    ASSERT_EQ(n, cfr_get_scrollback_lines(vt));

    const CfrPage *seen[512];
    int nseen = 0;
    const CfrPage *prev = NULL;
    int prev_row = 0;
    uint64_t bytes = 0;
    for (int d = 0; d < n; ++d) {
        int row = -1;
        const CfrPage *p = cfr_sb_page_for_row(vt, d, &row);
        ASSERT_NOT_NULL(p);
        ASSERT_TRUE(row >= 0 && row < p->row_count);
        bytes += (uint64_t)p->cols * sizeof(CfrCell);
        if (p == prev) {
            ASSERT_EQ(row, prev_row - 1);
        } else {
            ASSERT_EQ(row, p->row_count - 1); /* its newest live row */
            for (int i = 0; i < nseen; ++i)
                ASSERT_TRUE(seen[i] != p);
            ASSERT_TRUE(nseen < 512);
            seen[nseen++] = p;
        }
        prev = p;
        prev_row = row;
    }
    int row = -1;
    ASSERT_NULL(cfr_sb_page_for_row(vt, n, &row));

    /* The byte counter is exactly the held rows' cell storage, and the
     * budget never binds after eviction returns. */
    ASSERT_EQ(vt->sb_bytes, bytes);
    ASSERT_TRUE(vt->sb_bytes_cap == 0 || vt->sb_bytes <= vt->sb_bytes_cap);
}

/* Read the whole unified buffer back as logical lines: every line must be
 * exactly gen_line(n) for consecutive n (the oldest may be a cut-off
 * fragment, the last is the blank cursor row). Catches lost, duplicated,
 * reordered or corrupted rows. */
static void check_buffer_text(const CfrTerm *vt)
{
    int rows = 0, cols = 0;
    cfr_get_dimensions(vt, &rows, &cols);
    int sb = cfr_get_scrollback_lines(vt);

    char line[4096];
    char want[64];
    size_t len = 0;
    int next_n = -1;
    bool in_line = false;
    bool seen_blank = false;
    for (int row = -sb; row <= rows; ++row) {
        bool end_of_buffer = (row == rows);
        bool continues = !end_of_buffer && row > -sb && cfr_row_is_continuation(vt, row);
        if (!continues && in_line) { /* a logical line just ended */
            line[len] = '\0';
            if (next_n < 0 && line[0] == 'L') { /* first complete line */
                int n = -1;
                ASSERT_EQ(sscanf(line, "L%d:", &n), 1);
                next_n = n;
            }
            if (next_n >= 0 && line[0] != '\0') {
                ASSERT_FALSE(seen_blank); /* blank rows only trail */
                gen_line(want, sizeof(want), next_n);
                ASSERT_STR_EQ(line, want);
                next_n++;
            } else if (next_n >= 0) {
                seen_blank = true;
            }
            len = 0;
            in_line = false;
        }
        if (end_of_buffer)
            break;
        if (!continues)
            len = 0;
        size_t need = cfr_row_text_size(vt, row);
        char *buf = malloc(need);
        ASSERT_NOT_NULL(buf);
        cfr_row_text(vt, row, buf, need);
        ASSERT_TRUE(len + strlen(buf) < sizeof(line));
        memcpy(line + len, buf, strlen(buf));
        len += strlen(buf);
        free(buf);
        in_line = true;
    }
}

/* Pushes, evictions (including shrinking the capacity), width changes and
 * reflow pops interleaved at random: the ring must stay structurally sound
 * and hold exactly the newest lines, in order, intact. */
static void test_ring_survives_churn(void)
{
    CfrTerm *vt = make_term(3, 20, 150);
    uint32_t rng = 987654321u;
#define NEXT() (rng = rng * 1664525u + 1013904223u, rng >> 8)
    int next_line = 0;
    for (int step = 0; step < 600; ++step) {
        uint32_t op = NEXT() % 10;
        if (op < 6) {
            feed_line(vt, next_line++);
        } else if (op < 9) {
            cfr_resize(vt, 3, 6 + (int)(NEXT() % 35));
        } else {
            cfr_set_scrollback_size(vt, 20 + (int)(NEXT() % 180));
        }
        int before = test_fail_count;
        check_ring(vt);
        check_buffer_text(vt);
        if (test_fail_count != before) {
            fprintf(stderr, "  (step %d, op %u, %d lines)\n", step, op,
                    next_line);
            return;
        }
    }
#undef NEXT
    ASSERT_TRUE(cfr_get_scrollback_lines(vt) > 20);
    cfr_free(vt);
}

/* The ring never holds more than the capacity, and holds exactly that many
 * once full — eviction is by row, not by whole page. */
static void test_eviction_is_row_exact(void)
{
    CfrTerm *vt = make_term(3, 20, 100);
    for (int n = 0; n < 150; ++n)
        feed_short(vt, n);
    ASSERT_EQ(cfr_get_scrollback_lines(vt), 100);
    int newest = row_line_no(vt, -1);
    ASSERT_EQ(newest, 147); /* the grid holds lines 148, 149 and a blank */
    ASSERT_EQ(row_line_no(vt, -100), newest - 99);
    check_ring(vt);
    cfr_free(vt);
}

/* Lowering the capacity evicts at once, oldest first; raising it keeps
 * what is held. */
static void test_set_size_evicts_now(void)
{
    CfrTerm *vt = make_term(3, 20, 100);
    for (int n = 0; n < 150; ++n)
        feed_short(vt, n);
    int newest = row_line_no(vt, -1);

    cfr_set_scrollback_size(vt, 40);
    ASSERT_EQ(cfr_get_scrollback_lines(vt), 40);
    ASSERT_EQ(cfr_get_scrollback_capacity(vt), 40);
    ASSERT_EQ(row_line_no(vt, -1), newest);
    ASSERT_EQ(row_line_no(vt, -40), newest - 39);
    ASSERT_NULL(cfr_get_row(vt, -41, NULL));
    check_ring(vt);

    cfr_set_scrollback_size(vt, 500);
    ASSERT_EQ(cfr_get_scrollback_lines(vt), 40);
    cfr_set_scrollback_size(vt, 0);
    ASSERT_EQ(cfr_get_scrollback_lines(vt), 0);
    cfr_free(vt);
}

/* A selection that reaches into evicted rows is dropped; one inside the
 * rows that remain survives. */
static void test_set_size_drops_selection_into_evicted_rows(void)
{
    CfrTerm *vt = make_term(3, 20, 100);
    for (int n = 0; n < 150; ++n)
        feed_short(vt, n);

    cfr_selection_start(vt, -10, 0, CFR_SEL_CHAR);
    cfr_selection_update(vt, -8, 3);
    cfr_set_scrollback_size(vt, 60);
    ASSERT_TRUE(cfr_selection_active(vt));

    cfr_selection_start(vt, -50, 0, CFR_SEL_CHAR);
    cfr_selection_update(vt, -45, 3);
    cfr_set_scrollback_size(vt, 30);
    ASSERT_FALSE(cfr_selection_active(vt));
    cfr_free(vt);
}

/* Rows evicted off the old end of a page and the rest popped off the new
 * end leave the page unreferenced: it must be freed (LeakSanitizer fails
 * the run if it is not) and the ring must keep working. */
static void test_pop_after_front_eviction_frees_the_page(void)
{
    CfrTerm *vt = make_term(2, 20, 5);
    for (int n = 0; n < 12; ++n)
        feed_short(vt, n);
    ASSERT_EQ(cfr_get_scrollback_lines(vt), 5);

    cfr_scrollback_pop_rows(vt, 5);
    ASSERT_EQ(cfr_get_scrollback_lines(vt), 0);
    check_ring(vt);

    for (int n = 12; n < 20; ++n)
        feed_short(vt, n);
    ASSERT_EQ(cfr_get_scrollback_lines(vt), 5);
    ASSERT_EQ(row_line_no(vt, -1), 18);
    check_ring(vt);
    cfr_free(vt);
}

/* The byte budget is an additional bound: it evicts oldest-first exactly
 * like the line cap, and leaves the ring's byte accounting at the cap. */
static void test_byte_cap_evicts_by_bytes(void)
{
    enum
    {
        COLS = 8,
        ROWS = 3,
        HELD = 5
    };
    int cap = HELD * COLS * (int)sizeof(CfrCell);
    CfrTerm *vt = make_term_capped(ROWS, COLS, 100, cap);
    ASSERT_NOT_NULL(vt);
    ASSERT_EQ(vt->sb_bytes_cap, (uint64_t)cap);

    for (int n = 0; n < 20; ++n)
        feed_short(vt, n);
    ASSERT_EQ(cfr_get_scrollback_lines(vt), HELD);
    ASSERT_EQ(vt->sb_bytes, (uint64_t)cap);
    ASSERT_EQ(row_line_no(vt, -1), 17);
    ASSERT_EQ(row_line_no(vt, -HELD), 13);
    ASSERT_NULL(cfr_get_row(vt, -HELD - 1, NULL));
    check_ring(vt);
    cfr_free(vt);
}

/* The line cap still applies when it is the smaller of the two bounds. */
static void test_line_cap_still_wins(void)
{
    enum
    {
        COLS = 8,
        LINES = 3
    };
    CfrTerm *vt = make_term_capped(2, COLS, LINES, 1 << 20);
    ASSERT_NOT_NULL(vt);
    for (int n = 0; n < 10; ++n)
        feed_short(vt, n);
    ASSERT_EQ(cfr_get_scrollback_lines(vt), LINES);
    ASSERT_EQ(vt->sb_bytes, (uint64_t)LINES * COLS * sizeof(CfrCell));
    check_ring(vt);
    cfr_free(vt);
}

/* Eviction charges a row by the width it was stored at, not the current
 * grid width: after a shrink, removing an old wide row frees more budget
 * than removing a new narrow one. */
static void test_byte_cap_uses_each_rows_width(void)
{
    enum
    {
        OLD_COLS = 8,
        NEW_COLS = 4,
        ROWS = 2
    };
    uint64_t wide = (uint64_t)OLD_COLS * sizeof(CfrCell);
    uint64_t narrow = (uint64_t)NEW_COLS * sizeof(CfrCell);
    /* Four rows are still 8 wide when the grid shrinks; twelve more arrive
     * at 4 wide. The budget fits three wide + twelve narrow. */
    int cap = (int)(3 * wide + 12 * narrow);
    CfrTerm *vt = make_term_capped(ROWS, OLD_COLS, 100, cap);
    ASSERT_NOT_NULL(vt);

    for (int n = 0; n < 5; ++n)
        feed_short(vt, n);
    ASSERT_EQ(cfr_get_scrollback_lines(vt), 4);

    cfr_resize(vt, ROWS, NEW_COLS); /* reflow off: history keeps its width */
    for (int n = 5; n < 17; ++n)
        feed_short(vt, n);

    ASSERT_EQ(cfr_get_scrollback_lines(vt), 15);
    ASSERT_EQ(vt->sb_bytes, (uint64_t)cap);
    ASSERT_NULL(cfr_get_row(vt, -16, NULL));
    int cols = 0;
    ASSERT_NOT_NULL(cfr_get_row(vt, -15, &cols));
    ASSERT_EQ(cols, OLD_COLS); /* L1..L3 are the surviving wide rows */
    ASSERT_EQ(row_line_no(vt, -15), 1);
    ASSERT_EQ(row_line_no(vt, -13), 3);
    ASSERT_NOT_NULL(cfr_get_row(vt, -12, &cols));
    ASSERT_EQ(cols, NEW_COLS); /* L4..L15 are four wide */
    ASSERT_EQ(row_line_no(vt, -12), 4);
    ASSERT_EQ(row_line_no(vt, -1), 15);
    check_ring(vt);
    cfr_free(vt);
}

/* -1 selects the engine default budget; 0 disables the byte bound, and
 * ED 3 resets the counter along with the ring. */
static void test_byte_cap_default_off_and_clear(void)
{
    CfrTerm *vt = make_term_capped(3, 8, 100, -1);
    ASSERT_NOT_NULL(vt);
    ASSERT_EQ(vt->sb_bytes_cap, (uint64_t)CFR_DEFAULT_SCROLLBACK_BYTES);
    cfr_free(vt);

    vt = make_term_capped(3, 8, 100, 0);
    ASSERT_NOT_NULL(vt);
    ASSERT_EQ(vt->sb_bytes_cap, 0u);
    for (int n = 0; n < 20; ++n)
        feed_short(vt, n);
    ASSERT_EQ(cfr_get_scrollback_lines(vt), 18); /* line cap only */
    ASSERT_EQ(vt->sb_bytes, (uint64_t)18 * 8 * sizeof(CfrCell));
    check_ring(vt);

    feed(vt, "\x1b[3J"); /* erase saved lines */
    ASSERT_EQ(cfr_get_scrollback_lines(vt), 0);
    ASSERT_EQ(vt->sb_bytes, 0u);
    for (int n = 20; n < 30; ++n)
        feed_short(vt, n);
    ASSERT_EQ(cfr_get_scrollback_lines(vt), 10);
    check_ring(vt);
    cfr_free(vt);
}

/* A budget smaller than a single row retains nothing, without allocating a
 * page only to evict it again. */
static void test_byte_cap_below_one_row_retains_nothing(void)
{
    int cap = 8 * (int)sizeof(CfrCell) - 1;
    CfrTerm *vt = make_term_capped(2, 8, 100, cap);
    ASSERT_NOT_NULL(vt);
    for (int n = 0; n < 10; ++n)
        feed_short(vt, n);
    ASSERT_EQ(cfr_get_scrollback_lines(vt), 0);
    ASSERT_EQ(vt->sb_bytes, 0u);
    check_ring(vt);
    cfr_free(vt);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);
    RUN_TEST(test_unified_rows_bounds);
    RUN_TEST(test_row_width_is_per_row);
    RUN_TEST(test_page_scoped_ids_resolve_per_row);
    RUN_TEST(test_link_key_spans_pages);
    RUN_TEST(test_selection_text_reads_rows_at_their_width);
    RUN_TEST(test_row_text_and_search_use_row_width);
    RUN_TEST(test_selection_extend_across_long_history);
    RUN_TEST(test_ring_survives_churn);
    RUN_TEST(test_eviction_is_row_exact);
    RUN_TEST(test_set_size_evicts_now);
    RUN_TEST(test_set_size_drops_selection_into_evicted_rows);
    RUN_TEST(test_pop_after_front_eviction_frees_the_page);
    RUN_TEST(test_byte_cap_evicts_by_bytes);
    RUN_TEST(test_line_cap_still_wins);
    RUN_TEST(test_byte_cap_uses_each_rows_width);
    RUN_TEST(test_byte_cap_default_off_and_clear);
    RUN_TEST(test_byte_cap_below_one_row_retains_nothing);
    TEST_SUMMARY();
}
