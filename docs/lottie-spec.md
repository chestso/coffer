# Lottie Animation Protocol — coffer Engine Specification

This document specifies the Lottie animation protocol as implemented in
coffer. It covers the terminal escape sequence wire format, the command
vocabulary, the engine-side state machine, the public C API, frame selection,
memory management, and interaction with VT subsystems (scroll, clear, resize).
Vector rasterization belongs to the host. Portty implements it with ThorVG's
software canvas; see the portty documentation for texture upload, compositing
and the linear-light pipeline.

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

**Windows ConPTY fallback** — The bundled OpenConsole passes APC through,
but the system `conhost.exe` fallback strips it. On Windows, use OSC 5555
when the bundle is unavailable:

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

**Units**: every pixel quantity in this protocol — the `width`/`height`
region hints, the rasterization size, and the reported cell figures — is
**device pixels**, the same unit `CSI 16 t` reports. Rasters are drawn 1:1.

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
in-place. The object keeps its handle and placements. The definition and raster
revisions bump, so the host rebuilds the vector scene and produces a fresh
frame.

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
  optional — if any present and different from current, recompute the raster
  size and cell boxes. Playback continues from the current frame.
- `report` (bool, default false) — if true, emit an APC report after placement (see §2.8).

**Deduplication**: placements are keyed by `(abs_line, col)`. Sending `place`
with the same position as an existing placement **updates** that placement's
`layer` and `opacity` (and size if constraints changed) rather than creating a
duplicate. This enables efficient toggling (e.g. foreground ↔ background) at
the same position.

Each placement is auto-assigned a stable `id` by the shared graphics store.

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
- If the frame changes, `raster_rev` bumps and the host re-rasterizes it.

### 2.6 `delete` — Remove an animation

```json
{ "cmd": "delete", "id": 1 }
```

Removes the animation and all its placements. The JSON and placement state are
released. The host drops its vector scene and texture on its next
reconciliation pass.

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
- A `load-chunk` with `seq == total - 1` finishes the document, computes its
  layout and adds a default placement. The host builds and rasterizes the
  vector scene from the resulting definition.
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

| Field       | Type | Description                                        |
| ----------- | ---- | -------------------------------------------------- |
| `type`      | str  | Always `"report"`                                  |
| `id`        | int  | Animation id                                       |
| `row`       | int  | Placement row (top-left of region)                 |
| `col`       | int  | Placement col (top-left of region)                 |
| `rows`      | int  | Cell rows occupied (engine-computed)               |
| `cols`      | int  | Cell cols occupied (engine-computed)               |
| `raster_w`  | int  | Rasterization width in device px (design × scale)  |
| `raster_h`  | int  | Rasterization height in device px (design × scale) |
| `cell_w_px` | int  | Terminal cell width in device px                   |
| `cell_h_px` | int  | Terminal cell height in device px                  |

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
        │  load /  │        │  play/pause/ │       │  place /  │
        │  load-   │        │  stop/seek   │       │  delete   │
        │  chunk   │        │              │       │           │
        └────┬─────┘        └──────┬───────┘       └─────┬─────┘
             │                     │                     │
             ▼                     ▼                     ▼
  ┌───────────────────┐   ┌──────────────────┐   ┌─────────────────┐
  │ Retain the Lottie │   │ Update playback  │   │ Update or remove│
  │ JSON and compute  │   │ state and the    │   │ placements and  │
  │ device-pixel size │   │ requested frame  │   │ their grid life │
  └───────────────────┘   └──────────────────┘   └─────────────────┘
```

coffer does not parse the vector drawing commands or produce pixels. The host
receives the JSON, the requested frame and the exact raster size.

### 3.2 Parser Integration

The parser has a dedicated `CFR_STATE_APC_STRING` state. On `ESC _`, the parser
enters this state and accumulates bytes into `apc_buf[]`. On ST (`ESC \`) or
BEL, it dispatches:

```c
case CFR_STATE_APC_STRING:
    cfr_lottie_apc_dispatch(vt, p->apc_buf, p->apc_len);
    break;
```

OSC 5555 routes to the same dispatch function:

```c
case 5555:
    cfr_lottie_apc_dispatch(vt, body, body_len);
    break;
