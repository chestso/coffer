/*
 * coffer — headless PTY inspector for the coffer VT engine.
 *
 * Spawns a child process on a PTY (forkpty on POSIX, ConPTY on
 * Windows), pipes its output through coffer, and renders the
 * resulting grid as plain text. Optional features:
 *   - Scripted input with timing via -f script file
 *   - Save raw PTY output for offline replay
 *   - Dump individual rows or raw cell data
 *   - Assert grid contents for CI
 *
 * The coffer output callback is wired back to the PTY so that
 * terminal queries (DA, DSR, kitty keyboard) receive proper replies.
 * This allows interactive TUI applications (crush, helix, mudlark,
 * etc.) to render correctly without a real terminal emulator.
 */

#define _GNU_SOURCE
#include "config.h"
#include <coffer/coffer.h>
#include <coffer/script.h>

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include "pty.h"
#include <wchar.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif
#endif

/* ================================================================ */
/* Flag system                                                      */
/* ================================================================ */

typedef struct
{
    const char *name;  /* e.g. "--rows" or "-r" */
    const char *alias; /* short alias, may be NULL */
    int takes_value;   /* 1 if flag takes a value, 0 if boolean */
    const char *param; /* value placeholder for help (e.g. "ROWS"), NULL if boolean */
    const char *desc;  /* help description */
} FlagDef;

static const FlagDef OPTIONS[] = {
    { "--rows", "-r", 1, "ROWS", "Terminal rows (default: 24)" },
    { "--cols", "-c", 1, "COLS", "Terminal columns (default: 80)" },
    { "--wait", "-w", 1, "SEC", "Initial wait before script (default: 3)" },
    { "--script", "-f", 1, "FILE", "Script file to execute" },
    { "--output", "-o", 1, "FILE", "Save raw PTY output to file" },
    { "--show-row", "-s", 1, "ROW", "Dump specific row (repeatable)" },
    { "--cell-data", "-d", 0, NULL, "Dump raw cell data alongside text" },
    { "--style-data", "-S", 0, NULL, "Dump style info (underline, colors)" },
    { "--quiet", "-q", 0, NULL, "Only dump specified rows" },
    { "--kbd-data", "-k", 0, NULL,
      "Dump the kitty keyboard protocol state with the final dump" },
    { "--feed", NULL, 1, "FILE",
      "Feed FILE's bytes into the VT as terminal output before the "
      "script (no child is spawned unless a command follows --)" },
    { "--env", NULL, 1, "KEY=VAL",
      "Set a child env var (repeatable; applied after the built-ins; "
      "an empty value unsets it)" },
    { "--conpty", NULL, 1, "MODE",
      "Windows ConPTY backend: auto (default), bundled, or system" },
    { "--conpty-dir", NULL, 1, "DIR",
      "Directory containing conpty.dll and OpenConsole.exe" },
    { "--conpty-info", NULL, 0, NULL,
      "Print the selected ConPTY backend and exit" },
    { "--verbose", "-v", 0, NULL,
      "Log the PTY backend, child command and other diagnostics" },
    { "--help", "-h", 0, NULL, "Show this help" },
    { NULL, NULL, 0, NULL, NULL }
};

/* Script vocabulary comes from the engine's registry
 * (cfr_script_commands) — --help-spec prints it as machine-readable
 * CMD lines, --help as the human listing. A driver that needs
 * `wait-for` greps the spec for "CMD\twait-for" instead of guessing
 * from a version number. */

static int matches_flag(const char *arg, const FlagDef *def)
{
    return (strcmp(arg, def->name) == 0) ||
           (def->alias && strcmp(arg, def->alias) == 0);
}

static int find_flag(const char *arg)
{
    for (int i = 0; OPTIONS[i].name; i++) {
        if (matches_flag(arg, &OPTIONS[i]))
            return i;
    }
    return -1;
}

/* ================================================================ */
/* Help system                                                      */
/* ================================================================ */

