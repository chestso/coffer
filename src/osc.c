/*
 * coffer — OSC dispatcher.
 *
 * Parses the leading numeric code and routes title-setting (0/1/2) to
 * the set_title callback, hands OSC 8 to the hyperlink dispatcher, and
 * forwards everything else to the generic osc callback.
 *
 * OSC 8 — hyperlink protocol (de-facto spec by Egmont Koblinger):
 *   https://gist.github.com/egmontkob/eb114294efbcd5adb1944c9f3cb5feda
 * Sequence: OSC 8 ; params ; URI ST  (BEL also accepted).
 * Empty URI closes the active link. params is ':'-separated key=value
 * pairs (only `id` is defined); for v1 we parse-and-discard params and
 * use the URI alone — same-URI dedup gives renderers free run-continuity.
 */

#include "coffer_internal.h"

#include <stdlib.h>
#include <string.h>

static int parse_code(const uint8_t *data, size_t len, size_t *out_offset)
{
    size_t i = 0;
    int code = -1;
    while (i < len && data[i] >= '0' && data[i] <= '9') {
        if (code < 0)
            code = 0;
        code = code * 10 + (data[i] - '0');
        i++;
    }
    if (i < len && data[i] == ';')
        i++;
    *out_offset = i;
    return code;
}

/* OSC 8 dispatcher. Body shape: `params;URI`. The URI runs from the
 * first ';' to the end. An empty URI ('') closes the active hyperlink. */
static void osc8_dispatch(CfrTerm *vt, const uint8_t *body, size_t body_len)
{
    /* Commit any pending cluster before mutating the pen — same pattern
     * as csi.c / esc.c / modes.c. Without this the previous link's text
     * (still in cluster_buf) would be stamped with the new id. */
    cfr_flush_cluster(vt);

    /* Find the first ';' separating params from URI. Per spec the
     * params field cannot contain ';', so a literal scan is correct. */
    size_t sep = 0;
    while (sep < body_len && body[sep] != ';')
        sep++;
    if (sep >= body_len) {
        /* Malformed — no second ';'. Treat as unlink (defensive). */
        vt->cursor.hyperlink_id = 0;
        return;
    }
    const uint8_t *uri = body + sep + 1;
    size_t uri_len = body_len - sep - 1;

    if (uri_len == 0) {
        vt->cursor.hyperlink_id = 0;
        return;
    }
    /* Lazy grid alloc — interning lives on the active grid page. */
    cfr_grid_ensure(vt);
    vt->cursor.hyperlink_id =
        cfr_hyperlink_intern(vt, vt->grid, uri, (uint32_t)uri_len);
}

static void set_title(CfrTerm *vt, const uint8_t *data, size_t len)
{
    cfr_dealloc(vt, vt->title);
    vt->title = cfr_alloc(vt, len + 1);
    if (!vt->title)
        return;
    memcpy(vt->title, data, len);
    vt->title[len] = '\0';
    if (vt->callbacks.set_title)
        vt->callbacks.set_title(vt->title, vt->callback_user);
}

void cfr_osc_dispatch(CfrTerm *vt, const uint8_t *data, size_t len)
{
    size_t off = 0;
    int code = parse_code(data, len, &off);

    const uint8_t *body = data + off;
    size_t body_len = (off > len) ? 0 : len - off;

    switch (code) {
    case 0:
    case 1:
    case 2:
        set_title(vt, body, body_len);
        break;
    case 8:
        osc8_dispatch(vt, body, body_len);
        break;
    case 4:
        if (should_log_once(vt, CFR_LOGGED_OSC4))
            cfr_log(vt, CFR_LOG_WARN, "palette override (OSC 4) not implemented yet");
        break;
    case 104:
        if (should_log_once(vt, CFR_LOGGED_OSC104))
            cfr_log(vt, CFR_LOG_WARN, "palette reset (OSC 104) not implemented yet");
        break;
    case 5555:
        /* OSC 5555 — Lottie APC carrier for ConPTY on Windows.
         *
         * Windows ConPTY strips APC (ESC _) sequences even with
         * PSEUDOCONSOLE_PASSTHROUGH_MODE on some builds.  To get
         * Lottie commands through, the client encodes the APC
         * payload (base64 of the JSON command) inside OSC 5555,
         * which ConPTY does pass through.  The body after the
         * semicolon is identical to what would have been the APC
         * string body. */
        cfr_lottie_apc_dispatch(vt, body, body_len);
        break;
    default:
        if (vt->callbacks.osc) {
            /* The data is in osc_buf which is uint8_t[]; the
             * callback signature uses const char* for convenience.
             * NUL-termination is not guaranteed; len is authoritative. */
            vt->callbacks.osc(code, (const char *)body, body_len, vt->callback_user);
        }
        break;
    }
}
