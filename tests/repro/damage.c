/* Probe: does the kitty path mark the screen dirty?
 *
 * The host redraws only when coffer reports damage. A kitty placement
 * that damages nothing means the image lands on a screen the host never
 * repaints: a blank space where the image should be.
 */
#include <coffer/coffer.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *PNG_B64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAgAAAAICAYAAADED76LAAAALklEQVR4nGNgkLOJqpi25dIHPh2vjLYlhx"
    "78k7GKKJuy6cI7Hi2PtJZFDCNDAQAHKnkB/VWi/wAAAABJRU5ErkJggg==";

static int g_damage_calls;
static CfrRect g_last;
static int g_any_row;

static void on_damage(CfrRect r, void *u)
{
    (void)u;
    g_damage_calls++;
    g_last = r;
    printf("  damage: rows %d..%d cols %d..%d\n", r.start_row, r.end_row,
           r.start_col, r.end_col);
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
    CfrCallbacks cb = { 0 };
    cb.damage = on_damage;
    cfr_set_callbacks(vt, &cb, NULL);

    char seq[4096];
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=T,f=100,s=8,v=8,c=4,r=2,i=1,q=2,C=1;%s\x1b\\", PNG_B64);

    printf("feed kitty a=T (transmit+place):\n");
    feed(vt, seq);
    printf("  -> %d damage call(s) so far\n", g_damage_calls);

    printf("flush damage:\n");
    cfr_damage_flush(vt);
    printf("  -> %d damage call(s) total\n", g_damage_calls);

    printf("feed the row reservation (\\r\\n x2):\n");
    feed(vt, "\r\n\r\n");
    cfr_damage_flush(vt);
    printf("  -> %d damage call(s) total\n", g_damage_calls);

    printf("feed some text:\n");
    feed(vt, "hello");
    cfr_damage_flush(vt);
    printf("  -> %d damage call(s) total\n", g_damage_calls);

    cfr_free(vt);
    return 0;
}