static void print_help(void)
{
    printf("coffer — headless PTY inspector for the coffer VT engine\n\n");
    printf("Usage: coffer [options] [-- command args...]\n\n");
    printf("Spawn a child process on a pseudo-terminal, pipe its output\n"
           "through coffer, and render the resulting grid as plain text.\n\n");
    printf("Options:\n");
    for (int i = 0; OPTIONS[i].name; i++) {
        char label[64];
        if (OPTIONS[i].alias && OPTIONS[i].param)
            snprintf(label, sizeof(label), "%s, %s %s",
                     OPTIONS[i].name, OPTIONS[i].alias, OPTIONS[i].param);
        else if (OPTIONS[i].alias)
            snprintf(label, sizeof(label), "%s, %s",
                     OPTIONS[i].name, OPTIONS[i].alias);
        else if (OPTIONS[i].param)
            snprintf(label, sizeof(label), "%s %s",
                     OPTIONS[i].name, OPTIONS[i].param);
        else
            snprintf(label, sizeof(label), "%s",
                     OPTIONS[i].name);
        printf("  %-22s  %s\n", label, OPTIONS[i].desc);
    }
    printf("\nScript file commands (via --script):\n");
    size_t n_cmds = 0;
    const CfrScriptCmdDef *cmds = cfr_script_commands(&n_cmds);
    for (size_t i = 0; i < n_cmds; i++) {
        char label[80];
        if (cmds[i].args)
            snprintf(label, sizeof(label), "%s %s", cmds[i].name,
                     cmds[i].args);
        else
            snprintf(label, sizeof(label), "%s", cmds[i].name);
        printf("  %-30s  %s\n", label, cmds[i].desc);
    }
    printf("\n  send/raw write to the PTY INPUT (the child's stdin), never to\n"
           "  coffer's parser. To emit escape sequences the terminal processes,\n"
           "  have the child print them (e.g. printf '\\033[?12l').\n"
           "  assert-* and wait-for search the UNIFIED buffer: scrollback\n"
           "  (oldest first) then the visible rows.\n");
    printf("\nExamples:\n");
    printf("  coffer -- bash -l\n");
    printf("  coffer -c 68 -r 20 -w 5 -- crush\n");
    printf("  coffer -r 24 -c 80 -f test.script -- mudlark carrionfields.net 4449\n");
    printf("\nRun 'coffer --help-spec' for machine-readable flag listing.\n");
}

static void print_help_spec(void)
{
    for (int i = 0; OPTIONS[i].name; i++) {
        const char *alias = OPTIONS[i].alias ? OPTIONS[i].alias : "-";
        const char *param = OPTIONS[i].param ? OPTIONS[i].param : "-";
        printf("FLAG\t%s\t%s\t%d\t%s\t%s\n",
               OPTIONS[i].name, alias, OPTIONS[i].takes_value,
               param, OPTIONS[i].desc);
    }
    size_t n_cmds = 0;
    const CfrScriptCmdDef *cmds = cfr_script_commands(&n_cmds);
    for (size_t i = 0; i < n_cmds; i++) {
        const char *args = cmds[i].args ? cmds[i].args : "-";
        printf("CMD\t%s\t%s\t%s\n", cmds[i].name, args, cmds[i].desc);
    }
}

/* ================================================================ */
/* ConPTY backend selection (Windows)                               */
/* ================================================================ */

static int setup_conpty(const char *mode_name, const char *dir, int verbose)
{
    if (strcmp(mode_name, "auto") != 0 &&
        strcmp(mode_name, "bundled") != 0 &&
        strcmp(mode_name, "system") != 0) {
        fprintf(stderr, "Error: --conpty must be auto, bundled or system\n");
        return -1;
    }

#ifdef _WIN32
    CfrConptyMode mode = CFR_CONPTY_AUTO;
    if (strcmp(mode_name, "bundled") == 0)
        mode = CFR_CONPTY_BUNDLED;
    else if (strcmp(mode_name, "system") == 0)
        mode = CFR_CONPTY_SYSTEM;

    pty_set_verbose(verbose);
    if (pty_set_conpty(mode, dir) != 0) {
        fprintf(stderr, "Error: %s\n", pty_conpty_error());
        return -1;
    }
    if (verbose) {
        if (strcmp(pty_conpty_backend(), "bundled") == 0) {
            fprintf(stderr, "coffer: ConPTY backend: bundled %s\n",
                    pty_conpty_version() ? pty_conpty_version() : "");
            fprintf(stderr, "coffer: conpty.dll: %s (%s)\n",
                    pty_conpty_dll_path() ? pty_conpty_dll_path() : "?",
                    pty_conpty_dll_version() ? pty_conpty_dll_version() : "?");
            fprintf(stderr, "coffer: OpenConsole.exe: %s (%s)\n",
                    pty_conpty_host_path() ? pty_conpty_host_path() : "?",
                    pty_conpty_host_version() ? pty_conpty_host_version()
                                              : "?");
        } else {
            fprintf(stderr,
                    "coffer: ConPTY backend: system conhost.exe"
                    " (DCS/APC may be stripped)\n");
        }
    }
#else
    (void)dir;
    (void)verbose;
#endif
    return 0;
}

