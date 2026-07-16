/*
 * coffer — CSI dispatcher.
 *
 * Initial cut handles cursor moves, erase-in-line / erase-in-display,
 * and SGR reset. The full SGR repertoire (colors, 4:N underlines,
 * 38/48/58 8-bit and 24-bit colors) lands in a follow-up step.
 */

#include "coffer_internal.h"

#include <stdio.h>
#include <string.h>

static int param_or(CfrTerm *vt, int idx, int defval)
{
    CfrParser *p = &vt->parser;
    if (idx >= p->param_count)
        return defval;
    if (p->params[idx] == 0 && !p->param_present && idx == p->param_count - 1)
        return defval;
    return (p->params[idx] == 0) ? defval : (int)p->params[idx];
}

static bool param_is_subparam(const CfrParser *p, int idx)
{
    if (idx < 0 || idx >= p->param_count)
        return false;
    return (p->param_is_subparam & ((uint32_t)1u << (unsigned)idx)) != 0;
}

static bool has_intermediate(CfrTerm *vt, uint8_t b)
{
    CfrParser *p = &vt->parser;
    for (int i = 0; i < p->intermediate_count; ++i)
        if (p->intermediates[i] == b)
            return true;
    return false;
}

static void cursor_to(CfrTerm *vt, int row, int col)
{
    if (row < 0)
        row = 0;
    if (col < 0)
        col = 0;
    if (row >= vt->rows)
        row = vt->rows - 1;
    if (col >= vt->cols)
        col = vt->cols - 1;
    vt->cursor.row = row;
    vt->cursor.col = col;
    vt->cursor.pending_wrap = false;
}

/* CUP/HVP/VPA destination — applies DECOM (origin mode): when on, the
 * `row` argument is relative to the scroll region, and the cursor is
 * clamped to the scroll region. When off, behaves like cursor_to. */
static void cursor_to_origin(CfrTerm *vt, int row, int col)
{
    if (vt->decom) {
        int top = vt->scroll_top;
        int bot = vt->scroll_bottom;
        if (row < 0)
            row = 0;
        row += top;
        if (row > bot)
            row = bot;
        if (col < 0)
            col = 0;
        if (col >= vt->cols)
            col = vt->cols - 1;
        vt->cursor.row = row;
        vt->cursor.col = col;
        vt->cursor.pending_wrap = false;
        return;
    }
    cursor_to(vt, row, col);
}

static void reset_pen(CfrTerm *vt)
{
    memset(&vt->cursor.pen, 0, sizeof(vt->cursor.pen));
    vt->cursor.pen.color_flags =
        CFR_COLOR_DEFAULT_FG | CFR_COLOR_DEFAULT_BG | CFR_COLOR_DEFAULT_UL;
}

/* Consume an SGR color sub-sequence starting at index `i`. Sets
 * *out_rgb / *out_default per the parsed mode. Returns the new index
 * after the sub-sequence. Both 38;5;N and 38:5:N and 38;2;R;G;B and
 * 38:2:R:G:B forms are accepted (xterm and ITU/ISO variants). */
static int parse_color_arg(CfrTerm *vt, int i, uint32_t *out_rgb, bool *out_default)
{
    CfrParser *p = &vt->parser;
    if (i >= p->param_count) {
        *out_default = true;
        return i;
    }
    uint32_t mode = p->params[i++];
    if (mode == 5) {
        if (i < p->param_count) {
            *out_rgb = cfr_palette_lookup(vt, (uint8_t)p->params[i++]);
            *out_default = false;
        }
    } else if (mode == 2) {
        /* CSI 38;2;R;G;B  or  CSI 38:2:R:G:B  or  CSI 38:2::R:G:B —
         * the third form has an empty "colourspace ID" subparam slot.
         * Detect it: a subparam at i with 4+ remaining params means the
         * empty slot is present; skip it before reading R, G, B. */
        if (i < p->param_count && param_is_subparam(p, i) && (p->param_count - i) >= 4) {
            i++;
        }
        uint32_t r = (i < p->param_count) ? p->params[i++] : 0;
        uint32_t g = (i < p->param_count) ? p->params[i++] : 0;
        uint32_t b = (i < p->param_count) ? p->params[i++] : 0;
        *out_rgb = ((r & 0xFFu) << 16) | ((g & 0xFFu) << 8) | (b & 0xFFu);
        *out_default = false;
    }
    return i;
}

