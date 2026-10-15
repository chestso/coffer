/*
 * coffer — script engine: execution.
 *
 * One executor object (CfrScriptRun) serves both hosts: event-loop
 * hosts call cfr_script_step() once per frame and fold
 * cfr_script_wait_ms() into their event deadline (portty); batch hosts
 * call cfr_script_run_blocking() with a pump that drains their PTY
 * (cfr-debug). All state lives on the run object — no file-static
 * wait deadlines.
 */

#include "script_internal.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

/* Bytes reserved per cell when flattening a row: a grapheme cluster
 * holds at most CFR_CLUSTER_MAX (16) codepoints, 4 bytes each. */
#define ROW_UTF8_PER_CELL 64

/* ------------------------------------------------------------------ */
/* Time                                                               */
/* ------------------------------------------------------------------ */

static double now_sec(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    static int initialized;
    if (!initialized) {
        QueryPerformanceFrequency(&freq);
        initialized = 1;
    }
    LARGE_INTEGER count;
    QueryPerformanceCounter(&count);
    return (double)count.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
#endif
}

/* ------------------------------------------------------------------ */
/* Run object                                                         */
/* ------------------------------------------------------------------ */

struct CfrScriptRun
{
    const CfrScript *script; /* not owned */
    CfrScriptHost host;
    size_t index;
    double deadline; /* armed wait deadline (monotonic seconds) */
    bool deadline_armed;
    int exit_status; /* recorded by await-exit */
    bool have_exit_status;
    CfrScriptStatus status;
};

CfrScriptRun *cfr_script_run_new(const CfrScript *script,
                                 const CfrScriptHost *host)
{
    if (!script || !host)
        return NULL;
    CfrScriptRun *run = calloc(1, sizeof(*run));
    if (!run)
        return NULL;
    run->script = script;
    run->host = *host;
    run->status = CFR_SCRIPT_RUNNING;
    return run;
}

void cfr_script_run_free(CfrScriptRun *run)
{
    free(run);
}

/* ------------------------------------------------------------------ */
/* Logging                                                            */
/* ------------------------------------------------------------------ */

static void run_log(CfrScriptRun *run, int line, const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (run->host.log)
        run->host.log(run->host.ud, line, msg);
    else
        fprintf(stderr, "[%d] %s\n", line, msg);
}

static FILE *run_out(CfrScriptRun *run)
{
    return run->host.out ? run->host.out : stdout;
}

/* ------------------------------------------------------------------ */
/* Wait deadline                                                      */
/* ------------------------------------------------------------------ */

/* Arm (once) and return the current command's deadline. */
static double run_deadline(CfrScriptRun *run, const CfrScriptCmd *cmd)
{
    if (!run->deadline_armed) {
        run->deadline = now_sec() + cmd->seconds;
        run->deadline_armed = true;
    }
    return run->deadline;
}

static void run_advance(CfrScriptRun *run)
{
    run->deadline_armed = false;
    run->index++;
}

uint32_t cfr_script_wait_ms(CfrScriptRun *run)
{
    if (!run || run->status != CFR_SCRIPT_RUNNING ||
        run->index >= run->script->count)
        return UINT32_MAX;
    const CfrScriptCmd *cmd = &run->script->cmds[run->index];
    if (cmd->type != CFR_CMD_WAIT && cmd->type != CFR_CMD_WAIT_FOR &&
        cmd->type != CFR_CMD_AWAIT_EXIT)
        return UINT32_MAX;

    double remaining = run_deadline(run, cmd) - now_sec();
    if (remaining <= 0)
        return 0;
    double ms = remaining * 1000.0;
    uint32_t rounded = (uint32_t)ms;
    if ((double)rounded < ms)
        rounded++; /* ceil so we never wake before the deadline */
    return rounded == 0 ? 1 : rounded;
}

/* ------------------------------------------------------------------ */
/* Grid dumps (render / cursor / dump-sixel)                          */
/* ------------------------------------------------------------------ */

static void dump_row_text(CfrTerm *vt, FILE *out, char *buf, size_t cap,
                          int row)
{
    cfr_row_text(vt, row, buf, cap);
    fprintf(out, "%2d|%s\n", row, buf);
}

