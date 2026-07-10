/* lottie.c — Lottie animation subsystem for coffer.
 *
 * Parses APC sequences (ESC _ ... ST), manages animation state, rasterizes
 * frames via ThorVG, and exposes RGBA pixel data through cfr_get_lotties().
 *
 * Architecture mirrors sixel.c: engine owns all pixel data, host does
 * texture cache + compositing only. */

#include "coffer_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_THORVG
#include <thorvg_capi.h>
#endif

/* ------------------------------------------------------------------ */
/* Constants                                                          */
/* ------------------------------------------------------------------ */

#define LT_MAX_ANIMS      64
#define LT_MAX_PLACEMENTS 32
#define LT_SPARE_MAX      16
#define LT_LIVE_MAX       (128 * 1024 * 1024)
#define LT_RETAIN_MAX     (64 * 1024 * 1024)

/* ------------------------------------------------------------------ */
/* Types — all defined up front so functions can reference them       */
/* ------------------------------------------------------------------ */

typedef struct
{
    uint8_t *buf;
    size_t cap;
} LtSpare;

enum
{
    LT_FIT_CONTAIN = 0, /* default — scale to fit within region */
    LT_FIT_NONE = 1,    /* no auto-scaling — use explicit scale or design size */
};

typedef struct
{
    uint64_t id;      /* client-assigned, stable cache key */
    uint32_t version; /* bumps on any state/pixel change */

    /* Parsed Lottie data */
    void *json_root; /* parsed JSON tree root (arena-allocated) */

    /* Rasterized pixel buffer (engine-owned, like SxRec.rgba) */
    uint8_t *rgba;   /* RGBA32 pixel data for current frame */
    size_t rgba_cap; /* allocated bytes */

    /* Design space from Lottie JSON (w, h fields) */
    int design_w;
    int design_h;

    /* Rasterization pixel dimensions (design × scale, not cell box) */
    int px_w;
    int px_h;

    /* Placement region (from load/place commands) */
    int p_width;           /* pixel region width, 0 = no limit */
    int p_height;          /* pixel region height, 0 = no limit */
    int p_cols;            /* cell region width, 0 = no limit */
    int p_rows;            /* cell region height, 0 = no limit */
    double explicit_scale; /* user scale, only used with fit:"none" (default 1.0) */
    int fit;               /* LT_FIT_CONTAIN or LT_FIT_NONE */

    /* Playback state */
    int current_frame;
    int frame_ip;    /* Lottie in-point */
    int frame_op;    /* Lottie out-point */
    double frame_fr; /* Lottie framerate */
    double speed;
    bool playing;
    bool loop;
    bool dirty; /* frame changed, rgba needs re-upload */

    /* Timing */
    uint64_t last_tick_us;

    /* Placements */
    CfrLottiePlacement *placements;
    int placement_count;
    int placement_cap;

    /* ThorVG rasterization state (only when HAVE_THORVG) */
#ifdef HAVE_THORVG
    Tvg_Animation tvg_anim;
    Tvg_Canvas tvg_canvas;
#endif

    /* Arena for JSON parse tree */
    uint8_t *arena_base;
    size_t arena_offset;
    size_t arena_cap;
} LtRec;

/* Chunked upload accumulator */
typedef struct
{
    uint64_t id;
    uint8_t *buf;
    size_t buf_len;
    size_t buf_cap;
    int chunks_received;
    int chunks_total;
} LtChunkAccum;

/* Global Lottie state (analogous to CfrSixelState) */
struct CfrLottieState
{
    LtRec *recs;
    int rec_count;
    int rec_cap;
    uint64_t next_placement_id;
    size_t live_bytes;

    LtChunkAccum *chunks;
    int chunk_count;
    int chunk_cap;

    /* Spare buffer pool for rgba pixel buffers */
    LtSpare spares[LT_SPARE_MAX];
    int spare_count;
    size_t retain_bytes;

    /* Scratch buffer for cfr_get_lotties() snapshots */
    uint8_t *scratch;
    size_t scratch_cap;

