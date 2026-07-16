/*
 * test_cfr_log — tests for coffer's logging mechanism.
 *
 * Verifies:
 * - cfr_vlog calls the log callback when registered
 * - cfr_vlog is a no-op when no callback is registered
 * - should_log_once rate-limits per-CfrTerm (fires once, then suppressed)
 * - Rate-limit resets on cfr_full_reset (ESC c)
 * - The log callback receives the correct CfrLogLevel and formatted message
 */

#include "test_helpers.h"

#include <coffer/coffer.h>
#include <string.h>

/* Need internal header for cfr_vlog and cfr_log macro. */
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

/* ---- Test log capture state ---- */

typedef struct
{
    int call_count;
    CfrLogLevel last_level;
    char last_msg[256];
} LogCapture;

static void cb_log(CfrLogLevel level, const char *msg, void *user)
{
    LogCapture *cap = (LogCapture *)user;
    cap->call_count++;
    cap->last_level = level;
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

/* ---- Tests ---- */

/* cfr_vlog is a no-op when no log callback is registered. */
static void test_vlog_no_callback(void)
{
    CfrTerm *vt = make_term();
    /* Should not crash. */
    cfr_vlog(vt, CFR_LOG_WARN, "should not appear");
    cfr_free(vt);
}

/* cfr_vlog calls the callback with the correct level and message. */
static void test_vlog_with_callback(void)
{
    LogCapture cap;
    CfrTerm *vt = make_term_with_log(&cap);

    cfr_vlog(vt, CFR_LOG_WARN, "test message %d", 42);
    ASSERT_EQ(cap.call_count, 1);
    ASSERT_EQ(cap.last_level, CFR_LOG_WARN);
    ASSERT_STR_EQ(cap.last_msg, "test message 42");

    cfr_free(vt);
}

/* cfr_vlog formats a longer message correctly. */
static void test_vlog_format(void)
{
    LogCapture cap;
    CfrTerm *vt = make_term_with_log(&cap);

    cfr_vlog(vt, CFR_LOG_DEBUG, "palette override (OSC %d) not implemented yet", 4);
    ASSERT_EQ(cap.call_count, 1);
    ASSERT_EQ(cap.last_level, CFR_LOG_DEBUG);
    ASSERT_STR_EQ(cap.last_msg, "palette override (OSC 4) not implemented yet");

    cfr_free(vt);
}

/* cfr_log macro works the same as cfr_vlog. */
static void test_cfr_log_macro(void)
{
    LogCapture cap;
    CfrTerm *vt = make_term_with_log(&cap);

    cfr_log(vt, CFR_LOG_INFO, "hello %s", "world");
    ASSERT_EQ(cap.call_count, 1);
    ASSERT_EQ(cap.last_level, CFR_LOG_INFO);
    ASSERT_STR_EQ(cap.last_msg, "hello world");

    cfr_free(vt);
}

/* should_log_once fires once, then suppresses subsequent calls. */
static void test_should_log_once_rate_limit(void)
{
    LogCapture cap;
    CfrTerm *vt = make_term_with_log(&cap);

    /* Simulate the pattern stubs will use. */
    if (should_log_once(vt, CFR_LOGGED_MEDIA_COPY))
        cfr_vlog(vt, CFR_LOG_WARN, "media copy not implemented yet");
    ASSERT_EQ(cap.call_count, 1);

    /* Second occurrence: should_log_once returns false, no log. */
    if (should_log_once(vt, CFR_LOGGED_MEDIA_COPY))
        cfr_vlog(vt, CFR_LOG_WARN, "media copy not implemented yet");
    ASSERT_EQ(cap.call_count, 1);

    cfr_free(vt);
}

/* Different bits are independent. */
static void test_should_log_once_independent_bits(void)
{
    LogCapture cap;
    CfrTerm *vt = make_term_with_log(&cap);

    if (should_log_once(vt, CFR_LOGGED_OSC4))
        cfr_vlog(vt, CFR_LOG_WARN, "osc4");
    ASSERT_EQ(cap.call_count, 1);

    if (should_log_once(vt, CFR_LOGGED_OSC104))
        cfr_vlog(vt, CFR_LOG_WARN, "osc104");
    ASSERT_EQ(cap.call_count, 2);

    /* Repeating the first should not fire. */
    if (should_log_once(vt, CFR_LOGGED_OSC4))
        cfr_vlog(vt, CFR_LOG_WARN, "osc4 again");
    ASSERT_EQ(cap.call_count, 2);

    cfr_free(vt);
}

/* Rate-limit resets on cfr_full_reset (ESC c). */
static void test_rate_limit_resets_on_full_reset(void)
{
    LogCapture cap;
    CfrTerm *vt = make_term_with_log(&cap);

    if (should_log_once(vt, CFR_LOGGED_MEDIA_COPY))
        cfr_vlog(vt, CFR_LOG_WARN, "first");
    ASSERT_EQ(cap.call_count, 1);

    /* Suppressed. */
    if (should_log_once(vt, CFR_LOGGED_MEDIA_COPY))
        cfr_vlog(vt, CFR_LOG_WARN, "second");
    ASSERT_EQ(cap.call_count, 1);

    /* Full reset should clear the logged_once bits. */
    feed(vt, "\x1b"
             "c");

    if (should_log_once(vt, CFR_LOGGED_MEDIA_COPY))
        cfr_vlog(vt, CFR_LOG_WARN, "after reset");
    ASSERT_EQ(cap.call_count, 2);

    cfr_free(vt);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    RUN_TEST(test_vlog_no_callback);
    RUN_TEST(test_vlog_with_callback);
    RUN_TEST(test_vlog_format);
    RUN_TEST(test_cfr_log_macro);
    RUN_TEST(test_should_log_once_rate_limit);
    RUN_TEST(test_should_log_once_independent_bits);
    RUN_TEST(test_rate_limit_resets_on_full_reset);

    TEST_SUMMARY();
}
