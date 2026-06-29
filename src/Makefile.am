# src/Makefile.am - coffer static library

lib_LIBRARIES = libcoffer.a

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
    modes.c \
    palette.c \
    osc.c \
    dcs.c \
    sixel.c \
    lottie.c \
    esc.c \
    hyperlink.c \
    coffer_internal.h

libcoffer_a_CPPFLAGS = -I$(top_srcdir)/include

if HAVE_THORVG
libcoffer_a_CPPFLAGS += $(THORVG_CFLAGS)
endif
