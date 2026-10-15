/*
 * coffer — script engine: headless VT automation scripts.
 *
 * Optional public header for the script engine that ships inside
 * libcoffer.a. Include it alongside coffer.h when embedding the
 * script runner (cfr-debug and portty's -S mode are the reference
 * hosts). No other internal headers are needed.
 *
 * A script is a line-oriented command file (`wait`, `wait-for`,
 * `send`, `assert-contains`, `render`, ... — see cfr_script_commands()
 * for the core vocabulary). The host supplies I/O through CfrScriptHost
 * hooks; UI-specific commands are registered as extensions by name at
 * load time and dispatched to the host at run time.
 *
 * Two execution models share one engine:
 *   - event-loop hosts call cfr_script_step() once per frame and fold
 *     cfr_script_wait_ms() into their event deadline (portty);
 *   - batch hosts call cfr_script_run_blocking() with a pump callback
 *     that drains their PTY (cfr-debug).
 */

#ifndef COFFER_SCRIPT_H
#define COFFER_SCRIPT_H

#include <coffer/coffer.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A parsed script (immutable command list). */
typedef struct CfrScript CfrScript;

/* Execution state for one run of a script. */
typedef struct CfrScriptRun CfrScriptRun;

typedef enum
{
    CFR_SCRIPT_RUNNING, /* more commands or a wait is pending */
    CFR_SCRIPT_DONE,    /* finished (end of script or `quit`) */
    CFR_SCRIPT_FAILED,  /* an assertion or wait-for failed */
} CfrScriptStatus;

/* Host hooks. Any function pointer may be NULL, marking the capability
 * unsupported: commands that need it warn and skip when there is a
 * sensible skip (send/raw/emit), and fail the run otherwise
 * (assert-*, wait-for, await-exit, resize). */
typedef struct
{
    /* Grid for wait-for / assert-* / render / cursor / dump-sixel. */
    CfrTerm *term;

    /* `send`/`sendln`/`raw`: write to the child's stdin. */
    void (*pty_write)(void *ud, const char *data, size_t len);

    /* `emit`/`emit-raw`: feed bytes straight into the VT parser,
     * bypassing the child. */
    void (*emit)(void *ud, const char *data, size_t len);

    /* `await-exit`: returns 1 when the child has exited (*status set),
     * 0 while it still runs, -1 on error. */
    int (*poll_exit)(void *ud, int *status);

    /* `resize`: resize the PTY and the grid. Returns 0 on success. */
    int (*resize)(void *ud, int cols, int rows);

    /* Diagnostic output; the engine pre-formats `msg`. NULL logs to
     * stderr. `line` is the 1-based script line number. */
    void (*log)(void *ud, int line, const char *msg);

    /* Destination for render/cursor/dump-sixel dumps. NULL = stdout. */
    FILE *out;

    /* Extension dispatch: commands named in `ext_names` at load time
     * arrive here with their raw argument string (unparsed, escapes
     * NOT expanded) and 1-based line number. Return true to advance to
     * the next command; return false with *fatal set to fail the run,
     * or with *fatal clear to warn and skip. */
    bool (*exec_ext)(void *ud, const char *name, const char *args,
                     int line, bool *fatal);

    void *ud;
} CfrScriptHost;

/* One core command's vocabulary entry, for --help-spec-style machine
 * introspection and help listings. */
typedef struct
{
    const char *name;
    const char *args; /* NULL for no arguments */
    const char *desc;
} CfrScriptCmdDef;

/* Load and parse a script file. `ext_names` is a NULL-terminated list
 * of additional command names the host handles via exec_ext (may be
 * NULL); any other unknown command is a parse error. Returns NULL on
 * error with a "line N: ..." message in `err` (when errlen > 0). */
CfrScript *cfr_script_load(const char *path, char *err, size_t errlen,
                           const char *const *ext_names);

void cfr_script_free(CfrScript *script);

/* The core command table, for feature detection and help output.
 * The array is static storage, owned by the library. */
const CfrScriptCmdDef *cfr_script_commands(size_t *out_count);

/* Start a run. The host struct is copied; `host.term` may be replaced
 * between steps by keeping a pointer-stable CfrTerm. */
CfrScriptRun *cfr_script_run_new(const CfrScript *script,
                                 const CfrScriptHost *host);
void cfr_script_run_free(CfrScriptRun *run);

/* Non-blocking execution, one call per frame: executes immediate
 * commands in place and returns while a wait/wait-for/await-exit is
 * pending. Returns the run status. */
CfrScriptStatus cfr_script_step(CfrScriptRun *run);

/* Milliseconds until the current wait/wait-for/await-exit can advance,
 * for folding into an event loop's deadline. Returns UINT32_MAX when
 * the current command is not a wait. A returned 0 means the deadline
 * has expired and the next step() will advance. */
uint32_t cfr_script_wait_ms(CfrScriptRun *run);

/* Blocking runner: loops cfr_script_step(); between steps calls
 * `pump(pump_ud, max_ms)` so the host drains its PTY. `max_ms` is the
 * time until the current wait can advance (capped); UINT32_MAX is
 * never passed. Returns the final status. */
typedef void (*CfrScriptPump)(void *ud, uint32_t max_ms);
CfrScriptStatus cfr_script_run_blocking(CfrScriptRun *run,
                                        CfrScriptPump pump, void *pump_ud);

/* ------------------------------------------------------------------ */
/* Direct dumps (no script needed)                                    */
/* ------------------------------------------------------------------ */

/* Dump modes for cfr_script_dump() — the same output the `render`,
 * `cursor` and `dump-sixel` script commands produce. */
typedef enum
{
    CFR_SCRIPT_DUMP_VISIBLE,    /* visible rows as text (row ignored) */
    CFR_SCRIPT_DUMP_SCROLLBACK, /* scrollback, oldest first */
    CFR_SCRIPT_DUMP_ALL,        /* scrollback then visible */
    CFR_SCRIPT_DUMP_CELLS,      /* per-cell detail for `row`           */
    CFR_SCRIPT_DUMP_STYLES,     /* style detail for `row`              */
    CFR_SCRIPT_DUMP_CURSOR,     /* cursor state                        */
    CFR_SCRIPT_DUMP_SIXEL,      /* live images and placements          */
} CfrScriptDumpMode;

/* Dump a grid view to `out` (NULL = stdout). Rows are unified
 * coordinates: >= 0 visible, < 0 scrollback depth (-1 = most recent).
 * `col_start`/`col_end` apply to CFR_SCRIPT_DUMP_CELLS
 * (col_end -1 = whole row). */
void cfr_script_dump(CfrTerm *vt, FILE *out, CfrScriptDumpMode mode,
                     int row, int col_start, int col_end);

#ifdef __cplusplus
}
#endif

#endif /* COFFER_SCRIPT_H */
