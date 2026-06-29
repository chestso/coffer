/* test_cfr_lottie.c — unit tests for the Lottie animation subsystem.
 *
 * Tests OSC 837 dispatch, load/place/delete commands, playback state,
 * frame advancement (cfr_lottie_tick), query API (cfr_get_lotties),
 * scroll/clear culling, and chunked upload. */

#include "coffer_internal.h"
#include "test_helpers.h"
#include <coffer/coffer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Test infrastructure                                                */
/* ------------------------------------------------------------------ */

static char g_out[1024];
static size_t g_out_len;

static void on_output(const uint8_t *bytes, size_t len, void *u)
{
    (void)u;
    if (g_out_len + len >= sizeof(g_out))
        return;
    memcpy(g_out + g_out_len, bytes, len);
    g_out_len += len;
    g_out[g_out_len] = '\0';
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
    g_out_len = 0;
    g_out[0] = '\0';
    return vt;
}

static void feed(CfrTerm *vt, const char *s)
{
    cfr_input_write(vt, (const uint8_t *)s, strlen(s));
}

/* Helper: base64-encode a string. Returns malloc'd buffer. */
static char *b64_enc(const char *src)
{
    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t len = strlen(src);
    size_t out_len = 4 * ((len + 2) / 3);
    char *out = malloc(out_len + 1);
    size_t i, j;
    for (i = 0, j = 0; i < len; i += 3) {
        uint32_t a = (uint8_t)src[i];
        uint32_t b = (i + 1 < len) ? (uint8_t)src[i + 1] : 0;
        uint32_t c = (i + 2 < len) ? (uint8_t)src[i + 2] : 0;
        uint32_t t = (a << 16) | (b << 8) | c;
        out[j++] = b64[(t >> 18) & 0x3F];
        out[j++] = b64[(t >> 12) & 0x3F];
        out[j++] = (i + 1 < len) ? b64[(t >> 6) & 0x3F] : '=';
        out[j++] = (i + 2 < len) ? b64[t & 0x3F] : '=';
    }
    out[j] = '\0';
    return out;
}

/* Feed an APC with a JSON payload (auto-base64-encoded). */
static void feed_lottie(CfrTerm *vt, const char *json)
{
    char *b64 = b64_enc(json);
    char seq[4096];
    snprintf(seq, sizeof(seq), "\033_%s\033\\", b64);
    feed(vt, seq);
    free(b64);
}

/* Feed a load-chunk APC. The data parameter is a raw string that will
 * be base64-encoded into the "data" field. The rest of the JSON is built
 * automatically. */
static void feed_chunk(CfrTerm *vt, uint64_t id, int seq, int total,
                       const char *data)
{
    char *data_b64 = b64_enc(data);
    char json[8192];
    snprintf(json, sizeof(json),
             "{\"cmd\":\"load-chunk\",\"id\":%llu,\"seq\":%d,\"total\":%d,"
             "\"data\":\"%s\"}",
             (unsigned long long)id, seq, total, data_b64);
    free(data_b64);
    feed_lottie(vt, json);
}

/* Get placements for a specific animation. */
static const CfrLottiePlacement *get_placements(CfrTerm *vt, const CfrLottie *l)
{
    int pl_count = 0;
    return cfr_get_lottie_placements(vt, l->id, &pl_count);
}

/* ------------------------------------------------------------------ */
/* Tests: Load and query                                              */
/* ------------------------------------------------------------------ */

static void test_load_basic(void)
{
    CfrTerm *vt = make_term(24, 80);

    feed_lottie(vt,
                "{\"cmd\":\"load\",\"id\":1,"
                "\"lottie\":{\"v\":\"5.6.0\",\"fr\":30,\"ip\":0,\"op\":60,"
                "\"w\":40,\"h\":24,\"layers\":[]},"
                "\"placement\":{\"row\":5,\"col\":10,\"rows\":4,\"cols\":4},"
                "\"layer\":\"foreground\"}");

    int count = 0;
    const CfrLottie *lotties = cfr_get_lotties(vt, &count);
    ASSERT_EQ(count, 1);
    ASSERT_NOT_NULL(lotties);

    const CfrLottie *l = &lotties[0];
    ASSERT_EQ((long long)l->id, 1);
    ASSERT_TRUE(l->version > 0);
    ASSERT_EQ(l->canvas_w, 40);
    ASSERT_EQ(l->canvas_h, 24);
    ASSERT_EQ(l->current_frame, 0);
    ASSERT_EQ(l->frame_count, 60);
    ASSERT_TRUE(l->playing);
    ASSERT_EQ(l->placement_count, 1);

    const CfrLottiePlacement *pl = get_placements(vt, l);
    ASSERT_EQ(pl[0].col, 10);
    ASSERT_EQ(pl[0].rows, 4);
    ASSERT_EQ(pl[0].cols, 4);
    ASSERT_EQ(pl[0].layer, 0);

    cfr_free(vt);
}

