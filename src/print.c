/*
 * coffer — grid mutation: cell write, cursor advance, scroll, erase.
 *
 * Plain ASCII printing works end-to-end here; full grapheme cluster
 * accumulation arrives once UAX #29 tables are generated. For now,
 * each codepoint becomes its own cell with width derived from
 * cfr_codepoint_width().
 */

#include "coffer_internal.h"

#include <string.h>

void cfr_grid_ensure(CfrTerm *vt)
{
    if (vt->grid)
        return;
    vt->grid = cfr_page_new(vt, vt->rows, vt->cols);
    /* If allocation fails we leave grid == NULL and subsequent writes
     * become no-ops; a caller that wants strict failure can detect via
     * cfr_get_dimensions returning rows but cfr_get_cell returning
     * NULL. */
}

/* Fill a range of cells with a blank template that carries the current
 * pen's style_id (BCE — Back Color Erase).  Erased cells must inherit
 * the active background colour so that \e[K and \e[J extend the
 * background to the end of line/display, matching xterm/VTE/foot. */
static void erase_cells(CfrTerm *vt, CfrCell *dst, int count)
{
    uint32_t style_id = cfr_style_intern(vt, vt->grid, &vt->cursor.pen);
    CfrCell blank = { 0 };
    blank.cp = 0x20; /* space */
    blank.style_id = style_id;
    blank.width = 1;
    for (int i = 0; i < count; i++)
        dst[i] = blank;
}

static void cursor_clamp(CfrTerm *vt)
{
    if (vt->cursor.row < 0)
        vt->cursor.row = 0;
    if (vt->cursor.col < 0)
        vt->cursor.col = 0;
    if (vt->cursor.row >= vt->rows)
        vt->cursor.row = vt->rows - 1;
    if (vt->cursor.col >= vt->cols)
        vt->cursor.col = vt->cols - 1;
}

void cfr_scroll_up(CfrTerm *vt, int lines)
{
    cfr_grid_ensure(vt);
    if (!vt->grid || lines <= 0)
        return;
    int top = vt->scroll_top;
    int bot = vt->scroll_bottom;
    if (top < 0)
        top = 0;
    if (bot >= vt->rows)
        bot = vt->rows - 1;
    if (top >= bot)
        return;

    if (lines > (bot - top + 1))
        lines = bot - top + 1;

    /* Push lines off the top of the scroll region into scrollback iff
     * the region covers the full screen (no DECSTBM region active,
     * not on altscreen). cfr owns the scrollback storage; the
     * sb_pushline callback fires as a notification only. */
    bool push_to_sb = (top == 0 && bot == vt->rows - 1 && !vt->in_altscreen);
    if (push_to_sb) {
        for (int i = 0; i < lines; ++i) {
            const CfrCell *row = &vt->grid->cells[(size_t)(top + i) * vt->cols];
            bool wrapline =
                (vt->grid->row_flags[top + i] & CFR_CELL_WRAPLINE) != 0u;
            cfr_scrollback_push(vt, row, vt->cols, wrapline);
            if (vt->callbacks.sb_pushline)
                vt->callbacks.sb_pushline(row, vt->cols, wrapline,
                                          vt->callback_user);
        }
        /* A full-screen scroll moves grid row 0 into history: advance the
         * sixel absolute-line baseline so anchored images track the text
         * they sit on, then cull any that scrolled out of scrollback. */
        vt->sixel_abs_top += lines;
        if (vt->sixel)
            cfr_sixel_note_scroll(vt, lines);
        if (vt->lottie)
            cfr_lottie_note_scroll(vt, lines);
    }

    /* Move rows up. */
    int move_count = bot - top - lines + 1;
    if (move_count > 0) {
        memmove(&vt->grid->cells[(size_t)top * vt->cols],
                &vt->grid->cells[(size_t)(top + lines) * vt->cols],
                (size_t)move_count * vt->cols * sizeof(CfrCell));
        memmove(&vt->grid->row_flags[top],
                &vt->grid->row_flags[top + lines],
                (size_t)move_count);
    }
    /* Clear the new bottom rows. */
    int clear_start = bot - lines + 1;
    for (int i = 0; i < lines; ++i)
        erase_cells(vt,
                    &vt->grid->cells[(size_t)(clear_start + i) * vt->cols],
                    vt->cols);
    memset(&vt->grid->row_flags[clear_start], 0, (size_t)lines);

    cfr_damage_all(vt);
}

