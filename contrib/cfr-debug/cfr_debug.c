/*
 * cfr-debug — headless coffer PTY inspector.
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

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
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
    { "--env", NULL, 1, "KEY=VAL",
      "Set a child env var (repeatable; applied after the built-ins; "
      "an empty value unsets it)" },
    { "--help", "-h", 0, NULL, "Show this help" },
    { NULL, NULL, 0, NULL, NULL }
};

/* Script vocabulary, printed by --help-spec as machine-readable CMD
 * lines (one per command) and by --help as the human listing. A driver
 * that needs `wait-for` greps the spec for "CMD\twait-for" instead of
 * guessing from a version number. */
typedef struct
{
    const char *name;
    const char *args; /* NULL for no arguments */
    const char *desc;
} ScriptCmdDef;

static const ScriptCmdDef SCRIPT_COMMANDS[] = {
    { "wait", "SECONDS", "Drain PTY for N seconds" },
    { "wait-for", "\"TEXT\" [TIMEOUT] [line-end]",
      "Drain until TEXT appears; TIMEOUT (default 600) => fail" },
    { "send", "TEXT", "Write text to the child's stdin (escapes: \\n \\r \\t \\e \\\\)" },
    { "raw", "HEX [HEX ...]", "Write raw bytes to the child's stdin" },
    { "assert-contains", "TEXT", "Fail if the unified buffer lacks TEXT" },
    { "assert-not-contains", "TEXT", "Fail if the unified buffer contains TEXT" },
    { "await-exit", "[TIMEOUT]",
      "Drain until the child exits (default 10s); timeout => fail; records status" },
    { "assert-exit", "STATUS", "Fail unless await-exit recorded STATUS" },
    { "resize", "COLS ROWS", "Resize the PTY and the grid (SIGWINCH on POSIX)" },
    { "render", "[visible|scrollback|all]", "Dump the grid (default: visible rows)" },
    { "cursor", NULL, "Dump cursor state (row, col, visible, blink)" },
    { NULL, NULL, NULL }
};

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
    printf("cfr-debug — headless coffer PTY inspector\n\n");
    printf("Usage: cfr-debug [options] [-- command args...]\n\n");
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
    for (int i = 0; SCRIPT_COMMANDS[i].name; i++) {
        char label[80];
        if (SCRIPT_COMMANDS[i].args)
            snprintf(label, sizeof(label), "%s %s", SCRIPT_COMMANDS[i].name,
                     SCRIPT_COMMANDS[i].args);
        else
            snprintf(label, sizeof(label), "%s", SCRIPT_COMMANDS[i].name);
        printf("  %-30s  %s\n", label, SCRIPT_COMMANDS[i].desc);
    }
    printf("\n  send/raw write to the PTY INPUT (the child's stdin), never to\n"
           "  coffer's parser. To emit escape sequences the terminal processes,\n"
           "  have the child print them (e.g. printf '\\033[?12l').\n"
           "  assert-* and wait-for search the UNIFIED buffer: scrollback\n"
           "  (oldest first) then the visible rows.\n");
    printf("\nExamples:\n");
    printf("  cfr-debug -- bash -l\n");
    printf("  cfr-debug -c 68 -r 20 -w 5 -- crush\n");
    printf("  cfr-debug -r 24 -c 80 -f test.script -- mudlark carrionfields.net 4449\n");
    printf("\nRun 'cfr-debug --help-spec' for machine-readable flag listing.\n");
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
    for (int i = 0; SCRIPT_COMMANDS[i].name; i++) {
        const char *args = SCRIPT_COMMANDS[i].args ? SCRIPT_COMMANDS[i].args : "-";
        printf("CMD\t%s\t%s\t%s\n", SCRIPT_COMMANDS[i].name, args,
               SCRIPT_COMMANDS[i].desc);
    }
}

/* ================================================================ */
/* PTY abstraction                                                  */
/* ================================================================ */

typedef struct PtyCtx PtyCtx;

PtyCtx *pty_spawn(int rows, int cols, char *const argv[], char *const env[],
                  int n_env);
void pty_kill(PtyCtx *p);
ssize_t pty_write(PtyCtx *p, const char *data, size_t len);
ssize_t pty_read(PtyCtx *p, char *buf, size_t bufsize);
int pty_get_fd(PtyCtx *p);
/* Resize the PTY (the child's window size); the grid is resized by the
 * caller. POSIX: TIOCSWINSZ, which also SIGWINCHes the foreground group.
 * Windows: ResizePseudoConsole. Returns 0 on success. */
int pty_resize(PtyCtx *p, int rows, int cols);
/* Non-blocking child check. Returns 1 when the child has exited (with
 * *out_status set: the exit code, or 128+signal on POSIX), 0 while it
 * still runs, -1 on error. A reaped child stays "exited" for later
 * calls. */
int pty_poll_exit(PtyCtx *p, int *out_status);
#ifdef _WIN32
void *pty_get_data_event(PtyCtx *p);
int pty_is_eof(PtyCtx *p);
#endif

/* ---- POSIX PTY (forkpty + poll) ---- */

#ifndef _WIN32

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

/* ---- Windows PTY (ConPTY) ---- */

#ifdef _WIN32

