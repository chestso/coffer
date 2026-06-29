# coffer — follow-ups

Living checklist for coffer. Promote items to PRs as they get worked
on. Order is roughly priority, not strict dependency.

Scope: this file tracks coffer only. Items relating to portty's
wrapper of coffer (PNG harness, SDL3 / GTK4 platform layers, renderer
work) live in portty's own roadmap and are tagged here as
**[portty]** when referenced for context.

## Headless interactive testing infrastructure

- **A. `tests/test_cfr_pty.c`** — engine-only PTY harness. Spawns a child
  shell on a real PTY using `pty_create()` / `pty_read()` / `pty_write()` /
  `pty_destroy()` from `src/pty.c`, pipes raw output into `cfr_input_write()`,
  asserts on `cfr_get_cell` / `cfr_get_cursor` / `cfr_get_scrollback_lines` /
  `cfr_get_title`. **Status: done.** In tree and runs under `make check`.

- **[portty] B. `portty -P --exec CMD [--wait MS]`** —
  visual A/B harness in portty's `src/png_mode.c`.
- **[portty] C. `portty --headless --trace=PATH`** —
  full-stack harness with stubbed SDL3 and a synthetic key protocol on
  stdin.
- **[portty] D. `--exec` with input scripting** — extends B with
  `--input=BYTES` for full keystroke-to-PNG round-trip regression tests.

B / C / D live in portty's roadmap, not this one.

`scripts/pty_record.py` (in portty) is a simpler cousin to C —
Python, no SDL, no coffer — for diagnosing what a TUI emits _before_
any terminal renders it. Useful when a TUI's progressive enhancement
seems to silently fail: often the answer is that the TUI never tried.

## [portty] Soak test status (PNG mode A/B via `-P --exec`)

**Byte-identical to libvterm** (acceptance: pass without further work):

- echo, ls, uname, env probes, multi-line for loops
- SGR fg 30-37, tput setaf/setab, tput cup, ED/EL (`\033[2J\033[H`)
- bold, italic, curly underline (`4:3`), dashed underline (`4:5`)
- truecolor `38;2;R;G;B`
- box-drawing characters `┌─┬─┐`
- DEC special graphics line drawing (`\033(0lqk\nx x\nmqj\033(B`)
- CJK ideographs (你好世界) — width=2 cells with width=0 continuation
- VS16 ⚠️
- 100-line scrollback push
- 200-char line wrap
- CR overprint (`AAAA\rB`)
- 8-step tabs
- Scrolling region (DECSTBM `\033[2;5r`), IL/DL, ECH
- glow markdown render of `#` / `##` / fenced code
- vim --version, bat plain, tmux ls, htop -d 5 (1s timeout), btop -p 0
  (1s timeout), chafa --format=symbols, ls --color, ps aux, date, uname

**Accepted divergences** (bvt is correct; libvterm path is non-standard
and goes away in step 15):

- 256-color cube — bvt: xterm-standard `0/95/135/175/215/255`;
  libvterm: naive `0/51/102/153/204/255`.
- Plain `\033[4m` — bvt: single underline (per ECMA-48);
  libvterm path at `term_vt.c:307`: deliberately maps to dotted.
- 7-cp ZWJ family (👨‍👩‍👧‍👦) — bvt stores the full cluster;
  libvterm caps at 6 codepoints.
- Single RI flag (🇩🇰), skin-tone modifiers (👋🏽) — bvt honors UAX
  cluster width; libvterm reports per-codepoint widths.

**Non-deterministic** (PNG cmp not meaningful — use harness A
assertions instead):

- htop / btop / live monitors

## [portty] Manual interactive sweep (workstation with display only)

Run the binary directly with `PORTTY_VT=coffer`:

- `vim`, `nvim`, `emacs -nw`
- `htop`, `btop`, `lazygit`, `claude-code`
- `bat src/term.c`, `cat unicode-demo.txt`
- `chafa --format sixel image.png` (DCS sixel passthrough)
- mouse scroll + click + drag selection in tmux
- window resize reflow
- altscreen swap via vim

## Extraction history (done)

coffer was extracted from portty across steps 15–18:

- Step 15 (d5e62d8 + aae22d7): coffer became the default and only
  backend in portty. libvterm, `term_vt.c`, the
  `PORTTY_VT` env-var dispatch, the `ext_grid` SGR rewriting,
  and the libvterm cross-compile blocks are gone.
- Step 16 (7225bd7): VS16 shift hack removal — `TerminalRowIter` is a
  plain `vt_col += cell.width` walk.
- Step 17: renderer migration to `cfr_cell_get_grapheme()`. The
  6-codepoint-per-cell cap is gone; arbitrary-length clusters are
  retrieved via the public accessor.
- Step 18 (4d61c26): lift to `/home/thomasc/git/coffer` as a
  standalone autotools project with `coffer.pc` for pkg-config
  consumers.

