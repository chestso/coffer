/*
 * coffer — per-page OSC 8 hyperlink intern table.
 *
 * Layout mirrors style.c (FNV-1a + open-addressed probing) and
 * grapheme.c (bump-allocated byte arena). URIs are deduped, so two
 * cells with the same URL share an id — that gives a renderer
 * run-continuity for free, covering the OSC 8 spec's primary use case
 * for the optional `id=` parameter.
 *
 * Spec: https://gist.github.com/egmontkob/eb114294efbcd5adb1944c9f3cb5feda
 *
 * id 0 is reserved for "no link" (matches the cell's zero-init). Slot
 * value 0 in dedup_index means "empty"; non-zero values are ids.
 *
 * Cap: ids run up to UINT16_MAX (the cell's hyperlink_id field width).
 * On overflow the intern returns 0 ("no link") rather than fail loudly —
 * a workload that exhausts 65k unique URIs in one page is exotic and
 * "stops underlining new ones" is a benign degradation.
 */

#include "coffer_internal.h"

#include <string.h>

#define CFR_HYPERLINK_DATA_INIT  256u
#define CFR_HYPERLINK_IDS_INIT   8u
#define CFR_HYPERLINK_DEDUP_INIT 16u
#define CFR_HYPERLINK_ID_MAX     UINT16_MAX

static uint32_t fnv1a(const uint8_t *data, uint32_t len)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < len; ++i) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}

static bool ensure_init(CfrTerm *vt, CfrHyperlinkTable *t)
{
    if (t->data && t->offsets && t->lengths && t->dedup_index)
        return true;
    if (!t->data) {
        t->data = cfr_alloc(vt, CFR_HYPERLINK_DATA_INIT);
        if (!t->data)
            return false;
        t->capacity = CFR_HYPERLINK_DATA_INIT;
        t->used = 0;
    }
    if (!t->offsets) {
        t->offsets = cfr_alloc(vt, CFR_HYPERLINK_IDS_INIT * sizeof(uint32_t));
        if (!t->offsets)
            return false;
        t->capacity_ids = CFR_HYPERLINK_IDS_INIT;
    }
    if (!t->lengths) {
        t->lengths = cfr_alloc(vt, CFR_HYPERLINK_IDS_INIT * sizeof(uint32_t));
        if (!t->lengths)
            return false;
    }
    if (!t->dedup_index) {
        t->dedup_index = cfr_alloc(vt, CFR_HYPERLINK_DEDUP_INIT * sizeof(uint16_t));
        if (!t->dedup_index)
            return false;
        t->dedup_capacity = CFR_HYPERLINK_DEDUP_INIT;
        memset(t->dedup_index, 0, t->dedup_capacity * sizeof(uint16_t));
    }
    /* Slot 0 is reserved (= "no link"); count starts at 0 and the first
     * inserted id is 1. */
    t->count = 0;
    return true;
}

static bool grow_data(CfrTerm *vt, CfrHyperlinkTable *t, uint32_t need_extra)
{
    uint32_t new_cap = t->capacity ? t->capacity : CFR_HYPERLINK_DATA_INIT;
    while (new_cap < t->used + need_extra) {
        if (new_cap > UINT32_MAX / 2)
            return false;
        new_cap *= 2;
    }
    if (new_cap == t->capacity)
        return true;
    uint8_t *nd = cfr_realloc(vt, t->data, new_cap);
    if (!nd)
        return false;
    t->data = nd;
    t->capacity = new_cap;
    return true;
}

static bool grow_ids(CfrTerm *vt, CfrHyperlinkTable *t)
{
    uint32_t new_cap = (uint32_t)t->capacity_ids * 2u;
    if (new_cap > CFR_HYPERLINK_ID_MAX)
        new_cap = CFR_HYPERLINK_ID_MAX;
    if (new_cap == t->capacity_ids)
        return false;
    uint32_t *no = cfr_realloc(vt, t->offsets, new_cap * sizeof(uint32_t));
    if (!no)
        return false;
    t->offsets = no;
    uint32_t *nl = cfr_realloc(vt, t->lengths, new_cap * sizeof(uint32_t));
    if (!nl)
        return false;
    t->lengths = nl;
    t->capacity_ids = (uint16_t)new_cap;
    return true;
}

