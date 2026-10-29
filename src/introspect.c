/*
 * coffer — grid introspection: row flattening + unified-buffer search
 *
 * cfr_row_text() flattens one unified row (visible or scrollback) into
 * UTF-8 text; cfr_find_text() searches the whole unified buffer for a
 * needle. These are the primitives behind scriptable assertions
 * (assert-contains, wait-for) and text dumps in host tooling — lifted
 * from contrib/cfr-debug so every consumer shares one grapheme-aware
 * implementation.
 */

#include "coffer_internal.h"

#include <string.h>

/* Encode one codepoint into buf; returns the bytes written (0 when it
 * does not fit). */
static size_t utf8_encode(uint32_t cp, char *buf, size_t cap)
{
    if (cp < 0x80) {
        if (cap < 1)
            return 0;
        buf[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        if (cap < 2)
            return 0;
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        if (cap < 3)
            return 0;
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cap < 4)
        return 0;
    buf[0] = (char)(0xF0 | (cp >> 18));
    buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    buf[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

size_t cfr_row_text(const CfrTerm *vt, int row, char *buf, size_t cap)
{
    if (!buf || cap == 0)
        return 0;
    buf[0] = '\0';
    if (!vt)
        return 0;

    int rows = 0, cols = 0;
    cfr_get_dimensions(vt, &rows, &cols);
    if (cols <= 0)
        return 0;
    if (row >= rows || row < -cfr_get_scrollback_lines(vt))
        return 0;

    /* Resolve the row once. A scrollback row lives in one page, and the
     * per-column lookup re-walks the page list for every cell — a
     * full-buffer scan (wait-for, assert-contains) is quadratic in the
     * page count without this. The owning page also serves the grapheme
     * read directly, skipping the page search per multi-codepoint
     * cluster. */
    const CfrPage *page = NULL;
    int row_cols = cols;
    const CfrCell *row_cells;
    if (row >= 0) {
        if (!vt->grid)
            return 0;
        page = vt->grid;
        row_cells = &vt->grid->cells[(size_t)row * cols];
    } else {
        int row_in_page = 0;
        page = cfr_sb_page_for_row(vt, -row - 1, &row_in_page);
        if (!page)
            return 0;
        row_cols = page->cols;
        row_cells = &page->cells[(size_t)row_in_page * page->cols];
    }

    size_t n = 0;
    for (int c = 0; c < cols && c < row_cols; c++) {
        const CfrCell *cell = &row_cells[c];
        if (cell->width == 0)
            continue; /* continuation of a wide cluster, or a cell that
                       * was never written (indistinguishable by design:
                       * both are all-zero; reflow drops them too) */
        if (cell->cp == 0 || cell->cp == 0x20) {
            if (n + 1 >= cap)
                break;
            buf[n++] = ' ';
            continue;
        }
        uint32_t cps[CFR_CLUSTER_MAX];
        size_t nc;
        if (cell->grapheme_id == 0) {
            cps[0] = cell->cp;
            nc = 1;
        } else {
            nc = cfr_grapheme_read(page, cell->grapheme_id, cps,
                                   CFR_CLUSTER_MAX);
        }
        if (nc == 0) {
            if (n + 1 >= cap)
                break;
            buf[n++] = ' ';
            continue;
        }
        for (size_t i = 0; i < nc; i++) {
            size_t w = utf8_encode(cps[i], buf + n, cap - n - 1);
            if (w == 0)
                break;
            n += w;
        }
        if (n + 1 >= cap)
            break;
    }
    buf[n] = '\0';
    return n;
}

/* Does `row` (flattened text) match `needle`? With `line_end` the row
 * must END with the needle after trimming trailing blanks. */
static bool row_matches(const char *row, const char *needle, bool line_end)
{
    if (!line_end)
        return strstr(row, needle) != NULL;
    size_t len = strlen(row);
    while (len > 0 && row[len - 1] == ' ')
        len--; /* trim trailing blanks */
    size_t nlen = strlen(needle);
    if (nlen > len)
        return false;
    return memcmp(row + (len - nlen), needle, nlen) == 0;
}

bool cfr_find_text(CfrTerm *vt, const char *needle, bool line_end,
                   int *out_row)
{
    if (!vt)
        return false;
    if (!needle || !*needle) {
        /* Degenerate needle: an immediate match (assert-contains "" is
         * trivially true; wait-for "" completes at once). */
        if (out_row)
            *out_row = 0;
        return true;
    }

    int rows = 0, cols = 0;
    cfr_get_dimensions(vt, &rows, &cols);
    if (cols <= 0)
        return false;

    /* A grapheme cluster holds at most CFR_CLUSTER_MAX codepoints at
     * 4 UTF-8 bytes each. */
    size_t cap = (size_t)cols * (CFR_CLUSTER_MAX * 4) + 1;
    char *buf = cfr_alloc(vt, cap);
    if (!buf)
        return false;

    bool found = false;
    int sb_lines = cfr_get_scrollback_lines(vt);
    for (int sb = sb_lines - 1; sb >= 0 && !found; sb--) {
        int unified = -(sb + 1);
        cfr_row_text(vt, unified, buf, cap);
        found = row_matches(buf, needle, line_end);
        if (found && out_row)
            *out_row = unified;
    }
    for (int r = 0; r < rows && !found; r++) {
        cfr_row_text(vt, r, buf, cap);
        found = row_matches(buf, needle, line_end);
        if (found && out_row)
            *out_row = r;
    }

    cfr_dealloc(vt, buf);
    return found;
}
