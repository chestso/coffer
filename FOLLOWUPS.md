# coffer — follow-ups

Living checklist for coffer. Promote items to PRs as they get worked
on. Order is roughly priority, not strict dependency.

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

  **Benefit:** Eliminates a latent bug where future OSC sequences that
  change styling would silently miscolour pending text. Defensive
  hardening with minimal cost.

## Not yet implemented

### Synchronized output (mode 2026)

Mode flag is stored (`CFR_MODE_SYNC_OUTPUT`) and callbacks are notified,
but there's no behavioral effect. Per the spec, when mode 2026 is on,
output should be buffered until a subsequent mode-off event, then flushed
atomically. This prevents screen flicker during multi-step updates.

**Benefit:** Eliminates visual tearing in TUIs that emit partial frames
(e.g., lazygit, btop). The application sends `\e[?2026h`, emits all cells,
then `\e[?2026l` — the terminal renders everything at once instead of
showing intermediate states.

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

### OSC 8 `id=` continuity parameter

Currently parsed-and-discarded (line 48-50 in osc.c finds the params/URI
separator but never extracts key=value pairs from params). Adjacent
same-URI runs already share an interned id by construction (URI dedup),
which covers the common case. Add support for explicit `id=` extraction
and comparison to handle the spec's secondary case: same logical link
with differing displayed text or target URIs should remain continuous
when they share an explicit id.

**Benefit:** Correctly handles links where the visible text changes mid-run
(e.g., progressive loading showing "loading..." then the actual title) but
should remain a single clickable region. Required for full OSC 8 spec
compliance. Rare in practice — most applications just reuse the same URI.

### OSC 8 inside DCS / SOS-PM-APC strings

Not implemented. Standard OSC parsing only; no support for OSC sequences
embedded within other control string contexts.

**Benefit:** Handles exotic escape sequences from non-standard or
experimental terminals. Not a real corpus case — no known applications
emit this pattern. Purely defensive.

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
  handler.
- **Unicode placeholders (`q=1`)** — no placeholder-cell rendering or
  cursor-movement-via-placeholder-text.
- **`I=` image number** — parsed but otherwise ignored; only unique
  ids (`i=`) are supported.

**Benefit:** Full spec compliance for kitty-native clients (timg,
chafa, and tools driving kitty animations). Most consumers only need
the transmit/place/delete paths, which already work.