    /* Scratch buffer for cfr_get_lottie_placements() snapshots */
    CfrLottiePlacement *pl_scratch;
    int pl_scratch_cap;
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

/* ------------------------------------------------------------------ */
/* Lazy allocation                                                    */
/* ------------------------------------------------------------------ */

static struct CfrLottieState *lt_state(CfrTerm *vt)
{
    if (!vt->lottie) {
#ifdef HAVE_THORVG
        tvg_engine_init(0);
#endif
        vt->lottie = cfr_alloc(vt, sizeof(*vt->lottie));
        if (!vt->lottie)
            return NULL;
        memset(vt->lottie, 0, sizeof(*vt->lottie));
    }
    return vt->lottie;
}

/* ------------------------------------------------------------------ */
/* Buffer pool (mirrors SxSpare)                                      */
/* ------------------------------------------------------------------ */

static uint8_t *lt_buf_alloc(CfrTerm *vt, struct CfrLottieState *st,
                             size_t need)
{
    int best = -1;
    for (int i = 0; i < st->spare_count; i++) {
        if (st->spares[i].cap >= need) {
            if (best < 0 || st->spares[i].cap < st->spares[best].cap)
                best = i;
        }
    }
    if (best >= 0) {
        uint8_t *buf = st->spares[best].buf;
        size_t cap = st->spares[best].cap;
        st->spares[best] = st->spares[--st->spare_count];
        st->retain_bytes -= cap;
        return buf;
    }
    return cfr_alloc(vt, need);
}

static void lt_buf_release(CfrTerm *vt, struct CfrLottieState *st,
                           uint8_t *buf, size_t cap)
{
    if (st->spare_count < LT_SPARE_MAX &&
        st->retain_bytes + cap <= LT_RETAIN_MAX) {
        st->spares[st->spare_count].buf = buf;
        st->spares[st->spare_count].cap = cap;
        st->spare_count++;
        st->retain_bytes += cap;
    } else {
        cfr_dealloc(vt, buf);
    }
}

/* ------------------------------------------------------------------ */
/* Record management                                                  */
/* ------------------------------------------------------------------ */

static LtRec *lt_find_by_id(struct CfrLottieState *st, uint64_t id)
{
    for (int i = 0; i < st->rec_count; i++) {
        if (st->recs[i].id == id)
            return &st->recs[i];
    }
    return NULL;
}

static void lt_rec_release(CfrTerm *vt, struct CfrLottieState *st, int idx)
{
    LtRec *r = &st->recs[idx];
    if (r->rgba) {
        st->live_bytes -= r->rgba_cap;
        lt_buf_release(vt, st, r->rgba, r->rgba_cap);
    }
    if (r->arena_base)
        cfr_dealloc(vt, r->arena_base);
    if (r->placements)
        cfr_dealloc(vt, r->placements);
#ifdef HAVE_THORVG
    if (r->tvg_canvas && r->tvg_anim) {
        Tvg_Paint pic = tvg_animation_get_picture(r->tvg_anim);
        tvg_canvas_remove(r->tvg_canvas, pic);
    }
    if (r->tvg_anim)
        tvg_animation_del(r->tvg_anim);
    if (r->tvg_canvas)
        tvg_canvas_destroy(r->tvg_canvas);
    r->tvg_canvas = NULL;
    r->tvg_anim = NULL;
#endif
    st->recs[idx] = st->recs[--st->rec_count];
}

/* Evict animations until the budget can accommodate `incoming` bytes.
 * Evicts the animation whose first placement has the smallest abs_line
 * (oldest on screen), mirroring sixel's eviction policy. */
static void lt_evict_to_budget(CfrTerm *vt, struct CfrLottieState *st,
                               size_t incoming)
{
    while (st->rec_count > 0 && st->live_bytes + incoming > LT_LIVE_MAX) {
        int m = 0;
        long min_line = st->recs[0].placement_count > 0
                            ? st->recs[0].placements[0].abs_line
                            : 0;
        for (int i = 1; i < st->rec_count; ++i) {
            long line = st->recs[i].placement_count > 0
                            ? st->recs[i].placements[0].abs_line
                            : 0;
            if (line < min_line) {
                min_line = line;
                m = i;
            }
        }
        lt_rec_release(vt, st, m);
    }
}

/* ------------------------------------------------------------------ */
/* ThorVG rasterization                                                */
/* ------------------------------------------------------------------ */

#ifdef HAVE_THORVG
static uint8_t lt_srgb_lut[256];
static bool lt_srgb_lut_init = false;

static void lt_srgb_lut_ensure(void)
{
    if (lt_srgb_lut_init)
        return;
    for (int i = 0; i < 256; i++) {
        float s = (float)i / 255.0f;
        float l = s <= 0.04045f ? s / 12.92f
                                : powf((s + 0.055f) / 1.055f, 2.4f);
        int out = (int)(l * 255.0f + 0.5f);
        lt_srgb_lut[i] = (uint8_t)(out < 0 ? 0 : out > 255 ? 255
                                                           : out);
    }
    lt_srgb_lut_init = true;
}

static void lt_linearize_rgba(uint8_t *rgba, int w, int h)
{
    lt_srgb_lut_ensure();

    /* ThorVG renders to ARGB8888 with premultiplied alpha
     * (little-endian memory: B,G,R,A bytes; uint32 = A<<24|R<<16|G<<8|B).
     * The host renderer expects RGBA32 (R,G,B,A) with non-premultiplied
     * alpha, pre-linearized for the linear-light compositing path.
     * Un-premultiply, swap R<->B, linearize RGB.
     * Skip fully transparent pixels (alpha=0) — they're already zero. */
    const uint8_t *lut = lt_srgb_lut;
    size_t n = (size_t)w * (size_t)h;
    for (size_t i = 0; i < n; i++) {
        uint8_t *px = rgba + i * 4;
        uint8_t a = px[3];
        if (a == 0) {
            px[0] = px[1] = px[2] = 0;
            continue;
        }
        uint8_t b = px[0];
        uint8_t g = px[1];
        uint8_t r = px[2];
        if (a < 255) {
            r = (uint8_t)((int)r * 255 / a);
            g = (uint8_t)((int)g * 255 / a);
            b = (uint8_t)((int)b * 255 / a);
        }
        px[0] = lut[r];
        px[1] = lut[g];
        px[2] = lut[b];
    }
}

static void lt_rasterize(LtRec *r)
{
    if (!r->tvg_anim || !r->tvg_canvas)
        return;
    if (r->px_w <= 0 || r->px_h <= 0)
        return;

    Tvg_Result sf_res =
        tvg_animation_set_frame(r->tvg_anim, (float)r->current_frame);
    /* ThorVG 1.0 on Windows returns INSUFFICIENT_CONDITION on the first
     * set_frame call after loading a Lottie animation.  Performing a
     * full draw cycle at a different frame index forces ThorVG to fully
     * initialise the animation; the target frame can then be set. */
    if (sf_res != TVG_RESULT_SUCCESS) {
        float warm = (float)r->current_frame + 1.0f;
        tvg_animation_set_frame(r->tvg_anim, warm);
        tvg_canvas_update(r->tvg_canvas);
        tvg_canvas_draw(r->tvg_canvas, true);
        tvg_canvas_sync(r->tvg_canvas);
        tvg_animation_set_frame(r->tvg_anim, (float)r->current_frame);
    }
    tvg_canvas_update(r->tvg_canvas);
    tvg_canvas_draw(r->tvg_canvas, true);
    tvg_canvas_sync(r->tvg_canvas);

    lt_linearize_rgba(r->rgba, r->px_w, r->px_h);
    r->dirty = true;
}

static bool lt_tvg_init(LtRec *r)
{
    r->tvg_anim = tvg_animation_new();
    if (!r->tvg_anim)
        return false;

    Tvg_Paint picture = tvg_animation_get_picture(r->tvg_anim);
    if (!picture)
        goto fail;

    /* Load Lottie JSON from the arena buffer */
    if (r->arena_base && r->arena_offset > 0) {
        Tvg_Result res = tvg_picture_load_data(
            picture, (const char *)r->arena_base,
            (uint32_t)r->arena_offset, "lottie+json", NULL, true);
        if (res != TVG_RESULT_SUCCESS)
            goto fail;
    } else {
        goto fail;
    }

    /* Scale the animation from design space to pixel dimensions */
    if (r->design_w > 0 && r->design_h > 0 && r->px_w > 0 && r->px_h > 0)
        tvg_picture_set_size(picture, (float)r->px_w, (float)r->px_h);

    r->tvg_canvas = tvg_swcanvas_create(TVG_ENGINE_OPTION_DEFAULT);
    if (!r->tvg_canvas)
        goto fail;

    tvg_swcanvas_set_target(r->tvg_canvas, (uint32_t *)r->rgba,
                            (uint32_t)r->px_w,
                            (uint32_t)r->px_w,
                            (uint32_t)r->px_h,
                            TVG_COLORSPACE_ARGB8888);
    tvg_canvas_add(r->tvg_canvas, picture);

    /* Rasterize the initial frame */
    lt_rasterize(r);
    return true;

fail:
    if (r->tvg_anim && r->tvg_canvas) {
        Tvg_Paint pic = tvg_animation_get_picture(r->tvg_anim);
        tvg_canvas_remove(r->tvg_canvas, pic);
    }
    if (r->tvg_anim) {
        tvg_animation_del(r->tvg_anim);
        r->tvg_anim = NULL;
    }
    if (r->tvg_canvas) {
        tvg_canvas_destroy(r->tvg_canvas);
        r->tvg_canvas = NULL;
    }
    return false;
}
#else
static void lt_rasterize(LtRec *r)
{
    (void)r;
}
#endif

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
/* Base64 decoder                                                     */
/* ------------------------------------------------------------------ */

static const int8_t b64_table[256] = {
    ['A'] = 0,
    ['B'] = 1,
    ['C'] = 2,
    ['D'] = 3,
    ['E'] = 4,
    ['F'] = 5,
    ['G'] = 6,
    ['H'] = 7,
    ['I'] = 8,
    ['J'] = 9,
    ['K'] = 10,
    ['L'] = 11,
    ['M'] = 12,
    ['N'] = 13,
    ['O'] = 14,
    ['P'] = 15,
    ['Q'] = 16,
    ['R'] = 17,
    ['S'] = 18,
    ['T'] = 19,
    ['U'] = 20,
    ['V'] = 21,
    ['W'] = 22,
    ['X'] = 23,
    ['Y'] = 24,
    ['Z'] = 25,
    ['a'] = 26,
    ['b'] = 27,
    ['c'] = 28,
    ['d'] = 29,
    ['e'] = 30,
    ['f'] = 31,
    ['g'] = 32,
    ['h'] = 33,
    ['i'] = 34,
    ['j'] = 35,
    ['k'] = 36,
    ['l'] = 37,
    ['m'] = 38,
    ['n'] = 39,
    ['o'] = 40,
    ['p'] = 41,
    ['q'] = 42,
    ['r'] = 43,
    ['s'] = 44,
    ['t'] = 45,
    ['u'] = 46,
    ['v'] = 47,
    ['w'] = 48,
    ['x'] = 49,
    ['y'] = 50,
    ['z'] = 51,
    ['0'] = 52,
    ['1'] = 53,
    ['2'] = 54,
    ['3'] = 55,
    ['4'] = 56,
    ['5'] = 57,
    ['6'] = 58,
    ['7'] = 59,
    ['8'] = 60,
    ['9'] = 61,
    ['+'] = 62,
    ['/'] = 63,
};

static size_t lt_base64_decode(const uint8_t *in, size_t in_len,
                               uint8_t *out, size_t out_cap)
{
    size_t out_len = 0;
    uint32_t accum = 0;
    int bits = 0;

    for (size_t i = 0; i < in_len && out_len < out_cap; i++) {
        int8_t v = b64_table[in[i]];
        if (v < 0)
            continue;
        accum = (accum << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[out_len++] = (uint8_t)(accum >> bits);
        }
    }
    return out_len;
}

static size_t lt_base64_encode(const uint8_t *in, size_t in_len,
                               char *out, size_t out_cap)
{
    static const char chars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t out_len = 0;
    for (size_t i = 0; i < in_len; i += 3) {
        if (out_len + 4 > out_cap)
            break;
        uint32_t a = in[i];
        uint32_t b = (i + 1 < in_len) ? in[i + 1] : 0;
        uint32_t c = (i + 2 < in_len) ? in[i + 2] : 0;
        uint32_t t = (a << 16) | (b << 8) | c;
        out[out_len++] = chars[(t >> 18) & 0x3F];
        out[out_len++] = chars[(t >> 12) & 0x3F];
        out[out_len++] = (i + 1 < in_len) ? chars[(t >> 6) & 0x3F] : '=';
        out[out_len++] = (i + 2 < in_len) ? chars[t & 0x3F] : '=';
    }
    if (out_len < out_cap)
        out[out_len] = '\0';
    return out_len;
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

/* Emit an APC report (OSC 5556 on Windows) with placement info. */
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

    char b64[300];
    size_t b64_len = lt_base64_encode(
        (const uint8_t *)json, (size_t)jn, b64, sizeof(b64));
    if (b64_len == 0)
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
}

static CfrLottiePlacement *lt_add_placement(
    CfrTerm *vt, struct CfrLottieState *st, LtRec *rec,
    long abs_line, int col, int rows, int cols,
    uint8_t layer, uint8_t opacity)
{
    for (int i = 0; i < rec->placement_count; i++) {
        if (rec->placements[i].abs_line == abs_line &&
            rec->placements[i].col == col) {
            rec->placements[i].rows = rows;
            rec->placements[i].cols = cols;
            rec->placements[i].layer = layer;
            rec->placements[i].opacity_x256 = opacity;
            return &rec->placements[i];
        }
    }

    if (rec->placement_count >= rec->placement_cap) {
        if (rec->placement_cap >= LT_MAX_PLACEMENTS)
            return NULL;
        int new_cap = rec->placement_cap ? rec->placement_cap * 2 : 4;
        if (new_cap > LT_MAX_PLACEMENTS)
            new_cap = LT_MAX_PLACEMENTS;
        CfrLottiePlacement *p = cfr_alloc(vt, (size_t)new_cap * sizeof(*p));
        if (!p)
            return NULL;
        if (rec->placements) {
            memcpy(p, rec->placements,
                   (size_t)rec->placement_count * sizeof(*p));
            cfr_dealloc(vt, rec->placements);
        }
        rec->placements = p;
        rec->placement_cap = new_cap;
    }
    CfrLottiePlacement *pl = &rec->placements[rec->placement_count++];
    pl->id = ++st->next_placement_id;
    pl->abs_line = abs_line;
    pl->col = col;
    pl->rows = rows;
    pl->cols = cols;
    pl->layer = layer;
    pl->opacity_x256 = opacity;
    return pl;
}

/* ------------------------------------------------------------------ */
/* Damage helpers                                                     */
/* ------------------------------------------------------------------ */

static void lt_damage_placement(CfrTerm *vt, const CfrLottiePlacement *pl)
{
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
    for (int i = 0; i < rec->placement_count; i++)
        lt_damage_placement(vt, &rec->placements[i]);
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

    /* Compute aspect-correct rasterization size from constraints */
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

    /* Placement cells derived from rasterization size / cell px */
    int pcols = (px_w + vt->cell_w_px - 1) / vt->cell_w_px;
    int prows = (px_h + vt->cell_h_px - 1) / vt->cell_h_px;
    if (pcols < 1)
        pcols = 1;
    if (prows < 1)
        prows = 1;

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

    LtRec *rec = lt_find_by_id(st, id);
    size_t need = (size_t)px_w * (size_t)px_h * 4;
    if (need == 0 || need > LT_LIVE_MAX)
        return; /* single animation larger than the whole budget → drop */
    lt_evict_to_budget(vt, st, need);

    if (rec) {
        if (rec->arena_base)
            cfr_dealloc(vt, rec->arena_base);
        rec->arena_base = NULL;
        rec->arena_offset = 0;
        rec->arena_cap = 0;
        if (rec->placements)
            rec->placement_count = 0;

        if (need > rec->rgba_cap) {
            if (rec->rgba) {
                st->live_bytes -= rec->rgba_cap;
                lt_buf_release(vt, st, rec->rgba, rec->rgba_cap);
            }
            rec->rgba = lt_buf_alloc(vt, st, need);
            rec->rgba_cap = need;
            st->live_bytes += need;
            if (!rec->rgba) {
                lt_rec_release(vt, st, (int)(rec - st->recs));
                return;
            }
        }
    } else {
        if (st->rec_count >= st->rec_cap) {
            int new_cap = st->rec_cap ? st->rec_cap * 2 : 8;
            if (new_cap > LT_MAX_ANIMS)
                new_cap = LT_MAX_ANIMS;
            LtRec *new_recs = cfr_alloc(vt, (size_t)new_cap * sizeof(LtRec));
            if (!new_recs)
                return;
            if (st->recs) {
                memcpy(new_recs, st->recs,
                       (size_t)st->rec_count * sizeof(LtRec));
                cfr_dealloc(vt, st->recs);
            }
            st->recs = new_recs;
            st->rec_cap = new_cap;
        }
        rec = &st->recs[st->rec_count++];
        memset(rec, 0, sizeof(*rec));
        rec->id = id;
        rec->version = 1;

        rec->rgba = lt_buf_alloc(vt, st, need);
        rec->rgba_cap = need;
        st->live_bytes += need;
        if (!rec->rgba) {
            st->rec_count--;
            return;
        }
    }

    if (lottie_obj) {
        size_t arena_need = lottie_obj_len + 1;
        rec->arena_base = cfr_alloc(vt, arena_need);
        if (rec->arena_base) {
            rec->arena_cap = arena_need;
            rec->arena_offset = lottie_obj_len;
            memcpy(rec->arena_base, lottie_obj, lottie_obj_len);
            rec->arena_base[lottie_obj_len] = '\0';
        }
    }

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
    rec->dirty = true;

#ifdef HAVE_THORVG
    /* Destroy any previous ThorVG state for this rec (re-load) */
    if (rec->tvg_canvas && rec->tvg_anim) {
        Tvg_Paint pic = tvg_animation_get_picture(rec->tvg_anim);
        tvg_canvas_remove(rec->tvg_canvas, pic);
    }
    if (rec->tvg_anim) {
        tvg_animation_del(rec->tvg_anim);
        rec->tvg_anim = NULL;
    }
    if (rec->tvg_canvas) {
        tvg_canvas_destroy(rec->tvg_canvas);
        rec->tvg_canvas = NULL;
    }
    /* Initialize ThorVG and rasterize the first frame */
    if (!lt_tvg_init(rec))
        memset(rec->rgba, 0, rec->rgba_cap);
#else
    memset(rec->rgba, 0, rec->rgba_cap);
#endif

    long abs_line = vt->sixel_abs_top + prow;
    CfrLottiePlacement *pl = lt_add_placement(vt, st, rec, abs_line, pcol,
                                              prows, pcols, layer, opacity);
    if (pl)
        lt_damage_placement(vt, pl);

    if (want_report)
        lt_emit_report(vt, id, prow, pcol, prows, pcols, px_w, px_h);

    rec->version++;
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

    LtRec *rec = lt_find_by_id(st, id);
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

        double scale = lt_compute_scale(
            rec->design_w, rec->design_h,
            new_w, new_h, new_cols, new_rows,
            vt->cell_w_px, vt->cell_h_px,
            new_fit, new_scale);

        int new_px_w = (int)((double)rec->design_w * scale + 0.5);
        int new_px_h = (int)((double)rec->design_h * scale + 0.5);
        if (new_px_w < 1)
            new_px_w = 1;
        if (new_px_h < 1)
            new_px_h = 1;

        /* Realloc RGBA buffer if size changed */
        size_t need = (size_t)new_px_w * (size_t)new_px_h * 4;
        if (need > LT_LIVE_MAX)
            return;
        lt_evict_to_budget(vt, st, need);
        if (need != rec->rgba_cap) {
            st->live_bytes -= rec->rgba_cap;
            lt_buf_release(vt, st, rec->rgba, rec->rgba_cap);
            rec->rgba = lt_buf_alloc(vt, st, need);
            rec->rgba_cap = need;
            st->live_bytes += need;
            if (!rec->rgba)
                return;
        }

        rec->px_w = new_px_w;
        rec->px_h = new_px_h;

#ifdef HAVE_THORVG
        /* Update ThorVG picture size (no JSON reload) */
        if (rec->tvg_anim) {
            Tvg_Paint pic = tvg_animation_get_picture(rec->tvg_anim);
            tvg_picture_set_size(pic, (float)new_px_w, (float)new_px_h);

            /* Recreate SW canvas with new target buffer */
            if (rec->tvg_canvas) {
                tvg_canvas_remove(rec->tvg_canvas,
                                  tvg_animation_get_picture(rec->tvg_anim));
                tvg_canvas_destroy(rec->tvg_canvas);
            }
            rec->tvg_canvas = tvg_swcanvas_create(TVG_ENGINE_OPTION_DEFAULT);
            tvg_swcanvas_set_target(rec->tvg_canvas,
                                    (uint32_t *)rec->rgba,
                                    (uint32_t)new_px_w,
                                    (uint32_t)new_px_w,
                                    (uint32_t)new_px_h,
                                    TVG_COLORSPACE_ARGB8888);
            tvg_canvas_add(rec->tvg_canvas, pic);

            /* Re-rasterize current frame — seamless rescale */
            lt_rasterize(rec);
        }
#endif
#ifdef HAVE_THORVG
        if (!rec->tvg_anim)
#endif
            memset(rec->rgba, 0, rec->rgba_cap);

        rec->version++;
    }

