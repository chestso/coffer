# coffer — wrapped-line behavior spec

Status: **normative for the implemented behavior**. Companion to
`docs/wrapped-lines-design.md`, which describes the internal
representation. This document decides the policy knobs that design left
open (`wrapped-lines-design.md` §4) and states the rules the differential
fuzzer (`tests/test_cfr_wrap_fuzz.c`) holds the engine to.

Reference terminal for every open choice: **tmux**, except where a test
already pinned xterm's answer (ICH/IRM). A divergence from the reference
is called out explicitly.

The key words MUST, MUST NOT, SHOULD and MAY are used as in RFC 2119.

## 1. Terms

- **Cell** — one column of a row. `width == 0` marks the continuation of
  a width-2 cluster; `cp == 0 && width == 0` is a cell that was never
  written ("hole"). An erased cell is a space (`cp == 0x20`, `width == 1`).
- **Margin cell** — the last cell of a row (`col == cols - 1`).
- **Wrap edge** — the `CFR_CELL_WRAPLINE` flag on the **margin cell**.
  It means "this row soft-wraps into the row below". A bit anywhere else
  is meaningless and MUST NOT be produced (§5.6).
- **Phantom** — the deferred wrap: the cursor's logical column equals
  `cols`, one past the margin, with no cell at that position. Stored as
  `cursor.col ∈ [0..cols]`; `cols` is the phantom.
- **Lineage** — a per-row `uint32_t` logical-line id; `0` means blank or
  unwritten. Rows of one soft-wrapped line share a nonzero id.
- **Join / continuation** — row _r_ continues row _r-1_.

## 2. The join predicate

A row continues the row above it iff **all** of:

1. the row above's margin cell still carries the wrap edge, and
2. both rows share the same nonzero lineage id.

Implemented once, in `cfr_row_continues()` (`src/lines.c`) and exposed as
`cfr_row_is_continuation()`. Every consumer (selection, reflow,
scrollback joins, public text queries) MUST go through it.

The two ends are independent on purpose: rewriting the margin cell kills
end 1 automatically, and moving a different line under the row kills end
2 automatically. No scroll/IL/DL/erase site clears wrap state by hand.

## 3. Printing

### 3.1 Narrow cluster (width 1)

At the cursor, in this order:

1. If the column is the phantom and DECAWM is on → wrap commit (§4).
   With DECAWM off the phantom cannot exist (§5.4).
2. Clamp the cursor into the grid.
3. If IRM is on, insert one blank at the cursor (ICH, §5.6).
4. Stamp the row's lineage if it is 0 (fresh id).
5. Write the cell; the write sets the cell's flags to 0, so a write to
   the margin cell **severs** the row's edge.
6. If the written cell's old width was 2 (a narrow print over the lead of
   a wide cluster), blank the stranded continuation cell. If that cell is
   the margin, the blank also severs the edge (it rewrites the margin).
7. Advance: landing on the margin leaves the phantom with DECAWM on, or
   parks on the margin (`col == cols - 1`) with DECAWM off.

### 3.2 Wide cluster (width 2)

Same sequence with these rules:

1. Phantom resolution first, as for a narrow cluster.
2. If the cluster no longer fits (`col + 2 > cols`) and DECAWM is on →
   **eager wrap**: the wrap commits `on this print`, and the cluster
   prints at column 0 of the next row. The edge is stamped on the row
   being left, even though the wide cluster itself moved.
3. Writing the continuation cell at the margin clears the edge (step 5
   above applies to every cell write). A wide cluster that exactly fills
   the row therefore leaves the phantom **without** an edge; the next
   print's wrap commit stamps it.
4. DECAWM off: no eager wrap. The lead is written on the margin with no
   continuation (there is nowhere to put one) and the cursor parks on the
   margin. This is a deliberate corner: the grid is not widened, no join
   appears.

### 3.3 Lineage stamping

The first print into a row with lineage 0 assigns a fresh id. Printing
into a row that already has one keeps it, even if previous erases blanked
the row's cells (erases do not touch lineage). A row replaced wholesale
by IL/DL/scroll carries the incoming row's lineage.

### 3.4 Stray joiners across write boundaries

