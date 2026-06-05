/*
 * bloom-vt — 256-color palette resolution.
 *
 * Layout:
 *   0-15:   ANSI base colors (Charmbracelet CharmTone, per charm.land)
 *   16-231: 6×6×6 cube
 *   232-255: greyscale ramp
 *
 * The 0-15 colors are the canonical CharmTone hexes
 * (github.com/charmbracelet/x/exp/charmtone), named in the comments below.
 * Bright white (15) stays #fffdf5 to match charm.land's body text and the
 * default foreground, rather than CharmTone's Butter (#fffaf1).
 *
 * Homage to Charmbracelet (charm.land) — thanks for the gorgeous palette. 🌸
 */

#include "bloom_vt_internal.h"

static const uint8_t base16[16][3] = {
    { 0x20, 0x1f, 0x26 }, /* 0  black          Pepper  */
    { 0xff, 0x57, 0x7d }, /* 1  red            Coral   */
    { 0x12, 0xc7, 0x8f }, /* 2  green          Guac    */
    { 0xf5, 0xef, 0x34 }, /* 3  yellow         Mustard */
    { 0x6b, 0x50, 0xff }, /* 4  blue           Charple */
    { 0xff, 0x60, 0xff }, /* 5  magenta        Dolly   */
    { 0x0a, 0xdc, 0xd9 }, /* 6  cyan           Turtle  */
    { 0xbf, 0xbc, 0xc8 }, /* 7  white          Smoke   */
    { 0x60, 0x5f, 0x6b }, /* 8  bright black   Oyster  */
    { 0xff, 0x7f, 0x90 }, /* 9  bright red     Salmon  */
    { 0x00, 0xff, 0xb2 }, /* 10 bright green   Julep   */
    { 0xe8, 0xfe, 0x96 }, /* 11 bright yellow  Zest    */
    { 0x8b, 0x75, 0xff }, /* 12 bright blue    Hazy    */
    { 0xff, 0x84, 0xff }, /* 13 bright magenta Blush   */
    { 0x68, 0xff, 0xd6 }, /* 14 bright cyan    Bok     */
    { 0xff, 0xfd, 0xf5 }, /* 15 bright white   (cream) */
};

uint32_t bvt_palette_lookup(BvtTerm *vt, uint8_t idx)
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
