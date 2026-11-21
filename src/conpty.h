/*
 * conpty.h — bundled ConPTY loader for the coffer CLI (Windows only).
 *
 * The Microsoft.Windows.Console.ConPTY NuGet package ships an updated
 * conpty.dll + OpenConsole.exe whose VtEngine passes DCS (sixel), APC
 * (kitty/Lottie) and unknown OSC sequences through unmodified.  The system
 * conhost.exe strips DCS and APC.
 *
 * We load conpty.dll explicitly from a directory next to coffer.exe (or an
 * installed libexec directory), never from the current directory, and fall
 * back to the system CreatePseudoConsole when the bundle is absent or
 * unusable.
 */

#ifndef COFFER_CONPTY_H
#define COFFER_CONPTY_H

#ifdef _WIN32

#include <windows.h>

typedef enum
{
    CFR_CONPTY_AUTO = 0, /* use the bundle if found, else system */
    CFR_CONPTY_BUNDLED,  /* require the bundle */
    CFR_CONPTY_SYSTEM    /* force the system implementation */
} CfrConptyMode;

typedef struct CfrConpty CfrConpty;

/** Open the loader.  Returns NULL only on allocation failure.  In AUTO mode
 *  a system-backed loader is returned when no bundle exists; in BUNDLED mode
 *  the returned object is system-backed with cfr_conpty_error() set, so the
 *  caller can report why the strict request failed. */
CfrConpty *cfr_conpty_open(CfrConptyMode mode, const char *override_dir);

/** Open exactly `dir`, ignoring every other search location (the loader's
 *  other branches are covered by feeding this a directory that is missing
 *  OpenConsole.exe, holds an unloadable conpty.dll, or nests the host under
 *  <arch>/).  Used by the loader unit tests; the CLI always goes through
 *  cfr_conpty_open(). */
CfrConpty *cfr_conpty_open_dir(const char *dir);

/** Release the DLL (if any).  Safe with NULL. */
void cfr_conpty_free(CfrConpty *c);

/** Create / resize / close a pseudo-console through the selected backend. */
HRESULT cfr_conpty_create(CfrConpty *c, COORD size, HANDLE input,
                          HANDLE output, DWORD flags, HPCON *out);
HRESULT cfr_conpty_resize(CfrConpty *c, HPCON pc, COORD size);
void cfr_conpty_close_pc(CfrConpty *c, HPCON pc);

/** Diagnostics.  Strings are owned by the loader and valid until free. */
const char *cfr_conpty_backend(const CfrConpty *c); /* "bundled" / "system" */
const char *cfr_conpty_version(const CfrConpty *c); /* lock version or NULL */
const char *cfr_conpty_dll_path(const CfrConpty *c);
const char *cfr_conpty_host_path(const CfrConpty *c);
const char *cfr_conpty_error(const CfrConpty *c);
const char *cfr_conpty_dll_version(const CfrConpty *c);
const char *cfr_conpty_host_version(const CfrConpty *c);

/** True when OpenConsole.exe is actually present and selected.  This is
 *  intentionally stronger than "the DLL loaded": conpty.dll silently falls
 *  back to conhost.exe when OpenConsole.exe is missing. */
int cfr_conpty_has_openconsole(const CfrConpty *c);

/** True when the bundled backend is active. */
int cfr_conpty_is_bundled(const CfrConpty *c);

#endif /* _WIN32 */

#endif /* COFFER_CONPTY_H */
