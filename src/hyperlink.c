/*
 * coffer — per-page OSC 8 hyperlink intern table.
 *
 * Layout mirrors style.c (FNV-1a + open-addressed probing) and
 * grapheme.c (bump-allocated byte arena). Links are deduped, so two
 * cells that point at the same link share an id — that gives a
 * renderer run-continuity for free. A link's identity is its URI plus,
 * when present, the OSC 8 `id=` parameter: per the spec the id connects
 * cells only when the target URI is also the same, and an empty id is
 * interchangeable with no id. Kitty builds the same key (`id ":" URI`,
 * with an empty id when none was given); see kitty/hyperlink.c and
 * kitty/vt-parser.c. Spec-legal fields are
 * ASCII 32–126, so hashing the raw bytes matches kitty's sanitized
 * UTF-8 key; coffer keeps the URI untruncated (kitty's 2048-byte key
 * cap is an implementation limit, not protocol).
 *
 * Ids are page-scoped, but every entry also carries a 64-bit content key
 * (FNV-1a of the identity bytes, never 0) that is the same on every
 * page. The key is hashed once, when a link first reaches the engine,
 * and is what dedup, table growth and cross-page transfer reuse; hosts
 * get it through cfr_cell_hyperlink_key(). Only the URI bytes are stored
 * in the arena: the id has already been folded into the key, and two
 * links with the same URI but different ids stay distinct because their
 * keys differ. (A 2^-64 hash collision can merge their identities, the
 * same accepted risk as for two different URIs.)
 *
 * Spec: https://gist.github.com/egmontkob/eb114294efbcd5adb1944c9f3cb5feda
 *
 * id 0 is reserved for "no link" (matches the cell's zero-init). Slot
 * value 0 in dedup_index means "empty"; non-zero values are ids.
 *
 * Cap: ids run up to UINT16_MAX (the cell's hyperlink_id field width).
 * On overflow the intern returns 0 ("no link") rather than fail loudly.
 * The live grid page cannot reach the cap: cfr_grid_gc() (grid.c) rebuilds
 * it, dropping the links no cell uses, well before 65535 ids (see
 * CFR_GRID_GC_LINKS_MAX). Scrollback pages hold at most CFR_SB_PAGE_ROWS
 * rows, so only a screen showing 65k distinct links at once could hit it.
 */

#include "coffer_internal.h"

#include <string.h>

#define CFR_HYPERLINK_DATA_INIT  256u
#define CFR_HYPERLINK_IDS_INIT   8u
#define CFR_HYPERLINK_DEDUP_INIT 16u
#define CFR_HYPERLINK_ID_MAX     UINT16_MAX

/* 64-bit FNV-1a, remapped so that 0 stays free for "no link". */
static uint64_t fnv1a(uint64_t h, const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; ++i) {
        h ^= data[i];
        h *= 1099511628211ull;
    }
    return h;
}

static uint64_t finish_key(uint64_t h)
{
    return h ? h : 1u;
}

/* Link identity: the OSC 8 `id=` value (if any) plus the URI, always
 * separated by ':'. The separator mirrors kitty's key composition
 * (`id ":" URI`, empty id included). Emitting it even without an id is
 * load-bearing: hashing the bare URI would let a no-id link whose URI is
 * `<id>:<uri>` alias the id-bearing link (`id`, `<uri>`), and URIs
 * routinely contain ':'. The spec forbids ':' in params, so the id
 * itself cannot absorb the separator. An empty id hashes the same as no
 * id, per the spec. */
static uint64_t link_key_id(const uint8_t *id, uint32_t id_len,
                            const uint8_t *uri, uint32_t uri_len)
{
    uint64_t h = 14695981039346656037ull;
    if (id && id_len > 0)
        h = fnv1a(h, id, id_len);
    h ^= (uint8_t)':';
    h *= 1099511628211ull;
    return finish_key(fnv1a(h, uri, uri_len));
}

static bool ensure_init(CfrTerm *vt, CfrHyperlinkTable *t)
{
    if (t->data && t->offsets && t->lengths && t->keys && t->dedup_index)
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
    if (!t->keys) {
        t->keys = cfr_alloc(vt, CFR_HYPERLINK_IDS_INIT * sizeof(uint64_t));
        if (!t->keys)
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
    uint64_t *nk = cfr_realloc(vt, t->keys, new_cap * sizeof(uint64_t));
    if (!nk)
        return false;
    t->keys = nk;
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
        uint32_t slot = (uint32_t)t->keys[id] & mask;
        while (ni[slot] != 0)
            slot = (slot + 1) & mask;
        ni[slot] = id;
    }
    cfr_dealloc(vt, t->dedup_index);
    t->dedup_index = ni;
    t->dedup_capacity = new_cap;
    return true;
}

