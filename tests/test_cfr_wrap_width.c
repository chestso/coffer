/* tests/test_cfr_wrap_width.c — wrapping edge cases for ambiguous-width
 * emojis and VS16/VS15 presentation variations.
 *
 * Three width rule families interact at the right margin:
 *   - East Asian Ambiguous bases (U+25B6 ▶, U+00A1 ¡): 1 cell narrow,
 *     2 cells when the terminal sets ambiguous_wide.
 *   - VS16 (emoji presentation): the cluster takes 2 cells even when
 *     the base is narrow or ambiguous.
 *   - VS15 (text presentation): the cluster falls back to the base's
 *     East Asian width — it cancels VS16's doubling but must never
 *     collapse a Wide base (CJK has no 1-cell glyph) or the whole
 *     column grid drifts.
 * When a cluster carries both selectors, the LAST one wins (UTS #51).
 *
 * The grid-level tests pin the wrap mechanics: eager wrap of a wide
 * cluster at the margin, the deferred phantom, wrap edges riding the
 * continuation cell, scroll-at-bottom wraps, reflow re-wrapping, chunked
 * writes that split a base from its VS16, and overwrites that strand a
 * continuation. */

#include "coffer_internal.h"
#include "test_helpers.h"
#include <coffer/coffer.h>

#include <stdio.h>
#include <string.h>

static CfrTerm *make_term(int rows, int cols, bool ambiguous_wide, bool reflow)
{
    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.rows = rows;
    cfg.cols = cols;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 6;
    cfg.scrollback = 100;
    cfg.reflow = reflow;
    cfg.ambiguous_wide = ambiguous_wide;
    return cfr_new(&cfg);
}

static void feed(CfrTerm *vt, const char *s)
{
    cfr_input_write(vt, (const uint8_t *)s, strlen(s));
}

/* UTF-8 literals:
 *   "\xE2\x96\xB6"                   U+25B6 ▶  (Ambiguous)
 *   "\xE2\x9A\xA0"                   U+26A0 ⚠  (Neutral, narrow)
 *   "\xE2\x9D\xA4"                   U+2764 ❤  (Neutral, narrow)
 *   "\xEF\xB8\x8F"                   U+FE0F VS16
 *   "\xEF\xB8\x8E"                   U+FE0E VS15
 *   "\xE4\xB8\xAD"                   U+4E2D 中  (Wide)
 *   "\xF0\x9F\x98\x80"               U+1F600 😀 (Wide)
 *   "\xF0\x9F\x87\xA9\xF0\x9F\x87\xB0" 🇩🇰 (RI pair)
 *   "1\xEF\xB8\x8F\xE2\x83\xA3"       1️⃣  (keycap)
 *   "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9" 👨‍👩 (ZWJ family) */

/* ------------------------------------------------------------------ */
/* Cluster width rules                                                */
/* ------------------------------------------------------------------ */

static void test_cluster_width_presentation_selectors(void)
{
    /* Narrow base (U+26A0 is Neutral, not Ambiguous). */
    ASSERT_EQ(cfr_cluster_width(NULL, (uint32_t[]){ 0x26A0 }, 1), 1);
    ASSERT_EQ(cfr_cluster_width(NULL, (uint32_t[]){ 0x26A0, 0xFE0F }, 2), 2);
    ASSERT_EQ(cfr_cluster_width(NULL, (uint32_t[]){ 0x26A0, 0xFE0E }, 2), 1);
    /* Both selectors: the LAST one wins. */
    ASSERT_EQ(cfr_cluster_width(NULL, (uint32_t[]){ 0x26A0, 0xFE0F, 0xFE0E }, 3), 1);
    ASSERT_EQ(cfr_cluster_width(NULL, (uint32_t[]){ 0x26A0, 0xFE0E, 0xFE0F }, 3), 2);
    /* Keycap and ZWJ sequences stay 2 cells. */
    ASSERT_EQ(cfr_cluster_width(NULL, (uint32_t[]){ '1', 0xFE0F, 0x20E3 }, 3), 2);
    ASSERT_EQ(cfr_cluster_width(NULL, (uint32_t[]){ 0x1F468, 0x200D, 0x1F469 }, 3), 2);
    /* ❤️‍🔥 = U+2764 VS16 ZWJ U+1F525: 2 cells. */
    ASSERT_EQ(cfr_cluster_width(NULL, (uint32_t[]){ 0x2764, 0xFE0F, 0x200D, 0x1F525 }, 4), 2);
}

