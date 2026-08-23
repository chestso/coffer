/* tests/test_cfr_osc_1337.c — iTerm2 inline images (OSC 1337) TDD tests */

#include "coffer_internal.h"
#include "test_helpers.h"
#include <coffer/coffer.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* --------------------------------------------------------------- */
/* base64 helper for building large payloads                       */
/* --------------------------------------------------------------- */

static size_t b64_encoded_len(size_t n)
{
    return ((n + 2) / 3) * 4;
}

static void b64_encode(const uint8_t *in, size_t len, char *out)
{
    static const char T[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t a = in[i];
        uint32_t b = (i + 1 < len) ? in[i + 1] : 0;
        uint32_t c = (i + 2 < len) ? in[i + 2] : 0;
        uint32_t v = (a << 16) | (b << 8) | c;
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = (i + 1 < len) ? T[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < len) ? T[v & 63] : '=';
    }
    out[o] = '\0';
}

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

/* chafa emits `-f iterm2` pixels as a minimal uncompressed TIFF, not PNG.
 * This 2x2 RGBA chunky TIFF (top-down: red/green over blue/white) matches
 * that byte layout so the fallback decoder is exercised end-to-end. */
static const char *TIFF_2X2_RGBA_B64 =
    "SUkqABgAAAD/AAD/AP8A/wAA////////DAAAAQQAAQAAAAIAAAABAQQAAQAAAAIAAAACAQMABAAAAKoAAAADAQMAAQAAAAEAAAAGAQMAAQAAAAIAAAARAQQAAQAAAAgAAAASAQMAAQAAAAEAAAAVAQMAAQAAAAQAAAAWAQQAAQAAAAIAAAAXAQQAAQAAABAAAAAcAQMAAQAAAAEAAABSAQMAAQAAAAIAAAAIAAgACAAIAA==";

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
    /* 4x4 px image, cell is 10x6 px. width=2 means 2 cells = 20px wide.
     * width_px is physical (content_scale=1.0 so physical=logical).
     * buf_w/buf_h hold the native decoded size (4x4). */
    char seq[256];
    snprintf(seq, sizeof(seq),
             "\x1b]1337;File=inline=1;width=2:%s\x07",
             PNG_4X4_GREEN_B64);
    feed(vt, seq);

    int count = 0;
    const CfrImage *imgs = cfr_get_images(vt, &count);
    ASSERT_NOT_NULL(imgs);
    ASSERT_EQ(count, 1);
    ASSERT_EQ(imgs[0].width_px, 20);
    ASSERT_EQ(imgs[0].buf_w, 4);
    ASSERT_EQ(imgs[0].buf_h, 4);

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
    ASSERT_EQ(imgs[0].buf_w, 4);
    ASSERT_EQ(imgs[0].buf_h, 4);

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
/* 10. TIFF payload fallback (chafa -f iterm2)                     */
/* --------------------------------------------------------------- */

static void test_tiff_payload(void)
{
    CfrTerm *vt = make_term(24, 80);
    char seq[512];
    snprintf(seq, sizeof(seq),
             "\x1b]1337;File=inline=1:%s\x07",
             TIFF_2X2_RGBA_B64);
    feed(vt, seq);

    int count = 0;
    const CfrImage *imgs = cfr_get_images(vt, &count);
    ASSERT_NOT_NULL(imgs);
    ASSERT_EQ(count, 1);
    ASSERT_EQ(imgs[0].source, IMG_SRC_ITERM);
    ASSERT_EQ(imgs[0].width_px, 2);
    ASSERT_EQ(imgs[0].height_px, 2);
    /* Top-left must be red (TIFF rows are top-down, orientation 1). */
    ASSERT_EQ(imgs[0].rgba[0], 255);
    ASSERT_EQ(imgs[0].rgba[1], 0);
    ASSERT_EQ(imgs[0].rgba[2], 0);
    ASSERT_EQ(imgs[0].rgba[3], 255);
    /* Bottom-right must be white. */
    const uint8_t *last = imgs[0].rgba + (4 - 1) * 4;
    ASSERT_EQ(last[0], 255);
    ASSERT_EQ(last[1], 255);
    ASSERT_EQ(last[2], 255);
    ASSERT_EQ(last[3], 255);

    cfr_free(vt);
}

