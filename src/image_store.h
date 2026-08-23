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

/* Image source identifiers (IMG_SRC_*) are defined in coffer.h */

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

/* One stored image (was SxRec in sixel.c). */
typedef struct
{
    uint64_t id;
    uint32_t version;
    uint8_t layer;    /* 0 = foreground, 1 = background */
    uint8_t source;   /* IMG_SRC_* */
    long abs_line;    /* absolute line index of the image's top row */
    int col;          /* anchor column */
    int w, h;         /* logical display pixel dimensions */
    int buf_w, buf_h; /* logical pixel buffer stride (native decoded size) */
    int rows_tall;    /* cells tall (cached for cull/clear) */
    int cols_wide;    /* cells wide (cached) */
    uint8_t *rgba;    /* pixel buffer */
    size_t cap;       /* allocated bytes of rgba */
} CfrImg;

/* A retained free buffer (was SxSpare / LtSpare). */
typedef struct
{
    uint8_t *ptr;
    size_t cap;
} ImgSpare;

/* One placement of an image on the terminal grid (1:N image-to-placement
 * model used by Lottie and kitty). */
typedef struct
{
    uint64_t id;       /* placement id (client- or engine-assigned) */
    uint64_t image_id; /* owning image's id */
    long abs_line;     /* absolute line anchor (scrolls with text) */
    int col;
    int rows, cols;
    uint8_t layer;
    uint8_t opacity_x256;
    int z_index;                    /* kitty only; 0 for others */
    int src_x, src_y, src_w, src_h; /* source rect (kitty only) */
    int pix_offset_x, pix_offset_y; /* pixel offset in first cell (kitty only) */
    int cell_off_x, cell_off_y;
    uint64_t parent_img;   /* relative placement parent image (kitty) */
    uint64_t parent_place; /* relative placement parent placement (kitty) */
} CfrPlacement;

/* Public image snapshot (defined in coffer.h). */

/* The store state (was CfrSixelState storage fields / CfrLottieState). */
typedef struct CfrImgStore
{
    /* Tier 1: image records */
    CfrImg *imgs;
    int img_count, img_cap;
    uint64_t next_id;
    size_t live_bytes;

    /* Tier 1b: placement records (for 1:N systems) */
    CfrPlacement *places;
    int place_count, place_cap;
    uint64_t next_place_id;

    /* Tier 2: pixel-buffer free-list pool */
    ImgSpare spares[IMG_SPARE_MAX];
    int spare_count;
    size_t retain_bytes;

    /* Query scratch — reused, grown, never freed between calls */
    CfrImage *img_scratch;
    int img_scratch_cap;
    CfrImagePlacement *place_scratch;
    int place_scratch_cap;
} CfrImgStore;

/* CfrImage is typedef'd to CfrImage above. */

/* ------------------------------------------------------------------ */
/* Store lifecycle                                                    */
/* ------------------------------------------------------------------ */

CfrImgStore *cfr_img_store_new(void *vt);
void cfr_img_store_free(void *vt, CfrImgStore *st);

/* Lazy-initialize the terminal's image store (vt->images). */
CfrImgStore *cfr_img_get_store(void *vt);

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
 * `w`/`h` are the pixel buffer dimensions (logical, stride for the copy).
 * `disp_w`/`disp_h` are the display dimensions (logical); pass 0 for
 * either to use w/h (the sixel case where they're identical).  Grid
 * occupancy and the public CfrImage.width_px/height_px use the display
 * dimensions.  Handles buffer alloc, eviction, cursor advance, and
 * damage.  Returns image index >= 0, or -1 on failure. */
int cfr_img_add(void *vt, CfrImgStore *st,
                const uint8_t *rgba, int w, int h,
                int disp_w, int disp_h,
                uint8_t layer, uint8_t source);

/* Store RGBA data keyed by explicit id (lottie animation). Creates or
 * replaces the image with the given id at an explicit anchor without
 * advancing the cursor or damaging (caller handles placement/damage).
 * Returns image index >= 0, or -1 on failure. */
int cfr_img_add_named(void *vt, CfrImgStore *st,
                      uint64_t id, const uint8_t *rgba, int w, int h,
                      uint8_t layer, uint8_t source);

/* Ensure a zero-initialised RGBA buffer exists for the given id, sized
 * w*h. Returns the image index (>= 0) or -1. The caller writes pixels
 * directly into st->imgs[idx].rgba (e.g. a software rasterizer target)
 * and then calls cfr_img_mark_dirty(). No copy, no cursor advance. */
int cfr_img_blank_named(void *vt, CfrImgStore *st, uint64_t id,
                        int w, int h, uint8_t layer, uint8_t source);

/* Bump the image version after in-place pixel writes (rasterizer wrote
 * directly into the buffer). Damage is the caller's responsibility. */
void cfr_img_mark_dirty(CfrImgStore *st, int idx);

/* Replace image pixel data (animation / frame update). Bumps version. */
void cfr_img_replace(void *vt, CfrImgStore *st, int idx,
                     const uint8_t *rgba, int w, int h);

/* Find an image at a given anchor + layer. Returns index or -1. */
int cfr_img_find_at(CfrImgStore *st, long abs_line, int col, uint8_t layer);

/* Find an image by id. Returns index or -1. */
int cfr_img_find_by_id(CfrImgStore *st, uint64_t id);

/* Remove an image (and its placements) by index. */
void cfr_img_remove(void *vt, CfrImgStore *st, int idx);

/* Add a placement of an existing image (1:N model). Dedups on
 * image_id + abs_line + col. Returns placement index >= 0, or -1. */
int cfr_img_add_placement(void *vt, CfrImgStore *st, uint64_t image_id,
                          long abs_line, int col, int rows, int cols,
                          uint8_t layer, uint8_t opacity, int z_index);

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

/* Return the live placements as an array of CfrImagePlacement snapshots.
 * The array is owned by the engine and valid until the next mutation.
 * *out_count receives the count (0 if none). Returns NULL when empty. */
const CfrImagePlacement *cfr_img_get_placements(void *vt, CfrImgStore *st,
                                                int *out_count);

/* Return placements for a single image id (1:N model). The returned array
 * is owned by the engine and valid until the next mutation. *out_count
 * receives the count (0 if none). Returns NULL when the image has no
 * placements or doesn't exist. */
const CfrImagePlacement *cfr_img_get_placements_for(void *vt, CfrImgStore *st,
                                                    uint64_t image_id,
                                                    int *out_count);

#endif /* COFFER_IMAGE_STORE_H */
