/*
 * coffer — UAX #11 East Asian Width + UAX #29 grapheme cluster width.
 *
 * The interval tables live in the generated unicode_tables.h (UCD-derived;
 * see the provenance banner there). They are sorted, contiguous {lo, hi}
 * lists looked up by binary search; the hot path (ASCII printable) is a
 * branch-free fast exit.
 *
 *   CFR_WIDE      UAX #11 W/F — always two cells.
 *   CFR_ZERO      Extend ∪ Control ∪ ZWJ ∪ Default_Ignorable — no cells.
 *   CFR_AMBIGUOUS UAX #11 A — two cells only when vt->ambiguous_wide.
 *
 * Cluster width handles the user's primary goal: VS16 forces a 1-cell
 * emoji to 2 cells, RI pairs are 2 cells, combining marks attach
 * without widening. When a cluster carries both VS16 and VS15, the
 * last selector in the cluster decides the presentation (UTS #51);
 * VS15 cancels the doubling but never narrows a Wide base.
 */

#include "coffer_internal.h"

#include <stdint.h>

#include "unicode_tables.h"

static int range_lookup(const CfrRange *table, size_t n, uint32_t cp)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (cp < table[mid].lo)
            hi = mid;
        else if (cp > table[mid].hi)
            lo = mid + 1;
        else
            return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* Width rules                                                      */
/* ---------------------------------------------------------------- */

int cfr_codepoint_width(CfrTerm *vt, uint32_t cp)
{
    /* ASCII fast path */
    if (cp < 0x7Fu) {
        if (cp < 0x20u)
            return 0;
        return 1;
    }
    if (cp < 0xA0u)
        return 0; /* DEL + C1 */
    if (cp == 0x00ADu)
        return 0; /* soft hyphen */

    if (range_lookup(CFR_ZERO, CFR_ZERO_LEN, cp))
        return 0;
    if (range_lookup(CFR_WIDE, CFR_WIDE_LEN, cp))
        return 2;

    if (vt && vt->ambiguous_wide && range_lookup(CFR_AMBIGUOUS, CFR_AMBIGUOUS_LEN, cp))
        return 2;
    return 1;
}

/* UAX #29-style helpers driven by UCD-generated tables. */

static bool is_extend(uint32_t cp)
{
    /* GB9: Extend ∪ ZWJ — no break before these. */
    return range_lookup(CFR_EXTEND, CFR_EXTEND_LEN, cp) != 0;
}

static bool is_spacing_mark(uint32_t cp)
{
    /* GB9a: SpacingMark — no break before these. */
    return range_lookup(CFR_SPACING_MARK, CFR_SPACING_MARK_LEN, cp) != 0;
}

static bool is_prepend(uint32_t cp)
{
    /* GB9b: Prepend — forces next char into this cluster. */
    return range_lookup(CFR_PREPEND, CFR_PREPEND_LEN, cp) != 0;
}

static bool is_extended_pictographic(uint32_t cp)
{
    return range_lookup(CFR_EXT_PICT, CFR_EXT_PICT_LEN, cp) != 0;
}

static bool is_regional_indicator(uint32_t cp)
{
    return cp >= 0x1F1E6u && cp <= 0x1F1FFu;
}

int cfr_cluster_width(CfrTerm *vt, const uint32_t *cps, uint32_t len)
{
    if (len == 0)
        return 0;
    /* Regional indicator pair → 2 cells (a flag). */
    if (len >= 2 && is_regional_indicator(cps[0]) && is_regional_indicator(cps[1]))
        return 2;
    /* Presentation selectors, scanned from the END: the last variation
     * selector in the cluster wins. A base may carry both FE0E and FE0F
     * (with or without intervening ZWJ), and the trailing one is the
     * effective request:
     *   VS16 (emoji presentation) → 2 cells, even when the base is
     *     UAX #11 Narrow or Ambiguous — this is the shift libvterm
     *     cannot express without a lookahead hack.
     *   VS15 (text presentation) → the base's East Asian width: it
     *     cancels VS16's doubling but never narrows a Wide base — CJK
     *     and emoji-presentation codepoints have no 1-cell glyph, and
     *     collapsing them to one cell would corrupt the column grid.
     *     Ambiguous bases keep the ambiguous_wide setting. */
    for (uint32_t i = len; i-- > 0;) {
        if (cps[i] == 0xFE0Fu)
            return 2;
        if (cps[i] == 0xFE0Eu)
            return cfr_codepoint_width(vt, cps[0]);
    }
    /* Otherwise: width of base codepoint. */
    return cfr_codepoint_width(vt, cps[0]);
}

/* True for a codepoint that would extend a pending cluster rather than
 * start a new one: GCB Extend ∪ ZWJ (combining marks, variation
 * selectors, joiners) or SpacingMark. print.c uses this to re-attach a
 * stray modifier whose base was already committed by an earlier flush
 * (the base and its VS16 arrived in separate input writes). */
bool cfr_is_grapheme_joiner(uint32_t cp)
{
    return is_extend(cp) || is_spacing_mark(cp);
}

