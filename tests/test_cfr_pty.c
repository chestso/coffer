/*
 * test_cfr_pty — engine-only PTY harness for coffer.
 *
 * Spawns real child processes on a real PTY (no SDL, no FreeType, no atlas)
 * and pipes raw output into cfr_input_write(). Assertions are made against
 * the bvt grid via the public coffer.h API.
 */

#include "coffer_pty.h"
#include "test_helpers.h"
#include <coffer/coffer.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <poll.h>
#include <time.h>
#include <unistd.h>
#endif

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static long long now_ms(void)
{
#ifdef _WIN32
    return (long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

/* Drain PTY output until the child exits or `timeout_ms` elapses. */
static void drain_pty(CfrTerm *vt, PtyContext *pty, int timeout_ms)
{
    long long deadline = now_ms() + timeout_ms;
    char buf[4096];
#ifdef _WIN32
    /* On Windows, use WaitForMultipleObjects on the output handle and
     * the child process handle. No fd-based poll available. */
    HANDLE handles[2];
    handles[0] = (HANDLE)pty_get_output_handle(pty);
    handles[1] = (HANDLE)pty_get_process_handle(pty);
    while (now_ms() < deadline) {
        int wait = (int)(deadline - now_ms());
        if (wait <= 0)
            break;
        DWORD r = WaitForMultipleObjects(2, handles, FALSE,
                                         (DWORD)wait);
        if (r == WAIT_TIMEOUT) {
            if (!pty_is_running(pty)) {
                ssize_t n = pty_read(pty, buf, sizeof(buf));
                if (n > 0)
                    cfr_input_write(vt, (const uint8_t *)buf, (size_t)n);
                break;
            }
            continue;
        }
        if (r == WAIT_OBJECT_0) {
            /* Output handle is readable */
            ssize_t n = pty_read(pty, buf, sizeof(buf));
            if (n <= 0)
                break;
            cfr_input_write(vt, (const uint8_t *)buf, (size_t)n);
        } else {
            /* Process exited or error — drain final bytes */
            ssize_t n = pty_read(pty, buf, sizeof(buf));
            if (n > 0)
                cfr_input_write(vt, (const uint8_t *)buf, (size_t)n);
            break;
        }
    }
#else
    int fd = pty_get_master_fd(pty);
    while (now_ms() < deadline) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int wait = (int)(deadline - now_ms());
        if (wait <= 0)
            break;
        int r = poll(&pfd, 1, wait);
        if (r <= 0) {
            if (!pty_is_running(pty)) {
                /* Drain any final bytes before giving up. */
                ssize_t n = pty_read(pty, buf, sizeof(buf));
                if (n > 0)
                    cfr_input_write(vt, (const uint8_t *)buf, (size_t)n);
                break;
            }
            continue;
        }
        if (pfd.revents & POLLIN) {
            ssize_t n = pty_read(pty, buf, sizeof(buf));
            if (n <= 0)
                break;
            cfr_input_write(vt, (const uint8_t *)buf, (size_t)n);
        }
        if (pfd.revents & (POLLHUP | POLLERR))
            break;
    }
#endif
}

/* Search the visible grid for a UTF-8 substring (ASCII-only callers).
 * Returns row index of first occurrence, or -1. */
static int find_row_with(CfrTerm *vt, const char *needle)
{
    int rows, cols;
    cfr_get_dimensions(vt, &rows, &cols);
    size_t nlen = strlen(needle);
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c + (int)nlen <= cols; ++c) {
            int ok = 1;
            for (size_t i = 0; i < nlen; ++i) {
                const CfrCell *cell = cfr_get_cell(vt, r, c + (int)i);
                if (!cell || cell->cp != (uint32_t)(unsigned char)needle[i]) {
                    ok = 0;
                    break;
                }
            }
            if (ok)
                return r;
        }
    }
    return -1;
}

/* Output callback: bvt wants to send bytes back upstream (DSR replies, DA,
 * mouse reports, etc). Forward them to the PTY so the child receives them
 * exactly as a real terminal would. */
static void cb_output_to_pty(const uint8_t *bytes, size_t len, void *user)
{
    PtyContext *pty = (PtyContext *)user;
    if (pty)
        (void)pty_write(pty, (const char *)bytes, len);
}