/* --------------------------------------------------------------- */
/* 11. Single OSC 1337 payload larger than 64KB                    */
/* --------------------------------------------------------------- */

static void test_large_single_shot(void)
{
    int w = 512;
    int h = 40;
    size_t npix = (size_t)w * (size_t)h;
    size_t raw = npix * 4u;

    size_t ifd_off = 8 + raw;
    size_t tiff_len = ifd_off + 2 + 11 * 12 + 8;

    uint8_t *tiff = malloc(tiff_len);
    ASSERT_NOT_NULL(tiff);
    memset(tiff, 0, tiff_len);

    tiff[0] = 'I';
    tiff[1] = 'I';
    tiff[2] = 42;
    tiff[3] = 0;
    tiff[4] = (uint8_t)(ifd_off & 0xff);
    tiff[5] = (uint8_t)((ifd_off >> 8) & 0xff);
    tiff[6] = (uint8_t)((ifd_off >> 16) & 0xff);
    tiff[7] = (uint8_t)((ifd_off >> 24) & 0xff);

    for (size_t i = 0; i < npix; i++) {
        uint8_t *px = tiff + 8 + i * 4;
        px[0] = (i == 0) ? 255 : 0;
        px[1] = (i == npix - 1) ? 255 : 0;
        px[2] = 0;
        px[3] = 255;
    }

#define PUT16(o, v)                          \
    do {                                     \
        tiff[(o)] = (uint8_t)((v) & 0xff);   \
        tiff[(o) + 1] = (uint8_t)((v) >> 8); \
    } while (0)
#define PUT32(o, v)                                    \
    do {                                               \
        tiff[(o)] = (uint8_t)((v) & 0xff);             \
        tiff[(o) + 1] = (uint8_t)(((v) >> 8) & 0xff);  \
        tiff[(o) + 2] = (uint8_t)(((v) >> 16) & 0xff); \
        tiff[(o) + 3] = (uint8_t)(((v) >> 24) & 0xff); \
    } while (0)

    PUT16(ifd_off, 11);
    size_t e = ifd_off + 2;
    PUT16(e, 256);
    PUT16(e + 2, 4);
    PUT32(e + 4, 1);
    PUT32(e + 8, (uint32_t)w);
    e += 12;
    PUT16(e, 257);
    PUT16(e + 2, 4);
    PUT32(e + 4, 1);
    PUT32(e + 8, (uint32_t)h);
    e += 12;
    PUT16(e, 258);
    PUT16(e + 2, 3);
    PUT32(e + 4, 4);
    PUT32(e + 8, (uint32_t)(ifd_off + 2 + 11 * 12));
    e += 12;
    PUT16(e, 262);
    PUT16(e + 2, 3);
    PUT32(e + 4, 1);
    PUT32(e + 8, 2);
    e += 12;
    PUT16(e, 273);
    PUT16(e + 2, 4);
    PUT32(e + 4, 1);
    PUT32(e + 8, 8);
    e += 12;
    PUT16(e, 274);
    PUT16(e + 2, 3);
    PUT32(e + 4, 1);
    PUT32(e + 8, 1);
    e += 12;
    PUT16(e, 277);
    PUT16(e + 2, 3);
    PUT32(e + 4, 1);
    PUT32(e + 8, 4);
    e += 12;
    PUT16(e, 278);
    PUT16(e + 2, 4);
    PUT32(e + 4, 1);
    PUT32(e + 8, (uint32_t)h);
    e += 12;
    PUT16(e, 279);
    PUT16(e + 2, 4);
    PUT32(e + 4, 1);
    PUT32(e + 8, (uint32_t)raw);
    e += 12;
    PUT16(e, 284);
    PUT16(e + 2, 3);
    PUT32(e + 4, 1);
    PUT32(e + 8, 1);
    e += 12;
    PUT16(e, 338);
    PUT16(e + 2, 3);
    PUT32(e + 4, 1);
    PUT32(e + 8, 2);
    e += 12;
    size_t bps = ifd_off + 2 + 11 * 12;
    PUT16(bps, 8);
    PUT16(bps + 2, 8);
    PUT16(bps + 4, 8);
    PUT16(bps + 6, 8);

#undef PUT16
#undef PUT32

    size_t b64_len = b64_encoded_len(tiff_len);
    char *b64 = malloc(b64_len + 1);
    ASSERT_NOT_NULL(b64);
    b64_encode(tiff, tiff_len, b64);

    size_t seq_len = strlen("\x1b]1337;File=inline=1:") + b64_len + 1;
    char *seq = malloc(seq_len + 1);
    ASSERT_NOT_NULL(seq);
    int n = snprintf(seq, seq_len + 1, "\x1b]1337;File=inline=1:%s\x07", b64);
    ASSERT_EQ(n, (int)seq_len);

    CfrTerm *vt = make_term(24, 80);
    cfr_input_write(vt, (const uint8_t *)seq, n);

    int count = 0;
    const CfrImage *imgs = cfr_get_images(vt, &count);
    ASSERT_NOT_NULL(imgs);
    ASSERT_EQ(count, 1);
    ASSERT_EQ(imgs[0].source, IMG_SRC_ITERM);
    ASSERT_EQ(imgs[0].width_px, w);
    ASSERT_EQ(imgs[0].height_px, h);
    ASSERT_EQ(imgs[0].rgba[0], 255);
    const uint8_t *last = imgs[0].rgba + (npix - 1) * 4;
    ASSERT_EQ(last[1], 255);

    cfr_free(vt);
    free(seq);
    free(b64);
    free(tiff);
}