    /* Compute placement cells from current rasterization size */
    int pcols = (rec->px_w + vt->cell_w_px - 1) / vt->cell_w_px;
    int prows = (rec->px_h + vt->cell_h_px - 1) / vt->cell_h_px;
    if (pcols < 1)
        pcols = 1;
    if (prows < 1)
        prows = 1;

    /* Center cell box within the placement region */
    if (new_cols > 0)
        pcol += (new_cols - pcols) / 2;
    if (new_rows > 0)
        prow += (new_rows - prows) / 2;
    if (prow < 0)
        prow = 0;
    if (pcol < 0)
        pcol = 0;

    long abs_line = vt->sixel_abs_top + prow;
    CfrLottiePlacement *pl = lt_add_placement(vt, st, rec, abs_line, pcol,
                                              prows, pcols, layer, opacity);
    if (pl)
        lt_damage_placement(vt, pl);

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

    LtRec *rec = lt_find_by_id(st, id);
    if (!rec)
        return;

    rec->playing = true;
    rec->last_tick_us = 0;

    val = lt_json_find_key(json, json_len, "speed", &vlen);
    if (val)
        rec->speed = lt_json_double(val, vlen);

    val = lt_json_find_key(json, json_len, "loop", &vlen);
    if (val)
        rec->loop = lt_json_bool(val, vlen);