static void test_vs15_never_narrows_wide_base(void)
{
    /* U+4E2D and U+1F600 are Wide; a stray VS15 must not collapse them
     * to 1 cell or every following column shifts. */
    ASSERT_EQ(cfr_cluster_width(NULL, (uint32_t[]){ 0x4E2D }, 1), 2);
    ASSERT_EQ(cfr_cluster_width(NULL, (uint32_t[]){ 0x4E2D, 0xFE0E }, 2), 2);
    ASSERT_EQ(cfr_cluster_width(NULL, (uint32_t[]){ 0x1F600, 0xFE0E }, 2), 2);

    /* Grid form: 中 + VS15 + 中 — both ideographs occupy 2 cells, so the
     * second starts at column 2, not column 1. */
    CfrTerm *vt = make_term(4, 8, false, false);
    feed(vt, "\xE4\xB8\xAD\xEF\xB8\x8E"
             "\xE4\xB8\xAD");
    ASSERT_EQ(cfr_get_cell(vt, 0, 0)->width, 2);
    ASSERT_EQ(cfr_get_cell(vt, 0, 0)->cp, 0x4E2Du);
    ASSERT_EQ(cfr_get_cell(vt, 0, 2)->cp, 0x4E2Du);
    ASSERT_EQ(cfr_get_cell(vt, 0, 2)->width, 2);
    cfr_free(vt);
}

static void test_vs15_ambiguous_base_keeps_setting(void)
{
    /* U+25B6 is East Asian Ambiguous. VS15 leaves the width decision to
     * the ambiguous policy; VS16 doubles unconditionally. */
    CfrTerm *vt = make_term(4, 8, false, false);
    ASSERT_EQ(cfr_cluster_width(vt, (uint32_t[]){ 0x25B6 }, 1), 1);
    ASSERT_EQ(cfr_cluster_width(vt, (uint32_t[]){ 0x25B6, 0xFE0E }, 2), 1);
    ASSERT_EQ(cfr_cluster_width(vt, (uint32_t[]){ 0x25B6, 0xFE0F }, 2), 2);
    cfr_free(vt);

    vt = make_term(4, 8, true, false);
    ASSERT_EQ(cfr_cluster_width(vt, (uint32_t[]){ 0x25B6 }, 1), 2);
    ASSERT_EQ(cfr_cluster_width(vt, (uint32_t[]){ 0x25B6, 0xFE0E }, 2), 2);
    ASSERT_EQ(cfr_cluster_width(vt, (uint32_t[]){ 0x25B6, 0xFE0F }, 2), 2);
    cfr_free(vt);
}

/* ------------------------------------------------------------------ */
/* Wrapping at the right margin                                       */
/* ------------------------------------------------------------------ */

static void test_ambiguous_narrow_fits_margin(void)
{
    /* Default (narrow): ▶ fits in the last column; the row fills and the
     * cursor parks on the phantom without any wrap. */
    CfrTerm *vt = make_term(4, 4, false, false);
    feed(vt, "abc\xE2\x96\xB6");
    ASSERT_EQ(cfr_get_cell(vt, 0, 3)->cp, 0x25B6u);
    ASSERT_EQ(cfr_get_cell(vt, 0, 3)->width, 1);
    ASSERT_TRUE(PENDING_WRAP(vt));
    ASSERT_FALSE(cfr_row_is_continuation(vt, 1));
    ASSERT_EQ(cfr_get_cell(vt, 1, 0)->cp, 0u);
    cfr_free(vt);
}

static void test_ambiguous_wide_eager_wrap(void)
{
    /* ambiguous_wide: the same print needs 2 cells with 1 left, so the
     * wrap is eager — ▶ moves to the next row and the wrap edge lands on
     * the margin cell of the row being left. */
    CfrTerm *vt = make_term(4, 4, true, false);
    feed(vt, "abc\xE2\x96\xB6");
    ASSERT_EQ(cfr_get_cell(vt, 0, 3)->cp, 0u);
    ASSERT_EQ(cfr_get_cell(vt, 1, 0)->cp, 0x25B6u);
    ASSERT_EQ(cfr_get_cell(vt, 1, 0)->width, 2);
    ASSERT_EQ(cfr_get_cell(vt, 1, 1)->width, 0);
    ASSERT_TRUE(cfr_row_is_continuation(vt, 1));
    cfr_free(vt);
}

