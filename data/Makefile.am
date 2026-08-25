terminfodir = $(datadir)/terminfo

EXTRA_DIST = coffer.ti

if !HOST_WINDOWS
install-data-local:
	tic -x -o $(DESTDIR)$(datadir)/terminfo $(srcdir)/coffer.ti

uninstall-local:
	rm -f $(DESTDIR)$(datadir)/terminfo/c/coffer-256color
	rm -f $(DESTDIR)$(datadir)/terminfo/c/coffer-vty-256color
	rm -f $(DESTDIR)$(datadir)/terminfo/c/coffer
endif

if HOST_WINDOWS
install-data-local:
	tic -x -o $(DESTDIR)$(datadir)/terminfo $(srcdir)/coffer.ti \
	    || echo "  WARNING: Failed to install terminfo (is ncurses installed?)" >&2

uninstall-local:
	rm -f $(DESTDIR)$(datadir)/terminfo/c/coffer-256color
	rm -f $(DESTDIR)$(datadir)/terminfo/c/coffer-vty-256color
	rm -f $(DESTDIR)$(datadir)/terminfo/c/coffer
endif
