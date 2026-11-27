/* lottie.c — Lottie animation protocol for coffer.
 *
 * Parses APC sequences (ESC _ ... ST), owns animation definitions, placements
 * and playback clocks, and exposes the frame the host should rasterize. Pixel
 * production and raster caches belong to the host renderer. */

#include "coffer_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum
{
    LT_FIT_CONTAIN = 0, /* default — scale to fit within region */
    LT_FIT_NONE = 1,    /* no auto-scaling — use explicit scale or design size */
};

typedef struct
{
    uint32_t definition_rev; /* increments when the JSON document changes */

    /* The complete Lottie document. The host reads it when definition_rev
     * changes; coffer never interprets its vector drawing commands. */
    uint8_t *json;
    size_t json_len;

    /* Design space from the Lottie JSON (w, h fields). */
    int design_w;
    int design_h;

    /* Requested host raster size in device pixels. */
    int px_w;
    int px_h;

    /* Placement region (from load/place commands). */
    int p_width;           /* pixel region width, 0 = no limit */
    int p_height;          /* pixel region height, 0 = no limit */
    int p_cols;            /* cell region width, 0 = no limit */
    int p_rows;            /* cell region height, 0 = no limit */
    double explicit_scale; /* user scale, only used with fit:"none" */
    int fit;               /* LT_FIT_CONTAIN or LT_FIT_NONE */

    int current_frame;
    int frame_ip;
    int frame_op;
    double frame_fr;
    double speed;
    bool playing;
    bool loop;

    /* Playback position is a pure function of presentation time:
     *   pos(t) = pos0 + (t - anchor_us) * speed * frame_fr / 1e6
     * in frames from frame_ip. The first present after a playback change
     * anchors the clock. */
    uint64_t anchor_us;
    double pos0;
    double last_pos;
    bool anchored;
} LtRec;

/* Chunked upload accumulator. */
typedef struct
{
    uint64_t id;
    uint8_t *buf;
    size_t buf_len;
    size_t buf_cap;
    int chunks_received;
    int chunks_total;
} LtChunkAccum;

struct CfrLottieState
{
    LtChunkAccum *chunks;
    int chunk_count;
    int chunk_cap;

    CfrLottie *scratch;
    int scratch_cap;
};

/* ------------------------------------------------------------------ */
/* Forward declarations for command handlers                          */
/* ------------------------------------------------------------------ */

static void lt_cmd_load(struct CfrLottieState *st, CfrTerm *vt,
                        const char *json, size_t json_len);
static void lt_cmd_load_chunk(struct CfrLottieState *st, CfrTerm *vt,
                              const char *json, size_t json_len);
static void lt_cmd_place(struct CfrLottieState *st, CfrTerm *vt,
                         const char *json, size_t json_len);
static void lt_cmd_play(struct CfrLottieState *st, CfrTerm *vt,
                        const char *json, size_t json_len);
static void lt_cmd_pause(struct CfrLottieState *st, CfrTerm *vt,
                         const char *json, size_t json_len);
static void lt_cmd_stop(struct CfrLottieState *st, CfrTerm *vt,
                        const char *json, size_t json_len);
static void lt_cmd_seek(struct CfrLottieState *st, CfrTerm *vt,
                        const char *json, size_t json_len);
static void lt_cmd_delete(struct CfrLottieState *st, CfrTerm *vt,
                          const char *json, size_t json_len);
static int lt_placements_for_count(CfrTerm *vt, uint64_t id);

/* ------------------------------------------------------------------ */
/* Lazy allocation                                                    */
/* ------------------------------------------------------------------ */

static struct CfrLottieState *lt_state(CfrTerm *vt)
{
    if (!vt->lottie) {
        vt->lottie = cfr_alloc(vt, sizeof(*vt->lottie));
        if (!vt->lottie)
            return NULL;
        memset(vt->lottie, 0, sizeof(*vt->lottie));
    }
    return vt->lottie;
}

/* Get (or lazily create) the shared image store. */
static CfrImgStore *lt_store(CfrTerm *vt)
{
    if (vt->images)
        return vt->images;
    CfrImgStore *st = cfr_img_store_new(vt);
    if (st)
        vt->images = st;
    return st;
}

/* ------------------------------------------------------------------ */
/* Record management                                                  */
/* ------------------------------------------------------------------ */

/* Restart timing from the frame currently shown: the next present anchors
 * the clock and playback continues from the start of that frame. */
static void lt_anchor_at_frame(LtRec *r)
{
    r->pos0 = (double)(r->current_frame - r->frame_ip);
    r->last_pos = r->pos0;
    r->anchored = false;
}

/* Restart timing from the exact position of the last present (keeps the
 * fractional frame), for a speed or loop change on a running animation. */
static void lt_anchor_keep_position(LtRec *r)
{
    r->pos0 = r->last_pos;
    r->anchored = false;
}

static LtRec *lt_find_by_id(CfrTerm *vt, struct CfrLottieState *st,
                            uint64_t id)
{
    (void)st;
    if (!vt->images)
        return NULL;
    int idx = cfr_img_find_by_source_id(vt->images, IMG_SRC_LOTTIE, id,
                                        (uint8_t)vt->in_altscreen);
    return idx >= 0 ? vt->images->imgs[idx].source_state : NULL;
}

static int lt_index_of(CfrTerm *vt, const LtRec *rec)
{
    if (!vt->images)
        return -1;
    for (int i = 0; i < vt->images->img_count; i++) {
        if (vt->images->imgs[i].source == IMG_SRC_LOTTIE &&
            vt->images->imgs[i].source_state == rec)
            return i;
    }
    return -1;
}

static void lt_rec_free(void *vt, void *state)
{
    LtRec *rec = state;
    if (!rec)
        return;
    cfr_dealloc(vt, rec->json);
    cfr_dealloc(vt, rec);
}

