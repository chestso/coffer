# coffer — follow-ups

Living checklist for coffer. Promote items to PRs as they get worked
on. Order is roughly priority, not strict dependency.

## Investigate

- **Script engine leftovers** (see `docs/script-engine-design.md`):
  portty's `-S` process exit code does not reflect a failed script run
  (the engine reports CFR_SCRIPT_FAILED and the app quits, but exit 0);
  and in portty, `await-exit` races the app's own PTY-close quit path
  (the child exiting quits the app before the script observes it).
  Neither affects the `coffer` CLI, where both work end to end.

- **Wrapped-line behavioral spec + differential fuzz.** The spec is
  `docs/wrapped-lines-spec.md` (policy knobs, edit/severance table, text
  extraction) and the oracle is `tests/test_cfr_wrap_fuzz.c` (random op
  stream vs. a naive row model, checked after every op). The model now
  covers resize/reflow (per-row widths, the scrollback/grid boundary,
  `CFR_REFLOW_PREFIX_ROWS`), the clamp-only resize path (`reflow =
  false`, resize on the alt screen, and the exit-time catch-up of the
  stale main page), DECOM, partial scroll regions (DECSTBM), altscreen
  1049 and raw 47/1047, selector/ZWJ clusters printed as one write, and
  split-write base+joiner pairs (§3.4). The model extension found and
  pinned three engine fixes: the ASCII run fast path wrote one cell past
  the row after a failed wrap commit and ED 0/1 indexed rows out of
  bounds after a DECRC restored a pre-shrink cursor (both in
  `test_cfr_wrap_erase`), and a width-changing clamp carried the margin
  WRAPLINE bit onto an interior cell, where a later resize could
  resurrect a severed join (`test_cfr_lines`). The split-write ops also
  pin the parked-margin attach fix: a stray joiner now re-attaches to
  the recorded last printed cell instead of the cell before the cursor
  (`test_cfr_wrap_width`). The accepted
  DL-inside-one-wrapped-line over-join (spec §7) remains: wrong text,
  never wrong structure.

## Not yet implemented

### Kitty keyboard protocol — remaining flags

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

  **Benefit:** Enables applications to distinguish between initial key
  press, auto-repeat, and key release. Critical for games, text editors
  with held-key behavior, and accessibility tools.

- **0x4 — Report alternate keys**. Sends both the keysym and the
  shifted/base alternate (`CSI <code>:<alt>:<base>;<mods>u`). Requires
  the platform to surface the unmodified-layout keysym alongside the
  shifted one — SDL3 exposes both via `SDL_Keycode` + `SDL_Scancode` →
  `SDL_GetKeyFromScancode(.., 0)`, so feasible.

  **Benefit:** Allows applications to know what key was physically
  pressed regardless of modifier state or keyboard layout. Essential for
  keybinding systems that want "physical Q" rather than "logical A on
  AZERTY".

- **0x10 — Report associated text**. Appends UTF-8 text after the
  keysym: `CSI <code>;<mods>;<text-codepoints>u`. Needs the platform
  to pair the key event with its text-input result; today these
  arrive through separate SDL events.

  **Benefit:** Enables IME-aware applications to receive both the
  physical key and the composed text in a single event. Critical for
  CJK input methods, dead keys, and complex scripts.

None of these are needed by Claude Code or the common TUIs. Implement
when a concrete consumer asks for them or when building a full-featured
terminal emulator frontend.

### OSC 5 / OSC 105 special colors

The 256 indexed entries (`OSC 4`, `OSC 104`) are implemented. xterm's
special colors — `OSC 5` / `OSC 105` for bold, blink, reverse, underline,
italic and the like — are not; they fall through to the generic OSC
callback. No known consumer of the engine uses them.

### X11 color names in color specs

OSC 4 and OSC 10/11/12 accept the XParseColor RGB forms (`rgb:R/G/B`,
`#RGB`, `#RRGGBB`, `#RRRRGGGGBBBB`). X11 color _names_ (`red`, `LightSkyBlue`,
…) are not looked up: they are ignored, and logged once as an unparseable
specification. Applications that care can send the `rgb:` form.

### OSC 8 inside DCS / SOS-PM-APC strings

Not implemented. Standard OSC parsing only; no support for OSC sequences
embedded within other control string contexts.

**Benefit:** Handles exotic escape sequences from non-standard or
experimental terminals. Not a real corpus case — no known applications
emit this pattern. Purely defensive.

