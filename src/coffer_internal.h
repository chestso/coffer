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
#include "image_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

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
#define CFR_OSC_BUF_INIT       65536u                 /* initial OSC/APC buffer; grows on demand */
#define CFR_OSC_BUF_MAX        (256u * 1024u * 1024u) /* hard cap (sanity) */
#define CFR_CSI_PARAM_MAX      32u
#define CFR_INTERMEDIATE_MAX   4u
#define CFR_DEFAULT_SCROLLBACK 1000
/* Default byte budget for retained scrollback cell storage (see
 * CfrConfig.scrollback_bytes). Large enough for the default 1000 lines at
 * any sane width, small enough that a huge line count on a wide grid
 * cannot turn into gigabytes. */
#define CFR_DEFAULT_SCROLLBACK_BYTES (64ull * 1024ull * 1024ull)
#define CFR_SB_PAGE_ROWS             64u /* rows per scrollback page */

/* Bounded work. What an operation costs is the grid plus a named constant
 * below, or something linear in what the caller asked for (the size of a
 * selection, a search of the whole buffer) — never the amount of history
 * or the length of the line it touches. Scrollback can hold 100000 rows
 * or more and a single line can be megabytes, so a scan or rewrite that
 * follows either needs a constant here, not a loop to the end. */

/* Most scrollback rows of the line straddling the grid boundary that one
 * reflow pulls back and rewraps. Reflow cost must not depend on how much
 * history exists or how long a line is (`cat` of a multi-megabyte line
 * with a huge ring), so the rewrite stops here; older rows of that line
 * keep the width they were written at and still join (see reflow.c). */
#define CFR_REFLOW_PREFIX_ROWS 1024
/* Most cells a double-click scans either way from the click to find the
 * word's edges (selection.c). Same reasoning: the work a click costs must
 * not depend on how long the line under it is. */
#define CFR_WORD_SCAN_CELLS 4096

/* Live-grid table collection (grid.c, cfr_grid_gc). The style, grapheme and
 * hyperlink tables of the live grid page only grow, so once one holds more
 * than max(CFR_GRID_GC_MIN_ENTRIES, 2 * cells) entries the page is rebuilt
 * with just the entries its cells still use. A screen has at most `cells`
 * live entries, so a rebuild costs one screen and is paid for by at least
 * as many new entries; the link limit is also held under
 * CFR_GRID_GC_LINKS_MAX so the 65535-id cap is never reached between two
 * checks (a check runs every CFR_GRID_GC_SLICE_BYTES of input, which can
 * intern at most that many links). */
#define CFR_GRID_GC_MIN_ENTRIES 4096u
#define CFR_GRID_GC_LINKS_MAX   32768u
#define CFR_GRID_GC_SLICE_BYTES 16384u

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

/* Links are interned per page. Cells reference an entry by uint16_t id;
 * id 0 means "no link". `data` is a bump-allocated byte arena; offsets[id]
 * and lengths[id] locate each entry's URI bytes. keys[id] is the entry's
 * 64-bit content key (FNV-1a of the URI and, when present, the OSC 8
 * `id=` value; never 0): the one identity that means the same thing on
 * every page, hashed once at intern time and reused for dedup, table
 * growth and cross-page transfer. dedup_index maps a key to an id so
 * identical links share a slot — this gives a renderer free
 * run-continuity, while the same URI under different ids stays distinct.
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
    uint64_t *keys;        /* [id] -> content key, never 0 */
    uint16_t count;        /* number of interned links (id 1..count) */
    uint16_t capacity_ids; /* offsets/lengths/keys capacity */
    uint16_t *dedup_index; /* open-addressed hash slot -> id, 0 = empty */
    uint32_t dedup_capacity;
} CfrHyperlinkTable;

/* ------------------------------------------------------------------ */
/* Page                                                                */
/* ------------------------------------------------------------------ */

typedef struct CfrPage
{
    uint16_t cols;
    uint16_t row_count;
    uint16_t row_capacity;
    uint16_t _pad;
    uint64_t serial; /* unique per page for the term's lifetime; 0 = none */

    CfrCell *cells;    /* row_capacity * cols */
    uint32_t *lineage; /* per-row logical-line id; 0 = blank/unwritten */

    /* `cells` and `lineage` are a trailing flexible array off the
     * `CfrPage` header, allocated in one block by cfr_page_new and
     * freed with the header in cfr_page_free. The tables below are
     * kept as separate sub-allocations because they grow across the
     * page's lifetime; folding them into the trailing region would
     * force over-allocation up-front or relocate the whole page on
     * growth, invalidating every holding `CfrCell *`. */
    CfrStyleTable styles;
    CfrGraphemeArena graphemes;
    CfrHyperlinkTable hyperlinks;
} CfrPage;

