/* tests/test_bvt_keys.c — kitty keyboard protocol coverage */

#include "bloom_vt_internal.h"
#include "test_helpers.h"
#include <bloom-vt/bloom_vt.h>

#include <stdio.h>
#include <string.h>

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

static BvtTerm *make_term(void)
{
    BvtConfig cfg = BVT_CONFIG_DEFAULTS;
    cfg.rows = 24;
    cfg.cols = 80;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 6;
    BvtTerm *vt = bvt_new(&cfg);
    BvtCallbacks cb = { 0 };
    cb.output = on_output;
    bvt_set_callbacks(vt, &cb, NULL);
    g_output_len = 0;
    g_output_buf[0] = '\0';
    return vt;
}

static void feed(BvtTerm *vt, const char *s)
{
    bvt_input_write(vt, (const uint8_t *)s, strlen(s));
}

static void clear_output(void)
{
    g_output_len = 0;
    g_output_buf[0] = '\0';
}

/* Without the kitty protocol pushed, Shift+Enter still produces a bare
 * \r — preserving the legacy behaviour every other terminal had until
 * kitty-style progressive enhancement landed. */
static void test_default_shift_enter_is_cr(void)
{
    BvtTerm *vt = make_term();
    bvt_send_key(vt, BVT_KEY_ENTER, BVT_MOD_SHIFT);
    ASSERT_STR_EQ(g_output_buf, "\r");
    bvt_free(vt);
}

/* After CSI > 1 u (push Disambiguate), Shift+Enter should encode as
 * the kitty CSI-u form so the application can tell it apart from a
 * plain Enter — this is the original Claude Code regression. */
static void test_disambiguate_shift_enter(void)
{
    BvtTerm *vt = make_term();
    feed(vt, "\x1b[>1u");
    clear_output();
    bvt_send_key(vt, BVT_KEY_ENTER, BVT_MOD_SHIFT);
    ASSERT_STR_EQ(g_output_buf, "\x1b[13;2u");
    bvt_free(vt);
}

/* Bare Enter under Disambiguate still emits \r — flag 0x1's whole
 * point is to only kick in when there is something to disambiguate. */
static void test_disambiguate_bare_enter_unchanged(void)
{
    BvtTerm *vt = make_term();
    feed(vt, "\x1b[>1u");
    clear_output();
    bvt_send_key(vt, BVT_KEY_ENTER, 0);
    ASSERT_STR_EQ(g_output_buf, "\r");
    bvt_free(vt);
}

/* Disambiguate covers Tab, Backspace and Escape too. */
static void test_disambiguate_special_keys(void)
{
    BvtTerm *vt = make_term();
    feed(vt, "\x1b[>1u");

    clear_output();
    bvt_send_key(vt, BVT_KEY_TAB, BVT_MOD_CTRL);
    ASSERT_STR_EQ(g_output_buf, "\x1b[9;5u");

    clear_output();
    bvt_send_key(vt, BVT_KEY_BACKSPACE, BVT_MOD_ALT);
    ASSERT_STR_EQ(g_output_buf, "\x1b[127;3u");

    clear_output();
    bvt_send_key(vt, BVT_KEY_ESCAPE, BVT_MOD_SHIFT);
    ASSERT_STR_EQ(g_output_buf, "\x1b[27;2u");

    bvt_free(vt);
}

/* Ctrl+letter through bvt_send_text: legacy behaviour transforms 'a'
 * to 0x01; with Disambiguate active it goes through CSI-u with the
 * codepoint lowercased and Ctrl encoded as mod=5. */
static void test_disambiguate_ctrl_letter(void)
{
    BvtTerm *vt = make_term();
    feed(vt, "\x1b[>1u");
    clear_output();
    bvt_send_text(vt, "a", 1, BVT_MOD_CTRL);
    ASSERT_STR_EQ(g_output_buf, "\x1b[97;5u");
    bvt_free(vt);
}

/* Ctrl+Shift+letter — codepoint stays lowercased, mod = 1 + shift
 * (1) + ctrl (4) = 6, so the encoded mod parameter is 6. */
static void test_disambiguate_ctrl_shift_letter(void)
{
    BvtTerm *vt = make_term();
    feed(vt, "\x1b[>1u");
    clear_output();
    bvt_send_text(vt, "a", 1, BVT_MOD_CTRL | BVT_MOD_SHIFT);
    ASSERT_STR_EQ(g_output_buf, "\x1b[97;6u");
    bvt_free(vt);
}

