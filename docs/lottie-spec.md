# Lottie Animation Protocol — coffer Engine Specification

This document specifies the Lottie animation protocol as implemented in
coffer. It covers the terminal escape sequence wire format, the command
vocabulary, the engine-side state machine, the public C API, rasterization,
memory management, and interaction with VT subsystems (scroll, clear, resize).

For the host-side renderer design (texture cache, compositing, linear-light
pipeline), see the portty documentation.

---

## 1. Wire Format

### 1.1 APC Escape Sequence

Lottie animations use **APC** (Application Program Command, `ESC _`), per
ECMA-48 §7.2.1. The payload is a JSON object, base64-encoded.

7-bit representation:

```
ESC _ <base64-json> ESC \
```

Bytes: `\x1b` `_` `<base64-string>` `\x1b` `\`

**Windows ConPTY fallback** — ConPTY strips APC sequences even in passthrough
mode on some builds. On Windows, use OSC 5555 instead:

```
ESC ] 5 5 5 5 ; <base64-json> BEL
```

Bytes: `\x1b]5555;` `<base64-string>` `\x07`

coffer routes both APC and OSC 5555 to `cfr_lottie_apc_dispatch()`, so the
payload semantics are identical.

### 1.2 Rationale

**APC over OSC/DCS:**

- **ECMA-48 semantics**: APC is "command string for an application program" — a
  graphics protocol is exactly that.
- **Kitty precedent**: Kitty's graphics protocol uses APC, establishing it as the
  de facto channel for terminal graphics.
- **Safe in other terminals**: xterm states "xterm implements no APC functions;
  Pt is ignored." All well-behaved terminals silently discard unknown APC
  strings.
- **No namespace collision**: OSC codes are a flat numeric namespace. APC has no
  collision risk — the entire string is application-defined.
- **DCS is reserved for sixel**: DCS is already used for sixel's streaming
  decoder. Lottie JSON is an atomic document, not a streaming format.

**Base64 encoding:**

- Lottie JSON contains `"`, `\`, and characters that could conflict with the ST
  terminator. Base64 avoids all delimiter collisions.
- The ~33% overhead is acceptable: Lottie JSON is typically 1–50 KB and
  transmitted once (not per-frame).

### 1.3 Buffer Limits

The APC accumulator shares the same buffer as OSC:

```c
#define CFR_OSC_BUF_BYTES  65536u   /* parser internal limit */
```

Payloads exceeding 65536 bytes of base64 are truncated and discarded. For large
Lottie files, use the `load-chunk` command (§2.6).

---

## 2. Command Vocabulary

Each APC payload is a JSON object with a `"cmd"` field. The engine decodes the
base64, extracts the `"cmd"` key via `lt_json_find_key()`, and dispatches to
the appropriate handler.

### 2.1 `load` — Upload a Lottie animation

```json
{
  "cmd": "load",
  "id": 1,
  "lottie": { ... },
  "fit": "contain",
  "scale": 1.0,
  "placement": {
    "row": 5,
    "col": 10,
    "cols": 20,
    "rows": 10
  },
  "layer": "background",
  "opacity": 0.85,
  "play": {
    "speed": 1.0,
    "loop": true,
    "autostart": true
  }
}
```

| Field              | Type   | Default        | Description                                                              |
| ------------------ | ------ | -------------- | ------------------------------------------------------------------------ |
| `id`               | int    | **required**   | Client-assigned identifier (1–4294967295). Stable across commands        |
| `lottie`           | object | **required**   | Complete Lottie JSON body                                                |
| `fit`              | string | `"contain"`    | Sizing mode: `"contain"` or `"none"` (see §2.2)                          |
| `scale`            | float  | 1.0            | Uniform scale factor. Only used when `fit: "none"`. Ignored for contain. |
| `placement`        | object | cursor pos     | Placement region and position (see §2.2)                                 |
| `placement.row`    | int    | cursor row     | Top-left of the region                                                   |
| `placement.col`    | int    | cursor col     | Top-left of the region                                                   |
| `placement.cols`   | int    | 0 (unset)      | Region width in cells. Used for fitting (`contain`) and centering.       |
| `placement.rows`   | int    | 0 (unset)      | Region height in cells. Used for fitting (`contain`) and centering.      |
| `placement.width`  | int    | 0 (unset)      | Region width in px. Alternative to `cols`.                               |
| `placement.height` | int    | 0 (unset)      | Region height in px. Alternative to `rows`.                              |
| `layer`            | string | `"foreground"` | `"background"` or `"foreground"`                                         |
| `opacity`          | float  | 1.0            | Global alpha (0.0–1.0)                                                   |
| `play`             | object | —              | Playback parameters                                                      |
| `play.speed`       | float  | 1.0            | Playback rate multiplier                                                 |
| `play.loop`        | bool   | true           | Loop at end                                                              |
| `play.autostart`   | bool   | true           | Start playing immediately                                                |
| `report`           | bool   | false          | If true, emit an APC report after placement (see §2.8)                   |

