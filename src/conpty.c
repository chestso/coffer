/*
 * conpty.c — bundled ConPTY loader (Windows only).
 *
 * See conpty.h for the rationale.  The loader is deliberately independent
 * of the rest of coffer so portty can copy it verbatim later.
 */

#ifdef _WIN32

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "conpty.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <winver.h>

#ifndef COFFER_CONPTY_VERSION
#define COFFER_CONPTY_VERSION "unknown"
#endif

typedef HRESULT(WINAPI *CreatePseudoConsoleFn)(COORD, HANDLE, HANDLE, DWORD,
                                               HPCON *);
typedef HRESULT(WINAPI *ResizePseudoConsoleFn)(HPCON, COORD);
typedef void(WINAPI *ClosePseudoConsoleFn)(HPCON);

struct CfrConpty
{
    HMODULE dll;
    CreatePseudoConsoleFn create;
    ResizePseudoConsoleFn resize;
    ClosePseudoConsoleFn close;

    int bundled;        /* using conpty.dll */
    int strict_bundled; /* mode == CFR_CONPTY_BUNDLED */
    int has_openconsole;
    int has_legacy_exports;

    char error[512];
    char dll_path[1024];
    char host_path[1024];
    char host_version[64];
    char dll_version[64];
    const char *version;
};

/* ---------------------------------------------------------------- */
/* Small Win32 helpers                                               */
/* ---------------------------------------------------------------- */

