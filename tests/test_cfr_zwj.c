/* tests/test_cfr_zwj.c — every RGI emoji ZWJ sequence is one 2-cell cluster.
 *
 * The fixture (zwj_sequences.h) is generated from emoji-zwj-sequences.txt by
 * src/scripts/gen_zwj_sequences.py: every RGI_Emoji_ZWJ_Sequence, one per
 * line, space-separated. Walking all of them covers far more of the
 * generated unicode_tables.h than a hand-picked sample can — the codepoints
 * that follow a ZWJ span the whole Extended_Pictographic table (GB11), and
 * the leading codepoints span the Wide/VS16 bases the cluster width rule
 * keys off. A regeneration that drops a range from CFR_EXT_PICT or
 * CFR_EXTEND, or a change to the width rule, splits sequences apart here
 * instead of doing it silently in a terminal.
 *
 * Three levels, each stronger than the last:
 *   1. the pairwise break predicate the parser walks,
 *   2. cfr_cluster_width on the decoded sequence,
 *   3. the grid itself — the cell the sequence lands in, its continuation,
 *      and the grapheme arena the renderer reads back.
 */

#include "coffer_internal.h"
#include "test_helpers.h"
#include <coffer/coffer.h>

#include <stdio.h>
#include <string.h>

#include "zwj_sequences.h"

#define MAX_SEQ_CPS 16

/* Decode one UTF-8 sequence into `*out`; returns the bytes consumed, or 0
 * on a malformed sequence. */
static size_t decode_utf8(const char *s, uint32_t *out)
{
    const unsigned char *p = (const unsigned char *)s;
    uint32_t cp;
    size_t len;

    if (p[0] < 0x80) {
        *out = p[0];
        return 1;
    } else if ((p[0] & 0xE0) == 0xC0) {
        cp = p[0] & 0x1Fu;
        len = 2;
    } else if ((p[0] & 0xF0) == 0xE0) {
        cp = p[0] & 0x0Fu;
        len = 3;
    } else if ((p[0] & 0xF8) == 0xF0) {
        cp = p[0] & 0x07u;
        len = 4;
    } else {
        return 0;
    }

    for (size_t i = 1; i < len; i++) {
        if ((p[i] & 0xC0) != 0x80)
            return 0;
        cp = (cp << 6) | (p[i] & 0x3Fu);
    }
    *out = cp;
    return len;
}

/* Copy the codepoints of one space-delimited fixture entry. Returns the
 * codepoint count, or 0 on malformed UTF-8 / overflow. */
static size_t seq_codepoints(const char *seq, size_t seq_len, uint32_t *out,
                             size_t max)
{
    size_t n = 0;
    size_t i = 0;
    while (i < seq_len) {
        if (n >= max)
            return 0;
        size_t used = decode_utf8(seq + i, &out[n]);
        if (used == 0)
            return 0;
        n++;
        i += used;
    }
    return n;
}

typedef struct
{
    const char *p; /* cursor into the fixture blob */
    int index;     /* sequence ordinal, for reporting */
} SeqIter;

/* Advance to the next fixture entry. Returns false at the end (or on a
 * malformed entry, which the decode test reports). */
static bool seq_next(SeqIter *it, uint32_t *cps, size_t *n, const char **text,
                     size_t *text_len)
{
    if (*it->p == '\0')
        return false;
    const char *end = strchr(it->p, ' ');
    if (!end)
        return false;

    *text = it->p;
    *text_len = (size_t)(end - it->p);
    *n = seq_codepoints(*text, *text_len, cps, MAX_SEQ_CPS);
    it->index++;
    it->p = end + 1;
    return true;
}

/* The fixture is complete: every entry decodes, and the count matches the
 * generated constant (a truncated file or a stray separator fails here). */
static void test_fixture_entries_decode(void)
{
    SeqIter it = { CFR_ZWJ_SEQUENCES, 0 };
    uint32_t cps[MAX_SEQ_CPS];
    const char *text;
    size_t n, text_len;

    while (seq_next(&it, cps, &n, &text, &text_len)) {
        if (n == 0) {
            fprintf(stderr, "  entry %d does not decode\n", it.index - 1);
            ASSERT_TRUE(false);
        }
        ASSERT_TRUE(n <= MAX_SEQ_CPS);
    }
    ASSERT_EQ(it.index, CFR_ZWJ_SEQUENCE_COUNT);
}

/* GB9/GB11 as the parser walks them: no break before any codepoint of the
 * sequence, so the whole thing accumulates into one cluster. */
