/*
 * pty_w32.c — Windows ConPTY pseudo-terminal, shared by the coffer CLI and
 * test_cfr_pty.
 *
 * The bundled Microsoft.Windows.Console.ConPTY loader lives in conpty.c;
 * this file only deals with the pipe/thread/process plumbing.
 */

#ifdef _WIN32

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pty.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

struct PtyCtx
{
    CfrConpty *conpty;
    HPCON hpc;
    HANDLE input_write;
    HANDLE output_read;
    HANDLE process;
    HANDLE thread;
    HANDLE waiter_thread;
    HANDLE reader_thread;
    HANDLE data_event;         /* signaled when reader has data */
    HANDLE reader_done;        /* signaled when reader thread exits */
    CRITICAL_SECTION lock;     /* protects buf, buf_len, buf_eof */
    CRITICAL_SECTION hpc_lock; /* makes the one-shot close race-free */
    char buf[65536];
    size_t buf_len;
    int buf_eof;
    int reaped; /* child status already collected */
    int rows, cols;
};

static CfrConpty *g_conpty;
static char *g_conpty_dir;
static int g_conpty_initialized;
static int g_verbose;

void pty_set_verbose(int verbose)
{
    g_verbose = verbose;
}

int pty_set_conpty(CfrConptyMode mode, const char *override_dir)
{
    /* Tests and CI use COFFER_CONPTY=system|bundled without adding CLI
     * flags to every invocation.  An explicit CLI mode still wins because
     * setup_conpty() passes it here. */
    if (mode == CFR_CONPTY_AUTO) {
        const char *env = getenv("COFFER_CONPTY");
        if (env && _stricmp(env, "system") == 0)
            mode = CFR_CONPTY_SYSTEM;
        else if (env && _stricmp(env, "bundled") == 0)
            mode = CFR_CONPTY_BUNDLED;
    }

    if (g_conpty_initialized) {
        cfr_conpty_free(g_conpty);
        g_conpty = NULL;
    }
    free(g_conpty_dir);
    g_conpty_dir = NULL;
    if (override_dir && *override_dir) {
        size_t n = strlen(override_dir) + 1;
        g_conpty_dir = malloc(n);
        if (!g_conpty_dir)
            return -1;
        memcpy(g_conpty_dir, override_dir, n);
    }
    g_conpty = cfr_conpty_open(mode, g_conpty_dir);
    g_conpty_initialized = 1;
    if (!g_conpty)
        return -1;
    if (mode == CFR_CONPTY_BUNDLED && !cfr_conpty_is_bundled(g_conpty))
        return -1;
    return 0;
}

static int ensure_conpty(void)
{
    if (!g_conpty_initialized) {
        if (pty_set_conpty(CFR_CONPTY_AUTO, NULL) != 0)
            return -1;
    }
    return g_conpty ? 0 : -1;
}

const char *pty_conpty_backend(void)
{
    return g_conpty ? cfr_conpty_backend(g_conpty) : "system";
}

const char *pty_conpty_version(void)
{
    return g_conpty ? cfr_conpty_version(g_conpty) : NULL;
}

const char *pty_conpty_dll_path(void)
{
    return g_conpty ? cfr_conpty_dll_path(g_conpty) : NULL;
}

const char *pty_conpty_host_path(void)
{
    return g_conpty ? cfr_conpty_host_path(g_conpty) : NULL;
}

const char *pty_conpty_dll_version(void)
{
    return g_conpty ? cfr_conpty_dll_version(g_conpty) : NULL;
}

const char *pty_conpty_host_version(void)
{
    return g_conpty ? cfr_conpty_host_version(g_conpty) : NULL;
}

const char *pty_conpty_error(void)
{
    return g_conpty ? cfr_conpty_error(g_conpty) : "ConPTY loader not initialized";
}

int pty_conpty_has_openconsole(void)
{
    return g_conpty ? cfr_conpty_has_openconsole(g_conpty) : 0;
}

/* Close the pseudo-console exactly once.  The waiter thread and pty_kill()
 * can race here; ClosePseudoConsole on an already-closed handle raises
 * 0xc0000008.  Clear the handle under the lock, then close outside it. */
static void close_hpc_once(PtyCtx *ctx)
{
    EnterCriticalSection(&ctx->hpc_lock);
    HPCON hpc = ctx->hpc;
    ctx->hpc = INVALID_HANDLE_VALUE;
    LeaveCriticalSection(&ctx->hpc_lock);
    if (hpc != INVALID_HANDLE_VALUE)
        cfr_conpty_close_pc(ctx->conpty, hpc);
}

