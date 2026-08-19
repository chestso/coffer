/*
 * coffer — 256-color palette resolution.
 *
 * Layout:
 *   0-15:   ANSI base colors (Dracula, per draculatheme.com/spec)
 *   16-231: 6×6×6 cube
 *   232-255: greyscale ramp
 *
 * The 0-15 colors are the canonical Dracula ANSI palette from the
 * official specification at draculatheme.com/spec.
 *
 * The default terminal background is pitch-black (#000000), separate
 * from palette index 0 (AnsiBlack #21222C). The default foreground is
 * Dracula's foreground (#F8F8F2), which coincides with AnsiWhite
 * (palette index 7). Neither is user-configurable.
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

uint32_t cfr_palette_lookup(CfrTerm *vt, uint8_t idx)
{
    (void)vt; /* OSC 4 palette overrides will hook in here later. */
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
    return cfr_palette_lookup(NULL, index);
}