static void test_load_background(void)
{
    CfrTerm *vt = make_term(24, 80);

    feed_lottie(vt,
                "{\"cmd\":\"load\",\"id\":2,"
                "\"lottie\":{\"v\":\"5.6.0\",\"fr\":30,\"ip\":0,\"op\":30,"
                "\"w\":80,\"h\":24,\"layers\":[]},"
                "\"layer\":\"background\",\"opacity\":0.5}");

    int count = 0;
    const CfrLottie *lotties = cfr_get_lotties(vt, &count);
    ASSERT_EQ(count, 1);

    const CfrLottiePlacement *pl = get_placements(vt, &lotties[0]);
    ASSERT_EQ(pl[0].layer, 1);
    ASSERT_TRUE(pl[0].opacity_x256 > 100 && pl[0].opacity_x256 < 150);

    cfr_free(vt);
}

static void test_load_multiple(void)
{
    CfrTerm *vt = make_term(24, 80);

    feed_lottie(vt,
                "{\"cmd\":\"load\",\"id\":1,"
                "\"lottie\":{\"v\":\"5.6.0\",\"fr\":30,\"ip\":0,\"op\":30,"
                "\"w\":20,\"h\":20,\"layers\":[]}}");
    feed_lottie(vt,
                "{\"cmd\":\"load\",\"id\":2,"
                "\"lottie\":{\"v\":\"5.6.0\",\"fr\":60,\"ip\":0,\"op\":120,"
                "\"w\":40,\"h\":40,\"layers\":[]}}");

    int count = 0;
    const CfrLottie *lotties = cfr_get_lotties(vt, &count);
    ASSERT_EQ(count, 2);

    /* Find by id — order is not guaranteed */
    const CfrLottie *l1 = NULL, *l2 = NULL;
    for (int i = 0; i < count; i++) {
        if (lotties[i].id == 1)
            l1 = &lotties[i];
        if (lotties[i].id == 2)
            l2 = &lotties[i];
    }
    ASSERT_NOT_NULL(l1);
    ASSERT_NOT_NULL(l2);
    ASSERT_EQ(l1->frame_count, 30);
    ASSERT_EQ(l2->frame_count, 120);
    ASSERT_EQ(l1->canvas_w, 20);
    ASSERT_EQ(l1->canvas_h, 24);
    ASSERT_EQ(l2->canvas_w, 40);
    ASSERT_EQ(l2->canvas_h, 42);

    cfr_free(vt);
}

static void test_load_replace(void)
{
    CfrTerm *vt = make_term(24, 80);

    feed_lottie(vt,
                "{\"cmd\":\"load\",\"id\":1,"
                "\"lottie\":{\"v\":\"5.6.0\",\"fr\":30,\"ip\":0,\"op\":30,"
                "\"w\":20,\"h\":20,\"layers\":[]}}");

    int count = 0;
    const CfrLottie *lotties = cfr_get_lotties(vt, &count);
    ASSERT_EQ(count, 1);
    uint32_t v1 = lotties[0].version;

    /* Replace with same id */
    feed_lottie(vt,
                "{\"cmd\":\"load\",\"id\":1,"
                "\"lottie\":{\"v\":\"5.6.0\",\"fr\":60,\"ip\":0,\"op\":60,"
                "\"w\":40,\"h\":40,\"layers\":[]}}");

    lotties = cfr_get_lotties(vt, &count);
    ASSERT_EQ(count, 1);
    ASSERT_TRUE(lotties[0].version > v1);
    ASSERT_EQ(lotties[0].frame_count, 60);
    ASSERT_EQ(lotties[0].canvas_w, 40);
    ASSERT_EQ(lotties[0].canvas_h, 42);

    cfr_free(vt);
}

