/* tests/test_cfr_introspect.c — cfr_row_text + cfr_find_text.
 *
 * The unified-buffer search underpins scriptable assertions
 * (assert-contains, wait-for): it must flatten grapheme clusters whole,
 * skip continuation cells, search scrollback oldest-first, and honor
 * the line-end anchor. */

#include "coffer_internal.h"
#include "test_helpers.h"
#include <coffer/coffer.h>

#include <stdio.h>
#include <string.h>

#define BUF_CAP 4096

static CfrTerm *make_term(int rows, int cols)
{
    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.rows = rows;
    cfg.cols = cols;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 6;
    cfg.scrollback = 100;
    return cfr_new(&cfg);
}

static void feed(CfrTerm *vt, const char *s)
{
    cfr_input_write(vt, (const uint8_t *)s, strlen(s));
}

static void row_str(CfrTerm *vt, int row, char *buf)
{
    size_t n = cfr_row_text(vt, row, buf, BUF_CAP);
    ASSERT_TRUE(n < BUF_CAP);
    (void)n;
}

/* -------- Basic flattening ------------------------------------------- */

static void test_row_text_plain(void)
{
    CfrTerm *vt = make_term(5, 20);
    feed(vt, "hello");
    char buf[BUF_CAP];
    row_str(vt, 0, buf);
    ASSERT_STR_EQ(buf, "hello");
    cfr_free(vt);
}

static void test_row_text_blank_row_is_empty(void)
{
    /* A row whose cells were never written flattens to "" — virgin
     * cells carry no content (cp == 0, width == 0) and collapse. */
    CfrTerm *vt = make_term(5, 8);
    char buf[BUF_CAP];
    row_str(vt, 3, buf);
    ASSERT_STR_EQ(buf, "");
    cfr_free(vt);
}

static void test_row_text_out_of_range_is_empty(void)
{
    CfrTerm *vt = make_term(5, 8);
    feed(vt, "x");
    char buf[BUF_CAP];
    row_str(vt, 5, buf); /* past the last visible row */
    ASSERT_STR_EQ(buf, "");
    row_str(vt, -1, buf); /* no scrollback yet */
    ASSERT_STR_EQ(buf, "");
    cfr_free(vt);
}

static void test_row_text_erased_blanks_are_spaces(void)
{
    /* Erase blanks are real cells (cp == 0x20, width == 1): they
     * flatten to spaces, unlike never-written cells. */
    CfrTerm *vt = make_term(5, 10);
    feed(vt, "ab");
    feed(vt, "\x1b[1;3H\x1b[J"); /* erase cols 3..10 (1-based) */
    char buf[BUF_CAP];
    row_str(vt, 0, buf);
    ASSERT_STR_EQ(buf, "ab        ");
    cfr_free(vt);
}

static void test_row_text_wide_char_skips_continuation(void)
{
    CfrTerm *vt = make_term(5, 20);
    /* CJK ideograph occupies two cells; the continuation cell must not
     * emit a second codepoint. */
    feed(vt, "a\xE4\xB8\xAD"
             "b"); /* a 中 b */
    char buf[BUF_CAP];
    row_str(vt, 0, buf);
    ASSERT_STR_EQ(buf, "a\xE4\xB8\xAD"
                       "b");
    cfr_free(vt);
}

static void test_row_text_grapheme_cluster(void)
{
    CfrTerm *vt = make_term(5, 20);
    /* "👨‍👩‍👧" = 4 codepoints joined by ZWJ; one 2-cell cluster. The
     * flattened row must contain the whole cluster, not just the base. */
    feed(vt, "x\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D"
             "\xF0\x9F\x91\xA7y");
    char buf[BUF_CAP];
    row_str(vt, 0, buf);
    ASSERT_STR_EQ(buf, "x\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D"
                       "\xF0\x9F\x91\xA7y");
    cfr_free(vt);
}

