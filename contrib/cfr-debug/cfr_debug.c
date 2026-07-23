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
    { "--help", "-h", 0, NULL, "Show this help" },
    { NULL, NULL, 0, NULL, NULL }
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
    printf("  wait SECONDS              Drain PTY for N seconds\n");
    printf("  send TEXT                 Send text to PTY input (child's stdin).\n");
    printf("                            Escapes: \\n=CR \\r=CR \\e=ESC \\t=TAB \\\\=backslash.\n");
    printf("                            Text is written verbatim; the shell must execute it.\n");
    printf("                            Tip: append \\n to execute a shell command, e.g.\n");
    printf("                              send printf '\\\\033[?12l'\\n\n");
    printf("                            To send escape sequences the terminal will process,\n");
    printf("                            have the child output them (e.g. via printf); raw\n");
    printf("                            writes to stdin, not the terminal output side.\n");
    printf("  raw HEX [HEX ...]         Write raw bytes to PTY input (e.g. raw 1b 5b 6d).\n");
    printf("                            Bytes reach the child's stdin, not coffer's parser.\n");
    printf("                            Use send with printf to emit sequences coffer parses.\n");
    printf("  assert-contains TEXT      Fail if grid does not contain TEXT\n");
    printf("  assert-not-contains TEXT  Fail if grid contains TEXT\n");
    printf("  render                    Dump current grid to stdout\n");
    printf("  cursor                    Dump cursor state (row, col, visible, blink)\n");
    printf("  # comment                 Skipped\n");
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
}

/* ================================================================ */
/* PTY abstraction                                                  */
/* ================================================================ */

typedef struct PtyCtx PtyCtx;

PtyCtx *pty_spawn(int rows, int cols, char *const argv[]);
void pty_kill(PtyCtx *p);
ssize_t pty_write(PtyCtx *p, const char *data, size_t len);
ssize_t pty_read(PtyCtx *p, char *buf, size_t bufsize);
int pty_get_fd(PtyCtx *p);
#ifdef _WIN32
void *pty_get_data_event(PtyCtx *p);
int pty_is_eof(PtyCtx *p);
#endif

/* ---- POSIX PTY (forkpty + poll) ---- */

#ifndef _WIN32

struct PtyCtx
{
    int master_fd;
    pid_t child_pid;
    int rows, cols;
};

