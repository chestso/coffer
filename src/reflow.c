/*
 * coffer — wrapped-line reflow on resize.
 *
 * Algorithm (Alacritty / foot lineage), one static function per phase:
 *   1. collect_prefix / collect_grid: walk the grid top-to-bottom,
 *      collecting cells into a flat buffer. Continuation cells (width=0)
 *      are skipped — layout_row rebuilds them, and trailing blanks, from
 *      cell width. Every collected cell is re-interned against the OLD
 *      grid, so the flat buffer has exactly one source page.
 *   2. Logical lines are sequences of rows linked by the wrap predicate
 *      (cfr_row_continues: margin-cell WRAPLINE bit plus a shared nonzero
 *      lineage id); `lines[]` records where each line ends in the flat
 *      buffer and the id it carries. The cursor is captured as the index
 *      of the cell at-or-after which it sits.
 *   3. rewrap: for each logical line, lay cells into rows of width
 *      new_cols. A width-2 cluster that doesn't fit ends the row early
 *      (the line continues). Cursor placement falls out of tracking the
 *      row that contains the cursor offset.
 *   4. install: the oldest rows beyond `new_rows` are pushed to
 *      scrollback; the last new_rows land in the new grid. Rows emitted
 *      for one logical line share its lineage id, and every row but the
 *      line's last gets the wrap edge on its new margin cell.
 *
 * Scope: reflow owns the visible grid and the history rows of the line
 * that straddles its top edge. Those rows (at most CFR_REFLOW_PREFIX_ROWS
 * of them, newest first) are pulled back out of the ring and re-emitted
 * with the visible fragment at the new width. Everything older — other
 * lines, and the far end of a line longer than the cap — keeps the column
 * width it was written at, which the mixed-cols iterator handles. Work
 * per resize is therefore bounded by the grid plus the cap, never by the
 * amount of history.
 *
 * Failure: every fallible step runs before the first mutation of vt
 * (the old grid's intern tables only grow), so a false return from any
 * phase falls back to a clamp resize with the terminal intact.
 *
 * Altscreen and "reflow disabled" both fall through to a clamp-only
 * resize that preserves the existing top-left corner of content.
 */

#include "coffer_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct
{
    uint32_t start;   /* inclusive index into the flat cells */
    uint32_t end;     /* exclusive */
    uint32_t lineage; /* logical-line id shared by the line's rows */
    bool wraps;       /* the row continues into the next one */
} NewRow;

/* One pre-reflow logical line: where it ends in the flat cell buffer, and
 * the id it carries (so the rewrap can re-stamp both ends of each join). */
typedef struct
{
    uint32_t end;     /* exclusive cell index where the line ends */
    uint32_t lineage; /* logical-line id */
} Line;

typedef struct
{
    CfrTerm *vt;
    CfrPage *old_grid; /* the one page every flat cell's ids refer to */
    int old_rows, old_cols;
    int new_rows, new_cols;

    /* Phase 1: collected. */
    CfrCell *cells;
    uint32_t cells_count, cells_cap;
    Line *lines;
    uint32_t lines_count, lines_cap;
    int prefix_rows;       /* scrollback rows of the straddling line */
    int32_t cursor_offset; /* flat index of the cursor cell */
    int32_t cursor_line;   /* index into lines[] */

    /* Phase 2: rewrapped. */
    NewRow *rows;
    uint32_t rows_count, rows_cap;
    int32_t cursor_row; /* index into rows[] */
    int cursor_col;
} Reflow;

/* Grow `buf` so `count + need` elements fit; returns the (possibly moved)
 * buffer, or NULL on failure with the old buffer untouched. */
static void *grow(CfrTerm *vt, void *buf, uint32_t *cap, uint32_t count,
                  uint32_t need, size_t elem, uint32_t initial)
{
    if (count + need <= *cap)
        return buf;
    uint32_t nc = *cap ? *cap : initial;
    while (count + need > nc)
        nc *= 2;
    void *p = cfr_realloc(vt, buf, (size_t)nc * elem);
    if (!p)
        return NULL;
    *cap = nc;
    return p;
}