    rec->version++;
    lt_damage_all_placements(vt, rec);
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

    LtRec *rec = lt_find_by_id(st, id);
    if (!rec)
        return;

    rec->playing = false;
    rec->version++;
    lt_damage_all_placements(vt, rec);
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

    LtRec *rec = lt_find_by_id(st, id);
    if (!rec)
        return;

    rec->playing = false;
    rec->current_frame = rec->frame_ip;
    rec->dirty = true;
    lt_rasterize(rec);
    rec->version++;
    lt_damage_all_placements(vt, rec);
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

    LtRec *rec = lt_find_by_id(st, id);
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
        rec->dirty = true;
        lt_rasterize(rec);
    }
    rec->version++;
    lt_damage_all_placements(vt, rec);
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

    LtRec *rec = lt_find_by_id(st, id);
    if (!rec)
        return;

    lt_damage_all_placements(vt, rec);
    lt_rec_release(vt, st, (int)(rec - st->recs));
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

    /* Decode the base64 data field */
    size_t dec_cap = b64_len;
    uint8_t *decoded = cfr_alloc(vt, dec_cap + 1);
    if (!decoded) {
        lt_chunk_discard(vt, st, c);
        return;
    }
    size_t dec_len = lt_base64_decode(b64_in, b64_len, decoded, dec_cap);

    if (c->buf_len + dec_len > c->buf_cap) {
        size_t new_cap = (c->buf_len + dec_len) * 2;
        uint8_t *new_buf = cfr_alloc(vt, new_cap);
        if (!new_buf) {
            cfr_dealloc(vt, decoded);
            lt_chunk_discard(vt, st, c);
            return;
        }
        if (c->buf_len > 0)
            memcpy(new_buf, c->buf, c->buf_len);
        if (c->buf)
            cfr_dealloc(vt, c->buf);
        c->buf = new_buf;
        c->buf_cap = new_cap;
    }
    memcpy(c->buf + c->buf_len, decoded, dec_len);
    c->buf_len += dec_len;
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

    size_t dec_cap = body_len;
    uint8_t *decoded = cfr_alloc(vt, dec_cap + 1);
    if (!decoded)
        return;
    size_t dec_len = lt_base64_decode(body, body_len, decoded, dec_cap);
    decoded[dec_len] = '\0';

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

