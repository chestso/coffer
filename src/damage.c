/*
 * bloom-vt — damage rectangle accumulator.
 *
 * The VT layer tracks a single rectangular union since the last flush.
 * Backends call bvt_damage_flush() (typically just before rendering)
 * to drain it via the callback.
 */

#include "bloom_vt_internal.h"

static void union_rect(BvtRect *acc, BvtRect r)
{
    if (r.start_row < acc->start_row)
        acc->start_row = r.start_row;
    if (r.start_col < acc->start_col)
        acc->start_col = r.start_col;
    if (r.end_row > acc->end_row)
        acc->end_row = r.end_row;
    if (r.end_col > acc->end_col)
        acc->end_col = r.end_col;
}

void bvt_damage_cell(BvtTerm *vt, int row, int col)
{
    BvtRect r = { row, col, row, col };
    if (!vt->damage_dirty) {
        vt->damage = r;
        vt->damage_dirty = true;
    } else {
        union_rect(&vt->damage, r);
    }
}

void bvt_damage_row(BvtTerm *vt, int row)
{
    BvtRect r = { row, 0, row, vt->cols - 1 };
    if (!vt->damage_dirty) {
        vt->damage = r;
        vt->damage_dirty = true;
    } else {
        union_rect(&vt->damage, r);
    }
}

void bvt_damage_all(BvtTerm *vt)
{
    BvtRect r = { 0, 0, vt->rows - 1, vt->cols - 1 };
    vt->damage = r;
    vt->damage_dirty = true;
}

void bvt_damage_flush(BvtTerm *vt)
{
    /* A cursor-only move (CUP, arrows) changes no grid cell and emits no
     * damage. Fold it in by dirtying the old and new cursor cells so the
     * consumer repaints both — matching how Alacritty damages the cursor at
     * flush time. */
    if (vt->cursor.row != vt->dmg_cursor_row ||
        vt->cursor.col != vt->dmg_cursor_col ||
        vt->cursor.visible != vt->dmg_cursor_visible) {
        bvt_damage_cell(vt, vt->dmg_cursor_row, vt->dmg_cursor_col);
        bvt_damage_cell(vt, vt->cursor.row, vt->cursor.col);
        vt->dmg_cursor_row = vt->cursor.row;
        vt->dmg_cursor_col = vt->cursor.col;
        vt->dmg_cursor_visible = vt->cursor.visible;
    }

    if (!vt->damage_dirty)
        return;
    if (vt->callbacks.damage)
        vt->callbacks.damage(vt->damage, vt->callback_user);
    vt->damage_dirty = false;
}
