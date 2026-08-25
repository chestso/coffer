# Selection in Coffer — Design Document

Selection state moves from portty into coffer. Coffer owns the full
selection lifecycle: coordinate tracking, scroll adjustment, draw-clear,
text extraction. Portty becomes a thin client that sets selection
boundaries and renders what coffer reports.

This follows kitty's proven model: selection lives inside the VT/terminal
layer, adjusted inline during scroll operations, cleared when content is
overwritten.

## Coordinate System

**Unified rows** — identical to portty's existing concept:

- `row >= 0` = visible screen row (0 = top)
- `row < 0` = scrollback row (-1 = most recent, -2 = older, ...)

Maps directly to how the renderer already works
(`rend_display_row_to_unified`). No conversion needed.

## New Public Types (`coffer.h`)

```c
typedef enum {
    CFR_SEL_NONE = 0,
    CFR_SEL_CHAR,   // click-drag
    CFR_SEL_WORD,   // double-click
    CFR_SEL_LINE,   // triple-click
} CfrSelectionMode;

typedef struct {
    int row;  // unified: >=0 visible, <0 scrollback
    int col;
} CfrSelectionPoint;

typedef struct {
    bool active;
    CfrSelectionMode mode;
    CfrSelectionPoint anchor;  // original click point
    CfrSelectionPoint start;    // normalized (always <= end)
    CfrSelectionPoint end;     // normalized (always >= end)
} CfrSelection;
```

## New Internal Fields on `CfrTerm`

```c
CfrSelection selection;
char *selection_word_chars;  // configurable word-char set for word mode
```

## New Public API (`coffer.h`)

### Configuration

```c
void cfr_selection_set_word_chars(CfrTerm *vt, const char *chars);
```

### Start / update / extend / clear

```c
void cfr_selection_start(CfrTerm *vt, int row, int col, CfrSelectionMode mode);
void cfr_selection_update(CfrTerm *vt, int row, int col);
void cfr_selection_extend(CfrTerm *vt, int row, int col);
void cfr_selection_clear(CfrTerm *vt);
```

### Query

```c
bool cfr_selection_active(const CfrTerm *vt);
bool cfr_selection_in_cell(const CfrTerm *vt, int unified_row, int col);
const CfrSelection *cfr_selection_get(const CfrTerm *vt);
```

### Text extraction

```c
char *cfr_selection_get_text(const CfrTerm *vt);  // malloc'd, caller frees
```

## New Callback

```c
// In CfrCallbacks:
void (*selection_changed)(bool active, void *user);
```

Fires only when the selection transitions between active and inactive
(or vice versa). Specifically:

