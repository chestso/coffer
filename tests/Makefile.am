# tests/Makefile.am - bloom-vt unit and integration tests

check_PROGRAMS = test_bvt_parser test_bvt_keys test_bvt_pty test_bvt_sixel test_bvt_lottie test_bvt_altscreen test_bvt_sgr
TESTS = $(check_PROGRAMS)

AM_TESTS_ENVIRONMENT = srcdir='$(srcdir)'; export srcdir;
LDADD = $(top_builddir)/src/libbloom-vt.a -lm

if HAVE_THORVG
LDADD += $(THORVG_LIBS)
AM_CPPFLAGS = -I$(top_srcdir)/include -I$(top_srcdir)/src -I$(srcdir) $(THORVG_CFLAGS)
else
AM_CPPFLAGS = -I$(top_srcdir)/include -I$(top_srcdir)/src -I$(srcdir)
endif

# Platform-specific PTY implementation
if HOST_WINDOWS
PTY_SOURCE = pty_w32.c
PTY_LDADD = -lkernel32
else
PTY_SOURCE = pty.c
PTY_LDADD =
endif

test_bvt_parser_SOURCES = test_bvt_parser.c test_common.c
test_bvt_sixel_SOURCES  = test_bvt_sixel.c  test_common.c
test_bvt_lottie_SOURCES = test_bvt_lottie.c test_common.c
test_bvt_keys_SOURCES   = test_bvt_keys.c   test_common.c
test_bvt_pty_SOURCES    = test_bvt_pty.c    test_common.c $(PTY_SOURCE) bloom_pty.h common.h
test_bvt_pty_LDADD      = $(LDADD) $(PTY_LDADD)
test_bvt_altscreen_SOURCES = test_bvt_altscreen.c test_common.c
test_bvt_sgr_SOURCES      = test_bvt_sgr.c test_common.c

EXTRA_DIST = test_helpers.h common.h bloom_pty.h pty_w32.c
