/* tests/test_cfr_graphics.c — kitty graphics protocol TDD tests */

#include "coffer_internal.h"
#include "test_helpers.h"
#include <coffer/coffer.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* --------------------------------------------------------------- */
/* Test harness                                                    */
/* --------------------------------------------------------------- */

static char g_output[1024];
static size_t g_output_len;

static void on_output(const uint8_t *bytes, size_t len, void *u)
{
    (void)u;
    if (g_output_len + len >= sizeof(g_output))
        return;
    memcpy(g_output + g_output_len, bytes, len);
    g_output_len += len;
    g_output[g_output_len] = '\0';
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
    cfr_set_cell_pixels(vt, 10, 6);
    g_output_len = 0;
    g_output[0] = '\0';
    return vt;
}

static void feed(CfrTerm *vt, const char *s)
{
    cfr_input_write(vt, (const uint8_t *)s, strlen(s));
}

/* Make a tiny 2x2 RGBA buffer. */
static uint8_t *make_rgba(int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    uint8_t *buf = malloc((size_t)w * h * 4);
    for (int i = 0; i < w * h; i++) {
        buf[i * 4 + 0] = r;
        buf[i * 4 + 1] = g;
        buf[i * 4 + 2] = b;
        buf[i * 4 + 3] = a;
    }
    return buf;
}

/* Base64-encode a small buffer (for test payloads). */
static void b64_encode(const uint8_t *data, size_t len, char *out)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t oi = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i + 1 < len)
            v |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len)
            v |= data[i + 2];
        out[oi++] = tbl[(v >> 18) & 63];
        out[oi++] = tbl[(v >> 12) & 63];
        out[oi++] = (i + 1 < len) ? tbl[(v >> 6) & 63] : '=';
        out[oi++] = (i + 2 < len) ? tbl[v & 63] : '=';
    }
    out[oi] = '\0';
}

/* --------------------------------------------------------------- */
/* 1. APC router: G prefix → graphics, else → lottie             */
/* --------------------------------------------------------------- */

static void test_apc_router_graphics(void)
{
    CfrTerm *vt = make_term(24, 80);

    /* Send a kitty query: ESC _ G a=q,i=1 ESC \ */
    feed(vt, "\x1b_Ga=q,i=1\x1b\\");

    /* Should get a response (OK) via output callback */
    ASSERT_TRUE(g_output_len > 0);
    ASSERT_TRUE(strstr(g_output, "OK") != NULL);

    cfr_free(vt);
}

static void test_apc_router_lottie(void)
{
    CfrTerm *vt = make_term(24, 80);

    /* Send a non-G APC (Lottie JSON) — should not crash and not
     * produce a kitty graphics response. */
    feed(vt, "\x1b{\"cmd\":\"noop\"}\x1b\\");
    /* No kitty response expected (lottie may or may not respond) */

    cfr_free(vt);
}

/* --------------------------------------------------------------- */
/* 2. Query (a=q) — responds with OK                               */
/* --------------------------------------------------------------- */

static void test_query(void)
{
    CfrTerm *vt = make_term(24, 80);
    feed(vt, "\x1b_Ga=q,i=42\x1b\\");
    ASSERT_TRUE(g_output_len > 0);
    ASSERT_TRUE(strstr(g_output, "i=42") != NULL);
    ASSERT_TRUE(strstr(g_output, "OK") != NULL);
    cfr_free(vt);
}

static void test_query_quiet(void)
{
    CfrTerm *vt = make_term(24, 80);
    /* q=2 suppresses all responses */
    feed(vt, "\x1b_Ga=q,i=1,q=2\x1b\\");
    ASSERT_EQ(g_output_len, (size_t)0);
    cfr_free(vt);
}

/* --------------------------------------------------------------- */
/* 3. Transmit (a=t) — RGBA stored with IMG_SRC_KITTY             */
/* --------------------------------------------------------------- */

