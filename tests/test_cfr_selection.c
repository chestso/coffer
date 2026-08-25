/*
 * coffer — selection tests
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#include "coffer_internal.h"
#include "test_helpers.h"

#include <coffer/coffer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static CfrTerm *make_term(int rows, int cols)
{
    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.rows = rows;
    cfg.cols = cols;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 6;
    CfrTerm *vt = cfr_new(&cfg);
    cfr_set_scrollback_size(vt, 100);
    return vt;
}

static void feed(CfrTerm *vt, const char *s)
{
    cfr_input_write(vt, (const uint8_t *)s, strlen(s));
}

/* ------------------------------------------------------------------ */
/* 1. Basic start / update / clear / query                             */
/* ------------------------------------------------------------------ */

static void test_selection_start_char(void)
{
    CfrTerm *vt = make_term(24, 80);

    cfr_selection_start(vt, 5, 10, CFR_SEL_CHAR);
    ASSERT_TRUE(cfr_selection_active(vt));

    const CfrSelection *sel = cfr_selection_get(vt);
    ASSERT_NOT_NULL(sel);
    ASSERT_EQ(sel->mode, CFR_SEL_CHAR);
    ASSERT_EQ(sel->start.row, 5);
    ASSERT_EQ(sel->start.col, 10);
    ASSERT_EQ(sel->end.row, 5);
    ASSERT_EQ(sel->end.col, 10);
    ASSERT_EQ(sel->anchor.row, 5);
    ASSERT_EQ(sel->anchor.col, 10);

    cfr_free(vt);
}

static void test_selection_update_drag(void)
{
    CfrTerm *vt = make_term(24, 80);

    cfr_selection_start(vt, 5, 10, CFR_SEL_CHAR);
    cfr_selection_update(vt, 8, 20);

    const CfrSelection *sel = cfr_selection_get(vt);
    ASSERT_EQ(sel->start.row, 5);
    ASSERT_EQ(sel->start.col, 10);
    ASSERT_EQ(sel->end.row, 8);
    ASSERT_EQ(sel->end.col, 20);

    cfr_free(vt);
}

static void test_selection_update_drag_upward(void)
{
    CfrTerm *vt = make_term(24, 80);

    cfr_selection_start(vt, 8, 20, CFR_SEL_CHAR);
    cfr_selection_update(vt, 5, 10);

    const CfrSelection *sel = cfr_selection_get(vt);
    /* start < end after normalization */
    ASSERT_EQ(sel->start.row, 5);
    ASSERT_EQ(sel->start.col, 10);
    ASSERT_EQ(sel->end.row, 8);
    ASSERT_EQ(sel->end.col, 20);

    cfr_free(vt);
}

static void test_selection_clear(void)
{
    CfrTerm *vt = make_term(24, 80);

    cfr_selection_start(vt, 5, 10, CFR_SEL_CHAR);
    ASSERT_TRUE(cfr_selection_active(vt));

    cfr_selection_clear(vt);
    ASSERT_FALSE(cfr_selection_active(vt));
    ASSERT_NULL(cfr_selection_get(vt));

    cfr_free(vt);
}

static void test_selection_in_cell(void)
{
    CfrTerm *vt = make_term(24, 80);

    cfr_selection_start(vt, 5, 10, CFR_SEL_CHAR);
    cfr_selection_update(vt, 8, 20);

    ASSERT_TRUE(cfr_selection_in_cell(vt, 5, 10));
    ASSERT_TRUE(cfr_selection_in_cell(vt, 6, 0));
    ASSERT_TRUE(cfr_selection_in_cell(vt, 8, 20));
    ASSERT_FALSE(cfr_selection_in_cell(vt, 4, 10));
    ASSERT_FALSE(cfr_selection_in_cell(vt, 9, 0));
    ASSERT_FALSE(cfr_selection_in_cell(vt, 5, 9));
    ASSERT_FALSE(cfr_selection_in_cell(vt, 8, 21));

    cfr_free(vt);
}

