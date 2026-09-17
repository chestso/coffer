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
