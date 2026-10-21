/* Probe: the 1:1 vs 1:N misclassification.
 *
 * A kitty image that was transmitted but NOT yet placed (a=t, the
 * normal two-step flow) has NO placements, so the store treats it as a
 * 1:1 image and culls it by its (placeholder) anchor — an ED or a
 * scroll-over destroys it. Show it.
 */
#include <coffer/coffer.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *PNG_B64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAgAAAAICAYAAADED76LAAAALklEQVR4nGNgkLOJqpi25dIHPh2vjLYlhx"
    "78k7GKKJuy6cI7Hi2PtJZFDCNDAQAHKnkB/VWi/wAAAABJRU5ErkJggg==";

static void feed(CfrTerm *vt, const char *s)
{
    cfr_input_write(vt, (const uint8_t *)s, strlen(s));
}

static int nimg(CfrTerm *vt)
{
    int n = 0;
    cfr_get_images(vt, &n);
    return n;
}
static int npl(CfrTerm *vt)
{
    int n = 0;
    cfr_get_image_placements(vt, &n);
    return n;
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
    /* transmit only (a=t): kitty's normal two-step flow. */
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=t,f=100,s=8,v=8,i=1,q=2;%s\x1b\\", PNG_B64);
    feed(vt, seq);
    printf("after a=t (transmit, no placement): %d image(s), %d placement(s)\n",
           nimg(vt), npl(vt));

    /* an ED2 (clear screen) — kitty's own image store keeps the image. */
    feed(vt, "\x1b[2J");
    printf("after ED2:                          %d image(s), %d placement(s)\n",
           nimg(vt), npl(vt));

    /* and a resize wipes everything. */
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=t,f=100,s=8,v=8,i=2,q=2;%s\x1b\\", PNG_B64);
    feed(vt, seq);
    printf("after another a=t:                  %d image(s), %d placement(s)\n",
           nimg(vt), npl(vt));
    cfr_resize(vt, 30, 100);
    printf("after a resize:                     %d image(s), %d placement(s)\n",
           nimg(vt), npl(vt));

    cfr_free(vt);
    return 0;
}
