#!/bin/sh
# test-cfr-debug.sh - regression test for the cfr-debug binary.
#
# Exercises the non-script-mode render path, including the -q/--quiet
# combination with -s/--show-row: -q must restrict output to the
# specified rows, never suppress the render entirely.
#
# Requires a POSIX pty; skipped on Windows and in CI environments
# without a shell. The cfr-debug binary is taken from the build tree
# ($CFR_DEBUG, default: $top_builddir/contrib/cfr-debug/cfr-debug).

# The automake test driver runs from the build dir but may pass the
# script by its srcdir path, so try cwd first, then $0's directory.
CFR_DEBUG=${CFR_DEBUG:-./cfr-debug}
if [ ! -x "$CFR_DEBUG" ]; then
	CFR_DEBUG="$(dirname "$0")"/cfr-debug
fi

fail=0
say_fail() {
	echo "FAIL: $*" >&2
	fail=$((fail + 1))
}

if [ ! -x "$CFR_DEBUG" ]; then
	echo "SKIP: cfr-debug binary not found ($CFR_DEBUG)" >&2
	exit 77
fi

# Child prints a marker so the grid has predictable content on rows 0 and 1.
CHILD='printf "row-zero-marker\nrow-one-marker\n"; sleep 1'

# --- 1. plain render shows the marker -------------------------------
out=$("$CFR_DEBUG" -c 40 -r 6 -w 2 -- sh -c "$CHILD" 2>/dev/null)
case "$out" in
*row-zero-marker*) ;;
*) say_fail "plain render missing row-zero-marker" ;;
esac

# --- 2. -q without -s is a full dump (no rows to filter to) ----------
out=$("$CFR_DEBUG" -c 40 -r 6 -w 2 -q -- sh -c "$CHILD" 2>/dev/null)
case "$out" in
*row-zero-marker*) ;;
*) say_fail "-q without -s suppressed the render" ;;
esac

# --- 3. -q with -s dumps only the requested row (the regression) ----
out=$("$CFR_DEBUG" -c 40 -r 6 -w 2 -q -s 1 -- sh -c "$CHILD" 2>/dev/null)
case "$out" in
*row-one-marker*) ;;
*) say_fail "-q -s 1 missing the requested row 1" ;;
esac
case "$out" in
*row-zero-marker*) say_fail "-q -s 1 leaked row 0" ;;
esac

# --- 4. -s without -q dumps the requested row too --------------------
out=$("$CFR_DEBUG" -c 40 -r 6 -w 2 -s 1 -- sh -c "$CHILD" 2>/dev/null)
case "$out" in
*row-one-marker*) ;;
*) say_fail "-s 1 missing the requested row 1" ;;
esac

if [ "$fail" -ne 0 ]; then
	echo "test-cfr-debug.sh: $fail failure(s)" >&2
	exit 1
fi
echo "test-cfr-debug.sh: all checks passed" >&2
exit 0