void cfr_scroll_down(CfrTerm *vt, int lines)
{
    if (!vt->grid || lines <= 0)
        return;
    int top = vt->scroll_top;
    int bot = vt->scroll_bottom;
    if (top < 0)
        top = 0;
    if (bot >= vt->rows)
        bot = vt->rows - 1;
    if (top >= bot)
        return;
    if (lines > (bot - top + 1))
        lines = bot - top + 1;

    int move_count = bot - top - lines + 1;
    if (move_count > 0) {
        memmove(&vt->grid->cells[(size_t)(top + lines) * vt->cols],
                &vt->grid->cells[(size_t)top * vt->cols],
                (size_t)move_count * vt->cols * sizeof(CfrCell));
        memmove(&vt->grid->row_flags[top + lines],
                &vt->grid->row_flags[top],
                (size_t)move_count);
    }
    for (int i = 0; i < lines; ++i)
        erase_cells(vt,
                    &vt->grid->cells[(size_t)(top + i) * vt->cols],
                    vt->cols);
    memset(&vt->grid->row_flags[top], 0, (size_t)lines);

    cfr_damage_all(vt);
}

static void linefeed(CfrTerm *vt)
{
    if (vt->cursor.row == vt->scroll_bottom) {
        cfr_scroll_up(vt, 1);
    } else if (vt->cursor.row < vt->rows - 1) {
        vt->cursor.row++;
    }
}

static void carriage_return(CfrTerm *vt)
{
    vt->cursor.col = 0;
    vt->cursor.pending_wrap = false;
}

static void backspace(CfrTerm *vt)
{
    if (vt->cursor.col > 0) {
        vt->cursor.col--;
    }
    vt->cursor.pending_wrap = false;
}

static void horizontal_tab(CfrTerm *vt)
{
    if (vt->cursor.col >= vt->cols - 1)
        return;
    int c = vt->cursor.col + 1;
    while (c < vt->cols && (!vt->tabstops || !vt->tabstops[c]))
        c++;
    if (c >= vt->cols)
        c = vt->cols - 1;
    vt->cursor.col = c;
    vt->cursor.pending_wrap = false;
}

void cfr_execute_c0(CfrTerm *vt, uint8_t b)
{
    /* Any non-printable control terminates the current cluster. */
    cfr_flush_cluster(vt);
    switch (b) {
    case 0x07: /* BEL */
        if (vt->callbacks.bell)
            vt->callbacks.bell(vt->callback_user);
        return;
    case 0x08:
        backspace(vt);
        return;
    case 0x09:
        horizontal_tab(vt);
        return;
    case 0x0e:
        vt->charset_active = 1;
        return; /* SO / LS1 */
    case 0x0f:
        vt->charset_active = 0;
        return; /* SI / LS0 */
    case 0x0a:  /* LF */
    case 0x0b:  /* VT */
    case 0x0c:  /* FF */
        linefeed(vt);
        /* In LNM=off (default) LF doesn't move column. Standards
         * agree; xterm follows DEC behavior. */
        return;
    case 0x0d:
        carriage_return(vt);
        return;
    case 0x84: /* IND  (C1) */
        linefeed(vt);
        return;
    case 0x85: /* NEL  (C1) */
        linefeed(vt);
        carriage_return(vt);
        return;
    case 0x88: /* HTS — set tab stop at cursor column */
        if (vt->tabstops && vt->cursor.col >= 0 && vt->cursor.col < vt->cols)
            vt->tabstops[vt->cursor.col] = 1;
        return;
    case 0x8d: /* RI — reverse index */
        if (vt->cursor.row == vt->scroll_top)
            cfr_scroll_down(vt, 1);
        else if (vt->cursor.row > 0)
            vt->cursor.row--;
        return;
    default:
        return; /* swallow unknown C0/C1 */
    }
}