struct PtyCtx
{
    HPCON hpc;
    HANDLE input_write;
    HANDLE output_read;
    HANDLE process;
    HANDLE thread;
    HANDLE waiter_thread;
    HANDLE reader_thread;
    HANDLE data_event;     /* signaled when reader has data */
    HANDLE reader_done;    /* signaled when reader thread exits */
    CRITICAL_SECTION lock; /* protects buf, buf_len, buf_eof */
    char buf[65536];
    size_t buf_len;
    int buf_eof;
    int reaped; /* child status already collected */
    int rows, cols;
};

static DWORD WINAPI pty_waiter_thread(LPVOID param)
{
    PtyCtx *ctx = (PtyCtx *)param;
    WaitForSingleObject(ctx->process, INFINITE);
    if (ctx->hpc != INVALID_HANDLE_VALUE) {
        ClosePseudoConsole(ctx->hpc);
        ctx->hpc = INVALID_HANDLE_VALUE;
    }
    return 0;
}

/* Reader thread: continuously reads from the ConPTY output pipe.
 * Anonymous pipes can't be used with WaitForMultipleObjects, so a
 * background thread does blocking ReadFile and signals data_event
 * when new data arrives. */
static DWORD WINAPI pty_reader_thread(LPVOID param)
{
    PtyCtx *ctx = (PtyCtx *)param;
    char buf[65536];

    while (1) {
        DWORD got = 0;
        if (!ReadFile(ctx->output_read, buf, sizeof(buf), &got, NULL)) {
            DWORD err = GetLastError();
            if (got > 0) {
                EnterCriticalSection(&ctx->lock);
                size_t avail = sizeof(ctx->buf) - ctx->buf_len;
                size_t copy = got < avail ? got : avail;
                memcpy(ctx->buf + ctx->buf_len, buf, copy);
                ctx->buf_len += copy;
                SetEvent(ctx->data_event);
                LeaveCriticalSection(&ctx->lock);
            }
            if (err == ERROR_BROKEN_PIPE || err == ERROR_HANDLE_EOF)
                ctx->buf_eof = 1;
            break;
        }
        if (got == 0)
            break;
        EnterCriticalSection(&ctx->lock);
        size_t avail = sizeof(ctx->buf) - ctx->buf_len;
        size_t copy = got < avail ? got : avail;
        memcpy(ctx->buf + ctx->buf_len, buf, copy);
        ctx->buf_len += copy;
        SetEvent(ctx->data_event);
        LeaveCriticalSection(&ctx->lock);
    }

    SetEvent(ctx->data_event);
    SetEvent(ctx->reader_done);
    return 0;
}

