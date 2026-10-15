/* tests/test_script.c — script engine: parser + executor.
 *
 * Covers the parse grammar (escapes, hex, wait-for trailing options,
 * render modes, extension whitelist, parse-time errors) and the
 * executor (step/wait_ms/blocking runner, assert semantics against the
 * unified buffer, await-exit recording, quit, dumps) using a fake
 * host — no PTY; `emit` feeds a real CfrTerm. */

#include "script_internal.h"
#include "test_helpers.h"
#include <coffer/coffer.h>
#include <coffer/script.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static void msleep(unsigned ms)
{
    Sleep(ms);
}
#else
#include <time.h>
static void msleep(unsigned ms)
{
    struct timespec ts = { .tv_sec = ms / 1000,
                           .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
#endif

/* ------------------------------------------------------------------ */
/* Fake host                                                          */
/* ------------------------------------------------------------------ */

typedef struct
{
    CfrTerm *term;
    char sent[4096];
    size_t sent_len;
    char log[8192];
    size_t log_len;
    int exit_ready;
    int exit_code;
    int resized_cols, resized_rows;
    char ext_name[64];
    char ext_args[1024];
    int ext_calls;
    bool ext_ok;
    bool ext_fatal;
} FakeHost;

static void fake_pty_write(void *ud, const char *data, size_t len)
{
    FakeHost *fh = ud;
    if (fh->sent_len + len >= sizeof(fh->sent))
        len = sizeof(fh->sent) - fh->sent_len - 1;
    memcpy(fh->sent + fh->sent_len, data, len);
    fh->sent_len += len;
    fh->sent[fh->sent_len] = '\0';
}

static void fake_emit(void *ud, const char *data, size_t len)
{
    FakeHost *fh = ud;
    cfr_input_write(fh->term, (const uint8_t *)data, len);
}

static int fake_poll_exit(void *ud, int *status)
{
    FakeHost *fh = ud;
    if (!fh->exit_ready)
        return 0;
    *status = fh->exit_code;
    return 1;
}

static int fake_resize(void *ud, int cols, int rows)
{
    FakeHost *fh = ud;
    fh->resized_cols = cols;
    fh->resized_rows = rows;
    cfr_resize(fh->term, rows, cols);
    return 0;
}

static void fake_log(void *ud, int line, const char *msg)
{
    FakeHost *fh = ud;
    int n = snprintf(fh->log + fh->log_len,
                     sizeof(fh->log) - fh->log_len, "[%d] %s\n", line, msg);
    if (n > 0)
        fh->log_len += (size_t)n < sizeof(fh->log) - fh->log_len
                           ? (size_t)n
                           : sizeof(fh->log) - fh->log_len - 1;
}

static bool fake_exec_ext(void *ud, const char *name, const char *args,
                          int line, bool *fatal)
{
    FakeHost *fh = ud;
    (void)line;
    snprintf(fh->ext_name, sizeof(fh->ext_name), "%s", name);
    snprintf(fh->ext_args, sizeof(fh->ext_args), "%s", args);
    fh->ext_calls++;
    *fatal = fh->ext_fatal;
    return fh->ext_ok;
}

static CfrTerm *make_term(int rows, int cols)
{
    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.rows = rows;
    cfg.cols = cols;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 6;
    cfg.scrollback = 100;
    return cfr_new(&cfg);
}

static void host_init(FakeHost *fh, CfrScriptHost *host, CfrTerm *term)
{
    memset(fh, 0, sizeof(*fh));
    fh->term = term;
    fh->ext_ok = true;
    *host = (CfrScriptHost){
        .term = term,
        .pty_write = fake_pty_write,
        .emit = fake_emit,
        .poll_exit = fake_poll_exit,
        .resize = fake_resize,
        .log = fake_log,
        .exec_ext = fake_exec_ext,
        .ud = fh,
    };
}

/* Parse an in-memory script (exercises the internal entry point; the
 * public cfr_script_load path is covered by test_load_missing_file). */
static CfrScript *parse_str(const char *text, const char *const *ext,
                            char *err, size_t errlen)
{
    FILE *f = tmpfile();
    if (!f)
        return NULL;
    fwrite(text, 1, strlen(text), f);
    rewind(f);
    CfrScript *s = script_parse(f, ext, err, errlen);
    fclose(f);
    return s;
}

/* Run a script to completion; waits advance in 1 ms pump slices. */
static CfrScriptStatus run_str(const char *text, const char *const *ext,
                               FakeHost *fh, CfrScriptHost *host)
{
    char err[256];
    CfrScript *s = parse_str(text, ext, err, sizeof(err));
    if (!s)
        return CFR_SCRIPT_FAILED;
    CfrScriptRun *run = cfr_script_run_new(s, host);
    CfrScriptStatus st = CFR_SCRIPT_RUNNING;
    if (run) {
        for (int i = 0; i < 10000 && st == CFR_SCRIPT_RUNNING; i++) {
            st = cfr_script_step(run);
            if (st == CFR_SCRIPT_RUNNING &&
                cfr_script_wait_ms(run) != UINT32_MAX)
                msleep(1);
        }
        cfr_script_run_free(run);
    }
    cfr_script_free(s);
    (void)fh;
    return st;
}

/* ------------------------------------------------------------------ */
/* Parser                                                             */
/* ------------------------------------------------------------------ */

static void test_parse_all_core_commands(void)
{
    char err[256] = { 0 };
    const char *text = "# comment\n"
                       "\n"
                       "wait 0.5\n"
                       "wait-for \"ready\" 5 line-end\n"
                       "send ls\\n\n"
                       "sendln ls\n"
                       "raw 1b 5b 41\n"
                       "emit \\e[2J\n"
                       "emit-raw 1b 63\n"
                       "assert-contains foo\n"
                       "assert-not-contains bar\n"
                       "await-exit 3\n"
                       "assert-exit 0\n"
                       "resize 100 40\n"
                       "render\n"
                       "render all\n"
                       "render cells 0\n"
                       "render cells 0 1 5\n"
                       "render styles 2\n"
                       "cursor\n"
                       "dump-sixel\n"
                       "quit\n";
    CfrScript *s = parse_str(text, NULL, err, sizeof(err));
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(s->count, 20);
    ASSERT_EQ(s->cmds[0].type, CFR_CMD_WAIT);
    ASSERT_TRUE(s->cmds[0].seconds > 0.4 && s->cmds[0].seconds < 0.6);
    ASSERT_EQ(s->cmds[1].type, CFR_CMD_WAIT_FOR);
    ASSERT_STR_EQ(s->cmds[1].text, "ready");
    ASSERT_TRUE(s->cmds[1].seconds == 5.0);
    ASSERT_TRUE(s->cmds[1].line_end);
    ASSERT_EQ(s->cmds[2].type, CFR_CMD_SEND);
    ASSERT_STR_EQ(s->cmds[2].text, "ls\n"); /* \n expanded at parse */
    ASSERT_EQ(s->cmds[4].type, CFR_CMD_RAW);
    ASSERT_EQ(s->cmds[4].bytes_len, 3);
    ASSERT_EQ(s->cmds[4].bytes[0], 0x1b);
    ASSERT_EQ(s->cmds[11].type, CFR_CMD_RESIZE);
    ASSERT_EQ(s->cmds[11].a, 100); /* cols */
    ASSERT_EQ(s->cmds[11].b, 40);  /* rows */
    ASSERT_EQ(s->cmds[13].render_mode, CFR_RENDER_ALL);
    ASSERT_EQ(s->cmds[14].render_mode, CFR_RENDER_CELLS);
    ASSERT_EQ(s->cmds[14].a, 0);
    ASSERT_EQ(s->cmds[14].c, -1); /* whole row */
    ASSERT_EQ(s->cmds[15].render_mode, CFR_RENDER_CELLS);
    ASSERT_EQ(s->cmds[15].b, 1);
    ASSERT_EQ(s->cmds[15].c, 5);
    ASSERT_EQ(s->cmds[16].render_mode, CFR_RENDER_STYLES);
    ASSERT_EQ(s->cmds[16].a, 2);
    ASSERT_EQ(s->cmds[19].type, CFR_CMD_QUIT);
    cfr_script_free(s);
}

static void test_unknown_command_rejected(void)
{
    char err[256] = { 0 };
    CfrScript *s = parse_str("frobnicate now\n", NULL, err, sizeof(err));
    ASSERT_NULL(s);
    ASSERT_TRUE(strstr(err, "line 1: unknown command: frobnicate") != NULL);
}

static void test_ext_name_accepted_at_load(void)
{
    char err[256] = { 0 };
    const char *ext[] = { "screendump", NULL };
    CfrScript *s = parse_str("screendump /tmp/x.png\n", ext, err,
                             sizeof(err));
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(s->count, 1);
    ASSERT_EQ(s->cmds[0].type, CFR_CMD_EXT);
    ASSERT_STR_EQ(s->cmds[0].ext_name, "screendump");
    ASSERT_STR_EQ(s->cmds[0].text, "/tmp/x.png"); /* raw, unexpanded */
    cfr_script_free(s);
}

static void test_ext_unregistered_rejected(void)
{
    char err[256] = { 0 };
    const char *ext[] = { "panel", NULL };
    CfrScript *s = parse_str("screendump /tmp/x.png\n", ext, err,
                             sizeof(err));
    ASSERT_NULL(s);
    ASSERT_TRUE(strstr(err, "unknown command") != NULL);
}

static void test_raw_malformed_rejected(void)
{
    char err[256] = { 0 };
    CfrScript *s = parse_str("raw zz\n", NULL, err, sizeof(err));
    ASSERT_NULL(s);
    ASSERT_TRUE(strstr(err, "invalid hex") != NULL);
}

static void test_render_bad_mode_rejected(void)
{
    char err[256] = { 0 };
    ASSERT_NULL(parse_str("render sideways\n", NULL, err, sizeof(err)));
    ASSERT_NULL(parse_str("render cells\n", NULL, err, sizeof(err)));
    ASSERT_NULL(parse_str("render cells 0 1\n", NULL, err, sizeof(err)));
    ASSERT_NULL(parse_str("render styles\n", NULL, err, sizeof(err)));
}

static void test_load_missing_file(void)
{
    char err[256] = { 0 };
    CfrScript *s = cfr_script_load("/nonexistent/script.txt", err,
                                   sizeof(err), NULL);
    ASSERT_NULL(s);
    ASSERT_TRUE(strstr(err, "cannot open") != NULL);
}

static void test_wait_for_option_order(void)
{
    char err[256] = { 0 };
    /* timeout then line-end */
    CfrScript *s = parse_str("wait-for done 7 line-end\n", NULL, err,
                             sizeof(err));
    ASSERT_NOT_NULL(s);
    ASSERT_TRUE(s->cmds[0].seconds == 7.0);
    ASSERT_TRUE(s->cmds[0].line_end);
    ASSERT_STR_EQ(s->cmds[0].text, "done");
    cfr_script_free(s);

    /* line-end then timeout */
    s = parse_str("wait-for \"two words\" line-end 9\n", NULL, err,
                  sizeof(err));
    ASSERT_NOT_NULL(s);
    ASSERT_TRUE(s->cmds[0].seconds == 9.0);
    ASSERT_TRUE(s->cmds[0].line_end);
    ASSERT_STR_EQ(s->cmds[0].text, "two words");
    cfr_script_free(s);

    /* default timeout */
    s = parse_str("wait-for x\n", NULL, err, sizeof(err));
    ASSERT_NOT_NULL(s);
    ASSERT_TRUE(s->cmds[0].seconds == 600.0);
    cfr_script_free(s);
}

/* ------------------------------------------------------------------ */
/* Executor                                                           */
/* ------------------------------------------------------------------ */

static void test_send_escapes_captured(void)
{
    CfrTerm *vt = make_term(5, 20);
    FakeHost fh;
    CfrScriptHost host;
    host_init(&fh, &host, vt);
    CfrScriptStatus st = run_str("send a\\nb\\x41\\e\\\\\n", NULL, &fh,
                                 &host);
    ASSERT_EQ(st, CFR_SCRIPT_DONE);
    ASSERT_EQ(fh.sent_len, 6);
    ASSERT_TRUE(memcmp(fh.sent, "a\nbA\x1b\\", 6) == 0);
    cfr_free(vt);
}

static void test_sendln_appends_crlf(void)
{
    CfrTerm *vt = make_term(5, 20);
    FakeHost fh;
    CfrScriptHost host;
    host_init(&fh, &host, vt);
    ASSERT_EQ(run_str("sendln ls\n", NULL, &fh, &host), CFR_SCRIPT_DONE);
    ASSERT_STR_EQ(fh.sent, "ls\r\n");
    cfr_free(vt);
}

static void test_raw_nul_bytes_kept(void)
{
    CfrTerm *vt = make_term(5, 20);
    FakeHost fh;
    CfrScriptHost host;
    host_init(&fh, &host, vt);
    ASSERT_EQ(run_str("raw 00 41 1b\n", NULL, &fh, &host),
              CFR_SCRIPT_DONE);
    ASSERT_EQ(fh.sent_len, 3);
    ASSERT_TRUE(fh.sent[0] == 0 && fh.sent[1] == 'A' &&
                fh.sent[2] == 0x1b);
    cfr_free(vt);
}

static void test_emit_feeds_parser(void)
{
    CfrTerm *vt = make_term(5, 20);
    FakeHost fh;
    CfrScriptHost host;
    host_init(&fh, &host, vt);
    CfrScriptStatus st = run_str("emit hello world\n"
                                 "assert-contains \"hello world\"\n",
                                 NULL, &fh, &host);
    ASSERT_EQ(st, CFR_SCRIPT_DONE);
    int row = -1;
    ASSERT_TRUE(cfr_find_text(vt, "hello world", false, &row));
    ASSERT_EQ(row, 0);
    cfr_free(vt);
}

static void test_wait_for_found(void)
{
    CfrTerm *vt = make_term(5, 20);
    FakeHost fh;
    CfrScriptHost host;
    host_init(&fh, &host, vt);
    ASSERT_EQ(run_str("emit ready>\nwait-for \"ready>\" 5\n", NULL, &fh,
                      &host),
              CFR_SCRIPT_DONE);
    ASSERT_TRUE(strstr(fh.log, "found at row 0") != NULL);
    cfr_free(vt);
}

static void test_wait_for_timeout_fails(void)
{
    CfrTerm *vt = make_term(5, 20);
    FakeHost fh;
    CfrScriptHost host;
    host_init(&fh, &host, vt);
    ASSERT_EQ(run_str("wait-for \"nope\" 0.05\n", NULL, &fh, &host),
              CFR_SCRIPT_FAILED);
    ASSERT_TRUE(strstr(fh.log, "TIMEOUT") != NULL);
    cfr_free(vt);
}

static void test_wait_for_line_end(void)
{
    CfrTerm *vt = make_term(5, 20);
    FakeHost fh;
    CfrScriptHost host;
    host_init(&fh, &host, vt);
    /* "foo bar": "bar" anchors at the row end, "foo" does not. */
    ASSERT_EQ(run_str("emit foo bar\nwait-for bar 2 line-end\n", NULL,
                      &fh, &host),
              CFR_SCRIPT_DONE);
    ASSERT_EQ(run_str("emit foo bar\nwait-for foo 0.05 line-end\n",
                      NULL, &fh, &host),
              CFR_SCRIPT_FAILED);
    cfr_free(vt);
}

static void test_assert_contains_fail(void)
{
    CfrTerm *vt = make_term(5, 20);
    FakeHost fh;
    CfrScriptHost host;
    host_init(&fh, &host, vt);
    ASSERT_EQ(run_str("assert-contains zzz\n", NULL, &fh, &host),
              CFR_SCRIPT_FAILED);
    cfr_free(vt);
}

static void test_assert_sees_scrollback(void)
{
    /* 3 rows, 4 lines: "one" scrolls off. The unified-buffer search
     * must still see it — assert-not-contains fails. */
    CfrTerm *vt = make_term(3, 20);
    FakeHost fh;
    CfrScriptHost host;
    host_init(&fh, &host, vt);
    ASSERT_EQ(run_str("emit one\\ntwo\\nthree\\nfour\\n\n"
                      "assert-not-contains one\n",
                      NULL, &fh, &host),
              CFR_SCRIPT_FAILED);
    /* And assert-contains finds it in scrollback. */
    memset(fh.log, 0, sizeof(fh.log));
    fh.log_len = 0;
    ASSERT_EQ(run_str("assert-contains one\n", NULL, &fh, &host),
              CFR_SCRIPT_DONE);
    cfr_free(vt);
}

static void test_await_exit_and_assert_exit(void)
{
    CfrTerm *vt = make_term(5, 20);
    FakeHost fh;
    CfrScriptHost host;
    host_init(&fh, &host, vt);
    fh.exit_ready = 1;
    fh.exit_code = 3;
    ASSERT_EQ(run_str("await-exit 2\nassert-exit 3\n", NULL, &fh, &host),
              CFR_SCRIPT_DONE);
    ASSERT_EQ(run_str("await-exit 2\nassert-exit 0\n", NULL, &fh, &host),
              CFR_SCRIPT_FAILED);
    cfr_free(vt);
}

static void test_assert_exit_without_await_fails(void)
{
    CfrTerm *vt = make_term(5, 20);
    FakeHost fh;
    CfrScriptHost host;
    host_init(&fh, &host, vt);
    ASSERT_EQ(run_str("assert-exit 0\n", NULL, &fh, &host),
              CFR_SCRIPT_FAILED);
    ASSERT_TRUE(strstr(fh.log, "await-exit first") != NULL);
    cfr_free(vt);
}

static void test_await_exit_timeout_fails(void)
{
    CfrTerm *vt = make_term(5, 20);
    FakeHost fh;
    CfrScriptHost host;
    host_init(&fh, &host, vt);
    /* exit_ready stays 0 */
    ASSERT_EQ(run_str("await-exit 0.05\n", NULL, &fh, &host),
              CFR_SCRIPT_FAILED);
    cfr_free(vt);
}

static void test_resize_hook(void)
{
    CfrTerm *vt = make_term(5, 20);
    FakeHost fh;
    CfrScriptHost host;
    host_init(&fh, &host, vt);
    ASSERT_EQ(run_str("resize 100 40\n", NULL, &fh, &host),
              CFR_SCRIPT_DONE);
    ASSERT_EQ(fh.resized_cols, 100);
    ASSERT_EQ(fh.resized_rows, 40);
    int rows = 0, cols = 0;
    cfr_get_dimensions(vt, &rows, &cols);
    ASSERT_EQ(cols, 100);
    ASSERT_EQ(rows, 40);
    cfr_free(vt);
}

static void test_quit_ends_early_done(void)
{
    CfrTerm *vt = make_term(5, 20);
    FakeHost fh;
    CfrScriptHost host;
    host_init(&fh, &host, vt);
    /* The failing assert after quit must not run. */
    ASSERT_EQ(run_str("quit\nassert-contains never\n", NULL, &fh, &host),
              CFR_SCRIPT_DONE);
    cfr_free(vt);
}

static void test_ext_dispatch(void)
{
    CfrTerm *vt = make_term(5, 20);
    FakeHost fh;
    CfrScriptHost host;
    host_init(&fh, &host, vt);
    const char *ext[] = { "screendump", NULL };
    ASSERT_EQ(run_str("screendump /tmp/out.png\n", ext, &fh, &host),
              CFR_SCRIPT_DONE);
    ASSERT_EQ(fh.ext_calls, 1);
    ASSERT_STR_EQ(fh.ext_name, "screendump");
    ASSERT_STR_EQ(fh.ext_args, "/tmp/out.png");
    cfr_free(vt);
}

static void test_ext_fatal_failure(void)
{
    CfrTerm *vt = make_term(5, 20);
    FakeHost fh;
    CfrScriptHost host;
    host_init(&fh, &host, vt);
    fh.ext_ok = false;
    fh.ext_fatal = true;
    const char *ext[] = { "screendump", NULL };
    ASSERT_EQ(run_str("screendump /tmp/out.png\n", ext, &fh, &host),
              CFR_SCRIPT_FAILED);
    cfr_free(vt);
}

static void test_wait_ms_non_wait(void)
{
    CfrTerm *vt = make_term(5, 20);
    FakeHost fh;
    CfrScriptHost host;
    host_init(&fh, &host, vt);
    char err[256];
    CfrScript *s = parse_str("send x\n", NULL, err, sizeof(err));
    ASSERT_NOT_NULL(s);
    CfrScriptRun *run = cfr_script_run_new(s, &host);
    ASSERT_NOT_NULL(run);
    ASSERT_EQ(cfr_script_wait_ms(run), UINT32_MAX);
    cfr_script_run_free(run);
    cfr_script_free(s);

    s = parse_str("wait 5\n", NULL, err, sizeof(err));
    ASSERT_NOT_NULL(s);
    run = cfr_script_run_new(s, &host);
    ASSERT_NOT_NULL(run);
    uint32_t ms = cfr_script_wait_ms(run);
    ASSERT_TRUE(ms > 0 && ms <= 5000);
    cfr_script_run_free(run);
    cfr_script_free(s);
    cfr_free(vt);
}

/* ------------------------------------------------------------------ */
/* Dumps                                                              */
/* ------------------------------------------------------------------ */

static char *render_to_string(const char *text, CfrTerm *vt, FakeHost *fh)
{
    CfrScriptHost host;
    host_init(fh, &host, vt);
    FILE *out = tmpfile();
    if (!out)
        return NULL;
    host.out = out;
    if (run_str(text, NULL, fh, &host) != CFR_SCRIPT_DONE) {
        fclose(out);
        return NULL;
    }
    fflush(out);
    rewind(out);
    static char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, out);
    buf[n] = '\0';
    fclose(out);
    return buf;
}

static void test_render_visible(void)
{
    CfrTerm *vt = make_term(5, 20);
    FakeHost fh;
    char *out = render_to_string("emit hello\nrender\n", vt, &fh);
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(strstr(out, " 0|hello") != NULL);
    cfr_free(vt);
}

static void test_render_scrollback(void)
{
    CfrTerm *vt = make_term(3, 20);
    FakeHost fh;
    char *out = render_to_string("emit one\\ntwo\\nthree\\nfour\\n\n"
                                 "render scrollback\n",
                                 vt, &fh);
    ASSERT_NOT_NULL(out);
    /* "one" scrolled off first, so it is the deeper scrollback row. */
    ASSERT_TRUE(strstr(out, "-2|one") != NULL);
    ASSERT_TRUE(strstr(out, "-1|two") != NULL);
    cfr_free(vt);
}

static void test_render_cells(void)
{
    CfrTerm *vt = make_term(5, 20);
    FakeHost fh;
    char *out = render_to_string("emit hi\nrender cells 0 0 3\n", vt,
                                 &fh);
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(strstr(out, "render cells row=0 cols 0-3") != NULL);
    ASSERT_TRUE(strstr(out, "col   0: 'h' w=1") != NULL);
    ASSERT_TRUE(strstr(out, "col   1: 'i' w=1") != NULL);
    cfr_free(vt);
}

static void test_render_styles(void)
{
    CfrTerm *vt = make_term(5, 20);
    FakeHost fh;
    char *out = render_to_string("emit \\e[1;31mRED\nrender styles 0\n",
                                 vt, &fh);
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(strstr(out, "render styles row=0") != NULL);
    ASSERT_TRUE(strstr(out, "attrs=") != NULL);
    cfr_free(vt);
}

static void test_cursor_dump(void)
{
    CfrTerm *vt = make_term(5, 20);
    FakeHost fh;
    char *out = render_to_string("emit abc\ncursor\n", vt, &fh);
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(strstr(out, "cursor: row=0 col=3") != NULL);
    cfr_free(vt);
}

static void test_dump_sixel_empty(void)
{
    CfrTerm *vt = make_term(5, 20);
    FakeHost fh;
    char *out = render_to_string("dump-sixel\n", vt, &fh);
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(strstr(out, "dump-sixel: 0 image(s)") != NULL);
    ASSERT_TRUE(strstr(out, "dump-sixel: 0 placement(s)") != NULL);
    cfr_free(vt);
}

/* ------------------------------------------------------------------ */
/* Blocking runner end-to-end                                         */
/* ------------------------------------------------------------------ */

static void pump_sleep(void *ud, uint32_t max_ms)
{
    (void)ud;
    msleep(max_ms > 5 ? 5 : max_ms);
}

static void test_blocking_runner(void)
{
    CfrTerm *vt = make_term(5, 20);
    FakeHost fh;
    CfrScriptHost host;
    host_init(&fh, &host, vt);
    host.out = tmpfile();
    ASSERT_NOT_NULL(host.out);

    char err[256];
    CfrScript *s = parse_str("emit hello\n"
                             "wait-for hello 2\n"
                             "assert-contains hello\n"
                             "render\n"
                             "quit\n",
                             NULL, err, sizeof(err));
    ASSERT_NOT_NULL(s);
    CfrScriptRun *run = cfr_script_run_new(s, &host);
    ASSERT_NOT_NULL(run);
    ASSERT_EQ(cfr_script_run_blocking(run, pump_sleep, NULL),
              CFR_SCRIPT_DONE);

    fflush(host.out);
    rewind(host.out);
    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, host.out);
    buf[n] = '\0';
    ASSERT_TRUE(strstr(buf, " 0|hello") != NULL);

    fclose(host.out);
    cfr_script_run_free(run);
    cfr_script_free(s);
    cfr_free(vt);
}

