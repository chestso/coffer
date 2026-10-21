/* Probe: WHICH record does the IMG_MAX_IMAGES overflow evict?
 *
 * The overflow path is `img_rec_release(vt, st, 0)` — always index 0.
 * img_rec_release ends with `st->imgs[idx] = st->imgs[--st->img_count]`,
 * so releasing index 0 MOVES the LAST record (the newest) into slot 0.
 * The next overflow therefore releases the newest image.
 *
 * Show it: fill past the cap and watch which id disappears.
 */
#include <coffer/coffer.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void feed(CfrTerm *vt, const char *s)
{
    cfr_input_write(vt, (const uint8_t *)s, strlen(s));
}

static int has(CfrTerm *vt, long long id)
{
    int n = 0;
    const CfrImage *imgs = cfr_get_images(vt, &n);
    for (int i = 0; i < n; i++)
        if ((long long)imgs[i].id == id)
            return 1;
    return 0;
}

int main(void)
{
    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.rows = 24;
    cfg.cols = 80;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 20;
    CfrTerm *vt = cfr_new(&cfg);

    char seq[256];
    /* 1x1 raw RGBA: tiny, so the byte budget never bites. */
    const char *b64 = "/w=="; /* one byte -> not 4; use a real 4-byte b64 */
    const char *b64ok = "AAAAAAE="; /* 4 zero bytes */
    (void)b64;

    for (int id = 1; id <= 258; id++) {
        snprintf(seq, sizeof(seq), "\x1b_Ga=t,f=32,s=1,v=1,i=%d,q=2;%s\x1b\\",
                 id, b64ok);
        feed(vt, seq);
        if (id >= 254) {
            int n = 0;
            cfr_get_images(vt, &n);
            printf("after #%d: %d images live; id 1 %s, id %d %s, id %d %s\n",
                   id, n, has(vt, 1) ? "ALIVE" : "GONE", id - 1,
                   has(vt, id - 1) ? "ALIVE" : "GONE", id,
                   has(vt, id) ? "ALIVE" : "GONE");
        }
    }

    cfr_free(vt);
    return 0;
}