static void test_selection_extend(void)
{
    CfrTerm *vt = make_term(24, 80);

    cfr_selection_start(vt, 5, 10, CFR_SEL_CHAR);
    cfr_selection_update(vt, 8, 20);

    /* Extend beyond end */
    cfr_selection_extend(vt, 10, 5);
    const CfrSelection *sel = cfr_selection_get(vt);
    ASSERT_EQ(sel->end.row, 10);
    ASSERT_EQ(sel->end.col, 5);

    cfr_free(vt);
}

/* ------------------------------------------------------------------ */
/* 2. Scroll-up adjustment                                             */
/* ------------------------------------------------------------------ */

static void test_selection_scroll_up(void)
{
    CfrTerm *vt = make_term(10, 80);

    /* Fill screen so a scroll happens */
    for (int i = 0; i < 10; i++) {
        if (i)
            feed(vt, "\r\n");
        feed(vt, "line");
    }

    /* Select on rows 5-6 */
    cfr_selection_start(vt, 5, 0, CFR_SEL_CHAR);
    cfr_selection_update(vt, 6, 3);
    ASSERT_TRUE(cfr_selection_in_cell(vt, 5, 0));
    ASSERT_TRUE(cfr_selection_in_cell(vt, 6, 3));

    /* Scroll up by 1 (linefeed at bottom) */
    feed(vt, "\r\n");

    /* Selection should have shifted up by 1: rows 4-5 */
    ASSERT_TRUE(cfr_selection_in_cell(vt, 4, 0));
    ASSERT_TRUE(cfr_selection_in_cell(vt, 5, 3));
    ASSERT_FALSE(cfr_selection_in_cell(vt, 6, 0));

    cfr_free(vt);
}

static void test_selection_scroll_up_multiple(void)
{
    CfrTerm *vt = make_term(10, 80);

    for (int i = 0; i < 10; i++) {
        if (i)
            feed(vt, "\r\n");
        feed(vt, "line");
    }

    cfr_selection_start(vt, 5, 0, CFR_SEL_CHAR);
    cfr_selection_update(vt, 6, 3);

    /* Scroll up by 3 */
    feed(vt, "\r\n\r\n\r\n");

    /* Selection shifted to rows 2-3 */
    ASSERT_TRUE(cfr_selection_in_cell(vt, 2, 0));
    ASSERT_TRUE(cfr_selection_in_cell(vt, 3, 3));

    cfr_free(vt);
}

static void test_selection_scroll_up_into_scrollback(void)
{
    CfrTerm *vt = make_term(10, 80);

    for (int i = 0; i < 10; i++) {
        if (i)
            feed(vt, "\r\n");
        feed(vt, "line");
    }

    /* Select on row 0 (top of screen) */
    cfr_selection_start(vt, 0, 0, CFR_SEL_CHAR);
    cfr_selection_update(vt, 1, 3);

    /* Scroll up by 1 — row 0 goes to scrollback (-1) */
    feed(vt, "\r\n");

    /* Selection should now be at unified rows -1 and 0 */
    ASSERT_TRUE(cfr_selection_in_cell(vt, -1, 0));
    ASSERT_TRUE(cfr_selection_in_cell(vt, 0, 3));

    cfr_free(vt);
}

/* ------------------------------------------------------------------ */
/* 3. Scroll-down adjustment (reverse index)                            */
/* ------------------------------------------------------------------ */

static void test_selection_scroll_down(void)
{
    CfrTerm *vt = make_term(10, 80);

    for (int i = 0; i < 10; i++) {
        if (i)
            feed(vt, "\r\n");
        feed(vt, "line");
    }

    /* Scroll some content into scrollback first */
    feed(vt, "\r\n\r\n");

    /* Select on visible rows 3-4 */
    cfr_selection_start(vt, 3, 0, CFR_SEL_CHAR);
    cfr_selection_update(vt, 4, 3);
    ASSERT_TRUE(cfr_selection_in_cell(vt, 3, 0));

    /* Move cursor to row 0 (scroll_top) so RI triggers a scroll-down */
    feed(vt, "\x1b[1;1H");
    /* Reverse index (ESC M) scrolls down by 1 */
    feed(vt, "\x1bM");

    /* Selection should have shifted down by 1: rows 4-5 */
    ASSERT_TRUE(cfr_selection_in_cell(vt, 4, 0));
    ASSERT_TRUE(cfr_selection_in_cell(vt, 5, 3));
    ASSERT_FALSE(cfr_selection_in_cell(vt, 3, 0));

    cfr_free(vt);
}