/* Commit one cell from the codepoint sequence `cps[len]`. */
static void commit_cluster(CfrTerm *vt, const uint32_t *cps, uint32_t len)
{
    cfr_grid_ensure(vt);
    if (!vt->grid || len == 0)
        return;

    int width = cfr_cluster_width(vt, cps, len);
    if (width <= 0) {
        /* Stray combining mark with no preceding base. Discard rather
         * than write a width-0 cell that would confuse the renderer. */
        return;
    }

    /* Deferred wrap — only when DECAWM is on. */
    if (vt->cursor.pending_wrap) {
        if (vt->cursor.row >= 0 && vt->cursor.row < vt->rows)
            vt->grid->row_flags[vt->cursor.row] |= CFR_CELL_WRAPLINE;
        vt->cursor.col = 0;
        if (vt->cursor.row == vt->scroll_bottom)
            cfr_scroll_up(vt, 1);
        else if (vt->cursor.row < vt->rows - 1)
            vt->cursor.row++;
        vt->cursor.pending_wrap = false;
    }
    cursor_clamp(vt);

    /* Wide cluster at right margin → wrap first (DECAWM only). */
    if (width == 2 && vt->cursor.col == vt->cols - 1 && vt->modes[CFR_MODE_DECAWM]) {
        if (vt->cursor.row >= 0 && vt->cursor.row < vt->rows)
            vt->grid->row_flags[vt->cursor.row] |= CFR_CELL_WRAPLINE;
        vt->cursor.col = 0;
        if (vt->cursor.row == vt->scroll_bottom)
            cfr_scroll_up(vt, 1);
        else if (vt->cursor.row < vt->rows - 1)
            vt->cursor.row++;
    }

    /* IRM: shift existing cells right before writing. */
    if (vt->insert_mode)
        cfr_insert_chars(vt, width);

    CfrCell *cell = &vt->grid->cells[(size_t)vt->cursor.row * vt->cols + vt->cursor.col];
    cell->cp = cps[0];
    cell->grapheme_id = (len > 1) ? cfr_grapheme_intern(vt, vt->grid, cps, len) : 0;
    cell->style_id = cfr_style_intern(vt, vt->grid, &vt->cursor.pen);
    cell->width = (uint8_t)width;
    cell->flags = 0;
    cell->hyperlink_id = vt->cursor.hyperlink_id;

    /* Track last printed codepoint for REP (CSI Ps b). */
    vt->last_char = cps[0];

    if (width == 2 && vt->cursor.col + 1 < vt->cols) {
        CfrCell *cont = &vt->grid->cells[(size_t)vt->cursor.row * vt->cols + vt->cursor.col + 1];
        cont->cp = 0;
        cont->grapheme_id = 0;
        cont->style_id = cell->style_id;
        cont->width = 0;
        cont->flags = 0;
        cont->hyperlink_id = cell->hyperlink_id;
    }

    cfr_damage_cell(vt, vt->cursor.row, vt->cursor.col);

    if (vt->cursor.col + width >= vt->cols) {
        if (vt->modes[CFR_MODE_DECAWM]) {
            vt->cursor.col = vt->cols - 1;
            vt->cursor.pending_wrap = true;
        } else {
            /* No autowrap: character at right margin overwrites
             * the last column and cursor stays put. */
            vt->cursor.col = vt->cols - 1;
        }
    } else {
        vt->cursor.col += width;
    }
}

void cfr_flush_cluster(CfrTerm *vt)
{
    if (vt->cursor.cluster_len == 0)
        return;
    commit_cluster(vt, vt->cursor.cluster_buf, vt->cursor.cluster_len);
    vt->cursor.cluster_len = 0;
}

/* DEC special graphics charset: ESC ( 0 selects this for G0. While the
 * current GL slot is set to '0', GL bytes 0x5F..0x7E translate to the
 * graphic codepoints below. The table is the standard VT100 mapping
 * (xterm "Special Graphics and Line Drawing"). Bytes outside the range
 * pass through unchanged. */
static const uint16_t dec_graphics_table[32] = {
    /* 0x5F */ 0x00A0, /* NBSP */
    /* 0x60 */ 0x25C6, /* ◆ */
    /* 0x61 */ 0x2592, /* ▒ */
    /* 0x62 */ 0x2409, /* HT */
    /* 0x63 */ 0x240C, /* FF */
    /* 0x64 */ 0x240D, /* CR */
    /* 0x65 */ 0x240A, /* LF */
    /* 0x66 */ 0x00B0, /* ° */
    /* 0x67 */ 0x00B1, /* ± */
    /* 0x68 */ 0x2424, /* NL */
    /* 0x69 */ 0x240B, /* VT */
    /* 0x6A */ 0x2518, /* ┘ */
    /* 0x6B */ 0x2510, /* ┐ */
    /* 0x6C */ 0x250C, /* ┌ */
    /* 0x6D */ 0x2514, /* └ */
    /* 0x6E */ 0x253C, /* ┼ */
    /* 0x6F */ 0x23BA, /* ⎺ */
    /* 0x70 */ 0x23BB, /* ⎻ */
    /* 0x71 */ 0x2500, /* ─ */
    /* 0x72 */ 0x23BC, /* ⎼ */
    /* 0x73 */ 0x23BD, /* ⎽ */
    /* 0x74 */ 0x251C, /* ├ */
    /* 0x75 */ 0x2524, /* ┤ */
    /* 0x76 */ 0x2534, /* ┴ */
    /* 0x77 */ 0x252C, /* ┬ */
    /* 0x78 */ 0x2502, /* │ */
    /* 0x79 */ 0x2264, /* ≤ */
    /* 0x7A */ 0x2265, /* ≥ */
    /* 0x7B */ 0x03C0, /* π */
    /* 0x7C */ 0x2260, /* ≠ */
    /* 0x7D */ 0x00A3, /* £ */
    /* 0x7E */ 0x00B7, /* · */
};

