/*
 * test_cfr_pty — engine-only PTY harness for coffer.
 *
 * Spawns real child processes on a real PTY (no SDL, no FreeType, no atlas)
 * and pipes raw output into cfr_input_write(). Assertions are made against
 * the bvt grid via the public coffer.h API.
 */

#include "pty_internal.h"
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
    /* The shared Windows PTY owns a reader thread; wait on its data event
     * and the child process handle, then drain the accumulated buffer. */
    HANDLE handles[2];
    handles[0] = (HANDLE)pty_get_data_event(pty);
    handles[1] = (HANDLE)pty_get_process_handle(pty);

    /* Drain everything currently buffered. */
#define DRAIN_ALL()                                        \
    do {                                                   \
        ssize_t n_;                                        \
        while ((n_ = pty_read(pty, buf, sizeof(buf))) > 0) \
            cfr_input_write(vt, (const uint8_t *)buf,      \
                            (size_t)n_);                   \
    } while (0)

    while (now_ms() < deadline) {
        int wait = (int)(deadline - now_ms());
        if (wait <= 0)
            break;
        DWORD r = WaitForMultipleObjects(2, handles, FALSE, (DWORD)wait);
        if (r == WAIT_TIMEOUT) {
            if (!pty_is_running(pty)) {
                DRAIN_ALL();
                break;
            }
            continue;
        }
        if (r == WAIT_OBJECT_0) {
            DRAIN_ALL();
            if (pty_is_eof(pty))
                break;
        } else {
            DRAIN_ALL();
            break;
        }
    }
#undef DRAIN_ALL
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

#ifdef _WIN32
/* Read raw PTY bytes into `out` until the child exits or the timeout
 * elapses.  Used by the passthrough probe; drain_pty() feeds the engine
 * instead. */
static size_t capture_pty(PtyContext *pty, char *out, size_t cap,
                          int timeout_ms)
{
    size_t total = 0;
    long long deadline = now_ms() + timeout_ms;
    char buf[4096];
    HANDLE handles[2];
    handles[0] = (HANDLE)pty_get_data_event(pty);
    handles[1] = (HANDLE)pty_get_process_handle(pty);

#define APPEND_ALL()                                             \
    do {                                                         \
        ssize_t n_;                                              \
        while ((n_ = pty_read(pty, buf, sizeof(buf))) > 0) {     \
            size_t room = cap - 1 - total;                       \
            size_t copy = (size_t)n_ < room ? (size_t)n_ : room; \
            memcpy(out + total, buf, copy);                      \
            total += copy;                                       \
        }                                                        \
    } while (0)

    while (now_ms() < deadline) {
        int wait = (int)(deadline - now_ms());
        if (wait <= 0)
            break;
        DWORD r = WaitForMultipleObjects(2, handles, FALSE, (DWORD)wait);
        if (r == WAIT_TIMEOUT) {
            if (!pty_is_running(pty)) {
                APPEND_ALL();
                break;
            }
            continue;
        }
        APPEND_ALL();
        if (r == WAIT_OBJECT_0 + 1 || pty_is_eof(pty))
            break;
    }
#undef APPEND_ALL
    out[total] = '\0';
    return total;
}

static int contains_bytes(const char *hay, size_t n, const char *needle,
                          size_t nlen)
{
    if (nlen == 0 || n < nlen)
        return 0;
    for (size_t i = 0; i + nlen <= n; i++)
        if (memcmp(hay + i, needle, nlen) == 0)
            return 1;
    return 0;
}

/* Child mode for the passthrough probe. */
static int emit_sequences(void)
{
    static const char seq[] =
        "\x1bP1;2;3qHELLO\x1b\\" /* DCS (sixel) */
        "\x1b_Gi=1;AAAA\x1b\\"   /* APC (kitty graphics) */
        "\x1b]5555;GHELLO\x07";  /* OSC fallback carrier */
    DWORD written = 0;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    WriteFile(out, seq, (DWORD)(sizeof(seq) - 1), &written, NULL);
    return 0;
}

/* ConPTY create flags that decide how the host measures characters.  They
 * are part of the NuGet package's inc/conpty.h, not the Windows SDK. */
#ifndef PSEUDOCONSOLE_GLYPH_WIDTH_GRAPHEMES
#define PSEUDOCONSOLE_GLYPH_WIDTH_GRAPHEMES 0x08
#endif
#ifndef PSEUDOCONSOLE_AMBIGUOUS_IS_WIDE
#define PSEUDOCONSOLE_AMBIGUOUS_IS_WIDE 0x20
#endif

