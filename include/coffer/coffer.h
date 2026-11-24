/*
 * coffer — self-contained virtual terminal engine.
 *
 * Single public header. Embed by including coffer.h and linking
 * libcoffer.a. No other internal headers are needed by consumers.
 *
 * Thread-safety: a CfrTerm is not internally synchronized; callers
 * own all synchronization. Accessors that take a `const CfrTerm *` only
 * read, so any number of threads may call them at once as long as no
 * thread is writing (cfr_input_write, cfr_resize, ...). Allocation:
 * bounded; the input-write hot path is zero-allocation in steady state.
 * See "Memory model" in README.md for details.
 */

#ifndef COFFER_H
#define COFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CfrTerm CfrTerm;

/* ------------------------------------------------------------------ */
/* Cell representation                                                 */
/* ------------------------------------------------------------------ */

/*
 * One grapheme cluster per cell. The visual width of the cluster is
 * stored on the cell so the renderer iterates the grid as a sequence
 * of (col, cell) pairs with no peek-ahead logic.
 *
 * Single-codepoint clusters store the codepoint in `cp` directly with
 * grapheme_id == 0. Multi-codepoint clusters set grapheme_id to a
 * non-zero offset into the page's grapheme arena; the full sequence
 * is retrieved with cfr_cell_get_grapheme().
 *
 * style_id, grapheme_id and hyperlink_id are PAGE-SCOPED: each page of
 * the engine's storage numbers them independently, so the same id on two
 * rows can mean different styles, clusters or links, and the same link
 * can carry different ids. They are meaningful only through the
 * cfr_cell_* accessors, with the (row, col) the cell was read from — never
 * compare them across rows, and never use one as a cache key. Testing
 * for zero ("no style / no cluster / no link") is always safe. To ask
 * whether two cells carry the same link, compare cfr_cell_hyperlink_key().
 *
 * Continuation cells (the second cell of a width-2 cluster) have
 * width == 0 and are skipped by callers.
 */
typedef struct
{
    uint32_t cp;
    uint32_t grapheme_id;
    uint32_t style_id;
    uint8_t width;
    uint8_t flags;
    uint16_t hyperlink_id;
} CfrCell;

/* Cell flags */
enum
{
    CFR_CELL_WRAPLINE = 1u << 0, /* set on the last cell of a soft-wrapped logical row */
};

/* ------------------------------------------------------------------ */
/* Style                                                               */
/* ------------------------------------------------------------------ */

/* Style attribute bits (matching the term.h order to ease term_cfr.c) */
enum
{
    CFR_ATTR_BOLD = 1u << 0,
    CFR_ATTR_ITALIC = 1u << 1,
    CFR_ATTR_BLINK = 1u << 2,
    CFR_ATTR_REVERSE = 1u << 3,
    CFR_ATTR_STRIKETHROUGH = 1u << 4,
    CFR_ATTR_DWL = 1u << 5,        /* double-width line */
    CFR_ATTR_DHL_TOP = 1u << 6,    /* double-height line, top half */
    CFR_ATTR_DHL_BOTTOM = 1u << 7, /* double-height line, bottom half */
    CFR_ATTR_DIM = 1u << 8,        /* dim/faint (SGR 2) */
    CFR_ATTR_INVIS = 1u << 9,      /* invisible/concealed (SGR 8) */
    CFR_ATTR_OVERLINE = 1u << 10,  /* overline (SGR 53) */
};

/* Underline styles: SGR 4:N */
typedef enum
{
    CFR_UL_NONE = 0,
    CFR_UL_SINGLE = 1,
    CFR_UL_DOUBLE = 2,
    CFR_UL_CURLY = 3,
    CFR_UL_DOTTED = 4,
    CFR_UL_DASHED = 5,
} CfrUnderline;

/* How a style color was requested. DEFAULT is zero, so a zero-initialized
 * CfrStyleColor (and a zero-initialized CfrStyle) is all defaults. */
typedef enum
{
    CFR_COLOR_KIND_DEFAULT = 0, /* dynamic color (OSC 10/11/12) */
    CFR_COLOR_KIND_INDEXED = 1, /* 0–255 palette index; resolve through cfr_palette_rgb() */
    CFR_COLOR_KIND_RGB = 2,     /* explicit 0x00RRGGBB */
} CfrColorKind;

/* One requested color of a style. A style records what the application
 * asked for, not a resolved RGB value: an indexed color must survive an
 * OSC 4 palette change and repaint with the new value. Renderers resolve
 * through cfr_style_fg_rgb() / _bg_ / _ul_, or switch on `kind`.
 *
 * `reserved` is always 0. It exists so CfrStyleColor has no implicit
 * padding, which keeps the style intern table's raw-byte hash and memcmp
 * deterministic. */
typedef struct
{
    uint32_t rgb;      /* valid when kind == CFR_COLOR_KIND_RGB */
    uint8_t index;     /* valid when kind == CFR_COLOR_KIND_INDEXED */
    uint8_t kind;      /* CfrColorKind */
    uint16_t reserved; /* always 0 */
} CfrStyleColor;

typedef struct
{
    CfrStyleColor fg;
    CfrStyleColor bg;
    CfrStyleColor ul;
    uint16_t attrs;    /* CFR_ATTR_* bitmask */
    uint8_t underline; /* CfrUnderline */
    uint8_t font;      /* font select (0 = primary, 1-9 alt fonts via SGR 10-19) */
} CfrStyle;

/* Row addressing. Every accessor that takes a `row` uses UNIFIED rows:
 * row >= 0 is a visible row (0 = top), row < 0 is scrollback depth
 * (-1 = the line most recently scrolled off). Scrollback rows keep the
 * width they were written at, so a row's width (cfr_get_row) can differ
 * from the grid's; `col` is checked against the row's own width. */

/* Resolve the style of the cell at (row, col). Returns NULL if the cell
 * does not exist. The pointer is valid until the next call that changes
 * the terminal (cfr_input_write, cfr_resize, cfr_reset, ...). */
const CfrStyle *cfr_cell_style(const CfrTerm *vt, int row, int col);

