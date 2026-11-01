# coffer — headless PTY inspector for the coffer VT engine

`coffer` spawns a child process on a pseudo-terminal, pipes its
output through the coffer engine, and renders the resulting grid as
plain text.
It needs no display server, and is useful for:

- Debugging VT rendering
- Capturing raw PTY output for offline replay
- Inspecting individual grid cells (character, codepoint, width, style)
- Validating that TUI applications render correctly through coffer
- CI assertions: verify that specific text appears on screen

The coffer output callback is wired back to the PTY, so terminal
queries (DA, DSR, kitty keyboard protocol) receive proper replies.
This allows interactive TUI applications to render correctly without a
real terminal emulator.

This document is the long-form companion to the `coffer(1)` man page
(`src/coffer.1`); the two are kept in agreement. The man page is the
terse reference, this file adds rationale and worked examples.

## Building

From a coffer build tree:

```sh
./autogen.sh && ./configure && make
```

On Windows (MSYS2 UCRT64), use `scripts/build-ucrt64.sh`. The tool
uses ConPTY on Windows and `forkpty` on POSIX. On Windows, a
background reader thread does blocking `ReadFile` on the ConPTY
output pipe and signals an event when data arrives, because
anonymous pipes cannot be waited on with `WaitForMultipleObjects`.

The binary is built from `src/coffer.c` and linked against
`libcoffer.a`; `make` builds both, and `make install` installs the
binary to `$(bindir)` and `src/coffer.1` as its man page.

## Usage

```
coffer [options] [-- command args...]
```

Everything after `--` is the command to run. Without one, coffer
starts `$SHELL` (falling back to `/bin/sh`; `%COMSPEC%` or `cmd.exe` on
Windows). Options must precede `--`, and short options cannot be
combined.

### A run, step by step