bool cfr_lottie_tick(CfrTerm *vt, uint64_t now_us)
{
    struct CfrLottieState *st = vt->lottie;
    if (!st || st->rec_count == 0)
        return false;

    bool any_advanced = false;

    for (int i = 0; i < st->rec_count; i++) {
        LtRec *r = &st->recs[i];
        if (!r->playing)
            continue;

        if (r->last_tick_us == 0) {
            r->last_tick_us = now_us;
            continue;
        }

        uint64_t elapsed = now_us - r->last_tick_us;
        double frame_delta = (double)elapsed * r->speed * r->frame_fr / 1e6;
        int delta_int = (int)frame_delta;
        if (delta_int <= 0)
            continue;

        int new_frame = r->current_frame + delta_int;
        if (new_frame >= r->frame_op) {
            if (r->loop) {
                new_frame = r->frame_ip +
                            (new_frame - r->frame_ip) %
                                (r->frame_op - r->frame_ip);
            } else {
                new_frame = r->frame_op - 1;
                r->playing = false;
            }
        }

        if (new_frame != r->current_frame) {
            r->current_frame = new_frame;
            r->dirty = true;
            lt_rasterize(r);
            lt_damage_all_placements(vt, r);
            any_advanced = true;
        }

        r->last_tick_us += (uint64_t)((double)delta_int * 1e6 /
                                      (r->speed * r->frame_fr));
        r->version++;
    }

    return any_advanced;
}