/* ------------------------------------------------------------------ */
/* 4. Draw clears selection                                             */
/* ------------------------------------------------------------------ */

static void test_selection_draw_clears(void)
{
    CfrTerm *vt = make_term(24, 80);

    /* Write some content and select it */
    feed(vt, "hello");
    cfr_selection_start(vt, 0, 0, CFR_SEL_CHAR);
    cfr_selection_update(vt, 0, 4);
    ASSERT_TRUE(cfr_selection_active(vt));

    /* Draw a character on the selected row — should clear selection */
    feed(vt, "\rX");
    ASSERT_FALSE(cfr_selection_active(vt));

    cfr_free(vt);
}

static void test_selection_draw_outside_preserves(void)
{
    CfrTerm *vt = make_term(24, 80);

    feed(vt, "hello");
    cfr_selection_start(vt, 0, 0, CFR_SEL_CHAR);
    cfr_selection_update(vt, 0, 4);
    ASSERT_TRUE(cfr_selection_active(vt));

    /* Move cursor to row 5 and draw — selection on row 0 should survive */
    feed(vt, "\x1b[6;1HX");
    ASSERT_TRUE(cfr_selection_active(vt));

    cfr_free(vt);
}

/* ------------------------------------------------------------------ */
/* 5. Altscreen clears selection                                       */
/* ------------------------------------------------------------------ */

static void test_selection_altscreen_clears(void)
{
    CfrTerm *vt = make_term(24, 80);

    feed(vt, "hello");
    cfr_selection_start(vt, 0, 0, CFR_SEL_CHAR);
    ASSERT_TRUE(cfr_selection_active(vt));

    /* Enter altscreen — should clear selection */
    feed(vt, "\x1b[?1049h");
    ASSERT_FALSE(cfr_selection_active(vt));

    cfr_free(vt);
}

/* ------------------------------------------------------------------ */
/* 6. Callback fires correctly                                         */
/* ------------------------------------------------------------------ */

static int cb_active_count = 0;
static int cb_inactive_count = 0;

static void tracking_cb(bool active, void *user)
{
    (void)user;
    if (active)
        cb_active_count++;
    else
        cb_inactive_count++;
}

static void test_callback_fires_on_start(void)
{
    cb_active_count = 0;
    cb_inactive_count = 0;
    CfrTerm *vt = make_term(24, 80);

    CfrCallbacks cb = { 0 };
    cb.selection_changed = tracking_cb;
    cfr_set_callbacks(vt, &cb, NULL);

    cfr_selection_start(vt, 5, 10, CFR_SEL_CHAR);
    ASSERT_EQ(cb_active_count, 1);
    ASSERT_EQ(cb_inactive_count, 0);

    cfr_free(vt);
}

static void test_callback_fires_on_clear(void)
{
    cb_active_count = 0;
    cb_inactive_count = 0;
    CfrTerm *vt = make_term(24, 80);

    CfrCallbacks cb = { 0 };
    cb.selection_changed = tracking_cb;
    cfr_set_callbacks(vt, &cb, NULL);

    cfr_selection_start(vt, 5, 10, CFR_SEL_CHAR);
    cfr_selection_clear(vt);
    ASSERT_EQ(cb_active_count, 1);
    ASSERT_EQ(cb_inactive_count, 1);

    cfr_free(vt);
}