/* ------------------------------------------------------------------ */
/* Tests: Place command                                               */
/* ------------------------------------------------------------------ */

static void test_place_multiple(void)
{
    CfrTerm *vt = make_term(24, 80);

    feed_lottie(vt,
                "{\"cmd\":\"load\",\"id\":1,"
                "\"lottie\":{\"v\":\"5.6.0\",\"fr\":30,\"ip\":0,\"op\":30,"
                "\"w\":20,\"h\":20,\"layers\":[]}}");

    feed_lottie(vt,
                "{\"cmd\":\"place\",\"id\":1,"
                "\"placement\":{\"row\":0,\"col\":0,\"rows\":2,\"cols\":2},"
                "\"layer\":\"foreground\"}");

    feed_lottie(vt,
                "{\"cmd\":\"place\",\"id\":1,"
                "\"placement\":{\"row\":0,\"col\":78,\"rows\":2,\"cols\":2},"
                "\"layer\":\"background\",\"opacity\":0.8}");

    int count = 0;
    const CfrLottie *lotties = cfr_get_lotties(vt, &count);
    ASSERT_EQ(count, 1);
    ASSERT_EQ(lotties[0].placement_count, 2); /* load+place at (0,0) dedup'd, +1 at (0,78) */

    const CfrLottiePlacement *pl = get_placements(vt, &lotties[0]);
    ASSERT_EQ(pl[0].col, 0);
    ASSERT_EQ(pl[0].layer, 0);
    ASSERT_EQ(pl[0].rows, 2);
    ASSERT_EQ(pl[0].cols, 2);
    ASSERT_EQ(pl[1].col, 78);
    ASSERT_EQ(pl[1].layer, 1);

    cfr_free(vt);
}

/* ------------------------------------------------------------------ */
/* Tests: Delete command                                              */
/* ------------------------------------------------------------------ */

static void test_delete(void)
{
    CfrTerm *vt = make_term(24, 80);

    feed_lottie(vt,
                "{\"cmd\":\"load\",\"id\":1,"
                "\"lottie\":{\"v\":\"5.6.0\",\"fr\":30,\"ip\":0,\"op\":30,"
                "\"w\":20,\"h\":20,\"layers\":[]}}");
    feed_lottie(vt,
                "{\"cmd\":\"load\",\"id\":2,"
                "\"lottie\":{\"v\":\"5.6.0\",\"fr\":30,\"ip\":0,\"op\":30,"
                "\"w\":20,\"h\":20,\"layers\":[]}}");

    int count = 0;
    cfr_get_lotties(vt, &count);
    ASSERT_EQ(count, 2);

    feed_lottie(vt, "{\"cmd\":\"delete\",\"id\":1}");

    const CfrLottie *lotties = cfr_get_lotties(vt, &count);
    ASSERT_EQ(count, 1);
    ASSERT_EQ((long long)lotties[0].id, 2);

    cfr_free(vt);
}

/* ------------------------------------------------------------------ */
/* Tests: Playback control                                            */
/* ------------------------------------------------------------------ */

static void test_play_pause_stop(void)
{
    CfrTerm *vt = make_term(24, 80);

    /* Load with autostart=false */
    feed_lottie(vt,
                "{\"cmd\":\"load\",\"id\":1,"
                "\"lottie\":{\"v\":\"5.6.0\",\"fr\":30,\"ip\":0,\"op\":60,"
                "\"w\":40,\"h\":24,\"layers\":[]},"
                "\"play\":{\"autostart\":false}}");

    int count = 0;
    const CfrLottie *lotties = cfr_get_lotties(vt, &count);
    ASSERT_EQ(count, 1);
    ASSERT_FALSE(lotties[0].playing);

    /* Play */
    feed_lottie(vt, "{\"cmd\":\"play\",\"id\":1}");
    lotties = cfr_get_lotties(vt, &count);
    ASSERT_TRUE(lotties[0].playing);

    /* Pause */
    feed_lottie(vt, "{\"cmd\":\"pause\",\"id\":1}");
    lotties = cfr_get_lotties(vt, &count);
    ASSERT_FALSE(lotties[0].playing);

    /* Play again, then stop (should reset to frame 0) */
    feed_lottie(vt, "{\"cmd\":\"play\",\"id\":1}");
    cfr_lottie_tick(vt, 1000000); /* advance 1 second */
    cfr_lottie_tick(vt, 2000000); /* advance more */

    feed_lottie(vt, "{\"cmd\":\"stop\",\"id\":1}");
    lotties = cfr_get_lotties(vt, &count);
    ASSERT_FALSE(lotties[0].playing);
    ASSERT_EQ(lotties[0].current_frame, 0);

    cfr_free(vt);
}

