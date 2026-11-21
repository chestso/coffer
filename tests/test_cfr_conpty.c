/*
 * test_cfr_conpty — bundled ConPTY loader unit tests (Windows only).
 *
 * test_cfr_pty drives a real child through the selected backend; this one
 * covers the loader branches a working bundle never reaches: a directory
 * with conpty.dll but no OpenConsole.exe, an unloadable (wrong-architecture)
 * conpty.dll, the <arch>/OpenConsole.exe fallback, and the
 * override > COFFER_CONPTY_DIR > compiled-path search order.
 *
 * The binaries under test are copied from whatever bundle the build tree
 * (or COFFER_CONPTY_DIR) provides, so the suite skips itself when the
 * ConPTY bundle is absent.
 */

#include "test_helpers.h"

#ifdef _WIN32

#include "conpty.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define PATH_W (MAX_PATH * 3)

/* The real bundle, opened once from the default search order and used as
 * the copy source.  NULL when this build has no bundle (system-only). */
static CfrConpty *g_bundle;
static wchar_t g_saved_env[PATH_W];
static int g_had_env;
static int g_seq;

/* ------------------------------------------------------------------ */
/* Path and temp-directory helpers                                     */
/* ------------------------------------------------------------------ */

static void join_path(wchar_t *out, size_t cap, const wchar_t *dir,
                      const wchar_t *name)
{
    size_t len = wcslen(dir);
    int sep = len > 0 && dir[len - 1] != L'\\' && dir[len - 1] != L'/';
    _snwprintf(out, cap, L"%s%s%s", dir, sep ? L"\\" : L"", name);
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
    int n = WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, (int)cap, NULL,
                                NULL);
    if (n <= 0)
        dst[0] = '\0';
    else
        dst[cap - 1] = '\0';
}

static int make_temp_dir(wchar_t *out, size_t cap)
{
    wchar_t base[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, base);
    if (n == 0 || n >= MAX_PATH)
        return -1;
    _snwprintf(out, cap, L"%scoffer-conpty-%lu-%d", base,
               (unsigned long)GetCurrentProcessId(), ++g_seq);
    out[cap - 1] = L'\0';
    return CreateDirectoryW(out, NULL) ? 0 : -1;
}

static void remove_tree(const wchar_t *dir)
{
    static const wchar_t *files[] = { L"conpty.dll", L"OpenConsole.exe" };
    static const wchar_t *arch_dirs[] = { L"x64", L"arm64", L"x86" };
    wchar_t path[PATH_W];

    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        join_path(path, PATH_W, dir, files[i]);
        DeleteFileW(path);
    }
    for (size_t i = 0; i < sizeof(arch_dirs) / sizeof(arch_dirs[0]); i++) {
        wchar_t sub[PATH_W];
        join_path(sub, PATH_W, dir, arch_dirs[i]);
        join_path(path, PATH_W, sub, L"OpenConsole.exe");
        DeleteFileW(path);
        RemoveDirectoryW(sub);
    }
    RemoveDirectoryW(dir);
}

/* Copy a UTF-8 source path to `dir\name`. */
static int copy_to_dir(const char *src_utf8, const wchar_t *dir,
                       const wchar_t *name)
{
    wchar_t src[PATH_W];
    wchar_t dst[PATH_W];
    utf8_to_wide(src_utf8, src, PATH_W);
    if (!src[0])
        return -1;
    join_path(dst, PATH_W, dir, name);
    return CopyFileW(src, dst, FALSE) ? 0 : -1;
}

/* Create `dir` plus a file of the given bytes below it. */
static int write_file(const wchar_t *dir, const wchar_t *name, const char *data,
                      size_t len)
{
    wchar_t path[PATH_W];
    join_path(path, PATH_W, dir, name);
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    DWORD written = 0;
    BOOL ok = WriteFile(h, data, (DWORD)len, &written, NULL);
    CloseHandle(h);
    return ok && written == len ? 0 : -1;
}

static int make_dir(wchar_t *dir)
{
    if (!g_bundle) {
        printf("    (skipping: no bundled ConPTY available to copy)\n");
        return -1;
    }
    if (make_temp_dir(dir, PATH_W) != 0) {
        printf("    (skipping: cannot create a temp directory)\n");
        return -1;
    }
    return 0;
}

/* `dir` + a complete, loadable bundle. */
static int make_valid_bundle(wchar_t *dir)
{
    if (make_dir(dir) != 0)
        return -1;
    if (copy_to_dir(cfr_conpty_dll_path(g_bundle), dir, L"conpty.dll") != 0 ||
        copy_to_dir(cfr_conpty_host_path(g_bundle), dir, L"OpenConsole.exe") !=
            0) {
        printf("    (skipping: cannot copy the bundle: %lu)\n",
               (unsigned long)GetLastError());
        remove_tree(dir);
        return -1;
    }
    return 0;
}

