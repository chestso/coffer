/* Repro harness: kitty graphics via coffer, as boba/nevermore emit it.
 *
 * 1. transmit + place (a=T) with a PNG payload
 * 2. check image + placement state
 * 3. scroll and observe culling
 * 4. re-transmit the same id and observe placement loss
 */
#include <coffer/coffer.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static char g_out[4096];
static size_t g_out_len;

static void on_output(const uint8_t *b, size_t n, void *u)
{
    (void)u;
    if (g_out_len + n >= sizeof(g_out))
        return;
    memcpy(g_out + g_out_len, b, n);
    g_out_len += n;
    g_out[g_out_len] = 0;
}

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
        printf("    img[%d] id=%llu src=%u ver=%u row=%d col=%d %dx%d buf=%dx%d "
               "rgba0=%d,%d,%d,%d\n",
               i, (unsigned long long)imgs[i].id, imgs[i].source, imgs[i].version,
               imgs[i].row, imgs[i].col, imgs[i].width_px, imgs[i].height_px,
               imgs[i].buf_w, imgs[i].buf_h, imgs[i].rgba[0], imgs[i].rgba[1],
               imgs[i].rgba[2], imgs[i].rgba[3]);
    for (int i = 0; i < pc; i++)
        printf("    pl[%d] id=%llu image=%llu row=%d col=%d %dx%d cells z=%d "
               "src=%d,%d %dx%d\n",
               i, (unsigned long long)pls[i].id,
               (unsigned long long)pls[i].image_id, pls[i].row, pls[i].col,
               pls[i].rows, pls[i].cols, pls[i].z_index, pls[i].src_x,
               pls[i].src_y, pls[i].src_w, pls[i].src_h);
}

int main(void)
{
    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.rows = 24;
    cfg.cols = 80;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 20;
    CfrTerm *vt = cfr_new(&cfg);
    CfrCallbacks cb = { 0 };
    cb.output = on_output;
    cfr_set_callbacks(vt, &cb, NULL);
    cfr_set_cell_pixels(vt, 10, 20);

    char seq[4096];

    /* boba's exact head: a=T,f=100,s=8,v=8,c=4,r=2,i=1,q=2,C=1 */
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=T,f=100,s=8,v=8,c=4,r=2,i=1,q=2,C=1;%s\x1b\\", PNG_B64);
    cfr_input_write(vt, (const uint8_t *)seq, strlen(seq));
    dump(vt, "after a=T transmit+place");
    printf("    output: %s\n", g_out);

    /* boba then emits the reserved rows: \r\n x disp_rows */
    cfr_input_write(vt, (const uint8_t *)"\r\n\r\n", 4);
    dump(vt, "after row reservation");

    /* Scroll a few lines. */
    cfr_input_write(vt, (const uint8_t *)"\x1b[24;1H\n\n\n", 12);
    dump(vt, "after 3 scrolls");

    /* Re-transmit the same id (a=T again) — kitty says the placements die. */
    g_out_len = 0;
    g_out[0] = 0;
    cfr_input_write(vt, (const uint8_t *)seq, strlen(seq));
    dump(vt, "after re-transmit same id");
    printf("    output: %s\n", g_out);

    cfr_free(vt);
    return 0;
}
