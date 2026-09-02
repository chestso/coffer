/*
 * coffer — text selection (scroll-aware, draw-aware)
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#include "coffer_internal.h"

#include <coffer/coffer.h>

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static const char *default_word_chars =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-./~";

static int selpt_cmp(CfrSelectionPoint a, CfrSelectionPoint b)
{
    if (a.row < b.row)
        return -1;
    if (a.row > b.row)
        return 1;
    if (a.col < b.col)
        return -1;
    if (a.col > b.col)
        return 1;
    return 0;
}

void cfr_selection_fire_callback(CfrTerm *vt, bool active)
{
    if (vt && vt->callbacks.selection_changed)
        vt->callbacks.selection_changed(active, vt->callback_user);
}

void cfr_selection_set_word_chars(CfrTerm *vt, const char *chars)
{
    if (!vt)
        return;
    free(vt->selection_word_chars);
    vt->selection_word_chars = chars ? strdup(chars) : NULL;
}

void cfr_selection_clear(CfrTerm *vt)
{
    if (!vt || !vt->selection.active)
        return;
    vt->selection.active = false;
    vt->selection.mode = CFR_SEL_NONE;
    cfr_selection_fire_callback(vt, false);
}

bool cfr_selection_active(const CfrTerm *vt)
{
    return vt ? vt->selection.active : false;
}

const CfrSelection *cfr_selection_get(const CfrTerm *vt)
{
    if (!vt || !vt->selection.active)
        return NULL;
    return &vt->selection;
}

bool cfr_selection_in_cell(const CfrTerm *vt, int unified_row, int col)
{
    if (!vt || !vt->selection.active)
        return false;
    const CfrSelection *s = &vt->selection;
    CfrSelectionPoint pos = { unified_row, col };
    return selpt_cmp(pos, s->start) >= 0 && selpt_cmp(pos, s->end) <= 0;
}

static int char_class(uint32_t ch, const char *word_chars)
{
    if (ch == 0 || ch == ' ' || ch == '\t')
        return 0;
    if (ch < 128) {
        for (const char *p = word_chars; *p; p++) {
            if ((uint32_t)*p == ch)
                return 1;
        }
    }
    if (ch >= 128)
        return 1;
    return 2;
}

static const CfrCell *read_cell_unified(const CfrTerm *vt, int row, int col)
{
    if (row >= 0)
        return cfr_get_cell(vt, row, col);
    int sb_row = -(row + 1);
    return cfr_get_scrollback_cell(vt, sb_row, col);
}

/* True when unified row `row` continues the previous row (i.e. the row
 * above it soft-wrapped into it). CFR_CELL_WRAPLINE sits on the row that
 * *wrapped into* the next one, so the flag to consult is on `row - 1` —
 * the inverse direction of cfr_get_line_continuation(vt, row), which
 * answers "does *row* wrap into row+1". Walks across the
 * visible/scrollback boundary as well. */
static bool is_continuation_unified(const CfrTerm *vt, int row)
{
    if (!vt)
        return false;
    int prev = row - 1;
    int sb = cfr_get_scrollback_lines(vt);
    if (prev < -sb)
        return false; /* no row above `row` exists at all */
    if (prev >= 0)
        return cfr_get_line_continuation(vt, prev);
    return cfr_get_scrollback_wrapline(vt, -(prev + 1));
}

static void expand_word(const CfrTerm *vt, int row, int col,
                        CfrSelectionPoint *out_start,
                        CfrSelectionPoint *out_end)
{
    const char *wchars =
        vt->selection_word_chars ? vt->selection_word_chars : default_word_chars;
    int cols = vt->cols;

    const CfrCell *cell = read_cell_unified(vt, row, col);
    if (!cell) {
        *out_start = (CfrSelectionPoint){ row, col };
        *out_end = (CfrSelectionPoint){ row, col };
        return;
    }
    int cls = char_class(cell->cp, wchars);

    /* Scan left */
    int scan_row = row, left = col;
    while (left > 0 || is_continuation_unified(vt, scan_row)) {
        if (left > 0) {
            const CfrCell *c = read_cell_unified(vt, scan_row, left - 1);
            if (!c || char_class(c->cp, wchars) != cls)
                break;
            left--;
        } else {
            if (!is_continuation_unified(vt, scan_row))
                break;
            scan_row--;
            left = cols - 1;
            const CfrCell *c = read_cell_unified(vt, scan_row, left);
            if (!c || char_class(c->cp, wchars) != cls)
                break;
        }
    }

    /* Scan right */
    int scan_row_r = row, right = col;
    while (right < cols - 1 || is_continuation_unified(vt, scan_row_r + 1)) {
        if (right < cols - 1) {
            const CfrCell *c = read_cell_unified(vt, scan_row_r, right + 1);
            if (!c || char_class(c->cp, wchars) != cls)
                break;
            right++;
        } else {
            if (!is_continuation_unified(vt, scan_row_r + 1))
                break;
            scan_row_r++;
            right = 0;
            const CfrCell *c = read_cell_unified(vt, scan_row_r, right);
            if (!c || char_class(c->cp, wchars) != cls)
                break;
        }
    }

    *out_start = (CfrSelectionPoint){ scan_row, left };
    *out_end = (CfrSelectionPoint){ scan_row_r, right };
}