static void test_vs16_eager_wrap(void)
{
    /* ⚠ is narrow alone, but ⚠️ (VS16) needs 2 cells: eager wrap. */
    CfrTerm *vt = make_term(4, 4, false, false);
    feed(vt, "abc\xE2\x9A\xA0\xEF\xB8\x8F");
    ASSERT_EQ(cfr_get_cell(vt, 0, 3)->cp, 0u);
    ASSERT_EQ(cfr_get_cell(vt, 1, 0)->cp, 0x26A0u);
    ASSERT_EQ(cfr_get_cell(vt, 1, 0)->width, 2);
    ASSERT_EQ(cfr_get_cell(vt, 1, 1)->width, 0);
    ASSERT_TRUE(cfr_row_is_continuation(vt, 1));
    cfr_free(vt);
}

static void test_vs16_fits_exactly(void)
{
    /* With two columns left the widened cluster fills the row exactly;
     * the cursor takes the phantom, no wrap is committed. */
    CfrTerm *vt = make_term(4, 5, false, false);
    feed(vt, "abc\xE2\x9A\xA0\xEF\xB8\x8F");
    ASSERT_EQ(cfr_get_cell(vt, 0, 3)->cp, 0x26A0u);
    ASSERT_EQ(cfr_get_cell(vt, 0, 3)->width, 2);
    ASSERT_EQ(cfr_get_cell(vt, 0, 4)->width, 0);
    ASSERT_TRUE(PENDING_WRAP(vt));
    ASSERT_FALSE(cfr_row_is_continuation(vt, 1));
    cfr_free(vt);
}

static void test_vs15_stays_narrow_at_margin(void)
{
    /* ❤︎ (text presentation) keeps 1 cell: it fills the margin like any
     * narrow character. */
    CfrTerm *vt = make_term(4, 4, false, false);
    feed(vt, "abc\xE2\x9D\xA4\xEF\xB8\x8E");
    ASSERT_EQ(cfr_get_cell(vt, 0, 3)->cp, 0x2764u);
    ASSERT_EQ(cfr_get_cell(vt, 0, 3)->width, 1);
    ASSERT_TRUE(PENDING_WRAP(vt));
    ASSERT_FALSE(cfr_row_is_continuation(vt, 1));
    cfr_free(vt);
}

static void test_vs15_ambiguous_wide_wraps(void)
{
    /* Under ambiguous_wide, ▶︎ keeps its (wide) base width, so it still
     * wraps eagerly at the margin — VS15 cancels only VS16's doubling. */
    CfrTerm *vt = make_term(4, 4, true, false);
    feed(vt, "abc\xE2\x96\xB6\xEF\xB8\x8E");
    ASSERT_EQ(cfr_get_cell(vt, 1, 0)->cp, 0x25B6u);
    ASSERT_EQ(cfr_get_cell(vt, 1, 0)->width, 2);
    ASSERT_TRUE(cfr_row_is_continuation(vt, 1));
    cfr_free(vt);
}

static void test_vs16_no_autowrap_overwrites_margin(void)
{
    /* DECAWM off: the widened cluster cannot wrap, so it overwrites the
     * last column and the cursor stays put. */
    CfrTerm *vt = make_term(4, 4, false, false);
    feed(vt, "\x1b[?7l");
    feed(vt, "abc\xE2\x9A\xA0\xEF\xB8\x8F");
    ASSERT_EQ(cfr_get_cell(vt, 0, 3)->cp, 0x26A0u);
    ASSERT_EQ(cfr_get_cell(vt, 0, 3)->width, 2);
    ASSERT_FALSE(cfr_row_is_continuation(vt, 1));
    ASSERT_EQ(cfr_get_cell(vt, 1, 0)->cp, 0u);
    cfr_free(vt);
}

static void test_ambiguous_wide_pure_line_wraps(void)
{
    /* A line of ambiguous-wide clusters wraps pair-by-pair and the wrap
     * edge rides the continuation cell of the margin pair. */
    CfrTerm *vt = make_term(4, 4, true, false);
    feed(vt, "ab\xC2\xA1\xC2\xA1");
    ASSERT_EQ(cfr_get_cell(vt, 0, 2)->cp, 0x00A1u);
    ASSERT_EQ(cfr_get_cell(vt, 0, 2)->width, 2);
    ASSERT_EQ(cfr_get_cell(vt, 0, 3)->width, 0);
    ASSERT_EQ(cfr_get_cell(vt, 1, 0)->cp, 0x00A1u);
    ASSERT_EQ(cfr_get_cell(vt, 1, 0)->width, 2);
    ASSERT_TRUE(cfr_row_is_continuation(vt, 1));
    cfr_free(vt);
}

