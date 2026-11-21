/*
 * pty_posix.c — POSIX pseudo-terminal, shared by the coffer CLI and
 * test_cfr_pty.
 *
 * The CLI and the test harness used to carry two copies of this code
 * (src/coffer.c and tests/pty.c).  They had drifted: the test copy
 * resized through the master fd, which SIGWINCHes the child without
 * changing its tty size.  The shared implementation resizes through the
 * slave and keeps the two callers in step.
 *
 * pty_spawn() is the CLI entry point; pty_create() and the
 * pty_signal_* functions reproduce the test harness API (terminfo paths,
 * DEL as the erase character, SIGCHLD self-pipe).
 */

#ifndef _WIN32

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pty_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

struct PtyCtx
{
    int master_fd;
    char slave_name[256]; /* the slave's /dev path, for resize ioctls */
    pid_t child_pid;      /* 0 once reaped */
    int rows, cols;
};

/* ---------------------------------------------------------------- */
/* SIGCHLD self-pipe (test harness API)                              */
/* ---------------------------------------------------------------- */

static int sigchld_pipe[2] = { -1, -1 };
static struct sigaction old_sigchld_action;
static bool sigchld_handler_installed;

/* Writes one byte to the pipe; write() is async-signal-safe. */
static void sigchld_handler(int sig)
{
    (void)sig;
    char c = 1;
    int saved_errno = errno;
    ssize_t r = write(sigchld_pipe[1], &c, 1);
    (void)r;
    errno = saved_errno;
}