static void sgr_dispatch(CfrTerm *vt)
{
    CfrParser *p = &vt->parser;
    if (p->param_count == 0) {
        reset_pen(vt);
        return;
    }
    for (int i = 0; i < p->param_count; /* increment per-case */) {
        uint32_t s = p->params[i++];
        switch (s) {
        case 0:
            /* A trailing semicolon (e.g. \e[48;2;R;G;B;m) creates an
             * empty zero-valued parameter at the end.  Ignore it — it
             * does NOT mean "reset all attributes".  ConPTY on Windows
             * emits SGR sequences with trailing semicolons. */
            if (!p->param_present && i - 1 == p->param_count - 1)
                break;
            reset_pen(vt);
            break;
        case 1:
            vt->cursor.pen.attrs |= CFR_ATTR_BOLD;
            break;
        case 3:
            vt->cursor.pen.attrs |= CFR_ATTR_ITALIC;
            break;
        case 4:
        {
            /* CSI 4 m       → single underline.
             * CSI 4:N m     → extended underline style (N ∈ 0..5).
             * CSI 4;N m     → SGR 4 followed by SGR N — the N must NOT
             *                 be consumed here (e.g. 4;3 = underline +
             *                 italic). The subparam bit distinguishes. */
            uint8_t style = CFR_UL_SINGLE;
            if (i < p->param_count && param_is_subparam(p, i)) {
                uint32_t n = p->params[i++];
                if (n <= 5)
                    style = (uint8_t)n;
            }
            vt->cursor.pen.underline = style;
            break;
        }
        case 5:
            vt->cursor.pen.attrs |= CFR_ATTR_BLINK;
            break;
        case 2:
            vt->cursor.pen.attrs |= CFR_ATTR_DIM;
            break;
        case 8:
            vt->cursor.pen.attrs |= CFR_ATTR_INVIS;
            break;
        case 7:
            vt->cursor.pen.attrs |= CFR_ATTR_REVERSE;
            break;
        case 9:
            vt->cursor.pen.attrs |= CFR_ATTR_STRIKETHROUGH;
            break;
        case 22:
            vt->cursor.pen.attrs &= (uint16_t)~(CFR_ATTR_BOLD | CFR_ATTR_DIM);
            break;
        case 23:
            vt->cursor.pen.attrs &= (uint16_t)~CFR_ATTR_ITALIC;
            break;
        case 24:
            vt->cursor.pen.underline = CFR_UL_NONE;
            break;
        case 25:
            vt->cursor.pen.attrs &= (uint16_t)~CFR_ATTR_BLINK;
            break;
        case 27:
            vt->cursor.pen.attrs &= (uint16_t)~CFR_ATTR_REVERSE;
            break;
        case 28:
            vt->cursor.pen.attrs &= (uint16_t)~(CFR_ATTR_INVIS);
            break;
        case 29:
            vt->cursor.pen.attrs &= (uint16_t)~CFR_ATTR_STRIKETHROUGH;
            break;
        /* Foreground 16-color (30-37 normal, 90-97 bright). */
        case 30:
        case 31:
        case 32:
        case 33:
        case 34:
        case 35:
        case 36:
        case 37:
        {
            vt->cursor.pen.fg_rgb = cfr_palette_lookup(vt, (uint8_t)(s - 30));
            vt->cursor.pen.color_flags &= (uint16_t)~CFR_COLOR_DEFAULT_FG;
            break;
        }
        case 90:
        case 91:
        case 92:
        case 93:
        case 94:
        case 95:
        case 96:
        case 97:
        {
            vt->cursor.pen.fg_rgb = cfr_palette_lookup(vt, (uint8_t)(s - 90 + 8));
            vt->cursor.pen.color_flags &= (uint16_t)~CFR_COLOR_DEFAULT_FG;
            break;
        }
        case 38:
        {
            bool def = false;
            uint32_t rgb = 0;
            i = parse_color_arg(vt, i, &rgb, &def);
            if (def) {
                vt->cursor.pen.color_flags |= CFR_COLOR_DEFAULT_FG;
            } else {
                vt->cursor.pen.fg_rgb = rgb;
                vt->cursor.pen.color_flags &= (uint16_t)~CFR_COLOR_DEFAULT_FG;
            }
            break;
        }
        case 39:
            vt->cursor.pen.color_flags |= CFR_COLOR_DEFAULT_FG;
            break;
        /* Background 16-color (40-47, 100-107). */
        case 40:
        case 41:
        case 42:
        case 43:
        case 44:
        case 45:
        case 46:
        case 47:
        {
            vt->cursor.pen.bg_rgb = cfr_palette_lookup(vt, (uint8_t)(s - 40));
            vt->cursor.pen.color_flags &= (uint16_t)~CFR_COLOR_DEFAULT_BG;
            break;
        }
        case 100:
        case 101:
        case 102:
        case 103:
        case 104:
        case 105:
        case 106:
        case 107:
        {
            vt->cursor.pen.bg_rgb = cfr_palette_lookup(vt, (uint8_t)(s - 100 + 8));
            vt->cursor.pen.color_flags &= (uint16_t)~CFR_COLOR_DEFAULT_BG;
            break;
        }
        case 48:
        {
            bool def = false;
            uint32_t rgb = 0;
            i = parse_color_arg(vt, i, &rgb, &def);
            if (def) {
                vt->cursor.pen.color_flags |= CFR_COLOR_DEFAULT_BG;
            } else {
                vt->cursor.pen.bg_rgb = rgb;
                vt->cursor.pen.color_flags &= (uint16_t)~CFR_COLOR_DEFAULT_BG;
            }
            break;
        }
        case 49:
            vt->cursor.pen.color_flags |= CFR_COLOR_DEFAULT_BG;
            break;
        /* Underline color (58/59). */
        case 58:
        {
            bool def = false;
            uint32_t rgb = 0;
            i = parse_color_arg(vt, i, &rgb, &def);
            if (def) {
                vt->cursor.pen.color_flags |= CFR_COLOR_DEFAULT_UL;
            } else {
                vt->cursor.pen.ul_rgb = rgb;
                vt->cursor.pen.color_flags &= (uint16_t)~CFR_COLOR_DEFAULT_UL;
            }
            break;
        }
        case 59:
            vt->cursor.pen.color_flags |= CFR_COLOR_DEFAULT_UL;
            break;
        default:
            /* SGR 6 (rapid blink), 21 (double-underline),
             * 53 (overline) etc. are accepted-but-ignored for now. */
            break;
        }
    }
}