static void test_multi_emoji_line_wraps_and_joins(void)
{
    /* Four ⚠️ in 6 columns: three fill row 0, the fourth wraps; the join
     * is a real soft wrap (edge + shared lineage). */
    CfrTerm *vt = make_term(4, 6, false, false);
    feed(vt, "\xE2\x9A\xA0\xEF\xB8\x8F\xE2\x9A\xA0\xEF\xB8\x8F\xE2\x9A\xA0\xEF\xB8\x8F\xE2\x9A\xA0\xEF\xB8\x8F");
    ASSERT_EQ(cfr_get_cell(vt, 0, 0)->width, 2);
    ASSERT_EQ(cfr_get_cell(vt, 0, 2)->width, 2);
    ASSERT_EQ(cfr_get_cell(vt, 0, 4)->width, 2);
    ASSERT_EQ(cfr_get_cell(vt, 1, 0)->cp, 0x26A0u);
    ASSERT_EQ(cfr_get_cell(vt, 1, 0)->width, 2);
    ASSERT_TRUE(cfr_row_is_continuation(vt, 1));
    ASSERT_EQ(vt->cursor.row, 1);
    ASSERT_EQ(vt->cursor.col, 2);
    cfr_free(vt);
}

static void test_emoji_wrap_at_bottom_scrolls(void)
{
    /* The eager wrap on the bottom row scrolls the region: the top row
     * moves into scrollback and the emoji prints on the fresh bottom. */
    CfrTerm *vt = make_term(2, 4, false, false);
    feed(vt, "AAAAAAAA"); /* fills row 0, wraps row 1, phantom at bottom */
    feed(vt, "\xE2\x9A\xA0\xEF\xB8\x8F");
    ASSERT_EQ(cfr_get_scrollback_lines(vt), 1);
    ASSERT_EQ(cfr_get_cell(vt, 0, 0)->cp, (uint32_t)'A');
    ASSERT_EQ(cfr_get_cell(vt, 1, 0)->cp, 0x26A0u);
    ASSERT_EQ(cfr_get_cell(vt, 1, 0)->width, 2);
    ASSERT_EQ(cfr_get_cell(vt, 1, 1)->width, 0);
    ASSERT_TRUE(cfr_row_is_continuation(vt, 1));
    cfr_free(vt);
}

static void test_regional_indicator_pair_wraps_as_unit(void)
{
    /* 🇩🇰 is one 2-cell cluster: it fills the margin pair, 'c' resolves
     * the phantom and wraps with the edge on the continuation cell. */
    CfrTerm *vt = make_term(4, 4, false, false);
    feed(vt, "ab"
             "\xF0\x9F\x87\xA9"
             "\xF0\x9F\x87\xB0"
             "cd");
    ASSERT_EQ(cfr_get_cell(vt, 0, 2)->cp, 0x1F1E9u);
    ASSERT_EQ(cfr_get_cell(vt, 0, 2)->width, 2);
    ASSERT_EQ(cfr_get_cell(vt, 0, 3)->width, 0);
    ASSERT_EQ(cfr_get_cell(vt, 1, 0)->cp, (uint32_t)'c');
    ASSERT_EQ(cfr_get_cell(vt, 1, 1)->cp, (uint32_t)'d');
    ASSERT_TRUE(cfr_row_is_continuation(vt, 1));
    cfr_free(vt);
}

static void test_keycap_sequence_wraps(void)
{
    /* 1️⃣ ('1' + VS16 + U+20E3) is one width-2 cluster; at the margin it
     * wraps eagerly like any wide cluster. */
    CfrTerm *vt = make_term(4, 4, false, false);
    feed(vt, "abc"
             "1\xEF\xB8\x8F\xE2\x83\xA3");
    ASSERT_EQ(cfr_get_cell(vt, 1, 0)->cp, (uint32_t)'1');
    ASSERT_EQ(cfr_get_cell(vt, 1, 0)->width, 2);
    ASSERT_NEQ(cfr_get_cell(vt, 1, 0)->grapheme_id, 0u);
    ASSERT_TRUE(cfr_row_is_continuation(vt, 1));
    cfr_free(vt);
}

