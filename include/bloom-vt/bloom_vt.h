/*
 * bloom-vt — self-contained virtual terminal engine.
 *
 * Single public header. Embed by including bloom_vt.h and linking
 * libbloom_vt.a. No other internal headers are needed by consumers.
 *
 * Thread-safety: a BvtTerm is not internally synchronized; callers
 * own all synchronization. Allocation: bounded; the input-write hot
 * path is zero-allocation in steady state. See "Memory model" in
 * README.md for details.
 */

#ifndef BLOOM_VT_H
#define BLOOM_VT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BvtTerm BvtTerm;

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
 * is retrieved with bvt_cell_get_grapheme().
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
} BvtCell;

/* Cell flags */
enum
{
    BVT_CELL_WRAPLINE = 1u << 0, /* set on the last cell of a soft-wrapped logical row */
};

/* ------------------------------------------------------------------ */
/* Style                                                               */
/* ------------------------------------------------------------------ */

/* Style attribute bits (matching the term.h order to ease term_bvt.c) */
enum
{
    BVT_ATTR_BOLD = 1u << 0,
    BVT_ATTR_ITALIC = 1u << 1,
    BVT_ATTR_BLINK = 1u << 2,
    BVT_ATTR_REVERSE = 1u << 3,
    BVT_ATTR_STRIKETHROUGH = 1u << 4,
    BVT_ATTR_DWL = 1u << 5,        /* double-width line */
    BVT_ATTR_DHL_TOP = 1u << 6,    /* double-height line, top half */
    BVT_ATTR_DHL_BOTTOM = 1u << 7, /* double-height line, bottom half */
};

/* Underline styles: SGR 4:N */
typedef enum
{
    BVT_UL_NONE = 0,
    BVT_UL_SINGLE = 1,
    BVT_UL_DOUBLE = 2,
    BVT_UL_CURLY = 3,
    BVT_UL_DOTTED = 4,
    BVT_UL_DASHED = 5,
} BvtUnderline;

/* Color flags */
enum
{
    BVT_COLOR_DEFAULT_FG = 1u << 0,
    BVT_COLOR_DEFAULT_BG = 1u << 1,
    BVT_COLOR_DEFAULT_UL = 1u << 2,
    BVT_COLOR_INDEXED_FG = 1u << 3, /* fg_rgb low byte is a 0-255 palette index, not RGB */
    BVT_COLOR_INDEXED_BG = 1u << 4,
};

typedef struct
{
    uint32_t fg_rgb; /* 0x00RRGGBB if RGB; low byte = index if indexed */
    uint32_t bg_rgb;
    uint32_t ul_rgb;
    uint16_t attrs;       /* BVT_ATTR_* bitmask */
    uint8_t underline;    /* BvtUnderline */
    uint8_t font;         /* font select (0 = primary, 1-9 alt fonts via SGR 10-19) */
    uint16_t color_flags; /* BVT_COLOR_* */
} BvtStyle;

/* Resolve a cell's style. Returns NULL if the term/cell is invalid. */
const BvtStyle *bvt_cell_style(const BvtTerm *vt, const BvtCell *cell);

/* Copy a cell's grapheme codepoints into `out`. Returns the number of
 * codepoints written. `out_cap` caps the write; a return value equal
 * to `out_cap` indicates truncation. For single-cp cells this returns
 * 1 with cell->cp. */
size_t bvt_cell_get_grapheme(const BvtTerm *vt, const BvtCell *cell,
                             uint32_t *out, size_t out_cap);

/* Copy a cell's hyperlink URI bytes into `out_uri`. Returns the URI
 * length, or 0 if the cell has no link or the term/cell is invalid.
 * The URI bytes are raw (per OSC 8 spec, constrained to 32–126);
 * callers wanting a C string must NUL-terminate. A return value equal
 * to `out_cap` indicates truncation. */
size_t bvt_cell_get_hyperlink(const BvtTerm *vt, const BvtCell *cell,
                              uint8_t *out_uri, size_t out_cap);

