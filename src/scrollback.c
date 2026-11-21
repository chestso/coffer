/*
 * coffer — scrollback: a ring of row records over pages.
 *
 * Storage and addressing are separate. Pages (CFR_SB_PAGE_ROWS rows, one
 * width each) own the cells, and the page-scoped style, grapheme and
 * hyperlink tables. A ring of CfrSbRow records, oldest first, says which
 * page and row each history row lives in. Row `d` deep (0 = newest) is
 * therefore a direct index — no page walking, no search, nothing a reader
 * has to remember between calls.
 *
 *   - Rows of one page are contiguous in the ring, so a page is freed the
 *     moment no neighbouring record shares it: after the last of its rows
 *     is evicted off the old end, or popped off the new end. No per-page
 *     live count exists to drift.
 *   - Eviction is row-exact: the ring never holds more than the configured
 *     capacity, but a page lives until its last row leaves.
 *   - A push appends to the newest row's page when it has room and the
 *     same width; a different width (the grid was resized) starts a new
 *     page. That is why history can hold rows of several widths.
 *
 * Cells are re-interned into the receiving page's tables: style_id,
 * grapheme_id and hyperlink_id are page-scoped, so a pushed row names the
 * page its ids refer to (`src`) and each cell is reseated into the
 * scrollback page. This keeps page lifetimes independent.
 */

#include "coffer_internal.h"

#include <string.h>

/* Ring record `i`, counted from the oldest (0) to the newest (sb_lines - 1). */
static CfrSbRow *ring_at(const CfrTerm *vt, uint32_t i)
{
    return &vt->sb_rows[(vt->sb_first + i) & (vt->sb_ring_cap - 1)];
}

/* Make room for one more record, doubling the ring (and straightening it)
 * when it is full. */
static bool ring_reserve(CfrTerm *vt)
{
    if ((uint32_t)vt->sb_lines < vt->sb_ring_cap)
        return true;
    if (vt->sb_ring_cap >= (1u << 30))
        return false;
    uint32_t nc = vt->sb_ring_cap ? vt->sb_ring_cap * 2 : 64;
    CfrSbRow *rows = cfr_alloc(vt, (size_t)nc * sizeof(CfrSbRow));
    if (!rows)
        return false;
    for (uint32_t i = 0; i < (uint32_t)vt->sb_lines; ++i)
        rows[i] = *ring_at(vt, i);
    cfr_dealloc(vt, vt->sb_rows);
    vt->sb_rows = rows;
    vt->sb_ring_cap = nc;
    vt->sb_first = 0;
    return true;
}

static CfrPage *new_sb_page(CfrTerm *vt, int cols)
{
    CfrPage *p = cfr_page_new(vt, CFR_SB_PAGE_ROWS, cols);
    if (!p)
        return NULL;
    /* cfr_page_new sets row_count to row_capacity (full page); for
     * scrollback row_count is "rows written so far". Reset to empty. */
    p->row_count = 0;
    return p;
}

/* The page a push of `cols`-wide row goes to: the newest row's page while
 * it has room and the same width, otherwise a fresh one. */
static CfrPage *page_for_push(CfrTerm *vt, int cols)
{
    if (vt->sb_lines > 0) {
        CfrPage *newest = ring_at(vt, (uint32_t)vt->sb_lines - 1)->page;
        if (newest->cols == cols && newest->row_count < newest->row_capacity)
            return newest;
    }
    return new_sb_page(vt, cols);
}

/* Cell storage one retained row accounts for. Charging per row (not per
 * page) means unused page slots cost nothing and eviction subtracts
 * exactly what the push added, even when the row width changed since. */
static uint64_t row_bytes(int cols)
{
    return (uint64_t)cols * sizeof(CfrCell);
}

/* The one place rows leave the old end. */
static void evict_to_capacity(CfrTerm *vt)
{
    while (vt->sb_lines > 0 &&
           (vt->sb_lines > vt->sb_capacity ||
            (vt->sb_bytes_cap != 0 && vt->sb_bytes > vt->sb_bytes_cap))) {
        CfrSbRow *oldest = ring_at(vt, 0);
        CfrPage *page = oldest->page;
        uint64_t gone = row_bytes(page->cols);
        CFR_BUG_CHECK(vt->sb_bytes >= gone,
                      "scrollback bytes underflow: held=%llu row=%llu",
                      (unsigned long long)vt->sb_bytes,
                      (unsigned long long)gone);
        *oldest = (CfrSbRow){ 0 }; /* no stale pointer to a page we may free */
        vt->sb_first = (vt->sb_first + 1) & (vt->sb_ring_cap - 1);
        vt->sb_bytes -= gone;
        vt->sb_lines--;
        if (vt->sb_lines == 0 || ring_at(vt, 0)->page != page)
            cfr_page_free(vt, page);
    }
    CFR_BUG_CHECK(vt->sb_lines > 0 || vt->sb_bytes == 0,
                  "scrollback accounting: %llu bytes with no rows",
                  (unsigned long long)vt->sb_bytes);
}

