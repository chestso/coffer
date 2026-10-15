/*
 * coffer — script engine internals (parse/exec shared types).
 *
 * Private to src/script_parse.c and src/script_exec.c. Not installed.
 * The public API is include/coffer/script.h.
 */

#ifndef COFFER_SCRIPT_INTERNAL_H
#define COFFER_SCRIPT_INTERNAL_H

#include <coffer/script.h>

typedef enum
{
    CFR_CMD_WAIT,
    CFR_CMD_WAIT_FOR,
    CFR_CMD_SEND,
    CFR_CMD_SENDLN,
    CFR_CMD_RAW,
    CFR_CMD_EMIT,
    CFR_CMD_EMIT_RAW,
    CFR_CMD_ASSERT_CONTAINS,
    CFR_CMD_ASSERT_NOT_CONTAINS,
    CFR_CMD_AWAIT_EXIT,
    CFR_CMD_ASSERT_EXIT,
    CFR_CMD_RESIZE,
    CFR_CMD_RENDER,
    CFR_CMD_CURSOR,
    CFR_CMD_DUMP_SIXEL,
    CFR_CMD_QUIT,
    CFR_CMD_EXT,
} CfrScriptCmdType;

/* `render` sub-modes. */
enum
{
    CFR_RENDER_VISIBLE,
    CFR_RENDER_SCROLLBACK,
    CFR_RENDER_ALL,
    CFR_RENDER_CELLS,
    CFR_RENDER_STYLES,
};

typedef struct
{
    CfrScriptCmdType type;
    int line;       /* 1-based script line, for diagnostics */
    double seconds; /* WAIT duration; WAIT_FOR / AWAIT_EXIT timeout */
    bool line_end;  /* WAIT_FOR: anchor the match to a row's end */
    char *text;     /* SEND/SENDLN/EMIT/WAIT_FOR/ASSERT_* payload
                       (escapes expanded at parse time) */
    uint8_t *bytes; /* RAW / EMIT_RAW payload (binary, length-tagged) */
    size_t bytes_len;
    int a, b, c;     /* RESIZE cols/rows; ASSERT_EXIT status;
                        RENDER cells/styles row/col_start/col_end */
    int render_mode; /* CFR_RENDER_* */
    char *ext_name;  /* CFR_CMD_EXT: the command name as written */
} CfrScriptCmd;

struct CfrScript
{
    CfrScriptCmd *cmds;
    size_t count;
    size_t cap;
};

/* Parse an open stream into a script. Returns NULL on error with a
 * "line N: ..." message in `err` (when errlen > 0). `ext_names` is a
 * NULL-terminated list of host extension command names (may be NULL);
 * any other unknown command is a parse error. */
CfrScript *script_parse(FILE *f, const char *const *ext_names,
                        char *err, size_t errlen);

#endif /* COFFER_SCRIPT_INTERNAL_H */
