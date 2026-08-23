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

static void test_query_capabilities(void)
{
    CfrTerm *vt = make_term(24, 80);
    feed(vt, "\x1b_Ga=q,i=0\x1b\\");
    ASSERT_TRUE(g_output_len > 0);
    ASSERT_TRUE(strstr(g_output, "i=0") != NULL);
    ASSERT_TRUE(strstr(g_output, "flags=") != NULL);
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
    const CfrImage *imgs = cfr_get_images(vt, &count);
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
    const CfrImage *imgs = cfr_get_images(vt, &count);
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
    cfr_get_images(vt, &count);
    ASSERT_EQ(count, 1);

    /* Delete all */
    g_output_len = 0;
    feed(vt, "\x1b_Ga=d,d=a\x1b\\");

    cfr_get_images(vt, &count);
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
/* 7. Transmit stores image under the client-assigned id           */
/* --------------------------------------------------------------- */

static void test_transmit_client_id(void)
{
    CfrTerm *vt = make_term(24, 80);

    uint8_t rgba[4] = { 255, 0, 0, 255 };
    char b64[64];
    b64_encode(rgba, sizeof(rgba), b64);
    char seq[256];
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=t,f=32,s=1,v=1,i=42;%s\x1b\\", b64);
    feed(vt, seq);

    int count = 0;
    const CfrImage *imgs = cfr_get_images(vt, &count);
    ASSERT_NOT_NULL(imgs);
    ASSERT_EQ(count, 1);
    ASSERT_EQ((long long)imgs[0].id, 42);

    cfr_free(vt);
}

/* --------------------------------------------------------------- */
/* 8. Place (a=p) stores a placement with z-index                 */
/* --------------------------------------------------------------- */

static void test_place(void)
{
    CfrTerm *vt = make_term(24, 80);

    uint8_t rgba[4] = { 255, 0, 0, 255 };
    char b64[64];
    b64_encode(rgba, sizeof(rgba), b64);
    char seq[256];
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=t,f=32,s=1,v=1,i=7;%s\x1b\\", b64);
    feed(vt, seq);

    /* Place with explicit placement id, z-index, cell offsets and size.
     * kitty uses w= (cell width) and h= (cell height) for the placement
     * box; x=/y= are the cell offsets from the cursor. */
    feed(vt, "\x1b_Ga=p,i=7,p=99,z=-2,x=3,y=4,w=2,h=3\x1b\\");

    int count = 0;
    const CfrImagePlacement *pls = cfr_img_get_placements(vt, vt->images, &count);
    ASSERT_NOT_NULL(pls);
    ASSERT_EQ(count, 1);
    ASSERT_EQ((long long)pls[0].image_id, 7);
    ASSERT_EQ((long long)pls[0].id, 99);
    ASSERT_EQ(pls[0].z_index, -2);
    ASSERT_EQ(pls[0].col, 3);
    ASSERT_EQ(pls[0].row, 4);
    ASSERT_EQ(pls[0].cols, 2);
    ASSERT_EQ(pls[0].rows, 3);

    cfr_free(vt);
}

/* --------------------------------------------------------------- */
/* 8b. Place uses c=/r= as cursor advance, not placement size      */
/* --------------------------------------------------------------- */

static void test_place_cursor_advance(void)
{
    CfrTerm *vt = make_term(24, 80);

    uint8_t rgba[4] = { 255, 0, 0, 255 };
    char b64[64];
    b64_encode(rgba, sizeof(rgba), b64);
    char seq[256];
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=t,f=32,s=1,v=1,i=7;%s\x1b\\", b64);
    feed(vt, seq);

    /* c=2 r=1 moves the cursor; the placement box is the default 1x1. */
    feed(vt, "\x1b_Ga=p,i=7,c=2,r=1\x1b\\");

    /* The cursor advanced right 2, down 1 from (0,0). */
    ASSERT_EQ(vt->cursor.col, 2);
    ASSERT_EQ(vt->cursor.row, 1);

    int count = 0;
    const CfrImagePlacement *pls = cfr_img_get_placements(vt, vt->images, &count);
    ASSERT_NOT_NULL(pls);
    ASSERT_EQ(count, 1);
    /* Placement anchored at original cursor (0,0), 1x1 cells. */
    ASSERT_EQ(pls[0].col, 0);
    ASSERT_EQ(pls[0].row, 0);
    ASSERT_EQ(pls[0].cols, 1);
    ASSERT_EQ(pls[0].rows, 1);

    cfr_free(vt);
}

/* --------------------------------------------------------------- */
/* 8c. Virtual placement (U=1) — cursor is not advanced            */
/* --------------------------------------------------------------- */

static void test_place_virtual(void)
{
    CfrTerm *vt = make_term(24, 80);

    uint8_t rgba[4] = { 255, 0, 0, 255 };
    char b64[64];
    b64_encode(rgba, sizeof(rgba), b64);
    char seq[256];
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=t,f=32,s=1,v=1,i=7;%s\x1b\\", b64);
    feed(vt, seq);

    /* A virtual placement leaves the cursor where it is. */
    feed(vt, "\x1b_Ga=p,i=7,U=1\x1b\\");
    ASSERT_EQ(vt->cursor.col, 0);
    ASSERT_EQ(vt->cursor.row, 0);

    cfr_free(vt);
}

/* --------------------------------------------------------------- */
/* 8d. Relative placement (P= parent id, X=/Y= offset)            */
/* --------------------------------------------------------------- */

static void test_place_relative(void)
{
    CfrTerm *vt = make_term(24, 80);

    uint8_t rgba[4] = { 255, 0, 0, 255 };
    char b64[64];
    b64_encode(rgba, sizeof(rgba), b64);
    char seq[256];

    /* Parent placement at cell (5,6). */
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=t,f=32,s=1,v=1,i=7;%s\x1b\\", b64);
    feed(vt, seq);
    feed(vt, "\x1b_Ga=p,i=7,p=100,x=5,y=6,U=1\x1b\\");

    /* Child relative to parent 100, offset x=2 y=3. */
    uint8_t rgba2[4] = { 0, 255, 0, 255 };
    b64_encode(rgba2, sizeof(rgba2), b64);
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=t,f=32,s=1,v=1,i=8;%s\x1b\\", b64);
    feed(vt, seq);
    feed(vt, "\x1b_Ga=p,i=8,P=100,x=2,y=3,U=1\x1b\\");

    int count = 0;
    const CfrImagePlacement *pls = cfr_img_get_placements(vt, vt->images, &count);
    ASSERT_NOT_NULL(pls);
    ASSERT_EQ(count, 2);

    /* Find the child (image_id 8). */
    const CfrImagePlacement *child = NULL;
    for (int i = 0; i < count; i++)
        if (pls[i].image_id == 8)
            child = &pls[i];
    ASSERT_NOT_NULL(child);
    ASSERT_EQ(child->col, 7); /* 5 + 2 */
    ASSERT_EQ(child->row, 9); /* 6 + 3 */

    cfr_free(vt);
}

/* --------------------------------------------------------------- */
/* 9. Delete by image id (d=i) and placement id (d=p)             */
/* --------------------------------------------------------------- */

static void test_delete_by_id(void)
{
    CfrTerm *vt = make_term(24, 80);

    uint8_t rgba[4] = { 255, 0, 0, 255 };
    char b64[64];
    b64_encode(rgba, sizeof(rgba), b64);
    char seq[256];
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=t,f=32,s=1,v=1,i=7;%s\x1b\\", b64);
    feed(vt, seq);
    feed(vt, "\x1b_Ga=p,i=7,p=99,z=0,x=0,y=0\x1b\\");

    int count = 0;
    cfr_img_get_placements(vt, vt->images, &count);
    ASSERT_EQ(count, 1);

    /* Delete by placement id */
    feed(vt, "\x1b_Ga=d,d=p,p=99\x1b\\");
    cfr_img_get_placements(vt, vt->images, &count);
    ASSERT_EQ(count, 0);

    /* Image record still lives (pixel data persists) */
    int ic = 0;
    cfr_get_images(vt, &ic);
    ASSERT_EQ(ic, 1);

    /* Delete by image id */
    feed(vt, "\x1b_Ga=d,d=i,i=7\x1b\\");
    cfr_get_images(vt, &ic);
    ASSERT_EQ(ic, 0);

    cfr_free(vt);
}

/* --------------------------------------------------------------- */
/* 10. Outbound response uses the OSC 5556 carrier on Windows      */
/* --------------------------------------------------------------- */

static void test_outbound_carrier(void)
{
    CfrTerm *vt = make_term(24, 80);

    feed(vt, "\x1b_Ga=q,i=1\x1b\\");

#ifdef _WIN32
    /* On Windows responses must leave via OSC 5556 (raw G payload),
     * because ConPTY strips outbound APC (ESC _ ... ESC \) too. */
    ASSERT_TRUE(g_output_len > 0);
    ASSERT_TRUE(strstr(g_output, "\x1b]5556;G") != NULL);
    ASSERT_TRUE(strstr(g_output, "OK") != NULL);
#else
    /* On POSIX responses use APC form. */
    ASSERT_TRUE(g_output_len > 0);
    ASSERT_TRUE(strstr(g_output, "\x1b_G") != NULL);
    ASSERT_TRUE(strstr(g_output, "OK") != NULL);
#endif

    cfr_free(vt);
}

/* --------------------------------------------------------------- */
/* 10. Chunked transmit (m=1 accumulate, m=0 final)               */
/* --------------------------------------------------------------- */

static void test_chunked_transmit(void)
{
    CfrTerm *vt = make_term(24, 80);

    /* 2x2 RGBA red — 16 bytes. Split into two base64 halves. */
    uint8_t rgba[16] = { 255, 0, 0, 255, 255, 0, 0, 255,
                         255, 0, 0, 255, 255, 0, 0, 255 };
    char full[64];
    b64_encode(rgba, sizeof(rgba), full);
    size_t half = strlen(full) / 2;
    /* Split into two chunks (pad independently is messy; here we just
     * send the whole payload as a single chunk the simple way, but split
     * at the character level across two m=1 messages). */
    char first[64], second[64];
    memcpy(first, full, half);
    first[half] = '\0';
    strcpy(second, full + half);

    char seq[512];
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=t,f=32,s=2,v=2,i=1,m=1;%s\x1b\\", first);
    feed(vt, seq);
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=t,f=32,s=2,v=2,i=1,m=0;%s\x1b\\", second);
    feed(vt, seq);

    int count = 0;
    const CfrImage *imgs = cfr_get_images(vt, &count);
    ASSERT_NOT_NULL(imgs);
    ASSERT_EQ(count, 1);
    ASSERT_EQ(imgs[0].source, IMG_SRC_KITTY);
    ASSERT_EQ(imgs[0].width_px, 2);
    ASSERT_EQ(imgs[0].rgba[0], 255);
    ASSERT_EQ(imgs[0].rgba[3], 255);

    cfr_free(vt);
}

/* Chunked transmit where continuation chunks omit the action key
 * (the form chafa emits: "Gm=1;..." with no a=). These must be routed
 * to transmit, not rejected as unknown actions. The final m=0 chunk
 * must also inherit the original action (a=T) so placement happens. */
static void test_chunked_transmit_no_action(void)
{
    CfrTerm *vt = make_term(24, 80);

    uint8_t rgba[16] = { 255, 0, 0, 255, 255, 0, 0, 255,
                         255, 0, 0, 255, 255, 0, 0, 255 };
    char full[64];
    b64_encode(rgba, sizeof(rgba), full);
    size_t half = strlen(full) / 2;
    char first[64], second[64];
    memcpy(first, full, half);
    first[half] = '\0';
    strcpy(second, full + half);

    char seq[512];
    g_output_len = 0;
    g_output[0] = '\0';
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=T,f=32,s=2,v=2,i=1,m=1;%s\x1b\\", first);
    feed(vt, seq);
    snprintf(seq, sizeof(seq),
             "\x1b_Gm=1;%s\x1b\\", second);
    feed(vt, seq);
    snprintf(seq, sizeof(seq), "\x1b_Gm=0\x1b\\");
    feed(vt, seq);

    /* No unknown-action errors should have been emitted. */
    ASSERT_TRUE(strstr(g_output, "EINVAL") == NULL);

    int count = 0;
    const CfrImage *imgs = cfr_get_images(vt, &count);
    ASSERT_NOT_NULL(imgs);
    ASSERT_EQ(count, 1);
    ASSERT_EQ(imgs[0].source, IMG_SRC_KITTY);
    ASSERT_EQ(imgs[0].width_px, 2);
    ASSERT_EQ(imgs[0].rgba[0], 255);
    ASSERT_EQ(imgs[0].rgba[3], 255);

    /* Placement created at cursor (a=T places at cursor). */
    int pc = 0;
    const CfrImagePlacement *pls = cfr_get_image_placements(vt, &pc);
    ASSERT_NOT_NULL(pls);
    ASSERT_EQ(pc, 1);
    ASSERT_EQ((long long)pls[0].image_id, 1);

    cfr_free(vt);
}

/* --------------------------------------------------------------- */
/* 11. Animation: a=a control (sets current frame) + a=f frame data */
/* --------------------------------------------------------------- */

static void test_animate_put_frame(void)
{
    CfrTerm *vt = make_term(24, 80);

    /* Place an image with z-index in a negative layer. */
    uint8_t rgba[4] = { 255, 0, 0, 255 };
    char b64[64];
    b64_encode(rgba, sizeof(rgba), b64);
    char seq[256];
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=t,f=32,s=1,v=1,i=7;%s\x1b\\", b64);
    feed(vt, seq);

    /* a=f frame data with same id reuses/replaces */
    uint8_t frame2[4] = { 0, 255, 0, 255 };
    b64_encode(frame2, sizeof(frame2), b64);
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=f,f=32,s=1,v=1,i=7;%s\x1b\\", b64);
    feed(vt, seq);

    int count = 0;
    const CfrImage *imgs = cfr_get_images(vt, &count);
    ASSERT_NOT_NULL(imgs);
    ASSERT_EQ(count, 1);
    ASSERT_EQ((long long)imgs[0].id, 7);
    /* rgba replaced with frame2 green pixel */
    ASSERT_EQ(imgs[0].rgba[1], 255);
    ASSERT_EQ(imgs[0].rgba[0], 0);

    cfr_free(vt);
}

/* --------------------------------------------------------------- */
/* 12. zlib-compressed transfer (o=z)                             */
/* --------------------------------------------------------------- */

static void test_zlib_transmit(void)
{
    CfrTerm *vt = make_term(24, 80);

    /* 2x2 red RGBA zlib-compressed (789c fbcfc0f0ff3f12060043cc07f9) */
    const char *b64 = "eJz7z8Dw/z8SBgBDzAf5";
    char seq[256];
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=t,f=32,s=2,v=2,i=1,o=z;%s\x1b\\", b64);
    feed(vt, seq);

    int count = 0;
    const CfrImage *imgs = cfr_get_images(vt, &count);
    ASSERT_NOT_NULL(imgs);
    ASSERT_EQ(count, 1);
    ASSERT_EQ(imgs[0].width_px, 2);
    ASSERT_EQ(imgs[0].height_px, 2);
    ASSERT_EQ(imgs[0].rgba[0], 255);
    ASSERT_EQ(imgs[0].rgba[3], 255);

    cfr_free(vt);
}

/* --------------------------------------------------------------- */
/* 13. Compose (a=c) and other advanced actions accepted w/o error */
/* --------------------------------------------------------------- */

static void test_compose_accepted(void)
{
    CfrTerm *vt = make_term(24, 80);

    /* Compose should reply OK, not error. */
    g_output_len = 0;
    feed(vt, "\x1b_Ga=c,i=1,r=1\x1b\\");
    ASSERT_TRUE(strstr(g_output, "OK") != NULL);
    ASSERT_TRUE(strstr(g_output, "EINVAL") == NULL);

    cfr_free(vt);
}

/* --------------------------------------------------------------- */
/* 14. Transmit-and-place (a=T) stores image and places at cursor  */
/* --------------------------------------------------------------- */

static void test_transmit_and_place(void)
{
    CfrTerm *vt = make_term(24, 80);

    uint8_t rgba[4] = { 255, 0, 0, 255 };
    char b64[64];
    b64_encode(rgba, sizeof(rgba), b64);
    char seq[256];
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=T,f=32,s=1,v=1,i=7;%s\x1b\\", b64);
    feed(vt, seq);

    /* Image stored */
    int count = 0;
    const CfrImage *imgs = cfr_get_images(vt, &count);
    ASSERT_NOT_NULL(imgs);
    ASSERT_EQ(count, 1);
    ASSERT_EQ((long long)imgs[0].id, 7);

    /* Placement created at cursor (0,0) */
    int pc = 0;
    const CfrImagePlacement *pls = cfr_get_image_placements(vt, &pc);
    ASSERT_NOT_NULL(pls);
    ASSERT_EQ(pc, 1);
    ASSERT_EQ((long long)pls[0].image_id, 7);
    ASSERT_EQ(pls[0].col, 0);
    ASSERT_EQ(pls[0].row, 0);

    /* Cursor advanced below the placement (1x1 image → 1 row, 1 col). */
    ASSERT_EQ(vt->cursor.row, 1);
    ASSERT_EQ(vt->cursor.col, 1);

    cfr_free(vt);
}

/* 14b. a=T with explicit c=/r= cursor advance */
static void test_transmit_and_place_cursor_advance(void)
{
    CfrTerm *vt = make_term(24, 80);

    uint8_t rgba[4] = { 255, 0, 0, 255 };
    char b64[64];
    b64_encode(rgba, sizeof(rgba), b64);
    char seq[256];
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=T,f=32,s=1,v=1,i=7,c=3,r=2;%s\x1b\\", b64);
    feed(vt, seq);

    /* Cursor moved by c=3 cols, r=2 rows from (0,0). */
    ASSERT_EQ(vt->cursor.col, 3);
    ASSERT_EQ(vt->cursor.row, 2);

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
    RUN_TEST(test_query_capabilities);
    RUN_TEST(test_query_quiet);
    RUN_TEST(test_transmit_rgba);
    RUN_TEST(test_transmit_alpha);
    RUN_TEST(test_delete_all);
    RUN_TEST(test_osc5555_carrier);
    RUN_TEST(test_transmit_client_id);
    RUN_TEST(test_place);
    RUN_TEST(test_place_cursor_advance);
    RUN_TEST(test_place_virtual);
    RUN_TEST(test_place_relative);
    RUN_TEST(test_delete_by_id);
    RUN_TEST(test_outbound_carrier);
    RUN_TEST(test_chunked_transmit);
    RUN_TEST(test_chunked_transmit_no_action);
    RUN_TEST(test_animate_put_frame);
    RUN_TEST(test_zlib_transmit);
    RUN_TEST(test_compose_accepted);
    RUN_TEST(test_transmit_and_place);
    RUN_TEST(test_transmit_and_place_cursor_advance);

    TEST_SUMMARY();
}