static void print_conpty_info(void)
{
#ifdef _WIN32
    printf("backend=%s\n", pty_conpty_backend());
    printf("version=%s\n", pty_conpty_version() ? pty_conpty_version() : "?");
    printf("dll=%s\n", pty_conpty_dll_path() ? pty_conpty_dll_path() : "");
    printf("dll_version=%s\n",
           pty_conpty_dll_version() ? pty_conpty_dll_version() : "");
    printf("openconsole=%s\n",
           pty_conpty_host_path() ? pty_conpty_host_path() : "");
    printf("openconsole_version=%s\n",
           pty_conpty_host_version() ? pty_conpty_host_version() : "");
    printf("passthrough=%s\n", pty_conpty_has_openconsole() ? "yes" : "no");
#else
    printf("backend=system\n");
    printf("version=forkpty\n");
    printf("passthrough=no\n");
#endif
}

/* ================================================================ */
/* PTY abstraction                                                  */
/* ================================================================ */

/* ---- POSIX PTY (forkpty + poll) ----
 *
 * Windows uses src/pty_w32.c (shared with test_cfr_pty) and declares the
 * same entry points in src/pty.h. */

#ifndef _WIN32

typedef struct PtyCtx PtyCtx;

PtyCtx *pty_spawn(int rows, int cols, char *const argv[], char *const env[],
                  int n_env);
void pty_kill(PtyCtx *p);
ssize_t pty_write(PtyCtx *p, const char *data, size_t len);
ssize_t pty_read(PtyCtx *p, char *buf, size_t bufsize);
int pty_get_fd(PtyCtx *p);
/* Resize the PTY (the child's window size); the grid is resized by the
 * caller. POSIX: TIOCSWINSZ, which also SIGWINCHes the foreground group.
 * Returns 0 on success. */
int pty_resize(PtyCtx *p, int rows, int cols);
/* Non-blocking child check. Returns 1 when the child has exited (with
 * *out_status set: the exit code, or 128+signal on POSIX), 0 while it
 * still runs, -1 on error. A reaped child stays "exited" for later
 * calls. */
int pty_poll_exit(PtyCtx *p, int *out_status);

struct PtyCtx
{
    int master_fd;
    char slave_name[256]; /* the slave's /dev path, for resize ioctls */
    pid_t child_pid;      /* 0 once reaped */
    int rows, cols;
};

/* Apply a `KEY=VALUE` override in the child (empty VALUE unsets). The
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

PtyCtx *pty_spawn(int rows, int cols, char *const argv[], char *const env[],
                  int n_env)
{
    PtyCtx *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->rows = rows;
    p->cols = cols;
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
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        setenv("TERM_PROGRAM", "ghostty", 1);
        for (int i = 0; i < n_env; i++)
            apply_env_override(env[i]);
        if (argv && argv[0])
            execvp(argv[0], argv);
        else
            execlp(getenv("SHELL") ? getenv("SHELL") : "/bin/sh",
                   "-sh", (char *)NULL);
        _exit(127);
    }
    p->child_pid = pid;
    return p;
}

void pty_kill(PtyCtx *p)
{
    if (!p)
        return;
    if (p->master_fd >= 0)
        close(p->master_fd);
    if (p->child_pid > 0) {
        kill(p->child_pid, SIGHUP);
        usleep(100000);
        kill(p->child_pid, SIGKILL);
        waitpid(p->child_pid, NULL, 0);
    }
    free(p);
}

ssize_t pty_write(PtyCtx *p, const char *data, size_t len)
{
    if (!p || p->master_fd < 0)
        return -1;
    return write(p->master_fd, data, len);
}

ssize_t pty_read(PtyCtx *p, char *buf, size_t bufsize)
{
    if (!p || p->master_fd < 0)
        return -1;
    return read(p->master_fd, buf, bufsize);
}

int pty_get_fd(PtyCtx *p)
{
    return p ? p->master_fd : -1;
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
     * reads that one. The master is a different tty struct: an ioctl on
     * it still signals SIGWINCH, which makes a master-only resize look
     * like it worked while the child keeps the old size. Open the slave
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

#endif /* !_WIN32 */

