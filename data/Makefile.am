terminfodir = $(datadir)/terminfo

EXTRA_DIST = coffer.ti

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