static void dump_text(CfrTerm *vt, FILE *out, CfrScriptDumpMode mode)
{
    int rows = 0, cols = 0;
    cfr_get_dimensions(vt, &rows, &cols);
    size_t cap = (size_t)cols * ROW_UTF8_PER_CELL + 1;
    char *buf = malloc(cap);
    if (!buf)
        return;

    if (mode == CFR_SCRIPT_DUMP_SCROLLBACK ||
        mode == CFR_SCRIPT_DUMP_ALL) {
        int lines = cfr_get_scrollback_lines(vt);
        if (lines <= 0)
            fprintf(out, "(scrollback empty)\n");
        for (int sb = lines - 1; sb >= 0; sb--)
            dump_row_text(vt, out, buf, cap, -(sb + 1));
    }
    if (mode == CFR_SCRIPT_DUMP_VISIBLE || mode == CFR_SCRIPT_DUMP_ALL) {
        for (int r = 0; r < rows; r++)
            dump_row_text(vt, out, buf, cap, r);
    }
    free(buf);
}

/* Unified cell access: row < 0 is scrollback depth. */
static const CfrCell *unified_cell(CfrTerm *vt, int row, int col)
{
    if (row < 0)
        return cfr_get_scrollback_cell(vt, -row - 1, col);
    return cfr_get_cell(vt, row, col);
}

static void describe_cp(const CfrCell *cell, char *buf, size_t cap)
{
    if (cell->cp == 0 || cell->cp == 0x20)
        snprintf(buf, cap, "SP");
    else if (cell->cp < 0x80)
        snprintf(buf, cap, "'%c'", (char)cell->cp);
    else
        snprintf(buf, cap, "U+%04X", cell->cp);
}

static void dump_cells(CfrTerm *vt, FILE *out, int row, int col_start,
                       int col_end)
{
    int rows = 0, cols = 0;
    cfr_get_dimensions(vt, &rows, &cols);
    if (col_end < 0 || col_end >= cols)
        col_end = cols - 1;

    fprintf(out, "render cells row=%d cols %d-%d\n", row, col_start,
            col_end);
    for (int c = col_start; c <= col_end; c++) {
        const CfrCell *cell = unified_cell(vt, row, c);
        if (!cell) {
            fprintf(out, "  col %3d: [?]\n", c);
            continue;
        }
        if (cell->width == 0 && cell->cp == 0) {
            fprintf(out, "  col %3d: (blank/continuation)\n", c);
            continue;
        }
        const CfrStyle *st = cfr_cell_style(vt, cell);
        char cp_str[16];
        describe_cp(cell, cp_str, sizeof(cp_str));
        if (st)
            fprintf(out,
                    "  col %3d: %s w=%d attrs=0x%04X "
                    "fg=%06X%s bg=%06X%s ul=%d ul_rgb=%06X%s link=%u\n",
                    c, cp_str, cell->width, st->attrs, st->fg_rgb,
                    (st->color_flags & CFR_COLOR_DEFAULT_FG) ? "(def)" : "",
                    st->bg_rgb,
                    (st->color_flags & CFR_COLOR_DEFAULT_BG) ? "(def)" : "",
                    st->underline, st->ul_rgb,
                    (st->color_flags & CFR_COLOR_DEFAULT_UL) ? "(def)" : "",
                    cell->hyperlink_id);
        else
            fprintf(out, "  col %3d: %s w=%d (no style)\n", c, cp_str,
                    cell->width);
    }
}

static void dump_styles(CfrTerm *vt, FILE *out, int row)
{
    int rows = 0, cols = 0;
    cfr_get_dimensions(vt, &rows, &cols);

    fprintf(out, "render styles row=%d\n", row);
    for (int c = 0; c < cols; c++) {
        const CfrCell *cell = unified_cell(vt, row, c);
        if (!cell)
            continue;
        const CfrStyle *st = cfr_cell_style(vt, cell);
        if (!st)
            continue;
        if (st->underline || st->bg_rgb || st->fg_rgb || st->attrs ||
            st->color_flags) {
            char cp_str[16];
            describe_cp(cell, cp_str, sizeof(cp_str));
            fprintf(out,
                    "  col %3d: %s ul=%d ul_rgb=%06X%s fg=%06X%s "
                    "bg=%06X%s attrs=%02X\n",
                    c, cp_str, st->underline, st->ul_rgb,
                    (st->color_flags & CFR_COLOR_DEFAULT_UL) ? "(def)" : "",
                    st->fg_rgb,
                    (st->color_flags & CFR_COLOR_DEFAULT_FG) ? "(def)" : "",
                    st->bg_rgb,
                    (st->color_flags & CFR_COLOR_DEFAULT_BG) ? "(def)" : "",
                    st->attrs);
        }
        if (cell->width == 2)
            c++;
    }
}

