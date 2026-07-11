# contrib/cfr-debug/Makefile.am - cfr-debug: headless coffer PTY inspector

bin_PROGRAMS = cfr-debug
man1_MANS = cfr-debug.1
EXTRA_DIST = $(man1_MANS)

cfr_debug_SOURCES = cfr_debug.c

LDADD = $(top_builddir)/src/libcoffer.a -lm

if HAVE_THORVG
LDADD += $(THORVG_LIBS)
AM_CPPFLAGS = -I$(top_srcdir)/include -I$(top_srcdir)/src $(THORVG_CFLAGS)
else
AM_CPPFLAGS = -I$(top_srcdir)/include -I$(top_srcdir)/src
endif

if HOST_WINDOWS
cfr_debug_LDADD = $(LDADD) -lkernel32
else
cfr_debug_LDADD = $(LDADD)
endif