static void mode_set(CfrTerm *vt, bool on)
{
    CfrParser *p = &vt->parser;
    bool dec = has_intermediate(vt, '?');
    for (int i = 0; i < p->param_count; ++i) {
        uint32_t m = p->params[i];
        if (!dec) {
            /* ANSI modes. */
            switch (m) {
            case 4: /* IRM — Insert/Replace Mode */
                vt->insert_mode = on;
                break;
            default:
                break;
            }
            continue;
        }
        switch (m) {
        case 1: /* DECCKM */
            vt->decckm = on;
            break;
        case 6: /* DECOM origin mode */
            vt->decom = on;
            /* Setting DECOM also homes the cursor: with origin mode
             * on, home is the top-left of the scroll region. */
            vt->cursor.row = on ? vt->scroll_top : 0;
            vt->cursor.col = 0;
            vt->cursor.pending_wrap = false;
            break;
        case 7: /* DECAWM auto-wrap mode */
            vt->modes[CFR_MODE_DECAWM] = on;
            if (!on)
                vt->cursor.pending_wrap = false;
            break;
        case 25: /* DECTCEM cursor visible */
            vt->modes[CFR_MODE_CURSOR_VISIBLE] = on;
            vt->cursor.visible = on;
            if (vt->callbacks.set_mode)
                vt->callbacks.set_mode(CFR_MODE_CURSOR_VISIBLE, on, vt->callback_user);
            break;
        case 12: /* AT&T cursor blink */
            vt->modes[CFR_MODE_CURSOR_BLINK] = on;
            vt->cursor.blink = on;
            if (vt->callbacks.set_mode)
                vt->callbacks.set_mode(CFR_MODE_CURSOR_BLINK, on, vt->callback_user);
            break;
        case 1049: /* altscreen + save/restore cursor */
            cfr_set_altscreen(vt, on, true);
            break;
        case 47:
        case 1047: /* raw altscreen */
            cfr_set_altscreen(vt, on, false);
            break;
        case 1000:
            vt->modes[CFR_MODE_MOUSE_BTN_EVENT] = on;
            if (vt->callbacks.set_mode)
                vt->callbacks.set_mode(CFR_MODE_MOUSE_BTN_EVENT, on, vt->callback_user);
            break;
        case 1002:
            vt->modes[CFR_MODE_MOUSE_DRAG] = on;
            if (vt->callbacks.set_mode)
                vt->callbacks.set_mode(CFR_MODE_MOUSE_DRAG, on, vt->callback_user);
            break;
        case 1003:
            vt->modes[CFR_MODE_MOUSE_ANY_EVENT] = on;
            if (vt->callbacks.set_mode)
                vt->callbacks.set_mode(CFR_MODE_MOUSE_ANY_EVENT, on, vt->callback_user);
            break;
        case 1006:
            vt->modes[CFR_MODE_MOUSE_SGR] = on;
            if (vt->callbacks.set_mode)
                vt->callbacks.set_mode(CFR_MODE_MOUSE_SGR, on, vt->callback_user);
            break;
        case 2004:
            vt->modes[CFR_MODE_BRACKETED_PASTE] = on;
            if (vt->callbacks.set_mode)
                vt->callbacks.set_mode(CFR_MODE_BRACKETED_PASTE, on, vt->callback_user);
            break;
        case 2027:
            vt->modes[CFR_MODE_GRAPHEME_CLUSTERS] = on;
            if (vt->callbacks.set_mode)
                vt->callbacks.set_mode(CFR_MODE_GRAPHEME_CLUSTERS, on, vt->callback_user);
            break;
        case 2026:
            vt->modes[CFR_MODE_SYNC_OUTPUT] = on;
            if (vt->callbacks.set_mode)
                vt->callbacks.set_mode(CFR_MODE_SYNC_OUTPUT, on, vt->callback_user);
            break;
        case 80: /* DECSDM — sixel display (in-place) mode */
            vt->modes[CFR_MODE_SIXEL_SCROLLING] = on;
            if (vt->callbacks.set_mode)
                vt->callbacks.set_mode(CFR_MODE_SIXEL_SCROLLING, on, vt->callback_user);
            break;
        case 1070: /* private sixel color registers */
            vt->modes[CFR_MODE_SIXEL_PRIVATE_REGS] = on;
            if (vt->callbacks.set_mode)
                vt->callbacks.set_mode(CFR_MODE_SIXEL_PRIVATE_REGS, on, vt->callback_user);
            break;
        case 8452: /* cursor to right of sixel graphic */
            vt->modes[CFR_MODE_SIXEL_CURSOR_RIGHT] = on;
            if (vt->callbacks.set_mode)
                vt->callbacks.set_mode(CFR_MODE_SIXEL_CURSOR_RIGHT, on, vt->callback_user);
            break;
        case 5: /* DECSCNM — reverse video screen mode */
            vt->modes[CFR_MODE_REVERSE_VIDEO] = on;
            if (vt->callbacks.set_mode)
                vt->callbacks.set_mode(CFR_MODE_REVERSE_VIDEO, on, vt->callback_user);
            break;
        case 1034: /* Meta key mode */
            vt->modes[CFR_MODE_META] = on;
            if (vt->callbacks.set_mode)
                vt->callbacks.set_mode(CFR_MODE_META, on, vt->callback_user);
            if (on && should_log_once(vt, CFR_LOGGED_META))
                cfr_log(vt, CFR_LOG_WARN, "meta mode (?1034) accepted but not enforced");
            break;
        case 69: /* Left/right margin mode (DECSLRM) */
            vt->modes[CFR_MODE_LEFT_RIGHT_MARGINS] = on;
            if (vt->callbacks.set_mode)
                vt->callbacks.set_mode(CFR_MODE_LEFT_RIGHT_MARGINS, on, vt->callback_user);
            if (on && should_log_once(vt, CFR_LOGGED_LR_MARGIN))
                cfr_log(vt, CFR_LOG_WARN, "left/right margins (?69) accepted but not enforced");
            break;
        default:
            break;
        }
    }
}