/* ================================================================ */
/* Time                                                             */
/* ================================================================ */

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

/* ================================================================ */
/* Drain PTY into coffer                                            */
/* ================================================================ */

static PtyCtx *g_pty;
static FILE *g_raw_out;

static void cb_output(const uint8_t *bytes, size_t len, void *user)
{
    (void)user;
    if (g_pty)
        pty_write(g_pty, (const char *)bytes, len);
}

static void drain(CfrTerm *vt, PtyCtx *pty, double timeout_sec)
{
    char buf[65536];
    double deadline = now_sec() + timeout_sec;

    while (now_sec() < deadline) {
        double remaining = deadline - now_sec();
        if (remaining <= 0)
            break;

#ifdef _WIN32
        HANDLE data_ev = (HANDLE)pty_get_data_event(pty);
        if (!data_ev || data_ev == INVALID_HANDLE_VALUE)
            break;

        DWORD wait_ms = (DWORD)(remaining * 1000);
        DWORD r = WaitForSingleObject(data_ev, wait_ms);

        if (r == WAIT_FAILED || r == WAIT_TIMEOUT)
            break;

        /* Reader thread signaled — drain all available data */
        ssize_t n;
        while ((n = pty_read(pty, buf, sizeof(buf))) > 0) {
            if (g_raw_out) {
                fwrite(buf, 1, (size_t)n, g_raw_out);
                fflush(g_raw_out);
            }
            cfr_input_write(vt, (const uint8_t *)buf, (size_t)n);
        }

        if (pty_is_eof(pty))
            break;
#else
        int fd = pty_get_fd(pty);
        if (fd < 0)
            break;

        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int wait_ms = (int)(remaining * 1000);
        if (wait_ms <= 0)
            wait_ms = 1;
        int r = poll(&pfd, 1, wait_ms);
        if (r <= 0)
            break;
        if (pfd.revents & POLLIN) {
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n <= 0)
                break;
            if (g_raw_out) {
                fwrite(buf, 1, (size_t)n, g_raw_out);
                fflush(g_raw_out);
            }
            cfr_input_write(vt, (const uint8_t *)buf, (size_t)n);
        }
        if (pfd.revents & (POLLHUP | POLLERR))
            break;
#endif
    }
}

/* ================================================================ */
/* Script engine host hooks                                           */
/* ================================================================ */

typedef struct
{
    CfrTerm *vt;
    PtyCtx *pty;
} HostCtx;

static void host_pty_write(void *ud, const char *data, size_t len)
{
    HostCtx *hc = ud;
    pty_write(hc->pty, data, len);
}

static void host_emit(void *ud, const char *data, size_t len)
{
    HostCtx *hc = ud;
    cfr_input_write(hc->vt, (const uint8_t *)data, len);
}

static int host_poll_exit(void *ud, int *status)
{
    HostCtx *hc = ud;
    return pty_poll_exit(hc->pty, status);
}

static int host_resize(void *ud, int cols, int rows)
{
    HostCtx *hc = ud;
    if (!hc->pty)
        return -1;
    /* TIOCSWINSZ on the slave (SIGWINCHes the child), then the grid. */
    if (pty_resize(hc->pty, rows, cols) != 0)
        return -1;
    cfr_resize(hc->vt, rows, cols);
    return 0;
}

/* Blocking pump: drain the PTY into coffer for up to max_ms. With no
 * child (a --feed run) there is nothing to drain: the wait is the
 * clock's alone. */
static void host_pump(void *ud, uint32_t max_ms)
{
    HostCtx *hc = ud;
    if (!hc->pty) {
#ifdef _WIN32
        Sleep(max_ms);
#else
        struct timespec ts = { (time_t)(max_ms / 1000),
                               (long)(max_ms % 1000) * 1000000L };
        nanosleep(&ts, NULL);
#endif
        return;
    }
    drain(hc->vt, hc->pty, max_ms / 1000.0);
}

/* Read `path` whole and feed it into the VT parser as terminal output.
 * The replay is byte-exact and chunked, so a capture of a live session
 * reaches the parser in the same order (its state machine is
 * incremental either way). Returns 0 on success. */
