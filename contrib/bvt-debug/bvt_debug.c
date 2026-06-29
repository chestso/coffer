/*
 * bvt-debug — headless bloom-vt PTY inspector.
 *
 * Spawns a child process on a PTY, pipes its output through bloom-vt,
 * and renders the resulting grid as plain text. Optional features:
 *   - Send scripted input (with timing)
 *   - Save raw PTY output for offline replay
 *   - Dump individual rows or raw cell data
 *
 * The bloom-vt output callback is wired back to the PTY so that
 * terminal queries (DA, DSR, kitty keyboard) receive proper replies.
 * This allows interactive TUI applications (crush, helix, etc.) to
 * render correctly without a real terminal emulator.
 *
 * POSIX only (uses forkpty / poll).
 */

#define _GNU_SOURCE
#include <bloom-vt/bloom_vt.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

/* ---- PTY ---- */

typedef struct
{
    int master_fd;
    pid_t child_pid;
    int rows, cols;
} Pty;

static Pty *pty_spawn(int rows, int cols, char *const argv[])
{
    Pty *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->rows = rows;
    p->cols = cols;
    struct winsize ws = {
        .ws_row = (unsigned short)rows,
        .ws_col = (unsigned short)cols,
    };
    pid_t pid = forkpty(&p->master_fd, NULL, NULL, &ws);
    if (pid < 0) {
        perror("forkpty");
        free(p);
        return NULL;
    }
    if (pid == 0) {
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        setenv("TERM_PROGRAM", "ghostty", 1);
        if (argv && argv[0])
            execvp(argv[0], argv);
        else
            execlp(getenv("SHELL") ? getenv("SHELL") : "/bin/sh",
                   "-sh", (char *)NULL);
        _exit(127);
    }
    p->child_pid = pid;
    return p;
}

static void pty_kill(Pty *p)
{
    if (!p)
        return;
    if (p->master_fd >= 0)
        close(p->master_fd);
    if (p->child_pid > 0) {
        kill(p->child_pid, SIGHUP);
        usleep(100000);
        kill(p->child_pid, SIGKILL);
        waitpid(p->child_pid, NULL, 0);
    }
    free(p);
}

/* ---- Callbacks ---- */

static Pty *g_pty;
static FILE *g_raw_out;

static void cb_output(const uint8_t *bytes, size_t len, void *user)
{
    (void)user;
    if (g_pty)
        (void)write(g_pty->master_fd, bytes, len);
}

/* ---- Time ---- */

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

/* ---- Drain PTY into bloom-vt ---- */

static void drain(BvtTerm *vt, Pty *pty, int timeout_ms)
{
    char buf[65536];
    int fd = pty->master_fd;
    long long deadline = now_ms() + timeout_ms;
    while (now_ms() < deadline) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int wait = (int)(deadline - now_ms());
        if (wait <= 0)
            break;
        int r = poll(&pfd, 1, wait);
        if (r <= 0)
            continue;
        if (pfd.revents & POLLIN) {
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n <= 0)
                break;
            if (g_raw_out) {
                fwrite(buf, 1, (size_t)n, g_raw_out);
                fflush(g_raw_out);
            }
            bvt_input_write(vt, (const uint8_t *)buf, (size_t)n);
        }
        if (pfd.revents & (POLLHUP | POLLERR))
            break;
    }
}

/* ---- Grid rendering ---- */

