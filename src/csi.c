/*
 * coffer — CSI dispatcher.
 *
 * Cursor moves, erase-in-line / erase-in-display, the SGR repertoire
 * (attributes, 4:N underlines, 38/48/58 8-bit and 24-bit colors),
 * DECSET/DECRST, DECSTBM, tab stops, save/restore cursor, DECSCUSR,
 * device queries and reports.
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
    cfr_cursor_set(vt, row, col);
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
        cfr_cursor_set(vt, row, col);
        return;
    }
    cursor_to(vt, row, col);
}

static void reset_pen(CfrTerm *vt)
{
    /* Zeroed style colors are defaults (CFR_COLOR_KIND_DEFAULT == 0). */
    memset(&vt->cursor.pen, 0, sizeof(vt->cursor.pen));
}

/* Set all four fields of a style color. Keeping `reserved` at 0 here (and
 * everywhere a color is built) preserves the no-padding invariant the
 * style intern table's raw-byte hash relies on. */
static void color_set(CfrStyleColor *c, CfrColorKind kind, uint8_t index,
                      uint32_t rgb)
{
    c->kind = (uint8_t)kind;
    c->index = index;
    c->rgb = rgb;
    c->reserved = 0;
}

/* Consume an SGR color sub-sequence starting at index `i`. Fills `out`
 * and sets *out_valid; an unknown mode or a missing 256-color index
 * leaves the pen's color unchanged. Returns the new index after the
 * sub-sequence. Both 38;5;N and 38:5:N and 38;2;R;G;B and 38:2:R:G:B
 * forms are accepted (xterm and ITU/ISO variants). */
