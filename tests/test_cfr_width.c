/* tests/test_cfr_width.c — unit tests for cfr_utf8_display_width() */

#include "test_helpers.h"
#include <coffer/coffer.h>
#include <string.h>

/* Test ASCII strings */
static void test_ascii_width(void)
{
    ASSERT_EQ(cfr_utf8_display_width("hello", 5), 5);
    ASSERT_EQ(cfr_utf8_display_width("https://example.com", 18), 18);
    ASSERT_EQ(cfr_utf8_display_width("", 0), 0);
    ASSERT_EQ(cfr_utf8_display_width(NULL, 0), 0);
}

/* Test CJK wide characters (width 2) */
static void test_cjk_width(void)
{
    /* U+3042 hiragana A - 3-byte UTF-8, width 2 */
    ASSERT_EQ(cfr_utf8_display_width("\xE3\x81\x82", 3), 2);

    /* U+4E2D CJK 'middle' - 3-byte UTF-8, width 2 */
    ASSERT_EQ(cfr_utf8_display_width("\xE4\xB8\xAD", 3), 2);

    /* Mixed ASCII + CJK: "a" (1) + "中" (2) = 3 */
    ASSERT_EQ(cfr_utf8_display_width("a\xE4\xB8\xAD", 4), 3);
}

/* Test emoji (width 2) */
static void test_emoji_width(void)
{
    /* U+1F389 party popper - 4-byte UTF-8, width 2 */
    ASSERT_EQ(cfr_utf8_display_width("\xF0\x9F\x8E\x89", 4), 2);

    /* U+1F600 grinning face - 4-byte UTF-8, width 2 */
    ASSERT_EQ(cfr_utf8_display_width("\xF0\x9F\x98\x80", 4), 2);
}

/* Test zero-width codepoints */
static void test_zero_width(void)
{
    /* U+0301 combining acute accent - 2-byte UTF-8, width 0 */
    ASSERT_EQ(cfr_utf8_display_width("\xCC\x81", 2), 0);

    /* U+FE0F VS16 - 3-byte UTF-8, width 0 */
    ASSERT_EQ(cfr_utf8_display_width("\xEF\xB8\x8F", 3), 0);

    /* U+200D ZWJ - 3-byte UTF-8, width 0 */
    ASSERT_EQ(cfr_utf8_display_width("\xE2\x80\x8D", 3), 0);

    /* U+200B zero-width space - 3-byte UTF-8, width 0 */
    ASSERT_EQ(cfr_utf8_display_width("\xE2\x80\x8B", 3), 0);

    /* FEFF BOM - 3-byte UTF-8, width 0 */
    ASSERT_EQ(cfr_utf8_display_width("\xEF\xBB\xBF", 3), 0);
}

/* Test combining sequence: base + combining mark */
static void test_combining_sequence(void)
{
    /* "e" (1) + U+0301 combining acute (0) = 1 */
    ASSERT_EQ(cfr_utf8_display_width("e\xCC\x81", 3), 1);

    /* "a" (1) + U+0300 grave (0) + U+0301 acute (0) = 1 */
    ASSERT_EQ(cfr_utf8_display_width("a\xCC\x80\xCC\x81", 5), 1);
}

/* Test invalid UTF-8 */
static void test_invalid_utf8(void)
{
    /* Invalid lead byte */
    ASSERT_EQ(cfr_utf8_display_width("\xFF", 1), -1);

    /* Truncated sequence */
    ASSERT_EQ(cfr_utf8_display_width("\xE3\x81", 2), -1);

    /* Invalid continuation byte */
    ASSERT_EQ(cfr_utf8_display_width("\xE3\x00\x82", 3), -1);
}

/* Test symbol / emoji-presentation widths.
 *
 * UAX #11 Wide means two cells; Miscellaneous Symbols and Dingbats
 * (0x2600-0x27BF) is NOT wide wholesale — only the Emoji_Presentation
 * members are (U+2693, U+2705), the rest are East Asian Neutral or
 * Ambiguous and stay one cell without a VS16. Table data comes from the
 * generated unicode_tables.h; these assertions pin the boundaries. */
static void test_symbol_width(void)
{
    /* Narrow: East Asian Neutral/Ambiguous, one cell bare */
    ASSERT_EQ(cfr_utf8_display_width("\xE2\x9C\x93", 3), 1); /* U+2713 ✓ */
    ASSERT_EQ(cfr_utf8_display_width("\xE2\x9C\x94", 3), 1); /* U+2714 ✔ */
    ASSERT_EQ(cfr_utf8_display_width("\xE2\x9C\x97", 3), 1); /* U+2717 ✗ */
    ASSERT_EQ(cfr_utf8_display_width("\xE2\x98\x85", 3), 1); /* U+2605 ★ (ambiguous, narrow by default) */
    ASSERT_EQ(cfr_utf8_display_width("\xE2\x9C\x8F", 3), 1); /* U+270F ✏ */

    /* Wide: Emoji_Presentation members of the same block */
    ASSERT_EQ(cfr_utf8_display_width("\xE2\x9A\x93", 3), 2); /* U+2693 ⚓ */
    ASSERT_EQ(cfr_utf8_display_width("\xE2\x9C\x85", 3), 2); /* U+2705 ✅ */

    /* VS16 widens a narrow base to two cells */
    ASSERT_EQ(cfr_utf8_display_width("\xE2\x9C\x8F\xEF\xB8\x8F", 6), 2); /* ✏️ */
    ASSERT_EQ(cfr_utf8_display_width("\xE2\x9A\xA0\xEF\xB8\x8F", 6), 2); /* ⚠️ */

    /* Skin-tone modifier is zero-width (Extend), so the emoji stays 2 */
    ASSERT_EQ(cfr_utf8_display_width("\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD", 8), 2); /* 👍🏽 */

    /* Format characters are zero-width: WORD JOINER, BOM */
    ASSERT_EQ(cfr_utf8_display_width("\xE2\x81\xA0", 3), 0); /* U+2060 */
}

/* Test URL-like strings (primary use case) */
static void test_url_width(void)
{
    ASSERT_EQ(cfr_utf8_display_width("https://example.com/path?query=1", 30), 30);
    ASSERT_EQ(cfr_utf8_display_width("http://localhost:8080", 20), 20);
}

int main(void)
{
    test_ascii_width();
    test_cjk_width();
    test_emoji_width();
    test_zero_width();
    test_combining_sequence();
    test_symbol_width();
    test_invalid_utf8();
    test_url_width();

    printf("PASS: all cfr_width tests passed\n");
    return 0;
}
