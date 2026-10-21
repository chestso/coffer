/* Probe 2: the id-space collision, in the order that matters.
 *
 *  A) kitty (client id 1) FIRST, then an iTerm2 1:1 image.
 *     The 1:1 path takes an ENGINE id from the same counter the kitty
 *     path leaves untouched -> it lands on id 1 and REPLACES the kitty
 *     image's record (pixels + source).
 *  B) kitty (client id 1) then a Lottie animation with id 1.
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
        printf("    img[%d] id=%llu src=%u ver=%u row=%d col=%d %dx%d px "
               "rgba0=%d,%d,%d,%d\n",
               i, (unsigned long long)imgs[i].id, imgs[i].source, imgs[i].version,
               imgs[i].row, imgs[i].col, imgs[i].width_px, imgs[i].height_px,
               imgs[i].rgba[0], imgs[i].rgba[1], imgs[i].rgba[2], imgs[i].rgba[3]);
    for (int i = 0; i < pc; i++)
        printf("    pl[%d] image=%llu row=%d col=%d %dx%d\n", i,
               (unsigned long long)pls[i].image_id, pls[i].row, pls[i].col,
               pls[i].rows, pls[i].cols);
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

    /* A) kitty first. */
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=T,f=100,s=8,v=8,c=4,r=2,i=1,q=2,C=1;%s\x1b\\", PNG_B64);
    feed(vt, seq);
    dump(vt, "A1: kitty image, client id 1");

    /* now an iTerm2 inline image (1:1 protocol -> engine id) */
    snprintf(seq, sizeof(seq),
             "\x1b]1337;File=inline=1;size=103;width=4;height=2:%s\x07",
             PNG_B64);
    feed(vt, seq);
    dump(vt, "A2: after the iTerm2 image (engine id space)");

    cfr_free(vt);

    /* B) kitty client id 1 + lottie animation id 1. */
    CfrTerm *vt2 = cfr_new(&cfg);
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=T,f=100,s=8,v=8,c=4,r=2,i=1,q=2,C=1;%s\x1b\\", PNG_B64);
    feed(vt2, seq);
    dump(vt2, "B1: kitty image, client id 1");

    /* A minimal Lottie load: {"cmd":"load","id":1,"lottie":{...}} — the
     * lottie record takes the client's id in the SAME store. */
    const char *json =
        "{\"cmd\":\"load\",\"id\":1,\"lottie\":{\"v\":\"5.7.4\",\"fr\":30,"
        "\"ip\":0,\"op\":30,\"w\":16,\"h\":16,\"nm\":\"x\",\"ddd\":0,"
        "\"assets\":[],\"layers\":[]}}";
    char b64[1024];
    size_t oi = 0;
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t jl = strlen(json);
    for (size_t i = 0; i < jl; i += 3) {
        uint32_t v = (uint32_t)(unsigned char)json[i] << 16;
        if (i + 1 < jl)
            v |= (uint32_t)(unsigned char)json[i + 1] << 8;
        if (i + 2 < jl)
            v |= (uint32_t)(unsigned char)json[i + 2];
        b64[oi++] = tbl[(v >> 18) & 63];
        b64[oi++] = tbl[(v >> 12) & 63];
        b64[oi++] = (i + 1 < jl) ? tbl[(v >> 6) & 63] : '=';
        b64[oi++] = (i + 2 < jl) ? tbl[v & 63] : '=';
    }
    b64[oi] = 0;
    snprintf(seq, sizeof(seq), "\x1b_%s\x1b\\", b64);
    feed(vt2, seq);
    dump(vt2, "B2: after a lottie animation with id 1");

    cfr_free(vt2);
    return 0;
}
