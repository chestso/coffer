# coffer — unified image store: design + fix plan

Status: **investigation complete, plan written, not yet implemented.**
Owner repo: **coffer** (the engine; portty/nevermore/boba are consumers).
Last updated: 2026-10-21.

> This file is the design AND the state tracker. It is written so the work
> can resume after an outage or an OOM: the "Progress" section at the
> bottom is the resume point — read it first, then the phase it points
> at. Update it in the same commit that completes a step.

## 0. The report

Observed with **nevermore + boba running inside portty** (the user's
report):

1. "image eviction of images appears random, they do not disappear from
   the terminal FIFO"
2. "insertion of an image shows just a blank space"

The user also notes the suspicion that "still images are partly unified
in their storage now" and that "lottie is still a bastard". Both are
correct: the storage IS partly unified (`src/image_store.c`), and the
unification is _half_ done — the store carries three different lifetimes
in one record type.

## 1. What the store looks like today

`src/image_store.h` defines three record kinds that share one array
(`CfrImgStore`):

| Record           | Fields that matter                                                                                                | Who creates it                                                         |
| ---------------- | ----------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------- |
| `CfrImg` (image) | `id`, `version`, `layer`, `source`, `abs_line`, `col`, `w/h`, `buf_w/buf_h`, `rows_tall/cols_wide`, `rgba`, `cap` | `cfr_img_add` (1:1), `cfr_img_add_named` / `cfr_img_blank_named` (1:N) |
| `CfrPlacement`   | `id`, `image_id`, `abs_line`, `col`, `rows/cols`, `layer`, `opacity_x256`, `z_index`, `src_*`, `parent_*`         | `cfr_img_add_placement`                                                |
| `ImgSpare`       | a retained pixel buffer                                                                                           | the free-list pool                                                     |

Three protocols share the store:

| Protocol          | Model | Anchor lives in               | Image id             |
| ----------------- | ----- | ----------------------------- | -------------------- |
| sixel             | 1:1   | the image record (`abs_line`) | engine (`next_id++`) |
| iTerm2 (OSC 1337) | 1:1   | the image record (`abs_line`) | engine (`next_id++`) |
| kitty (APC G)     | 1:N   | the **placement**             | **client's `i=`**    |
| Lottie (APC)      | 1:N   | the **placement**             | **client's `id`**    |

The 1:1 vs 1:N distinction is _inferred at runtime_ by
`img_has_placements()` — "does this image have a placement?" — and that
inference is the root of most of the defects below.

## 2. Findings (each reproduced against the engine)

Repro programs live in `tests/repro/` (one per finding; they print, they
do not assert) and are being turned into regression tests as part of the
work.

### F1 — the 1:1 / 1:N split is inferred, so a transmitted-but-unplaced kitty image is treated as a sixel and dies with the screen

`cfr_img_clear_display_rows()` and `cfr_img_note_scroll()` branch on
`img_has_placements()`. A kitty image that was transmitted (`a=t`) but
not yet placed has **no** placement, so the store treats it as a 1:1
image and culls it by its placeholder anchor (`abs_line == 0`).

Repro (`tests/repro/misclass.c`): after `a=t` → 1 image; after `ESC [ 2 J`
→ **0 images**; after a resize → **0 images**.

Kitty's spec says the opposite: _"The other commands to erase text must
have no effect on graphics"_ and _"When resetting the terminal, all
images **that are visible on the screen** must be cleared"_. A
transmitted image with no placement is not visible, so it must SURVIVE
an ED. coffer deletes it.

**Impact:** the image disappears while its bytes are still in the
terminal's FIFO and the client believes it is displayed. This is the
"they do not disappear from the terminal FIFO" half of the report.

### F2 — the record-count overflow evicts the NEWEST image

`cfr_img_add*()` does `if (st->img_count >= IMG_MAX_IMAGES)
img_rec_release(vt, st, 0);` and `img_rec_release()` ends with
`st->imgs[idx] = st->imgs[--st->img_count];` — a swap-remove. Releasing
index 0 therefore moves the **last** (newest) record into slot 0, and
the next overflow releases _that_ one.