static void test_transmit_rgba(void)
{
    CfrTerm *vt = make_term(24, 80);

    /* 2x2 RGBA red, opaque */
    uint8_t rgba[2 * 2 * 4] = { 255, 0, 0, 255, 255, 0, 0, 255,
                                255, 0, 0, 255, 255, 0, 0, 255 };
    char b64[64];
    b64_encode(rgba, sizeof(rgba), b64);

    char seq[256];
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=t,f=32,s=2,v=2,i=1;%s\x1b\\", b64);
    feed(vt, seq);

    int count = 0;
    const CfrSixel *imgs = cfr_get_sixels(vt, &count);
    ASSERT_NOT_NULL(imgs);
    ASSERT_EQ(count, 1);
    ASSERT_EQ(imgs[0].source, IMG_SRC_KITTY);
    ASSERT_EQ(imgs[0].width_px, 2);
    ASSERT_EQ(imgs[0].height_px, 2);
    ASSERT_EQ(imgs[0].rgba[0], 255);
    ASSERT_EQ(imgs[0].rgba[3], 255);

    /* Should also have emitted an OK response */
    ASSERT_TRUE(strstr(g_output, "OK") != NULL);

    cfr_free(vt);
}

/* --------------------------------------------------------------- */
/* 4. Transmit with intermediate alpha                             */
/* --------------------------------------------------------------- */

static void test_transmit_alpha(void)
{
    CfrTerm *vt = make_term(24, 80);

    /* 1x1 RGBA with alpha=128 */
    uint8_t rgba[4] = { 100, 200, 50, 128 };
    char b64[64];
    b64_encode(rgba, sizeof(rgba), b64);

    char seq[256];
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=t,f=32,s=1,v=1,i=1;%s\x1b\\", b64);
    feed(vt, seq);

    int count = 0;
    const CfrSixel *imgs = cfr_get_sixels(vt, &count);
    ASSERT_NOT_NULL(imgs);
    ASSERT_EQ(count, 1);
    ASSERT_EQ(imgs[0].rgba[0], 100);
    ASSERT_EQ(imgs[0].rgba[1], 200);
    ASSERT_EQ(imgs[0].rgba[2], 50);
    ASSERT_EQ(imgs[0].rgba[3], 128);

    cfr_free(vt);
}

/* --------------------------------------------------------------- */
/* 5. Delete all (a=d,d=a)                                          */
/* --------------------------------------------------------------- */

static void test_delete_all(void)
{
    CfrTerm *vt = make_term(24, 80);

    /* First transmit an image */
    uint8_t rgba[4] = { 255, 0, 0, 255 };
    char b64[64];
    b64_encode(rgba, sizeof(rgba), b64);
    char seq[256];
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=t,f=32,s=1,v=1,i=1;%s\x1b\\", b64);
    feed(vt, seq);

    int count = 0;
    cfr_get_sixels(vt, &count);
    ASSERT_EQ(count, 1);

    /* Delete all */
    g_output_len = 0;
    feed(vt, "\x1b_Ga=d,d=a\x1b\\");

    cfr_get_sixels(vt, &count);
    ASSERT_EQ(count, 0);

    cfr_free(vt);
}

/* --------------------------------------------------------------- */
/* 6. OSC 5555 carrier routes to graphics                           */
/* --------------------------------------------------------------- */

static void test_osc5555_carrier(void)
{
    CfrTerm *vt = make_term(24, 80);

    /* OSC 5555 with a raw kitty query (NOT base64 — kitty over
     * OSC 5555 sends the raw G... payload, not base64-encoded).
     * The Lottie path uses base64, but kitty uses raw text. */
    feed(vt, "\x1b]5555;Ga=q,i=1\x07");

    /* Should get a kitty response */
    ASSERT_TRUE(g_output_len > 0);
    ASSERT_TRUE(strstr(g_output, "OK") != NULL);

    cfr_free(vt);
}

/* --------------------------------------------------------------- */
/* main                                                           */
/* --------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);
    printf("Running kitty graphics protocol tests:\n");

    RUN_TEST(test_apc_router_graphics);
    RUN_TEST(test_apc_router_lottie);
    RUN_TEST(test_query);
    RUN_TEST(test_query_quiet);
    RUN_TEST(test_transmit_rgba);
    RUN_TEST(test_transmit_alpha);
    RUN_TEST(test_delete_all);
    RUN_TEST(test_osc5555_carrier);

    TEST_SUMMARY();
}