**Fit modes:**

| `fit`       | Sizing                                              | Centering                   |
| ----------- | --------------------------------------------------- | --------------------------- |
| `"contain"` | Scale to fit within region (largest aspect-correct) | Cell box centered in region |
| `"none"`    | Scale by explicit `scale` field (default 1.0)       | Cell box centered in region |

**Size computation** (always aspect-correct):

```
if fit == "contain":
    # Convert region to px, take tightest
    px_max_w = width > 0 ? width : infinity
    px_max_h = height > 0 ? height : infinity
    if cols > 0: px_max_w = min(px_max_w, cols * cell_w_px)
    if rows > 0: px_max_h = min(px_max_h, rows * cell_h_px)
    scale = min(px_max_w / design_w, px_max_h / design_h)
else:  # fit == "none"
    scale = explicit scale field

raster_w = round(design_w * scale)
raster_h = round(design_h * scale)
```

**Placement cells** are engine-computed from the rasterization size:

```
cols = ceil(raster_w / cell_w_px)
rows = ceil(raster_h / cell_h_px)
```

**Centering**: the cell box is always centered within the region defined by
`placement.cols`/`placement.rows`:

```
actual_row = row + (rows - cell_rows) / 2
actual_col = col + (cols - cell_cols) / 2
```

The host renderer additionally centers the rasterized texture within the cell
box with transparent padding — this is automatic and not client-controllable.

If no region is specified (`cols`/`rows`/`width`/`height` all 0) and `fit` is
omitted, the animation renders at design size (scale = 1.0) with no centering
offset.

**Replacement**: sending `load` with an existing `id` replaces the animation
in-place. The pixel buffer is reused if dimensions match; ThorVG state is
rebuilt. The version bumps, signaling the host to re-upload pixels.

**Default placement**: if `placement` is omitted, the animation is placed at
the current cursor position with rasterization at design size (scale = 1.0).
Cell dimensions are computed from the rasterization size and the terminal's
cell pixel dimensions:

```
cols = ceil(raster_w / cell_w_px)
rows = ceil(raster_h / cell_h_px)
```

### 2.2 Placement Coordinates

Placements are anchored by **absolute line** (`abs_line`), not display row.
This means animations scroll with text — identical to sixel images.

- `row`/`col` are in terminal cell coordinates (0-based), defining the top-left
  of the region.
- `cols`/`rows` define the region size in cells. The animation is scaled to
  fit within this region (aspect-correct, `fit: "contain"`), or rendered at
  explicit scale (`fit: "none"`) and centered within the region.
- `width`/`height` are alternatives to `cols`/`rows` in pixels.
- The engine-computed cell box (`ceil(raster / cell_px)`) is centered within
  the region. The host renderer additionally centers the rasterized texture
  within the cell box with transparent padding — this is automatic.
- The rasterization pixel dimensions are `design_w * scale` by `design_h *
scale` — always aspect-correct.
- `abs_line = sixel_abs_top + row` at the time of placement.

### 2.3 `place` — Add or update a placement

```json
{
  "cmd": "place",
  "id": 1,
  "fit": "none",
  "scale": 1.0,
  "placement": {
    "row": 0,
    "col": 0,
    "cols": 40,
    "rows": 20
  },
  "layer": "background",
  "opacity": 0.9
}
```

An animation can have **multiple placements** at different cell positions. Each
placement can independently specify `layer` and `opacity`.

- `fit`/`scale`/`placement.cols`/`placement.rows`/`placement.width`/`placement.height`
  optional — if any present and different from current, triggers seamless
  re-rasterization + buffer realloc + cell recompute. Playback continues from
  the current frame.
- `report` (bool, default false) — if true, emit an APC report after placement (see §2.8).

**Deduplication**: placements are keyed by `(abs_line, col)`. Sending `place`
with the same position as an existing placement **updates** that placement's
`layer` and `opacity` (and size if constraints changed) rather than creating a
duplicate. This enables efficient toggling (e.g. foreground ↔ background) at
the same position.

Each placement is auto-assigned a stable `id` by the engine (incrementing
counter in `CfrLottieState.next_placement_id`).

### 2.4 `play` / `pause` / `stop` — Control playback

