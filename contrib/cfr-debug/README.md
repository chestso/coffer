# cfr-debug — headless coffer PTY inspector

`cfr-debug` spawns a child process on a pseudo-terminal, pipes its
output through coffer, and renders the resulting grid as plain text.
It is useful for:

- Debugging VT rendering without a display server
- Capturing raw PTY output for offline replay
- Inspecting individual grid cells (character, codepoint, width)
- Validating that TUI applications render correctly through coffer

The coffer output callback is wired back to the PTY, so terminal
queries (DA, DSR, kitty keyboard protocol) receive proper replies.
This allows interactive TUI applications to render correctly without a
real terminal emulator.

## Building

From a coffer build tree:

```sh
./autogen.sh && ./configure && make
```

The `cfr-debug` binary is installed alongside the library.

## Usage

```
cfr-debug [options] [-- command args...]
```

### Options

| Flag | Description |
|------|-------------|
| `-r ROWS` | Terminal rows (default 24) |
| `-c COLS` | Terminal cols (default 80) |
| `-w SEC` | Wait before input (default 3) |
| `-i TEXT` | First input to send (`\n`=CR, `\r`=CR, `\e`=ESC) |
| `-W SEC` | Wait after first input (default 15) |
| `-I TEXT` | Second input to send |
| `-W2 SEC` | Wait after second input (default 15) |
| `-o FILE` | Save raw PTY output to file |
| `-s ROW` | Dump specific row (repeatable, default: all) |
| `-d` | Dump raw cell data alongside text |
| `-q` | Quiet: only dump specified rows |
| `-h` | Show help |

### Input escaping

The `-i` and `-I` flags accept escape sequences:

- `\n` or `\r` → carriage return (Enter key)
- `\e` → ESC (0x1B)

### Examples

**Interactive shell:**
```sh
cfr-debug -- bash -l
```

**Crush at 68×20 (initial screen only):**
```sh
cfr-debug -c 68 -r 20 -w 5 -- crush
```

**Send a prompt and capture raw output:**
```sh
cfr-debug -c 68 -r 20 -w 5 -i "hello world\n" -W 20 \
    -o /tmp/crush.raw -- crush
```

**Inspect just the status bar row with cell data:**
```sh
cfr-debug -c 68 -r 20 -w 5 -s 18 -d -q -- crush
```

**Two prompts (wait for each response):**
```sh
cfr-debug -c 80 -r 24 \
    -i "hello world\n" -W 20 \
    -I "what is 2+2?\n" -W2 20 \
    -- crush
```

**Replay a saved raw capture:**
```sh
# First capture, then replay with a small C program:
gcc -o replay replay.c -lcoffer
./replay /tmp/crush.raw 20 68
```

## How it works

1. Creates a PTY with the requested terminal size
2. Forks and execs the child process
3. Sets `TERM=xterm-256color`, `COLORTERM=truecolor`,
   `TERM_PROGRAM=ghostty` in the child environment
4. Drains PTY output into `cfr_input_write()`
5. Wires `CfrCallbacks.output` back to the PTY so the child
   receives DA/DSR/kitty-keyboard responses
6. After the drain timeout, renders the coffer grid as text

## Limitations

- POSIX only (uses `forkpty` and `poll`)
- No interactive input — all input is scripted via `-i`/`-I`
- No mouse support
- The child receives no real keypress events; applications that
  require interactive keyboard input (e.g. vim in insert mode)
  will not work well