/* ------------------------------------------------------------------ */
/* Geometry                                                            */
/* ------------------------------------------------------------------ */

typedef struct
{
    int row;
    int col;
} BvtPos;

typedef struct
{
    int start_row, start_col; /* inclusive */
    int end_row, end_col;     /* inclusive */
} BvtRect;

typedef struct
{
    int row;
    int col;
    bool visible;
    bool blink;
} BvtCursor;

/* ------------------------------------------------------------------ */
/* Modes                                                               */
/* ------------------------------------------------------------------ */

typedef enum
{
    BVT_MODE_ALTSCREEN,
    BVT_MODE_CURSOR_VISIBLE,
    BVT_MODE_CURSOR_BLINK,
    BVT_MODE_DECAWM,        /* Auto-Wrap Mode, DECSET ?7 — default on */
    BVT_MODE_REVERSE_VIDEO, /* DECSCNM */
    BVT_MODE_BRACKETED_PASTE,
    BVT_MODE_MOUSE_X10,          /* DECSET 9 */
    BVT_MODE_MOUSE_BTN_EVENT,    /* DECSET 1000 */
    BVT_MODE_MOUSE_DRAG,         /* DECSET 1002 */
    BVT_MODE_MOUSE_ANY_EVENT,    /* DECSET 1003 */
    BVT_MODE_MOUSE_SGR,          /* DECSET 1006 */
    BVT_MODE_FOCUS_REPORTING,    /* DECSET 1004 */
    BVT_MODE_GRAPHEME_CLUSTERS,  /* mode 2027 (Contour) */
    BVT_MODE_SYNC_OUTPUT,        /* mode 2026 */
    BVT_MODE_SIXEL_SCROLLING,    /* DECSDM, mode 80 — when on, draw sixel in place */
    BVT_MODE_SIXEL_PRIVATE_REGS, /* mode 1070 — private sixel color registers */
    BVT_MODE_SIXEL_CURSOR_RIGHT, /* mode 8452 — cursor to right of graphic */
} BvtMode;

/* ------------------------------------------------------------------ */
/* Keyboard / mouse                                                    */
/* ------------------------------------------------------------------ */

typedef enum
{
    BVT_MOD_NONE = 0,
    BVT_MOD_SHIFT = 1u << 0,
    BVT_MOD_ALT = 1u << 1,
    BVT_MOD_CTRL = 1u << 2,
    BVT_MOD_META = 1u << 3,
} BvtMods;

typedef enum
{
    BVT_KEY_NONE = 0,
    BVT_KEY_ENTER,
    BVT_KEY_TAB,
    BVT_KEY_BACKSPACE,
    BVT_KEY_ESCAPE,
    BVT_KEY_UP,
    BVT_KEY_DOWN,
    BVT_KEY_LEFT,
    BVT_KEY_RIGHT,
    BVT_KEY_INS,
    BVT_KEY_DEL,
    BVT_KEY_HOME,
    BVT_KEY_END,
    BVT_KEY_PAGEUP,
    BVT_KEY_PAGEDOWN,
    BVT_KEY_F1,
    BVT_KEY_F2,
    BVT_KEY_F3,
    BVT_KEY_F4,
    BVT_KEY_F5,
    BVT_KEY_F6,
    BVT_KEY_F7,
    BVT_KEY_F8,
    BVT_KEY_F9,
    BVT_KEY_F10,
    BVT_KEY_F11,
    BVT_KEY_F12,
    BVT_KEY_KP_0,
    BVT_KEY_KP_1,
    BVT_KEY_KP_2,
    BVT_KEY_KP_3,
    BVT_KEY_KP_4,
    BVT_KEY_KP_5,
    BVT_KEY_KP_6,
    BVT_KEY_KP_7,
    BVT_KEY_KP_8,
    BVT_KEY_KP_9,
    BVT_KEY_KP_MULTIPLY,
    BVT_KEY_KP_PLUS,
    BVT_KEY_KP_COMMA,
    BVT_KEY_KP_MINUS,
    BVT_KEY_KP_PERIOD,
    BVT_KEY_KP_DIVIDE,
    BVT_KEY_KP_ENTER,
    BVT_KEY_KP_EQUAL,
} BvtKey;

