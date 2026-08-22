/* coffer/src/image_store.h — generic grid-anchored image store.
 *
 * Extracted from sixel.c and lottie.c. Provides the shared infrastructure
 * for all inline image protocols: sixel, iTerm2 (OSC 1337), kitty graphics,
 * and Lottie animations.
 *
 * The store owns RGBA pixel buffers anchored to absolute grid lines so they
 * scroll with text. Memory is managed in two tiers: a dense record array
 * (swap-remove O(1) deletion) and a best-fit free-list pool for pixel buffers.
 */
#ifndef COFFER_IMAGE_STORE_H
#define COFFER_IMAGE_STORE_H

#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Constants                                                          */
/* ------------------------------------------------------------------ */

#define IMG_MAX_DIM    10000                  /* per-dimension clamp */
#define IMG_MAX_IMAGES 256                    /* live record cap */
#define IMG_LIVE_MAX   (128u * 1024u * 1024u) /* live pixel-byte budget */
#define IMG_SPARE_MAX  16                     /* retained free buffers */
#define IMG_RETAIN_MAX (32u * 1024u * 1024u)  /* retained free bytes */

/* Image source identifiers */
enum
{
    IMG_SRC_SIXEL = 0,
    IMG_SRC_LOTTIE = 1,
    IMG_SRC_ITERM = 2,
    IMG_SRC_KITTY = 3,
};

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

/* One stored image (was SxRec in sixel.c). */
typedef struct
{
    uint64_t id;
    uint32_t version;
    uint8_t layer;  /* 0 = foreground, 1 = background */
    uint8_t source; /* IMG_SRC_* */
    long abs_line;  /* absolute line index of the image's top row */
    int col;        /* anchor column */
    int w, h;       /* display pixel dimensions */
    int rows_tall;  /* cells tall (cached for cull/clear) */
    int cols_wide;  /* cells wide (cached) */
    uint8_t *rgba;  /* pixel buffer */
    size_t cap;     /* allocated bytes of rgba */
} CfrImg;

/* A retained free buffer (was SxSpare / LtSpare). */
typedef struct
{
    uint8_t *ptr;
    size_t cap;
} ImgSpare;

/* Public image snapshot. CfrSixel is defined in coffer.h and is the
 * same struct — the alias keeps the name in sync with the rename. */
typedef CfrSixel CfrImage;

/* The store state (was CfrSixelState storage fields / CfrLottieState). */
typedef struct CfrImgStore
{
    /* Tier 1: image records */
    CfrImg *imgs;
    int img_count, img_cap;
    uint64_t next_id;
    size_t live_bytes;

    /* Tier 2: pixel-buffer free-list pool */
    ImgSpare spares[IMG_SPARE_MAX];
    int spare_count;
    size_t retain_bytes;

    /* Query scratch — reused, grown, never freed between calls */
    CfrImage *img_scratch;
    int img_scratch_cap;
} CfrImgStore;

/* CfrImage is typedef'd to CfrSixel above. */

/* ------------------------------------------------------------------ */
/* Store lifecycle                                                    */
/* ------------------------------------------------------------------ */

CfrImgStore *cfr_img_store_new(void *vt);
void cfr_img_store_free(void *vt, CfrImgStore *st);

/* ------------------------------------------------------------------ */
/* Buffer pool (tier 2)                                               */
/* ------------------------------------------------------------------ */

/* Best-fit pop a retained buffer with cap >= need, else alloc exactly. */
uint8_t *img_buf_alloc(void *vt, CfrImgStore *st, size_t need, size_t *out_cap);

/* Retain a freed buffer for reuse, or free it if the pool is full. */
void img_buf_release(void *vt, CfrImgStore *st, uint8_t *ptr, size_t cap);

/* ------------------------------------------------------------------ */
/* Image lifecycle (tier 1)                                           */
/* ------------------------------------------------------------------ */

/* Store pre-decoded RGBA data as a new image at the cursor position.
 * Handles buffer alloc, eviction, cursor advance, and damage.
 * Returns image index >= 0, or -1 on failure. */
int cfr_img_add(void *vt, CfrImgStore *st,
                const uint8_t *rgba, int w, int h,
                uint8_t layer, uint8_t source);

/* Replace image pixel data (animation / frame update). Bumps version. */
void cfr_img_replace(void *vt, CfrImgStore *st, int idx,
                     const uint8_t *rgba, int w, int h);

/* Find an image at a given anchor + layer. Returns index or -1. */
int cfr_img_find_at(CfrImgStore *st, long abs_line, int col, uint8_t layer);

/* Evict oldest (lowest abs_line) images until incoming more bytes fit. */
void img_evict_to_budget(void *vt, CfrImgStore *st, size_t incoming);

/* ------------------------------------------------------------------ */
/* Grid maintenance                                                   */
/* ------------------------------------------------------------------ */

/* Cull images whose bottom row has scrolled past the oldest retained
 * scrollback line. */
void cfr_img_note_scroll(void *vt, CfrImgStore *st, int lines);

/* Remove foreground-layer (0) images overlapping the inclusive display
 * row range [top, bot]. Background-layer (1) images are preserved. */
void cfr_img_clear_display_rows(void *vt, CfrImgStore *st, int top, int bot);

/* Remove all images. */
void cfr_img_clear_all(void *vt, CfrImgStore *st);

/* ------------------------------------------------------------------ */
/* Public query                                                       */
/* ------------------------------------------------------------------ */

/* Return the live images as an array of CfrImage snapshots. The array
 * is owned by the engine and valid until the next mutation.
 * *out_count receives the count (0 if none). Returns NULL when empty. */
const CfrImage *cfr_img_get(void *vt, CfrImgStore *st, int *out_count);

#endif /* COFFER_IMAGE_STORE_H */
