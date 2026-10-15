# Script engine unification — design + progress tracker

Status: **in progress**
Owner repo: **coffer** (the shared engine lives here; portty consumes it)
Last updated: 2026-10-15

> This file is both the design and the state tracker. If work is
> interrupted, resume from the first unchecked box in
> [Phases and progress](#phases-and-progress). Update this file in the
> same commit that completes a step.

## Goal

`cfr-debug` (coffer) and `portty -S` (portty) each implement a debug /
automation script engine. The parsers, escape expanders, hex decoders,
grid-text searches, clocks, and half the command vocabulary are
duplicated. Consolidate:

- **Everything that is not UI-related lives in coffer** and is consumed
  by portty.
- **Only directly UI-related commands stay in portty** (anything needing
  the SDL/GL renderer, the compositor, the window, or synthetic input
  devices).
- Where both sides already define a similar feature, **coffer's
  definition wins** (command names, grammar, search semantics, failure
  behavior).

## Current state (survey, 2026-10-15)

### coffer: `contrib/cfr-debug/cfr_debug.c` (single-file, blocking)

- Line-at-a-time interpreter inside `run_script()`; drains the PTY in
  blocking `poll()`/`WaitForSingleObject` loops.
- Commands: `wait`, `wait-for`, `send`, `raw`, `assert-contains`,
  `assert-not-contains`, `await-exit`, `assert-exit`, `resize`,
  `render [visible|scrollback|all]`, `cursor`.
- Grid text search already lives in the library as public API:
  `cfr_row_text()` + `cfr_find_text()` (unified buffer: scrollback
  oldest-first, then visible rows; negative unified rows = scrollback).
- `wait-for` grammar: trailing options trimmed right-to-left; a bare
  number is the timeout (default 600 s, timeout = **failure**),
  `line-end` anchors the match to a row's end. portty's `-S` grammar was
  the model; both now parse the same grammar.
- Escape set in `send`: `\n \r \t \e \\` (no `\xNN`).
- `--help-spec` emits machine-readable `FLAG`/`CMD` lines for feature
  detection by downstream tooling.
- Exit code: 0 = all assertions passed, 1 = failure. First failure
  aborts the script.

### portty: `src/portty_script.c` (parse-up-front + per-frame step)

- `portty_script_load()` parses the whole file into a `ScriptCmd[]`
  array; `portty_script_step()` is called once per frame by the
  backend's main loop (non-blocking; a static wait deadline +
  `portty_script_wait_remaining_ms()` fold into the event-loop timer).
- Commands overlapping cfr-debug: `wait`, `wait-for`, `send`, `raw`,
  `assert-contains`, `assert-not-contains`, `resize`.
- portty-only, **engine-shaped** (candidates to move to coffer):
  `sendln`, `emit` / `emit-raw` (feed bytes straight into the VT
  parser), `quit`, `dump-sixel`.
- portty-only, **UI-shaped** (stay in portty): `screendump`,
  `renderdump`, `record-start` / `record-stop`, `panel` / `panel_hide`,
  `mousemove` / `mousedown` / `mouseup`, `winsize`, `dumpstate`,
  `assert-hover` / `assert-no-hover`, `dumpgrid` / `dumprow` /
  `dumpcells` (renderer-adjacent dumps; moved to core per decision 3).
- Duplicated machinery: escape expander (superset: adds `\xNN`),
  hex parser, monotonic clock (`portty_now_seconds`),
  grid row→UTF-8 scan (`portty_debug_grid_contains/find`,
  `grid_build_row_text`) — all of which duplicate coffer code.
- **Behavioral differences to erase** (coffer wins):
  - Search scope: portty scans the **visible grid only**; coffer
    searches the **unified buffer**. Unify on unified buffer
    (assert-not-contains becomes stricter in portty).
  - `wait-for` timeout: portty warns and **continues**; cfr-debug
    **fails**. Unify on fail.
  - Vocabulary: portty `dumpgrid` ≈ coffer `render`;
    portty has `sendln`, cfr-debug relies on `send ...\n`.
  - Escapes: union set `\n \r \t \e \\ \xNN`; `\033` stays unexpanded
    in both (documented gotcha).

## Design

### Where the engine lives

**Decision (Q1): folded into `libcoffer.a`.** No separate artifact, no
new pkg-config module — portty already links `coffer` and picks the
engine up on the next coffer version bump.

```
include/coffer/script.h     — public header for the script engine
src/script*.c               — parser + executor + runners
```

