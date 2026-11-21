/* tests/test_cfr_wrap_fuzz.c — differential fuzz for soft wraps.
 *
 * A random stream of print/cursor/edit/scroll ops is applied to a CfrTerm
 * and to a naive line model: an array of rows, each with a row-level wrap
 * edge ("the margin cell still ends in a wrap") and a lineage id ("this
 * row is a fragment of logical line N"). After every op the test compares
 * the engine against the model:
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

#define ROWS_MAX 8
#define COLS_MAX 12
#define HIST_MAX 128
#define TEXT_CAP 8192

/* UTF-8 for U+4E00 (一), always two cells wide. */
#define WIDE_UTF8     "\xE4\xB8\xAD"
#define WIDE_UTF8_LEN 3

/* Cell kinds. The model keeps only what the wrap predicate and the two
 * text extractions need:
 *   HOLE  never written (engine: all-zero cell)
 *   BLANK erased or a printed space (engine: cp 0x20, width 1)
 *   CHAR  one printable ASCII cell
 *   WIDE  lead of a two-cell cluster
 *   CONT  its continuation (engine: width 0)
 */
enum
{
    MC_HOLE = 0,
    MC_BLANK,
    MC_CHAR,
    MC_WIDE,
    MC_CONT
};

typedef struct
{
    uint8_t kind;
    char ch;
} MCell;

typedef struct
{
    MCell cells[COLS_MAX];
    uint32_t line; /* 0 = blank/unwritten */
    bool wrap;     /* the margin cell carries the wrap edge */
} MRow;