static void test_zwj_family_wraps_as_unit(void)
{
    /* 👨‍👩 is a single width-2 cluster spanning three codepoints. */
    CfrTerm *vt = make_term(4, 6, false, false);
    feed(vt, "abcd"
             "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9"
             "x");
    const CfrCell *family = cfr_get_cell(vt, 0, 4);
    ASSERT_EQ(family->cp, 0x1F468u);
    ASSERT_EQ(family->width, 2);
    ASSERT_NEQ(family->grapheme_id, 0u);
    ASSERT_EQ(cfr_get_cell(vt, 0, 5)->width, 0);
    ASSERT_EQ(cfr_get_cell(vt, 1, 0)->cp, (uint32_t)'x');
    ASSERT_TRUE(cfr_row_is_continuation(vt, 1));
    cfr_free(vt);

    /* With only one column left the whole cluster wraps eagerly. */
    vt = make_term(4, 5, false, false);
    feed(vt, "abcd"
             "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9");
    ASSERT_EQ(cfr_get_cell(vt, 1, 0)->cp, 0x1F468u);
    ASSERT_EQ(cfr_get_cell(vt, 1, 0)->width, 2);
    ASSERT_TRUE(cfr_row_is_continuation(vt, 1));
    cfr_free(vt);
}

/* ------------------------------------------------------------------ */
/* Reflow of wrapped emoji lines                                      */
/* ------------------------------------------------------------------ */

static void test_reflow_shrink_rewraps_emoji_line(void)
{
    CfrTerm *vt = make_term(4, 6, false, true);
    feed(vt, "\xE2\x9A\xA0\xEF\xB8\x8F\xE2\x9A\xA0\xEF\xB8\x8F\xE2\x9A\xA0\xEF\xB8\x8F\xE2\x9A\xA0\xEF\xB8\x8F");
    cfr_resize(vt, 4, 5); /* two per row now */
    ASSERT_EQ(cfr_get_cell(vt, 0, 0)->cp, 0x26A0u);
    ASSERT_EQ(cfr_get_cell(vt, 0, 0)->width, 2);
    ASSERT_EQ(cfr_get_cell(vt, 0, 2)->cp, 0x26A0u);
    ASSERT_EQ(cfr_get_cell(vt, 0, 2)->width, 2);
    ASSERT_EQ(cfr_get_cell(vt, 0, 4)->width, 0);
    ASSERT_EQ(cfr_get_cell(vt, 1, 0)->cp, 0x26A0u);
    ASSERT_EQ(cfr_get_cell(vt, 1, 2)->cp, 0x26A0u);
    ASSERT_TRUE(cfr_row_is_continuation(vt, 1));
    ASSERT_FALSE(cfr_row_is_continuation(vt, 2));
    cfr_free(vt);
}

static void test_reflow_grow_unwraps_emoji_line(void)
{
    CfrTerm *vt = make_term(4, 5, false, true);
    feed(vt, "\xE2\x9A\xA0\xEF\xB8\x8F\xE2\x9A\xA0\xEF\xB8\x8F\xE2\x9A\xA0\xEF\xB8\x8F\xE2\x9A\xA0\xEF\xB8\x8F");
    ASSERT_TRUE(cfr_row_is_continuation(vt, 1));
    cfr_resize(vt, 4, 8); /* all four fit on one row */
    ASSERT_EQ(cfr_get_cell(vt, 0, 0)->cp, 0x26A0u);
    ASSERT_EQ(cfr_get_cell(vt, 0, 6)->cp, 0x26A0u);
    ASSERT_EQ(cfr_get_cell(vt, 0, 6)->width, 2);
    ASSERT_FALSE(cfr_row_is_continuation(vt, 1));
    ASSERT_EQ(cfr_get_cell(vt, 1, 0)->cp, 0u);
    cfr_free(vt);
}

/* ------------------------------------------------------------------ */
/* Overwriting a wide cluster                                         */
/* ------------------------------------------------------------------ */