A combining mark, variation selector or ZWJ that reaches a flush with no
base in the pending cluster (a write boundary or a control sequence split
it from its base) re-attaches to the last printed cell **in the same
row**, re-commits the combined cluster, and recomputes its width and any
wrap it now forces. Cross-row re-attachment is a drop: the modifier is
discarded there.

Cluster width follows UAX #11 plus UTS #51 presentation selectors:
VS16 widens to 2; VS15 narrows only where the base has a text-presentation
sequence; the last selector wins.

## 4. Wrap commit

Called only from a print (`commit_cluster` / the ASCII fast path):

1. Give the source row a lineage id if it has none.
2. Set the wrap edge on the source row's margin cell.
3. If the source row is the scroll bottom, scroll the region up one row
   (pushing the top row to scrollback when the region is the full screen);
   the print lands on the newly cleared bottom row.
4. Otherwise move down one row.
5. Set the destination row's lineage to the source row's id.
6. The cursor lands on column 0 of the destination row.

If the cursor is on the bottom row but outside the scroll region, no wrap
is possible; the print path returns without writing. (The fuzzer keeps a
full-screen region, so this branch is not exercised there.)

## 5. Cursor motion, edits and the phantom

### 5.1 Operations that kill the phantom

These resolve `col == cols` to `cols - 1` and then move as specified:

- CR, BS, HT/CHT/CBT (explicit tab stops), CUP/HVP, CHA, VPA, DECOM home
- RI (`ESC M`)
- DECAWM off (`CSI ?7l`)
- EL (all modes), ECH, ICH, DCH — when a grid exists
- image cursor advance and kitty placement cursor advance
- a print that finds the phantom with DECAWM off (step 1 of §3.1 does not
  commit a wrap; step 6 of the write parks on the margin)

### 5.2 Operations that resolve the phantom without a join

LF/VT/FF (and IND) with a pending phantom set the column to 0 and then
move down or scroll. The phantom is **not** a wrap: it never stamped an
edge, so no join appears. A committed wrap elsewhere on the screen is
real content and LF never touches it.

### 5.3 SU/SD, IL/DL and ED 2

SU/SD scroll the screen and leave the cursor — including a pending
phantom — exactly where it is.

IL/DL operate at the cursor row and set the cursor **column to 0**; they
do not convert the phantom into a wrap.

ED 2 clears the cells and lineages of every row but does **not** touch the
cursor, so a pending phantom survives an ED 2. ED 0/ED 1 go through EL
and therefore do kill it.

### 5.4 DECAWM

- DECAWM is on by default.
- DECRST 7 kills the phantom (clamp to the margin).
- DECSET 7 creates no phantom and does not commit a wrap.
- With DECAWM off, printing on the margin overwrites that cell and parks
  the cursor there; no edge is stamped.

### 5.5 DECSC / DECRC

DECSC saves the cursor **logical** column, including the phantom
(`col == cols`), and DECRC restores it. Cursor visibility, blink and shape
are not part of the save; they keep their current values across a DECRC
(matching xterm). The saved-cursor register is per screen buffer.

### 5.6 Erases, inserts and deletes

The representation rule: **a wrap edge is invalidated exactly by the
edits that rewrite the margin cell; a whole-row move carries it.**
Lineage is invalidated only by replacing the row wholesale (an inserted
or scrolled-in blank, or an ED over the whole row).

| Operation                     | Edge                                                    | Lineage                                             |
| ----------------------------- | ------------------------------------------------------- | --------------------------------------------------- |
| Print at any cell             | cleared if the write reaches the margin cell            | stamped if 0                                        |
| Narrow print over wide lead   | the blanked continuation also clears it if at margin    | unchanged                                           |
| EL 0 / EL 2                   | cleared (the range always includes the margin)          | unchanged                                           |
| EL 1                          | cleared iff the cursor is on the margin                 | unchanged                                           |
| ECH                           | cleared iff the erased range reaches the margin         | unchanged                                           |
| ICH / IRM                     | **preserved** (see below)                               | unchanged                                           |
| DCH                           | **cleared** (the tail erase always includes the margin) | unchanged                                           |
| ED 0 / ED 1                   | rows erased by the EL/ED path lose it                   | untouched on the cursor row; 0 on fully erased rows |
| ED 2                          | every row loses it                                      | 0 on every row                                      |
| IL                            | moved rows carry it; inserted blanks have none          | moved rows carry it; blanks 0                       |
| DL                            | moved rows carry it; blanks at the tail have none       | moved rows carry it; blanks 0                       |
| Scroll up (SU / LF at bottom) | moved rows carry it; new bottom rows (holes) have none  | moved rows carry it; new rows 0                     |
| Scroll down (SD / RI at top)  | moved rows carry it; new top rows (blanks) have none    | moved rows carry it; new rows 0                     |

