# coffer — wrapped-line representation design

Status: **implemented** (steps 1–5; see §7 for the FOLLOWUPS items the
later behavior spec owns). Written down so work could resume. Reference
for behavior questions during implementation: tmux only. Pre-alpha: no
backwards compatibility, architecture over expedience. Scope is the
_internal representation_ only — no behavior fixes ride along; behavior
fixes are a separate later step with their own spec and tests.

Three terms recur throughout; their meaning in this document:

- **Phantom** — the deferred wrap. When a print fills the final
  column, the cursor logically sits one column _past_ the right
  margin, in a position that has no cell; only the next print turns
  it into an actual wrap. Pure cursor state (`pending_wrap` today,
  logical column `cols` in §2.1) — it never touches the grid until
  that print.
- **Lineage** — a per-row id (`0` = blank/unwritten) naming the
  logical line a row's content is a fragment of. Stamped when a
  print first writes a fresh row, propagated r→r+1 when a wrap is
  committed, carried unchanged by whole-row moves. Continuation is
  the two-sided handshake: wrap edge on the margin cell of the row
  above _and_ equal nonzero lineage on both rows.
- **Policy** — a behavioral choice the representation supports but
  does not decide (§4): the LF column rule, ICH/IRM severance,
  whether the phantom survives LF, DECAWM toggled mid-phantom. Data,
  not architecture — a wrong choice can over- or under-join by one
  row, never corrupt structure. Decided by the later behavior spec,
  with tmux as the reference.

## 1. The problem

The current representation (as of `09674e2`) is two pieces of state:

- `vt->cursor.pending_wrap` — a bool encoding the deferred-wrap
  phantom _positionally_: it only means anything while
  `cursor.col == cols - 1`.
- `page->row_flags[r] & CFR_CELL_WRAPLINE` — a soft-wrap edge stored
  on the **row index** rather than on content.

Consequences (diagnosed, deliberately _not_ fixed by this work):

1. The phantom has no slot for "wrap pending at column 0". This is
   the FOLLOWUPS "Line feed at the bottom row while a deferred wrap is
   pending" entry. The `09674e2` fix works around it by clearing the
   WRAPLINE flag inside `linefeed()`.
2. Row-index-coupled edges survive content replacement: probes
   confirmed false joins after DL/IL under a live edge, RI carrying a
   phantom across a scroll, and SU shifting rows under an edge.
   `09674e2` hand-patched five sites (EL modes reaching the margin,
   ECH, DCH, ED0, LF-resolves-phantom); the same bug class exists at
   the unpatched sites.
3. The LF clear is conflated: LF with a pending phantom also severs a
   _genuine committed_ wrap (21 chars wrapping row 0 into row 1, then
   `CUP(1;20)` + print + LF kills the 0→1 join). Verified: removing
   the `clear_wrapline` call from `linefeed()` leaves
   `test_cfr_wrap_erase` passing — the call is unnecessary for its own
   test and harmful in this case.

