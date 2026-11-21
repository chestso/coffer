/*
 * pty_internal.h — internal pseudo-terminal interface shared by the coffer
 * CLI and its test harness.
 *
 * One implementation per platform: src/pty_posix.c (forkpty) and
 * src/pty_w32.c (ConPTY, with the bundled-DLL loader in conpty.c).  Both
 * are archived into libcofferpty.a, which the CLI and test_cfr_pty link.
 *
 * The pty_create()/pty_destroy()/pty_signal_* group is the test-harness
 * API; pty_spawn()/pty_kill() is the CLI API.
 *
 * The file is deliberately not called pty.h: src/ is on the include path,
 * so a local pty.h would shadow the system <pty.h> that pty_posix.c needs
 * for forkpty().
 */

#ifndef COFFER_PTY_INTERNAL_H
#define COFFER_PTY_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

#ifdef _WIN32

#include <basetsd.h>
#include <windows.h>

#include "conpty.h"

typedef SSIZE_T ssize_t;

#else /* !_WIN32 */

#include <sys/types.h>

#endif /* _WIN32 */

typedef struct PtyCtx PtyCtx;
typedef PtyCtx PtyContext;

/* ---------------------------------------------------------------- */
/* CLI API                                                           */
/* ---------------------------------------------------------------- */

/* Spawn `argv` (or $SHELL when NULL) on a new PTY.  `env` holds
 * KEY=VALUE overrides applied after the built-ins; an empty value
 * unsets the variable.  Returns NULL on failure. */
PtyCtx *pty_spawn(int rows, int cols, char *const argv[], char *const env[],
                  int n_env);

/* Release the PTY and kill the child if it is still running. */
void pty_kill(PtyCtx *p);

ssize_t pty_write(PtyCtx *p, const char *data, size_t len);
ssize_t pty_read(PtyCtx *p, char *buf, size_t bufsize);

/* Master fd for poll/select (POSIX); -1 on Windows. */
int pty_get_fd(PtyCtx *p);

/* Resize the child's window size; the grid is resized by the caller.
 * POSIX: TIOCSWINSZ on the slave, which also SIGWINCHes the foreground
 * group.  Returns 0 on success. */
int pty_resize(PtyCtx *p, int rows, int cols);

/* Non-blocking child check.  Returns 1 when the child has exited (with
 * *out_status set: the exit code, or 128+signal on POSIX), 0 while it
 * still runs, -1 on error.  A reaped child stays "exited" for later
 * calls. */
int pty_poll_exit(PtyCtx *p, int *out_status);

/* ---------------------------------------------------------------- */
/* Test-harness API                                                  */
/* ---------------------------------------------------------------- */

PtyContext *pty_create(int rows, int cols, char *const argv[]);
void pty_destroy(PtyContext *p);
bool pty_is_running(PtyContext *p);
int pty_get_master_fd(PtyContext *p);
int pty_get_child_pid(PtyContext *p);

/* SIGCHLD self-pipe: init installs the handler, get_fd returns the read
 * end for poll(), drain empties it.  No-ops on Windows. */
int pty_signal_init(void);
void pty_signal_cleanup(void);
int pty_signal_get_fd(void);
void pty_signal_drain(void);

#ifdef _WIN32

/* Select and load the ConPTY backend.  AUTO falls back to the system
 * implementation; BUNDLED fails (returns -1) when no usable bundle exists;
 * SYSTEM never loads conpty.dll.  Call before pty_spawn(). */
int pty_set_conpty(CfrConptyMode mode, const char *override_dir);
void pty_set_verbose(int verbose);

/* Diagnostics for --conpty-info / verbose logging. */
const char *pty_conpty_backend(void);
const char *pty_conpty_version(void);
const char *pty_conpty_dll_path(void);
const char *pty_conpty_host_path(void);
const char *pty_conpty_dll_version(void);
const char *pty_conpty_host_version(void);
const char *pty_conpty_error(void);
int pty_conpty_has_openconsole(void);

/* Windows-only plumbing the test harness waits on. */
void *pty_get_data_event(PtyCtx *p);
int pty_is_eof(PtyCtx *p);
void *pty_get_process_handle(PtyContext *p);
void pty_close_console(PtyContext *p);

/* Test hook: ConPTY create flags (PSEUDOCONSOLE_* bits) for subsequent
 * spawns.  Production always passes 0; test_cfr_pty uses it to check the
 * width-alignment modes. */
void pty_set_conpty_flags(DWORD flags);

#endif /* _WIN32 */

#endif /* COFFER_PTY_INTERNAL_H */