static bool reserve_cells(Reflow *R, uint32_t need)
{
    CfrCell *p = grow(R->vt, R->cells, &R->cells_cap, R->cells_count, need,
                      sizeof(CfrCell), 256);
    if (!p)
        return false;
    R->cells = p;
    return true;
}

static bool push_row(Reflow *R, uint32_t start, uint32_t end,
                     uint32_t lineage, bool wraps)
{
    NewRow *p = grow(R->vt, R->rows, &R->rows_cap, R->rows_count, 1,
                     sizeof(NewRow), 32);
    if (!p)
        return false;
    R->rows = p;
    p[R->rows_count++] = (NewRow){ start, end, lineage, wraps };
    return true;
}

static void reflow_free(Reflow *R)
{
    cfr_dealloc(R->vt, R->cells);
    cfr_dealloc(R->vt, R->lines);
    cfr_dealloc(R->vt, R->rows);
}

/* Rebuild the default 8-column tab stops for a new width. */
static void reset_tabstops(CfrTerm *vt, int cols)
{
    uint8_t *tabs = cfr_alloc(vt, (size_t)cols);
    if (!tabs)
        return;
    for (int i = 0; i < cols; ++i)
        tabs[i] = (i % 8 == 0) ? 1u : 0u;
    cfr_dealloc(vt, vt->tabstops);
    vt->tabstops = tabs;
}

/* ------------------------------------------------------------------ */
/* Clamp-only resize (altscreen / reflow disabled).                    */
/* ------------------------------------------------------------------ */

void cfr_resize_clamp(CfrTerm *vt, int new_rows, int new_cols)
{
    if (new_rows <= 0 || new_cols <= 0)
        return;
    CfrPage *old_grid = vt->grid;

    CfrPage *new_grid = cfr_page_new(vt, new_rows, new_cols);
    if (!new_grid)
        return;

    if (old_grid) {
        int copy_rows = (new_rows < vt->rows) ? new_rows : vt->rows;
        int copy_cols = (new_cols < vt->cols) ? new_cols : vt->cols;
        for (int r = 0; r < copy_rows; ++r) {
            for (int c = 0; c < copy_cols; ++c) {
                CfrCell src = old_grid->cells[(size_t)r * vt->cols + c];
                new_grid->cells[(size_t)r * new_cols + c] =
                    cfr_cell_reintern(vt, new_grid, old_grid, src);
            }
            /* Keep the lineage id; a clamp resize preserves row identity. */
            new_grid->lineage[r] = old_grid->lineage[r];
        }
        cfr_page_free(vt, old_grid);
    }

    vt->grid = new_grid;
    vt->rows = new_rows;
    vt->cols = new_cols;
    vt->scroll_top = 0;
    vt->scroll_bottom = new_rows - 1;
    cfr_cursor_set(vt, vt->cursor.row, vt->cursor.col);

    reset_tabstops(vt, new_cols);
    cfr_damage_all(vt);
}

/* ------------------------------------------------------------------ */
/* Phase 1: collect                                                    */
/* ------------------------------------------------------------------ */

/* A logical line can start above the grid: if the top visible row
 * continues the newest scrollback row, that line's head is in history.
 * Pull back the line's newest CFR_REFLOW_PREFIX_ROWS history rows and
 * rewrap them with the visible rows (the two-sided predicate already
 * answers across the boundary). The cap keeps the work independent of
 * how much history exists: a multi-megabyte line in a huge ring would
 * otherwise be copied, re-interned and rewrapped on every resize step.
 *
 * Rows of the line beyond the cap stay put at their old width. They still
 * join: their margin cell keeps its wrap edge and the rewritten rows carry
 * the same lineage id, which is all the predicate checks.
 *
 * The cells are re-interned against the OLD grid, so the flat buffer never
 * points into a ring page and those rows can be popped once the new grid
 * is allocated. */
