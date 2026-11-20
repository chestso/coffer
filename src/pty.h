/*
 * pty.h — internal pseudo-terminal interface shared by the coffer CLI and
 * its Windows test harness.
 *
 * The POSIX implementation still lives next to its callers for now; the
 * Windows side is shared so the bundled-ConPTY loader is exercised by both
 * the CLI and test_cfr_pty.  Keep the API deliberately small.
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

typedef struct PtyCtx PtyCtx;
typedef PtyCtx PtyContext;

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

/* CLI API: spawn with optional KEY=VAL environment overrides. */
PtyCtx *pty_spawn(int rows, int cols, char *const argv[], char *const env[],
                  int n_env);
void pty_kill(PtyCtx *p);

ssize_t pty_write(PtyCtx *p, const char *data, size_t len);
ssize_t pty_read(PtyCtx *p, char *buf, size_t bufsize);
int pty_get_fd(PtyCtx *p);
int pty_resize(PtyCtx *p, int rows, int cols);
int pty_poll_exit(PtyCtx *p, int *out_status);

void *pty_get_data_event(PtyCtx *p);
int pty_is_eof(PtyCtx *p);

/* Test-compatibility wrappers (same as the old tests/coffer_pty.h API). */
PtyContext *pty_create(int rows, int cols, char *const argv[]);
void pty_destroy(PtyContext *p);
bool pty_is_running(PtyContext *p);
int pty_get_master_fd(PtyContext *p);
int pty_get_child_pid(PtyContext *p);
int pty_signal_init(void);
void pty_signal_cleanup(void);
int pty_signal_get_fd(void);
void pty_signal_drain(void);
void *pty_get_process_handle(PtyContext *p);
void pty_close_console(PtyContext *p);

#endif /* _WIN32 */

#endif /* COFFER_PTY_INTERNAL_H */