```json
{ "cmd": "play",  "id": 1, "speed": 2.0, "loop": false }
{ "cmd": "pause", "id": 1 }
{ "cmd": "stop",  "id": 1 }
```

| Command | Effect                                                 |
| ------- | ------------------------------------------------------ |
| `play`  | Start or resume playback. Optional `speed` and `loop`. |
| `pause` | Freeze at the current frame.                           |
| `stop`  | Reset to `frame_ip` (in-point) and pause.              |

`play` fields:

| Field   | Type  | Default | Description              |
| ------- | ----- | ------- | ------------------------ |
| `speed` | float | 1.0     | Playback rate multiplier |
| `loop`  | bool  | true    | Loop at end              |

### 2.5 `seek` — Jump to a specific frame

```json
{ "cmd": "seek", "id": 1, "frame": 15 }
```

- `frame`: 0-based frame index. Clamped to the `[ip, op)` range.
- If the frame changes, the animation is re-rasterized and the version bumps.

### 2.6 `delete` — Remove an animation

```json
{ "cmd": "delete", "id": 1 }
```

Removes the animation and all its placements. The pixel buffer is released to
the spare pool (or freed if over the retain limit). The host's GPU texture is
freed on the next frame's cache reconciliation pass.

### 2.7 `load-chunk` — Upload a large animation in chunks

For Lottie files whose base64 payload exceeds a practical single-APC size limit,
chunk the upload:

```json
{ "cmd": "load-chunk", "id": 1, "seq": 0, "total": 3, "data": "<base64-part-0>" }
{ "cmd": "load-chunk", "id": 1, "seq": 1, "total": 3, "data": "<base64-part-1>" }
{ "cmd": "load-chunk", "id": 1, "seq": 2, "total": 3, "data": "<base64-part-2>" }
```

| Field   | Type   | Description                             |
| ------- | ------ | --------------------------------------- |
| `seq`   | int    | 0-based chunk index                     |
| `total` | int    | Total number of chunks                  |
| `data`  | string | Base64-encoded chunk of the Lottie JSON |

- The engine concatenates all `data` fields (after base64-decoding each) before
  parsing the Lottie JSON.
- A `load-chunk` with `seq == total - 1` triggers JSON parsing, ThorVG
  initialization, rasterization of frame 0, and adds a default placement.
- If a new `load-chunk` with `seq == 0` arrives while a previous chunked upload
  is in progress, the previous upload is discarded.

### 2.8 Report — Engine-to-client feedback

The engine emits a report **only when the client requests it** by including
`"report": true` in the `load` or `place` command. This is opt-in because the
report is sent via the output callback to the PTY — the child process receives
it on stdin. Clients that don't expect report data (e.g. TUI frameworks that
interpret raw bytes as key events) should not request reports.

The report uses the same wire format as commands — APC with base64-encoded
JSON on POSIX, OSC 5556 with base64-encoded JSON on Windows (mirroring the
OSC 5555 command carrier):

| Direction       | POSIX                       | Windows                          |
| --------------- | --------------------------- | -------------------------------- |
| Client → engine | `ESC _ <base64-json> ESC \` | `ESC ] 5555 ; <base64-json> BEL` |
| Engine → client | `ESC _ <base64-json> ESC \` | `ESC ] 5556 ; <base64-json> BEL` |

The report payload is a JSON object:

```json
{
  "type": "report",
  "id": 1,
  "row": 5,
  "col": 10,
  "rows": 4,
  "cols": 8,
  "raster_w": 375,
  "raster_h": 375,
  "cell_w_px": 9,
  "cell_h_px": 22
}
```

| Field       | Type | Description                                 |
| ----------- | ---- | ------------------------------------------- |
| `type`      | str  | Always `"report"`                           |
| `id`        | int  | Animation id                                |
| `row`       | int  | Placement row (top-left of region)          |
| `col`       | int  | Placement col (top-left of region)          |
| `rows`      | int  | Cell rows occupied (engine-computed)        |
| `cols`      | int  | Cell cols occupied (engine-computed)        |
| `raster_w`  | int  | Rasterization width in px (design × scale)  |
| `raster_h`  | int  | Rasterization height in px (design × scale) |
| `cell_w_px` | int  | Terminal cell width in px                   |
| `cell_h_px` | int  | Terminal cell height in px                  |

**Requesting a report**: add `"report": true` to the `load` or `place` command:

```json
{"cmd":"load","id":1,"lottie":{...},"report":true}
{"cmd":"place","id":1,"placement":{"cols":20},"report":true}
```

---

## 3. Engine-Side State Machine

### 3.1 Dispatch Flow

```
                         ┌──────────────────────┐
  APC ──────────────────►│ cfr_lottie_apc_      │
                         │ dispatch()           │
                         └──────────┬───────────┘
                                    │
                           base64-decode
                           JSON parse "cmd"
                                    │
              ┌─────────────────────┼─────────────────────┐
              ▼                     ▼                     ▼
        ┌──────────┐        ┌──────────────┐       ┌───────────┐
        │  load /  │        │  play/pause/ │       │  delete / │
        │  load-   │        │  stop/seek / │       │  place    │
        │  chunk   │        │  place       │       │           │
        └────┬─────┘        └──────┬───────┘       └─────┬─────┘
             │                     │                     │
             ▼                     ▼                     ▼
  ┌───────────────────┐   ┌──────────────────┐   ┌─────────────────┐
  │ Parse Lottie JSON │   │ Update playback  │   │ Remove record + │
  │ Init ThorVG       │   │ state in LtRec   │   │ destroy painter │
  │ Rasterize frame 0 │   │ Re-rasterize if  │   │ release rgba buf│
  │ Un-premultiply +  │   │ frame changed    │   │ release arena   │
  │ BGRA→RGBA +       │   │ version++        │   │ version++       │
  │ sRGB→linear       │   └──────────────────┘   └─────────────────┘
  │ Store in LtRec    │
  └───────────────────┘