/* One scrollback row: the page that stores it and its row index there.
 * The scrollback is a ring of these (scrollback.c). Rows of one page are
 * contiguous in the ring, so a page is freed when no neighbour shares it. */
typedef struct
{
    CfrPage *page;
    uint32_t row;
} CfrSbRow;

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
    /* The terminal's allocator: the OSC/APC accumulators grow through
     * it, so a host-installed allocator (e.g. a hardened wrapper) sees
     * every byte the parser owns and the matching free in cfr_free()
     * stays balanced. */
    CfrAllocator alloc;
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
    /* OSC accumulator (grows for large inline-image payloads) */
    uint8_t *osc_buf;
    uint32_t osc_len;
    uint32_t osc_cap;
    bool osc_truncated;
    /* APC accumulator (kitty graphics / lottie — grows similarly) */
    uint8_t *apc_buf;
    uint32_t apc_len;
    uint32_t apc_cap;
    bool apc_truncated;
    /* DCS streaming state — passthrough emits chunks via callback */
    bool dcs_initial_sent;
    bool dcs_is_sixel;                           /* current DCS is a sixel (final byte 'q') — handled internally */
    uint8_t dcs_intro[CFR_INTERMEDIATE_MAX + 4]; /* params + final */
    uint8_t dcs_intro_len;
} CfrParser;

/* ------------------------------------------------------------------ */
/* Image upload accumulators                                           */
/* ------------------------------------------------------------------ */

/* Kitty graphics chunked-upload state (graphics.c). A chunked transfer
 * (m=1 … m=0) carries its control data on the FIRST chunk only, so the
 * accumulator stashes that chunk's key=value string and re-parses it
 * when the final chunk arrives. Lives in the terminal state: two
 * terminals in one process upload independently, and a half-finished
 * upload dies with its terminal. */
typedef struct
{
    char *b64; /* accumulated base64 payload */
    size_t len;
    size_t cap;
    int active; /* a chunked upload is in flight */
    char *ctrl; /* first chunk's control data, re-parsed at finalize */
    size_t ctrl_len;
    size_t ctrl_cap;
} CfrKChunk;

/* iTerm2 OSC 1337 upload parameters (osc_1337.c). */
typedef struct
{
    int inline_display;
    int width;  /* display width; 0 = auto */
    int height; /* display height; 0 = auto */
    int width_is_px;
    int height_is_px;
    int preserve_aspect;
    int has_width;
    int has_height;
} CfrItermParams;

/* iTerm2 OSC 1337 multipart-upload state (osc_1337.c). The same
 * per-terminal reasoning as CfrKChunk. */
typedef struct
{
    CfrItermParams params;
    uint8_t *b64_buf;
    size_t b64_len;
    size_t b64_cap;
    int active;
} CfrItermMultipart;

/* ------------------------------------------------------------------ */
/* Cursor / pen state                                                  */
/* ------------------------------------------------------------------ */

#define CFR_CLUSTER_MAX 16 /* max codepoints in a single grapheme cluster */

/* The OSC 8 link the pen is inside. A page-scoped hyperlink id is useless
 * here: the pen outlives the page it was opened on (reflow replaces the
 * grid, altscreen swaps it, DECSC saves it for later), so the pen holds
 * the URI itself — an immutable, reference-counted block shared by the
 * cursor and its saved registers — and interns it into whichever page it
 * is printing on. `key` is the link's identity: the URI with the OSC 8
 * `id=` parameter folded in when one was given (see hyperlink.c), so the
 * id bytes do not need to stay alive. `cache_*` remembers the id on the
 * last page it was interned into, so a print run costs one compare per
 * cell, not a URI lookup. Ids are append-only within a page and `serial`
 * is never reused, so a cache hit is always right. */
typedef struct CfrLink
{
    uint32_t refs;
    uint32_t len;
    uint64_t key;          /* content key, as in CfrHyperlinkTable */
    uint64_t cache_serial; /* CfrPage.serial of the cached page, 0 = none */
    uint16_t cache_id;
    uint8_t uri[]; /* len bytes, not NUL-terminated */
} CfrLink;

typedef struct
{
    int row, col; /* col ∈ [0..cols]; == cols is the deferred-wrap phantom */
    bool visible;
    bool blink;
    CfrCursorShape shape; /* DECSCUSR cursor style */
    CfrStyle pen;         /* current SGR pen */
    CfrLink *link;        /* OSC 8 active link, NULL = none */
    /* Pending grapheme cluster — codepoints accumulated since the last
     * boundary. Committed as a single cell on the next break or on any
     * forced flush (CSI dispatch, C0 control, etc.). */
    uint32_t cluster_buf[CFR_CLUSTER_MAX];
    uint8_t cluster_len;
} CfrCursorState;

