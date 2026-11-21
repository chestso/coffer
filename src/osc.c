/*
 * coffer — OSC dispatcher.
 *
 * Parses the leading numeric code and routes title-setting (0/1/2) to
 * the set_title callback, hands OSC 8 to the hyperlink dispatcher,
 * OSC 10/11/12 (+110/111/112) to the dynamic color handlers below,
 * and forwards everything else to the generic osc callback.
 *
 * OSC 8 — hyperlink protocol (de-facto spec by Egmont Koblinger):
 *   https://gist.github.com/egmontkob/eb114294efbcd5adb1944c9f3cb5feda
 * Sequence: OSC 8 ; params ; URI ST  (BEL also accepted).
 * Empty URI closes the active link. params is ':'-separated key=value
 * pairs; only `id` is defined. The id is part of the link's identity:
 * per the spec it connects cells only when their URIs also match, and an
 * empty id is the same as no id. Parsing mirrors kitty's parse_osc_8():
 * the first whole `id=` token with a nonempty value wins, and the value
 * is capped at CFR_OSC8_ID_MAX (kitty's MAX_ID_LEN).
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

/* Find the `id=` parameter in the params field. Mirrors kitty's
 * parse_osc_8(): params are ':'-separated tokens, empty tokens are
 * skipped, and the first whole `id=` token with a nonempty value wins
 * (so `xid=` is not `id=`, and a later `id=` cannot override it). Longer
 * values are truncated to CFR_OSC8_ID_MAX bytes, as kitty does. */
static bool osc8_parse_id(const uint8_t *params, size_t len,
                          const uint8_t **out, size_t *out_len)
{
    size_t i = 0;
    while (i < len) {
        while (i < len && params[i] == ':')
            i++; /* strtok_r skips leading and repeated delimiters */
        size_t start = i;
        while (i < len && params[i] != ':')
            i++;
        size_t tlen = i - start;
        if (tlen > 3 && params[start] == 'i' && params[start + 1] == 'd' &&
            params[start + 2] == '=' && params[start + 3]) {
            size_t vlen = tlen - 3;
            if (vlen > CFR_OSC8_ID_MAX)
                vlen = CFR_OSC8_ID_MAX;
            *out = params + start + 3;
            *out_len = vlen;
            return true;
        }
    }
    return false;
}

/* OSC 8 dispatcher. Body shape: `params;URI`. The URI runs from the
 * first ';' to the end. An empty URI ('') closes the active hyperlink. */