static int parse_color_arg(CfrTerm *vt, int i, CfrStyleColor *out,
                           bool *out_valid)
{
    CfrParser *p = &vt->parser;
    *out_valid = false;
    if (i >= p->param_count) {
        /* Bare 38/48/58 with no mode: reset to the default color. */
        color_set(out, CFR_COLOR_KIND_DEFAULT, 0, 0);
        *out_valid = true;
        return i;
    }
    uint32_t mode = p->params[i++];
    if (mode == 5) {
        if (i < p->param_count) {
            color_set(out, CFR_COLOR_KIND_INDEXED, (uint8_t)p->params[i++], 0);
            *out_valid = true;
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
        color_set(out, CFR_COLOR_KIND_RGB, 0,
                  ((r & 0xFFu) << 16) | ((g & 0xFFu) << 8) | (b & 0xFFu));
        *out_valid = true;
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
        case 21:
            /* Double underline (kitty, xterm, foot). Legacy ANSI hardware
             * used 21 for "bold off"; the double underline reading wins
             * here, and 22 stays the reset for bold/dim. */
            vt->cursor.pen.underline = CFR_UL_DOUBLE;
            break;
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
        case 53:
            vt->cursor.pen.attrs |= CFR_ATTR_OVERLINE;
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
        case 55:
            vt->cursor.pen.attrs &= (uint16_t)~CFR_ATTR_OVERLINE;
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
            color_set(&vt->cursor.pen.fg, CFR_COLOR_KIND_INDEXED,
                      (uint8_t)(s - 30), 0);
            break;
        case 90:
        case 91:
        case 92:
        case 93:
        case 94:
        case 95:
        case 96:
        case 97:
            color_set(&vt->cursor.pen.fg, CFR_COLOR_KIND_INDEXED,
                      (uint8_t)(s - 90 + 8), 0);
            break;
        case 38:
        {
            CfrStyleColor c = { 0 };
            bool valid = false;
            i = parse_color_arg(vt, i, &c, &valid);
            if (valid)
                vt->cursor.pen.fg = c;
            break;
        }
        case 39:
            color_set(&vt->cursor.pen.fg, CFR_COLOR_KIND_DEFAULT, 0, 0);
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
            color_set(&vt->cursor.pen.bg, CFR_COLOR_KIND_INDEXED,
                      (uint8_t)(s - 40), 0);
            break;
        case 100:
        case 101:
        case 102:
        case 103:
        case 104:
        case 105:
        case 106:
        case 107:
            color_set(&vt->cursor.pen.bg, CFR_COLOR_KIND_INDEXED,
                      (uint8_t)(s - 100 + 8), 0);
            break;
        case 48:
        {
            CfrStyleColor c = { 0 };
            bool valid = false;
            i = parse_color_arg(vt, i, &c, &valid);
            if (valid)
                vt->cursor.pen.bg = c;
            break;
        }
        case 49:
            color_set(&vt->cursor.pen.bg, CFR_COLOR_KIND_DEFAULT, 0, 0);
            break;
        /* Underline color (58/59). */
        case 58:
        {
            CfrStyleColor c = { 0 };
            bool valid = false;
            i = parse_color_arg(vt, i, &c, &valid);
            if (valid)
                vt->cursor.pen.ul = c;
            break;
        }
        case 59:
            color_set(&vt->cursor.pen.ul, CFR_COLOR_KIND_DEFAULT, 0, 0);
            break;
        default:
            /* SGR 6 (rapid blink), 10-19 (alternate fonts), 26 (proportional
             * spacing) and the rest of the ECMA-48 tail are
             * accepted-but-ignored. */
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
            cfr_cursor_set(vt, on ? vt->scroll_top : 0, 0);
            break;
        case 7: /* DECAWM auto-wrap mode */
            cfr_mode_flag_set(vt, CFR_MODE_DECAWM, on);
            if (!on)
                cfr_cursor_kill_phantom(vt);
            break;
        case 25: /* DECTCEM cursor visible */
            cfr_cursor_set_visible(vt, on);
            break;
        case 12: /* AT&T 610 — start/stop blinking cursor */
            cfr_cursor_set_style(vt, vt->cursor.shape, on);
            break;
        case 13: /* "Disable blinking cursor" — xterm ignores it (cursor
                  * blinking is a user preference), and so do we: the
                  * blink state is ?12's and DECSCUSR's alone. */
        case 14: /* XOR of blinking cursor control sequence — xterm
                  * ignores it (user preference) too. */
            break;
        case 1049: /* altscreen + save/restore cursor */
            cfr_set_altscreen(vt, on, true);
            break;
        case 47:
        case 1047: /* raw altscreen */
            cfr_set_altscreen(vt, on, false);
            break;
        case 1000:
            cfr_mode_flag_set(vt, CFR_MODE_MOUSE_BTN_EVENT, on);
            break;
        case 1002:
            cfr_mode_flag_set(vt, CFR_MODE_MOUSE_DRAG, on);
            break;
        case 1003:
            cfr_mode_flag_set(vt, CFR_MODE_MOUSE_ANY_EVENT, on);
            break;
        case 1006:
            cfr_mode_flag_set(vt, CFR_MODE_MOUSE_SGR, on);
            break;
        case 2004:
            cfr_mode_flag_set(vt, CFR_MODE_BRACKETED_PASTE, on);
            break;
        case 2027:
            cfr_mode_flag_set(vt, CFR_MODE_GRAPHEME_CLUSTERS, on);
            break;
        case 2026:
            cfr_mode_flag_set(vt, CFR_MODE_SYNC_OUTPUT, on);
            break;
        case 80: /* DECSDM — sixel display (in-place) mode */
            cfr_mode_flag_set(vt, CFR_MODE_SIXEL_SCROLLING, on);
            break;
        case 1070: /* private sixel color registers */
            cfr_mode_flag_set(vt, CFR_MODE_SIXEL_PRIVATE_REGS, on);
            break;
        case 8452: /* cursor to right of sixel graphic */
            cfr_mode_flag_set(vt, CFR_MODE_SIXEL_CURSOR_RIGHT, on);
            break;
        case 5: /* DECSCNM — reverse video screen mode */
            cfr_mode_flag_set(vt, CFR_MODE_REVERSE_VIDEO, on);
            break;
        case 1034: /* Meta key mode */
            cfr_mode_flag_set(vt, CFR_MODE_META, on);
            if (on && should_log_once(vt, CFR_LOGGED_META))
                cfr_log(vt, CFR_LOG_WARN, "meta mode (?1034) accepted but not enforced");
            break;
        case 69: /* Left/right margin mode (DECSLRM) */
            cfr_mode_flag_set(vt, CFR_MODE_LEFT_RIGHT_MARGINS, on);
            if (on && should_log_once(vt, CFR_LOGGED_LR_MARGIN))
                cfr_log(vt, CFR_LOG_WARN, "left/right margins (?69) accepted but not enforced");
            break;
        default:
            break;
        }
    }
}