static void test_seek(void)
{
    CfrTerm *vt = make_term(24, 80);

    feed_lottie(vt,
                "{\"cmd\":\"load\",\"id\":1,"
                "\"lottie\":{\"v\":\"5.6.0\",\"fr\":30,\"ip\":0,\"op\":60,"
                "\"w\":40,\"h\":24,\"layers\":[]}}");

    feed_lottie(vt, "{\"cmd\":\"seek\",\"id\":1,\"frame\":15}");

    int count = 0;
    const CfrLottie *lotties = cfr_get_lotties(vt, &count);
    ASSERT_EQ(lotties[0].current_frame, 15);

    /* Seek out of range — clamp */
    feed_lottie(vt, "{\"cmd\":\"seek\",\"id\":1,\"frame\":999}");
    lotties = cfr_get_lotties(vt, &count);
    ASSERT_EQ(lotties[0].current_frame, 59); /* op-1 */

    cfr_free(vt);
}

/* ------------------------------------------------------------------ */
/* Tests: Frame advancement (cfr_lottie_tick)                         */
/* ------------------------------------------------------------------ */

static void test_tick_advances_frame(void)
{
    CfrTerm *vt = make_term(24, 80);

    feed_lottie(vt,
                "{\"cmd\":\"load\",\"id\":1,"
                "\"lottie\":{\"v\":\"5.6.0\",\"fr\":30,\"ip\":0,\"op\":60,"
                "\"w\":40,\"h\":24,\"layers\":[]}}");

    /* First tick sets baseline */
    bool advanced = cfr_lottie_tick(vt, 1000000); /* t = 1s */
    ASSERT_FALSE(advanced);                       /* no delta yet */

    /* 1 second later = 30 frames at 30fps */
    advanced = cfr_lottie_tick(vt, 2000000);
    ASSERT_TRUE(advanced);

    int count = 0;
    const CfrLottie *lotties = cfr_get_lotties(vt, &count);
    ASSERT_TRUE(lotties[0].current_frame >= 29);
    ASSERT_TRUE(lotties[0].current_frame <= 31); /* tolerance for rounding */

    cfr_free(vt);
}

static void test_tick_loops(void)
{
    CfrTerm *vt = make_term(24, 80);

    feed_lottie(vt,
                "{\"cmd\":\"load\",\"id\":1,"
                "\"lottie\":{\"v\":\"5.6.0\",\"fr\":10,\"ip\":0,\"op\":10,"
                "\"w\":40,\"h\":24,\"layers\":[]},"
                "\"play\":{\"loop\":true}}");

    cfr_lottie_tick(vt, 1000000);
    /* 1.5 seconds = 15 frames at 10fps. op=10, so should loop.
     * 15 frames past 0: 15 % 10 = 5 → frame 5 */
    bool advanced = cfr_lottie_tick(vt, 2500000);
    ASSERT_TRUE(advanced);

    int count = 0;
    const CfrLottie *lotties = cfr_get_lotties(vt, &count);
    ASSERT_TRUE(lotties[0].playing);
    ASSERT_TRUE(lotties[0].current_frame >= 4);
    ASSERT_TRUE(lotties[0].current_frame <= 6);

    cfr_free(vt);
}

static void test_tick_no_loop_stops(void)
{
    CfrTerm *vt = make_term(24, 80);

    feed_lottie(vt,
                "{\"cmd\":\"load\",\"id\":1,"
                "\"lottie\":{\"v\":\"5.6.0\",\"fr\":10,\"ip\":0,\"op\":10,"
                "\"w\":40,\"h\":24,\"layers\":[]},"
                "\"play\":{\"loop\":false}}");

    cfr_lottie_tick(vt, 1000000);
    /* 10 seconds = 100 frames at 10fps. Exceeds op=10, no loop → stops. */
    cfr_lottie_tick(vt, 11000000);

    int count = 0;
    const CfrLottie *lotties = cfr_get_lotties(vt, &count);
    ASSERT_FALSE(lotties[0].playing);
    ASSERT_EQ(lotties[0].current_frame, 9); /* op - 1 */

    cfr_free(vt);
}

