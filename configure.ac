dnl configure.ac - Autoconf configuration for bloom-vt
dnl Process this file with autoreconf -fi to produce a configure script.

AC_PREREQ([2.69])
AC_INIT([bloom-vt],
        m4_esyscmd_s([cat version 2>/dev/null || echo "0.0.0-unknown"]),
        [thomasc1971@hotmail.com],
        [bloom-vt],
        [https://github.com/thomas-christensen/bloom-vt])
AC_CONFIG_SRCDIR([src/term.c])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIRS([m4])
AC_USE_SYSTEM_EXTENSIONS
AC_CANONICAL_HOST

dnl Recompute the version at configure time so a rebuild after new commits
dnl picks up the current version without needing to re-run autoreconf. Falls
dnl back to PACKAGE_VERSION (set at autoreconf time via m4_esyscmd_s above)
dnl for tarball builds where build-aux/git-version.sh has nothing to read.
BLOOM_VT_VERSION=`"$srcdir/build-aux/git-version.sh" "$srcdir" 2>/dev/null`
AS_IF([test -z "$BLOOM_VT_VERSION"],
      [BLOOM_VT_VERSION="$PACKAGE_VERSION"])
AC_SUBST([BLOOM_VT_VERSION])
AC_MSG_NOTICE([bloom-vt version: $BLOOM_VT_VERSION])

AM_INIT_AUTOMAKE([1.14 foreign subdir-objects -Wall -Werror])
AM_SILENT_RULES([yes])

dnl Detect target platform
case "$host_os" in
  mingw*|msys*) host_windows=yes ;;
  *)            host_windows=no ;;
esac
AM_CONDITIONAL([HOST_WINDOWS], [test "x$host_windows" = "xyes"])

dnl Programs
user_cflags="$CFLAGS"
AC_PROG_CC
AM_PROG_AR
AC_PROG_RANLIB

dnl Build mode selection (--enable-release > --enable-debug > default ASan)
AC_ARG_ENABLE([debug],
  [AS_HELP_STRING([--enable-debug],
    [unsanitized debug build (-O0 -g3, no ASan)])],
  [enable_debug=$enableval],
  [enable_debug=no])

AC_ARG_ENABLE([release],
  [AS_HELP_STRING([--enable-release],
    [optimized release build (-O3 -DNDEBUG)])],
  [enable_release=$enableval],
  [enable_release=no])

if test "x$enable_release" = "xyes"; then
  CFLAGS="-O3 -DNDEBUG"
  AC_MSG_NOTICE([Release mode: CFLAGS set to -O3 -DNDEBUG])
elif test "x$enable_debug" = "xyes"; then
  CFLAGS="-O0 -g3"
  AC_MSG_NOTICE([Debug mode: CFLAGS set to -O0 -g3])
elif test "x$user_cflags" = "x"; then
  dnl ASan/UBSan is not available on Windows/MinGW. Fall back to
  dnl unsanitized debug when building natively on Windows (MinGW
  dnl does not ship libasan/libubsan).
  if test "x$host_windows" = "xyes"; then
    CFLAGS="-O1 -g3 -fno-omit-frame-pointer -fno-common"
    AC_MSG_NOTICE([Debug mode (no ASan on Windows): CFLAGS=$CFLAGS])
  else
    CFLAGS="-O1 -g3 -fno-omit-frame-pointer -fno-common -fsanitize=address,undefined -fno-sanitize-recover=undefined"
    LDFLAGS="-fsanitize=address,undefined $LDFLAGS"
    AC_MSG_NOTICE([ASan+UBSan mode (default): CFLAGS=$CFLAGS])
  fi
fi

dnl Headers / types
AC_CHECK_HEADERS([stdlib.h string.h])
AC_TYPE_SIZE_T

dnl Optional: openpty() lives in libutil on Linux/glibc, libc on macOS.
dnl On Windows, ConPTY replaces openpty entirely.
dnl test_bvt_pty links against it on POSIX; on Windows it uses pty_w32.c.
if test "x$host_windows" = "xno"; then
  AC_SEARCH_LIBS([openpty], [util], [], [
      AC_MSG_WARN([openpty() not found - test_bvt_pty will not link])
  ])
fi

dnl Optional: ThorVG for Lottie animation rasterization.
dnl When found, bloom-vt links libthorvg-1 and the lottie subsystem produces
dnl real RGBA pixels; otherwise the RGBA buffer is zeroed and APC sequences
dnl are accepted but produce no visible output.
AC_ARG_ENABLE([thorvg],
    [AS_HELP_STRING([--disable-thorvg],
        [disable ThorVG Lottie rasterization even if libthorvg-1 is found])],
    [enable_thorvg="$enableval"],
    [enable_thorvg=auto])

HAVE_THORVG=no
AS_IF([test "x$enable_thorvg" != xno], [
    PKG_CHECK_MODULES([THORVG], [thorvg-1], [
        HAVE_THORVG=yes
        AC_DEFINE([HAVE_THORVG], [1], [Define to 1 if ThorVG is available])
    ], [
        AS_IF([test "x$enable_thorvg" = xyes], [
            AC_MSG_ERROR([ThorVG was explicitly requested but thorvg-1 was not found])
        ])
        AC_MSG_WARN([thorvg-1 not found — Lottie rasterization will be disabled])
    ])
])
AM_CONDITIONAL([HAVE_THORVG], [test "x$HAVE_THORVG" = xyes])

dnl Propagate ThorVG dependency through pkg-config so consumers link it.
AS_IF([test "x$HAVE_THORVG" = xyes], [
    THORVG_REQUIRES="thorvg-1"
    THORVG_LIBS="$THORVG_LIBS"
], [
    THORVG_REQUIRES=""
    THORVG_LIBS=""
])
AC_SUBST([THORVG_REQUIRES])
AC_SUBST([THORVG_LIBS])

AC_CONFIG_FILES([
    Makefile
    src/Makefile
    include/Makefile
    tests/Makefile
    contrib/Makefile
    contrib/bvt-debug/Makefile
    bloom-vt.pc
])

AC_OUTPUT
