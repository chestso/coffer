/*
 * bloom-vt — ESC dispatcher (non-CSI/non-OSC/non-DCS escapes).
 *
 * Handles ESC X commands like keypad mode, save/restore cursor,
 * charset designations (G0/G1/G2/G3), reverse index, and full reset.
 */

#include "bloom_vt_internal.h"

void bvt_esc_dispatch(BvtTerm *vt, uint8_t final)
{
    bvt_flush_cluster(vt);
    BvtParser *p = &vt->parser;

    /* Charset designation: ESC ( c | ) c | * c | + c — single-char
     * intermediate captured during ESCAPE_INTERMEDIATE. We track only
     * the designation here; the GL translation runs at print time. */
    if (p->intermediate_count == 1) {
        uint8_t inter = p->intermediates[0];
        if (inter == '(' || inter == ')' || inter == '*' || inter == '+') {
            int slot = (inter == '(') ? 0 : (inter == ')') ? 1
                                        : (inter == '*')   ? 2
                                                           : 3;
            vt->charset[slot] = final;
            return;
        }
    }

    switch (final) {
    case '7':
        *bvt_active_saved_cursor(vt) = vt->cursor;
        break; /* DECSC */
    case '8':  /* DECRC */
        bvt_cursor_restore(vt, bvt_active_saved_cursor(vt));
        break;
    case 'n':
        vt->charset_active = 2;
        break; /* LS2 */
    case 'o':
        vt->charset_active = 3;
        break; /* LS3 */
    case '=':
        vt->deckpam = true;
        break; /* DECKPAM */
    case '>':
        vt->deckpam = false;
        break; /* DECKPNM */
    case 'D':  /* IND */
        if (vt->cursor.row == vt->scroll_bottom)
            bvt_scroll_up(vt, 1);
        else if (vt->cursor.row < vt->rows - 1)
            vt->cursor.row++;
        break;
    case 'E': /* NEL */
        if (vt->cursor.row == vt->scroll_bottom)
            bvt_scroll_up(vt, 1);
        else if (vt->cursor.row < vt->rows - 1)
            vt->cursor.row++;
        vt->cursor.col = 0;
        break;
    case 'M': /* RI */
        if (vt->cursor.row == vt->scroll_top)
            bvt_scroll_down(vt, 1);
        else if (vt->cursor.row > 0)
            vt->cursor.row--;
        break;
    case 'c': /* RIS — full reset */
        bvt_full_reset(vt);
        break;
    default:
        /* Many ESC dispatches are silently ignored. */
        break;
    }
}
