/*
 * coffer — APC (Application Program Command) router.
 *
 * All APC sequences (ESC _ ... ESC \) are dispatched here. The first
 * byte of the payload determines the protocol:
 *   'G' → kitty graphics protocol (graphics.c)
 *   *   → Lottie animation protocol (lottie.c)
 *
 * On Windows, OSC 5555 routes to the same dispatcher.  The bundled
 * OpenConsole passes APC through, but the system conhost fallback strips
 * it, so OSC 5555 remains the portable carrier.
 */

#include "coffer_internal.h"

void cfr_apc_dispatch(CfrTerm *vt, const uint8_t *buf, size_t len)
{
    if (!buf || len == 0)
        return;

    if (buf[0] == 'G')
        cfr_graphics_apc_dispatch(vt, buf + 1, len - 1);
    else
        cfr_lottie_apc_dispatch(vt, buf, len);
}