static void test_tick_paused_no_advance(void)
{
    CfrTerm *vt = make_term(24, 80);

    feed_lottie(vt,
                "{\"cmd\":\"load\",\"id\":1,"
                "\"lottie\":{\"v\":\"5.6.0\",\"fr\":30,\"ip\":0,\"op\":60,"
                "\"w\":40,\"h\":24,\"layers\":[]}}");

    feed_lottie(vt, "{\"cmd\":\"pause\",\"id\":1}");

    cfr_lottie_tick(vt, 1000000);
    bool advanced = cfr_lottie_tick(vt, 2000000);
    ASSERT_FALSE(advanced);

    int count = 0;
    const CfrLottie *lotties = cfr_get_lotties(vt, &count);
    ASSERT_EQ(lotties[0].current_frame, 0);

    cfr_free(vt);
}

/* ------------------------------------------------------------------ */
/* Tests: Scroll/clear culling                                        */
/* ------------------------------------------------------------------ */

static void test_scroll_cull(void)
{
    CfrTerm *vt = make_term(24, 80);
    cfr_set_scrollback_size(vt, 5);

    /* Place animation at the last row */
    feed_lottie(vt,
                "{\"cmd\":\"load\",\"id\":1,"
                "\"lottie\":{\"v\":\"5.6.0\",\"fr\":30,\"ip\":0,\"op\":30,"
                "\"w\":20,\"h\":20,\"layers\":[]},"
                "\"placement\":{\"row\":23,\"col\":0,\"rows\":1,\"cols\":1}}");

    int count = 0;
    cfr_get_lotties(vt, &count);
    ASSERT_EQ(count, 1);

    /* Move cursor to bottom of screen so subsequent linefeeds scroll */
    feed(vt, "\033[24;1H");
    /* Scroll 30 lines by sending 30 linefeeds */
    for (int i = 0; i < 30; i++)
        feed(vt, "\n");

    const CfrLottie *lotties = cfr_get_lotties(vt, &count);
    ASSERT_EQ(count, 0);

    cfr_free(vt);
}

static void test_clear_display_rows_foreground(void)
{
    CfrTerm *vt = make_term(24, 80);

    feed_lottie(vt,
                "{\"cmd\":\"load\",\"id\":1,"
                "\"lottie\":{\"v\":\"5.6.0\",\"fr\":30,\"ip\":0,\"op\":30,"
                "\"w\":20,\"h\":20,\"layers\":[]},"
                "\"placement\":{\"row\":5,\"col\":0,\"rows\":2,\"cols\":2},"
                "\"layer\":\"foreground\"}");

    int count = 0;
    cfr_get_lotties(vt, &count);
    ASSERT_EQ(count, 1);

    /* ED 2 — clear entire screen should remove foreground placements */
    feed(vt, "\033[2J");

    const CfrLottie *lotties = cfr_get_lotties(vt, &count);
    ASSERT_EQ(count, 0); /* foreground placement removed, no placements
                          * left → record removed */

    cfr_free(vt);
}

static void test_clear_preserves_background(void)
{
    CfrTerm *vt = make_term(24, 80);

    feed_lottie(vt,
                "{\"cmd\":\"load\",\"id\":1,"
                "\"lottie\":{\"v\":\"5.6.0\",\"fr\":30,\"ip\":0,\"op\":30,"
                "\"w\":80,\"h\":144,\"layers\":[]},"
                "\"placement\":{\"row\":0,\"col\":0,\"rows\":24,\"cols\":80},"
                "\"layer\":\"background\"}");

    /* ED 2 — clear entire screen */
    feed(vt, "\033[2J");

    int count = 0;
    const CfrLottie *lotties = cfr_get_lotties(vt, &count);
    ASSERT_EQ(count, 1); /* background placement survives */

    cfr_free(vt);
}