/* Copy the grapheme codepoints of the cell at (row, col) into `out`.
 * Returns the number of codepoints written, or 0 if the cell does not
 * exist. `out_cap` caps the write; a return value equal to `out_cap`
 * indicates truncation. For single-cp cells this returns 1 with the
 * cell's `cp`. */
size_t cfr_cell_get_grapheme(const CfrTerm *vt, int row, int col,
                             uint32_t *out, size_t out_cap);

/* Copy the hyperlink URI bytes of the cell at (row, col) into `out_uri`.
 * Returns the URI length, or 0 if the cell has no link or does not
 * exist. The URI bytes are raw (per OSC 8 spec, constrained to 32–126);
 * callers wanting a C string must NUL-terminate. A return value equal
 * to `out_cap` indicates truncation. */
size_t cfr_cell_get_hyperlink(const CfrTerm *vt, int row, int col,
                              uint8_t *out_uri, size_t out_cap);

/* A stable identity for the hyperlink of the cell at (row, col): 0 if the
 * cell has no link or does not exist, otherwise a 64-bit key derived from
 * the link's URI and its OSC 8 `id=` parameter. Unlike CfrCell.hyperlink_id
 * it means the same thing on every row, page and screen, so it is the
 * value to compare ("did the hovered link change?", "does this underline
 * run continue?") or to use as a map key. Per the OSC 8 spec an id
 * connects cells only when the URI also matches: equal (URI, id) pairs
 * always give equal keys, links with the same URI but different ids are
 * distinct, and an empty or absent id is the same as no id. Distinct links
 * differ with probability 1 - 2^-64. The key is deterministic but not a
 * stable format: do not persist it across coffer versions. The id itself
 * is not exposed; cfr_cell_get_hyperlink() returns the URI. */
uint64_t cfr_cell_hyperlink_key(const CfrTerm *vt, int row, int col);

/* ------------------------------------------------------------------ */
/* Geometry                                                            */
/* ------------------------------------------------------------------ */

typedef struct
{
    int row;
    int col;
} CfrPos;

typedef struct
{
    int start_row, start_col; /* inclusive */
    int end_row, end_col;     /* inclusive */
} CfrRect;

/* Cursor shape (DECSCUSR, `CSI Ps SP q`): which shape the cursor draws.
 * The blinking/steady half of the style is CfrCursor.blink. */
typedef enum
{
    CFR_CURSOR_BLOCK = 0, /* full-cell block (the default) */
    CFR_CURSOR_UNDERLINE, /* underlined: a bar along the cell bottom */
    CFR_CURSOR_BAR,       /* vertical bar along the cell's left edge */
} CfrCursorShape;

typedef struct
{
    int row;
    int col;
    bool visible;
    bool blink;
    CfrCursorShape shape;
} CfrCursor;

/* ------------------------------------------------------------------ */
/* Modes                                                               */
/* ------------------------------------------------------------------ */

typedef enum
{
    CFR_MODE_ALTSCREEN,
    CFR_MODE_CURSOR_VISIBLE,
    CFR_MODE_CURSOR_BLINK,
    CFR_MODE_DECAWM,        /* Auto-Wrap Mode, DECSET ?7 — default on */
    CFR_MODE_REVERSE_VIDEO, /* DECSCNM */
    CFR_MODE_BRACKETED_PASTE,
    CFR_MODE_MOUSE_X10,          /* DECSET 9 */
    CFR_MODE_MOUSE_BTN_EVENT,    /* DECSET 1000 */
    CFR_MODE_MOUSE_DRAG,         /* DECSET 1002 */
    CFR_MODE_MOUSE_ANY_EVENT,    /* DECSET 1003 */
    CFR_MODE_MOUSE_SGR,          /* DECSET 1006 */
    CFR_MODE_FOCUS_REPORTING,    /* DECSET 1004 */
    CFR_MODE_GRAPHEME_CLUSTERS,  /* mode 2027 (Contour) */
    CFR_MODE_SYNC_OUTPUT,        /* mode 2026 */
    CFR_MODE_SIXEL_SCROLLING,    /* DECSDM, mode 80 — when on, draw sixel in place */
    CFR_MODE_SIXEL_PRIVATE_REGS, /* mode 1070 — private sixel color registers */
    CFR_MODE_SIXEL_CURSOR_RIGHT, /* mode 8452 — cursor to right of graphic */
    CFR_MODE_META,               /* DECSET 1034 — meta key mode */
    CFR_MODE_LEFT_RIGHT_MARGINS, /* DECSET 69 — left/right margin support (DECSLRM) */
} CfrMode;

/* ------------------------------------------------------------------ */
/* Keyboard / mouse                                                    */
/* ------------------------------------------------------------------ */

typedef enum
{
    CFR_MOD_NONE = 0,
    CFR_MOD_SHIFT = 1u << 0,
    CFR_MOD_ALT = 1u << 1,
    CFR_MOD_CTRL = 1u << 2,
    CFR_MOD_META = 1u << 3,
} CfrMods;

typedef enum
{
    CFR_KEY_NONE = 0,
    CFR_KEY_ENTER,
    CFR_KEY_TAB,
    CFR_KEY_BACKSPACE,
    CFR_KEY_ESCAPE,
    CFR_KEY_UP,
    CFR_KEY_DOWN,
    CFR_KEY_LEFT,
    CFR_KEY_RIGHT,
    CFR_KEY_INS,
    CFR_KEY_DEL,
    CFR_KEY_HOME,
    CFR_KEY_END,
    CFR_KEY_PAGEUP,
    CFR_KEY_PAGEDOWN,
    CFR_KEY_F1,
    CFR_KEY_F2,
    CFR_KEY_F3,
    CFR_KEY_F4,
    CFR_KEY_F5,
    CFR_KEY_F6,
    CFR_KEY_F7,
    CFR_KEY_F8,
    CFR_KEY_F9,
    CFR_KEY_F10,
    CFR_KEY_F11,
    CFR_KEY_F12,
    CFR_KEY_KP_0,
    CFR_KEY_KP_1,
    CFR_KEY_KP_2,
    CFR_KEY_KP_3,
    CFR_KEY_KP_4,
    CFR_KEY_KP_5,
    CFR_KEY_KP_6,
    CFR_KEY_KP_7,
    CFR_KEY_KP_8,
    CFR_KEY_KP_9,
    CFR_KEY_KP_MULTIPLY,
    CFR_KEY_KP_PLUS,
    CFR_KEY_KP_COMMA,
    CFR_KEY_KP_MINUS,
    CFR_KEY_KP_PERIOD,
    CFR_KEY_KP_DIVIDE,
    CFR_KEY_KP_ENTER,
    CFR_KEY_KP_EQUAL,
} CfrKey;