const CfrLottie *cfr_get_lotties(CfrTerm *vt, int *out_count)
{
    struct CfrLottieState *st = vt->lottie;
    if (!st || st->rec_count == 0) {
        *out_count = 0;
        return NULL;
    }

    size_t total = (size_t)st->rec_count * sizeof(CfrLottie);
    if (total > st->scratch_cap) {
        size_t new_cap = total * 2;
        uint8_t *new_scratch = cfr_alloc(vt, new_cap);
        if (!new_scratch) {
            *out_count = 0;
            return NULL;
        }
        if (st->scratch)
            cfr_dealloc(vt, st->scratch);
        st->scratch = new_scratch;
        st->scratch_cap = new_cap;
    }

    for (int i = 0; i < st->rec_count; i++) {
        LtRec *r = &st->recs[i];
        CfrLottie *l = &((CfrLottie *)st->scratch)[i];
        l->id = r->id;
        l->version = r->version;
        l->canvas_w = r->px_w;
        l->canvas_h = r->px_h;
        l->rgba = r->rgba;
        l->current_frame = r->current_frame;
        l->frame_count = r->frame_op - r->frame_ip;
        l->playing = r->playing;
        l->speed = r->speed;
        l->loop = r->loop;
        l->placement_count = r->placement_count;
        r->dirty = false;
    }

    *out_count = st->rec_count;
    return (CfrLottie *)st->scratch;
}