static bool collect_prefix(Reflow *R)
{
    CfrTerm *vt = R->vt;
    if (vt->sb_lines <= 0 || !cfr_row_continues(vt, 0))
        return true;

    int rows = 1;
    while (rows < vt->sb_lines && rows < CFR_REFLOW_PREFIX_ROWS &&
           cfr_row_continues(vt, -rows))
        rows++;
    R->prefix_rows = rows;

    for (int sr = rows - 1; sr >= 0; --sr) { /* oldest first */
        int row_in_page = 0;
        const CfrPage *page = cfr_sb_page_for_row(vt, sr, &row_in_page);
        CFR_BUG_CHECK(page != NULL, "reflow prefix: no page for sb row %d",
                      sr);
        if (!page)
            return false;
        int page_cols = page->cols;
        if (!reserve_cells(R, (uint32_t)page_cols))
            return false;
        for (int col = 0; col < page_cols; ++col) {
            CfrCell cell = page->cells[(size_t)row_in_page * page_cols + col];
            if (cell.width == 0)
                continue;
            cell.flags &= (uint8_t)~CFR_CELL_WRAPLINE;
            R->cells[R->cells_count++] =
                cfr_cell_reintern(vt, R->old_grid, page, cell);
        }
    }
    return true;
}

/* Close the logical line that began at grid row `start_row` and ended at
 * row `end_row`. Reuses the first row's own id, or mints one when that
 * row never had content (lineage 0). */
static bool close_line(Reflow *R, int start_row, int end_row, int cursor_row)
{
    Line *p = grow(R->vt, R->lines, &R->lines_cap, R->lines_count, 1,
                   sizeof(Line), 32);
    if (!p)
        return false;
    R->lines = p;
    uint32_t id = R->old_grid->lineage[start_row];
    if (id == 0)
        id = cfr_lineage_next(R->vt);
    p[R->lines_count] = (Line){ R->cells_count, id };
    if (cursor_row >= start_row && cursor_row <= end_row)
        R->cursor_line = (int32_t)R->lines_count;
    R->lines_count++;
    return true;
}

static bool collect_grid(Reflow *R)
{
    const CfrPage *grid = R->old_grid;
    int cursor_row = R->vt->cursor.row;
    int cursor_col = R->vt->cursor.col;
    int line_start_row = 0; /* first grid row of the line being collected */

    if (!reserve_cells(R, (uint32_t)(R->old_rows * R->old_cols + 1)))
        return false;

    for (int r = 0; r < R->old_rows; ++r) {
        uint32_t row_start = R->cells_count;
        int32_t cursor_in_row = -1;

        for (int col = 0; col < R->old_cols; ++col) {
            if (r == cursor_row && col == cursor_col)
                cursor_in_row = (int32_t)(R->cells_count - row_start);
            CfrCell cell = grid->cells[(size_t)r * R->old_cols + col];
            if (cell.width == 0)
                continue;
            /* Wrap edges are re-stamped on the new margin cells; a stale
             * bit carried in the flat buffer could land on an interior
             * cell and spuriously join. */
            cell.flags &= (uint8_t)~CFR_CELL_WRAPLINE;
            R->cells[R->cells_count++] = cell;
        }
        if (r == cursor_row) {
            if (cursor_in_row < 0)
                cursor_in_row = (int32_t)(R->cells_count - row_start);
            R->cursor_offset = (int32_t)(row_start + (uint32_t)cursor_in_row);
        }

        /* Row r continues into r+1 when the two-sided predicate says so. */
        bool wraps = (r + 1 < R->old_rows) && cfr_row_continues(R->vt, r + 1);
        if (!wraps) {
            if (!close_line(R, line_start_row, r, cursor_row))
                return false;
            line_start_row = r + 1;
        }
    }
    /* The last row never wraps, so it always closes a line. */
    CFR_BUG_CHECK(R->lines_count > 0, "reflow collect: no lines for %d rows",
                  R->old_rows);

    if (R->cursor_offset < 0)
        R->cursor_offset = (int32_t)R->cells_count;
    if (R->cursor_line < 0)
        R->cursor_line = (int32_t)R->lines_count - 1;
    return true;
}

/* Trim trailing blank logical lines that don't host the cursor. The
 * original grid often has blank rows (spaces from \e[2J or uninitialized
 * cells) below content; propagating them through reflow would push real
 * content into scrollback unnecessarily. */