static int file_exists(const wchar_t *path)
{
    DWORD attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES &&
           !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static int dir_exists(const wchar_t *path)
{
    DWORD attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static void join_path(wchar_t *out, size_t cap, const wchar_t *dir,
                      const wchar_t *name)
{
    size_t len = wcslen(dir);
    int needs_sep = len > 0 && dir[len - 1] != L'\\' && dir[len - 1] != L'/';
    _snwprintf(out, cap, L"%s%s%s", dir, needs_sep ? L"\\" : L"", name);
    out[cap - 1] = L'\0';
}

static void utf8_to_wide(const char *src, wchar_t *dst, size_t cap)
{
    if (!src || !*src) {
        dst[0] = L'\0';
        return;
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, (int)cap);
    if (n <= 0)
        dst[0] = L'\0';
    else
        dst[cap - 1] = L'\0';
}

static void wide_to_utf8(const wchar_t *src, char *dst, size_t cap)
{
    if (!src || !*src) {
        dst[0] = '\0';
        return;
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, (int)cap, NULL, NULL);
    if (n <= 0)
        dst[0] = '\0';
    else
        dst[cap - 1] = '\0';
}

static int get_exe_dir(wchar_t *out, size_t cap)
{
    DWORD n = GetModuleFileNameW(NULL, out, (DWORD)cap);
    if (n == 0 || n >= cap)
        return -1;
    wchar_t *slash = wcsrchr(out, L'\\');
    wchar_t *fwd = wcsrchr(out, L'/');
    if (fwd > slash)
        slash = fwd;
    if (!slash)
        return -1;
    slash[1] = L'\0';
    return 0;
}

static const wchar_t *native_arch_dir(void)
{
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    switch (si.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64:
        return L"x64";
    case PROCESSOR_ARCHITECTURE_ARM64:
        return L"arm64";
    case PROCESSOR_ARCHITECTURE_INTEL:
    case PROCESSOR_ARCHITECTURE_IA32_ON_WIN64:
        return L"x86";
    default:
        return L"";
    }
}

static void read_file_version(const wchar_t *path, char *out, size_t cap)
{
    out[0] = '\0';
    DWORD dummy = 0;
    DWORD size = GetFileVersionInfoSizeW(path, &dummy);
    if (size == 0)
        return;
    void *buf = malloc(size);
    if (!buf)
        return;
    if (GetFileVersionInfoW(path, 0, size, buf)) {
        VS_FIXEDFILEINFO *info = NULL;
        UINT len = 0;
        if (VerQueryValueW(buf, L"\\", (LPVOID *)&info, &len) && info) {
            snprintf(out, cap, "%u.%u.%u.%u",
                     (unsigned)HIWORD(info->dwFileVersionMS),
                     (unsigned)LOWORD(info->dwFileVersionMS),
                     (unsigned)HIWORD(info->dwFileVersionLS),
                     (unsigned)LOWORD(info->dwFileVersionLS));
        }
    }
    free(buf);
}

/* Try to load a bundle from `dir`.  On success, fills `c`. */
static int try_bundle(CfrConpty *c, const wchar_t *dir)
{
    wchar_t dll[MAX_PATH * 3];
    wchar_t host[MAX_PATH * 3];
    wchar_t arch_host[MAX_PATH * 3];

    if (!dir_exists(dir))
        return -1;

    join_path(dll, sizeof(dll) / sizeof(dll[0]), dir, L"conpty.dll");
    if (!file_exists(dll))
        return -1;

    join_path(host, sizeof(host) / sizeof(host[0]), dir, L"OpenConsole.exe");
    const wchar_t *arch = native_arch_dir();
    if (!file_exists(host) && arch[0]) {
        join_path(arch_host, sizeof(arch_host) / sizeof(arch_host[0]), dir,
                  arch);
        join_path(host, sizeof(host) / sizeof(host[0]), arch_host,
                  L"OpenConsole.exe");
    }
    if (!file_exists(host)) {
        snprintf(c->error, sizeof(c->error),
                 "found %ls but no OpenConsole.exe (conpty.dll would fall "
                 "back to system conhost)",
                 dll);
        return -1;
    }

    HMODULE mod = LoadLibraryExW(dll, NULL,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                     LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!mod) {
        snprintf(c->error, sizeof(c->error),
                 "LoadLibraryExW(%ls) failed: %lu", dll, GetLastError());
        return -1;
    }

    CreatePseudoConsoleFn create =
        (CreatePseudoConsoleFn)(void (*)(void))GetProcAddress(
            mod, "ConptyCreatePseudoConsole");
    ResizePseudoConsoleFn resize =
        (ResizePseudoConsoleFn)(void (*)(void))GetProcAddress(
            mod, "ConptyResizePseudoConsole");
    ClosePseudoConsoleFn close =
        (ClosePseudoConsoleFn)(void (*)(void))GetProcAddress(
            mod, "ConptyClosePseudoConsole");
    int legacy = 0;
    if (!create || !resize || !close) {
        /* The package still exports the legacy names for older consumers. */
        create = (CreatePseudoConsoleFn)(void (*)(void))GetProcAddress(
            mod, "CreatePseudoConsole");
        resize = (ResizePseudoConsoleFn)(void (*)(void))GetProcAddress(
            mod, "ResizePseudoConsole");
        close = (ClosePseudoConsoleFn)(void (*)(void))GetProcAddress(
            mod, "ClosePseudoConsole");
        legacy = 1;
    }
    if (!create || !resize || !close) {
        FreeLibrary(mod);
        snprintf(c->error, sizeof(c->error),
                 "%ls does not export the ConPTY entry points", dll);
        return -1;
    }

    c->dll = mod;
    c->create = create;
    c->resize = resize;
    c->close = close;
    c->bundled = 1;
    c->has_openconsole = 1;
    c->has_legacy_exports = legacy;
    wide_to_utf8(dll, c->dll_path, sizeof(c->dll_path));
    wide_to_utf8(host, c->host_path, sizeof(c->host_path));
    read_file_version(dll, c->dll_version, sizeof(c->dll_version));
    read_file_version(host, c->host_version, sizeof(c->host_version));
    c->version = COFFER_CONPTY_VERSION;
    c->error[0] = '\0';
    return 0;
}

static int try_bundle_str(CfrConpty *c, const char *dir_utf8)
{
    wchar_t dir[MAX_PATH * 3];
    utf8_to_wide(dir_utf8, dir, sizeof(dir) / sizeof(dir[0]));
    if (!dir[0])
        return -1;
    return try_bundle(c, dir);
}

/* ---------------------------------------------------------------- */
/* Public API                                                        */
/* ---------------------------------------------------------------- */

CfrConpty *cfr_conpty_open(CfrConptyMode mode, const char *override_dir)
{
    CfrConpty *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->version = "system";

    if (mode == CFR_CONPTY_SYSTEM) {
        c->error[0] = '\0';
        return c;
    }

    c->strict_bundled = (mode == CFR_CONPTY_BUNDLED);

    /* 1. Explicit override / environment. */
    if (override_dir && *override_dir) {
        if (try_bundle_str(c, override_dir) == 0)
            return c;
    }

    wchar_t env_dir[1024];
    DWORD env_len = GetEnvironmentVariableW(L"COFFER_CONPTY_DIR", env_dir,
                                            (DWORD)(sizeof(env_dir) /
                                                    sizeof(env_dir[0])));
    if (env_len > 0 && env_len < sizeof(env_dir) / sizeof(env_dir[0])) {
        if (try_bundle(c, env_dir) == 0)
            return c;
    }

    /* 2. Beside the executable (portable/build-tree layout). */
    wchar_t exe_dir[MAX_PATH * 2];
    if (get_exe_dir(exe_dir, sizeof(exe_dir) / sizeof(exe_dir[0])) == 0) {
        if (try_bundle(c, exe_dir) == 0)
            return c;
        wchar_t sub[MAX_PATH * 2];
        join_path(sub, sizeof(sub) / sizeof(sub[0]), exe_dir, L"conpty");
        if (try_bundle(c, sub) == 0)
            return c;
    }

    /* 3. Compiled install path (libexec/coffer) and build-tree path. */
#ifdef COFFER_CONPTY_DIR
    {
        wchar_t dir[MAX_PATH * 3];
        utf8_to_wide(COFFER_CONPTY_DIR, dir, sizeof(dir) / sizeof(dir[0]));
        if (dir[0] && try_bundle(c, dir) == 0)
            return c;
    }
#endif
#ifdef COFFER_CONPTY_BUILDDIR
    {
        wchar_t dir[MAX_PATH * 3];
        utf8_to_wide(COFFER_CONPTY_BUILDDIR, dir,
                     sizeof(dir) / sizeof(dir[0]));
        if (dir[0] && try_bundle(c, dir) == 0)
            return c;
    }
#endif

    /* Not found.  In strict mode keep the object so the caller can print
     * c->error; pty_set_conpty() turns the empty bundled flag into a hard
     * failure.  In auto mode a system-backed loader is still usable. */
    if (c->error[0] == '\0')
        snprintf(c->error, sizeof(c->error),
                 "no bundled conpty.dll + OpenConsole.exe found");
    c->bundled = 0;
    c->has_openconsole = 0;
    c->version = "system";
    return c;
}

void cfr_conpty_free(CfrConpty *c)
{
    if (!c)
        return;
    if (c->dll)
        FreeLibrary(c->dll);
    free(c);
}

HRESULT cfr_conpty_create(CfrConpty *c, COORD size, HANDLE input,
                          HANDLE output, DWORD flags, HPCON *out)
{
    if (!c)
        return E_INVALIDARG;
    if (c->bundled) {
        HRESULT hr = c->create(size, input, output, flags, out);
        if (SUCCEEDED(hr))
            return hr;
        if (c->strict_bundled) {
            snprintf(c->error, sizeof(c->error),
                     "ConptyCreatePseudoConsole failed: 0x%08lx",
                     (unsigned long)hr);
            return hr;
        }
        /* The bundle loaded but could not create a console (missing
         * OpenConsole.exe at runtime, blocked by policy, ...).  Retry with
         * the system implementation and remember the switch. */
        snprintf(c->error, sizeof(c->error),
                 "ConptyCreatePseudoConsole failed: 0x%08lx; using system",
                 (unsigned long)hr);
        c->bundled = 0;
        c->version = "system";
    }
    return CreatePseudoConsole(size, input, output, flags, out);
}

HRESULT cfr_conpty_resize(CfrConpty *c, HPCON pc, COORD size)
{
    if (!c)
        return E_INVALIDARG;
    if (c->bundled)
        return c->resize(pc, size);
    return ResizePseudoConsole(pc, size);
}

void cfr_conpty_close_pc(CfrConpty *c, HPCON pc)
{
    if (!c || pc == INVALID_HANDLE_VALUE)
        return;
    if (c->bundled)
        c->close(pc);
    else
        ClosePseudoConsole(pc);
}

const char *cfr_conpty_backend(const CfrConpty *c)
{
    return (c && c->bundled) ? "bundled" : "system";
}

const char *cfr_conpty_version(const CfrConpty *c)
{
    return c ? c->version : NULL;
}

const char *cfr_conpty_dll_path(const CfrConpty *c)
{
    return (c && c->bundled) ? c->dll_path : NULL;
}

const char *cfr_conpty_host_path(const CfrConpty *c)
{
    return (c && c->bundled) ? c->host_path : NULL;
}

const char *cfr_conpty_error(const CfrConpty *c)
{
    return c ? c->error : "no loader";
}

int cfr_conpty_has_openconsole(const CfrConpty *c)
{
    return c && c->bundled && c->has_openconsole;
}

int cfr_conpty_is_bundled(const CfrConpty *c)
{
    return c && c->bundled;
}

/* Expose the versions for diagnostics without adding public header churn. */
const char *cfr_conpty_dll_version(const CfrConpty *c)
{
    return (c && c->bundled) ? c->dll_version : NULL;
}

const char *cfr_conpty_host_version(const CfrConpty *c)
{
    return (c && c->bundled) ? c->host_version : NULL;
}

#endif /* _WIN32 */
