/*
 * coffer — scrollback as a doubly-linked page ring.
 *
 * Each page holds up to CFR_SB_PAGE_ROWS chronological rows. New
 * lines are appended to the head page (newest); when the head page
 * fills, a new page is allocated at the front. When the configured
 * line cap is exceeded, the tail page (oldest) is freed wholesale —
 * one `free` returns up to CFR_SB_PAGE_ROWS lines plus their style
 * intern table and grapheme arena. No fragmentation accumulates.
 *
 * Cells are re-interned into the receiving page's tables: style_id,
 * grapheme_id and hyperlink_id are page-scoped, so a pushed row names
 * the page its ids refer to (`src`) and each cell is reseated into the
 * scrollback page. This keeps page lifetimes independent.
 *
 * Layout convention:
 *   vt->sb_head  = newest page (push target)
 *   vt->sb_tail  = oldest page (eviction target)
 *   page->next   = older neighbor (toward tail)
 *   page->prev   = newer neighbor (toward head)
 *
 * Reading: sb_row 0 = newest. Within a page, rows are stored
 * chronologically; the newest in each page is at index `row_count - 1`.
 */

#include "coffer_internal.h"

#include <string.h>

static CfrPage *new_sb_page(CfrTerm *vt, int cols)
{
    CfrPage *p = cfr_page_new(vt, CFR_SB_PAGE_ROWS, cols);
    if (!p)
        return NULL;
    /* cfr_page_new sets row_count to row_capacity (full page); for
     * scrollback we treat row_count as "rows-used-so-far" and grow it
     * up to row_capacity. Reset to empty. */
    p->row_count = 0;
    p->next = NULL;
    p->prev = NULL;
    return p;
}

static bool ensure_head(CfrTerm *vt, int cols)
{
    if (vt->sb_head && vt->sb_head->row_count < vt->sb_head->row_capacity && vt->sb_head->cols == cols) {
        return true;
    }
    CfrPage *page = new_sb_page(vt, cols);
    if (!page)
        return false;
    /* Insert as new head. */
    page->next = vt->sb_head;
    if (vt->sb_head)
        vt->sb_head->prev = page;
    vt->sb_head = page;
    if (!vt->sb_tail)
        vt->sb_tail = page;
    return true;
}

static void evict_to_capacity(CfrTerm *vt)
{
    while (vt->sb_lines > vt->sb_capacity && vt->sb_tail) {
        CfrPage *tail = vt->sb_tail;
        vt->sb_hint_page = NULL;  /* the hint may name the page we free */
        vt->sb_tail = tail->prev; /* newer than the evicted page */
        if (vt->sb_tail)
            vt->sb_tail->next = NULL;
        else
            vt->sb_head = NULL; /* the only page */
        vt->sb_lines -= tail->row_count;
        cfr_page_free(vt, tail);
    }
}

void cfr_scrollback_push(CfrTerm *vt, const CfrPage *src,
                         const CfrCell *src_cells, int cols, uint32_t lineage)
{
    if (!vt || cols <= 0 || !src || !src_cells)
        return;
    vt->sb_hint_page = NULL; /* the ring changes under the hint */

    /* Store the row, if the ring retains anything and can allocate. */
    const CfrCell *stored = NULL;
    if (vt->sb_capacity > 0 && ensure_head(vt, cols)) {
        CfrPage *head = vt->sb_head;
        /* Invariant: the push reads style/grapheme/hyperlink ids from
         * `src` and writes them into head. If a caller ever passes the
         * head AS the source, source and dest alias — the intern table
         * grows mid-read and any held pointer dangles. Fail loud at the
         * door. */
        CFR_BUG_CHECK(head != src,
                      "scrollback push target page is also the source (%p)",
                      (void *)head);
        int row = head->row_count;
        CfrCell *dst = &head->cells[(size_t)row * head->cols];
        head->lineage[row] = lineage;

        /* Re-intern style, grapheme, and hyperlink references into the
         * head page — page-scoped tables don't carry across pages. */
        for (int c = 0; c < cols && c < head->cols; ++c)
            dst[c] = cfr_cell_reintern(vt, head, src, src_cells[c]);
        head->row_count++;
        vt->sb_lines++;
        stored = dst;
    }

    /* The one place a row enters history, hence the one place the host is
     * told. Hand out the retained copy when there is one: its ids resolve
     * through the cfr_cell_* accessors. With scrollback off (or on an
     * allocation failure) the caller's cells go out as they are. */
    if (vt->callbacks.sb_pushline)
        vt->callbacks.sb_pushline(stored ? stored : src_cells, cols, lineage,
                                  vt->callback_user);

    evict_to_capacity(vt);
}

