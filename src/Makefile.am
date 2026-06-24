# src/Makefile.am - bloom-vt static library

lib_LIBRARIES = libbloom-vt.a

libbloom_vt_a_SOURCES = \
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
    bloom_vt_internal.h

libbloom_vt_a_CPPFLAGS = -I$(top_srcdir)/include

if HAVE_THORVG
libbloom_vt_a_CPPFLAGS += $(THORVG_CFLAGS)
endif