1. Spawn the child on a PTY of the requested size.
2. Drain its output for the initial wait (`--wait`).
3. Execute the script, if `--script` was given.
4. Dump the final screen (see [Final dump](#final-dump)), unless the
   script failed.
5. Kill the child if it is still running, and exit.

Progress and diagnostics (the startup notice, per-command `OK` / `FAIL`
lines, script errors) go to standard error. Grid dumps go to standard
output.

### Options

| Flag                   | Description                                                     |
| ---------------------- | --------------------------------------------------------------- |
| `-r`, `--rows ROWS`    | Terminal rows (default: 24)                                     |
| `-c`, `--cols COLS`    | Terminal columns (default: 80)                                  |
| `-w`, `--wait SEC`     | Initial wait before the script or render (default: 3; decimals) |
| `-f`, `--script FILE`  | Script file (one command per line, see below)                   |
| `-o`, `--output FILE`  | Save raw PTY output to file                                     |
| `-s`, `--show-row ROW` | Select a row for the final dump (repeatable, up to 64)          |
| `-d`, `--cell-data`    | After the final dump, print per-cell detail                     |
| `-S`, `--style-data`   | After the final dump, print style detail                        |
| `-q`, `--quiet`        | With `-s`, print only the selected rows                         |
| `--env KEY=VALUE`      | Set a child env var (repeatable, up to 64)                      |
| `-h`, `--help`         | Show help (options and script commands) and exit                |
| `--help-spec`          | Emit machine-readable flag + command listing and exit           |

The initial wait always happens, with or without a script; a script that
synchronizes with `wait-for` can pass a small value (`-w 0.5`).

`--show-row` without `--quiet` still prints the full screen: it only
restricts the `--cell-data` / `--style-data` detail to the selected
rows. `--quiet` has no effect without `--show-row`. `--cell-data`
prints what `render cells` does (codepoint, width, style, hyperlink id)
and `--style-data` what `render styles` does, for every row or the
selected ones.

`--env` is applied after the built-ins (`TERM`, `COLORTERM`,
`TERM_PROGRAM`), and an empty value unsets the variable — including a
built-in, which is how a test pins a color tier:

```sh
coffer --env COLORTERM= -- my-tui
```

### Final dump

When no script is given, or the script finishes without failing
(including via `quit`), coffer renders the visible grid to standard
output after the script, one row per line as `ROW|TEXT` (the same
output as a bare `render`). `--show-row`, `--cell-data`, `--style-data`
and `--quiet` shape this dump only; they do not affect `render`
commands inside a script. When the script fails, the dump is skipped
and the exit status is 1.

### Script file

The `-f`/`--script FILE` option reads a script with one command per
line. Blank lines and lines whose first non-blank character is `#` are
ignored; a line may be at most 4095 bytes. A parse error (unknown
command, bad arguments) is reported with its line number and fails the
run before the script starts.

The script engine lives in the coffer library
(`include/coffer/script.h`) and is shared with other consumers (e.g.
portty's `-S` mode, which adds its own UI commands as extensions).

Commands that take a `TEXT` argument accept it with or without one
layer of surrounding single or double quotes.

| Command                                | Description                                                                      |
| -------------------------------------- | -------------------------------------------------------------------------------- |
| `wait SECONDS`                         | Drain PTY for N seconds (decimals; missing or non-positive means 1)              |
| `wait-for "TEXT" [TIMEOUT] [line-end]` | Drain until TEXT appears (unified buffer); TIMEOUT (default 600) is a failure    |
| `send TEXT`                            | Send text to PTY input (child's stdin); escapes expanded (see below)             |
| `sendln TEXT`                          | Like `send`, with a trailing CR LF                                               |
| `raw HEX [HEX ...]`                    | Send literal bytes to PTY input (e.g. `raw ff fc 01` for IAC WONT ECHO)          |
| `emit TEXT`                            | Feed text straight into coffer's parser, bypassing the child (escapes expanded)  |
| `emit-raw HEX [HEX ...]`               | Feed literal bytes straight into coffer's parser                                 |
| `assert-contains TEXT`                 | Fail (exit 1) if the unified buffer lacks TEXT (literal, no escape expansion)    |
| `assert-not-contains TEXT`             | Fail (exit 1) if the unified buffer contains TEXT                                |
| `await-exit [TIMEOUT]`                 | Drain until the child exits (default 10 s; timeout fails), recording its status  |
| `assert-exit STATUS`                   | Fail (exit 1) unless `await-exit` recorded STATUS (or none ran)                  |
| `resize COLS ROWS`                     | Resize the PTY and the grid; both positive (the child gets SIGWINCH on POSIX)    |
| `render [visible\|scrollback\|all]`    | Dump the grid (default: `visible`; `scrollback` = oldest first, unified rows)    |
| `render cells ROW [C0 C1]`             | Dump per-cell detail (codepoint, width, attrs, colors, underline, hyperlink)     |
| `render styles ROW`                    | Dump style detail (underline, colors, attrs) for the cells of ROW that carry any |
| `cursor`                               | Dump cursor state (row, col, visible, blink)                                     |
| `dump-sixel`                           | Dump live images and placements (id, anchor row/col, pixel size)                 |
| `quit`                                 | End the script run early (success); the final dump still happens                 |
| `# comment`                            | Skipped                                                                          |

`render` prints each row as `ROW|TEXT`. Rows are unified coordinates:
`>= 0` is a visible row, `< 0` is scrollback depth (`-1` = the line
most recently scrolled off), so `render cells -1` inspects the most
recent scrollback line. `render scrollback` prints `(scrollback empty)`
when there is none; `render all` prints the scrollback followed by the
visible rows. `render cells` covers columns `C0` through `C1` (default:
the whole row).

`dump-sixel` assumes a 10x6 pixel cell, which determines the cell
footprint of a sixel.

`send`, `sendln` and `emit` expand backslash escapes in their argument:
`\n` (LF), `\r` (CR), `\t` (TAB), `\e` (ESC), `\xNN` (byte NN, hex) and
`\\` (backslash). A malformed `\x` keeps its backslash. Octal escapes
such as `\033` are deliberately **not** expanded — write `\e`. The
`wait-for` needle expands the same escapes; `assert-contains` and
`assert-not-contains` take their text literally.

`wait-for` is the synchronization primitive: `wait 3` guesses, `wait-for`
waits on a fact and fails loudly on timeout, so a script has no fixed
sleeps to tune. Its options are trailing tokens (a bare number, the
keyword `line-end`), peeled off the end of the line right to left (at
most one number); anything else is part of the needle. The grammar is
the same one portty's `-S` scripts use:

```
wait-for "ready>" 10          # needle, 10 s budget
wait-for "done" 10 line-end   # ... and the row must END with it
```

#### The unified buffer

`assert-contains`, `assert-not-contains` and `wait-for` search the
**unified buffer**: the scrollback (oldest first) followed by the
visible rows. A full-screen TUI whose transcript has scrolled off the
screen is therefore still assertable. Needles are matched as UTF-8, so
a prompt glyph like `❯` (U+276F) matches.

That is a change from earlier releases, where the search covered the
visible grid only: `assert-contains` is now more permissive and
`assert-not-contains` stricter. `render` keeps dumping the visible rows
by default.

#### Feature detection

`--help-spec` prints one line per option and one per script command,
tab-separated, with `-` standing for an absent value:

```
FLAG	NAME	ALIAS	TAKES_VALUE	PARAM	DESCRIPTION
CMD	NAME	ARGS	DESCRIPTION
```

`render cells` and `render styles` get their own `CMD` lines. A driver
that needs a command greps for `CMD<TAB>NAME` and skips (77) when the
installed binary predates it, instead of guessing from a version number.

#### `send` vs `raw`

Both `send` and `raw` write to the **PTY input** (the child process's
stdin), not to coffer's terminal parser. This is the most common source
of confusion:

- **`send`** writes text with escape expansion (`\n`→LF, `\e`→ESC, etc.).
  Use it to type shell commands — append `\n` to execute them (or use
  `sendln`, which appends CR LF).
- **`raw`** writes literal hex bytes with no expansion.
  Use it for binary input the child should receive verbatim.

Neither can directly emit escape sequences that coffer will parse,
because the bytes go to the child's stdin. The child must output them on
its stdout for coffer to process them. To make the terminal handle an
escape sequence, have the child print it, or use `emit` / `emit-raw`,
which bypass the child entirely:

```
# Child prints the sequence; coffer parses it
send printf '\033[?12l'\n
wait 1
send printf '\033[?12h'\n

# Direct: no child involved
emit \e[?12l

# Wrong: raw writes ESC [ ? 1 2 l to stdin; the shell sees them as input,
# not as terminal control sequences
raw 1b 5b 3f 31 32 6c
```

Key points:

- A shell only runs what you type once it sees a newline: end `send`
  commands meant for the shell with `\n`, or the text sits on the input
  line unexecuted.
- Inside `printf` arguments write `\033`, not `\e`. The script parser
  does not expand `\033`, so the shell receives the literal text `\033`
  and `printf` turns it into ESC. A `\e` would be expanded by the script
  parser into a raw ESC byte that the shell's readline swallows.
- `raw` is useful for sending non-printable input to the child (e.g.
  `raw 03` for Ctrl-C, `raw 1a` for Ctrl-Z).

### Examples

**Interactive shell:**

```sh
coffer -- bash -l
```

**Capture initial screen of crush (short flags):**

```sh
coffer -c 68 -r 20 -w 5 -- crush
```

**Capture initial screen of crush (long flags):**

```sh
coffer --cols 68 --rows 20 --wait 5 -- crush
```

**Capture raw output:**

```sh
coffer -c 68 -r 20 -o /tmp/crush.raw -- crush
```

**Inspect one row's cells, hiding the rest of the screen:**

```sh
coffer -c 68 -r 20 -s 18 -d -q -- crush
```

**Scripted CI test with assertions:**

```sh
coffer -r 24 -c 80 -w 0.5 -f test.script -- sh -c "echo hello world"
```

Where `test.script` contains:

```
wait-for "hello world" 5
assert-contains "hello world"
await-exit 5
assert-exit 0
```

**Scripted interaction with a remote client:**

```sh
coffer -r 24 -c 80 -f test.script -- mudlark carrionfields.net 4449
```

```
# Wait for mudlark to connect and render
wait-for "Carrion Fields" 10

# Send a name and wait
send "guest\n"
wait-for "Welcome" 10

# Check we got past the banner
assert-not-contains "mourned"

# Wait for the client to exit and pin its status
send "/quit\n"
await-exit 10
assert-exit 0

render
```

**Drive an interactive shell and resize mid-run** (the child sees
`SIGWINCH` and the grid follows):

```
wait-for "$ " 10
send stty size\n
wait-for "24 80" 5
resize 120 40
send stty size\n
wait-for "40 120" 5
```

## Exit status

| Status | Meaning                                                                                                                                                       |
| ------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 0      | Success; all assertions passed (if any)                                                                                                                       |
| 1      | An assertion failed, a `wait-for` or `await-exit` timed out, the script could not be read or parsed, an option was invalid, or another runtime error occurred |

A script that never reaches its `wait-for` needle fails on the timeout
instead of hanging, so the budgets are the only timing a script states.

The child's exit status is **not** propagated; use `await-exit` with
`assert-exit` to check it. A child that cannot be executed exits with
status 127 inside the PTY.

## Environment

| Variable       | Meaning                                                          |
| -------------- | ---------------------------------------------------------------- |
| `TERM`         | The child sees `xterm-256color`                                  |
| `COLORTERM`    | The child sees `truecolor`                                       |
| `TERM_PROGRAM` | The child sees `ghostty`                                         |
| `SHELL`        | Program started when no command is given (POSIX; else `/bin/sh`) |

The first three can be overridden with `--env`.

`coffer --help` shows all options and script commands with descriptions.
`coffer --help-spec` emits the tab-separated listing for downstream
tooling.

## Man page

`src/coffer.1` documents the tool; it is installed as `coffer(1)` via
`make install`.

## How it works

1. Creates a PTY with the requested terminal size
   - POSIX: `forkpty`
   - Windows: `CreatePseudoConsole` (ConPTY)
2. Forks and execs the child process (status 127 if the exec fails)
3. Sets `TERM=xterm-256color`, `COLORTERM=truecolor`,
   `TERM_PROGRAM=ghostty` in the child environment, then applies the
   `--env` overrides
4. Drains PTY output into `cfr_input_write()`
   - POSIX: `poll` on the PTY FD
   - Windows: background reader thread does blocking `ReadFile` on the
     ConPTY output pipe and signals an event when data arrives
5. Wires `CfrCallbacks.output` back to the PTY so the child
   receives DA/DSR/kitty-keyboard responses
6. Waits `--wait` seconds, executes the script file (if any), then
   dumps the final grid and kills the child if it is still running

On POSIX, `resize` sets `TIOCSWINSZ` on the slave's tty (opened by name
for the ioctl and closed at once). An ioctl on the master alone is not
enough: it signals `SIGWINCH` and makes the resize look applied while
the child's own `stty size` still reports the old geometry. On Windows
it calls `ResizePseudoConsole`.

## Limitations

- No interactive input — all input is scripted via `-f` script file
- No mouse support
- `resize` on Windows needs `ResizePseudoConsole` (Windows 10 1809+);
  wine returns `E_NOTIMPL`, so the command fails loudly there (the
  HRESULT is printed)
- On Windows, an `--env` override is silently dropped if it has no `=`,
  its key is 128 characters or longer, or its value is 512 or longer;
  POSIX has no such limits
- A `wait-for` needle is matched against a row's text; a match spanning
  a soft wrap is two rows' text and will not be found (use the shorter
  needle)