void cfr_scrollback_pop_rows(CfrTerm *vt, int n)
{
    if (!vt || n <= 0)
        return;
    vt->sb_hint_page = NULL;
    if (n > vt->sb_lines)
        n = vt->sb_lines;
    const int popped = n;
    while (n > 0) {
        CfrPage *head = vt->sb_head;
        CFR_BUG_CHECK(head != NULL, "scrollback pop: %d rows left, no head",
                      n);
        if (!head)
            break;
        int take = (n < (int)head->row_count) ? n : (int)head->row_count;
        head->row_count = (uint16_t)(head->row_count - (uint16_t)take);
        vt->sb_lines -= take;
        n -= take;
        if (head->row_count == 0) {
            vt->sb_head = head->next; /* older neighbor */
            if (vt->sb_head)
                vt->sb_head->prev = NULL;
            else
                vt->sb_tail = NULL;
            cfr_page_free(vt, head);
        }
    }
    CFR_BUG_CHECK(vt->sb_lines >= 0, "scrollback pop: sb_lines=%d",
                  vt->sb_lines);
    if (popped > 0 && vt->callbacks.sb_truncate)
        vt->callbacks.sb_truncate(popped, vt->callback_user);
}

void cfr_scrollback_clear(CfrTerm *vt)
{
    if (!vt)
        return;
    vt->sb_hint_page = NULL;
    CfrPage *p = vt->sb_head;
    while (p) {
        CfrPage *next = p->next;
        cfr_page_free(vt, p);
        p = next;
    }
    vt->sb_head = NULL;
    vt->sb_tail = NULL;
    vt->sb_lines = 0;
}

/* Locate scrollback row `sb_row` (0 = newest). Starts from the page the
 * previous lookup found and walks whichever way the target lies, so a
 * scan over neighbouring rows — a logical-line walk, a word scan, a
 * renderer reading consecutive cells — costs O(1) per step rather than
 * O(pages) from the head. A cold lookup still walks from the head.
 *
 * The hint is a cache behind a const interface: the object is never const
 * (cfr_new allocates it mutable), so refreshing it is well-defined; it does
 * mean two threads must not use the const accessors on one term at once. */
static const CfrPage *find_sb_row(const CfrTerm *vt, int sb_row,
                                  int *out_row_in_page)
{
    if (!vt || sb_row < 0 || sb_row >= vt->sb_lines)
        return NULL;
    const CfrPage *p = vt->sb_hint_page;
    int before = vt->sb_hint_before; /* rows in pages newer than p */
    if (!p) {
        p = vt->sb_head;
        before = 0;
    }
    while (p && sb_row < before) { /* toward newer */
        p = p->prev;
        if (p)
            before -= p->row_count;
    }
    while (p && sb_row >= before + p->row_count) { /* toward older */
        before += p->row_count;
        p = p->next;
    }
    if (!p)
        return NULL;
    CfrTerm *m = (CfrTerm *)vt;
    m->sb_hint_page = p;
    m->sb_hint_before = before;
    *out_row_in_page = p->row_count - 1 - (sb_row - before);
    return p;
}

const CfrPage *cfr_sb_page_for_row(const CfrTerm *vt, int sb_row,
                                   int *out_row_in_page)
{
    return find_sb_row(vt, sb_row, out_row_in_page);
}