typedef enum
{
    CFR_MOUSE_NONE = 0,
    CFR_MOUSE_LEFT,
    CFR_MOUSE_MIDDLE,
    CFR_MOUSE_RIGHT,
    CFR_MOUSE_WHEEL_UP,
    CFR_MOUSE_WHEEL_DOWN,
} CfrMouseButton;

/* ------------------------------------------------------------------ */
/* Logging                                                             */
/* ------------------------------------------------------------------ */

typedef enum
{
    CFR_LOG_DEBUG,
    CFR_LOG_INFO,
    CFR_LOG_WARN,
} CfrLogLevel;

/* ------------------------------------------------------------------ */
/* Callbacks                                                           */
/* ------------------------------------------------------------------ */

typedef struct
{
    /* damage: a rectangular region of the visible grid changed */
    void (*damage)(CfrRect rect, void *user);

    /* moverect: contents of `src` were moved to `dst` (e.g. scroll).
     * Emitted for whole-row moves of the visible grid — scrolling (LF
     * at the bottom, SU/SD, RI), IL and DL. Both rectangles are
     * inclusive and span the full width, so a host may implement this
     * with a scroll-blit; the vacated rows are still reported through
     * `damage`. */
    void (*moverect)(CfrRect dst, CfrRect src, void *user);

    /* movecursor: the cursor's appearance or visibility changed (its
     * position is read lazily via cfr_get_cursor). A plain move is not
     * reported — the damage flush covers it by dirtying the old and new
     * cursor cells. */
    void (*movecursor)(CfrCursor cur, void *user);

    /* bell: BEL received */
    void (*bell)(void *user);

    /* set_title: OSC 0/1/2 received. `utf8` is NUL-terminated and
     * valid only during the call. */
    void (*set_title)(const char *utf8, void *user);

    /* set_mode: a tracked mode changed */
    void (*set_mode)(CfrMode mode, bool on, void *user);

    /* set_color: a dynamic color changed (xterm OSC 10/11/12 family).
     * `code` is 10 (default foreground), 11 (default background) or
     * 12 (cursor); `rgb` is the new effective value as 0x00RRGGBB and
     * `set` reports whether an explicit color is in effect. `set` is
     * always true for 10/11; for 12 it turns false when the cursor
     * color is reset to its default (OSC 112) — the host then draws a
     * reverse-video cursor, and `rgb` reports the foreground the
     * default resolves to. */
    void (*set_color)(int code, uint32_t rgb, bool set, void *user);

    /* output: bytes the application produced to send to the PTY
     * (mouse reports, DA/DSR responses, kitty keyboard responses...) */
    void (*output)(const uint8_t *bytes, size_t len, void *user);

    /* sb_pushline: grid row `row` is scrolling off the top as a result of
     * output (LF at the bottom, SU, ...). Rows are announced top to bottom,
     * and each is still readable at unified row `row` — cfr_get_row(),
     * cfr_cell_style() and the other accessors — for the duration of the
     * call; when scrollback is enabled it is also readable at row -1.
     * `lineage` is the row's logical-line id (0 = blank): a value equal to
     * the one announced for the previous row means the two are fragments of
     * one soft-wrapped line.
     *
     * Fires whether or not the engine keeps the row, so a host that owns its
     * own history, or tracks scrolling to keep a view steady, sees every
     * row. It reports output only: a resize rewrites history synchronously,
     * and the caller reads the result when cfr_resize() returns. Do not
     * mutate the terminal from inside the callback. */
    void (*sb_pushline)(int row, uint32_t lineage, void *user);

    /* osc: OSC string callback (title is delivered via set_title) */
    void (*osc)(int code, const char *data, size_t len, void *user);

    /* dcs: streamed DCS callback. `final` is true on the final chunk. */
    void (*dcs)(const char *intro, const char *data, size_t len, bool final,
                void *user);

    /* log: diagnostic message from the engine (e.g. unimplemented
     * sequence warnings). NULL = silently dropped. */
    void (*log)(CfrLogLevel level, const char *msg, void *user);

    /* selection_changed: selection transitioned between active and
     * inactive. Fires on start, clear, draw-induced clear, and
     * scroll-boundary clear. Does NOT fire on update/extend.
     * NULL = host doesn't care. */
    void (*selection_changed)(bool active, void *user);
} CfrCallbacks;

/* ------------------------------------------------------------------ */
/* Allocator hooks                                                     */
/* ------------------------------------------------------------------ */

typedef struct
{
    void *(*alloc)(size_t size, void *user);
    void *(*realloc)(void *ptr, size_t size, void *user);
    void (*free)(void *ptr, void *user);
    void *user;
} CfrAllocator;

/* A dynamic color value with its explicit-set flag (the xterm OSC
 * 10/11/12 family). `set` false means "no explicit color": the engine
 * default applies (foreground #F8F8F2, background #000000, cursor = the
 * reverse-video default that resolves to the foreground). */
typedef struct
{
    bool set;
    uint32_t rgb; /* 0x00RRGGBB */
} CfrColorValue;

/* Configuration for cfr_new(). Zero-initialize (or start from
 * CFR_CONFIG_DEFAULTS) then set fields. Rows, cols, and cell pixel size
 * are required and must be > 0. `scrollback`, `scrollback_bytes` and
 * `cursor_blink` use -1 for "engine default" (their zero value is a real
 * choice). */
#define CFR_CONFIG_DEFAULTS                                                                                                                                                                                                                                                                                     \
    {                                                                                                                                                                                                                                                                                                           \
        .rows = 0, .cols = 0, .cell_w_px = 0, .cell_h_px = 0, .scrollback = -1, .scrollback_bytes = -1, .reflow = false, .ambiguous_wide = false, .terminal_name = NULL, .cursor_shape = CFR_CURSOR_BLOCK, .cursor_blink = -1, .fg_color = { false, 0 }, .bg_color = { false, 0 }, .cursor_color = { false, 0 } \
    }
