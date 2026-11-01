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

libcoffer_a_CPPFLAGS = -I$(top_srcdir)/include -I$(top_srcdir)/src -I$(top_srcdir)/third_party/stb

# The CLI: a headless PTY inspector that drives the library. It links the
# static library next door, so `make` in src/ builds both.
coffer_SOURCES = coffer.c
coffer_CPPFLAGS = -I$(top_srcdir)/include
coffer_LDADD = libcoffer.a -lm

# automake does not ship man_MANS on its own.
EXTRA_DIST = $(man1_MANS)

if HAVE_THORVG
libcoffer_a_CPPFLAGS += $(THORVG_CFLAGS)
coffer_CPPFLAGS += $(THORVG_CFLAGS)
coffer_LDADD += $(THORVG_LIBS)
endif

# The PTY layer uses the Win32 ConPTY API directly.
if HOST_WINDOWS
coffer_LDADD += -lkernel32
endif