PtyCtx *pty_spawn(int rows, int cols, char *const argv[], char *const env[],
                  int n_env)
{
    PtyCtx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;
    ctx->rows = rows;
    ctx->cols = cols;
    ctx->hpc = INVALID_HANDLE_VALUE;
    ctx->input_write = INVALID_HANDLE_VALUE;
    ctx->output_read = INVALID_HANDLE_VALUE;
    ctx->process = INVALID_HANDLE_VALUE;
    ctx->thread = INVALID_HANDLE_VALUE;
    ctx->waiter_thread = INVALID_HANDLE_VALUE;
    ctx->reader_thread = INVALID_HANDLE_VALUE;
    ctx->data_event = INVALID_HANDLE_VALUE;
    ctx->reader_done = INVALID_HANDLE_VALUE;
    InitializeCriticalSection(&ctx->lock);

    HANDLE input_read = INVALID_HANDLE_VALUE;
    HANDLE output_write = INVALID_HANDLE_VALUE;

    if (!CreatePipe(&input_read, &ctx->input_write, NULL, 0)) {
        fprintf(stderr, "ERROR: CreatePipe (input) failed: %lu\n",
                GetLastError());
        goto fail;
    }

    if (!CreatePipe(&ctx->output_read, &output_write, NULL, 0)) {
        fprintf(stderr, "ERROR: CreatePipe (output) failed: %lu\n",
                GetLastError());
        goto fail;
    }

    COORD size;
    size.X = (SHORT)cols;
    size.Y = (SHORT)rows;

    HRESULT hr = CreatePseudoConsole(size, input_read, output_write, 0,
                                     &ctx->hpc);
    if (FAILED(hr)) {
        fprintf(stderr, "ERROR: CreatePseudoConsole failed: 0x%lx\n",
                (unsigned long)hr);
        goto fail;
    }

    CloseHandle(input_read);
    input_read = INVALID_HANDLE_VALUE;
    CloseHandle(output_write);
    output_write = INVALID_HANDLE_VALUE;

    /* STARTUPINFOEX with pseudo-console attribute */
    STARTUPINFOEXW si;
    ZeroMemory(&si, sizeof(si));
    si.StartupInfo.cb = sizeof(STARTUPINFOEXW);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = INVALID_HANDLE_VALUE;
    si.StartupInfo.hStdOutput = INVALID_HANDLE_VALUE;
    si.StartupInfo.hStdError = INVALID_HANDLE_VALUE;

    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
    si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attr_size);
    if (!si.lpAttributeList)
        goto fail;

    if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0,
                                           &attr_size)) {
        free(si.lpAttributeList);
        goto fail;
    }

    if (!UpdateProcThreadAttribute(
            si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
            ctx->hpc, sizeof(HPCON), NULL, NULL)) {
        DeleteProcThreadAttributeList(si.lpAttributeList);
        free(si.lpAttributeList);
        goto fail;
    }

    /* Build command line */
    WCHAR cmdline[MAX_PATH * 2];
    if (argv && argv[0]) {
        const char *ext = strrchr(argv[0], '.');
        int is_cmd_script =
            ext && (_stricmp(ext, ".cmd") == 0 ||
                    _stricmp(ext, ".bat") == 0);

        WCHAR *p = cmdline;
        if (is_cmd_script) {
            const char *comspec = getenv("COMSPEC");
            if (!comspec)
                comspec = "cmd.exe";
            MultiByteToWideChar(CP_UTF8, 0, comspec, -1, p, MAX_PATH * 2);
            p += wcslen(p);
            wcscpy(p, L" /c ");
            p += 4;
        }

        MultiByteToWideChar(CP_UTF8, 0, argv[0], -1, p,
                            (MAX_PATH * 2) - (p - cmdline));
        p += wcslen(p);
        for (int i = 1; argv[i]; i++) {
            *p++ = L' ';
            MultiByteToWideChar(CP_UTF8, 0, argv[i], -1, p,
                                (MAX_PATH * 2) - (p - cmdline));
            p += wcslen(p);
        }
    } else {
        const char *comspec = getenv("COMSPEC");
        if (!comspec)
            comspec = "cmd.exe";
        MultiByteToWideChar(CP_UTF8, 0, comspec, -1, cmdline, MAX_PATH * 2);
    }

    /* Build the environment block. First occurrence wins in a Windows
     * environment block, so the order is: --env overrides (empty value =
     * unset), the built-ins an override did not name, then the parent's
     * variables minus every key already decided above. */
    enum
    {
        MAX_OV = 32,
        OV_KEY_MAX = 128,
        OV_VAL_MAX = 512
    };
    WCHAR ov_key[MAX_OV][OV_KEY_MAX]; /* the KEY of each --env override */
    WCHAR ov_val[MAX_OV][OV_VAL_MAX]; /* its value ("" = unset) */
    int n_ov = 0;
    for (int i = 0; i < n_env && n_ov < MAX_OV; i++) {
        const char *eq = strchr(env[i], '=');
        if (!eq)
            continue;
        size_t klen = (size_t)(eq - env[i]);
        if (klen == 0 || klen >= OV_KEY_MAX)
            continue;
        int n = MultiByteToWideChar(CP_UTF8, 0, env[i], (int)klen,
                                    ov_key[n_ov], OV_KEY_MAX);
        if (n <= 0)
            continue;
        ov_key[n_ov][n] = L'\0';
        ov_val[n_ov][0] = L'\0';
        if (eq[1] != '\0' &&
            MultiByteToWideChar(CP_UTF8, 0, eq + 1, -1, ov_val[n_ov],
                                OV_VAL_MAX) <= 0)
            continue; /* conversion failure: drop the override */
        n_ov++;
    }

    static const WCHAR *builtins[] = { L"TERM=xterm-256color",
                                       L"COLORTERM=truecolor",
                                       L"TERM_PROGRAM=ghostty" };
    const int n_builtins = 3;

    /* Does `entry` ("KEY=...") name `key`? (Windows env names are
     * case-insensitive.) */
#define ENV_KEY_IS(entry, key)                                   \
    (wcslen(key) == (size_t)(wcschr((entry), L'=') - (entry)) && \
     _wcsnicmp((entry), (key), wcslen(key)) == 0)

    LPWCH parent_env = GetEnvironmentStringsW();
    WCHAR envBlock[65536];
    WCHAR *ep = envBlock;

    /* 1. The overrides that set a value. */
    for (int i = 0; i < n_ov; i++) {
        if (ov_val[i][0] == L'\0')
            continue;
        size_t len = wcslen(ov_key[i]) + 1 + wcslen(ov_val[i]);
        if (ep + len + 1 >= envBlock + 65536)
            break;
        wcscpy(ep, ov_key[i]);
        wcscat(ep, L"=");
        wcscat(ep, ov_val[i]);
        ep += len + 1;
    }

    /* 2. The built-ins an override did not name. */
    for (int i = 0; i < n_builtins; i++) {
        int decided = 0;
        for (int j = 0; j < n_ov && !decided; j++)
            decided = ENV_KEY_IS(builtins[i], ov_key[j]);
        if (decided)
            continue;
        size_t len = wcslen(builtins[i]);
        if (ep + len + 1 >= envBlock + 65536)
            break;
        wmemcpy(ep, builtins[i], len + 1);
        ep += len + 1;
    }

    /* 3. The parent's variables minus every key decided above. */
    if (parent_env) {
        LPWCH p = parent_env;
        while (*p) {
            size_t len = wcslen(p);
            int decided = 0;
            for (int i = 0; i < n_ov && !decided; i++)
                decided = ENV_KEY_IS(p, ov_key[i]);
            for (int i = 0; i < n_builtins && !decided; i++)
                decided = ENV_KEY_IS(p, builtins[i]);
            if (!decided) {
                if (ep + len + 1 >= envBlock + 65536)
                    break;
                wmemcpy(ep, p, len + 1);
                ep += len + 1;
            }
            p += len + 1;
        }
        FreeEnvironmentStringsW(parent_env);
    }
    *ep = L'\0';