typedef struct
{
    int rows;       /* grid height (required > 0) */
    int cols;       /* grid width  (required > 0) */
    int cell_w_px;  /* pixel width of one cell  (required > 0) */
    int cell_h_px;  /* pixel height of one cell (required > 0) */
    int scrollback; /* scrollback lines (-1 = default 1000, 0 = off) */
    /* Additional bound on retained scrollback cell storage, in bytes:
     * sizeof(CfrCell) * the row's width for every held row (page overhead
     * and intern tables excluded). The line count and this budget both
     * evict; whichever binds first wins. -1 = default 64 MiB, 0 = no byte
     * cap. */
    int scrollback_bytes;
    bool reflow;               /* reflow text on resize (default false) */
    bool ambiguous_wide;       /* treat ambiguous-width chars as wide (default false) */
    const char *terminal_name; /* XTVERSION response string (default "coffer") */
    /* Initial cursor style — what DECSCUSR `CSI 7 SP q` and RIS restore
     * to. `cursor_blink`: -1 = engine default (blinking), 0 = steady,
     * 1 = blinking. */
    CfrCursorShape cursor_shape;
    int cursor_blink;
    /* Initial dynamic colors — what OSC 110/111/112 and RIS reset to.
     * With `set` false the engine default applies. */
    CfrColorValue fg_color;
    CfrColorValue bg_color;
    CfrColorValue cursor_color;
} CfrConfig;

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/* Create a terminal from a config struct. Returns NULL on allocation
 * failure or if required fields (rows, cols, cell_w_px, cell_h_px)
 * are <= 0. */
CfrTerm *cfr_new(const CfrConfig *cfg);

/* Variant that takes an allocator. The allocator is copied; the user
 * pointer must outlive the term. Pass NULL to use the stdlib. */
CfrTerm *cfr_new_with_allocator(const CfrConfig *cfg, const CfrAllocator *alloc);

void cfr_free(CfrTerm *vt);

/* Wire callbacks. May be called multiple times; the table is copied. */
void cfr_set_callbacks(CfrTerm *vt, const CfrCallbacks *cb, void *user);

/* Resize and reflow if reflow is enabled. Cursor position is preserved. */
void cfr_resize(CfrTerm *vt, int rows, int cols);

/* Configuration. */
void cfr_set_reflow(CfrTerm *vt, bool enabled);
void cfr_set_ambiguous_wide(CfrTerm *vt, bool wide);
void cfr_set_scrollback_size(CfrTerm *vt, int lines);

/* ------------------------------------------------------------------ */
/* I/O                                                                 */
/* ------------------------------------------------------------------ */

/* Feed PTY bytes to the parser. Returns the number consumed (always
 * `len` unless the term is in an error state). */
size_t cfr_input_write(CfrTerm *vt, const uint8_t *bytes, size_t len);

/* Drain accumulated damage: invoke the `damage` callback (if set) with the
 * rectangular union of everything that changed since the last flush, then
 * clear the accumulator. The engine never fires `damage` on its own — the
 * consumer calls this at a controlled time (typically once per frame, just
 * before rendering), so damage from a burst of cfr_input_write() calls
 * coalesces into one callback. A cursor-only move (which dirties no grid cell)
 * is folded in here by damaging the old and new cursor cells.
 *
 * Synchronized output (DECSET 2026): while the application is mid-frame this
 * call reports nothing and keeps the union pending, so the consumer paints
 * once — the whole frame — after the `\e[?2026l` end marker instead of
 * showing intermediate states. The consumer notices the mode through the
 * `set_mode` callback (or cfr_get_mode()) and should also arm its own
 * timeout: a program that dies mid-frame would otherwise freeze the screen.
 * On expiry call cfr_sync_output_cancel() and flush what has accumulated. */
void cfr_damage_flush(CfrTerm *vt);

void cfr_send_key(CfrTerm *vt, CfrKey key, CfrMods mods);
void cfr_send_text(CfrTerm *vt, const char *utf8, size_t len, CfrMods mods);
void cfr_send_mouse(CfrTerm *vt, int row, int col, CfrMouseButton b,
                    bool pressed, CfrMods mods);

void cfr_paste_begin(CfrTerm *vt);
void cfr_paste_end(CfrTerm *vt);

/* ------------------------------------------------------------------ */
/* Reset                                                               */
/* ------------------------------------------------------------------ */

/* Reset the *input protocol* state: the kitty keyboard protocol stack,
 * xterm modifyOtherKeys, application cursor keys (DECCKM) and
 * application keypad (DECKPAM), mouse reporting and focus reporting,
 * and a synchronized-output hold (DECSET 2026).
 * The screen, the scrollback and the bracketed-paste mode are left
 * alone — bracketed paste is a mode a shell owns (bash enables it at
 * startup), so clearing it would break pasting into the shell.
 *
 * Use this to recover from a program that enabled one of those
 * protocols and died without cleaning up: the shell then receives
 * legacy key encodings again (Ctrl+A is 0x01, not CSI 97;5u), and a
 * program that died mid-frame no longer freezes the screen (the pending
 * damage is delivered on the next cfr_damage_flush()). */
void cfr_reset_input_state(CfrTerm *vt);

/* Full reset (RIS) — what the shell command `reset` triggers. Clears
 * everything cfr_reset_input_state() clears, plus the screen, the
 * scrollback, the character sets, the scroll region, the tab stops,
 * the saved cursor and the pen. The cursor style and the dynamic
 * colors (OSC 10/11/12) return to their CfrConfig values. */
void cfr_reset(CfrTerm *vt);

/* Kitty keyboard protocol flags currently in effect — the top of the
 * stack. 0 means the protocol is off and keys are encoded the legacy
 * way. */
uint32_t cfr_get_kitty_kb_flags(const CfrTerm *vt);

/* Pop `levels` entries from the kitty keyboard protocol stack. A
 * `levels` of 0 or less pops the whole stack. Popping the last entry
 * resets the flags to zero, per the kitty keyboard protocol spec. */
void cfr_kitty_kb_pop(CfrTerm *vt, int levels);