void cfr_csi_dispatch(CfrTerm *vt, uint8_t final)
{
    cfr_flush_cluster(vt); /* commit any pending cluster before state-changing op */
    CfrParser *p = &vt->parser;
    int p1 = param_or(vt, 0, 1);
    int p2 = param_or(vt, 1, 1);

    switch (final) {
    /* Cursor moves */
    case 'A':
        cursor_to(vt, vt->cursor.row - p1, vt->cursor.col);
        break; /* CUU */
    case 'B':
        cursor_to(vt, vt->cursor.row + p1, vt->cursor.col);
        break; /* CUD */
    case 'C':
        cursor_to(vt, vt->cursor.row, vt->cursor.col + p1);
        break; /* CUF */
    case 'D':
        cursor_to(vt, vt->cursor.row, vt->cursor.col - p1);
        break; /* CUB */
    case 'E':
        cursor_to(vt, vt->cursor.row + p1, 0);
        break; /* CNL */
    case 'F':
        cursor_to(vt, vt->cursor.row - p1, 0);
        break; /* CPL */
    case 'G':
        cursor_to(vt, vt->cursor.row, p1 - 1);
        break; /* CHA */
    case 'H':
    case 'f': /* CUP / HVP */
        cursor_to_origin(vt, p1 - 1, p2 - 1);
        break;
    case 'd':
        cursor_to_origin(vt, p1 - 1, vt->cursor.col);
        break; /* VPA */

    case 'J':
        cfr_erase_in_display(vt, param_or(vt, 0, 0));
        break;
    case 'K':
        cfr_erase_in_line(vt, param_or(vt, 0, 0));
        break;

    case 'S':
        if (has_intermediate(vt, '?')) {
            /* XTSMGRAPHICS — CSI ? Pi ; Pa ; Pv S. Report fixed sixel
             * capabilities; status 0 = success. Pi 1 = color registers,
             * Pi 2 = graphics geometry. We ignore Pa (read/set/max) and
             * always report our maximum. */
            int pi = param_or(vt, 0, 0);
            char buf[48];
            int n = 0;
            if (pi == 1) {
                n = snprintf(buf, sizeof(buf), "\x1b[?1;0;%dS", 256);
            } else if (pi == 2) {
                int gw = vt->cols * vt->cell_w_px;
                int gh = vt->rows * vt->cell_h_px;
                if (gw > 10000)
                    gw = 10000;
                if (gh > 10000)
                    gh = 10000;
                n = snprintf(buf, sizeof(buf), "\x1b[?2;0;%d;%dS", gw, gh);
            } else {
                n = snprintf(buf, sizeof(buf), "\x1b[?%d;3;0S", pi); /* failure */
            }
            if (n > 0)
                cfr_emit_bytes(vt, (const uint8_t *)buf, (size_t)n);
            break;
        }
        cfr_scroll_up(vt, p1);
        break;
    case 'T':
        cfr_scroll_down(vt, p1);
        break;

    case 'r':
    { /* DECSTBM — Set Top and Bottom Margins */
        int top = param_or(vt, 0, 1) - 1;
        int bottom = param_or(vt, 1, vt->rows) - 1;
        if (top < 0)
            top = 0;
        if (bottom >= vt->rows)
            bottom = vt->rows - 1;
        /* Per VT100/xterm: an invalid region (top >= bottom) is
         * rejected — leave the previous scroll region and cursor
         * unchanged. brick (Haskell vty) emits `CSI 1;1 r` as part
         * of its inline-render setup; if we accepted that as a
         * one-row region, every subsequent `\n` would be pinned to
         * that single row and inline UIs would draw at row 0,
         * clobbering the prompts above. */
        if (top >= bottom)
            break;
        vt->scroll_top = top;
        vt->scroll_bottom = bottom;
        /* DECSTBM homes the cursor: with origin mode on, home is
         * the top-left of the new scroll region. */
        cursor_to(vt, vt->decom ? top : 0, 0);
        break;
    }

    case 'h':
        mode_set(vt, true);
        break;
    case 'l':
        mode_set(vt, false);
        break;

    case 'm':
        if (p->intermediate_count > 0 &&
            p->intermediates[0] >= 0x3c && p->intermediates[0] <= 0x3f) {
            /* CSI {<,=,>,?} … m — DEC-private form. Examples:
             *   `CSI > Pp ; Pv m` — xterm modifyOtherKeys (key-resource
             *      set/reset). We cover the same ground via the kitty
             *      keyboard protocol, so this is a no-op.
             *   `CSI ? Pm m` — crush emits this on startup (probing for
             *      an xterm extension). Undefined for us; ignore.
             * Critical: do NOT fall through to sgr_dispatch — interpreting
             * e.g. `?4` as SGR 4 turns on underline and paints the next
             * styled run underlined until the program's next reset. */
            break;
        }
        sgr_dispatch(vt);
        break;

    case '@':
        cfr_insert_chars(vt, p1);
        break; /* ICH */
    case 'P':
        cfr_delete_chars(vt, p1);
        break; /* DCH */
    case 'X':
        cfr_erase_chars(vt, p1);
        break; /* ECH */
    case 'L':
        cfr_insert_lines(vt, p1);
        break; /* IL  */
    case 'M':
        cfr_delete_lines(vt, p1);
        break; /* DL  */

    case 'g':
    { /* TBC — Tab Clear */
        int mode = param_or(vt, 0, 0);
        if (!vt->tabstops)
            break;
        if (mode == 0) {
            if (vt->cursor.col >= 0 && vt->cursor.col < vt->cols)
                vt->tabstops[vt->cursor.col] = 0;
        } else if (mode == 3) {
            memset(vt->tabstops, 0, (size_t)vt->cols);
        }
        break;
    }

    case 's': /* CSI s — Save Cursor (ANSI.SYS form). Only the bare
               * variant; `CSI ? s` is XTSAVE for DEC private modes
               * and `CSI < s` / `CSI > s` are vendor extensions we
               * do not implement. */
        if (p->intermediate_count == 0)
            *cfr_active_saved_cursor(vt) = vt->cursor;
        break;
    case 'u':
        if (p->intermediate_count == 0) {
            /* CSI u — ANSI.SYS Restore Cursor. */
            cfr_cursor_restore(vt, cfr_active_saved_cursor(vt));
        } else if (has_intermediate(vt, '?')) {
            /* CSI ? u — kitty keyboard query. Reply with the active
             * flags at the top of the stack as `CSI ? <flags> u`. The
             * `u` is the final byte; no space separator. */
            char buf[32];
            int n = snprintf(buf, sizeof(buf), "\x1b[?%uu",
                             (unsigned)vt->kitty_kb_stack[vt->kitty_kb_depth]);
            cfr_emit_bytes(vt, (const uint8_t *)buf, (size_t)n);
        } else if (has_intermediate(vt, '>')) {
            /* CSI > flags u — kitty keyboard push. The new top of the
             * stack is `flags`. If the stack is full, overwrite the
             * top in place rather than dropping the request. */
            uint32_t flags = (uint32_t)param_or(vt, 0, 0);
            if (vt->kitty_kb_depth + 1 < 16)
                vt->kitty_kb_depth++;
            vt->kitty_kb_stack[vt->kitty_kb_depth] = flags;
        } else if (has_intermediate(vt, '<')) {
            /* CSI < n u — kitty keyboard pop n levels (default 1).
             * Never goes below depth 0; the baseline mask at index 0
             * is left at zero so popping past the bottom restores
             * legacy emit behaviour. */
            int n = param_or(vt, 0, 1);
            if (n < 1)
                n = 1;
            while (n-- > 0 && vt->kitty_kb_depth > 0) {
                vt->kitty_kb_stack[vt->kitty_kb_depth] = 0;
                vt->kitty_kb_depth--;
            }
        } else if (has_intermediate(vt, '=')) {
            /* CSI = flags ; mode u — update the active flags. Per the
             * kitty keyboard protocol:
             *   mode 1 (default) — reset all flags, then set given.
             *   mode 2           — set given (OR in).
             *   mode 3           — reset given (AND-NOT).
             * Mode 1 is the cleanup form most TUIs use on exit:
             * `CSI = 0 ; 1 u` clears every flag they pushed without
             * needing to know what was there. Crush relies on this. */
            uint32_t flags = (uint32_t)param_or(vt, 0, 0);
            int mode = param_or(vt, 1, 1);
            uint32_t *top = &vt->kitty_kb_stack[vt->kitty_kb_depth];
            if (mode == 1)
                *top = flags;
            else if (mode == 2)
                *top |= flags;
            else if (mode == 3)
                *top &= ~flags;
        }
        break;

    case 'n': /* DSR */
        if (param_or(vt, 0, 0) == 6) {
            /* Cursor position report. */
            char buf[32];
            int n = snprintf(buf, sizeof(buf), "\x1b[%d;%dR",
                             vt->cursor.row + 1, vt->cursor.col + 1);
            cfr_emit_bytes(vt, (const uint8_t *)buf, (size_t)n);
        } else if (param_or(vt, 0, 0) == 5) {
            cfr_emit_bytes(vt, (const uint8_t *)"\x1b[0n", 4);
        }
        break;

    case 'c': /* Primary Device Attributes. */
        if (!has_intermediate(vt, '>')) {
            /* VT220, with ANSI colors (22) and sixel graphics (4). The
             * `4` is what sixel-probing apps (img2sixel, lsix, chafa)
             * look for before emitting sixel. */
            cfr_emit_bytes(vt, (const uint8_t *)"\x1b[?62;4;22c", 11);
        } else {
            /* Secondary DA: identify as coffer with version 0. */
            cfr_emit_bytes(vt, (const uint8_t *)"\x1b[>1;0;0c", 9);
        }
        break;

    case 'i':
    { /* Media copy (mc0/mc4/mc5) — printer control, not implemented. */
        if (should_log_once(vt, CFR_LOGGED_MEDIA_COPY))
            cfr_log(vt, CFR_LOG_WARN, "media copy (CSI i) not implemented yet");
        break;
    }

    case 'b':
    { /* REP — Repeat the preceding graphic character Ps times (default 1). */
        int n = param_or(vt, 0, 1);
        if (n < 1)
            n = 1;
        if (vt->last_char) {
            for (int k = 0; k < n; k++)
                cfr_print_codepoint(vt, vt->last_char);
        }
        break;
    }
    default:
        /* TODO: DA3, DECRQM, DECSCUSR cursor shape, etc. */
        break;
    }
    (void)p;
}
