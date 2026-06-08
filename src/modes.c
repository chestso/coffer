/*
 * bloom-vt — DEC private mode handlers that need real state changes
 * beyond the boolean flag flip in csi.c (altscreen save/restore,
 * cursor key application mode, etc.).
 */

#include "bloom_vt_internal.h"

#include <stdlib.h>
#include <string.h>

/* Altscreen toggle. With save_restore_cursor, mimics DECSET 1049 (the
 * "smcup/rmcup" variant): save cursor on entry, restore on exit. With
 * save_restore_cursor=false this matches DECSET 47 / 1047 (raw alt). */
void bvt_set_altscreen(BvtTerm *vt, bool on, bool save_restore_cursor)
{
    if (!vt)
        return;
    if (on == vt->in_altscreen)
        return;

    /* Flush any pending cluster before swapping grids. */
    bvt_flush_cluster(vt);

    /* Sixel images belong to the primary grid. We don't keep a separate
     * per-screen image set yet, so clear on any altscreen switch. */
    if (vt->sixel)
        bvt_sixel_clear_all(vt);

    if (on) {
        if (save_restore_cursor)
            vt->saved_cursor[0] = vt->cursor; /* normal-screen register */

        /* Lazily allocate or resize the alt grid to current geometry. */
        if (!vt->altgrid ||
            vt->altgrid->cols != vt->cols ||
            vt->altgrid->row_capacity != vt->rows) {
            if (vt->altgrid) {
                bvt_page_free(vt, vt->altgrid);
                vt->altgrid = NULL;
            }
            vt->altgrid = bvt_page_new(vt, vt->rows, vt->cols);
            if (!vt->altgrid)
                return; /* OOM — leave altscreen flag off */
        }

        /* Swap. The current grid is preserved in vt->altgrid; the
         * alt grid (which is zeroed by bvt_page_new) becomes active. */
        BvtPage *tmp = vt->grid;
        vt->grid = vt->altgrid;
        vt->altgrid = tmp;

        vt->in_altscreen = true;
        vt->modes[BVT_MODE_ALTSCREEN] = true;
        /* DECSET 1049 also moves cursor to home. */
        if (save_restore_cursor) {
            vt->cursor.row = 0;
            vt->cursor.col = 0;
            vt->cursor.pending_wrap = false;
        }
    } else {
        BvtPage *tmp = vt->grid;
        vt->grid = vt->altgrid;
        vt->altgrid = tmp;

        vt->in_altscreen = false;
        vt->modes[BVT_MODE_ALTSCREEN] = false;
        if (save_restore_cursor)
            bvt_cursor_restore(vt, &vt->saved_cursor[0]); /* normal-screen register */
    }

    bvt_damage_all(vt);
    if (vt->callbacks.set_mode)
        vt->callbacks.set_mode(BVT_MODE_ALTSCREEN, on, vt->callback_user);
}

void bvt_full_reset(BvtTerm *vt)
{
    if (!vt)
        return;

    bvt_flush_cluster(vt);

    /* Drop altscreen first so the visible grid is the primary one when
     * we clear it below. RIS doesn't preserve the saved cursor across
     * the swap. */
    if (vt->in_altscreen) {
        BvtPage *tmp = vt->grid;
        vt->grid = vt->altgrid;
        vt->altgrid = tmp;
        vt->in_altscreen = false;
    }

    /* Snapshot which modes were on so we can fire set_mode callbacks
     * for anything that flips off. The host typically uses these to
     * tear down mouse capture, bracketed-paste handling, etc. */
    bool prev_modes[32];
    memcpy(prev_modes, vt->modes, sizeof(prev_modes));

    memset(vt->modes, 0, sizeof(vt->modes));
    vt->modes[BVT_MODE_CURSOR_VISIBLE] = true;
    vt->modes[BVT_MODE_CURSOR_BLINK] = true;

    if (vt->callbacks.set_mode) {
        for (size_t i = 0; i < sizeof(prev_modes) / sizeof(prev_modes[0]); ++i) {
            if (prev_modes[i] != vt->modes[i])
                vt->callbacks.set_mode((BvtMode)i, vt->modes[i],
                                       vt->callback_user);
        }
    }

    /* Cursor + saved cursor with default pen. */
    memset(&vt->cursor, 0, sizeof(vt->cursor));
    vt->cursor.visible = true;
    vt->cursor.blink = true;
    vt->cursor.pen.color_flags =
        BVT_COLOR_DEFAULT_FG | BVT_COLOR_DEFAULT_BG | BVT_COLOR_DEFAULT_UL;
    vt->saved_cursor[0] = vt->saved_cursor[1] = vt->cursor;

    /* Scroll region back to full screen. */
    vt->scroll_top = 0;
    vt->scroll_bottom = vt->rows - 1;

    /* Keyboard / cursor key state. */
    vt->decckm = false;
    vt->deckpam = false;
    vt->decom = false;

    /* The bug this exists to fix: pop every kitty keyboard flag.
     * Without this, a TUI that pushed `CSI > 1 u` and crashed leaves
     * Ctrl+letter routed through CSI-u, breaking the shell. */
    memset(vt->kitty_kb_stack, 0, sizeof(vt->kitty_kb_stack));
    vt->kitty_kb_depth = 0;

    /* Charsets back to ASCII. */
    vt->charset[0] = vt->charset[1] = vt->charset[2] = vt->charset[3] = 'B';
    vt->charset_active = 0;

    /* Tab stops every 8 columns. */
    if (vt->tabstops) {
        for (int i = 0; i < vt->cols; ++i)
            vt->tabstops[i] = (i % 8 == 0) ? 1u : 0u;
    }

    /* Clear the visible grid. */
    if (vt->grid) {
        memset(vt->grid->cells, 0,
               (size_t)vt->rows * vt->cols * sizeof(BvtCell));
        memset(vt->grid->row_flags, 0, (size_t)vt->rows);
    }

    if (vt->sixel)
        bvt_sixel_clear_all(vt);

    bvt_damage_all(vt);
}
