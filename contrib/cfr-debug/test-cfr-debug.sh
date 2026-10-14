#!/bin/sh
# test-cfr-debug.sh - regression test for the cfr-debug binary.
#
# Exercises the render path (-q/--show-row: -q must restrict output to the
# specified rows, never suppress the render entirely) and the script
# vocabulary: wait-for (incl. the line-end anchor and UTF-8 needles), the
# unified buffer search that reaches the scrollback, await-exit /
# assert-exit, resize, and --env overrides.
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

tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT

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

# --- 5. --help-spec lists the script vocabulary ----------------------
# (the feature-detection seam downstream drivers use)
spec=$("$CFR_DEBUG" --help-spec 2>/dev/null)
for c in wait wait-for send raw assert-contains assert-not-contains \
	await-exit assert-exit resize render cursor; do
	case "$spec" in
	*"$(printf 'CMD\t%s' "$c")"*) ;;
	*) say_fail "--help-spec lacks CMD $c" ;;
	esac
done

# --- 6. wait-for + await-exit + assert-exit --------------------------
cat >"$tmp/exit.script" <<'EOF'
wait-for "EXIT-MARKER" 5
assert-contains "EXIT-MARKER"
assert-not-contains "NEVER-PRESENT"
await-exit 5
assert-exit 7
EOF
if ! "$CFR_DEBUG" -c 40 -r 6 -w 0 -f "$tmp/exit.script" \
	-- sh -c 'echo EXIT-MARKER; exit 7' >/dev/null 2>&1; then
	say_fail "wait-for/assert-exit round trip failed (want exit 7, rc 0)"
fi

# The negative: a status assertion that must fail.
cat >"$tmp/exit-bad.script" <<'EOF'
await-exit 5
assert-exit 0
EOF
if "$CFR_DEBUG" -c 40 -r 6 -w 0 -f "$tmp/exit-bad.script" \
	-- sh -c 'exit 7' >/dev/null 2>&1; then
	say_fail "assert-exit accepted the wrong status"
fi

# assert-exit without await-exit is a failure, not a pass.
cat >"$tmp/exit-none.script" <<'EOF'
assert-exit 0
EOF
if "$CFR_DEBUG" -c 40 -r 6 -w 0 -f "$tmp/exit-none.script" \
	-- sh -c 'sleep 0.2' >/dev/null 2>&1; then
	say_fail "assert-exit passed with no recorded status"
fi

# --- 7. the unified search reaches the scrollback --------------------
# 20 lines on a 6-row terminal: SB-0 is only in the scrollback, so the
# visible-only search this replaced would time out.
cat >"$tmp/scroll.script" <<'EOF'
wait-for "SB-0" 5
assert-contains "SB-0"
wait-for "SB-19" 5
render scrollback
EOF
out=$("$CFR_DEBUG" -c 40 -r 6 -w 0 -f "$tmp/scroll.script" \
	-- sh -c 'i=0; while [ $i -lt 20 ]; do echo "SB-$i"; i=$((i+1)); done; sleep 0.3' 2>/dev/null)
case "$out" in
*SB-0*) ;;
*) say_fail "render scrollback did not show SB-0" ;;
esac

# --- 8. UTF-8 needles (the nevermore prompt glyph is U+276F) ---------
cat >"$tmp/utf8.script" <<'EOF'
wait-for "❯" 5
wait-for "·" 5
assert-contains "❯ ok ·"
EOF
if ! "$CFR_DEBUG" -c 40 -r 4 -w 0 -f "$tmp/utf8.script" \
	-- sh -c 'printf "\342\235\257 ok \302\267\n"; sleep 0.2' >/dev/null 2>&1; then
	say_fail "UTF-8 needle (U+276F / U+00B7) not matched"
fi

# --- 9. line-end anchors the match to a row's end --------------------
cat >"$tmp/line-end.script" <<'EOF'
wait-for "ANCHOR" 5 line-end
EOF
if ! "$CFR_DEBUG" -c 40 -r 4 -w 0 -f "$tmp/line-end.script" \
	-- sh -c 'echo ANCHOR' >/dev/null 2>&1; then
	say_fail "line-end did not match a row ending with the needle"
fi
if "$CFR_DEBUG" -c 40 -r 4 -w 0 -f "$tmp/line-end.script" \
	-- sh -c 'echo ANCHOR-and-more' >/dev/null 2>&1; then
	say_fail "line-end matched a row with a suffix"
fi

# --- 10. resize reaches the child's tty ------------------------------
if command -v stty >/dev/null 2>&1; then
	cat >"$tmp/resize.script" <<'EOF'
wait-for "RS-START" 5
resize 60 10
wait-for "10 60" 5
EOF
	if ! "$CFR_DEBUG" -c 40 -r 6 -w 0 -f "$tmp/resize.script" \
		-- sh -c 'echo RS-START; while :; do stty size; sleep 0.2; done' \
		>/dev/null 2>&1; then
		say_fail "resize did not reach the child (stty size never showed 10 60)"
	fi
fi

# --- 11. --env overrides (and an empty value unsets) -----------------
out=$("$CFR_DEBUG" -c 60 -r 4 -w 2 --env CFRTEST=yes --env COLORTERM= \
	-- sh -c 'echo "CFRTEST=$CFRTEST"; echo "COLORTERM=[$COLORTERM]"' 2>/dev/null)
case "$out" in
*CFRTEST=yes*) ;;
*) say_fail "--env CFRTEST=yes did not reach the child" ;;
esac
case "$out" in
*"COLORTERM=[]"*) ;;
*) say_fail "--env COLORTERM= did not unset the built-in" ;;
esac

if [ "$fail" -ne 0 ]; then
	echo "test-cfr-debug.sh: $fail failure(s)" >&2
	exit 1
fi
echo "test-cfr-debug.sh: all checks passed" >&2
exit 0
