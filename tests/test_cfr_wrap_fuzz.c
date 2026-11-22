/* tests/test_cfr_wrap_fuzz.c — differential fuzz for soft wraps.
 *
 * A random stream of print/cursor/edit/scroll/resize ops is applied to a
 * CfrTerm and to a naive line model: an array of rows, each with its own
 * width, a row-level wrap edge ("the margin cell still ends in a wrap")
 * and a lineage id ("this row is a fragment of logical line N"). Cells
 * own their UTF-8 bytes so selector and ZWJ clusters need no intern
 * tables. After every op the test compares the engine against the model:
 *
 *   - the scrollback depth,
 *   - cfr_row_is_continuation() for every unified row,
 *   - cfr_row_text() for every unified row,
 *   - cfr_selection_get_text() for the whole unified buffer.
 *
 * The model is the executable form of docs/wrapped-lines-spec.md; the
 * rules it implements are the spec's, not a second copy of lines.c
 * (lineage is stamped once per row, the edge is a row field, and the
 * model has no pages, no cell interning and no scrollback ring).
 *
 * Coverage includes partial scroll regions (DECSTBM), DECOM origin mode,
 * altscreen 1049 and raw 47/1047 (including resizes while it is active
 * and the exit-time catch-up), resize with reflow on and clamp-only
 * resize with reflow off, and VS16/VS15/combining/ZWJ clusters printed
 * as one write. Not modeled: stray-joiner re-attachment across writes
 * (see spec §8).
 *
 * The run is deterministic. Set COFFER_FUZZ_ITERS to run longer and
 * COFFER_FUZZ_SEED to start from another base seed; a failure prints the
 * seed, the config, the op index and the op stream. COFFER_FUZZ_ONLY
 * narrows the config, COFFER_FUZZ_STRICT also compares exact lineage ids
 * and margin bits, and COFFER_FUZZ_TRACE=N dumps both grids from op N.
 */

#include "coffer_internal.h"
#include "test_helpers.h"
#include <coffer/coffer.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Model                                                              */
/* ------------------------------------------------------------------ */

#define ROWS_MAX      8
#define COLS_MAX      12
#define HIST_MAX      128
#define TEXT_CAP      8192
#define CELL_TEXT_MAX 24

/* UTF-8 for U+4E00 (一), always two cells wide. */
#define WIDE_UTF8     "\xE4\xB8\xAD"
#define WIDE_UTF8_LEN 3

/* Cell kinds. The model keeps only what the wrap predicate, reflow and
 * the two text extractions need:
 *   HOLE  never written (engine: all-zero cell, width 0)
 *   BLANK erased or a printed space (engine: cp 0x20, width 1)
 *   CONT  continuation of a two-cell cluster (engine: width 0)
 *   CELL  any printable cluster, one or two cells wide
 *
 * A CELL owns its UTF-8 bytes: width alone cannot distinguish a plain
 * ASCII byte from a selector cluster or a ZWJ sequence, and reflow has
 * to move the bytes, not just the cells. */
enum
{
    MC_HOLE = 0,
    MC_BLANK,
    MC_CONT,
    MC_CELL
};

typedef struct
{
    uint8_t kind;
    uint8_t width;
    uint8_t len;
    char text[CELL_TEXT_MAX];
} MCell;

typedef struct
{
    MCell cells[COLS_MAX];
    int cols;      /* the width this row was written at */
    uint32_t line; /* 0 = blank/unwritten */
    bool wrap;     /* the margin cell carries the wrap edge */
} MRow;

typedef struct
{
    int rows, cols; /* current terminal geometry (vt->rows/cols) */
    /* The main page's allocation dims. Equal to rows/cols while the main
     * screen is active; stale while the alt screen is active after a
     * resize. The alt page is recreated on every entry, so its dims
     * always match the terminal's. */
    int main_rows, main_cols;
    int sb_cap;
    MRow grid[ROWS_MAX];
    MRow altgrid[ROWS_MAX];
    MRow hist[HIST_MAX];
    int hist_len;
    int row, col; /* col may be cols: the deferred-wrap phantom */
    bool decawm;
    bool irm;
    bool decom;
    bool reflow_on;
    bool in_alt;
    int scroll_top, scroll_bottom;
    /* DECSC/DECRC use the active screen's register (normal = 0, alt = 1);
     * 1049 always saves and restores register 0. */
    int saved_row[2], saved_col[2];
    uint32_t next_line;
} Model;

static MRow *m_row(Model *m, int u)
{
    if (u >= 0)
        return u < m->rows ? &m->grid[u] : NULL;
    int idx = m->hist_len + u;
    return (idx >= 0 && idx < m->hist_len) ? &m->hist[idx] : NULL;
}

static uint32_t m_next_line(Model *m)
{
    if (m->next_line == 0)
        m->next_line = 1;
    return m->next_line++;
}

static void m_stamp(Model *m, int r)
{
    if (m->grid[r].line == 0)
        m->grid[r].line = m_next_line(m);
}

static void m_cell_hole(MCell *c)
{
    memset(c, 0, sizeof(*c));
    c->kind = MC_HOLE;
}

static void m_cell_blank(MCell *c)
{
    memset(c, 0, sizeof(*c));
    c->kind = MC_BLANK;
    c->width = 1;
    c->text[0] = ' ';
    c->len = 1;
}

static void m_cell_cont(MCell *c)
{
    memset(c, 0, sizeof(*c));
    c->kind = MC_CONT;
}

static void m_cell_text(MCell *c, const char *text, size_t len, int width)
{
    memset(c, 0, sizeof(*c));
    c->kind = MC_CELL;
    c->width = (uint8_t)width;
    if (len > CELL_TEXT_MAX - 1)
        len = CELL_TEXT_MAX - 1;
    memcpy(c->text, text, len);
    c->len = (uint8_t)len;
}

/* Whole-row clears, matching the engine's two flavors: scroll-induced
 * erasure uses memset (holes), explicit erasure uses erase_cells
 * (spaces). Both reset the row's lineage and wrap edge. */
static void m_clear_holes(MRow *r, int cols)
{
    for (int c = 0; c < cols; ++c)
        m_cell_hole(&r->cells[c]);
    r->cols = cols;
    r->line = 0;
    r->wrap = false;
}

static void m_clear_blanks(MRow *r, int cols)
{
    for (int c = 0; c < cols; ++c)
        m_cell_blank(&r->cells[c]);
    r->cols = cols;
    r->line = 0;
    r->wrap = false;
}

static void m_hist_push(Model *m, const MRow *row)
{
    if (m->sb_cap <= 0)
        return;
    if (m->hist_len == m->sb_cap) {
        memmove(&m->hist[0], &m->hist[1],
                (size_t)(m->hist_len - 1) * sizeof(MRow));
        m->hist_len--;
    }
    m->hist[m->hist_len++] = *row;
}

/* Scroll the current region [scroll_top, scroll_bottom] up by n rows.
 * Only a full-screen scroll feeds scrollback, and never on the alt
 * screen (cfr_scroll_up's push_to_sb condition). */
static void m_scroll_up(Model *m, int n)
{
    int top = m->scroll_top;
    int bot = m->scroll_bottom;
    if (top < 0)
        top = 0;
    if (bot >= m->rows)
        bot = m->rows - 1;
    if (n <= 0 || top >= bot)
        return;
    int height = bot - top + 1;
    if (n > height)
        n = height;
    if (top == 0 && bot == m->rows - 1 && !m->in_alt) {
        for (int i = 0; i < n; ++i)
            m_hist_push(m, &m->grid[top + i]);
    }
    if (n < height)
        memmove(&m->grid[top], &m->grid[top + n],
                (size_t)(height - n) * sizeof(MRow));
    for (int i = height - n; i < height; ++i)
        m_clear_holes(&m->grid[top + i], m->cols);
}