typedef struct
{
    int rows, cols;
    int sb_cap;
    MRow grid[ROWS_MAX];
    MRow hist[HIST_MAX];
    int hist_len;
    int row, col; /* col may be cols: the deferred-wrap phantom */
    bool decawm;
    bool irm;
    int saved_row, saved_col;
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

/* Whole-row clears, matching the engine's two flavors: scroll-induced
 * erasure uses memset (holes), explicit erasure uses erase_cells
 * (spaces). Both reset the row's lineage and wrap edge. */
static void m_clear_holes(MRow *r, int cols)
{
    for (int c = 0; c < cols; ++c) {
        r->cells[c].kind = MC_HOLE;
        r->cells[c].ch = 0;
    }
    r->line = 0;
    r->wrap = false;
}

static void m_clear_blanks(MRow *r, int cols)
{
    for (int c = 0; c < cols; ++c) {
        r->cells[c].kind = MC_BLANK;
        r->cells[c].ch = 0;
    }
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

static void m_scroll_up(Model *m, int n)
{
    if (n <= 0)
        return;
    if (n > m->rows)
        n = m->rows;
    for (int i = 0; i < n; ++i)
        m_hist_push(m, &m->grid[i]);
    if (n < m->rows)
        memmove(&m->grid[0], &m->grid[n],
                (size_t)(m->rows - n) * sizeof(MRow));
    for (int i = m->rows - n; i < m->rows; ++i)
        m_clear_holes(&m->grid[i], m->cols);
}

static void m_scroll_down(Model *m, int n)
{
    if (n <= 0)
        return;
    if (n > m->rows)
        n = m->rows;
    if (n < m->rows)
        memmove(&m->grid[n], &m->grid[0],
                (size_t)(m->rows - n) * sizeof(MRow));
    for (int i = 0; i < n; ++i)
        m_clear_blanks(&m->grid[i], m->cols);
}

/* Resolve the phantom into a wrap. Mirrors cfr_wrap_commit(): stamp the
 * row being left, move to the next row (scrolling at the bottom), and
 * propagate the lineage id onto the row the print lands on. */
static void m_wrap_commit(Model *m)
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
    if (src == m->rows - 1) {
        m_scroll_up(m, 1);
        dst = src;
    } else {
        dst = src + 1;
    }
    m->grid[dst].line = id;
    m->row = dst;
    m->col = 0;
}

static void m_ich(Model *m, int count)
{
    if (count <= 0)
        return;
    if (m->col >= m->cols)
        m->col = m->cols - 1;
    int c = m->col;
    if (c < 0 || c >= m->cols)
        return;
    if (count > m->cols - c)
        count = m->cols - c;

    MRow *r = &m->grid[m->row];
    bool had_wrap = r->wrap;
    int move = m->cols - c - count;
    if (move > 0)
        memmove(&r->cells[c + count], &r->cells[c],
                (size_t)move * sizeof(MCell));
    for (int i = 0; i < count; ++i) {
        r->cells[c + i].kind = MC_BLANK;
        r->cells[c + i].ch = 0;
    }
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
    if (c < 0 || c >= m->cols)
        return;
    if (count > m->cols - c)
        count = m->cols - c;

    MRow *r = &m->grid[m->row];
    int move = m->cols - c - count;
    if (move > 0)
        memmove(&r->cells[c], &r->cells[c + count],
                (size_t)move * sizeof(MCell));
    for (int i = m->cols - count; i < m->cols; ++i) {
        r->cells[i].kind = MC_BLANK;
        r->cells[i].ch = 0;
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
    if (c < 0 || c >= m->cols)
        return;
    if (count > m->cols - c)
        count = m->cols - c;

    MRow *r = &m->grid[m->row];
    for (int i = 0; i < count; ++i) {
        r->cells[c + i].kind = MC_BLANK;
        r->cells[c + i].ch = 0;
    }
    if (c + count - 1 == m->cols - 1)
        r->wrap = false;
}

static void m_el(Model *m, int mode)
{
    if (m->col >= m->cols)
        m->col = m->cols - 1;
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
    for (int c = from; c < to; ++c) {
        r->cells[c].kind = MC_BLANK;
        r->cells[c].ch = 0;
    }
    if (to == m->cols)
        r->wrap = false;
}

static void m_ed(Model *m, int mode)
{
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
    int region = m->rows - row;
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
    int region = m->rows - row;
    if (count > region)
        count = region;
    int move = region - count;
    if (move > 0)
        memmove(&m->grid[row], &m->grid[row + count],
                (size_t)move * sizeof(MRow));
    for (int i = 0; i < count; ++i)
        m_clear_blanks(&m->grid[m->rows - count + i], m->cols);
    m->col = 0;
}

static void m_lf(Model *m)
{
    if (m->col >= m->cols)
        m->col = 0;
    if (m->row == m->rows - 1)
        m_scroll_up(m, 1);
    else if (m->row < m->rows - 1)
        m->row++;
}

static void m_ri(Model *m)
{
    if (m->row == 0)
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
    m->col = c;
}

/* Print one printable ASCII cell through the commit_cluster() rules. */
static void m_print_char(Model *m, char ch)
{
    if (m->col >= m->cols) {
        if (m->decawm)
            m_wrap_commit(m);
        else
            m->col = m->cols - 1;
    }
    if (m->irm) {
        m_ich(m, 1);
        if (m->col >= m->cols)
            m->col = m->cols - 1;
    }
    if (m->col >= m->cols)
        m->col = m->cols - 1;

    m_stamp(m, m->row);
    MRow *r = &m->grid[m->row];
    int c = m->col;
    uint8_t old = r->cells[c].kind;
    r->cells[c].kind = (ch == ' ') ? MC_BLANK : MC_CHAR;
    r->cells[c].ch = (ch == ' ') ? 0 : ch;
    if (c == m->cols - 1)
        r->wrap = false;
    if (old == MC_WIDE && c + 1 < m->cols) {
        /* A narrow print over a wide lead blanks the stranded
         * continuation. */
        r->cells[c + 1].kind = MC_BLANK;
        r->cells[c + 1].ch = 0;
        if (c + 1 == m->cols - 1)
            r->wrap = false;
    }

    if (c + 1 >= m->cols)
        m->col = m->decawm ? m->cols : m->cols - 1;
    else
        m->col = c + 1;
}

/* Print the wide fixture through the same rules, including the eager
 * wrap when two cells no longer fit. */
static void m_print_wide(Model *m)
{
    if (m->col >= m->cols && m->decawm)
        m_wrap_commit(m);
    if (m->col + 2 > m->cols && m->decawm) {
        m_wrap_commit(m);
        m->col = 0;
    }
    if (m->irm) {
        m_ich(m, 2);
        if (m->col >= m->cols)
            m->col = m->cols - 1;
    }
    if (m->col >= m->cols)
        m->col = m->cols - 1;

    m_stamp(m, m->row);
    MRow *r = &m->grid[m->row];
    int c = m->col;
    r->cells[c].kind = MC_WIDE;
    r->cells[c].ch = 0;
    if (c == m->cols - 1)
        r->wrap = false;
    if (c + 1 < m->cols) {
        r->cells[c + 1].kind = MC_CONT;
        r->cells[c + 1].ch = 0;
        if (c + 1 == m->cols - 1)
            r->wrap = false;
    }

    if (c + 2 >= m->cols)
        m->col = m->decawm ? m->cols : m->cols - 1;
    else
        m->col = c + 2;
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
 * blanks flatten to spaces; the wide fixture as its three UTF-8 bytes. */
static size_t m_row_text(Model *m, int u, char *out, size_t cap)
{
    const MRow *r = m_row(m, u);
    if (!r || cap == 0)
        return 0;
    size_t len = 0;
    for (int c = 0; c < m->cols; ++c) {
        switch (r->cells[c].kind) {
        case MC_HOLE:
        case MC_CONT:
            break;
        case MC_BLANK:
            len = put_bytes(out, len, cap, " ", 1);
            break;
        case MC_CHAR:
            len = put_bytes(out, len, cap, &r->cells[c].ch, 1);
            break;
        case MC_WIDE:
            len = put_bytes(out, len, cap, WIDE_UTF8, WIDE_UTF8_LEN);
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
        for (int c = 0; c < m->cols;) {
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
            case MC_CHAR:
                len = put_bytes(out, len, cap, &r->cells[c].ch, 1);
                last_nonblank = len;
                c++;
                break;
            case MC_WIDE:
                len = put_bytes(out, len, cap, WIDE_UTF8, WIDE_UTF8_LEN);
                last_nonblank = len;
                c += 2;
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
    if (strict_check &&
        (vt->cursor.row != m->row || vt->cursor.col != m->col)) {
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
static bool run_op(CfrTerm *vt, Model *m, uint64_t *rng, char *op,
                   size_t op_cap)
{
    unsigned pick = rng_below(rng, 100);
    char seq[128];

    if (pick < 30) {
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
    if (pick < 37) {
        m_print_wide(m);
        snprintf(op, op_cap, "print wide");
        strcpy(seq, WIDE_UTF8 "\x1b[0m");
        cfr_input_write(vt, (const uint8_t *)seq, strlen(seq));
        return true;
    }
    if (pick < 43) {
        m->col = 0;
        snprintf(op, op_cap, "CR");
        strcpy(seq, "\r\x1b[0m");
    } else if (pick < 51) {
        m_lf(m);
        snprintf(op, op_cap, "LF");
        strcpy(seq, "\n\x1b[0m");
    } else if (pick < 54) {
        m->col--;
        if (m->col < 0)
            m->col = 0;
        if (m->col >= m->cols)
            m->col = m->cols - 1;
        snprintf(op, op_cap, "BS");
        strcpy(seq, "\b\x1b[0m");
    } else if (pick < 57) {
        m_tab(m);
        snprintf(op, op_cap, "HT");
        strcpy(seq, "\t\x1b[0m");
    } else if (pick < 60) {
        m_ri(m);
        snprintf(op, op_cap, "RI");
        strcpy(seq, "\x1bM\x1b[0m");
    } else if (pick < 66) {
        int r = (int)rng_below(rng, (unsigned)m->rows);
        int c = (int)rng_below(rng, (unsigned)m->cols);
        m->row = r;
        m->col = c;
        snprintf(op, op_cap, "CUP %d;%d", r + 1, c + 1);
        snprintf(seq, sizeof(seq), "\x1b[%d;%dH\x1b[0m", r + 1, c + 1);
    } else if (pick < 69) {
        int c = (int)rng_below(rng, (unsigned)m->cols);
        m->col = c;
        snprintf(op, op_cap, "CHA %d", c + 1);
        snprintf(seq, sizeof(seq), "\x1b[%dG\x1b[0m", c + 1);
    } else if (pick < 72) {
        int r = (int)rng_below(rng, (unsigned)m->rows);
        m->row = r;
        /* VPA is an explicit cursor move: the column, including a
         * pending phantom, is clamped like CUP's. */
        if (m->col >= m->cols)
            m->col = m->cols - 1;
        snprintf(op, op_cap, "VPA %d", r + 1);
        snprintf(seq, sizeof(seq), "\x1b[%dd\x1b[0m", r + 1);
    } else if (pick < 76) {
        int n = 1 + (int)rng_below(rng, (unsigned)m->rows);
        m_scroll_up(m, n);
        snprintf(op, op_cap, "SU %d", n);
        snprintf(seq, sizeof(seq), "\x1b[%dS\x1b[0m", n);
    } else if (pick < 78) {
        int n = 1 + (int)rng_below(rng, (unsigned)m->rows);
        m_scroll_down(m, n);
        snprintf(op, op_cap, "SD %d", n);
        snprintf(seq, sizeof(seq), "\x1b[%dT\x1b[0m", n);
    } else if (pick < 81) {
        int n = 1 + (int)rng_below(rng, (unsigned)m->rows);
        m_il(m, n);
        snprintf(op, op_cap, "IL %d", n);
        snprintf(seq, sizeof(seq), "\x1b[%dL\x1b[0m", n);
    } else if (pick < 84) {
        int n = 1 + (int)rng_below(rng, (unsigned)m->rows);
        m_dl(m, n);
        snprintf(op, op_cap, "DL %d", n);
        snprintf(seq, sizeof(seq), "\x1b[%dM\x1b[0m", n);
    } else if (pick < 87) {
        int n = 1 + (int)rng_below(rng, (unsigned)m->cols);
        m_ich(m, n);
        snprintf(op, op_cap, "ICH %d", n);
        snprintf(seq, sizeof(seq), "\x1b[%d@\x1b[0m", n);
    } else if (pick < 89) {
        int n = 1 + (int)rng_below(rng, (unsigned)m->cols);
        m_dch(m, n);
        snprintf(op, op_cap, "DCH %d", n);
        snprintf(seq, sizeof(seq), "\x1b[%dP\x1b[0m", n);
    } else if (pick < 91) {
        int n = 1 + (int)rng_below(rng, (unsigned)m->cols);
        m_ech(m, n);
        snprintf(op, op_cap, "ECH %d", n);
        snprintf(seq, sizeof(seq), "\x1b[%dX\x1b[0m", n);
    } else if (pick < 94) {
        int mode = (int)rng_below(rng, 3);
        m_el(m, mode);
        snprintf(op, op_cap, "EL %d", mode);
        snprintf(seq, sizeof(seq), "\x1b[%dK\x1b[0m", mode);
    } else if (pick < 96) {
        int mode = (int)rng_below(rng, 3);
        m_ed(m, mode);
        snprintf(op, op_cap, "ED %d", mode);
        snprintf(seq, sizeof(seq), "\x1b[%dJ\x1b[0m", mode);
    } else if (pick < 97) {
        m->decawm = !m->decawm;
        if (!m->decawm && m->col >= m->cols)
            m->col = m->cols - 1;
        snprintf(op, op_cap, "DECAWM %s", m->decawm ? "on" : "off");
        strcpy(seq, m->decawm ? "\x1b[?7h\x1b[0m" : "\x1b[?7l\x1b[0m");
    } else if (pick < 98) {
        m->irm = !m->irm;
        snprintf(op, op_cap, "IRM %s", m->irm ? "on" : "off");
        strcpy(seq, m->irm ? "\x1b[4h\x1b[0m" : "\x1b[4l\x1b[0m");
    } else if (pick < 99) {
        m->saved_row = m->row;
        m->saved_col = m->col;
        snprintf(op, op_cap, "DECSC");
        strcpy(seq, "\x1b"
                    "7\x1b[0m");
    } else {
        m->row = m->saved_row;
        m->col = m->saved_col;
        snprintf(op, op_cap, "DECRC");
        strcpy(seq, "\x1b"
                    "8\x1b[0m");
    }

    cfr_input_write(vt, (const uint8_t *)seq, strlen(seq));
    return true;
}

static void fuzz_dump_grid(CfrTerm *vt, Model *m)
{
    fprintf(stderr, "  cursor: model %d,%d  engine %d,%d\n", m->row, m->col,
            vt->cursor.row, vt->cursor.col);
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
            for (int c = 0; c < m->cols; ++c)
                fprintf(stderr, " %d:%02x", mr->cells[c].kind,
                        (unsigned char)mr->cells[c].ch);
            fprintf(stderr, "\n           engine cp:  ");
            for (int c = 0; c < m->cols; ++c)
                fprintf(stderr, " %02x/%d",
                        (unsigned)pp->cells[(size_t)pr * pp->cols + c].cp,
                        (int)pp->cells[(size_t)pr * pp->cols + c].width);
            fprintf(stderr, "\n");
        }
    }
}

static bool fuzz_run(int rows, int cols, int sb_cap, uint64_t seed, int iters)
{
    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.rows = rows;
    cfg.cols = cols;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 6;
    cfg.scrollback = sb_cap;
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
    m.sb_cap = sb_cap;
    m.decawm = true;
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
        uint64_t rng = seed + (uint64_t)i * 0x9E3779B97F4A7C15ULL;
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

static bool run_config(int rows, int cols, int sb, int seeds)
{
    uint64_t base = fuzz_seed();
    int iters = fuzz_iters();
    for (int i = 0; i < seeds; ++i) {
        check_failed = false;
        if (!fuzz_run(rows, cols, sb, base + (uint64_t)i * 0x10001u,
                      iters))
            return false;
    }
    return true;
}

static void test_wrap_fuzz_small(void)
{
    ASSERT_TRUE(run_config(4, 3, 16, 8));
}

static void test_wrap_fuzz_narrow(void)
{
    ASSERT_TRUE(run_config(6, 5, 32, 8));
}

static void test_wrap_fuzz_wide(void)
{
    ASSERT_TRUE(run_config(8, 12, 64, 8));
}

static void test_wrap_fuzz_evicting(void)
{
    ASSERT_TRUE(run_config(5, 8, 5, 8));
}

static void test_wrap_fuzz_no_scrollback(void)
{
    ASSERT_TRUE(run_config(4, 6, 0, 8));
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
    /* COFFER_FUZZ_ONLY=small|narrow|wide|evict|none narrows the run. */
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

    TEST_SUMMARY();
}