void cfr_selection_start(CfrTerm *vt, int row, int col, CfrSelectionMode mode)
{
    if (!vt)
        return;

    CfrSelection *sel = &vt->selection;
    sel->active = true;
    sel->mode = mode;
    sel->anchor = (CfrSelectionPoint){ row, col };

    switch (mode) {
    case CFR_SEL_CHAR:
        sel->start = sel->end = (CfrSelectionPoint){ row, col };
        break;
    case CFR_SEL_WORD:
        expand_word(vt, row, col, &sel->start, &sel->end);
        break;
    case CFR_SEL_LINE:
        sel->start = (CfrSelectionPoint){ row, 0 };
        sel->end = (CfrSelectionPoint){ row, vt->cols - 1 };
        break;
    default:
        sel->active = false;
        break;
    }

    if (sel->active)
        cfr_selection_fire_callback(vt, true);
}

void cfr_selection_update(CfrTerm *vt, int row, int col)
{
    if (!vt || !vt->selection.active)
        return;

    CfrSelection *sel = &vt->selection;
    CfrSelectionPoint cursor = { row, col };

    switch (sel->mode) {
    case CFR_SEL_CHAR:
    {
        if (selpt_cmp(cursor, sel->anchor) < 0) {
            sel->start = cursor;
            sel->end = sel->anchor;
        } else {
            sel->start = sel->anchor;
            sel->end = cursor;
        }
        break;
    }
    case CFR_SEL_WORD:
    {
        CfrSelectionPoint a_start, a_end, c_start, c_end;
        expand_word(vt, sel->anchor.row, sel->anchor.col, &a_start, &a_end);
        expand_word(vt, row, col, &c_start, &c_end);
        sel->start = selpt_cmp(a_start, c_start) < 0 ? a_start : c_start;
        sel->end = selpt_cmp(a_end, c_end) > 0 ? a_end : c_end;
        break;
    }
    case CFR_SEL_LINE:
    {
        if (row < sel->anchor.row) {
            sel->start = (CfrSelectionPoint){ row, 0 };
            sel->end = (CfrSelectionPoint){ sel->anchor.row, vt->cols - 1 };
        } else {
            sel->start = (CfrSelectionPoint){ sel->anchor.row, 0 };
            sel->end = (CfrSelectionPoint){ row, vt->cols - 1 };
        }
        break;
    }
    default:
        break;
    }
}

void cfr_selection_extend(CfrTerm *vt, int row, int col)
{
    if (!vt || !vt->selection.active)
        return;

    CfrSelection *sel = &vt->selection;
    CfrSelectionPoint click = { row, col };

    int cmp_start = selpt_cmp(click, sel->start);
    int cmp_end = selpt_cmp(click, sel->end);

    if (cmp_start <= 0) {
        sel->start = click;
        sel->anchor = sel->end;
    } else if (cmp_end >= 0) {
        sel->end = click;
        sel->anchor = sel->start;
    } else {
        int d_start = (click.row - sel->start.row) * 1000000 +
                      (click.col - sel->start.col);
        int d_end = (sel->end.row - click.row) * 1000000 +
                    (sel->end.col - click.col);
        if (d_start <= d_end) {
            sel->start = click;
            sel->anchor = sel->end;
        } else {
            sel->end = click;
            sel->anchor = sel->start;
        }
    }

    if (selpt_cmp(sel->start, sel->end) > 0) {
        CfrSelectionPoint tmp = sel->start;
        sel->start = sel->end;
        sel->end = tmp;
    }
}

