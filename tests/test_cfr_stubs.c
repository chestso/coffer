/*
 * test_cfr_stubs — tests for unimplemented terminfo capability stubs.
 *
 * Covers:
 * - SGR 2 (dim) and SGR 8 (invisible): attr bits set/cleared
 * - DECSET ?5 (DECSCNM), ?1034 (meta), ?69 (margins): mode tracking
 * - DECSLRM vs. `CSI s` save cursor: DECSLRM is not a save-cursor alias
 * - CSI i (media copy), ESC l (meml), ESC m (memu): noop + log-once
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
    const CfrStyle *s = cfr_cell_style(vt, 0, 0);
    ASSERT_TRUE((s->attrs & CFR_ATTR_DIM) != 0);
    cfr_free(vt);
}

static void test_sgr_dim_cleared_by_22(void)
{
    CfrTerm *vt = make_term();
    feed(vt, "\x1b[2m\x1b[22mX");
    const CfrStyle *s = cfr_cell_style(vt, 0, 0);
    ASSERT_TRUE((s->attrs & CFR_ATTR_DIM) == 0);
    cfr_free(vt);
}

/* ---- SGR 8 (invisible) ---- */

static void test_sgr_invis_set(void)
{
    CfrTerm *vt = make_term();
    feed(vt, "\x1b[8mX");
    const CfrStyle *s = cfr_cell_style(vt, 0, 0);
    ASSERT_TRUE((s->attrs & CFR_ATTR_INVIS) != 0);
    cfr_free(vt);
}

static void test_sgr_invis_cleared_by_28(void)
{
    CfrTerm *vt = make_term();
    feed(vt, "\x1b[8m\x1b[28mX");
    const CfrStyle *s = cfr_cell_style(vt, 0, 0);
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

/* ---- DECSLRM (CSI Pl ; Pr s) is not a save-cursor alias ---- */

/* DECSLRM shares its final byte with the ANSI.SYS save-cursor form. A
 * parameterized `CSI ... s` is always DECSLRM: margins are not
 * implemented, so it must do nothing — in particular it must not
 * overwrite the saved cursor that a later `CSI u` restores. */
static void test_declrmm_parameterized_not_save(void)
{
    CfrTerm *vt = make_term();
    feed(vt, "\x1b[3;3H");  /* cursor (2,2) */
    feed(vt, "\x1b[1;10s"); /* DECSLRM, ignored */
    feed(vt, "\x1b[5;5H");  /* cursor (4,4) */
    feed(vt, "\x1b[0s");    /* also parameterized: not a save */
    feed(vt, "\x1b[u");     /* restores the initial (0,0) */
    CfrCursor cur = cfr_get_cursor(vt);
    ASSERT_EQ(cur.row, 0);
    ASSERT_EQ(cur.col, 0);
    cfr_free(vt);
}

/* The bare form still saves while DECLRMM is off. */
static void test_save_cursor_bare_still_saves(void)
{
    CfrTerm *vt = make_term();
    feed(vt, "\x1b[3;4H\x1b[s");
    feed(vt, "\x1b[1;1H\x1b[u");
    CfrCursor cur = cfr_get_cursor(vt);
    ASSERT_EQ(cur.row, 2);
    ASSERT_EQ(cur.col, 3);
    cfr_free(vt);
}

/* With DECLRMM set, `CSI s` is DECSLRM with default margins; it is not
 * a save-cursor either. */
static void test_declrmm_on_bare_s_not_save(void)
{
    CfrTerm *vt = make_term();
    feed(vt, "\x1b[?69h");
    feed(vt, "\x1b[3;4H\x1b[s");
    feed(vt, "\x1b[1;1H\x1b[u");
    CfrCursor cur = cfr_get_cursor(vt);
    ASSERT_EQ(cur.row, 0);
    ASSERT_EQ(cur.col, 0);
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

/* ---- OSC 4 (initc) and OSC 104 (oc) are covered by test_cfr_palette ---- */

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
    RUN_TEST(test_declrmm_parameterized_not_save);
    RUN_TEST(test_save_cursor_bare_still_saves);
    RUN_TEST(test_declrmm_on_bare_s_not_save);
    RUN_TEST(test_media_copy_noop_log);
    RUN_TEST(test_media_copy_no_callback);
    RUN_TEST(test_meml_noop_log);
    RUN_TEST(test_memu_noop_log);

    TEST_SUMMARY();
}