static bool intern_keyed(CfrTerm *vt, CfrPage *page, const uint8_t *uri,
                         uint32_t uri_len, uint64_t key, uint16_t *out_id)
{
    if (!page || !uri || uri_len == 0) {
        *out_id = 0;
        return true;
    }
    CfrHyperlinkTable *t = &page->hyperlinks;
    if (!ensure_init(vt, t))
        return false;

    uint32_t mask = t->dedup_capacity - 1;
    uint32_t slot = (uint32_t)key & mask;
    for (;;) {
        uint16_t id = t->dedup_index[slot];
        if (id == 0)
            break; /* empty — insert here */
        if (t->keys[id] == key && t->lengths[id] == uri_len &&
            memcmp(t->data + t->offsets[id], uri, uri_len) == 0) {
            *out_id = id; /* hit */
            return true;
        }
        slot = (slot + 1) & mask;
    }

    /* Insert. Cap at UINT16_MAX. */
    if (t->count >= CFR_HYPERLINK_ID_MAX)
        return false;
    if (t->count + 1 >= t->capacity_ids) {
        if (!grow_ids(vt, t))
            return false;
    }
    if (t->used + uri_len > t->capacity) {
        if (!grow_data(vt, t, uri_len))
            return false;
    }
    uint16_t new_id = (uint16_t)(t->count + 1u);
    t->offsets[new_id] = t->used;
    t->lengths[new_id] = uri_len;
    t->keys[new_id] = key;
    memcpy(t->data + t->used, uri, uri_len);
    t->used += uri_len;
    t->count = new_id;
    t->dedup_index[slot] = new_id;

    /* Maintain load factor < 70%. Rebuild rehashes; the just-placed
     * entry's slot may move, which is fine. A failed rebuild leaves a
     * valid, merely dense, index — entries stay findable, so it is not
     * an intern failure. */
    if ((uint32_t)t->count * 10u > t->dedup_capacity * 7u)
        grow_dedup(vt, t);
    *out_id = new_id;
    return true;
}

uint16_t cfr_hyperlink_intern(CfrTerm *vt, CfrPage *page,
                              const uint8_t *uri, uint32_t uri_len)
{
    if (!uri || uri_len == 0)
        return 0;
    uint16_t id = 0;
    (void)intern_keyed(vt, page, uri, uri_len,
                       link_key_id(NULL, 0, uri, uri_len), &id);
    return id;
}

uint64_t cfr_hyperlink_key(const CfrPage *page, uint16_t id)
{
    if (!page || id == 0 || id > page->hyperlinks.count)
        return 0;
    return page->hyperlinks.keys[id];
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

bool cfr_hyperlink_transfer(CfrTerm *vt, CfrPage *dst,
                            const CfrPage *src, uint16_t id, uint16_t *out_id)
{
    if (!dst || !src || id == 0 || id > src->hyperlinks.count) {
        *out_id = 0;
        return true;
    }
    const CfrHyperlinkTable *t = &src->hyperlinks;
    /* Intern straight out of the source arena: reading through a fixed
     * stack buffer would truncate a URI longer than the buffer and
     * re-intern the truncated prefix as a different link. The stored key
     * is the link's content key already (id folded in), so it is not
     * computed again. */
    return intern_keyed(vt, dst, t->data + t->offsets[id], t->lengths[id],
                        t->keys[id], out_id);
}

CfrLink *cfr_link_new(CfrTerm *vt, const uint8_t *uri, uint32_t uri_len,
                      const uint8_t *id, uint32_t id_len)
{
    if (!uri || uri_len == 0)
        return NULL;
    if (!id)
        id_len = 0;
    CfrLink *l = cfr_alloc(vt, sizeof(*l) + uri_len);
    if (!l)
        return NULL;
    memset(l, 0, sizeof(*l));
    l->refs = 1;
    l->len = uri_len;
    l->key = link_key_id(id, id_len, uri, uri_len);
    memcpy(l->uri, uri, uri_len);
    return l;
}

void cfr_link_release(CfrTerm *vt, CfrLink *link)
{
    if (!link)
        return;
    CFR_BUG_CHECK(link->refs > 0, "link release: refcount underflow");
    if (--link->refs == 0)
        cfr_dealloc(vt, link);
}

uint16_t cfr_link_page_id(CfrTerm *vt, CfrPage *page, CfrLink *link)
{
    if (!link || !page)
        return 0;
    if (link->cache_serial == page->serial && page->serial != 0)
        return link->cache_id;
    uint16_t id = 0;
    if (!intern_keyed(vt, page, link->uri, link->len, link->key, &id))
        return 0;
    if (id != 0) {
        link->cache_serial = page->serial;
        link->cache_id = id;
    }
    return id;
}

void cfr_hyperlink_free(CfrTerm *vt, CfrHyperlinkTable *t)
{
    if (!t)
        return;
    cfr_dealloc(vt, t->data);
    cfr_dealloc(vt, t->offsets);
    cfr_dealloc(vt, t->lengths);
    cfr_dealloc(vt, t->keys);
    cfr_dealloc(vt, t->dedup_index);
    memset(t, 0, sizeof(*t));
}
