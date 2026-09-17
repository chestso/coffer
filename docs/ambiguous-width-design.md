# Design: Ambiguous-Width (2-Cell) Rendering — Coffer Side

## Executive Summary

The plumbing for ambiguous-width rendering is fully in place (`CfrConfig.ambiguous_wide`, `CfrTerm.ambiguous_wide`, `cfr_set_ambiguous_wide()`). Two things were missing: the `AMBIGUOUS[]` range table, and the check in `cfr_codepoint_width()`. Both have landed: the tables are now UCD-derived data in the generated `src/unicode_tables.h` (`CFR_AMBIGUOUS`, consulted only when `vt->ambiguous_wide` is set), emitted by `src/scripts/gen_unicode_tables.py`.

## Current State

| Component                     | Location                            | Status                                                                 |
| ----------------------------- | ----------------------------------- | ---------------------------------------------------------------------- |
| `CfrConfig.ambiguous_wide`    | `include/coffer/coffer.h:336`       | Exists, defaults to `false`                                            |
| `CfrTerm.ambiguous_wide`      | `src/coffer_internal.h:271`         | Exists                                                                 |
| Config copy to `vt`           | `src/term.c:84`                     | `vt->ambiguous_wide = cfg->ambiguous_wide`                             |
| `cfr_set_ambiguous_wide()`    | `src/term.c:175`                    | Implemented                                                            |
| `CFR_AMBIGUOUS[]` table       | `src/unicode_tables.h` (generated)  | Present — 179 ranges from `EastAsianWidth.txt` class A                 |
| `cfr_codepoint_width()` check | `src/width.c`                       | Implemented — consults `vt->ambiguous_wide`                            |
| `cfr_utf8_display_width()`    | `src/width.c`                       | Passes `NULL` to `cfr_codepoint_width()` — ambiguous always narrow     |
| `gen_unicode_tables.py`       | `src/scripts/gen_unicode_tables.py` | Emits the committed header (`--prefix`/`--type`/`--out`); run + commit |

## Implementation Steps

### Step 1 — Generate `AMBIGUOUS[]` table using `gen_unicode_tables.py`

Download UCD files from `https://www.unicode.org/Public/UCD/latest/ucd/`:

- `EastAsianWidth.txt`
- `auxiliary/GraphemeBreakProperty.txt`
- `emoji/emoji-data.txt`
- `DerivedCoreProperties.txt`

The script downloads these automatically and writes the generated header
(a provenance banner records the UCD versions, per-file SHA-256, the
generator's SHA-256, and the exact command):

```sh
python3 src/scripts/gen_unicode_tables.py --out src/unicode_tables.h
```

Or with a local UCD directory:

```sh
python3 src/scripts/gen_unicode_tables.py --ucd /path/to/UCD --out src/unicode_tables.h
```

Commit the regenerated header. `width.c` includes it and holds all the
logic: the ranges are `CFR_*` (sized by the generated `CFR_*_LEN`
macros), so the tables are never hand-maintained or hand-copied.

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
    if (vt && vt->ambiguous_wide && range_lookup(CFR_AMBIGUOUS, CFR_AMBIGUOUS_LEN, cp))
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

| File                      | Change                                                                                                                                                                                                                    |
| ------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `src/unicode_tables.h`    | Generated: adds `CFR_AMBIGUOUS` (179 ranges) alongside the existing `CFR_WIDE` / `CFR_ZERO` / grapheme tables; committed from `gen_unicode_tables.py`                                                                     |
| `src/width.c`             | Includes the generated header instead of carrying the tables inline; `range_lookup()` takes `const CfrRange *` and call sites use the generated `CFR_*_LEN` macros; `cfr_codepoint_width()` consults `vt->ambiguous_wide` |
| `tests/test_cfr_parser.c` | Add `test_ambiguous_wide` test + `RUN_TEST` registration                                                                                                                                                                  |
| `tests/test_cfr_width.c`  | Add `test_symbol_width` — pins the UAX #11 boundaries (`U+2713` ✓ / `U+270F` ✏ narrow, `U+2693` ⚓ wide, VS16 widens, skin-tone modifiers zero-width)                                                                     |

## No Changes Needed

| File                                | Why                                                                                                                                           |
| ----------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------- |
| `include/coffer/coffer.h`           | `CfrConfig.ambiguous_wide` and `cfr_set_ambiguous_wide()` already exist                                                                       |
| `src/term.c`                        | Already copies `cfg->ambiguous_wide` to `vt->ambiguous_wide` (line 84) and implements `cfr_set_ambiguous_wide()` (line 175)                   |
| `src/coffer_internal.h`             | `CfrTerm.ambiguous_wide` field already exists (line 271)                                                                                      |
| `src/scripts/gen_unicode_tables.py` | Emits the committed `src/unicode_tables.h` (`--out`), with `--prefix`/`--type` for downstream vendors; `CFR_AMBIGUOUS` is part of that output |

## Risk Assessment

- **Low risk**: The `AMBIGUOUS[]` table is generated from official UCD data via `gen_unicode_tables.py`, not hand-maintained. The committed table has been verified to match the script output exactly.
- **No risk to existing behavior**: Default is `false`, identical to current behavior. The new table is only consulted when the flag is on. The existing `test_vs16_widens_ambiguous` test (which asserts U+26A0 is width 1 without VS16) continues to pass because `make_term()` uses `CFR_CONFIG_DEFAULTS` where `ambiguous_wide = false`.
