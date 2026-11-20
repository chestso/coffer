terminfodir = $(datadir)/terminfo

EXTRA_DIST = coffer.ti \
    conpty/conpty.lock \
    conpty/LICENSE \
    conpty/NOTICE.md \
    conpty/README.md

# The pinned ConPTY DLL and OpenConsole live in libexec, not bin, so they
# cannot collide with other MSYS2 packages.  The build-tree fallback and
# the exe-directory search in conpty.c cover portable layouts.
if HAVE_CONPTY_BUNDLE
conptydir = $(libexecdir)/coffer
conpty_DATA = conpty/$(CONPTY_ARCH)/conpty.dll \
              conpty/$(CONPTY_ARCH)/OpenConsole.exe

docconptydir = $(docdir)
docconpty_DATA = conpty/LICENSE conpty/NOTICE.md conpty/README.md
endif

if !HOST_WINDOWS
# tic creates the per-letter subdirectory (c/) but not the -o directory
# itself, so a prefix that has no share/terminfo yet needs it made first.
install-data-local:
	$(MKDIR_P) "$(DESTDIR)$(datadir)/terminfo"
	tic -x -o $(DESTDIR)$(datadir)/terminfo $(srcdir)/coffer.ti

uninstall-local:
	rm -f $(DESTDIR)$(datadir)/terminfo/c/coffer-256color
	rm -f $(DESTDIR)$(datadir)/terminfo/c/coffer-vty-256color
	rm -f $(DESTDIR)$(datadir)/terminfo/c/coffer
endif

if HOST_WINDOWS
install-data-local:
	$(MKDIR_P) "$(DESTDIR)$(datadir)/terminfo"
	tic -x -o $(DESTDIR)$(datadir)/terminfo $(srcdir)/coffer.ti \
	    || echo "  WARNING: Failed to install terminfo (is ncurses installed?)" >&2

uninstall-local:
	rm -f $(DESTDIR)$(datadir)/terminfo/c/coffer-256color
	rm -f $(DESTDIR)$(datadir)/terminfo/c/coffer-vty-256color
	rm -f $(DESTDIR)$(datadir)/terminfo/c/coffer
endif