Notably `include/coffer/coffer.h` already documents
`CFR_CELL_WRAPLINE` as a **cell** flag ("set on the last cell of a
soft-wrapped logical row"). The header's model is right; `print.c`
parks it on rows instead.

## 2. The design

Three primitives, one predicate. Representation only — this changes
_how_ wrap state is stored, not _when_ it changes.

### 2.1 Phantom = logical column (future step 3)

`cursor.col ∈ [0..cols]`; the value `cols` _is_ the phantom
("logically past the margin"). The `pending_wrap` bool is deleted.

- One normalizer, e.g. `cfr_cursor_set(vt, row, col)`, used by every
  explicit cursor move (CUP/CUU/CUD/CUF/CUB/CHA/VPA/DECOM home/HT/BS/
  CR): clamps to `[0, cols-1]`, i.e. the phantom dies at exactly the
  operations where it dies today. The ~25 scattered
  `pending_wrap = false` assignments collapse into this one function.
- `cfr_get_cursor()` (public, renderer-facing) reports the clamped
  physical column. DECSC/DECRC save/restore the logical column
  natively (xterm saves `do_wrap` in its saved cursor; same effect for
  free).
- Only print resolves `col == cols` into a wrap — wraps are created
  only inside `commit_cluster()`, never elsewhere.
- LF policy (tmux): resolve the phantom, move down/scroll, column per
  the DEC LF rule. If the later behavior spec wants another LF policy,
  it is one function body; the representation supports either.

Wide (2-cell) chars fit the model unchanged; three cases, all
matching today's behavior:

- **Phantom created by a wide char** — printed at `cols-2`, it
  occupies `cols-2` and `cols-1`; the cursor's logical column
  becomes `cols`, the phantom. `cfr_get_cursor()` clamps to `cols-1`
  — the continuation cell, the same physical position reported today
  (`pending_wrap` + `col == cols-1`). DECSC/DECRC save the phantom
  natively here too.
- **Wide char resolving a pending phantom** — the print wraps to
  column 0 of the next row first, the char lands there. Same as
  today.
- **Wide char arriving at `cols-1`** — it cannot fit (needs 2 cells,
  1 remains), so this wrap is **eager**: `commit_cluster()` moves to
  the next row _before_ writing, and the char prints at column 0.
  This is not a phantom — no cursor column can mean "half past the
  margin", and the char must land somewhere this print, not the next
  one (tmux and xterm wrap it immediately too). It is the "wide-char
  wrap branch" §2.2 names: a second branch inside the single place
  wraps are created, not a phantom resolution. Where its edge bit
  lands is a §4 knob, already listed.

After this the phantom never coexists ambiguously with anything, and
it never touches the grid until a print resolves it.

### 2.2 Wrap edge = bit on the margin cell (step 2)

`CFR_CELL_WRAPLINE` moves from `row_flags[]` to
`cell(r, cols-1).flags`, set at the single wrap-resolution point in
`commit_cluster()` (and the wide-char wrap branch). Cells are values
that travel with `memmove` and are rewritten by every erase/overwrite:

| Operation                            | Effect in new model                                |
| ------------------------------------ | -------------------------------------------------- |
| EL reaching margin, ECH/DCH covering | margin cell rewritten → bit gone, automatic        |
| ED0/ED2 over the row                 | same, automatic                                    |
| Print overwriting margin             | same, automatic                                    |
| LF resolving a phantom               | no code at all — the phantom never committed a bit |
| Scroll / DL / IL moving rows         | bit travels inside the cells                       |
| ICH/IRM shifting the row             | severed by the shift — _policy knob_, see §4       |

The `09674e2` `clear_wrapline()` helper and its five call sites
become dead code and are deleted.

### 2.3 Row lineage id (step 1, then completed in step 4)

A `uint32_t lineage[rows]` per page (grid, altgrid, scrollback pages —
the existing `row_flags` array widened; every `memmove` site already
exists). Semantics:

- `0` = blank/unwritten.
- Assigned when a print first writes into a fresh row; stamped on the
  row.
- **Propagated** r→r+1 when a print resolves a wrap (handshake
  origin).
- Travels unchanged with whole-row moves (scroll, IL, DL, SD, SU).
- A row replaced wholesale (DL/IL shifting a foreign line in) carries
  its _own_ lineage — the edge on the row above stops matching, with
  no explicit clear-call anywhere.

**Why both the flag and the id.** A join has two ends, and an edit
can destroy one end without ever touching the other. One small
example shows why neither half suffices on its own. Screen is 4
columns wide. Type twelve `A`s, then LF and type `BBBB`:

```
row 0:  AAAA   wrap flag   lineage 7
row 1:  AAAA   wrap flag   lineage 7
row 2:  AAAA   no flag     lineage 7
row 3:  BBBB   no flag     lineage 8
```

(Row 2 has no flag: the twelfth `A` filled it exactly and LF started
a new line — a wrap is only recorded when a print actually spills
over.)

**Flag alone fails when the row below changes.** DL at row 2 deletes
row 2; `BBBB` slides up into its place. Row 1 was never touched, so
its flag survives. A flag-only predicate now reports that row 2
(`BBBB`) continues row 1 — extraction glues two different lines into
`AAAABBBB`. This is the §1.2 bug class verbatim (the DL false join;
`09674e2` hand-patched five sites of it, unpatched sites still
wrong). Lineage catches it: row 1 says 7, the slid-up row says 8 —
no match, severed.

**Lineage alone fails when the row above changes.** From the
original setup, move to row 0, column 2, and EL (erase to end of
line). Row 0 becomes `AA` + blanks. The margin cell was rewritten, so
the flag dies automatically (§2.2) — but row 0 still has content, so
its lineage stays 7, and row 1 below still says 7. A lineage-only
predicate joins them: the truncated `AA` glued to the spillover
`AAAA` as one line, when the app clearly ended the first. The flag
catches it: gone, severed.

So the two halves each guard one end:

- **flag** — "this row ends in a wrap": invalidated by any rewrite of
  the row's own margin cell (§2.2's table, automatic).
