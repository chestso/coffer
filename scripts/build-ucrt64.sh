#!/bin/bash
# build-ucrt64.sh - build bloom-vt natively on Windows (MSYS2 UCRT64).
#
# Two MSYS2 quirks defeat a plain "./autogen.sh && ./configure && make" from a
# non-MSYS shell (git-bash, cmd, PowerShell):
#
#   1. /ucrt64 is only mounted inside a real MSYS2 UCRT64 shell. Outside one,
#      $MINGW_PREFIX is empty, /ucrt64 does not exist, and --prefix=/ucrt64
#      either installs to a wrong/scoop-relative location or configure aborts
#      with "unsafe srcdir value" because the backslashes in a Windows path
#      confuse automake's sanity check. We re-exec into a genuine UCRT64 shell
#      via msys2_shell.cmd so the mount table is populated and $MINGW_PREFIX
#      resolves to /ucrt64 before configure ever runs.
#
#   2. MSYS2's /usr/bin/sh and /usr/bin/bash are the same binary, but when bash
#      is invoked as "sh" (as libtoolize's #!/usr/bin/env sh does) its
#      POSIX-mode `test -d` builtin intermittently fails to see /ucrt64 paths
#      — a mount-table race that aborts autoreconf with "$pkgauxdir is not a
#      directory". Shadowing sh with a bash copy earlier on PATH sidesteps the
#      race so autoreconf works.
#
# Can be launched from any shell (git-bash, cmd, PowerShell, or an MSYS2
# shell); it re-execs into a real MSYS2 UCRT64 shell via msys2_shell.cmd.
#
# Usage:
#   ./scripts/build-ucrt64.sh            # autogen + configure + make + check
#   ./scripts/build-ucrt64.sh --install  # build, then make install
#
# Extra args are forwarded to configure, e.g.:
#   ./scripts/build-ucrt64.sh --enable-release
#   ./scripts/build-ucrt64.sh --disable-thorvg

set -eu

cd "$(dirname "$0")/.."

DO_INSTALL=0
CONFIGURE_ARGS=()
for arg in "$@"; do
	case "$arg" in
	--install) DO_INSTALL=1 ;;
	*) CONFIGURE_ARGS+=("$arg") ;;
	esac
done

# If not already inside an MSYS2 UCRT64 shell, re-exec into one via
# msys2_shell.cmd so /ucrt64 is mounted and /usr/bin/pacman resolves.
if [ -z "${MSYSTEM:-}" ] || [ "${MSYSTEM:-}" != "UCRT64" ]; then
	PACMAN_PATH="$(command -v pacman 2>/dev/null || true)"
	if [ -z "$PACMAN_PATH" ]; then
		echo "ERROR: pacman not found on PATH. Install MSYS2 and add its" >&2
		echo "       /usr/bin to PATH, or run this from an MSYS2 UCRT64 shell." >&2
		exit 1
	fi
	# pacman is at <MSYS2_ROOT>/usr/bin/pacman; msys2_shell.cmd is at
	# <MSYS2_ROOT>/msys2_shell.cmd — two directories up from usr/bin.
	PACMAN_DIR="$(cd "$(dirname "$PACMAN_PATH")" && pwd)"
	MSYS2_ROOT="$(cd "$PACMAN_DIR/../.." && pwd)"
	MSYS2_SHELL="$MSYS2_ROOT/msys2_shell.cmd"
	if [ ! -f "$MSYS2_SHELL" ]; then
		echo "ERROR: msys2_shell.cmd not found at $MSYS2_SHELL" >&2
		exit 1
	fi
	REPO_WIN="$(cygpath -w "$(pwd)" 2>/dev/null || echo "$(pwd)")"
	exec cmd.exe //c "$MSYS2_SHELL" -ucrt64 -defterm -no-start -here \
		-c "cd '$REPO_WIN' && ./scripts/build-ucrt64.sh $*"
fi

# --- Inside the MSYS2 UCRT64 shell from here on ------------------------------

echo "==> MSYS2 UCRT64 shell (MSYSTEM=$MSYSTEM)"

# Workaround for the /usr/bin/sh test -d mount race (see header comment).
FIXSH_DIR="$(mktemp -d /tmp/bloom-fixsh.XXXXXX)"
cp /usr/bin/bash "$FIXSH_DIR/sh"
export PATH="$FIXSH_DIR:$PATH"
trap 'rm -rf "$FIXSH_DIR"' EXIT
echo "==> Applied sh workaround (shadowed /usr/bin/sh with bash copy)"

# Ensure build dependencies are installed. ThorVG is optional (Lottie
# rasterization); configure auto-detects it, so install it by default but
# allow skipping with --disable-thorvg via a no-op (configure handles that).
echo "==> Ensuring build dependencies (pacman -S --needed)"
pacman -S --noconfirm --needed \
	mingw-w64-ucrt-x86_64-gcc \
	mingw-w64-ucrt-x86_64-autotools \
	mingw-w64-ucrt-x86_64-thorvg 2>&1 | tail -3 || true

# Always regenerate the autotools files so maintainer-mode rebuilds don't
# trigger aclocal with a broken m4 path mid-build. Use autogen.sh (not bare
# autoreconf) so ./version is written from git first — autoreconf alone would
# bake 0.0.0-unknown into PACKAGE_VERSION on a fresh clone (version is
# gitignored).
echo "==> ./autogen.sh"
./autogen.sh

echo "==> configure"
rm -rf build
mkdir build
(cd build && sh ../configure --prefix="$MINGW_PREFIX" "${CONFIGURE_ARGS[@]}")

echo "==> make -j$(nproc)"
make -C build -j"$(nproc)"

echo "==> make check"
make -C build check

if [ "$DO_INSTALL" -eq 1 ]; then
	echo "==> make install"
	make -C build install
fi

echo "==> Build complete: build/src/libbloom-vt.a"
