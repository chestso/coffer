#!/bin/sh
# test-coffer-w32.sh - minimal Windows CLI regression for the ConPTY backend.
#
# The full test-coffer.sh needs a POSIX pty and a POSIX shell; this script
# only checks that the Windows CLI starts a child and reports its backend
# consistently.  Automake runs it from build/tests.

COFFER=${COFFER_BIN:-../src/coffer.exe}
if [ ! -x "$COFFER" ]; then
    COFFER="$(dirname "$0")"/../src/coffer.exe
fi
if [ ! -x "$COFFER" ]; then
    echo "SKIP: coffer.exe not found ($COFFER)" >&2
    exit 77
fi

fail=0
say_fail() {
    echo "FAIL: $*" >&2
    fail=$((fail + 1))
}

# --- child spawn ------------------------------------------------------
out=$("$COFFER" --wait 2 -- hostname.exe 2>/dev/null)
case "$out" in
*[A-Za-z0-9]*) ;;
*) say_fail "child produced no output" ;;
esac

# --- default backend is self-consistent ------------------------------
info=$("$COFFER" --conpty-info 2>/dev/null)
case "$info" in
*"backend=bundled"*)
    case "$info" in
    *"passthrough=yes"*) ;;
    *) say_fail "bundled backend does not report passthrough" ;;
    esac
    ;;
*"backend=system"*)
    case "$info" in
    *"passthrough=no"*) ;;
    *) say_fail "system backend claims passthrough" ;;
    esac
    ;;
*) say_fail "--conpty-info lacks a backend line" ;;
esac

# --- forced system fallback ------------------------------------------
info=$("$COFFER" --conpty system --conpty-info 2>/dev/null)
case "$info" in
*"backend=system"*) ;;
*) say_fail "--conpty system did not select system" ;;
esac
case "$info" in
*"passthrough=no"*) ;;
*) say_fail "--conpty system claims passthrough" ;;
esac

# --- environment override --------------------------------------------
info=$(COFFER_CONPTY=system "$COFFER" --conpty-info 2>/dev/null)
case "$info" in
*"backend=system"*) ;;
*) say_fail "COFFER_CONPTY=system did not select system" ;;
esac

# --- bad mode is rejected --------------------------------------------
if "$COFFER" --conpty nonsense --conpty-info >/dev/null 2>&1; then
    say_fail "--conpty nonsense was accepted"
fi

if [ "$fail" -ne 0 ]; then
    exit 1
fi
echo "test-coffer-w32: all checks passed"
exit 0
