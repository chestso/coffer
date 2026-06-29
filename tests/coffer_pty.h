#ifndef COFFER_PTY_H
#define COFFER_PTY_H

#include <stdbool.h>
#include <stddef.h>

#ifdef _WIN32
#include <basetsd.h>
typedef SSIZE_T ssize_t;
#else
#include <sys/types.h>
#endif

/* Opaque — defined in platform-specific pty.c / pty_w32.c */
typedef struct PtyContext PtyContext;

/**
 * Create a new PTY and spawn a process.
 *
 * @param rows Initial terminal rows
 * @param cols Initial terminal columns
 * @param argv NULL-terminated argument array (argv[0] is program to execute).
 *             If NULL, spawns the default shell.
 * @return PtyContext pointer on success, NULL on failure
 */
PtyContext *pty_create(int rows, int cols, char *const argv[]);

/**
 * Destroy PTY context and terminate child process.
 *
 * @param ctx PTY context to destroy
 */
void pty_destroy(PtyContext *ctx);

/**
 * Write data to the PTY (send to child).
 *
 * @param ctx PTY context
 * @param data Data to write
 * @param len Length of data
 * @return Number of bytes written, or -1 on error
 */
ssize_t pty_write(PtyContext *ctx, const char *data, size_t len);

/**
 * Read data from the PTY (receive from child).
 *
 * @param ctx PTY context
 * @param buf Buffer to read into
 * @param bufsize Size of buffer
 * @return Number of bytes read, 0 on EOF, or -1 on error
 */
ssize_t pty_read(PtyContext *ctx, char *buf, size_t bufsize);

/**
 * Resize the PTY window size.
 *
 * @param ctx PTY context
 * @param rows New number of rows
 * @param cols New number of columns
 * @return 0 on success, -1 on error
 */
int pty_resize(PtyContext *ctx, int rows, int cols);

/**
 * Check if the child process is still running.
 *
 * @param ctx PTY context
 * @return true if child is still running, false otherwise
 */
bool pty_is_running(PtyContext *ctx);

/**
 * Get the master file descriptor for poll/select (POSIX only).
 *
 * @param ctx PTY context
 * @return Master file descriptor, or -1 on Windows / not available
 */
int pty_get_master_fd(PtyContext *ctx);

/**
 * Initialize SIGCHLD signal handling (POSIX only; no-op on Windows).
 *
 * @return 0 on success, -1 on failure
 */
int pty_signal_init(void);

/**
 * Cleanup SIGCHLD signal handling (POSIX only; no-op on Windows).
 */
void pty_signal_cleanup(void);

/**
 * Get the read end of the signal pipe for poll/select (POSIX only).
 *
 * @return File descriptor, or -1 if signal handling not initialized / Windows
 */
int pty_signal_get_fd(void);

/**
 * Drain the signal pipe after it becomes readable (POSIX only; no-op on Windows).
 */
void pty_signal_drain(void);

/**
 * Get the child process PID (POSIX only, for /proc queries).
 *
 * @param ctx PTY context
 * @return Child PID, or -1 if not running
 */
int pty_get_child_pid(PtyContext *ctx);

#ifdef _WIN32
/**
 * Get the child process handle for WaitForMultipleObjects.
 *
 * @param ctx PTY context
 * @return Process HANDLE cast to void*, or NULL
 */
void *pty_get_process_handle(PtyContext *ctx);

/**
 * Get the output read handle for ReadFile / WaitForMultipleObjects.
 *
 * @param ctx PTY context
 * @return Read handle cast to void*, or NULL
 */
void *pty_get_output_handle(PtyContext *ctx);

/**
 * Close the pseudo-console to unblock any pending ReadFile.
 *
 * Call this before waiting for the reader thread to exit on shutdown.
 * Safe to call multiple times.
 */
void pty_close_console(PtyContext *ctx);
#endif

#endif /* COFFER_PTY_H */