- **lineage** — "the row below is still the same logical line":
  invalidated when a different line's fragment (its own id) or a
  blank (`0`) takes that row.

A join is reported only while both ends say yes — and each end is
invalidated by exactly the edits that touch it, which is the whole
trick: no IL/DL/scroll/erase site needs to know wrap state exists.
Wrap state gains one writer (the print wrap commit, plus reflow's
re-wrap) and one reader (the `lines.c` predicate). Ids rather than
neighbor-pointers because a pointer must be rewritten by every
whole-row move — O(rows) per scroll, §5 — while an id travels free
inside the same `memmove`s, including across the scrollback boundary
(§3 step 5).

**Known imperfection: DL inside a wrapped line.** Lineage carries no
ordering: fragment 1 and fragment 3 of line 7 share the same id. So
when a shift brings two non-adjacent fragments of one line together,
nothing can tell. Same setup, DL at row 1 — the middle fragment is
deleted, rows below slide up:

```
before:                      after:
row 0:  AAAA  flag  lin 7    row 0:  AAAA  flag  lin 7   untouched
row 1:  AAAA  flag  lin 7    row 1:  AAAA  -     lin 7   was row 2
row 2:  AAAA  -     lin 7    row 2:  BBBB  -     lin 8   was row 3
row 3:  BBBB  -     lin 8    row 3:  ____  -     lin 0   new blank
```

The predicate at the 0→1 boundary: flag present, lineage 7 == 7 →
join. Extraction returns `AAAAAAAA` — the deleted middle silently
stitched shut. The defensible answer is two lines: `AAAA` /
`AAAA`.

Compare the two DL cases. When a _different_ line slid under the flag
(DL at row 2), lineage caught it. When the _same_ line's later
fragment slid under (DL at row 1), nothing can — to the data it
looks identical to a legitimate join. That is the whole flaw.

**Why accept a design with a known wrong answer?**

- **No spec defines the right answer.** Terminal specs say what DL
  does to rows; none says what text extraction owes afterwards.
  Join-or-sever here is a judgment call — policy (§4), not an
  architecture fact. The rejected row-flag design (§5) answers
  "join" too: its flag on row 0 also survives the shift.
- **Every cheap design has this flaw or worse.** The status quo has
  it _plus_ all the cross-line false joins of §1.2 — those are now
  structurally impossible, because a foreign fragment carries a
  different id and a blank carries `0`, neither ever matches. The
  remaining error is confined to two genuine fragments of one line
  meeting at one boundary: wrong text, never wrong structure.
- **Closing it reinstates the machinery this design exists to
  delete.** Telling "next fragment" from "later fragment" needs
  sequence position, and shifts destroy position. The candidate
  fixes are: renumber on every scroll/DL/IL (the O(rows) pointer
  bookkeeping §5 rejects), or hand-clear flags at every shift site
  (the `09674e2` five-patch maintenance surface again).
- **The escape hatch is cheap.** If the behavior spec decides DL
  inside a wrapped line should sever, the patch is one rule at one
  site: when DL deletes row _r_, clear the wrap flag on row _r-1_ —
  the join it described ended with the deleted row, by definition.
  (IL needs no rule even then: the inserted blank arrives with
  lineage 0 and lineage severs on its own.) Local, uses the
  design's own data, no redesign. That is why it is recorded here
  rather than resolved.

### 2.4 The predicate — single source of truth (new `src/lines.c`)

```c
/* True when `row` (unified coords) continues the logical line above it. */
bool cfr_row_continues(const CfrTerm *vt, int unified_row)
{
    return (margin_cell(prev).flags & CFR_CELL_WRAPLINE) != 0
        && lineage(prev) != 0
        && lineage(prev) == lineage(this_row);
}
```

Two-sided handshake (why: §2.3): both sides invalidated by content
rewrite, zero coupling to row indices. Every consumer funnels through
it: selection
word-scan (`selection.c`), `cfr_selection_get_text`, reflow
(`reflow.c`), scrollback joins, and the public query API (plus a small
`cfr_logical_line_bounds()` helper for word-scan/reflow walks).
`cfr_get_line_continuation` and `cfr_row_is_continuation` collapse
into one predicate; the direction-inverted twin and its header caveat
are deleted.

## 3. Migration (each step keeps `make check` green)

1. **Lines skeleton + lineage plumbing.** Add `src/lines.c` (empty
   predicate reading the old state), `src/Makefile.am` entry. Widen
   `row_flags` → `lineage` array through the existing memmove sites
   (`grid.c` allocation, `print.c` scroll/IL/DL, `modes.c` memsets,
   `reflow.c`, `scrollback.c`). No behavior change.