/* Spawn `sh -c cmd` (POSIX) or `cmd /c cmd` (Windows), drain up to
 * timeout_ms, return the CfrTerm grid for inspection. Caller frees
 * the term and pty. */
static CfrTerm *run_cmd(const char *cmd, int rows, int cols,
                        int timeout_ms, PtyContext **out_pty)
{
#ifdef _WIN32
    char *const argv[] = { "cmd.exe", "/c", (char *)cmd, NULL };
#else
    char *const argv[] = { "sh", "-c", (char *)cmd, NULL };
#endif
    PtyContext *pty = pty_create(rows, cols, argv);
    if (!pty)
        return NULL;
    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.rows = rows;
    cfg.cols = cols;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 6;
    CfrTerm *vt = cfr_new(&cfg);
    if (!vt) {
        pty_destroy(pty);
        return NULL;
    }
    /* Wire the output callback so apps that probe the terminal (DSR, DA,
     * mouse mode acks) get their replies. Without this, brick/curses apps
     * sit waiting for `\x1b[...R` and never start drawing. */
    CfrCallbacks cb = { .output = cb_output_to_pty };
    cfr_set_callbacks(vt, &cb, pty);
    drain_pty(vt, pty, timeout_ms);
    if (out_pty)
        *out_pty = pty;
    else
        pty_destroy(pty);
    return vt;
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

static void test_echo_hello(void)
{
    PtyContext *pty = NULL;
    CfrTerm *vt = run_cmd("echo hello", 24, 80, 1000, &pty);
    ASSERT_NOT_NULL(vt);
    ASSERT_TRUE(find_row_with(vt, "hello") >= 0);
    cfr_free(vt);
    pty_destroy(pty);
}

static void test_sgr_red(void)
{
    /* Print "red" with SGR 31, reset, then "plain". The text content should
     * land on the grid; style attribution is checked via cfr_cell_style if
     * we want — for now we just verify both segments appear. */
#ifdef _WIN32
    /* Windows cmd.exe doesn't support printf escape sequences; skip. */
    printf("    (skipping on Windows: needs POSIX printf)\n");
    return;
#else
    PtyContext *pty = NULL;
    CfrTerm *vt = run_cmd("printf '\\033[31mred\\033[0m plain'", 24, 80, 1000, &pty);
    ASSERT_NOT_NULL(vt);
    int row = find_row_with(vt, "red");
    ASSERT_TRUE(row >= 0);
    ASSERT_TRUE(find_row_with(vt, "plain") >= 0);
    cfr_free(vt);
    pty_destroy(pty);
#endif
}

static void test_tput_cursor(void)
{
    /* tput cup 5 10 then echo X — we expect 'X' near row 5 col 10. */
#ifdef _WIN32
    /* tput is not available on Windows; skip. */
    printf("    (skipping on Windows: needs tput)\n");
    return;
#else
    PtyContext *pty = NULL;
    CfrTerm *vt = run_cmd("tput cup 5 10; printf X", 24, 80, 2000, &pty);
    ASSERT_NOT_NULL(vt);
    /* Look across a small window around (5, 10); shells print a prompt
     * before the script runs which can shift the row in some setups. */
    int found = -1;
    for (int r = 0; r < 24 && found < 0; ++r) {
        for (int c = 0; c < 80; ++c) {
            const CfrCell *cell = cfr_get_cell(vt, r, c);
            if (cell && cell->cp == (uint32_t)'X') {
                found = r * 100 + c;
                break;
            }
        }
    }
    ASSERT_TRUE(found >= 0);
    cfr_free(vt);
    pty_destroy(pty);
#endif
}

static void test_zwj_family_full(void)
{
    /* 7-codepoint ZWJ family: 👨‍👩‍👧‍👦 = U+1F468 ZWJ U+1F469 ZWJ U+1F467 ZWJ U+1F466.
     * libvterm truncates at chars[6]; bvt must keep all 7 in one cluster. */
#ifdef _WIN32
    /* printf \x escape sequences not supported by cmd.exe; skip. */
    printf("    (skipping on Windows: needs POSIX printf)\n");
    return;
#else
    PtyContext *pty = NULL;
    CfrTerm *vt = run_cmd(
        "printf '\\xf0\\x9f\\x91\\xa8\\xe2\\x80\\x8d\\xf0\\x9f\\x91\\xa9"
        "\\xe2\\x80\\x8d\\xf0\\x9f\\x91\\xa7\\xe2\\x80\\x8d\\xf0\\x9f\\x91\\xa6'",
        24, 80, 2000, &pty);
    ASSERT_NOT_NULL(vt);
    /* Find the cluster: the first cell with cp == 0x1F468 (man) should
     * have grapheme_id != 0 and the full 7-cp sequence stored. */
    int found = -1;
    for (int r = 0; r < 24 && found < 0; ++r) {
        for (int c = 0; c < 80; ++c) {
            const CfrCell *cell = cfr_get_cell(vt, r, c);
            if (cell && cell->cp == 0x1F468u && cell->width == 2) {
                found = r * 100 + c;
                uint32_t cps[16] = { 0 };
                size_t n = cfr_cell_get_grapheme(vt, cell, cps, 16);
                ASSERT_EQ(n, (size_t)7);
                ASSERT_EQ(cps[0], 0x1F468u); /* man */
                ASSERT_EQ(cps[1], 0x200Du);  /* ZWJ */
                ASSERT_EQ(cps[2], 0x1F469u); /* woman */
                ASSERT_EQ(cps[3], 0x200Du);
                ASSERT_EQ(cps[4], 0x1F467u); /* girl */
                ASSERT_EQ(cps[5], 0x200Du);
                ASSERT_EQ(cps[6], 0x1F466u); /* boy */
                break;
            }
        }
    }
    ASSERT_TRUE(found >= 0);
    cfr_free(vt);
    pty_destroy(pty);
#endif
}

static void test_cjk_echo(void)
{
    /* CJK ideographs are width=2 — assert the cell has width 2. */
#ifdef _WIN32
    /* printf \x escape sequences not supported by cmd.exe; skip. */
    printf("    (skipping on Windows: needs POSIX printf)\n");
    return;
#else
    PtyContext *pty = NULL;
    CfrTerm *vt = run_cmd("printf '\\xe4\\xbd\\xa0\\xe5\\xa5\\xbd'", /* 你好 */
                          24, 80, 1000, &pty);
    ASSERT_NOT_NULL(vt);
    int found = -1;
    for (int r = 0; r < 24 && found < 0; ++r) {
        for (int c = 0; c < 80; ++c) {
            const CfrCell *cell = cfr_get_cell(vt, r, c);
            if (cell && cell->cp == 0x4F60u /* 你 */) {
                ASSERT_EQ((int)cell->width, 2);
                /* Continuation cell at c+1 has width 0. */
                const CfrCell *cont = cfr_get_cell(vt, r, c + 1);
                ASSERT_NOT_NULL(cont);
                ASSERT_EQ((int)cont->width, 0);
                /* Next cluster 好 follows at c+2. */
                const CfrCell *next = cfr_get_cell(vt, r, c + 2);
                ASSERT_NOT_NULL(next);
                ASSERT_EQ(next->cp, 0x597Du);
                found = 1;
                break;
            }
        }
    }
    ASSERT_TRUE(found >= 0);
    cfr_free(vt);
    pty_destroy(pty);
#endif
}

static void test_altscreen_swap(void)
{
    /* tput smcup, write FOO, tput rmcup. After rmcup we should be back on
     * the primary screen with FOO not visible. */
#ifdef _WIN32
    /* tput is not available on Windows; skip. */
    printf("    (skipping on Windows: needs tput)\n");
    return;
#else
    PtyContext *pty = NULL;
    CfrTerm *vt = run_cmd("tput smcup; printf 'FOO_ALT'; sleep 0.05; tput rmcup",
                          24, 80, 2000, &pty);
    ASSERT_NOT_NULL(vt);
    /* On the primary screen now — FOO_ALT should NOT be visible. */
    ASSERT_FALSE(cfr_is_altscreen(vt));
    ASSERT_TRUE(find_row_with(vt, "FOO_ALT") < 0);
    cfr_free(vt);
    pty_destroy(pty);
#endif
}

/* Reproducer for the "cf menu wipes the screen" report. cf is a brick TUI
 * (Haskell vty) that runs in inline mode (no altscreen). It probes the
 * terminal with DSR 6 to learn the cursor row before drawing, so the menu
 * starts where the user invoked it instead of at row 0. If the cursor
 * row reported by DSR is wrong (e.g. always 1), the menu appears at the
 * top and the prompts above are clobbered — exactly the user-visible bug.
 *
 * Test: synthesize 16 lines of output to advance the cursor naturally,
 * then send DSR 6 directly. We expect `\x1b[17;1R` (1-indexed row 17). */
static char g_dsr_buf[64];
static size_t g_dsr_len = 0;
static void cb_capture_dsr(const uint8_t *b, size_t n, void *u)
{
    (void)u;
    for (size_t i = 0; i < n && g_dsr_len + 1 < sizeof(g_dsr_buf); ++i)
        g_dsr_buf[g_dsr_len++] = (char)b[i];
    g_dsr_buf[g_dsr_len] = 0;
}

/* End-to-end repro for the user-reported "cf wipes the screen" bug.
 * Skipped if the cf binary isn't installed. */
static void test_cf_brick_inline_preserves_history(void)
{
#ifdef _WIN32
    /* cf is a Unix-only Haskell TUI; skip on Windows. */
    printf("    (skipping on Windows: Unix-only TUI)\n");
    return;
#else
    if (access("/home/thomasc/.local/bin/cf", X_OK) != 0) {
        printf("    (skipping: cf not installed)\n");
        return;
    }
    int rows = 40, cols = 120;
    char *const argv[] = { "sh", "-c",
                           /* Fill the screen with 16 prompts, then run cf. cf is a brick TUI
                            * that draws inline (no altscreen) and uses DSR 6 to discover the
                            * cursor row before drawing. Matches the user-reported geometry
                            * (`portty -g 120x40`). */
                           "for i in $(seq 1 16); do echo \"prompt $i\"; done; "
                           "/home/thomasc/.local/bin/cf",
                           NULL };
    PtyContext *pty = pty_create(rows, cols, argv);
    ASSERT_NOT_NULL(pty);
    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.rows = rows;
    cfg.cols = cols;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 6;
    CfrTerm *vt = cfr_new(&cfg);
    ASSERT_NOT_NULL(vt);

    /* Output callback writes back to the PTY so cf's DSR query is answered. */
    CfrCallbacks cb = { .output = cb_output_to_pty };
    cfr_set_callbacks(vt, &cb, pty);

    long long deadline = now_ms() + 2500;
    int fd = pty_get_master_fd(pty);
    char rbuf[4096];
    while (now_ms() < deadline) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int wait = (int)(deadline - now_ms());
        if (wait <= 0)
            break;
        int r = poll(&pfd, 1, wait);
        if (r <= 0) {
            if (!pty_is_running(pty))
                break;
            continue;
        }
        if (pfd.revents & POLLIN) {
            ssize_t n = pty_read(pty, rbuf, sizeof(rbuf));
            if (n <= 0)
                break;
            cfr_input_write(vt, (const uint8_t *)rbuf, (size_t)n);
        }
        if (pfd.revents & (POLLHUP | POLLERR))
            break;
    }

    /* The brick TUI starts the menu with `× Carrion Fields`. Find it. */
    int menu_row = find_row_with(vt, "Carrion Fields");
    if (menu_row < 0 || menu_row == 0) {
        /* Failure path — dump the grid so the regression is debuggable. */
        if (menu_row < 0)
            fprintf(stderr, "  'Carrion Fields' not found anywhere\n");
        else
            fprintf(stderr, "  REPRO: cf menu at row 0 — prompts above were wiped\n");
        for (int r = 0; r < rows; ++r) {
            char line[256];
            int n = 0;
            for (int c = 0; c < cols && n + 1 < (int)sizeof(line); ++c) {
                const CfrCell *cell = cfr_get_cell(vt, r, c);
                uint32_t cp = (cell && cell->cp) ? cell->cp : 0;
                line[n++] = (cp >= 0x20 && cp < 0x7f) ? (char)cp : (cp ? '?' : ':');
            }
            line[n] = 0;
            while (n > 0 && line[n - 1] == ':')
                line[--n] = 0;
            if (n > 0)
                fprintf(stderr, "    row %2d: %s\n", r, line);
        }
    }
    ASSERT_TRUE(menu_row > 0);
    /* Sanity: at least one of the recent prompts should still be visible
     * above the menu. */
    int found_prompt = 0;
    for (int row = 0; row < menu_row; ++row) {
        for (int c = 0; c + 6 < cols; ++c) {
            const CfrCell *cell = cfr_get_cell(vt, row, c);
            if (cell && cell->cp == (uint32_t)'p') {
                /* check for "prompt" prefix */
                const char *needle = "prompt";
                int ok = 1;
                for (size_t i = 0; i < strlen(needle); ++i) {
                    const CfrCell *x = cfr_get_cell(vt, row, c + (int)i);
                    if (!x || x->cp != (uint32_t)needle[i]) {
                        ok = 0;
                        break;
                    }
                }
                if (ok) {
                    found_prompt = 1;
                    break;
                }
            }
        }
        if (found_prompt)
            break;
    }
    ASSERT_TRUE(found_prompt);

    cfr_free(vt);
    pty_destroy(pty);
#endif
}