void cfr_selection_on_scroll(CfrTerm *vt, bool up, int lines, int top, int bottom)
{
    if (!vt || !vt->selection.active || lines <= 0)
        return;

    CfrSelection *sel = &vt->selection;
    CfrSelectionPoint *pts[] = { &sel->start, &sel->end, &sel->anchor };

    /* Check for boundary-spanning */
    bool start_in = (sel->start.row >= top && sel->start.row <= bottom) ||
                    (sel->start.row < 0 && top == 0);
    bool end_in = (sel->end.row >= top && sel->end.row <= bottom) ||
                  (sel->end.row < 0 && top == 0);

    /* Determine if the selection is fully inside, fully outside, or spanning */
    bool any_in = false, any_out = false;
    for (int i = 0; i < 3; i++) {
        CfrSelectionPoint *p = pts[i];
        bool in_region;
        if (up) {
            /* Scroll up: content in [top, bottom] moves up.
             * Points above the region (in scrollback or above top for
             * region scrolls) also move. Points below don't. */
            in_region = (p->row <= bottom);
        } else {
            /* Scroll down: content in [top, bottom] moves down.
             * Points in scrollback (row < 0) also move (come back).
             * Points below the region don't. */
            in_region = (p->row < 0) || (p->row >= top && p->row <= bottom);
        }
        if (in_region)
            any_in = true;
        else
            any_out = true;
    }

    /* If the selection spans the boundary (some points affected by the
     * scroll, some not), the selection is incoherent — clear it. */
    if (any_in && any_out) {
        /* But only if it's a partial-region scroll. For full-screen
         * scrolls, all visible rows are inside, and scrollback rows
         * are "above" — both move. So check more carefully. */
        bool full_screen = (top == 0 && bottom == vt->rows - 1);
        if (!full_screen) {
            cfr_selection_clear(vt);
            return;
        }
    }

    int delta = up ? -lines : lines;
    for (int i = 0; i < 3; i++) {
        CfrSelectionPoint *p = pts[i];
        if (up) {
            /* Scroll up: all rows in [0, bottom] and all scrollback
             * rows shift by -lines. Rows below bottom don't move. */
            if (p->row <= bottom)
                p->row += delta; /* delta is negative */
        } else {
            /* Scroll down: scrollback rows and rows in [top, bottom]
             * shift by +lines. Rows above top (visible, outside region)
             * and rows below bottom don't move. */
            if (p->row < 0 || (p->row >= top && p->row <= bottom))
                p->row += delta;
        }
    }
}

void cfr_selection_on_draw(CfrTerm *vt, int row)
{
    if (!vt || !vt->selection.active)
        return;

    /* Check if the drawn row intersects the selection's row range.
     * row is in screen-space (0 = top visible). Selection uses unified
     * coordinates. Only visible selection rows (>= 0) can be hit. */
    const CfrSelection *sel = &vt->selection;
    int sel_start = sel->start.row;
    int sel_end = sel->end.row;

    if (sel_end < 0)
        return; /* selection entirely in scrollback */

    int vis_start = sel_start < 0 ? 0 : sel_start;
    if (row >= vis_start && row <= sel_end)
        cfr_selection_clear(vt);
}

void cfr_selection_free(CfrTerm *vt)
{
    if (!vt)
        return;
    free(vt->selection_word_chars);
    vt->selection_word_chars = NULL;
}

static int codepoint_to_utf8(uint32_t cp, char *buf)
{
    if (cp < 0x80) {
        buf[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else if (cp < 0x110000) {
        buf[0] = (char)(0xF0 | (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

char *cfr_selection_get_text(const CfrTerm *vt)
{
    if (!vt || !vt->selection.active)
        return NULL;

    const CfrSelection *sel = &vt->selection;
    int cols = vt->cols;

    int row_count = sel->end.row - sel->start.row + 1;
    size_t buf_size = (size_t)row_count * (size_t)(cols * 4 + 1) + 1;
    char *buf = malloc(buf_size);
    if (!buf)
        return NULL;

    size_t pos = 0;

    for (int row = sel->start.row; row <= sel->end.row; row++) {
        int col_start = (row == sel->start.row) ? sel->start.col : 0;
        int col_end = (row == sel->end.row) ? sel->end.col : cols - 1;

        size_t row_start_pos = pos;
        size_t last_nonblank_pos = row_start_pos;

        int col = col_start;
        while (col <= col_end) {
            const CfrCell *cell = read_cell_unified(vt, row, col);
            if (!cell) {
                col++;
                continue;
            }

            if (cell->cp == 0) {
                if (pos < buf_size - 1)
                    buf[pos++] = ' ';
                col++;
            } else {
                uint32_t cps[16];
                size_t n_cps = 1;
                cps[0] = cell->cp;
                if (cell->grapheme_id != 0) {
                    n_cps = cfr_cell_get_grapheme(vt, cell, cps,
                                                  sizeof(cps) / sizeof(cps[0]));
                    if (n_cps == 0) {
                        cps[0] = cell->cp;
                        n_cps = 1;
                    }
                }
                for (size_t i = 0; i < n_cps; i++) {
                    char utf8[4];
                    int n = codepoint_to_utf8(cps[i], utf8);
                    if (pos + (size_t)n < buf_size) {
                        memcpy(buf + pos, utf8, n);
                        pos += n;
                    }
                }
                if (cell->cp != 0x20)
                    last_nonblank_pos = pos;
                col += (cell->width == 2) ? 2 : 1;
            }
        }

        /* Strip trailing whitespace at hard line breaks */
        bool next_is_continuation =
            row < sel->end.row && is_continuation_unified(vt, row + 1);
        if (!next_is_continuation)
            pos = last_nonblank_pos;

        if (row < sel->end.row && pos < buf_size - 1) {
            if (!next_is_continuation)
                buf[pos++] = '\n';
        }
    }

    buf[pos] = '\0';
    return buf;
}
