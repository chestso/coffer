/*
 * coffer — script engine: script file parser.
 *
 * Parses the line-oriented script vocabulary shared by cfr-debug and
 * portty into an immutable command list. One command per line, `#`
 * comments, blank lines skipped. Unknown commands are a parse error
 * unless the host whitelisted them as extensions (ext_names).
 *
 * Escape set (expanded at parse time in send/sendln/emit/wait-for
 * payloads): \n \r \t \e \\ \xNN. `\033` is deliberately NOT expanded.
 */

#include "script_internal.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static char *dup_str(const char *s)
{
    size_t len = strlen(s) + 1;
    char *copy = malloc(len);
    if (copy)
        memcpy(copy, s, len);
    return copy;
}

/* ------------------------------------------------------------------ */
/* Core command registry (drives parsing and --help-spec listings)    */
/* ------------------------------------------------------------------ */

static const CfrScriptCmdDef COMMANDS[] = {
    { "wait", "SECONDS", "Drain for N seconds (accepts decimals)" },
    { "wait-for", "\"TEXT\" [TIMEOUT] [line-end]",
      "Wait until TEXT appears in the unified buffer; TIMEOUT "
      "(default 600) fails the run" },
    { "send", "TEXT",
      "Write text to the child's stdin (escapes: \\n \\r \\t \\e \\xNN \\\\)" },
    { "sendln", "TEXT", "Like send, with a trailing CR LF" },
    { "raw", "HEX [HEX ...]", "Write raw bytes to the child's stdin" },
    { "emit", "TEXT", "Feed text straight into the VT parser (escapes expanded)" },
    { "emit-raw", "HEX [HEX ...]", "Feed raw bytes straight into the VT parser" },
    { "assert-contains", "TEXT", "Fail if the unified buffer lacks TEXT" },
    { "assert-not-contains", "TEXT", "Fail if the unified buffer contains TEXT" },
    { "await-exit", "[TIMEOUT]",
      "Wait until the child exits (default 10s); timeout fails; records status" },
    { "assert-exit", "STATUS", "Fail unless await-exit recorded STATUS" },
    { "resize", "COLS ROWS", "Resize the PTY and the grid" },
    { "render", "[visible|scrollback|all]",
      "Dump the grid as text (default: visible rows)" },
    { "render cells", "ROW [C0 C1]", "Dump per-cell detail for ROW" },
    { "render styles", "ROW", "Dump style detail for ROW" },
    { "cursor", NULL, "Dump cursor state (row, col, visible, blink)" },
    { "dump-sixel", NULL, "Dump live images and placements" },
    { "quit", NULL, "End the script run early (status DONE)" },
    { NULL, NULL, NULL }
};

const CfrScriptCmdDef *cfr_script_commands(size_t *out_count)
{
    size_t n = 0;
    while (COMMANDS[n].name)
        n++;
    if (out_count)
        *out_count = n;
    return COMMANDS;
}

/* ------------------------------------------------------------------ */
/* Small string helpers                                               */
/* ------------------------------------------------------------------ */

static char *skip_ws(char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    return s;
}

static void rtrim(char *s)
{
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1]))
        s[--len] = '\0';
}

/* Strip one layer of surrounding quotes (' or ") in place. */
static void strip_quotes(char *s)
{
    size_t len = strlen(s);
    if (len >= 2 && ((s[0] == '"' && s[len - 1] == '"') ||
                     (s[0] == '\'' && s[len - 1] == '\''))) {
        memmove(s, s + 1, len - 2);
        s[len - 2] = '\0';
    }
}

/* Expand backslash escapes (\n \r \t \e \\ \xNN) in place. A malformed
 * \xNN keeps its backslash. `\033` is deliberately not expanded. */
