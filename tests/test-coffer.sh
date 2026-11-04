#!/bin/sh
# test-coffer.sh - regression test for the coffer CLI.
#
# Exercises the render path (-q/--show-row: -q must restrict output to the
# specified rows, never suppress the render entirely) and the script
# vocabulary: wait-for (incl. the line-end anchor and UTF-8 needles), the
# unified buffer search that reaches the scrollback, await-exit /
# assert-exit, resize, and --env overrides. Also covers replaying a capture
# with no child (--feed, incl. the PTY commands that degrade without one)
# and the kitty keyboard protocol dump (-k / dump-kbd).
#
# Requires a POSIX pty; skipped on Windows and in CI environments
# without a shell. The binary is taken from the build tree
# ($COFFER_BIN, default: ../src/coffer, falling back to this script's
# sibling ../src/coffer).

# The automake test driver runs from the build dir but may pass the
# script by its srcdir path, so try cwd-relative first, then relative
# to $0.
COFFER=${COFFER_BIN:-../src/coffer}
if [ ! -x "$COFFER" ]; then
	COFFER="$(dirname "$0")"/../src/coffer
fi

fail=0
say_fail() {
	echo "FAIL: $*" >&2
	fail=$((fail + 1))
}

if [ ! -x "$COFFER" ]; then
	echo "SKIP: coffer binary not found ($COFFER)" >&2
	exit 77
fi

tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT

# Child prints a marker so the grid has predictable content on rows 0 and 1.
CHILD='printf "row-zero-marker\nrow-one-marker\n"; sleep 1'

# --- 1. plain render shows the marker -------------------------------
out=$("$COFFER" -c 40 -r 6 -w 2 -- sh -c "$CHILD" 2>/dev/null)
case "$out" in
*row-zero-marker*) ;;
*) say_fail "plain render missing row-zero-marker" ;;
esac

# --- 2. -q without -s is a full dump (no rows to filter to) ----------
out=$("$COFFER" -c 40 -r 6 -w 2 -q -- sh -c "$CHILD" 2>/dev/null)
case "$out" in
*row-zero-marker*) ;;
*) say_fail "-q without -s suppressed the render" ;;
esac

# --- 3. -q with -s dumps only the requested row (the regression) ----
out=$("$COFFER" -c 40 -r 6 -w 2 -q -s 1 -- sh -c "$CHILD" 2>/dev/null)
case "$out" in
*row-one-marker*) ;;
*) say_fail "-q -s 1 missing the requested row 1" ;;
esac
case "$out" in
*row-zero-marker*) say_fail "-q -s 1 leaked row 0" ;;
esac

# --- 4. -s without -q dumps the requested row too --------------------
out=$("$COFFER" -c 40 -r 6 -w 2 -s 1 -- sh -c "$CHILD" 2>/dev/null)
case "$out" in
*row-one-marker*) ;;
*) say_fail "-s 1 missing the requested row 1" ;;
esac

# --- 5. --help-spec lists the script vocabulary ----------------------
# (the feature-detection seam downstream drivers use)
spec=$("$COFFER" --help-spec 2>/dev/null)
for c in wait wait-for send raw assert-contains assert-not-contains \
	await-exit assert-exit resize render cursor dump-sixel dump-kbd; do
	case "$spec" in
	*"$(printf 'CMD\t%s' "$c")"*) ;;
	*) say_fail "--help-spec lacks CMD $c" ;;
	esac
done
# The options a driver probes for before passing them.
for f in --feed --kbd-data; do
	case "$spec" in
	*"$(printf 'FLAG\t%s' "$f")"*) ;;
	*) say_fail "--help-spec lacks FLAG $f" ;;
	esac
done
case "$spec" in
*"$(printf 'FLAG\t--kbd-data\t-k')"*) ;;
*) say_fail "--help-spec lost the -k alias for --kbd-data" ;;
esac

