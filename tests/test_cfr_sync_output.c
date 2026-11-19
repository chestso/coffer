/* tests/test_cfr_sync_output.c — synchronized output (DECSET 2026).
 *
 * While the mode is on the application is mid-frame: cfr_damage_flush()
 * must report nothing and keep the rectangular union pending, so the host
 * paints once — the whole frame — after the end marker (or after its own
 * timeout calls cfr_sync_output_cancel()). The cursor fold is deferred
 * with the cells; folding it at a held flush would lose a cursor move
 * made inside the frame.
 */

#include "coffer_internal.h"
#include "test_helpers.h"
#include <coffer/coffer.h>

#include <stdio.h>
#include <string.h>

static int g_damage_calls;
static CfrRect g_damage_union;
static int g_sync_on;
static int g_sync_off;

static void reset_counters(void)
{
    g_damage_calls = 0;
    g_damage_union = (CfrRect){ 0, 0, 0, 0 };
    g_sync_on = 0;
    g_sync_off = 0;
}

static void on_damage(CfrRect r, void *u)
{
    (void)u;
    g_damage_calls++;
    if (g_damage_calls == 1) {
        g_damage_union = r;
        return;
    }
    if (r.start_row < g_damage_union.start_row)
        g_damage_union.start_row = r.start_row;
    if (r.start_col < g_damage_union.start_col)
        g_damage_union.start_col = r.start_col;
    if (r.end_row > g_damage_union.end_row)
        g_damage_union.end_row = r.end_row;
    if (r.end_col > g_damage_union.end_col)
        g_damage_union.end_col = r.end_col;
}

static void on_set_mode(CfrMode mode, bool on, void *u)
{
    (void)u;
    if (mode != CFR_MODE_SYNC_OUTPUT)
        return;
    if (on)
        g_sync_on++;
    else
        g_sync_off++;
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
    cb.damage = on_damage;
    cb.set_mode = on_set_mode;
    cfr_set_callbacks(vt, &cb, NULL);
    reset_counters();
    return vt;
}

static void feed(CfrTerm *vt, const char *s)
{
    cfr_input_write(vt, (const uint8_t *)s, strlen(s));
}

/* Control: with the mode off the flush delivers immediately. With it on,
 * nothing is delivered and the mode state is reported. */
static void test_flush_holds_damage(void)
{
    CfrTerm *vt = make_term(4, 10);
    feed(vt, "AAAA");
    cfr_damage_flush(vt);
    ASSERT_EQ(g_damage_calls, 1);

    reset_counters();
    feed(vt, "\x1b[?2026h");
    ASSERT_TRUE(cfr_get_mode(vt, CFR_MODE_SYNC_OUTPUT));
    ASSERT_EQ(g_sync_on, 1);

    feed(vt, "\x1b[1;6HBBBB");
    cfr_damage_flush(vt);
    ASSERT_EQ(g_damage_calls, 0);
    cfr_free(vt);
}

/* The end marker releases one callback whose rect covers the whole frame
 * — both writes, on different rows. The cursor is parked where the last
 * delivered flush left it, so the rect is exactly the two writes. */
static void test_end_delivers_union(void)
{
    CfrTerm *vt = make_term(4, 10);
    feed(vt, "\x1b[?2026h\x1b[1;1HAA\x1b[3;1HCC");
    cfr_damage_flush(vt);
    ASSERT_EQ(g_damage_calls, 0);

    feed(vt, "\x1b[H"); /* back to the cell the last flush knew about */
    feed(vt, "\x1b[?2026l");
    cfr_damage_flush(vt);
    ASSERT_EQ(g_damage_calls, 1);
    ASSERT_EQ(g_damage_union.start_row, 0);
    ASSERT_EQ(g_damage_union.start_col, 0);
    ASSERT_EQ(g_damage_union.end_row, 2);
    ASSERT_EQ(g_damage_union.end_col, 1);

    /* Drained: a second flush reports nothing. */
    cfr_damage_flush(vt);
    ASSERT_EQ(g_damage_calls, 1);
    cfr_free(vt);
}

/* ?2026h is a flag, not a nesting counter: a repeat does not re-notify,
 * and one end marker finishes the frame. */
static void test_repeated_begin_is_idempotent(void)
{
    CfrTerm *vt = make_term(4, 10);
    feed(vt, "\x1b[?2026h");
    ASSERT_EQ(g_sync_on, 1);
    feed(vt, "\x1b[?2026h");
    ASSERT_EQ(g_sync_on, 1);
    feed(vt, "\x1b[?2026l");
    ASSERT_EQ(g_sync_off, 1);
    ASSERT_FALSE(cfr_get_mode(vt, CFR_MODE_SYNC_OUTPUT));
    cfr_free(vt);
}