static int feed_file(CfrTerm *vt, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return -1;
    }
    uint8_t buf[65536];
    size_t n;
    int rc = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        cfr_input_write(vt, buf, n);
    if (ferror(f)) {
        perror(path);
        rc = -1;
    }
    fclose(f);
    return rc;
}

static int run_script(HostCtx *hc, const char *script_path)
{
    char err[512];
    CfrScript *script = cfr_script_load(script_path, err, sizeof(err),
                                        NULL);
    if (!script) {
        fprintf(stderr, "%s: %s\n", script_path, err);
        return 1;
    }

    CfrScriptHost host = {
        .term = hc->vt,
        /* No child, no PTY side: send/raw warn and skip, await-exit has
         * nothing to await, resize fails. A --feed run is the VT's. */
        .pty_write = hc->pty ? host_pty_write : NULL,
        .emit = host_emit,
        .poll_exit = hc->pty ? host_poll_exit : NULL,
        .resize = hc->pty ? host_resize : NULL,
        .ud = hc,
    };
    CfrScriptRun *run = cfr_script_run_new(script, &host);
    if (!run) {
        cfr_script_free(script);
        return 1;
    }
    CfrScriptStatus st = cfr_script_run_blocking(run, host_pump, hc);
    cfr_script_run_free(run);
    cfr_script_free(script);
    return st == CFR_SCRIPT_FAILED ? 1 : 0;
}

/* ================================================================ */
/* Final dump (CLI -s/-d/-S/-q flags)                                 */
/* ================================================================ */

static void dump_one_row_text(CfrTerm *vt, int row)
{
    /* Sized by the row's own width: history can be wider than the grid. */
    size_t cap = cfr_row_text_size(vt, row);
    if (cap == 0)
        cap = 1; /* no such row: prints an empty line */
    char *buf = malloc(cap);
    if (!buf)
        return;
    cfr_row_text(vt, row, buf, cap);
    printf("%2d|%s\n", row, buf);
    free(buf);
}

static void final_dump(CfrTerm *vt, const int *show_rows, int n_show,
                       int show_cells, int show_styles, int show_kbd,
                       int quiet)
{
    int rows = 0, cols = 0;
    cfr_get_dimensions(vt, &rows, &cols);
    if (show_kbd)
        cfr_script_dump(vt, stdout, CFR_SCRIPT_DUMP_KBD, 0, 0, 0);

    if (quiet && n_show > 0) {
        /* Only the requested rows. */
        for (int i = 0; i < n_show; i++) {
            int r = show_rows[i];
            dump_one_row_text(vt, r);
            if (show_cells)
                cfr_script_dump(vt, stdout, CFR_SCRIPT_DUMP_CELLS, r,
                                0, -1);
            if (show_styles)
                cfr_script_dump(vt, stdout, CFR_SCRIPT_DUMP_STYLES, r,
                                0, 0);
        }
        return;
    }

    cfr_script_dump(vt, stdout, CFR_SCRIPT_DUMP_VISIBLE, 0, 0, 0);
    if (show_cells || show_styles) {
        for (int r = 0; r < rows; r++) {
            if (n_show > 0) {
                int found = 0;
                for (int i = 0; i < n_show; i++)
                    if (show_rows[i] == r)
                        found = 1;
                if (!found)
                    continue;
            }
            if (show_cells)
                cfr_script_dump(vt, stdout, CFR_SCRIPT_DUMP_CELLS, r,
                                0, -1);
            if (show_styles)
                cfr_script_dump(vt, stdout, CFR_SCRIPT_DUMP_STYLES, r,
                                0, 0);
        }
    }
}

/* ================================================================ */
/* Main                                                             */
/* ================================================================ */