static void test_row_text_nul_termination_small_cap(void)
{
    CfrTerm *vt = make_term(5, 20);
    feed(vt, "hello world");
    char buf[6];
    size_t n = cfr_row_text(vt, 0, buf, sizeof(buf));
    ASSERT_EQ(n, 5);
    ASSERT_STR_EQ(buf, "hello");
    ASSERT_EQ(cfr_row_text(vt, 0, buf, 0), 0); /* cap 0: no write */
    cfr_free(vt);
}

/* -------- Scrollback coordinates ------------------------------------- */

/* Push lines off the top: 10 rows of content on a 5-row screen leaves
 * the first lines in scrollback (-1 = most recently scrolled off). */
static void test_row_text_scrollback_negative_rows(void)
{
    CfrTerm *vt = make_term(5, 20);
    for (int i = 0; i < 9; i++) {
        char line[32];
        snprintf(line, sizeof(line), "line-%d\r\n", i);
        feed(vt, line);
    }
    feed(vt, "line-9");
    char buf[BUF_CAP];
    row_str(vt, -1, buf); /* most recently scrolled off = line-4 */
    ASSERT_STR_EQ(buf, "line-4");
    row_str(vt, -5, buf); /* oldest scrollback row = line-0 */
    ASSERT_STR_EQ(buf, "line-0");
    row_str(vt, 4, buf); /* bottom visible row = line-9 */
    ASSERT_STR_EQ(buf, "line-9");
    cfr_free(vt);
}

/* -------- Search ------------------------------------------------------ */

static void test_find_text_visible(void)
{
    CfrTerm *vt = make_term(5, 20);
    feed(vt, "hello\r\nworld");
    int row = -99;
    ASSERT_TRUE(cfr_find_text(vt, "world", false, &row));
    ASSERT_EQ(row, 1);
    ASSERT_TRUE(cfr_find_text(vt, "hello", false, &row));
    ASSERT_EQ(row, 0);
    ASSERT_FALSE(cfr_find_text(vt, "absent", false, &row));
    cfr_free(vt);
}

static void test_find_text_searches_scrollback_oldest_first(void)
{
    CfrTerm *vt = make_term(5, 20);
    for (int i = 0; i < 9; i++) {
        char line[32];
        snprintf(line, sizeof(line), "line-%d\r\n", i);
        feed(vt, line);
    }
    feed(vt, "line-9");
    int row = 0;
    /* "line-2" has scrolled off: match must report a negative row. */
    ASSERT_TRUE(cfr_find_text(vt, "line-2", false, &row));
    ASSERT_EQ(row, -3);
    /* The needle matches every row "line-N" as a substring: oldest
     * first means the scrollback's oldest row wins. */
    ASSERT_TRUE(cfr_find_text(vt, "line-", false, &row));
    ASSERT_EQ(row, -5);
    cfr_free(vt);
}

static void test_find_text_line_end_anchor(void)
{
    CfrTerm *vt = make_term(5, 20);
    feed(vt, "prefix needle\r\nneedle prefix");
    int row = -99;
    /* Substring search finds the first row; line-end must find the
     * row whose text ENDS with the needle (trailing blanks trimmed). */
    ASSERT_TRUE(cfr_find_text(vt, "needle", false, &row));
    ASSERT_EQ(row, 0);
    ASSERT_TRUE(cfr_find_text(vt, "needle", true, &row));
    ASSERT_EQ(row, 0);
    ASSERT_TRUE(cfr_find_text(vt, "prefix", true, &row));
    ASSERT_EQ(row, 1);
    /* A needle matching only mid-row (followed by blanks) must NOT
     * satisfy line-end. */
    ASSERT_FALSE(cfr_find_text(vt, "prefix needle xx", true, &row));
    cfr_free(vt);
}

static void test_find_text_utf8_needle(void)
{
    CfrTerm *vt = make_term(5, 20);
    /* ❯ (U+276F) prompt glyph, encoded raw. */
    feed(vt, "abc \xE2\x9D\xAF done");
    int row = -99;
    ASSERT_TRUE(cfr_find_text(vt, "\xE2\x9D\xAF done", false, &row));
    ASSERT_EQ(row, 0);
    cfr_free(vt);
}