PtyCtx *pty_spawn(int rows, int cols, char *const argv[])
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
    pid_t pid = forkpty(&p->master_fd, NULL, NULL, &ws);
    if (pid < 0) {
        perror("forkpty");
        free(p);
        return NULL;
    }
    if (pid == 0) {
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        setenv("TERM_PROGRAM", "ghostty", 1);
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

PtyCtx *pty_spawn(int rows, int cols, char *const argv[])
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

    /* Build environment block with terminal overrides */
    LPWCH parent_env = GetEnvironmentStringsW();
    WCHAR envBlock[65536];
    WCHAR *ep = envBlock;

    {
        static const WCHAR *overrides[] = {
            L"TERM=xterm-256color",
            L"COLORTERM=truecolor",
        };
        for (int i = 0; i < 2; i++) {
            size_t len = wcslen(overrides[i]);
            if (ep + len + 1 >= envBlock + 65536)
                break;
            wmemcpy(ep, overrides[i], len + 1);
            ep += len + 1;
        }
    }

    if (parent_env) {
        LPWCH p = parent_env;
        while (*p) {
            size_t len = wcslen(p);
            if (ep + len + 1 >= envBlock + 65536)
                break;
            wmemcpy(ep, p, len + 1);
            ep += len + 1;
            p += len + 1;
        }
        FreeEnvironmentStringsW(parent_env);
    }
    *ep = L'\0';

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
        if (WaitForSingleObject(ctx->process, 500) != WAIT_OBJECT_0) {
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

static void sleep_sec(double seconds)
{
#ifdef _WIN32
    Sleep((DWORD)(seconds * 1000));
#else
    usleep((useconds_t)(seconds * 1e6));
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
/* Grid search                                                      */
/* ================================================================ */

static int grid_contains(CfrTerm *vt, int rows, int cols, const char *needle)
{
    int needle_len = (int)strlen(needle);
    if (needle_len == 0)
        return 1;

    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            const CfrCell *cell = cfr_get_cell(vt, r, c);
            if (!cell || cell->cp == 0)
                continue;
            if (cell->width == 2) {
                c++;
                continue;
            }
            int ci = c;
            const char *np = needle;
            while (*np && ci < cols) {
                cell = cfr_get_cell(vt, r, ci);
                if (!cell)
                    break;
                if ((char)cell->cp != *np)
                    break;
                np++;
                ci++;
                if (cell->width == 2)
                    ci++;
            }
            if (*np == '\0')
                return 1;
        }
    }
    return 0;
}

/* ================================================================ */
/* Escape expansion                                                 */
/* ================================================================ */

static void expand_escapes(char *s)
{
    char *src = s, *dst = s;
    while (*src) {
        if (src[0] == '\\' && src[1] == 'n') {
            *dst++ = '\r';
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

static int run_script(CfrTerm *vt, PtyCtx *pty, int rows, int cols,
                      const char *script_path, const int *show_rows,
                      int n_show, int show_cells, int show_styles, int quiet)
{
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

        /* Strip surrounding quotes from args */
        size_t alen = strlen(args);
        if (alen >= 2 && ((args[0] == '"' && args[alen - 1] == '"') ||
                          (args[0] == '\'' && args[alen - 1] == '\''))) {
            args[alen - 1] = '\0';
            args++;
        }

        if (strcmp(cmd, "wait") == 0) {
            double sec = atof(args);
            if (sec <= 0)
                sec = 1.0;
            fprintf(stderr, "[%d] wait %.2fs\n", line_num, sec);
            drain(vt, pty, sec);
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
            if (!grid_contains(vt, rows, cols, args)) {
                fprintf(stderr, "[%d] FAIL: grid does not contain \"%s\"\n",
                        line_num, args);
                rc = 1;
                break;
            }
            fprintf(stderr, "[%d] OK: grid contains \"%s\"\n", line_num, args);
        } else if (strcmp(cmd, "assert-not-contains") == 0) {
            if (grid_contains(vt, rows, cols, args)) {
                fprintf(stderr, "[%d] FAIL: grid contains \"%s\"\n",
                        line_num, args);
                rc = 1;
                break;
            }
            fprintf(stderr, "[%d] OK: grid does not contain \"%s\"\n",
                    line_num, args);
        } else if (strcmp(cmd, "render") == 0) {
            fprintf(stderr, "[%d] render\n", line_num);
            render_grid(vt, rows, cols, show_rows, n_show, show_cells,
                        show_styles, quiet);
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

static void usage(void)
{
    print_help();
}

int main(int argc, char *argv[])
{
    int rows = 24, cols = 80;
    double initial_wait = 3.0;
    const char *script_path = NULL;
    const char *outfile = NULL;
    int show_rows[64];
    int n_show = 0;
    int show_cells = 0, show_styles = 0, quiet = 0;

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

        if (matches_flag(arg, &OPTIONS[9])) { /* --help / -h */
            print_help();
            return 0;
        }

        if (f->takes_value) {
            if (i + 1 >= cmd_start) {
                fprintf(stderr, "Error: %s requires a value\n", arg);
                return 1;
            }
            const char *val = argv[++i];

            if (matches_flag(arg, &OPTIONS[0])) /* --rows */
                rows = atoi(val);
            else if (matches_flag(arg, &OPTIONS[1])) /* --cols */
                cols = atoi(val);
            else if (matches_flag(arg, &OPTIONS[2])) /* --wait */
                initial_wait = atof(val);
            else if (matches_flag(arg, &OPTIONS[3])) /* --script */
                script_path = val;
            else if (matches_flag(arg, &OPTIONS[4])) /* --output */
                outfile = val;
            else if (matches_flag(arg, &OPTIONS[5]) && n_show < 64) /* --show-row */
                show_rows[n_show++] = atoi(val);
        } else {
            if (matches_flag(arg, &OPTIONS[6])) /* --cell-data */
                show_cells = 1;
            else if (matches_flag(arg, &OPTIONS[7])) /* --style-data */
                show_styles = 1;
            else if (matches_flag(arg, &OPTIONS[8])) /* --quiet */
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

    PtyCtx *pty = pty_spawn(rows, cols, cmd_argv);
    if (!pty) {
        cfr_free(vt);
        return 1;
    }
    g_pty = pty;

    CfrCallbacks cb = { .output = cb_output };
    cfr_set_callbacks(vt, &cb, NULL);

    fprintf(stderr, "Waiting %.1fs for startup...\n", initial_wait);
    drain(vt, pty, initial_wait);

    int rc = 0;
    if (script_path) {
        rc = run_script(vt, pty, rows, cols, script_path, show_rows,
                        n_show, show_cells, show_styles, quiet);
    }

    /* Final render if no script or script didn't render */
    if (!script_path || rc == 0) {
        if (!quiet || n_show == 0) {
            render_grid(vt, rows, cols, show_rows, n_show, show_cells,
                        show_styles, quiet);
        }
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