int main(int argc, char *argv[])
{
    int rows = 24, cols = 80;
    double initial_wait = 3.0;
    const char *script_path = NULL;
    const char *outfile = NULL;
    const char *feed_path = NULL;
    int show_rows[64];
    int n_show = 0;
    int show_cells = 0, show_styles = 0, show_kbd = 0, quiet = 0;
    const char *envs[64];
    int n_env = 0;
    const char *conpty_mode = "auto";
    const char *conpty_dir = NULL;
    int conpty_info = 0;
    int verbose = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help-spec") == 0) {
            print_help_spec();
            return 0;
        }
    }

    int cmd_start = argc;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            cmd_start = i + 1;
            break;
        }
        const char *arg = argv[i];
        int fi = find_flag(arg);

        if (fi < 0) {
            fprintf(stderr, "Unknown option: %s\n", arg);
            fprintf(stderr, "Try 'coffer --help' for more information.\n");
            return 1;
        }

        const FlagDef *f = &OPTIONS[fi];

        if (strcmp(f->name, "--help") == 0) {
            print_help();
            return 0;
        }

        if (f->takes_value) {
            if (i + 1 >= cmd_start) {
                fprintf(stderr, "Error: %s requires a value\n", arg);
                return 1;
            }
            const char *val = argv[++i];

            if (strcmp(f->name, "--rows") == 0)
                rows = atoi(val);
            else if (strcmp(f->name, "--cols") == 0)
                cols = atoi(val);
            else if (strcmp(f->name, "--wait") == 0)
                initial_wait = atof(val);
            else if (strcmp(f->name, "--script") == 0)
                script_path = val;
            else if (strcmp(f->name, "--output") == 0)
                outfile = val;
            else if (strcmp(f->name, "--feed") == 0)
                feed_path = val;
            else if (strcmp(f->name, "--show-row") == 0 && n_show < 64)
                show_rows[n_show++] = atoi(val);
            else if (strcmp(f->name, "--env") == 0) {
                if (n_env < 64)
                    envs[n_env++] = val;
                else
                    fprintf(stderr, "Too many --env overrides (max 64)\n");
            } else if (strcmp(f->name, "--conpty") == 0)
                conpty_mode = val;
            else if (strcmp(f->name, "--conpty-dir") == 0)
                conpty_dir = val;
        } else {
            if (strcmp(f->name, "--cell-data") == 0)
                show_cells = 1;
            else if (strcmp(f->name, "--style-data") == 0)
                show_styles = 1;
            else if (strcmp(f->name, "--kbd-data") == 0)
                show_kbd = 1;
            else if (strcmp(f->name, "--quiet") == 0)
                quiet = 1;
            else if (strcmp(f->name, "--conpty-info") == 0)
                conpty_info = 1;
            else if (strcmp(f->name, "--verbose") == 0)
                verbose = 1;
        }
    }

    if (setup_conpty(conpty_mode, conpty_dir, verbose) != 0)
        return 1;
    if (conpty_info) {
        print_conpty_info();
        return 0;
    }

    char *const *cmd_argv =
        (cmd_start < argc) ? &argv[cmd_start] : NULL;

    if (outfile) {
        g_raw_out = fopen(outfile, "wb");
        if (!g_raw_out)
            perror("fopen");
    }

    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.rows = rows;
    cfg.cols = cols;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 6;
    CfrTerm *vt = cfr_new(&cfg);
    if (!vt) {
        fprintf(stderr, "cfr_new failed\n");
        return 1;
    }

    /* A --feed run is a replay: the parser gets the file's bytes and no
     * child is spawned unless the command line asks for one. */
    int spawn = !feed_path || cmd_argv != NULL;

    PtyCtx *pty = NULL;
    if (spawn) {
        pty = pty_spawn(rows, cols, cmd_argv, (char *const *)envs, n_env);
        if (!pty) {
            cfr_free(vt);
            return 1;
        }
        g_pty = pty;
    }

    CfrCallbacks cb = { .output = cb_output };
    cfr_set_callbacks(vt, &cb, NULL);

    int rc = 0;
    if (feed_path && feed_file(vt, feed_path) != 0) {
        rc = 1;
    } else if (spawn) {
        fprintf(stderr, "Waiting %.1fs for startup...\n", initial_wait);
        drain(vt, pty, initial_wait);
    }

    HostCtx hc = { .vt = vt, .pty = pty };

    if (rc == 0 && script_path)
        rc = run_script(&hc, script_path);

    /* Final dump if no script ran, or the script passed */
    if (rc == 0)
        final_dump(vt, show_rows, n_show, show_cells, show_styles, show_kbd,
                   quiet);

    if (outfile) {
        fclose(g_raw_out);
        fprintf(stderr, "Raw output saved to %s\n", outfile);
    }

    g_pty = NULL;
    if (pty)
        pty_kill(pty);
    cfr_free(vt);
    return rc;
}
