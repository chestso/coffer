# bloom-vt

A standalone virtual terminal engine in C — parser, grid, scrollback, reflow,
charsets, kitty keyboard protocol, sixel and Lottie graphics — with no
external dependencies. Extracted
from [bloom-terminal](https://github.com/thomas-christensen/bloom-terminal),
where it replaces libvterm.

## What it does

- **VT100 / VT220 / xterm parser** — CSI / OSC / DCS / ESC dispatch, DEC
  modes, scrolling regions, charsets (incl. DEC special graphics line
  drawing), origin mode, tab-stop manipulation (TBC).
- **UAX-aware grid** — Unicode #11 East Asian Width and #29 grapheme
  cluster widths computed at insertion time and stored on each cell.
  ZWJ sequences, regional indicators, skin-tone modifiers, and VS16
  emoji presentation all carry the right cell width without
  per-renderer peek-ahead.
- **Grapheme arena** — full clusters are interned; cells reference them
  by id, so there is no hardcoded codepoint cap (libvterm caps at 6).
- **Scrollback** — paged ring buffer; default 1000 lines, configurable.
- **Reflow** — recomputes wrap on resize; preserves cursor row.
- **Sixel graphics** — DCS sixel images (`DCS q … ST`) are decoded and
  stored in the engine, anchored to an absolute grid line so they scroll,
  enter scrollback, and clear with the text they sit on. Decode covers
  RLE, RGB and DEC HLS color, P2 transparency, and raster attributes;
  capability is advertised via DA1 (`4`), DECSET 80/1070/8452, and
  XTSMGRAPHICS. The host declares cell pixel size with
  `bvt_set_cell_pixels()` and fetches images to draw with
  `bvt_get_sixels()`.
- **Lottie graphics** — APC sequences (`ESC _ … ST`) with base64-encoded JSON
  payloads load, place, and control Lottie animations on the grid. Eight
  commands (load, load-chunk, place, play, pause, stop, seek, delete) manage
  animation state, per-frame RGBA buffers, and placement tracking. Animations
  scroll with the text, enter scrollback, and are culled on clear — the same
  ownership model as sixel. The host fetches animations via
  `bvt_get_lotties()` / `bvt_get_lottie_placements()` and advances frames
  with `bvt_lottie_tick()`. Rasterization is handled by ThorVG (optional
  dependency, auto-detected at configure time); when ThorVG is absent the APC
  sequences are still accepted but RGBA buffers are zeroed. A Python TUI
  player ([plotty](https://codeberg.org/thomasc/bloom-terminal/src/branch/master/bloom-terminal/contrib/plotty))
  provides interactive playback with keyboard controls for pause, seek,
  speed, opacity, and layer toggling.
- **Windows ConPTY note:** Windows ConPTY intercepts and re-serialises VT output
  through conhost's VtEngine, which recognises CSI, OSC, and DCS but _not_ APC
  (`ESC _`). APC sequences are silently dropped — the same limitation that
  prevents the kitty image protocol from working on Windows (Windows Terminal
  issue #8389, open since 2020). `PSEUDOCONSOLE_PASSTHROUGH_MODE` (flag 0x8,
  Windows 11 22H2+) is intended to relay the raw VT stream unmodified, but on
  some builds the flag is accepted by `CreatePseudoConsole` yet unknown
  sequences are still stripped. As a workaround, the Lottie client on Windows
  carries the same base64-encoded JSON payload inside **OSC 5555** (`ESC ] 5555 ; \<base64\> BEL`), which ConPTY does pass through because OSC is a recognised VT
  family. bloom-vt routes OSC code 5555 to `bvt_lottie_apc_dispatch()`, so the
  payload is processed identically regardless of carrier. This mirrors how
  iTerm2's image protocol works on Windows (OSC 1337) — encode image data in
  OSC instead of APC.
- **Damage tracking** — the changed region is accumulated as input is
  parsed; the consumer calls `bvt_damage_flush()` at a controlled time
  (typically once per frame, before rendering) to receive it via the
  `damage` callback and repaint only what changed. A cursor-only move
  (which dirties no grid cell) is folded in — flush damages the old and
  new cursor cells.
- **Kitty keyboard protocol** — flags 0x1 (Disambiguate) and 0x8 (Report
  all keys as escape codes) are fully implemented. Push/pop/set/query
  of the flag stack works. Flags 0x2/0x4/0x10 are accepted on the stack
  but currently no-ops (waiting on a concrete consumer).
- **Hyperlinks (OSC 8)** — cells carry a hyperlink id; the host retrieves
  the URI via `bvt_cell_get_hyperlink()`.
- **Zero external dependencies** — libc only (ThorVG is optional and
  auto-detected).

For the project status — what is byte-identical to libvterm, accepted
divergences, and the deferred items — see [`FOLLOWUPS.md`](FOLLOWUPS.md).

## Memory model

Bounded allocation and a zero-allocation steady-state hot path are explicit
design goals. Concretely:

- **Pluggable allocator.** All heap traffic goes through a `BvtAllocator`
  (malloc/realloc/free function pointers) supplied at `bvt_new()` time.
  Callers can route everything to an arena, a pool, or a tracked allocator
  without patching the library. Pass `NULL` to use libc.
- **Paged grids.** Each `BvtPage` owns a contiguous cell buffer plus a
  per-row flag byte. A terminal has at most two pages live at once (the
  primary grid and, while in altscreen, the alternate). Page allocation
  happens on `bvt_new`, on resize, and on altscreen entry — never from
  `bvt_input_write`.
- **Paged scrollback.** A doubly linked ring of pages, 64 rows per page,
  default 1000 lines total. Allocations happen only when a page fills; the
  ring evicts the oldest page when capacity is reached, so scrollback
  memory is bounded by the configured line count regardless of input.
- **Per-page interning.** A style intern table keyed by SGR pen and a
  grapheme arena keyed by codepoint sequence both live on the page.
  Cells store a 32-bit `style_id` and 32-bit `grapheme_id`, so repeated
  styles and clusters cost one slot regardless of how many cells use them.
  Single-codepoint clusters use `cp` directly with `grapheme_id == 0` and
  hit the arena zero times. Unlike libvterm's hard-coded 6-codepoint cell
  cap, clusters here are arbitrary length.
- **Hot path.** `bvt_input_write` does no allocation in steady state —
  all hot-path structures (parser scratch, OSC accumulator up to 4 KiB,
  cursor cluster buffer up to 16 codepoints) are inline in `BvtTerm`.
  The intern tables grow geometrically and amortize to zero; the grapheme
  arena reuses existing entries on hash hit.
- **Sixel store.** Decoded images live in a dense record array
  (swap-remove deletion, no fragmentation) with their pixel buffers drawn
  from a small best-fit free-list pool, so animation that streams
  same-size frames recycles one buffer instead of churning the heap. A
  global live-byte budget evicts the oldest image first (the same order
  they scroll off), and a per-dimension clamp bounds any single image.
  The store is allocated lazily on the first sixel and freed by
  `bvt_free`.
- **Lottie store.** Mirrors the sixel architecture: dense record array with
  swap-remove deletion, a spare buffer pool that recycles same-size RGBA
  frames, and a global live-byte budget (128 MiB live, 64 MiB retained) that
  evicts the oldest animation first. ThorVG rasterizes each frame to
  ARGB8888, which is then un-premultiplied and converted to linear-light
  RGBA32 for the host compositor. The store is allocated lazily on the first
  Lottie APC and freed by `bvt_free`.
- **Lifecycle.** `bvt_new` / `bvt_free` is the only ownership pair the
  consumer manages. `bvt_free` releases every page, intern table, arena,
  tab-stop bitmap, sixel store, and lottie store through the same allocator
  that produced them.

`BvtTerm` itself is not internally synchronized; callers own all locking.

## Build and install

Standard GNU autotools workflow:

```sh
./autogen.sh                                # writes ./version, runs autoreconf -fi
./configure --prefix=$HOME/.local           # ThorVG auto-detected; --disable-thorvg to skip
make
make check
make install
```

On Windows, use MSYS2 UCRT64 with MinGW-w64 GCC.

### Prerequisites

Install [MSYS2](https://www.msys2.org/), then in a UCRT64 shell (`msys2_shell.cmd -ucrt64`):

```sh
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-autotools

# Optional: ThorVG enables Lottie rasterization (auto-detected by configure)
pacman -S mingw-w64-ucrt-x86_64-thorvg
```

### Building

The recommended way is the helper script, which can be launched from any shell
(git-bash, cmd, PowerShell, or an MSYS2 shell) and re-execs into a real MSYS2
UCRT64 shell so `/ucrt64` is mounted and `$MINGW_PREFIX` resolves correctly.
It assumes the [prerequisites](#prerequisites) above are already installed —
the script never runs `pacman` itself:

```sh
./scripts/build-ucrt64.sh            # autogen + configure + make + check
./scripts/build-ucrt64.sh --install  # build, then make install
```

Extra args are forwarded to `configure`:

```sh
./scripts/build-ucrt64.sh --enable-release
./scripts/build-ucrt64.sh --disable-thorvg    # skip optional Lottie rasterizer
```

To build manually inside an MSYS2 UCRT64 shell:

```sh
./autogen.sh
mkdir build && cd build
../configure --prefix="$MINGW_PREFIX"       # ThorVG auto-detected; --disable-thorvg to skip
make -j$(nproc)
make check                                  # POSIX-only PTY tests are skipped automatically
make install
```

> Note: building from outside an MSYS2 UCRT64 shell (e.g. plain git-bash) with
> `--prefix="$MINGW_PREFIX"` fails because `/ucrt64` is not mounted and
> `$MINGW_PREFIX` is empty, which makes autotools reject the backslash-bearing
> Windows path as an "unsafe srcdir". Use `./scripts/build-ucrt64.sh`, which
> re-execs into the correct shell first.

### `autoreconf` fails on scoop-installed MSYS2 (sh workaround)

On some MSYS2 installs (notably scoop-managed), `/usr/bin/sh` and
`/usr/bin/bash` are the same binary. When bash is invoked as `sh` — as
`libtoolize` does via `#!/usr/bin/env sh` — its POSIX-mode `test -d` builtin
intermittently fails to see `/ucrt64` paths (a mount-table visibility race),
aborting `autoreconf` with:

```
libtoolize: error: $pkgauxdir is not a directory: '/ucrt64/share/libtool/build-aux'
```

`build-ucrt64.sh` applies the workaround automatically (shadowing `sh` with a
bash copy earlier on `PATH`). It also sanitizes `ACLOCAL_PATH` — which arrives
as a Windows-style value (backslashes, `;` separators) when the script is
re-execed from a Windows parent — converting it to POSIX paths so `aclocal`
can stat them. Both fixes happen before `autogen.sh` runs.

To do the `sh` workaround manually before running `autogen.sh`:

```sh
FIXSH=$(mktemp -d /tmp/bloom-fixsh.XXXXXX)
cp /usr/bin/bash "$FIXSH/sh"
export PATH="$FIXSH:$PATH"
./autogen.sh          # now succeeds
rm -rf "$FIXSH"
```

Build mode flags for `./configure` (all platforms):

| Flag               | Effect                                                                |
| ------------------ | --------------------------------------------------------------------- |
| _(default)_        | ASan + UBSan on POSIX; unsanitized debug (`-O1 -g3`) on Windows/MinGW |
| `--enable-debug`   | Unsanitized debug (`-O0 -g3`)                                         |
| `--enable-release` | Optimized (`-O3 -DNDEBUG`)                                            |

Optional targets:

```sh
make format       # clang-format on src/, include/, tests/; prettier on *.md
make bear         # produce compile_commands.json for clangd
make distcheck    # build, test, and verify the dist tarball is self-contained
```

## Linking

```sh
cc app.c $(pkg-config --cflags --libs bloom-vt)
```

`bloom-vt.pc` installs to `${libdir}/pkgconfig/`.

## Minimal usage

```c
#include <bloom-vt/bloom_vt.h>
#include <stdio.h>

int main(void)
{
    BvtConfig cfg = BVT_CONFIG_DEFAULTS;
    cfg.rows = 24;
    cfg.cols = 80;
    cfg.cell_w_px = 8;
    cfg.cell_h_px = 16;
    BvtTerm *vt = bvt_new(&cfg);

    const char *bytes = "hello\033[31m world\033[0m\n";
    bvt_input_write(vt, (const uint8_t *)bytes, strlen(bytes));

    int rows, cols;
    bvt_get_dimensions(vt, &rows, &cols);
    for (int c = 0; c < cols; c++) {
        const BvtCell *cell = bvt_get_cell(vt, 0, c);
        if (!cell || cell->width == 0) continue;
        putchar(cell->cp ? (int)cell->cp : ' ');
    }
    putchar('\n');

    bvt_free(vt);
    return 0;
}
```

The full API is in [`include/bloom-vt/bloom_vt.h`](include/bloom-vt/bloom_vt.h).

## Acknowledgments

The default 16-color ANSI palette is [CharmTone](https://github.com/charmbracelet/x/tree/main/exp/charmtone), the palette behind [charm.land](https://charm.land) — with thanks to the folks at [Charmbracelet](https://charm.sh) for making the terminal a more colorful place. 🌸

## License

MIT. See [`COPYING`](COPYING).
