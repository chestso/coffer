/*
 * coffer internal types — not part of the public API.
 *
 * Defines the CfrTerm struct, the page layout, the style intern table,
 * and the grapheme arena. These exist in this header (rather than each
 * .c file) so that all internal translation units share the layout
 * without exposing it through coffer.h.
 */

#ifndef COFFER_INTERNAL_H
#define COFFER_INTERNAL_H

#include <coffer/coffer.h>

#include <stdio.h>
#include <stdlib.h>

/*
 * CFR_BUG_CHECK — invariant assertion that aborts on violation.
 *
 * These guard documented invariants at three audit-identified fragile
 * sites in the engine (style intern after realloc, scrollback push
 * source page, reflow distribute phase). Each is a "should never fire
 * in a correct caller" condition — a fire means a bug has corrupted
 * state. Crash so the cause can be diagnosed, instead of letting
 * corruption propagate.
 *
 * Always-on: not gated on a debug switch, because the cost is one
 * branch in non-hot paths and the benefit is fail-fast on bugs that
 * would otherwise scribble heap minutes later.
 */
#define CFR_BUG_CHECK(cond, ...)                                     \
    do {                                                             \
        if (__builtin_expect(!(cond), 0)) {                          \
            fprintf(stderr, "COFFER BUG: " __VA_ARGS__);             \
            fprintf(stderr, " at %s:%d (!%s)\n", __FILE__, __LINE__, \
                    #cond);                                          \
            fflush(stderr);                                          \
            abort();                                                 \
        }                                                            \
    } while (0)

/* ------------------------------------------------------------------ */
/* Tunables                                                            */
/* ------------------------------------------------------------------ */

#define CFR_PAGE_BYTES         (64u * 1024u)
#define CFR_OSC_BUF_BYTES      65536u
#define CFR_CSI_PARAM_MAX      32u
#define CFR_INTERMEDIATE_MAX   4u
#define CFR_DEFAULT_SCROLLBACK 1000
#define CFR_SB_PAGE_ROWS       64u /* rows per scrollback page */

/* Grapheme arena: codepoints / dedup table initial sizes (bytes). */
#define CFR_ARENA_CP_INIT    1024u
#define CFR_ARENA_DEDUP_INIT 64u
#define CFR_STYLES_INIT      16u

/* ------------------------------------------------------------------ */
/* Style intern table                                                  */
/* ------------------------------------------------------------------ */

typedef struct
{
    CfrStyle *entries; /* index 0 reserved for the default style */
    uint32_t count;
    uint32_t capacity; /* power of two */
    uint32_t *index;   /* open-addressed: hash slot -> entries[] index */
    uint32_t index_capacity;
} CfrStyleTable;

/* ------------------------------------------------------------------ */
/* Grapheme arena                                                      */
/* ------------------------------------------------------------------ */

/* Each entry is laid out in `codepoints` as:
 *   [u32 hash][u32 length][u32 cps[length]]
 * The grapheme_id stored on the cell is the offset of the first u32
 * (the hash word). offset 0 is reserved (single-cp cell sentinel).
 */
typedef struct
{
    uint32_t *codepoints;
    uint32_t used;
    uint32_t capacity;
    uint32_t *dedup_index;   /* open-addressed: hash slot -> arena offset */
    uint32_t dedup_capacity; /* power of two */
    uint32_t dedup_count;    /* live entries in dedup_index */
} CfrGraphemeArena;

/* ------------------------------------------------------------------ */
/* Hyperlink intern table (OSC 8)                                       */
/* ------------------------------------------------------------------ */

/* URIs are interned per page. Cells reference an entry by uint16_t id;
 * id 0 means "no link". `data` is a bump-allocated byte arena; offsets[id]
 * and lengths[id] locate each entry. dedup_index hashes URI bytes (FNV-1a)
 * to an id so identical URIs share a slot — this gives a renderer free
 * run-continuity (OSC 8 spec's primary use case for the `id=` parameter).
 *
 * Slot value 0 in dedup_index means "empty"; non-zero values are ids.
 */
typedef struct
{
    uint8_t *data;
    uint32_t used;
    uint32_t capacity;
    uint32_t *offsets;     /* [id] -> offset into data */
    uint32_t *lengths;     /* [id] -> URI byte length */
    uint16_t count;        /* number of interned URIs (id 1..count) */
    uint16_t capacity_ids; /* offsets/lengths capacity */
    uint16_t *dedup_index; /* open-addressed hash slot -> id, 0 = empty */
    uint32_t dedup_capacity;
} CfrHyperlinkTable;

/* ------------------------------------------------------------------ */
/* Page                                                                */
/* ------------------------------------------------------------------ */

typedef struct CfrPage
{
    struct CfrPage *prev, *next;
    uint16_t cols;
    uint16_t row_count;
    uint16_t row_capacity;
    uint16_t _pad;

    CfrCell *cells;     /* row_capacity * cols */
    uint8_t *row_flags; /* per-row flags (currently: WRAPLINE on last cell of row) */

    CfrStyleTable styles;
    CfrGraphemeArena graphemes;
    CfrHyperlinkTable hyperlinks;

    /* No flexible array yet — initial scaffold uses sub-allocations.
     * The plan calls for a single backing buffer; we'll consolidate
     * once the grid is implemented. */
} CfrPage;

/* ------------------------------------------------------------------ */
/* Parser state                                                        */
/* ------------------------------------------------------------------ */

typedef enum
{
    CFR_STATE_GROUND = 0,
    CFR_STATE_ESCAPE,
    CFR_STATE_ESCAPE_INTERMEDIATE,
    CFR_STATE_CSI_ENTRY,
    CFR_STATE_CSI_PARAM,
    CFR_STATE_CSI_INTERMEDIATE,
    CFR_STATE_CSI_IGNORE,
    CFR_STATE_DCS_ENTRY,
    CFR_STATE_DCS_PARAM,
    CFR_STATE_DCS_INTERMEDIATE,
    CFR_STATE_DCS_PASSTHROUGH,
    CFR_STATE_DCS_IGNORE,
    CFR_STATE_OSC_STRING,
    CFR_STATE_SOS_PM_STRING,
    CFR_STATE_APC_STRING,
} CfrParserState;

typedef struct
{
    CfrParserState state;
    uint32_t params[CFR_CSI_PARAM_MAX];
    /* Bit i set ⇒ params[i] was introduced by ':' (a subparam of the
     * preceding param), not by ';'. Used to distinguish e.g. SGR 4:3
     * (curly underline) from 4;3 (underline + italic), and to detect the
     * empty colourspace slot in 38:2::R:G:B. */
    uint32_t param_is_subparam;
    uint8_t param_count;
    bool param_present; /* whether the current slot has a digit */
    uint8_t intermediates[CFR_INTERMEDIATE_MAX];
    uint8_t intermediate_count;
    /* UTF-8 decoder state (Bjoern Hoehrmann) */
    uint32_t utf8_state;
    uint32_t utf8_codepoint;
    /* OSC accumulator */
    uint8_t osc_buf[CFR_OSC_BUF_BYTES];
    uint16_t osc_len;
    bool osc_truncated;
    /* APC accumulator */
    uint8_t apc_buf[CFR_OSC_BUF_BYTES];
    uint16_t apc_len;
    bool apc_truncated;
    /* DCS streaming state — passthrough emits chunks via callback */
    bool dcs_initial_sent;
    bool dcs_is_sixel;                           /* current DCS is a sixel (final byte 'q') — handled internally */
    uint8_t dcs_intro[CFR_INTERMEDIATE_MAX + 4]; /* params + final */
    uint8_t dcs_intro_len;
} CfrParser;

/* ------------------------------------------------------------------ */
/* Cursor / pen state                                                  */
/* ------------------------------------------------------------------ */

#define CFR_CLUSTER_MAX 16 /* max codepoints in a single grapheme cluster */

typedef struct
{
    int row, col;
    bool visible;
    bool blink;
    bool pending_wrap;     /* "deferred wrap" — DEC behavior at right margin */
    CfrStyle pen;          /* current SGR pen */
    uint16_t hyperlink_id; /* OSC 8 active link, 0 = none */
    /* Pending grapheme cluster — codepoints accumulated since the last
     * boundary. Committed as a single cell on the next break or on any
     * forced flush (CSI dispatch, C0 control, etc.). */
    uint32_t cluster_buf[CFR_CLUSTER_MAX];
    uint8_t cluster_len;
} CfrCursorState;

/* ------------------------------------------------------------------ */
/* The terminal                                                        */
/* ------------------------------------------------------------------ */

struct CfrTerm
{
    int rows;
    int cols;

    /* Active grid (visible). For now, exactly one page; later may chain. */
    CfrPage *grid;

    /* Alternate screen — saved when in altscreen. */
    CfrPage *altgrid;
    bool in_altscreen;

    /* Scrollback page ring (head = most recent). */
    CfrPage *sb_head;
    CfrPage *sb_tail;
    int sb_lines;
    int sb_capacity;

    CfrCursorState cursor;
    /* One saved-cursor register per screen: [0]=normal, [1]=alt. DECSC/DECRC
     * and the ANSI.SYS CSI s/u forms save/restore the *active* screen's
     * register, matching xterm — so a TUI's DECSC inside the alt screen can't
     * clobber the cursor that altscreen 1049 stashed for the normal screen. */
    CfrCursorState saved_cursor[2];

    /* Scroll region (DECSTBM). Inclusive. */
    int scroll_top;
    int scroll_bottom;

    /* Tab stops. Bit per column. */
    uint8_t *tabstops;

    CfrParser parser;
    CfrCallbacks callbacks;
    void *callback_user;
    CfrAllocator alloc;

    /* Modes. Indexed by CfrMode enum value. */
    bool modes[32];

    /* Character set designations. ESC ( c | ) c | * c | + c stores the
     * intermediate `c` here. 'B' = ASCII (default), '0' = DEC special
     * graphics. `charset_active` selects which slot GL maps to: 0 (G0)
     * via SI / ESC n, 1 (G1) via SO / ESC o. We honor only G0/G1
     * because that's all anything in the wild uses. */
    uint8_t charset[4];
    uint8_t charset_active;

    /* Settings. */
    bool reflow_enabled;
    bool ambiguous_wide;
    /* Cursor key application mode (DECCKM). When true, arrow keys
     * emit ESC O <X> instead of ESC [ <X>. */
    bool decckm;
    /* Keypad application mode (DECKPAM). */
    bool deckpam;
    /* Origin mode (DECOM). When true, CUP/HVP/VPA coordinates are
     * relative to the active scroll region instead of the full screen,
     * and the cursor is confined to the scroll region. */
    bool decom;
    /* Insert/Replace Mode (IRM, CSI 4h/4l). When on, printing a character
     * inserts it at the cursor position, shifting existing cells right;
     * cells shifted past the right margin are lost. Default off. */
    bool insert_mode;

    /* Kitty keyboard protocol flag stack. Index 0 is always the active
     * baseline (zero = protocol off, identical to legacy behaviour);
     * pushes increment depth, pops decrement it. The stack is bounded:
     * pushing past depth 15 silently overwrites the top, matching
     * kitty's own implementation. The currently active flag mask is
     * always kitty_kb_stack[kitty_kb_depth]. We honour bits 0x1
     * (Disambiguate escape codes) and 0x8 (Report all keys as escape
     * codes); the other documented flags (0x2 event types, 0x4 alt
     * keys, 0x10 associated text) are accepted-and-stored but do not
     * yet affect emit behaviour — see FOLLOWUPS.md. */
    uint32_t kitty_kb_stack[16];
    uint8_t kitty_kb_depth;

    /* Title — owned, NUL-terminated. */
    char *title;

    /* Damage accumulator (rectangular union since last clear). */
    CfrRect damage;
    bool damage_dirty;

    /* Cursor position/visibility at the last cfr_damage_flush. A cursor-only
     * move (CUP, arrows) changes no grid cell and so emits no damage; flush
     * folds it in by dirtying the old and new cursor cells. */
    int dmg_cursor_row;
    int dmg_cursor_col;
    bool dmg_cursor_visible;

    /* Sixel graphics (sixel.c). The state is allocated lazily on the
     * first sixel DCS. `sixel_abs_top` is the absolute line index of grid
     * row 0 — it advances by one each time a line scrolls off the top
     * into history, so an image stored with an absolute anchor line
     * tracks the text it sits on with no per-image bookkeeping. */
    struct CfrSixelState *sixel;
    int cell_w_px; /* px per cell, set at creation */
    int cell_h_px;
    float content_scale; /* DPI scale factor (1.0 = unscaled), set by host */
    long sixel_abs_top;

    /* Lottie animations (lottie.c). Lazily allocated on first APC.
     * Shares sixel_abs_top for absolute-line anchoring. */
    struct CfrLottieState *lottie;
};

/* The saved-cursor register for the currently active screen. DECSC/DECRC and
 * CSI s/u operate on this; altscreen 1049 always uses saved_cursor[0]. */
static inline CfrCursorState *cfr_active_saved_cursor(CfrTerm *vt)
{
    return &vt->saved_cursor[vt->in_altscreen ? 1 : 0];
}

/* Restore the cursor from a saved register (DECRC, CSI u, altscreen exit).
 * Cursor visibility (DECTCEM, ?25) and blink (AT&T, ?12) are independent DEC
 * modes, not part of the DECSC/DECRC save register — so they survive the
 * restore. Otherwise a TUI's final `?25h` would be clobbered by the cursor
 * register saved while the cursor was hidden, leaving it stuck invisible. */
static inline void cfr_cursor_restore(CfrTerm *vt, const CfrCursorState *src)
{
    bool visible = vt->cursor.visible;
    bool blink = vt->cursor.blink;
    vt->cursor = *src;
    vt->cursor.visible = visible;
    vt->cursor.blink = blink;
}

/* ------------------------------------------------------------------ */
/* Internal helpers (cross-file)                                       */
/* ------------------------------------------------------------------ */

/* Allocator helpers — route through vt->alloc. */
void *cfr_alloc(CfrTerm *vt, size_t size);
void *cfr_realloc(CfrTerm *vt, void *ptr, size_t size);
void cfr_dealloc(CfrTerm *vt, void *ptr);

/* Page lifecycle. */
CfrPage *cfr_page_new(CfrTerm *vt, int rows, int cols);
void cfr_page_free(CfrTerm *vt, CfrPage *page);

/* Style intern. Returns 0 for the default style. */
uint32_t cfr_style_intern(CfrTerm *vt, CfrPage *page, const CfrStyle *style);
const CfrStyle *cfr_style_lookup(const CfrPage *page, uint32_t id);

/* Grapheme arena. */
uint32_t cfr_grapheme_intern(CfrTerm *vt, CfrPage *page,
                             const uint32_t *cps, uint32_t len);
size_t cfr_grapheme_read(const CfrPage *page, uint32_t id,
                         uint32_t *out, size_t out_cap);

/* Hyperlink intern (OSC 8). Returns id (1..UINT16_MAX), or 0 on
 * allocation failure / overflow / empty URI. */
uint16_t cfr_hyperlink_intern(CfrTerm *vt, CfrPage *page,
                              const uint8_t *uri, uint32_t uri_len);
size_t cfr_hyperlink_read(const CfrPage *page, uint16_t id,
                          uint8_t *out, size_t out_cap);
void cfr_hyperlink_free(CfrTerm *vt, CfrHyperlinkTable *t);

/* Parser entry. */
void cfr_parser_init(CfrParser *p);
void cfr_parser_feed(CfrTerm *vt, const uint8_t *bytes, size_t len);

/* Width helpers (width.c). */
int cfr_codepoint_width(CfrTerm *vt, uint32_t cp);
int cfr_cluster_width(CfrTerm *vt, const uint32_t *cps, uint32_t len);

/* Grid mutators (print.c). */
void cfr_grid_ensure(CfrTerm *vt);
void cfr_print_codepoint(CfrTerm *vt, uint32_t cp);
void cfr_flush_cluster(CfrTerm *vt);
void cfr_execute_c0(CfrTerm *vt, uint8_t b);
void cfr_scroll_up(CfrTerm *vt, int lines);
void cfr_scroll_down(CfrTerm *vt, int lines);
void cfr_erase_in_line(CfrTerm *vt, int mode);
void cfr_erase_in_display(CfrTerm *vt, int mode);

/* Width / grapheme break (width.c). */
bool cfr_grapheme_break_before(uint32_t prev, uint32_t cur, void *state);

/* Palette (palette.c) — resolves 0..255 indexed colors to 0x00RRGGBB. */
uint32_t cfr_palette_lookup(CfrTerm *vt, uint8_t idx);

/* Output emit (keys.c). */
void cfr_emit_bytes(CfrTerm *vt, const uint8_t *bytes, size_t len);

/* Altscreen (modes.c). */
void cfr_set_altscreen(CfrTerm *vt, bool on, bool save_restore_cursor);

/* RIS — full reset (modes.c). Restores the terminal to its initial
 * state: clears the kitty keyboard stack, every DEC private mode,
 * DECCKM/DECKPAM/DECOM, charset designations, saved cursor, scroll
 * region, tab stops, and the grid. Notifies the host via set_mode for
 * any mode that turned off, so mouse/paste/etc. observers stay in
 * sync. */
void cfr_full_reset(CfrTerm *vt);

/* Reflow (reflow.c). When reflow_enabled is true cfr_reflow walks the
 * grid's logical lines (rows linked by WRAPLINE) and re-wraps them at
 * the new geometry, pushing overflow into scrollback. When disabled
 * (or in altscreen) it falls through to a clamp-only resize. */
void cfr_reflow(CfrTerm *vt, int new_rows, int new_cols);
void cfr_resize_clamp(CfrTerm *vt, int new_rows, int new_cols);

/* Grid edits (print.c). */
void cfr_insert_chars(CfrTerm *vt, int count);
void cfr_delete_chars(CfrTerm *vt, int count);
void cfr_insert_lines(CfrTerm *vt, int count);
void cfr_delete_lines(CfrTerm *vt, int count);
void cfr_erase_chars(CfrTerm *vt, int count);

/* Dispatchers. */
void cfr_csi_dispatch(CfrTerm *vt, uint8_t final);
void cfr_esc_dispatch(CfrTerm *vt, uint8_t final);
void cfr_osc_dispatch(CfrTerm *vt, const uint8_t *data, size_t len);
void cfr_dcs_hook(CfrTerm *vt, uint8_t final);
void cfr_dcs_put(CfrTerm *vt, uint8_t b);
void cfr_dcs_unhook(CfrTerm *vt);

/* Damage helper. cfr_damage_flush() is public (declared in coffer.h). */
void cfr_damage_cell(CfrTerm *vt, int row, int col);
void cfr_damage_row(CfrTerm *vt, int row);
void cfr_damage_all(CfrTerm *vt);

/* Scrollback (scrollback.c). */
void cfr_scrollback_push(CfrTerm *vt, const CfrCell *src_cells, int cols, bool wrapline);
void cfr_scrollback_clear(CfrTerm *vt);

/* Sixel graphics (sixel.c). The state hangs off vt->sixel, allocated
 * lazily. All entry points are no-ops when no sixel has been seen. */
void cfr_sixel_state_free(CfrTerm *vt);
/* DCS lifecycle, driven from dcs.c when the final byte is 'q'. */
void cfr_sixel_begin(CfrTerm *vt, const uint32_t *params, int nparams);
void cfr_sixel_put(CfrTerm *vt, const uint8_t *data, size_t len);
void cfr_sixel_finish(CfrTerm *vt);
/* Grid maintenance. note_scroll is called after sixel_abs_top advances by
 * `lines`; it culls images that have scrolled out of retained scrollback.
 * clear_display_rows removes foreground images overlapping the inclusive
 * display-row range [top,bot]; clear_all removes everything. */
void cfr_sixel_note_scroll(CfrTerm *vt, int lines);
void cfr_sixel_clear_display_rows(CfrTerm *vt, int top, int bot);
void cfr_sixel_clear_all(CfrTerm *vt);

/* Lottie animations (lottie.c). Lazily allocated, same pattern as sixel.
 * All entry points are no-ops when no animation has been loaded. */
void cfr_lottie_state_free(CfrTerm *vt);
void cfr_lottie_apc_dispatch(CfrTerm *vt, const uint8_t *body, size_t body_len);
void cfr_lottie_note_scroll(CfrTerm *vt, int lines);
void cfr_lottie_clear_display_rows(CfrTerm *vt, int top, int bot);
void cfr_lottie_clear_all(CfrTerm *vt);

/* Page ownership lookup — used by cell accessors that must resolve
 * a cell's grapheme/style entry against the page that owns it. */
const CfrPage *cfr_find_owner_page(const CfrTerm *vt, const CfrCell *cell);

#endif /* COFFER_INTERNAL_H */