```

### 3.2 Parser Integration

The parser has a dedicated `CFR_STATE_APC_STRING` state. On `ESC _`, the parser
enters this state and accumulates bytes into `apc_buf[]`. On ST (`ESC \`) or
BEL, it dispatches:

```c
/* parser.c — ESC _ transitions to APC_STRING state */
case CFR_STATE_APC_STRING:
    /* ST/BEL received → dispatch the accumulated APC payload */
    cfr_lottie_apc_dispatch(vt, p->apc_buf, p->apc_len);
    break;
```

OSC 5555 routes to the same dispatch function:

```c
/* osc.c — OSC code 5555 → lottie dispatch */
case 5555:
    cfr_lottie_apc_dispatch(vt, body, body_len);
    break;
```

### 3.3 Internal Data Structures

#### LtRec — Per-animation record

```c
typedef struct
{
    uint64_t id;             /* client-assigned, stable cache key */
    uint32_t version;        /* bumps on any state/pixel change   */

    void     *json_root;     /* unused (retained for struct compat) */

    uint8_t  *rgba;          /* RGBA32 pixel data for current frame */
    size_t    rgba_cap;      /* allocated bytes                     */

    int       design_w;      /* Lottie JSON width  (abstract)  */
    int       design_h;      /* Lottie JSON height (abstract)  */

    int       px_w;          /* rasterization pixel width  */
    int       px_h;          /* rasterization pixel height */

    int       current_frame; /* 0-based, within [ip, op) */
    int       frame_ip;      /* Lottie in-point          */
    int       frame_op;      /* Lottie out-point         */
    double    frame_fr;      /* Lottie framerate         */
    double    speed;         /* playback rate multiplier */
    bool      playing;       /* actively advancing?      */
    bool      loop;          /* loop at end?             */
    bool      dirty;         /* frame changed, rgba needs re-upload */

    uint64_t  last_tick_us;  /* last frame advance timestamp */

    CfrLottiePlacement *placements;
    int         placement_count;
    int         placement_cap;

#ifdef HAVE_THORVG
    Tvg_Animation tvg_anim;
    Tvg_Canvas    tvg_canvas;
#endif

    uint8_t  *arena_base;    /* raw Lottie JSON for ThorVG */
    size_t     arena_offset;
    size_t     arena_cap;
} LtRec;
```

Directly analogous to `SxRec`: owns the pixel buffer, has `id` + `version` for
cache invalidation.

#### LtChunkAccum — Chunked upload accumulator

```c
typedef struct
{
    uint64_t id;
    uint8_t *buf;
    size_t   buf_len;
    size_t   buf_cap;
    int      chunks_received;
    int      chunks_total;
} LtChunkAccum;
```

One per in-progress chunked upload. Discarded when a new `seq == 0` chunk
arrives for the same `id`.

#### CfrLottieState — Global lottie subsystem

```c
struct CfrLottieState
{
    LtRec         *recs;
    int            rec_count;
    int            rec_cap;
    uint64_t       next_placement_id;
    size_t         live_bytes;    /* sum of all LtRec.rgba_cap */

    LtChunkAccum  *chunks;
    int            chunk_count;
    int            chunk_cap;

    LtSpare        spares[LT_SPARE_MAX]; /* 16 spare RGBA buffers */
    int            spare_count;
    size_t         retain_bytes;