/* DECSCUSR (CSI Ps SP q) — set cursor style, xterm/VT520:
 *   0 / 1 ⇒ blinking block,   2 ⇒ steady block,
 *   3     ⇒ blinking underline, 4 ⇒ steady underline,
 *   5     ⇒ blinking bar,     6 ⇒ steady bar,
 *   7     ⇒ the initial (CfrConfig) cursor state.
 * A missing Ps behaves like 0; anything else is ignored. */
static void decscusr(CfrTerm *vt, int ps)
{
    static const struct
    {
        CfrCursorShape shape;
        bool blink;
    } styles[] = {
        { CFR_CURSOR_BLOCK, true },      /* 0 */
        { CFR_CURSOR_BLOCK, true },      /* 1 */
        { CFR_CURSOR_BLOCK, false },     /* 2 */
        { CFR_CURSOR_UNDERLINE, true },  /* 3 */
        { CFR_CURSOR_UNDERLINE, false }, /* 4 */
        { CFR_CURSOR_BAR, true },        /* 5 */
        { CFR_CURSOR_BAR, false },       /* 6 */
    };
    if (ps == 7) {
        cfr_cursor_set_style(vt, vt->default_shape, vt->default_blink);
    } else if (ps >= 0 && ps < (int)(sizeof(styles) / sizeof(styles[0]))) {
        cfr_cursor_set_style(vt, styles[ps].shape, styles[ps].blink);
    }
}

/* ------------------------------------------------------------------ */
/* Mode reporting (DECRQM)                                             */
/* ------------------------------------------------------------------ */

/* ECMA-48 states for the `Pm` parameter of the DECRQM reply:
 * 0 not recognized, 1 set, 2 reset, 3 permanently set, 4 permanently
 * reset. A `3`/`4` mode is one whose behaviour cannot be changed by
 * DECSET/DECRST. coffer has no keyboard-lock or local-echo/monitor
 * behaviour (KAM, SRM) and never lets LF imply CR (LNM); it always
 * clusters graphemes (?2027) and always gives each sixel a fresh palette
 * (?1070); and `?69`/`?1034` are only stored, so a probe must learn that
 * DECSLRM and meta mode cannot be relied on. */
static int ansi_mode_report(const CfrTerm *vt, uint32_t m)
{
    switch (m) {
    case 2:  /* KAM — keyboard action mode */
    case 12: /* SRM — send/receive mode */
    case 20: /* LNM — line feed/new line mode */
        return 4;
    case 4: /* IRM — insert/replace mode */
        return vt->insert_mode ? 1 : 2;
    default:
        return 0;
    }
}

