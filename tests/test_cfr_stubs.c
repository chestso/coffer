/*
 * test_cfr_stubs — tests for unimplemented terminfo capability stubs.
 *
 * Covers:
 * - SGR 2 (dim) and SGR 8 (invisible): attr bits set/cleared
 * - DECSET ?5 (DECSCNM), ?1034 (meta), ?69 (margins): mode tracking
 * - CSI i (media copy), ESC l (meml), ESC m (memu): noop + log-once
 * - OSC 4 (initc), OSC 104 (oc): noop + log-once
 */

#include "test_helpers.h"

#include <coffer/coffer.h>
#include <string.h>

#include "coffer_internal.h"

static CfrTerm *make_term(void)
{
    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.rows = 24;
    cfg.cols = 80;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 6;
    CfrTerm *vt = cfr_new(&cfg);
    CfrCallbacks cb = { 0 };
    cfr_set_callbacks(vt, &cb, NULL);
    return vt;
}

static void feed(CfrTerm *vt, const char *s)
{
    cfr_input_write(vt, (const uint8_t *)s, strlen(s));
}

/* ---- Log capture ---- */

typedef struct
{
    int call_count;
    char last_msg[256];
} LogCapture;

static void cb_log(CfrLogLevel level, const char *msg, void *user)
{
    LogCapture *cap = (LogCapture *)user;
    (void)level;
    cap->call_count++;
    if (msg)
        strncpy(cap->last_msg, msg, sizeof(cap->last_msg) - 1);
}

static CfrTerm *make_term_with_log(LogCapture *cap)
{
    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.rows = 24;
    cfg.cols = 80;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 6;
    CfrTerm *vt = cfr_new(&cfg);
    memset(cap, 0, sizeof(*cap));
    CfrCallbacks cb = { 0 };
    cb.log = cb_log;
    cfr_set_callbacks(vt, &cb, cap);
    return vt;
}

/* ---- Mode capture ---- */

typedef struct
{
    int set_mode_count;
    CfrMode last_mode;
    bool last_on;
} ModeCapture;

static void cb_set_mode(CfrMode mode, bool on, void *user)
{
    ModeCapture *cap = (ModeCapture *)user;
    cap->set_mode_count++;
    cap->last_mode = mode;
    cap->last_on = on;
}

static CfrTerm *make_term_with_mode(ModeCapture *cap)
{
    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.rows = 24;
    cfg.cols = 80;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 6;
    CfrTerm *vt = cfr_new(&cfg);
    memset(cap, 0, sizeof(*cap));
    CfrCallbacks cb = { 0 };
    cb.set_mode = cb_set_mode;
    cfr_set_callbacks(vt, &cb, cap);
    return vt;
}

/* ---- SGR 2 (dim) ---- */

static void test_sgr_dim_set(void)
{
    CfrTerm *vt = make_term();
    feed(vt, "\x1b[2mX");
    const CfrCell *c = cfr_get_cell(vt, 0, 0);
    const CfrStyle *s = cfr_cell_style(vt, c);
    ASSERT_TRUE((s->attrs & CFR_ATTR_DIM) != 0);
    cfr_free(vt);
}

static void test_sgr_dim_cleared_by_22(void)
{
    CfrTerm *vt = make_term();
    feed(vt, "\x1b[2m\x1b[22mX");
    const CfrCell *c = cfr_get_cell(vt, 0, 0);
    const CfrStyle *s = cfr_cell_style(vt, c);
    ASSERT_TRUE((s->attrs & CFR_ATTR_DIM) == 0);
    cfr_free(vt);
}

/* ---- SGR 8 (invisible) ---- */

static void test_sgr_invis_set(void)
{
    CfrTerm *vt = make_term();
    feed(vt, "\x1b[8mX");
    const CfrCell *c = cfr_get_cell(vt, 0, 0);
    const CfrStyle *s = cfr_cell_style(vt, c);
    ASSERT_TRUE((s->attrs & CFR_ATTR_INVIS) != 0);
    cfr_free(vt);
}

static void test_sgr_invis_cleared_by_28(void)
{
    CfrTerm *vt = make_term();
    feed(vt, "\x1b[8m\x1b[28mX");
    const CfrCell *c = cfr_get_cell(vt, 0, 0);
    const CfrStyle *s = cfr_cell_style(vt, c);
    ASSERT_TRUE((s->attrs & CFR_ATTR_INVIS) == 0);
    cfr_free(vt);
}

/* ---- DECSET ?5 (DECSCNM) ---- */

static void test_decscnm_set(void)
{
    ModeCapture cap;
    CfrTerm *vt = make_term_with_mode(&cap);
    feed(vt, "\x1b[?5h");
    ASSERT_TRUE(cfr_get_mode(vt, CFR_MODE_REVERSE_VIDEO));
    ASSERT_TRUE(cap.set_mode_count > 0);
    ASSERT_EQ(cap.last_mode, CFR_MODE_REVERSE_VIDEO);
    ASSERT_TRUE(cap.last_on);
    cfr_free(vt);
}