/* --------------------------------------------------------------- */
/* 12. Physical dimensions with content_scale = 2.0                */
/* --------------------------------------------------------------- */

/* The single-conversion design: cfr_img_get converts logical→physical.
 * With content_scale=2.0, a 4x4 decoded image with width=2 cells should
 * have width_px = 2 * 5 * 2 = 20 (physical) and buf_w = 4 * 2 = 8
 * (physical).  The renderer uses these directly with zero conversions. */
static void test_content_scale_physical(void)
{
    CfrTerm *vt = make_term(24, 80);
    cfr_set_content_scale(vt, 2.0f);
    /* cell_w_px=10, cell_h_px=6 (physical, from make_term).
     * logical_cw = 10 / 2 = 5.  width=2 cells → disp_w = 2*5 = 10 (logical).
     * cfr_img_get: width_px = 10 * 2 = 20 (physical).
     * buf_w = 4 * 2 = 8 (physical). */
    char seq[256];
    snprintf(seq, sizeof(seq),
             "\x1b]1337;File=inline=1;width=2:%s\x07",
             PNG_4X4_GREEN_B64);
    feed(vt, seq);

    int count = 0;
    const CfrImage *imgs = cfr_get_images(vt, &count);
    ASSERT_NOT_NULL(imgs);
    ASSERT_EQ(count, 1);
    ASSERT_EQ(imgs[0].width_px, 20);
    ASSERT_EQ(imgs[0].height_px, 20);
    ASSERT_EQ(imgs[0].buf_w, 8);
    ASSERT_EQ(imgs[0].buf_h, 8);

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
    RUN_TEST(test_tiff_payload);
    RUN_TEST(test_large_single_shot);
    RUN_TEST(test_content_scale_physical);

    TEST_SUMMARY();
}
