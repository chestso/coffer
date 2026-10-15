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
#include <coffer/script.h>

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
    size_t n_cmds = 0;
    const CfrScriptCmdDef *cmds = cfr_script_commands(&n_cmds);
    for (size_t i = 0; i < n_cmds; i++) {
        const char *args = cmds[i].args ? cmds[i].args : "-";
        printf("CMD\t%s\t%s\t%s\n", cmds[i].name, args, cmds[i].desc);
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
    /* TIOCSWINSZ on the slave (SIGWINCHes the child), then the grid. */
    if (pty_resize(hc->pty, rows, cols) != 0)
        return -1;
    cfr_resize(hc->vt, rows, cols);
    return 0;
}

/* Blocking pump: drain the PTY into coffer for up to max_ms. */
static void host_pump(void *ud, uint32_t max_ms)
{
    HostCtx *hc = ud;
    drain(hc->vt, hc->pty, max_ms / 1000.0);
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
        .pty_write = host_pty_write,
        .emit = host_emit,
        .poll_exit = host_poll_exit,
        .resize = host_resize,
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

static void dump_one_row_text(CfrTerm *vt, int row, int cols)
{
    size_t cap = (size_t)cols * 64 + 1;
    char *buf = malloc(cap);
    if (!buf)
        return;
    cfr_row_text(vt, row, buf, cap);
    printf("%2d|%s\n", row, buf);
    free(buf);
}

static void final_dump(CfrTerm *vt, const int *show_rows, int n_show,
                       int show_cells, int show_styles, int quiet)
{
    int rows = 0, cols = 0;
    cfr_get_dimensions(vt, &rows, &cols);

    if (quiet && n_show > 0) {
        /* Only the requested rows. */
        for (int i = 0; i < n_show; i++) {
            int r = show_rows[i];
            dump_one_row_text(vt, r, cols);
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

    HostCtx hc = { .vt = vt, .pty = pty };

    int rc = 0;
    if (script_path)
        rc = run_script(&hc, script_path);

    /* Final dump if no script ran, or the script passed */
    if (!script_path || rc == 0)
        final_dump(vt, show_rows, n_show, show_cells, show_styles, quiet);

    if (outfile) {
        fclose(g_raw_out);
        fprintf(stderr, "Raw output saved to %s\n", outfile);
    }

    g_pty = NULL;
    pty_kill(pty);
    cfr_free(vt);
    return rc;
}