/* Bump the globally unique pixel generation for this animation. */
static void lt_touch_raster(CfrTerm *vt, LtRec *rec)
{
    int idx = lt_index_of(vt, rec);
    if (idx >= 0)
        vt->images->imgs[idx].version = ++vt->images->version_seq;
}

/* ------------------------------------------------------------------ */
/* JSON command parsing                                               */
/* ------------------------------------------------------------------ */

static const char *lt_json_find_key(const char *json, size_t json_len,
                                    const char *key, size_t *out_len)
{
    size_t klen = strlen(key);
    const char *p = json;
    const char *end = json + json_len;

    while (p < end && *p != '{')
        p++;
    if (p >= end)
        return NULL;
    p++;

    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
            p++;
        if (p >= end || *p == '}')
            break;

        if (*p != '"') {
            p++;
            continue;
        }
        p++;

        const char *kstart = p;
        while (p < end && *p != '"') {
            if (*p == '\\')
                p++;
            p++;
        }
        size_t found_klen = (size_t)(p - kstart);
        if (p < end)
            p++;

        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
            p++;
        if (p < end && *p == ':')
            p++;
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
            p++;

        const char *vstart = p;
        if (p >= end)
            break;

        if (*p == '"') {
            p++;
            while (p < end && *p != '"') {
                if (*p == '\\')
                    p++;
                p++;
            }
            if (p < end)
                p++;
        } else if (*p == '{' || *p == '[') {
            char open = *p, close = (*p == '{') ? '}' : ']';
            int depth = 1;
            p++;
            while (p < end && depth > 0) {
                if (*p == '"') {
                    p++;
                    while (p < end && *p != '"') {
                        if (*p == '\\')
                            p++;
                        p++;
                    }
                    if (p < end)
                        p++;
                    continue;
                }
                if (*p == open)
                    depth++;
                else if (*p == close)
                    depth--;
                p++;
            }
        } else if (*p == 't' || *p == 'f' || *p == 'n') {
            while (p < end && *p >= 'a' && *p <= 'z')
                p++;
        } else {
            while (p < end && ((*p >= '0' && *p <= '9') ||
                               *p == '.' || *p == '-' ||
                               *p == '+' || *p == 'e' || *p == 'E'))
                p++;
        }

        size_t vlen = (size_t)(p - vstart);

        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' ||
                           *p == '\r' || *p == ','))
            p++;

        if (found_klen == klen && memcmp(kstart, key, klen) == 0) {
            *out_len = vlen;
            return vstart;
        }
    }
    return NULL;
}

static long lt_json_int(const char *val, size_t len)
{
    if (!val || len == 0)
        return 0;
    char tmp[32];
    size_t n = len < 31 ? len : 31;
    memcpy(tmp, val, n);
    tmp[n] = '\0';
    return strtol(tmp, NULL, 10);
}

static double lt_json_double(const char *val, size_t len)
{
    if (!val || len == 0)
        return 0.0;
    char tmp[64];
    size_t n = len < 63 ? len : 63;
    memcpy(tmp, val, n);
    tmp[n] = '\0';
    return strtod(tmp, NULL);
}

static bool lt_json_bool(const char *val, size_t len)
{
    return val && len >= 4 && memcmp(val, "true", 4) == 0;
}

/* ------------------------------------------------------------------ */
/* Size computation                                                   */
/* ------------------------------------------------------------------ */

/* Compute aspect-correct rasterization pixels from design size and
 * constraints.  Returns the uniform scale factor via *out_scale. */
static double lt_compute_scale(int design_w, int design_h,
                               int p_width, int p_height,
                               int p_cols, int p_rows,
                               int cell_w_px, int cell_h_px,
                               int fit, double explicit_scale)
{
    switch (fit) {
    case LT_FIT_NONE:
        return explicit_scale > 0.0 ? explicit_scale : 1.0;
    case LT_FIT_CONTAIN:
    default:
    {
        /* Convert region to px, take tightest */
        double px_max_w = -1.0; /* -1 = infinity */
        double px_max_h = -1.0;
        if (p_width > 0)
            px_max_w = (double)p_width;
        if (p_cols > 0) {
            double cw = (double)p_cols * cell_w_px;
            if (px_max_w < 0 || cw < px_max_w)
                px_max_w = cw;
        }
        if (p_height > 0)
            px_max_h = (double)p_height;
        if (p_rows > 0) {
            double ch = (double)p_rows * cell_h_px;
            if (px_max_h < 0 || ch < px_max_h)
                px_max_h = ch;
        }

        double scale = 1.0;
        if (px_max_w > 0 && design_w > 0)
            scale = px_max_w / design_w;
        if (px_max_h > 0 && design_h > 0) {
            double sh = px_max_h / design_h;
            if (sh < scale)
                scale = sh;
        }
        return scale;
    }
    }
}

/* Emit an APC report (OSC 5556 on Windows) with placement info. The cell
 * figures are DEVICE pixels, matching the cell-size queries (CSI 16 t). */
static void lt_emit_report(CfrTerm *vt, uint64_t id,
                           int row, int col, int rows, int cols,
                           int raster_w, int raster_h)
{
    char json[200];
    int jn = snprintf(json, sizeof(json),
                      "{\"type\":\"report\",\"id\":%llu,\"row\":%d,\"col\":%d,"
                      "\"rows\":%d,\"cols\":%d,\"raster_w\":%d,\"raster_h\":%d,"
                      "\"cell_w_px\":%d,\"cell_h_px\":%d}",
                      (unsigned long long)id, row, col, rows, cols,
                      raster_w, raster_h, vt->cell_w_px, vt->cell_h_px);
    if (jn <= 0)
        return;

    size_t b64_len = 0;
    char *b64 = cfr_base64_encode(vt, (const uint8_t *)json, (size_t)jn,
                                  &b64_len);
    if (!b64)
        return;

    char seq[340];
    int n;
#ifdef _WIN32
    n = snprintf(seq, sizeof(seq), "\x1b]5556;%.*s\x07", (int)b64_len, b64);
#else
    n = snprintf(seq, sizeof(seq), "\x1b_%.*s\x1b\\", (int)b64_len, b64);
#endif
    if (n > 0)
        cfr_emit_bytes(vt, (const uint8_t *)seq, (size_t)n);
    cfr_dealloc(vt, b64);
}