typedef enum
{
    BVT_MOUSE_NONE = 0,
    BVT_MOUSE_LEFT,
    BVT_MOUSE_MIDDLE,
    BVT_MOUSE_RIGHT,
    BVT_MOUSE_WHEEL_UP,
    BVT_MOUSE_WHEEL_DOWN,
} BvtMouseButton;

/* ------------------------------------------------------------------ */
/* Callbacks                                                           */
/* ------------------------------------------------------------------ */

typedef struct
{
    /* damage: a rectangular region of the visible grid changed */
    void (*damage)(BvtRect rect, void *user);

    /* moverect: contents of `src` were moved to `dst` (e.g. scroll). */
    void (*moverect)(BvtRect dst, BvtRect src, void *user);

    /* movecursor: cursor moved or visibility changed */
    void (*movecursor)(BvtCursor cur, void *user);

    /* bell: BEL received */
    void (*bell)(void *user);

    /* set_title: OSC 0/1/2 received. `utf8` is NUL-terminated and
     * valid only during the call. */
    void (*set_title)(const char *utf8, void *user);

    /* set_mode: a tracked mode changed */
    void (*set_mode)(BvtMode mode, bool on, void *user);

    /* output: bytes the application produced to send to the PTY
     * (mouse reports, DA/DSR responses, kitty keyboard responses...) */
    void (*output)(const uint8_t *bytes, size_t len, void *user);

    /* sb_pushline: a row scrolled off the top into scrollback */
    void (*sb_pushline)(const BvtCell *cells, int cols, bool wrapline, void *user);

    /* sb_popline: scrollback bottommost line is popped back onto screen */
    void (*sb_popline)(BvtCell *out_cells, int cols, void *user);

    /* osc: OSC string callback (title is delivered via set_title) */
    void (*osc)(int code, const char *data, size_t len, void *user);

    /* dcs: streamed DCS callback. `final` is true on the final chunk. */
    void (*dcs)(const char *intro, const char *data, size_t len, bool final,
                void *user);
} BvtCallbacks;

/* ------------------------------------------------------------------ */
/* Allocator hooks                                                     */
/* ------------------------------------------------------------------ */

typedef struct
{
    void *(*alloc)(size_t size, void *user);
    void *(*realloc)(void *ptr, size_t size, void *user);
    void (*free)(void *ptr, void *user);
    void *user;
} BvtAllocator;

/* Configuration for bvt_new(). Zero-initialize then set fields; unset
 * fields (0 / false) use defaults. Rows, cols, and cell pixel size
 * are required and must be > 0. */
#define BVT_CONFIG_DEFAULTS                                                                                              \
    {                                                                                                                    \
        .rows = 0, .cols = 0, .cell_w_px = 0, .cell_h_px = 0, .scrollback = -1, .reflow = false, .ambiguous_wide = false \
    }
typedef struct
{
    int rows;            /* grid height (required > 0) */
    int cols;            /* grid width  (required > 0) */
    int cell_w_px;       /* pixel width of one cell  (required > 0) */
    int cell_h_px;       /* pixel height of one cell (required > 0) */
    int scrollback;      /* scrollback lines (-1 = default 1000, 0 = off) */
    bool reflow;         /* reflow text on resize (default false) */
    bool ambiguous_wide; /* treat ambiguous-width chars as wide (default false) */
} BvtConfig;

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/* Create a terminal from a config struct. Returns NULL on allocation
 * failure or if required fields (rows, cols, cell_w_px, cell_h_px)
 * are <= 0. */
BvtTerm *bvt_new(const BvtConfig *cfg);

