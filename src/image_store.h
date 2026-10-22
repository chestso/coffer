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
#include <stdbool.h>

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
/* Image kinds                                                        */
/* ------------------------------------------------------------------ */

/* The two lifetimes the store carries. The kind is set at creation and
 * NEVER inferred: "does this image happen to have a placement?" is not
 * a lifetime question (a kitty image can be transmitted before it is
 * placed, and it must survive an erase until it is placed).
 *
 *   ANCHORED   — the image IS its placement (sixel, iTerm2). The record
 *                carries the anchor (abs_line/col); the image lives as
 *                long as the screen area it covers.
 *   REFERENCED — the image is referenced by placements (kitty, Lottie).
 *                The record carries no anchor of its own; the image
 *                lives until the client deletes it, until the id is
 *                re-transmitted, or until the store's budget evicts it.
 */
enum
{
    IMG_KIND_ANCHORED = 0,
    IMG_KIND_REFERENCED = 1
};

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

/* One stored image (was SxRec in sixel.c). */
typedef struct
{
    uint64_t id;         /* PUBLIC id: the client's i= for 1:N, the engine's
                          * for 1:1. The host's texture cache keys on it. */
    uint64_t handle;     /* store-internal identity, never reused, never
                          * exposed; the placements point at THIS, so a
                          * client id and an engine id cannot collide. */
    uint64_t seq;        /* creation order (monotonic); the eviction's order */
    uint32_t version;    /* pixel generation from version_seq; never repeats */
    uint8_t layer;       /* 0 = foreground, 1 = background */
    uint8_t source;      /* IMG_SRC_* */
    uint8_t kind;        /* IMG_KIND_* */
    uint8_t ever_placed; /* 1 once a placement referenced this image; an
                          * image that was transmitted but never placed
                          * is INVISIBLE, so a screen erase must not
                          * touch it (kitty's rule) */
    long abs_line;       /* absolute line index of the image's top row */
    int col;             /* anchor column */
    int w, h;            /* logical display pixel dimensions */
    int buf_w, buf_h;    /* pixel buffer stride (native decoded size) */
    int rows_tall;       /* cells tall (cached for cull/clear) */
    int cols_wide;       /* cells wide (cached) */
    uint8_t *rgba;       /* pixel buffer */
    size_t cap;          /* allocated bytes of rgba */
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
    uint64_t id;           /* placement id (client- or engine-assigned) */
    uint64_t image_id;     /* owning image's PUBLIC id */
    uint64_t image_handle; /* owning image's store-internal handle; the
                            * store's own lookups use THIS, so a client
                            * id cannot collide with an engine id */
    uint8_t source;        /* the owning image's IMG_SRC_* — the id space
                            * the public id belongs to */
    long abs_line;         /* absolute line anchor (scrolls with text) */
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
    /* Store-internal handle counter. Handles are never reused and never
     * exposed: the placements point at a handle, so a client-assigned
     * id (kitty's i=) and an engine-assigned id (sixel/iTerm2) can
     * never resolve to each other. */
    uint64_t next_handle;
    /* Creation order counter: every new record gets the next seq, so
     * the eviction has a total order over ALL records (the 1:N images
     * have no anchor of their own to order them by). */
    uint64_t next_seq;
    /* Pixel-generation counter: every store or replace of pixel data
     * stamps the record with ++version_seq, so an (id, version) pair
     * never repeats for different pixels — even when a named image is
     * removed and re-added under the same id (kitty re-transmit). */
    uint32_t version_seq;
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

/* Find an image by id. Returns index or -1.
 *
 * The id spaces of the protocols overlap by construction (kitty's
 * client-chosen i=1 and the engine's first id are both 1), so this
 * lookup is only meaningful WITHIN one protocol family. A client-id
 * protocol (kitty, Lottie) must use cfr_img_find_by_source_id(); the
 * engine-id protocols (sixel, iTerm2) own their ids exclusively. */
int cfr_img_find_by_id(CfrImgStore *st, uint64_t id);

/* Find an image by its PUBLIC id within one protocol family. Returns
 * index or -1. */
int cfr_img_find_by_source_id(CfrImgStore *st, uint8_t source, uint64_t id);

/* Find an image by its store-internal handle. Unambiguous across
 * protocols — this is what the placements point at. */
int cfr_img_find_by_handle(CfrImgStore *st, uint64_t handle);

/* Remove an image (and its placements) by index. */
void cfr_img_remove(void *vt, CfrImgStore *st, int idx);

/* Add a placement of an existing image (1:N model). The image is named
 * by its HANDLE (unambiguous across protocols), not by its public id.
 * Dedups on image_handle + abs_line + col. Returns placement index
 * >= 0, or -1. Damages the rows the placement covers. */
int cfr_img_add_placement(void *vt, CfrImgStore *st, uint64_t image_handle,
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
 * row range [top, bot]. Background-layer (1) images are preserved.
 *
 * `full` marks the screen-wide clear (ED 2). It matters for the 1:N
 * protocols: kitty's "the other commands to erase text must have no
 * effect on graphics" leaves the kitty images alone, and only the
 * screen-wide clear touches them — and it clears the VISIBLE ones
 * only (an image that was transmitted but never placed is not on the
 * screen, so it survives). */
void cfr_img_clear_display_rows(void *vt, CfrImgStore *st, int top, int bot,
                                bool full);

/* Remove all images (a terminal reset, kitty's "delete all images"). */
void cfr_img_clear_all(void *vt, CfrImgStore *st);

/* Remove the images that die WITH the screen: the anchored (1:1)
 * ones, whose anchors are the grid's own rows. A resize re-anchors
 * the grid and an altscreen switch swaps it, so those images cannot be
 * followed — the client-owned 1:N images (kitty) live on. */
void cfr_img_clear_anchored(void *vt, CfrImgStore *st);

/* Remove every image of ONE protocol. The protocols' id spaces are
 * their own, so "delete all images" (kitty) means the kitty images. */
void cfr_img_clear_source(void *vt, CfrImgStore *st, uint8_t source);

/* Remove the foreground placements of ONE protocol in the display row
 * range [top, bot], and the images that die with them. The Lottie
 * handler uses it: a Lottie animation's clear behaviour is its own
 * (the Lottie spec), so it must not be decided by the kitty rules. */
void cfr_img_clear_source_rows(void *vt, CfrImgStore *st, uint8_t source,
                               int top, int bot);

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
 * placements or doesn't exist.
 *
 * `source` scopes the query to one protocol's id space (the public ids
 * of the 1:N protocols are chosen by their clients, so kitty's i=1 and
 * a Lottie id of 1 are the same number in different spaces). Pass
 * IMG_SRC_ANY to search every protocol. */
const CfrImagePlacement *cfr_img_get_placements_for(void *vt, CfrImgStore *st,
                                                    uint8_t source,
                                                    uint64_t image_id,
                                                    int *out_count);

#endif /* COFFER_IMAGE_STORE_H */