static void m_scroll_down(Model *m, int n)
{
    int top = m->scroll_top;
    int bot = m->scroll_bottom;
    if (top < 0)
        top = 0;
    if (bot >= m->rows)
        bot = m->rows - 1;
    if (n <= 0 || top >= bot)
        return;
    int height = bot - top + 1;
    if (n > height)
        n = height;
    if (n < height)
        memmove(&m->grid[top + n], &m->grid[top],
                (size_t)(height - n) * sizeof(MRow));
    for (int i = 0; i < n; ++i)
        m_clear_blanks(&m->grid[top + i], m->cols);
}

/* Resolve the phantom into a wrap. Mirrors cfr_wrap_commit(): stamp the
 * row being left, move to the next row (scrolling the region at its
 * bottom), and propagate the lineage id onto the row the print lands on.
 * False when the cursor is on the bottom row but below the region's
 * bottom: the engine writes at the margin without a join. */
static bool m_wrap_commit(Model *m)
{
    int src = m->row;
    if (src < 0)
        src = 0;
    if (src >= m->rows)
        src = m->rows - 1;

    uint32_t id = m->grid[src].line;
    if (id == 0) {
        id = m_next_line(m);
        m->grid[src].line = id;
    }
    m->grid[src].wrap = true;

    int dst;
    if (src == m->scroll_bottom) {
        m_scroll_up(m, 1);
        dst = src;
    } else if (src < m->rows - 1) {
        dst = src + 1;
    } else {
        return false;
    }
    m->grid[dst].line = id;
    m->row = dst;
    m->col = 0;
    return true;
}

/* cfr_cursor_set(): explicit moves clamp both coordinates (unlike the
 * phantom-preserving clamp used by prints). */
static void m_cursor_set(Model *m, int row, int col)
{
    if (row < 0)
        row = 0;
    if (row >= m->rows)
        row = m->rows - 1;
    if (col < 0)
        col = 0;
    if (col >= m->cols)
        col = m->cols - 1;
    m->row = row;
    m->col = col;
}

/* CUP/HVP/VPA destination: with DECOM on, the row is relative to the
 * scroll region and clamped to its bottom (csi.c cursor_to_origin). The
 * column is always absolute and clamped by cfr_cursor_set(). */
static void m_cursor_to_origin(Model *m, int row, int col)
{
    if (m->decom) {
        int top = m->scroll_top;
        int bot = m->scroll_bottom;
        if (row < 0)
            row = 0;
        row += top;
        if (row > bot)
            row = bot;
    }
    m_cursor_set(m, row, col);
}

/* cursor_clamp(): a print clamps the row and lets the column reach
 * `cols` (the deferred phantom). */
static void m_cursor_clamp(Model *m)
{
    if (m->row < 0)
        m->row = 0;
    if (m->row >= m->rows)
        m->row = m->rows - 1;
    if (m->col < 0)
        m->col = 0;
    if (m->col > m->cols)
        m->col = m->cols;
}

static void m_ich(Model *m, int count)
{
    if (count <= 0)
        return;
    if (m->col >= m->cols)
        m->col = m->cols - 1;
    int c = m->col;
    if (m->row < 0 || m->row >= m->rows || c < 0 || c >= m->cols)
        return;
    if (count > m->cols - c)
        count = m->cols - c;

    MRow *r = &m->grid[m->row];
    bool had_wrap = r->wrap;
    int move = m->cols - c - count;
    if (move > 0)
        memmove(&r->cells[c + count], &r->cells[c],
                (size_t)move * sizeof(MCell));
    for (int i = 0; i < count; ++i)
        m_cell_blank(&r->cells[c + i]);
    /* ICH shifts the margin cell instead of erasing the row's end, so
     * the edge and the lineage survive (the spec's policy choice). */
    r->wrap = had_wrap;
}

static void m_dch(Model *m, int count)
{
    if (count <= 0)
        return;
    if (m->col >= m->cols)
        m->col = m->cols - 1;
    int c = m->col;
    if (m->row < 0 || m->row >= m->rows || c < 0 || c >= m->cols)
        return;
    if (count > m->cols - c)
        count = m->cols - c;

    MRow *r = &m->grid[m->row];
    int move = m->cols - c - count;
    if (move > 0)
        memmove(&r->cells[c], &r->cells[c + count],
                (size_t)move * sizeof(MCell));
    for (int i = m->cols - count; i < m->cols; ++i) {
        m_cell_blank(&r->cells[i]);
    }
    /* The tail erase always rewrites the margin cell. */
    r->wrap = false;
}

static void m_ech(Model *m, int count)
{
    if (count <= 0)
        return;
    if (m->col >= m->cols)
        m->col = m->cols - 1;
    int c = m->col;
    if (m->row < 0 || m->row >= m->rows || c < 0 || c >= m->cols)
        return;
    if (count > m->cols - c)
        count = m->cols - c;

    MRow *r = &m->grid[m->row];
    for (int i = 0; i < count; ++i)
        m_cell_blank(&r->cells[c + i]);
    if (c + count - 1 == m->cols - 1)
        r->wrap = false;
}

static void m_el(Model *m, int mode)
{
    /* cfr_erase_in_line() checks the row before it resolves a pending
     * phantom, so a stale row from DECRC leaves the column alone. */
    if (m->row < 0 || m->row >= m->rows)
        return;
    if (m->col >= m->cols)
        m->col = m->cols - 1;
    if (m->col < 0 || m->col >= m->cols)
        return;
    MRow *r = &m->grid[m->row];
    int from, to;
    switch (mode) {
    case 0:
        from = m->col;
        to = m->cols;
        break;
    case 1:
        from = 0;
        to = m->col + 1;
        break;
    default:
        from = 0;
        to = m->cols;
        break;
    }
    for (int c = from; c < to; ++c)
        m_cell_blank(&r->cells[c]);
    if (to == m->cols)
        r->wrap = false;
}

static void m_ed(Model *m, int mode)
{
    /* ED 0/1 are cursor-relative: a stale cursor row (DECRC restored
     * from before a shrink) makes them no-ops, as in cfr_erase_in_line.
     * ED 2 still clears everything; ED 3 is scrollback-only. */
    if ((mode == 0 || mode == 1) &&
        (m->row < 0 || m->row >= m->rows))
        return;
    if (mode == 3) {
        m->hist_len = 0;
        return;
    }
    if (mode == 0) {
        m_el(m, 0);
        for (int rr = m->row + 1; rr < m->rows; ++rr)
            m_clear_blanks(&m->grid[rr], m->cols);
    } else if (mode == 1) {
        m_el(m, 1);
        for (int rr = 0; rr < m->row; ++rr)
            m_clear_blanks(&m->grid[rr], m->cols);
    } else {
        /* ED 2 clears the cells but not the cursor; a pending phantom
         * survives (the engine's ED 2 does not go through EL). */
        for (int rr = 0; rr < m->rows; ++rr)
            m_clear_blanks(&m->grid[rr], m->cols);
    }
}

static void m_il(Model *m, int count)
{
    if (count <= 0)
        return;
    int row = m->row;
    if (row < m->scroll_top || row > m->scroll_bottom)
        return;
    int region = m->scroll_bottom - row + 1;
    if (count > region)
        count = region;
    int move = region - count;
    if (move > 0)
        memmove(&m->grid[row + count], &m->grid[row],
                (size_t)move * sizeof(MRow));
    for (int i = 0; i < count; ++i)
        m_clear_blanks(&m->grid[row + i], m->cols);
    m->col = 0;
}