const CfrLottiePlacement *cfr_get_lottie_placements(CfrTerm *vt, uint64_t id,
                                                    int *out_count)
{
    struct CfrLottieState *st = vt->lottie;
    if (!st) {
        *out_count = 0;
        return NULL;
    }

    LtRec *rec = lt_find_by_id(st, id);
    if (!rec || rec->placement_count == 0) {
        *out_count = 0;
        return NULL;
    }

    int need = rec->placement_count;
    if (need > st->pl_scratch_cap) {
        int ncap = st->pl_scratch_cap ? st->pl_scratch_cap * 2 : 8;
        while (ncap < need)
            ncap *= 2;
        CfrLottiePlacement *ns = cfr_alloc(vt, (size_t)ncap * sizeof(*ns));
        if (!ns) {
            *out_count = 0;
            return NULL;
        }
        if (st->pl_scratch)
            cfr_dealloc(vt, st->pl_scratch);
        st->pl_scratch = ns;
        st->pl_scratch_cap = ncap;
    }

    for (int i = 0; i < need; i++) {
        st->pl_scratch[i] = rec->placements[i];
        st->pl_scratch[i].row =
            (int)(rec->placements[i].abs_line - vt->sixel_abs_top);
    }

    *out_count = need;
    return st->pl_scratch;
}

