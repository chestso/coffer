/* Probe: the host-side texture upload at content_scale != 1.
 *
 * portty's image_get_texture() does:
 *     bw = img->buf_w; bh = img->buf_h;
 *     linearize_for_upload(data, img->rgba, bw, bh);   // reads bw*bh*4 bytes
 *     SDL_UpdateTexture(tex, NULL, pixels, bw * 4);
 *
 * coffer's cfr_img_get() reports buf_w/buf_h MULTIPLIED by
 * content_scale, but the pixel buffer is the DECODED (native) size.
 * At scale 2 the host reads twice the buffer's size.
 *
 * Built with -fsanitize=address: the overread is the point.
 */
#include <coffer/coffer.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *PNG_B64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAgAAAAICAYAAADED76LAAAALklEQVR4nGNgkLOJqpi25dIHPh2vjLYlhx"
    "78k7GKKJuy6cI7Hi2PtJZFDCNDAQAHKnkB/VWi/wAAAABJRU5ErkJggg==";

int main(void)
{
    const int S = 2; /* content_scale 2.0 (a HiDPI display) */
    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.rows = 24;
    cfg.cols = 80;
    cfg.cell_w_px = 10 * S;
    cfg.cell_h_px = 20 * S;
    CfrTerm *vt = cfr_new(&cfg);
    cfr_set_content_scale(vt, (float)S);

    char seq[4096];
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=T,f=100,s=8,v=8,c=4,r=2,i=1,q=2,C=1;%s\x1b\\", PNG_B64);
    cfr_input_write(vt, (const uint8_t *)seq, strlen(seq));

    int n = 0;
    const CfrImage *imgs = cfr_get_images(vt, &n);
    if (n != 1) {
        printf("no image\n");
        return 1;
    }
    const CfrImage *img = &imgs[0];
    printf("engine reports: width_px=%d height_px=%d buf_w=%d buf_h=%d\n",
           img->width_px, img->height_px, img->buf_w, img->buf_h);

    /* What the host does: a full copy of buf_w*buf_h*4 bytes. */
    size_t need = (size_t)img->buf_w * (size_t)img->buf_h * 4;
    printf("host would read %zu bytes from the engine's buffer\n", need);

    volatile uint8_t sink = 0;
    uint8_t *copy = malloc(need);
    if (!copy)
        return 1;
    memcpy(copy, img->rgba, need); /* <-- the overread */
    for (size_t i = 0; i < need; i++)
        sink = (uint8_t)(sink + copy[i]);
    printf("read %zu bytes, sink=%d\n", need, (int)sink);
    free(copy);
    cfr_free(vt);
    return 0;
}