static void m_dl(Model *m, int count)
{
    if (count <= 0)
        return;
    int row = m->row;
    if (row < m->scroll_top || row > m->scroll_bottom)
        return;
    int region = m->scroll_bottom - row + 1;
    if (count > region)
        count = region;
    int move = region - count;
    if (move > 0)
        memmove(&m->grid[row], &m->grid[row + count],
                (size_t)move * sizeof(MRow));
    int clear_start = m->scroll_bottom - count + 1;
    for (int i = 0; i < count; ++i)
        m_clear_blanks(&m->grid[clear_start + i], m->cols);
    m->col = 0;
}

static void m_lf(Model *m)
{
    if (m->col >= m->cols)
        m->col = 0;
    if (m->row == m->scroll_bottom)
        m_scroll_up(m, 1);
    else if (m->row < m->rows - 1)
        m->row++;
}

static void m_ri(Model *m)
{
    if (m->row == m->scroll_top)
        m_scroll_down(m, 1);
    else if (m->row > 0)
        m->row--;
    if (m->col >= m->cols)
        m->col = m->cols - 1;
}

static void m_tab(Model *m)
{
    int col = m->col;
    if (col > m->cols - 1)
        col = m->cols - 1;
    int c = col + 1;
    while (c < m->cols && (c % 8) != 0)
        c++;
    if (c >= m->cols)
        c = m->cols - 1;
    /* HT ends in cfr_cursor_set(), so a stale row is clamped too. */
    m_cursor_set(m, m->row, c);
}

/* Print one cluster through the commit_cluster() rules: phantom
 * resolution, the eager wrap when a two-cell cluster no longer fits,
 * IRM, overwriting, and the advance to the phantom or the margin. */
static void m_print_cell(Model *m, const char *text, size_t len, int width)
{
    if (m->col >= m->cols) {
        if (m->decawm) {
            if (!m_wrap_commit(m))
                m->col = m->cols - 1;
        } else {
            m->col = m->cols - 1;
        }
    }
    m_cursor_clamp(m);
    if (width == 2 && m->col + width > m->cols && m->decawm) {
        if (!m_wrap_commit(m) && m->col >= m->cols)
            m->col = m->cols - 1;
        m_cursor_clamp(m);
    }
    if (m->irm)
        m_ich(m, width);
    m_cursor_clamp(m);
    if (m->col >= m->cols)
        m->col = m->cols - 1;

    m_stamp(m, m->row);
    MRow *r = &m->grid[m->row];
    int c = m->col;
    int old_width = r->cells[c].width;
    if (len == 1 && text[0] == ' ')
        m_cell_blank(&r->cells[c]);
    else
        m_cell_text(&r->cells[c], text, len, width);
    if (c == m->cols - 1)
        r->wrap = false;
    if (width == 2 && c + 1 < m->cols) {
        m_cell_cont(&r->cells[c + 1]);
        if (c + 1 == m->cols - 1)
            r->wrap = false;
    } else if (width == 1 && old_width == 2 && c + 1 < m->cols) {
        /* A narrow print over a wide lead blanks the stranded
         * continuation. */
        m_cell_blank(&r->cells[c + 1]);
        if (c + 1 == m->cols - 1)
            r->wrap = false;
    }

    if (c + width >= m->cols)
        m->col = m->decawm ? m->cols : m->cols - 1;
    else
        m->col = c + width;
}

static void m_print_char(Model *m, char ch)
{
    m_print_cell(m, &ch, 1, 1);
}

/* Print the wide fixture through the same rules, including the eager
 * wrap when two cells no longer fit. */
static void m_print_wide(Model *m)
{
    m_print_cell(m, WIDE_UTF8, WIDE_UTF8_LEN, 2);
}

/* The model's join predicate: the two-sided handshake. */
static bool m_continues(Model *m, int u)
{
    const MRow *prev = m_row(m, u - 1);
    const MRow *cur = m_row(m, u);
    if (!prev || !cur)
        return false;
    return prev->wrap && prev->line != 0 && prev->line == cur->line;
}

/* ------------------------------------------------------------------ */
/* Text extraction models                                             */
/* ------------------------------------------------------------------ */

static size_t put_bytes(char *out, size_t len, size_t cap, const char *s,
                        size_t n)
{
    if (len + n >= cap)
        return len;
    memcpy(out + len, s, n);
    return len + n;
}

/* cfr_row_text(): width-0 cells (holes and continuations) are skipped;
 * blanks flatten to spaces; a printable cell contributes its cluster's
 * bytes at the row's own width. */
static size_t m_row_text(Model *m, int u, char *out, size_t cap)
{
    const MRow *r = m_row(m, u);
    if (!r || cap == 0)
        return 0;
    size_t len = 0;
    for (int c = 0; c < r->cols; ++c) {
        switch (r->cells[c].kind) {
        case MC_HOLE:
        case MC_CONT:
            break;
        case MC_BLANK:
            len = put_bytes(out, len, cap, " ", 1);
            break;
        case MC_CELL:
            len = put_bytes(out, len, cap, r->cells[c].text,
                            r->cells[c].len);
            break;
        }
    }
    out[len < cap ? len : cap - 1] = '\0';
    return len;
}

/* cfr_selection_get_text() for the whole unified buffer, selecting every
 * cell from (u0, 0) to (u1, cols-1). The engine's rules:
 *   - cp == 0 (holes and continuations) append a space, one column;
 *   - a blank (0x20) appends a space, one column;
 *   - a char appends its bytes and marks the row's last non-blank;
 *   - a wide lead appends its bytes and skips its continuation column;
 *   - a hard break trims the row's trailing blanks and appends '\n';
 *   - a soft join appends nothing between the rows.
 */
static size_t m_selection_text(Model *m, int u0, int u1, char *out,
                               size_t cap)
{
    size_t len = 0;
    for (int u = u0; u <= u1; ++u) {
        const MRow *r = m_row(m, u);
        if (!r)
            continue;
        size_t last_nonblank = len;
        for (int c = 0; c < r->cols;) {
            switch (r->cells[c].kind) {
            case MC_HOLE:
            case MC_CONT:
                len = put_bytes(out, len, cap, " ", 1);
                c++;
                break;
            case MC_BLANK:
                len = put_bytes(out, len, cap, " ", 1);
                c++;
                break;
            case MC_CELL:
                len = put_bytes(out, len, cap, r->cells[c].text,
                                r->cells[c].len);
                last_nonblank = len;
                c += r->cells[c].width;
                break;
            }
        }
        bool joins_next = u < u1 && m_continues(m, u + 1);
        if (!joins_next) {
            len = last_nonblank;
            if (u < u1)
                len = put_bytes(out, len, cap, "\n", 1);
        }
    }
    out[len < cap ? len : cap - 1] = '\0';
    return len;
}

/* ------------------------------------------------------------------ */
/* Differential check                                                 */
/* ------------------------------------------------------------------ */

static bool check_failed;
static bool strict_check;
static int trace_from = -1;

static void fuzz_report(const char *what, int u, const char *model,
                        const char *engine, const char *op)
{
    if (check_failed)
        return;
    check_failed = true;
    fprintf(stderr, "  MISMATCH (%s) unified row %d\n", what, u);
    fprintf(stderr, "    op:     %s\n", op);
    fprintf(stderr, "    model:  \"%s\"\n", model);
    fprintf(stderr, "    engine: \"%s\"\n", engine);
}