static void dump_cursor(CfrTerm *vt, FILE *out)
{
    CfrCursor c = cfr_get_cursor(vt);
    fprintf(out, "cursor: row=%d col=%d visible=%d blink=%d\n", c.row,
            c.col, c.visible, c.blink);
}

static void dump_sixel(CfrTerm *vt, FILE *out)
{
    int count = 0;
    const CfrImage *imgs = cfr_get_images(vt, &count);
    fprintf(out, "dump-sixel: %d image(s)\n", count);
    for (int i = 0; i < count; i++) {
        fprintf(out, "  [%d] id=%llu row=%d col=%d %dx%dpx\n", i,
                (unsigned long long)imgs[i].id, imgs[i].row, imgs[i].col,
                imgs[i].width_px, imgs[i].height_px);
    }

    int pl_count = 0;
    const CfrImagePlacement *pls = cfr_get_image_placements(vt, &pl_count);
    fprintf(out, "dump-sixel: %d placement(s)\n", pl_count);
    for (int i = 0; i < pl_count; i++) {
        fprintf(out,
                "  [%d] id=%llu image=%llu row=%d col=%d %dx%d cells "
                "src=%d,%d %dx%d z=%d\n",
                i, (unsigned long long)pls[i].id,
                (unsigned long long)pls[i].image_id, pls[i].row, pls[i].col,
                pls[i].rows, pls[i].cols, pls[i].src_x, pls[i].src_y,
                pls[i].src_w, pls[i].src_h, pls[i].z_index);
    }
}