static int lt_add_placement(CfrTerm *vt, LtRec *rec,
                            long requested_abs_line, int requested_col,
                            long abs_line, int col, int rows, int cols,
                            uint8_t layer, uint8_t opacity)
{
    CfrImgStore *store = lt_store(vt);
    int idx = lt_index_of(vt, rec);
    if (!store || idx < 0)
        return -1;
    int pi = cfr_img_add_placement(vt, store, store->imgs[idx].handle,
                                   abs_line, col, rows, cols, layer, opacity, 0);
    if (pi >= 0) {
        store->places[pi].requested_abs_line = requested_abs_line;
        store->places[pi].requested_col = requested_col;
    }
    return pi;
}

/* ------------------------------------------------------------------ */
/* Damage helpers                                                     */
/* ------------------------------------------------------------------ */

static void lt_damage_placement(CfrTerm *vt, int idx)
{
    CfrImgStore *store = vt->images;
    if (!store || idx < 0 || idx >= store->place_count)
        return;
    CfrPlacement *pl = &store->places[idx];
    if (pl->screen != (uint8_t)vt->in_altscreen)
        return;
    int top = (int)(pl->abs_line - vt->sixel_abs_top);
    int bot = top + pl->rows - 1;
    if (top < 0)
        top = 0;
    if (bot >= vt->rows)
        bot = vt->rows - 1;
    for (int r = top; r <= bot; r++)
        cfr_damage_row(vt, r);
}

static void lt_damage_all_placements(CfrTerm *vt, LtRec *rec)
{
    CfrImgStore *store = vt->images;
    int idx = lt_index_of(vt, rec);
    if (!store || idx < 0)
        return;
    uint64_t handle = store->imgs[idx].handle;
    for (int i = 0; i < store->place_count; i++)
        if (store->places[i].image_handle == handle)
            lt_damage_placement(vt, i);
}

static void lt_cell_box(CfrTerm *vt, const LtRec *rec, int *cols, int *rows)
{
    *cols = (rec->px_w + vt->cell_w_px - 1) / vt->cell_w_px;
    *rows = (rec->px_h + vt->cell_h_px - 1) / vt->cell_h_px;
    if (*cols < 1)
        *cols = 1;
    if (*rows < 1)
        *rows = 1;
}

/* Recompute every placement from its requested region. The requested anchor
 * survives centering and cell-metric changes, so a raster is never scaled to
 * hide stale geometry. */
static void lt_relayout_placements(CfrTerm *vt, LtRec *rec)
{
    CfrImgStore *store = vt->images;
    int idx = lt_index_of(vt, rec);
    if (!store || idx < 0)
        return;
    uint64_t handle = store->imgs[idx].handle;
    int cols, rows;
    lt_cell_box(vt, rec, &cols, &rows);

    for (int i = 0; i < store->place_count; i++) {
        CfrPlacement *pl = &store->places[i];
        if (pl->image_handle != handle)
            continue;
        lt_damage_placement(vt, i);
        pl->abs_line = pl->requested_abs_line;
        pl->col = pl->requested_col;
        if (rec->p_rows > 0)
            pl->abs_line += (rec->p_rows - rows) / 2;
        if (rec->p_cols > 0)
            pl->col += (rec->p_cols - cols) / 2;
        if (pl->abs_line < vt->sixel_abs_top)
            pl->abs_line = vt->sixel_abs_top;
        if (pl->col < 0)
            pl->col = 0;
        pl->rows = rows;
        pl->cols = cols;
        lt_damage_placement(vt, i);
    }
}

static void lt_recompute_size(CfrTerm *vt, LtRec *rec)
{
    double scale = lt_compute_scale(rec->design_w, rec->design_h,
                                    rec->p_width, rec->p_height,
                                    rec->p_cols, rec->p_rows,
                                    vt->cell_w_px, vt->cell_h_px,
                                    rec->fit, rec->explicit_scale);
    int px_w = (int)((double)rec->design_w * scale + 0.5);
    int px_h = (int)((double)rec->design_h * scale + 0.5);
    if (px_w < 1)
        px_w = 1;
    if (px_h < 1)
        px_h = 1;
    bool changed = px_w != rec->px_w || px_h != rec->px_h;
    rec->px_w = px_w;
    rec->px_h = px_h;

    int idx = lt_index_of(vt, rec);
    if (idx >= 0) {
        vt->images->imgs[idx].w = px_w;
        vt->images->imgs[idx].h = px_h;
    }
    if (changed)
        lt_touch_raster(vt, rec);
    lt_relayout_placements(vt, rec);
}

/* ------------------------------------------------------------------ */
/* Command handlers                                                   */
/* ------------------------------------------------------------------ */

