# contrib/bvt-debug/Makefile.am - bvt-debug: headless bloom-vt PTY inspector

bin_PROGRAMS = bvt-debug

bvt_debug_SOURCES = bvt_debug.c

LDADD = $(top_builddir)/src/libbloom-vt.a -lm

if HAVE_THORVG
LDADD += $(THORVG_LIBS)
AM_CPPFLAGS = -I$(top_srcdir)/include -I$(top_srcdir)/src $(THORVG_CFLAGS)
else
AM_CPPFLAGS = -I$(top_srcdir)/include -I$(top_srcdir)/src
endif

if HOST_WINDOWS
bvt_debug_LDADD = $(LDADD) -lkernel32
else
bvt_debug_LDADD = $(LDADD)
endif