static void osc8_dispatch(CfrTerm *vt, const uint8_t *body, size_t body_len)
{
    /* The pending cluster was already committed by cfr_osc_dispatch
     * before the pen is mutated here — same pattern as csi.c / esc.c /
     * modes.c. Without that flush the previous link's text (still in
     * cluster_buf) would be stamped with the new id. */

    /* Find the first ';' separating params from URI. Per spec the
     * params field cannot contain ';', so a literal scan is correct. A
     * body without one is malformed; kitty ignores it rather than
     * closing the active link, and so do we. */
    size_t sep = 0;
    while (sep < body_len && body[sep] != ';')
        sep++;
    if (sep >= body_len)
        return;

    const uint8_t *id = NULL;
    size_t id_len = 0;
    (void)osc8_parse_id(body, sep, &id, &id_len);

    const uint8_t *uri = body + sep + 1;
    size_t uri_len = body_len - sep - 1;

    /* An empty URI closes the active link whether or not params carry an
     * id: the id only narrows identity, it is not a link of its own. */
    cfr_link_release(vt, vt->cursor.link);
    vt->cursor.link =
        uri_len == 0 ? NULL
                     : cfr_link_new(vt, uri, (uint32_t)uri_len, id,
                                    (uint32_t)id_len);
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

/*
 * OSC 10 / 11 / 12 — dynamic colors (xterm).
 *
 * Codes 10 (default foreground), 11 (default background) and 12
 * (cursor color) set, query and reset the dynamic colors:
 *
 *   OSC 10 ; Pt ST    Pt = a color spec, or "?" to query
 *   OSC 110 ; ST      reset the foreground to its CfrConfig value
 *
 * Each successive Pt applies to the next color in the list (the OSC
 * code is the starting point), so `OSC 10 ; fg ; bg ; cursor ST` sets
 * all three — the xterm behaviour. Codes 13-19 (pointer, Tektronix,
 * highlight colors) are not modeled and stay with the generic osc
 * callback.
 *
 * Many image viewers (chafa, viu, etc.) send `OSC 11 ?` to detect the
 * terminal background so they can alpha-composite semi-transparent
 * pixels against it before encoding to sixel. Without a response, the
 * query hangs or falls back to a default, and partial transparency is
 * lost.
 *
 * Color specs are the XParseColor RGB forms: `rgb:R[/G[/B]]` (1-4 hex
 * digits per component, missing components 0) and `#RGB` / `#RRGGBB` /
 * `#RRRRRGGGGGGBBBBBB`. X color names are out of scope (logged once).
 */

/* The value one hex digit stands for at each width, per XParseColor:
 * a component of `digits` hex digits covers 0..16^digits - 1 and is
 * scaled to 0..255 rounded to nearest. */
static uint32_t scale_hex_component(uint32_t v, int digits)
{
    static const uint32_t max[5] = { 0, 0xfu, 0xffu, 0xfffu, 0xffffu };
    return (v * 255u + max[digits] / 2u) / max[digits];
}

static int hex_digit(uint8_t c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Parse an XParseColor RGB specification into 0x00RRGGBB. */
static bool parse_color_spec(const uint8_t *s, size_t len, uint32_t *out_rgb)
{
    uint32_t comp[3] = { 0, 0, 0 };

    if (len >= 4 && memcmp(s, "rgb:", 4) == 0) {
        size_t pos = 4;
        int parsed = 0;
        for (int i = 0; i < 3 && pos < len; i++) {
            uint32_t v = 0;
            int digits = 0;
            while (pos < len && s[pos] != '/') {
                int h = hex_digit(s[pos]);
                if (h < 0 || digits == 4)
                    return false;
                v = v * 16u + (uint32_t)h;
                digits++;
                pos++;
            }
            if (digits == 0)
                return false;
            comp[i] = scale_hex_component(v, digits);
            parsed++;
            if (i < 2 && pos < len)
                pos++; /* skip the '/' */
        }
        if (parsed == 0 || pos != len)
            return false;
    } else if (len >= 1 && s[0] == '#') {
        int width = ((int)len - 1) / 3;
        if (len != 4 && len != 7 && len != 13) /* #RGB, #RRGGBB, #RRRRRGGGGGGBBBBBB */
            return false;
        for (int i = 0; i < 3; i++) {
            uint32_t v = 0;
            for (int k = 0; k < width; k++) {
                int h = hex_digit(s[1 + i * width + k]);
                if (h < 0)
                    return false;
                v = v * 16u + (uint32_t)h;
            }
            comp[i] = scale_hex_component(v, width);
        }
    } else {
        return false;
    }

    *out_rgb = (comp[0] << 16) | (comp[1] << 8) | comp[2];
    return true;
}

/* Query reply, xterm's form: OSC code ; rgb:RRRR/GGGG/BBBB ST (16-bit
 * per channel). One reply per queried color. */
static void emit_color_report(CfrTerm *vt, int code, uint32_t rgb)
{
    unsigned r = (rgb >> 16) & 0xffu;
    unsigned g = (rgb >> 8) & 0xffu;
    unsigned b = rgb & 0xffu;
    char buf[48];
    int n = snprintf(buf, sizeof(buf), "\x1b]%d;rgb:%04x/%04x/%04x\x1b\\",
                     code, r * 0x101u, g * 0x101u, b * 0x101u);
    if (n > 0)
        cfr_emit_bytes(vt, (const uint8_t *)buf, (size_t)n);
}

static void dyn_color_spec(CfrTerm *vt, int code, const uint8_t *spec, size_t len)
{
    if (len == 0)
        return;
    if (len == 1 && spec[0] == '?') {
        emit_color_report(vt, code, cfr_dyn_color_effective(vt, code - 10));
        return;
    }
    uint32_t rgb;
    if (!parse_color_spec(spec, len, &rgb)) {
        if (should_log_once(vt, CFR_LOGGED_BAD_COLOR))
            cfr_log(vt, CFR_LOG_WARN,
                    "unparseable color specification in OSC %d", code);
        return;
    }
    cfr_dyn_color_set(vt, code - 10, true, rgb);
}

/* One spec per color starting at `code`; each successive parameter
 * changes the next color in the list. */
static void osc_dyn_colors(CfrTerm *vt, int code, const uint8_t *body, size_t body_len)
{
    size_t pos = 0;
    for (int c = code; c <= 12; c++) {
        size_t end = pos;
        while (end < body_len && body[end] != ';')
            end++;
        dyn_color_spec(vt, c, body + pos, end - pos);
        if (end >= body_len)
            break;
        pos = end + 1;
    }
}

void cfr_osc_dispatch(CfrTerm *vt, const uint8_t *data, size_t len)
{
    /* Commit any pending cluster before the OSC is dispatched — the
     * same boundary csi.c / esc.c / modes.c draw. An OSC can move the
     * pen (OSC 8's link), the cursor (OSC 1337's image anchor) or
     * render state, and a cluster left pending would be committed
     * afterwards with the NEW state: the character before the OSC
     * would take the link/image position that belongs after it. The
     * flush lives here, not in the individual handlers, so every OSC
     * code (including the generic callback's) gets it for free. */
    cfr_flush_cluster(vt);

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
    case 10:
    case 11:
    case 12:
        osc_dyn_colors(vt, code, body, body_len);
        break;
    case 110:
    case 111:
    case 112:
        /* Reset one dynamic color to its configured default (xterm:
         * OSC 110 foreground, 111 background, 112 cursor color). */
        cfr_dyn_color_reset(vt, code - 110);
        break;
    case 4:
        if (should_log_once(vt, CFR_LOGGED_OSC4))
            cfr_log(vt, CFR_LOG_WARN, "palette override (OSC 4) not implemented yet");
        break;
    case 1337:
        cfr_osc_1337_dispatch(vt, body, body_len);
        break;
    case 104:
        if (should_log_once(vt, CFR_LOGGED_OSC104))
            cfr_log(vt, CFR_LOG_WARN, "palette reset (OSC 104) not implemented yet");
        break;
    case 5555:
        /* OSC 5555 — APC carrier for the system ConPTY fallback.
         *
         * The bundled OpenConsole passes APC (ESC _) through, but the
         * system conhost.exe fallback strips it.  Clients encode the APC
         * payload inside OSC 5555, which conhost does pass through.  The
         * body after the semicolon is the raw APC payload — NOT
         * base64-encoded for the kitty graphics path (starts with G).
         * For the Lottie path, the body IS base64-encoded JSON, and the
         * Lottie handler decodes it internally. The APC router checks
         * the first byte: G → graphics, else → lottie. */
        cfr_apc_dispatch(vt, body, body_len);
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