static void lt_cmd_load(struct CfrLottieState *st, CfrTerm *vt,
                        const char *json, size_t json_len)
{
    size_t vlen;
    const char *val;

    val = lt_json_find_key(json, json_len, "id", &vlen);
    if (!val)
        return;
    uint64_t id = (uint64_t)lt_json_int(val, vlen);

    /* Parse report flag (default false — opt-in) */
    bool want_report = false;
    val = lt_json_find_key(json, json_len, "report", &vlen);
    if (val)
        want_report = lt_json_bool(val, vlen);

    const char *lottie_obj = lt_json_find_key(json, json_len, "lottie", &vlen);
    size_t lottie_obj_len = vlen;

    uint8_t layer = 0;
    val = lt_json_find_key(json, json_len, "layer", &vlen);
    if (val && vlen >= 12 && memcmp(val, "\"background\"", 12) == 0)
        layer = 1;

    uint8_t opacity = 255;
    val = lt_json_find_key(json, json_len, "opacity", &vlen);
    if (val)
        opacity = (uint8_t)(lt_json_double(val, vlen) * 255.0 + 0.5);

    int prow = -1, pcol = -1;
    int p_cols = 0, p_rows = 0;
    int p_width = 0, p_height = 0;
    const char *placement = lt_json_find_key(json, json_len, "placement", &vlen);
    if (placement) {
        const char *pv;
        size_t pvlen;
        pv = lt_json_find_key(placement, vlen, "row", &pvlen);
        if (pv)
            prow = (int)lt_json_int(pv, pvlen);
        pv = lt_json_find_key(placement, vlen, "col", &pvlen);
        if (pv)
            pcol = (int)lt_json_int(pv, pvlen);
        pv = lt_json_find_key(placement, vlen, "cols", &pvlen);
        if (pv)
            p_cols = (int)lt_json_int(pv, pvlen);
        pv = lt_json_find_key(placement, vlen, "rows", &pvlen);
        if (pv)
            p_rows = (int)lt_json_int(pv, pvlen);
        pv = lt_json_find_key(placement, vlen, "width", &pvlen);
        if (pv)
            p_width = (int)lt_json_int(pv, pvlen);
        pv = lt_json_find_key(placement, vlen, "height", &pvlen);
        if (pv)
            p_height = (int)lt_json_int(pv, pvlen);
    }

    if (prow < 0)
        prow = vt->cursor.row;
    if (pcol < 0)
        pcol = vt->cursor.col;

    /* Parse fit mode (default "contain") and explicit scale */
    int fit = LT_FIT_CONTAIN;
    val = lt_json_find_key(json, json_len, "fit", &vlen);
    if (val && vlen >= 6 && memcmp(val, "\"none\"", 6) == 0)
        fit = LT_FIT_NONE;
    double explicit_scale = 1.0;
    val = lt_json_find_key(json, json_len, "scale", &vlen);
    if (val)
        explicit_scale = lt_json_double(val, vlen);
    if (explicit_scale <= 0.0)
        explicit_scale = 1.0;

    int design_w = 0, design_h = 0;
    int frame_ip = 0, frame_op = 30;
    double frame_fr = 30.0;
    if (lottie_obj) {
        const char *lv;
        size_t lvlen;
        lv = lt_json_find_key(lottie_obj, lottie_obj_len, "w", &lvlen);
        if (lv)
            design_w = (int)lt_json_int(lv, lvlen);
        lv = lt_json_find_key(lottie_obj, lottie_obj_len, "h", &lvlen);
        if (lv)
            design_h = (int)lt_json_int(lv, lvlen);
        lv = lt_json_find_key(lottie_obj, lottie_obj_len, "ip", &lvlen);
        if (lv)
            frame_ip = (int)lt_json_int(lv, lvlen);
        lv = lt_json_find_key(lottie_obj, lottie_obj_len, "op", &lvlen);
        if (lv)
            frame_op = (int)lt_json_int(lv, lvlen);
        lv = lt_json_find_key(lottie_obj, lottie_obj_len, "fr", &lvlen);
        if (lv)
            frame_fr = lt_json_double(lv, lvlen);
    }

    /* Compute aspect-correct rasterization size from constraints.
     * Every pixel quantity is device pixels: the region's px hints are
     * device px and cols/rows multiply the physical cell, so the
     * rasterization size is device px too — drawn 1:1. */
    double scale = lt_compute_scale(design_w, design_h,
                                    p_width, p_height,
                                    p_cols, p_rows,
                                    vt->cell_w_px, vt->cell_h_px,
                                    fit, explicit_scale);
    int px_w = (int)((double)design_w * scale + 0.5);
    int px_h = (int)((double)design_h * scale + 0.5);
    if (px_w < 1)
        px_w = 1;
    if (px_h < 1)
        px_h = 1;

    /* Ensure design space has a fallback too */
    if (design_w <= 0)
        design_w = px_w;
    if (design_h <= 0)
        design_h = px_h;

    /* Placement cells: the rasterization size in device pixels against
     * the physical cell — the same unit, a plain ceiling division. */
    int pcols = (px_w + vt->cell_w_px - 1) / vt->cell_w_px;
    int prows = (px_h + vt->cell_h_px - 1) / vt->cell_h_px;
    if (pcols < 1)
        pcols = 1;
    if (prows < 1)
        prows = 1;

    int requested_row = prow;
    int requested_col = pcol;

    /* Center cell box within the placement region */
    if (p_cols > 0)
        pcol += (p_cols - pcols) / 2;
    if (p_rows > 0)
        prow += (p_rows - prows) / 2;
    if (prow < 0)
        prow = 0;
    if (pcol < 0)
        pcol = 0;

    bool autostart = true;
    double speed = 1.0;
    bool loop = true;
    const char *play = lt_json_find_key(json, json_len, "play", &vlen);
    if (play) {
        const char *pv;
        size_t pvlen;
        pv = lt_json_find_key(play, vlen, "speed", &pvlen);
        if (pv)
            speed = lt_json_double(pv, pvlen);
        pv = lt_json_find_key(play, vlen, "loop", &pvlen);
        if (pv)
            loop = lt_json_bool(pv, pvlen);
        pv = lt_json_find_key(play, vlen, "autostart", &pvlen);
        if (pv)
            autostart = lt_json_bool(pv, pvlen);
    }

    if (!lottie_obj)
        return;

    uint8_t *doc = cfr_alloc(vt, lottie_obj_len + 1);
    if (!doc)
        return;
    memcpy(doc, lottie_obj, lottie_obj_len);
    doc[lottie_obj_len] = '\0';

    CfrImgStore *store = lt_store(vt);
    if (!store) {
        cfr_dealloc(vt, doc);
        return;
    }

    LtRec *rec = lt_find_by_id(vt, st, id);
    int idx;
    if (rec) {
        idx = lt_index_of(vt, rec);
        rec->definition_rev++;
        cfr_dealloc(vt, rec->json);
    } else {
        rec = cfr_alloc(vt, sizeof(*rec));
        if (!rec) {
            cfr_dealloc(vt, doc);
            return;
        }
        memset(rec, 0, sizeof(*rec));
        rec->definition_rev = 1;
        idx = cfr_img_add_object(vt, store, id, layer, IMG_SRC_LOTTIE,
                                 rec, lt_rec_free);
        if (idx < 0) {
            cfr_dealloc(vt, rec);
            cfr_dealloc(vt, doc);
            return;
        }
    }

    rec->json = doc;
    rec->json_len = lottie_obj_len;
    rec->design_w = design_w;
    rec->design_h = design_h;
    rec->px_w = px_w;
    rec->px_h = px_h;
    rec->p_width = p_width;
    rec->p_height = p_height;
    rec->p_cols = p_cols;
    rec->p_rows = p_rows;
    rec->explicit_scale = explicit_scale;
    rec->fit = fit;
    rec->frame_ip = frame_ip;
    rec->frame_op = frame_op;
    rec->frame_fr = frame_fr;
    rec->current_frame = frame_ip;
    rec->speed = speed;
    rec->loop = loop;
    rec->playing = autostart;
    lt_anchor_at_frame(rec);

    store->imgs[idx].w = px_w;
    store->imgs[idx].h = px_h;
    store->imgs[idx].layer = layer;
    lt_touch_raster(vt, rec);
    lt_relayout_placements(vt, rec);

    lt_add_placement(vt, rec,
                     vt->sixel_abs_top + requested_row, requested_col,
                     vt->sixel_abs_top + prow, pcol,
                     prows, pcols, layer, opacity);

    if (want_report)
        lt_emit_report(vt, id, prow, pcol, prows, pcols, px_w, px_h);
}

