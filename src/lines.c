/*
 * coffer — wrapped-line identity: lineage ids, the wrap commit, and
 * the single logical-line predicate.
 *
 * A soft wrap is recorded twice, once at each end of the join:
 *
 *   - the WRAPLINE bit on the margin cell of the row above ("this row
 *     ends in a wrap"), which travels with the cell and is therefore
 *     invalidated automatically by every erase / overwrite / DCH that
 *     rewrites that cell;
 *   - a shared nonzero lineage id on both rows ("the row below is still
 *     the same logical line"), which travels with whole-row moves
 *     (scroll, IL, DL, SU, SD) inside the same memmove as the cells.
 *
 * A join is reported only while both ends say yes. That is what makes
 * the representation independent of row indices: a foreign line moving
 * under a stale margin bit brings its own id, and a rewrite of the row
 * above drops the bit. No IL/DL/scroll/erase site needs to know wrap
 * state exists.
 *
 * See docs/wrapped-lines-design.md.
 */

#include "coffer_internal.h"

/* ------------------------------------------------------------------ */
/* Row addressing                                                      */
/* ------------------------------------------------------------------ */

/* Resolve a unified row to the page that owns it and the row index
 * within that page. Unified rows: >= 0 are visible grid rows, < 0 are
 * scrollback (-1 = the line most recently scrolled off). */
static const CfrPage *unified_page(const CfrTerm *vt, int unified_row,
                                   int *out_row_in_page)
{
    if (!vt)
        return NULL;
    if (unified_row >= 0) {
        if (!vt->grid || unified_row >= vt->rows)
            return NULL;
        *out_row_in_page = unified_row;
        return vt->grid;
    }
    int sb_row = -(unified_row + 1);
    return cfr_sb_page_for_row(vt, sb_row, out_row_in_page);
}

static uint32_t lineage_in_page(const CfrPage *page, int row_in_page)
{
    if (!page || row_in_page < 0 || row_in_page >= page->row_capacity)
        return 0;
    return page->lineage[row_in_page];
}

int cfr_row_width(const CfrTerm *vt, int unified_row)
{
    int r = 0;
    const CfrPage *p = unified_page(vt, unified_row, &r);
    return p ? p->cols : 0;
}

/* Assign a fresh logical-line id. Ids start at 1; 0 means blank. A
 * wraparound to 0 is folded back to 1 so a wrapped counter can never
 * alias the blank sentinel. Callers that need an id without touching the
 * grid use this directly (reflow's lineage propagation). */
uint32_t cfr_lineage_next(CfrTerm *vt)
{
    if (!vt)
        return 0;
    if (vt->next_lineage == 0)
        vt->next_lineage = 1;
    return vt->next_lineage++;
}

void cfr_lineage_stamp(CfrTerm *vt, int row)
{
    if (!vt->grid || row < 0 || row >= vt->rows)
        return;
    if (vt->grid->lineage[row] == 0)
        vt->grid->lineage[row] = cfr_lineage_next(vt);
}

/* ------------------------------------------------------------------ */
/* Wrap commit                                                         */
/* ------------------------------------------------------------------ */

/* Resolve the deferred wrap into an actual wrap. Called only from
 * commit_cluster(), so wraps are created only where a print happens.
 *
 * Stamps the wrap edge on the margin cell of the row being left and
 * propagates that row's lineage to the row the print lands on, scrolling
 * the region if the cursor is on the bottom row. The eager wide-char
 * case (a width-2 cluster with one column left) shares this path: the
 * wrap happens on this print because the cluster cannot fit, and it is
 * the same join as a deferred wrap with one print fewer.
 *
 * Returns true when the cursor ended on a different row than it started
 * on (the normal case; false only if the grid is unusable or the row
 * below the cursor is outside the scroll region). */
bool cfr_wrap_commit(CfrTerm *vt)
{
    cfr_grid_ensure(vt);
    if (!vt->grid || vt->cols <= 0)
        return false;

    int cols = vt->cols;
    int src = vt->cursor.row;
    if (src < 0)
        src = 0;
    if (src >= vt->rows)
        src = vt->rows - 1;

    /* Give the wrapping row an identity if it does not have one yet, so
     * both ends of the join can carry it. */
    uint32_t id = vt->grid->lineage[src];
    if (id == 0) {
        id = cfr_lineage_next(vt);
        vt->grid->lineage[src] = id;
    }

    /* The wrap edge lives on the margin cell, so erases and overwrites
     * of that cell sever the join with no explicit clear-call. */
    CfrCell *margin = &vt->grid->cells[(size_t)src * cols + (cols - 1)];
    margin->flags |= CFR_CELL_WRAPLINE;

    int dst;
    if (src == vt->scroll_bottom) {
        cfr_scroll_up(vt, 1);
        /* The region scroll moves the stamped row up by one and clears a
         * fresh bottom row; the print lands on that cleared row, which is
         * still index `src`. */
        dst = src;
    } else if (src < vt->rows - 1) {
        dst = src + 1;
    } else {
        /* Bottom row but not the scroll margin (a partial region below
         * the cursor): nothing to scroll, no wrap possible. */
        return false;
    }

    /* Propagate the lineage onto the destination row: this is what makes
     * the join two-sided, and what survives whole-row moves. */
    vt->grid->lineage[dst] = id;
    vt->cursor.row = dst;
    vt->cursor.col = 0;
    return true;
}

/* ------------------------------------------------------------------ */
/* The predicate                                                       */
/* ------------------------------------------------------------------ */

bool cfr_row_continues(const CfrTerm *vt, int unified_row)
{
    if (!vt)
        return false;
    int pr = 0, cr = 0;
    const CfrPage *pp = unified_page(vt, unified_row - 1, &pr);
    const CfrPage *cp = unified_page(vt, unified_row, &cr);
    if (!pp || !cp || pp->cols == 0)
        return false;

    /* End one: the row above still ends in a wrap. */
    const CfrCell *margin =
        &pp->cells[(size_t)pr * pp->cols + (pp->cols - 1)];
    if ((margin->flags & CFR_CELL_WRAPLINE) == 0u)
        return false;

    /* End two: the row below is still the same logical line. */
    uint32_t lp = lineage_in_page(pp, pr);
    uint32_t lc = lineage_in_page(cp, cr);
    return lp != 0 && lp == lc;
}

void cfr_logical_line_bounds(const CfrTerm *vt, int row, int lo_limit,
                             int hi_limit, int *out_start, int *out_end)
{
    int start = row;
    int end = row;
    if (row < lo_limit || row > hi_limit) {
        if (out_start)
            *out_start = row;
        if (out_end)
            *out_end = row;
        return;
    }
    while (start > lo_limit && cfr_row_continues(vt, start))
        start--;
    while (end < hi_limit && cfr_row_continues(vt, end + 1))
        end++;
    if (out_start)
        *out_start = start;
    if (out_end)
        *out_end = end;
}