static void test_find_text_grapheme_needle(void)
{
    CfrTerm *vt = make_term(5, 20);
    /* The ZWJ cluster must be findable as a whole — the failure mode
     * this API exists to prevent is a search that reads only cell->cp
     * and can never match multi-codepoint graphemes. */
    feed(vt, "ok \xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D"
             "\xF0\x9F\x91\xA7 !");
    int row = -99;
    ASSERT_TRUE(cfr_find_text(
        vt, "\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7", false, &row));
    ASSERT_EQ(row, 0);
    cfr_free(vt);
}

static void test_find_text_empty_needle_matches(void)
{
    CfrTerm *vt = make_term(5, 20);
    int row = -99;
    ASSERT_TRUE(cfr_find_text(vt, "", false, &row));
    ASSERT_EQ(row, 0);
    ASSERT_TRUE(cfr_find_text(vt, NULL, false, &row));
    cfr_free(vt);
}

static void test_find_text_no_wrap_span(void)
{
    CfrTerm *vt = make_term(3, 10);
    /* "abcdefghij" wraps onto row 1 ("klmnop"): a needle spanning the
     * soft wrap is two rows' text and must NOT match. */
    feed(vt, "abcdefghijklmnop");
    int row = -99;
    ASSERT_FALSE(cfr_find_text(vt, "jklmnop", false, &row));
    ASSERT_TRUE(cfr_find_text(vt, "klmnop", false, &row));
    ASSERT_EQ(row, 1);
    cfr_free(vt);
}

/* The row accessor behind a fast full-row flatten: it must agree with the
 * per-column lookup on every cell of every scrollback row. */
static void test_scrollback_row_matches_per_cell_reads(void)
{
    CfrTerm *vt = make_term(2, 5);
    /* 24 cells into 2 rows of 5: five wrapped rows, three of them
     * scrolled off into history. */
    feed(vt, "aaaaabbbbbcccccdddddeeee");
    ASSERT_TRUE(cfr_get_scrollback_lines(vt) > 0);
    for (int sb = 0; sb < cfr_get_scrollback_lines(vt); sb++) {
        int row_cols = 0;
        const CfrCell *row = cfr_get_scrollback_row(vt, sb, &row_cols);
        ASSERT_NOT_NULL(row);
        ASSERT_EQ(row_cols, 5);
        for (int c = 0; c < row_cols; c++) {
            ASSERT_EQ(row[c].cp, cfr_get_scrollback_cell(vt, sb, c)->cp);
            ASSERT_EQ(row[c].width, cfr_get_scrollback_cell(vt, sb, c)->width);
            ASSERT_EQ(row[c].grapheme_id,
                      cfr_get_scrollback_cell(vt, sb, c)->grapheme_id);
        }
    }
    /* find_text walks the full history through the row-resolved path. */
    ASSERT_TRUE(cfr_find_text(vt, "aaaaa", false, NULL));
    ASSERT_TRUE(cfr_find_text(vt, "ccccc", false, NULL));
    ASSERT_FALSE(cfr_find_text(vt, "zzzzz", false, NULL));
    cfr_free(vt);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN_TEST(test_row_text_plain);
    RUN_TEST(test_row_text_blank_row_is_empty);
    RUN_TEST(test_row_text_erased_blanks_are_spaces);
    RUN_TEST(test_row_text_out_of_range_is_empty);
    RUN_TEST(test_row_text_wide_char_skips_continuation);
    RUN_TEST(test_row_text_grapheme_cluster);
    RUN_TEST(test_row_text_nul_termination_small_cap);
    RUN_TEST(test_row_text_scrollback_negative_rows);
    RUN_TEST(test_find_text_visible);
    RUN_TEST(test_find_text_searches_scrollback_oldest_first);
    RUN_TEST(test_find_text_line_end_anchor);
    RUN_TEST(test_find_text_utf8_needle);
    RUN_TEST(test_find_text_grapheme_needle);
    RUN_TEST(test_find_text_empty_needle_matches);
    RUN_TEST(test_find_text_no_wrap_span);
    RUN_TEST(test_scrollback_row_matches_per_cell_reads);
    TEST_SUMMARY();
}