static void lt_cmd_place(struct CfrLottieState *st, CfrTerm *vt,
                         const char *json, size_t json_len)
{
    size_t vlen;
    const char *val;

    val = lt_json_find_key(json, json_len, "id", &vlen);
    if (!val)
        return;
    uint64_t id = (uint64_t)lt_json_int(val, vlen);

    /* Parse report flag (default false — opt-in) */
    bool want_report = false;
    val = lt_json_find_key(json, json_len, "report", &vlen);
    if (val)
        want_report = lt_json_bool(val, vlen);

    LtRec *rec = lt_find_by_id(vt, st, id);
    if (!rec)
        return;

    uint8_t layer = 0;
    val = lt_json_find_key(json, json_len, "layer", &vlen);
    if (val && vlen >= 12 && memcmp(val, "\"background\"", 12) == 0)
        layer = 1;

    uint8_t opacity = 255;
    val = lt_json_find_key(json, json_len, "opacity", &vlen);
    if (val)
        opacity = (uint8_t)(lt_json_double(val, vlen) * 255.0 + 0.5);

    /* Parse placement position and region */
    const char *placement = lt_json_find_key(json, json_len, "placement", &vlen);
    int prow = vt->cursor.row, pcol = vt->cursor.col;
    int p_cols = 0, p_rows = 0, p_width = 0, p_height = 0;
    if (placement) {
        const char *pv;
        size_t pvlen;
        pv = lt_json_find_key(placement, vlen, "row", &pvlen);
        if (pv)
            prow = (int)lt_json_int(pv, pvlen);
        pv = lt_json_find_key(placement, vlen, "col", &pvlen);
        if (pv)
            pcol = (int)lt_json_int(pv, pvlen);
        pv = lt_json_find_key(placement, vlen, "cols", &pvlen);
        if (pv)
            p_cols = (int)lt_json_int(pv, pvlen);
        pv = lt_json_find_key(placement, vlen, "rows", &pvlen);
        if (pv)
            p_rows = (int)lt_json_int(pv, pvlen);
        pv = lt_json_find_key(placement, vlen, "width", &pvlen);
        if (pv)
            p_width = (int)lt_json_int(pv, pvlen);
        pv = lt_json_find_key(placement, vlen, "height", &pvlen);
        if (pv)
            p_height = (int)lt_json_int(pv, pvlen);
    }

    /* Parse size constraints (default: keep current values) */
    int new_w = p_width ? p_width : rec->p_width;
    int new_h = p_height ? p_height : rec->p_height;
    int new_cols = p_cols ? p_cols : rec->p_cols;
    int new_rows = p_rows ? p_rows : rec->p_rows;
    int new_fit = rec->fit;
    double new_scale = rec->explicit_scale;

    val = lt_json_find_key(json, json_len, "fit", &vlen);
    if (val && vlen >= 6 && memcmp(val, "\"none\"", 6) == 0)
        new_fit = LT_FIT_NONE;
    else if (val && vlen >= 9 && memcmp(val, "\"contain\"", 9) == 0)
        new_fit = LT_FIT_CONTAIN;
    val = lt_json_find_key(json, json_len, "scale", &vlen);
    if (val)
        new_scale = lt_json_double(val, vlen);
    if (new_scale <= 0.0)
        new_scale = 1.0;

    /* Recompute rasterization size if constraints or fit changed */
    bool size_changed = (new_w != rec->p_width ||
                         new_h != rec->p_height ||
                         new_cols != rec->p_cols ||
                         new_rows != rec->p_rows ||
                         new_fit != rec->fit ||
                         new_scale != rec->explicit_scale);
    if (size_changed) {
        rec->p_width = new_w;
        rec->p_height = new_h;
        rec->p_cols = new_cols;
        rec->p_rows = new_rows;
        rec->fit = new_fit;
        rec->explicit_scale = new_scale;
        lt_recompute_size(vt, rec);
    }

    /* Compute placement cells from current rasterization size.
     * px_w/px_h are device pixels, the same unit as the physical cell. */
    int pcols = (rec->px_w + vt->cell_w_px - 1) / vt->cell_w_px;
    int prows = (rec->px_h + vt->cell_h_px - 1) / vt->cell_h_px;
    if (pcols < 1)
        pcols = 1;
    if (prows < 1)
        prows = 1;

    int requested_row = prow;
    int requested_col = pcol;

    /* Center cell box within the placement region */
    if (new_cols > 0)
        pcol += (new_cols - pcols) / 2;
    if (new_rows > 0)
        prow += (new_rows - prows) / 2;
    if (prow < 0)
        prow = 0;
    if (pcol < 0)
        pcol = 0;

    lt_add_placement(vt, rec,
                     vt->sixel_abs_top + requested_row, requested_col,
                     vt->sixel_abs_top + prow, pcol,
                     prows, pcols, layer, opacity);

    if (want_report)
        lt_emit_report(vt, id, prow, pcol, prows, pcols, rec->px_w, rec->px_h);
}