static bool grow_dedup(CfrTerm *vt, CfrHyperlinkTable *t)
{
    uint32_t new_cap = t->dedup_capacity * 2;
    uint16_t *ni = cfr_alloc(vt, new_cap * sizeof(uint16_t));
    if (!ni)
        return false;
    memset(ni, 0, new_cap * sizeof(uint16_t));
    uint32_t mask = new_cap - 1;
    for (uint16_t id = 1; id <= t->count; ++id) {
        uint32_t h = fnv1a(t->data + t->offsets[id], t->lengths[id]);
        uint32_t slot = h & mask;
        while (ni[slot] != 0)
            slot = (slot + 1) & mask;
        ni[slot] = id;
    }
    cfr_dealloc(vt, t->dedup_index);
    t->dedup_index = ni;
    t->dedup_capacity = new_cap;
    return true;
}

uint16_t cfr_hyperlink_intern(CfrTerm *vt, CfrPage *page,
                              const uint8_t *uri, uint32_t uri_len)
{
    if (!page || !uri || uri_len == 0)
        return 0;
    CfrHyperlinkTable *t = &page->hyperlinks;
    if (!ensure_init(vt, t))
        return 0;

    uint32_t hash = fnv1a(uri, uri_len);
    uint32_t mask = t->dedup_capacity - 1;
    uint32_t slot = hash & mask;
    for (;;) {
        uint16_t id = t->dedup_index[slot];
        if (id == 0)
            break; /* empty — insert here */
        if (t->lengths[id] == uri_len &&
            memcmp(t->data + t->offsets[id], uri, uri_len) == 0)
            return id; /* hit */
        slot = (slot + 1) & mask;
    }

    /* Insert. Cap at UINT16_MAX. */
    if (t->count >= CFR_HYPERLINK_ID_MAX)
        return 0;
    if (t->count + 1 >= t->capacity_ids) {
        if (!grow_ids(vt, t))
            return 0;
    }
    if (t->used + uri_len > t->capacity) {
        if (!grow_data(vt, t, uri_len))
            return 0;
    }
    uint16_t new_id = (uint16_t)(t->count + 1u);
    t->offsets[new_id] = t->used;
    t->lengths[new_id] = uri_len;
    memcpy(t->data + t->used, uri, uri_len);
    t->used += uri_len;
    t->count = new_id;
    t->dedup_index[slot] = new_id;

    /* Maintain load factor < 70%. Rebuild rehashes; the just-placed
     * entry's slot may move, which is fine. */
    if ((uint32_t)t->count * 10u > t->dedup_capacity * 7u)
        grow_dedup(vt, t);
    return new_id;
}

size_t cfr_hyperlink_read(const CfrPage *page, uint16_t id,
                          uint8_t *out, size_t out_cap)
{
    if (!page || id == 0 || !out || out_cap == 0)
        return 0;
    const CfrHyperlinkTable *t = &page->hyperlinks;
    if (id > t->count)
        return 0;
    uint32_t len = t->lengths[id];
    size_t n = (len > out_cap) ? out_cap : len;
    memcpy(out, t->data + t->offsets[id], n);
    return n;
}

void cfr_hyperlink_free(CfrTerm *vt, CfrHyperlinkTable *t)
{
    if (!t)
        return;
    cfr_dealloc(vt, t->data);
    cfr_dealloc(vt, t->offsets);
    cfr_dealloc(vt, t->lengths);
    cfr_dealloc(vt, t->dedup_index);
    memset(t, 0, sizeof(*t));
}