### Bundled ConPTY

The CLI loads the pinned `Microsoft.Windows.Console.ConPTY`
`conpty.dll` + `OpenConsole.exe` bundle when available; system conhost is
the fallback. The PTY layer is shared with the tests on both platforms:
`src/pty_posix.c` (`forkpty`) and `src/pty_w32.c` + `src/conpty.c`, archived
into `libcofferpty.a`.

- `test_cfr_conpty` covers the loader branches a live bundle cannot reach:
  a directory with `conpty.dll` but no `OpenConsole.exe`, an unloadable
  (wrong-architecture) DLL, the `<arch>/OpenConsole.exe` fallback, and the
  `--conpty-dir` > `COFFER_CONPTY_DIR` > compiled-path search order.
- `PSEUDOCONSOLE_GLYPH_WIDTH_GRAPHEMES` and
  `PSEUDOCONSOLE_AMBIGUOUS_IS_WIDE` were evaluated with
  `test_conpty_width_alignment`: the bundled OpenConsole re-serialises text
  and lets the receiving engine measure it, so these host flags do not
  change coffer's grid, and no `--conpty-glyph-width` / `--ambiguous-wide`
  switch is warranted. The test fails if a future OpenConsole starts baking
  widths into the stream, which is the signal to revisit that decision.
- OSC 4 and OSC 104 pass through ConPTY untouched: verified on the Windows
  CI runners (x64 and arm64) for both the bundle and system conhost, in
  stream order, with indexed SGR kept as an index rather than baked to RGB.
  `test_conpty_palette` runs a child through the selected backend, replays
  the captured stream, and checks the engine's palette and cell colours; the
  bundle's behaviour is asserted, the system-conhost observation is only
  logged. Palette-override parity behind ConPTY matches the POSIX path.
- The OSC 5555 (input) and 5556 (response) carriers stay. 5555 is emitted by
  the child, which cannot know which backend the CLI selected; 5556 must
  remain usable because `cfr_conpty_create()` can fall back to system
  conhost at runtime.
- `scripts/verify_conpty_bundle.py` checks every fetched architecture's PE
  machine type, `SHA256SUMS` and lock version; `make dist` runs it for all
  architectures, and CI covers Windows arm64 natively in addition to x64.
- Portty now bundles 1.25.261109001, prefers the `Conpty*` exports and uses
  flags 0 (the old `0x8` passthrough constant is
  `PSEUDOCONSOLE_GLYPH_WIDTH_GRAPHEMES` in 1.25). Remaining portty work:
  adopt coffer's `conpty.lock` + fetch/update scripts instead of tracking
  the binaries directly, and expose the package version in diagnostics.

### Kitty graphics protocol — remaining gaps

The core protocol is implemented (`src/graphics.c`, tests in
`test_cfr_graphics.c`): transmit `a=t`/`a=T`, place `a=p` (absolute,
`U=1` virtual, `P=`/`Q=` relative-to-parent), delete `a=d` with
`d=a`/`d=i`/`d=p`, capability query `a=q`, chunked upload (`m=`), zlib
compression (`o=z`), formats `f=24`/`32`/`100`, cursor advance
(`c=`/`r=`), z-index, and the ConPTY OSC 5556 carrier workaround.

Not yet implemented:

- **Animation control (`a=a`)** — only acknowledged with OK; no frame
  index, no loop/gap timing, no playback. `a=f` replaces the image's
  pixels in place (single-frame), there is no frame store.
- **Compose (`a=c`)** — no-op acknowledgment; we always render the
  latest frame in every visible placement.
- **Delete subtypes** — `d=c` (cursor cell/region), `d=n` (image
  number `I=`), `d=x`/`d=y` (intersecting ranges). The `x=`/`y=` keys
  are parsed into `delete_x`/`delete_y` but unused by the delete
  handler. `d=a` is scoped to kitty's own id space: it deletes the
  kitty images, not the sixels or the Lottie animations. That is the
  correct reading of the protocol (each source owns its id space),
  but a client that mixed protocols and expected one "delete all" to
  clear everything will be surprised.
- **Unicode placeholders (`q=1`)** — no placeholder-cell rendering or
  cursor-movement-via-placeholder-text.
- **`I=` image number** — parsed but otherwise ignored; only unique
  ids (`i=`) are supported.

**Benefit:** Full spec compliance for kitty-native clients (timg,
chafa, and tools driving kitty animations). Most consumers only need
the transmit/place/delete paths, which already work.