void cfr_script_dump(CfrTerm *vt, FILE *out, CfrScriptDumpMode mode,
                     int row, int col_start, int col_end)
{
    if (!vt)
        return;
    if (!out)
        out = stdout;
    switch (mode) {
    case CFR_SCRIPT_DUMP_VISIBLE:
    case CFR_SCRIPT_DUMP_SCROLLBACK:
    case CFR_SCRIPT_DUMP_ALL:
        dump_text(vt, out, mode);
        break;
    case CFR_SCRIPT_DUMP_CELLS:
        dump_cells(vt, out, row, col_start, col_end);
        break;
    case CFR_SCRIPT_DUMP_STYLES:
        dump_styles(vt, out, row);
        break;
    case CFR_SCRIPT_DUMP_CURSOR:
        dump_cursor(vt, out);
        break;
    case CFR_SCRIPT_DUMP_SIXEL:
        dump_sixel(vt, out);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Command execution                                                  */
/* ------------------------------------------------------------------ */

/* Commands that need host.term: fail the run when it is missing. */
static bool require_term(CfrScriptRun *run, const CfrScriptCmd *cmd,
                         const char *what)
{
    if (run->host.term)
        return true;
    run_log(run, cmd->line, "FAIL: %s: no terminal", what);
    run->status = CFR_SCRIPT_FAILED;
    return false;
}

CfrScriptStatus cfr_script_step(CfrScriptRun *run)
{
    if (!run)
        return CFR_SCRIPT_FAILED;
    if (run->status != CFR_SCRIPT_RUNNING)
        return run->status;
    if (run->index >= run->script->count) {
        run->status = CFR_SCRIPT_DONE;
        return run->status;
    }

    const CfrScriptCmd *cmd = &run->script->cmds[run->index];

    switch (cmd->type) {
    case CFR_CMD_WAIT:
        if (now_sec() >= run_deadline(run, cmd))
            run_advance(run);
        break;

    case CFR_CMD_WAIT_FOR:
    {
        if (!require_term(run, cmd, "wait-for"))
            break;
        int row = 0;
        if (cfr_find_text(run->host.term, cmd->text, cmd->line_end,
                          &row)) {
            double deadline = run_deadline(run, cmd);
            double elapsed = cmd->seconds - (deadline - now_sec());
            if (row < 0)
                run_log(run, cmd->line,
                        "wait-for \"%s\": found at scrollback row %d "
                        "after %.1fs",
                        cmd->text, row, elapsed);
            else
                run_log(run, cmd->line,
                        "wait-for \"%s\": found at row %d after %.1fs",
                        cmd->text, row, elapsed);
            run_advance(run);
        } else if (now_sec() >= run_deadline(run, cmd)) {
            run_log(run, cmd->line,
                    "FAIL: wait-for \"%s\": TIMEOUT after %.1fs",
                    cmd->text, cmd->seconds);
            run->status = CFR_SCRIPT_FAILED;
        }
        break;
    }

    case CFR_CMD_SEND:
        if (run->host.pty_write) {
            run_log(run, cmd->line, "send: %s", cmd->text);
            run->host.pty_write(run->host.ud, cmd->text,
                                strlen(cmd->text));
        } else {
            run_log(run, cmd->line, "WARN: send: no pty_write hook; skipped");
        }
        run_advance(run);
        break;

    case CFR_CMD_SENDLN:
        if (run->host.pty_write) {
            run_log(run, cmd->line, "sendln: %s", cmd->text);
            run->host.pty_write(run->host.ud, cmd->text, strlen(cmd->text));
            run->host.pty_write(run->host.ud, "\r\n", 2);
        } else {
            run_log(run, cmd->line,
                    "WARN: sendln: no pty_write hook; skipped");
        }
        run_advance(run);
        break;

    case CFR_CMD_RAW:
        if (run->host.pty_write) {
            run_log(run, cmd->line, "raw: %zu bytes", cmd->bytes_len);
            run->host.pty_write(run->host.ud, (const char *)cmd->bytes,
                                cmd->bytes_len);
        } else {
            run_log(run, cmd->line, "WARN: raw: no pty_write hook; skipped");
        }
        run_advance(run);
        break;

    case CFR_CMD_EMIT:
        if (run->host.emit) {
            run_log(run, cmd->line, "emit: %s", cmd->text);
            run->host.emit(run->host.ud, cmd->text, strlen(cmd->text));
        } else {
            run_log(run, cmd->line, "WARN: emit: no emit hook; skipped");
        }
        run_advance(run);
        break;

    case CFR_CMD_EMIT_RAW:
        if (run->host.emit) {
            run_log(run, cmd->line, "emit-raw: %zu bytes", cmd->bytes_len);
            run->host.emit(run->host.ud, (const char *)cmd->bytes,
                           cmd->bytes_len);
        } else {
            run_log(run, cmd->line,
                    "WARN: emit-raw: no emit hook; skipped");
        }
        run_advance(run);
        break;

    case CFR_CMD_ASSERT_CONTAINS:
        if (!require_term(run, cmd, "assert-contains"))
            break;
        if (cfr_find_text(run->host.term, cmd->text, false, NULL)) {
            run_log(run, cmd->line, "OK: buffer contains \"%s\"", cmd->text);
            run_advance(run);
        } else {
            run_log(run, cmd->line, "FAIL: buffer does not contain \"%s\"",
                    cmd->text);
            run->status = CFR_SCRIPT_FAILED;
        }
        break;

    case CFR_CMD_ASSERT_NOT_CONTAINS:
        if (!require_term(run, cmd, "assert-not-contains"))
            break;
        if (cfr_find_text(run->host.term, cmd->text, false, NULL)) {
            run_log(run, cmd->line, "FAIL: buffer contains \"%s\"",
                    cmd->text);
            run->status = CFR_SCRIPT_FAILED;
        } else {
            run_log(run, cmd->line, "OK: buffer does not contain \"%s\"",
                    cmd->text);
            run_advance(run);
        }
        break;

    case CFR_CMD_AWAIT_EXIT:
    {
        if (!run->host.poll_exit) {
            run_log(run, cmd->line,
                    "FAIL: await-exit: no poll_exit hook");
            run->status = CFR_SCRIPT_FAILED;
            break;
        }
        int st = 0;
        int r = run->host.poll_exit(run->host.ud, &st);
        if (r == 1) {
            run->exit_status = st;
            run->have_exit_status = true;
            double deadline = run_deadline(run, cmd);
            run_log(run, cmd->line,
                    "await-exit: child exited with %d after %.1fs", st,
                    cmd->seconds - (deadline - now_sec()));
            run_advance(run);
        } else if (r < 0) {
            run_log(run, cmd->line,
                    "FAIL: await-exit: cannot query the child");
            run->status = CFR_SCRIPT_FAILED;
        } else if (now_sec() >= run_deadline(run, cmd)) {
            run_log(run, cmd->line, "FAIL: await-exit: TIMEOUT after %.1fs",
                    cmd->seconds);
            run->status = CFR_SCRIPT_FAILED;
        }
        break;
    }

    case CFR_CMD_ASSERT_EXIT:
        if (!run->have_exit_status) {
            run_log(run, cmd->line,
                    "FAIL: assert-exit: no status recorded "
                    "(await-exit first)");
            run->status = CFR_SCRIPT_FAILED;
        } else if (run->exit_status != cmd->a) {
            run_log(run, cmd->line, "FAIL: child exit status %d, want %d",
                    run->exit_status, cmd->a);
            run->status = CFR_SCRIPT_FAILED;
        } else {
            run_log(run, cmd->line, "OK: child exit status %d", cmd->a);
            run_advance(run);
        }
        break;

    case CFR_CMD_RESIZE:
        if (!run->host.resize) {
            run_log(run, cmd->line, "FAIL: resize: no resize hook");
            run->status = CFR_SCRIPT_FAILED;
            break;
        }
        if (run->host.resize(run->host.ud, cmd->a, cmd->b) != 0) {
            run_log(run, cmd->line, "FAIL: resize %dx%d failed", cmd->a,
                    cmd->b);
            run->status = CFR_SCRIPT_FAILED;
        } else {
            run_log(run, cmd->line, "resize: %dx%d", cmd->a, cmd->b);
            run_advance(run);
        }
        break;

    case CFR_CMD_RENDER:
        if (!require_term(run, cmd, "render"))
            break;
        cfr_script_dump(run->host.term, run_out(run),
                        (CfrScriptDumpMode)cmd->render_mode, cmd->a,
                        cmd->b, cmd->c);
        run_advance(run);
        break;

    case CFR_CMD_CURSOR:
        if (!require_term(run, cmd, "cursor"))
            break;
        cfr_script_dump(run->host.term, run_out(run),
                        CFR_SCRIPT_DUMP_CURSOR, 0, 0, 0);
        run_advance(run);
        break;

    case CFR_CMD_DUMP_SIXEL:
        if (!require_term(run, cmd, "dump-sixel"))
            break;
        cfr_script_dump(run->host.term, run_out(run),
                        CFR_SCRIPT_DUMP_SIXEL, 0, 0, 0);
        run_advance(run);
        break;

    case CFR_CMD_QUIT:
        run_log(run, cmd->line, "quit");
        run->status = CFR_SCRIPT_DONE;
        break;

    case CFR_CMD_EXT:
        if (!run->host.exec_ext) {
            run_log(run, cmd->line,
                    "WARN: %s: registered but no exec_ext hook; skipped",
                    cmd->ext_name);
            run_advance(run);
            break;
        }
        {
            bool fatal = false;
            bool ok = run->host.exec_ext(run->host.ud, cmd->ext_name,
                                         cmd->text, cmd->line, &fatal);
            if (!ok && fatal) {
                run->status = CFR_SCRIPT_FAILED;
            } else {
                if (!ok)
                    run_log(run, cmd->line, "WARN: %s: failed; skipped",
                            cmd->ext_name);
                run_advance(run);
            }
        }
        break;
    }

    if (run->status == CFR_SCRIPT_RUNNING &&
        run->index >= run->script->count)
        run->status = CFR_SCRIPT_DONE;
    return run->status;
}

/* ------------------------------------------------------------------ */
/* Blocking runner                                                    */
/* ------------------------------------------------------------------ */

CfrScriptStatus cfr_script_run_blocking(CfrScriptRun *run,
                                        CfrScriptPump pump, void *pump_ud)
{
    for (;;) {
        CfrScriptStatus st = cfr_script_step(run);
        if (st != CFR_SCRIPT_RUNNING)
            return st;
        uint32_t ms = cfr_script_wait_ms(run);
        if (ms == UINT32_MAX)
            ms = 0;
        /* Pump in 50 ms slices so a long wait still drains eagerly. */
        if (ms > 50)
            ms = 50;
        if (pump)
            pump(pump_ud, ms);
    }
}