static void lt_cmd_play(struct CfrLottieState *st, CfrTerm *vt,
                        const char *json, size_t json_len)
{
    size_t vlen;
    const char *val;

    val = lt_json_find_key(json, json_len, "id", &vlen);
    if (!val)
        return;
    uint64_t id = (uint64_t)lt_json_int(val, vlen);

    LtRec *rec = lt_find_by_id(vt, st, id);
    if (!rec)
        return;

    /* A play on a running animation (a speed or loop change) continues from
     * the exact position of the last present; otherwise it starts from the
     * frame on screen. Either way the next present sets the anchor. */
    if (rec->playing && rec->anchored)
        lt_anchor_keep_position(rec);
    else
        lt_anchor_at_frame(rec);
    rec->playing = true;

    val = lt_json_find_key(json, json_len, "speed", &vlen);
    if (val)
        rec->speed = lt_json_double(val, vlen);

    val = lt_json_find_key(json, json_len, "loop", &vlen);
    if (val)
        rec->loop = lt_json_bool(val, vlen);
}

static void lt_cmd_pause(struct CfrLottieState *st, CfrTerm *vt,
                         const char *json, size_t json_len)
{
    size_t vlen;
    const char *val;

    val = lt_json_find_key(json, json_len, "id", &vlen);
    if (!val)
        return;
    uint64_t id = (uint64_t)lt_json_int(val, vlen);

    LtRec *rec = lt_find_by_id(vt, st, id);
    if (!rec)
        return;

    rec->playing = false;
    lt_anchor_at_frame(rec);
}

static void lt_cmd_stop(struct CfrLottieState *st, CfrTerm *vt,
                        const char *json, size_t json_len)
{
    size_t vlen;
    const char *val;

    val = lt_json_find_key(json, json_len, "id", &vlen);
    if (!val)
        return;
    uint64_t id = (uint64_t)lt_json_int(val, vlen);

    LtRec *rec = lt_find_by_id(vt, st, id);
    if (!rec)
        return;

    rec->playing = false;
    if (rec->current_frame != rec->frame_ip) {
        rec->current_frame = rec->frame_ip;
        lt_touch_raster(vt, rec);
        lt_damage_all_placements(vt, rec);
    }
    lt_anchor_at_frame(rec);
}

static void lt_cmd_seek(struct CfrLottieState *st, CfrTerm *vt,
                        const char *json, size_t json_len)
{
    size_t vlen;
    const char *val;

    val = lt_json_find_key(json, json_len, "id", &vlen);
    if (!val)
        return;
    uint64_t id = (uint64_t)lt_json_int(val, vlen);

    LtRec *rec = lt_find_by_id(vt, st, id);
    if (!rec)
        return;

    val = lt_json_find_key(json, json_len, "frame", &vlen);
    if (!val)
        return;
    int frame = (int)lt_json_int(val, vlen);
    if (frame < rec->frame_ip)
        frame = rec->frame_ip;
    if (frame >= rec->frame_op)
        frame = rec->frame_op - 1;

    if (rec->current_frame != frame) {
        rec->current_frame = frame;
        lt_touch_raster(vt, rec);
        lt_damage_all_placements(vt, rec);
    }
    lt_anchor_at_frame(rec);
}

static void lt_cmd_delete(struct CfrLottieState *st, CfrTerm *vt,
                          const char *json, size_t json_len)
{
    size_t vlen;
    const char *val;

    val = lt_json_find_key(json, json_len, "id", &vlen);
    if (!val)
        return;
    uint64_t id = (uint64_t)lt_json_int(val, vlen);

    LtRec *rec = lt_find_by_id(vt, st, id);
    if (!rec)
        return;

    int idx = lt_index_of(vt, rec);
    if (idx >= 0)
        cfr_img_remove(vt, vt->images, idx);
}

/* ------------------------------------------------------------------ */
/* Chunked upload                                                     */
/* ------------------------------------------------------------------ */

static LtChunkAccum *lt_find_chunk(struct CfrLottieState *st, uint64_t id)
{
    for (int i = 0; i < st->chunk_count; i++) {
        if (st->chunks[i].id == id)
            return &st->chunks[i];
    }
    return NULL;
}

static LtChunkAccum *lt_chunk_alloc(struct CfrLottieState *st, CfrTerm *vt,
                                    uint64_t id)
{
    if (st->chunk_count >= st->chunk_cap) {
        int new_cap = st->chunk_cap ? st->chunk_cap * 2 : 4;
        LtChunkAccum *new_chunks = cfr_alloc(vt, (size_t)new_cap * sizeof(LtChunkAccum));
        if (!new_chunks)
            return NULL;
        if (st->chunks) {
            memcpy(new_chunks, st->chunks,
                   (size_t)st->chunk_count * sizeof(LtChunkAccum));
            cfr_dealloc(vt, st->chunks);
        }
        st->chunks = new_chunks;
        st->chunk_cap = new_cap;
    }
    LtChunkAccum *c = &st->chunks[st->chunk_count++];
    memset(c, 0, sizeof(*c));
    c->id = id;
    return c;
}

static void lt_chunk_discard(CfrTerm *vt, struct CfrLottieState *st,
                             LtChunkAccum *c)
{
    if (c->buf)
        cfr_dealloc(vt, c->buf);
    *c = st->chunks[--st->chunk_count];
}