The public header is separate from `coffer.h` (consumers include it
opt-in), but the object code ships in the same static library and the
same `coffer.pc`. **File names are unprefixed** for this module
(`script.h`, `src/script_*.c` — user directive 2026-10-15), while
**types/functions/enums follow the engine's `cfr_` convention**
(`cfr_script_load()`, `CfrScriptHost`, `CFR_SCRIPT_FAILED`).

### Architecture: parse once, step incrementally, host provides I/O

portty's structure (parse-up-front + incremental step) is the one that
serves both hosts; cfr-debug's blocking interpreter becomes a thin
runner on top. One executor object, no statics (portty's current
static wait deadline goes away):

```c
typedef struct CfrScript CfrScript;    /* parsed command list  */
typedef struct CfrScriptRun CfrScriptRun;    /* execution state      */

CfrScript *cfr_script_load(const char *path, char *err, size_t errlen,
                           const char *const *ext_names /* NULL-terminated,
                             command names the host will handle; anything
                             else unknown is a parse error (Q5) */);
void       cfr_script_free(CfrScript *);

/* Host hooks. NULL = capability unsupported; the engine warns+skips
 * (or fails, for commands with no sensible skip). */
typedef struct
{
    CfrTerm *term;                 /* grid for wait-for/assert/render  */
    void (*pty_write)(void *ud, const char *data, size_t len);
    void (*emit)(void *ud, const char *data, size_t len);   /* VT input */
    int  (*poll_exit)(void *ud, int *status);               /* 1 exited */
    int  (*resize)(void *ud, int cols, int rows);  /* PTY+grid resize  */
    void (*log)(void *ud, int line, const char *fmt, ...);
    FILE *out;                     /* render/cursor dump destination   */
    /* Extension point: commands named in ext_names at load time
     * dispatch here at run time. portty registers its UI commands
     * (screendump, panel, mouse*, ...). */
    bool (*exec_ext)(void *ud, const char *name, const char *args,
                     int line, bool *fatal);
    void *ud;
} CfrScriptHost;

CfrScriptRun *cfr_script_run_new(const CfrScript *, const CfrScriptHost *);
void          cfr_script_run_free(CfrScriptRun *);

typedef enum { CFR_SCRIPT_RUNNING, CFR_SCRIPT_DONE, CFR_SCRIPT_FAILED }
    CfrScriptStatus;

/* Non-blocking: called once per frame by an event-loop host (portty). */
CfrScriptStatus cfr_script_step(CfrScriptRun *);

/* ms until the current wait/wait-for can advance, for deadline folding
 * (replaces portty_script_wait_remaining_ms). UINT32_MAX = not waiting. */
uint32_t cfr_script_wait_ms(const CfrScriptRun *);

/* Blocking convenience runner (cfr-debug): loops step(); between steps
 * calls pump(ud, cfr_script_wait_ms(run)) so the host drains its PTY.
 * Returns the final status. */
typedef void (*CfrScriptPump)(void *ud, uint32_t max_ms);
CfrScriptStatus cfr_script_run_blocking(CfrScriptRun *,
                                        CfrScriptPump pump, void *ud);
```

Key properties:

- **Same parser for both hosts.** portty's parse-time error reporting
  (`line N: ...`) becomes the shared behavior. **Decision (Q5):
  unknown commands fail at parse time.** The host's extension registry
  is passed to `cfr_script_load()`, so a typo'd UI command is a load
  error, not a runtime surprise. cfr-debug's warn-and-skip behavior is
  dropped.
- **Host supplies the `CfrTerm *`.** portty already exposes it:
  `term_cfr_get_cfr_term()` in `term_cfr.h`.
- **No statics.** Wait deadlines, recorded exit status, and failure
  state live in `CfrScriptRun`, not file-static variables.
- **Raw bytes carry a length.** portty currently stores `raw` payload in
  a NUL-terminated `text` (truncates at NUL — documented hack). The new
  command struct gets `const uint8_t *data; size_t len`.
- **`--help-spec` stays**, extended: the engine exposes its core command
  table so cfr-debug can keep emitting `CMD` lines, and portty can
  enumerate core + registered extensions.

### Command split

