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

/* Last column of unified `row`. Scrollback rows keep the width they were
 * written at, so the grid's width is the wrong bound for them. A row that
 * does not exist ends at column 0. */
static int last_col(const CfrTerm *vt, int row)
{
    int w = cfr_row_width(vt, row);
    return w > 0 ? w - 1 : 0;
}

static void expand_word(const CfrTerm *vt, int row, int col,
                        CfrSelectionPoint *out_start,
                        CfrSelectionPoint *out_end)
{
    const char *wchars =
        vt->selection_word_chars ? vt->selection_word_chars : default_word_chars;

    const CfrCell *cell = cfr_get_cell(vt, row, col);
    if (!cell) {
        *out_start = (CfrSelectionPoint){ row, col };
        *out_end = (CfrSelectionPoint){ row, col };
        return;
    }
    int cls = char_class(cell->cp, wchars);

    /* Bound the scan to the logical line: walks cross wrap boundaries via
     * the two-sided predicate, and rows can have different widths across
     * the scrollback boundary. */
    int sb = cfr_get_scrollback_lines(vt);
    int ls = row, le = row;
    cfr_logical_line_bounds(vt, row, -sb, vt->rows - 1, &ls, &le);

    int sr = row, sc = col;
    int r = row, c = col;
    while (r > ls || c > 0) {
        if (c > 0) {
            c--;
        } else {
            r--;
            c = cfr_row_width(vt, r) - 1;
            if (c < 0)
                break;
        }
        const CfrCell *x = cfr_get_cell(vt, r, c);
        if (!x || char_class(x->cp, wchars) != cls)
            break;
        sr = r;
        sc = c;
    }

    int er = row, ec = col;
    r = row;
    c = col;
    for (;;) {
        int w = cfr_row_width(vt, r);
        if (c < w - 1) {
            c++;
        } else if (r < le) {
            r++;
            c = 0;
        } else {
            break;
        }
        const CfrCell *x = cfr_get_cell(vt, r, c);
        if (!x || char_class(x->cp, wchars) != cls)
            break;
        er = r;
        ec = c;
    }

    *out_start = (CfrSelectionPoint){ sr, sc };
    *out_end = (CfrSelectionPoint){ er, ec };
}

void cfr_selection_start(CfrTerm *vt, int row, int col, CfrSelectionMode mode)
{
    if (!vt)
        return;

    CfrSelection *sel = &vt->selection;
    bool was_active = sel->active;
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
        sel->end = (CfrSelectionPoint){ row, last_col(vt, row) };
        break;
    default:
        sel->active = false;
        break;
    }

    /* selection_changed is an edge, not a level: starting a new selection on
     * top of an active one (double-click while a word is selected, then a
     * triple-click) replaces it without leaving the active state, so the
     * callback must not fire again — a host that pauses work on `true` and
     * resumes on `false` would otherwise hold one pause per restart and never
     * get a matching `false` when the selection is finally cleared. */
    if (sel->active != was_active)
        cfr_selection_fire_callback(vt, sel->active);
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
            sel->end = (CfrSelectionPoint){ sel->anchor.row,
                                            last_col(vt, sel->anchor.row) };
        } else {
            sel->start = (CfrSelectionPoint){ sel->anchor.row, 0 };
            sel->end = (CfrSelectionPoint){ row, last_col(vt, row) };
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
        /* 64-bit: a row gap beyond ~2000 overflows an int at this scale. */
        long long d_start = (long long)(click.row - sel->start.row) * 1000000 +
                            (click.col - sel->start.col);
        long long d_end = (long long)(sel->end.row - click.row) * 1000000 +
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

/* Growable output buffer for cfr_selection_get_text. */
typedef struct
{
    char *p;
    size_t len, cap;
} TextBuf;

static bool tb_put(TextBuf *b, const char *src, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 256;
        while (b->len + n + 1 > nc)
            nc *= 2;
        char *np = realloc(b->p, nc);
        if (!np)
            return false;
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->len, src, n);
    b->len += n;
    return true;
}

char *cfr_selection_get_text(const CfrTerm *vt)
{
    if (!vt || !vt->selection.active)
        return NULL;

    const CfrSelection *sel = &vt->selection;
    TextBuf out = { 0 };

    for (int row = sel->start.row; row <= sel->end.row; row++) {
        /* Each row is read at its own width. */
        int width = 0;
        const CfrCell *cells = cfr_get_row(vt, row, &width);
        int col_start = (row == sel->start.row && sel->start.col > 0)
                            ? sel->start.col
                            : 0;
        int col_end = width - 1;
        if (row == sel->end.row && sel->end.col < col_end)
            col_end = sel->end.col;

        size_t last_nonblank_len = out.len;

        for (int col = col_start; cells && col <= col_end;) {
            const CfrCell *cell = &cells[col];
            if (cell->cp == 0) {
                if (!tb_put(&out, " ", 1))
                    goto oom;
                col++;
                continue;
            }
            uint32_t cps[16];
            size_t n_cps = 1;
            cps[0] = cell->cp;
            if (cell->grapheme_id != 0) {
                n_cps = cfr_cell_get_grapheme(vt, row, col, cps,
                                              sizeof(cps) / sizeof(cps[0]));
                if (n_cps == 0) {
                    cps[0] = cell->cp;
                    n_cps = 1;
                }
            }
            for (size_t i = 0; i < n_cps; i++) {
                char utf8[4];
                int n = codepoint_to_utf8(cps[i], utf8);
                if (n > 0 && !tb_put(&out, utf8, (size_t)n))
                    goto oom;
            }
            if (cell->cp != 0x20)
                last_nonblank_len = out.len;
            col += (cell->width == 2) ? 2 : 1;
        }

        /* Strip trailing whitespace at hard line breaks; a soft wrap
         * joins the next row with no newline. */
        bool next_is_continuation =
            row < sel->end.row && cfr_row_is_continuation(vt, row + 1);
        if (!next_is_continuation)
            out.len = last_nonblank_len;
        if (row < sel->end.row && !next_is_continuation) {
            if (!tb_put(&out, "\n", 1))
                goto oom;
        }
    }

    if (!tb_put(&out, "", 0))
        goto oom;
    out.p[out.len] = '\0';
    return out.p;

oom:
    free(out.p);
    return NULL;
}