- **Fires** on `cfr_selection_start` (inactive → active)
- **Fires** on `cfr_selection_clear` (active → inactive)
- **Does NOT fire** on `cfr_selection_update` (selection is being
  dragged — the host already knows it's active)
- **Does NOT fire** on `cfr_selection_extend` (same reason)
- **Fires** when scroll adjustment clears a spanning-boundary selection
  (the selection was active and is now gone)
- **Fires** when a draw operation clears an intersecting selection
  (content overwrote the selected region — the host needs to know the
  selection is gone, e.g. to resume the PTY)

The host (portty) uses this callback for PTY pause/resume. When
selection becomes active, pause the PTY (screen stability during drag).
When it becomes inactive, resume the PTY. Because draw-induced clears
fire the callback, the PTY resumes immediately when `less` overwrites
the selection — no separate timer or polling needed.

Redundant calls are suppressed: if the selection is already inactive,
a draw-clear does not fire the callback again (same pattern as portty's
existing `terminal_selection_clear` no-op guard).

## Internal: Scroll Adjustment

New internal function called inline from `cfr_scroll_up` and
`cfr_scroll_down`, **after** scrollback push/pop but **before** the
memmove and damage flush:

```c
static void cfr_selection_on_scroll(CfrTerm *vt, bool up, int lines,
                                     int top, int bottom);
```

### Scroll up (content moves up, lines push to scrollback)

For each selection point (start, end, anchor):

1. Determine if the point is above, inside, or below the scroll region
   `[top, bottom]`.
2. **Above the region** (in scrollback, `row < 0` for full-screen
   scroll, or `row < top` for region scroll): shift by `-lines`.
   The content moved deeper into scrollback.
3. **Inside the region** (`top <= row <= bottom`): shift by `-lines`.
   If this pushes `row` below `top`, the point is now in scrollback
   (negative) — correct, the content scrolled off.
4. **Below the region** (`row > bottom`): leave unchanged. Content
   below the scroll region doesn't move.
5. **Boundary check**: if the selection has one end inside the region
   and one outside, the selection spans the boundary. Content at one
   end moved while the other didn't — the selection is now
   incoherent. Clear it.

### Scroll down (content moves down, lines pop from scrollback)

For each selection point:

1. **In scrollback** (`row < 0`): shift by `+lines`. If this brings
   `row >= 0`, the content returned to the visible screen — correct.
2. **Inside the region** (`top <= row <= bottom`): shift by `+lines`.
   If this pushes `row > bottom`, the content was overwritten by the
   new blank line at the bottom of the region — clamp to `bottom`
   and mark for clearing (the selection points at erased content).
3. **Above the region** (`row >= 0 && row < top`): leave unchanged.
4. **Boundary check**: same spanning-clear logic.

If any point is clamped past the scroll region bottom, the selection
references erased content — clear it and fire the callback.

## Internal: Clear on Overwrite

New internal function called from the grid draw path (character output)
and erase operations:

```c
static void cfr_selection_on_draw(CfrTerm *vt, int row);
```

Checks if `row` intersects the active selection's row range (in unified
coordinates). If so, clears the selection and fires the
`selection_changed` callback.

Called from:

