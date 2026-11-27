# coffer — follow-ups

Living checklist for coffer. Promote items to PRs as they get worked
on. Order is roughly priority, not strict dependency. Landed work lives
in `docs/` (the wrapped-line spec and its differential fuzzer, the
script engine, the image store); this file tracks what is still open.

## Open

### Kitty keyboard protocol — remaining flags

Implemented: **flag 0x1** (Disambiguate escape codes) and **flag 0x8**
(Report all keys as escape codes). Push/pop/set/query of the flag
stack is wired in `csi.c` and the four special-key paths plus the
Ctrl/Alt+ASCII text path in `keys.c` honour the active flags.
Coverage in `tests/test_cfr_keys.c`.

The remaining three flags are accepted-and-stored on the stack but
have no behavioural effect:

- **0x2 — Report event types** (press / repeat / release). The encoder
  has to be told the event type, so this is an API addition before it is
  a VT one: `cfr_send_key()` grows an event argument (`keys.c`), and the
  host's platform layer supplies it — portty's SDL3 backend has
  `SDL_EVENT_KEY_UP` and a repeat flag on `SDL_EVENT_KEY_DOWN`. Encoding:
  `CSI <code>;<mods>:<event>u` where event is 1=press, 2=repeat,
  3=release.

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

### Left/right margins (DECSLRM, DECSET ?69)

`?69` is accepted and stored as `CFR_MODE_LEFT_RIGHT_MARGINS` (the
warning says "accepted but not enforced"), but margins have no effect:
cursor movement, printing/autowrap, erasing and editing still span the
full row. `cfr_get_mode()` reports the flag as set while output ignores
it; DECRQM is the honest path — it reports `?69` as permanently reset
(`CSI ? 69 ; 4 $ y`), so a probing consumer learns that margins cannot
be relied on.

`CSI Pl ; Pr s` (DECSLRM) shares its final byte with `CSI s` (save
cursor). The save form is now honoured only bare and with DECLRMM off;
a DECSLRM — parameterized, or bare while `?69` is on — is recognized
and ignored instead of silently overwriting the saved cursor that a
later `CSI u` would restore.

Implementing margins means bounding print/wrap and cursor motion by the
left/right margins, deciding which editing operations (ICH/DCH/ECH at
minimum) respect them, homing the cursor as DECSLRM specifies, and
resetting the margins to full width when `?69` is reset. DECSTBM stays
orthogonal (it owns the vertical region). No known consumer of the
engine sends DECSLRM; implement when one does or when strict VT420
conformance becomes a goal.

### OSC 8 inside DCS / SOS-PM-APC strings

Not implemented. Standard OSC parsing only; no support for OSC sequences
embedded within other control string contexts.

**Benefit:** Handles exotic escape sequences from non-standard or
experimental terminals. Not a real corpus case — no known applications
emit this pattern. Purely defensive.

### Kitty graphics protocol — remaining gaps

The core protocol is implemented (`src/graphics.c`, tests in
`test_cfr_graphics.c`): transmit `a=t`/`a=T`, place `a=p` (absolute,
`U=1` virtual, `P=`/`Q=` relative-to-parent), delete `a=d` with the
full kitty delete table (`d=a/c/i/n/p/q/r/x/y/z` and their uppercase
data-freeing twins), image numbers (`I=`), capability query `a=q`,
chunked upload (`m=`), zlib compression (`o=z`), formats
`f=24`/`32`/`100`, cursor advance (`c=`/`r=`), z-index, and the ConPTY
OSC 5556 carrier workaround. The delete forms follow kitty's semantics
exactly: the lowercase form removes placements and keeps the image data
for re-display, the uppercase form also frees the data of matched
images left without placements, `d=p` means "placements intersecting
the cell `x=`,`y=`" (placement-by-id is `d=i,i=..,p=..`), and a delete
aborts a chunked upload in flight. An id-less image's data dies with
its last placement, as in kitty.

Not yet implemented:

- **Unicode placeholders (`q=1`)** — no placeholder-cell rendering or
  cursor-movement-via-placeholder-text. The `e=` feature queries report
  this honestly: `e=T,U=1` replies without `U=1`.

The animation model: `a=f` stores frames (partial-rect compose onto a
base frame with `X=`/`Y=`/`c=`/`r=`, gaps via `z=` incl. gapless
negatives), `a=a` drives it (`c=` current frame, `s=1/2/3` stop /
loading / run, `v=` loops, `r=`+`z=` per-frame gaps), `a=c` composes
rectangles between frames (C=1 overwrite, EINVAL on bounds and
self-overlap, ENOENT on unknown frames), and `d=f` deletes frames. The
host advances playback with `cfr_kitty_tick()` / `cfr_kitty_present()`;
a frame switch copies the frame into the image's pixels (version bump +
damage), so the renderer is unchanged. Frame memory counts into the
store's live-byte budget and evicts with the image. Feature queries
(`e=`) answer with exactly the supported keys per action.

**Benefit:** Full spec compliance for kitty-native clients (timg,
chafa, and tools driving kitty animations). Most consumers only need
the transmit/place/delete paths, which already work.

#### Follow-ups from the spec work

- **Real-client smoke test.** The tests feed synthetic APC streams; a
  pass with a real client (chafa slideshow with `--clear`, timg, `kitten
  icat --animate`) through the coffer CLI or a PTY harness would catch
  wire-format divergences the unit tests cannot. Worth doing once before
  relying on the animation paths in a front end.
- **One animation clock (optional).** The store's `cfr_img_tick` drives
  kitty frames; Lottie keeps its own ThorVG clock. A per-source
  `advance` hook on the policy table would let one host call drive both,
  with `cfr_lottie_present/tick` as wrappers. Don't hold anything on it —
  only if the host side wants a single scheduler call.

## Landed — kept so the decisions are not re-litigated

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
  `PSEUDOCONSOLE_GLYPH_WIDTH_GRAPHEMES` in 1.25). It has adopted this
  directory's lock and fetch/update/verify scripts (portty `10d9555`) and
  no longer tracks the binaries; its diagnostics report the pinned package
  version. Portty's `-S` exit status and `await-exit` races are fixed in
  portty `1dcec02`.

### `cfr_image_decode` returns C-library memory

stb_image and the TIFF fallback both allocate with the C library's
`malloc`; `src/image_decode.c` documents the boundary at the one entry
point, which is where the rule lives. Callers either free with `free()`
(iTerm2 keeps no engine-owned copy of the decoded buffer) or copy onto
the terminal's allocator (kitty `f=100`, zlib). Decision: leave the
decoders on the C allocator rather than thread a `CfrTerm` through them
— the decoded buffer never crosses the public API, so a custom allocator
sees no foreign pointer.
