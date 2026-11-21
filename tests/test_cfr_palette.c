/*
 * test_cfr_palette — xterm OSC 4 / OSC 104 indexed palette overrides.
 *
 * Covers set/query/reset, all XParseColor spec forms, index validation,
 * log-once for bad specs, damage on change (and no damage on a no-op),
 * RIS reset, altscreen survival, and the key contract: indexed styles
 * resolve through the live palette and keep their intern identity when
 * the palette changes.
 */

#include "test_helpers.h"

#include <coffer/coffer.h>
#include <string.h>

typedef struct
{
    char out[512];
    size_t out_len;
    int out_calls;

    int damage_calls;
    CfrRect last_damage;

    int log_calls;
    char last_log[256];
} Capture;

static void cb_output(const uint8_t *bytes, size_t len, void *user)
{
    Capture *cap = user;
    cap->out_calls++;
    size_t room = sizeof(cap->out) - cap->out_len - 1;
    if (len > room)
        len = room;
    memcpy(cap->out + cap->out_len, bytes, len);
    cap->out_len += len;
    cap->out[cap->out_len] = '\0';
}

static void cb_damage(CfrRect rect, void *user)
{
    Capture *cap = user;
    cap->damage_calls++;
    cap->last_damage = rect;
}

static void cb_log(CfrLogLevel level, const char *msg, void *user)
{
    (void)level;
    Capture *cap = user;
    cap->log_calls++;
    if (msg) {
        strncpy(cap->last_log, msg, sizeof(cap->last_log) - 1);
        cap->last_log[sizeof(cap->last_log) - 1] = '\0';
    }
}

/* make_term has to wire the callbacks after the capture exists; this
 * variant fills them in and is the one the tests use. */
static CfrTerm *make_captured_term(Capture *cap, bool with_log)
{
    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.rows = 4;
    cfg.cols = 10;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 6;
    CfrTerm *vt = cfr_new(&cfg);

    CfrCallbacks cb = { 0 };
    cb.output = cb_output;
    cb.damage = cb_damage;
    if (with_log)
        cb.log = cb_log;
    cfr_set_callbacks(vt, &cb, cap);
    return vt;
}

static void feed(CfrTerm *vt, const char *s)
{
    cfr_input_write(vt, (const uint8_t *)s, strlen(s));
}

static void reset_output(Capture *cap)
{
    cap->out_len = 0;
    cap->out[0] = '\0';
    cap->out_calls = 0;
}

static void reset_damage(Capture *cap)
{
    cap->damage_calls = 0;
    cap->last_damage = (CfrRect){ 0, 0, 0, 0 };
}

/* OSC 4 sets one entry; the built-in table is untouched. */
static void test_osc4_set(void)
{
    Capture cap = { 0 };
    CfrTerm *vt = make_captured_term(&cap, false);
    feed(vt, "\x1b]4;1;rgb:12/34/56\x1b\\");
    ASSERT_EQ(cfr_palette_rgb(vt, 1), 0x123456u);
    ASSERT_EQ(cfr_default_palette_rgb(1), 0xFF5555u);
    cfr_free(vt);
}

/* OSC 4 ; index ; ? replies in xterm's indexed form, 16-bit channels. */
static void test_osc4_query(void)
{
    Capture cap = { 0 };
    CfrTerm *vt = make_captured_term(&cap, false);
    feed(vt, "\x1b]4;1;rgb:12/34/56\x1b\\");
    feed(vt, "\x1b]4;1;?\x1b\\");
    ASSERT_STR_EQ(cap.out, "\x1b]4;1;rgb:1212/3434/5656\x1b\\");

    reset_output(&cap);
    feed(vt, "\x1b]4;2;?\x07"); /* untouched entry, BEL terminator */
    ASSERT_STR_EQ(cap.out, "\x1b]4;2;rgb:5050/fafa/7b7b\x1b\\");
    cfr_free(vt);
}

