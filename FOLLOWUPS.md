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

- **Wrapped-line behavioral spec + differential fuzz.** The internal
  representation of soft wraps is now margin-cell `CFR_CELL_WRAPLINE`
  plus per-row lineage ids, with a two-sided predicate
  (`cfr_row_continues`, `src/lines.c`; see
  `docs/wrapped-lines-design.md`). Open follow-ups from that work:
  a written behavioral spec (LF column rule, phantom lifetime, the §4
  policy knobs — ICH/IRM severance, wide-char at margin, DECAWM toggled
  mid-phantom, DECSC/phantom), and a differential fuzzer that runs a
  random op stream against a naive line model, asserting joins and text
  extraction. The representation should be frozen while the spec is
  written; a wrong policy choice can over- or under-join by one row,
  never corrupt structure.

  Two more policy points are now pinned by
  `tests/test_cfr_wrap_width.c`: (1) a width-1 print over the lead cell
  of a wide cluster at the margin blanks the stranded continuation cell
  and severs the join — the §2.2 "print overwriting margin" rule
  extended to the print's side effect; (2) a presentation selector or
  combining mark whose base was committed by an earlier flush (a write
  boundary split them) re-attaches to that cell in the same row and
  re-commits the combined cluster — cross-row re-attach stays a drop.

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

- **Cell re-interning degrades silently on failure.** `cfr_cell_reintern()`
  maps an allocation failure, or a full hyperlink table (65535 ids), to
  default style / no cluster / no link, because the intern functions
  return 0 for both "failed" and "default". Resize and scrollback moves
  therefore lose an attribute rather than abort. Fixing it means giving
  the intern functions a distinct failure return.

- **Scrollback capacity is in rows, not bytes.** 100000 rows is about
  130 MB at 80 columns and four times that at 300; an unbounded ring fed a
  480 MB file is gigabytes. Eviction is one function (`evict_to_capacity`
  in `scrollback.c`), so a byte budget — a `CfrConfig` field checked there
  alongside the row count — slots in without touching the ring, the
  readers or the callers.

- **`CfrCallbacks.moverect` is never emitted.** The callback is in the
  public header, but no engine path fires it (`grep moverect src/` is
  empty), so a host that wires it for a scroll-blit gets silence. Either
  emit it on the whole-row moves (scroll, IL/DL) or drop it from the
  API.

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

### The image store — one image set, not one per screen

The unified-store work itself is done (all eight phases; see
`docs/image-store-design.md`). What follows here is what it
deliberately left out.

The store holds ONE set of images for the whole terminal
(`src/image_store.c`), not one per screen buffer. Kitty's rule is that
switching to the alternate screen clears the images _of the alternate
screen_ — so the images drawn in the main screen should survive the
switch, and the ones drawn in the alternate screen should go when it
is left. Coffer cannot tell them apart: the altscreen switch clears
the ANCHORED (1:1) images, whose anchors are the grid's own rows, and
keeps the client-owned 1:N images (kitty), which carry their own
anchors and live until the client deletes them.

**Impact:** a kitty image drawn in the alternate screen stays visible
after the switch back to the main screen. The client can delete it
(`a=d,d=i`).

**Fix:** a per-screen image set (or a per-image screen tag), and the
altscreen switch clearing the set of the screen being left.

### The image store — the byte-budget eviction

The budget eviction takes the OLDEST record (by creation order), which
is deterministic — but it does not yet prefer an image that is not on
the screen over one that is. A kitty image the client placed and still
wants can be evicted by a flood of transmissions, and the client has
no way to know (kitty's "delete older images to make space" is a soft
delete of the oldest, but the victim should ideally be invisible).

**Fix:** the eviction skips the images with a placement on screen
while an invisible one is available.