    uint8_t       *scratch;       /* snapshot buffer for cfr_get_lotties() */
    size_t         scratch_cap;
    CfrLottiePlacement *pl_scratch;
    int            pl_scratch_cap;
};
```

Lazily allocated: `vt->lottie` is `NULL` until the first lottie APC arrives.
`cfr_have_lottie()` returns whether ThorVG is available at runtime.

---

## 4. Public API

All public types and functions are declared in `<coffer/coffer.h>`.

### 4.1 Types

#### CfrLottiePlacement

```c
typedef struct
{
    uint64_t id;           /* auto-assigned stable placement id */
    long     abs_line;     /* absolute line (scrolls with text)  */
    int      row;          /* display-relative row (after centering) */
    int      col;          /* display-relative col (after centering) */
    int      rows;         /* engine-computed cell box height    */
    int      cols;         /* engine-computed cell box width     */
    uint8_t  layer;        /* 0 = foreground, 1 = background    */
    uint8_t  opacity_x256; /* 0–255                              */
} CfrLottiePlacement;
```

`row`/`col` are the final placement position after centering within the
region. `rows`/`cols` are the engine-computed cell box dimensions derived
from the rasterization size (`ceil(raster / cell_px)`), not the client-specified
region.

#### CfrLottie

```c
typedef struct
{
    uint64_t      id;
    uint32_t      version;
    int           canvas_w;        /* rasterization width in px (design × scale)  */
    int           canvas_h;        /* rasterization height in px (design × scale) */
    const uint8_t *rgba;           /* canvas_w × canvas_h × 4, engine-owned */
    int           current_frame;
    int           frame_count;     /* frame_op - frame_ip            */
    bool          playing;
    double        speed;
    bool          loop;
    int           placement_count;
} CfrLottie;
```

`rgba` is valid until the next `cfr_input_write()` or `cfr_get_lotties()`
call. The host must copy or upload pixels before the next engine mutation.

### 4.2 Query Functions

```c
/* Snapshot of all active animations. Returned pointer valid until next
 * cfr_input_write / cfr_get_lotties. */
const CfrLottie *cfr_get_lotties(CfrTerm *vt, int *out_count);

/* Placements for a specific animation. .row is pre-computed for the
 * renderer (abs_line - sixel_abs_top). Valid until next call. */
const CfrLottiePlacement *cfr_get_lottie_placements(CfrTerm *vt, uint64_t id,
                                                    int *out_count);
```

### 4.3 Tick Function

```c
/* Advance all playing animations to the frame appropriate for now_us.
 * Re-rasterize any whose frame changed. Call once per frame before
 * cfr_get_lotties(). Returns true if any animation advanced. */
bool cfr_lottie_tick(CfrTerm *vt, uint64_t now_us);
```

The first call to `cfr_lottie_tick()` for a given animation establishes the
baseline timestamp (`last_tick_us`). The second call actually advances frames
based on elapsed time. This means the host must call `cfr_lottie_tick()` on
every frame, even if no animations have been loaded yet (the function is a
no-op when `vt->lottie == NULL`).

### 4.4 Scroll and Clear Functions

```c
/* Cull placements that scrolled past scrollback. */
void cfr_lottie_note_scroll(CfrTerm *vt, int lines);

/* Remove foreground placements in the given row range.
 * Background placements survive (text erase should not remove
 * background decorations). */
void cfr_lottie_clear_display_rows(CfrTerm *vt, int top, int bot);
```

### 4.5 Configuration

```c
/* Update cell pixel dimensions (called on resize). Existing placements
 * are NOT re-rasterized; only new placements use updated dimensions. */
void cfr_set_cell_pixels(CfrTerm *vt, int cell_w_px, int cell_h_px);

/* Returns true if ThorVG was found at build time and lottie rendering
 * is available. When false, APC sequences are still accepted but the
 * RGBA buffer is zeroed. */