/* ------------------------------------------------------------------ */
/* Tests: No lottie state — all no-ops                                */
/* ------------------------------------------------------------------ */

static void test_no_lottie_noop(void)
{
    CfrTerm *vt = make_term(24, 80);

    /* These should all be safe no-ops when no animation has been loaded */
    cfr_lottie_tick(vt, 1000000);
    cfr_lottie_note_scroll(vt, 10);
    cfr_lottie_clear_display_rows(vt, 0, 23);
    cfr_lottie_clear_all(vt);

    int count = -1;
    const CfrLottie *lotties = cfr_get_lotties(vt, &count);
    ASSERT_EQ(count, 0);
    ASSERT_NULL(lotties);

    cfr_free(vt);
}

/* ------------------------------------------------------------------ */
/* Tests: Version bump on mutations                                   */
/* ------------------------------------------------------------------ */

static void test_version_bumps_on_state_change(void)
{
    CfrTerm *vt = make_term(24, 80);

    feed_lottie(vt,
                "{\"cmd\":\"load\",\"id\":1,"
                "\"lottie\":{\"v\":\"5.6.0\",\"fr\":30,\"ip\":0,\"op\":60,"
                "\"w\":40,\"h\":24,\"layers\":[]}}");

    int count = 0;
    const CfrLottie *lotties = cfr_get_lotties(vt, &count);
    uint32_t v0 = lotties[0].version;

    feed_lottie(vt, "{\"cmd\":\"pause\",\"id\":1}");
    lotties = cfr_get_lotties(vt, &count);
    ASSERT_TRUE(lotties[0].version > v0);

    uint32_t v1 = lotties[0].version;
    feed_lottie(vt, "{\"cmd\":\"play\",\"id\":1}");
    lotties = cfr_get_lotties(vt, &count);
    ASSERT_TRUE(lotties[0].version > v1);

    cfr_free(vt);
}

/* ------------------------------------------------------------------ */
/* Tests: Speed multiplier                                            */
/* ------------------------------------------------------------------ */

static void test_speed_multiplier(void)
{
    CfrTerm *vt = make_term(24, 80);

    feed_lottie(vt,
                "{\"cmd\":\"load\",\"id\":1,"
                "\"lottie\":{\"v\":\"5.6.0\",\"fr\":30,\"ip\":0,\"op\":120,"
                "\"w\":40,\"h\":24,\"layers\":[]},"
                "\"play\":{\"speed\":2.0}}");

    cfr_lottie_tick(vt, 1000000);
    /* 0.5s later at 2x speed = 30 frames */
    bool advanced = cfr_lottie_tick(vt, 1500000);
    ASSERT_TRUE(advanced);

    int count = 0;
    const CfrLottie *lotties = cfr_get_lotties(vt, &count);
    ASSERT_TRUE(lotties[0].current_frame >= 28);
    ASSERT_TRUE(lotties[0].current_frame <= 32);

    cfr_free(vt);
}

/* ------------------------------------------------------------------ */
/* Tests: Resize clears all                                           */
/* ------------------------------------------------------------------ */

static void test_resize_clears_all(void)
{
    CfrTerm *vt = make_term(24, 80);

    feed_lottie(vt,
                "{\"cmd\":\"load\",\"id\":1,"
                "\"lottie\":{\"v\":\"5.6.0\",\"fr\":30,\"ip\":0,\"op\":30,"
                "\"w\":20,\"h\":20,\"layers\":[]}}");

    int count = 0;
    cfr_get_lotties(vt, &count);
    ASSERT_EQ(count, 1);

    cfr_resize(vt, 40, 120);

    const CfrLottie *lotties = cfr_get_lotties(vt, &count);
    ASSERT_EQ(count, 0);

    cfr_free(vt);
}

/* ------------------------------------------------------------------ */
/* Tests: Chunked upload                                              */
/* ------------------------------------------------------------------ */

