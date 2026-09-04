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
/* 8b. Place uses c=/r= as display size                            */
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

    /* c=2 r=1 display a 2x1-cell rectangle; the cursor advances by
     * the placement size. */
    feed(vt, "\x1b_Ga=p,i=7,c=2,r=1\x1b\\");

    /* The cursor advanced right 2, down 1 - 1 = 0 rows from (0,0). */
    ASSERT_EQ(vt->cursor.col, 2);
    ASSERT_EQ(vt->cursor.row, 0);

    int count = 0;
    const CfrImagePlacement *pls = cfr_img_get_placements(vt, vt->images, &count);
    ASSERT_NOT_NULL(pls);
    ASSERT_EQ(count, 1);
    /* Placement anchored at original cursor (0,0), 2x1 cells. */
    ASSERT_EQ(pls[0].col, 0);
    ASSERT_EQ(pls[0].row, 0);
    ASSERT_EQ(pls[0].cols, 2);
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

/* Chunked transmit where the first chunk carries only control data
 * and no payload at all — the exact form chafa emits for the first
 * message: "ESC _ G a=T,f=32,s=W,v=H,c=..,r=..,m=1,q=2 ESC \" (no
 * ';'). This used to crash: cfr_buf_append() with n=0 leaves the
 * accumulator NULL and the NUL-terminate wrote through it. */
static void test_chunked_transmit_control_only_first(void)
{
    CfrTerm *vt = make_term(24, 80);

    uint8_t rgba[4] = { 0, 255, 0, 255 };
    char b64[64];
    b64_encode(rgba, sizeof(rgba), b64);

    char seq[512];
    /* First chunk: control data only, m=1, no ';' and no payload. */
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=T,f=32,s=1,v=1,i=3,c=1,r=1,m=1,q=2\x1b\\");
    feed(vt, seq);
    /* Continuation chunk with the full payload. */
    snprintf(seq, sizeof(seq), "\x1b_Gm=1;%s\x1b\\", b64);
    feed(vt, seq);
    /* Final chunk: control only, no payload (chafa's m=0 form). */
    snprintf(seq, sizeof(seq), "\x1b_Gm=0\x1b\\");
    feed(vt, seq);

    int count = 0;
    const CfrImage *imgs = cfr_get_images(vt, &count);
    ASSERT_NOT_NULL(imgs);
    ASSERT_EQ(count, 1);
    ASSERT_EQ(imgs[0].source, IMG_SRC_KITTY);
    ASSERT_EQ(imgs[0].width_px, 1);
    ASSERT_EQ(imgs[0].rgba[1], 255);
    ASSERT_EQ(imgs[0].rgba[3], 255);

    /* a=T places at the cursor. */
    int pc = 0;
    const CfrImagePlacement *pls = cfr_get_image_placements(vt, &pc);
    ASSERT_NOT_NULL(pls);
    ASSERT_EQ(pc, 1);
    ASSERT_EQ((long long)pls[0].image_id, 3);

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

    /* Cursor after the placement: kitty's rule is col += cols,
     * row += rows - 1. A 1x1 image leaves the cursor on its own
     * row, one column right. */
    ASSERT_EQ(vt->cursor.row, 0);
    ASSERT_EQ(vt->cursor.col, 1);

    cfr_free(vt);
}

/* 14b. a=T with c=/r= display size */
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

    /* c=3,r=2 set the display size to 3x2 cells; the cursor advance
     * is by the placement size: col += 3, row += 2 - 1. */
    int pc = 0;
    const CfrImagePlacement *pls = cfr_get_image_placements(vt, &pc);
    ASSERT_EQ(pc, 1);
    ASSERT_EQ(pls[0].cols, 3);
    ASSERT_EQ(pls[0].rows, 2);
    ASSERT_EQ(vt->cursor.col, 3);
    ASSERT_EQ(vt->cursor.row, 1);

    cfr_free(vt);
}