/* Every XParseColor form, plus multiple pairs and last-wins. */
static void test_osc4_spec_forms(void)
{
    Capture cap = { 0 };
    CfrTerm *vt = make_captured_term(&cap, false);

    feed(vt, "\x1b]4;10;#f00\x1b\\");
    ASSERT_EQ(cfr_palette_rgb(vt, 10), 0xFF0000u);

    feed(vt, "\x1b]4;10;#00FF00\x1b\\");
    ASSERT_EQ(cfr_palette_rgb(vt, 10), 0x00FF00u);

    feed(vt, "\x1b]4;10;#ffff00000000\x1b\\");
    ASSERT_EQ(cfr_palette_rgb(vt, 10), 0xFF0000u);

    feed(vt, "\x1b]4;10;rgb:f/0/0\x1b\\");
    ASSERT_EQ(cfr_palette_rgb(vt, 10), 0xFF0000u);

    feed(vt, "\x1b]4;10;rgb:ffff/0000/0000\x1b\\");
    ASSERT_EQ(cfr_palette_rgb(vt, 10), 0xFF0000u);

    feed(vt, "\x1b]4;10;rgb:0/ff/0\x1b\\");
    ASSERT_EQ(cfr_palette_rgb(vt, 10), 0x00FF00u);

    /* Multiple pairs in one sequence. */
    feed(vt, "\x1b]4;11;#010203;12;rgb:4/5/6\x1b\\");
    ASSERT_EQ(cfr_palette_rgb(vt, 11), 0x010203u);
    ASSERT_EQ(cfr_palette_rgb(vt, 12), 0x445566u);

    /* The last pair wins. */
    feed(vt, "\x1b]4;13;#ffffff;13;#000000\x1b\\");
    ASSERT_EQ(cfr_palette_rgb(vt, 13), 0x000000u);

    /* The full 0..255 range is addressable. */
    feed(vt, "\x1b]4;255;#123456\x1b\\");
    ASSERT_EQ(cfr_palette_rgb(vt, 255), 0x123456u);

    cfr_free(vt);
}

/* Out-of-range indices and malformed pairs are ignored. */
static void test_osc4_bad_input_ignored(void)
{
    Capture cap = { 0 };
    CfrTerm *vt = make_captured_term(&cap, false);

    feed(vt, "\x1b]4;256;#ffffff\x1b\\");
    feed(vt, "\x1b]4;-1;#ffffff\x1b\\");
    feed(vt, "\x1b]4;x;#ffffff\x1b\\");
    feed(vt, "\x1b]4;;#ffffff\x1b\\");
    feed(vt, "\x1b]4;1\x1b\\");  /* no spec */
    feed(vt, "\x1b]4;1;\x1b\\"); /* empty spec */
    feed(vt, "\x1b]4;1;rgb:zz/0/0\x1b\\");
    feed(vt, "\x1b]4\x1b\\");     /* no body */
    feed(vt, "\x1b]4;1;?\x1b\\"); /* query remains valid */

    for (int i = 0; i < 16; i++)
        ASSERT_EQ(cfr_palette_rgb(vt, (uint8_t)i), cfr_default_palette_rgb((uint8_t)i));
    ASSERT_STR_EQ(cap.out, "\x1b]4;1;rgb:ffff/5555/5555\x1b\\");
    cfr_free(vt);
}

/* Unparseable specs log once per terminal; X11 color names are out of
 * scope and take the same path. */
static void test_osc4_bad_spec_logs_once(void)
{
    Capture cap = { 0 };
    CfrTerm *vt = make_captured_term(&cap, true);

    feed(vt, "\x1b]4;1;nope\x1b\\");
    feed(vt, "\x1b]4;2;also-bad\x1b\\");
    feed(vt, "\x1b]4;3;red\x1b\\");
    ASSERT_EQ(cap.log_calls, 1);
    ASSERT_TRUE(strstr(cap.last_log, "OSC 4") != NULL);
    ASSERT_EQ(cfr_palette_rgb(vt, 1), cfr_default_palette_rgb(1));
    cfr_free(vt);
}

/* OSC 104 with a list resets just those entries; no body resets all. */
static void test_osc104_reset(void)
{
    Capture cap = { 0 };
    CfrTerm *vt = make_captured_term(&cap, false);

    feed(vt, "\x1b]4;1;#111111;2;#222222;3;#333333\x1b\\");
    ASSERT_EQ(cfr_palette_rgb(vt, 1), 0x111111u);
    ASSERT_EQ(cfr_palette_rgb(vt, 2), 0x222222u);
    ASSERT_EQ(cfr_palette_rgb(vt, 3), 0x333333u);

    feed(vt, "\x1b]104;1\x1b\\");
    ASSERT_EQ(cfr_palette_rgb(vt, 1), cfr_default_palette_rgb(1));
    ASSERT_EQ(cfr_palette_rgb(vt, 2), 0x222222u);

    feed(vt, "\x1b]104;2;3\x07");
    ASSERT_EQ(cfr_palette_rgb(vt, 2), cfr_default_palette_rgb(2));
    ASSERT_EQ(cfr_palette_rgb(vt, 3), cfr_default_palette_rgb(3));

    feed(vt, "\x1b]4;1;#111111\x1b\\");
    feed(vt, "\x1b]104\x1b\\");
    ASSERT_EQ(cfr_palette_rgb(vt, 1), cfr_default_palette_rgb(1));

    cfr_free(vt);
}