bool cfr_have_lottie(void);
```

### 4.6 API Comparison with Sixel

| Sixel                            | Lottie                                      | Notes                                   |
| -------------------------------- | ------------------------------------------- | --------------------------------------- |
| `cfr_get_sixels(vt, &count)`     | `cfr_get_lotties(vt, &count)`               | Same pull model, contiguous arrays      |
| `CfrSixel.id` / `.version`       | `CfrLottie.id` / `.version`                 | Same cache-key pattern                  |
| `CfrSixel.rgba`                  | `CfrLottie.rgba`                            | Engine-owned, valid until next mutation |
| `CfrSixel.row/col/layer`         | `CfrLottiePlacement.row/col/layer`          | Separate placement query API            |
| N/A                              | `cfr_lottie_tick(vt, now_us)`               | Frame advancement                       |
| N/A                              | `cfr_get_lottie_placements(vt, id, &count)` | Separate placement query with `.row`    |
| `cfr_sixel_note_scroll()`        | `cfr_lottie_note_scroll()`                  | Same cull logic                         |
| `cfr_sixel_clear_display_rows()` | `cfr_lottie_clear_display_rows()`           | Same clear logic                        |

---

## 5. Frame Advancement and Rasterization

### 5.1 Tick Algorithm

`cfr_lottie_tick(vt, now_us)` advances all playing animations:

```
For each LtRec where playing == true:
    elapsed = now_us - last_tick_us
    frame_delta = elapsed × speed × fr / 1_000_000
    new_frame = current_frame + floor(frame_delta)
    if new_frame >= op:
        if loop:
            new_frame = ip + (new_frame - ip) % (op - ip)
        else:
            new_frame = op - 1
            playing = false
    if new_frame != current_frame:
        current_frame = new_frame
        lt_rasterize(rec)            // re-rasterize into rec->rgba
        rec->dirty = true
        rec->version++
    last_tick_us += frame_delta × 1_000_000 / (speed × fr)
