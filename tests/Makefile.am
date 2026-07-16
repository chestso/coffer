# tests/Makefile.am - coffer unit and integration tests

check_PROGRAMS = test_cfr_parser test_cfr_keys test_cfr_pty test_cfr_sixel test_cfr_lottie test_cfr_altscreen test_cfr_sgr test_cfr_log test_cfr_rep test_cfr_stubs
TESTS = $(check_PROGRAMS)

AM_TESTS_ENVIRONMENT = srcdir='$(srcdir)'; export srcdir;
LDADD = $(top_builddir)/src/libcoffer.a -lm

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

test_cfr_parser_SOURCES = test_cfr_parser.c test_common.c
test_cfr_sixel_SOURCES  = test_cfr_sixel.c  test_common.c
test_cfr_lottie_SOURCES = test_cfr_lottie.c test_common.c
test_cfr_keys_SOURCES   = test_cfr_keys.c   test_common.c
test_cfr_pty_SOURCES    = test_cfr_pty.c    test_common.c $(PTY_SOURCE) coffer_pty.h common.h
test_cfr_pty_LDADD      = $(LDADD) $(PTY_LDADD)
test_cfr_altscreen_SOURCES = test_cfr_altscreen.c test_common.c
test_cfr_sgr_SOURCES      = test_cfr_sgr.c test_common.c
test_cfr_log_SOURCES      = test_cfr_log.c test_common.c
test_cfr_rep_SOURCES      = test_cfr_rep.c test_common.c
test_cfr_stubs_SOURCES    = test_cfr_stubs.c test_common.c

EXTRA_DIST = test_helpers.h common.h coffer_pty.h pty_w32.c
