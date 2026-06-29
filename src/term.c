/*
 * coffer — lifecycle + public API entry points.
 *
 * This file is the seam between coffer.h and the internal subsystems.
 * Behavior for grid/parser/etc is implemented in their dedicated files;
 * here we own creation, destruction, allocator routing, and dispatch.
 */

#include "coffer_internal.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Allocator                                                           */
/* ------------------------------------------------------------------ */

static void *stdlib_alloc(size_t size, void *user)
{
    (void)user;
    return malloc(size);
}
static void *stdlib_realloc(void *ptr, size_t size, void *user)
{
    (void)user;
    return realloc(ptr, size);
}
static void stdlib_free(void *ptr, void *user)
{
    (void)user;
    free(ptr);
}
static const CfrAllocator CFR_STDLIB_ALLOCATOR = {
    .alloc = stdlib_alloc,
    .realloc = stdlib_realloc,
    .free = stdlib_free,
    .user = NULL,
};

void *cfr_alloc(CfrTerm *vt, size_t size)
{
    return vt->alloc.alloc(size, vt->alloc.user);
}
void *cfr_realloc(CfrTerm *vt, void *ptr, size_t size)
{
    return vt->alloc.realloc(ptr, size, vt->alloc.user);
}
void cfr_dealloc(CfrTerm *vt, void *ptr)
{
    if (ptr)
        vt->alloc.free(ptr, vt->alloc.user);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

CfrTerm *cfr_new(const CfrConfig *cfg)
{
    return cfr_new_with_allocator(cfg, NULL);
}

CfrTerm *cfr_new_with_allocator(const CfrConfig *cfg, const CfrAllocator *alloc)
{
    if (!cfg || cfg->rows <= 0 || cfg->cols <= 0 ||
        cfg->cell_w_px <= 0 || cfg->cell_h_px <= 0)
        return NULL;

    const CfrAllocator *a = alloc ? alloc : &CFR_STDLIB_ALLOCATOR;
    CfrTerm *vt = a->alloc(sizeof(*vt), a->user);
    if (!vt)
        return NULL;

    memset(vt, 0, sizeof(*vt));
    vt->alloc = *a;
    vt->rows = cfg->rows;
    vt->cols = cfg->cols;
    vt->cell_w_px = cfg->cell_w_px;
    vt->cell_h_px = cfg->cell_h_px;
    vt->scroll_top = 0;
    vt->scroll_bottom = cfg->rows - 1;
    vt->sb_capacity = cfg->scrollback >= 0 ? cfg->scrollback : CFR_DEFAULT_SCROLLBACK;
    vt->reflow_enabled = cfg->reflow;
    vt->ambiguous_wide = cfg->ambiguous_wide;
    vt->cursor.visible = true;
    vt->cursor.blink = true;
    /* Mirror the initial cursor so the first flush doesn't spuriously damage. */
    vt->dmg_cursor_visible = true;
    vt->cursor.pen.color_flags =
        CFR_COLOR_DEFAULT_FG | CFR_COLOR_DEFAULT_BG | CFR_COLOR_DEFAULT_UL;
    vt->modes[CFR_MODE_CURSOR_VISIBLE] = true;
    vt->modes[CFR_MODE_CURSOR_BLINK] = true;
    vt->modes[CFR_MODE_DECAWM] = true;
    vt->charset[0] = vt->charset[1] = vt->charset[2] = vt->charset[3] = 'B';
    vt->charset_active = 0;

    cfr_parser_init(&vt->parser);

    /* Tab stops every 8 columns by default. */
    vt->tabstops = vt->alloc.alloc((size_t)cfg->cols, vt->alloc.user);
    if (!vt->tabstops) {
        vt->alloc.free(vt, vt->alloc.user);
        return NULL;
    }
    for (int i = 0; i < cfg->cols; ++i)
        vt->tabstops[i] = (i % 8 == 0) ? 1u : 0u;

    /* Grid + altgrid pages allocated lazily once grid.c is implemented. */
    vt->grid = NULL;
    vt->altgrid = NULL;

    return vt;
}

void cfr_free(CfrTerm *vt)
{
    if (!vt)
        return;

    /* Drop scrollback pages. */
    CfrPage *p = vt->sb_head;
    while (p) {
        CfrPage *next = p->next;
        cfr_page_free(vt, p);
        p = next;
    }

    if (vt->grid)
        cfr_page_free(vt, vt->grid);
    if (vt->altgrid)
        cfr_page_free(vt, vt->altgrid);

    cfr_sixel_state_free(vt);
    cfr_lottie_state_free(vt);

    cfr_dealloc(vt, vt->tabstops);
    cfr_dealloc(vt, vt->title);

    /* Use saved allocator (vt->alloc.free) — vt itself is freed last. */
    CfrAllocator a = vt->alloc;
    a.free(vt, a.user);
}

void cfr_set_callbacks(CfrTerm *vt, const CfrCallbacks *cb, void *user)
{
    if (!vt)
        return;
    if (cb)
        vt->callbacks = *cb;
    else
        memset(&vt->callbacks, 0, sizeof(vt->callbacks));
    vt->callback_user = user;
}

void cfr_resize(CfrTerm *vt, int rows, int cols)
{
    if (!vt || rows <= 0 || cols <= 0)
        return;
    if (rows == vt->rows && cols == vt->cols)
        return;
    /* Reflow rebuilds the grid with no line identity, so anchored sixel
     * images can't be followed across a rewrap — drop them on resize. */
    if (vt->sixel)
        cfr_sixel_clear_all(vt);
    if (vt->lottie)
        cfr_lottie_clear_all(vt);
    cfr_reflow(vt, rows, cols);
}

void cfr_set_reflow(CfrTerm *vt, bool enabled)
{
    if (vt)
        vt->reflow_enabled = enabled;
}
void cfr_set_ambiguous_wide(CfrTerm *vt, bool wide)
{
    if (vt)
        vt->ambiguous_wide = wide;
}
void cfr_set_scrollback_size(CfrTerm *vt, int lines)
{
    if (vt && lines >= 0)
        vt->sb_capacity = lines;
}

/* ------------------------------------------------------------------ */
/* I/O                                                                 */
/* ------------------------------------------------------------------ */

size_t cfr_input_write(CfrTerm *vt, const uint8_t *bytes, size_t len)
{
    if (!vt || !bytes || len == 0)
        return 0;
    cfr_parser_feed(vt, bytes, len);
    /* Commit any in-flight grapheme cluster. Real PTY frames almost
     * never split clusters across writes; for the rare exception
     * callers can chain writes without intervening reads. */
    cfr_flush_cluster(vt);
    return len;
}

/* cfr_send_key, cfr_send_text, cfr_send_mouse, cfr_paste_begin,
 * cfr_paste_end are implemented in keys.c. */

/* ------------------------------------------------------------------ */
/* Queries                                                             */
/* ------------------------------------------------------------------ */

const CfrCell *cfr_get_cell(const CfrTerm *vt, int row, int col)
{
    if (!vt || !vt->grid)
        return NULL;
    if (row < 0 || row >= vt->rows)
        return NULL;
    if (col < 0 || col >= vt->cols)
        return NULL;
    return &vt->grid->cells[(size_t)row * vt->cols + col];
}

void cfr_get_dimensions(const CfrTerm *vt, int *out_rows, int *out_cols)
{
    if (!vt)
        return;
    if (out_rows)
        *out_rows = vt->rows;
    if (out_cols)
        *out_cols = vt->cols;
}

int cfr_get_scrollback_lines(const CfrTerm *vt)
{
    return vt ? vt->sb_lines : 0;
}

int cfr_get_scrollback_capacity(const CfrTerm *vt)
{
    return vt ? vt->sb_capacity : 0;
}

/* cfr_get_scrollback_cell, cfr_get_scrollback_wrapline are implemented
 * in scrollback.c. */

CfrCursor cfr_get_cursor(const CfrTerm *vt)
{
    CfrCursor out = { 0 };
    if (!vt)
        return out;
    out.row = vt->cursor.row;
    out.col = vt->cursor.col;
    out.visible = vt->cursor.visible;
    out.blink = vt->cursor.blink;
    return out;
}

const char *cfr_get_title(const CfrTerm *vt)
{
    return vt ? vt->title : NULL;
}

bool cfr_is_altscreen(const CfrTerm *vt)
{
    return vt ? vt->in_altscreen : false;
}

bool cfr_get_mode(const CfrTerm *vt, CfrMode mode)
{
    if (!vt)
        return false;
    if ((unsigned)mode >= sizeof(vt->modes) / sizeof(vt->modes[0]))
        return false;
    return vt->modes[mode];
}

bool cfr_get_line_continuation(const CfrTerm *vt, int row)
{
    if (!vt || !vt->grid)
        return false;
    if (row < 0 || row >= vt->rows)
        return false;
    return (vt->grid->row_flags[row] & CFR_CELL_WRAPLINE) != 0u;
}

const CfrStyle *cfr_cell_style(const CfrTerm *vt, const CfrCell *cell)
{
    if (!vt || !cell)
        return NULL;
    const CfrPage *page = cfr_find_owner_page(vt, cell);
    if (!page)
        return NULL;
    return cfr_style_lookup(page, cell->style_id);
}

size_t cfr_cell_get_grapheme(const CfrTerm *vt, const CfrCell *cell,
                             uint32_t *out, size_t out_cap)
{
    if (!vt || !cell || !out || out_cap == 0)
        return 0;
    if (cell->grapheme_id == 0) {
        out[0] = cell->cp;
        return 1;
    }
    const CfrPage *page = cfr_find_owner_page(vt, cell);
    if (!page)
        return 0;
    return cfr_grapheme_read(page, cell->grapheme_id, out, out_cap);
}

size_t cfr_cell_get_hyperlink(const CfrTerm *vt, const CfrCell *cell,
                              uint8_t *out_uri, size_t out_cap)
{
    if (!vt || !cell || cell->hyperlink_id == 0 || !out_uri || out_cap == 0)
        return 0;
    const CfrPage *page = cfr_find_owner_page(vt, cell);
    if (!page)
        return 0;
    return cfr_hyperlink_read(page, cell->hyperlink_id, out_uri, out_cap);
}