/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    RUN_TEST(test_parse_all_core_commands);
    RUN_TEST(test_unknown_command_rejected);
    RUN_TEST(test_ext_name_accepted_at_load);
    RUN_TEST(test_ext_unregistered_rejected);
    RUN_TEST(test_raw_malformed_rejected);
    RUN_TEST(test_render_bad_mode_rejected);
    RUN_TEST(test_load_missing_file);
    RUN_TEST(test_wait_for_option_order);

    RUN_TEST(test_send_escapes_captured);
    RUN_TEST(test_sendln_appends_crlf);
    RUN_TEST(test_raw_nul_bytes_kept);
    RUN_TEST(test_emit_feeds_parser);
    RUN_TEST(test_wait_for_found);
    RUN_TEST(test_wait_for_timeout_fails);
    RUN_TEST(test_wait_for_line_end);
    RUN_TEST(test_assert_contains_fail);
    RUN_TEST(test_assert_sees_scrollback);
    RUN_TEST(test_await_exit_and_assert_exit);
    RUN_TEST(test_assert_exit_without_await_fails);
    RUN_TEST(test_await_exit_timeout_fails);
    RUN_TEST(test_resize_hook);
    RUN_TEST(test_quit_ends_early_done);
    RUN_TEST(test_ext_dispatch);
    RUN_TEST(test_ext_fatal_failure);
    RUN_TEST(test_wait_ms_non_wait);

    RUN_TEST(test_render_visible);
    RUN_TEST(test_render_scrollback);
    RUN_TEST(test_render_cells);
    RUN_TEST(test_render_styles);
    RUN_TEST(test_cursor_dump);
    RUN_TEST(test_dump_sixel_empty);

    RUN_TEST(test_blocking_runner);

    TEST_SUMMARY();
}