/* Without kitty active, Ctrl+A must keep producing the literal control
 * byte 0x01 — anything else would break shell signal handling. */
static void test_default_ctrl_letter_is_control_byte(void)
{
    BvtTerm *vt = make_term();
    bvt_send_text(vt, "a", 1, BVT_MOD_CTRL);
    ASSERT_EQ(g_output_len, (size_t)1);
    ASSERT_EQ((unsigned char)g_output_buf[0], 0x01u);
    bvt_free(vt);
}

/* Push two distinct flag sets, pop one, confirm the lower set is
 * restored as the active baseline by observing key-emit behaviour. */
static void test_push_pop_restores_flags(void)
{
    BvtTerm *vt = make_term();
    feed(vt, "\x1b[>1u"); /* push flags=1 (disambiguate) */
    feed(vt, "\x1b[>9u"); /* push flags=9 (disambig + report-all) */
    feed(vt, "\x1b[<u");  /* pop one — back to flags=1 */
    clear_output();
    /* Bare Tab under flags=1 alone should NOT use CSI-u (no mods,
     * report-all is off again). */
    bvt_send_key(vt, BVT_KEY_TAB, 0);
    ASSERT_STR_EQ(g_output_buf, "\t");

    /* Shift+Enter still encodes (disambiguate is still on). */
    clear_output();
    bvt_send_key(vt, BVT_KEY_ENTER, BVT_MOD_SHIFT);
    ASSERT_STR_EQ(g_output_buf, "\x1b[13;2u");
    bvt_free(vt);
}

/* Popping when the stack is at depth 0 must be a no-op, never crash,
 * and must not push the depth into negative territory. */
static void test_pop_past_empty(void)
{
    BvtTerm *vt = make_term();
    feed(vt, "\x1b[<u");
    feed(vt, "\x1b[<u");
    feed(vt, "\x1b[<u");
    ASSERT_EQ((int)vt->kitty_kb_depth, 0);
    ASSERT_EQ(vt->kitty_kb_stack[0], 0u);
    bvt_free(vt);
}

/* Pushing past the 16-deep stack must clamp at depth 15 with the
 * latest flag mask present at the top — matches kitty's own behaviour. */
static void test_push_past_capacity(void)
{
    BvtTerm *vt = make_term();
    for (int i = 0; i < 20; ++i)
        feed(vt, "\x1b[>1u");
    feed(vt, "\x1b[>5u"); /* final push with flags=5 */
    ASSERT_EQ((int)vt->kitty_kb_depth, 15);
    ASSERT_EQ(vt->kitty_kb_stack[15], 5u);
    bvt_free(vt);
}

/* CSI ? u must reply with the active flag mask in CSI-? <flags> u
 * form so applications can probe without pushing/popping. */
static void test_query_active_flags(void)
{
    BvtTerm *vt = make_term();
    feed(vt, "\x1b[>9u");
    clear_output();
    feed(vt, "\x1b[?u");
    ASSERT_STR_EQ(g_output_buf, "\x1b[?9u");
    bvt_free(vt);
}

/* CSI = flags ; mode u: per the kitty keyboard protocol, mode 1
 * (default) is reset-all-then-set, mode 2 is OR in, mode 3 is AND-NOT.
 * Crush's exit cleanup uses the mode-1 form `CSI = 0 ; 1 u` to wipe the
 * flag set; if mode 1 is mis-implemented as OR, the cleanup is a no-op
 * and the disambiguate flag stays set, which causes ctrl-letters to
 * keep arriving as CSI-u sequences after the program exits. */
static void test_set_clear_replace(void)
{
    BvtTerm *vt = make_term();
    feed(vt, "\x1b[>1u"); /* depth=1, flags=1 */

    feed(vt, "\x1b[=8;2u"); /* mode 2 = set/OR in 0x8 → 0x9 */
    ASSERT_EQ(vt->kitty_kb_stack[vt->kitty_kb_depth], 9u);

    feed(vt, "\x1b[=1;3u"); /* mode 3 = reset bit 0x1 → 0x8 */
    ASSERT_EQ(vt->kitty_kb_stack[vt->kitty_kb_depth], 8u);

    feed(vt, "\x1b[=4;1u"); /* mode 1 = reset all + set 0x4 → 0x4 */
    ASSERT_EQ(vt->kitty_kb_stack[vt->kitty_kb_depth], 4u);

    /* Crush's exit form: `CSI = 0 ; 1 u` clears every flag. */
    feed(vt, "\x1b[=0;1u");
    ASSERT_EQ(vt->kitty_kb_stack[vt->kitty_kb_depth], 0u);

    /* Default mode is 1; bare `CSI = N u` is reset-and-set. */
    feed(vt, "\x1b[=2u");
    ASSERT_EQ(vt->kitty_kb_stack[vt->kitty_kb_depth], 2u);
    bvt_free(vt);
}