/* The whole kitty keyboard protocol stack state, for diagnostics: the
 * active mask (`*flags`, the same value cfr_get_kitty_kb_flags returns)
 * and how many entries sit above the zero baseline (`*depth`; 0 = the
 * program in the terminal pushed nothing, so a non-zero `*depth` with
 * the program gone is a leaked protocol). Either out pointer may be
 * NULL. Returns 0 when `vt` is NULL. */
int cfr_get_kitty_kb_state(const CfrTerm *vt, uint32_t *flags, int *depth);

/* ------------------------------------------------------------------ */
/* Queries                                                             */
/* ------------------------------------------------------------------ */

/* The cell at unified (row, col); NULL when it does not exist. Continuation
 * cells (width == 0) are returned as-is; callers skip them. The pointer is
 * valid until the next call that changes the terminal. */
const CfrCell *cfr_get_cell(const CfrTerm *vt, int row, int col);

/* The cells of one unified row, contiguous, and the row's own width in
 * *out_cols (optional). NULL when the row does not exist. Read a whole row
 * through this rather than per-column cfr_get_cell() calls. Same lifetime
 * as cfr_get_cell(). */
const CfrCell *cfr_get_row(const CfrTerm *vt, int row, int *out_cols);

void cfr_get_dimensions(const CfrTerm *vt, int *out_rows, int *out_cols);

/* Number of lines currently held in scrollback (grows as content scrolls off,
 * capped at the configured capacity). */
int cfr_get_scrollback_lines(const CfrTerm *vt);
/* Configured scrollback capacity in lines (what cfr_set_scrollback_size set, or
 * the built-in default). 0 means scrollback is disabled. */
int cfr_get_scrollback_capacity(const CfrTerm *vt);

CfrCursor cfr_get_cursor(const CfrTerm *vt);
const char *cfr_get_title(const CfrTerm *vt);
bool cfr_is_altscreen(const CfrTerm *vt);
bool cfr_get_mode(const CfrTerm *vt, CfrMode mode);
/* End a synchronized-output hold (DECSET 2026) early. Hosts call this
 * from their frame timeout: an application that dies mid-frame leaves
 * the mode set, and cfr_damage_flush() would otherwise report nothing
 * forever. Pending damage is kept, so the next flush paints what the
 * application had drawn; a later `\e[?2026l` from it is harmless.
 * No-op when the mode is already off; touches nothing else (unlike
 * cfr_reset_input_state()). */
void cfr_sync_output_cancel(CfrTerm *vt);
/* True when unified `row` continues the row above it (the row above
 * soft-wrapped into it). Unified coordinates: negative rows are
 * scrollback (-1 = most recent). A join requires both ends of the
 * handshake: the row above's margin cell still carries the wrap edge,
 * and both rows share the same nonzero logical-line id. The inverse
 * question ("does row r wrap into r+1?") is this predicate at r+1. */
bool cfr_row_is_continuation(const CfrTerm *vt, int row);

/* ------------------------------------------------------------------ */
/* Introspection (row text + unified-buffer search)                    */
/* ------------------------------------------------------------------ */

/* Flatten one unified row into UTF-8 text: erased/printed blanks become
 * spaces, continuation cells of wide clusters are skipped,
 * multi-codepoint graphemes are encoded whole. Cells that were never
 * written carry no content at all (cp == 0, width == 0,
 * indistinguishable from continuation cells by design — reflow drops
 * them too) and are collapsed: a cursor-positioned gap does not flatten
 * to spaces. Unified coordinates: `row` >= 0 is a visible row,
 * `row` < 0 is scrollback depth (-1 = the line most recently scrolled
 * off). Returns the number of bytes written, excluding the NUL
 * terminator; the output is NUL-terminated whenever `cap` > 0.
 * Out-of-range rows produce an empty string. */
size_t cfr_row_text(const CfrTerm *vt, int row, char *buf, size_t cap);

/* Buffer capacity (including the NUL) that flattens unified `row` with
 * cfr_row_text() without truncation, whatever its content; 0 if the row
 * does not exist. Size by this, not by the grid width: scrollback rows keep
 * the width they were written at and can be wider than the grid. */
size_t cfr_row_text_size(const CfrTerm *vt, int row);

/* Search the unified buffer — scrollback (oldest first), then the
 * visible rows — for `needle` (matched as UTF-8). With `line_end` a
 * row matches only when it ends with the needle (trailing blanks
 * trimmed). A NULL or empty needle is an immediate match with
 * *out_row = 0. On a match returns true and reports the unified row
 * via *out_row (< 0 = scrollback depth). */
bool cfr_find_text(CfrTerm *vt, const char *needle, bool line_end,
                   int *out_row);

/* ------------------------------------------------------------------ */
/* Palette and default colors                                          */
/* ------------------------------------------------------------------ */

/* Built-in default terminal background color as 0x00RRGGBB.
 * Pitch-black (#000000) — the value a term starts with and OSC 111 /
 * RIS fall back to when CfrConfig.bg_color is unset. The live value
 * can differ (CfrConfig.bg_color, OSC 11): read that through
 * cfr_get_dynamic_color(). */
uint32_t cfr_default_bg_rgb(void);

/* Built-in default terminal foreground color as 0x00RRGGBB.
 * Dracula foreground (#F8F8F2) — see cfr_default_bg_rgb(). */
uint32_t cfr_default_fg_rgb(void);

/* Lookup a color from the built-in 256-color palette (index 0–255).
 * Returns 0x00RRGGBB. This is the engine's immutable default palette; for
 * the value a renderer should draw, use cfr_palette_rgb(), which includes
 * OSC 4 overrides. */
uint32_t cfr_default_palette_rgb(uint8_t index);

/* Lookup a color from the terminal's live 256-color palette: the built-in
 * value unless an application changed the entry with OSC 4. Returns
 * 0x00RRGGBB. `vt == NULL` falls back to the built-in palette. */
uint32_t cfr_palette_rgb(const CfrTerm *vt, uint8_t index);

/* Resolve one color of a style to the value a renderer should draw:
 *   RGB       -> the explicit color
 *   INDEXED   -> cfr_palette_rgb() for its index (OSC 4 overrides included)
 *   DEFAULT   -> the live dynamic color (the default underline resolves to
 *                the default foreground, matching xterm)
 * These helpers are the recommended way to read colors: they keep the
 * palette/default rules in one place. */
