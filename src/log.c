/*
 * coffer — logging support.
 *
 * cfr_vlog formats a message and forwards it to the host's log callback
 * if one is registered. When no callback is set (the default), it is a
 * no-op: just a NULL pointer check, no output.
 */

#include "coffer_internal.h"

void cfr_vlog(CfrTerm *vt, CfrLogLevel level, const char *fmt, ...)
{
    if (!vt || !vt->callbacks.log)
        return;

    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    vt->callbacks.log(level, buf, vt->callback_user);
}
