/*
 * coffer — grid introspection: row flattening + unified-buffer search
 *
 * cfr_row_text() flattens one unified row (visible or scrollback) into
 * UTF-8 text; cfr_find_text() searches the whole unified buffer for a
 * needle. These are the primitives behind scriptable assertions
 * (assert-contains, wait-for) and text dumps in host tooling — lifted
 * from the coffer CLI (src/coffer.c) so every consumer shares one
 * grapheme-aware implementation.
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

size_t cfr_row_text_size(const CfrTerm *vt, int row)
{
    int row_in_page = 0;
    const CfrPage *page = cfr_row_page(vt, row, &row_in_page);
    if (!page)
        return 0;
    /* A grapheme cluster holds at most CFR_CLUSTER_MAX codepoints at
     * 4 UTF-8 bytes each. */
    return (size_t)page->cols * (CFR_CLUSTER_MAX * 4) + 1;
}

size_t cfr_row_text(const CfrTerm *vt, int row, char *buf, size_t cap)
{
    if (!buf || cap == 0)
        return 0;
    buf[0] = '\0';

    /* Resolve the row once; the owning page serves the grapheme reads
     * directly. The row's own width bounds the walk: scrollback rows keep
     * the width they were written at, which can exceed the grid's. */
    int row_in_page = 0;
    const CfrPage *page = cfr_row_page(vt, row, &row_in_page);
    if (!page)
        return 0;
    int row_cols = page->cols;
    const CfrCell *row_cells = &page->cells[(size_t)row_in_page * page->cols];

    size_t n = 0;
    for (int c = 0; c < row_cols; c++) {
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

    /* Rows differ in width (history keeps the width it was written at),
     * so the flattening buffer grows to the widest row seen. A row that
     * does not exist (the grid is allocated lazily) matches nothing. */
    size_t cap = 0;
    char *buf = NULL;
    bool found = false;
    int sb_lines = cfr_get_scrollback_lines(vt);
    for (int unified = -sb_lines; unified < rows && !found; unified++) {
        size_t need = cfr_row_text_size(vt, unified);
        if (need == 0)
            continue;
        if (need > cap) {
            char *bigger = cfr_realloc(vt, buf, need);
            if (!bigger) {
                cfr_dealloc(vt, buf);
                return false;
            }
            buf = bigger;
            cap = need;
        }
        cfr_row_text(vt, unified, buf, cap);
        found = row_matches(buf, needle, line_end);
        if (found && out_row)
            *out_row = unified;
    }

    cfr_dealloc(vt, buf);
    return found;
}
