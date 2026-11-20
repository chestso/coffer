/* tests/test_cfr_grid_gc.c — the live grid's intern tables stay bounded.
 *
 * The style, grapheme and hyperlink tables of a page only ever grow, and
 * the live grid page lives as long as the terminal does. cfr_grid_gc()
 * (grid.c) rebuilds the page with just the entries its cells still use
 * once a table outgrows the screen; cfr_input_write() runs it between
 * 16 KiB slices of input. Without it, the 65535th distinct link silently
 * disabled every later link, and styles and clusters grew without bound. */

#include "coffer_internal.h"
#include "test_helpers.h"
#include <coffer/coffer.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum
{
    ROWS = 24,
    COLS = 80,
    LINES = 70000 /* past the 65535 hyperlink id cap */
};

static CfrTerm *make_term(void)
{
    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.rows = ROWS;
    cfg.cols = COLS;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 6;
    cfg.reflow = true;
    cfg.scrollback = 100;
    return cfr_new(&cfg);
}

static void write_all(CfrTerm *vt, const char *s, size_t len, size_t chunk)
{
    for (size_t off = 0; off < len; off += chunk) {
        size_t n = len - off < chunk ? len - off : chunk;
        cfr_input_write(vt, (const uint8_t *)s + off, n);
    }
}

/* The most any table may hold once a write has returned: the collection
 * limit, which is the bigger of the floor and twice the cell count. */
static uint32_t table_limit(void)
{
    uint32_t limit = 2u * ROWS * COLS;
    return limit < CFR_GRID_GC_MIN_ENTRIES ? CFR_GRID_GC_MIN_ENTRIES : limit;
}

/* Append to a growing text buffer. */
typedef struct
{
    char *p;
    size_t len, cap;
} Buf;