/* Overrides are global, not per screen. */
static void test_osc4_survives_altscreen(void)
{
    Capture cap = { 0 };
    CfrTerm *vt = make_captured_term(&cap, false);

    feed(vt, "\x1b]4;1;#111111\x1b\\");
    feed(vt, "\x1b[?1049h");
    ASSERT_EQ(cfr_palette_rgb(vt, 1), 0x111111u);
    feed(vt, "\x1b]4;5;#555555\x1b\\");
    feed(vt, "\x1b[?1049l");
    ASSERT_EQ(cfr_palette_rgb(vt, 1), 0x111111u);
    ASSERT_EQ(cfr_palette_rgb(vt, 5), 0x555555u);
    cfr_free(vt);
}

/* A palette change damages the grid; a no-op set does not. Indexed
 * styles keep their intern identity and repaint with the new value. */
static void test_osc4_damage_and_repaint(void)
{
    Capture cap = { 0 };
    CfrTerm *vt = make_captured_term(&cap, false);

    feed(vt, "\x1b[31mX\x1b[0m");
    cfr_damage_flush(vt);
    const CfrCell *x = cfr_get_cell(vt, 0, 0);
    ASSERT_NOT_NULL(x);
    uint32_t x_style = x->style_id;
    const CfrStyle *sx = cfr_cell_style(vt, 0, 0);
    ASSERT_NOT_NULL(sx);
    ASSERT_EQ(cfr_style_fg_rgb(vt, sx), 0xFF5555u);

    reset_damage(&cap);
    feed(vt, "\x1b]4;1;#00ff00\x1b\\");
    cfr_damage_flush(vt);
    ASSERT_EQ(cap.damage_calls, 1);
    ASSERT_EQ(cap.last_damage.start_row, 0);
    ASSERT_EQ(cap.last_damage.start_col, 0);
    ASSERT_EQ(cap.last_damage.end_row, 3);
    ASSERT_EQ(cap.last_damage.end_col, 9);
    ASSERT_EQ(cfr_style_fg_rgb(vt, sx), 0x00FF00u);

    /* The same SGR request interns to the same style id. */
    feed(vt, "\x1b[31mY\x1b[0m");
    cfr_damage_flush(vt);
    const CfrCell *y = cfr_get_cell(vt, 0, 1);
    ASSERT_NOT_NULL(y);
    ASSERT_EQ(y->style_id, x_style);
    ASSERT_EQ(cfr_style_fg_rgb(vt, cfr_cell_style(vt, 0, 1)), 0x00FF00u);

    /* Setting the same value again is not damage. */
    reset_damage(&cap);
    feed(vt, "\x1b]4;1;#00ff00\x1b\\");
    cfr_damage_flush(vt);
    ASSERT_EQ(cap.damage_calls, 0);

    /* Reset repaints back to the built-in color. */
    reset_damage(&cap);
    feed(vt, "\x1b]104;1\x1b\\");
    cfr_damage_flush(vt);
    ASSERT_EQ(cap.damage_calls, 1);
    ASSERT_EQ(cfr_style_fg_rgb(vt, sx), 0xFF5555u);
    cfr_free(vt);
}

/* RIS restores the built-in palette. */
static void test_osc4_reset_on_ris(void)
{
    Capture cap = { 0 };
    CfrTerm *vt = make_captured_term(&cap, false);

    feed(vt, "\x1b]4;1;#111111;196;#222222\x1b\\");
    ASSERT_EQ(cfr_palette_rgb(vt, 1), 0x111111u);
    ASSERT_EQ(cfr_palette_rgb(vt, 196), 0x222222u);

    feed(vt, "\x1b"
             "c"); /* RIS */
    ASSERT_EQ(cfr_palette_rgb(vt, 1), cfr_default_palette_rgb(1));
    ASSERT_EQ(cfr_palette_rgb(vt, 196), cfr_default_palette_rgb(196));
    cfr_free(vt);
}

int main(int argc, char **argv)
{
    test_parse_args(argc, argv);

    RUN_TEST(test_osc4_set);
    RUN_TEST(test_osc4_query);
    RUN_TEST(test_osc4_spec_forms);
    RUN_TEST(test_osc4_bad_input_ignored);
    RUN_TEST(test_osc4_bad_spec_logs_once);
    RUN_TEST(test_osc104_reset);
    RUN_TEST(test_osc4_survives_altscreen);
    RUN_TEST(test_osc4_damage_and_repaint);
    RUN_TEST(test_osc4_reset_on_ris);

    TEST_SUMMARY();
}
