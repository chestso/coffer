/*
 * coffer — DCS dispatcher.
 *
 * Streams DCS payload to the dcs callback chunk-by-chunk. The intro
 * (parameters + intermediates + final byte) is captured at hook time
 * and presented as a NUL-terminated string to the consumer; the body
 * is streamed via repeated calls with final=false until unhook.
 *
 * This matches libvterm's VTermStringFragment model and lets the sixel
 * parser (sixel.c) ingest data without ever holding a full image in
 * the VT layer.
 */

#include "coffer_internal.h"

#include <stdio.h>
#include <string.h>

static void format_intro(CfrTerm *vt, uint8_t final)
{
    CfrParser *p = &vt->parser;
    /* "<params>;<intermediates><final>" — params separated by ';',
     * intermediates concatenated. Truncated to fit dcs_intro buffer. */
    size_t pos = 0;
    char buf[64];
    for (int i = 0; i < p->param_count && pos < sizeof(buf) - 8; ++i) {
        if (i > 0)
            buf[pos++] = ';';
        int n = snprintf(buf + pos, sizeof(buf) - pos, "%u", p->params[i]);
        if (n < 0)
            break;
        pos += (size_t)n;
        if (pos >= sizeof(buf)) {
            pos = sizeof(buf) - 1;
            break;
        }
    }
    for (int i = 0; i < p->intermediate_count && pos < sizeof(buf) - 2; ++i)
        buf[pos++] = (char)p->intermediates[i];
    if (pos < sizeof(buf) - 1)
        buf[pos++] = (char) final;
    buf[pos] = '\0';

    size_t to_copy = pos;
    if (to_copy > sizeof(p->dcs_intro) - 1)
        to_copy = sizeof(p->dcs_intro) - 1;
    memcpy(p->dcs_intro, buf, to_copy);
    p->dcs_intro[to_copy] = '\0';
    p->dcs_intro_len = (uint8_t)to_copy;
}

void cfr_dcs_hook(CfrTerm *vt, uint8_t final)
{
    CfrParser *p = &vt->parser;
    format_intro(vt, final);
    p->dcs_initial_sent = true;

    /* Sixel (final byte 'q') is decoded internally by the engine, not
     * surfaced to the host dcs callback. The params (P1;P2;P3) were
     * collected into p->params before the final byte arrived. */
    p->dcs_is_sixel = (final == 'q');
    if (p->dcs_is_sixel) {
        cfr_sixel_begin(vt, p->params, p->param_count);
        return;
    }

    /* Initial chunk with empty body so the consumer can prepare state. */
    if (vt->callbacks.dcs)
        vt->callbacks.dcs((const char *)p->dcs_intro, NULL, 0, false, vt->callback_user);
}

void cfr_dcs_put(CfrTerm *vt, uint8_t b)
{
    CfrParser *p = &vt->parser;
    if (!p->dcs_initial_sent)
        return;
    if (p->dcs_is_sixel) {
        cfr_sixel_put(vt, &b, 1);
        return;
    }
    if (vt->callbacks.dcs)
        vt->callbacks.dcs((const char *)p->dcs_intro,
                          (const char *)&b, 1, false, vt->callback_user);
}

void cfr_dcs_unhook(CfrTerm *vt)
{
    CfrParser *p = &vt->parser;
    if (!p->dcs_initial_sent)
        return;
    if (p->dcs_is_sixel) {
        cfr_sixel_finish(vt);
        p->dcs_is_sixel = false;
        p->dcs_initial_sent = false;
        return;
    }
    if (vt->callbacks.dcs)
        vt->callbacks.dcs((const char *)p->dcs_intro, NULL, 0, true, vt->callback_user);
    p->dcs_initial_sent = false;
}