/* ------------------------------------------------------------------ */
/* The terminal                                                        */
/* ------------------------------------------------------------------ */

/* Dynamic color slots (xterm OSC 10/11/12): default foreground,
 * default background, cursor color. */
enum
{
    CFR_DYN_FG = 0,
    CFR_DYN_BG = 1,
    CFR_DYN_CURSOR = 2,
    CFR_DYN_COUNT = 3,
};

struct CfrTerm
{
    int rows;
    int cols;

    /* Active grid (visible). For now, exactly one page; later may chain. */
    CfrPage *grid;

    /* Alternate screen — saved when in altscreen. */
    CfrPage *altgrid;
    bool in_altscreen;

    /* Scrollback (scrollback.c): a ring of row records, oldest at
     * sb_first, over pages that own the cells. Row `d` deep (0 = newest) is
     * a direct index — no walking. sb_ring_cap is zero or a power of two. */
    CfrSbRow *sb_rows;
    uint32_t sb_ring_cap;
    uint32_t sb_first;
    int sb_lines;    /* rows held */
    int sb_capacity; /* most rows kept; 0 = scrollback off */
    /* Retained cell storage: cols * sizeof(CfrCell) per held row. Kept in
     * step with the ring (push adds, evict/pop subtract, clear zeroes) so
     * eviction can enforce sb_bytes_cap without walking history. The cap
     * is 0 when disabled. */
    uint64_t sb_bytes;
    uint64_t sb_bytes_cap;

    CfrCursorState cursor;
    /* One saved-cursor register per screen: [0]=normal, [1]=alt. DECSC/DECRC
     * and the ANSI.SYS CSI s/u forms save/restore the *active* screen's
     * register, matching xterm — so a TUI's DECSC inside the alt screen can't
     * clobber the cursor that altscreen 1049 stashed for the normal screen. */
    CfrCursorState saved_cursor[2];
    uint64_t page_serial; /* last CfrPage.serial handed out */

    /* Initial cursor style — DECSCUSR `CSI 7 SP q` and RIS restore it
     * (xterm: the cursorBlink / cursorUnderLine / cursorBar resources). */
    CfrCursorShape default_shape;
    bool default_blink;

    /* Dynamic colors (xterm OSC 10/11/12 family), indexed by
     * CFR_DYN_* below. `dyn_color` is the live value; `dyn_color_seed`
     * is the configured default it resets to (OSC 110/111/112, RIS). */
    CfrColorValue dyn_color[CFR_DYN_COUNT];
    CfrColorValue dyn_color_seed[CFR_DYN_COUNT];

    /* Live 256-color palette: the built-ins, with any OSC 4 overrides
     * applied. cfr_palette_init() seeds it at cfr_new(); OSC 104 and RIS
     * recompute the built-in values. Styles store palette indices, so a
     * change here recolors already-written text on the next repaint. */
    uint32_t palette[256];

    /* Scroll region (DECSTBM). Inclusive. */
    int scroll_top;
    int scroll_bottom;

    /* Tab stops. Bit per column. */
    uint8_t *tabstops;

    CfrParser parser;
    CfrCallbacks callbacks;
    void *callback_user;
    CfrAllocator alloc;
    uint64_t alloc_failures; /* allocations that returned NULL, ever */

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
    /* xterm modifyOtherKeys level (CSI > 4 ; m). 0 = off (default),
     * 1 = emit the xterm tilde form (CSI 27 ; mod ; code ~) for
     * modified keys that have no traditional control encoding (Enter,
     * Tab, Backspace, Escape, and Ctrl+non-letter symbols). Level 1
     * leaves Ctrl+ASCII alone so Ctrl+C still produces 0x03/SIGINT.
     * The kitty keyboard protocol takes precedence when active. */
    uint8_t modify_other_keys;
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
    /* Shared image store — owns all image records (sixel, iTerm2, kitty).
     * Lazily initialized alongside the sixel state (which owns the
     * DCS decode canvas + palette). */
    CfrImgStore *images;

    struct CfrSixelState *sixel;
    int cell_w_px; /* px per cell, set at creation */
    int cell_h_px;
    float content_scale; /* DPI scale (1.0 = unscaled); ReportCellSize only */
    long sixel_abs_top;

    /* Lottie animations (lottie.c). Lazily allocated on first APC.
     * Shares sixel_abs_top for absolute-line anchoring. */
    struct CfrLottieState *lottie;