# --- 6. wait-for + await-exit + assert-exit --------------------------
cat >"$tmp/exit.script" <<'EOF'
wait-for "EXIT-MARKER" 5
assert-contains "EXIT-MARKER"
assert-not-contains "NEVER-PRESENT"
await-exit 5
assert-exit 7
EOF
if ! "$COFFER" -c 40 -r 6 -w 0 -f "$tmp/exit.script" \
	-- sh -c 'echo EXIT-MARKER; exit 7' >/dev/null 2>&1; then
	say_fail "wait-for/assert-exit round trip failed (want exit 7, rc 0)"
fi

# The negative: a status assertion that must fail.
cat >"$tmp/exit-bad.script" <<'EOF'
await-exit 5
assert-exit 0
EOF
if "$COFFER" -c 40 -r 6 -w 0 -f "$tmp/exit-bad.script" \
	-- sh -c 'exit 7' >/dev/null 2>&1; then
	say_fail "assert-exit accepted the wrong status"
fi

# assert-exit without await-exit is a failure, not a pass.
cat >"$tmp/exit-none.script" <<'EOF'
assert-exit 0
EOF
if "$COFFER" -c 40 -r 6 -w 0 -f "$tmp/exit-none.script" \
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
out=$("$COFFER" -c 40 -r 6 -w 0 -f "$tmp/scroll.script" \
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
if ! "$COFFER" -c 40 -r 4 -w 0 -f "$tmp/utf8.script" \
	-- sh -c 'printf "\342\235\257 ok \302\267\n"; sleep 0.2' >/dev/null 2>&1; then
	say_fail "UTF-8 needle (U+276F / U+00B7) not matched"
fi

# --- 9. line-end anchors the match to a row's end --------------------
cat >"$tmp/line-end.script" <<'EOF'
wait-for "ANCHOR" 5 line-end
EOF
if ! "$COFFER" -c 40 -r 4 -w 0 -f "$tmp/line-end.script" \
	-- sh -c 'echo ANCHOR' >/dev/null 2>&1; then
	say_fail "line-end did not match a row ending with the needle"
fi
if "$COFFER" -c 40 -r 4 -w 0 -f "$tmp/line-end.script" \
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
	if ! "$COFFER" -c 40 -r 6 -w 0 -f "$tmp/resize.script" \
		-- sh -c 'echo RS-START; while :; do stty size; sleep 0.2; done' \
		>/dev/null 2>&1; then
		say_fail "resize did not reach the child (stty size never showed 10 60)"
	fi
fi

# --- 11. --env overrides (and an empty value unsets) -----------------
out=$("$COFFER" -c 60 -r 4 -w 2 --env CFRTEST=yes --env COLORTERM= \
	-- sh -c 'echo "CFRTEST=$CFRTEST"; echo "COLORTERM=[$COLORTERM]"' 2>/dev/null)
case "$out" in
*CFRTEST=yes*) ;;
*) say_fail "--env CFRTEST=yes did not reach the child" ;;
esac
case "$out" in
*"COLORTERM=[]"*) ;;
*) say_fail "--env COLORTERM= did not unset the built-in" ;;
esac

# --- 12. --feed replays a capture with no child ----------------------
# The file carries what a child would have printed. With no command after
# --, nothing is spawned, so the grid comes from the file alone.
printf 'fed-zero\nfed-one\n' >"$tmp/feed.raw"
out=$("$COFFER" -c 40 -r 6 --feed "$tmp/feed.raw" 2>/dev/null)
case "$out" in
*"0|fed-zero"*) ;;
*) say_fail "--feed did not replay fed-zero onto row 0" ;;
esac
case "$out" in
*"1|fed-one"*) ;;
*) say_fail "--feed did not replay fed-one onto row 1" ;;
esac

# A script can read the replayed grid: this is the CI use of --feed.
cat >"$tmp/feed.script" <<'EOF'
assert-contains "fed-one"
EOF
if ! "$COFFER" -c 40 -r 6 --feed "$tmp/feed.raw" -f "$tmp/feed.script" \
	>/dev/null 2>&1; then
	say_fail "a script could not assert on the replayed grid"
