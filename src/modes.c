/*
 * coffer — DEC private mode handlers that need real state changes
 * beyond the boolean flag flip in csi.c (altscreen save/restore,
 * cursor key application mode, etc.).
 */

#include "coffer_internal.h"

#include <stdlib.h>
#include <string.h>

/* Altscreen toggle. With save_restore_cursor, mimics DECSET 1049 (the
 * "smcup/rmcup" variant): save cursor on entry, restore on exit. With
 * save_restore_cursor=false this matches DECSET 47 / 1047 (raw alt). */
void cfr_set_altscreen(CfrTerm *vt, bool on, bool save_restore_cursor)
{
    if (!vt)
        return;
    if (on == vt->in_altscreen)
        return;

    /* Flush any pending cluster before swapping grids. */
    cfr_flush_cluster(vt);

    /* Sixel images belong to the primary grid. We don't keep a separate
     * per-screen image set yet, so clear on any altscreen switch. */
    if (vt->sixel)
        cfr_sixel_clear_all(vt);

    if (on) {
        if (save_restore_cursor)
            vt->saved_cursor[0] = vt->cursor; /* normal-screen register */

        /* Lazily allocate or resize the alt grid to current geometry. */
        if (!vt->altgrid ||
            vt->altgrid->cols != vt->cols ||
            vt->altgrid->row_capacity != vt->rows) {
            if (vt->altgrid) {
                cfr_page_free(vt, vt->altgrid);
                vt->altgrid = NULL;
            }
            vt->altgrid = cfr_page_new(vt, vt->rows, vt->cols);
            if (!vt->altgrid)
                return; /* OOM — leave altscreen flag off */
        }

        /* Swap. The current grid is preserved in vt->altgrid; the
         * alt grid becomes active. */
        CfrPage *tmp = vt->grid;
        vt->grid = vt->altgrid;
        vt->altgrid = tmp;

        /* Clear the newly-active alt grid so each altscreen entry
         * starts with a blank slate.  When the altgrid was just
         * allocated by cfr_page_new it is already zeroed; this handles
         * the reuse case where the altgrid still holds data from a
         * previous altscreen session. */
        if (vt->grid) {
            memset(vt->grid->cells, 0,
                   (size_t)vt->grid->row_capacity * vt->grid->cols * sizeof(CfrCell));
            memset(vt->grid->row_flags, 0, (size_t)vt->grid->row_capacity);
        }

        vt->in_altscreen = true;
        vt->modes[CFR_MODE_ALTSCREEN] = true;
        /* DECSET 1049 also moves cursor to home. */
        if (save_restore_cursor) {
            vt->cursor.row = 0;
            vt->cursor.col = 0;
            vt->cursor.pending_wrap = false;
        }
    } else {
        CfrPage *tmp = vt->grid;
        vt->grid = vt->altgrid;
        vt->altgrid = tmp;

        vt->in_altscreen = false;
        vt->modes[CFR_MODE_ALTSCREEN] = false;

        /* The main grid was saved when entering the alt screen. If the
         * terminal was resized while on the alt screen (e.g. a
         * fullscreen toggle), the restored grid is stale — its cells
         * array was allocated at the old dimensions, but vt->rows and
         * vt->cols now reflect the current (possibly larger) size.
         * cfr_get_cell uses vt->cols for the stride, so a stale grid
         * with fewer columns causes a heap buffer overflow when the
         * renderer reads cells beyond the old allocation.
         *
         * Resize the grid to match the current geometry.  Temporarily
         * roll vt->rows/vt->cols back to the grid's actual allocation
         * size so cfr_reflow sees a genuine dimension change (it
         * returns early when new == current).  in_altscreen is already
         * false so cfr_reflow will reflow (not clamp) the main grid,
         * re-wrapping long lines instead of truncating them. */
        if (vt->grid &&
            (vt->grid->cols != vt->cols ||
             vt->grid->row_capacity != vt->rows)) {
            int saved_rows = vt->rows;
            int saved_cols = vt->cols;
            vt->rows = vt->grid->row_count;
            vt->cols = vt->grid->cols;
            cfr_resize(vt, saved_rows, saved_cols);
        }

        if (save_restore_cursor) {
            /* cfr_cursor_restore preserves the live cursor.blink/visible
             * (see its comment in coffer_internal.h). For DECSC/DECRC
             * that's correct — ?25 and ?12 are independent modes. But
             * 1049 is a full screen save/restore: the pre-altscreen
             * cursor state should be fully restored, including blink and
             * visible. Apps like ncdu turn off blink (?12l) in the alt
             * screen and never restore it; without this, the host is
             * left with blink permanently off after the app exits. */
            bool saved_blink = vt->saved_cursor[0].blink;
            bool saved_visible = vt->saved_cursor[0].visible;
            cfr_cursor_restore(vt, &vt->saved_cursor[0]);
            if (vt->cursor.blink != saved_blink) {
                vt->cursor.blink = saved_blink;
                vt->modes[CFR_MODE_CURSOR_BLINK] = saved_blink;
                if (vt->callbacks.set_mode)
                    vt->callbacks.set_mode(CFR_MODE_CURSOR_BLINK, saved_blink,
                                           vt->callback_user);
            }
            if (vt->cursor.visible != saved_visible) {
                vt->cursor.visible = saved_visible;
                vt->modes[CFR_MODE_CURSOR_VISIBLE] = saved_visible;
                if (vt->callbacks.set_mode)
                    vt->callbacks.set_mode(CFR_MODE_CURSOR_VISIBLE, saved_visible,
                                           vt->callback_user);
            }
        }
    }

    cfr_damage_all(vt);
    if (vt->callbacks.set_mode)
        vt->callbacks.set_mode(CFR_MODE_ALTSCREEN, on, vt->callback_user);
}