/* 14c. a=T display-size keys survive a chunked upload */
static void test_transmit_chunked_keeps_display_size(void)
{
    CfrTerm *vt = make_term(30, 130);

    uint8_t rgba[4] = { 255, 0, 0, 255 };
    char b64[64];
    b64_encode(rgba, sizeof(rgba), b64);

    /* chafa's form: first chunk carries the control data (with the
     * display size), the final m=0 chunk carries only m=. */
    char seq[256];
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=T,f=32,s=1,v=1,c=99,r=29,m=1,q=2\x1b\\");
    feed(vt, seq);
    snprintf(seq, sizeof(seq),
             "\x1b_Gm=1;%s\x1b\\", b64);
    feed(vt, seq);
    feed(vt, "\x1b_Gm=0\x1b\\");

    /* The placement kept the first chunk's 99x29 display size. */
    int pc = 0;
    const CfrImagePlacement *pls = cfr_get_image_placements(vt, &pc);
    ASSERT_EQ(pc, 1);
    ASSERT_EQ(pls[0].cols, 99);
    ASSERT_EQ(pls[0].rows, 29);
    /* Kitty cursor rule: col 0 + 99 = 99 (no wrap), row 0 + 29 - 1. */
    ASSERT_EQ(vt->cursor.col, 99);
    ASSERT_EQ(vt->cursor.row, 28);

    cfr_free(vt);
}

/* 14d. C=1 leaves the cursor where it is */
static void test_transmit_no_cursor_move(void)
{
    CfrTerm *vt = make_term(24, 80);

    /* Park the cursor away from the origin. */
    feed(vt, "\x1b[5;10H");

    uint8_t rgba[4] = { 255, 0, 0, 255 };
    char b64[64];
    b64_encode(rgba, sizeof(rgba), b64);
    char seq[256];
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=T,f=32,s=1,v=1,i=7,C=1;%s\x1b\\", b64);
    feed(vt, seq);

    /* Placement created at (4,9), cursor untouched. */
    int pc = 0;
    const CfrImagePlacement *pls = cfr_get_image_placements(vt, &pc);
    ASSERT_EQ(pc, 1);
    ASSERT_EQ(pls[0].row, 4);
    ASSERT_EQ(pls[0].col, 9);
    ASSERT_EQ(vt->cursor.row, 4);
    ASSERT_EQ(vt->cursor.col, 9);

    cfr_free(vt);
}

/* 14e. The cursor advance uses rows - 1, so a near-full-height image
 * leaves the cursor one row above the bottom and a trailing newline
 * moves to the bottom-left without scrolling the grid.
 * Regression test for chafa --align mid,mid rendering, where the
 * old rows-count advance hit the bottom margin early and the
 * trailing newline scrolled the image up, clipping its top and
 * leaving two blank rows. */
static void test_transmit_full_width_no_extra_scroll(void)
{
    CfrTerm *vt = make_term(30, 130);

    /* chafa's shape: cursor parked at col 15 by leading spaces, then a
     * 99x29 display-size transmit. */
    uint8_t rgba[4] = { 255, 0, 0, 255 };
    char b64[64];
    b64_encode(rgba, sizeof(rgba), b64);
    char seq[256];
    feed(vt, "\x1b[1;16H"); /* park the cursor at col 15 like chafa's spaces */
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=T,f=32,s=1,v=1,c=99,r=29,m=1,q=2\x1b\\");
    feed(vt, seq);
    snprintf(seq, sizeof(seq),
             "\x1b_Gm=1;%s\x1b\\", b64);
    feed(vt, seq);
    feed(vt, "\x1b_Gm=0\x1b\\");

    /* col 15 + 99 = 114 (no wrap), row 0 + 29 - 1 = 28. */
    ASSERT_EQ(vt->cursor.row, 28);
    ASSERT_EQ(vt->cursor.col, 114);

    /* The trailing \r\n clients print after the image moves to the
     * bottom-left without scrolling: the image stays at row 0. */
    feed(vt, "\r\n");
    ASSERT_EQ(vt->cursor.row, 29);
    ASSERT_EQ(vt->cursor.col, 0);
    int pc = 0;
    const CfrImagePlacement *pls = cfr_get_image_placements(vt, &pc);
    ASSERT_EQ(pc, 1);
    ASSERT_EQ(pls[0].row, 0);

    cfr_free(vt);
}

/* --------------------------------------------------------------- */
/* 15. Id-less transmit always creates a new image                  */
/* --------------------------------------------------------------- */

/* The kitty spec allows multiple images with id=0 to coexist: a
 * transmit without an i= key must never reuse or replace a previous
 * id-less image. Regression test for chafa --clear slideshows, where
 * a second a=T without i= replaced the first image's pixels in place
 * and rendered the new image stretched inside the old placement while
 * also adding a second, correct placement. */