static DWORD WINAPI pty_waiter_thread(LPVOID param)
{
    PtyCtx *ctx = (PtyCtx *)param;
    WaitForSingleObject(ctx->process, INFINITE);
    close_hpc_once(ctx);
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
            if (err == ERROR_BROKEN_PIPE || err == ERROR_HANDLE_EOF) {
                EnterCriticalSection(&ctx->lock);
                ctx->buf_eof = 1;
                LeaveCriticalSection(&ctx->lock);
            }
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
    if (ensure_conpty() != 0)
        return NULL;

    PtyCtx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;
    ctx->conpty = g_conpty;
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
    InitializeCriticalSection(&ctx->hpc_lock);

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

    /* Flags are deliberately 0: the bundled 1.25 OpenConsole passes DCS,
     * APC and unknown OSC through by default.  0x8 used to be the system
     * passthrough flag but is now PSEUDOCONSOLE_GLYPH_WIDTH_GRAPHEMES. */
    HRESULT hr = cfr_conpty_create(ctx->conpty, size, input_read, output_write,
                                   0, &ctx->hpc);
    if (FAILED(hr)) {
        fprintf(stderr, "ERROR: CreatePseudoConsole failed: 0x%08lx%s%s\n",
                (unsigned long)hr,
                pty_conpty_error()[0] ? " (" : "",
                pty_conpty_error()[0] ? pty_conpty_error() : "");
        goto fail;
    }

    CloseHandle(input_read);
    input_read = INVALID_HANDLE_VALUE;
    CloseHandle(output_write);
    output_write = INVALID_HANDLE_VALUE;

    /* STARTUPINFOEX with pseudo-console attribute.  STARTF_USESTDHANDLES
     * with invalid handles is what makes the child attach to the
     * pseudo-console instead of inheriting the CLI's console. */
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
        MAX_OV = 64, /* matches the 64-entry cap on --env in main() */
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

    if (g_verbose) {
        char cmdline_utf8[MAX_PATH * 8];
        cmdline_utf8[0] = '\0';
        WideCharToMultiByte(CP_UTF8, 0, cmdline, -1, cmdline_utf8,
                            sizeof(cmdline_utf8), NULL, NULL);
        fprintf(stderr, "coffer: spawning: %s\n", cmdline_utf8);
    }

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
        cfr_conpty_close_pc(ctx->conpty, ctx->hpc);
    free(ctx);
    return NULL;
}

void pty_kill(PtyCtx *ctx)
{
    if (!ctx)
        return;

    /* Close pseudo-console first — this unblocks the reader thread's
     * ReadFile (pipe breaks) and the waiter thread's wait. */
    close_hpc_once(ctx);

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
    DeleteCriticalSection(&ctx->hpc_lock);
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
    HRESULT hr = cfr_conpty_resize(ctx->conpty, ctx->hpc, size);
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

/* ---------------------------------------------------------------- */
/* Test-compatibility API                                            */
/* ---------------------------------------------------------------- */

PtyContext *pty_create(int rows, int cols, char *const argv[])
{
    return pty_spawn(rows, cols, argv, NULL, 0);
}

void pty_destroy(PtyContext *ctx)
{
    pty_kill(ctx);
}

bool pty_is_running(PtyContext *ctx)
{
    if (!ctx || ctx->process == INVALID_HANDLE_VALUE || ctx->reaped)
        return false;
    return WaitForSingleObject(ctx->process, 0) != WAIT_OBJECT_0;
}

int pty_get_master_fd(PtyContext *ctx)
{
    (void)ctx;
    return -1;
}

int pty_get_child_pid(PtyContext *ctx)
{
    if (!ctx || ctx->process == INVALID_HANDLE_VALUE)
        return -1;
    return (int)GetProcessId(ctx->process);
}

int pty_signal_init(void)
{
    return 0;
}

void pty_signal_cleanup(void)
{
}

int pty_signal_get_fd(void)
{
    return -1;
}

void pty_signal_drain(void)
{
}

void *pty_get_process_handle(PtyContext *ctx)
{
    if (!ctx)
        return NULL;
    return (void *)ctx->process;
}

void pty_close_console(PtyContext *ctx)
{
    if (ctx)
        close_hpc_once(ctx);
}

#endif /* _WIN32 */