/* Variant that takes an allocator. The allocator is copied; the user
 * pointer must outlive the term. Pass NULL to use the stdlib. */
BvtTerm *bvt_new_with_allocator(const BvtConfig *cfg, const BvtAllocator *alloc);

void bvt_free(BvtTerm *vt);

/* Wire callbacks. May be called multiple times; the table is copied. */
void bvt_set_callbacks(BvtTerm *vt, const BvtCallbacks *cb, void *user);

/* Resize and reflow if reflow is enabled. Cursor position is preserved. */
void bvt_resize(BvtTerm *vt, int rows, int cols);

/* Configuration. */
void bvt_set_reflow(BvtTerm *vt, bool enabled);
void bvt_set_ambiguous_wide(BvtTerm *vt, bool wide);
void bvt_set_scrollback_size(BvtTerm *vt, int lines);

/* ------------------------------------------------------------------ */
/* I/O                                                                 */
/* ------------------------------------------------------------------ */

/* Feed PTY bytes to the parser. Returns the number consumed (always
 * `len` unless the term is in an error state). */
size_t bvt_input_write(BvtTerm *vt, const uint8_t *bytes, size_t len);

/* Drain accumulated damage: invoke the `damage` callback (if set) with the
 * rectangular union of everything that changed since the last flush, then
 * clear the accumulator. The engine never fires `damage` on its own — the
 * consumer calls this at a controlled time (typically once per frame, just
 * before rendering), so damage from a burst of bvt_input_write() calls
 * coalesces into one callback. A cursor-only move (which dirties no grid cell)
 * is folded in here by damaging the old and new cursor cells. */
void bvt_damage_flush(BvtTerm *vt);

void bvt_send_key(BvtTerm *vt, BvtKey key, BvtMods mods);
void bvt_send_text(BvtTerm *vt, const char *utf8, size_t len, BvtMods mods);
void bvt_send_mouse(BvtTerm *vt, int row, int col, BvtMouseButton b,
                    bool pressed, BvtMods mods);

void bvt_paste_begin(BvtTerm *vt);
void bvt_paste_end(BvtTerm *vt);

/* ------------------------------------------------------------------ */
/* Queries                                                             */
/* ------------------------------------------------------------------ */

/* Returns NULL for out-of-range coordinates. Continuation cells
 * (width == 0) are returned as-is; callers skip them. */
const BvtCell *bvt_get_cell(const BvtTerm *vt, int row, int col);

void bvt_get_dimensions(const BvtTerm *vt, int *out_rows, int *out_cols);

/* Number of lines currently held in scrollback (grows as content scrolls off,
 * capped at the configured capacity). */
int bvt_get_scrollback_lines(const BvtTerm *vt);
/* Configured scrollback capacity in lines (what bvt_set_scrollback_size set, or
 * the built-in default). 0 means scrollback is disabled. */
int bvt_get_scrollback_capacity(const BvtTerm *vt);
const BvtCell *bvt_get_scrollback_cell(const BvtTerm *vt, int sb_row, int col);
bool bvt_get_scrollback_wrapline(const BvtTerm *vt, int sb_row);

BvtCursor bvt_get_cursor(const BvtTerm *vt);
const char *bvt_get_title(const BvtTerm *vt);
bool bvt_is_altscreen(const BvtTerm *vt);
bool bvt_get_mode(const BvtTerm *vt, BvtMode mode);
bool bvt_get_line_continuation(const BvtTerm *vt, int row);

/* ------------------------------------------------------------------ */
/* Sixel graphics                                                      */
/* ------------------------------------------------------------------ */