#undef ENV_KEY_IS

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(NULL, cmdline, NULL, NULL, FALSE,
                        EXTENDED_STARTUPINFO_PRESENT |
                            CREATE_UNICODE_ENVIRONMENT,
                        envBlock, NULL, &si.StartupInfo, &pi)) {
        fprintf(stderr, "ERROR: CreateProcessW failed: %lu\n",
                GetLastError());
        DeleteProcThreadAttributeList(si.lpAttributeList);
        free(si.lpAttributeList);
        goto fail;
    }

    ctx->process = pi.hProcess;
    ctx->thread = pi.hThread;

    DeleteProcThreadAttributeList(si.lpAttributeList);
    free(si.lpAttributeList);

    ctx->waiter_thread =
        CreateThread(NULL, 0, pty_waiter_thread, ctx, 0, NULL);

    /* Create events and start reader thread */
    ctx->data_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    ctx->reader_done = CreateEvent(NULL, TRUE, FALSE, NULL);
    ctx->reader_thread =
        CreateThread(NULL, 0, pty_reader_thread, ctx, 0, NULL);

    return ctx;

fail:
    if (input_read != INVALID_HANDLE_VALUE)
        CloseHandle(input_read);
    if (output_write != INVALID_HANDLE_VALUE)
        CloseHandle(output_write);
    if (ctx->input_write != INVALID_HANDLE_VALUE)
        CloseHandle(ctx->input_write);
    if (ctx->output_read != INVALID_HANDLE_VALUE)
        CloseHandle(ctx->output_read);
    if (ctx->hpc != INVALID_HANDLE_VALUE)
        ClosePseudoConsole(ctx->hpc);
    free(ctx);
    return NULL;
}

void pty_kill(PtyCtx *ctx)
{
    if (!ctx)
        return;

    /* Close pseudo-console first — this unblocks the reader thread's
     * ReadFile (pipe breaks) and the waiter thread's wait. */
    if (ctx->hpc != INVALID_HANDLE_VALUE) {
        ClosePseudoConsole(ctx->hpc);
        ctx->hpc = INVALID_HANDLE_VALUE;
    }

    if (ctx->waiter_thread != INVALID_HANDLE_VALUE) {
        WaitForSingleObject(ctx->waiter_thread, 2000);
        CloseHandle(ctx->waiter_thread);
    }

    /* Reader thread should exit now that the pipe is broken */
    if (ctx->reader_thread != INVALID_HANDLE_VALUE) {
        WaitForSingleObject(ctx->reader_done, 2000);
        CloseHandle(ctx->reader_thread);
    }

    if (ctx->process != INVALID_HANDLE_VALUE) {
        if (!ctx->reaped &&
            WaitForSingleObject(ctx->process, 500) != WAIT_OBJECT_0) {
            TerminateProcess(ctx->process, 1);
            WaitForSingleObject(ctx->process, 1000);
        }
        CloseHandle(ctx->process);
    }
    if (ctx->thread != INVALID_HANDLE_VALUE)
        CloseHandle(ctx->thread);
    if (ctx->input_write != INVALID_HANDLE_VALUE)
        CloseHandle(ctx->input_write);
    if (ctx->output_read != INVALID_HANDLE_VALUE)
        CloseHandle(ctx->output_read);
    if (ctx->data_event != INVALID_HANDLE_VALUE)
        CloseHandle(ctx->data_event);
    if (ctx->reader_done != INVALID_HANDLE_VALUE)
        CloseHandle(ctx->reader_done);

    DeleteCriticalSection(&ctx->lock);
    free(ctx);
}

ssize_t pty_write(PtyCtx *ctx, const char *data, size_t len)
{
    if (!ctx || ctx->input_write == INVALID_HANDLE_VALUE || !data || len == 0)
        return -1;

    size_t total = 0;
    while (total < len) {
        DWORD written = 0;
        DWORD chunk =
            (DWORD)((len - total) > 0x7FFFFFFFu ? 0x7FFFFFFFu
                                                : (len - total));
        if (!WriteFile(ctx->input_write, data + total, chunk, &written, NULL))
            return total > 0 ? (ssize_t)total : -1;
        if (written == 0)
            break;
        total += (size_t)written;
    }
    return (ssize_t)total;
}

ssize_t pty_read(PtyCtx *ctx, char *buf, size_t bufsize)
{
    if (!ctx || !buf || !bufsize)
        return -1;

    /* On Windows, the reader thread owns the blocking ReadFile. We
     * drain its accumulated buffer under the lock. */
    EnterCriticalSection(&ctx->lock);
    size_t to_copy = ctx->buf_len < bufsize ? ctx->buf_len : bufsize;
    memcpy(buf, ctx->buf, to_copy);
    if (to_copy < ctx->buf_len)
        memmove(ctx->buf, ctx->buf + to_copy, ctx->buf_len - to_copy);
    ctx->buf_len -= to_copy;
    LeaveCriticalSection(&ctx->lock);
    return (ssize_t)to_copy;
}