static void trim_blank_lines(Reflow *R)
{
    while (R->lines_count > 0) {
        uint32_t end = R->lines[R->lines_count - 1].end;
        uint32_t start =
            (R->lines_count > 1) ? R->lines[R->lines_count - 2].end : 0u;
        /* A blank cell is one whose codepoint is space (0x20) or NUL. */
        bool has_content = false;
        for (uint32_t k = start; k < end; ++k) {
            uint32_t cp = R->cells[k].cp;
            if (cp != 0u && cp != 0x20u) {
                has_content = true;
                break;
            }
        }
        if (has_content)
            break;
        if (R->cursor_line == (int32_t)(R->lines_count - 1))
            break;
        R->lines_count--;
        R->cells_count = start;
    }
}

/* ------------------------------------------------------------------ */
/* Phase 2: rewrap                                                     */
/* ------------------------------------------------------------------ */

static bool rewrap(Reflow *R)
{
    bool cursor_placed = false;
    uint32_t line_start = 0;

    for (uint32_t li = 0; li < R->lines_count; ++li) {
        const Line *line = &R->lines[li];
        bool is_cursor_line = ((int32_t)li == R->cursor_line);
        uint32_t i = line_start;

        /* do/while: an empty line still emits one (empty) row. */
        do {
            uint32_t row_start = i;
            int row_used = 0;

            while (i < line->end) {
                int w = R->cells[i].width;
                if (w <= 0) {
                    ++i;
                    continue;
                }
                if (row_used + w > R->new_cols)
                    break;
                row_used += w;
                ++i;
            }
            if (i == row_start && i < line->end)
                ++i; /* a cluster wider than the grid: force progress */

            if (!cursor_placed && is_cursor_line) {
                uint32_t off = (uint32_t)R->cursor_offset;
                if (off >= row_start && off < i) {
                    int w = 0;
                    for (uint32_t k = row_start; k < off; ++k)
                        w += R->cells[k].width;
                    R->cursor_row = (int32_t)R->rows_count;
                    R->cursor_col = w;
                    cursor_placed = true;
                } else if (off == i) {
                    R->cursor_row = (int32_t)R->rows_count;
                    R->cursor_col =
                        (row_used >= R->new_cols) ? R->new_cols - 1 : row_used;
                    cursor_placed = true;
                }
            }
            if (!push_row(R, row_start, i, line->lineage, i < line->end))
                return false;
        } while (i < line->end);

        line_start = line->end;
    }
    if (!cursor_placed) {
        R->cursor_row = (R->rows_count > 0) ? (int32_t)(R->rows_count - 1) : 0;
        R->cursor_col = 0;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Phase 3: install                                                    */
/* ------------------------------------------------------------------ */

/* Lay one rewrapped row into `dst` (new_cols cells). The cells still
 * carry old-grid ids: the caller re-interns them into the page that
 * receives the row. */
static void layout_row(const Reflow *R, const NewRow *nr, CfrCell *dst)
{
    int cols = R->new_cols;
    memset(dst, 0, (size_t)cols * sizeof(CfrCell));
    int col = 0;
    for (uint32_t k = nr->start; k < nr->end && col < cols; ++k) {
        CfrCell cell = R->cells[k];
        int w = cell.width;
        if (col + w > cols)
            break;
        dst[col] = cell;
        if (w == 2 && col + 1 < cols) {
            CfrCell cont = (CfrCell){ 0 };
            cont.style_id = cell.style_id;
            dst[col + 1] = cont;
        }
        col += w;
    }
    /* The wrap edge belongs to the new geometry's margin cell only. */
    if (nr->wraps)
        dst[cols - 1].flags |= CFR_CELL_WRAPLINE;
}

static bool install(Reflow *R)
{
    CfrTerm *vt = R->vt;
    int new_rows = R->new_rows;
    int new_cols = R->new_cols;

    int total = (int)R->rows_count;
    int sb_count = (total > new_rows) ? total - new_rows : 0;
    int visible_count = total - sb_count;

    /* distribute partitions [0..total) into sb-bound + visible; the
     * per-row range checks catch a rewrap bug that left a NewRow pointing
     * outside the flat buffer. */
    CFR_BUG_CHECK(visible_count <= new_rows,
                  "reflow distribute: visible=%d > new_rows=%d",
                  visible_count, new_rows);
    for (uint32_t k = 0; k < R->rows_count; ++k) {
        CFR_BUG_CHECK(R->rows[k].start <= R->rows[k].end,
                      "reflow row %u: start=%u > end=%u", k, R->rows[k].start,
                      R->rows[k].end);
        CFR_BUG_CHECK(R->rows[k].end <= R->cells_count,
                      "reflow row %u: end=%u beyond cells_count=%u", k,
                      R->rows[k].end, R->cells_count);
    }

    /* Every allocation reflow itself makes happens before the first
     * mutation below, so a failure here still falls back to clamp. */
    CfrPage *new_grid = cfr_page_new(vt, new_rows, new_cols);
    if (!new_grid)
        return false;
    CfrCell *row_buf = cfr_alloc(vt, (size_t)new_cols * sizeof(CfrCell));
    if (!row_buf) {
        cfr_page_free(vt, new_grid);
        return false;
    }

    /* The rows pulled out of history are in the flat buffer (re-interned
     * against the old grid), so they can leave the ring now. (A later
     * ring-page allocation failure inside cfr_scrollback_push still drops
     * that row; push has no way to report it.) */
    if (R->prefix_rows > 0)
        cfr_scrollback_pop_rows(vt, R->prefix_rows);

    /* Oldest rows go to scrollback; the push re-interns from old_grid. */
    for (int i = 0; i < sb_count; ++i) {
        layout_row(R, &R->rows[i], row_buf);
        cfr_scrollback_push(vt, R->old_grid, row_buf, new_cols,
                            R->rows[i].lineage);
    }

    /* The newest rows land in the new grid. */
    for (int i = 0; i < visible_count; ++i) {
        const NewRow *nr = &R->rows[sb_count + i];
        layout_row(R, nr, row_buf);
        CfrCell *dst = &new_grid->cells[(size_t)i * new_cols];
        for (int c = 0; c < new_cols; ++c)
            dst[c] = cfr_cell_reintern(vt, new_grid, R->old_grid, row_buf[c]);
        new_grid->lineage[i] = nr->lineage;
    }
    cfr_dealloc(vt, row_buf);

    cfr_page_free(vt, R->old_grid);
    vt->grid = new_grid;
    vt->rows = new_rows;
    vt->cols = new_cols;
    vt->scroll_top = 0;
    vt->scroll_bottom = new_rows - 1;

    int target_row = (int)R->cursor_row - sb_count;
    if (target_row < 0)
        cfr_cursor_set(vt, 0, 0);
    else
        cfr_cursor_set(vt, target_row, R->cursor_col);

    reset_tabstops(vt, new_cols);
    cfr_damage_all(vt);
    return true;
}

/* ------------------------------------------------------------------ */
/* Reflow                                                              */
/* ------------------------------------------------------------------ */

void cfr_reflow(CfrTerm *vt, int new_rows, int new_cols)
{
    if (!vt)
        return;
    if (new_rows <= 0 || new_cols <= 0)
        return;
    if (new_rows == vt->rows && new_cols == vt->cols)
        return;

    /* Altscreen and reflow-disabled fall through to clamp resize. */
    if (vt->in_altscreen || !vt->reflow_enabled) {
        cfr_resize_clamp(vt, new_rows, new_cols);
        return;
    }

    cfr_flush_cluster(vt);

    if (!vt->grid) {
        cfr_resize_clamp(vt, new_rows, new_cols);
        return;
    }

    Reflow R = {
        .vt = vt,
        .old_grid = vt->grid,
        .old_rows = vt->rows,
        .old_cols = vt->cols,
        .new_rows = new_rows,
        .new_cols = new_cols,
        .cursor_offset = -1,
        .cursor_line = -1,
    };

    bool ok = collect_prefix(&R) && collect_grid(&R);
    if (ok) {
        trim_blank_lines(&R);
        ok = rewrap(&R) && install(&R);
    }
    reflow_free(&R);

    /* On failure fall back to clamp so the user isn't left with a broken
     * geometry. */
    if (!ok)
        cfr_resize_clamp(vt, new_rows, new_cols);
}
