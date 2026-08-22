/* tests/test_cfr_osc_1337.c — iTerm2 inline images (OSC 1337) TDD tests */

#include "coffer_internal.h"
#include "test_helpers.h"
#include <coffer/coffer.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* --------------------------------------------------------------- */
/* Test harness                                                    */
/* --------------------------------------------------------------- */

static char g_output_buf[1024];
static size_t g_output_len = 0;

static void on_output(const uint8_t *bytes, size_t len, void *u)
{
    (void)u;
    if (g_output_len + len >= sizeof(g_output_buf))
        return;
    memcpy(g_output_buf + g_output_len, bytes, len);
    g_output_len += len;
    g_output_buf[g_output_len] = '\0';
}

static CfrTerm *make_term(int rows, int cols)
{
    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.rows = rows;
    cfg.cols = cols;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 6;
    CfrTerm *vt = cfr_new(&cfg);
    CfrCallbacks cb = { 0 };
    cb.output = on_output;
    cfr_set_callbacks(vt, &cb, NULL);
    g_output_len = 0;
    g_output_buf[0] = '\0';
    return vt;
}

static void feed(CfrTerm *vt, const char *s)
{
    cfr_input_write(vt, (const uint8_t *)s, strlen(s));
}

/* A minimal 1x1 red RGBA PNG (67 bytes), base64-encoded.
 * Generated with: printf '\x89PNG\r\n\x1a\n' | ... (known-good bytes). */
static const char *PNG_1X1_RED_B64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==";

/* --------------------------------------------------------------- */
/* 1. Capabilities query                                           */
/* --------------------------------------------------------------- */

static void test_capabilities(void)
{
    CfrTerm *vt = make_term(24, 80);
    feed(vt, "\x1b]1337;Capabilities\x07");
    /* Should respond with the F flag (inline file support) */
    ASSERT_TRUE(g_output_len > 0);
    ASSERT_TRUE(strstr(g_output_buf, "Capabilities=") != NULL);
    ASSERT_TRUE(strstr(g_output_buf, "F") != NULL);
    cfr_free(vt);
}

/* --------------------------------------------------------------- */
/* 2. ReportCellSize query                                        */
/* --------------------------------------------------------------- */

static void test_report_cell_size(void)
{
    CfrTerm *vt = make_term(24, 80);
    feed(vt, "\x1b]1337;ReportCellSize\x07");
    /* Should respond with "ReportCellSize=<h>;<w>;<scale>" */
    ASSERT_TRUE(g_output_len > 0);
    ASSERT_TRUE(strstr(g_output_buf, "ReportCellSize=") != NULL);
    /* Cell size is 10x6, so response should contain "6" and "10" */
    ASSERT_TRUE(strstr(g_output_buf, "6") != NULL);
    ASSERT_TRUE(strstr(g_output_buf, "10") != NULL);
    cfr_free(vt);
}

/* --------------------------------------------------------------- */
/* 3. Inline PNG image                                            */
/* --------------------------------------------------------------- */

static void test_inline_png(void)
{
    CfrTerm *vt = make_term(24, 80);
    /* Feed: ESC ] 1337 ; File = inline = 1 : <base64-png> BEL */
    char seq[512];
    int n = snprintf(seq, sizeof(seq),
                     "\x1b]1337;File=inline=1:%s\x07",
                     PNG_1X1_RED_B64);
    feed(vt, seq);
    (void)n;

    /* Should have created an image in the store */
    int count = -1;
    const CfrSixel *imgs = cfr_get_sixels(vt, &count);
    ASSERT_NOT_NULL(imgs);
    ASSERT_EQ(count, 1);
    ASSERT_EQ(imgs[0].source, IMG_SRC_ITERM);
    ASSERT_EQ(imgs[0].width_px, 1);
    ASSERT_EQ(imgs[0].height_px, 1);
    /* Red pixel: R=255, G=0, B=0, A=255 */
    ASSERT_EQ(imgs[0].rgba[0], 255);
    ASSERT_EQ(imgs[0].rgba[1], 0);
    ASSERT_EQ(imgs[0].rgba[2], 0);
    ASSERT_EQ(imgs[0].rgba[3], 255);

    cfr_free(vt);
}

/* --------------------------------------------------------------- */
/* 4. Download mode (inline=0) is ignored                         */
/* --------------------------------------------------------------- */

static void test_download_ignored(void)
{
    CfrTerm *vt = make_term(24, 80);
    char seq[512];
    snprintf(seq, sizeof(seq),
             "\x1b]1337;File=inline=0:%s\x07",
             PNG_1X1_RED_B64);
    feed(vt, seq);

    int count = 0;
    const CfrSixel *imgs = cfr_get_sixels(vt, &count);
    ASSERT_EQ(count, 0);
    ASSERT_NULL(imgs);

    cfr_free(vt);
}

/* --------------------------------------------------------------- */
/* 5. Image scrolls with text                                     */
/* --------------------------------------------------------------- */

static void test_inline_scroll(void)
{
    CfrTerm *vt = make_term(24, 80);
    char seq[512];
    snprintf(seq, sizeof(seq),
             "\x1b]1337;File=inline=1:%s\x07",
             PNG_1X1_RED_B64);
    feed(vt, seq);

    int count = -1;
    const CfrSixel *imgs = cfr_get_sixels(vt, &count);
    ASSERT_EQ(count, 1);
    int row_before = imgs[0].row;

    /* Scroll the terminal by sending 24 newlines */
    feed(vt, "\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n");

    /* After scrolling, the image row should decrease (or be culled) */
    imgs = cfr_get_sixels(vt, &count);
    if (count > 0) {
        ASSERT_TRUE(imgs[0].row < row_before);
    }
    /* If count == 0, the image was culled (scrolled past scrollback) */

    cfr_free(vt);
}

/* --------------------------------------------------------------- */
/* main                                                           */
/* --------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);
    printf("Running OSC 1337 (iTerm2 inline images) tests:\n");

    RUN_TEST(test_capabilities);
    RUN_TEST(test_report_cell_size);
    RUN_TEST(test_inline_png);
    RUN_TEST(test_download_ignored);
    RUN_TEST(test_inline_scroll);

    TEST_SUMMARY();
}