static void test_overwrite_wide_lead_blanks_continuation(void)
{
    /* Printing a narrow character over the lead of a wide cluster must
     * not strand a width-0 continuation cell (renderers skip it — a
     * hole). The column becomes an ordinary blank. */
    CfrTerm *vt = make_term(4, 8, false, false);
    feed(vt, "ab\xF0\x9F\x98\x80"
             "cd");
    feed(vt, "\x1b[1;3HZ"); /* overwrite the emoji's lead cell */
    ASSERT_EQ(cfr_get_cell(vt, 0, 2)->cp, (uint32_t)'Z');
    ASSERT_EQ(cfr_get_cell(vt, 0, 2)->width, 1);
    ASSERT_EQ(cfr_get_cell(vt, 0, 3)->width, 1);
    ASSERT_EQ(cfr_get_cell(vt, 0, 3)->cp, 0x20u);
    ASSERT_EQ(cfr_get_cell(vt, 0, 4)->cp, (uint32_t)'c');
    ASSERT_EQ(cfr_get_cell(vt, 0, 5)->cp, (uint32_t)'d');
    cfr_free(vt);
}

static void test_overwrite_wide_margin_pair_severs_wrap(void)
{
    /* The wrap edge rides the continuation cell of a margin wide pair;
     * overwriting the lead blanks that cell, and rewriting the margin
     * severs the join (the §2.2 "print overwriting margin" rule). */
    CfrTerm *vt = make_term(4, 6, false, false);
    feed(vt, "aaaa\xE2\x9A\xA0\xEF\xB8\x8F"
             "b");
    ASSERT_TRUE(cfr_row_is_continuation(vt, 1));
    feed(vt, "\x1b[1;5HZ"); /* overwrite the ⚠️ lead at the margin pair */
    ASSERT_EQ(cfr_get_cell(vt, 0, 4)->cp, (uint32_t)'Z');
    ASSERT_EQ(cfr_get_cell(vt, 0, 4)->width, 1);
    ASSERT_EQ(cfr_get_cell(vt, 0, 5)->width, 1);
    ASSERT_EQ(cfr_get_cell(vt, 0, 5)->cp, 0x20u);
    ASSERT_FALSE(cfr_row_is_continuation(vt, 1));
    cfr_free(vt);
}

/* ------------------------------------------------------------------ */
/* Presentation selectors split across writes                         */
/* ------------------------------------------------------------------ */

static void test_vs16_split_across_writes_attaches(void)
{
    /* PTY frames can split a base from its VS16; the end-of-write flush
     * must not lose the selector (the row would wrap one cell early). */
    CfrTerm *vt = make_term(4, 8, false, false);
    feed(vt, "\xE2\x9A\xA0");
    feed(vt, "\xEF\xB8\x8F");
    feed(vt, "x");
    const CfrCell *cell = cfr_get_cell(vt, 0, 0);
    ASSERT_EQ(cell->cp, 0x26A0u);
    ASSERT_EQ(cell->width, 2);
    ASSERT_NEQ(cell->grapheme_id, 0u);
    uint32_t cps[8] = { 0 };
    size_t n = cfr_cell_get_grapheme(vt, cell, cps, 8);
    ASSERT_EQ(n, 2u);
    ASSERT_EQ(cps[0], 0x26A0u);
    ASSERT_EQ(cps[1], 0xFE0Fu);
    ASSERT_EQ(cfr_get_cell(vt, 0, 1)->width, 0);
    ASSERT_EQ(cfr_get_cell(vt, 0, 2)->cp, (uint32_t)'x');
    cfr_free(vt);
}

static void test_vs15_split_cancels_vs16(void)
{
    /* ❤️ written in one frame, VS15 in the next: the combined cluster
     * is [2764 FE0F FE0E] and the last selector narrows it back to 1. */
    CfrTerm *vt = make_term(4, 8, false, false);
    feed(vt, "\xE2\x9D\xA4\xEF\xB8\x8F");
    ASSERT_EQ(cfr_get_cell(vt, 0, 0)->width, 2);
    feed(vt, "\xEF\xB8\x8E");
    const CfrCell *cell = cfr_get_cell(vt, 0, 0);
    ASSERT_EQ(cell->width, 1);
    uint32_t cps[8] = { 0 };
    size_t n = cfr_cell_get_grapheme(vt, cell, cps, 8);
    ASSERT_EQ(n, 3u);
    ASSERT_EQ(cps[1], 0xFE0Fu);
    ASSERT_EQ(cps[2], 0xFE0Eu);
    /* The old continuation is blanked, not stranded. */
    ASSERT_EQ(cfr_get_cell(vt, 0, 1)->width, 1);
    ASSERT_EQ(cfr_get_cell(vt, 0, 1)->cp, 0x20u);
    cfr_free(vt);
}