static int dec_mode_report(const CfrTerm *vt, uint32_t m)
{
    switch (m) {
    case 1: /* DECCKM */
        return vt->decckm ? 1 : 2;
    case 5: /* DECSCNM */
        return vt->modes[CFR_MODE_REVERSE_VIDEO] ? 1 : 2;
    case 6: /* DECOM */
        return vt->decom ? 1 : 2;
    case 7: /* DECAWM */
        return vt->modes[CFR_MODE_DECAWM] ? 1 : 2;
    case 12: /* AT&T 610 cursor blink */
        return vt->modes[CFR_MODE_CURSOR_BLINK] ? 1 : 2;
    case 25: /* DECTCEM */
        return vt->modes[CFR_MODE_CURSOR_VISIBLE] ? 1 : 2;
    case 69: /* DECLRMM — accepted, margins not enforced */
        return 4;
    case 80: /* DECSDM */
        return vt->modes[CFR_MODE_SIXEL_SCROLLING] ? 1 : 2;
    case 1000: /* mouse button events */
        return vt->modes[CFR_MODE_MOUSE_BTN_EVENT] ? 1 : 2;
    case 1002: /* mouse drag events */
        return vt->modes[CFR_MODE_MOUSE_DRAG] ? 1 : 2;
    case 1003: /* mouse any-event */
        return vt->modes[CFR_MODE_MOUSE_ANY_EVENT] ? 1 : 2;
    case 1006: /* SGR mouse encoding */
        return vt->modes[CFR_MODE_MOUSE_SGR] ? 1 : 2;
    case 1034: /* meta key mode — accepted, not enforced */
        return 4;
    case 1070: /* sixel private color registers — always on */
        return 3;
    case 2004: /* bracketed paste */
        return vt->modes[CFR_MODE_BRACKETED_PASTE] ? 1 : 2;
    case 2026: /* synchronized output */
        return vt->modes[CFR_MODE_SYNC_OUTPUT] ? 1 : 2;
    case 2027: /* grapheme clustering — always on */
        return 3;
    case 8452: /* cursor to the right of a sixel */
        return vt->modes[CFR_MODE_SIXEL_CURSOR_RIGHT] ? 1 : 2;
    /* The three alt-screen entry points share one screen state. */
    case 47:
    case 1047:
    case 1049:
        return vt->in_altscreen ? 1 : 2;
    default:
        return 0;
    }
}

/* Reply to a DECRQM query: `CSI Ps ; Pm $ y`, with the `?` private
 * marker echoed for a DEC private mode. */