bool cfr_grapheme_break_before(uint32_t prev, uint32_t cur, void *state_in)
{
    /* `state_in` is reserved for future per-cluster state (RI parity,
     * Pictographic+Extend* + ZWJ continuation). For now we use a
     * stateless approximation that handles the dominant cases. */
    (void)state_in;
    if (prev == 0u)
        return false; /* SOT — first codepoint of cluster */
    /* GB3: CR × LF */
    if (prev == 0x0Du && cur == 0x0Au)
        return false;
    /* GB4-5: control × any, any × control → break. Controls hit GROUND
     * before reaching the cluster accumulator, so this is mostly
     * defensive. */
    if (cur == 0x0Au || cur == 0x0Du || cur == 0x00)
        return true;
    /* GB9 / GB9a: × Extend, × ZWJ, × SpacingMark → no break. */
    if (is_extend(cur))
        return false;
    if (is_spacing_mark(cur))
        return false;
    /* GB9b: Prepend × any → no break. */
    if (is_prepend(prev))
        return false;
    /* GB11: \p{Extended_Pictographic} Extend* ZWJ × \p{Extended_Pictographic}.
     * Per UAX #29, ZWJ only suppresses a break when the following
     * character is Extended_Pictographic. This prevents mutt's
     * NBSP+SPACE+ZWJ fill pattern from over-merging: the NBSP after
     * ZWJ correctly starts a new cluster, so each NBSP+SPACE+ZWJ
     * triple occupies 2 cells as expected. The full stateful GB11
     * also requires that an Extended_Pictographic preceded the ZWJ;
     * we approximate by only checking the current character, which
     * is sufficient in practice since ZWJ emoji sequences always
     * have an EP codepoint on both sides. */
    if (prev == 0x200Du && is_extended_pictographic(cur))
        return false;
    /* GB12-13: ^(RI RI)* RI × RI. Without state we'd over-merge long
     * RI runs. Accept the pair-merge here only when prev is RI and cur
     * is RI; subsequent RI starts a fresh cluster naturally because
     * the cluster has already committed after this pair. */
    if (is_regional_indicator(prev) && is_regional_indicator(cur))
        return false;
    /* GB999: any ÷ any. */
    return true;
}

/* --- Public UTF-8 display width ---------------------------------------- */

#include <stddef.h>

/* Grid-equivalent width of one decoded cluster for the public measuring
 * helper: a cluster with no base (a leading modifier) is dropped by
 * commit_cluster(), so it measures 0 here too. */
static int measurable_cluster_width(const uint32_t *cps, uint32_t len)
{
    if (len == 0 || cfr_is_grapheme_joiner(cps[0]))
        return 0;
    return cfr_cluster_width(NULL, cps, len);
}

int cfr_utf8_display_width(const char *utf8, size_t len)
{
    if (!utf8 || len == 0)
        return 0;

    int width = 0;
    const uint8_t *s = (const uint8_t *)utf8;
    const uint8_t *end = s + len;

    /* Cluster-aware: codepoints are grouped with the same grapheme
     * break predicate and cluster width rule the grid uses, so the
     * measurement matches what cfr_input_write would lay out — an
     * emoji presentation sequence (base + VS16) measures 2 cells, a
     * regional indicator pair 2, combining marks 0. There is no
     * CfrTerm* here, so East Asian Ambiguous codepoints are always
     * narrow (ambiguous width is a per-terminal setting). */
    uint32_t cluster[CFR_CLUSTER_MAX];
    uint32_t clen = 0;

    while (s < end && *s) {
        uint32_t cp;
        int char_len;

        if (s[0] < 0x80) {
            cp = s[0];
            char_len = 1;
        } else if ((s[0] & 0xE0) == 0xC0) {
            cp = s[0] & 0x1F;
            char_len = 2;
        } else if ((s[0] & 0xF0) == 0xE0) {
            cp = s[0] & 0x0F;
            char_len = 3;
        } else if ((s[0] & 0xF8) == 0xF0) {
            cp = s[0] & 0x07;
            char_len = 4;
        } else {
            return -1; /* invalid UTF-8 lead byte */
        }

        if (s + char_len > end)
            return -1; /* truncated sequence */

        for (int i = 1; i < char_len; i++) {
            if ((s[i] & 0xC0) != 0x80)
                return -1; /* invalid continuation */
            cp = (cp << 6) | (s[i] & 0x3F);
        }

        if (clen > 0 && cfr_grapheme_break_before(cluster[clen - 1], cp, NULL)) {
            width += measurable_cluster_width(cluster, clen);
            clen = 0;
        }
        /* Same cap as the grid: a cluster past CFR_CLUSTER_MAX drops
         * further joiners instead of overflowing the stack buffer. */
        if (clen < CFR_CLUSTER_MAX)
            cluster[clen++] = cp;
        s += char_len;
    }
    if (clen > 0)
        width += measurable_cluster_width(cluster, clen);

    return width;
}