static void test_vs16_split_at_margin_wraps(void)
{
    /* The base was printed at the margin; the separately-arriving VS16
     * widens it, which no longer fits — the attach re-runs the wrap: the
     * row is blanked at the margin and the emoji moves to the next row. */
    CfrTerm *vt = make_term(4, 4, false, false);
    feed(vt, "abc");
    feed(vt, "\xE2\x9A\xA0"); /* narrow ⚠ in the last column, phantom */
    ASSERT_EQ(cfr_get_cell(vt, 0, 3)->width, 1);
    feed(vt, "\xEF\xB8\x8F"); /* widens it to 2 cells */
    ASSERT_EQ(cfr_get_cell(vt, 0, 3)->cp, 0x20u);
    ASSERT_EQ(cfr_get_cell(vt, 0, 3)->width, 1);
    ASSERT_EQ(cfr_get_cell(vt, 1, 0)->cp, 0x26A0u);
    ASSERT_EQ(cfr_get_cell(vt, 1, 0)->width, 2);
    ASSERT_EQ(cfr_get_cell(vt, 1, 1)->width, 0);
    ASSERT_TRUE(cfr_row_is_continuation(vt, 1));
    cfr_free(vt);
}

static void test_combining_mark_split_across_writes(void)
{
    /* Same split case with a plain combining mark. */
    CfrTerm *vt = make_term(4, 8, false, false);
    feed(vt, "e");
    feed(vt, "\xCC\x81");
    feed(vt, "f");
    const CfrCell *cell = cfr_get_cell(vt, 0, 0);
    ASSERT_EQ(cell->cp, (uint32_t)'e');
    ASSERT_EQ(cell->width, 1);
    uint32_t cps[4] = { 0 };
    ASSERT_EQ(cfr_cell_get_grapheme(vt, cell, cps, 4), 2u);
    ASSERT_EQ(cps[1], 0x0301u);
    ASSERT_EQ(cfr_get_cell(vt, 0, 1)->cp, (uint32_t)'f');
    cfr_free(vt);
}

static void test_stray_selector_dropped(void)
{
    /* A selector with nothing before it in the row is discarded — it
     * must not become a baseless width-2 cell. */
    CfrTerm *vt = make_term(4, 8, false, false);
    feed(vt, "\xEF\xB8\x8F");
    ASSERT_EQ(cfr_get_cell(vt, 0, 0)->cp, 0u);
    cfr_free(vt);

    /* After an erase, the preceding cell is a blank: attaching would
     * widen a space, so the selector is dropped there too — the cursor
     * never moves and no cell becomes wide. */
    vt = make_term(4, 8, false, false);
    feed(vt, "ab");
    feed(vt, "\x1b[2K");
    feed(vt, "\xEF\xB8\x8F");
    ASSERT_EQ(vt->cursor.row, 0);
    ASSERT_EQ(vt->cursor.col, 2);
    for (int c = 0; c < 8; ++c) {
        ASSERT_EQ(cfr_get_cell(vt, 0, c)->width, 1);
        ASSERT_EQ(cfr_get_cell(vt, 0, c)->grapheme_id, 0u);
    }
    cfr_free(vt);
}

/* ------------------------------------------------------------------ */
/* cfr_utf8_display_width vs the grid                                 */
/* ------------------------------------------------------------------ */

static void test_utf8_display_width_presentation(void)
{
    ASSERT_EQ(cfr_utf8_display_width("\xE2\x9A\xA0", 3), 1);
    ASSERT_EQ(cfr_utf8_display_width("\xE2\x9A\xA0\xEF\xB8\x8F", 6), 2);
    ASSERT_EQ(cfr_utf8_display_width("\xE2\x9D\xA4\xEF\xB8\x8E", 6), 1);
    /* Ambiguous bases are narrow without a CfrTerm. */
    ASSERT_EQ(cfr_utf8_display_width("\xE2\x96\xB6", 3), 1);
    ASSERT_EQ(cfr_utf8_display_width("\xE2\x96\xB6\xEF\xB8\x8F", 6), 2);
}