uint32_t cfr_style_fg_rgb(const CfrTerm *vt, const CfrStyle *style);
uint32_t cfr_style_bg_rgb(const CfrTerm *vt, const CfrStyle *style);
uint32_t cfr_style_ul_rgb(const CfrTerm *vt, const CfrStyle *style);

/* Read one dynamic color (the xterm OSC 10/11/12 family): `code` is
 * 10 (default foreground), 11 (default background) or 12 (cursor).
 * The effective value is written to *out_rgb as 0x00RRGGBB: the
 * explicitly set color when one is in effect (CfrConfig, OSC 10/11/12),
 * otherwise the engine default. The cursor's default is the foreground
 * (xterm's cursorColor default is XtDefaultForeground).
 *
 * *out_set (optional) reports whether an explicit color is in effect;
 * it is false only for an unset cursor color, which a renderer draws as
 * a reverse-video cursor. Returns false for a NULL term or an
 * unsupported code. Either out pointer may be NULL. */
bool cfr_get_dynamic_color(const CfrTerm *vt, int code, uint32_t *out_rgb,
                           bool *out_set);

/* ------------------------------------------------------------------ */
/* Sixel graphics                                                      */
/* ------------------------------------------------------------------ */

/*
 * A decoded image, anchored to a grid line. The engine owns the
 * pixel data and the lifetime; the view returned by cfr_get_images() is
 * valid until the next cfr_input_write()/cfr_get_images() call.
 *
 * `row` is a unified row coordinate: >= 0 is a visible grid row (0 =
 * top), < 0 is scrollback depth (-1 = the line most recently scrolled
 * off). The host maps it to a display position exactly as it does for
 * cells, accounting for the user's scrollback offset.
 *
 * `id` is stable across frames for the same image (use it as a texture
 * cache key); `version` bumps whenever the pixels at that id change
 * (animation / in-place frame replacement / kitty same-id re-transmit),
 * so the host re-uploads only when it must. Versions come from a
 * store-global monotonic counter, so a removed-and-re-added id never
 * reuses a version for different pixels. `layer` is 0 for foreground
 * (drawn over text); layer 1 (background, drawn behind text) is
 * reserved for a future extension.
 *
 * THE LIFETIME OF AN IMAGE (per `source`):
 *
 *   IMG_SRC_SIXEL, IMG_SRC_ITERM — the image IS its placement: it
 *     lives as long as the screen area it covers. An erase of those
 *     rows removes it, and so does a scroll that carries it out of
 *     the scrollback.
 *
 *   IMG_SRC_KITTY — the image belongs to the CLIENT: it lives until
 *     the client deletes it (kitty's a=d), until the same id is
 *     re-transmitted, or until the store's budget evicts it. It is not
 *     touched by the text erases ("the other commands to erase text
 *     must have no effect on graphics"); the screen-wide clear
 *     (ESC [ 2 J) removes the images that are visible on the screen.
 *     Its position comes from its PLACEMENTS (see
 *     CfrImagePlacement), not from `row`.
 *
 *   IMG_SRC_LOTTIE — an animation IS a placement: it lives as long as
 *     its last placement, and an erase of the rows a placement covers
 *     removes it.
 *
 * The `id` spaces are per protocol: a kitty image's id is the
 * client's, a sixel's is the engine's, and they may be the same
 * number. `handle` is the engine's own identity for the image —
 * unique across protocols, never reused — and the placements name
 * their image by it.
 */
/* Image source identifiers for the `source` field on CfrImage. */
enum
{
    IMG_SRC_SIXEL = 0,
    IMG_SRC_LOTTIE = 1,
    IMG_SRC_ITERM = 2,
    IMG_SRC_KITTY = 3,
    /* Not a real source: the "any protocol" wildcard for the placement
     * queries, whose id spaces belong to the protocols themselves. */
    IMG_SRC_ANY = 255
};

typedef struct
{
    uint64_t id;
    /* The engine's internal handle for this image — stable for the
     * image's whole life, never reused, and unique across protocols.
     * The public `id` is the CLIENT's id for the 1:N protocols (kitty's
     * i=, Lottie's id) and the engine's own id for the 1:1 protocols
     * (sixel, iTerm2); those id spaces overlap by construction, so a
     * host that matches a placement to its image must use the handle
     * (CfrImagePlacement.image_handle), not the id. */
    uint64_t handle;
    uint32_t version;
    uint8_t layer;
    uint8_t source; /* IMG_SRC_SIXEL, IMG_SRC_LOTTIE, IMG_SRC_ITERM, IMG_SRC_KITTY */
    int row;
    int col;
    int width_px;        /* physical display pixels (coffer pre-converts) */
    int height_px;       /* physical display pixels */
    int buf_w;           /* pixel buffer width (for texture upload) — the
                          * REAL width of `rgba`, NOT scaled: the buffer
                          * holds the decoded (native) pixels */
    int buf_h;           /* pixel buffer height (see buf_w) */
    const uint8_t *rgba; /* buf_w * buf_h * 4, RGBA, engine-owned */
} CfrImage;

/* Update the pixel size of one character cell. Call on font load and on
 * resize. The cell pixel size is always known (set at creation), but this
 * updates it when it changes. */
void cfr_set_cell_pixels(CfrTerm *vt, int cell_w_px, int cell_h_px);

/* Set the DPI content scale factor (1.0 = unscaled). The host passes its
 * content_scale so the engine can compute correct sixel cell occupancy when
 * cell_px already includes the scale but image pixels are unscaled. */
void cfr_set_content_scale(CfrTerm *vt, float scale);

/* Physical pixels -> the LOGICAL (unscaled) pixels a client measures in.
 * A client's pixel dimensions are logical (the display fields are scaled
 * by content_scale once, in cfr_img_get), so every figure reported back to
 * a client goes through this. Rounds half-up and never returns below 1.
 *
 * At a fractional content scale the logical cell can be +/-1 px off
 * physical/scale, so an image sized in logical px can overflow its cell
 * box by a few device px. That is inherent to an integer-cell grid (kitty
 * and foot behave the same); it is not a defect in the rounding. */
