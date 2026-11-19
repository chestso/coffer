/*
 * coffer — DEC private mode handlers and cursor state changes.
 *
 * Real state changes beyond the boolean flag flip in csi.c (altscreen
 * save/restore, cursor key application mode, RIS), plus the single
 * points where mode flags and the mirrored cursor attributes
 * (visible / blink / shape) change — so the flags and the cursor can
 * never disagree and the host is told exactly once per change.
 */

#include "coffer_internal.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Mode / cursor state changes                                         */
/* ------------------------------------------------------------------ */

/* Set a mode flag and tell the host — only when the value actually
 * changes, so redundant DECSET/DECRST repeats don't nag the host (the
 * reset paths diff the same way). Keeps mirrored state (the cursor's
 * visible/blink fields) and the flags from drifting apart. */
void cfr_mode_flag_set(CfrTerm *vt, CfrMode mode, bool on)
{
    if (vt->modes[mode] == on)
        return;
    vt->modes[mode] = on;
    if (vt->callbacks.set_mode)
        vt->callbacks.set_mode(mode, on, vt->callback_user);
}

/* DECTCEM (?25) — cursor visibility mirrors its mode. */
void cfr_cursor_set_visible(CfrTerm *vt, bool on)
{
    vt->cursor.visible = on;
    cfr_mode_flag_set(vt, CFR_MODE_CURSOR_VISIBLE, on);
}

/* DECSCUSR and the AT&T blink mode (?12) — one style state for both
 * halves of the cursor style. A change repaints the cursor cell (the
 * damage flush watches only row/col/visibility) and re-reports the
 * cursor; a plain move is covered by the damage flush alone. */
void cfr_cursor_set_style(CfrTerm *vt, CfrCursorShape shape, bool blink)
{
    bool changed = (vt->cursor.shape != shape) || (vt->cursor.blink != blink);
    vt->cursor.shape = shape;
    vt->cursor.blink = blink;
    cfr_mode_flag_set(vt, CFR_MODE_CURSOR_BLINK, blink);
    if (!changed)
        return;
    cfr_damage_cursor(vt);
    if (vt->callbacks.movecursor)
        vt->callbacks.movecursor(cfr_get_cursor(vt), vt->callback_user);
}

/* Altscreen 1049 exit — a full pre-altscreen cursor restore. Unlike
 * DECSC/DECRC, the mode-ish attributes come back too: apps like ncdu
 * turn off blink (?12l) and switch cursor shape in the alt screen and
 * never restore them, and 1049 is a screen-level save/restore. */
void cfr_cursor_restore_full(CfrTerm *vt, const CfrCursorState *src)
{
    cfr_cursor_restore(vt, src);
    cfr_cursor_set_visible(vt, src->visible);
    cfr_cursor_set_style(vt, src->shape, src->blink);
}

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

    /* Selection is screen-specific — clear on any altscreen switch. */
    cfr_selection_clear(vt);

    /* The anchored (1:1) images belong to the grid that is being left:
     * their anchors are that grid's rows, and the grid is swapped here.
     * The client-owned 1:N images (kitty) carry their own anchors and
     * live until the client deletes them — kitty's rule is that only
     * the images of the screen being left are cleared, and coffer has
     * one image set, not one per screen. */
    if (vt->images)
        cfr_img_clear_anchored(vt, vt->images);

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
            memset(vt->grid->lineage, 0,
                   (size_t)vt->grid->row_capacity * sizeof(uint32_t));
        }

        vt->in_altscreen = true;
        vt->modes[CFR_MODE_ALTSCREEN] = true;
        /* DECSET 1049 also moves cursor to home. */
        if (save_restore_cursor)
            cfr_cursor_set(vt, 0, 0);
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

        if (save_restore_cursor)
            cfr_cursor_restore_full(vt, &vt->saved_cursor[0]);
    }

    cfr_damage_all(vt);
    if (vt->callbacks.set_mode)
        vt->callbacks.set_mode(CFR_MODE_ALTSCREEN, on, vt->callback_user);
}