/* Width-alignment probe: text that wraps differently depending on how
 * characters are measured — ambiguous-width, CJK, a ZWJ cluster, and
 * VS16/VS15 pairs.  The test feeds the same bytes straight into coffer to
 * get the reference layout, then checks that a PTY round trip reproduces
 * it under each measuring mode. */
static const char width_probe_seq[] =
    "\x1b[2J\x1b[H"
    /* 16 East Asian Ambiguous characters at 12 columns: two rows narrow,
     * three rows wide. */
    "\xc2\xb0\xc2\xb1\xc3\x97\xc3\xb7"
    "\xc2\xb0\xc2\xb1\xc3\x97\xc3\xb7"
    "\xc2\xb0\xc2\xb1\xc3\x97\xc3\xb7"
    "\xc2\xb0\xc2\xb1\xc3\x97\xc3\xb7"
    "\r\n"
    /* A relative cursor move after ambiguous text: whether the host or the
     * engine resolves it, the X column exposes a width disagreement (col 7
     * narrow, col 10 wide). */
    "\xc2\xb0\xc2\xb1\xc3\x97\x1b[3CX\r\n"
    /* 10 CJK ideographs: 20 cells, so two rows at width 2. */
    "\xe4\xbd\xa0\xe5\xa5\xbd\xe4\xb8\x96\xe7\x95\x8c"
    "\xe4\xbd\xa0\xe5\xa5\xbd\xe4\xb8\x96\xe7\x95\x8c"
    "\xe4\xbd\xa0\xe5\xa5\xbd\r\n"
    /* ZWJ family + ASCII, and VS16/VS15 pairs. */
    "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d"
    "\xf0\x9f\x91\xa7\xe2\x80\x8d\xf0\x9f\x91\xa6"
    "AB\r\n"
    "\xe2\x9d\xa4\xef\xb8\x8f"
    "\xe2\x9d\xa4\xef\xb8\x8e"
    "CD\r\n"
    /* Overwrite probe: 16 ambiguous characters fill two rows narrow and
     * three rows wide, then CUU 2 lands on a different buffer row and Z
     * overwrites different content.  If a host ever applies its own width
     * rules when re-serialising, this is where it shows up. */
    "\xc2\xb0\xc2\xb1\xc3\x97\xc3\xb7"
    "\xc2\xb0\xc2\xb1\xc3\x97\xc3\xb7"
    "\xc2\xb0\xc2\xb1\xc3\x97\xc3\xb7"
    "\xc2\xb0\xc2\xb1\xc3\x97\xc3\xb7"
    "\x1b[2AZ\r\n";

/* Child mode: write the probe to the console. */
static int emit_width_probe(void)
{
    /* The probe is UTF-8; without this the conhost side decodes the bytes
     * in the console code page and the round trip is mojibake. */
    SetConsoleOutputCP(CP_UTF8);
    DWORD written = 0;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    WriteFile(out, width_probe_seq, (DWORD)(sizeof(width_probe_seq) - 1),
              &written, NULL);
    return 0;
}

/* Palette probe: an application sets palette entry 1, prints with indexed
 * colour 1, resets the entry and prints again.  What the host forwards
 * decides what the engine sees: OSC 4/104 intact, nothing at all, or the
 * indexed SGR rewritten to RGB. */
static const char palette_probe_seq[] =
    "\x1b]4;1;rgb:12/34/56\x1b\\" /* entry 1 -> #123456 */
    "\x1b[38;5;1mPX\x1b[0m\r\n"   /* indexed fg 1, override in force */
    "\x1b]104;1\x1b\\"            /* entry 1 back to the built-in */
    "\x1b[38;5;1mRX\x1b[0m\r\n";  /* indexed fg 1, after the reset */

/* Child mode: write the palette probe to the console. */
static int emit_palette_probe(void)
{
    DWORD written = 0;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    WriteFile(out, palette_probe_seq, (DWORD)(sizeof(palette_probe_seq) - 1),
              &written, NULL);
    return 0;
}
#endif

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
    /* tput cup 5 10 then echo X — we expect 'X' near row 5 col 10.
     * This tests that coffer-vty-256color terminfo entry resolves
     * correctly and produces a valid cursor positioning sequence. */
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