Repro (`tests/repro/evict3.c`): 256 images live; the 257th transmit makes
id 1 vanish and id 257 appear. The oldest image is evicted, the newest
survives — the opposite of the documented "evicts the oldest first".

**Impact:** with more than 256 live images the terminal drops the
_newest_ one — the one the user is looking at. The eviction looks
"random" because it depends on the array's shuffle state.

### F3 — the byte-budget eviction cannot order 1:N images

`img_evict_to_budget()` picks the victim by the lowest `abs_line`. Every
named image (kitty, Lottie) has `abs_line == 0` — the anchor is in the
placement — so all candidates tie and the first one found wins.

Repro (`tests/repro/budget2.c`): ten 16 MB images; after the budget bites
the live set is `9 2 3 4 5 6 7 10` — id 1 (the oldest) survived, id 9
(the newest) died.

**Impact:** the budget path is a random eviction too — and it takes the
newest image with it.

### F4 — one id space for two different id authorities

`st->next_id` is the engine's counter. The 1:1 path takes
`next_id++`; the 1:N path stores the **client's** id verbatim and never
advances `next_id`. So the two id authorities share one space:

- an iTerm2/sixel image takes engine id 1,
- a kitty image with `i=1` also claims id 1,
- and `cfr_img_find_by_id()` resolves the kitty placement to whichever
  record comes first in the dense array.

Repro (`tests/repro/collide2.c`): a kitty image with `i=1` is _replaced_
by a following iTerm2 image (source flips KITTY→ITERM, pixels 8x8 →
40x40) and by a following Lottie animation (source flips KITTY→LOTTIE).
The kitty placement then points at the other protocol's pixels.

**Impact:** "images disappear at random" whenever two protocols (or two
clients with overlapping id spaces) share a session. boba's kitty ids
come from a per-transcript counter that starts at 1, so the FIRST
image of a session collides with the first sixel/iTerm2 image.

### F5 — the kitty path never marks the screen dirty

`cfr_img_add*()` calls `img_damage()`; the kitty path does not (it
places through `cfr_img_add_placement()`, which does not damage). A
kitty placement therefore produces **zero** damage callbacks, and the
host (portty) only repaints when coffer reports damage.

Repro (`tests/repro/damage.c`): `a=T` → 0 damage callbacks; the following
`\r\n` → 1.

**Impact:** the "blank space". The image is in the store, the placement
is there, but the host never repaints those rows — the reserved rows
stay blank until something else dirties them.

### F6 — a re-transmit deletes the placements (kitty says it must)

Kitty's spec: _"When re-transmitting image data for a specific id, the
existing image and all its placements must be deleted. The new data
replaces the old image data but is not actually displayed until a
placement for it is created."_

coffer follows that for `a=t`/`a=T` with an explicit `i=` — but
nevermore/boba re-send the same id for a _new_ picture, and the old
placement disappears with the image. Repro (`tests/repro/repro.c`): the
re-transmit moved the placement to the cursor's row (a new placement
was created) and the old one was gone.

