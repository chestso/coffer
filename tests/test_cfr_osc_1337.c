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
/* A minimal 1x1 red RGBA PNG, base64-encoded. */
static const char *PNG_1X1_RED_B64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==";

/* A 4x4 green RGBA PNG, base64-encoded (96 chars). */
static const char *PNG_4X4_GREEN_B64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAQAAAAECAYAAACp8Z5+AAAAD0lEQVR4nGNg+I8GSRcAACxQH+H81I9XAAAAAElFTkSuQmCC";

/* A 2x2 RGBA PNG with semi-transparent pixels (r=128, g=64, b=32, a=128). */
static const char *PNG_2X2_SEMI_ALPHA_B64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAEUlEQVR4nGNocFBoAGEGGAMAMdIFgRA3teIAAAAASUVORK5CYII=";

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
    const CfrImage *imgs = cfr_get_images(vt, &count);
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
    const CfrImage *imgs = cfr_get_images(vt, &count);
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
    const CfrImage *imgs = cfr_get_images(vt, &count);
    ASSERT_EQ(count, 1);
    int row_before = imgs[0].row;

    /* Scroll the terminal by sending 24 newlines */
    feed(vt, "\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n");

    /* After scrolling, the image row should decrease (or be culled) */
    imgs = cfr_get_images(vt, &count);
    if (count > 0) {
        ASSERT_TRUE(imgs[0].row < row_before);
    }
    /* If count == 0, the image was culled (scrolled past scrollback) */

    cfr_free(vt);
}

/* --------------------------------------------------------------- */
/* 6. Multipart transfer (MultipartFile=/FilePart=/FileEnd)       */
/* --------------------------------------------------------------- */

static void test_multipart(void)
{
    CfrTerm *vt = make_term(24, 80);

    /* Split the 96-char base64 into 3 chunks of 32 chars each */
    const char *b64 = PNG_4X4_GREEN_B64;
    size_t b64_len = strlen(b64);
    size_t chunk_size = 32;

    /* Start: ESC ] 1337 ; MultipartFile = inline = 1 BEL */
    feed(vt, "\x1b]1337;MultipartFile=inline=1\x07");

    /* Send chunks */
    for (size_t off = 0; off < b64_len; off += chunk_size) {
        char seq[128];
        size_t this_len = b64_len - off;
        if (this_len > chunk_size)
            this_len = chunk_size;
        snprintf(seq, sizeof(seq), "\x1b]1337;FilePart=%.*s\x07",
                 (int)this_len, b64 + off);
        feed(vt, seq);
    }

    /* End */
    feed(vt, "\x1b]1337;FileEnd\x07");

    /* Should have assembled and stored the image */
    int count = 0;
    const CfrImage *imgs = cfr_get_images(vt, &count);
    ASSERT_NOT_NULL(imgs);
    ASSERT_EQ(count, 1);
    ASSERT_EQ(imgs[0].source, IMG_SRC_ITERM);
    ASSERT_EQ(imgs[0].width_px, 4);
    ASSERT_EQ(imgs[0].height_px, 4);
    /* Green pixel */
    ASSERT_EQ(imgs[0].rgba[1], 255);
    ASSERT_EQ(imgs[0].rgba[3], 255);

    cfr_free(vt);
}

/* --------------------------------------------------------------- */
/* 7. Width/height scaling in cells                                */
/* --------------------------------------------------------------- */

static void test_width_cells(void)
{
    CfrTerm *vt = make_term(24, 80);
    /* 4x4 px image, cell is 10x6 px. width=2 means 2 cells = 20px wide */
    char seq[256];
    snprintf(seq, sizeof(seq),
             "\x1b]1337;File=inline=1;width=2:%s\x07",
             PNG_4X4_GREEN_B64);
    feed(vt, seq);

    int count = 0;
    const CfrImage *imgs = cfr_get_images(vt, &count);
    ASSERT_NOT_NULL(imgs);
    ASSERT_EQ(count, 1);
    /* With width=2 (cells) and cell_w=10, display width should be 20px */
    ASSERT_EQ(imgs[0].width_px, 20);

    cfr_free(vt);
}

/* --------------------------------------------------------------- */
/* 8. Width in pixels                                              */
/* --------------------------------------------------------------- */

static void test_width_pixels(void)
{
    CfrTerm *vt = make_term(24, 80);
    char seq[256];
    snprintf(seq, sizeof(seq),
             "\x1b]1337;File=inline=1;width=16px:%s\x07",
             PNG_4X4_GREEN_B64);
    feed(vt, seq);

    int count = 0;
    const CfrImage *imgs = cfr_get_images(vt, &count);
    ASSERT_NOT_NULL(imgs);
    ASSERT_EQ(count, 1);
    ASSERT_EQ(imgs[0].width_px, 16);

    cfr_free(vt);
}

/* --------------------------------------------------------------- */
/* 9. Intermediate alpha preservation                              */
/* --------------------------------------------------------------- */

static void test_intermediate_alpha(void)
{
    CfrTerm *vt = make_term(24, 80);
    char seq[256];
    snprintf(seq, sizeof(seq),
             "\x1b]1337;File=inline=1:%s\x07",
             PNG_2X2_SEMI_ALPHA_B64);
    feed(vt, seq);

    int count = 0;
    const CfrImage *imgs = cfr_get_images(vt, &count);
    ASSERT_NOT_NULL(imgs);
    ASSERT_EQ(count, 1);
    ASSERT_EQ(imgs[0].width_px, 2);
    ASSERT_EQ(imgs[0].height_px, 2);
    /* Semi-transparent pixel: r=128, g=64, b=32, a=128 */
    ASSERT_EQ(imgs[0].rgba[0], 128);
    ASSERT_EQ(imgs[0].rgba[1], 64);
    ASSERT_EQ(imgs[0].rgba[2], 32);
    ASSERT_EQ(imgs[0].rgba[3], 128);

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
    RUN_TEST(test_multipart);
    RUN_TEST(test_width_cells);
    RUN_TEST(test_width_pixels);
    RUN_TEST(test_intermediate_alpha);

    TEST_SUMMARY();
}