static void utf8_put(uint32_t cp)
{
    if (cp < 0x80)
        putchar((int)cp);
    else if (cp < 0x800) {
        putchar(0xC0 | (cp >> 6));
        putchar(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        putchar(0xE0 | (cp >> 12));
        putchar(0x80 | ((cp >> 6) & 0x3F));
        putchar(0x80 | (cp & 0x3F));
    } else {
        putchar(0xF0 | (cp >> 18));
        putchar(0x80 | ((cp >> 12) & 0x3F));
        putchar(0x80 | ((cp >> 6) & 0x3F));
        putchar(0x80 | (cp & 0x3F));
    }
}

static void render_row_text(BvtTerm *vt, int row, int cols)
{
    for (int c = 0; c < cols; c++) {
        const BvtCell *cell = bvt_get_cell(vt, row, c);
        if (!cell || cell->cp == 0 || cell->cp == 0x20) {
            putchar(' ');
            continue;
        }
        utf8_put(cell->cp);
        if (cell->width == 2)
            c++;
    }
    putchar('\n');
}

static void render_row_cells(BvtTerm *vt, int row, int cols)
{
    printf("Row %2d cells:", row);
    for (int c = 0; c < cols; c++) {
        const BvtCell *cell = bvt_get_cell(vt, row, c);
        if (!cell) {
            printf(" [?]");
            continue;
        }
        if (cell->cp == 0 || cell->cp == 0x20)
            printf(" \xc2\xb7");
        else if (cell->cp < 0x80)
            printf(" %c", (char)cell->cp);
        else
            printf(" U+%04X", cell->cp);
        if (cell->width == 2) {
            c++;
            printf("(w2)");
        }
    }
    putchar('\n');
}

/* ---- Input expansion ---- */

static void expand_input(char *s)
{
    char *src = s, *dst = s;
    while (*src) {
        if (src[0] == '\\' && src[1] == 'n') {
            *dst++ = '\r';
            src += 2;
        } else if (src[0] == '\\' && src[1] == 'r') {
            *dst++ = '\r';
            src += 2;
        } else if (src[0] == '\\' && src[1] == 'e') {
            *dst++ = '\x1b';
            src += 2;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/* ---- Main ---- */

static void usage(void)
{
    printf(
        "bvt-debug -- spawn a child on a PTY and render through bloom-vt\n"
        "\n"
        "Usage: bvt-debug [options] [-- command args...]\n"
        "\n"
        "  -r ROWS   terminal rows (default 24)\n"
        "  -c COLS   terminal cols (default 80)\n"
        "  -w SEC    wait before input (default 3)\n"
        "  -i TEXT   first input (\\n=CR, \\r=CR, \\e=ESC)\n"
        "  -W SEC    wait after first input (default 15)\n"
        "  -I TEXT   second input\n"
        "  -W2 SEC   wait after second input (default 15)\n"
        "  -o FILE   save raw PTY output\n"
        "  -s ROW    dump specific row (repeatable)\n"
        "  -d        dump raw cell data\n"
        "  -q        quiet: only specified rows\n"
        "  -h        this help\n"
        "\n"
        "Examples:\n"
        "  bvt-debug -- bash -l\n"
        "  bvt-debug -c 68 -r 20 -- crush\n"
        "  bvt-debug -c 68 -r 20 -i \"hello world\\n\" -W 20 -o /tmp/out.raw -- crush\n"
        "  bvt-debug -s 18 -d -q -c 68 -r 20 -- crush\n");
}

int main(int argc, char *argv[])
{
    int rows = 24, cols = 80;
    int wait1 = 3, wait2 = 15, wait3 = 15;
    char *input1 = NULL, *input2 = NULL;
    const char *outfile = NULL;
    int show_rows[64];
    int n_show = 0;
    int show_cells = 0, quiet = 0;

    int cmd_start = argc;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            cmd_start = i + 1;
            break;
        }
        if (strcmp(argv[i], "-r") == 0 && i + 1 < argc)
            rows = atoi(argv[++i]);
        else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
            cols = atoi(argv[++i]);
        else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc)
            wait1 = atoi(argv[++i]);
        else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc)
            input1 = argv[++i];
        else if (strcmp(argv[i], "-W") == 0 && i + 1 < argc)
            wait2 = atoi(argv[++i]);
        else if (strcmp(argv[i], "-I") == 0 && i + 1 < argc)
            input2 = argv[++i];
        else if (strcmp(argv[i], "-W2") == 0 && i + 1 < argc)
            wait3 = atoi(argv[++i]);
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            outfile = argv[++i];
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc && n_show < 64)
            show_rows[n_show++] = atoi(argv[++i]);
        else if (strcmp(argv[i], "-d") == 0)
            show_cells = 1;
        else if (strcmp(argv[i], "-q") == 0)
            quiet = 1;
        else if (strcmp(argv[i], "-h") == 0) {
            usage();
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    char *const *cmd_argv =
        (cmd_start < argc) ? &argv[cmd_start] : NULL;

    if (outfile) {
        g_raw_out = fopen(outfile, "wb");
        if (!g_raw_out)
            perror("fopen");
    }

    BvtConfig cfg = BVT_CONFIG_DEFAULTS;
    cfg.rows = rows;
    cfg.cols = cols;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 6;
    BvtTerm *vt = bvt_new(&cfg);
    if (!vt) {
        fprintf(stderr, "bvt_new failed\n");
        return 1;
    }

    Pty *pty = pty_spawn(rows, cols, cmd_argv);
    if (!pty) {
        bvt_free(vt);
        return 1;
    }
    g_pty = pty;

    BvtCallbacks cb = { .output = cb_output };
    bvt_set_callbacks(vt, &cb, NULL);

    fprintf(stderr, "Waiting %ds for startup...\n", wait1);
    drain(vt, pty, wait1 * 1000);

    if (input1) {
        char *send = strdup(input1);
        expand_input(send);
        fprintf(stderr, "Sending input: %s\n", input1);
        write(pty->master_fd, send, strlen(send));
        free(send);
        fprintf(stderr, "Waiting %ds for response...\n", wait2);
        drain(vt, pty, wait2 * 1000);
    }

    if (input2) {
        char *send = strdup(input2);
        expand_input(send);
        fprintf(stderr, "Sending second input: %s\n", input2);
        write(pty->master_fd, send, strlen(send));
        free(send);
        fprintf(stderr, "Waiting %ds for response...\n", wait3);
        drain(vt, pty, wait3 * 1000);
    }

    for (int r = 0; r < rows; r++) {
        if (quiet && n_show > 0) {
            int found = 0;
            for (int i = 0; i < n_show; i++)
                if (show_rows[i] == r)
                    found = 1;
            if (!found)
                continue;
        }
        printf("%2d|", r);
        render_row_text(vt, r, cols);
        if (show_cells)
            render_row_cells(vt, r, cols);
    }

    if (outfile) {
        fclose(g_raw_out);
        fprintf(stderr, "Raw output saved to %s\n", outfile);
    }

    g_pty = NULL;
    pty_kill(pty);
    bvt_free(vt);
    return 0;
}