2. **Margin-cell bit + predicate flip.** Move the overflow bit to
   margin cells; flip the predicate; **delete** `clear_wrapline()` and
   the `linefeed()` special case. `test_cfr_wrap_erase.c` must stay
   green — it is the behavioral contract the representation now
   preserves structurally.
3. **Logical cursor column.** Delete scattered `pending_wrap` resets.
   New tests: LF preserves a committed wrap (today's conflation bug,
   §1.3), DECSC/DECRC round-trips the phantom.
4. **Lineage stamping.** Print/erase/scroll/IL/DL/ED stamp lineage;
   predicate adds the lineage handshake. New tests: probes A/B/C/D/E
   (RI-phantom, DL/IL false joins, SU under edge, phantom + SU) as
   regressions.
5. **Consumer unification.** Selection, get_text, reflow, scrollback
   join, public API collapse; `sb_pushline` callback gains lineage
   (pre-alpha break, fine). Reflow must propagate lineage across its
   sb/grid split — ids must match across that boundary; call this out.

## 4. Policy knobs (spec + tests later; tmux is the reference)

These are _data_ now, not architecture — a wrong choice can only over-
or under-join by one row, never corrupt structure:

- ICH/IRM: re-establish bit+lineage after a shift, or accept
  severance (tmux default today: sever).
- Wide-char at margin: bit lands on the wrapping row's margin cell
  even when the wide cluster itself moved to the next row.
- LF column rule; whether the phantom survives LF (tmux: no).
- DECAWM toggled off mid-phantom; DECSC/phantom interaction.

## 5. Rejected alternatives (do not relitigate)

- **Logical lines as storage** (line objects, view = projection):
  breaks O(1) random-access cell writes — the terminal contract;
  scroll/resize re-lining becomes O(screen).
- **Derive continuation from content**: impossible in principle —
  "full row + LF" and "full row + wrap" are content-identical.
  Provenance must be recorded.
- **Backward pointer** ("r+1 stores the row it continues"): every
  scroll/DL/IL must rewrite pointers O(rows); symmetric lineage ids
  travel free under memmove.
- **Row-level flag alone** (status quo / xterm / tmux): exactly the
  maintenance surface `09674e2` had to patch, with unpatched sites
  still wrong.

## 6. Implementation inventory (gathered, verified against HEAD)

- `grid.c`: page = one allocation; `row_flags` is trailing bytes →
  becomes the `lineage` array (4-byte rows, aligned).
- `row_flags` touch sites: `print.c` (scroll ×2, IL/DL, ED ×3,
  WRAPLINE set/clear ×5), `modes.c` (altscreen memset, RIS memset),
  `reflow.c` (collect + redistribute), `scrollback.c` (push stores the
  `wrapline` bool → gains lineage), `term.c` (public queries), header
  comment in `coffer_internal.h:133`.
- `pending_wrap` touch sites: `print.c` (linefeed, CR, BS, HT,
  commit_cluster ×2, EL, ECH, IL, DL), `csi.c` (cursor_to,
  cursor_to_origin, DECOM, DECAWM off), `esc.c` (via struct copy —
  DECSC saves, DECRC restores), `modes.c` (altscreen home, RIS),
  `image_store.c` (×2, image cursor advance), `graphics.c` (kitty
  placement), `reflow.c` (×2).
- `src/Makefile.am` needs `lines.c`.
- Tests pinning current behavior (gate for steps 1–3):
  `test_cfr_wrap_erase.c`, `test_cfr_selection.c` (soft-wrap text +
  word select), `test_cfr_parser.c` (reflow grow/shrink, `test_wrap`).
- The two public predicates (`cfr_get_line_continuation`,
  `cfr_row_is_continuation`) have no users outside tests and contrib —
  collapsing them (step 5) breaks nothing real.
- Reference for LF behavior when needed: tmux resolves the phantom on
  LF (moves down/scrolls, column 0); libvterm keeps the deferred wrap.
  xterm's `do_wrap`/`ResetWrap` model is the origin of the
  normalizer/pattern in §2.1. No further cross-terminal validation.

## 7. Follow-up actions

- Close the FOLLOWUPS "Line feed at the bottom row …" entry when the
  logical cursor column lands (representation solves it; the LF clear
  goes away).
- Open FOLLOWUPS items: wrapped-line behavioral spec + differential
  fuzz (random op stream vs. a naive model; assert joins and text
  extraction); the §4 policy knobs.