static void test_transmit_no_id_creates_new_image(void)
{
    CfrTerm *vt = make_term(24, 80);

    uint8_t rgba1[4] = { 255, 0, 0, 255 };
    char b64[64];
    b64_encode(rgba1, sizeof(rgba1), b64);
    char seq[128];
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=T,f=32,s=1,v=1;%s\x1b\\", b64);
    feed(vt, seq);

    uint8_t rgba2[4] = { 0, 255, 0, 255 };
    b64_encode(rgba2, sizeof(rgba2), b64);
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=T,f=32,s=1,v=1;%s\x1b\\", b64);
    feed(vt, seq);

    /* Two distinct images, each with its own pixels. */
    int count = 0;
    const CfrImage *imgs = cfr_get_images(vt, &count);
    ASSERT_NOT_NULL(imgs);
    ASSERT_EQ(count, 2);
    ASSERT_TRUE(imgs[0].id != imgs[1].id);
    ASSERT_EQ(imgs[0].rgba[0], 255); /* first image kept its pixels */
    ASSERT_EQ(imgs[0].rgba[1], 0);
    ASSERT_EQ(imgs[1].rgba[1], 255); /* second image has its own */

    /* Two placements, each referencing its own image. */
    int pc = 0;
    const CfrImagePlacement *pls = cfr_get_image_placements(vt, &pc);
    ASSERT_NOT_NULL(pls);
    ASSERT_EQ(pc, 2);
    ASSERT_EQ(pls[0].image_id, imgs[0].id);
    ASSERT_EQ(pls[1].image_id, imgs[1].id);

    cfr_free(vt);
}

/* --------------------------------------------------------------- */
/* 15b. Re-transmitting an explicit id deletes the old placements    */
/* --------------------------------------------------------------- */

/* Kitty spec: "When re-transmitting image data for a specific id, the
 * existing image and all its placements must be deleted. The new data
 * replaces the old image data but is not actually displayed until a
 * placement for it is created." */
static void test_retransmit_id_drops_placements(void)
{
    CfrTerm *vt = make_term(24, 80);

    uint8_t rgba[4] = { 255, 0, 0, 255 };
    char b64[64];
    b64_encode(rgba, sizeof(rgba), b64);
    char seq[128];
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=T,f=32,s=1,v=1,i=7;%s\x1b\\", b64);
    feed(vt, seq);

    /* Image + placement created. */
    int count = 0;
    cfr_get_images(vt, &count);
    ASSERT_EQ(count, 1);
    int pc = 0;
    cfr_get_image_placements(vt, &pc);
    ASSERT_EQ(pc, 1);

    /* Re-transmit the same id with a=t (no place): image replaced,
     * old placement dropped, no new placement created. */
    uint8_t rgba2[4] = { 0, 255, 0, 255 };
    b64_encode(rgba2, sizeof(rgba2), b64);
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=t,f=32,s=1,v=1,i=7;%s\x1b\\", b64);
    feed(vt, seq);

    const CfrImage *imgs = cfr_get_images(vt, &count);
    ASSERT_EQ(count, 1);
    ASSERT_EQ((long long)imgs[0].id, 7);
    ASSERT_EQ(imgs[0].rgba[1], 255); /* new pixels */
    pc = 0;
    cfr_get_image_placements(vt, &pc);
    ASSERT_EQ(pc, 0); /* old placement deleted */

    cfr_free(vt);
}

/* --------------------------------------------------------------- */
/* 15c. a=f animation frame keeps placements                        */
/* --------------------------------------------------------------- */

/* Unlike a=t/T re-transmit, an a=f frame only replaces pixels; the
 * existing placements must survive and now show the new frame. */
