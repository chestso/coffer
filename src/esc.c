/*
 * coffer — ESC dispatcher (non-CSI/non-OSC/non-DCS escapes).
 *
 * Handles ESC X commands like keypad mode, save/restore cursor,
 * charset designations (G0/G1/G2/G3), reverse index, and full reset.
 */

#include "coffer_internal.h"

void cfr_esc_dispatch(CfrTerm *vt, uint8_t final)
{
    cfr_flush_cluster(vt);
    CfrParser *p = &vt->parser;

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
        *cfr_active_saved_cursor(vt) = vt->cursor;
        break; /* DECSC */
    case '8':  /* DECRC */
        cfr_cursor_restore(vt, cfr_active_saved_cursor(vt));
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
            cfr_scroll_up(vt, 1);
        else if (vt->cursor.row < vt->rows - 1)
            vt->cursor.row++;
        break;
    case 'E': /* NEL */
        if (vt->cursor.row == vt->scroll_bottom)
            cfr_scroll_up(vt, 1);
        else if (vt->cursor.row < vt->rows - 1)
            vt->cursor.row++;
        vt->cursor.col = 0;
        break;
    case 'M': /* RI */
        if (vt->cursor.row == vt->scroll_top)
            cfr_scroll_down(vt, 1);
        else if (vt->cursor.row > 0)
            vt->cursor.row--;
        break;
    case 'c': /* RIS — full reset */
        cfr_full_reset(vt);
        break;
    case 'l': /* meml — locking shift G0 to G1 */
        if (should_log_once(vt, CFR_LOGGED_MEML))
            cfr_log(vt, CFR_LOG_WARN, "locking shift meml (ESC l) not implemented yet");
        break;
    case 'm': /* memu — locking shift G1 to G0 */
        if (should_log_once(vt, CFR_LOGGED_MEMU))
            cfr_log(vt, CFR_LOG_WARN, "locking shift memu (ESC m) not implemented yet");
        break;
    default:
        /* Many ESC dispatches are silently ignored. */
        break;
    }
}