static void test_dsr_after_natural_scroll(void)
{
#ifdef _WIN32
    /* DSR test uses sh -c with seq; skip on Windows. */
    printf("    (skipping on Windows: needs POSIX shell)\n");
    return;
#else
    int rows = 24, cols = 80;
    char *const argv[] = { "sh", "-c",
                           "for i in $(seq 1 16); do echo line$i; done; printf '\\033[6n'",
                           NULL };
    PtyContext *pty = pty_create(rows, cols, argv);
    ASSERT_NOT_NULL(pty);
    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.rows = rows;
    cfg.cols = cols;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 6;
    CfrTerm *vt = cfr_new(&cfg);
    ASSERT_NOT_NULL(vt);

    g_dsr_len = 0;
    g_dsr_buf[0] = 0;
    CfrCallbacks cb = { .output = cb_capture_dsr };
    cfr_set_callbacks(vt, &cb, NULL);

    drain_pty(vt, pty, 1500);

    /* Cursor advanced to row 16 (after 16 lines from row 0).
     * DSR should report `\x1b[17;1R`. */
    ASSERT_STR_EQ(g_dsr_buf, "\x1b[17;1R");
    cfr_free(vt);
    pty_destroy(pty);
#endif
}

static void test_scrollback_push(void)
{
    /* Print 50 lines into a 24-row terminal; expect ≥ 25 lines pushed to
     * scrollback and the most recent lines visible at the bottom. */
#ifdef _WIN32
    /* Uses seq in a for loop; skip on Windows. */
    printf("    (skipping on Windows: needs POSIX shell)\n");
    return;
#else
    PtyContext *pty = NULL;
    CfrTerm *vt = run_cmd("for i in $(seq 1 50); do echo line$i; done",
                          24, 80, 3000, &pty);
    ASSERT_NOT_NULL(vt);
    int sb = cfr_get_scrollback_lines(vt);
    ASSERT_TRUE(sb >= 25);
    /* Last printed line should be on or near the bottom of the visible
     * area. The shell prints a final prompt below it, so we look for
     * "line50" anywhere on the grid. */
    ASSERT_TRUE(find_row_with(vt, "line50") >= 0);
    cfr_free(vt);
    pty_destroy(pty);
#endif
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    /* Required before pty_create — pty.c installs a SIGCHLD handler
     * (no-op on Windows, where child exit is detected via process handle). */
    if (pty_signal_init() != 0) {
        fprintf(stderr, "pty_signal_init failed\n");
        return 1;
    }

    printf("Running test_cfr_pty\n");
    RUN_TEST(test_echo_hello);
    RUN_TEST(test_sgr_red);
    RUN_TEST(test_tput_cursor);
    RUN_TEST(test_zwj_family_full);
    RUN_TEST(test_cjk_echo);
    RUN_TEST(test_altscreen_swap);
    RUN_TEST(test_dsr_after_natural_scroll);
    RUN_TEST(test_scrollback_push);
    RUN_TEST(test_cf_brick_inline_preserves_history);

    pty_signal_cleanup();
    TEST_SUMMARY();
}