/* ------------------------------------------------------------------ */
/* Grid maintenance                                                   */
/* ------------------------------------------------------------------ */

void cfr_lottie_note_scroll(CfrTerm *vt, int lines)
{
    struct CfrLottieState *st = vt->lottie;
    if (!st || st->rec_count == 0)
        return;
    (void)lines;

    int cap = vt->sb_capacity;
    for (int i = 0; i < st->rec_count;) {
        bool any_visible = false;
        for (int j = 0; j < st->recs[i].placement_count; j++) {
            long depth = vt->sixel_abs_top -
                         st->recs[i].placements[j].abs_line;
            long bottom_depth = depth - st->recs[i].placements[j].rows + 1;
            if (bottom_depth <= cap) {
                any_visible = true;
                break;
            }
        }
        if (!any_visible)
            lt_rec_release(vt, st, i);
        else
            ++i;
    }
}

void cfr_lottie_clear_display_rows(CfrTerm *vt, int top, int bot)
{
    struct CfrLottieState *st = vt->lottie;
    if (!st)
        return;
    if (st->rec_count == 0)
        return;

    for (int i = 0; i < st->rec_count;) {
        bool removed_any = false;
        for (int j = 0; j < st->recs[i].placement_count;) {
            CfrLottiePlacement *pl = &st->recs[i].placements[j];
            if (pl->layer != 0) {
                ++j;
                continue;
            }
            int ptop = (int)(pl->abs_line - vt->sixel_abs_top);
            int pbot = ptop + pl->rows - 1;
            if (ptop <= bot && pbot >= top) {
                st->recs[i].placements[j] =
                    st->recs[i].placements[--st->recs[i].placement_count];
                removed_any = true;
            } else {
                ++j;
            }
        }
        if (removed_any)
            st->recs[i].version++;

        if (st->recs[i].placement_count == 0)
            lt_rec_release(vt, st, i);
        else
            ++i;
    }
}

void cfr_lottie_clear_all(CfrTerm *vt)
{
    struct CfrLottieState *st = vt->lottie;
    if (!st)
        return;

    while (st->rec_count > 0)
        lt_rec_release(vt, st, 0);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */

void cfr_lottie_state_free(CfrTerm *vt)
{
    struct CfrLottieState *st = vt->lottie;
    if (!st)
        return;

    cfr_lottie_clear_all(vt);

    if (st->recs)
        cfr_dealloc(vt, st->recs);

    for (int i = 0; i < st->chunk_count; i++) {
        if (st->chunks[i].buf)
            cfr_dealloc(vt, st->chunks[i].buf);
    }
    if (st->chunks)
        cfr_dealloc(vt, st->chunks);

    for (int i = 0; i < st->spare_count; i++)
        cfr_dealloc(vt, st->spares[i].buf);

    if (st->scratch)
        cfr_dealloc(vt, st->scratch);

    if (st->pl_scratch)
        cfr_dealloc(vt, st->pl_scratch);

    cfr_dealloc(vt, st);
    vt->lottie = NULL;

#ifdef HAVE_THORVG
    tvg_engine_term();
#endif
}

bool cfr_have_lottie(void)
{
#ifdef HAVE_THORVG
    return true;
#else
    return false;
#endif
}

int cfr_lottie_active_count(CfrTerm *vt)
{
    struct CfrLottieState *st = vt->lottie;
    if (!st)
        return 0;
    return st->rec_count;
}