static void expand_escapes(char *s)
{
    char *src = s, *dst = s;
    while (*src) {
        if (src[0] == '\\' && src[1]) {
            switch (src[1]) {
            case 'n':
                *dst++ = '\n';
                src += 2;
                continue;
            case 'r':
                *dst++ = '\r';
                src += 2;
                continue;
            case 't':
                *dst++ = '\t';
                src += 2;
                continue;
            case 'e':
                *dst++ = '\x1b';
                src += 2;
                continue;
            case '\\':
                *dst++ = '\\';
                src += 2;
                continue;
            case 'x':
                if (isxdigit((unsigned char)src[2]) &&
                    isxdigit((unsigned char)src[3])) {
                    int hi = isdigit((unsigned char)src[2])
                                 ? src[2] - '0'
                                 : tolower((unsigned char)src[2]) - 'a' + 10;
                    int lo = isdigit((unsigned char)src[3])
                                 ? src[3] - '0'
                                 : tolower((unsigned char)src[3]) - 'a' + 10;
                    *dst++ = (char)((hi << 4) | lo);
                    src += 4;
                    continue;
                }
                break; /* malformed: keep the backslash */
            default:
                break;
            }
        }
        *dst++ = *src++;
    }
    *dst = '\0';
}

/* Parse whitespace-separated hex byte pairs ("ff fc 01") into a
 * malloc'd buffer. Returns false on malformed input. */
static bool parse_hex_bytes(const char *args, uint8_t **out, size_t *out_len)
{
    size_t cap = strlen(args) / 2 + 1;
    uint8_t *buf = malloc(cap ? cap : 1);
    if (!buf)
        return false;
    size_t n = 0;
    const char *p = args;
    while (*p) {
        while (*p && isspace((unsigned char)*p))
            p++;
        if (!*p)
            break;
        if (!isxdigit((unsigned char)p[0]) ||
            !isxdigit((unsigned char)p[1])) {
            free(buf);
            return false;
        }
        int hi = isdigit((unsigned char)p[0])
                     ? p[0] - '0'
                     : tolower((unsigned char)p[0]) - 'a' + 10;
        int lo = isdigit((unsigned char)p[1])
                     ? p[1] - '0'
                     : tolower((unsigned char)p[1]) - 'a' + 10;
        buf[n++] = (uint8_t)((hi << 4) | lo);
        p += 2;
    }
    *out = buf;
    *out_len = n;
    return true;
}

/* ------------------------------------------------------------------ */
/* Script construction                                                */
/* ------------------------------------------------------------------ */

static void set_error(char *err, size_t errlen, int line, const char *fmt, ...)
{
    if (!err || errlen == 0)
        return;
    va_list ap;
    va_start(ap, fmt);
    char msg[256];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (line > 0)
        snprintf(err, errlen, "line %d: %s", line, msg);
    else
        snprintf(err, errlen, "%s", msg);
}

static CfrScriptCmd *script_new_cmd(CfrScript *s)
{
    if (s->count >= s->cap) {
        size_t new_cap = s->cap == 0 ? 16 : s->cap * 2;
        CfrScriptCmd *nc = realloc(s->cmds, new_cap * sizeof(*nc));
        if (!nc)
            return NULL;
        s->cmds = nc;
        s->cap = new_cap;
    }
    CfrScriptCmd *cmd = &s->cmds[s->count++];
    memset(cmd, 0, sizeof(*cmd));
    cmd->c = -1; /* render cells: default col_end = whole row */
    return cmd;
}