- Character draw (the grid's cell-write function) — when any text is
  written to a row that's part of the selection
- Line erase (EL, ED, ECH) — when erased content overlaps the selection
- Line insert/delete (IL, DL) — when shifted content overlaps

This is a per-row check during the actual VT operation, not a
post-processing pass. It replaces portty's `cb_damage` overlap approach
with a precise, targeted check.

## Where It's Called

| Coffer function                 | Selection call                                        | Why                                |
| ------------------------------- | ----------------------------------------------------- | ---------------------------------- |
| `cfr_scroll_up`                 | `cfr_selection_on_scroll(vt, true, lines, top, bot)`  | Adjust coordinates for scroll-up   |
| `cfr_scroll_down`               | `cfr_selection_on_scroll(vt, false, lines, top, bot)` | Adjust coordinates for scroll-down |
| Grid draw (char output)         | `cfr_selection_on_draw(vt, row)`                      | Clear if overwriting selected row  |
| Grid erase (EL/ED/ECH)          | `cfr_selection_on_draw(vt, row)`                      | Clear if erasing selected row      |
| Grid insert/delete line (IL/DL) | `cfr_selection_on_draw(vt, row)`                      | Clear if shifting selected row     |
| `cfr_resize`                    | `cfr_selection_clear(vt)`                             | Clear on resize                    |
| `cfr_set_altscreen` (enter)     | `cfr_selection_clear(vt)`                             | Clear on altscreen enter           |
| `cfr_set_altscreen` (exit)      | `cfr_selection_clear(vt)`                             | Clear on altscreen exit            |

## Word Selection

Coffer owns all selection modes including word and line. It has direct
cell access via the grid and scrollback, making word expansion natural.

Word expansion logic (mirrors portty's existing `expand_word`):

1. Read the cell at `(row, col)`.
2. Classify it: whitespace/empty (0), word char (1), or other (2).
3. Scan left: while the previous cell has the same class, extend
   leftward. Cross soft-wrap boundaries (check `CFR_CELL_WRAPLINE`).
4. Scan right: same, extend rightward.
5. For the union of anchor and cursor word expansions, take the min
   start and max end.

`cfr_selection_set_word_chars` configures the word character set.
Default: `a-zA-Z0-9_-./~`. Non-ASCII (>= 128) is always treated as a
word character.

## Text Extraction

`cfr_selection_get_text` walks from `start` to `end` in unified
coordinates, reading cells via `cfr_get_cell` (visible) and
`cfr_get_scrollback_cell` (scrollback). Strips trailing whitespace at
hard line breaks, joins soft-wrapped lines without newline. Emits
UTF-8 from codepoints, handles multi-codepoint grapheme clusters via
`cfr_cell_get_grapheme`.

This is a direct port of portty's `terminal_selection_get_text`, but
running inside coffer where it has direct, zero-indirection cell access.

## Altscreen Handling

- **Enter altscreen** (`cfr_set_altscreen(true)`): clear selection. The
  alt screen is a fresh buffer — selection from the main screen is
  meaningless.
- **Exit altscreen** (`cfr_set_altscreen(false)`): clear selection. The
  main screen is restored but content may have changed — selection is
  stale.

Both fire the `selection_changed(false, ...)` callback if a selection
was active.

## Portty Changes

### Removed from portty

- `TerminalSelection` struct (`term.h`) — replaced by `CfrSelection`
  in coffer
- `TerminalSelectMode` enum (`term.h`) — replaced by `CfrSelectionMode`
  in coffer
- All `terminal_selection_*` functions (`term.c`) — replaced by
  `cfr_selection_*` calls
- `selection_damaged` / `pushed_rows` (for selection) / `popped_rows`
  tracking (`term_cfr.c`) — no longer needed
- `cb_damage` overlap check (`term_cfr.c`) — replaced by inline
  `cfr_selection_on_draw` in coffer
- `TerminalBackend` selection function pointers (vtable entries) —
  selection is now a coffer API, not a backend vtable method
- `TerminalSelectionChangeFn` callback type — replaced by coffer's
  `selection_changed` callback
- `terminal_selection_adjust_scroll` / `terminal_selection_overlaps_damage`
  (`term.c`) — logic moves to coffer's internal `cfr_selection_on_scroll`
  / `cfr_selection_on_draw`

### Replaced with coffer calls

| Old portty call                                   | New coffer call                             |
| ------------------------------------------------- | ------------------------------------------- |
| `terminal_selection_start(term, r, c, mode)`      | `cfr_selection_start(vt, r, c, mode)`       |
| `terminal_selection_update(term, r, c)`           | `cfr_selection_update(vt, r, c)`            |
| `terminal_selection_extend(term, r, c)`           | `cfr_selection_extend(vt, r, c)`            |
| `terminal_selection_clear(term)`                  | `cfr_selection_clear(vt)`                   |
| `terminal_selection_active(term)`                 | `cfr_selection_active(vt)`                  |
| `terminal_cell_in_selection(term, r, c)`          | `cfr_selection_in_cell(vt, r, c)`           |
| `terminal_selection_get_text(term)`               | `cfr_selection_get_text(vt)`                |
| `terminal_selection_set_word_chars(term, chars)`  | `cfr_selection_set_word_chars(vt, chars)`   |
| `terminal_set_selection_callback(term, cb, data)` | via `cfr_set_callbacks` `selection_changed` |

Portty's `term_cfr.c` exposes the coffer `CfrTerm*` to the rest of
portty via a getter, so callers can invoke `cfr_selection_*` directly.
No vtable indirection needed.

### Renderer (`rend_sdl3.c`)

`render_cell_selection` calls `cfr_selection_in_cell(vt, unified_row,
col)` instead of `terminal_cell_in_selection(term, row, col)`.

### PTY data processing (`rend_sdl3_process_pty_data`)

Simplified — no post-processing for selection:

```c
int rend_sdl3_process_pty_data(...) {
    terminal_consume_pushed_rows(term);  // reset counter
    terminal_process_input(term, data, len);
    // Selection was already adjusted inline by coffer during scroll.
    // Draw-clear was already handled inline by coffer during char output.
    // Nothing selection-related to do here.
    int pushed = terminal_consume_pushed_rows(term);
    if (pushed > 0 && scroll_offset > 0)
        rend_sdl3_scroll(data, term, pushed);
    return pushed;
}
```

No `selection_damaged` check, no `selection_adjust_scroll`, no
`popped_rows`. Coffer handled everything during VT processing.

### PTY pause/resume

Portty registers a `selection_changed` callback with coffer (via
`cfr_set_callbacks`). The callback body is the existing
`portty_app_selection_change`:

```c
static void on_selection_changed(bool active, void *user) {
    PorttyApp *app = user;
    if (active)
        app->backend->pause_pty(app->backend);
    else
        app->backend->resume_pty(app->backend);
}
```

### Keypress during selection

Portty's `portty_app_handle_key` no longer clears selection (that's
coffer's job via draw-clear). It still calls `resume_pty` on keypress
during active selection — the user is interacting with the app, and
the app needs to respond. If the app's output overwrites the selection,
coffer's `cfr_selection_on_draw` clears it inline and fires
`selection_changed(false)`, which resumes the PTY via the callback.

## What About `less -X` Redraws?

When `less -X` scrolls up using reverse-index (`\e[M`), coffer calls
`cfr_scroll_down` which calls `cfr_selection_on_scroll` — selection
coordinates adjust. The selection follows the content. This is the
correct, kitty-like behavior.

If `less -X` instead redraws the screen (cursor-position + text output
without scrolling), each character draw calls
`cfr_selection_on_draw(vt, row)`. If `less` writes to a row that's part
of the selection, the selection is cleared. This is also correct — the
content at that position changed, so the selection is stale.

The distinction is: **scroll adjusts, draw clears**. This is exactly
kitty's model (`index_selection` for scroll,
`clear_intersecting_selections` for draw).

## Testing Strategy

### Coffer tests (`coffer/tests/`)

- `test_selection_basic` — start, update, extend, clear, query
- `test_selection_scroll_up` — selection follows content on scroll-up,
  verified via `cfr_selection_in_cell` at post-scroll coordinates
- `test_selection_scroll_down` — selection follows content on
  reverse-index scroll-down
- `test_selection_scroll_region` — selection inside a DECSTBM scroll
  region adjusts correctly; selection spanning the boundary is cleared
- `test_selection_draw_clears` — drawing a character on a selected row
  clears the selection and fires `selection_changed`
- `test_selection_erase_clears` — EL/ED/ECH on a selected row clears
- `test_selection_word_mode` — word selection expands to word boundaries
- `test_selection_line_mode` — line selection covers full row
- `test_selection_text_extraction` — `cfr_selection_get_text` returns
  correct content, strips trailing whitespace, joins soft-wraps
- `test_selection_altscreen` — entering/exiting altscreen clears
  selection
- `test_selection_callback` — `selection_changed` fires on start, clear,
  draw-clear, and scroll-boundary-clear; does NOT fire on update/extend

### Portty tests (updated)

- `test_selection_scroll` — now an integration test: feed PTY data that
  causes a scroll, verify selection survived via coffer API
- `test_portty_app` — keypress-during-selection (unchanged behavior,
  calls coffer API instead of portty selection API)
- `test_pty_pause` — `selection_changed` callback fires for PTY
  pause/resume
- `test_selection_extend` — shift+click extend (calls coffer API)

## Migration Plan

1. **Add selection to coffer** — types, fields, API, inline scroll
   adjustment, draw-clear, word/line expansion, text extraction,
   altscreen clear, `selection_changed` callback
2. **Add coffer tests** — all selection behavior tested at the VT level
3. **Add `cfr_back_get_cfr_term` to portty bridge** — expose `CfrTerm*`
   so portty code can call coffer API directly
4. **Update portty bridge** — `term_cfr.c` registers
   `selection_changed` callback; portty callers use `cfr_selection_*`
5. **Update renderer** — call `cfr_selection_in_cell` instead of
   `terminal_cell_in_selection`
6. **Update portty_app** — call coffer API directly or via thin wrappers
7. **Remove portty selection code** — `TerminalSelection`, damage
   tracking, pushed/popped rows for selection, overlap check
8. **Remove redundant portty selection tests** — keep integration tests,
   remove unit tests that are now covered by coffer tests
9. **Remove the `consume_selection_damaged` / `consume_popped_rows`
   vtable entries** — no longer needed