static void test_every_sequence_has_no_internal_break(void)
{
    SeqIter it = { CFR_ZWJ_SEQUENCES, 0 };
    uint32_t cps[MAX_SEQ_CPS];
    const char *text;
    size_t n, text_len;

    while (seq_next(&it, cps, &n, &text, &text_len)) {
        for (size_t i = 1; i < n; i++) {
            if (cfr_grapheme_break_before(cps[i - 1], cps[i], NULL)) {
                fprintf(stderr,
                        "  entry %d breaks at cp %zu: U+%04X ÷ U+%04X\n",
                        it.index - 1, i, cps[i - 1], cps[i]);
                ASSERT_TRUE(false);
            }
        }
    }
    ASSERT_EQ(it.index, CFR_ZWJ_SEQUENCE_COUNT);
}

/* Every RGI ZWJ sequence occupies two cells, whatever its base and
 * selectors: the base is Wide, or a trailing VS16 doubles it. */
static void test_every_sequence_is_two_cells(void)
{
    SeqIter it = { CFR_ZWJ_SEQUENCES, 0 };
    uint32_t cps[MAX_SEQ_CPS];
    const char *text;
    size_t n, text_len;

    while (seq_next(&it, cps, &n, &text, &text_len)) {
        int width = cfr_cluster_width(NULL, cps, (uint32_t)n);
        if (width != 2) {
            fprintf(stderr,
                    "  entry %d (starts U+%04X) measures %d cells\n",
                    it.index - 1, cps[0], width);
            ASSERT_EQ(width, 2);
        }
    }
    ASSERT_EQ(it.index, CFR_ZWJ_SEQUENCE_COUNT);
}

/* The grid form: the sequence lands in one cell of width 2, the next column
 * is its continuation, and the grapheme arena hands the renderer back every
 * codepoint. One row per sequence, so a failure names the row. */
static void test_every_sequence_occupies_one_two_cell_grid_cell(void)
{
    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.rows = CFR_ZWJ_SEQUENCE_COUNT + 1;
    cfg.cols = 8;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 6;
    cfg.scrollback = 0;
    cfg.reflow = false;

    CfrTerm *vt = cfr_new(&cfg);
    ASSERT_NOT_NULL(vt);

    SeqIter it = { CFR_ZWJ_SEQUENCES, 0 };
    uint32_t cps[MAX_SEQ_CPS];
    const char *text;
    size_t n, text_len;
    int row = 0;

    while (seq_next(&it, cps, &n, &text, &text_len)) {
        if (n == 0)
            break;
        cfr_input_write(vt, (const uint8_t *)text, text_len);

        const CfrCell *head = cfr_get_cell(vt, row, 0);
        ASSERT_NOT_NULL(head);
        if (head->cp != cps[0] || head->width != 2) {
            fprintf(stderr, "  row %d (starts U+%04X): cp=U+%04X width=%d\n",
                    row, cps[0], head->cp, head->width);
            ASSERT_EQ(head->width, 2);
        }

        const CfrCell *cont = cfr_get_cell(vt, row, 1);
        ASSERT_NOT_NULL(cont);
        ASSERT_EQ(cont->cp, 0u);
        ASSERT_EQ(cont->width, 0);

        uint32_t readback[MAX_SEQ_CPS];
        size_t got = cfr_cell_get_grapheme(vt, head, readback, MAX_SEQ_CPS);
        if (got != n) {
            fprintf(stderr,
                    "  row %d (starts U+%04X): grapheme has %zu cps, "
                    "expected %zu\n",
                    row, cps[0], got, n);
            ASSERT_EQ(got, n);
        }
        for (size_t i = 0; i < n; i++) {
            if (readback[i] != cps[i]) {
                fprintf(stderr, "  row %d cp %zu: U+%04X != U+%04X\n", row, i,
                        readback[i], cps[i]);
                ASSERT_EQ(readback[i], cps[i]);
            }
        }

        cfr_input_write(vt, (const uint8_t *)"\r\n", 2);
        row++;
    }

    ASSERT_EQ(row, CFR_ZWJ_SEQUENCE_COUNT);
    cfr_free(vt);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    RUN_TEST(test_fixture_entries_decode);
    RUN_TEST(test_every_sequence_has_no_internal_break);
    RUN_TEST(test_every_sequence_is_two_cells);
    RUN_TEST(test_every_sequence_occupies_one_two_cell_grid_cell);

    TEST_SUMMARY();
}
