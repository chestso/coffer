/*
 * coffer — 256-color palette resolution and dynamic colors.
 *
 * Layout:
 *   0-15:   ANSI base colors (Dracula, per draculatheme.com/spec)
 *   16-231: 6×6×6 cube
 *   232-255: greyscale ramp
 *
 * The 0-15 colors are the canonical Dracula ANSI palette from the
 * official specification at draculatheme.com/spec.
 *
 * The built-in default background is pitch-black (#000000), separate
 * from palette index 0 (AnsiBlack #21222C). The built-in default
 * foreground is Dracula's foreground (#F8F8F2), which coincides with
 * AnsiWhite (palette index 7). Both are the fallback for the dynamic
 * colors (xterm OSC 10/11/12): a CfrConfig seed or an OSC set
 * overrides them, OSC 110/111/112 and RIS restore the seed.
 */

#include "coffer_internal.h"

static const uint8_t base16[16][3] = {
    { 0x21, 0x22, 0x2c }, /* 0  black          */
    { 0xff, 0x55, 0x55 }, /* 1  red            */
    { 0x50, 0xfa, 0x7b }, /* 2  green          */
    { 0xf1, 0xfa, 0x8c }, /* 3  yellow         */
    { 0xbd, 0x93, 0xf9 }, /* 4  blue           */
    { 0xff, 0x79, 0xc6 }, /* 5  magenta        */
    { 0x8b, 0xe9, 0xfd }, /* 6  cyan           */
    { 0xf8, 0xf8, 0xf2 }, /* 7  white          */
    { 0x62, 0x72, 0xa4 }, /* 8  bright black   */
    { 0xff, 0x6e, 0x6e }, /* 9  bright red     */
    { 0x69, 0xff, 0x94 }, /* 10 bright green   */
    { 0xff, 0xff, 0xa5 }, /* 11 bright yellow  */
    { 0xd6, 0xac, 0xff }, /* 12 bright blue    */
    { 0xff, 0x92, 0xdf }, /* 13 bright magenta */
    { 0xa4, 0xff, 0xff }, /* 14 bright cyan    */
    { 0xff, 0xff, 0xff }, /* 15 bright white   */
};

static uint32_t builtin_palette_rgb(uint8_t idx)
{
    if (idx < 16) {
        const uint8_t *c = base16[idx];
        return ((uint32_t)c[0] << 16) | ((uint32_t)c[1] << 8) | c[2];
    }
    if (idx < 232) {
        /* 6×6×6 cube. Each step: {0, 95, 135, 175, 215, 255}. */
        static const uint8_t levels[6] = { 0x00, 0x5f, 0x87, 0xaf, 0xd7, 0xff };
        uint8_t v = idx - 16;
        uint8_t r = levels[(v / 36) % 6];
        uint8_t g = levels[(v / 6) % 6];
        uint8_t b = levels[v % 6];
        return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    }
    /* 24-step grey: 8 + 10*(idx-232). */
    uint8_t v = (uint8_t)(8 + 10 * (idx - 232));
    return ((uint32_t)v << 16) | ((uint32_t)v << 8) | v;
}

/* Seed the live palette from the built-ins. Fixed-size, allocation-free. */
void cfr_palette_init(CfrTerm *vt)
{
    for (int i = 0; i < 256; i++)
        vt->palette[i] = builtin_palette_rgb((uint8_t)i);
}

/* Change one entry. Returns true when the effective value changed, so the
 * caller can damage the grid once per OSC instead of per entry. */
bool cfr_palette_set(CfrTerm *vt, uint8_t idx, uint32_t rgb)
{
    rgb &= 0x00FFFFFFu;
    if (vt->palette[idx] == rgb)
        return false;
    vt->palette[idx] = rgb;
    return true;
}

/* Restore one entry to its built-in value. Returns true on a change. */
bool cfr_palette_reset(CfrTerm *vt, uint8_t idx)
{
    uint32_t builtin = builtin_palette_rgb(idx);
    if (vt->palette[idx] == builtin)
        return false;
    vt->palette[idx] = builtin;
    return true;
}

/* Restore every entry (OSC 104 with no parameters, RIS). Returns true if
 * any entry changed. */
bool cfr_palette_reset_all(CfrTerm *vt)
{
    bool changed = false;
    for (int i = 0; i < 256; i++) {
        uint32_t builtin = builtin_palette_rgb((uint8_t)i);
        if (vt->palette[i] != builtin) {
            vt->palette[i] = builtin;
            changed = true;
        }
    }
    return changed;
}

uint32_t cfr_palette_rgb(const CfrTerm *vt, uint8_t index)
{
    if (!vt)
        return builtin_palette_rgb(index);
    return vt->palette[index];
}

uint32_t cfr_default_bg_rgb(void)
{
    return 0x000000u; /* pitch-black */
}

uint32_t cfr_default_fg_rgb(void)
{
    return 0xF8F8F2u; /* Dracula foreground */
}

