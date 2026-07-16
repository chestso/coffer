# cfr-debug — headless coffer PTY inspector

`cfr-debug` spawns a child process on a pseudo-terminal, pipes its
output through coffer, and renders the resulting grid as plain text.
It is useful for:

- Debugging VT rendering without a display server
- Capturing raw PTY output for offline replay
- Inspecting individual grid cells (character, codepoint, width)
- Validating that TUI applications render correctly through coffer
- CI assertions: verify that specific text appears on screen

The coffer output callback is wired back to the PTY, so terminal
queries (DA, DSR, kitty keyboard protocol) receive proper replies.
This allows interactive TUI applications to render correctly without a
real terminal emulator.

## Building

From a coffer build tree:

```sh
./autogen.sh && ./configure && make
```

On Windows (MSYS2 UCRT64), use `scripts/build-ucrt64.sh`. cfr-debug
uses ConPTY on Windows and `forkpty` on POSIX. On Windows, a
background reader thread does blocking `ReadFile` on the ConPTY
output pipe and signals an event when data arrives, because
anonymous pipes cannot be waited on with `WaitForMultipleObjects`.

The `cfr-debug` binary is installed alongside the library via
`make install`.

## Usage

```
cfr-debug [options] [-- command args...]
```

### Options

| Flag                   | Description                                   |
| ---------------------- | --------------------------------------------- |
| `-r`, `--rows ROWS`    | Terminal rows (default: 24)                   |
| `-c`, `--cols COLS`    | Terminal columns (default: 80)                |
| `-w`, `--wait SEC`     | Initial wait before script (default: 3)       |
| `-f`, `--script FILE`  | Script file (one command per line, see below) |
| `-o`, `--output FILE`  | Save raw PTY output to file                   |
| `-s`, `--show-row ROW` | Dump specific row (repeatable, default: all)  |
| `-d`, `--cell-data`    | Dump raw cell data alongside text             |
| `-q`, `--quiet`        | Only dump rows specified with `-s`            |
| `-h`, `--help`         | Show help                                     |
| `--help-spec`          | Emit machine-readable flag listing            |

### Script file

The `-f`/`--script FILE` option reads a script with one command per line:

| Command                    | Description                                                |
| -------------------------- | ---------------------------------------------------------- |
| `wait SECONDS`             | Drain PTY for N seconds (accepts decimals)                 |
| `send TEXT`                | Send text to PTY input (child's stdin). `\n`=CR, `\r`=CR, `\e`=ESC, `\t`=TAB |
| `raw HEX [HEX ...]`        | Send literal bytes to PTY input (e.g. `raw ff fc 01` for IAC WONT ECHO) |
| `assert-contains TEXT`     | Fail (exit 1) if rendered grid does not contain TEXT       |
| `assert-not-contains TEXT` | Fail (exit 1) if rendered grid contains TEXT               |
| `render`                   | Dump current grid to stdout                                |
| `cursor`                   | Dump cursor state (row, col, visible, blink)               |
| `# comment`                | Skipped                                                    |

If no `-f`/`--script` is given, cfr-debug waits `-w`/`--wait` seconds, then renders the
grid once and exits.

#### `send` vs `raw`

Both `send` and `raw` write to the **PTY input** (the child process's stdin),
not to coffer's terminal parser. This is the most common source of confusion:

- **`send`** writes text with escape expansion (`\n`→CR, `\e`→ESC, etc.).
  Use it to type shell commands — append `\n` to execute them.
- **`raw`** writes literal hex bytes with no expansion.
  Use it for binary input the child should receive verbatim.

Neither can directly emit escape sequences that coffer will parse, because the
bytes go to the child's stdin. The child must output them on its stdout for
coffer to process them. To send escape sequences the terminal will process,
have the child print them:

```
# Correct: printf outputs ESC bytes on stdout, coffer parses them
send printf '\033[?12l'\n
wait 1
send printf '\033[?12h'\n

# Wrong: raw writes ESC [ ? 1 2 l to stdin; the shell sees them as input,
# not as terminal control sequences
raw 1b 5b 3f 31 32 6c
```

Key points:

- Always append `\n` to `send` commands that should be executed by the shell —
  without it, the text sits on the shell's input line unexecuted.
- Use `\033` (not `\e`) inside `printf` arguments. `\e` is expanded by the
  script parser before the shell sees it, so the shell's readline intercepts
  the raw ESC byte. `\\033` passes the literal string `\033` to the shell,
  which `printf` then interprets.
- `raw` is useful for sending non-printable input to the child (e.g. `raw 03`
  for Ctrl-C, `raw 1a` for Ctrl-Z).

### Examples

**Interactive shell:**

```sh
cfr-debug -- bash -l
```

**Capture initial screen of crush (short flags):**

```sh
cfr-debug -c 68 -r 20 -w 5 -- crush
```

**Capture initial screen of crush (long flags):**

```sh
cfr-debug --cols 68 --rows 20 --wait 5 -- crush
```

**Capture raw output:**

```sh
cfr-debug -c 68 -r 20 -o /tmp/crush.raw -- crush
```

**Inspect a specific row with cell data:**

```sh
cfr-debug -c 68 -r 20 -s 18 -d -q -- crush
```

**Scripted interaction with assertions (CI):**

```sh
cfr-debug -r 24 -c 80 -f test.script -- mudlark carrionfields.net 4449
```

Where `test.script` contains:

```
# Wait for mudlark to connect and render
wait 3

# Verify the welcome banner appeared
assert-contains "Carrion Fields"

# Send a name and wait
send "guest\n"
wait 2

# Check we got past the banner
assert-not-contains "mourned"

render
```

Exit code 0 = all assertions passed, 1 = assertion failed.

`cfr-debug --help` shows all options with descriptions.
`cfr-debug --help-spec` emits a tab-separated flag listing for downstream tooling.

## Man page

`cfr-debug.1` is installed alongside the binary via `make install`.

## How it works

1. Creates a PTY with the requested terminal size
   - POSIX: `forkpty`
   - Windows: `CreatePseudoConsole` (ConPTY)
2. Forks and execs the child process
3. Sets `TERM=xterm-256color`, `COLORTERM=truecolor` in the child
   environment
4. Drains PTY output into `cfr_input_write()`
   - POSIX: `poll` on the PTY FD
   - Windows: background reader thread does blocking `ReadFile` on the
     ConPTY output pipe and signals an event when data arrives
5. Wires `CfrCallbacks.output` back to the PTY so the child
   receives DA/DSR/kitty-keyboard responses
6. Executes the script file (if any), then renders the coffer grid

## Limitations

- No interactive input — all input is scripted via `-f` script file
- No mouse support