static void test_frame_keeps_placements(void)
{
    CfrTerm *vt = make_term(24, 80);

    uint8_t rgba[4] = { 255, 0, 0, 255 };
    char b64[64];
    b64_encode(rgba, sizeof(rgba), b64);
    char seq[128];
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=T,f=32,s=1,v=1,i=7;%s\x1b\\", b64);
    feed(vt, seq);

    uint8_t frame2[4] = { 0, 255, 0, 255 };
    b64_encode(frame2, sizeof(frame2), b64);
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=f,f=32,s=1,v=1,i=7;%s\x1b\\", b64);
    feed(vt, seq);

    int count = 0;
    const CfrImage *imgs = cfr_get_images(vt, &count);
    ASSERT_EQ(count, 1);
    ASSERT_EQ((long long)imgs[0].id, 7);
    ASSERT_EQ(imgs[0].rgba[1], 255); /* frame pixels replaced */

    int pc = 0;
    const CfrImagePlacement *pls = cfr_get_image_placements(vt, &pc);
    ASSERT_EQ(pc, 1); /* placement survived */
    ASSERT_EQ(pls[0].image_id, 7);

    cfr_free(vt);
}

/* --------------------------------------------------------------- */
/* 16. ED (clear screen) clears kitty images                       */
/* --------------------------------------------------------------- */

/* The kitty spec: "The clear screen escape code (usually ESC[2J)
 * should also clear all images." The image store is shared by sixel,
 * kitty and iTerm2, so the gate must be the store (vt->images), not
 * the lazily-created sixel decoder state (vt->sixel) — a session that
 * only ever used kitty graphics has vt->sixel == NULL, and chafa's
 * --clear (ESC[2J) silently left the previous image on screen. */
static void test_clear_on_ed_kitty_only(void)
{
    CfrTerm *vt = make_term(24, 80);

    uint8_t rgba[4] = { 255, 0, 0, 255 };
    char b64[64];
    b64_encode(rgba, sizeof(rgba), b64);
    char seq[128];
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=T,f=32,s=1,v=1;%s\x1b\\", b64);
    feed(vt, seq);

    int count = 0;
    cfr_get_images(vt, &count);
    ASSERT_EQ(count, 1);

    /* No sixel DCS was ever parsed, so vt->sixel is NULL — the clear
     * must still drop the kitty image and its placement. */
    feed(vt, "\x1b[2J");
    cfr_get_images(vt, &count);
    ASSERT_EQ(count, 0);
    int pc = 0;
    cfr_get_image_placements(vt, &pc);
    ASSERT_EQ(pc, 0);

    cfr_free(vt);
}

/* --------------------------------------------------------------- */
/* 16b. Scrolling culls kitty-only images into scrollback         */
/* --------------------------------------------------------------- */

/* The scroll-cull gate had the same vt->sixel bug: with only kitty
 * graphics in use, images never entered scrollback bookkeeping and
 * kept rendering at stale rows once they scrolled off. */
static void test_scroll_culls_kitty_only(void)
{
    CfrTerm *vt = make_term(4, 20);

    uint8_t rgba[4] = { 255, 0, 0, 255 };
    char b64[64];
    b64_encode(rgba, sizeof(rgba), b64);
    char seq[128];
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=T,f=32,s=1,v=1;%s\x1b\\", b64);
    feed(vt, seq);

    int n = -1;
    const CfrImage *imgs = cfr_get_images(vt, &n);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(imgs[0].row, 0);

    /* Scroll the image off the top of a 4-row screen. */
    feed(vt, "\x1b[4;1H");
    feed(vt, "\n\n\n\n\n");

    /* sixel_abs_top advanced by 5 even though no sixel was parsed;
     * the kitty image's row must track into scrollback. */
    imgs = cfr_get_images(vt, &n);
    ASSERT_EQ(n, 1);
    ASSERT_TRUE(imgs[0].row < 0);

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
    RUN_TEST(test_chunked_transmit_control_only_first);
    RUN_TEST(test_animate_put_frame);
    RUN_TEST(test_zlib_transmit);
    RUN_TEST(test_compose_accepted);
    RUN_TEST(test_transmit_and_place);
    RUN_TEST(test_transmit_and_place_cursor_advance);
    RUN_TEST(test_transmit_chunked_keeps_display_size);
    RUN_TEST(test_transmit_no_cursor_move);
    RUN_TEST(test_transmit_full_width_no_extra_scroll);
    RUN_TEST(test_transmit_no_id_creates_new_image);
    RUN_TEST(test_retransmit_id_drops_placements);
    RUN_TEST(test_frame_keeps_placements);
    RUN_TEST(test_clear_on_ed_kitty_only);
    RUN_TEST(test_scroll_culls_kitty_only);

    TEST_SUMMARY();
}