static void fuzz_dump_join(CfrTerm *vt, Model *m, int u)
{
    int pr = 0, cr = 0;
    const CfrPage *pp = cfr_row_page(vt, u - 1, &pr);
    const CfrPage *cp = cfr_row_page(vt, u, &cr);
    const MRow *mp = m_row(m, u - 1);
    const MRow *mc = m_row(m, u);
    fprintf(stderr,
            "  model:  prev wrap=%d line=%u  cur line=%u\n",
            mp ? (int)mp->wrap : -1, mp ? mp->line : 0,
            mc ? mc->line : 0);
    if (pp && cp) {
        const CfrCell *margin =
            &pp->cells[(size_t)pr * pp->cols + (pp->cols - 1)];
        fprintf(stderr,
                "  engine: prev margin_wrap=%d line=%u  cur line=%u\n",
                (margin->flags & CFR_CELL_WRAPLINE) != 0u,
                pp->lineage[pr], cp->lineage[cr]);
    }
}

static bool fuzz_verify(CfrTerm *vt, Model *m, const char *op)
{
    /* The cursor is user-visible state and every text comparison below
     * depends on it, so check it on every run (strict mode adds the
     * implementation-detail fields). */
    if (vt->cursor.row != m->row || vt->cursor.col != m->col) {
        char mb[32], eb[32];
        snprintf(mb, sizeof(mb), "%d,%d", m->row, m->col);
        snprintf(eb, sizeof(eb), "%d,%d", vt->cursor.row, vt->cursor.col);
        fuzz_report("cursor", 0, mb, eb, op);
        return false;
    }

    int sb = cfr_get_scrollback_lines(vt);
    if (sb != m->hist_len) {
        char mb[32], eb[32];
        snprintf(mb, sizeof(mb), "%d", m->hist_len);
        snprintf(eb, sizeof(eb), "%d", sb);
        fuzz_report("scrollback depth", 0, mb, eb, op);
        return false;
    }

    for (int u = -m->hist_len; u <= m->rows - 1; ++u) {
        bool mc = m_continues(m, u);
        bool ec = cfr_row_is_continuation(vt, u);
        if (mc != ec) {
            fuzz_report("join", u, mc ? "continues" : "severed",
                        ec ? "continues" : "severed", op);
            fuzz_dump_join(vt, m, u);
            return false;
        }

        if (strict_check) {
            /* Debug oracle: the model's row field should match the
             * engine's margin cell and lineage exactly. Exact id values
             * are not part of the spec — this mode is for diagnosing a
             * first divergence, not a correctness requirement. */
            const MRow *mr = m_row(m, u);
            int pr = 0;
            const CfrPage *pp = cfr_row_page(vt, u, &pr);
            if (mr && pp) {
                const CfrCell *margin =
                    &pp->cells[(size_t)pr * pp->cols + (pp->cols - 1)];
                bool ew = (margin->flags & CFR_CELL_WRAPLINE) != 0u;
                if (mr->wrap != ew) {
                    fuzz_report("wrap edge", u,
                                mr->wrap ? "set" : "clear",
                                ew ? "set" : "clear", op);
                    return false;
                }
                if (mr->line != pp->lineage[pr]) {
                    char mb[32], eb[32];
                    snprintf(mb, sizeof(mb), "%u", mr->line);
                    snprintf(eb, sizeof(eb), "%u", pp->lineage[pr]);
                    fuzz_report("lineage", u, mb, eb, op);
                    return false;
                }
            }
        }

        char mt[TEXT_CAP], et[TEXT_CAP];
        m_row_text(m, u, mt, sizeof(mt));
        cfr_row_text(vt, u, et, sizeof(et));
        if (strcmp(mt, et) != 0) {
            fuzz_report("row text", u, mt, et, op);
            return false;
        }
    }

    /* Whole-buffer extraction through the selection API. */
    int u0 = -m->hist_len;
    int u1 = m->rows - 1;
    cfr_selection_start(vt, u0, 0, CFR_SEL_CHAR);
    cfr_selection_update(vt, u1, m->cols - 1);
    char *got = cfr_selection_get_text(vt);
    cfr_selection_clear(vt);
    if (!got) {
        fuzz_report("selection text", 0, "text", "(null)", op);
        return false;
    }
    char want[TEXT_CAP];
    m_selection_text(m, u0, u1, want, sizeof(want));
    bool ok = strcmp(want, got) == 0;
    if (!ok)
        fuzz_report("selection text", 0, want, got, op);
    free(got);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Op stream                                                          */
/* ------------------------------------------------------------------ */

static uint64_t rng_next(uint64_t *s)
{
    *s += 0x9E3779B97F4A7C15ULL;
    uint64_t z = *s;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static unsigned rng_below(uint64_t *s, unsigned n)
{
    return (unsigned)(rng_next(s) % n);
}

static const char ASCII_POOL[] =
    "abcdefghijklmnopqrstuvwxyz0123456789 _-./:";

/* Apply one random op to the model and the engine. Returns false when
 * the op is not applicable to the model (never happens today). */
/* Presentation-selector and combining clusters. Widths follow
 * cfr_cluster_width(): VS16 always widens to 2, VS15 narrows only where
 * UTS #51 has a text glyph (☺ yes, CJK 一 no), combining marks do not
 * widen, and a ZWJ sequence takes the base's width. */
typedef struct
{
    const char *text;
    size_t len;
    int width;
    const char *name;
} ClusterFixture;

static const ClusterFixture CLUSTERS[] = {
    /* Non-ASCII bases only: an ASCII base is committed by the ASCII
     * run fast path before the selector arrives, so the engine's
     * last-cell re-attachment (§3.4) handles it, not commit_cluster's
     * one-shot path this model mirrors. Stray joiners stay covered by
     * test_cfr_wrap_width. */
    { "\xE2\x98\xBA\xEF\xB8\x8F", 6, 2, "smile+VS16" },
    { "\xE2\x98\xBA\xEF\xB8\x8E", 6, 1, "smile+VS15" },
    { WIDE_UTF8 "\xEF\xB8\x8E", 6, 2, "CJK+VS15" },
    { "\xE2\x98\xBA\xCC\x81", 5, 1, "smile+acute" },
    { "\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA9", 11, 2,
      "woman+ZWJ+woman" },
};

/* DECSTBM: an invalid region (top >= bottom) is ignored and leaves the
 * cursor alone; a valid one homes the cursor (to the region's top with
 * DECOM on, to row 0 otherwise). */
static void m_decstbm(Model *m, int top, int bottom)
{
    if (top < 0)
        top = 0;
    if (bottom >= m->rows)
        bottom = m->rows - 1;
    if (top >= bottom)
        return;
    m->scroll_top = top;
    m->scroll_bottom = bottom;
    m_cursor_set(m, m->decom ? top : 0, 0);
}

static int m_saved_idx(const Model *m)
{
    return m->in_alt ? 1 : 0;
}

static void m_swap_grids(Model *m)
{
    MRow tmp[ROWS_MAX];
    memcpy(tmp, m->grid, sizeof(tmp));
    memcpy(m->grid, m->altgrid, sizeof(tmp));
    memcpy(m->altgrid, tmp, sizeof(tmp));
}

static void m_reflow(Model *m, int old_rows, int old_cols, int new_rows,
                     int new_cols);
static void m_clamp_grid(Model *m, int old_rows, int old_cols,
                         int new_rows, int new_cols);

/* DECSET/DECRST 1049 (save cursor + fresh alt screen + home) and
 * 47/1047 (raw alt: no save, no home). Both enter on a brand-new page,
 * so the alt grid is blank every time; the main grid and the shared
 * scrollback are untouched. */
static void m_set_altscreen(Model *m, bool on, bool save_cursor)
{
    if (on == m->in_alt)
        return;
    if (on) {
        if (save_cursor) {
            m->saved_row[0] = m->row;
            m->saved_col[0] = m->col;
        }
        for (int r = 0; r < m->rows; ++r)
            m_clear_holes(&m->altgrid[r], m->cols);
        m_swap_grids(m);
        m->in_alt = true;
        if (save_cursor)
            m_cursor_set(m, 0, 0);
    } else {
        m_swap_grids(m);
        m->in_alt = false;
        /* The main page kept its dimensions while the alt screen was
         * resized. modes.c rolls vt->rows/cols back to the page's dims
         * and runs the normal resize path at the current size, using the
         * alt cursor, before restoring the 1049 register. */
        if (m->main_rows != m->rows || m->main_cols != m->cols) {
            if (m->reflow_on)
                m_reflow(m, m->main_rows, m->main_cols, m->rows, m->cols);
            else
                m_clamp_grid(m, m->main_rows, m->main_cols, m->rows,
                             m->cols);
        }
        m->main_rows = m->rows;
        m->main_cols = m->cols;
        if (save_cursor) {
            m->row = m->saved_row[0];
            m->col = m->saved_col[0];
        }
    }
}

/* ------------------------------------------------------------------ */
/* Reflow model (resize)                                              */
/* ------------------------------------------------------------------ */

/* Clamp-only resize (cfr_resize_clamp): copy the overlapping top-left
 * rectangle into a fresh page, leave the new cells as holes, keep each
 * copied row's lineage, and preserve the wrap edge only across a
 * height-only resize. A width change cannot carry the edge: the old
 * margin cell is not the new margin, and the engine clears the bit when
 * a copied cell lands inside the row. */
static void m_clamp_grid(Model *m, int old_rows, int old_cols,
                         int new_rows, int new_cols)
{
    if (new_rows <= 0 || new_cols <= 0)
        return;
    if (new_rows == old_rows && new_cols == old_cols)
        return;

    MRow newgrid[ROWS_MAX];
    memset(newgrid, 0, sizeof(newgrid));
    for (int r = 0; r < new_rows; ++r) {
        MRow *dst = &newgrid[r];
        dst->cols = new_cols;
        if (r >= old_rows)
            continue;
        const MRow *src = &m->grid[r];
        int copy_cols = new_cols < old_cols ? new_cols : old_cols;
        for (int c = 0; c < copy_cols; ++c)
            dst->cells[c] = src->cells[c];
        dst->line = src->line;
        dst->wrap = (new_cols == old_cols) ? src->wrap : false;
    }
    memcpy(m->grid, newgrid, sizeof(m->grid));
    m->rows = new_rows;
    m->cols = new_cols;
    m->scroll_top = 0;
    m->scroll_bottom = new_rows - 1;
    m_cursor_set(m, m->row, m->col);
}

#define FLAT_MAX 4096

typedef struct
{
    uint32_t start, end;
    uint32_t line;
    bool wraps;
} MFlatRow;

typedef struct
{
    uint32_t end;
    uint32_t line;
} MFlatLine;

typedef struct
{
    MCell cells[FLAT_MAX];
    int count;
    MFlatRow rows[FLAT_MAX];
    int rows_count;
    MFlatLine lines[FLAT_MAX];
    int lines_count;
    int prefix;
    int cursor_offset;
    int cursor_line;
    int cursor_row, cursor_col;
} MReflow;

/* Lay one rewrapped row into `dst`: the cells were collected at their
 * old widths and are re-emitted at the new width. */
static void m_layout_row(const MReflow *R, const MFlatRow *fr, MRow *dst,
                         int cols)
{
    m_clear_holes(dst, cols);
    dst->line = fr->line;
    int col = 0;
    for (uint32_t k = fr->start; k < fr->end && col < cols; ++k) {
        MCell cell = R->cells[k];
        int w = cell.width;
        if (col + w > cols)
            break;
        dst->cells[col] = cell;
        if (w == 2 && col + 1 < cols)
            m_cell_cont(&dst->cells[col + 1]);
        col += w;
    }
    dst->wrap = fr->wraps;
}

/* The spec's reflow: collect the logical line that straddles the top
 * scrollback edge (up to CFR_REFLOW_PREFIX_ROWS), collect the grid,
 * group rows by the join predicate, trim blank trailing lines, rewrap
 * at the new width, and install -- oldest rows to scrollback, newest in
 * the grid. Mirrors cfr_reflow() phase for phase; the model still has
 * no pages and no intern tables. */
static void m_reflow(Model *m, int old_rows, int old_cols, int new_rows,
                     int new_cols)
{
    if (new_rows <= 0 || new_cols <= 0)
        return;
    if (new_rows == old_rows && new_cols == old_cols)
        return;

    MReflow *R = calloc(1, sizeof(*R));
    if (!R)
        return;
    R->cursor_offset = -1;
    R->cursor_line = -1;
    /* Collection walks the active page at its own dimensions. On an
     * alt-screen exit the old main page can be smaller or wider than the
     * terminal's current geometry, and m_row()/m_continues() must bound
     * positive rows by the page, not by vt->rows/cols. */
    m->rows = old_rows;
    m->cols = old_cols;

    /* Phase 1a: the history rows of the line that straddles the top
     * edge. The chain is checked with the same unified-row predicate
     * the engine uses. */
    if (m->hist_len > 0 && m_continues(m, 0)) {
        int rows = 1;
        while (rows < m->hist_len && rows < CFR_REFLOW_PREFIX_ROWS &&
               m_continues(m, -rows))
            rows++;
        R->prefix = rows;
        for (int sr = rows - 1; sr >= 0; --sr) {
            const MRow *row = &m->hist[m->hist_len - sr - 1];
            for (int c = 0; c < row->cols; ++c) {
                if (row->cells[c].width == 0)
                    continue;
                if (R->count < FLAT_MAX)
                    R->cells[R->count++] = row->cells[c];
            }
        }
    }

    /* Phase 1b: the grid, one logical line per run of joined rows. */
    int line_start = 0;
    for (int r = 0; r < old_rows; ++r) {
        int row_start = R->count;
        int cursor_in_row = -1;
        for (int c = 0; c < old_cols; ++c) {
            if (r == m->row && c == m->col)
                cursor_in_row = R->count - row_start;
            if (m->grid[r].cells[c].width == 0)
                continue;
            if (R->count < FLAT_MAX)
                R->cells[R->count++] = m->grid[r].cells[c];
        }
        if (r == m->row) {
            if (cursor_in_row < 0)
                cursor_in_row = R->count - row_start;
            R->cursor_offset = row_start + cursor_in_row;
        }
        bool wraps = (r + 1 < old_rows) && m_continues(m, r + 1);
        if (!wraps) {
            uint32_t id = m->grid[line_start].line;
            if (id == 0)
                id = m_next_line(m);
            if (R->lines_count < FLAT_MAX) {
                R->lines[R->lines_count].end = (uint32_t)R->count;
                R->lines[R->lines_count].line = id;
                if (m->row >= line_start && m->row <= r)
                    R->cursor_line = R->lines_count;
                R->lines_count++;
            }
            line_start = r + 1;
        }
    }
    if (R->cursor_offset < 0)
        R->cursor_offset = R->count;
    if (R->cursor_line < 0)
        R->cursor_line = R->lines_count - 1;

    /* Trim trailing blank lines that do not host the cursor (the same
     * blank test: a hole or a space). */
    while (R->lines_count > 0) {
        int end = (int)R->lines[R->lines_count - 1].end;
        int start = R->lines_count > 1
                        ? (int)R->lines[R->lines_count - 2].end
                        : 0;
        bool content = false;
        for (int k = start; k < end; ++k) {
            if (R->cells[k].kind != MC_HOLE &&
                R->cells[k].kind != MC_BLANK) {
                content = true;
                break;
            }
        }
        if (content || R->cursor_line == R->lines_count - 1)
            break;
        R->lines_count--;
        R->count = start;
    }

    /* Phase 2: rewrap each logical line at the new width. */
    int placed = 0;
    line_start = 0;
    for (int li = 0; li < R->lines_count; ++li) {
        const MFlatLine *line = &R->lines[li];
        bool is_cursor = (li == R->cursor_line);
        int i = line_start;
        do {
            int row_start = i;
            int row_used = 0;
            while (i < (int)line->end) {
                int w = R->cells[i].width;
                if (w <= 0) {
                    ++i;
                    continue;
                }
                if (row_used + w > new_cols)
                    break;
                row_used += w;
                ++i;
            }
            if (i == row_start && i < (int)line->end)
                ++i; /* a cluster wider than the grid: force progress */
            if (!placed && is_cursor) {
                int off = R->cursor_offset;
                if (off >= row_start && off < i) {
                    int w = 0;
                    for (int k = row_start; k < off; ++k)
                        w += R->cells[k].width;
                    R->cursor_row = R->rows_count;
                    R->cursor_col = w;
                    placed = 1;
                } else if (off == i) {
                    R->cursor_row = R->rows_count;
                    R->cursor_col =
                        row_used >= new_cols ? new_cols - 1 : row_used;
                    placed = 1;
                }
            }
            if (R->rows_count < FLAT_MAX) {
                R->rows[R->rows_count].start = (uint32_t)row_start;
                R->rows[R->rows_count].end = (uint32_t)i;
                R->rows[R->rows_count].line = line->line;
                R->rows[R->rows_count].wraps = i < (int)line->end;
                R->rows_count++;
            }
        } while (i < (int)line->end);
        line_start = (int)line->end;
    }
    if (!placed) {
        R->cursor_row = R->rows_count > 0 ? R->rows_count - 1 : 0;
        R->cursor_col = 0;
    }

    /* Phase 3: install. The oldest rows go to scrollback, the newest
     * land in the grid; the prefix rows leave the ring. */
    int total = R->rows_count;
    int sb_count = total > new_rows ? total - new_rows : 0;
    int visible = total - sb_count;
    if (R->prefix > 0)
        m->hist_len -= R->prefix;

    MRow newgrid[ROWS_MAX];
    for (int r = 0; r < ROWS_MAX; ++r)
        m_clear_holes(&newgrid[r], new_cols);
    for (int i = 0; i < visible; ++i)
        m_layout_row(R, &R->rows[sb_count + i], &newgrid[i], new_cols);
    for (int i = 0; i < sb_count; ++i) {
        MRow tmp;
        m_layout_row(R, &R->rows[i], &tmp, new_cols);
        m_hist_push(m, &tmp);
    }

    memcpy(m->grid, newgrid, sizeof(m->grid));
    m->rows = new_rows;
    m->cols = new_cols;
    m->scroll_top = 0;
    m->scroll_bottom = new_rows - 1;

    int target_row = R->cursor_row - sb_count;
    if (target_row < 0) {
        m->row = 0;
        m->col = 0;
    } else {
        m->row = target_row < new_rows ? target_row : new_rows - 1;
        m->col = R->cursor_col < new_cols ? R->cursor_col : new_cols - 1;
        if (m->col < 0)
            m->col = 0;
    }
    free(R);
}

/* Apply one random op to the model and the engine. Returns false when
 * the op is not applicable to the model (never happens today). */
static bool run_op(CfrTerm *vt, Model *m, uint64_t *rng, char *op,
                   size_t op_cap)
{
    unsigned pick = rng_below(rng, 1000);
    char seq[128];

    if (pick < 210) {
        /* Print a run of printable ASCII. */
        int n = 1 + (int)rng_below(rng, 8);
        for (int i = 0; i < n; ++i)
            seq[i] = ASCII_POOL[rng_below(rng, sizeof(ASCII_POOL) - 1)];
        seq[n] = '\0';
        for (int i = 0; i < n; ++i)
            m_print_char(m, seq[i]);
        snprintf(op, op_cap, "print \"%s\"", seq);
        /* The SGR reset flushes a pending cluster (it never touches the
         * grid), so the engine's state is comparable after the write. */
        snprintf(seq + n, sizeof(seq) - (size_t)n, "\x1b[0m");
        cfr_input_write(vt, (const uint8_t *)seq, strlen(seq));
        return true;
    }
    if (pick < 240) {
        m_print_wide(m);
        snprintf(op, op_cap, "print wide");
        strcpy(seq, WIDE_UTF8 "\x1b[0m");
        cfr_input_write(vt, (const uint8_t *)seq, strlen(seq));
        return true;
    }
    if (pick < 300) {
        const ClusterFixture *cf =
            &CLUSTERS[rng_below(rng, sizeof(CLUSTERS) /
                                         sizeof(CLUSTERS[0]))];
        m_print_cell(m, cf->text, cf->len, cf->width);
        snprintf(op, op_cap, "print cluster %s", cf->name);
        memcpy(seq, cf->text, cf->len);
        memcpy(seq + cf->len, "\x1b[0m", 4);
        cfr_input_write(vt, (const uint8_t *)seq, cf->len + 4);
        return true;
    }
    if (pick < 340) {
        m_cursor_set(m, m->row, 0);
        snprintf(op, op_cap, "CR");
        strcpy(seq, "\r\x1b[0m");
    } else if (pick < 420) {
        m_lf(m);
        snprintf(op, op_cap, "LF");
        strcpy(seq, "\n\x1b[0m");
    } else if (pick < 450) {
        m_cursor_set(m, m->row, m->col - 1);
        snprintf(op, op_cap, "BS");
        strcpy(seq, "\b\x1b[0m");
    } else if (pick < 480) {
        m_tab(m);
        snprintf(op, op_cap, "HT");
        strcpy(seq, "\t\x1b[0m");
    } else if (pick < 510) {
        m_ri(m);
        snprintf(op, op_cap, "RI");
        strcpy(seq, "\x1bM\x1b[0m");
    } else if (pick < 555) {
        int r = (int)rng_below(rng, (unsigned)(m->rows * 2));
        int c = (int)rng_below(rng, (unsigned)m->cols);
        m_cursor_to_origin(m, r, c);
        snprintf(op, op_cap, "CUP %d;%d", r + 1, c + 1);
        snprintf(seq, sizeof(seq), "\x1b[%d;%dH\x1b[0m", r + 1, c + 1);
    } else if (pick < 585) {
        int c = (int)rng_below(rng, (unsigned)m->cols);
        m_cursor_set(m, m->row, c);
        snprintf(op, op_cap, "CHA %d", c + 1);
        snprintf(seq, sizeof(seq), "\x1b[%dG\x1b[0m", c + 1);
    } else if (pick < 615) {
        int r = (int)rng_below(rng, (unsigned)(m->rows * 2));
        /* VPA is an explicit cursor move: the column, including a
         * pending phantom, is clamped like CUP's. */
        m_cursor_to_origin(m, r, m->col);
        snprintf(op, op_cap, "VPA %d", r + 1);
        snprintf(seq, sizeof(seq), "\x1b[%dd\x1b[0m", r + 1);
    } else if (pick < 645) {
        int n = 1 + (int)rng_below(rng, (unsigned)m->rows);
        m_scroll_up(m, n);
        snprintf(op, op_cap, "SU %d", n);
        snprintf(seq, sizeof(seq), "\x1b[%dS\x1b[0m", n);
    } else if (pick < 675) {
        int n = 1 + (int)rng_below(rng, (unsigned)m->rows);
        m_scroll_down(m, n);
        snprintf(op, op_cap, "SD %d", n);
        snprintf(seq, sizeof(seq), "\x1b[%dT\x1b[0m", n);
    } else if (pick < 705) {
        int n = 1 + (int)rng_below(rng, (unsigned)m->rows);
        m_il(m, n);
        snprintf(op, op_cap, "IL %d", n);
        snprintf(seq, sizeof(seq), "\x1b[%dL\x1b[0m", n);
    } else if (pick < 735) {
        int n = 1 + (int)rng_below(rng, (unsigned)m->rows);
        m_dl(m, n);
        snprintf(op, op_cap, "DL %d", n);
        snprintf(seq, sizeof(seq), "\x1b[%dM\x1b[0m", n);
    } else if (pick < 765) {
        int n = 1 + (int)rng_below(rng, (unsigned)m->cols);
        m_ich(m, n);
        snprintf(op, op_cap, "ICH %d", n);
        snprintf(seq, sizeof(seq), "\x1b[%d@\x1b[0m", n);
    } else if (pick < 785) {
        int n = 1 + (int)rng_below(rng, (unsigned)m->cols);
        m_dch(m, n);
        snprintf(op, op_cap, "DCH %d", n);
        snprintf(seq, sizeof(seq), "\x1b[%dP\x1b[0m", n);
    } else if (pick < 805) {
        int n = 1 + (int)rng_below(rng, (unsigned)m->cols);
        m_ech(m, n);
        snprintf(op, op_cap, "ECH %d", n);
        snprintf(seq, sizeof(seq), "\x1b[%dX\x1b[0m", n);
    } else if (pick < 825) {
        int mode = (int)rng_below(rng, 3);
        m_el(m, mode);
        snprintf(op, op_cap, "EL %d", mode);
        snprintf(seq, sizeof(seq), "\x1b[%dK\x1b[0m", mode);
    } else if (pick < 845) {
        int mode = (int)rng_below(rng, 4);
        m_ed(m, mode);
        snprintf(op, op_cap, "ED %d", mode);
        snprintf(seq, sizeof(seq), "\x1b[%dJ\x1b[0m", mode);
    } else if (pick < 865) {
        int top, bottom;
        if (rng_below(rng, 4) == 0) {
            top = 0;
            bottom = m->rows - 1;
        } else {
            top = (int)rng_below(rng, (unsigned)m->rows);
            bottom = (int)rng_below(rng, (unsigned)m->rows);
        }
        m_decstbm(m, top, bottom);
        snprintf(op, op_cap, "DECSTBM %d;%d", top + 1, bottom + 1);
        snprintf(seq, sizeof(seq), "\x1b[%d;%dr\x1b[0m", top + 1,
                 bottom + 1);
    } else if (pick < 880) {
        m->decom = !m->decom;
        m_cursor_set(m, m->decom ? m->scroll_top : 0, 0);
        snprintf(op, op_cap, "DECOM %s", m->decom ? "on" : "off");
        strcpy(seq, m->decom ? "\x1b[?6h\x1b[0m" : "\x1b[?6l\x1b[0m");
    } else if (pick < 890) {
        m->decawm = !m->decawm;
        if (!m->decawm && m->col >= m->cols)
            m->col = m->cols - 1;
        snprintf(op, op_cap, "DECAWM %s", m->decawm ? "on" : "off");
        strcpy(seq, m->decawm ? "\x1b[?7h\x1b[0m" : "\x1b[?7l\x1b[0m");
    } else if (pick < 900) {
        m->irm = !m->irm;
        snprintf(op, op_cap, "IRM %s", m->irm ? "on" : "off");
        strcpy(seq, m->irm ? "\x1b[4h\x1b[0m" : "\x1b[4l\x1b[0m");
    } else if (pick < 918) {
        bool on = !m->in_alt;
        m_set_altscreen(m, on, true);
        snprintf(op, op_cap, "alt 1049 %s", on ? "on" : "off");
        snprintf(seq, sizeof(seq), "\x1b[?1049%c\x1b[0m", on ? 'h' : 'l');
    } else if (pick < 936) {
        bool on = !m->in_alt;
        bool raw47 = rng_below(rng, 2) == 0;
        m_set_altscreen(m, on, false);
        snprintf(op, op_cap, "alt %s %s", raw47 ? "47" : "1047",
                 on ? "on" : "off");
        snprintf(seq, sizeof(seq), "\x1b[?%d%c\x1b[0m", raw47 ? 47 : 1047,
                 on ? 'h' : 'l');
    } else if (pick < 958) {
        int si = m_saved_idx(m);
        m->saved_row[si] = m->row;
        m->saved_col[si] = m->col;
        snprintf(op, op_cap, "DECSC");
        strcpy(seq, "\x1b"
                    "7\x1b[0m");
    } else if (pick < 978) {
        int si = m_saved_idx(m);
        m->row = m->saved_row[si];
        m->col = m->saved_col[si];
        snprintf(op, op_cap, "DECRC");
        strcpy(seq, "\x1b"
                    "8\x1b[0m");
    } else {
        /* Host API calls, not escape sequences. Flip the reflow setting
         * occasionally so one run covers both resize paths. */
        if (rng_below(rng, 10) == 0) {
            m->reflow_on = !m->reflow_on;
            cfr_set_reflow(vt, m->reflow_on);
            snprintf(op, op_cap, "reflow %s", m->reflow_on ? "on" : "off");
            return true;
        }
        int nr = 1 + (int)rng_below(rng, ROWS_MAX);
        int nc = 1 + (int)rng_below(rng, COLS_MAX);
        snprintf(op, op_cap, "resize %dx%d", nr, nc);
        if (m->in_alt) {
            /* cfr_reflow() clamps on the alt screen regardless of the
             * reflow setting; the main page keeps its old dimensions
             * until the exit-time catch-up. */
            m_clamp_grid(m, m->rows, m->cols, nr, nc);
        } else if (m->reflow_on) {
            m_reflow(m, m->rows, m->cols, nr, nc);
            m->main_rows = nr;
            m->main_cols = nc;
        } else {
            m_clamp_grid(m, m->rows, m->cols, nr, nc);
            m->main_rows = nr;
            m->main_cols = nc;
        }
        cfr_resize(vt, nr, nc);
        return true;
    }

    cfr_input_write(vt, (const uint8_t *)seq, strlen(seq));
    return true;
}

static void fuzz_dump_grid(CfrTerm *vt, Model *m)
{
    fprintf(stderr,
            "  cursor: model %d,%d  engine %d,%d | region: model %d..%d  "
            "engine %d..%d\n",
            m->row, m->col, vt->cursor.row, vt->cursor.col, m->scroll_top,
            m->scroll_bottom, vt->scroll_top, vt->scroll_bottom);
    for (int u = -m->hist_len; u <= m->rows - 1; ++u) {
        char mt[TEXT_CAP], et[TEXT_CAP];
        m_row_text(m, u, mt, sizeof(mt));
        cfr_row_text(vt, u, et, sizeof(et));
        int pr = 0;
        const CfrPage *pp = cfr_row_page(vt, u, &pr);
        bool ew = false;
        uint32_t el = 0;
        if (pp) {
            const CfrCell *margin =
                &pp->cells[(size_t)pr * pp->cols + (pp->cols - 1)];
            ew = (margin->flags & CFR_CELL_WRAPLINE) != 0u;
            el = pp->lineage[pr];
        }
        const MRow *mr = m_row(m, u);
        fprintf(stderr,
                "  row %3d: model [%s] wrap=%d line=%u | engine [%s] "
                "wrap=%d line=%u\n",
                u, mt, mr ? (int)mr->wrap : -1, mr ? mr->line : 0, et,
                (int)ew, el);
        if (u >= 0 && u < m->rows && mr && pp) {
            fprintf(stderr, "           model kinds:");
            for (int c = 0; c < mr->cols; ++c)
                fprintf(stderr, " %d:%d", mr->cells[c].kind,
                        (int)mr->cells[c].width);
            fprintf(stderr, "\n           engine cp:  ");
            const CfrCell *engine_row =
                &pp->cells[(size_t)pr * (size_t)pp->cols];
            for (int c = 0; c < m->cols; ++c)
                fprintf(stderr, " %02x/%d", (unsigned)engine_row[c].cp,
                        (int)engine_row[c].width);
            fprintf(stderr, "\n");
        }
    }
}

static bool fuzz_run(int rows, int cols, int sb_cap, bool reflow,
                     uint64_t seed, int iters)
{
    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.rows = rows;
    cfg.cols = cols;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 6;
    cfg.scrollback = sb_cap;
    cfg.reflow = reflow;
    CfrTerm *vt = cfr_new(&cfg);
    if (!vt)
        return false;

    /* The grid is allocated lazily, and an edit op on a grid-less
     * terminal is a no-op (while ED allocates and blanks it). Start every
     * run from the same, fully materialized, erased screen so the model
     * need not carry that lazy-allocation quirk. */
    cfr_input_write(vt, (const uint8_t *)"\x1b[2J", 4);

    Model m;
    memset(&m, 0, sizeof(m));
    m.rows = rows;
    m.cols = cols;
    m.main_rows = rows;
    m.main_cols = cols;
    m.sb_cap = sb_cap;
    m.decawm = true;
    m.reflow_on = reflow;
    m.scroll_top = 0;
    m.scroll_bottom = rows - 1;
    /* cfr_new() seeds both saved-cursor registers from the home cursor. */
    m.saved_row[0] = m.saved_row[1] = 0;
    m.saved_col[0] = m.saved_col[1] = 0;
    for (int r = 0; r < rows; ++r)
        m_clear_blanks(&m.grid[r], cols);

    /* DECRC with no DECSC restores the initial register, which cfr_new
     * seeds from the home cursor. */

    bool ok = true;
    char op[160] = "(initial)";
    char recent[12][160];
    int nrecent = 0;
    if (trace_from >= 0)
        fprintf(stderr, "=== run seed=%llu %dx%d sb=%d ===\n",
                (unsigned long long)seed, rows, cols, sb_cap);
    if (!fuzz_verify(vt, &m, op))
        ok = false;

    for (int i = 0; ok && i < iters; ++i) {
        uint64_t rng =
            seed + (uint64_t)i * UINT64_C(0x9E3779B97F4A7C15);
        if (trace_from >= 0 && i == trace_from) {
            fprintf(stderr, "--- before op %d ---\n", i);
            fuzz_dump_grid(vt, &m);
        }
        run_op(vt, &m, &rng, op, sizeof(op));
        snprintf(recent[nrecent % 12], sizeof(recent[0]), "%s", op);
        nrecent++;
        if (trace_from >= 0 && i >= trace_from) {
            fprintf(stderr, "--- after op %d (%s) ---\n", i, op);
            fuzz_dump_grid(vt, &m);
        }
        if (!fuzz_verify(vt, &m, op)) {
            fprintf(stderr,
                    "  fuzz failure: seed=%llu op=%d config=%dx%d sb=%d\n",
                    (unsigned long long)seed, i, rows, cols, sb_cap);
            int first = nrecent > 12 ? nrecent - 12 : 0;
            fprintf(stderr, "  recent ops:\n");
            for (int k = first; k < nrecent; ++k)
                fprintf(stderr, "    [%d] %s\n", k,
                        recent[k % 12]);
            fuzz_dump_grid(vt, &m);
            ok = false;
        }
    }

    cfr_free(vt);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Tests                                                              */
/* ------------------------------------------------------------------ */

static int fuzz_iters(void)
{
    const char *s = getenv("COFFER_FUZZ_ITERS");
    int n = s ? atoi(s) : 0;
    return n > 0 ? n : 150;
}

static uint64_t fuzz_seed(void)
{
    const char *s = getenv("COFFER_FUZZ_SEED");
    return s ? strtoull(s, NULL, 0) : 0x5EEDu;
}

static bool run_config(int rows, int cols, int sb, bool reflow, int seeds)
{
    uint64_t base = fuzz_seed();
    int iters = fuzz_iters();
    for (int i = 0; i < seeds; ++i) {
        check_failed = false;
        if (!fuzz_run(rows, cols, sb, reflow,
                      base + (uint64_t)i * 0x10001u, iters))
            return false;
    }
    return true;
}

static void test_wrap_fuzz_small(void)
{
    ASSERT_TRUE(run_config(4, 3, 16, true, 8));
}

static void test_wrap_fuzz_narrow(void)
{
    ASSERT_TRUE(run_config(6, 5, 32, true, 8));
}

static void test_wrap_fuzz_wide(void)
{
    ASSERT_TRUE(run_config(8, 12, 64, true, 8));
}

static void test_wrap_fuzz_evicting(void)
{
    ASSERT_TRUE(run_config(5, 8, 5, true, 8));
}

static void test_wrap_fuzz_no_scrollback(void)
{
    ASSERT_TRUE(run_config(4, 6, 0, true, 8));
}

static void test_wrap_fuzz_clamp(void)
{
    ASSERT_TRUE(run_config(5, 6, 16, false, 8));
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);
    strict_check = getenv("COFFER_FUZZ_STRICT") != NULL;
    {
        const char *t = getenv("COFFER_FUZZ_TRACE");
        if (t)
            trace_from = atoi(t);
    }
    /* COFFER_FUZZ_ONLY=small|narrow|wide|evict|none|clamp narrows the
     * run. */
    const char *only = getenv("COFFER_FUZZ_ONLY");
    printf("Running wrapped-line differential fuzz:\n");

    if (!only || strcmp(only, "small") == 0)
        RUN_TEST(test_wrap_fuzz_small);
    if (!only || strcmp(only, "narrow") == 0)
        RUN_TEST(test_wrap_fuzz_narrow);
    if (!only || strcmp(only, "wide") == 0)
        RUN_TEST(test_wrap_fuzz_wide);
    if (!only || strcmp(only, "evict") == 0)
        RUN_TEST(test_wrap_fuzz_evicting);
    if (!only || strcmp(only, "none") == 0)
        RUN_TEST(test_wrap_fuzz_no_scrollback);
    if (!only || strcmp(only, "clamp") == 0)
        RUN_TEST(test_wrap_fuzz_clamp);

    TEST_SUMMARY();
}