/* A cursor-only move inside the frame folds in at delivery: the rect must
 * cover both the old and the new cursor cell. */
static void test_cursor_moves_fold_at_delivery(void)
{
    CfrTerm *vt = make_term(4, 10);
    cfr_damage_flush(vt); /* no spurious damage; dmg cursor = (0,0) */
    reset_counters();

    feed(vt, "\x1b[?2026h");
    feed(vt, "\x1b[3;5H");
    cfr_damage_flush(vt); /* held: the cursor must not fold here */
    ASSERT_EQ(g_damage_calls, 0);

    feed(vt, "\x1b[?2026l");
    cfr_damage_flush(vt);
    ASSERT_EQ(g_damage_calls, 1);
    ASSERT_EQ(g_damage_union.start_row, 0);
    ASSERT_EQ(g_damage_union.start_col, 0);
    ASSERT_EQ(g_damage_union.end_row, 2);
    ASSERT_EQ(g_damage_union.end_col, 4);
    cfr_free(vt);
}

/* Output written just before the begin marker belongs to the frame too:
 * the flush after ?2026h reports nothing. */
static void test_pending_before_begin_is_held(void)
{
    CfrTerm *vt = make_term(4, 10);
    feed(vt, "A");
    feed(vt, "\x1b[?2026h");
    cfr_damage_flush(vt);
    ASSERT_EQ(g_damage_calls, 0);

    feed(vt, "\x1b[?2026l");
    cfr_damage_flush(vt);
    ASSERT_EQ(g_damage_calls, 1);
    cfr_free(vt);
}

/* RIS clears the mode (and reports it) but keeps the pending damage, so
 * the reset screen is painted on the next flush. */
static void test_ris_ends_hold(void)
{
    CfrTerm *vt = make_term(4, 10);
    feed(vt, "\x1b[?2026h");
    feed(vt, "A");
    ASSERT_TRUE(cfr_get_mode(vt, CFR_MODE_SYNC_OUTPUT));

    cfr_reset(vt);
    ASSERT_FALSE(cfr_get_mode(vt, CFR_MODE_SYNC_OUTPUT));
    ASSERT_EQ(g_sync_off, 1);

    cfr_damage_flush(vt);
    ASSERT_EQ(g_damage_calls, 1);
    cfr_free(vt);
}

/* cfr_reset_input_state() is the "the program died" recovery: it must end
 * a hold too, or the screen stays frozen. */
static void test_reset_input_state_ends_hold(void)
{
    CfrTerm *vt = make_term(4, 10);
    feed(vt, "\x1b[?2026h");
    feed(vt, "A");
    ASSERT_TRUE(cfr_get_mode(vt, CFR_MODE_SYNC_OUTPUT));

    cfr_reset_input_state(vt);
    ASSERT_FALSE(cfr_get_mode(vt, CFR_MODE_SYNC_OUTPUT));
    ASSERT_EQ(g_sync_off, 1);

    cfr_damage_flush(vt);
    ASSERT_EQ(g_damage_calls, 1);
    cfr_free(vt);
}

/* The host timeout path: cancel ends the frame without touching anything
 * else, the partial frame paints, and the application's late end marker
 * is harmless. */
static void test_cancel_delivers_partial_frame(void)
{
    CfrTerm *vt = make_term(4, 10);
    feed(vt, "\x1b[>1u"); /* kitty keyboard flags 1 — must survive cancel */
    feed(vt, "\x1b[?2026h\x1b[1;1HAA");
    ASSERT_TRUE(cfr_get_mode(vt, CFR_MODE_SYNC_OUTPUT));

    cfr_sync_output_cancel(vt);
    ASSERT_FALSE(cfr_get_mode(vt, CFR_MODE_SYNC_OUTPUT));
    ASSERT_EQ(g_sync_off, 1);
    ASSERT_EQ(cfr_get_kitty_kb_flags(vt), 1u);

    cfr_damage_flush(vt);
    ASSERT_EQ(g_damage_calls, 1);
    ASSERT_EQ(cfr_get_cell(vt, 0, 0)->cp, (uint32_t)'A');

    /* The application's own end marker, and a second cancel, are no-ops. */
    feed(vt, "\x1b[?2026l");
    cfr_sync_output_cancel(vt);
    ASSERT_EQ(g_sync_off, 1);
    cfr_free(vt);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    RUN_TEST(test_flush_holds_damage);
    RUN_TEST(test_end_delivers_union);
    RUN_TEST(test_repeated_begin_is_idempotent);
    RUN_TEST(test_cursor_moves_fold_at_delivery);
    RUN_TEST(test_pending_before_begin_is_held);
    RUN_TEST(test_ris_ends_hold);
    RUN_TEST(test_reset_input_state_ends_hold);
    RUN_TEST(test_cancel_delivers_partial_frame);

    TEST_SUMMARY();
}