    /* Chunked-upload accumulators (kitty graphics, iTerm2 multipart).
     * Per-terminal: two terminals in one process upload independently,
     * and a half-finished upload dies with its terminal. */
    CfrKChunk kchunk;
    CfrItermMultipart iterm_mp;

    /* Last printed codepoint for REP (CSI Ps b). 0 = nothing printed yet. */
    uint32_t last_char;

    /* Lead cell of the last print, for stray-joiner re-attachment across
     * write boundaries (print.c, spec §3.4). Kept as row/column rather
     * than a pointer: cfr_grid_gc() may replace the page between input
     * bytes. -1, -1 = no print in the current grid. */
    int last_print_row;
    int last_print_col;

    /* Monotonic logical-line id source (lines.c). Wraps at 2^32, skipping
     * 0 — the blank/unwritten sentinel. */
    uint32_t next_lineage;

    /* One-time log guards. See should_log_once() above. */
    uint32_t logged_once;

    /* Selection state. Owned by the engine, adjusted inline during
     * scroll and cleared on draw/erase/resize/altscreen. */
    CfrSelection selection;
    char *selection_word_chars;

    /* Terminal name for XTVERSION response (CSI > q). Borrowed from
     * CfrConfig.terminal_name at cfr_new time; NULL = "coffer". */
    const char *terminal_name;
};

/* Copy a cursor state (DECSC, DECRC, altscreen save): a plain struct copy
 * plus the link's reference count. Never assign a CfrCursorState directly. */
void cfr_cursor_assign(CfrTerm *vt, CfrCursorState *dst,
                       const CfrCursorState *src);

/* The saved-cursor register for the currently active screen. DECSC/DECRC and
 * CSI s/u operate on this; altscreen 1049 always uses saved_cursor[0]. */
static inline CfrCursorState *cfr_active_saved_cursor(CfrTerm *vt)
{
    return &vt->saved_cursor[vt->in_altscreen ? 1 : 0];
}

/* Restore the cursor from a saved register (DECRC, CSI u). Cursor
 * visibility (DECTCEM, ?25), blink (AT&T, ?12) and shape (DECSCUSR)
 * are independent mode-ish attributes, not part of the DECSC/DECRC
 * save register — so they survive the restore. Otherwise a TUI's
 * final `?25h` would be clobbered by the cursor register saved while
 * the cursor was hidden, leaving it stuck invisible. Altscreen 1049
 * wants the opposite (a full pre-altscreen restore) and uses
 * cfr_cursor_restore_full() (modes.c). */
static inline void cfr_cursor_restore(CfrTerm *vt, const CfrCursorState *src)
{
    bool visible = vt->cursor.visible;
    bool blink = vt->cursor.blink;
    CfrCursorShape shape = vt->cursor.shape;
    cfr_cursor_assign(vt, &vt->cursor, src);
    vt->cursor.visible = visible;
    vt->cursor.blink = blink;
    vt->cursor.shape = shape;
}

/* ------------------------------------------------------------------ */
/* Logging                                                             */
/* ------------------------------------------------------------------ */

/* One-time log guards for unimplemented sequences. Bit set = already
 * logged; subsequent occurrences are silently dropped. Reset on
 * cfr_reset() so a new session gets fresh warnings. */
#define CFR_LOGGED_MEDIA_COPY (1u << 0)
#define CFR_LOGGED_MEML       (1u << 1)
#define CFR_LOGGED_MEMU       (1u << 2)
#define CFR_LOGGED_LR_MARGIN  (1u << 3)
#define CFR_LOGGED_META       (1u << 4)
#define CFR_LOGGED_BAD_COLOR  (1u << 5)

/* Format a message and forward it to the log callback if registered.
 * No-op when the callback is NULL (the default). */
void cfr_vlog(CfrTerm *vt, CfrLogLevel level, const char *fmt, ...);