```

### 3.3 Internal Data Structures

`CfrImg` in the shared graphics store owns the Lottie record's public id,
unique handle, screen, placements, lifetime policy and `raster_rev`. Its
`source_state` points to the protocol-private record:

```c
typedef struct
{
    uint32_t definition_rev;
    uint8_t *json;
    size_t json_len;

    int design_w, design_h;
    int px_w, px_h;

    int p_width, p_height;
    int p_cols, p_rows;
    double explicit_scale;
    int fit;

    int current_frame;
    int frame_ip;
    int frame_op;
    double frame_fr;
    double speed;
    bool playing;
    bool loop;

    uint64_t anchor_us;
    double pos0;
    double last_pos;
    bool anchored;
} LtRec;
```

The store's source destructor releases this record with the graphics object.
There is no parallel animation array to reconcile after scroll, erase, screen
switches, eviction or terminal teardown.

`CfrLottieState` contains only chunked-upload accumulators and the public query
scratch array. The state is allocated on the first Lottie APC.

---

## 4. Public API

All public types and functions are declared in `<coffer/coffer.h>`.

### 4.1 `CfrLottie`

```c
typedef struct
{
    uint64_t handle;         /* stable, unique, never reused */
    uint64_t id;             /* client protocol id */
    uint32_t definition_rev; /* JSON changed: rebuild the vector scene */
    uint32_t raster_rev;     /* frame/size/definition changed: re-raster */
    const uint8_t *json;     /* complete Lottie document */
    size_t json_len;
    int px_w, px_h;          /* exact raster size in device pixels */
    int current_frame;
    int frame_count;
    bool playing;
    double speed;
    bool loop;
    int placement_count;
} CfrLottie;
```

`definition_rev` and `raster_rev` are nonzero. A placement movement, opacity
change, play or pause does not by itself bump `raster_rev`. A seek, stop that
changes the frame, frame advance, document replacement and raster-size change
do.

`json` and the snapshot are borrowed from the engine and remain valid until
the next engine mutation or query. A host that retains JSON outside its normal
synchronous rasterization pass must copy it.

### 4.2 Queries

```c
const CfrLottie *cfr_get_lotties(CfrTerm *vt, int *out_count);
const CfrImagePlacement *cfr_get_image_placements(CfrTerm *vt,
                                                  int *out_count);
```

The first query returns Lottie animations on the active screen. The second
returns all active-screen 1:N placements. Match `CfrImagePlacement.source ==
IMG_SRC_LOTTIE` and `image_handle == CfrLottie.handle`.

Lottie objects are not returned by `cfr_get_images()`: that query is for
engine-owned raster images. Lottie pixels are host-owned.

### 4.3 Frame Advancement

```c
bool cfr_lottie_present(CfrTerm *vt, uint64_t present_us, uint64_t hold_us);
int cfr_lottie_playing_count(CfrTerm *vt);
int cfr_lottie_active_count(CfrTerm *vt);
```

`cfr_lottie_present()` evaluates every playing animation's frame, bumps its
`raster_rev` and damages its placements when the frame changes. It performs no
allocation or rasterization and can therefore run in the host's presentation
path without making vector rendering part of VT input processing.

The frame is a pure function of time (§5). `cfr_lottie_playing_count()` drives
the host's adaptive frame-pacing decision; `cfr_lottie_active_count()` also
counts paused animations.

### 4.4 Configuration

```c
void cfr_set_cell_pixels(CfrTerm *vt, int cell_w_px, int cell_h_px);
```

Cell pixel changes recompute every animation's raster size and placement cell
box. If the device-pixel size changes, `raster_rev` bumps. The host must
re-rasterize rather than stretch an existing bitmap.

### 4.5 Division of Responsibility

| Concern                                             | Owner  |
| --------------------------------------------------- | ------ |
| APC / OSC 5555 transport                            | coffer |
| Command parsing and chunked upload                  | coffer |
| JSON metadata, fit and placement geometry           | coffer |
| Scroll, erase, resize and alternate-screen lifetime | coffer |
| Playback clock and frame selection                  | coffer |
| Vector scene and rasterization                      | host   |
| Pixel-format conversion and texture cache           | host   |
| Layer compositing and presentation                  | host   |

---

## 5. Frame Selection and Host Rasterization

### 5.1 Sampling Algorithm

Each playing `LtRec` keeps an anchor: `anchor_us` (the `present_us` of the
first call after it started playing, resumed, seeked or changed speed) and
`pos0` (its position in frames from `ip` at that moment). The sampler is a
closed form, with no accumulated frame error:

```
t   = present_us + hold_us / 2
pos = pos0 + (t - anchor_us) × speed × fr / 1_000_000
span = op - ip
if pos >= span:
    if loop:  pos = fmod(pos, span)
    else:     pos = span; playing = false
