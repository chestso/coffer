/* Probe: a nevermore-like chat flow through coffer.
 *
 * Sequence: chat text, an image (client id 1), chat text, another image
 * (client id 2), more text, then a scroll. Check the store for:
 *  - the id space (client ids vs engine ids),
 *  - placements pointing at the right image,
 *  - the images' pixel dims vs their buffer dims.
 */
#include <coffer/coffer.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *PNG_B64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAgAAAAICAYAAADED76LAAAALklEQVR4nGNgkLOJqpi25dIHPh2vjLYlhx"
    "78k7GKKJuy6cI7Hi2PtJZFDCNDAQAHKnkB/VWi/wAAAABJRU5ErkJggg==";

static void dump(CfrTerm *vt, const char *tag)
{
    int n = 0;
    const CfrImage *imgs = cfr_get_images(vt, &n);
    int pc = 0;
    const CfrImagePlacement *pls = cfr_get_image_placements(vt, &pc);
    printf("--- %s: %d image(s), %d placement(s)\n", tag, n, pc);
    for (int i = 0; i < n; i++)
        printf("    img[%d] id=%llu src=%u ver=%u row=%d col=%d disp=%dx%d "
               "buf=%dx%d\n",
               i, (unsigned long long)imgs[i].id, imgs[i].source, imgs[i].version,
               imgs[i].row, imgs[i].col, imgs[i].width_px, imgs[i].height_px,
               imgs[i].buf_w, imgs[i].buf_h);
    for (int i = 0; i < pc; i++) {
        const CfrImage *owner = NULL;
        for (int j = 0; j < n; j++)
            if (imgs[j].id == pls[i].image_id)
                owner = &imgs[j];
        printf("    pl[%d] image=%llu row=%d col=%d %dx%d cells -> owner %s\n", i,
               (unsigned long long)pls[i].image_id, pls[i].row, pls[i].col,
               pls[i].rows, pls[i].cols,
               owner ? (owner->source == IMG_SRC_KITTY ? "KITTY" : "OTHER")
                     : "*** MISSING ***");
    }
}

static void feed(CfrTerm *vt, const char *s)
{
    cfr_input_write(vt, (const uint8_t *)s, strlen(s));
}

int main(void)
{
    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.rows = 24;
    cfg.cols = 80;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 20;
    CfrTerm *vt = cfr_new(&cfg);

    char seq[4096];

    feed(vt, "hello there\r\n");
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=T,f=100,s=8,v=8,c=4,r=2,i=1,q=2,C=1;%s\x1b\\", PNG_B64);
    feed(vt, seq);
    feed(vt, "\r\n\r\n");
    feed(vt, "and more text\r\n");
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=T,f=100,s=8,v=8,c=4,r=2,i=2,q=2,C=1;%s\x1b\\", PNG_B64);
    feed(vt, seq);
    feed(vt, "\r\n\r\n");
    feed(vt, "the end\r\n");
    dump(vt, "after two images");

    /* Scroll the first image deep into the scrollback. */
    for (int i = 0; i < 30; i++)
        feed(vt, "\x1b[24;1H\n");
    dump(vt, "after 30 scrolls");

    cfr_free(vt);
    return 0;
}