**ICH / IRM policy: preserve.** ICH shifts the row's content right and
re-stamps the margin edge and the row's lineage afterwards, matching
xterm's `ScrnInsertChar`. tmux severs instead; coffer picks xterm, and
`test_cfr_wrap_erase` pins it. IRM runs ICH before each written cell, so
an insert-mode print preserves the edge unless the written cell is itself
the margin.

**DCH: clear the edge everywhere.** The left shift moves the old margin
cell — with its `WRAPLINE` bit — into the row's interior, where the bit is
not an edge. The tail erase severs the row and the bits that rode along
are cleared. Without that, a later ICH can shift the stale bit back onto
the margin and resurrect a join DCH already severed. This is a
representation invariant, not a preference: **only the margin cell may
carry `CFR_CELL_WRAPLINE`.**

IL and DL reset the cursor column to 0; SU and SD do not touch the cursor.

## 6. Text extraction

### 6.1 Row flattening (`cfr_row_text`)

Walk every column of the row at the row's own width:

- `width == 0` cells (holes and wide continuations) contribute nothing;
- `cp == 0` or a space contributes one `' '`;
- any other cell contributes its codepoint(s) as UTF-8, a multi-codepoint
  grapheme whole.

The result is not trimmed.

### 6.2 Logical-line extraction (selection)

`cfr_selection_get_text()` walks the selected unified rows in order:

- a cell with `cp == 0` (hole or continuation) contributes a space and
  advances one column;
- a blank contributes a space and advances one column;
- a printable cell contributes its bytes and advances its width;
- at the end of a row, if the **next** row is a continuation, the rows
  are joined with no separator and no trailing-blank trim;