void *pty_get_data_event(PtyCtx *ctx)
{
    if (!ctx)
        return NULL;
    return (void *)ctx->data_event;
}

int pty_is_eof(PtyCtx *ctx)
{
    if (!ctx)
        return 1;
    EnterCriticalSection(&ctx->lock);
    int eof = ctx->buf_eof && ctx->buf_len == 0;
    LeaveCriticalSection(&ctx->lock);
    return eof;
}

int pty_get_fd(PtyCtx *ctx)
{
    (void)ctx;
    return -1;
}

int pty_resize(PtyCtx *ctx, int rows, int cols)
{
    if (!ctx || ctx->hpc == INVALID_HANDLE_VALUE)
        return -1;
    COORD size;
    size.X = (SHORT)cols;
    size.Y = (SHORT)rows;
    HRESULT hr = ResizePseudoConsole(ctx->hpc, size);
    if (FAILED(hr)) {
        /* wine returns E_NOTIMPL (0x80004001) here; real Windows
         * (10 1809+) resizes the console. */
        fprintf(stderr, "resize: ResizePseudoConsole failed: 0x%08lx\n",
                (unsigned long)hr);
        return -1;
    }
    ctx->rows = rows;
    ctx->cols = cols;
    return 0;
}

int pty_poll_exit(PtyCtx *ctx, int *out_status)
{
    if (!ctx)
        return -1;
    if (ctx->reaped)
        return 1;
    if (ctx->process == INVALID_HANDLE_VALUE)
        return -1;
    if (WaitForSingleObject(ctx->process, 0) != WAIT_OBJECT_0)
        return 0;
    DWORD code = 0;
    if (GetExitCodeProcess(ctx->process, &code) && out_status)
        *out_status = (int)code;
    ctx->reaped = 1;
    return 1;
}

#endif /* _WIN32 */

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
/* Grid rendering                                                   */
/* ================================================================ */

