# Design: Ambiguous-Width (2-Cell) Rendering — Coffer Side

## Executive Summary

The plumbing for ambiguous-width rendering is fully in place (`CfrConfig.ambiguous_wide`, `CfrTerm.ambiguous_wide`, `cfr_set_ambiguous_wide()`). Two things are missing: the `AMBIGUOUS[]` range table in `width.c`, and the check in `cfr_codepoint_width()`. The existing `scripts/gen_unicode_tables.py` already knows how to generate this table from `EastAsianWidth.txt`, but no UCD data is vendored and the script has never been run.

## Current State

| Component                     | Location                            | Status                                                             |
| ----------------------------- | ----------------------------------- | ------------------------------------------------------------------ |
| `CfrConfig.ambiguous_wide`    | `include/coffer/coffer.h:336`       | Exists, defaults to `false`                                        |
| `CfrTerm.ambiguous_wide`      | `src/coffer_internal.h:271`         | Exists                                                             |
| Config copy to `vt`           | `src/term.c:84`                     | `vt->ambiguous_wide = cfg->ambiguous_wide`                         |
| `cfr_set_ambiguous_wide()`    | `src/term.c:175`                    | Implemented                                                        |
| `AMBIGUOUS[]` table           | `src/width.c`                       | **Missing** (header comment acknowledges this)                     |
| `cfr_codepoint_width()` check | `src/width.c:510-513`               | **TODO stub** — discards `vt` with `(void)vt`                      |
| `cfr_utf8_display_width()`    | `src/width.c:617`                   | Passes `NULL` to `cfr_codepoint_width()` — ambiguous always narrow |
| `gen_unicode_tables.py`       | `src/scripts/gen_unicode_tables.py` | Supports `EastAsianWidth.txt` class `A` but has never been run     |

## Implementation Steps

### Step 1 — Generate `AMBIGUOUS[]` table using `gen_unicode_tables.py`

Download UCD files from `https://www.unicode.org/Public/UCD/latest/ucd/`:

- `EastAsianWidth.txt`
- `auxiliary/GraphemeBreakProperty.txt`
- `emoji/emoji-data.txt`
- `DerivedCoreProperties.txt`

The script downloads these automatically when no UCD directory is provided:

```sh
python3 src/scripts/gen_unicode_tables.py > /tmp/unicode_tables.c
```

Or with a local UCD directory:

```sh
python3 src/scripts/gen_unicode_tables.py /path/to/UCD > /tmp/unicode_tables.c
```

Extract the `CFR_AMBIGUOUS` table from the output and commit it in `width.c` as `AMBIGUOUS[]` (adapting the formatting to match the existing `WIDE[]` / `ZERO[]` style — spaces inside braces, no `CFR_` prefix, no `_LEN` macro since `width.c` uses `ARRAY_LEN()`).

Place the table after `ZERO[]` and before `range_lookup()` (around line 474). Format:

```c
static const Range AMBIGUOUS[] = {
    { 0x00A1, 0x00A1 },
    { 0x00A4, 0x00A4 },
    /* ... full UAX #11 Ambiguous set (179 ranges) ... */
};
```

Update the header comment (lines 12-15) to remove the "not yet enumerated" note.

Add a regeneration comment above the table documenting the source and process.

### Step 2 — Wire `vt->ambiguous_wide` into `cfr_codepoint_width()`

**File**: `src/width.c:510-513`

Replace the current TODO stub:

```c
    /* Ambiguous: TODO once UCD-derived table is generated. With the
     * default (not-wide), all unclassified codepoints fall through as 1. */
    (void)vt;
    return 1;
```

with:

```c
    if (vt && vt->ambiguous_wide && range_lookup(AMBIGUOUS, ARRAY_LEN(AMBIGUOUS), cp))
        return 2;
    return 1;
```

The `vt` parameter (previously discarded via `(void)vt`) is now consulted. This is the only behavioral change. `cfr_cluster_width()` and `cfr_utf8_display_width()` both call `cfr_codepoint_width()`, so they inherit the change — except `cfr_utf8_display_width()` passes `NULL`, so it always treats ambiguous as narrow (see step 3).

### Step 3 — Update `cfr_utf8_display_width()` comment

**File**: `src/width.c:655-658`

The function passes `NULL` as the `CfrTerm*` and has a comment explaining why. After step 2, the implementation _does_ use the pointer. Update the comment to note that `cfr_utf8_display_width()` always treats ambiguous as width=1 (narrow) since it has no `CfrTerm*` — this is the correct behavior for the public API, as ambiguous width is a per-terminal setting.

### Step 4 — Add test `test_ambiguous_wide` in `tests/test_cfr_parser.c`

Add a test near the existing `test_vs16_widens_ambiguous` (after line 502). The test:

1. Creates a terminal with `cfg.ambiguous_wide = true` (via `cfr_set_ambiguous_wide(vt, true)` after `make_term`)
2. Feeds U+00A1 (INVERTED EXCLAMATION MARK, an Ambiguous char) + `"x"`
3. Asserts cell (0,0) has `width == 2`
4. Asserts cell (0,1) has `width == 0` (continuation)
5. Asserts cell (0,2) has `cp == 'x'`
6. Creates a second terminal with default config (`ambiguous_wide = false`)
7. Feeds the same input
8. Asserts cell (0,0) has `width == 1`
9. Asserts cell (0,1) has `cp == 'x'`

Register with `RUN_TEST(test_ambiguous_wide)` in `main()`.

**Note**: U+2713 (CHECK MARK) was initially considered but is UAX #11 property N (Neutral), not A (Ambiguous). U+00A1 is the first codepoint in the Ambiguous table and is used instead.

### Step 5 — Build and test

```sh
make -j$(nproc) && make check TESTS='test_cfr_parser'
```

## Files Changed

| File                      | Change                                                                                                                                                                                         |
| ------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `src/width.c`             | Add `AMBIGUOUS[]` table (179 ranges); wire `vt->ambiguous_wide` check into `cfr_codepoint_width()`; update `cfr_utf8_display_width()` comment; update header comment; add regeneration comment |
| `tests/test_cfr_parser.c` | Add `test_ambiguous_wide` test + `RUN_TEST` registration                                                                                                                                       |

## No Changes Needed

| File                                | Why                                                                                                                          |
| ----------------------------------- | ---------------------------------------------------------------------------------------------------------------------------- |
| `include/coffer/coffer.h`           | `CfrConfig.ambiguous_wide` and `cfr_set_ambiguous_wide()` already exist                                                      |
| `src/term.c`                        | Already copies `cfg->ambiguous_wide` to `vt->ambiguous_wide` (line 84) and implements `cfr_set_ambiguous_wide()` (line 175)  |
| `src/coffer_internal.h`             | `CfrTerm.ambiguous_wide` field already exists (line 271)                                                                     |
| `src/scripts/gen_unicode_tables.py` | Already supports generating the `CFR_AMBIGUOUS` table from `EastAsianWidth.txt` — used as-is to generate the committed table |

## Risk Assessment

- **Low risk**: The `AMBIGUOUS[]` table is generated from official UCD data via `gen_unicode_tables.py`, not hand-maintained. The committed table has been verified to match the script output exactly.
- **No risk to existing behavior**: Default is `false`, identical to current behavior. The new table is only consulted when the flag is on. The existing `test_vs16_widens_ambiguous` test (which asserts U+26A0 is width 1 without VS16) continues to pass because `make_term()` uses `CFR_CONFIG_DEFAULTS` where `ambiguous_wide = false`.