Original detail lives in `git log` of portty.

## Planned

### OSC 8 hyperlink parsing (parse + store; no rendering) ✅ done

Spec: <https://gist.github.com/egmontkob/eb114294efbcd5adb1944c9f3cb5feda>
(ECMA-48 §8.3.89 for `ST`). Adoption tracker:
<https://github.com/Alhadis/OSC8-Adoption>.

Sequence: `OSC 8 ; params ; URI ST` (BEL terminator also accepted; spec
prefers `ST`). Empty URI closes the active link. `params` is `:`-separated
`key=value` pairs (only `id` is defined; we parse-and-discard for v1).
URI/params bytes are constrained to 32–126 per spec; URI size limit is
~2083 bytes (VTE / iTerm2 de-facto).

Implementation: `src/hyperlink.c` interns URIs per page (FNV-1a +
open-addressed dedup); cells reference URIs by `uint16_t hyperlink_id`.
The active id lives on `CfrCursorState` and is stamped into each cell at
print time. `cfr_scrollback_push` re-interns ids so URIs survive the
boundary into a scrollback page. Public accessor:
`cfr_cell_get_hyperlink(vt, cell, out, cap) -> length`. Coverage in
`tests/test_cfr_parser.c::test_osc8_*` (9 tests).

Rendering — clickable link styling, hover state, click dispatch — is
**[portty]**'s next slice.

## Investigate

- **Hoist `cfr_flush_cluster()` into `cfr_osc_dispatch`.** OSC 8 added a
  flush at the top of its handler (mirroring the csi.c / esc.c / modes.c
  pattern) so the previous link's still-pending cluster gets the old
  pen. Title-setting (OSC 0/1/2) doesn't currently mutate render state,
  so the missing flush there is benign — but the asymmetry is a trap:
  the next OSC code that touches the pen will quietly miscolour the
  pending cluster. Cheap fix: move the flush to the top of
  `cfr_osc_dispatch`. Wait for either a second consumer or a
  reproducible corpus bug before changing it.

## Out of scope for v1 (defer indefinitely)

- **Kitty graphics protocol.**
- **Synchronized output** (mode 2026) — easy to add once parser is solid.
- **Image-cell background protocol** — sixel scrolling is **[portty]**'s
  domain (handled by its sixel layer).
- **Right-to-left text shaping** — handled at HarfBuzz in **[portty]**,
  not at the VT layer.
- **OSC 8 link rendering / click dispatch / hover styling** —
  **[portty]**. coffer parses and stores OSC 8 (see "Planned"
  above); rendering it as clickable underlined text is portty's
  job.
- **OSC 8 `id=` continuity parameter** — parsed-and-discarded for v1.
  Adjacent same-URI runs already share an interned id by construction
  (URI dedup), which covers the spec's primary use case. Add this only
  when a renderer surfaces the gap in the secondary case (same logical
  link, differing URI text).
- **OSC 8 inside DCS / SOS-PM-APC strings** — not a real corpus case;
  standard OSC parsing only.

## Kitty keyboard protocol — deferred flags

Implemented: **flag 0x1** (Disambiguate escape codes) and **flag 0x8**
(Report all keys as escape codes). Push/pop/set/query of the flag
stack is wired in `csi.c` and the four special-key paths plus the
Ctrl/Alt+ASCII text path in `keys.c` honour the active flags.
Coverage in `tests/test_cfr_keys.c`.

The remaining three flags are accepted-and-stored on the stack but
have no behavioural effect:

- **0x2 — Report event types** (press / repeat / release). Requires
  the platform layer to emit key-up events. SDL3 delivers
  `SDL_EVENT_KEY_UP` and a `repeat` flag on `SDL_EVENT_KEY_DOWN`, so
  this is plumbing work in `platform_sdl3.c` + `platform_gtk4.c` + the
  `terminal_send_key` API (add an event-type argument) more than VT
  work. Encoding: `CSI <code>;<mods>:<event>u` where event is 1=press,
  2=repeat, 3=release.
- **0x4 — Report alternate keys**. Sends both the keysym and the
  shifted/base alternate (`CSI <code>:<alt>:<base>;<mods>u`). Requires
  the platform to surface the unmodified-layout keysym alongside the
  shifted one — SDL3 exposes both via `SDL_Keycode` + `SDL_Scancode` →
  `SDL_GetKeyFromScancode(.., 0)`, so feasible.
- **0x10 — Report associated text**. Appends UTF-8 text after the
  keysym: `CSI <code>;<mods>;<text-codepoints>u`. Needs the platform
  to pair the key event with its text-input result; today these
  arrive through separate SDL events.

None of these are needed by Claude Code or the common TUIs. Defer
until a concrete consumer asks for them.

## Resolved during soak