```

**Important**: the first tick establishes `last_tick_us` and returns `false`
(no frame advance). The second tick computes elapsed time and actually advances
frames. Hosts must call `cfr_lottie_tick()` every frame.

### 5.2 Rasterization Pipeline

When ThorVG is available (auto-detected via pkg-config `thorvg-1`):

```c
static void lt_rasterize(LtRec *r) {
    // Set ThorVG animation frame
    tvg_animation_set_frame(r->tvg_anim, (float)r->current_frame);

    // Rasterize into our RGBA buffer via ThorVG SW canvas
    tvg_canvas_update(r->tvg_canvas);
    tvg_canvas_draw(r->tvg_canvas, true);
    tvg_canvas_sync(r->tvg_canvas);

    // ThorVG outputs premultiplied BGRA — convert to non-premultiplied
    // linear-light RGBA for correct compositing in the host renderer.
    // 1. Un-premultiply (divide RGB by alpha × 255)
    // 2. Swap R↔B (BGRA → RGBA byte order)
    // 3. Linearize each RGB channel (sRGB → linear)
    lt_linearize_rgba(r->rgba, r->px_w, r->px_h);
    r->dirty = true;
}
```

**Without ThorVG** (`--disable-thorvg` at build time): APC sequences are still
accepted and animation state is tracked, but the RGBA buffer is zeroed (all
pixels are transparent black). `cfr_have_lottie()` returns `false`.

### 5.3 Pixel Format

The engine outputs **non-premultiplied linear-light RGBA32** pixels:

| Step | Input              | Transform                | Output            |
| ---- | ------------------ | ------------------------ | ----------------- |
| 1    | Premultiplied BGRA | Un-premultiply           | Non-premulti BGRA |
| 2    | Non-premulti BGRA  | Swap R↔B                 | Non-premulti RGBA |
| 3    | Non-premulti RGBA  | sRGB → linear (RGB only) | Linear-light RGBA |

Alpha is left unchanged (non-premultiplied) through all steps. This format
composites correctly in the host's linear-light render pipeline without any
additional conversion.

---

## 6. Memory Management

### 6.1 Resource Overview

| Resource                 | Owner  | Lifetime      |
| ------------------------ | ------ | ------------- |
| Raw Lottie JSON body     | engine | load → delete |
| ThorVG painter + surface | engine | load → delete |
| RGBA pixel buffer        | engine | load → delete |
| GPU texture cache        | host   | load → delete |

### 6.2 Constants

```c
#define LT_MAX_ANIMS      64
#define LT_MAX_PLACEMENTS 32
#define LT_SPARE_MAX      16
#define LT_LIVE_MAX       (128 * 1024 * 1024)   /* 128 MiB */
#define LT_RETAIN_MAX     (64 * 1024 * 1024)    /* 64 MiB  */
```

### 6.3 RGBA Buffer Pool

Following the same pattern as coffer's `SxSpare` free-list pool:

```c
typedef struct {
    uint8_t *buf;
    size_t   cap;
} LtSpare;
```

- On animation delete, the RGBA buffer is retained in the spare pool if
  `retain_bytes < LT_RETAIN_MAX` (64 MiB). Otherwise it is freed.
- On animation load, `lt_buf_alloc()` searches the pool for a best-fit spare
  before allocating fresh memory.
- Animation replacement reuses the buffer in-place if `rec->rgba_cap >= need`.

### 6.4 Budget Enforcement

| Budget              | Value     | Enforcement                                                   |
| ------------------- | --------- | ------------------------------------------------------------- |
| Max animations      | 64        | Evict animation with oldest `abs_line` (LRU by scroll)        |
| Max live pixels     | 128 MiB   | Evict oldest animations until budget is clear                 |
| Max spare retention | 64 MiB    | Free buffers that exceed the retain limit                     |
| Max per-animation   | 32 places | `LT_MAX_PLACEMENTS` — subsequent `place` commands are ignored |

### 6.5 Per-Frame Allocation Cost: Zero

The critical design property: **advancing a frame produces no allocations.**

| Event                           | Allocations                                        |
| ------------------------------- | -------------------------------------------------- |
| `load`                          | 1 arena, 1 RGBA buffer, 1 ThorVG painter+surface   |
| `delete`                        | 0 (arena free, RGBA → spare pool, painter destroy) |
| Frame advance (tick)            | 0 (in-place rasterize + memcpy into existing buf)  |
| Replace (`load` with same `id`) | 0 (arena reset + reuse, RGBA buffer reuse)         |
| Scroll-off cull                 | 0 (same as delete)                                 |

---

## 7. Interaction with VT Subsystems

### 7.1 Cell Ownership

Lottie animations **do not replace or modify cells**. The cells beneath an
animation retain their text, colors, and SGR attributes. The animation is a
supplemental visual layer composited by the host renderer.

### 7.2 Scroll Behavior

Lottie placements scroll identically to sixel images:

- Anchored by `abs_line` in coffer's absolute coordinate space.
- `cfr_lottie_note_scroll()` culls placements that scrolled past scrollback.
- The host renderer maps `abs_line` → display row:
  `screen_row = abs_line - abs_top + scroll_offset`.

### 7.3 Clear Behavior

- **ED** (erase display): `cfr_lottie_clear_display_rows()` removes foreground
  placements in the cleared row range. Background placements survive — text
  erase should not remove background decorations.
- **Terminal reset**: all animations are freed during `cfr_free()`.

### 7.4 Resize

On terminal resize, `cfr_set_cell_pixels()` updates the engine's cell pixel
dimensions. **Existing placements are not re-rasterized.** The pixel dimensions
(`px_w`/`px_h`) of existing animations remain based on the cell size at the
time of placement. Only new placements use the updated cell pixel dimensions.

`design_w`/`design_h` (from Lottie JSON `w`/`h`) remain unchanged for all
animations.

### 7.5 Alternate Screen

Switching to alternate screen (mode 1049) clears all Lottie placements in the
alternate screen, matching sixel behavior.

### 7.6 Selection and Cursor

Selection highlights render on top of background Lottie but below foreground
Lottie (enforced by the host's render pipeline ordering). The cursor renders
within the cell pipeline. A foreground Lottie may obscure the cursor — this
matches existing sixel foreground behavior and is acceptable.

---

## 8. Complete Protocol Examples

### 8.1 Minimal Load (foreground spinner)

```
ESC _ eyJjbWQiOiJsb2FkIiwiaWQiOjEsImxvdHRpZSI6eyJ2IjoiNS42LjAiLCJmciI6MzAsImlwIjowLCJvcCI6OTAsInciOjQwLCJoIjo0MCwibGF5ZXJzIjpbXX0sInBsYWNlbWVudCI6eyJyb3ciOjUsImNvbCI6MTAsInJvd3MiOjIsImNvbHMiOjJ9LCJsYXllciI6ImZvcmVncm91bmQifQ== ESC \
```

Decoded JSON:

```json
{
  "cmd": "load",
  "id": 1,
  "lottie": {
    "v": "5.6.0",
    "fr": 30,
    "ip": 0,
    "op": 90,
    "w": 40,
    "h": 40,
    "layers": []
  },
  "placement": { "row": 5, "col": 10, "rows": 2, "cols": 2 },
  "layer": "foreground"
}
```

### 8.2 Background Animation with Opacity

```json
{
  "cmd": "load",
  "id": 2,
  "lottie": { ... },
  "placement": {"row":0,"col":0,"rows":24,"cols":80},
  "layer": "background",
  "opacity": 0.3
}
```

### 8.3 Pause and Seek

```
APC eyJjbWQiOiJwYXVzZSIsImlkIjoxfQ== ST
APC eyJjbWQiOiJzZWVrIiwiaWQiOjEsImZyYW1lIjoxNX0= ST
```

### 8.4 Multiple Placements (same animation, different positions)

```json
{ "cmd": "load",  "id": 3, "lottie": { ... } }
{ "cmd": "place", "id": 3, "placement": {"row":0,"col":0,"rows":2,"cols":2},  "layer": "foreground" }
{ "cmd": "place", "id": 3, "placement": {"row":0,"col":78,"rows":2,"cols":2}, "layer": "foreground" }
```

### 8.5 Same-Position Update (foreground → background toggle)

```json
{ "cmd": "place", "id": 1, "placement": {"row":5,"col":10,"rows":2,"cols":2}, "layer": "foreground", "opacity": 1.0 }
{ "cmd": "place", "id": 1, "placement": {"row":5,"col":10,"rows":2,"cols":2}, "layer": "background", "opacity": 0.5 }
```

The second `place` updates the existing placement at `(row=5, col=10)` — it
does not create a duplicate.

### 8.6 Cleanup

```json
{ "cmd": "delete", "id": 1 }
{ "cmd": "delete", "id": 2 }
{ "cmd": "delete", "id": 3 }
```

---

## 9. Host Integration Guide

A host renderer that wants to display Lottie animations needs:

1. **Per-frame tick**: call `terminal_lottie_tick(term, now_us)` before querying
   animations.
2. **Query animations**: `terminal_get_lotties(term, &count)` returns RGBA
   snapshots.
3. **Query placements**: `terminal_get_lottie_placements(term, id, &pl_count)`
   returns cell regions.
4. **GPU texture cache**: keyed by `id`, version-gated (same pattern as sixel).
5. **Compositing**: render background-layer placements after cell content but
   before foreground-layer placements.

The host has zero Lottie-specific knowledge beyond pixel upload and draw. No
ThorVG dependency is required in the host.

### Per-Frame Data Flow

```
1. terminal_lottie_tick(term, now_us)        // advance + re-rasterize
2. terminal_get_lotties(term, &lottie_count) // pull RGBA snapshots
3. For each animation:
     a. Lookup/create GPU texture (keyed by id, version-gated)
     b. If version changed: SDL_UpdateTexture from CfrLottie.rgba
     c. terminal_get_lottie_placements(term, id, &pl_count)
     d. For each placement:
          - Skip if wrong layer
          - Compute screen coordinates from abs_line
          - Frustum cull
          - Set per-placement opacity
          - SDL_RenderTexture