static void lt_cmd_load_chunk(struct CfrLottieState *st, CfrTerm *vt,
                              const char *json, size_t json_len)
{
    size_t vlen;
    const char *val;

    val = lt_json_find_key(json, json_len, "id", &vlen);
    if (!val)
        return;
    uint64_t id = (uint64_t)lt_json_int(val, vlen);

    val = lt_json_find_key(json, json_len, "seq", &vlen);
    if (!val)
        return;
    int seq = (int)lt_json_int(val, vlen);

    val = lt_json_find_key(json, json_len, "total", &vlen);
    if (!val)
        return;
    int total = (int)lt_json_int(val, vlen);
    if (total <= 0 || seq < 0 || seq >= total)
        return;

    const char *data_val = lt_json_find_key(json, json_len, "data", &vlen);
    if (!data_val)
        return;

    /* Strip surrounding quotes from the string value */
    const uint8_t *b64_in = (const uint8_t *)data_val;
    size_t b64_len = vlen;
    if (b64_len >= 2 && b64_in[0] == '"' && b64_in[b64_len - 1] == '"') {
        b64_in++;
        b64_len -= 2;
    }

    LtChunkAccum *c = lt_find_chunk(st, id);
    if (seq == 0) {
        if (c)
            lt_chunk_discard(vt, st, c);
        c = lt_chunk_alloc(st, vt, id);
        if (!c)
            return;
        c->chunks_total = total;
    } else {
        if (!c || c->chunks_total != total)
            return;
    }

    size_t dec_len = 0;
    uint8_t *decoded = cfr_base64_decode(vt, (const char *)b64_in, b64_len,
                                         &dec_len);
    if (!decoded) {
        lt_chunk_discard(vt, st, c);
        return;
    }
    if (cfr_buf_append(vt, &c->buf, &c->buf_len, &c->buf_cap,
                       decoded, dec_len) < 0) {
        cfr_dealloc(vt, decoded);
        lt_chunk_discard(vt, st, c);
        return;
    }
    c->chunks_received++;
    cfr_dealloc(vt, decoded);

    if (c->chunks_received == c->chunks_total) {
        char *full_json = (char *)c->buf;
        size_t full_len = c->buf_len;
        full_json[full_len] = '\0';

        /* Build a synthetic load command wrapping the concatenated Lottie JSON */
        size_t synth_cap = full_len + 64;
        char *synth = cfr_alloc(vt, synth_cap);
        if (synth) {
            int n = snprintf(synth, synth_cap,
                             "{\"cmd\":\"load\",\"id\":%llu,\"lottie\":",
                             (unsigned long long)id);
            if (n > 0 && (size_t)n < synth_cap) {
                memcpy(synth + n, full_json, full_len);
                size_t synth_len = (size_t)n + full_len;
                /* Close the outer JSON object */
                if (synth_len + 2 <= synth_cap) {
                    synth[synth_len++] = '}';
                    synth[synth_len] = '\0';
                    lt_cmd_load(st, vt, synth, synth_len);
                }
            }
            cfr_dealloc(vt, synth);
        }

        lt_chunk_discard(vt, st, c);
    }
}

/* ------------------------------------------------------------------ */
/* APC dispatch                                                       */
/* ------------------------------------------------------------------ */