void cfr_full_reset(CfrTerm *vt)
{
    if (!vt)
        return;

    cfr_flush_cluster(vt);

    /* Drop altscreen first so the visible grid is the primary one when
     * we clear it below. RIS doesn't preserve the saved cursor across
     * the swap. */
    if (vt->in_altscreen) {
        CfrPage *tmp = vt->grid;
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
    vt->modes[CFR_MODE_CURSOR_VISIBLE] = true;
    vt->modes[CFR_MODE_CURSOR_BLINK] = true;
    vt->modes[CFR_MODE_DECAWM] = true;

    if (vt->callbacks.set_mode) {
        for (size_t i = 0; i < sizeof(prev_modes) / sizeof(prev_modes[0]); ++i) {
            if (prev_modes[i] != vt->modes[i])
                vt->callbacks.set_mode((CfrMode)i, vt->modes[i],
                                       vt->callback_user);
        }
    }

    /* Cursor + saved cursor with default pen. */
    memset(&vt->cursor, 0, sizeof(vt->cursor));
    vt->cursor.visible = true;
    vt->cursor.blink = true;
    vt->cursor.pen.color_flags =
        CFR_COLOR_DEFAULT_FG | CFR_COLOR_DEFAULT_BG | CFR_COLOR_DEFAULT_UL;
    vt->saved_cursor[0] = vt->saved_cursor[1] = vt->cursor;

    /* Scroll region back to full screen. */
    vt->scroll_top = 0;
    vt->scroll_bottom = vt->rows - 1;

    /* Keyboard / cursor key state. */
    vt->decckm = false;
    vt->deckpam = false;
    vt->decom = false;
    vt->insert_mode = false;

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
               (size_t)vt->rows * vt->cols * sizeof(CfrCell));
        memset(vt->grid->row_flags, 0, (size_t)vt->rows);
    }

    if (vt->sixel)
        cfr_sixel_clear_all(vt);

    cfr_damage_all(vt);
}