static void test_load_chunk(void)
{
    CfrTerm *vt = make_term(24, 80);

    /* Split a Lottie JSON into 3 chunks */
    const char *part0 = "{\"v\":\"5.6.0\",\"fr\":30,";
    const char *part1 = "\"ip\":0,\"op\":30,";
    const char *part2 = "\"w\":20,\"h\":20,\"layers\":[]}";

    feed_chunk(vt, 1, 0, 3, part0);
    feed_chunk(vt, 1, 1, 3, part1);
    feed_chunk(vt, 1, 2, 3, part2);

    int count = 0;
    const CfrLottie *lotties = cfr_get_lotties(vt, &count);
    ASSERT_EQ(count, 1);
    ASSERT_EQ(lotties[0].id, 1);
    ASSERT_EQ(lotties[0].canvas_w, 20);
    ASSERT_EQ(lotties[0].canvas_h, 24);
    ASSERT_EQ(lotties[0].frame_count, 30);
    ASSERT_EQ(lotties[0].playing, true);

    cfr_free(vt);
}

static void test_load_chunk_restart(void)
{
    CfrTerm *vt = make_term(24, 80);

    /* Start a chunked upload for id 1, then restart it */
    feed_chunk(vt, 1, 0, 3, "{\"v\":\"5.6");
    feed_chunk(vt, 1, 1, 3, "old data");
    /* Restart with seq=0 — should discard previous */
    feed_chunk(vt, 1, 0, 2, "{\"v\":\"5.6.0\",\"fr\":30,");
    feed_chunk(vt, 1, 1, 2, "\"ip\":0,\"op\":30,\"w\":20,\"h\":20,\"layers\":[]}");

    int count = 0;
    const CfrLottie *lotties = cfr_get_lotties(vt, &count);
    ASSERT_EQ(count, 1);
    ASSERT_EQ(lotties[0].id, 1);
    ASSERT_EQ(lotties[0].canvas_w, 20);
    ASSERT_EQ(lotties[0].canvas_h, 24);

    cfr_free(vt);
}

static void test_place_opacity_update(void)
{
    CfrTerm *vt = make_term(24, 80);

    feed_lottie(vt,
                "{\"cmd\":\"load\",\"id\":1,"
                "\"lottie\":{\"v\":\"5.6.0\",\"fr\":30,\"ip\":0,\"op\":30,"
                "\"w\":20,\"h\":20,\"layers\":[]},"
                "\"layer\":\"foreground\",\"opacity\":0.5}");

    int count = 0;
    const CfrLottie *lotties = cfr_get_lotties(vt, &count);
    ASSERT_EQ(count, 1);

    const CfrLottiePlacement *pl = get_placements(vt, &lotties[0]);
    ASSERT_EQ(pl[0].opacity_x256, 128);

    /* Re-place at same position with new opacity — should update, not append */
    feed_lottie(vt,
                "{\"cmd\":\"place\",\"id\":1,"
                "\"placement\":{\"row\":0,\"col\":0,\"rows\":1,\"cols\":2},"
                "\"layer\":\"foreground\",\"opacity\":0.3}");

    lotties = cfr_get_lotties(vt, &count);
    ASSERT_EQ(count, 1);
    ASSERT_EQ(lotties[0].placement_count, 1);

    pl = get_placements(vt, &lotties[0]);
    ASSERT_EQ(pl[0].opacity_x256, 77);
    ASSERT_EQ(pl[0].rows, 1);
    ASSERT_EQ(pl[0].cols, 2);

    cfr_free(vt);
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);
    printf("test_cfr_lottie\n");

    RUN_TEST(test_no_lottie_noop);
    RUN_TEST(test_load_basic);
    RUN_TEST(test_load_background);
    RUN_TEST(test_load_multiple);
    RUN_TEST(test_load_replace);
    RUN_TEST(test_place_multiple);
    RUN_TEST(test_delete);
    RUN_TEST(test_play_pause_stop);
    RUN_TEST(test_seek);
    RUN_TEST(test_tick_advances_frame);
    RUN_TEST(test_tick_loops);
    RUN_TEST(test_tick_no_loop_stops);
    RUN_TEST(test_tick_paused_no_advance);
    RUN_TEST(test_scroll_cull);
    RUN_TEST(test_clear_display_rows_foreground);
    RUN_TEST(test_clear_preserves_background);
    RUN_TEST(test_version_bumps_on_state_change);
    RUN_TEST(test_speed_multiplier);
    RUN_TEST(test_resize_clears_all);
    RUN_TEST(test_load_chunk);
    RUN_TEST(test_load_chunk_restart);
    RUN_TEST(test_place_opacity_update);

    TEST_SUMMARY();
}