- otherwise the row's trailing blanks are trimmed and a `'\n'` is
  appended (unless it is the selection's last row).

So soft wraps read back as one unbroken line, hard breaks as separate
lines with trailing blanks gone.

## 7. Decisions (the design doc's §4 knobs)

| Knob                           | Decision                                                                                                                                                                               |
| ------------------------------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| LF column rule                 | phantom → column 0; otherwise the column is unchanged                                                                                                                                  |
| Phantom survives LF?           | no; it resolves without committing a wrap                                                                                                                                              |
| ICH / IRM                      | preserve the edge and lineage (xterm)                                                                                                                                                  |
| Wide char at the margin        | eager wrap with DECAWM; edge on the row being left; no wrap with DECAWM off                                                                                                            |
| DECAWM toggled off mid-phantom | the phantom dies, clamped to the margin                                                                                                                                                |
| DECSC / phantom                | saved and restored natively (logical column)                                                                                                                                           |
| Narrow print over a wide lead  | blank the stranded continuation (severs if that is the margin)                                                                                                                         |
| Clamp resize, height only      | join survives (the margin cell is copied unchanged)                                                                                                                                    |
| Clamp resize, width change     | join severed: the old margin is not the new margin, and copied interior edge bits are cleared                                                                                          |
| DL inside one wrapped line     | **still joins** — the accepted limitation (design §2.3): two fragments of one line meeting after a shift are indistinguishable from a genuine join. Wrong text, never wrong structure. |
| Wrap edges on interior cells   | MUST NOT exist; DCH and a width-changing clamp clear the bits they would otherwise shift inward (§5.6)                                                                                 |

## 8. The differential fuzzer

`tests/test_cfr_wrap_fuzz.c` runs a deterministic random op stream against
a naive model and the engine and compares state after **every** op:

- scrollback depth;
- the join predicate for every unified row (`cfr_row_is_continuation`);
- every row's flattened text (`cfr_row_text`);
- the whole-buffer text extraction (`cfr_selection_get_text` over all
  rows).

The model is an array of rows, each with a width, `wrap` (the margin
edge) and `line` (lineage), plus the main page's dimensions while the alt
screen is active. Cells own their UTF-8 bytes, so the model can
distinguish a plain ASCII byte from a selector cluster or a ZWJ sequence
without reusing the engine's intern tables. It implements §3–§6 directly,
not the engine's page/cell/interning machinery; it relies on the §5.6
invariant that the edge exists only on the margin.

Ops: printable ASCII runs, a wide ideograph, and selector/ZWJ clusters
(VS16, VS15 with and without a text glyph, a combining mark, a ZWJ
sequence); CR/LF/BS/HT/RI; CUP/CHA/VPA; SU/SD, IL/DL, ICH/DCH/ECH;
EL 0–2, ED 0–3; DECAWM, IRM and DECOM toggles; DECSC/DECRC; DECSTBM
(including partial regions and invalid regions); altscreen 1049 and raw
47/1047, including `cfr_resize()` while the alt screen is active; and
host calls `cfr_resize()` / `cfr_set_reflow()`.

The model's reflow resize replays `cfr_reflow()` phase for phase: collect
the logical line straddling the top scrollback edge (bounded by
`CFR_REFLOW_PREFIX_ROWS`), collect the grid, group rows by the join
predicate, trim blank trailing lines not hosting the cursor, rewrap at
the new width, and install — the oldest rows to scrollback, the newest in
the grid. Rows keep the width they were written at, so the text
extraction bounds every walk by the row's own width.

The clamp-only resize replays `cfr_resize_clamp()`: copy the overlapping
top-left cells, leave cells outside the old page as holes, keep lineage
per copied row, and preserve the wrap edge only across a height-only
resize. A width change cannot carry the edge — the old margin cell is not
the new margin — and copied interior edge bits are cleared so a later
clamp back to the old width cannot resurrect a severed join. The scroll
region resets to full and the cursor is clamped absolutely (a phantom
dies).

A resize on the alt screen always clamps the alt page; the main page
keeps its dimensions until the screen is left. On exit the model runs the
normal reflow or clamp path on the main grid at the current size, placed
with the **alt cursor**, before restoring the 1049 register. That mirrors
`cfr_set_altscreen()`: the main grid is caught up before the saved cursor
comes back.

Each run starts with ED 2 so the lazily allocated grid exists; the model
starts from the same erased screen. Scrollback pages written at a
different width are modeled as per-row widths.

Knobs (environment): `COFFER_FUZZ_ITERS` (default 150),
`COFFER_FUZZ_SEED` (base seed), `COFFER_FUZZ_ONLY`
(`small|narrow|wide|evict|none|clamp`), `COFFER_FUZZ_STRICT` (also
compare the exact lineage ids and margin bits — a debugging oracle; ids
are an implementation detail), `COFFER_FUZZ_TRACE=N` (dump both grids
from op N). The `clamp` run starts with `reflow = false`; every run
flips `cfr_set_reflow()` occasionally, so both resize paths interleave.

Not modeled: stray-joiner re-attachment (§3.4). The cluster ops print a
base and its selectors in one write, so `commit_cluster()`'s one-shot
path is what the model mirrors; `test_cfr_wrap_width` covers the
split-write re-attachment path. The extension found three engine bugs:
the ASCII run fast path wrote one cell past the row after a failed wrap
commit and ED 0/1 indexed rows out of bounds when DECRC had restored a
cursor saved before a shrink (both pinned by `test_cfr_wrap_erase`); a
width-changing clamp could move the margin WRAPLINE bit to an interior
cell and later restore a severed join (pinned by
`test_cfr_lines`'s `test_clamp_resize_join_rules`).

## 9. Normative test references

- `test_cfr_wrap_erase` — erase/overwrite severance, ICH preserve, DCH
  sever, LF vs committed wraps.
- `test_cfr_wrap_width` — wide/eager wraps, selectors, split clusters,
  narrow-over-wide-lead.
- `test_cfr_lines` — DL/IL/scroll/scrollback joins, reflow, pushline.
- `test_cfr_wrap_fuzz` — the differential oracle above.