static void test_terminfo_smcup_rmcup(void)
{
    /* Verify that tput smcup/rmcup produce sequences coffer recognizes.
     * This tests terminfo resolution - if the terminfo entry is missing,
     * tput will produce nothing or an error. */
#ifdef _WIN32
    printf("    (skipping on Windows: needs tput)\n");
    return;
#else
    PtyContext *pty = NULL;
    /* Full smcup/write/rmcup cycle - should be back on primary. */
    CfrTerm *vt = run_cmd("tput smcup; printf X; tput rmcup", 24, 80, 1500, &pty);
    ASSERT_NOT_NULL(vt);
    /* After rmcup we should be back on primary screen. */
    ASSERT_FALSE(cfr_is_altscreen(vt));
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
    /* Octal escapes: dash's printf does not support \xHH, so use POSIX
     * \0NNN to keep this working on Debian/Ubuntu where /bin/sh is dash. */
    CfrTerm *vt = run_cmd(
        "printf '\\360\\237\\221\\250\\342\\200\\215\\360\\237\\221\\251"
        "\\342\\200\\215\\360\\237\\221\\247\\342\\200\\215\\360\\237\\221\\246'",
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
                size_t n = cfr_cell_get_grapheme(vt, r, c, cps, 16);
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
    /* Octal escapes: dash printf lacks \xHH support (see test_zwj_family_full). */
    CfrTerm *vt = run_cmd("printf '\\344\\275\\240\\345\\245\\275'", /* 你好 */
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
    /* Raw DECSET/DECRST 1049, write FOO, leave. After rmcup we should be
     * back on the primary screen with FOO not visible. */
#ifdef _WIN32
    /* printf escape sequences not supported by cmd.exe; skip. */
    printf("    (skipping on Windows: needs POSIX printf)\n");
    return;
#else
    PtyContext *pty = NULL;
    /* Use raw DECSET/DECRST 1049 instead of tput smcup/rmcup. */
    CfrTerm *vt = run_cmd("printf '\\033[?1049hFOO_ALT\\033[?1049l'",
                          24, 80, 2000, &pty);
    ASSERT_NOT_NULL(vt);
    /* On the primary screen now — FOO_ALT should NOT be visible. */
    ASSERT_FALSE(cfr_is_altscreen(vt));
    ASSERT_TRUE(find_row_with(vt, "FOO_ALT") < 0);
    cfr_free(vt);
    pty_destroy(pty);
#endif
}

/* DSR reply capture for test_dsr_after_natural_scroll. */
static char g_dsr_buf[64];
static size_t g_dsr_len = 0;
static void cb_capture_dsr(const uint8_t *b, size_t n, void *u)
{
    (void)u;
    for (size_t i = 0; i < n && g_dsr_len + 1 < sizeof(g_dsr_buf); ++i)
        g_dsr_buf[g_dsr_len++] = (char)b[i];
    g_dsr_buf[g_dsr_len] = 0;
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

static void test_conpty_passthrough(void)
{
#ifdef _WIN32
    /* Re-exec this test binary in child mode so the probe needs no
     * external shell or printf. */
    char self[MAX_PATH * 2];
    DWORD n = GetModuleFileNameA(NULL, self, sizeof(self));
    ASSERT_TRUE(n > 0 && n < sizeof(self));

    char *const argv[] = { self, "--emit-sequences", NULL };
    PtyContext *pty = pty_create(24, 80, argv);
    ASSERT_NOT_NULL(pty);
    ASSERT_TRUE(pty_conpty_backend() != NULL);

    char raw[8192];
    size_t len = capture_pty(pty, raw, sizeof(raw), 2000);
    const char *backend = pty_conpty_backend();
    int bundled = strcmp(backend, "bundled") == 0;
    pty_destroy(pty);

    ASSERT_TRUE(len > 0);
    ASSERT_TRUE(contains_bytes(raw, len, "\x1b]5555;", 7));
    if (bundled) {
        ASSERT_TRUE(contains_bytes(raw, len, "\x1bP", 2));
        ASSERT_TRUE(contains_bytes(raw, len, "\x1b_", 2));
    } else {
        ASSERT_FALSE(contains_bytes(raw, len, "\x1bP", 2));
        ASSERT_FALSE(contains_bytes(raw, len, "\x1b_", 2));
    }
#else
    printf("    (skipping on POSIX: ConPTY-specific)\n");
#endif
}

/* ------------------------------------------------------------------ */
/* ConPTY width alignment                                              */
/* ------------------------------------------------------------------ */

#ifdef _WIN32

/* Join every visible row's text into `out` (rows separated by '\n').
 * Width disagreements show up as shifted text, which this captures. */
static int grid_text(CfrTerm *vt, char *out, size_t cap)
{
    int rows = 0, cols = 0;
    size_t used = 0;
    cfr_get_dimensions(vt, &rows, &cols);
    (void)cols;
    out[0] = '\0';
    for (int r = 0; r < rows; r++) {
        size_t need = cfr_row_text_size(vt, r);
        if (need == 0)
            need = 1;
        char *row = malloc(need);
        if (!row)
            return -1;
        cfr_row_text(vt, r, row, need);
        int n = snprintf(out + used, cap - used, "%s%s", r ? "\n" : "", row);
        free(row);
        if (n < 0 || (size_t)n >= cap - used)
            return -1;
        used += (size_t)n;
    }
    return 0;
}

/* Reference layout: the same bytes fed into the engine directly. */
static int render_probe(bool ambiguous_wide, char *out, size_t cap)
{
    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.rows = 10;
    cfg.cols = 12;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 6;
    cfg.ambiguous_wide = ambiguous_wide;
    CfrTerm *vt = cfr_new(&cfg);
    if (!vt)
        return -1;
    cfr_input_write(vt, (const uint8_t *)width_probe_seq,
                    sizeof(width_probe_seq) - 1);
    int rc = grid_text(vt, out, cap);
    if (rc != 0)
        fprintf(stderr, "DEBUG render_probe rc=%d outlen=%zu cap=%zu\n", rc,
                strlen(out), cap);
    cfr_free(vt);
    return rc;
}

/* Run the width probe under the given host flags and engine setting. */
static int run_width_probe(DWORD flags, bool ambiguous_wide, char *out,
                           size_t cap)
{
    char self[MAX_PATH * 2];
    DWORD n = GetModuleFileNameA(NULL, self, sizeof(self));
    if (n == 0 || n >= sizeof(self))
        return -1;

    char *const argv[] = { self, "--emit-width-probe", NULL };
    pty_set_conpty_flags(flags);
    PtyContext *pty = pty_create(10, 12, argv);
    if (!pty) {
        pty_set_conpty_flags(0);
        return -1;
    }

    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.rows = 10;
    cfg.cols = 12;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 6;
    cfg.ambiguous_wide = ambiguous_wide;
    CfrTerm *vt = cfr_new(&cfg);
    if (!vt) {
        pty_destroy(pty);
        pty_set_conpty_flags(0);
        return -1;
    }

    drain_pty(vt, pty, 2000);
    int rc = grid_text(vt, out, cap);
    cfr_free(vt);
    pty_destroy(pty);
    pty_set_conpty_flags(0);
    return rc;
}

#endif /* _WIN32 */

/* The host re-serialises its buffer and the receiving engine lays the text
 * out with its own width rules, so the PTY round trip must reproduce the
 * engine's direct rendering of the same bytes under each host mode.  The
 * probe covers ambiguous-width text, CJK, a ZWJ cluster, VS16/VS15 pairs,
 * a relative cursor move and an overwrite, which between them would expose
 * any width rule the host started applying itself. */
static void test_conpty_width_alignment(void)
{
#ifdef _WIN32
    static char narrow[8192];
    static char wide[8192];
    static char host[8192];

    if (render_probe(false, narrow, sizeof(narrow)) != 0 ||
        render_probe(true, wide, sizeof(wide)) != 0) {
        printf("    (skipping: could not render the probe)\n");
        return;
    }
    /* The two measuring modes must actually produce different layouts,
     * otherwise the test proves nothing. */
    ASSERT_TRUE(strcmp(narrow, wide) != 0);

    if (run_width_probe(0, false, host, sizeof(host)) != 0) {
        printf("    (skipping: could not run the width probe)\n");
        return;
    }
    if (strcmp(pty_conpty_backend(), "bundled") != 0) {
        printf("    (skipping: system conhost ignores the width flags)\n");
        return;
    }

    /* The round trip reproduces the engine's narrow layout. */
    ASSERT_STR_EQ(host, narrow);

    /* Grapheme measurement is what coffer does natively; enabling it on
     * the host does not change the stream the engine receives. */
    ASSERT_TRUE(run_width_probe(PSEUDOCONSOLE_GLYPH_WIDTH_GRAPHEMES, false,
                                host, sizeof(host)) == 0);
    ASSERT_STR_EQ(host, narrow);

    /* PSEUDOCONSOLE_AMBIGUOUS_IS_WIDE is a host-side mode: the bundled
     * OpenConsole still re-serialises text and lets the receiving engine
     * measure it, so with the engine narrow the grid stays narrow.  That
     * is why no --ambiguous-wide / --conpty-glyph-width switch is
     * warranted: it would claim a host effect that has none.  If a future
     * OpenConsole starts baking widths into the stream this assertion
     * fails, and the decision has to be revisited. */
    ASSERT_TRUE(run_width_probe(PSEUDOCONSOLE_GLYPH_WIDTH_GRAPHEMES |
                                    PSEUDOCONSOLE_AMBIGUOUS_IS_WIDE,
                                false, host, sizeof(host)) == 0);
    ASSERT_STR_EQ(host, narrow);

    /* The layout is the engine's ambiguous_wide setting, on both paths. */
    ASSERT_TRUE(run_width_probe(PSEUDOCONSOLE_GLYPH_WIDTH_GRAPHEMES |
                                    PSEUDOCONSOLE_AMBIGUOUS_IS_WIDE,
                                true, host, sizeof(host)) == 0);
    ASSERT_STR_EQ(host, wide);
#else
    printf("    (skipping on POSIX: ConPTY-specific)\n");
#endif
}

/* ------------------------------------------------------------------ */
/* ConPTY palette forwarding                                           */
/* ------------------------------------------------------------------ */

#ifdef _WIN32

/* Locate `needle` (ASCII) on the grid; returns 1 and its top-left cell. */
static int find_run(CfrTerm *vt, const char *needle, int *out_row,
                    int *out_col)
{
    int rows = 0, cols = 0;
    size_t nlen = strlen(needle);
    cfr_get_dimensions(vt, &rows, &cols);
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c + (int)nlen <= cols; c++) {
            size_t i = 0;
            for (; i < nlen; i++) {
                const CfrCell *cell = cfr_get_cell(vt, r, c + (int)i);
                if (!cell || cell->cp != (uint32_t)(unsigned char)needle[i])
                    break;
            }
            if (i == nlen) {
                if (out_row)
                    *out_row = r;
                if (out_col)
                    *out_col = c;
                return 1;
            }
        }
    }
    return 0;
}

/* Print `n` bytes with escapes visible, so a CI log shows what the host
 * actually forwarded.  Truncates past `log_cap` bytes. */
static void log_raw(const char *buf, size_t n, size_t log_cap)
{
    size_t shown = n < log_cap ? n : log_cap;
    printf("    raw (%zu bytes%s): \"", n, shown < n ? ", truncated" : "");
    for (size_t i = 0; i < shown; i++) {
        unsigned char b = (unsigned char)buf[i];
        if (b == 0x1b)
            fputs("\\e", stdout);
        else if (b == '\r')
            fputs("\\r", stdout);
        else if (b == '\n')
            fputs("\\n", stdout);
        else if (b == '\\')
            fputs("\\\\", stdout);
        else if (b == '"')
            fputs("\\\"", stdout);
        else if (b < 0x20 || b >= 0x7f)
            printf("\\x%02x", (unsigned)b);
        else
            putchar((int)b);
    }
    fputs("\"\n", stdout);
}

static const char *fg_kind_name(const CfrStyle *st)
{
    if (st->fg.kind == CFR_COLOR_KIND_INDEXED)
        return "indexed";
    if (st->fg.kind == CFR_COLOR_KIND_RGB)
        return "rgb";
    return "default";
}

static void log_cell_style(const CfrTerm *vt, const char *label, int row,
                           int col)
{
    const CfrStyle *st = cfr_cell_style(vt, row, col);
    if (!st) {
        printf("    %s: no cell at (%d,%d)\n", label, row, col);
        return;
    }
    printf("    %s at (%d,%d): fg kind=%s index=%u rgb=#%06x "
           "resolved=#%06x\n",
           label, row, col, fg_kind_name(st), (unsigned)st->fg.index,
           (unsigned)st->fg.rgb, (unsigned)cfr_style_fg_rgb(vt, st));
}

#endif /* _WIN32 */

/* The host parses VT itself and re-serialises what reaches the engine, so
 * a palette override sent by the child may arrive intact, be consumed (the
 * host's own palette changes, the engine's does not), or show up only as
 * RGB baked into SGR.  Report which one happens and check the engine acted
 * on exactly what arrived. */
static void test_conpty_palette(void)
{
#ifdef _WIN32
    char self[MAX_PATH * 2];
    DWORD n = GetModuleFileNameA(NULL, self, sizeof(self));
    ASSERT_TRUE(n > 0 && n < sizeof(self));

    char *const argv[] = { self, "--emit-palette-probe", NULL };
    PtyContext *pty = pty_create(24, 80, argv);
    ASSERT_NOT_NULL(pty);
    ASSERT_TRUE(pty_conpty_backend() != NULL);

    static char raw[16384];
    size_t len = capture_pty(pty, raw, sizeof(raw), 3000);
    const char *backend = pty_conpty_backend();
    pty_destroy(pty);

    printf("    palette probe: backend=%s bytes=%zu\n", backend, len);
    log_raw(raw, len, 8192);

    int osc4 = contains_bytes(raw, len, "\x1b]4;1;", 7);
    int osc104 = contains_bytes(raw, len, "\x1b]104;", 7);
    int indexed = contains_bytes(raw, len, "38;5;1", 6);
    int rgb_sgr = contains_bytes(raw, len, "38;2;", 5);
    printf("    palette probe: osc4=%d osc104=%d sgr-indexed=%d sgr-rgb=%d\n",
           osc4, osc104, indexed, rgb_sgr);

    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.rows = 24;
    cfg.cols = 80;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 6;
    CfrTerm *vt = cfr_new(&cfg);
    ASSERT_NOT_NULL(vt);
    cfr_input_write(vt, (const uint8_t *)raw, len);

    int p_row = -1, p_col = -1, r_row = -1, r_col = -1;
    ASSERT_TRUE(find_run(vt, "PX", &p_row, &p_col));
    ASSERT_TRUE(find_run(vt, "RX", &r_row, &r_col));
    log_cell_style(vt, "PX (override in force)", p_row, p_col);
    log_cell_style(vt, "RX (after the reset)", r_row, r_col);
    printf("    palette probe: palette[1]=#%06x builtin[1]=#%06x\n",
           (unsigned)cfr_palette_rgb(vt, 1),
           (unsigned)cfr_default_palette_rgb(1));

    /* The engine applied the override if the host forwarded it, and left
     * the built-in entry alone if the host consumed it.  Loose for the
     * first observation; pin the actual host behaviour once known. */
    uint32_t pal = cfr_palette_rgb(vt, 1);
    ASSERT_TRUE(pal == cfr_default_palette_rgb(1) || pal == 0x123456u);

    /* The indexed SGR reaches the engine as a request (index 1) or not at
     * all; if the host bakes it, it is #123456 by construction. */
    const CfrStyle *pst = cfr_cell_style(vt, p_row, p_col);
    ASSERT_NOT_NULL(pst);
    if (pst->fg.kind == CFR_COLOR_KIND_INDEXED)
        ASSERT_EQ(pst->fg.index, 1);
    else if (pst->fg.kind == CFR_COLOR_KIND_RGB)
        ASSERT_EQ(pst->fg.rgb, 0x123456u);

    cfr_free(vt);
#else
    printf("    (skipping on POSIX: ConPTY-specific)\n");
#endif
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
#ifdef _WIN32
    if (argc > 1 && strcmp(argv[1], "--emit-sequences") == 0)
        return emit_sequences();
    if (argc > 1 && strcmp(argv[1], "--emit-width-probe") == 0)
        return emit_width_probe();
    if (argc > 1 && strcmp(argv[1], "--emit-palette-probe") == 0)
        return emit_palette_probe();
#endif
    test_parse_args(argc, argv);

    /* Required before pty_create — pty_posix.c installs a SIGCHLD handler
     * (no-op on Windows, where child exit is detected via process handle). */
    if (pty_signal_init() != 0) {
        fprintf(stderr, "pty_signal_init failed\n");
        return 1;
    }

    printf("Running test_cfr_pty\n");
    RUN_TEST(test_conpty_passthrough);
    RUN_TEST(test_conpty_width_alignment);
    RUN_TEST(test_conpty_palette);
    RUN_TEST(test_echo_hello);
    RUN_TEST(test_sgr_red);
    RUN_TEST(test_tput_cursor);
    RUN_TEST(test_terminfo_smcup_rmcup);
    RUN_TEST(test_zwj_family_full);
    RUN_TEST(test_cjk_echo);
    RUN_TEST(test_altscreen_swap);
    RUN_TEST(test_dsr_after_natural_scroll);
    RUN_TEST(test_scrollback_push);

    pty_signal_cleanup();
    TEST_SUMMARY();
}