int pty_signal_init(void)
{
    if (pipe(sigchld_pipe) < 0) {
        fprintf(stderr, "ERROR: pipe() failed: %s\n", strerror(errno));
        return -1;
    }

    int flags = fcntl(sigchld_pipe[0], F_GETFL);
    if (flags >= 0)
        fcntl(sigchld_pipe[0], F_SETFL, flags | O_NONBLOCK);

    flags = fcntl(sigchld_pipe[1], F_GETFL);
    if (flags >= 0)
        fcntl(sigchld_pipe[1], F_SETFL, flags | O_NONBLOCK);

    /* Do not leak the pipe into the child. */
    fcntl(sigchld_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(sigchld_pipe[1], F_SETFD, FD_CLOEXEC);

    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;

    if (sigaction(SIGCHLD, &sa, &old_sigchld_action) < 0) {
        fprintf(stderr, "ERROR: sigaction() failed: %s\n", strerror(errno));
        close(sigchld_pipe[0]);
        close(sigchld_pipe[1]);
        sigchld_pipe[0] = sigchld_pipe[1] = -1;
        return -1;
    }

    sigchld_handler_installed = true;
    return 0;
}

void pty_signal_cleanup(void)
{
    if (sigchld_handler_installed) {
        sigaction(SIGCHLD, &old_sigchld_action, NULL);
        sigchld_handler_installed = false;
    }

    if (sigchld_pipe[0] >= 0) {
        close(sigchld_pipe[0]);
        sigchld_pipe[0] = -1;
    }
    if (sigchld_pipe[1] >= 0) {
        close(sigchld_pipe[1]);
        sigchld_pipe[1] = -1;
    }
}

int pty_signal_get_fd(void)
{
    return sigchld_pipe[0];
}

void pty_signal_drain(void)
{
    if (sigchld_pipe[0] < 0)
        return;
    char buf[64];
    while (read(sigchld_pipe[0], buf, sizeof(buf)) > 0) {
        /* keep draining */
    }
}

/* ---------------------------------------------------------------- */
/* Spawn                                                             */
/* ---------------------------------------------------------------- */

/* Apply a `KEY=VALUE` override in the child (empty VALUE unsets).  The
 * override strings are the caller's; this copies the key out. */
static void apply_env_override(const char *kv)
{
    const char *eq = strchr(kv, '=');
    if (!eq)
        return;
    char key[256];
    size_t klen = (size_t)(eq - kv);
    if (klen == 0 || klen >= sizeof(key))
        return;
    memcpy(key, kv, klen);
    key[klen] = '\0';
    if (eq[1] == '\0')
        unsetenv(key);
    else
        setenv(key, eq + 1, 1);
}

/* Child-side environment for the test harness: the PTY tests need the
 * coffer terminfo entry and a shell prompt that does not depend on the
 * user's dotfiles.  Runs after forkpty(), before exec. */
static void apply_test_profile(void)
{
    /* TERM matches terminfo kbs=^?; macOS defaults to ^H, which makes
     * backspace look broken in the tests. */
    struct termios tios;
    if (tcgetattr(STDIN_FILENO, &tios) == 0) {
        tios.c_cc[VERASE] = 0x7f;
        tcsetattr(STDIN_FILENO, TCSANOW, &tios);
    }

    /* Clear an inherited TERMINFO (e.g. kitty's) that would shadow our
     * entry, and let the child find coffer's terminfo. */
    unsetenv("TERMINFO");
    {
        char buf[4096];
        const char *home = getenv("HOME");
        const char *existing = getenv("TERMINFO_DIRS");
        const char *sep = existing ? ":" : "";
        snprintf(buf, sizeof(buf), "%s/.local/share/terminfo:%s%s",
                 home ? home : "", sep, existing ? existing : "");
        setenv("TERMINFO_DIRS", buf, 1);
    }

    /* Fedora's /etc/bashrc guards its PROMPT_COMMAND setup with
     * [ -z "$PROMPT_COMMAND" ], so preset it only when unset. */
    setenv("PROMPT_COMMAND",
           "printf '\\033]0;%s@%s:%s\\007' "
           "\"${USER}\" \"${HOSTNAME%%.*}\" \"${PWD/#$HOME/\\~}\"",
           0);
}

static PtyCtx *spawn_pty(int rows, int cols, char *const argv[],
                         char *const env[], int n_env, const char *term,
                         int test_profile)
{
    PtyCtx *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->rows = rows;
    p->cols = cols;
    p->master_fd = -1;
    p->child_pid = -1;

    struct winsize ws = {
        .ws_row = (unsigned short)rows,
        .ws_col = (unsigned short)cols,
    };
    pid_t pid = forkpty(&p->master_fd, p->slave_name, NULL, &ws);
    if (pid < 0) {
        perror("forkpty");
        free(p);
        return NULL;
    }

    if (pid == 0) {
        if (test_profile)
            apply_test_profile();

        setenv("TERM", term, 1);
        setenv("COLORTERM", "truecolor", 1);
        /* Advertise kitty keyboard support the way TUIs expect: they gate
         * progressive enhancement on TERM_PROGRAM, and terminfo lookup
         * uses TERM, not this. */
        setenv("TERM_PROGRAM", "ghostty", 1);

        for (int i = 0; i < n_env; i++)
            apply_env_override(env[i]);

        if (argv && argv[0])
            execvp(argv[0], argv);
        else
            execlp(getenv("SHELL") ? getenv("SHELL") : "/bin/sh", "-sh",
                   (char *)NULL);
        _exit(127);
    }

    p->child_pid = pid;
    return p;
}

PtyCtx *pty_spawn(int rows, int cols, char *const argv[], char *const env[],
                  int n_env)
{
    return spawn_pty(rows, cols, argv, env, n_env, "xterm-256color", 0);
}

/* Test-harness entry point: same PTY, test-profile environment. */
PtyContext *pty_create(int rows, int cols, char *const argv[])
{
    return spawn_pty(rows, cols, argv, NULL, 0, "coffer-vty-256color", 1);
}

void pty_kill(PtyCtx *p)
{
    if (!p)
        return;

    /* Close the master first so the child sees EOF. */
    if (p->master_fd >= 0) {
        close(p->master_fd);
        p->master_fd = -1;
    }

    if (p->child_pid > 0) {
        int status;
        if (waitpid(p->child_pid, &status, WNOHANG) == 0) {
            kill(p->child_pid, SIGHUP);
            usleep(100000);
            if (waitpid(p->child_pid, &status, WNOHANG) == 0) {
                kill(p->child_pid, SIGKILL);
                waitpid(p->child_pid, &status, 0);
            }
        }
        p->child_pid = 0;
    }
    free(p);
}

void pty_destroy(PtyContext *p)
{
    pty_kill(p);
}

ssize_t pty_write(PtyCtx *p, const char *data, size_t len)
{
    if (!p || p->master_fd < 0 || !data || len == 0)
        return -1;
    return write(p->master_fd, data, len);
}

ssize_t pty_read(PtyCtx *p, char *buf, size_t bufsize)
{
    if (!p || p->master_fd < 0 || !buf || bufsize == 0)
        return -1;
    return read(p->master_fd, buf, bufsize);
}

int pty_get_fd(PtyCtx *p)
{
    return p ? p->master_fd : -1;
}

int pty_get_master_fd(PtyContext *p)
{
    return pty_get_fd(p);
}

int pty_get_child_pid(PtyContext *p)
{
    if (!p || p->child_pid <= 0)
        return -1;
    return (int)p->child_pid;
}

int pty_resize(PtyCtx *p, int rows, int cols)
{
    if (!p || p->master_fd < 0)
        return -1;
    struct winsize ws = {
        .ws_row = (unsigned short)rows,
        .ws_col = (unsigned short)cols,
    };
    /* TIOCSWINSZ must land on the SLAVE's tty — the child's `stty size`
     * reads that one.  The master is a different tty struct: an ioctl on
     * it still signals SIGWINCH, which makes a master-only resize look
     * like it worked while the child keeps the old size.  Open the slave
     * by name for the ioctl and close it immediately: holding it open
     * would suppress the master's EOF when the child exits. */
    int fd = open(p->slave_name, O_RDWR | O_NOCTTY);
    if (fd < 0)
        return -1;
    int r = ioctl(fd, TIOCSWINSZ, &ws);
    close(fd);
    if (r != 0)
        return -1;
    p->rows = rows;
    p->cols = cols;
    return 0;
}

int pty_poll_exit(PtyCtx *p, int *out_status)
{
    if (!p)
        return -1;
    if (p->child_pid <= 0)
        return 1; /* already reaped */
    int st = 0;
    pid_t r = waitpid(p->child_pid, &st, WNOHANG);
    if (r == 0)
        return 0;
    if (r < 0)
        return -1;
    if (out_status)
        *out_status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    p->child_pid = 0;
    return 1;
}

bool pty_is_running(PtyContext *p)
{
    return pty_poll_exit(p, NULL) == 0;
}

#endif /* !_WIN32 */