uint32_t cfr_default_palette_rgb(uint8_t index)
{
    return builtin_palette_rgb(index);
}

/* Resolution rule for one style color. A NULL terminal has no dynamic
 * colors or overrides, so it falls back to the configured defaults. */
static uint32_t style_color_rgb(const CfrTerm *vt, const CfrStyleColor *c,
                                int dyn_idx)
{
    switch (c->kind) {
    case CFR_COLOR_KIND_RGB:
        return c->rgb;
    case CFR_COLOR_KIND_INDEXED:
        return cfr_palette_rgb(vt, c->index);
    default:
        if (vt)
            return cfr_dyn_color_effective(vt, dyn_idx);
        return (dyn_idx == CFR_DYN_BG) ? cfr_default_bg_rgb()
                                       : cfr_default_fg_rgb();
    }
}

uint32_t cfr_style_fg_rgb(const CfrTerm *vt, const CfrStyle *style)
{
    return style_color_rgb(vt, &style->fg, CFR_DYN_FG);
}

uint32_t cfr_style_bg_rgb(const CfrTerm *vt, const CfrStyle *style)
{
    return style_color_rgb(vt, &style->bg, CFR_DYN_BG);
}

uint32_t cfr_style_ul_rgb(const CfrTerm *vt, const CfrStyle *style)
{
    /* xterm's default underline color is the default foreground. */
    return style_color_rgb(vt, &style->ul, CFR_DYN_FG);
}

/* ------------------------------------------------------------------ */
/* Dynamic colors (xterm OSC 10/11/12 family)                          */
/* ------------------------------------------------------------------ */

/* Seed the dynamic colors from the config (or the built-in defaults)
 * and apply them. No notifications: this runs inside cfr_new(), before
 * any host can have registered callbacks. */
void cfr_dyn_color_init(CfrTerm *vt, const CfrConfig *cfg)
{
    vt->dyn_color_seed[CFR_DYN_FG] = cfg->fg_color;
    vt->dyn_color_seed[CFR_DYN_BG] = cfg->bg_color;
    vt->dyn_color_seed[CFR_DYN_CURSOR] = cfg->cursor_color;
    if (!vt->dyn_color_seed[CFR_DYN_FG].set)
        vt->dyn_color_seed[CFR_DYN_FG].rgb = cfr_default_fg_rgb();
    if (!vt->dyn_color_seed[CFR_DYN_BG].set)
        vt->dyn_color_seed[CFR_DYN_BG].rgb = cfr_default_bg_rgb();
    for (int i = 0; i < CFR_DYN_COUNT; i++)
        vt->dyn_color[i] = vt->dyn_color_seed[i];
}

/* The value a query reports and a renderer draws: the explicit color
 * when one is in effect, else the engine default. The cursor's default
 * is the foreground — xterm's cursorColor default is
 * XtDefaultForeground, and its reverse-video cursor is drawn in it. */
uint32_t cfr_dyn_color_effective(const CfrTerm *vt, int idx)
{
    const CfrColorValue *c = &vt->dyn_color[idx];
    if (c->set)
        return c->rgb;
    if (idx == CFR_DYN_CURSOR)
        return cfr_dyn_color_effective(vt, CFR_DYN_FG);
    return c->rgb;
}

/* Apply a dynamic color value: remember it, dirty what renders with it
 * (default-colored cells for fg/bg, the cursor cell for the cursor
 * color) and tell the host — once, and only on a real change. */
void cfr_dyn_color_set(CfrTerm *vt, int idx, bool set, uint32_t rgb)
{
    CfrColorValue *slot = &vt->dyn_color[idx];
    if (slot->set == set && slot->rgb == rgb)
        return;
    slot->set = set;
    slot->rgb = rgb;
    if (idx == CFR_DYN_CURSOR)
        cfr_damage_cursor(vt);
    else
        cfr_damage_all(vt);
    if (vt->callbacks.set_color)
        vt->callbacks.set_color(idx + 10, cfr_dyn_color_effective(vt, idx),
                                set, vt->callback_user);
}

void cfr_dyn_color_reset(CfrTerm *vt, int idx)
{
    cfr_dyn_color_set(vt, idx, vt->dyn_color_seed[idx].set,
                      vt->dyn_color_seed[idx].rgb);
}

void cfr_dyn_color_reset_all(CfrTerm *vt)
{
    for (int i = 0; i < CFR_DYN_COUNT; i++)
        cfr_dyn_color_reset(vt, i);
}

bool cfr_get_dynamic_color(const CfrTerm *vt, int code, uint32_t *out_rgb,
                           bool *out_set)
{
    if (!vt || code < 10 || code > 12)
        return false;
    int idx = code - 10;
    if (out_rgb)
        *out_rgb = cfr_dyn_color_effective(vt, idx);
    if (out_set)
        *out_set = vt->dyn_color[idx].set;
    return true;
}
