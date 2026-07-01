# contrib/cfr-debug/Makefile.am - cfr-debug: headless coffer PTY inspector

# cfr-debug requires POSIX (forkpty, poll) — cannot build on Windows.
if HOST_WINDOWS
else
bin_PROGRAMS = cfr-debug
endif

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
