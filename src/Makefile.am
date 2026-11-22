# src/Makefile.am - coffer static library and the coffer CLI

lib_LIBRARIES = libcoffer.a
bin_PROGRAMS = coffer
man1_MANS = coffer.1

libcoffer_a_SOURCES = \
    term.c \
    parser.c \
    csi.c \
    width.c \
    print.c \
    reflow.c \
    keys.c \
    grapheme.c \
    scrollback.c \
    style.c \
    grid.c \
    damage.c \
    lines.c \
    modes.c \
    palette.c \
    osc.c \
    dcs.c \
    sixel.c \
    lottie.c \
    image_store.c \
    image_decode.c \
    base64.c \
    osc_1337.c \
    apc.c \
    graphics.c \
    esc.c \
    hyperlink.c \
    log.c \
    selection.c \
    introspect.c \
    script_parse.c \
    script_exec.c \
    coffer_internal.h \
    script_internal.h \
    image_store.h \
    unicode_tables.h

# stb_image.h is vendored verbatim. Treat it as a system header so the
# project's warning gate (-Wconversion) does not hold third-party code to
# our conversion rules.
libcoffer_a_CPPFLAGS = -I$(top_srcdir)/include -I$(top_srcdir)/src \
    -isystem $(top_srcdir)/third_party/stb

# The CLI: a headless PTY inspector that drives the library. It links the
# static library next door, so `make` in src/ builds both.
coffer_SOURCES = coffer.c
coffer_CPPFLAGS = -I$(top_srcdir)/include -I$(top_srcdir)/src
coffer_LDADD = libcoffer.a -lm

# automake does not ship man_MANS on its own.
EXTRA_DIST = $(man1_MANS)

if HAVE_THORVG
libcoffer_a_CPPFLAGS += $(THORVG_CFLAGS)
coffer_CPPFLAGS += $(THORVG_CFLAGS)
coffer_LDADD += $(THORVG_LIBS)
endif

# Windows PTY + bundled ConPTY loader, shared with test_cfr_pty.
if HOST_WINDOWS
noinst_LIBRARIES = libcofferpty.a
libcofferpty_a_SOURCES = pty_w32.c conpty.c pty_internal.h conpty.h
libcofferpty_a_CPPFLAGS = -I$(top_srcdir)/src
coffer_LDADD += libcofferpty.a -lkernel32 -lversion
endif

# POSIX PTY (forkpty), shared with test_cfr_pty.
if !HOST_WINDOWS
noinst_LIBRARIES = libcofferpty.a
libcofferpty_a_SOURCES = pty_posix.c pty_internal.h
libcofferpty_a_CPPFLAGS = -I$(top_srcdir)/src
coffer_LDADD += libcofferpty.a
endif