This is spec-conformant, but it is a footgun for a client that reuses
ids. boba never reuses ids (`image_seq` is monotonic for the
transcript's lifetime), so this is **not** the reported bug — it is a
constraint the fix must respect.

### F7 — an unterminated APC string silently drops the payload

The APC accumulator is bounded by `CFR_OSC_BUF_MAX` (256 MB) and the
parser truncates silently (`apc_truncated`) — but a payload that is
never ST-terminated is simply not dispatched at all. Repro
(`tests/repro/budget2.c` with and without the trailing `ESC \`): the
same 21 MB transmit; without the terminator the image never lands and
no error is emitted.

**Impact:** low (boba always terminates), but it is the shape of the
"silently nothing" family the report is about.

### F8 — `content_scale` inflates the buffer dimensions the host reads

`cfr_img_get()` reports `buf_w`/`buf_h` **multiplied by
`content_scale`**, but the pixel buffer is the DECODED (native) size.
portty's `image_get_texture()` copies `buf_w * buf_h * 4` bytes from
`img->rgba`.

Repro (`tests/repro/host_upload.c`, built with ASan): at
`content_scale = 2.0` the host reads 1024 bytes from a 256-byte
allocation — **heap-buffer-overflow**, confirmed by ASan.

**Impact:** on a HiDPI display the kitty/sixel/iTerm2 path reads out of
bounds. This is a memory-safety bug that also corrupts the rendered
image (garbage pixels → "blank space" or noise).

### F9 — the lottie subsystem keeps a parallel record array

`lottie.c` keeps `LtRec` (json arena, ThorVG handles, playback state)
**and** the shared store's `CfrImg` (pixels). It reconciles the two
after every store mutation (`cfr_lottie_note_scroll`,
`cfr_lottie_clear_display_rows` walk the store to find out which
records died). `LtRec.img_index` is written but never read — a stale
index into a swap-removed array.

**Impact:** the "lottie is a bastard" part of the report. It is also a
latent bug: `img_index` points into a dense array that the store
swap-removes underneath it.

## 3. The plan

### Phase 0 — the invariants (write them down first)

The store must satisfy, and the tests must pin:

- **I1** An image's lifetime is decided by its **source**, not by
  whether it happens to have a placement. A 1:N image (kitty, Lottie)
  lives until the client deletes it (`a=d`), until the id is
  re-transmitted, or until the store's budget evicts it. A 1:1 image
  (sixel, iTerm2) lives until the screen area it occupies is erased.
- **I2** A client-supplied id and an engine-assigned id live in
  **different spaces**. A placement names its image by a store-internal
  handle, never by the client's id.
- **I3** Every mutation that changes what the screen shows reports
  damage. An image that appears, changes, or disappears dirties the rows
  it covers.
- **I4** The pixel buffer's dimensions the host reads are the buffer's
  REAL dimensions. Logical→physical scaling happens in the display
  fields, never in the buffer fields.
- **I5** The store's eviction order is well-defined: oldest first, by a
  single, total order over all records (creation order for named
  images, anchor order for 1:1 images).

### Phase 1 — the store's data model (`src/image_store.{h,c}`)

1. Add an explicit `kind` to `CfrImg`: `IMG_KIND_ANCHORED` (1:1 — the
   record carries the anchor) vs `IMG_KIND_REFERENCED` (1:N — the
   placements carry the anchor). Set it at every creation site
   (`cfr_img_add` → ANCHORED; `cfr_img_add_named`/`blank_named` →
   REFERENCED). `img_has_placements()` disappears as a _lifetime_
   decision; it stays only where a "does this image have any placement"
   question is genuinely asked (e.g. "is this image still referenced?",
   for the _soft_ delete of a placement).
2. Give every record a **monotonic creation sequence**
   (`uint64_t seq`) and a **creation order** that the eviction uses.
   `next_id` becomes the engine-id counter and is used ONLY by the 1:1
   path and by kitty's id-less transmits.
3. Split the id space:
   - `CfrImg.id` stays the _public_ id (the client's `i=` for 1:N, the
     engine's for 1:1) — the host's texture cache keys on it.
   - Add `CfrImg.handle` (a store-internal, never-reused handle) and
     make `CfrPlacement.image_handle` point at THAT. The public
     `CfrImagePlacement.image_id` keeps the public id for the host's
     convenience, but the _store's_ lookups go through the handle.
   - Concretely: `CfrPlacement.image_id` → `CfrPlacement.image_handle`
     internally; `cfr_img_get_placements*()` maps the handle back to
     the public id on the way out. A 1:1 image and a 1:N image with the
     same public id then coexist without either shadowing the other.
4. `CfrImg.abs_line` becomes meaningful for 1:N images too: set it to
   the placement's anchor when a placement is added (the minimum
   anchor, so the image's own record can be culled/ordered), or keep it
   0 and give the store a helper `img_anchor_line()` that answers
   "the oldest line this image touches" for both kinds.
5. `buf_w`/`buf_h` are the REAL buffer dimensions (never scaled);
   `w`/`h` are the logical display dimensions. `cfr_img_get()` scales
   only the display fields (as the header already says).

### Phase 2 — the lifetimes (F1, F2, F3)

6. `cfr_img_clear_display_rows()`: an ANCHORED (1:1) image is culled
   when the erased rows overlap it; a REFERENCED (1:N) image is **not**
   culled — its placements are culled only by a kitty `a=d` (which
   coffer does not implement per-placement beyond `d=p`), by a
   re-transmit, or by the budget. Per kitty: "The other commands to
   erase text must have no effect on graphics."
   The only screen-wide clear kitty sanctions is `ESC [ 2 J` ("the clear
   screen escape code should also clear all images") — that one clears
   the _visible_ images (1:1 images and 1:N images that have a
   placement on screen), and leaves invisible ones alone.
7. `cfr_img_note_scroll()`: culls 1:1 images whose anchor scrolled out
   of the scrollback; 1:N images only when their placements all
   scrolled out AND the client has not re-placed them. A kitty image
   whose placement scrolled into the scrollback is still a live image
   (kitty keeps images until deleted), but the placement is off-screen.
   Decide: keep the placement, report it with a negative row (the
   unified-row convention already supports that), and let the host
   cull it.
8. `cfr_img_clear_all()` stays for RIS/altscreen/resize — but the
   altscreen switch must only clear the images of the screen being
   left. Today it clears the whole store. (Kitty: "When switching from
   the main screen to the alternate screen buffer (1049) all images in
   the alternate screen must be cleared".)
9. The overflow path evicts by **creation order** (oldest first), not
   by array index.
10. `img_evict_to_budget()` evicts by the same creation order. It must
    also never evict an image that still has placements _in the
    visible area_ while an invisible one is available — kitty's
    "delete older images to make space" is a soft-delete of the oldest
    by storage order, so oldest-first is right, but the victim must be
    chosen deterministically.

### Phase 3 — the damage (F5)

11. `cfr_img_add_placement()` damages the rows the placement covers
    (`img_damage()`); `cfr_img_remove()`/placement removal damages the
    rows the placement _used_ to cover.
12. The image's own record's `abs_line`/`col` is set for 1:N images
    too (the first placement's anchor), so the host's "one image, many
    placements" rendering keeps working and the damage is computable
    without a placement lookup.

### Phase 4 — the buffer dimensions (F8)

13. `cfr_img_get()` reports `buf_w`/`buf_h` unscaled; the display
    fields (`width_px`/`height_px`) stay scaled. portty's
    `image_get_texture()` then reads exactly the buffer. Add a
    regression test at `content_scale = 2.0` (ASan) that copies
    `buf_w * buf_h * 4` bytes and asserts no overread.
14. Check every other host of `CfrImage` (portty's `render_to_png`
    expands the canvas from `width_px`/`height_px` — fine; the lottie
    path uses its own `canvas_w/h` — fine).

### Phase 5 — the lottie reconciliation (F9)

15. Delete `LtRec.img_index` (it is write-only and wrong after a
    swap-remove).
16. Make the lottie record lookup go through the store's handle
    (`cfr_img_find_by_handle`), so a lottie image that the store
    evicted is detected by the store, not by a parallel walk.
17. Keep the `LtRec` array for the things the store does not model
    (ThorVG handles, the JSON arena, playback state) — that is its job.
    But the "is my image still alive?" question must be answered by the
    store (one source of truth), not by a reconcile pass that walks two
    arrays.

### Phase 6 — the public surface

18. `CfrImagePlacement.image_id` keeps its meaning (the public id of
    the owning image) — no host change needed. A new field
    `CfrImagePlacement.image_handle` may be added for hosts that want
    a stable identity across an id re-use.
19. Document in `include/coffer/coffer.h` the _lifetime_ of an image per
    source — the header currently documents the record, not the
    contract.

### Phase 7 — the tests

New/extended test files (all in `tests/`, no new dependency):

| Test                     | Pins                                                                                                                                                        |
| ------------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `test_cfr_image_store.c` | I1–I5: kind-based lifetimes, id-space separation, deterministic eviction, damage on placement, buffer dims at scale 2                                       |
| `test_cfr_graphics.c`    | the kitty-specific rules: a transmitted-but-unplaced image survives ED2; a re-transmit with the same id drops the placements (kitty's rule); `a=d` variants |
| `test_cfr_lottie.c`      | the lottie/store reconciliation after an eviction                                                                                                           |

Plus a **host-side** regression in portty (`tests/test_kitty.c`): the
texture upload at `content_scale != 1` must not read out of bounds (ASan
run in CI).

### Phase 8 — the consumers

19. **portty**: `render_images()` must use the _placement_ rect for
    1:N images (it does) and the _image_ rect for 1:1 (it does). The
    `img->width_px` vs `buf_w` distinction is the only change needed
    (phase 4). The damage path needs no change once phase 3 lands.
20. **nevermore/boba**: no change is required — boba's kitty bytes are
    correct (`a=T,f=100,s=,v=,c=,r=,i=,q=2,C=1` + chunking + the row
    reservation). The client-id reuse is already avoided by boba's
    monotonic `image_seq`.

## 4. Out of scope (deliberately)

- **Lottie's animation model** (frames, composition, `a=a`/`a=c`) stays
  as it is; only its _storage_ integration changes.
- **kitty's `a=d` subtypes** (`d=c`, `d=n`, `d=r`, …) stay unimplemented;
  the plan only makes sure the store's own lifetime rules do not fight
  them.
- **The `I=` (image number) key** stays parsed-and-ignored.

## 5. Progress

> Resume here. Each step: implement, `make -C build format`,
> `make -C build check`, commit, then tick the box in the SAME commit.

- [x] **Phase 0** — investigation, repro programs, findings F1–F9
- [ ] **Phase 1** — the store's data model (kind, seq, handle, id space)
- [ ] **Phase 2** — the lifetimes (F1, F2, F3)
- [ ] **Phase 3** — the damage (F5)
- [ ] **Phase 4** — the buffer dimensions (F8)
- [ ] **Phase 5** — the lottie reconciliation (F9)
- [ ] **Phase 6** — the public surface + header docs
- [ ] **Phase 7** — the tests
- [ ] **Phase 8** — the consumers (portty)

## 6. Files

| File                                      | Role in this work                                                    |
| ----------------------------------------- | -------------------------------------------------------------------- |
| `src/image_store.h` / `src/image_store.c` | the store: records, ids, lifetimes, eviction, damage, query          |
| `src/graphics.c`                          | the kitty handler (creates named images + placements)                |
| `src/lottie.c`                            | the Lottie handler (parallel record array → reconcile)               |
| `src/sixel.c`                             | the 1:1 path + the public query wrappers                             |
| `src/osc_1337.c`                          | the iTerm2 1:1 path                                                  |
| `src/print.c`                             | ED/EL → `cfr_img_clear_display_rows`; scroll → `cfr_img_note_scroll` |
| `src/modes.c`                             | altscreen / RIS → `cfr_img_clear_all`                                |
| `src/term.c`                              | resize → `cfr_img_clear_all`                                         |
| `include/coffer/coffer.h`                 | the public surface (`CfrImage`, `CfrImagePlacement`)                 |
| `tests/test_cfr_image_store.c`            | the store's invariants                                               |
| `tests/test_cfr_graphics.c`               | the kitty rules                                                      |
| `tests/test_cfr_lottie.c`                 | the lottie/store integration                                         |

## 7. Notes for the implementer

- The store is a **dense array with swap-remove**. Every index cached
  anywhere (`LtRec.img_index`) is a bug waiting to happen; prefer
  lookups by id/handle over cached indices.
- `cfr_img_get*()` return engine-owned scratch arrays that are valid
  "until the next mutation" — the host re-reads them every frame, so a
  re-ordering inside the store is fine, but a _dangling pointer_ is not.
- The damage callback is the host's only repaint trigger. Any new
  mutation path that changes the screen MUST call `cfr_damage_row()` /
  `cfr_damage_all()`.
- The `content_scale` conversion has exactly ONE home: the display
  fields on the way out (`cfr_img_get`). Never scale the buffer
  fields.
- Budget constants live in `src/image_store.h` (`IMG_MAX_IMAGES`,
  `IMG_LIVE_MAX`, `IMG_SPARE_MAX`, `IMG_RETAIN_MAX`).