static void test_utf8_display_width_clusters(void)
{
    /* RI pair, ZWJ family, keycap: 2 cells each, matching the grid. */
    ASSERT_EQ(cfr_utf8_display_width("\xF0\x9F\x87\xA9\xF0\x9F\x87\xB0", 8), 2);
    ASSERT_EQ(cfr_utf8_display_width("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9", 11), 2);
    ASSERT_EQ(cfr_utf8_display_width("1\xEF\xB8\x8F\xE2\x83\xA3", 7), 2);
    ASSERT_EQ(cfr_utf8_display_width("e\xCC\x81", 3), 1);
    ASSERT_EQ(cfr_utf8_display_width("a\xE4\xB8\xAD"
                                     "b",
                                     5),
              4);
}

static void test_utf8_display_width_matches_grid(void)
{
    /* Invariant: feeding a string into a wide-enough terminal lays out
     * exactly as many cells as cfr_utf8_display_width reports. */
    static const char *strs[] = {
        "a",
        "ab\xE2\x9A\xA0\xEF\xB8\x8F"
        "cd",
        "\xE4\xB8\xAD\xE4\xB8\xAD",
        "\xF0\x9F\x87\xA9\xF0\x9F\x87\xB0"
        "x",
        "e\xCC\x81"
        "f",
        "1\xEF\xB8\x8F\xE2\x83\xA3"
        "!",
        "\xE2\x9A\xA0\xEF\xB8\x8E"
        "z",
    };
    for (size_t i = 0; i < sizeof(strs) / sizeof(strs[0]); ++i) {
        CfrTerm *vt = make_term(2, 40, false, false);
        feed(vt, strs[i]);
        int grid_cells = 0;
        for (int c = 0; c < 40; ++c)
            grid_cells += cfr_get_cell(vt, 0, c)->width;
        int measured = cfr_utf8_display_width(strs[i], strlen(strs[i]));
        ASSERT_EQ(grid_cells, measured);
        cfr_free(vt);
    }
}

static void test_utf8_display_width_baseless_selector(void)
{
    /* A selector with no base is dropped by the grid; the measuring
     * helper agrees instead of counting it as a width-2 cell. */
    ASSERT_EQ(cfr_utf8_display_width("\xEF\xB8\x8F", 3), 0);
    ASSERT_EQ(cfr_utf8_display_width("\xEF\xB8\x8F"
                                     "a",
                                     4),
              1);
    ASSERT_EQ(cfr_utf8_display_width("\xCC\x81", 2), 0);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    RUN_TEST(test_cluster_width_presentation_selectors);
    RUN_TEST(test_vs15_never_narrows_wide_base);
    RUN_TEST(test_vs15_ambiguous_base_keeps_setting);
    RUN_TEST(test_ambiguous_narrow_fits_margin);
    RUN_TEST(test_ambiguous_wide_eager_wrap);
    RUN_TEST(test_vs16_eager_wrap);
    RUN_TEST(test_vs16_fits_exactly);
    RUN_TEST(test_vs15_stays_narrow_at_margin);
    RUN_TEST(test_vs15_ambiguous_wide_wraps);
    RUN_TEST(test_vs16_no_autowrap_overwrites_margin);
    RUN_TEST(test_ambiguous_wide_pure_line_wraps);
    RUN_TEST(test_multi_emoji_line_wraps_and_joins);
    RUN_TEST(test_emoji_wrap_at_bottom_scrolls);
    RUN_TEST(test_regional_indicator_pair_wraps_as_unit);
    RUN_TEST(test_keycap_sequence_wraps);
    RUN_TEST(test_zwj_family_wraps_as_unit);
    RUN_TEST(test_reflow_shrink_rewraps_emoji_line);
    RUN_TEST(test_reflow_grow_unwraps_emoji_line);
    RUN_TEST(test_overwrite_wide_lead_blanks_continuation);
    RUN_TEST(test_overwrite_wide_margin_pair_severs_wrap);
    RUN_TEST(test_vs16_split_across_writes_attaches);
    RUN_TEST(test_vs15_split_cancels_vs16);
    RUN_TEST(test_vs16_split_at_margin_wraps);
    RUN_TEST(test_combining_mark_split_across_writes);
    RUN_TEST(test_stray_selector_dropped);
    RUN_TEST(test_utf8_display_width_presentation);
    RUN_TEST(test_utf8_display_width_clusters);
    RUN_TEST(test_utf8_display_width_baseless_selector);
    RUN_TEST(test_utf8_display_width_matches_grid);

    TEST_SUMMARY();
}