int cfr_px_logical(int px, float content_scale);

/* Return the live images of the active screen (sixel, iTerm2, kitty,
 * Lottie). The returned array is owned by the engine and valid until the
 * next mutation; *out_count receives the number of images (0 if none).
 * Returns NULL when there are none. The other screen buffer's images are
 * not reported: kitty keeps a graphics set per screen, so a main-screen
 * image is hidden while the alternate screen is active and comes back
 * when the main screen does. */
const CfrImage *cfr_get_images(CfrTerm *vt, int *out_count);

/* A placement of an image on the terminal grid (1:N image-to-placement,
 * used by Lottie and kitty graphics). Anchored by absolute line so it
 * scrolls with text. `row` is the display-relative row (abs_line minus
 * the scroll top). `image_id` links to the owning CfrImage's id (the
 * id of the OWNING PROTOCOL's id space) and `image_handle` to its
 * engine-internal identity — the unambiguous link, since the
 * protocols' id spaces overlap. */
typedef struct
{
    uint64_t id;
    uint64_t image_id;
    /* The owning image's handle (CfrImage.handle) and its protocol
     * (IMG_SRC_*). The public `image_id` is only unique WITHIN one
     * protocol's id space — kitty's i=1 and a Lottie id of 1 are the
     * same number — so a host that pairs a placement with its image
     * must compare handles, not ids. */
    uint64_t image_handle;
    uint8_t source;
    int row; /* display-relative row */
    int col;
    int rows;
    int cols;
    uint8_t layer;
    uint8_t opacity_x256;
    int z_index;
    int src_x; /* source rect in the image (kitty); 0 = full */
    int src_y;
    int src_w;
    int src_h;
    int cell_off_x; /* X= (kitty): offset within the first cell, pixels */
    int cell_off_y; /* Y= */
} CfrImagePlacement;

/* Return the live image placements of the active screen (kitty
 * graphics). The returned array is owned by the engine and valid until
 * the next mutation; *out_count receives the number of placements (0 if
 * none). Returns NULL if empty. */
const CfrImagePlacement *cfr_get_image_placements(CfrTerm *vt, int *out_count);

/* Return placements for a single image id. Engine-owned, valid until the
 * next mutation; *out_count receives the count (0 if none). Returns NULL
 * when the image has no placements.
 *
 * The id is looked up in EVERY protocol's id space (the protocols'
 * id spaces overlap), so a kitty id of 1 and a Lottie id of 1 both
 * match. Use cfr_get_image_placements_for_source() when the protocol
 * is known. */
const CfrImagePlacement *cfr_get_image_placements_for(CfrTerm *vt,
                                                      uint64_t image_id,
                                                      int *out_count);

/* Return placements for a single image id within ONE protocol's id
 * space (source is an IMG_SRC_* value). This is the unambiguous
 * query: kitty's i=1 and a Lottie id of 1 are different images. */
const CfrImagePlacement *cfr_get_image_placements_for_source(CfrTerm *vt,
                                                             uint8_t source,
                                                             uint64_t image_id,
                                                             int *out_count);

/* --- Lottie animations ------------------------------------------------- */

/* A placement of a Lottie animation on the terminal grid. Anchored by
 * absolute line so the animation scrolls with text. `layer` is 0 for
 * foreground (drawn over text), 1 for background (drawn behind text).
 * `opacity_x256` is the per-placement opacity scaled to 0–255. */
typedef struct
{
    uint64_t id;
    long abs_line;
    int row; /* display-relative row (abs_line - sixel_abs_top) */
    int col;
    int rows;
    int cols;
    uint8_t layer;
    uint8_t opacity_x256;
} CfrLottiePlacement;

/* A Lottie animation snapshot, returned by cfr_get_lotties(). Engine-owned.
 * `rgba` is the rasterized RGBA32 pixel data for the current frame,
 * valid until the next cfr_input_write()/cfr_get_lotties() call.
 * Placements are queried separately via cfr_get_lottie_placements(). */
typedef struct
{
    uint64_t id;
    uint32_t version;
    int canvas_w;        /* rasterization width in px (design × scale, aspect-correct) */
    int canvas_h;        /* rasterization height in px (design × scale, aspect-correct) */
    const uint8_t *rgba; /* canvas_w * canvas_h * 4, RGBA32, engine-owned */
    int current_frame;
    int frame_count;
    bool playing;
    double speed;
    bool loop;
    int placement_count;
} CfrLottie;

/* Return the live Lottie animations. The returned array is a contiguous
 * array of CfrLottie structs (NO interleaved placement data), owned by the
 * engine and valid until the next mutation; *out_count receives the
 * number of animations (0 if none). Returns NULL when there are none.
 * Use cfr_get_lottie_placements() to query placements for each animation. */
const CfrLottie *cfr_get_lotties(CfrTerm *vt, int *out_count);

/* Return the placements for a specific animation. The returned array is
 * owned by the engine and valid until the next mutation. *out_count
 * receives the number of placements (0 if none). Returns NULL if the
 * animation has no placements or doesn't exist. */
const CfrLottiePlacement *cfr_get_lottie_placements(CfrTerm *vt, uint64_t id,
                                                    int *out_count);

/* Advance all playing Lottie animations to the frame that should be on
 * screen at the given presentation time, re-rasterizing any whose frame
 * changed. Returns true if any animation advanced (pixel data changed).
 *
 * The frame is a pure function of time: position = anchor + elapsed *
 * speed * frame rate, so how often the host calls this, and how late a call
 * runs, changes nothing about which frame is chosen. The clock is anchored
 * at the first call after a record starts playing, resumes, seeks or
 * changes speed.
 *
 * `present_us` (microseconds, any monotonic clock the host keeps using) is
 * when the frame about to be drawn reaches the screen; `hold_us` is how
 * long it stays there, normally one display refresh period. The frame is
 * chosen at `present_us + hold_us / 2`, so a frame boundary that lands on a
 * refresh (30 fps on a 60 Hz display) has half a refresh of margin against
 * jitter. A host without a display clock passes `hold_us = 0` (a point
 * sample at `present_us`). Call it once per presented frame, before
 * cfr_get_lotties(). */
