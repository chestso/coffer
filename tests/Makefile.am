# tests/Makefile.am - bloom-vt unit and integration tests

check_PROGRAMS = test_bvt_parser test_bvt_keys test_bvt_pty test_bvt_sixel
TESTS = $(check_PROGRAMS)

AM_CPPFLAGS = -I$(top_srcdir)/include -I$(top_srcdir)/src -I$(srcdir)
AM_TESTS_ENVIRONMENT = srcdir='$(srcdir)'; export srcdir;
LDADD = $(top_builddir)/src/libbloom-vt.a

test_bvt_parser_SOURCES = test_bvt_parser.c test_common.c
test_bvt_sixel_SOURCES  = test_bvt_sixel.c  test_common.c
test_bvt_keys_SOURCES   = test_bvt_keys.c   test_common.c
test_bvt_pty_SOURCES    = test_bvt_pty.c    test_common.c pty.c bloom_pty.h common.h

EXTRA_DIST = test_helpers.h common.h bloom_pty.h