/*
 * A decoded sixel image, anchored to a grid line. The engine owns the
 * pixel data and the lifetime; the view returned by bvt_get_sixels() is
 * valid until the next bvt_input_write()/bvt_get_sixels() call.
 *
 * `row` is a unified row coordinate: >= 0 is a visible grid row (0 =
 * top), < 0 is scrollback depth (-1 = the line most recently scrolled
 * off). The host maps it to a display position exactly as it does for
 * cells, accounting for the user's scrollback offset.
 *
 * `id` is stable across frames for the same image (use it as a texture
 * cache key); `version` bumps whenever the pixels at that id change
 * (animation / in-place frame replacement), so the host re-uploads only
 * when it must. `layer` is 0 for foreground (drawn over text); layer 1
 * (background, drawn behind text) is reserved for a future extension.
 */
typedef struct
{
    uint64_t id;
    uint32_t version;
    uint8_t layer;
    int row;
    int col;
    int width_px;
    int height_px;
    const uint8_t *rgba; /* width_px * height_px * 4, RGBA, engine-owned */
} BvtSixel;

/* Update the pixel size of one character cell. Call on font load and on
 * resize. The cell pixel size is always known (set at creation), but this
 * updates it when it changes. */
void bvt_set_cell_pixels(BvtTerm *vt, int cell_w_px, int cell_h_px);

/* Return the live sixel images. The returned array is owned by the
 * engine and valid until the next mutation; *out_count receives the
 * number of images (0 if none). Returns NULL when there are none. */
const BvtSixel *bvt_get_sixels(BvtTerm *vt, int *out_count);

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
} BvtLottiePlacement;

/* A Lottie animation snapshot, returned by bvt_get_lotties(). Engine-owned.
 * `rgba` is the rasterized RGBA32 pixel data for the current frame,
 * valid until the next bvt_input_write()/bvt_get_lotties() call.
 * Placements are queried separately via bvt_get_lottie_placements(). */
typedef struct
{
    uint64_t id;
    uint32_t version;
    int canvas_w;        /* rasterization width in pixels (placement cells × cell px) */
    int canvas_h;        /* rasterization height in pixels (placement cells × cell px) */
    const uint8_t *rgba; /* canvas_w * canvas_h * 4, RGBA32, engine-owned */
    int current_frame;
    int frame_count;
    bool playing;
    double speed;
    bool loop;
    int placement_count;
} BvtLottie;

/* Return the live Lottie animations. The returned array is a contiguous
 * array of BvtLottie structs (NO interleaved placement data), owned by the
 * engine and valid until the next mutation; *out_count receives the
 * number of animations (0 if none). Returns NULL when there are none.
 * Use bvt_get_lottie_placements() to query placements for each animation. */
const BvtLottie *bvt_get_lotties(BvtTerm *vt, int *out_count);

/* Return the placements for a specific animation. The returned array is
 * owned by the engine and valid until the next mutation. *out_count
 * receives the number of placements (0 if none). Returns NULL if the
 * animation has no placements or doesn't exist. */
const BvtLottiePlacement *bvt_get_lottie_placements(BvtTerm *vt, uint64_t id,
                                                    int *out_count);

/* Advance all playing Lottie animations to the frame appropriate for
 * the given timestamp (microseconds). Re-rasterizes any whose frame
 * changed. Call once per frame before bvt_get_lotties(). Returns true
 * if any animation advanced (pixel data changed). */
bool bvt_lottie_tick(BvtTerm *vt, uint64_t now_us);

/* Notify the Lottie subsystem of a scroll event (cull scrolled-off
 * placements). Called automatically after scroll; also available for
 * manual invocation. */
void bvt_lottie_note_scroll(BvtTerm *vt, int lines);

/* Remove foreground-layer placements overlapping the inclusive display-row
 * range [top,bot]. Background placements survive. */
void bvt_lottie_clear_display_rows(BvtTerm *vt, int top, int bot);

/* Returns true if bloom-vt was built with ThorVG (Lottie rasterization
 * produces real pixels). Returns false if ThorVG was absent or disabled
 * at build time — APC sequences are accepted but produce blank frames. */
bool bvt_have_lottie(void);

#ifdef __cplusplus
}
#endif

#endif /* BLOOM_VT_H */