bool cfr_lottie_present(CfrTerm *vt, uint64_t present_us, uint64_t hold_us);

/* Point-sample form: cfr_lottie_present(vt, now_us, 0). */
bool cfr_lottie_tick(CfrTerm *vt, uint64_t now_us);

/* Kitty graphics animation clock. Advances every playing kitty image's
 * current frame to `present_us` and copies the frame into the image's
 * pixels (version bump + damage), so the host draws exactly what
 * cfr_get_images() reports. `hold_us` is accepted for symmetry with
 * cfr_lottie_present and ignored. Returns true while any kitty
 * animation is still playing, for adaptive frame pacing. Call once per
 * presented frame, before cfr_get_images(). */
bool cfr_kitty_present(CfrTerm *vt, uint64_t present_us, uint64_t hold_us);

/* Point-sample form: cfr_kitty_present(vt, now_us, 0). */
bool cfr_kitty_tick(CfrTerm *vt, uint64_t now_us);

/* Number of kitty images whose animation clock is running (anim_mode run or
 * loading). Zero-allocation, side-effect-free: unlike cfr_kitty_tick() this
 * advances nothing, so a host can call it to decide whether frame pacing
 * should run. Mirrors cfr_lottie_playing_count(). */
int cfr_kitty_playing_count(CfrTerm *vt);

/* Number of animations that are currently playing (not paused, stopped, or
 * finished). Zero-allocation. Hosts use it to decide whether frame pacing
 * should run; cfr_lottie_active_count() also counts paused animations. */
int cfr_lottie_playing_count(CfrTerm *vt);

/* Notify the Lottie subsystem of a scroll event (cull scrolled-off
 * placements). Called automatically after scroll; also available for
 * manual invocation. */
void cfr_lottie_note_scroll(CfrTerm *vt, int lines);

/* Remove foreground-layer placements overlapping the inclusive display-row
 * range [top,bot]. Background placements survive. */
void cfr_lottie_clear_display_rows(CfrTerm *vt, int top, int bot);

/* Returns the number of registered Lottie animations (playing or not).
 * Zero-allocation, single field read — safe to call frequently. */
int cfr_lottie_active_count(CfrTerm *vt);

/* Returns true if coffer was built with ThorVG (Lottie rasterization
 * produces real pixels). Returns false if ThorVG was absent or disabled
 * at build time — APC sequences are accepted but produce blank frames. */
bool cfr_have_lottie(void);

/* --- UTF-8 display width ------------------------------------------------*/

/* Compute the display width (terminal columns) of a UTF-8 string.
 * Returns the number of cells the string would occupy: 1 for ASCII and
 * Narrow codepoints, 2 for East Asian Wide (CJK, emoji), 0 for zero-width
 * codepoints (combining marks, variation selectors, ZWJ, etc.).
 *
 * Cluster-aware: codepoints are grouped with the same grapheme-break
 * predicate and cluster width rule the grid uses, so an emoji
 * presentation sequence (base + U+FE0F) measures 2 cells, a regional
 * indicator pair 2, and a VS15 (text presentation) sequence measures 1
 * cell when UTS #51 defines a text glyph for the base (the base's East
 * Asian width otherwise). East Asian Ambiguous codepoints are always
 * narrow here — ambiguous width is a per-terminal setting (CfrConfig
 * .ambiguous_wide); with it enabled, measure through a CfrTerm instead
 * (the grid cells carry the effective widths).
 *
 * @param utf8  UTF-8 string (need not be NUL-terminated)
 * @param len   Length in bytes
 * @return Display width in terminal cells, or -1 on invalid UTF-8.
 */
int cfr_utf8_display_width(const char *utf8, size_t len);

/* ------------------------------------------------------------------ */
/* Selection                                                           */
/* ------------------------------------------------------------------ */

typedef enum
{
    CFR_SEL_NONE = 0,
    CFR_SEL_CHAR, /* click-drag */
    CFR_SEL_WORD, /* double-click */
    CFR_SEL_LINE, /* triple-click */
} CfrSelectionMode;

typedef struct
{
    int row; /* unified: >=0 visible, <0 scrollback */
    int col;
} CfrSelectionPoint;

typedef struct
{
    bool active;
    CfrSelectionMode mode;
    CfrSelectionPoint anchor; /* original click point */
    CfrSelectionPoint start;  /* normalized (always <= end) */
    CfrSelectionPoint end;    /* normalized (always >= end) */
} CfrSelection;

/* Configure the word character set for CFR_SEL_WORD mode.
 * NULL or empty resets to the default: a-zA-Z0-9_-./~ */
void cfr_selection_set_word_chars(CfrTerm *vt, const char *chars);

/* Start a selection at (row, col) in the given mode. For WORD and
 * LINE modes, the selection is expanded immediately. Fires
 * selection_changed(true) if the selection becomes active. */
void cfr_selection_start(CfrTerm *vt, int row, int col,
                         CfrSelectionMode mode);

/* Update the selection endpoint to (row, col). Re-normalizes start/end
 * relative to the anchor. Does NOT fire selection_changed. */
void cfr_selection_update(CfrTerm *vt, int row, int col);

/* Extend the selection to (row, col) from the nearer endpoint.
 * Used for shift+click. Does NOT fire selection_changed. */
void cfr_selection_extend(CfrTerm *vt, int row, int col);

/* Clear the selection. Fires selection_changed(false) if it was active.
 * No-op if already inactive. */
void cfr_selection_clear(CfrTerm *vt);

/* Query whether a selection is active. */
bool cfr_selection_active(const CfrTerm *vt);

/* Check if (unified_row, col) falls within the active selection. */
bool cfr_selection_in_cell(const CfrTerm *vt, int unified_row, int col);

/* Get the current selection state (read-only pointer, valid until the
 * next selection operation). NULL if no selection is active. */
const CfrSelection *cfr_selection_get(const CfrTerm *vt);

/* Extract the selected text as UTF-8. Returns a malloc'd,
 * NUL-terminated string the caller must free. NULL if no selection
 * or allocation failure. */
char *cfr_selection_get_text(const CfrTerm *vt);

#ifdef __cplusplus
}
#endif

#endif /* COFFER_H */