static int path_is_under(const char *path_utf8, const wchar_t *dir)
{
    wchar_t path[PATH_W];
    wchar_t want[PATH_W];
    if (!path_utf8 || !*path_utf8)
        return 0;
    utf8_to_wide(path_utf8, path, PATH_W);
    join_path(want, PATH_W, dir, L"");
    return _wcsnicmp(path, want, wcslen(want)) == 0;
}

static void env_clear(void)
{
    SetEnvironmentVariableW(L"COFFER_CONPTY_DIR", NULL);
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

static void test_system_mode(void)
{
    CfrConpty *c = cfr_conpty_open(CFR_CONPTY_SYSTEM, NULL);
    ASSERT_NOT_NULL(c);
    ASSERT_STR_EQ(cfr_conpty_backend(c), "system");
    ASSERT_NULL(cfr_conpty_dll_path(c));
    ASSERT_NULL(cfr_conpty_host_path(c));
    ASSERT_EQ(cfr_conpty_has_openconsole(c), 0);
    cfr_conpty_free(c);
}

/* SYSTEM must ignore COFFER_CONPTY_DIR even when it names a valid bundle. */
static void test_system_mode_ignores_env(void)
{
    wchar_t dir[PATH_W];
    if (make_valid_bundle(dir) != 0)
        return;

    SetEnvironmentVariableW(L"COFFER_CONPTY_DIR", dir);
    CfrConpty *c = cfr_conpty_open(CFR_CONPTY_SYSTEM, NULL);
    env_clear();

    ASSERT_NOT_NULL(c);
    ASSERT_STR_EQ(cfr_conpty_backend(c), "system");
    ASSERT_NULL(cfr_conpty_dll_path(c));
    cfr_conpty_free(c);
    remove_tree(dir);
}

static void test_load_valid_dir(void)
{
    wchar_t dir[PATH_W];
    char u8[PATH_W];
    if (make_valid_bundle(dir) != 0)
        return;
    wide_to_utf8(dir, u8, sizeof(u8));

    CfrConpty *c = cfr_conpty_open_dir(u8);
    ASSERT_NOT_NULL(c);
    ASSERT_STR_EQ(cfr_conpty_backend(c), "bundled");
    ASSERT_TRUE(path_is_under(cfr_conpty_dll_path(c), dir));
    ASSERT_TRUE(path_is_under(cfr_conpty_host_path(c), dir));
    ASSERT_EQ(cfr_conpty_has_openconsole(c), 1);
    ASSERT_STR_EQ(cfr_conpty_error(c), "");
    cfr_conpty_free(c);
    remove_tree(dir);
}

/* conpty.dll alone: the loader must not report the bundle as usable. */
static void test_missing_openconsole(void)
{
    wchar_t dir[PATH_W];
    char u8[PATH_W];
    if (make_dir(dir) != 0)
        return;
    if (copy_to_dir(cfr_conpty_dll_path(g_bundle), dir, L"conpty.dll") != 0) {
        remove_tree(dir);
        return;
    }
    wide_to_utf8(dir, u8, sizeof(u8));

    CfrConpty *c = cfr_conpty_open_dir(u8);
    ASSERT_NOT_NULL(c);
    ASSERT_STR_EQ(cfr_conpty_backend(c), "system");
    ASSERT_EQ(cfr_conpty_has_openconsole(c), 0);
    ASSERT_TRUE(strstr(cfr_conpty_error(c), "no OpenConsole.exe") != NULL);
    cfr_conpty_free(c);
    remove_tree(dir);
}

/* A conpty.dll that is not a loadable image (the wrong-architecture case
 * looks the same to LoadLibraryExW: ERROR_BAD_EXE_FORMAT). */
static void test_wrong_arch_dll(void)
{
    static const char junk[] = "this file is not a PE image";
    wchar_t dir[PATH_W];
    char u8[PATH_W];
    if (make_dir(dir) != 0)
        return;
    if (write_file(dir, L"conpty.dll", junk, sizeof(junk) - 1) != 0 ||
        copy_to_dir(cfr_conpty_host_path(g_bundle), dir, L"OpenConsole.exe") !=
            0) {
        remove_tree(dir);
        return;
    }
    wide_to_utf8(dir, u8, sizeof(u8));

    CfrConpty *c = cfr_conpty_open_dir(u8);
    ASSERT_NOT_NULL(c);
    ASSERT_STR_EQ(cfr_conpty_backend(c), "system");
    ASSERT_TRUE(strstr(cfr_conpty_error(c), "LoadLibraryExW") != NULL);
    cfr_conpty_free(c);
    remove_tree(dir);
}

/* OpenConsole.exe is also looked up in a native-architecture subdirectory,
 * which is how the package lays itself out. */
static void test_arch_subdir_host(void)
{
    static const wchar_t *arches[] = { L"x64", L"arm64", L"x86" };
    SYSTEM_INFO si;
    const wchar_t *arch = NULL;
    wchar_t dir[PATH_W];
    wchar_t sub[PATH_W];
    char u8[PATH_W];

    GetNativeSystemInfo(&si);
    switch (si.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64:
        arch = arches[0];
        break;
    case PROCESSOR_ARCHITECTURE_ARM64:
        arch = arches[1];
        break;
    case PROCESSOR_ARCHITECTURE_INTEL:
        arch = arches[2];
        break;
    default:
        return;
    }

    if (make_dir(dir) != 0)
        return;
    if (copy_to_dir(cfr_conpty_dll_path(g_bundle), dir, L"conpty.dll") != 0) {
        remove_tree(dir);
        return;
    }
    join_path(sub, PATH_W, dir, arch);
    if (!CreateDirectoryW(sub, NULL) ||
        copy_to_dir(cfr_conpty_host_path(g_bundle), sub, L"OpenConsole.exe") !=
            0) {
        printf("    (skipping: cannot build the <arch> layout: %lu)\n",
               (unsigned long)GetLastError());
        remove_tree(dir);
        return;
    }

    wide_to_utf8(dir, u8, sizeof(u8));
    CfrConpty *c = cfr_conpty_open_dir(u8);
    ASSERT_NOT_NULL(c);
    ASSERT_STR_EQ(cfr_conpty_backend(c), "bundled");
    ASSERT_TRUE(path_is_under(cfr_conpty_host_path(c), sub));
    cfr_conpty_free(c);
    remove_tree(dir);
}

/* COFFER_CONPTY_DIR is consulted before the executable/compiled paths, and
 * an explicit override is consulted before the environment. */
static void test_search_order(void)
{
    wchar_t env_dir[PATH_W];
    wchar_t ovr_dir[PATH_W];
    char u8[PATH_W];
    if (make_valid_bundle(env_dir) != 0)
        return;
    if (make_valid_bundle(ovr_dir) != 0) {
        remove_tree(env_dir);
        return;
    }

    SetEnvironmentVariableW(L"COFFER_CONPTY_DIR", env_dir);

    CfrConpty *c = cfr_conpty_open(CFR_CONPTY_BUNDLED, NULL);
    ASSERT_NOT_NULL(c);
    ASSERT_STR_EQ(cfr_conpty_backend(c), "bundled");
    ASSERT_TRUE(path_is_under(cfr_conpty_dll_path(c), env_dir));
    cfr_conpty_free(c);

    wide_to_utf8(ovr_dir, u8, sizeof(u8));
    c = cfr_conpty_open(CFR_CONPTY_BUNDLED, u8);
    env_clear();
    ASSERT_NOT_NULL(c);
    ASSERT_STR_EQ(cfr_conpty_backend(c), "bundled");
    ASSERT_TRUE(path_is_under(cfr_conpty_dll_path(c), ovr_dir));
    cfr_conpty_free(c);

    remove_tree(env_dir);
    remove_tree(ovr_dir);
}

/* A broken explicit override must not stop the search: a later location
 * (the build tree in this test) still provides a usable bundle. */
static void test_broken_override_falls_through(void)
{
    wchar_t dir[PATH_W];
    char u8[PATH_W];
    if (make_dir(dir) != 0)
        return;
    if (write_file(dir, L"conpty.dll", "junk", 4) != 0) {
        remove_tree(dir);
        return;
    }
    wide_to_utf8(dir, u8, sizeof(u8));

    CfrConpty *c = cfr_conpty_open(CFR_CONPTY_BUNDLED, u8);
    ASSERT_NOT_NULL(c);
    ASSERT_STR_EQ(cfr_conpty_backend(c), "bundled");
    ASSERT_FALSE(path_is_under(cfr_conpty_dll_path(c), dir));
    cfr_conpty_free(c);
    remove_tree(dir);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    g_had_env =
        GetEnvironmentVariableW(L"COFFER_CONPTY_DIR", g_saved_env, PATH_W) > 0;
    env_clear();

    /* Copy source: whatever the normal search order resolves to here. */
    CfrConpty *src = cfr_conpty_open(CFR_CONPTY_AUTO, NULL);
    if (cfr_conpty_is_bundled(src))
        g_bundle = src;
    else
        cfr_conpty_free(src);

    printf("Running test_cfr_conpty\n");
    RUN_TEST(test_system_mode);
    RUN_TEST(test_system_mode_ignores_env);
    RUN_TEST(test_load_valid_dir);
    RUN_TEST(test_missing_openconsole);
    RUN_TEST(test_wrong_arch_dll);
    RUN_TEST(test_arch_subdir_host);
    RUN_TEST(test_search_order);
    RUN_TEST(test_broken_override_falls_through);

    if (g_bundle)
        cfr_conpty_free(g_bundle);
    if (g_had_env)
        SetEnvironmentVariableW(L"COFFER_CONPTY_DIR", g_saved_env);
    else
        env_clear();

    TEST_SUMMARY();
}

#else /* !_WIN32 */

#include <stdio.h>

int main(void)
{
    printf("test_cfr_conpty: Windows-only test, skipped\n");
    return 0;
}

#endif /* _WIN32 */