#define cfr_log(vt, level, fmt, ...) \
    cfr_vlog(vt, level, fmt, ##__VA_ARGS__)

/* Returns true the first time `bit` is seen for this CfrTerm, false
 * thereafter. Sets the bit so subsequent calls with the same bit are
 * suppressed. */
static inline bool should_log_once(CfrTerm *vt, uint32_t bit)
{
    if (vt->logged_once & bit)
        return false;
    vt->logged_once |= bit;
    return true;
}

/* ------------------------------------------------------------------ */
/* Internal helpers (cross-file)                                       */
/* ------------------------------------------------------------------ */

/* The cell's size in POINTS (logical pixels) for OSC 1337
 * ReportCellSize — the ONE client-facing report that defines its own
 * unit (iTerm2's points + scale). Every other pixel quantity the engine
 * reports or accepts is device pixels (cell_w_px/cell_h_px), drawn 1:1;
 * this conversion serves ReportCellSize alone. */
static inline void cfr_cell_logical_px(const CfrTerm *vt, int *out_w,
                                       int *out_h)
{
    if (out_w)
        *out_w = cfr_px_logical(vt->cell_w_px, vt->content_scale);
    if (out_h)
        *out_h = cfr_px_logical(vt->cell_h_px, vt->content_scale);
}

/* Allocator helpers — route through vt->alloc. */
void *cfr_alloc(CfrTerm *vt, size_t size);
void *cfr_realloc(CfrTerm *vt, void *ptr, size_t size);
void cfr_dealloc(CfrTerm *vt, void *ptr);

/* Page lifecycle. */
CfrPage *cfr_page_new(CfrTerm *vt, int rows, int cols);
void cfr_page_free(CfrTerm *vt, CfrPage *page);
/* Rebuild the live grid page when its intern tables have outgrown the
 * screen (see CFR_GRID_GC_*). A no-op when they have not, and when memory
 * runs out mid-rebuild: the old page is kept and a later call retries.
 * Safe at any byte boundary of the input — between bytes nothing but the
 * grid's own cells holds a page-scoped id — and unsafe anywhere a caller
 * holds a CfrCell pointer or a page id across the call. */
void cfr_grid_gc(CfrTerm *vt);
/* Re-intern one cell into `dst` from `src`. style_id, grapheme_id and
 * hyperlink_id are page-scoped, so every cross-page cell move (reflow,
 * clamp resize, scrollback push) must route through this: a raw id copy
 * either finds nothing in the destination table or, worse, silently
 * attaches a different style/cluster/link. `dst` and `src` must be
 * distinct, non-NULL pages (bug check otherwise).
 *
 * Returns true and writes the re-interned cell to *out on success. Returns
 * false (leaving *out untouched) when an attribute cannot be seated: an
 * allocation failure, or a destination table at its id cap. A caller
 * moving cells must then abandon the move — keep the old page, drop the
 * history row, fall back to clamp — rather than store a cell that
 * silently lost a style, cluster or link. A source id the source page
 * does not know still degrades to default/none: that is corrupt input,
 * not a resource failure. */
bool cfr_cell_reintern(CfrTerm *vt, CfrPage *dst, const CfrPage *src,
                       CfrCell cell, CfrCell *out);

/* Style intern. On success writes the page-local id to *out_id (0 = the
 * default style) and returns true. Returns false (leaving *out_id
 * untouched) when the table cannot grow. */
bool cfr_style_intern(CfrTerm *vt, CfrPage *page, const CfrStyle *style,
                      uint32_t *out_id);
const CfrStyle *cfr_style_lookup(const CfrPage *page, uint32_t id);

/* Grapheme arena. On success writes the arena offset to *out_id (0 when
 * len < 2 or page is NULL: the single-codepoint case) and returns true.
 * Returns false (leaving *out_id untouched) when the arena cannot grow. */
bool cfr_grapheme_intern(CfrTerm *vt, CfrPage *page,
                         const uint32_t *cps, uint32_t len, uint32_t *out_id);
size_t cfr_grapheme_read(const CfrPage *page, uint32_t id,
                         uint32_t *out, size_t out_cap);

/* Hyperlink intern (OSC 8) for a link with no `id=` parameter. Returns id
 * (1..UINT16_MAX), or 0 on allocation failure / overflow / empty URI.
 * Links that carry an id go through cfr_link_new + cfr_link_page_id. */
uint16_t cfr_hyperlink_intern(CfrTerm *vt, CfrPage *page,
                              const uint8_t *uri, uint32_t uri_len);
size_t cfr_hyperlink_read(const CfrPage *page, uint16_t id,
                          uint8_t *out, size_t out_cap);
/* The content key of `id` (see CfrHyperlinkTable), or 0 for id 0 or an id
 * the page does not know. The key covers the URI and, when present, the
 * OSC 8 `id=` parameter, so equal links have equal keys on every page. */
uint64_t cfr_hyperlink_key(const CfrPage *page, uint16_t id);
/* Re-intern `id` from `src` into `dst`. On success writes the dst-local
 * id to *out_id (0 for no link, or an id the source page does not know)
 * and returns true. Returns false (leaving *out_id untouched) on
 * allocation failure or when the destination table is at its id cap.
 * Lossless: the URI is interned straight from the source arena. */
bool cfr_hyperlink_transfer(CfrTerm *vt, CfrPage *dst,
                            const CfrPage *src, uint16_t id,
                            uint16_t *out_id);
void cfr_hyperlink_free(CfrTerm *vt, CfrHyperlinkTable *t);

/* Cap on the OSC 8 `id=` parameter's contribution to link identity.
 * Mirrors kitty's MAX_ID_LEN; the spec makes the limit implementation-
 * defined (VTE uses 250). Longer ids are truncated, as kitty does. */
#define CFR_OSC8_ID_MAX 256u

/* The pen's link (see CfrLink). `id` is the OSC 8 `id=` value (may be NULL
 * with id_len 0); it is folded into the link's identity key but not stored.
 * new returns NULL for an empty URI or on allocation failure, which reads
 * as "no link". release accepts NULL. */
CfrLink *cfr_link_new(CfrTerm *vt, const uint8_t *uri, uint32_t uri_len,
                      const uint8_t *id, uint32_t id_len);
void cfr_link_release(CfrTerm *vt, CfrLink *link);
/* The id of `link` in `page`'s table, interning it on first use. 0 for no
 * link, no page, or a failed intern (not cached, so it is retried). */
uint16_t cfr_link_page_id(CfrTerm *vt, CfrPage *page, CfrLink *link);

/* Parser entry. */
void cfr_parser_init(CfrParser *p);
void cfr_parser_feed(CfrTerm *vt, const uint8_t *bytes, size_t len);

/* Width helpers (width.c). */
int cfr_codepoint_width(CfrTerm *vt, uint32_t cp);
int cfr_cluster_width(CfrTerm *vt, const uint32_t *cps, uint32_t len);

/* True when `cp` extends a pending cluster instead of starting a new
 * one (GCB Extend ∪ ZWJ ∪ SpacingMark: combining marks, variation
 * selectors, joiners). print.c re-attaches stray joiners whose base was
 * committed by an earlier flush — a write boundary can split a base
 * from its VS16. */
bool cfr_is_grapheme_joiner(uint32_t cp);

/* Grid mutators (print.c). */
void cfr_grid_ensure(CfrTerm *vt);
void cfr_print_codepoint(CfrTerm *vt, uint32_t cp);
/* Bulk-print a run of printable ASCII (0x20..0x7E). Returns the number of
 * bytes consumed; 0 means the caller must use cfr_print_codepoint(). */
size_t cfr_print_ascii_run(CfrTerm *vt, const uint8_t *bytes, size_t len);
void cfr_flush_cluster(CfrTerm *vt);
void cfr_execute_c0(CfrTerm *vt, uint8_t b);
void cfr_scroll_up(CfrTerm *vt, int lines);
void cfr_scroll_down(CfrTerm *vt, int lines);
void cfr_erase_in_line(CfrTerm *vt, int mode);
void cfr_erase_in_display(CfrTerm *vt, int mode);
/* Tabs (print.c): HT/CHT forward, CBT backward. `count` is the number of
 * tab stops to cross; 1 or less means one. */
void cfr_tab_forward(CfrTerm *vt, int count);
void cfr_tab_backward(CfrTerm *vt, int count);

/* Cursor normalizer (print.c). cfr_cursor_set() clamps into the grid and
 * in doing so resolves the deferred phantom (col == cols). */
void cfr_cursor_set(CfrTerm *vt, int row, int col);

/* Resolve a deferred phantom in place without moving the cursor. Used by
 * operations that must not carry the phantom (EL/ECH/IL/DL, image cursor
 * advance, DECAWM off). */
void cfr_cursor_kill_phantom(CfrTerm *vt);

/* Cursor state changes (modes.c). Each keeps the matching DEC mode
 * (CFR_MODE_CURSOR_VISIBLE / CFR_MODE_CURSOR_BLINK) in sync and
 * notifies the host (set_mode, movecursor) and the damage accumulator
 * only when something really changed — so the mirrored mode and the
 * cursor fields can never disagree. */
void cfr_mode_flag_set(CfrTerm *vt, CfrMode mode, bool on);
void cfr_cursor_set_visible(CfrTerm *vt, bool on);
void cfr_cursor_set_style(CfrTerm *vt, CfrCursorShape shape, bool blink);

/* Full cursor restore (altscreen 1049 exit): everything in the
 * register, including the mode-ish attributes (visible, blink, shape)
 * that cfr_cursor_restore() deliberately preserves. */
void cfr_cursor_restore_full(CfrTerm *vt, const CfrCursorState *src);

/* Line-feed / reverse-index (print.c). IND, LF/VT/FF and NEL all share
 * linefeed; RI (ESC M, C1 0x8D) is cfr_reverse_index. */
void cfr_linefeed(CfrTerm *vt);
void cfr_reverse_index(CfrTerm *vt);

/* Wrap bookkeeping (lines.c). cfr_wrap_commit() is the single point that
 * creates a wrap: it stamps the wrap edge on the margin cell of the row
 * being left, moves the cursor to column 0 of the next row (scrolling the
 * region if needed), and propagates the row's lineage onto the row the
 * print lands on. Called for the deferred phantom and for the eager
 * wide-char-at-the-margin wrap. */
bool cfr_wrap_commit(CfrTerm *vt);

/* Lineage helpers (lines.c). cfr_lineage_stamp() assigns a fresh
 * logical-line id when a print first writes into a blank row (0 = blank);
 * cfr_lineage_next() mints an id for callers that assign it themselves
 * (reflow's lineage propagation, wrap commit). */
void cfr_lineage_stamp(CfrTerm *vt, int row);
uint32_t cfr_lineage_next(CfrTerm *vt);

/* The one row resolver (lines.c). Every consumer that reaches a row by
 * unified coordinate goes through here: `unified_row` >= 0 is a visible
 * grid row, < 0 is scrollback (-1 = the line most recently scrolled off).
 * Returns the page that owns the row and sets *out_row_in_page, or NULL
 * when the row does not exist. A page's `cols` is that row's own width —
 * scrollback rows keep the width they were written at, so it can differ
 * from the grid's. All page-scoped ids in the row's cells resolve against
 * the returned page. */
const CfrPage *cfr_row_page(const CfrTerm *vt, int unified_row,
                            int *out_row_in_page);

/* Logical-line predicate (lines.c). True when unified `row` continues
 * the row above it: the row above's margin cell carries the wrap edge
 * AND both rows share the same nonzero lineage. */
bool cfr_row_continues(const CfrTerm *vt, int unified_row);

/* Page width for a unified row (any page in sb/grid/altgrid; rows can
 * differ across the scrollback boundary). */
int cfr_row_width(const CfrTerm *vt, int unified_row);

/* Width / grapheme break (width.c). */
bool cfr_grapheme_break_before(uint32_t prev, uint32_t cur, void *state);

/* Palette (palette.c) — owns the live 256-color table (built-ins plus
 * OSC 4 overrides) and the dynamic color state (xterm OSC 10/11/12
 * family). `idx` is a CFR_DYN_* slot for the dynamic-color functions. */
void cfr_palette_init(CfrTerm *vt);
bool cfr_palette_set(CfrTerm *vt, uint8_t idx, uint32_t rgb);
bool cfr_palette_reset(CfrTerm *vt, uint8_t idx);
bool cfr_palette_reset_all(CfrTerm *vt);
void cfr_dyn_color_init(CfrTerm *vt, const CfrConfig *cfg);
uint32_t cfr_dyn_color_effective(const CfrTerm *vt, int idx);
void cfr_dyn_color_set(CfrTerm *vt, int idx, bool set, uint32_t rgb);
void cfr_dyn_color_reset(CfrTerm *vt, int idx);
void cfr_dyn_color_reset_all(CfrTerm *vt);

/* Output emit (keys.c). */
void cfr_emit_bytes(CfrTerm *vt, const uint8_t *bytes, size_t len);

/* Image decoding (image_decode.c). Decodes PNG/JPEG/etc. to RGBA. */
uint8_t *cfr_image_decode(const uint8_t *data, size_t len, int *w, int *h);

/* Zlib decompression (image_decode.c). Inflates a RFC1950 stream into a
 * freshly allocated buffer (the caller's allocator). Returns NULL on
 * failure; sets *out_len. */
uint8_t *cfr_zlib_decompress(CfrTerm *vt, const uint8_t *data, size_t len,
                             size_t *out_len);

/* Base64 decode (base64.c). RFC 4648; strips whitespace. Returns a
 * freshly allocated buffer (the caller's allocator) and sets *out_len,
 * or NULL on error. */
uint8_t *cfr_base64_decode(CfrTerm *vt, const char *in, size_t in_len,
                           size_t *out_len);

/* Append n bytes to a growing buffer (base64.c, the caller's allocator).
 * Doubles capacity as needed; one byte past the appended data is kept
 * and set to '\0' so the accumulator can be read as a C string. Returns
 * 0 on success, -1 on OOM. */
int cfr_buf_append(CfrTerm *vt, uint8_t **buf, size_t *len, size_t *cap,
                   const void *data, size_t n);

/* OSC 1337 (iTerm2 inline images, osc_1337.c). */
void cfr_osc_1337_dispatch(CfrTerm *vt, const uint8_t *body, size_t body_len);

/* APC router (apc.c). Routes by first byte: G → graphics, else → lottie. */
void cfr_apc_dispatch(CfrTerm *vt, const uint8_t *buf, size_t len);

/* Kitty graphics protocol (graphics.c). Called by the APC router. */
void cfr_graphics_apc_dispatch(CfrTerm *vt, const uint8_t *buf, size_t len);

/* Altscreen (modes.c). */
void cfr_set_altscreen(CfrTerm *vt, bool on, bool save_restore_cursor);

/* RIS — full reset (modes.c). Restores the terminal to its initial
 * state: clears the kitty keyboard stack, every DEC private mode,
 * DECCKM/DECKPAM/DECOM, charset designations, saved cursor, scroll
 * region, tab stops, and the grid. Notifies the host via set_mode for
 * any mode that turned off, so mouse/paste/etc. observers stay in
 * sync. Declared in <coffer/coffer.h> as cfr_reset(). */

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
/* Dirty the cursor's cell. The damage flush tracks only cursor
 * row/col/visibility, so an appearance change (style, blink, cursor
 * color) must dirty the cell explicitly. */
void cfr_damage_cursor(CfrTerm *vt);
/* Rectangular union — equivalent to cfr_damage_cell() on every cell it
 * covers, since the accumulator keeps a single union rect. */
void cfr_damage_span(CfrTerm *vt, int start_row, int start_col, int end_row,
                     int end_col);

/* Scrollback (scrollback.c). */
/* Append one row. `src` is the page that `src_cells`' style/grapheme/
 * hyperlink ids refer to (the live grid for a scroll, the old grid for
 * reflow); the cells are re-interned into the ring, so `src` need not
 * outlive the call. A push the ring cannot allocate for, or whose cells
 * cannot be re-interned with their attributes intact, is dropped rather
 * than stored degraded. Silent: the scroll that pushes announces its rows
 * (sb_pushline), reflow does not. */
void cfr_scrollback_push(CfrTerm *vt, const CfrPage *src,
                         const CfrCell *src_cells, int cols, uint32_t lineage);
void cfr_scrollback_clear(CfrTerm *vt);
/* Set the capacity and evict down to it at once (oldest rows first),
 * dropping the selection and anchored images that pointed at evicted
 * rows. Growing, or shrinking below what is held, evicts nothing. */
void cfr_scrollback_set_capacity(CfrTerm *vt, int lines);
/* Remove the `n` NEWEST scrollback rows (the end nearest the grid),
 * freeing any page that empties. `n` is clamped to the row count.
 *
 * The caller must have copied whatever it needs out of those rows
 * first: a page that empties is freed, and a push after the pop reuses
 * the freed slot. Reflow is the intended caller — it re-interns the
 * rows into the old grid while collecting, then pops once the new grid
 * is allocated, so a reflow failure leaves history intact.
 *
 * No image bookkeeping: cfr_resize() drops anchored images before
 * reflow, so nothing can be anchored to a row that moves here. */
void cfr_scrollback_pop_rows(CfrTerm *vt, int n);
/* Page/row lookup for a scrollback row, used by lines.c's unified-row
 * resolver. */
const CfrPage *cfr_sb_page_for_row(const CfrTerm *vt, int sb_row, int *out_row_in_page);

/* Selection (selection.c). Internal functions called inline during
 * scroll, draw, erase, resize, and altscreen transitions. */
void cfr_selection_on_scroll(CfrTerm *vt, bool up, int lines, int top, int bottom);
void cfr_selection_on_draw(CfrTerm *vt, int row);
void cfr_selection_fire_callback(CfrTerm *vt, bool active);
void cfr_selection_free(CfrTerm *vt);

/* Sixel graphics (sixel.c). The state hangs off vt->sixel, allocated
 * lazily. All entry points are no-ops when no sixel has been seen. */
void cfr_sixel_state_free(CfrTerm *vt);
/* DCS lifecycle, driven from dcs.c when the final byte is 'q'. */
void cfr_sixel_begin(CfrTerm *vt, const uint32_t *params, int nparams);
void cfr_sixel_put(CfrTerm *vt, const uint8_t *data, size_t len);
void cfr_sixel_finish(CfrTerm *vt);

/* Lottie animations (lottie.c). Lazily allocated, same pattern as sixel.
 * All entry points are no-ops when no animation has been loaded. */
void cfr_lottie_state_free(CfrTerm *vt);
void cfr_lottie_apc_dispatch(CfrTerm *vt, const uint8_t *body, size_t body_len);
void cfr_lottie_note_scroll(CfrTerm *vt, int lines);
void cfr_lottie_screen_switch(CfrTerm *vt);
void cfr_lottie_clear_display_rows(CfrTerm *vt, int top, int bot);
void cfr_lottie_clear_all(CfrTerm *vt);

#endif /* COFFER_INTERNAL_H */