```

---

## 10. JSON Parser

The engine uses a lightweight JSON parser (`lt_json_find_key`) that searches
for keys at the **top level** of a JSON object. It correctly skips nested
objects and arrays (tracking depth) to avoid finding keys inside the `lottie`
sub-object. Only top-level command fields (`cmd`, `id`, `lottie`, `placement`,
`layer`, `opacity`, `play`, `frame`, `speed`, `loop`, `autostart`, `data`,
`seq`, `total`) are extracted.

The full Lottie JSON body is passed verbatim to ThorVG's
`tvg_picture_load_data()` which handles parsing internally.

---

## 11. Alternatives Considered

### Direct pixel streaming (like sixel)

The client could pre-rasterize frames and send them as pixel data.
**Rejected** because:

- Lottie JSON is typically 1–50 KB; pre-rasterized RGBA frames are ~720 KB per
  frame at 4×8 cells HiDPI. A 30-frame animation would be ~21 MB vs ~50 KB.
- JSON enables resolution-independent rendering (crisp at any DPI or zoom).
- Frame composition (Lottie's partial frames) would need client-side work,
  duplicating ThorVG's effort.

### Host-side rasterization

Rasterization in the host with ThorVG linked only there. **Rejected** because:

- Inconsistent with sixel: host must manage ThorVG state coupled to the engine's
  animation lifecycle.
- A Lottie-capable host requires ThorVG as a dependency. With engine-side
  rasterization, any host consuming `CfrLottie.rgba` works with zero
  Lottie-specific code.
- The engine can correctly manage canvas resize, frame timing, and pixel buffer
  reuse without cross-process coordination.

### Host-side JSON parsing

The engine fires a callback with the raw payload and the host handles
everything. **Rejected** because:

- Animation state (playback, timing, loop) is grid-coupled — must scroll with
  text, be culled on scroll-off, and cleared on ED. These are engine
  responsibilities (same rationale as sixel being internal).
- Cell-anchoring and placement management are naturally part of the VT engine's
  coordinate system.

### OSC instead of APC

The original design used OSC 837. **Changed to APC** because:

- ECMA-48 defines APC as "command string for an application program."
- Kitty's graphics protocol uses APC, establishing precedent.
- OSC codes are a flat numeric namespace with no registry — any chosen number
  could conflict.
- APC strings are safely ignored by other terminals (xterm discards them).