new_frame = ip + min(floor(pos), span - 1)
```

`last_pos` preserves the fractional frame when speed or loop changes on a
running animation. `pause`, `stop` and `seek` anchor at the frame currently
shown. `raster_rev` changes only when the selected frame changes.

Sampling at `present_us + hold_us / 2` avoids a rounding decision exactly on a
refresh boundary: 30 fps content on a 60 Hz display has every frame boundary
coincide with a refresh, and the half-period offset gives it margin against
jitter.

### 5.2 Raster Contract

The host rasterizes `json` at exactly `px_w × px_h` device pixels and
`current_frame`. A typical implementation uses ThorVG's software canvas:

1. Rebuild its animation/picture when `definition_rev` changes.
2. Resize the vector picture and output target when `px_w`/`px_h` change.
3. Select and draw the frame when `raster_rev` changes.
4. Upload or composite the resulting pixels 1:1.

The engine imposes no GPU requirement. The host can use CPU rasterization on
every platform and still use a GPU for texture storage and scene composition.

### 5.3 Pixel Format

The host owns its pixel contract. Portty's rasterizer produces straight-alpha,
sRGB-encoded RGBA8 and applies exactly one sRGB→linear conversion when its
linear-light compositor is active. Per-placement opacity is applied during
composition, not baked into the raster.

### 5.4 Device-Pixel Geometry

The raster is drawn at its native `px_w × px_h` size. No additional content
scale is applied. The renderer centers that rectangle within the placement's
cell box. A cell-metric change causes re-layout and, where necessary, a fresh
vector raster at the new size.

---

## 6. Memory Management

| Resource                     | Owner  | Lifetime                              |
| ---------------------------- | ------ | ------------------------------------- |
| Raw Lottie JSON              | coffer | load → delete                         |
| Placement and playback state | coffer | load → delete                         |
| Vector scene and canvas      | host   | definition revision / object lifetime |
| RGBA frame buffer            | host   | reusable while its size is unchanged  |
| Texture cache                | host   | keyed by `(handle, raster_rev)`       |

coffer's Lottie path does not allocate per presented frame. The host's current
frame buffer and upload staging buffer can likewise be retained and grown only
when dimensions change. A host should cap total raster bytes and handle invalid
or oversized documents without displaying stale pixels under a new revision.

---

## 7. Interaction with VT Subsystems

### 7.1 Cell Ownership

Lottie animations do not replace or modify cells. The cells beneath an
animation retain their text, colors and SGR attributes. The animation is a
supplemental visual layer composited by the host renderer.

### 7.2 Scroll Behavior

Placements are anchored by absolute line and scroll with text. The shared
graphics store culls placements that move beyond scrollback. A Lottie object
that has had a placement dies with its last placement. The public placement
query returns display-relative rows; the host adds its scrollback offset as it
does for cells.

### 7.3 Clear Behavior

A text erase removes foreground Lottie placements in the cleared rows and
preserves background placements. Store release damages the affected rows so
the host repaints them.

### 7.4 Resize and Cell Metrics

A grid resize clears Lottie animations because reflow cannot preserve their
text anchors. A cell-pixel change without a grid resize preserves them and
recomputes raster and placement geometry from the requested region.

### 7.5 Alternate Screen

Lottie objects are screen-scoped. Entering or leaving the alternate screen
clears the alternate screen's set; main-screen objects survive and become
visible again on return. Queries report only the active screen.

### 7.6 Selection and Cursor

The host places background graphics behind the cell pass and foreground
graphics above it. A foreground animation may obscure text or the cursor, as
other foreground graphics do.

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

A host renderer performs these steps once per presented frame:

```
1. cfr_lottie_present(vt, present_us, hold_us)
2. anims = cfr_get_lotties(vt, &anim_count)
3. Reconcile host scenes with the animation handles
4. For each animation:
     a. If definition_rev changed, load json into the vector renderer
     b. If px_w/px_h changed, resize the vector picture and raster target
     c. If raster_rev changed, rasterize current_frame
     d. Upload the frame to the texture cache if raster_rev changed
5. placements = cfr_get_image_placements(vt, &placement_count)
6. For each Lottie placement:
     - Match its animation by image_handle
     - Select background or foreground pass
     - Map the display row through the host's scrollback offset
     - Cull boxes outside the viewport
     - Apply per-placement opacity
     - Draw the raster 1:1, centered in the cell box
```

A headless host can ignore rasterization entirely while retaining full VT and
protocol behavior. A graphical host chooses its own CPU or GPU vector stack;
coffer has no vector-renderer dependency.

## 10. JSON Metadata Reader

The engine uses a lightweight reader (`lt_json_find_key`) that searches for
keys at the top level of a JSON object and skips nested objects and arrays.
It extracts only command fields and the Lottie document's `w`, `h`, `ip`, `op`
and `fr` metadata needed for layout and playback.

The complete Lottie document is retained verbatim for the host. coffer does
not implement the Lottie vector object model.