/* Report-all (flag 0x8) makes even an unmodified Tab go through
 * CSI-u. Useful for TUIs that want unambiguous key reporting. */
static void test_report_all_bare_tab(void)
{
    BvtTerm *vt = make_term();
    feed(vt, "\x1b[>9u"); /* disambig + report-all */
    clear_output();
    bvt_send_key(vt, BVT_KEY_TAB, 0);
    ASSERT_STR_EQ(g_output_buf, "\x1b[9u");
    bvt_free(vt);
}

/* CSI > 4 ; 2 m is xterm's modifyOtherKeys; the same params interpreted
 * as SGR mean "underline; faint". Claude Code (and other modern TUIs)
 * emit this on startup right next to their kitty-stack push, so if we
 * route it through SGR every subsequent character renders underlined.
 * The intermediate must be honoured. */
static void test_modify_other_keys_is_not_sgr(void)
{
    BvtTerm *vt = make_term();
    feed(vt, "\x1b[>4;2m"); /* must NOT touch the SGR pen */
    feed(vt, "X");
    const BvtCell *c = bvt_get_cell(vt, 0, 0);
    ASSERT_NOT_NULL(c);
    ASSERT_EQ(c->cp, (uint32_t)'X');
    /* Default style — no underline, no faint set. The simplest
     * post-condition is that the style id matches the default
     * (0) since no SGR has been applied. */
    ASSERT_EQ(c->style_id, 0u);
    bvt_free(vt);
}

/* RIS (ESC c) — what `reset(1)` sends — must pop every kitty keyboard
 * flag. Without this, a TUI that pushed `CSI > 1 u` and crashed before
 * popping leaves Ctrl+letter routed through CSI-u, breaking the shell:
 * Ctrl+C arrives as `ESC[99;5u` instead of 0x03, so SIGINT never fires
 * and `reset` doesn't fix it. The regression this test guards. */
static void test_ris_clears_kitty_stack(void)
{
    BvtTerm *vt = make_term();
    feed(vt, "\x1b[>1u"); /* push disambiguate */
    feed(vt, "\x1b[>9u"); /* push disambig + report-all */
    ASSERT_EQ((int)vt->kitty_kb_depth, 2);

    feed(vt, "\x1b"
             "c"); /* RIS */
    ASSERT_EQ((int)vt->kitty_kb_depth, 0);
    ASSERT_EQ(vt->kitty_kb_stack[0], 0u);

    /* The behavioural assertion: Ctrl+a is back to 0x01, not CSI-u. */
    clear_output();
    bvt_send_text(vt, "a", 1, BVT_MOD_CTRL);
    ASSERT_EQ(g_output_len, (size_t)1);
    ASSERT_EQ((unsigned char)g_output_buf[0], 0x01u);
    bvt_free(vt);
}

/* RIS clears DEC private modes (mouse, bracketed paste, DECCKM, ...).
 * Without this, an app that left mouse tracking on or arrows in
 * application mode would persist through `reset`. */
static void test_ris_clears_modes(void)
{
    BvtTerm *vt = make_term();
    feed(vt, "\x1b[?1000h"); /* mouse btn-event */
    feed(vt, "\x1b[?2004h"); /* bracketed paste */
    feed(vt, "\x1b[?1h");    /* DECCKM */
    ASSERT_TRUE(bvt_get_mode(vt, BVT_MODE_MOUSE_BTN_EVENT));
    ASSERT_TRUE(bvt_get_mode(vt, BVT_MODE_BRACKETED_PASTE));
    ASSERT_TRUE(vt->decckm);

    feed(vt, "\x1b"
             "c"); /* RIS */
    ASSERT_TRUE(!bvt_get_mode(vt, BVT_MODE_MOUSE_BTN_EVENT));
    ASSERT_TRUE(!bvt_get_mode(vt, BVT_MODE_BRACKETED_PASTE));
    ASSERT_TRUE(!vt->decckm);

    /* Cursor visible/blink defaults are restored, not zeroed. */
    ASSERT_TRUE(bvt_get_mode(vt, BVT_MODE_CURSOR_VISIBLE));
    ASSERT_TRUE(bvt_get_mode(vt, BVT_MODE_CURSOR_BLINK));

    /* Behavioural: arrow keys are back in normal mode (ESC[A, not ESCOA). */
    clear_output();
    bvt_send_key(vt, BVT_KEY_UP, 0);
    ASSERT_STR_EQ(g_output_buf, "\x1b[A");
    bvt_free(vt);
}

