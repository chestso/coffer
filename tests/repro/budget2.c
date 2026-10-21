/* Probe: the budget eviction, in a SINGLE write.
 *
 * The previous probe fed the 21 MB payload in one cfr_input_write() and
 * the FIRST image never landed. Suspect: the APC buffer's growth
 * (CFR_OSC_BUF_MAX 256 MB) is fine, but the payload is a single
 * APC string of ~28 MB base64 -> the parser's apc_buf doubles from
 * 64 KB. That should work... unless the base64 decode of a 21 MB
 * string allocates a 16 MB "clean" copy and the raw buffer.
 *
 * The first image missing while the second is present smells like an
 * off-by-one in the store's live_bytes accounting, or a failed
 * allocation. Let me watch the reply codes: q=2 suppresses errors, so
 * drop q=2 to see the engine's answer.
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

static void feed(CfrTerm *vt, const char *s)
{
    cfr_input_write(vt, (const uint8_t *)s, strlen(s));
}

static char *b64_zeros(size_t n)
{
    size_t b64len = (n / 3) * 4 + 4;
    char *out = malloc(b64len + 1);
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t oi = 0;
    for (size_t i = 0; i < n; i += 3) {
        out[oi++] = tbl[0];
        out[oi++] = tbl[0];
        out[oi++] = (i + 1 < n) ? tbl[0] : '=';
        out[oi++] = (i + 2 < n) ? tbl[0] : '=';
    }
    out[oi] = 0;
    return out;
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

    const int W = 2000, H = 2000;
    char *payload = b64_zeros((size_t)W * H * 4);
    size_t plen = strlen(payload);
    printf("payload b64: %zu bytes\n", plen);

    for (int id = 1; id <= 10; id++) {
        char *seq = malloc(plen + 200);
        int n = snprintf(seq, 200, "\x1b_Ga=t,f=32,s=%d,v=%d,i=%d;", W, H, id);
        memcpy(seq + n, payload, plen);
        seq[n + plen] = 0x1b;   /* ST */
        seq[n + plen + 1] = 0x5c;
        seq[n + plen + 2] = 0;
        feed(vt, seq);
        free(seq);

        int cnt = 0;
        const CfrImage *imgs = cfr_get_images(vt, &cnt);
        printf("after #%d: %d image(s):", id, cnt);
        for (int i = 0; i < cnt; i++)
            printf(" %llu", (unsigned long long)imgs[i].id);
        printf("   reply: %s\n", g_out);
        g_out_len = 0;
        g_out[0] = 0;
    }
    free(payload);
    cfr_free(vt);
    return 0;
}
