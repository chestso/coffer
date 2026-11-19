/*
 * coffer — page allocation.
 *
 * A page is allocated as a single block: the CfrPage header followed
 * by a flexible-array storage region holding the cell grid and the
 * per-row lineage ids. Style intern entries and grapheme arena buffers
 * live separately and grow lazily; a page that never sees a non-default
 * style or multi-codepoint cluster never allocates them.
 *
 * `cfr_page_free` returns the cells+lineage block in one free, plus up
 * to four optional frees for the dynamic sub-tables.
 */

#include "coffer_internal.h"

#include <stdint.h>
#include <string.h>

CfrPage *cfr_page_new(CfrTerm *vt, int rows, int cols)
{
    if (rows <= 0 || cols <= 0)
        return NULL;

    size_t header_bytes = sizeof(CfrPage);
    size_t cells_bytes = (size_t)rows * (size_t)cols * sizeof(CfrCell);
    size_t lineage_bytes = (size_t)rows * sizeof(uint32_t);

    /* Cells must start on a 4-byte boundary; the CfrPage header is
     * already aligned by the allocator (>= 8 on every supported
     * target), and we lay them out cells-then-lineage. */
    size_t total = header_bytes + cells_bytes + lineage_bytes;

    CfrPage *p = cfr_alloc(vt, total);
    if (!p)
        return NULL;

    memset(p, 0, sizeof(*p));
    p->cols = (uint16_t)cols;
    p->row_capacity = (uint16_t)rows;
    p->row_count = (uint16_t)rows;

    /* `storage` is the trailing flexible array. */
    uint8_t *base = (uint8_t *)p + header_bytes;
    p->cells = (CfrCell *)base;
    p->lineage = (uint32_t *)(base + cells_bytes);

    /* cfr_alloc returns uninitialized memory; zero the trailing region
     * (cells *and* lineage — lineage 0 means "blank/unwritten"). */
    memset(p->cells, 0, cells_bytes + lineage_bytes);

    return p;
}

void cfr_page_free(CfrTerm *vt, CfrPage *page)
{
    if (!page)
        return;
    /* Dynamic sub-tables (may be NULL if the page never used them). */
    cfr_dealloc(vt, page->styles.entries);
    cfr_dealloc(vt, page->styles.index);
    cfr_dealloc(vt, page->graphemes.codepoints);
    cfr_dealloc(vt, page->graphemes.dedup_index);
    cfr_hyperlink_free(vt, &page->hyperlinks);
    /* The page header + cells + lineage are one allocation. */
    cfr_dealloc(vt, page);
}

CfrCell cfr_cell_reintern(CfrTerm *vt, CfrPage *dst, const CfrPage *src,
                          CfrCell cell)
{
    /* A NULL or aliased page is a caller bug; passing the cell through
     * would be exactly the raw-id copy this helper exists to prevent. */
    CFR_BUG_CHECK(dst && src && dst != src,
                  "cell reintern: bad pages (dst=%p src=%p)", (void *)dst,
                  (void *)src);

    /* Nothing page-scoped to move (the common case for plain text). */
    if (cell.style_id == 0u && cell.grapheme_id == 0u &&
        cell.hyperlink_id == 0u)
        return cell;

    const CfrStyle *st = cfr_style_lookup(src, cell.style_id);
    cell.style_id = st ? cfr_style_intern(vt, dst, st) : 0u;

    if (cell.grapheme_id != 0u) {
        uint32_t cps[CFR_CLUSTER_MAX];
        size_t n = cfr_grapheme_read(src, cell.grapheme_id, cps,
                                     CFR_CLUSTER_MAX);
        cell.grapheme_id =
            (n >= 2) ? cfr_grapheme_intern(vt, dst, cps, (uint32_t)n) : 0u;
    }

    if (cell.hyperlink_id != 0u)
        cell.hyperlink_id =
            cfr_hyperlink_transfer(vt, dst, src, cell.hyperlink_id);

    return cell;
}