static void test_decscnm_reset(void)
{
    ModeCapture cap;
    CfrTerm *vt = make_term_with_mode(&cap);
    feed(vt, "\x1b[?5h\x1b[?5l");
    ASSERT_FALSE(cfr_get_mode(vt, CFR_MODE_REVERSE_VIDEO));
    cfr_free(vt);
}

/* ---- DECSET ?1034 (meta) ---- */

static void test_meta_mode_set(void)
{
    ModeCapture cap;
    CfrTerm *vt = make_term_with_mode(&cap);
    feed(vt, "\x1b[?1034h");
    ASSERT_TRUE(cfr_get_mode(vt, CFR_MODE_META));
    cfr_free(vt);
}

static void test_meta_mode_reset(void)
{
    ModeCapture cap;
    CfrTerm *vt = make_term_with_mode(&cap);
    feed(vt, "\x1b[?1034h\x1b[?1034l");
    ASSERT_FALSE(cfr_get_mode(vt, CFR_MODE_META));
    cfr_free(vt);
}

/* ---- DECSET ?69 (left/right margins) ---- */

static void test_lr_margin_mode_set(void)
{
    ModeCapture cap;
    CfrTerm *vt = make_term_with_mode(&cap);
    feed(vt, "\x1b[?69h");
    ASSERT_TRUE(cfr_get_mode(vt, CFR_MODE_LEFT_RIGHT_MARGINS));
    cfr_free(vt);
}

static void test_lr_margin_mode_reset(void)
{
    ModeCapture cap;
    CfrTerm *vt = make_term_with_mode(&cap);
    feed(vt, "\x1b[?69h\x1b[?69l");
    ASSERT_FALSE(cfr_get_mode(vt, CFR_MODE_LEFT_RIGHT_MARGINS));
    cfr_free(vt);
}

/* ---- CSI i (media copy) — noop + log-once ---- */

static void test_media_copy_noop_log(void)
{
    LogCapture cap;
    CfrTerm *vt = make_term_with_log(&cap);
    feed(vt, "\x1b[4i");
    ASSERT_EQ(cap.call_count, 1);
    /* Second occurrence: rate-limited. */
    feed(vt, "\x1b[4i");
    ASSERT_EQ(cap.call_count, 1);
    cfr_free(vt);
}

/* No crash without log callback. */
static void test_media_copy_no_callback(void)
{
    CfrTerm *vt = make_term();
    feed(vt, "\x1b[4i");
    feed(vt, "\x1b[5i");
    cfr_free(vt);
}

/* ---- ESC l (meml) — noop + log-once ---- */

static void test_meml_noop_log(void)
{
    LogCapture cap;
    CfrTerm *vt = make_term_with_log(&cap);
    feed(vt, "X\x1b"
             "l");
    ASSERT_EQ(cap.call_count, 1);
    feed(vt, "\x1b"
             "l");
    ASSERT_EQ(cap.call_count, 1);
    cfr_free(vt);
}

/* ---- ESC m (memu) — noop + log-once ---- */

static void test_memu_noop_log(void)
{
    LogCapture cap;
    CfrTerm *vt = make_term_with_log(&cap);
    feed(vt, "X\x1b"
             "m");
    ASSERT_EQ(cap.call_count, 1);
    feed(vt, "\x1b"
             "m");
    ASSERT_EQ(cap.call_count, 1);
    cfr_free(vt);
}

/* ---- OSC 4 (initc) — noop + log-once ---- */

static void test_osc4_noop_log(void)
{
    LogCapture cap;
    CfrTerm *vt = make_term_with_log(&cap);
    feed(vt, "\x1b]4;0;rgb:ff/00/00\x1b\\");
    ASSERT_EQ(cap.call_count, 1);
    /* Second palette set: rate-limited. */
    feed(vt, "\x1b]4;1;rgb:00/ff/00\x1b\\");
    ASSERT_EQ(cap.call_count, 1);
    cfr_free(vt);
}

/* ---- OSC 104 (oc) — noop + log-once ---- */

static void test_osc104_noop_log(void)
{
    LogCapture cap;
    CfrTerm *vt = make_term_with_log(&cap);
    feed(vt, "\x1b]104\x1b\\");
    ASSERT_EQ(cap.call_count, 1);
    feed(vt, "\x1b]104\x07");
    ASSERT_EQ(cap.call_count, 1);
    cfr_free(vt);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    RUN_TEST(test_sgr_dim_set);
    RUN_TEST(test_sgr_dim_cleared_by_22);
    RUN_TEST(test_sgr_invis_set);
    RUN_TEST(test_sgr_invis_cleared_by_28);
    RUN_TEST(test_decscnm_set);
    RUN_TEST(test_decscnm_reset);
    RUN_TEST(test_meta_mode_set);
    RUN_TEST(test_meta_mode_reset);
    RUN_TEST(test_lr_margin_mode_set);
    RUN_TEST(test_lr_margin_mode_reset);
    RUN_TEST(test_media_copy_noop_log);
    RUN_TEST(test_media_copy_no_callback);
    RUN_TEST(test_meml_noop_log);
    RUN_TEST(test_memu_noop_log);
    RUN_TEST(test_osc4_noop_log);
    RUN_TEST(test_osc104_noop_log);

    TEST_SUMMARY();
}