void cfr_scrollback_push(CfrTerm *vt, const CfrPage *src,
                         const CfrCell *src_cells, int cols, uint32_t lineage)
{
    if (!vt || cols <= 0 || !src || !src_cells)
        return;

    /* A row that alone exceeds the byte budget can never be retained:
     * drop it before allocating a page just to evict it again. */
    if (vt->sb_bytes_cap != 0 && row_bytes(cols) > vt->sb_bytes_cap)
        return;

    /* Store the row, if the ring retains anything and can allocate. */
    CfrPage *page = NULL;
    bool fresh_page = false;
    if (vt->sb_capacity > 0 && ring_reserve(vt)) {
        page = page_for_push(vt, cols);
        /* A page the ring does not yet reference was just allocated for
         * this push; an abort below must free it. */
        fresh_page =
            page && (vt->sb_lines == 0 ||
                     ring_at(vt, (uint32_t)vt->sb_lines - 1)->page != page);
    }
    if (page) {
        /* Invariant: the push reads style/grapheme/hyperlink ids from
         * `src` and writes them into `page`. If a caller ever passes the
         * page AS the source, source and dest alias — the intern table
         * grows mid-read and any held pointer dangles. Fail loud at the
         * door. */
        CFR_BUG_CHECK(page != src,
                      "scrollback push target page is also the source (%p)",
                      (void *)page);
        uint32_t row = page->row_count;
        CfrCell *dst = &page->cells[(size_t)row * page->cols];

        /* Re-intern style, grapheme, and hyperlink references into the
         * page — page-scoped tables don't carry across pages. If one
         * cannot be seated, drop the whole row: a history row that lost
         * its attributes is worse than a missing one. */
        bool stored = true;
        for (int c = 0; c < cols && c < page->cols; ++c) {
            if (!cfr_cell_reintern(vt, page, src, src_cells[c], &dst[c])) {
                stored = false;
                break;
            }
        }
        if (stored) {
            page->lineage[row] = lineage;
            page->row_count++;
            *ring_at(vt, (uint32_t)vt->sb_lines) = (CfrSbRow){ page, row };
            vt->sb_lines++;
            vt->sb_bytes += row_bytes(page->cols);
        } else if (fresh_page) {
            cfr_page_free(vt, page);
        }
    }

    evict_to_capacity(vt);
}

void cfr_scrollback_pop_rows(CfrTerm *vt, int n)
{
    if (!vt || n <= 0)
        return;
    if (n > vt->sb_lines)
        n = vt->sb_lines;
    for (; n > 0; --n) {
        CfrSbRow *slot = ring_at(vt, (uint32_t)vt->sb_lines - 1);
        CfrSbRow gone = *slot;
        CFR_BUG_CHECK(gone.row + 1 == gone.page->row_count,
                      "scrollback pop: row %u of a page filled to %u",
                      gone.row, (unsigned)gone.page->row_count);
        uint64_t bytes = row_bytes(gone.page->cols);
        CFR_BUG_CHECK(vt->sb_bytes >= bytes,
                      "scrollback bytes underflow on pop: held=%llu row=%llu",
                      (unsigned long long)vt->sb_bytes,
                      (unsigned long long)bytes);
        *slot = (CfrSbRow){ 0 };
        vt->sb_bytes -= bytes;
        vt->sb_lines--;
        gone.page->row_count--; /* the slot is free for the next push */
        if (vt->sb_lines == 0 ||
            ring_at(vt, (uint32_t)vt->sb_lines - 1)->page != gone.page)
            cfr_page_free(vt, gone.page);
    }
}

void cfr_scrollback_clear(CfrTerm *vt)
{
    if (!vt)
        return;
    CfrPage *prev = NULL;
    for (uint32_t i = 0; i < (uint32_t)vt->sb_lines; ++i) {
        CfrPage *page = ring_at(vt, i)->page;
        if (page != prev) { /* a page's rows are contiguous: free it once */
            cfr_page_free(vt, page);
            prev = page;
        }
    }
    cfr_dealloc(vt, vt->sb_rows);
    vt->sb_rows = NULL;
    vt->sb_ring_cap = 0;
    vt->sb_first = 0;
    vt->sb_lines = 0;
    vt->sb_bytes = 0;
}

void cfr_scrollback_set_capacity(CfrTerm *vt, int lines)
{
    if (!vt || lines < 0)
        return;
    int before = vt->sb_lines;
    vt->sb_capacity = lines;
    evict_to_capacity(vt);
    if (vt->sb_lines == before)
        return;

    /* Rows left history: drop what still points at them. */
    if (vt->selection.active && vt->selection.start.row < -vt->sb_lines)
        cfr_selection_clear(vt);
    if (vt->images)
        cfr_img_note_scroll(vt, vt->images, 0);
    if (vt->lottie)
        cfr_lottie_note_scroll(vt, 0);
}

const CfrPage *cfr_sb_page_for_row(const CfrTerm *vt, int sb_row,
                                   int *out_row_in_page)
{
    if (!vt || sb_row < 0 || sb_row >= vt->sb_lines)
        return NULL;
    const CfrSbRow *r = ring_at(vt, (uint32_t)(vt->sb_lines - 1 - sb_row));
    *out_row_in_page = (int)r->row;
    return r->page;
}