void cfr_lottie_apc_dispatch(CfrTerm *vt, const uint8_t *body, size_t body_len)
{
    struct CfrLottieState *st = lt_state(vt);
    if (!st)
        return;

    size_t dec_len = 0;
    uint8_t *decoded = cfr_base64_decode(vt, (const char *)body, body_len,
                                         &dec_len);
    if (!decoded)
        return;

    size_t cmd_len;
    const char *cmd = lt_json_find_key((const char *)decoded, dec_len,
                                       "cmd", &cmd_len);
    if (!cmd) {
        cfr_dealloc(vt, decoded);
        return;
    }

    if (cmd_len == 6 && memcmp(cmd, "\"load\"", 6) == 0)
        lt_cmd_load(st, vt, (const char *)decoded, dec_len);
    else if (cmd_len == 7 && memcmp(cmd, "\"place\"", 7) == 0)
        lt_cmd_place(st, vt, (const char *)decoded, dec_len);
    else if (cmd_len == 6 && memcmp(cmd, "\"play\"", 6) == 0)
        lt_cmd_play(st, vt, (const char *)decoded, dec_len);
    else if (cmd_len == 7 && memcmp(cmd, "\"pause\"", 7) == 0)
        lt_cmd_pause(st, vt, (const char *)decoded, dec_len);
    else if (cmd_len == 6 && memcmp(cmd, "\"stop\"", 6) == 0)
        lt_cmd_stop(st, vt, (const char *)decoded, dec_len);
    else if (cmd_len == 6 && memcmp(cmd, "\"seek\"", 6) == 0)
        lt_cmd_seek(st, vt, (const char *)decoded, dec_len);
    else if (cmd_len == 8 && memcmp(cmd, "\"delete\"", 8) == 0)
        lt_cmd_delete(st, vt, (const char *)decoded, dec_len);
    else if (cmd_len == 12 && memcmp(cmd, "\"load-chunk\"", 12) == 0)
        lt_cmd_load_chunk(st, vt, (const char *)decoded, dec_len);

    cfr_dealloc(vt, decoded);
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

bool cfr_lottie_present(CfrTerm *vt, uint64_t present_us, uint64_t hold_us)
{
    if (!vt || !vt->images)
        return false;

    /* The frame is on screen for [present_us, present_us + hold_us); sample
     * its middle. A frame boundary that coincides with a refresh (30 fps on
     * a 60 Hz display: every one does) is then half a refresh from the
     * rounding edge instead of exactly on it. */
    const uint64_t sample_us = present_us + hold_us / 2;
    bool any_advanced = false;

    for (int i = 0; i < vt->images->img_count; i++) {
        CfrImg *img = &vt->images->imgs[i];
        if (img->source != IMG_SRC_LOTTIE)
            continue;
        LtRec *r = img->source_state;
        if (!r || !r->playing)
            continue;

        const double span = (double)(r->frame_op - r->frame_ip);
        if (r->speed <= 0.0 || r->frame_fr <= 0.0 || span <= 0.0)
            continue;

        if (!r->anchored) {
            r->anchor_us = present_us;
            r->anchored = true;
        }

        const uint64_t dt = sample_us > r->anchor_us ? sample_us - r->anchor_us : 0;
        double pos = r->pos0 + (double)dt * r->speed * r->frame_fr / 1e6;
        if (pos >= span) {
            if (r->loop) {
                pos = fmod(pos, span);
            } else {
                pos = span;
                r->playing = false;
            }
        }
        r->last_pos = pos;

        int frame_idx = (int)pos;
        if (frame_idx >= (int)span)
            frame_idx = (int)span - 1;
        int new_frame = r->frame_ip + frame_idx;

        if (new_frame != r->current_frame) {
            r->current_frame = new_frame;
            img->version = ++vt->images->version_seq;
            lt_damage_all_placements(vt, r);
            any_advanced = true;
        }
    }

    return any_advanced;
}

int cfr_lottie_playing_count(CfrTerm *vt)
{
    if (!vt || !vt->images)
        return 0;
    int n = 0;
    for (int i = 0; i < vt->images->img_count; i++) {
        CfrImg *img = &vt->images->imgs[i];
        if (img->source == IMG_SRC_LOTTIE && img->source_state) {
            LtRec *rec = img->source_state;
            if (rec->playing)
                n++;
        }
    }
    return n;
}

/* Count placements by their stable object handle, not the protocol id. */
static int lt_placements_for_count(CfrTerm *vt, uint64_t handle)
{
    CfrImgStore *store = vt->images;
    if (!store)
        return 0;
    int n = 0;
    for (int i = 0; i < store->place_count; i++)
        if (store->places[i].source == IMG_SRC_LOTTIE &&
            store->places[i].image_handle == handle)
            n++;
    return n;
}

const CfrLottie *cfr_get_lotties(CfrTerm *vt, int *out_count)
{
    if (out_count)
        *out_count = 0;
    if (!vt || !vt->lottie || !vt->images)
        return NULL;

    struct CfrLottieState *st = vt->lottie;
    CfrImgStore *store = vt->images;
    uint8_t active = (uint8_t)vt->in_altscreen;
    int want = 0;
    for (int i = 0; i < store->img_count; i++)
        if (store->imgs[i].source == IMG_SRC_LOTTIE &&
            store->imgs[i].screen == active)
            want++;
    if (want == 0)
        return NULL;

    if (st->scratch_cap < want) {
        int ncap = st->scratch_cap ? st->scratch_cap * 2 : 8;
        while (ncap < want)
            ncap *= 2;
        CfrLottie *ns = cfr_realloc(vt, st->scratch,
                                    (size_t)ncap * sizeof(*ns));
        if (!ns)
            return NULL;
        st->scratch = ns;
        st->scratch_cap = ncap;
    }

    int n = 0;
    for (int i = 0; i < store->img_count; i++) {
        CfrImg *img = &store->imgs[i];
        if (img->source != IMG_SRC_LOTTIE || img->screen != active)
            continue;
        LtRec *r = img->source_state;
        if (!r)
            continue;
        CfrLottie *l = &st->scratch[n++];
        memset(l, 0, sizeof(*l));
        l->handle = img->handle;
        l->id = img->id;
        l->definition_rev = r->definition_rev;
        l->raster_rev = img->version;
        l->json = r->json;
        l->json_len = r->json_len;
        l->px_w = r->px_w;
        l->px_h = r->px_h;
        l->current_frame = r->current_frame;
        l->frame_count = r->frame_op - r->frame_ip;
        l->playing = r->playing;
        l->speed = r->speed;
        l->loop = r->loop;
        l->placement_count = lt_placements_for_count(vt, img->handle);
    }

    if (out_count)
        *out_count = n;
    return n ? st->scratch : NULL;
}

/* Cell metrics are inputs to Lottie layout. Recompute every animation so the
 * host receives a new raster revision when its device-pixel size changes. */
void cfr_lottie_cell_pixels_changed(CfrTerm *vt)
{
    if (!vt || !vt->images)
        return;
    for (int i = 0; i < vt->images->img_count; i++) {
        CfrImg *img = &vt->images->imgs[i];
        if (img->source == IMG_SRC_LOTTIE && img->source_state)
            lt_recompute_size(vt, img->source_state);
    }
}

void cfr_lottie_clear_display_rows(CfrTerm *vt, int top, int bot)
{
    if (vt && vt->images)
        cfr_img_clear_source_rows(vt, vt->images, IMG_SRC_LOTTIE, top, bot);
}

void cfr_lottie_clear_all(CfrTerm *vt)
{
    if (!vt || !vt->images)
        return;
    for (int i = 0; i < vt->images->img_count;) {
        if (vt->images->imgs[i].source == IMG_SRC_LOTTIE)
            cfr_img_remove(vt, vt->images, i);
        else
            i++;
    }
}

void cfr_lottie_state_free(CfrTerm *vt)
{
    struct CfrLottieState *st = vt->lottie;
    if (!st)
        return;

    for (int i = 0; i < st->chunk_count; i++)
        cfr_dealloc(vt, st->chunks[i].buf);
    cfr_dealloc(vt, st->chunks);
    cfr_dealloc(vt, st->scratch);

    cfr_dealloc(vt, st);
    vt->lottie = NULL;
}

int cfr_lottie_active_count(CfrTerm *vt)
{
    if (!vt || !vt->images)
        return 0;
    int n = 0;
    for (int i = 0; i < vt->images->img_count; i++)
        if (vt->images->imgs[i].source == IMG_SRC_LOTTIE)
            n++;
    return n;
}