static void utf8_put(uint32_t cp)
{
    if (cp < 0x80)
        putchar((int)cp);
    else if (cp < 0x800) {
        putchar(0xC0 | (cp >> 6));
        putchar(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        putchar(0xE0 | (cp >> 12));
        putchar(0x80 | ((cp >> 6) & 0x3F));
        putchar(0x80 | (cp & 0x3F));
    } else {
        putchar(0xF0 | (cp >> 18));
        putchar(0x80 | ((cp >> 12) & 0x3F));
        putchar(0x80 | ((cp >> 6) & 0x3F));
        putchar(0x80 | (cp & 0x3F));
    }
}

static void render_row_text(CfrTerm *vt, int row, int cols)
{
    for (int c = 0; c < cols; c++) {
        const CfrCell *cell = cfr_get_cell(vt, row, c);
        if (!cell || cell->cp == 0 || cell->cp == 0x20) {
            putchar(' ');
            continue;
        }
        utf8_put(cell->cp);
        if (cell->width == 2)
            c++;
    }
    putchar('\n');
}

static void render_row_cells(CfrTerm *vt, int row, int cols)
{
    printf("Row %2d cells:", row);
    for (int c = 0; c < cols; c++) {
        const CfrCell *cell = cfr_get_cell(vt, row, c);
        if (!cell) {
            printf(" [?]");
            continue;
        }
        if (cell->cp == 0 || cell->cp == 0x20)
            printf(" \xc2\xb7");
        else if (cell->cp < 0x80)
            printf(" %c", (char)cell->cp);
        else
            printf(" U+%04X", cell->cp);
        if (cell->width == 2) {
            c++;
            printf("(w2)");
        }
    }
    putchar('\n');
}

static void render_row_styles(CfrTerm *vt, int row, int cols)
{
    printf("Row %2d styles:\n", row);
    for (int c = 0; c < cols; c++) {
        const CfrCell *cell = cfr_get_cell(vt, row, c);
        if (!cell)
            continue;
        const CfrStyle *st = cfr_cell_style(vt, cell);
        if (!st)
            continue;
        if (st->underline || st->bg_rgb || st->fg_rgb || st->attrs || st->color_flags) {
            char cp_str[16] = "";
            if (cell->cp == 0 || cell->cp == 0x20)
                snprintf(cp_str, sizeof(cp_str), "SP");
            else if (cell->cp < 0x80)
                snprintf(cp_str, sizeof(cp_str), "'%c'", (char)cell->cp);
            else
                snprintf(cp_str, sizeof(cp_str), "U+%04X", cell->cp);
            printf("  col %2d: %s ul=%d ul_rgb=%06X%s fg=%06X%s bg=%06X%s attrs=%02X\n",
                   c, cp_str, st->underline, st->ul_rgb,
                   (st->color_flags & CFR_COLOR_DEFAULT_UL) ? "(def)" : "",
                   st->fg_rgb, (st->color_flags & CFR_COLOR_DEFAULT_FG) ? "(def)" : "",
                   st->bg_rgb, (st->color_flags & CFR_COLOR_DEFAULT_BG) ? "(def)" : "",
                   st->attrs);
        }
        if (cell->width == 2)
            c++;
    }
}

static void render_grid(CfrTerm *vt, int rows, int cols,
                        const int *show_rows, int n_show,
                        int show_cells, int show_styles, int quiet)
{
    for (int r = 0; r < rows; r++) {
        if (quiet && n_show > 0) {
            int found = 0;
            for (int i = 0; i < n_show; i++)
                if (show_rows[i] == r)
                    found = 1;
            if (!found)
                continue;
        }
        printf("%2d|", r);
        render_row_text(vt, r, cols);
        if (show_cells)
            render_row_cells(vt, r, cols);
        if (show_styles)
            render_row_styles(vt, r, cols);
    }
}

/* ================================================================ */
/* Unified-buffer search (cfr_row_text / cfr_find_text in the library) */
/* ================================================================ */

/* Bytes reserved per cell when flattening a row: a grapheme cluster
 * holds at most CFR_CLUSTER_MAX (16) codepoints, 4 bytes each. */
#define ROW_UTF8_PER_CELL 64

/* ================================================================ */
/* Escape expansion                                                 */
/* ================================================================ */

static void expand_escapes(char *s)
{
    char *src = s, *dst = s;
    while (*src) {
        if (src[0] == '\\' && src[1] == 'n') {
            *dst++ = '\n';
            src += 2;
        } else if (src[0] == '\\' && src[1] == 'r') {
            *dst++ = '\r';
            src += 2;
        } else if (src[0] == '\\' && src[1] == 't') {
            *dst++ = '\t';
            src += 2;
        } else if (src[0] == '\\' && src[1] == 'e') {
            *dst++ = '\x1b';
            src += 2;
        } else if (src[0] == '\\' && src[1] == '\\') {
            *dst++ = '\\';
            src += 2;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/* Parse hex bytes from a string: "ff fc 01" -> \xff\xfc\x01 */
static int parse_hex_bytes(const char *s, unsigned char *out, int max)
{
    int count = 0;
    while (*s && count < max) {
        while (*s == ' ' || *s == '\t')
            s++;
        if (!*s)
            break;
        char hex[3] = { 0 };
        hex[0] = *s++;
        if (!*s)
            break;
        hex[1] = *s++;
        out[count++] = (unsigned char)strtol(hex, NULL, 16);
    }
    return count;
}

/* ================================================================ */
/* Script file processing                                           */
/* ================================================================ */

/* Dump the scrollback, oldest first. Unified rows are negative
 * (-1 = the line most recently scrolled off). */
static void render_scrollback(CfrTerm *vt, int cols)
{
    int lines = cfr_get_scrollback_lines(vt);
    if (lines <= 0) {
        printf("(scrollback empty)\n");
        return;
    }
    size_t cap = (size_t)cols * ROW_UTF8_PER_CELL + 1;
    char *buf = malloc(cap);
    if (!buf)
        return;
    for (int sb = lines - 1; sb >= 0; sb--) {
        cfr_row_text(vt, -(sb + 1), buf, cap);
        printf("%2d|%s\n", -(sb + 1), buf);
    }
    free(buf);
}

/* Poll the unified buffer until `needle` appears or the timeout
 * expires. Bounded by the real clock; returns 0 on a match, -1 on
 * timeout. */
static int wait_for(CfrTerm *vt, PtyCtx *pty, const char *needle,
                    double timeout, int line_end, int line_num)
{
    double start = now_sec();
    double deadline = start + timeout;
    int row = 0;

    for (;;) {
        if (cfr_find_text(vt, needle, line_end, &row)) {
            if (row < 0)
                fprintf(stderr,
                        "[%d] wait-for \"%s\": found at scrollback row %d "
                        "after %.1fs\n",
                        line_num, needle, row, now_sec() - start);
            else
                fprintf(stderr,
                        "[%d] wait-for \"%s\": found at row %d after %.1fs\n",
                        line_num, needle, row, now_sec() - start);
            return 0;
        }
        double left = deadline - now_sec();
        if (left <= 0) {
            fprintf(stderr,
                    "[%d] FAIL: wait-for \"%s\": TIMEOUT after %.1fs\n",
                    line_num, needle, timeout);
            return -1;
        }
        drain(vt, pty, left < 0.05 ? left : 0.05);
    }
}

/* Drain until the child exits; records its status. Returns 0 when it
 * exited, -1 on timeout/error. */
static int await_exit(CfrTerm *vt, PtyCtx *pty, double timeout, int line_num,
                      int *out_status, int *have_status)
{
    double start = now_sec();
    double deadline = start + timeout;

    for (;;) {
        int st = 0;
        int r = pty_poll_exit(pty, &st);
        if (r == 1) {
            *out_status = st;
            *have_status = 1;
            fprintf(stderr, "[%d] await-exit: child exited with %d after %.1fs\n",
                    line_num, st, now_sec() - start);
            return 0;
        }
        if (r < 0) {
            fprintf(stderr, "[%d] FAIL: await-exit: cannot query the child\n",
                    line_num);
            return -1;
        }
        double left = deadline - now_sec();
        if (left <= 0) {
            fprintf(stderr, "[%d] FAIL: await-exit: TIMEOUT after %.1fs\n",
                    line_num, timeout);
            return -1;
        }
        drain(vt, pty, left < 0.05 ? left : 0.05);
    }
}

/* Parse `"TEXT" [TIMEOUT] [line-end]` (portty's wait-for grammar):
 * trailing option tokens are trimmed right to left, a bare number is the
 * timeout, `line-end` anchors the match to a row's end; the remainder is
 * the needle, quotes optional. Returns a malloc'd needle (caller frees)
 * or NULL when the argument is empty. */
static char *parse_wait_for(char *args, double *out_timeout, int *out_line_end)
{
    int line_end = 0, got_num = 0;
    for (;;) {
        char *last_ws = NULL;
        for (char *p = args; *p; p++) {
            if (*p == ' ' || *p == '\t')
                last_ws = p;
        }
        if (!last_ws || *(last_ws + 1) == '\0')
            break; /* no trailing token */
        const char *tok = last_ws + 1;
        if (!got_num && (*tok >= '0' && *tok <= '9')) {
            *out_timeout = atof(tok);
            got_num = 1;
            *last_ws = '\0';
            continue;
        }
        if (strcmp(tok, "line-end") == 0) {
            line_end = 1;
            *last_ws = '\0';
            continue;
        }
        break;
    }
    /* Strip surrounding quotes from what remains */
    size_t len = strlen(args);
    if (len >= 2 && ((args[0] == '"' && args[len - 1] == '"') ||
                     (args[0] == '\'' && args[len - 1] == '\''))) {
        args[len - 1] = '\0';
        args++;
    }
    if (*args == '\0')
        return NULL;
    expand_escapes(args);
    *out_line_end = line_end;
    return strdup(args);
}

typedef struct
{
    CfrTerm *vt;
    PtyCtx *pty;
    const int *show_rows;
    int n_show;
    int show_cells, show_styles, quiet;
    int exit_status; /* recorded by await-exit */
    int have_exit_status;
} ScriptCtx;

static int run_script(ScriptCtx *ctx, const char *script_path)
{
    CfrTerm *vt = ctx->vt;
    PtyCtx *pty = ctx->pty;

    FILE *f = fopen(script_path, "r");
    if (!f) {
        fprintf(stderr, "Cannot open script file: %s: %s\n", script_path,
                strerror(errno));
        return 1;
    }

    char line[4096];
    int line_num = 0;
    int rc = 0;

    while (fgets(line, sizeof(line), f)) {
        line_num++;

        /* Strip trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        /* Skip blank lines and comments */
        if (len == 0 || line[0] == '#')
            continue;

        /* Parse command keyword */
        char *cmd = line;
        while (*cmd == ' ' || *cmd == '\t')
            cmd++;
        char *args = cmd;
        while (*args && *args != ' ' && *args != '\t')
            args++;
        if (*args) {
            *args++ = '\0';
            while (*args == ' ' || *args == '\t')
                args++;
        }

        /* Strip surrounding quotes from args (wait-for does its own,
         * after the trailing options are trimmed) */
        if (strcmp(cmd, "wait-for") != 0) {
            size_t alen = strlen(args);
            if (alen >= 2 && ((args[0] == '"' && args[alen - 1] == '"') ||
                              (args[0] == '\'' && args[alen - 1] == '\''))) {
                args[alen - 1] = '\0';
                args++;
            }
        }

        if (strcmp(cmd, "wait") == 0) {
            double sec = atof(args);
            if (sec <= 0)
                sec = 1.0;
            fprintf(stderr, "[%d] wait %.2fs\n", line_num, sec);
            drain(vt, pty, sec);
        } else if (strcmp(cmd, "wait-for") == 0) {
            double timeout = 600;
            int line_end = 0;
            char *needle = parse_wait_for(args, &timeout, &line_end);
            if (!needle) {
                fprintf(stderr,
                        "[%d] FAIL: wait-for requires text "
                        "(usage: wait-for \"TEXT\" [TIMEOUT] [line-end])\n",
                        line_num);
                rc = 1;
                break;
            }
            if (timeout <= 0)
                timeout = 600;
            int r = wait_for(vt, pty, needle, timeout, line_end, line_num);
            free(needle);
            if (r != 0) {
                rc = 1;
                break;
            }
        } else if (strcmp(cmd, "send") == 0) {
            expand_escapes(args);
            fprintf(stderr, "[%d] send: %s\n", line_num, args);
            pty_write(pty, args, strlen(args));
        } else if (strcmp(cmd, "raw") == 0) {
            unsigned char bytes[1024];
            int n = parse_hex_bytes(args, bytes, (int)sizeof(bytes));
            fprintf(stderr, "[%d] raw: %d bytes\n", line_num, n);
            pty_write(pty, (const char *)bytes, (size_t)n);
        } else if (strcmp(cmd, "assert-contains") == 0) {
            if (!cfr_find_text(vt, args, false, NULL)) {
                fprintf(stderr, "[%d] FAIL: buffer does not contain \"%s\"\n",
                        line_num, args);
                rc = 1;
                break;
            }
            fprintf(stderr, "[%d] OK: buffer contains \"%s\"\n", line_num,
                    args);
        } else if (strcmp(cmd, "assert-not-contains") == 0) {
            if (cfr_find_text(vt, args, false, NULL)) {
                fprintf(stderr, "[%d] FAIL: buffer contains \"%s\"\n",
                        line_num, args);
                rc = 1;
                break;
            }
            fprintf(stderr, "[%d] OK: buffer does not contain \"%s\"\n",
                    line_num, args);
        } else if (strcmp(cmd, "await-exit") == 0) {
            double timeout = atof(args);
            if (timeout <= 0)
                timeout = 10;
            if (await_exit(vt, pty, timeout, line_num, &ctx->exit_status,
                           &ctx->have_exit_status) != 0) {
                rc = 1;
                break;
            }
        } else if (strcmp(cmd, "assert-exit") == 0) {
            if (!ctx->have_exit_status) {
                fprintf(stderr,
                        "[%d] FAIL: assert-exit: no status recorded "
                        "(await-exit first)\n",
                        line_num);
                rc = 1;
                break;
            }
            int want = atoi(args);
            if (ctx->exit_status != want) {
                fprintf(stderr, "[%d] FAIL: child exit status %d, want %d\n",
                        line_num, ctx->exit_status, want);
                rc = 1;
                break;
            }
            fprintf(stderr, "[%d] OK: child exit status %d\n", line_num, want);
        } else if (strcmp(cmd, "resize") == 0) {
            int new_cols = 0, new_rows = 0;
            if (sscanf(args, "%d %d", &new_cols, &new_rows) != 2 ||
                new_cols <= 0 || new_rows <= 0) {
                fprintf(stderr, "[%d] FAIL: resize requires COLS ROWS\n",
                        line_num);
                rc = 1;
                break;
            }
            if (pty_resize(pty, new_rows, new_cols) != 0) {
                fprintf(stderr, "[%d] FAIL: resize %dx%d failed\n", line_num,
                        new_cols, new_rows);
                rc = 1;
                break;
            }
            cfr_resize(vt, new_rows, new_cols);
            fprintf(stderr, "[%d] resize: %dx%d\n", line_num, new_cols,
                    new_rows);
        } else if (strcmp(cmd, "render") == 0) {
            int rows = 0, cols = 0;
            cfr_get_dimensions(vt, &rows, &cols);
            const char *what = (*args) ? args : "visible";
            fprintf(stderr, "[%d] render %s\n", line_num, what);
            if (strcmp(what, "scrollback") == 0 || strcmp(what, "all") == 0)
                render_scrollback(vt, cols);
            if (strcmp(what, "visible") == 0 || strcmp(what, "all") == 0)
                render_grid(vt, rows, cols, ctx->show_rows, ctx->n_show,
                            ctx->show_cells, ctx->show_styles, ctx->quiet);
        } else if (strcmp(cmd, "cursor") == 0) {
            CfrCursor c = cfr_get_cursor(vt);
            fprintf(stderr, "[%d] cursor: row=%d col=%d visible=%d blink=%d\n",
                    line_num, c.row, c.col, c.visible, c.blink);
        } else {
            fprintf(stderr, "[%d] WARN: unknown command: %s\n", line_num,
                    cmd);
        }
    }

    fclose(f);
    return rc;
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
    int show_rows[64];
    int n_show = 0;
    int show_cells = 0, show_styles = 0, quiet = 0;
    const char *envs[64];
    int n_env = 0;

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
            fprintf(stderr, "Try 'cfr-debug --help' for more information.\n");
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
            else if (strcmp(f->name, "--show-row") == 0 && n_show < 64)
                show_rows[n_show++] = atoi(val);
            else if (strcmp(f->name, "--env") == 0) {
                if (n_env < 64)
                    envs[n_env++] = val;
                else
                    fprintf(stderr, "Too many --env overrides (max 64)\n");
            }
        } else {
            if (strcmp(f->name, "--cell-data") == 0)
                show_cells = 1;
            else if (strcmp(f->name, "--style-data") == 0)
                show_styles = 1;
            else if (strcmp(f->name, "--quiet") == 0)
                quiet = 1;
        }
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

    PtyCtx *pty = pty_spawn(rows, cols, cmd_argv, (char *const *)envs, n_env);
    if (!pty) {
        cfr_free(vt);
        return 1;
    }
    g_pty = pty;

    CfrCallbacks cb = { .output = cb_output };
    cfr_set_callbacks(vt, &cb, NULL);

    fprintf(stderr, "Waiting %.1fs for startup...\n", initial_wait);
    drain(vt, pty, initial_wait);

    ScriptCtx ctx = {
        .vt = vt,
        .pty = pty,
        .show_rows = show_rows,
        .n_show = n_show,
        .show_cells = show_cells,
        .show_styles = show_styles,
        .quiet = quiet,
    };

    int rc = 0;
    if (script_path)
        rc = run_script(&ctx, script_path);

    /* Final render if no script or the script did not render */
    if (!script_path || rc == 0) {
        int cur_rows = 0, cur_cols = 0;
        cfr_get_dimensions(vt, &cur_rows, &cur_cols);
        render_grid(vt, cur_rows, cur_cols, show_rows, n_show, show_cells,
                    show_styles, quiet);
    }

    if (outfile) {
        fclose(g_raw_out);
        fprintf(stderr, "Raw output saved to %s\n", outfile);
    }

    g_pty = NULL;
    pty_kill(pty);
    cfr_free(vt);
    return rc;
}