static void put(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(Buf *b, const char *fmt, ...)
{
    char tmp[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (b->len + (size_t)n + 1 > b->cap) {
        b->cap = (b->cap ? b->cap * 2 : 4096) + (size_t)n;
        b->p = realloc(b->p, b->cap);
    }
    memcpy(b->p + b->len, tmp, (size_t)n);
    b->len += (size_t)n;
}

/* After LINES lines, each ending in CR LF, the last cursor row is blank and
 * row r holds line LINES - (ROWS - 1) + r. */
static int line_at(int row)
{
    return LINES - (ROWS - 1) + row;
}

static void uri_at(CfrTerm *vt, int row, char *out, size_t cap)
{
    size_t n = cfr_cell_get_hyperlink(vt, row, 0, (uint8_t *)out, cap - 1);
    out[n] = '\0';
}

static void check_links(CfrTerm *vt)
{
    for (int r = 0; r < ROWS - 1; ++r) {
        char want[48], got[48];
        snprintf(want, sizeof(want), "http://h/%d", line_at(r));
        uri_at(vt, r, got, sizeof(got));
        ASSERT_STR_EQ(got, want);
    }
}

/* 70000 distinct links on the main screen, in one write and in small ones:
 * the last rows still resolve their URIs and the table stays bounded. */
static void run_links(size_t chunk)
{
    Buf b = { 0 };
    for (int i = 0; i < LINES; ++i)
        put(&b, "\x1b]8;;http://h/%d\x07X\x1b]8;;\x07\r\n", i);
    CfrTerm *vt = make_term();
    write_all(vt, b.p, b.len, chunk);
    free(b.p);

    check_links(vt);
    ASSERT_TRUE(vt->grid->hyperlinks.count <= table_limit());
    cfr_free(vt);
}

static void test_links_past_the_id_cap_one_write(void)
{
    run_links((size_t)1 << 30);
}

static void test_links_past_the_id_cap_small_writes(void)
{
    run_links(997);
}

/* 70000 distinct truecolor pens: colors survive every rebuild and the
 * style table stays bounded. */
static void test_styles_stay_bounded(void)
{
    Buf b = { 0 };
    for (int i = 0; i < LINES; ++i)
        put(&b, "\x1b[38;2;%d;%d;%dmX\r\n", i & 255, (i >> 8) & 255, 7);
    CfrTerm *vt = make_term();
    write_all(vt, b.p, b.len, (size_t)1 << 30);
    free(b.p);

    for (int r = 0; r < ROWS - 1; ++r) {
        int i = line_at(r);
        const CfrStyle *st = cfr_cell_style(vt, r, 0);
        ASSERT_NOT_NULL(st);
        ASSERT_EQ(st->fg_rgb,
                  (uint32_t)((i & 255) << 16 | ((i >> 8) & 255) << 8 | 7));
    }
    ASSERT_TRUE(vt->grid->styles.count <= table_limit());
    cfr_free(vt);
}

/* Distinct two-codepoint clusters: base U+4E00+k with a combining acute. */
static void utf8(Buf *b, uint32_t cp)
{
    char s[5] = { 0 };
    if (cp < 0x80)
        s[0] = (char)cp;
    else if (cp < 0x800) {
        s[0] = (char)(0xC0 | (cp >> 6));
        s[1] = (char)(0x80 | (cp & 0x3F));
    } else {
        s[0] = (char)(0xE0 | (cp >> 12));
        s[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        s[2] = (char)(0x80 | (cp & 0x3F));
    }
    put(b, "%s", s);
}

static void test_clusters_stay_bounded(void)
{
    enum
    {
        N = 20000
    };
    Buf b = { 0 };
    for (int k = 0; k < N; ++k) {
        utf8(&b, 0x4E00u + (uint32_t)k);
        utf8(&b, 0x0301u);
        put(&b, "\r\n");
    }
    CfrTerm *vt = make_term();
    write_all(vt, b.p, b.len, (size_t)1 << 30);
    free(b.p);

    for (int r = 0; r < ROWS - 1; ++r) {
        int k = N - (ROWS - 1) + r;
        uint32_t cps[4];
        ASSERT_EQ(cfr_cell_get_grapheme(vt, r, 0, cps, 4), 2u);
        ASSERT_EQ(cps[0], 0x4E00u + (uint32_t)k);
        ASSERT_EQ(cps[1], 0x0301u);
    }
    ASSERT_TRUE(vt->grid->graphemes.dedup_count <= table_limit());
    cfr_free(vt);
}

/* A link that stays open while the table that triggers the rebuild is the
 * style table: the next print carries the link into the new page. */
static void test_open_link_survives_rebuild(void)
{
    Buf b = { 0 };
    put(&b, "\x1b]8;;http://open\x07");
    for (int i = 0; i < 9000; ++i)
        put(&b, "\x1b[38;2;%d;%d;1mX", i & 255, i >> 8);
    CfrTerm *vt = make_term();
    write_all(vt, b.p, b.len, (size_t)1 << 30);
    free(b.p);
    ASSERT_TRUE(vt->grid->styles.count <= table_limit());

    write_all(vt, "Z", 1, 1);
    CfrCursor cur = cfr_get_cursor(vt);
    int col = cur.col - 1, row = cur.row;
    if (col < 0) {
        col = COLS - 1;
        row -= 1;
    }
    char got[32];
    size_t n = cfr_cell_get_hyperlink(vt, row, col, (uint8_t *)got, 31);
    got[n] = '\0';
    ASSERT_STR_EQ(got, "http://open");
    cfr_free(vt);
}

/* The grid after a stream must not depend on how the stream is cut into
 * writes, whether or not a cut lands where a rebuild runs. */
static void snapshot(CfrTerm *vt, Buf *out)
{
    for (int r = 0; r < ROWS; ++r) {
        for (int c = 0; c < COLS; ++c) {
            const CfrCell *cell = cfr_get_cell(vt, r, c);
            const CfrStyle *st = cfr_cell_style(vt, r, c);
            char uri[32];
            size_t n = cfr_cell_get_hyperlink(vt, r, c, (uint8_t *)uri, 31);
            uri[n] = '\0';
            put(out, "%u/%x/%s;", cell->cp, st ? st->fg_rgb : 0u, uri);
        }
    }
}

static void test_write_boundaries_are_invisible(void)
{
    Buf b = { 0 };
    for (int i = 0; i < 9000; ++i)
        put(&b, "\x1b]8;;http://h/%d\x07\x1b[38;2;%d;%d;3m%c\x1b]8;;\x07%s",
            i, i & 255, i >> 8, 'a' + i % 26, i % 40 == 39 ? "\r\n" : "");
    Buf ref = { 0 }, cut = { 0 };

    CfrTerm *whole = make_term();
    write_all(whole, b.p, b.len, (size_t)1 << 30);
    snapshot(whole, &ref);
    cfr_free(whole);

    CfrTerm *bytes = make_term();
    write_all(bytes, b.p, b.len, 7);
    snapshot(bytes, &cut);
    cfr_free(bytes);

    ASSERT_EQ(ref.len, cut.len);
    ASSERT_TRUE(memcmp(ref.p, cut.p, ref.len) == 0);
    free(b.p);
    free(ref.p);
    free(cut.p);
}

/* An allocator that can be told to refuse every allocation of one size. */
typedef struct
{
    size_t fail_size;
    bool armed;
    int failed;
} FailAlloc;

static void *fa_alloc(size_t size, void *user)
{
    FailAlloc *f = user;
    if (f->armed && size == f->fail_size) {
        f->failed++;
        return NULL;
    }
    return malloc(size);
}

static void *fa_realloc(void *ptr, size_t size, void *user)
{
    (void)user;
    return realloc(ptr, size);
}

static void fa_free(void *ptr, void *user)
{
    (void)user;
    free(ptr);
}

/* Memory running out in the middle of a rebuild keeps the old page whole;
 * a later write retries and succeeds. */
static void test_failed_rebuild_keeps_the_old_page(void)
{
    FailAlloc f = { 0 };
    CfrAllocator a = { fa_alloc, fa_realloc, fa_free, &f };
    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.rows = ROWS;
    cfg.cols = COLS;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 6;
    cfg.scrollback = 100;
    CfrTerm *vt = cfr_new_with_allocator(&cfg, &a);

    write_all(vt, "\x1b[31mX", 6, 6); /* the grid's style table exists */
    f.fail_size = CFR_STYLES_INIT * sizeof(CfrStyle);
    f.armed = true; /* a rebuilt page's first style table is refused */

    Buf b = { 0 };
    for (int i = 0; i < 5000; ++i)
        put(&b, "\x1b[38;2;%d;%d;9mX\r\n", i & 255, i >> 8);
    write_all(vt, b.p, b.len, (size_t)1 << 30);
    free(b.p);

    ASSERT_TRUE(f.failed >= 1);
    ASSERT_TRUE(vt->alloc_failures >= 1);
    ASSERT_TRUE(vt->grid->styles.count > table_limit()); /* not rebuilt */
    for (int r = 0; r < ROWS - 1; ++r) {
        int i = 5000 - (ROWS - 1) + r;
        const CfrStyle *st = cfr_cell_style(vt, r, 0);
        ASSERT_NOT_NULL(st);
        ASSERT_EQ(st->fg_rgb,
                  (uint32_t)((i & 255) << 16 | (i >> 8) << 8 | 9));
    }

    f.armed = false;
    write_all(vt, "Y", 1, 1); /* the retry */
    ASSERT_TRUE(vt->grid->styles.count <= table_limit());
    for (int r = 0; r < ROWS - 1; ++r) {
        int i = 5000 - (ROWS - 1) + r;
        const CfrStyle *st = cfr_cell_style(vt, r, 0);
        ASSERT_NOT_NULL(st);
        ASSERT_EQ(st->fg_rgb,
                  (uint32_t)((i & 255) << 16 | (i >> 8) << 8 | 9));
    }
    cfr_free(vt);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    RUN_TEST(test_links_past_the_id_cap_one_write);
    RUN_TEST(test_links_past_the_id_cap_small_writes);
    RUN_TEST(test_styles_stay_bounded);
    RUN_TEST(test_clusters_stay_bounded);
    RUN_TEST(test_open_link_survives_rebuild);
    RUN_TEST(test_write_boundaries_are_invisible);
    RUN_TEST(test_failed_rebuild_keeps_the_old_page);

    TEST_SUMMARY();
}