static uint32_t apply_charset(CfrTerm *vt, uint32_t cp)
{
    uint8_t designation = vt->charset[vt->charset_active];
    if (designation == '0' && cp >= 0x5F && cp <= 0x7E)
        return dec_graphics_table[cp - 0x5F];
    return cp;
}

void cfr_print_codepoint(CfrTerm *vt, uint32_t cp)
{
    cp = apply_charset(vt, cp);
    /* Empty cluster — start one with this codepoint. */
    if (vt->cursor.cluster_len == 0) {
        vt->cursor.cluster_buf[0] = cp;
        vt->cursor.cluster_len = 1;
        return;
    }
    uint32_t prev = vt->cursor.cluster_buf[vt->cursor.cluster_len - 1];
    if (cfr_grapheme_break_before(prev, cp, NULL)) {
        /* Boundary: commit the pending cluster, then start a new one. */
        commit_cluster(vt, vt->cursor.cluster_buf, vt->cursor.cluster_len);
        vt->cursor.cluster_buf[0] = cp;
        vt->cursor.cluster_len = 1;
        return;
    }
    /* No break: extend the current cluster (cap at CFR_CLUSTER_MAX). */
    if (vt->cursor.cluster_len < CFR_CLUSTER_MAX) {
        vt->cursor.cluster_buf[vt->cursor.cluster_len++] = cp;
    }
    /* If full, silently drop additional extends — extremely rare. */
}

void cfr_erase_in_line(CfrTerm *vt, int mode)
{
    if (!vt->grid)
        return;
    int row = vt->cursor.row;
    if (row < 0 || row >= vt->rows)
        return;
    vt->cursor.pending_wrap = false;
    CfrCell *line = &vt->grid->cells[(size_t)row * vt->cols];
    int from = 0, to = vt->cols;
    switch (mode) {
    case 0:
        from = vt->cursor.col;
        to = vt->cols;
        break;
    case 1:
        from = 0;
        to = vt->cursor.col + 1;
        break;
    case 2:
        from = 0;
        to = vt->cols;
        break;
    default:
        return;
    }
    if (from < 0)
        from = 0;
    if (to > vt->cols)
        to = vt->cols;
    if (from >= to)
        return;
    erase_cells(vt, &line[from], to - from);
    cfr_damage_row(vt, row);
}

void cfr_insert_chars(CfrTerm *vt, int count)
{
    if (!vt->grid || count <= 0)
        return;
    int row = vt->cursor.row;
    int col = vt->cursor.col;
    if (row < 0 || row >= vt->rows || col < 0 || col >= vt->cols)
        return;
    if (count > vt->cols - col)
        count = vt->cols - col;

    CfrCell *line = &vt->grid->cells[(size_t)row * vt->cols];
    int move = vt->cols - col - count;
    if (move > 0) {
        memmove(&line[col + count], &line[col],
                (size_t)move * sizeof(CfrCell));
    }
    erase_cells(vt, &line[col], count);
    cfr_damage_row(vt, row);
}

void cfr_delete_chars(CfrTerm *vt, int count)
{
    if (!vt->grid || count <= 0)
        return;
    int row = vt->cursor.row;
    int col = vt->cursor.col;
    if (row < 0 || row >= vt->rows || col < 0 || col >= vt->cols)
        return;
    if (count > vt->cols - col)
        count = vt->cols - col;

    CfrCell *line = &vt->grid->cells[(size_t)row * vt->cols];
    int move = vt->cols - col - count;
    if (move > 0) {
        memmove(&line[col], &line[col + count],
                (size_t)move * sizeof(CfrCell));
    }
    erase_cells(vt, &line[vt->cols - count], count);
    cfr_damage_row(vt, row);
}