static bool ext_name_known(const char *const *ext_names, const char *name)
{
    if (!ext_names)
        return false;
    for (size_t i = 0; ext_names[i]; i++) {
        if (strcmp(ext_names[i], name) == 0)
            return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Per-command parsing                                                */
/* ------------------------------------------------------------------ */

/* wait-for grammar: trailing option tokens trimmed right to left — a
 * bare number is the timeout, `line-end` anchors the match to a row's
 * end. The remainder is the needle, quotes optional, escapes expanded. */
static bool parse_wait_for(CfrScriptCmd *cmd, char *args,
                           char *err, size_t errlen)
{
    bool got_num = false;
    for (;;) {
        char *last_ws = NULL;
        for (char *p = args; *p; p++) {
            if (*p == ' ' || *p == '\t')
                last_ws = p;
        }
        if (!last_ws || *(last_ws + 1) == '\0')
            break; /* no trailing token */
        const char *tok = last_ws + 1;
        if (!got_num && isdigit((unsigned char)*tok)) {
            cmd->seconds = atof(tok);
            got_num = true;
            *last_ws = '\0';
            continue;
        }
        if (strcmp(tok, "line-end") == 0) {
            cmd->line_end = true;
            *last_ws = '\0';
            continue;
        }
        break; /* not an option token — part of the needle */
    }
    rtrim(args);
    strip_quotes(args);
    expand_escapes(args);
    if (*args == '\0') {
        set_error(err, errlen, cmd->line,
                  "wait-for requires text "
                  "(usage: wait-for \"TEXT\" [TIMEOUT] [line-end])");
        return false;
    }
    cmd->text = dup_str(args);
    if (cmd->seconds <= 0)
        cmd->seconds = 600;
    return cmd->text != NULL;
}

/* render [visible|scrollback|all] | render cells ROW [C0 C1] |
 * render styles ROW */
static bool parse_render(CfrScriptCmd *cmd, char *args,
                         char *err, size_t errlen)
{
    cmd->render_mode = CFR_RENDER_VISIBLE;
    if (*args == '\0')
        return true;

    char *tok = args;
    while (*args && !isspace((unsigned char)*args))
        args++;
    if (*args)
        *args++ = '\0';
    args = skip_ws(args);

    if (strcmp(tok, "visible") == 0)
        cmd->render_mode = CFR_RENDER_VISIBLE;
    else if (strcmp(tok, "scrollback") == 0)
        cmd->render_mode = CFR_RENDER_SCROLLBACK;
    else if (strcmp(tok, "all") == 0)
        cmd->render_mode = CFR_RENDER_ALL;
    else if (strcmp(tok, "cells") == 0)
        cmd->render_mode = CFR_RENDER_CELLS;
    else if (strcmp(tok, "styles") == 0)
        cmd->render_mode = CFR_RENDER_STYLES;
    else {
        set_error(err, errlen, cmd->line,
                  "render: unknown mode '%s' "
                  "(visible|scrollback|all|cells|styles)",
                  tok);
        return false;
    }

    if (cmd->render_mode == CFR_RENDER_CELLS) {
        cmd->a = 0;
        cmd->b = 0;
        cmd->c = -1;
        int n = sscanf(args, "%d %d %d", &cmd->a, &cmd->b, &cmd->c);
        if (n < 1) {
            set_error(err, errlen, cmd->line,
                      "render cells requires ROW [C0 C1]");
            return false;
        }
        if (n == 2) {
            set_error(err, errlen, cmd->line,
                      "render cells takes ROW alone or ROW C0 C1");
            return false;
        }
    } else if (cmd->render_mode == CFR_RENDER_STYLES) {
        if (sscanf(args, "%d", &cmd->a) != 1) {
            set_error(err, errlen, cmd->line,
                      "render styles requires ROW");
            return false;
        }
    }
    return true;
}

static bool parse_command(CfrScript *s, char *line, int line_num,
                          const char *const *ext_names,
                          char *err, size_t errlen)
{
    char *cmd_name = skip_ws(line);
    if (*cmd_name == '\0' || *cmd_name == '#')
        return true; /* blank or comment */

    /* Split keyword / args at the first whitespace run */
    char *args = cmd_name;
    while (*args && !isspace((unsigned char)*args))
        args++;
    if (*args) {
        *args++ = '\0';
        args = skip_ws(args);
    }
    rtrim(args);

    CfrScriptCmd *cmd = script_new_cmd(s);
    if (!cmd) {
        set_error(err, errlen, line_num, "out of memory");
        return false;
    }
    cmd->line = line_num;

    if (strcmp(cmd_name, "wait") == 0) {
        cmd->type = CFR_CMD_WAIT;
        cmd->seconds = atof(args);
        if (cmd->seconds <= 0)
            cmd->seconds = 1.0;
        return true;
    }
    if (strcmp(cmd_name, "wait-for") == 0) {
        cmd->type = CFR_CMD_WAIT_FOR;
        return parse_wait_for(cmd, args, err, errlen);
    }
    if (strcmp(cmd_name, "send") == 0 || strcmp(cmd_name, "sendln") == 0 ||
        strcmp(cmd_name, "emit") == 0) {
        cmd->type = strcmp(cmd_name, "send") == 0     ? CFR_CMD_SEND
                    : strcmp(cmd_name, "sendln") == 0 ? CFR_CMD_SENDLN
                                                      : CFR_CMD_EMIT;
        strip_quotes(args);
        expand_escapes(args);
        cmd->text = dup_str(args);
        return cmd->text != NULL;
    }
    if (strcmp(cmd_name, "raw") == 0 || strcmp(cmd_name, "emit-raw") == 0) {
        cmd->type = strcmp(cmd_name, "raw") == 0 ? CFR_CMD_RAW
                                                 : CFR_CMD_EMIT_RAW;
        if (!parse_hex_bytes(args, &cmd->bytes, &cmd->bytes_len)) {
            set_error(err, errlen, line_num,
                      "%s: invalid hex bytes: %s", cmd_name, args);
            return false;
        }
        return true;
    }
    if (strcmp(cmd_name, "assert-contains") == 0 ||
        strcmp(cmd_name, "assert-not-contains") == 0) {
        cmd->type = strcmp(cmd_name, "assert-contains") == 0
                        ? CFR_CMD_ASSERT_CONTAINS
                        : CFR_CMD_ASSERT_NOT_CONTAINS;
        strip_quotes(args);
        cmd->text = dup_str(args);
        return cmd->text != NULL;
    }
    if (strcmp(cmd_name, "await-exit") == 0) {
        cmd->type = CFR_CMD_AWAIT_EXIT;
        cmd->seconds = atof(args);
        if (cmd->seconds <= 0)
            cmd->seconds = 10;
        return true;
    }
    if (strcmp(cmd_name, "assert-exit") == 0) {
        cmd->type = CFR_CMD_ASSERT_EXIT;
        cmd->a = atoi(args);
        return true;
    }
    if (strcmp(cmd_name, "resize") == 0) {
        cmd->type = CFR_CMD_RESIZE;
        if (sscanf(args, "%d %d", &cmd->a, &cmd->b) != 2 ||
            cmd->a <= 0 || cmd->b <= 0) {
            set_error(err, errlen, line_num,
                      "resize requires COLS ROWS (positive)");
            return false;
        }
        return true;
    }
    if (strcmp(cmd_name, "render") == 0) {
        cmd->type = CFR_CMD_RENDER;
        return parse_render(cmd, args, err, errlen);
    }
    if (strcmp(cmd_name, "cursor") == 0) {
        cmd->type = CFR_CMD_CURSOR;
        return true;
    }
    if (strcmp(cmd_name, "dump-sixel") == 0) {
        cmd->type = CFR_CMD_DUMP_SIXEL;
        return true;
    }
    if (strcmp(cmd_name, "quit") == 0) {
        cmd->type = CFR_CMD_QUIT;
        return true;
    }
    if (ext_name_known(ext_names, cmd_name)) {
        cmd->type = CFR_CMD_EXT;
        cmd->ext_name = dup_str(cmd_name);
        cmd->text = dup_str(args); /* raw args, escapes NOT expanded */
        return cmd->ext_name != NULL && cmd->text != NULL;
    }

    set_error(err, errlen, line_num, "unknown command: %s", cmd_name);
    return false;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

CfrScript *script_parse(FILE *f, const char *const *ext_names,
                        char *err, size_t errlen)
{
    CfrScript *s = calloc(1, sizeof(*s));
    if (!s) {
        set_error(err, errlen, 0, "out of memory");
        return NULL;
    }

    char line[4096];
    int line_num = 0;
    bool ok = true;
    while (fgets(line, sizeof(line), f)) {
        line_num++;
        size_t len = strlen(line);
        if (len == sizeof(line) - 1 && line[len - 1] != '\n' &&
            !feof(f)) {
            set_error(err, errlen, line_num, "line too long");
            ok = false;
            break;
        }
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (!parse_command(s, line, line_num, ext_names, err, errlen)) {
            ok = false;
            break;
        }
    }

    if (!ok) {
        cfr_script_free(s);
        return NULL;
    }
    return s;
}

CfrScript *cfr_script_load(const char *path, char *err, size_t errlen,
                           const char *const *ext_names)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        set_error(err, errlen, 0, "cannot open %s: %s", path,
                  strerror(errno));
        return NULL;
    }
    CfrScript *s = script_parse(f, ext_names, err, errlen);
    fclose(f);
    return s;
}

void cfr_script_free(CfrScript *script)
{
    if (!script)
        return;
    for (size_t i = 0; i < script->count; i++) {
        free(script->cmds[i].text);
        free(script->cmds[i].bytes);
        free(script->cmds[i].ext_name);
    }
    free(script->cmds);
    free(script);
}