fi

# A command after -- still spawns a child, and the file is fed first.
out=$("$COFFER" -c 40 -r 6 -w 1 --feed "$tmp/feed.raw" \
	-- sh -c 'echo child-marker' 2>/dev/null)
case "$out" in
*"0|fed-zero"*) ;;
*) say_fail "--feed with a command lost the fed row 0" ;;
esac
case "$out" in
*"2|child-marker"*) ;;
*) say_fail "--feed with a command lost the child's output" ;;
esac

# --- 13. the PTY side degrades on a feed-only run --------------------
# send warns and skips instead of failing; await-exit and resize fail,
# because there is no child to wait for and no tty to resize.
cat >"$tmp/feed-send.script" <<'EOF'
send "ignored\n"
EOF
if ! err=$("$COFFER" -c 40 -r 6 --feed "$tmp/feed.raw" \
	-f "$tmp/feed-send.script" 2>&1 >/dev/null); then
	say_fail "send on a feed-only run failed the run"
fi
case "$err" in
*"WARN: send"*) ;;
*) say_fail "send on a feed-only run did not warn" ;;
esac

for cmd in 'await-exit 5' 'resize 60 10'; do
	printf '%s\n' "$cmd" >"$tmp/feed-pty.script"
	if "$COFFER" -c 40 -r 6 --feed "$tmp/feed.raw" \
		-f "$tmp/feed-pty.script" >/dev/null 2>&1; then
		say_fail "$cmd passed with no PTY behind it"
	fi
done

# --output records bytes drained from the PTY, so a feed leaves it empty.
"$COFFER" -c 40 -r 6 --feed "$tmp/feed.raw" -o "$tmp/feed.out" \
	>/dev/null 2>&1
if [ ! -e "$tmp/feed.out" ]; then
	say_fail "--output did not create its file on a feed-only run"
elif [ -s "$tmp/feed.out" ]; then
	say_fail "--output recorded bytes on a feed-only run"
fi

# --- 14. -k reports the kitty keyboard protocol state ----------------
printf '\033[>1u' >"$tmp/kbd-push.raw"
printf '\033[>1u\033[<1u' >"$tmp/kbd-pop.raw"

out=$("$COFFER" -c 40 -r 4 --feed "$tmp/feed.raw" -k 2>/dev/null)
case "$out" in
"kbd: flags=0x0 (0) depth=0 off"*) ;;
*) say_fail "-k did not report an untouched keyboard protocol" ;;
esac

# CSI > 1 u pushes the disambiguate flag. The stack depth is what tells a
# leak (a program that died without popping) from a clean exit.
out=$("$COFFER" -c 40 -r 4 --feed "$tmp/kbd-push.raw" -k 2>/dev/null)
case "$out" in
*"kbd: flags=0x1 (1) depth=1 on"*) ;;
*) say_fail "-k missed a pushed kitty keyboard protocol" ;;
esac

out=$("$COFFER" -c 40 -r 4 --feed "$tmp/kbd-pop.raw" -k 2>/dev/null)
case "$out" in
*"depth=0 off"*) ;;
*) say_fail "-k did not see the protocol popped back to the baseline" ;;
esac

# --- 15. dump-kbd reports the same state from a script ---------------
cat >"$tmp/kbd.script" <<'EOF'
dump-kbd
EOF
out=$("$COFFER" -c 40 -r 4 --feed "$tmp/kbd-push.raw" \
	-f "$tmp/kbd.script" 2>/dev/null)
case "$out" in
*"kbd: flags=0x1 (1) depth=1 on"*) ;;
*) say_fail "dump-kbd did not print the protocol state" ;;
esac

if [ "$fail" -ne 0 ]; then
	echo "test-coffer.sh: $fail failure(s)" >&2
	exit 1
fi
echo "test-coffer.sh: all checks passed" >&2
exit 0