static void test_callback_not_fired_on_update(void)
{
    cb_active_count = 0;
    cb_inactive_count = 0;
    CfrTerm *vt = make_term(24, 80);

    CfrCallbacks cb = { 0 };
    cb.selection_changed = tracking_cb;
    cfr_set_callbacks(vt, &cb, NULL);

    cfr_selection_start(vt, 5, 10, CFR_SEL_CHAR);
    cfr_selection_update(vt, 8, 20);
    ASSERT_EQ(cb_active_count, 1);
    ASSERT_EQ(cb_inactive_count, 0);

    cfr_free(vt);
}

static void test_callback_fires_on_draw_clear(void)
{
    cb_active_count = 0;
    cb_inactive_count = 0;
    CfrTerm *vt = make_term(24, 80);

    CfrCallbacks cb = { 0 };
    cb.selection_changed = tracking_cb;
    cfr_set_callbacks(vt, &cb, NULL);

    feed(vt, "hello");
    cfr_selection_start(vt, 0, 0, CFR_SEL_CHAR);
    ASSERT_EQ(cb_active_count, 1);

    /* Draw on selected row — should fire callback with active=false */
    feed(vt, "\rX");
    ASSERT_EQ(cb_inactive_count, 1);

    cfr_free(vt);
}

/* ------------------------------------------------------------------ */
/* 7. Text extraction                                                  */
/* ------------------------------------------------------------------ */

static void test_selection_get_text_basic(void)
{
    CfrTerm *vt = make_term(24, 80);

    feed(vt, "hello world");
    cfr_selection_start(vt, 0, 0, CFR_SEL_CHAR);
    cfr_selection_update(vt, 0, 4);

    char *text = cfr_selection_get_text(vt);
    ASSERT_NOT_NULL(text);
    ASSERT_STR_EQ(text, "hello");
    free(text);

    cfr_free(vt);
}

static void test_selection_get_text_multiline(void)
{
    CfrTerm *vt = make_term(24, 80);

    feed(vt, "line1\r\nline2");
    cfr_selection_start(vt, 0, 0, CFR_SEL_CHAR);
    cfr_selection_update(vt, 1, 4);

    char *text = cfr_selection_get_text(vt);
    ASSERT_NOT_NULL(text);
    ASSERT_STR_EQ(text, "line1\nline2");
    free(text);

    cfr_free(vt);
}

/* ------------------------------------------------------------------ */
/* 8. Word mode                                                        */
/* ------------------------------------------------------------------ */

static void test_selection_word_mode(void)
{
    CfrTerm *vt = make_term(24, 80);

    feed(vt, "hello world");
    cfr_selection_start(vt, 0, 2, CFR_SEL_WORD);

    const CfrSelection *sel = cfr_selection_get(vt);
    ASSERT_NOT_NULL(sel);
    /* "hello" starts at col 0, ends at col 4 */
    ASSERT_EQ(sel->start.col, 0);
    ASSERT_EQ(sel->end.col, 4);

    cfr_free(vt);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    printf("test_cfr_selection\n");

    RUN_TEST(test_selection_start_char);
    RUN_TEST(test_selection_update_drag);
    RUN_TEST(test_selection_update_drag_upward);
    RUN_TEST(test_selection_clear);
    RUN_TEST(test_selection_in_cell);
    RUN_TEST(test_selection_extend);

    RUN_TEST(test_selection_scroll_up);
    RUN_TEST(test_selection_scroll_up_multiple);
    RUN_TEST(test_selection_scroll_up_into_scrollback);
    RUN_TEST(test_selection_scroll_down);

    RUN_TEST(test_selection_draw_clears);
    RUN_TEST(test_selection_draw_outside_preserves);

    RUN_TEST(test_selection_altscreen_clears);

    RUN_TEST(test_callback_fires_on_start);
    RUN_TEST(test_callback_fires_on_clear);
    RUN_TEST(test_callback_not_fired_on_update);
    RUN_TEST(test_callback_fires_on_draw_clear);

    RUN_TEST(test_selection_get_text_basic);
    RUN_TEST(test_selection_get_text_multiline);

    RUN_TEST(test_selection_word_mode);

    TEST_SUMMARY();
}