void cfr_erase_chars(CfrTerm *vt, int count)
{
    if (!vt->grid || count <= 0)
        return;
    vt->cursor.pending_wrap = false;
    int row = vt->cursor.row;
    int col = vt->cursor.col;
    if (row < 0 || row >= vt->rows || col < 0 || col >= vt->cols)
        return;
    if (count > vt->cols - col)
        count = vt->cols - col;
    CfrCell *line = &vt->grid->cells[(size_t)row * vt->cols];
    erase_cells(vt, &line[col], count);
    cfr_damage_row(vt, row);
}

void cfr_insert_lines(CfrTerm *vt, int count)
{
    if (!vt->grid || count <= 0)
        return;
    int row = vt->cursor.row;
    if (row < vt->scroll_top || row > vt->scroll_bottom)
        return;
    int region_height = vt->scroll_bottom - row + 1;
    if (count > region_height)
        count = region_height;
    int move = region_height - count;
    if (move > 0) {
        memmove(&vt->grid->cells[(size_t)(row + count) * vt->cols],
                &vt->grid->cells[(size_t)row * vt->cols],
                (size_t)move * vt->cols * sizeof(CfrCell));
        memmove(&vt->grid->row_flags[row + count],
                &vt->grid->row_flags[row],
                (size_t)move);
    }
    for (int i = 0; i < count; ++i)
        erase_cells(vt,
                    &vt->grid->cells[(size_t)(row + i) * vt->cols],
                    vt->cols);
    memset(&vt->grid->row_flags[row], 0, (size_t)count);
    cfr_damage_all(vt);
    vt->cursor.col = 0;
    vt->cursor.pending_wrap = false;
}

void cfr_delete_lines(CfrTerm *vt, int count)
{
    if (!vt->grid || count <= 0)
        return;
    int row = vt->cursor.row;
    if (row < vt->scroll_top || row > vt->scroll_bottom)
        return;
    int region_height = vt->scroll_bottom - row + 1;
    if (count > region_height)
        count = region_height;
    int move = region_height - count;
    if (move > 0) {
        memmove(&vt->grid->cells[(size_t)row * vt->cols],
                &vt->grid->cells[(size_t)(row + count) * vt->cols],
                (size_t)move * vt->cols * sizeof(CfrCell));
        memmove(&vt->grid->row_flags[row],
                &vt->grid->row_flags[row + count],
                (size_t)move);
    }
    int clear_start = vt->scroll_bottom - count + 1;
    for (int i = 0; i < count; ++i)
        erase_cells(vt,
                    &vt->grid->cells[(size_t)(clear_start + i) * vt->cols],
                    vt->cols);
    memset(&vt->grid->row_flags[clear_start], 0, (size_t)count);
    cfr_damage_all(vt);
    vt->cursor.col = 0;
    vt->cursor.pending_wrap = false;
}

void cfr_erase_in_display(CfrTerm *vt, int mode)
{
    cfr_grid_ensure(vt);
    if (!vt->grid)
        return;
    int row = vt->cursor.row;
    switch (mode) {
    case 0:
        cfr_erase_in_line(vt, 0);
        for (int r = row + 1; r < vt->rows; ++r) {
            erase_cells(vt, &vt->grid->cells[(size_t)r * vt->cols],
                        vt->cols);
            vt->grid->row_flags[r] = 0;
        }
        if (vt->sixel)
            cfr_sixel_clear_display_rows(vt, row, vt->rows - 1);
        if (vt->lottie)
            cfr_lottie_clear_display_rows(vt, row, vt->rows - 1);
        break;
    case 1:
        cfr_erase_in_line(vt, 1);
        for (int r = 0; r < row; ++r) {
            erase_cells(vt, &vt->grid->cells[(size_t)r * vt->cols],
                        vt->cols);
            vt->grid->row_flags[r] = 0;
        }
        if (vt->sixel)
            cfr_sixel_clear_display_rows(vt, 0, row);
        if (vt->lottie)
            cfr_lottie_clear_display_rows(vt, 0, row);
        break;
    case 2:
    case 3:
        for (int r = 0; r < vt->rows; ++r)
            erase_cells(vt, &vt->grid->cells[(size_t)r * vt->cols],
                        vt->cols);
        memset(vt->grid->row_flags, 0, (size_t)vt->rows);
        if (vt->sixel)
            cfr_sixel_clear_display_rows(vt, 0, vt->rows - 1);
        if (vt->lottie)
            cfr_lottie_clear_display_rows(vt, 0, vt->rows - 1);
        break;
    default:
        return;
    }
    cfr_damage_all(vt);
}