| Command                                   | Owner       | Notes                                                                                                  |
| ----------------------------------------- | ----------- | ------------------------------------------------------------------------------------------------------ |
| `wait`                                    | coffer core |                                                                                                        |
| `wait-for "TEXT" [TIMEOUT] [line-end]`    | coffer core | unified-buffer search via `cfr_find_text`; timeout = failure                                           |
| `send` / `sendln`                         | coffer core | `sendln` moves up from portty (**Q2 confirmed**)                                                       |
| `raw`                                     | coffer core | hex bytes → PTY input                                                                                  |
| `emit` / `emit-raw`                       | coffer core | feed VT parser directly; engine-shaped, not UI (**Q2 confirmed**)                                      |
| `assert-contains` / `assert-not-contains` | coffer core | unified buffer                                                                                         |
| `await-exit` / `assert-exit`              | coffer core | via `poll_exit` hook; portty gains these                                                               |
| `resize COLS ROWS`                        | coffer core | via `resize` hook (PTY + grid)                                                                         |
| `render [visible\|scrollback\|all]`       | coffer core | text dump to `host.out`                                                                                |
| `render cells ROW [C0 C1]`                | coffer core | per-cell detail dump — absorbs portty's `dumprow` / `dumpcells` (**Q3**: rendering info is not UI)     |
| `render styles ROW`                       | coffer core | style detail dump (scriptable form of cfr-debug's `-S` flag) (**Q3**)                                  |
| `cursor`                                  | coffer core |                                                                                                        |
| `quit`                                    | coffer core | ends the run early, status DONE; hosts treat run-end as exit (**Q6**: kept even though semi-redundant) |
| `dump-sixel`                              | coffer core | sixel store is engine state (**Q2 confirmed**)                                                         |
| `screendump`, `renderdump`                | portty      | presented framebuffer / offscene render — pure renderer                                                |
| `record-start` / `record-stop`            | portty      | QOI frame recorder, GL-frame cadence                                                                   |
| `panel` / `panel_hide`                    | portty      | notification panels                                                                                    |
| `mousemove` / `mousedown` / `mouseup`     | portty      | synthetic input drives hover/selection in the UI                                                       |
| `winsize`                                 | portty      | SDL window pixel size                                                                                  |
| `dumpstate`                               | portty      | renderer/backend state dump                                                                            |
| `assert-hover` / `assert-no-hover`        | portty      | hover state is a UI concept                                                                            |
| ~~`dumpgrid` / `dumprow` / `dumpcells`~~  | **deleted** | replaced by core `render` / `render cells` (**Q3**; coffer's vocabulary wins, no aliases)              |

### Vocabulary unification (coffer wins)

- portty's `assert-contains` / `wait-for` switch to **unified-buffer**
  search through `cfr_find_text()`. `portty_debug_grid_contains/find`
  and `grid_build_row_text` are deleted.
- `wait-for` timeout **fails the run** (exit 1 / script FAILED), as in
  cfr-debug. portty scripts that relied on warn-and-continue must be
  audited (`portty-scripts/*.portty`).
- Escape set is the **union**: `\n \r \t \e \\ \xNN`. `\033` remains
  deliberately unexpanded (README documents why).
- cfr-debug keeps `send` + explicit `\n`; portty's `sendln` becomes a
  core command available to both.

## Phases and progress

How to work this plan:

- Steps are ordered and small; do them in order, one commit per step.
- **Update the checkbox in this file in the same commit** that completes
  the step — this file is the resume point after an interruption.
- Every step that touches C ends with: build green,
  `make -C build check` green, `make -C build format`, then commit.
  Do not proceed with a red tree.
- Phases 2–3 land in coffer; phases 4–5 land in portty; phases 6–7
  touch both.
- portty consumes coffer via pkg-config, so phase 4 requires
  `make install` of coffer (or a `PKG_CONFIG_PATH` pointing at the
  coffer build tree) after each coffer change.

### Phase 0 — survey and design (done)

- [x] **0.1** Survey both implementations (`cfr_debug.c`,
      `portty_script.c`, both README/AGENTS sections).
- [x] **0.2** Write this design doc; commit it on its own.

### Phase 1 — decisions (done)

- [x] **1.1** Record decisions 1–6 (see [Decisions](#decisions)).

### Phase 2 — coffer: the `script` module

- [x] **2.1** Public header `include/coffer/script.h`: `CfrScript`,
      `CfrScriptRun`, `CfrScriptHost`, `CfrScriptStatus`,
      `CfrScriptPump`, and the load/free/run/step/wait_ms/blocking-run
      functions from the API sketch above. **Naming: files unprefixed,
      symbols follow the `cfr_` convention** — `script.h` /
      `src/script_*.c` on disk, `CfrScriptRun` / `cfr_script_step()` in
      code (user directive 2026-10-15). Otherwise follow coffer
      conventions (extern "C" guard, doc comments per function).
- [x] **2.2** Parser `src/script_parse.c`: line splitting,
      `#` comments, keyword/args split, quote stripping, escape
      expansion (union set `\n \r \t \e \\ \xNN`), hex decoder
      (portty's lenient token walk, but with a real byte length — no
      NUL-truncation hack), `wait-for` trailing-option grammar
      (right-to-left trim: bare number = timeout, `line-end`),
      core command table, `ext_names` whitelist check, parse errors
      with `line N: ...` messages.
- [x] **2.3** Executor `src/script_exec.c`: `CfrScriptRun` state
      (current index, wait deadline, recorded exit status, failure
      flag — **no file-static state**, portty's static deadline goes
      away), monotonic clock helper, `cfr_script_step()`,
      `cfr_script_wait_ms()`, `cfr_script_run_blocking()`, core command
      execution through the host hooks, `render` / `render cells` /
      `render styles` / `cursor` / `dump-sixel` output to `host.out`,
      `quit` → early DONE.
- [x] **2.4** Build wiring: add sources + header to `src/Makefile.am`
      (and the installed-headers list); confirm `libcoffer.a` grows the
      new objects and `coffer.pc` is unchanged.
- [x] **2.5** Tests `tests/test_script.c` with a fake host (no PTY;
      `emit` feeds a real `CfrTerm`, asserts run against it). Cover:
      each core command parses; escape union incl. `\xNN`; malformed
      hex rejected; `raw` with a NUL byte keeps its length; `wait-for`
      grammar (quoted/unquoted needle, timeout, `line-end`, either
      order); unknown command rejected at load; registered ext name
      loads and dispatches; `wait-for` timeout → FAILED; `quit` → early
      DONE; blocking runner end-to-end (`emit` text → `assert-contains`
      → `render` into a `tmpfile()`).
- [x] **2.6** Register the test in `tests/Makefile.am`; `make check`
      green.
- [x] **2.7** `make -C build format`; commit (`script: parser +
  executor, folded into libcoffer`).

### Phase 3 — cfr-debug migrates to the module

- [x] **3.1** Replace `run_script()` and its helpers
      (`parse_wait_for`, `expand_escapes`, `parse_hex_bytes`,
      `wait_for`, `await_exit`) in `cfr_debug.c` with
      `cfr_script_load` (no ext names) + `CfrScriptHost` wired to
      `PtyCtx` + `cfr_script_run_blocking()` with the existing `drain()`
      as the pump.
- [x] **3.2** Delete the now-duplicated grid render/search helpers in
      `cfr_debug.c` that moved into the engine (`render_grid`,
      `render_scrollback`, `render_row_cells`, `render_row_styles`);
      keep the CLI flag handling (`-s/-d/-S/-q`) by translating flags
      into engine render modes for the final dump.
- [x] **3.3** cfr-debug gains the new core commands for free; wire the
      `emit` hook to `cfr_input_write()` and expose `sendln` in help.
      Extend `--help-spec` `CMD` lines (`sendln`, `emit`, `emit-raw`,
      `render cells`, `render styles`, `quit`, `dump-sixel`) and
      `SCRIPT_COMMANDS[]`/`--help` accordingly — the table should be
      generated from the engine's command registry, not hand-duplicated.
- [x] **3.4** `contrib/cfr-debug/test-cfr-debug.sh` green via
      `make check`; manual smoke: scripted session against `bash -l`
      with `sendln`, `wait-for ... line-end`, `resize`, `render`,
      `await-exit`/`assert-exit`.
- [x] **3.5** Format; commit (`cfr-debug: run scripts through the
  script engine`).

### Phase 4 — portty adopts the engine

- [x] **4.0** `make install` the new coffer (or set `PKG_CONFIG_PATH`);
      portty `configure` still passes against the interim version.
- [x] **4.1** `portty_script.c`: replace `portty_script_load` internals
      with `cfr_script_load()`, passing portty's extension name list:
      `screendump`, `renderdump`, `dumpstate`, `record-start`,
      `record-stop`, `panel`, `panel_hide`, `mousemove`, `mousedown`,
      `mouseup`, `winsize`, `assert-hover`, `assert-no-hover`.
      Keep the `PorttyScript` handle as a thin wrapper so callers
      change minimally (or update call sites directly — pick per
      diff size).
- [x] **4.2** Build the `CfrScriptHost`: `pty_write` → portty's
      `pty_write`; `emit` → `terminal_process_input`; `term` →
      `term_cfr_get_cfr_term()`; `resize` → the existing
      backend resize path; `poll_exit` → portty's pty status query
      (this gives portty `await-exit` / `assert-exit` for free);
      `log` → portty's logging; `out` → stdout.
- [x] **4.3** Replace `portty_script_step()` call sites with
      `cfr_script_step()` per frame; replace
      `portty_script_wait_remaining_ms()` with `cfr_script_wait_ms()`
      in the event-deadline fold.
- [x] **4.4** Implement `exec_ext`: move the existing UI command case
      handlers behind it, preserving the deferred-flag mechanism for
      `screendump`/`renderdump` (GL access) and the recorder timer
      lifecycle. Raw args arrive as a string — parse per command
      (the old `ScriptCmd` field pre-parsing moves here).
- [x] **4.5** Delete duplicated code from portty: `expand_escapes`,
      `parse_hex_bytes`, `hex_val`, `grid_build_row_text`,
      `portty_debug_grid_contains/find`, `ScriptCmd`/`ScriptCmdType`,
      `portty_script_wait_remaining_ms`. Check `portty_now_seconds`
      for non-script users first — if the renderer uses it, keep it
      (it's not script-specific); otherwise delete.
- [x] **4.6** `tests/test_debug_script` rewritten against the new
      wiring (or deleted if it only tested the deleted parser — its
      grammar coverage now lives in `test_script`).
- [x] **4.7** portty `make check` green; run one `-S` script that
      exercises an extension command (`screendump`) and one that
      exercises a core command (`wait-for`).
- [x] **4.8** Format; commit (`script: adopt coffer's script
  engine, keep UI commands as extensions`).

### Phase 5 — portty script audit

- [x] **5.1** `grep` `portty-scripts/*.portty` and `tests/` for
      `wait-for` lines that relied on warn-and-continue (a timeout was
      survivable before; now it's fatal) — tighten needles or budgets.
- [x] **5.2** Same pass for `assert-not-contains` (now sees
      scrollback) — confirm no false failures from scrolled-off text.
- [x] **5.3** Migrate `dumpgrid` / `dumprow` / `dumpcells` uses to
      `render` / `render cells` / `render styles`.
- [x] **5.4** Run every `.portty` script end to end; fix fallout.
- [x] **5.5** Update the `Debug scripts (-S FILE)` section of
      `portty/AGENTS.md` (vocabulary, semantics, extension list).
- [x] **5.6** Commit (`scripts: migrate to unified script semantics`).

### Phase 6 — docs

- [ ] **6.1** coffer: `contrib/cfr-debug/README.md` script table
      (new commands, `render cells`/`render styles`), `cfr-debug.1`
      man page.
- [ ] **6.2** coffer: `README.md` feature list mentions the script
      engine; `AGENTS.md` gains `script_parse.c` /
      `script_exec.c` in the layout table and `test_script`
      in the test list.
- [ ] **6.3** Both repos: `FOLLOWUPS.md` / `TODO.md` entries closed
      or created for leftovers.
- [ ] **6.4** Commit (`docs: script engine unification`).

### Phase 7 — release mechanics

- [ ] **7.1** coffer version bump (minor — new public API in
      `libcoffer`), `make distcheck`, tag.
- [ ] **7.2** portty `configure.ac`: raise
      `PKG_CHECK_MODULES([COFFER], [coffer >= <new>])` floor.
- [ ] **7.3** Final cross-repo verification: coffer `make check`,
      portty `make check`, one `-S` run, one `cfr-debug -f` run.
- [ ] **7.4** Commit; mark this section complete.

## Decisions

Recorded 2026-10-15:

1. **Artifact shape** — the script engine folds into `libcoffer.a`
   (public header `include/coffer/script.h`, same `coffer.pc`). No
   separate library or pkg-config module.
2. **Engine-shaped portty commands** — `sendln`, `emit` / `emit-raw`,
   `dump-sixel`, `quit` all move to coffer core. Confirmed.
3. **Cell/style dumps** — become core `render cells ROW [C0 C1]` and
   `render styles ROW` modes; anything that doesn't require UI
   rendering lives in coffer. portty's `dumpgrid` / `dumprow` /
   `dumpcells` are deleted, not aliased.
4. **portty behavior change accepted** — unified-buffer search
   (stricter `assert-not-contains`) and fatal `wait-for` timeouts are
   adopted in portty as the better semantics; existing portty scripts
   are audited in phase 5.
5. **Unknown commands** — parse-time error. The host passes its
   extension command names to `cfr_script_load()`; anything not core
   and not registered fails the load with `line N: unknown command`.
6. **`quit`** — stays a core command (ends the run early, status DONE)
   even though it is semi-redundant in `-S`-style batch mode; hosts
   exit when the run ends.
7. **File names unprefixed; symbols follow the `cfr_` convention**
   (user directives 2026-10-15) — files are `include/coffer/script.h`,
   `src/script_parse.c`, `src/script_exec.c`; types/functions/enums use
   the project-standard prefix (`cfr_script_load()`, `CfrScriptRun`,
   `CFR_SCRIPT_FAILED`).