- ~~`printf 'A\033[s\nB\033[uC'` (DECSC across LF) reported an 18-byte
  PNG diff against libvterm~~ — bvt was already correct per spec
  (DECSC saves cursor pos; LF preserves column; DECRC restores
  exactly). Coverage in `tests/test_cfr_parser.c::test_decsc_across_lf`.
- ~~`printf 'A\tB\tC\033[3g\rX\tY'` (HT after CSI 3 g) diverged from
  xterm~~ — TBC (CSI g) was unimplemented; `\033[3g` was silently
  dropped so default 8-column tab stops survived. Implemented in
  `csi.c` (mode 0 clears stop at cursor; mode 3 clears all). HT then
  advances to the last column when no stops remain, matching xterm /
  foot / alacritty. Coverage in `tests/test_cfr_parser.c::test_tbc_*`.
- ~~DEC special graphics (line drawing) was unimplemented — `\033(0`
  designation was silently swallowed and `lqk\nx x\nmqj` rendered as
  literal ASCII instead of `┌─┐ │ │ └─┘`~~ — added charset slot
  tracking on CfrTerm, ESC ( ) \* + dispatch, SO / SI shifts, and the
  standard VT100 0x5F..0x7E translation table at print time. Coverage
  in `test_cfr_parser.c::test_dec_graphics_g0` and `::test_dec_graphics_si_so`.
- ~~Wrap-aware selection broke at the visible/scrollback boundary, and
  resize never reflowed because reflow was off by default~~ — backend
  adapter now translates wrapline semantics across the boundary and
  flips reflow on by default for bvt. Tests in `test_term_bvt.c`. See
  cae5f69.
- ~~Ctrl+letter key combos didn't work in coffer — Ctrl+C produced raw
  `c` instead of 0x03~~ — `cfr_send_text` now applies the standard
  Ctrl-byte transformation (Ctrl+@ → 0x00, Ctrl+A..Z → 0x01..0x1A, etc.)
  before forwarding. Fixed in 2de4465.
- ~~cf wiped the screen on launch — the brick (Haskell vty) inline TUI
  drew at row 0 instead of preserving prompts above~~ — bvt was missing
  DECOM (origin mode 6) entirely, and DECSTBM accepted the degenerate
  `CSI 1;1 r` brick emits during setup. Both fixed; see 04f4854.
  Repro lives in `tests/test_cfr_pty.c::test_cf_brick_inline_preserves_history`
  and unit coverage in `tests/test_cfr_parser.c::test_decom_cup` /
  `::test_decstbm_invalid_rejected` / `::test_cf_byte_replay`.

## Resolved during scaffolding (kept here for context)

- ~~`get_cell` / `get_dimensions` / `get_scrollback_cell` returned `0/1`
  instead of `-1/0`~~ — fixed in `src/term_bvt.c`; renderer was treating
  every cell as missing and PNGs came out 1 cell wide.
- ~~PNG mode hardcoded the libvterm backend~~ — `src/png_mode.c` now
  honors `PORTTY_VT` so A/B comparison works.
- ~~Reverse video (`\033[7m … \033[27m`) PNG diverged from libvterm~~ —
  `term_bvt.c::convert_cell` now pre-swaps fg/bg and clears
  `bg.is_default`, matching the libvterm backend. Byte-identical PNG.
- ~~`test_cfr_parser` link error against `cfr_palette_lookup`~~ — added
  `palette.c` to `tests/Makefile.am`.
- ~~Reflow shrink put real content into scrollback because trailing
  empty rows produced empty logical lines~~ — `reflow.c` now tracks
  `cursor_line` and trims trailing empty logical lines that don't host
  the cursor.
- ~~`test_utf8_replacement` failed: bare 0x80 treated as C1 control~~ —
  removed C1 anywhere transition in the parser (xterm UTF-8 mode).
- ~~`test_style_intern_dedup` failed: pen color_flags was 0 initially,
  then set to defaults after first SGR reset~~ — initialize pen with
  `CFR_COLOR_DEFAULT_FG | _BG | _UL` in `cfr_new`.
- ~~256-color SGR (`\033[48;5;220m`) PNG diverged from libvterm~~ —
  _accepted divergence_. libvterm uses a naive 51-step ramp
  `(0, 51, 102, 153, 204, 255)` and produces `#FFCC00` for color 220.
  bvt uses the xterm-standard ramp `(0, 95, 135, 175, 215, 255)` and
  produces `#FFD700`, matching xterm / iTerm / foot / Alacritty. Document
  in CLAUDE.md when default flips.
- ~~Plain `\033[4m` underline diverged from libvterm~~ — _accepted
  divergence_. portty's libvterm wrapper at `src/term_vt.c:307`
  deliberately rewrites plain SGR 4 to dotted (`ext_ul_style = 4`).
  Standard ECMA-48 SGR 4 = single underline. bvt follows the standard
  and renders `\033[4m` as single underline, matching vim, less, man,
  etc. The divergence vanishes once libvterm is removed (step 15).