void cfr_reset_input_state(CfrTerm *vt)
{
    if (!vt)
        return;

    /* Snapshot the modes so the host is told about every one that
     * flips — the host uses these to tear down mouse capture and the
     * like. */
    bool prev_modes[32];
    memcpy(prev_modes, vt->modes, sizeof(prev_modes));

    /* Kitty keyboard protocol: pop the whole stack. A program that
     * pushed the flags and died leaves them set for the rest of the
     * session, which routes every Ctrl+key through CSI-u and leaves
     * the shell unable to read a line. */
    cfr_kitty_kb_pop(vt, 0);

    /* xterm modifyOtherKeys (DECSET ?2): modified keys are reported
     * as CSI 27;<mod>;<code>~, which readline does not understand. */
    vt->modify_other_keys = 0;

    /* Application cursor keys (DECCKM) and application keypad
     * (DECKPAM). */
    vt->decckm = false;
    vt->deckpam = false;

    /* Mouse reporting and focus reporting. Bracketed paste stays: a
     * shell enables it for its own lifetime. */
    vt->modes[CFR_MODE_MOUSE_X10] = false;
    vt->modes[CFR_MODE_MOUSE_BTN_EVENT] = false;
    vt->modes[CFR_MODE_MOUSE_DRAG] = false;
    vt->modes[CFR_MODE_MOUSE_ANY_EVENT] = false;
    vt->modes[CFR_MODE_FOCUS_REPORTING] = false;
    /* A program that died mid-frame leaves this set; the screen would
     * never repaint (cfr_damage_flush holds everything while it is on). */
    vt->modes[CFR_MODE_SYNC_OUTPUT] = false;

    if (vt->callbacks.set_mode) {
        for (size_t i = 0; i < sizeof(prev_modes) / sizeof(prev_modes[0]); ++i) {
            if (prev_modes[i] != vt->modes[i])
                vt->callbacks.set_mode((CfrMode)i, vt->modes[i],
                                       vt->callback_user);
        }
    }
}

/* Host-side frame timeout for synchronized output. Ending the hold is
 * all it does: the pending damage is left alone, so the next flush
 * paints the partial frame, and the application's own end marker later
 * is a no-op because the flag is already clear. */
void cfr_sync_output_cancel(CfrTerm *vt)
{
    if (!vt)
        return;
    cfr_mode_flag_set(vt, CFR_MODE_SYNC_OUTPUT, false);
}

void cfr_reset(CfrTerm *vt)
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
    vt->modes[CFR_MODE_CURSOR_BLINK] = vt->default_blink;
    vt->modes[CFR_MODE_DECAWM] = true;

    if (vt->callbacks.set_mode) {
        for (size_t i = 0; i < sizeof(prev_modes) / sizeof(prev_modes[0]); ++i) {
            if (prev_modes[i] != vt->modes[i])
                vt->callbacks.set_mode((CfrMode)i, vt->modes[i],
                                       vt->callback_user);
        }
    }

    /* Cursor + saved cursor: default pen, configured style. */
    memset(&vt->cursor, 0, sizeof(vt->cursor));
    vt->cursor.visible = true;
    vt->cursor.blink = vt->default_blink;
    vt->cursor.shape = vt->default_shape;
    vt->cursor.pen.color_flags =
        CFR_COLOR_DEFAULT_FG | CFR_COLOR_DEFAULT_BG | CFR_COLOR_DEFAULT_UL;
    vt->saved_cursor[0] = vt->saved_cursor[1] = vt->cursor;

    /* Dynamic colors (OSC 10/11/12) back to their configured values. */
    cfr_dyn_color_reset_all(vt);

    /* Scroll region back to full screen. */
    vt->scroll_top = 0;
    vt->scroll_bottom = vt->rows - 1;

    /* Keyboard / cursor key state. */
    vt->decckm = false;
    vt->deckpam = false;
    vt->decom = false;
    vt->insert_mode = false;
    vt->modify_other_keys = 0;

    /* Pop every kitty keyboard flag. Without this, a TUI that pushed
     * `CSI > 1 u` and crashed leaves Ctrl+letter routed through
     * CSI-u, breaking the shell. */
    cfr_kitty_kb_pop(vt, 0);

    /* RIS clears scrollback too — `reset` must behave like a fresh
     * terminal, not like `clear`. bash's `reset` runs `tput reset`,
     * which sends RIS (ESC c). */
    cfr_scrollback_clear(vt);

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
        memset(vt->grid->lineage, 0, (size_t)vt->rows * sizeof(uint32_t));
    }

    /* Reset log-once guards so a new session gets fresh warnings. */
    vt->logged_once = 0;
    /* Reset last-printed char for REP. */
    vt->last_char = 0;
    /* Fresh logical-line id sequence (not required for correctness —
     * the grid was just zeroed — but keeps ids tidy across sessions). */
    vt->next_lineage = 0;

    if (vt->images)
        cfr_img_clear_all(vt, vt->images);

    cfr_damage_all(vt);
}
