/* tests/wrap_cases.h — table-driven soft-wrap join cases.
 *
 * A case creates a terminal, feeds `setup` and `action`, then checks the
 * join predicate at `row`. Cases that also need cursor, cell or text
 * assertions stay as ordinary functions in the owning test; this table
 * only removes the repeated make/feed/free/assert scaffolding from the
 * pure join cases.
 */
#ifndef WRAP_CASES_H
#define WRAP_CASES_H

#include "coffer_internal.h"

#include <stdio.h>
#include <string.h>

typedef struct
{
    const char *name;
    int rows, cols;
    const char *setup;  /* may be NULL */
    const char *action; /* may be NULL */
    int row;
    bool joins;
    bool reflow; /* optional; omitted trailing field defaults to false */
} JoinCase;

static void join_case_feed(CfrTerm *vt, const char *s)
{
    cfr_input_write(vt, (const uint8_t *)s, strlen(s));
}

/* Run every case, reporting the first mismatch with the suite and case
 * names. The caller wraps this in ASSERT_TRUE so RUN_TEST accounts for it. */
static bool run_join_cases(const char *suite, const JoinCase *cases, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        const JoinCase *c = &cases[i];
        CfrConfig cfg = CFR_CONFIG_DEFAULTS;
        cfg.rows = c->rows;
        cfg.cols = c->cols;
        cfg.cell_w_px = 10;
        cfg.cell_h_px = 6;
        cfg.reflow = c->reflow;
        CfrTerm *vt = cfr_new(&cfg);
        if (!vt) {
            fprintf(stderr, "  FAIL: %s case \"%s\": cfr_new failed\n",
                    suite, c->name);
            return false;
        }
        if (c->setup)
            join_case_feed(vt, c->setup);
        if (c->action)
            join_case_feed(vt, c->action);
        bool got = cfr_row_is_continuation(vt, c->row);
        if (got != c->joins) {
            fprintf(stderr,
                    "  FAIL: %s case \"%s\": row %d joins=%d, want %d\n",
                    suite, c->name, c->row, (int)got, (int)c->joins);
            cfr_free(vt);
            return false;
        }
        cfr_free(vt);
    }
    return true;
}

#endif /* WRAP_CASES_H */