static void report_mode(CfrTerm *vt, bool dec, uint32_t ps)
{
    int pm = dec ? dec_mode_report(vt, ps) : ansi_mode_report(vt, ps);
    char buf[40];
    int n = snprintf(buf, sizeof(buf), "\x1b[%s%u;%d$y", dec ? "?" : "",
                     (unsigned)ps, pm);
    if (n > 0)
        cfr_emit_bytes(vt, (const uint8_t *)buf, (size_t)n);
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
    case 'I':
        cfr_tab_forward(vt, p1);
        break; /* CHT */
    case 'Z':
        cfr_tab_backward(vt, p1);
        break; /* CBT */
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
                int cell_w = 1, cell_h = 1;
                cfr_cell_logical_px(vt, &cell_w, &cell_h);
                int gw = vt->cols * cell_w;
                int gh = vt->rows * cell_h;
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
             *      set/reset). Level 1 (Pv=1) makes modified keys with no
             *      traditional control encoding emit the xterm tilde form
             *      (CSI 27 ; mod ; code ~); Pv=0 turns it off. The kitty
             *      keyboard protocol covers the same ground and takes
             *      precedence, but honouring this lets apps that request
             *      modifyOtherKeys (Emacs's term/xterm.el sends
             *      `CSI > 4 ; 1 m`) receive Ctrl+Enter etc. without any
             *      Emacs-side changes.
             *   `CSI ? Pm m` — crush emits this on startup (probing for
             *      an xterm extension). Undefined for us; ignore.
             * Critical: do NOT fall through to sgr_dispatch — interpreting
             * e.g. `?4` as SGR 4 turns on underline and paints the next
             * styled run underlined until the program's next reset. */
            if (p->intermediates[0] == '>' && param_or(vt, 0, 0) == 4)
                vt->modify_other_keys =
                    (uint8_t)(param_or(vt, 1, 0) == 0 ? 0 : 1);
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
            cfr_cursor_assign(vt, cfr_active_saved_cursor(vt), &vt->cursor);
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
            cfr_kitty_kb_pop(vt, param_or(vt, 0, 1));
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

    case 't': /* XTWINOPS — terminal window operations */
    {
        int mode = param_or(vt, 0, 0);
        if (mode == 18) {
            /* Report text area size in character cells. */
            char buf[32];
            int n = snprintf(buf, sizeof(buf), "\x1b[8;%d;%dt",
                             vt->rows, vt->cols);
            cfr_emit_bytes(vt, (const uint8_t *)buf, (size_t)n);
        } else if (mode == 14) {
            /* Report text area size in pixels. Logical pixels, like the
             * cell size below: every pixel figure the client measures is
             * scaled once by the engine on the way back in. */
            int cell_w = 1, cell_h = 1;
            cfr_cell_logical_px(vt, &cell_w, &cell_h);
            char buf[32];
            int px_h = vt->rows * cell_h;
            int px_w = vt->cols * cell_w;
            int n = snprintf(buf, sizeof(buf), "\x1b[4;%d;%dt",
                             px_h, px_w);
            cfr_emit_bytes(vt, (const uint8_t *)buf, (size_t)n);
        } else if (mode == 16) {
            /* Report cell size in pixels — logical, so a client that
             * sizes an image by this figure produces dimensions the
             * engine can scale once (see cfr_cell_logical_px). */
            int cell_w = 1, cell_h = 1;
            cfr_cell_logical_px(vt, &cell_w, &cell_h);
            char buf[32];
            int n = snprintf(buf, sizeof(buf), "\x1b[6;%d;%dt",
                             cell_h, cell_w);
            cfr_emit_bytes(vt, (const uint8_t *)buf, (size_t)n);
        }
        break;
    }

    case 'p': /* DECRQM — Report Mode. `CSI Ps $ p` asks about an ANSI
               * mode, `CSI ? Ps $ p` about a DEC private one. Other uses
               * of this final byte (`CSI ! p` / DECSTR) stay
               * unimplemented. */
        if (has_intermediate(vt, '$')) {
            bool dec = has_intermediate(vt, '?');
            /* The private form carries two intermediates (`?` then `$`);
             * anything else is malformed and gets no reply. */
            if (p->intermediate_count != (dec ? 2 : 1))
                break;
            uint32_t ps = (p->param_count > 0) ? p->params[0] : 0;
            report_mode(vt, dec, ps);
        }
        break;

    case 'c': /* Device Attributes. */
        if (p->intermediate_count == 0) {
            /* Primary DA: VT220, with ANSI colors (22) and sixel
             * graphics (4). The `4` is what sixel-probing apps
             * (img2sixel, lsix, chafa) look for before emitting sixel. */
            cfr_emit_bytes(vt, (const uint8_t *)"\x1b[?62;4;22c", 11);
        } else if (p->intermediate_count == 1 && has_intermediate(vt, '>')) {
            /* Secondary DA: identify as coffer with version 0. */
            cfr_emit_bytes(vt, (const uint8_t *)"\x1b[>1;0;0c", 9);
        } else if (p->intermediate_count == 1 && has_intermediate(vt, '=')) {
            /* Tertiary DA (`CSI = c`): the DCS reply carrying the unit
             * ID, as xterm answers. It must not fall through to the
             * primary reply, which would claim to be a DA1 response. */
            cfr_emit_bytes(vt, (const uint8_t *)"\x1bP!|00000000\x1b\\", 14);
        }
        break;

    case 'q':
    {
        /* Shared final byte, routed by intermediate: XTVERSION
         * (CSI > Ps q) and DECSCUSR (CSI Ps SP q, set cursor style).
         * Everything else (`CSI ? Ps q`, `CSI Ps " q` / DECSCA) is
         * unknown here and stays silent. */
        if (has_intermediate(vt, '>')) {
            /* XTVERSION. Reply with DCS > | <name> ST. Emacs uses this
             * to auto-enable xterm-mouse-mode on known terminals. Ps is
             * ignored (all values query the version). */
            const char *name = vt->terminal_name ? vt->terminal_name : "coffer";
            char buf[64];
            int n = snprintf(buf, sizeof(buf), "\x1bP>|%s\x1b\\", name);
            cfr_emit_bytes(vt, (const uint8_t *)buf, (size_t)n);
        } else if (has_intermediate(vt, ' ')) {
            decscusr(vt, param_or(vt, 0, 0));
        }
        break;
    }

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
        break; /* unknown or unimplemented final byte */
    }
    (void)p;
}