/* RIS must fire set_mode for every mode that flips off so the host
 * tears down mouse capture, paste handling, etc. */
static int g_set_mode_calls;
static BvtMode g_set_mode_last_mode;
static bool g_set_mode_last_on;
static bool g_set_mode_saw_mouse_off;
static bool g_set_mode_saw_paste_off;
static void on_set_mode(BvtMode m, bool on, void *u)
{
    (void)u;
    g_set_mode_calls++;
    g_set_mode_last_mode = m;
    g_set_mode_last_on = on;
    if (m == BVT_MODE_MOUSE_BTN_EVENT && !on)
        g_set_mode_saw_mouse_off = true;
    if (m == BVT_MODE_BRACKETED_PASTE && !on)
        g_set_mode_saw_paste_off = true;
}

static void test_ris_fires_set_mode_callback(void)
{
    BvtConfig cfg = BVT_CONFIG_DEFAULTS;
    cfg.rows = 24;
    cfg.cols = 80;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 6;
    BvtTerm *vt = bvt_new(&cfg);
    BvtCallbacks cb = { 0 };
    cb.output = on_output;
    cb.set_mode = on_set_mode;
    bvt_set_callbacks(vt, &cb, NULL);
    g_output_len = 0;

    feed(vt, "\x1b[?1000h"); /* mouse on — host gets set_mode(..., true) */
    feed(vt, "\x1b[?2004h"); /* paste on */

    g_set_mode_calls = 0;
    g_set_mode_saw_mouse_off = false;
    g_set_mode_saw_paste_off = false;

    feed(vt, "\x1b"
             "c"); /* RIS */

    ASSERT_TRUE(g_set_mode_saw_mouse_off);
    ASSERT_TRUE(g_set_mode_saw_paste_off);
    bvt_free(vt);
}

/* Bare CSI u (no intermediate) must still behave as ANSI restore-
 * cursor — not regressing the pre-kitty behaviour everything else
 * already relies on. */
static void test_bare_csi_u_still_restores(void)
{
    BvtTerm *vt = make_term();
    feed(vt, "\x1b[5;5H\x1b[s"); /* save (4,4) */
    feed(vt, "\x1b[10;10H");     /* move to (9,9) */
    feed(vt, "\x1b[u");          /* bare restore */
    BvtCursor c = bvt_get_cursor(vt);
    ASSERT_EQ(c.row, 4);
    ASSERT_EQ(c.col, 4);
    bvt_free(vt);
}

int main(int argc, char **argv)
{
    test_parse_args(argc, argv);
    RUN_TEST(test_default_shift_enter_is_cr);
    RUN_TEST(test_disambiguate_shift_enter);
    RUN_TEST(test_disambiguate_bare_enter_unchanged);
    RUN_TEST(test_disambiguate_special_keys);
    RUN_TEST(test_disambiguate_ctrl_letter);
    RUN_TEST(test_disambiguate_ctrl_shift_letter);
    RUN_TEST(test_default_ctrl_letter_is_control_byte);
    RUN_TEST(test_push_pop_restores_flags);
    RUN_TEST(test_pop_past_empty);
    RUN_TEST(test_push_past_capacity);
    RUN_TEST(test_query_active_flags);
    RUN_TEST(test_set_clear_replace);
    RUN_TEST(test_report_all_bare_tab);
    RUN_TEST(test_modify_other_keys_is_not_sgr);
    RUN_TEST(test_ris_clears_kitty_stack);
    RUN_TEST(test_ris_clears_modes);
    RUN_TEST(test_ris_fires_set_mode_callback);
    RUN_TEST(test_bare_csi_u_still_restores);
    TEST_SUMMARY();
}
